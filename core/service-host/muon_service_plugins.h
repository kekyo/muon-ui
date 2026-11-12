/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "services/muon_service_plugin_pool.h"
#include <yyjson.h>

/** Reads a validated registry and installs the independent desktop native loader.
 * @param registry Root service registry object.
 * @param directory Absolute registry directory.
 * @param dispatcher Serialized owner used by plugins and their asynchronous I/O.
 * @param post Thread-safe deferred owner delivery.
 * @returns Process plugin pool, or null when no service declares native modules.
 * @throws std::invalid_argument For malformed or inconsistent plugin declarations. */
std::unique_ptr<MuonServicePluginPool> CreateMuonDesktopServicePlugins(
    yyjson_val* registry, const std::filesystem::path& directory,
    cardio::dispatcher& dispatcher, std::function<void(std::function<void()>)> post);
