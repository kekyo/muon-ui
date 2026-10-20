/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
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

/** Default per-operation byte limit for readFile. */
inline constexpr uint64_t kMuonFsDefaultReadFileMaxBytes = uint64_t{67108864};

/** Internal plugin configuration key for the readFile byte limit. */
inline constexpr char kMuonFsReadFileMaxBytesConfigKey[] =
    "fs.readFile.maxBytes";

/** Default per-operation byte limit for readTextFile. */
inline constexpr uint64_t kMuonFsDefaultReadTextFileMaxBytes =
    uint64_t{67108864};

/** Internal plugin configuration key for the readTextFile byte limit. */
inline constexpr char kMuonFsReadTextFileMaxBytesConfigKey[] =
    "fs.readTextFile.maxBytes";

/** Maximum source read size for each readTextFile stream operation. */
inline constexpr size_t kMuonFsReadTextFileChunkBytes = size_t{65536};

/** Stable error returned when readTextFile exceeds its byte limit. */
inline constexpr char kMuonFsReadTextFileLimitError[] =
    "readTextFile result exceeds configured maximum";

struct MuonFsReadOptions {
  bool has_position = false;
  uint64_t position = 0;
  bool has_length = false;
  uint64_t length = 0;
};

/** Resolved readFile range after applying the source size. */
struct MuonFsReadRange {
  /** Source offset in bytes. */
  uint64_t position = 0;
  /** Number of bytes requested from the source. */
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
/**
 * Parses the internal readFile byte-limit configuration value.
 *
 * @param value Decimal byte count, or null when the key is unspecified.
 * @param max_bytes Receives the parsed limit.
 * @param error_message Receives a validation error on failure.
 * @return True when the value is valid.
 */
bool ParseMuonFsReadFileMaxBytes(const char* value,
                                 uint64_t* max_bytes,
                                 std::string* error_message);

/**
 * Parses the internal readTextFile byte-limit configuration value.
 *
 * @param value Decimal byte count, or null when the key is unspecified.
 * @param max_bytes Receives the parsed limit.
 * @param error_message Receives a validation error on failure.
 * @return True when the value is valid.
 */
bool ParseMuonFsReadTextFileMaxBytes(const char* value,
                                     uint64_t* max_bytes,
                                     std::string* error_message);
/**
 * Validates an explicit readFile length before accessing the source.
 *
 * @param options Requested read options.
 * @param max_bytes Configured per-operation byte limit.
 * @param error_message Receives a validation error on failure.
 * @return True when metadata access may proceed.
 */
bool ValidateMuonFsReadFileLength(const MuonFsReadOptions& options,
                                  uint64_t max_bytes,
                                  std::string* error_message);
/**
 * Resolves a readFile range and enforces the configured result limit.
 *
 * @param options Requested read options.
 * @param file_size Source size in bytes.
 * @param max_bytes Configured per-operation byte limit.
 * @param range Receives the bounded source range.
 * @param error_message Receives a validation error on failure.
 * @return True when the range is valid.
 */
bool ResolveMuonFsReadFileRange(const MuonFsReadOptions& options,
                                uint64_t file_size,
                                uint64_t max_bytes,
                                MuonFsReadRange* range,
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

std::string NormalizeLocalPathOrFileUri(std::string_view value);
std::filesystem::path CreateLocalFilesystemPath(std::string_view value);
/** Native I/O requirements, separate from URI decoding and symlink payloads. */
enum class LocalFilesystemIoUsage {
  /** An individual file, using extended syntax only when necessary. */
  File,
  /** Directory creation or traversal, preserving exact child names. */
  Tree,
};
/**
 * Resolves a local path or file URI once for a native filesystem operation.
 * @param value UTF-8 caller input, including a supported file URI.
 * @param usage Requirements of the operation receiving the path.
 * @return Owned native path; on Windows, normalized and extended as needed.
 * @throws std::system_error When the Windows path cannot be converted.
 * @remarks Does not require existence. Do not use for relative symlink targets,
 * public return values, process arguments, or shell/CEF configuration.
 */
std::filesystem::path CreateLocalFilesystemIoPath(
    std::string_view value, LocalFilesystemIoUsage usage);
std::string PathToUtf8String(const std::filesystem::path& path);
/**
 * Formats a native result without changing the filesystem object it denotes.
 * @param path Native result, including Windows extended or NT link syntax.
 * @param preserve_extended Whether the caller explicitly requested extended syntax.
 * @return UTF-8 path; Windows prefixes are omitted only for equivalent ordinary names.
 * @remarks Relative link targets remain relative. Exact trailing dots, spaces and
 * DOS-device filenames retain their extended namespace.
 */
std::string PathToPublicUtf8String(const std::filesystem::path& path,
                                   bool preserve_extended);
/**
 * Formats an operation error with a path and the original failure detail.
 * @param action Filesystem operation name.
 * @param path Native path identifying the failing object.
 * @param detail Failure description, including the native error number.
 * @return Diagnostic text with Windows paths shortened to fit the callback ABI.
 * @remarks Retains the beginning and end of a long path, cutting only at UTF-8
 * character boundaries, so the callback limit does not discard the error code.
 */
std::string FormatFilesystemError(const char* action,
                                   const std::filesystem::path& path,
                                   std::string_view detail);
/**
 * Reads the type and CRT-compatible permission flags of a native I/O path.
 * @param path Path already converted for the receiving filesystem operation.
 * @param follow_symlink Whether to describe a link's target instead of the link.
 * @return File type and permission flags without CRT path-length restrictions.
 * @throws std::runtime_error When the object cannot be inspected.
 */
std::filesystem::file_status ReadFilesystemStatus(
    const std::filesystem::path& path, bool follow_symlink);
std::string CreateStatusJson(const std::filesystem::path& path,
                             bool follow_symlink);
#if defined(_WIN32)
/**
 * Serializes a Windows directory entry obtained by native enumeration.
 * @param path Exact child path, including its extended-length parent.
 * @return Entry name and metadata, describing a link rather than its target.
 * @throws std::runtime_error When entry metadata cannot be read.
 */
std::string CreateDirentJson(const std::filesystem::path& path);
#endif
void ThrowFilesystemError(const char* action,
                          const std::filesystem::path& path,
                          const std::error_code& error);
void ThrowIfNotRegularFile(const std::filesystem::path& path,
                           std::string_view message);
bool CanAccessPath(const std::filesystem::path& path,
                   const MuonFsAccessOptions& options);

}  // namespace muon_internal
