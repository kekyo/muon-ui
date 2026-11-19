/* C99 HTTP and WebSocket counterpart to server.ts. The host copies every submitted buffer. */
#include "muon_service_api.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum step { CREATE, ATTACH, LISTEN, NEXT, REPLY, SEND, ACK, CLOSE } step;
typedef struct listener {
  const muon_service_context* context;
  muon_service_start_completion started;
  muon_service_stop_completion stopped;
  void* completion_data;
  const muon_service_signal* cleanup;
  char id[24], binding[24], request[24], generation[24];
  step pending;
  int active;
} listener;

static void completed(void*, const muon_service_io_result*);

static void submit(listener* s, step pending, const char* action, const char* fields, const uint8_t* bytes, size_t size) {
  char json[768];
  snprintf(json, sizeof(json), "{\"id\":\"%s\",\"binding\":\"%s\",\"requestId\":\"%s\"%s}",s->id,s->binding,s->request,fields);
  const muon_service_io_request request = {MUON_SERVICE_ABI_VERSION,sizeof(request),action,json,bytes,size,
      pending == CLOSE ? s->cleanup : s->context->signal};
  s->pending = pending;
  s->active = 1;
  s->context->io->request(s->context->io->user_data,&request,completed,s);
}

static void stop(void* data, const muon_service_stop_info* info, muon_service_stop_completion done, void* completion_data) {
  listener* s = data;
  s->stopped = done; s->completion_data = completion_data; s->cleanup = info->signal;
  if (!s->active) submit(s,CLOSE,"server.close","",NULL,0);
}
static void next(listener* s) { submit(s,NEXT,"server.next","",NULL,0); }

static int read_id(char* output, size_t capacity, const muon_service_io_result* result, const char* name) {
  const char* text = result->string_field(result->fields,name);
  if (!text) return 0;
  const size_t size = strlen(text);
  if (!size || size >= capacity || strspn(text,"0123456789") != size) return 0;
  memcpy(output,text,size+1); return 1;
}
static void completed(void* data, const muon_service_io_result* result) {
  listener* s = data;
  s->active = 0;
  if (s->pending == CLOSE) {
    const muon_service_stop_completion done = s->stopped;
    void* user = s->completion_data; free(s); done(user); return;
  }
  if (s->stopped) { submit(s,CLOSE,"server.close","",NULL,0); return; }
  if (result->code) {
    if (s->started) { s->started(s->completion_data,NULL,result->message); free(s); }
    else if (s->pending != NEXT && !s->context->signal->aborted(s->context->signal->user_data)) next(s);
    return;
  }
  switch (s->pending) {
    case CREATE:
      if (!read_id(s->id,sizeof(s->id),result,"id")) break;
      submit(s,ATTACH,"server.attach",",\"websocket\":true",NULL,0); return;
    case ATTACH:
      if (!read_id(s->binding,sizeof(s->binding),result,"binding")) break;
      submit(s,LISTEN,"server.listen","",NULL,0); return;
    case LISTEN: {
      const muon_service_instance instance = {s,stop,NULL,NULL};
      const muon_service_start_completion done = s->started;
      s->started = NULL; done(s->completion_data,&instance,NULL);
      next(s); return;
    }
    case NEXT: {
      const char* type = result->string_field(result->fields,"type");
      if (!type) return;
      if (!read_id(s->request,sizeof(s->request),result,"requestId")) { next(s); return; }
      if (strcmp(type,"request") == 0) {
        char fields[160];
        snprintf(fields,sizeof(fields),",\"status\":200,\"headers\":[[\"x-service-generation\",\"%s\"]]",s->generation);
        submit(s,REPLY,"server.respond",fields,result->body_size ? result->body : (const uint8_t*)s->generation,
            result->body_size ? result->body_size : strlen(s->generation)); return;
      }
      if (strcmp(type,"ws_request") == 0) { submit(s,REPLY,"server.ws_accept",",\"protocol\":\"\"",NULL,0); return; }
      if (strcmp(type,"ws_open") == 0) { submit(s,ACK,"server.ws_ack","",NULL,0); return; }
      if (strcmp(type,"ws_message") == 0) {
        submit(s,SEND,"server.ws_send",result->boolean_field(result->fields,"binary") ? ",\"binary\":true" : ",\"binary\":false",
            result->body,result->body_size); return;
      }
      next(s); return;
    }
    case SEND: submit(s,ACK,"server.ws_ack","",NULL,0); return;
    case ACK: case REPLY: next(s); return;
    case CLOSE: return;
  }
  s->started(s->completion_data,NULL,"Invalid listener identity"); free(s);
}
static void start(const muon_service_context* context, muon_service_start_completion done, void* data) {
  long port = 0;
  const char* token = NULL;
  for (int32_t i=0;i<context->config_count;++i) {
    if (strcmp(context->config_entries[i].key,"bearerToken") == 0) token = context->config_entries[i].value;
    if (strcmp(context->config_entries[i].key,"port") == 0) port = strtol(context->config_entries[i].value,NULL,10);
  }
  if (port < 1025 || port > 65535) { done(data,NULL,"Expected a loopback port"); return; }
  if (token && (strlen(token) > 120 || strspn(token,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~+/=") != strlen(token))) {
    done(data,NULL,"Invalid configured Bearer token"); return;
  }
  listener* s = calloc(1,sizeof(*s));
  if (!s) { done(data,NULL,"Allocation failed"); return; }
  s->context = context; s->started = done; s->completion_data = data;
  snprintf(s->generation,sizeof(s->generation),"%" PRIu64,context->generation);
  char fields[384];
  snprintf(fields,sizeof(fields),",\"options\":{\"name\":\"echo\",\"port\":%ld,\"allowedOrigins\":[\"https://example.test\"]%s%s%s}",
      port,token ? ",\"bearerToken\":\"" : "",token ? token : "",token ? "\"" : "");
  submit(s,CREATE,"server.create",fields,NULL,0);
}

/** @return Native HTTP and WebSocket echo service factory. */
MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory(void) {
  static const muon_service_factory factory = {MUON_SERVICE_ABI_VERSION,start,"{\"schemaVersion\":1,\"methods\":{},\"events\":{}}"};
  return &factory;
}
