/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "muon_service_login.h"
#include "services/muon_service_lifecycle.h"
#include <filesystem>
#include <memory>

/**
 * Creates the persistent UI/service preference coordinator before activation.
 * @param directory Private host data root with an existing .autostart directory.
 * @param definitions Configured services with their already loaded overrides.
 * @param startup Trusted launcher JSON, or empty for a standalone host without OS registration.
 * @return Coordinator with UI preference restored and optional registration reconciled.
 */
cardio::promise<std::unique_ptr<MuonServiceLoginPolicy>> CreateMuonServiceLoginPolicy(
    std::filesystem::path directory, std::vector<MuonServiceDefinition> definitions,
    std::string startup);

/** @param policy Current serialized preferences, retained across UI launches.
 *  @param startup Trusted launch metadata with updated paths and registration mode.
 *  @return After installing the command and optional OS registration refresh. */
cardio::promise<void> ConfigureMuonServiceLoginPolicy(MuonServiceLoginPolicy& policy, std::string startup);
