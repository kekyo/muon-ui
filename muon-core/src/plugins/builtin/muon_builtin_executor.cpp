/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_executor.h"

#include "yyjson.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace muon_internal {

struct RunOptions {
  std::string command;
  std::vector<std::string> args;
  std::string stdin_data;
  std::string cwd;
  bool has_cwd = false;
  std::map<std::string, std::string> env;
  bool has_env = false;
};

struct RunResult {
  uint32_t process_id = 0;
  int32_t exit_code = 0;
  std::string stdout_data;
  std::string stderr_data;
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor run_args[] = {
    type_string,
};

static bool ContainsNul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

static void AppendJsonHex(std::string* target, uint8_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  target->push_back(kHex[(value >> 4) & 0x0f]);
  target->push_back(kHex[value & 0x0f]);
}

static void AppendJsonString(std::string* target, std::string_view value) {
  target->push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<uint8_t>(character);
    switch (character) {
      case '"':
        *target += "\\\"";
        break;
      case '\\':
        *target += "\\\\";
        break;
      case '\b':
        *target += "\\b";
        break;
      case '\f':
        *target += "\\f";
        break;
      case '\n':
        *target += "\\n";
        break;
      case '\r':
        *target += "\\r";
        break;
      case '\t':
        *target += "\\t";
        break;
      default:
        if (byte < 0x20) {
          *target += "\\u00";
          AppendJsonHex(target, byte);
        } else {
          target->push_back(character);
        }
        break;
    }
  }
  target->push_back('"');
}

static void CompleteString(muon_completion_func completion,
                           const std::string& result) {
  const auto* pointer = result.c_str();
  completion(&pointer, nullptr);
}

static void CompleteError(muon_completion_func completion,
                          const std::string& message) {
  completion(nullptr, message.c_str());
}

static std::string CreateRunResultJson(const RunResult& result) {
  std::string json = "{\"processId\":";
  json += std::to_string(result.process_id);
  json += ",\"exitCode\":";
  json += std::to_string(result.exit_code);
  json += ",\"stdout\":";
  AppendJsonString(&json, result.stdout_data);
  json += ",\"stderr\":";
  AppendJsonString(&json, result.stderr_data);
  json += "}";
  return json;
}

static bool ReadRequiredString(yyjson_val* object,
                               const char* key,
                               std::string* target,
                               std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (!yyjson_is_str(value)) {
    *error_message = std::string(key) + " is required";
    return false;
  }
  target->assign(yyjson_get_str(value), yyjson_get_len(value));
  if (target->empty()) {
    *error_message = std::string(key) + " is required";
    return false;
  }
  if (ContainsNul(*target)) {
    *error_message = std::string(key) + " must not contain NUL";
    return false;
  }
  return true;
}

static bool ReadOptionalString(yyjson_val* object,
                               const char* key,
                               std::string* target,
                               bool* has_value,
                               std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  *has_value = false;
  target->clear();
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = std::string(key) + " must be a string";
    return false;
  }
  target->assign(yyjson_get_str(value), yyjson_get_len(value));
  if (ContainsNul(*target)) {
    *error_message = std::string(key) + " must not contain NUL";
    return false;
  }
  *has_value = true;
  return true;
}

static bool ReadStringArray(yyjson_val* object,
                            const char* key,
                            std::vector<std::string>* target,
                            std::string* error_message) {
  target->clear();
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_arr(value)) {
    *error_message = std::string(key) + " must be an array";
    return false;
  }
  const auto size = yyjson_arr_size(value);
  target->reserve(size);
  for (auto index = size_t{0}; index < size; ++index) {
    const auto entry = yyjson_arr_get(value, index);
    if (!yyjson_is_str(entry)) {
      *error_message = std::string(key) + " entries must be strings";
      return false;
    }
    std::string string_entry(yyjson_get_str(entry), yyjson_get_len(entry));
    if (ContainsNul(string_entry)) {
      *error_message = std::string(key) + " entries must not contain NUL";
      return false;
    }
    target->push_back(std::move(string_entry));
  }
  return true;
}

