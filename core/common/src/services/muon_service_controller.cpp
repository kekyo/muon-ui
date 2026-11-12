/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_controller.h"

#include <cassert>
#include <deque>
#include <stdexcept>
#include <utility>

struct MuonServiceControllerImpl {
  MuonServiceLifecycle lifecycle;
  MuonServiceBackend backend;
  std::deque<MuonServiceEvent> events;
  bool dispatching = false;
  bool scheduled = false;

  MuonServiceControllerImpl(const std::vector<MuonServiceDefinition>& definitions,
                             MuonServiceBackend callbacks)
      : lifecycle(definitions), backend(std::move(callbacks)) {}
};

static void Dispatch(const std::shared_ptr<MuonServiceControllerImpl>& impl);

static void DispatchAction(const std::shared_ptr<MuonServiceControllerImpl>& impl,
                            MuonServiceEvent event) {
  const auto weak = std::weak_ptr<MuonServiceControllerImpl>{impl};
  const auto id = event.snapshot.id;
  const auto generation = event.snapshot.generation;
  switch (event.kind) {
    case MuonServiceEventKind::Start: {
      const auto current = impl->lifecycle.GetSnapshot(id);
      if (!current || current->generation != generation ||
          current->state != MuonServiceState::Starting) return;
      if (impl->lifecycle.IsShuttingDown() || !current->error.empty()) {
        impl->lifecycle.CompleteStart(id, generation, current->error.empty()
            ? "service host is shutting down" : current->error);
        return;
      }
      impl->backend.start(event.snapshot, [weak, id, generation](const std::string& error) {
        if (const auto owner = weak.lock()) {
          owner->lifecycle.CompleteStart(id, generation, error);
          Dispatch(owner);
        }
      });
      return;
    }
    case MuonServiceEventKind::Stop:
      impl->backend.stop(event.snapshot, [weak, id, generation] {
        if (const auto owner = weak.lock()) {
          owner->lifecycle.CompleteStop(id, generation);
          Dispatch(owner);
        }
      });
      return;
    case MuonServiceEventKind::SaveAutostart:
      impl->backend.save_autostart(id, event.autostart,
          [weak, id, operation = event.operation_id](const std::string& error) {
        if (const auto owner = weak.lock()) {
          owner->lifecycle.CompleteAutostart(id, operation, error);
          Dispatch(owner);
        }
      });
      return;
    case MuonServiceEventKind::Acquired:
      if (!impl->lifecycle.IsReady(event.reference.value())) {
        // Another page's earlier notification may have shut down the host.
        // Its still-waiting peer needs a rejection, not a discarded response.
        event.kind = MuonServiceEventKind::AcquireFailed;
        event.error = "service acquisition was invalidated before delivery";
      }
      break;
    default:
      break;
  }
  impl->backend.notify(event);
}

static void Drain(const std::shared_ptr<MuonServiceControllerImpl>& impl) {
  impl->scheduled = false;
  impl->dispatching = true;
  struct DispatchScope {
    MuonServiceControllerImpl& impl;
    ~DispatchScope() { impl.dispatching = false; }
  } scope{*impl};
  while (!impl->events.empty()) {
    auto event = std::move(impl->events.front());
    impl->events.pop_front();
    DispatchAction(impl, std::move(event));
    // New observations always follow the remainder of the original batch,
    // including when a backend completes synchronously or notify reenters.
    for (auto& next : impl->lifecycle.TakeEvents()) {
      impl->events.push_back(std::move(next));
    }
  }
}

static void Dispatch(const std::shared_ptr<MuonServiceControllerImpl>& impl) {
  for (auto& event : impl->lifecycle.TakeEvents()) {
    impl->events.push_back(std::move(event));
  }
  if (impl->dispatching || impl->scheduled || impl->events.empty()) return;
  impl->scheduled = true;
  impl->backend.post([weak = std::weak_ptr<MuonServiceControllerImpl>{impl}] {
    if (const auto owner = weak.lock()) Drain(owner);
  });
}

MuonServiceController::MuonServiceController(
    const std::vector<MuonServiceDefinition>& definitions, MuonServiceBackend backend)
    : impl_(std::make_shared<MuonServiceControllerImpl>(definitions, std::move(backend))) {
  if (!impl_->backend.start || !impl_->backend.stop ||
      !impl_->backend.save_autostart || !impl_->backend.notify || !impl_->backend.post) {
    throw std::invalid_argument("service backend operations are required");
  }
}

MuonServiceController::~MuonServiceController() {
  assert(IsQuiescent());
}

void MuonServiceController::Activate() { Dispatch(impl_); }

std::optional<MuonServiceReference> MuonServiceController::Acquire(
    const std::string& id, const std::string& owner) {
  auto result = impl_->lifecycle.Acquire(id, owner);
  Dispatch(impl_);
  return result;
}

bool MuonServiceController::Release(const MuonServiceReference& reference) {
  const auto result = impl_->lifecycle.Release(reference);
  Dispatch(impl_);
  return result;
}

size_t MuonServiceController::ReleaseOwner(const std::string& owner) {
  const auto result = impl_->lifecycle.ReleaseOwner(owner);
  Dispatch(impl_);
  return result;
}

uint64_t MuonServiceController::SetAutostart(const std::string& id, bool enabled) {
  const auto result = impl_->lifecycle.SetAutostart(id, enabled);
  Dispatch(impl_);
  return result;
}

void MuonServiceController::RequestRuntimeStop(const std::string& id,
                                               uint64_t generation,
                                               const std::string& reason) {
  impl_->lifecycle.RequestRuntimeStop(id, generation, reason);
  Dispatch(impl_);
}

void MuonServiceController::RuntimeExited(const std::string& id,
                                          uint64_t generation,
                                          const std::string& reason) {
  impl_->lifecycle.RuntimeExited(id, generation, reason);
  Dispatch(impl_);
}

void MuonServiceController::Shutdown(const std::string& reason) {
  impl_->lifecycle.Shutdown(reason);
  Dispatch(impl_);
}

bool MuonServiceController::IsReady(const MuonServiceReference& reference) const {
  return impl_->lifecycle.IsReady(reference);
}

std::optional<MuonServiceSnapshot> MuonServiceController::GetSnapshot(
    const std::string& id) const {
  return impl_->lifecycle.GetSnapshot(id);
}

bool MuonServiceController::IsQuiescent() const {
  return impl_->lifecycle.IsQuiescent() && impl_->events.empty() &&
         !impl_->scheduled && !impl_->dispatching;
}
