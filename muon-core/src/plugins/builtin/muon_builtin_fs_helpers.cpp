/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/builtin/muon_builtin_fs_helpers.h"

#include "muon_json_helpers.h"
#include "yyjson.h"

#include <cardio.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace muon_internal {

static constexpr char kMuonBuiltinFsShutdownError[] =
    "muon filesystem runtime is shutting down";
static constexpr char kMuonBuiltinFsGenericError[] =
    "Filesystem operation failed";

bool ContainsNul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

static int DecodeAsciiHex(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

static bool StartsWithFileScheme(std::string_view value) {
  constexpr auto scheme = std::string_view{"file:"};
  if (value.size() < scheme.size()) {
    return false;
  }
  for (auto index = size_t{0}; index < scheme.size(); ++index) {
    const auto left = static_cast<unsigned char>(value[index]);
    const auto right = static_cast<unsigned char>(scheme[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

static std::string ToLowerAsciiString(std::string_view value) {
  auto result = std::string(value);
  std::transform(result.begin(), result.end(), result.begin(), [](char item) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(item)));
  });
  return result;
}

static std::string DecodeFileUriPath(std::string_view value) {
  auto result = std::string{};
  result.reserve(value.size());
  for (auto index = size_t{0}; index < value.size(); ++index) {
    if (value[index] != '%') {
      result.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) {
      throw std::runtime_error("File URI percent encoding is invalid");
    }
    const auto upper = DecodeAsciiHex(value[index + 1]);
    const auto lower = DecodeAsciiHex(value[index + 2]);
    if (upper < 0 || lower < 0) {
      throw std::runtime_error("File URI percent encoding is invalid");
    }
    result.push_back(static_cast<char>((upper << 4) | lower));
    index += 2;
  }
  if (ContainsNul(result) ||
      !IsValidUtf8WithoutNul(
          reinterpret_cast<const uint8_t*>(result.data()),
          result.size())) {
    throw std::runtime_error("File URI path is not valid UTF-8");
  }
  return result;
}

#if !defined(_WIN32)
bool LooksLikeUri(std::string_view value) {
  return value.find("://") != std::string_view::npos ||
         value.rfind("file:", 0) == 0;
}

GFile* CreateGFileFromPathOrUri(const std::string& value) {
  return LooksLikeUri(value) ? g_file_new_for_uri(value.c_str())
                             : g_file_new_for_path(value.c_str());
}
#endif

#if defined(_WIN32)
static void ReplacePathSeparators(std::string* path, char separator) {
  std::replace(path->begin(), path->end(), '/', separator);
  std::replace(path->begin(), path->end(), '\\', separator);
}
#endif

std::string NormalizeLocalPathOrFileUri(std::string_view value) {
  if (!StartsWithFileScheme(value)) {
    return std::string(value);
  }

  auto rest = value.substr(5);
  auto authority = std::string_view{};
  auto path_part = std::string_view{};
  if (rest.rfind("//", 0) == 0) {
    rest.remove_prefix(2);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) {
      authority = rest;
      path_part = std::string_view{};
    } else {
      authority = rest.substr(0, slash);
      path_part = rest.substr(slash);
    }
  } else {
    path_part = rest;
  }

  const auto decoded_path = DecodeFileUriPath(path_part);
  const auto lowercase_authority = ToLowerAsciiString(authority);
  const auto has_local_authority =
      authority.empty() || lowercase_authority == "localhost";
#if defined(_WIN32)
  auto result = decoded_path;
  if (!has_local_authority) {
    while (!result.empty() && (result.front() == '/' ||
                              result.front() == '\\')) {
      result.erase(result.begin());
    }
    ReplacePathSeparators(&result, '\\');
    return "\\\\" + std::string(authority) +
           (result.empty() ? std::string{} : "\\" + result);
  }
  if (result.size() >= 3 && result[0] == '/' &&
      std::isalpha(static_cast<unsigned char>(result[1])) &&
      result[2] == ':') {
    result.erase(result.begin());
  }
  ReplacePathSeparators(&result, '\\');
  return result;
#else
  if (!has_local_authority) {
    throw std::runtime_error("File URI host is not supported");
  }
  return decoded_path;
#endif
}

#if defined(_WIN32)
static std::wstring Utf8ToWidePath(const std::string& value) {
  if (value.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("Path is too long");
  }
  if (value.empty()) {
    return std::wstring{};
  }
  const auto source_size = static_cast<int>(value.size());
  const auto length = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_size, nullptr, 0);
  if (length <= 0) {
    throw std::runtime_error("Path is not valid UTF-8");
  }
  auto result = std::wstring(static_cast<size_t>(length), L'\0');
  const auto converted = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_size,
      result.data(), length);
  if (converted != length) {
    throw std::runtime_error("Path conversion failed");
  }
  return result;
}
#endif

