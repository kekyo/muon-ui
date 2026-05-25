/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <filesystem>
#include <string>

/**
 * Autostart state reported by the platform backend.
 */
enum MuonAutostartStatus {
  /** The current application is not registered for autostart. */
  kMuonAutostartStatusDisabled,
  /** The current application is registered for autostart. */
  kMuonAutostartStatusEnabled,
  /** The active launch source cannot report a definite state. */
  kMuonAutostartStatusUnknown,
};

/**
 * Platform autostart request parameters.
 */
struct MuonAutostartOptions final {
  /** Executable path registered as the startup command. */
  std::filesystem::path executable_path;
  /** Launch source that selects the platform backend. */
  std::string launch_source;
};

/**
 * Creates autostart options for the currently running Muon process.
 *
 * @param options Receives executable path and launch source.
 * @param error_message Receives a diagnostic on failure.
 * @return true when options were created.
 */
bool CreateDefaultMuonAutostartOptions(MuonAutostartOptions* options,
                                       std::string* error_message);

/**
 * Reads the autostart state for the selected backend.
 *
 * @param options Autostart backend options.
 * @param status Receives the current autostart state.
 * @param error_message Receives a diagnostic on failure.
 * @return true when the state was read or determined to be unknown.
 */
bool GetMuonAutostartStatus(const MuonAutostartOptions& options,
                            MuonAutostartStatus* status,
                            std::string* error_message);

/**
 * Enables or disables autostart for the selected backend.
 *
 * @param options Autostart backend options.
 * @param enabled Whether autostart should be enabled.
 * @param error_message Receives a diagnostic on failure.
 * @return true when the backend operation succeeded.
 */
bool SetMuonAutostart(const MuonAutostartOptions& options,
                      bool enabled,
                      std::string* error_message);
