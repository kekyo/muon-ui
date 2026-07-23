/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace muon_internal {

/**
 * Calculates the lowercase SHA-256 hexadecimal digest of a file followed by a
 * suffix.
 *
 * @param path File whose bytes are added to the digest first.
 * @param suffix Bytes appended to the digest input after the file contents.
 * @param digest Receives the 64-character lowercase hexadecimal digest.
 * @return true when the file was read and the digest was calculated; otherwise
 * false.
 */
bool CalculateFileSha256Hex(const std::filesystem::path& path,
                            const std::vector<uint8_t>& suffix,
                            std::string* digest);

/**
 * Calculates the lowercase SHA-256 hexadecimal digest of a file.
 *
 * @param path File whose bytes are used as the digest input.
 * @param digest Receives the 64-character lowercase hexadecimal digest.
 * @return true when the file was read and the digest was calculated; otherwise
 * false.
 */
bool CalculateFileSha256Hex(const std::filesystem::path& path,
                            std::string* digest);

}  // namespace muon_internal
