/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_store.h"

#include <gio/gio.h>
#include <array>
#include <memory>
#include <stdexcept>

namespace muon_internal {

struct NotificationGObjectDeleter {
  void operator()(void* object) const { if (object != nullptr) g_object_unref(object); }
};
template <typename T>
using NotificationGObject = std::unique_ptr<T, NotificationGObjectDeleter>;

static bool IsIoError(const cardio::gio::gio_error& error, GIOErrorEnum code) {
  return error.domain() == G_IO_ERROR && error.code() == code;
}

static cardio::promise<void> EnsureDirectory(GFile* directory) {
  auto missing_parent = false;
  try {
    co_await cardio::gio::make_directory(directory);
    co_return;
  } catch (const cardio::gio::gio_error& error) {
    if (IsIoError(error, G_IO_ERROR_EXISTS)) co_return;
    if (!IsIoError(error, G_IO_ERROR_NOT_FOUND)) throw;
    missing_parent = true;
  }
  if (missing_parent) {
    auto parent = NotificationGObject<GFile>(g_file_get_parent(directory));
    if (!parent) throw std::runtime_error("Notification state parent is unavailable");
    co_await EnsureDirectory(parent.get());
    try { co_await cardio::gio::make_directory(directory); }
    catch (const cardio::gio::gio_error& error) {
      if (!IsIoError(error, G_IO_ERROR_EXISTS)) throw;
    }
  }
}

cardio::promise<MuonNotificationSnapshot> LoadMuonNotificationSnapshot(std::string path) {
  auto file = NotificationGObject<GFile>(g_file_new_for_path(path.c_str()));
  auto input = NotificationGObject<GFileInputStream>{};
  try { input.reset(co_await cardio::gio::read(file.get())); }
  catch (const cardio::gio::gio_error& error) {
    if (IsIoError(error, G_IO_ERROR_NOT_FOUND)) co_return MuonNotificationSnapshot{};
    throw;
  }
  auto json = std::string{};
  auto buffer = std::array<std::byte, 65536>{};
  while (true) {
    const auto count = co_await cardio::gio::read(G_INPUT_STREAM(input.get()), buffer);
    if (count == 0) break;
    if (json.size() + count > 32 * 1024 * 1024)
      throw std::runtime_error("Notification snapshot is too large");
    json.append(reinterpret_cast<const char*>(buffer.data()), count);
  }
  co_await cardio::gio::close(G_INPUT_STREAM(input.get()));
  co_return ParseMuonNotificationSnapshot(json);
}

cardio::promise<void> SaveMuonNotificationSnapshot(std::string path,
                                                  MuonNotificationSnapshot state) {
  auto file = NotificationGObject<GFile>(g_file_new_for_path(path.c_str()));
  auto parent = NotificationGObject<GFile>(g_file_get_parent(file.get()));
  if (!parent) throw std::runtime_error("Notification state parent is unavailable");
  co_await EnsureDirectory(parent.get());
  const auto temporary_path = path + ".next";
  auto temporary = NotificationGObject<GFile>(g_file_new_for_path(temporary_path.c_str()));
  const auto json = SerializeMuonNotificationSnapshot(state);
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(json.data()), json.size());
  // GFile.replace_contents may fall back to in-place writes. Restrict that
  // behavior to our sibling staging file, never the durable snapshot itself.
  co_await cardio::gio::replace_contents(
      temporary.get(), bytes, nullptr, false,
      static_cast<GFileCreateFlags>(G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION));
  // Both files are local and share a parent. NO_FALLBACK_FOR_MOVE prevents a
  // copy-and-delete fallback from exposing a partly written durable snapshot.
  co_await cardio::gio::submit<void>(
      [source = temporary.get(), destination = file.get()](
          GCancellable* cancellation, GAsyncReadyCallback callback, gpointer data) {
        g_file_move_async(source, destination,
                          static_cast<GFileCopyFlags>(G_FILE_COPY_OVERWRITE |
                                                       G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                          G_PRIORITY_DEFAULT, cancellation, nullptr, nullptr,
                          callback, data);
      },
      [](GObject* source, GAsyncResult* result, GError** error) {
        if (!g_file_move_finish(G_FILE(source), result, error) && *error == nullptr)
          g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Failed to commit notification snapshot");
      });
}

cardio::promise<std::string> CreateMuonNotificationToken() {
  auto random = NotificationGObject<GFile>(g_file_new_for_path("/dev/urandom"));
  auto input = NotificationGObject<GFileInputStream>(
      co_await cardio::gio::read(random.get()));
  auto bytes = std::array<std::byte, 32>{};
  auto offset = size_t{0};
  while (offset < bytes.size()) {
    const auto count = co_await cardio::gio::read(
        G_INPUT_STREAM(input.get()), std::span<std::byte>(bytes).subspan(offset));
    if (count == 0) throw std::runtime_error("Notification random source ended unexpectedly");
    offset += count;
  }
  co_await cardio::gio::close(G_INPUT_STREAM(input.get()));
  auto token = std::string{};
  token.reserve(64);
  static constexpr char hexadecimal[] = "0123456789abcdef";
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    token += hexadecimal[value >> 4];
    token += hexadecimal[value & 15];
  }
  co_return token;
}

}  // namespace muon_internal
