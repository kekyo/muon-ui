/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "include/cef_app.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <cardio.h>

#include "app/muon_app.h"
#include "browser/muon_native_wheel_forwarder.h"
#include "browser/muon_title_bar.h"
#include "config/muon_cef_settings.h"
#include "config/muon_config.h"
#include "config/muon_paths.h"
#include "config/muon_startup.h"
#include "log/muon_log.h"

static CefMainArgs CreateMainArgs(int argc, char* argv[]) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  return CefMainArgs(GetModuleHandleW(nullptr));
#else
  return CefMainArgs(argc, argv);
#endif
}

NO_STACK_PROTECTOR int main(int argc, char* argv[]) {
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

  auto main_args = CreateMainArgs(argc, argv);

  CefRefPtr<MuonApp> app(
      new MuonApp(config, cef_log_path, config_paths, dispatcher));
  const auto exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
  if (exit_code >= 0) {
    ShutdownMuonLogger();
    return exit_code;
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
