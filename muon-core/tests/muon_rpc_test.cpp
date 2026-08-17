/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "rpc/muon_rpc.h"
#include "rpc/muon_rpc_host.h"

#include "plugins/muon_plugin_policy.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static MuonRpcOwner CreateOwner(int browser_id,
                                const std::string& frame_id,
                                int context_id) {
  MuonRpcOwner owner;
  owner.browser_id = browser_id;
  owner.frame_id = frame_id;
  owner.context_id = context_id;
  return owner;
}

static bool RunOwnerIdentityTest() {
  const auto alpha = CreateOwner(1, "frame-a", 7);
  const auto same = CreateOwner(1, "frame-a", 7);
  const auto other_context = CreateOwner(1, "frame-a", 8);
  const auto invalid = CreateOwner(0, "", 0);

  return Expect(IsValidMuonRpcOwner(alpha), "valid RPC owner was rejected") &&
         Expect(!IsValidMuonRpcOwner(invalid),
                "invalid RPC owner was accepted") &&
         Expect(AreEqualMuonRpcOwners(alpha, same),
                "equal RPC owners did not compare equal") &&
         Expect(!AreEqualMuonRpcOwners(alpha, other_context),
                "different RPC owners compared equal") &&
         Expect(CreateMuonRpcOwnerKey(alpha) == "1:frame-a:7",
                "RPC owner key changed");
}

static bool RunBinaryStorageTest() {
  const auto storage = CreateMuonRpcOwnedBuffer(8);
  if (!Expect(storage != nullptr, "owned RPC buffer was not allocated") ||
      !Expect(storage->GetSize() == 8, "owned RPC buffer size changed")) {
    return false;
  }
  auto* bytes = static_cast<uint8_t*>(storage->GetData());
  for (auto index = size_t{0}; index < storage->GetSize(); ++index) {
    bytes[index] = static_cast<uint8_t>(index + 1);
  }

  MuonRpcBinary slice;
  slice.storage = storage;
  slice.offset = 2;
  slice.size = 4;
  const auto* slice_data = static_cast<const uint8_t*>(
      GetMuonRpcBinaryData(slice));
  MuonRpcBinary zero_length;
  zero_length.storage = storage;
  zero_length.offset = storage->GetSize();
  zero_length.size = 0;
  MuonRpcBinary invalid_range;
  invalid_range.storage = storage;
  invalid_range.offset = 7;
  invalid_range.size = 2;

  return Expect(IsValidMuonRpcBinary(slice), "valid RPC binary was rejected") &&
         Expect(slice_data != nullptr && slice_data[0] == 3 &&
                    slice_data[3] == 6,
                "RPC binary slice points at the wrong bytes") &&
         Expect(IsValidMuonRpcBinary(zero_length),
                "zero-length RPC binary was rejected") &&
         Expect(GetMuonRpcBinaryData(zero_length) == nullptr,
                "zero-length RPC binary exposed a data pointer") &&
         Expect(!IsValidMuonRpcBinary(invalid_range),
                "out-of-range RPC binary was accepted");
}

static bool RunTypedMessageTest() {
  const auto owner = CreateOwner(2, "frame-b", 11);
  MuonRpcCallRequest request;
  request.owner = owner;
  request.call_id = 13;
  request.kind = MuonRpcCallKind::Plugin;
  request.function_id = 17;
  request.capability.id = "files";
  request.capability.function_path = "muon.fs.readText";

  MuonRpcValue string_value;
  string_value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
  string_value.string_value = "alpha";
  request.arguments.push_back(string_value);

  MuonRpcValue function_value;
  function_value.type.type = MUON_TYPE_FUNCTION;
  function_value.type.function_return_type.push_back(
      CreateMuonPrimitiveType(MUON_TYPE_VOID));
  function_value.function.kind = MuonRpcFunctionKind::RendererSource;
  function_value.function.function_id = 19;
  function_value.function.renderer_context_id = owner.context_id;
  function_value.function.type = function_value.type;
  request.arguments.push_back(function_value);

  MuonRpcMessage message = request;
  const auto* decoded = std::get_if<MuonRpcCallRequest>(&message);
  return Expect(decoded != nullptr, "typed RPC call message was lost") &&
         Expect(decoded->owner.context_id == 11,
                "typed RPC call owner changed") &&
         Expect(decoded->arguments.size() == 2,
                "typed RPC call arguments changed") &&
         Expect(decoded->arguments[0].string_value == "alpha",
                "typed RPC string argument changed") &&
         Expect(decoded->arguments[1].function.function_id == 19,
                "typed RPC function reference changed");
}

