/* C99 counterpart to http.ts. Request metadata is supplied by the caller as JSON. */
#include "muon_service_api.h"
#include <stdlib.h>
#include <string.h>

typedef struct http_call {
  muon_service_result_completion complete;
  void* data;
  int metadata;
} http_call;

static void received(void* data, const muon_service_io_result* reply) {
  http_call* call = data;
  muon_service_value value = {0};
  if (reply->code) {
    const muon_service_error error = {reply->code, reply->message};
    call->complete(call->data, NULL, &error);
  } else {
    if (call->metadata) {
      value.kind = MUON_SERVICE_VALUE_STRING;
      value.data.string_value = (muon_service_string){reply->metadata_json, strlen(reply->metadata_json)};
    } else {
      value.kind = MUON_SERVICE_VALUE_BYTES;
      value.data.bytes_value = (muon_service_bytes){reply->body, reply->body_size};
    }
    call->complete(call->data, &value, NULL);
  }
  free(call);
}
static void invoke(void* data, const muon_service_call_context* context, const char* method,
    const muon_service_value* args, size_t count, muon_service_result_completion done, void* completion_data) {
  (void)count;
  const muon_service_io* io = data;
  const muon_service_string options = args[0].data.string_value;
  http_call* call = malloc(sizeof(*call));
  char* json = malloc(options.size + 1);
  if (!call || !json || memchr(options.data, 0, options.size)) {
    const muon_service_error error = {"invalid-input", "Invalid HTTP options"};
    free(json); free(call); done(completion_data, NULL, &error); return;
  }
  memcpy(json, options.data, options.size); json[options.size] = 0;
  *call = (http_call){done, completion_data, strcmp(method, "describe") == 0};
  const muon_service_io_request request = {MUON_SERVICE_ABI_VERSION, sizeof(request), "http.request", json,
      args[1].data.bytes_value.data, args[1].data.bytes_value.size, context->signal};
  io->request(io->user_data, &request, received, call);
  free(json);
}
static void stop(void* data, const muon_service_stop_info* info, muon_service_stop_completion done, void* completion_data) {
  (void)data; (void)info; done(completion_data);
}
static void cancel(void* data, uint64_t id) { (void)data; (void)id; }
static void start(const muon_service_context* context, muon_service_start_completion done, void* data) {
  const muon_service_instance instance = {(void*)context->io, stop, invoke, cancel};
  done(data, &instance, NULL);
}

/** @return Service factory for requests through the platform HTTP client. */
MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory(void) {
  static const muon_service_factory factory = {MUON_SERVICE_ABI_VERSION, start,
      "{\"schemaVersion\":1,\"methods\":{\"bytes\":{\"parameters\":[\"string\",\"bytes\"],\"returns\":\"bytes\",\"errors\":[]},"
      "\"describe\":{\"parameters\":[\"string\",\"bytes\"],\"returns\":\"string\",\"errors\":[]}},\"events\":{}}"};
  return &factory;
}
