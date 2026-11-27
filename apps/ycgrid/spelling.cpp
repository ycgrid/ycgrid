// SPDX-License-Identifier: Apache-2.0
// spelling.cpp - Review > Spelling (F7): the Windows spell checker over the text cells of a sheet or selection
#include "app_state.h"
#include "core/i18n.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "engine/tile_iter.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "theme/theme.h"
#include "yc_ui_items.h"

#include "imgui.h"

#include <windows.h>
#include <spellcheck.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

namespace fs = std::filesystem;

// ---- the Windows spell checker (one per language, created on the UI thread) ----

struct Checker {
  ISpellChecker* sc = nullptr;
  bool tried = false;
};

std::map<std::wstring, Checker>& Checkers() {
  static std::map<std::wstring, Checker> m;
  return m;
}

// YCGrid's own dictionary ("Add to Dictionary"): one word per line next to settings.json; the Windows user dictionary is left alone
fs::path UserDictionaryPath() { return fs::path(SettingsPath()).parent_path() / L"custom.dic"; }

std::set<std::wstring>& UserWords() {
  static std::set<std::wstring> words;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    std::ifstream f(UserDictionaryPath(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (!line.empty()) words.insert(FromUtf8(line));
    }
  }
  return words;
}

ISpellChecker* CheckerFor(const std::wstring& lang) {
  Checker& c = Checkers()[lang];
  if (c.tried) return c.sc;
  c.tried = true;
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ISpellCheckerFactory* f = nullptr;
  if (FAILED(::CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) || !f) return nullptr;
  BOOL ok = FALSE;
  if (SUCCEEDED(f->IsSupported(lang.c_str(), &ok)) && ok) f->CreateSpellChecker(lang.c_str(), &c.sc);
  f->Release();
  if (c.sc) {
    for (const auto& w : UserWords()) c.sc->Ignore(w.c_str());
  }
  return c.sc;
}

std::vector<std::wstring> Suggest(ISpellChecker* sc, const std::wstring& word) {
  std::vector<std::wstring> out;
  IEnumString* e = nullptr;
  if (!sc || FAILED(sc->Suggest(word.c_str(), &e)) || !e) return out;
  LPOLESTR s = nullptr;
  while (out.size() < 12 && e->Next(1, &s, nullptr) == S_OK) {
    out.emplace_back(s);
    ::CoTaskMemFree(s);
  }
  e->Release();
  return out;
}

struct SpellError {
  size_t start = 0, len = 0;  // UTF-16 offsets
  CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
  std::wstring replacement;
};

std::vector<SpellError> Errors(ISpellChecker* sc, const std::wstring& text) {
  std::vector<SpellError> out;
  IEnumSpellingError* e = nullptr;
  if (!sc || text.empty() || FAILED(sc->Check(text.c_str(), &e)) || !e) return out;
  ISpellingError* err = nullptr;
  while (e->Next(&err) == S_OK) {
    SpellError x;
    ULONG st = 0, ln = 0;
    err->get_StartIndex(&st);
    err->get_Length(&ln);
    err->get_CorrectiveAction(&x.action);
    LPWSTR rep = nullptr;
    if (SUCCEEDED(err->get_Replacement(&rep)) && rep) {
      x.replacement = rep;
      ::CoTaskMemFree(rep);
    }
    x.start = st;
    x.len = ln;
    out.push_back(std::move(x));
    err->Release();
  }
  e->Release();
  return out;
}

// ---- the Spelling window ----

struct SpellUi {
  bool open = false;
  std::wstring lang = L"en-US";
  uint32_t sheet = 0;
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  size_t idx = 0, offset = 0;  // next cell; UTF-16 offset in the current cell's text
  std::wstring text;           // the current cell's text (with the changes made so far)
  bool has_error = false, repeated = false;
  size_t start = 0, len = 0;
  std::vector<std::string> suggestions;
  int sel = 0;
  char change_to[256] = {};
  std::set<std::wstring> ignore_all;
  std::map<std::wstring, std::wstring> change_all;
  int changes = 0;
  // Undo Last: each step of this check (the word's place, the text before a change, an Ignore All / Change All added)
  struct Step {
    size_t idx = 0, start = 0;
    std::wstring before;
    bool changed = false;
    std::wstring ignored, changed_all;
  };
  std::vector<Step> steps;
  bool options_request = false;
  bool text_ready = false;  // Undo Last put the text back: Advance doesn't read the cell (the change is still on its way)
};

