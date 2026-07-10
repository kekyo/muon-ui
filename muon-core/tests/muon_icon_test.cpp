/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_icon.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

static bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "%s\n", message);
  return false;
}

static void WriteBigEndianUint32(std::vector<uint8_t>* data,
                                 size_t offset,
                                 uint32_t value) {
  (*data)[offset] = static_cast<uint8_t>(value >> 24);
  (*data)[offset + 1] = static_cast<uint8_t>(value >> 16);
  (*data)[offset + 2] = static_cast<uint8_t>(value >> 8);
  (*data)[offset + 3] = static_cast<uint8_t>(value);
}

static std::vector<uint8_t> CreateSyntheticPngIhdr(uint32_t width,
                                                   uint32_t height) {
  auto data = std::vector<uint8_t>(33, 0);
  const uint8_t signature[] = {0x89, 0x50, 0x4e, 0x47,
                               0x0d, 0x0a, 0x1a, 0x0a};
  for (auto index = size_t{0}; index < sizeof(signature); ++index) {
    data[index] = signature[index];
  }
  WriteBigEndianUint32(&data, 8, 13);
  data[12] = 'I';
  data[13] = 'H';
  data[14] = 'D';
  data[15] = 'R';
  WriteBigEndianUint32(&data, 16, width);
  WriteBigEndianUint32(&data, 20, height);
  data[24] = 8;
  data[25] = 6;
  return data;
}

static bool ExpectDecodeResult(const std::vector<uint8_t>& data,
                               const MuonIconPngDecodeLimits& limits,
                               MuonIconPngDecodeResult expected,
                               int expected_calls,
                               bool decoder_result,
                               const char* message) {
  auto decoder_calls = 0;
  const auto result = DecodeMuonIconPngWithinLimits(
      data.data(), data.size(), limits, [&decoder_calls, decoder_result]() {
        ++decoder_calls;
        return decoder_result;
      });
  return Expect(result == expected, message) &&
         Expect(decoder_calls == expected_calls,
                "unexpected guarded decoder call count");
}

static bool TestIconPngMetadataBudget() {
  const auto limits = MuonIconPngDecodeLimits{64, 64, 64, 1024, 4096};
  const auto rgba_limit = MuonIconPngDecodeLimits{64, 64, 64, 1024, 15};
  const auto overflow_limit = MuonIconPngDecodeLimits{
      64, std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint64_t>::max(),
      std::numeric_limits<uint64_t>::max()};
  auto wrong_signature = CreateSyntheticPngIhdr(32, 32);
  wrong_signature[0] = 0;
  auto truncated_ihdr = CreateSyntheticPngIhdr(32, 32);
  truncated_ihdr.resize(24);
  auto wrong_chunk = CreateSyntheticPngIhdr(32, 32);
  wrong_chunk[12] = 'I';
  wrong_chunk[13] = 'D';
  wrong_chunk[14] = 'A';
  wrong_chunk[15] = 'T';
  auto wrong_length = CreateSyntheticPngIhdr(32, 32);
  WriteBigEndianUint32(&wrong_length, 8, 12);

  return ExpectDecodeResult(CreateSyntheticPngIhdr(32, 32), limits,
                            MuonIconPngDecodeResult::Decoded, 1, true,
                            "32x32 PNG metadata should reach the decoder") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(64, 16), limits,
                            MuonIconPngDecodeResult::Decoded, 1, true,
                            "64x16 PNG metadata should reach the decoder") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(65, 1), limits,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "over-width PNG metadata was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(1, 65), limits,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "over-height PNG metadata was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(33, 33), limits,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "over-area PNG metadata was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(2, 2), rgba_limit,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "over-RGBA-byte PNG metadata was accepted") &&
         ExpectDecodeResult(
             CreateSyntheticPngIhdr(
                 std::numeric_limits<uint32_t>::max(),
                 std::numeric_limits<uint32_t>::max()),
             overflow_limit, MuonIconPngDecodeResult::OverBudget, 0, true,
             "overflowing RGBA byte count was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(0, 1), limits,
                            MuonIconPngDecodeResult::InvalidMetadata, 0, true,
                            "zero-width PNG metadata was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(1, 0), limits,
                            MuonIconPngDecodeResult::InvalidMetadata, 0, true,
                            "zero-height PNG metadata was accepted") &&
         ExpectDecodeResult(wrong_signature, limits,
                            MuonIconPngDecodeResult::NotPng, 0, true,
                            "non-PNG input reached the decoder") &&
         ExpectDecodeResult(truncated_ihdr, limits,
                            MuonIconPngDecodeResult::InvalidMetadata, 0, true,
                            "truncated IHDR reached the decoder") &&
         ExpectDecodeResult(wrong_chunk, limits,
                            MuonIconPngDecodeResult::InvalidMetadata, 0, true,
                            "non-IHDR first chunk reached the decoder") &&
         ExpectDecodeResult(wrong_length, limits,
                            MuonIconPngDecodeResult::InvalidMetadata, 0, true,
                            "invalid IHDR length reached the decoder") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(32, 32), limits,
                            MuonIconPngDecodeResult::DecodeFailed, 1, false,
                            "decoder failure was not preserved");
}

