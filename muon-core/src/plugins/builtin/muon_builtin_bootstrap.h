/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

#include <string>

/**
 * JavaScript-visible namespace metadata for built-in bootstrap functions.
 */
extern const muon_plugin_namespace kMuonBuiltinBootstrapNamespace;

/**
 * Sets the default CEF version policy used by built-in bootstrap settings.
 */
void InitializeMuonBuiltinBootstrap(const std::string& default_version_policy);

/**
 * Returns metadata for JavaScript-visible built-in bootstrap functions.
 */
const muon_plugin_metadata* GetMuonBuiltinBootstrapPluginMetadata();
