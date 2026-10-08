/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "log/muon_log.h"

#include <cardio.h>

#if CARDIO_WITH_GLIB
#include <gio/gio.h>
#endif

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static std::filesystem::path CreateTestDirectory() {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  if (error) {
    return {};
  }
  const auto unique =
      std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count());
  const auto directory = base / ("muon-log-" + unique);
  if (!std::filesystem::create_directories(directory, error) || error) {
    return {};
  }
  return directory;
}

static bool WriteFile(const std::filesystem::path& path,
                      const std::string& content,
                      std::ios::openmode mode = std::ios::trunc) {
  std::ofstream output(path, std::ios::binary | mode);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

static bool ReadFile(const std::filesystem::path& path, std::string* content) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  content->assign((std::istreambuf_iterator<char>(input)),
                  std::istreambuf_iterator<char>());
  return input.eof() || !input.fail();
}

static bool Contains(const std::string& text, const std::string& expected) {
  return text.find(expected) != std::string::npos;
}

static cardio::promise<void> ShutdownAfterDelay(
    cardio::dispatcher_group* group,
    uint64_t milliseconds) {
  co_await cardio::promises::delay(milliseconds);
  group->shutdown();
}

#if CARDIO_WITH_GLIB
class FileContentWatcher final {
 public:
  FileContentWatcher(std::filesystem::path path,
                     std::string expected,
                     cardio::dispatcher_group* group)
      : path_(std::move(path)),
        expected_(std::move(expected)),
        group_(group) {}

  ~FileContentWatcher() {
    if (monitor_ != nullptr && handler_id_ != 0) {
      g_signal_handler_disconnect(monitor_, handler_id_);
    }
    if (monitor_ != nullptr) {
      g_object_unref(monitor_);
    }
  }

  FileContentWatcher(const FileContentWatcher&) = delete;
  FileContentWatcher& operator=(const FileContentWatcher&) = delete;

  bool Start() {
    auto* file = g_file_new_for_path(path_.c_str());
    if (file == nullptr) {
      return false;
    }
    monitor_ = g_file_monitor_file(
        file, G_FILE_MONITOR_NONE, nullptr, nullptr);
    g_object_unref(file);
    if (monitor_ == nullptr) {
      return false;
    }
    handler_id_ = g_signal_connect(
        monitor_, "changed", G_CALLBACK(OnChanged), this);
    Check();
    return true;
  }

  bool found() const {
    return found_;
  }

 private:
  static void OnChanged(GFileMonitor* monitor,
                        GFile* file,
                        GFile* other_file,
                        GFileMonitorEvent event,
                        gpointer user_data) {
    (void)monitor;
    (void)file;
    (void)other_file;
    (void)event;
    static_cast<FileContentWatcher*>(user_data)->Check();
  }

  void Check() {
    std::string content;
    if (!found_ && ReadFile(path_, &content) &&
        Contains(content, expected_)) {
      found_ = true;
      if (group_ != nullptr) {
        group_->shutdown();
      }
    }
  }

  std::filesystem::path path_;
  std::string expected_;
  cardio::dispatcher_group* group_ = nullptr;
  GFileMonitor* monitor_ = nullptr;
  gulong handler_id_ = 0;
  bool found_ = false;
};
#endif

static MuonLogConfig CreateFileLogConfig(const std::filesystem::path& path) {
  MuonLogConfig config;
  config.output.type = kMuonLogOutputFile;
  config.output.path = path;
  return config;
}

static std::filesystem::path GetTestInternalCefLogPath(
    const std::filesystem::path& test_directory) {
  return GetMuonInternalCefLogPath(test_directory, MuonBrowserConfig().profile);
}

static bool RunInternalCefLogPathTest(
    const std::filesystem::path& test_directory) {
  const auto relative_profile_path =
      GetMuonInternalCefLogPath(test_directory, "profiles/custom");
  const auto absolute_profile = test_directory / "absolute-profile";
  const auto absolute_profile_path =
      GetMuonInternalCefLogPath(test_directory, absolute_profile);
  return Expect(relative_profile_path ==
                    (test_directory / "profiles" / "custom" /
                     "muon-cef.log")
                        .lexically_normal(),
                "relative browser.profilePath was not used for CEF log path") &&
         Expect(absolute_profile_path ==
                    (absolute_profile / "muon-cef.log").lexically_normal(),
                "absolute browser.profilePath was not used for CEF log path");
}

static bool RunFileLoggerTest(const std::filesystem::path& test_directory) {
  const auto output_path = std::filesystem::path("logs") / "muon.log";
  const auto internal_cef_path = GetTestInternalCefLogPath(test_directory);
  auto config = CreateFileLogConfig(output_path);
  std::string error_message;
  if (!Expect(InitializeMuonLogger(config, test_directory, internal_cef_path,
                                   &error_message),
              error_message)) {
    return false;
  }
  LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelDebug,
                 "filtered debug message");
  LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelInfo, "visible info");
  LogMuonMessage(kMuonLogSourceConsole, kMuonLogLevelDebug,
                 "console debug");
  LogMuonMessage(kMuonLogSourceCef, kMuonLogLevelInfo, "filtered cef info");
  ShutdownMuonLogger();

  std::string content;
  if (!Expect(ReadFile(test_directory / output_path, &content),
              "failed to read log output")) {
    return false;
  }
  return Expect(Contains(content, "][info][muon] visible info"),
                "muon info log was not written") &&
         Expect(Contains(content, "][debug][console] console debug"),
                "console debug log was not written") &&
         Expect(!Contains(content, "filtered debug message"),
                "muon debug log was not filtered") &&
         Expect(!Contains(content, "filtered cef info"),
                "cef info log was not filtered") &&
         Expect(!content.empty() && content[0] == '[',
                "log line did not use human-readable prefix");
}

