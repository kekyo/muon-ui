/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_window_title.h"

static constexpr char kWindowTitle[] = "Muon";
static constexpr char kDevToolsWindowTitle[] = "Muon DevTools";

const char* GetMuonDefaultWindowTitle() {
  return kWindowTitle;
}

const char* GetMuonDevToolsWindowTitle() {
  return kDevToolsWindowTitle;
}

std::string GetMuonWindowTitleOrDefault(const std::string& page_title) {
  return page_title.empty() ? GetMuonDefaultWindowTitle() : page_title;
}
