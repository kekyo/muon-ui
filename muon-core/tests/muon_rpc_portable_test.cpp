/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "rpc/muon_rpc_host.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

int main() {
  auto pending_completion = MuonRpcHostCompletion{};
  auto received_results = std::vector<MuonRpcCallResult>{};
  MuonRpcHostServices services;
  services.invoke_plugin =
      [](const MuonRpcCallRequest&, MuonRpcHostCompletion) {};
  services.invoke_platform =
      [&pending_completion](const MuonRpcCallRequest&,
                            MuonRpcHostCompletion completion) {
        pending_completion = std::move(completion);
      };
  services.release_plugin_proxy = [](const MuonRpcPluginProxyRelease&) {};
  services.release_context = [](const MuonRpcContextReleased&) {};
  services.send_result = [&received_results](const MuonRpcCallResult& result) {
    received_results.push_back(result);
  };

  auto host = std::shared_ptr<MuonRpcHost>{};
  auto error_message = std::string{};
  const auto routes = std::vector<MuonRpcFunctionRoute>{
      {0, "muon.environments.getConfigValues", MuonRpcRouteKind::Platform},
  };
  if (!Expect(CreateMuonRpcHost(MuonRpcHostMode::Simple, routes, {},
                                std::move(services), &host, &error_message),
              "portable RPC host was not created: " + error_message)) {
    return 1;
  }

  MuonRpcCallRequest request;
  request.owner = {1, "android-main-frame", 1};
  request.call_id = 1;
  request.function_id = 0;
  if (!Expect(host->DispatchCall(request),
              "portable RPC host rejected a platform call") ||
      !Expect(static_cast<bool>(pending_completion),
              "portable RPC host did not route the platform call")) {
    return 1;
  }

  MuonRpcCallResult completion;
  completion.success = true;
  completion.value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
  completion.value.string_value = R"({"platform":"android"})";
  pending_completion(completion);

  return Expect(received_results.size() == 1,
                "portable RPC host did not return one result") &&
                 Expect(received_results[0].owner.browser_id == 1 &&
                            received_results[0].call_id == 1 &&
                            received_results[0].success,
                        "portable RPC host changed the result identity") &&
                 Expect(received_results[0].value.string_value ==
                            R"({"platform":"android"})",
                        "portable RPC host changed the result payload")
             ? 0
             : 1;
}
