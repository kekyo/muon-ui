/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_quickjs_service_host.h"
#include <yyjson.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <stdexcept>

struct QuickJsOwnedValue {
  JSContext* context;
  JSValue value;
  QuickJsOwnedValue(JSContext* context, JSValue value) : context(context), value(value) {}
  ~QuickJsOwnedValue() { JS_FreeValue(context, value); }
  QuickJsOwnedValue(const QuickJsOwnedValue&) = delete;
  QuickJsOwnedValue& operator=(const QuickJsOwnedValue&) = delete;
};

struct QuickJsAwait {
  JSValue promise;
  std::function<void(const std::string&, const std::string&)> complete;
};

struct QuickJsCall {
  uint64_t id;
  const MuonServiceMethod* method;
  MuonServiceCallbackHandler callback;
  MuonServiceCallCompletion complete;
  bool cancelled = false;
};

enum class QuickJsState { Starting, Running, Stopping, Closed };
struct QuickJsEntry {
  std::weak_ptr<MuonQuickJsServiceHostImpl> owner;
  MuonQuickJsServiceSpec spec;
  uint64_t generation;
  std::unique_ptr<MuonQuickJsEngine> engine;
  JSValue runtime = JS_UNDEFINED;
  JSValue getter = JS_UNDEFINED;
  JSValue stringify = JS_UNDEFINED;
  QuickJsState state = QuickJsState::Starting;
  MuonServiceInterface interface;
  std::string interface_json;
  std::vector<QuickJsAwait> pending;
  std::map<uint64_t, std::shared_ptr<QuickJsCall>> calls;
  std::function<void(const std::string&)> started;
  std::vector<std::function<void()>> stopped;
  std::function<void()> cancel_deadline;
  bool queued = false;
};

struct MuonQuickJsServiceHostImpl {
  MuonQuickJsServicePlatform platform;
  std::map<std::string, std::shared_ptr<QuickJsEntry>> entries;
  MuonServiceEventSink events;
  uint64_t next_call = 0;
  size_t queued = 0;
};

static void Pump(const std::shared_ptr<QuickJsEntry>& entry);
static void Dispose(const std::shared_ptr<QuickJsEntry>& entry, const std::string& error);

static void Queue(const std::shared_ptr<MuonQuickJsServiceHostImpl>& owner,
                  std::function<void()> task) {
  ++owner->queued;
  owner->platform.post([owner, task = std::move(task)] {
    struct Completion { MuonQuickJsServiceHostImpl& owner; ~Completion() { --owner.queued; } } done{*owner};
    task();
  });
}

static void Wake(const std::shared_ptr<QuickJsEntry>& entry) {
  if (entry->queued || entry->state == QuickJsState::Closed) return;
  entry->queued = true;
  Queue(entry->owner.lock(), [entry] {
    entry->queued = false;
    if (entry->state != QuickJsState::Closed) Pump(entry);
  });
}

static void Await(const std::shared_ptr<QuickJsEntry>& entry, JSValue promise,
                  std::function<void(const std::string&, const std::string&)> complete) {
  entry->pending.push_back({promise, std::move(complete)});
  Wake(entry);
}

static std::string Text(JSContext* context, JSValueConst value) {
  if (!JS_IsString(value)) throw std::invalid_argument("Expected a JavaScript string");
  size_t length = 0;
  const auto text = JS_ToCStringLen(context, &length, value);
  if (!text) throw std::runtime_error("Unable to read a JavaScript string");
  const auto result = std::string(text, length);
  JS_FreeCString(context, text);
  return result;
}

static std::string Json(const std::shared_ptr<QuickJsEntry>& entry, JSValueConst value) {
  auto* context = entry->engine->Context();
  const auto json = QuickJsOwnedValue(context,
      entry->engine->Call(entry->stringify, JS_UNDEFINED, {value}));
  return Text(context, json.value);
}

static JSValue Parse(const std::shared_ptr<QuickJsEntry>& entry, const std::string& json) {
  auto* context = entry->engine->Context();
  const auto value = JS_ParseJSON(context, json.data(), json.size(), "<service-wire>");
  if (JS_IsException(value)) {
    const auto error = QuickJsOwnedValue(context, JS_GetException(context));
    throw std::runtime_error(entry->engine->ExceptionText(error.value));
  }
  return value;
}

static JSValue Call(const std::shared_ptr<QuickJsEntry>& entry, const char* method,
                    const std::vector<JSValue>& arguments) {
  auto* context = entry->engine->Context();
  const auto name = QuickJsOwnedValue(context, JS_NewString(context, method));
  const auto function = QuickJsOwnedValue(context,
      entry->engine->Call(entry->getter, JS_UNDEFINED, {entry->runtime, name.value}));
  if (!JS_IsFunction(context, function.value)) throw std::invalid_argument("Missing service runtime operation");
  return entry->engine->Call(function.value, entry->runtime, arguments);
}

