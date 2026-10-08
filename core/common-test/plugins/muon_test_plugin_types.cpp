/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"
#include "muon_cardio_post.h"

#include <cardio.h>

#include <cstddef>
#include <stdint.h>
#include <utility>

using StringCallback = void (*)(muon_completion_func, const char*);
using I64Callback = void (*)(muon_completion_func, int64_t);
using U64Callback = void (*)(muon_completion_func, uint64_t);
using PointerCallback = void (*)(muon_completion_func, void*);
using BufferCallback = void (*)(muon_completion_func, muon_buffer_view);

static const muon_plugin_helpers* helper_table = nullptr;
static muon_completion_func pending_buffer_callback_completion = nullptr;
static muon_native_function registered_buffer_function = nullptr;
static char helper_error_message[128] = "";
static uint8_t normal_buffer_storage[8] = {};
static uint8_t transformed_buffer_storage[8] = {};
static uint8_t callback_buffer_storage[4] = {};
static uint8_t proxy_buffer_storage[8] = {};

template <typename Task>
static void schedule_async(muon_completion_func comp, Task&& task) {
  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr) {
    comp(nullptr, "muon main dispatcher is unavailable");
    return;
  }
  muon_internal::FireAndForgetOnDispatcher(
      dispatcher, std::forward<Task>(task));
}

static void set_helper_error_message(const char* message) {
  auto index = uint32_t{0};
  while (index + 1 < sizeof(helper_error_message) && message[index] != '\0') {
    helper_error_message[index] = message[index];
    ++index;
  }
  helper_error_message[index] = '\0';
}

