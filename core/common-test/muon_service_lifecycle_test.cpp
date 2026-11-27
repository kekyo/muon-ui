/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_lifecycle.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static size_t Count(const std::vector<MuonServiceEvent>& events,
                    MuonServiceEventKind kind) {
  return static_cast<size_t>(std::count_if(
      events.begin(), events.end(), [kind](const auto& event) {
        return event.kind == kind;
      }));
}

static bool ActiveWorkOutlivesItsPage() {
  auto manager = MuonServiceLifecycle({{"capture", false, std::nullopt}});
  const auto page = manager.Acquire("capture", "first-page");
  if (!page) return false;
  manager.CompleteStart("capture", page->generation, "");
  if (!manager.SetWorkActive("capture", page->generation, true)) return false;
  manager.ReleaseOwner("first-page");
  const auto retained = *manager.GetSnapshot("capture");
  if (!Expect(retained.state == MuonServiceState::Running && retained.client_references == 0 &&
              !retained.resident && !retained.autostart, "accepted work must outlive its page without autostart")) return false;
  const auto next = manager.Acquire("capture", "next-page");
  if (!next || next->generation != retained.generation) return false;
  manager.Release(*next);
  if (manager.SetWorkActive("capture", retained.generation + 1, false)) return false;
  manager.SetWorkActive("capture", retained.generation, false);
  manager.CompleteStop("capture", retained.generation);
  return Expect(manager.GetSnapshot("capture")->state == MuonServiceState::Stopped &&
                !manager.SetWorkActive("capture", retained.generation, true),
                "completing the final job must stop the unreferenced generation");
}

static bool DefaultsAndOverrides() {
  auto manager = MuonServiceLifecycle({
      {"off", false, std::nullopt}, {"on", true, std::nullopt},
      {"user-off", true, false}, {"user-on", false, true}});
  const auto events = manager.TakeEvents();
  return Expect(Count(events, MuonServiceEventKind::Start) == 2,
                "only effective autostart services should start") &&
         Expect(!manager.GetSnapshot("off")->resident &&
                    manager.GetSnapshot("on")->resident &&
                    !manager.GetSnapshot("user-off")->resident &&
                    manager.GetSnapshot("user-on")->resident,
                "user overrides must precede manifest defaults") &&
         Expect(manager.GetSnapshot("on")->client_references == 0,
                "resident starts must not create page references");
}

static bool RetryDeniedResidentStart() {
  auto manager = MuonServiceLifecycle({{"worker", true, std::nullopt}});
  manager.TakeEvents();
  manager.CompleteStart("worker", 1, "background launch denied");
  manager.TakeEvents();
  manager.RetryResidentStart("worker", 1);
  manager.RetryResidentStart("worker", 1);
  if (!Expect(Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 1 &&
              manager.GetSnapshot("worker")->client_references == 0 &&
              manager.GetSnapshot("worker")->start_reason == MUON_SERVICE_START_APP,
              "foreground eligibility retries one resident generation without adding a page reference")) return false;
  manager.CompleteStart("worker", 2, "permission missing");
  manager.RetryResidentStart("worker", 1);
  if (!Expect(manager.GetSnapshot("worker")->generation == 2,
              "an older failed generation cannot retry its replacement")) return false;
  manager.CompleteAutostart("worker", manager.SetAutostart("worker", false), "");
  manager.RetryResidentStart("worker", 2);
  return Expect(manager.GetSnapshot("worker")->generation == 2 && !manager.GetSnapshot("worker")->resident,
                "a saved OFF prevents a later foreground retry");
}

