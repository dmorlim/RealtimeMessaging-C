#include "libortc.h"
#include "connection.h"
#include "balancer.h"
#include "channel.h"
#include "message.h"
#include "common.h"
#include "dlist.h"

#include "libwebsockets.h"

#include <pthread.h>
#include <string.h>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
#include <Winsock2.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif


#define MAX_ORTC_PAYLOAD 1800


static int ortc_callback(struct libwebsocket_context *lws_context,
			 struct libwebsocket *wsi,
			 enum libwebsocket_callback_reasons reason,
			 void *user, 
 			 void *in, 
			 size_t len)
{
  ortc_context *context = (ortc_context *) user;
  ortc_dnode *t;
  char *messageToSend;
  unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + MAX_ORTC_PAYLOAD + LWS_SEND_BUFFER_POST_PADDING];
  int tret;
  switch (reason) {
  case LWS_CALLBACK_CLIENT_WRITEABLE:{
    if(context->throttleCounter>=2000){
      return 0;
    }
    if(context->ortcCommands->count>0){
      while(context->ortcCommands->isBlocked);
      context->ortcCommands->isBlocked = 1;
      t = (ortc_dnode*)_ortc_dlist_take_first(context->ortcCommands);
      if(t!=NULL){
	messageToSend = t->id;
	sprintf((char *)&buf[LWS_SEND_BUFFER_PRE_PADDING], "%s", messageToSend);
	//printf(":#: %s\n", messageToSend);
	libwebsocket_write(context->wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING], strlen(messageToSend), LWS_WRITE_TEXT);
	_ortc_dlist_free_dnode(t);
	context->throttleCounter++;
      }
      context->ortcCommands->isBlocked = 0;
    } else if(context->isConnected){
      while(context->messagesToSend->isBlocked);
      context->messagesToSend->isBlocked = 1;
      t = (ortc_dnode*)_ortc_dlist_take_first(context->messagesToSend);
      if(t!=NULL){
	char* messageToSend = t->id;
	sprintf((char *)&buf[LWS_SEND_BUFFER_PRE_PADDING], "%s", messageToSend);
	libwebsocket_write(context->wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING], strlen(messageToSend), LWS_WRITE_TEXT);
	_ortc_dlist_free_dnode(t);
	context->throttleCounter++;
      } 
      context->messagesToSend->isBlocked = 0;
    }
    if(context->messagesToSend->count>0 || context->ortcCommands->count>0){	
      libwebsocket_callback_on_writable(context->lws_context, context->wsi);	
    }
    break;
  }
  case LWS_CALLBACK_CLIENT_RECEIVE:{
    char * message = (char *) in;
    context->heartbeat_counter = 0;
    _ortc_parse_message(context, message);
    break;
  }
  case LWS_CALLBACK_CLOSED:{
    if(!context->isReconnecting){
      _ortc_stop_loops(context);
      if(context->onDisconnected != NULL)
	context->onDisconnected(context);     

      if(!context->isDisconnecting && !context->reconnecting_loop_active){
	context->reconnecting_loop_active = 1;
	tret = pthread_create(&context->reconnectingThread, NULL, _ortc_reconnecting_loop, context);
	if(tret!=0){
	  _ortc_exception(context, "Error creating reconnecting thread!");	  
	}
      }
    }
    context->isConnecting = 0;
    context->isConnected = 0;
    context->isDisconnecting = 0;
    break;
  }
      default : {
          break;
      }

  }
  return 0;
}

static struct libwebsocket_protocols ortc_protocols[] = {
  {"ortc-protocol", ortc_callback, 0, 4096},
  { NULL, NULL, 0, 0}
};

void *_ortc_connection_loop(void *ptr){
  int c = 0;
  ortc_context *context;
  context = (ortc_context *) ptr;
  context->thread_counter++;
  while(context->connection_loop_active){
    if(context->lws_context)
      libwebsocket_service(context->lws_context, 10);
    if(c == 50){
      c = 0;
      if(context->lws_context && context->wsi)
	libwebsocket_callback_on_writable(context->lws_context, context->wsi);
    }
    c++;
  }
  context->thread_counter--;
  pthread_detach(pthread_self());
  return 0;
}

