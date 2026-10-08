// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha2.h"

#define MUON_SHA256_DIGEST_LENGTH 32
#define MUON_SHA256_HEX_LENGTH (MUON_SHA256_DIGEST_LENGTH * 2)

typedef struct {
  const uint8_t *data;
  size_t size;
} Sha256Chunk;

static void digest_to_hex(const uint8_t digest[MUON_SHA256_DIGEST_LENGTH],
                          char output[MUON_SHA256_HEX_LENGTH + 1]) {
  static const char hexadecimal[] = "0123456789abcdef";
  for (size_t index = 0; index < MUON_SHA256_DIGEST_LENGTH; index += 1) {
    output[index * 2] = hexadecimal[digest[index] >> 4];
    output[index * 2 + 1] = hexadecimal[digest[index] & 0x0f];
  }
  output[MUON_SHA256_HEX_LENGTH] = '\0';
}

static int expect_digest(const char *name, const Sha256Chunk *chunks,
                         size_t chunk_count, const char *expected) {
  SHA256_CTX context;
  SHA256_Init(&context);
  for (size_t index = 0; index < chunk_count; index += 1) {
    SHA256_Update(&context, chunks[index].data, chunks[index].size);
  }

  uint8_t digest[MUON_SHA256_DIGEST_LENGTH];
  char actual[MUON_SHA256_HEX_LENGTH + 1];
  SHA256_Final(digest, &context);
  digest_to_hex(digest, actual);
  if (strcmp(actual, expected) == 0) {
    return 0;
  }

  fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual);
  return 1;
}

static int expect_bytes(const char *name, const uint8_t *data, size_t size,
                        const char *expected) {
  const Sha256Chunk chunks[] = {{data, size}};
  return expect_digest(name, chunks, 1, expected);
}

static int expect_repeated_a(const char *name, size_t size,
                             const char *expected) {
  uint8_t *data = (uint8_t *)malloc(size);
  if (data == NULL) {
    fprintf(stderr, "%s: failed to allocate %llu bytes\n", name,
            (unsigned long long)size);
    return 1;
  }
  memset(data, 'a', size);
  const int failed = expect_bytes(name, data, size, expected);
  free(data);
  return failed;
}

static int test_empty_input(void) {
  return expect_digest(
      "empty input", NULL, 0,
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static int test_abc(void) {
  static const uint8_t input[] = {'a', 'b', 'c'};
  return expect_bytes(
      "abc", input, sizeof(input),
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

static int test_multiple_updates(void) {
  static const char input[] = "The quick brown fox jumps over the lazy dog";
  const Sha256Chunk chunks[] = {
      {(const uint8_t *)input, 4},
      {(const uint8_t *)input + 4, 7},
      {(const uint8_t *)input + 11, 16},
      {(const uint8_t *)input + 27, sizeof(input) - 1 - 27},
  };
  return expect_digest(
      "multiple updates", chunks, sizeof(chunks) / sizeof(chunks[0]),
      "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

static int test_padding_boundaries(void) {
  int failed = 0;
  failed |= expect_repeated_a(
      "55-byte input", 55,
      "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  failed |= expect_repeated_a(
      "56-byte input", 56,
      "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  failed |= expect_repeated_a(
      "63-byte input", 63,
      "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
  failed |= expect_repeated_a(
      "64-byte input", 64,
      "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  failed |= expect_repeated_a(
      "65-byte input", 65,
      "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
  return failed;
}

static int test_input_larger_than_64_kib(void) {
  const size_t size = 64 * 1024 + 1;
  uint8_t *data = (uint8_t *)malloc(size);
  if (data == NULL) {
    fprintf(stderr, "64-KiB input: failed to allocate test input\n");
    return 1;
  }
  memset(data, 'a', size);
  const Sha256Chunk chunks[] = {
      {data, 1},
      {data + 1, 64 * 1024 - 1},
      {data + 64 * 1024, 1},
  };
  const int failed = expect_digest(
      "64-KiB input", chunks, sizeof(chunks) / sizeof(chunks[0]),
      "008ffc88d3c96a9f307524eb361e47c5222a887fc45fa0c1fb8d429c5c23b430");
  free(data);
  return failed;
}

static int test_file_bytes_followed_by_suffix(void) {
  static const uint8_t file_bytes[] = {
      0x00, 0x01, 0x02, 0x7f, 0x80, 0xfe, 0xff, 'f', 'i', 'l', 'e',
  };
  static const uint8_t suffix[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x10};
  const Sha256Chunk chunks[] = {
      {file_bytes, sizeof(file_bytes)},
      {suffix, sizeof(suffix)},
  };
  return expect_digest(
      "file bytes followed by suffix", chunks,
      sizeof(chunks) / sizeof(chunks[0]),
      "130e165eeeae2ec394e4e2214afed071a864eef153ec117f29c05ed36f95c22a");
}

int main(void) {
  int failed = 0;
  failed |= test_empty_input();
  failed |= test_abc();
  failed |= test_multiple_updates();
  failed |= test_padding_boundaries();
  failed |= test_input_larger_than_64_kib();
  failed |= test_file_bytes_followed_by_suffix();
  return failed;
}