static bool GenerationReasons() {
  auto manager = MuonServiceLifecycle({{"sync", true, std::nullopt}}, MUON_SERVICE_START_LOGIN_AUTOSTART);
  if (!Expect(manager.GetSnapshot("sync")->start_reason == MUON_SERVICE_START_LOGIN_AUTOSTART,
              "the first resident generation records the login launch")) return false;
  manager.CompleteStart("sync", 1, "");
  const auto handle = *manager.Acquire("sync", "page");
  manager.CompleteAutostart("sync", manager.SetAutostart("sync", false), "");
  manager.Release(handle);
  const auto pending = *manager.Acquire("sync", "page");
  auto state = *manager.GetSnapshot("sync");
  if (!Expect(state.stop.reason == MUON_SERVICE_STOP_REFERENCE_RELEASED && pending.generation == 2,
              "a new acquisition cannot change the first stop reason")) return false;
  manager.CompleteStop("sync", 1);
  if (!Expect(manager.GetSnapshot("sync")->start_reason == MUON_SERVICE_START_ACQUIRE,
              "the waiting page starts a new generation")) return false;
  manager.TakeEvents();
  manager.Shutdown("User logged off", {MUON_SERVICE_STOP_SESSION_ENDING, "logoff"});
  manager.Shutdown("duplicate");
  const auto cancelling = manager.TakeEvents();
  manager.CompleteStart("sync", 2, "");
  state = *manager.GetSnapshot("sync");
  return Expect(Count(cancelling, MuonServiceEventKind::CancelStart) == 1 &&
      Count(manager.TakeEvents(), MuonServiceEventKind::Stop) == 1 &&
      state.stop.reason == MUON_SERVICE_STOP_SESSION_ENDING && state.stop.detail == "logoff",
      "late successful start is cancelled and disposed once with the first reason");
}

static bool SharedAcquisition() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto a = manager.Acquire("sync", "page-a").value();
  const auto b = manager.Acquire("sync", "page-b").value();
  const auto pending = manager.TakeEvents();
  if (!Expect(a.id != b.id && a.generation == b.generation &&
                  Count(pending, MuonServiceEventKind::Start) == 1 &&
                  Count(pending, MuonServiceEventKind::Acquired) == 0 &&
                  !manager.IsReady(a),
              "concurrent acquisition must reserve before one shared start"))
    return false;
  manager.CompleteStart("sync", a.generation, "");
  const auto ready = manager.TakeEvents();
  if (!Expect(Count(ready, MuonServiceEventKind::Acquired) == 2 &&
                  manager.IsReady(a) && manager.IsReady(b),
              "both owners must acquire the ready generation"))
    return false;
  manager.Release(a);
  const auto after_a = manager.TakeEvents();
  if (!Expect(Count(after_a, MuonServiceEventKind::Stop) == 0 &&
                  manager.IsReady(b) && !manager.Release(a),
              "releasing one owner twice must leave the other callable"))
    return false;
  manager.Release(b);
  return Expect(Count(manager.TakeEvents(), MuonServiceEventKind::Stop) == 1 &&
                    !manager.IsReady(b),
                "last reference must stop accepting work and request stop");
}

static bool ExactReferenceIdentity() {
  auto manager = MuonServiceLifecycle({{"a", false, std::nullopt},
                                       {"b", false, std::nullopt}});
  const auto reference = manager.Acquire("a", "page").value();
  manager.CompleteStart("a", reference.generation, "");
  for (const auto& altered : std::vector<MuonServiceReference>{
           {reference.id, "a", "wrong", reference.generation},
           {reference.id, "b", "page", reference.generation},
           {reference.id, "a", "page", reference.generation + 1}}) {
    if (!Expect(!manager.Release(altered) && !manager.IsReady(altered),
                "a mismatched reference must not affect its real owner"))
      return false;
  }
  return Expect(manager.IsReady(reference), "real reference must remain usable");
}

static bool OwnerDisappearsDuringStart() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto a = manager.Acquire("sync", "page-a").value();
  const auto b = manager.Acquire("sync", "page-b").value();
  manager.TakeEvents();
  if (!Expect(manager.ReleaseOwner("page-a") == 1,
              "disconnection must recover its pending acquisition")) return false;
  manager.CompleteStart("sync", a.generation, "");
  const auto events = manager.TakeEvents();
  if (!Expect(Count(events, MuonServiceEventKind::Acquired) == 1 &&
                  !manager.IsReady(a) && manager.IsReady(b),
              "only the surviving owner may receive a ready reference"))
    return false;
  manager.ReleaseOwner("page-b");
  manager.CompleteStop("sync", a.generation);
  const auto c = manager.Acquire("sync", "page-c").value();
  manager.ReleaseOwner("page-c");
  manager.TakeEvents();
  manager.CompleteStart("sync", c.generation, "");
  const auto orphan = manager.TakeEvents();
  return Expect(Count(orphan, MuonServiceEventKind::Acquired) == 0 &&
                    Count(orphan, MuonServiceEventKind::Stop) == 1,
                "a late successful start without owners must immediately stop");
}

