/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "log/muon_log.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <gio/gio.h>
#include <syslog.h>
#endif

#include <cardio.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

struct MuonLoggerState {
  MuonLogConfig config;
  std::filesystem::path file_path;
  std::ofstream file;
#if defined(_WIN32)
  HANDLE event_source = nullptr;
#endif
};

struct MuonCefLogForwarderState {
  std::filesystem::path path;
  std::ifstream input;
#if defined(_WIN32)
  HANDLE change_handle = INVALID_HANDLE_VALUE;
  cardio::cancellation_source cancellation_source;
  std::unique_ptr<cardio::promise<void>> task;
#else
  GFileMonitor* monitor = nullptr;
  gulong monitor_handler = 0;
#endif
};

static std::unique_ptr<MuonLoggerState> g_muon_logger;
static std::unique_ptr<MuonCefLogForwarderState> g_muon_cef_forwarder;

static std::filesystem::path ResolveMuonLogPath(
    const std::filesystem::path& base_directory,
    const std::filesystem::path& path) {
  if (path.is_absolute()) {
    return path.lexically_normal();
  }
  return (base_directory / path).lexically_normal();
}

static bool EnsureMuonLogParentDirectory(
    const std::filesystem::path& path,
    std::string* error_message) {
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return true;
  }
  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error) {
    *error_message = "Failed to create log directory: " + parent.string();
    return false;
  }
  return true;
}

static std::string NormalizeMuonLogMessage(const std::string& message) {
  std::string normalized;
  normalized.reserve(message.size());
  for (const auto character : message) {
    if (character == '\n') {
      normalized += "\\n";
    } else if (character == '\r') {
      normalized += "\\r";
    } else {
      normalized += character;
    }
  }
  return normalized;
}

static std::string FormatMuonLogTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      1000;
  std::tm local_time;
#if defined(_WIN32)
  localtime_s(&local_time, &time);
#else
  localtime_r(&time, &local_time);
#endif
  std::ostringstream output;
  output << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << "."
         << std::setw(3) << std::setfill('0') << milliseconds.count();
  return output.str();
}

static std::string FormatMuonLogLine(MuonLogSource source,
                                     MuonLogLevel level,
                                     const std::string& message) {
  return "[" + FormatMuonLogTimestamp() + "][" +
         GetMuonLogLevelName(level) + "][" + GetMuonLogSourceName(source) +
         "] " + NormalizeMuonLogMessage(message);
}

#if !defined(_WIN32)
static int GetMuonSyslogPriority(MuonLogLevel level) {
  switch (level) {
    case kMuonLogLevelDebug:
      return LOG_DEBUG;
    case kMuonLogLevelInfo:
      return LOG_INFO;
    case kMuonLogLevelWarning:
      return LOG_WARNING;
    case kMuonLogLevelError:
      return LOG_ERR;
    case kMuonLogLevelFatal:
      return LOG_CRIT;
    case kMuonLogLevelOff:
      return LOG_DEBUG;
  }
  return LOG_INFO;
}
#endif

#if defined(_WIN32)
static WORD GetMuonEventLogType(MuonLogLevel level) {
  switch (level) {
    case kMuonLogLevelWarning:
      return EVENTLOG_WARNING_TYPE;
    case kMuonLogLevelError:
    case kMuonLogLevelFatal:
      return EVENTLOG_ERROR_TYPE;
    case kMuonLogLevelDebug:
    case kMuonLogLevelInfo:
    case kMuonLogLevelOff:
      return EVENTLOG_INFORMATION_TYPE;
  }
  return EVENTLOG_INFORMATION_TYPE;
}
#endif

