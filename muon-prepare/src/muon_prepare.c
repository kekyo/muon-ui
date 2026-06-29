// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#include "prepare.h"
#include "muon_prepare_resource.h"

#include <string.h>

int main(int argc, char **argv) {
  if (argc >= 2 && strcmp(argv[1], "resource") == 0) {
    return muon_prepare_resource_main(argc, argv, 2);
  }
  return muon_prepare_main(argc, argv);
}
