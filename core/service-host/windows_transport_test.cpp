/* Deterministic Win32 transport barriers; no polling or elapsed-time ordering. */
#include "services/muon_service_transport.h"
#include "muon_filesystem.h"
#include <windows.h>
#include <iostream>
#include <span>
#include <vector>

struct Handle {
  HANDLE value;
  ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

static void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

static HANDLE NewPipe(const std::wstring& endpoint) {
  const auto pipe = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
      65536, 65536, 0, nullptr);
  Require(pipe != INVALID_HANDLE_VALUE, "create test pipe");
  return pipe;
}

static cardio::promise<void> Greet(HANDLE pipe) {
  bool connected = false;
  try {
    co_await cardio::win32::submit(pipe, [&connected](HANDLE value, OVERLAPPED& overlapped) {
      if (ConnectNamedPipe(value, &overlapped)) return DWORD{ERROR_SUCCESS};
      const auto error = GetLastError();
      connected = error == ERROR_PIPE_CONNECTED;
      return error;
    });
  } catch (const std::system_error&) { if (!connected) throw; }
  constexpr auto ready = std::string_view("{\"event\":\"ready\"}\n");
  co_await cardio::win32::write(pipe, std::as_bytes(std::span(ready.data(), ready.size())));
}

template <typename T>
static cardio::promise<bool> Cancelled(cardio::promise<T> operation) {
  try { co_await operation; }
  catch (const cardio::canceled_exception&) { co_return true; }
  co_return false;
}

static cardio::promise<void> Barriers() {
  const auto endpoint = std::wstring(L"\\\\.\\pipe\\muon-service-transport-") +
      std::to_wstring(GetCurrentProcessId());
  Handle server1{NewPipe(endpoint)};
  cardio::cancellation_source cancellation;
  const auto text = muon_internal::FilesystemPathToUtf8(endpoint);
  auto first = ConnectMuonServiceTransport(text, cancellation.get_cancellation());
  auto second = ConnectMuonServiceTransport(text, cancellation.get_cancellation());
  Require(!first.is_ready(), "Connect returned before the host rearm barrier");
  Require(!second.is_ready(), "Concurrent Connect did not wait for the first barrier");
  Handle server2{NewPipe(endpoint)};
  co_await Greet(server1.value);
  auto transport1 = co_await first;
  co_await Greet(server2.value);
  auto transport2 = co_await second;
  transport1.close();
  auto read = transport2.read();
  auto write = transport2.write(std::string(8 * 1024 * 1024, 'x'));
  Require(!read.is_ready() && !write.is_ready(), "Cancellation requires native pending I/O");
  auto read_cancelled = Cancelled(std::move(read));
  auto write_cancelled = Cancelled(std::move(write));
  transport2.close();
  transport2.close();
  Require((co_await read_cancelled) && (co_await write_cancelled),
          "Close did not drain pending read/write cancellation");
  cancellation.cancel();
  bool connect_cancelled = false;
  try { co_await ConnectMuonServiceTransport(text, cancellation.get_cancellation()); }
  catch (const cardio::canceled_exception&) { connect_cancelled = true; }
  Require(connect_cancelled, "Pre-cancelled Connect succeeded");
  bool missing = false;
  try { co_await ConnectMuonServiceTransport(text + "-missing", {}); }
  catch (const std::system_error& error) { missing = error.code().value() == ERROR_FILE_NOT_FOUND; }
  Require(missing, "Missing endpoint lost its native error code");
  std::cout << "transport barriers: passed" << std::endl;
}

static cardio::promise<void> Clients(std::string endpoint) {
  std::vector<std::unique_ptr<MuonServiceClient>> clients;
  std::vector<cardio::promise<std::string>> calls;
  for (int index = 0; index < 16; ++index) {
    auto client = std::make_unique<MuonServiceClient>([endpoint](cardio::cancellation signal) {
      return ConnectMuonServiceTransport(endpoint, signal);
    });
    calls.push_back(client->Call("acquire", "{\"serviceId\":\"resident.v1-test\"}"));
    clients.push_back(std::move(client));
  }
  for (auto& call : calls)
    Require((co_await call).find("resident.v1-test") != std::string::npos, "Missing native client reference");
  const auto state = co_await clients[0]->Call("getState", "{\"serviceId\":\"resident.v1-test\"}");
  Require(state.find("\"clientReferences\":16") != std::string::npos, "Concurrent owners were lost");
  for (auto& client : clients) co_await client->Close();
  std::cout << "native clients: passed" << std::endl;
}

static cardio::promise<void> Run(std::string endpoint, int& code) {
  try {
    co_await Barriers();
    if (!endpoint.empty()) co_await Clients(std::move(endpoint));
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    code = 1;
  }
}

int wmain(int argc, wchar_t** argv) {
  int code = 0;
  cardio::dispatcher_host_win32_auto dispatcher;
  cardio::fire_and_forget(Run(argc == 2 ? muon_internal::FilesystemPathToUtf8(argv[1]) : "", code));
  dispatcher.park();
  return code;
}