SpellUi& U() {
  static SpellUi u;
  return u;
}

std::wstring Lower(std::wstring s) {
  for (auto& ch : s) ch = static_cast<wchar_t>(::towlower(ch));
  return s;
}

// what is checked: cells (row, column), and like Excel F7 also the notes (row | kNoteBit, column) and the text of the
// shapes and text boxes (kShapeRow, shape id)
constexpr uint32_t kShapeRow = 0xFFFFFFFFu, kNoteBit = 0x80000000u;

std::wstring CellText(const AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh) return {};
  if (r == kShapeRow) {
    for (const auto& x : *sh->shapes) {
      if (x.id == c) return FromUtf8(x.text);
    }
    return {};
  }
  if (r & kNoteBit) {
    const auto it = sh->notes->find((uint64_t{r & ~kNoteBit} << 16) | c);
    return it != sh->notes->end() && !it->second.threaded ? FromUtf8(it->second.text) : std::wstring();
  }
  const core::Value v = sh->GetValue(r, c);
  if (!v.IsString() || sh->GetFormula(r, c) != 0) return {};
  return FromUtf8(std::string(v.str()));
}

void WriteCell(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::wstring& text) {
  const std::string t = ToUtf8(text);
  if (r == kShapeRow) {
    s.engine->PostUser(TR("Spelling"), [sheet, c, t](Document& d) { d.UpdateShape(sheet, c, [&](model::ShapeObj& x) { x.text = t; }); });
  } else if (r & kNoteBit) {
    std::string author;
    if (const model::Sheet* sh = s.snap->Sheet(sheet)) {
      const auto it = sh->notes->find((uint64_t{r & ~kNoteBit} << 16) | c);
      if (it != sh->notes->end()) author = it->second.author;
    }
    const uint32_t row = r & ~kNoteBit;
    s.engine->PostUser(TR("Spelling"), [sheet, row, c, t, author](Document& d) { d.SetNote(sheet, row, c, author, t); });
  } else {
    s.engine->PostUser(TR("Spelling"), [sheet, r, c, t](Document& d) { d.SetValue(sheet, r, c, core::Value::String(t), engine::kSrcUser); });
  }
  ++U().changes;
}

// replace [start, start + len) in the current cell and continue after the replacement
void Replace(AppState& s, const std::wstring& with) {
  SpellUi& u = U();
  size_t a = u.start, n = u.len;
  if (with.empty() && a > 0 && u.text[a - 1] == L' ') --a, ++n;  // a deleted repeated word takes its space along
  u.text = u.text.substr(0, a) + with + u.text.substr(a + n);
  const auto [r, c] = u.cells[u.idx];
  WriteCell(s, u.sheet, r, c, u.text);
  u.offset = a + with.size();
}

// File > Options > Proofing: words the checker skips (UPPERCASE, with numbers, Internet and file addresses)
bool Skipped(const std::wstring& text, size_t start, size_t len) {
  const Settings& cf = Config();
  const std::wstring w = text.substr(start, len);
  bool upper = false, lower = false, digit = false;
  for (wchar_t ch : w) {
    if (::iswupper(ch)) upper = true;
    if (::iswlower(ch)) lower = true;
    if (::iswdigit(ch)) digit = true;
  }
  if (cf.spell_ignore_upper && upper && !lower) return true;
  if (cf.spell_ignore_numbers && digit) return true;
  if (cf.spell_ignore_internet) {
    // the "word" in its run of non-spaces: www.x, http://, a path or an e-mail address
    size_t a = start, b = start + len;
    while (a > 0 && !::iswspace(text[a - 1])) --a;
    while (b < text.size() && !::iswspace(text[b])) ++b;
    const std::wstring run = text.substr(a, b - a);
    if (run.find(L"://") != std::wstring::npos || run.rfind(L"www.", 0) == 0 || run.find(L'@') != std::wstring::npos || run.find(L'\\') != std::wstring::npos)
      return true;
  }
  return false;
}

