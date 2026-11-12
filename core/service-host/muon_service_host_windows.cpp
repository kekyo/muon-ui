/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_service_protocol.h"
#include "muon_filesystem.h"
#include "services/muon_native_service_host.h"
#include "services/muon_native_service_library.h"

#include <cardio.h>
#include <yyjson.h>
#include <windows.h>
#include <sddl.h>

#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(static_cast<int>(GetLastError()),
                                        std::system_category(), operation);
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};

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

static void MakeDirectory(const std::filesystem::path& path) {
  // Win32 directory creation has no OVERLAPPED form. Reuse the platform path
  // helper for this bounded metadata operation; file contents remain async.
  auto error = std::error_code{};
  if (!muon_internal::CreateFilesystemDirectories(path, error))
    throw std::system_error(error, "create service data directory");
}

static cardio::promise<std::string> ReadConfig(std::filesystem::path path) {
  const auto io_path = muon_internal::FilesystemPathForIo(
      path, muon_internal::FilesystemIoUsage::File);
  Handle file{CreateFileW(io_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
  Check(file.value != INVALID_HANDLE_VALUE, "open service registry");
  LARGE_INTEGER length{};
  Check(GetFileSizeEx(file.value, &length) != 0, "size service registry");
  if (length.QuadPart < 0 || length.QuadPart > 8 * 1024 * 1024)
    throw std::invalid_argument("Service registry exceeds 8 MiB");
  auto result = std::string(static_cast<size_t>(length.QuadPart), '\0');
  size_t offset = 0;
  while (offset < result.size()) {
    const auto size = co_await cardio::win32::read(file.value,
        std::as_writable_bytes(std::span(result.data() + offset, result.size() - offset)),
        static_cast<uint64_t>(offset));
    if (size == 0) throw std::runtime_error("Truncated service registry");
    offset += size;
  }
  co_return result;
}

static std::shared_ptr<void> LogonSecurity() {
  Handle token;
  Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != 0,
        "open process token");
  DWORD length = 0;
  GetTokenInformation(token.value, TokenGroups, nullptr, 0, &length);
  Check(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size token groups");
  auto bytes = std::vector<unsigned char>(length);
  Check(GetTokenInformation(token.value, TokenGroups, bytes.data(), length, &length) != 0,
        "read token groups");
  const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
  for (DWORD index = 0; index < groups->GroupCount; ++index) {
    const auto& group = groups->Groups[index];
    if ((group.Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID) continue;
    LPWSTR sid = nullptr;
    Check(ConvertSidToStringSidW(group.Sid, &sid) != 0, "format logon SID");
    const auto sid_owner = std::shared_ptr<void>(sid, [](void* value) { LocalFree(value); });
    const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        &descriptor, nullptr) != 0, "create logon security descriptor");
    return {descriptor, [](void* value) { LocalFree(value); }};
  }
  throw std::runtime_error("Service host requires an interactive logon SID");
}

struct Peer {
  std::shared_ptr<Handle> pipe;
  std::string owner;
  cardio::cancellation_source cancellation;
  std::deque<std::string> output;
  size_t queued_bytes = 0;
  bool writing = false;
  bool closed = false;
};

// One owner dispatcher serves every logical native instance and pipe. It owns
// no CEF state and remains alive after every page process disconnects.
class ServiceProcess final {
 public:
  ServiceProcess(cardio::dispatcher_group& group,
                 cardio::dispatcher_host_win32_auto& dispatcher,
                 std::wstring endpoint, std::filesystem::path data_root)
      : group_(group), dispatcher_(dispatcher), endpoint_(std::move(endpoint)),
        data_root_(std::move(data_root)) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"MuonServiceSessionHost";
    Check(RegisterClassW(&window_class) != 0, "register service session window");
    // A message-only window does not receive session-ending broadcasts.
    window_ = CreateWindowExW(0, window_class.lpszClassName, endpoint_.c_str(), 0,
        0, 0, 0, 0, nullptr, nullptr, window_class.hInstance, this);
    Check(window_ != nullptr, "create service session window");
  }

  ~ServiceProcess() {
    protocol_.reset();
    controller_.reset();
    native_.reset();
    listener_.reset();
    if (window_) DestroyWindow(window_);
  }

  cardio::promise<void> Run(std::filesystem::path config) {
    // Native factories may finish using only our posted window messages. Keep
    // the dispatcher alive even when no cardio I/O remains during that drain.
    cardio::fire_and_forget(KeepAlive());
    try {
      security_ = LogonSecurity();
      // Acquire the singleton before loading factories or activating roots.
      listener_ = NewPipe(true);
      MakeDirectory(data_root_);
      const auto contents = co_await ReadConfig(config);
      if (session_ending_) { FinishLoop(); co_return; }
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
        if (!ValidId(id) || specs_.contains(id))
          throw std::invalid_argument("Invalid or duplicate service identity: " + id);
        const auto library = muon_internal::FilesystemPathToUtf8(
            muon_internal::ResolveFilesystemPath(config.parent_path(),
                muon_internal::FilesystemPathFromUtf8(StringValue(yyjson_obj_get(value, "library")))));
        auto* autostart = yyjson_obj_get(value, "autostart");
        if (autostart && !yyjson_is_bool(autostart))
          throw std::invalid_argument("Service autostart must be boolean");
        const auto directory = data_root_ / id;
        auto spec = MuonNativeServiceSpec{id, library,
            muon_internal::FilesystemPathToUtf8(directory), {}};
        auto* settings = yyjson_obj_get(value, "config");
        if (settings) {
          if (!yyjson_is_obj(settings)) throw std::invalid_argument("Invalid service config");
          size_t setting_index, setting_count;
          yyjson_val *setting_key, *setting_value;
          yyjson_obj_foreach(settings, setting_index, setting_count, setting_key, setting_value)
            spec.config.emplace_back(StringValue(setting_key), StringValue(setting_value));
        }
        MakeDirectory(directory);
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
      ++io_count_;
      cardio::fire_and_forget(Accept());
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
      } else FinishLoop();
    }
  }

  int ExitCode() const { return exit_code_; }

 private:
  static constexpr UINT kPostedTask = WM_APP + 1;

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<ServiceProcess*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_QUERYENDSESSION) return TRUE;
    if (message == WM_ENDSESSION && wparam) {
      self->session_ending_ = true;
      if (self->controller_) self->controller_->Shutdown("User session ended");
      self->MaybeFinish();
      // Returning from WM_ENDSESSION permits immediate OS termination. Keep
      // pumping completion messages while native stops drain; Windows may still
      // terminate an unresponsive application under its shutdown policy.
      self->dispatcher_.park();
      return 0;
    }
    if (message == kPostedTask) {
      if (self->posted_.empty()) return 0;
      auto* previous = &cardio::get_current_dispatcher();
      cardio::set_current_dispatcher(&self->dispatcher_);
      auto task = std::move(self->posted_.front());
      self->posted_.pop_front();
      task();
      self->MaybeFinish();
      cardio::set_current_dispatcher(previous);
      return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }

  void Post(std::function<void()> task) {
    posted_.push_back(std::move(task));
    Check(PostMessageW(window_, kPostedTask, 0, 0) != 0, "post service task");
  }

  cardio::promise<void> KeepAlive() { co_await lifetime_.get_promise(); }

  void FinishLoop() {
    if (loop_finished_) return;
    loop_finished_ = true;
    lifetime_.resolve();
    group_.shutdown();
  }

  std::shared_ptr<Handle> NewPipe(bool first) const {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security_.get(), FALSE};
    const auto handle = CreateNamedPipeW(endpoint_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &attributes);
    Check(handle != INVALID_HANDLE_VALUE, "create service pipe");
    return std::make_shared<Handle>(handle);
  }

  void ClosePeer(const std::shared_ptr<Peer>& peer) {
    if (peer->closed) return;
    peer->closed = true;
    peers_.erase(peer->owner);
    protocol_->CloseOwner(peer->owner);
    peer->output.clear();
    peer->queued_bytes = 0;
    // read/write coroutines retain Peer and HANDLE until cardio has observed
    // native completion. Cancellation alone never authorizes CloseHandle.
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
          const auto size = co_await cardio::win32::write(peer->pipe->value,
              std::as_bytes(std::span(output.data() + offset, output.size() - offset)),
              peer->cancellation.get_cancellation());
          if (size == 0) throw std::runtime_error("Empty service response write");
          offset += size;
        }
      }
    } catch (const std::exception&) { ClosePeer(peer); }
    peer->writing = false;
    --io_count_;
    MaybeFinish();
  }

  cardio::promise<void> Read(std::shared_ptr<Peer> peer) {
    std::string input;
    try {
      while (!peer->closed) {
        char bytes[8192];
        const auto size = co_await cardio::win32::read(peer->pipe->value,
            std::as_writable_bytes(std::span(bytes)), peer->cancellation.get_cancellation());
        if (size == 0) break;
        input.append(bytes, size);
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
      // Broken pipe, malformed input, and I/O cancellation close one owner.
    }
    ClosePeer(peer);
    --io_count_;
    MaybeFinish();
  }

  cardio::promise<void> Accept() {
    try {
      while (!transport_stopping_) {
        auto pipe = listener_;
        bool connected = false;
        try {
          co_await cardio::win32::submit(pipe->value,
              [&connected](HANDLE handle, OVERLAPPED& overlapped) {
                if (ConnectNamedPipe(handle, &overlapped)) return DWORD{ERROR_SUCCESS};
                const auto error = GetLastError();
                connected = error == ERROR_PIPE_CONNECTED;
                // ERROR_PIPE_CONNECTED has no submitted operation. Let the
                // helper reject it, then accept the already connected peer.
                return error;
              }, transport_cancellation_.get_cancellation());
        } catch (const std::system_error&) { if (!connected) throw; }
        if (transport_stopping_) break;
        // Always retain another instance before dispatching this connection.
        // The pipe name cannot disappear between clients and lose singleton ownership.
        listener_ = NewPipe(false);
        auto peer = std::make_shared<Peer>();
        peer->pipe = std::move(pipe);
        peer->owner = std::to_string(++owner_sequence_);
        protocol_->OpenOwner(peer->owner);
        peers_.emplace(peer->owner, peer);
        ever_connected_ = true;
        ++io_count_;
        cardio::fire_and_forget(Read(peer));
        Send(peer->owner, "{\"event\":\"ready\"}");
      }
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
    finishing_ = true;
    if (!transport_stopping_) {
      transport_stopping_ = true;
      transport_cancellation_.cancel();
    }
    if (controller_->IsQuiescent() && native_->IsIdle() && posted_.empty()) {
      std::vector<std::shared_ptr<Peer>> peers;
      for (const auto& [owner, peer] : peers_) {
        (void)owner;
        // Explicit exit must not depend on a page reading its queued replies.
        // Shutdown acknowledgements are best effort; native stop is the barrier.
        peers.push_back(peer);
      }
      for (const auto& peer : peers) ClosePeer(peer);
      if (io_count_ == 0 && posted_.empty()) FinishLoop();
    }
    finishing_ = false;
  }

  cardio::dispatcher_group& group_;
  cardio::dispatcher_host_win32_auto& dispatcher_;
  std::wstring endpoint_;
  std::filesystem::path data_root_;
  std::shared_ptr<void> security_;
  std::shared_ptr<Handle> listener_;
  std::map<std::string, MuonNativeServiceSpec> specs_;
  std::unique_ptr<MuonNativeServiceHost> native_;
  std::unique_ptr<MuonServiceController> controller_;
  std::unique_ptr<MuonServiceProtocol> protocol_;
  std::map<std::string, std::shared_ptr<Peer>> peers_;
  std::deque<std::function<void()>> posted_;
  cardio::cancellation_source transport_cancellation_;
  cardio::promise_source<void> lifetime_;
  HWND window_ = nullptr;
  uint64_t owner_sequence_ = 0;
  size_t io_count_ = 0;
  int exit_code_ = 0;
  bool initialized_ = false;
  bool ever_connected_ = false;
  bool transport_stopping_ = false;
  bool finishing_ = false;
  bool session_ending_ = false;
  bool loop_finished_ = false;
};

int wmain(int argc, wchar_t** argv) {
  try {
    std::map<std::wstring, std::wstring> arguments;
    for (int index = 1; index + 1 < argc; index += 2)
      arguments.emplace(argv[index], argv[index + 1]);
    if (argc != 7 || arguments.size() != 3 || !arguments.contains(L"--config") ||
        !arguments.contains(L"--endpoint") || !arguments.contains(L"--data-root"))
      throw std::invalid_argument("Usage: muon-service-host --config FILE --endpoint PIPE --data-root DIR");
    const auto& endpoint = arguments.at(L"--endpoint");
    constexpr auto prefix = L"\\\\.\\pipe\\muon-service-";
    if (!endpoint.starts_with(prefix) || endpoint.size() <= std::wcslen(prefix) ||
        endpoint.size() > 256 || endpoint.find(L'\\', std::wcslen(prefix)) != std::wstring::npos)
      throw std::invalid_argument("Service endpoint must be a local muon-service named pipe");
    for (const auto* key : {L"--config", L"--data-root"}) {
      if (!std::filesystem::path(arguments.at(key)).is_absolute())
        throw std::invalid_argument("Service file paths must be absolute");
    }
    cardio::dispatcher_group group;
    cardio::dispatcher_host_win32_auto dispatcher(group);
    ServiceProcess process(group, dispatcher, endpoint, arguments.at(L"--data-root"));
    cardio::fire_and_forget(process.Run(arguments.at(L"--config")));
    dispatcher.park();
    return process.ExitCode();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
