/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "muon_service_lifecycle_types.h"
#include <stddef.h>

/** One I/O request. The host copies the strings and bytes before returning. */
typedef struct muon_service_io_request {
  uint32_t abi_version; /**< MUON_SERVICE_ABI_VERSION. */
  uint32_t size; /**< sizeof(muon_service_io_request). */
  const char* operation; /**< Portable SDK operation, e.g. fs.read or fs.write. */
  const char* parameters_json; /**< UTF-8 JSON object with path, area and operation-specific fields. */
  const uint8_t* body; /**< Optional binary bytes; may be null for an empty body. */
  size_t body_size; /**< Byte count. */
  const muon_service_signal* signal; /**< Start, invocation or cleanup signal from this generation. */
} muon_service_io_request;

/** Borrowed result valid only while the completion callback is running. */
typedef struct muon_service_io_result {
  uint32_t abi_version; /**< MUON_SERVICE_ABI_VERSION. */
  uint32_t size; /**< sizeof(muon_service_io_result). */
  const char* metadata_json; /**< Successful UTF-8 JSON metadata. */
  const uint8_t* body; /**< Successful binary output, possibly null if empty. */
  size_t body_size; /**< Length of binary output. */
  const char* code; /**< Stable error identifier, null on success. */
  const char* message; /**< Error diagnostic, null on success. */
  const void* fields; /**< Borrowed object for field access during this callback. */
  /** @param fields This result's fields. @param name Top-level metadata key. @return Borrowed string, or null. */
  const char* (*string_field)(const void* fields, const char* name);
  /** @param fields This result's fields. @param name Top-level metadata key. @return Nonzero only for JSON true. */
  int (*boolean_field)(const void* fields, const char* name);
} muon_service_io_result;

/** @param data Caller-owned state, retained until this one owner-dispatcher completion.
 * @param result Borrowed through callback return. */
typedef void (*muon_service_io_completion)(void* data, const muon_service_io_result* result);

/** Generation-bound host operations. Calls and completions use the service dispatcher. */
typedef struct muon_service_io {
  uint32_t abi_version; /**< MUON_SERVICE_ABI_VERSION. */
  uint32_t size; /**< sizeof(muon_service_io). */
  void* user_data; /**< Host identity passed unchanged to request. */
  /** @param user_data This table's identity. @param request Inputs copied before return.
   * @param completion Called once after the submitter returns. @param data User state retained through completion. */
  void (*request)(void* user_data, const muon_service_io_request* request,
                  muon_service_io_completion completion, void* data);
} muon_service_io;
