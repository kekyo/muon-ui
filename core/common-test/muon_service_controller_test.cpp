/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_controller.h"

#include <iostream>
#include <deque>
#include <stdexcept>

static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  auto queue = std::deque<std::function<void()>>{};
  const auto post = [&](std::function<void()> task) { queue.push_back(std::move(task)); };
  const auto pump = [&] {
    while (!queue.empty()) {
      auto task = std::move(queue.front());
      queue.pop_front();
      task();
    }
  };
  auto starts = 0;
  auto stops = 0;
  auto saves = 0;
  auto acquired = 0;
  auto depth = 0;
  auto maximum_depth = 0;
  auto events = std::vector<MuonServiceEvent>{};
  auto pending_start = std::function<void(const std::string&)>{};
  auto pending_stop = std::function<void()>{};
  auto* current = static_cast<MuonServiceController*>(nullptr);
  auto controller = MuonServiceController({{"alpha", false, std::nullopt}}, {
      [&](const MuonServiceSnapshot&, auto complete) {
        ++starts;
        pending_start = std::move(complete);
      },
      [&](const MuonServiceSnapshot&, auto complete) {
        ++stops;
        pending_stop = std::move(complete);
      },
      [&](const std::string&, bool, auto complete) { ++saves; complete(""); },
      [&](const MuonServiceEvent& event) {
        ++depth;
        if (depth > maximum_depth) maximum_depth = depth;
        events.push_back(event);
        if (event.kind == MuonServiceEventKind::Acquired) {
          ++acquired;
          current->Release(event.reference.value());
        }
        --depth;
      }, post});
  current = &controller;
  controller.Activate();
  const auto a = controller.Acquire("alpha", "page-a").value();
  controller.Acquire("alpha", "page-b");
  Require(starts == 0 && events.empty(), "reservation identities must return before backend dispatch");
  pump();
  Require(starts == 1 && acquired == 0, "pending clients must share one backend start");
  pending_start("");
  pump();
  Require(acquired == 2 && stops == 1 && maximum_depth == 1 &&
              !controller.IsReady(a),
          "reentrant release must drain notifications without recursive delivery");
  const auto next = controller.Acquire("alpha", "page-c").value();
  Require(starts == 1, "reacquisition must await native cleanup");
  pending_stop();
  pump();
  Require(starts == 2, "next generation must start exactly once after cleanup");
  controller.ReleaseOwner("page-c");
  pending_start("");
  pump();
  Require(stops == 2 && !controller.IsReady(next), "orphaned startup must be disposed");
  pending_stop();
  pump();
  Require(controller.IsQuiescent(), "native cleanup must settle the controller");

  controller.SetAutostart("alpha", true);
  pump();
  Require(saves == 1 && starts == 3, "committed resident policy must dispatch startup");
  controller.SetAutostart("alpha", true);
  pump();
  Require(saves == 1 && starts == 3, "duplicate ON must not duplicate save or startup");
  controller.Shutdown("application exit");
  pending_start("");
  pump();
  pending_stop();
  pump();
  Require(controller.IsQuiescent() && controller.GetSnapshot("alpha")->autostart &&
              !controller.GetSnapshot("alpha")->resident,
          "shutdown must preserve preference while disposing of late startup");

  auto delivered = 0;
  auto rejected = 0;
  auto* shutting = static_cast<MuonServiceController*>(nullptr);
  auto second = MuonServiceController({{"beta", false, std::nullopt}}, {
      [&](const MuonServiceSnapshot&, auto complete) { pending_start = std::move(complete); },
      [&](const MuonServiceSnapshot&, auto complete) { complete(); },
      [&](const std::string&, bool, auto complete) { complete(""); },
      [&](const MuonServiceEvent& event) {
        if (event.kind == MuonServiceEventKind::Acquired) {
          ++delivered;
          shutting->Shutdown("exit during notification");
        } else if (event.kind == MuonServiceEventKind::AcquireFailed) ++rejected;
      }, post});
  shutting = &second;
  second.Activate();
  second.Acquire("beta", "one");
  second.Acquire("beta", "two");
  pump();
  pending_start("");
  pump();
  Require(delivered == 1 && rejected == 1 && second.IsQuiescent(),
          "reentrant shutdown must fail a stale acquisition instead of stranding its caller");
  auto aborted_starts = 0;
  auto aborted_stops = 0;
  auto third = MuonServiceController({{"gamma", true, std::nullopt}}, {
      [&](const MuonServiceSnapshot&, auto complete) { ++aborted_starts; complete(""); },
      [&](const MuonServiceSnapshot&, auto complete) { ++aborted_stops; complete(); },
      [&](const std::string&, bool, auto complete) { complete(""); },
      [](const MuonServiceEvent&) {}, post});
  third.RequestRuntimeStop("gamma", 1, "OS rejected background start");
  pump();
  Require(aborted_starts == 0 && aborted_stops == 0 && third.IsQuiescent() &&
              third.GetSnapshot("gamma")->error == "OS rejected background start",
          "an OS stop before dispatch must settle startup without invoking the runtime");
  std::cout << "service controller: passed\n";
  return 0;
}
