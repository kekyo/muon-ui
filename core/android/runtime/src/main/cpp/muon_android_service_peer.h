/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include "services/muon_service_broker.h"
#include <functional>
#include <memory>

/** One non-owning RPC channel authenticated by its existing Binder page owner. */
struct MuonAndroidServicePeer {
  /** Closes the stream and reclaims that owner exactly once; I/O drains asynchronously. */
  std::function<void()> close;
};

/**
 * Starts a bounded JSON RPC stream using the process's native service broker.
 * @param descriptor Owned socket received through authenticated Binder bootstrap.
 * @param owner Existing Binder page identity; cannot be selected in an RPC payload.
 * @param broker Process-owned broker, outliving every peer.
 * @param post Always-deferred owner Looper queue used for completed coroutine disposal.
 * @param disconnected Removes the corresponding Java Binder owner, idempotently.
 * @returns A channel control that must be closed on Binder death or explicit page release.
 */
std::shared_ptr<MuonAndroidServicePeer> OpenMuonAndroidServicePeer(
    int descriptor, std::string owner, MuonServiceBroker& broker,
    std::function<void(std::function<void()>)> post, std::function<void()> disconnected);
