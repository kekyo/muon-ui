/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cardio.h>
#include <filesystem>
#include <functional>
#include <optional>

/**
 * Reads one user preference without applying the manifest default.
 * @param path Private host-owned preference file, outside service data directories.
 * @return The saved boolean, or no value only when the file is absent.
 * @throws std::exception On corrupt content or other I/O failures.
 */
cardio::promise<std::optional<bool>> LoadMuonServiceAutostart(std::filesystem::path path);

/**
 * Atomically replaces one user preference through a private sibling file.
 * @param path Preference file whose parent already exists.
 * @param enabled User's explicit preference, including an explicit false.
 * @return After replacement; a failed operation leaves the previous file intact.
 * @remarks The owner serializes saves per service. Files store exactly one ASCII
 * byte, 0 or 1; references and runtime generations are never persisted.
 */
cardio::promise<void> SaveMuonServiceAutostart(std::filesystem::path path, bool enabled);

/**
 * Runs a platform operation which has no asynchronous system API.
 * @param work Bounded blocking filesystem or registration work; must not wait for another queued job.
 * @return After the platform pool completes, resumed on the owner dispatcher.
 * @remarks The OS pool also supports handle-relative file operations that cannot
 * be submitted through a pathname-based asynchronous file API.
 */
cardio::promise<void> RunMuonServiceFileWork(std::function<void()> work);
