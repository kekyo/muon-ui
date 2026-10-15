/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "config/muon_autostart.h"
#include "config/muon_startup.h"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
  const auto directory = base / ("muon-autostart-" + unique);
  if (!std::filesystem::create_directories(directory, error) || error) {
    return {};
  }
  return directory;
}

static bool WriteFile(const std::filesystem::path& path,
                      const std::string& content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

static std::string QuoteDesktopExec(const std::filesystem::path& path) {
  std::string result = "\"";
  const auto text = path.string();
  for (const auto character : text) {
    switch (character) {
      case '"':
      case '\\':
      case '$':
      case '`':
        result.push_back('\\');
        result.push_back(character);
        break;
      default:
        result.push_back(character);
        break;
    }
  }
  result.push_back('"');
  return result;
}

static void SetEnvironment(const char* name, const std::filesystem::path& path) {
  setenv(name, path.string().c_str(), 1);
}

static MuonAutostartOptions CreateTestOptions(
    const std::filesystem::path& test_directory,
    const std::string& launch_source) {
  MuonAutostartOptions options;
  options.executable_path = test_directory / "bin" / "sample-app";
  options.launch_source = launch_source;
  return options;
}

static bool ExpectStatus(const MuonAutostartOptions& options,
                         MuonAutostartStatus expected,
                         const std::string& message) {
  MuonAutostartStatus status = kMuonAutostartStatusUnknown;
  std::string error_message;
  return Expect(GetMuonAutostartStatus(options, &status, &error_message),
                error_message) &&
         Expect(status == expected, message);
}

static bool RunXdgAutostartRoundtripTest(
    const std::filesystem::path& test_directory,
    const std::string& launch_source) {
  const auto user_config = test_directory / "user-config";
  const auto system_config = test_directory / "empty-system-config";
  SetEnvironment("XDG_CONFIG_HOME", user_config);
  SetEnvironment("XDG_CONFIG_DIRS", system_config);

  const auto options = CreateTestOptions(test_directory, launch_source);
  std::string error_message;
  return ExpectStatus(options, kMuonAutostartStatusDisabled,
                      "autostart should begin disabled") &&
         Expect(SetMuonAutostart(options, true, &error_message),
                error_message) &&
         ExpectStatus(options, kMuonAutostartStatusEnabled,
                      "autostart should be enabled after set true") &&
         Expect(SetMuonAutostart(options, false, &error_message),
                error_message) &&
         ExpectStatus(options, kMuonAutostartStatusDisabled,
                      "autostart should be disabled after set false");
}

static bool RunXdgSystemShadowTest(
    const std::filesystem::path& test_directory) {
  const auto user_config = test_directory / "shadow-user-config";
  const auto system_config = test_directory / "system-config";
  SetEnvironment("XDG_CONFIG_HOME", user_config);
  SetEnvironment("XDG_CONFIG_DIRS", system_config);

  const auto options = CreateTestOptions(test_directory, "normal");
  const auto desktop_path =
      system_config / "autostart" / "sample-app.desktop";
  const auto content = std::string("[Desktop Entry]\n") +
                       "Type=Application\n" +
                       "Name=sample-app\n" +
                       "Exec=" + QuoteDesktopExec(options.executable_path) +
                       "\nTerminal=false\n";
  std::string error_message;
  return Expect(WriteFile(desktop_path, content),
                "failed to write system autostart entry") &&
         ExpectStatus(options, kMuonAutostartStatusEnabled,
                      "system autostart entry should be enabled") &&
         Expect(SetMuonAutostart(options, false, &error_message),
                error_message) &&
         ExpectStatus(options, kMuonAutostartStatusDisabled,
                      "user Hidden=true entry should disable system entry");
}

static bool RunUnknownLaunchSourceTest(
    const std::filesystem::path& test_directory) {
  const auto options = CreateTestOptions(test_directory, "flatpak");
  std::string error_message;
  return ExpectStatus(options, kMuonAutostartStatusUnknown,
                      "unknown launch source should report unknown status") &&
         Expect(!SetMuonAutostart(options, true, &error_message),
                "unknown launch source should reject setAutostart") &&
         Expect(error_message.find("unsupported") != std::string::npos,
                "unknown launch source error should mention unsupported");
}

static bool RunLaunchSourceParsingTest() {
  return Expect(GetMuonLaunchSourceFromCommandLine({"muon"}) == "none",
                "missing launch source should default to none") &&
         Expect(GetMuonLaunchSourceFromCommandLine(
                    {"muon", "--muon-launch-from=none"}) == "none",
                "none launch source should be parsed") &&
         Expect(GetMuonLaunchSourceFromCommandLine(
                    {"muon", "--muon-launch-from=normal"}) == "normal",
                "normal launch source should be parsed") &&
         Expect(GetMuonLaunchSourceFromCommandLine(
                    {"muon", "--muon-launch-from=normal",
                     "--muon-launch-from=flatpak"}) == "flatpak",
                "last launch source should win");
}

static bool RunAppImageRelocationTest(
    const std::filesystem::path& test_directory) {
  const auto user_config = test_directory / "appimage-config";
  SetEnvironment("XDG_CONFIG_HOME", user_config);
  SetEnvironment("XDG_CONFIG_DIRS", test_directory / "empty-system-config");
  SetEnvironment("MUON_LAUNCHER_APP_ID", "scope.appimage");
  const auto original = test_directory / "日本語 original.AppImage";
  SetEnvironment("MUON_LAUNCHER_APPIMAGE", original);
  MuonAutostartOptions options;
  std::string error;
  if (!Expect(CreateDefaultMuonAutostartOptions(&options, &error), error) ||
      !Expect(options.executable_path == original,
              "AppImage autostart must use the distribution path") ||
      !Expect(SetMuonAutostart(options, true, &error), error)) return false;
  const auto desktop = user_config / "autostart" / "scope.appimage.desktop";
  if (!Expect(std::filesystem::exists(desktop),
              "AppImage autostart must use a stable application ID")) return false;
  {
    std::ofstream output(desktop, std::ios::app);
    output << "OnlyShowIn=GNOME;\n";
  }

  const auto moved = test_directory / "renamed 日本語 $ %f \\ \".AppImage";
  SetEnvironment("MUON_LAUNCHER_APPIMAGE", moved);
  if (!Expect(CreateDefaultMuonAutostartOptions(&options, &error), error) ||
      !Expect(RefreshMuonAppImageAutostart(options, &error), error) ||
      !ExpectStatus(options, kMuonAutostartStatusEnabled,
                    "relocated AppImage must remain enabled")) return false;
  std::ifstream input(desktop);
  const std::string content((std::istreambuf_iterator<char>(input)), {});
  if (!Expect(content.find("original.AppImage") == std::string::npos,
              "relocation must replace the old command") ||
      !Expect(content.find("renamed 日本語") != std::string::npos,
              "relocation must register the new command") ||
      !Expect(content.find("OnlyShowIn=GNOME;") != std::string::npos,
              "relocation must preserve user-added desktop restrictions")) return false;

  const auto hidden = content + "Hidden=true\n";
  if (!Expect(WriteFile(desktop, hidden), "write Hidden entry") ||
      !Expect(RefreshMuonAppImageAutostart(options, &error), error) ||
      !ExpectStatus(options, kMuonAutostartStatusDisabled,
                    "refresh must preserve a disabled registration")) return false;
  const auto foreign = "[Desktop Entry]\nType=Application\nName=foreign\nExec=/foreign\n";
  if (!Expect(WriteFile(desktop, foreign), "write foreign entry") ||
      !Expect(RefreshMuonAppImageAutostart(options, &error), error)) return false;
  std::ifstream foreign_input(desktop);
  const std::string actual((std::istreambuf_iterator<char>(foreign_input)), {});
  if (!Expect(actual == foreign, "refresh must preserve foreign registrations") ||
      !Expect(SetMuonAutostart(options, false, &error), error) ||
      !Expect(RefreshMuonAppImageAutostart(options, &error), error) ||
      !Expect(!std::filesystem::exists(desktop),
              "refresh must not enable autostart implicitly")) return false;
  unsetenv("MUON_LAUNCHER_APPIMAGE");
  unsetenv("MUON_LAUNCHER_APP_ID");
  return true;
}

int main() {
  const auto test_directory = CreateTestDirectory();
  if (!Expect(!test_directory.empty(), "failed to create test directory")) {
    return 1;
  }

  const auto passed =
      RunXdgAutostartRoundtripTest(test_directory, "normal") &&
      RunXdgAutostartRoundtripTest(test_directory, "none") &&
      RunXdgSystemShadowTest(test_directory) &&
      RunUnknownLaunchSourceTest(test_directory) &&
      RunLaunchSourceParsingTest() &&
      RunAppImageRelocationTest(test_directory);

  std::error_code error;
  std::filesystem::remove_all(test_directory, error);
  return passed ? 0 : 1;
}
