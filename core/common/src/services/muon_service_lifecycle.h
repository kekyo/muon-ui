/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/** Observed execution state, independently of the requested lifetime. */
enum class MuonServiceState {
  /** No runtime resources remain. */
  Stopped,
  /** The host is creating runtime resources. */
  Starting,
  /** The runtime can accept calls from ready references. */
  Running,
  /** The host is disposing of the current runtime. */
  Stopping,
  /** Execution failed and all resources of that generation are disposed. */
  Failed,
};

/** Build-time identity and previously loaded user policy for one service. */
struct MuonServiceDefinition {
  /** Unique, nonempty identifier within this application. */
  std::string id;
  /** Manifest policy used when the user has not selected a value. */
  bool default_autostart = false;
  /** Persisted user policy; an explicit false differs from an absent value. */
  std::optional<bool> user_autostart;
};

/** An owner-bound reservation that becomes callable after an acquired event. */
struct MuonServiceReference {
  /** Host-lifetime unique reference identity; zero is never issued. */
  uint64_t id = 0;
  /** Configured service identity. */
  std::string service_id;
  /** Authenticated page or connection session owning this reservation. */
  std::string owner_id;
  /** Runtime generation to which this reference exclusively belongs. */
  uint64_t generation = 0;

  /** Compares the complete identity, including owner and generation. */
  bool operator==(const MuonServiceReference&) const = default;
};

/** A copied observation; retaining it does not keep a service alive. */
struct MuonServiceSnapshot {
  /** Configured service identity. */
  std::string id;
  /** Current execution state. */
  MuonServiceState state = MuonServiceState::Stopped;
  /** Current or most recently terminated runtime generation. */
  uint64_t generation = 0;
  /** Last successfully committed setting operation, initially zero. */
  uint64_t revision = 0;
  /** Effective user policy, falling back to the manifest default. */
  bool autostart = false;
  /** Whether the host currently retains the single resident reference. */
  bool resident = false;
  /** Number of client references, including pending acquisitions. */
  size_t client_references = 0;
  /** Last runtime failure, empty after a new start or normal stop. */
  std::string error;
};

/** Actions and observations produced by the serialized lifecycle manager. */
enum class MuonServiceEventKind {
  /** Create the runtime identified by snapshot.id and snapshot.generation. */
  Start,
  /** Dispose of that runtime, then report completion. */
  Stop,
  /** Atomically persist autostart for the specified setting operation. */
  SaveAutostart,
  /** A reservation became callable; reference identifies its owner. */
  Acquired,
  /** A pending reservation failed and was removed. */
  AcquireFailed,
  /** A previously callable reference was invalidated by runtime termination. */
  Invalidated,
  /** A setting request completed; success and error describe its outcome. */
  AutostartResult,
  /** An updated observation that does not create a client reference. */
  Changed,
};

/**
 * A value-owned action or notification. No event retains the manager.
 *
 * Only fields documented for the selected kind carry additional data.
 */
struct MuonServiceEvent {
  /** Action or notification discriminator. */
  MuonServiceEventKind kind = MuonServiceEventKind::Changed;
  /** State captured when the event was created. */
  MuonServiceSnapshot snapshot;
  /** Present for acquisition results and invalidation. */
  std::optional<MuonServiceReference> reference;
  /** Nonzero for SaveAutostart and AutostartResult. */
  uint64_t operation_id = 0;
  /** Requested value for SaveAutostart and AutostartResult. */
  bool autostart = false;
  /** True for a successful AutostartResult. */
  bool success = false;
  /** Diagnostic for failed acquisition, invalidation, or setting requests. */
  std::string error;
};

/** Private reference, generation, and persistence queues. */
struct MuonServiceLifecycleImpl;

/**
 * OS-independent service lifetime manager.
 *
 * The host serializes every method and completion on its dispatcher. Methods
 * never invoke host code: TakeEvents transfers actions after state is settled,
 * so a synchronous backend completion or a reentrant client callback is safe.
 * Before delivering Acquired, a reentrant transport must recheck IsReady: an
 * earlier notification may have released that reference or shut down the host.
 * A no-longer-ready acquisition still needs a failed/cancelled transport reply
 * if its requester is waiting; silently dropping it must not strand a promise.
 * Runtime actions always address their recorded generation, never whichever
 * generation happens to be current when the action is handled.
 * Every Start, Stop, and SaveAutostart action must settle, including an action
 * cancelled before backend dispatch. Report cancelled starts/saves as failures;
 * report a stop only after that generation's resources are actually gone.
 * The host must stop delivering completions before destroying this object.
 * Runtime completion methods ignore stale generations and duplicate messages.
 */
