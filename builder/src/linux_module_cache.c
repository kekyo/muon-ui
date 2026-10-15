// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include "linux_module_cache.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

char *muon_expand_linux_module_cache(const char *source,
                                     const char *runtime_path) {
  static const char marker[] = "@MUON_RUNTIME@";
  const size_t path_length = strlen(runtime_path);
  if (path_length > (SIZE_MAX - 1) / 4) {
    return NULL;
  }
  char *escaped = (char *)malloc(path_length * 4 + 1);
  if (escaped == NULL) {
    return NULL;
  }
  char *next = escaped;
  for (const unsigned char *cursor = (const unsigned char *)runtime_path;
       *cursor != 0; cursor += 1) {
    if (*cursor == '\\' || *cursor == '"') {
      *next++ = '\\';
      *next++ = (char)*cursor;
    } else if (*cursor < 32 || *cursor == 127) {
      *next++ = '\\';
      *next++ = (char)('0' + ((*cursor >> 6) & 7));
      *next++ = (char)('0' + ((*cursor >> 3) & 7));
      *next++ = (char)('0' + (*cursor & 7));
    } else {
      *next++ = (char)*cursor;
    }
  }
  *next = '\0';
  const size_t escaped_length = (size_t)(next - escaped);
  size_t count = 0;
  const char *cursor = source;
  const char *found;
  while ((found = strstr(cursor, marker)) != NULL) {
    count += 1;
    cursor = found + sizeof(marker) - 1;
  }
  const size_t source_length = strlen(source);
  if (count != 0 && escaped_length > (SIZE_MAX - source_length - 1) / count) {
    free(escaped);
    return NULL;
  }
  char *result = (char *)malloc(source_length + count * escaped_length + 1);
  if (result == NULL) {
    free(escaped);
    return NULL;
  }
  cursor = source;
  next = result;
  while ((found = strstr(cursor, marker)) != NULL) {
    const size_t prefix_length = (size_t)(found - cursor);
    memcpy(next, cursor, prefix_length);
    next += prefix_length;
    memcpy(next, escaped, escaped_length);
    next += escaped_length;
    cursor = found + sizeof(marker) - 1;
  }
  strcpy(next, cursor);
  free(escaped);
  return result;
}
