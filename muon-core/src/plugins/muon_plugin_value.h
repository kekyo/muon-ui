/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "muon_plugin_api.h"
#include "plugins/muon_shared_buffer.h"
#include "plugins/muon_type_metadata.h"

#include <cstdint>
#include <string>

/**
 * Runtime value copied between the browser plugin runtime and renderer IPC.
 */
struct MuonPluginValue {
  muon_value_type type = MUON_TYPE_VOID;
  bool bool_value = false;
  int8_t i8_value = 0;
  uint8_t u8_value = 0;
  int16_t i16_value = 0;
  uint16_t u16_value = 0;
  int32_t i32_value = 0;
  uint32_t u32_value = 0;
  int64_t i64_value = 0;
  uint64_t u64_value = 0;
  float f32_value = 0.0f;
  double f64_value = 0.0;
  void* pointer_value = nullptr;
  bool is_null = false;
  std::string string_value;
  muon_native_function function_value = nullptr;
  MuonTypeMetadata function_type;
  uint32_t function_proxy_id = 0;
  std::string function_proxy_lease_token;
  muon_buffer_view buffer_view = {nullptr, 0};
};

/**
 * Result of one native plugin invocation.
 */
struct MuonPluginCallResult {
  bool success = false;
  std::string error_message;
  MuonPluginValue value;
  bool has_shared_buffer_message = false;
  MuonCreatedSharedBufferMessage shared_buffer_message;
};
