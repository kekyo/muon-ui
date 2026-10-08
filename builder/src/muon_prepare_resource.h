// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_PREPARE_RESOURCE_H
#define MUON_PREPARE_RESOURCE_H

/**
 * Runs the muon-builder resource update subcommand.
 *
 * @param argc Process argument count.
 * @param argv Process argument values.
 * @param arg_start Index of the first resource option in argv.
 * @return 0 on success, non-zero on failure.
 */
int muon_prepare_resource_main(int argc, char **argv, int arg_start);

#endif
