/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <muon_service_value.h>

#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

/** Opaque callback identity scoped by the enclosing service invocation. */
struct MuonServiceCallback {
  uint64_t token; /**< Nonzero identity; never an address. */
};

/** Owned service value, independent of native or transport buffer lifetimes. */
struct MuonServiceValue {
  using Bytes = std::vector<uint8_t>; /**< Owned binary data. */
  using Array = std::vector<MuonServiceValue>; /**< Owned ordered child values. */
  /** Portable scalar, data, or callback representation. */
  using Data = std::variant<std::monostate, bool, int32_t, uint32_t, int64_t,
      uint64_t, double, std::string, Bytes, Array, MuonServiceCallback>;
  Data data; /**< Payload; monostate represents void. */
};

/**
 * Copies borrowed C ABI data before its producer regains control.
 * @param value Complete borrowed value tree.
 * @return Independently owned values; integer widths are preserved exactly.
 * @throws std::invalid_argument For invalid kinds, pointers, UTF-8, nonfinite
 *     numbers, zero callback IDs, or excessive depth/size. A tree is limited to
 *     16 array levels, 65536 values, and 1 MiB of string and binary data.
 */
MuonServiceValue CopyMuonServiceValue(const muon_service_value& value);

/** Private storage retaining native arrays and their owned source values. */
struct MuonNativeServiceValuesImpl;

/** Retains immutable C ABI argument views across asynchronous native calls. */
class MuonNativeServiceValues final {
 public:
  /** @param values Owned values to retain until this object is destroyed.
   *  @throws std::invalid_argument For unsupported or excessive values. */
  explicit MuonNativeServiceValues(std::vector<MuonServiceValue> values);
  /** Releases views and all retained data. */
  ~MuonNativeServiceValues();
  /** Transfers ownership without invalidating previously obtained views. */
  MuonNativeServiceValues(MuonNativeServiceValues&&) noexcept;
  /** Transfers ownership, disposing of this object's previous views. */
  MuonNativeServiceValues& operator=(MuonNativeServiceValues&&) noexcept;
  /** Views have one owning container. */
  MuonNativeServiceValues(const MuonNativeServiceValues&) = delete;
  /** Views have one owning container. */
  MuonNativeServiceValues& operator=(const MuonNativeServiceValues&) = delete;
  /** @return Immutable argument views; empty for a moved-from container. */
  std::span<const muon_service_value> Get() const;

 private:
  std::unique_ptr<MuonNativeServiceValuesImpl> impl_;
};