static bool ReacquireDuringStop() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto old = manager.Acquire("sync", "page-a").value();
  manager.CompleteStart("sync", old.generation, "");
  manager.Release(old);
  manager.TakeEvents();
  const auto next = manager.Acquire("sync", "page-b").value();
  if (!Expect(next.generation > old.generation &&
                  Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 0,
              "next generation must wait for old runtime disposal")) return false;
  manager.CompleteStop("sync", old.generation);
  if (!Expect(Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 1,
              "old cleanup must preserve next-generation reservation")) return false;
  manager.CompleteStart("sync", next.generation, "");
  manager.TakeEvents();
  manager.CompleteStop("sync", old.generation);
  manager.CompleteStart("sync", old.generation, "late failure");
  manager.RuntimeExited("sync", old.generation, "late exit");
  return Expect(!manager.Release(old) && manager.IsReady(next) &&
                    manager.TakeEvents().empty(),
                "stale completions and releases must not alter a new runtime");
}

static bool ReacquireDuringCancelledStart() {
  MuonServiceLifecycle manager({{"sync", false, std::nullopt}});
  const auto old = *manager.Acquire("sync", "first");
  manager.Release(old);
  const auto next = *manager.Acquire("sync", "second");
  manager.TakeEvents();
  manager.CompleteStart("sync", old.generation, "author cancelled initialization");
  const auto events = manager.TakeEvents();
  manager.CompleteStart("sync", next.generation, "");
  return Expect(next.generation == old.generation + 1 &&
      Count(events, MuonServiceEventKind::Start) == 1 && manager.IsReady(next),
      "cancelling an abandoned startup must not strand a later page acquisition");
}

static bool CancelNextGeneration() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto first = manager.Acquire("sync", "page-a").value();
  manager.CompleteStart("sync", first.generation, "");
  manager.Release(first);
  manager.Acquire("sync", "page-b");
  manager.ReleaseOwner("page-b");
  manager.TakeEvents();
  manager.CompleteStop("sync", first.generation);
  return Expect(Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 0 &&
                    manager.IsQuiescent(),
                "cancelled next-generation demand must not restart a service");
}

static bool PersistExplicitDefault() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto operation = manager.SetAutostart("sync", false);
  const auto events = manager.TakeEvents();
  if (!Expect(Count(events, MuonServiceEventKind::SaveAutostart) == 1 &&
                  Count(events, MuonServiceEventKind::AutostartResult) == 0,
              "explicit user OFF must be saved even when default is OFF")) return false;
  manager.CompleteAutostart("sync", operation, "");
  manager.TakeEvents();
  manager.SetAutostart("sync", false);
  const auto repeated = manager.TakeEvents();
  return Expect(Count(repeated, MuonServiceEventKind::SaveAutostart) == 0 &&
                    Count(repeated, MuonServiceEventKind::AutostartResult) == 1 &&
                    !manager.GetSnapshot("sync")->resident,
                "repeating persisted OFF must complete without acquiring a root");
}

static bool ResidentSwitchPreservesClients() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto on = manager.SetAutostart("sync", true);
  if (!Expect(!manager.GetSnapshot("sync")->resident,
              "uncommitted setting must not create a resident reference")) return false;
  manager.CompleteAutostart("sync", on, "");
  const auto started = manager.TakeEvents();
  if (!Expect(Count(started, MuonServiceEventKind::Start) == 1 &&
                  Count(started, MuonServiceEventKind::AutostartResult) == 1,
              "setter must finish without waiting for runtime readiness")) return false;
  const auto ref = manager.Acquire("sync", "page").value();
  manager.CompleteStart("sync", ref.generation, "");
  manager.SetAutostart("sync", true);
  manager.TakeEvents();
  const auto off = manager.SetAutostart("sync", false);
  manager.CompleteAutostart("sync", off, "");
  const auto disabled = manager.TakeEvents();
  if (!Expect(!manager.GetSnapshot("sync")->resident && manager.IsReady(ref) &&
                  Count(disabled, MuonServiceEventKind::Stop) == 0 &&
                  Count(disabled, MuonServiceEventKind::AutostartResult) == 1,
              "OFF must finish while a caller still holds its reference")) return false;
  manager.Release(ref);
  return Expect(Count(manager.TakeEvents(), MuonServiceEventKind::Stop) == 1,
                "idempotent ON must not leave an extra resident reference");
}

