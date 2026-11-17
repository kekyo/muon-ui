/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_transport.h"
#include "muon_service_process_lock.h"
#include "muon_filesystem.h"

#include <windows.h>
#include <span>
#include <system_error>
#include <vector>

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(static_cast<int>(GetLastError()),
                                        std::system_category(), operation);
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

static std::vector<unsigned char> LogonSid(HANDLE process) {
  Handle token;
  Check(OpenProcessToken(process, TOKEN_QUERY, &token.value) != 0, "open service peer token");
  DWORD length = 0;
  GetTokenInformation(token.value, TokenLogonSid, nullptr, 0, &length);
  Check(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size service peer logon SID");
  auto bytes = std::vector<unsigned char>(length);
  Check(GetTokenInformation(token.value, TokenLogonSid, bytes.data(), length, &length) != 0,
        "read service peer logon SID");
  const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
  if (groups->GroupCount != 1 || !IsValidSid(groups->Groups[0].Sid))
    throw std::system_error(ERROR_ACCESS_DENIED, std::system_category(), "Missing service peer logon SID");
  auto sid = std::vector<unsigned char>(GetLengthSid(groups->Groups[0].Sid));
  Check(CopySid(static_cast<DWORD>(sid.size()), sid.data(), groups->Groups[0].Sid) != 0,
        "copy service peer logon SID");
  return sid;
}

static void Authenticate(HANDLE pipe) {
  ULONG pid = 0;
  Check(GetNamedPipeServerProcessId(pipe, &pid) != 0, "identify service pipe server");
  Handle server{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  Check(server.value != nullptr, "open service pipe server process");
  const auto local = LogonSid(GetCurrentProcess());
  const auto remote = LogonSid(server.value);
  if (!EqualSid(const_cast<unsigned char*>(local.data()), const_cast<unsigned char*>(remote.data())))
    throw std::system_error(ERROR_ACCESS_DENIED, std::system_category(), "Service peer logon session differs");
}

struct PipeState {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  cardio::cancellation_source cancellation;
  size_t pending = 0;
  bool closed = false;

  ~PipeState() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }

  void DisposeIfIdle() {
    if (closed && pending == 0 && pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
    }
  }

  void Close() {
    if (closed) return;
    closed = true;
    cancellation.cancel();
    DisposeIfIdle();
  }
};

struct Operation {
  std::shared_ptr<PipeState> state;
  explicit Operation(std::shared_ptr<PipeState> value) : state(std::move(value)) {
    if (state->closed) throw cardio::canceled_exception();
    ++state->pending;
  }
  ~Operation() { --state->pending; state->DisposeIfIdle(); }
};

static cardio::promise<std::string> Read(std::shared_ptr<PipeState> state) {
  auto operation = Operation(state);
  char bytes[8192];
  try {
    const auto size = co_await cardio::win32::read(state->pipe,
        std::as_writable_bytes(std::span(bytes)), state->cancellation.get_cancellation());
    co_return std::string(bytes, size);
  } catch (const std::system_error& error) {
    if (error.code().value() == ERROR_BROKEN_PIPE || error.code().value() == ERROR_PIPE_NOT_CONNECTED)
      co_return std::string{};
    throw;
  }
}

static cardio::promise<void> Write(std::shared_ptr<PipeState> state, std::string bytes) {
  auto operation = Operation(state);
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto size = co_await cardio::win32::write(state->pipe,
        std::as_bytes(std::span(bytes.data() + offset, bytes.size() - offset)),
        state->cancellation.get_cancellation());
    if (size == 0) throw std::system_error(ERROR_WRITE_FAULT,
        std::system_category(), "Empty service transport write");
    offset += size;
  }
}

cardio::promise<MuonServiceTransport> ConnectMuonServiceTransport(
    std::string endpoint, cardio::cancellation cancellation) {
  if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  // CreateFile has no asynchronous busy-pipe wait. Serialize establishment
  // through the server's rearm acknowledgement, not just the CreateFile call.
  const auto native = muon_internal::FilesystemPathFromUtf8(endpoint).native();
  constexpr auto prefix = L"\\\\.\\pipe\\muon-service-";
  if (!native.starts_with(prefix) || native.size() <= std::wcslen(prefix) ||
      native.size() > 256 || native.find(L'\\', std::wcslen(prefix)) != std::wstring::npos)
    throw std::invalid_argument("Service endpoint must be a local muon-service named pipe");
  auto connection = co_await AcquireMuonServiceProcessLock("connect:" + endpoint, cancellation);
  auto state = std::make_shared<PipeState>();
  state->pipe = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
      OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  Check(state->pipe != INVALID_HANDLE_VALUE, "connect service pipe");
  // A logon-only server DACL does not authenticate a client-facing pipe that
  // another account created first. Verify the kernel-reported server identity.
  Authenticate(state->pipe);
  auto registration = cancellation.on_cancellation_requested([state] { state->Close(); });
  std::string greeting;
  constexpr auto ready = std::string_view("{\"event\":\"ready\"}\n");
  while (greeting.size() < ready.size()) {
    auto chunk = co_await Read(state);
    if (chunk.empty()) throw std::system_error(ERROR_BROKEN_PIPE,
        std::system_category(), "Service host closed before readiness");
    greeting += chunk;
  }
  if (greeting != ready) throw std::runtime_error("Invalid service transport readiness frame");
  co_return MuonServiceTransport{
      [state] { return Read(state); },
      [state](std::string bytes) { return Write(state, std::move(bytes)); },
      [state] { state->Close(); }};
}
