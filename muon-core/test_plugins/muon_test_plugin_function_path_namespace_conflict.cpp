/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

extern "C" void nested_after_function(muon_completion_func comp) {
  const auto* result = "nested";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata conflict_functions[] = {
    {
        "nested",
        reinterpret_cast<muon_native_function>(&nested_after_function),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const conflict_functions_pointers[] = {
    &conflict_functions[0],
    nullptr,
};

static const muon_plugin_namespace conflict_namespaces[] = {
    {
        "muon.test.alpha.alphaName",
        nullptr,
        conflict_functions_pointers,
    },
};

static const muon_plugin_namespace* const conflict_namespaces_pointers[] = {
    &conflict_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata conflict_metadata = {
    conflict_namespaces_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &conflict_metadata;
}
