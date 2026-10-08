/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "browser/muon_title_bar_loader.h"

#include "ui/muon_ui_title_bar.h"

MuonTitleBarManifest LoadMuonTitleBarManifestFromUi() {
  return ParseMuonTitleBarManifest(muon_ui_title_bar_get_manifest());
}

