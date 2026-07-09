/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

extern "C" void namespace_path_conflict_leaf(muon_completion_func comp) {
  const auto* result = "leaf";
  comp(&result, nullptr);
}

extern "C" void namespace_path_conflict_nested(muon_completion_func comp) {
  const auto* result = "nested";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata leaf_functions[] = {
    {
        "leaf",
        reinterpret_cast<muon_native_function>(&namespace_path_conflict_leaf),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const leaf_functions_pointers[] = {
    &leaf_functions[0],
    nullptr,
};

static const muon_plugin_function_metadata nested_functions[] = {
    {
        "nested",
        reinterpret_cast<muon_native_function>(&namespace_path_conflict_nested),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const nested_functions_pointers[] = {
    &nested_functions[0],
    nullptr,
};

static const muon_plugin_namespace namespace_path_conflict_namespaces[] = {
    {
        "muon.test.inner",
        nullptr,
        leaf_functions_pointers,
    },
    {
        "muon.test.inner.leaf",
        nullptr,
        nested_functions_pointers,
    },
};

static const muon_plugin_namespace* const namespace_path_conflict_namespaces_pointers[] = {
    &namespace_path_conflict_namespaces[0],
    &namespace_path_conflict_namespaces[1],
    nullptr,
};

static const muon_plugin_metadata namespace_path_conflict_metadata = {
    namespace_path_conflict_namespaces_pointers,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &namespace_path_conflict_metadata;
}
