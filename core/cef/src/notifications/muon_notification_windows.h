/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include "notifications/muon_notification_service.h"

namespace muon_internal {

/** Application identity and assets registered with Windows for local notifications. */
struct MuonWindowsNotificationOptions {
  /** Architecture-specific, stable AppUserModelID. */
  std::string app_id;
  /** User-visible application name. */
  std::string display_name;
  /** Absolute path of the installed PNG notification icon. */
  std::string icon_path;
  /** Stable COM activator identifier associated with this application identity. */
  std::string activator_clsid;
  /** Absolute originating launcher path; empty for direct runtime execution. */
  std::string launcher_path;
};

/**
 * Creates the Windows notification backend for one application identity.
 * @param options Stable identity and notification assets for the application.
 * @returns Operations owned by the current cardio/COM apartment.
 * @remarks User-level identity registration is asynchronous. Cold process
 * activation and removal of installed registration belong to the installer/launcher.
 * Missing registration metadata and OS initialization failures remain observable
 * through capabilities/status instead of aborting application startup. Stop must
 * run on the owner dispatcher to release its COM apartment resources.
 */
MuonNotificationBackend CreateMuonWindowsNotificationBackend(
    MuonWindowsNotificationOptions options);

}  // namespace muon_internal
