/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_android_dispatcher.h"
#include <stdexcept>
#include <vector>
#include <thread>

struct MuonAndroidDispatcherOwner {
  const std::thread::id thread = std::this_thread::get_id();
  std::unique_ptr<cardio::dispatcher_host_android_auto> host =
      std::make_unique<cardio::dispatcher_host_android_auto>();
  std::vector<std::function<void()>> cleanup;
  ~MuonAndroidDispatcherOwner() {
    host.reset();
    for (const auto& dispose : cleanup) dispose();
  }
};

static std::weak_ptr<MuonAndroidDispatcherOwner> current;

std::shared_ptr<cardio::dispatcher_host_android_auto> AcquireMuonAndroidDispatcher() {
  auto owner = current.lock();
  if (owner) {
    if (owner->thread != std::this_thread::get_id())
      throw std::runtime_error("Android service streams require their owner Looper");
  } else {
    owner = std::make_shared<MuonAndroidDispatcherOwner>();
    current = owner;
  }
  return {owner, owner->host.get()};
}

void RetainMuonAndroidDispatcherCleanup(
    const std::shared_ptr<cardio::dispatcher_host_android_auto>& host,
    std::function<void()> cleanup) {
  const auto owner = current.lock();
  if (!owner || owner->host.get() != host.get() || owner->thread != std::this_thread::get_id())
    throw std::logic_error("Unknown Android dispatcher lease");
  owner->cleanup.push_back(std::move(cleanup));
}
