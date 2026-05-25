/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

/**
 * JavaScript-visible namespace metadata for built-in environment functions.
 */
extern const muon_plugin_namespace kMuonBuiltinEnvironmentsNamespace;

/**
 * Returns metadata for JavaScript-visible built-in environment functions.
 */
const muon_plugin_metadata* GetMuonBuiltinEnvironmentsPluginMetadata();
