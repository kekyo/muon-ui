/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#ifndef MUON_CLOSE_DEBUG_LOG_H
#define MUON_CLOSE_DEBUG_LOG_H

#include <string>

/**
 * Appends a temporary close lifecycle diagnostic message next to muon-core.
 *
 * @param message Message body to append.
 */
void AppendMuonCloseDebugLog(const std::string& message);

/**
 * Formats a pointer value for temporary close lifecycle diagnostics.
 *
 * @param pointer Pointer value.
 * @return Hexadecimal pointer text.
 */
std::string FormatMuonCloseDebugPointer(const void* pointer);

/**
 * Formats a boolean value for temporary close lifecycle diagnostics.
 *
 * @param value Boolean value.
 * @return "true" or "false".
 */
const char* FormatMuonCloseDebugBool(bool value);

#endif
