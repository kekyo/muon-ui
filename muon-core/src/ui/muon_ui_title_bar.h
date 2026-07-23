/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#if defined(_WIN32)
#if defined(MUON_UI_BUILDING_LIBRARY)
#define MUON_UI_API __declspec(dllexport)
#else
#define MUON_UI_API __declspec(dllimport)
#endif
#else
#define MUON_UI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns the title bar provider manifest as a UTF-8 JSON string.
 *
 * @remarks The returned pointer is owned by libmuon-ui and remains valid for
 * the process lifetime.
 */
MUON_UI_API const char* muon_ui_title_bar_get_manifest(void);

#ifdef __cplusplus
}
#endif

