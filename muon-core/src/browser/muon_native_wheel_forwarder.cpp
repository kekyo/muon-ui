/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_native_wheel_forwarder.h"

#include "browser/muon_title_bar.h"
#include "log/muon_close_debug_log.h"

#include "include/cef_task.h"
#include "include/views/cef_display.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

#include <map>
#include <mutex>
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

#if defined(_WIN32)
static CefPoint GetNativeWheelDipScreenPoint(CefPoint screen_point) {
  return CefDisplay::ConvertScreenPointFromPixels(screen_point);
}
#elif defined(OS_LINUX) && defined(CEF_X11)
static CefPoint GetNativeWheelDipScreenPoint(CefPoint screen_point) {
  return screen_point;
}
#endif

#if defined(_WIN32) || (defined(OS_LINUX) && defined(CEF_X11))
static bool ForwardNativeWheelFromScreenPoint(CefWindowHandle window_handle,
                                              CefPoint screen_point,
                                              int delta_x,
                                              int delta_y,
                                              uint32_t modifiers) {
  const auto dip_screen_point =
      GetNativeWheelDipScreenPoint(screen_point);
  return ForwardRegisteredMuonPageDraggableRegionWheel(
      window_handle, dip_screen_point, delta_x, delta_y, modifiers);
}
#endif

#if defined(_WIN32)

constexpr UINT_PTR kMuonWheelForwarderSubclassId = 0x4d574648u;
constexpr DWORD kMuonWindowsReleasedDragGraceMs = 250;
std::mutex g_muon_windows_subclass_mutex;
std::map<HWND, std::set<HWND>> g_muon_windows_subclassed_for_wheel_by_root;
std::map<HWND, HWND> g_muon_windows_wheel_root_by_subclassed_window;

struct MuonWindowsPendingTitleBarControl {
  HWND root_window_handle = nullptr;
  MuonTitleBarControlAction action = MuonTitleBarControlAction::NoControl;
};

struct MuonWindowsPendingWindowDrag {
  HWND root_window_handle = nullptr;
  CefPoint start_screen_point;
  RECT start_window_rect = {};
  bool moved = false;
  bool released_before_move = false;
  DWORD release_tick = 0;
};

std::map<HWND, MuonWindowsPendingTitleBarControl>
    g_muon_windows_pending_title_bar_controls;
std::map<HWND, MuonWindowsPendingWindowDrag>
    g_muon_windows_pending_window_drags;

static LRESULT CALLBACK MuonWheelForwarderSubclassProc(
    HWND window_handle,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR ref_data);

static HWND GetMuonWindowsRootWindow(HWND window_handle) {
  if (window_handle == nullptr) {
    return nullptr;
  }
  auto root_handle = GetAncestor(window_handle, GA_ROOT);
  return root_handle != nullptr ? root_handle : window_handle;
}

static HWND GetMuonWindowsSubclassRootWindow(HWND window_handle,
                                             DWORD_PTR ref_data) {
  const auto root_window_handle = reinterpret_cast<HWND>(ref_data);
  return root_window_handle != nullptr ? root_window_handle
                                       : GetMuonWindowsRootWindow(window_handle);
}

static void AppendMuonWindowsDragDebugLog(const std::string& message) {
  AppendMuonCloseDebugLog("WindowsDrag " + message);
}

static std::string FormatMuonWindowsHandle(HWND handle) {
  return FormatMuonCloseDebugPointer(handle);
}

static bool IsNativePageDraggableRegionScreenPixels(
    CefWindowHandle window_handle,
    CefPoint screen_point) {
  const auto dip_screen_point =
      CefDisplay::ConvertScreenPointFromPixels(screen_point);
  return IsRegisteredMuonPageDraggableRegionPoint(
      window_handle, dip_screen_point);
}

static bool GetMuonWindowsScreenPointFromClientPoint(
    HWND window_handle,
    LPARAM lparam,
    CefPoint* screen_point) {
  if (screen_point == nullptr) {
    return false;
  }
  POINT raw_screen_point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  if (ClientToScreen(window_handle, &raw_screen_point) == FALSE) {
    return false;
  }
  *screen_point = CefPoint(raw_screen_point.x, raw_screen_point.y);
  return true;
}

