/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_service.h"
#include "notifications/muon_notification_store.h"
#include "yyjson.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <gio/gio.h>
#endif
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
  auto stop_count = 0;
  auto activated_count = 0;
  auto startup_ids = std::vector<std::string>{};
  auto capabilities = MuonNotificationBackendCapabilities{true, true, true, "", true};
  capabilities.launch = true;
  capabilities.launch_reason.clear();
  auto status = MuonNotificationBackendStatus{true, ""};
  auto backend = MuonNotificationBackend{};
  backend.capabilities = [&]() -> cardio::promise<MuonNotificationBackendCapabilities> {
    co_return capabilities;
  };
  backend.status = [&]() -> cardio::promise<MuonNotificationBackendStatus> { co_return status; };
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
  backend.stop = [&] { activation = {}; ++stop_count; };
  const auto path = (directory / "events.json").string();
  {
    auto abandoned = std::make_shared<MuonNotificationService>(path, "general", backend, [] { return 1; });
    abandoned->Start();
    const auto lease = Field(co_await abandoned->WatchEvents(R"({"operation":"acquire"})", "abandoned"), "token");
    auto held = abandoned->WatchEvents("{\"operation\":\"next\",\"token\":\"" + lease + "\",\"afterCursor\":0}", "abandoned");
    abandoned.reset();
    Expect(stop_count == 1 && !activation,
           "destruction without Stop retained the backend");
    Expect((co_await held).find("\"events\":null") != std::string::npos,
           "destruction did not terminate a held watch");
  }
  {
    auto stopped = std::make_shared<MuonNotificationService>(path, "general", backend, [] { return 1; });
    stopped->Start();
    auto window_owner = std::make_shared<int>(0);
    auto weak_window_owner = std::weak_ptr(window_owner);
    stopped->SetActivationHandler([window_owner](std::string) { ++*window_owner; });
    window_owner.reset();
    co_await stopped->Stop();
    Expect(weak_window_owner.expired(), "stopping retained the window presentation owner");
    co_await stopped->Stop();
    stopped.reset();
    Expect(stop_count == 2, "repeated Stop or destruction stopped the backend twice");
  }
  auto service = std::make_shared<MuonNotificationService>(path, "general", backend,
      [] { return 10; }, [&](std::string id) { ++activated_count; startup_ids.push_back(std::move(id)); });
  service->Start();
  Expect((co_await service->GetStatus("{}")).find("\"app\":{\"state\":\"enabled\"") != std::string::npos,
         "observable enabled OS status was not exposed");
  Expect((co_await service->GetCapabilities()).find("\"appStatus\":{\"state\":\"supported\"") != std::string::npos,
         "OS status capability was not exposed");
  Expect((co_await service->GetCapabilities()).find("\"launch\":{\"state\":\"supported\",\"reason\":null}") != std::string::npos,
         "verified process activation registration was not exposed");
  capabilities.launch = std::nullopt;
  capabilities.launch_reason = "registration-unobservable";
  Expect((co_await service->GetCapabilities()).find("\"launch\":{\"state\":\"unknown\",\"reason\":\"registration-unobservable\"}") != std::string::npos,
         "unobservable process activation was guessed");
  capabilities.launch = false;
  capabilities.launch_reason = "registration-missing";
  Expect((co_await service->GetCapabilities()).find("\"launch\":{\"state\":\"unsupported\",\"reason\":\"registration-missing\"}") != std::string::npos,
         "unregistered process activation was confused with running interactions");
  status = {false, "disabled-for-application"};
  const auto disabled = co_await service->GetStatus(R"({"category":"arbitrary"})");
  Expect(disabled.find("\"app\":{\"state\":\"disabled\",\"reason\":\"disabled-for-application\"}") != std::string::npos &&
             disabled.find("\"category\":{\"id\":\"arbitrary\",\"state\":\"unknown\"") != std::string::npos,
         "OS setting was confused with category status");
  auto disabled_rejected = false;
  try { co_await service->Post(R"({"id":"disabled","title":"Blocked","body":""})"); }
  catch (const std::exception& error) {
    disabled_rejected = std::string(error.what()).find("disabled-for-application") != std::string::npos;
  }
  Expect(disabled_rejected && posted.empty(), "known disabled OS setting did not reject before posting");
  status = {std::nullopt, "app-status-unavailable"};
  Expect((co_await service->GetStatus("{}")).find("\"app\":{\"state\":\"unknown\"") != std::string::npos,
         "unknown OS setting was guessed");
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
  activation(posted[0], std::nullopt, "desktop-user_TIME1234");
  activation(posted[1], std::nullopt, "desktop-user_TIME1234");
  const auto batch = co_await next;
  Expect(batch.find("\"data\":2") != std::string::npos &&
             batch.find(posted[0]) == std::string::npos,
         "watch delivered stale revision data");
  const auto pending = co_await service->GetPendingEvents();
  Expect(pending.find(posted[1]) != std::string::npos, "event was not durable");
  Expect(activated_count == 1, "a durable interaction did not activate its application");
  Expect(startup_ids == std::vector<std::string>{"desktop-user_TIME1234"},
         "desktop activation context was lost before presenting the window");
  activation(posted[1], std::nullopt, "desktop-user_TIME1234");
  activation("unknown-token", std::nullopt, "desktop-user_TIME1234");
  co_await service->GetPendingEvents();
  Expect(activated_count == 1, "a stale or unknown interaction activated its application");
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
  service = std::make_shared<MuonNotificationService>(path, "general", backend,
      [] { return 12; }, [&](std::string id) { ++activated_count; startup_ids.push_back(std::move(id)); });
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
  activation(posted.back(), std::nullopt, "desktop-user_TIME1234");
  co_await service->Stop();
  const auto stopped_snapshot = co_await LoadMuonNotificationSnapshot(path);
  Expect(stopped_snapshot.pending.size() == 1 && stopped_snapshot.pending[0].id == "shutdown",
         "shutdown lost an already accepted activation");
  Expect(activated_count == 1, "an accepted shutdown interaction reopened its application");

  service = std::make_shared<MuonNotificationService>((directory / "failures.json").string(),
      "general", backend, [] { return 20; }, [&](std::string id) { ++activated_count; startup_ids.push_back(std::move(id)); });
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
  activation(saved_token, std::nullopt, "desktop-user_TIME1234");
  // This read is ordered after the signal's reserved queue slot.
  Expect((co_await service->GetPendingEvents()) == "[]" && !waiting.is_ready(),
         "failed persistence was delivered as a durable event");
  Expect(activated_count == 1, "an unsaved interaction activated its application");
  std::filesystem::remove(directory / "failures.json.next");
  fail_cancel = true;
  activation(saved_token, std::nullopt, "desktop-user_TIME1234");
  const auto delivered = co_await waiting;
  Expect(delivered.find("\"data\":42") != std::string::npos,
         "rollback lost the old revision or OS cancellation suppressed durable delivery");
  Expect((co_await service->GetPendingEvents()).find(saved_token) != std::string::npos,
         "failed OS cancellation lost the durable event");
  Expect(activated_count == 2, "OS cancellation failure suppressed application activation");
  fail_cancel = false;
  auto terminal_wait = service->WatchEvents("{\"operation\":\"next\",\"token\":\"" + live + "\",\"afterCursor\":1}", "live");
  co_await service->WatchEvents("{\"operation\":\"release\",\"token\":\"" + live + "\"}", "live");
  Expect((co_await terminal_wait).find("\"events\":null") != std::string::npos,
         "explicit release did not finish a held wait");
  co_await service->Stop();

  // A notification may reach the native service before the first browser and
  // its presentation callback exist. Keep its OS context until that window is ready.
  const auto early_path = (directory / "early.json").string();
  service = std::make_shared<MuonNotificationService>(early_path, "", backend, [] { return 30; });
  service->Start();
  co_await service->Post(R"({"id":"early","title":"Cold start","body":""})");
  const auto early_token = posted.back();
  co_await service->Stop();
  service = std::make_shared<MuonNotificationService>(early_path, "", backend, [] { return 31; });
  service->Start();
  activation(early_token, std::nullopt, "cold-desktop_TIME5678");
  Expect((co_await service->GetPendingEvents()).find(early_token) != std::string::npos,
         "an early notification interaction was not durable");
  auto early_presentations = std::vector<std::string>{};
  service->SetActivationHandler([&](std::string id) {
    early_presentations.push_back(std::move(id));
  });
  Expect(early_presentations == std::vector<std::string>{"cold-desktop_TIME5678"},
         "early OS context was lost before the window callback was installed");
  service->SetActivationHandler([&](std::string id) {
    early_presentations.push_back(std::move(id));
  });
  Expect(early_presentations.size() == 1, "installing a callback replayed window presentation");
  co_await service->Stop();

  // DBusActivatable desktop entries also deliver ordinary menu launches.
  // Present those without manufacturing a notification interaction.
  auto application_activation = std::function<void(std::string)>{};
  backend.set_application_activation = [&](std::function<void(std::string)> callback) {
    application_activation = std::move(callback);
  };
  auto ordinary = std::make_shared<MuonNotificationService>(
      (directory / "ordinary.json").string(), "", backend, [] { return 40; });
  ordinary->Start();
  Expect(static_cast<bool>(application_activation), "ordinary application activation was not connected");
  application_activation("menu_TIME9012");
  auto ordinary_presentations = std::vector<std::string>{};
  ordinary->SetActivationHandler([&](std::string context) {
    ordinary_presentations.push_back(std::move(context));
  });
  Expect(ordinary_presentations == std::vector<std::string>{"menu_TIME9012"},
         "ordinary early activation lost the desktop context");
  Expect((co_await ordinary->GetPendingEvents()) == "[]",
         "ordinary menu activation became a notification event");
  co_await ordinary->Stop();
  application_activation("late_TIME9999");
  Expect(ordinary_presentations.size() == 1, "ordinary activation presented a stopped application");
}

#if defined(_WIN32)
static cardio::promise<void> RunAndReport(std::filesystem::path directory, bool& passed) {
  try { co_await RunTests(std::move(directory)); passed = true; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  PostQuitMessage(0);
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
      ("muon-notification-service-" + std::to_string(GetCurrentProcessId()));
  auto group = cardio::dispatcher_group();
  auto dispatcher = cardio::dispatcher_host_win32_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto passed = false;
  auto pending = RunAndReport(directory, passed);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  std::filesystem::remove_all(directory);
  if (passed) std::cout << "Windows notification service passed\n";
  return passed ? 0 : 1;
}
#else
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
#endif
