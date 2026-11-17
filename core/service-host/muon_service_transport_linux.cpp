/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_transport.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <system_error>

struct MuonServiceSocket {
  int fd = -1;
  bool closed = false;
  cardio::cancellation_source cancellation;

  ~MuonServiceSocket() { if (fd >= 0) close(fd); }
};

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(errno, std::generic_category(), operation);
}

static void Close(const std::shared_ptr<MuonServiceSocket>& socket) {
  if (socket->closed) return;
  socket->closed = true;
  shutdown(socket->fd, SHUT_RDWR);
  socket->cancellation.cancel();
}

static cardio::promise<std::string> Read(std::shared_ptr<MuonServiceSocket> socket) {
  const auto cancellation = socket->cancellation.get_cancellation();
  while (!socket->closed) {
    char bytes[8192];
    const auto count = recv(socket->fd, bytes, sizeof(bytes), 0);
    if (count >= 0) co_return std::string(bytes, static_cast<size_t>(count));
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await cardio::from_fd(socket->fd, cardio::fd_event::read, cancellation);
      continue;
    }
    Check(false, "read service socket");
  }
  throw cardio::canceled_exception();
}

static cardio::promise<void> Write(std::shared_ptr<MuonServiceSocket> socket, std::string frame) {
  const auto cancellation = socket->cancellation.get_cancellation();
  size_t offset = 0;
  while (offset < frame.size()) {
    if (socket->closed) throw cardio::canceled_exception();
    const auto count = send(socket->fd, frame.data() + offset, frame.size() - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await cardio::from_fd(socket->fd, cardio::fd_event::write, cancellation);
      continue;
    }
    Check(count > 0, "write service socket");
    offset += static_cast<size_t>(count);
  }
}

cardio::promise<MuonServiceTransport> ConnectMuonServiceTransport(
    std::string endpoint, cardio::cancellation cancellation) {
  static cardio::primitives::mutex connection_mutex;
  auto lock = std::move(co_await connection_mutex.lock(cancellation));
  if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  sockaddr_un address{};
  if (endpoint.empty() || endpoint.front() != '/' || endpoint.size() >= sizeof(address.sun_path))
    throw std::invalid_argument("Invalid absolute service socket path");
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  auto socket = std::make_shared<MuonServiceSocket>();
  socket->fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  Check(socket->fd >= 0, "create service socket");
  auto registration = cancellation.on_cancellation_requested([weak = std::weak_ptr(socket)] {
    if (auto socket = weak.lock()) Close(socket);
  });
  if (connect(socket->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    if (errno != EINPROGRESS) Check(false, "connect service socket");
    co_await cardio::from_fd(socket->fd, cardio::fd_event::write,
                            socket->cancellation.get_cancellation());
    int result = 0;
    socklen_t size = sizeof(result);
    Check(getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &result, &size) == 0, "read socket error");
    if (result) throw std::system_error(result, std::generic_category(), "connect service socket");
  }
  ucred peer{};
  socklen_t size = sizeof(peer);
  Check(getsockopt(socket->fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0, "authenticate service socket");
  if (size != sizeof(peer) || peer.uid != geteuid())
    throw std::runtime_error("Service host belongs to another user");
  std::string greeting;
  while (greeting.find('\n') == std::string::npos && greeting.size() < 128) {
    const auto chunk = co_await Read(socket);
    if (chunk.empty()) throw std::runtime_error("Service host closed before its ready handshake");
    greeting += chunk;
  }
  if (greeting != "{\"event\":\"ready\"}\n")
    throw std::runtime_error("Invalid service host ready handshake");
  if (socket->closed || cancellation.is_cancellation_requested())
    throw cardio::canceled_exception();
  co_return MuonServiceTransport{
      [socket] { return Read(socket); },
      [socket](std::string frame) { return Write(socket, std::move(frame)); },
      [socket] { Close(socket); }};
}
