/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#ifndef MUON_BUILTIN_ENVIRONMENT_HELPERS_H
#define MUON_BUILTIN_ENVIRONMENT_HELPERS_H

#include <string>
#include <utility>
#include <vector>

namespace muon_internal {

/**
 * Enumerates the current process environment as UTF-8 key/value pairs.
 *
 * @return Environment entries in platform enumeration order.
 */
std::vector<std::pair<std::string, std::string>> GetMuonEnvironmentEntries();

#if defined(_WIN32)

/**
 * Converts a null-terminated UTF-16 Windows string to UTF-8.
 *
 * @param source Source wide string.
 * @param target Destination string.
 * @return True when conversion succeeds.
 */
bool MuonWideToUtf8(const wchar_t* source, std::string* target);

/**
 * Converts a UTF-8 string to UTF-16 for Windows APIs.
 *
 * @param source Source UTF-8 string.
 * @param target Destination wide string.
 * @return True when conversion succeeds.
 */
bool MuonUtf8ToWide(const std::string& source, std::wstring* target);

#endif

}  // namespace muon_internal

#endif  // MUON_BUILTIN_ENVIRONMENT_HELPERS_H