static bool ReadEnvironmentObject(yyjson_val* object,
                                  RunOptions* options,
                                  std::string* error_message) {
  const auto value = yyjson_obj_get(object, "env");
  options->env.clear();
  options->has_env = false;
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_obj(value)) {
    *error_message = "env must be an object";
    return false;
  }
  options->has_env = true;
  yyjson_val* key = nullptr;
  yyjson_val* entry = nullptr;
  size_t index = 0;
  size_t max = 0;
  yyjson_obj_foreach(value, index, max, key, entry) {
    if (!yyjson_is_str(key) || !yyjson_is_str(entry)) {
      *error_message = "env entries must be strings";
      return false;
    }
    std::string name(yyjson_get_str(key), yyjson_get_len(key));
    std::string env_value(yyjson_get_str(entry), yyjson_get_len(entry));
    if (name.empty() || name.find('=') != std::string::npos ||
        ContainsNul(name)) {
      *error_message = "env keys must be non-empty names without '=' or NUL";
      return false;
    }
    if (ContainsNul(env_value)) {
      *error_message = "env values must not contain NUL";
      return false;
    }
    options->env[std::move(name)] = std::move(env_value);
  }
  return true;
}

static bool ParseRunOptions(const char* options_json,
                            RunOptions* options,
                            std::string* error_message) {
  if (options_json == nullptr) {
    *error_message = "Options JSON is required";
    return false;
  }
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(options_json), std::strlen(options_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    *error_message = "Options JSON is invalid";
    return false;
  }

  auto success = false;
  auto* root = yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) {
    *error_message = "Options JSON root must be an object";
  } else if (ReadRequiredString(root, "command", &options->command,
                                error_message) &&
             ReadStringArray(root, "args", &options->args, error_message) &&
             ReadOptionalString(root, "stdin", &options->stdin_data, &success,
                                error_message)) {
    success = ReadOptionalString(root, "cwd", &options->cwd, &options->has_cwd,
                                 error_message) &&
              ReadEnvironmentObject(root, options, error_message);
  }
  yyjson_doc_free(document);
  return success;
}

#if defined(_WIN32)

static bool WideToUtf8(const wchar_t* source, std::string* target) {
  if (source == nullptr || target == nullptr) {
    return false;
  }
  const auto required = WideCharToMultiByte(
      CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return false;
  }
  std::string converted;
  converted.resize(static_cast<size_t>(required));
  if (required > 1 &&
      WideCharToMultiByte(CP_UTF8, 0, source, -1, converted.data(), required,
                          nullptr, nullptr) <= 0) {
    return false;
  }
  converted.resize(static_cast<size_t>(required - 1));
  *target = std::move(converted);
  return true;
}

static bool Utf8ToWide(const std::string& source, std::wstring* target) {
  if (target == nullptr) {
    return false;
  }
  const auto required = MultiByteToWideChar(
      CP_UTF8, 0, source.c_str(), -1, nullptr, 0);
  if (required <= 0) {
    return false;
  }
  std::wstring converted;
  converted.resize(static_cast<size_t>(required));
  if (required > 1 &&
      MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, converted.data(),
                          required) <= 0) {
    return false;
  }
  converted.resize(static_cast<size_t>(required - 1));
  *target = std::move(converted);
  return true;
}

static std::vector<std::pair<std::string, std::string>> GetEnvironmentEntries() {
  std::vector<std::pair<std::string, std::string>> entries;
  auto* block = GetEnvironmentStringsW();
  if (block == nullptr) {
    return entries;
  }
  for (auto* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
    const auto* separator = std::wcschr(entry, L'=');
    if (separator == nullptr || separator == entry) {
      continue;
    }
    std::wstring key_wide(entry, separator - entry);
    std::wstring value_wide(separator + 1);
    std::string key;
    std::string value;
    if (WideToUtf8(key_wide.c_str(), &key) &&
        WideToUtf8(value_wide.c_str(), &value)) {
      entries.emplace_back(std::move(key), std::move(value));
    }
  }
  FreeEnvironmentStringsW(block);
  return entries;
}

