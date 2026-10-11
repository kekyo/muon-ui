#include "muon_plugin_api.h"
#include <cardio.h>
#include <exception>

static cardio::promise<void> increment_async(muon_completion_func completion, int32_t value) {
  try {
    co_await cardio::promises::delay(1);
    const int32_t result = value + 1;
    completion(&result, nullptr);
  } catch (const std::exception& error) {
    completion(nullptr, error.what());
  }
}

static void increment(muon_completion_func completion, int32_t value) {
  // muon owns the current cardio dispatcher and drains pending calls before stop.
  cardio::fire_and_forget(increment_async(completion, value));
}

static cardio::promise<void> stop_async(muon_plugin_stop_completion completion, void* data) {
  // Release asynchronous plugin-owned resources before notifying the host.
  co_await cardio::promises::delay(1);
  completion(data);
}

static void stop(muon_plugin_stop_completion completion, void* data) {
  cardio::fire_and_forget(stop_async(completion, data));
}

static const muon_type_descriptor i32 = { MUON_TYPE_I32, nullptr };
static const muon_plugin_function_metadata increment_metadata = {
  "increment", (muon_native_function)increment, { 1, &i32, &i32 }, nullptr
};
static const muon_plugin_function_metadata* const functions[] = { &increment_metadata, nullptr };
static const muon_plugin_namespace plugin_namespace = { "muon.counter", nullptr, functions };
static const muon_plugin_namespace* const namespaces[] = { &plugin_namespace, nullptr };
static const muon_plugin_metadata metadata = { namespaces, stop, nullptr };

/**
 * Registers the plugin's static metadata.
 * @param context Host helpers, borrowed for the loaded plugin lifetime.
 * @return Metadata owned by this library until the asynchronous stop completes.
 */
extern "C" MUON_PLUGIN_EXPORT const muon_plugin_metadata* muon_init_plugin(const muon_plugin_init_context* context) {
  (void)context;
  return &metadata;
}
