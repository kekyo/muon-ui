/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include <string>
#include <string_view>

/**
 * Encodes Java UTF-16 code units as standard, length-preserving UTF-8.
 * @param value UTF-16 input, including embedded NUL.
 * @returns Standard UTF-8 rather than JNI Modified UTF-8.
 * @throws std::invalid_argument For unpaired surrogates.
 */
std::string MuonAndroidEncodeUtf8(std::u16string_view value);

/**
 * Decodes standard UTF-8 for a Java string.
 * @param value Length-delimited UTF-8 input, including embedded NUL.
 * @returns UTF-16 code units with surrogate pairs for supplementary characters.
 * @throws std::invalid_argument For malformed or noncanonical UTF-8.
 */
std::u16string MuonAndroidDecodeUtf8(std::string_view value);
