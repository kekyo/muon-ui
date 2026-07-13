/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_gio_read_source.h"

#if !defined(_WIN32)

#include <cstring>
#include <utility>

namespace muon_internal {

cardio::promise<cardio::gio::file_contents> LoadMuonFsContentsAsync(
    GFile* file,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  if (probe != nullptr) {
    probe->read_calls += 1;
  }
  return cardio::gio::submit<cardio::gio::file_contents>(
      [file](GCancellable* cancellable,
             GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_load_contents_async(file, cancellable, callback, user_data);
      },
      [probe](GObject* source_object,
              GAsyncResult* result,
              GError** error) {
        auto* contents = static_cast<char*>(nullptr);
        auto length = gsize{};
        auto* etag = static_cast<char*>(nullptr);
        auto loaded = cardio::gio::file_contents{};
        const auto succeeded = g_file_load_contents_finish(
            G_FILE(source_object), result, &contents, &length, &etag, error);
        if (!succeeded) {
          if (error != nullptr && *error == nullptr) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "g_file_load_contents_finish failed");
          }
          g_free(contents);
          g_free(etag);
          return loaded;
        }
        if (probe != nullptr) {
          probe->bytes_read += static_cast<uint64_t>(length);
        }
        loaded.bytes.resize(static_cast<size_t>(length));
        if (length != 0) {
          std::memcpy(loaded.bytes.data(), contents,
                      static_cast<size_t>(length));
        }
        if (etag != nullptr) {
          loaded.etag = etag;
        }
        g_free(contents);
        g_free(etag);
        return loaded;
      },
      std::move(cancellation));
}

}  // namespace muon_internal

#endif
