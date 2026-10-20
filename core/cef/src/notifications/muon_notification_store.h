/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include "notifications/muon_notification_model.h"
#include <cardio.h>
#include <string>

namespace muon_internal {

/** Loads one local snapshot asynchronously; only an absent file means empty state.
 * @param path Local snapshot file path.
 * @return Parsed state; rejects on I/O failure or corruption.
 */
cardio::promise<MuonNotificationSnapshot> LoadMuonNotificationSnapshot(
    std::string path);
/**
 * Writes a private sibling file before atomically replacing the local snapshot.
 * @param path Local snapshot file path.
 * @param state Complete validated snapshot to persist.
 * @return Completion after replacement, or an I/O error.
 * @remarks The caller serializes writes. Errors preserve the previous snapshot.
 */
cardio::promise<void> SaveMuonNotificationSnapshot(
    std::string path, MuonNotificationSnapshot state);
/** Generates a 256-bit revision token using the operating system random device.
 * @return A hexadecimal token, or an I/O error.
 */
cardio::promise<std::string> CreateMuonNotificationToken();

}  // namespace muon_internal
