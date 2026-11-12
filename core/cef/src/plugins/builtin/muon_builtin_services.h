/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "muon_plugin_api.h"
struct MuonConfig;
/** @param config Resolves app/profile service identity and starts a non-owning connection. */
void InitializeMuonBuiltinServices(const MuonConfig& config);
/** @returns Service metadata when an application registry is configured; otherwise nullptr. */
const muon_plugin_metadata* GetMuonBuiltinServicesMetadata();
/** Marks explicit application exit so plugin shutdown also awaits independent services. */
void RequestMuonBuiltinServicesShutdown();
/** Releases UI service state after the asynchronous plugin stop callback completes. */
void ShutdownMuonBuiltinServices();
