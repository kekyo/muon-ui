/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_rpc.h"

void BindMuonServerOwner(MuonRpcCallRequest& request,
                        const std::vector<MuonFunctionMetadata>& functions) {
  if (request.kind != MuonRpcCallKind::Plugin || request.arguments.size() != 3) return;
  for (const auto& function : functions) {
    if (function.id == request.function_id &&
        function.plugin_namespace == "muon.server" && function.js_name == "__dispatch") {
      auto& owner = request.arguments[2];
      owner.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
      owner.string_value = CreateMuonRpcOwnerKey(request.owner);
      return;
    }
  }
}
