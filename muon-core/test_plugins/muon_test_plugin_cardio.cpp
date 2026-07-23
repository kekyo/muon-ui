/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"
#include "muon_cardio_post.h"

#include <cardio.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

static std::atomic_bool dispatcher_available_at_init = false;

static void append_stop_marker(const char* text) {
  const auto* path = std::getenv("MUON_TEST_PLUGIN_STOP_MARKER");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  auto* file = std::fopen(path, "ab");
  if (file == nullptr) {
    return;
  }
  std::fputs(text, file);
  std::fclose(file);
}

struct CardioPluginUnloadMarker {
  ~CardioPluginUnloadMarker() { append_stop_marker("unloaded\n"); }
};

static CardioPluginUnloadMarker unload_marker;

static cardio::promise<void> complete_stop_after_delay(
    muon_plugin_stop_completion completion,
    void* user_data) {
  co_await cardio::promises::delay(25);
  append_stop_marker("stop-completed\n");
  completion(user_data);
}

static void stop_cardio_plugin(muon_plugin_stop_completion completion,
                               void* user_data) {
  append_stop_marker("stop-started\n");
  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr) {
    append_stop_marker("stop-completed\n");
    completion(user_data);
    return;
  }
  muon_internal::FireAndForgetOnDispatcher(
      dispatcher, [completion, user_data]() {
        cardio::fire_and_forget(
            complete_stop_after_delay(completion, user_data));
      });
}

extern "C" void dispatcher_available_at_init_call(muon_completion_func comp) {
  const auto result = dispatcher_available_at_init.load();
  comp(&result, nullptr);
}

extern "C" void dispatcher_available(muon_completion_func comp) {
  const auto result = cardio::unsafe_get_current_dispatcher() != nullptr;
  comp(&result, nullptr);
}

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_plugin_function_metadata cardio_functions[] = {
    {
        "dispatcherAvailableAtInit",
        reinterpret_cast<muon_native_function>(
            &dispatcher_available_at_init_call),
        {0, nullptr, &type_bool},
        nullptr,
    },
    {
        "dispatcherAvailable",
        reinterpret_cast<muon_native_function>(&dispatcher_available),
        {0, nullptr, &type_bool},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const cardio_functions_pointers[] = {
    &cardio_functions[0],
    &cardio_functions[1],
    nullptr,
};

static const muon_plugin_namespace cardio_namespaces[] = {
    {
        "muon.test.cardio",
        nullptr,
        cardio_functions_pointers,
    },
};

static const muon_plugin_namespace* const cardio_namespaces_pointers[] = {
    &cardio_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata cardio_metadata = {
    cardio_namespaces_pointers,
    &stop_cardio_plugin,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  dispatcher_available_at_init.store(
      cardio::unsafe_get_current_dispatcher() != nullptr);
  return &cardio_metadata;
}
