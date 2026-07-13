/*
 * SHA-256 implementation adapted from NetBSD's sha2 implementation.
 * SHA-224, SHA-384, SHA-512, and platform-specific code were removed.
 *
 * Copyright 2000 Aaron D. Gifford.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) AND CONTRIBUTOR(S) ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR(S) OR CONTRIBUTOR(S) BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "sha2.h"

#include <string.h>

#define SHA256_SHORT_BLOCK_LENGTH (SHA256_BLOCK_LENGTH - 8)

static const uint32_t kSha256RoundConstants[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
};

static const uint32_t kSha256InitialState[8] = {
    0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
    0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL,
};

static uint32_t rotate_right(uint32_t value, unsigned int bits) {
  return (value >> bits) | (value << (32U - bits));
}

static uint32_t load_big_endian_32(const uint8_t *data) {
  return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void store_big_endian_32(uint8_t *output, uint32_t value) {
  output[0] = (uint8_t)(value >> 24);
  output[1] = (uint8_t)(value >> 16);
  output[2] = (uint8_t)(value >> 8);
  output[3] = (uint8_t)value;
}

static void store_big_endian_64(uint8_t *output, uint64_t value) {
  output[0] = (uint8_t)(value >> 56);
  output[1] = (uint8_t)(value >> 48);
  output[2] = (uint8_t)(value >> 40);
  output[3] = (uint8_t)(value >> 32);
  output[4] = (uint8_t)(value >> 24);
  output[5] = (uint8_t)(value >> 16);
  output[6] = (uint8_t)(value >> 8);
  output[7] = (uint8_t)value;
}

static void sha256_transform(SHA256_CTX *context, const uint8_t *data) {
  uint32_t words[64];
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t e;
  uint32_t f;
  uint32_t g;
  uint32_t h;
  size_t index;

  for (index = 0; index < 16; index += 1) {
    words[index] = load_big_endian_32(data + index * 4);
  }
  for (; index < 64; index += 1) {
    const uint32_t first = words[index - 15];
    const uint32_t second = words[index - 2];
    const uint32_t sigma0 = rotate_right(first, 7) ^ rotate_right(first, 18) ^
                            (first >> 3);
    const uint32_t sigma1 = rotate_right(second, 17) ^
                            rotate_right(second, 19) ^ (second >> 10);
    words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
  }

  a = context->state[0];
  b = context->state[1];
  c = context->state[2];
  d = context->state[3];
  e = context->state[4];
  f = context->state[5];
  g = context->state[6];
  h = context->state[7];

  for (index = 0; index < 64; index += 1) {
    const uint32_t choice = (e & f) ^ ((~e) & g);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                            rotate_right(a, 22);
    const uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                            rotate_right(e, 25);
    const uint32_t first =
        h + sigma1 + choice + kSha256RoundConstants[index] + words[index];
    const uint32_t second = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + first;
    d = c;
    c = b;
    b = a;
    a = first + second;
  }

  context->state[0] += a;
  context->state[1] += b;
  context->state[2] += c;
  context->state[3] += d;
  context->state[4] += e;
  context->state[5] += f;
  context->state[6] += g;
  context->state[7] += h;

  memset(words, 0, sizeof(words));
}

int SHA256_Init(SHA256_CTX *context) {
  if (context == NULL) {
    return 0;
  }
  memcpy(context->state, kSha256InitialState, sizeof(kSha256InitialState));
  context->bitcount = 0;
  memset(context->buffer, 0, sizeof(context->buffer));
  return 1;
}

int SHA256_Update(SHA256_CTX *context, const uint8_t *data, size_t length) {
  size_t used;
  size_t available;

  if (context == NULL || (data == NULL && length != 0)) {
    return 0;
  }
  if (length == 0) {
    return 1;
  }

  used = (size_t)((context->bitcount >> 3) % SHA256_BLOCK_LENGTH);
  context->bitcount += (uint64_t)length << 3;

  if (used != 0) {
    available = SHA256_BLOCK_LENGTH - used;
    if (length < available) {
      memcpy(context->buffer + used, data, length);
      return 1;
    }
    memcpy(context->buffer + used, data, available);
    sha256_transform(context, context->buffer);
    data += available;
    length -= available;
  }

  while (length >= SHA256_BLOCK_LENGTH) {
    sha256_transform(context, data);
    data += SHA256_BLOCK_LENGTH;
    length -= SHA256_BLOCK_LENGTH;
  }
  if (length != 0) {
    memcpy(context->buffer, data, length);
  }
  return 1;
}

int SHA256_Final(uint8_t digest[SHA256_DIGEST_LENGTH], SHA256_CTX *context) {
  const uint64_t bitcount = context != NULL ? context->bitcount : 0;
  size_t used;
  size_t index;

  if (context == NULL || digest == NULL) {
    return 0;
  }

  used = (size_t)((bitcount >> 3) % SHA256_BLOCK_LENGTH);
  context->buffer[used] = 0x80;
  used += 1;

  if (used > SHA256_SHORT_BLOCK_LENGTH) {
    memset(context->buffer + used, 0, SHA256_BLOCK_LENGTH - used);
    sha256_transform(context, context->buffer);
    used = 0;
  }
  memset(context->buffer + used, 0, SHA256_SHORT_BLOCK_LENGTH - used);
  store_big_endian_64(context->buffer + SHA256_SHORT_BLOCK_LENGTH, bitcount);
  sha256_transform(context, context->buffer);

  for (index = 0; index < 8; index += 1) {
    store_big_endian_32(digest + index * 4, context->state[index]);
  }
  memset(context, 0, sizeof(*context));
  return 1;
}
