// SPDX-License-Identifier: Apache-2.0
// i18n.h - UI language (English / Traditional Chinese)
//
// UI text in the source code is always English, wrapped in TR("...") (TRW(L"...") for wide strings).
// In Chinese mode the text is looked up in i18n_zh.inc (generated from i18n/zh-TW.json); text without an
// entry is shown as is.
//
// ImGui IDs: for "Label##id" / "Label###id" only the label is translated, the id is kept, so window
// positions and the docking layout do not depend on the language.
// Context markers: when one English text needs different translations, write "Top##ctx_align". The
// "##ctx_..." part is a translation key only and is removed before the text is returned.
//
// The language is chosen at startup (like Excel, switching needs a restart):
//   command line --lang en|zh-TW  >  env YCGRID_LANG  >  %LOCALAPPDATA%\ycgrid\language  >  Windows display language
#pragma once

#include <string>

namespace yc::i18n {

enum class Lang { En = 0, ZhTW = 1 };

Lang Current();
void SetCurrent(Lang l);  // tests: switch the language at run time (the app picks it once at startup)
const char* LangCode(Lang l);  // "en", "zh-TW"
const char* LangName(Lang l);  // each language's own name (UTF-8): "English", Traditional Chinese in Chinese
const char* LanguageLabel();   // "Language / ..." in both languages, for the language picker
bool SaveLanguage(Lang l);     // saves the choice (used from the next start); see SetLanguageSaver
// The language is read from "language" in %LOCALAPPDATA%\ycgrid\settings.json (older installs: the one-line
// %LOCALAPPDATA%\ycgrid\language file). The app installs a saver so SaveLanguage writes settings.json.
void SetLanguageSaver(bool (*saver)(const char* code));
Lang SavedLanguage();          // the saved choice (may differ from Current() until restart)

const char* Tr(const char* text);
const wchar_t* TrW(const wchar_t* text);

}  // namespace yc::i18n

#define TR(s) ::yc::i18n::Tr(s)
#define TRW(s) ::yc::i18n::TrW(s)
