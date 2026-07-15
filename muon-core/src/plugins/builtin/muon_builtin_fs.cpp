/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs.h"

#include "muon_json_helpers.h"
#include "muon_cardio_post.h"
#include "log/muon_close_debug_log.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "plugins/builtin/muon_builtin_fs_gio_read_source.h"
#include "plugins/builtin/muon_builtin_fs_helpers.h"
#include "plugins/builtin/muon_builtin_fs_watch_registry.h"
#include "plugins/muon_traffic_cardio_operation.h"

#include <cardio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <gio/gio.h>
#include <sys/stat.h>
#endif

namespace muon_internal {

static constexpr char kMuonBuiltinFsShutdownError[] =
    "muon filesystem runtime is shutting down";
static constexpr char kMuonBuiltinFsUnavailableError[] =
    "muon filesystem runtime is unavailable";
static constexpr char kMuonBuiltinFsEncodingError[] =
    "Only utf8 encoding is supported";
static constexpr char kMuonBuiltinFsGenericError[] =
    "Filesystem operation failed";
static constexpr char kMuonBuiltinFsAbortError[] =
    "The operation was aborted";
static constexpr char kMuonBuiltinFsWatchLeaseUnavailableError[] =
    "Filesystem watcher lease is unavailable";

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static std::atomic<uint64_t> g_muon_fs_operation_sequence{0};

static void AppendMuonFsDiagnosticLog(uint64_t operation_id,
                                      std::string message) {
  AppendMuonCloseDebugLog(
      "MuonFs op=" + std::to_string(operation_id) + " " + std::move(message));
}

static const muon_type_descriptor type_u32 = {
    MUON_TYPE_U32,
    nullptr,
};

static const muon_type_descriptor type_buffer_view = {
    MUON_TYPE_BUFFER_VIEW,
    nullptr,
};

static const muon_function_signature cancel_operation_signature = {
    0,
    nullptr,
    &type_void,
};

static const muon_type_descriptor type_cancel_operation_function = {
    MUON_TYPE_FUNCTION,
    &cancel_operation_signature,
};

static const muon_type_descriptor abort_watcher_args[] = {
    type_cancel_operation_function,
};

static const muon_function_signature abort_watcher_signature = {
    1,
    abort_watcher_args,
    &type_void,
};

static const muon_type_descriptor type_abort_watcher_function = {
    MUON_TYPE_FUNCTION,
    &abort_watcher_signature,
};

struct MuonFsBufferResult {
  MuonFsBufferResult() = default;

  MuonFsBufferResult(const muon_plugin_helpers* source_helpers,
                     muon_buffer_view source_view,
                     muon_shared_buffer_handle source_handle)
      : helpers(source_helpers),
        view(source_view),
        handle(source_handle) {}

  explicit MuonFsBufferResult(std::string error_message)
      : error(std::move(error_message)) {}

  ~MuonFsBufferResult() {
    Release();
  }

  MuonFsBufferResult(const MuonFsBufferResult&) = delete;
  MuonFsBufferResult& operator=(const MuonFsBufferResult&) = delete;

  MuonFsBufferResult(MuonFsBufferResult&& other) noexcept {
    MoveFrom(other);
  }

  MuonFsBufferResult& operator=(MuonFsBufferResult&& other) noexcept {
    if (this != &other) {
      Release();
      MoveFrom(other);
    }
    return *this;
  }

  std::span<std::byte> Bytes() {
    return std::span<std::byte>(
        static_cast<std::byte*>(view.data),
        static_cast<size_t>(view.size));
  }

  void Resize(size_t size) {
    view.size = static_cast<uintptr_t>(size);
  }

  void Complete(muon_completion_func completion) {
    if (completion == nullptr) {
      return;
    }
    if (!error.empty()) {
      CompleteMuonError(completion, error);
      Release();
      return;
    }
    completion(&view, nullptr);
    handle = nullptr;
    view = {nullptr, 0};
  }

  void Release() {
    if (helpers == nullptr || helpers->release_shared_buffer == nullptr ||
        handle == nullptr) {
      return;
    }
    helpers->release_shared_buffer(handle);
    handle = nullptr;
    view = {nullptr, 0};
  }

  void MoveFrom(MuonFsBufferResult& other) noexcept {
    helpers = other.helpers;
    view = other.view;
    handle = other.handle;
    error = std::move(other.error);
    other.helpers = nullptr;
    other.view = {nullptr, 0};
    other.handle = nullptr;
    other.error.clear();
  }

  const muon_plugin_helpers* helpers = nullptr;
  muon_buffer_view view = {nullptr, 0};
  muon_shared_buffer_handle handle = nullptr;
  std::string error;
};

static MuonFsBufferResult AllocateResultBuffer(
    const muon_plugin_helpers* helpers,
    size_t size) {
  if (size == 0) {
    return MuonFsBufferResult(helpers, {nullptr, 0}, nullptr);
  }
  if (helpers == nullptr || helpers->allocate_shared_buffer == nullptr) {
    throw std::runtime_error("Shared buffer helper is unavailable");
  }

  auto view = muon_buffer_view{nullptr, 0};
  auto handle = muon_shared_buffer_handle{};
  muon_error_buffer error = {};
  char error_storage[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = "";
  error.message = error_storage;
  error.message_capacity = static_cast<uint32_t>(sizeof(error_storage));
  if (!helpers->allocate_shared_buffer(
          static_cast<uintptr_t>(size), &view, &handle, &error)) {
    throw std::runtime_error(error_storage[0] == '\0'
                                 ? "Failed to allocate read buffer"
                                 : error_storage);
  }
  return MuonFsBufferResult(helpers, view, handle);
}

struct MuonFsStringResult {
  std::string value;
  std::string error;
};

enum class MuonFsWatchRpcOperation {
  kAcquire,
  kRelease,
  kSnapshot,
};

struct MuonFsWatchRpcRequest {
  MuonFsWatchRpcOperation operation = MuonFsWatchRpcOperation::kAcquire;
  std::string path;
  std::string token;
};

#if defined(_WIN32)
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)
#endif

enum class MuonFsOpenMode {
  Read,
  WriteTruncate,
  WriteAt,
};

struct MuonFsIoContext;

static void CloseWindowsFileSync(HANDLE& file) {
  if (file != INVALID_HANDLE_VALUE) {
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
  }
}

struct MuonFsFile {
  MuonFsFile() = default;

  explicit MuonFsFile(
      HANDLE source_handle,
      std::shared_ptr<MuonFsIoContext> source_context = nullptr)
      : handle(source_handle),
        io_context(std::move(source_context)) {}

  ~MuonFsFile() {
    CloseWindowsFileSync(handle);
  }

  MuonFsFile(const MuonFsFile&) = delete;
  MuonFsFile& operator=(const MuonFsFile&) = delete;

  MuonFsFile(MuonFsFile&& other) noexcept
      : handle(other.Release()),
        io_context(std::move(other.io_context)) {}

  MuonFsFile& operator=(MuonFsFile&& other) noexcept {
    if (this != &other) {
      CloseWindowsFileSync(handle);
      handle = other.Release();
      io_context = std::move(other.io_context);
    }
    return *this;
  }

  bool IsOpen() const {
    return handle != INVALID_HANDLE_VALUE;
  }

  HANDLE Release() {
    const auto result = handle;
    handle = INVALID_HANDLE_VALUE;
    return result;
  }

  HANDLE handle = INVALID_HANDLE_VALUE;
  std::shared_ptr<MuonFsIoContext> io_context;
};

struct MuonFsSymbolicLinkReparseData {
  ULONG reparse_tag;
  USHORT reparse_data_length;
  USHORT reserved;
  USHORT substitute_name_offset;
  USHORT substitute_name_length;
  USHORT print_name_offset;
  USHORT print_name_length;
  ULONG flags;
  WCHAR path_buffer[1];
};

struct MuonFsMountPointReparseData {
  ULONG reparse_tag;
  USHORT reparse_data_length;
  USHORT reserved;
  USHORT substitute_name_offset;
  USHORT substitute_name_length;
  USHORT print_name_offset;
  USHORT print_name_length;
  WCHAR path_buffer[1];
};

#endif

struct MuonFsIoContext {
#if defined(_WIN32)
  cardio::io_completion_port iocp;
#endif
};

#if defined(_WIN32)

static cardio::promise<void> CloseFileAsync(MuonFsIoContext& context,
                                            MuonFsFile& file);

template <typename Result>
struct MuonWindowsBlockingOutcome {
  std::optional<Result> value;
  std::string error;
};

static std::string CaptureWindowsBlockingErrorMessage() {
  try {
    throw;
  } catch (const std::exception& error) {
    const auto* message = error.what();
    return message == nullptr || message[0] == '\0'
               ? kMuonBuiltinFsGenericError
               : message;
  } catch (...) {
    return kMuonBuiltinFsGenericError;
  }
}

template <typename Task>
static auto RunWindowsBlockingFsAsync(Task&& task)
    -> cardio::promise<std::invoke_result_t<std::decay_t<Task>&>> {
  using TaskStorage = std::decay_t<Task>;
  using Result = std::invoke_result_t<TaskStorage&>;

  if constexpr (std::is_void_v<Result>) {
    auto worker = cardio::promises::start_new(
        [task = TaskStorage(std::forward<Task>(task))]() mutable {
          try {
            std::invoke(task);
            return std::string{};
          } catch (...) {
            return CaptureWindowsBlockingErrorMessage();
          }
        });
    auto error = co_await worker;
    if (!error.empty()) {
      throw std::runtime_error(std::move(error));
    }
    co_return;
  } else {
    auto worker = cardio::promises::start_new(
        [task = TaskStorage(std::forward<Task>(task))]() mutable {
          auto outcome = MuonWindowsBlockingOutcome<Result>{};
          try {
            outcome.value.emplace(std::invoke(task));
          } catch (...) {
            outcome.error = CaptureWindowsBlockingErrorMessage();
          }
          return outcome;
        });
    auto outcome = std::move(co_await worker);
    if (!outcome.error.empty()) {
      throw std::runtime_error(std::move(outcome.error));
    }
    if (!outcome.value) {
      throw std::runtime_error(
          "Windows blocking filesystem operation returned no value");
    }
    co_return std::move(*outcome.value);
  }
}

static size_t ClampNativeIoSize(size_t size) {
  return size > static_cast<size_t>(std::numeric_limits<unsigned>::max())
             ? static_cast<size_t>(std::numeric_limits<unsigned>::max())
             : size;
}

static std::string WindowsErrorMessage(const char* action, DWORD error_code) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%s: Windows error %lu", action,
                static_cast<unsigned long>(error_code));
  return buffer;
}

static std::string WindowsStatPreflightError(
    const std::filesystem::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return std::string{};
  }
  const auto error_code = GetLastError();
  if (error_code == ERROR_FILE_NOT_FOUND ||
      error_code == ERROR_PATH_NOT_FOUND) {
    return "Path does not exist";
  }
  return WindowsErrorMessage("stat", error_code);
}

static std::string WindowsRegularFilePreflightError(
    const std::filesystem::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error_code = GetLastError();
    if (error_code == ERROR_FILE_NOT_FOUND ||
        error_code == ERROR_PATH_NOT_FOUND) {
      return "Path does not exist";
    }
    return WindowsErrorMessage("readFile", error_code);
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return "Path is not a regular file";
  }
  return std::string{};
}

static void ThrowWindowsFilesystemError(const char* action,
                                        const std::filesystem::path& path,
                                        DWORD error_code) {
  ThrowFilesystemError(
      action,
      path,
      std::error_code(static_cast<int>(error_code), std::system_category()));
}

static bool StartsWithWide(std::wstring_view value,
                           std::wstring_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

static std::wstring StripWindowsNtPathPrefix(std::wstring value) {
  if (StartsWithWide(value, L"\\??\\UNC\\") ||
      StartsWithWide(value, L"\\\\?\\UNC\\")) {
    return L"\\\\" + value.substr(8);
  }
  if (StartsWithWide(value, L"\\??\\") ||
      StartsWithWide(value, L"\\\\?\\")) {
    return value.substr(4);
  }
  return value;
}

static std::wstring ReadWindowsReparseName(const WCHAR* path_buffer,
                                           size_t path_buffer_offset,
                                           DWORD returned_bytes,
                                           USHORT name_offset,
                                           USHORT name_length) {
  if (name_offset % sizeof(WCHAR) != 0 ||
      name_length % sizeof(WCHAR) != 0 ||
      path_buffer_offset + static_cast<size_t>(name_offset) +
              static_cast<size_t>(name_length) >
          static_cast<size_t>(returned_bytes)) {
    throw std::runtime_error("Invalid symbolic link reparse data");
  }
  return std::wstring(
      path_buffer + name_offset / sizeof(WCHAR),
      name_length / sizeof(WCHAR));
}

static std::wstring ReadWindowsSymbolicLinkTarget(
    const std::filesystem::path& link_path) {
  const auto wide_path = link_path.wstring();
  auto file = MuonFsFile(CreateFileW(
      wide_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file.IsOpen()) {
    ThrowWindowsFilesystemError("readlink", link_path, GetLastError());
  }

  auto buffer = std::vector<std::byte>(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
  auto returned_bytes = DWORD{0};
  if (!DeviceIoControl(
          file.handle,
          FSCTL_GET_REPARSE_POINT,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &returned_bytes,
          nullptr)) {
    ThrowWindowsFilesystemError("readlink", link_path, GetLastError());
  }
  if (returned_bytes < sizeof(ULONG)) {
    throw std::runtime_error("Invalid symbolic link reparse data");
  }

  const auto reparse_tag =
      reinterpret_cast<const MuonFsSymbolicLinkReparseData*>(
          buffer.data())->reparse_tag;
  if (reparse_tag == IO_REPARSE_TAG_SYMLINK) {
    const auto* data =
        reinterpret_cast<const MuonFsSymbolicLinkReparseData*>(buffer.data());
    const auto uses_print_name = data->print_name_length != 0;
    auto target = ReadWindowsReparseName(
        data->path_buffer,
        offsetof(MuonFsSymbolicLinkReparseData, path_buffer),
        returned_bytes,
        uses_print_name ? data->print_name_offset
                        : data->substitute_name_offset,
        uses_print_name ? data->print_name_length
                        : data->substitute_name_length);
    return StripWindowsNtPathPrefix(std::move(target));
  }
  if (reparse_tag == IO_REPARSE_TAG_MOUNT_POINT) {
    const auto* data =
        reinterpret_cast<const MuonFsMountPointReparseData*>(buffer.data());
    const auto uses_print_name = data->print_name_length != 0;
    return StripWindowsNtPathPrefix(ReadWindowsReparseName(
        data->path_buffer,
        offsetof(MuonFsMountPointReparseData, path_buffer),
        returned_bytes,
        uses_print_name ? data->print_name_offset
                        : data->substitute_name_offset,
        uses_print_name ? data->print_name_length
                        : data->substitute_name_length));
  }
  throw std::runtime_error("Path is not a symbolic link");
}

static std::string CreateWindowsFilesystemErrorMessage(
    const char* action,
    const std::filesystem::path& path,
    DWORD error_code) {
  return std::string(action) + " failed for " + PathToUtf8String(path) +
         ": " + WindowsErrorMessage("Windows filesystem API failed", error_code);
}

static std::string TryCreateWindowsSymbolicLink(
    const std::filesystem::path& target,
    const std::filesystem::path& link_path,
    DWORD flags) {
  const auto target_wide = target.wstring();
  const auto link_wide = link_path.wstring();
  const auto unprivileged_flags =
      flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
  if (CreateSymbolicLinkW(
          link_wide.c_str(), target_wide.c_str(), unprivileged_flags)) {
    return {};
  }
  auto error = GetLastError();
  if (error == ERROR_INVALID_PARAMETER) {
    if (CreateSymbolicLinkW(link_wide.c_str(), target_wide.c_str(), flags)) {
      return {};
    }
    error = GetLastError();
  }
  return CreateWindowsFilesystemErrorMessage("symlink", link_path, error);
}

static void ValidateWindowsRegularFile(HANDLE file) {
  if (GetFileType(file) != FILE_TYPE_DISK) {
    throw std::runtime_error("Path is not a regular file");
  }
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!GetFileInformationByHandle(file, &info)) {
    throw std::runtime_error(
        WindowsErrorMessage("GetFileInformationByHandle failed",
                            GetLastError()));
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    throw std::runtime_error("Path is not a regular file");
  }
}

static MuonFsFile OpenRegularFileSync(std::string path,
                                      MuonFsOpenMode mode,
                                      cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const auto target_path = CreateLocalFilesystemPath(path);
  if (mode == MuonFsOpenMode::Read) {
    const auto preflight_error = WindowsRegularFilePreflightError(target_path);
    if (!preflight_error.empty()) {
      throw std::runtime_error(preflight_error);
    }
  }
  const auto wide_path = target_path.wstring();
  const auto access =
      mode == MuonFsOpenMode::Read ? GENERIC_READ : GENERIC_WRITE;
  const auto disposition = mode == MuonFsOpenMode::Read
                               ? OPEN_EXISTING
                               : (mode == MuonFsOpenMode::WriteAt
                                      ? OPEN_ALWAYS
                                      : CREATE_ALWAYS);
  auto file = CreateFileW(
      wide_path.c_str(),
      access,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      disposition,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw std::runtime_error(
        WindowsErrorMessage("CreateFileW failed", GetLastError()));
  }
  auto result = MuonFsFile(file);
  result.io_context = std::make_shared<MuonFsIoContext>();
  try {
    ValidateWindowsRegularFile(result.handle);
  } catch (...) {
    CloseWindowsFileSync(result.handle);
    throw;
  }
  cancellation.throw_if_cancellation_requested();
  return result;
}

static cardio::promise<MuonFsFile> OpenRegularFileAsync(
    MuonFsIoContext& context,
    std::string path,
    MuonFsOpenMode mode,
    cardio::cancellation cancellation) {
  (void)context;
  AppendMuonFsDiagnosticLog(
      0,
      "open_regular_file start dispatcher=" +
          FormatMuonCloseDebugPointer(cardio::unsafe_get_current_dispatcher()));
  try {
    auto file = std::move(co_await RunWindowsBlockingFsAsync(
        [path = std::move(path), mode, cancellation]() mutable {
          AppendMuonFsDiagnosticLog(
              0,
              "open_regular_file worker_start dispatcher=" +
                  FormatMuonCloseDebugPointer(
                      cardio::unsafe_get_current_dispatcher()));
          auto result = OpenRegularFileSync(std::move(path), mode, cancellation);
          AppendMuonFsDiagnosticLog(0, "open_regular_file worker_success");
          return result;
        }));
    AppendMuonFsDiagnosticLog(0, "open_regular_file complete");
    co_return std::move(file);
  } catch (const std::exception& error) {
    AppendMuonFsDiagnosticLog(
        0, std::string("open_regular_file error message=") + error.what());
    throw;
  } catch (...) {
    AppendMuonFsDiagnosticLog(0, "open_regular_file error unknown");
    throw;
  }
}

static cardio::promise<std::string> RegularFileReadPreflightAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  auto preflight = RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto target_path = CreateLocalFilesystemPath(path);
        const auto error = WindowsRegularFilePreflightError(target_path);
        cancellation.throw_if_cancellation_requested();
        return error;
      });
  co_return std::move(co_await preflight);
}

