/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_native_service_host.h"

#include <cassert>
#include <limits>
#include <map>
#include <stdexcept>

enum class MuonNativeInstanceState { Starting, Running, Stopping };

struct MuonNativeServiceEntry final
    : std::enable_shared_from_this<MuonNativeServiceEntry> {
  std::weak_ptr<MuonNativeServiceHostImpl> owner;
  MuonNativeServiceSpec spec;
  std::vector<muon_plugin_config_entry> config;
  muon_service_host host{};
  muon_service_context context{};
  std::shared_ptr<const muon_service_factory> factory;
  muon_service_instance instance{};
  MuonNativeInstanceState state = MuonNativeInstanceState::Starting;
  std::function<void(const std::string&)> started;
  std::vector<std::function<void()>> stopped;
};

struct MuonNativeServiceHostImpl {
  MuonNativeServicePlatform platform;
  std::map<std::string, std::weak_ptr<const muon_service_factory>> factories;
  std::map<std::string, std::shared_ptr<MuonNativeServiceEntry>> entries;
  size_t pending_tasks = 0;
};

static void Queue(const std::shared_ptr<MuonNativeServiceHostImpl>& impl,
                   std::function<void()> task) {
  ++impl->pending_tasks;
  impl->platform.post([impl, task = std::move(task)] {
    struct CompletionScope {
      MuonNativeServiceHostImpl& impl;
      ~CompletionScope() { --impl.pending_tasks; }
    } scope{*impl};
    task();
  });
}

static void PostNativeTask(void* data, muon_service_task task, void* task_data) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  assert(impl && task);
  Queue(impl, [entry, task, task_data] { task(task_data); });
}

static void LogNativeMessage(void* data, muon_log_level level,
                             const char* message) {
  const auto& entry = *static_cast<MuonNativeServiceEntry*>(data);
  const auto impl = entry.owner.lock();
  assert(impl && message);
  impl->platform.log(entry.spec.id, level, message);
}

static void NativeStarted(void* data, const muon_service_instance* instance,
                           const char* error) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  assert(impl);
  auto failure = error ? std::string(error) : std::string{};
  if (failure.empty() && (!instance || !instance->stop)) {
    failure = "native service returned an invalid instance descriptor";
  }
  if (failure.empty()) entry->instance = *instance;
  // Never erase the final library reference while returning through its start
  // or stop function. The platform queue also serializes synchronous factories
  // with asynchronously completing ones for the lifecycle manager.
  Queue(impl, [impl, entry, failure = std::move(failure)] {
    auto complete = std::move(entry->started);
    if (failure.empty()) entry->state = MuonNativeInstanceState::Running;
    else impl->entries.erase(entry->spec.id);
    complete(failure);
  });
}

static void NativeStopped(void* data) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  assert(impl);
  Queue(impl, [impl, entry] {
    impl->entries.erase(entry->spec.id);
    auto completions = std::move(entry->stopped);
    for (const auto& complete : completions) complete();
  });
}

MuonNativeServiceHost::MuonNativeServiceHost(MuonNativeServicePlatform platform)
    : impl_(std::make_shared<MuonNativeServiceHostImpl>()) {
  if (!platform.load || !platform.post || !platform.log) {
    throw std::invalid_argument("native service platform operations are required");
  }
  impl_->platform = std::move(platform);
}

MuonNativeServiceHost::~MuonNativeServiceHost() {
  assert(IsIdle());
}

void MuonNativeServiceHost::Start(
    const MuonNativeServiceSpec& spec, uint64_t generation,
    std::function<void(const std::string&)> complete) {
  auto error = std::string{};
  if (spec.id.empty() || spec.library_path.empty() || generation == 0 ||
      spec.config.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    error = "invalid native service identity or configuration";
  } else if (impl_->entries.contains(spec.id)) {
    error = "native service already has an active generation";
  }
  auto factory = std::shared_ptr<const muon_service_factory>{};
  if (error.empty()) {
    factory = impl_->factories[spec.library_path].lock();
    if (!factory) factory = impl_->platform.load(spec.library_path, error);
    if (error.empty() && !factory) error = "native service library has no factory";
    if (error.empty() && (factory->abi_version != MUON_SERVICE_ABI_VERSION ||
                          !factory->start)) {
      error = "native service factory ABI is incompatible";
    }
  }
  if (!error.empty()) {
    Queue(impl_, [complete = std::move(complete), error] { complete(error); });
    return;
  }
  impl_->factories[spec.library_path] = factory;
  auto entry = std::make_shared<MuonNativeServiceEntry>();
  entry->owner = impl_;
  entry->spec = spec;
  entry->factory = std::move(factory);
  entry->started = std::move(complete);
  for (const auto& [key, value] : entry->spec.config) {
    entry->config.push_back({key.c_str(), value.c_str()});
  }
  entry->host = {entry.get(), &PostNativeTask, &LogNativeMessage};
  entry->context = {&entry->host, entry->spec.id.c_str(), generation,
                    entry->spec.data_directory.c_str(),
                    static_cast<int32_t>(entry->config.size()),
                    entry->config.empty() ? nullptr : entry->config.data()};
  impl_->entries.emplace(spec.id, entry);
  entry->factory->start(&entry->context, &NativeStarted, entry.get());
}

void MuonNativeServiceHost::Stop(const std::string& id, uint64_t generation,
                                std::function<void()> complete) {
  const auto found = impl_->entries.find(id);
  if (found == impl_->entries.end() ||
      found->second->context.generation != generation) {
    Queue(impl_, std::move(complete));
    return;
  }
  const auto entry = found->second;
  if (entry->state == MuonNativeInstanceState::Starting) {
    throw std::logic_error("native service startup must complete before stopping");
  }
  entry->stopped.push_back(std::move(complete));
  if (entry->state == MuonNativeInstanceState::Stopping) return;
  entry->state = MuonNativeInstanceState::Stopping;
  entry->instance.stop(entry->instance.user_data, &NativeStopped, entry.get());
}

bool MuonNativeServiceHost::IsIdle() const {
  return impl_->entries.empty() && impl_->pending_tasks == 0;
}