// find the next error from the current position; false when the check is complete
bool Advance(AppState& s) {
  SpellUi& u = U();
  ISpellChecker* sc = CheckerFor(u.lang);
  u.has_error = false;
  if (!sc) return false;
  while (u.idx < u.cells.size()) {
    const auto [r, c] = u.cells[u.idx];
    if (u.offset == 0 && !u.text_ready) u.text = CellText(s, u.sheet, r, c);
    u.text_ready = false;
    bool again = true;
    while (again) {
      again = false;
      for (const SpellError& e : Errors(sc, u.text)) {
        if (e.start < u.offset || e.start + e.len > u.text.size()) continue;
        const std::wstring word = u.text.substr(e.start, e.len);
        if (u.ignore_all.count(Lower(word))) continue;
        if (e.action == CORRECTIVE_ACTION_DELETE ? !Config().spell_repeated : Skipped(u.text, e.start, e.len)) continue;
        u.start = e.start;
        u.len = e.len;
        if (auto it = u.change_all.find(Lower(word)); it != u.change_all.end()) {
          Replace(s, it->second);
          again = true;  // offsets moved: check the cell again from the new position
          break;
        }
        u.has_error = true;
        // the cell with the word is selected and brought into view, as Excel does
        if (const model::Sheet* sh = s.snap ? s.snap->Sheet(u.sheet) : nullptr; sh && s.focus_sheet == u.sheet && r != kShapeRow && !(r & kNoteBit)) {
          render::SelectCell(s.FocusView(), r, c, false);
          render::EnsureVisible(*sh, s.FocusView(), r, c);
        }
        u.repeated = e.action == CORRECTIVE_ACTION_DELETE;
        u.suggestions.clear();
        if (e.action == CORRECTIVE_ACTION_REPLACE && !e.replacement.empty()) u.suggestions.push_back(ToUtf8(e.replacement));
        if (!u.repeated) {
          for (const auto& w : Suggest(sc, word)) {
            const std::string x = ToUtf8(w);
            if (std::find(u.suggestions.begin(), u.suggestions.end(), x) == u.suggestions.end()) u.suggestions.push_back(x);
          }
        }
        u.sel = 0;
        std::snprintf(u.change_to, sizeof u.change_to, "%s", u.repeated ? "" : (u.suggestions.empty() ? ToUtf8(word).c_str() : u.suggestions[0].c_str()));
        return true;
      }
    }
    ++u.idx;
    u.offset = 0;
  }
  return false;
}

void Finish(AppState& s) {
  SpellUi& u = U();
  u.open = false;
  s.ShowMessage("YCGrid", u.changes ? TR("The spelling check is complete. Changes were made.") : TR("The spelling check is complete."));
}

void Start(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  SpellUi& u = U();
  if (!CheckerFor(u.lang)) {
    s.ShowError("YCGrid", TR("The Windows spell checker for this language isn't available."));
    return;
  }
  u.sheet = sh->id;
  u.cells.clear();
  u.idx = u.offset = 0;
  u.changes = 0;
  u.steps.clear();
  // a selection of several cells is checked; a single cell checks the whole sheet, like Excel
  std::vector<Rect> areas = SelAreas(s);
  bool single = areas.size() == 1 && areas[0].Count() == 1;
  if (single) {
    if (!sh->has_used && sh->shapes->empty() && sh->notes->empty()) {
      s.ShowMessage("YCGrid", TR("The spelling check is complete."));
      return;
    }
    areas = sh->has_used ? std::vector<Rect>{sh->used} : std::vector<Rect>{};
  }
  for (const Rect& a : areas) {
    const Rect b{a.r0, a.c0, std::min(a.r1, sh->has_used ? sh->used.r1 : a.r0), std::min(a.c1, sh->has_used ? sh->used.c1 : a.c0)};
    // the cells holding a value only: a value in XFD1048576 makes the used range the whole sheet, and a walk of
    // every cell of it froze F7 for minutes
    engine::ForEachValueRowMajor(*sh, b, [&](uint32_t r, uint32_t c, const core::Value& v) {
      if (v.IsString() && sh->GetFormula(r, c) == 0 && !sh->SpillAt(r, c)) u.cells.emplace_back(r, c);
      return true;
    });
  }
  if (single) {
    // Excel starts at the active cell and goes on from the top when it reaches the end
    const auto& sel = s.FocusView().sel;
    size_t first = 0;
    while (first < u.cells.size() && (u.cells[first].first < sel.act_r || (u.cells[first].first == sel.act_r && u.cells[first].second < sel.act_c))) ++first;
    std::rotate(u.cells.begin(), u.cells.begin() + static_cast<std::ptrdiff_t>(first), u.cells.end());
    // then the notes and the text of the shapes and text boxes (Excel's F7 checks them too)
    for (const auto& [k, n] : *sh->notes) {
      if (!n.threaded && !n.text.empty()) u.cells.emplace_back(static_cast<uint32_t>(k >> 16) | kNoteBit, static_cast<uint32_t>(k & 0xFFFF));
    }
    for (const auto& x : *sh->shapes) {
      if (!x.text.empty()) u.cells.emplace_back(kShapeRow, x.id);
    }
  }
  if (!Advance(s)) {
    s.ShowMessage("YCGrid", TR("The spelling check is complete."));
    return;
  }
  u.open = true;
}

}  // namespace

