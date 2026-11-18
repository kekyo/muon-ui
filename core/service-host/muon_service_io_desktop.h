/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "services/muon_service_io.h"
#include <yyjson.h>

/** @param policy Registry I/O object, or null when no grants were declared. @return Owner-dispatcher file executor and grants. */
MuonServiceIoSettings CreateDesktopMuonServiceIo(yyjson_val* policy);
