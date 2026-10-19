// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include "prepare.h"
#include "muon_prepare_resource.h"
#include "common.h"

#include <string.h>

static int run_builder(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "resource") == 0) {
    return muon_prepare_resource_main(argc, argv, 2);
  }
  return muon_prepare_main(argc, argv);
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  return muon_windows_main(argc, argv, run_builder);
}
#else
int main(int argc, char **argv) {
  return run_builder(argc, argv);
}
#endif
