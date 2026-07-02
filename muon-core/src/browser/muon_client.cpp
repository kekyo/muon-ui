/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_client.h"

#include "browser/muon_browser_background_color.h"
#include "browser/muon_browser_view_delegate.h"
#include "browser/muon_title_bar.h"
#include "browser/muon_window_delegate.h"
#include "plugins/muon_js_bridge.h"
#include "plugins/muon_plugin_metadata.h"
#include "network/muon_network_request_handler.h"
#include "browser/muon_window_state.h"
#include "browser/muon_window_title.h"
#include "browser/show_dev_tools_task.h"
#include "config/muon_startup.h"
#include "log/muon_close_debug_log.h"
#include "log/muon_log.h"
#include "muon_json_helpers.h"
#include "plugins/builtin/muon_builtin_fs_dialogs_plugin.h"
#include "ui/muon_ui_fs_dialogs.h"

#include "include/cef_app.h"
#include "include/cef_command_ids.h"
#include "include/cef_process_message.h"
#include "include/cef_request.h"
#include "include/cef_task.h"
#include "include/cef_urlrequest.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

#include "yyjson.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

constexpr int kVirtualKeyF5 = 0x74;
constexpr int kVirtualKeyF11 = 0x7A;
constexpr int kVirtualKeyF12 = 0x7B;
constexpr int kVirtualKeyI = 0x49;
constexpr int kVirtualKeyR = 0x52;
constexpr int kVirtualKey0 = 0x30;
constexpr int kVirtualKeyOemPlus = 0xBB;
constexpr int kVirtualKeyOemMinus = 0xBD;

static bool IsMuonKnownPopupTargetUrl(const std::string& url) {
  if (url.empty()) {
    return false;
  }
  return url.rfind("about:blank", 0) != 0 &&
         url.rfind("about:srcdoc", 0) != 0;
}

static std::string GetMuonPopupNavigationUrl(const std::string& target_url) {
  return IsMuonKnownPopupTargetUrl(target_url) ? target_url : "about:blank";
}

class CloseMuonWindowTask final : public CefTask {
 public:
  explicit CloseMuonWindowTask(CefRefPtr<CefWindow> window)
      : window_(window) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();

    {
      std::ostringstream log;
      log << "CloseWindowTask Execute window="
          << FormatMuonCloseDebugPointer(window_.get());
      AppendMuonCloseDebugLog(log.str());
    }
    if (window_) {
      window_->Close();
    }
  }

 private:
  CefRefPtr<CefWindow> window_;

  IMPLEMENT_REFCOUNTING(CloseMuonWindowTask);
  DISALLOW_COPY_AND_ASSIGN(CloseMuonWindowTask);
};

class QuitMuonMessageLoopTask final : public CefTask {
 public:
  QuitMuonMessageLoopTask() = default;

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();

    AppendMuonCloseDebugLog("QuitMessageLoopTask Execute CefQuitMessageLoop");
    CefQuitMessageLoop();
  }

 private:
  IMPLEMENT_REFCOUNTING(QuitMuonMessageLoopTask);
  DISALLOW_COPY_AND_ASSIGN(QuitMuonMessageLoopTask);
};

class MuonFaviconUrlRequestClient final : public CefURLRequestClient {
 public:
  using Completion =
      std::function<void(bool, std::string, std::vector<uint8_t>)>;

  explicit MuonFaviconUrlRequestClient(Completion completion)
      : completion_(std::move(completion)) {}

  void OnRequestComplete(CefRefPtr<CefURLRequest> request) override {
    auto success = false;
    std::string mime_type;
    if (request && request->GetRequestStatus() == UR_SUCCESS) {
      const auto response = request->GetResponse();
      const auto status = response ? response->GetStatus() : 0;
      if (status >= 200 && status < 300) {
        success = true;
        mime_type = response->GetMimeType().ToString();
      }
    }
    completion_(success, std::move(mime_type),
                success ? std::move(data_) : std::vector<uint8_t>());
  }

  void OnUploadProgress(CefRefPtr<CefURLRequest> request,
                        int64_t current,
                        int64_t total) override {
    (void)request;
    (void)current;
    (void)total;
  }

  void OnDownloadProgress(CefRefPtr<CefURLRequest> request,
                          int64_t current,
                          int64_t total) override {
    (void)request;
    (void)current;
    (void)total;
  }

  void OnDownloadData(CefRefPtr<CefURLRequest> request,
                      const void* data,
                      size_t data_length) override {
    (void)request;
    if (data == nullptr || data_length == 0) {
      return;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    data_.insert(data_.end(), bytes, bytes + data_length);
  }

  bool GetAuthCredentials(bool isProxy,
                          const CefString& host,
                          int port,
                          const CefString& realm,
                          const CefString& scheme,
                          CefRefPtr<CefAuthCallback> callback) override {
    (void)isProxy;
    (void)host;
    (void)port;
    (void)realm;
    (void)scheme;
    (void)callback;
    return false;
  }

 private:
  Completion completion_;
  std::vector<uint8_t> data_;

  IMPLEMENT_REFCOUNTING(MuonFaviconUrlRequestClient);
  DISALLOW_COPY_AND_ASSIGN(MuonFaviconUrlRequestClient);
};

static CefRefPtr<CefWindow> GetMuonWindowForCloseBrowser(
    CefRefPtr<CefBrowser> browser) {
  if (!browser) {
    return nullptr;
  }
  const auto browser_view = CefBrowserView::GetForBrowser(browser);
  if (browser_view) {
    const auto window = browser_view->GetWindow();
    if (window) {
      return window;
    }
  }
  return nullptr;
}

class CloseMuonBrowsersForShutdownTask final : public CefTask {
 public:
  explicit CloseMuonBrowsersForShutdownTask(
      std::vector<CefRefPtr<CefBrowser>> browsers)
      : browsers_(std::move(browsers)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();

    auto started_close = false;
    {
      std::ostringstream log;
      log << "ShutdownCloseTask Execute browsers=" << browsers_.size();
      AppendMuonCloseDebugLog(log.str());
    }
    for (const auto& browser : browsers_) {
      if (!browser || !browser->IsValid()) {
        std::ostringstream log;
        log << "ShutdownCloseTask skip invalid browser="
            << FormatMuonCloseDebugPointer(browser.get());
        AppendMuonCloseDebugLog(log.str());
        continue;
      }
      auto host = browser->GetHost();
      if (!host) {
        std::ostringstream log;
        log << "ShutdownCloseTask skip no_host browser="
            << FormatMuonCloseDebugPointer(browser.get())
            << " browser_id=" << browser->GetIdentifier();
        AppendMuonCloseDebugLog(log.str());
        continue;
      }
      host->CloseDevTools();
      const auto window = GetMuonWindowForCloseBrowser(browser);
      if (window) {
        std::ostringstream log;
        log << "ShutdownCloseTask close window="
            << FormatMuonCloseDebugPointer(window.get())
            << " browser=" << FormatMuonCloseDebugPointer(browser.get())
            << " browser_id=" << browser->GetIdentifier();
        AppendMuonCloseDebugLog(log.str());
        window->Close();
        started_close = true;
        continue;
      }
      {
        std::ostringstream log;
        log << "ShutdownCloseTask close browser="
            << FormatMuonCloseDebugPointer(browser.get())
            << " browser_id=" << browser->GetIdentifier();
        AppendMuonCloseDebugLog(log.str());
      }
      host->CloseBrowser(true);
      started_close = true;
    }
    if (!started_close) {
      AppendMuonCloseDebugLog(
          "ShutdownCloseTask no_started_close CefQuitMessageLoop");
      CefQuitMessageLoop();
    }
  }

 private:
  std::vector<CefRefPtr<CefBrowser>> browsers_;

  IMPLEMENT_REFCOUNTING(CloseMuonBrowsersForShutdownTask);
  DISALLOW_COPY_AND_ASSIGN(CloseMuonBrowsersForShutdownTask);
};

class OpenMuonDetachedPopupTask final : public CefTask {
 public:
  OpenMuonDetachedPopupTask(
      CefRefPtr<CefClient> client,
      CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler,
      MuonWindowCloseHandler* close_handler,
      std::string target_url,
      MuonBrowserBackgroundColorConfig browser_background_color,
      CefRefPtr<CefDictionaryValue> extra_info,
      bool initial_title_bar_visibility,
      MuonTitleBarManifest title_bar_manifest,
      MuonTitleBarBackgroundColor title_bar_background_color,
      std::string linux_desktop_id)
      : client_(client),
        shortcut_handler_(shortcut_handler),
        close_handler_(close_handler),
        target_url_(std::move(target_url)),
        browser_background_color_(browser_background_color),
        extra_info_(extra_info),
        initial_title_bar_visibility_(initial_title_bar_visibility),
        title_bar_manifest_(std::move(title_bar_manifest)),
        title_bar_background_color_(title_bar_background_color),
        linux_desktop_id_(std::move(linux_desktop_id)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();

    {
      std::ostringstream log;
      log << "DetachedPopupTask Execute target_url=" << target_url_
          << " client=" << FormatMuonCloseDebugPointer(client_.get())
          << " titlebar_custom="
          << FormatMuonCloseDebugBool(IsCustomMuonTitleBar(title_bar_manifest_));
      AppendMuonCloseDebugLog(log.str());
    }
    if (!client_) {
      return;
    }
    CefBrowserSettings settings;
    ApplyMuonBrowserBackgroundColor(settings, browser_background_color_);
    const auto browser_view = CefBrowserView::CreateBrowserView(
        client_, GetMuonPopupNavigationUrl(target_url_), settings,
        extra_info_, nullptr,
        new MuonBrowserViewDelegate(
            false, initial_title_bar_visibility_, title_bar_manifest_,
            title_bar_background_color_, shortcut_handler_, close_handler_,
            linux_desktop_id_));
    if (!browser_view) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                     "Failed to create detached popup browser view");
      AppendMuonCloseDebugLog(
          "DetachedPopupTask failed reason=no_browser_view");
      return;
    }
    {
      std::ostringstream log;
      log << "DetachedPopupTask CreateTopLevelWindow browser_view="
          << FormatMuonCloseDebugPointer(browser_view.get());
      AppendMuonCloseDebugLog(log.str());
    }
    CefWindow::CreateTopLevelWindow(
        new MuonWindowDelegate(browser_view, false,
                               kMuonBrowserInitialWindowStateNormal,
                               initial_title_bar_visibility_,
                               title_bar_manifest_,
                               title_bar_background_color_,
                               shortcut_handler_,
                               close_handler_,
                               linux_desktop_id_));
  }

 private:
  CefRefPtr<CefClient> client_;
  CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler_;
  MuonWindowCloseHandler* close_handler_;
  const std::string target_url_;
  const MuonBrowserBackgroundColorConfig browser_background_color_;
  CefRefPtr<CefDictionaryValue> extra_info_;
  const bool initial_title_bar_visibility_;
  const MuonTitleBarManifest title_bar_manifest_;
  const MuonTitleBarBackgroundColor title_bar_background_color_;
  const std::string linux_desktop_id_;

  IMPLEMENT_REFCOUNTING(OpenMuonDetachedPopupTask);
  DISALLOW_COPY_AND_ASSIGN(OpenMuonDetachedPopupTask);
};

class CompleteCefFileDialogTask final : public CefTask {
 public:
  CompleteCefFileDialogTask(
      CefRefPtr<CefFileDialogCallback> callback,
      std::vector<std::string> file_paths,
      std::string error_message)
      : callback_(callback),
        file_paths_(std::move(file_paths)),
        error_message_(std::move(error_message)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();

    if (!callback_) {
      return;
    }
    if (!error_message_.empty()) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                     "CEF file dialog failed: " + error_message_);
      callback_->Cancel();
      return;
    }
    if (file_paths_.empty()) {
      callback_->Cancel();
      return;
    }
    std::vector<CefString> cef_paths;
    cef_paths.reserve(file_paths_.size());
    for (const auto& path : file_paths_) {
      cef_paths.push_back(path);
    }
    callback_->Continue(cef_paths);
  }

 private:
  CefRefPtr<CefFileDialogCallback> callback_;
  std::vector<std::string> file_paths_;
  std::string error_message_;

  IMPLEMENT_REFCOUNTING(CompleteCefFileDialogTask);
  DISALLOW_COPY_AND_ASSIGN(CompleteCefFileDialogTask);
};

static MuonPluginInvocationContext CreateMuonPluginInvocationContext(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    int renderer_context_id = 0) {
  MuonPluginInvocationContext context;
  context.browser_id = browser ? browser->GetIdentifier() : 0;
  context.frame_id = frame ? frame->GetIdentifier().ToString() : "";
  context.renderer_context_id = renderer_context_id;
  context.frame = frame;
  return context;
}

