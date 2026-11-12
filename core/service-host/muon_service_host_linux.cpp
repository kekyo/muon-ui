/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_service_protocol.h"
#include "services/muon_native_service_host.h"
#include "services/muon_native_service_library.h"

#include <cardio.h>
#include <yyjson.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>

static void Check(bool success, const char* operation) {
  if (!success) throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

static std::string StringValue(yyjson_val* value) {
  if (!yyjson_is_str(value)) throw std::invalid_argument("Expected a service string value");
  const auto result = std::string(yyjson_get_str(value), yyjson_get_len(value));
  if (result.find('\0') != std::string::npos)
    throw std::invalid_argument("Service strings cannot contain NUL");
  return result;
}

static bool ValidId(const std::string& id) {
  return !id.empty() && id.size() <= 64 &&
      ((id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z')) &&
      id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") ==
          std::string::npos;
}

static cardio::promise<void> MakeDirectory(std::filesystem::path path) {
  if (path.empty() || path == path.root_path()) co_return;
  co_await MakeDirectory(path.parent_path());
  auto file = std::unique_ptr<GFile, decltype(&g_object_unref)>(
      g_file_new_for_path(path.c_str()), g_object_unref);
  try {
    co_await cardio::gio::make_directory(file.get());
  } catch (const cardio::gio::gio_error& error) {
    if (error.domain() != G_IO_ERROR || error.code() != G_IO_ERROR_EXISTS) throw;
  }
}

struct Peer {
  int fd;
  std::string owner;
  cardio::cancellation_source cancellation;
  std::deque<std::string> output;
  size_t queued_bytes = 0;
  bool writing = false;
  bool closed = false;

  ~Peer() { close(fd); }
};

// This process owns no CEF state. The dispatcher is shared with native services,
// so all factory callbacks and transport lifecycle changes have one owner.
class ServiceProcess final {
 public:
  ServiceProcess(cardio::dispatcher_group_glib& group,
                 cardio::dispatcher_host_glib& dispatcher,
                 std::string endpoint, std::string data_root)
      : group_(group), dispatcher_(dispatcher), endpoint_(std::move(endpoint)),
        data_root_(std::move(data_root)) {}

  ~ServiceProcess() {
    protocol_.reset();
    controller_.reset();
    native_.reset();
    if (listener_ >= 0) close(listener_);
    if (signal_fd_ >= 0) close(signal_fd_);
    // Only the process holding the singleton lock may remove this endpoint.
    if (owns_endpoint_) unlink(endpoint_.c_str());
    if (lock_ >= 0) close(lock_);
  }

  cardio::promise<void> Run(std::string config, sigset_t signals) {
    try {
      co_await MakeDirectory(std::filesystem::path(endpoint_).parent_path());
      co_await MakeDirectory(data_root_);
      LockEndpoint();
      auto file = std::unique_ptr<GFile, decltype(&g_object_unref)>(
          g_file_new_for_path(config.c_str()), g_object_unref);
      const auto contents = co_await cardio::gio::load_contents(file.get());
      auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
          yyjson_read(reinterpret_cast<const char*>(contents.bytes.data()),
                      contents.bytes.size(), 0), yyjson_doc_free);
      if (!document) throw std::invalid_argument("Invalid service registry JSON");
      auto* services = yyjson_obj_get(yyjson_doc_get_root(document.get()), "services");
      if (!yyjson_is_obj(services)) throw std::invalid_argument("Missing service registry");
      std::vector<MuonServiceDefinition> definitions;
      size_t index, count;
      yyjson_val *key, *value;
      yyjson_obj_foreach(services, index, count, key, value) {
        const auto id = StringValue(key);
        if (!ValidId(id) || specs_.contains(id))
          throw std::invalid_argument("Invalid or duplicate service identity: " + id);
        const auto library = (std::filesystem::path(config).parent_path() /
            StringValue(yyjson_obj_get(value, "library"))).lexically_normal().string();
        auto* autostart = yyjson_obj_get(value, "autostart");
        if (autostart && !yyjson_is_bool(autostart))
          throw std::invalid_argument("Service autostart must be boolean");
        const auto directory = (std::filesystem::path(data_root_) / id).string();
        auto spec = MuonNativeServiceSpec{id, library, directory, {}};
        auto* settings = yyjson_obj_get(value, "config");
        if (settings) {
          if (!yyjson_is_obj(settings)) throw std::invalid_argument("Invalid service config");
          size_t setting_index, setting_count;
          yyjson_val *setting_key, *setting_value;
          yyjson_obj_foreach(settings, setting_index, setting_count, setting_key, setting_value) {
            spec.config.emplace_back(StringValue(setting_key), StringValue(setting_value));
          }
        }
        co_await MakeDirectory(directory);
        definitions.push_back({id, yyjson_get_bool(autostart), std::nullopt});
        specs_.emplace(id, std::move(spec));
      }
      const auto post = [this](std::function<void()> task) { Post(std::move(task)); };
      native_ = std::make_unique<MuonNativeServiceHost>(MuonNativeServicePlatform{
          LoadMuonNativeServiceFactory, post,
          [](const std::string& id, muon_log_level, const std::string& message) {
            std::cerr << id << ": " << message << '\n';
          }});
      controller_ = std::make_unique<MuonServiceController>(definitions, MuonServiceBackend{
          [this](const MuonServiceSnapshot& state, auto complete) {
            native_->Start(specs_.at(state.id), state.generation, std::move(complete));
          },
          [this](const MuonServiceSnapshot& state, auto complete) {
            native_->Stop(state.id, state.generation, std::move(complete));
          },
          [](const std::string&, bool, auto complete) {
            complete("Service policy persistence is not available");
          },
          [this](const MuonServiceEvent& event) {
            protocol_->Notify(event);
            if (event.kind == MuonServiceEventKind::Changed)
              std::cout << SerializeMuonServiceSnapshot(event.snapshot) << std::endl;
          }, post});
      protocol_ = std::make_unique<MuonServiceProtocol>(*controller_,
          [this](const std::string& owner, const std::string& json) { Send(owner, json); });
      OpenListener();
      signal_fd_ = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
      Check(signal_fd_ >= 0, "signalfd");
      io_count_ += 2;
      cardio::fire_and_forget(Accept());
      cardio::fire_and_forget(WatchSignals());
      controller_->Activate();
      initialized_ = true;
      std::cout << "{\"ready\":true}" << std::endl;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      exit_code_ = 1;
      if (controller_) {
        initialized_ = true;
        controller_->Shutdown(error.what());
        MaybeFinish();
      } else {
        group_.shutdown();
      }
    }
  }

  int ExitCode() const { return exit_code_; }

 private:
  void LockEndpoint() {
    struct stat parent{};
    const auto directory = std::filesystem::path(endpoint_).parent_path().string();
    Check(stat(directory.c_str(), &parent) == 0, "stat endpoint directory");
    if (parent.st_uid != geteuid() || (parent.st_mode & 0077) != 0)
      throw std::runtime_error("Service endpoint directory must be private to its user");
    lock_ = open((endpoint_ + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    Check(lock_ >= 0, "open service lock");
    Check(flock(lock_, LOCK_EX | LOCK_NB) == 0, "lock service endpoint");
    owns_endpoint_ = true;
  }

  void OpenListener() {
    sockaddr_un address{};
    if (endpoint_.size() >= sizeof(address.sun_path))
      throw std::invalid_argument("Service endpoint path is too long");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint_.c_str(), endpoint_.size() + 1);
    listener_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    Check(listener_ >= 0, "socket");
    if (unlink(endpoint_.c_str()) != 0 && errno != ENOENT) Check(false, "unlink stale endpoint");
    Check(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
    Check(chmod(endpoint_.c_str(), 0600) == 0, "chmod");
    Check(listen(listener_, 32) == 0, "listen");
  }

  cardio::promise<void> Posted(cardio::promise<void> ready, std::function<void()> task) {
    co_await ready;
    task();
    --posted_;
    MaybeFinish();
  }

  void Post(std::function<void()> task) {
    ++posted_;
    auto ready = cardio::promise_source<void>();
    cardio::fire_and_forget(Posted(ready.get_promise(), std::move(task)));
    // All callers already run on dispatcher_. Removing the current dispatcher
    // only while resolving prevents a native completion from running inline.
    cardio::set_current_dispatcher(nullptr);
    ready.resolve();
    cardio::set_current_dispatcher(&dispatcher_);
  }

  void ClosePeer(const std::shared_ptr<Peer>& peer) {
    if (peer->closed) return;
    peer->closed = true;
    peers_.erase(peer->owner);
    protocol_->CloseOwner(peer->owner);
    peer->output.clear();
    peer->queued_bytes = 0;
    shutdown(peer->fd, SHUT_RDWR);
    peer->cancellation.cancel();
  }

  void Send(const std::string& owner, const std::string& json) {
    const auto found = peers_.find(owner);
    if (found == peers_.end()) return;
    auto peer = found->second;
    if (peer->queued_bytes + json.size() + 1 > 4 * 1024 * 1024) {
      ClosePeer(peer);
      return;
    }
    peer->output.push_back(json + '\n');
    peer->queued_bytes += json.size() + 1;
    if (!peer->writing) {
      peer->writing = true;
      ++io_count_;
      cardio::fire_and_forget(Write(peer));
    }
  }

  cardio::promise<void> Write(std::shared_ptr<Peer> peer) {
    try {
      while (!peer->closed && !peer->output.empty()) {
        const auto output = std::move(peer->output.front());
        peer->output.pop_front();
        peer->queued_bytes -= output.size();
        size_t offset = 0;
        while (offset < output.size()) {
          const auto written = send(peer->fd, output.data() + offset, output.size() - offset,
                                    MSG_NOSIGNAL);
          if (written < 0 && errno == EINTR) continue;
          if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            co_await cardio::from_fd(peer->fd, cardio::fd_event::write,
                                    peer->cancellation.get_cancellation());
            continue;
          }
          Check(written > 0, "write service response");
          offset += static_cast<size_t>(written);
        }
      }
    } catch (const std::exception&) {
      ClosePeer(peer);
    }
    peer->writing = false;
    --io_count_;
    MaybeFinish();
  }

  cardio::promise<void> Read(std::shared_ptr<Peer> peer) {
    std::string input;
    try {
      while (!peer->closed) {
        char bytes[8192];
        const auto size = recv(peer->fd, bytes, sizeof(bytes), 0);
        if (size == 0) break;
        if (size < 0 && errno == EINTR) continue;
        if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          co_await cardio::from_fd(peer->fd, cardio::fd_event::read,
                                  peer->cancellation.get_cancellation());
          continue;
        }
        Check(size > 0, "read service request");
        input.append(bytes, static_cast<size_t>(size));
        size_t end;
        while (!peer->closed && (end = input.find('\n')) != std::string::npos) {
          const auto valid = protocol_->Receive(peer->owner, input.substr(0, end));
          input.erase(0, end + 1);
          if (!valid) ClosePeer(peer);
        }
        if (input.size() > 1024 * 1024) ClosePeer(peer);
        MaybeFinish();
      }
    } catch (const std::exception&) {
      // EOF, malformed input, and I/O failures all close the same native owner.
    }
    ClosePeer(peer);
    --io_count_;
    MaybeFinish();
  }

  cardio::promise<void> Accept() {
    try {
      while (!transport_stopping_) {
        co_await cardio::from_fd(listener_, cardio::fd_event::read,
                                transport_cancellation_.get_cancellation());
        const auto fd = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        Check(fd >= 0, "accept");
        ucred credentials{};
        socklen_t length = sizeof(credentials);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
            credentials.uid != geteuid()) {
          close(fd);
          continue;
        }
        auto peer = std::make_shared<Peer>();
        peer->fd = fd;
        peer->owner = std::to_string(++owner_sequence_);
        protocol_->OpenOwner(peer->owner);
        peers_.emplace(peer->owner, peer);
        ever_connected_ = true;
        ++io_count_;
        cardio::fire_and_forget(Read(peer));
      }
    } catch (const cardio::canceled_exception&) {
    } catch (const std::exception& error) {
      exit_code_ = 1;
      controller_->Shutdown(error.what());
    }
    --io_count_;
    MaybeFinish();
  }

  cardio::promise<void> WatchSignals() {
    try {
      co_await cardio::from_fd(signal_fd_, cardio::fd_event::read,
                              transport_cancellation_.get_cancellation());
      signalfd_siginfo signal{};
      Check(read(signal_fd_, &signal, sizeof(signal)) == sizeof(signal), "read signal");
      controller_->Shutdown("User session or application ended");
    } catch (const cardio::canceled_exception&) {
    } catch (const std::exception& error) {
      exit_code_ = 1;
      controller_->Shutdown(error.what());
    }
    --io_count_;
    MaybeFinish();
  }

  void MaybeFinish() {
    if (!initialized_ || finishing_) return;
    if (!controller_->IsShuttingDown()) {
      if (ever_connected_ && peers_.empty() && controller_->IsQuiescent() && native_->IsIdle())
        controller_->Shutdown("No service references remain");
      else return;
    }
    // Cancellation may resume suspended coroutines inline; the guard keeps
    // transport disposal from recursively walking the connection map.
    finishing_ = true;
    if (!transport_stopping_) {
      transport_stopping_ = true;
      transport_cancellation_.cancel();
    }
    if (controller_->IsQuiescent() && native_->IsIdle() && posted_ == 0) {
      std::vector<std::shared_ptr<Peer>> peers;
      for (const auto& [owner, peer] : peers_) {
        (void)owner;
        if (!peer->writing) peers.push_back(peer);
      }
      for (const auto& peer : peers) ClosePeer(peer);
      if (io_count_ == 0 && posted_ == 0) group_.shutdown();
    }
    finishing_ = false;
  }

  cardio::dispatcher_group_glib& group_;
  cardio::dispatcher_host_glib& dispatcher_;
  std::string endpoint_;
  std::string data_root_;
  std::map<std::string, MuonNativeServiceSpec> specs_;
  std::unique_ptr<MuonNativeServiceHost> native_;
  std::unique_ptr<MuonServiceController> controller_;
  std::unique_ptr<MuonServiceProtocol> protocol_;
  std::map<std::string, std::shared_ptr<Peer>> peers_;
  cardio::cancellation_source transport_cancellation_;
  uint64_t owner_sequence_ = 0;
  size_t posted_ = 0;
  size_t io_count_ = 0;
  int listener_ = -1;
  int signal_fd_ = -1;
  int lock_ = -1;
  int exit_code_ = 0;
  bool owns_endpoint_ = false;
  bool initialized_ = false;
  bool ever_connected_ = false;
  bool transport_stopping_ = false;
  bool finishing_ = false;
};

int main(int argc, char** argv) {
  try {
    std::map<std::string, std::string> arguments;
    for (int index = 1; index + 1 < argc; index += 2)
      arguments.emplace(argv[index], argv[index + 1]);
    if (argc != 7 || arguments.size() != 3 || !arguments.contains("--config") ||
        !arguments.contains("--endpoint") || !arguments.contains("--data-root"))
      throw std::invalid_argument("Usage: muon-service-host --config FILE --endpoint SOCKET --data-root DIR");
    for (const auto& [key, path] : arguments) {
      if (!std::filesystem::path(path).is_absolute())
        throw std::invalid_argument(key + " must be an absolute path");
    }
    // Block before GIO or a native service can create supporting threads.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    Check(sigprocmask(SIG_BLOCK, &signals, nullptr) == 0, "block shutdown signals");
    umask(0077);
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    ServiceProcess process(group, dispatcher, arguments.at("--endpoint"), arguments.at("--data-root"));
    cardio::fire_and_forget(process.Run(arguments.at("--config"), signals));
    dispatcher.park();
    return process.ExitCode();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
