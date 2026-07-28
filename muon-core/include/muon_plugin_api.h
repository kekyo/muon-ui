/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <stdint.h>

/**
 * @file muon_plugin_api.h
 * @brief Public C ABI used by native muon plugins.
 *
 * A muon plugin exports a `muon_init_plugin` function and returns metadata
 * for the functions it wants to expose to JavaScript. Each native function uses
 * a completion-first ABI:
 *
 * `void function(muon_completion_func completion, typed_arguments...)`
 *
 * `muon_function_signature` describes only `typed_arguments` and the
 * completion result type. It does not include the completion callback itself.
 *
 * @remarks The API is C-compatible so plugins may be implemented in C, C++, or
 * another native language that can produce the same ABI. Function and metadata
 * pointers must remain valid for the duration described by the individual
 * type comments below.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Executable function pointer owned by muon or by a plugin.
 *
 * This is an opaque callable pointer. Cast a C function with the ABI described
 * by a `muon_function_signature` to this type when publishing metadata or
 * returning a function value.
 *
 * @remarks Do not call an arbitrary `muon_native_function` directly unless you
 * know the exact native signature associated with it. Function values received
 * as arguments are borrowed unless retained through the
 * `retain_plugin_function_pointer` helper macro.
 */
typedef void (*muon_native_function)(void);

/**
 * @brief Plugin implementation function pointer accepted by helper APIs.
 *
 * Pure functions receive `muon_completion_func` followed by the typed
 * arguments from their signature. Closure functions receive
 * `muon_completion_func`, `void* user_data`, then the typed arguments.
 */
typedef void (*muon_user_function)(void);

/**
 * @brief Completes one asynchronous native function call.
 *
 * @param result Pointer to a value whose C type matches the declared return
 * type. For `MUON_TYPE_STRING`, pass a pointer to `const char*`. For
 * `MUON_TYPE_POINTER`, pass a pointer to `void*`. For `MUON_TYPE_FUNCTION`,
 * pass a pointer to `muon_native_function`. For `MUON_TYPE_BUFFER_VIEW`,
 * pass a pointer to `muon_buffer_view`.
 * @param error_message Null on success. A non-null UTF-8 message rejects the
 * call and causes `result` to be ignored.
 *
 * @remarks For a successful `MUON_TYPE_VOID` result, pass null for both
 * parameters. For all other successful result types, `result` must be non-null.
 * muon copies scalar values and strings before this callback returns. Buffer
 * view memory must remain readable until the callback returns unless it was
 * allocated through `muon_plugin_helpers::allocate_shared_buffer` and returned
 * as the completion result. Only the first completion call for an invocation is
 * used; later calls are ignored. Returning a plugin-owned function value gives
 * muon its own reference; the plugin should still release any registration or
 * retain it no longer needs.
 */
typedef void (*muon_completion_func)(const void* result,
                                      const char* error_message);

/**
 * @brief Finalizes user data attached to a registered closure function.
 *
 * @param user_data The state pointer passed to `register_closure`.
 *
 * @remarks muon calls this when the registered closure function is released.
 * The callback may be null when no cleanup is required.
 */
typedef void (*muon_finalize_user_data)(void* user_data);

/**
 * @brief Value kinds supported by the plugin ABI.
 *
 * @remarks `MUON_TYPE_VOID` is valid only as a return type. Function types
 * require a nested `muon_function_signature`. Non-function types must set
 * `muon_type_descriptor::function_signature` to null.
 */
