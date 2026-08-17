/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "rpc/muon_rpc_host.h"

#include <limits>
#include <tuple>
#include <utility>

struct MuonRpcHostPendingKey {
  int browser_id = 0;
  std::string frame_id;
  int context_id = 0;
  uint32_t call_id = 0;
};

static bool operator<(const MuonRpcHostPendingKey& first,
                      const MuonRpcHostPendingKey& second) {
  return std::tie(first.browser_id, first.frame_id, first.context_id,
                  first.call_id) <
         std::tie(second.browser_id, second.frame_id, second.context_id,
                  second.call_id);
}

struct MuonRpcHostPendingCall {
  MuonRpcOwner owner;
  uint32_t call_id = 0;
  uint64_t generation = 0;
};

struct MuonRpcHostImpl {
  MuonRpcHostMode mode = MuonRpcHostMode::Validate;
  std::map<uint32_t, MuonRpcFunctionRoute> routes;
  std::map<std::string, std::shared_ptr<MuonPluginPolicy>>
      capability_policies;
  MuonRpcHostServices services;
  uint64_t next_generation = 1;
  std::map<MuonRpcHostPendingKey, MuonRpcHostPendingCall> pending_calls;
};

static MuonRpcHostPendingKey CreatePendingKey(
    const MuonRpcOwner& owner,
    uint32_t call_id) {
  MuonRpcHostPendingKey key;
  key.browser_id = owner.browser_id;
  key.frame_id = owner.frame_id;
  key.context_id = owner.context_id;
  key.call_id = call_id;
  return key;
}

static void SendRejectedCall(const std::shared_ptr<MuonRpcHostImpl>& impl,
                             const MuonRpcCallRequest& request,
                             const std::string& error_message) {
  MuonRpcCallResult result;
  result.owner = request.owner;
  result.call_id = request.call_id;
  result.success = false;
  result.error_message = error_message;
  impl->services.send_result(result);
}

static bool ValidateCapability(
    const std::shared_ptr<MuonRpcHostImpl>& impl,
    const MuonRpcCallRequest& request,
    const MuonRpcFunctionRoute& route,
    std::string* error_message) {
  if (impl->mode != MuonRpcHostMode::Validate) {
    return true;
  }
  if (request.capability.id.empty() ||
      request.capability.function_path.empty()) {
    *error_message = "muon plugin capability is required for validate mode";
    return false;
  }
  const auto policy_iterator =
      impl->capability_policies.find(request.capability.id);
  if (policy_iterator == impl->capability_policies.end() ||
      !policy_iterator->second) {
    *error_message =
        "muon plugin capability is unknown: " + request.capability.id;
    return false;
  }
  if (!policy_iterator->second->IsAllowedFunctionPath(
          request.capability.function_path)) {
    *error_message = "muon plugin capability is not allowed for " +
                     request.capability.function_path;
    return false;
  }
  if (route.public_path != request.capability.function_path) {
    *error_message =
        "muon plugin capability function path does not match the requested "
        "function";
    return false;
  }
  return true;
}

static void CompleteCall(const std::weak_ptr<MuonRpcHostImpl>& weak_impl,
                         const MuonRpcHostPendingKey& key,
                         uint64_t generation,
                         const MuonRpcCallResult& untrusted_result) {
  const auto impl = weak_impl.lock();
  if (!impl) {
    return;
  }
  const auto pending_iterator = impl->pending_calls.find(key);
  if (pending_iterator == impl->pending_calls.end() ||
      pending_iterator->second.generation != generation) {
    return;
  }
  const auto pending_call = pending_iterator->second;
  impl->pending_calls.erase(pending_iterator);

  auto result = untrusted_result;
  result.owner = pending_call.owner;
  result.call_id = pending_call.call_id;
  impl->services.send_result(result);
}

static bool IsSameOwner(const MuonRpcHostPendingKey& key,
                        const MuonRpcOwner& owner) {
  return key.browser_id == owner.browser_id &&
         key.frame_id == owner.frame_id &&
         key.context_id == owner.context_id;
}

MuonRpcHost::MuonRpcHost(std::shared_ptr<MuonRpcHostImpl> impl)
    : impl_(std::move(impl)) {}

MuonRpcHost::~MuonRpcHost() = default;

