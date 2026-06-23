/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_native_wheel_forwarder.h"

#include "browser/muon_title_bar.h"

#include "include/cef_task.h"
#include "include/views/cef_display.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <mutex>
#include <set>
#include <vector>
#endif

#if defined(OS_LINUX) && defined(CEF_X11)
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <mutex>
#include <poll.h>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>
#endif

namespace {

#if defined(_WIN32) || (defined(OS_LINUX) && defined(CEF_X11))
static bool ForwardNativeWheelFromScreenPixels(CefWindowHandle window_handle,
                                               CefPoint screen_point,
                                               int delta_x,
                                               int delta_y,
                                               uint32_t modifiers) {
  const auto dip_screen_point =
      CefDisplay::ConvertScreenPointFromPixels(screen_point);
  return ForwardRegisteredMuonPageDraggableRegionWheel(
      window_handle, dip_screen_point, delta_x, delta_y, modifiers);
}
#endif

#if defined(_WIN32)

constexpr UINT_PTR kMuonWheelForwarderSubclassId = 0x4d574648u;
std::mutex g_muon_windows_subclass_mutex;
std::set<HWND> g_muon_windows_subclassed_for_wheel;

static uint32_t GetMuonWindowsEventFlags(WPARAM wparam) {
  const auto key_state = GET_KEYSTATE_WPARAM(wparam);
  auto modifiers = uint32_t{0};
  if ((key_state & MK_SHIFT) != 0) {
    modifiers |= EVENTFLAG_SHIFT_DOWN;
  }
  if ((key_state & MK_CONTROL) != 0) {
    modifiers |= EVENTFLAG_CONTROL_DOWN;
  }
  if ((key_state & MK_LBUTTON) != 0) {
    modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  }
  if ((key_state & MK_MBUTTON) != 0) {
    modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  }
  if ((key_state & MK_RBUTTON) != 0) {
    modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  }
  if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
    modifiers |= EVENTFLAG_ALT_DOWN;
  }
  return modifiers;
}

static void ForgetMuonWindowsSubclass(HWND window_handle) {
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  g_muon_windows_subclassed_for_wheel.erase(window_handle);
}

static LRESULT CALLBACK MuonWheelForwarderSubclassProc(
    HWND window_handle,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR ref_data) {
  (void)ref_data;
  if (subclass_id != kMuonWheelForwarderSubclassId) {
    return DefSubclassProc(window_handle, message, wparam, lparam);
  }

  switch (message) {
    case WM_MOUSEWHEEL: {
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (ForwardNativeWheelFromScreenPixels(
              window_handle, screen_point, 0, GET_WHEEL_DELTA_WPARAM(wparam),
              GetMuonWindowsEventFlags(wparam))) {
        return 0;
      }
      break;
    }
    case WM_MOUSEHWHEEL: {
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (ForwardNativeWheelFromScreenPixels(
              window_handle, screen_point, -GET_WHEEL_DELTA_WPARAM(wparam), 0,
              GetMuonWindowsEventFlags(wparam))) {
        return 0;
      }
      break;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(window_handle, MuonWheelForwarderSubclassProc,
                           kMuonWheelForwarderSubclassId);
      ForgetMuonWindowsSubclass(window_handle);
      break;
    default:
      break;
  }
  return DefSubclassProc(window_handle, message, wparam, lparam);
}

static void RegisterMuonWindowsWheelForwarder(HWND window_handle) {
  if (window_handle == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    if (g_muon_windows_subclassed_for_wheel.find(window_handle) !=
        g_muon_windows_subclassed_for_wheel.end()) {
      return;
    }
  }
  if (SetWindowSubclass(window_handle, MuonWheelForwarderSubclassProc,
                        kMuonWheelForwarderSubclassId, 0) == FALSE) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  g_muon_windows_subclassed_for_wheel.insert(window_handle);
}

static void UnregisterMuonWindowsWheelForwarder(HWND window_handle) {
  if (window_handle == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    g_muon_windows_subclassed_for_wheel.erase(window_handle);
  }
  RemoveWindowSubclass(window_handle, MuonWheelForwarderSubclassProc,
                       kMuonWheelForwarderSubclassId);
}

static void ClearMuonWindowsWheelForwarders() {
  std::vector<HWND> window_handles;
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    window_handles.assign(g_muon_windows_subclassed_for_wheel.begin(),
                          g_muon_windows_subclassed_for_wheel.end());
    g_muon_windows_subclassed_for_wheel.clear();
  }
  for (const auto window_handle : window_handles) {
    RemoveWindowSubclass(window_handle, MuonWheelForwarderSubclassProc,
                         kMuonWheelForwarderSubclassId);
  }
}

#endif

#if defined(OS_LINUX) && defined(CEF_X11)

constexpr int kMuonWheelDelta = 120;
std::mutex g_muon_x11_wheel_mutex;
std::set<CefWindowHandle> g_muon_x11_wheel_windows;
std::thread g_muon_x11_wheel_thread;
std::atomic<bool> g_muon_x11_wheel_running{false};
int g_muon_x11_wheel_wake_pipe[2] = {-1, -1};