typedef enum muon_value_type {
  /** No result value. Valid for completion return types only. */
  MUON_TYPE_VOID = 0,
  /** C `bool` value. C plugins should include `<stdbool.h>`. */
  MUON_TYPE_BOOL = 1,
  /** Signed 8-bit integer. */
  MUON_TYPE_I8 = 2,
  /** Unsigned 8-bit integer. */
  MUON_TYPE_U8 = 3,
  /** Signed 16-bit integer. */
  MUON_TYPE_I16 = 4,
  /** Unsigned 16-bit integer. */
  MUON_TYPE_U16 = 5,
  /** Signed 32-bit integer. */
  MUON_TYPE_I32 = 6,
  /** Unsigned 32-bit integer. */
  MUON_TYPE_U32 = 7,
  /** Signed 64-bit integer. JavaScript transport preserves it as text. */
  MUON_TYPE_I64 = 8,
  /** Unsigned 64-bit integer. JavaScript transport preserves it as text. */
  MUON_TYPE_U64 = 9,
  /** 32-bit floating-point number. Non-finite results are rejected. */
  MUON_TYPE_F32 = 10,
  /** 64-bit floating-point number. Non-finite results are rejected. */
  MUON_TYPE_F64 = 11,
  /** Borrowed null-terminated UTF-8 string. Null is accepted as a value. */
  MUON_TYPE_STRING = 12,
  /** Opaque raw pointer value. Null is accepted as a value. */
  MUON_TYPE_POINTER = 13,
  /** Callable function value described by a nested signature. */
  MUON_TYPE_FUNCTION = 14,
  /** Borrowed mutable byte buffer view. */
  MUON_TYPE_BUFFER_VIEW = 15,
} muon_value_type;

/**
 * @brief Log severity accepted by `muon_plugin_helpers::log_message`.
 */
typedef enum muon_log_level {
  /** Debug diagnostic output. */
  MUON_LOG_LEVEL_DEBUG = 0,
  /** Informational output. */
  MUON_LOG_LEVEL_INFO = 1,
  /** Warning output. */
  MUON_LOG_LEVEL_WARNING = 2,
  /** Error output. */
  MUON_LOG_LEVEL_ERROR = 3,
  /** Fatal error output. */
  MUON_LOG_LEVEL_FATAL = 4,
} muon_log_level;

/**
 * @brief Borrowed mutable byte buffer view.
 *
 * @remarks Arguments received from JavaScript are temporary views owned by
 * muon. A plugin may read or write them while the invocation is pending, but
 * must not keep the pointer after completing the invocation. Result buffer views
 * are copied to the renderer unless they refer to a shared buffer allocated by
 * `allocate_shared_buffer`.
 */
typedef struct muon_buffer_view {
  /** Borrowed pointer to the first byte. Must be null only when size is zero. */
  void* data;
  /** Number of addressable bytes in data. */
  uintptr_t size;
} muon_buffer_view;

/**
 * @brief Opaque handle for a host-owned shared byte buffer allocation.
 *
 * @remarks Handles are created by
 * `muon_plugin_helpers::allocate_shared_buffer`. Return the associated
 * `muon_buffer_view` through a completion callback to transfer the allocation
 * to muon, or call `muon_plugin_helpers::release_shared_buffer` if the
 * allocation will not be returned. Do not dereference the handle.
 */
typedef struct muon_shared_buffer* muon_shared_buffer_handle;

/**
 * @brief Completion-first function signature descriptor.
 */
typedef struct muon_function_signature muon_function_signature;

/**
 * @brief Recursive type descriptor used by function signatures.
 */
typedef struct muon_type_descriptor {
  /** Value kind for this position. */
  muon_value_type type;
  /**
   * Nested signature for `MUON_TYPE_FUNCTION`.
   *
   * Must be null for every non-function type and non-null for function types.
   * Nested signatures follow the same completion-first convention as top-level
   * plugin functions.
   */
  const muon_function_signature* function_signature;
} muon_type_descriptor;

/**
 * @brief Function signature metadata excluding the completion callback.
 *
 * @remarks The current host exposes JavaScript-visible plugin functions with
 * at most 32 arguments and supports function type nesting up to 16 levels.
 */
