/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "config/muon_autostart.h"

#include "config/muon_paths.h"
#include "config/muon_startup.h"
#include "muon_string_helpers.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace muon_internal {

#if !defined(_WIN32)
static constexpr char kDesktopEntryGroup[] = "[Desktop Entry]";
static constexpr char kDesktopEntryExecKey[] = "Exec";
static constexpr char kDesktopEntryHiddenKey[] = "Hidden";
static constexpr char kDesktopEntryNameKey[] = "Name";
static constexpr char kDesktopEntryTypeKey[] = "Type";

struct DesktopAutostartEntry final {
  bool exists = false;
  bool hidden = false;
  bool has_exec = false;
  std::string exec;
};
#else
static constexpr wchar_t kWindowsRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr size_t kWindowsRunCommandMaxLength = 260;
#endif

static bool IsDefaultAutostartLaunchSource(const std::string& launch_source) {
  return launch_source == kMuonLaunchSourceNone ||
         launch_source == kMuonLaunchSourceNormal;
}

static bool RequireOutputArguments(const void* target,
                                   const std::string* error_message) {
  return target != nullptr && error_message != nullptr;
}

static bool ValidateAutostartOptions(const MuonAutostartOptions& options,
                                     std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (options.executable_path.empty()) {
    *error_message = "Autostart executable path is unavailable";
    return false;
  }
  if (options.executable_path.filename().empty()) {
    *error_message = "Autostart executable path has no file name";
    return false;
  }
  return true;
}

#if !defined(_WIN32)

static std::string GetAutostartEntryName(
    const std::filesystem::path& executable_path) {
  return executable_path.stem().string();
}

static std::filesystem::path GetAutostartDesktopFilePath(
    const std::filesystem::path& autostart_directory,
    const std::string& entry_name) {
  return autostart_directory / (entry_name + ".desktop");
}

static bool GetEnvironmentPath(const char* name, std::filesystem::path* path) {
  const auto* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  *path = std::filesystem::path(value);
  return true;
}

static bool GetXdgUserAutostartDirectory(std::filesystem::path* directory,
                                         std::string* error_message) {
  std::filesystem::path config_home;
  if (!GetEnvironmentPath("XDG_CONFIG_HOME", &config_home)) {
    std::filesystem::path home;
    if (!GetEnvironmentPath("HOME", &home)) {
      *error_message = "XDG_CONFIG_HOME and HOME are unavailable";
      return false;
    }
    config_home = home / ".config";
  }
  *directory = config_home / "autostart";
  return true;
}

static std::vector<std::filesystem::path> GetXdgSystemAutostartDirectories() {
  std::vector<std::filesystem::path> directories;
  const auto* value = std::getenv("XDG_CONFIG_DIRS");
  const std::string source =
      value == nullptr || value[0] == '\0' ? "/etc/xdg" : value;
  auto begin = size_t{0};
  while (begin <= source.size()) {
    const auto end = source.find(':', begin);
    const auto part = source.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!part.empty()) {
      directories.emplace_back(std::filesystem::path(part) / "autostart");
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return directories;
}

static bool EnsureDirectory(const std::filesystem::path& directory,
                            std::string* error_message) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    *error_message = "Failed to create autostart directory: " +
                     directory.string();
    return false;
  }
  return true;
}

static bool WriteTextFile(const std::filesystem::path& path,
                          const std::string& content,
                          std::string* error_message) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    *error_message = "Failed to write autostart file: " + path.string();
    return false;
  }
  output << content;
  if (!output) {
    *error_message = "Failed to write autostart file: " + path.string();
    return false;
  }
  return true;
}

static bool RemoveFileIfExists(const std::filesystem::path& path,
                               std::string* error_message) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    *error_message = "Failed to remove autostart file: " + path.string();
    return false;
  }
  return true;
}

