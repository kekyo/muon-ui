/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_title_bar.h"

#include "yyjson.h"

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/views/cef_browser_view_delegate.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kMuonTitleBarTitle[] = "Muon Title Bar";
constexpr char kMuonTitleBarActionPrefix[] =
    "https://muon.internal/title-bar/";
constexpr char kMuonTitleBarExtraInfoKey[] = "muonInternalTitleBar";
constexpr int kMuonTitleBarDefaultWindowWidth = 1024;
constexpr int kMuonTitleBarMaxHeight = 96;
constexpr int kMuonTitleBarMaxControlsWidth = 512;

std::map<CefWindow*, MuonTitleBarController*> g_muon_title_bar_controllers;
std::map<CefWindow*, CefRefPtr<CefBrowserView>> g_muon_title_bar_views;
std::map<CefWindow*, int> g_muon_title_bar_browser_ids_by_window;
std::map<CefBrowserView*, CefRefPtr<CefWindow>>
    g_muon_title_bar_windows_by_browser_view;
std::map<int, MuonTitleBarController*>
    g_muon_title_bar_controllers_by_browser_id;
std::map<int, CefRefPtr<CefWindow>> g_muon_title_bar_windows_by_browser_id;
std::map<int, std::string> g_muon_title_bar_pending_titles_by_browser_id;
std::map<int, std::string>
    g_muon_title_bar_pending_icon_data_urls_by_browser_id;

static bool ReadJsonString(yyjson_val* root,
                           const char* key,
                           std::string* value) {
  auto* raw = yyjson_obj_get(root, key);
  if (!yyjson_is_str(raw)) {
    return false;
  }
  *value = yyjson_get_str(raw);
  return true;
}

static bool ReadJsonInt(yyjson_val* root,
                        const char* key,
                        int minimum,
                        int maximum,
                        int* value) {
  auto* raw = yyjson_obj_get(root, key);
  if (!yyjson_is_int(raw)) {
    return false;
  }
  const auto parsed = yyjson_get_int(raw);
  if (parsed < minimum || parsed > maximum) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

static bool IsUnreservedUrlByte(unsigned char value) {
  return std::isalnum(value) != 0 || value == '-' || value == '_' ||
         value == '.' || value == '~';
}

static std::string PercentEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const auto raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    if (IsUnreservedUrlByte(byte)) {
      encoded.push_back(static_cast<char>(byte));
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(kHex[(byte >> 4) & 0x0f]);
    encoded.push_back(kHex[byte & 0x0f]);
  }
  return encoded;
}

static std::string CreateJavaScriptStringLiteral(std::string_view value) {
  std::string literal;
  literal.reserve(value.size() + 2);
  literal.push_back('"');
  for (const auto raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '\\':
        literal += "\\\\";
        break;
      case '"':
        literal += "\\\"";
        break;
      case '\b':
        literal += "\\b";
        break;
      case '\f':
        literal += "\\f";
        break;
      case '\n':
        literal += "\\n";
        break;
      case '\r':
        literal += "\\r";
        break;
      case '\t':
        literal += "\\t";
        break;
      default:
        if (byte < 0x20) {
          char buffer[7] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
          literal += buffer;
        } else {
          literal.push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  literal.push_back('"');
  return literal;
}

static std::string CreateCssRgbColor(uint8_t red,
                                     uint8_t green,
                                     uint8_t blue) {
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", red, green, blue);
  return buffer;
}

static std::string CreateTitleBarBackgroundCss(
    const MuonTitleBarBackgroundColor& background_color) {
  if (!background_color.has_color) {
    return {};
  }
  const auto color = CreateCssRgbColor(
      background_color.red, background_color.green, background_color.blue);
  return std::string(R"CSS(
:root {
  --muon-titlebar-bg-top: )CSS") +
         color + R"CSS(;
  --muon-titlebar-bg-bottom: )CSS" + color +
         R"CSS(;
  --muon-titlebar-bg-inactive-top: )CSS" + color +
         R"CSS(;
  --muon-titlebar-bg-inactive-bottom: )CSS" + color +
         R"CSS(;
}
)CSS";
}