static bool IsMuonPluginCallMessageName(const std::string& message_name) {
  return message_name == kMuonPluginCallSharedMessageName ||
         message_name == kMuonPluginCallMessageName ||
         message_name == kMuonPluginProxyCallSharedMessageName ||
         message_name == kMuonPluginProxyCallMessageName;
}

static int GetMuonPluginCallRendererContextId(
    const std::string& message_name,
    CefRefPtr<CefListValue> args) {
  if (!args) {
    return 0;
  }
  if (message_name == kMuonPluginCallMessageName && args->GetSize() >= 4) {
    return args->GetInt(3);
  }
  if (message_name == kMuonPluginProxyCallMessageName &&
      args->GetSize() >= 4) {
    return args->GetInt(3);
  }
  return 0;
}

static constexpr char kMuonFunctionValueKindKey[] = "kind";
static constexpr char kMuonFunctionValueKindPluginProxy[] = "plugin_proxy";
static constexpr char kMuonFunctionValueProxyIdKey[] = "proxy_id";
static constexpr char kMuonFunctionValueTypeKey[] = "type_key";
static constexpr char kMuonFsDialogsNamespace[] = "muon.fs.dialogs";
static constexpr cef_color_t kMuonModalInputBlockerBackground =
    CefColorSetARGB(1, 0, 0, 0);

struct MuonCefFileDialogState {
  CefRefPtr<CefFileDialogCallback> callback;
  muon_ui_fs_dialog_operation_handle operation = nullptr;
};

static bool GetMuonUiDialogKindFromCefMode(
    CefDialogHandler::FileDialogMode mode,
    muon_ui_fs_dialog_kind* kind) {
  if (kind == nullptr) {
    return false;
  }
  switch (mode) {
    case FILE_DIALOG_OPEN:
      *kind = MUON_UI_FS_DIALOG_SELECT_FILE;
      return true;
    case FILE_DIALOG_OPEN_MULTIPLE:
      *kind = MUON_UI_FS_DIALOG_SELECT_FILES;
      return true;
    case FILE_DIALOG_OPEN_FOLDER:
      *kind = MUON_UI_FS_DIALOG_SELECT_DIRECTORY;
      return true;
    case FILE_DIALOG_SAVE:
      *kind = MUON_UI_FS_DIALOG_SELECT_SAVE_FILE;
      return true;
    case FILE_DIALOG_NUM_VALUES:
      return false;
  }
  return false;
}

static bool IsCefSaveDialogMode(CefDialogHandler::FileDialogMode mode) {
  return mode == FILE_DIALOG_SAVE;
}

static std::string GetDefaultCefFileDialogTitle(
    CefDialogHandler::FileDialogMode mode) {
  if (mode == FILE_DIALOG_SAVE) {
    return "Save";
  }
  if (mode == FILE_DIALOG_OPEN_FOLDER) {
    return "Select Folder";
  }
  return "Open";
}

static bool IsCefMimeFilter(const std::string& value) {
  return value.find('/') != std::string::npos;
}

static std::string NormalizeCefFileExtension(std::string value) {
  if (value.rfind("*.", 0) == 0) {
    value.erase(0, 2);
  } else if (!value.empty() && value[0] == '.') {
    value.erase(0, 1);
  }
  return value;
}

