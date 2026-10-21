/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "server/muon_server_socket.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TestClient {
#if defined(_WIN32)
  SOCKET socket = INVALID_SOCKET;
  WSAEVENT event = WSA_INVALID_EVENT;
  ~TestClient() {
    if (socket != INVALID_SOCKET) closesocket(socket);
    if (event != WSA_INVALID_EVENT) WSACloseEvent(event);
  }
#else
  int socket = -1;
  ~TestClient() { if (socket >= 0) ::close(socket); }
#endif
};

static cardio::promise<void> WaitClient(const std::shared_ptr<TestClient>& client, bool writing) {
#if defined(_WIN32)
  const auto result = co_await cardio::from_win32_handle(client->event);
  Expect(result == cardio::win32_handle_event::signaled, "client event wait failed");
  WSANETWORKEVENTS events{};
  Expect(WSAEnumNetworkEvents(client->socket, client->event, &events) == 0,
         "client event collection failed");
  (void)writing;
#else
  (void)co_await cardio::from_fd(client->socket,
      writing ? cardio::fd_event::write : cardio::fd_event::read);
#endif
}

static cardio::promise<std::shared_ptr<TestClient>> Connect(uint16_t port, bool small_buffer) {
  auto client = std::make_shared<TestClient>();
#if defined(_WIN32)
  client->socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  Expect(client->socket != INVALID_SOCKET, "client socket failed");
  client->event = WSACreateEvent();
  Expect(client->event != WSA_INVALID_EVENT, "client event failed");
  Expect(WSAEventSelect(client->socket, client->event,
      FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE) == 0, "client event selection failed");
#else
  client->socket = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  Expect(client->socket >= 0, "client socket failed");
#endif
  if (small_buffer) {
    const auto size = int{4096};
    Expect(setsockopt(client->socket, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<const char*>(&size), sizeof(size)) == 0, "client receive limit failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const auto result = ::connect(client->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result != 0) {
#if defined(_WIN32)
    Expect(WSAGetLastError() == WSAEWOULDBLOCK, "client connect failed");
#else
    Expect(errno == EINPROGRESS, "client connect failed");
#endif
    co_await WaitClient(client, true);
    auto error = int{0};
#if defined(_WIN32)
    auto length = int{sizeof(error)};
#else
    auto length = socklen_t{sizeof(error)};
#endif
    Expect(getsockopt(client->socket, SOL_SOCKET, SO_ERROR,
        reinterpret_cast<char*>(&error), &length) == 0 && error == 0, "client connect completion failed");
  }
  co_return client;
}

static cardio::promise<size_t> ReadClient(const std::shared_ptr<TestClient>& client,
                                        std::span<uint8_t> buffer) {
  while (true) {
    const auto result = recv(client->socket, reinterpret_cast<char*>(buffer.data()),
                            static_cast<int>(buffer.size()), 0);
    if (result >= 0) co_return static_cast<size_t>(result);
#if defined(_WIN32)
    Expect(WSAGetLastError() == WSAEWOULDBLOCK, "client read failed");
#else
    if (errno == EINTR) continue;
    Expect(errno == EAGAIN || errno == EWOULDBLOCK, "client read failed");
#endif
    co_await WaitClient(client, false);
  }
}

static cardio::promise<void> WriteClient(const std::shared_ptr<TestClient>& client,
                                       std::span<const uint8_t> buffer) {
  while (!buffer.empty()) {
#if defined(_WIN32)
    const auto flags = 0;
#else
    const auto flags = MSG_NOSIGNAL;
#endif
    const auto result = send(client->socket, reinterpret_cast<const char*>(buffer.data()),
                            static_cast<int>(buffer.size()), flags);
    if (result > 0) { buffer = buffer.subspan(static_cast<size_t>(result)); continue; }
#if defined(_WIN32)
    Expect(result < 0 && WSAGetLastError() == WSAEWOULDBLOCK, "client write failed");
#else
    if (result < 0 && errno == EINTR) continue;
    Expect(result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK), "client write failed");
#endif
    co_await WaitClient(client, true);
  }
}

template <typename T>
static cardio::promise<void> ExpectFailure(cardio::promise<T>& operation) {
  auto failed = false;
  try { (void)co_await operation; }
  catch (const std::exception&) { failed = true; }
  Expect(failed, "operation unexpectedly succeeded");
}

