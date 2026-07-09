/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

extern "C" void multi_namespace_a_name(muon_completion_func comp) {
  const auto* result = "multi-a";
  comp(&result, nullptr);
}

extern "C" void multi_namespace_b_name(muon_completion_func comp) {
  const auto* result = "multi-b";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata multi_namespace_a_functions[] = {
    {
        "name",
        reinterpret_cast<muon_native_function>(&multi_namespace_a_name),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const multi_namespace_a_functions_pointers[] = {
    &multi_namespace_a_functions[0],
    nullptr,
};

static const muon_plugin_function_metadata multi_namespace_b_functions[] = {
    {
        "name",
        reinterpret_cast<muon_native_function>(&multi_namespace_b_name),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const multi_namespace_b_functions_pointers[] = {
    &multi_namespace_b_functions[0],
    nullptr,
};

static const muon_plugin_namespace multi_namespace_namespaces[] = {
    {
        "muon.test.multiA",
        nullptr,
        multi_namespace_a_functions_pointers,
    },
    {
        "muon.test.multiB",
        nullptr,
        multi_namespace_b_functions_pointers,
    },
};

static const muon_plugin_namespace* const multi_namespace_namespaces_pointers[] = {
    &multi_namespace_namespaces[0],
    &multi_namespace_namespaces[1],
    nullptr,
};

static const muon_plugin_metadata multi_namespace_metadata = {
    multi_namespace_namespaces_pointers,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &multi_namespace_metadata;
}
