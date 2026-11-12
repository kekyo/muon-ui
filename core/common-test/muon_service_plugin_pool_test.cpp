/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_plugin_pool.h"
#include "services/muon_quickjs_service_plugins.h"
#include <cardio.h>
#include <cassert>
#include <deque>
#include <dlfcn.h>
#include <iostream>
#include <mutex>
#include <thread>

static void Evaluate(MuonQuickJsEngine& engine, const std::string& source, const std::string& name) {
  const auto result = engine.Evaluate(source, name, JS_EVAL_TYPE_MODULE);
  JS_FreeValue(engine.Context(), result);
}
static void CheckPromise(MuonQuickJsEngine& engine, const char* name) {
  const auto context = engine.Context();
  const auto global = JS_GetGlobalObject(context);
  const auto promise = JS_GetPropertyStr(context, global, name);
  const auto state = JS_PromiseState(context, promise);
  const auto value = JS_PromiseResult(context, promise);
  if (state == JS_PROMISE_REJECTED) std::cerr << engine.ExceptionText(value) << '\n';
  assert(state == JS_PROMISE_FULFILLED);
  JS_FreeValue(context, value); JS_FreeValue(context, promise); JS_FreeValue(context, global);
}
static void InstallLoader(MuonQuickJsEngine& engine, MuonQuickJsPluginBindings* bindings) {
  JS_SetModuleLoaderFunc(engine.Runtime(), nullptr,
      [](JSContext* context, const char* name, void* opaque) {
        return static_cast<MuonQuickJsPluginBindings*>(opaque)->LoadModule(context, name);
      }, bindings);
}

