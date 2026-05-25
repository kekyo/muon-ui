/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

/**
 * Returns C ABI metadata for native built-in plugin functions.
 */
const muon_plugin_metadata* GetMuonBuiltinPluginMetadata();
