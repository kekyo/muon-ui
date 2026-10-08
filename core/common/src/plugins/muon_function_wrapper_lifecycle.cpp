/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_function_wrapper_lifecycle.h"

#include <limits>
#include <utility>

static bool IsKnownFunctionWrapperKind(MuonFunctionWrapperKind kind) {
  return kind == MuonFunctionWrapperKind::kRendererSource ||
         kind == MuonFunctionWrapperKind::kPluginProxyLease;
}

static void IncrementFunctionWrapperCounts(
    MuonFunctionWrapperCounts* counts,
    MuonFunctionWrapperKind kind) {
  if (kind == MuonFunctionWrapperKind::kRendererSource) {
    ++counts->renderer_source_count;
  } else {
    ++counts->plugin_proxy_lease_count;
  }
  ++counts->total_count;
}

static void DecrementFunctionWrapperCounts(
    MuonFunctionWrapperCounts* counts,
    MuonFunctionWrapperKind kind) {
  if (kind == MuonFunctionWrapperKind::kRendererSource) {
    --counts->renderer_source_count;
  } else {
    --counts->plugin_proxy_lease_count;
  }
  --counts->total_count;
}

MuonFunctionWrapperLifecycle::MuonFunctionWrapperLifecycle()
    : MuonFunctionWrapperLifecycle(
          {kMuonFunctionWrapperOwnerLimit,
           kMuonFunctionWrapperGlobalLimit}) {}

MuonFunctionWrapperLifecycle::MuonFunctionWrapperLifecycle(
    MuonFunctionWrapperLimits limits)
    : limits_(limits) {}

std::optional<MuonFunctionWrapperLease>
MuonFunctionWrapperLifecycle::TryAcquire(
    const std::string& owner_id,
    MuonFunctionWrapperKind kind,
    const MuonFunctionWrapperAcquireCallback& acquire_backend) {
  auto lease = MuonFunctionWrapperLease{};
  {
    const auto lock = std::lock_guard(mutex_);
    const auto owner_iterator = owner_counts_.find(owner_id);
    const auto owner_total = owner_iterator == owner_counts_.end()
                                 ? 0
                                 : owner_iterator->second.total_count;
    if (!IsKnownFunctionWrapperKind(kind) || lease_ids_exhausted_ ||
        owner_total >= limits_.owner_limit ||
        global_counts_.total_count >= limits_.global_limit) {
      return std::nullopt;
    }

    lease = {next_lease_id_, owner_id, kind};
    if (next_lease_id_ == std::numeric_limits<uint64_t>::max()) {
      lease_ids_exhausted_ = true;
    } else {
      ++next_lease_id_;
    }

    const auto [owner_counts_iterator, owner_inserted] =
        owner_counts_.try_emplace(owner_id);
    try {
      active_leases_.emplace(lease.id, lease);
    } catch (...) {
      if (owner_inserted) {
        owner_counts_.erase(owner_counts_iterator);
      }
      throw;
    }
    IncrementFunctionWrapperCounts(&owner_counts_iterator->second, kind);
    IncrementFunctionWrapperCounts(&global_counts_, kind);
  }

  try {
    if (acquire_backend()) {
      return lease;
    }
  } catch (...) {
    Release(lease);
    throw;
  }

  Release(lease);
  return std::nullopt;
}

bool MuonFunctionWrapperLifecycle::Release(
    const MuonFunctionWrapperLease& lease) {
  const auto lock = std::lock_guard(mutex_);
  return ReleaseLocked(lease);
}

bool MuonFunctionWrapperLifecycle::ReleaseLocked(
    const MuonFunctionWrapperLease& lease) {
  const auto active_iterator = active_leases_.find(lease.id);
  if (active_iterator == active_leases_.end() ||
      active_iterator->second.owner_id != lease.owner_id ||
      active_iterator->second.kind != lease.kind) {
    return false;
  }

  const auto owner_iterator = owner_counts_.find(lease.owner_id);
  if (owner_iterator == owner_counts_.end()) {
    return false;
  }

  DecrementFunctionWrapperCounts(&owner_iterator->second, lease.kind);
  DecrementFunctionWrapperCounts(&global_counts_, lease.kind);
  active_leases_.erase(active_iterator);
  if (owner_iterator->second.total_count == 0) {
    owner_counts_.erase(owner_iterator);
  }
  return true;
}

MuonFunctionWrapperCounts MuonFunctionWrapperLifecycle::GetOwnerCounts(
    const std::string& owner_id) const {
  const auto lock = std::lock_guard(mutex_);
  const auto iterator = owner_counts_.find(owner_id);
  return iterator == owner_counts_.end() ? MuonFunctionWrapperCounts{}
                                        : iterator->second;
}

MuonFunctionWrapperCounts MuonFunctionWrapperLifecycle::GetGlobalCounts()
    const {
  const auto lock = std::lock_guard(mutex_);
  return global_counts_;
}
