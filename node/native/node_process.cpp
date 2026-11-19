/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "node_process.h"

#if defined(_WIN32)
#include "muon_windows_job_process.h"
#endif

#include <cardio.h>
#include <yyjson.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <algorithm>
#include <bcrypt.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

static constexpr size_t kMuonNodeMaximumFrameSize = 16u * 1024u * 1024u;

std::string CreateMuonNodeSessionToken() {
  std::array<uint8_t, 32> bytes{};
#if defined(_WIN32)
  const auto status = BCryptGenRandom(
      nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0) {
    throw std::runtime_error("BCryptGenRandom failed for Node IPC token");
  }
#else
  auto offset = size_t{0};
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset,
                                   bytes.size() - offset, 0);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error("getrandom failed for Node IPC token");
  }
#endif
  static constexpr char digits[] = "0123456789abcdef";
  std::string token;
  token.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    token.push_back(digits[(byte >> 4) & 0x0f]);
    token.push_back(digits[byte & 0x0f]);
  }
  return token;
}

std::vector<std::byte> CreateMuonNodeFrame(
    const std::string& json) {
  if (json.size() > kMuonNodeMaximumFrameSize ||
      json.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Node IPC frame exceeds the size limit");
  }
  const auto size = static_cast<uint32_t>(json.size());
  std::vector<std::byte> frame(4 + json.size());
  frame[0] = static_cast<std::byte>((size >> 24) & 0xff);
  frame[1] = static_cast<std::byte>((size >> 16) & 0xff);
  frame[2] = static_cast<std::byte>((size >> 8) & 0xff);
  frame[3] = static_cast<std::byte>(size & 0xff);
  if (!json.empty()) {
    std::memcpy(frame.data() + 4, json.data(), json.size());
  }
  return frame;
}

#if defined(_WIN32)
static cardio::promise<size_t> ReadMuonNodeTransport(
    MuonNodeProcess* runtime,
    std::span<std::byte> buffer) {
  if (runtime == nullptr || runtime->pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Node IPC pipe is unavailable");
  }
  co_return co_await cardio::win32::read(
      runtime->pipe, buffer, runtime->io_cancellation.get_cancellation());
}

static cardio::promise<size_t> WriteMuonNodeTransport(
    MuonNodeProcess* runtime,
    std::span<const std::byte> buffer) {
  if (runtime == nullptr || runtime->pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Node IPC pipe is unavailable");
  }
  co_return co_await cardio::win32::write(
      runtime->pipe, buffer, runtime->io_cancellation.get_cancellation());
}
#else
static cardio::promise<size_t> ReadMuonNodeTransport(
    MuonNodeProcess* runtime,
    std::span<std::byte> buffer) {
  if (runtime == nullptr || runtime->socket_fd < 0) {
    throw std::runtime_error("Node IPC socket is unavailable");
  }
  for (;;) {
    const auto count =
        ::read(runtime->socket_fd, buffer.data(), buffer.size());
    if (count >= 0) {
      co_return static_cast<size_t>(count);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      throw std::system_error(
          errno, std::generic_category(), "Node IPC read failed");
    }
    const auto events = co_await cardio::from_fd(
        runtime->socket_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup,
        runtime->io_cancellation.get_cancellation());
    if ((events & cardio::fd_event::error) != cardio::fd_event::none) {
      throw std::runtime_error("Node IPC socket reported a read error");
    }
  }
}

