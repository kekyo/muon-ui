/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_rpc.h"
#include <iostream>
#include <stdexcept>

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    for (const auto* name : {"acquire", "release", "getState", "setAutostart", "watchState"}) {
      auto function = MuonFunctionMetadata{};
      function.id = 7;
      function.plugin_namespace = "muon.services";
      function.public_name = name;
      auto request = MuonRpcCallRequest{};
      request.function_id = 7;
      request.owner = {3, "frame-a", 1};
      request.arguments.resize(2);
      request.arguments[0].string_value = "{\"owner\":\"another-page\"}";
      request.arguments[1].string_value = "another-page";
      BindMuonServiceOwner(request, {function});
      Expect(request.arguments[1].string_value == CreateMuonRpcOwnerKey(request.owner),
             "A service call trusted its renderer-supplied owner");
      const auto first = request.arguments[1].string_value;
      request.owner = {4, "frame-b", 1};
      BindMuonServiceOwner(request, {function});
      Expect(first != request.arguments[1].string_value, "Two renderer pages shared a service owner");
      request.arguments[1].string_value = "application-data";
      function.plugin_namespace = "example.services";
      request.capability.function_path = "muon.services.acquire";
      BindMuonServiceOwner(request, {function});
      Expect(request.arguments[1].string_value == "application-data", "A caller path changed other arguments");
      function.plugin_namespace = "muon.services";
      request.kind = MuonRpcCallKind::PluginProxy;
      BindMuonServiceOwner(request, {function});
      Expect(request.arguments[1].string_value == "application-data", "A proxy collision changed arguments");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
