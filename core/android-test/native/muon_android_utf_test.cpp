#include "muon_android_utf.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
  const auto utf16 = std::u16string{u'A', 0, u'日', 0xd83d, 0xde00, u'Z'};
  const auto utf8 = std::string("A\0\xe6\x97\xa5\xf0\x9f\x98\x80Z", 10);
  assert(MuonAndroidEncodeUtf8(utf16) == utf8);
  assert(MuonAndroidDecodeUtf8(utf8) == utf16);
  assert(MuonAndroidEncodeUtf8({}).empty());
  assert(MuonAndroidDecodeUtf8({}).empty());
  for (const auto& malformed : {std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
      std::string("\xf4\x90\x80\x80"), std::string("\xe3\x81"), std::string("\x80")}) {
    bool rejected = false;
    try { MuonAndroidDecodeUtf8(malformed); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  for (const auto& malformed : {std::u16string{0xd800}, std::u16string{0xdc00}, std::u16string{0xd800, u'A'}}) {
    bool rejected = false;
    try { MuonAndroidEncodeUtf8(malformed); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  std::cout << "Android UTF-8/UTF-16 preserves emoji and embedded NUL, rejects malformed input\n";
}