static cardio::promise<uint64_t> GetFileSizeAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  (void)context;
  auto* handle = file.handle;
  co_return co_await RunWindowsBlockingFsAsync([handle, cancellation] {
    cancellation.throw_if_cancellation_requested();
    LARGE_INTEGER file_size = {};
    if (!GetFileSizeEx(handle, &file_size) || file_size.QuadPart < 0) {
      throw std::runtime_error("File size is invalid");
    }
    cancellation.throw_if_cancellation_requested();
    return static_cast<uint64_t>(file_size.QuadPart);
  });
}

static cardio::promise<size_t> ReadAtAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    std::span<std::byte> buffer,
    uint64_t offset,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  (void)context;
  if (buffer.empty()) {
    co_return size_t{0};
  }
  if (!file.io_context) {
    throw std::runtime_error("Windows file IOCP context is unavailable");
  }
  const auto size = ClampNativeIoSize(buffer.size());
  AppendMuonFsDiagnosticLog(
      0,
      "iocp_read start handle=" +
          std::to_string(reinterpret_cast<uintptr_t>(file.handle)) +
          " offset=" + std::to_string(offset) +
          " size=" + std::to_string(size));
  try {
    const auto read_size = co_await cardio::iocps::read(
        file.io_context->iocp, file.handle, buffer.subspan(0, size), offset,
        cancellation);
    AppendMuonFsDiagnosticLog(
        0,
        "iocp_read complete handle=" +
            std::to_string(reinterpret_cast<uintptr_t>(file.handle)) +
            " offset=" + std::to_string(offset) +
            " size=" + std::to_string(size) +
            " result=" + std::to_string(read_size));
    cancellation.throw_if_cancellation_requested();
    co_return read_size;
  } catch (const std::system_error& error) {
    if (error.code().value() == static_cast<int>(ERROR_HANDLE_EOF)) {
      AppendMuonFsDiagnosticLog(
          0,
          "iocp_read eof handle=" +
              std::to_string(reinterpret_cast<uintptr_t>(file.handle)) +
              " offset=" + std::to_string(offset) +
              " size=" + std::to_string(size));
      cancellation.throw_if_cancellation_requested();
      co_return size_t{0};
    }
    AppendMuonFsDiagnosticLog(
        0,
        "iocp_read error handle=" +
            std::to_string(reinterpret_cast<uintptr_t>(file.handle)) +
            " offset=" + std::to_string(offset) +
            " size=" + std::to_string(size) + " message=" + error.what());
    throw;
  }
}

static cardio::promise<size_t> WriteAtAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    std::span<const std::byte> buffer,
    uint64_t offset,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  (void)context;
  if (buffer.empty()) {
    co_return size_t{0};
  }
  if (!file.io_context) {
    throw std::runtime_error("Windows file IOCP context is unavailable");
  }
  const auto size = ClampNativeIoSize(buffer.size());
  AppendMuonFsDiagnosticLog(
      0,
      "iocp_write start handle=" +
          std::to_string(reinterpret_cast<uintptr_t>(file.handle)) +
          " offset=" + std::to_string(offset) +
          " size=" + std::to_string(size));
  const auto written = co_await cardio::iocps::write(
      file.io_context->iocp, file.handle, buffer.subspan(0, size), offset,
      cancellation);
  AppendMuonFsDiagnosticLog(
      0,
      "iocp_write complete handle=" +
          std::to_string(reinterpret_cast<uintptr_t>(file.handle)) +
          " offset=" + std::to_string(offset) +
          " size=" + std::to_string(size) +
          " result=" + std::to_string(written));
  cancellation.throw_if_cancellation_requested();
  co_return written;
}

static cardio::promise<void> CloseFileAsync(MuonFsIoContext& context,
                                            MuonFsFile& file) {
  if (!file.IsOpen()) {
    co_return;
  }
  (void)context;
  auto handle = file.Release();
  co_await RunWindowsBlockingFsAsync([handle]() mutable {
    CloseWindowsFileSync(handle);
  });
}

template <typename Result, typename Body>
static cardio::promise<Result> WithOpenRegularFileAsync(
    MuonFsIoContext& context,
    std::string path,
    MuonFsOpenMode mode,
    cardio::cancellation cancellation,
    Body body) {
  auto open_promise = OpenRegularFileAsync(
      context, std::move(path), mode, cancellation);
  auto file = std::move(co_await open_promise);
  auto pending_exception = std::exception_ptr{};

  if constexpr (std::is_void_v<Result>) {
    try {
      auto body_promise = body(file);
      co_await body_promise;
    } catch (...) {
      pending_exception = std::current_exception();
    }

    try {
      auto close_promise = CloseFileAsync(context, file);
      co_await close_promise;
    } catch (...) {
      if (!pending_exception) {
        pending_exception = std::current_exception();
      }
    }
    if (pending_exception) {
      std::rethrow_exception(pending_exception);
    }
    co_return;
  } else {
    auto result = std::optional<Result>{};
    try {
      auto body_promise = body(file);
      result.emplace(std::move(co_await body_promise));
    } catch (...) {
      pending_exception = std::current_exception();
    }

    try {
      auto close_promise = CloseFileAsync(context, file);
      co_await close_promise;
    } catch (...) {
      if (!pending_exception) {
        pending_exception = std::current_exception();
      }
    }
    if (pending_exception) {
      std::rethrow_exception(pending_exception);
    }
    if (!result) {
      throw std::runtime_error(kMuonBuiltinFsGenericError);
    }
    co_return std::move(*result);
  }
}

static cardio::promise<size_t> ReadAllAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    std::span<std::byte> target,
    uint64_t file_offset,
    cardio::cancellation cancellation) {
  auto offset = size_t{0};
  while (offset < target.size()) {
    cancellation.throw_if_cancellation_requested();
    const auto chunk = ClampNativeIoSize(target.size() - offset);
    auto read_promise = ReadAtAsync(
        context,
        file,
        target.subspan(offset, chunk),
        file_offset + static_cast<uint64_t>(offset),
        cancellation);
    const auto read_size = co_await read_promise;
    if (read_size == 0) {
      break;
    }
    offset += read_size;
  }
  cancellation.throw_if_cancellation_requested();
  co_return offset;
}

static cardio::promise<void> WriteAllAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    std::span<const std::byte> source,
    uint64_t file_offset,
    cardio::cancellation cancellation) {
  auto offset = size_t{0};
  while (offset < source.size()) {
    cancellation.throw_if_cancellation_requested();
    const auto chunk = ClampNativeIoSize(source.size() - offset);
    auto write_promise = WriteAtAsync(
        context,
        file,
        source.subspan(offset, chunk),
        file_offset + static_cast<uint64_t>(offset),
        cancellation);
    const auto written = co_await write_promise;
    if (written == 0) {
      throw std::runtime_error("write failed: wrote zero bytes");
    }
    offset += written;
  }
  cancellation.throw_if_cancellation_requested();
}

static cardio::promise<MuonFsBufferResult> ReadBytesFromOpenFileAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    const muon_plugin_helpers* helpers,
    MuonFsReadOptions options,
    uint64_t max_bytes,
    cardio::cancellation cancellation) {
  auto size_promise = GetFileSizeAsync(context, file, cancellation);
  const auto size = co_await size_promise;
  auto range = MuonFsReadRange{};
  auto range_error = std::string{};
  if (!ResolveMuonFsReadFileRange(
          options, size, max_bytes, &range, &range_error)) {
    co_return MuonFsBufferResult(std::move(range_error));
  }
  if (range.length >
      static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    co_return MuonFsBufferResult("Read range is too large");
  }
  auto result =
      AllocateResultBuffer(helpers, static_cast<size_t>(range.length));
  auto read_promise =
      ReadAllAsync(context, file, result.Bytes(), range.position, cancellation);
  const auto actual_size = co_await read_promise;
  result.Resize(actual_size);
  co_return std::move(result);
}

static cardio::promise<MuonFsStringResult> ReadTextFromOpenFileAsync(
    MuonFsIoContext& context,
    MuonFsFile& file,
    uint64_t max_bytes,
    cardio::cancellation cancellation) {
  auto size_promise = GetFileSizeAsync(context, file, cancellation);
  const auto size = co_await size_promise;
  if (size > max_bytes) {
    co_return MuonFsStringResult{
        std::string{}, kMuonFsReadTextFileLimitError};
  }
  if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    throw std::runtime_error("File size is invalid");
  }
  auto storage = std::vector<std::byte>(static_cast<size_t>(size));
  const auto actual_size = co_await ReadAllAsync(
      context,
      file,
      std::span<std::byte>(storage.data(), storage.size()),
      0,
      cancellation);
  cancellation.throw_if_cancellation_requested();
  storage.resize(actual_size);
  if (!IsValidUtf8WithoutNul(
          reinterpret_cast<const uint8_t*>(storage.data()),
          storage.size())) {
    co_return MuonFsStringResult{
        std::string{}, "File is not valid UTF-8 text"};
  }
  co_return MuonFsStringResult{
      std::string(reinterpret_cast<const char*>(storage.data()),
                  storage.size()),
      std::string{}};
}

static cardio::promise<MuonFsBufferResult> ReadBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    const muon_plugin_helpers* helpers,
    MuonFsReadOptions options,
    uint64_t max_bytes,
    cardio::cancellation cancellation) {
  auto start_gate = cardio::resolved();
  co_await start_gate;
  try {
    AppendMuonFsDiagnosticLog(
        0,
        "read_bytes enter dispatcher=" +
            FormatMuonCloseDebugPointer(
                cardio::unsafe_get_current_dispatcher()));
    cancellation.throw_if_cancellation_requested();
    auto validation_error = std::string{};
    if (!ValidateMuonFsReadFileLength(
            options, max_bytes, &validation_error)) {
      co_return MuonFsBufferResult(validation_error);
    }
    if (options.has_length && options.length == 0) {
      co_return AllocateResultBuffer(helpers, 0);
    }
    cancellation.throw_if_cancellation_requested();
    auto preflight_promise = RegularFileReadPreflightAsync(
        context, path, cancellation);
    const auto preflight_error = co_await preflight_promise;
    if (!preflight_error.empty()) {
      co_return MuonFsBufferResult(preflight_error);
    }
    cancellation.throw_if_cancellation_requested();
    auto result_promise = WithOpenRegularFileAsync<MuonFsBufferResult>(
        context,
        std::move(path),
        MuonFsOpenMode::Read,
        cancellation,
        [&context, helpers, options, max_bytes, cancellation](MuonFsFile& file) {
          return ReadBytesFromOpenFileAsync(
              context, file, helpers, options, max_bytes, cancellation);
        });
    co_return std::move(co_await result_promise);
  } catch (const std::exception& error) {
    co_return MuonFsBufferResult(error.what());
  }
}

static cardio::promise<MuonFsStringResult> ReadTextAsync(
    MuonFsIoContext& context,
    std::string path,
    uint64_t max_bytes,
    cardio::cancellation cancellation) {
  auto start_gate = cardio::resolved();
  co_await start_gate;
  try {
    cancellation.throw_if_cancellation_requested();
    auto preflight_promise = RegularFileReadPreflightAsync(
        context, path, cancellation);
    const auto preflight_error = co_await preflight_promise;
    if (!preflight_error.empty()) {
      co_return MuonFsStringResult{std::string{}, preflight_error};
    }
    cancellation.throw_if_cancellation_requested();
    auto read_promise = WithOpenRegularFileAsync<MuonFsStringResult>(
        context,
        std::move(path),
        MuonFsOpenMode::Read,
        cancellation,
        [&context, max_bytes, cancellation](MuonFsFile& file) {
          return ReadTextFromOpenFileAsync(
              context, file, max_bytes, cancellation);
        });
    co_return co_await read_promise;
  } catch (const std::exception& error) {
    co_return MuonFsStringResult{std::string{}, error.what()};
  }
}

static cardio::promise<void> WriteBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::span<const std::byte> source,
    MuonFsWriteOptions options,
    cardio::cancellation cancellation) {
  auto write_promise = WithOpenRegularFileAsync<void>(
      context,
      std::move(path),
      options.has_position ? MuonFsOpenMode::WriteAt
                           : MuonFsOpenMode::WriteTruncate,
      cancellation,
      [&context, source, options, cancellation](MuonFsFile& file) {
        return WriteAllAsync(
            context, file, source, options.has_position ? options.position : 0,
            cancellation);
      });
  co_await write_promise;
}