static void AppendCefFileDialogFilterExtensions(
    std::vector<std::string>* extensions,
    const std::string& source) {
  auto start = size_t{0};
  while (start <= source.size()) {
    const auto end = source.find(';', start);
    auto entry = source.substr(start, end == std::string::npos
                                          ? std::string::npos
                                          : end - start);
    entry = NormalizeCefFileExtension(entry);
    if (!entry.empty() && !IsCefMimeFilter(entry)) {
      extensions->push_back(std::move(entry));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

static void AppendJsonStringArray(std::string* target,
                                  const std::vector<std::string>& values) {
  target->push_back('[');
  auto first = true;
  for (const auto& value : values) {
    if (!first) {
      target->push_back(',');
    }
    first = false;
    muon_internal::AppendJsonString(target, value);
  }
  target->push_back(']');
}

static std::vector<std::string> ReadCefAcceptMimeTypes(
    const std::vector<CefString>& accept_filters,
    const std::vector<CefString>& accept_extensions) {
  std::vector<std::string> mime_types;
  for (auto index = size_t{0}; index < accept_filters.size(); ++index) {
    const auto filter = accept_filters[index].ToString();
    const auto extensions = index < accept_extensions.size()
                                ? accept_extensions[index].ToString()
                                : std::string{};
    if (!filter.empty() && IsCefMimeFilter(filter) && extensions.empty()) {
      mime_types.push_back(filter);
    }
  }
  return mime_types;
}

static void AppendCefAcceptFiltersJson(
    std::string* target,
    const std::vector<CefString>& accept_filters,
    const std::vector<CefString>& accept_extensions,
    const std::vector<CefString>& accept_descriptions) {
  std::vector<std::pair<std::string, std::vector<std::string>>> filters;
  for (auto index = size_t{0}; index < accept_filters.size(); ++index) {
    const auto filter = accept_filters[index].ToString();
    const auto expanded_extensions = index < accept_extensions.size()
                                         ? accept_extensions[index].ToString()
                                         : std::string{};
    std::vector<std::string> extensions;
    if (!expanded_extensions.empty()) {
      AppendCefFileDialogFilterExtensions(&extensions, expanded_extensions);
    } else if (!filter.empty()) {
      AppendCefFileDialogFilterExtensions(&extensions, filter);
    }
    if (extensions.empty()) {
      continue;
    }
    auto name = index < accept_descriptions.size()
                    ? accept_descriptions[index].ToString()
                    : std::string{};
    if (name.empty()) {
      name = filter.empty() ? "Accepted files" : filter;
    }
    filters.push_back({std::move(name), std::move(extensions)});
  }
  if (filters.empty()) {
    return;
  }
  target->append(",\"filters\":[");
  auto first_filter = true;
  for (const auto& filter : filters) {
    if (!first_filter) {
      target->push_back(',');
    }
    first_filter = false;
    target->append("{\"name\":");
    muon_internal::AppendJsonString(target, filter.first);
    target->append(",\"extensions\":");
    AppendJsonStringArray(target, filter.second);
    target->push_back('}');
  }
  target->push_back(']');
}

static std::string CreateCefFileDialogOptionsJson(
    CefDialogHandler::FileDialogMode mode,
    const CefString& title,
    const CefString& default_file_path,
    const std::vector<CefString>& accept_filters,
    const std::vector<CefString>& accept_extensions,
    const std::vector<CefString>& accept_descriptions) {
  auto result = std::string("{\"title\":");
  const auto title_text = title.ToString().empty()
                              ? GetDefaultCefFileDialogTitle(mode)
                              : title.ToString();
  muon_internal::AppendJsonString(&result, title_text);
  const auto default_path = default_file_path.ToString();
  if (!default_path.empty()) {
    result.append(",\"defaultPath\":");
    if (IsCefSaveDialogMode(mode)) {
      const auto path = std::filesystem::path(default_path);
      const auto parent = path.parent_path();
      muon_internal::AppendJsonString(
          &result, parent.empty() ? default_path : parent.string());
      const auto filename = path.filename().string();
      if (!filename.empty()) {
        result.append(",\"defaultName\":");
        muon_internal::AppendJsonString(&result, filename);
      }
    } else {
      muon_internal::AppendJsonString(&result, default_path);
    }
  }
  AppendCefAcceptFiltersJson(
      &result, accept_filters, accept_extensions, accept_descriptions);
  const auto mime_types =
      ReadCefAcceptMimeTypes(accept_filters, accept_extensions);
  if (!mime_types.empty()) {
    result.append(",\"gtk\":{\"mimeTypes\":");
    AppendJsonStringArray(&result, mime_types);
    result.push_back('}');
  }
  result.push_back('}');
  return result;
}

static bool ParseMuonFsDialogResultJson(const char* result_json,
                                        std::vector<std::string>* paths,
                                        std::string* error_message) {
  paths->clear();
  if (result_json == nullptr) {
    return true;
  }
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(result_json), std::strlen(result_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    *error_message = "Native dialog returned invalid JSON";
    return false;
  }
  auto* root = yyjson_doc_get_root(document);
  if (!yyjson_is_arr(root)) {
    yyjson_doc_free(document);
    *error_message = "Native dialog returned a non-array result";
    return false;
  }
  size_t index = 0;
  size_t max = 0;
  yyjson_val* entry = nullptr;
  yyjson_arr_foreach(root, index, max, entry) {
    if (!yyjson_is_str(entry)) {
      yyjson_doc_free(document);
      *error_message = "Native dialog returned a non-string path";
      return false;
    }
    paths->push_back(std::string(yyjson_get_str(entry), yyjson_get_len(entry)));
  }
  yyjson_doc_free(document);
  return true;
}

static void CompleteCefFileDialog(
    void* raw_state,
    const char* result_json,
    const char* error_message) {
  auto state = std::unique_ptr<MuonCefFileDialogState>(
      static_cast<MuonCefFileDialogState*>(raw_state));
  if (!state) {
    return;
  }
  std::vector<std::string> paths;
  std::string parsed_error;
  if (error_message != nullptr) {
    parsed_error = error_message;
  } else if (!ParseMuonFsDialogResultJson(
                 result_json, &paths, &parsed_error)) {
  }
  CefPostTask(TID_UI, new CompleteCefFileDialogTask(
                          state->callback, std::move(paths),
                          std::move(parsed_error)));
}

static uint32_t ReadMuonShortcutModifiers(const CefKeyEvent& event) {
  auto modifiers = uint32_t{0};
  if ((event.modifiers & EVENTFLAG_SHIFT_DOWN) != 0) {
    modifiers |= kMuonShortcutModifierShift;
  }
  if ((event.modifiers & EVENTFLAG_CONTROL_DOWN) != 0) {
    modifiers |= kMuonShortcutModifierControl;
  }
  if ((event.modifiers & EVENTFLAG_ALT_DOWN) != 0) {
    modifiers |= kMuonShortcutModifierAlt;
  }
  if ((event.modifiers & EVENTFLAG_COMMAND_DOWN) != 0) {
    modifiers |= kMuonShortcutModifierMeta;
  }
  return modifiers;
}

static bool MatchesShortcut(const MuonKeyboardShortcut& shortcut,
                            const CefKeyEvent& event) {
  if (!shortcut.enabled || event.windows_key_code != shortcut.windows_key_code) {
    return false;
  }
  if ((event.modifiers & EVENTFLAG_ALTGR_DOWN) != 0) {
    return false;
  }
  const auto event_modifiers = ReadMuonShortcutModifiers(event);
  if (event_modifiers == shortcut.modifiers) {
    return true;
  }
  if (!shortcut.accepts_shift_variant ||
      (shortcut.modifiers & kMuonShortcutModifierShift) != 0) {
    return false;
  }
  return event_modifiers ==
         (shortcut.modifiers | kMuonShortcutModifierShift);
}

static bool MatchesShortcutSpec(int windows_key_code,
                                uint32_t modifiers,
                                bool accepts_shift_variant,
                                const CefKeyEvent& event) {
  MuonKeyboardShortcut shortcut;
  shortcut.enabled = true;
  shortcut.windows_key_code = windows_key_code;
  shortcut.modifiers = modifiers;
  shortcut.accepts_shift_variant = accepts_shift_variant;
  return MatchesShortcut(shortcut, event);
}

static bool MatchesShortcutSpec(int windows_key_code,
                                uint32_t modifiers,
                                const CefKeyEvent& event) {
  return MatchesShortcutSpec(windows_key_code, modifiers, false, event);
}

static void MarkKeyboardShortcut(bool* is_keyboard_shortcut) {
  if (is_keyboard_shortcut != nullptr) {
    *is_keyboard_shortcut = true;
  }
}

static MuonLogLevel GetMuonLogLevelFromCefSeverity(cef_log_severity_t level) {
  switch (level) {
    case LOGSEVERITY_VERBOSE:
      return kMuonLogLevelDebug;
    case LOGSEVERITY_INFO:
    case LOGSEVERITY_DEFAULT:
      return kMuonLogLevelInfo;
    case LOGSEVERITY_WARNING:
      return kMuonLogLevelWarning;
    case LOGSEVERITY_ERROR:
      return kMuonLogLevelError;
    case LOGSEVERITY_FATAL:
      return kMuonLogLevelFatal;
    case LOGSEVERITY_DISABLE:
      return kMuonLogLevelInfo;
  }
  return kMuonLogLevelInfo;
}

static const char* GetMuonBuiltinBrowserFunctionKindName(
    MuonBuiltinBrowserFunctionKind kind) {
  switch (kind) {
    case MuonBuiltinBrowserFunctionKind::Reload:
      return "Reload";
    case MuonBuiltinBrowserFunctionKind::HardReload:
      return "HardReload";
    case MuonBuiltinBrowserFunctionKind::ToggleFullscreen:
      return "ToggleFullscreen";
    case MuonBuiltinBrowserFunctionKind::EnterFullscreen:
      return "EnterFullscreen";
    case MuonBuiltinBrowserFunctionKind::ExitFullscreen:
      return "ExitFullscreen";
    case MuonBuiltinBrowserFunctionKind::ZoomIn:
      return "ZoomIn";
    case MuonBuiltinBrowserFunctionKind::ZoomOut:
      return "ZoomOut";
    case MuonBuiltinBrowserFunctionKind::ResetZoom:
      return "ResetZoom";
    case MuonBuiltinBrowserFunctionKind::Show:
      return "Show";
    case MuonBuiltinBrowserFunctionKind::Hide:
      return "Hide";
    case MuonBuiltinBrowserFunctionKind::Focus:
      return "Focus";
    case MuonBuiltinBrowserFunctionKind::Blur:
      return "Blur";
    case MuonBuiltinBrowserFunctionKind::Minimize:
      return "Minimize";
    case MuonBuiltinBrowserFunctionKind::Maximize:
      return "Maximize";
    case MuonBuiltinBrowserFunctionKind::Restore:
      return "Restore";
    case MuonBuiltinBrowserFunctionKind::SetTitleBarVisibility:
      return "SetTitleBarVisibility";
    case MuonBuiltinBrowserFunctionKind::SetTitleBarIcon:
      return "SetTitleBarIcon";
    case MuonBuiltinBrowserFunctionKind::GetWindowBounds:
      return "GetWindowBounds";
    case MuonBuiltinBrowserFunctionKind::SetWindowBounds:
      return "SetWindowBounds";
    case MuonBuiltinBrowserFunctionKind::Close:
      return "Close";
    case MuonBuiltinBrowserFunctionKind::Shutdown:
      return "Shutdown";
    case MuonBuiltinBrowserFunctionKind::Recycle:
      return "Recycle";
    case MuonBuiltinBrowserFunctionKind::None:
      return "None";
  }
  return "Unknown";
}

MuonClient::MuonClient(std::shared_ptr<MuonPluginRuntime> plugin_runtime,
                       std::shared_ptr<MuonNetworkPolicy> network_policy,
                       std::shared_ptr<MuonNetworkPolicy> plugin_page_policy,
                       std::map<std::string,
                                std::shared_ptr<MuonPluginPolicy>>
                           plugin_capability_policies,
                       std::shared_ptr<MuonNetworkPolicy>
                           unsafe_parent_access_policy,
                       std::function<bool(int32_t)> shutdown_requester,
                       std::shared_ptr<MuonAppStorage> app_storage,
                       const MuonBrowserConfig& browser_config,
                       MuonTitleBarManifest title_bar_manifest,
                       MuonTitleBarBackgroundColor title_bar_background_color,
                       bool has_initial_title_bar_icon,
                       MuonTitleBarIcon initial_title_bar_icon,
                       std::string linux_desktop_id)
    : browser_config_(browser_config),
      title_bar_manifest_(std::move(title_bar_manifest)),
      title_bar_background_color_(title_bar_background_color),
      linux_desktop_id_(std::move(linux_desktop_id)),
      has_initial_title_bar_icon_(has_initial_title_bar_icon),
      initial_title_bar_icon_(std::move(initial_title_bar_icon)),
      shutdown_requester_(std::move(shutdown_requester)),
      app_storage_(std::move(app_storage)),
      plugin_runtime_(std::move(plugin_runtime)),
      network_policy_(std::move(network_policy)),
      plugin_page_policy_(std::move(plugin_page_policy)),
      plugin_capability_policies_(std::move(plugin_capability_policies)),
      unsafe_parent_access_policy_(std::move(unsafe_parent_access_policy)) {
  std::ostringstream log;
  log << "MuonClient ctor this=" << FormatMuonCloseDebugPointer(this)
      << " titlebar_custom="
      << FormatMuonCloseDebugBool(IsCustomMuonTitleBar(title_bar_manifest_))
      << " has_initial_title_bar_icon="
      << FormatMuonCloseDebugBool(has_initial_title_bar_icon_);
  AppendMuonCloseDebugLog(log.str());
}

CefRefPtr<CefLifeSpanHandler> MuonClient::GetLifeSpanHandler() {
  return this;
}

CefRefPtr<CefDisplayHandler> MuonClient::GetDisplayHandler() {
  return this;
}

CefRefPtr<CefContextMenuHandler> MuonClient::GetContextMenuHandler() {
  return this;
}

CefRefPtr<CefCommandHandler> MuonClient::GetCommandHandler() {
  return this;
}

CefRefPtr<CefKeyboardHandler> MuonClient::GetKeyboardHandler() {
  return this;
}

CefRefPtr<CefDialogHandler> MuonClient::GetDialogHandler() {
  return this;
}

CefRefPtr<CefDragHandler> MuonClient::GetDragHandler() {
  return this;
}

CefRefPtr<CefRequestHandler> MuonClient::GetRequestHandler() {
  return this;
}

CefRefPtr<CefResourceRequestHandler> MuonClient::GetResourceRequestHandler(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool is_navigation,
    bool is_download,
    const CefString& request_initiator,
    bool& disable_default_handling) {
  disable_default_handling = false;
  const auto is_top_level_navigation =
      is_navigation && (!frame || frame->IsMain());
  return CreateMuonNetworkResourceRequestHandler(
      network_policy_, is_top_level_navigation, request_initiator.ToString());
}

bool MuonClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefRefPtr<CefRequest> request,
                                bool user_gesture,
                                bool is_redirect) {
  CEF_REQUIRE_UI_THREAD();
  (void)request;
  (void)user_gesture;
  (void)is_redirect;

  if (IsCustomMuonTitleBar(title_bar_manifest_) && frame && frame->IsMain()) {
    CefRefPtr<CefBrowserView> browser_view;
    CefRefPtr<CefWindow> window;
    std::string error_message;
    if (GetBrowserViewAndWindow(browser, &browser_view, &window,
                                &error_message)) {
      SetRegisteredMuonPageDraggableRegions(window, browser_view, {});
    }
  }
  return false;
}

bool MuonClient::OnBeforePopup(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    int popup_id,
    const CefString& target_url,
    const CefString& target_frame_name,
    CefLifeSpanHandler::WindowOpenDisposition target_disposition,
    bool user_gesture,
    const CefPopupFeatures& popupFeatures,
    CefWindowInfo& windowInfo,
    CefRefPtr<CefClient>& client,
    CefBrowserSettings& settings,
    CefRefPtr<CefDictionaryValue>& extra_info,
    bool* no_javascript_access) {
  CEF_REQUIRE_UI_THREAD();
  (void)browser;
  (void)frame;
  (void)popup_id;
  (void)target_frame_name;
  (void)target_disposition;
  (void)user_gesture;
  (void)popupFeatures;
  (void)windowInfo;

  const auto url = target_url.ToString();
  const auto has_known_target_url = IsPopupTargetUrlKnown(url);
  const auto is_network_allowed_target =
      !has_known_target_url || !network_policy_ ||
      network_policy_->IsAllowedRequest(url, true, "");
  {
    std::ostringstream log;
    log << "MuonClient OnBeforePopup browser="
        << FormatMuonCloseDebugPointer(browser.get())
        << " browser_id=" << (browser ? browser->GetIdentifier() : 0)
        << " popup_id=" << popup_id
        << " target_url=" << url
        << " has_known_target_url="
        << FormatMuonCloseDebugBool(has_known_target_url)
        << " is_network_allowed_target="
        << FormatMuonCloseDebugBool(is_network_allowed_target)
        << " tracked=" << browsers_by_id_.size();
    AppendMuonCloseDebugLog(log.str());
  }

  client = this;
  if (plugin_runtime_) {
    extra_info = plugin_runtime_->CreateRendererMetadata();
    WriteMuonRendererUrlHint(extra_info, url);
  }
  ApplyMuonBrowserBackgroundColor(settings, browser_config_.background_color);

  const auto cef_requests_no_javascript_access =
      no_javascript_access != nullptr && *no_javascript_access;
  const auto allows_opener_access =
      is_network_allowed_target && has_known_target_url &&
      unsafe_parent_access_policy_ &&
      unsafe_parent_access_policy_->IsAllowedUrl(url) &&
      !cef_requests_no_javascript_access;
  if (allows_opener_access) {
    AppendMuonCloseDebugLog(
        "MuonClient OnBeforePopup allow_cef_default reason=opener_access");
    if (no_javascript_access != nullptr) {
      *no_javascript_access = false;
    }
    return false;
  }

  CefRefPtr<CefDictionaryValue> detached_extra_info;
  if (plugin_runtime_) {
    detached_extra_info = plugin_runtime_->CreateRendererMetadata();
    if (has_known_target_url) {
      WriteMuonRendererUrlHint(detached_extra_info, url);
    }
  }
  CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler(this);
  auto* close_handler = static_cast<MuonWindowCloseHandler*>(this);
  AppendMuonCloseDebugLog(
      "MuonClient OnBeforePopup post DetachedPopupTask");
  CefPostTask(TID_UI, new OpenMuonDetachedPopupTask(
                          client, shortcut_handler, close_handler, url,
                          browser_config_.background_color,
                          detached_extra_info,
                          browser_config_.initial_title_bar_visibility,
                          title_bar_manifest_, title_bar_background_color_,
                          linux_desktop_id_));
  return true;
}

void MuonClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  {
    std::ostringstream log;
    log << "MuonClient OnAfterCreated begin this="
        << FormatMuonCloseDebugPointer(this)
        << " browser=" << FormatMuonCloseDebugPointer(browser.get())
        << " browser_id=" << (browser ? browser->GetIdentifier() : 0)
        << " tracked_before=" << browsers_by_id_.size()
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }
  browsers_by_id_[browser->GetIdentifier()] = browser;
  const auto browser_view = CefBrowserView::GetForBrowser(browser);
  if (browser_view) {
    RegisterMuonTitleBarBrowserViewBrowser(
        browser_view, browser->GetIdentifier());
  }
  RestoreInitialTitleBarIconForBrowser(
      browser, BeginTitleBarIconUpdateForBrowser(browser->GetIdentifier()));
  if (!message_loop_quit_requested_) {
    quit_message_loop_after_pending_fs_dialogs_ = false;
    quit_message_loop_after_pending_fs_dialogs_browser_id_ = 0;
  }
  {
    std::ostringstream log;
    log << "MuonClient OnAfterCreated end this="
        << FormatMuonCloseDebugPointer(this)
        << " browser=" << FormatMuonCloseDebugPointer(browser.get())
        << " browser_id=" << browser->GetIdentifier()
        << " browser_view="
        << FormatMuonCloseDebugPointer(browser_view.get())
        << " tracked_after=" << browsers_by_id_.size()
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }
}

void MuonClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  const auto browser_id = browser->GetIdentifier();
  {
    std::ostringstream log;
    log << "MuonClient OnBeforeClose begin this="
        << FormatMuonCloseDebugPointer(this)
        << " browser=" << FormatMuonCloseDebugPointer(browser.get())
        << " browser_id=" << browser_id
        << " tracked_before=" << browsers_by_id_.size()
        << " has_browser="
        << FormatMuonCloseDebugBool(
               browsers_by_id_.find(browser_id) != browsers_by_id_.end())
        << " pending_fs=" << pending_fs_dialog_calls_
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }
  if (plugin_runtime_) {
    plugin_runtime_->CancelFsDialogsForOwner(browser_id);
  }
  const auto pending_favicon_request =
      pending_favicon_requests_.find(browser_id);
  if (pending_favicon_request != pending_favicon_requests_.end() &&
      pending_favicon_request->second) {
    pending_favicon_request->second->Cancel();
  }
  pending_favicon_requests_.erase(browser_id);
  title_bar_icon_update_generations_.erase(browser_id);
  ClearModalBrowserViewDisable(browser_id);
  browsers_by_id_.erase(browser_id);
  {
    std::ostringstream log;
    log << "MuonClient OnBeforeClose after_erase this="
        << FormatMuonCloseDebugPointer(this)
        << " browser_id=" << browser_id
        << " tracked_after=" << browsers_by_id_.size()
        << " pending_fs=" << pending_fs_dialog_calls_
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }
  QuitMessageLoopWhenIdle();
}

