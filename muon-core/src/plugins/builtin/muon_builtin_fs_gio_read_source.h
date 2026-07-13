/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#if !defined(_WIN32)

#include <cardio.h>

#include "plugins/builtin/muon_builtin_fs_helpers.h"

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <span>

namespace muon_internal {

/** Test probe for observable GIO readFile source operations. */
struct MuonFsReadSourceProbe {
  /** Number of metadata queries started. */
  uint64_t metadata_queries = 0;
  /** Number of input streams opened. */
  uint64_t open_calls = 0;
  /** Number of skip operations started. */
  uint64_t skip_calls = 0;
  /** Number of source bytes skipped. */
  uint64_t bytes_skipped = 0;
  /** Number of result reads started. */
  uint64_t read_calls = 0;
  /** Number of source bytes written to result buffers. */
  uint64_t bytes_read = 0;
  /** Number of asynchronous stream closes started. */
  uint64_t close_calls = 0;
};

/** Allocates the exact result buffer for a GIO readFile operation. */
using MuonFsReadBufferAllocator =
    std::function<std::span<std::byte>(size_t)>;

/**
 * Loads the complete contents of a GFile asynchronously.
 *
 * @param file File whose contents are loaded.
 * @param cancellation Cancellation signal for the GIO operation.
 * @param probe Optional test probe that observes source consumption.
 * @return Promise for the copied file contents.
 */
cardio::promise<cardio::gio::file_contents> LoadMuonFsContentsAsync(
    GFile* file,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe);

/**
 * Reads a configured range from a GFile into an allocated result buffer.
 *
 * @param file File to read.
 * @param options Requested source range.
 * @param max_bytes Configured per-operation byte limit.
 * @param allocate_result Allocates the exact shared result buffer.
 * @param cancellation Cancellation signal for all GIO operations.
 * @param probe Optional test probe that observes source operations.
 * @return Promise for the number of bytes written to the result buffer.
 */
cardio::promise<size_t> ReadMuonFsFileRangeAsync(
    GFile* file,
    MuonFsReadOptions options,
    uint64_t max_bytes,
    MuonFsReadBufferAllocator allocate_result,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe);

}  // namespace muon_internal

#endif