static cardio::promise<void> WriteOwnedBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::vector<std::byte> source,
    MuonFsWriteOptions options,
    cardio::cancellation cancellation) {
  const auto source_view = std::span<const std::byte>(
      source.empty() ? nullptr : source.data(), source.size());
  auto write_promise =
      WriteBytesAsync(context, std::move(path), source_view, options,
                      cancellation);
  co_await write_promise;
}

static cardio::promise<void> WriteTextAsync(
    MuonFsIoContext& context,
    std::string path,
    std::string text,
    cardio::cancellation cancellation) {
  auto source = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text.data()), text.size());
  auto write_promise = WriteBytesAsync(
      context, std::move(path), source, MuonFsWriteOptions{}, cancellation);
  co_await write_promise;
}

#else

template <typename T>
class MuonGObjectPtr final {
 public:
  explicit MuonGObjectPtr(T* source = nullptr)
      : value_(source) {}

  ~MuonGObjectPtr() {
    Reset();
  }

  MuonGObjectPtr(const MuonGObjectPtr&) = delete;
  MuonGObjectPtr& operator=(const MuonGObjectPtr&) = delete;

  MuonGObjectPtr(MuonGObjectPtr&& other) noexcept
      : value_(other.value_) {
    other.value_ = nullptr;
  }

  MuonGObjectPtr& operator=(MuonGObjectPtr&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }

  T* get() const {
    return value_;
  }

  T* release() {
    auto* value = value_;
    value_ = nullptr;
    return value;
  }

  void Reset(T* next = nullptr) {
    if (value_ != nullptr) {
      g_object_unref(value_);
    }
    value_ = next;
  }

 private:
  T* value_ = nullptr;
};

struct MuonFsGFile {
  explicit MuonFsGFile(const std::string& path)
      : file(CreateGFileFromPathOrUri(path)) {
    if (file.get() == nullptr) {
      throw std::runtime_error(kMuonBuiltinFsGenericError);
    }
  }

  MuonFsGFile(const MuonFsGFile&) = delete;
  MuonFsGFile& operator=(const MuonFsGFile&) = delete;

  GFile* get() const {
    return file.get();
  }

  MuonGObjectPtr<GFile> file;
};

struct MuonFsGBytes {
  explicit MuonFsGBytes(std::span<const std::byte> source)
      : bytes(g_bytes_new(source.empty()
                              ? static_cast<const void*>("")
                              : static_cast<const void*>(source.data()),
                          static_cast<gsize>(source.size()))) {
    if (bytes == nullptr) {
      throw std::runtime_error(kMuonBuiltinFsGenericError);
    }
  }

  ~MuonFsGBytes() {
    if (bytes != nullptr) {
      g_bytes_unref(bytes);
    }
  }

  MuonFsGBytes(const MuonFsGBytes&) = delete;
  MuonFsGBytes& operator=(const MuonFsGBytes&) = delete;

  GBytes* get() const {
    return bytes;
  }

  GBytes* bytes = nullptr;
};

static constexpr char kMuonFsGFileInfoAttributes[] =
    G_FILE_ATTRIBUTE_STANDARD_NAME ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET ","
    G_FILE_ATTRIBUTE_TIME_MODIFIED ","
    G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC ","
    G_FILE_ATTRIBUTE_UNIX_MODE ","
    G_FILE_ATTRIBUTE_ACCESS_CAN_READ ","
    G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE ","
    G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE;

static constexpr int kMuonFsEnumeratorBatchSize = 64;

static bool IsGioNotFound(const cardio::gio::gio_error& error) {
  return error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_NOT_FOUND;
}

static std::string TakeGErrorMessage(GError* error, const char* fallback) {
  if (error == nullptr) {
    return fallback;
  }
  auto message = std::string(error->message == nullptr ? fallback
                                                       : error->message);
  g_error_free(error);
  return message;
}

static void ThrowIfGioFailed(gboolean succeeded,
                             GError* error,
                             const char* fallback) {
  if (succeeded) {
    return;
  }
  throw std::runtime_error(TakeGErrorMessage(error, fallback));
}

static GFileQueryInfoFlags GetQueryFlags(bool follow_symlink) {
  return follow_symlink ? G_FILE_QUERY_INFO_NONE
                        : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;
}

