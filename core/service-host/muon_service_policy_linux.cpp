/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_policy.h"

#include <array>
#include <memory>
#include <stdexcept>

struct PolicyObjectDeleter {
  void operator()(void* value) const { if (value) g_object_unref(value); }
};
template <typename T>
using PolicyObject = std::unique_ptr<T, PolicyObjectDeleter>;

static void RunMetadataTask(GTask* task, gpointer, gpointer data, GCancellable*) {
  const auto& work = *static_cast<std::function<void()>*>(data);
  try { work(); g_task_return_boolean(task, TRUE); }
  catch (const std::exception& error) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.what());
  } catch (...) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Service metadata operation failed");
  }
}

cardio::promise<void> RunMuonServiceFileWork(std::function<void()> work) {
  // Return the GIO promise directly: no coroutine frame needs to own a
  // temporary closure, which GCC 12 can destroy twice in a co_await expression.
  return cardio::gio::submit<void>(
      [work = std::move(work)](GCancellable* cancellation, GAsyncReadyCallback callback, gpointer data) {
        auto task = PolicyObject<GTask>(g_task_new(nullptr, cancellation, callback, data));
        g_task_set_task_data(task.get(), new std::function<void()>(work),
            [](gpointer value) { delete static_cast<std::function<void()>*>(value); });
        g_task_run_in_thread(task.get(), RunMetadataTask);
      }, [](GObject*, GAsyncResult* result, GError** error) {
        g_task_propagate_boolean(G_TASK(result), error);
      });
}

cardio::promise<std::optional<bool>> LoadMuonServiceAutostart(std::filesystem::path path) {
  auto file = PolicyObject<GFile>(g_file_new_for_path(path.c_str()));
  auto input = PolicyObject<GFileInputStream>{};
  try { input.reset(co_await cardio::gio::read(file.get())); }
  catch (const cardio::gio::gio_error& error) {
    if (error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_NOT_FOUND) co_return std::nullopt;
    throw;
  }
  auto bytes = std::array<std::byte, 2>{};
  auto length = size_t{0};
  auto failure = std::exception_ptr{};
  try {
    while (length < bytes.size()) {
      const auto count = co_await cardio::gio::read(G_INPUT_STREAM(input.get()), std::span(bytes).subspan(length));
      if (count == 0) break;
      length += count;
    }
  } catch (...) { failure = std::current_exception(); }
  co_await cardio::gio::close(G_INPUT_STREAM(input.get()));
  if (failure) std::rethrow_exception(failure);
  if (length != 1 || (bytes[0] != std::byte{'0'} && bytes[0] != std::byte{'1'}))
    throw std::runtime_error("Invalid service autostart preference: " + path.string());
  co_return bytes[0] == std::byte{'1'};
}

cardio::promise<void> SaveMuonServiceAutostart(std::filesystem::path path, bool enabled) {
  auto token = std::unique_ptr<char, decltype(&g_free)>(g_uuid_string_random(), &g_free);
  const auto staging_path = path.string() + ".next-" + token.get();
  auto file = PolicyObject<GFile>(g_file_new_for_path(path.c_str()));
  auto staging = PolicyObject<GFile>(g_file_new_for_path(staging_path.c_str()));
  auto failure = std::exception_ptr{};
  try {
    const auto byte = enabled ? std::byte{'1'} : std::byte{'0'};
    // GIO can fall back to in-place replacement. Restrict it to the staging
    // file and commit with a local rename that cannot fall back to copying.
    co_await cardio::gio::replace_contents(staging.get(), {&byte, 1}, nullptr, false,
        static_cast<GFileCreateFlags>(G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION));
    co_await cardio::gio::submit<void>(
        [source = staging.get(), destination = file.get()](GCancellable* cancellation,
            GAsyncReadyCallback callback, gpointer data) {
          g_file_move_async(source, destination,
              static_cast<GFileCopyFlags>(G_FILE_COPY_OVERWRITE | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
              G_PRIORITY_DEFAULT, cancellation, nullptr, nullptr, callback, data);
        },
        [](GObject* source, GAsyncResult* result, GError** error) {
          if (!g_file_move_finish(G_FILE(source), result, error) && !*error)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot commit service preference");
        });
  } catch (...) { failure = std::current_exception(); }
  if (failure) {
    try { co_await cardio::gio::delete_file(staging.get()); }
    catch (...) { /* Preserve the original failure; an orphan cannot change policy. */ }
    std::rethrow_exception(failure);
  }
}