struct muon_function_signature {
  /** Number of typed arguments after the completion callback. */
  uint32_t arg_count;
  /**
   * Argument type table with `arg_count` entries.
   *
   * Must be non-null when `arg_count` is non-zero. Void is not valid as an
   * argument type.
   */
  const muon_type_descriptor* arg_types;
  /**
   * Completion result type.
   *
   * Must be non-null. Use `MUON_TYPE_VOID` for functions that resolve without
   * a value.
   */
  const muon_type_descriptor* return_type;
};

/**
 * @brief Metadata for one JavaScript-visible plugin function.
 */
typedef struct muon_plugin_function_metadata {
  /**
   * JavaScript property name under the plugin namespace object.
   *
   * Must be a non-null valid JavaScript identifier. Duplicate names are skipped
   * by the host.
   */
  const char* js_name;
  /**
   * Native implementation function.
   *
   * Must be non-null and callable with `muon_completion_func` followed by the
   * typed arguments described by `signature`.
   */
  muon_native_function native_func;
  /** Signature for JavaScript arguments and completion result. */
  muon_function_signature signature;
  /**
   * Optional public name used for plugin allow filtering and setup scripts.
   *
   * May be null to use `js_name`. Internal native functions used by setup
   * scripts can set this to the public wrapper function name.
   */
  const char* filter_name;
} muon_plugin_function_metadata;

/**
 * @brief Caller-owned buffer that receives helper diagnostics.
 */
typedef struct muon_error_buffer {
  /** Writable UTF-8 buffer. */
  char* message;
  /**
   * Total buffer capacity including the null terminator.
   *
   * A zero capacity or null message pointer disables diagnostic writes. Helper
   * functions always null-terminate diagnostics when capacity is non-zero.
   */
  uint32_t message_capacity;
} muon_error_buffer;

/**
 * @brief Maximum number of bytes stored in a completion callback diagnostic.
 */
#define MUON_COMPLETION_ERROR_MESSAGE_CAPACITY 256u

/**
 * @brief Error passed to a plugin completion callback.
 */
typedef struct muon_completion_error {
  /** UTF-8 diagnostic text. Empty when no error has been written. */
  char message[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY];
} muon_completion_error;

/**
 * @brief Callback invoked by a completion function created through helpers.
 *
 * @remarks For non-void result types the callback ABI is
 * `void callback(void* user_data, typed_result,
 * const muon_completion_error* error)`. For `MUON_TYPE_VOID` results the ABI
 * is `void callback(void* user_data, const muon_completion_error* error)`.
 * When `error` is non-null, the typed result argument must be ignored.
 */
typedef void (*muon_completion_callback)(void);

/**
 * @brief Registers a plugin-owned pure function with the muon host marshaller.
 *
 * @param signature Completion-first signature for the function.
 * @param function Plugin implementation. The native ABI is
 * `void function(muon_completion_func completion, typed_arguments...)`.
 * @param out_function Receives a callable function pointer on success.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks The returned function pointer is owned by the plugin until it is
 * given to muon as a value or explicitly released with the
 * `release_plugin_function_pointer` helper macro. Use this helper for stateless
 * function values that do not need a per-function user data pointer.
 */
typedef uint8_t (*muon_plugin_helper_register_pure_function)(
    const muon_function_signature* signature,
    muon_user_function function,
    muon_native_function* out_function,
    muon_error_buffer* error);

/**
 * @brief Registers a plugin-owned stateful closure with the muon host
 * marshaller.
 *
 * @param signature Completion-first signature for the closure.
 * @param function Plugin implementation. The native ABI is
 * `void function(muon_completion_func completion, void* state,
 * typed_arguments...)`.
 * @param state User data passed to the implementation.
 * @param finalize_state Optional finalizer for `state`.
 * @param out_function Receives a callable function pointer on success.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks Use this helper for function values that need state. The returned
 * function must be released exactly like a pure function. `finalize_state` runs
 * after the registered function's last release.
 */