static bool CaptureFdLog(const std::filesystem::path& test_directory,
                         int fd,
                         MuonLogOutputType type,
                         std::string* content) {
  const auto capture_path =
      test_directory / (type == kMuonLogOutputStdout ? "stdout.log"
                                                     : "stderr.log");
  const auto capture_fd =
      open(capture_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (capture_fd < 0) {
    return false;
  }
  const auto saved_fd = dup(fd);
  if (saved_fd < 0) {
    close(capture_fd);
    return false;
  }
  fflush(stdout);
  fflush(stderr);
  if (dup2(capture_fd, fd) < 0) {
    close(saved_fd);
    close(capture_fd);
    return false;
  }
  close(capture_fd);

  MuonLogConfig config;
  config.output.type = type;
  std::string error_message;
  const auto initialized = InitializeMuonLogger(
      config, test_directory, GetTestInternalCefLogPath(test_directory),
      &error_message);
  if (initialized) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelInfo,
                   type == kMuonLogOutputStdout ? "stdout sink"
                                                : "stderr sink");
    ShutdownMuonLogger();
  }
  fflush(stdout);
  fflush(stderr);
  const auto restored = dup2(saved_fd, fd) >= 0;
  close(saved_fd);
  return initialized && restored && ReadFile(capture_path, content);
}

static bool RunStreamLoggerTest(const std::filesystem::path& test_directory) {
  std::string stdout_content;
  std::string stderr_content;
  return Expect(CaptureFdLog(test_directory, STDOUT_FILENO,
                             kMuonLogOutputStdout, &stdout_content),
                "failed to capture stdout logger") &&
         Expect(CaptureFdLog(test_directory, STDERR_FILENO,
                             kMuonLogOutputStderr, &stderr_content),
                "failed to capture stderr logger") &&
         Expect(Contains(stdout_content, "][info][muon] stdout sink"),
                "stdout sink did not receive log line") &&
         Expect(Contains(stderr_content, "][info][muon] stderr sink"),
                "stderr sink did not receive log line");
}

static bool RunFileFailureTest(const std::filesystem::path& test_directory) {
  const auto blocked = test_directory / "blocked";
  if (!Expect(WriteFile(blocked, "not a directory"),
              "failed to write blocked path")) {
    return false;
  }
  auto config = CreateFileLogConfig(std::filesystem::path("blocked") /
                                    "muon.log");
  std::string error_message;
  return Expect(!InitializeMuonLogger(config, test_directory,
                                      GetTestInternalCefLogPath(test_directory),
                                      &error_message),
                "logger accepted an invalid output file path") &&
         Expect(Contains(error_message, "Failed to create log directory"),
                "file failure did not produce a directory diagnostic");
}

static bool RunCefForwarderRequiresDispatcherTest(
    const std::filesystem::path& test_directory) {
  const auto internal_cef_path = GetTestInternalCefLogPath(test_directory);
  std::string error_message;
  const auto started =
      StartMuonCefLogForwarder(internal_cef_path, &error_message);
  if (started) {
    StopMuonCefLogForwarder();
  }
  return Expect(!started, "CEF forwarder started without a dispatcher") &&
         Expect(Contains(error_message, "dispatcher"),
                "dispatcher failure did not produce a diagnostic");
}

static bool RunCefForwarderTest(const std::filesystem::path& test_directory) {
#if !CARDIO_WITH_GLIB
  (void)test_directory;
  return true;
#else
  auto group = cardio::dispatcher_group_glib();
  auto host = cardio::dispatcher_host_glib_auto(group);
  const auto output_path = std::filesystem::path("logs") / "cef-forward.log";
  const auto output_full_path = test_directory / output_path;
  const auto internal_cef_path = GetTestInternalCefLogPath(test_directory);
  auto config = CreateFileLogConfig(output_path);
  config.cef = kMuonLogLevelDebug;
  std::string error_message;
  if (!Expect(InitializeMuonLogger(config, test_directory, internal_cef_path,
                                   &error_message),
              error_message) ||
      !Expect(StartMuonCefLogForwarder(internal_cef_path, &error_message),
              error_message)) {
    return false;
  }
  auto watcher = FileContentWatcher(
      output_full_path, "][error][cef] ", &group);
  if (!Expect(watcher.Start(), "failed to watch log output")) {
    StopMuonCefLogForwarder();
    ShutdownMuonLogger();
    return false;
  }
  auto timeout = ShutdownAfterDelay(&group, 3000);
  (void)timeout;
  if (!Expect(WriteFile(internal_cef_path,
                        "[0529/123456.789:ERROR:sample.cc(1)] cef failure\n",
                        std::ios::app),
              "failed to append internal cef log")) {
    StopMuonCefLogForwarder();
    ShutdownMuonLogger();
    return false;
  }
  host.park();
  const auto found = watcher.found();
  StopMuonCefLogForwarder();
  ShutdownMuonLogger();
  return Expect(found, "CEF forwarder did not relay appended log line");
#endif
}

int main() {
  const auto test_directory = CreateTestDirectory();
  if (!Expect(!test_directory.empty(), "failed to create test directory")) {
    return 1;
  }

  const auto passed = RunInternalCefLogPathTest(test_directory) &&
                      RunFileLoggerTest(test_directory) &&
                      RunStreamLoggerTest(test_directory) &&
                      RunFileFailureTest(test_directory) &&
                      RunCefForwarderRequiresDispatcherTest(test_directory) &&
                      RunCefForwarderTest(test_directory);

  std::error_code error;
  std::filesystem::remove_all(test_directory, error);
  return passed ? 0 : 1;
}
