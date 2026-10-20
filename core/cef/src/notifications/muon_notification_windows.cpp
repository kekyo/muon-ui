/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_windows.h"
#include "notifications/muon_notification_winrt_abi.h"
#include "notifications/muon_notification_windows_async.h"
#include "muon_cardio_post.h"
#include <windows.h>
#include <roapi.h>
#include <shobjidl.h>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#if defined(MUON_NOTIFICATION_TEST)
#include <latch>
#include <thread>
#endif

namespace muon_internal {

static void CheckToastResult(HRESULT result, const char* operation) {
  if (SUCCEEDED(result)) return;
  auto message = std::ostringstream{};
  message << operation << " failed (HRESULT 0x" << std::hex
          << static_cast<unsigned long>(result) << ')';
  throw std::runtime_error(message.str());
}

static GUID ToastGuid(const wchar_t* value) {
  GUID result{};
  CheckToastResult(CLSIDFromString(value, &result), "CLSIDFromString");
  return result;
}

static std::wstring ToastWide(const std::string& value) {
  if (value.empty()) return {};
  const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (count == 0) throw std::runtime_error("Invalid notification UTF-8");
  std::wstring result(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), result.data(), count) != count) {
    throw std::runtime_error("Notification UTF-8 conversion failed");
  }
  return result;
}

static std::string ToastUtf8(const wchar_t* value) {
  if (!value || !value[0]) return {};
  const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      value, -1, nullptr, 0, nullptr, nullptr);
  if (count == 0) throw std::runtime_error("Invalid notification UTF-16");
  std::string result(static_cast<size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
      result.data(), count, nullptr, nullptr) != count) {
    throw std::runtime_error("Notification UTF-16 conversion failed");
  }
  result.pop_back();
  return result;
}

struct ToastString {
  HSTRING value = nullptr;
  explicit ToastString(const std::wstring& text) {
    CheckToastResult(WindowsCreateString(text.data(),
        static_cast<UINT32>(text.size()), &value), "WindowsCreateString");
  }
  ~ToastString() { WindowsDeleteString(value); }
  ToastString(const ToastString&) = delete;
  ToastString& operator=(const ToastString&) = delete;
};

template <typename Interface>
struct ToastCom {
  Interface* value = nullptr;
  ~ToastCom() { if (value) value->Release(); }
  ToastCom() = default;
  ToastCom(const ToastCom&) = delete;
  ToastCom& operator=(const ToastCom&) = delete;
};

template <typename Interface>
static void QueryToastInterface(IUnknown* source, const wchar_t* iid,
                                ToastCom<Interface>& target) {
  CheckToastResult(source->QueryInterface(ToastGuid(iid),
      reinterpret_cast<void**>(&target.value)), "Notification QueryInterface");
}

template <typename Interface>
static void ActivateToastFactory(const wchar_t* name, const wchar_t* iid,
                                 ToastCom<Interface>& target) {
  const auto class_name = ToastString(name);
  CheckToastResult(RoGetActivationFactory(class_name.value, ToastGuid(iid),
      reinterpret_cast<void**>(&target.value)), "RoGetActivationFactory");
}

static std::string EscapeToastXml(const std::string& value) {
  std::string result;
  for (const auto c : value) {
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += c; break;
    }
  }
  return result;
}

static std::string CompactToastToken(const std::string& token) {
  // Preserve every UUID bit in the tag/group pair, rather than truncating a hash.
  // Both fields stay within the original Windows 10 limit of 16 characters.
  std::string result;
  if (token.size() != 36) throw std::runtime_error("Invalid notification revision token");
  for (size_t i = 0; i < token.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (token[i] != '-') throw std::runtime_error("Invalid notification revision token");
    } else if ((token[i] >= '0' && token[i] <= '9') ||
               (token[i] >= 'a' && token[i] <= 'f')) {
      result += token[i];
    } else {
      throw std::runtime_error("Invalid notification revision token");
    }
  }
  return result;
}