static void WriteMuonLogLine(MuonLoggerState* logger,
                             MuonLogLevel level,
                             const std::string& line) {
  const auto output_line = line + "\n";
  switch (logger->config.output.type) {
    case kMuonLogOutputStdout:
      std::fwrite(output_line.data(), 1, output_line.size(), stdout);
      std::fflush(stdout);
      return;
    case kMuonLogOutputStderr:
      std::fwrite(output_line.data(), 1, output_line.size(), stderr);
      std::fflush(stderr);
      return;
    case kMuonLogOutputFile:
      logger->file << output_line;
      logger->file.flush();
      return;
    case kMuonLogOutputDebug:
#if defined(_WIN32)
      OutputDebugStringA(output_line.c_str());
#endif
      return;
    case kMuonLogOutputEventLog:
#if defined(_WIN32)
      if (logger->event_source != nullptr) {
        const char* strings[] = {line.c_str()};
        ReportEventA(logger->event_source, GetMuonEventLogType(level), 0, 0,
                     nullptr, 1, 0, strings, nullptr);
      }
#endif
      return;
    case kMuonLogOutputSyslog:
#if !defined(_WIN32)
      syslog(GetMuonSyslogPriority(level), "%s", line.c_str());
#endif
      return;
  }
}

static MuonLogLevel ParseMuonCefForwardedLogLevel(const std::string& line) {
  if (line.find(":FATAL:") != std::string::npos) {
    return kMuonLogLevelFatal;
  }
  if (line.find(":ERROR:") != std::string::npos) {
    return kMuonLogLevelError;
  }
  if (line.find(":WARNING:") != std::string::npos) {
    return kMuonLogLevelWarning;
  }
  if (line.find(":INFO:") != std::string::npos) {
    return kMuonLogLevelInfo;
  }
  if (line.find(":VERBOSE") != std::string::npos ||
      line.find(":DEBUG:") != std::string::npos) {
    return kMuonLogLevelDebug;
  }
  return kMuonLogLevelInfo;
}

static void DrainMuonCefForwardedLogLines(
    MuonCefLogForwarderState* forwarder) {
  if (forwarder == nullptr) {
    return;
  }
  if (!forwarder->input.is_open()) {
    forwarder->input.open(forwarder->path, std::ios::binary);
    if (!forwarder->input) {
      forwarder->input.close();
      return;
    }
  }

  std::string line;
  while (std::getline(forwarder->input, line)) {
    LogMuonMessage(kMuonLogSourceCef, ParseMuonCefForwardedLogLevel(line),
                   line);
  }
  if (forwarder->input.eof()) {
    forwarder->input.clear();
  }
}

#if defined(_WIN32)
static cardio::promise<void> RunMuonCefLogForwarder(
    MuonCefLogForwarderState* forwarder,
    cardio::cancellation cancellation) {
  try {
    while (forwarder != nullptr &&
           forwarder->change_handle != INVALID_HANDLE_VALUE) {
      cancellation.throw_if_cancellation_requested();
      (void)co_await cardio::from_win32_handle(
          forwarder->change_handle, cancellation);
      DrainMuonCefForwardedLogLines(forwarder);
      if (!FindNextChangeNotification(forwarder->change_handle)) {
        co_return;
      }
    }
  } catch (const cardio::canceled_exception&) {
  }
}

static void CloseMuonCefLogForwarderHandle(
    MuonCefLogForwarderState* forwarder) {
  if (forwarder != nullptr &&
      forwarder->change_handle != INVALID_HANDLE_VALUE) {
    FindCloseChangeNotification(forwarder->change_handle);
    forwarder->change_handle = INVALID_HANDLE_VALUE;
  }
}

static void FinishReadyMuonCefLogForwarderTask(
    MuonCefLogForwarderState* forwarder) {
  if (forwarder == nullptr || !forwarder->task) {
    return;
  }
  try {
    if (forwarder->task->is_ready()) {
      forwarder->task->try_result();
      forwarder->task.reset();
    }
  } catch (...) {
    forwarder->task.reset();
  }
}
#else
static void OnMuonCefLogFileChanged(GFileMonitor* monitor,
                                    GFile* file,
                                    GFile* other_file,
                                    GFileMonitorEvent event,
                                    gpointer user_data) {
  (void)monitor;
  (void)file;
  (void)other_file;
  (void)event;
  DrainMuonCefForwardedLogLines(
      static_cast<MuonCefLogForwarderState*>(user_data));
}
#endif

