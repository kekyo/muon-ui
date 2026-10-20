/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "notifications/muon_notification_service.h"

namespace muon_internal {

/** Withdraws owned OS notifications during an installation change.
 * @param path Application notification snapshot path.
 * @param backend Backend whose launch capability proves installation ownership.
 * @return Completion after cancellation and saving the remaining state.
 * @remarks Runs independently of the public plugin's enabled setting. Pending
 * events and acknowledgements are retained. A failure preserves the snapshot
 * for retry, and the backend is stopped on both success and failure.
 */
cardio::promise<void> ClearMuonNotificationHistory(
    std::string path, MuonNotificationBackend backend);

}  // namespace muon_internal
