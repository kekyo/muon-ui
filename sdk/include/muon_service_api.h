/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "muon_plugin_api.h"
#include "muon_service_value.h"
#include "muon_service_lifecycle_types.h"
#include "muon_service_io.h"

/** Native service ABI revision used by service artifact manifests. */
#define MUON_SERVICE_ABI_VERSION 3

/**
 * @file muon_service_api.h
 * @brief Platform-independent asynchronous service lifecycle and typed call ABI.
 *
 * Export muon_get_service_factory with C linkage and MUON_PLUGIN_EXPORT.
 * A factory may create several independent logical services from one library.
 * No CEF, JNI, JavaScript engine, or page context is needed to start a service.
 *
 * @remarks All entry points, completions, and posted tasks execute on the host's
 * owner dispatcher. Do not block it: C++ services can use the SDK's cardio
 * runtime for asynchronous I/O. No C++ exception may cross this C ABI. The
 * factory and function pointers remain valid while the library is loaded.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @param user_data State supplied when posting this task. */
typedef void (*muon_service_task)(void* user_data);

/** Borrowed failure; both strings are copied by the receiving completion. */
typedef struct muon_service_error {
  const char* code; /**< Declared application code or a common service error. */
  const char* message; /**< Non-null UTF-8 diagnostic. */
} muon_service_error;

/**
 * Completes a method or callback exactly once on the owner dispatcher.
 * @param completion_data State supplied with the invocation.
 * @param value Non-null on success, including an explicit void value.
 * @param error Non-null on failure, with value null.
 * @remarks The receiver copies values before returning. A native method must
 *     finish all its callback requests before completing. Its input values and
 *     call context remain valid until this completion returns, and must not be
 *     used afterwards. Cancellation still requires a final completion.
 */
typedef void (*muon_service_result_completion)(void* completion_data,
    const muon_service_value* value, const muon_service_error* error);

/** Context retained for one method invocation, independently of other calls. */
typedef struct muon_service_call_context {
  uint64_t call_id; /**< Nonzero host identity used by cancellation. */
  void* user_data; /**< Opaque state supplied unchanged to callback. */
  /**
   * Invokes an input callback owned by this method invocation.
   * @param user_data Context state.
   * @param token Callback token received within this method's arguments.
   * @param arguments Borrowed arguments, copied before this operation returns.
   * @param count Number of arguments; zero permits a null arguments pointer.
   * @param complete Non-null completion, always deferred on the owner dispatcher.
   * @param completion_data Caller state retained until completion.
   * @return Nonzero when accepted and completion is guaranteed; zero rejects
   *     invalid, cancelled, or excessive requests without scheduling completion.
   * @remarks Cancellation or page loss completes outstanding requests with an
   *     error. The caller must drain these completions before completing its
   *     method. Tokens cannot be retained after the parent method ends.
   */
  int (*callback)(void* user_data, uint64_t token,
      const muon_service_value* arguments, size_t count,
      muon_service_result_completion complete, void* completion_data);
  const muon_service_signal* signal; /**< Cancelled when the invocation ends or the caller withdraws it. */
} muon_service_call_context;

/** Host operations borrowed by one service until its stop completion. */
typedef struct muon_service_host {
  /** Opaque state passed unchanged to every host operation. */
  void* user_data;
  /**
   * Queues a task on the owner dispatcher, never invoking it inline.
   * @param user_data This host's opaque state.
   * @param task Non-null function to execute once.
   * @param task_data State owned by the caller until task executes.
   * @remarks Call only on the owner dispatcher. All queued tasks and async I/O
   *     must finish or be cancelled before completing startup failure or stop.
   */
  void (*post)(void* user_data, muon_service_task task, void* task_data);
  /**
   * Writes a service-scoped diagnostic, copying message before returning.
   * @param user_data This host's opaque state.
   * @param level Diagnostic severity.
   * @param message Non-null, borrowed UTF-8 message.
   */
  void (*log)(void* user_data, muon_log_level level, const char* message);
  /**
   * Publishes a copied, typed event to this generation's active subscriptions.
   * @param user_data This host's opaque state.
   * @param event_name Event declared in the factory's interface.
   * @param value Borrowed payload, copied before returning.
   * @return Nonzero if accepted; zero for invalid data, exhausted queue capacity,
   *     or a stopping service.
   * @remarks Acceptance does not guarantee a subscriber exists. Events do not
   *     retain page references or survive service termination.
   */
  int (*emit)(void* user_data, const char* event_name, const muon_service_value* value);
} muon_service_host;

