/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include "quickjs/muon_quickjs_engine.h"
#include "services/muon_service_execution.h"
#include <muon_plugin_api.h>

/** Resolved source and immutable configuration of one JavaScript service. */
struct MuonQuickJsServiceSpec {
  std::string id; /**< Configured logical identity. */
  std::string filename; /**< Bundle identity used in diagnostics. */
  std::string source; /**< Verified application bundle, loaded by the platform. */
  std::string data_directory; /**< Private directory created by the platform. */
  std::vector<std::pair<std::string, std::string>> config; /**< String settings. */
  /** Optional platform deadline, including time spent loading the bundle. */
  std::optional<std::chrono::steady_clock::time_point> startup_deadline = std::nullopt;
};

/** Generation-scoped host modules and their asynchronous native resources. */
struct MuonQuickJsServiceExtension {
  /** Resolves only explicitly granted host modules; throws for unknown names. */
  std::function<JSModuleDef*(JSContext*, const std::string&)> load_module = {};
  /** Releases every JSValue and disables JavaScript delivery before heap disposal. */
  std::function<void()> detach = {};
  /** After detach, drains native work; completion must run on the owner dispatcher. */
  std::function<void(std::function<void()>)> close = {};
};

/** OS operations; all completions are serialized on the owner dispatcher. */
struct MuonQuickJsServicePlatform {
  /** Queues one turn, never invoking it inline. */
  std::function<void(std::function<void()>)> post;
  /** Schedules one deferred turn and returns an idempotent cancellation handle. */
  std::function<std::function<void()>(std::chrono::milliseconds,
      std::function<void()>)> delay;
  /** Copies service-scoped diagnostics. */
  std::function<void(const std::string&, muon_log_level, const std::string&)> log;
  /** Reports a failed generation after heap disposal; native work may still drain. */
  std::function<void(const std::string&, uint64_t, const std::string&)> exited;
  MuonQuickJsLimits limits; /**< Independent heap, stack, and turn limits. */
  std::chrono::milliseconds shutdown_timeout; /**< Startup and stop deadline. */
  /** Optional extension factory called before bundle evaluation. Wake queues jobs;
   * fault closes this exact generation. A throwing factory releases its resources.
   * Returned detach and close hooks must not throw. */
  std::function<MuonQuickJsServiceExtension(const MuonQuickJsServiceSpec&, uint64_t,
      MuonQuickJsEngine&, std::function<void()>,
      std::function<void(const std::string&)>)> attach = {};
};

struct MuonQuickJsServiceHostImpl; /**< Private generation and queue ownership. */

/**
 * Executes isolated JavaScript services on an externally owned dispatcher.
 * The caller loads bundles with asynchronous platform I/O. The host owns no
 * threads and queues bounded promise-job turns. Stop and failed startup dispose
 * all engine values before reporting completion; pending external replies are
 * bound to the exact generation and ignored after disposal.
 */
class MuonQuickJsServiceHost final {
 public:
  /** @param platform Owner scheduling, limits, and diagnostics. */
  explicit MuonQuickJsServiceHost(MuonQuickJsServicePlatform platform);
  /** Requires every generation and posted turn to have drained. */
  ~MuonQuickJsServiceHost();
  MuonQuickJsServiceHost(const MuonQuickJsServiceHost&) = delete;
  MuonQuickJsServiceHost& operator=(const MuonQuickJsServiceHost&) = delete;
  /** @param spec Bundle and configuration, copied before returning.
   *  @param generation Nonzero lifecycle generation.
   *  @param complete Deferred result, empty on success. */
  void Start(const MuonQuickJsServiceSpec& spec, uint64_t generation,
             std::function<void(const std::string&)> complete);
  /** @param id Service identity.
   *  @param generation Exact generation to dispose.
   *  @param complete Deferred completion after heap disposal and native drainage. */
  void Stop(const std::string& id, uint64_t generation, std::function<void()> complete);
  /** @returns Borrowed runtime operations; this host must outlive them. */
  MuonServiceExecution Execution();
  /** @returns True when no generation, native cleanup, or posted turn remains. */
  bool IsIdle() const;
 private:
  std::shared_ptr<MuonQuickJsServiceHostImpl> impl_;
};
