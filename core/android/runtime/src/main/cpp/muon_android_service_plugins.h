/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "services/muon_service_plugin_pool.h"

/** Creates the private process's plugin pool from APK-resolved library locators.
 * @param registry Packaged plugin definitions, with Java class-loader locators.
 * @param definitions Separate grants for each QuickJS service.
 * @param dispatcher Process-lifetime Android owner dispatcher.
 * @returns Shared native pool, or null when no native modules are configured.
 */
std::unique_ptr<MuonServicePluginPool> CreateMuonAndroidServicePlugins(
    const std::string& registry, std::vector<MuonServicePluginDefinition> definitions,
    cardio::dispatcher& dispatcher);
