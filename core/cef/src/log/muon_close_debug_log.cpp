/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "log/muon_close_debug_log.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static constexpr char kMuonCloseDebugLogFileName[] = "muon-close-debug.log";

static std::mutex g_muon_close_debug_log_mutex;

static std::filesystem::path GetMuonCloseDebugExecutablePath() {
#if defined(_WIN32)
  std::wstring buffer;
  buffer.resize(MAX_PATH);
  for (;;) {
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::filesystem::current_path();
    }
    if (length < buffer.size() - 1) {
      buffer.resize(length);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
#else
  std::string buffer;
  buffer.resize(4096);
  const auto length =
      readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return std::filesystem::current_path();
  }
  buffer.resize(static_cast<size_t>(length));
  return std::filesystem::path(buffer);
#endif
}

static std::filesystem::path GetMuonCloseDebugLogPath() {
  const auto executable_path = GetMuonCloseDebugExecutablePath();
  auto directory = executable_path.parent_path();
  if (directory.empty()) {
    directory = std::filesystem::current_path();
  }
  return directory / kMuonCloseDebugLogFileName;
}

static std::string GetMuonCloseDebugTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      1000;
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
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

static unsigned long GetMuonCloseDebugProcessId() {
#if defined(_WIN32)
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

std::string FormatMuonCloseDebugPointer(const void* pointer) {
  std::ostringstream output;
  output << pointer;
  return output.str();
}

const char* FormatMuonCloseDebugBool(bool value) {
  return value ? "true" : "false";
}

void AppendMuonCloseDebugLog(const std::string& message) {
  try {
    std::lock_guard<std::mutex> lock(g_muon_close_debug_log_mutex);
    std::ofstream output(GetMuonCloseDebugLogPath(),
                         std::ios::out | std::ios::app);
    if (!output) {
      return;
    }
    output << GetMuonCloseDebugTimestamp() << " pid="
           << GetMuonCloseDebugProcessId() << " tid="
           << std::this_thread::get_id() << " " << message << '\n';
  } catch (...) {
  }
}