bool MuonRpcHost::DispatchCall(const MuonRpcCallRequest& request) {
  if (!impl_ || !IsValidMuonRpcOwner(request.owner)) {
    return false;
  }
  if (request.call_id == 0 || request.function_id == 0) {
    SendRejectedCall(impl_, request, "Invalid muon plugin call");
    return true;
  }

  auto route_kind = MuonRpcRouteKind::Plugin;
  if (request.kind == MuonRpcCallKind::PluginProxy) {
    if (request.proxy_lease_token.empty()) {
      SendRejectedCall(impl_, request, "Invalid muon function proxy call");
      return true;
    }
  } else {
    const auto route_iterator = impl_->routes.find(request.function_id);
    if (route_iterator == impl_->routes.end()) {
      SendRejectedCall(impl_, request, "Unknown muon plugin function");
      return true;
    }
    auto error_message = std::string{};
    if (!ValidateCapability(impl_, request, route_iterator->second,
                            &error_message)) {
      SendRejectedCall(impl_, request, error_message);
      return true;
    }
    route_kind = route_iterator->second.kind;
  }

  const auto key = CreatePendingKey(request.owner, request.call_id);
  const auto pending_iterator = impl_->pending_calls.find(key);
  if (pending_iterator != impl_->pending_calls.end()) {
    impl_->pending_calls.erase(pending_iterator);
    SendRejectedCall(impl_, request, "Duplicate muon plugin call");
    return true;
  }
  if (impl_->next_generation == 0 ||
      impl_->next_generation == std::numeric_limits<uint64_t>::max()) {
    SendRejectedCall(impl_, request, "Muon RPC call generations are exhausted");
    return true;
  }

  MuonRpcHostPendingCall pending_call;
  pending_call.owner = request.owner;
  pending_call.call_id = request.call_id;
  pending_call.generation = impl_->next_generation;
  impl_->next_generation += 1;
  impl_->pending_calls.emplace(key, pending_call);

  const auto completion =
      [weak_impl = std::weak_ptr<MuonRpcHostImpl>(impl_), key,
       generation = pending_call.generation](const MuonRpcCallResult& result) {
        CompleteCall(weak_impl, key, generation, result);
      };
  if (route_kind == MuonRpcRouteKind::Platform) {
    impl_->services.invoke_platform(request, completion);
  } else {
    impl_->services.invoke_plugin(request, completion);
  }
  return true;
}

bool MuonRpcHost::HandleMessage(const MuonRpcMessage& message) {
  if (const auto* call = std::get_if<MuonRpcCallRequest>(&message)) {
    return DispatchCall(*call);
  }
  if (!impl_) {
    return false;
  }
  if (const auto* release =
          std::get_if<MuonRpcPluginProxyRelease>(&message)) {
    if (!IsValidMuonRpcOwner(release->owner) || release->proxy_id == 0 ||
        release->lease_token.empty()) {
      return false;
    }
    impl_->services.release_plugin_proxy(*release);
    return true;
  }
  if (const auto* release = std::get_if<MuonRpcContextReleased>(&message)) {
    if (!IsValidMuonRpcOwner(release->owner)) {
      return false;
    }
    auto iterator = impl_->pending_calls.begin();
    while (iterator != impl_->pending_calls.end()) {
      if (IsSameOwner(iterator->first, release->owner)) {
        iterator = impl_->pending_calls.erase(iterator);
      } else {
        ++iterator;
      }
    }
    impl_->services.release_context(*release);
    return true;
  }
  return false;
}

size_t MuonRpcHost::GetPendingCallCount() const {
  return impl_ ? impl_->pending_calls.size() : 0;
}

bool CreateMuonRpcHost(
    MuonRpcHostMode mode,
    const std::vector<MuonRpcFunctionRoute>& routes,
    const std::map<std::string, std::shared_ptr<MuonPluginPolicy>>&
        capability_policies,
    MuonRpcHostServices services,
    std::shared_ptr<MuonRpcHost>* host,
    std::string* error_message) {
  if (host == nullptr || error_message == nullptr) {
    return false;
  }
  host->reset();
  error_message->clear();
  if (mode != MuonRpcHostMode::Simple && mode != MuonRpcHostMode::Validate) {
    *error_message = "Invalid muon RPC host mode";
    return false;
  }
  if (!services.invoke_plugin || !services.invoke_platform ||
      !services.release_plugin_proxy || !services.release_context ||
      !services.send_result) {
    *error_message = "Muon RPC host services are incomplete";
    return false;
  }

  auto impl = std::make_shared<MuonRpcHostImpl>();
  impl->mode = mode;
  impl->capability_policies = capability_policies;
  impl->services = std::move(services);
  for (const auto& route : routes) {
    if (route.function_id == 0 || route.public_path.empty()) {
      *error_message = "Invalid muon RPC function route";
      return false;
    }
    if (route.kind != MuonRpcRouteKind::Plugin &&
        route.kind != MuonRpcRouteKind::Platform) {
      *error_message = "Invalid muon RPC function route kind";
      return false;
    }
    if (!impl->routes.emplace(route.function_id, route).second) {
      *error_message = "Duplicate muon RPC function route id";
      return false;
    }
  }

  *host = std::shared_ptr<MuonRpcHost>(new MuonRpcHost(std::move(impl)));
  return true;
}
