/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "browser/muon_title_bar.h"

/**
 * Loads and parses the title bar manifest provided by libmuon-ui.
 */
MuonTitleBarManifest LoadMuonTitleBarManifestFromUi();

