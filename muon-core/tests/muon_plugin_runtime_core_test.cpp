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

static const muon_plugin_metadata* DeclinePackagedPlugin(
    const muon_plugin_init_context*) {
  return nullptr;
}

static const muon_plugin_namespace invalid_packaged_namespace = {
    "invalid namespace",
    nullptr,
    nullptr,
};

static const muon_plugin_namespace* const
    invalid_packaged_namespace_pointers[] = {
        &invalid_packaged_namespace,
        nullptr,
};

static const muon_plugin_metadata invalid_packaged_metadata = {
    invalid_packaged_namespace_pointers,
    nullptr,
    nullptr,
};

static const muon_plugin_metadata* LoadInvalidPackagedPlugin(
    const muon_plugin_init_context*) {
  return &invalid_packaged_metadata;
}

static void BetaPackagedFunction(muon_completion_func completion) {
  completion(nullptr, nullptr);
}

static const muon_type_descriptor packaged_type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_plugin_function_metadata beta_packaged_function = {
    "betaValue",
    reinterpret_cast<muon_native_function>(&BetaPackagedFunction),
    {0, nullptr, &packaged_type_void},
    nullptr,
};

static const muon_plugin_function_metadata* const
    beta_packaged_function_pointers[] = {
        &beta_packaged_function,
        nullptr,
};

static const muon_plugin_namespace beta_packaged_namespace = {
    "muon.test.beta",
    nullptr,
    beta_packaged_function_pointers,
};

static const muon_plugin_namespace* const beta_packaged_namespace_pointers[] = {
    &beta_packaged_namespace,
    nullptr,
};

static const muon_plugin_metadata beta_packaged_metadata = {
    beta_packaged_namespace_pointers,
    nullptr,
    nullptr,
};

static const muon_plugin_metadata* LoadDisallowedPackagedPlugin(
    const muon_plugin_init_context*) {
  return &beta_packaged_metadata;
}

static MuonPluginRuntimeServices CreateRuntimeServices(
    cardio::dispatcher* dispatcher,
    std::thread::id owner_thread) {
  MuonPluginRuntimeServices services;
  services.dispatcher = dispatcher;
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

static bool ExpectPackagedPluginStartupFailure(
    cardio::dispatcher* dispatcher,
    std::thread::id owner_thread,
    muon_init_plugin_func init_plugin,
    const std::string& expected_diagnostic) {
  auto services = CreateRuntimeServices(dispatcher, owner_thread);
  services.open_library = [](const std::string&, std::string*) -> void* {
    return reinterpret_cast<void*>(1);
  };
  services.find_symbol = [init_plugin](void*, const char*) -> void* {
    return reinterpret_cast<void*>(init_plugin);
  };
  services.close_library = [](void*) {};

  MuonPluginRuntimeLoadEntry plugin;
  plugin.plugin = "logical-alpha";
  plugin.has_library_locator = true;
  plugin.library_locator = "libunrelated-soname.so";
  plugin.plugin_policy = CreateAlphaPolicy();
  const auto runtime = std::make_shared<MuonPluginRuntime>(
      std::filesystem::path{},
      std::vector<MuonPluginRuntimeLoadEntry>{std::move(plugin)},
      std::move(services));
  return Expect(!runtime->IsReady(),
                "packaged plugin failure was accepted") &&
         Expect(runtime->GetStartupError().find("logical-alpha") !=
                    std::string::npos,
                "packaged plugin diagnostic omitted its logical name: " +
                    runtime->GetStartupError()) &&
         Expect(runtime->GetStartupError().find(expected_diagnostic) !=
                    std::string::npos,
                "packaged plugin diagnostic omitted its cause: " +
                    runtime->GetStartupError());
}

static bool RunPackagedPluginStartupDiagnosticTests(
    cardio::dispatcher* dispatcher,
    std::thread::id owner_thread) {
  auto open_failure_services = CreateRuntimeServices(dispatcher, owner_thread);
  open_failure_services.open_library = [](
      const std::string&,
      std::string* error_message) -> void* {
    *error_message = "not installed";
    return nullptr;
  };
  MuonPluginRuntimeLoadEntry missing_plugin;
  missing_plugin.plugin = "logical-alpha";
  missing_plugin.has_library_locator = true;
  missing_plugin.library_locator = "libunrelated-soname.so";
  missing_plugin.plugin_policy = CreateAlphaPolicy();
  const auto missing_runtime = std::make_shared<MuonPluginRuntime>(
      std::filesystem::path{},
      std::vector<MuonPluginRuntimeLoadEntry>{std::move(missing_plugin)},
      std::move(open_failure_services));
  if (!Expect(!missing_runtime->IsReady(),
              "missing packaged plugin was accepted") ||
      !Expect(missing_runtime->GetStartupError().find("logical-alpha") !=
                  std::string::npos,
              "load failure omitted its logical plugin name")) {
    return false;
  }

  return ExpectPackagedPluginStartupFailure(
             dispatcher, owner_thread, nullptr, "muon_init_plugin") &&
         ExpectPackagedPluginStartupFailure(
             dispatcher, owner_thread, &DeclinePackagedPlugin,
             "declined loading") &&
         ExpectPackagedPluginStartupFailure(
             dispatcher, owner_thread, &LoadInvalidPackagedPlugin,
             "namespace") &&
         ExpectPackagedPluginStartupFailure(
             dispatcher, owner_thread, &LoadDisallowedPackagedPlugin,
             "no allowed functions");
}

static bool RunPortablePluginRuntimeTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  const auto owner_thread = std::this_thread::get_id();
  const auto policy = CreateAlphaPolicy();
  if (!Expect(dispatcher != nullptr, "test dispatcher is unavailable") ||
      !Expect(policy != nullptr, "test plugin policy is unavailable") ||
      !RunPackagedPluginStartupDiagnosticTests(dispatcher, owner_thread)) {
    return false;
  }

  MuonPluginRuntimeLoadEntry plugin;
  plugin.plugin = "muon_test_plugin_alpha";
  plugin.plugin_policy = policy;
  plugin.config.push_back({"alpha.config", "portable"});
  plugin.has_expected_function_paths = true;
  plugin.expected_function_paths = {"muon.test.alpha.nonexistent"};
  {
    auto mismatch = std::make_shared<MuonPluginRuntime>(
        std::filesystem::path(MUON_TEST_PLUGIN_DIRECTORY),
        std::vector<MuonPluginRuntimeLoadEntry>{plugin},
        CreateRuntimeServices(dispatcher, owner_thread));
    if (!Expect(!mismatch->IsReady(), "producer catalog mismatch was accepted") ||
        !Expect(mismatch->GetStartupError().find("catalog mismatch") != std::string::npos,
                "producer catalog mismatch diagnostic is missing")) {
      return false;
    }
  }
  plugin.expected_function_paths = {"muon.test.alpha.alphaName",
                                   "muon.test.alpha.alphaAdd",
                                   "muon.test.alpha.alphaConfig"};
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