void *_ortc_throttle_loop(void *ptr){
  ortc_context *context;
  context = (ortc_context *) ptr;
  context->thread_counter++;
  context->throttleCounter = 0;
  while(context->connection_loop_active){
    Sleep(1000);
    context->throttleCounter = 0;
  }
  context->thread_counter--;
  pthread_detach(pthread_self());
  return 0;
}

void _ortc_stop_loops(ortc_context* context){
  if(context->connection_loop_active){
    context->connection_loop_active = 0;
  }
  if(context->heartbeat_loop_active){
    context->heartbeat_loop_active = 0;
  }
  if(context->reconnecting_loop_active){
    context->reconnecting_loop_active = 0;
  }
  if(context->init_loop_active){
    context->init_loop_active = 0;
  }
  if(context->clientHB_loop_active){
    context->clientHB_loop_active = 0;
  }
}

void *_ortc_init_loop(void *ptr){
  ortc_context *context;
  context = (ortc_context *) ptr;
  context->thread_counter++;
  _ortc_connect(context);
  while(context->init_loop_active && !context->isConnected){    
    Sleep(ORTC_RECONNECT_INTERVAL*1000);
    if(!context->isConnected)
      _ortc_do_connect(context);
  }
  context->init_loop_active = 0;
  context->thread_counter--;
  pthread_detach(pthread_self());
  return 0;
}

void *_ortc_reconnecting_loop(void *ptr){
  int sec_count = ORTC_RECONNECT_INTERVAL;
  ortc_context *context;
  context = (ortc_context *) ptr;
  context->thread_counter++;
  context->isReconnecting = 1;
  while(context->reconnecting_loop_active){
    if(sec_count >= ORTC_RECONNECT_INTERVAL){
      sec_count = 0;
      if(context->reconnecting_loop_active){
        if(!context->isConnected){
          int cres = 0;
          if(context->onReconnecting != NULL)
            context->onReconnecting(context);	
          cres = _ortc_do_connect(context);
        } else
          break;      
      }
    }
    Sleep(1000);
    sec_count++;
  }
  context->isReconnecting = 0;
  context->thread_counter--;
  pthread_detach(pthread_self());
  return 0;
}

void *_ortc_heartbeat_loop(void *ptr){
  int tret;
  ortc_context *context;
  context = (ortc_context *) ptr;
  context->thread_counter++;
  while(context->heartbeat_loop_active){
    Sleep(1000);
    context->heartbeat_counter++;
    if(context->heartbeat_counter > ORTC_HEARTBEAT_TIMEOUT && context->heartbeat_loop_active){
      _ortc_exception(context, "Server heartbeat failed!");
      context->connection_loop_active = 0;            
      context->clientHB_loop_active = 0;
      context->isConnected = 0;
      if(!context->isReconnecting){
	if(context->onDisconnected != NULL)
	  context->onDisconnected(context);
        if(!context->reconnecting_loop_active){
          context->reconnecting_loop_active = 1;
          tret = pthread_create(&context->reconnectingThread, NULL, _ortc_reconnecting_loop, context);
          if(tret!=0){
            _ortc_exception(context, "Error creating reconnecting thread!");
          }	
        }
      }
      context->heartbeat_loop_active = 0;
      context->thread_counter--;
      pthread_detach(pthread_self());
      return 0;
    }
  }
  context->heartbeat_loop_active = 0;
  context->thread_counter--;
  pthread_detach(pthread_self());
  return 0;
}

void *_ortc_clientHB_loop(void *ptr){
  int sec_count = 0;
  ortc_context *context;
  char* hb;
  context = (ortc_context *) ptr;
  context->thread_counter++;
  if(context->isConnected && context->heartbeatActive){
    hb = (char*)malloc(sizeof(char) * (strlen("\"b\"")+1));
    sprintf(hb, "\"b\"");
    _ortc_send_command(context, hb);
	}
  while(context->clientHB_loop_active){
	Sleep(1000);
	sec_count++;
	if(sec_count >= context->heartbeatTime){
		sec_count = 0;
		if(context->isConnected && context->heartbeatActive && context->clientHB_loop_active){
                  hb = (char*)malloc(sizeof(char) * (strlen("\"b\"")+1));
                  sprintf(hb, "\"b\"");
			_ortc_send_command(context, hb);
		}
	}
  }
  context->clientHB_loop_active = 0;
  context->thread_counter--;
  pthread_detach(pthread_self());
  return 0;  
}