static std::wstring QuoteWindowsArgument(const std::wstring& argument) {
  if (argument.empty()) {
    return L"\"\"";
  }
  auto needs_quotes = false;
  for (const auto character : argument) {
    if (character == L' ' || character == L'\t' || character == L'"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return argument;
  }

  std::wstring quoted = L"\"";
  auto backslashes = size_t{0};
  for (const auto character : argument) {
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
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

static bool ReadPipeToString(HANDLE pipe, std::string* target) {
  char buffer[4096];
  DWORD read_size = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &read_size, nullptr) &&
         read_size > 0) {
    target->append(buffer, read_size);
  }
  return true;
}

static bool CreateWindowsEnvironmentBlock(const RunOptions& options,
                                          std::vector<wchar_t>* block,
                                          std::string* error_message) {
  block->clear();
  if (!options.has_env) {
    return true;
  }

  std::map<std::string, std::string> merged;
  for (auto entry : GetEnvironmentEntries()) {
    merged[std::move(entry.first)] = std::move(entry.second);
  }
  for (const auto& entry : options.env) {
    merged[entry.first] = entry.second;
  }

  for (const auto& entry : merged) {
    std::wstring key;
    std::wstring value;
    if (!Utf8ToWide(entry.first, &key) || !Utf8ToWide(entry.second, &value)) {
      *error_message = "env entries must be valid UTF-8";
      return false;
    }
    block->insert(block->end(), key.begin(), key.end());
    block->push_back(L'=');
    block->insert(block->end(), value.begin(), value.end());
    block->push_back(L'\0');
  }
  block->push_back(L'\0');
  return true;
}

static bool RunProcess(const RunOptions& options,
                       RunResult* result,
                       std::string* error_message) {
  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

  HANDLE stdin_read = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  HANDLE stderr_read = nullptr;
  HANDLE stderr_write = nullptr;
  auto close_handle = [](HANDLE* handle) {
    if (*handle != nullptr) {
      CloseHandle(*handle);
      *handle = nullptr;
    }
  };
  auto close_pipe_handles = [&close_handle, &stdin_read, &stdin_write,
                             &stdout_read, &stdout_write, &stderr_read,
                             &stderr_write]() {
    close_handle(&stdin_read);
    close_handle(&stdin_write);
    close_handle(&stdout_read);
    close_handle(&stdout_write);
    close_handle(&stderr_read);
    close_handle(&stderr_write);
  };
  if (!CreatePipe(&stdin_read, &stdin_write, &security_attributes, 0) ||
      !CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0) ||
      !CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0)) {
    close_pipe_handles();
    *error_message = "Failed to create process pipes";
    return false;
  }
  SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

  std::wstring command;
  if (!Utf8ToWide(options.command, &command)) {
    close_pipe_handles();
    *error_message = "Command is not valid UTF-8";
    return false;
  }
  std::wstring command_line = QuoteWindowsArgument(command);
  for (const auto& arg : options.args) {
    std::wstring wide_arg;
    if (!Utf8ToWide(arg, &wide_arg)) {
      close_pipe_handles();
      *error_message = "Argument is not valid UTF-8";
      return false;
    }
    command_line.push_back(L' ');
    command_line += QuoteWindowsArgument(wide_arg);
  }
  std::vector<wchar_t> mutable_command_line(
      command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  std::wstring cwd;
  const wchar_t* cwd_pointer = nullptr;
  if (options.has_cwd) {
    if (!Utf8ToWide(options.cwd, &cwd)) {
      close_pipe_handles();
      *error_message = "cwd is not valid UTF-8";
      return false;
    }
    cwd_pointer = cwd.c_str();
  }
  std::vector<wchar_t> environment_block;
  void* environment_pointer = nullptr;
  DWORD creation_flags = 0;
  if (!CreateWindowsEnvironmentBlock(options, &environment_block,
                                     error_message)) {
    close_pipe_handles();
    return false;
  }
  if (options.has_env) {
    environment_pointer = environment_block.data();
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;
  }

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = stdin_read;
  startup_info.hStdOutput = stdout_write;
  startup_info.hStdError = stderr_write;
  PROCESS_INFORMATION process_info = {};
  const auto created = CreateProcessW(
      nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE,
      creation_flags, environment_pointer, cwd_pointer, &startup_info,
      &process_info);
  close_handle(&stdin_read);
  close_handle(&stdout_write);
  close_handle(&stderr_write);
  if (!created) {
    close_pipe_handles();
    *error_message = "Failed to start process";
    return false;
  }

  result->process_id = process_info.dwProcessId;
  DWORD written = 0;
  if (!options.stdin_data.empty()) {
    WriteFile(stdin_write, options.stdin_data.data(),
              static_cast<DWORD>(options.stdin_data.size()), &written, nullptr);
  }
  close_handle(&stdin_write);
  WaitForSingleObject(process_info.hProcess, INFINITE);
  ReadPipeToString(stdout_read, &result->stdout_data);
  ReadPipeToString(stderr_read, &result->stderr_data);
  close_handle(&stdout_read);
  close_handle(&stderr_read);

  DWORD exit_code = 0;
  GetExitCodeProcess(process_info.hProcess, &exit_code);
  result->exit_code = static_cast<int32_t>(exit_code);
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return true;
}

#else

static bool SetNonBlocking(int fd, std::string* error_message) {
  const auto flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    *error_message = "Failed to configure process pipe";
    return false;
  }
  return true;
}

