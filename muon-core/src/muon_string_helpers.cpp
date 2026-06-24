/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_string_helpers.h"

#include <cctype>

namespace muon_internal {

bool IsAsciiSpace(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

std::string TrimAscii(std::string_view value) {
  auto begin = size_t{0};
  while (begin < value.size() && IsAsciiSpace(value[begin])) {
    ++begin;
  }

  auto end = value.size();
  while (end > begin && IsAsciiSpace(value[end - 1])) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::string ToLowerAscii(std::string value) {
  for (auto& character : value) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

bool IsAsciiHexDigit(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'A' && value <= 'F') ||
         (value >= 'a' && value <= 'f');
}

static uint8_t DecodeAsciiHexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<uint8_t>(value - '0');
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<uint8_t>(value - 'A' + 10);
  }
  return static_cast<uint8_t>(value - 'a' + 10);
}

uint8_t DecodeAsciiHexByte(char high, char low) {
  return static_cast<uint8_t>((DecodeAsciiHexNibble(high) << 4) |
                              DecodeAsciiHexNibble(low));
}

}  // namespace muon_internal