static bool GetMuonWindowsWindowBoundsInScreenDip(
    HWND window_handle,
    CefRect* window_bounds) {
  if (window_handle == nullptr || window_bounds == nullptr) {
    return false;
  }
  RECT rect = {};
  if (GetWindowRect(window_handle, &rect) == FALSE) {
    return false;
  }
  const auto top_left = CefDisplay::ConvertScreenPointFromPixels(
      CefPoint(rect.left, rect.top));
  const auto bottom_right = CefDisplay::ConvertScreenPointFromPixels(
      CefPoint(rect.right, rect.bottom));
  *window_bounds = CefRect(
      top_left.x, top_left.y,
      std::max(0, bottom_right.x - top_left.x),
      std::max(0, bottom_right.y - top_left.y));
  return window_bounds->width > 0 && window_bounds->height > 0;
}

static MuonTitleBarControlAction GetMuonWindowsTitleBarControlAction(
    HWND root_window_handle,
    CefPoint screen_point_pixels) {
  CefRect window_bounds;
  if (!GetMuonWindowsWindowBoundsInScreenDip(root_window_handle,
                                            &window_bounds)) {
    return MuonTitleBarControlAction::NoControl;
  }
  const auto dip_screen_point =
      CefDisplay::ConvertScreenPointFromPixels(screen_point_pixels);
  return GetRegisteredMuonTitleBarControlActionAtScreenPoint(
      root_window_handle, dip_screen_point, window_bounds);
}

static bool IsMuonWindowsTitleBarDragRegion(
    HWND root_window_handle,
    CefPoint screen_point_pixels) {
  CefRect window_bounds;
  if (!GetMuonWindowsWindowBoundsInScreenDip(root_window_handle,
                                            &window_bounds)) {
    return false;
  }
  const auto dip_screen_point =
      CefDisplay::ConvertScreenPointFromPixels(screen_point_pixels);
  return IsRegisteredMuonTitleBarDragRegionPoint(
      root_window_handle, dip_screen_point, window_bounds);
}

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
  g_muon_windows_pending_title_bar_controls.erase(window_handle);
  const auto root =
      g_muon_windows_wheel_root_by_subclassed_window.find(window_handle);
  if (root == g_muon_windows_wheel_root_by_subclassed_window.end()) {
    return;
  }
  const auto root_window_handle = root->second;
  g_muon_windows_wheel_root_by_subclassed_window.erase(root);

  const auto windows =
      g_muon_windows_subclassed_for_wheel_by_root.find(root_window_handle);
  if (windows == g_muon_windows_subclassed_for_wheel_by_root.end()) {
    return;
  }
  windows->second.erase(window_handle);
  if (windows->second.empty()) {
    g_muon_windows_subclassed_for_wheel_by_root.erase(windows);
  }
}

static void ForgetMuonWindowsSubclassForRoot(HWND root_window_handle,
                                             HWND window_handle) {
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  const auto root =
      g_muon_windows_wheel_root_by_subclassed_window.find(window_handle);
  if (root == g_muon_windows_wheel_root_by_subclassed_window.end() ||
      root->second != root_window_handle) {
    return;
  }
  g_muon_windows_pending_title_bar_controls.erase(window_handle);
  g_muon_windows_wheel_root_by_subclassed_window.erase(root);

  const auto windows =
      g_muon_windows_subclassed_for_wheel_by_root.find(root_window_handle);
  if (windows == g_muon_windows_subclassed_for_wheel_by_root.end()) {
    return;
  }
  windows->second.erase(window_handle);
  if (windows->second.empty()) {
    g_muon_windows_subclassed_for_wheel_by_root.erase(windows);
  }
}

static void ClearMuonWindowsPendingTitleBarControl(HWND window_handle,
                                                   bool release_capture) {
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    g_muon_windows_pending_title_bar_controls.erase(window_handle);
  }
  if (release_capture && GetCapture() == window_handle) {
    ReleaseCapture();
  }
}

static void ClearMuonWindowsPendingWindowDrag(HWND root_window_handle,
                                              HWND window_handle,
                                              bool release_capture) {
  if (root_window_handle == nullptr) {
    root_window_handle = GetMuonWindowsRootWindow(window_handle);
  }
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    g_muon_windows_pending_window_drags.erase(window_handle);
    if (root_window_handle != window_handle) {
      g_muon_windows_pending_window_drags.erase(root_window_handle);
    }
  }
  if (release_capture && GetCapture() == window_handle) {
    ReleaseCapture();
  }
}

