/* C99 counterpart to state.ts. Both services use the same schema and file grants. */
#include "muon_service_api.h"
#include <stdlib.h>
#include <string.h>

typedef struct state_service {
  const muon_service_context* context;
  muon_service_start_completion started;
  muon_service_stop_completion stopped;
  void* completion_data;
  uint8_t* bytes;
  size_t size;
  int loading_asset;
} state_service;

static int store(state_service* state, const uint8_t* bytes, size_t size) {
  uint8_t* copy = size ? malloc(size) : NULL;
  if (size && !copy) return 0;
  if (size) memcpy(copy, bytes, size);
  free(state->bytes);
  state->bytes = copy;
  state->size = size;
  return 1;
}

static void saved(void* data, const muon_service_io_result* result) {
  state_service* state = data;
  if (result->code) state->context->host->log(state->context->host->user_data, MUON_LOG_LEVEL_ERROR, result->message);
  muon_service_stop_completion complete = state->stopped;
  void* completion_data = state->completion_data;
  free(state->bytes);
  free(state);
  complete(completion_data);
}

static void stop(void* data, const muon_service_stop_info* info, muon_service_stop_completion complete, void* completion_data) {
  state_service* state = data;
  state->stopped = complete;
  state->completion_data = completion_data;
  const muon_service_io_request request = {MUON_SERVICE_ABI_VERSION, sizeof(request), "fs.write",
      "{\"area\":\"data\",\"path\":\"state.bin\"}", state->bytes, state->size, info->signal};
  state->context->io->request(state->context->io->user_data, &request, saved, state);
}

static void invoke(void* data, const muon_service_call_context* context, const char* method,
    const muon_service_value* arguments, size_t count, muon_service_result_completion complete, void* completion_data) {
  (void)context; (void)count;
  state_service* state = data;
  muon_service_value value = {0};
  if (strcmp(method, "get") == 0) {
    value.kind = MUON_SERVICE_VALUE_BYTES;
    value.data.bytes_value = (muon_service_bytes){state->bytes, state->size};
  } else if (!store(state, arguments[0].data.bytes_value.data, arguments[0].data.bytes_value.size)) {
    const muon_service_error error = {"resource-exhausted", "Cannot store service bytes"};
    complete(completion_data, NULL, &error);
    return;
  }
  complete(completion_data, &value, NULL);
}

static void cancel(void* state, uint64_t id) { (void)state; (void)id; }

static void loaded(void* data, const muon_service_io_result* result) {
  state_service* state = data;
  if (result->code && !state->loading_asset && strcmp(result->code, "not-found") == 0) {
    state->loading_asset = 1;
    const muon_service_io_request request = {MUON_SERVICE_ABI_VERSION, sizeof(request), "fs.read",
        "{\"area\":\"assets\",\"path\":\"initial\"}", NULL, 0, state->context->signal};
    state->context->io->request(state->context->io->user_data, &request, loaded, state);
    return;
  }
  if (result->code || !store(state, result->body, result->body_size)) {
    state->started(state->completion_data, NULL, result->code ? result->message : "Cannot store service bytes");
    free(state);
    return;
  }
  const muon_service_instance instance = {state, stop, invoke, cancel};
  state->started(state->completion_data, &instance, NULL);
}

static void start(const muon_service_context* context, muon_service_start_completion complete, void* data) {
  state_service* state = calloc(1, sizeof(*state));
  if (!state) { complete(data, NULL, "Allocation failed"); return; }
  state->context = context;
  state->started = complete;
  state->completion_data = data;
  const muon_service_io_request request = {MUON_SERVICE_ABI_VERSION, sizeof(request), "fs.read",
      "{\"area\":\"data\",\"path\":\"state.bin\"}", NULL, 0, context->signal};
  context->io->request(context->io->user_data, &request, loaded, state);
}

/** @return Shared ABI factory for independent persistent-state instances. */
MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory(void) {
  static const muon_service_factory factory = {MUON_SERVICE_ABI_VERSION, start,
      "{\"schemaVersion\":1,\"methods\":{\"get\":{\"parameters\":[],\"returns\":\"bytes\",\"errors\":[]},"
      "\"set\":{\"parameters\":[\"bytes\"],\"returns\":\"void\",\"errors\":[]}},\"events\":{}}"};
  return &factory;
}
