/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#ifndef MUON_BUILTIN_COMPLETION_H
#define MUON_BUILTIN_COMPLETION_H

#include "muon_plugin_api.h"

#include <string>

namespace muon_internal {

/**
 * Completes a builtin plugin function with a void result.
 *
 * @param completion Completion callback passed to the builtin function.
 */
inline void CompleteMuonVoid(muon_completion_func completion) {
  if (completion == nullptr) {
    return;
  }
  completion(nullptr, nullptr);
}

/**
 * Completes a builtin plugin function with a boolean result.
 *
 * @param completion Completion callback passed to the builtin function.
 * @param result Boolean value to return.
 */
inline void CompleteMuonBool(muon_completion_func completion, bool result) {
  if (completion == nullptr) {
    return;
  }
  completion(&result, nullptr);
}

/**
 * Completes a builtin plugin function with a string result.
 *
 * @param completion Completion callback passed to the builtin function.
 * @param result UTF-8 string value to return.
 */
inline void CompleteMuonString(muon_completion_func completion,
                               const std::string& result) {
  if (completion == nullptr) {
    return;
  }
  const auto* pointer = result.c_str();
  completion(&pointer, nullptr);
}

/**
 * Completes a builtin plugin function with a string result.
 *
 * @param completion Completion callback passed to the builtin function.
 * @param result UTF-8 string value to return.
 */
inline void CompleteMuonString(muon_completion_func completion,
                               const char* result) {
  if (completion == nullptr) {
    return;
  }
  const auto* pointer = result;
  completion(&pointer, nullptr);
}

/**
 * Completes a builtin plugin function with an error.
 *
 * @param completion Completion callback passed to the builtin function.
 * @param message Error message.
 */
inline void CompleteMuonError(muon_completion_func completion,
                              const char* message) {
  if (completion == nullptr) {
    return;
  }
  completion(nullptr, message);
}

/**
 * Completes a builtin plugin function with an error.
 *
 * @param completion Completion callback passed to the builtin function.
 * @param message Error message.
 */
inline void CompleteMuonError(muon_completion_func completion,
                              const std::string& message) {
  CompleteMuonError(completion, message.c_str());
}

}  // namespace muon_internal

#endif  // MUON_BUILTIN_COMPLETION_H
