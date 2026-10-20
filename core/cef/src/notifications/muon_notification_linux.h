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
 * @param application_id Runtime application identifier for installation ownership.
 * @param launcher_path Absolute distributed launcher path; empty for direct runtime execution.
 * @remarks Registered installations use Portal activation. Other environments
 * use the freedesktop notification service and report their launch limitation.
 */
MuonNotificationBackend CreateMuonLinuxNotificationBackend(
    std::string application_name, std::string desktop_id,
    std::string application_id, std::string launcher_path);
}  // namespace muon_internal