typedef uint8_t (*muon_plugin_helper_register_closure)(
    const muon_function_signature* signature,
    muon_user_function function,
    void* state,
    muon_finalize_user_data finalize_state,
    muon_native_function* out_function,
    muon_error_buffer* error);

/**
 * @brief Creates a one-shot completion function for calling nested functions.
 *
 * @param return_type Completion result type expected by the callback.
 * @param callback Plugin callback invoked when the completion is called.
 * @param user_data Opaque state passed to the callback.
 * @param out_completion Receives a completion function pointer on success.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks The returned completion function is owned by the plugin until
 * released with the `release_plugin_function_pointer` helper macro. Only the
 * first call to the completion function is delivered to `callback`; later calls
 * are ignored.
 */
typedef uint8_t (*muon_plugin_helper_create_completion_function)(
    const muon_type_descriptor* return_type,
    muon_completion_callback callback,
    void* user_data,
    muon_completion_func* out_completion,
    muon_error_buffer* error);

/**
 * @brief Retains a muon-owned function pointer beyond the current call.
 *
 * @param retained_func Function value received from muon or JavaScript.
 * @return 1 on success, 0 on failure.
 *
 * @remarks Function arguments are borrowed until the current invocation
 * completes. Retain a function before storing it for use after completion, and
 * release that retain with
 * the `release_plugin_function_pointer` helper macro.
 */
typedef uint8_t (*muon_plugin_retain_function_pointer)(
    muon_native_function retained_func);

/**
 * @brief Releases one retain or registration for a function pointer.
 *
 * @param released_func Function pointer to release. Null is ignored.
 *
 * @remarks Use the `release_plugin_function_pointer` helper macro for function
 * pointers created by `register_pure_function` or `register_closure`,
 * completion functions created by `create_completion_function`, and retains
 * acquired by `retain_plugin_function_pointer`.
 */
typedef void (*muon_plugin_release_function_pointer)(
    muon_native_function released_func);

/**
 * @brief Allocates a host-owned shared byte buffer.
 *
 * @param size Number of writable bytes requested.
 * @param out_view Receives the writable byte view.
 * @param out_handle Receives the opaque allocation handle.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks Fill `out_view->data` and return `out_view` as a
 * `MUON_TYPE_BUFFER_VIEW` completion result to transfer the allocation to
 * muon without copying. If the plugin does not return the view, it must pass
 * `out_handle` to `release_shared_buffer`. After either transfer or release,
 * both the view and handle are invalid.
 */
typedef uint8_t (*muon_plugin_allocate_shared_buffer)(
    uintptr_t size,
    muon_buffer_view* out_view,
    muon_shared_buffer_handle* out_handle,
    muon_error_buffer* error);

/**
 * @brief Releases an unused host-owned shared byte buffer.
 *
 * @param handle Handle returned by `allocate_shared_buffer`. Null is ignored.
 *
 * @remarks Call this only when the associated view will not be returned through
 * a completion callback. Releasing a buffer and then returning a view into it
 * causes the result to fail validation.
 */
typedef void (*muon_plugin_release_shared_buffer)(
    muon_shared_buffer_handle handle);

/**
 * @brief Emits a plugin log message through muon's configured logger.
 *
 * @param level Message severity.
 * @param message Null-terminated UTF-8 message. Null is treated as empty.
 */
typedef void (*muon_plugin_helper_log_message)(
    muon_log_level level,
    const char* message);

/**
 * @brief Host-provided helper functions available to plugins.
 *
 * @remarks The helper table pointer passed to `muon_init_plugin` may be
 * cached by the plugin while the plugin remains loaded. Helper calls fail or
 * become no-ops after the owning muon runtime has been destroyed.
 */
