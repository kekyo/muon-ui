/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_rpc.h"
#include <iostream>
#include <stdexcept>

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
int main() {
  try {
    auto function = MuonFunctionMetadata{};
    function.id = 12;
    function.plugin_namespace = "muon.server";
    function.js_name = "__dispatch";
    function.public_name = "createServer";
    const auto functions = std::vector<MuonFunctionMetadata>{function};
    auto request = MuonRpcCallRequest{};
    request.function_id = function.id;
    request.owner = {5, "frame-one", 1};
    request.arguments.resize(3);
    request.arguments[0].string_value = "command";
    request.arguments[2].string_value = "spoofed-owner";
    BindMuonServerOwner(request, functions);
    const auto first = request.arguments[2].string_value;
    Expect(first == CreateMuonRpcOwnerKey(request.owner), "Server owner was not authenticated");
    Expect(request.arguments[0].string_value == "command", "Server command changed");
    request.owner = {6, "frame-two", 1};
    BindMuonServerOwner(request, functions);
    Expect(request.arguments[2].string_value != first, "Different renderers share an owner");
    request.kind = MuonRpcCallKind::PluginProxy;
    request.arguments[2].string_value = "proxy data";
    BindMuonServerOwner(request, functions);
    Expect(request.arguments[2].string_value == "proxy data", "Proxy call changed by ID collision");
    request.kind = MuonRpcCallKind::Plugin;
    request.function_id = 13;
    request.capability.function_path = "muon.server.createServer";
    BindMuonServerOwner(request, functions);
    Expect(request.arguments[2].string_value == "proxy data", "Untrusted path selected the binding");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
