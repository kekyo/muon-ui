/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "services/muon_service_process.h"
#include <memory>

/** Private UI-session state; no references owned by individual pages are shared. */
struct MuonServiceSessionImpl;

/** A UI process's connections to the independently owned desktop service host. */
class MuonServiceSession final {
 public:
  /** @param options App/profile-specific host paths and endpoint. */
  explicit MuonServiceSession(MuonServiceProcessOptions options);
  /** @returns After attaching the non-owning monitor and applying manifest startup. */
  cardio::promise<void> Start();
  /**
   * Dispatches a lifetime operation over a connection private to the native owner.
   * @param owner Browser-authenticated full page identity, never renderer input.
   * @param operation acquire, release, or getState.
   * @param json Validated request payload, excluding trusted owner identity.
   * @returns Native result JSON; connection loss invalidates that page's handles.
   */
  cardio::promise<std::string> Call(std::string owner, std::string operation, std::string json);
  /** @param owner Page identity to disconnect; asynchronous disposal is joined by Stop. */
  void ReleaseOwner(const std::string& owner);
  /**
   * Closes all UI-owned I/O after optional whole-application service shutdown.
   * @param shutdown True only for explicit app exit, not last-window closure.
   * @returns After native I/O and bootstrap cleanup; repeated calls are harmless.
   */
  cardio::promise<void> Stop(bool shutdown);
 private:
  std::shared_ptr<MuonServiceSessionImpl> impl_;
};