const char* GetMuonLogLevelName(MuonLogLevel level) {
  switch (level) {
    case kMuonLogLevelDebug:
      return "debug";
    case kMuonLogLevelInfo:
      return "info";
    case kMuonLogLevelWarning:
      return "warning";
    case kMuonLogLevelError:
      return "error";
    case kMuonLogLevelFatal:
      return "fatal";
    case kMuonLogLevelOff:
      return "off";
  }
  return "info";
}

const char* GetMuonLogSourceName(MuonLogSource source) {
  switch (source) {
    case kMuonLogSourceMuon:
      return "muon";
    case kMuonLogSourceCef:
      return "cef";
    case kMuonLogSourceConsole:
      return "console";
    case kMuonLogSourcePlugin:
      return "plugin";
  }
  return "muon";
}

MuonLogLevel GetMuonLogSourceLevel(const MuonLogConfig& config,
                                   MuonLogSource source) {
  switch (source) {
    case kMuonLogSourceMuon:
      return config.muon;
    case kMuonLogSourceCef:
      return config.cef;
    case kMuonLogSourceConsole:
      return config.console;
    case kMuonLogSourcePlugin:
      return config.plugin;
  }
  return config.level;
}

bool IsMuonLogEnabled(const MuonLogConfig& config,
                      MuonLogSource source,
                      MuonLogLevel level) {
  const auto source_level = GetMuonLogSourceLevel(config, source);
  return source_level != kMuonLogLevelOff && level >= source_level &&
         level != kMuonLogLevelOff;
}

std::filesystem::path GetMuonInternalCefLogPath(
    const std::filesystem::path& base_directory,
    const std::filesystem::path& profile_path) {
  const auto resolved_profile_path =
      profile_path.is_absolute() ? profile_path : base_directory / profile_path;
  return (resolved_profile_path / "muon-cef.log").lexically_normal();
}

const char* GetMuonCefLogSeveritySwitchValue(MuonLogLevel level) {
  switch (level) {
    case kMuonLogLevelDebug:
      return "verbose";
    case kMuonLogLevelInfo:
      return "info";
    case kMuonLogLevelWarning:
      return "warning";
    case kMuonLogLevelError:
      return "error";
    case kMuonLogLevelFatal:
      return "fatal";
    case kMuonLogLevelOff:
      return "disable";
  }
  return "info";
}

bool ResetMuonCefLogFile(const std::filesystem::path& path,
                         std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (!EnsureMuonLogParentDirectory(path, error_message)) {
    return false;
  }
  std::ofstream output(path, std::ios::trunc | std::ios::binary);
  if (!output) {
    *error_message = "Failed to initialize internal CEF log file: " +
                     path.string();
    return false;
  }
  return true;
}

bool InitializeMuonLogger(const MuonLogConfig& config,
                          const std::filesystem::path& base_directory,
                          const std::filesystem::path& internal_cef_log_path,
                          std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  ShutdownMuonLogger();
  error_message->clear();

  auto logger = std::make_unique<MuonLoggerState>();
  logger->config = config;
  if (logger->config.output.type == kMuonLogOutputFile) {
    logger->file_path =
        ResolveMuonLogPath(base_directory, logger->config.output.path);
    if (logger->file_path == internal_cef_log_path.lexically_normal()) {
      *error_message = "log output file must differ from internal CEF log file";
      return false;
    }
    if (!EnsureMuonLogParentDirectory(logger->file_path, error_message)) {
      return false;
    }
    logger->file.open(logger->file_path, std::ios::app | std::ios::binary);
    if (!logger->file) {
      *error_message = "Failed to open log file: " + logger->file_path.string();
      return false;
    }
  }
#if defined(_WIN32)
  if (logger->config.output.type == kMuonLogOutputEventLog) {
    logger->event_source = RegisterEventSourceA(nullptr, "muon");
    if (logger->event_source == nullptr) {
      *error_message = "Failed to register muon event log source";
      return false;
    }
  }
#else
  if (logger->config.output.type == kMuonLogOutputSyslog) {
    openlog("muon", LOG_PID, LOG_USER);
  }
#endif

  g_muon_logger = std::move(logger);
  return true;
}

