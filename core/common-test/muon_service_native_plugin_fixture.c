/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_plugin_api.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

static const muon_plugin_helpers* helpers;
static muon_completion_func held_completion;
static char* held_text;
static muon_native_function held_callback;
static bool hold_shutdown;
static muon_plugin_stop_completion stopped;
static void* stopped_context;
static const muon_type_descriptor string_type = {MUON_TYPE_STRING, NULL};
static const muon_type_descriptor i64_type = {MUON_TYPE_I64, NULL};
static const muon_type_descriptor u64_type = {MUON_TYPE_U64, NULL};
static const muon_type_descriptor bytes_type = {MUON_TYPE_BUFFER_VIEW, NULL};
static const muon_type_descriptor void_type = {MUON_TYPE_VOID, NULL};
static const muon_type_descriptor pointer_type = {MUON_TYPE_POINTER, NULL};
static const muon_function_signature string_callback = {1, &string_type, &string_type};
static const muon_type_descriptor function_type = {MUON_TYPE_FUNCTION, &string_callback};
static const muon_function_signature pointer_callback = {0, NULL, &pointer_type};
static const muon_type_descriptor forbidden_type = {MUON_TYPE_FUNCTION, &pointer_callback};
static const muon_type_descriptor callback_arguments[] = {
    {MUON_TYPE_STRING, NULL}, {MUON_TYPE_FUNCTION, &string_callback}};

static void Echo(muon_completion_func complete, const char* value) { complete(&value, NULL); }
static void EchoI64(muon_completion_func complete, int64_t value) { complete(&value, NULL); }
static void EchoU64(muon_completion_func complete, uint64_t value) { complete(&value, NULL); }
static void EchoBytes(muon_completion_func complete, muon_buffer_view value) { complete(&value, NULL); }
static void Fail(muon_completion_func complete) { complete(NULL, "fixture native failure"); }
static void Callback(muon_completion_func complete, const char* value, muon_native_function callback) {
  if (!callback) { complete(&value, NULL); return; }
  ((void (*)(muon_completion_func, const char*))callback)(complete, value);
}
static void MakeEcho(muon_completion_func complete) {
  muon_native_function function = NULL;
  if (!helpers->register_pure_function(&string_callback, Echo, &function, NULL)) {
    complete(NULL, "fixture function registration failed"); return;
  }
  complete(&function, NULL);
  helpers->release_plugin_function_pointer(function);
}
static void Forbidden(muon_completion_func complete, muon_native_function callback) {
  (void)callback; complete(NULL, NULL);
}
static void RetainCallback(muon_completion_func complete, muon_native_function callback) {
  if (held_callback || !helpers->retain_plugin_function_pointer(callback)) {
    complete(NULL, "fixture callback retain failed"); return;
  }
  held_callback = callback;
  complete(NULL, NULL);
}
FIXTURE_EXPORT void muon_fixture_release_callback(void) {
  const muon_native_function callback = held_callback;
  held_callback = NULL;
  if (callback) helpers->release_plugin_function_pointer(callback);
}
static void Hold(muon_completion_func complete, const char* value) {
  if (held_completion) { complete(NULL, "fixture hold is occupied"); return; }
  const size_t size = strlen(value) + 1;
  held_text = malloc(size);
  if (!held_text) { complete(NULL, "fixture allocation failed"); return; }
  memcpy(held_text, value, size);
  held_completion = complete;
}
FIXTURE_EXPORT bool muon_fixture_has_pending(void) { return held_completion != NULL; }
FIXTURE_EXPORT void muon_fixture_complete_pending(void) {
  const muon_completion_func complete = held_completion;
  char* value = held_text;
  held_completion = NULL; held_text = NULL;
  if (complete) complete(&value, NULL);
  free(value);
}
FIXTURE_EXPORT void muon_fixture_hold_shutdown(bool hold) { hold_shutdown = hold; }
FIXTURE_EXPORT bool muon_fixture_is_stopping(void) { return stopped != NULL; }
FIXTURE_EXPORT void muon_fixture_finish_shutdown(void) {
  const muon_plugin_stop_completion complete = stopped;
  void* context = stopped_context;
  stopped = NULL; stopped_context = NULL;
  if (complete) complete(context);
}
static void Stop(muon_plugin_stop_completion complete, void* context) {
  // A real plugin may cancel I/O here. The explicit test barrier keeps pending
  // completion separate so the host must await both completion and shutdown.
  if (hold_shutdown) { stopped = complete; stopped_context = context; }
  else { muon_fixture_complete_pending(); muon_fixture_release_callback(); complete(context); }
}

static const muon_plugin_function_metadata echo = {"echo", (muon_native_function)Echo, {1, &string_type, &string_type}, NULL};
static const muon_plugin_function_metadata i64 = {"echoInt64", (muon_native_function)EchoI64, {1, &i64_type, &i64_type}, NULL};
static const muon_plugin_function_metadata u64 = {"echoUint64", (muon_native_function)EchoU64, {1, &u64_type, &u64_type}, NULL};
static const muon_plugin_function_metadata bytes = {"echoBytes", (muon_native_function)EchoBytes, {1, &bytes_type, &bytes_type}, NULL};
static const muon_plugin_function_metadata callback = {"callback", (muon_native_function)Callback, {2, callback_arguments, &string_type}, NULL};
static const muon_plugin_function_metadata failure = {"fail", (muon_native_function)Fail, {0, NULL, &void_type}, NULL};
static const muon_plugin_function_metadata make = {"makeEcho", (muon_native_function)MakeEcho, {0, NULL, &function_type}, NULL};
static const muon_plugin_function_metadata hold = {"hold", (muon_native_function)Hold, {1, &string_type, &string_type}, NULL};
static const muon_plugin_function_metadata retain = {"retain", (muon_native_function)RetainCallback, {1, &function_type, &void_type}, NULL};
static const muon_plugin_function_metadata forbidden = {"forbidden", (muon_native_function)Forbidden, {1, &forbidden_type, &void_type}, NULL};
static const muon_plugin_function_metadata wrapped = {"__wrapped", (muon_native_function)Echo, {1, &string_type, &string_type}, "wrapped"};
static const muon_plugin_function_metadata* const functions[] = {&echo, &i64, &u64, &bytes, &callback, &failure, &make, &hold, &retain, &forbidden, &wrapped, NULL};
static const muon_plugin_namespace api = {"fixture.api",
    "if(isAllowed('wrapped'))namespace.wrapped=(value)=>namespace.__wrapped(value);", functions};
static const muon_plugin_namespace* const namespaces[] = {&api, NULL};
static const muon_plugin_metadata metadata = {namespaces, Stop, NULL};
FIXTURE_EXPORT const muon_plugin_metadata* muon_init_plugin(const muon_plugin_init_context* context) {
  helpers = context->helpers;
  return &metadata;
}