int _ortc_connect(ortc_context* context){
  struct lws_context_creation_info info;

  if(context->isConnecting)
    return 0;
  if(context->isConnected)
    return 0;
  if(context->lws_context)
    libwebsocket_context_destroy(context->lws_context);
  context->lws_context = NULL;
  if(context->host)
    free(context->host);
  context->host = NULL;
  if(context->server)
    free(context->server);
  context->server = NULL;
  context->isConnecting = 1;  
  memset(&info, 0, sizeof info);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.gid = -1;
  info.uid = -1;
  info.protocols = ortc_protocols;
  info.ssl_cipher_list = "RC4-MD5:RC4-SHA:AES128-SHA:AES256-SHA:HIGH:!DSS:!aNULL";

  info.ka_time = 0;
  info.ka_interval = 0;
  info.ka_probes = 0;
  
  context->lws_context = libwebsocket_create_context(&info);
  if (context->lws_context == NULL) {
    _ortc_exception(context,  "Creating libwebsocket context failed!");
    context->isConnecting = 0;
    return -1;
  }
  _ortc_do_connect(context);
    return 0;
}

int _ortc_do_connect(ortc_context* context){
  char *balancerResponse = NULL, *path = NULL;
  int tret;
  
  context->connection_loop_active = 0;

  if(!context->url){//get the url from balancer    
    if(_ortc_getBalancer(context->cluster, context->appKey, context->verifyPeerCert, &balancerResponse)!=0){
      _ortc_exception(context, balancerResponse);
      free(balancerResponse);
      context->isConnecting = 0;
      return -1;
    }
    context->server = strdup(balancerResponse);
    if(_ortc_parseUrl(balancerResponse, &context->host, &context->port, &context->useSSL) < 0){
      _ortc_exception(context, "malloc() failed! parsing cluster url");
      free(balancerResponse);
      context->isConnecting = 0;
      return -1;
    }
    free(balancerResponse);
  } else {//use url provided by user
    context->server = strdup(context->url);
    if(_ortc_parseUrl(context->url, &context->host, &context->port, &context->useSSL) < 0){
      _ortc_exception(context, "malloc() failed! parsing user url");
      context->isConnecting = 0;
      return -1;
    }
  }
  path = _ortc_prepareConnectionPath();
 
  if(context->verifyPeerCert && context->useSSL > 0){
	context->useSSL = 1;
  }

  libwebsocket_cancel_service(context->lws_context);

  context->wsi = libwebsocket_client_connect_extended(context->lws_context, context->host, context->port, context->useSSL, path, "", "", "ortc-protocol", -1, context);
  free(path);
  if(context->wsi == NULL){
    _ortc_exception(context,  "Creating websocket failed!");
    context->isConnecting = 0;
    return -1;
  }
#if !defined(WIN32) && !defined(_WIN32) && !defined(__APPLE__)
  int wsSock = libwebsocket_get_socket_fd(context->wsi);
  int opt = 1;
  setsockopt(wsSock, IPPROTO_TCP, TCP_CORK, &opt, sizeof(opt));
#endif

  context->connection_loop_active = 1;
  tret = pthread_create(&context->connectionThread, NULL, _ortc_connection_loop, context);
  if(tret!=0){
    _ortc_exception(context,  "Error creating connection thread!");
    context->isConnecting = 0;
    return -1;
  }
  return 1;
}