static cardio::promise<size_t> WriteMuonNodeTransport(
    MuonNodeProcess* runtime,
    std::span<const std::byte> buffer) {
  if (runtime == nullptr || runtime->socket_fd < 0) {
    throw std::runtime_error("Node IPC socket is unavailable");
  }
  for (;;) {
    // A service process exit must become an IPC error, never a process-wide SIGPIPE.
    const auto count =
        ::send(runtime->socket_fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (count >= 0) {
      co_return static_cast<size_t>(count);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      throw std::system_error(
          errno, std::generic_category(), "Node IPC write failed");
    }
    const auto events = co_await cardio::from_fd(
        runtime->socket_fd,
        cardio::fd_event::write | cardio::fd_event::error |
            cardio::fd_event::hangup,
        runtime->io_cancellation.get_cancellation());
    if ((events & cardio::fd_event::error) != cardio::fd_event::none) {
      throw std::runtime_error("Node IPC socket reported a write error");
    }
  }
}
#endif

cardio::promise<void> ReadMuonNodeExact(
    MuonNodeProcess* runtime,
    std::span<std::byte> buffer) {
  auto offset = size_t{0};
  while (offset < buffer.size()) {
    const auto count =
        co_await ReadMuonNodeTransport(runtime, buffer.subspan(offset));
    if (count == 0) {
      throw std::runtime_error("Node IPC transport closed");
    }
    offset += count;
  }
}

cardio::promise<void> WriteMuonNodeExact(
    MuonNodeProcess* runtime,
    std::span<const std::byte> buffer) {
  auto offset = size_t{0};
  while (offset < buffer.size()) {
    const auto count =
        co_await WriteMuonNodeTransport(runtime, buffer.subspan(offset));
    if (count == 0) {
      throw std::runtime_error("Node IPC transport closed while writing");
    }
    offset += count;
  }
}

void CloseMuonNodeTransport(MuonNodeProcess* runtime) {
  if (runtime == nullptr) {
    return;
  }
#if defined(_WIN32)
  if (runtime->pipe != INVALID_HANDLE_VALUE) {
    (void)DisconnectNamedPipe(runtime->pipe);
    (void)CloseHandle(runtime->pipe);
    runtime->pipe = INVALID_HANDLE_VALUE;
  }
#else
  if (runtime->socket_fd >= 0) {
    (void)::close(runtime->socket_fd);
    runtime->socket_fd = -1;
  }
#endif
  runtime->transport_connected = false;
}

void CloseMuonNodeProcessHandles(MuonNodeProcess* runtime) {
  if (runtime == nullptr) {
    return;
  }
#if defined(_WIN32)
  CloseMuonWindowsJobProcess(&runtime->process);
#else
  if (runtime->process_fd >= 0) {
    (void)::close(runtime->process_fd);
    runtime->process_fd = -1;
  }
  runtime->process_id = -1;
#endif
}

void TerminateMuonNodeProcess(MuonNodeProcess* runtime,
                                     bool force) {
  if (runtime == nullptr || !runtime->process_started ||
      runtime->process_exited) {
    return;
  }
#if defined(_WIN32)
  (void)TerminateMuonWindowsJobProcess(
      &runtime->process, force ? 137u : 143u);
#else
  if (runtime->process_id > 0) {
    (void)::kill(runtime->process_id, force ? SIGKILL : SIGTERM);
  }
#endif
}

#if defined(_WIN32)
static std::wstring ConvertMuonNodeUtf8ToWide(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const auto size = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    throw std::runtime_error("Failed to decode a Node UTF-8 path");
  }
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
          static_cast<int>(value.size()), result.data(), size) != size) {
    throw std::runtime_error("Failed to decode a Node UTF-8 path");
  }
  return result;
}

static std::wstring QuoteMuonNodeWindowsArgument(
    const std::wstring& argument) {
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring result = L"\"";
  auto slash_count = size_t{0};
  for (const auto character : argument) {
    if (character == L'\\') {
      slash_count += 1;
      continue;
    }
    if (character == L'"') {
      result.append(slash_count * 2 + 1, L'\\');
      result.push_back(L'"');
      slash_count = 0;
      continue;
    }
    result.append(slash_count, L'\\');
    slash_count = 0;
    result.push_back(character);
  }
  result.append(slash_count * 2, L'\\');
  result.push_back(L'"');
  return result;
}

