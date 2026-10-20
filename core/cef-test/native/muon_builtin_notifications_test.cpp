/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "plugins/builtin/muon_builtin_notifications.h"
#include "config/muon_config.h"
#include <cardio.h>
#include <gio/gio.h>
#include <iostream>
#include <stdexcept>
#include <string>

static bool completed = false;
static bool rejected = false;
static GMainLoop* loop = nullptr;
static void Complete(const void*, const char* error) {
  completed = true;
  rejected = error != nullptr;
  g_main_loop_quit(loop);
}
int main() {
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  loop = g_main_loop_new(nullptr, FALSE);
  auto passed = false;
  try {
    MuonConfig config;
    InitializeMuonBuiltinNotifications(config);
    if (GetMuonBuiltinNotificationsMetadata()) throw std::runtime_error("disabled notifications were exposed");
    config.notifications.enabled = true;
    InitializeMuonBuiltinNotifications(config);
    const auto* metadata = GetMuonBuiltinNotificationsMetadata();
    if (!metadata) throw std::runtime_error("enabled notifications were missing");
    for (auto item = metadata->namespaces[0]->functions; *item; ++item) {
      const auto& function = **item;
      if (function.signature.arg_count == 0) continue;
      completed = rejected = false;
      if (std::string(function.filter_name) == "watchEvents") {
        const auto invoke = reinterpret_cast<void (*)(muon_completion_func, const char*, const char*)>(function.native_func);
        invoke(&Complete, nullptr, "trusted-owner");
      } else {
        const auto invoke = reinterpret_cast<void (*)(muon_completion_func, const char*)>(function.native_func);
        invoke(&Complete, nullptr);
      }
      if (!completed) g_main_loop_run(loop);
      if (!rejected) throw std::runtime_error("null native notification argument was not rejected");
    }
    passed = true;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  ShutdownMuonBuiltinNotifications();
  g_main_loop_unref(loop);
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  return passed ? 0 : 1;
}