static cardio::promise<GFileInfo*> QueryInfoAsync(
    GFile* file,
    bool follow_symlink,
    cardio::cancellation cancellation) {
  const auto flags = GetQueryFlags(follow_symlink);
  return cardio::gio::submit<GFileInfo*>(
      [file, flags](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_query_info_async(
            file,
            kMuonFsGFileInfoAttributes,
            flags,
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

static cardio::promise<GFileEnumerator*> EnumerateChildrenAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<GFileEnumerator*>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_enumerate_children_async(
            file,
            kMuonFsGFileInfoAttributes,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_enumerate_children_finish(
            G_FILE(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<GList*> NextFilesAsync(
    GFileEnumerator* enumerator,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<GList*>(
      [enumerator](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_enumerator_next_files_async(
            enumerator,
            kMuonFsEnumeratorBatchSize,
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_enumerator_next_files_finish(
            G_FILE_ENUMERATOR(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<void> CloseEnumeratorAsync(
    GFileEnumerator* enumerator,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [enumerator](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_enumerator_close_async(
            enumerator, G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded = g_file_enumerator_close_finish(
            G_FILE_ENUMERATOR(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_file_enumerator_close_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> DeleteFileAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_delete_async(
            file, G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded =
            g_file_delete_finish(G_FILE(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error, G_IO_ERROR, G_IO_ERROR_FAILED,
              "g_file_delete_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> MakeDirectoryAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_make_directory_async(
            file, G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded =
            g_file_make_directory_finish(G_FILE(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_file_make_directory_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> MoveFileAsync(GFile* source,
                                           GFile* destination,
                                           cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [source, destination](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_move_async(
            source,
            destination,
            G_FILE_COPY_OVERWRITE,
            G_PRIORITY_DEFAULT,
            cancellable,
            nullptr,
            nullptr,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded =
            g_file_move_finish(G_FILE(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error, G_IO_ERROR, G_IO_ERROR_FAILED,
              "g_file_move_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> CopyFileContentsAsync(
    GFile* source,
    GFile* destination,
    MuonFsCopyOptions options,
    cardio::cancellation cancellation) {
  const auto flags = options.overwrite ? G_FILE_COPY_OVERWRITE
                                       : G_FILE_COPY_NONE;
  return cardio::gio::submit<void>(
      [source, destination, flags](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_copy_async(
            source,
            destination,
            flags,
            G_PRIORITY_DEFAULT,
            cancellable,
            nullptr,
            nullptr,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded =
            g_file_copy_finish(G_FILE(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error, G_IO_ERROR, G_IO_ERROR_FAILED,
              "g_file_copy_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<GFileOutputStream*> AppendToAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<GFileOutputStream*>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_append_to_async(
            file,
            G_FILE_CREATE_NONE,
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_append_to_finish(G_FILE(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<GFileIOStream*> OpenReadwriteAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<GFileIOStream*>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_open_readwrite_async(
            file, G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_open_readwrite_finish(
            G_FILE(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<GFileIOStream*> CreateReadwriteAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<GFileIOStream*>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_create_readwrite_async(
            file,
            G_FILE_CREATE_NONE,
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        return g_file_create_readwrite_finish(
            G_FILE(source_object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<void> CloseOutputStreamAsync(
    GOutputStream* stream,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [stream](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_output_stream_close_async(
            stream, G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded = g_output_stream_close_finish(
            G_OUTPUT_STREAM(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_output_stream_close_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> CloseIoStreamAsync(
    GIOStream* stream,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [stream](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_io_stream_close_async(
            stream, G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded =
            g_io_stream_close_finish(G_IO_STREAM(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_io_stream_close_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> WriteAllToOutputStreamAsync(
    GOutputStream* stream,
    std::span<const std::byte> source,
    cardio::cancellation cancellation) {
  if (source.empty()) {
    cancellation.throw_if_cancellation_requested();
    co_return;
  }
  const auto* data = source.data();
  const auto size = source.size();
  co_await cardio::gio::submit<void>(
      [stream, data, size](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_output_stream_write_all_async(
            stream,
            data,
            static_cast<gsize>(size),
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        auto bytes_written = gsize{};
        const auto succeeded = g_output_stream_write_all_finish(
            G_OUTPUT_STREAM(source_object), result, &bytes_written, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_output_stream_write_all_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> MakeSymbolicLinkAsync(
    GFile* file,
    const std::string& target,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [file, target](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_make_symbolic_link_async(
            file,
            target.c_str(),
            G_PRIORITY_DEFAULT,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        const auto succeeded = g_file_make_symbolic_link_finish(
            G_FILE(source_object), result, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_file_make_symbolic_link_finish failed");
        }
      },
      std::move(cancellation));
}

static cardio::promise<void> ReplaceContentsBytesAsync(
    GFile* file,
    GBytes* bytes,
    cardio::cancellation cancellation) {
  return cardio::gio::submit<void>(
      [file, bytes](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_replace_contents_bytes_async(
            file,
            bytes,
            nullptr,
            FALSE,
            G_FILE_CREATE_NONE,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        auto* new_etag = static_cast<char*>(nullptr);
        const auto succeeded = g_file_replace_contents_finish(
            G_FILE(source_object), result, &new_etag, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_file_replace_contents_finish failed");
        }
        g_free(new_etag);
      },
      std::move(cancellation));
}

static cardio::promise<void> ReplaceEmptyContentsAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  static constexpr char empty_contents[] = "";
  return cardio::gio::submit<void>(
      [file](
          GCancellable* cancellable,
          GAsyncReadyCallback callback,
          gpointer user_data) {
        g_file_replace_contents_async(
            file,
            empty_contents,
            0,
            nullptr,
            FALSE,
            G_FILE_CREATE_NONE,
            cancellable,
            callback,
            user_data);
      },
      [](GObject* source_object, GAsyncResult* result, GError** error) {
        auto* new_etag = static_cast<char*>(nullptr);
        const auto succeeded = g_file_replace_contents_finish(
            G_FILE(source_object), result, &new_etag, error);
        if (!succeeded && error != nullptr && *error == nullptr) {
          g_set_error_literal(
              error,
              G_IO_ERROR,
              G_IO_ERROR_FAILED,
              "g_file_replace_contents_finish failed");
        }
        g_free(new_etag);
      },
      std::move(cancellation));
}

static bool GFileInfoHasUnixMode(GFileInfo* info) {
  return g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE);
}

static mode_t GFileInfoUnixMode(GFileInfo* info) {
  return static_cast<mode_t>(
      g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE));
}

static std::string GFileTypeNameFromMode(mode_t mode) {
  if (S_ISREG(mode)) {
    return "file";
  }
  if (S_ISDIR(mode)) {
    return "directory";
  }
  if (S_ISLNK(mode)) {
    return "symlink";
  }
  if (S_ISBLK(mode)) {
    return "blockDevice";
  }
  if (S_ISCHR(mode)) {
    return "characterDevice";
  }
  if (S_ISFIFO(mode)) {
    return "fifo";
  }
  if (S_ISSOCK(mode)) {
    return "socket";
  }
  return "other";
}

static std::string GFileTypeNameFromInfo(GFileInfo* info) {
  if (GFileInfoHasUnixMode(info)) {
    return GFileTypeNameFromMode(GFileInfoUnixMode(info));
  }
  switch (g_file_info_get_file_type(info)) {
    case G_FILE_TYPE_REGULAR:
      return "file";
    case G_FILE_TYPE_DIRECTORY:
      return "directory";
    case G_FILE_TYPE_SYMBOLIC_LINK:
      return "symlink";
    default:
      return "other";
  }
}

static bool GFileInfoIsRegularFile(GFileInfo* info) {
  return GFileTypeNameFromInfo(info) == "file";
}

static bool GFileInfoIsDirectory(GFileInfo* info) {
  return GFileTypeNameFromInfo(info) == "directory";
}

static bool GFileInfoIsSymlink(GFileInfo* info) {
  return GFileTypeNameFromInfo(info) == "symlink";
}

static bool GFileInfoIsReadonly(GFileInfo* info) {
  if (GFileInfoHasUnixMode(info)) {
    constexpr auto write_permissions = S_IWUSR | S_IWGRP | S_IWOTH;
    return (GFileInfoUnixMode(info) & write_permissions) == 0;
  }
  if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)) {
    return false;
  }
  return !g_file_info_get_attribute_boolean(
      info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
}

static uint64_t GFileInfoSize(GFileInfo* info) {
  if (!GFileInfoIsRegularFile(info)) {
    return 0;
  }
  const auto size = g_file_info_get_size(info);
  return size < 0 ? 0 : static_cast<uint64_t>(size);
}

static uint64_t GFileInfoMtimeMs(GFileInfo* info) {
  const auto seconds = g_file_info_get_attribute_uint64(
      info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  const auto usec =
      g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)
          ? g_file_info_get_attribute_uint32(
                info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC)
          : uint32_t{0};
  return seconds * uint64_t{1000} + usec / uint32_t{1000};
}

static std::string CreateGFileInfoStatusJson(GFileInfo* info) {
  auto result = std::string("{\"type\":");
  AppendJsonString(&result, GFileTypeNameFromInfo(info));
  result += ",\"size\":";
  result += std::to_string(GFileInfoSize(info));
  result += ",\"mtimeMs\":";
  result += std::to_string(GFileInfoMtimeMs(info));
  result += ",\"readonly\":";
  result += GFileInfoIsReadonly(info) ? "true" : "false";
  result += "}";
  return result;
}

static std::string CreateGFileInfoDirentJson(GFileInfo* info) {
  auto result = std::string("{\"name\":");
  const auto* name = g_file_info_get_name(info);
  AppendJsonString(&result, name == nullptr ? std::string_view{}
                                            : std::string_view(name));
  result += ",\"type\":";
  AppendJsonString(&result, GFileTypeNameFromInfo(info));
  result += ",\"size\":";
  result += std::to_string(GFileInfoSize(info));
  result += ",\"mtimeMs\":";
  result += std::to_string(GFileInfoMtimeMs(info));
  result += ",\"readonly\":";
  result += GFileInfoIsReadonly(info) ? "true" : "false";
  result += "}";
  return result;
}

static cardio::promise<MuonFsBufferResult> ReadBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    const muon_plugin_helpers* helpers,
    MuonFsReadOptions options,
    uint64_t max_bytes,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto validation_error = std::string{};
  if (!ValidateMuonFsReadFileLength(
          options, max_bytes, &validation_error)) {
    throw std::runtime_error(validation_error);
  }
  if (options.has_length && options.length == 0) {
    co_return AllocateResultBuffer(helpers, 0);
  }
  auto file = MuonFsGFile(path);
  auto result = AllocateResultBuffer(helpers, 0);
  const auto actual_size = co_await ReadMuonFsFileRangeAsync(
      file.get(),
      options,
      max_bytes,
      [&result, helpers](size_t size) -> std::span<std::byte> {
        result = AllocateResultBuffer(helpers, size);
        return result.Bytes();
      },
      cancellation,
      nullptr);
  result.Resize(actual_size);
  co_return std::move(result);
}

static cardio::promise<std::string> ReadTextAsync(
    MuonFsIoContext& context,
    std::string path,
    uint64_t max_bytes,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto contents = co_await ReadMuonFsTextContentsAsync(
      file.get(),
      max_bytes,
      kMuonFsReadTextFileChunkBytes,
      cancellation,
      nullptr);
  if (!IsValidUtf8WithoutNul(
          reinterpret_cast<const uint8_t*>(contents.data()),
          contents.size())) {
    throw std::runtime_error("File is not valid UTF-8 text");
  }
  if (contents.empty()) {
    co_return std::string{};
  }
  co_return std::string(
      reinterpret_cast<const char*>(contents.data()), contents.size());
}

static cardio::promise<void> WriteBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::span<const std::byte> source,
    MuonFsWriteOptions options,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  if (options.has_position) {
    auto file = MuonFsGFile(path);
    auto stream = MuonGObjectPtr<GFileIOStream>();
    auto should_create = false;
    try {
      stream.Reset(co_await OpenReadwriteAsync(file.get(), cancellation));
    } catch (const cardio::gio::gio_error& error) {
      if (!IsGioNotFound(error)) {
        throw;
      }
      should_create = true;
    }
    if (should_create) {
      stream.Reset(co_await CreateReadwriteAsync(file.get(), cancellation));
    }
    auto* io_stream = G_IO_STREAM(stream.get());
    if (!G_IS_SEEKABLE(io_stream) ||
        !g_seekable_can_seek(G_SEEKABLE(io_stream))) {
      throw std::runtime_error("File does not support seeking");
    }
    if (options.position > static_cast<uint64_t>(
                               std::numeric_limits<goffset>::max())) {
      throw std::runtime_error("Write position is too large");
    }
    auto* error = static_cast<GError*>(nullptr);
    ThrowIfGioFailed(
        g_seekable_seek(
            G_SEEKABLE(io_stream),
            static_cast<goffset>(options.position),
            G_SEEK_SET,
            nullptr,
            &error),
        error,
        "Failed to seek file");
    co_await WriteAllToOutputStreamAsync(
        g_io_stream_get_output_stream(io_stream), source, cancellation);
    co_await CloseIoStreamAsync(io_stream, cancellation);
    cancellation.throw_if_cancellation_requested();
    co_return;
  }
  auto file = MuonFsGFile(path);
  if (source.empty()) {
    co_await ReplaceEmptyContentsAsync(file.get(), cancellation);
    co_return;
  }
  auto bytes = MuonFsGBytes(source);
  co_await ReplaceContentsBytesAsync(file.get(), bytes.get(), cancellation);
}

static cardio::promise<void> WriteOwnedBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::vector<std::byte> source,
    MuonFsWriteOptions options,
    cardio::cancellation cancellation) {
  const auto source_view = std::span<const std::byte>(
      source.empty() ? nullptr : source.data(), source.size());
  co_await WriteBytesAsync(
      context, std::move(path), source_view, options, cancellation);
}

static cardio::promise<void> WriteTextAsync(
    MuonFsIoContext& context,
    std::string path,
    std::string text,
    cardio::cancellation cancellation) {
  auto source = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text.data()), text.size());
  co_await WriteBytesAsync(
      context, std::move(path), source, MuonFsWriteOptions{}, cancellation);
}

#endif

#if defined(_WIN32)

static cardio::promise<MuonFsStringResult> StatAsync(
    MuonFsIoContext& context,
    std::string path,
    bool follow_symlink,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), follow_symlink, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        try {
          const auto target_path = CreateLocalFilesystemPath(path);
          const auto preflight_error = WindowsStatPreflightError(target_path);
          if (!preflight_error.empty()) {
            return MuonFsStringResult{std::string{}, preflight_error};
          }
          const auto result = CreateStatusJson(target_path, follow_symlink);
          cancellation.throw_if_cancellation_requested();
          return MuonFsStringResult{result, std::string{}};
        } catch (const std::exception& error) {
          return MuonFsStringResult{std::string{}, error.what()};
        }
      });
}

static cardio::promise<bool> ExistsAsync(MuonFsIoContext& context,
                                         std::string path,
                                         cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        std::error_code error;
        const auto result = std::filesystem::exists(
            CreateLocalFilesystemPath(path), error);
        cancellation.throw_if_cancellation_requested();
        return !error && result;
      });
}

static cardio::promise<bool> AccessAsync(MuonFsIoContext& context,
                                         std::string path,
                                         MuonFsAccessOptions options,
                                         cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), options, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto result = CanAccessPath(
            CreateLocalFilesystemPath(path), options);
        cancellation.throw_if_cancellation_requested();
        return result;
      });
}

static cardio::promise<std::string> ReaddirAsync(
    MuonFsIoContext& context,
    std::string path,
    MuonFsReaddirOptions options,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), options, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        auto result = std::string("[");
        auto first = true;
        std::error_code error;
        const auto target = CreateLocalFilesystemPath(path);
        for (const auto& entry : std::filesystem::directory_iterator(target,
                                                                     error)) {
          ThrowFilesystemError("readdir", target, error);
          cancellation.throw_if_cancellation_requested();
          if (!first) {
            result += ",";
          }
          first = false;
          if (options.with_file_types) {
            result += CreateDirentJson(entry);
          } else {
            AppendJsonString(&result, PathToUtf8String(entry.path().filename()));
          }
        }
        ThrowFilesystemError("readdir", target, error);
        result += "]";
        cancellation.throw_if_cancellation_requested();
        return result;
      });
}

static cardio::promise<void> MkdirAsync(MuonFsIoContext& context,
                                        std::string path,
                                        MuonFsMkdirOptions options,
                                        cardio::cancellation cancellation) {
  (void)context;
  co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), options, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        std::error_code error;
        const auto target = CreateLocalFilesystemPath(path);
        if (options.recursive) {
          std::filesystem::create_directories(target, error);
          ThrowFilesystemError("mkdir", target, error);
          cancellation.throw_if_cancellation_requested();
          return;
        }
        const auto created = std::filesystem::create_directory(target, error);
        ThrowFilesystemError("mkdir", target, error);
        if (!created) {
          throw std::runtime_error("Path already exists");
        }
        cancellation.throw_if_cancellation_requested();
      });
}

static cardio::promise<void> RmAsync(MuonFsIoContext& context,
                                     std::string path,
                                     MuonFsRmOptions options,
                                     cardio::cancellation cancellation) {
  (void)context;
  co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), options, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto target = CreateLocalFilesystemPath(path);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(target, error);
        if (error || status.type() == std::filesystem::file_type::not_found) {
          if (options.force) {
            return;
          }
          ThrowFilesystemError("rm", target, error);
          throw std::runtime_error("Path does not exist");
        }
        if (std::filesystem::is_directory(status) && !options.recursive) {
          throw std::runtime_error("Path is a directory");
        }
        if (options.recursive && !std::filesystem::is_symlink(status)) {
          std::filesystem::remove_all(target, error);
        } else {
          std::filesystem::remove(target, error);
        }
        ThrowFilesystemError("rm", target, error);
        cancellation.throw_if_cancellation_requested();
      });
}

static cardio::promise<std::string> UnlinkAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        try {
          cancellation.throw_if_cancellation_requested();
          const auto target = CreateLocalFilesystemPath(path);
          std::error_code error;
          const auto status = std::filesystem::symlink_status(target, error);
          ThrowFilesystemError("unlink", target, error);
          if (std::filesystem::is_directory(status)) {
            return std::string{"Path is a directory"};
          }
          std::filesystem::remove(target, error);
          ThrowFilesystemError("unlink", target, error);
          cancellation.throw_if_cancellation_requested();
        } catch (const std::exception& error) {
          return std::string{error.what()};
        }
        return std::string{};
      });
}

static cardio::promise<void> RmdirAsync(MuonFsIoContext& context,
                                        std::string path,
                                        cardio::cancellation cancellation) {
  (void)context;
  co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto target = CreateLocalFilesystemPath(path);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(target, error);
        ThrowFilesystemError("rmdir", target, error);
        if (!std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
          throw std::runtime_error("Path is not a directory");
        }
        std::filesystem::remove(target, error);
        ThrowFilesystemError("rmdir", target, error);
        cancellation.throw_if_cancellation_requested();
      });
}

static cardio::promise<void> RenameAsync(MuonFsIoContext& context,
                                         std::string old_path,
                                         std::string new_path,
                                         cardio::cancellation cancellation) {
  (void)context;
  co_await RunWindowsBlockingFsAsync(
      [old_path = std::move(old_path),
       new_path = std::move(new_path),
       cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto old_target = CreateLocalFilesystemPath(old_path);
        std::error_code error;
        std::filesystem::rename(
            old_target, CreateLocalFilesystemPath(new_path), error);
        ThrowFilesystemError("rename", old_target, error);
        cancellation.throw_if_cancellation_requested();
      });
}

static cardio::promise<std::string> CopyFileAsync(
    MuonFsIoContext& context,
    std::string source,
    std::string destination,
    MuonFsCopyOptions options,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [source = std::move(source),
       destination = std::move(destination),
       options,
       cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        try {
          const auto source_path = CreateLocalFilesystemPath(source);
          ThrowIfNotRegularFile(source_path, "Source is not a regular file");
          const auto destination_path = CreateLocalFilesystemPath(destination);
          if (!options.overwrite) {
            std::error_code exists_error;
            const auto destination_exists =
                std::filesystem::exists(destination_path, exists_error);
            ThrowFilesystemError("copyFile", destination_path, exists_error);
            if (destination_exists) {
              return std::string{"Destination already exists"};
            }
          }
          const auto copy_options =
              options.overwrite
                  ? std::filesystem::copy_options::overwrite_existing
                  : std::filesystem::copy_options::none;
          std::error_code error;
          const auto copied = std::filesystem::copy_file(
              source_path, destination_path, copy_options, error);
          ThrowFilesystemError("copyFile", source_path, error);
          if (!copied && !options.overwrite) {
            return std::string{"Destination already exists"};
          }
        } catch (const std::exception& error) {
          return std::string{error.what()};
        }
        cancellation.throw_if_cancellation_requested();
        return std::string{};
      });
}

static cardio::promise<void> AppendBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::span<const std::byte> source,
    cardio::cancellation cancellation) {
  (void)context;
  co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), source, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        auto stream = std::ofstream(
            CreateLocalFilesystemPath(path), std::ios::binary | std::ios::app);
        if (!stream.is_open()) {
          throw std::runtime_error("Failed to open file");
        }
        if (!source.empty()) {
          stream.write(reinterpret_cast<const char*>(source.data()),
                       static_cast<std::streamsize>(source.size()));
        }
        if (!stream.good()) {
          throw std::runtime_error("Failed to append file");
        }
        cancellation.throw_if_cancellation_requested();
      });
}

static cardio::promise<void> AppendOwnedBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::vector<std::byte> source,
    cardio::cancellation cancellation) {
  const auto source_view = std::span<const std::byte>(
      source.empty() ? nullptr : source.data(), source.size());
  co_await AppendBytesAsync(
      context, std::move(path), source_view, cancellation);
}

static cardio::promise<void> AppendTextAsync(
    MuonFsIoContext& context,
    std::string path,
    std::string text,
    cardio::cancellation cancellation) {
  auto source = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text.data()), text.size());
  co_await AppendBytesAsync(context, std::move(path), source, cancellation);
}

static cardio::promise<void> TruncateAsync(MuonFsIoContext& context,
                                           std::string path,
                                           uint64_t length,
                                           cardio::cancellation cancellation) {
  (void)context;
  co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), length, cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        std::error_code error;
        const auto target = CreateLocalFilesystemPath(path);
        std::filesystem::resize_file(target, length, error);
        ThrowFilesystemError("truncate", target, error);
        cancellation.throw_if_cancellation_requested();
      });
}

static cardio::promise<std::string> RealpathAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        std::error_code error;
        const auto target = CreateLocalFilesystemPath(path);
        const auto canonical = std::filesystem::canonical(target, error);
        ThrowFilesystemError("realpath", target, error);
        cancellation.throw_if_cancellation_requested();
        return PathToUtf8String(canonical);
      });
}

static cardio::promise<std::string> ReadlinkAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto link_path = CreateLocalFilesystemPath(path);
        const auto target = ReadWindowsSymbolicLinkTarget(link_path);
        cancellation.throw_if_cancellation_requested();
        return PathToUtf8String(std::filesystem::path(target));
      });
}

static cardio::promise<std::string> SymlinkAsync(
    MuonFsIoContext& context,
    std::string target,
    std::string path,
    std::string type,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [target = std::move(target),
       path = std::move(path),
       type = std::move(type),
       cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto target_path = CreateLocalFilesystemPath(target);
        const auto link_path = CreateLocalFilesystemPath(path);
        if (type == "dir" || type == "junction") {
          const auto error = TryCreateWindowsSymbolicLink(
              target_path, link_path, SYMBOLIC_LINK_FLAG_DIRECTORY);
          if (!error.empty()) {
            return error;
          }
        } else if (type.empty() || type == "file") {
          const auto error =
              TryCreateWindowsSymbolicLink(target_path, link_path, 0);
          if (!error.empty()) {
            return error;
          }
        } else {
          return std::string{"Symlink type must be file, dir, or junction"};
        }
        cancellation.throw_if_cancellation_requested();
        return std::string{};
      });
}

static cardio::promise<std::string> WatchSnapshotAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  co_return co_await RunWindowsBlockingFsAsync(
      [path = std::move(path), cancellation]() mutable {
        cancellation.throw_if_cancellation_requested();
        const auto target = CreateLocalFilesystemPath(path);
        std::error_code error;
        const auto status = std::filesystem::symlink_status(target, error);
        ThrowFilesystemError("watch", target, error);
        auto result = std::string("{\"root\":");
        result += CreateStatusJson(target, false);
        result += ",\"entries\":";
        if (std::filesystem::is_directory(status) &&
            !std::filesystem::is_symlink(status)) {
          result += "[";
          auto first = true;
          for (const auto& entry : std::filesystem::directory_iterator(target,
                                                                       error)) {
            ThrowFilesystemError("watch", target, error);
            if (!first) {
              result += ",";
            }
            first = false;
            result += CreateDirentJson(entry);
          }
          ThrowFilesystemError("watch", target, error);
          result += "]";
        } else {
          result += "[]";
        }
        result += "}";
        cancellation.throw_if_cancellation_requested();
        return result;
      });
}

#else

static cardio::promise<bool> ExistsAsync(MuonFsIoContext& context,
                                         std::string path,
                                         cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  try {
    auto info = MuonGObjectPtr<GFileInfo>(
        co_await QueryInfoAsync(file.get(), true, cancellation));
    (void)info;
    cancellation.throw_if_cancellation_requested();
    co_return true;
  } catch (const cardio::gio::gio_error&) {
    cancellation.throw_if_cancellation_requested();
    co_return false;
  }
}

static cardio::promise<void> EnsureDirectoryAsync(
    GFile* file,
    cardio::cancellation cancellation) {
  try {
    auto info = MuonGObjectPtr<GFileInfo>(
        co_await QueryInfoAsync(file, false, cancellation));
    if (GFileInfoIsDirectory(info.get()) &&
        !GFileInfoIsSymlink(info.get())) {
      co_return;
    }
    throw std::runtime_error("Path already exists");
  } catch (const cardio::gio::gio_error& error) {
    if (!IsGioNotFound(error)) {
      throw;
    }
  }

  auto parent = MuonGObjectPtr<GFile>(g_file_get_parent(file));
  if (parent.get() != nullptr) {
    co_await EnsureDirectoryAsync(parent.get(), cancellation);
  }
  auto already_exists = false;
  try {
    co_await MakeDirectoryAsync(file, cancellation);
  } catch (const cardio::gio::gio_error& error) {
    if (error.domain() != G_IO_ERROR ||
        error.code() != G_IO_ERROR_EXISTS) {
      throw;
    }
    already_exists = true;
  }
  if (already_exists) {
    auto info = MuonGObjectPtr<GFileInfo>(
        co_await QueryInfoAsync(file, false, cancellation));
    if (!GFileInfoIsDirectory(info.get()) ||
        GFileInfoIsSymlink(info.get())) {
      throw std::runtime_error("Path already exists");
    }
  }
}

static cardio::promise<std::string> StatAsync(
    MuonFsIoContext& context,
    std::string path,
    bool follow_symlink,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(file.get(), follow_symlink, cancellation));
  cancellation.throw_if_cancellation_requested();
  co_return CreateGFileInfoStatusJson(info.get());
}

static cardio::promise<bool> AccessAsync(MuonFsIoContext& context,
                                         std::string path,
                                         MuonFsAccessOptions options,
                                         cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto info = MuonGObjectPtr<GFileInfo>();
  try {
    info.Reset(co_await QueryInfoAsync(file.get(), true, cancellation));
  } catch (const cardio::gio::gio_error&) {
    cancellation.throw_if_cancellation_requested();
    co_return false;
  }
  if (!options.read && !options.write && !options.execute) {
    cancellation.throw_if_cancellation_requested();
    co_return true;
  }
  if (options.read &&
      (!g_file_info_has_attribute(info.get(),
                                  G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
       !g_file_info_get_attribute_boolean(
           info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_READ))) {
    co_return false;
  }
  if (options.write &&
      (!g_file_info_has_attribute(info.get(),
                                  G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) ||
       !g_file_info_get_attribute_boolean(
           info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))) {
    co_return false;
  }
  if (options.execute &&
      (!g_file_info_has_attribute(info.get(),
                                  G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) ||
       !g_file_info_get_attribute_boolean(
           info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))) {
    co_return false;
  }
  cancellation.throw_if_cancellation_requested();
  co_return true;
}

static cardio::promise<std::string> ReaddirAsync(
    MuonFsIoContext& context,
    std::string path,
    MuonFsReaddirOptions options,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto enumerator = MuonGObjectPtr<GFileEnumerator>(
      co_await EnumerateChildrenAsync(file.get(), cancellation));
  auto result = std::string("[");
  auto first = true;
  while (true) {
    auto* raw_list = co_await NextFilesAsync(enumerator.get(), cancellation);
    if (raw_list == nullptr) {
      break;
    }
    auto infos = std::vector<MuonGObjectPtr<GFileInfo>>();
    for (auto* entry = raw_list; entry != nullptr; entry = entry->next) {
      infos.emplace_back(static_cast<GFileInfo*>(entry->data));
      entry->data = nullptr;
    }
    g_list_free(raw_list);
    for (const auto& info : infos) {
      cancellation.throw_if_cancellation_requested();
      if (!first) {
        result += ",";
      }
      first = false;
      if (options.with_file_types) {
        result += CreateGFileInfoDirentJson(info.get());
      } else {
        const auto* name = g_file_info_get_name(info.get());
        AppendJsonString(&result, name == nullptr ? std::string_view{}
                                                  : std::string_view(name));
      }
    }
  }
  co_await CloseEnumeratorAsync(enumerator.get(), cancellation);
  result += "]";
  cancellation.throw_if_cancellation_requested();
  co_return result;
}

static cardio::promise<void> MkdirAsync(MuonFsIoContext& context,
                                        std::string path,
                                        MuonFsMkdirOptions options,
                                        cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  if (options.recursive) {
    co_await EnsureDirectoryAsync(file.get(), cancellation);
    co_return;
  }
  co_await MakeDirectoryAsync(file.get(), cancellation);
}

static cardio::promise<void> RemoveGFileAsync(
    GFile* file,
    MuonFsRmOptions options,
    cardio::cancellation cancellation) {
  auto info = MuonGObjectPtr<GFileInfo>();
  try {
    info.Reset(co_await QueryInfoAsync(file, false, cancellation));
  } catch (const cardio::gio::gio_error& error) {
    if (options.force && IsGioNotFound(error)) {
      co_return;
    }
    throw;
  }
  if (GFileInfoIsDirectory(info.get()) && !GFileInfoIsSymlink(info.get())) {
    if (!options.recursive) {
      throw std::runtime_error("Path is a directory");
    }
    auto enumerator = MuonGObjectPtr<GFileEnumerator>(
        co_await EnumerateChildrenAsync(file, cancellation));
    while (true) {
      auto* raw_list = co_await NextFilesAsync(enumerator.get(), cancellation);
      if (raw_list == nullptr) {
        break;
      }
      auto children = std::vector<MuonGObjectPtr<GFile>>();
      for (auto* entry = raw_list; entry != nullptr; entry = entry->next) {
        auto* child_info = static_cast<GFileInfo*>(entry->data);
        children.emplace_back(
            g_file_enumerator_get_child(enumerator.get(), child_info));
      }
      g_list_free_full(raw_list, g_object_unref);
      for (const auto& child : children) {
        co_await RemoveGFileAsync(
            child.get(),
            MuonFsRmOptions{true, false},
            cancellation);
      }
    }
    co_await CloseEnumeratorAsync(enumerator.get(), cancellation);
  }
  try {
    co_await DeleteFileAsync(file, cancellation);
  } catch (const cardio::gio::gio_error& error) {
    if (!options.force || !IsGioNotFound(error)) {
      throw;
    }
  }
}

static cardio::promise<void> RmAsync(MuonFsIoContext& context,
                                     std::string path,
                                     MuonFsRmOptions options,
                                     cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  co_await RemoveGFileAsync(file.get(), options, cancellation);
}

static cardio::promise<void> UnlinkAsync(MuonFsIoContext& context,
                                         std::string path,
                                         cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(file.get(), false, cancellation));
  if (GFileInfoIsDirectory(info.get())) {
    throw std::runtime_error("Path is a directory");
  }
  co_await DeleteFileAsync(file.get(), cancellation);
}

static cardio::promise<void> RmdirAsync(MuonFsIoContext& context,
                                        std::string path,
                                        cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(file.get(), false, cancellation));
  if (!GFileInfoIsDirectory(info.get()) ||
      GFileInfoIsSymlink(info.get())) {
    throw std::runtime_error("Path is not a directory");
  }
  co_await DeleteFileAsync(file.get(), cancellation);
}

static cardio::promise<void> RenameAsync(MuonFsIoContext& context,
                                         std::string old_path,
                                         std::string new_path,
                                         cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto source = MuonFsGFile(old_path);
  auto destination = MuonFsGFile(new_path);
  co_await MoveFileAsync(source.get(), destination.get(), cancellation);
}

static cardio::promise<void> CopyFileAsync(MuonFsIoContext& context,
                                           std::string source,
                                           std::string destination,
                                           MuonFsCopyOptions options,
                                           cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto source_file = MuonFsGFile(source);
  auto destination_file = MuonFsGFile(destination);
  auto info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(source_file.get(), false, cancellation));
  if (!GFileInfoIsRegularFile(info.get())) {
    throw std::runtime_error("Source is not a regular file");
  }
  co_await CopyFileContentsAsync(
      source_file.get(), destination_file.get(), options, cancellation);
}

static cardio::promise<void> AppendBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::span<const std::byte> source,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto stream = MuonGObjectPtr<GFileOutputStream>(
      co_await AppendToAsync(file.get(), cancellation));
  co_await WriteAllToOutputStreamAsync(
      G_OUTPUT_STREAM(stream.get()), source, cancellation);
  co_await CloseOutputStreamAsync(G_OUTPUT_STREAM(stream.get()), cancellation);
}

static cardio::promise<void> AppendOwnedBytesAsync(
    MuonFsIoContext& context,
    std::string path,
    std::vector<std::byte> source,
    cardio::cancellation cancellation) {
  const auto source_view = std::span<const std::byte>(
      source.empty() ? nullptr : source.data(), source.size());
  co_await AppendBytesAsync(
      context, std::move(path), source_view, cancellation);
}

static cardio::promise<void> AppendTextAsync(
    MuonFsIoContext& context,
    std::string path,
    std::string text,
    cardio::cancellation cancellation) {
  auto source = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text.data()), text.size());
  co_await AppendBytesAsync(context, std::move(path), source, cancellation);
}

static cardio::promise<void> TruncateAsync(MuonFsIoContext& context,
                                           std::string path,
                                           uint64_t length,
                                           cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  if (length >
      static_cast<uint64_t>(std::numeric_limits<goffset>::max())) {
    throw std::runtime_error("Length is too large");
  }
  auto file = MuonFsGFile(path);
  auto stream = MuonGObjectPtr<GFileIOStream>(
      co_await OpenReadwriteAsync(file.get(), cancellation));
  auto* io_stream = G_IO_STREAM(stream.get());
  if (!G_IS_SEEKABLE(io_stream) ||
      !g_seekable_can_truncate(G_SEEKABLE(io_stream))) {
    throw std::runtime_error("File does not support truncation");
  }
  auto* error = static_cast<GError*>(nullptr);
  ThrowIfGioFailed(
      g_seekable_truncate(
          G_SEEKABLE(io_stream),
          static_cast<goffset>(length),
          nullptr,
          &error),
      error,
      "Failed to truncate file");
  co_await CloseIoStreamAsync(io_stream, cancellation);
}

static cardio::promise<std::string> RealpathAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(file.get(), true, cancellation));
  (void)info;
  auto* local_path = g_file_get_path(file.get());
  if (local_path != nullptr) {
    auto local_path_holder =
        std::unique_ptr<char, decltype(&g_free)>(local_path, &g_free);
    std::error_code error;
    const auto canonical = std::filesystem::canonical(
        std::filesystem::path(local_path_holder.get()), error);
    ThrowFilesystemError("realpath",
                         std::filesystem::path(local_path_holder.get()),
                         error);
    co_return PathToUtf8String(canonical);
  }
  auto* uri = g_file_get_uri(file.get());
  if (uri == nullptr) {
    throw std::runtime_error("Failed to resolve path");
  }
  auto uri_holder = std::unique_ptr<char, decltype(&g_free)>(uri, &g_free);
  co_return std::string(uri_holder.get());
}

