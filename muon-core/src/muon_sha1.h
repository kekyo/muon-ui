/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace muon_internal {

/**
 * Calculates a lowercase SHA-1 hexadecimal digest for byte data and a suffix.
 */
std::string CalculateSha1Hex(const std::vector<uint8_t>& data,
                             const std::vector<uint8_t>& suffix);

/**
 * Calculates a lowercase SHA-1 hexadecimal digest for a file.
 */
bool CalculateFileSha1Hex(const std::filesystem::path& path,
                          std::string* digest);

}  // namespace muon_internal