static std::string CreateTitleBarDocument(
    const MuonTitleBarManifest& manifest,
    const MuonTitleBarBackgroundColor& background_color) {
  return std::string(R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>)HTML") +
         kMuonTitleBarTitle + R"HTML(</title>
<style>
)HTML" + manifest.css +
         CreateTitleBarBackgroundCss(background_color) +
         R"HTML(
</style>
</head>
<body>
)HTML" + manifest.html +
         R"HTML(
<script>
)HTML" + manifest.js +
         R"HTML(
</script>
</body>
</html>
)HTML";
}

static std::string CreateDataUrl(const std::string& document) {
  return "data:text/html;charset=utf-8," + PercentEncode(document);
}

static bool ParseMuonTitleBarIconPath(const std::string& path,
                                      MuonAppStorageRequest* request,
                                      std::string* error_message) {
  if (request == nullptr || error_message == nullptr) {
    return false;
  }
  if (path.empty()) {
    *error_message = "Title bar icon path must not be empty";
    return false;
  }

  const auto scheme_separator = path.find("://");
  if (scheme_separator != std::string::npos) {
    if (path.rfind("asset://", 0) != 0) {
      *error_message = "Title bar icon path must use asset://main";
      return false;
    }
    const auto host_start = std::strlen("asset://");
    const auto path_start = path.find('/', host_start);
    if (path_start == std::string::npos || path_start == host_start ||
        path_start + 1 >= path.size()) {
      *error_message = "Title bar icon path must use asset://main/<path>";
      return false;
    }
    if (path.find_first_of("?#", path_start) != std::string::npos) {
      *error_message = "Title bar icon path must not contain a query or hash";
      return false;
    }
    request->host = path.substr(host_start, path_start - host_start);
    request->path = path.substr(path_start + 1);
    return true;
  }

  const auto colon = path.find(':');
  const auto slash = path.find('/');
  if (colon != std::string::npos &&
      (slash == std::string::npos || colon < slash)) {
    *error_message = "Title bar icon path must use asset://main";
    return false;
  }
  if (path.find_first_of("?#") != std::string::npos) {
    *error_message = "Title bar icon path must not contain a query or hash";
    return false;
  }

  request->host = "main";
  request->path = path;
  return true;
}

class MuonTitleBarViewDelegate final : public CefBrowserViewDelegate {
 public:
  explicit MuonTitleBarViewDelegate(int height) : height_(height) {}

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    (void)view;
    return CefSize(kMuonTitleBarDefaultWindowWidth, height_);
  }

  CefSize GetMinimumSize(CefRefPtr<CefView> view) override {
    (void)view;
    return CefSize(0, height_);
  }

  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }

 private:
  const int height_;

  IMPLEMENT_REFCOUNTING(MuonTitleBarViewDelegate);
  DISALLOW_COPY_AND_ASSIGN(MuonTitleBarViewDelegate);
};

static std::string ExtractTitleBarAction(const std::string& url) {
  if (url.rfind(kMuonTitleBarActionPrefix, 0) != 0) {
    return {};
  }
  const auto action_start = std::strlen(kMuonTitleBarActionPrefix);
  auto action_end = url.find_first_of("?#", action_start);
  if (action_end == std::string::npos) {
    action_end = url.size();
  }
  return url.substr(action_start, action_end - action_start);
}

static std::string ToLowerAscii(std::string value) {
  for (auto& character : value) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

static std::string GetCommandLineSwitchValue(
    const std::vector<std::string>& command_line,
    const char* name) {
  const auto switch_name = std::string("--") + name;
  const auto switch_prefix = switch_name + "=";
  std::string value;
  for (auto index = size_t{1}; index < command_line.size(); ++index) {
    if (command_line[index] == switch_name && index + 1 < command_line.size()) {
      value = command_line[index + 1];
      ++index;
    } else if (command_line[index].rfind(switch_prefix, 0) == 0) {
      value = command_line[index].substr(switch_prefix.size());
    }
  }
  return ToLowerAscii(value);
}

static bool StringEqualsIgnoreCase(const char* value, const char* expected) {
  if (value == nullptr) {
    return false;
  }
  return ToLowerAscii(value) == expected;
}

static bool IsNonEmptyString(const char* value) {
  return value != nullptr && value[0] != '\0';
}

}  // namespace