static std::string EscapeDesktopExecArgument(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const auto character : value) {
    switch (character) {
      case '"':
      case '\\':
      case '$':
      case '`':
        escaped.push_back('\\');
        escaped.push_back(character);
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  escaped.push_back('"');
  return escaped;
}

static std::string CreateEnabledDesktopEntry(
    const MuonAutostartOptions& options,
    const std::string& entry_name) {
  std::string content;
  content += kDesktopEntryGroup;
  content += "\n";
  content += kDesktopEntryTypeKey;
  content += "=Application\n";
  content += kDesktopEntryNameKey;
  content += "=";
  content += entry_name;
  content += "\n";
  content += kDesktopEntryExecKey;
  content += "=";
  content += EscapeDesktopExecArgument(options.executable_path.string());
  content += "\nTerminal=false\n";
  return content;
}

static std::string CreateHiddenDesktopEntry(
    const MuonAutostartOptions& options,
    const std::string& entry_name) {
  std::string content = CreateEnabledDesktopEntry(options, entry_name);
  content += kDesktopEntryHiddenKey;
  content += "=true\n";
  return content;
}

static bool ReadDesktopAutostartEntry(const std::filesystem::path& path,
                                      DesktopAutostartEntry* entry,
                                      std::string* error_message) {
  entry->exists = false;
  entry->hidden = false;
  entry->has_exec = false;
  entry->exec.clear();

  std::error_code error;
  const auto exists = std::filesystem::exists(path, error);
  if (error) {
    *error_message = "Failed to inspect autostart file: " + path.string();
    return false;
  }
  if (!exists) {
    return true;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error_message = "Failed to read autostart file: " + path.string();
    return false;
  }

  entry->exists = true;
  auto in_desktop_entry = false;
  std::string line;
  while (std::getline(input, line)) {
    const auto trimmed = TrimAscii(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      in_desktop_entry = trimmed == kDesktopEntryGroup;
      continue;
    }
    if (!in_desktop_entry) {
      continue;
    }
    const auto separator = trimmed.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const auto key = TrimAscii(trimmed.substr(0, separator));
    const auto value = TrimAscii(trimmed.substr(separator + 1));
    if (key == kDesktopEntryHiddenKey) {
      entry->hidden = ToLowerAscii(value) == "true";
    } else if (key == kDesktopEntryExecKey) {
      entry->has_exec = true;
      entry->exec = value;
    }
  }
  if (!input.eof() && input.fail()) {
    *error_message = "Failed to read autostart file: " + path.string();
    return false;
  }
  return true;
}

static std::string ExtractDesktopExecCommand(const std::string& exec) {
  const auto source = TrimAscii(exec);
  if (source.empty()) {
    return "";
  }
  if (source[0] != '"') {
    auto end = size_t{0};
    while (end < source.size() && !IsAsciiSpace(source[end])) {
      ++end;
    }
    return source.substr(0, end);
  }

  std::string result;
  for (auto index = size_t{1}; index < source.size(); ++index) {
    const auto character = source[index];
    if (character == '"') {
      return result;
    }
    if (character == '\\' && index + 1 < source.size()) {
      ++index;
      result.push_back(source[index]);
      continue;
    }
    result.push_back(character);
  }
  return result;
}

static bool DesktopEntryMatchesExecutable(
    const DesktopAutostartEntry& entry,
    const std::filesystem::path& executable_path) {
  if (!entry.has_exec) {
    return false;
  }
  const auto command = ExtractDesktopExecCommand(entry.exec);
  if (command == executable_path.string()) {
    return true;
  }
  const std::filesystem::path command_path(command);
  return !command_path.empty() &&
         command_path.filename() == executable_path.filename();
}

static MuonAutostartStatus GetDesktopAutostartStatus(
    const DesktopAutostartEntry& entry,
    const std::filesystem::path& executable_path) {
  if (!entry.exists || entry.hidden) {
    return kMuonAutostartStatusDisabled;
  }
  return DesktopEntryMatchesExecutable(entry, executable_path)
             ? kMuonAutostartStatusEnabled
             : kMuonAutostartStatusDisabled;
}

static bool HasSystemDesktopEntry(const std::string& entry_name,
                                  std::string* error_message) {
  for (const auto& directory : GetXdgSystemAutostartDirectories()) {
    DesktopAutostartEntry entry;
    if (!ReadDesktopAutostartEntry(
            GetAutostartDesktopFilePath(directory, entry_name), &entry,
            error_message)) {
      return false;
    }
    if (entry.exists) {
      return true;
    }
  }
  return false;
}

static bool GetXdgAutostartStatus(const MuonAutostartOptions& options,
                                  MuonAutostartStatus* status,
                                  std::string* error_message) {
  const auto entry_name = GetAutostartEntryName(options.executable_path);
  std::filesystem::path user_directory;
  if (!GetXdgUserAutostartDirectory(&user_directory, error_message)) {
    return false;
  }

  DesktopAutostartEntry user_entry;
  if (!ReadDesktopAutostartEntry(
          GetAutostartDesktopFilePath(user_directory, entry_name), &user_entry,
          error_message)) {
    return false;
  }
  if (user_entry.exists) {
    *status = GetDesktopAutostartStatus(user_entry, options.executable_path);
    return true;
  }

  for (const auto& directory : GetXdgSystemAutostartDirectories()) {
    DesktopAutostartEntry entry;
    if (!ReadDesktopAutostartEntry(
            GetAutostartDesktopFilePath(directory, entry_name), &entry,
            error_message)) {
      return false;
    }
    if (entry.exists) {
      *status = GetDesktopAutostartStatus(entry, options.executable_path);
      return true;
    }
  }
  *status = kMuonAutostartStatusDisabled;
  return true;
}

static bool SetXdgAutostart(const MuonAutostartOptions& options,
                            bool enabled,
                            std::string* error_message) {
  const auto entry_name = GetAutostartEntryName(options.executable_path);
  std::filesystem::path user_directory;
  if (!GetXdgUserAutostartDirectory(&user_directory, error_message) ||
      !EnsureDirectory(user_directory, error_message)) {
    return false;
  }

  const auto user_path =
      GetAutostartDesktopFilePath(user_directory, entry_name);
  if (enabled) {
    return WriteTextFile(user_path,
                         CreateEnabledDesktopEntry(options, entry_name),
                         error_message);
  }

  if (HasSystemDesktopEntry(entry_name, error_message)) {
    return WriteTextFile(user_path,
                         CreateHiddenDesktopEntry(options, entry_name),
                         error_message);
  }
  return RemoveFileIfExists(user_path, error_message);
}

#else

static std::wstring QuoteWindowsCommandArgument(std::wstring_view value) {
  std::wstring quoted;
  quoted.push_back(L'"');
  auto backslashes = size_t{0};
  for (const auto character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(character);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    quoted.push_back(character);
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

static std::wstring CreateWindowsRunCommand(
    const MuonAutostartOptions& options) {
  return QuoteWindowsCommandArgument(options.executable_path.wstring());
}

static std::wstring ExtractWindowsCommandExecutable(
    const std::wstring& command) {
  auto index = size_t{0};
  while (index < command.size() && iswspace(command[index]) != 0) {
    ++index;
  }
  if (index >= command.size()) {
    return L"";
  }
  if (command[index] != L'"') {
    const auto begin = index;
    while (index < command.size() && iswspace(command[index]) == 0) {
      ++index;
    }
    return command.substr(begin, index - begin);
  }

  ++index;
  std::wstring result;
  auto backslashes = size_t{0};
  while (index < command.size()) {
    const auto character = command[index++];
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes / 2, L'\\');
      if (backslashes % 2 == 0) {
        return result;
      }
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    result.push_back(character);
    backslashes = 0;
  }
  result.append(backslashes, L'\\');
  return result;
}

static bool WindowsRunCommandMatchesExecutable(
    const std::wstring& command,
    const std::filesystem::path& executable_path) {
  const auto executable = ExtractWindowsCommandExecutable(command);
  if (_wcsicmp(executable.c_str(), executable_path.wstring().c_str()) == 0) {
    return true;
  }
  return _wcsicmp(std::filesystem::path(executable)
                      .filename()
                      .wstring()
                      .c_str(),
                  executable_path.filename().wstring().c_str()) == 0;
}

static bool GetWindowsAutostartStatus(const MuonAutostartOptions& options,
                                      MuonAutostartStatus* status,
                                      std::string* error_message) {
  HKEY key = nullptr;
  auto result =
      RegOpenKeyExW(HKEY_CURRENT_USER, kWindowsRunKey, 0, KEY_QUERY_VALUE,
                    &key);
  if (result == ERROR_FILE_NOT_FOUND) {
    *status = kMuonAutostartStatusDisabled;
    return true;
  }
  if (result != ERROR_SUCCESS) {
    *error_message = "Failed to open Windows Run registry key";
    return false;
  }

  const auto value_name = options.executable_path.stem().wstring();
  DWORD type = 0;
  DWORD byte_count = 0;
  result = RegQueryValueExW(key, value_name.c_str(), nullptr, &type, nullptr,
                            &byte_count);
  if (result == ERROR_FILE_NOT_FOUND) {
    RegCloseKey(key);
    *status = kMuonAutostartStatusDisabled;
    return true;
  }
  if (result != ERROR_SUCCESS) {
    RegCloseKey(key);
    *error_message = "Failed to read Windows Run registry value";
    return false;
  }
  if (type != REG_SZ && type != REG_EXPAND_SZ) {
    RegCloseKey(key);
    *status = kMuonAutostartStatusDisabled;
    return true;
  }

  std::wstring command(byte_count / sizeof(wchar_t), L'\0');
  result = RegQueryValueExW(
      key, value_name.c_str(), nullptr, &type,
      reinterpret_cast<LPBYTE>(command.data()), &byte_count);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) {
    *error_message = "Failed to read Windows Run registry value";
    return false;
  }
  while (!command.empty() && command.back() == L'\0') {
    command.pop_back();
  }
  *status = WindowsRunCommandMatchesExecutable(command, options.executable_path)
                ? kMuonAutostartStatusEnabled
                : kMuonAutostartStatusDisabled;
  return true;
}

static bool SetWindowsAutostart(const MuonAutostartOptions& options,
                                bool enabled,
                                std::string* error_message) {
  const auto value_name = options.executable_path.stem().wstring();
  if (enabled) {
    const auto command = CreateWindowsRunCommand(options);
    if (command.size() > kWindowsRunCommandMaxLength) {
      *error_message = "Windows Run command is too long";
      return false;
    }

    HKEY key = nullptr;
    const auto create_result =
        RegCreateKeyExW(HKEY_CURRENT_USER, kWindowsRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (create_result != ERROR_SUCCESS) {
      *error_message = "Failed to open Windows Run registry key";
      return false;
    }
    const auto byte_count =
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    const auto set_result = RegSetValueExW(
        key, value_name.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()), byte_count);
    RegCloseKey(key);
    if (set_result != ERROR_SUCCESS) {
      *error_message = "Failed to write Windows Run registry value";
      return false;
    }
    return true;
  }

  HKEY key = nullptr;
  const auto open_result =
      RegOpenKeyExW(HKEY_CURRENT_USER, kWindowsRunKey, 0, KEY_SET_VALUE, &key);
  if (open_result == ERROR_FILE_NOT_FOUND) {
    return true;
  }
  if (open_result != ERROR_SUCCESS) {
    *error_message = "Failed to open Windows Run registry key";
    return false;
  }
  const auto delete_result = RegDeleteValueW(key, value_name.c_str());
  RegCloseKey(key);
  if (delete_result != ERROR_SUCCESS &&
      delete_result != ERROR_FILE_NOT_FOUND) {
    *error_message = "Failed to delete Windows Run registry value";
    return false;
  }
  return true;
}

#endif

}  // namespace muon_internal