/** Immutable inputs retained by the host through startup and stop completion. */
typedef struct muon_service_context {
  /** Host operations valid for this instance. */
  const muon_service_host* host;
  /** Configured logical service identity, not a process identity. */
  const char* service_id;
  /** Runtime generation; never reuse an instance from a previous generation. */
  uint64_t generation;
  /** Absolute private directory in which this service may store its data. */
  const char* data_directory;
  /** Number of string configuration entries. */
  int32_t config_count;
  /** Borrowed entries; null when config_count is zero. */
  const muon_plugin_config_entry* config_entries;
  muon_service_start_reason start_reason; /**< What caused this generation to start. */
  const muon_service_signal* signal; /**< Cancelled when shutdown begins, including during startup. */
  const muon_service_io* io; /**< Permission-checked, asynchronous I/O for this generation. */
} muon_service_context;

/**
 * Reports that all work and instance-owned resources have been disposed.
 * @param completion_data Opaque state supplied to stop.
 * @remarks Invoke exactly once on the owner dispatcher. Do not call host
 *     operations or leave callbacks outstanding after invoking this completion.
 *     The host keeps the library loaded until the stop callback returns.
 *     This includes asynchronous framework cleanup: a detached coroutine can
 *     still queue its own destruction after its body returns. Retain such work
 *     in the instance, post final disposal with host.post after it completes,
 *     and destroy the completed promises before invoking this callback.
 */
typedef void (*muon_service_stop_completion)(void* completion_data);

/** One independently owned service instance; copied by the start completion. */
typedef struct muon_service_instance {
  /** Implementation state, released by stop before reporting completion. */
  void* user_data;
  /**
   * Begins asynchronous disposal, called exactly once for a successful start.
   * @param user_data Instance state returned at startup.
   * @param info Reason, optional deadline and cancellation for cleanup I/O.
   * @param completion Non-null callback called after disposal.
   * @param completion_data State to pass unchanged to completion.
   * @remarks Stop may complete inline. Cancel outstanding work and release all
   *     instance state before completion. The host context stays valid until
   *     completion returns. OS deadlines may terminate the process even if stop
   *     is unfinished; this hook does not guarantee execution after a crash.
   */
  void (*stop)(void* user_data, const muon_service_stop_info* info, muon_service_stop_completion completion,
               void* completion_data);
  /**
   * Begins a typed method invocation; null for an interface with no methods.
   * @param user_data Instance state.
   * @param context Call identity and invocation-owned callback operations.
   * @param method Declared method name, retained until completion returns.
   * @param arguments Typed input values retained until completion returns.
   * @param count Argument count; zero permits a null arguments pointer.
   * @param complete Non-null completion; may be called inline.
   * @param completion_data State to pass unchanged to completion.
   */
  void (*invoke)(void* user_data, const muon_service_call_context* context,
      const char* method, const muon_service_value* arguments, size_t count,
      muon_service_result_completion complete, void* completion_data);
  /**
   * Requests cancellation of one pending invocation, at most once per call.
   * @param user_data Instance state.
   * @param call_id Identity from that call's context.
   * @remarks Required when invoke is present. The invocation must still complete
   *     once after draining its work and callback requests. Stop is called only
   *     after every invocation has completed, including cancelled invocations.
   */
  void (*cancel)(void* user_data, uint64_t call_id);
} muon_service_instance;

/**
 * Completes asynchronous startup on the owner dispatcher, exactly once.
 * @param completion_data Opaque state supplied to start.
 * @param instance On success, a borrowed instance descriptor with non-null stop.
 * @param error Null on success, otherwise a borrowed nonempty UTF-8 diagnostic.
 * @remarks The host copies instance/error before returning. Failure requires
 *     instance to be null and all resources and callbacks already disposed.
 *     A successful instance transfers ownership to the host, which later calls
 *     stop even if the acquiring page disappeared during startup.
 */
typedef void (*muon_service_start_completion)(
    void* completion_data, const muon_service_instance* instance,
    const char* error);

/** Library-level factory shared by logical service instances. */
typedef struct muon_service_factory {
  /** Must equal MUON_SERVICE_ABI_VERSION. */
  uint32_t abi_version;
  /**
   * Begins creation of an instance, independently of every other start call.
   * @param context Immutable inputs retained through this instance's lifetime.
   * @param completion Non-null callback called after initialization or rollback.
   * @param completion_data State to pass unchanged to completion.
   * @remarks May complete inline. Keep instance state separate from library
   *     initialization; several service IDs can use the same factory at once.
   */
  void (*start)(const muon_service_context* context,
                muon_service_start_completion completion,
                void* completion_data);
  /**
   * Version 1 interface JSON describing methods, results, errors, and events.
   * Null declares an empty interface for lifecycle-only services. This immutable
   * UTF-8 string remains valid for the lifetime of the loaded library.
   */
  const char* interface_json;
} muon_service_factory;

/**
 * Type of the exported muon_get_service_factory entry point.
 * @return Non-null descriptor valid until the library is unloaded.
 * @remarks Queries metadata without creating work or an instance. The host
 *     calls this once per library load; it does not call muon_init_plugin.
 */
typedef const muon_service_factory* (*muon_get_service_factory_func)(void);

#ifdef __cplusplus
}
#endif