static bool RunClientStateTest() {
  const auto owner = CreateOwner(3, "frame-c", 21);
  const auto other_owner = CreateOwner(3, "frame-c", 22);
  auto state = MuonRpcClientState(2);
  auto error_message = std::string{};
  auto first_id = uint32_t{0};
  auto second_id = uint32_t{0};
  const auto string_type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
  const auto bool_type = CreateMuonPrimitiveType(MUON_TYPE_BOOL);

  if (!Expect(state.BeginCall(owner, string_type, &first_id, &error_message),
              "first RPC call was rejected") ||
      !Expect(state.BeginCall(owner, bool_type, &second_id, &error_message),
              "second RPC call was rejected") ||
      !Expect(first_id == 1 && second_id == 2,
              "RPC call ids were not monotonic") ||
      !Expect(state.GetPendingCallCount() == 2,
              "RPC pending call count changed")) {
    return false;
  }

  auto exhausted_id = uint32_t{0};
  if (!Expect(!state.BeginCall(owner, bool_type, &exhausted_id,
                              &error_message),
              "exhausted RPC call id was allocated") ||
      !Expect(error_message == "muon call ids are exhausted",
              "RPC call id exhaustion error changed")) {
    return false;
  }

  MuonRpcPendingCall pending;
  if (!Expect(state.CompleteCall(other_owner, first_id, &pending) ==
                  MuonRpcCallCompletionStatus::OwnerMismatch,
              "cross-owner RPC result was accepted") ||
      !Expect(state.CompleteCall(owner, first_id, &pending) ==
                  MuonRpcCallCompletionStatus::Completed,
              "RPC result was not completed") ||
      !Expect(pending.call_id == first_id &&
                  pending.return_type.type == MUON_TYPE_STRING,
              "completed RPC call metadata changed") ||
      !Expect(state.CompleteCall(owner, first_id, &pending) ==
                  MuonRpcCallCompletionStatus::Duplicate,
              "duplicate RPC result was not detected") ||
      !Expect(state.CompleteCall(owner, 99, &pending) ==
                  MuonRpcCallCompletionStatus::UnknownCall,
              "unknown RPC result was not detected")) {
    return false;
  }

  const auto released = state.ReleaseOwner(owner);
  return Expect(released.size() == 1 && released[0].call_id == second_id,
                "RPC owner release did not return its pending call") &&
         Expect(state.GetPendingCallCount() == 0,
                "RPC owner release left pending calls");
}

struct FakeRpcHostTransport {
  std::vector<MuonRpcCallRequest> plugin_calls;
  std::vector<MuonRpcCallRequest> platform_calls;
  std::vector<MuonRpcCallResult> results;
  std::vector<MuonRpcPluginProxyRelease> proxy_releases;
  std::vector<MuonRpcContextReleased> context_releases;
  std::vector<MuonRpcHostCompletion> pending_completions;
};

static MuonRpcHostServices CreateFakeRpcHostServices(
    FakeRpcHostTransport* transport) {
  MuonRpcHostServices services;
  services.invoke_plugin =
      [transport](const MuonRpcCallRequest& request,
                  MuonRpcHostCompletion completion) {
        transport->plugin_calls.push_back(request);
        transport->pending_completions.push_back(std::move(completion));
      };
  services.invoke_platform =
      [transport](const MuonRpcCallRequest& request,
                  MuonRpcHostCompletion completion) {
        transport->platform_calls.push_back(request);
        transport->pending_completions.push_back(std::move(completion));
      };
  services.release_plugin_proxy =
      [transport](const MuonRpcPluginProxyRelease& release) {
        transport->proxy_releases.push_back(release);
      };
  services.release_context =
      [transport](const MuonRpcContextReleased& release) {
        transport->context_releases.push_back(release);
      };
  services.send_result = [transport](const MuonRpcCallResult& result) {
    transport->results.push_back(result);
  };
  return services;
}

