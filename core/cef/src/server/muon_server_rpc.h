/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "plugins/muon_plugin_metadata.h"
#include "rpc/muon_rpc.h"

/** Replaces the server command owner with the IPC-authenticated page identity.
 * @param request Authorized invocation, modified only for the builtin server dispatcher.
 * @param functions Host-registered function metadata used to identify the dispatcher.
 */
void BindMuonServerOwner(MuonRpcCallRequest& request,
                        const std::vector<MuonFunctionMetadata>& functions);
