/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <stdlib.h>
#include <string.h>

#include "muon_plugin_api.h"

static const muon_type_descriptor void_type = {MUON_TYPE_VOID, NULL};

static void never_called(void) {
  abort();
}

static const muon_plugin_function_metadata alpha_function = {
    "alpha",
    never_called,
    {0, NULL, &void_type},
    NULL,
};

static const muon_plugin_function_metadata native_beta_function = {
    "nativeBeta",
    never_called,
    {0, NULL, &void_type},
    "beta",
};

static const muon_plugin_function_metadata *const namespace_functions[] = {
    &alpha_function,
    &native_beta_function,
    NULL,
};

static const muon_plugin_namespace namespace_metadata = {
    "test.namespace",
    NULL,
    namespace_functions,
};

static const muon_plugin_namespace *const plugin_namespaces[] = {
    &namespace_metadata,
    NULL,
};

static const muon_plugin_metadata plugin_metadata = {
    plugin_namespaces,
    NULL,
    NULL,
};

const muon_plugin_metadata *muon_init_plugin(
    const muon_plugin_init_context *context) {
  const char *mode = muon_plugin_get_config_value(context, "mode");
  if (context == NULL || context->plugin_name == NULL ||
      strcmp(context->plugin_name, "valid-plugin") != 0 || mode == NULL ||
      strcmp(mode, "metadata") != 0) {
    return NULL;
  }
  return &plugin_metadata;
}