std::filesystem::path CreateLocalFilesystemPath(std::string_view value) {
  auto normalized = NormalizeLocalPathOrFileUri(value);
#if defined(_WIN32)
  return std::filesystem::path(Utf8ToWidePath(normalized));
#else
  return std::filesystem::path(normalized);
#endif
}

std::string PathToUtf8String(const std::filesystem::path& path) {
  const auto value = path.u8string();
#if defined(__cpp_char8_t)
  return std::string(reinterpret_cast<const char*>(value.data()),
                     value.size());
#else
  return value;
#endif
}

static int64_t FileTimeToUnixMillis(
    std::filesystem::file_time_type file_time) {
  const auto system_time = std::chrono::time_point_cast<
      std::chrono::milliseconds>(
      file_time - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now());
  return system_time.time_since_epoch().count();
}

static std::string FileTypeName(std::filesystem::file_type type) {
  switch (type) {
    case std::filesystem::file_type::regular:
      return "file";
    case std::filesystem::file_type::directory:
      return "directory";
    case std::filesystem::file_type::symlink:
      return "symlink";
    case std::filesystem::file_type::block:
      return "blockDevice";
    case std::filesystem::file_type::character:
      return "characterDevice";
    case std::filesystem::file_type::fifo:
      return "fifo";
    case std::filesystem::file_type::socket:
      return "socket";
    default:
      return "other";
  }
}

static bool IsReadonly(std::filesystem::perms permissions) {
  constexpr auto write_permissions =
      std::filesystem::perms::owner_write |
      std::filesystem::perms::group_write |
      std::filesystem::perms::others_write;
  return (permissions & write_permissions) == std::filesystem::perms::none;
}

static std::string FilesystemErrorMessage(const std::error_code& error) {
#if defined(_WIN32)
  return "Windows filesystem error " + std::to_string(error.value());
#else
  return error.message();
#endif
}

void ThrowFilesystemError(const char* action,
                          const std::filesystem::path& path,
                          const std::error_code& error) {
  if (!error) {
    return;
  }
  throw std::runtime_error(
      std::string(action) + " failed for " + PathToUtf8String(path) +
      ": " + FilesystemErrorMessage(error));
}

void ThrowIfNotRegularFile(const std::filesystem::path& path,
                           std::string_view message) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  ThrowFilesystemError("stat", path, error);
  if (!std::filesystem::is_regular_file(status)) {
    throw std::runtime_error(std::string(message));
  }
}

static uintmax_t ReadFileSize(const std::filesystem::path& path,
                              const std::filesystem::file_status& status) {
  if (!std::filesystem::is_regular_file(status)) {
    return 0;
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  ThrowFilesystemError("file_size", path, error);
  return size;
}

std::string CreateStatusJson(const std::filesystem::path& path,
                             bool follow_symlink) {
  std::error_code error;
  const auto status = follow_symlink ? std::filesystem::status(path, error)
                                     : std::filesystem::symlink_status(path,
                                                                       error);
  if (status.type() == std::filesystem::file_type::not_found) {
    throw std::runtime_error("Path does not exist");
  }
  ThrowFilesystemError("stat", path, error);
  const auto size = ReadFileSize(path, status);
  const auto last_write_time = std::filesystem::last_write_time(path, error);
  ThrowFilesystemError("last_write_time", path, error);

  auto result = std::string("{\"type\":");
  AppendJsonString(&result, FileTypeName(status.type()));
  result += ",\"size\":";
  result += std::to_string(size);
  result += ",\"mtimeMs\":";
  result += std::to_string(FileTimeToUnixMillis(last_write_time));
  result += ",\"readonly\":";
  result += IsReadonly(status.permissions()) ? "true" : "false";
  result += "}";
  return result;
}

std::string CreateDirentJson(
    const std::filesystem::directory_entry& entry) {
  std::error_code error;
  const auto status = entry.symlink_status(error);
  ThrowFilesystemError("stat", entry.path(), error);
  auto result = std::string("{\"name\":");
  AppendJsonString(&result, PathToUtf8String(entry.path().filename()));
  result += ",\"type\":";
  AppendJsonString(&result, FileTypeName(status.type()));
  result += ",\"size\":";
  result += std::to_string(ReadFileSize(entry.path(), status));
  result += ",\"mtimeMs\":";
  const auto last_write_time = entry.last_write_time(error);
  ThrowFilesystemError("last_write_time", entry.path(), error);
  result += std::to_string(FileTimeToUnixMillis(last_write_time));
  result += ",\"readonly\":";
  result += IsReadonly(status.permissions()) ? "true" : "false";
  result += "}";
  return result;
}

static bool ReadOptionalBool(yyjson_val* object,
                             const char* key,
                             bool default_value,
                             bool* target,
                             std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    *target = default_value;
    return true;
  }
  if (!yyjson_is_bool(value)) {
    *error_message = std::string(key) + " must be a boolean";
    return false;
  }
  *target = yyjson_get_bool(value);
  return true;
}

