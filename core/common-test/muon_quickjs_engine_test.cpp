/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "quickjs/muon_quickjs_engine.h"
#include "quickjs/muon_quickjs_module.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>

static std::string Text(JSContext* context, JSValue value) {
  size_t length = 0;
  const auto text = JS_ToCStringLen(context, &length, value);
  assert(text);
  const auto result = std::string(text, length);
  JS_FreeCString(context, text);
  JS_FreeValue(context, value);
  return result;
}

static bool Fails(MuonQuickJsEngine& engine, const std::string& source) {
  try {
    const auto result = engine.Evaluate(source, "failure.js", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(engine.Context(), result);
    return false;
  } catch (const std::runtime_error& error) {
    return *error.what() != '\0';
  }
}

static JSModuleDef* Load(JSContext* context, const char* name, void*) {
  if (std::strcmp(name, "muon:fixture") != 0) {
    JS_ThrowReferenceError(context, "Unknown host module: %s", name);
    return nullptr;
  }
  const auto module = JS_NewObject(context);
  JS_SetPropertyStr(context, module, "answer", JS_NewBigUint64(context, UINT64_MAX));
  return CreateMuonQuickJsObjectModule(context, name, module);
}

int main() {
  const auto limits = MuonQuickJsLimits{64 * 1024 * 1024, 1024 * 1024,
      std::chrono::milliseconds(100)};
  {
    MuonQuickJsEngine engine(limits);
    JS_SetModuleLoaderFunc(engine.Runtime(), nullptr, Load, nullptr);
    const auto module = engine.Evaluate(
        "import api, { answer } from 'muon:fixture'; globalThis.same = api.answer === answer;",
        "modules.mjs", JS_EVAL_TYPE_MODULE);
    while (engine.RunJobs(16)) {}
    assert(JS_PromiseState(engine.Context(), module) == JS_PROMISE_FULFILLED);
    JS_FreeValue(engine.Context(), module);
    assert(Text(engine.Context(), engine.Evaluate("same", "probe.js", JS_EVAL_TYPE_GLOBAL)) == "true");
    try {
      JS_FreeValue(engine.Context(), engine.Evaluate(
          "import 'muon:missing';", "missing.mjs", JS_EVAL_TYPE_MODULE));
      assert(false);
    } catch (const std::runtime_error&) {}
  }
  {
    std::atomic<bool> stopped{false};
    MuonQuickJsEngine engine(limits, [&stopped]() { return stopped.load(); });
    assert(Text(engine.Context(), engine.Evaluate("21 * 2", "live.js", JS_EVAL_TYPE_GLOBAL)) == "42");
    stopped.store(true);
    assert(Fails(engine, "for (;;) {}"));
  }
  {
    MuonQuickJsEngine first(limits);
    MuonQuickJsEngine second(limits);
    const auto module = first.Evaluate(
        "globalThis.answer = await Promise.resolve(18446744073709551615n);",
        "service.mjs", JS_EVAL_TYPE_MODULE);
    while (first.RunJobs(16)) {}
    assert(JS_PromiseState(first.Context(), module) == JS_PROMISE_FULFILLED);
    JS_FreeValue(first.Context(), module);
    assert(Text(first.Context(), first.Evaluate("answer", "probe.js", JS_EVAL_TYPE_GLOBAL)) ==
        "18446744073709551615");
    assert(Text(second.Context(), second.Evaluate("typeof answer", "probe.js", JS_EVAL_TYPE_GLOBAL)) ==
        "undefined");
    assert(Fails(first, "throw new Error('service failure')"));
    assert(Text(second.Context(), second.Evaluate("40 + 2", "probe.js", JS_EVAL_TYPE_GLOBAL)) == "42");
    const auto callable = first.Evaluate("value => value + 1n", "function.js", JS_EVAL_TYPE_GLOBAL);
    const auto argument = JS_NewBigUint64(first.Context(), UINT64_MAX - 1);
    assert(Text(first.Context(), first.Call(callable, JS_UNDEFINED, {argument})) == "18446744073709551615");
    JS_FreeValue(first.Context(), argument);
    JS_FreeValue(first.Context(), callable);
  }
  {
    MuonQuickJsEngine engine(limits);
    const auto result = engine.Evaluate(R"(
      globalThis.count = 0;
      globalThis.done = false;
      globalThis.loop = async () => {
        while (!globalThis.done) { ++globalThis.count; await 0; }
      };
      loop();
    )", "jobs.js", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(engine.Context(), result);
    assert(engine.RunJobs(8));
    assert(Text(engine.Context(), engine.Evaluate("count", "probe.js", JS_EVAL_TYPE_GLOBAL)) == "9");
    JS_FreeValue(engine.Context(), engine.Evaluate("done = true", "stop.js", JS_EVAL_TYPE_GLOBAL));
    assert(!engine.RunJobs(8));
  }
  {
    MuonQuickJsEngine engine(limits);
    const auto error = engine.Evaluate(
        "Object.defineProperty(new Error(), 'message', {get() { for (;;) {} }})",
        "host-error.js", JS_EVAL_TYPE_GLOBAL);
    assert(!engine.ExceptionText(error).empty());
    JS_FreeValue(engine.Context(), error);
    assert(Fails(engine, "for (;;) {}"));
    assert(Fails(engine, "throw { toString() { for (;;) {} } }"));
    assert(Fails(engine, "throw Object.defineProperty(new Error(), 'message', {get() { for (;;) {} }})"));
    assert(Text(engine.Context(), engine.Evaluate("1", "recovery.js", JS_EVAL_TYPE_GLOBAL)) == "1");
    assert(Fails(engine, "const grow = () => grow(); grow();"));
    assert(Fails(engine, "new ArrayBuffer(128 * 1024 * 1024)"));
    assert(Text(engine.Context(), engine.Evaluate("2", "recovery.js", JS_EVAL_TYPE_GLOBAL)) == "2");
    engine.RequestInterrupt();
    assert(Fails(engine, "for (;;) {}"));
  }
  {
    MuonQuickJsEngine engine(limits);
    // Destruction also frees an unfinished chain of jobs without running more
    // application code during teardown.
    JS_FreeValue(engine.Context(), engine.Evaluate(
        "(async () => { for (;;) await 0; })()", "pending.js", JS_EVAL_TYPE_GLOBAL));
    assert(engine.RunJobs(8));
  }
  std::cout << "QuickJS isolation, bounded jobs, memory, stack, and interruption passed\n";
}
