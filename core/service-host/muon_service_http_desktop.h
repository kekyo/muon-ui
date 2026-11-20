/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "services/muon_service_http.h"

/** Sends one validated request using the desktop operating system's TLS trust configuration. */
MuonServiceHttpResult RunDesktopMuonServiceHttp(const MuonServiceHttpHop& hop, MuonServiceIoCancellation& cancel);