int main() {
  cardio::dispatcher_group group(cardio::exit_condition::exit_by_empty);
  cardio::dispatcher_host host(group);
  std::deque<std::function<void()>> tasks;
  std::mutex tasks_mutex;
  const auto post = [&](std::function<void()> task) {
    const auto lock = std::lock_guard(tasks_mutex);
    tasks.push_back(std::move(task));
  };
  const auto pump = [&] {
    size_t turns = 0;
    do {
      host.park();
      while (!tasks.empty()) { assert(++turns < 10000); auto next = std::move(tasks.front()); tasks.pop_front(); next(); }
      host.park();
    } while (!tasks.empty());
  };
  int opens = 0, closes = 0;
  bool (*has_pending)() = nullptr;
  bool (*is_stopping)() = nullptr;
  void (*complete_pending)() = nullptr;
  void (*finish_shutdown)() = nullptr;
  void (*release_callback)() = nullptr;
  MuonPluginRuntimeServices platform;
  platform.dispatcher = &host;
  platform.is_owner_thread = [owner = std::this_thread::get_id()] { return owner == std::this_thread::get_id(); };
  platform.post_owner_task = [&](std::function<void()> task) { post(std::move(task)); return true; };
  platform.allocate_buffer = [](size_t size, std::string*) { return CreateMuonRpcOwnedBuffer(size); };
  platform.emit_log = [](MuonPluginRuntimeLogSource, muon_log_level, const std::string&) {};
  platform.open_library = [&](const std::string& path, std::string* error) {
    auto library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) { *error = dlerror(); return library; }
    ++opens;
    has_pending = reinterpret_cast<bool(*)()>(dlsym(library, "muon_fixture_has_pending"));
    is_stopping = reinterpret_cast<bool(*)()>(dlsym(library, "muon_fixture_is_stopping"));
    complete_pending = reinterpret_cast<void(*)()>(dlsym(library, "muon_fixture_complete_pending"));
    finish_shutdown = reinterpret_cast<void(*)()>(dlsym(library, "muon_fixture_finish_shutdown"));
    release_callback = reinterpret_cast<void(*)()>(dlsym(library, "muon_fixture_release_callback"));
    const auto hold = reinterpret_cast<void(*)(bool)>(dlsym(library, "muon_fixture_hold_shutdown"));
    assert(has_pending && is_stopping && complete_pending && finish_shutdown && hold);
    hold(true);
    return library;
  };
  platform.find_symbol = [](void* library, const char* name) { return dlsym(library, name); };
  platform.close_library = [&](void* library) { assert(!has_pending() && !is_stopping()); ++closes; dlclose(library); };
  std::shared_ptr<MuonPluginPolicy> policy;
  std::string error;
  assert(CreateMuonPluginPolicy({"fixture.api.*"}, &policy, &error));
  MuonPluginRuntimeLoadEntry plugin;
  plugin.plugin = "fixture"; plugin.has_library_locator = true;
  plugin.library_locator = MUON_PLUGIN_FIXTURE_PATH; plugin.plugin_policy = policy;
  const std::vector<std::string> allow{"fixture.api.echo", "fixture.api.echoInt64", "fixture.api.echoUint64",
      "fixture.api.echoBytes", "fixture.api.callback", "fixture.api.fail", "fixture.api.makeEcho", "fixture.api.hold", "fixture.api.retain", "fixture.api.wrapped"};
  MuonServicePluginPool pool({}, {plugin}, {{"a", allow}, {"b", {"fixture.api.echo"}}, {"p", {"fixture.api.forbidden"}}}, platform);
  std::shared_ptr<MuonServicePluginLease> a, b, next, forbidden;
  const auto acquire = [&](auto& slot) {
    return [&](auto lease, const std::string& failure) {
      if (!failure.empty()) std::cerr << failure << '\n';
      assert(failure.empty()); slot = std::move(lease);
    };
  };
  pool.Acquire("a", 1, acquire(a)); pool.Acquire("b", 2, acquire(b));
  assert(!a && !b && opens == 0);
  pump(); assert(a && b && opens == 1 && !pool.IsIdle());
  bool duplicate = false;
  pool.Acquire("a", 1, [&](auto lease, const auto& failure) { duplicate = !lease && !failure.empty(); });
  pump(); assert(duplicate && opens == 1);
  const MuonQuickJsLimits limits{64 * 1024 * 1024, 1024 * 1024, std::chrono::seconds(2)};
  auto engine = std::make_unique<MuonQuickJsEngine>(limits);
  auto second = std::make_unique<MuonQuickJsEngine>(limits);
  const auto fault = [](const std::string& failure) { std::cerr << failure << '\n'; assert(false); };
  const auto wake = [&] { tasks.push_back([&] { if (engine) while (engine->RunJobs(64)) {} }); };
  auto bindings = a->Attach(*engine, wake, fault);
  auto other = b->Attach(*second, [&] { tasks.push_back([&] { if (second) while (second->RunJobs(64)) {} }); }, fault);
  InstallLoader(*engine, bindings.get()); InstallLoader(*second, other.get());
  Evaluate(*engine, R"js(
    import * as api from 'fixture:api'; globalThis.api=api;
    globalThis.answer=(async()=>{
      const check=(value)=>{if(!value)throw Error('native boundary assertion')};
      check(await api.echo('日本語😀')==='日本語😀'); check(await api.echo(null)===null);
      check(await api.wrapped('setup')==='setup');
      check(await api.echoInt64(-9223372036854775808n)===-9223372036854775808n);
      check(await api.echoUint64(18446744073709551615n)===18446744073709551615n);
      const bytes=await api.echoBytes(new Uint8Array([1,0,255,4]).subarray(1,3));
      check(bytes instanceof Uint8Array && bytes.length===2 && bytes[0]===0 && bytes[1]===255);
      check((await api.echoBytes(new Uint8Array(0))).length===0);
      check((await api.echoBytes(new ArrayBuffer(0))).length===0);
      check(await api.callback('outer',async(value)=>await api.callback(value,async(inner)=>await api.echo(inner+'!')))==='outer!');
      check(await api.callback('nullable',null)==='nullable');
      Object.defineProperty(Function.prototype,'release',{configurable:true,set(){throw Error('prototype setter must not run')}});
      const proxy=await api.makeEcho(); check(await proxy('proxy')==='proxy'); proxy.release();
      delete Function.prototype.release;
      let released=false;try{await proxy('expired')}catch{released=true}check(released);
      let nul=false;try{await api.echo('a\0b')}catch{nul=true}check(nul);
      let range=false;try{await api.echoUint64(18446744073709551616n)}catch{range=true}check(range);
      Object.defineProperty(Error.prototype,'message',{configurable:true,set(){throw Error('error prototype setter must not run')}});
      let failure=false;try{await api.fail()}catch(error){failure=error instanceof Error && error.message.includes('fixture native failure')}check(failure);
      Object.defineProperty(Error.prototype,'message',{configurable:true,writable:true,value:''});
    })();
  )js", "values.mjs");
  Evaluate(*second, "import * as api from 'fixture:api';globalThis.api=api;globalThis.answer=(async()=>{if(Object.keys(api).join(',')!=='default,echo')throw Error('owner policy');if(await api.echo('other')!=='other')throw Error('other owner')})()", "policy.mjs");
  pump(); CheckPromise(*engine, "answer"); CheckPromise(*second, "answer");
  pool.Acquire("p", 1, acquire(forbidden)); pump();
  bool rejected = false;
  MuonQuickJsEngine forbidden_engine(limits);
  try { auto unused = forbidden->Attach(forbidden_engine, [] {}, fault); (void)unused; }
  catch (const std::exception& failure) { rejected = std::string(failure.what()).find("pointer") != std::string::npos; }
  assert(rejected); forbidden->Close([] {}); pump();

  Evaluate(*engine, "globalThis.retained=api.retain((value)=>value);globalThis.callbackPending=api.callback('pending',()=>new Promise(()=>{}));globalThis.late=api.hold('old generation')", "pending.mjs"); pump(); assert(has_pending()); CheckPromise(*engine, "retained");
  bindings->Detach(); engine.reset();
  bool a_closed = false, b_closed = false;
  a->Close([&] { a_closed = true; }); pump(); assert(!a_closed && closes == 0);
  pool.Acquire("a", 3, acquire(next)); pump(); assert(next && opens == 1);
  engine = std::make_unique<MuonQuickJsEngine>(limits);
  auto replacement = next->Attach(*engine, wake, fault); InstallLoader(*engine, replacement.get());
  Evaluate(*engine, "import * as api from 'fixture:api';globalThis.api=api;globalThis.answer=api.echo('new generation')", "replacement.mjs");
  pump(); CheckPromise(*engine, "answer");
  std::thread native_completion(complete_pending);
  native_completion.join();
  assert(!a_closed); pump(); assert(!a_closed && closes == 0);
  release_callback(); assert(!a_closed); pump(); assert(a_closed && closes == 0);
  Evaluate(*second, "globalThis.answer=api.echo('survived')", "survived.mjs"); pump(); CheckPromise(*second, "answer");

  // Stop completion alone cannot unload a library with an actual call pending.
  Evaluate(*engine, "globalThis.late=api.hold('last native call')", "last.mjs"); pump(); assert(has_pending());
  other->Detach(); second.reset(); b->Close([&] { b_closed = true; }); pump(); assert(b_closed && !is_stopping());
  replacement->Detach(); engine.reset();
  bool next_closed = false;
  next->Close([&] { next_closed = true; }); pump(); assert(is_stopping() && !next_closed);
  std::shared_ptr<MuonServicePluginLease> restarted;
  pool.Acquire("a", 4, acquire(restarted)); pump(); assert(!restarted && opens == 1);
  finish_shutdown(); pump(); assert(!next_closed && closes == 0 && !restarted);
  complete_pending(); assert(!next_closed); pump(); assert(next_closed && restarted && opens == 2 && closes == 1);
  restarted->Close([] {}); pump(); assert(is_stopping()); finish_shutdown(); pump();
  assert(pool.IsIdle() && closes == 2);
  std::map<uint64_t, std::function<void()>> timers;
  uint64_t next_timer = 0;
  const auto delay = [&](std::chrono::milliseconds, std::function<void()> task) {
    const auto id = ++next_timer;
    timers.emplace(id, std::move(task));
    return std::function<void()>([&, id] { timers.erase(id); });
  };
  MuonQuickJsServicePlugins coordinator(pool, delay);
  unsigned attachments = 0;
  MuonQuickJsServiceHost script_host({post, delay,
      [](const std::string&, muon_log_level, const std::string&) {},
      [](const std::string&, uint64_t, const std::string&) {}, limits, std::chrono::seconds(10),
      [&](const MuonQuickJsServiceSpec& spec, uint64_t generation, MuonQuickJsEngine& current,
          std::function<void()> current_wake, std::function<void(const std::string&)> current_fault) {
        ++attachments;
        return coordinator.Attach(spec.id, generation, current, std::move(current_wake), std::move(current_fault));
      }});
  MuonQuickJsServiceSpec spec{"b", "coordinator.mjs", R"js(
    globalThis.__muonServiceInterface={schemaVersion:1,methods:{},events:{}};
    globalThis.__muonServiceRuntime={async start(){},async invoke(){},cancel(){},async stop(){}};
  )js", "/private", {}, std::chrono::steady_clock::now() + std::chrono::seconds(10)};
  std::shared_ptr<MuonServicePluginLease> blocker;
  pool.Acquire("a", 10, acquire(blocker)); pump(); blocker->Close([] {}); pump();
  assert(is_stopping());
  unsigned reported = 0;
  coordinator.Start(script_host, spec, 1, [&](const auto& failure) { assert(!failure.empty()); ++reported; });
  pump(); assert(!coordinator.IsIdle() && reported == 0 && attachments == 0);
  assert(timers.size() == 1);
  auto deadline = timers.begin()->second; deadline();
  assert(reported == 1 && script_host.IsIdle() && !coordinator.IsIdle());
  finish_shutdown(); pump(); assert(is_stopping() && reported == 1 && attachments == 0);
  finish_shutdown(); pump(); assert(coordinator.IsIdle() && pool.IsIdle() && reported == 1);

  // A host rejection before Attach must still close the already acquired lease.
  bool ready = false;
  script_host.Start(spec, 2, [&](const auto& failure) { assert(failure.empty()); ready = true; });
  pump(); assert(ready);
  const auto initial_attachments = attachments;
  coordinator.Start(script_host, spec, 3, [&](const auto& failure) { assert(!failure.empty()); ++reported; });
  pump(); assert(reported == 2 && attachments == initial_attachments && is_stopping());
  finish_shutdown(); pump(); assert(coordinator.IsIdle() && pool.IsIdle());
  script_host.Stop("b", 2, [] {}); pump(); assert(script_host.IsIdle() && timers.empty());
  std::cout << "Shared native pool, real C99 ABI values/callbacks/policy, heap detach, drain and restart passed\n";
}
