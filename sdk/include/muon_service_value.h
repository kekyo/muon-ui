/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/** Values supported by the portable service interface; no process-local pointers. */
typedef enum muon_service_value_kind {
  MUON_SERVICE_VALUE_VOID = 0, /**< No return value. */
  MUON_SERVICE_VALUE_BOOLEAN, /**< Boolean, represented by exactly zero or one. */
  MUON_SERVICE_VALUE_INT32, /**< Signed 32-bit integer. */
  MUON_SERVICE_VALUE_UINT32, /**< Unsigned 32-bit integer. */
  MUON_SERVICE_VALUE_INT64, /**< Signed 64-bit integer, transported without rounding. */
  MUON_SERVICE_VALUE_UINT64, /**< Unsigned 64-bit integer, transported without rounding. */
  MUON_SERVICE_VALUE_FLOAT64, /**< Finite double-precision number. */
  MUON_SERVICE_VALUE_STRING, /**< UTF-8 text, including embedded NUL characters. */
  MUON_SERVICE_VALUE_BYTES, /**< Arbitrary binary data. */
  MUON_SERVICE_VALUE_ARRAY, /**< Ordered, recursively typed values. */
  MUON_SERVICE_VALUE_CALLBACK /**< Nonzero token valid only during its parent call. */
} muon_service_value_kind;

/** Borrowed UTF-8 view; not required to be NUL terminated. */
typedef struct muon_service_string {
  const char* data; /**< Bytes, or null only when size is zero. */
  size_t size; /**< Byte length, excluding any external terminator. */
} muon_service_string;

/** Borrowed binary view. */
typedef struct muon_service_bytes {
  const uint8_t* data; /**< Bytes, or null only when size is zero. */
  size_t size; /**< Byte length. */
} muon_service_bytes;

struct muon_service_value;

/** Borrowed array view; the declared interface determines its element type. */
typedef struct muon_service_array {
  const struct muon_service_value* data; /**< Elements, or null for an empty array. */
  size_t size; /**< Number of elements, not a byte length. */
} muon_service_array;

/**
 * One value crossing the service C ABI.
 * @remarks Only the union member selected by kind may be read. All views borrow
 * their storage; the operation's completion contract specifies their lifetime.
 * Text must be valid UTF-8 and numbers must be finite. Arrays nest at most 16
 * levels. The host bounds message size and rejects unsupported or invalid values.
 * Callback tokens are opaque identities, never addresses or function pointers.
 */
typedef struct muon_service_value {
  muon_service_value_kind kind; /**< Selects the active union member. */
  union {
    uint8_t boolean_value; /**< MUON_SERVICE_VALUE_BOOLEAN. */
    int32_t int32_value; /**< MUON_SERVICE_VALUE_INT32. */
    uint32_t uint32_value; /**< MUON_SERVICE_VALUE_UINT32. */
    int64_t int64_value; /**< MUON_SERVICE_VALUE_INT64. */
    uint64_t uint64_value; /**< MUON_SERVICE_VALUE_UINT64. */
    double float64_value; /**< MUON_SERVICE_VALUE_FLOAT64. */
    muon_service_string string_value; /**< MUON_SERVICE_VALUE_STRING. */
    muon_service_bytes bytes_value; /**< MUON_SERVICE_VALUE_BYTES. */
    muon_service_array array_value; /**< MUON_SERVICE_VALUE_ARRAY. */
    uint64_t callback_id; /**< MUON_SERVICE_VALUE_CALLBACK. */
  } data; /**< Borrowed payload; unused for MUON_SERVICE_VALUE_VOID. */
} muon_service_value;
