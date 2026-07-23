/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "config/muon_linux_display_backend.h"

#include "muon_string_helpers.h"

#include <cstddef>

namespace {

using muon_internal::ToLowerAscii;

static std::string GetCommandLineSwitchValue(
    const std::vector<std::string>& command_line,
    const char* name) {
  const auto switch_name = std::string("--") + name;
  const auto switch_prefix = switch_name + "=";
  std::string value;
  for (auto index = size_t{1}; index < command_line.size(); ++index) {
    if (command_line[index] == switch_name && index + 1 < command_line.size()) {
      value = command_line[index + 1];
      ++index;
    } else if (command_line[index].rfind(switch_prefix, 0) == 0) {
      value = command_line[index].substr(switch_prefix.size());
    }
  }
  return ToLowerAscii(value);
}

static bool HasCommandLineSwitch(const std::vector<std::string>& command_line,
                                 const char* name) {
  const auto switch_name = std::string("--") + name;
  const auto switch_prefix = switch_name + "=";
  for (auto index = size_t{1}; index < command_line.size(); ++index) {
    if (command_line[index] == switch_name ||
        command_line[index].rfind(switch_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

static bool StringEqualsIgnoreCase(const char* value, const char* expected) {
  if (value == nullptr) {
    return false;
  }
  return ToLowerAscii(value) == expected;
}

static bool IsNonEmptyString(const char* value) {
  return value != nullptr && value[0] != '\0';
}

static MuonLinuxDisplayBackend ResolveOzonePlatformValue(
    const std::string& value) {
  if (value == "x11") {
    return kMuonLinuxDisplayBackendX11;
  }
  if (value == "wayland") {
    return kMuonLinuxDisplayBackendWayland;
  }
  return kMuonLinuxDisplayBackendUnknown;
}

}  // namespace

MuonLinuxDisplayBackend ResolveMuonLinuxDisplayBackend(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display) {
  const auto ozone_platform =
      ResolveOzonePlatformValue(
          GetCommandLineSwitchValue(command_line, "ozone-platform"));
  if (ozone_platform != kMuonLinuxDisplayBackendUnknown) {
    return ozone_platform;
  }

  const auto ozone_platform_hint =
      ResolveOzonePlatformValue(
          GetCommandLineSwitchValue(command_line, "ozone-platform-hint"));
  if (ozone_platform_hint != kMuonLinuxDisplayBackendUnknown) {
    return ozone_platform_hint;
  }

  if (StringEqualsIgnoreCase(xdg_session_type, "x11")) {
    return kMuonLinuxDisplayBackendX11;
  }
  if (StringEqualsIgnoreCase(xdg_session_type, "wayland") ||
      IsNonEmptyString(wayland_display)) {
    return kMuonLinuxDisplayBackendWayland;
  }
  if (IsNonEmptyString(display)) {
    return kMuonLinuxDisplayBackendX11;
  }
  return kMuonLinuxDisplayBackendUnknown;
}

bool ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display) {
  return ResolveMuonLinuxDisplayBackend(command_line, xdg_session_type,
                                        wayland_display, display) ==
         kMuonLinuxDisplayBackendWayland;
}

bool ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display) {
  return ResolveMuonLinuxDisplayBackend(command_line, xdg_session_type,
                                        wayland_display, display) ==
             kMuonLinuxDisplayBackendWayland &&
         !HasCommandLineSwitch(command_line, "use-gl") &&
         !HasCommandLineSwitch(command_line, "use-angle");
}
