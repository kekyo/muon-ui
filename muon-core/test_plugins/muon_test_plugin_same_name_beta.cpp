/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

extern "C" void shared_name_beta(muon_completion_func comp) {
  const auto* result = "same-beta";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata same_name_beta_functions[] = {
    {
        "sharedName",
        reinterpret_cast<muon_native_function>(&shared_name_beta),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const same_name_beta_functions_pointers[] = {
    &same_name_beta_functions[0],
    nullptr,
};

static const muon_plugin_namespace same_name_beta_namespaces[] = {
    {
        "muon.test.sameNameBeta",
        nullptr,
        same_name_beta_functions_pointers,
    },
};

static const muon_plugin_namespace* const same_name_beta_namespaces_pointers[] = {
    &same_name_beta_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata same_name_beta_metadata = {
    same_name_beta_namespaces_pointers,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &same_name_beta_metadata;
}
