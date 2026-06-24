/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_default_title_bar_icon.h"
#include "browser/muon_default_title_bar_icon_generated.h"

#include <string>

bool LoadDefaultMuonTitleBarIcon(MuonTitleBarIcon* icon,
                                 std::string* error_message) {
  return LoadMuonTitleBarIconFromPngBytes(
      muon_internal::kMuonDefaultTitleBarIconPng.data(),
      muon_internal::kMuonDefaultTitleBarIconPng.size(),
      "embedded default title bar icon", icon, error_message);
}