bool CreateDefaultMuonAutostartOptions(MuonAutostartOptions* options,
                                       std::string* error_message) {
  if (!muon_internal::RequireOutputArguments(options, error_message)) {
    return false;
  }
  error_message->clear();
  options->executable_path.clear();
  options->launch_source = GetMuonStartupLaunchSource();
  if (!GetMuonExecutablePath(&options->executable_path)) {
    *error_message = "Failed to resolve muon executable path";
    return false;
  }
  return true;
}

bool GetMuonAutostartStatus(const MuonAutostartOptions& options,
                            MuonAutostartStatus* status,
                            std::string* error_message) {
  if (!muon_internal::RequireOutputArguments(status, error_message)) {
    return false;
  }
  error_message->clear();
  *status = kMuonAutostartStatusUnknown;
  if (!muon_internal::ValidateAutostartOptions(options, error_message)) {
    return false;
  }
  if (!muon_internal::IsDefaultAutostartLaunchSource(options.launch_source)) {
    return true;
  }
#if defined(_WIN32)
  return muon_internal::GetWindowsAutostartStatus(options, status,
                                                  error_message);
#else
  return muon_internal::GetXdgAutostartStatus(options, status, error_message);
#endif
}

bool SetMuonAutostart(const MuonAutostartOptions& options,
                      bool enabled,
                      std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (!muon_internal::ValidateAutostartOptions(options, error_message)) {
    return false;
  }
  if (!muon_internal::IsDefaultAutostartLaunchSource(options.launch_source)) {
    *error_message = "Autostart launch source is unsupported: " +
                     options.launch_source;
    return false;
  }
#if defined(_WIN32)
  return muon_internal::SetWindowsAutostart(options, enabled, error_message);
#else
  return muon_internal::SetXdgAutostart(options, enabled, error_message);
#endif
}
