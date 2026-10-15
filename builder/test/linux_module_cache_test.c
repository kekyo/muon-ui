// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include "linux_module_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *source = "\"@MUON_RUNTIME@/linux/im.so\"\n"
                       "\"@MUON_RUNTIME@/linux/image.so\"\n"
                       "\"name\" \"Input method\"\n";
  const char *expected = "\"/tmp/日本語 dir/\\\"quote\\\"/back\\\\slash/line\\012tab\\011/linux/im.so\"\n"
                         "\"/tmp/日本語 dir/\\\"quote\\\"/back\\\\slash/line\\012tab\\011/linux/image.so\"\n"
                         "\"name\" \"Input method\"\n";
  char *rendered = muon_expand_linux_module_cache(
      source, "/tmp/日本語 dir/\"quote\"/back\\slash/line\ntab\t");
  if (rendered == NULL || strcmp(rendered, expected) != 0) {
    fprintf(stderr, "Module cache did not preserve escaped runtime paths\n");
    free(rendered);
    return 1;
  }
  free(rendered);
  rendered = muon_expand_linux_module_cache("unchanged\n", "/runtime");
  if (rendered == NULL || strcmp(rendered, "unchanged\n") != 0) {
    fprintf(stderr, "Module cache metadata was changed\n");
    free(rendered);
    return 1;
  }
  free(rendered);
  return 0;
}
