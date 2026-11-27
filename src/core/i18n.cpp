// SPDX-License-Identifier: Apache-2.0
// i18n.cpp - UI language
#include "core/i18n.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace yc::i18n {

namespace {

struct Pair {
  const char* en;
  const char* zh;
};

// Translation table (constant-initialized, so TR is safe even during static initialization)
constexpr Pair kZh[] = {
    {"", ""},  // keeps the array non-empty
#include "core/i18n_zh.inc"
};

// Native names, written as UTF-8 escapes so the source stays ASCII
constexpr const char* kZhName = "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87";  // Traditional Chinese
constexpr const char* kLanguageLabel = "Language / \xE8\xAA\x9E\xE8\xA8\x80";         // "Language" in both languages

#ifdef _WIN32
std::wstring Env(const wchar_t* name) {
  wchar_t buf[1024];
  const DWORD n = ::GetEnvironmentVariableW(name, buf, 1024);
  return n && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

// legacy one-line file (before settings.json); still read when settings.json has no "language"
std::wstring SettingPath() {
  const std::wstring base = Env(L"LOCALAPPDATA");
  return base.empty() ? std::wstring() : base + L"\\ycgrid\\language";
}

std::wstring JsonSettingsPath() {
  const std::wstring base = Env(L"LOCALAPPDATA");
  return base.empty() ? std::wstring() : base + L"\\ycgrid\\settings.json";
}

// "language" of settings.json; read with a tiny scan because this runs during static initialization
// (TR() in namespace-scope tables), before the app's JSON settings are loaded
bool JsonLanguage(std::string* out) {
  const std::wstring p = JsonSettingsPath();
  if (p.empty()) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const size_t k = text.find("\"language\"");
  if (k == std::string::npos) return false;
  const size_t colon = text.find(':', k);
  const size_t q1 = colon == std::string::npos ? colon : text.find('"', colon);
  const size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  *out = text.substr(q1 + 1, q2 - q1 - 1);
  return true;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}
#endif

bool ParseLang(std::string_view v, Lang* out) {
  std::string t(v);
  for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
  if (t == "en" || t == "en-us" || t == "english") {
    *out = Lang::En;
    return true;
  }
  if (t == "zh-tw" || t == "zh" || t == "zh_tw" || t == "tw") {
    *out = Lang::ZhTW;
    return true;
  }
  return false;
}

// Default when nothing is saved: Chinese on a Traditional Chinese Windows, otherwise English
Lang SystemDefault() {
#ifdef _WIN32
  const LANGID id = ::GetUserDefaultUILanguage();
  if (PRIMARYLANGID(id) == LANG_CHINESE && SUBLANGID(id) != SUBLANG_CHINESE_SIMPLIFIED && SUBLANGID(id) != SUBLANG_CHINESE_SINGAPORE) {
    return Lang::ZhTW;
  }
#endif
  return Lang::En;
}

bool ReadSaved(Lang* out) {
#ifdef _WIN32
  std::string json_lang;
  if (JsonLanguage(&json_lang)) {
    if (ParseLang(json_lang, out)) return true;
    if (json_lang.empty() || json_lang == "system") return false;  // follow the Windows display language
  }
  const std::wstring p = SettingPath();
  if (!p.empty()) {
    std::ifstream f(p, std::ios::binary);
    std::string v;
    if (f && std::getline(f, v)) return ParseLang(v, out);
  }
#else
  (void)out;
#endif
  return false;
}

Lang Detect() {
  Lang l = Lang::En;
#ifdef _WIN32
  // command line: --lang xx
  int argc = 0;
  if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc)) {
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::wcscmp(argv[i], L"--lang") == 0 && ParseLang(Narrow(argv[i + 1]), &l)) {
        ::LocalFree(argv);
        return l;
      }
    }
    ::LocalFree(argv);
  }
  if (ParseLang(Narrow(Env(L"YCGRID_LANG")), &l)) return l;
#endif
  if (ReadSaved(&l)) return l;
  return SystemDefault();
}

struct State {
  Lang lang = Lang::En;
  std::unordered_map<std::string_view, const char*> table;
  std::mutex mu;
  std::unordered_map<const void*, const void*> cache;  // address of the string literal -> result
  std::deque<std::string> owned;                       // strings built here (translation + "##id", stripped markers)
  std::deque<std::wstring> owned_w;
  std::unordered_set<std::string> missing;
  std::string missing_path;
};

// The Chinese table, built the first time Chinese is used (at startup, or by SetCurrent on an English system)
void LoadTable(State& st) {
  if (!st.table.empty()) return;
  st.table.reserve(std::size(kZh));
  for (const Pair& p : kZh) {
    if (*p.en) st.table.emplace(p.en, p.zh);
  }
}

State& St() {
  static State* s = [] {
    auto* st = new State();  // never freed: results may be used until the process exits
    st->lang = Detect();
    if (st->lang == Lang::ZhTW) LoadTable(*st);
#ifdef _WIN32
    st->missing_path = Narrow(Env(L"YCGRID_I18N_MISSING"));  // debugging aid: log text without a translation
#endif
    return st;
  }();
  return *s;
}

void LogMissing(State& st, std::string_view label) {
  if (st.missing_path.empty() || label.empty() || !st.missing.insert(std::string(label)).second) return;
  FILE* f = nullptr;
  if (fopen_s(&f, st.missing_path.c_str(), "ab") != 0 || !f) return;
  std::string esc;
  for (char ch : label) {
    if (ch == '\n') esc += "\\n";
    else esc += ch;
  }
  esc += '\n';
  std::fwrite(esc.data(), 1, esc.size(), f);
  std::fclose(f);
}

