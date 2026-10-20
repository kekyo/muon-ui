/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "notifications/muon_notification_service.h"

namespace muon_internal {

/** Installed Linux identity used for notification activation. */
struct MuonPortalNotificationOptions {
  /** Runtime application identifier recorded by the distribution. */
  std::string application_id;
  /** Desktop basename and D-Bus application name, without `.desktop`. */
  std::string desktop_id;
  /** Absolute path of the distributed launcher that started this runtime. */
  std::string launcher_path;
};

/** Creates asynchronous Portal notifications for a verified Linux installation.
 * The backend exports its activation endpoint before acquiring the application
 * name. Unregistered installations report unsupported launch capabilities.
 */
MuonNotificationBackend CreateMuonPortalNotificationBackend(
    MuonPortalNotificationOptions options);

}  // namespace muon_internal