int _ortc_start_threads(ortc_context *context){
  int tret;
  tret = pthread_create(&context->throttleThread, NULL, _ortc_throttle_loop, context);
  if(tret!=0){    
    _ortc_exception(context,  "Error creating throttle thread!");
    context->isConnecting = 0;
    return -1;
  }
  context->heartbeat_loop_active = 1;
  context->heartbeat_counter = 0;
  tret = pthread_create(&context->heartbeatThread, NULL, _ortc_heartbeat_loop, context);
  if(tret!=0){    
    _ortc_exception(context,  "Error creating heartbeat thread!");
    context->isConnecting = 0;
    return -1;
  }
  if(context->heartbeatActive){
    context->clientHB_loop_active = 1;
    tret = pthread_create(&context->clientHbThread, NULL, _ortc_clientHB_loop, context);
    if(tret!=0){    
      _ortc_exception(context,  "Error creating client heartbeat thread!");
      context->isConnecting = 0;
      return -1;
    }
  }
  return 0;
}

void _ortc_send_command(ortc_context *context, char *message){
  while(context->ortcCommands->isBlocked);
  context->ortcCommands->isBlocked = 1;
  _ortc_dlist_insert(context->ortcCommands, message, NULL, NULL, 0, NULL);
  context->ortcCommands->isBlocked = 0;
  free(message);
  libwebsocket_callback_on_writable(context->lws_context, context->wsi);
}

void _ortc_send_message(ortc_context *context, char *message){
  while(context->messagesToSend->isBlocked);
  context->messagesToSend->isBlocked = 1;
  _ortc_dlist_insert(context->messagesToSend, message, NULL, NULL, 0, NULL);
  context->messagesToSend->isBlocked = 0;
  free(message);
  libwebsocket_callback_on_writable(context->lws_context, context->wsi);
}

void _ortc_send(ortc_context* context, char* channel, char* message){
  int i;
  size_t len;
  char *hash = _ortc_get_channel_permission(context, channel);
  char messageId[9], sParts[15], sMessageCount[15];
    int messageCount = 0;
  char* messagePart, *m;
  size_t parts = strlen(message) / ORTC_MAX_MESSAGE_SIZE;

  _ortc_random_string(messageId, 9);
  if(strlen(message) % ORTC_MAX_MESSAGE_SIZE > 0)
    parts++;
  sprintf(sParts, "%d", (int)parts);

  for(i=0; i<parts; i++){
    size_t messageSize;
    char *messageR;
    messageSize = strlen(message) - i * ORTC_MAX_MESSAGE_SIZE;
    if(messageSize > ORTC_MAX_MESSAGE_SIZE)
      messageSize = ORTC_MAX_MESSAGE_SIZE;
    
    messageCount = i + 1;    
    
    sprintf(sMessageCount, "%d", messageCount);

    messagePart = (char*)malloc(messageSize+1);
    if(messagePart==NULL){
      _ortc_exception(context, "malloc() failed in ortc send!");
      return;
    }
    memcpy(messagePart, message + i * ORTC_MAX_MESSAGE_SIZE, messageSize);
    messagePart[messageSize] = '\0';

    messageR = _ortc_escape_sequences_before(messagePart);
  
    len = 15 + strlen(context->appKey) + strlen(context->authToken) + strlen(channel) + strlen(hash) + strlen(messageId) + strlen(sParts) + strlen(sMessageCount) + strlen(messageR); 
    m = (char*)malloc(len + 1);
    if(m == NULL){
      _ortc_exception(context, "malloc() failed in ortc send!");
      free(messagePart);
      free(messageR);
      return;
    }
    snprintf(m, len, "\"send;%s;%s;%s;%s;%s_%d-%d_%s\"", context->appKey, context->authToken, channel, hash, messageId, messageCount, (int)parts, messageR);
    free(messagePart);
    free(messageR);
    _ortc_send_message(context, m);
  }
}

void _ortc_disconnect(ortc_context *context){
  _ortc_stop_loops(context);
  context->isConnected = 0;
  context->isConnecting = 0;
  
  if(context->lws_context)
    libwebsocket_context_destroy(context->lws_context);
  context->lws_context = NULL;
  context->wsi = NULL;
  
  _ortc_dlist_clear(context->multiparts);
  _ortc_dlist_clear(context->channels);
  _ortc_dlist_clear(context->permissions);
  _ortc_dlist_clear(context->messagesToSend);
  _ortc_dlist_clear(context->ortcCommands);
}
