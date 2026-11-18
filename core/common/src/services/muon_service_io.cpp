/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "services/muon_service_io.h"
#include "services/muon_service_files.h"
#include <set>

struct MuonServiceIoState {
  std::string directory;
  MuonServiceIoSettings settings;
  std::function<void(std::function<void()>)> post;
  std::set<std::shared_ptr<std::atomic_bool>> pending;
  bool closed = false;
  std::vector<std::function<void()>> waiters;
};

MuonServiceIo::MuonServiceIo(std::string directory, MuonServiceIoSettings settings,
    std::function<void(std::function<void()>)> post) : state_(std::make_shared<MuonServiceIoState>()) {
  state_->directory = std::move(directory);
  state_->settings = std::move(settings);
  state_->post = std::move(post);
}

static void Drained(const std::shared_ptr<MuonServiceIoState>& state) {
  if (!state->closed || !state->pending.empty()) return;
  auto waiters = std::move(state->waiters);
  for (auto& waiter : waiters) waiter();
}

std::function<void()> MuonServiceIo::Run(MuonServiceIoRequest request, std::function<void(MuonServiceIoReply)> complete) {
  const auto state = state_;
  const auto cancel = std::make_shared<std::atomic_bool>(false);
  const auto reply = std::make_shared<MuonServiceIoReply>();
  if (state->closed || state->pending.size() >= 64 || !state->settings.submit) {
    reply->code = state->closed ? "cancelled" : state->pending.size() >= 64 ? "resource-exhausted" : "permission-denied";
    reply->message = "Service I/O admission is closed";
    state->post([reply, complete = std::move(complete)] { complete(std::move(*reply)); });
    return [] {};
  }
  state->pending.insert(cancel);
  auto finish = [state, cancel, reply, complete = std::move(complete)](std::string error) {
    state->post([state, cancel, reply, complete, error = std::move(error)] {
      if (!state->pending.contains(cancel)) return;
      if (!error.empty()) { reply->code = "io-error"; reply->message = error; }
      complete(std::move(*reply));
      state->pending.erase(cancel);
      Drained(state);
    });
  };
  try {
    state->settings.submit([state, cancel, reply, request = std::move(request)] {
      try { *reply = RunMuonServiceFile(state->directory, state->settings.policy, request, *cancel); }
      catch (const MuonServiceIoError& error) { reply->code = error.code; reply->message = error.what(); }
      catch (const std::exception& error) { reply->code = "io-error"; reply->message = error.what(); }
    }, finish);
  } catch (const std::exception& error) { finish(error.what()); }
  return [cancel] { cancel->store(true); };
}

void MuonServiceIo::Close(std::function<void()> drained) {
  state_->closed = true;
  for (const auto& cancel : state_->pending) cancel->store(true);
  state_->waiters.push_back(std::move(drained));
  Drained(state_);
}
