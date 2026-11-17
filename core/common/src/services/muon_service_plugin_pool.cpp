/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_plugin_pool_internal.h"
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

// Plugin ABI helper function pointers are process globals. No replacement may
// initialize until the previous runtime's destructor and dlclose have returned.
static MuonServicePluginPoolImpl* active_pool = nullptr;

void MuonServicePluginPoolImpl::Post(std::function<void()> task) {
  ++posted;
  const auto self = shared_from_this();
  if (!services.post_owner_task([self, task = std::move(task)] {
        --self->posted;
        task();
      })) {
    --posted;
    throw std::runtime_error("Service plugin owner dispatcher is unavailable");
  }
}

bool MuonServicePluginPoolImpl::Receive(const MuonRpcMessage& message, std::string* error) {
  (void)error;
  const auto owner = std::visit([](const auto& value) { return value.owner; }, message);
  // Native callbacks never enter JavaScript on the native caller's stack.
  Post([self = shared_from_this(), owner, message] {
    const auto found = self->owners.find(CreateMuonRpcOwnerKey(owner));
    if (found == self->owners.end()) return;
    const auto lease = found->second;
    if (!lease->closing && lease->receive) lease->receive(message);
  });
  return true;
}

void MuonServicePluginPoolImpl::Acquire(const std::string& id, uint64_t generation,
    std::function<void(std::shared_ptr<MuonServicePluginLease>, const std::string&)> complete) {
  if (stopping) {
    acquisitions.push_back([self = shared_from_this(), id, generation, complete = std::move(complete)] {
      self->Acquire(id, generation, complete);
    });
    return;
  }
  const auto policy = policies.find(id);
  if (policy == policies.end() || !generation) {
    complete(nullptr, "Unknown service or invalid generation");
    return;
  }
  for (const auto& [key, lease] : owners) {
    (void)key;
    if (lease->id == id && lease->generation == generation) {
      complete(nullptr, "Service plugin generation already has an owner");
      return;
    }
  }
  if (next_owner > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    complete(nullptr, "Service plugin owner identifiers exhausted");
    return;
  }
  if (!runtime) {
    if (active_pool) {
      complete(nullptr, "Another service plugin pool owns the process runtime");
      return;
    }
    auto platform = services;
    const auto weak = weak_from_this();
    platform.platform_adapter.reset();
    platform.post_owner_task = [weak](std::function<void()> task) {
      const auto self = weak.lock();
      if (!self) return false;
      ++self->runtime_posts;
      self->Post([self, task = std::move(task)] {
        task();
        --self->runtime_posts;
        // The raw runtime callback and its captures must return before unload.
        self->Post([self] { self->Advance(); });
      });
      return true;
    };
    platform.is_owner_available = [weak](const MuonRpcOwner& owner) {
      const auto self = weak.lock();
      if (!self) return false;
      const auto lock = std::lock_guard(self->owners_mutex);
      const auto found = self->owners.find(CreateMuonRpcOwnerKey(owner));
      return found != self->owners.end() && !found->second->closing;
    };
    platform.send_message = [weak](const MuonRpcMessage& message, std::string* error) {
      const auto self = weak.lock();
      return self && self->Receive(message, error);
    };
    active_pool = this;
    try {
      runtime = std::make_unique<MuonPluginRuntime>(directory, plugins, std::move(platform));
    } catch (...) {
      active_pool = nullptr;
      throw;
    }
    if (!runtime->IsReady()) {
      const auto error = runtime->GetStartupError();
      stopping = true;
      runtime->Stop([self = shared_from_this()] {
        self->Post([self] { self->stopped = true; self->Advance(); });
      });
      complete(nullptr, error);
      return;
    }
  }
  auto lease = std::make_shared<MuonServicePluginLeaseImpl>();
  lease->pool = shared_from_this();
  lease->id = id;
  lease->generation = generation;
  lease->owner = {1, id + ":" + std::to_string(generation), static_cast<int>(next_owner++)};
  lease->policy = policy->second;
  {
    const auto lock = std::lock_guard(owners_mutex);
    owners.emplace(CreateMuonRpcOwnerKey(lease->owner), lease);
  }
  complete(std::shared_ptr<MuonServicePluginLease>(new MuonServicePluginLease(lease)), {});
}

