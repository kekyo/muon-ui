// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#if defined(MUON_PATH_DEPENDENCY)
/** Returns an observable value from the adjacent dependency DLL. */
__declspec(dllexport) int path_dependency_value(void) { return 42; }
#else
#include "muon_plugin_api.h"

__declspec(dllimport) int path_dependency_value(void);

static void read_value(muon_completion_func completion) {
  int32_t value = path_dependency_value();
  completion(&value, NULL);
}

static const muon_type_descriptor value_type = {MUON_TYPE_I32, NULL};
static const muon_plugin_function_metadata value_function = {
    "value", (muon_native_function)&read_value, {0, NULL, &value_type}, NULL};
static const muon_plugin_function_metadata *const functions[] = {
    &value_function, NULL};
static const muon_plugin_namespace path_namespace = {
    "muon.test.path", NULL, functions};
static const muon_plugin_namespace *const namespaces[] = {
    &path_namespace, NULL};
static const muon_plugin_metadata metadata = {namespaces, NULL, NULL};

/** Validates dependency loading even when the inspector only reads metadata. */
__declspec(dllexport) const muon_plugin_metadata *muon_init_plugin(
    const muon_plugin_init_context *context) {
  (void)context;
  return path_dependency_value() == 42 ? &metadata : NULL;
}
#endif
