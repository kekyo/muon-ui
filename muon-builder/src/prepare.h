// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_H
#define MUON_PREPARE_H

#ifndef MUON_PREPARE_TARGET_NAME
#ifdef _WIN32
#ifdef _WIN64
#define MUON_PREPARE_TARGET_NAME "windows-amd64"
#else
#define MUON_PREPARE_TARGET_NAME "windows-i686"
#endif
#else
#define MUON_PREPARE_TARGET_NAME "linux-amd64"
#endif
#endif

#include "prepare_progress.h"

/**
 * Runs the muon-builder command-line entry point.
 *
 * @param argc Number of command-line arguments.
 * @param argv Command-line argument values.
 * @return Process exit code for the prepare operation.
 */
int muon_prepare_main(int argc, char **argv);

/**
 * Prepares CEF files inside an existing portable muon runtime directory.
 *
 * @param muon_path Directory containing muon-core runtime files.
 * @param target Public runtime target such as linux-amd64, linux-armhf,
 * linux-arm64, windows-i686, or windows-amd64.
 * @param cache_dir Cache directory. Pass NULL to use the default cache.
 * @param force Non-zero to rebuild cached CEF files and runtime placement.
 * @param quiet Non-zero to suppress progress messages.
 * @return 0 when the runtime is ready; non-zero on failure.
 */
int muon_prepare_in_place(const char *muon_path, const char *target,
                          const char *cache_dir, int force, int quiet);

/**
 * Prepares CEF files inside an existing portable muon runtime directory and
 * reports progress events to the caller.
 *
 * @param muon_path Directory containing muon-core runtime files.
 * @param target Public runtime target such as linux-amd64, linux-armhf,
 * linux-arm64, windows-i686, or windows-amd64.
 * @param cache_dir Cache directory. Pass NULL to use the default cache.
 * @param force Non-zero to rebuild cached CEF files and runtime placement.
 * @param quiet Non-zero to suppress diagnostic text output.
 * @param progress_callback Callback receiving progress events, or NULL.
 * @param progress_user_data Opaque value passed to progress_callback.
 * @return 0 when the runtime is ready; non-zero on failure.
 */
int muon_prepare_in_place_with_progress(
    const char *muon_path, const char *target, const char *cache_dir, int force,
    int quiet, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data);

/**
 * Prepares CEF and muon runtime files in a separate staging directory and
 * reports progress events to the caller.
 *
 * @param muon_path Directory containing source muon-core runtime files.
 * @param stage_dir Directory where executable runtime files are staged.
 * @param target Public runtime target such as linux-amd64, linux-armhf,
 * linux-arm64, windows-i686, or windows-amd64.
 * @param cache_dir Cache directory. Pass NULL to use the default cache.
 * @param force Non-zero to rebuild cached CEF files and runtime placement.
 * @param quiet Non-zero to suppress diagnostic text output.
 * @param progress_callback Callback receiving progress events, or NULL.
 * @param progress_user_data Opaque value passed to progress_callback.
 * @return 0 when the staged runtime is ready; non-zero on failure.
 */
int muon_prepare_staged_with_progress(
    const char *muon_path, const char *stage_dir, const char *target,
    const char *cache_dir, int force, int quiet,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data);

#endif
