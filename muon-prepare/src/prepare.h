// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_H
#define MUON_PREPARE_H

#ifndef MUON_PREPARE_TARGET_NAME
#ifdef _WIN32
#ifdef _WIN64
#define MUON_PREPARE_TARGET_NAME "windows64"
#else
#define MUON_PREPARE_TARGET_NAME "windows32"
#endif
#else
#define MUON_PREPARE_TARGET_NAME "linux64"
#endif
#endif

/**
 * Runs the muon-prepare command-line entry point.
 *
 * @param argc Number of command-line arguments.
 * @param argv Command-line argument values.
 * @return Process exit code for the prepare operation.
 */
int muon_prepare_main(int argc, char **argv);

/**
 * Prepares CEF files inside an existing portable Muon runtime directory.
 *
 * @param muon_path Directory containing muon-core runtime files.
 * @param target Runtime target such as linux64, linuxarm, linuxarm64,
 * windows32, or windows64.
 * @param cache_dir Cache directory. Pass NULL to use the default cache.
 * @param force Non-zero to rebuild cached CEF files and runtime placement.
 * @param quiet Non-zero to suppress progress messages.
 * @return 0 when the runtime is ready; non-zero on failure.
 */
int muon_prepare_in_place(const char *muon_path, const char *target,
                          const char *cache_dir, int force, int quiet);

#endif
