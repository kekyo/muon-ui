/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_policy.h"
#include "muon_filesystem.h"

#include <windows.h>
#include <objbase.h>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

static void CheckPolicy(bool success, const char* operation) {
  if (!success) throw std::system_error(GetLastError(), std::system_category(), operation);
}

struct PolicyMetadataTask {
  std::function<void()> work;
  cardio::promise_source<std::string> completed;
};

static void CALLBACK CompletePolicyMetadata(PTP_CALLBACK_INSTANCE, void* context) {
  auto task = std::unique_ptr<PolicyMetadataTask>(static_cast<PolicyMetadataTask*>(context));
  auto error = std::string{};
  try { task->work(); }
  catch (const std::exception& exception) { error = exception.what(); }
  catch (...) { error = "Service preference metadata operation failed"; }
  task->completed.resolve(std::move(error));
}

static cardio::promise<void> Metadata(std::function<void()> work) {
  auto task = std::make_unique<PolicyMetadataTask>();
  task->work = std::move(work);
  auto result = task->completed.get_promise();
  // Win32 has no asynchronous open/close/flush/rename. Use the OS default pool
  // for those calls; content I/O uses cardio OVERLAPPED operations below.
  CheckPolicy(TrySubmitThreadpoolCallback(&CompletePolicyMetadata, task.get(), nullptr) != 0,
      "submit service preference metadata");
  task.release();
  const auto error = co_await result;
  if (!error.empty()) throw std::runtime_error(error);
}

struct PolicyFile {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~PolicyFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

static cardio::promise<void> Close(std::shared_ptr<PolicyFile> file) {
  if (file->handle == INVALID_HANDLE_VALUE) co_return;
  co_await Metadata([file] {
    CheckPolicy(CloseHandle(std::exchange(file->handle, INVALID_HANDLE_VALUE)) != 0,
        "close service preference");
  });
}

cardio::promise<std::optional<bool>> LoadMuonServiceAutostart(std::filesystem::path path) {
  const auto native = muon_internal::FilesystemPathForIo(path, muon_internal::FilesystemIoUsage::File);
  auto file = std::make_shared<PolicyFile>();
  co_await Metadata([file, native] {
    file->handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        CheckPolicy(false, "open service preference");
    }
  });
  if (file->handle == INVALID_HANDLE_VALUE) co_return std::nullopt;
  auto bytes = std::array<std::byte, 2>{};
  auto length = size_t{0};
  auto failure = std::exception_ptr{};
  try {
    while (length < bytes.size()) {
      auto count = size_t{0};
      try { count = co_await cardio::win32::read(file->handle, std::span(bytes).subspan(length), length); }
      catch (const std::system_error& error) { if (error.code().value() != ERROR_HANDLE_EOF) throw; }
      if (count == 0) break;
      length += count;
    }
  } catch (...) { failure = std::current_exception(); }
  co_await Close(file);
  if (failure) std::rethrow_exception(failure);
  if (length != 1 || (bytes[0] != std::byte{'0'} && bytes[0] != std::byte{'1'}))
    throw std::runtime_error("Invalid service autostart preference");
  co_return bytes[0] == std::byte{'1'};
}

cardio::promise<void> SaveMuonServiceAutostart(std::filesystem::path path, bool enabled) {
  const auto native = muon_internal::FilesystemPathForIo(path, muon_internal::FilesystemIoUsage::File);
  GUID identity{};
  if (FAILED(CoCreateGuid(&identity))) throw std::runtime_error("Cannot create service preference staging identity");
  auto staging = native.native() + L".next-";
  for (const auto byte : std::as_bytes(std::span(&identity, 1))) {
    const auto value = std::to_integer<unsigned>(byte);
    staging += L"0123456789abcdef"[value >> 4];
    staging += L"0123456789abcdef"[value & 15];
  }
  auto file = std::make_shared<PolicyFile>();
  co_await Metadata([file, staging] {
    file->handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH, nullptr);
    CheckPolicy(file->handle != INVALID_HANDLE_VALUE, "create service preference staging file");
  });
  auto failure = std::exception_ptr{};
  try {
    const auto byte = enabled ? std::byte{'1'} : std::byte{'0'};
    const auto size = co_await cardio::win32::write(file->handle, {&byte, 1});
    if (size != 1) throw std::runtime_error("Service preference write made no progress");
    co_await Metadata([file] { CheckPolicy(FlushFileBuffers(file->handle) != 0, "flush service preference"); });
  } catch (...) { failure = std::current_exception(); }
  try { co_await Close(file); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  if (!failure) {
    try {
      co_await Metadata([staging, native] {
        CheckPolicy(MoveFileExW(staging.c_str(), native.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "commit service preference");
      });
    } catch (...) { failure = std::current_exception(); }
  }
  if (failure) {
    try { co_await Metadata([staging] { DeleteFileW(staging.c_str()); }); }
    catch (...) { /* Preserve the failed commit, not an orphan-cleanup diagnostic. */ }
    std::rethrow_exception(failure);
  }
}