static bool CreateCloseOnExecPipe(int fds[2], std::string* error_message) {
  if (pipe(fds) != 0) {
    *error_message = "Failed to create process pipe";
    return false;
  }
  for (auto index = 0; index < 2; ++index) {
    const auto flags = fcntl(fds[index], F_GETFD, 0);
    if (flags < 0 || fcntl(fds[index], F_SETFD, flags | FD_CLOEXEC) < 0) {
      close(fds[0]);
      close(fds[1]);
      *error_message = "Failed to configure process pipe";
      return false;
    }
  }
  return true;
}

static void CloseFd(int* fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static std::vector<std::string> SplitPathList(const std::string& path) {
  std::vector<std::string> entries;
  auto begin = size_t{0};
  while (begin <= path.size()) {
    const auto end = path.find(':', begin);
    const auto length =
        end == std::string::npos ? path.size() - begin : end - begin;
    entries.push_back(length == 0 ? "." : path.substr(begin, length));
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return entries;
}

static std::vector<std::string> CreateCommandCandidates(
    const std::string& command,
    const std::map<std::string, std::string>& environment,
    bool has_environment) {
  if (command.find('/') != std::string::npos) {
    return {command};
  }
  std::string path_value;
  if (has_environment) {
    const auto iterator = environment.find("PATH");
    path_value = iterator == environment.end() ? "" : iterator->second;
  } else {
    const auto* path = std::getenv("PATH");
    path_value = path == nullptr ? "" : path;
  }
  if (path_value.empty()) {
    path_value = "/bin:/usr/bin";
  }

  std::vector<std::string> candidates;
  for (const auto& path_entry : SplitPathList(path_value)) {
    candidates.push_back(path_entry + "/" + command);
  }
  return candidates;
}

static std::map<std::string, std::string> CreateMergedEnvironment(
    const RunOptions& options) {
  std::map<std::string, std::string> merged;
  if (environ != nullptr) {
    for (auto index = size_t{0}; environ[index] != nullptr; ++index) {
      const auto* entry = environ[index];
      const auto* separator = std::strchr(entry, '=');
      if (separator == nullptr || separator == entry) {
        continue;
      }
      merged[std::string(entry, separator - entry)] = separator + 1;
    }
  }
  for (const auto& entry : options.env) {
    merged[entry.first] = entry.second;
  }
  return merged;
}

static bool RunProcess(const RunOptions& options,
                       RunResult* result,
                       std::string* error_message) {
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (!CreateCloseOnExecPipe(stdin_pipe, error_message) ||
      !CreateCloseOnExecPipe(stdout_pipe, error_message) ||
      !CreateCloseOnExecPipe(stderr_pipe, error_message)) {
    return false;
  }

  std::vector<std::string> argv_storage;
  argv_storage.reserve(options.args.size() + 1);
  argv_storage.push_back(options.command);
  for (const auto& arg : options.args) {
    argv_storage.push_back(arg);
  }
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto& arg : argv_storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const auto merged_environment = CreateMergedEnvironment(options);
  std::vector<std::string> env_storage;
  std::vector<char*> envp;
  char** environment_pointer = environ;
  if (options.has_env) {
    env_storage.reserve(merged_environment.size());
    for (const auto& entry : merged_environment) {
      env_storage.push_back(entry.first + "=" + entry.second);
    }
    envp.reserve(env_storage.size() + 1);
    for (auto& entry : env_storage) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    environment_pointer = envp.data();
  }
  const auto command_candidates = CreateCommandCandidates(
      options.command, merged_environment, options.has_env);

  const auto child = fork();
  if (child < 0) {
    CloseFd(&stdin_pipe[0]);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stdout_pipe[1]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&stderr_pipe[1]);
    *error_message = "Failed to start process";
    return false;
  }

  if (child == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    if (options.has_cwd && chdir(options.cwd.c_str()) != 0) {
      _exit(126);
    }
    for (const auto& candidate : command_candidates) {
      execve(candidate.c_str(), argv.data(), environment_pointer);
    }
    _exit(errno == ENOENT ? 127 : 126);
  }

  result->process_id = static_cast<uint32_t>(child);
  CloseFd(&stdin_pipe[0]);
  CloseFd(&stdout_pipe[1]);
  CloseFd(&stderr_pipe[1]);
  if (!SetNonBlocking(stdin_pipe[1], error_message) ||
      !SetNonBlocking(stdout_pipe[0], error_message) ||
      !SetNonBlocking(stderr_pipe[0], error_message)) {
    kill(child, SIGKILL);
    return false;
  }

  auto stdin_offset = size_t{0};
  auto stdin_open = true;
  auto stdout_open = true;
  auto stderr_open = true;
  while (stdin_open || stdout_open || stderr_open) {
    std::vector<pollfd> poll_fds;
    if (stdin_open) {
      if (stdin_offset >= options.stdin_data.size()) {
        CloseFd(&stdin_pipe[1]);
        stdin_open = false;
      } else {
        poll_fds.push_back({stdin_pipe[1], POLLOUT, 0});
      }
    }
    if (stdout_open) {
      poll_fds.push_back({stdout_pipe[0], POLLIN, 0});
    }
    if (stderr_open) {
      poll_fds.push_back({stderr_pipe[0], POLLIN, 0});
    }
    if (poll_fds.empty()) {
      break;
    }

    if (poll(poll_fds.data(), poll_fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      kill(child, SIGKILL);
      *error_message = "Failed to poll process pipes";
      return false;
    }

    auto poll_index = size_t{0};
    if (stdin_open && stdin_offset < options.stdin_data.size()) {
      const auto events = poll_fds[poll_index].revents;
      ++poll_index;
      if ((events & (POLLOUT | POLLERR | POLLHUP)) != 0) {
        const auto remaining = options.stdin_data.size() - stdin_offset;
        const auto written = write(stdin_pipe[1],
                                   options.stdin_data.data() + stdin_offset,
                                   remaining);
        if (written > 0) {
          stdin_offset += static_cast<size_t>(written);
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
          CloseFd(&stdin_pipe[1]);
          stdin_open = false;
        }
      }
    }

    auto read_pipe = [](int* fd, std::string* target, bool* open) {
      char buffer[4096];
      const auto read_size = read(*fd, buffer, sizeof(buffer));
      if (read_size > 0) {
        target->append(buffer, static_cast<size_t>(read_size));
        return;
      }
      if (read_size == 0 ||
          (read_size < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
           errno != EINTR)) {
        CloseFd(fd);
        *open = false;
      }
    };

    if (stdout_open) {
      const auto events = poll_fds[poll_index].revents;
      ++poll_index;
      if ((events & (POLLIN | POLLERR | POLLHUP)) != 0) {
        read_pipe(&stdout_pipe[0], &result->stdout_data, &stdout_open);
      }
    }
    if (stderr_open) {
      const auto events = poll_fds[poll_index].revents;
      if ((events & (POLLIN | POLLERR | POLLHUP)) != 0) {
        read_pipe(&stderr_pipe[0], &result->stderr_data, &stderr_open);
      }
    }
  }

  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      *error_message = "Failed to wait for process";
      return false;
    }
  }
  if (WIFEXITED(status)) {
    result->exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result->exit_code = 128 + WTERMSIG(status);
  } else {
    result->exit_code = -1;
  }
  return true;
}

