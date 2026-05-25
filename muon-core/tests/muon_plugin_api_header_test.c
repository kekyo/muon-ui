/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

static void sample_completion(const void* result, const char* error_message) {
  (void)result;
  (void)error_message;
}

static void sample_completion_callback(
    void* user_data,
    int32_t result,
    const muon_completion_error* error) {
  (void)user_data;
  (void)result;
  (void)error;
}

static void sample_function(muon_completion_func completion, int32_t value) {
  completion(&value, 0);
}

static void sample_finalizer(void* user_data) {
  (void)user_data;
}

static uint8_t sample_register_pure_function(
    const muon_function_signature* signature,
    muon_user_function function,
    muon_native_function* out_function,
    muon_error_buffer* error) {
  (void)signature;
  (void)function;
  (void)error;
  if (out_function != 0) {
    *out_function = 0;
  }
  return 1;
}

static uint8_t sample_register_closure(
    const muon_function_signature* signature,
    muon_user_function function,
    void* state,
    muon_finalize_user_data finalize_state,
    muon_native_function* out_function,
    muon_error_buffer* error) {
  (void)signature;
  (void)function;
  (void)state;
  (void)finalize_state;
  (void)error;
  if (out_function != 0) {
    *out_function = 0;
  }
  return 1;
}

static uint8_t sample_retain_function_pointer(muon_native_function function) {
  (void)function;
  return 1;
}

static void sample_release_function_pointer(muon_native_function function) {
  (void)function;
}

static uint8_t sample_allocate_shared_buffer(
    uintptr_t size,
    muon_buffer_view* out_view,
    muon_shared_buffer_handle* out_handle,
    muon_error_buffer* error) {
  (void)size;
  (void)out_view;
  (void)out_handle;
  (void)error;
  return 0;
}

static void sample_release_shared_buffer(muon_shared_buffer_handle handle) {
  (void)handle;
}

static uint8_t sample_create_completion_function(
    const muon_type_descriptor* return_type,
    muon_completion_callback callback,
    void* user_data,
    muon_completion_func* out_completion,
    muon_error_buffer* error) {
  (void)return_type;
  (void)callback;
  (void)user_data;
  (void)out_completion;
  (void)error;
  return 0;
}

static void sample_log_message(muon_log_level level, const char* message) {
  (void)level;
  (void)message;
}

int main(void) {
  const muon_type_descriptor type_i32 = {
      MUON_TYPE_I32,
      0,
  };
  const muon_type_descriptor type_i64 = {
      MUON_TYPE_I64,
      0,
  };
  const muon_type_descriptor type_f32 = {
      MUON_TYPE_F32,
      0,
  };
  const muon_type_descriptor type_pointer = {
      MUON_TYPE_POINTER,
      0,
  };
  const muon_type_descriptor type_buffer_view = {
      MUON_TYPE_BUFFER_VIEW,
      0,
  };
  const muon_type_descriptor args[] = {
      {MUON_TYPE_I32, 0},
  };
  const muon_function_signature signature = {
      1,
      args,
      &type_i32,
  };
  const muon_plugin_function_metadata function = {
      "sample",
      (muon_native_function)&sample_function,
      {1, args, &type_i32},
      0,
  };
  const muon_plugin_function_metadata functions[] = {
      function,
  };
  const muon_plugin_function_metadata* const function_pointers[] = {
      &functions[0],
      0,
  };
  const muon_plugin_namespace namespaces[] = {
      {
          "muon.test.header",
          0,
          function_pointers,
      },
  };
  const muon_plugin_namespace* const namespace_pointers[] = {
      &namespaces[0],
      0,
  };
  const muon_plugin_metadata metadata = {
      namespace_pointers,
  };
  muon_plugin_helper_register_pure_function register_pure = 0;
  muon_plugin_helper_register_closure register_closure = 0;
  muon_plugin_retain_function_pointer retain = 0;
  muon_plugin_release_function_pointer release = 0;
  muon_plugin_helper_create_completion_function create_completion =
      &sample_create_completion_function;
  muon_plugin_allocate_shared_buffer allocate_shared_buffer =
      &sample_allocate_shared_buffer;
  muon_plugin_release_shared_buffer release_shared_buffer =
      &sample_release_shared_buffer;
  muon_plugin_helper_log_message log_message = &sample_log_message;
  muon_user_function user_function = (muon_user_function)&sample_function;
  muon_completion_func completion = &sample_completion;
  muon_completion_callback completion_callback =
      (muon_completion_callback)&sample_completion_callback;
  muon_completion_error completion_error = {{0}};
  muon_finalize_user_data finalizer = &sample_finalizer;
  muon_native_function native_function =
      (muon_native_function)&sample_function;
  muon_buffer_view buffer_view = {0, 0};
  muon_shared_buffer_handle shared_buffer = 0;
  muon_log_level log_level = MUON_LOG_LEVEL_INFO;
  muon_plugin_helpers helpers = {
      &sample_register_pure_function,
      &sample_register_closure,
      &sample_retain_function_pointer,
      &sample_release_function_pointer,
      &sample_allocate_shared_buffer,
      &sample_release_shared_buffer,
      &sample_create_completion_function,
      &sample_log_message,
  };

  (void)signature;
  (void)metadata;
  (void)type_i64;
  (void)type_f32;
  (void)type_pointer;
  (void)type_buffer_view;
  (void)register_pure;
  (void)register_closure;
  (void)retain;
  (void)release;
  (void)create_completion;
  (void)allocate_shared_buffer;
  (void)release_shared_buffer;
  (void)log_message;
  (void)user_function;
  (void)completion;
  (void)completion_callback;
  (void)completion_error;
  (void)finalizer;
  (void)native_function;
  (void)buffer_view;
  (void)shared_buffer;
  (void)log_level;
  (void)helpers.register_pure_function(
      &signature, &sample_function, &native_function, 0);
  (void)helpers.register_closure(
      &signature, &sample_function, 0, &sample_finalizer, &native_function, 0);
  (void)helpers.create_completion_function(
      &type_i32, &sample_completion_callback, 0, &completion, 0);
  (void)helpers.retain_plugin_function_pointer(completion);
  helpers.release_plugin_function_pointer(completion);
  helpers.log_message(MUON_LOG_LEVEL_WARNING, "sample");
  return 0;
}