MuonTitleBarManifest CreateNativeMuonTitleBarManifest() {
  return {};
}

bool LoadMuonTitleBarIconFromPngBytes(const uint8_t* data,
                                      size_t size,
                                      const std::string& source,
                                      MuonTitleBarIcon* icon,
                                      std::string* error_message) {
  if (icon == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  const auto diagnostic_source = source.empty() ? "title bar icon" : source;
  if (data == nullptr || size == 0) {
    *error_message =
        "Title bar icon PNG must not be empty: " + diagnostic_source;
    return false;
  }

  auto image = CefImage::CreateImage();
  if (!image || !image->AddPNG(1.0f, data, size)) {
    *error_message =
        "Title bar icon must be a valid PNG: " + diagnostic_source;
    return false;
  }

  icon->image = image;
  icon->data_url =
      "data:image/png;base64," + CefBase64Encode(data, size).ToString();
  return true;
}

bool LoadMuonTitleBarIconFromStorage(std::shared_ptr<MuonAppStorage> storage,
                                     const std::string& path,
                                     MuonTitleBarIcon* icon,
                                     std::string* error_message) {
  if (icon == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (!storage) {
    *error_message = "Title bar icon storage is not available";
    return false;
  }
  MuonAppStorageRequest request;
  if (!ParseMuonTitleBarIconPath(path, &request, error_message)) {
    return false;
  }

  auto resource = storage->ReadResource(request);
  if (resource.status == MuonAppStorageStatus::kNotFound) {
    *error_message = "Title bar icon was not found: " + path;
    return false;
  }
  if (resource.status == MuonAppStorageStatus::kRejected) {
    *error_message = "Title bar icon path was rejected: " + path;
    return false;
  }
  if (resource.status != MuonAppStorageStatus::kOk) {
    *error_message = "Failed to read title bar icon: " + path;
    return false;
  }
  if (resource.data.empty()) {
    *error_message = "Title bar icon PNG must not be empty: " + path;
    return false;
  }

  return LoadMuonTitleBarIconFromPngBytes(
      resource.data.data(), resource.data.size(), path, icon, error_message);
}

bool IsCustomMuonTitleBar(const MuonTitleBarManifest& manifest) {
  return manifest.mode == MuonTitleBarMode::Custom && manifest.height > 0 &&
         manifest.controls_width > 0 && !manifest.html.empty() &&
         !manifest.css.empty() && !manifest.js.empty();
}

bool IsMuonNativeTitleBarSupported(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display) {
#if defined(OS_LINUX)
  const auto ozone_platform =
      GetCommandLineSwitchValue(command_line, "ozone-platform");
  if (ozone_platform == "x11") {
    return true;
  }
  if (ozone_platform == "wayland") {
    return false;
  }

  const auto ozone_platform_hint =
      GetCommandLineSwitchValue(command_line, "ozone-platform-hint");
  if (ozone_platform_hint == "x11") {
    return true;
  }
  if (ozone_platform_hint == "wayland") {
    return false;
  }

  if (StringEqualsIgnoreCase(xdg_session_type, "x11")) {
    return true;
  }
  if (StringEqualsIgnoreCase(xdg_session_type, "wayland") ||
      IsNonEmptyString(wayland_display)) {
    return false;
  }
  return IsNonEmptyString(display);
#else
  (void)command_line;
  (void)xdg_session_type;
  (void)wayland_display;
  (void)display;
  return true;
#endif
}

CefRefPtr<CefDictionaryValue> CreateMuonTitleBarExtraInfo() {
  auto extra_info = CefDictionaryValue::Create();
  extra_info->SetBool(kMuonTitleBarExtraInfoKey, true);
  return extra_info;
}

bool IsMuonTitleBarExtraInfo(CefRefPtr<CefDictionaryValue> extra_info) {
  return extra_info && extra_info->HasKey(kMuonTitleBarExtraInfoKey) &&
         extra_info->GetBool(kMuonTitleBarExtraInfoKey);
}

MuonTitleBarManifest ParseMuonTitleBarManifest(const char* manifest_json) {
  if (manifest_json == nullptr || manifest_json[0] == '\0') {
    return CreateNativeMuonTitleBarManifest();
  }

  yyjson_doc* document =
      yyjson_read(manifest_json, std::strlen(manifest_json), 0);
  if (document == nullptr) {
    return CreateNativeMuonTitleBarManifest();
  }
  yyjson_val* root = yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(document);
    return CreateNativeMuonTitleBarManifest();
  }

  std::string mode;
  if (!ReadJsonString(root, "mode", &mode)) {
    yyjson_doc_free(document);
    return CreateNativeMuonTitleBarManifest();
  }
  if (mode == "native") {
    yyjson_doc_free(document);
    return CreateNativeMuonTitleBarManifest();
  }
  if (mode != "custom") {
    yyjson_doc_free(document);
    return CreateNativeMuonTitleBarManifest();
  }

  MuonTitleBarManifest manifest;
  manifest.mode = MuonTitleBarMode::Custom;
  if (!ReadJsonInt(root, "height", 1, kMuonTitleBarMaxHeight,
                   &manifest.height) ||
      !ReadJsonInt(root, "controlsWidth", 1, kMuonTitleBarMaxControlsWidth,
                   &manifest.controls_width) ||
      !ReadJsonString(root, "html", &manifest.html) ||
      !ReadJsonString(root, "css", &manifest.css) ||
      !ReadJsonString(root, "js", &manifest.js) ||
      !IsCustomMuonTitleBar(manifest)) {
    yyjson_doc_free(document);
    return CreateNativeMuonTitleBarManifest();
  }

  yyjson_doc_free(document);
  return manifest;
}

MuonTitleBarController::MuonTitleBarController(
    MuonTitleBarManifest manifest,
    MuonTitleBarBackgroundColor background_color)
    : manifest_(std::move(manifest)),
      background_color_(background_color) {}

CefRefPtr<CefBrowserView> MuonTitleBarController::CreateBrowserView() {
  CefBrowserSettings settings;
  const auto document = CreateTitleBarDocument(manifest_, background_color_);
  return CefBrowserView::CreateBrowserView(
      this, CreateDataUrl(document), settings, CreateMuonTitleBarExtraInfo(),
      nullptr,
      new MuonTitleBarViewDelegate(manifest_.height));
}

void MuonTitleBarController::AttachWindow(CefRefPtr<CefWindow> window) {
  window_ = window.get();
  maximized_ = window_ && window_->IsMaximized();
  UpdateDraggableRegions();
  SendState();
}

void MuonTitleBarController::DetachWindow() {
  window_ = nullptr;
  browser_ = nullptr;
}

void MuonTitleBarController::SetTitle(const std::string& title) {
  title_ = title.empty() ? "Muon" : title;
  SendTitle();
}

void MuonTitleBarController::SetIconDataUrl(
    const std::string& icon_data_url) {
  icon_data_url_ = icon_data_url;
  SendIcon();
}

void MuonTitleBarController::SetActive(bool active) {
  active_ = active;
  SendState();
}

void MuonTitleBarController::SetMaximized(bool maximized) {
  maximized_ = maximized;
  SendState();
}

void MuonTitleBarController::SetVisible(bool visible) {
  visible_ = visible;
  UpdateDraggableRegions();
  if (visible_) {
    SendTitle();
    SendState();
    SendIcon();
  }
}

void MuonTitleBarController::UpdateDraggableRegions() {
  if (!window_ || !IsCustomMuonTitleBar(manifest_)) {
    return;
  }
  if (!visible_) {
    window_->SetDraggableRegions({});
    return;
  }

  auto bounds = window_->GetBounds();
  auto width = bounds.width;
  if (width <= 0) {
    width = kMuonTitleBarDefaultWindowWidth;
  }
  const auto controls_width = std::min(manifest_.controls_width, width);
  std::vector<CefDraggableRegion> regions;
  regions.emplace_back(CefRect(0, 0, width, manifest_.height), true);
  regions.emplace_back(
      CefRect(width - controls_width, 0, controls_width, manifest_.height),
      false);
  window_->SetDraggableRegions(regions);
}

int MuonTitleBarController::GetHeight() const {
  return manifest_.height;
}

int MuonTitleBarController::GetControlsWidth() const {
  return manifest_.controls_width;
}

CefRefPtr<CefLifeSpanHandler> MuonTitleBarController::GetLifeSpanHandler() {
  return this;
}

CefRefPtr<CefLoadHandler> MuonTitleBarController::GetLoadHandler() {
  return this;
}

CefRefPtr<CefRequestHandler> MuonTitleBarController::GetRequestHandler() {
  return this;
}

void MuonTitleBarController::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
}