typedef struct muon_plugin_helpers {
  /** ABI slot for `register_pure_function`. */
  muon_plugin_helper_register_pure_function __register_pure_function_impl;
  /** ABI slot for `register_closure`. */
  muon_plugin_helper_register_closure __register_closure_impl;
  /** ABI slot for `retain_plugin_function_pointer`. */
  muon_plugin_retain_function_pointer
      __retain_plugin_function_pointer_impl;
  /** ABI slot for `release_plugin_function_pointer`. */
  muon_plugin_release_function_pointer
      __release_plugin_function_pointer_impl;
  /**
   * @brief Allocates a host-owned shared byte buffer.
   *
   * @param size Number of writable bytes requested.
   * @param out_view Receives the writable byte view.
   * @param out_handle Receives the opaque allocation handle.
   * @param error Optional caller-owned diagnostic buffer.
   * @return 1 on success, 0 on failure.
   *
   * @remarks Fill `out_view->data` and return `out_view` as a
   * `MUON_TYPE_BUFFER_VIEW` completion result to transfer the allocation to
   * muon without copying. If the plugin does not return the view, it must pass
   * `out_handle` to `release_shared_buffer`. After either transfer or release,
   * both the view and handle are invalid.
   */
  muon_plugin_allocate_shared_buffer allocate_shared_buffer;
  /**
   * @brief Releases an unused host-owned shared byte buffer.
   *
   * @param handle Handle returned by `allocate_shared_buffer`. Null is ignored.
   *
   * @remarks Call this only when the associated view will not be returned through
   * a completion callback. Releasing a buffer and then returning a view into it
   * causes the result to fail validation.
   */
  muon_plugin_release_shared_buffer release_shared_buffer;
  /** ABI slot for `create_completion_function`. */
  muon_plugin_helper_create_completion_function
      __create_completion_function_impl;
  /** ABI slot for `log_message`. */
  muon_plugin_helper_log_message __log_message_impl;
} muon_plugin_helpers;

/**
 * @brief Registers a stateless plugin-owned function pointer.
 *
 * @param signature Completion-first signature for the function.
 * @param function Plugin implementation. The native ABI is
 * `void function(muon_completion_func completion, typed_arguments...)`.
 * @param out_function Receives a callable function pointer on success.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks The returned function pointer is owned by the plugin until it is
 * given to muon as a value or explicitly released with the
 * `release_plugin_function_pointer` helper macro. Use this helper for stateless
 * function values that do not need a per-function user data pointer.
 */
#define register_pure_function(signature, function, out_function, error)      \
  __register_pure_function_impl(                                              \
      (signature), (muon_user_function)(function), (out_function), (error))

/**
 * @brief Registers a plugin-owned stateful closure with the muon host
 * marshaller.
 *
 * @param signature Completion-first signature for the closure.
 * @param function Plugin implementation. The native ABI is
 * `void function(muon_completion_func completion, void* state,
 * typed_arguments...)`.
 * @param state User data passed to the implementation.
 * @param finalize_state Optional finalizer for `state`.
 * @param out_function Receives a callable function pointer on success.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks Use this helper for function values that need state. The returned
 * function must be released exactly like a pure function. `finalize_state` runs
 * after the registered function's last release.
 */
#define register_closure(signature, function, state, finalize_state,           \
                         out_function, error)                                 \
  __register_closure_impl((signature), (muon_user_function)(function),        \
                          (state), (muon_finalize_user_data)(finalize_state), \
                          (out_function), (error))

/**
 * @brief Creates a one-shot completion function for calling nested functions.
 *
 * @param return_type Completion result type expected by the callback.
 * @param callback Plugin callback invoked when the completion is called.
 * @param user_data Opaque state passed to the callback.
 * @param out_completion Receives a completion function pointer on success.
 * @param error Optional caller-owned diagnostic buffer.
 * @return 1 on success, 0 on failure.
 *
 * @remarks The returned completion function is owned by the plugin until
 * released with the `release_plugin_function_pointer` helper macro. Only the
 * first call to the completion function is delivered to `callback`; later calls
 * are ignored.
 */
