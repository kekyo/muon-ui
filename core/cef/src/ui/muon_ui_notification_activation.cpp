/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "ui/muon_ui_notification_activation.h"

#if defined(__linux__)
#include <gdk/gdkx.h>
#include <charconv>
#include <string_view>
#endif

extern "C" int muon_ui_notification_activate_x11(
    uint64_t window, const char* startup_id) {
#if defined(__linux__)
  if (window == 0 || startup_id == nullptr) return 0;
  const auto id = std::string_view(startup_id);
  const auto marker = id.rfind("_TIME");
  if (marker == std::string_view::npos) return 0;
  const auto suffix = id.substr(marker + 5);
  auto timestamp = uint32_t{};
  const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), timestamp);
  if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) return 0;
  auto* display = gdk_display_get_default();
  if (!display || !GDK_IS_X11_DISPLAY(display)) return 0;
  auto* target = gdk_x11_window_foreign_new_for_display(display, window);
  if (!target) return 0;
  // CEF does not expose the timestamp accepted by the window manager. Preserve
  // the user's real notification interaction time instead of using stale input.
  gdk_window_set_startup_id(target, startup_id);
  gdk_window_focus(target, timestamp);
  gdk_display_flush(display);
  g_object_unref(target);
  return 1;
#else
  (void)window;
  (void)startup_id;
  return 0;
#endif
}