static cardio::promise<void> TestBinaryAndHalfClose(const std::shared_ptr<MuonServerSockets>& sockets) {
  auto listener = sockets->listen(0);
  Expect(listener->port != 0, "listener did not report its selected port");
  auto accepting = listener->accept();
  Expect(!accepting.is_ready(), "accept did not wait for a client");
  auto client = co_await Connect(listener->port, false);
  auto server = co_await accepting;
  std::array<uint8_t, 5> data{0, 255, 13, 10, 128};
  std::array<uint8_t, 5> received{};
  auto reading = server->read(received);
  Expect(!reading.is_ready(), "read did not wait for client data");
  auto duplicate_failed = false;
  try { (void)co_await server->read(received); }
  catch (const std::exception&) { duplicate_failed = true; }
  Expect(duplicate_failed, "concurrent read was accepted");
  co_await WriteClient(client, data);
  auto count = co_await reading;
  while (count < received.size()) {
    const auto next = co_await server->read(std::span(received).subspan(count));
    Expect(next != 0, "binary request ended early");
    count += next;
  }
  Expect(received == data, "binary request changed");
#if defined(_WIN32)
  Expect(shutdown(client->socket, SD_SEND) == 0, "client half-close failed");
#else
  Expect(shutdown(client->socket, SHUT_WR) == 0, "client half-close failed");
#endif
  const auto half_close = co_await server->read(received);
  Expect(half_close == 0, "read did not report EOF");
  co_await server->write(data);
  co_await server->close();
  count = 0;
  while (count < received.size()) {
    const auto next = co_await ReadClient(client, std::span(received).subspan(count));
    Expect(next != 0, "half-closed client lost the response");
    count += next;
  }
  Expect(received == data, "binary response changed");
  const auto closed = co_await ReadClient(client, received);
  Expect(closed == 0, "server did not close");
  co_await listener->close();
}

static cardio::promise<void> TestBackpressure(const std::shared_ptr<MuonServerSockets>& sockets) {
  auto listener = sockets->listen(0);
  auto client = co_await Connect(listener->port, true);
  auto server = co_await listener->accept();
  auto data = std::vector<uint8_t>(8 * 1024 * 1024);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 17 + i / 251);
  auto writing = server->write(data);
  Expect(!writing.is_ready(), "test could not establish a blocked write within its 8 MiB buffer budget");
  auto duplicate_failed = false;
  try { co_await server->write(data); }
  catch (const std::exception&) { duplicate_failed = true; }
  Expect(duplicate_failed, "concurrent write was accepted");
  // Another connection must progress while the first one cannot write.
  auto other_client = co_await Connect(listener->port, false);
  auto other_server = co_await listener->accept();
  const auto marker = std::array<uint8_t, 1>{42};
  co_await other_server->write(marker);
  auto marker_received = std::array<uint8_t, 1>{};
  const auto marker_count = co_await ReadClient(other_client, marker_received);
  Expect(marker_count == 1 && marker_received == marker,
         "blocked write stalled another connection");
  co_await other_server->close();
  std::array<uint8_t, 32768> buffer{};
  auto offset = size_t{0};
  while (offset < data.size()) {
    const auto count = co_await ReadClient(client, buffer);
    Expect(count > 0 && offset + count <= data.size(), "large response was truncated");
    Expect(std::equal(buffer.begin(), buffer.begin() + count, data.begin() + offset),
           "large response was corrupted or reordered");
    offset += count;
  }
  co_await writing;
  co_await server->close();
  const auto closed = co_await ReadClient(client, buffer);
  Expect(closed == 0, "large response did not finish");
  co_await listener->close();
}

