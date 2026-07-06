/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Process exit code reserved for muon recycle requests.
 */
inline constexpr int32_t kMuonRecycleExitCode = 88;

/**
 * Launch source that keeps local development startup behavior.
 */
inline constexpr char kMuonLaunchSourceNone[] = "none";

/**
 * Launch source used for platform-normal application startup.
 */
inline constexpr char kMuonLaunchSourceNormal[] = "normal";

/**
 * Command-line switch prefix used to select the muon launch source.
 */
inline constexpr char kMuonLaunchSourceSwitchPrefix[] = "--muon-launch-from=";

/**
 * Stores the process command line captured at muon startup.
 *
 * @param argc Argument count passed to main.
 * @param argv Argument vector passed to main.
 */
void SetMuonStartupCommandLine(int argc, char* argv[]);

/**
 * Extracts the muon launch source from a command line.
 *
 * @remarks Missing switch values default to `none`. When the switch appears
 * more than once, the last value is used.
 *
 * @param command_line Process command line, including argv[0] when available.
 * @return Launch source value.
 */
std::string GetMuonLaunchSourceFromCommandLine(
    const std::vector<std::string>& command_line);

/**
 * Returns the command line captured at muon startup.
 */
std::vector<std::string> GetMuonStartupCommandLine();

/**
 * Returns the launch source captured at muon startup.
 */
std::string GetMuonStartupLaunchSource();
