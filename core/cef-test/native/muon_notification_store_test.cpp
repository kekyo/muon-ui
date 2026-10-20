/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_store.h"
#include <gio/gio.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace muon_internal;

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> RunTests(std::filesystem::path directory) {
  const auto path = (directory / "nested" / "events.json").string();
  auto state = co_await LoadMuonNotificationSnapshot(path);
  Expect(state.pending.empty(), "absent store was not empty");
  const auto token = co_await CreateMuonNotificationToken();
  const auto other_token = co_await CreateMuonNotificationToken();
  Expect(token.size() == 64 && token != other_token, "revision tokens are invalid");
  auto post = ParseMuonNotificationPost(
      R"({"id":"inbox","title":"Message","body":"","data":{"version":1}})", "");
  ReserveMuonNotification(state, post, token, 1);
  Expect(ActivateMuonNotification(state, token, std::nullopt, 2), "activation failed");
  co_await SaveMuonNotificationSnapshot(path, state);
  auto reloaded = co_await LoadMuonNotificationSnapshot(path);
  Expect(reloaded.pending.size() == 1 && reloaded.pending[0].event_id == token,
         "saved event was not retained after reload");
  // A directory at the private staging path makes a real OS write fail.
  std::filesystem::create_directory(path + ".next");
  AcknowledgeMuonNotification(state, token, 3);
  auto rejected = false;
  try { co_await SaveMuonNotificationSnapshot(path, state); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected, "failed save was reported as successful");
  reloaded = co_await LoadMuonNotificationSnapshot(path);
  Expect(reloaded.pending.size() == 1, "failed save destroyed the previous event");
  std::filesystem::remove(path + ".next");
  co_await SaveMuonNotificationSnapshot(path, state);
  reloaded = co_await LoadMuonNotificationSnapshot(path);
  Expect(reloaded.pending.empty() && reloaded.acknowledged.size() == 1,
         "ACK was lost after reload");
  std::ofstream(path, std::ios::trunc) << "{corrupt";
  rejected = false;
  try { (void)co_await LoadMuonNotificationSnapshot(path); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected && std::filesystem::file_size(path) == 8,
         "corrupt store was reset or not diagnosed");
}

static cardio::promise<void> RunAndQuit(std::filesystem::path directory,
                                       GMainLoop* loop, bool& passed) {
  try { co_await RunTests(std::move(directory)); passed = true; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  g_main_loop_quit(loop);
}

int main() {
  auto* path = g_dir_make_tmp("muon-notification-store-XXXXXX", nullptr);
  if (path == nullptr) return 1;
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
