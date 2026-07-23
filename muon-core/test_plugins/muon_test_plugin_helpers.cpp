/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

#include <stdint.h>

static const muon_plugin_helpers* helper_table = nullptr;
static muon_native_function helper_closure_function = nullptr;
static int32_t helper_base = 100;
static char helper_error_message[128] = "";

struct CompletionCallbackState {
  muon_completion_func outer_completion = nullptr;
  muon_completion_func nested_completion = nullptr;
};

static void set_helper_error_message(const char* message) {
  auto index = uint32_t{0};
  while (index + 1 < sizeof(helper_error_message) && message[index] != '\0') {
    helper_error_message[index] = message[index];
    ++index;
  }
  helper_error_message[index] = '\0';
}

static void add_helper_base(muon_completion_func completion,
                            void* raw_state,
                            int32_t value) {
  const auto* base = static_cast<const int32_t*>(raw_state);
  const auto result = *base + value;
  completion(&result, nullptr);
}

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_i32 = {
    MUON_TYPE_I32,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor helper_closure_add_args[] = {type_i32};
static const muon_type_descriptor helper_log_info_args[] = {type_string};

static const muon_function_signature i32_callback_signature = {
    1,
    helper_closure_add_args,
    &type_i32,
};

static const muon_type_descriptor type_i32_callback = {
    MUON_TYPE_FUNCTION,
    &i32_callback_signature,
};

static const muon_type_descriptor helper_completion_add_args[] = {
    type_i32_callback,
    type_i32,
};

static void release_nested_completion(CompletionCallbackState* state) {
  if (state != nullptr && state->nested_completion != nullptr &&
      helper_table != nullptr &&
      helper_table->__release_plugin_function_pointer_impl != nullptr) {
    helper_table->release_plugin_function_pointer(state->nested_completion);
  }
}

static void complete_helper_completion_add(
    void* raw_state,
    int32_t value,
    const muon_completion_error* error) {
  auto* state = static_cast<CompletionCallbackState*>(raw_state);
  if (state == nullptr || state->outer_completion == nullptr) {
    release_nested_completion(state);
    delete state;
    return;
  }
  if (error != nullptr) {
    state->outer_completion(nullptr, error->message);
    release_nested_completion(state);
    delete state;
    return;
  }
  const auto result = value + 1;
  state->outer_completion(&result, nullptr);
  release_nested_completion(state);
  delete state;
}

extern "C" void helper_closure_add(muon_completion_func comp, int32_t value) {
  if (helper_closure_function == nullptr) {
    comp(nullptr, helper_error_message[0] == '\0'
                      ? "helper closure is unavailable"
                      : helper_error_message);
    return;
  }

  using HelperClosureFunction = void (*)(muon_completion_func, int32_t);
  reinterpret_cast<HelperClosureFunction>(helper_closure_function)(comp, value);
}

extern "C" void helper_closure_release(muon_completion_func comp) {
  if (helper_table != nullptr &&
      helper_table->__release_plugin_function_pointer_impl != nullptr &&
      helper_closure_function != nullptr) {
    helper_table->release_plugin_function_pointer(helper_closure_function);
    helper_closure_function = nullptr;
  }
  comp(nullptr, nullptr);
}

extern "C" void helper_completion_add(muon_completion_func comp,
                                      muon_native_function callback,
                                      int32_t value) {
  if (callback == nullptr) {
    comp(nullptr, "helper callback is unavailable");
    return;
  }
  if (helper_table == nullptr ||
      helper_table->__create_completion_function_impl == nullptr) {
    comp(nullptr, "completion function helper is unavailable");
    return;
  }

  auto* state = new CompletionCallbackState;
  state->outer_completion = comp;
  char error_message[128] = "";
  muon_error_buffer error = {
      error_message,
      static_cast<uint32_t>(sizeof(error_message)),
  };
  if (!helper_table->create_completion_function(
          &type_i32, &complete_helper_completion_add,
          state, &state->nested_completion, &error)) {
    comp(nullptr, error_message[0] == '\0'
                      ? "completion function helper failed"
                      : error_message);
    delete state;
    return;
  }

  using I32Callback = void (*)(muon_completion_func, int32_t);
  reinterpret_cast<I32Callback>(callback)(state->nested_completion, value);
}

extern "C" void helper_log_info(muon_completion_func comp,
                                const char* message) {
  if (helper_table != nullptr &&
      helper_table->__log_message_impl != nullptr) {
    helper_table->log_message(MUON_LOG_LEVEL_INFO, message);
  }
  comp(nullptr, nullptr);
}

static const muon_plugin_function_metadata helper_functions[] = {
    {
        "helperClosureAdd",
        reinterpret_cast<muon_native_function>(&helper_closure_add),
        {1, helper_closure_add_args, &type_i32},
        nullptr,
    },
    {
        "helperClosureRelease",
        reinterpret_cast<muon_native_function>(&helper_closure_release),
        {0, nullptr, &type_void},
        nullptr,
    },
    {
        "helperCompletionAdd",
        reinterpret_cast<muon_native_function>(&helper_completion_add),
        {2, helper_completion_add_args, &type_i32},
        nullptr,
    },
    {
        "helperLogInfo",
        reinterpret_cast<muon_native_function>(&helper_log_info),
        {1, helper_log_info_args, &type_void},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const helper_functions_pointers[] = {
    &helper_functions[0],
    &helper_functions[1],
    &helper_functions[2],
    &helper_functions[3],
    nullptr,
};

static const muon_plugin_namespace helper_namespaces[] = {
    {
        "muon.test.helpers",
        nullptr,
        helper_functions_pointers,
    },
};

static const muon_plugin_namespace* const helper_namespaces_pointers[] = {
    &helper_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata helper_metadata = {
    helper_namespaces_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  helper_table = context == nullptr ? nullptr : context->helpers;
  helper_error_message[0] = '\0';
  helper_closure_function = nullptr;
  if (helper_table == nullptr ||
      helper_table->__register_closure_impl == nullptr) {
    set_helper_error_message("plugin helpers are unavailable");
    return &helper_metadata;
  }

  const muon_function_signature signature = {
      1,
      helper_closure_add_args,
      &type_i32,
  };
  muon_error_buffer error = {
      helper_error_message,
      static_cast<uint32_t>(sizeof(helper_error_message)),
  };
  helper_table->register_closure(
      &signature, &add_helper_base, &helper_base, nullptr,
      &helper_closure_function, &error);
  return &helper_metadata;
}