static std::string Field(yyjson_val* object, const char* name) {
  const auto value = yyjson_obj_get(object, name);
  if (!yyjson_is_str(value)) throw std::invalid_argument("Missing service result string");
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static MuonServiceResult Result(const MuonServiceMethod& method, const std::string& json) {
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  if (!document) throw std::invalid_argument("Invalid service result JSON");
  const auto root = yyjson_doc_get_root(document.get());
  const auto status = Field(root, "status");
  if (status == "ok") {
    const auto value = yyjson_obj_get(root, "value");
    if (!value) throw std::invalid_argument("Missing service result value");
    const auto text = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(value, 0, nullptr), &std::free);
    if (!text) throw std::runtime_error("Unable to copy service result");
    return {DecodeMuonServiceValue(method.signature.returns, text.get()), {}, {}};
  }
  if (status != "error") throw std::invalid_argument("Unknown service result status");
  const auto code = Field(root, "code");
  const auto message = Field(root, "message");
  if (code != "cancelled" && code != "unavailable" && code != "invalid-result" &&
      code != "callback-failed" && code != "resource-exhausted" &&
      std::find(method.errors.begin(), method.errors.end(), code) == method.errors.end())
    throw std::invalid_argument("Undeclared service failure");
  return {{}, code, message};
}

static void Complete(const std::shared_ptr<QuickJsEntry>& entry,
                     const std::shared_ptr<QuickJsCall>& call, MuonServiceResult result) {
  if (!call->complete) return;
  auto complete = std::move(call->complete);
  call->complete = nullptr;
  Queue(entry->owner.lock(), [complete = std::move(complete), result = std::move(result)]() mutable {
    complete(std::move(result));
  });
}

static void Deadline(const std::shared_ptr<QuickJsEntry>& entry) {
  if (entry->cancel_deadline) entry->cancel_deadline();
  const auto owner = entry->owner.lock();
  entry->cancel_deadline = owner->platform.delay(owner->platform.shutdown_timeout,
      [weak = std::weak_ptr<QuickJsEntry>(entry)] {
        if (const auto entry = weak.lock(); entry && entry->state != QuickJsState::Closed)
          Dispose(entry, "QuickJS service lifecycle deadline exceeded");
      });
}

static void Dispose(const std::shared_ptr<QuickJsEntry>& entry, const std::string& error) {
  if (entry->state == QuickJsState::Closed) return;
  const auto was_starting = entry->state == QuickJsState::Starting;
  entry->state = QuickJsState::Closed;
  const auto owner = entry->owner.lock();
  if (entry->cancel_deadline) entry->cancel_deadline();
  entry->cancel_deadline = nullptr;
  for (const auto& [id, call] : entry->calls) {
    (void)id;
    Complete(entry, call, {{}, "unavailable", error.empty() ? "Service stopped" : error});
  }
  entry->calls.clear();
  if (entry->engine) {
    auto* context = entry->engine->Context();
    for (const auto& pending : entry->pending) JS_FreeValue(context, pending.promise);
    entry->pending.clear();
    JS_FreeValue(context, entry->runtime);
    JS_FreeValue(context, entry->getter);
    JS_FreeValue(context, entry->stringify);
    entry->runtime = entry->getter = entry->stringify = JS_UNDEFINED;
    entry->engine.reset();
  }
  owner->entries.erase(entry->spec.id);
  if (was_starting) {
    auto complete = std::move(entry->started);
    entry->started = nullptr;
    if (complete) complete(error.empty() ? "Service startup interrupted" : error);
  } else if (!error.empty()) {
    owner->platform.exited(entry->spec.id, entry->generation, error);
  }
  auto stopped = std::move(entry->stopped);
  entry->stopped.clear();
  for (const auto& complete : stopped) complete();
}

