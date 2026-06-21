/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "browser/muon_title_bar.h"

#include <string>

/**
 * Loads the embedded default PNG title bar icon.
 *
 * @param icon Receives the loaded icon.
 * @param error_message Receives a validation diagnostic.
 * @return true when the embedded PNG icon was loaded.
 */
bool LoadDefaultMuonTitleBarIcon(MuonTitleBarIcon* icon,
                                 std::string* error_message);