#define create_completion_function(return_type, callback, user_data,           \
                                   out_completion, error)                     \
  __create_completion_function_impl(                                           \
      (return_type), (muon_completion_callback)(callback), (user_data),       \
      (out_completion), (error))

/**
 * @brief Retains a muon-owned function pointer beyond the current call.
 *
 * @param retained_func Function value received from muon or JavaScript.
 * @return 1 on success, 0 on failure.
 *
 * @remarks Function arguments are borrowed until the current invocation
 * completes. Retain a function before storing it for use after completion, and
 * release that retain with
 * the `release_plugin_function_pointer` helper macro.
 */
#define retain_plugin_function_pointer(retained_func)                          \
  __retain_plugin_function_pointer_impl(                                       \
      (muon_native_function)(retained_func))

/**
 * @brief Releases one retain or registration for a function pointer.
 *
 * @param released_func Function pointer to release. Null is ignored.
 *
 * @remarks Use the `release_plugin_function_pointer` helper macro for function
 * pointers created by `register_pure_function` or `register_closure`,
 * completion functions created by `create_completion_function`, and retains
 * acquired by `retain_plugin_function_pointer`.
 */
#define release_plugin_function_pointer(released_func)                         \
  __release_plugin_function_pointer_impl(                                      \
      (muon_native_function)(released_func))

/**
 * @brief Emits a plugin log message through muon's configured logger.
 *
 * @param level Message severity.
 * @param message Null-terminated UTF-8 message. Null is treated as empty.
 */
#define log_message(level, message)                                            \
  __log_message_impl((level), (message))

/**
 * @brief Single string configuration entry for a plugin.
 */
typedef struct muon_plugin_config_entry {
  /**
   * Non-empty UTF-8 configuration key.
   */
  const char* key;
  /**
   * UTF-8 configuration value. Empty strings are valid.
   */
  const char* value;
} muon_plugin_config_entry;

/**
 * @brief Plugin initialization context supplied by the muon host.
 *
 * @remarks `helpers` may be cached while the plugin remains loaded.
 * `plugin_name` and `config_entries` are borrowed only for the duration of the
 * `muon_init_plugin` call. Copy configuration values during initialization when
 * they must be used later.
 */
typedef struct muon_plugin_init_context {
  /**
   * Host helper table. May be null only when the host cannot provide helpers.
   */
  const muon_plugin_helpers* helpers;
  /**
   * Plugin entry name from muon.json.
   */
  const char* plugin_name;
  /**
   * Number of entries in `config_entries`.
   */
  uint32_t config_count;
  /**
   * String key-value configuration entries from plugin.plugins[].config.
   */
  const muon_plugin_config_entry* config_entries;
} muon_plugin_init_context;

static inline uint8_t muon_plugin_config_key_equals(const char* left,
                                                     const char* right) {
  if (left == 0 || right == 0) {
    return 0;
  }
  while (*left != '\0' && *right != '\0') {
    if (*left != *right) {
      return 0;
    }
    ++left;
    ++right;
  }
  return *left == '\0' && *right == '\0';
}

/**
 * @brief Returns the first plugin configuration value for a key.
 *
 * @param context Initialization context supplied to `muon_init_plugin`.
 * @param key Non-null configuration key to search for.
 * @return Borrowed value pointer, or null when the key is not configured.
 */
static inline const char* muon_plugin_get_config_value(
    const muon_plugin_init_context* context,
    const char* key) {
  if (context == 0 || key == 0 || context->config_entries == 0) {
    return 0;
  }
  for (uint32_t index = 0; index < context->config_count; ++index) {
    const muon_plugin_config_entry* entry = &context->config_entries[index];
    if (muon_plugin_config_key_equals(entry->key, key)) {
      return entry->value;
    }
  }
  return 0;
}

/**
 * @brief JavaScript namespace exported by a plugin.
 */
