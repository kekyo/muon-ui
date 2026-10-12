/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

#include <cstddef>
#include <stdint.h>

static const muon_plugin_helpers* helper_table = nullptr;
static muon_completion_func pending_outer_completion = nullptr;
static muon_native_function registered_add_one = nullptr;
static char helper_error_message[128] = "";
static muon_completion_func pending_buffer_completion = nullptr;
static uint8_t first_buffer_storage[4] = {12, 13, 14, 15};
static uint8_t second_buffer_storage[4] = {21, 22, 23, 24};
static uint8_t final_buffer_storage[4] = {31, 32, 33, 34};

using I32Function = void (*)(muon_completion_func, int32_t);
using FunctionToFunction = void (*)(muon_completion_func,
                                    muon_native_function);
using BufferToFunction = void (*)(muon_completion_func, muon_buffer_view);
using BufferFunction = void (*)(muon_completion_func, muon_buffer_view);

extern "C" void plugin_add_one(muon_completion_func comp, int32_t value) {
  const auto result = value + 1;
  comp(&result, nullptr);
}

static void complete_outer_with_i32(const void* result,
                                    const char* error_message) {
  const auto outer = pending_outer_completion;
  pending_outer_completion = nullptr;
  if (outer == nullptr) {
    return;
  }
  if (error_message != nullptr) {
    outer(nullptr, error_message);
    return;
  }
  if (result == nullptr) {
    outer(nullptr, "callback returned a non-i32 value");
    return;
  }
  outer(result, nullptr);
}

static void complete_returned_function_call(const void* result,
                                            const char* error_message) {
  complete_outer_with_i32(result, error_message);
}

static void complete_with_returned_function(const void* result,
                                            const char* error_message) {
  if (error_message != nullptr) {
    complete_outer_with_i32(nullptr, error_message);
    return;
  }
  if (result == nullptr) {
    complete_outer_with_i32(nullptr, "callback returned a non-function value");
    return;
  }

  const auto function =
      *static_cast<muon_native_function const*>(result);
  if (function == nullptr) {
    complete_outer_with_i32(nullptr, "callback returned a non-function value");
    return;
  }
  reinterpret_cast<I32Function>(function)(complete_returned_function_call, 32);
}

static void complete_roundtrip_function(const void* result,
                                        const char* error_message) {
  if (error_message != nullptr) {
    complete_outer_with_i32(nullptr, error_message);
    return;
  }
  if (result == nullptr) {
    complete_outer_with_i32(nullptr, "callback returned a non-function value");
    return;
  }

  const auto function =
      *static_cast<muon_native_function const*>(result);
  if (function == nullptr) {
    complete_outer_with_i32(nullptr, "callback returned a non-function value");
    return;
  }
  if (function != registered_add_one) {
    complete_outer_with_i32(nullptr, "function pointer identity changed");
    return;
  }

  reinterpret_cast<I32Function>(function)(complete_returned_function_call, 41);
}

static bool is_expected_buffer(const muon_buffer_view& view,
                               const uint8_t* expected,
                               uintptr_t size) {
  if (view.data == nullptr || view.size != size) {
    return false;
  }
  const auto* bytes = static_cast<const uint8_t*>(view.data);
  for (auto index = size_t{0}; index < size; ++index) {
    if (bytes[index] != expected[index]) {
      return false;
    }
  }
  return true;
}

static void complete_inner_buffer_function(const void* result,
                                           const char* error_message) {
  const auto outer = pending_buffer_completion;
  pending_buffer_completion = nullptr;
  if (outer == nullptr) {
    return;
  }
  if (error_message != nullptr) {
    outer(nullptr, error_message);
    return;
  }
  if (result == nullptr) {
    outer(nullptr, "inner callback returned a non-buffer value");
    return;
  }
  const auto returned = *static_cast<const muon_buffer_view*>(result);
  const uint8_t expected[] = {201, 202, 203, 204};
  if (!is_expected_buffer(returned, expected, sizeof(expected))) {
    outer(nullptr, "inner callback returned unexpected buffer bytes");
    return;
  }
  muon_buffer_view final_result = {
      final_buffer_storage,
      static_cast<uintptr_t>(sizeof(final_buffer_storage)),
  };
  outer(&final_result, nullptr);
}

static void complete_with_buffer_function(const void* result,
                                          const char* error_message) {
  if (error_message != nullptr) {
    complete_inner_buffer_function(nullptr, error_message);
    return;
  }
  if (result == nullptr) {
    complete_inner_buffer_function(nullptr,
                                   "callback returned a non-function value");
    return;
  }
  const auto function = *static_cast<muon_native_function const*>(result);
  if (function == nullptr) {
    complete_inner_buffer_function(nullptr,
                                   "callback returned a non-function value");
    return;
  }
  muon_buffer_view value = {
      second_buffer_storage,
      static_cast<uintptr_t>(sizeof(second_buffer_storage)),
  };
  reinterpret_cast<BufferFunction>(function)(complete_inner_buffer_function,
                                             value);
}

extern "C" void recursive_invoke(muon_completion_func comp,
                                  muon_native_function callback) {
  pending_outer_completion = comp;
  reinterpret_cast<I32Function>(callback)(complete_outer_with_i32, 41);
}