static bool is_expected_pattern(const muon_buffer_view& view,
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

static void fill_transformed(uint8_t* target,
                             const muon_buffer_view& source) {
  const auto* bytes = static_cast<const uint8_t*>(source.data);
  for (auto index = size_t{0}; index < source.size; ++index) {
    target[index] = static_cast<uint8_t>(bytes[source.size - 1 - index] ^ 0xa5u);
  }
}

extern "C" void echo_bool(muon_completion_func comp, bool value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_i8(muon_completion_func comp, int8_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_u8(muon_completion_func comp, uint8_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_i16(muon_completion_func comp, int16_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_u16(muon_completion_func comp, uint16_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_i32(muon_completion_func comp, int32_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_u32(muon_completion_func comp, uint32_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_i64(muon_completion_func comp, int64_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_u64(muon_completion_func comp, uint64_t value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_f32(muon_completion_func comp, float value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_f64(muon_completion_func comp, double value) {
  const auto result = value;
  comp(&result, nullptr);
}

extern "C" void echo_pointer(muon_completion_func comp, void* value) {
  auto* result = value;
  comp(&result, nullptr);
}

extern "C" void echo_string(muon_completion_func comp,
                             const char* value) {
  comp(&value, nullptr);
}

extern "C" void return_null_string(muon_completion_func comp) {
  const char* result = nullptr;
  comp(&result, nullptr);
}

extern "C" void return_null_pointer(muon_completion_func comp) {
  void* result = nullptr;
  comp(&result, nullptr);
}

extern "C" void string_null_callback_roundtrip(
    muon_completion_func comp,
    muon_native_function callback) {
  const char* value = nullptr;
  reinterpret_cast<StringCallback>(callback)(comp, value);
}

extern "C" void i64_callback_roundtrip(muon_completion_func comp,
                                        muon_native_function callback,
                                        int64_t value) {
  reinterpret_cast<I64Callback>(callback)(comp, value);
}

extern "C" void u64_callback_roundtrip(muon_completion_func comp,
                                        muon_native_function callback,
                                        uint64_t value) {
  reinterpret_cast<U64Callback>(callback)(comp, value);
}

extern "C" void pointer_callback_roundtrip(muon_completion_func comp,
                                           muon_native_function callback,
                                           void* value) {
  reinterpret_cast<PointerCallback>(callback)(comp, value);
}

extern "C" void buffer_checksum(muon_completion_func comp,
                                 muon_buffer_view value) {
  if (value.data == nullptr && value.size != 0) {
    comp(nullptr, "invalid buffer_view");
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(value.data);
  auto result = uint32_t{0};
  for (auto index = size_t{0}; index < value.size; ++index) {
    result += bytes[index];
  }
  comp(&result, nullptr);
}

extern "C" void transform_buffer(muon_completion_func comp,
                                  muon_buffer_view value) {
  if (value.data == nullptr || value.size != sizeof(transformed_buffer_storage)) {
    comp(nullptr, "invalid buffer_view");
    return;
  }
  fill_transformed(transformed_buffer_storage, value);
  muon_buffer_view result = {
      transformed_buffer_storage,
      static_cast<uintptr_t>(sizeof(transformed_buffer_storage)),
  };
  comp(&result, nullptr);
}

extern "C" void mutate_buffer_copy(muon_completion_func comp,
                                    muon_buffer_view value) {
  if (value.data == nullptr || value.size != sizeof(transformed_buffer_storage)) {
    comp(nullptr, "invalid buffer_view");
    return;
  }
  auto* bytes = static_cast<uint8_t*>(value.data);
  for (auto index = size_t{0}; index < value.size; ++index) {
    bytes[index] = static_cast<uint8_t>(bytes[index] + 1);
    transformed_buffer_storage[index] = static_cast<uint8_t>(bytes[index] ^ 0x5au);
  }
  muon_buffer_view result = {
      transformed_buffer_storage,
      static_cast<uintptr_t>(sizeof(transformed_buffer_storage)),
  };
  comp(&result, nullptr);
}

extern "C" void return_normal_buffer(muon_completion_func comp) {
  for (auto index = size_t{0}; index < sizeof(normal_buffer_storage); ++index) {
    normal_buffer_storage[index] = static_cast<uint8_t>(31 + index * 3);
  }
  muon_buffer_view result = {
      normal_buffer_storage,
      static_cast<uintptr_t>(sizeof(normal_buffer_storage)),
  };
  comp(&result, nullptr);
}

extern "C" void return_shared_buffer(muon_completion_func comp) {
  if (helper_table == nullptr ||
      helper_table->allocate_shared_buffer == nullptr) {
    comp(nullptr, "shared buffer helper is unavailable");
    return;
  }
  muon_buffer_view result = {nullptr, 0};
  muon_shared_buffer_handle handle = nullptr;
  muon_error_buffer error = {
      helper_error_message,
      static_cast<uint32_t>(sizeof(helper_error_message)),
  };
  if (!helper_table->allocate_shared_buffer(
          static_cast<uintptr_t>(sizeof(normal_buffer_storage)), &result,
          &handle, &error)) {
    comp(nullptr, helper_error_message);
    return;
  }
  auto* bytes = static_cast<uint8_t*>(result.data);
  for (auto index = size_t{0}; index < result.size; ++index) {
    bytes[index] = static_cast<uint8_t>(91 + index);
  }
  comp(&result, nullptr);
}

static void complete_buffer_callback(const void* result,
                                     const char* error_message) {
  const auto outer = pending_buffer_callback_completion;
  pending_buffer_callback_completion = nullptr;
  if (outer == nullptr) {
    return;
  }
  if (error_message != nullptr) {
    outer(nullptr, error_message);
    return;
  }
  if (result == nullptr) {
    outer(nullptr, "callback returned a non-buffer_view value");
    return;
  }
  const auto returned = *static_cast<const muon_buffer_view*>(result);
  const uint8_t expected[] = {101, 102, 103, 104};
  if (!is_expected_pattern(returned, expected, sizeof(expected))) {
    outer(nullptr, "callback returned unexpected bytes");
    return;
  }
  const auto success = uint32_t{1};
  outer(&success, nullptr);
}

extern "C" void buffer_callback_roundtrip(muon_completion_func comp,
                                           muon_native_function callback) {
  pending_buffer_callback_completion = comp;
  for (auto index = size_t{0}; index < sizeof(callback_buffer_storage); ++index) {
    callback_buffer_storage[index] = static_cast<uint8_t>(7 + index);
  }
  muon_buffer_view value = {
      callback_buffer_storage,
      static_cast<uintptr_t>(sizeof(callback_buffer_storage)),
  };
  reinterpret_cast<BufferCallback>(callback)(complete_buffer_callback, value);
}

extern "C" void plugin_buffer_transform(muon_completion_func comp,
                                         muon_buffer_view value) {
  if (value.data == nullptr || value.size != sizeof(proxy_buffer_storage)) {
    comp(nullptr, "invalid proxy buffer_view argument");
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(value.data);
  for (auto index = size_t{0}; index < value.size; ++index) {
    proxy_buffer_storage[index] = static_cast<uint8_t>(bytes[index] + 17);
  }
  muon_buffer_view result = {
      proxy_buffer_storage,
      static_cast<uintptr_t>(sizeof(proxy_buffer_storage)),
  };
  comp(&result, nullptr);
}

extern "C" void return_buffer_function(muon_completion_func comp) {
  if (registered_buffer_function == nullptr) {
    comp(nullptr, helper_error_message[0] == '\0'
                      ? "buffer function is unavailable"
                      : helper_error_message);
    return;
  }
  comp(&registered_buffer_function, nullptr);
}

extern "C" void pointer_bit_size(muon_completion_func comp) {
  const auto result = static_cast<uint32_t>(sizeof(uintptr_t) * 8);
  comp(&result, nullptr);
}

extern "C" void return_void(muon_completion_func comp) {
  comp(nullptr, nullptr);
}

extern "C" void reject_value(muon_completion_func comp) {
  comp(nullptr, "plugin failure");
}

extern "C" void resolve_async(muon_completion_func comp, int32_t value) {
  schedule_async(comp, [comp, value]() {
    const auto result = value + 1;
    comp(&result, nullptr);
  });
}

extern "C" void resolve_twice(muon_completion_func comp) {
  const auto* first = "first";
  comp(&first, nullptr);
  const auto* second = "second";
  comp(&second, nullptr);
}

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_type_descriptor type_i8 = {
    MUON_TYPE_I8,
    nullptr,
};

static const muon_type_descriptor type_u8 = {
    MUON_TYPE_U8,
    nullptr,
};

static const muon_type_descriptor type_i16 = {
    MUON_TYPE_I16,
    nullptr,
};

static const muon_type_descriptor type_u16 = {
    MUON_TYPE_U16,
    nullptr,
};

static const muon_type_descriptor type_i32 = {
    MUON_TYPE_I32,
    nullptr,
};

static const muon_type_descriptor type_u32 = {
    MUON_TYPE_U32,
    nullptr,
};

static const muon_type_descriptor type_i64 = {
    MUON_TYPE_I64,
    nullptr,
};

static const muon_type_descriptor type_u64 = {
    MUON_TYPE_U64,
    nullptr,
};

static const muon_type_descriptor type_f32 = {
    MUON_TYPE_F32,
    nullptr,
};

static const muon_type_descriptor type_f64 = {
    MUON_TYPE_F64,
    nullptr,
};

static const muon_type_descriptor type_pointer = {
    MUON_TYPE_POINTER,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor type_buffer_view = {
    MUON_TYPE_BUFFER_VIEW,
    nullptr,
};

static const muon_type_descriptor bool_args[] = {type_bool};
static const muon_type_descriptor i8_args[] = {type_i8};
static const muon_type_descriptor u8_args[] = {type_u8};
static const muon_type_descriptor i16_args[] = {type_i16};
static const muon_type_descriptor u16_args[] = {type_u16};
static const muon_type_descriptor i32_args[] = {type_i32};
static const muon_type_descriptor u32_args[] = {type_u32};
static const muon_type_descriptor i64_args[] = {type_i64};
static const muon_type_descriptor u64_args[] = {type_u64};
static const muon_type_descriptor f32_args[] = {type_f32};
static const muon_type_descriptor f64_args[] = {type_f64};
static const muon_type_descriptor pointer_args[] = {type_pointer};
static const muon_type_descriptor string_args[] = {type_string};
static const muon_type_descriptor buffer_view_args[] = {type_buffer_view};

static const muon_function_signature string_callback_signature = {
    1,
    string_args,
    &type_string,
};

static const muon_type_descriptor type_string_callback = {
    MUON_TYPE_FUNCTION,
    &string_callback_signature,
};

static const muon_function_signature i64_callback_signature = {
    1,
    i64_args,
    &type_i64,
};

static const muon_type_descriptor type_i64_callback = {
    MUON_TYPE_FUNCTION,
    &i64_callback_signature,
};

static const muon_function_signature u64_callback_signature = {
    1,
    u64_args,
    &type_u64,
};

static const muon_type_descriptor type_u64_callback = {
    MUON_TYPE_FUNCTION,
    &u64_callback_signature,
};

static const muon_function_signature pointer_callback_signature = {
    1,
    pointer_args,
    &type_pointer,
};

static const muon_type_descriptor type_pointer_callback = {
    MUON_TYPE_FUNCTION,
    &pointer_callback_signature,
};

static const muon_function_signature buffer_callback_signature = {
    1,
    buffer_view_args,
    &type_buffer_view,
};

static const muon_type_descriptor type_buffer_callback = {
    MUON_TYPE_FUNCTION,
    &buffer_callback_signature,
};

static const muon_type_descriptor string_callback_args[] = {
    type_string_callback,
};

static const muon_type_descriptor i64_callback_roundtrip_args[] = {
    type_i64_callback,
    type_i64,
};

static const muon_type_descriptor u64_callback_roundtrip_args[] = {
    type_u64_callback,
    type_u64,
};

static const muon_type_descriptor pointer_callback_roundtrip_args[] = {
    type_pointer_callback,
    type_pointer,
};

static const muon_type_descriptor buffer_callback_roundtrip_args[] = {
    type_buffer_callback,
};

static const muon_plugin_function_metadata type_functions[] = {
    {
        "echoBool",
        reinterpret_cast<muon_native_function>(&echo_bool),
        {1, bool_args, &type_bool},
        nullptr,
    },
    {
        "echoI8",
        reinterpret_cast<muon_native_function>(&echo_i8),
        {1, i8_args, &type_i8},
        nullptr,
    },
    {
        "echoU8",
        reinterpret_cast<muon_native_function>(&echo_u8),
        {1, u8_args, &type_u8},
        nullptr,
    },
    {
        "echoI16",
        reinterpret_cast<muon_native_function>(&echo_i16),
        {1, i16_args, &type_i16},
        nullptr,
    },
    {
        "echoU16",
        reinterpret_cast<muon_native_function>(&echo_u16),
        {1, u16_args, &type_u16},
        nullptr,
    },
    {
        "echoI32",
        reinterpret_cast<muon_native_function>(&echo_i32),
        {1, i32_args, &type_i32},
        nullptr,
    },
    {
        "echoU32",
        reinterpret_cast<muon_native_function>(&echo_u32),
        {1, u32_args, &type_u32},
        nullptr,
    },
    {
        "echoI64",
        reinterpret_cast<muon_native_function>(&echo_i64),
        {1, i64_args, &type_i64},
        nullptr,
    },
    {
        "echoU64",
        reinterpret_cast<muon_native_function>(&echo_u64),
        {1, u64_args, &type_u64},
        nullptr,
    },
    {
        "echoF32",
        reinterpret_cast<muon_native_function>(&echo_f32),
        {1, f32_args, &type_f32},
        nullptr,
    },
    {
        "echoF64",
        reinterpret_cast<muon_native_function>(&echo_f64),
        {1, f64_args, &type_f64},
        nullptr,
    },
    {
        "echoPointer",
        reinterpret_cast<muon_native_function>(&echo_pointer),
        {1, pointer_args, &type_pointer},
        nullptr,
    },
    {
        "echoString",
        reinterpret_cast<muon_native_function>(&echo_string),
        {1, string_args, &type_string},
        nullptr,
    },
    {
        "returnNullString",
        reinterpret_cast<muon_native_function>(&return_null_string),
        {0, nullptr, &type_string},
        nullptr,
    },
    {
        "returnNullPointer",
        reinterpret_cast<muon_native_function>(&return_null_pointer),
        {0, nullptr, &type_pointer},
        nullptr,
    },
    {
        "stringNullCallbackRoundtrip",
        reinterpret_cast<muon_native_function>(&string_null_callback_roundtrip),
        {1, string_callback_args, &type_string},
        nullptr,
    },
    {
        "i64CallbackRoundtrip",
        reinterpret_cast<muon_native_function>(&i64_callback_roundtrip),
        {2, i64_callback_roundtrip_args, &type_i64},
        nullptr,
    },
    {
        "u64CallbackRoundtrip",
        reinterpret_cast<muon_native_function>(&u64_callback_roundtrip),
        {2, u64_callback_roundtrip_args, &type_u64},
        nullptr,
    },
    {
        "pointerCallbackRoundtrip",
        reinterpret_cast<muon_native_function>(&pointer_callback_roundtrip),
        {2, pointer_callback_roundtrip_args, &type_pointer},
        nullptr,
    },
    {
        "bufferChecksum",
        reinterpret_cast<muon_native_function>(&buffer_checksum),
        {1, buffer_view_args, &type_u32},
        nullptr,
    },
    {
        "transformBuffer",
        reinterpret_cast<muon_native_function>(&transform_buffer),
        {1, buffer_view_args, &type_buffer_view},
        nullptr,
    },
    {
        "mutateBufferCopy",
        reinterpret_cast<muon_native_function>(&mutate_buffer_copy),
        {1, buffer_view_args, &type_buffer_view},
        nullptr,
    },
    {
        "returnNormalBuffer",
        reinterpret_cast<muon_native_function>(&return_normal_buffer),
        {0, nullptr, &type_buffer_view},
        nullptr,
    },
    {
        "returnSharedBuffer",
        reinterpret_cast<muon_native_function>(&return_shared_buffer),
        {0, nullptr, &type_buffer_view},
        nullptr,
    },
    {
        "bufferCallbackRoundtrip",
        reinterpret_cast<muon_native_function>(&buffer_callback_roundtrip),
        {1, buffer_callback_roundtrip_args, &type_u32},
        nullptr,
    },
    {
        "returnBufferFunction",
        reinterpret_cast<muon_native_function>(&return_buffer_function),
        {0, nullptr, &type_buffer_callback},
        nullptr,
    },
    {
        "pointerBitSize",
        reinterpret_cast<muon_native_function>(&pointer_bit_size),
        {0, nullptr, &type_u32},
        nullptr,
    },
    {
        "returnVoid",
        reinterpret_cast<muon_native_function>(&return_void),
        {0, nullptr, &type_void},
        nullptr,
    },
    {
        "rejectValue",
        reinterpret_cast<muon_native_function>(&reject_value),
        {0, nullptr, &type_string},
        nullptr,
    },
    {
        "resolveAsync",
        reinterpret_cast<muon_native_function>(&resolve_async),
        {1, i32_args, &type_i32},
        nullptr,
    },
    {
        "resolveTwice",
        reinterpret_cast<muon_native_function>(&resolve_twice),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const type_functions_pointers[] = {
    &type_functions[0],
    &type_functions[1],
    &type_functions[2],
    &type_functions[3],
    &type_functions[4],
    &type_functions[5],
    &type_functions[6],
    &type_functions[7],
    &type_functions[8],
    &type_functions[9],
    &type_functions[10],
    &type_functions[11],
    &type_functions[12],
    &type_functions[13],
    &type_functions[14],
    &type_functions[15],
    &type_functions[16],
    &type_functions[17],
    &type_functions[18],
    &type_functions[19],
    &type_functions[20],
    &type_functions[21],
    &type_functions[22],
    &type_functions[23],
    &type_functions[24],
    &type_functions[25],
    &type_functions[26],
    &type_functions[27],
    &type_functions[28],
    &type_functions[29],
    &type_functions[30],
    nullptr,
};

static const muon_plugin_namespace type_namespaces[] = {
    {
        "muon.test.types",
        nullptr,
        type_functions_pointers,
    },
};

static const muon_plugin_namespace* const type_namespaces_pointers[] = {
    &type_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata type_metadata = {
    type_namespaces_pointers,
    nullptr,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  helper_table = context == nullptr ? nullptr : context->helpers;
  pending_buffer_callback_completion = nullptr;
  registered_buffer_function = nullptr;
  helper_error_message[0] = '\0';
  if (helper_table != nullptr &&
      helper_table->__register_pure_function_impl != nullptr) {
    muon_error_buffer error = {
        helper_error_message,
        static_cast<uint32_t>(sizeof(helper_error_message)),
    };
    if (!helper_table->register_pure_function(
            &buffer_callback_signature, &plugin_buffer_transform,
            &registered_buffer_function, &error)) {
      set_helper_error_message(helper_error_message[0] == '\0'
                                   ? "failed to register buffer function"
                                   : helper_error_message);
    }
  }
  return &type_metadata;
}
