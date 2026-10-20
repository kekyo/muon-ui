/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_maintenance.h"
#include "notifications/muon_notification_store.h"
#include <gio/gio.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace muon_internal;

static void Expect(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
static cardio::promise<void> RunTests(std::filesystem::path directory) {
  const auto path = (directory / "notifications.json").string();
  auto initial = MuonNotificationSnapshot{};
  auto post = ParseMuonNotificationPost(R"({"id":"owned","title":"Owned","body":""})", "");
  ReserveMuonNotification(initial, post, std::string(64, 'a'), 1);
  initial.revisions[0].handle = {"portal", "dev.muon.Test/" + std::string(64, 'a'), 1};
  post.id = "pending";
  ReserveMuonNotification(initial, post, std::string(64, 'b'), 1);
  ActivateMuonNotification(initial, std::string(64, 'b'), std::nullopt, 2);
  post.id = "acknowledged";
  ReserveMuonNotification(initial, post, std::string(64, 'c'), 1);
  ActivateMuonNotification(initial, std::string(64, 'c'), std::nullopt, 2);
  AcknowledgeMuonNotification(initial, std::string(64, 'c'), 3);
  co_await SaveMuonNotificationSnapshot(path, initial);
  const auto saved = SerializeMuonNotificationSnapshot(initial);

  auto owned = false;
  auto fail_cancel = false;
  auto stopped = 0;
  auto canceled = std::vector<std::string>{};
  auto backend = MuonNotificationBackend{};
  backend.capabilities = [&]() -> cardio::promise<MuonNotificationBackendCapabilities> {
    auto caps = MuonNotificationBackendCapabilities{true, true, true, ""};
    caps.launch = owned;
    co_return caps;
  };
  backend.cancel = [&](MuonNotificationHandle handle) -> cardio::promise<void> {
    if (fail_cancel) throw std::runtime_error("OS cancellation failed");
    canceled.push_back(handle.owner);
    co_return;
  };
  backend.stop = [&] { ++stopped; };
  auto rejected = false;
  try { co_await ClearMuonNotificationHistory(path, backend); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected && stopped == 1 && canceled.empty(),
         "Maintenance changed state without owning the launch registration");
  Expect(SerializeMuonNotificationSnapshot(co_await LoadMuonNotificationSnapshot(path)) == saved,
         "Failed ownership check changed the persisted state");
  owned = true;
  fail_cancel = true;
  rejected = false;
  try { co_await ClearMuonNotificationHistory(path, backend); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected && stopped == 2, "Failed OS cancellation was not reported");
  Expect(SerializeMuonNotificationSnapshot(co_await LoadMuonNotificationSnapshot(path)) == saved,
         "Failed OS cancellation destroyed retry information");
  fail_cancel = false;
  co_await ClearMuonNotificationHistory(path, backend);
  Expect(stopped == 3 && canceled == std::vector<std::string>{initial.revisions[0].handle.owner},
         "Maintenance did not cancel exactly the owned notification");
  auto remaining = co_await LoadMuonNotificationSnapshot(path);
  initial.revisions.clear();
  Expect(SerializeMuonNotificationSnapshot(remaining) == SerializeMuonNotificationSnapshot(initial),
         "Maintenance removed pending events or acknowledgements");
  co_await ClearMuonNotificationHistory(path, backend);
  Expect(stopped == 4 && canceled.size() == 1, "Maintenance retry was not idempotent");
}
static cardio::promise<void> RunAndQuit(std::filesystem::path directory, GMainLoop* loop, bool& passed) {
  try { co_await RunTests(std::move(directory)); passed = true; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  g_main_loop_quit(loop);
}
int main() {
  auto* path = g_dir_make_tmp("muon-notification-maintenance-XXXXXX", nullptr);
  if (!path) return 1;
  const auto directory = std::filesystem::path(path);
  g_free(path);
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto* loop = g_main_loop_new(nullptr, FALSE);
  auto passed = false;
  auto pending = RunAndQuit(directory, loop, passed);
  if (!pending.is_ready()) g_main_loop_run(loop);
  g_main_loop_unref(loop);
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
