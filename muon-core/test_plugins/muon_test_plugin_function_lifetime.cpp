/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"
#include "muon_cardio_post.h"

#include <cardio.h>

#include <stdint.h>
#include <utility>

static const muon_plugin_helpers* helper_table = nullptr;
static muon_native_function retained_function = nullptr;
static muon_completion_func pending_overlap_completion = nullptr;
static muon_native_function pending_overlap_function = nullptr;
static bool pending_overlap_arguments_match = false;

using FunctionToFunction = void (*)(muon_completion_func,
                                    muon_native_function);
using VoidFunction = void (*)(muon_completion_func);

static void complete_bool(muon_completion_func comp, bool value) {
  const auto result = value;
  comp(&result, nullptr);
}

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

extern "C" void lifetime_same_pointer(muon_completion_func comp,
                                       muon_native_function first,
                                       muon_native_function second) {
  complete_bool(comp, first != nullptr && first == second);
}

extern "C" void lifetime_different_pointer(muon_completion_func comp,
                                            muon_native_function first,
                                            muon_native_function second) {
  complete_bool(comp, first != nullptr && second != nullptr && first != second);
}

extern "C" void lifetime_async_same_pointer(muon_completion_func comp,
                                             muon_native_function first,
                                             muon_native_function second) {
  schedule_async(comp, [comp, first, second]() {
    complete_bool(comp, first != nullptr && first == second);
  });
}

extern "C" void lifetime_overlap_same_pointer(muon_completion_func comp,
                                               muon_native_function first,
                                               muon_native_function second) {
  const auto arguments_match = first != nullptr && first == second;
  if (pending_overlap_completion == nullptr) {
    pending_overlap_completion = comp;
    pending_overlap_function = first;
    pending_overlap_arguments_match = arguments_match;
    return;
  }

  const auto previous_completion = pending_overlap_completion;
  const auto calls_match = pending_overlap_arguments_match &&
                           arguments_match &&
                           pending_overlap_function == first;
  pending_overlap_completion = nullptr;
  pending_overlap_function = nullptr;
  pending_overlap_arguments_match = false;
  complete_bool(previous_completion, calls_match);
  complete_bool(comp, calls_match);
}

extern "C" void lifetime_null_pointer(muon_completion_func comp,
                                       muon_native_function function) {
  complete_bool(comp, function == nullptr);
}

extern "C" void lifetime_return_null_function(muon_completion_func comp) {
  muon_native_function function = nullptr;
  comp(&function, nullptr);
}

extern "C" void lifetime_null_callback_roundtrip(
    muon_completion_func comp,
    muon_native_function callback) {
  reinterpret_cast<FunctionToFunction>(callback)(comp, nullptr);
}

extern "C" void lifetime_retain(muon_completion_func comp,
                                 muon_native_function function) {
  if (helper_table == nullptr ||
      helper_table->__retain_plugin_function_pointer_impl == nullptr) {
    comp(nullptr, "retain helper is unavailable");
    return;
  }

  const auto retained =
      helper_table->retain_plugin_function_pointer(function) != 0;
  if (retained != 0) {
    retained_function = function;
  }
  complete_bool(comp, retained);
}

extern "C" void lifetime_retained_matches(muon_completion_func comp,
                                           muon_native_function function) {
  complete_bool(comp, retained_function != nullptr &&
                          retained_function == function);
}

extern "C" void lifetime_invoke_retained(muon_completion_func comp) {
  if (retained_function == nullptr) {
    comp(nullptr, "retained function is unavailable");
    return;
  }
  reinterpret_cast<VoidFunction>(retained_function)(comp);
}

extern "C" void lifetime_finalize_retained(muon_completion_func comp) {
  if (helper_table != nullptr &&
      helper_table->__release_plugin_function_pointer_impl != nullptr &&
      retained_function != nullptr) {
    helper_table->release_plugin_function_pointer(retained_function);
    retained_function = nullptr;
  }
  comp(nullptr, nullptr);
}

extern "C" void lifetime_async_retain_finalize(muon_completion_func comp,
                                                muon_native_function function) {
  schedule_async(comp, [comp, function]() {
    auto retained = false;
    if (helper_table != nullptr &&
        helper_table->__retain_plugin_function_pointer_impl != nullptr) {
      retained = helper_table->retain_plugin_function_pointer(function) != 0;
    }
    if (retained != 0 && helper_table != nullptr &&
        helper_table->__release_plugin_function_pointer_impl != nullptr) {
      helper_table->release_plugin_function_pointer(function);
    }
    complete_bool(comp, retained);
  });
}

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_function_signature void_callback_signature = {
    0,
    nullptr,
    &type_void,
};

