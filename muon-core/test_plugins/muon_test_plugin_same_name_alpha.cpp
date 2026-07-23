/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

extern "C" void shared_name_alpha(muon_completion_func comp) {
  const auto* result = "same-alpha";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata same_name_alpha_functions[] = {
    {
        "sharedName",
        reinterpret_cast<muon_native_function>(&shared_name_alpha),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const same_name_alpha_functions_pointers[] = {
    &same_name_alpha_functions[0],
    nullptr,
};

static const muon_plugin_namespace same_name_alpha_namespaces[] = {
    {
        "muon.test.sameNameAlpha",
        nullptr,
        same_name_alpha_functions_pointers,
    },
};

static const muon_plugin_namespace* const same_name_alpha_namespaces_pointers[] = {
    &same_name_alpha_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata same_name_alpha_metadata = {
    same_name_alpha_namespaces_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &same_name_alpha_metadata;
}
