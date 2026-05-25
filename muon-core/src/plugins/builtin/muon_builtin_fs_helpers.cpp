/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_helpers.h"

#include "yyjson.h"

#include <cardio.h>

#include <chrono>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace muon_internal {

static constexpr char kMuonBuiltinFsShutdownError[] =
    "Muon filesystem runtime is shutting down";
static constexpr char kMuonBuiltinFsGenericError[] =
    "Filesystem operation failed";

bool ContainsNul(const std::string& value) {
  return value.find('\0') != std::string::npos;
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

static std::string ReadJsonString(yyjson_val* value) {
  return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

static void AppendJsonHex(std::string* target, uint8_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  target->push_back(kHex[(value >> 4) & 0x0f]);
  target->push_back(kHex[value & 0x0f]);
}

void AppendJsonString(std::string* target, std::string_view value) {
  target->push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<uint8_t>(character);
    switch (character) {
      case '"':
        *target += "\\\"";
        break;
      case '\\':
        *target += "\\\\";
        break;
      case '\b':
        *target += "\\b";
        break;
      case '\f':
        *target += "\\f";
        break;
      case '\n':
        *target += "\\n";
        break;
      case '\r':
        *target += "\\r";
        break;
      case '\t':
        *target += "\\t";
        break;
      default:
        if (byte < 0x20) {
          *target += "\\u00";
          AppendJsonHex(target, byte);
        } else {
          target->push_back(character);
        }
        break;
    }
  }
  target->push_back('"');
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

void ThrowFilesystemError(const char* action,
                          const std::filesystem::path& path,
                          const std::error_code& error) {
  if (!error) {
    return;
  }
  throw std::runtime_error(
      std::string(action) + " failed for " + PathToUtf8String(path) +
      ": " + error.message());
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
  ThrowFilesystemError("stat", path, error);
  if (status.type() == std::filesystem::file_type::not_found) {
    throw std::runtime_error("Path does not exist");
  }
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

class MuonJsonDocument final {
 public:
  explicit MuonJsonDocument(yyjson_doc* source)
      : document_(source) {}

  ~MuonJsonDocument() {
    if (document_ != nullptr) {
      yyjson_doc_free(document_);
    }
  }

  MuonJsonDocument(const MuonJsonDocument&) = delete;
  MuonJsonDocument& operator=(const MuonJsonDocument&) = delete;

  MuonJsonDocument(MuonJsonDocument&& other) noexcept
      : document_(other.document_) {
    other.document_ = nullptr;
  }

  MuonJsonDocument& operator=(MuonJsonDocument&& other) noexcept {
    if (this != &other) {
      if (document_ != nullptr) {
        yyjson_doc_free(document_);
      }
      document_ = other.document_;
      other.document_ = nullptr;
    }
    return *this;
  }

  yyjson_doc* get() const {
    return document_;
  }

 private:
  yyjson_doc* document_ = nullptr;
};

static bool ParseOptionsJson(const char* options_json,
                             MuonJsonDocument* document,
                             yyjson_val** root,
                             std::string* error_message) {
  if (options_json == nullptr) {
    *error_message = "Options JSON is required";
    return false;
  }
  yyjson_read_err read_error = {};
  auto* raw_document = yyjson_read_opts(
      const_cast<char*>(options_json), std::strlen(options_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (raw_document == nullptr) {
    *error_message = "Options JSON is invalid";
    return false;
  }
  *document = MuonJsonDocument(raw_document);
  *root = yyjson_doc_get_root(document->get());
  if (!yyjson_is_obj(*root)) {
    *error_message = "Options JSON root must be an object";
    return false;
  }
  return true;
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
  return ParseOptionsJson(options_json, &document, &root, error_message) &&
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
  return ParseOptionsJson(options_json, &document, &root, error_message) &&
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
  return ParseOptionsJson(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "withFileTypes", false,
                          &options->with_file_types, error_message);
}

bool ParseMkdirOptions(const char* options_json,
                       MuonFsMkdirOptions* options,
                       std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseOptionsJson(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "recursive", false, &options->recursive,
                          error_message);
}

bool ParseRmOptions(const char* options_json,
                    MuonFsRmOptions* options,
                    std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  return ParseOptionsJson(options_json, &document, &root, error_message) &&
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
  return ParseOptionsJson(options_json, &document, &root, error_message) &&
         ReadOptionalBool(root, "overwrite", true, &options->overwrite,
                          error_message);
}

bool ParseAccessOptions(const char* options_json,
                        MuonFsAccessOptions* options,
                        std::string* error_message) {
  auto document = MuonJsonDocument(nullptr);
  yyjson_val* root = nullptr;
  if (!ParseOptionsJson(options_json, &document, &root, error_message)) {
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
  if (!ParseOptionsJson(options_json, &document, &root, error_message)) {
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