void MuonServicePluginPoolImpl::Close(const std::shared_ptr<MuonServicePluginLeaseImpl>& lease,
                                    std::function<void()> complete) {
  if (lease->closed) { Post(std::move(complete)); return; }
  lease->completions.push_back(std::move(complete));
  if (lease->closing) return;
  lease->closing = true;
  if (lease->detach) lease->detach();
  lease->detach = {};
  lease->receive = {};
  runtime->ReleaseFunctionContext({lease->owner});
  runtime->DrainFunctionContext(lease->owner, [self = shared_from_this(), lease] {
    lease->drained = true;
    self->Post([self] { self->Advance(); });
  });
  bool any_active = false;
  for (const auto& [key, owner] : owners) { (void)key; any_active |= !owner->closing; }
  if (!any_active && !stopping) {
    stopping = true;
    // Stop can complete/cancel outstanding plugin work; waiting for calls first
    // would deadlock a plugin whose stop operation is its cancellation source.
    runtime->Stop([self = shared_from_this()] {
      self->Post([self] { self->stopped = true; self->Advance(); });
    });
  }
  Post([self = shared_from_this()] { self->Advance(); });
}

void MuonServicePluginPoolImpl::Invoke(const std::shared_ptr<MuonServicePluginLeaseImpl>& lease,
    MuonRpcCallRequest request, MuonPluginRuntime::Completion complete) {
  if (lease->closing || !runtime) throw std::runtime_error("Service plugin owner is closed");
  ++lease->calls;
  request.owner = lease->owner;
  runtime->Invoke(request, [self = shared_from_this(), lease, complete = std::move(complete)]
      (const MuonRpcCallResult& result) {
    // MuonPluginRuntime's traffic call frame still owns plugin borrows until
    // this callback returns. Never destroy the runtime or enter JS here.
    self->Post([self, lease, complete, result] {
      if (lease->closing && result.success && result.value.type.type == MUON_TYPE_FUNCTION &&
          !result.value.is_null && result.value.function.kind == MuonRpcFunctionKind::PluginProxy) {
        self->runtime->ReleasePluginFunctionProxy({lease->owner,
            result.value.function.proxy_id, result.value.function.lease_token});
      }
      complete(result);
      --lease->calls;
      self->Advance();
    });
  });
}

void MuonServicePluginPoolImpl::Advance() {
  if (stopping) {
    if (!stopped || runtime_posts) return;
    for (const auto& [key, owner] : owners) { (void)key; if (owner->calls || !owner->drained) return; }
    runtime.reset();
    assert(active_pool == this);
    active_pool = nullptr;
    stopping = false;
    stopped = false;
  }
  std::vector<std::function<void()>> completed;
  for (auto it = owners.begin(); it != owners.end();) {
    const auto& owner = it->second;
    if (!owner->closing || owner->calls || !owner->drained) { ++it; continue; }
    owner->closed = true;
    for (auto& callback : owner->completions) completed.push_back(std::move(callback));
    owner->completions.clear();
    const auto lock = std::lock_guard(owners_mutex);
    it = owners.erase(it);
  }
  auto pending = std::move(acquisitions);
  acquisitions.clear();
  for (auto& callback : completed) callback();
  for (auto& acquire : pending) Post(std::move(acquire));
}

MuonServicePluginPool::MuonServicePluginPool(std::filesystem::path directory,
    std::vector<MuonPluginRuntimeLoadEntry> plugins,
    std::vector<MuonServicePluginDefinition> definitions, MuonPluginRuntimeServices services)
    : impl_(std::make_shared<MuonServicePluginPoolImpl>()) {
  if (!services.dispatcher || !services.post_owner_task) throw std::invalid_argument("Plugin owner dispatcher is required");
  impl_->directory = std::move(directory); impl_->plugins = std::move(plugins); impl_->services = std::move(services);
  for (const auto& definition : definitions) {
    std::shared_ptr<MuonPluginPolicy> policy;
    std::string error;
    if (definition.id.empty() || !CreateMuonPluginPolicy(definition.allow, &policy, &error) ||
        !impl_->policies.emplace(definition.id, policy).second) {
      throw std::invalid_argument("Invalid service plugin definition: " + definition.id + " " + error);
    }
  }
}
MuonServicePluginPool::~MuonServicePluginPool() { assert(IsIdle()); }
void MuonServicePluginPool::Acquire(const std::string& id, uint64_t generation,
    std::function<void(std::shared_ptr<MuonServicePluginLease>, const std::string&)> complete) {
  if (!complete) throw std::invalid_argument("Plugin lease completion is required");
  impl_->Post([impl = impl_, id, generation, complete = std::move(complete)] { impl->Acquire(id, generation, complete); });
}
bool MuonServicePluginPool::IsIdle() const { return !impl_->runtime && impl_->owners.empty() && impl_->acquisitions.empty() && !impl_->posted; }
MuonServicePluginLease::MuonServicePluginLease(std::shared_ptr<MuonServicePluginLeaseImpl> impl) : impl_(std::move(impl)) {}
MuonServicePluginLease::~MuonServicePluginLease() { assert(impl_->closed); }
void MuonServicePluginLease::Close(std::function<void()> complete) {
  if (!complete) throw std::invalid_argument("Plugin lease close completion is required");
  impl_->pool->Close(impl_, std::move(complete));
}