static std::shared_ptr<MuonPluginPolicy> CreateTestPluginPolicy(
    const std::vector<std::string>& patterns) {
  auto policy = std::shared_ptr<MuonPluginPolicy>{};
  auto error_message = std::string{};
  if (!CreateMuonPluginPolicy(patterns, &policy, &error_message)) {
    std::cerr << "could not create test plugin policy: " << error_message
              << "\n";
    return nullptr;
  }
  return policy;
}

static std::shared_ptr<MuonRpcHost> CreateTestRpcHost(
    MuonRpcHostMode mode,
    FakeRpcHostTransport* transport,
    const std::map<std::string, std::shared_ptr<MuonPluginPolicy>>& policies) {
  std::vector<MuonRpcFunctionRoute> routes;
  routes.push_back({7, "muon.files.readText", MuonRpcRouteKind::Plugin});
  routes.push_back({8, "muon.browser.close", MuonRpcRouteKind::Platform});
  auto host = std::shared_ptr<MuonRpcHost>{};
  auto error_message = std::string{};
  if (!CreateMuonRpcHost(mode, routes, policies,
                         CreateFakeRpcHostServices(transport), &host,
                         &error_message)) {
    std::cerr << "could not create test RPC host: " << error_message << "\n";
    return nullptr;
  }
  return host;
}

static MuonRpcCallRequest CreateHostCall(const MuonRpcOwner& owner,
                                         uint32_t call_id,
                                         uint32_t function_id) {
  MuonRpcCallRequest request;
  request.owner = owner;
  request.call_id = call_id;
  request.function_id = function_id;
  return request;
}

static bool RunRpcHostRoutingTest() {
  const auto owner = CreateOwner(4, "frame-d", 31);
  FakeRpcHostTransport transport;
  const auto host = CreateTestRpcHost(MuonRpcHostMode::Simple, &transport, {});
  if (!Expect(host != nullptr, "simple RPC host was not created")) {
    return false;
  }

  auto plugin_call = CreateHostCall(owner, 1, 7);
  plugin_call.capability.id = "ignored";
  plugin_call.capability.function_path = "ignored";
  const auto platform_call = CreateHostCall(owner, 2, 8);
  auto proxy_call = CreateHostCall(owner, 3, 77);
  proxy_call.kind = MuonRpcCallKind::PluginProxy;
  proxy_call.proxy_lease_token = "lease-77";
  host->DispatchCall(plugin_call);
  host->DispatchCall(platform_call);
  host->DispatchCall(proxy_call);

  if (!Expect(transport.plugin_calls.size() == 2,
              "RPC host did not route plugin and proxy calls") ||
      !Expect(transport.platform_calls.size() == 1,
              "RPC host did not route the platform call") ||
      !Expect(transport.pending_completions.size() == 3,
              "RPC host did not retain routed calls") ||
      !Expect(host->GetPendingCallCount() == 3,
              "RPC host pending count changed")) {
    return false;
  }

  MuonRpcCallResult successful_result;
  successful_result.success = true;
  successful_result.value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
  successful_result.value.string_value = "done";
  transport.pending_completions[0](successful_result);
  return Expect(transport.results.size() == 1,
                "RPC host did not forward a completion") &&
         Expect(AreEqualMuonRpcOwners(transport.results[0].owner, owner) &&
                    transport.results[0].call_id == 1,
                "RPC host did not restore trusted result identity") &&
         Expect(transport.results[0].value.string_value == "done",
                "RPC host changed the successful result") &&
         Expect(host->GetPendingCallCount() == 2,
                "RPC host did not retire the completed call");
}

