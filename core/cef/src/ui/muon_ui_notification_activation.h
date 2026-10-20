/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include <stdint.h>

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
 * Presents an existing X11 window using the OS notification interaction time.
 * @param window Existing X11 window identifier, never a Wayland surface.
 * @param startup_id Startup notification identifier supplied by the desktop.
 * @return One when the request was submitted, zero for missing context or an
 * unsupported display. The window manager decides whether to grant focus.
 */
MUON_UI_API int muon_ui_notification_activate_x11(
    uint64_t window, const char* startup_id);

#ifdef __cplusplus
}
#endif
