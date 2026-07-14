/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_icon.h"

#include <cstring>
#include <limits>

static uint32_t ReadBigEndianUint32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

bool IsMuonIconBitmapWithinLimits(const MuonIconBitmap& bitmap) {
  if (bitmap.pixel_width <= 0 || bitmap.pixel_height <= 0) {
    return false;
  }
  const auto width = static_cast<uint64_t>(bitmap.pixel_width);
  const auto height = static_cast<uint64_t>(bitmap.pixel_height);
  if (width > kMuonIconPngDecodeLimits.max_width ||
      height > kMuonIconPngDecodeLimits.max_height) {
    return false;
  }
  const auto pixels = width * height;
  if (pixels > kMuonIconPngDecodeLimits.max_pixels ||
      pixels > std::numeric_limits<uint64_t>::max() / 4) {
    return false;
  }
  const auto rgba_bytes = pixels * 4;
  if (rgba_bytes > kMuonIconPngDecodeLimits.max_rgba_bytes ||
      rgba_bytes > std::numeric_limits<size_t>::max()) {
    return false;
  }
  return bitmap.rgba.size() == static_cast<size_t>(rgba_bytes);
}

MuonIconPngDecodeResult DecodeMuonIconPngWithinLimits(
    const uint8_t* data,
    size_t size,
    const MuonIconPngDecodeLimits& limits,
    const MuonIconPngDecodeCallback& decode) {
  static constexpr uint8_t kPngSignature[] = {0x89, 0x50, 0x4e, 0x47,
                                               0x0d, 0x0a, 0x1a, 0x0a};
  if (data == nullptr || size < sizeof(kPngSignature) ||
      std::memcmp(data, kPngSignature, sizeof(kPngSignature)) != 0) {
    return MuonIconPngDecodeResult::NotPng;
  }
  if (size > limits.max_encoded_bytes) {
    return MuonIconPngDecodeResult::OverBudget;
  }

  static constexpr size_t kPngIhdrEndOffset = 33;
  if (size < kPngIhdrEndOffset || ReadBigEndianUint32(data + 8) != 13 ||
      std::memcmp(data + 12, "IHDR", 4) != 0) {
    return MuonIconPngDecodeResult::InvalidMetadata;
  }

  const auto width = ReadBigEndianUint32(data + 16);
  const auto height = ReadBigEndianUint32(data + 20);
  if (width == 0 || height == 0) {
    return MuonIconPngDecodeResult::InvalidMetadata;
  }
  if (width > limits.max_width || height > limits.max_height) {
    return MuonIconPngDecodeResult::OverBudget;
  }

  const auto pixels =
      static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
  if (pixels > limits.max_pixels ||
      pixels > std::numeric_limits<uint64_t>::max() / 4 ||
      pixels * 4 > limits.max_rgba_bytes) {
    return MuonIconPngDecodeResult::OverBudget;
  }
  if (!decode || !decode()) {
    return MuonIconPngDecodeResult::DecodeFailed;
  }
  return MuonIconPngDecodeResult::Decoded;
}

bool AppendMuonBytesWithinLimit(std::vector<uint8_t>* destination,
                                const void* data,
                                size_t size,
                                size_t max_size) {
  if (destination == nullptr || (data == nullptr && size > 0)) {
    return false;
  }
  if (destination->size() > max_size ||
      size > max_size - destination->size()) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  destination->insert(destination->end(), bytes, bytes + size);
  return true;
}

void ReleaseMuonByteBuffer(std::vector<uint8_t>* buffer) {
  if (buffer == nullptr) {
    return;
  }
  std::vector<uint8_t>().swap(*buffer);
}
