/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_windows.h"
#include <windows.h>
#include <roapi.h>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <stdexcept>

#if defined(MUON_NOTIFICATION_TEST)
namespace muon_internal {
void TestMuonWindowsNotificationCallbackShutdown();
}
#endif

static constexpr const char* kProbeAppId =
#if defined(_WIN64)
    "dev.muon.NotificationProbe.amd64";
static constexpr const wchar_t* kProbeName = L"Muon Notification Probe 64";
static constexpr const wchar_t* kProbeClsid = L"{BD34C9A0-4D75-4B36-BD46-18BCE46A0E64}";
#else
    "dev.muon.NotificationProbe.i686";
static constexpr const wchar_t* kProbeName = L"Muon Notification Probe 32";
static constexpr const wchar_t* kProbeClsid = L"{BD34C9A0-4D75-4B36-BD46-18BCE46A0E32}";
#endif

static cardio::promise<void> RunDriver(bool& passed, const std::string& mode) {
  const auto exercise = mode == "--exercise" || mode == "--exercise-default";
  const auto missing_registration = mode == "--missing-registration";
  HWND window = nullptr;
  try {
#if defined(MUON_NOTIFICATION_TEST)
    if (mode == "--callback-shutdown") {
      muon_internal::TestMuonWindowsNotificationCallbackShutdown();
      passed = true;
      std::cout << "retained COM callbacks ignored after dispatcher shutdown\n";
      PostQuitMessage(0);
      co_return;
    }
#endif
    wchar_t executable[32768];
    if (!GetModuleFileNameW(nullptr, executable, 32768)) throw std::runtime_error("GetModuleFileNameW");
    auto backend = muon_internal::CreateMuonWindowsNotificationBackend({
        kProbeAppId, std::string(kProbeName, kProbeName + std::wcslen(kProbeName)),
        (std::filesystem::path(executable).parent_path() / L"muon.png").string(),
        missing_registration ? std::string{} :
            std::string(kProbeClsid, kProbeClsid + std::wcslen(kProbeClsid))});
    const auto capabilities = co_await backend.capabilities();
    if (missing_registration) {
      if (capabilities.available || capabilities.reason.empty())
        throw std::runtime_error("Missing registration was not diagnosed");
      const auto status = co_await backend.status();
      if (status.enabled.has_value() || status.reason.empty())
        throw std::runtime_error("Missing registration status was not unknown");
      bool rejected = false;
      try { co_await backend.post({}, "f716b7a0-82e6-4ddf-b2aa-811cda210001"); }
      catch (const std::exception&) { rejected = true; }
      if (!rejected) throw std::runtime_error("Missing registration accepted a post");
      backend.stop();
      passed = true;
      std::cout << "missing registration diagnosed without failing startup\n";
      PostQuitMessage(0);
      co_return;
    }
    if (!capabilities.available || !capabilities.actions) {
      throw std::runtime_error("Registered Windows notifications are unavailable");
    }
    const auto status = co_await backend.status();
    if (mode == "--disabled") {
      if (status.enabled != false || status.reason != "disabled-for-user")
        throw std::runtime_error("Expected the disabled Windows notification switch");
      bool rejected = false;
      try { co_await backend.post({}, "f716b7a0-82e6-4ddf-b2aa-811cda210001"); }
      catch (const std::exception& error) {
        rejected = std::string(error.what()) == "disabled-for-user";
      }
      if (!rejected) throw std::runtime_error("Disabled OS notification post was not rejected");
      backend.stop();
      passed = true;
      std::cout << "disabled-for-user diagnosed and post rejected\n";
      PostQuitMessage(0);
      co_return;
    }
    if (status.enabled == false) throw std::runtime_error("Notification setting: " + status.reason);
    std::cout << "capabilities and setting " << status.enabled.value_or(false) << " " << status.reason << '\n' << std::flush;
    if (exercise) {
      auto activated = cardio::promise_source<std::string>{};
      backend.set_activation([&activated](std::string token, std::optional<std::string> action) {
        activated.resolve(token + ":" + action.value_or("<activate>"));
      });
      const auto token = std::string("f716b7a0-82e6-4ddf-b2aa-811cda210001");
      muon_internal::MuonNotificationPost post;
      post.id = "same";
      post.title = "Muon notification probe " + std::to_string(sizeof(void*) * 8);
      post.body = "Native Windows notification; XML <plain & text>.";
      post.actions.push_back({"default", "View order"});
      auto first = co_await backend.post(post, token);
      auto second = co_await backend.post(post, "f716b7a0-82e6-4ddf-b2aa-811cda210002");
      co_await backend.cancel(first);
      std::cout << "posted " << post.title << '\n' << std::flush;
      window = CreateWindowExW(0, L"STATIC", kProbeName,
          WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
          420, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      const auto event = co_await activated.get_promise();
      std::cout << "activated " << event << '\n' << std::flush;
      const auto expected = std::string("f716b7a0-82e6-4ddf-b2aa-811cda210002:") +
          (mode == "--exercise-default" ? "<activate>" : "default");
      if (event != expected) {
        throw std::runtime_error("Wrong revision or named default action");
      }
      co_await backend.cancel(second);
    }
    backend.stop();
    passed = true;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  if (window) DestroyWindow(window);
  PostQuitMessage(passed ? 0 : 1);
}

int main(int argc, char** argv) {
  const auto initialized = RoInitialize(RO_INIT_SINGLETHREADED);
  if (FAILED(initialized)) return 2;
  auto group = cardio::dispatcher_group();
  auto dispatcher = cardio::dispatcher_host_win32_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto passed = false;
  const auto mode = std::string(argc > 1 ? argv[1] : "");
  auto running = RunDriver(passed, mode);
  if (!running.is_ready()) {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  const auto app_id = std::wstring(kProbeAppId, kProbeAppId + std::strlen(kProbeAppId));
  RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\AppUserModelId\\" + app_id).c_str());
  RoUninitialize();
  return passed ? 0 : 1;
}
