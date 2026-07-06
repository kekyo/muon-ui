/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cardio.h>

#include <string>

#include "muon_plugin_api.h"

/**
 * JavaScript-visible namespace metadata for built-in executor functions.
 */
extern const muon_plugin_namespace kMuonBuiltinExecutorNamespace;

/**
 * Returns metadata for JavaScript-visible built-in executor functions.
 */
const muon_plugin_metadata* GetMuonBuiltinExecutorPluginMetadata();

/**
 * Initializes process state for JavaScript-visible built-in executor functions.
 */
bool InitializeMuonBuiltinExecutor(const muon_plugin_helpers* helpers,
                                   cardio::dispatcher* dispatcher,
                                   std::string* error_message);

/**
 * Terminates and releases all built-in executor process state.
 */
void ShutdownMuonBuiltinExecutor();

/**
 * Terminates executor processes owned by a released renderer V8 context.
 */
void ReleaseMuonBuiltinExecutorContext(int renderer_context_id);
