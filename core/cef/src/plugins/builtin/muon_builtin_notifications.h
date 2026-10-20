/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "muon_plugin_api.h"
#include <functional>
#include <string>
struct MuonConfig;
/** Creates app-owned notification state from configuration without synchronous I/O. */
void InitializeMuonBuiltinNotifications(const MuonConfig& config);
/** Sets application presentation after a durably accepted notification interaction. */
void SetMuonBuiltinNotificationActivationHandler(std::function<void(std::string)> callback);
/** Returns enabled Linux/Windows notification metadata, or nullptr when disabled. */
const muon_plugin_metadata* GetMuonBuiltinNotificationsMetadata();
/** Releases the builtin after its asynchronous plugin stop callback has completed. */
void ShutdownMuonBuiltinNotifications();
