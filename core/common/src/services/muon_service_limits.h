/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstddef>

/** Encoded JSON frame budget, including worst-case escaping of 1 MiB values. */
inline constexpr size_t kMuonServiceMaxFrameBytes = 8 * 1024 * 1024;
/** Maximum bytes waiting for output on one authenticated service connection. */
inline constexpr size_t kMuonServiceMaxQueuedBytes = 32 * 1024 * 1024;