static bool SettingsAreSerialized() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto on1 = manager.SetAutostart("sync", true);
  const auto off = manager.SetAutostart("sync", false);
  const auto on2 = manager.SetAutostart("sync", true);
  if (!Expect(Count(manager.TakeEvents(), MuonServiceEventKind::SaveAutostart) == 1,
              "only one persistence operation may be in flight")) return false;
  manager.CompleteAutostart("sync", on2, "");
  if (!Expect(!manager.GetSnapshot("sync")->autostart,
              "an unissued completion must not apply settings")) return false;
  manager.CompleteAutostart("sync", on1, "");
  const auto generation = manager.GetSnapshot("sync")->generation;
  manager.CompleteStart("sync", generation, "");
  manager.CompleteAutostart("sync", off, "");
  manager.CompleteAutostart("sync", on2, "");
  manager.TakeEvents();
  manager.CompleteAutostart("sync", off, "");
  manager.CompleteStop("sync", generation);
  const auto latest = manager.GetSnapshot("sync").value();
  return Expect(latest.autostart && latest.resident &&
                    latest.revision == on2 && latest.generation > generation &&
                    Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 1,
                "ON/OFF/ON must restart once with the last committed preference");
}

static bool FailedSaveDoesNotApply() {
  auto manager = MuonServiceLifecycle({{"off", false, std::nullopt},
                                       {"on", true, std::nullopt}});
  manager.TakeEvents();
  const auto on = manager.SetAutostart("off", true);
  const auto off = manager.SetAutostart("on", false);
  manager.CompleteAutostart("off", on, "disk full");
  manager.CompleteAutostart("on", off, "disk full");
  const auto events = manager.TakeEvents();
  return Expect(!manager.GetSnapshot("off")->resident &&
                    manager.GetSnapshot("on")->resident &&
                    manager.GetSnapshot("off")->revision == 0 &&
                    manager.GetSnapshot("on")->revision == 0 &&
                    Count(events, MuonServiceEventKind::Start) == 0 &&
                    Count(events, MuonServiceEventKind::Stop) == 0 &&
                    Count(events, MuonServiceEventKind::AutostartResult) == 2 &&
                    std::none_of(events.begin(), events.end(), [](const auto& e) {
                      return e.kind == MuonServiceEventKind::AutostartResult &&
                             (e.success || e.error != "disk full");
                    }),
                "failed saves must preserve policy, revision, and runtime demand");
}

static bool FailureInvalidatesWithoutRestartLoop() {
  auto manager = MuonServiceLifecycle({{"sync", true, std::nullopt}});
  const auto pending = manager.Acquire("sync", "page-a").value();
  manager.TakeEvents();
  manager.CompleteStart("sync", pending.generation, "permission denied");
  const auto failure = manager.TakeEvents();
  if (!Expect(Count(failure, MuonServiceEventKind::AcquireFailed) == 1 &&
                  Count(failure, MuonServiceEventKind::Start) == 0 &&
                  manager.GetSnapshot("sync")->client_references == 0 &&
                  manager.GetSnapshot("sync")->resident &&
                  manager.GetSnapshot("sync")->state == MuonServiceState::Failed,
              "startup failure must reject demand without retrying resident policy"))
    return false;
  const auto retry = manager.Acquire("sync", "page-b").value();
  manager.CompleteStart("sync", retry.generation, "");
  manager.TakeEvents();
  manager.RuntimeExited("sync", retry.generation, "process died");
  const auto exited = manager.TakeEvents();
  if (!Expect(!manager.IsReady(retry) && !manager.Release(retry) &&
                  Count(exited, MuonServiceEventKind::Invalidated) == 1 &&
                  Count(exited, MuonServiceEventKind::Start) == 0,
              "a failed runtime must invalidate acquired handles without replay"))
    return false;
  const auto next = manager.Acquire("sync", "page-c").value();
  manager.CompleteStart("sync", next.generation, "");
  return Expect(next.generation > retry.generation && manager.IsReady(next),
                "an explicit later acquisition may retry a failed service");
}

