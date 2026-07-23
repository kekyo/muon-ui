/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

#include <stdint.h>

#include <string>

static std::string alpha_config_value;

extern "C" void alpha_name(muon_completion_func comp) {
  const auto* result = "alpha";
  comp(&result, nullptr);
}

extern "C" void alpha_add(muon_completion_func comp,
                           int32_t a,
                           int32_t b) {
  const auto result = a + b;
  comp(&result, nullptr);
}

extern "C" void alpha_config(muon_completion_func comp) {
  const auto* result = alpha_config_value.c_str();
  comp(&result, nullptr);
}

static const muon_type_descriptor type_i32 = {
    MUON_TYPE_I32,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor alpha_add_args[] = {
    type_i32,
    type_i32,
};

static const muon_plugin_function_metadata alpha_functions[] = {
    {
        "alphaName",
        reinterpret_cast<muon_native_function>(&alpha_name),
        {0, nullptr, &type_string},
        nullptr,
    },
    {
        "alphaAdd",
        reinterpret_cast<muon_native_function>(&alpha_add),
        {2, alpha_add_args, &type_i32},
        nullptr,
    },
    {
        "alphaConfig",
        reinterpret_cast<muon_native_function>(&alpha_config),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const alpha_functions_pointers[] = {
    &alpha_functions[0],
    &alpha_functions[1],
    &alpha_functions[2],
    nullptr,
};

static const muon_plugin_namespace alpha_namespaces[] = {
    {
        "muon.test.alpha",
        nullptr,
        alpha_functions_pointers,
    },
};

static const muon_plugin_namespace* const alpha_namespaces_pointers[] = {
    &alpha_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata alpha_metadata = {
    alpha_namespaces_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  const auto* value = muon_plugin_get_config_value(context, "alpha.config");
  alpha_config_value = value == nullptr ? "" : value;
  return &alpha_metadata;
}