void ShutdownMuonLogger() {
  StopMuonCefLogForwarder();
  if (!g_muon_logger) {
    return;
  }
#if defined(_WIN32)
  if (g_muon_logger->event_source != nullptr) {
    DeregisterEventSource(g_muon_logger->event_source);
    g_muon_logger->event_source = nullptr;
  }
#else
  if (g_muon_logger->config.output.type == kMuonLogOutputSyslog) {
    closelog();
  }
#endif
  g_muon_logger.reset();
}

void LogMuonMessage(MuonLogSource source,
                    MuonLogLevel level,
                    const std::string& message) {
  if (!g_muon_logger ||
      !IsMuonLogEnabled(g_muon_logger->config, source, level)) {
    return;
  }
  WriteMuonLogLine(g_muon_logger.get(), level,
                   FormatMuonLogLine(source, level, message));
}

bool StartMuonCefLogForwarder(const std::filesystem::path& path,
                              std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  StopMuonCefLogForwarder();
  error_message->clear();
  if (cardio::unsafe_get_current_dispatcher() == nullptr) {
    *error_message = "muon main dispatcher is unavailable";
    return false;
  }
  if (!EnsureMuonLogParentDirectory(path, error_message)) {
    return false;
  }
  {
    std::ofstream output(path, std::ios::app | std::ios::binary);
    if (!output) {
      *error_message = "Failed to initialize internal CEF log file: " +
                       path.string();
      return false;
    }
  }

  auto forwarder = std::make_unique<MuonCefLogForwarderState>();
  forwarder->path = path;
#if defined(_WIN32)
  auto directory = path.parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  forwarder->change_handle = FindFirstChangeNotificationW(
      directory.wstring().c_str(), FALSE,
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
          FILE_NOTIFY_CHANGE_SIZE);
  if (forwarder->change_handle == INVALID_HANDLE_VALUE) {
    *error_message = "Failed to monitor internal CEF log file";
    return false;
  }
  DrainMuonCefForwardedLogLines(forwarder.get());
  forwarder->task = std::make_unique<cardio::promise<void>>(
      RunMuonCefLogForwarder(
          forwarder.get(), forwarder->cancellation_source.get_cancellation()));
#else
  auto* file = g_file_new_for_path(path.c_str());
  if (file == nullptr) {
    *error_message = "Failed to monitor internal CEF log file";
    return false;
  }
  GError* monitor_error = nullptr;
  forwarder->monitor = g_file_monitor_file(
      file, G_FILE_MONITOR_NONE, nullptr, &monitor_error);
  g_object_unref(file);
  if (forwarder->monitor == nullptr) {
    *error_message =
        monitor_error != nullptr && monitor_error->message != nullptr
            ? monitor_error->message
            : "Failed to monitor internal CEF log file";
    if (monitor_error != nullptr) {
      g_error_free(monitor_error);
    }
    return false;
  }
  forwarder->monitor_handler = g_signal_connect(
      forwarder->monitor, "changed",
      G_CALLBACK(OnMuonCefLogFileChanged), forwarder.get());
  DrainMuonCefForwardedLogLines(forwarder.get());
#endif
  g_muon_cef_forwarder = std::move(forwarder);
  return true;
}

void StopMuonCefLogForwarder() {
  auto forwarder = std::move(g_muon_cef_forwarder);
  if (!forwarder) {
    return;
  }
#if defined(_WIN32)
  (void)forwarder->cancellation_source.cancel();
  FinishReadyMuonCefLogForwarderTask(forwarder.get());
  if (forwarder->task) {
    // The cardio wait owns no handle, so the waited handle must remain valid
    // until the dispatcher processes cancellation. Retain the small state
    // instead of closing a handle that may still be registered.
    (void)forwarder.release();
    return;
  }
  CloseMuonCefLogForwarderHandle(forwarder.get());
#else
  if (forwarder->monitor != nullptr && forwarder->monitor_handler != 0) {
    g_signal_handler_disconnect(forwarder->monitor,
                                forwarder->monitor_handler);
    forwarder->monitor_handler = 0;
  }
  if (forwarder->monitor != nullptr) {
    g_file_monitor_cancel(forwarder->monitor);
    g_object_unref(forwarder->monitor);
    forwarder->monitor = nullptr;
  }
#endif
}
