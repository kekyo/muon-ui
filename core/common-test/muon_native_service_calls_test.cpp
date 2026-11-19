/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_native_service_host.h"

#include <cassert>
#include <deque>
#include <iostream>
#include <map>

struct Pending {
  const muon_service_call_context* context;
  const muon_service_value* arguments;
  muon_service_result_completion complete;
  void* data;
  int cancellations = 0;
};
static std::map<uint64_t, Pending> pending;
static const muon_service_context* service;
static int stops = 0;
static bool library_alive = false;
static bool invalid_instance = false;

static void Invoke(void*, const muon_service_call_context* context, const char*,
    const muon_service_value* arguments, size_t count,
    muon_service_result_completion complete, void* data) {
  assert(count == 2);
  pending.emplace(context->call_id, Pending{context, arguments, complete, data, 0});
}

static void Cancel(void*, uint64_t id) { ++pending.at(id).cancellations; }
static void Stop(void*, const muon_service_stop_info*, muon_service_stop_completion complete, void* data) {
  assert(pending.empty());
  ++stops;
  complete(data);
  assert(library_alive);
}
static void Start(const muon_service_context* context,
    muon_service_start_completion complete, void* data) {
  service = context;
  const auto instance = muon_service_instance{nullptr, &Stop, invalid_instance ? nullptr : &Invoke, &Cancel};
  complete(data, &instance, nullptr);
}

static void Finish(uint64_t id, const muon_service_value* value,
                   const muon_service_error* error) {
  const auto item = pending.at(id);
  pending.erase(id);
  item.complete(item.data, value, error);
  assert(library_alive);
}

