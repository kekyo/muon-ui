/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_android_service_transport.h"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <memory>
#include <system_error>

struct MuonAndroidServiceSocket {
  int descriptor = -1;
  bool closed = false;
  cardio::cancellation_source cancellation;
  ~MuonAndroidServiceSocket() { if (descriptor >= 0) ::close(descriptor); }
};

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(errno, std::generic_category(), operation);
}

static void Close(const std::shared_ptr<MuonAndroidServiceSocket>& socket) {
  if (socket->closed) return;
  socket->closed = true;
  ::shutdown(socket->descriptor, SHUT_RDWR);
  socket->cancellation.cancel();
}

static cardio::promise<std::string> Read(std::shared_ptr<MuonAndroidServiceSocket> socket) {
  while (!socket->closed) {
    char data[16384];
    const auto count = ::recv(socket->descriptor, data, sizeof(data), 0);
    if (count >= 0) co_return std::string(data, static_cast<size_t>(count));
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await cardio::from_fd(socket->descriptor, cardio::fd_event::read,
          socket->cancellation.get_cancellation());
      continue;
    }
    Check(false, "Read Android service stream");
  }
  throw cardio::canceled_exception();
}

static cardio::promise<void> Write(std::shared_ptr<MuonAndroidServiceSocket> socket, std::string value) {
  size_t offset = 0;
  while (offset < value.size()) {
    if (socket->closed) throw cardio::canceled_exception();
    const auto count = ::send(socket->descriptor, value.data() + offset, value.size() - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      co_await cardio::from_fd(socket->descriptor, cardio::fd_event::write,
          socket->cancellation.get_cancellation());
      continue;
    }
    Check(count > 0, "Write Android service stream");
    offset += static_cast<size_t>(count);
  }
}

MuonServiceTransport AdoptMuonAndroidServiceTransport(int descriptor) {
  struct OwnedDescriptor {
    int value;
    ~OwnedDescriptor() { if (value >= 0) ::close(value); }
  } owned{descriptor};
  const auto socket = std::make_shared<MuonAndroidServiceSocket>();
  socket->descriptor = descriptor;
  owned.value = -1;
  const auto status = ::fcntl(descriptor, F_GETFL);
  Check(status >= 0 && ::fcntl(descriptor, F_SETFL, status | O_NONBLOCK) == 0,
      "Configure Android service stream");
  const auto flags = ::fcntl(descriptor, F_GETFD);
  Check(flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0,
      "Protect Android service descriptor");
  return {
      [socket] { return Read(socket); },
      [socket](std::string value) { return Write(socket, std::move(value)); },
      [socket] { Close(socket); }};
}
