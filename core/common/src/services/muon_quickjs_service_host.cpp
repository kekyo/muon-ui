/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_quickjs_service_host.h"
#include <yyjson.h>
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

struct QuickJsOwnedValue {
  JSContext* context;
  JSValue value;
  QuickJsOwnedValue(JSContext* context, JSValue value) : context(context), value(value) {
    if (JS_IsException(value)) {
      JS_FreeValue(context, JS_GetException(context));
      throw std::runtime_error("Unable to allocate a QuickJS value");
    }
  }
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
  bool returned = false;
  std::map<uint64_t, const MuonServiceSignature*> tokens;
};

struct QuickJsCallback {
  uint64_t call_id;
  const MuonServiceSignature* signature;
  JSValue resolve;
  JSValue reject;
};

struct QuickJsTimer {
  JSValue function;
  std::vector<JSValue> arguments;
  std::function<void()> cancel;
};

enum class QuickJsState { Starting, Running, Stopping, Closed };
struct QuickJsEntry {
  std::weak_ptr<QuickJsEntry> self;
  std::weak_ptr<MuonQuickJsServiceHostImpl> owner;
  MuonQuickJsServiceSpec spec;
  uint64_t generation;
  std::unique_ptr<MuonQuickJsEngine> engine;
  MuonQuickJsServiceExtension extension;
  JSValue runtime = JS_UNDEFINED;
  JSValue getter = JS_UNDEFINED;
  JSValue stringify = JS_UNDEFINED;
  QuickJsState state = QuickJsState::Starting;
  MuonServiceInterface interface;
  std::string interface_json;
  std::vector<QuickJsAwait> pending;
  std::map<uint64_t, std::shared_ptr<QuickJsCall>> calls;
  std::map<uint64_t, QuickJsCallback> callbacks;
  std::map<uint64_t, QuickJsTimer> timers;
  uint64_t next_callback = 0;
  uint64_t next_timer = 0;
  size_t pending_events = 0;
  size_t pending_event_bytes = 0;
  std::function<void(const std::string&)> started;
  std::vector<std::function<void()>> stopped;
  std::function<void()> cancel_deadline;
  std::chrono::steady_clock::time_point lifecycle_deadline;
  bool queued = false;
  std::string fault;
};

