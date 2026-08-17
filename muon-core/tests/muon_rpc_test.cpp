/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "rpc/muon_rpc.h"

#include <cstdint>
#include <iostream>
#include <limits>
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

int main() {
  return RunOwnerIdentityTest() && RunBinaryStorageTest() &&
                 RunTypedMessageTest() && RunClientStateTest()
             ? 0
             : 1;
}
