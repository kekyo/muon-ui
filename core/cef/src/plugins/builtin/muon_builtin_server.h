/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "muon_plugin_api.h"

/** Initializes the page-owned server service on the browser dispatcher.
 * @param context Runtime-owned buffer allocation helpers, valid until shutdown.
 */
void InitializeMuonBuiltinServer(const muon_plugin_init_context* context);
/** Returns the server plugin metadata, including asynchronous stop and owner release. */
const muon_plugin_metadata* GetMuonBuiltinServerMetadata();
/** Releases the service after its asynchronous stop hook has completed. */
void ShutdownMuonBuiltinServer();