struct WindowsToastState {
  MuonWindowsNotificationOptions options;
  std::wstring wide_app_id;
  cardio::dispatcher* dispatcher = nullptr;
  MuonNotificationActivation activation;
  ToastCom<notification_abi::ToastNotifier> notifier;
  DWORD registration = 0;
  std::mutex activation_mutex;
  bool stopped = false;
  bool initialized = false;
  bool identity_registered = false;
  std::string initialization_error;
#if defined(MUON_NOTIFICATION_TEST)
  std::function<void()> before_activation_post;
#endif
  void ReleaseApartmentResources() {
    if (registration) CoRevokeClassObject(registration);
    registration = 0;
    if (notifier.value) {
      notifier.value->Release();
      notifier.value = nullptr;
    }
    if (initialized) RoUninitialize();
    initialized = false;
  }
};

#if defined(MUON_NOTIFICATION_TEST)
static std::weak_ptr<WindowsToastState> test_backend_state;
#endif

class ToastActivationCallback final : public notification_abi::NotificationActivation {
 public:
  explicit ToastActivationCallback(std::weak_ptr<WindowsToastState> state)
      : state_(std::move(state)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (iid == IID_IUnknown ||
        iid == ToastGuid(L"{53E31837-6600-4A81-9395-75CFFE746F94}")) {
      *value = static_cast<notification_abi::NotificationActivation*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE Activate(LPCWSTR app_id, LPCWSTR arguments,
      const notification_abi::NotificationUserInput*, ULONG) override {
    try {
      const auto state = state_.lock();
      if (!state || !app_id || state->wide_app_id != app_id) return S_OK;
      const auto payload = ToastUtf8(arguments);
      if (payload.size() < 38 || payload[36] != '|') return S_OK;
      const auto token = payload.substr(0, 36);
      CompactToastToken(token);
      auto action = std::optional<std::string>{};
      if (payload.substr(37) == "d") {
        action.reset();
      } else if (payload.size() > 39 && payload.substr(37, 2) == "a|") {
        action = payload.substr(39);
      } else return S_OK;
#if defined(MUON_NOTIFICATION_TEST)
      if (state->before_activation_post) state->before_activation_post();
#endif
      // Revoking the class factory does not wait for retained COM callbacks.
      // Close and submission share this lock so Stop cannot return while a
      // callback is still about to use the owner dispatcher's raw pointer.
      const auto lock = std::lock_guard(state->activation_mutex);
      if (state->stopped) return S_OK;
      FireAndForgetOnDispatcher(state->dispatcher,
          [state, token, action = std::move(action)] {
            if (!state->stopped && state->activation) state->activation(token, action);
          });
      return S_OK;
    } catch (...) {
      return E_INVALIDARG;
    }
  }
 private:
  std::atomic<ULONG> references_{1};
  std::weak_ptr<WindowsToastState> state_;
};

class ToastClassFactory final : public IClassFactory {
 public:
  explicit ToastClassFactory(std::weak_ptr<WindowsToastState> state)
      : state_(std::move(state)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (iid == IID_IUnknown || iid == IID_IClassFactory) {
      *value = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
                                            void** value) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* callback = new (std::nothrow) ToastActivationCallback(state_);
    if (!callback) return E_OUTOFMEMORY;
    const auto result = callback->QueryInterface(iid, value);
    callback->Release();
    return result;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
 private:
  std::atomic<ULONG> references_{1};
  std::weak_ptr<WindowsToastState> state_;
};

static void RequireToastRunning(const WindowsToastState& state) {
  if (state.stopped) throw std::runtime_error("Notification backend is stopped");
}

struct ToastRegistryKey {
  HKEY value = nullptr;
  ~ToastRegistryKey() { if (value) RegCloseKey(value); }
};

static void SetToastRegistryString(HKEY key, const wchar_t* name,
                                  const std::wstring& value, DWORD type) {
  CheckToastResult(HRESULT_FROM_WIN32(RegSetValueExW(key, name, 0, type,
      reinterpret_cast<const BYTE*>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))),
      "Register Windows notification identity");
}

static void CheckToastRegistryString(HKEY key, const wchar_t* name,
                                    const std::wstring& expected) {
  DWORD size = 0;
  CheckToastResult(HRESULT_FROM_WIN32(RegGetValueW(key, nullptr, name,
      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr, &size)),
      "Read Windows notification registration");
  if (size > 65536 || size < sizeof(wchar_t))
    throw std::runtime_error("Invalid Windows notification registration");
  auto value = std::wstring(size / sizeof(wchar_t) + 1, L'\0');
  CheckToastResult(HRESULT_FROM_WIN32(RegGetValueW(key, nullptr, name,
      RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, value.data(), &size)),
      "Read Windows notification registration");
  value.resize(std::wcslen(value.c_str()));
  if (value != expected) throw std::runtime_error("Windows notification registration does not match this application");
}

static cardio::promise<void> EnsureToastIdentity(std::shared_ptr<WindowsToastState> state) {
  RequireToastRunning(*state);
  if (!state->initialization_error.empty())
    throw std::runtime_error(state->initialization_error);
  const auto first = !state->identity_registered;
  const auto options = state->options;
  co_await RunMuonWindowsNotificationTask([options, first] {
    const auto app = ToastWide(options.app_id);
    const auto name = ToastWide(options.display_name);
    const auto icon = ToastWide(options.icon_path);
    const auto clsid = ToastWide(options.activator_clsid);
    const auto attributes = GetFileAttributesW(icon.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
      throw std::runtime_error("Windows notification icon is unavailable");
    const auto path = L"Software\\Classes\\AppUserModelId\\" + app;
    ToastRegistryKey key;
    // Explicitly select the shared 64-bit registration view, including when a
    // 32-bit app runs on 64-bit Windows. App IDs themselves include architecture.
    if (first) {
      CheckToastResult(HRESULT_FROM_WIN32(RegCreateKeyExW(HKEY_CURRENT_USER,
          path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
          KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key.value, nullptr)),
          "Create Windows notification registration");
      SetToastRegistryString(key.value, L"DisplayName", name, REG_EXPAND_SZ);
      SetToastRegistryString(key.value, L"IconUri", icon, REG_EXPAND_SZ);
      SetToastRegistryString(key.value, L"CustomActivator", clsid, REG_SZ);
    } else {
      CheckToastResult(HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_CURRENT_USER,
          path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.value)),
          "Open Windows notification registration");
    }
    CheckToastRegistryString(key.value, L"DisplayName", name);
    CheckToastRegistryString(key.value, L"IconUri", icon);
    CheckToastRegistryString(key.value, L"CustomActivator", clsid);
  });
  RequireToastRunning(*state);
  state->identity_registered = true;
}

static cardio::promise<MuonNotificationBackendCapabilities> ReadToastCapabilities(
    std::shared_ptr<WindowsToastState> state) {
  try {
    co_await EnsureToastIdentity(state);
    co_return MuonNotificationBackendCapabilities{true, true, std::nullopt,
        "post-exit-persistence-unverified", true};
  } catch (const std::exception& error) {
    co_return MuonNotificationBackendCapabilities{false, false, false,
        std::string("windows-notification-registration-unavailable: ") + error.what(), true};
  }
}

static cardio::promise<MuonNotificationBackendStatus> ReadToastStatus(
    std::shared_ptr<WindowsToastState> state) {
  RequireToastRunning(*state);
  try { co_await EnsureToastIdentity(state); }
  catch (const std::exception& error) {
    co_return MuonNotificationBackendStatus{std::nullopt,
        std::string("windows-notification-registration-unavailable: ") + error.what()};
  }
  INT32 setting = 0;
  const auto result = state->notifier.value->get_Setting(&setting);
  if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
    co_return MuonNotificationBackendStatus{std::nullopt, "windows-notification-setting-unavailable"};
  }
  CheckToastResult(result, "ToastNotifier.Setting");
  switch (setting) {
    case 0: co_return MuonNotificationBackendStatus{true, {}};
    case 1: co_return MuonNotificationBackendStatus{false, "disabled-for-application"};
    case 2: co_return MuonNotificationBackendStatus{false, "disabled-for-user"};
    case 3: co_return MuonNotificationBackendStatus{false, "disabled-by-group-policy"};
    case 4: co_return MuonNotificationBackendStatus{false, "disabled-by-manifest"};
    default: co_return MuonNotificationBackendStatus{std::nullopt, "unknown-windows-notification-setting"};
  }
}