int main() {
  static const auto schema = R"({"schemaVersion":1,"methods":{"work":{
    "parameters":["bytes",{"callback":{"parameters":["string"],"returns":"uint64"}}],
    "returns":"bytes","errors":["unavailable"]}},"events":{"progress":"int32"}})";
  const auto factory = muon_service_factory{MUON_SERVICE_ABI_VERSION, &Start, schema};
  auto queue = std::deque<std::function<void()>>{};
  const auto pump = [&] {
    while (!queue.empty()) {
      auto task = std::move(queue.front());
      queue.pop_front();
      task();
    }
  };
  auto host = MuonNativeServiceHost({
    [&](const std::string&, std::string&) {
      library_alive = true;
      return std::shared_ptr<const muon_service_factory>(&factory,
          [](const muon_service_factory*) { library_alive = false; });
    },
    [&](std::function<void()> task) { queue.push_back(std::move(task)); },
    [](const std::string&, muon_log_level, const std::string&) {}});
  auto started = false;
  host.Start({"worker", "library", "/private/worker", {}}, 7,
      [&](const std::string& error) { assert(error.empty()); started = true; });
  pump();
  assert(started && host.Interface("worker", 7).methods.contains("work"));
  auto results = std::vector<MuonServiceResult>{};
  auto callbacks = std::vector<MuonServiceCallCompletion>{};
  const auto on_callback = [&](uint64_t token, const MuonServiceSignature& signature,
      std::vector<MuonServiceValue> arguments, MuonServiceCallCompletion complete) {
    assert(token == 9 && signature.returns.kind == MUON_SERVICE_VALUE_UINT64);
    assert(std::get<std::string>(arguments.at(0).data) == "hello");
    callbacks.push_back(std::move(complete));
  };
  const auto on_result = [&](MuonServiceResult result) { results.push_back(std::move(result)); };
  auto source = MuonServiceValue::Bytes{0, 127, 255};
  const auto first = host.Invoke("worker", 7, "work", {{source}, {MuonServiceCallback{9}}}, on_callback, on_result);
  source[0] = 99;
  assert(pending.at(first).arguments[0].data.bytes_value.data[0] == 0);
  auto callback_calls = 0;
  const auto callback_done = [](void* data, const muon_service_value* value, const muon_service_error* error) {
    assert(!error && value && value->data.uint64_value == UINT64_MAX);
    ++*static_cast<int*>(data);
  };
  auto hello = std::string("hello");
  auto callback_argument = muon_service_value{};
  callback_argument.kind = MUON_SERVICE_VALUE_STRING;
  callback_argument.data.string_value = {hello.data(), hello.size()};
  const auto context = pending.at(first).context;
  context->callback(context->user_data, 9, &callback_argument, 1, callback_done, &callback_calls);
  hello[0] = 'X';
  assert(callbacks.empty());
  pump();
  assert(callbacks.size() == 1);
  callbacks[0]({MuonServiceValue{UINT64_MAX}, {}, {}});
  pump();
  assert(callback_calls == 1);
  auto bytes = MuonServiceValue::Bytes{1, 2, 3};
  auto result = muon_service_value{};
  result.kind = MUON_SERVICE_VALUE_BYTES;
  result.data.bytes_value = {bytes.data(), bytes.size()};
  Finish(first, &result, nullptr);
  bytes[0] = 99;
  assert(results.empty());
  pump();
  assert(results.size() == 1 && std::get<MuonServiceValue::Bytes>(results[0].value->data)[0] == 1);

  auto events = 0;
  host.SetEventSink([&](const std::string& id, uint64_t generation,
      const std::string& name, const MuonServiceValue& value) {
    assert(id == "worker" && generation == 7 && name == "progress");
    assert(std::get<int32_t>(value.data) == 12);
    ++events;
  });
  auto progress = muon_service_value{};
  progress.kind = MUON_SERVICE_VALUE_INT32;
  progress.data.int32_value = 12;
  assert(service->host->emit(service->host->user_data, "progress", &progress));
  assert(!service->host->emit(service->host->user_data, "missing", &progress));
  assert(!service->host->emit(service->host->user_data, "progress", &result));
  pump();
  assert(events == 1);
  for (auto index = 0; index < 256; ++index)
    assert(service->host->emit(service->host->user_data, "progress", &progress));
  assert(!service->host->emit(service->host->user_data, "progress", &progress));
  pump();
  assert(events == 257);
  assert(service->host->emit(service->host->user_data, "progress", &progress));
  pump();
  assert(events == 258);

  const auto cancelled = host.Invoke("worker", 7, "work", {{source}, {MuonServiceCallback{9}}}, on_callback, on_result);
  const auto other = host.Invoke("worker", 7, "work", {{source}, {MuonServiceCallback{9}}}, on_callback, on_result);
  auto callback_cancelled = 0;
  const auto cancellation_done = [](void* data, const muon_service_value* value, const muon_service_error* error) {
    assert(!value && error && std::string(error->code) == "cancelled");
    ++*static_cast<int*>(data);
  };
  hello = "hello";
  callback_argument.data.string_value = {hello.data(), hello.size()};
  const auto cancel_context = pending.at(cancelled).context;
  cancel_context->callback(cancel_context->user_data, 9, &callback_argument, 1,
      cancellation_done, &callback_cancelled);
  pump();
  assert(callbacks.size() == 2);
  host.Cancel("worker", 8, cancelled);
  assert(pending.at(cancelled).cancellations == 0);
  host.Cancel("worker", 7, cancelled);
  host.Cancel("worker", 7, cancelled);
  pump();
  assert(callback_cancelled == 1 && pending.at(cancelled).cancellations == 1);
  assert(results.size() == 2 && results.back().code == "cancelled");
  callbacks[1]({MuonServiceValue{uint64_t{99}}, {}, {}});
  pump();
  assert(callback_cancelled == 1);
  Finish(other, &result, nullptr);
  pump();
  assert(results.size() == 3 && results.back().code.empty());
  auto stopped = false;
  host.Stop("worker", 7, [&] { stopped = true; });
  pump();
  assert(stops == 0 && !stopped && library_alive);
  assert(pending.at(cancelled).cancellations == 1);
  assert(!service->host->emit(service->host->user_data, "progress", &progress));
  const auto failure = muon_service_error{"cancelled", "cancelled by owner"};
  Finish(cancelled, nullptr, &failure);
  pump();
  assert(results.size() == 3);
  assert(stopped && stops == 1);
  assert(!library_alive && host.IsIdle());
  host.Start({"worker", "library", "/private/worker", {}}, 8,
      [](const std::string& error) { assert(error.empty()); });
  pump();
  const auto bounded = host.Invoke("worker", 8, "work", {{source}, {MuonServiceCallback{9}}}, on_callback, on_result);
  const auto bounded_context = pending.at(bounded).context;
  auto bounded_completions = 0;
  for (auto index = 0; index < 32; ++index) {
    assert(bounded_context->callback(bounded_context->user_data, 9, &callback_argument, 1,
        cancellation_done, &bounded_completions));
  }
  assert(!bounded_context->callback(bounded_context->user_data, 9, &callback_argument, 1,
      cancellation_done, &bounded_completions));
  host.Stop("worker", 8, [] {});
  pump();
  assert(bounded_completions == 32 && stops == 1 && pending.at(bounded).cancellations == 1);
  assert(!bounded_context->callback(bounded_context->user_data, 9, &callback_argument, 1,
      cancellation_done, &bounded_completions));
  Finish(bounded, nullptr, &failure);
  pump();
  assert(stops == 2 && host.IsIdle());
  invalid_instance = true;
  auto rejected_start = false;
  host.Start({"worker", "library", "/private/worker", {}}, 9,
      [&](const std::string& error) {
        assert(!error.empty() && stops == 3);
        rejected_start = true;
      });
  pump();
  assert(rejected_start && !library_alive && host.IsIdle());
  std::cout << "Native service calls, callback ownership, and asynchronous drain passed\n";
}