void MuonClient::BeginPendingFsDialogCall(int browser_id) {
  CEF_REQUIRE_UI_THREAD();
  pending_fs_dialog_calls_ += 1;
  if (browser_id > 0) {
    pending_fs_dialog_calls_by_browser_[browser_id] += 1;
  }
}

void MuonClient::EndPendingFsDialogCall(int browser_id) {
  CEF_REQUIRE_UI_THREAD();
  {
    std::ostringstream log;
    log << "MuonClient EndPendingFsDialogCall begin this="
        << FormatMuonCloseDebugPointer(this)
        << " browser_id=" << browser_id
        << " pending_before=" << pending_fs_dialog_calls_
        << " tracked=" << browsers_by_id_.size()
        << " quit_after_pending="
        << FormatMuonCloseDebugBool(quit_message_loop_after_pending_fs_dialogs_)
        << " quit_after_pending_browser_id="
        << quit_message_loop_after_pending_fs_dialogs_browser_id_;
    AppendMuonCloseDebugLog(log.str());
  }
  if (pending_fs_dialog_calls_ > 0) {
    pending_fs_dialog_calls_ -= 1;
  }
  if (browser_id > 0) {
    const auto iterator =
        pending_fs_dialog_calls_by_browser_.find(browser_id);
    if (iterator != pending_fs_dialog_calls_by_browser_.end()) {
      iterator->second -= 1;
      if (iterator->second <= 0) {
        pending_fs_dialog_calls_by_browser_.erase(iterator);
      }
    }
  }
  CefRefPtr<CefWindow> close_window_after_pending;
  if (browser_id > 0 && !HasPendingFsDialogCallForBrowser(browser_id)) {
    const auto close_iterator =
        close_windows_after_pending_fs_dialogs_.find(browser_id);
    if (close_iterator != close_windows_after_pending_fs_dialogs_.end()) {
      close_window_after_pending = close_iterator->second;
      close_windows_after_pending_fs_dialogs_.erase(close_iterator);
    }
  }
  if (close_window_after_pending && close_window_after_pending->IsValid()) {
    std::ostringstream log;
    log << "MuonClient EndPendingFsDialogCall retry_window_close browser_id="
        << browser_id
        << " window="
        << FormatMuonCloseDebugPointer(close_window_after_pending.get());
    AppendMuonCloseDebugLog(log.str());
    close_window_after_pending->Close();
  }
  if (pending_fs_dialog_calls_ == 0) {
    if (quit_message_loop_after_pending_fs_dialogs_) {
      auto has_other_browser = false;
      for (const auto& browser_entry : browsers_by_id_) {
        if (browser_entry.first !=
            quit_message_loop_after_pending_fs_dialogs_browser_id_) {
          has_other_browser = true;
          break;
        }
      }
      quit_message_loop_after_pending_fs_dialogs_ = false;
      quit_message_loop_after_pending_fs_dialogs_browser_id_ = 0;
      if (!has_other_browser) {
        RequestMessageLoopQuit(true);
        return;
      }
      QuitMessageLoopWhenIdle();
      return;
    }
    QuitMessageLoopWhenIdle();
  }
  {
    std::ostringstream log;
    log << "MuonClient EndPendingFsDialogCall end this="
        << FormatMuonCloseDebugPointer(this)
        << " browser_id=" << browser_id
        << " pending_after=" << pending_fs_dialog_calls_
        << " tracked=" << browsers_by_id_.size()
        << " quit_after_pending="
        << FormatMuonCloseDebugBool(quit_message_loop_after_pending_fs_dialogs_)
        << " quit_after_pending_browser_id="
        << quit_message_loop_after_pending_fs_dialogs_browser_id_;
    AppendMuonCloseDebugLog(log.str());
  }
}

bool MuonClient::RequestCloseAfterPendingFsDialog(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefWindow> window) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser || !window) {
    return false;
  }
  const auto browser_id = browser->GetIdentifier();
  if (!HasPendingFsDialogCallForBrowser(browser_id)) {
    return false;
  }
  close_windows_after_pending_fs_dialogs_[browser_id] = window;
  if (plugin_runtime_) {
    plugin_runtime_->CancelFsDialogsForOwner(browser_id);
  }
  muon_ui_fs_dialogs_cancel_owner_browser(browser_id);
  ClearModalBrowserViewDisable(browser_id);
  std::ostringstream log;
  log << "MuonClient RequestCloseAfterPendingFsDialog browser_id="
      << browser_id
      << " window=" << FormatMuonCloseDebugPointer(window.get())
      << " pending_fs=" << pending_fs_dialog_calls_;
  AppendMuonCloseDebugLog(log.str());
  return true;
}

bool MuonClient::HasPendingFsDialogCallForBrowser(int browser_id) const {
  CEF_REQUIRE_UI_THREAD();
  const auto pending = pending_fs_dialog_calls_by_browser_.find(browser_id);
  return pending != pending_fs_dialog_calls_by_browser_.end() &&
         pending->second > 0;
}

void MuonClient::RequestMessageLoopQuit(bool post_task) {
  CEF_REQUIRE_UI_THREAD();
  {
    std::ostringstream log;
    log << "MuonClient RequestMessageLoopQuit enter this="
        << FormatMuonCloseDebugPointer(this)
        << " post_task=" << FormatMuonCloseDebugBool(post_task)
        << " already_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_)
        << " tracked=" << browsers_by_id_.size()
        << " pending_fs=" << pending_fs_dialog_calls_;
    AppendMuonCloseDebugLog(log.str());
  }
  if (message_loop_quit_requested_) {
    return;
  }
  message_loop_quit_requested_ = true;
  if (post_task) {
    AppendMuonCloseDebugLog(
        "MuonClient RequestMessageLoopQuit post QuitMessageLoopTask");
    CefPostTask(TID_UI, new QuitMuonMessageLoopTask());
    return;
  }
  AppendMuonCloseDebugLog(
      "MuonClient RequestMessageLoopQuit direct CefQuitMessageLoop");
  CefQuitMessageLoop();
}

void MuonClient::QuitMessageLoopWhenIdle() {
  CEF_REQUIRE_UI_THREAD();
  {
    std::ostringstream log;
    log << "MuonClient QuitWhenIdle enter this="
        << FormatMuonCloseDebugPointer(this)
        << " tracked=" << browsers_by_id_.size()
        << " pending_fs=" << pending_fs_dialog_calls_
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }
  if (!browsers_by_id_.empty() || message_loop_quit_requested_) {
    AppendMuonCloseDebugLog(
        "MuonClient QuitWhenIdle return reason=busy_or_already_requested");
    return;
  }
  if (pending_fs_dialog_calls_ > 0) {
    quit_message_loop_after_pending_fs_dialogs_ = true;
    quit_message_loop_after_pending_fs_dialogs_browser_id_ = 0;
    AppendMuonCloseDebugLog(
        "MuonClient QuitWhenIdle defer reason=pending_fs_dialogs");
    return;
  }
  quit_message_loop_after_pending_fs_dialogs_ = false;
  quit_message_loop_after_pending_fs_dialogs_browser_id_ = 0;
  AppendMuonCloseDebugLog(
      "MuonClient QuitWhenIdle request_quit reason=no_tracked_browsers");
  RequestMessageLoopQuit(false);
}

bool MuonClient::PrepareShutdown(
    int32_t exit_code,
    std::vector<CefRefPtr<CefBrowser>>* browsers,
    bool* should_start_shutdown,
    std::string* error_message) {
  CEF_REQUIRE_UI_THREAD();
  {
    std::ostringstream log;
    log << "MuonClient PrepareShutdown begin this="
        << FormatMuonCloseDebugPointer(this)
        << " exit_code=" << exit_code
        << " tracked=" << browsers_by_id_.size()
        << " shutdown_started="
        << FormatMuonCloseDebugBool(shutdown_started_)
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }
  if (browsers == nullptr || should_start_shutdown == nullptr ||
      error_message == nullptr) {
    return false;
  }
  browsers->clear();
  *should_start_shutdown = false;
  error_message->clear();
  if (shutdown_started_) {
    return true;
  }
  if (!shutdown_requester_) {
    *error_message = "Muon shutdown is unavailable";
    return false;
  }
  if (!shutdown_requester_(exit_code)) {
    *error_message = "Muon shutdown was not accepted";
    return false;
  }

  shutdown_started_ = true;
  *should_start_shutdown = true;
  for (const auto& browser_entry : browsers_by_id_) {
    browsers->push_back(browser_entry.second);
  }
  {
    std::ostringstream log;
    log << "MuonClient PrepareShutdown end this="
        << FormatMuonCloseDebugPointer(this)
        << " exit_code=" << exit_code
        << " should_start_shutdown="
        << FormatMuonCloseDebugBool(*should_start_shutdown)
        << " browsers_to_close=" << browsers->size();
    AppendMuonCloseDebugLog(log.str());
  }
  return true;
}

uint64_t MuonClient::BeginTitleBarIconUpdateForBrowser(int browser_id) {
  if (browser_id <= 0) {
    return 0;
  }
  const auto pending_favicon_request =
      pending_favicon_requests_.find(browser_id);
  if (pending_favicon_request != pending_favicon_requests_.end() &&
      pending_favicon_request->second) {
    pending_favicon_request->second->Cancel();
  }
  pending_favicon_requests_.erase(browser_id);
  auto& generation = title_bar_icon_update_generations_[browser_id];
  generation += 1;
  return generation;
}

bool MuonClient::IsCurrentTitleBarIconUpdate(int browser_id,
                                             uint64_t generation) const {
  if (browser_id <= 0 || generation == 0) {
    return false;
  }
  const auto iterator = title_bar_icon_update_generations_.find(browser_id);
  return iterator != title_bar_icon_update_generations_.end() &&
         iterator->second == generation;
}

void MuonClient::RestoreInitialTitleBarIconForBrowser(
    CefRefPtr<CefBrowser> browser,
    uint64_t generation) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser ||
      !IsCurrentTitleBarIconUpdate(browser->GetIdentifier(), generation)) {
    return;
  }

  std::string title_bar_icon_error;
  if (!SetTitleBarIconForBrowser(
          browser,
          has_initial_title_bar_icon_ ? &initial_title_bar_icon_ : nullptr,
          &title_bar_icon_error)) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                   title_bar_icon_error);
  }
}

void MuonClient::StartFaviconTitleBarIconUpdate(
    CefRefPtr<CefBrowser> browser,
    std::vector<std::string> icon_urls) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser) {
    return;
  }
  const auto generation =
      BeginTitleBarIconUpdateForBrowser(browser->GetIdentifier());
  if (icon_urls.empty()) {
    RestoreInitialTitleBarIconForBrowser(browser, generation);
    return;
  }
  ContinueFaviconTitleBarIconUpdate(browser, std::move(icon_urls), 0,
                                    generation);
}

void MuonClient::ContinueFaviconTitleBarIconUpdate(
    CefRefPtr<CefBrowser> browser,
    std::vector<std::string> icon_urls,
    size_t icon_url_index,
    uint64_t generation) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser ||
      !IsCurrentTitleBarIconUpdate(browser->GetIdentifier(), generation)) {
    return;
  }
  while (icon_url_index < icon_urls.size() &&
         icon_urls[icon_url_index].empty()) {
    ++icon_url_index;
  }
  if (icon_url_index >= icon_urls.size()) {
    RestoreInitialTitleBarIconForBrowser(browser, generation);
    return;
  }

  const auto frame = browser->GetMainFrame();
  if (!frame || !frame->IsValid()) {
    RestoreInitialTitleBarIconForBrowser(browser, generation);
    return;
  }

  auto request = CefRequest::Create();
  request->SetMethod("GET");
  request->SetURL(icon_urls[icon_url_index]);

  CefRefPtr<MuonClient> self(this);
  CefRefPtr<MuonFaviconUrlRequestClient> client(new MuonFaviconUrlRequestClient(
      [self, browser, icon_urls, icon_url_index,
       generation](bool success, std::string mime_type,
                   std::vector<uint8_t> data) mutable {
        if (!success) {
          data.clear();
        }
        self->CompleteFaviconTitleBarIconUpdate(
            browser, std::move(icon_urls), icon_url_index, generation,
            std::move(mime_type), std::move(data));
      }));
  auto url_request = frame->CreateURLRequest(request, client);
  if (!url_request) {
    ContinueFaviconTitleBarIconUpdate(browser, std::move(icon_urls),
                                      icon_url_index + 1, generation);
    return;
  }
  pending_favicon_requests_[browser->GetIdentifier()] = url_request;
}

