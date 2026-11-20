/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include "services/muon_service_client.h"

/**
 * Connects one page to an existing local service host on the owner dispatcher.
 * @param endpoint Absolute Unix socket or local Windows named pipe endpoint.
 * @param cancellation Cancels connection establishment and authentication.
 * @returns Authenticated transport after consuming the host's ready frame.
 * @throws std::system_error Native connection failures retain their OS error code.
 * @remarks The ready frame confirms that the host has rearmed its listener.
 *     Does not start a host or retry unavailable/busy endpoints. The caller must
 *     close the returned transport and await its pending I/O before disposal.
 */
cardio::promise<MuonServiceTransport> ConnectMuonServiceTransport(
    std::string endpoint, cardio::cancellation cancellation);
