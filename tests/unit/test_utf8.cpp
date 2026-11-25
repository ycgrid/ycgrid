// SPDX-License-Identifier: Apache-2.0
// Text cut by characters, never in the middle of a UTF-8 sequence (sheet names, undo labels, previews)
#include <catch2/catch_test_macros.hpp>

#include "core/utf8.h"

#include <string>

using namespace yc::core;

namespace {
const std::string kTsmc = "\xE5\x8F\xB0\xE7\xA9\x8D\xE9\x9B\xBB";  // three CJK characters, 9 bytes
}

TEST_CASE("UTF-8: characters are counted, not bytes") {
  CHECK(Utf8Length("") == 0);
  CHECK(Utf8Length("abc") == 3);
  CHECK(Utf8Length(kTsmc) == 3);
  CHECK(Utf8Length("a" + kTsmc + "b") == 5);
  CHECK(Utf8Length("\xC3\xA9") == 1);          // é (2 bytes)
  CHECK(Utf8Length("\xF0\x9F\x98\x80") == 1);  // an emoji (4 bytes)
}

TEST_CASE("UTF-8: a prefix by bytes ends on a character boundary") {
  const std::string mixed = "x" + kTsmc + kTsmc;  // 1 + 18 bytes
  CHECK(Utf8Prefix(mixed, 100) == mixed);
  CHECK(Utf8Prefix(mixed, 19) == mixed);
  CHECK(Utf8Prefix(mixed, 18) == "x" + kTsmc + kTsmc.substr(0, 6));  // byte 18 is inside the last character: it goes
  CHECK(Utf8Prefix(mixed, 17) == "x" + kTsmc + kTsmc.substr(0, 6));
  CHECK(Utf8Prefix(mixed, 16) == "x" + kTsmc + kTsmc.substr(0, 6));
  CHECK(Utf8Prefix(mixed, 4) == "x" + kTsmc.substr(0, 3));
  CHECK(Utf8Prefix(mixed, 3) == "x");
  CHECK(Utf8Prefix(mixed, 1) == "x");
  CHECK(Utf8Prefix(mixed, 0).empty());
  CHECK(Utf8Prefix("abcdef", 3) == "abc");
  // every prefix is complete UTF-8: no continuation byte is left without its lead
  for (size_t n = 0; n <= mixed.size(); ++n) {
    const std::string_view p = Utf8Prefix(mixed, n);
    CHECK(p.size() <= Utf8Length(p) * 3);
    CHECK((p.empty() || Utf8Lead(static_cast<unsigned char>(mixed[p.size() < mixed.size() ? p.size() : 0]))));
  }
}

TEST_CASE("UTF-8: the first n characters") {
  const std::string mixed = "a" + kTsmc + "b";
  CHECK(Utf8First(mixed, 0).empty());
  CHECK(Utf8First(mixed, 1) == "a");
  CHECK(Utf8First(mixed, 2) == "a" + kTsmc.substr(0, 3));
  CHECK(Utf8First(mixed, 4) == "a" + kTsmc);
  CHECK(Utf8First(mixed, 5) == mixed);
  CHECK(Utf8First(mixed, 50) == mixed);
}