static bool ReadOptionalUint64(yyjson_val* object,
                               const char* key,
                               bool* has_value,
                               uint64_t* target,
                               std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    *has_value = false;
    *target = 0;
    return true;
  }
  if (!yyjson_is_uint(value)) {
    *error_message = std::string(key) + " must be a non-negative integer";
    return false;
  }
  *has_value = true;
  *target = yyjson_get_uint(value);
  return true;
}

bool ParseReadOptions(const char* options_json,
                      MuonFsReadOptions* options,
                      std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseJsonObjectOptions(options_json, &document, &root, error_message) &&
         ReadOptionalUint64(root, "position", &options->has_position,
                            &options->position, error_message) &&
         ReadOptionalUint64(root, "length", &options->has_length,
                            &options->length, error_message);
}

bool ParseWriteOptions(const char* options_json,
                       MuonFsWriteOptions* options,
                       std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseJsonObjectOptions(options_json, &document, &root, error_message) &&
         ReadOptionalUint64(root, "position", &options->has_position,
                            &options->position, error_message);
}

bool IsSupportedUtf8Encoding(const char* encoding) {
  if (encoding == nullptr) {
    return false;
  }
  return std::strcmp(encoding, "utf8") == 0 ||
         std::strcmp(encoding, "utf-8") == 0;
}

static bool IsUtf8Continuation(uint8_t value) {
  return value >= 0x80u && value <= 0xbfu;
}

bool IsValidUtf8WithoutNul(const uint8_t* data, size_t size) {
  if (size > 0 && data == nullptr) {
    return false;
  }

  auto index = size_t{0};
  while (index < size) {
    const auto first = data[index];
    if (first == 0u) {
      return false;
    }
    if (first <= 0x7fu) {
      index += 1;
      continue;
    }
    if (first >= 0xc2u && first <= 0xdfu) {
      if (index + 1 >= size || !IsUtf8Continuation(data[index + 1])) {
        return false;
      }
      index += 2;
      continue;
    }
    if (first == 0xe0u) {
      if (index + 2 >= size || data[index + 1] < 0xa0u ||
          data[index + 1] > 0xbfu || !IsUtf8Continuation(data[index + 2])) {
        return false;
      }
      index += 3;
      continue;
    }
    if ((first >= 0xe1u && first <= 0xecu) ||
        (first >= 0xeeu && first <= 0xefu)) {
      if (index + 2 >= size || !IsUtf8Continuation(data[index + 1]) ||
          !IsUtf8Continuation(data[index + 2])) {
        return false;
      }
      index += 3;
      continue;
    }
    if (first == 0xedu) {
      if (index + 2 >= size || data[index + 1] < 0x80u ||
          data[index + 1] > 0x9fu || !IsUtf8Continuation(data[index + 2])) {
        return false;
      }
      index += 3;
      continue;
    }
    if (first == 0xf0u) {
      if (index + 3 >= size || data[index + 1] < 0x90u ||
          data[index + 1] > 0xbfu || !IsUtf8Continuation(data[index + 2]) ||
          !IsUtf8Continuation(data[index + 3])) {
        return false;
      }
      index += 4;
      continue;
    }
    if (first >= 0xf1u && first <= 0xf3u) {
      if (index + 3 >= size || !IsUtf8Continuation(data[index + 1]) ||
          !IsUtf8Continuation(data[index + 2]) ||
          !IsUtf8Continuation(data[index + 3])) {
        return false;
      }
      index += 4;
      continue;
    }
    if (first == 0xf4u) {
      if (index + 3 >= size || data[index + 1] < 0x80u ||
          data[index + 1] > 0x8fu || !IsUtf8Continuation(data[index + 2]) ||
          !IsUtf8Continuation(data[index + 3])) {
        return false;
      }
      index += 4;
      continue;
    }
    return false;
  }
  return true;
}

