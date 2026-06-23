/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "ui/muon_ui_title_bar.h"

#include "yyjson.h"

#include <cstdio>
#include <cstring>
#include <string>

static bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "%s\n", message);
  return false;
}

static std::string ReadString(yyjson_val* root, const char* key) {
  auto* value = yyjson_obj_get(root, key);
  return yyjson_is_str(value) ? std::string(yyjson_get_str(value)) : "";
}

static int ReadInt(yyjson_val* root, const char* key) {
  auto* value = yyjson_obj_get(root, key);
  return yyjson_is_int(value) ? static_cast<int>(yyjson_get_int(value)) : 0;
}

static bool ReadBool(yyjson_val* root, const char* key) {
  auto* value = yyjson_obj_get(root, key);
  return yyjson_is_bool(value) && yyjson_get_bool(value);
}

static bool Contains(const std::string& value, const char* needle) {
  return value.find(needle) != std::string::npos;
}

int main() {
  const auto* manifest_json = muon_ui_title_bar_get_manifest();
  if (!Expect(manifest_json != nullptr, "missing title bar manifest")) {
    return 1;
  }
  yyjson_doc* document =
      yyjson_read(manifest_json, std::strlen(manifest_json), 0);
  if (!Expect(document != nullptr, "invalid title bar manifest JSON")) {
    return 1;
  }
  auto* root = yyjson_doc_get_root(document);
  if (!Expect(yyjson_is_obj(root), "title bar manifest is not an object")) {
    yyjson_doc_free(document);
    return 1;
  }
  const auto mode = ReadString(root, "mode");
  const auto html = ReadString(root, "html");
  const auto css = ReadString(root, "css");
  const auto js = ReadString(root, "js");
  const auto passed =
      Expect(mode == "custom", "title bar manifest must be custom") &&
      Expect(ReadInt(root, "height") > 0, "missing title bar height") &&
      Expect(ReadInt(root, "controlsWidth") > 0,
             "missing title bar control width") &&
      Expect(ReadBool(root, "nativeWindowControls"),
             "built-in title bar should enable native window controls") &&
#if defined(_WIN32)
      Expect(ReadInt(root, "controlsWidth") == 138,
             "Windows title bar controls should keep the current width") &&
      Expect(Contains(css, "width: 46px;"),
             "Windows title bar controls should keep rectangular buttons") &&
#else
      Expect(ReadInt(root, "controlsWidth") == 96,
             "Linux title bar controls should use compact round button width") &&
      Expect(Contains(css, "width: 20px;"),
             "Linux title bar controls should use round button width") &&
      Expect(Contains(css, "height: 20px;"),
             "Linux title bar controls should use round button height") &&
      Expect(Contains(css, "gap: 12px;"),
             "Linux title bar controls should use wider button spacing") &&
      Expect(Contains(css, "border-radius: 999px;"),
             "Linux title bar controls should be round") &&
      Expect(Contains(css, "--muon-titlebar-round-button-bg"),
             "Linux title bar controls should define round button background") &&
#endif
      Expect(Contains(html, "data-action=\"minimize\""),
             "missing minimize action") &&
      Expect(Contains(html, "data-action=\"maximize\""),
             "missing maximize action") &&
      Expect(Contains(html, "data-action=\"close\""),
             "missing close action") &&
      Expect(Contains(css, "prefers-color-scheme"),
             "missing system theme switching") &&
      Expect(Contains(css, "--muon-titlebar-close-hover"),
             "missing themed button accent") &&
      Expect(Contains(css, "border-bottom"), "missing GTK-like separator") &&
      Expect(Contains(js, "__muonTitleBar"), "missing title bar bridge");

  yyjson_doc_free(document);
  return passed ? 0 : 1;
}
