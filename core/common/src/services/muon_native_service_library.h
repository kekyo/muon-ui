/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "muon_service_api.h"

#include <memory>
#include <string>

/**
 * Loads native service metadata without starting an instance.
 * @param path Absolute UTF-8 library path selected from the installed manifest.
 * @param error Failure diagnostic; cleared on success.
 * @return Factory retaining the loaded library, or null on failure.
 * @remarks The last reference unloads the library. Release it only after every
 *     instance and callback has finished returning through library code.
 */
std::shared_ptr<const muon_service_factory> LoadMuonNativeServiceFactory(
    const std::string& path, std::string& error);
