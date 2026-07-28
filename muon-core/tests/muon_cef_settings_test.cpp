/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "config/muon_cef_settings.h"

#include <cstdlib>
#include <iostream>
#include <string>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static std::string ReadCefSettingString(cef_string_t* value) {
  return CefString(value).ToString();
}

static void SetEnvironmentValue(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

static void ClearEnvironmentValue(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

static bool TestCreatesLauncherSafeCefSettings() {
  ClearEnvironmentValue("MUON_CEF_SANDBOX");
  MuonConfig config;
  config.browser.profile = "profile";
  config.cdp.enable = true;
  config.cdp.port = 9555;
  config.log.cef = kMuonLogLevelError;

  auto settings = CreateMuonCefSettings(
      config, "/tmp/muon-runtime", "/tmp/muon-runtime/profile/muon-cef.log");
  return Expect(settings.no_sandbox,
                "CEF sandbox was not disabled for user runtime launch") &&
         Expect(settings.use_views_default_popup,
                "views popup setting was not enabled") &&
         Expect(settings.remote_debugging_port == 9555,
                "remote debugging port was not propagated") &&
         Expect(ReadCefSettingString(&settings.root_cache_path) ==
                    "/tmp/muon-runtime/profile",
                "root cache path was not resolved from executable directory") &&
         Expect(ReadCefSettingString(&settings.cache_path) ==
                    "/tmp/muon-runtime/profile",
                "cache path was not resolved from executable directory") &&
         Expect(ReadCefSettingString(&settings.log_file) ==
                    "/tmp/muon-runtime/profile/muon-cef.log",
                "CEF log path was not propagated") &&
         Expect(settings.log_severity == LOGSEVERITY_ERROR,
                "CEF log severity was not propagated");
}

static bool TestEnablesCefSandboxWhenLauncherRequestsIt() {
  SetEnvironmentValue("MUON_CEF_SANDBOX", "1");
  MuonConfig config;
  config.browser.profile = "profile";

  auto settings = CreateMuonCefSettings(
      config, "/tmp/muon-runtime", "/tmp/muon-runtime/profile/muon-cef.log");
  return Expect(!settings.no_sandbox,
                "CEF sandbox was not enabled for system setuid runtime");
}

static bool TestConfiguresInsecureLocalhostCommandLine() {
  MuonConfig enabled_config;
  enabled_config.network.local_access.allow_insecure_localhost = true;
  auto browser_command_line = CefCommandLine::CreateCommandLine();
  ConfigureMuonCefNetworkCommandLine(enabled_config, CefString(),
                                     browser_command_line);
  if (!Expect(browser_command_line->HasSwitch("allow-insecure-localhost"),
              "enabled insecure localhost switch was not added")) {
    return false;
  }

  auto renderer_command_line = CefCommandLine::CreateCommandLine();
  ConfigureMuonCefNetworkCommandLine(enabled_config, CefString("renderer"),
                                     renderer_command_line);
  if (!Expect(!renderer_command_line->HasSwitch("allow-insecure-localhost"),
              "insecure localhost switch was added to a child process")) {
    return false;
  }

  MuonConfig disabled_config;
  auto disabled_command_line = CefCommandLine::CreateCommandLine();
  ConfigureMuonCefNetworkCommandLine(disabled_config, CefString(),
                                     disabled_command_line);
  if (!Expect(!disabled_command_line->HasSwitch("allow-insecure-localhost"),
              "disabled insecure localhost switch was added")) {
    return false;
  }

  disabled_command_line->AppendSwitch("allow-insecure-localhost");
  ConfigureMuonCefNetworkCommandLine(disabled_config, CefString(),
                                     disabled_command_line);
  return Expect(disabled_command_line->HasSwitch("allow-insecure-localhost"),
                "disabled config removed an existing insecure localhost "
                "switch");
}

int main() {
  return TestCreatesLauncherSafeCefSettings() &&
                 TestEnablesCefSandboxWhenLauncherRequestsIt() &&
                 TestConfiguresInsecureLocalhostCommandLine()
             ? 0
             : 1;
}