void MuonTitleBarController::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  if (browser_ && browser_ == browser) {
    browser_ = nullptr;
  }
}

void MuonTitleBarController::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       int http_status_code) {
  (void)browser;
  (void)http_status_code;
  if (!frame || !frame->IsMain()) {
    return;
  }
  loaded_ = true;
  SendTitle();
  SendState();
  SendIcon();
}

bool MuonTitleBarController::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefRefPtr<CefRequest> request,
                                            bool user_gesture,
                                            bool is_redirect) {
  (void)browser;
  (void)frame;
  (void)user_gesture;
  (void)is_redirect;
  if (!request) {
    return false;
  }
  const auto action = ExtractTitleBarAction(request->GetURL().ToString());
  if (action.empty()) {
    return false;
  }
  HandleAction(action);
  return true;
}

void MuonTitleBarController::HandleAction(const std::string& action) {
  if (!window_) {
    return;
  }
  if (action == "minimize") {
    window_->Minimize();
    return;
  }
  if (action == "maximize") {
    if (window_->IsMaximized()) {
      window_->Restore();
      maximized_ = false;
    } else {
      window_->Maximize();
      maximized_ = true;
    }
    SendState();
    UpdateDraggableRegions();
    return;
  }
  if (action == "close") {
    window_->Close();
    return;
  }
}

