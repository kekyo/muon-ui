/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_maintenance.h"
#include "notifications/muon_notification_store.h"
#include <stdexcept>

namespace muon_internal {
cardio::promise<void> ClearMuonNotificationHistory(
    std::string path, MuonNotificationBackend backend) {
  try {
    // The Portal launch probe also acquires the activation name. A running
    // application therefore keeps maintenance from racing its snapshot writes.
    const auto caps = co_await backend.capabilities();
    if (!caps.available || caps.launch != true)
      throw std::runtime_error("Notification maintenance requires owned launch registration: " +
                               caps.launch_reason);
    auto state = co_await LoadMuonNotificationSnapshot(path);
    if (!state.revisions.empty()) {
      for (const auto& revision : state.revisions)
        co_await backend.cancel(revision.handle);
      state.revisions.clear();
      co_await SaveMuonNotificationSnapshot(path, state);
    }
  } catch (...) {
    backend.stop();
    throw;
  }
  backend.stop();
}
}  // namespace muon_internal
