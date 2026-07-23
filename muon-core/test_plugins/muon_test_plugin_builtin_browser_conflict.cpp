/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

extern "C" void builtin_browser_conflict_function(muon_completion_func comp) {
  comp(nullptr, nullptr);
}

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_plugin_function_metadata builtin_browser_conflict_functions[] = {
    {
        "conflict",
        reinterpret_cast<muon_native_function>(
            &builtin_browser_conflict_function),
        {0, nullptr, &type_void},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const builtin_browser_conflict_functions_pointers[] = {
    &builtin_browser_conflict_functions[0],
    nullptr,
};

static const muon_plugin_namespace builtin_browser_conflict_namespaces[] = {
    {
        "muon.browser",
        nullptr,
        builtin_browser_conflict_functions_pointers,
    },
};

static const muon_plugin_namespace* const builtin_browser_conflict_namespaces_pointers[] = {
    &builtin_browser_conflict_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata builtin_browser_conflict_metadata = {
    builtin_browser_conflict_namespaces_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &builtin_browser_conflict_metadata;
}