static cardio::promise<std::string> ReadlinkAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(file.get(), false, cancellation));
  if (!GFileInfoIsSymlink(info.get())) {
    throw std::runtime_error("Path is not a symbolic link");
  }
  const auto* target = g_file_info_get_symlink_target(info.get());
  if (target == nullptr) {
    throw std::runtime_error("Failed to read symbolic link");
  }
  co_return std::string(target);
}

static cardio::promise<void> SymlinkAsync(MuonFsIoContext& context,
                                          std::string target,
                                          std::string path,
                                          std::string type,
                                          cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  if (!type.empty() && type != "file" && type != "dir" &&
      type != "junction") {
    throw std::runtime_error("Symlink type must be file, dir, or junction");
  }
  auto file = MuonFsGFile(path);
  co_await MakeSymbolicLinkAsync(file.get(), target, cancellation);
}

static cardio::promise<std::string> WatchSnapshotAsync(
    MuonFsIoContext& context,
    std::string path,
    cardio::cancellation cancellation) {
  (void)context;
  cancellation.throw_if_cancellation_requested();
  auto file = MuonFsGFile(path);
  auto root_info = MuonGObjectPtr<GFileInfo>(
      co_await QueryInfoAsync(file.get(), false, cancellation));
  auto result = std::string("{\"root\":");
  result += CreateGFileInfoStatusJson(root_info.get());
  result += ",\"entries\":";
  if (GFileInfoIsDirectory(root_info.get()) &&
      !GFileInfoIsSymlink(root_info.get())) {
    auto enumerator = MuonGObjectPtr<GFileEnumerator>(
        co_await EnumerateChildrenAsync(file.get(), cancellation));
    result += "[";
    auto first = true;
    while (true) {
      auto* raw_list = co_await NextFilesAsync(enumerator.get(), cancellation);
      if (raw_list == nullptr) {
        break;
      }
      auto infos = std::vector<MuonGObjectPtr<GFileInfo>>();
      for (auto* entry = raw_list; entry != nullptr; entry = entry->next) {
        infos.emplace_back(static_cast<GFileInfo*>(entry->data));
        entry->data = nullptr;
      }
      g_list_free(raw_list);
      for (const auto& info : infos) {
        cancellation.throw_if_cancellation_requested();
        if (!first) {
          result += ",";
        }
        first = false;
        result += CreateGFileInfoDirentJson(info.get());
      }
    }
    co_await CloseEnumeratorAsync(enumerator.get(), cancellation);
    result += "]";
  } else {
    result += "[]";
  }
  result += "}";
  cancellation.throw_if_cancellation_requested();
  co_return result;
}

#endif

static bool RegisterAbortWatcher(
    const muon_plugin_helpers* helpers,
    muon_native_function abort_watcher,
    const std::shared_ptr<MuonTrafficCardioCancellation>& cancellation,
    muon_native_function* cancel_function_out,
    std::string* error_message);

class MuonFsPluginFunctionRef final {
 public:
  MuonFsPluginFunctionRef(const muon_plugin_helpers* helpers,
                          muon_native_function function)
      : helpers_(helpers), function_(function) {}

  ~MuonFsPluginFunctionRef() {
    Release();
  }

  MuonFsPluginFunctionRef(const MuonFsPluginFunctionRef&) = delete;
  MuonFsPluginFunctionRef& operator=(const MuonFsPluginFunctionRef&) = delete;

  void Release() {
    if (function_ == nullptr) {
      return;
    }
    if (helpers_ != nullptr &&
        helpers_->__release_plugin_function_pointer_impl != nullptr) {
      helpers_->release_plugin_function_pointer(function_);
    }
    function_ = nullptr;
  }

 private:
  const muon_plugin_helpers* helpers_ = nullptr;
  muon_native_function function_ = nullptr;
};

template <typename Task>
static cardio::promise<void> RunMuonFsCompletionOnDispatcher(
    cardio::dispatcher* dispatcher,
    Task&& task) {
  auto* current_dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr || dispatcher == current_dispatcher) {
    std::forward<Task>(task)();
    co_return;
  }
  if (current_dispatcher == nullptr) {
    throw std::runtime_error(
        "muon filesystem completion dispatcher is unavailable");
  }
  co_await cardio::switch_to(*dispatcher);
  std::forward<Task>(task)();
  co_return;
}

class MuonBuiltinFsRuntime final {
 public:
  MuonBuiltinFsRuntime(const muon_plugin_helpers* helpers,
                       uint64_t read_file_max_bytes,
                       uint64_t read_text_file_max_bytes)
      : helpers_(helpers),
        read_file_max_bytes_(read_file_max_bytes),
        read_text_file_max_bytes_(read_text_file_max_bytes),
        context_(std::make_shared<MuonFsIoContext>()),
        completion_dispatcher_(cardio::unsafe_get_current_dispatcher()),
        completions_enabled_(std::make_shared<std::atomic_bool>(false)) {}

  ~MuonBuiltinFsRuntime() {
    Stop();
    if (!HasPendingOperations()) {
      ResetIoContext();
    }
  }

  bool Start(std::string* error_message) {
    if (error_message == nullptr) {
      return false;
    }
    if (helpers_ == nullptr) {
      *error_message = "muon filesystem helpers are unavailable";
      return false;
    }
#if defined(_WIN32)
    try {
      if (!context_) {
        context_ = std::make_shared<MuonFsIoContext>();
      }
    } catch (const std::exception& error) {
      *error_message = error.what();
      return false;
    }
#endif
    if (completion_dispatcher_ == nullptr) {
      *error_message = "muon filesystem dispatcher is unavailable";
      return false;
    }
    completions_enabled_->store(true);
    running_ = true;
    return true;
  }

  void Stop() {
    running_ = false;
    completions_enabled_->store(false);
    watch_registry_.ReleaseAll();
    for (auto& operation : active_) {
      operation.cancellation->ForceCancel(kMuonBuiltinFsShutdownError);
    }
    PruneActiveOperationsLocked();
  }

  bool HasPendingOperations() {
    PruneActiveOperationsLocked();
    return !active_.empty();
  }

