/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include <quickjs.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

/** Resource limits for an independent JavaScript heap on one owner dispatcher. */
struct MuonQuickJsLimits {
  size_t memory_bytes; /**< Maximum engine allocation, including pending jobs. */
  size_t stack_bytes; /**< Maximum JavaScript stack usage on the owner thread. */
  std::chrono::milliseconds execution_time; /**< Deadline for one call or job batch. */
};

/** Private engine ownership and execution deadline. */
struct MuonQuickJsEngineImpl;

/**
 * Owns an isolated QuickJS runtime and context without threads or an event loop.
 * The platform invokes this engine only on its owner dispatcher and schedules
 * another RunJobs turn when work remains. All borrowed and owned JSValues must
 * be released before destruction. This is internal; the public service ABI does
 * not expose QuickJS types.
 */
class MuonQuickJsEngine final {
 public:
  /** @param limits Heap, stack, and execution limits; every limit must be positive.
   *  @throws std::runtime_error When engine or context creation fails. */
  explicit MuonQuickJsEngine(MuonQuickJsLimits limits);
  /** Releases pending jobs, context, and heap on the owner thread. */
  ~MuonQuickJsEngine();
  MuonQuickJsEngine(const MuonQuickJsEngine&) = delete;
  MuonQuickJsEngine& operator=(const MuonQuickJsEngine&) = delete;

  /** @returns Borrowed runtime, for module registration and diagnostics. */
  JSRuntime* Runtime() const;
  /** @returns Borrowed context, for host function and value creation. */
  JSContext* Context() const;
  /**
   * Evaluates script or module source within the execution deadline.
   * @param source UTF-8 JavaScript source, owned by the caller.
   * @param filename Module identity and diagnostic location.
   * @param flags Public JS_EVAL_TYPE_GLOBAL/MODULE and JS_EVAL_FLAG_* options.
   * @returns An owned result or module-evaluation promise; free with JS_FreeValue.
   * @throws std::runtime_error For JavaScript exceptions and resource limits.
   */
  JSValue Evaluate(const std::string& source, const std::string& filename, int flags);
  /**
   * Calls a borrowed JavaScript function within the execution deadline.
   * @param function Borrowed callable value in this engine.
   * @param receiver Borrowed this value, or JS_UNDEFINED.
   * @param arguments Borrowed argument values, retained by the caller.
   * @returns Owned result; promises are progressed by subsequent RunJobs turns.
   * @throws std::runtime_error For JavaScript exceptions and resource limits.
   */
  JSValue Call(JSValueConst function, JSValueConst receiver,
               const std::vector<JSValue>& arguments);
  /**
   * Runs a bounded number of pending promise jobs under one execution deadline.
   * @param maximum_jobs Positive job count; remaining work yields to the platform.
   * @returns True when another dispatcher turn is required.
   * @throws std::runtime_error If a job throws or exceeds its execution deadline.
   */
  bool RunJobs(size_t maximum_jobs);
  /** Requests permanent interruption; the sole operation allowed from any thread. */
  void RequestInterrupt();

 private:
  std::unique_ptr<MuonQuickJsEngineImpl> impl_;
};
