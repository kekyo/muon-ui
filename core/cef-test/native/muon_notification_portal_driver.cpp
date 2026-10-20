/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_linux.h"
#include "notifications/muon_notification_store.h"
#include <gio/gio.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

using namespace muon_internal;
static cardio::promise<void> Run(std::string directory,
    std::string desktop, std::string launcher,
    std::string operation, GMainLoop* loop, bool& passed) {
  auto activation = cardio::promise_source<std::string>{};
  auto arrived = activation.get_promise();
  auto service = std::make_shared<MuonNotificationService>(
      (std::filesystem::path(directory) / "snapshot.json").string(), "",
      CreateMuonLinuxNotificationBackend("Muon Portal Backend", desktop, "portal-driver", launcher),
      [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
      }, [&](std::string context) { activation.try_resolve(std::move(context)); });
  service->Start();
  try {
    const auto caps = co_await service->GetCapabilities();
    std::cout << caps << '\n' << std::flush;
    if (caps.find("\"launch\":{\"state\":\"supported\"") == std::string::npos)
      throw std::runtime_error("Installed notification launch was not supported");
    if (operation == "post") {
      co_await service->Post(R"({
        "id":"cold","title":"Muon portal backend cold launch","body":"Process has exited; original data remains.",
        "data":{"order":42},"actions":[{"id":"default","title":"View original order"}]
      })");
    } else if (operation == "receive") {
      const auto context = co_await arrived;
      const auto pending = co_await service->GetPendingEvents();
      const auto state = std::filesystem::path(directory);
      std::ofstream(state / "startup.txt") << context;
      std::ofstream(state / "received.next") << pending;
      std::filesystem::rename(state / "received.next", state / "received.json");
      std::cout << pending << '\n';
    } else if (operation == "pending") {
      std::cout << co_await service->GetPendingEvents() << '\n';
    } else if (operation == "cancel") {
      co_await service->Cancel("cold");
    } else if (operation == "ack") {
      const auto snapshot = co_await LoadMuonNotificationSnapshot(
          (std::filesystem::path(directory) / "snapshot.json").string());
      for (const auto& event : snapshot.pending)
        co_await service->AcknowledgeEvent(event.event_id);
    } else {
      throw std::runtime_error("Unknown driver operation");
    }
    passed = true;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  activation.try_resolve("");
  co_await service->Stop();
  g_main_loop_quit(loop);
}
int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "Usage: driver <state-directory> <desktop-id> <launcher> <post|receive|pending|cancel|ack>\n";
    return 2;
  }
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto* loop = g_main_loop_new(nullptr, FALSE);
  auto passed = false;
  auto pending = Run(argv[1], argv[2], argv[3], argv[4], loop, passed);
  if (!pending.is_ready()) g_main_loop_run(loop);
  g_main_loop_unref(loop);
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  return passed ? 0 : 1;
}
