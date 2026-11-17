/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#ifdef _WIN32
#include <windows.h>
#endif
#include "services/muon_quickjs_service_host.h"
#include "quickjs-service-fixture.h"
#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>

int main() {
  std::deque<std::function<void()>> tasks;
  struct Timer { std::chrono::milliseconds delay; std::function<void()> task; };
  std::map<uint64_t, Timer> timers;
  uint64_t next_timer = 0;
  const auto post = [&](std::function<void()> task) { tasks.push_back(std::move(task)); };
  const auto pump = [&] {
    size_t turns = 0;
    while (!tasks.empty()) {
      assert(++turns < 1000);
      auto task = std::move(tasks.front()); tasks.pop_front(); task();
    }
  };
  MuonQuickJsServiceHost host({post,
    [&](std::chrono::milliseconds delay, std::function<void()> task) {
      const auto id = ++next_timer;
      timers.emplace(id, Timer{delay, std::move(task)});
      return std::function<void()>([&, id] { timers.erase(id); });
    }, [](const std::string&, muon_log_level, const std::string&) {},
    [](const std::string&, uint64_t, const std::string&) { assert(false); },
    {64 * 1024 * 1024, 1024 * 1024, std::chrono::milliseconds(2000)}, std::chrono::seconds(5)});
  bool started = false;
  std::string startup_error;
  host.Start({"sdk", "fixture.mjs", kMuonQuickJsServiceFixture, "/private", {}}, 7,
      [&](const std::string& error) { startup_error = error; started = error.empty(); });
  pump();
  if (!startup_error.empty()) std::cerr << startup_error << '\n';
  assert(started);
  const auto execution = host.Execution();
  std::vector<MuonServiceResult> results;
  const auto collect = [&](MuonServiceResult result) { results.push_back(std::move(result)); };
  const auto callback = [](uint64_t token, const MuonServiceSignature& signature,
      std::vector<MuonServiceValue> args, MuonServiceCallCompletion complete) {
    assert(token == 41 && signature.parameters.size() == 1);
    complete({MuonServiceValue{std::get<std::string>(args[0].data) + " callback"}, {}, {}});
  };
  static constexpr char text[] = "日本語😀\0tail";
  const auto message = std::string(text, sizeof(text) - 1);
  execution.invoke("sdk", 7, "echo", {{message}}, callback, collect);
  execution.invoke("sdk", 7, "echoBytes", {{MuonServiceValue::Bytes(1024 * 1024, 0xa5)}}, callback, collect);
  execution.invoke("sdk", 7, "echoInt64", {{int64_t{INT64_MIN}}}, callback, collect);
  execution.invoke("sdk", 7, "echoUint64", {{uint64_t{UINT64_MAX}}}, callback, collect);
  execution.invoke("sdk", 7, "callback", {{message}, {MuonServiceCallback{41}}}, callback, collect);
  std::vector<int32_t> events;
  execution.set_event_sink([&](const std::string& id, uint64_t generation, const std::string& event,
      const MuonServiceValue& value) {
    assert(id == "sdk" && generation == 7 && event == "progress");
    events.push_back(std::get<int32_t>(value.data));
  });
  execution.invoke("sdk", 7, "emit", {{int32_t{42}}}, callback, collect);
  pump();
  assert(results.size() == 6 && events == std::vector<int32_t>{42});
  bool found_callback = false, found_bytes = false, found_signed = false, found_unsigned = false;
  for (const auto& result : results) {
    if (!result.value) std::cerr << result.code << ": " << result.message << '\n';
    assert(result.value);
    if (const auto value = std::get_if<std::string>(&result.value->data))
      found_callback |= *value == message + " callback";
    if (const auto value = std::get_if<MuonServiceValue::Bytes>(&result.value->data))
      found_bytes |= *value == MuonServiceValue::Bytes(1024 * 1024, 0xa5);
    if (const auto value = std::get_if<int64_t>(&result.value->data)) found_signed |= *value == INT64_MIN;
    if (const auto value = std::get_if<uint64_t>(&result.value->data)) found_unsigned |= *value == UINT64_MAX;
  }
  assert(found_callback && found_bytes && found_signed && found_unsigned);
  MuonServiceCallCompletion late;
  const auto wait_callback = [&](uint64_t, const MuonServiceSignature&,
      std::vector<MuonServiceValue>, MuonServiceCallCompletion complete) { late = std::move(complete); };
  const auto cancelled = execution.invoke("sdk", 7, "callback", {{message}, {MuonServiceCallback{41}}},
      wait_callback, collect);
  pump();
  assert(late && results.size() == 6);
  execution.cancel("sdk", 7, cancelled);
  execution.cancel("sdk", 7, cancelled);
  pump();
  assert(results.size() == 7 && results.back().code == "cancelled");
  late({MuonServiceValue{std::string("too late")}, {}, {}});
  pump();
  assert(results.size() == 7);
  execution.invoke("sdk", 7, "callback", {{message}, {MuonServiceCallback{41}}}, wait_callback, collect);
  pump();
  bool stopped = false;
  host.Stop("sdk", 7, [&] { stopped = true; });
  pump();
  assert(!stopped && timers.size() == 2);
  auto timer = std::find_if(timers.begin(), timers.end(), [](const auto& item) {
    return item.second.delay == std::chrono::milliseconds(1);
  });
  assert(timer != timers.end());
  auto expire = std::move(timer->second.task); timers.erase(timer); expire();
  pump();
  assert(stopped && host.IsIdle() && timers.empty());
  assert(results.size() == 8 && results.back().code == "cancelled");
  late({MuonServiceValue{std::string("after heap disposal")}, {}, {}});
  pump();
  assert(results.size() == 8 && host.IsIdle());
  std::cout << "TypeScript service SDK callbacks, events, values, and timer-driven stop passed\n";
}
