/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "browser/muon_window_state.h"

#include <algorithm>

#if defined(OS_LINUX) && defined(CEF_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "include/internal/cef_types_linux.h"
#endif

#if defined(OS_LINUX) && defined(CEF_X11)
static void SetX11FullscreenState(CefWindowHandle window_handle,
                                  bool fullscreen) {
  if (window_handle == 0) {
    return;
  }
  auto* display = cef_get_xdisplay();
  if (display == nullptr) {
    return;
  }
  const auto wm_state = XInternAtom(display, "_NET_WM_STATE", False);
  const auto fullscreen_atom =
      XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
  if (wm_state == None || fullscreen_atom == None) {
    return;
  }

  XEvent event = {};
  event.xclient.type = ClientMessage;
  event.xclient.send_event = True;
  event.xclient.display = display;
  event.xclient.window = static_cast<Window>(window_handle);
  event.xclient.message_type = wm_state;
  event.xclient.format = 32;
  event.xclient.data.l[0] = fullscreen ? 1 : 0;
  event.xclient.data.l[1] = static_cast<long>(fullscreen_atom);
  event.xclient.data.l[2] = 0;
  event.xclient.data.l[3] = 1;
  event.xclient.data.l[4] = 0;

  XSendEvent(display, DefaultRootWindow(display), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &event);
  XFlush(display);
}
#endif

cef_show_state_t GetMuonCefInitialShowState(
    MuonBrowserInitialWindowState initial_window_state) {
  switch (initial_window_state) {
    case kMuonBrowserInitialWindowStateMinimized:
      return CEF_SHOW_STATE_MINIMIZED;
    case kMuonBrowserInitialWindowStateMaximized:
      return CEF_SHOW_STATE_MAXIMIZED;
    case kMuonBrowserInitialWindowStateFullscreen:
      return CEF_SHOW_STATE_FULLSCREEN;
    case kMuonBrowserInitialWindowStateNormal:
    case kMuonBrowserInitialWindowStateHidden:
      return CEF_SHOW_STATE_NORMAL;
  }
  return CEF_SHOW_STATE_NORMAL;
}

void SetMuonWindowFullscreen(CefRefPtr<CefWindow> window, bool fullscreen) {
  if (!window) {
    return;
  }
  window->SetFullscreen(fullscreen);
#if defined(OS_LINUX) && defined(CEF_X11)
  SetX11FullscreenState(window->GetWindowHandle(), fullscreen);
#endif
}

CefRect GetMuonCenteredWindowBounds(const CefRect& work_area,
                                    const CefSize& preferred_size) {
  if (work_area.width <= 0 || work_area.height <= 0) {
    return CefRect(work_area.x, work_area.y, 0, 0);
  }
  const auto width =
      preferred_size.width <= 0
          ? work_area.width
          : std::min(preferred_size.width, work_area.width);
  const auto height =
      preferred_size.height <= 0
          ? work_area.height
          : std::min(preferred_size.height, work_area.height);
  const auto x = work_area.x + (work_area.width - width) / 2;
  const auto y = work_area.y + (work_area.height - height) / 2;
  return CefRect(x, y, width, height);
}

void ShowMuonWindow(CefRefPtr<CefWindow> window) {
  if (!window) {
    return;
  }
  window->Show();
  window->Restore();
}
