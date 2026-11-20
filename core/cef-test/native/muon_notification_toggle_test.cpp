#include "muon_notification_toggle.h"
#include <cassert>
#include <iostream>

int main() {
  auto state = std::optional<bool>{};
  bool toggled = false;
  int changes = 0;
  const auto read = [&] { return state; };
  const auto toggle = [&] { ++changes; state.reset(); };

  // An unresolved initial value must neither fail nor mutate the OS setting.
  assert(!ObserveMuonNotificationToggle(read, toggle, false, toggled));
  assert(changes == 0 && !toggled);
  state = true;
  assert(!ObserveMuonNotificationToggle(read, toggle, false, toggled));
  assert(changes == 1 && toggled);
  // Unrelated events and an unresolved post-toggle value must not toggle twice.
  for (int event = 0; event < 3; ++event)
    assert(!ObserveMuonNotificationToggle(read, toggle, false, toggled));
  assert(changes == 1);
  state = false;
  assert(ObserveMuonNotificationToggle(read, toggle, false, toggled) == false);

  // Reading the original setting also requires a definite value for restoration.
  state.reset();
  toggled = false;
  for (int event = 0; event < 3; ++event) {
    assert(!ObserveMuonNotificationToggle(read, toggle, std::nullopt, toggled));
    assert(!ObserveMuonNotificationToggle(read, toggle, true, toggled));
  }
  assert(changes == 1 && !toggled);
  state = true;
  assert(ObserveMuonNotificationToggle(read, toggle, std::nullopt, toggled) == true);
  assert(ObserveMuonNotificationToggle(read, toggle, true, toggled) == true);
  assert(changes == 1 && !toggled);
  std::cout << "Notification setting transitions passed\n";
}