void MuonTitleBarController::SendState() {
  ExecuteJavaScript(
      std::string("window.__muonTitleBar && "
                  "window.__muonTitleBar.setState({active:") +
      (active_ ? "true" : "false") + ",maximized:" +
      (maximized_ ? "true" : "false") + "});");
}

void MuonTitleBarController::SendTitle() {
  ExecuteJavaScript(
      std::string("window.__muonTitleBar && "
                  "window.__muonTitleBar.setTitle(") +
      CreateJavaScriptStringLiteral(title_) + ");");
}

void MuonTitleBarController::SendIcon() {
  ExecuteJavaScript(
      std::string("window.__muonTitleBar && "
                  "window.__muonTitleBar.setIcon(") +
      (icon_data_url_.empty() ? std::string("null")
                              : CreateJavaScriptStringLiteral(
                                    icon_data_url_)) +
      ");");
}

void MuonTitleBarController::ExecuteJavaScript(const std::string& source) {
  if (!loaded_ || !browser_) {
    return;
  }
  const auto frame = browser_->GetMainFrame();
  if (!frame) {
    return;
  }
  frame->ExecuteJavaScript(source, "muon-title-bar://internal", 0);
}

void RegisterMuonTitleBarController(
    CefRefPtr<CefWindow> window,
    CefRefPtr<MuonTitleBarController> controller,
    int browser_id) {
  if (!window || !controller) {
    return;
  }
  g_muon_title_bar_controllers[window.get()] = controller.get();
  if (browser_id <= 0) {
    return;
  }
  g_muon_title_bar_browser_ids_by_window[window.get()] = browser_id;
  g_muon_title_bar_controllers_by_browser_id[browser_id] = controller.get();
  g_muon_title_bar_windows_by_browser_id[browser_id] = window;
  const auto pending =
      g_muon_title_bar_pending_titles_by_browser_id.find(browser_id);
  if (pending != g_muon_title_bar_pending_titles_by_browser_id.end()) {
    controller->SetTitle(pending->second);
  }
  const auto pending_icon =
      g_muon_title_bar_pending_icon_data_urls_by_browser_id.find(browser_id);
  if (pending_icon !=
      g_muon_title_bar_pending_icon_data_urls_by_browser_id.end()) {
    controller->SetIconDataUrl(pending_icon->second);
  }
}

