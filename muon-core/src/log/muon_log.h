/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "config/muon_config.h"

#include <filesystem>
#include <string>

/**
 * Returns the canonical display name for a log level.
 *
 * @param level Log level.
 * @return Static lowercase level name.
 */
const char* GetMuonLogLevelName(MuonLogLevel level);

/**
 * Returns the canonical display name for a log source.
 *
 * @param source Log source.
 * @return Static lowercase source name.
 */
const char* GetMuonLogSourceName(MuonLogSource source);

/**
 * Returns the level configured for one source.
 *
 * @param config Log configuration.
 * @param source Log source.
 * @return Effective source level.
 */
MuonLogLevel GetMuonLogSourceLevel(const MuonLogConfig& config,
                                   MuonLogSource source);

/**
 * Returns whether a message passes the source level filter.
 *
 * @param config Log configuration.
 * @param source Log source.
 * @param level Message level.
 * @return true when the message should be emitted.
 */
bool IsMuonLogEnabled(const MuonLogConfig& config,
                      MuonLogSource source,
                      MuonLogLevel level);

/**
 * Returns the internal CEF log file path used for CEF log forwarding.
 *
 * @param base_directory Directory relative paths are resolved against.
 * @param profile_path CEF profile directory path from browser.profilePath.
 * @return Internal CEF log file path.
 */
std::filesystem::path GetMuonInternalCefLogPath(
    const std::filesystem::path& base_directory,
    const std::filesystem::path& profile_path);

/**
 * Returns the command-line value for CEF log severity.
 *
 * @param level Effective CEF source level.
 * @return CEF log-severity switch value.
 */
const char* GetMuonCefLogSeveritySwitchValue(MuonLogLevel level);

/**
 * Creates or truncates the internal CEF log file before CEF starts.
 *
 * @param path CEF log file path.
 * @param error_message Receives a startup failure diagnostic.
 * @return true when the file is ready for CEF.
 */
bool ResetMuonCefLogFile(const std::filesystem::path& path,
                         std::string* error_message);

/**
 * Initializes the process-global muon logger.
 *
 * @param config Log configuration.
 * @param base_directory Directory used for relative file paths.
 * @param internal_cef_log_path CEF forwarder input file path.
 * @param error_message Receives a startup failure diagnostic.
 * @return true when the logger is ready.
 */
bool InitializeMuonLogger(const MuonLogConfig& config,
                          const std::filesystem::path& base_directory,
                          const std::filesystem::path& internal_cef_log_path,
                          std::string* error_message);

/**
 * Shuts down the process-global muon logger.
 */
void ShutdownMuonLogger();

/**
 * Emits one message through the process-global muon logger.
 *
 * @param source Message source.
 * @param level Message level.
 * @param message UTF-8 message text.
 */
void LogMuonMessage(MuonLogSource source,
                    MuonLogLevel level,
                    const std::string& message);

/**
 * Starts forwarding CEF's internal log file to the configured muon sink.
 *
 * @param path CEF log file path.
 * @param error_message Receives a startup failure diagnostic.
 * @return true when the forwarder thread started.
 */
bool StartMuonCefLogForwarder(const std::filesystem::path& path,
                              std::string* error_message);

/**
 * Stops the CEF log forwarder, if running.
 */
void StopMuonCefLogForwarder();