static void Pump(const std::shared_ptr<QuickJsEntry>& entry) {
  try {
    entry->engine->RunJobs(64);
    // Only inspect promises after a bounded job turn. External completions wake
    // the next turn; unresolved promises never cause a polling loop.
    auto pending = std::move(entry->pending);
    entry->pending.clear();
    struct PendingValues {
      JSContext* context;
      std::vector<QuickJsAwait>& values;
      ~PendingValues() {
        for (const auto& item : values) JS_FreeValue(context, item.promise);
      }
    } release{entry->engine->Context(), pending};
    for (auto& item : pending) {
      auto* context = entry->engine->Context();
      const auto state = JS_PromiseState(context, item.promise);
      if (state == JS_PROMISE_PENDING) {
        entry->pending.push_back(std::move(item));
        item.promise = JS_UNDEFINED;
        continue;
      }
      const auto result = QuickJsOwnedValue(context,
          state == JS_PROMISE_FULFILLED || state == JS_PROMISE_REJECTED
              ? JS_PromiseResult(context, item.promise) : JS_DupValue(context, item.promise));
      JS_FreeValue(context, item.promise);
      item.promise = JS_UNDEFINED;
      const auto error = state == JS_PROMISE_REJECTED ? entry->engine->ExceptionText(result.value) : std::string{};
      // Copy before delivery: lifecycle completions may dispose the heap.
      const auto json = error.empty() && !JS_IsUndefined(result.value)
          ? Json(entry, result.value) : std::string{};
      auto complete = std::move(item.complete);
      item.complete = nullptr;
      Queue(entry->owner.lock(), [entry, complete = std::move(complete), json, error] {
        if (entry->state == QuickJsState::Closed) return;
        try { complete(json, error); }
        catch (const std::exception& failure) { Dispose(entry, failure.what()); }
      });
    }
    if (JS_IsJobPending(entry->engine->Runtime())) Wake(entry);
  } catch (const std::exception& error) {
    Dispose(entry, error.what());
  }
}

static std::shared_ptr<QuickJsEntry> Running(const std::shared_ptr<MuonQuickJsServiceHostImpl>& owner,
                                           const std::string& id, uint64_t generation) {
  const auto found = owner->entries.find(id);
  if (found == owner->entries.end() || found->second->generation != generation ||
      found->second->state != QuickJsState::Running)
    throw std::invalid_argument("QuickJS service generation is not running");
  return found->second;
}

MuonQuickJsServiceHost::MuonQuickJsServiceHost(MuonQuickJsServicePlatform platform)
    : impl_(std::make_shared<MuonQuickJsServiceHostImpl>()) {
  if (!platform.post || !platform.delay || !platform.log || !platform.exited ||
      platform.shutdown_timeout.count() <= 0)
    throw std::invalid_argument("Incomplete QuickJS service platform");
  impl_->platform = std::move(platform);
}

MuonQuickJsServiceHost::~MuonQuickJsServiceHost() { assert(IsIdle()); }
bool MuonQuickJsServiceHost::IsIdle() const { return impl_->entries.empty() && !impl_->queued; }

void MuonQuickJsServiceHost::Start(const MuonQuickJsServiceSpec& spec, uint64_t generation,
                                 std::function<void(const std::string&)> complete) {
  if (spec.id.empty() || !generation || impl_->entries.contains(spec.id))
    throw std::invalid_argument("Invalid or duplicate QuickJS service generation");
  auto entry = std::make_shared<QuickJsEntry>();
  entry->owner = impl_; entry->spec = spec; entry->generation = generation;
  entry->started = std::move(complete);
  impl_->entries.emplace(spec.id, entry);
  Queue(impl_, [entry] {
    try {
      entry->engine = std::make_unique<MuonQuickJsEngine>(entry->owner.lock()->platform.limits);
      entry->getter = entry->engine->Evaluate("(object, key) => object[key]", "<service-host>", JS_EVAL_TYPE_GLOBAL);
      entry->stringify = entry->engine->Evaluate("JSON.stringify", "<service-host>", JS_EVAL_TYPE_GLOBAL);
      Deadline(entry);
      Await(entry, entry->engine->Evaluate(entry->spec.source, entry->spec.filename, JS_EVAL_TYPE_MODULE),
          [entry](const std::string&, const std::string& error) {
            if (!error.empty()) { Dispose(entry, error); return; }
            try {
              entry->runtime = entry->engine->Evaluate("globalThis.__muonServiceRuntime", "<service-host>", JS_EVAL_TYPE_GLOBAL);
              auto* context = entry->engine->Context();
              const auto metadata = QuickJsOwnedValue(context, entry->engine->Evaluate(
                  "globalThis.__muonServiceInterface", "<service-host>", JS_EVAL_TYPE_GLOBAL));
              entry->interface_json = Json(entry, metadata.value);
              entry->interface = ParseMuonServiceInterface(entry->interface_json);
              const auto input = QuickJsOwnedValue(context, JS_NewObject(context));
              JS_SetPropertyStr(context, input.value, "id", JS_NewString(context, entry->spec.id.c_str()));
              JS_SetPropertyStr(context, input.value, "generation", JS_NewString(context, std::to_string(entry->generation).c_str()));
              JS_SetPropertyStr(context, input.value, "dataDirectory", JS_NewString(context, entry->spec.data_directory.c_str()));
              const auto config = JS_NewObjectProto(context, JS_NULL);
              for (const auto& [key, value] : entry->spec.config)
                JS_SetPropertyStr(context, config, key.c_str(), JS_NewStringLen(context, value.data(), value.size()));
              JS_SetPropertyStr(context, input.value, "config", config);
              Await(entry, Call(entry, "start", {input.value}), [entry](const std::string&, const std::string& error) {
                if (!error.empty()) { Dispose(entry, error); return; }
                entry->state = QuickJsState::Running;
                if (entry->cancel_deadline) entry->cancel_deadline();
                entry->cancel_deadline = nullptr;
                auto complete = std::move(entry->started);
                entry->started = nullptr;
                complete({});
              });
            } catch (const std::exception& error) { Dispose(entry, error.what()); }
          });
    } catch (const std::exception& error) { Dispose(entry, error.what()); }
  });
}