void RegisterMuonTitleBarView(CefRefPtr<CefWindow> window,
                              CefRefPtr<CefBrowserView> title_bar_view) {
  if (!window || !title_bar_view) {
    return;
  }
  g_muon_title_bar_views[window.get()] = title_bar_view;
}

static void RegisterMuonTitleBarBrowserForWindow(CefRefPtr<CefWindow> window,
                                                 int browser_id) {
  if (!window || browser_id <= 0) {
    return;
  }
  const auto controller = g_muon_title_bar_controllers.find(window.get());
  if (controller == g_muon_title_bar_controllers.end()) {
    return;
  }
  g_muon_title_bar_browser_ids_by_window[window.get()] = browser_id;
  g_muon_title_bar_controllers_by_browser_id[browser_id] = controller->second;
  g_muon_title_bar_windows_by_browser_id[browser_id] = window;
  const auto pending =
      g_muon_title_bar_pending_titles_by_browser_id.find(browser_id);
  if (pending != g_muon_title_bar_pending_titles_by_browser_id.end()) {
    controller->second->SetTitle(pending->second);
  }
  const auto pending_icon =
      g_muon_title_bar_pending_icon_data_urls_by_browser_id.find(browser_id);
  if (pending_icon !=
      g_muon_title_bar_pending_icon_data_urls_by_browser_id.end()) {
    controller->second->SetIconDataUrl(pending_icon->second);
  }
}

void RegisterMuonTitleBarBrowserView(CefRefPtr<CefWindow> window,
                                     CefRefPtr<CefBrowserView> browser_view) {
  if (!window || !browser_view) {
    return;
  }
  g_muon_title_bar_windows_by_browser_view[browser_view.get()] = window;
}

void RegisterMuonTitleBarBrowser(CefRefPtr<CefWindow> window, int browser_id) {
  if (!window || browser_id <= 0) {
    return;
  }
  RegisterMuonTitleBarBrowserForWindow(window, browser_id);
}

void RegisterMuonTitleBarBrowserViewBrowser(
    CefRefPtr<CefBrowserView> browser_view,
    int browser_id) {
  if (!browser_view || browser_id <= 0) {
    return;
  }
  const auto window =
      g_muon_title_bar_windows_by_browser_view.find(browser_view.get());
  if (window == g_muon_title_bar_windows_by_browser_view.end()) {
    return;
  }
  RegisterMuonTitleBarBrowserForWindow(window->second, browser_id);
}

void UnregisterMuonTitleBarController(CefRefPtr<CefWindow> window) {
  if (!window) {
    return;
  }
  const auto browser_id =
      g_muon_title_bar_browser_ids_by_window.find(window.get());
  if (browser_id != g_muon_title_bar_browser_ids_by_window.end()) {
    g_muon_title_bar_controllers_by_browser_id.erase(browser_id->second);
    g_muon_title_bar_windows_by_browser_id.erase(browser_id->second);
    g_muon_title_bar_pending_titles_by_browser_id.erase(browser_id->second);
    g_muon_title_bar_pending_icon_data_urls_by_browser_id.erase(
        browser_id->second);
    g_muon_title_bar_browser_ids_by_window.erase(browser_id);
  }
  for (auto iterator = g_muon_title_bar_windows_by_browser_view.begin();
       iterator != g_muon_title_bar_windows_by_browser_view.end();) {
    if (iterator->second.get() == window.get()) {
      iterator = g_muon_title_bar_windows_by_browser_view.erase(iterator);
    } else {
      ++iterator;
    }
  }
  g_muon_title_bar_controllers.erase(window.get());
  g_muon_title_bar_views.erase(window.get());
}