typedef struct muon_plugin_namespace {
  /**
   * JavaScript namespace used to expose this plugin's functions.
   *
   * Must be non-null, non-empty, and composed of valid JavaScript identifier
   * segments separated by dots. For example, `foo.bar` exposes functions under
   * `window.foo.bar`.
   */
  const char* plugin_namespace;
  /**
   * JavaScript function body executed after native functions in this namespace
   * are registered. May be null when no setup is needed.
   *
   * @remarks muon invokes this body as a trusted setup script with
   * `namespace` bound to the namespace object and `globalThis` bound to the V8
   * global object. `isAllowed(name)` returns whether a public function name in
   * this namespace passed plugin allow filtering. The body must remain valid
   * while muon reads plugin metadata.
   */
  const char* setup_script;
  /**
   * Null-terminated table of JavaScript-visible function metadata pointers.
   *
   * May be null to expose no functions. Static storage is recommended so the
   * table and all referenced descriptors remain valid while muon reads the
   * returned metadata.
   */
  const muon_plugin_function_metadata* const* functions;
} muon_plugin_namespace;

/**
 * @brief Completion callback for asynchronous plugin shutdown.
 *
 * @param user_data Opaque value supplied by the muon host.
 *
 * @remarks A plugin stop function must invoke this callback exactly once after
 * all plugin-owned asynchronous operations and resources are stopped. The
 * callback may be invoked before the stop function returns.
 */
typedef void (*muon_plugin_stop_completion)(void* user_data);

/**
 * @brief Starts asynchronous shutdown for a plugin.
 *
 * @param completion Non-null host callback to invoke exactly once.
 * @param user_data Opaque host value passed to `completion`.
 *
 * @remarks muon calls this function at most once and keeps the plugin library,
 * host helpers, and cardio dispatcher available until `completion` is invoked.
 * New work must not be started after this function is called.
 */
typedef void (*muon_plugin_stop_func)(
    muon_plugin_stop_completion completion,
    void* user_data);

/**
 * @brief Notifies a plugin that one renderer-owned function scope was
 * released.
 *
 * @param owner_token Non-null opaque owner token previously supplied to a
 * plugin function owned by that renderer context.
 *
 * @remarks muon invokes this function synchronously on the browser UI thread,
 * at most once for each owner token and loaded plugin, before invalidating
 * renderer-owned function sources. The token pointer remains valid only for
 * the duration of this call. A plugin may begin asynchronous cleanup from this
 * notification, but must not block the renderer release path.
 */
typedef void (*muon_plugin_renderer_context_released_func)(
    const char* owner_token);

/**
 * @brief Metadata returned by a plugin entry point.
 */
typedef struct muon_plugin_metadata {
  /**
   * Null-terminated table of JavaScript namespace metadata pointers.
   *
   * May be null to expose no namespaces. Static storage is recommended so the
   * table and all referenced descriptors remain valid while muon reads the
   * returned metadata.
   */
  const muon_plugin_namespace* const* namespaces;
  /**
   * Optional asynchronous shutdown function.
   *
   * May be null when the plugin owns no resources that require shutdown.
   */
  muon_plugin_stop_func stop;
  /**
   * Optional renderer-context release notification.
   *
   * May be null when the plugin owns no resources scoped to renderer
   * contexts.
   */
  muon_plugin_renderer_context_released_func renderer_context_released;
} muon_plugin_metadata;

/**
 * @brief Plugin entry point type.
 *
 * @param context Initialization context supplied by the muon host.
 * @return Plugin metadata, or null to decline loading.
 *
 * @remarks A plugin library must export a function named `muon_init_plugin`
 * with this signature. muon calls it once while loading the library. Returning
 * invalid metadata causes the library or individual functions to be skipped.
 */
typedef const muon_plugin_metadata* (*muon_init_plugin_func)(
    const muon_plugin_init_context* context);

#ifdef __cplusplus
}
#endif
