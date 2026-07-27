/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

extern "C" void invalid_namespace_name(muon_completion_func comp) {
  const auto* result = "invalid";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata invalid_namespace_functions[] = {
    {
        "invalidName",
        reinterpret_cast<muon_native_function>(&invalid_namespace_name),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const invalid_namespace_functions_pointers[] = {
    &invalid_namespace_functions[0],
    nullptr,
};

static const muon_plugin_namespace invalid_namespace_namespaces[] = {
    {
        "muon.test.invalid-name",
        nullptr,
        invalid_namespace_functions_pointers,
    },
};

static const muon_plugin_namespace* const invalid_namespace_namespaces_pointers[] = {
    &invalid_namespace_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata invalid_namespace_metadata = {
    invalid_namespace_namespaces_pointers,
    nullptr,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &invalid_namespace_metadata;
}
