/* Portable C99 service example. Build with muon-plugin-sdk, kind service. */
#include "muon_service_api.h"
#include <stdlib.h>

static void stop(void* state, muon_service_stop_completion complete, void* data) {
  free(state);
  complete(data);
}

static void invoke(void* state, const muon_service_call_context* context,
    const char* method, const muon_service_value* arguments, size_t count,
    muon_service_result_completion complete, void* data) {
  (void)state; (void)context; (void)method; (void)count;
  /* The host validates the schema and copies the result before returning. */
  complete(data, &arguments[0], NULL);
}

static void cancel(void* state, uint64_t call_id) {
  /* This example completes inline and has no pending work to cancel. */
  (void)state; (void)call_id;
}

static void start(const muon_service_context* context,
    muon_service_start_completion complete, void* data) {
  (void)context;
  void* state = malloc(1);
  if (state == NULL) { complete(data, NULL, "allocation failed"); return; }
  const muon_service_instance instance = {state, stop, invoke, cancel};
  complete(data, &instance, NULL);
}

/**
 * Returns the shared factory; each start creates an independent instance.
 * @return Borrowed factory metadata retained while the library is loaded.
 */
MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory(void) {
  static const muon_service_factory factory = {
    MUON_SERVICE_ABI_VERSION, start,
    "{\"schemaVersion\":1,\"methods\":{\"echo\":{\"parameters\":[\"string\"],"
    "\"returns\":\"string\",\"errors\":[]}},\"events\":{}}"
  };
  return &factory;
}
