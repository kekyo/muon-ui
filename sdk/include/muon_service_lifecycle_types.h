/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include <stdint.h>

/** The demand that created a service generation. */
typedef enum muon_service_start_reason {
  MUON_SERVICE_START_ACQUIRE, /**< The first client acquired the service. */
  MUON_SERVICE_START_APP, /**< Resident policy was active at application startup. */
  MUON_SERVICE_START_LOGIN_AUTOSTART, /**< The desktop login registration launched the host. */
  MUON_SERVICE_START_AUTOSTART_ENABLED, /**< The user enabled residency. */
  MUON_SERVICE_START_DEVELOPMENT_UPDATE /**< Development rebuilt the service host. */
} muon_service_start_reason;

/** The first termination request accepted for a service generation. */
typedef enum muon_service_stop_reason {
  MUON_SERVICE_STOP_REFERENCE_RELEASED, /**< The last reference was released. */
  MUON_SERVICE_STOP_APP_EXIT, /**< Explicit application shutdown. */
  MUON_SERVICE_STOP_SESSION_ENDING, /**< The OS confirmed session or device shutdown. */
  MUON_SERVICE_STOP_DEVELOPMENT_UPDATE, /**< A rebuilt development host will take over. */
  MUON_SERVICE_STOP_DEVELOPMENT_EXIT, /**< The development supervisor ended. */
  MUON_SERVICE_STOP_EXECUTION_TIMEOUT, /**< The OS withdrew the execution allowance. */
  MUON_SERVICE_STOP_HOST, /**< The host was asked to stop without a more specific reason. */
} muon_service_stop_reason;

/** @param reason Native start reason. @return The portable SDK name or null for an unknown value. */
static inline const char* muon_service_start_reason_name(muon_service_start_reason reason) {
  switch (reason) {
    case MUON_SERVICE_START_ACQUIRE: return "acquire";
    case MUON_SERVICE_START_APP: return "app-start";
    case MUON_SERVICE_START_LOGIN_AUTOSTART: return "login-autostart";
    case MUON_SERVICE_START_AUTOSTART_ENABLED: return "autostart-enabled";
    case MUON_SERVICE_START_DEVELOPMENT_UPDATE: return "development-update";
  }
  return 0;
}

/** @param reason Native stop reason. @return The portable SDK name or null for an unknown value. */
static inline const char* muon_service_stop_reason_name(muon_service_stop_reason reason) {
  switch (reason) {
    case MUON_SERVICE_STOP_REFERENCE_RELEASED: return "reference-released";
    case MUON_SERVICE_STOP_APP_EXIT: return "app-exit";
    case MUON_SERVICE_STOP_SESSION_ENDING: return "session-ending";
    case MUON_SERVICE_STOP_DEVELOPMENT_UPDATE: return "development-update";
    case MUON_SERVICE_STOP_DEVELOPMENT_EXIT: return "development-exit";
    case MUON_SERVICE_STOP_EXECUTION_TIMEOUT: return "execution-timeout";
    case MUON_SERVICE_STOP_HOST: return "host-stop";
  }
  return 0;
}

/** Owner-dispatcher cancellation. Borrowed through completion of the enclosing operation. */
typedef struct muon_service_signal {
  void* user_data; /**< Pass unchanged to the following operations. */
  int (*aborted)(void* user_data); /**< Returns nonzero after cancellation. */
  /** Registers a one-shot owner-dispatcher listener. Returns zero if already cancelled. */
  uint64_t (*subscribe)(void* user_data, void (*notify)(void*), void* data);
  /** Removes a listener; an expired identity is harmless. */
  void (*unsubscribe)(void* user_data, uint64_t subscription);
} muon_service_signal;

/** Inputs borrowed until the service completes stop on the owner dispatcher. */
typedef struct muon_service_stop_info {
  uint32_t abi_version; /**< MUON_SERVICE_ABI_VERSION. */
  uint32_t size; /**< Size of this structure in bytes. */
  muon_service_stop_reason reason; /**< Why the host requested shutdown. */
  const char* detail; /**< Optional UTF-8 platform detail, or null. */
  int64_t remaining_ms; /**< Milliseconds remaining at handler entry, or -1 if unknown. */
  const muon_service_signal* signal; /**< Cancellation source for cleanup I/O. */
} muon_service_stop_info;