static bool DuplicateCompletion() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto ref = manager.Acquire("sync", "page").value();
  manager.CompleteStart("sync", ref.generation, "");
  manager.TakeEvents();
  manager.CompleteStart("sync", ref.generation, "");
  manager.CompleteStart("sync", ref.generation, "late failure");
  if (!Expect(manager.IsReady(ref) && manager.TakeEvents().empty(),
              "duplicate start completion must not resolve acquisition twice"))
    return false;
  manager.Release(ref);
  manager.CompleteStop("sync", ref.generation);
  manager.TakeEvents();
  manager.CompleteStop("sync", ref.generation);
  return Expect(manager.TakeEvents().empty(), "duplicate stop must be ignored");
}

static bool IndependentServicesAndObservation() {
  auto manager = MuonServiceLifecycle({{"a", false, std::nullopt},
                                       {"b", false, std::nullopt}});
  const auto a = manager.Acquire("a", "page-a").value();
  const auto b = manager.Acquire("b", "page-b").value();
  manager.CompleteStart("a", a.generation, "");
  manager.CompleteStart("b", b.generation, "");
  const auto observation = manager.GetSnapshot("a").value();
  manager.ReleaseOwner("page-a");
  manager.CompleteStop("a", a.generation);
  return Expect(observation.state == MuonServiceState::Running &&
                    manager.GetSnapshot("a")->state == MuonServiceState::Stopped &&
                    manager.IsReady(b),
                "observing A must neither keep it alive nor stop B");
}

static bool ShutdownOverridesResidentAndPendingSave() {
  auto manager = MuonServiceLifecycle({{"sync", true, std::nullopt}});
  const auto ref = manager.Acquire("sync", "page").value();
  manager.CompleteStart("sync", ref.generation, "");
  const auto off = manager.SetAutostart("sync", false);
  manager.SetAutostart("sync", true);
  manager.TakeEvents();
  manager.Shutdown("application exit");
  const auto events = manager.TakeEvents();
  if (!Expect(manager.IsShuttingDown() && !manager.IsReady(ref) &&
                  !manager.GetSnapshot("sync")->resident &&
                  manager.GetSnapshot("sync")->autostart &&
                  Count(events, MuonServiceEventKind::Invalidated) == 1 &&
                  Count(events, MuonServiceEventKind::Stop) == 1 &&
                  !manager.Acquire("sync", "late-page").has_value() &&
                  manager.SetAutostart("sync", true) == 0,
              "host shutdown must reject new work without rewriting preferences"))
    return false;
  manager.CompleteStop("sync", ref.generation);
  if (!Expect(!manager.IsQuiescent(), "an issued save must settle before exit"))
    return false;
  manager.CompleteAutostart("sync", off, "");
  const auto after = manager.TakeEvents();
  manager.Shutdown("duplicate exit");
  return Expect(manager.IsQuiescent() && !manager.GetSnapshot("sync")->resident &&
                    Count(after, MuonServiceEventKind::Start) == 0 &&
                    Count(after, MuonServiceEventKind::SaveAutostart) == 0 &&
                    manager.TakeEvents().empty(),
                "late saves and duplicate shutdown must not recreate demand");
}

static bool InvalidIdentities() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  if (!Expect(!manager.Acquire("missing", "page") &&
                  !manager.Acquire("sync", "") &&
                  !manager.GetSnapshot("missing") &&
                  manager.SetAutostart("missing", true) == 0,
              "unconfigured services and empty owners must be rejected")) return false;
  try {
    auto duplicate = MuonServiceLifecycle({{"a", false, std::nullopt},
                                          {"a", true, std::nullopt}});
    return Expect(false, "duplicate definitions were accepted");
  } catch (const std::invalid_argument&) {
    return true;
  }
}

static bool SynchronousBackendAndReentrantRelease() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  manager.Acquire("sync", "page");
  auto acquired = size_t{0};
  auto stopped = size_t{0};
  // A backend can finish inside action dispatch and a client can release its
  // handle inside acquisition delivery. Neither may mutate a scanned map.
  for (;;) {
    const auto events = manager.TakeEvents();
    if (events.empty()) break;
    for (const auto& event : events) {
      if (event.kind == MuonServiceEventKind::Start) {
        manager.CompleteStart(event.snapshot.id, event.snapshot.generation, "");
      } else if (event.kind == MuonServiceEventKind::Acquired &&
                 manager.IsReady(event.reference.value())) {
        ++acquired;
        manager.Release(event.reference.value());
      } else if (event.kind == MuonServiceEventKind::Stop) {
        ++stopped;
        manager.CompleteStop(event.snapshot.id, event.snapshot.generation);
      }
    }
  }
  return Expect(acquired == 1 && stopped == 1 && manager.IsQuiescent(),
                "synchronous backends and reentrant release must settle once");
}

