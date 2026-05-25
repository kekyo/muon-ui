/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "network/muon_network_policy.h"

#include "include/cef_resource_request_handler.h"

#include <memory>
#include <string>

/**
 * Creates a resource request handler that enforces the network allowlist.
 *
 * @param policy Network access policy.
 * @param is_top_level_navigation Whether this request is a main-frame
 * navigation.
 * @param request_initiator Origin URL of the page that initiated the request.
 * @return CEF resource request handler for normal browser requests.
 */
CefRefPtr<CefResourceRequestHandler> CreateMuonNetworkResourceRequestHandler(
    std::shared_ptr<MuonNetworkPolicy> policy,
    bool is_top_level_navigation,
    std::string request_initiator);
