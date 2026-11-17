/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_broker.h"
#include "services/muon_native_service_host.h"
#include <yyjson.h>

#include <cassert>
#include <deque>
#include <iostream>

struct NativeCall {
  const muon_service_value* arguments;
  muon_service_result_completion complete;
  void* data;
  bool cancelled = false;
};
static std::map<uint64_t, NativeCall> native_calls;
static const muon_service_context* service;
static uint64_t latest_native_call = 0;
static int stops = 0;

static void Complete(uint64_t id, const muon_service_value* value, const muon_service_error* error) {
  const auto call = native_calls.at(id);
  native_calls.erase(id);
  call.complete(call.data, value, error);
}

static void CallbackDone(void* data, const muon_service_value* value, const muon_service_error* error) {
  const auto id = *static_cast<uint64_t*>(data);
  delete static_cast<uint64_t*>(data);
  Complete(id, value, error);
}

static void Invoke(void*, const muon_service_call_context* context, const char* method,
    const muon_service_value* values, size_t count, muon_service_result_completion complete, void* data) {
  assert(count == 1);
  latest_native_call = context->call_id;
  if (std::string(method) == "echo") {
    complete(data, values, nullptr);
    assert(values[0].kind == MUON_SERVICE_VALUE_STRING);
    return;
  }
  native_calls.emplace(context->call_id, NativeCall{values, complete, data, false});
  if (std::string(method) == "work") {
    auto argument = muon_service_value{};
    argument.kind = MUON_SERVICE_VALUE_STRING;
    argument.data.string_value = {"hello", 5};
    auto* identity = new uint64_t(context->call_id);
    assert(context->callback(context->user_data, values[0].data.callback_id,
        &argument, 1, &CallbackDone, identity));
  }
}

static void Cancel(void*, uint64_t id) { native_calls.at(id).cancelled = true; }
static void Stop(void*, muon_service_stop_completion complete, void* data) {
  assert(native_calls.empty());
  ++stops;
  complete(data);
}
static void Start(const muon_service_context* context, muon_service_start_completion complete, void* data) {
  service = context;
  const auto instance = muon_service_instance{nullptr, &Stop, &Invoke, &Cancel};
  complete(data, &instance, nullptr);
}

using Reply = std::shared_ptr<std::optional<std::string>>;

static Reply Request(MuonServiceBroker& broker, const MuonServiceReference& reference,
    const std::string& action, const std::string& payload) {
  auto result = std::make_shared<std::optional<std::string>>();
  broker.Request(reference, action, payload, [result](const std::string& json) {
    assert(!result->has_value());
    *result = json;
  });
  return result;
}

