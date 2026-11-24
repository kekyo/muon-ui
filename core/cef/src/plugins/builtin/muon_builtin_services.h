/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include <cardio.h>
#include "muon_plugin_api.h"
#include <filesystem>
#include <memory>
#include <vector>
struct MuonConfig;
class MuonServiceSession;
/** @param config Resolves app/profile identity and starts a non-owning connection.
 *  @param config_paths Absolute configuration files to retain in login startup. */
void InitializeMuonBuiltinServices(const MuonConfig& config,
    const std::vector<std::filesystem::path>& config_paths);
/** @param config Application and profile configuration.
 *  @param config_paths Absolute startup configuration paths.
 *  @param refresh Whether ordinary launch/setup should reconcile OS registration.
 *  @param login_launch Whether a login registration launched this process.
 *  @return A host session, or null for applications with only a UI. */
std::shared_ptr<MuonServiceSession> CreateMuonBuiltinServiceSession(const MuonConfig& config,
    const std::vector<std::filesystem::path>& config_paths, bool refresh, bool login_launch);
/** @param config Application with no service host.
 *  @param config_paths Absolute startup configuration paths.
 *  @param refresh Whether to reconcile OS registration during installer setup.
 *  @return The saved UI login preference for this profile. */
cardio::promise<bool> ReadMuonUiOnlyLogin(const MuonConfig& config,
    const std::vector<std::filesystem::path>& config_paths, bool refresh);
/** @returns Service metadata after initialization, including an empty service registry. */
const muon_plugin_metadata* GetMuonBuiltinServicesMetadata();
/** @param completion Receives the committed UI auto-display preference as JSON. */
void GetMuonBuiltinUiAutostart(muon_completion_func completion);
/** @param completion Completes after UI preference persistence and login reconciliation.
 *  @param enabled Whether login should display UI, independent of services. */
void SetMuonBuiltinUiAutostart(muon_completion_func completion, bool enabled);
/** Marks explicit application exit so plugin shutdown also awaits independent services. */
void RequestMuonBuiltinServicesShutdown();
/** Releases UI service state after the asynchronous plugin stop callback completes. */
void ShutdownMuonBuiltinServices();
