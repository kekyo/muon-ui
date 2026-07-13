/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#if !defined(_WIN32)

#include <cardio.h>

#include <gio/gio.h>

#include <cstdint>

namespace muon_internal {

/** Test probe for observable GIO readFile source operations. */
struct MuonFsReadSourceProbe {
  uint64_t metadata_queries = 0;
  uint64_t open_calls = 0;
  uint64_t skip_calls = 0;
  uint64_t bytes_skipped = 0;
  uint64_t read_calls = 0;
  uint64_t bytes_read = 0;
  uint64_t close_calls = 0;
};

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

}  // namespace muon_internal

#endif
