/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

/**
 * Decoded runtime icon pixels shared by native icon consumers.
 *
 * @remarks Pixels are tightly packed in row-major premultiplied RGBA8888
 * format. The row stride is `pixel_width * 4`, and the byte length must be
 * exactly `pixel_width * pixel_height * 4`.
 */
struct MuonIconBitmap {
  /** Premultiplied RGBA8888 pixel bytes. */
  std::vector<uint8_t> rgba;
  /** Bitmap width in physical pixels. */
  int pixel_width = 0;
  /** Bitmap height in physical pixels. */
  int pixel_height = 0;
};

/**
 * Immutable shared decoded runtime icon bitmap.
 */
using MuonIconBitmapPtr = std::shared_ptr<const MuonIconBitmap>;

/**
 * Resource limits enforced before decoding a runtime icon PNG.
 */
struct MuonIconPngDecodeLimits {
  /** Maximum encoded PNG byte length. */
  size_t max_encoded_bytes;
  /** Maximum PNG width in pixels. */
  uint32_t max_width;
  /** Maximum PNG height in pixels. */
  uint32_t max_height;
  /** Maximum total PNG pixel count. */
  uint64_t max_pixels;
  /** Maximum decoded 32-bit RGBA byte length. */
  uint64_t max_rgba_bytes;
};

/**
 * Production resource limits for runtime icon PNGs and favicon responses.
 */
inline constexpr MuonIconPngDecodeLimits kMuonIconPngDecodeLimits = {
    1024 * 1024,
    256,
    256,
    256 * 256,
    256 * 256 * 4,
};

/**
 * Result of PNG metadata validation and a guarded decoder invocation.
 */
enum class MuonIconPngDecodeResult {
  /** Input does not begin with the standard PNG signature. */
  NotPng,
  /** PNG signature is present but the mandatory IHDR is invalid. */
  InvalidMetadata,
  /** Encoded or decoded resource limits would be exceeded. */
  OverBudget,
  /** The guarded decoder rejected the input. */
  DecodeFailed,
  /** Metadata validation and the guarded decoder both succeeded. */
  Decoded,
};

/**
 * Returns whether decoded icon pixels satisfy the production bitmap contract.
 *
 * @param bitmap Decoded premultiplied RGBA8888 bitmap.
 * @return true when dimensions, pixel count, and exact byte length are within
 * production runtime icon limits.
 */
bool IsMuonIconBitmapWithinLimits(const MuonIconBitmap& bitmap);

/**
 * Callback that performs full PNG decoding after metadata validation.
 */
using MuonIconPngDecodeCallback = std::function<bool()>;

/**
 * Validates PNG metadata and invokes a decoder only within explicit limits.
 *
 * @param data Encoded PNG bytes.
 * @param size Encoded byte length.
 * @param limits Resource limits to enforce before decoding.
 * @param decode Full decoder callback.
 * @return Validation and decoder result.
 */
MuonIconPngDecodeResult DecodeMuonIconPngWithinLimits(
    const uint8_t* data,
    size_t size,
    const MuonIconPngDecodeLimits& limits,
    const MuonIconPngDecodeCallback& decode);

/**
 * Appends bytes without allowing a destination buffer to exceed a limit.
 *
 * @param destination Buffer that receives the bytes.
 * @param data Source bytes.
 * @param size Source byte length.
 * @param max_size Maximum destination byte length.
 * @return true when the bytes were appended, otherwise false with the
 * destination unchanged.
 */
bool AppendMuonBytesWithinLimit(std::vector<uint8_t>* destination,
                                const void* data,
                                size_t size,
                                size_t max_size);

/**
 * Releases all byte storage retained by a buffer.
 *
 * @param buffer Buffer whose elements and allocated storage are released.
 */
void ReleaseMuonByteBuffer(std::vector<uint8_t>* buffer);
