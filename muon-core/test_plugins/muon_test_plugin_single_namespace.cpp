/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

extern "C" void single_namespace_name(muon_completion_func comp) {
  const auto* result = "single";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata single_namespace_functions[] = {
    {
        "singleName",
        reinterpret_cast<muon_native_function>(&single_namespace_name),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const
    single_namespace_function_pointers[] = {
        &single_namespace_functions[0],
        nullptr,
};

static const muon_plugin_namespace single_namespace_namespaces[] = {
    {
        "single",
        nullptr,
        single_namespace_function_pointers,
    },
};

static const muon_plugin_namespace* const single_namespace_namespace_pointers[] =
    {
        &single_namespace_namespaces[0],
        nullptr,
};

static const muon_plugin_metadata single_namespace_metadata = {
    single_namespace_namespace_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &single_namespace_metadata;
}
