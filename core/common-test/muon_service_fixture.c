/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_service_api.h"

#include <stdlib.h>

typedef struct fixture_state {
  const muon_service_context* context;
  muon_service_stop_completion stopped;
  void* completion_data;
} fixture_state;

static void finish_stop(void* data) {
  fixture_state* state = (fixture_state*)data;
  muon_service_stop_completion complete = state->stopped;
  void* completion_data = state->completion_data;
  state->context->host->log(state->context->host->user_data,
                           MUON_LOG_LEVEL_INFO, "stopped");
  free(state);
  complete(completion_data);
}

static void stop(void* data, muon_service_stop_completion complete,
                  void* completion_data) {
  fixture_state* state = (fixture_state*)data;
  state->stopped = complete;
  state->completion_data = completion_data;
  state->context->host->post(state->context->host->user_data, finish_stop, state);
}

static void start(const muon_service_context* context,
                   muon_service_start_completion complete, void* data) {
  fixture_state* state = (fixture_state*)calloc(1, sizeof(fixture_state));
  if (state == NULL) {
    complete(data, NULL, "allocation failed");
    return;
  }
  state->context = context;
  context->host->log(context->host->user_data, MUON_LOG_LEVEL_INFO, context->service_id);
  const muon_service_instance instance = {state, stop};
  complete(data, &instance, NULL);
}

/** Returns the C99 fixture factory without initializing an instance. */
MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory(void) {
  static const muon_service_factory factory = {MUON_SERVICE_ABI_VERSION, start};
  return &factory;
}
