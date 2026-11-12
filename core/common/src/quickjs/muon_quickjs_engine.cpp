/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "quickjs/muon_quickjs_engine.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <thread>

struct MuonQuickJsEngineImpl {
  MuonQuickJsLimits limits;
  std::function<bool()> should_interrupt;
  const std::thread::id owner = std::this_thread::get_id();
  JSRuntime* runtime = nullptr;
  JSContext* context = nullptr;
  std::atomic<bool> interrupted{false};
  size_t depth = 0;
  std::chrono::steady_clock::time_point deadline;

  explicit MuonQuickJsEngineImpl(MuonQuickJsLimits value, std::function<bool()> stop)
      : limits(value), should_interrupt(std::move(stop)) {}
  ~MuonQuickJsEngineImpl() {
    assert(owner == std::this_thread::get_id());
    if (context) JS_FreeContext(context);
    if (runtime) JS_FreeRuntime(runtime);
  }
  void RequireOwner() const {
    if (owner != std::this_thread::get_id())
      throw std::logic_error("QuickJS must run on its owner dispatcher");
  }
};

// Nested host calls share the outer deadline. A callback cannot reset its
// caller's budget by synchronously reentering the same engine.
struct Execution {
  MuonQuickJsEngineImpl& engine;
  explicit Execution(MuonQuickJsEngineImpl& value) : engine(value) {
    engine.RequireOwner();
    if (engine.depth++ == 0)
      engine.deadline = std::chrono::steady_clock::now() + engine.limits.execution_time;
  }
  ~Execution() { --engine.depth; }
};

static int Interrupt(JSRuntime*, void* opaque) {
  const auto& engine = *static_cast<MuonQuickJsEngineImpl*>(opaque);
  try {
    if (engine.should_interrupt && engine.should_interrupt()) return 1;
  } catch (...) {
    // A host predicate must never unwind through the C engine.
    return 1;
  }
  return engine.interrupted.load(std::memory_order_relaxed) ||
      (engine.depth && std::chrono::steady_clock::now() >= engine.deadline);
}

static std::string String(JSContext* context, JSValueConst value) {
  // Never invoke a user-defined toString while reporting another exception.
  if (!JS_IsString(value)) return {};
  size_t size = 0;
  const auto text = JS_ToCStringLen(context, &size, value);
  if (!text) return {};
  auto result = std::string(text, std::min<size_t>(size, 8192));
  JS_FreeCString(context, text);
  return result;
}

static std::string DescribeException(JSContext* context, JSValueConst exception) {
  auto result = String(context, exception);
  if (JS_IsError(context, exception)) {
    // Error properties may be getters; the surrounding execution deadline
    // also covers diagnostic extraction and its secondary exceptions.
    for (const auto property : {"message", "stack"}) {
      const auto value = JS_GetPropertyStr(context, exception, property);
      if (JS_IsException(value)) JS_FreeValue(context, JS_GetException(context));
      else {
        const auto text = String(context, value);
        if (!text.empty()) {
          if (!result.empty()) result += '\n';
          result += text;
        }
      }
      JS_FreeValue(context, value);
    }
  }
  return result.empty() ? "QuickJS execution failed" : result;
}

static std::string Exception(JSContext* context) {
  const auto error = JS_GetException(context);
  const auto result = DescribeException(context, error);
  JS_FreeValue(context, error);
  return result;
}

MuonQuickJsEngine::MuonQuickJsEngine(MuonQuickJsLimits limits,
    std::function<bool()> should_interrupt)
    : impl_(std::make_unique<MuonQuickJsEngineImpl>(limits, std::move(should_interrupt))) {
  if (!limits.memory_bytes || !limits.stack_bytes || limits.execution_time.count() <= 0)
    throw std::invalid_argument("QuickJS resource limits must be positive");
  impl_->runtime = JS_NewRuntime();
  if (!impl_->runtime) throw std::runtime_error("Unable to create a QuickJS runtime");
  JS_SetMemoryLimit(impl_->runtime, limits.memory_bytes);
  JS_SetMaxStackSize(impl_->runtime, limits.stack_bytes);
  JS_SetCanBlock(impl_->runtime, false);
  JS_SetInterruptHandler(impl_->runtime, Interrupt, impl_.get());
  impl_->context = JS_NewContext(impl_->runtime);
  if (!impl_->context) throw std::runtime_error("Unable to create a QuickJS context");
}

MuonQuickJsEngine::~MuonQuickJsEngine() = default;
JSRuntime* MuonQuickJsEngine::Runtime() const { impl_->RequireOwner(); return impl_->runtime; }
JSContext* MuonQuickJsEngine::Context() const { impl_->RequireOwner(); return impl_->context; }

JSValue MuonQuickJsEngine::Evaluate(const std::string& source,
                                  const std::string& filename, int flags) {
  Execution scope(*impl_);
  const auto result = JS_Eval(impl_->context, source.c_str(), source.size(), filename.c_str(), flags);
  if (JS_IsException(result)) throw std::runtime_error(Exception(impl_->context));
  return result;
}

JSValue MuonQuickJsEngine::Call(JSValueConst function, JSValueConst receiver,
                               const std::vector<JSValue>& arguments) {
  if (arguments.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("Too many QuickJS call arguments");
  Execution scope(*impl_);
  // JSValueConst does not add C++ const on every QuickJS representation. The
  // engine borrows these value bits; this array does not own additional refs.
  auto borrowed = arguments;
  const auto result = JS_Call(impl_->context, function, receiver,
      static_cast<int>(borrowed.size()), borrowed.data());
  if (JS_IsException(result)) throw std::runtime_error(Exception(impl_->context));
  return result;
}

bool MuonQuickJsEngine::RunJobs(size_t maximum_jobs) {
  if (!maximum_jobs) throw std::invalid_argument("QuickJS job budget must be positive");
  Execution scope(*impl_);
  for (size_t index = 0; index < maximum_jobs && JS_IsJobPending(impl_->runtime); ++index) {
    JSContext* context = nullptr;
    if (JS_ExecutePendingJob(impl_->runtime, &context) < 0)
      throw std::runtime_error(Exception(context ? context : impl_->context));
  }
  return JS_IsJobPending(impl_->runtime);
}

std::string MuonQuickJsEngine::ExceptionText(JSValueConst error) {
  Execution scope(*impl_);
  return DescribeException(impl_->context, error);
}

void MuonQuickJsEngine::RequestInterrupt() { impl_->interrupted.store(true, std::memory_order_relaxed); }
