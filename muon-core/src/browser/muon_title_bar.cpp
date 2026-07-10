/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_title_bar.h"

#include "browser/muon_icon.h"
#include "config/muon_linux_display_backend.h"
#include "log/muon_close_debug_log.h"
#include "yyjson.h"

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_parser.h"
#include "include/cef_request.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/wrapper/cef_helpers.h"

#if defined(OS_LINUX) && defined(CEF_X11)
#include "include/internal/cef_types_linux.h"

#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr char kMuonTitleBarTitle[] = "muon Title Bar";
constexpr char kMuonTitleBarActionPrefix[] =
    "https://muon.internal/title-bar/";
constexpr char kMuonTitleBarExtraInfoKey[] = "muonInternalTitleBar";
constexpr int kMuonTitleBarDefaultWindowWidth = 1024;
constexpr int kMuonTitleBarMaxHeight = 96;
constexpr int kMuonTitleBarMaxControlsWidth = 512;
constexpr int kMuonNativeTitleBarIconDipSize = 16;
constexpr int kMuonNativeAppIconDipSize = 32;

std::map<CefWindow*, MuonTitleBarController*> g_muon_title_bar_controllers;
std::map<CefWindow*, CefRefPtr<CefBrowserView>> g_muon_title_bar_views;
std::map<int, CefRefPtr<CefBrowserView>> g_muon_title_bar_views_by_browser_id;
std::map<CefWindow*, int> g_muon_title_bar_browser_ids_by_window;
std::map<CefBrowserView*, CefRefPtr<CefWindow>>
    g_muon_title_bar_windows_by_browser_view;
std::map<CefBrowserView*, int> g_muon_title_bar_browser_ids_by_browser_view;
std::map<int, MuonTitleBarController*>
    g_muon_title_bar_controllers_by_browser_id;
std::map<int, CefRefPtr<CefWindow>> g_muon_title_bar_windows_by_browser_id;
std::map<int, std::string> g_muon_title_bar_pending_titles_by_browser_id;
std::map<int, MuonTitleBarIcon> g_muon_title_bar_pending_icons_by_browser_id;
using MuonWindowDraggableRegionKey = std::uintptr_t;
std::map<CefWindow*, MuonWindowDraggableRegionKey>
    g_muon_draggable_region_keys_by_window;
std::map<MuonWindowDraggableRegionKey, MuonTitleBarController*>
    g_muon_title_bar_controllers_by_window_handle;
std::map<MuonWindowDraggableRegionKey, std::vector<CefDraggableRegion>>
    g_muon_title_bar_draggable_regions;
std::map<MuonWindowDraggableRegionKey, std::vector<CefDraggableRegion>>
    g_muon_applied_draggable_regions;

struct MuonPageDraggableRegions {
  CefRefPtr<CefBrowserView> browser_view;
  std::vector<CefDraggableRegion> regions;
};

struct MuonDecodedIconBitmap {
  std::vector<uint8_t> rgba;
  int pixel_width = 0;
  int pixel_height = 0;
};

std::map<MuonWindowDraggableRegionKey, MuonPageDraggableRegions>
    g_muon_page_draggable_regions;

#if defined(OS_LINUX) && defined(CEF_X11)
struct MuonMotifWmHints {
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
};

constexpr unsigned long kMuonMotifWmHintsDecorations = 1UL << 1;
constexpr unsigned long kMuonMotifWmDecorAll = 1UL << 0;

static void SetMuonX11TitleBarVisibility(CefRefPtr<CefWindow> window,
                                         bool visible) {
  if (!window) {
    return;
  }
  const auto handle = window->GetWindowHandle();
  if (handle == kNullWindowHandle) {
    return;
  }
  auto* display = cef_get_xdisplay();
  if (display == nullptr) {
    return;
  }
  const auto hints_atom = XInternAtom(display, "_MOTIF_WM_HINTS", False);
  if (hints_atom == None) {
    return;
  }

  MuonMotifWmHints hints = {};
  hints.flags = kMuonMotifWmHintsDecorations;
  hints.decorations = visible ? kMuonMotifWmDecorAll : 0;
  XChangeProperty(display, handle, hints_atom, hints_atom, 32,
                  PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&hints), 5);
  XFlush(display);
}
#else
static void SetMuonX11TitleBarVisibility(CefRefPtr<CefWindow> window,
                                         bool visible) {
  (void)window;
  (void)visible;
}
#endif

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