  template <typename Result, typename Start, typename Complete>
  void SubmitOperation(muon_completion_func completion,
                       muon_native_function abort_watcher,
                       Start&& start,
                       Complete&& complete) {
    const auto operation_id =
        g_muon_fs_operation_sequence.fetch_add(1, std::memory_order_relaxed) +
        1;
    AppendMuonFsDiagnosticLog(
        operation_id,
        "submit running=" + std::string(FormatMuonCloseDebugBool(running_)) +
            " dispatcher=" +
            FormatMuonCloseDebugPointer(completion_dispatcher_) +
            " active=" + std::to_string(active_.size()));
    PruneActiveOperations();
    if (!running_) {
      AppendMuonFsDiagnosticLog(operation_id, "reject_not_running");
      CompleteMuonError(completion, kMuonBuiltinFsShutdownError);
      return;
    }
    auto cancellation = std::make_shared<MuonTrafficCardioCancellation>();
    auto abort_error = std::string{};
    auto cancel_function = muon_native_function{};
    if (!RegisterAbortWatcher(
            helpers_, abort_watcher, cancellation, &cancel_function,
            &abort_error)) {
      AppendMuonFsDiagnosticLog(
          operation_id,
          "abort_watcher_error message=" + abort_error);
      CompleteMuonError(completion, abort_error);
      return;
    }
    auto cancel_function_ref =
        std::make_shared<MuonFsPluginFunctionRef>(helpers_, cancel_function);
    if (!running_) {
      cancellation->ForceCancel(kMuonBuiltinFsShutdownError);
      AppendMuonFsDiagnosticLog(operation_id, "reject_stopped_after_abort");
      CompleteMuonError(completion, kMuonBuiltinFsShutdownError);
      return;
    }
    auto context = context_;
    auto start_copy = std::forward<Start>(start);
    auto complete_copy = std::forward<Complete>(complete);
    auto* completion_dispatcher = completion_dispatcher_;
    auto completions_enabled = completions_enabled_;
    auto fail = [completion,
                 completions_enabled,
                 completion_dispatcher,
                 cancel_function_ref,
                 operation_id](std::string message)
        -> cardio::promise<void> {
      AppendMuonFsDiagnosticLog(
          operation_id,
          "fail_ready dispatcher=" +
              FormatMuonCloseDebugPointer(completion_dispatcher) +
              " message=" + message);
      co_await RunMuonFsCompletionOnDispatcher(
          completion_dispatcher,
          [completion,
           completions_enabled,
           cancel_function_ref,
           operation_id,
           message = std::move(message)] {
            AppendMuonFsDiagnosticLog(operation_id, "fail_dispatch");
            if (completions_enabled->load()) {
              CompleteMuonError(completion, message);
            }
            cancel_function_ref->Release();
          });
    };
    auto complete_operation = [complete_copy = std::move(complete_copy),
                               completions_enabled,
                               completion_dispatcher,
                               cancel_function_ref,
                               operation_id](auto&&... args) mutable
        -> cardio::promise<void> {
      auto args_tuple =
          std::make_tuple(std::forward<decltype(args)>(args)...);
      AppendMuonFsDiagnosticLog(
          operation_id,
          "complete_ready dispatcher=" +
              FormatMuonCloseDebugPointer(completion_dispatcher));
      co_await RunMuonFsCompletionOnDispatcher(
          completion_dispatcher,
          [complete_copy = std::move(complete_copy),
           completions_enabled,
           cancel_function_ref,
           operation_id,
           args_tuple = std::move(args_tuple)]() mutable {
            AppendMuonFsDiagnosticLog(operation_id, "complete_dispatch");
            if (completions_enabled->load()) {
              std::apply(
                  [&complete_copy](auto&&... values) {
                    if constexpr (sizeof...(values) == 0) {
                      complete_copy();
                    } else {
                      complete_copy(
                          std::forward<decltype(values)>(values)...);
                    }
                  },
                  std::move(args_tuple));
            }
            cancel_function_ref->Release();
          });
    };
    auto operation = RunMuonTrafficCardioOperation<Result>(
        cancellation,
        kMuonBuiltinFsGenericError,
        [context, start = std::move(start_copy), operation_id](
            cardio::cancellation cancellation_signal) mutable
            -> cardio::promise<Result> {
          AppendMuonFsDiagnosticLog(
              operation_id,
              "body_start dispatcher=" + FormatMuonCloseDebugPointer(
                                             cardio::unsafe_get_current_dispatcher()));
          try {
            auto body = start(*context, cancellation_signal);
            if constexpr (std::is_void_v<Result>) {
              co_await body;
              AppendMuonFsDiagnosticLog(operation_id, "body_success");
              co_return;
            } else {
              auto result = std::move(co_await body);
              AppendMuonFsDiagnosticLog(operation_id, "body_success");
              co_return std::move(result);
            }
          } catch (const std::exception& error) {
            AppendMuonFsDiagnosticLog(
                operation_id,
                std::string("body_error message=") + error.what());
            throw;
          } catch (...) {
            AppendMuonFsDiagnosticLog(operation_id, "body_error unknown");
            throw;
          }
        },
        std::move(complete_operation),
        std::move(fail));
    active_.push_back(MuonFsActiveOperation{
        cancellation, std::move(operation), std::move(cancel_function_ref)});
    if (!running_) {
      active_.back().cancellation->ForceCancel(kMuonBuiltinFsShutdownError);
      AppendMuonFsDiagnosticLog(operation_id, "cancel_after_submit");
    }
    PruneActiveOperationsLocked();
  }

  const muon_plugin_helpers* helpers() const {
    return helpers_;
  }

  uint64_t read_file_max_bytes() const {
    return read_file_max_bytes_;
  }

  uint64_t read_text_file_max_bytes() const {
    return read_text_file_max_bytes_;
  }

  std::optional<MuonBuiltinFsWatchLease> TryAcquireWatchLease(
      int renderer_context_id,
      std::string* error_message) {
    if (!running_) {
      if (error_message != nullptr) {
        *error_message = kMuonBuiltinFsShutdownError;
      }
      return std::nullopt;
    }
    return watch_registry_.TryAcquire(renderer_context_id, error_message);
  }

  bool IsWatchLeaseActive(int renderer_context_id,
                          const std::string& token) const {
    return watch_registry_.IsActive(renderer_context_id, token);
  }

  bool ReleaseWatchLease(int renderer_context_id, const std::string& token) {
    return watch_registry_.Release(renderer_context_id, token);
  }

  void ReleaseWatchContext(int renderer_context_id) {
    watch_registry_.ReleaseContext(renderer_context_id);
  }

 private:
  struct MuonFsActiveOperation {
    std::shared_ptr<MuonTrafficCardioCancellation> cancellation;
    cardio::promise<void> completion;
    std::shared_ptr<MuonFsPluginFunctionRef> cancel_function;
  };

  void ResetIoContext() {
#if defined(_WIN32)
    if (running_) {
      context_ = std::make_shared<MuonFsIoContext>();
    } else {
      context_.reset();
    }
#endif
  }

  void PruneActiveOperations() {
    PruneActiveOperationsLocked();
  }

  void PruneActiveOperationsLocked() {
    const auto active_before = active_.size();
    active_.erase(
        std::remove_if(active_.begin(),
                       active_.end(),
                       [](const MuonFsActiveOperation& operation) {
                         return operation.completion.is_ready();
                       }),
        active_.end());
    if (active_before != 0 && active_.empty()) {
      ResetIoContext();
    }
  }

  const muon_plugin_helpers* helpers_ = nullptr;
  const uint64_t read_file_max_bytes_ =
      kMuonFsDefaultReadFileMaxBytes;
  const uint64_t read_text_file_max_bytes_ =
      kMuonFsDefaultReadTextFileMaxBytes;
  std::shared_ptr<MuonFsIoContext> context_;
  cardio::dispatcher* completion_dispatcher_ = nullptr;
  std::shared_ptr<std::atomic_bool> completions_enabled_;
  MuonBuiltinFsWatchRegistry watch_registry_;
  std::vector<MuonFsActiveOperation> active_;
  bool running_ = false;
};

std::shared_ptr<MuonBuiltinFsRuntime> g_runtime;

static std::shared_ptr<MuonBuiltinFsRuntime> GetRuntime() {
  return g_runtime;
}

struct MuonFsAbortCallbackState {
  std::shared_ptr<MuonTrafficCardioCancellation> cancellation;
};

static void FinalizeMuonFsAbortCallbackState(void* raw_state) {
  delete static_cast<MuonFsAbortCallbackState*>(raw_state);
}

extern "C" void muon_builtin_fs_cancel_task(muon_completion_func completion,
                                             void* raw_state) {
  auto* state = static_cast<MuonFsAbortCallbackState*>(raw_state);
  if (state != nullptr && state->cancellation) {
    state->cancellation->Cancel(kMuonBuiltinFsAbortError);
  }
  CompleteMuonVoid(completion);
}

static bool RegisterAbortWatcher(
    const muon_plugin_helpers* helpers,
    muon_native_function abort_watcher,
    const std::shared_ptr<MuonTrafficCardioCancellation>& cancellation,
    muon_native_function* cancel_function_out,
    std::string* error_message) {
  if (cancel_function_out != nullptr) {
    *cancel_function_out = nullptr;
  }
  if (abort_watcher == nullptr) {
    return true;
  }
  if (helpers == nullptr ||
      helpers->__register_closure_impl == nullptr ||
      helpers->__release_plugin_function_pointer_impl == nullptr) {
    *error_message = "AbortSignal helper is unavailable";
    return false;
  }

  auto* cancel_state = new MuonFsAbortCallbackState;
  cancel_state->cancellation = cancellation;
  auto cancel_function = muon_native_function{};
  char cancel_error_storage[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = "";
  auto cancel_error = muon_error_buffer{
      cancel_error_storage,
      static_cast<uint32_t>(sizeof(cancel_error_storage)),
  };
  if (!helpers->register_closure(
          &cancel_operation_signature,
          &muon_builtin_fs_cancel_task,
          cancel_state,
          &FinalizeMuonFsAbortCallbackState,
          &cancel_function,
          &cancel_error)) {
    delete cancel_state;
    *error_message = cancel_error_storage[0] == '\0'
                         ? "AbortSignal cancel callback registration failed"
                         : cancel_error_storage;
    return false;
  }

  using AbortWatcherFunction = void (*)(muon_completion_func,
                                        muon_native_function);
  reinterpret_cast<AbortWatcherFunction>(abort_watcher)(nullptr,
                                                        cancel_function);
  if (cancel_function_out != nullptr) {
    *cancel_function_out = cancel_function;
  } else {
    helpers->release_plugin_function_pointer(cancel_function);
  }
  return true;
}

static bool ValidatePathValue(muon_completion_func completion,
                              const std::string& path,
                              std::string* target) {
  *target = path;
  if (target->empty()) {
    CompleteMuonError(completion, "Path is required");
    return false;
  }
  if (ContainsNul(*target)) {
    CompleteMuonError(completion, "Path must not contain NUL");
    return false;
  }
  return true;
}

static bool ValidatePathArgument(muon_completion_func completion,
                                 const char* path,
                                 std::string* target) {
  if (path == nullptr) {
    CompleteMuonError(completion, "Path is required");
    return false;
  }
  return ValidatePathValue(completion, path, target);
}

static std::string CreateWatchLeaseResultJson(const std::string& token) {
  std::string result = "{\"token\":";
  AppendJsonString(&result, token);
  result += "}";
  return result;
}

static bool ReadWatchRpcString(yyjson_val* root,
                               const char* key,
                               std::string* target,
                               std::string* error_message) {
  auto* value = yyjson_obj_get(root, key);
  if (!yyjson_is_str(value)) {
    *error_message = std::string("Request ") + key + " is required";
    return false;
  }
  *target = ReadJsonString(value);
  return true;
}

static bool ReadWatchRpcToken(yyjson_val* root,
                              std::string* target,
                              std::string* error_message) {
  if (!ReadWatchRpcString(root, "token", target, error_message)) {
    *error_message = kMuonBuiltinFsWatchLeaseUnavailableError;
    return false;
  }
  if (target->empty() || ContainsNul(*target)) {
    *error_message = kMuonBuiltinFsWatchLeaseUnavailableError;
    return false;
  }
  return true;
}

static bool ParseWatchRpcRequest(const char* request_json,
                                 MuonFsWatchRpcRequest* request,
                                 std::string* error_message) {
  if (request_json == nullptr) {
    *error_message = "Request JSON is required";
    return false;
  }
  yyjson_read_err read_error = {};
  auto document = MuonJsonDocument(yyjson_read_opts(
      const_cast<char*>(request_json), std::strlen(request_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error));
  if (document.get() == nullptr) {
    *error_message = "Request JSON is invalid";
    return false;
  }
  auto* root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root)) {
    *error_message = "Request JSON root must be an object";
    return false;
  }

  auto operation = std::string{};
  if (!ReadWatchRpcString(root, "operation", &operation, error_message)) {
    return false;
  }
  if (operation == "acquire") {
    request->operation = MuonFsWatchRpcOperation::kAcquire;
    return true;
  }
  if (operation == "release") {
    request->operation = MuonFsWatchRpcOperation::kRelease;
    return ReadWatchRpcToken(root, &request->token, error_message);
  }
  if (operation == "snapshot") {
    request->operation = MuonFsWatchRpcOperation::kSnapshot;
    if (!ReadWatchRpcToken(root, &request->token, error_message)) {
      return false;
    }
    if (!ReadWatchRpcString(root, "path", &request->path, error_message)) {
      *error_message = "Path is required";
      return false;
    }
    return true;
  }

  *error_message = "Unsupported filesystem watcher operation";
  return false;
}

extern "C" void muon_builtin_fs_read_file(
    muon_completion_func completion,
    const char* path,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options = MuonFsReadOptions{};
  auto options_error = std::string{};
  if (!ParseReadOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  const auto* helpers = runtime->helpers();
  const auto max_bytes = runtime->read_file_max_bytes();
  if (!ValidateMuonFsReadFileLength(options, max_bytes, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  runtime->SubmitOperation<MuonFsBufferResult>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), helpers, options, max_bytes](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return ReadBytesAsync(
            context,
            std::move(target_path),
            helpers,
            options,
            max_bytes,
            cancellation);
      },
      [completion](MuonFsBufferResult result) mutable {
        result.Complete(completion);
      });
}

extern "C" void muon_builtin_fs_write_file(
    muon_completion_func completion,
    const char* path,
    muon_buffer_view data,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options = MuonFsWriteOptions{};
  auto options_error = std::string{};
  if (!ParseWriteOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  if (data.data == nullptr && data.size != 0) {
    CompleteMuonError(completion, "Invalid buffer_view");
    return;
  }
  auto source = std::vector<std::byte>(static_cast<size_t>(data.size));
  if (!source.empty()) {
    std::memcpy(source.data(), data.data, source.size());
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path),
       source = std::move(source),
       options](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return WriteOwnedBytesAsync(
            context, std::move(target_path), std::move(source), options,
            cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_read_text_file(
    muon_completion_func completion,
    const char* path,
    const char* encoding,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  if (!IsSupportedUtf8Encoding(encoding)) {
    CompleteMuonError(completion, kMuonBuiltinFsEncodingError);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  const auto max_bytes = runtime->read_text_file_max_bytes();
#if defined(_WIN32)
  runtime->SubmitOperation<MuonFsStringResult>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), max_bytes](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return ReadTextAsync(
            context, std::move(target_path), max_bytes, cancellation);
      },
      [completion](MuonFsStringResult result) {
        if (result.error.empty()) {
          CompleteMuonString(completion, std::move(result.value));
        } else {
          CompleteMuonError(completion, result.error);
        }
      });
#else
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), max_bytes](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return ReadTextAsync(
            context, std::move(target_path), max_bytes, cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
#endif
}

extern "C" void muon_builtin_fs_write_text_file(
    muon_completion_func completion,
    const char* path,
    const char* data,
    const char* encoding,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  if (data == nullptr) {
    CompleteMuonError(completion, "Text data is required");
    return;
  }
  if (!IsSupportedUtf8Encoding(encoding)) {
    CompleteMuonError(completion, kMuonBuiltinFsEncodingError);
    return;
  }
  auto text = std::string(data);
  if (!IsValidUtf8WithoutNul(
          reinterpret_cast<const uint8_t*>(text.data()), text.size())) {
    CompleteMuonError(completion, "Text data is not valid UTF-8");
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), text = std::move(text)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return WriteTextAsync(
            context, std::move(target_path), std::move(text), cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_stat(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
#if defined(_WIN32)
  runtime->SubmitOperation<MuonFsStringResult>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return StatAsync(context, std::move(target_path), true, cancellation);
      },
      [completion](MuonFsStringResult result) {
        if (result.error.empty()) {
          CompleteMuonString(completion, result.value);
        } else {
          CompleteMuonError(completion, result.error);
        }
      });
#else
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return StatAsync(context, std::move(target_path), true, cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
#endif
}

extern "C" void muon_builtin_fs_lstat(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
#if defined(_WIN32)
  runtime->SubmitOperation<MuonFsStringResult>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return StatAsync(context, std::move(target_path), false, cancellation);
      },
      [completion](MuonFsStringResult result) {
        if (result.error.empty()) {
          CompleteMuonString(completion, result.value);
        } else {
          CompleteMuonError(completion, result.error);
        }
      });
#else
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return StatAsync(context, std::move(target_path), false, cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
#endif
}

extern "C" void muon_builtin_fs_exists(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<bool>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return ExistsAsync(context, std::move(target_path), cancellation);
      },
      [completion](bool result) {
        CompleteMuonBool(completion, result);
      });
}

