/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#ifndef MUON_FUNCTION_WRAPPER_LIFECYCLE_H
#define MUON_FUNCTION_WRAPPER_LIFECYCLE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

/** Maximum combined renderer source and plugin proxy leases per owner. */
inline constexpr size_t kMuonFunctionWrapperOwnerLimit = 256;

/** Maximum combined renderer source and plugin proxy leases per runtime. */
inline constexpr size_t kMuonFunctionWrapperGlobalLimit = 4096;

/** Maximum retained renderer function references per V8 context. */
inline constexpr size_t kMuonRendererFunctionReferenceLimit = 256;

/** Identifies the resource represented by a function wrapper lease. */
enum class MuonFunctionWrapperKind {
  /** A native source backed by a renderer function. */
  kRendererSource,

  /** A renderer wrapper backed by a retained plugin function. */
  kPluginProxyLease,
};

/** Limits applied by a function wrapper lifecycle tracker. */
struct MuonFunctionWrapperLimits {
  /** Maximum combined leases belonging to one owner. */
  size_t owner_limit;

  /** Maximum combined leases across all owners. */
  size_t global_limit;
};

/** Counts of active function wrapper resources. */
struct MuonFunctionWrapperCounts {
  /** Number of renderer function sources. */
  size_t renderer_source_count = 0;

  /** Number of retained plugin function proxy leases. */
  size_t plugin_proxy_lease_count = 0;

  /** Combined number of function wrapper resources. */
  size_t total_count = 0;
};

/** Identifies one active function wrapper reservation. */
struct MuonFunctionWrapperLease {
  /** Monotonically increasing identity that is never reused. */
  uint64_t id;

  /** Browser, frame, and V8 context owner identity. */
  std::string owner_id;

  /** Resource kind reserved by this lease. */
  MuonFunctionWrapperKind kind;
};

/** Callback that creates or retains the backend resource for a reservation. */
using MuonFunctionWrapperAcquireCallback = std::function<bool()>;

/**
 * Tracks owner-scoped function wrapper resources and enforces fixed quotas.
 *
 * Reservations count against quota before the backend callback runs. A backend
 * failure releases the reservation, and a thrown exception releases it before
 * the exception is propagated to the caller.
 */
class MuonFunctionWrapperLifecycle final {
 public:
  /** Creates a tracker with the production limits. */
  MuonFunctionWrapperLifecycle();

  /**
   * Creates a tracker with explicit limits.
   *
   * @param limits Owner and runtime-wide limits. Test code can provide smaller
   *     values to exercise quota behavior.
   */
  explicit MuonFunctionWrapperLifecycle(MuonFunctionWrapperLimits limits);

  /** Function wrapper lifecycle state cannot be copied. */
  MuonFunctionWrapperLifecycle(const MuonFunctionWrapperLifecycle&) = delete;

  /** Function wrapper lifecycle state cannot be copy-assigned. */
  MuonFunctionWrapperLifecycle& operator=(
      const MuonFunctionWrapperLifecycle&) = delete;

  /**
   * Reserves quota and acquires the corresponding backend resource.
   *
   * @param owner_id Browser, frame, and V8 context owner identity.
   * @param kind Resource kind being acquired.
   * @param acquire_backend Callback that creates or retains the backend
   *     resource. It is not called when quota or lease identities are
   *     exhausted.
   * @return An active lease on success, or no value when quota is unavailable
   *     or the backend rejects acquisition.
   * @remarks If `acquire_backend` throws, the provisional reservation is
   *     rolled back before the exception is propagated.
   */
  std::optional<MuonFunctionWrapperLease> TryAcquire(
      const std::string& owner_id,
      MuonFunctionWrapperKind kind,
      const MuonFunctionWrapperAcquireCallback& acquire_backend);

  /**
   * Releases an active lease.
   *
   * @param lease Lease identity, owner, and kind to release.
   * @return True only when an exactly matching active lease was released.
   * @remarks Duplicate and mismatched releases do not change any count.
   */
  bool Release(const MuonFunctionWrapperLease& lease);

  /**
   * Gets active counts for one owner.
   *
   * @param owner_id Browser, frame, and V8 context owner identity.
   * @return A snapshot of the owner's active counts.
   */
  MuonFunctionWrapperCounts GetOwnerCounts(
      const std::string& owner_id) const;

  /** @return A snapshot of runtime-wide active counts. */
  MuonFunctionWrapperCounts GetGlobalCounts() const;

 private:
  bool ReleaseLocked(const MuonFunctionWrapperLease& lease);

  const MuonFunctionWrapperLimits limits_;
  mutable std::mutex mutex_;
  uint64_t next_lease_id_ = 1;
  bool lease_ids_exhausted_ = false;
  std::unordered_map<uint64_t, MuonFunctionWrapperLease> active_leases_;
  std::unordered_map<std::string, MuonFunctionWrapperCounts> owner_counts_;
  MuonFunctionWrapperCounts global_counts_;
};

#endif