static cardio::promise<void> TestCancellation(const std::shared_ptr<MuonServerSockets>& sockets) {
  auto listener = sockets->listen(0);
  const auto port = listener->port;
  auto accepting = listener->accept();
  auto accept_failure = ExpectFailure(accepting);
  auto close_one = listener->close();
  auto close_two = listener->close();
  co_await close_one;
  co_await close_two;
  Expect(accepting.is_ready(), "listener close left accept pending");
  co_await accept_failure;
  listener = sockets->listen(port);
  auto client = co_await Connect(port, true);
  auto server = co_await listener->accept();
  std::array<uint8_t, 1> byte{};
  auto reading = server->read(byte);
  auto data = std::vector<uint8_t>(8 * 1024 * 1024, 123);
  auto writing = server->write(data);
  Expect(!writing.is_ready(), "cancellation test could not establish a blocked write within its 8 MiB buffer budget");
  auto read_failure = ExpectFailure(reading);
  auto write_failure = ExpectFailure(writing);
  close_one = server->close();
  close_two = server->close();
  co_await close_one;
  co_await close_two;
  Expect(reading.is_ready() && writing.is_ready(), "close left an I/O operation pending");
  co_await read_failure;
  co_await write_failure;
  co_await listener->close();
}

static cardio::promise<void> TestManyConnectionsAndStop(const std::shared_ptr<MuonServerSockets>& sockets) {
  auto listener = sockets->listen(0);
  constexpr auto count = size_t{80};
  auto clients = std::vector<std::shared_ptr<TestClient>>{};
  auto servers = std::vector<std::shared_ptr<MuonServerSocket>>{};
  auto reads = std::vector<cardio::promise<size_t>>{};
  auto data = std::array<std::array<uint8_t, 1>, count>{};
  for (size_t i = 0; i < count; ++i) {
    clients.push_back(co_await Connect(listener->port, false));
    servers.push_back(co_await listener->accept());
    reads.push_back(servers.back()->read(data[i]));
  }
  for (size_t i = 0; i < count; ++i) {
    const auto byte = std::array<uint8_t, 1>{static_cast<uint8_t>(i)};
    co_await WriteClient(clients[i], byte);
  }
  for (size_t i = 0; i < count; ++i) {
    const auto received = co_await reads[i];
    Expect(received == 1 && data[i][0] == i, "shared readiness lost a connection");
    reads[i] = servers[i]->read(data[i]);
  }
  auto failures = std::vector<cardio::promise<void>>{};
  failures.reserve(count);
  for (auto& reading : reads) failures.push_back(ExpectFailure(reading));
  auto accepting = listener->accept();
  auto accept_failure = ExpectFailure(accepting);
  auto stop_one = sockets->stop();
  auto stop_two = sockets->stop();
  co_await stop_one;
  co_await stop_two;
  for (const auto& reading : reads)
    Expect(reading.is_ready(), "stop left a connection pending");
  for (auto& failure : failures) co_await failure;
  co_await accept_failure;
  auto rejected = false;
  try { (void)sockets->listen(0); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected, "stopped service accepted a new listener");
}

static cardio::promise<void> RunTests(bool& passed, std::string selected) {
  auto sockets = CreateMuonServerSockets();
  std::exception_ptr failure;
  const char* test_case = "binary and half-close";
  try {
    if (selected.empty() || selected == "binary") co_await TestBinaryAndHalfClose(sockets);
    test_case = "backpressure";
    if (selected.empty() || selected == "backpressure") co_await TestBackpressure(sockets);
    test_case = "cancellation";
    if (selected.empty() || selected == "cancellation") co_await TestCancellation(sockets);
    test_case = "many connections and stop";
    if (selected.empty() || selected == "many") co_await TestManyConnectionsAndStop(sockets);
  } catch (...) { failure = std::current_exception(); }
  co_await sockets->stop();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception& error) { std::cerr << test_case << ": " << error.what() << '\n'; }
  } else { passed = true; }
}

int main(int argc, char** argv) {
  const auto selected = argc == 2 ? std::string(argv[1]) : std::string{};
  if (argc > 2 || (!selected.empty() && selected != "binary" && selected != "backpressure" &&
      selected != "cancellation" && selected != "many")) return 2;
#if defined(_WIN32)
  auto dispatcher = cardio::dispatcher_host_win32(cardio::dispatcher_thread_policy::current_thread);
#else
  auto dispatcher = cardio::dispatcher_host();
#endif
  cardio::set_current_dispatcher(&dispatcher);
  auto passed = false;
  auto pending = RunTests(passed, selected);
  dispatcher.park();
  cardio::set_current_dispatcher(nullptr);
  if (passed) std::cout << "Server socket " << (selected.empty() ? "all tests" : selected) << " passed\n";
  return passed ? 0 : 1;
}
