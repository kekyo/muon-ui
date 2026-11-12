/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "plugins/muon_plugin_metadata.h"
#include "rpc/muon_rpc.h"

/**
 * Replaces the native service owner with the authenticated browser IPC owner.
 * @param request Authorized decoded invocation; its second argument is replaced.
 * @param functions Registered metadata, independent of caller-supplied paths.
 * @remarks Unrelated functions and proxy invocations remain untouched.
 */
void BindMuonServiceOwner(MuonRpcCallRequest& request,
    const std::vector<MuonFunctionMetadata>& functions);