static int CompareMuonNodeWindowsStrings(
    const std::wstring& first,
    const std::wstring& second) {
  if (first.size() >
          static_cast<size_t>((std::numeric_limits<int>::max)()) ||
      second.size() >
          static_cast<size_t>((std::numeric_limits<int>::max)())) {
    throw std::runtime_error(
        "Node environment entry exceeds the Win32 string size limit");
  }
  const auto result = CompareStringOrdinal(
      first.data(), static_cast<int>(first.size()),
      second.data(), static_cast<int>(second.size()), TRUE);
  if (result == 0) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CompareStringOrdinal failed for Node environment");
  }
  return result;
}

static bool IsMuonNodeWindowsEnvironmentEntry(
    const std::wstring& entry,
    const std::wstring& name) {
  if (entry.size() <= name.size() ||
      entry[name.size()] != L'=') {
    return false;
  }
  return CompareMuonNodeWindowsStrings(
             entry.substr(0, name.size()), name) == CSTR_EQUAL;
}

static std::vector<wchar_t> CreateMuonNodeWindowsEnvironment(
    const std::wstring& pipe_name,
    const std::wstring& token) {
  auto* environment = GetEnvironmentStringsW();
  if (environment == nullptr) {
    throw std::runtime_error(
        "GetEnvironmentStringsW failed for Node service process");
  }
  auto environment_guard =
      std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)>(
          environment, &FreeEnvironmentStringsW);
  std::vector<std::wstring> entries;
  for (auto* cursor = environment; *cursor != L'\0';) {
    const auto entry = std::wstring(cursor);
    cursor += entry.size() + 1;
    if (IsMuonNodeWindowsEnvironmentEntry(
            entry, L"MUON_NODE_PIPE") ||
        IsMuonNodeWindowsEnvironmentEntry(
            entry, L"MUON_NODE_TOKEN")) {
      continue;
    }
    entries.push_back(entry);
  }
  entries.push_back(L"MUON_NODE_PIPE=" + pipe_name);
  entries.push_back(L"MUON_NODE_TOKEN=" + token);
  // CreateProcessW requires a locale-independent, case-insensitive Unicode
  // ordering for caller-supplied environment blocks.
  std::sort(
      entries.begin(), entries.end(),
      [](const std::wstring& first, const std::wstring& second) {
        return CompareMuonNodeWindowsStrings(first, second) ==
               CSTR_LESS_THAN;
      });

  std::vector<wchar_t> block;
  for (const auto& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

static void CloseMuonNodeWindowsHandle(HANDLE* handle) noexcept {
  if (handle == nullptr || *handle == nullptr ||
      *handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)CloseHandle(*handle);
  *handle = nullptr;
}

static HANDLE CreateMuonNodeInheritedStandardHandle(
    DWORD standard_handle,
    DWORD null_access, bool inherit) {
  const auto source = inherit ? GetStdHandle(standard_handle) : nullptr;
  if (source != nullptr && source != INVALID_HANDLE_VALUE) {
    HANDLE inherited = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(), source, GetCurrentProcess(), &inherited,
            0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE) {
      return inherited;
    }
    const auto error = GetLastError();
    // GUI subsystem processes normally have no standard handles. Treat stale
    // values as absent, but preserve actionable duplication failures.
    if (error != ERROR_INVALID_HANDLE) {
      throw std::system_error(
          static_cast<int>(error), std::system_category(),
          "Failed to duplicate a Node standard handle");
    }
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const auto handle = CreateFileW(
      L"NUL", null_access, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "Failed to open NUL for the Node service process");
  }
  return handle;
}