void MuonQuickJsServiceHost::Stop(const std::string& id, uint64_t generation, std::function<void()> complete) {
  const auto found = impl_->entries.find(id);
  if (found == impl_->entries.end() || found->second->generation != generation) {
    Queue(impl_, std::move(complete)); return;
  }
  const auto entry = found->second;
  if (entry->state == QuickJsState::Starting) throw std::logic_error("Stop during QuickJS startup");
  entry->stopped.push_back(std::move(complete));
  if (entry->state == QuickJsState::Stopping) return;
  entry->state = QuickJsState::Stopping;
  Queue(impl_, [entry] {
    if (entry->state == QuickJsState::Closed) return;
    try {
      Deadline(entry);
      const auto reason = QuickJsOwnedValue(entry->engine->Context(), JS_NewString(entry->engine->Context(), "Service stopped"));
      Await(entry, Call(entry, "stop", {reason.value}), [entry](const std::string&, const std::string& error) {
        Dispose(entry, error);
      });
    } catch (const std::exception& error) { Dispose(entry, error.what()); }
  });
}

MuonServiceExecution MuonQuickJsServiceHost::Execution() {
  const auto owner = impl_;
  return {
    [owner](const std::string& id, uint64_t generation) {
      return std::cref(Running(owner, id, generation)->interface);
    },
    [owner](const std::string& id, uint64_t generation) { return Running(owner, id, generation)->interface_json; },
    [owner](const std::string& id, uint64_t generation, const std::string& method,
        std::vector<MuonServiceValue> arguments, MuonServiceCallbackHandler callback,
        MuonServiceCallCompletion complete) {
      const auto entry = Running(owner, id, generation);
      const auto& signature = entry->interface.methods.at(method);
      const auto json = EncodeMuonServiceArguments(signature.signature.parameters, arguments);
      if (entry->calls.size() >= 128 || owner->next_call == UINT64_MAX)
        throw std::invalid_argument("QuickJS service call limit exceeded");
      const auto call = std::make_shared<QuickJsCall>(QuickJsCall{
          ++owner->next_call, &signature, std::move(callback), std::move(complete), false});
      entry->calls.emplace(call->id, call);
      Queue(owner, [entry, call, method, json] {
        if (entry->state == QuickJsState::Closed) return;
        try {
          auto* context = entry->engine->Context();
          const auto id = QuickJsOwnedValue(context, JS_NewString(context, std::to_string(call->id).c_str()));
          const auto name = QuickJsOwnedValue(context, JS_NewString(context, method.c_str()));
          const auto arguments = QuickJsOwnedValue(context, Parse(entry, json));
          Await(entry, Call(entry, "invoke", {id.value, name.value, arguments.value}),
              [entry, call](const std::string& json, const std::string& error) {
                try {
                  if (!error.empty()) throw std::runtime_error(error);
                  Complete(entry, call, Result(*call->method, json));
                } catch (const std::exception& failure) {
                  Complete(entry, call, {{}, "invalid-result", failure.what()});
                }
                entry->calls.erase(call->id);
              });
        } catch (const std::exception& error) {
          Complete(entry, call, {{}, "invalid-result", error.what()});
          entry->calls.erase(call->id);
        }
      });
      return call->id;
    },
    [owner](const std::string& id, uint64_t generation, uint64_t identity) {
      const auto found = owner->entries.find(id);
      if (found == owner->entries.end() || found->second->generation != generation) return;
      const auto entry = found->second;
      const auto found_call = entry->calls.find(identity);
      if (found_call == entry->calls.end() || found_call->second->cancelled) return;
      const auto call = found_call->second;
      call->cancelled = true;
      Complete(entry, call, {{}, "cancelled", "Service invocation cancelled"});
      Queue(owner, [entry, identity] {
        if (entry->state == QuickJsState::Closed) return;
        try {
          auto* context = entry->engine->Context();
          const auto id = QuickJsOwnedValue(context, JS_NewString(context, std::to_string(identity).c_str()));
          const auto result = QuickJsOwnedValue(context, Call(entry, "cancel", {id.value}));
          Wake(entry);
        } catch (const std::exception& error) { Dispose(entry, error.what()); }
      });
    },
    [owner](MuonServiceEventSink sink) { owner->events = std::move(sink); }
  };
}
