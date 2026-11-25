// SPDX-License-Identifier: Apache-2.0
// Coverage of the UI translation (i18n.cpp): every case starts in English (test_listener.cpp), so the Chinese lookups,
// the context markers, the wide-string path and the saved choice are run here on purpose. Nothing is written to the
// user's settings: saving goes through a test saver.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "core/i18n.h"

namespace i18n = yc::i18n;

namespace {

struct Chinese {
  i18n::Lang old = i18n::Current();
  Chinese() { i18n::SetCurrent(i18n::Lang::ZhTW); }
  ~Chinese() { i18n::SetCurrent(old); }
};

bool NonAscii(const char* s) {
  for (const char* p = s; *p; ++p) {
    if (static_cast<unsigned char>(*p) >= 0x80) return true;
  }
  return false;
}

const char* g_saved = nullptr;
bool TestSaver(const char* code) {
  g_saved = code;
  return true;
}

}  // namespace

TEST_CASE("Translation: Chinese lookups, ids after ##, context markers, text without a translation") {
  CHECK(std::string(i18n::Tr("Cancel")) == "Cancel");  // English: as written
  CHECK(std::string(i18n::Tr("Month##ctx_axis")) == "Month");  // the context marker goes in English too
  Chinese zh;  // builds the Chinese table even where YCGrid started in English (CI runs English Windows)
  // the cache is keyed by the literal's address (a debug build doesn't merge equal literals): ask with one pointer
  static const char* const kCancel = "Cancel";
  const char* cancel = i18n::Tr(kCancel);
  CHECK(std::strcmp(cancel, "Cancel") != 0);
  CHECK(NonAscii(cancel));
  CHECK(i18n::Tr(kCancel) == cancel);  // cached: the same pointer
  const std::string with_id = i18n::Tr("Cancel##dlg_id_12");
  CHECK(with_id == std::string(cancel) + "##dlg_id_12");  // an id after ## stays for ImGui
  const std::string ctx = i18n::Tr("Month##ctx_axis");
  CHECK(ctx.find("##ctx_") == std::string::npos);
  CHECK(std::string(i18n::Tr("A text no table has, kept as written 7f3a")) == "A text no table has, kept as written 7f3a");
  CHECK(std::string(i18n::Tr("No such text either##ctx_x")) == "No such text either");
  CHECK(i18n::Tr(nullptr) == nullptr);
  // the wide-string path (Win32 dialogs)
  static const wchar_t* const kWCancel = L"Cancel";
  const wchar_t* wcancel = i18n::TrW(kWCancel);
  CHECK(std::wcscmp(wcancel, L"Cancel") != 0);
  CHECK(i18n::TrW(kWCancel) == wcancel);
  CHECK(std::wcscmp(i18n::TrW(L"Untranslated wide text 9c1e"), L"Untranslated wide text 9c1e") == 0);
}

TEST_CASE("Languages: codes, names, the label and the saved choice") {
  CHECK(std::string(i18n::LangCode(i18n::Lang::En)) == "en");
  CHECK(std::string(i18n::LangCode(i18n::Lang::ZhTW)) == "zh-TW");
  CHECK(std::string(i18n::LangName(i18n::Lang::En)) == "English");
  CHECK(NonAscii(i18n::LangName(i18n::Lang::ZhTW)));
  CHECK(std::strlen(i18n::LanguageLabel()) > 0);
  const i18n::Lang saved = i18n::SavedLanguage();
  CHECK((saved == i18n::Lang::En || saved == i18n::Lang::ZhTW));
  i18n::SetLanguageSaver(TestSaver);
  CHECK(i18n::SaveLanguage(i18n::Lang::ZhTW));
  CHECK(std::string(g_saved) == "zh-TW");
  CHECK(i18n::SaveLanguage(i18n::Lang::En));
  CHECK(std::string(g_saved) == "en");
  i18n::SetLanguageSaver(nullptr);
}
