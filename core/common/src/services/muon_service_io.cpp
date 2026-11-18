/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "services/muon_service_io.h"
#include "services/muon_service_files.h"
#include "services/muon_service_http.h"
#include <set>

struct MuonServiceIoState {
  std::string directory;
  MuonServiceIoSettings settings;
  std::function<void(std::function<void()>)> post;
  MuonServiceIoDelay delay;
  size_t active_http = 0;
  std::set<std::shared_ptr<MuonServiceIoCancellation>> pending;
  bool closed = false;
  std::vector<std::function<void()>> waiters;
};

MuonServiceIo::MuonServiceIo(std::string directory, MuonServiceIoSettings settings,
    std::function<void(std::function<void()>)> post, MuonServiceIoDelay delay) : state_(std::make_shared<MuonServiceIoState>()) {
  state_->directory = std::move(directory);
  state_->settings = std::move(settings);
  state_->delay = std::move(delay);
  state_->post = std::move(post);
}

static void Drained(const std::shared_ptr<MuonServiceIoState>& state) {
  if (!state->closed || !state->pending.empty()) return;
  auto waiters = std::move(state->waiters);
  for (auto& waiter : waiters) waiter();
}

std::function<void()> MuonServiceIo::Run(MuonServiceIoRequest request, std::function<void(MuonServiceIoReply)> complete) {
  const auto state = state_;
  const bool http = request.operation == "http.request";
  const auto cancel = std::make_shared<MuonServiceIoCancellation>();
  const auto reply = std::make_shared<MuonServiceIoReply>();
  const bool full = state->pending.size() >= 64 || (http && state->active_http >= 8);
  if (state->closed || full || !state->settings.submit) {
    reply->code = state->closed ? "cancelled" : full ? "resource-exhausted" : "permission-denied";
    reply->message = "Service I/O admission is closed";
    state->post([reply, complete = std::move(complete)] { complete(std::move(*reply)); });
    return [] {};
  }
  std::function<void()> stop_timer;
  if (http) {
    try {
      const auto timeout = MuonServiceHttpTimeout(request.parameters);
      if (state->delay) stop_timer = state->delay(std::chrono::milliseconds(timeout), [cancel] { cancel->Cancel(true); });
    } catch (const std::exception& error) {
      reply->code = "invalid-input"; reply->message = error.what();
      state->post([reply, complete = std::move(complete)] { complete(std::move(*reply)); });
      return [] {};
    }
    ++state->active_http;
  }
  state->pending.insert(cancel);
  auto finish = [state, cancel, reply, http, stop_timer = std::move(stop_timer), complete = std::move(complete)](std::string error) {
    state->post([state, cancel, reply, http, stop_timer, complete, error = std::move(error)] {
      if (!state->pending.contains(cancel)) return;
      if (stop_timer) stop_timer();
      if (http) --state->active_http;
      if (!error.empty()) { reply->code = "io-error"; reply->message = error; }
      state->pending.erase(cancel);
      complete(std::move(*reply));
      Drained(state);
    });
  };
  try {
    state->settings.submit([state, cancel, reply, http, request = std::move(request)] {
      try {
        cancel->Check();
        if (http) *reply = RunMuonServiceHttp(state->settings.policy, request, state->settings.http, *cancel);
        else *reply = RunMuonServiceFile(state->directory, state->settings.policy, request, cancel->cancelled);
      }
      catch (const MuonServiceIoError& error) { reply->code = error.code; reply->message = error.what(); }
      catch (const std::exception& error) { reply->code = "io-error"; reply->message = error.what(); }
      cancel->Bind({});
      if (http && cancel->cancelled.load()) {
        try { cancel->Check(); } catch (const MuonServiceIoError& error) { reply->code = error.code; reply->message = error.what(); }
      }
    }, finish);
  } catch (const std::exception& error) { finish(error.what()); }
  return [cancel] { cancel->Cancel(); };
}

void MuonServiceIo::Close(std::function<void()> drained) {
  state_->closed = true;
  for (const auto& cancel : state_->pending) cancel->Cancel();
  state_->waiters.push_back(std::move(drained));
  Drained(state_);
}
