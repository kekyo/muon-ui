/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace muon_internal {

/**
 * Returns true when the byte is an ASCII whitespace character.
 */
bool IsAsciiSpace(char value);

/**
 * Trims ASCII whitespace from both ends of the string.
 */
std::string TrimAscii(std::string_view value);

/**
 * Converts ASCII characters to lower case.
 */
std::string ToLowerAscii(std::string value);

/**
 * Returns true when the byte is an ASCII hexadecimal digit.
 */
bool IsAsciiHexDigit(char value);

/**
 * Decodes two ASCII hexadecimal digits into one byte.
 */
uint8_t DecodeAsciiHexByte(char high, char low);

}  // namespace muon_internal
