/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include "services/muon_service_client.h"

/**
 * Adopts one authenticated Binder-transferred stream descriptor.
 * @param descriptor Owned connected socket; ownership transfers even on failure.
 * @returns A nonblocking cardio stream retaining the fd through pending I/O cleanup.
 * @throws std::system_error If descriptor configuration fails.
 * @remarks The caller authenticates its Binder owner before transferring the fd.
 */
MuonServiceTransport AdoptMuonAndroidServiceTransport(int descriptor);
