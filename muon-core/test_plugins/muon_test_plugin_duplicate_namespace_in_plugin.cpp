/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

extern "C" void duplicate_namespace_in_plugin_first(muon_completion_func comp) {
  const auto* result = "first";
  comp(&result, nullptr);
}

extern "C" void duplicate_namespace_in_plugin_second(
    muon_completion_func comp) {
  const auto* result = "second";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata first_functions[] = {
    {
        "first",
        reinterpret_cast<muon_native_function>(
            &duplicate_namespace_in_plugin_first),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const first_functions_pointers[] = {
    &first_functions[0],
    nullptr,
};

static const muon_plugin_function_metadata second_functions[] = {
    {
        "second",
        reinterpret_cast<muon_native_function>(
            &duplicate_namespace_in_plugin_second),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const second_functions_pointers[] = {
    &second_functions[0],
    nullptr,
};

static const muon_plugin_namespace duplicate_namespace_in_plugin_namespaces[] = {
    {
        "muon.test.duplicateInside",
        nullptr,
        first_functions_pointers,
    },
    {
        "muon.test.duplicateInside",
        nullptr,
        second_functions_pointers,
    },
};

static const muon_plugin_namespace* const duplicate_namespace_in_plugin_namespaces_pointers[] = {
    &duplicate_namespace_in_plugin_namespaces[0],
    &duplicate_namespace_in_plugin_namespaces[1],
    nullptr,
};

static const muon_plugin_metadata duplicate_namespace_in_plugin_metadata = {
    duplicate_namespace_in_plugin_namespaces_pointers,
    nullptr,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &duplicate_namespace_in_plugin_metadata;
}
