/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_default_title_bar_icon_generated.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "%s\n", message);
  return false;
}

static std::vector<uint8_t> ReadBinaryFile(const char* path) {
  auto stream = std::ifstream(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>());
}

static uint32_t ReadBigEndianUint32(const std::vector<uint8_t>& data,
                                    size_t offset) {
  return (static_cast<uint32_t>(data[offset]) << 24) |
         (static_cast<uint32_t>(data[offset + 1]) << 16) |
         (static_cast<uint32_t>(data[offset + 2]) << 8) |
         static_cast<uint32_t>(data[offset + 3]);
}

static bool TestGeneratedDefaultTitleBarIconMatchesSource() {
  const auto source = ReadBinaryFile(EXPECTED_DEFAULT_TITLE_BAR_ICON_PNG);
  const auto generated = std::vector<uint8_t>(
      muon_internal::kMuonDefaultTitleBarIconPng.begin(),
      muon_internal::kMuonDefaultTitleBarIconPng.end());

  return Expect(!source.empty(), "default title bar icon source is empty") &&
         Expect(generated == source,
                "generated default title bar icon does not match source PNG");
}

static bool TestGeneratedDefaultTitleBarIconPngShape() {
  const auto generated = std::vector<uint8_t>(
      muon_internal::kMuonDefaultTitleBarIconPng.begin(),
      muon_internal::kMuonDefaultTitleBarIconPng.end());
  const auto png_signature =
      std::vector<uint8_t>{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};

  return Expect(generated.size() >= 26,
                "generated default title bar icon PNG is too small") &&
         Expect(std::vector<uint8_t>(generated.begin(),
                                     generated.begin() + png_signature.size()) ==
                    png_signature,
                "generated default title bar icon is not a PNG") &&
         Expect(ReadBigEndianUint32(generated, 16) == 256,
                "generated default title bar icon width is not 256") &&
         Expect(ReadBigEndianUint32(generated, 20) == 256,
                "generated default title bar icon height is not 256") &&
         Expect(generated[24] == 8,
                "generated default title bar icon bit depth is not 8") &&
         Expect(generated[25] == 6,
                "generated default title bar icon color type is not RGBA");
}

int main() {
  return TestGeneratedDefaultTitleBarIconMatchesSource() &&
                 TestGeneratedDefaultTitleBarIconPngShape()
             ? 0
             : 1;
}
