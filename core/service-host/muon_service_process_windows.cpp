/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_process.h"
#include "muon_filesystem.h"
#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <array>
#include <span>
#include <utility>
#include <vector>

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(static_cast<int>(GetLastError()),
                                        std::system_category(), operation);
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  ~Handle() { Close(); }
  void Close() {
    const auto handle = std::exchange(value, INVALID_HANDLE_VALUE);
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

static std::shared_ptr<void> PipeSecurity() {
  Handle token;
  Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != 0, "open bootstrap token");
  DWORD length = 0;
  GetTokenInformation(token.value, TokenLogonSid, nullptr, 0, &length);
  Check(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size bootstrap logon SID");
  auto bytes = std::vector<unsigned char>(length);
  Check(GetTokenInformation(token.value, TokenLogonSid, bytes.data(), length, &length) != 0,
        "read bootstrap logon SID");
  const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
  if (groups->GroupCount != 1) throw std::runtime_error("Missing bootstrap logon SID");
  LPWSTR sid = nullptr;
  Check(ConvertSidToStringSidW(groups->Groups[0].Sid, &sid) != 0, "format bootstrap logon SID");
  auto sid_owner = std::shared_ptr<void>(sid, [](void* value) { LocalFree(value); });
  const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
      &descriptor, nullptr) != 0, "create bootstrap pipe security");
  return {descriptor, [](void* value) { LocalFree(value); }};
}

struct Pipe {
  Handle read;
  Handle write;
};

static Pipe CreatePipePair(PSECURITY_DESCRIPTOR security) {
  GUID id{};
  const auto result = CoCreateGuid(&id);
  if (FAILED(result)) throw std::system_error(static_cast<int>(result),
      std::system_category(), "create bootstrap pipe identity");
  wchar_t text[40];
  if (!StringFromGUID2(id, text, static_cast<int>(std::size(text))))
    throw std::runtime_error("Cannot format bootstrap pipe identity");
  const auto name = std::wstring(L"\\\\.\\pipe\\muon-bootstrap-") + text;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security, FALSE};
  Handle read{CreateNamedPipeW(name.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 4096, 0, &attributes)};
  Check(read.value != INVALID_HANDLE_VALUE, "create bootstrap pipe");
  // Opening the client connects this private pair before it is inherited. The
  // read end is overlapped; ordinary CRT writes on the child use the sync end.
  Handle write{CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
  Check(write.value != INVALID_HANDLE_VALUE, "connect bootstrap pipe");
  return {std::move(read), std::move(write)};
}

static std::wstring Quote(const std::wstring& argument) {
  std::wstring result = L"\"";
  size_t backslashes = 0;
  for (const auto character : argument) {
    if (character == L'\\') { ++backslashes; continue; }
    result.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    result.push_back(character);
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

static cardio::promise<void> Detach(std::shared_ptr<Handle> input) {
  // No writes are submitted on this handle; EOF is the entire bootstrap signal.
  input->Close();
  co_return;
}

cardio::promise<MuonServiceProcess> LaunchMuonServiceProcess(
    MuonServiceProcessOptions options, cardio::cancellation cancellation) {
  for (const auto* path : {&options.executable, &options.registry, &options.data_root}) {
    const auto native = muon_internal::FilesystemPathFromUtf8(*path);
    const auto absolute = native.is_absolute() || native.native().starts_with(L"\\\\") ||
        native.native().starts_with(L"//");
    if (!absolute || path->find('\0') != std::string::npos)
      throw std::invalid_argument("Service process paths must be absolute and contain no NUL");
  }
  if (options.endpoint.find('\0') != std::string::npos)
    throw std::invalid_argument("Service endpoint cannot contain NUL");
  if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  const auto security = PipeSecurity();
  auto input = CreatePipePair(security.get());
  auto output = CreatePipePair(security.get());
  Handle error;
  const auto inherited_error = GetStdHandle(STD_ERROR_HANDLE);
  if (inherited_error && inherited_error != INVALID_HANDLE_VALUE) {
    Check(DuplicateHandle(GetCurrentProcess(), inherited_error, GetCurrentProcess(), &error.value,
        0, TRUE, DUPLICATE_SAME_ACCESS) != 0, "inherit service diagnostics");
  } else {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    error.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &attributes, OPEN_EXISTING, 0, nullptr);
    Check(error.value != INVALID_HANDLE_VALUE, "open service diagnostics sink");
  }
  Check(SetHandleInformation(input.read.value, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != 0,
        "inherit bootstrap input");
  Check(SetHandleInformation(output.write.value, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) != 0,
        "inherit bootstrap output");
  std::array<HANDLE, 3> inherited{input.read.value, output.write.value, error.value};
  SIZE_T attribute_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  Check(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size service process attributes");
  auto attribute_bytes = std::vector<unsigned char>(attribute_size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_bytes.data());
  Check(InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) != 0,
        "initialize service process attributes");
  struct Attributes {
    LPPROC_THREAD_ATTRIBUTE_LIST value;
    ~Attributes() { DeleteProcThreadAttributeList(value); }
  } attribute_scope{attributes};
  Check(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      inherited.data(), sizeof(inherited), nullptr, nullptr) != 0, "limit inherited service handles");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.read.value;
  startup.StartupInfo.hStdOutput = output.write.value;
  startup.StartupInfo.hStdError = error.value;
  startup.lpAttributeList = attributes;
  const auto executable = muon_internal::FilesystemPathForIo(
      muon_internal::FilesystemPathFromUtf8(options.executable), muon_internal::FilesystemIoUsage::File);
  auto command = Quote(executable.native());
  for (const auto& argument : {std::string("--config"), options.registry, std::string("--endpoint"),
      options.endpoint, std::string("--data-root"), options.data_root, std::string("--bootstrap"), std::string("stdin")})
    command += L" " + Quote(muon_internal::FilesystemPathFromUtf8(argument).native());
  PROCESS_INFORMATION created{};
  Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
      &startup.StartupInfo, &created) != 0, "launch independent service host");
  Handle process{created.hProcess};
  Handle thread{created.hThread};
  input.read.Close();
  output.write.Close();
  error.Close();
  std::string text;
  bool ready = false;
  while (!ready) {
    std::array<char, 4096> bytes;
    size_t size = 0;
    try {
      size = co_await cardio::win32::read(output.read.value,
          std::as_writable_bytes(std::span(bytes)), cancellation);
    } catch (const std::system_error& failure) {
      if (failure.code().value() != ERROR_BROKEN_PIPE) throw;
    }
    if (size == 0) throw std::runtime_error("Service host exited before readiness");
    text.append(bytes.data(), size);
    if (text.size() > 1024 * 1024) throw std::runtime_error("Service readiness output exceeds 1 MiB");
    size_t end;
    while ((end = text.find('\n')) != std::string::npos) {
      auto line = text.substr(0, end);
      if (line.ends_with('\r')) line.pop_back();
      if (line == "{\"ready\":true}") ready = true;
      text.erase(0, end + 1);
    }
  }
  if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  // Closing stdout prevents a resident host from blocking on unread snapshots.
  // Handle disposal never terminates the independently running child process.
  output.read.Close();
  auto bootstrap = std::make_shared<Handle>(std::move(input.write));
  co_return MuonServiceProcess{[bootstrap] { return Detach(bootstrap); }};
}
