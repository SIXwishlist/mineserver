/*
   Copyright (c) 2016, The Mineserver Project
   All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the The Mineserver Project nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "mineserver.h"
#include "user.h"
#include "tools.h"

#include "threadpool.h"
#include "json.h"


void ThreadPool::taskValidateUser(ThreadTask *task)
{
  // https://wiki.openssl.org/index.php/SSL/TLS_Client
  #define HOST_NAME "sessionserver.mojang.com"
  #define HOST_PORT "443"
  #define HOST_RESOURCE "/session/minecraft/hasJoined?"
  const char* const PREFERRED_CIPHERS = "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4";
  long res = 1;
  int len = 0;
  SSL_CTX* ctx = NULL;
  BIO *web = NULL;
  SSL *ssl = NULL;
  char buff[1536] = {};
  std::string::size_type findResult;
  const long flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
  Mineserver::userValidation userValid;
  userValid.valid = false;
  std::string output,request_uri, get_req;  
  X509* cert;

  const SSL_METHOD* method = SSLv23_method();
  if(!(NULL != method)) goto cleanup;

  ctx = SSL_CTX_new(method);
  if(!(ctx != NULL)) goto cleanup;

  // ToDo: use cert verification
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

  SSL_CTX_set_verify_depth(ctx, 4);

  SSL_CTX_set_options(ctx, flags);

  //res = SSL_CTX_load_verify_locations(ctx, "chain.pem", NULL);
  //if(!(1 == res)) goto failure;

  web = BIO_new_ssl_connect(ctx);
  if(!(web != NULL)) goto cleanup;

  res = BIO_set_conn_hostname(web, HOST_NAME ":" HOST_PORT);
  if(!(1 == res)) goto cleanup;

  BIO_get_ssl(web, &ssl);
  if(!(ssl != NULL)) goto cleanup;
    
  res = SSL_set_cipher_list(ssl, PREFERRED_CIPHERS);
  if(!(1 == res)) goto cleanup;

  res = SSL_set_tlsext_host_name(ssl, HOST_NAME);
  if(!(1 == res)) goto cleanup;

  res = BIO_do_connect(web);
  if(!(1 == res)) goto cleanup;

  res = BIO_do_handshake(web);
  if(!(1 == res)) goto cleanup;

  // verify a server certificate was presented during the negotiation
  cert = SSL_get_peer_certificate(ssl);
  if(cert) { X509_free(cert); } // Free immediately
  if(NULL == cert) goto cleanup;

  //ToDo: verify
  //res = SSL_get_verify_result(ssl);
  //if(!(X509_V_OK == res)) return;

  // Hostname verification ?  
  request_uri = HOST_RESOURCE "username=" + task->user->nick;
  request_uri += std::string("&serverId=") + task->user->generateDigest();

  get_req  = "GET " + request_uri + " HTTP/1.1\r\n"
                "Host: " HOST_NAME "\r\n"
                "Connection: close\r\n"
                "\r\n\r\n";

  BIO_puts(web, get_req.c_str());    
  
  do
  {    
    len = BIO_read(web, buff, sizeof(buff));            
    if(len > 0)
      output.append(buff, len);

  } while (len > 0 || BIO_should_retry(web));
  
  // Make sure we have the right HTTP code
  findResult = output.find("200 OK");
  if (findResult == std::string::npos) goto cleanup;
  findResult = output.find("\r\n\r\n");
  if (findResult == std::string::npos) goto cleanup;

  {
    // Parse the JSON
    std::string input_json = output.substr(findResult);
    int32_t jsonlen = input_json.length();  
    JSON_Val value((uint8_t*)input_json.c_str(), jsonlen, JSON_Val::JSON_COMPOUND);

    // Grab the values
    if (value["id"] != nullptr && value["id"]->getType() == JSON_Val::JSON_STRING &&
       value["name"] != nullptr && value["name"]->getType() == JSON_Val::JSON_STRING &&
       value["properties"] != nullptr && value["properties"]->getType() == JSON_Val::JSON_LIST)
    {
      task->user->setUUID(*value["id"]->getString(), false);    
      //std::cout << "name: " << *value["name"]->getString() << std::endl;
      for(JSON_Val *var : *value["properties"]->getList())
      {
        if ((*var)["name"] != nullptr && (*var)["name"]->getType() == JSON_Val::JSON_STRING &&
           (*var)["value"] != nullptr && (*var)["value"]->getType() == JSON_Val::JSON_STRING &&
           (*var)["signature"] != nullptr && (*var)["signature"]->getType() == JSON_Val::JSON_STRING)
        {
          task->user->properties.push_back(UserProperty(*(*var)["name"]->getString(),
                                                        *(*var)["value"]->getString(),
                                                        *(*var)["signature"]->getString()));
        }
      }
    } else {
      goto cleanup;
    }

  }

  //std::cout << output << std::endl;
 
  userValid.valid = true;
  userValid.user = task->user;
  userValid.UID = task->user->UID;

cleanup:

  std::unique_lock<std::mutex> l(ServerInstance->m_validation_mutex);
  ServerInstance->validatedUsers.push_back(userValid);
  l.unlock();

  if(web != NULL)
    BIO_free_all(web);

  if(NULL != ctx)
    SSL_CTX_free(ctx);
}