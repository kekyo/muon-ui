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

int main() {
  return TestWholeContentsProbe() ? 0 : 1;
}
