/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace muon_internal {

/**
 * Maximum active filesystem watchers owned by one renderer V8 context.
 */
inline constexpr size_t kMuonBuiltinFsWatchPerContextLimit = 16;

/**
 * Maximum active filesystem watchers across the process.
 */
inline constexpr size_t kMuonBuiltinFsWatchGlobalLimit = 128;

/**
 * Filesystem watcher quota limits.
 */
struct MuonBuiltinFsWatchLimits {
  /** Per-renderer V8 context watcher limit. */
  size_t max_per_context = kMuonBuiltinFsWatchPerContextLimit;
  /** Process-wide watcher limit. */
  size_t max_global = kMuonBuiltinFsWatchGlobalLimit;
};

/**
 * Active filesystem watcher lease.
 */
struct MuonBuiltinFsWatchLease {
  /** Monotonic lease id used to derive an opaque token. */
  uint64_t id = 0;
  /** Renderer V8 context that owns the watcher lease. */
  int renderer_context_id = 0;
  /** Opaque token required by subsequent watcher operations. */
  std::string token;
};

/**
 * Filesystem watcher quota counters.
 */
struct MuonBuiltinFsWatchCounts {
  /** Active watcher count for the requested owner context. */
  size_t owner_count = 0;
  /** Active watcher count across the process. */
  size_t global_count = 0;
};

/**
 * Tracks active filesystem watcher leases and enforces per-context quotas.
 */
class MuonBuiltinFsWatchRegistry final {
 public:
  /**
   * Creates a registry with production watcher limits.
   */
  MuonBuiltinFsWatchRegistry();

  /**
   * Creates a registry with explicit watcher limits.
   */
  explicit MuonBuiltinFsWatchRegistry(MuonBuiltinFsWatchLimits limits);

  /**
   * Acquires a watcher lease for a renderer V8 context.
   *
   * @param renderer_context_id Renderer V8 context id supplied by muon.
   * @param error_message Receives a quota diagnostic when acquisition fails.
   * @return A watcher lease when quota is available.
   */
  std::optional<MuonBuiltinFsWatchLease> TryAcquire(
      int renderer_context_id,
      std::string* error_message);

  /**
   * Returns whether a token belongs to an active lease for the context.
   */
  bool IsActive(int renderer_context_id, const std::string& token) const;

  /**
   * Releases one watcher lease.
   */
  bool Release(int renderer_context_id, const std::string& token);

  /**
   * Releases every watcher lease owned by the renderer V8 context.
   */
  void ReleaseContext(int renderer_context_id);

  /**
   * Releases every watcher lease.
   */
  void ReleaseAll();

  /**
   * Returns counters for one renderer V8 context.
   */
  MuonBuiltinFsWatchCounts GetCounts(int renderer_context_id) const;

  /**
   * Returns process-wide watcher counters.
   */
  MuonBuiltinFsWatchCounts GetGlobalCounts() const;

 private:
  using LeaseIterator =
      std::unordered_map<std::string, MuonBuiltinFsWatchLease>::iterator;

  void ReleaseLocked(LeaseIterator iterator);

  const MuonBuiltinFsWatchLimits limits_;
  mutable std::mutex mutex_;
  uint64_t next_id_ = 1;
  size_t global_count_ = 0;
  std::unordered_map<std::string, MuonBuiltinFsWatchLease> leases_by_token_;
  std::unordered_map<int, size_t> counts_by_context_;
};

}  // namespace muon_internal
