/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_sha256.h"

#include "sha2.h"

#include <array>
#include <cstddef>
#include <fstream>

static const auto ToLowerHex =
    [](const std::array<uint8_t, SHA256_DIGEST_LENGTH>& digest) {
      constexpr char kHexDigits[] = "0123456789abcdef";
      std::string hex;
      hex.reserve(digest.size() * 2);
      for (const auto byte : digest) {
        hex.push_back(kHexDigits[(byte >> 4) & 0x0f]);
        hex.push_back(kHexDigits[byte & 0x0f]);
      }
      return hex;
    };

namespace muon_internal {

bool CalculateFileSha256Hex(const std::filesystem::path& path,
                            const std::vector<uint8_t>& suffix,
                            std::string* digest) {
  if (digest == nullptr) {
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }

  SHA256_CTX context{};
  if (SHA256_Init(&context) == 0) {
    return false;
  }

  std::array<uint8_t, 64 * 1024> buffer{};
  for (;;) {
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    const auto read_size = input.gcount();
    if (read_size > 0 &&
        SHA256_Update(&context, buffer.data(),
                      static_cast<size_t>(read_size)) == 0) {
      return false;
    }
    if (input.eof()) {
      break;
    }
    if (input.fail()) {
      return false;
    }
  }

  if (!suffix.empty() &&
      SHA256_Update(&context, suffix.data(), suffix.size()) == 0) {
    return false;
  }

  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest_bytes{};
  if (SHA256_Final(digest_bytes.data(), &context) == 0) {
    return false;
  }
  *digest = ToLowerHex(digest_bytes);
  return true;
}

bool CalculateFileSha256Hex(const std::filesystem::path& path,
                            std::string* digest) {
  return CalculateFileSha256Hex(path, {}, digest);
}

}  // namespace muon_internal
