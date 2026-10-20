/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include "plugins/muon_plugin_metadata.h"
#include "rpc/muon_rpc.h"

/**
 * Binds notification watch calls to the owner authenticated by the host.
 * @param request Decoded, authorized invocation; its owner argument is replaced.
 * @param functions Registered function metadata, independent of caller path text.
 * @remarks Other functions and proxy calls keep their original arguments.
 */
void BindMuonNotificationWatchOwner(
    MuonRpcCallRequest& request,
    const std::vector<MuonFunctionMetadata>& functions);
