/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "config/muon_config.h"
#include "include/cef_app.h"

#include <filesystem>

/**
 * Creates the CEF initialization settings used by the Muon browser process.
 *
 * @param config Loaded Muon runtime configuration.
 * @param executable_directory Directory that contains the running Muon binary.
 * @param cef_log_path CEF log file path.
 * @return CEF settings configured for the current Muon runtime.
 */
CefSettings CreateMuonCefSettings(
    const MuonConfig& config,
    const std::filesystem::path& executable_directory,
    const std::filesystem::path& cef_log_path);