static std::string Field(const Reply& result, const char* name) {
  assert(result->has_value());
  const auto& json = result->value();
  const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  auto* value = yyjson_obj_get(yyjson_doc_get_root(doc.get()), name);
  assert(yyjson_is_str(value));
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static std::string Call(const std::string& id) { return "{\"callId\":\"" + id + "\"}"; }
static std::string Subscription(const std::string& id) { return "{\"subscriptionId\":\"" + id + "\"}"; }

template <typename Action>
static void Reject(Action action) {
  auto rejected = false;
  try { action(); } catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
}

int main() {
  static const auto factory = muon_service_factory{MUON_SERVICE_ABI_VERSION, &Start,
    R"({"schemaVersion":1,"methods":{
      "echo":{"parameters":["string"],"returns":"string","errors":[]},
      "hold":{"parameters":["string"],"returns":"string","errors":[]},
      "work":{"parameters":[{"callback":{"parameters":["string"],"returns":"uint64"}}],"returns":"uint64","errors":[]}
    },"events":{"tick":"int32"}})"};
  auto tasks = std::deque<std::function<void()>>{};
  const auto post = [&](std::function<void()> task) { tasks.push_back(std::move(task)); };
  const auto pump = [&] {
    while (!tasks.empty()) {
      auto task = std::move(tasks.front());
      tasks.pop_front();
      task();
    }
  };
  auto host = MuonNativeServiceHost({
    [](const std::string&, std::string&) {
      return std::shared_ptr<const muon_service_factory>(&factory, [](const muon_service_factory*) {});
    }, post, [](const std::string&, muon_log_level, const std::string&) {}});
  auto controller = MuonServiceController({{"worker", true, std::nullopt}}, {
    [&](const MuonServiceSnapshot& state, std::function<void(const std::string&)> complete) {
      host.Start({state.id, "library", "/private/worker", {}}, state.generation, std::move(complete));
    },
    [&](const MuonServiceSnapshot& state, std::function<void()> complete) {
      host.Stop(state.id, state.generation, std::move(complete));
    },
    [](const std::string&, bool, std::function<void(const std::string&)> complete) { complete({}); },
    [](const MuonServiceEvent&) {}, post});
  auto broker = MuonServiceBroker(controller, host.Execution());
  controller.Activate();
  pump();
  const auto a = controller.Acquire("worker", "page-a").value();
  const auto a2 = controller.Acquire("worker", "page-a").value();
  const auto b = controller.Acquire("worker", "page-b").value();
  pump();
  assert(controller.IsReady(a) && controller.IsReady(a2) && controller.IsReady(b));
  const auto describe = Request(broker, a, "describe", "{}");
  assert(describe->value().find("\"echo\"") != std::string::npos);
  assert(describe->value().find('\n') == std::string::npos);
  auto forged = a;
  forged.owner_id = "page-b";
  Reject([&] { Request(broker, forged, "describe", "{}"); });
  Reject([&] { Request(broker, a, "begin", R"({"method":"absent","arguments":[]})"); });
  Reject([&] { Request(broker, a, "begin", R"({"method":"echo","arguments":[1]})"); });
  const auto echo = Field(Request(broker, a, "begin", R"({"method":"echo","arguments":["hello"]})"), "callId");
  pump();
  assert(Field(Request(broker, a, "result", Call(echo)), "value") == "hello");
  assert(Request(broker, a, "callbackNext", Call(echo))->value() == "{\"done\":true}");

  const auto outer = Field(Request(broker, a, "begin", R"({"method":"work","arguments":["9"]})"), "callId");
  const auto outer_result = Request(broker, a, "result", Call(outer));
  const auto outer_callback = Request(broker, a, "callbackNext", Call(outer));
  pump();
  assert(Field(outer_callback, "token") == "9");
  const auto outer_wait = Request(broker, a, "callbackNext", Call(outer));
  Reject([&] { Request(broker, a, "callbackNext", Call(outer)); });
  const auto inner = Field(Request(broker, b, "begin", R"({"method":"work","arguments":["9"]})"), "callId");
  const auto inner_result = Request(broker, b, "result", Call(inner));
  const auto inner_callback = Request(broker, b, "callbackNext", Call(inner));
  pump();
  const auto respond = [&](const MuonServiceReference& reference, const std::string& id,
      const Reply& callback, const std::string& value) {
    return Request(broker, reference, "callbackResult", "{\"callId\":\"" + id +
        "\",\"callbackId\":\"" + Field(callback, "callbackId") +
        "\",\"result\":{\"status\":\"ok\",\"value\":\"" + value + "\"}}");
  };
  respond(b, inner, inner_callback, "18446744073709551615");
  pump();
  assert(Field(inner_result, "value") == "18446744073709551615" && !outer_result->has_value());
  respond(a, outer, outer_callback, "7");
  pump();
  assert(Field(outer_result, "value") == "7" && outer_wait->value() == "{\"done\":true}");
  assert(respond(a, outer, outer_callback, "8")->value() == "null");

  const auto held = Field(Request(broker, a, "begin", R"({"method":"hold","arguments":["held"]})"), "callId");
  const auto held_native = latest_native_call;
  const auto held_result = Request(broker, a, "result", Call(held));
  Reject([&] { Request(broker, a, "result", Call(held)); });
  const auto unrelated = Field(Request(broker, a2, "begin", R"({"method":"hold","arguments":["other"]})"), "callId");
  const auto unrelated_native = latest_native_call;
  const auto unrelated_result = Request(broker, a2, "result", Call(unrelated));
  Request(broker, a, "cancel", Call(held));
  pump();
  assert(Field(held_result, "code") == "cancelled" && !unrelated_result->has_value());
  const auto late_value = native_calls.at(held_native).arguments[0];
  Complete(held_native, &late_value, nullptr);
  pump();
  assert(Field(held_result, "code") == "cancelled");
  const auto unrelated_value = native_calls.at(unrelated_native).arguments[0];
  Complete(unrelated_native, &unrelated_value, nullptr);
  pump();
  assert(Field(unrelated_result, "value") == "other");

  const auto subscription = Field(Request(broker, a2, "subscribe", R"({"event":"tick"})"), "subscriptionId");
  auto tick = muon_service_value{};
  tick.kind = MUON_SERVICE_VALUE_INT32;
  tick.data.int32_value = 1;
  assert(service->host->emit(service->host->user_data, "tick", &tick));
  tick.data.int32_value = 2;
  assert(service->host->emit(service->host->user_data, "tick", &tick));
  pump();
  assert(Request(broker, a2, "eventNext", Subscription(subscription))->value() == "{\"done\":false,\"value\":1}");
  assert(Request(broker, a2, "eventNext", Subscription(subscription))->value() == "{\"done\":false,\"value\":2}");
  const auto event_wait = Request(broker, a2, "eventNext", Subscription(subscription));
  Request(broker, a2, "unsubscribe", Subscription(subscription));
  assert(event_wait->value() == "{\"done\":true}");
  const auto overflowing = Field(Request(broker, a2, "subscribe", R"({"event":"tick"})"), "subscriptionId");
  for (auto index = 0; index != 65; ++index)
    assert(service->host->emit(service->host->user_data, "tick", &tick));
  pump();
  const auto overflow = Request(broker, a2, "eventNext", Subscription(overflowing));
  assert(overflow->value().find("resource-exhausted") != std::string::npos);

  const auto abandoned = Field(Request(broker, a, "begin", R"({"method":"work","arguments":["9"]})"), "callId");
  const auto abandoned_result = Request(broker, a, "result", Call(abandoned));
  const auto abandoned_next = Request(broker, a, "callbackNext", Call(abandoned));
  broker.Release(a);
  controller.Release(a);
  pump();
  assert(Field(abandoned_result, "code") == "cancelled");
  assert(abandoned_next->value() == "{\"done\":true}");
  assert(controller.IsReady(a2) && controller.IsReady(b));
  Reject([&] { Request(broker, a, "describe", "{}"); });
  assert(native_calls.empty());

  auto retained = std::vector<std::string>{};
  for (auto index = 0; index < 128; ++index) {
    retained.push_back(Field(Request(broker, a2, "begin", R"({"method":"echo","arguments":["bounded"]})"), "callId"));
    pump();
  }
  Reject([&] { Request(broker, a2, "begin", R"({"method":"echo","arguments":["overflow"]})"); });
  for (const auto& id : retained)
    assert(Field(Request(broker, a2, "result", Call(id)), "value") == "bounded");
  const auto recovered = Field(Request(broker, a2, "begin", R"({"method":"echo","arguments":["recovered"]})"), "callId");
  pump();
  assert(Field(Request(broker, a2, "result", Call(recovered)), "value") == "recovered");
  const auto large = EncodeMuonServiceValue({MUON_SERVICE_VALUE_STRING, {}, {}},
      {std::string(1024 * 1024, '\0')});
  auto large_results = std::vector<std::string>{};
  for (auto index = 0; index < 3; ++index) {
    large_results.push_back(Field(Request(broker, a2, "begin",
        "{\"method\":\"echo\",\"arguments\":[" + large + "]}"), "callId"));
    pump();
  }
  assert(Field(Request(broker, a2, "result", Call(large_results[2])), "code") == "resource-exhausted");
  for (auto index = 0; index < 2; ++index)
    assert(Field(Request(broker, a2, "result", Call(large_results[index])), "value").size() == 1024 * 1024);

  const auto c = controller.Acquire("worker", "page-c").value();
  pump();
  const auto close_callback_call = Field(Request(broker, c, "begin", R"({"method":"work","arguments":["9"]})"), "callId");
  const auto close_callback_result = Request(broker, c, "result", Call(close_callback_call));
  auto callback_closed = false;
  broker.Request(c, "callbackNext", Call(close_callback_call), [&](const std::string&) {
    callback_closed = true;
    broker.CloseOwner("page-c");
  });
  pump();
  assert(callback_closed && Field(close_callback_result, "code") == "cancelled" && native_calls.empty());
  const auto close_result_call = Field(Request(broker, b, "begin", R"({"method":"echo","arguments":["done"]})"), "callId");
  auto result_closed = false;
  broker.Request(b, "result", Call(close_result_call), [&](const std::string& result) {
    assert(result.find("done") != std::string::npos);
    result_closed = true;
    broker.CloseOwner("page-b");
  });
  pump();
  assert(result_closed && controller.IsReady(a2) && !controller.IsReady(b));
  const auto b2 = controller.Acquire("worker", "page-b").value();
  pump();
  const auto first_subscription = Field(Request(broker, a2, "subscribe", R"({"event":"tick"})"), "subscriptionId");
  const auto second_subscription = Field(Request(broker, b2, "subscribe", R"({"event":"tick"})"), "subscriptionId");
  auto shutdown_from_event = false;
  auto invalidated_callback = Reply{};
  auto invalidated_call_result = Reply{};
  broker.Request(a2, "eventNext", Subscription(first_subscription), [&](const std::string&) {
    shutdown_from_event = true;
    const auto call = Field(Request(broker, b2, "begin", R"({"method":"work","arguments":["9"]})"), "callId");
    invalidated_callback = Request(broker, b2, "callbackNext", Call(call));
    invalidated_call_result = Request(broker, b2, "result", Call(call));
    controller.Shutdown("OS stop during event delivery");
  });
  const auto invalidated_event = Request(broker, b2, "eventNext", Subscription(second_subscription));
  assert(service->host->emit(service->host->user_data, "tick", &tick));
  pump();
  assert(shutdown_from_event && !invalidated_event->has_value());
  assert(invalidated_callback->value() == "{\"done\":true}");
  assert(Field(invalidated_call_result, "code") == "cancelled");
  broker.CloseOwner("page-a");
  controller.ReleaseOwner("page-a");
  broker.CloseOwner("page-b");
  controller.ReleaseOwner("page-b");
  broker.Shutdown();
  assert(invalidated_event->value() == "{\"done\":true}");
  controller.Shutdown("test complete");
  pump();
  assert(stops == 1 && host.IsIdle() && controller.IsQuiescent());
  std::cout << "Service RPC ownership, callback routing, cancellation, and bounded events passed\n";
}
