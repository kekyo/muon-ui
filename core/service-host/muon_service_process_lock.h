/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include <cardio.h>
#include <memory>
#include <string>

/**
 * Acquires a process-shared lock without blocking the dispatcher.
 * @param key Endpoint and operation identifying the protected transition.
 * @param cancellation Cancels waiting, retaining native I/O until completion.
 * @return RAII ownership; releasing the last pointer unlocks the transition.
 * @remarks The OS releases ownership if the process terminates. Lock files
 * remain in a per-user location to preserve their filesystem identity.
 */
cardio::promise<std::shared_ptr<void>> AcquireMuonServiceProcessLock(
    std::string key, cardio::cancellation cancellation);

// A collision only serializes unrelated operations. Endpoint authentication
// remains the transport's responsibility, independently of this filename.
static inline std::string MuonServiceProcessLockHash(const std::string& key) {
  uint64_t value = 14695981039346656037ULL;
  for (const auto byte : key) { value ^= static_cast<unsigned char>(byte); value *= 1099511628211ULL; }
  std::string result(16, '0');
  for (size_t index = 0; index < result.size(); ++index)
    result[index] = "0123456789abcdef"[(value >> ((15 - index) * 4)) & 15];
  return result;
}