static cardio::promise<MuonNotificationHandle> PostWindowsToast(
    std::shared_ptr<WindowsToastState> state, MuonNotificationPost post,
    std::string token) {
  RequireToastRunning(*state);
  co_await EnsureToastIdentity(state);
  const auto status = co_await ReadToastStatus(state);
  if (status.enabled == false) throw std::runtime_error(status.reason);
  const auto compact_token = CompactToastToken(token);
  auto xml = "<toast launch=\"" + token + "|d\"><visual><binding template=\"ToastGeneric\">";
  xml += "<text>" + EscapeToastXml(post.title) + "</text><text>" +
         EscapeToastXml(post.body) + "</text></binding></visual>";
  if (!post.actions.empty()) {
    xml += "<actions>";
    for (const auto& action : post.actions) {
      xml += "<action content=\"" + EscapeToastXml(action.title) +
             "\" arguments=\"" + token + "|a|" + EscapeToastXml(action.id) +
             "\" activationType=\"foreground\"/>";
    }
    xml += "</actions>";
  }
  xml += "</toast>";
  ToastCom<IInspectable> document;
  const auto document_class = ToastString(L"Windows.Data.Xml.Dom.XmlDocument");
  CheckToastResult(RoActivateInstance(document_class.value, &document.value), "Activate XmlDocument");
  ToastCom<notification_abi::XmlDocumentIo> io;
  QueryToastInterface(document.value, L"{6CD0E74E-EE65-4489-9EBF-CA43E87BA637}", io);
  const auto xml_string = ToastString(ToastWide(xml));
  CheckToastResult(io.value->LoadXml(xml_string.value), "XmlDocument.LoadXml");
  ToastCom<IInspectable> content;
  QueryToastInterface(document.value, L"{F7F3A506-1E87-42D6-BCFB-B8C809FA5494}", content);
  ToastCom<notification_abi::ToastFactory> factory;
  ActivateToastFactory(L"Windows.UI.Notifications.ToastNotification",
      L"{04124B20-82C6-4229-B109-FD9ED4662B53}", factory);
  ToastCom<IInspectable> toast;
  CheckToastResult(factory.value->CreateToastNotification(content.value, &toast.value), "CreateToastNotification");
  ToastCom<notification_abi::ToastIdentity> identity;
  QueryToastInterface(toast.value, L"{9DFB9FD1-143A-490E-90BF-B9FBA7132DE7}", identity);
  const auto tag = ToastString(ToastWide(compact_token.substr(0, 16)));
  const auto group = ToastString(ToastWide(compact_token.substr(16)));
  CheckToastResult(identity.value->put_Tag(tag.value), "ToastNotification.Tag");
  CheckToastResult(identity.value->put_Group(group.value), "ToastNotification.Group");
  CheckToastResult(state->notifier.value->Show(toast.value), "ToastNotifier.Show");
  co_return MuonNotificationHandle{"windows-toast", std::move(token), 1};
}

