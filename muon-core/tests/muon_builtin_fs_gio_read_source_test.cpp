/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_gio_read_source.h"

#include <cardio.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

template <typename Operation>
static auto RunGioOperation(Operation operation) -> bool {
  auto* context = g_main_context_new();
  auto group = cardio::dispatcher_group_glib(context);
  g_main_context_unref(context);
  auto host = cardio::dispatcher_host_glib_auto(group);
  auto* loop = g_main_loop_new(group.context(), FALSE);
  auto promise = std::optional<cardio::promise<bool>>{};

  struct LoopState {
    std::optional<cardio::promise<bool>>* promise;
    GMainLoop* loop;
  } state{&promise, loop};

  auto* source = g_timeout_source_new(1);
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        auto* current = static_cast<LoopState*>(data);
        if (current->promise->has_value() &&
            (*current->promise)->is_ready()) {
          g_main_loop_quit(current->loop);
          return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
      },
      &state,
      nullptr);
  g_source_attach(source, group.context());
  g_source_unref(source);

  cardio::set_current_dispatcher(nullptr);
  cardio::internal::dangerous_schedule_later__(&host, [&] {
    promise.emplace(operation());
  });
  g_main_loop_run(loop);

  auto succeeded = false;
  if (promise.has_value() && promise->is_ready()) {
    try {
      succeeded = promise->unsafe_result();
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
    }
  }
  g_main_loop_unref(loop);
  return succeeded;
}

static auto TestWholeContentsProbe() -> bool {
  auto* directory = g_dir_make_tmp("muon-fs-read-source-test-XXXXXX", nullptr);
  if (!Expect(directory != nullptr, "failed to create temporary directory")) {
    return false;
  }
  auto* path = g_build_filename(directory, "source.bin", nullptr);
  constexpr auto contents = std::string_view{"0123456789"};
  auto error = static_cast<GError*>(nullptr);
  const auto wrote = g_file_set_contents(
      path, contents.data(), static_cast<gssize>(contents.size()), &error);
  if (!wrote) {
    std::cerr << (error == nullptr ? "failed to write source" : error->message)
              << "\n";
    g_clear_error(&error);
    g_free(path);
    g_rmdir(directory);
    g_free(directory);
    return false;
  }

  auto* file = g_file_new_for_path(path);
  auto probe = muon_internal::MuonFsReadSourceProbe{};
  const auto succeeded = RunGioOperation([file, &probe]()
      -> cardio::promise<bool> {
    auto loaded = co_await muon_internal::LoadMuonFsContentsAsync(
        file, cardio::cancellation{}, &probe);
    const auto expected = std::string_view{"0123456789"};
    const auto actual = std::string(
        reinterpret_cast<const char*>(loaded.bytes.data()),
        loaded.bytes.size());
    co_return Expect(actual == expected, "loaded contents changed") &&
              Expect(probe.read_calls == 1, "whole-content read was not observed") &&
              Expect(probe.bytes_read == expected.size(),
                     "whole-content byte count changed");
  });

  g_object_unref(file);
  g_remove(path);
  g_free(path);
  g_rmdir(directory);
  g_free(directory);
  return succeeded;
}

