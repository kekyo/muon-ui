/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include <muon_service_lifecycle_types.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

/** Stop observation owned by the exact generation. */
struct MuonServiceStopCause {
  muon_service_stop_reason reason = MUON_SERVICE_STOP_REFERENCE_RELEASED; /**< Accepted trigger. */
  std::string detail{}; /**< OS detail where available. */
  std::optional<std::chrono::steady_clock::time_point> deadline{}; /**< Monotonic host limit. */
};

/** @param cause First accepted stop request. @return Milliseconds left, or -1 without a deadline. */
inline int64_t MuonServiceRemainingMs(const MuonServiceStopCause& cause) {
  if (!cause.deadline) return -1;
  return std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
      *cause.deadline - std::chrono::steady_clock::now()).count());
}

/** Owner-dispatcher signal shared with pending startup, including asynchronous module loading. */
struct MuonServiceCancellation {
  bool aborted = false; /**< Sticky after cancellation. */
  uint64_t next = 0; /**< Last issued subscription identity. */
  std::map<uint64_t, std::function<void()>> listeners; /**< Active, one-shot callbacks. */
  /** Cancels once. A listener may remove another listener before it runs. */
  void Cancel() {
    aborted = true;
    while (!listeners.empty()) {
      auto callback = listeners.extract(listeners.begin());
      callback.mapped()();
    }
  }
  /** @param listener Invoked once on cancellation. @return Subscription, or zero if cancelled. */
  uint64_t Subscribe(std::function<void()> listener) {
    if (aborted || next == UINT64_MAX) return 0;
    listeners.emplace(++next, std::move(listener));
    return next;
  }
};

/** @param reason Native start reason. @return Corresponding SDK literal. */
inline const char* MuonServiceReasonName(muon_service_start_reason reason) {
  const auto name = muon_service_start_reason_name(reason);
  if (!name) throw std::invalid_argument("Invalid service start reason");
  return name;
}
/** @param reason Native stop reason. @return Corresponding SDK literal. */
inline const char* MuonServiceReasonName(muon_service_stop_reason reason) {
  const auto name = muon_service_stop_reason_name(reason);
  if (!name) throw std::invalid_argument("Invalid service stop reason");
  return name;
}