static cardio::promise<void> ConnectMuonNodeWindowsPipe(
    MuonNodeProcess* runtime) {
  const auto event =
      CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CreateEventW failed for Node IPC connection");
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  auto pending = false;
  try {
    if (ConnectNamedPipe(runtime->pipe, &overlapped) == FALSE) {
      const auto error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) {
        // A client won the documented connect race; no OVERLAPPED operation
        // is pending in this case.
        (void)CloseHandle(event);
        co_return;
      }
      if (error != ERROR_IO_PENDING) {
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "ConnectNamedPipe failed for Node service process");
      }
      pending = true;
      (void)co_await cardio::from_win32_overlapped(
          runtime->pipe, overlapped,
          runtime->io_cancellation.get_cancellation());
      pending = false;
    }
  } catch (...) {
    if (pending) {
      (void)CancelIoEx(runtime->pipe, &overlapped);
      DWORD ignored = 0;
      (void)GetOverlappedResult(
          runtime->pipe, &overlapped, &ignored, TRUE);
    }
    (void)CloseHandle(event);
    throw;
  }
  (void)CloseHandle(event);
}

cardio::promise<void> StartMuonNodeProcess(
    MuonNodeProcess* runtime, std::function<void()> spawned) {
  const auto pipe_name =
      std::wstring(L"\\\\.\\pipe\\muon-node-") +
      std::to_wstring(GetCurrentProcessId()) + L"-" +
      ConvertMuonNodeUtf8ToWide(CreateMuonNodeSessionToken());
  runtime->pipe = CreateNamedPipeW(
      pipe_name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(kMuonNodeMaximumFrameSize),
      static_cast<DWORD>(kMuonNodeMaximumFrameSize), 0, nullptr);
  if (runtime->pipe == INVALID_HANDLE_VALUE) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CreateNamedPipeW failed for Node service process");
  }

  const auto executable =
      ConvertMuonNodeUtf8ToWide(runtime->executable_path);
  const auto bridge = ConvertMuonNodeUtf8ToWide(runtime->bridge_path);
  auto command_line =
      QuoteMuonNodeWindowsArgument(executable) + L" " +
      QuoteMuonNodeWindowsArgument(bridge);
  std::vector<wchar_t> command_line_buffer(
      command_line.begin(), command_line.end());
  command_line_buffer.push_back(L'\0');
  auto environment = CreateMuonNodeWindowsEnvironment(
      pipe_name, ConvertMuonNodeUtf8ToWide(runtime->session_token));

  std::array<HANDLE, 3> inherited_standard_handles{
      nullptr, nullptr, nullptr};
  try {
    inherited_standard_handles[0] =
        CreateMuonNodeInheritedStandardHandle(
            STD_INPUT_HANDLE, GENERIC_READ, false);
    inherited_standard_handles[1] =
        CreateMuonNodeInheritedStandardHandle(
            STD_ERROR_HANDLE, GENERIC_WRITE, true);
    inherited_standard_handles[2] =
        CreateMuonNodeInheritedStandardHandle(
            STD_ERROR_HANDLE, GENERIC_WRITE, true);

    MuonWindowsJobProcessLaunchOptions launch_options{};
    launch_options.application_name = executable.c_str();
    launch_options.command_line = command_line_buffer.data();
    launch_options.environment = environment.data();
    launch_options.creation_flags =
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    launch_options.startup_info.dwFlags = STARTF_USESTDHANDLES;
    launch_options.startup_info.hStdInput =
        inherited_standard_handles[0];
    launch_options.startup_info.hStdOutput =
        inherited_standard_handles[1];
    launch_options.startup_info.hStdError =
        inherited_standard_handles[2];
    launch_options.inherited_handles =
        inherited_standard_handles.data();
    launch_options.inherited_handle_count =
        inherited_standard_handles.size();
    launch_options.lifetime =
        MuonWindowsJobProcessLifetime::KillOnOwnerClose;
    auto launch_error = MuonWindowsJobProcessLaunchError{};
    if (!LaunchMuonWindowsJobProcess(
            launch_options, &runtime->process, &launch_error)) {
      const auto* message =
          launch_error.message == nullptr
              ? "Failed to launch Node service process"
              : launch_error.message;
      throw std::system_error(
          static_cast<int>(launch_error.windows_error),
          std::system_category(),
          "Failed to start Node executable " +
              runtime->executable_path + ": " + message);
    }
    for (auto& handle : inherited_standard_handles) {
      CloseMuonNodeWindowsHandle(&handle);
    }
    runtime->process_started = true;
  } catch (...) {
    for (auto& handle : inherited_standard_handles) {
      CloseMuonNodeWindowsHandle(&handle);
    }
    throw;
  }
  // Observe pre-connect exits so ConnectNamedPipe cannot strand startup.
  spawned();
  co_await ConnectMuonNodeWindowsPipe(runtime);
  // Authenticate the peer before the caller can send the initialize frame,
  // which is the first frame containing the session token.
  ULONG pipe_client_process_id = 0;
  if (GetNamedPipeClientProcessId(
          runtime->pipe, &pipe_client_process_id) == FALSE) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "GetNamedPipeClientProcessId failed for Node service process");
  }
  if (runtime->process.process_id == 0 ||
      pipe_client_process_id != runtime->process.process_id) {
    throw std::runtime_error(
        "Node IPC pipe was connected by an unexpected process");
  }
  runtime->transport_connected = true;
}
#else
static std::vector<std::string> CreateMuonNodePosixEnvironment(
    const std::string& token) {
  std::vector<std::string> entries;
  for (auto** cursor = environ; cursor != nullptr && *cursor != nullptr;
       ++cursor) {
    const auto entry = std::string(*cursor);
    if (entry.rfind("MUON_NODE_FD=", 0) == 0 ||
        entry.rfind("MUON_NODE_TOKEN=", 0) == 0) {
      continue;
    }
    entries.push_back(entry);
  }
  entries.push_back("MUON_NODE_FD=3");
  entries.push_back("MUON_NODE_TOKEN=" + token);
  return entries;
}

