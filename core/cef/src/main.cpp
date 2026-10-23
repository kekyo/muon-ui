/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "include/cef_app.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <cardio.h>

#if CARDIO_WITH_GLIB
#include <gio/gio.h>
#endif

#include "app/muon_app.h"
#include "browser/muon_native_wheel_forwarder.h"
#include "browser/muon_title_bar.h"
#include "config/muon_cef_settings.h"
#include "config/muon_autostart.h"
#include "config/muon_config.h"
#include "config/muon_paths.h"
#include "config/muon_startup.h"
#include "log/muon_log.h"
#include "muon_filesystem.h"

#if defined(__linux__)
#include "notifications/muon_notification_linux.h"
#include "notifications/muon_notification_maintenance.h"

static cardio::promise<void> ClearNotificationHistory(
    const MuonConfig& config, GMainLoop* loop, int& exit_code) {
  try {
    co_await muon_internal::ClearMuonNotificationHistory(
        (config.browser.profile / "notifications.json").string(),
        muon_internal::CreateMuonLinuxNotificationBackend(
            config.app_id, config.desktop_id, config.app_id, config.launcher_path));
    exit_code = 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "muon notification maintenance failed: %s\n", error.what());
  }
  g_main_loop_quit(loop);
}
#endif

static CefMainArgs CreateMainArgs(int argc, char* argv[]) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  return CefMainArgs(GetModuleHandleW(nullptr));
#else
  return CefMainArgs(argc, argv);
#endif
}

NO_STACK_PROTECTOR static int RunMuon(int argc, char* argv[]) {
  SetMuonStartupCommandLine(argc, argv);
  MuonConfig config;
  std::string error_message;
  std::vector<std::filesystem::path> config_paths;
  auto embedded_config = false;
  if (!LoadMuonStartupConfig(GetMuonStartupCommandLine(), &config,
                             &config_paths, &embedded_config,
                             &error_message)) {
    std::fprintf(stderr, "muon startup failed: %s\n", error_message.c_str());
    std::fflush(stderr);
    return 1;
  }
  (void)embedded_config;
  const auto executable_directory = GetMuonExecutableDirectory();
#if defined(_WIN32)
  // CEF/Chromium can silently substitute a different profile for an unsupported
  // path, even after Muon's own filesystem operations succeed. Do not expose
  // that fallback as a successful startup with persistent application data.
  const auto profile_path = muon_internal::ResolveFilesystemPath(
      executable_directory, config.browser.profile);
  if (profile_path.native().size() >= MAX_PATH ||
      profile_path.native().starts_with(L"\\\\?\\")) {
    std::fprintf(stderr,
        "muon startup failed: browser.profilePath requires a short ordinary "
        "Windows path for CEF. Set browser.profilePath to a shallow directory "
        "without the extended namespace prefix: %s\n",
        muon_internal::FilesystemPathToUtf8(profile_path).c_str());
    std::fflush(stderr);
    return 1;
  }
#endif
  const auto cef_log_path =
      GetMuonInternalCefLogPath(executable_directory, config.browser.profile);
  if (!InitializeMuonLogger(config.log, executable_directory, cef_log_path,
                            &error_message)) {
    std::fprintf(stderr, "muon startup failed: %s\n", error_message.c_str());
    std::fflush(stderr);
    return 1;
  }

  // Initialize the dispatcher before CEF can invoke UI-thread callbacks.
  cardio::dispatcher* dispatcher = nullptr;
#if CARDIO_WITH_GLIB
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto d(group);
  dispatcher = &d;
#elif CARDIO_HAS_WIN32_HANDLE
  cardio::dispatcher_group group;
  cardio::dispatcher_host_win32_auto d(group);
  dispatcher = &d;
#endif

#if defined(__linux__)
  for (const auto& argument : GetMuonStartupCommandLine()) {
    if (argument != "--muon-notification-clear-history") continue;
    // Installer maintenance uses the same profile and verified registration as
    // the app, including a disabled update, without starting CEF or a window.
    cardio::set_current_dispatcher(dispatcher);
    auto* loop = g_main_loop_new(nullptr, FALSE);
    auto result = 1;
    auto pending = ClearNotificationHistory(config, loop, result);
    if (!pending.is_ready()) g_main_loop_run(loop);
    g_main_loop_unref(loop);
    cardio::set_current_dispatcher(nullptr);
    ShutdownMuonLogger();
    return result;
  }
#endif

  auto main_args = CreateMainArgs(argc, argv);

  CefRefPtr<MuonApp> app(
      new MuonApp(config, cef_log_path, config_paths, dispatcher));
  const auto exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
  if (exit_code >= 0) {
    ShutdownMuonLogger();
    return exit_code;
  }

  MuonAutostartOptions autostart_options;
  if (!CreateDefaultMuonAutostartOptions(&autostart_options, &error_message) ||
      !RefreshMuonAppImageAutostart(autostart_options, &error_message)) {
    std::fprintf(stderr, "muon autostart update failed: %s\n", error_message.c_str());
  }

  auto settings =
      CreateMuonCefSettings(config, executable_directory, cef_log_path);

  const auto cef_logging_enabled =
      GetMuonLogSourceLevel(config.log, kMuonLogSourceCef) !=
      kMuonLogLevelOff;
  if (cef_logging_enabled &&
      !ResetMuonCefLogFile(cef_log_path, &error_message)) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + error_message);
    ShutdownMuonLogger();
    return 1;
  }

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    const auto cef_exit_code = CefGetExitCode();
    StopMuonCefLogForwarder();
    ShutdownMuonLogger();
    return cef_exit_code;
  }
  if (cef_logging_enabled &&
      !StartMuonCefLogForwarder(cef_log_path, &error_message)) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + error_message);
    ClearMuonNativeWheelForwarders();
    ClearMuonTitleBarRegistrations();
    CefShutdown();
    ShutdownMuonLogger();
    return 1;
  }
  if (app->GetExitCode() != 0) {
    const auto exit_code = app->GetExitCode();
    ClearMuonNativeWheelForwarders();
    ClearMuonTitleBarRegistrations();
    CefShutdown();
    StopMuonCefLogForwarder();
    ShutdownMuonLogger();
    return exit_code;
  }

  CefRunMessageLoop();

  ClearMuonNativeWheelForwarders();
  ClearMuonTitleBarRegistrations();
  CefShutdown();
  const auto app_exit_code = app->GetExitCode();
  StopMuonCefLogForwarder();
  ShutdownMuonLogger();
  return app_exit_code;
}

#if defined(_WIN32)
NO_STACK_PROTECTOR int wmain(int argc, wchar_t* argv[]) {
  auto values = std::vector<std::string>{};
  values.reserve(static_cast<size_t>(argc));
  for (auto index = 0; index < argc; ++index) {
    values.push_back(muon_internal::FilesystemPathToUtf8(
        std::filesystem::path(argv[index])));
  }
  auto arguments = std::vector<char*>{};
  arguments.reserve(values.size() + 1);
  for (auto& value : values) arguments.push_back(value.data());
  arguments.push_back(nullptr);
  return RunMuon(argc, arguments.data());
}
#else
NO_STACK_PROTECTOR int main(int argc, char* argv[]) {
  return RunMuon(argc, argv);
}
#endif
