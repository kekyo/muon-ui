/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include <cardio.h>
#include <memory>
#include <functional>

/**
 * Borrows the UI process's sole cardio host on its existing Android Looper.
 * @returns A lease retaining the host until its owner's I/O and continuations drain.
 * @throws std::runtime_error When used from a different thread or without a Looper.
 * @remarks No plugin runtime is created. Release the final lease only after
 * completed coroutine frames and their queued cleanup have been disposed.
 */
std::shared_ptr<cardio::dispatcher_host_android_auto> AcquireMuonAndroidDispatcher();

/**
 * Retains module cleanup until the shared host and its queued work are destroyed.
 * @param host A live lease returned by AcquireMuonAndroidDispatcher.
 * @param cleanup Runs on the owner Looper after the final host lease is released.
 */
void RetainMuonAndroidDispatcherCleanup(
    const std::shared_ptr<cardio::dispatcher_host_android_auto>& host,
    std::function<void()> cleanup);