static bool RunRpcHostCapabilityTest() {
  const auto owner = CreateOwner(5, "frame-e", 41);
  FakeRpcHostTransport transport;
  const auto policy = CreateTestPluginPolicy({"muon.files.*"});
  const auto other_policy = CreateTestPluginPolicy({"muon.other.*"});
  if (!Expect(policy != nullptr && other_policy != nullptr,
              "RPC capability policies were not created")) {
    return false;
  }
  const auto host = CreateTestRpcHost(
      MuonRpcHostMode::Validate, &transport,
      {{"files", policy}, {"other", other_policy}});
  if (!Expect(host != nullptr, "validate RPC host was not created")) {
    return false;
  }

  host->DispatchCall(CreateHostCall(owner, 1, 7));
  auto unknown = CreateHostCall(owner, 2, 7);
  unknown.capability = {"unknown", "muon.files.readText"};
  host->DispatchCall(unknown);
  auto denied = CreateHostCall(owner, 3, 7);
  denied.capability = {"other", "muon.files.readText"};
  host->DispatchCall(denied);
  auto mismatch = CreateHostCall(owner, 4, 7);
  mismatch.capability = {"files", "muon.files.writeText"};
  host->DispatchCall(mismatch);
  auto accepted = CreateHostCall(owner, 5, 7);
  accepted.capability = {"files", "muon.files.readText"};
  host->DispatchCall(accepted);

  return Expect(transport.results.size() == 4,
                "RPC host capability rejections changed") &&
         Expect(transport.results[0].error_message ==
                    "muon plugin capability is required for validate mode",
                "missing RPC capability error changed") &&
         Expect(transport.results[1].error_message ==
                    "muon plugin capability is unknown: unknown",
                "unknown RPC capability error changed") &&
         Expect(transport.results[2].error_message ==
                    "muon plugin capability is not allowed for "
                    "muon.files.readText",
                "denied RPC capability error changed") &&
         Expect(transport.results[3].error_message ==
                    "muon plugin capability function path does not match the "
                    "requested function",
                "mismatched RPC capability path error changed") &&
         Expect(transport.plugin_calls.size() == 1 &&
                    transport.plugin_calls[0].call_id == 5,
                "valid RPC capability was not routed");
}

static bool RunRpcHostLifecycleTest() {
  const auto owner = CreateOwner(6, "frame-f", 51);
  FakeRpcHostTransport transport;
  const auto host = CreateTestRpcHost(MuonRpcHostMode::Simple, &transport, {});
  if (!Expect(host != nullptr, "lifecycle RPC host was not created")) {
    return false;
  }

  const auto call = CreateHostCall(owner, 1, 7);
  host->DispatchCall(call);
  host->DispatchCall(call);
  if (!Expect(transport.results.size() == 1 &&
                  transport.results[0].error_message ==
                      "Duplicate muon plugin call",
              "duplicate RPC call was not rejected once") ||
      !Expect(host->GetPendingCallCount() == 0,
              "duplicate RPC call left the original pending")) {
    return false;
  }
  MuonRpcCallResult late_duplicate_result;
  late_duplicate_result.success = true;
  transport.pending_completions[0](late_duplicate_result);
  if (!Expect(transport.results.size() == 1,
              "retired duplicate RPC call produced a late result")) {
    return false;
  }

  auto second_call = CreateHostCall(owner, 2, 7);
  host->DispatchCall(second_call);
  MuonRpcPluginProxyRelease proxy_release;
  proxy_release.owner = owner;
  proxy_release.proxy_id = 17;
  proxy_release.lease_token = "lease-17";
  host->HandleMessage(proxy_release);
  MuonRpcContextReleased context_release;
  context_release.owner = owner;
  host->HandleMessage(context_release);
  if (!Expect(transport.proxy_releases.size() == 1,
              "RPC proxy release was not routed") ||
      !Expect(transport.context_releases.size() == 1,
              "RPC context release was not routed") ||
      !Expect(host->GetPendingCallCount() == 0,
              "RPC context release left pending calls")) {
    return false;
  }
  MuonRpcCallResult late_context_result;
  late_context_result.success = true;
  transport.pending_completions[1](late_context_result);
  return Expect(transport.results.size() == 1,
                "released RPC context produced a late result");
}

int main() {
  return RunOwnerIdentityTest() && RunBinaryStorageTest() &&
                 RunTypedMessageTest() && RunClientStateTest() &&
                 RunRpcHostRoutingTest() && RunRpcHostCapabilityTest() &&
                 RunRpcHostLifecycleTest()
             ? 0
             : 1;
}