// Removes "##ctx_..." (up to the next "##" or the end); returns false if there is no marker
bool StripContext(std::string_view text, std::string* out) {
  const size_t c = text.find("##ctx_");
  if (c == std::string_view::npos) return false;
  const size_t end = text.find("##", c + 2);
  *out = std::string(text.substr(0, c));
  if (end != std::string_view::npos) *out += text.substr(end);
  return true;
}

// Translation of the whole text (label, optional ##ctx marker, optional ##id). Returns nullptr when the
// text should be returned unchanged.
const char* Translate(State& st, std::string_view text) {
  std::string stripped;
  const bool has_ctx = StripContext(text, &stripped);
  if (st.lang == Lang::En) {
    if (!has_ctx) return nullptr;
    st.owned.push_back(std::move(stripped));
    return st.owned.back().c_str();
  }
  // Chinese: the key is the label including its context marker; the ImGui id (if any) is appended afterwards
  const size_t first = text.find("##");
  size_t key_end = first;
  if (first != std::string_view::npos && text.substr(first).starts_with("##ctx_")) key_end = text.find("##", first + 2);
  const std::string_view key = key_end == std::string_view::npos ? text : text.substr(0, key_end);
  const std::string_view id = key_end == std::string_view::npos ? std::string_view() : text.substr(key_end);
  auto it = st.table.find(key);
  if (it == st.table.end()) {
    LogMissing(st, key);
    if (!has_ctx) return nullptr;
    st.owned.push_back(std::move(stripped));
    return st.owned.back().c_str();
  }
  if (id.empty()) return it->second;
  st.owned.emplace_back(std::string(it->second) + std::string(id));
  return st.owned.back().c_str();
}

}  // namespace

Lang Current() { return St().lang; }

void SetCurrent(Lang l) {
  State& st = St();
  std::lock_guard<std::mutex> lk(st.mu);
  if (l == Lang::ZhTW) LoadTable(st);
  st.lang = l;
  st.cache.clear();
}

const char* LangCode(Lang l) { return l == Lang::ZhTW ? "zh-TW" : "en"; }
const char* LangName(Lang l) { return l == Lang::ZhTW ? kZhName : "English"; }
const char* LanguageLabel() { return kLanguageLabel; }

Lang SavedLanguage() {
  Lang l = Lang::En;
  return ReadSaved(&l) ? l : SystemDefault();
}

namespace {
bool (*g_language_saver)(const char*) = nullptr;
}

void SetLanguageSaver(bool (*saver)(const char* code)) { g_language_saver = saver; }

bool SaveLanguage(Lang l) {
  if (g_language_saver) return g_language_saver(LangCode(l));
#ifdef _WIN32
  const std::wstring p = SettingPath();
  if (p.empty()) return false;
  ::CreateDirectoryW((Env(L"LOCALAPPDATA") + L"\\ycgrid").c_str(), nullptr);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << LangCode(l) << "\n";
  return static_cast<bool>(f);
#else
  (void)l;
  return false;
#endif
}

const char* Tr(const char* text) {
  if (!text) return text;
  State& st = St();
  // English without a context marker: nothing to do (the common case, no lock)
  if (st.lang == Lang::En && !std::strstr(text, "##ctx_")) return text;
  std::lock_guard<std::mutex> lk(st.mu);
  auto it = st.cache.find(text);
  if (it != st.cache.end()) return static_cast<const char*>(it->second);
  const char* tr = Translate(st, text);
  const char* out = tr ? tr : text;
  st.cache.emplace(text, out);
  return out;
}

const char* SourceOf(const char* text) {
  if (!text) return text;
  State& st = St();
  if (st.lang == Lang::En) return text;
  std::lock_guard<std::mutex> lk(st.mu);
  static std::unordered_map<std::string_view, const char*> rev;  // translation -> its English text
  if (rev.empty()) {
    // a translation of several texts ("is greater than", "Greater Than") takes a label's: the one with a capital first,
    // then the shortest, then the first in order (the same every run)
    auto better = [](std::string_view a, std::string_view b) {
      const bool ua = !a.empty() && std::isupper(static_cast<unsigned char>(a[0])), ub = !b.empty() && std::isupper(static_cast<unsigned char>(b[0]));
      if (ua != ub) return ua;
      if (a.size() != b.size()) return a.size() < b.size();
      return a < b;
    };
    for (const auto& [en, zh] : st.table) {
      auto [it, fresh] = rev.emplace(std::string_view(zh), en.data());
      if (!fresh && better(en, it->second)) it->second = en.data();
    }
  }
  const auto it = rev.find(std::string_view(text));
  return it != rev.end() ? it->second : text;
}

const wchar_t* TrW(const wchar_t* text) {
  if (!text) return text;
  State& st = St();
  if (st.lang == Lang::En && !std::wcsstr(text, L"##ctx_")) return text;
#ifdef _WIN32
  std::lock_guard<std::mutex> lk(st.mu);
  auto it = st.cache.find(text);
  if (it != st.cache.end()) return static_cast<const wchar_t*>(it->second);
  const char* tr = Translate(st, Narrow(text));
  const wchar_t* out = text;
  if (tr) {
    st.owned_w.emplace_back(Widen(tr));
    out = st.owned_w.back().c_str();
  }
  st.cache.emplace(text, out);
  return out;
#else
  return text;
#endif
}

}  // namespace yc::i18n
