/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include <cardio.h>
#include <functional>
#include <map>
#include <string>

/** Serializes UI/service preferences and derives one application login entry. */
class MuonServiceLoginPolicy final {
 public:
  /**
   * @param preferences Effective values; the empty key denotes the UI preference.
   * @param save Atomically persists one override; failure leaves its old value intact.
   * @param registration Creates/removes the application's OS login entry.
   * @param diagnostic Reports failure to clean up a redundant derived OS entry.
   * @remarks All methods run on the owner dispatcher. Await pending operations
   * before disposing this object. Registration is derived from preferences and
   * never used to infer the UI preference.
   */
  MuonServiceLoginPolicy(std::map<std::string, bool> preferences,
      std::function<cardio::promise<void>(std::string, bool)> save,
      std::function<cardio::promise<void>(bool)> registration,
      std::function<void(const std::string&)> diagnostic);
  /** @return The committed UI preference, independent of running services. */
  bool UiEnabled() const;
  /** @return After reconciling the OS entry during ordinary launch or setup.
   * @remarks Do not rewrite Windows Run entries while processing login itself. */
  cardio::promise<void> Reconcile();
  /** @param registration Updated command from a trusted ordinary/login launcher.
   *  @param refresh Reconcile now for ordinary launch/setup, or defer during login.
   *  @return After installing the command under the same settings transaction lock. */
  cardio::promise<void> Configure(std::function<cardio::promise<void>(bool)> registration, bool refresh);
  /**
   * @param id Configured service ID, or empty for UI auto-display.
   * @param enabled Explicit user override to persist.
   * @return After the override commits; may throw before commitment.
   * @remarks Failure to remove an unnecessary OS entry is diagnostic only.
   * Its login command reads committed OFF values and starts nothing. Enabling
   * from an all-OFF state requires successful OS registration before commitment.
   */
  cardio::promise<void> Set(std::string id, bool enabled);
 private:
  std::map<std::string, bool> preferences_;
  std::function<cardio::promise<void>(std::string, bool)> save_;
  std::function<cardio::promise<void>(bool)> registration_;
  std::function<void(const std::string&)> diagnostic_;
  cardio::primitives::mutex mutex_;
};
