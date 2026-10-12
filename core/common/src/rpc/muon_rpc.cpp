/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "rpc/muon_rpc.h"

#include <utility>

class MuonRpcOwnedBufferStorage final : public MuonRpcBufferStorage {
 public:
  explicit MuonRpcOwnedBufferStorage(size_t size) : bytes_(size) {}

  void* GetData() override {
    return bytes_.empty() ? nullptr : bytes_.data();
  }

  const void* GetData() const override {
    return bytes_.empty() ? nullptr : bytes_.data();
  }

  size_t GetSize() const override { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
};

bool IsValidMuonRpcOwner(const MuonRpcOwner& owner) {
  return owner.browser_id > 0 && !owner.frame_id.empty() &&
         owner.context_id > 0;
}

bool AreEqualMuonRpcOwners(const MuonRpcOwner& first,
                           const MuonRpcOwner& second) {
  return first.browser_id == second.browser_id &&
         first.frame_id == second.frame_id &&
         first.context_id == second.context_id;
}

std::string CreateMuonRpcOwnerKey(const MuonRpcOwner& owner) {
  return std::to_string(owner.browser_id) + ":" + owner.frame_id + ":" +
         std::to_string(owner.context_id);
}

std::shared_ptr<MuonRpcBufferStorage> CreateMuonRpcOwnedBuffer(size_t size) {
  return std::make_shared<MuonRpcOwnedBufferStorage>(size);
}

bool IsValidMuonRpcBinary(const MuonRpcBinary& binary) {
  if (!binary.storage) {
    return false;
  }
  const auto storage_size = binary.storage->GetSize();
  return binary.offset <= storage_size && binary.size <= storage_size &&
         binary.size <= storage_size - binary.offset &&
         (binary.size == 0 || binary.storage->GetData() != nullptr);
}

void* GetMuonRpcBinaryData(const MuonRpcBinary& binary) {
  if (!IsValidMuonRpcBinary(binary) || binary.size == 0) {
    return nullptr;
  }
  auto* data = static_cast<uint8_t*>(binary.storage->GetData());
  return data + binary.offset;
}

MuonRpcClientState::MuonRpcClientState(uint32_t maximum_call_id)
    : maximum_call_id_(maximum_call_id) {}

bool MuonRpcClientState::BeginCall(const MuonRpcOwner& owner,
                                   const MuonTypeMetadata& return_type,
                                   uint32_t* call_id,
                                   std::string* error_message) {
  if (call_id == nullptr || error_message == nullptr) {
    return false;
  }
  *call_id = 0;
  error_message->clear();
  if (!IsValidMuonRpcOwner(owner)) {
    *error_message = "muon RPC owner is invalid";
    return false;
  }
  if (next_call_id_ == 0 || next_call_id_ > maximum_call_id_) {
    *error_message = "muon call ids are exhausted";
    return false;
  }

  MuonRpcPendingCall pending_call;
  pending_call.owner = owner;
  pending_call.call_id = next_call_id_;
  pending_call.return_type = return_type;
  pending_calls_.emplace(pending_call.call_id, pending_call);
  *call_id = pending_call.call_id;
  next_call_id_ = pending_call.call_id == maximum_call_id_
                      ? 0
                      : pending_call.call_id + 1;
  return true;
}

MuonRpcCallCompletionStatus MuonRpcClientState::CompleteCall(
    const MuonRpcOwner& owner,
    uint32_t call_id,
    MuonRpcPendingCall* pending_call) {
  const auto iterator = pending_calls_.find(call_id);
  if (iterator == pending_calls_.end()) {
    return completed_call_ids_.find(call_id) == completed_call_ids_.end()
               ? MuonRpcCallCompletionStatus::UnknownCall
               : MuonRpcCallCompletionStatus::Duplicate;
  }
  if (!AreEqualMuonRpcOwners(iterator->second.owner, owner)) {
    return MuonRpcCallCompletionStatus::OwnerMismatch;
  }
  if (pending_call != nullptr) {
    *pending_call = iterator->second;
  }
  pending_calls_.erase(iterator);
  completed_call_ids_.insert(call_id);
  return MuonRpcCallCompletionStatus::Completed;
}

bool MuonRpcClientState::CancelCall(const MuonRpcOwner& owner,
                                    uint32_t call_id,
                                    MuonRpcPendingCall* pending_call) {
  const auto iterator = pending_calls_.find(call_id);
  if (iterator == pending_calls_.end() ||
      !AreEqualMuonRpcOwners(iterator->second.owner, owner)) {
    return false;
  }
  if (pending_call != nullptr) {
    *pending_call = iterator->second;
  }
  pending_calls_.erase(iterator);
  return true;
}

std::vector<MuonRpcPendingCall> MuonRpcClientState::ReleaseOwner(
    const MuonRpcOwner& owner) {
  auto released = std::vector<MuonRpcPendingCall>{};
  auto iterator = pending_calls_.begin();
  while (iterator != pending_calls_.end()) {
    if (!AreEqualMuonRpcOwners(iterator->second.owner, owner)) {
      ++iterator;
      continue;
    }
    released.push_back(iterator->second);
    iterator = pending_calls_.erase(iterator);
  }
  return released;
}

size_t MuonRpcClientState::GetPendingCallCount() const {
  return pending_calls_.size();
}
