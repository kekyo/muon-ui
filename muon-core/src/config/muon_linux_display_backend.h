/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <string>
#include <vector>

/**
 * Linux display backend resolved from Chromium command-line switches and
 * desktop environment variables.
 */
enum MuonLinuxDisplayBackend {
  /** No display backend could be selected from the available signals. */
  kMuonLinuxDisplayBackendUnknown,
  /** Chromium's X11 Ozone backend is selected. */
  kMuonLinuxDisplayBackendX11,
  /** Chromium's Wayland Ozone backend is selected. */
  kMuonLinuxDisplayBackendWayland,
};

/**
 * Resolves the Linux display backend selected for CEF/Chromium.
 *
 * @param command_line Process command line, including argv[0] when available.
 * @param xdg_session_type XDG_SESSION_TYPE environment value, or null.
 * @param wayland_display WAYLAND_DISPLAY environment value, or null.
 * @param display DISPLAY environment value, or null.
 * @return The selected display backend, or unknown when no reliable signal is
 * available.
 */
MuonLinuxDisplayBackend ResolveMuonLinuxDisplayBackend(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display);

/**
 * Returns whether CEF Vulkan surface usage should be disabled for the resolved
 * Linux display backend.
 *
 * @param command_line Process command line, including argv[0] when available.
 * @param xdg_session_type XDG_SESSION_TYPE environment value, or null.
 * @param wayland_display WAYLAND_DISPLAY environment value, or null.
 * @param display DISPLAY environment value, or null.
 * @return true when the selected display backend is Wayland.
 */
bool ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display);