extern "C" void recursive_return_function(muon_completion_func comp,
                                           muon_native_function callback) {
  pending_outer_completion = comp;
  reinterpret_cast<I32Function>(callback)(complete_with_returned_function, 10);
}

extern "C" void recursive_function_arg_roundtrip(
    muon_completion_func comp,
    muon_native_function callback) {
  if (registered_add_one == nullptr) {
    comp(nullptr, helper_error_message[0] == '\0'
                      ? "registered add-one function is unavailable"
                      : helper_error_message);
    return;
  }
  pending_outer_completion = comp;
  reinterpret_cast<FunctionToFunction>(callback)(
      complete_roundtrip_function, registered_add_one);
}

extern "C" void recursive_buffer_return_function(
    muon_completion_func comp,
    muon_native_function callback) {
  pending_buffer_completion = comp;
  muon_buffer_view value = {
      first_buffer_storage,
      static_cast<uintptr_t>(sizeof(first_buffer_storage)),
  };
  reinterpret_cast<BufferToFunction>(callback)(complete_with_buffer_function,
                                               value);
}

static const muon_type_descriptor type_i32 = {
    MUON_TYPE_I32,
    nullptr,
};

static const muon_type_descriptor type_buffer_view = {
    MUON_TYPE_BUFFER_VIEW,
    nullptr,
};

static const muon_type_descriptor i32_function_args[] = {
    type_i32,
};

static const muon_function_signature i32_function_signature = {
    1,
    i32_function_args,
    &type_i32,
};

static const muon_type_descriptor type_i32_function = {
    MUON_TYPE_FUNCTION,
    &i32_function_signature,
};

static const muon_type_descriptor i32_to_function_args[] = {
    type_i32,
};

static const muon_function_signature i32_to_function_signature = {
    1,
    i32_to_function_args,
    &type_i32_function,
};

static const muon_type_descriptor type_i32_to_function = {
    MUON_TYPE_FUNCTION,
    &i32_to_function_signature,
};

static const muon_type_descriptor function_to_function_args[] = {
    type_i32_function,
};

static const muon_function_signature function_to_function_signature = {
    1,
    function_to_function_args,
    &type_i32_function,
};

static const muon_type_descriptor type_function_to_function = {
    MUON_TYPE_FUNCTION,
    &function_to_function_signature,
};

static const muon_type_descriptor buffer_function_args[] = {
    type_buffer_view,
};

static const muon_function_signature buffer_function_signature = {
    1,
    buffer_function_args,
    &type_buffer_view,
};

static const muon_type_descriptor type_buffer_function = {
    MUON_TYPE_FUNCTION,
    &buffer_function_signature,
};

static const muon_type_descriptor buffer_to_function_args[] = {
    type_buffer_view,
};

static const muon_function_signature buffer_to_function_signature = {
    1,
    buffer_to_function_args,
    &type_buffer_function,
};

static const muon_type_descriptor type_buffer_to_function = {
    MUON_TYPE_FUNCTION,
    &buffer_to_function_signature,
};

static const muon_type_descriptor one_i32_function_arg[] = {
    type_i32_function,
};

static const muon_type_descriptor one_i32_to_function_arg[] = {
    type_i32_to_function,
};

static const muon_type_descriptor one_function_to_function_arg[] = {
    type_function_to_function,
};

static const muon_type_descriptor one_buffer_to_function_arg[] = {
    type_buffer_to_function,
};

static const muon_plugin_function_metadata recursive_functions[] = {
    {
        "recursiveInvoke",
        reinterpret_cast<muon_native_function>(&recursive_invoke),
        {1, one_i32_function_arg, &type_i32},
        nullptr,
    },
    {
        "recursiveReturnFunction",
        reinterpret_cast<muon_native_function>(&recursive_return_function),
        {1, one_i32_to_function_arg, &type_i32},
        nullptr,
    },
    {
        "recursiveFunctionArgRoundtrip",
        reinterpret_cast<muon_native_function>(
            &recursive_function_arg_roundtrip),
        {1, one_function_to_function_arg, &type_i32},
        nullptr,
    },
    {
        "recursiveBufferReturnFunction",
        reinterpret_cast<muon_native_function>(
            &recursive_buffer_return_function),
        {1, one_buffer_to_function_arg, &type_buffer_view},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const recursive_functions_pointers[] = {
    &recursive_functions[0],
    &recursive_functions[1],
    &recursive_functions[2],
    &recursive_functions[3],
    nullptr,
};

static const muon_plugin_namespace recursive_namespaces[] = {
    {
        "muon.test.recursiveFunctions",
        nullptr,
        recursive_functions_pointers,
    },
};

static const muon_plugin_namespace* const recursive_namespaces_pointers[] = {
    &recursive_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata recursive_metadata = {
    recursive_namespaces_pointers,
    nullptr,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  helper_table = context == nullptr ? nullptr : context->helpers;
  pending_outer_completion = nullptr;
  pending_buffer_completion = nullptr;
  registered_add_one = nullptr;
  helper_error_message[0] = '\0';
  if (helper_table != nullptr &&
      helper_table->__register_pure_function_impl != nullptr) {
    muon_error_buffer error = {
        helper_error_message,
        static_cast<uint32_t>(sizeof(helper_error_message)),
    };
    helper_table->register_pure_function(
        &i32_function_signature, &plugin_add_one,
        &registered_add_one, &error);
  }
  return &recursive_metadata;
}