// a word against the Windows spell checker (UI thread); suggestions when it is misspelled
bool SpellCheckWord(const std::string& lang, const std::string& word, std::vector<std::string>* suggestions) {
  ISpellChecker* sc = CheckerFor(FromUtf8(lang.empty() ? std::string("en-US") : lang));
  if (!sc) return true;
  const std::wstring w = FromUtf8(word);
  const bool ok = Errors(sc, w).empty();
  if (!ok && suggestions) {
    for (const auto& x : Suggest(sc, w)) suggestions->push_back(ToUtf8(x));
  }
  return ok;
}

// AutoCorrect's replacements, each with Delete (the spelling dialog's Options and File > Options > Proofing)
void DrawAutoCorrectList() {
  // (the list is read again whenever the window it is in appears)
  static const std::vector<std::pair<std::string, std::string>>* shown = nullptr;
  static std::vector<std::pair<std::string, std::string>> list;
  if (ImGui::IsWindowAppearing() || !shown) list = AutoCorrectList(), shown = &list;
  ImGui::TextDisabled(TR("%d replacements"), static_cast<int>(list.size()));
  if (ImGui::BeginListBox("##ac", ImVec2(S(320), ImGui::GetTextLineHeightWithSpacing() * 8))) {
    int del = -1;
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(list.size()));
    while (clip.Step()) {
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        ImGui::PushID(i);
        ImGui::TextUnformatted((list[static_cast<size_t>(i)].first + "  \xE2\x86\x92  " + list[static_cast<size_t>(i)].second).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Delete"))) del = i;
        ImGui::PopID();
      }
    }
    ImGui::EndListBox();
    if (del >= 0) {
      AutoCorrectDelete(list[static_cast<size_t>(del)].first);
      list = AutoCorrectList();
    }
  }
}

