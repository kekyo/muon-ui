/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "notifications/muon_notification_service.h"

namespace muon_internal {
/** Creates an asynchronous freedesktop backend for the current session bus.
 * @param application_name Human-readable name shown by notification services.
 * @param desktop_id Desktop entry ID without the .desktop suffix.
 * @remarks The backend reports interactions only while this process is running.
 */
MuonNotificationBackend CreateMuonLinuxNotificationBackend(
    std::string application_name, std::string desktop_id);
}  // namespace muon_internal