extern "C" void muon_builtin_fs_access(
    muon_completion_func completion,
    const char* path,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options = MuonFsAccessOptions{};
  auto options_error = std::string{};
  if (!ParseAccessOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<bool>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), options](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return AccessAsync(
            context, std::move(target_path), options, cancellation);
      },
      [completion](bool result) {
        CompleteMuonBool(completion, result);
      });
}

extern "C" void muon_builtin_fs_readdir(
    muon_completion_func completion,
    const char* path,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options = MuonFsReaddirOptions{};
  auto options_error = std::string{};
  if (!ParseReaddirOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), options](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return ReaddirAsync(
            context, std::move(target_path), options, cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
}

extern "C" void muon_builtin_fs_mkdir(
    muon_completion_func completion,
    const char* path,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options = MuonFsMkdirOptions{};
  auto options_error = std::string{};
  if (!ParseMkdirOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), options](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return MkdirAsync(
            context, std::move(target_path), options, cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_rm(
    muon_completion_func completion,
    const char* path,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options = MuonFsRmOptions{};
  auto options_error = std::string{};
  if (!ParseRmOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), options](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return RmAsync(
            context, std::move(target_path), options, cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_unlink(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
#if defined(_WIN32)
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return UnlinkAsync(context, std::move(target_path), cancellation);
      },
      [completion](std::string error) {
        if (error.empty()) {
          CompleteMuonVoid(completion);
        } else {
          CompleteMuonError(completion, error);
        }
      });
#else
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return UnlinkAsync(context, std::move(target_path), cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
#endif
}

extern "C" void muon_builtin_fs_rmdir(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return RmdirAsync(context, std::move(target_path), cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_rename(
    muon_completion_func completion,
    const char* old_path,
    const char* new_path,
    muon_native_function abort_watcher) {
  auto source_path = std::string{};
  auto destination_path = std::string{};
  if (!ValidatePathArgument(completion, old_path, &source_path) ||
      !ValidatePathArgument(completion, new_path, &destination_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [source_path = std::move(source_path),
       destination_path = std::move(destination_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return RenameAsync(
            context, std::move(source_path), std::move(destination_path),
            cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_copy_file(
    muon_completion_func completion,
    const char* source,
    const char* destination,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto source_path = std::string{};
  auto destination_path = std::string{};
  if (!ValidatePathArgument(completion, source, &source_path) ||
      !ValidatePathArgument(completion, destination, &destination_path)) {
    return;
  }
  auto options = MuonFsCopyOptions{};
  auto options_error = std::string{};
  if (!ParseCopyOptions(options_json, &options, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
#if defined(_WIN32)
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [source_path = std::move(source_path),
       destination_path = std::move(destination_path),
       options](MuonFsIoContext& context,
                cardio::cancellation cancellation) mutable {
        return CopyFileAsync(
            context, std::move(source_path), std::move(destination_path),
            options, cancellation);
      },
      [completion](std::string error) {
        if (error.empty()) {
          CompleteMuonVoid(completion);
        } else {
          CompleteMuonError(completion, error);
        }
      });
#else
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [source_path = std::move(source_path),
       destination_path = std::move(destination_path),
       options](MuonFsIoContext& context,
                cardio::cancellation cancellation) mutable {
        return CopyFileAsync(
            context, std::move(source_path), std::move(destination_path),
            options, cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
#endif
}

extern "C" void muon_builtin_fs_append_file(
    muon_completion_func completion,
    const char* path,
    muon_buffer_view data,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  if (data.data == nullptr && data.size != 0) {
    CompleteMuonError(completion, "Invalid buffer_view");
    return;
  }
  auto source = std::vector<std::byte>(static_cast<size_t>(data.size));
  if (!source.empty()) {
    std::memcpy(source.data(), data.data, source.size());
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), source = std::move(source)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return AppendOwnedBytesAsync(
            context, std::move(target_path), std::move(source), cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_append_text_file(
    muon_completion_func completion,
    const char* path,
    const char* data,
    const char* encoding,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  if (data == nullptr) {
    CompleteMuonError(completion, "Text data is required");
    return;
  }
  if (!IsSupportedUtf8Encoding(encoding)) {
    CompleteMuonError(completion, kMuonBuiltinFsEncodingError);
    return;
  }
  auto text = std::string(data);
  if (!IsValidUtf8WithoutNul(
          reinterpret_cast<const uint8_t*>(text.data()), text.size())) {
    CompleteMuonError(completion, "Text data is not valid UTF-8");
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), text = std::move(text)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return AppendTextAsync(
            context, std::move(target_path), std::move(text), cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_truncate(
    muon_completion_func completion,
    const char* path,
    const char* options_json,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  auto options_error = std::string{};
  auto length = uint64_t{0};
  if (!ParseTruncateLength(options_json, &length, &options_error)) {
    CompleteMuonError(completion, options_error);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path), length](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return TruncateAsync(
            context, std::move(target_path), length, cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
}

extern "C" void muon_builtin_fs_realpath(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return RealpathAsync(context, std::move(target_path), cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
}

extern "C" void muon_builtin_fs_readlink(
    muon_completion_func completion,
    const char* path,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  if (!ValidatePathArgument(completion, path, &target_path)) {
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return ReadlinkAsync(context, std::move(target_path), cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
}

extern "C" void muon_builtin_fs_symlink(
    muon_completion_func completion,
    const char* target,
    const char* path,
    const char* type,
    muon_native_function abort_watcher) {
  auto target_path = std::string{};
  auto link_path = std::string{};
  if (!ValidatePathArgument(completion, target, &target_path) ||
      !ValidatePathArgument(completion, path, &link_path)) {
    return;
  }
  auto link_type = std::string(type == nullptr ? "" : type);
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
#if defined(_WIN32)
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path),
       link_path = std::move(link_path),
       link_type = std::move(link_type)](MuonFsIoContext& context,
                                         cardio::cancellation cancellation)
          mutable {
        return SymlinkAsync(
            context, std::move(target_path), std::move(link_path),
            std::move(link_type), cancellation);
      },
      [completion](std::string error) {
        if (error.empty()) {
          CompleteMuonVoid(completion);
        } else {
          CompleteMuonError(completion, error);
        }
      });
#else
  runtime->SubmitOperation<void>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path),
       link_path = std::move(link_path),
       link_type = std::move(link_type)](MuonFsIoContext& context,
                                         cardio::cancellation cancellation)
          mutable {
        return SymlinkAsync(
            context, std::move(target_path), std::move(link_path),
            std::move(link_type), cancellation);
      },
      [completion]() {
        CompleteMuonVoid(completion);
      });
#endif
}

extern "C" void muon_builtin_fs_watch_rpc(
    muon_completion_func completion,
    const char* request_json,
    uint32_t renderer_context_id,
    muon_native_function abort_watcher) {
  auto request = MuonFsWatchRpcRequest{};
  auto error_message = std::string{};
  if (!ParseWatchRpcRequest(request_json, &request, &error_message)) {
    CompleteMuonError(completion, error_message);
    return;
  }
  const auto runtime = GetRuntime();
  if (!runtime) {
    CompleteMuonError(completion, kMuonBuiltinFsUnavailableError);
    return;
  }
  const auto context_id = static_cast<int>(renderer_context_id);
  switch (request.operation) {
    case MuonFsWatchRpcOperation::kAcquire: {
      const auto lease =
          runtime->TryAcquireWatchLease(context_id, &error_message);
      if (!lease.has_value()) {
        CompleteMuonError(completion, error_message);
        return;
      }
      CompleteMuonString(completion,
                         CreateWatchLeaseResultJson(lease->token));
      return;
    }
    case MuonFsWatchRpcOperation::kRelease:
      (void)runtime->ReleaseWatchLease(context_id, request.token);
      CompleteMuonString(completion, "{}");
      return;
    case MuonFsWatchRpcOperation::kSnapshot:
      break;
  }

  if (!runtime->IsWatchLeaseActive(context_id, request.token)) {
    CompleteMuonError(completion,
                      kMuonBuiltinFsWatchLeaseUnavailableError);
    return;
  }
  auto target_path = std::string{};
  if (!ValidatePathValue(completion, request.path, &target_path)) {
    return;
  }
  runtime->SubmitOperation<std::string>(
      completion,
      abort_watcher,
      [target_path = std::move(target_path)](
          MuonFsIoContext& context,
          cardio::cancellation cancellation) mutable {
        return WatchSnapshotAsync(
            context, std::move(target_path), cancellation);
      },
      [completion](std::string result) {
        CompleteMuonString(completion, std::move(result));
      });
}

static const muon_type_descriptor read_file_args[] = {
    type_string,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor write_file_args[] = {
    type_string,
    type_buffer_view,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor path_abort_args[] = {
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor watch_rpc_args[] = {
    type_string,
    type_u32,
    type_abort_watcher_function,
};

static const muon_type_descriptor path_options_abort_args[] = {
    type_string,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor two_path_abort_args[] = {
    type_string,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor two_path_options_abort_args[] = {
    type_string,
    type_string,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor read_text_file_args[] = {
    type_string,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor write_text_file_args[] = {
    type_string,
    type_string,
    type_string,
    type_abort_watcher_function,
};

static const muon_type_descriptor append_file_args[] = {
    type_string,
    type_buffer_view,
    type_abort_watcher_function,
};

static const muon_type_descriptor symlink_args[] = {
    type_string,
    type_string,
    type_string,
    type_abort_watcher_function,
};

static constexpr char fs_setup_script[] = R"JS(
const createAbortReason = (signal) => {
  if ("reason" in signal && signal.reason !== undefined) {
    return signal.reason;
  }
  if (typeof globalThis.DOMException === "function") {
    return new globalThis.DOMException(
      "The operation was aborted",
      "AbortError",
    );
  }
  const error = new Error("The operation was aborted");
  error.name = "AbortError";
  return error;
};

const getAbortSignal = (options) => {
  if (options === undefined || options === null) {
    return null;
  }
  if (typeof options !== "object") {
    throw new TypeError("options must be an object");
  }
  const signal = options.signal;
  if (signal === undefined || signal === null) {
    return null;
  }
  if (
    typeof signal.aborted !== "boolean" ||
    typeof signal.addEventListener !== "function" ||
    typeof signal.removeEventListener !== "function"
  ) {
    throw new TypeError("options.signal must be an AbortSignal");
  }
  return signal;
};

const runAbortable = async (options, nativeCall) => {
  const signal = getAbortSignal(options);
  if (signal === null) {
    return await nativeCall(null);
  }
  if (signal.aborted) {
    throw createAbortReason(signal);
  }

  const nativeCancelRecords = new Map();
  let aborted = false;
  let settled = false;
  let rejectAbort = null;
  const abortPromise = new Promise((_resolve, reject) => {
    rejectAbort = reject;
  });

  const invokeNativeCancel = async (record) => {
    try {
      await record.cancel();
    } catch (_error) {
    }
  };
  const requestNativeCancel = async (record) => {
    if (record.released) {
      return;
    }
    if (record.cancelPromise === null) {
      record.cancelPromise = invokeNativeCancel(record);
    }
    await record.cancelPromise;
  };
  const releaseNativeCancel = async (record, shouldCancel) => {
    if (record.released) {
      return;
    }
    if (shouldCancel) {
      await requestNativeCancel(record);
    }
    try {
      record.cancel.release();
    } catch (_error) {
    }
    record.released = true;
    nativeCancelRecords.delete(record.cancel);
  };
  const getNativeCancelRecord = (cancel) => {
    const existing = nativeCancelRecords.get(cancel);
    if (existing !== undefined) {
      return existing;
    }
    const record = {
      cancel,
      cancelPromise: null,
      released: false,
    };
    nativeCancelRecords.set(cancel, record);
    return record;
  };
  const onAbort = async () => {
    if (aborted) {
      return;
    }
    aborted = true;
    rejectAbort(createAbortReason(signal));
    const records = Array.from(nativeCancelRecords.values());
    for (const record of records) {
      await requestNativeCancel(record);
    }
  };
  signal.addEventListener("abort", onAbort, { once: true });

  const abortWatcher = async (cancel) => {
    const record = getNativeCancelRecord(cancel);
    if (settled) {
      await releaseNativeCancel(record, aborted || signal.aborted);
      return;
    }
    if (aborted || signal.aborted) {
      await requestNativeCancel(record);
    }
  };

  const invokeNative = async () => {
    await Promise.resolve();
    if (aborted || signal.aborted) {
      throw createAbortReason(signal);
    }
    return await nativeCall(abortWatcher);
  };
  const nativePromise = invokeNative();
  try {
    return await Promise.race([nativePromise, abortPromise]);
  } finally {
    settled = true;
    signal.removeEventListener("abort", onAbort);
    const shouldCancel = aborted || signal.aborted;
    const records = Array.from(nativeCancelRecords.values());
    for (const record of records) {
      await releaseNativeCancel(record, shouldCancel);
    }
  }
};

const parseNativeJson = async (source) => JSON.parse(await source);

const getOptionsObject = (options) => {
  if (options === undefined || options === null) {
    return {};
  }
  if (typeof options !== "object") {
    throw new TypeError("options must be an object");
  }
  return options;
};

const validateSafeUint = (value, name) => {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(name + " must be a non-negative safe integer");
  }
  return value;
};

const normalizeReadOptions = (options) => {
  const source = getOptionsObject(options);
  const target = {};
  if (source.position !== undefined && source.position !== null) {
    target.position = validateSafeUint(source.position, "options.position");
  }
  if (source.length !== undefined && source.length !== null) {
    target.length = validateSafeUint(source.length, "options.length");
  }
  return target;
};

const normalizeWriteOptions = (options) => {
  const source = getOptionsObject(options);
  const target = {};
  if (source.position !== undefined && source.position !== null) {
    target.position = validateSafeUint(source.position, "options.position");
  }
  return target;
};

const normalizeBooleanOptions = (options, keys) => {
  const source = getOptionsObject(options);
  const target = {};
  for (const key of keys) {
    if (source[key] !== undefined && source[key] !== null) {
      if (typeof source[key] !== "boolean") {
        throw new TypeError("options." + key + " must be a boolean");
      }
      target[key] = source[key];
    }
  }
  return target;
};

const normalizeAccessOptions = (options) => {
  const source = getOptionsObject(options);
  const target = {};
  if (source.mode !== undefined && source.mode !== null) {
    if (!Array.isArray(source.mode)) {
      throw new TypeError("options.mode must be an array");
    }
    target.mode = source.mode.map((entry) => {
      if (entry !== "read" && entry !== "write" && entry !== "execute") {
        throw new TypeError(
          "options.mode entries must be read, write, or execute",
        );
      }
      return entry;
    });
  }
  return target;
};

const addTypeMethods = (value) => {
  Object.defineProperties(value, {
    isFile: {
      value: () => value.type === "file",
    },
    isDirectory: {
      value: () => value.type === "directory",
    },
    isSymbolicLink: {
      value: () => value.type === "symlink",
    },
  });
  return value;
};

const parseStats = async (source) => addTypeMethods(await parseNativeJson(source));

const parseDirents = async (source, withFileTypes) => {
  const entries = await parseNativeJson(source);
  return withFileTypes ? entries.map(addTypeMethods) : entries;
};

const emitWatchEvent = (listener, event) => {
  try {
    Promise.resolve(listener(event)).catch(() => undefined);
  } catch (_error) {
  }
};

const normalizeWatchSnapshot = (snapshot) => {
  snapshot.root = addTypeMethods(snapshot.root);
  snapshot.entries = snapshot.entries.map(addTypeMethods).sort((a, b) =>
    a.name < b.name ? -1 : a.name > b.name ? 1 : 0,
  );
  return snapshot;
};

const statusKey = (entry) =>
  [entry.type, entry.size, entry.mtimeMs, entry.readonly].join(":");

const emitWatchDiff = (previous, next, listener) => {
  if (statusKey(previous.root) !== statusKey(next.root)) {
    emitWatchEvent(listener, { eventType: "change", filename: null });
  }
  const previousEntries = new Map(
    previous.entries.map((entry) => [entry.name, entry]),
  );
  const nextEntries = new Map(next.entries.map((entry) => [entry.name, entry]));
  for (const [name, entry] of previousEntries) {
    const nextEntry = nextEntries.get(name);
    if (nextEntry === undefined) {
      emitWatchEvent(listener, { eventType: "rename", filename: name });
    } else if (statusKey(entry) !== statusKey(nextEntry)) {
      emitWatchEvent(listener, { eventType: "change", filename: name });
    }
  }
  for (const name of nextEntries.keys()) {
    if (!previousEntries.has(name)) {
      emitWatchEvent(listener, { eventType: "rename", filename: name });
    }
  }
};

const properties = {};
if (isAllowed("readFile")) {
  properties.readFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) => {
        const nativeOptions = normalizeReadOptions(options);
        return namespace.__readFile(
          path,
          JSON.stringify(nativeOptions),
          abortWatcher,
        );
      }),
  };
}
if (isAllowed("writeFile")) {
  properties.writeFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, data, options) =>
      runAbortable(options, (abortWatcher) => {
        const nativeOptions = normalizeWriteOptions(options);
        return namespace.__writeFile(
          path,
          data,
          JSON.stringify(nativeOptions),
          abortWatcher,
        );
      }),
  };
}
if (isAllowed("readTextFile")) {
  properties.readTextFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, encoding, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__readTextFile(path, encoding, abortWatcher),
      ),
  };
}
if (isAllowed("writeTextFile")) {
  properties.writeTextFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, data, encoding, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__writeTextFile(path, data, encoding, abortWatcher),
      ),
  };
}
if (isAllowed("stat")) {
  properties.stat = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        parseStats(namespace.__stat(path, abortWatcher)),
      ),
  };
}
if (isAllowed("lstat")) {
  properties.lstat = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        parseStats(namespace.__lstat(path, abortWatcher)),
      ),
  };
}
if (isAllowed("exists")) {
  properties.exists = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__exists(path, abortWatcher),
      ),
  };
}
if (isAllowed("access")) {
  properties.access = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__access(
          path,
          JSON.stringify(normalizeAccessOptions(options)),
          abortWatcher,
        ),
      ),
  };
}
if (isAllowed("readdir")) {
  properties.readdir = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) => {
        const nativeOptions = normalizeBooleanOptions(options, [
          "withFileTypes",
        ]);
        return parseDirents(
          namespace.__readdir(
            path,
            JSON.stringify(nativeOptions),
            abortWatcher,
          ),
          nativeOptions.withFileTypes === true,
        );
      }),
  };
}
if (isAllowed("mkdir")) {
  properties.mkdir = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__mkdir(
          path,
          JSON.stringify(normalizeBooleanOptions(options, ["recursive"])),
          abortWatcher,
        ),
      ),
  };
}
if (isAllowed("rm")) {
  properties.rm = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__rm(
          path,
          JSON.stringify(normalizeBooleanOptions(options, [
            "recursive",
            "force",
          ])),
          abortWatcher,
        ),
      ),
  };
}
if (isAllowed("unlink")) {
  properties.unlink = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__unlink(path, abortWatcher),
      ),
  };
}
if (isAllowed("rmdir")) {
  properties.rmdir = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__rmdir(path, abortWatcher),
      ),
  };
}
if (isAllowed("rename")) {
  properties.rename = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (oldPath, newPath, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__rename(oldPath, newPath, abortWatcher),
      ),
  };
}
if (isAllowed("copyFile")) {
  properties.copyFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (source, destination, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__copyFile(
          source,
          destination,
          JSON.stringify(normalizeBooleanOptions(options, ["overwrite"])),
          abortWatcher,
        ),
      ),
  };
}
if (isAllowed("appendFile")) {
  properties.appendFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, data, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__appendFile(path, data, abortWatcher),
      ),
  };
}
if (isAllowed("appendTextFile")) {
  properties.appendTextFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, data, encoding, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__appendTextFile(path, data, encoding, abortWatcher),
      ),
  };
}
if (isAllowed("truncate")) {
  properties.truncate = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, length, options) => {
      let nativeLength = 0;
      let operationOptions = options;
      if (length !== undefined && length !== null && typeof length === "object") {
        operationOptions = length;
      } else if (length !== undefined && length !== null) {
        nativeLength = validateSafeUint(length, "length");
      }
      return runAbortable(operationOptions, (abortWatcher) =>
        namespace.__truncate(
          path,
          JSON.stringify({ length: nativeLength }),
          abortWatcher,
        ),
      );
    },
  };
}
if (isAllowed("realpath")) {
  properties.realpath = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__realpath(path, abortWatcher),
      ),
  };
}
if (isAllowed("readlink")) {
  properties.readlink = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (path, options) =>
      runAbortable(options, (abortWatcher) =>
        namespace.__readlink(path, abortWatcher),
      ),
  };
}
if (isAllowed("symlink")) {
  properties.symlink = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (target, path, type, options) => {
      let linkType = "file";
      let operationOptions = options;
      if (type !== undefined && type !== null && typeof type === "object") {
        operationOptions = type;
      } else if (type !== undefined && type !== null) {
        if (type !== "file" && type !== "dir" && type !== "junction") {
          throw new TypeError("type must be file, dir, or junction");
        }
        linkType = type;
      }
      return runAbortable(operationOptions, (abortWatcher) =>
        namespace.__symlink(target, path, linkType, abortWatcher),
      );
    },
  };
}
if (isAllowed("watch")) {
  properties.watch = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (path, listener, options) => {
      if (typeof listener !== "function") {
        throw new TypeError("listener must be a function");
      }
      const signal = getAbortSignal(options);
      if (signal !== null && signal.aborted) {
        throw createAbortReason(signal);
      }
      const runWatchRpc = async (request, operationOptions) =>
        await runAbortable(operationOptions, (abortWatcher) =>
          parseNativeJson(
            namespace.__watchRpc(JSON.stringify(request), 0, abortWatcher),
          ),
        );
      const lease = await runWatchRpc({ operation: "acquire" }, undefined);
      const leaseToken = lease.token;
      let released = false;
      const releaseLease = async () => {
        if (released) {
          return;
        }
        released = true;
        try {
          await runWatchRpc(
            { operation: "release", token: leaseToken },
            undefined,
          );
        } catch (_error) {}
      };
      if (signal !== null && signal.aborted) {
        await releaseLease();
        throw createAbortReason(signal);
      }
      let snapshot;
      try {
        snapshot = normalizeWatchSnapshot(
          await runWatchRpc(
            { operation: "snapshot", path, token: leaseToken },
            options,
          ),
        );
      } catch (error) {
        await releaseLease();
        throw error;
      }
      let closed = false;
      let polling = false;
      let pollPromise = null;
      let timer = null;
      let closePromise = null;
      const beginClose = async (waitForPoll) => {
        if (closePromise !== null) {
          await closePromise;
          return;
        }
        const pendingPoll = waitForPoll ? pollPromise : null;
        closePromise = (async () => {
          if (closed) {
            return;
          }
          closed = true;
          if (timer !== null) {
            clearInterval(timer);
            timer = null;
          }
          if (signal !== null) {
            signal.removeEventListener("abort", onAbort);
          }
          if (pendingPoll !== null) {
            try {
              await pendingPoll;
            } catch (_error) {}
          }
          await releaseLease();
        })();
        await closePromise;
      };
      const close = async () => {
        await beginClose(true);
      };
      const onAbort = () => {
        void close();
      };
      const runPoll = async () => {
        try {
          const next = normalizeWatchSnapshot(
            await runWatchRpc(
              { operation: "snapshot", path, token: leaseToken },
              undefined,
            ),
          );
          if (closed) {
            return;
          }
          emitWatchDiff(snapshot, next, listener);
          snapshot = next;
        } catch (error) {
          if (closed) {
            return;
          }
          emitWatchEvent(listener, {
            eventType: "error",
            filename: null,
            message: String(error && error.message ? error.message : error),
          });
          await beginClose(false);
        }
      };
      const poll = async () => {
        if (closed) {
          return;
        }
        if (polling) {
          return;
        }
        polling = true;
        const currentPoll = runPoll();
        pollPromise = currentPoll;
        try {
          await currentPoll;
        } finally {
          if (pollPromise === currentPoll) {
            pollPromise = null;
          }
          polling = false;
        }
      };
      timer = setInterval(() => {
        void poll();
      }, 100);
      if (signal !== null) {
        signal.addEventListener("abort", onAbort, { once: true });
        if (signal.aborted) {
          await close();
          throw createAbortReason(signal);
        }
      }
      return { close };
    },
  };
}
Object.defineProperties(namespace, properties);
)JS";

