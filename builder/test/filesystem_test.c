// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "common.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char *large = muon_path_join(argv[1], "large");
  char *source = muon_path_join(argv[1], "source");
  char *destination = muon_path_join(argv[1], "destination");
  char *entry = source == NULL ? NULL : muon_path_join(source, "entry");
  if (large == NULL || source == NULL || destination == NULL || entry == NULL) return 2;
  const unsigned long long expected = 3ULL * 1024 * 1024 * 1024;
  const int file = open(large, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (file < 0 || ftruncate(file, (off_t)expected) != 0 || close(file) != 0) return 2;
  unsigned long long size = 0;
  int result = 1;
  if (muon_get_file_size(large, &size) != 0 || size != expected) {
    fprintf(stderr, "Large file size was not preserved: %llu\n", size);
  } else {
    size_t copied = 0;
    size_t counted = 0;
    if (muon_write_text_file(entry, "entry\n") != 0 ||
        muon_copy_directory_contents(source, destination, &copied) != 0 ||
        muon_count_files_recursive(destination, &counted) != 0 ||
        copied != 1 || counted != 1) {
      fprintf(stderr, "Directory entries were lost: copied=%zu counted=%zu\n", copied, counted);
    } else {
      result = 0;
    }
  }
  free(large);
  free(source);
  free(destination);
  free(entry);
  return result;
}
