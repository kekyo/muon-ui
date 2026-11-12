/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_android_utf.h"
#include <cstdint>
#include <stdexcept>

std::string MuonAndroidEncodeUtf8(std::u16string_view value) {
  std::string result;
  for (size_t index = 0; index < value.size(); ++index) {
    auto scalar = static_cast<uint32_t>(value[index]);
    if (scalar >= 0xd800 && scalar <= 0xdbff) {
      if (++index >= value.size() || value[index] < 0xdc00 || value[index] > 0xdfff)
        throw std::invalid_argument("Unpaired UTF-16 surrogate");
      scalar = 0x10000 + ((scalar - 0xd800) << 10) + (value[index] - 0xdc00);
    } else if (scalar >= 0xdc00 && scalar <= 0xdfff) {
      throw std::invalid_argument("Unpaired UTF-16 surrogate");
    }
    if (scalar < 0x80) result.push_back(static_cast<char>(scalar));
    else if (scalar < 0x800) {
      result.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
      result.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else if (scalar < 0x10000) {
      result.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
      result.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
      result.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else {
      result.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
      result.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 63)));
      result.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
      result.push_back(static_cast<char>(0x80 | (scalar & 63)));
    }
  }
  return result;
}

std::u16string MuonAndroidDecodeUtf8(std::string_view value) {
  std::u16string result;
  for (size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index++]);
    auto scalar = uint32_t{};
    auto remaining = size_t{};
    auto minimum = uint32_t{};
    if (first < 0x80) scalar = first;
    else if (first >= 0xc2 && first <= 0xdf) { scalar = first & 31; remaining = 1; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { scalar = first & 15; remaining = 2; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { scalar = first & 7; remaining = 3; minimum = 0x10000; }
    else throw std::invalid_argument("Invalid UTF-8 leading byte");
    if (remaining > value.size() - index) throw std::invalid_argument("Incomplete UTF-8 character");
    while (remaining > 0) {
      --remaining;
      const auto next = static_cast<unsigned char>(value[index++]);
      if ((next & 0xc0) != 0x80) throw std::invalid_argument("Invalid UTF-8 continuation");
      scalar = (scalar << 6) | (next & 63);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
      throw std::invalid_argument("Invalid UTF-8 scalar");
    if (scalar < 0x10000) result.push_back(static_cast<char16_t>(scalar));
    else {
      scalar -= 0x10000;
      result.push_back(static_cast<char16_t>(0xd800 | (scalar >> 10)));
      result.push_back(static_cast<char16_t>(0xdc00 | (scalar & 1023)));
    }
  }
  return result;
}
