/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_sha1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace muon_internal {

class MuonSha1 final {
 public:
  void Update(const uint8_t* data, size_t size) {
    total_size_ += size;
    auto offset = size_t{0};
    if (buffer_size_ > 0) {
      const auto copy_size = std::min(size, buffer_.size() - buffer_size_);
      std::memcpy(buffer_.data() + buffer_size_, data, copy_size);
      buffer_size_ += copy_size;
      offset += copy_size;
      if (buffer_size_ == buffer_.size()) {
        ProcessBlock(buffer_.data());
        buffer_size_ = 0;
      }
    }

    while (offset + buffer_.size() <= size) {
      ProcessBlock(data + offset);
      offset += buffer_.size();
    }

    if (offset < size) {
      buffer_size_ = size - offset;
      std::memcpy(buffer_.data(), data + offset, buffer_size_);
    }
  }

  std::array<uint8_t, 20> Finalize() {
    const auto bit_size = total_size_ * 8;
    const auto padding = uint8_t{0x80};
    Update(&padding, 1);

    const uint8_t zeros[64] = {};
    if (buffer_size_ > 56) {
      Update(zeros, buffer_.size() - buffer_size_);
    }
    if (buffer_size_ < 56) {
      Update(zeros, 56 - buffer_size_);
    }

    uint8_t length[8] = {};
    for (auto index = size_t{0}; index < sizeof(length); ++index) {
      length[sizeof(length) - index - 1] =
          static_cast<uint8_t>((bit_size >> (index * 8)) & 0xff);
    }
    Update(length, sizeof(length));

    std::array<uint8_t, 20> digest{};
    for (auto state_index = size_t{0}; state_index < state_.size();
         ++state_index) {
      const auto value = state_[state_index];
      digest[state_index * 4] = static_cast<uint8_t>((value >> 24) & 0xff);
      digest[state_index * 4 + 1] =
          static_cast<uint8_t>((value >> 16) & 0xff);
      digest[state_index * 4 + 2] =
          static_cast<uint8_t>((value >> 8) & 0xff);
      digest[state_index * 4 + 3] = static_cast<uint8_t>(value & 0xff);
    }
    return digest;
  }

 private:
  static uint32_t RotateLeft(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void ProcessBlock(const uint8_t* block) {
    uint32_t words[80] = {};
    for (auto index = size_t{0}; index < 16; ++index) {
      const auto offset = index * 4;
      words[index] =
          (static_cast<uint32_t>(block[offset]) << 24) |
          (static_cast<uint32_t>(block[offset + 1]) << 16) |
          (static_cast<uint32_t>(block[offset + 2]) << 8) |
          static_cast<uint32_t>(block[offset + 3]);
    }
    for (auto index = size_t{16}; index < 80; ++index) {
      words[index] = RotateLeft(words[index - 3] ^ words[index - 8] ^
                                    words[index - 14] ^ words[index - 16],
                                1);
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    for (auto index = size_t{0}; index < 80; ++index) {
      uint32_t function = 0;
      uint32_t constant = 0;
      if (index < 20) {
        function = (b & c) | ((~b) & d);
        constant = 0x5a827999;
      } else if (index < 40) {
        function = b ^ c ^ d;
        constant = 0x6ed9eba1;
      } else if (index < 60) {
        function = (b & c) | (b & d) | (c & d);
        constant = 0x8f1bbcdc;
      } else {
        function = b ^ c ^ d;
        constant = 0xca62c1d6;
      }

      const auto temp =
          RotateLeft(a, 5) + function + e + constant + words[index];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  std::array<uint32_t, 5> state_ = {
      0x67452301,
      0xefcdab89,
      0x98badcfe,
      0x10325476,
      0xc3d2e1f0,
  };
  std::array<uint8_t, 64> buffer_{};
  size_t buffer_size_ = 0;
  uint64_t total_size_ = 0;
};

static std::string ToLowerHex(const std::array<uint8_t, 20>& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    hex.push_back(kHex[(byte >> 4) & 0x0f]);
    hex.push_back(kHex[byte & 0x0f]);
  }
  return hex;
}

static void UpdateSha1(MuonSha1* sha1, const std::vector<uint8_t>& data) {
  if (!data.empty()) {
    sha1->Update(data.data(), data.size());
  }
}

std::string CalculateSha1Hex(const std::vector<uint8_t>& data,
                             const std::vector<uint8_t>& suffix) {
  MuonSha1 sha1;
  UpdateSha1(&sha1, data);
  UpdateSha1(&sha1, suffix);
  return ToLowerHex(sha1.Finalize());
}

bool CalculateFileSha1Hex(const std::filesystem::path& path,
                          std::string* digest) {
  if (digest == nullptr) {
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }

  MuonSha1 sha1;
  std::array<uint8_t, 64 * 1024> buffer{};
  for (;;) {
    input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    const auto read_size = input.gcount();
    if (read_size > 0) {
      sha1.Update(buffer.data(), static_cast<size_t>(read_size));
    }
    if (input.eof()) {
      break;
    }
    if (input.fail()) {
      return false;
    }
  }

  *digest = ToLowerHex(sha1.Finalize());
  return true;
}

}  // namespace muon_internal
