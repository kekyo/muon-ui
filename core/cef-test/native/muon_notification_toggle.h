/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include <optional>

/** Observes a boolean notification switch, applying at most one toggle.
 * @tparam Read Callable returning the current boolean state or nullopt.
 * @tparam Toggle Callable issuing the UI toggle operation.
 * @param read Reads the current state; nullopt represents an indeterminate UI.
 * @param toggle Changes the switch once its initial state is known.
 * @param requested Desired state, or nullopt for observation without mutation.
 * @param toggled Whether this request has already issued a toggle.
 * @returns The confirmed state, or nullopt to await another UIA event. The caller
 *     retains its existing deadline; an indefinite unknown state is not success.
 */
template <typename Read, typename Toggle>
static std::optional<bool> ObserveMuonNotificationToggle(
    Read read, Toggle toggle, std::optional<bool> requested, bool& toggled) {
  auto state = read();
  if (!state) return std::nullopt;
  if (!requested) return state;
  if (state != requested && !toggled) {
    toggle();
    toggled = true;
    state = read();
  }
  return state == requested ? state : std::nullopt;
}
