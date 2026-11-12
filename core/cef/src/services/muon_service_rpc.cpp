/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_rpc.h"

void BindMuonServiceOwner(MuonRpcCallRequest& request,
    const std::vector<MuonFunctionMetadata>& functions) {
  if (request.kind != MuonRpcCallKind::Plugin || request.arguments.size() != 2) return;
  for (const auto& function : functions) {
    if (function.id == request.function_id && function.plugin_namespace == "muon.services" &&
        (function.public_name == "acquire" || function.public_name == "release" ||
         function.public_name == "getState")) {
      auto owner = MuonRpcValue{};
      owner.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
      owner.string_value = CreateMuonRpcOwnerKey(request.owner);
      request.arguments[1] = std::move(owner);
      return;
    }
  }
}
