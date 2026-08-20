/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_cardio_post.h"
#include "plugins/muon_plugin_policy.h"
#include "plugins/muon_plugin_runtime.h"

#include <cardio.h>

#include <dlfcn.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static cardio::promise<void> StopAfterTimeout(
    cardio::dispatcher_group* group) {
  co_await cardio::promises::delay(5000);
  group->shutdown();
}

static std::shared_ptr<MuonPluginPolicy> CreateAlphaPolicy() {
  auto policy = std::shared_ptr<MuonPluginPolicy>{};
  auto error_message = std::string{};
  if (!CreateMuonPluginPolicy(
          {"muon.test.alpha.*"}, &policy, &error_message)) {
    std::cerr << "could not create plugin policy: " << error_message << "\n";
    return nullptr;
  }
  return policy;
}

static MuonPluginRuntimeServices CreateRuntimeServices(
    cardio::dispatcher* dispatcher,
    std::thread::id owner_thread) {
  MuonPluginRuntimeServices services;
  services.is_owner_thread = [owner_thread]() {
    return std::this_thread::get_id() == owner_thread;
  };
  services.post_owner_task = [dispatcher](std::function<void()> task) {
    if (!task) {
      return false;
    }
    muon_internal::FireAndForgetOnDispatcher(
        dispatcher, [task = std::move(task)]() mutable { task(); });
    return true;
  };
  services.allocate_buffer = [](size_t size, std::string*) {
    return CreateMuonRpcOwnedBuffer(size);
  };
  services.is_owner_available = [](const MuonRpcOwner& owner) {
    return IsValidMuonRpcOwner(owner);
  };
  services.send_message = [](const MuonRpcMessage&, std::string*) {
    return true;
  };
  services.emit_log = [](
      MuonPluginRuntimeLogSource,
      muon_log_level,
      const std::string&) {};
  services.open_library = [](
      const std::string& locator,
      std::string* error_message) -> void* {
    dlerror();
    auto* handle = dlopen(locator.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && error_message != nullptr) {
      const auto* error = dlerror();
      *error_message = error == nullptr ? "dlopen failed" : error;
    }
    return handle;
  };
  services.find_symbol = [](void* handle, const char* symbol) -> void* {
    return handle == nullptr || symbol == nullptr
               ? nullptr
               : dlsym(handle, symbol);
  };
  services.close_library = [](void* handle) {
    if (handle != nullptr) {
      dlclose(handle);
    }
  };
  return services;
}

static bool RunPortablePluginRuntimeTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  const auto owner_thread = std::this_thread::get_id();
  const auto policy = CreateAlphaPolicy();
  if (!Expect(dispatcher != nullptr, "test dispatcher is unavailable") ||
      !Expect(policy != nullptr, "test plugin policy is unavailable")) {
    return false;
  }

  MuonPluginRuntimeLoadEntry plugin;
  plugin.plugin = "muon_test_plugin_alpha";
  plugin.plugin_policy = policy;
  plugin.config.push_back({"alpha.config", "portable"});
  auto runtime = std::make_shared<MuonPluginRuntime>(
      std::filesystem::path(MUON_TEST_PLUGIN_DIRECTORY),
      std::vector<MuonPluginRuntimeLoadEntry>{std::move(plugin)},
      CreateRuntimeServices(dispatcher, owner_thread));
  if (!Expect(runtime->IsReady(), runtime->GetStartupError()) ||
      !Expect(runtime->GetFunctions().size() == 3,
              "portable runtime did not expose the alpha plugin")) {
    return false;
  }

  const auto* alpha_add = static_cast<const MuonFunctionMetadata*>(nullptr);
  for (const auto& function : runtime->GetFunctions()) {
    if (CreateMuonFunctionPublicPath(function) ==
        "muon.test.alpha.alphaAdd") {
      alpha_add = &function;
      break;
    }
  }
  if (!Expect(alpha_add != nullptr,
              "portable runtime did not expose alphaAdd")) {
    return false;
  }

  MuonRpcCallRequest request;
  request.owner = {1, "main", 1};
  request.call_id = 1;
  request.function_id = alpha_add->id;
  MuonRpcValue left;
  left.type = CreateMuonPrimitiveType(MUON_TYPE_I32);
  left.i32_value = 20;
  MuonRpcValue right;
  right.type = CreateMuonPrimitiveType(MUON_TYPE_I32);
  right.i32_value = 22;
  request.arguments = {left, right};

  auto result = MuonRpcCallResult{};
  auto completed = false;
  auto stopped = false;
  auto timeout = StopAfterTimeout(&group);
  runtime->Invoke(request, [&](const MuonRpcCallResult& call_result) {
    result = call_result;
    completed = true;
    runtime->Stop([&]() {
      stopped = true;
      group.shutdown();
    });
  });
  (void)timeout;
  host.park();

  return Expect(completed, "portable plugin call did not complete") &&
         Expect(result.success, result.error_message) &&
         Expect(result.value.type.type == MUON_TYPE_I32 &&
                    result.value.i32_value == 42,
                "portable plugin call returned the wrong value") &&
         Expect(stopped, "portable plugin runtime did not stop");
}

int main() {
  return RunPortablePluginRuntimeTest() ? 0 : 1;
}