struct MuonQuickJsServiceHostImpl {
  MuonQuickJsServicePlatform platform;
  std::map<std::string, std::shared_ptr<QuickJsEntry>> entries;
  std::map<std::pair<std::string, uint64_t>, std::shared_ptr<QuickJsEntry>> closing;
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

// Some failures are detected inside another engine call or while borrowed
// values remain on the C++ stack. Close admission now, dispose on the next turn.
static void Fault(const std::shared_ptr<QuickJsEntry>& entry, const std::string& reason) {
  if (entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
  entry->fault = reason;
  entry->engine->RequestInterrupt();
  Queue(entry->owner.lock(), [entry] { Dispose(entry, entry->fault); });
}

static void Wake(const std::shared_ptr<QuickJsEntry>& entry) {
  if (entry->queued || entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
  entry->queued = true;
  Queue(entry->owner.lock(), [entry] {
    entry->queued = false;
    if (entry->state != QuickJsState::Closed && entry->fault.empty()) Pump(entry);
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

// JS_SetPropertyStr consumes its value even when assigning it fails.
static void Set(JSContext* context, JSValueConst object, const char* key, JSValue value) {
  if (JS_IsException(value) || JS_SetPropertyStr(context, object, key, value) < 0) {
    JS_FreeValue(context, JS_GetException(context));
    throw std::runtime_error("Unable to create QuickJS service inputs");
  }
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

static std::shared_ptr<QuickJsEntry> Current(JSContext* context) {
  const auto raw = static_cast<QuickJsEntry*>(JS_GetContextOpaque(context));
  if (!raw || raw->state == QuickJsState::Closed || !raw->fault.empty())
    throw std::runtime_error("QuickJS service is closed");
  return raw->self.lock();
}

static uint64_t Identity(JSContext* context, JSValueConst value) {
  const auto text = Text(context, value);
  uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !result || std::to_string(result) != text)
    throw std::invalid_argument("Invalid service call identity");
  return result;
}

static JSValue BridgeError(JSContext* context, const char* code, const char* message) {
  const auto error = JS_NewError(context);
  if (JS_IsException(error)) return error;
  const auto code_value = JS_NewString(context, code);
  if (JS_IsException(code_value)) { JS_FreeValue(context, error); return JS_EXCEPTION; }
  if (JS_SetPropertyStr(context, error, "code", code_value) < 0) {
    JS_FreeValue(context, error); return JS_EXCEPTION;
  }
  const auto message_value = JS_NewString(context, message);
  if (JS_IsException(message_value)) { JS_FreeValue(context, error); return JS_EXCEPTION; }
  if (JS_SetPropertyStr(context, error, "message", message_value) < 0) {
    JS_FreeValue(context, error); return JS_EXCEPTION;
  }
  return JS_Throw(context, error);
}

static std::string CallbackResult(const QuickJsCallback& request, const MuonServiceResult& result) {
  if (result.value && result.code.empty())
    return "{\"status\":\"ok\",\"value\":" + EncodeMuonServiceValue(request.signature->returns, *result.value) + "}";
  const auto string_type = MuonServiceType{MUON_SERVICE_VALUE_STRING, {}, {}};
  const auto code = result.code.empty() ? "callback-failed" : result.code;
  return "{\"status\":\"error\",\"code\":" + EncodeMuonServiceValue(string_type, {code}) +
      ",\"message\":" + EncodeMuonServiceValue(string_type, {result.message}) + "}";
}

static void SettleCallback(const std::shared_ptr<QuickJsEntry>& entry, uint64_t id, MuonServiceResult result) {
  if (entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
  const auto found = entry->callbacks.find(id);
  if (found == entry->callbacks.end()) return;
  const auto request = found->second;
  entry->callbacks.erase(found);
  try {
    auto* context = entry->engine->Context();
    const auto resolve = QuickJsOwnedValue(context, request.resolve);
    const auto reject = QuickJsOwnedValue(context, request.reject);
    std::string json;
    try { json = CallbackResult(request, result); }
    catch (const std::exception& error) {
      json = CallbackResult(request, {{}, "invalid-result", error.what()});
    }
    const auto value = QuickJsOwnedValue(context, Parse(entry, json));
    const auto ignored = QuickJsOwnedValue(context, entry->engine->Call(resolve.value, JS_UNDEFINED, {value.value}));
    Wake(entry);
  } catch (const std::exception& error) { Fault(entry, error.what()); }
}

static void CancelCallbacks(const std::shared_ptr<QuickJsEntry>& entry, uint64_t call_id) {
  std::vector<uint64_t> pending;
  for (const auto& [id, request] : entry->callbacks) if (request.call_id == call_id) pending.push_back(id);
  for (const auto id : pending)
    SettleCallback(entry, id, {{}, "cancelled", "Parent service invocation was cancelled"});
}

static JSValue RequestCallback(JSContext* context, JSValueConst, int count, JSValueConst* values) {
  try {
    if (count != 3) throw std::invalid_argument("Expected callback identity and arguments");
    const auto entry = Current(context);
    const auto call_id = Identity(context, values[0]);
    const auto token = Identity(context, values[1]);
    const auto found = entry->calls.find(call_id);
    if (entry->state != QuickJsState::Running || found == entry->calls.end() || (found->second->cancelled || found->second->returned))
      return BridgeError(context, "cancelled", "Service invocation is no longer active");
    const auto call = found->second;
    const auto signature = call->tokens.find(token);
    if (signature == call->tokens.end() || !call->callback)
      throw std::invalid_argument("Callback token does not belong to this invocation");
    const auto has_capacity = [&] {
      size_t pending = 0;
      for (const auto& [id, request] : entry->callbacks) { (void)id; pending += request.call_id == call_id; }
      return pending < 32 && entry->callbacks.size() < 128 && entry->next_callback < UINT64_MAX;
    };
    if (!has_capacity())
      return BridgeError(context, "resource-exhausted", "Service callback capacity exceeded");
    auto arguments = DecodeMuonServiceArguments(signature->second->parameters, Json(entry, values[2]));
    // JSON getters can reenter host functions, including this callback bridge.
    if (call->cancelled || call->returned || !entry->fault.empty() || entry->state != QuickJsState::Running)
      return BridgeError(context, "cancelled", "Service invocation is no longer active");
    if (!has_capacity())
      return BridgeError(context, "resource-exhausted", "Service callback capacity exceeded");
    JSValue functions[2] = {JS_UNDEFINED, JS_UNDEFINED};
    const auto promise = JS_NewPromiseCapability(context, functions);
    if (JS_IsException(promise)) return promise;
    const auto id = ++entry->next_callback;
    entry->callbacks.emplace(id, QuickJsCallback{call_id, signature->second, functions[0], functions[1]});
    Queue(entry->owner.lock(), [entry, call, id, token, signature = signature->second,
        arguments = std::move(arguments)]() mutable {
      if (entry->state == QuickJsState::Closed || !entry->fault.empty() || !entry->callbacks.contains(id)) return;
      if (call->cancelled || call->returned) { SettleCallback(entry, id, {{}, "cancelled", "Service invocation cancelled"}); return; }
      try {
        call->callback(token, *signature, std::move(arguments),
            [weak = std::weak_ptr<QuickJsEntry>(entry), id](MuonServiceResult result) {
              const auto entry = weak.lock();
              if (!entry || entry->state == QuickJsState::Closed) return;
              Queue(entry->owner.lock(), [entry, id, result = std::move(result)]() mutable {
                SettleCallback(entry, id, std::move(result));
              });
            });
      } catch (const std::exception& error) {
        Queue(entry->owner.lock(), [entry, id, message = std::string(error.what())] {
          SettleCallback(entry, id, {{}, "callback-failed", message});
        });
      }
    });
    return promise;
  } catch (const std::exception& error) { return BridgeError(context, "callback-failed", error.what()); }
}

static JSValue Emit(JSContext* context, JSValueConst, int count, JSValueConst* values) {
  try {
    if (count != 2) throw std::invalid_argument("Expected event name and value");
    const auto entry = Current(context);
    if (entry->state != QuickJsState::Running)
      return BridgeError(context, "unavailable", "Service is not running");
    const auto name = Text(context, values[0]);
    const auto found = entry->interface.events.find(name);
    if (found == entry->interface.events.end()) throw std::invalid_argument("Unknown service event");
    const auto json = Json(entry, values[1]);
    auto value = DecodeMuonServiceValue(found->second, json);
    const auto bytes = json.size();
    if (entry->state != QuickJsState::Running || !entry->fault.empty())
      return BridgeError(context, "unavailable", "Service is no longer running");
    if (entry->pending_events >= 256 || bytes > 8 * 1024 * 1024 - entry->pending_event_bytes)
      return BridgeError(context, "resource-exhausted", "Service event capacity exceeded");
    ++entry->pending_events;
    entry->pending_event_bytes += bytes;
    const auto owner = entry->owner.lock();
    Queue(owner, [owner, entry, name, value = std::move(value), bytes] {
      --entry->pending_events; entry->pending_event_bytes -= bytes;
      if (entry->state == QuickJsState::Running && entry->fault.empty() && owner->events)
        owner->events(entry->spec.id, entry->generation, name, value);
    });
    return JS_UNDEFINED;
  } catch (const std::exception& error) { return BridgeError(context, "invalid-result", error.what()); }
}

static void FreeTimer(JSContext* context, QuickJsTimer& timer) {
  JS_FreeValue(context, timer.function); timer.function = JS_UNDEFINED;
  for (const auto argument : timer.arguments) JS_FreeValue(context, argument);
  timer.arguments.clear();
}

static void RunTimer(const std::shared_ptr<QuickJsEntry>& entry, uint64_t id) {
  if (entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
  const auto found = entry->timers.find(id);
  if (found == entry->timers.end()) return;
  auto timer = std::move(found->second);
  entry->timers.erase(found);
  try {
    auto* context = entry->engine->Context();
    struct Cleanup { JSContext* context; QuickJsTimer& timer; ~Cleanup() { FreeTimer(context, timer); } } cleanup{context, timer};
    const auto ignored = QuickJsOwnedValue(context,
        entry->engine->Call(timer.function, JS_UNDEFINED, timer.arguments));
    Wake(entry);
  } catch (const std::exception& error) { Dispose(entry, error.what()); }
}

static JSValue SetTimeout(JSContext* context, JSValueConst, int count, JSValueConst* values) {
  try {
    const auto entry = Current(context);
    if (count < 1 || count > 34 || !JS_IsFunction(context, values[0]))
      throw std::invalid_argument("setTimeout requires a function and at most 32 arguments");
    if (entry->timers.size() >= 1024 || entry->next_timer == 9007199254740991ULL)
      return BridgeError(context, "resource-exhausted", "Service timer capacity exceeded");
    double delay = 0;
    if (count > 1 && JS_ToFloat64(context, &delay, values[1]) < 0) return JS_EXCEPTION;
    if (!std::isfinite(delay) || delay < 0) delay = 0;
    delay = std::min(delay, static_cast<double>(INT32_MAX));
    if (!entry->fault.empty() || entry->state == QuickJsState::Closed ||
        entry->timers.size() >= 1024 || entry->next_timer == 9007199254740991ULL)
      return BridgeError(context, "resource-exhausted", "Service timer capacity exceeded");
    const auto id = ++entry->next_timer;
    QuickJsTimer timer{JS_DupValue(context, values[0]), {}, {}};
    for (int index = 2; index < count; ++index) timer.arguments.push_back(JS_DupValue(context, values[index]));
    try {
      timer.cancel = entry->owner.lock()->platform.delay(std::chrono::milliseconds(static_cast<int64_t>(delay)),
          [weak = std::weak_ptr<QuickJsEntry>(entry), id] {
            if (const auto entry = weak.lock()) RunTimer(entry, id);
          });
      entry->timers.emplace(id, std::move(timer));
    } catch (...) { FreeTimer(context, timer); throw; }
    return JS_NewInt64(context, static_cast<int64_t>(id));
  } catch (const std::exception& error) { return BridgeError(context, "resource-exhausted", error.what()); }
}

static JSValue ClearTimeout(JSContext* context, JSValueConst, int count, JSValueConst* values) {
  try {
    if (count < 1) return JS_UNDEFINED;
    int64_t id = 0;
    if (JS_ToInt64(context, &id, values[0]) < 0) return JS_EXCEPTION;
    const auto entry = Current(context);
    const auto found = entry->timers.find(static_cast<uint64_t>(id));
    if (found != entry->timers.end()) {
      auto timer = std::move(found->second); entry->timers.erase(found);
      if (timer.cancel) timer.cancel();
      FreeTimer(context, timer);
    }
    return JS_UNDEFINED;
  } catch (const std::exception& error) { return BridgeError(context, "invalid-result", error.what()); }
}

static JSValue Log(JSContext* context, JSValueConst, int count, JSValueConst* values) {
  try {
    if (count != 2) throw std::invalid_argument("Expected log severity and message");
    int32_t level = 0;
    if (JS_ToInt32(context, &level, values[0]) < 0) return JS_EXCEPTION;
    if (level < MUON_LOG_LEVEL_DEBUG || level > MUON_LOG_LEVEL_FATAL)
      throw std::invalid_argument("Invalid log severity");
    const auto entry = Current(context);
    const auto message = Text(context, values[1]);
    entry->owner.lock()->platform.log(entry->spec.id, static_cast<muon_log_level>(level), message.substr(0, 16384));
    return JS_UNDEFINED;
  } catch (const std::exception& error) { return BridgeError(context, "invalid-result", error.what()); }
}

static void InstallBridge(const std::shared_ptr<QuickJsEntry>& entry) {
  auto* context = entry->engine->Context();
  JS_SetContextOpaque(context, entry.get());
  const auto global = QuickJsOwnedValue(context, JS_GetGlobalObject(context));
  for (const auto& [name, function] : std::map<std::string, JSCFunction*>{
      {"__muonServiceCallback", RequestCallback}, {"__muonServiceEmit", Emit},
      {"__muonServiceLog", Log}, {"setTimeout", SetTimeout}, {"clearTimeout", ClearTimeout}}) {
    if (JS_SetPropertyStr(context, global.value, name.c_str(), JS_NewCFunction(context, function, name.c_str(), 3)) < 0)
      throw std::runtime_error("Unable to register service host functions");
  }
  const auto ignored = QuickJsOwnedValue(context, entry->engine->Evaluate(R"(
    globalThis.console = Object.freeze(Object.fromEntries(
      [['debug',0],['log',1],['info',1],['warn',2],['error',3]].map(([name,level]) =>
        [name, (...args) => __muonServiceLog(level, args.map(String).join(' '))])));
  )", "<service-host>", JS_EVAL_TYPE_GLOBAL));
}

static void CollectTokens(const MuonServiceType& type, const MuonServiceValue& value,
                           std::map<uint64_t, const MuonServiceSignature*>& tokens) {
  if (type.kind == MUON_SERVICE_VALUE_CALLBACK) {
    if (tokens.size() >= 1024 || !tokens.emplace(std::get<MuonServiceCallback>(value.data).token,
        type.callback.get()).second) throw std::invalid_argument("Duplicate or excessive service callback tokens");
  } else if (type.kind == MUON_SERVICE_VALUE_ARRAY) {
    for (const auto& item : std::get<MuonServiceValue::Array>(value.data)) CollectTokens(*type.element, item, tokens);
  }
}

static void Deadline(const std::shared_ptr<QuickJsEntry>& entry) {
  if (entry->cancel_deadline) entry->cancel_deadline();
  const auto owner = entry->owner.lock();
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
      entry->lifecycle_deadline - std::chrono::steady_clock::now());
  if (remaining.count() <= 0) throw std::runtime_error("QuickJS service lifecycle deadline exceeded");
  entry->cancel_deadline = owner->platform.delay(remaining,
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
  if (entry->extension.detach) entry->extension.detach();
  entry->extension.detach = nullptr;
  entry->extension.load_module = nullptr;
  if (entry->engine) {
    auto* context = entry->engine->Context();
    for (auto& [id, timer] : entry->timers) {
      (void)id;
      if (timer.cancel) timer.cancel();
      FreeTimer(context, timer);
    }
    entry->timers.clear();
    for (const auto& [id, request] : entry->callbacks) {
      (void)id;
      JS_FreeValue(context, request.resolve);
      JS_FreeValue(context, request.reject);
    }
    entry->callbacks.clear();
    for (const auto& pending : entry->pending) JS_FreeValue(context, pending.promise);
    entry->pending.clear();
    JS_FreeValue(context, entry->runtime);
    JS_FreeValue(context, entry->getter);
    JS_FreeValue(context, entry->stringify);
    entry->runtime = entry->getter = entry->stringify = JS_UNDEFINED;
    entry->engine.reset();
  }
  owner->entries.erase(entry->spec.id);
  // Keep old generations separately so a fault can admit a replacement without
  // losing the old native drain or completing a repeated stop prematurely.
  const auto identity = std::make_pair(entry->spec.id, entry->generation);
  owner->closing.emplace(identity, entry);
  if (was_starting) {
    auto complete = std::move(entry->started);
    entry->started = nullptr;
    if (complete) complete(error.empty() ? "Service startup interrupted" : error);
  } else if (!error.empty()) {
    owner->platform.exited(entry->spec.id, entry->generation, error);
  }
  auto drained = [owner, entry, identity] {
    Queue(owner, [owner, entry, identity] {
      if (!owner->closing.erase(identity)) return;
      entry->extension.close = nullptr;
      auto stopped = std::move(entry->stopped);
      entry->stopped.clear();
      for (const auto& complete : stopped) complete();
    });
  };
  if (entry->extension.close) entry->extension.close(std::move(drained));
  else drained();
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
        if (entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
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
      found->second->state != QuickJsState::Running || !found->second->fault.empty())
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
bool MuonQuickJsServiceHost::IsIdle() const {
  return impl_->entries.empty() && impl_->closing.empty() && !impl_->queued;
}

static JSModuleDef* LoadModule(JSContext* context, const char* name, void* opaque) {
  const auto entry = static_cast<QuickJsEntry*>(opaque);
  try {
    if (entry->state == QuickJsState::Closed || !entry->fault.empty() || !entry->extension.load_module)
      throw std::invalid_argument("Service host module is unavailable");
    return entry->extension.load_module(context, name);
  } catch (const std::exception& error) {
    JS_ThrowReferenceError(context, "%s", error.what());
    return nullptr;
  }
}

void MuonQuickJsServiceHost::Start(const MuonQuickJsServiceSpec& spec, uint64_t generation,
                                 std::function<void(const std::string&)> complete) {
  if (spec.id.empty() || !generation || impl_->entries.contains(spec.id) ||
      impl_->closing.contains({spec.id, generation}))
    throw std::invalid_argument("Invalid or duplicate QuickJS service generation");
  auto entry = std::make_shared<QuickJsEntry>();
  entry->self = entry;
  entry->owner = impl_; entry->spec = spec; entry->generation = generation;
  entry->lifecycle_deadline = std::min(std::chrono::steady_clock::now() + impl_->platform.shutdown_timeout,
      spec.startup_deadline.value_or(std::chrono::steady_clock::time_point::max()));
  entry->started = std::move(complete);
  impl_->entries.emplace(spec.id, entry);
  Queue(impl_, [entry] {
    try {
      entry->engine = std::make_unique<MuonQuickJsEngine>(entry->owner.lock()->platform.limits,
          [weak = std::weak_ptr<QuickJsEntry>(entry)] {
            const auto entry = weak.lock();
            return !entry || ((entry->state == QuickJsState::Starting || entry->state == QuickJsState::Stopping) &&
                std::chrono::steady_clock::now() >= entry->lifecycle_deadline);
          });
      entry->getter = entry->engine->Evaluate("(object, key) => object[key]", "<service-host>", JS_EVAL_TYPE_GLOBAL);
      entry->stringify = entry->engine->Evaluate("JSON.stringify", "<service-host>", JS_EVAL_TYPE_GLOBAL);
      InstallBridge(entry);
      if (const auto owner = entry->owner.lock(); owner->platform.attach) {
        const auto weak = std::weak_ptr<QuickJsEntry>(entry);
        entry->extension = owner->platform.attach(entry->spec, entry->generation, *entry->engine,
            [weak] { if (const auto entry = weak.lock()) Wake(entry); },
            [weak](const std::string& error) { if (const auto entry = weak.lock()) Fault(entry, error); });
      }
      JS_SetModuleLoaderFunc(entry->engine->Runtime(), nullptr, LoadModule, entry.get());
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
              Set(context, input.value, "id", JS_NewString(context, entry->spec.id.c_str()));
              Set(context, input.value, "generation", JS_NewString(context, std::to_string(entry->generation).c_str()));
              Set(context, input.value, "dataDirectory", JS_NewString(context, entry->spec.data_directory.c_str()));
              const auto config = QuickJsOwnedValue(context, JS_NewObjectProto(context, JS_NULL));
              for (const auto& [key, value] : entry->spec.config)
                Set(context, config.value, key.c_str(), JS_NewStringLen(context, value.data(), value.size()));
              Set(context, input.value, "config", JS_DupValue(context, config.value));
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
  if (const auto closing = impl_->closing.find({id, generation}); closing != impl_->closing.end()) {
    closing->second->stopped.push_back(std::move(complete));
    return;
  }
  const auto found = impl_->entries.find(id);
  if (found == impl_->entries.end() || found->second->generation != generation) {
    Queue(impl_, std::move(complete)); return;
  }
  const auto entry = found->second;
  if (entry->state == QuickJsState::Starting) throw std::logic_error("Stop during QuickJS startup");
  entry->stopped.push_back(std::move(complete));
  if (entry->state == QuickJsState::Stopping || !entry->fault.empty()) return;
  entry->state = QuickJsState::Stopping;
  entry->lifecycle_deadline = std::chrono::steady_clock::now() + impl_->platform.shutdown_timeout;
  Queue(impl_, [entry] {
    if (entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
    try {
      Deadline(entry);
      const auto reason = QuickJsOwnedValue(entry->engine->Context(), JS_NewString(entry->engine->Context(), "Service stopped"));
      for (const auto& [id, call] : entry->calls) {
        (void)id;
        call->cancelled = true;
        Complete(entry, call, {{}, "cancelled", "Service stopped"});
      }
      Await(entry, Call(entry, "stop", {reason.value}), [entry](const std::string&, const std::string& error) {
        Dispose(entry, error);
      });
      for (const auto& [id, call] : entry->calls) { (void)call; CancelCallbacks(entry, id); }
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
      const auto call = std::make_shared<QuickJsCall>();
      call->id = ++owner->next_call;
      call->method = &signature;
      call->callback = std::move(callback);
      call->complete = std::move(complete);
      for (size_t index = 0; index < arguments.size(); ++index)
        CollectTokens(signature.signature.parameters[index], arguments[index], call->tokens);
      entry->calls.emplace(call->id, call);
      Queue(owner, [entry, call, method, json] {
        if (entry->state == QuickJsState::Closed || !entry->fault.empty()) return;
        if (call->cancelled || entry->state != QuickJsState::Running) {
          Complete(entry, call, {{}, "cancelled", "Service invocation cancelled before dispatch"});
          entry->calls.erase(call->id);
          return;
        }
        try {
          auto* context = entry->engine->Context();
          const auto id = QuickJsOwnedValue(context, JS_NewString(context, std::to_string(call->id).c_str()));
          const auto name = QuickJsOwnedValue(context, JS_NewString(context, method.c_str()));
          const auto arguments = QuickJsOwnedValue(context, Parse(entry, json));
          Await(entry, Call(entry, "invoke", {id.value, name.value, arguments.value}),
              [entry, call](const std::string& json, const std::string& error) {
                call->returned = true;
                const auto unfinished = std::any_of(entry->callbacks.begin(), entry->callbacks.end(),
                    [&](const auto& item) { return item.second.call_id == call->id; });
                CancelCallbacks(entry, call->id);
                try {
                  if (unfinished) throw std::runtime_error("Service completed with callbacks still outstanding");
                  if (!error.empty()) throw std::runtime_error(error);
                  Complete(entry, call, Result(*call->method, json));
                } catch (const std::exception& failure) {
                  Complete(entry, call, {{}, "invalid-result", failure.what()});
                }
                entry->calls.erase(call->id);
              });
        } catch (const std::exception& error) {
          // Runtime-level failures can strand private SDK promises. Dispose
          // this generation and settle every outstanding call exactly once.
          // Ordinary author errors arrive through the encoded result above.
          Fault(entry, error.what());
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
          CancelCallbacks(entry, identity);
          Wake(entry);
        } catch (const std::exception& error) { Dispose(entry, error.what()); }
      });
    },
    [owner](MuonServiceEventSink sink) { owner->events = std::move(sink); }
  };
}
