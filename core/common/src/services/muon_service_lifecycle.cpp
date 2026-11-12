/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_lifecycle.h"

#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

struct MuonServiceClientReference {
  MuonServiceReference identity;
  bool ready = false;
};

struct MuonServiceSettingRequest {
  uint64_t id;
  bool enabled;
};

struct MuonServiceEntry {
  MuonServiceDefinition definition;
  MuonServiceState state = MuonServiceState::Stopped;
  uint64_t generation = 0;
  uint64_t revision = 0;
  bool resident = false;
  std::string error;
  std::map<uint64_t, MuonServiceClientReference> references;
  std::deque<MuonServiceSettingRequest> settings;
  bool saving = false;
};

struct MuonServiceLifecycleImpl {
  std::map<std::string, MuonServiceEntry> services;
  std::vector<MuonServiceEvent> events;
  uint64_t next_reference = 1;
  uint64_t next_operation = 1;
  bool closing = false;
};

static uint64_t AllocateIdentity(uint64_t& next) {
  const auto value = next;
  if (next != 0) {
    next = next == std::numeric_limits<uint64_t>::max() ? 0 : next + 1;
  }
  return value;
}

static MuonServiceSnapshot Snapshot(const MuonServiceEntry& entry) {
  return {entry.definition.id, entry.state, entry.generation, entry.revision,
          entry.definition.user_autostart.value_or(
              entry.definition.default_autostart),
          entry.resident, entry.references.size(), entry.error};
}

static MuonServiceEvent Event(MuonServiceEventKind kind,
                              const MuonServiceEntry& entry) {
  auto event = MuonServiceEvent{};
  event.kind = kind;
  event.snapshot = Snapshot(entry);
  return event;
}

static void Changed(MuonServiceLifecycleImpl& impl,
                    const MuonServiceEntry& entry) {
  impl.events.push_back(Event(MuonServiceEventKind::Changed, entry));
}

static bool HasDemand(const MuonServiceEntry& entry) {
  return entry.resident || !entry.references.empty();
}

static void StartIfNeeded(MuonServiceLifecycleImpl& impl,
                          MuonServiceEntry& entry) {
  if (impl.closing || !HasDemand(entry) ||
      (entry.state != MuonServiceState::Stopped &&
       entry.state != MuonServiceState::Failed)) return;
  if (entry.generation == std::numeric_limits<uint64_t>::max()) {
    entry.state = MuonServiceState::Failed;
    entry.error = "service generation identities exhausted";
    Changed(impl, entry);
    return;
  }
  ++entry.generation;
  entry.state = MuonServiceState::Starting;
  entry.error.clear();
  Changed(impl, entry);
  impl.events.push_back(Event(MuonServiceEventKind::Start, entry));
}

static void StopIfUnused(MuonServiceLifecycleImpl& impl,
                         MuonServiceEntry& entry) {
  // An in-flight start must settle before its resources can be disposed. Its
  // completion rechecks demand instead of handing an orphan runtime to a page.
  if (HasDemand(entry) || entry.state != MuonServiceState::Running) return;
  entry.state = MuonServiceState::Stopping;
  Changed(impl, entry);
  impl.events.push_back(Event(MuonServiceEventKind::Stop, entry));
}

static void ReferenceEvent(MuonServiceLifecycleImpl& impl,
                           const MuonServiceEntry& entry,
                           MuonServiceEventKind kind,
                           const MuonServiceReference& reference,
                           const std::string& error) {
  auto event = Event(kind, entry);
  event.reference = reference;
  event.error = error;
  impl.events.push_back(std::move(event));
}

