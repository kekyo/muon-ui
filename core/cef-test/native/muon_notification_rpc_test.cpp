/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_rpc.h"
#include <iostream>
#include <stdexcept>

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    auto metadata = MuonFunctionMetadata{};
    metadata.id = 7;
    metadata.plugin_namespace = "muon.notifications";
    metadata.js_name = "__watchEvents";
    metadata.public_name = "watchEvents";
    auto other = metadata;
    other.id = 8;
    other.plugin_namespace = "example.notifications";
    const auto functions = std::vector<MuonFunctionMetadata>{metadata, other};
    auto request = MuonRpcCallRequest{};
    request.owner = {3, "frame-a", 1};
    request.function_id = metadata.id;
    request.arguments.resize(2);
    for (auto& value : request.arguments) value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
    request.arguments[0].string_value = R"({"operation":"acquire"})";
    request.arguments[1].string_value = "spoofed-owner";
    BindMuonNotificationWatchOwner(request, functions);
    Expect(request.arguments[1].string_value == CreateMuonRpcOwnerKey(request.owner),
           "renderer-supplied notification owner was trusted");
    Expect(request.arguments[0].string_value == R"({"operation":"acquire"})",
           "notification operation payload changed");
    const auto first_owner = request.arguments[1].string_value;
    // Distinct renderer processes may both allocate context 1.
    request.owner = {4, "frame-b", 1};
    BindMuonNotificationWatchOwner(request, functions);
    Expect(request.arguments[1].string_value != first_owner &&
               request.arguments[1].string_value == CreateMuonRpcOwnerKey(request.owner),
           "different windows shared their notification owner");
    request.function_id = other.id;
    request.capability.function_path = "muon.notifications.watchEvents";
    request.arguments[1].string_value = "application-data";
    BindMuonNotificationWatchOwner(request, functions);
    Expect(request.arguments[1].string_value == "application-data",
           "caller path text changed a different function's arguments");
    request.function_id = metadata.id;
    request.kind = MuonRpcCallKind::PluginProxy;
    BindMuonNotificationWatchOwner(request, functions);
    Expect(request.arguments[1].string_value == "application-data",
           "proxy ID collision changed proxy arguments");
    request.kind = MuonRpcCallKind::Plugin;
    request.arguments.resize(1);
    BindMuonNotificationWatchOwner(request, functions);
    Expect(request.arguments.size() == 1, "malformed call bypassed argument validation");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
