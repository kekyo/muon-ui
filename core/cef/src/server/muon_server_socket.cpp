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
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
using MuonSocketHandle = SOCKET;
static constexpr auto kInvalidMuonSocket = INVALID_SOCKET;
#else
using MuonSocketHandle = int;
static constexpr auto kInvalidMuonSocket = -1;
#endif
using SocketSignal = std::shared_ptr<cardio::promise_source<void>>;

static void CloseNativeSocket(MuonSocketHandle handle) noexcept {
  if (handle == kInvalidMuonSocket) return;
#if defined(_WIN32)
  (void)::closesocket(handle);
#else
  (void)::close(handle);
#endif
}

static int SocketErrorCode() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

static std::system_error SocketError(int code, const char* operation) {
  return std::system_error(code, std::system_category(), operation);
}

static bool SocketWouldBlock(int code) {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK;
#else
  return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

static bool SocketInterrupted(int code) {
#if defined(_WIN32)
  return code == WSAEINTR;
#else
  return code == EINTR;
#endif
}

struct SocketEndpoint {
  MuonSocketHandle handle = kInvalidMuonSocket;
  bool listener = false;
  bool closing = false;
  bool closed = false;
  bool reading = false;
  bool writing = false;
  bool read_eof = false;
  cardio::cancellation_source cancellation;
  SocketSignal idle;
  std::vector<SocketSignal> closed_waiters;
#if defined(_WIN32)
  SocketSignal read_ready;
  SocketSignal write_ready;
#endif
  ~SocketEndpoint() { CloseNativeSocket(handle); }
};

struct SocketServiceState {
  cardio::dispatcher* dispatcher;
  bool stopping = false;
  bool stopped = false;
  std::map<MuonSocketHandle, std::shared_ptr<SocketEndpoint>> endpoints;
  std::vector<SocketSignal> stopped_waiters;
#if defined(_WIN32)
  bool winsock_started = false;
  WSAEVENT event = WSA_INVALID_EVENT;
  std::optional<cardio::promise<void>> pump;
  std::exception_ptr pump_failure;
  ~SocketServiceState() {
    // Destroying a suspended wait unregisters it before its event is released.
    pump.reset();
    endpoints.clear();
    if (event != WSA_INVALID_EVENT) WSACloseEvent(event);
    if (winsock_started) WSACleanup();
  }
#endif
};

static void CheckSocketDispatcher(const std::shared_ptr<SocketServiceState>& service) {
  if (&cardio::get_current_dispatcher() != service->dispatcher)
    throw std::runtime_error("Server socket used from a different dispatcher");
}

static void CheckSocketOpen(const std::shared_ptr<SocketEndpoint>& endpoint) {
  if (endpoint->closing) throw cardio::canceled_exception();
}

struct SocketOperation {
  std::shared_ptr<SocketEndpoint> endpoint;
  bool write;
  SocketOperation(std::shared_ptr<SocketEndpoint> value, bool writing)
      : endpoint(std::move(value)), write(writing) {
    CheckSocketOpen(endpoint);
    auto& active = write ? endpoint->writing : endpoint->reading;
    if (active) throw std::runtime_error("Concurrent server socket operation");
    active = true;
  }
  ~SocketOperation() {
    (write ? endpoint->writing : endpoint->reading) = false;
    if (!endpoint->reading && !endpoint->writing && endpoint->idle) {
      auto idle = std::exchange(endpoint->idle, {});
      idle->resolve();
    }
  }
  SocketOperation(const SocketOperation&) = delete;
  SocketOperation& operator=(const SocketOperation&) = delete;
};

#if defined(_WIN32)
struct SocketReadyCompletion {
  SocketSignal signal;
  int error;
};

static cardio::promise<void> PumpSocketEvents(SocketServiceState* service) {
  try {
    while (!service->stopping) {
      (void)co_await cardio::from_win32_handle(service->event);
      if (service->stopping) break;
      auto completions = std::vector<SocketReadyCompletion>{};
      auto reset = false;
      // Every socket shares one event, so the dispatcher consumes only one
      // HANDLE wait. Reset it atomically with the first socket's event record.
      // Later records are read without reset: an event for an already-visited
      // socket stays signaled for the next pass. No callbacks run during this
      // traversal, since resolving a cardio source may resume code inline.
      for (const auto& [handle, endpoint] : service->endpoints) {
        if (endpoint->closing) continue;
        WSANETWORKEVENTS events{};
        if (WSAEnumNetworkEvents(handle, reset ? nullptr : service->event, &events) != 0)
          throw SocketError(WSAGetLastError(), "Collect server socket events");
        reset = true;
        const auto read_event = endpoint->listener ? FD_ACCEPT : FD_READ;
        const auto read_index = endpoint->listener ? FD_ACCEPT_BIT : FD_READ_BIT;
        const auto close_error = (events.lNetworkEvents & FD_CLOSE) != 0
            ? events.iErrorCode[FD_CLOSE_BIT] : 0;
        if ((events.lNetworkEvents & (read_event | FD_CLOSE)) != 0 && endpoint->read_ready)
          completions.push_back({std::exchange(endpoint->read_ready, {}),
              close_error != 0 ? close_error : events.iErrorCode[read_index]});
        if ((events.lNetworkEvents & (FD_WRITE | FD_CLOSE)) != 0 && endpoint->write_ready)
          completions.push_back({std::exchange(endpoint->write_ready, {}),
              close_error != 0 ? close_error : events.iErrorCode[FD_WRITE_BIT]});
      }
      // An event may outlive the last closed socket. There is no associated
      // socket left whose record could race this reset on our dispatcher.
      if (!reset && !WSAResetEvent(service->event))
        throw SocketError(WSAGetLastError(), "Reset server socket event");
      for (const auto& completion : completions) {
        if (completion.error != 0)
          completion.signal->try_reject(std::make_exception_ptr(
              SocketError(completion.error, "Server socket readiness")));
        else completion.signal->try_resolve();
      }
    }
  } catch (const cardio::canceled_exception&) {
    if (!service->stopping) service->pump_failure = std::current_exception();
  } catch (...) { service->pump_failure = std::current_exception(); }
  if (service->pump_failure) {
    auto signals = std::vector<SocketSignal>{};
    for (const auto& [handle, endpoint] : service->endpoints) {
      (void)handle;
      if (endpoint->read_ready) signals.push_back(std::exchange(endpoint->read_ready, {}));
      if (endpoint->write_ready) signals.push_back(std::exchange(endpoint->write_ready, {}));
    }
    for (const auto& signal : signals) signal->try_reject(service->pump_failure);
  }
}
#endif

static cardio::promise<void> WaitSocketReady(
    std::shared_ptr<SocketServiceState> service,
    std::shared_ptr<SocketEndpoint> endpoint, bool writing) {
  CheckSocketOpen(endpoint);
#if defined(_WIN32)
  if (service->pump_failure) std::rethrow_exception(service->pump_failure);
  auto signal = std::make_shared<cardio::promise_source<void>>();
  auto ready = signal->get_promise();
  (writing ? endpoint->write_ready : endpoint->read_ready) = signal;
  co_await ready;
#else
  (void)service;
  (void)co_await cardio::from_fd(endpoint->handle,
      writing ? cardio::fd_event::write : cardio::fd_event::read,
      endpoint->cancellation.get_cancellation());
#endif
  CheckSocketOpen(endpoint);
}

static cardio::promise<size_t> ReadSocket(std::shared_ptr<SocketServiceState> service,
                                        std::shared_ptr<SocketEndpoint> endpoint,
                                        std::span<uint8_t> buffer) {
  CheckSocketDispatcher(service);
  auto operation = SocketOperation(endpoint, false);
  if (buffer.empty() || endpoint->read_eof) co_return 0;
  while (true) {
    CheckSocketOpen(endpoint);
    const auto length = static_cast<int>(std::min(buffer.size(),
        static_cast<size_t>(std::numeric_limits<int>::max())));
    const auto count = ::recv(endpoint->handle, reinterpret_cast<char*>(buffer.data()), length, 0);
    if (count >= 0) {
      if (count == 0) endpoint->read_eof = true;
      co_return static_cast<size_t>(count);
    }
    const auto error = SocketErrorCode();
    if (SocketInterrupted(error)) continue;
    if (!SocketWouldBlock(error)) throw SocketError(error, "Read server socket");
    co_await WaitSocketReady(service, endpoint, false);
  }
}

static cardio::promise<void> WriteSocket(std::shared_ptr<SocketServiceState> service,
                                        std::shared_ptr<SocketEndpoint> endpoint,
                                        std::span<const uint8_t> buffer) {
  CheckSocketDispatcher(service);
  auto operation = SocketOperation(endpoint, true);
  while (!buffer.empty()) {
    CheckSocketOpen(endpoint);
    const auto length = static_cast<int>(std::min(buffer.size(),
        static_cast<size_t>(std::numeric_limits<int>::max())));
#if defined(_WIN32)
    const auto flags = 0;
#else
    const auto flags = MSG_NOSIGNAL;
#endif
    const auto count = ::send(endpoint->handle, reinterpret_cast<const char*>(buffer.data()), length, flags);
    if (count > 0) { buffer = buffer.subspan(static_cast<size_t>(count)); continue; }
    if (count == 0) throw std::runtime_error("Server socket write made no progress");
    const auto error = SocketErrorCode();
    if (SocketInterrupted(error)) continue;
    if (!SocketWouldBlock(error)) throw SocketError(error, "Write server socket");
    co_await WaitSocketReady(service, endpoint, true);
  }
}

static cardio::promise<void> CloseSocket(std::shared_ptr<SocketServiceState> service,
                                        std::shared_ptr<SocketEndpoint> endpoint) {
  CheckSocketDispatcher(service);
  if (endpoint->closed) co_return;
  if (endpoint->closing) {
    auto signal = std::make_shared<cardio::promise_source<void>>();
    auto done = signal->get_promise();
    endpoint->closed_waiters.push_back(signal);
    co_await done;
    co_return;
  }
  endpoint->closing = true;
#if defined(_WIN32)
  auto reader = std::exchange(endpoint->read_ready, {});
  auto writer = std::exchange(endpoint->write_ready, {});
  const auto canceled = std::make_exception_ptr(cardio::canceled_exception());
  if (reader) reader->try_reject(canceled);
  if (writer) writer->try_reject(canceled);
#else
  endpoint->cancellation.cancel();
#endif
  if (endpoint->reading || endpoint->writing) {
    endpoint->idle = std::make_shared<cardio::promise_source<void>>();
    co_await endpoint->idle->get_promise();
  }
  // A guard can signal idle while its coroutine is still unwinding. Awaiting
  // an already-resolved cardio promise schedules the continuation, allowing
  // the read/write result to complete before the native handle is reused.
  co_await cardio::resolved();
  const auto handle = std::exchange(endpoint->handle, kInvalidMuonSocket);
  if (!endpoint->listener) {
#if defined(_WIN32)
    (void)::shutdown(handle, SD_SEND);
#else
    (void)::shutdown(handle, SHUT_WR);
#endif
  }
  CloseNativeSocket(handle);
  service->endpoints.erase(handle);
  endpoint->closed = true;
  auto waiters = std::move(endpoint->closed_waiters);
  for (const auto& signal : waiters) signal->resolve();
}

static std::shared_ptr<MuonServerSocket> MakeSocket(
    const std::shared_ptr<SocketServiceState>& service,
    const std::shared_ptr<SocketEndpoint>& endpoint) {
  auto socket = std::make_shared<MuonServerSocket>();
  socket->read = [service, endpoint](std::span<uint8_t> buffer) {
    return ReadSocket(service, endpoint, buffer);
  };
  socket->write = [service, endpoint](std::span<const uint8_t> buffer) {
    return WriteSocket(service, endpoint, buffer);
  };
  socket->close = [service, endpoint] { return CloseSocket(service, endpoint); };
  return socket;
}

static cardio::promise<std::shared_ptr<MuonServerSocket>> AcceptSocket(
    std::shared_ptr<SocketServiceState> service, std::shared_ptr<SocketEndpoint> listener) {
  CheckSocketDispatcher(service);
  auto operation = SocketOperation(listener, false);
  while (true) {
    CheckSocketOpen(listener);
    auto endpoint = std::make_shared<SocketEndpoint>();
#if defined(_WIN32)
    endpoint->handle = ::accept(listener->handle, nullptr, nullptr);
#else
    endpoint->handle = ::accept4(listener->handle, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
    const auto handle = endpoint->handle;
    if (handle != kInvalidMuonSocket) {
#if defined(_WIN32)
      if (WSAEventSelect(handle, service->event, FD_READ | FD_WRITE | FD_CLOSE) != 0)
        throw SocketError(WSAGetLastError(), "Watch accepted server socket");
#endif
      auto socket = MakeSocket(service, endpoint);
      service->endpoints.emplace(handle, endpoint);
      co_return socket;
    }
    const auto error = SocketErrorCode();
    if (SocketInterrupted(error)) continue;
    if (!SocketWouldBlock(error)) throw SocketError(error, "Accept server socket");
    co_await WaitSocketReady(service, listener, false);
  }
}

static std::shared_ptr<MuonServerListener> ListenSocket(
    const std::shared_ptr<SocketServiceState>& service, uint16_t port) {
  CheckSocketDispatcher(service);
  if (service->stopping) throw std::runtime_error("Server socket service is stopped");
#if defined(_WIN32)
  if (service->pump_failure) std::rethrow_exception(service->pump_failure);
#endif
  auto endpoint = std::make_shared<SocketEndpoint>();
  endpoint->listener = true;
#if defined(_WIN32)
  endpoint->handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
  endpoint->handle = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
  if (endpoint->handle == kInvalidMuonSocket) throw SocketError(SocketErrorCode(), "Create server socket");
  const auto enabled = int{1};
#if defined(_WIN32)
  const auto option = SO_EXCLUSIVEADDRUSE;
#else
  const auto option = SO_REUSEADDR;
#endif
  if (setsockopt(endpoint->handle, SOL_SOCKET, option,
      reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
    throw SocketError(SocketErrorCode(), "Configure server socket binding");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::bind(endpoint->handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
    throw SocketError(SocketErrorCode(), "Bind server socket");
  if (::listen(endpoint->handle, SOMAXCONN) != 0)
    throw SocketError(SocketErrorCode(), "Listen on server socket");
#if defined(_WIN32)
  auto length = int{sizeof(address)};
  if (WSAEventSelect(endpoint->handle, service->event, FD_ACCEPT | FD_CLOSE) != 0)
    throw SocketError(WSAGetLastError(), "Watch server listener");
#else
  auto length = socklen_t{sizeof(address)};
#endif
  if (getsockname(endpoint->handle, reinterpret_cast<sockaddr*>(&address), &length) != 0)
    throw SocketError(SocketErrorCode(), "Get server socket address");
  auto listener = std::make_shared<MuonServerListener>();
  listener->port = ntohs(address.sin_port);
  listener->accept = [service, endpoint] { return AcceptSocket(service, endpoint); };
  listener->close = [service, endpoint] { return CloseSocket(service, endpoint); };
  service->endpoints.emplace(endpoint->handle, endpoint);
  return listener;
}

static cardio::promise<void> StopSockets(std::shared_ptr<SocketServiceState> service) {
  CheckSocketDispatcher(service);
  if (service->stopped) co_return;
  if (service->stopping) {
    auto signal = std::make_shared<cardio::promise_source<void>>();
    auto done = signal->get_promise();
    service->stopped_waiters.push_back(signal);
    co_await done;
    co_return;
  }
  service->stopping = true;
  auto endpoints = std::vector<std::shared_ptr<SocketEndpoint>>{};
  endpoints.reserve(service->endpoints.size());
  for (const auto& [handle, endpoint] : service->endpoints) {
    (void)handle;
    endpoints.push_back(endpoint);
  }
  auto pending = std::vector<cardio::promise<void>>{};
  pending.reserve(endpoints.size());
  for (const auto& endpoint : endpoints) pending.push_back(CloseSocket(service, endpoint));
  for (auto& close : pending) co_await close;
#if defined(_WIN32)
  // Wake the native wait normally before releasing its shared HANDLE. This
  // also joins an auto dispatcher's native wait snapshot, rather than relying
  // on cancellation removing the registration while that snapshot is in use.
  if (service->event != WSA_INVALID_EVENT) WSASetEvent(service->event);
  if (service->pump) { co_await *service->pump; service->pump.reset(); }
  if (service->event != WSA_INVALID_EVENT) {
    WSACloseEvent(service->event);
    service->event = WSA_INVALID_EVENT;
  }
#endif
  service->stopped = true;
  auto waiters = std::move(service->stopped_waiters);
  for (const auto& signal : waiters) signal->resolve();
}

std::shared_ptr<MuonServerSockets> CreateMuonServerSockets() {
  auto service = std::make_shared<SocketServiceState>();
  service->dispatcher = &cardio::get_current_dispatcher();
#if defined(_WIN32)
  WSADATA data{};
  const auto error = WSAStartup(MAKEWORD(2, 2), &data);
  if (error != 0) throw SocketError(error, "Initialize server sockets");
  service->winsock_started = true;
  service->event = WSACreateEvent();
  if (service->event == WSA_INVALID_EVENT)
    throw SocketError(WSAGetLastError(), "Create server socket event");
  service->pump.emplace(PumpSocketEvents(service.get()));
#endif
  auto sockets = std::make_shared<MuonServerSockets>();
  sockets->listen = [service](uint16_t port) { return ListenSocket(service, port); };
  sockets->stop = [service] { return StopSockets(service); };
  return sockets;
}
