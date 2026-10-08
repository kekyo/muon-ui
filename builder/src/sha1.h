/*	$NetBSD: sha1.h,v 1.16 2023/08/01 07:04:16 mrg Exp $	*/

/*
 * SHA-1 in C
 * By Steve Reid <steve@edmweb.com>
 * 100% Public Domain
 */

#ifndef MUON_PREPARE_SHA1_H
#define MUON_PREPARE_SHA1_H

#include <stddef.h>
#include <stdint.h>

#define SHA1_DIGEST_LENGTH 20
#define SHA1_DIGEST_STRING_LENGTH 41
#define SHA1_BLOCK_LENGTH 64

typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  uint8_t buffer[SHA1_BLOCK_LENGTH];
} SHA1_CTX;

void SHA1Transform(uint32_t state[5], const uint8_t buffer[64]);
void SHA1Init(SHA1_CTX *context);
void SHA1Update(SHA1_CTX *context, const uint8_t *data, unsigned int length);
void SHA1Final(uint8_t digest[SHA1_DIGEST_LENGTH], SHA1_CTX *context);

#endif
