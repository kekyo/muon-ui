/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <string>

/**
 * Returns the default title for normal browser windows.
 */
const char* GetMuonDefaultWindowTitle();

/**
 * Returns the default title for DevTools windows.
 */
const char* GetMuonDevToolsWindowTitle();

/**
 * Returns a non-empty window title, falling back to the muon default.
 *
 * @param page_title Title reported by the page.
 * @return page_title when non-empty; otherwise the default browser title.
 */
std::string GetMuonWindowTitleOrDefault(const std::string& page_title);