static bool TestProductionIconPngLimits() {
  auto encoded_limit = kMuonIconPngDecodeLimits;
  encoded_limit.max_encoded_bytes = 32;
  auto exact_encoded_limit = kMuonIconPngDecodeLimits;
  exact_encoded_limit.max_encoded_bytes = 33;
  return ExpectDecodeResult(CreateSyntheticPngIhdr(256, 256),
                            kMuonIconPngDecodeLimits,
                            MuonIconPngDecodeResult::Decoded, 1, true,
                            "256x256 production boundary was rejected") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(257, 1),
                            kMuonIconPngDecodeLimits,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "257px production width was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(1, 257),
                            kMuonIconPngDecodeLimits,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "257px production height was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(1, 1), encoded_limit,
                            MuonIconPngDecodeResult::OverBudget, 0, true,
                            "over-encoded-byte PNG was accepted") &&
         ExpectDecodeResult(CreateSyntheticPngIhdr(1, 1), exact_encoded_limit,
                            MuonIconPngDecodeResult::Decoded, 1, true,
                            "exact encoded-byte boundary was rejected");
}

static bool TestBoundedByteAppend() {
  auto data = std::vector<uint8_t>{1, 2};
  const uint8_t within_limit[] = {3, 4};
  const uint8_t over_limit[] = {5};
  if (!Expect(AppendMuonBytesWithinLimit(&data, within_limit,
                                        sizeof(within_limit), 4),
              "bytes at the limit were rejected") ||
      !Expect(data == std::vector<uint8_t>({1, 2, 3, 4}),
              "bounded append changed accepted bytes") ||
      !Expect(!AppendMuonBytesWithinLimit(&data, over_limit,
                                         sizeof(over_limit), 4),
              "bytes above the limit were accepted") ||
      !Expect(data == std::vector<uint8_t>({1, 2, 3, 4}),
              "rejected append changed the destination")) {
    return false;
  }
  auto already_over_limit = std::vector<uint8_t>{1, 2};
  const uint8_t byte = 3;
  return Expect(AppendMuonBytesWithinLimit(&data, nullptr, 0, 4),
                "empty append should succeed") &&
         Expect(!AppendMuonBytesWithinLimit(nullptr, over_limit,
                                            sizeof(over_limit), 4),
                "null destination was accepted") &&
         Expect(!AppendMuonBytesWithinLimit(&already_over_limit, &byte, 1, 1),
                "destination already above the limit was accepted") &&
         Expect(already_over_limit == std::vector<uint8_t>({1, 2}),
                "rejected existing-over-limit append changed bytes") &&
         Expect(!AppendMuonBytesWithinLimit(
                    &already_over_limit, &byte,
                    std::numeric_limits<size_t>::max(),
                    std::numeric_limits<size_t>::max()),
                "overflowing append length was accepted") &&
         Expect(already_over_limit == std::vector<uint8_t>({1, 2}),
                "rejected overflowing append changed bytes");
}

static MuonIconBitmap CreateBitmap(int pixel_width,
                                   int pixel_height,
                                   size_t byte_length) {
  MuonIconBitmap bitmap;
  bitmap.pixel_width = pixel_width;
  bitmap.pixel_height = pixel_height;
  bitmap.rgba.resize(byte_length);
  return bitmap;
}

static bool TestDecodedIconBitmapLimits() {
  return Expect(IsMuonIconBitmapWithinLimits(CreateBitmap(1, 1, 4)),
                "valid 1x1 decoded bitmap was rejected") &&
         Expect(IsMuonIconBitmapWithinLimits(
                    CreateBitmap(256, 256, 256 * 256 * 4)),
                "valid 256x256 decoded bitmap was rejected") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(0, 1, 0)),
                "zero-width decoded bitmap was accepted") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(-1, 1, 4)),
                "negative-width decoded bitmap was accepted") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(1, 0, 0)),
                "zero-height decoded bitmap was accepted") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(257, 1, 257 * 4)),
                "over-width decoded bitmap was accepted") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(1, 257, 257 * 4)),
                "over-height decoded bitmap was accepted") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(2, 2, 15)),
                "short decoded bitmap was accepted") &&
         Expect(!IsMuonIconBitmapWithinLimits(CreateBitmap(2, 2, 17)),
                "long decoded bitmap was accepted");
}

int main() {
  return TestIconPngMetadataBudget() && TestProductionIconPngLimits() &&
                 TestBoundedByteAppend() && TestDecodedIconBitmapLimits()
             ? 0
             : 1;
}
