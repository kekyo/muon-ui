/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

#include <string>

/**
 * Initializes the built-in filesystem plugin runtime.
 *
 * @param context Plugin initialization context supplied by muon.
 * @param error_message Receives an initialization diagnostic.
 * @return true when the runtime is ready.
 */
bool InitializeMuonBuiltinFs(const muon_plugin_init_context* context,
                              std::string* error_message);

/**
 * Shuts down the built-in filesystem plugin runtime.
 */
void ShutdownMuonBuiltinFs();

/**
 * Releases filesystem state owned by a released renderer V8 context.
 */
void ReleaseMuonBuiltinFsContext(int renderer_context_id);

/**
 * JavaScript-visible namespace metadata for built-in filesystem functions.
 */
extern const muon_plugin_namespace kMuonBuiltinFsNamespace;

/**
 * Returns metadata for JavaScript-visible built-in filesystem functions.
 */
const muon_plugin_metadata* GetMuonBuiltinFsPluginMetadata();