class MuonForwardX11WheelTask final : public CefTask {
 public:
  MuonForwardX11WheelTask(CefWindowHandle window_handle,
                          CefPoint screen_point,
                          int delta_x,
                          int delta_y,
                          uint32_t modifiers)
      : window_handle_(window_handle),
        screen_point_(screen_point),
        delta_x_(delta_x),
        delta_y_(delta_y),
        modifiers_(modifiers) {}

  void Execute() override {
    ForwardNativeWheelFromScreenPixels(
        window_handle_, screen_point_, delta_x_, delta_y_, modifiers_);
  }

 private:
  const CefWindowHandle window_handle_;
  const CefPoint screen_point_;
  const int delta_x_;
  const int delta_y_;
  const uint32_t modifiers_;

  IMPLEMENT_REFCOUNTING(MuonForwardX11WheelTask);
  DISALLOW_COPY_AND_ASSIGN(MuonForwardX11WheelTask);
};

static uint32_t GetMuonX11ButtonEventFlags(const XIDeviceEvent* event) {
  auto modifiers = uint32_t{0};
  if ((event->mods.effective & ShiftMask) != 0) {
    modifiers |= EVENTFLAG_SHIFT_DOWN;
  }
  if ((event->mods.effective & ControlMask) != 0) {
    modifiers |= EVENTFLAG_CONTROL_DOWN;
  }
  if ((event->mods.effective & Mod1Mask) != 0) {
    modifiers |= EVENTFLAG_ALT_DOWN;
  }
  if (XIMaskIsSet(event->buttons.mask, 1)) {
    modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  }
  if (XIMaskIsSet(event->buttons.mask, 2)) {
    modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  }
  if (XIMaskIsSet(event->buttons.mask, 3)) {
    modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  }
  return modifiers;
}

static bool GetMuonX11WheelDeltas(int button, int* delta_x, int* delta_y) {
  *delta_x = 0;
  *delta_y = 0;
  switch (button) {
    case 4:
      *delta_y = kMuonWheelDelta;
      return true;
    case 5:
      *delta_y = -kMuonWheelDelta;
      return true;
    case 6:
      *delta_x = kMuonWheelDelta;
      return true;
    case 7:
      *delta_x = -kMuonWheelDelta;
      return true;
    default:
      return false;
  }
}

static void PostMuonX11WheelEvent(const XIDeviceEvent* event) {
  int delta_x = 0;
  int delta_y = 0;
  if (!GetMuonX11WheelDeltas(event->detail, &delta_x, &delta_y)) {
    return;
  }

  const auto window_handle =
      static_cast<CefWindowHandle>(event->child != None ? event->child
                                                       : event->event);
  const auto screen_point =
      CefPoint(static_cast<int>(std::lround(event->root_x)),
               static_cast<int>(std::lround(event->root_y)));
  CefPostTask(
      TID_UI,
      new MuonForwardX11WheelTask(window_handle, screen_point, delta_x, delta_y,
                                  GetMuonX11ButtonEventFlags(event)));
}

static bool SelectMuonX11WheelEvents(Display* display, int* xi_opcode) {
  int event_code = 0;
  int error_code = 0;
  if (XQueryExtension(display, "XInputExtension", xi_opcode, &event_code,
                      &error_code) == False) {
    return false;
  }

  auto major = 2;
  auto minor = 0;
  if (XIQueryVersion(display, &major, &minor) != Success) {
    return false;
  }

  unsigned char mask[(XI_LASTEVENT + 7) / 8] = {};
  XISetMask(mask, XI_ButtonPress);
  XIEventMask event_mask;
  event_mask.deviceid = XIAllMasterDevices;
  event_mask.mask_len = sizeof(mask);
  event_mask.mask = mask;

  std::vector<Window> windows;
  windows.push_back(DefaultRootWindow(display));
  {
    std::lock_guard<std::mutex> lock(g_muon_x11_wheel_mutex);
    windows.insert(windows.end(), g_muon_x11_wheel_windows.begin(),
                   g_muon_x11_wheel_windows.end());
  }
  for (const auto window : windows) {
    XISelectEvents(display, window, &event_mask, 1);
  }
  XFlush(display);
  return true;
}

static void HandleMuonX11Event(Display* display,
                               int xi_opcode,
                               XEvent* event) {
  if (event->type != GenericEvent || event->xcookie.extension != xi_opcode) {
    return;
  }
  if (XGetEventData(display, &event->xcookie) == False) {
    return;
  }
  if (event->xcookie.evtype == XI_ButtonPress) {
    PostMuonX11WheelEvent(
        static_cast<const XIDeviceEvent*>(event->xcookie.data));
  }
  XFreeEventData(display, &event->xcookie);
}