static bool ShutdownDuringAcquisitionDelivery() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto a = manager.Acquire("sync", "page-a").value();
  const auto b = manager.Acquire("sync", "page-b").value();
  manager.TakeEvents();
  manager.CompleteStart("sync", a.generation, "");
  auto delivered = size_t{0};
  for (const auto& event : manager.TakeEvents()) {
    if (event.kind == MuonServiceEventKind::Acquired &&
        manager.IsReady(event.reference.value())) {
      ++delivered;
      manager.Shutdown("closed while notifying another page");
    }
  }
  manager.CompleteStop("sync", a.generation);
  return Expect(delivered == 1 && !manager.IsReady(b) && manager.IsQuiescent(),
                "reentrant shutdown must prevent a later stale acquisition delivery");
}

static bool ShutdownDuringStartAndSave() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto ref = manager.Acquire("sync", "page").value();
  const auto setting = manager.SetAutostart("sync", true);
  manager.Shutdown("application exit");
  manager.TakeEvents();
  manager.CompleteAutostart("sync", setting, "");
  manager.CompleteStart("sync", ref.generation, "");
  const auto events = manager.TakeEvents();
  if (!Expect(Count(events, MuonServiceEventKind::Acquired) == 0 &&
                  Count(events, MuonServiceEventKind::Stop) == 1 &&
                  manager.GetSnapshot("sync")->autostart &&
                  !manager.GetSnapshot("sync")->resident,
              "late startup and committed ON must respect host shutdown"))
    return false;
  manager.CompleteStop("sync", ref.generation);
  return Expect(manager.IsQuiescent(), "shutdown must finish after late startup cleanup");
}

static bool OsStopInvalidatesBeforeCleanup() {
  auto manager = MuonServiceLifecycle({{"sync", true, std::nullopt},
                                       {"other", true, std::nullopt}});
  const auto reference = manager.Acquire("sync", "page").value();
  manager.CompleteStart("sync", reference.generation, "");
  manager.CompleteStart("other", 1, "");
  manager.TakeEvents();
  manager.RequestRuntimeStop("sync", reference.generation, "OS time limit");
  const auto events = manager.TakeEvents();
  if (!Expect(!manager.IsReady(reference) &&
                  !manager.Acquire("sync", "late-page").has_value() &&
                  Count(events, MuonServiceEventKind::Invalidated) == 1 &&
                  Count(events, MuonServiceEventKind::Stop) == 1 &&
                  manager.GetSnapshot("sync")->state == MuonServiceState::Stopping &&
                  manager.GetSnapshot("other")->state == MuonServiceState::Running,
              "OS stop must invalidate clients before cleanup without affecting other services"))
    return false;
  manager.CompleteStop("sync", reference.generation);
  const auto stopped = manager.TakeEvents();
  const auto snapshot = manager.GetSnapshot("sync").value();
  if (!Expect(snapshot.state == MuonServiceState::Failed && snapshot.resident &&
                  snapshot.autostart && snapshot.error == "OS time limit" &&
                  Count(stopped, MuonServiceEventKind::Start) == 0,
              "OS stop must preserve policy without restarting the resident service"))
    return false;
  const auto retry = manager.Acquire("sync", "retry").value();
  manager.CompleteStart("sync", retry.generation, "");
  manager.RequestRuntimeStop("sync", reference.generation, "late timeout");
  return Expect(manager.IsReady(retry), "explicit retry must be immune to old OS stop messages");
}

static bool OsStopDuringStart() {
  auto manager = MuonServiceLifecycle({{"sync", true, std::nullopt}});
  const auto reference = manager.Acquire("sync", "page").value();
  manager.TakeEvents();
  manager.RequestRuntimeStop("sync", reference.generation, "OS start cancelled");
  if (!Expect(Count(manager.TakeEvents(), MuonServiceEventKind::AcquireFailed) == 1,
              "OS cancellation must fail pending acquisition immediately")) return false;
  manager.CompleteStart("sync", reference.generation, "");
  const auto started = manager.TakeEvents();
  if (!Expect(Count(started, MuonServiceEventKind::Acquired) == 0 &&
                  Count(started, MuonServiceEventKind::Stop) == 1,
              "late startup must be cleaned up without delivering a usable handle"))
    return false;
  manager.RuntimeExited("sync", reference.generation, "OS process termination");
  return Expect(Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 0 &&
                    manager.IsQuiescent(),
                "forced process exit during OS stop must not retry resident demand");
}

