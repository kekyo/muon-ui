/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

#include <tra_ffic.h>

static inline bool ConvertMuonValueTypeToTraffic(
    muon_value_type source,
    tra_ffic_type_kind* target) {
  if (target == nullptr) {
    return false;
  }
  switch (source) {
    case MUON_TYPE_VOID:
      *target = TRA_FFIC_TYPE_VOID;
      return true;
    case MUON_TYPE_BOOL:
      *target = TRA_FFIC_TYPE_BOOL;
      return true;
    case MUON_TYPE_I8:
      *target = TRA_FFIC_TYPE_INT8;
      return true;
    case MUON_TYPE_U8:
      *target = TRA_FFIC_TYPE_UINT8;
      return true;
    case MUON_TYPE_I16:
      *target = TRA_FFIC_TYPE_INT16;
      return true;
    case MUON_TYPE_U16:
      *target = TRA_FFIC_TYPE_UINT16;
      return true;
    case MUON_TYPE_I32:
      *target = TRA_FFIC_TYPE_INT32;
      return true;
    case MUON_TYPE_U32:
      *target = TRA_FFIC_TYPE_UINT32;
      return true;
    case MUON_TYPE_I64:
      *target = TRA_FFIC_TYPE_INT64;
      return true;
    case MUON_TYPE_U64:
      *target = TRA_FFIC_TYPE_UINT64;
      return true;
    case MUON_TYPE_F32:
      *target = TRA_FFIC_TYPE_FLOAT;
      return true;
    case MUON_TYPE_F64:
      *target = TRA_FFIC_TYPE_DOUBLE;
      return true;
    case MUON_TYPE_STRING:
      *target = TRA_FFIC_TYPE_STRING;
      return true;
    case MUON_TYPE_POINTER:
      *target = TRA_FFIC_TYPE_POINTER;
      return true;
    case MUON_TYPE_FUNCTION:
      *target = TRA_FFIC_TYPE_FUNCTION;
      return true;
    case MUON_TYPE_BUFFER_VIEW:
      *target = TRA_FFIC_TYPE_BUFFER_VIEW;
      return true;
  }
  return false;
}

static inline bool ConvertTrafficValueTypeToMuon(
    tra_ffic_type_kind source,
    muon_value_type* target) {
  if (target == nullptr) {
    return false;
  }
  switch (source) {
    case TRA_FFIC_TYPE_VOID:
      *target = MUON_TYPE_VOID;
      return true;
    case TRA_FFIC_TYPE_BOOL:
      *target = MUON_TYPE_BOOL;
      return true;
    case TRA_FFIC_TYPE_INT8:
      *target = MUON_TYPE_I8;
      return true;
    case TRA_FFIC_TYPE_UINT8:
      *target = MUON_TYPE_U8;
      return true;
    case TRA_FFIC_TYPE_INT16:
      *target = MUON_TYPE_I16;
      return true;
    case TRA_FFIC_TYPE_UINT16:
      *target = MUON_TYPE_U16;
      return true;
    case TRA_FFIC_TYPE_INT32:
      *target = MUON_TYPE_I32;
      return true;
    case TRA_FFIC_TYPE_UINT32:
      *target = MUON_TYPE_U32;
      return true;
    case TRA_FFIC_TYPE_INT64:
      *target = MUON_TYPE_I64;
      return true;
    case TRA_FFIC_TYPE_UINT64:
      *target = MUON_TYPE_U64;
      return true;
    case TRA_FFIC_TYPE_FLOAT:
      *target = MUON_TYPE_F32;
      return true;
    case TRA_FFIC_TYPE_DOUBLE:
      *target = MUON_TYPE_F64;
      return true;
    case TRA_FFIC_TYPE_STRING:
      *target = MUON_TYPE_STRING;
      return true;
    case TRA_FFIC_TYPE_POINTER:
      *target = MUON_TYPE_POINTER;
      return true;
    case TRA_FFIC_TYPE_FUNCTION:
      *target = MUON_TYPE_FUNCTION;
      return true;
    case TRA_FFIC_TYPE_BUFFER_VIEW:
      *target = MUON_TYPE_BUFFER_VIEW;
      return true;
  }
  return false;
}

static inline tra_ffic_user_function ConvertMuonUserFunctionToTraffic(
    muon_user_function function) {
  return reinterpret_cast<tra_ffic_user_function>(function);
}

static inline tra_ffic_completion ConvertMuonCompletionToTraffic(
    muon_completion_func completion) {
  return reinterpret_cast<tra_ffic_completion>(completion);
}

static inline muon_completion_func ConvertTrafficCompletionToMuon(
    tra_ffic_completion completion) {
  return reinterpret_cast<muon_completion_func>(completion);
}

static inline tra_ffic_completion_callback
ConvertMuonCompletionCallbackToTraffic(muon_completion_callback callback) {
  return reinterpret_cast<tra_ffic_completion_callback>(callback);
}

static inline tra_ffic_finalize_user_data ConvertMuonFinalizerToTraffic(
    muon_finalize_user_data finalizer) {
  return reinterpret_cast<tra_ffic_finalize_user_data>(finalizer);
}