static cardio::promise<void> CancelWindowsToast(
    std::shared_ptr<WindowsToastState> state, MuonNotificationHandle handle) {
  RequireToastRunning(*state);
  if (!state->initialization_error.empty())
    throw std::runtime_error(state->initialization_error);
  if (handle.backend != "windows-toast" || handle.id != 1) co_return;
  const auto compact_token = CompactToastToken(handle.owner);
  ToastCom<notification_abi::ToastHistoryFactory> factory;
  ActivateToastFactory(L"Windows.UI.Notifications.ToastNotificationManager",
      L"{7AB93C52-0E48-4750-BA9D-1A4113981847}", factory);
  ToastCom<notification_abi::ToastHistory> history;
  CheckToastResult(factory.value->get_History(&history.value), "ToastNotificationManager.History");
  const auto tag = ToastString(ToastWide(compact_token.substr(0, 16)));
  const auto group = ToastString(ToastWide(compact_token.substr(16)));
  const auto app = ToastString(state->wide_app_id);
  CheckToastResult(history.value->RemoveGroupedTagWithId(tag.value, group.value, app.value),
      "ToastNotificationHistory.Remove");
  co_return;
}

MuonNotificationBackend CreateMuonWindowsNotificationBackend(
    MuonWindowsNotificationOptions options) {
  auto state = std::make_shared<WindowsToastState>();
#if defined(MUON_NOTIFICATION_TEST)
  test_backend_state = state;
#endif
  state->options = std::move(options);
  state->dispatcher = &cardio::get_current_dispatcher();
  try {
    if (state->options.app_id.empty() || state->options.display_name.empty() ||
        state->options.icon_path.empty() || state->options.activator_clsid.empty())
      throw std::runtime_error("Windows notification registration metadata is missing");
    const auto clsid = ToastGuid(ToastWide(state->options.activator_clsid).c_str());
    state->wide_app_id = ToastWide(state->options.app_id);
    auto initialized = RoInitialize(RO_INIT_SINGLETHREADED);
    if (initialized == RPC_E_CHANGED_MODE) initialized = RoInitialize(RO_INIT_MULTITHREADED);
    CheckToastResult(initialized, "RoInitialize");
    state->initialized = true;
    ToastCom<notification_abi::ToastManager> manager;
    ActivateToastFactory(L"Windows.UI.Notifications.ToastNotificationManager",
        L"{50AC103F-D235-4598-BBEF-98FE4D1A3AD4}", manager);
    const auto app = ToastString(state->wide_app_id);
    CheckToastResult(manager.value->CreateToastNotifierWithId(app.value, &state->notifier.value),
        "CreateToastNotifierWithId");
    auto* factory = new ToastClassFactory(state);
    const auto result = CoRegisterClassObject(clsid, factory, CLSCTX_LOCAL_SERVER,
        REGCLS_MULTIPLEUSE, &state->registration);
    factory->Release();
    CheckToastResult(result, "CoRegisterClassObject");
  } catch (const std::exception& error) {
    // Keep the API usable for diagnostics even when the OS or a manually
    // unpacked application cannot provide the required registration metadata.
    state->initialization_error = error.what();
    state->ReleaseApartmentResources();
  }
  MuonNotificationBackend backend;
  backend.capabilities = [state] { return ReadToastCapabilities(state); };
  backend.status = [state] { return ReadToastStatus(state); };
  backend.post = [state](MuonNotificationPost post, std::string token) {
    return PostWindowsToast(state, std::move(post), std::move(token));
  };
  backend.cancel = [state](MuonNotificationHandle handle) {
    return CancelWindowsToast(state, std::move(handle));
  };
  backend.set_activation = [state](MuonNotificationActivation callback) {
    state->activation = std::move(callback);
  };
  backend.stop = [state] {
    {
      const auto lock = std::lock_guard(state->activation_mutex);
      state->stopped = true;
    }
    state->activation = {};
    // Stop runs on the owner dispatcher. External COM callbacks may still hold
    // state, but they must never release apartment resources on their thread.
    state->ReleaseApartmentResources();
  };
  return backend;
}

