/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_process_lock.h"
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <system_error>

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(errno, std::generic_category(), operation);
}

struct Descriptor {
  int value = -1;
  ~Descriptor() { if (value >= 0) close(value); }
};

struct ProcessLock {
  std::shared_ptr<cardio::primitives::mutex> local;
  cardio::primitives::lock_handle local_lock;
  Descriptor file;
};

static std::shared_ptr<cardio::primitives::mutex> LocalLock(const std::string& path) {
  static std::mutex gate;
  static std::map<std::string, std::weak_ptr<cardio::primitives::mutex>> locks;
  const auto guard = std::lock_guard(gate);
  for (auto iterator = locks.begin(); iterator != locks.end();) {
    if (iterator->second.expired()) iterator = locks.erase(iterator);
    else ++iterator;
  }
  auto value = locks[path].lock();
  if (!value) locks[path] = value = std::make_shared<cardio::primitives::mutex>();
  return value;
}

cardio::promise<std::shared_ptr<void>> AcquireMuonServiceProcessLock(
    std::string key, cardio::cancellation cancellation) {
  const auto path = "/tmp/muon-service-lock-" + std::to_string(geteuid()) + "-" +
      MuonServiceProcessLockHash(key);
  auto lock = std::make_shared<ProcessLock>();
  lock->local = LocalLock(path);
  // POSIX record locks belong to the process: a second open/close of this inode
  // would release the first lock. Serialize before opening, including cleanup.
  lock->local_lock = std::move(co_await lock->local->lock(cancellation));
  lock->file.value = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  Check(lock->file.value >= 0, "open service process lock");
  struct stat status{};
  Check(fstat(lock->file.value, &status) == 0, "inspect service process lock");
  if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0077))
    throw std::runtime_error("Service process lock must be a private, user-owned regular file");
  Descriptor changes{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
  Check(changes.value >= 0, "observe service process lock");
  Check(inotify_add_watch(changes.value, path.c_str(), IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF) >= 0,
        "watch service process lock");
  struct flock record{};
  record.l_type = F_WRLCK;
  record.l_whence = SEEK_SET;
  record.l_len = 1;
  for (;;) {
    if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
    if (fcntl(lock->file.value, F_SETLK, &record) == 0) co_return lock;
    if (errno != EACCES && errno != EAGAIN) Check(false, "acquire service process lock");
    // Install the watch before testing ownership. Linux removes POSIX locks in
    // filp_close before emitting IN_CLOSE_WRITE, including process termination.
    // flock/OFD locks are removed later and cannot use this wakeup ordering.
    co_await cardio::from_fd(changes.value, cardio::fd_event::read, cancellation);
    alignas(inotify_event) char events[4096];
    const auto size = read(changes.value, events, sizeof(events));
    if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
    Check(size > 0, "read service process lock changes");
    for (size_t offset = 0; offset < static_cast<size_t>(size);) {
      const auto* event = reinterpret_cast<const inotify_event*>(events + offset);
      if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
        throw std::runtime_error("Service process lock identity changed while waiting");
      offset += sizeof(inotify_event) + event->len;
    }
  }
}
