/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "config/muon_config.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"

#include <filesystem>

/**
 * Creates the CEF initialization settings used by the muon browser process.
 *
 * @param config Loaded muon runtime configuration.
 * @param executable_directory Directory that contains the running muon binary.
 * @param cef_log_path CEF log file path.
 * @return CEF settings configured for the current muon runtime.
 */
CefSettings CreateMuonCefSettings(
    const MuonConfig& config,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& cef_log_path);

/**
 * Applies network-related CEF command-line switches for the browser process.
 *
 * @param config Loaded muon runtime configuration.
 * @param process_type CEF process type. An empty value identifies the browser
 * process.
 * @param command_line Mutable process command line.
 */
void ConfigureMuonCefNetworkCommandLine(
    const MuonConfig& config,
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line);