static void RunMuonX11WheelThread(int wake_pipe) {
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    return;
  }

  int xi_opcode = 0;
  if (!SelectMuonX11WheelEvents(display, &xi_opcode)) {
    XCloseDisplay(display);
    return;
  }

  const auto x11_fd = ConnectionNumber(display);
  pollfd descriptors[2] = {
      {x11_fd, POLLIN, 0},
      {wake_pipe, POLLIN, 0},
  };
  while (g_muon_x11_wheel_running.load()) {
    const auto poll_result = poll(descriptors, 2, -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
      uint8_t value = 0;
      const auto read_count = read(wake_pipe, &value, sizeof(value));
      (void)read_count;
      if (!g_muon_x11_wheel_running.load()) {
        break;
      }
      SelectMuonX11WheelEvents(display, &xi_opcode);
      continue;
    }
    if ((descriptors[0].revents & POLLIN) == 0) {
      continue;
    }
    while (XPending(display) > 0) {
      XEvent event;
      XNextEvent(display, &event);
      HandleMuonX11Event(display, xi_opcode, &event);
    }
  }
  XCloseDisplay(display);
}

static void WakeMuonX11WheelThread() {
  if (g_muon_x11_wheel_wake_pipe[1] == -1) {
    return;
  }
  const auto value = uint8_t{1};
  ssize_t written = 0;
  do {
    written = write(g_muon_x11_wheel_wake_pipe[1], &value, sizeof(value));
  } while (written < 0 && errno == EINTR);
}

static void StartMuonX11WheelThreadLocked() {
  if (g_muon_x11_wheel_running.load()) {
    return;
  }
  if (pipe(g_muon_x11_wheel_wake_pipe) != 0) {
    return;
  }
  g_muon_x11_wheel_running.store(true);
  g_muon_x11_wheel_thread =
      std::thread(RunMuonX11WheelThread, g_muon_x11_wheel_wake_pipe[0]);
}

static void StopMuonX11WheelThread() {
  if (!g_muon_x11_wheel_running.load()) {
    return;
  }
  g_muon_x11_wheel_running.store(false);
  if (g_muon_x11_wheel_wake_pipe[1] != -1) {
    WakeMuonX11WheelThread();
  }
  if (g_muon_x11_wheel_thread.joinable()) {
    g_muon_x11_wheel_thread.join();
  }
  if (g_muon_x11_wheel_wake_pipe[0] != -1) {
    close(g_muon_x11_wheel_wake_pipe[0]);
  }
  if (g_muon_x11_wheel_wake_pipe[1] != -1) {
    close(g_muon_x11_wheel_wake_pipe[1]);
  }
  g_muon_x11_wheel_wake_pipe[0] = -1;
  g_muon_x11_wheel_wake_pipe[1] = -1;
}

static void RegisterMuonX11WheelForwarder(CefWindowHandle window_handle) {
  if (window_handle == 0) {
    return;
  }
  auto wake_thread = false;
  {
    std::lock_guard<std::mutex> lock(g_muon_x11_wheel_mutex);
    g_muon_x11_wheel_windows.insert(window_handle);
    if (g_muon_x11_wheel_running.load()) {
      wake_thread = true;
    } else {
      StartMuonX11WheelThreadLocked();
    }
  }
  if (wake_thread) {
    WakeMuonX11WheelThread();
  }
}

static void UnregisterMuonX11WheelForwarder(CefWindowHandle window_handle) {
  if (window_handle == 0) {
    return;
  }
  auto stop_thread = false;
  auto wake_thread = false;
  {
    std::lock_guard<std::mutex> lock(g_muon_x11_wheel_mutex);
    g_muon_x11_wheel_windows.erase(window_handle);
    if (g_muon_x11_wheel_windows.empty()) {
      stop_thread = true;
    } else if (g_muon_x11_wheel_running.load()) {
      wake_thread = true;
    }
  }
  if (stop_thread) {
    StopMuonX11WheelThread();
  } else if (wake_thread) {
    WakeMuonX11WheelThread();
  }
}

static void ClearMuonX11WheelForwarders() {
  {
    std::lock_guard<std::mutex> lock(g_muon_x11_wheel_mutex);
    g_muon_x11_wheel_windows.clear();
  }
  StopMuonX11WheelThread();
}

#endif

}  // namespace

void RegisterMuonNativeWheelForwarder(CefRefPtr<CefWindow> window) {
  if (!window) {
    return;
  }
#if defined(_WIN32)
  RegisterMuonWindowsWheelForwarder(window->GetWindowHandle());
#elif defined(OS_LINUX) && defined(CEF_X11)
  RegisterMuonX11WheelForwarder(window->GetWindowHandle());
#endif
}

void UnregisterMuonNativeWheelForwarder(CefRefPtr<CefWindow> window) {
  if (!window) {
    return;
  }
#if defined(_WIN32)
  UnregisterMuonWindowsWheelForwarder(window->GetWindowHandle());
#elif defined(OS_LINUX) && defined(CEF_X11)
  UnregisterMuonX11WheelForwarder(window->GetWindowHandle());
#endif
}

void ClearMuonNativeWheelForwarders() {
#if defined(_WIN32)
  ClearMuonWindowsWheelForwarders();
#elif defined(OS_LINUX) && defined(CEF_X11)
  ClearMuonX11WheelForwarders();
#endif
}
