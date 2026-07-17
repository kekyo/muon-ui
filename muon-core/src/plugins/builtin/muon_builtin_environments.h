/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

#include "config/muon_config.h"

/**
 * JavaScript-visible namespace metadata for built-in environment functions.
 */
extern const muon_plugin_namespace kMuonBuiltinEnvironmentsNamespace;

/**
 * Stores the application configuration returned by getConfigValues().
 *
 * @param config Application-defined string key-value configuration.
 */
void InitializeMuonBuiltinEnvironments(
    const std::vector<MuonStringConfigEntry>& config);

/**
 * Returns metadata for JavaScript-visible built-in environment functions.
 */
const muon_plugin_metadata* GetMuonBuiltinEnvironmentsPluginMetadata();
