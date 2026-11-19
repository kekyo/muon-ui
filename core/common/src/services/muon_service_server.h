/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "services/muon_service_io.h"

/** @param policy Normalized service grants. @return An independent loopback server on this dispatcher, or null when denied. */
std::shared_ptr<MuonServiceServer> CreateMuonServiceListeners(const std::string& policy);
