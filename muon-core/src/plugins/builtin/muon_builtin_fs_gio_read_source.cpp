/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_gio_read_source.h"

#if !defined(_WIN32)

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace muon_internal {

static cardio::promise<GFileInfo*> QueryReadFileInfoAsync(
    GFile* file,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  if (probe != nullptr) {
    probe->metadata_queries += 1;
  }
  return cardio::gio::submit<GFileInfo*>(
      [file](GCancellable* cancellable,
             GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_query_info_async(
            file,
            G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
            G_FILE_QUERY_INFO_NONE,
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_query_info_finish(G_FILE(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<GFileInputStream*> OpenReadFileStreamAsync(
    GFile* file,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  if (probe != nullptr) {
    probe->open_calls += 1;
  }
  return cardio::gio::submit<GFileInputStream*>(
      [file](GCancellable* cancellable,
             GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_read_async(file,
                          G_PRIORITY_DEFAULT,
                          cancellable,
                          callback,
                          user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_read_finish(G_FILE(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<size_t> SkipReadFileStreamAsync(
    GInputStream* stream,
    size_t count,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  if (probe != nullptr) {
    probe->skip_calls += 1;
  }
  auto skipped = co_await cardio::gio::submit<size_t>(
      [stream, count](GCancellable* cancellable,
                      GAsyncReadyCallback callback,
                      gpointer user_data) {
        g_input_stream_skip_async(stream,
                                  static_cast<gsize>(count),
                                  G_PRIORITY_DEFAULT,
                                  cancellable,
                                  callback,
                                  user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto value = g_input_stream_skip_finish(
            G_INPUT_STREAM(source_object), result, error);
        if (value < 0) {
          if (error != nullptr && *error == nullptr) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "g_input_stream_skip_finish failed");
          }
          return size_t{0};
        }
        return static_cast<size_t>(value);
      },
      std::move(cancellation));
  if (probe != nullptr) {
    probe->bytes_skipped += static_cast<uint64_t>(skipped);
  }
  co_return skipped;
}

static void UnrefGFileInfo(GFileInfo* info) {
  if (info != nullptr) {
    g_object_unref(info);
  }
}

static void UnrefGFileInputStream(GFileInputStream* stream) {
  if (stream != nullptr) {
    g_object_unref(stream);
  }
}

static cardio::promise<size_t> ReadTextStreamChunkAsync(
    GInputStream* stream,
    std::span<std::byte> buffer,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  if (probe != nullptr) {
    probe->read_calls += 1;
    probe->maximum_requested_read_size =
        std::max(probe->maximum_requested_read_size,
                 static_cast<uint64_t>(buffer.size()));
  }
  const auto actual_size =
      co_await cardio::gio::read(stream, buffer, cancellation);
  if (actual_size > buffer.size()) {
    throw std::runtime_error("readTextFile source returned too many bytes");
  }
  if (probe != nullptr) {
    probe->bytes_read += static_cast<uint64_t>(actual_size);
  }
  co_return actual_size;
}

cardio::promise<std::vector<std::byte>> ReadMuonFsTextContentsAsync(
    GFile* file,
    uint64_t max_bytes,
    size_t chunk_bytes,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  if (chunk_bytes == 0) {
    throw std::runtime_error("readTextFile chunk size must be positive");
  }

  auto contents = std::vector<std::byte>{};
  const auto bounded_chunk_bytes = static_cast<size_t>(std::min<uint64_t>(
      static_cast<uint64_t>(chunk_bytes), max_bytes));
  auto chunk = std::vector<std::byte>(bounded_chunk_bytes);
  auto stream = std::unique_ptr<GFileInputStream,
                                void (*)(GFileInputStream*)>(
      co_await OpenReadFileStreamAsync(file, cancellation, probe),
      UnrefGFileInputStream);
  auto operation_error = std::exception_ptr{};
  try {
    auto reached_eof = false;
    while (static_cast<uint64_t>(contents.size()) < max_bytes) {
      cancellation.throw_if_cancellation_requested();
      const auto remaining =
          max_bytes - static_cast<uint64_t>(contents.size());
      const auto request_size = static_cast<size_t>(std::min<uint64_t>(
          static_cast<uint64_t>(chunk.size()), remaining));
      const auto actual_size = co_await ReadTextStreamChunkAsync(
          G_INPUT_STREAM(stream.get()),
          std::span<std::byte>(chunk.data(), request_size),
          cancellation,
          probe);
      cancellation.throw_if_cancellation_requested();
      if (actual_size == 0) {
        reached_eof = true;
        break;
      }
      contents.insert(
          contents.end(), chunk.begin(), chunk.begin() + actual_size);
      if (probe != nullptr) {
        probe->maximum_retained_bytes =
            std::max(probe->maximum_retained_bytes,
                     static_cast<uint64_t>(contents.size()));
      }
    }

    if (!reached_eof) {
      auto lookahead = std::byte{};
      const auto lookahead_size = co_await ReadTextStreamChunkAsync(
          G_INPUT_STREAM(stream.get()),
          std::span<std::byte>(&lookahead, size_t{1}),
          cancellation,
          probe);
      cancellation.throw_if_cancellation_requested();
      if (lookahead_size != 0) {
        throw std::runtime_error(kMuonFsReadTextFileLimitError);
      }
    }
  } catch (...) {
    operation_error = std::current_exception();
  }

  auto close_error = std::exception_ptr{};
  try {
    if (probe != nullptr) {
      probe->close_calls += 1;
    }
    co_await cardio::gio::close(G_INPUT_STREAM(stream.get()));
  } catch (...) {
    close_error = std::current_exception();
  }
  if (operation_error != nullptr) {
    std::rethrow_exception(operation_error);
  }
  if (close_error != nullptr) {
    std::rethrow_exception(close_error);
  }
  co_return contents;
}

cardio::promise<size_t> ReadMuonFsFileRangeAsync(
    GFile* file,
    MuonFsReadOptions options,
    uint64_t max_bytes,
    MuonFsReadBufferAllocator allocate_result,
    cardio::cancellation cancellation,
    MuonFsReadSourceProbe* probe) {
  auto validation_error = std::string{};
  if (!ValidateMuonFsReadFileLength(
          options, max_bytes, &validation_error)) {
    throw std::runtime_error(validation_error);
  }
  if (options.has_length && options.length == 0) {
    co_return size_t{0};
  }

  auto info = std::unique_ptr<GFileInfo, void (*)(GFileInfo*)>(
      co_await QueryReadFileInfoAsync(file, cancellation, probe),
      UnrefGFileInfo);
  if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_REGULAR) {
    throw std::runtime_error("Path is not a regular file");
  }
  const auto signed_size = g_file_info_get_size(info.get());
  const auto file_size = signed_size < 0 ? uint64_t{0}
                                         : static_cast<uint64_t>(signed_size);
  auto range = MuonFsReadRange{};
  if (!ResolveMuonFsReadFileRange(
          options, file_size, max_bytes, &range, &validation_error)) {
    throw std::runtime_error(validation_error);
  }
  if (range.length == 0) {
    co_return size_t{0};
  }
  if (range.position >
          static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
      range.position > static_cast<uint64_t>(G_MAXSSIZE) ||
      range.length >
          static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
      range.length > static_cast<uint64_t>(G_MAXSSIZE)) {
    throw std::runtime_error("Read range is too large");
  }

  auto stream = std::unique_ptr<GFileInputStream,
                                void (*)(GFileInputStream*)>(
      co_await OpenReadFileStreamAsync(file, cancellation, probe),
      UnrefGFileInputStream);
  auto actual_size = size_t{0};
  auto operation_error = std::exception_ptr{};
  try {
    if (range.position != 0) {
      if (!G_IS_SEEKABLE(stream.get())) {
        throw std::runtime_error("readFile source is not seekable");
      }
      auto* seekable = G_SEEKABLE(stream.get());
      if (!g_seekable_can_seek(seekable)) {
        throw std::runtime_error("readFile source is not seekable");
      }
      const auto skipped = co_await SkipReadFileStreamAsync(
          G_INPUT_STREAM(stream.get()),
          static_cast<size_t>(range.position),
          cancellation,
          probe);
      cancellation.throw_if_cancellation_requested();
      if (skipped < range.position) {
        actual_size = 0;
      } else {
        auto result = allocate_result(static_cast<size_t>(range.length));
        if (result.size() != range.length) {
          throw std::runtime_error("readFile result allocation size changed");
        }
        if (probe != nullptr) {
          probe->read_calls += 1;
        }
        actual_size = co_await cardio::gio::read_all(
            G_INPUT_STREAM(stream.get()), result, cancellation);
        if (probe != nullptr) {
          probe->bytes_read += static_cast<uint64_t>(actual_size);
        }
      }
    } else {
      auto result = allocate_result(static_cast<size_t>(range.length));
      if (result.size() != range.length) {
        throw std::runtime_error("readFile result allocation size changed");
      }
      if (probe != nullptr) {
        probe->read_calls += 1;
      }
      actual_size = co_await cardio::gio::read_all(
          G_INPUT_STREAM(stream.get()), result, cancellation);
      if (probe != nullptr) {
        probe->bytes_read += static_cast<uint64_t>(actual_size);
      }
    }
  } catch (...) {
    operation_error = std::current_exception();
  }

  auto close_error = std::exception_ptr{};
  try {
    if (probe != nullptr) {
      probe->close_calls += 1;
    }
    co_await cardio::gio::close(G_INPUT_STREAM(stream.get()));
  } catch (...) {
    close_error = std::current_exception();
  }
  if (operation_error != nullptr) {
    std::rethrow_exception(operation_error);
  }
  if (close_error != nullptr) {
    std::rethrow_exception(close_error);
  }
  co_return actual_size;
}

}  // namespace muon_internal

#endif
