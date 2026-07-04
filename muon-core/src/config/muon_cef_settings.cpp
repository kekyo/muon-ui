/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "config/muon_cef_settings.h"

static std::filesystem::path ResolveExecutableRelativePath(
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& path) {
  if (path.is_absolute()) {
    return path.lexically_normal();
  }
  return (executable_directory / path).lexically_normal();
}

static void SetCefPath(cef_string_t* target,
                       const std::filesystem::path& path) {
#if defined(_WIN32)
  CefString(target).FromWString(path.wstring());
#else
  CefString(target).FromString(path.string());
#endif
}

static void ConfigureProfilePath(
    CefSettings& settings,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& profile_path) {
  const auto resolved_profile_path =
      ResolveExecutableRelativePath(executable_directory, profile_path);
  SetCefPath(&settings.root_cache_path, resolved_profile_path);
  SetCefPath(&settings.cache_path, resolved_profile_path);
}

static void ConfigureCefLogPath(CefSettings& settings,
                                const std::filesystem::path& path) {
  SetCefPath(&settings.log_file, path);
}

static cef_log_severity_t GetCefLogSeverity(MuonLogLevel level) {
  switch (level) {
    case kMuonLogLevelDebug:
      return LOGSEVERITY_VERBOSE;
    case kMuonLogLevelInfo:
      return LOGSEVERITY_INFO;
    case kMuonLogLevelWarning:
      return LOGSEVERITY_WARNING;
    case kMuonLogLevelError:
      return LOGSEVERITY_ERROR;
    case kMuonLogLevelFatal:
      return LOGSEVERITY_FATAL;
    case kMuonLogLevelOff:
      return LOGSEVERITY_DISABLE;
  }
  return LOGSEVERITY_INFO;
}

CefSettings CreateMuonCefSettings(
    const MuonConfig& config,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& cef_log_path) {
  CefSettings settings;
  settings.no_sandbox = true;
  settings.use_views_default_popup = true;
  if (config.cdp.enable) {
    settings.remote_debugging_port = config.cdp.port;
  }
  ConfigureProfilePath(settings, executable_directory,
                       config.browser.profile);
  ConfigureCefLogPath(settings, cef_log_path);
  settings.log_severity = GetCefLogSeverity(config.log.cef);
  return settings;
}