static const muon_plugin_function_metadata fs_functions[] = {
    {
        "__readFile",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_read_file),
        {3, read_file_args, &type_buffer_view},
        "readFile",
    },
    {
        "__writeFile",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_write_file),
        {4, write_file_args, &type_void},
        "writeFile",
    },
    {
        "__readTextFile",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_read_text_file),
        {3, read_text_file_args, &type_string},
        "readTextFile",
    },
    {
        "__writeTextFile",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_write_text_file),
        {4, write_text_file_args, &type_void},
        "writeTextFile",
    },
    {
        "__stat",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_stat),
        {2, path_abort_args, &type_string},
        "stat",
    },
    {
        "__lstat",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_lstat),
        {2, path_abort_args, &type_string},
        "lstat",
    },
    {
        "__exists",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_exists),
        {2, path_abort_args, &type_bool},
        "exists",
    },
    {
        "__access",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_access),
        {3, path_options_abort_args, &type_bool},
        "access",
    },
    {
        "__readdir",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_readdir),
        {3, path_options_abort_args, &type_string},
        "readdir",
    },
    {
        "__mkdir",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_mkdir),
        {3, path_options_abort_args, &type_void},
        "mkdir",
    },
    {
        "__rm",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_rm),
        {3, path_options_abort_args, &type_void},
        "rm",
    },
    {
        "__unlink",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_unlink),
        {2, path_abort_args, &type_void},
        "unlink",
    },
    {
        "__rmdir",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_rmdir),
        {2, path_abort_args, &type_void},
        "rmdir",
    },
    {
        "__rename",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_rename),
        {3, two_path_abort_args, &type_void},
        "rename",
    },
    {
        "__copyFile",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_copy_file),
        {4, two_path_options_abort_args, &type_void},
        "copyFile",
    },
    {
        "__appendFile",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_append_file),
        {3, append_file_args, &type_void},
        "appendFile",
    },
    {
        "__appendTextFile",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_append_text_file),
        {4, write_text_file_args, &type_void},
        "appendTextFile",
    },
    {
        "__truncate",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_truncate),
        {3, path_options_abort_args, &type_void},
        "truncate",
    },
    {
        "__realpath",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_realpath),
        {2, path_abort_args, &type_string},
        "realpath",
    },
    {
        "__readlink",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_readlink),
        {2, path_abort_args, &type_string},
        "readlink",
    },
    {
        "__symlink",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_symlink),
        {4, symlink_args, &type_void},
        "symlink",
    },
    {
        "__watchRpc",
        reinterpret_cast<muon_native_function>(&muon_builtin_fs_watch_rpc),
        {3, watch_rpc_args, &type_string},
        "watch",
    },
};

static const muon_plugin_function_metadata* const fs_function_pointers[] = {
    &fs_functions[0],
    &fs_functions[1],
    &fs_functions[2],
    &fs_functions[3],
    &fs_functions[4],
    &fs_functions[5],
    &fs_functions[6],
    &fs_functions[7],
    &fs_functions[8],
    &fs_functions[9],
    &fs_functions[10],
    &fs_functions[11],
    &fs_functions[12],
    &fs_functions[13],
    &fs_functions[14],
    &fs_functions[15],
    &fs_functions[16],
    &fs_functions[17],
    &fs_functions[18],
    &fs_functions[19],
    &fs_functions[20],
    &fs_functions[21],
    nullptr,
};

}  // namespace muon_internal

const muon_plugin_namespace kMuonBuiltinFsNamespace = {
    "muon.fs",
    muon_internal::fs_setup_script,
    muon_internal::fs_function_pointers,
};

static const muon_plugin_namespace* const fs_namespaces[] = {
    &kMuonBuiltinFsNamespace,
    nullptr,
};

static const muon_plugin_metadata fs_metadata = {
    fs_namespaces,
};

bool InitializeMuonBuiltinFs(const muon_plugin_init_context* context,
                              std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  const auto* helpers = context == nullptr ? nullptr : context->helpers;
  auto read_file_max_bytes = uint64_t{0};
  const auto* configured_max = muon_plugin_get_config_value(
      context, muon_internal::kMuonFsReadFileMaxBytesConfigKey);
  if (!muon_internal::ParseMuonFsReadFileMaxBytes(
          configured_max, &read_file_max_bytes, error_message)) {
    return false;
  }
  auto read_text_file_max_bytes = uint64_t{0};
  const auto* configured_text_max = muon_plugin_get_config_value(
      context, muon_internal::kMuonFsReadTextFileMaxBytesConfigKey);
  if (!muon_internal::ParseMuonFsReadTextFileMaxBytes(
          configured_text_max, &read_text_file_max_bytes, error_message)) {
    return false;
  }
  auto runtime = std::make_shared<muon_internal::MuonBuiltinFsRuntime>(
      helpers, read_file_max_bytes, read_text_file_max_bytes);
  if (!runtime->Start(error_message)) {
    return false;
  }
  muon_internal::g_runtime = runtime;
  return true;
}

void ShutdownMuonBuiltinFs() {
  const auto runtime = muon_internal::g_runtime;
  if (!runtime) {
    return;
  }
  runtime->Stop();
  if (!runtime->HasPendingOperations()) {
    muon_internal::g_runtime.reset();
  }
}

void ReleaseMuonBuiltinFsContext(int renderer_context_id) {
  const auto runtime = muon_internal::g_runtime;
  if (!runtime) {
    return;
  }
  runtime->ReleaseWatchContext(renderer_context_id);
}

const muon_plugin_metadata* GetMuonBuiltinFsPluginMetadata() {
  return &fs_metadata;
}
