/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_session.h"
#include "services/muon_service_client.h"
#include "services/muon_service_transport.h"
#include <cerrno>
#include <map>
#include <optional>
#include <system_error>
#include <vector>

struct MuonServiceSessionImpl {
  MuonServiceProcessOptions options;
  cardio::primitives::mutex connection_mutex;
  cardio::primitives::manually_conditional idle{true};
  cardio::primitives::manually_conditional stopped{false};
  cardio::cancellation_source cancellation;
  std::map<std::string, std::shared_ptr<MuonServiceClient>> pages;
  std::optional<MuonServiceTransport> monitor;
  std::vector<MuonServiceProcess> launches;
  size_t jobs = 0;
  bool stopping = false;
};

struct MuonServiceSessionJob {
  std::shared_ptr<MuonServiceSessionImpl> impl;
  explicit MuonServiceSessionJob(std::shared_ptr<MuonServiceSessionImpl> value) : impl(std::move(value)) {
    ++impl->jobs;
    impl->idle.drop();
  }
  ~MuonServiceSessionJob() { if (--impl->jobs == 0) impl->idle.raise(); }
};

static bool MissingEndpoint(const std::system_error& error) {
#if defined(_WIN32)
  // ERROR_FILE_NOT_FOUND and ERROR_PATH_NOT_FOUND; other pipe failures must
  // retain their original error rather than launching a competing host.
  return error.code().category() == std::system_category() &&
      (error.code().value() == 2 || error.code().value() == 3);
#else
  return error.code().category() == std::generic_category() &&
      (error.code().value() == ENOENT || error.code().value() == ECONNREFUSED);
#endif
}

static cardio::promise<MuonServiceTransport> Connect(
    std::shared_ptr<MuonServiceSessionImpl> impl, cardio::cancellation cancellation) {
  auto lock = std::move(co_await impl->connection_mutex.lock(cancellation));
  if (impl->stopping) throw std::runtime_error("Service UI session is stopping");
  std::optional<MuonServiceTransport> transport;
  try { transport = co_await ConnectMuonServiceTransport(impl->options.endpoint, cancellation); }
  catch (const std::system_error& error) { if (!MissingEndpoint(error)) throw; }
  if (!transport) {
    if (impl->options.connect_only) throw std::runtime_error("Development service host is unavailable");
    impl->launches.push_back(co_await LaunchMuonServiceProcess(impl->options, cancellation));
    transport = co_await ConnectMuonServiceTransport(impl->options.endpoint, cancellation);
  }
  if (impl->stopping || cancellation.is_cancellation_requested()) {
    transport->close();
    throw cardio::canceled_exception();
  }
  co_return std::move(*transport);
}

static cardio::promise<void> Start(std::shared_ptr<MuonServiceSessionImpl> impl) {
  MuonServiceSessionJob job(impl);
  if (impl->monitor) co_return;
  auto transport = co_await Connect(impl, impl->cancellation.get_cancellation());
  // Concurrent initialization can create a second harmless monitor connection.
  // Keep exactly one, and never turn it into a service lifetime reference.
  if (impl->monitor) transport.close();
  else impl->monitor = std::move(transport);
  if (!impl->options.startup.empty()) {
    MuonServiceClient administration([impl](cardio::cancellation cancellation) {
      return Connect(impl, cancellation);
    });
    std::exception_ptr error;
    try { co_await administration.Call("configureStartup", "{\"startup\":" + impl->options.startup + "}"); }
    catch (...) { error = std::current_exception(); }
    co_await administration.Close();
    if (error) std::rethrow_exception(error);
  }
}

static cardio::promise<void> Release(std::shared_ptr<MuonServiceSessionImpl> impl,
                                    std::shared_ptr<MuonServiceClient> client) {
  MuonServiceSessionJob job(impl);
  co_await client->Close();
}

static cardio::promise<void> Stop(std::shared_ptr<MuonServiceSessionImpl> impl, bool shutdown) {
  if (impl->stopping) { co_await impl->stopped.wait(); co_return; }
  impl->stopping = true;
  impl->cancellation.cancel();
  auto pages = std::move(impl->pages);
  impl->pages.clear();
  std::vector<cardio::promise<void>> closed;
  for (const auto& [owner, page] : pages) { (void)owner; closed.push_back(page->Close()); }
  for (auto& close : closed) co_await close;
  co_await impl->idle.wait();
  std::exception_ptr failure;
  if (shutdown) {
    std::optional<MuonServiceTransport> transport;
    try {
      // Administrative shutdown is intentionally unavailable to page payloads.
      // Connect without spawning: explicit exit must never start a new host.
      transport = co_await ConnectMuonServiceTransport(impl->options.endpoint, {});
      co_await transport->write("{\"requestId\":\"exit\",\"op\":\"shutdown\"}\n");
      while (!(co_await transport->read()).empty()) {}
    } catch (const std::system_error& error) {
      if (!MissingEndpoint(error)) failure = std::current_exception();
    } catch (...) { failure = std::current_exception(); }
    if (transport) transport->close();
  }
  if (impl->monitor) { impl->monitor->close(); impl->monitor.reset(); }
  for (auto& process : impl->launches) {
    try { co_await process.detach(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
  }
  impl->launches.clear();
  impl->stopped.raise();
  if (failure) std::rethrow_exception(failure);
}

MuonServiceSession::MuonServiceSession(MuonServiceProcessOptions options)
    : impl_(std::make_shared<MuonServiceSessionImpl>()) { impl_->options = std::move(options); }

cardio::promise<void> MuonServiceSession::Start() { return ::Start(impl_); }

cardio::promise<std::string> MuonServiceSession::Call(
    std::string owner, std::string operation, std::string json) {
  if (impl_->stopping) throw std::runtime_error("Service UI session is stopping");
  if (owner.empty()) throw std::invalid_argument("Service page owner is missing");
  auto found = impl_->pages.find(owner);
  if (found == impl_->pages.end()) {
    auto client = std::make_shared<MuonServiceClient>([impl = impl_](cardio::cancellation cancellation) {
      return Connect(impl, cancellation);
    });
    found = impl_->pages.emplace(std::move(owner), std::move(client)).first;
  }
  return found->second->Call(std::move(operation), std::move(json));
}

void MuonServiceSession::ReleaseOwner(const std::string& owner) {
  const auto found = impl_->pages.find(owner);
  if (found == impl_->pages.end()) return;
  auto client = std::move(found->second);
  impl_->pages.erase(found);
  cardio::fire_and_forget(Release(impl_, std::move(client)));
}

cardio::promise<void> MuonServiceSession::Stop(bool shutdown) { return ::Stop(impl_, shutdown); }