cardio::promise<void> StartMuonNodeProcess(
    MuonNodeProcess* runtime, std::function<void()> spawned) {
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    throw std::system_error(
        errno, std::generic_category(),
        "socketpair failed for Node service process");
  }
  runtime->socket_fd = sockets[0];
  auto child_socket = sockets[1];
  if (child_socket == 3) {
    const auto duplicated = fcntl(child_socket, F_DUPFD_CLOEXEC, 4);
    if (duplicated < 0) {
      (void)::close(runtime->socket_fd);
      (void)::close(child_socket);
      runtime->socket_fd = -1;
      throw std::system_error(
          errno, std::generic_category(),
          "Failed to reserve the Node service process descriptor");
    }
    (void)::close(child_socket);
    child_socket = duplicated;
  }
  const auto close_sockets = [&]() {
    if (runtime->socket_fd >= 0) {
      (void)::close(runtime->socket_fd);
      runtime->socket_fd = -1;
    }
    if (child_socket >= 0) {
      (void)::close(child_socket);
    }
  };

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close_sockets();
    throw std::runtime_error(
        "posix_spawn file actions initialization failed");
  }
  const auto destroy_actions =
      std::unique_ptr<posix_spawn_file_actions_t,
                      std::function<void(posix_spawn_file_actions_t*)>>(
          &actions, [](posix_spawn_file_actions_t* value) {
            (void)posix_spawn_file_actions_destroy(value);
          });
  auto action_error =
      posix_spawn_file_actions_addclose(&actions, runtime->socket_fd);
  if (action_error == 0) {
    action_error =
        posix_spawn_file_actions_adddup2(&actions, child_socket, 3);
  }
  if (action_error == 0) {
    action_error =
        posix_spawn_file_actions_addclose(&actions, child_socket);
  }
  // The service host's stdin/stdout carry supervisor ownership and readiness.
  // Author reads must not consume that pipe, and logs belong on stderr.
  if (action_error == 0)
    action_error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (action_error == 0)
    action_error = posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
  if (action_error != 0) {
    close_sockets();
    throw std::system_error(
        action_error, std::generic_category(),
        "Failed to configure Node service process descriptors");
  }
  const auto current_flags = fcntl(runtime->socket_fd, F_GETFL, 0);
  if (current_flags < 0 ||
      fcntl(runtime->socket_fd, F_SETFL, current_flags | O_NONBLOCK) != 0) {
    const auto configure_error = errno;
    close_sockets();
    throw std::system_error(
        configure_error, std::generic_category(),
        "Failed to configure the Node IPC socket");
  }

  auto arguments_storage =
      std::vector<std::string>{runtime->executable_path,
                               runtime->bridge_path};
  std::vector<char*> arguments;
  for (auto& argument : arguments_storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);
  auto environment_storage =
      CreateMuonNodePosixEnvironment(runtime->session_token);
  std::vector<char*> environment;
  for (auto& entry : environment_storage) {
    environment.push_back(entry.data());
  }
  environment.push_back(nullptr);

  pid_t process_id = -1;
  const auto spawn_error = posix_spawn(
      &process_id, runtime->executable_path.c_str(), &actions, nullptr,
      arguments.data(), environment.data());
  (void)::close(child_socket);
  if (spawn_error != 0) {
    (void)::close(runtime->socket_fd);
    runtime->socket_fd = -1;
    throw std::system_error(
        spawn_error, std::generic_category(),
        "Failed to start Node executable " + runtime->executable_path);
  }
