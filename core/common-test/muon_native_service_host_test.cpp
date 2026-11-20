/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_native_service_host.h"

#include <deque>
#include <iostream>
#include <stdexcept>

struct TestInstance {
  const muon_service_context* context;
  muon_service_start_completion started;
  void* start_data;
  muon_service_stop_completion stopped = nullptr;
  void* stop_data = nullptr;
  int stop_calls = 0;
  const muon_service_stop_info* info = nullptr;
};

static std::vector<std::unique_ptr<TestInstance>> instances;
static bool library_alive = false;

static void Start(const muon_service_context* context,
                  muon_service_start_completion complete, void* data) {
  instances.push_back(std::make_unique<TestInstance>(
      TestInstance{context, complete, data, nullptr, nullptr, 0}));
}

static void Stop(void* data, const muon_service_stop_info* info, muon_service_stop_completion complete,
                 void* completion_data) {
  auto& instance = *static_cast<TestInstance*>(data);
  ++instance.stop_calls;
  instance.info = info;
  instance.stopped = complete;
  instance.stop_data = completion_data;
}

static void CompleteStart(TestInstance& instance) {
  const auto value = muon_service_instance{&instance, &Stop, nullptr, nullptr};
  instance.started(instance.start_data, &value, nullptr);
}

static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  auto tasks = std::deque<std::function<void()>>{};
  const auto pump = [&] {
    while (!tasks.empty()) {
      auto task = std::move(tasks.front());
      tasks.pop_front();
      task();
    }
  };
  auto loads = 0;
  auto unloads = 0;
  auto logs = std::vector<std::string>{};
  const auto factory = muon_service_factory{MUON_SERVICE_ABI_VERSION, &Start, nullptr};
  const auto bad_factory = muon_service_factory{999, &Start, nullptr};
  auto host = MuonNativeServiceHost({
      [&](const std::string& path, std::string& error) {
        ++loads;
        if (path == "missing") {
          error = "cannot open library";
          return std::shared_ptr<const muon_service_factory>{};
        }
        library_alive = true;
        return std::shared_ptr<const muon_service_factory>{
                path == "bad" ? &bad_factory : &factory,
                [&](const muon_service_factory*) {
                  ++unloads;
                  library_alive = false;
                }};
      },
      [&](std::function<void()> task) { tasks.push_back(std::move(task)); },
      [&](const std::string& id, muon_log_level, const std::string& message) {
        logs.push_back(id + ":" + message);
      }});
  auto started = 0;
  auto stopped = 0;
  auto errors = std::vector<std::string>{};
  const auto completed = [&](const std::string& error) {
    if (error.empty()) ++started;
    else errors.push_back(error);
  };
  const auto a = MuonNativeServiceSpec{"alpha", "shared", "/private/alpha", {{"key", "value"}}, MUON_SERVICE_START_LOGIN_AUTOSTART};
  const auto b = MuonNativeServiceSpec{"beta", "shared", "/private/beta", {}};
  host.Start(a, 1, completed);
  host.Start(b, 1, completed);
  Require(loads == 1 && instances.size() == 2 && !host.IsIdle(),
          "one library must create independently owned service instances");
  Require(std::string(instances[0]->context->service_id) == "alpha" &&
              std::string(instances[1]->context->data_directory) == "/private/beta" &&
              std::string(instances[0]->context->config_entries[0].value) == "value",
          "each asynchronous start must retain its own immutable context");
  host.Start(a, 2, completed);
  CompleteStart(*instances[0]);
  CompleteStart(*instances[1]);
  Require(started == 0, "startup completion must be deferred past native callback return");
  pump();
  Require(started == 2 && errors.size() == 1, "duplicate identities must not start a second instance");
  const auto context = instances[0]->context;
  context->host->log(context->host->user_data, MUON_LOG_LEVEL_INFO, "running");
  auto task_count = 0;
  context->host->post(context->host->user_data,
                      [](void* data) { ++*static_cast<int*>(data); }, &task_count);
  Require(task_count == 0 && logs == std::vector<std::string>{"alpha:running"},
          "service tasks must be queued and diagnostics scoped by service");
  pump();
  Require(task_count == 1, "queued native work must execute exactly once");
  host.Stop("alpha", 7, [&] { ++stopped; });
  pump();
  Require(stopped == 1 && instances[0]->stop_calls == 0, "stale stop must not stop a live generation");
  host.Stop("alpha", 1, [&] { ++stopped; }, {MUON_SERVICE_STOP_SESSION_ENDING, "logoff"});
  host.Stop("alpha", 1, [&] { ++stopped; });
  Require(instances[0]->stop_calls == 1, "repeated stop must share one asynchronous cleanup");
  const auto info = instances[0]->info;
  Require(context->start_reason == MUON_SERVICE_START_LOGIN_AUTOSTART && context->signal->aborted(context->signal->user_data) &&
          info->abi_version == MUON_SERVICE_ABI_VERSION && info->size == sizeof(*info) &&
          info->reason == MUON_SERVICE_STOP_SESSION_ENDING && std::string(info->detail) == "logoff" &&
          !info->signal->aborted(info->signal->user_data), "native cleanup observes the platform reason and a fresh signal");
  instances[0]->stopped(instances[0]->stop_data);
  Require(stopped == 1 && library_alive, "native stop must return before host cleanup");
  pump();
  Require(stopped == 3 && unloads == 0 && !host.IsIdle(),
          "stopping one service must preserve another instance and shared library");
  host.Stop("beta", 1, [&] { ++stopped; });
  instances[1]->stopped(instances[1]->stop_data);
  Require(library_alive, "library must remain loaded while its callback returns");
  pump();
  Require(stopped == 4 && unloads == 1 && host.IsIdle(),
          "last cleanup must release the factory and instance resources");

  host.Start(a, 2, completed);
  Require(loads == 2 && instances.back()->context->generation == 2,
          "later generation must reload and create fresh state");
  instances.back()->started(instances.back()->start_data, nullptr, "initialization failed");
  pump();
  Require(errors.back() == "initialization failed" && host.IsIdle() && unloads == 2,
          "failed startup must release host resources before reporting failure");
  host.Start({"missing", "missing", "/private/missing", {}}, 1, completed);
  host.Start({"bad", "bad", "/private/bad", {}}, 1, completed);
  pump();
  Require(errors.size() == 4 && instances.size() == 3 && host.IsIdle(),
          "missing library and incompatible ABI must fail without executing a factory");
  instances.clear();
  std::cout << "native service host: passed\n";
  return 0;
}
