/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/builtin/muon_builtin_fs_watch_registry.h"

#include <limits>
#include <utility>
#include <vector>

namespace muon_internal {

MuonBuiltinFsWatchRegistry::MuonBuiltinFsWatchRegistry()
    : MuonBuiltinFsWatchRegistry(MuonBuiltinFsWatchLimits{}) {}

MuonBuiltinFsWatchRegistry::MuonBuiltinFsWatchRegistry(
    MuonBuiltinFsWatchLimits limits)
    : limits_(limits) {}

std::optional<MuonBuiltinFsWatchLease> MuonBuiltinFsWatchRegistry::TryAcquire(
    int renderer_context_id,
    std::string* error_message) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto owner = counts_by_context_.find(renderer_context_id);
  const auto owner_count =
      owner == counts_by_context_.end() ? size_t{0} : owner->second;
  if (owner_count >= limits_.max_per_context) {
    if (error_message != nullptr) {
      *error_message = "Filesystem watcher limit exceeded";
    }
    return std::nullopt;
  }
  if (global_count_ >= limits_.max_global) {
    if (error_message != nullptr) {
      *error_message = "Global filesystem watcher limit exceeded";
    }
    return std::nullopt;
  }

  auto lease_id = next_id_;
  auto token = std::string{};
  while (true) {
    token = "watch:" + std::to_string(lease_id);
    const auto next_id = lease_id == std::numeric_limits<uint64_t>::max()
                             ? uint64_t{1}
                             : lease_id + 1;
    if (leases_by_token_.find(token) == leases_by_token_.end()) {
      next_id_ = next_id;
      break;
    }
    lease_id = next_id;
  }
  auto lease = MuonBuiltinFsWatchLease{
      lease_id,
      renderer_context_id,
      std::move(token),
  };
  ++global_count_;
  ++counts_by_context_[renderer_context_id];
  leases_by_token_.emplace(lease.token, lease);
  if (error_message != nullptr) {
    error_message->clear();
  }
  return lease;
}

bool MuonBuiltinFsWatchRegistry::IsActive(
    int renderer_context_id,
    const std::string& token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = leases_by_token_.find(token);
  return iterator != leases_by_token_.end() &&
         iterator->second.renderer_context_id == renderer_context_id;
}

bool MuonBuiltinFsWatchRegistry::Release(int renderer_context_id,
                                         const std::string& token) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iterator = leases_by_token_.find(token);
  if (iterator == leases_by_token_.end() ||
      iterator->second.renderer_context_id != renderer_context_id) {
    return false;
  }
  ReleaseLocked(iterator);
  return true;
}

void MuonBuiltinFsWatchRegistry::ReleaseContext(int renderer_context_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> tokens;
  tokens.reserve(leases_by_token_.size());
  for (const auto& entry : leases_by_token_) {
    if (entry.second.renderer_context_id == renderer_context_id) {
      tokens.push_back(entry.first);
    }
  }
  for (const auto& token : tokens) {
    const auto iterator = leases_by_token_.find(token);
    if (iterator != leases_by_token_.end()) {
      ReleaseLocked(iterator);
    }
  }
}

void MuonBuiltinFsWatchRegistry::ReleaseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  leases_by_token_.clear();
  counts_by_context_.clear();
  global_count_ = 0;
}

MuonBuiltinFsWatchCounts MuonBuiltinFsWatchRegistry::GetCounts(
    int renderer_context_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto counts = MuonBuiltinFsWatchCounts{};
  counts.global_count = global_count_;
  const auto owner = counts_by_context_.find(renderer_context_id);
  if (owner != counts_by_context_.end()) {
    counts.owner_count = owner->second;
  }
  return counts;
}

MuonBuiltinFsWatchCounts MuonBuiltinFsWatchRegistry::GetGlobalCounts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return MuonBuiltinFsWatchCounts{0, global_count_};
}

void MuonBuiltinFsWatchRegistry::ReleaseLocked(LeaseIterator iterator) {
  const auto renderer_context_id = iterator->second.renderer_context_id;
  if (global_count_ > 0) {
    --global_count_;
  }
  const auto count = counts_by_context_.find(renderer_context_id);
  if (count != counts_by_context_.end()) {
    if (count->second > 1) {
      --count->second;
    } else {
      counts_by_context_.erase(count);
    }
  }
  leases_by_token_.erase(iterator);
}

}  // namespace muon_internal
