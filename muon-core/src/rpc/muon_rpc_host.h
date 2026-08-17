/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_plugin_policy.h"
#include "rpc/muon_rpc.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * Selects whether direct RPC calls require a capability proof.
 */
enum class MuonRpcHostMode {
  /** Direct calls are routed without a capability proof. */
  Simple,

  /** Direct calls require a matching configured capability. */
  Validate,
};

/**
 * Selects the implementation boundary for one direct RPC function.
 */
enum class MuonRpcRouteKind {
  /** Function is implemented by the native plugin runtime. */
  Plugin,

  /** Function is implemented by the platform host. */
  Platform,
};

/**
 * Immutable routing metadata for one direct RPC function.
 */
struct MuonRpcFunctionRoute {
  /** Runtime-wide zero-based function identifier. */
  uint32_t function_id = 0;

  /** Public JavaScript function path used by capability validation. */
  std::string public_path;

  /** Implementation boundary that receives the decoded call. */
  MuonRpcRouteKind kind = MuonRpcRouteKind::Plugin;
};

/**
 * Completion supplied to plugin and platform invocation services.
 */
using MuonRpcHostCompletion =
    std::function<void(const MuonRpcCallResult& result)>;

/**
 * CEF-independent transport and implementation services used by an RPC host.
 */
struct MuonRpcHostServices {
  /** Routes one decoded plugin or plugin-proxy invocation. */
  std::function<void(const MuonRpcCallRequest& request,
                     MuonRpcHostCompletion completion)>
      invoke_plugin;

  /** Routes one decoded platform-host invocation. */
  std::function<void(const MuonRpcCallRequest& request,
                     MuonRpcHostCompletion completion)>
      invoke_platform;

  /** Releases one plugin-proxy wrapper lease. */
  std::function<void(const MuonRpcPluginProxyRelease& release)>
      release_plugin_proxy;

  /** Releases every RPC resource owned by one JavaScript context. */
  std::function<void(const MuonRpcContextReleased& release)> release_context;

  /** Sends one validated invocation result to the client transport. */
  std::function<void(const MuonRpcCallResult& result)> send_result;
};

/** Opaque implementation state retained by the RPC host. */
struct MuonRpcHostImpl;

/**
 * CEF-independent capability validator and invocation router.
 *
 * All methods and service completions must be serialized by the platform host.
 */
class MuonRpcHost final {
 public:
  /** Releases shared routing state and invalidates outstanding completions. */
  ~MuonRpcHost();

  /**
   * Validates and routes one decoded plugin call.
   *
   * Validation failures for a valid owner are returned through send_result.
   *
   * @param request Decoded direct or plugin-proxy call.
   * @return true when the request was consumed.
   */
  bool DispatchCall(const MuonRpcCallRequest& request);

  /**
   * Handles one decoded host-bound RPC control message.
   *
   * @param message Message to route.
   * @return true for calls and supported release messages.
   */
  bool HandleMessage(const MuonRpcMessage& message);

  /** Returns the number of invocations awaiting a completion. */
  size_t GetPendingCallCount() const;

 private:
  explicit MuonRpcHost(std::shared_ptr<MuonRpcHostImpl> impl);

  std::shared_ptr<MuonRpcHostImpl> impl_;

  friend bool CreateMuonRpcHost(
      MuonRpcHostMode mode,
      const std::vector<MuonRpcFunctionRoute>& routes,
      const std::map<std::string, std::shared_ptr<MuonPluginPolicy>>&
          capability_policies,
      MuonRpcHostServices services,
      std::shared_ptr<MuonRpcHost>* host,
      std::string* error_message);
};

/**
 * Creates a validated CEF-independent RPC host.
 *
 * @param mode Capability validation mode for direct calls.
 * @param routes Complete direct-function routing table.
 * @param capability_policies Policies keyed by generated capability id.
 * @param services Invocation, release, and result transport services.
 * @param host Receives the created host.
 * @param error_message Receives a configuration diagnostic on failure.
 * @return true when the routing table and services are valid.
 */
bool CreateMuonRpcHost(
    MuonRpcHostMode mode,
    const std::vector<MuonRpcFunctionRoute>& routes,
    const std::map<std::string, std::shared_ptr<MuonPluginPolicy>>&
        capability_policies,
    MuonRpcHostServices services,
    std::shared_ptr<MuonRpcHost>* host,
    std::string* error_message);
