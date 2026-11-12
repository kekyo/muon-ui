/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_quickjs_service_host.h"
#include <cassert>
#include <deque>
#include <iostream>

int main() {
  std::deque<std::function<void()>> tasks;
  std::map<uint64_t, std::function<void()>> timers;
  uint64_t next_timer = 0;
  auto failures = std::vector<std::string>{};
  const auto post = [&](std::function<void()> task) { tasks.push_back(std::move(task)); };
  const auto pump = [&] {
    size_t turns = 0;
    while (!tasks.empty()) {
      assert(++turns < 1000);
      auto task = std::move(tasks.front());
      tasks.pop_front();
      task();
    }
  };
  MuonQuickJsServiceHost host({post,
    [&](std::chrono::milliseconds, std::function<void()> task) {
      const auto id = ++next_timer;
      timers.emplace(id, std::move(task));
      return std::function<void()>([&, id] { timers.erase(id); });
    },
    [](const std::string&, muon_log_level, const std::string&) {},
    [&](const std::string& id, uint64_t, const std::string&) { failures.push_back(id); },
    {64 * 1024 * 1024, 1024 * 1024, std::chrono::milliseconds(100)},
    std::chrono::seconds(5)});
  const auto source = R"(
    globalThis.__muonServiceInterface = {schemaVersion:1, methods:{
      echo:{parameters:['string'],returns:'string',errors:[]},
      spin:{parameters:[],returns:'void',errors:[]}
    }, events:{}};
    let identity;
    globalThis.__muonServiceRuntime = {
      async start(input) { identity = input.id; await 0; },
      async invoke(id, method, args) {
        if (method === 'spin') {
          void (async () => { for (;;) {} })();
          return await new Promise(() => {});
        }
        await 0;
        return {status:'ok', value:identity + ':' + args[0]};
      },
      cancel() {},
      async stop() { await 0; }
    };
  )";
  int started = 0;
  for (const auto id : {"first", "second"}) {
    host.Start({id, "fixture.mjs", source, "/private", {}}, 1,
        [&](const std::string& error) { assert(error.empty()); ++started; });
  }
  assert(started == 0);
  pump();
  assert(started == 2 && !host.IsIdle());
  assert(host.Execution().describe("first", 1).methods.size() == 2);
  auto results = std::vector<MuonServiceResult>{};
  const auto collect = [&](MuonServiceResult result) { results.push_back(std::move(result)); };
  host.Execution().invoke("first", 1, "echo", {{std::string("hello\0world", 11)}}, {}, collect);
  host.Execution().invoke("second", 1, "echo", {{std::string("独立😀")}}, {}, collect);
  pump();
  assert(results.size() == 2);
  assert(std::get<std::string>(results[0].value->data) == std::string("first:hello\0world", 17));
  assert(std::get<std::string>(results[1].value->data) == "second:独立😀");
  host.Execution().invoke("first", 1, "spin", {}, {}, collect);
  pump();
  assert(results.size() == 3 && !results.back().value);
  assert(failures == std::vector<std::string>{"first"});
  host.Execution().invoke("second", 1, "echo", {{std::string("still alive")}}, {}, collect);
  pump();
  assert(std::get<std::string>(results.back().value->data) == "second:still alive");
  int stopped = 0;
  host.Stop("first", 1, [&] { ++stopped; });
  host.Stop("second", 1, [&] { ++stopped; });
  pump();
  assert(stopped == 2 && host.IsIdle() && timers.empty());
  auto startup_error = std::string{};
  host.Start({"broken", "broken.mjs", "throw new Error('bad module')", "/private", {}}, 2,
      [&](const std::string& error) { startup_error = error; });
  pump();
  assert(!startup_error.empty() && host.IsIdle() && timers.empty());
  startup_error.clear();
  host.Start({"expired", "expired.mjs", source, "/private", {},
      std::chrono::steady_clock::now() - std::chrono::milliseconds(1)}, 8,
      [&](const std::string& error) { startup_error = error; });
  pump();
  assert(!startup_error.empty() && host.IsIdle() && timers.empty());
  startup_error.clear();
  host.Start({"pending", "pending.mjs", "await new Promise(() => {});", "/private", {}}, 3,
      [&](const std::string& error) { startup_error = error; });
  pump();
  assert(startup_error.empty() && timers.size() == 1);
  auto deadlines = std::move(timers);
  timers.clear();
  for (const auto& [id, expire] : deadlines) { (void)id; expire(); }
  pump();
  assert(!startup_error.empty() && host.IsIdle());
  const auto hanging_stop = R"(
    globalThis.__muonServiceInterface = {schemaVersion:1,methods:{},events:{}};
    globalThis.__muonServiceRuntime = {
      async start() {},
      async stop() { await new Promise(() => {}); }
    };
  )";
  host.Start({"stop-timeout", "stop.mjs", hanging_stop, "/private", {}}, 4,
      [&](const std::string& error) { assert(error.empty()); });
  pump();
  host.Stop("stop-timeout", 4, [&] { ++stopped; });
  pump();
  assert(stopped == 2 && timers.size() == 1);
  deadlines = std::move(timers);
  timers.clear();
  for (const auto& [id, expire] : deadlines) { (void)id; expire(); }
  pump();
  assert(stopped == 3 && host.IsIdle() && failures == (std::vector<std::string>{"first", "stop-timeout"}));
  const auto invalid_result = R"(
    globalThis.__muonServiceInterface = {schemaVersion:1,methods:{
      invalid:{parameters:[],returns:'string',errors:[]}
    },events:{}};
    globalThis.__muonServiceRuntime = {
      async start() {},
      async invoke() { return {status:'ok', get value() { throw new Error('bad getter'); }}; },
      async stop() {}
    };
  )";
  host.Start({"failed-before-stop", "result.mjs", invalid_result, "/private", {}}, 5,
      [&](const std::string& error) { assert(error.empty()); });
  pump();
  host.Execution().invoke("failed-before-stop", 5, "invalid", {}, {}, collect);
  auto invoke = std::move(tasks.front());
  tasks.pop_front();
  invoke();
  // A runtime fault and a stop can already be queued on the same dispatcher.
  host.Stop("failed-before-stop", 5, [&] { ++stopped; });
  pump();
  assert(stopped == 4 && host.IsIdle() && timers.empty());
  assert(failures.back() == "failed-before-stop");
  const auto boundaries = R"(
    globalThis.__muonServiceInterface = {schemaVersion:1,methods:{
      touch:{parameters:[],returns:'uint32',errors:[]},
      count:{parameters:[],returns:'uint32',errors:[]},
      leak:{parameters:[{callback:{parameters:['string'],returns:'string'}}],returns:'string',errors:[]},
      flood:{parameters:[{callback:{parameters:['string'],returns:'string'}}],returns:'string',errors:[]}
    },events:{}};
    let count = 0;
    globalThis.__muonServiceRuntime = {
      async start() {},
      async invoke(id, method, args) {
        if (method === 'touch') ++count;
        if (method === 'leak') {
          __muonServiceCallback(id, args[0], ['pending']);
          return {status:'ok',value:'parent returned'};
        }
        if (method === 'flood') {
          const pending = [];
          const reentrant = [null];
          Object.defineProperty(reentrant, 0, {get() {
            for (let index = 0; index < 32; ++index)
              pending.push(__muonServiceCallback(id, args[0], ['inner']));
            return 'outer';
          }});
          let outcome = 'accepted';
          try { pending.push(__muonServiceCallback(id, args[0], reentrant)); }
          catch (error) { outcome = error.code; }
          await Promise.all(pending);
          return {status:'ok',value:outcome};
        }
        return {status:'ok',value:count};
      },
      cancel() {},
      async stop() {}
    };
  )";
  host.Start({"boundaries", "boundaries.mjs", boundaries, "/private", {}}, 6,
      [&](const std::string& error) { assert(error.empty()); });
  pump();
  const auto touch = host.Execution().invoke("boundaries", 6, "touch", {}, {}, collect);
  host.Execution().cancel("boundaries", 6, touch);
  pump();
  host.Execution().invoke("boundaries", 6, "count", {}, {}, collect);
  pump();
  assert(std::get<uint32_t>(results.back().value->data) == 0);
  MuonServiceCallCompletion pending_callback;
  host.Execution().invoke("boundaries", 6, "leak", {{MuonServiceCallback{1}}},
      [&](uint64_t, const MuonServiceSignature&, std::vector<MuonServiceValue>, MuonServiceCallCompletion complete) {
        pending_callback = std::move(complete);
      }, collect);
  pump();
  assert(pending_callback && results.back().code == "invalid-result");
  const auto results_before_late_reply = results.size();
  pending_callback({MuonServiceValue{std::string("late")}, {}, {}});
  pump();
  assert(results.size() == results_before_late_reply);
  size_t callback_count = 0;
  host.Execution().invoke("boundaries", 6, "flood", {{MuonServiceCallback{1}}},
      [&](uint64_t, const MuonServiceSignature&, std::vector<MuonServiceValue>, MuonServiceCallCompletion complete) {
        ++callback_count;
        complete({MuonServiceValue{std::string("done")}, {}, {}});
      }, collect);
  pump();
  assert(std::get<std::string>(results.back().value->data) == "resource-exhausted");
  assert(callback_count == 32);
  host.Stop("boundaries", 6, [&] { ++stopped; });
  pump();
  assert(host.IsIdle() && timers.empty());
  startup_error.clear();
  host.Start({"oversized-config", "config.mjs", source, "/private",
      {{"value", std::string(80 * 1024 * 1024, 'x')}}}, 7,
      [&](const std::string& error) { startup_error = error; });
  pump();
  assert(!startup_error.empty() && host.IsIdle() && timers.empty());
  std::cout << "QuickJS service isolation, typed calls, interruption, and async stop passed\n";
}
