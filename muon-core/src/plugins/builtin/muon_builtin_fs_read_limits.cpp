/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_helpers.h"

#include <algorithm>
#include <charconv>
#include <string>

namespace muon_internal {

static bool ParseMuonFsMaxBytes(const char* value,
                                uint64_t default_max_bytes,
                                std::string_view config_key,
                                uint64_t* max_bytes,
                                std::string* error_message) {
  if (value == nullptr) {
    *max_bytes = default_max_bytes;
    return true;
  }
  const auto length = std::char_traits<char>::length(value);
  if (length == 0 ||
      !std::all_of(value, value + length, [](char item) {
        return item >= '0' && item <= '9';
      })) {
    *error_message = std::string(config_key) +
                     " must be an unsigned decimal byte count";
    return false;
  }
  auto parsed = uint64_t{0};
  const auto result = std::from_chars(value, value + length, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != value + length) {
    *error_message = std::string(config_key) +
                     " must be an unsigned decimal byte count";
    return false;
  }
  *max_bytes = parsed;
  return true;
}

bool ParseMuonFsReadFileMaxBytes(const char* value,
                                 uint64_t* max_bytes,
                                 std::string* error_message) {
  return ParseMuonFsMaxBytes(value,
                             kMuonFsDefaultReadFileMaxBytes,
                             kMuonFsReadFileMaxBytesConfigKey,
                             max_bytes,
                             error_message);
}

bool ParseMuonFsReadTextFileMaxBytes(const char* value,
                                     uint64_t* max_bytes,
                                     std::string* error_message) {
  return ParseMuonFsMaxBytes(value,
                             kMuonFsDefaultReadTextFileMaxBytes,
                             kMuonFsReadTextFileMaxBytesConfigKey,
                             max_bytes,
                             error_message);
}

bool ValidateMuonFsReadFileLength(const MuonFsReadOptions& options,
                                  uint64_t max_bytes,
                                  std::string* error_message) {
  if (options.has_length && options.length > max_bytes) {
    *error_message = "readFile length exceeds configured maximum";
    return false;
  }
  return true;
}

bool ResolveMuonFsReadFileRange(const MuonFsReadOptions& options,
                                uint64_t file_size,
                                uint64_t max_bytes,
                                MuonFsReadRange* range,
                                std::string* error_message) {
  if (!ValidateMuonFsReadFileLength(options, max_bytes, error_message)) {
    return false;
  }
  const auto position = options.has_position ? options.position : uint64_t{0};
  const auto available = position >= file_size
                             ? uint64_t{0}
                             : file_size - position;
  const auto length = options.has_length ? std::min(options.length, available)
                                         : available;
  if (length > max_bytes) {
    *error_message = "readFile result exceeds configured maximum";
    return false;
  }
  range->position = position;
  range->length = length;
  return true;
}

}  // namespace muon_internal
