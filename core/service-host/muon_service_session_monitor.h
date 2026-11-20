/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include <functional>

/** Monitors this user's desktop portal without inhibiting logout.
 * @param ending Called on the owning GLib context when the session is ending.
 * @return Idempotent cleanup to call on the same context before destroying the host.
 * @remarks An unavailable or older portal leaves ordinary host shutdown available. */
std::function<void()> WatchMuonServiceSession(std::function<void()> ending);