static bool OsStopCancelsNextGeneration() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto old = manager.Acquire("sync", "page-a").value();
  manager.CompleteStart("sync", old.generation, "");
  manager.Release(old);
  const auto pending = manager.Acquire("sync", "page-b").value();
  manager.TakeEvents();
  manager.RequestRuntimeStop("sync", old.generation, "OS revoked eligibility");
  manager.RequestRuntimeStop("sync", old.generation, "duplicate");
  const auto events = manager.TakeEvents();
  manager.CompleteStop("sync", old.generation);
  return Expect(Count(events, MuonServiceEventKind::AcquireFailed) == 1 &&
                    Count(events, MuonServiceEventKind::Stop) == 0 &&
                    !manager.IsReady(pending) &&
                    Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 0,
                "OS stop must cancel next generation without issuing duplicate cleanup");
}

static bool OsStopDuringPolicySave() {
  auto manager = MuonServiceLifecycle({{"sync", false, std::nullopt}});
  const auto reference = manager.Acquire("sync", "page").value();
  manager.CompleteStart("sync", reference.generation, "");
  const auto setting = manager.SetAutostart("sync", true);
  manager.RequestRuntimeStop("sync", reference.generation, "OS time limit");
  manager.CompleteStop("sync", reference.generation);
  manager.TakeEvents();
  manager.CompleteAutostart("sync", setting, "");
  return Expect(manager.GetSnapshot("sync")->autostart &&
                    manager.GetSnapshot("sync")->state == MuonServiceState::Failed &&
                    Count(manager.TakeEvents(), MuonServiceEventKind::Start) == 0,
                "a late policy save must not resurrect a service stopped by the OS");
}

int main() {
  const auto tests = std::vector<std::pair<const char*, bool (*)()>>{
      {"defaults and overrides", DefaultsAndOverrides},
      {"active work outlives its page", ActiveWorkOutlivesItsPage},
      {"retry denied resident start", RetryDeniedResidentStart},
      {"shared acquisition", SharedAcquisition},
      {"exact reference identity", ExactReferenceIdentity},
      {"owner disappears during start", OwnerDisappearsDuringStart},
      {"generation reasons", GenerationReasons},
      {"reacquire during cancelled start", ReacquireDuringCancelledStart},
      {"reacquire during stop", ReacquireDuringStop},
      {"cancel next generation", CancelNextGeneration},
      {"persist explicit default", PersistExplicitDefault},
      {"resident switch preserves clients", ResidentSwitchPreservesClients},
      {"settings are serialized", SettingsAreSerialized},
      {"failed save does not apply", FailedSaveDoesNotApply},
      {"failure invalidates without restart", FailureInvalidatesWithoutRestartLoop},
      {"duplicate completion", DuplicateCompletion},
      {"independent services and observation", IndependentServicesAndObservation},
      {"shutdown overrides resident", ShutdownOverridesResidentAndPendingSave},
      {"invalid identities", InvalidIdentities},
      {"synchronous backend and release", SynchronousBackendAndReentrantRelease},
      {"shutdown during acquisition delivery", ShutdownDuringAcquisitionDelivery},
      {"shutdown during start and save", ShutdownDuringStartAndSave},
      {"OS stop invalidates before cleanup", OsStopInvalidatesBeforeCleanup},
      {"OS stop during start", OsStopDuringStart},
      {"OS stop cancels next generation", OsStopCancelsNextGeneration},
      {"OS stop during policy save", OsStopDuringPolicySave}};
  auto failures = size_t{0};
  for (const auto& [name, run] : tests) {
    if (!run()) {
      std::cerr << "FAIL: " << name << '\n';
      ++failures;
    }
  }
  std::cout << "service lifecycle: " << tests.size() - failures << "/"
            << tests.size() << " passed\n";
  return failures == 0 ? 0 : 1;
}
