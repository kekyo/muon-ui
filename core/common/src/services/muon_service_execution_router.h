/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "services/muon_service_execution.h"

/**
 * Combines execution hosts without changing reference or generation identities.
 * @param runtimes Borrowed operation tables, copied into the returned router.
 * @param select Resolves an application service identity to its runtime index.
 * @returns Operations routed by service identity; event sinks reach every host.
 * @remarks The registry must keep a service's runtime stable for this router's
 * lifetime. All hosts and the selector's captured registry outlive this table.
 * Calls stay on the hosts' shared owner dispatcher. This adds no lifetime refs.
 * @throws std::invalid_argument If a table or selector is incomplete.
 * @throws std::out_of_range If the selector returns an unknown runtime index.
 */
MuonServiceExecution RouteMuonServiceExecutions(std::vector<MuonServiceExecution> runtimes,
    std::function<size_t(const std::string&)> select);