static void RemoveReferences(MuonServiceLifecycleImpl& impl,
                             MuonServiceEntry& entry, bool all_generations,
                             const std::string& reason) {
  auto removed = std::vector<MuonServiceClientReference>{};
  for (auto it = entry.references.begin(); it != entry.references.end();) {
    if (all_generations || it->second.identity.generation == entry.generation) {
      removed.push_back(it->second);
      it = entry.references.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& reference : removed) {
    ReferenceEvent(impl, entry,
                   reference.ready ? MuonServiceEventKind::Invalidated
                                   : MuonServiceEventKind::AcquireFailed,
                   reference.identity, reason);
  }
}

static void SettingResult(MuonServiceLifecycleImpl& impl,
                          const MuonServiceEntry& entry,
                          const MuonServiceSettingRequest& request,
                          const std::string& error) {
  auto event = Event(MuonServiceEventKind::AutostartResult, entry);
  event.operation_id = request.id;
  event.autostart = request.enabled;
  event.success = error.empty();
  event.error = error;
  impl.events.push_back(std::move(event));
}

static void IssueNextSave(MuonServiceLifecycleImpl& impl,
                          MuonServiceEntry& entry) {
  while (!entry.saving && !entry.settings.empty()) {
    const auto request = entry.settings.front();
    if (impl.closing) {
      entry.settings.pop_front();
      SettingResult(impl, entry, request, "service host is shutting down");
      continue;
    }
    // An absent override must be written even when the manifest has the same
    // effective value. Otherwise a future manifest could undo a user's OFF.
    if (entry.definition.user_autostart.has_value() &&
        entry.definition.user_autostart.value() == request.enabled) {
      entry.settings.pop_front();
      SettingResult(impl, entry, request, "");
      continue;
    }
    entry.saving = true;
    auto event = Event(MuonServiceEventKind::SaveAutostart, entry);
    event.operation_id = request.id;
    event.autostart = request.enabled;
    impl.events.push_back(std::move(event));
  }
}

MuonServiceLifecycle::MuonServiceLifecycle(
    const std::vector<MuonServiceDefinition>& definitions)
    : impl_(std::make_unique<MuonServiceLifecycleImpl>()) {
  for (const auto& definition : definitions) {
    if (definition.id.empty() || impl_->services.contains(definition.id)) {
      throw std::invalid_argument("service identities must be nonempty and unique");
    }
    auto entry = MuonServiceEntry{};
    entry.definition = definition;
    entry.resident = definition.user_autostart.value_or(definition.default_autostart);
    impl_->services.emplace(definition.id, std::move(entry));
  }
  for (auto& [id, entry] : impl_->services) {
    StartIfNeeded(*impl_, entry);
  }
}

MuonServiceLifecycle::~MuonServiceLifecycle() = default;

std::optional<MuonServiceReference> MuonServiceLifecycle::Acquire(
    const std::string& service_id, const std::string& owner_id) {
  const auto found = impl_->services.find(service_id);
  if (impl_->closing || owner_id.empty() || found == impl_->services.end()) {
    return std::nullopt;
  }
  auto& entry = found->second;
  const auto current = entry.state == MuonServiceState::Starting ||
                       entry.state == MuonServiceState::Running;
  if (!current && entry.generation == std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  const auto id = AllocateIdentity(impl_->next_reference);
  if (id == 0) return std::nullopt;
  const auto reference = MuonServiceReference{
      id, service_id, owner_id, current ? entry.generation : entry.generation + 1};
  const auto ready = entry.state == MuonServiceState::Running;
  entry.references.emplace(id, MuonServiceClientReference{reference, ready});
  Changed(*impl_, entry);
  if (ready) {
    ReferenceEvent(*impl_, entry, MuonServiceEventKind::Acquired, reference, "");
  } else {
    StartIfNeeded(*impl_, entry);
  }
  return reference;
}

bool MuonServiceLifecycle::Release(const MuonServiceReference& reference) {
  const auto found = impl_->services.find(reference.service_id);
  if (found == impl_->services.end()) return false;
  auto& entry = found->second;
  const auto retained = entry.references.find(reference.id);
  if (retained == entry.references.end() ||
      retained->second.identity != reference) return false;
  const auto was_ready = retained->second.ready;
  entry.references.erase(retained);
  Changed(*impl_, entry);
  if (!was_ready) {
    ReferenceEvent(*impl_, entry, MuonServiceEventKind::AcquireFailed,
                   reference, "service acquisition was released");
  }
  StopIfUnused(*impl_, entry);
  return true;
}

size_t MuonServiceLifecycle::ReleaseOwner(const std::string& owner_id) {
  auto references = std::vector<MuonServiceReference>{};
  for (const auto& [id, entry] : impl_->services) {
    for (const auto& [lease_id, reference] : entry.references) {
      if (reference.identity.owner_id == owner_id) {
        references.push_back(reference.identity);
      }
    }
  }
  for (const auto& reference : references) Release(reference);
  return references.size();
}

void MuonServiceLifecycle::CompleteStart(const std::string& service_id,
                                         uint64_t generation,
                                         const std::string& error) {
  const auto found = impl_->services.find(service_id);
  if (found == impl_->services.end()) return;
  auto& entry = found->second;
  if (entry.generation != generation || entry.state != MuonServiceState::Starting) {
    return;
  }
  if (!error.empty()) {
    RuntimeExited(service_id, generation, error);
    return;
  }
  entry.state = MuonServiceState::Running;
  for (auto& [id, reference] : entry.references) {
    if (reference.identity.generation == generation) reference.ready = true;
  }
  Changed(*impl_, entry);
  for (const auto& [id, reference] : entry.references) {
    if (reference.identity.generation == generation) {
      ReferenceEvent(*impl_, entry, MuonServiceEventKind::Acquired,
                     reference.identity, "");
    }
  }
  StopIfUnused(*impl_, entry);
}

void MuonServiceLifecycle::CompleteStop(const std::string& service_id,
                                        uint64_t generation) {
  const auto found = impl_->services.find(service_id);
  if (found == impl_->services.end()) return;
  auto& entry = found->second;
  if (entry.generation != generation || entry.state != MuonServiceState::Stopping) {
    return;
  }
  entry.state = MuonServiceState::Stopped;
  entry.error.clear();
  RemoveReferences(*impl_, entry, false, "service generation stopped");
  Changed(*impl_, entry);
  StartIfNeeded(*impl_, entry);
}

void MuonServiceLifecycle::RuntimeExited(const std::string& service_id,
                                         uint64_t generation,
                                         const std::string& error) {
  const auto found = impl_->services.find(service_id);
  if (found == impl_->services.end()) return;
  auto& entry = found->second;
  if (entry.generation != generation || entry.state == MuonServiceState::Stopped ||
      entry.state == MuonServiceState::Failed) return;
  const auto was_stopping = entry.state == MuonServiceState::Stopping;
  entry.state = MuonServiceState::Failed;
  entry.error = error.empty() ? "service runtime exited" : error;
  RemoveReferences(*impl_, entry, false, entry.error);
  Changed(*impl_, entry);
  // New demand received during a normal stop is an explicit acquisition or
  // policy change. It may proceed once the old process has actually exited.
  // A running/starting failure never retries a retained resident reference.
  if (was_stopping) StartIfNeeded(*impl_, entry);
}

uint64_t MuonServiceLifecycle::SetAutostart(const std::string& service_id,
                                           bool enabled) {
  const auto found = impl_->services.find(service_id);
  if (impl_->closing || found == impl_->services.end()) return 0;
  const auto id = AllocateIdentity(impl_->next_operation);
  if (id == 0) return 0;
  auto& entry = found->second;
  entry.settings.push_back({id, enabled});
  IssueNextSave(*impl_, entry);
  return id;
}

void MuonServiceLifecycle::CompleteAutostart(const std::string& service_id,
                                             uint64_t operation_id,
                                             const std::string& error) {
  const auto found = impl_->services.find(service_id);
  if (found == impl_->services.end()) return;
  auto& entry = found->second;
  if (!entry.saving || entry.settings.empty() ||
      entry.settings.front().id != operation_id) return;
  const auto request = entry.settings.front();
  entry.settings.pop_front();
  entry.saving = false;
  if (error.empty()) {
    const auto was_resident = entry.resident;
    entry.definition.user_autostart = request.enabled;
    entry.revision = request.id;
    entry.resident = request.enabled && !impl_->closing;
    Changed(*impl_, entry);
    if (entry.resident && !was_resident) StartIfNeeded(*impl_, entry);
    StopIfUnused(*impl_, entry);
  }
  SettingResult(*impl_, entry, request, error);
  IssueNextSave(*impl_, entry);
}

void MuonServiceLifecycle::Shutdown(const std::string& reason) {
  if (impl_->closing) return;
  impl_->closing = true;
  const auto diagnostic = reason.empty() ? "service host is shutting down" : reason;
  for (auto& [id, entry] : impl_->services) {
    entry.resident = false;
    RemoveReferences(*impl_, entry, true, diagnostic);
    Changed(*impl_, entry);
    StopIfUnused(*impl_, entry);
    // The issued save owns its eventual durable result. Cancel only requests
    // that have not reached the storage adapter, preserving their FIFO order.
    auto pending = entry.settings.begin();
    if (entry.saving) ++pending;
    const auto cancelled = std::vector<MuonServiceSettingRequest>(
        pending, entry.settings.end());
    entry.settings.erase(pending, entry.settings.end());
    for (const auto& request : cancelled) {
      SettingResult(*impl_, entry, request, diagnostic);
    }
  }
}

bool MuonServiceLifecycle::IsReady(const MuonServiceReference& reference) const {
  const auto found = impl_->services.find(reference.service_id);
  if (found == impl_->services.end()) return false;
  const auto& entry = found->second;
  if (entry.state != MuonServiceState::Running ||
      entry.generation != reference.generation) return false;
  const auto retained = entry.references.find(reference.id);
  return retained != entry.references.end() && retained->second.ready &&
         retained->second.identity == reference;
}

std::optional<MuonServiceSnapshot> MuonServiceLifecycle::GetSnapshot(
    const std::string& service_id) const {
  const auto found = impl_->services.find(service_id);
  if (found == impl_->services.end()) return std::nullopt;
  return Snapshot(found->second);
}

bool MuonServiceLifecycle::IsShuttingDown() const { return impl_->closing; }

bool MuonServiceLifecycle::IsQuiescent() const {
  for (const auto& [id, entry] : impl_->services) {
    if ((entry.state != MuonServiceState::Stopped &&
         entry.state != MuonServiceState::Failed) ||
        entry.saving || !entry.settings.empty()) return false;
  }
  return true;
}

std::vector<MuonServiceEvent> MuonServiceLifecycle::TakeEvents() {
  auto events = std::vector<MuonServiceEvent>{};
  events.swap(impl_->events);
  return events;
}
