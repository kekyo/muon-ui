/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#if !defined(_WIN32)

#include <cardio.h>

#include "plugins/builtin/muon_builtin_fs_helpers.h"

#include <gio/gio.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

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
  /** Largest buffer size requested by one source read. */
  uint64_t maximum_requested_read_size = 0;
  /** Largest number of source bytes retained in a text result. */
  uint64_t maximum_retained_bytes = 0;
  /** Number of asynchronous stream closes started. */
  uint64_t close_calls = 0;
};

/** Allocates the exact result buffer for a GIO readFile operation. */
using MuonFsReadBufferAllocator =
    std::function<std::span<std::byte>(size_t)>;

/**
 * Reads bounded text contents from a GFile asynchronously.
 *
 * @param file File whose contents are read.
 * @param max_bytes Maximum number of bytes retained in the result.
 * @param chunk_bytes Maximum number of bytes requested by each source read.
 * @param cancellation Cancellation signal for the GIO operation.
 * @param probe Optional test probe that observes source consumption.
 * @return Promise for the bounded file contents.
 *
 * @remarks One additional source byte may be read to distinguish an exact-size
 * result from an oversized result. That byte is never retained.
 */
cardio::promise<std::vector<std::byte>> ReadMuonFsTextContentsAsync(
    GFile* file,
    uint64_t max_bytes,
    size_t chunk_bytes,
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
