/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <filesystem>

/**
 * Resolves the running muon executable path.
 *
 * @param path Receives the platform-reported executable path.
 * @return true when the executable path could be resolved.
 */
bool GetMuonExecutablePath(std::filesystem::path* path);

/**
 * Returns the directory that contains the running muon executable.
 *
 * @remarks Falls back to the current working directory when the platform
 * executable path cannot be resolved.
 */
std::filesystem::path GetMuonExecutableDirectory();