void ClearMuonTitleBarRegistrations() {
  for (const auto& controller : g_muon_title_bar_controllers) {
    if (controller.second != nullptr) {
      controller.second->DetachWindow();
    }
  }
  g_muon_title_bar_controllers.clear();
  g_muon_title_bar_views.clear();
  g_muon_title_bar_browser_ids_by_window.clear();
  g_muon_title_bar_windows_by_browser_view.clear();
  g_muon_title_bar_controllers_by_browser_id.clear();
  g_muon_title_bar_windows_by_browser_id.clear();
  g_muon_title_bar_pending_titles_by_browser_id.clear();
  g_muon_title_bar_pending_icon_data_urls_by_browser_id.clear();
}

void SetRegisteredMuonTitleBarTitle(CefRefPtr<CefWindow> window,
                                    const std::string& title) {
  if (!window) {
    return;
  }
  const auto iterator = g_muon_title_bar_controllers.find(window.get());
  if (iterator == g_muon_title_bar_controllers.end()) {
    return;
  }
  iterator->second->SetTitle(title);
}

void SetRegisteredMuonTitleBarIcon(CefRefPtr<CefWindow> window,
                                   CefRefPtr<CefImage> image,
                                   const std::string& icon_data_url) {
  if (!window) {
    return;
  }
  window->SetWindowIcon(image ? image : CefImage::CreateImage());
  const auto iterator = g_muon_title_bar_controllers.find(window.get());
  if (iterator == g_muon_title_bar_controllers.end()) {
    return;
  }
  iterator->second->SetIconDataUrl(icon_data_url);
}

void SetRegisteredMuonTitleBarTitleForBrowser(int browser_id,
                                              const std::string& title) {
  if (browser_id <= 0) {
    return;
  }
  g_muon_title_bar_pending_titles_by_browser_id[browser_id] = title;
  const auto iterator =
      g_muon_title_bar_controllers_by_browser_id.find(browser_id);
  if (iterator == g_muon_title_bar_controllers_by_browser_id.end()) {
    return;
  }
  iterator->second->SetTitle(title);
}

void SetRegisteredMuonTitleBarIconForBrowser(
    int browser_id,
    CefRefPtr<CefImage> image,
    const std::string& icon_data_url) {
  if (browser_id <= 0) {
    return;
  }
  g_muon_title_bar_pending_icon_data_urls_by_browser_id[browser_id] =
      icon_data_url;
  const auto window = GetRegisteredMuonWindowForBrowser(browser_id);
  SetRegisteredMuonTitleBarIcon(window, image, icon_data_url);
}

void SetRegisteredMuonTitleBarVisibility(CefRefPtr<CefWindow> window,
                                         bool visible) {
  if (!window) {
    return;
  }
  const auto view = g_muon_title_bar_views.find(window.get());
  if (view == g_muon_title_bar_views.end() || !view->second) {
    return;
  }
  const auto controller = g_muon_title_bar_controllers.find(window.get());
  if (controller != g_muon_title_bar_controllers.end() &&
      controller->second != nullptr) {
    controller->second->SetVisible(visible);
  }
  view->second->SetVisible(visible);
  view->second->InvalidateLayout();
  window->InvalidateLayout();
  window->Layout();
}

void SetRegisteredMuonTitleBarVisibilityForBrowser(int browser_id,
                                                   bool visible) {
  const auto window = GetRegisteredMuonWindowForBrowser(browser_id);
  SetRegisteredMuonTitleBarVisibility(window, visible);
}

CefRefPtr<CefWindow> GetRegisteredMuonWindowForBrowser(int browser_id) {
  const auto iterator = g_muon_title_bar_windows_by_browser_id.find(browser_id);
  return iterator == g_muon_title_bar_windows_by_browser_id.end()
             ? nullptr
             : iterator->second;
}