void MuonClient::CompleteFaviconTitleBarIconUpdate(
    CefRefPtr<CefBrowser> browser,
    std::vector<std::string> icon_urls,
    size_t icon_url_index,
    uint64_t generation,
    const std::string& mime_type,
    std::vector<uint8_t> data) {
  CEF_REQUIRE_UI_THREAD();
  if (!browser ||
      !IsCurrentTitleBarIconUpdate(browser->GetIdentifier(), generation)) {
    return;
  }
  pending_favicon_requests_.erase(browser->GetIdentifier());

  if (!data.empty()) {
    MuonTitleBarIcon icon;
    std::string error_message;
    if (LoadMuonTitleBarIconFromImageBytes(
            data.data(), data.size(), mime_type, icon_urls[icon_url_index],
            !IsCustomMuonTitleBar(title_bar_manifest_), &icon,
            &error_message) &&
        SetTitleBarIconForBrowser(browser, &icon, &error_message)) {
      return;
    }
  }

  ContinueFaviconTitleBarIconUpdate(browser, std::move(icon_urls),
                                    icon_url_index + 1, generation);
}

bool MuonClient::SetTitleBarIconForBrowser(
    CefRefPtr<CefBrowser> browser,
    const MuonTitleBarIcon* icon,
    std::string* error_message) {
  if (!browser) {
    if (error_message != nullptr) {
      *error_message = "Browser is not available";
    }
    return false;
  }
  const auto browser_id = browser->GetIdentifier();
  SetRegisteredMuonTitleBarIconForBrowser(browser_id, icon);
  if (!HasRegisteredMuonWindowForBrowser(browser_id)) {
    return true;
  }

  CefRefPtr<CefBrowserView> browser_view;
  CefRefPtr<CefWindow> window;
  if (!GetBrowserViewAndWindow(browser, &browser_view, &window,
                               error_message)) {
    return false;
  }
  (void)browser_view;
  SetRegisteredMuonTitleBarIcon(window, icon);
  return true;
}

void MuonClient::OnAddressChange(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 const CefString& url) {
  CEF_REQUIRE_UI_THREAD();
  (void)url;
  if (!browser || !frame || !frame->IsMain()) {
    return;
  }
  RestoreInitialTitleBarIconForBrowser(
      browser, BeginTitleBarIconUpdateForBrowser(browser->GetIdentifier()));
}

void MuonClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  const auto window_title = GetMuonWindowTitleOrDefault(title.ToString());
  if (browser) {
    SetRegisteredMuonTitleBarTitleForBrowser(
        browser->GetIdentifier(), window_title);
  }
  CefRefPtr<CefBrowserView> browser_view;
  CefRefPtr<CefWindow> window;
  std::string error_message;
  if (!GetBrowserViewAndWindow(browser, &browser_view, &window,
                               &error_message)) {
    return;
  }
  window->SetTitle(window_title);
  SetRegisteredMuonTitleBarTitle(window, window_title);
}

void MuonClient::OnFaviconURLChange(
    CefRefPtr<CefBrowser> browser,
    const std::vector<CefString>& icon_urls) {
  CEF_REQUIRE_UI_THREAD();
  std::vector<std::string> urls;
  urls.reserve(icon_urls.size());
  for (const auto& icon_url : icon_urls) {
    urls.push_back(icon_url.ToString());
  }
  StartFaviconTitleBarIconUpdate(browser, std::move(urls));
}

bool MuonClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                  cef_log_severity_t level,
                                  const CefString& message,
                                  const CefString& source,
                                  int line) {
  CEF_REQUIRE_UI_THREAD();
  (void)browser;
  auto formatted = std::string();
  const auto source_text = source.ToString();
  if (!source_text.empty()) {
    formatted += source_text;
    if (line > 0) {
      formatted += ":" + std::to_string(line);
    }
    formatted += " ";
  }
  formatted += message.ToString();
  LogMuonMessage(kMuonLogSourceConsole, GetMuonLogLevelFromCefSeverity(level),
                 formatted);
  return true;
}

void MuonClient::OnBeforeContextMenu(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefContextMenuParams> params,
    CefRefPtr<CefMenuModel> model) {
  CEF_REQUIRE_UI_THREAD();
  model->Clear();
}

bool MuonClient::OnChromeCommand(
    CefRefPtr<CefBrowser> browser,
    int command_id,
    cef_window_open_disposition_t disposition) {
  CEF_REQUIRE_UI_THREAD();
  return command_id == IDC_RELOAD ||
         command_id == IDC_RELOAD_BYPASSING_CACHE ||
         command_id == IDC_RELOAD_CLEARING_CACHE ||
         command_id == IDC_FULLSCREEN ||
         command_id == IDC_ZOOM_PLUS ||
         command_id == IDC_ZOOM_MINUS ||
         command_id == IDC_ZOOM_NORMAL;
}

bool MuonClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                const CefKeyEvent& event,
                                CefEventHandle os_event,
                                bool* is_keyboard_shortcut) {
  CEF_REQUIRE_UI_THREAD();
  return HandleBrowserShortcut(browser, event, is_keyboard_shortcut);
}