static bool TestRangedReadContract() {
  auto* directory = g_dir_make_tmp("muon-fs-range-source-test-XXXXXX", nullptr);
  if (!Expect(directory != nullptr, "failed to create range test directory")) {
    return false;
  }
  auto* path = g_build_filename(directory, "source.bin", nullptr);
  auto* missing_path = g_build_filename(directory, "missing.bin", nullptr);
  constexpr auto contents = std::string_view{"0123456789abcdefg"};
  auto error = static_cast<GError*>(nullptr);
  const auto wrote = g_file_set_contents(
      path, contents.data(), static_cast<gssize>(contents.size()), &error);
  if (!wrote) {
    std::cerr << (error == nullptr ? "failed to write range source"
                                   : error->message)
              << "\n";
    g_clear_error(&error);
    g_free(missing_path);
    g_free(path);
    g_rmdir(directory);
    g_free(directory);
    return false;
  }

  auto* file = g_file_new_for_path(path);
  auto* missing_file = g_file_new_for_path(missing_path);
  const auto succeeded = RunGioOperation([file, missing_file, contents]()
      -> cardio::promise<bool> {
    auto succeeded = true;
    auto probe = muon_internal::MuonFsReadSourceProbe{};
    auto storage = std::vector<std::byte>{};
    auto allocation_calls = uint64_t{0};
    auto allocate = [&](size_t size) -> std::span<std::byte> {
      allocation_calls += 1;
      storage.resize(size);
      return storage;
    };

    auto options = muon_internal::MuonFsReadOptions{};
    options.has_position = true;
    options.position = 4;
    options.has_length = true;
    options.length = 3;
    const auto actual_size =
        co_await muon_internal::ReadMuonFsFileRangeAsync(
            file,
            options,
            uint64_t{16},
            allocate,
            cardio::cancellation{},
            &probe);
    const auto actual = std::string(
        reinterpret_cast<const char*>(storage.data()), actual_size);
    succeeded = Expect(actual == "456", "ranged read result changed") &&
                succeeded;
    succeeded = Expect(probe.metadata_queries == 1,
                       "ranged read metadata query count changed") &&
                succeeded;
    succeeded = Expect(probe.open_calls == 1,
                       "ranged read open count changed") &&
                succeeded;
    succeeded = Expect(probe.skip_calls == 1 && probe.bytes_skipped == 4,
                       "ranged read skip changed") &&
                succeeded;
    succeeded = Expect(probe.read_calls == 1 && probe.bytes_read == 3,
                       "ranged read consumed bytes outside the requested range") &&
                succeeded;
    succeeded = Expect(probe.close_calls == 1,
                       "ranged read close count changed") &&
                succeeded;
    succeeded = Expect(allocation_calls == 1 && storage.size() == 3,
                       "ranged read result allocation changed") &&
                succeeded;

    probe = {};
    storage.clear();
    allocation_calls = 0;
    options = {};
    options.has_length = true;
    options.length = 0;
    auto zero_error = std::string{};
    try {
      const auto zero_size =
          co_await muon_internal::ReadMuonFsFileRangeAsync(
              missing_file,
              options,
              uint64_t{16},
              allocate,
              cardio::cancellation{},
              &probe);
      succeeded = Expect(zero_size == 0, "zero-length read returned data") &&
                  succeeded;
    } catch (const std::exception& exception) {
      zero_error = exception.what();
    }
    succeeded = Expect(zero_error.empty(),
                       "zero-length read accessed its source") &&
                succeeded;
    succeeded = Expect(probe.metadata_queries == 0 && probe.open_calls == 0 &&
                           probe.read_calls == 0 && probe.close_calls == 0 &&
                           allocation_calls == 0,
                       "zero-length read performed I/O or allocation") &&
                succeeded;

    probe = {};
    storage.clear();
    allocation_calls = 0;
    options.length = 17;
    auto explicit_error = std::string{};
    try {
      (void)co_await muon_internal::ReadMuonFsFileRangeAsync(
          missing_file,
          options,
          uint64_t{16},
          allocate,
          cardio::cancellation{},
          &probe);
    } catch (const std::exception& exception) {
      explicit_error = exception.what();
    }
    succeeded = Expect(
                    explicit_error ==
                        "readFile length exceeds configured maximum",
                    "explicit maximum error or precedence changed") &&
                succeeded;
    succeeded = Expect(probe.metadata_queries == 0 && probe.open_calls == 0 &&
                           probe.read_calls == 0 && allocation_calls == 0,
                       "explicit oversized read touched its source") &&
                succeeded;

    probe = {};
    storage.clear();
    allocation_calls = 0;
    options = {};
    auto implicit_error = std::string{};
    try {
      (void)co_await muon_internal::ReadMuonFsFileRangeAsync(
          file,
          options,
          uint64_t{16},
          allocate,
          cardio::cancellation{},
          &probe);
    } catch (const std::exception& exception) {
      implicit_error = exception.what();
    }
    succeeded = Expect(
                    implicit_error ==
                        "readFile result exceeds configured maximum",
                    "implicit oversized result was truncated or accepted") &&
                succeeded;
    succeeded = Expect(probe.metadata_queries == 1 && probe.open_calls == 0 &&
                           probe.read_calls == 0 && allocation_calls == 0,
                       "implicit oversized result started a data read") &&
                succeeded;

    probe = {};
    storage.clear();
    allocation_calls = 0;
    options.has_position = true;
    options.position = contents.size();
    const auto eof_size = co_await muon_internal::ReadMuonFsFileRangeAsync(
        file,
        options,
        uint64_t{16},
        allocate,
        cardio::cancellation{},
        &probe);
    succeeded = Expect(eof_size == 0, "EOF range returned data") && succeeded;
    succeeded = Expect(probe.metadata_queries == 1 && probe.open_calls == 0 &&
                           probe.read_calls == 0 && allocation_calls == 0,
                       "EOF range opened or read its source") &&
                succeeded;
    probe = {};
    options.position = contents.size() + 1;
    const auto beyond_eof_size =
        co_await muon_internal::ReadMuonFsFileRangeAsync(
            file,
            options,
            uint64_t{16},
            allocate,
            cardio::cancellation{},
            &probe);
    succeeded = Expect(beyond_eof_size == 0,
                       "range beyond EOF returned data") &&
                succeeded;
    succeeded = Expect(probe.metadata_queries == 1 && probe.open_calls == 0 &&
                           probe.read_calls == 0 && allocation_calls == 0,
                       "range beyond EOF opened or read its source") &&
                succeeded;

    probe = {};
    storage.clear();
    allocation_calls = 0;
    options.position = 1;
    options.has_length = true;
    options.length = 16;
    const auto tail_size = co_await muon_internal::ReadMuonFsFileRangeAsync(
        file,
        options,
        uint64_t{16},
        allocate,
        cardio::cancellation{},
        &probe);
    const auto tail = std::string(
        reinterpret_cast<const char*>(storage.data()), tail_size);
    succeeded = Expect(tail == "123456789abcdefg",
                       "bounded tail range changed") &&
                succeeded;
    succeeded = Expect(probe.bytes_read == 16,
                       "bounded tail range consumed extra bytes") &&
                succeeded;

    probe = {};
    storage.clear();
    allocation_calls = 0;
    options = {};
    options.has_length = true;
    options.length = 0;
    zero_error.clear();
    try {
      const auto zero_max_size =
          co_await muon_internal::ReadMuonFsFileRangeAsync(
              missing_file,
              options,
              uint64_t{0},
              allocate,
              cardio::cancellation{},
              &probe);
      succeeded = Expect(zero_max_size == 0,
                         "zero maximum empty range returned data") &&
                  succeeded;
    } catch (const std::exception& exception) {
      zero_error = exception.what();
    }
    succeeded = Expect(zero_error.empty(),
                       "zero maximum rejected an empty range") &&
                succeeded;
    co_return succeeded;
  });

  g_object_unref(missing_file);
  g_object_unref(file);
  g_remove(path);
  g_free(missing_path);
  g_free(path);
  g_rmdir(directory);
  g_free(directory);
  return succeeded;
}

int main() {
  return TestWholeContentsProbe() && TestRangedReadContract() ? 0 : 1;
}