class MuonServiceLifecycle final {
 public:
  /**
   * Creates configured services and requests their resident starts.
   * @param definitions Complete manifest identities and loaded user overrides.
   * @throws std::invalid_argument If an identity is empty or duplicated.
   */
  explicit MuonServiceLifecycle(
      const std::vector<MuonServiceDefinition>& definitions);
  /** Releases bookkeeping; the host must complete Shutdown first. */
  ~MuonServiceLifecycle();
  /** Lifecycle identities cannot be copied. */
  MuonServiceLifecycle(const MuonServiceLifecycle&) = delete;
  /** Lifecycle identities cannot be assigned. */
  MuonServiceLifecycle& operator=(const MuonServiceLifecycle&) = delete;

  /**
   * Reserves an owner reference before requesting asynchronous startup.
   * @param service_id Configured service identity.
   * @param owner_id Nonempty authenticated live connection or page session.
   * @return Reservation, or no value for an invalid identity, shutdown, or
   *     exhausted identities. Only Acquired makes the reservation callable.
   * @remarks The host rejects messages from closed owners before this call.
   */
  std::optional<MuonServiceReference> Acquire(
      const std::string& service_id, const std::string& owner_id);

  /**
   * Releases one exact reservation or ready reference.
   * @param reference Complete identity issued by Acquire.
   * @return True only for the first matching release. A pending acquisition
   *     receives AcquireFailed; an already acquired reference is just released.
   */
  bool Release(const MuonServiceReference& reference);

  /**
   * Recovers all reservations and references belonging to a disconnected owner.
   * @param owner_id Authenticated owner whose session has ended.
   * @return Number of recovered references across all services.
   */
  size_t ReleaseOwner(const std::string& owner_id);

  /**
   * Reports runtime startup after its resources are usable or fully rolled back.
   * @param service_id Started service identity.
   * @param generation Generation from Start.
   * @param error Empty on success, otherwise the startup failure diagnostic.
   */
  void CompleteStart(const std::string& service_id, uint64_t generation,
                     const std::string& error);

  /**
   * Reports normal shutdown after every runtime resource is disposed.
   * @param service_id Stopped service identity.
   * @param generation Generation from Stop.
   */
  void CompleteStop(const std::string& service_id, uint64_t generation);

  /**
   * Begins cleanup required by the OS, independently of resident/client demand.
   * @param service_id Affected service identity.
   * @param generation Generation reported by the platform.
   * @param reason OS rejection, deadline, or forced shutdown diagnostic.
   * @remarks Internal platform operation, not a page-facing stop API. Invalidates
   *     all acquisitions immediately and rejects new ones until cleanup finishes.
   *     Pending startup must settle before Stop is issued. CompleteStop records
   *     failure without retrying the retained resident policy. A later explicit
   *     acquisition can retry, subject to the platform's eligibility checks.
   */
  void RequestRuntimeStop(const std::string& service_id, uint64_t generation,
                          const std::string& reason);

  /**
   * Reports an unexpected exit after that generation's resources are gone.
   * @param service_id Failed service identity.
   * @param generation Failed runtime generation.
   * @param error Runtime or OS failure diagnostic.
   * @remarks Retains resident policy but does not automatically restart it.
   */
  void RuntimeExited(const std::string& service_id, uint64_t generation,
                     const std::string& error);

  /**
   * Queues a persistent policy change, without waiting for runtime stop/start.
   * @param service_id Configured service identity.
   * @param enabled Requested persistent user override.
   * @return Operation identity, or zero for an unknown service or shutdown.
   * @remarks Saves are serialized per service. Explicitly choosing a manifest
   *     default is still persisted when no user override exists yet.
   */
  uint64_t SetAutostart(const std::string& service_id, bool enabled);

  /**
   * Completes the outstanding atomic save and applies its resident reference.
   * @param service_id Identity from SaveAutostart.
   * @param operation_id Identity from SaveAutostart.
   * @param error Empty on successful durable save. Failure must leave storage
   *     unchanged; the lifecycle then leaves its policy and reference unchanged.
   */
  void CompleteAutostart(const std::string& service_id, uint64_t operation_id,
                         const std::string& error);

  /**
   * Stops the whole host without changing persisted autostart preferences.
   * @param reason Reason sent to pending acquisitions and live references.
   * @remarks Rejects new work. A save already issued may finish, but cannot
   *     recreate resident references. Unissued setting requests are cancelled.
   */
  void Shutdown(const std::string& reason);

  /**
   * Checks whether an exact reference can invoke its current generation.
   * @param reference Identity issued by Acquire.
   * @return True only after acquisition and before release or invalidation.
   */
  bool IsReady(const MuonServiceReference& reference) const;

  /**
   * Observes a configured service without acquiring it.
   * @param service_id Configured identity.
   * @return Current snapshot, or no value for an unknown identity.
   */
  std::optional<MuonServiceSnapshot> GetSnapshot(
      const std::string& service_id) const;

  /** @return True once Shutdown has rejected new work. */
  bool IsShuttingDown() const;
  /** @return True when all runtimes and issued setting saves have settled. */
  bool IsQuiescent() const;
  /** @return Pending actions and observations in their original order. */
  std::vector<MuonServiceEvent> TakeEvents();

 private:
  std::unique_ptr<MuonServiceLifecycleImpl> impl_;
};