#endif

extern "C" void muon_builtin_executor_run(muon_completion_func completion,
                                           const char* options_json) {
  RunOptions options;
  std::string error_message;
  if (!ParseRunOptions(options_json, &options, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  RunResult result;
  if (!RunProcess(options, &result, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  CompleteString(completion, CreateRunResultJson(result));
}

static const muon_plugin_function_metadata spawn_function = {
    "__run",
    reinterpret_cast<muon_native_function>(&muon_builtin_executor_run),
    {1, run_args, &type_string},
    "spawn",
};

static const muon_plugin_function_metadata* const executor_functions[] = {
    &spawn_function,
    nullptr,
};

static constexpr char executor_setup_script[] = R"JS(
const parseNativeJson = async (source) => JSON.parse(await source);
const properties = {};
if (isAllowed("spawn")) {
  properties.spawn = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (options) =>
      parseNativeJson(namespace.__run(JSON.stringify(options ?? {}))),
  };
}
Object.defineProperties(namespace, properties);
)JS";

}  // namespace muon_internal

const muon_plugin_namespace kMuonBuiltinExecutorNamespace = {
    "muon.executor",
    muon_internal::executor_setup_script,
    muon_internal::executor_functions,
};

static const muon_plugin_namespace* const executor_namespaces[] = {
    &kMuonBuiltinExecutorNamespace,
    nullptr,
};

static const muon_plugin_metadata executor_metadata = {
    executor_namespaces,
};

const muon_plugin_metadata* GetMuonBuiltinExecutorPluginMetadata() {
  return &executor_metadata;
}
