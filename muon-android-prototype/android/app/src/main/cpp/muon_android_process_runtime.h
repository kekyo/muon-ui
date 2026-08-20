/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_plugin_runtime.h"
#include "rpc/muon_rpc.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

/** Platform callbacks retained for one Android Activity/WebView session. */
struct MuonAndroidProcessSessionCallbacks {
  /** Sends a typed runtime message to this session's WebView transport. */
  std::function<bool(const MuonRpcMessage& message,
                     std::string* error_message)>
      send_message;

  /** Delivers the completed cardio integration probe mask. */
  std::function<void(uint32_t mask)> deliver_runtime_probe;

  /** Signals that a probe settled and whether its result was delivered. */
  std::function<void(bool delivered)> settle_runtime_probe;
};

/** Test-observable process runtime and Android dispatcher lifecycle counts. */
struct MuonAndroidProcessRuntimeDiagnostics {
  /** Monotonic identity of the current or most recently created runtime. */
  uint64_t generation = 0;

  /** Number of dispatcher hosts created during this process lifetime. */
  uint64_t created_dispatcher_hosts = 0;

  /** Number of dispatcher hosts destroyed after asynchronous plugin stop. */
  uint64_t destroyed_dispatcher_hosts = 0;

  /** Number of currently live Android dispatcher hosts. */
  size_t live_dispatcher_hosts = 0;

  /** Number of Activity/WebView sessions registered with the process. */
  size_t active_sessions = 0;

  /** File descriptors opened while the current dispatcher was constructed. */
  size_t runtime_file_descriptors = 0;

  /** Dispatcher-owned descriptors still open immediately after destruction. */
  size_t leaked_dispatcher_file_descriptors = 0;

  /** Plugin probes that have been invoked and have not completed. */
  size_t outstanding_probes = 0;

  /** Probe results suppressed because their owner context was released. */
  uint64_t suppressed_probe_results = 0;

  /** Whether diagnostics were requested from the runtime owner thread. */
  bool owner_thread = false;
};

/** Immutable plugin metadata and policies exposed to one WebView session. */
struct MuonAndroidPluginCatalog {
  /** Namespaces produced by the plugins that successfully loaded. */
  std::vector<MuonNamespaceMetadata> namespaces;

  /** Functions produced by the plugins that successfully loaded. */
  std::vector<MuonFunctionMetadata> functions;

  /** Registry policies keyed by the capability id used by Android pages. */
  std::map<std::string, std::shared_ptr<MuonPluginPolicy>>
      capability_policies;

  /** Default simple-mode capability id for each public function path. */
  std::map<std::string, std::string> capability_ids_by_function_path;
};

struct MuonAndroidProcessRuntimeControllerImpl;

/** Owns one cardio host and native plugin runtime per Android process. */
class MuonAndroidProcessRuntimeController final {
 public:
  /**
   * Captures the constructing Java main Looper thread as runtime owner.
   *
   * @param schedule_stop_completion Posts final destruction to a later Java
   * main Looper iteration, after the completing cardio callback has returned.
   */
  explicit MuonAndroidProcessRuntimeController(
      std::function<bool()> schedule_stop_completion);

  /** Releases controller bookkeeping after all native runtime resources stop. */
  ~MuonAndroidProcessRuntimeController();

  /**
   * Registers one Activity/WebView session and starts the process runtime.
   *
   * @param callbacks Session transport and diagnostic callbacks.
   * @param owner Receives a new monotonic process-wide owner identity.
   * @param error_message Receives a deterministic startup error.
   * @return true when the session was accepted.
   */
  bool RegisterSession(MuonAndroidProcessSessionCallbacks callbacks,
                       MuonRpcOwner* owner,
                       std::string* error_message);

  /** Marks a session available when its WebView creates a new context. */
  void ActivateSession(const MuonRpcOwner& owner);

  /**
   * Copies the loaded plugin catalog used to construct one WebView context.
   *
   * @param catalog Receives the immutable runtime metadata and policies.
   * @param error_message Receives a deterministic availability diagnostic.
   * @return true while the process runtime is ready for calls.
   */
  bool GetPluginCatalog(MuonAndroidPluginCatalog* catalog,
                        std::string* error_message) const;

  /** Resolves recursive argument metadata for a plugin or proxy call. */
  bool GetCallArgumentTypes(const MuonRpcCallRequest& request,
                            std::vector<MuonTypeMetadata>* argument_types,
                            std::string* error_message) const;

  /** Invokes one fully decoded native plugin call. */
  void Invoke(const MuonRpcCallRequest& request,
              MuonPluginRuntime::Completion completion);

  /** Resolves the result type for one pending renderer callback. */
  bool GetRendererFunctionReturnType(const MuonRpcOwner& owner,
                                     uint32_t call_id,
                                     MuonTypeMetadata* return_type) const;

  /** Completes one plugin-initiated renderer callback. */
  void CompleteRendererFunctionCall(
      const MuonRpcRendererFunctionResult& result);

  /** Releases one native plugin proxy wrapper lease. */
  void ReleasePluginFunctionProxy(
      const MuonRpcPluginProxyRelease& release);

  /** Releases runtime resources associated with one WebView context. */
  void ReleaseSessionContext(const MuonRpcOwner& owner);

  /**
   * Removes a session and asynchronously stops the last normal runtime.
   *
   * @param owner Session identity to remove.
   * @param preserve_runtime true for an Activity configuration change.
   */
  void UnregisterSession(const MuonRpcOwner& owner,
                         bool preserve_runtime);

  /** Invokes the packaged cardio test plugin for one active session. */
  void StartRuntimeProbe(const MuonRpcOwner& owner);

  /** Finalizes a pending asynchronous stop outside the cardio call stack. */
  void CompletePendingStop();

  /** Returns process runtime lifecycle diagnostics. */
  MuonAndroidProcessRuntimeDiagnostics GetDiagnostics() const;

  MuonAndroidProcessRuntimeController(
      const MuonAndroidProcessRuntimeController&) = delete;
  MuonAndroidProcessRuntimeController& operator=(
      const MuonAndroidProcessRuntimeController&) = delete;

 private:
  std::unique_ptr<MuonAndroidProcessRuntimeControllerImpl> impl_;
};