static bool FindMuonWindowsPendingWindowDrag(
    HWND root_window_handle,
    HWND window_handle,
    HWND* pending_key,
    MuonWindowsPendingWindowDrag* pending) {
  if (pending_key == nullptr || pending == nullptr) {
    return false;
  }
  if (root_window_handle == nullptr) {
    root_window_handle = GetMuonWindowsRootWindow(window_handle);
  }
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  auto iterator = g_muon_windows_pending_window_drags.find(root_window_handle);
  if (iterator == g_muon_windows_pending_window_drags.end()) {
    iterator = g_muon_windows_pending_window_drags.find(window_handle);
  }
  if (iterator == g_muon_windows_pending_window_drags.end()) {
    return false;
  }
  *pending_key = iterator->first;
  *pending = iterator->second;
  return true;
}

static void UpdateMuonWindowsPendingWindowDrag(
    HWND pending_key,
    const MuonWindowsPendingWindowDrag& pending) {
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  const auto iterator = g_muon_windows_pending_window_drags.find(pending_key);
  if (iterator != g_muon_windows_pending_window_drags.end()) {
    iterator->second = pending;
  }
}

static bool BeginMuonWindowsTitleBarControl(HWND root_window_handle,
                                            HWND window_handle,
                                            LPARAM lparam) {
  CefPoint screen_point;
  if (!GetMuonWindowsScreenPointFromClientPoint(
          window_handle, lparam, &screen_point)) {
    return false;
  }

  const auto action =
      GetMuonWindowsTitleBarControlAction(root_window_handle, screen_point);
  if (action == MuonTitleBarControlAction::NoControl) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    g_muon_windows_pending_title_bar_controls[window_handle] =
        {root_window_handle, action};
  }
  SetCapture(window_handle);
  return true;
}

static bool CompleteMuonWindowsTitleBarControl(HWND window_handle,
                                               LPARAM lparam) {
  MuonWindowsPendingTitleBarControl pending;
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    const auto iterator =
        g_muon_windows_pending_title_bar_controls.find(window_handle);
    if (iterator == g_muon_windows_pending_title_bar_controls.end()) {
      return false;
    }
    pending = iterator->second;
    g_muon_windows_pending_title_bar_controls.erase(iterator);
  }

  if (GetCapture() == window_handle) {
    ReleaseCapture();
  }

  CefPoint screen_point;
  if (!GetMuonWindowsScreenPointFromClientPoint(
          window_handle, lparam, &screen_point)) {
    return true;
  }
  const auto action = GetMuonWindowsTitleBarControlAction(
      pending.root_window_handle, screen_point);
  if (action == pending.action) {
    HandleRegisteredMuonTitleBarControlAction(
        pending.root_window_handle, pending.action);
  }
  return true;
}

static bool BeginMuonWindowsWindowDragAtScreenPoint(
    HWND root_window_handle,
    HWND window_handle,
    CefPoint screen_point,
    bool page_draggable) {
  const auto drag_handle = root_window_handle != nullptr
                               ? root_window_handle
                               : GetMuonWindowsRootWindow(window_handle);
  const auto should_drag =
      page_draggable
          ? IsNativePageDraggableRegionScreenPixels(drag_handle, screen_point)
          : IsMuonWindowsTitleBarDragRegion(drag_handle, screen_point);
  {
    std::ostringstream log;
    log << "BeginDrag window=" << FormatMuonWindowsHandle(window_handle)
        << " root=" << FormatMuonWindowsHandle(root_window_handle)
        << " drag_handle=" << FormatMuonWindowsHandle(drag_handle)
        << " x=" << screen_point.x << " y=" << screen_point.y
        << " page=" << FormatMuonCloseDebugBool(page_draggable)
        << " should_drag=" << FormatMuonCloseDebugBool(should_drag);
    AppendMuonWindowsDragDebugLog(log.str());
  }
  if (!should_drag) {
    return false;
  }

  RECT window_rect = {};
  if (GetWindowRect(drag_handle, &window_rect) == FALSE) {
    AppendMuonWindowsDragDebugLog("BeginDrag GetWindowRect failed");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    g_muon_windows_pending_window_drags[drag_handle] =
        {drag_handle, screen_point, window_rect};
  }
  SetForegroundWindow(drag_handle);
  SetActiveWindow(drag_handle);
  SetCapture(window_handle);
  AppendMuonWindowsDragDebugLog("BeginDrag captured");
  return true;
}

