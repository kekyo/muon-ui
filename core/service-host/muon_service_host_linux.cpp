/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_service_protocol.h"
#include "muon_service_policy.h"
#include "muon_service_session_monitor.h"
#include "muon_service_startup.h"
#include "muon_service_plugins.h"
#include "muon_node_service_host.h"
#include "muon_cardio_post.h"
#include "services/muon_quickjs_service_plugins.h"
#include "services/muon_native_service_host.h"
#include "services/muon_native_service_library.h"
#include "services/muon_service_broker.h"
#include "services/muon_quickjs_service_host.h"
#include "services/muon_service_execution_router.h"
#include "services/muon_service_limits.h"

#include <cardio.h>
#include <yyjson.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#include <array>
#include <span>
#include <cerrno>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
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

static cardio::promise<std::string> ReadTextFile(std::filesystem::path path,
                                               cardio::cancellation cancellation) {
  auto file = std::unique_ptr<GFile, decltype(&g_object_unref)>(
      g_file_new_for_path(path.c_str()), g_object_unref);
  const auto raw = co_await cardio::gio::submit<GFileInputStream*>(
      [file = file.get()](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
        g_file_read_async(file, G_PRIORITY_DEFAULT, cancel, callback, data);
      }, [](GObject* source, GAsyncResult* result, GError** error) {
        return g_file_read_finish(G_FILE(source), result, error);
      }, cancellation);
  auto input = std::unique_ptr<GFileInputStream, decltype(&g_object_unref)>(raw, g_object_unref);
  std::string contents;
  auto failure = std::exception_ptr{};
  try {
    auto bytes = std::array<std::byte, 65536>{};
    for (;;) {
      const auto count = co_await cardio::gio::read(G_INPUT_STREAM(input.get()), std::span(bytes), cancellation);
      if (!count) break;
      if (count > 8 * 1024 * 1024 - contents.size())
        throw std::invalid_argument("Service file exceeds 8 MiB");
      contents.append(reinterpret_cast<const char*>(bytes.data()), count);
    }
  } catch (...) { failure = std::current_exception(); }
  co_await cardio::gio::close(G_INPUT_STREAM(input.get()));
  if (failure) std::rethrow_exception(failure);
  co_return contents;
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
                 std::string endpoint, std::string data_root, bool bootstrap, bool development)
      : group_(group), dispatcher_(dispatcher), endpoint_(std::move(endpoint)),
        data_root_(std::move(data_root)), bootstrap_open_(bootstrap), development_(development) {}

  ~ServiceProcess() {
    if (close_monitor_) close_monitor_();
    protocol_.reset();
    broker_.reset();
    controller_.reset();
    node_.reset();
    quickjs_.reset();
    plugin_startup_.reset();
    plugins_.reset();
    native_.reset();
    if (listener_ >= 0) close(listener_);
    if (signal_fd_ >= 0) close(signal_fd_);
    // Only the process holding the singleton lock may remove this endpoint.
    if (owns_endpoint_) unlink(endpoint_.c_str());
    if (lock_ >= 0) close(lock_);
  }

  cardio::promise<void> Run(std::string config, sigset_t signals, std::string startup, muon_service_start_reason resident_reason) {
    try {
      co_await MakeDirectory(std::filesystem::path(endpoint_).parent_path());
      co_await MakeDirectory(data_root_);
      LockEndpoint();
      co_await MakeDirectory(std::filesystem::path(data_root_) / ".autostart");
      auto contents = std::string(R"({"services":{}})");
      if (!config.empty()) contents = co_await ReadTextFile(config, {});
      auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
          yyjson_read(contents.data(), contents.size(), 0), yyjson_doc_free);
      if (!document) throw std::invalid_argument("Invalid service registry JSON");
      auto* services = yyjson_obj_get(yyjson_doc_get_root(document.get()), "services");
      if (!yyjson_is_obj(services)) throw std::invalid_argument("Missing service registry");
      std::vector<MuonServiceDefinition> definitions;
      size_t index, count;
      yyjson_val *key, *value;
      yyjson_obj_foreach(services, index, count, key, value) {
        const auto id = StringValue(key);
        if (!ValidId(id) || specs_.contains(id) || scripts_.contains(id) || nodes_.contains(id))
          throw std::invalid_argument("Invalid or duplicate service identity: " + id);
        const auto runtime_value = yyjson_obj_get(value, "runtime");
        const auto runtime = runtime_value ? StringValue(runtime_value) : "native";
        if (runtime != "native" && runtime != "quickjs" && runtime != "node")
          throw std::invalid_argument("Unsupported service runtime: " + runtime);
        const auto source_key = runtime == "native" ? "library" : runtime == "node" ? "project" : "script";
        const auto library = (std::filesystem::path(config).parent_path() /
            StringValue(yyjson_obj_get(value, source_key))).lexically_normal().string();
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
        const auto preference = co_await LoadMuonServiceAutostart(std::filesystem::path(data_root_) / ".autostart" / id);
        definitions.push_back({id, yyjson_get_bool(autostart), preference});
        if (yyjson_arr_size(yyjson_obj_get(value, "plugins"))) plugin_services_.insert(id);
        const auto resolve_node_path = [&](const char* name) { return (std::filesystem::path(config).parent_path() / StringValue(yyjson_obj_get(value, name))).lexically_normal().string(); };
        if (runtime == "node") nodes_.emplace(id, MuonNodeServiceSpec{
            id, library, StringValue(yyjson_obj_get(value, "entry")),
            resolve_node_path("executable"), resolve_node_path("bridge"), spec.data_directory, std::move(spec.config)});
        else if (runtime == "native") specs_.emplace(id, std::move(spec));
        else scripts_.emplace(id, MuonQuickJsServiceSpec{
            id, spec.library_path, {}, spec.data_directory, std::move(spec.config)});
      }
      login_ = std::move(co_await CreateMuonServiceLoginPolicy(data_root_, definitions, std::move(startup)));
      const auto post = [this](std::function<void()> task) { Post(std::move(task)); };
      plugins_ = CreateMuonDesktopServicePlugins(yyjson_doc_get_root(document.get()),
          std::filesystem::path(config).parent_path(), dispatcher_, [this](std::function<void()> task) {
            muon_internal::FireAndForgetOnDispatcher(&dispatcher_, [this, task = std::move(task)] {
              task();
              MaybeFinish();
            });
          });
      auto quickjs_platform = MuonQuickJsServicePlatform{
          post,
          [this](std::chrono::milliseconds delay, std::function<void()> task) {
            auto cancellation = std::make_shared<cardio::cancellation_source>();
            ++runtime_jobs_;
            cardio::fire_and_forget(Timer(delay, cancellation, std::move(task)));
            return std::function<void()>([cancellation] { cancellation->cancel(); });
          },
          [](const std::string& id, muon_log_level, const std::string& message) {
            std::cerr << id << ": " << message << '\n';
          },
          [this](const std::string& id, uint64_t generation, const std::string& reason) {
            controller_->RuntimeExited(id, generation, reason);
          }, {64 * 1024 * 1024, 1024 * 1024, std::chrono::seconds(2)}, std::chrono::seconds(10),
          [this](const MuonQuickJsServiceSpec& spec, uint64_t generation, MuonQuickJsEngine& engine,
              std::function<void()> wake, std::function<void(const std::string&)> fault) {
            return plugin_startup_ ? plugin_startup_->Attach(spec.id, generation, engine, std::move(wake), std::move(fault))
                                   : MuonQuickJsServiceExtension{};
          }};
      native_ = std::make_unique<MuonNativeServiceHost>(MuonNativeServicePlatform{
          LoadMuonNativeServiceFactory, post,
          [](const std::string& id, muon_log_level, const std::string& message) {
            std::cerr << id << ": " << message << '\n';
          }, quickjs_platform.delay});
      if (plugins_) plugin_startup_ = std::make_unique<MuonQuickJsServicePlugins>(*plugins_, quickjs_platform.delay);
      node_ = std::make_unique<MuonNodeServiceHost>(MuonNodeServicePlatform{
          post, quickjs_platform.delay, quickjs_platform.log, quickjs_platform.exited, std::chrono::seconds(10)});
      quickjs_ = std::make_unique<MuonQuickJsServiceHost>(std::move(quickjs_platform));
      controller_ = std::make_unique<MuonServiceController>(definitions, MuonServiceBackend{
          [this](const MuonServiceSnapshot& state, auto complete) {
            if (nodes_.contains(state.id)) node_->Start(MuonServiceSpecForGeneration(nodes_.at(state.id), state), state.generation, std::move(complete));
            else if (scripts_.contains(state.id))
              cardio::fire_and_forget(StartScript(state, std::move(complete)));
            else native_->Start(MuonServiceSpecForGeneration(specs_.at(state.id), state), state.generation, std::move(complete));
          },
          [this](const MuonServiceSnapshot& state, auto complete) {
            if (nodes_.contains(state.id)) node_->Stop(state.id, state.generation, std::move(complete), state.stop);
            else if (scripts_.contains(state.id)) quickjs_->Stop(state.id, state.generation, std::move(complete), state.stop);
            else native_->Stop(state.id, state.generation, std::move(complete), state.stop);
          },
          [this](const std::string& id, bool enabled, auto complete) {
            cardio::fire_and_forget(SavePolicy(id, enabled, std::move(complete)));
          },
          [this](const MuonServiceEvent& event) {
            protocol_->Notify(event);
            if (event.kind == MuonServiceEventKind::Changed)
              std::cout << SerializeMuonServiceSnapshot(event.snapshot) << std::endl;
          }, post}, resident_reason);
      broker_ = std::make_unique<MuonServiceBroker>(*controller_, RouteMuonServiceExecutions(
          {native_->Execution(), quickjs_->Execution(), node_->Execution()},
          [this](const std::string& id) { return nodes_.contains(id) ? size_t{2} : scripts_.contains(id) ? size_t{1} : size_t{0}; }));
      protocol_ = std::make_unique<MuonServiceProtocol>(*controller_,
          [this](const std::string& owner, const std::string& json) { Send(owner, json); },
          MuonServiceUiPolicy{[this] { return login_->UiEnabled(); },
            [this](bool enabled, auto complete) {
              cardio::fire_and_forget(SavePolicy("", enabled, std::move(complete)));
            }, [this](std::string startup, auto complete) {
              cardio::fire_and_forget(ConfigureStartup(std::move(startup), std::move(complete)));
            }}, MuonServiceCommunication{
            [this](const auto& reference, const auto& action, const auto& payload, auto reply) {
              broker_->Request(reference, action, payload, std::move(reply));
            }, [this](const auto& reference) { broker_->Release(reference); },
            [this](const auto& owner) { broker_->CloseOwner(owner); },
            [this] { broker_->Shutdown(); }});
      OpenListener();
      signal_fd_ = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
      Check(signal_fd_ >= 0, "signalfd");
      io_count_ += 2;
      cardio::fire_and_forget(Accept());
      cardio::fire_and_forget(WatchSignals());
      close_monitor_ = WatchMuonServiceSession([this] {
        Post([this] { controller_->Shutdown("Desktop session ending", {MUON_SERVICE_STOP_SESSION_ENDING}); });
      });
      controller_->Activate();
      initialized_ = true;
      std::cout << "{\"ready\":true}" << std::endl;
      if (bootstrap_open_) {
        ++io_count_;
        cardio::fire_and_forget(WatchBootstrap());
      }
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
  cardio::promise<void> Timer(std::chrono::milliseconds duration,
      std::shared_ptr<cardio::cancellation_source> cancellation, std::function<void()> task) {
    try {
      co_await cardio::promises::delay(static_cast<uint64_t>(duration.count()), cancellation->get_cancellation());
    } catch (const cardio::canceled_exception&) {}
    // Even a zero timer or inline cancellation resumes on a later owner turn.
    // Keep the process alive until the coroutine and its callback have drained.
    Post([this, cancellation, task = std::move(task)] {
      if (!cancellation->get_cancellation().is_cancellation_requested()) task();
      --runtime_jobs_;
      MaybeFinish();
    });
  }

  cardio::promise<void> StartScript(MuonServiceSnapshot state,
      std::function<void(const std::string&)> complete) {
    ++runtime_jobs_;
    auto spec = MuonServiceSpecForGeneration(scripts_.at(state.id), state);
    spec.startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto timeout = cardio::cancellations::timeout(10000);
    std::string error;
    try {
      spec.source = co_await ReadTextFile(spec.filename, timeout.get_cancellation());
      timeout.cancel();
      if (plugin_services_.contains(state.id)) plugin_startup_->Start(*quickjs_, spec, state.generation, complete);
      else quickjs_->Start(spec, state.generation, complete);
    } catch (const std::exception& failure) { error = failure.what(); }
    timeout.cancel();
    if (!error.empty()) complete(error);
    --runtime_jobs_;
    MaybeFinish();
  }

  cardio::promise<void> ConfigureStartup(std::string startup,
      std::function<void(const std::string&)> complete) {
    ++policy_jobs_;
    std::string error;
    try { co_await ConfigureMuonServiceLoginPolicy(*login_, std::move(startup)); }
    catch (const std::exception& failure) { error = failure.what(); }
    complete(error);
    --policy_jobs_;
    MaybeFinish();
  }

  cardio::promise<void> SavePolicy(std::string id, bool enabled,
      std::function<void(const std::string&)> complete) {
    ++policy_jobs_;
    auto error = std::string{};
    try { co_await login_->Set(id, enabled); }
    catch (const std::exception& failure) { error = failure.what(); }
    complete(error);
    --policy_jobs_;
    MaybeFinish();
  }

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
    if (json.size() > kMuonServiceMaxFrameBytes ||
        peer->queued_bytes + json.size() + 1 > kMuonServiceMaxQueuedBytes) {
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
        if (input.size() > kMuonServiceMaxFrameBytes) ClosePeer(peer);
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
        Send(peer->owner, "{\"event\":\"ready\"}");
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
      controller_->Shutdown("Host received a termination signal", {MUON_SERVICE_STOP_HOST,
          signal.ssi_signo == SIGTERM ? "SIGTERM" : signal.ssi_signo == SIGINT ? "SIGINT" : "SIGHUP"});
    } catch (const cardio::canceled_exception&) {
    } catch (const std::exception& error) {
      exit_code_ = 1;
      controller_->Shutdown(error.what());
    }
    --io_count_;
    MaybeFinish();
  }

  cardio::promise<void> WatchBootstrap() {
    bool updated = false;
    try {
      // Normal UI bootstraps carry no payload. A development supervisor sends
      // 'd' after its last UI exits, releasing the startup guard without giving
      // up ownership. EOF then stops even resident services after parent death.
      Check(fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == 0, "set bootstrap flags");
      for (;;) {
        co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read,
                                transport_cancellation_.get_cancellation());
        char byte;
        const auto count = read(STDIN_FILENO, &byte, 1);
        if (count == 0) break;
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        Check(count > 0, "read bootstrap");
        if (development_) {
          if (byte == 'u') { updated = true; continue; }
          if (byte != 'd') throw std::invalid_argument("Invalid development session command");
          bootstrap_open_ = false;
          ever_connected_ = true;
          MaybeFinish();
        }
      }
    } catch (const cardio::canceled_exception&) {
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
    }
    bootstrap_open_ = false;
    ever_connected_ = true;
    if (development_) controller_->Shutdown("Development session ended",
        {updated ? MUON_SERVICE_STOP_DEVELOPMENT_UPDATE : MUON_SERVICE_STOP_DEVELOPMENT_EXIT});
    --io_count_;
    MaybeFinish();
  }

  void MaybeFinish() {
    if (!initialized_ || finishing_ || policy_jobs_ != 0) return;
    if (!controller_->IsShuttingDown()) {
      if (ever_connected_ && !bootstrap_open_ && peers_.empty() &&
          controller_->IsQuiescent() && native_->IsIdle() && quickjs_->IsIdle() && node_->IsIdle() && (!plugins_ || plugins_->IsIdle()) &&
          (!plugin_startup_ || plugin_startup_->IsIdle()) && runtime_jobs_ == 0)
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
    if (controller_->IsQuiescent() && native_->IsIdle() && quickjs_->IsIdle() && node_->IsIdle() && (!plugins_ || plugins_->IsIdle()) &&
          (!plugin_startup_ || plugin_startup_->IsIdle()) && runtime_jobs_ == 0 && posted_ == 0) {
      std::vector<std::shared_ptr<Peer>> peers;
      for (const auto& [owner, peer] : peers_) {
        (void)owner;
        peers.push_back(peer);
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
  std::map<std::string, MuonQuickJsServiceSpec> scripts_;
  std::map<std::string, MuonNodeServiceSpec> nodes_;
  std::unique_ptr<MuonNodeServiceHost> node_;
  std::unique_ptr<MuonQuickJsServiceHost> quickjs_;
  std::unique_ptr<MuonServicePluginPool> plugins_;
  std::unique_ptr<MuonQuickJsServicePlugins> plugin_startup_;
  std::set<std::string> plugin_services_;
  size_t runtime_jobs_ = 0;
  std::unique_ptr<MuonServiceLoginPolicy> login_;
  size_t policy_jobs_ = 0;
  std::unique_ptr<MuonNativeServiceHost> native_;
  std::unique_ptr<MuonServiceController> controller_;
  std::unique_ptr<MuonServiceBroker> broker_;
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
  std::function<void()> close_monitor_;
  bool owns_endpoint_ = false;
  bool initialized_ = false;
  bool ever_connected_ = false;
  bool transport_stopping_ = false;
  bool finishing_ = false;
  bool bootstrap_open_ = false;
  bool development_ = false;
};

int main(int argc, char** argv) {
  try {
    std::map<std::string, std::string> arguments;
    for (int index = 1; index + 1 < argc; index += 2)
      arguments.emplace(argv[index], argv[index + 1]);
    const auto bootstrap = arguments.contains("--bootstrap");
    const auto mode = bootstrap ? arguments.at("--bootstrap") : "";
    const auto development = mode == "development" || mode == "development-update";
    const auto resident_reason = mode == "login" ? MUON_SERVICE_START_LOGIN_AUTOSTART :
        mode == "development-update" ? MUON_SERVICE_START_DEVELOPMENT_UPDATE : MUON_SERVICE_START_APP;
    const auto count = 3 + static_cast<int>(bootstrap) + static_cast<int>(arguments.contains("--startup"));
    if (argc != count * 2 + 1 || arguments.size() != static_cast<size_t>(count) ||
        (bootstrap && !development && mode != "stdin" && mode != "login") || !arguments.contains("--config") ||
        !arguments.contains("--endpoint") || !arguments.contains("--data-root"))
      throw std::invalid_argument("Usage: muon-service-host --config FILE --endpoint SOCKET --data-root DIR");
    for (const auto& [key, path] : arguments) {
      if (key != "--bootstrap" && key != "--startup" && !(key == "--config" && path.empty()) &&
          !std::filesystem::path(path).is_absolute())
        throw std::invalid_argument(key + " must be an absolute path");
    }
    // Block before GIO or a native service can create supporting threads.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    Check(sigprocmask(SIG_BLOCK, &signals, nullptr) == 0, "block shutdown signals");
    // Readiness stdout may close with the UI while resident services keep
    // running. A later diagnostic/state write must never terminate the host.
    struct sigaction broken_pipe{};
    broken_pipe.sa_handler = SIG_IGN;
    sigemptyset(&broken_pipe.sa_mask);
    Check(sigaction(SIGPIPE, &broken_pipe, nullptr) == 0, "ignore broken output pipe");
    umask(0077);
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    ServiceProcess process(group, dispatcher, arguments.at("--endpoint"), arguments.at("--data-root"), bootstrap, development);
    cardio::fire_and_forget(process.Run(arguments.at("--config"), signals,
        arguments.contains("--startup") ? arguments.at("--startup") : "", resident_reason));
    dispatcher.park();
    return process.ExitCode();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
