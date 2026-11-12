/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "muon_plugin_api.h"

/** Native service ABI revision used by service artifact manifests. */
#define MUON_SERVICE_ABI_VERSION 1

/**
 * @file muon_service_api.h
 * @brief Platform-independent, asynchronous native service lifecycle ABI.
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
   * @param completion Non-null callback called after disposal.
   * @param completion_data State to pass unchanged to completion.
   * @remarks Stop may complete inline. Cancel outstanding work and release all
   *     instance state before completion. The host context stays valid until
   *     completion returns. OS deadlines may terminate the process even if stop
   *     is unfinished; this hook does not guarantee execution after a crash.
   */
  void (*stop)(void* user_data, muon_service_stop_completion completion,
               void* completion_data);
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
