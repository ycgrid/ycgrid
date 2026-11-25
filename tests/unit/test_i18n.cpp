// SPDX-License-Identifier: Apache-2.0
// UI language: translation lookup (properties that hold whatever the current language is)
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"

#include <cstring>
#include <string_view>

namespace {
struct ZhPair {
  const char* en;
  const char* zh;
};
constexpr ZhPair kZhAll[] = {
#include "core/i18n_zh.inc"
};

// strict UTF-8: no stray continuation byte, no cut sequence, no overlong form
bool ValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size() || (n == 2 && b < 0xC2)) return false;
    for (size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}
}  // namespace

TEST_CASE("Every Chinese translation is valid UTF-8") {
  // zh-TW.json's values are C string literals: a single backslash before a Chinese comma became the escape \343
  // followed by two stray bytes, and every ui.state() in the Chinese UI failed to decode after Alt+F11 made a project
  for (const ZhPair& p : kZhAll) {
    INFO(p.en);
    CHECK(ValidUtf8(p.zh));
  }
}

TEST_CASE("TR: ImGui's ##id is kept, text without a translation comes back unchanged") {
  const char* a = TR("確定##yc_test_ok");
  REQUIRE(a != nullptr);
  CHECK(std::string_view(a).ends_with("##yc_test_ok"));
  CHECK(std::strcmp(TR("確定##yc_test_ok"), a) == 0);
  const char* raw = "這段文字沒有翻譯 12345";
  CHECK(std::strcmp(TR(raw), raw) == 0);
  CHECK(TR(nullptr) == nullptr);
  CHECK(std::wstring_view(TRW(L"沒有翻譯的寬字元")) == L"沒有翻譯的寬字元");
  CHECK(std::string_view(yc::i18n::LangCode(yc::i18n::Lang::En)) == "en");
}
