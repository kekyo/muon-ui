/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_service.h"
#include "notifications/muon_notification_store.h"
#include "yyjson.h"
#include <gio/gio.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace muon_internal;

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static std::string Field(const std::string& json, const char* field) {
  auto* document = yyjson_read(json.data(), json.size(), 0);
  if (!document) throw std::runtime_error("Invalid response JSON");
  auto* value = yyjson_obj_get(yyjson_doc_get_root(document), field);
  auto result = yyjson_is_str(value) ? std::string(yyjson_get_str(value)) : std::string{};
  yyjson_doc_free(document);
  return result;
}

static cardio::promise<void> RunTests(std::filesystem::path directory) {
  auto activation = MuonNotificationActivation{};
  auto posted = std::vector<std::string>{};
  auto canceled = std::vector<uint32_t>{};
  auto fail_cancel = false;
  auto fail_post = false;
  auto backend = MuonNotificationBackend{};
  backend.capabilities = []() -> cardio::promise<MuonNotificationBackendCapabilities> {
    co_return MuonNotificationBackendCapabilities{true, true, true, ""};
  };
  backend.post = [&](MuonNotificationPost, std::string token) -> cardio::promise<MuonNotificationHandle> {
    if (fail_post) throw std::runtime_error("OS submission failed");
    posted.push_back(std::move(token));
    co_return MuonNotificationHandle{"test", ":1.2", static_cast<uint32_t>(posted.size())};
  };
  backend.cancel = [&](MuonNotificationHandle handle) -> cardio::promise<void> {
    if (fail_cancel) throw std::runtime_error("OS cancellation failed");
    canceled.push_back(handle.id); co_return;
  };
  backend.set_activation = [&](MuonNotificationActivation callback) { activation = std::move(callback); };
  backend.stop = [&] { activation = {}; };
  const auto path = (directory / "events.json").string();
  auto service = std::make_shared<MuonNotificationService>(path, "general", backend, [] { return 10; });
  service->Start();
  co_await service->Post(R"({"id":"id","title":"First","body":"","data":1})");
  co_await service->Post(R"({"id":"id","title":"Second","body":"","data":2})");
  Expect(posted.size() == 2 && canceled == std::vector<uint32_t>{1}, "update did not retire only the old OS ID");
  const auto lease = co_await service->WatchEvents(R"({"operation":"acquire"})", "1:frame:1");
  const auto token = Field(lease, "token");
  Expect(!token.empty(), "watch lease was not allocated");
  auto next = service->WatchEvents("{\"operation\":\"next\",\"token\":\"" + token + "\",\"afterCursor\":0}", "1:frame:1");
  auto rejected = false;
  try {
    co_await service->WatchEvents("{\"operation\":\"release\",\"token\":\"" + token + "\"}", "2:frame:1");
  } catch (const std::exception&) { rejected = true; }
  Expect(rejected, "another renderer released a watch");
  activation(posted[0], std::nullopt);
  activation(posted[1], std::nullopt);
  const auto batch = co_await next;
  Expect(batch.find("\"data\":2") != std::string::npos &&
             batch.find(posted[0]) == std::string::npos,
         "watch delivered stale revision data");
  const auto pending = co_await service->GetPendingEvents();
  Expect(pending.find(posted[1]) != std::string::npos, "event was not durable");
  auto held = service->WatchEvents("{\"operation\":\"next\",\"token\":\"" + token + "\",\"afterCursor\":1}", "1:frame:1");
  service->ReleaseOwner("1:frame:1");
  const auto terminal = co_await held;
  Expect(terminal.find("\"events\":null") != std::string::npos, "context release did not finish a held wait");
  co_await service->Stop();
  service = std::make_shared<MuonNotificationService>(path, "general", backend, [] { return 11; });
  service->Start();
  Expect((co_await service->GetPendingEvents()).find(posted[1]) != std::string::npos,
         "window-independent event was lost on reopen");
  co_await service->AcknowledgeEvent(posted[1]);
  co_await service->Stop();
  const auto snapshot = co_await LoadMuonNotificationSnapshot(path);
  Expect(snapshot.pending.empty(), "ACK did not persist");
  service = std::make_shared<MuonNotificationService>(path, "general", backend, [] { return 12; });
  service->Start();
  co_await service->Post(R"({"id":"retry-cancel","title":"Retry","body":""})");
  const auto cancel_count = canceled.size();
  fail_cancel = true;
  rejected = false;
  try { co_await service->Cancel("retry-cancel"); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected, "failed OS cancel succeeded");
  fail_cancel = false;
  co_await service->Cancel("retry-cancel");
  Expect(canceled.size() == cancel_count + 1, "failed OS cancel could not be retried");
  co_await service->Post(R"({"id":"shutdown","title":"Shutdown","body":""})");
  activation(posted.back(), std::nullopt);
  co_await service->Stop();
  const auto stopped_snapshot = co_await LoadMuonNotificationSnapshot(path);
  Expect(stopped_snapshot.pending.size() == 1 && stopped_snapshot.pending[0].id == "shutdown",
         "shutdown lost an already accepted activation");

  service = std::make_shared<MuonNotificationService>((directory / "failures.json").string(),
      "general", backend, [] { return 20; });
  service->Start();
  auto acquiring = service->WatchEvents(R"({"operation":"acquire"})", "gone");
  service->ReleaseOwner("gone");
  rejected = false;
  try { co_await acquiring; } catch (const std::exception&) { rejected = true; }
  Expect(rejected, "page destruction leaked an in-flight acquire");
  co_await service->Post(R"({"id":"failure","title":"Saved","body":"","data":42})");
  const auto saved_token = posted.back();
  fail_post = true;
  rejected = false;
  try { co_await service->Post(R"({"id":"failure","title":"Failed update","body":"","data":99})"); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected, "failed submission succeeded");
  fail_post = false;
  const auto live = Field(co_await service->WatchEvents(R"({"operation":"acquire"})", "live"), "token");
  auto waiting = service->WatchEvents("{\"operation\":\"next\",\"token\":\"" + live + "\",\"afterCursor\":0}", "live");
  rejected = false;
  try { co_await service->WatchEvents("{\"operation\":\"next\",\"token\":\"" + live + "\",\"afterCursor\":0}", "live"); }
  catch (const std::exception&) { rejected = true; }
  Expect(rejected, "concurrent next waits were accepted");
  std::filesystem::create_directory(directory / "failures.json.next");
  activation(saved_token, std::nullopt);
  // This read is ordered after the signal's reserved queue slot.
  Expect((co_await service->GetPendingEvents()) == "[]" && !waiting.is_ready(),
         "failed persistence was delivered as a durable event");
  std::filesystem::remove(directory / "failures.json.next");
  fail_cancel = true;
  activation(saved_token, std::nullopt);
  const auto delivered = co_await waiting;
  Expect(delivered.find("\"data\":42") != std::string::npos,
         "rollback lost the old revision or OS cancellation suppressed durable delivery");
  Expect((co_await service->GetPendingEvents()).find(saved_token) != std::string::npos,
         "failed OS cancellation lost the durable event");
  fail_cancel = false;
  auto terminal_wait = service->WatchEvents("{\"operation\":\"next\",\"token\":\"" + live + "\",\"afterCursor\":1}", "live");
  co_await service->WatchEvents("{\"operation\":\"release\",\"token\":\"" + live + "\"}", "live");
  Expect((co_await terminal_wait).find("\"events\":null") != std::string::npos,
         "explicit release did not finish a held wait");
  co_await service->Stop();
}

static cardio::promise<void> RunAndQuit(std::filesystem::path directory,
                                       GMainLoop* loop, bool& passed) {
  try { co_await RunTests(std::move(directory)); passed = true; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  g_main_loop_quit(loop);
}

int main() {
  auto* path = g_dir_make_tmp("muon-notification-service-XXXXXX", nullptr);
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
