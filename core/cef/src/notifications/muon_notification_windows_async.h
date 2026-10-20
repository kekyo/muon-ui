/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cardio.h>
#include <functional>

namespace muon_internal {

/**
 * Adapts synchronous Windows notification metadata operations to cardio.
 * @param work One metadata operation without an asynchronous completion API.
 * @return Completion on the submitting dispatcher, with OS failures propagated.
 * @remarks Uses the OS default pool. The callback must perform its operation
 * directly, without polling or waiting for another asynchronous operation.
 */
cardio::promise<void> RunMuonWindowsNotificationTask(std::function<void()> work);

}  // namespace muon_internal
