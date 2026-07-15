/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include <stdlib.h>

#include "muon_plugin_api.h"

static const muon_type_descriptor void_type = {MUON_TYPE_VOID, NULL};

static void never_called(void) {
  abort();
}

static const muon_plugin_function_metadata invalid_namespace_function = {
    "alpha",
    never_called,
    {0, NULL, &void_type},
    NULL,
};

static const muon_plugin_function_metadata *const namespace_functions[] = {
    &invalid_namespace_function,
    NULL,
};

static const muon_plugin_namespace namespace_metadata = {
    "invalid",
    NULL,
    namespace_functions,
};

static const muon_plugin_namespace *const plugin_namespaces[] = {
    &namespace_metadata,
    NULL,
};

static const muon_plugin_metadata plugin_metadata = {
    plugin_namespaces,
};

const muon_plugin_metadata *muon_init_plugin(
    const muon_plugin_init_context *context) {
  (void)context;
  return &plugin_metadata;
}