static const muon_type_descriptor type_void_callback = {
    MUON_TYPE_FUNCTION,
    &void_callback_signature,
};

static const muon_type_descriptor two_function_args[] = {
    type_void_callback,
    type_void_callback,
};

static const muon_type_descriptor one_function_arg[] = {
    type_void_callback,
};

static const muon_function_signature function_to_function_signature = {
    1,
    one_function_arg,
    &type_void_callback,
};

static const muon_type_descriptor type_function_to_function = {
    MUON_TYPE_FUNCTION,
    &function_to_function_signature,
};

static const muon_type_descriptor one_function_to_function_arg[] = {
    type_function_to_function,
};

static const muon_plugin_function_metadata lifetime_functions[] = {
    {
        "lifetimeSamePointer",
        reinterpret_cast<muon_native_function>(&lifetime_same_pointer),
        {2, two_function_args, &type_bool},
        nullptr,
    },
    {
        "lifetimeDifferentPointer",
        reinterpret_cast<muon_native_function>(&lifetime_different_pointer),
        {2, two_function_args, &type_bool},
        nullptr,
    },
    {
        "lifetimeAsyncSamePointer",
        reinterpret_cast<muon_native_function>(&lifetime_async_same_pointer),
        {2, two_function_args, &type_bool},
        nullptr,
    },
    {
        "lifetimeOverlapSamePointer",
        reinterpret_cast<muon_native_function>(&lifetime_overlap_same_pointer),
        {2, two_function_args, &type_bool},
        nullptr,
    },
    {
        "lifetimeNullPointer",
        reinterpret_cast<muon_native_function>(&lifetime_null_pointer),
        {1, one_function_arg, &type_bool},
        nullptr,
    },
    {
        "lifetimeReturnNullFunction",
        reinterpret_cast<muon_native_function>(&lifetime_return_null_function),
        {0, nullptr, &type_void_callback},
        nullptr,
    },
    {
        "lifetimeNullCallbackRoundtrip",
        reinterpret_cast<muon_native_function>(&lifetime_null_callback_roundtrip),
        {1, one_function_to_function_arg, &type_void_callback},
        nullptr,
    },
    {
        "lifetimeRetain",
        reinterpret_cast<muon_native_function>(&lifetime_retain),
        {1, one_function_arg, &type_bool},
        nullptr,
    },
    {
        "lifetimeRetainedMatches",
        reinterpret_cast<muon_native_function>(&lifetime_retained_matches),
        {1, one_function_arg, &type_bool},
        nullptr,
    },
    {
        "lifetimeInvokeRetained",
        reinterpret_cast<muon_native_function>(&lifetime_invoke_retained),
        {0, nullptr, &type_void},
        nullptr,
    },
    {
        "lifetimeFinalizeRetained",
        reinterpret_cast<muon_native_function>(&lifetime_finalize_retained),
        {0, nullptr, &type_void},
        nullptr,
    },
    {
        "lifetimeAsyncRetainFinalize",
        reinterpret_cast<muon_native_function>(
            &lifetime_async_retain_finalize),
        {1, one_function_arg, &type_bool},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const lifetime_functions_pointers[] = {
    &lifetime_functions[0],
    &lifetime_functions[1],
    &lifetime_functions[2],
    &lifetime_functions[3],
    &lifetime_functions[4],
    &lifetime_functions[5],
    &lifetime_functions[6],
    &lifetime_functions[7],
    &lifetime_functions[8],
    &lifetime_functions[9],
    &lifetime_functions[10],
    &lifetime_functions[11],
    nullptr,
};

static const muon_plugin_namespace lifetime_namespaces[] = {
    {
        "muon.test.functionLifetime",
        nullptr,
        lifetime_functions_pointers,
    },
};

static const muon_plugin_namespace* const lifetime_namespaces_pointers[] = {
    &lifetime_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata lifetime_metadata = {
    lifetime_namespaces_pointers,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  helper_table = context == nullptr ? nullptr : context->helpers;
  retained_function = nullptr;
  pending_overlap_completion = nullptr;
  pending_overlap_function = nullptr;
  pending_overlap_arguments_match = false;
  return &lifetime_metadata;
}