std::string CurrentExceptionMessage() {
  try {
    throw;
  } catch (const cardio::canceled_exception&) {
    return kMuonBuiltinFsShutdownError;
  } catch (const std::exception& error) {
    return error.what() == nullptr || error.what()[0] == '\0'
               ? kMuonBuiltinFsGenericError
               : error.what();
  } catch (...) {
    return kMuonBuiltinFsGenericError;
  }
}

bool ParseReaddirOptions(const char* options_json,
                         MuonFsReaddirOptions* options,
                         std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseJsonObjectOptions(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "withFileTypes", false,
                          &options->with_file_types, error_message);
}

bool ParseMkdirOptions(const char* options_json,
                       MuonFsMkdirOptions* options,
                       std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseJsonObjectOptions(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "recursive", false, &options->recursive,
                          error_message);
}

bool ParseRmOptions(const char* options_json,
                    MuonFsRmOptions* options,
                    std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseJsonObjectOptions(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "recursive", false, &options->recursive,
                          error_message) &&
         ReadOptionalBool(root, "force", false, &options->force,
                          error_message);
}

bool ParseCopyOptions(const char* options_json,
                      MuonFsCopyOptions* options,
                      std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseJsonObjectOptions(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "overwrite", true, &options->overwrite,
                          error_message);
}

bool ParseAccessOptions(const char* options_json,
                        MuonFsAccessOptions* options,
                        std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  if (!ParseJsonObjectOptions(options_json, &document, &root, error_message)) {
    return false;
  }
  const auto mode = yyjson_obj_get(root, "mode");
  if (mode == nullptr || yyjson_is_null(mode)) {
    return true;
  }
  if (!yyjson_is_arr(mode)) {
    *error_message = "mode must be an array";
    return false;
  }
  yyjson_val* entry = nullptr;
  size_t index = 0;
  size_t max = 0;
  yyjson_arr_foreach(mode, index, max, entry) {
    if (!yyjson_is_str(entry)) {
      *error_message = "mode entries must be strings";
      return false;
    }
    const auto value = ReadJsonString(entry);
    if (value == "read") {
      options->read = true;
    } else if (value == "write") {
      options->write = true;
    } else if (value == "execute") {
      options->execute = true;
    } else {
      *error_message = "mode entries must be read, write, or execute";
      return false;
    }
  }
  return true;
}

bool ParseTruncateLength(const char* options_json,
                         uint64_t* length,
                         std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  if (!ParseJsonObjectOptions(options_json, &document, &root, error_message)) {
    return false;
  }
  auto has_length = false;
  if (!ReadOptionalUint64(root, "length", &has_length, length,
                          error_message)) {
    return false;
  }
  (void)has_length;
  return true;
}

static bool HasAnyPermission(std::filesystem::perms permissions,
                             std::filesystem::perms requested) {
  return (permissions & requested) != std::filesystem::perms::none;
}

bool CanAccessPath(const std::filesystem::path& path,
                   const MuonFsAccessOptions& options) {
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error || status.type() == std::filesystem::file_type::not_found) {
    return false;
  }
  const auto permissions = status.permissions();
  if (options.read &&
      !HasAnyPermission(permissions,
                        std::filesystem::perms::owner_read |
                            std::filesystem::perms::group_read |
                            std::filesystem::perms::others_read)) {
    return false;
  }
  if (options.write &&
      !HasAnyPermission(permissions,
                        std::filesystem::perms::owner_write |
                            std::filesystem::perms::group_write |
                            std::filesystem::perms::others_write)) {
    return false;
  }
  if (options.execute &&
      !HasAnyPermission(permissions,
                        std::filesystem::perms::owner_exec |
                            std::filesystem::perms::group_exec |
                            std::filesystem::perms::others_exec)) {
    return false;
  }
  return true;
}

}  // namespace muon_internal
