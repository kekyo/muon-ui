/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_rpc.h"
#include <utility>

void BindMuonNotificationWatchOwner(
    MuonRpcCallRequest& request,
    const std::vector<MuonFunctionMetadata>& functions) {
  if (request.kind != MuonRpcCallKind::Plugin || request.arguments.size() != 2) {
    return;
  }
  for (const auto& function : functions) {
    if (function.id == request.function_id &&
        function.plugin_namespace == "muon.notifications" &&
        function.public_name == "watchEvents") {
      // The browser authenticated the complete owner while decoding the IPC.
      // A renderer's numeric context ID alone is not unique across processes.
      auto owner = MuonRpcValue{};
      owner.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
      owner.string_value = CreateMuonRpcOwnerKey(request.owner);
      request.arguments[1] = std::move(owner);
      return;
    }
  }
}