bool MuonClient::HandleBrowserShortcut(CefRefPtr<CefBrowser> browser,
                                       const CefKeyEvent& event,
                                       bool* is_keyboard_shortcut) {
  CEF_REQUIRE_UI_THREAD();
  if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
    return false;
  }

  if (MatchesShortcut(browser_config_.devtools, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    CefPostTask(TID_UI, new ShowDevToolsTask(browser));
    return true;
  }
  if (MatchesShortcut(browser_config_.reload, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    if (browser) {
      browser->Reload();
    }
    return true;
  }
  if (MatchesShortcut(browser_config_.hard_reload, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    if (browser) {
      browser->ReloadIgnoreCache();
    }
    return true;
  }
  if (MatchesShortcut(browser_config_.fullscreen, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    ToggleFullscreen(browser);
    return true;
  }
  if (MatchesShortcut(browser_config_.zoom_in, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    ZoomBrowser(browser, CEF_ZOOM_COMMAND_IN);
    return true;
  }
  if (MatchesShortcut(browser_config_.zoom_out, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    ZoomBrowser(browser, CEF_ZOOM_COMMAND_OUT);
    return true;
  }
  if (MatchesShortcut(browser_config_.reset_zoom, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    ZoomBrowser(browser, CEF_ZOOM_COMMAND_RESET);
    return true;
  }
  if (MatchesShortcut(browser_config_.recycle, event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    std::vector<CefRefPtr<CefBrowser>> browsers;
    auto should_start_shutdown = false;
    std::string error_message;
    if (!PrepareShutdown(kMuonRecycleExitCode, &browsers,
                         &should_start_shutdown, &error_message)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                     "Muon recycle shortcut failed: " + error_message);
      return true;
    }
    if (should_start_shutdown) {
      CefPostTask(TID_UI,
                  new CloseMuonBrowsersForShutdownTask(std::move(browsers)));
    }
    return true;
  }
  if (IsKnownBrowserShortcut(event)) {
    MarkKeyboardShortcut(is_keyboard_shortcut);
    return true;
  }
  return false;
}

bool MuonClient::OnFileDialog(
    CefRefPtr<CefBrowser> browser,
    CefDialogHandler::FileDialogMode mode,
    const CefString& title,
    const CefString& default_file_path,
    const std::vector<CefString>& accept_filters,
    const std::vector<CefString>& accept_extensions,
    const std::vector<CefString>& accept_descriptions,
    CefRefPtr<CefFileDialogCallback> callback) {
  CEF_REQUIRE_UI_THREAD();
  if (!callback) {
    return true;
  }

  auto kind = MUON_UI_FS_DIALOG_SELECT_FILE;
  if (!GetMuonUiDialogKindFromCefMode(mode, &kind)) {
    callback->Cancel();
    return true;
  }

  auto state = std::make_unique<MuonCefFileDialogState>();
  state->callback = callback;
  const auto options_json = CreateCefFileDialogOptionsJson(
      mode, title, default_file_path, accept_filters, accept_extensions,
      accept_descriptions);
  const auto owner_browser_id = browser ? browser->GetIdentifier() : 0;
  auto* raw_state = state.release();
  auto operation = muon_ui_fs_dialog_operation_handle{};
  const auto started = muon_ui_fs_dialogs_run(
      kind, options_json.c_str(), owner_browser_id, &CompleteCefFileDialog,
      raw_state, &operation);
  if (started != 0) {
    state.reset(raw_state);
    callback->Cancel();
    return true;
  }
  raw_state->operation = operation;
  return true;
}

void MuonClient::OnDraggableRegionsChanged(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const std::vector<CefDraggableRegion>& regions) {
  CEF_REQUIRE_UI_THREAD();

  if (!IsCustomMuonTitleBar(title_bar_manifest_) || !frame ||
      !frame->IsMain()) {
    return;
  }

  CefRefPtr<CefBrowserView> browser_view;
  CefRefPtr<CefWindow> window;
  std::string error_message;
  if (!GetBrowserViewAndWindow(browser, &browser_view, &window,
                               &error_message)) {
    return;
  }
  SetRegisteredMuonPageDraggableRegions(window, browser_view, regions);
}

bool MuonClient::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();
  if (source_process != PID_RENDERER) {
    return false;
  }

  const auto message_name = message->GetName().ToString();
  if (IsMuonPluginCallMessageName(message_name) &&
      !IsPluginPageAllowed(frame, plugin_page_policy_)) {
    const auto args = message->GetArgumentList();
    if ((message_name == kMuonPluginCallMessageName ||
         message_name == kMuonPluginProxyCallMessageName) &&
        args && args->GetSize() >= 1) {
      PendingPluginCall rejected_call;
      rejected_call.context = CreateMuonPluginInvocationContext(
          browser, frame,
          GetMuonPluginCallRendererContextId(message_name, args));
      rejected_call.frame = frame;
      rejected_call.call_id = args->GetInt(0);
      RejectPluginCall(rejected_call,
                       "Muon plugin API is not available for this page");
    }
    return true;
  }
  if (message_name == kMuonPluginCallSharedMessageName ||
      message_name == kMuonPluginProxyCallSharedMessageName) {
    auto call_id = 0;
    std::shared_ptr<MuonSharedBufferPayload> payload;
    std::string error_message;
    const auto decoded = DecodeMuonSharedBufferPayload(
        message, &call_id, &payload, &error_message);
    const auto renderer_context_id =
        payload ? payload->renderer_context_id : 0;
    const auto key = CreatePendingSharedKey(message_name,
                                            renderer_context_id,
                                            call_id);
    const auto pending_iterator = pending_plugin_calls_.find(key);
    if (pending_iterator != pending_plugin_calls_.end()) {
      const auto pending_call = pending_iterator->second;
      pending_plugin_calls_.erase(pending_iterator);
      if (!decoded) {
        RejectPluginCall(pending_call, error_message);
      } else {
        DispatchPluginCall(pending_call, payload);
      }
      return true;
    }
    PendingSharedPayload pending_payload;
    pending_payload.payload = payload;
    pending_payload.error_message = error_message;
    pending_payload.has_error = !decoded;
    pending_plugin_call_payloads_[key] = pending_payload;
    return true;
  }
  if (message_name == kMuonPluginCallMessageName) {
    const auto args = message->GetArgumentList();
    if (!plugin_runtime_ || !args || args->GetSize() < 3) {
      return true;
    }

    const auto call_id = args->GetInt(0);
    const auto function_id = static_cast<uint32_t>(args->GetInt(1));
    const auto encoded_args = args->GetList(2);
    const auto renderer_context_id = args->GetSize() >= 4 ? args->GetInt(3) : 0;
    const auto invocation_context = CreateMuonPluginInvocationContext(
        browser, frame, renderer_context_id);
    PendingPluginCall pending_call;
    pending_call.context = invocation_context;
    pending_call.browser = browser;
    pending_call.frame = frame;
    pending_call.encoded_args = encoded_args;
    pending_call.call_id = call_id;
    pending_call.function_id = function_id;
    pending_call.proxy_call = false;
    const auto has_shared_placeholders =
        CefListValueHasMuonSharedBufferPlaceholders(encoded_args);
    const auto shared_key =
        has_shared_placeholders
            ? CreatePendingSharedKey(kMuonPluginCallSharedMessageName,
                                     renderer_context_id, call_id)
            : std::string{};
    if (browser_config_.plugin.mode == kMuonBrowserPluginModeValidate) {
      std::string error_message;
      if (args->GetSize() < 6 ||
          !IsPluginCapabilityAllowed(function_id,
                                     args->GetString(4).ToString(),
                                     args->GetString(5).ToString(),
                                     &error_message)) {
        if (has_shared_placeholders) {
          pending_plugin_call_payloads_.erase(shared_key);
        }
        RejectPluginCall(
            pending_call,
            error_message.empty()
                ? "Muon plugin capability is required for validate mode"
                : error_message);
        return true;
      }
    }
    if (has_shared_placeholders) {
      const auto payload_iterator =
          pending_plugin_call_payloads_.find(shared_key);
      if (payload_iterator == pending_plugin_call_payloads_.end()) {
        pending_plugin_calls_[shared_key] = pending_call;
        return true;
      }
      const auto pending_payload = payload_iterator->second;
      pending_plugin_call_payloads_.erase(payload_iterator);
      if (pending_payload.has_error) {
        RejectPluginCall(pending_call, pending_payload.error_message);
        return true;
      }
      DispatchPluginCall(pending_call, pending_payload.payload);
      return true;
    }
    DispatchPluginCall(pending_call, nullptr);
    return true;
  }
  if (message_name == kMuonPluginProxyCallMessageName) {
    const auto args = message->GetArgumentList();
    if (!plugin_runtime_ || !args || args->GetSize() < 4) {
      return true;
    }

    const auto call_id = args->GetInt(0);
    const auto proxy_id = static_cast<uint32_t>(args->GetInt(1));
    const auto encoded_args = args->GetList(2);
    const auto renderer_context_id = args->GetInt(3);
    const auto invocation_context = CreateMuonPluginInvocationContext(
        browser, frame, renderer_context_id);
    PendingPluginCall pending_call;
    pending_call.context = invocation_context;
    pending_call.browser = browser;
    pending_call.frame = frame;
    pending_call.encoded_args = encoded_args;
    pending_call.call_id = call_id;
    pending_call.function_id = proxy_id;
    pending_call.proxy_call = true;
    if (CefListValueHasMuonSharedBufferPlaceholders(encoded_args)) {
      const auto key = CreatePendingSharedKey(
          kMuonPluginProxyCallSharedMessageName, renderer_context_id, call_id);
      const auto payload_iterator = pending_plugin_call_payloads_.find(key);
      if (payload_iterator == pending_plugin_call_payloads_.end()) {
        pending_plugin_calls_[key] = pending_call;
        return true;
      }
      const auto pending_payload = payload_iterator->second;
      pending_plugin_call_payloads_.erase(payload_iterator);
      if (pending_payload.has_error) {
        RejectPluginCall(pending_call, pending_payload.error_message);
        return true;
      }
      DispatchPluginCall(pending_call, pending_payload.payload);
      return true;
    }
    DispatchPluginCall(pending_call, nullptr);
    return true;
  }
  if (message_name == kMuonRendererFunctionResultSharedMessageName) {
    auto call_id = 0;
    std::shared_ptr<MuonSharedBufferPayload> payload;
    std::string error_message;
    const auto decoded = DecodeMuonSharedBufferPayload(
        message, &call_id, &payload, &error_message);
    const auto metadata_iterator =
        pending_renderer_function_result_messages_.find(call_id);
    if (metadata_iterator !=
        pending_renderer_function_result_messages_.end()) {
      const auto metadata = metadata_iterator->second;
      pending_renderer_function_result_messages_.erase(metadata_iterator);
      if (plugin_runtime_) {
        if (!decoded) {
          const auto error_result =
              CefProcessMessage::Create(kMuonRendererFunctionResultMessageName);
          const auto error_args = error_result->GetArgumentList();
          error_args->SetSize(4);
          error_args->SetInt(0, call_id);
          error_args->SetBool(1, false);
          error_args->SetString(2, error_message);
          error_args->SetNull(3);
          plugin_runtime_->CompleteRendererFunctionCall(error_result, nullptr);
        } else {
          plugin_runtime_->CompleteRendererFunctionCall(metadata, payload);
        }
      }
      return true;
    }
    PendingSharedPayload pending_payload;
    pending_payload.payload = payload;
    pending_payload.error_message = error_message;
    pending_payload.has_error = !decoded;
    pending_renderer_function_result_payloads_[call_id] = pending_payload;
    return true;
  }
  if (message_name == kMuonRendererFunctionResultMessageName) {
    if (plugin_runtime_) {
      const auto args = message->GetArgumentList();
      const auto needs_shared =
          args && args->GetSize() >= 4 && args->GetBool(1) &&
          CefListValueHasMuonSharedBufferPlaceholders(args);
      if (needs_shared) {
        const auto call_id = args->GetInt(0);
        const auto payload_iterator =
            pending_renderer_function_result_payloads_.find(call_id);
        if (payload_iterator ==
            pending_renderer_function_result_payloads_.end()) {
          pending_renderer_function_result_messages_[call_id] = message;
          return true;
        }
        const auto pending_payload = payload_iterator->second;
        pending_renderer_function_result_payloads_.erase(payload_iterator);
        if (pending_payload.has_error) {
          const auto error_result =
              CefProcessMessage::Create(kMuonRendererFunctionResultMessageName);
          const auto error_args = error_result->GetArgumentList();
          error_args->SetSize(4);
          error_args->SetInt(0, call_id);
          error_args->SetBool(1, false);
          error_args->SetString(2, pending_payload.error_message);
          error_args->SetNull(3);
          plugin_runtime_->CompleteRendererFunctionCall(error_result, nullptr);
          return true;
        }
        plugin_runtime_->CompleteRendererFunctionCall(
            message, pending_payload.payload);
        return true;
      }
      plugin_runtime_->CompleteRendererFunctionCall(message, nullptr);
    }
    return true;
  }
  if (message_name == kMuonFunctionContextReleasedMessageName) {
    const auto args = message->GetArgumentList();
    if (!plugin_runtime_ || !args || args->GetSize() < 1) {
      return true;
    }
    const auto invocation_context = CreateMuonPluginInvocationContext(
        browser, frame);
    plugin_runtime_->ReleaseFunctionContext(invocation_context,
                                            args->GetInt(0));
    pending_plugin_calls_.clear();
    pending_plugin_call_payloads_.clear();
    pending_renderer_function_result_messages_.clear();
    pending_renderer_function_result_payloads_.clear();
    return true;
  }
  return false;
}

bool MuonClient::IsKnownBrowserShortcut(const CefKeyEvent& event) {
  return
      // DevTools defaults.
      MatchesShortcutSpec(kVirtualKeyF12, 0, event) ||
      MatchesShortcutSpec(kVirtualKeyI,
                          kMuonShortcutModifierControl |
                              kMuonShortcutModifierShift,
                          event) ||
      MatchesShortcutSpec(kVirtualKeyI,
                          kMuonShortcutModifierMeta |
                              kMuonShortcutModifierAlt,
                          event) ||
      // Fullscreen defaults.
      MatchesShortcutSpec(kVirtualKeyF11, 0, event) ||
      // Reload defaults.
      MatchesShortcutSpec(kVirtualKeyF5, 0, event) ||
      MatchesShortcutSpec(kVirtualKeyR, kMuonShortcutModifierControl,
                          event) ||
      MatchesShortcutSpec(kVirtualKeyR, kMuonShortcutModifierMeta, event) ||
      MatchesShortcutSpec(kVirtualKeyF5, kMuonShortcutModifierControl,
                          event) ||
      MatchesShortcutSpec(kVirtualKeyF5, kMuonShortcutModifierShift, event) ||
      MatchesShortcutSpec(kVirtualKeyR,
                          kMuonShortcutModifierControl |
                              kMuonShortcutModifierShift,
                          event) ||
      MatchesShortcutSpec(kVirtualKeyR,
                          kMuonShortcutModifierMeta |
                              kMuonShortcutModifierShift,
                          event) ||
      // Zoom defaults.
      MatchesShortcutSpec(kVirtualKeyOemPlus, kMuonShortcutModifierControl,
                          true, event) ||
      MatchesShortcutSpec(kVirtualKeyOemPlus, kMuonShortcutModifierMeta,
                          true, event) ||
      MatchesShortcutSpec(kVirtualKeyOemMinus, kMuonShortcutModifierControl,
                          event) ||
      MatchesShortcutSpec(kVirtualKeyOemMinus, kMuonShortcutModifierMeta,
                          event) ||
      MatchesShortcutSpec(kVirtualKey0, kMuonShortcutModifierControl,
                          event) ||
      MatchesShortcutSpec(kVirtualKey0, kMuonShortcutModifierMeta,
                          event);
}

bool MuonClient::GetBrowserViewAndWindow(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefBrowserView>* browser_view,
    CefRefPtr<CefWindow>* window,
    std::string* error_message) {
  if (browser_view != nullptr) {
    *browser_view = nullptr;
  }
  if (window != nullptr) {
    *window = nullptr;
  }
  if (!browser) {
    if (error_message != nullptr) {
      *error_message = "Muon browser is unavailable";
    }
    return false;
  }
  const auto resolved_browser_view = CefBrowserView::GetForBrowser(browser);
  if (!resolved_browser_view) {
    if (error_message != nullptr) {
      *error_message = "Muon browser view is unavailable";
    }
    return false;
  }
  auto resolved_window = resolved_browser_view->GetWindow();
  if (!resolved_window) {
    if (error_message != nullptr) {
      *error_message = "Muon browser window is unavailable";
    }
    return false;
  }
  if (browser_view != nullptr) {
    *browser_view = resolved_browser_view;
  }
  if (window != nullptr) {
    *window = resolved_window;
  }
  return true;
}

bool MuonClient::ReadFsDialogModal(CefRefPtr<CefListValue> encoded_args) {
  if (!encoded_args || encoded_args->GetSize() == 0 ||
      encoded_args->GetType(0) != VTYPE_STRING) {
    return true;
  }
  const auto options_json = encoded_args->GetString(0).ToString();
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(options_json.data()), options_json.size(),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    return true;
  }
  auto modal = true;
  auto* root = yyjson_doc_get_root(document);
  if (root != nullptr && yyjson_is_obj(root)) {
    auto* value = yyjson_obj_get(root, "modal");
    if (value != nullptr && yyjson_is_bool(value)) {
      modal = yyjson_get_bool(value);
    }
  }
  yyjson_doc_free(document);
  return modal;
}

std::uintptr_t GetCefWindowHandleValue(CefWindowHandle handle) {
#if defined(_WIN32) || defined(__APPLE__)
  return reinterpret_cast<std::uintptr_t>(handle);
#else
  return static_cast<std::uintptr_t>(handle);
#endif
}

bool MuonClient::CreateFsDialogArgsWithOwnerBrowserId(
    CefRefPtr<CefListValue> encoded_args,
    int browser_id,
    CefWindowHandle owner_window_handle,
    CefRefPtr<CefListValue>* target,
    std::string* error_message) {
  if (target != nullptr) {
    *target = nullptr;
  }
  if (!encoded_args || encoded_args->GetSize() == 0 ||
      encoded_args->GetType(0) != VTYPE_STRING) {
    if (error_message != nullptr) {
      *error_message = "Filesystem dialog options are unavailable";
    }
    return false;
  }

  const auto options_json = encoded_args->GetString(0).ToString();
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(options_json.data()), options_json.size(),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Filesystem dialog options JSON is invalid";
    }
    return false;
  }

  auto* root = yyjson_doc_get_root(document);
  if (root == nullptr || !yyjson_is_obj(root)) {
    yyjson_doc_free(document);
    if (error_message != nullptr) {
      *error_message = "Filesystem dialog options JSON root must be an object";
    }
    return false;
  }
  const auto object_size = yyjson_obj_size(root);
  yyjson_doc_free(document);

  const auto insertion_position =
      options_json.find_last_not_of(" \t\r\n");
  if (insertion_position == std::string::npos ||
      options_json[insertion_position] != '}') {
    if (error_message != nullptr) {
      *error_message = "Filesystem dialog options JSON object is invalid";
    }
    return false;
  }

  auto owner_options_json = options_json;
  auto owner_options = std::string("\"") +
                       kMuonFsDialogsOwnerBrowserIdOption + "\":" +
                       std::to_string(browser_id);
  const auto owner_window_handle_value =
      GetCefWindowHandleValue(owner_window_handle);
  if (owner_window_handle_value != 0) {
    owner_options += std::string(",\"") +
                     kMuonFsDialogsOwnerWindowHandleOption + "\":\"" +
                     std::to_string(owner_window_handle_value) + "\"";
  }
  owner_options_json.insert(
      insertion_position,
      std::string(object_size == 0 ? "" : ",") + owner_options);

  const auto copied_args = encoded_args->Copy();
  copied_args->SetString(0, owner_options_json);
  if (target != nullptr) {
    *target = copied_args;
  }
  return true;
}

bool MuonClient::IsFsDialogFunction(uint32_t function_id) const {
  if (!plugin_runtime_) {
    return false;
  }
  for (const auto& function : plugin_runtime_->GetFunctions()) {
    if (function.id == function_id &&
        function.plugin_namespace == kMuonFsDialogsNamespace) {
      return true;
    }
  }
  return false;
}

bool MuonClient::BeginModalBrowserViewDisable(
    const PendingPluginCall& call,
    int* browser_id,
    CefWindowHandle* owner_window_handle,
    std::string* error_message) {
  if (browser_id != nullptr) {
    *browser_id = 0;
  }
  if (owner_window_handle != nullptr) {
    *owner_window_handle = 0;
  }
  CefRefPtr<CefBrowserView> browser_view;
  CefRefPtr<CefWindow> window;
  if (!GetBrowserViewAndWindow(call.browser, &browser_view, &window,
                               error_message)) {
    return false;
  }
  const auto resolved_browser_id = call.browser->GetIdentifier();
  auto& state = modal_browser_view_disable_states_[resolved_browser_id];
  if (state.depth == 0) {
    state.browser_view = browser_view;
    state.restore_browser_view_enabled = browser_view->IsEnabled();
    if (state.restore_browser_view_enabled) {
      browser_view->SetEnabled(false);
    }
    // CEF BrowserView disabling does not reliably stop native child-window
    // input on Linux, so cover the browser area with a focusable overlay.
    const auto input_blocker = CefPanel::CreatePanel(nullptr);
    if (input_blocker) {
      input_blocker->SetFocusable(true);
      input_blocker->SetBackgroundColor(kMuonModalInputBlockerBackground);
      const auto overlay = window->AddOverlayView(
          input_blocker, CEF_DOCKING_MODE_CUSTOM, true);
      if (overlay) {
        overlay->SetBounds(browser_view->GetBounds());
        overlay->SetVisible(true);
        input_blocker->RequestFocus();
        state.overlay_controller = overlay;
      }
    }
  }
  state.depth += 1;
  if (browser_id != nullptr) {
    *browser_id = resolved_browser_id;
  }
  if (owner_window_handle != nullptr) {
    *owner_window_handle = window->GetWindowHandle();
  }
  return true;
}

void MuonClient::EndModalBrowserViewDisable(int browser_id) {
  const auto it = modal_browser_view_disable_states_.find(browser_id);
  if (it == modal_browser_view_disable_states_.end()) {
    return;
  }
  auto& state = it->second;
  state.depth -= 1;
  if (state.depth > 0) {
    return;
  }
  const auto browser_view = state.browser_view;
  const auto overlay_controller = state.overlay_controller;
  const auto restore_browser_view_enabled = state.restore_browser_view_enabled;
  modal_browser_view_disable_states_.erase(it);
  if (overlay_controller && overlay_controller->IsValid()) {
    overlay_controller->Destroy();
  }
  if (restore_browser_view_enabled && browser_view && browser_view->IsValid()) {
    browser_view->SetEnabled(true);
  }
}

void MuonClient::ClearModalBrowserViewDisable(int browser_id) {
  const auto it = modal_browser_view_disable_states_.find(browser_id);
  if (it == modal_browser_view_disable_states_.end()) {
    return;
  }
  const auto browser_view = it->second.browser_view;
  const auto overlay_controller = it->second.overlay_controller;
  const auto restore_browser_view_enabled =
      it->second.restore_browser_view_enabled;
  modal_browser_view_disable_states_.erase(it);
  if (overlay_controller && overlay_controller->IsValid()) {
    overlay_controller->Destroy();
  }
  if (restore_browser_view_enabled && browser_view && browser_view->IsValid()) {
    browser_view->SetEnabled(true);
  }
}

void MuonClient::SetFullscreen(CefRefPtr<CefBrowser> browser,
                                bool fullscreen) {
  CefRefPtr<CefWindow> window;
  std::string error_message;
  if (!GetBrowserViewAndWindow(browser, nullptr, &window, &error_message)) {
    return;
  }
  SetMuonWindowFullscreen(window, fullscreen);
}

void MuonClient::ToggleFullscreen(CefRefPtr<CefBrowser> browser) {
  CefRefPtr<CefWindow> window;
  std::string error_message;
  if (!GetBrowserViewAndWindow(browser, nullptr, &window, &error_message)) {
    return;
  }
  SetFullscreen(browser, !window->IsFullscreen());
}

void MuonClient::ZoomBrowser(CefRefPtr<CefBrowser> browser,
                              cef_zoom_command_t command) {
  if (!browser) {
    return;
  }
  const auto host = browser->GetHost();
  if (host && host->CanZoom(command)) {
    host->Zoom(command);
  }
}

std::string MuonClient::CreatePendingSharedKey(const std::string& message_name,
                                                int renderer_context_id,
                                                int call_id) {
  return message_name + ":" + std::to_string(renderer_context_id) + ":" +
         std::to_string(call_id);
}

bool MuonClient::IsPluginPageAllowed(
    CefRefPtr<CefFrame> frame,
    const std::shared_ptr<MuonNetworkPolicy>& plugin_page_policy) {
  if (!frame || !frame->IsMain() || !plugin_page_policy) {
    return false;
  }
  const auto url = frame->GetURL().ToString();
  if (url.rfind("devtools://", 0) == 0 ||
      url.rfind("chrome-devtools://", 0) == 0) {
    return false;
  }
  return plugin_page_policy->IsAllowedUrl(url);
}

bool MuonClient::IsPopupTargetUrlKnown(const std::string& url) {
  return IsMuonKnownPopupTargetUrl(url);
}

bool MuonClient::IsPluginCapabilityAllowed(
    uint32_t function_id,
    const std::string& capability_id,
    const std::string& function_path,
    std::string* error_message) const {
  if (error_message == nullptr) {
    return false;
  }
  if (browser_config_.plugin.mode != kMuonBrowserPluginModeValidate) {
    return true;
  }
  if (capability_id.empty() || function_path.empty()) {
    *error_message = "Muon plugin capability is required for validate mode";
    return false;
  }
  const auto capability_iterator =
      plugin_capability_policies_.find(capability_id);
  if (capability_iterator == plugin_capability_policies_.end() ||
      !capability_iterator->second) {
    *error_message = "Muon plugin capability is unknown: " + capability_id;
    return false;
  }
  if (!capability_iterator->second->IsAllowedFunctionPath(function_path)) {
    *error_message =
        "Muon plugin capability is not allowed for " + function_path;
    return false;
  }
  if (!plugin_runtime_) {
    *error_message = "Muon plugin runtime is unavailable";
    return false;
  }
  for (const auto& function : plugin_runtime_->GetFunctions()) {
    if (function.id != function_id) {
      continue;
    }
    const auto expected_function_path = CreateMuonFunctionPublicPath(function);
    if (expected_function_path != function_path) {
      *error_message =
          "Muon plugin capability function path does not match the requested "
          "function";
      return false;
    }
    return true;
  }

  *error_message = "Unknown Muon plugin function";
  return false;
}

void MuonClient::DispatchPluginCall(
    const PendingPluginCall& call,
    std::shared_ptr<MuonSharedBufferPayload> payload) {
  if (!plugin_runtime_) {
    return;
  }
  CefRefPtr<MuonClient> self(this);
  if (call.proxy_call) {
    plugin_runtime_->InvokeProxy(
        call.context, call.function_id, call.call_id, call.encoded_args,
        std::move(payload),
        [self, frame = call.frame, call_id = call.call_id,
         invocation_context = call.context](
            const MuonPluginCallResult& result) {
          self->SendPluginResult(invocation_context, frame, call_id, result);
        });
    return;
  }
  const auto browser_function =
      plugin_runtime_->GetBuiltinBrowserFunctionKind(call.function_id);
  if (browser_function != MuonBuiltinBrowserFunctionKind::None) {
    DispatchBuiltinBrowserCall(browser_function, call);
    return;
  }
  auto modal_browser_id = 0;
  auto modal_owner_window_handle = CefWindowHandle{};
  auto invoke_args = call.encoded_args;
  const auto fs_dialog_call = IsFsDialogFunction(call.function_id);
  const auto fs_dialog_modal =
      fs_dialog_call && ReadFsDialogModal(call.encoded_args);
  if (fs_dialog_modal) {
    std::string error_message;
    if (!BeginModalBrowserViewDisable(call, &modal_browser_id,
                                      &modal_owner_window_handle,
                                      &error_message)) {
      RejectPluginCall(call, error_message);
      return;
    }
    if (!CreateFsDialogArgsWithOwnerBrowserId(
            call.encoded_args, modal_browser_id, modal_owner_window_handle,
            &invoke_args, &error_message)) {
      EndModalBrowserViewDisable(modal_browser_id);
      RejectPluginCall(call, error_message);
      return;
    }
  }
  const auto track_fs_dialog_call = fs_dialog_call;
  const auto fs_dialog_browser_id = call.context.browser_id;
  if (track_fs_dialog_call) {
    BeginPendingFsDialogCall(fs_dialog_browser_id);
  }
  plugin_runtime_->Invoke(
      call.context, call.function_id, call.call_id, invoke_args,
      std::move(payload),
      [self, frame = call.frame, call_id = call.call_id,
       invocation_context = call.context,
       modal_browser_id,
       track_fs_dialog_call,
       fs_dialog_browser_id](const MuonPluginCallResult& result) {
        if (modal_browser_id != 0) {
          self->EndModalBrowserViewDisable(modal_browser_id);
        }
        self->SendPluginResult(invocation_context, frame, call_id, result);
        if (track_fs_dialog_call) {
          self->EndPendingFsDialogCall(fs_dialog_browser_id);
        }
      });
}

void MuonClient::DispatchBuiltinBrowserCall(
    MuonBuiltinBrowserFunctionKind kind,
    const PendingPluginCall& call) {
  CEF_REQUIRE_UI_THREAD();

  std::string error_message;
  if (!call.browser) {
    std::ostringstream log;
    log << "MuonClient DispatchBuiltinBrowserCall no_browser kind="
        << GetMuonBuiltinBrowserFunctionKindName(kind);
    AppendMuonCloseDebugLog(log.str());
    RejectPluginCall(call, "Muon browser is unavailable");
    return;
  }
  {
    std::ostringstream log;
    log << "MuonClient DispatchBuiltinBrowserCall kind="
        << GetMuonBuiltinBrowserFunctionKindName(kind)
        << " browser=" << FormatMuonCloseDebugPointer(call.browser.get())
        << " browser_id=" << call.browser->GetIdentifier()
        << " tracked=" << browsers_by_id_.size()
        << " pending_fs=" << pending_fs_dialog_calls_
        << " quit_requested="
        << FormatMuonCloseDebugBool(message_loop_quit_requested_);
    AppendMuonCloseDebugLog(log.str());
  }

  MuonPluginCallResult result;
  result.success = true;
  result.value.type = MUON_TYPE_VOID;

  switch (kind) {
    case MuonBuiltinBrowserFunctionKind::Reload:
      SendPluginResult(call.context, call.frame, call.call_id, result);
      call.browser->Reload();
      break;
    case MuonBuiltinBrowserFunctionKind::HardReload:
      SendPluginResult(call.context, call.frame, call.call_id, result);
      call.browser->ReloadIgnoreCache();
      break;
    case MuonBuiltinBrowserFunctionKind::ToggleFullscreen:
      if (!GetBrowserViewAndWindow(call.browser, nullptr, nullptr,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      ToggleFullscreen(call.browser);
      break;
    case MuonBuiltinBrowserFunctionKind::EnterFullscreen:
      if (!GetBrowserViewAndWindow(call.browser, nullptr, nullptr,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      SetFullscreen(call.browser, true);
      break;
    case MuonBuiltinBrowserFunctionKind::ExitFullscreen:
      if (!GetBrowserViewAndWindow(call.browser, nullptr, nullptr,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      SetFullscreen(call.browser, false);
      break;
    case MuonBuiltinBrowserFunctionKind::ZoomIn:
      SendPluginResult(call.context, call.frame, call.call_id, result);
      ZoomBrowser(call.browser, CEF_ZOOM_COMMAND_IN);
      break;
    case MuonBuiltinBrowserFunctionKind::ZoomOut:
      SendPluginResult(call.context, call.frame, call.call_id, result);
      ZoomBrowser(call.browser, CEF_ZOOM_COMMAND_OUT);
      break;
    case MuonBuiltinBrowserFunctionKind::ResetZoom:
      SendPluginResult(call.context, call.frame, call.call_id, result);
      ZoomBrowser(call.browser, CEF_ZOOM_COMMAND_RESET);
      break;
    case MuonBuiltinBrowserFunctionKind::Show: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      ShowMuonWindow(window);
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Hide: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      window->Hide();
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Focus: {
      CefRefPtr<CefBrowserView> browser_view;
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, &browser_view, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      ShowMuonWindow(window);
      window->Activate();
      browser_view->RequestFocus();
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Blur: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      window->Deactivate();
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Minimize: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      window->Minimize();
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Maximize: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      window->Maximize();
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Restore: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      window->Restore();
      break;
    }
    case MuonBuiltinBrowserFunctionKind::SetTitleBarVisibility: {
      if (!call.encoded_args || call.encoded_args->GetSize() != 1 ||
          call.encoded_args->GetType(0) != VTYPE_BOOL) {
        RejectPluginCall(call, "Invalid title bar visibility");
        return;
      }
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      const auto visible = call.encoded_args->GetBool(0);
      SendPluginResult(call.context, call.frame, call.call_id, result);
      SetRegisteredMuonTitleBarVisibility(window, visible);
      SetRegisteredMuonTitleBarVisibilityForBrowser(call.browser->GetIdentifier(),
                                                   visible);
      break;
    }
    case MuonBuiltinBrowserFunctionKind::SetTitleBarIcon: {
      if (!call.encoded_args || call.encoded_args->GetSize() != 1) {
        RejectPluginCall(call, "Invalid title bar icon");
        return;
      }
      const auto arg_type = call.encoded_args->GetType(0);
      if (arg_type == VTYPE_NULL) {
        BeginTitleBarIconUpdateForBrowser(call.browser->GetIdentifier());
        if (!SetTitleBarIconForBrowser(call.browser, nullptr,
                                       &error_message)) {
          RejectPluginCall(call, error_message);
          return;
        }
        SendPluginResult(call.context, call.frame, call.call_id, result);
        break;
      }
      if (arg_type != VTYPE_STRING) {
        RejectPluginCall(call, "Invalid title bar icon");
        return;
      }
      MuonTitleBarIcon icon;
      const auto icon_path = call.encoded_args->GetString(0).ToString();
      const auto icon_loaded =
          IsCustomMuonTitleBar(title_bar_manifest_)
              ? LoadMuonTitleBarIconDataUrlFromStorage(
                    app_storage_, icon_path, &icon, &error_message)
              : LoadMuonTitleBarIconFromStorage(
                    app_storage_, icon_path, &icon, &error_message);
      if (!icon_loaded) {
        RejectPluginCall(call, error_message);
        return;
      }
      BeginTitleBarIconUpdateForBrowser(call.browser->GetIdentifier());
      if (!SetTitleBarIconForBrowser(call.browser, &icon, &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      break;
    }
    case MuonBuiltinBrowserFunctionKind::GetWindowBounds: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      const auto bounds = window->GetBounds();
      result.value.type = MUON_TYPE_STRING;
      result.value.string_value = "{\"x\":" + std::to_string(bounds.x) +
                                  ",\"y\":" + std::to_string(bounds.y) +
                                  ",\"width\":" +
                                  std::to_string(bounds.width) +
                                  ",\"height\":" +
                                  std::to_string(bounds.height) + "}";
      SendPluginResult(call.context, call.frame, call.call_id, result);
      break;
    }
    case MuonBuiltinBrowserFunctionKind::SetWindowBounds: {
      if (!call.encoded_args || call.encoded_args->GetSize() != 4) {
        RejectPluginCall(call, "Invalid window bounds");
        return;
      }
      for (auto index = size_t{0}; index < 4; ++index) {
        if (call.encoded_args->GetType(index) != VTYPE_INT) {
          RejectPluginCall(call, "Invalid window bounds");
          return;
        }
      }
      const auto x = call.encoded_args->GetInt(0);
      const auto y = call.encoded_args->GetInt(1);
      const auto width = call.encoded_args->GetInt(2);
      const auto height = call.encoded_args->GetInt(3);
      if (width <= 0 || height <= 0) {
        RejectPluginCall(call, "Invalid window bounds");
        return;
      }
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      window->SetBounds(CefRect(x, y, width, height));
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Close: {
      CefRefPtr<CefWindow> window;
      if (!GetBrowserViewAndWindow(call.browser, nullptr, &window,
                                   &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      const auto browser_id = call.browser->GetIdentifier();
      {
        std::ostringstream log;
        log << "MuonClient BuiltinClose begin browser_id=" << browser_id
            << " window=" << FormatMuonCloseDebugPointer(window.get())
            << " tracked=" << browsers_by_id_.size()
            << " pending_fs=" << pending_fs_dialog_calls_;
        AppendMuonCloseDebugLog(log.str());
      }
      const auto pending_iterator =
          pending_fs_dialog_calls_by_browser_.find(browser_id);
      if (pending_iterator != pending_fs_dialog_calls_by_browser_.end() &&
          pending_iterator->second > 0) {
        quit_message_loop_after_pending_fs_dialogs_ = true;
        quit_message_loop_after_pending_fs_dialogs_browser_id_ = browser_id;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      {
        std::ostringstream log;
        log << "MuonClient BuiltinClose post CloseWindowTask browser_id="
            << browser_id
            << " quit_after_pending="
            << FormatMuonCloseDebugBool(
                   quit_message_loop_after_pending_fs_dialogs_)
            << " quit_after_pending_browser_id="
            << quit_message_loop_after_pending_fs_dialogs_browser_id_;
        AppendMuonCloseDebugLog(log.str());
      }
      CefPostTask(TID_UI, new CloseMuonWindowTask(window));
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Shutdown: {
      if (!call.encoded_args || call.encoded_args->GetSize() != 1 ||
          call.encoded_args->GetType(0) != VTYPE_INT) {
        RejectPluginCall(call, "Invalid shutdown exit code");
        return;
      }
      const auto exit_code = call.encoded_args->GetInt(0);
      if (exit_code == kMuonRecycleExitCode) {
        RejectPluginCall(call, "Invalid shutdown exit code");
        return;
      }
      std::vector<CefRefPtr<CefBrowser>> browsers;
      auto should_start_shutdown = false;
      if (!PrepareShutdown(exit_code, &browsers, &should_start_shutdown,
                           &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      if (should_start_shutdown) {
        std::ostringstream log;
        log << "MuonClient BuiltinShutdown post ShutdownCloseTask browsers="
            << browsers.size() << " exit_code=" << exit_code;
        AppendMuonCloseDebugLog(log.str());
        CefPostTask(TID_UI,
                    new CloseMuonBrowsersForShutdownTask(std::move(browsers)));
      }
      break;
    }
    case MuonBuiltinBrowserFunctionKind::Recycle: {
      if (!call.encoded_args || call.encoded_args->GetSize() != 0) {
        RejectPluginCall(call, "Invalid recycle request");
        return;
      }
      std::vector<CefRefPtr<CefBrowser>> browsers;
      auto should_start_shutdown = false;
      if (!PrepareShutdown(kMuonRecycleExitCode, &browsers,
                           &should_start_shutdown, &error_message)) {
        RejectPluginCall(call, error_message);
        return;
      }
      SendPluginResult(call.context, call.frame, call.call_id, result);
      if (should_start_shutdown) {
        std::ostringstream log;
        log << "MuonClient BuiltinRecycle post ShutdownCloseTask browsers="
            << browsers.size();
        AppendMuonCloseDebugLog(log.str());
        CefPostTask(TID_UI,
                    new CloseMuonBrowsersForShutdownTask(std::move(browsers)));
      }
      break;
    }
    case MuonBuiltinBrowserFunctionKind::None:
      RejectPluginCall(call, "Unknown Muon browser function");
      return;
  }
}

void MuonClient::RejectPluginCall(const PendingPluginCall& call,
                                   const std::string& error_message) {
  MuonPluginCallResult result;
  result.success = false;
  result.error_message = error_message;
  SendPluginResult(call.context, call.frame, call.call_id, result);
}

void MuonClient::SendPluginResult(const MuonPluginInvocationContext& context,
                                   CefRefPtr<CefFrame> frame,
                                   int call_id,
                                   const MuonPluginCallResult& result) {
  CEF_REQUIRE_UI_THREAD();
  if (!frame || !frame->IsValid()) {
    return;
  }

  const auto message = CefProcessMessage::Create(kMuonPluginResultMessageName);
  const auto args = message->GetArgumentList();
  args->SetSize(5);
  args->SetInt(0, call_id);
  args->SetBool(1, result.success);
  args->SetInt(4, context.renderer_context_id);
  if (!result.success) {
    args->SetString(2, result.error_message);
    args->SetNull(3);
    frame->SendProcessMessage(PID_RENDERER, message);
    return;
  }

  args->SetInt(2, static_cast<int>(result.value.type));
  switch (result.value.type) {
    case MUON_TYPE_VOID:
      args->SetNull(3);
      break;
    case MUON_TYPE_BOOL:
      args->SetBool(3, result.value.bool_value);
      break;
    case MUON_TYPE_I8:
      args->SetInt(3, result.value.i8_value);
      break;
    case MUON_TYPE_U8:
      args->SetInt(3, result.value.u8_value);
      break;
    case MUON_TYPE_I16:
      args->SetInt(3, result.value.i16_value);
      break;
    case MUON_TYPE_U16:
      args->SetInt(3, result.value.u16_value);
      break;
    case MUON_TYPE_I32:
      args->SetInt(3, result.value.i32_value);
      break;
    case MUON_TYPE_U32:
      args->SetDouble(3, static_cast<double>(result.value.u32_value));
      break;
    case MUON_TYPE_I64:
      args->SetString(3, std::to_string(result.value.i64_value));
      break;
    case MUON_TYPE_U64:
      args->SetString(3, std::to_string(result.value.u64_value));
      break;
    case MUON_TYPE_F32:
      args->SetDouble(3, static_cast<double>(result.value.f32_value));
      break;
    case MUON_TYPE_F64:
      args->SetDouble(3, result.value.f64_value);
      break;
    case MUON_TYPE_POINTER:
      args->SetDouble(
          3,
          static_cast<double>(reinterpret_cast<uintptr_t>(
              result.value.pointer_value)));
      break;
    case MUON_TYPE_STRING:
      if (result.value.is_null) {
        args->SetNull(3);
        break;
      }
      args->SetString(3, result.value.string_value);
      break;
    case MUON_TYPE_FUNCTION: {
      if (result.value.is_null) {
        args->SetNull(3);
        break;
      }
      const auto proxy_id = plugin_runtime_
                                ? plugin_runtime_->RegisterPluginFunctionProxy(
                                      context, result.value.function_value,
                                      result.value.function_type)
                                : 0;
      if (proxy_id == 0) {
        args->SetBool(1, false);
        args->SetString(2, "Failed to register function result");
        args->SetNull(3);
        break;
      }
      const auto encoded_function = CefDictionaryValue::Create();
      encoded_function->SetString(kMuonFunctionValueKindKey,
                                  kMuonFunctionValueKindPluginProxy);
      encoded_function->SetInt(kMuonFunctionValueProxyIdKey,
                               static_cast<int>(proxy_id));
      encoded_function->SetString(kMuonFunctionValueTypeKey,
                                  CreateMuonTypeCanonicalKey(
                                      result.value.function_type));
      encoded_function->SetDictionary("type",
                                      CreateMuonTypeMetadataDictionary(
                                          result.value.function_type));
      args->SetDictionary(3, encoded_function);
      break;
    }
    case MUON_TYPE_BUFFER_VIEW: {
      MuonSharedBufferEntry entry;
      if (!result.has_shared_buffer_message ||
          !FindMuonSharedBufferEntry(
              result.shared_buffer_message.entries, 3, &entry) ||
          !result.shared_buffer_message.message) {
        args->SetBool(1, false);
        args->SetString(2, "Missing plugin buffer result payload");
        args->SetNull(3);
        break;
      }
      args->SetDictionary(3, CreateMuonSharedBufferPlaceholder(entry));
      break;
    }
    default:
      args->SetBool(1, false);
      args->SetString(2, "Unsupported plugin result type");
      args->SetNull(3);
      break;
  }
  if (result.value.type == MUON_TYPE_BUFFER_VIEW && args->GetBool(1) &&
      result.shared_buffer_message.message) {
    frame->SendProcessMessage(PID_RENDERER,
                              result.shared_buffer_message.message);
  }
  frame->SendProcessMessage(PID_RENDERER, message);
}