#if defined(MUON_NOTIFICATION_TEST)
// Exercise the real COM callback and backend stop without requiring WinRT
// notification support. The barriers reproduce a callback retained across stop.
void TestMuonWindowsNotificationCallbackShutdown() {
  for (const auto* arguments : {
      L"f716b7a0-82e6-4ddf-b2aa-811cda210001|d",
      L"f716b7a0-82e6-4ddf-b2aa-811cda210001|a|default"}) {
    auto backend = CreateMuonWindowsNotificationBackend({});
    auto state = test_backend_state.lock();
    if (!state) throw std::runtime_error("Notification test backend is missing");
    state->wide_app_id = L"dev.muon.NotificationProbe.Shutdown";
    auto entered = std::latch(1);
    auto resume = std::latch(1);
    state->before_activation_post = [&] {
      entered.count_down();
      resume.wait();
    };
    auto* callback = new ToastActivationCallback(state);
    auto result = E_UNEXPECTED;
    auto worker = std::jthread([&] {
      result = callback->Activate(state->wide_app_id.c_str(), arguments, nullptr, 0);
    });
    entered.wait();
    backend.stop();
    backend.stop();
    // Model the owner dispatcher no longer being available. Any attempt to
    // submit a new promise from this callback now fails instead of using it.
    state->dispatcher = nullptr;
    resume.count_down();
    worker.join();
    callback->Release();
    if (result != S_OK)
      throw std::runtime_error("A retained notification callback accessed the stopped dispatcher");
  }
}
#endif
}  // namespace muon_internal