void DrawSpellingWindow(AppState& s) {
  SpellUi& u = U();
  if (!u.open) return;
  if (!u.has_error || u.idx >= u.cells.size()) {
    Finish(s);
    return;
  }
  ImGui::SetNextWindowSize(ImVec2(S(460), 0), ImGuiCond_Appearing);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  // the dictionary's language by its name (en-US: English (United States))
  static const std::pair<const wchar_t*, const char*> kLangs[] = {{L"en-US", TR("English (United States)")}, {L"en-GB", TR("English (United Kingdom)")},
                                                                   {L"fr-FR", TR("French (France)")},           {L"de-DE", TR("German (Germany)")},
                                                                   {L"es-ES", TR("Spanish (Spain)")},           {L"it-IT", TR("Italian (Italy)")},
                                                                   {L"pt-BR", TR("Portuguese (Brazil)")},       {L"nl-NL", TR("Dutch (Netherlands)")}};
  std::string lang_name = ToUtf8(u.lang);
  for (const auto& [tag, name] : kLangs) {
    if (u.lang == tag) lang_name = name;
  }
  const std::string title = std::string(TR("Spelling: ")) + lang_name + "###yc_spelling";
  KeepOpenedWidth(title.c_str());
  if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    NoteOpenedWidth();
    const auto [r, c] = u.cells[u.idx];
    const std::wstring word = u.text.substr(u.start, u.len);
    ImGui::TextDisabled("%s", (core::CellA1(r, c) + ":").c_str());
    // the cell text with the word marked
    ImGui::TextWrapped("%s[%s]%s", ToUtf8(u.text.substr(0, u.start)).c_str(), ToUtf8(word).c_str(), ToUtf8(u.text.substr(u.start + u.len)).c_str());
    ImGui::Text("%s %s", u.repeated ? TR("Repeated word:") : TR("Not in Dictionary:"), ToUtf8(word).c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(TR("Change to:"));
    ImGui::SetNextItemWidth(S(300));
    ImGui::InputText("##change_to", u.change_to, sizeof u.change_to);
    ImGui::TextUnformatted(TR("Suggestions:"));
    if (ImGui::BeginListBox("##sugg", ImVec2(S(300), ImGui::GetTextLineHeightWithSpacing() * 6))) {
      if (u.suggestions.empty()) ImGui::TextDisabled("%s", u.repeated ? TR("(Delete the repeated word)") : TR("(No Suggestions)"));
      for (int i = 0; i < static_cast<int>(u.suggestions.size()); ++i) {
        if (ImGui::Selectable(u.suggestions[static_cast<size_t>(i)].c_str(), u.sel == i)) {
          u.sel = i;
          std::snprintf(u.change_to, sizeof u.change_to, "%s", u.suggestions[static_cast<size_t>(i)].c_str());
        }
      }
      ImGui::EndListBox();
    }
    const ImVec2 bw(S(140), 0);
    bool next = false;
    // every action is a step Undo Last can take back
    auto step = [&](bool changed) {
      SpellUi::Step st;
      st.idx = u.idx;
      st.start = u.start;
      st.before = u.text;
      st.changed = changed;
      u.steps.push_back(std::move(st));
    };
    ImGui::BeginGroup();
    if (ImGui::Button(TR("Ignore Once"), bw)) {
      step(false);
      u.offset = u.start + u.len;
      next = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Ignore All"), bw)) {
      step(false);
      u.steps.back().ignored = Lower(word);
      u.ignore_all.insert(Lower(word));
      u.offset = u.start + u.len;
      next = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Add to Dictionary"), bw)) {
      // YCGrid's own list (custom.dic next to settings.json)
      if (UserWords().insert(word).second) {
        std::error_code ec;
        fs::create_directories(UserDictionaryPath().parent_path(), ec);
        std::ofstream f(UserDictionaryPath(), std::ios::binary | std::ios::app);
        f << ToUtf8(word) << "\r\n";
      }
      for (auto& [k, ch] : Checkers()) {
        if (ch.sc) ch.sc->Ignore(word.c_str());
      }
      u.offset = u.start + u.len;
      next = true;
    }
    if (ImGui::Button(u.repeated ? TR("Delete") : TR("Change"), bw)) {
      step(true);
      Replace(s, FromUtf8(u.change_to));
      next = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Change All"), bw)) {
      step(true);
      u.steps.back().changed_all = Lower(word);
      const std::wstring to = FromUtf8(u.change_to);
      u.change_all[Lower(word)] = to;
      Replace(s, to);
      next = true;
    }
    ImGui::SameLine();
    // AutoCorrect: the word and its correction go to AutoCorrect's list (typing it later corrects it), and it is changed
    ImGui::BeginDisabled(u.repeated || !u.change_to[0]);
    if (ImGui::Button(TR("AutoCorrect"), bw)) {
      AutoCorrectAdd(ToUtf8(word), u.change_to);
      step(true);
      Replace(s, FromUtf8(u.change_to));
      next = true;
    }
    ImGui::EndDisabled();
    // Options... (File > Options > Proofing), Undo Last, Cancel (Close once something changed)
    if (ImGui::Button(TR("Options..."), bw)) u.options_request = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(u.steps.empty());
    if (ImGui::Button(TR("Undo Last"), bw) && !u.steps.empty()) {
      const SpellUi::Step st = u.steps.back();
      u.steps.pop_back();
      if (!st.ignored.empty()) u.ignore_all.erase(st.ignored);
      if (!st.changed_all.empty()) u.change_all.erase(st.changed_all);
      u.idx = st.idx;
      if (st.changed) {
        const auto [ur, uc] = u.cells[u.idx];
        WriteCell(s, u.sheet, ur, uc, st.before);
        u.changes = std::max(0, u.changes - 2);
      }
      u.text = st.before;
      u.offset = st.start;
      u.text_ready = true;
      // the word is checked again where it was (the text as it was before the step)
      const size_t keep_idx = u.idx;
      if (!Advance(s) || u.idx != keep_idx) {
        u.idx = keep_idx;
        u.offset = st.start;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(u.changes ? TR("Close") : TR("Cancel"), bw) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(TR("Spelling Options###yc_spell_opts")))) open = false;
    ImGui::EndGroup();
    // Spelling Options: Excel's Proofing page (When correcting spelling in Microsoft Office programs)
    if (u.options_request) {
      u.options_request = false;
      ImGui::OpenPopup(TR("Spelling Options###yc_spell_opts"));
    }
    if (BeginModal(TR("Spelling Options###yc_spell_opts"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
      Settings& cf = Config();
      ImGui::TextUnformatted(TR("When correcting spelling in YCGrid:"));
      bool any = false;
      any |= ImGui::Checkbox(TR("Ignore words in UPPERCASE"), &cf.spell_ignore_upper);
      any |= ImGui::Checkbox(TR("Ignore words that contain numbers"), &cf.spell_ignore_numbers);
      any |= ImGui::Checkbox(TR("Ignore Internet and file addresses"), &cf.spell_ignore_internet);
      any |= ImGui::Checkbox(TR("Flag repeated words"), &cf.spell_repeated);
      if (any) SaveSettings();
      ImGui::SeparatorText(TR("AutoCorrect"));
      if (ImGui::Checkbox(TR("Replace text as you type"), &cf.autocorrect_replace)) SaveSettings();
      if (ImGui::Checkbox(TR("Internet and network paths with hyperlinks"), &cf.autoformat_hyperlinks)) SaveSettings();
      DrawAutoCorrectList();
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    // Dictionary language: another language checks the text again from this word
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Dictionary language:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(220));
    if (ui::BeginCombo("##lang", lang_name.c_str())) {
      for (const auto& [tag, name] : kLangs) {
        if (ImGui::Selectable(name, u.lang == tag)) {
          if (!CheckerFor(tag)) {
            s.ShowError("YCGrid", TR("The Windows spell checker for this language isn't available."));
          } else {
            u.lang = tag;
            u.offset = u.start;
            next = true;
          }
        }
      }
      ImGui::EndCombo();
    }
    if (next && !Advance(s)) Finish(s);
  }
  ImGui::End();
  if (!open) u.open = false;
}

namespace {
struct AcEntry {
  const char* what;
  const char* with;
};
const AcEntry kBuiltinAutoCorrect[] = {
#include "autocorrect_en.inc"
};
}  // namespace

// AutoCorrect's list: Excel's built-in replacements (less the ones deleted) and the user's own (they win)
std::vector<std::pair<std::string, std::string>> AutoCorrectList() {
  const Settings& cf = Config();
  std::vector<std::pair<std::string, std::string>> out;
  for (const AcEntry& e : kBuiltinAutoCorrect) {
    const std::string w = e.what;
    if (std::find(cf.autocorrect_removed.begin(), cf.autocorrect_removed.end(), w) != cf.autocorrect_removed.end()) continue;
    if (std::any_of(cf.autocorrect.begin(), cf.autocorrect.end(), [&](const auto& x) { return x.first == w; })) continue;
    out.emplace_back(w, e.with);
  }
  out.insert(out.end(), cf.autocorrect.begin(), cf.autocorrect.end());
  std::sort(out.begin(), out.end());
  return out;
}

void AutoCorrectAdd(const std::string& what, const std::string& with) {
  auto& cf = Config();
  cf.autocorrect.erase(std::remove_if(cf.autocorrect.begin(), cf.autocorrect.end(), [&](const auto& x) { return x.first == what; }), cf.autocorrect.end());
  cf.autocorrect.emplace_back(what, with);
  cf.autocorrect_removed.erase(std::remove(cf.autocorrect_removed.begin(), cf.autocorrect_removed.end(), what), cf.autocorrect_removed.end());
  SaveSettings();
}

void AutoCorrectDelete(const std::string& what) {
  auto& cf = Config();
  cf.autocorrect.erase(std::remove_if(cf.autocorrect.begin(), cf.autocorrect.end(), [&](const auto& x) { return x.first == what; }), cf.autocorrect.end());
  for (const AcEntry& e : kBuiltinAutoCorrect) {
    if (what == e.what && std::find(cf.autocorrect_removed.begin(), cf.autocorrect_removed.end(), what) == cf.autocorrect_removed.end())
      cf.autocorrect_removed.push_back(what);
  }
  SaveSettings();
}

std::string ApplyAutoCorrect(const std::string& text) {
  if (!Config().autocorrect_replace || text.empty() || text[0] == '=' || text[0] == '\'') return text;
  // the list by what is typed (built once per change of the settings)
  static std::map<std::string, std::string> table;
  static size_t stamp = static_cast<size_t>(-1);
  const Settings& cf = Config();
  const size_t now = cf.autocorrect.size() * 7919 + cf.autocorrect_removed.size();
  static std::vector<std::pair<std::string, std::string>> last_user;
  if (now != stamp || last_user != cf.autocorrect) {
    table.clear();
    for (const auto& [a, b] : AutoCorrectList()) table[a] = b;
    stamp = now;
    last_user = cf.autocorrect;
  }
  if (table.empty()) return text;
  auto word_char = [](unsigned char ch) { return std::isalnum(ch) || ch >= 0x80 || ch == '\''; };
  // a word: the entry as typed, or with a capital first letter (Excel keeps the capital)
  auto word = [&](const std::string& w) {
    if (auto it = table.find(w); it != table.end()) return it->second;
    if (!w.empty() && std::isupper(static_cast<unsigned char>(w[0]))) {
      std::string low = w;
      low[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(low[0])));
      if (auto it = table.find(low); it != table.end() && !it->second.empty()) {
        std::string rep = it->second;
        rep[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rep[0])));
        return rep;
      }
    }
    return w;
  };
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      out += text[i++];
      continue;
    }
    // a run between spaces: a symbol entry as a whole ("(c)", "-->", "..."), else its words one by one
    size_t j = i;
    while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
    const std::string run = text.substr(i, j - i);
    if (auto it = table.find(run); it != table.end() && !std::all_of(run.begin(), run.end(), [&](char ch) { return word_char(static_cast<unsigned char>(ch)); })) {
      out += it->second;
    } else {
      size_t k = 0;
      while (k < run.size()) {
        if (!word_char(static_cast<unsigned char>(run[k]))) {
          // a symbol at the end of a word ("Inc(tm)"), as Excel replaces it
          bool done = false;
          for (size_t len = std::min<size_t>(5, run.size() - k); len >= 2 && !done; --len) {
            if (auto hit = table.find(run.substr(k, len)); hit != table.end() && k + len == run.size()) {
              out += hit->second;
              k += len;
              done = true;
            }
          }
          if (!done) out += run[k++];
          continue;
        }
        size_t e = k;
        while (e < run.size() && word_char(static_cast<unsigned char>(run[e]))) ++e;
        out += word(run.substr(k, e - k));
        k = e;
      }
    }
    i = j;
  }
  return out;
}

void RegisterSpellingCommands(AppState& s) {
  ui::Command c;
  c.id = "Spelling";
  c.label = TR("Spelling");
  c.icon = ui::icon::Spelling;
  c.shortcut = "F7";
  c.tip_desc = TR("Check the spelling of the text in the selection (one cell checks the whole sheet).");
  ui::Commands().Add(c).execute = [&s](ui::Command&) { Start(s); };
}

}  // namespace yc::app