static bool BeginMuonWindowsWindowDrag(HWND root_window_handle,
                                       HWND window_handle,
                                       LPARAM lparam,
                                       bool page_draggable) {
  CefPoint screen_point;
  if (!GetMuonWindowsScreenPointFromClientPoint(
          window_handle, lparam, &screen_point)) {
    return false;
  }
  return BeginMuonWindowsWindowDragAtScreenPoint(
      root_window_handle, window_handle, screen_point, page_draggable);
}

static bool ContinueMuonWindowsWindowDragAtScreenPoint(
    HWND root_window_handle,
    HWND window_handle,
    CefPoint screen_point,
    bool left_button_pressed) {
  MuonWindowsPendingWindowDrag pending;
  HWND pending_key = nullptr;
  if (!FindMuonWindowsPendingWindowDrag(
          root_window_handle, window_handle, &pending_key, &pending)) {
    AppendMuonWindowsDragDebugLog("ContinueDrag no pending");
    return false;
  }

  const auto delta_x = screen_point.x - pending.start_screen_point.x;
  const auto delta_y = screen_point.y - pending.start_screen_point.y;
  if (!left_button_pressed) {
    if (pending.released_before_move &&
        GetTickCount() - pending.release_tick <=
            kMuonWindowsReleasedDragGraceMs &&
        (delta_x != 0 || delta_y != 0)) {
      SetWindowPos(
          pending.root_window_handle, nullptr,
          pending.start_window_rect.left + delta_x,
          pending.start_window_rect.top + delta_y,
          0, 0,
          SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    AppendMuonWindowsDragDebugLog("ContinueDrag left released");
    ClearMuonWindowsPendingWindowDrag(root_window_handle, window_handle, true);
    return true;
  }

  {
    std::ostringstream log;
    log << "ContinueDrag window=" << FormatMuonWindowsHandle(window_handle)
        << " root=" << FormatMuonWindowsHandle(pending.root_window_handle)
        << " x=" << screen_point.x << " y=" << screen_point.y
        << " dx=" << delta_x << " dy=" << delta_y;
    AppendMuonWindowsDragDebugLog(log.str());
  }
  SetWindowPos(
      pending.root_window_handle, nullptr,
      pending.start_window_rect.left + delta_x,
      pending.start_window_rect.top + delta_y,
      0, 0,
      SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
  pending.moved = true;
  pending.released_before_move = false;
  UpdateMuonWindowsPendingWindowDrag(pending_key, pending);
  return true;
}

static bool ContinueMuonWindowsWindowDrag(HWND window_handle,
                                          HWND root_window_handle,
                                          WPARAM wparam,
                                          LPARAM lparam) {
  CefPoint screen_point;
  if (!GetMuonWindowsScreenPointFromClientPoint(
          window_handle, lparam, &screen_point)) {
    return true;
  }
  return ContinueMuonWindowsWindowDragAtScreenPoint(
      root_window_handle, window_handle, screen_point,
      (wparam & MK_LBUTTON) != 0);
}

static bool CompleteMuonWindowsWindowDrag(HWND root_window_handle,
                                          HWND window_handle) {
  MuonWindowsPendingWindowDrag pending;
  HWND pending_key = nullptr;
  if (!FindMuonWindowsPendingWindowDrag(
          root_window_handle, window_handle, &pending_key, &pending)) {
    return false;
  }
  if (!pending.moved && !pending.released_before_move) {
    pending.released_before_move = true;
    pending.release_tick = GetTickCount();
    UpdateMuonWindowsPendingWindowDrag(pending_key, pending);
    return true;
  }
  ClearMuonWindowsPendingWindowDrag(root_window_handle, window_handle, true);
  return true;
}

static bool StartMuonWindowsPageDrag(HWND root_window_handle,
                                     HWND window_handle,
                                     LPARAM lparam) {
  return BeginMuonWindowsWindowDrag(
      root_window_handle, window_handle, lparam, true);
}

static bool StartMuonWindowsTitleBarDrag(HWND root_window_handle,
                                         HWND window_handle,
                                         LPARAM lparam) {
  return BeginMuonWindowsWindowDrag(
      root_window_handle, window_handle, lparam, false);
}

static BOOL CALLBACK AppendMuonWindowsChildForwarderTarget(
    HWND child_window_handle,
    LPARAM user_data) {
  auto* child_window_handles =
      reinterpret_cast<std::vector<CefWindowHandle>*>(user_data);
  child_window_handles->push_back(child_window_handle);
  return TRUE;
}

static std::vector<HWND> GetMuonWindowsForwarderTargets(
    HWND root_window_handle) {
  std::vector<CefWindowHandle> child_window_handles;
  EnumChildWindows(root_window_handle, AppendMuonWindowsChildForwarderTarget,
                   reinterpret_cast<LPARAM>(&child_window_handles));

  std::vector<HWND> targets;
  const auto handles = GetMuonNativeForwarderWindowHandlesForRegistration(
      root_window_handle, child_window_handles);
  targets.reserve(handles.size());
  for (const auto handle : handles) {
    targets.push_back(handle);
  }
  return targets;
}

static bool IsMuonWindowsWheelForwarderRootRegistered(
    HWND root_window_handle) {
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  return g_muon_windows_subclassed_for_wheel_by_root.find(
             root_window_handle) !=
         g_muon_windows_subclassed_for_wheel_by_root.end();
}

static bool IsMuonWindowsWheelForwarderTargetRegisteredForRoot(
    HWND root_window_handle,
    HWND target_window_handle) {
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  const auto root =
      g_muon_windows_wheel_root_by_subclassed_window.find(target_window_handle);
  return root != g_muon_windows_wheel_root_by_subclassed_window.end() &&
         root->second == root_window_handle;
}

static std::vector<HWND> GetMuonWindowsWheelForwarderTargetsForRoot(
    HWND root_window_handle) {
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  std::vector<HWND> window_handles;
  const auto windows =
      g_muon_windows_subclassed_for_wheel_by_root.find(root_window_handle);
  if (windows == g_muon_windows_subclassed_for_wheel_by_root.end()) {
    return window_handles;
  }
  window_handles.assign(windows->second.begin(), windows->second.end());
  return window_handles;
}

static void RegisterMuonWindowsWheelForwarderTarget(HWND root_window_handle,
                                                    HWND target_window_handle) {
  if (root_window_handle == nullptr || target_window_handle == nullptr) {
    return;
  }
  if (SetWindowSubclass(target_window_handle, MuonWheelForwarderSubclassProc,
                        kMuonWheelForwarderSubclassId,
                        reinterpret_cast<DWORD_PTR>(
                            root_window_handle)) == FALSE) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
  const auto previous_root =
      g_muon_windows_wheel_root_by_subclassed_window.find(
          target_window_handle);
  if (previous_root != g_muon_windows_wheel_root_by_subclassed_window.end() &&
      previous_root->second != root_window_handle) {
    const auto previous_windows =
        g_muon_windows_subclassed_for_wheel_by_root.find(
            previous_root->second);
    if (previous_windows != g_muon_windows_subclassed_for_wheel_by_root.end()) {
      previous_windows->second.erase(target_window_handle);
      if (previous_windows->second.empty()) {
        g_muon_windows_subclassed_for_wheel_by_root.erase(previous_windows);
      }
    }
  }
  g_muon_windows_wheel_root_by_subclassed_window[target_window_handle] =
      root_window_handle;
  g_muon_windows_subclassed_for_wheel_by_root[root_window_handle].insert(
      target_window_handle);
}

static void RefreshMuonWindowsWheelForwarder(HWND root_window_handle,
                                             HWND window_handle) {
  if (root_window_handle == nullptr) {
    root_window_handle = GetMuonWindowsRootWindow(window_handle);
  }
  if (!IsMuonWindowsWheelForwarderRootRegistered(root_window_handle)) {
    return;
  }
  for (const auto target_window_handle :
       GetMuonWindowsForwarderTargets(root_window_handle)) {
    RegisterMuonWindowsWheelForwarderTarget(root_window_handle,
                                            target_window_handle);
  }
}

static LRESULT CALLBACK MuonWheelForwarderSubclassProc(
    HWND window_handle,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR ref_data) {
  if (subclass_id != kMuonWheelForwarderSubclassId) {
    return DefSubclassProc(window_handle, message, wparam, lparam);
  }
  const auto root_window_handle =
      GetMuonWindowsSubclassRootWindow(window_handle, ref_data);

  switch (message) {
    case WM_NCHITTEST: {
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      const auto title_hit =
          IsMuonWindowsTitleBarDragRegion(root_window_handle, screen_point);
      const auto page_hit =
          IsNativePageDraggableRegionScreenPixels(
              root_window_handle, screen_point);
      if (title_hit || page_hit) {
        std::ostringstream log;
        log << "NCHITTEST window=" << FormatMuonWindowsHandle(window_handle)
            << " root=" << FormatMuonWindowsHandle(root_window_handle)
            << " x=" << screen_point.x << " y=" << screen_point.y
            << " title=" << FormatMuonCloseDebugBool(title_hit)
            << " page=" << FormatMuonCloseDebugBool(page_hit);
        AppendMuonWindowsDragDebugLog(log.str());
        return HTCLIENT;
      }
      break;
    }
    case WM_LBUTTONDOWN:
      AppendMuonWindowsDragDebugLog("LBUTTONDOWN");
      if (BeginMuonWindowsTitleBarControl(
              root_window_handle, window_handle, lparam)) {
        return 0;
      }
      if (StartMuonWindowsTitleBarDrag(
              root_window_handle, window_handle, lparam)) {
        return 0;
      }
      if (StartMuonWindowsPageDrag(
              root_window_handle, window_handle, lparam)) {
        return 0;
      }
      break;
    case WM_NCLBUTTONDOWN: {
      AppendMuonWindowsDragDebugLog("NCLBUTTONDOWN");
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (BeginMuonWindowsWindowDragAtScreenPoint(
              root_window_handle, window_handle, screen_point, false)) {
        return 0;
      }
      if (BeginMuonWindowsWindowDragAtScreenPoint(
              root_window_handle, window_handle, screen_point, true)) {
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE:
      AppendMuonWindowsDragDebugLog("MOUSEMOVE");
      if (ContinueMuonWindowsWindowDrag(
              window_handle, root_window_handle, wparam, lparam)) {
        return 0;
      }
      break;
    case WM_NCMOUSEMOVE: {
      AppendMuonWindowsDragDebugLog("NCMOUSEMOVE");
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (ContinueMuonWindowsWindowDragAtScreenPoint(
              root_window_handle, window_handle, screen_point,
              (GetKeyState(VK_LBUTTON) & 0x8000) != 0)) {
        return 0;
      }
      break;
    }
    case WM_LBUTTONUP:
      AppendMuonWindowsDragDebugLog("LBUTTONUP");
      if (CompleteMuonWindowsWindowDrag(root_window_handle, window_handle)) {
        return 0;
      }
      if (CompleteMuonWindowsTitleBarControl(window_handle, lparam)) {
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      AppendMuonWindowsDragDebugLog("NCLBUTTONUP");
      if (CompleteMuonWindowsWindowDrag(root_window_handle, window_handle)) {
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      ClearMuonWindowsPendingTitleBarControl(window_handle, false);
      ClearMuonWindowsPendingWindowDrag(
          root_window_handle, window_handle, false);
      break;
    case WM_CANCELMODE:
      ClearMuonWindowsPendingTitleBarControl(window_handle, true);
      ClearMuonWindowsPendingWindowDrag(
          root_window_handle, window_handle, true);
      break;
    case WM_PARENTNOTIFY:
      if (LOWORD(wparam) == WM_CREATE) {
        RefreshMuonWindowsWheelForwarder(root_window_handle, window_handle);
      }
      break;
    case WM_MOUSEWHEEL: {
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (ForwardNativeWheelFromScreenPoint(
              root_window_handle, screen_point, 0,
              GET_WHEEL_DELTA_WPARAM(wparam),
              GetMuonWindowsEventFlags(wparam))) {
        return 0;
      }
      break;
    }
    case WM_MOUSEHWHEEL: {
      const auto screen_point =
          CefPoint(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (ForwardNativeWheelFromScreenPoint(
              root_window_handle, screen_point,
              -GET_WHEEL_DELTA_WPARAM(wparam), 0,
              GetMuonWindowsEventFlags(wparam))) {
        return 0;
      }
      break;
    }
    case WM_NCDESTROY:
      ClearMuonWindowsPendingTitleBarControl(window_handle, true);
      ClearMuonWindowsPendingWindowDrag(
          root_window_handle, window_handle, true);
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
  const auto root_window_handle = GetMuonWindowsRootWindow(window_handle);
  if (root_window_handle == nullptr) {
    return;
  }
  for (const auto target_window_handle :
       GetMuonWindowsForwarderTargets(root_window_handle)) {
    RegisterMuonWindowsWheelForwarderTarget(root_window_handle,
                                            target_window_handle);
  }
}

static void UnregisterMuonWindowsWheelForwarder(HWND window_handle) {
  const auto root_window_handle = GetMuonWindowsRootWindow(window_handle);
  if (root_window_handle == nullptr) {
    return;
  }
  const auto window_handles =
      GetMuonWindowsWheelForwarderTargetsForRoot(root_window_handle);
  for (const auto target_window_handle : window_handles) {
    if (!IsMuonWindowsWheelForwarderTargetRegisteredForRoot(
            root_window_handle, target_window_handle)) {
      continue;
    }
    if (IsWindow(target_window_handle) != FALSE) {
      RemoveWindowSubclass(target_window_handle, MuonWheelForwarderSubclassProc,
                           kMuonWheelForwarderSubclassId);
    }
    ForgetMuonWindowsSubclassForRoot(root_window_handle, target_window_handle);
  }
}

static void ClearMuonWindowsWheelForwarders() {
  std::vector<HWND> window_handles;
  {
    std::lock_guard<std::mutex> lock(g_muon_windows_subclass_mutex);
    window_handles.reserve(
        g_muon_windows_wheel_root_by_subclassed_window.size());
    for (const auto& window :
         g_muon_windows_wheel_root_by_subclassed_window) {
      window_handles.push_back(window.first);
    }
    g_muon_windows_wheel_root_by_subclassed_window.clear();
    g_muon_windows_subclassed_for_wheel_by_root.clear();
    g_muon_windows_pending_title_bar_controls.clear();
    g_muon_windows_pending_window_drags.clear();
  }
  for (const auto window_handle : window_handles) {
    if (IsWindow(window_handle) != FALSE) {
      RemoveWindowSubclass(window_handle, MuonWheelForwarderSubclassProc,
                           kMuonWheelForwarderSubclassId);
    }
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
    ForwardNativeWheelFromScreenPoint(
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

static CefWindowHandle GetRegisteredMuonX11WheelWindowAtRootPoint(
    Display* display,
    CefPoint root_point,
    const std::vector<CefWindowHandle>& registered_window_handles) {
  if (display == nullptr) {
    return 0;
  }

  const auto root_window = DefaultRootWindow(display);
  Window root_return = None;
  Window parent_return = None;
  Window* children = nullptr;
  unsigned int child_count = 0;
  if (XQueryTree(display, root_window, &root_return, &parent_return,
                 &children, &child_count) == False) {
    return 0;
  }

  std::vector<MuonNativeWheelForwarderTopLevelWindow> top_level_windows;
  top_level_windows.reserve(child_count);
  for (auto index = 0u; index < child_count; ++index) {
    XWindowAttributes attributes;
    if (XGetWindowAttributes(display, children[index], &attributes) ==
        False) {
      continue;
    }

    int root_x = 0;
    int root_y = 0;
    Window translated_child = None;
    if (XTranslateCoordinates(display, children[index], root_window, 0, 0,
                              &root_x, &root_y,
                              &translated_child) == False) {
      continue;
    }

    top_level_windows.push_back(
        {static_cast<CefWindowHandle>(children[index]), root_x, root_y,
         attributes.width, attributes.height,
         attributes.map_state == IsViewable,
         attributes.override_redirect != False});
  }
  if (children != nullptr) {
    XFree(children);
  }

  return GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
      root_point, registered_window_handles, top_level_windows);
}

static bool IsMuonNativeWheelForwarderWindowRegistered(
    CefWindowHandle window_handle,
    const std::vector<CefWindowHandle>& registered_window_handles) {
  return window_handle != 0 &&
         std::find(registered_window_handles.begin(),
                   registered_window_handles.end(),
                   window_handle) != registered_window_handles.end();
}

static void PostMuonX11WheelEvent(Display* display,
                                  const XIDeviceEvent* event) {
  int delta_x = 0;
  int delta_y = 0;
  if (!GetMuonX11WheelDeltas(event->detail, &delta_x, &delta_y)) {
    return;
  }

  std::vector<CefWindowHandle> registered_window_handles;
  {
    std::lock_guard<std::mutex> lock(g_muon_x11_wheel_mutex);
    registered_window_handles.assign(g_muon_x11_wheel_windows.begin(),
                                     g_muon_x11_wheel_windows.end());
  }
  const auto window_handle =
      GetMuonNativeWheelForwarderTargetWindowHandle(
          static_cast<CefWindowHandle>(event->event),
          static_cast<CefWindowHandle>(event->child != None ? event->child : 0),
          registered_window_handles);
  const auto screen_point =
      CefPoint(static_cast<int>(std::lround(event->root_x)),
               static_cast<int>(std::lround(event->root_y)));
  auto target_window_handle = window_handle;
  if (!IsMuonNativeWheelForwarderWindowRegistered(
          target_window_handle, registered_window_handles)) {
    const auto point_window_handle =
        GetRegisteredMuonX11WheelWindowAtRootPoint(
            display, screen_point, registered_window_handles);
    if (point_window_handle != 0) {
      target_window_handle = point_window_handle;
    }
  }
  CefPostTask(
      TID_UI,
      new MuonForwardX11WheelTask(target_window_handle, screen_point, delta_x,
                                  delta_y, GetMuonX11ButtonEventFlags(event)));
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
        display,
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

std::vector<CefWindowHandle> GetMuonNativeForwarderWindowHandlesForRegistration(
    CefWindowHandle root_window_handle,
    const std::vector<CefWindowHandle>& child_window_handles) {
  std::set<CefWindowHandle> seen;
  std::vector<CefWindowHandle> handles;
  if (root_window_handle != 0 && seen.insert(root_window_handle).second) {
    handles.push_back(root_window_handle);
  }
  for (const auto child_window_handle : child_window_handles) {
    if (child_window_handle == 0 ||
        !seen.insert(child_window_handle).second) {
      continue;
    }
    handles.push_back(child_window_handle);
  }
  return handles;
}

CefWindowHandle GetMuonNativeWheelForwarderTargetWindowHandle(
    CefWindowHandle event_window_handle,
    CefWindowHandle child_window_handle,
    const std::vector<CefWindowHandle>& registered_window_handles) {
  if (event_window_handle != 0 &&
      std::find(registered_window_handles.begin(),
                registered_window_handles.end(),
                event_window_handle) != registered_window_handles.end()) {
    return event_window_handle;
  }
  if (child_window_handle != 0 &&
      std::find(registered_window_handles.begin(),
                registered_window_handles.end(),
                child_window_handle) != registered_window_handles.end()) {
    return child_window_handle;
  }
  return child_window_handle != 0 ? child_window_handle : event_window_handle;
}

CefWindowHandle GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
    CefPoint screen_point,
    const std::vector<CefWindowHandle>& registered_window_handles,
    const std::vector<MuonNativeWheelForwarderTopLevelWindow>&
        top_level_windows) {
  for (auto iterator = top_level_windows.rbegin();
       iterator != top_level_windows.rend(); ++iterator) {
    if (!iterator->visible || iterator->override_redirect ||
        iterator->width <= 0 || iterator->height <= 0) {
      continue;
    }
    if (screen_point.x < iterator->x ||
        screen_point.y < iterator->y ||
        screen_point.x >= iterator->x + iterator->width ||
        screen_point.y >= iterator->y + iterator->height) {
      continue;
    }
    if (std::find(registered_window_handles.begin(),
                  registered_window_handles.end(),
                  iterator->window_handle) !=
        registered_window_handles.end()) {
      return iterator->window_handle;
    }
    return 0;
  }
  return 0;
}

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
