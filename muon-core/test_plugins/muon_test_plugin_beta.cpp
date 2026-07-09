/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

#include <stdio.h>
#include <stdint.h>

extern "C" void beta_name(muon_completion_func comp) {
  const auto* result = "beta";
  comp(&result, nullptr);
}

extern "C" void beta_describe(muon_completion_func comp,
                               bool enabled,
                               uint32_t count,
                               double ratio,
                               const char* label) {
  char buffer[128];
  snprintf(buffer, sizeof(buffer), "%s:%u:%.2f:%s", enabled ? "true" : "false",
           count, ratio, label);
  const auto* result = buffer;
  comp(&result, nullptr);
}

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_type_descriptor type_u32 = {
    MUON_TYPE_U32,
    nullptr,
};

static const muon_type_descriptor type_f64 = {
    MUON_TYPE_F64,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor beta_describe_args[] = {
    type_bool,
    type_u32,
    type_f64,
    type_string,
};

static const muon_plugin_function_metadata beta_functions[] = {
    {
        "betaName",
        reinterpret_cast<muon_native_function>(&beta_name),
        {0, nullptr, &type_string},
        nullptr,
    },
    {
        "betaDescribe",
        reinterpret_cast<muon_native_function>(&beta_describe),
        {4, beta_describe_args, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const beta_functions_pointers[] = {
    &beta_functions[0],
    &beta_functions[1],
    nullptr,
};

static const muon_plugin_namespace beta_namespaces[] = {
    {
        "muon.test.beta",
        nullptr,
        beta_functions_pointers,
    },
};

static const muon_plugin_namespace* const beta_namespaces_pointers[] = {
    &beta_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata beta_metadata = {
    beta_namespaces_pointers,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  return &beta_metadata;
}
