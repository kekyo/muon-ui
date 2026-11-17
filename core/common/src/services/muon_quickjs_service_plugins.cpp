/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_quickjs_service_plugins.h"
#include <cassert>
#include <stdexcept>

struct MuonQuickJsPluginStartup {
  std::shared_ptr<MuonServicePluginLease> lease;
  std::function<void(const std::string&)> complete;
  std::function<void()> cancel_deadline;
};

struct MuonQuickJsServicePluginsImpl {
  MuonServicePluginPool& pool;
  decltype(MuonQuickJsServicePlatform::delay) delay;
  std::map<std::pair<std::string,uint64_t>, std::shared_ptr<MuonQuickJsPluginStartup>> pending;
};

static void Report(const std::shared_ptr<MuonQuickJsPluginStartup>& startup,
                   const std::string& error) {
  if (startup->cancel_deadline) startup->cancel_deadline();
  startup->cancel_deadline = nullptr;
  auto complete = std::move(startup->complete);
  startup->complete = nullptr;
  if (complete) complete(error);
}

static void ReleaseUnused(const std::shared_ptr<MuonQuickJsServicePluginsImpl>& owner,
                          const std::pair<std::string,uint64_t>& key,
                          const std::shared_ptr<MuonQuickJsPluginStartup>& startup) {
  if (startup->lease) {
    startup->lease->Close([owner,key,startup] {
      startup->lease.reset();
      owner->pending.erase(key);
    });
  } else owner->pending.erase(key);
}

MuonQuickJsServicePlugins::MuonQuickJsServicePlugins(MuonServicePluginPool& pool,
    decltype(MuonQuickJsServicePlatform::delay) delay)
    : impl_(std::make_shared<MuonQuickJsServicePluginsImpl>(pool,std::move(delay),
          std::map<std::pair<std::string,uint64_t>,std::shared_ptr<MuonQuickJsPluginStartup>>{})) {
  if (!impl_->delay) throw std::invalid_argument("Missing service plugin timer");
}
MuonQuickJsServicePlugins::~MuonQuickJsServicePlugins() { assert(IsIdle()); }
bool MuonQuickJsServicePlugins::IsIdle() const { return impl_->pending.empty(); }

void MuonQuickJsServicePlugins::Start(MuonQuickJsServiceHost& host,
    const MuonQuickJsServiceSpec& spec, uint64_t generation,
    std::function<void(const std::string&)> complete) {
  const auto key = std::make_pair(spec.id,generation);
  if (!spec.startup_deadline || !complete || impl_->pending.contains(key))
    throw std::invalid_argument("Invalid service plugin startup");
  auto startup = std::make_shared<MuonQuickJsPluginStartup>();
  startup->complete = std::move(complete);
  impl_->pending.emplace(key,startup);
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
      *spec.startup_deadline-std::chrono::steady_clock::now());
  if (remaining.count() <= 0) {
    impl_->pending.erase(key);
    Report(startup,"Service native plugin startup deadline exceeded");
    return;
  }
  startup->cancel_deadline = impl_->delay(remaining,[startup] {
    Report(startup,"Service native plugin startup deadline exceeded");
  });
  const auto owner = impl_;
  owner->pool.Acquire(spec.id,generation,[owner,key,startup,spec,&host,generation](
      std::shared_ptr<MuonServicePluginLease> lease,const std::string& error) {
    startup->lease = std::move(lease);
    if (!startup->complete || !error.empty() ||
        std::chrono::steady_clock::now() >= *spec.startup_deadline) {
      Report(startup,error.empty() ? "Service native plugin startup deadline exceeded" : error);
      ReleaseUnused(owner,key,startup);
      return;
    }
    // The host now enforces the same absolute deadline through evaluation/start.
    if (startup->cancel_deadline) startup->cancel_deadline();
    startup->cancel_deadline = nullptr;
    try {
      host.Start(spec,generation,[owner,key,startup](const std::string& error) {
        Report(startup,error);
        ReleaseUnused(owner,key,startup);
      });
    } catch (const std::exception& error) {
      Report(startup,error.what());
      ReleaseUnused(owner,key,startup);
    }
  });
}

MuonQuickJsServiceExtension MuonQuickJsServicePlugins::Attach(const std::string& id,
    uint64_t generation, MuonQuickJsEngine& engine, std::function<void()> wake,
    std::function<void(const std::string&)> fault) {
  const auto found = impl_->pending.find({id,generation});
  if (found == impl_->pending.end()) return {};
  const auto startup = found->second;
  if (!startup->lease) throw std::logic_error("Service native plugins already attached");
  const auto lease = startup->lease;
  const auto bindings = lease->Attach(engine,std::move(wake),std::move(fault));
  startup->lease.reset();
  return {
    [bindings](JSContext* context,const std::string& name) { return bindings->LoadModule(context,name.c_str()); },
    [bindings] { bindings->Detach(); },
    [bindings,lease](std::function<void()> complete) {
      bindings->Close([lease,complete = std::move(complete)] { complete(); });
    }};
}