#if defined(SYS_pidfd_open)
  const auto process_fd =
      static_cast<int>(::syscall(SYS_pidfd_open, process_id, 0));
  if (process_fd < 0) {
    const auto pidfd_error = errno;
    (void)::kill(process_id, SIGKILL);
    int ignored_status = 0;
    (void)::waitpid(process_id, &ignored_status, 0);
    CloseMuonNodeTransport(runtime);
    throw std::system_error(
        pidfd_error, std::generic_category(),
        "pidfd_open failed for Node service process");
  }
  runtime->process_fd = process_fd;
#else
  (void)::kill(process_id, SIGKILL);
  int ignored_status = 0;
  (void)::waitpid(process_id, &ignored_status, 0);
  CloseMuonNodeTransport(runtime);
  throw std::runtime_error(
      "pidfd_open is required for the Node service process");
#endif
  runtime->process_id = process_id;
  runtime->process_started = true;
  runtime->transport_connected = true;
  spawned();
  co_return;
}
#endif

cardio::promise<MuonNodeProcessExit> WaitMuonNodeProcess(MuonNodeProcess* runtime) {
  auto exit_code = -1;
  auto exit_status_available = false;
#if defined(_WIN32)
    if (runtime->process.process == nullptr) {
      throw std::runtime_error("Node process handle is unavailable");
    }
    (void)co_await cardio::from_win32_handle(
        runtime->process.process);
    DWORD native_exit_code = 0;
    if (GetExitCodeProcess(
            runtime->process.process, &native_exit_code) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "GetExitCodeProcess failed for Node service process");
    }
    exit_code = static_cast<int>(native_exit_code);
    exit_status_available = true;
#else
    if (runtime->process_fd < 0 || runtime->process_id <= 0) {
      throw std::runtime_error("Node process descriptor is unavailable");
    }
    (void)co_await cardio::from_fd(
        runtime->process_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup);
    int status = 0;
    for (;;) {
      const auto waited = ::waitpid(runtime->process_id, &status, 0);
      if (waited == runtime->process_id) {
        exit_status_available = true;
        break;
      }
      const auto wait_error = errno;
      if (waited < 0 && wait_error == EINTR) {
        continue;
      }
      if (waited < 0 && wait_error == ECHILD) {
        break;
      }
      throw std::system_error(
          wait_error, std::generic_category(),
          "waitpid failed for Node service process");
    }
    if (exit_status_available && WIFEXITED(status)) {
      exit_code = WEXITSTATUS(status);
    } else if (exit_status_available && WIFSIGNALED(status)) {
      exit_code = 128 + WTERMSIG(status);
    } else {
      exit_status_available = false;
    }
#endif
  co_return MuonNodeProcessExit{exit_code, exit_status_available};
}
