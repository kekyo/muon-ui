/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

#include <cardio.h>

#include <atomic>

static std::atomic_bool dispatcher_available_at_init = false;

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
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  dispatcher_available_at_init.store(
      cardio::unsafe_get_current_dispatcher() != nullptr);
  return &cardio_metadata;
}