static bool ReadOptionalJsonBool(yyjson_val* root, const char* key) {
  auto* raw = yyjson_obj_get(root, key);
  return yyjson_is_bool(raw) && yyjson_get_bool(raw);
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

static std::string CreateImageDataUrl(const uint8_t* data,
                                      size_t size,
                                      const std::string& mime_type) {
  const auto effective_mime_type =
      mime_type.empty() ? std::string("application/octet-stream") : mime_type;
  return "data:" + effective_mime_type + ";base64," +
         CefBase64Encode(data, size).ToString();
}

static std::vector<float> GetMuonNativeIconPngScaleFactors(
    int pixel_width,
    int pixel_height,
    int dip_size) {
  auto scale_factors = std::vector<float>{1.0f};
  if (pixel_width == pixel_height && pixel_width >= dip_size * 2) {
    scale_factors.push_back(2.0f);
  }
  return scale_factors;
}

static bool DecodeMuonTitleBarIconPng(const std::vector<uint8_t>& png_data,
                                      MuonDecodedIconBitmap* bitmap) {
  if (png_data.empty() || bitmap == nullptr) {
    return false;
  }
  CefRefPtr<CefImage> image;
  const auto result = DecodeMuonIconPngWithinLimits(
      png_data.data(), png_data.size(), kMuonIconPngDecodeLimits,
      [&image, &png_data]() {
        image = CefImage::CreateImage();
        return image &&
               image->AddPNG(1.0f, png_data.data(), png_data.size());
      });
  if (result != MuonIconPngDecodeResult::Decoded) {
    return false;
  }

  auto pixel_width = 0;
  auto pixel_height = 0;
  auto data = image->GetAsBitmap(1.0f, CEF_COLOR_TYPE_RGBA_8888,
                                 CEF_ALPHA_TYPE_PREMULTIPLIED, pixel_width,
                                 pixel_height);
  if (!data || !data->IsValid() || pixel_width <= 0 || pixel_height <= 0) {
    return false;
  }

  const auto width = static_cast<size_t>(pixel_width);
  const auto height = static_cast<size_t>(pixel_height);
  if (width > std::numeric_limits<size_t>::max() / height / 4) {
    return false;
  }
  const auto expected_size = width * height * 4;
  if (data->GetSize() != expected_size) {
    return false;
  }

  auto rgba = std::vector<uint8_t>(expected_size);
  if (data->GetData(rgba.data(), rgba.size(), 0) != rgba.size()) {
    return false;
  }

  bitmap->rgba = std::move(rgba);
  bitmap->pixel_width = pixel_width;
  bitmap->pixel_height = pixel_height;
  return true;
}

static std::vector<uint8_t> ResizeMuonTitleBarIconBitmap(
    const MuonDecodedIconBitmap& source,
    int target_width,
    int target_height) {
  auto target = std::vector<uint8_t>(
      static_cast<size_t>(target_width) * static_cast<size_t>(target_height) *
      4);
  for (auto y = 0; y < target_height; ++y) {
    const auto source_y =
        (static_cast<int64_t>(y) * source.pixel_height) / target_height;
    for (auto x = 0; x < target_width; ++x) {
      const auto source_x =
          (static_cast<int64_t>(x) * source.pixel_width) / target_width;
      const auto source_offset =
          (static_cast<size_t>(source_y) *
               static_cast<size_t>(source.pixel_width) +
           static_cast<size_t>(source_x)) *
          4;
      const auto target_offset =
          (static_cast<size_t>(y) * static_cast<size_t>(target_width) +
           static_cast<size_t>(x)) *
          4;
      std::memcpy(target.data() + target_offset,
                  source.rgba.data() + source_offset, 4);
    }
  }
  return target;
}

static CefRefPtr<CefImage> CreateMuonNativeIconImage(
    const MuonDecodedIconBitmap& source,
    int dip_size) {
  auto image = CefImage::CreateImage();
  if (!image) {
    return nullptr;
  }
  const auto scale_factors = GetMuonNativeIconPngScaleFactors(
      source.pixel_width, source.pixel_height, dip_size);
  for (const auto scale_factor : scale_factors) {
    const auto pixel_size =
        static_cast<int>(static_cast<float>(dip_size) * scale_factor);
    const auto bitmap =
        ResizeMuonTitleBarIconBitmap(source, pixel_size, pixel_size);
    if (!image->AddBitmap(scale_factor, pixel_size, pixel_size,
                          CEF_COLOR_TYPE_RGBA_8888,
                          CEF_ALPHA_TYPE_PREMULTIPLIED, bitmap.data(),
                          bitmap.size())) {
      return nullptr;
    }
  }
  return image->IsEmpty() ? nullptr : image;
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

template <typename TWindowHandle>
static MuonWindowDraggableRegionKey GetWindowHandleDraggableRegionKey(
    TWindowHandle handle) {
  if constexpr (std::is_pointer_v<TWindowHandle>) {
    return reinterpret_cast<MuonWindowDraggableRegionKey>(handle);
  } else {
    return static_cast<MuonWindowDraggableRegionKey>(handle);
  }
}

static MuonWindowDraggableRegionKey GetWindowPointerDraggableRegionKey(
    CefWindow* window) {
  return reinterpret_cast<MuonWindowDraggableRegionKey>(window);
}

template <typename TValue>
static void MoveMuonDraggableRegionMapEntry(
    std::map<MuonWindowDraggableRegionKey, TValue>* map,
    MuonWindowDraggableRegionKey from,
    MuonWindowDraggableRegionKey to) {
  if (map == nullptr || from == to) {
    return;
  }
  const auto from_entry = map->find(from);
  if (from_entry == map->end()) {
    return;
  }
  if (map->find(to) == map->end()) {
    (*map)[to] = std::move(from_entry->second);
  }
  map->erase(from_entry);
}

static void MoveMuonDraggableRegionState(
    MuonWindowDraggableRegionKey from,
    MuonWindowDraggableRegionKey to) {
  MoveMuonDraggableRegionMapEntry(
      &g_muon_title_bar_controllers_by_window_handle, from, to);
  MoveMuonDraggableRegionMapEntry(&g_muon_title_bar_draggable_regions, from,
                                  to);
  MoveMuonDraggableRegionMapEntry(&g_muon_applied_draggable_regions, from, to);
  MoveMuonDraggableRegionMapEntry(&g_muon_page_draggable_regions, from, to);
}

static MuonWindowDraggableRegionKey GetDraggableRegionKey(CefWindow* window) {
  if (window == nullptr) {
    return 0;
  }
  const auto handle_key =
      GetWindowHandleDraggableRegionKey(window->GetWindowHandle());
  const auto pointer_key = GetWindowPointerDraggableRegionKey(window);
  const auto stored_key = g_muon_draggable_region_keys_by_window.find(window);
  if (handle_key != 0) {
    if (stored_key != g_muon_draggable_region_keys_by_window.end() &&
        stored_key->second != handle_key) {
      MoveMuonDraggableRegionState(stored_key->second, handle_key);
    }
    MoveMuonDraggableRegionState(pointer_key, handle_key);
    g_muon_draggable_region_keys_by_window[window] = handle_key;
    return handle_key;
  }
  const auto key = stored_key != g_muon_draggable_region_keys_by_window.end()
                       ? stored_key->second
                       : pointer_key;
  g_muon_draggable_region_keys_by_window[window] = key;
  return key;
}

static void EraseMuonDraggableRegionState(CefWindow* window) {
  if (window == nullptr) {
    return;
  }
  const auto stored_key = g_muon_draggable_region_keys_by_window.find(window);
  if (stored_key != g_muon_draggable_region_keys_by_window.end()) {
    g_muon_title_bar_draggable_regions.erase(stored_key->second);
    g_muon_page_draggable_regions.erase(stored_key->second);
    g_muon_applied_draggable_regions.erase(stored_key->second);
    g_muon_draggable_region_keys_by_window.erase(stored_key);
  }

  const auto pointer_key = GetWindowPointerDraggableRegionKey(window);
  g_muon_title_bar_draggable_regions.erase(pointer_key);
  g_muon_page_draggable_regions.erase(pointer_key);
  g_muon_applied_draggable_regions.erase(pointer_key);
}

static void AppendDraggableRegions(
    std::vector<CefDraggableRegion>* target,
    const std::vector<CefDraggableRegion>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

static void AppendBrowserViewDraggableRegions(
    std::vector<CefDraggableRegion>* target,
    CefBrowserView* browser_view,
    const std::vector<CefDraggableRegion>& source) {
  if (!browser_view) {
    return;
  }
  for (const auto& source_region : source) {
    auto region = source_region;
    CefPoint origin(region.bounds.x, region.bounds.y);
    if (!browser_view->ConvertPointToWindow(origin)) {
      continue;
    }
    region.bounds.x = origin.x;
    region.bounds.y = origin.y;
    target->push_back(region);
  }
}

static bool AreDraggableRegionsEqual(
    const std::vector<CefDraggableRegion>& left,
    const std::vector<CefDraggableRegion>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    const auto& left_region = left[i];
    const auto& right_region = right[i];
    if (left_region.draggable != right_region.draggable ||
        left_region.bounds.x != right_region.bounds.x ||
        left_region.bounds.y != right_region.bounds.y ||
        left_region.bounds.width != right_region.bounds.width ||
        left_region.bounds.height != right_region.bounds.height) {
      return false;
    }
  }
  return true;
}

static bool ContainsPoint(const CefRect& bounds, const CefPoint& point) {
  return point.x >= bounds.x && point.y >= bounds.y &&
         point.x < bounds.x + bounds.width &&
         point.y < bounds.y + bounds.height;
}

static const char* GetMuonTitleBarControlActionName(
    MuonTitleBarControlAction action) {
  switch (action) {
    case MuonTitleBarControlAction::Minimize:
      return "minimize";
    case MuonTitleBarControlAction::Maximize:
      return "maximize";
    case MuonTitleBarControlAction::Close:
      return "close";
    case MuonTitleBarControlAction::NoControl:
      return nullptr;
  }
  return nullptr;
}

static MuonTitleBarController* FindMuonTitleBarControllerByWindowHandle(
    CefWindowHandle window_handle) {
  if (window_handle == 0) {
    return nullptr;
  }
  const auto handle_key = GetWindowHandleDraggableRegionKey(window_handle);
  const auto controller =
      g_muon_title_bar_controllers_by_window_handle.find(handle_key);
  if (controller != g_muon_title_bar_controllers_by_window_handle.end()) {
    return controller->second;
  }
  for (const auto& entry : g_muon_title_bar_controllers) {
    auto* window = entry.first;
    if (window == nullptr || window->GetWindowHandle() != window_handle) {
      continue;
    }
    const auto key = GetDraggableRegionKey(window);
    const auto migrated =
        g_muon_title_bar_controllers_by_window_handle.find(key);
    return migrated != g_muon_title_bar_controllers_by_window_handle.end()
               ? migrated->second
               : entry.second;
  }
  return nullptr;
}

static bool GetPageDraggableRegionViewPoint(
    const MuonPageDraggableRegions& page_regions,
    const CefPoint& screen_point,
    CefPoint* view_point) {
  if (!page_regions.browser_view || view_point == nullptr) {
    return false;
  }

  auto converted_point = screen_point;
  if (!page_regions.browser_view->ConvertPointFromScreen(converted_point) ||
      !IsMuonPageDraggableRegionPoint(page_regions.regions, converted_point)) {
    return false;
  }
  *view_point = converted_point;
  return true;
}

static const MuonPageDraggableRegions* FindPageDraggableRegionsAtScreenPoint(
    CefWindowHandle window_handle,
    const CefPoint& screen_point,
    CefPoint* view_point) {
  std::vector<std::uintptr_t> registered_window_keys;
  registered_window_keys.reserve(g_muon_page_draggable_regions.size());
  for (const auto& page_regions : g_muon_page_draggable_regions) {
    registered_window_keys.push_back(page_regions.first);
  }

  const auto window_key =
      window_handle != 0 ? GetWindowHandleDraggableRegionKey(window_handle)
                         : 0;
  for (const auto key :
       GetMuonPageDraggableRegionSearchKeys(window_key,
                                            registered_window_keys)) {
    const auto page_regions = g_muon_page_draggable_regions.find(key);
    if (page_regions != g_muon_page_draggable_regions.end() &&
        GetPageDraggableRegionViewPoint(page_regions->second, screen_point,
                                        view_point)) {
      return &page_regions->second;
    }
  }
  return nullptr;
}

static bool ForwardPageDraggableRegionWheel(
    const MuonPageDraggableRegions& page_regions,
    const CefPoint& view_point,
    int delta_x,
    int delta_y,
    uint32_t modifiers) {
  if (!page_regions.browser_view || (delta_x == 0 && delta_y == 0)) {
    return false;
  }

  const auto browser = page_regions.browser_view->GetBrowser();
  if (!browser) {
    return false;
  }
  const auto host = browser->GetHost();
  if (!host) {
    return false;
  }

  CefMouseEvent event;
  event.x = view_point.x;
  event.y = view_point.y;
  event.modifiers = modifiers;
  host->SendMouseWheelEvent(event, delta_x, delta_y);
  return true;
}

static void ApplyMuonDraggableRegions(CefWindow* window) {
  if (window == nullptr) {
    return;
  }
  const auto key = GetDraggableRegionKey(window);

  std::vector<CefDraggableRegion> regions;
  const auto page_regions = g_muon_page_draggable_regions.find(key);
  if (page_regions != g_muon_page_draggable_regions.end()) {
    AppendBrowserViewDraggableRegions(
        &regions, page_regions->second.browser_view.get(),
        page_regions->second.regions);
  }
  const auto title_bar_regions =
      g_muon_title_bar_draggable_regions.find(key);
  if (title_bar_regions != g_muon_title_bar_draggable_regions.end()) {
    AppendDraggableRegions(&regions, title_bar_regions->second);
  }

  const auto applied = g_muon_applied_draggable_regions.find(key);
  if (applied != g_muon_applied_draggable_regions.end() &&
      AreDraggableRegionsEqual(applied->second, regions)) {
    return;
  }

  // Avoid disturbing native move loops with redundant region updates.
  if (regions.empty()) {
    if (applied == g_muon_applied_draggable_regions.end()) {
      return;
    }
    g_muon_applied_draggable_regions.erase(key);
  } else {
    g_muon_applied_draggable_regions[key] = regions;
  }
  window->SetDraggableRegions(regions);
}

static void SetMuonTitleBarDraggableRegions(
    CefWindow* window,
    const std::vector<CefDraggableRegion>& regions) {
  if (window == nullptr) {
    return;
  }
  const auto key = GetDraggableRegionKey(window);
  if (regions.empty()) {
    g_muon_title_bar_draggable_regions.erase(key);
  } else {
    g_muon_title_bar_draggable_regions[key] = regions;
  }
  ApplyMuonDraggableRegions(window);
}

static int GetRegisteredMuonTitleBarBrowserIdForWindow(CefWindow* window) {
  if (window == nullptr) {
    return 0;
  }
  const auto iterator = g_muon_title_bar_browser_ids_by_window.find(window);
  return iterator == g_muon_title_bar_browser_ids_by_window.end()
             ? 0
             : iterator->second;
}

static bool HasRegisteredMuonTitleBarController(CefWindow* window) {
  return window != nullptr &&
         g_muon_title_bar_controllers.find(window) !=
             g_muon_title_bar_controllers.end();
}

static MuonTitleBarController* GetRegisteredMuonTitleBarController(
    CefWindow* window) {
  if (window == nullptr) {
    return nullptr;
  }
  const auto controller = g_muon_title_bar_controllers.find(window);
  return controller == g_muon_title_bar_controllers.end()
             ? nullptr
             : controller->second;
}

static void RegisterMuonTitleBarWindowForBrowser(CefRefPtr<CefWindow> window,
                                                 int browser_id) {
  if (!window || browser_id <= 0) {
    std::ostringstream log;
    log << "TitleBar RegisterWindowForBrowser skipped window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
    return;
  }
  const auto current = g_muon_title_bar_windows_by_browser_id.find(browser_id);
  {
    std::ostringstream log;
    log << "TitleBar RegisterWindowForBrowser begin window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id
        << " current_window="
        << (current == g_muon_title_bar_windows_by_browser_id.end()
                ? "null"
                : FormatMuonCloseDebugPointer(current->second.get()))
        << " new_has_controller="
        << FormatMuonCloseDebugBool(
               HasRegisteredMuonTitleBarController(window.get()));
    AppendMuonCloseDebugLog(log.str());
  }
  if (current == g_muon_title_bar_windows_by_browser_id.end() ||
      current->second.get() == window.get() ||
      ShouldReplaceRegisteredMuonTitleBarWindowForBrowser(
          HasRegisteredMuonTitleBarController(current->second.get()),
          HasRegisteredMuonTitleBarController(window.get()))) {
    g_muon_title_bar_windows_by_browser_id[browser_id] = window;
    std::ostringstream log;
    log << "TitleBar RegisterWindowForBrowser stored window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
  }
}

static void ApplyPendingMuonTitleBarState(CefRefPtr<CefWindow> window,
                                          int browser_id) {
  if (!window || browser_id <= 0) {
    std::ostringstream log;
    log << "TitleBar ApplyPendingState skipped window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
    return;
  }
  const auto controller = g_muon_title_bar_controllers.find(window.get());
  const auto pending_title =
      g_muon_title_bar_pending_titles_by_browser_id.find(browser_id);
  {
    std::ostringstream log;
    log << "TitleBar ApplyPendingState window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id
        << " has_controller="
        << FormatMuonCloseDebugBool(
               controller != g_muon_title_bar_controllers.end())
        << " has_pending_title="
        << FormatMuonCloseDebugBool(
               pending_title !=
               g_muon_title_bar_pending_titles_by_browser_id.end())
        << " has_pending_icon="
        << FormatMuonCloseDebugBool(
               g_muon_title_bar_pending_icons_by_browser_id.find(browser_id) !=
               g_muon_title_bar_pending_icons_by_browser_id.end());
    AppendMuonCloseDebugLog(log.str());
  }
  if (controller != g_muon_title_bar_controllers.end() &&
      pending_title != g_muon_title_bar_pending_titles_by_browser_id.end()) {
    controller->second->SetTitle(pending_title->second);
  }
  const auto pending_icon =
      g_muon_title_bar_pending_icons_by_browser_id.find(browser_id);
  if (pending_icon != g_muon_title_bar_pending_icons_by_browser_id.end()) {
    SetRegisteredMuonTitleBarIcon(window, &pending_icon->second);
  }
}

static bool HasMuonTitleBarBrowserId(const std::vector<int>& browser_ids,
                                     int browser_id) {
  return std::find(browser_ids.begin(), browser_ids.end(), browser_id) !=
         browser_ids.end();
}

static void AddMuonTitleBarBrowserId(std::vector<int>* browser_ids,
                                     int browser_id) {
  if (browser_ids == nullptr || browser_id <= 0 ||
      HasMuonTitleBarBrowserId(*browser_ids, browser_id)) {
    return;
  }
  browser_ids->push_back(browser_id);
}

static std::vector<int> CollectRegisteredMuonTitleBarBrowserIds(
    CefWindow* requested_window,
    MuonTitleBarController* requested_controller) {
  auto browser_ids = std::vector<int>{};
  for (const auto& entry : g_muon_title_bar_browser_ids_by_window) {
    if (ShouldRemoveRegisteredMuonTitleBarController(
            entry.first, requested_window,
            GetRegisteredMuonTitleBarController(entry.first),
            requested_controller)) {
      AddMuonTitleBarBrowserId(&browser_ids, entry.second);
    }
  }
  for (const auto& entry : g_muon_title_bar_controllers_by_browser_id) {
    if (ShouldRemoveRegisteredMuonTitleBarController(
            nullptr, requested_window, entry.second, requested_controller)) {
      AddMuonTitleBarBrowserId(&browser_ids, entry.first);
    }
  }
  for (const auto& entry : g_muon_title_bar_windows_by_browser_id) {
    const auto registered_window = entry.second.get();
    if (ShouldRemoveRegisteredMuonTitleBarController(
            registered_window, requested_window,
            GetRegisteredMuonTitleBarController(registered_window),
            requested_controller)) {
      AddMuonTitleBarBrowserId(&browser_ids, entry.first);
    }
  }
  for (const auto& entry : g_muon_title_bar_browser_ids_by_browser_view) {
    const auto window =
        g_muon_title_bar_windows_by_browser_view.find(entry.first);
    const auto registered_window =
        window == g_muon_title_bar_windows_by_browser_view.end()
            ? nullptr
            : window->second.get();
    if (ShouldRemoveRegisteredMuonTitleBarController(
            registered_window, requested_window,
            GetRegisteredMuonTitleBarController(registered_window),
            requested_controller)) {
      AddMuonTitleBarBrowserId(&browser_ids, entry.second);
    }
  }
  return browser_ids;
}

static void EraseRegisteredMuonTitleBarBrowserState(
    const std::vector<int>& browser_ids) {
  {
    std::ostringstream log;
    log << "TitleBar EraseBrowserState browser_ids=";
    for (size_t index = 0; index < browser_ids.size(); ++index) {
      if (index != 0) {
        log << ",";
      }
      log << browser_ids[index];
    }
    log << " count=" << browser_ids.size();
    AppendMuonCloseDebugLog(log.str());
  }
  for (const auto browser_id : browser_ids) {
    g_muon_title_bar_controllers_by_browser_id.erase(browser_id);
    g_muon_title_bar_windows_by_browser_id.erase(browser_id);
    g_muon_title_bar_views_by_browser_id.erase(browser_id);
    g_muon_title_bar_pending_titles_by_browser_id.erase(browser_id);
    g_muon_title_bar_pending_icons_by_browser_id.erase(browser_id);
  }
  for (auto iterator = g_muon_title_bar_browser_ids_by_window.begin();
       iterator != g_muon_title_bar_browser_ids_by_window.end();) {
    if (HasMuonTitleBarBrowserId(browser_ids, iterator->second)) {
      iterator = g_muon_title_bar_browser_ids_by_window.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (auto iterator = g_muon_title_bar_browser_ids_by_browser_view.begin();
       iterator != g_muon_title_bar_browser_ids_by_browser_view.end();) {
    if (HasMuonTitleBarBrowserId(browser_ids, iterator->second)) {
      g_muon_title_bar_windows_by_browser_view.erase(iterator->first);
      iterator = g_muon_title_bar_browser_ids_by_browser_view.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

static void EraseRegisteredMuonTitleBarController(
    CefWindow* requested_window,
    MuonTitleBarController* requested_controller) {
  const auto browser_ids = CollectRegisteredMuonTitleBarBrowserIds(
      requested_window, requested_controller);
  {
    std::ostringstream log;
    log << "TitleBar EraseController begin requested_window="
        << FormatMuonCloseDebugPointer(requested_window)
        << " requested_controller="
        << FormatMuonCloseDebugPointer(requested_controller)
        << " collected_browser_ids=";
    for (size_t index = 0; index < browser_ids.size(); ++index) {
      if (index != 0) {
        log << ",";
      }
      log << browser_ids[index];
    }
    log << " controllers=" << g_muon_title_bar_controllers.size()
        << " controllers_by_browser="
        << g_muon_title_bar_controllers_by_browser_id.size()
        << " windows_by_browser="
        << g_muon_title_bar_windows_by_browser_id.size()
        << " windows_by_browser_view="
        << g_muon_title_bar_windows_by_browser_view.size();
    AppendMuonCloseDebugLog(log.str());
  }
  EraseRegisteredMuonTitleBarBrowserState(browser_ids);

  for (auto iterator = g_muon_title_bar_windows_by_browser_view.begin();
       iterator != g_muon_title_bar_windows_by_browser_view.end();) {
    const auto registered_window = iterator->second.get();
    if (ShouldRemoveRegisteredMuonTitleBarController(
            registered_window, requested_window,
            GetRegisteredMuonTitleBarController(registered_window),
            requested_controller)) {
      g_muon_title_bar_browser_ids_by_browser_view.erase(iterator->first);
      iterator = g_muon_title_bar_windows_by_browser_view.erase(iterator);
    } else {
      ++iterator;
    }
  }

  if (requested_window != nullptr) {
    const auto window_handle = requested_window->GetWindowHandle();
    if (window_handle != 0) {
      g_muon_title_bar_controllers_by_window_handle.erase(
          GetWindowHandleDraggableRegionKey(window_handle));
    }
  }
  if (requested_controller != nullptr) {
    for (auto iterator =
             g_muon_title_bar_controllers_by_window_handle.begin();
         iterator != g_muon_title_bar_controllers_by_window_handle.end();) {
      if (iterator->second == requested_controller) {
        iterator = g_muon_title_bar_controllers_by_window_handle.erase(
            iterator);
      } else {
        ++iterator;
      }
    }
  }

  for (auto iterator = g_muon_title_bar_controllers.begin();
       iterator != g_muon_title_bar_controllers.end();) {
    const auto registered_window = iterator->first;
    if (ShouldRemoveRegisteredMuonTitleBarController(
            registered_window, requested_window, iterator->second,
            requested_controller)) {
      g_muon_title_bar_views.erase(registered_window);
      EraseMuonDraggableRegionState(registered_window);
      iterator = g_muon_title_bar_controllers.erase(iterator);
    } else {
      ++iterator;
    }
  }
  if (requested_window != nullptr) {
    g_muon_title_bar_views.erase(requested_window);
    EraseMuonDraggableRegionState(requested_window);
  }
  {
    std::ostringstream log;
    log << "TitleBar EraseController end requested_window="
        << FormatMuonCloseDebugPointer(requested_window)
        << " requested_controller="
        << FormatMuonCloseDebugPointer(requested_controller)
        << " controllers=" << g_muon_title_bar_controllers.size()
        << " controllers_by_browser="
        << g_muon_title_bar_controllers_by_browser_id.size()
        << " windows_by_browser="
        << g_muon_title_bar_windows_by_browser_id.size()
        << " windows_by_browser_view="
        << g_muon_title_bar_windows_by_browser_view.size();
    AppendMuonCloseDebugLog(log.str());
  }
}

}  // namespace

MuonTitleBarManifest CreateNativeMuonTitleBarManifest() {
  return {};
}

std::vector<float> GetMuonTitleBarIconPngScaleFactors(int pixel_width,
                                                      int pixel_height) {
  return GetMuonNativeIconPngScaleFactors(
      pixel_width, pixel_height, kMuonNativeTitleBarIconDipSize);
}

static MuonIconPngDecodeResult LoadMuonTitleBarIconFromPngBytesWithResult(
    const uint8_t* data,
    size_t size,
    const std::string& source,
    MuonTitleBarIcon* icon,
    std::string* error_message);

bool LoadMuonTitleBarIconFromPngBytes(const uint8_t* data,
                                      size_t size,
                                      const std::string& source,
                                      MuonTitleBarIcon* icon,
                                      std::string* error_message) {
  return LoadMuonTitleBarIconFromPngBytesWithResult(
             data, size, source, icon, error_message) ==
         MuonIconPngDecodeResult::Decoded;
}

static MuonIconPngDecodeResult LoadMuonTitleBarIconFromPngBytesWithResult(
    const uint8_t* data,
    size_t size,
    const std::string& source,
    MuonTitleBarIcon* icon,
    std::string* error_message) {
  if (icon == nullptr || error_message == nullptr) {
    return MuonIconPngDecodeResult::DecodeFailed;
  }
  error_message->clear();
  const auto diagnostic_source = source.empty() ? "title bar icon" : source;
  if (data == nullptr || size == 0) {
    *error_message =
        "Title bar icon PNG must not be empty: " + diagnostic_source;
    return MuonIconPngDecodeResult::InvalidMetadata;
  }

  CefRefPtr<CefImage> image;
  const auto result = DecodeMuonIconPngWithinLimits(
      data, size, kMuonIconPngDecodeLimits, [&image, data, size]() {
        image = CefImage::CreateImage();
        return image && image->AddPNG(1.0f, data, size);
      });
  if (result == MuonIconPngDecodeResult::OverBudget) {
    *error_message =
        "Title bar icon PNG exceeds the supported 256x256 pixel or 1 MiB "
        "encoded limit: " +
        diagnostic_source;
    return result;
  }
  if (result != MuonIconPngDecodeResult::Decoded) {
    *error_message =
        "Title bar icon must be a valid PNG: " + diagnostic_source;
    return result;
  }

  auto loaded_icon = MuonTitleBarIcon{};
  loaded_icon.png_data.assign(data, data + size);
  loaded_icon.data_url =
      "data:image/png;base64," + CefBase64Encode(data, size).ToString();
  *icon = std::move(loaded_icon);
  return MuonIconPngDecodeResult::Decoded;
}

bool LoadMuonTitleBarIconFromImageBytes(const uint8_t* data,
                                        size_t size,
                                        const std::string& mime_type,
                                        const std::string& source,
                                        bool require_png,
                                        MuonTitleBarIcon* icon,
                                        std::string* error_message) {
  if (icon == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (data == nullptr || size == 0) {
    const auto diagnostic_source = source.empty() ? "title bar icon" : source;
    *error_message = "Title bar icon image must not be empty: " +
                     diagnostic_source;
    return false;
  }

  std::string png_error;
  const auto png_result = LoadMuonTitleBarIconFromPngBytesWithResult(
      data, size, source, icon, &png_error);
  if (png_result == MuonIconPngDecodeResult::Decoded) {
    return true;
  }
  if (require_png || png_result != MuonIconPngDecodeResult::NotPng) {
    *error_message = png_error;
    return false;
  }

  *icon = MuonTitleBarIcon{};
  icon->data_url = CreateImageDataUrl(data, size, mime_type);
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

bool LoadMuonTitleBarIconDataUrlFromStorage(
    std::shared_ptr<MuonAppStorage> storage,
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

  return LoadMuonTitleBarIconFromImageBytes(
      resource.data.data(), resource.data.size(), resource.mime_type, path,
      false, icon, error_message);
}

MuonWindowIconUpdateBehavior GetMuonWindowIconUpdateBehavior(
    bool has_native_png_data,
    const std::string& icon_data_url) {
  if (has_native_png_data) {
    return {MuonWindowIconAction::Set, MuonWindowIconAction::Set};
  }
  if (!icon_data_url.empty()) {
    return {MuonWindowIconAction::Keep, MuonWindowIconAction::Keep};
  }
  return {MuonWindowIconAction::Clear, MuonWindowIconAction::Clear};
}

bool IsCustomMuonTitleBar(const MuonTitleBarManifest& manifest) {
  return manifest.mode == MuonTitleBarMode::Custom && manifest.height > 0 &&
         manifest.controls_width > 0 && !manifest.html.empty() &&
         !manifest.css.empty() && !manifest.js.empty();
}

int GetMuonResolvedTitleBarBrowserId(int browser_id,
                                     int registered_browser_id) {
  if (browser_id > 0) {
    return browser_id;
  }
  return registered_browser_id > 0 ? registered_browser_id : 0;
}

bool ShouldReplaceRegisteredMuonTitleBarWindowForBrowser(
    bool current_has_controller,
    bool candidate_has_controller) {
  return !current_has_controller || candidate_has_controller;
}

bool ShouldRemoveRegisteredMuonTitleBarController(
    const CefWindow* registered_window,
    const CefWindow* requested_window,
    const MuonTitleBarController* registered_controller,
    const MuonTitleBarController* requested_controller) {
  return (registered_window != nullptr &&
          registered_window == requested_window) ||
         (registered_controller != nullptr &&
          registered_controller == requested_controller);
}

MuonTitleBarControlAction GetMuonTitleBarControlActionAtWindowPoint(
    bool native_window_controls,
    int title_bar_height,
    int controls_width,
    const CefSize& window_size,
    const CefPoint& window_point) {
  if (!native_window_controls || title_bar_height <= 0 || controls_width <= 0 ||
      window_size.width <= 0 || window_size.height <= 0) {
    return MuonTitleBarControlAction::NoControl;
  }
  if (window_point.x < 0 || window_point.y < 0 ||
      window_point.x >= window_size.width ||
      window_point.y >= window_size.height ||
      window_point.y >= title_bar_height) {
    return MuonTitleBarControlAction::NoControl;
  }

  const auto effective_controls_width =
      std::min(controls_width, window_size.width);
  const auto controls_x = window_size.width - effective_controls_width;
  if (window_point.x < controls_x) {
    return MuonTitleBarControlAction::NoControl;
  }

  const auto control_x = window_point.x - controls_x;
  const auto control_index = (control_x * 3) / effective_controls_width;
  if (control_index == 0) {
    return MuonTitleBarControlAction::Minimize;
  }
  if (control_index == 1) {
    return MuonTitleBarControlAction::Maximize;
  }
  return MuonTitleBarControlAction::Close;
}

bool IsMuonNativeTitleBarSupported(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display) {
#if defined(OS_LINUX)
  return ResolveMuonLinuxDisplayBackend(command_line, xdg_session_type,
                                        wayland_display, display) ==
         kMuonLinuxDisplayBackendX11;
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
  manifest.native_window_controls =
      ReadOptionalJsonBool(root, "nativeWindowControls");
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
  window_handle_ = window ? window->GetWindowHandle() : 0;
  maximized_ = window && window->IsMaximized();
  UpdateDraggableRegions(window);
  SendState();
}

void MuonTitleBarController::DetachWindow() {
  window_handle_ = 0;
  browser_ = nullptr;
}

void MuonTitleBarController::SetTitle(const std::string& title) {
  title_ = title.empty() ? "muon" : title;
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

void MuonTitleBarController::SetNativeHoveredControl(
    MuonTitleBarControlAction action) {
  if (native_hovered_control_ == action) {
    if (action != MuonTitleBarControlAction::NoControl) {
      SendNativeHover();
    }
    return;
  }
  native_hovered_control_ = action;
  SendNativeHover();
}

void MuonTitleBarController::SetNativePressedControl(
    MuonTitleBarControlAction action) {
  if (native_pressed_control_ == action) {
    if (action != MuonTitleBarControlAction::NoControl) {
      SendNativePressed();
    }
    return;
  }
  native_pressed_control_ = action;
  SendNativePressed();
}

void MuonTitleBarController::SetVisible(bool visible) {
  visible_ = visible;
  if (!visible_) {
    SetNativePressedControl(MuonTitleBarControlAction::NoControl);
    SetNativeHoveredControl(MuonTitleBarControlAction::NoControl);
  }
  const auto title_bar_view = ResolveTitleBarView();
  auto window = CefRefPtr<CefWindow>();
  if (title_bar_view) {
    title_bar_view->SetVisible(visible);
    title_bar_view->InvalidateLayout();
    window = title_bar_view->GetWindow();
  }
  if (!window) {
    window = ResolveWindow();
  }
  if (window) {
    window->InvalidateLayout();
    window->Layout();
  }
  UpdateDraggableRegions(window);
  if (visible_) {
    SendTitle();
    SendState();
    SendIcon();
  }
}

void MuonTitleBarController::UpdateDraggableRegions(
    CefRefPtr<CefWindow> window) {
  if (!window) {
    window = ResolveWindow();
  }
  if (!window || !IsCustomMuonTitleBar(manifest_)) {
    return;
  }
  if (!visible_) {
    SetMuonTitleBarDraggableRegions(window.get(), {});
    return;
  }

  auto bounds = window->GetBounds();
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
  SetMuonTitleBarDraggableRegions(window.get(), regions);
}

int MuonTitleBarController::GetHeight() const {
  return manifest_.height;
}

int MuonTitleBarController::GetControlsWidth() const {
  return manifest_.controls_width;
}

bool MuonTitleBarController::CanHandleNativeWindowControls() const {
  return visible_ && manifest_.native_window_controls &&
         IsCustomMuonTitleBar(manifest_);
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
  {
    std::ostringstream log;
    log << "TitleBarController OnAfterCreated controller="
        << FormatMuonCloseDebugPointer(this)
        << " browser=" << FormatMuonCloseDebugPointer(browser.get())
        << " browser_id=" << (browser ? browser->GetIdentifier() : 0)
        << " window_handle=" << GetWindowHandleDraggableRegionKey(
               window_handle_);
    AppendMuonCloseDebugLog(log.str());
  }
  browser_ = browser;
  SetVisible(visible_);
}

void MuonTitleBarController::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  {
    std::ostringstream log;
    log << "TitleBarController OnBeforeClose controller="
        << FormatMuonCloseDebugPointer(this)
        << " browser=" << FormatMuonCloseDebugPointer(browser.get())
        << " browser_id=" << (browser ? browser->GetIdentifier() : 0)
        << " current_browser="
        << FormatMuonCloseDebugPointer(browser_.get())
        << " window_handle=" << GetWindowHandleDraggableRegionKey(
               window_handle_);
    AppendMuonCloseDebugLog(log.str());
  }
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
  SendNativeHover();
  SendNativePressed();
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
  const auto window = ResolveWindow();
  {
    std::ostringstream log;
    log << "TitleBarController HandleAction controller="
        << FormatMuonCloseDebugPointer(this)
        << " action=" << action
        << " window=" << FormatMuonCloseDebugPointer(window.get())
        << " window_handle=" << GetWindowHandleDraggableRegionKey(
               window_handle_)
        << " browser=" << FormatMuonCloseDebugPointer(browser_.get())
        << " browser_id=" << (browser_ ? browser_->GetIdentifier() : 0);
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window) {
    return;
  }
  if (action == "minimize") {
    window->Minimize();
    return;
  }
  if (action == "maximize") {
    if (window->IsMaximized()) {
      window->Restore();
      maximized_ = false;
    } else {
      window->Maximize();
      maximized_ = true;
    }
    SendState();
    UpdateDraggableRegions(window);
    return;
  }
  if (action == "close") {
    {
      std::ostringstream log;
      log << "TitleBarController HandleAction close window_close controller="
          << FormatMuonCloseDebugPointer(this)
          << " window=" << FormatMuonCloseDebugPointer(window.get())
          << " browser=" << FormatMuonCloseDebugPointer(browser_.get())
          << " browser_id=" << (browser_ ? browser_->GetIdentifier() : 0);
      AppendMuonCloseDebugLog(log.str());
    }
    window->Close();
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

void MuonTitleBarController::SendNativeHover() {
  const auto* action_name =
      GetMuonTitleBarControlActionName(native_hovered_control_);
  ExecuteJavaScript(
      std::string("window.__muonTitleBar && "
                  "window.__muonTitleBar.setNativeHover(") +
      (action_name == nullptr ? std::string("null")
                              : CreateJavaScriptStringLiteral(action_name)) +
      ");");
}

void MuonTitleBarController::SendNativePressed() {
  const auto* action_name =
      GetMuonTitleBarControlActionName(native_pressed_control_);
  ExecuteJavaScript(
      std::string("window.__muonTitleBar && "
                  "window.__muonTitleBar.setNativePressed(") +
      (action_name == nullptr ? std::string("null")
                              : CreateJavaScriptStringLiteral(action_name)) +
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

CefRefPtr<CefBrowserView> MuonTitleBarController::ResolveTitleBarView() const {
  if (!browser_) {
    return nullptr;
  }
  return CefBrowserView::GetForBrowser(browser_);
}

CefRefPtr<CefWindow> MuonTitleBarController::ResolveWindow() const {
  const auto title_bar_view = ResolveTitleBarView();
  if (!title_bar_view) {
    return nullptr;
  }
  return title_bar_view->GetWindow();
}

void RegisterMuonTitleBarController(
    CefRefPtr<CefWindow> window,
    CefRefPtr<MuonTitleBarController> controller,
    int browser_id) {
  {
    std::ostringstream log;
    log << "TitleBar RegisterController begin window="
        << FormatMuonCloseDebugPointer(window.get())
        << " controller=" << FormatMuonCloseDebugPointer(controller.get())
        << " browser_id_arg=" << browser_id
        << " existing_browser_id="
        << GetRegisteredMuonTitleBarBrowserIdForWindow(window.get());
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window || !controller) {
    return;
  }
  g_muon_title_bar_controllers[window.get()] = controller.get();
  g_muon_title_bar_controllers_by_window_handle
      [GetDraggableRegionKey(window.get())] = controller.get();
  const auto effective_browser_id = GetMuonResolvedTitleBarBrowserId(
      browser_id, GetRegisteredMuonTitleBarBrowserIdForWindow(window.get()));
  if (effective_browser_id <= 0) {
    std::ostringstream log;
    log << "TitleBar RegisterController no_effective_browser window="
        << FormatMuonCloseDebugPointer(window.get())
        << " controller=" << FormatMuonCloseDebugPointer(controller.get());
    AppendMuonCloseDebugLog(log.str());
    return;
  }
  g_muon_title_bar_browser_ids_by_window[window.get()] = effective_browser_id;
  g_muon_title_bar_controllers_by_browser_id[effective_browser_id] =
      controller.get();
  const auto title_bar_view = g_muon_title_bar_views.find(window.get());
  if (title_bar_view != g_muon_title_bar_views.end() &&
      title_bar_view->second) {
    g_muon_title_bar_views_by_browser_id[effective_browser_id] =
        title_bar_view->second;
  }
  RegisterMuonTitleBarWindowForBrowser(window, effective_browser_id);
  ApplyPendingMuonTitleBarState(window, effective_browser_id);
  {
    std::ostringstream log;
    log << "TitleBar RegisterController end window="
        << FormatMuonCloseDebugPointer(window.get())
        << " controller=" << FormatMuonCloseDebugPointer(controller.get())
        << " effective_browser_id=" << effective_browser_id;
    AppendMuonCloseDebugLog(log.str());
  }
}

void RegisterMuonTitleBarView(CefRefPtr<CefWindow> window,
                              CefRefPtr<CefBrowserView> title_bar_view) {
  {
    std::ostringstream log;
    log << "TitleBar RegisterView window="
        << FormatMuonCloseDebugPointer(window.get())
        << " title_bar_view="
        << FormatMuonCloseDebugPointer(title_bar_view.get());
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window || !title_bar_view) {
    return;
  }
  g_muon_title_bar_views[window.get()] = title_bar_view;
}

static void RegisterMuonTitleBarBrowserForWindow(CefRefPtr<CefWindow> window,
                                                 int browser_id) {
  {
    std::ostringstream log;
    log << "TitleBar RegisterBrowserForWindow begin window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window || browser_id <= 0) {
    return;
  }
  g_muon_title_bar_browser_ids_by_window[window.get()] = browser_id;
  RegisterMuonTitleBarWindowForBrowser(window, browser_id);
  const auto controller = g_muon_title_bar_controllers.find(window.get());
  if (controller != g_muon_title_bar_controllers.end()) {
    g_muon_title_bar_controllers_by_browser_id[browser_id] =
        controller->second;
  }
  const auto title_bar_view = g_muon_title_bar_views.find(window.get());
  if (title_bar_view != g_muon_title_bar_views.end() &&
      title_bar_view->second) {
    g_muon_title_bar_views_by_browser_id[browser_id] =
        title_bar_view->second;
  }
  ApplyPendingMuonTitleBarState(window, browser_id);
  {
    std::ostringstream log;
    log << "TitleBar RegisterBrowserForWindow end window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id
        << " registered_window="
        << FormatMuonCloseDebugPointer(window.get());
    AppendMuonCloseDebugLog(log.str());
  }
}

void RegisterMuonTitleBarBrowserView(CefRefPtr<CefWindow> window,
                                     CefRefPtr<CefBrowserView> browser_view) {
  {
    std::ostringstream log;
    log << "TitleBar RegisterBrowserView window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_view="
        << FormatMuonCloseDebugPointer(browser_view.get());
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window || !browser_view) {
    return;
  }
  g_muon_title_bar_windows_by_browser_view[browser_view.get()] = window;
  const auto browser_id =
      g_muon_title_bar_browser_ids_by_browser_view.find(browser_view.get());
  if (browser_id != g_muon_title_bar_browser_ids_by_browser_view.end()) {
    RegisterMuonTitleBarBrowserForWindow(window, browser_id->second);
  }
  ApplyMuonDraggableRegions(window.get());
}

void RegisterMuonTitleBarBrowser(CefRefPtr<CefWindow> window, int browser_id) {
  {
    std::ostringstream log;
    log << "TitleBar RegisterBrowser window="
        << FormatMuonCloseDebugPointer(window.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window || browser_id <= 0) {
    return;
  }
  RegisterMuonTitleBarBrowserForWindow(window, browser_id);
}

void RegisterMuonTitleBarBrowserViewBrowser(
    CefRefPtr<CefBrowserView> browser_view,
    int browser_id) {
  {
    std::ostringstream log;
    log << "TitleBar RegisterBrowserViewBrowser browser_view="
        << FormatMuonCloseDebugPointer(browser_view.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
  }
  if (!browser_view || browser_id <= 0) {
    return;
  }
  g_muon_title_bar_browser_ids_by_browser_view[browser_view.get()] =
      browser_id;
  const auto window =
      g_muon_title_bar_windows_by_browser_view.find(browser_view.get());
  if (window == g_muon_title_bar_windows_by_browser_view.end()) {
    std::ostringstream log;
    log << "TitleBar RegisterBrowserViewBrowser pending browser_view="
        << FormatMuonCloseDebugPointer(browser_view.get())
        << " browser_id=" << browser_id;
    AppendMuonCloseDebugLog(log.str());
    return;
  }
  RegisterMuonTitleBarBrowserForWindow(window->second, browser_id);
}

void UnregisterMuonTitleBarController(CefRefPtr<CefWindow> window) {
  {
    std::ostringstream log;
    log << "TitleBar UnregisterControllerByWindow window="
        << FormatMuonCloseDebugPointer(window.get());
    AppendMuonCloseDebugLog(log.str());
  }
  if (!window) {
    return;
  }
  EraseRegisteredMuonTitleBarController(window.get(), nullptr);
}

void UnregisterMuonTitleBarController(
    CefRefPtr<MuonTitleBarController> controller) {
  {
    std::ostringstream log;
    log << "TitleBar UnregisterControllerByController controller="
        << FormatMuonCloseDebugPointer(controller.get());
    AppendMuonCloseDebugLog(log.str());
  }
  if (!controller) {
    return;
  }
  EraseRegisteredMuonTitleBarController(nullptr, controller.get());
}

void ClearMuonTitleBarRegistrations() {
  for (const auto& controller : g_muon_title_bar_controllers) {
    if (controller.second != nullptr) {
      controller.second->DetachWindow();
    }
  }
  g_muon_title_bar_controllers.clear();
  g_muon_title_bar_views.clear();
  g_muon_title_bar_views_by_browser_id.clear();
  g_muon_title_bar_browser_ids_by_window.clear();
  g_muon_title_bar_windows_by_browser_view.clear();
  g_muon_title_bar_browser_ids_by_browser_view.clear();
  g_muon_title_bar_controllers_by_browser_id.clear();
  g_muon_title_bar_controllers_by_window_handle.clear();
  g_muon_title_bar_windows_by_browser_id.clear();
  g_muon_title_bar_pending_titles_by_browser_id.clear();
  g_muon_title_bar_pending_icons_by_browser_id.clear();
  g_muon_title_bar_draggable_regions.clear();
  g_muon_page_draggable_regions.clear();
  g_muon_applied_draggable_regions.clear();
  g_muon_draggable_region_keys_by_window.clear();
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
                                   const MuonTitleBarIcon* icon) {
  if (!window) {
    return;
  }
  const auto icon_data_url = icon == nullptr ? std::string() : icon->data_url;
  const auto behavior =
      GetMuonWindowIconUpdateBehavior(
          icon != nullptr && !icon->png_data.empty(), icon_data_url);
  auto decoded_icon = MuonDecodedIconBitmap{};
  const auto has_decoded_icon =
      icon != nullptr &&
      behavior.window_icon_action == MuonWindowIconAction::Set &&
      DecodeMuonTitleBarIconPng(icon->png_data, &decoded_icon);
  if (behavior.window_icon_action == MuonWindowIconAction::Set) {
    CefRefPtr<CefImage> image;
    if (has_decoded_icon) {
      image = CreateMuonNativeIconImage(decoded_icon,
                                        kMuonNativeTitleBarIconDipSize);
    }
    window->SetWindowIcon(image ? image : CefImage::CreateImage());
  } else if (behavior.window_icon_action == MuonWindowIconAction::Clear) {
    window->SetWindowIcon(CefImage::CreateImage());
  }
  if (behavior.app_icon_action == MuonWindowIconAction::Set) {
    CefRefPtr<CefImage> image;
    if (has_decoded_icon) {
      image = CreateMuonNativeIconImage(decoded_icon, kMuonNativeAppIconDipSize);
    }
    window->SetWindowAppIcon(image ? image : CefImage::CreateImage());
  } else if (behavior.app_icon_action == MuonWindowIconAction::Clear) {
    window->SetWindowAppIcon(CefImage::CreateImage());
  }
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
    const MuonTitleBarIcon* icon) {
  if (browser_id <= 0) {
    return;
  }
  g_muon_title_bar_pending_icons_by_browser_id[browser_id] =
      icon == nullptr ? MuonTitleBarIcon{} : *icon;
  const auto pending_icon =
      g_muon_title_bar_pending_icons_by_browser_id.find(browser_id);
  const auto controller =
      g_muon_title_bar_controllers_by_browser_id.find(browser_id);
  if (controller == g_muon_title_bar_controllers_by_browser_id.end() ||
      controller->second == nullptr) {
    return;
  }
  const auto* resolved_icon =
      pending_icon == g_muon_title_bar_pending_icons_by_browser_id.end()
          ? nullptr
          : &pending_icon->second;
  controller->second->SetIconDataUrl(
      resolved_icon == nullptr ? std::string() : resolved_icon->data_url);
}

void SetRegisteredMuonTitleBarVisibility(CefRefPtr<CefWindow> window,
                                         bool visible) {
  if (!window) {
    return;
  }
  const auto view = g_muon_title_bar_views.find(window.get());
  if (view == g_muon_title_bar_views.end() || !view->second) {
    SetMuonX11TitleBarVisibility(window, visible);
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
  ApplyMuonDraggableRegions(window.get());
}

bool SetRegisteredMuonTitleBarVisibilityForBrowser(int browser_id,
                                                   bool visible) {
  const auto controller =
      g_muon_title_bar_controllers_by_browser_id.find(browser_id);
  if (controller != g_muon_title_bar_controllers_by_browser_id.end() &&
      controller->second != nullptr) {
    controller->second->SetVisible(visible);
    const auto view = g_muon_title_bar_views_by_browser_id.find(browser_id);
    if (view != g_muon_title_bar_views_by_browser_id.end() && view->second) {
      SetMuonX11TitleBarVisibility(view->second->GetWindow(), false);
    }
    return true;
  }
  const auto view = g_muon_title_bar_views_by_browser_id.find(browser_id);
  if (view == g_muon_title_bar_views_by_browser_id.end() || !view->second) {
    return false;
  }
  view->second->SetVisible(visible);
  view->second->InvalidateLayout();
  const auto window = view->second->GetWindow();
  if (!window) {
    return true;
  }
  SetMuonX11TitleBarVisibility(window, false);
  window->InvalidateLayout();
  window->Layout();
  ApplyMuonDraggableRegions(window.get());
  return true;
}

void SetRegisteredMuonPageDraggableRegions(
    CefRefPtr<CefWindow> window,
    CefRefPtr<CefBrowserView> browser_view,
    const std::vector<CefDraggableRegion>& regions) {
  if (!window) {
    return;
  }
  const auto key = GetDraggableRegionKey(window.get());
  if (regions.empty() || !browser_view) {
    g_muon_page_draggable_regions.erase(key);
  } else {
    g_muon_page_draggable_regions[key] = {browser_view, regions};
  }
  ApplyMuonDraggableRegions(window.get());
}

bool IsMuonPageDraggableRegionPoint(
    const std::vector<CefDraggableRegion>& regions,
    const CefPoint& point) {
  auto draggable = false;
  for (const auto& region : regions) {
    if (!ContainsPoint(region.bounds, point)) {
      continue;
    }
    if (!region.draggable) {
      return false;
    }
    draggable = true;
  }
  return draggable;
}

std::vector<std::uintptr_t> GetMuonPageDraggableRegionSearchKeys(
    std::uintptr_t window_key,
    const std::vector<std::uintptr_t>& registered_window_keys) {
  std::vector<std::uintptr_t> search_keys;
  if (window_key != 0) {
    search_keys.push_back(window_key);
    return search_keys;
  }
  for (const auto registered_window_key : registered_window_keys) {
    if (registered_window_key == 0) {
      continue;
    }
    search_keys.push_back(registered_window_key);
  }
  return search_keys;
}

bool IsRegisteredMuonPageDraggableRegionPoint(
    CefWindowHandle window_handle,
    const CefPoint& screen_point) {
  CEF_REQUIRE_UI_THREAD();

  CefPoint view_point;
  return FindPageDraggableRegionsAtScreenPoint(
             window_handle, screen_point, &view_point) != nullptr;
}

bool ForwardRegisteredMuonPageDraggableRegionWheel(
    CefWindowHandle window_handle,
    const CefPoint& screen_point,
    int delta_x,
    int delta_y,
    uint32_t modifiers) {
  CEF_REQUIRE_UI_THREAD();

  CefPoint view_point;
  const auto* page_regions = FindPageDraggableRegionsAtScreenPoint(
      window_handle, screen_point, &view_point);
  if (page_regions == nullptr) {
    return false;
  }
  return ForwardPageDraggableRegionWheel(
      *page_regions, view_point, delta_x, delta_y, modifiers);
}

MuonTitleBarControlAction GetRegisteredMuonTitleBarControlActionAtScreenPoint(
    CefWindowHandle window_handle,
    const CefPoint& screen_point,
    const CefRect& window_bounds_in_screen) {
  CEF_REQUIRE_UI_THREAD();

  const auto controller =
      FindMuonTitleBarControllerByWindowHandle(window_handle);
  if (controller == nullptr ||
      !controller->CanHandleNativeWindowControls()) {
    return MuonTitleBarControlAction::NoControl;
  }
  if (window_bounds_in_screen.width <= 0 ||
      window_bounds_in_screen.height <= 0) {
    return MuonTitleBarControlAction::NoControl;
  }

  return GetMuonTitleBarControlActionAtWindowPoint(
      true, controller->GetHeight(), controller->GetControlsWidth(),
      CefSize(window_bounds_in_screen.width, window_bounds_in_screen.height),
      CefPoint(screen_point.x - window_bounds_in_screen.x,
               screen_point.y - window_bounds_in_screen.y));
}

bool IsRegisteredMuonTitleBarDragRegionPoint(
    CefWindowHandle window_handle,
    const CefPoint& screen_point,
    const CefRect& window_bounds_in_screen) {
  CEF_REQUIRE_UI_THREAD();

  const auto controller =
      FindMuonTitleBarControllerByWindowHandle(window_handle);
  if (controller == nullptr ||
      !controller->CanHandleNativeWindowControls()) {
    return false;
  }
  if (window_bounds_in_screen.width <= 0 ||
      window_bounds_in_screen.height <= 0 ||
      controller->GetHeight() <= 0) {
    return false;
  }

  const auto window_point =
      CefPoint(screen_point.x - window_bounds_in_screen.x,
               screen_point.y - window_bounds_in_screen.y);
  if (window_point.x < 0 || window_point.y < 0 ||
      window_point.x >= window_bounds_in_screen.width ||
      window_point.y >= window_bounds_in_screen.height ||
      window_point.y >= controller->GetHeight()) {
    return false;
  }

  return GetMuonTitleBarControlActionAtWindowPoint(
             true, controller->GetHeight(), controller->GetControlsWidth(),
             CefSize(window_bounds_in_screen.width,
                     window_bounds_in_screen.height),
             window_point) == MuonTitleBarControlAction::NoControl;
}

bool HandleRegisteredMuonTitleBarControlAction(
    CefWindowHandle window_handle,
    MuonTitleBarControlAction action) {
  CEF_REQUIRE_UI_THREAD();

  const auto controller =
      FindMuonTitleBarControllerByWindowHandle(window_handle);
  if (controller == nullptr ||
      !controller->CanHandleNativeWindowControls()) {
    return false;
  }
  const auto* action_name = GetMuonTitleBarControlActionName(action);
  if (action_name == nullptr) {
    return false;
  }
  controller->HandleAction(action_name);
  return true;
}

bool SetRegisteredMuonTitleBarHoveredControl(
    CefWindowHandle window_handle,
    MuonTitleBarControlAction action) {
  CEF_REQUIRE_UI_THREAD();

  const auto controller =
      FindMuonTitleBarControllerByWindowHandle(window_handle);
  if (controller == nullptr) {
    return false;
  }
  if (action != MuonTitleBarControlAction::NoControl &&
      !controller->CanHandleNativeWindowControls()) {
    return false;
  }
  controller->SetNativeHoveredControl(action);
  return true;
}

bool SetRegisteredMuonTitleBarPressedControl(
    CefWindowHandle window_handle,
    MuonTitleBarControlAction action) {
  CEF_REQUIRE_UI_THREAD();

  const auto controller =
      FindMuonTitleBarControllerByWindowHandle(window_handle);
  if (controller == nullptr) {
    return false;
  }
  if (action != MuonTitleBarControlAction::NoControl &&
      !controller->CanHandleNativeWindowControls()) {
    return false;
  }
  controller->SetNativePressedControl(action);
  return true;
}

bool HasRegisteredMuonWindowForBrowser(int browser_id) {
  return g_muon_title_bar_windows_by_browser_id.find(browser_id) !=
         g_muon_title_bar_windows_by_browser_id.end();
}
