/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <gio/gio.h>
#endif

namespace muon_internal {

struct MuonFsReadOptions {
  bool has_position = false;
  uint64_t position = 0;
  bool has_length = false;
  uint64_t length = 0;
};

struct MuonFsWriteOptions {
  bool has_position = false;
  uint64_t position = 0;
};

struct MuonFsReaddirOptions {
  bool with_file_types = false;
};

struct MuonFsMkdirOptions {
  bool recursive = false;
};

struct MuonFsRmOptions {
  bool recursive = false;
  bool force = false;
};

struct MuonFsCopyOptions {
  bool overwrite = true;
};

struct MuonFsAccessOptions {
  bool read = false;
  bool write = false;
  bool execute = false;
};

bool ContainsNul(const std::string& value);
#if !defined(_WIN32)
bool LooksLikeUri(std::string_view value);
GFile* CreateGFileFromPathOrUri(const std::string& value);
#endif
bool IsSupportedUtf8Encoding(const char* encoding);
bool IsValidUtf8WithoutNul(const uint8_t* data, size_t size);
std::string CurrentExceptionMessage();

bool ParseReadOptions(const char* options_json,
                      MuonFsReadOptions* options,
                      std::string* error_message);
bool ParseWriteOptions(const char* options_json,
                       MuonFsWriteOptions* options,
                       std::string* error_message);
bool ParseReaddirOptions(const char* options_json,
                         MuonFsReaddirOptions* options,
                         std::string* error_message);
bool ParseMkdirOptions(const char* options_json,
                       MuonFsMkdirOptions* options,
                       std::string* error_message);
bool ParseRmOptions(const char* options_json,
                    MuonFsRmOptions* options,
                    std::string* error_message);
bool ParseCopyOptions(const char* options_json,
                      MuonFsCopyOptions* options,
                      std::string* error_message);
bool ParseAccessOptions(const char* options_json,
                        MuonFsAccessOptions* options,
                        std::string* error_message);
bool ParseTruncateLength(const char* options_json,
                         uint64_t* length,
                         std::string* error_message);

std::string PathToUtf8String(const std::filesystem::path& path);
std::string CreateStatusJson(const std::filesystem::path& path,
                             bool follow_symlink);
std::string CreateDirentJson(
    const std::filesystem::directory_entry& entry);
void ThrowFilesystemError(const char* action,
                          const std::filesystem::path& path,
                          const std::error_code& error);
void ThrowIfNotRegularFile(const std::filesystem::path& path,
                           std::string_view message);
bool CanAccessPath(const std::filesystem::path& path,
                   const MuonFsAccessOptions& options);

}  // namespace muon_internal
