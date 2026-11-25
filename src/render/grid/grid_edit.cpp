// SPDX-License-Identifier: Apache-2.0
// grid_edit.cpp - the edit field shared by cells and the formula bar (PLAN §9.2)
//
// Excel's key semantics differ from ImGui InputText: Enter commits and moves down, Alt+Enter inserts a line break, Ctrl+Enter fills the selection,
// Tab commits and moves right, Esc cancels, arrow keys commit and move in Enter mode. These keys are taken before InputText sees them
// (SetKeyOwner + LockThisFrame), so InputText never gets them.
#include "grid/grid.h"
#include "core/i18n.h"

#include "fonts/fonts.h"
#include "formula/func_help.h"
#include "formula/functions.h"
#include "formula/parser.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace yc::render {

namespace {

struct CbState {
  EditState* e;
  bool insert_newline;
};

// AutoComplete's entry for the text typed: the one entry of the column it begins (case ignored, an entry as long as
// the text counting too: apple with Apple and Apple pie completes nothing), as in Excel 16 (measured)
const std::string* FindCompletion(const std::vector<std::string>& words, std::string_view typed) {
  const std::string t = formula::FoldCase(typed);
  const std::string* match = nullptr;
  for (const std::string& w : words) {
    if (w.size() < typed.size() || formula::FoldCase(std::string_view(w).substr(0, typed.size())) != t) continue;
    if (match && formula::FoldCase(*match) != formula::FoldCase(w)) return nullptr;
    match = &w;
  }
  return match;
}

// AutoComplete: typed at the end, the start of exactly one text entry of the column finishes it, the rest added and
// selected (typing on replaces it, Enter takes the entry as written in the column, Backspace drops it), as in Excel;
// not for formulas or numbers. The letters typed keep their case meanwhile (f, then Delete, is f).
void AutoCompleteEntry(ImGuiInputTextCallbackData* d, EditState& e) {
  const bool grew = d->BufTextLen > e.ac_prev_len;
  e.ac_prev_len = d->BufTextLen;
  e.ac_full.clear();  // an edit ends the suggestion shown; it comes back below while the text still leads to one
  const std::string typed(d->Buf, static_cast<size_t>(d->BufTextLen));
  if (!grew || !e.autocomplete || e.in_formula_bar || d->CursorPos != d->BufTextLen || typed.empty() || typed[0] == '=' || typed[0] == '+' ||
      typed[0] == '-' || !std::any_of(typed.begin(), typed.end(), [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) || (ch & 0x80); })) {
    return;
  }
  const std::string* match = FindCompletion(e.column_words, typed);
  if (!match || match->size() == typed.size()) return;  // none, several, or typed in full (Excel keeps the typing)
  const std::string tail = match->substr(typed.size());
  d->InsertChars(d->BufTextLen, tail.data(), tail.data() + tail.size());
  d->SelectionStart = static_cast<int>(typed.size());
  d->SelectionEnd = d->CursorPos = d->BufTextLen;
  e.ac_prev_len = static_cast<int>(typed.size());  // typed on, a matching letter replaces the selection
  e.ac_full = *match;
}

// Excel's words in the cell editor (Ctrl+arrows, Ctrl+Backspace): runs of letters and digits, or runs of other
// characters that aren't spaces (`)*` is one run, `_ . - / % " & : , + (` stand alone between letters); spaces are passed
// over (Excel 16, measured on SUM(B2:B4)*2+C2,D3 x and ab cd.ef-gh_ij/kl 1.5% "q" a&b)
int WordClass(unsigned char ch) {
  if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') return 0;
  return std::isalnum(ch) || ch >= 0x80 ? 1 : 2;
}
int WordLeft(std::string_view t, int p) {
  p = std::clamp(p, 0, static_cast<int>(t.size()));
  while (p > 0 && WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p) - 1])) == 0) --p;
  if (p == 0) return 0;
  const int k = WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p) - 1]));
  while (p > 0 && WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p) - 1])) == k) --p;
  return p;
}
int WordRight(std::string_view t, int p) {
  const int n = static_cast<int>(t.size());
  p = std::clamp(p, 0, n);
  if (p < n && WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p)])) != 0) {
    const int k = WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p)]));
    while (p < n && WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p)])) == k) ++p;
  }
  while (p < n && WordClass(static_cast<unsigned char>(t[static_cast<size_t>(p)])) == 0) ++p;
  return p;
}

// the undo bookkeeping of every callback: a change typed in the box goes on the run its first change began; one made
// from outside it (a reference pointed at, F4, F9) is a step of its own; a caret move ends the run
void TrackUndo(ImGuiInputTextCallbackData* d, EditState& e, bool typed) {
  const std::string_view now(d->Buf, static_cast<size_t>(d->BufTextLen));
  if (now != e.undo_last) {
    if (!typed || !e.undo_run) {
      e.undo.push_back(EditState::UndoStep{e.undo_last, e.undo_last_caret});
      if (e.undo.size() > 100) e.undo.erase(e.undo.begin());
      e.redo.clear();
    }
    e.undo_run = typed;
    e.undo_last.assign(now);
  } else if (d->CursorPos != e.undo_last_caret) {
    e.undo_run = false;
  }
  e.undo_last_caret = d->CursorPos;
}

int EditCallback(ImGuiInputTextCallbackData* d) {
  auto* st = static_cast<CbState*>(d->UserData);
  EditState& e = *st->e;
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    e.text.resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = e.text.data();
    return 0;
  }
  if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
    if (e.pending_replace) {
      e.pending_replace = false;
      const std::string next = e.pending_text;
      d->DeleteChars(0, d->BufTextLen);
      d->InsertChars(0, next.data(), next.data() + next.size());
    }
    // the caret first, then Alt+Enter's line break at it: typed fast, the text came before the box had focus and the
    // break went to the start of the text
    if (e.cursor_to_end) {
      e.cursor_to_end = false;
      d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
    }
    if (e.pending_caret >= 0) {
      d->CursorPos = d->SelectionStart = d->SelectionEnd = std::min(e.pending_caret, d->BufTextLen);
      // a selection to open with (AutoSum's range, what F4 changed); it may run backwards (Shift+Up)
      if (e.pending_sel_from >= 0) d->SelectionStart = std::min(e.pending_sel_from, d->BufTextLen);
      e.pending_caret = e.pending_sel_from = -1;
    }
    if (st->insert_newline) {
      // over a selection (AutoComplete's rest) the line break takes its place, as Excel's (a, Alt+Enter, b is a and b)
      if (d->SelectionStart != d->SelectionEnd) {
        const int a = std::min(d->SelectionStart, d->SelectionEnd), b = std::max(d->SelectionStart, d->SelectionEnd);
        d->DeleteChars(a, b - a);
        d->CursorPos = d->SelectionStart = d->SelectionEnd = a;
      }
      d->InsertChars(d->CursorPos, "\n");
      st->insert_newline = false;
      e.ac_full.clear();
    }
    // Edit mode: Up on the first line goes to its start and Down on the last line to its end (Shift: selects to there),
    // as in Excel (F2, Up, then 1 typed before 10 is 110); ImGui's box stays put there
    const ImGuiIO& io = ImGui::GetIO();
    if ((!e.enter_mode || e.in_formula_bar) && !io.KeyCtrl && !io.KeyAlt && d->CursorPos == e.caret) {
      const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow), down = !up && ImGui::IsKeyPressed(ImGuiKey_DownArrow);
      if (up || down) {
        const int anchor = d->SelectionStart != d->SelectionEnd ? d->SelectionStart : d->CursorPos;
        d->CursorPos = d->SelectionEnd = up ? 0 : d->BufTextLen;
        d->SelectionStart = io.KeyShift ? anchor : d->CursorPos;
      }
    }
    // the user moved the caret (typing, clicking): stop replacing the reference just inserted
    if (d->CursorPos != e.caret && e.ref_begin >= 0 && !e.ref_dragging && d->CursorPos != e.ref_end) {
      e.ref_begin = e.ref_end = -1;
    }
    e.caret = d->CursorPos;
    e.sel_start = d->SelectionStart;
    e.sel_end = d->SelectionEnd;
    if (e.ac_first && !e.pending_replace) {
      e.ac_first = false;  // the character that began the entry: no edit event comes for it
      if (d->CursorPos == d->BufTextLen) AutoCompleteEntry(d, e);
    }
    TrackUndo(d, e, false);
  }
  if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
    // after typing, references are no longer replaced and keyboard pointing starts over
    e.ref_begin = e.ref_end = -1;
    // a % typed just before the one automatic percent entry put after the caret takes its place (5, then %, is 5%)
    if (e.auto_pct && d->CursorPos > 0 && d->CursorPos < d->BufTextLen && d->Buf[d->CursorPos - 1] == '%' && d->Buf[d->CursorPos] == '%' &&
        d->CursorPos + 1 == d->BufTextLen) {
      d->DeleteChars(d->CursorPos, 1);
      e.auto_pct = false;
    }
    // and the caret follows the character just typed: InputText calls the Always callback before it inserts it, so
    // an arrow the next frame read the caret before the "," and committed =SUM(F2,) instead of pointing
    e.caret = d->CursorPos;
    e.sel_start = d->SelectionStart;
    e.sel_end = d->SelectionEnd;
    e.kpoint = false;
    e.ac_first = false;
    AutoCompleteEntry(d, e);
    TrackUndo(d, e, true);
  }
  return 0;
}


// ---- Formula autocomplete and argument hints ----

std::vector<Completion>& ExtraCompletions() {
  static std::vector<Completion> v;
  return v;
}

bool IsIdentChar(unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '.' || ch >= 0x80; }

std::string UpperAscii(std::string_view s) {
  std::string out(s);
  for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return out;
}

struct AssistItem {
  std::string name;
  std::string detail;
  int kind;  // 0 function, 1 name, 2 UDF, 3 table, 4 a table's column or special item (#Data)
};

struct Assist {
  int tok_begin = -1;  // prefix being typed (before the caret)
  std::string token;
  std::vector<AssistItem> items;
  std::string call;  // function call the caret is in (uppercase)
  int arg = 0;       // argument index
};

// scan the formula text before the caret: the prefix, the enclosing function call and the argument index (skipping strings and quoted sheet names)
Assist ComputeAssist(const EditState& e) {
  Assist a;
  const std::string& t = e.text;
  if (!formula::LooksLikeFormula(t)) return a;  // "=", or "+" / "-" followed by a name (keypad users start so)
  const int caret = std::clamp(e.caret, 0, static_cast<int>(t.size()));
  // the caret before the "=": the first characters of an edit started by a double-click arrive before the box has
  // focus and moves its caret; nothing to suggest yet (the scan below reads the character before the caret)
  if (caret < 1) return a;
  struct Frame {
    std::string name;
    int arg = 0;
  };
  std::vector<Frame> stack;
  bool in_str = false, in_quote = false;
  for (int i = 1; i < caret; ++i) {
    const char ch = t[static_cast<size_t>(i)];
    if (in_str) {
      if (ch == '"') {
        if (i + 1 < caret && t[static_cast<size_t>(i) + 1] == '"') ++i;
        else in_str = false;
      }
      continue;
    }
    if (in_quote) {
      if (ch == '\'') in_quote = false;
      continue;
    }
    if (ch == '"') in_str = true;
    else if (ch == '\'') in_quote = true;
    else if (ch == '(') {
      int j = i - 1;
      while (j >= 1 && IsIdentChar(static_cast<unsigned char>(t[static_cast<size_t>(j)]))) --j;
      stack.push_back({UpperAscii(std::string_view(t).substr(static_cast<size_t>(j + 1), static_cast<size_t>(i - j - 1))), 0});
    } else if (ch == ')') {
      if (!stack.empty()) stack.pop_back();
    } else if (ch == ',' && !stack.empty()) {
      ++stack.back().arg;
    }
  }
  if (in_str || in_quote) return a;
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    if (!it->name.empty()) {
      a.call = it->name;
      a.arg = it->arg;
      break;
    }
  }
  // after "Table1[": the table's columns and its special items, as Excel lists them for structured references
  {
    int j = caret;
    while (j > 1 && std::strchr("[]()\",\"=+-*/^&<>", t[static_cast<size_t>(j) - 1]) == nullptr) --j;
    if (j > 1 && t[static_cast<size_t>(j) - 1] == '[') {
      int nb = j - 1;
      while (nb > 1 && IsIdentChar(static_cast<unsigned char>(t[static_cast<size_t>(nb) - 1]))) --nb;
      const std::string tname = UpperAscii(std::string_view(t).substr(static_cast<size_t>(nb), static_cast<size_t>(j - 1 - nb)));
      for (const auto& c : ExtraCompletions()) {
        if (c.kind != 3 || UpperAscii(c.name) != tname) continue;
        a.tok_begin = j;
        a.token = t.substr(static_cast<size_t>(j), static_cast<size_t>(caret - j));
        const std::string up = UpperAscii(a.token);
        // Escape closed the list: not again until the text changes (else a second Escape never reaches the edit)
        if (!e.formula_list || (a.token.empty() ? std::string("\x01") : a.token) == e.ac_hidden_token) return a;
        for (const std::string& col : c.columns) {
          if (UpperAscii(col).compare(0, up.size(), up) == 0) a.items.push_back({col, c.name, 4});
        }
        for (const char* sp : {"#All", "#Data", "#Headers", "#Totals", "@"}) {
          if (UpperAscii(sp).compare(0, up.size(), up) == 0) a.items.push_back({sp, c.name, 4});
        }
        return a;
      }
    }
  }
  // prefix
  int i = caret;
  while (i > 1 && IsIdentChar(static_cast<unsigned char>(t[static_cast<size_t>(i) - 1]))) --i;
  if (i == caret) {
    // nothing typed yet: no list (Excel lists names only after their first letter), so a click on the cells under
    // the edit box after "=" points at them; inside a call the argument hint stays
    return a;
  } else {
    const unsigned char first = static_cast<unsigned char>(t[static_cast<size_t>(i)]);
    if (std::isdigit(first) || first == '.') return a;
    const char prev = t[static_cast<size_t>(i) - 1];
    if (prev == '!' || prev == '$' || prev == ':' || prev == '\'') return a;
  }
  if (!e.formula_list) return a;  // Formula AutoComplete turned off (File > Options > Formulas)
  a.tok_begin = i;
  a.token = t.substr(static_cast<size_t>(i), static_cast<size_t>(caret - i));
  if ((a.token.empty() ? std::string("\x01") : a.token) == e.ac_hidden_token) return a;
  const std::string up = UpperAscii(a.token);
  for (const auto& h : formula::AllFuncHelp()) {
    if (std::strncmp(h.name, up.c_str(), up.size()) == 0) a.items.push_back({h.name, h.desc, 0});
  }
  for (const auto& c : ExtraCompletions()) {
    if (UpperAscii(c.name).compare(0, up.size(), up) == 0) a.items.push_back({c.name, c.detail, c.kind});
  }
  std::stable_sort(a.items.begin(), a.items.end(), [](const AssistItem& x, const AssistItem& y) { return UpperAscii(x.name) < UpperAscii(y.name); });
  // the prefix is already a complete name followed by "(": no more suggestions
  if (a.items.size() == 1 && UpperAscii(a.items[0].name) == up && caret < static_cast<int>(t.size()) && t[static_cast<size_t>(caret)] == '(') a.items.clear();
  if (!a.token.empty() && a.items.size() > 200) a.items.resize(200);
  return a;
}

void AcceptCompletion(EditState& e, const Assist& a, const AssistItem& it) {
  std::string next = e.text;
  const int caret = std::clamp(e.caret, 0, static_cast<int>(next.size()));
  std::string ins = it.name;
  const bool fn = it.kind == 0 || it.kind == 2;
  const bool has_paren = caret < static_cast<int>(next.size()) && next[static_cast<size_t>(caret)] == '(';
  if (fn && !has_paren) ins += "(";
  next.replace(static_cast<size_t>(a.tok_begin), static_cast<size_t>(caret - a.tok_begin), ins);
  SetEditText(e, std::move(next), a.tok_begin + static_cast<int>(ins.size()) + (fn && has_paren ? 1 : 0));
  e.focus_request = true;
  e.ac_sel = 0;
}

// argument hint: SUM(number1, [number2], ...), the current argument in bold
void DrawArgHint(const Assist& a) {
  const formula::FuncHelp* h = formula::FindFuncHelp(a.call);
  std::vector<std::string> parts;
  std::string args = h ? h->args : "";
  for (size_t p = 0; p < args.size();) {
    size_t q = args.find(", ", p);
    parts.push_back(args.substr(p, q == std::string::npos ? std::string::npos : q - p));
    if (q == std::string::npos) break;
    p = q + 2;
  }
  int cur = parts.empty() ? -1 : std::min<int>(a.arg, static_cast<int>(parts.size()) - 1);
  if (cur >= 0 && parts[static_cast<size_t>(cur)] == "..." && cur > 0) cur = -1;  // beyond the listed arguments: nothing is highlighted
  ImGui::TextUnformatted((a.call + "(").c_str());
  for (size_t k = 0; k < parts.size(); ++k) {
    ImGui::SameLine(0, 0);
    if (static_cast<int>(k) == cur) {
      ImGui::PushFont(ui::F().ui_bold, 0.0f);
      ImGui::TextUnformatted(parts[k].c_str());
      ImGui::PopFont();
    } else {
      ImGui::TextUnformatted(parts[k].c_str());
    }
    if (k + 1 < parts.size()) {
      ImGui::SameLine(0, 0);
      ImGui::TextUnformatted(", ");
    }
  }
  ImGui::SameLine(0, 0);
  ImGui::TextUnformatted(")");
}

void DrawAssist(EditState& e, const Assist& a, const ImRect& field) {
  const bool hint = e.formula_tips && !a.call.empty() && (formula::FindFuncHelp(a.call) != nullptr);
  const bool list = !a.items.empty();
  if (!hint && !list) return;
  // doesn't fit below: put it above the edit box (so it doesn't become a separate OS window)
  const ImGuiViewport* vp = ImGui::GetWindowViewport();
  const float est_h = list ? ImGui::GetFontSize() * 1.45f * static_cast<float>(std::min<size_t>(10, a.items.size()) + 4) : ImGui::GetFontSize() * 2.0f;
  const bool above = field.Max.y + 2 + est_h > vp->Pos.y + vp->Size.y && field.Min.y - est_h > vp->Pos.y;
  ImGui::SetNextWindowPos(above ? ImVec2(field.Min.x, field.Min.y - 2) : ImVec2(field.Min.x, field.Max.y + 2), ImGuiCond_Always, above ? ImVec2(0, 1) : ImVec2(0, 0));
  ImGui::PushFont(ui::F().ui, ui::F().ui_size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                 // the argument hint alone lets clicks through: the cell under it can still be pointed at
                                 (list ? 0 : ImGuiWindowFlags_NoInputs);
  if (ImGui::Begin("##yc_formula_assist", nullptr, flags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    if (hint && !list) DrawArgHint(a);
    if (list) {
      e.ac_sel = std::clamp(e.ac_sel, 0, static_cast<int>(a.items.size()) - 1);
      const float row = ImGui::GetTextLineHeightWithSpacing();
      const int shown = std::min<int>(10, static_cast<int>(a.items.size()));
      ImGui::BeginChild("##ac_list", ImVec2(ImGui::GetFontSize() * 14, row * static_cast<float>(shown) + 4), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
      for (size_t k = 0; k < a.items.size(); ++k) {
        const auto& it = a.items[k];
        ImGui::PushID(static_cast<int>(k));
        const bool sel = static_cast<int>(k) == e.ac_sel;
        ImGui::TextDisabled(it.kind == 0 ? "fx" : it.kind == 1 ? TR("nm") : it.kind == 3 ? TR("tb") : it.kind == 4 ? "[]" : "py");
        ImGui::SameLine(ImGui::GetFontSize() * 1.8f);
        if (ImGui::Selectable(it.name.c_str(), sel, ImGuiSelectableFlags_AllowDoubleClick)) {
          e.ac_sel = static_cast<int>(k);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) AcceptCompletion(e, a, it);
          else e.focus_request = true;
        }
        if (sel && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        if (sel && e.ac_scroll) {
          ImGui::SetScrollHereY();
        }
        ImGui::PopID();
      }
      e.ac_scroll = false;
      ImGui::EndChild();
      // description of the selected item
      const auto& cur = a.items[static_cast<size_t>(e.ac_sel)];
      const formula::FuncHelp* h = cur.kind == 0 ? formula::FindFuncHelp(cur.name) : nullptr;
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
      if (h) {
        ImGui::TextDisabled("%s(%s)", h->name, h->args);
        ImGui::TextUnformatted(h->desc);
      } else if (!cur.detail.empty()) {
        ImGui::TextDisabled("%s", cur.kind == 1 ? TR("Define Name") : cur.kind == 3 ? TR("Table") : cur.kind == 4 ? TR("Table column") : TR("Python function"));
        ImGui::TextUnformatted(cur.detail.c_str());
      }
      ImGui::TextDisabled("%s", a.token.empty() ? TR("Type to filter, Tab inserts, Esc closes") : TR("Tab inserts, ↑↓ selects, Esc closes"));
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopFont();
}

// Excel colors each reference of a formula being typed in the color of its box on the sheet. The field draws its text
// transparent; the text is drawn here again, laid out line by line as InputText does (line breaks and word wrap)
void DrawFormulaText(const EditState& e, ImGuiWindow* child, ImVec2 pos, float wrap_width, ImU32 base) {
  ImFont* font = ImGui::GetFont();
  const float size = ImGui::GetFontSize();
  const char* text = e.text.c_str();
  const char* end = text + std::strlen(text);
  const bool spans_ok = e.ref_spans_text == std::string_view(text, static_cast<size_t>(end - text));
  const ImRect& cr = child->InnerClipRect;
  const ImVec4 clip(cr.Min.x, cr.Min.y, cr.Max.x, cr.Max.y);
  const char* p = text;
  for (int guard = 0; guard < 100000; ++guard) {
    const char* eol = p;
    if (wrap_width > 0.0f) {
      eol = ImFontCalcWordWrapPositionEx(font, size, p, end, wrap_width, ImDrawTextFlags_WrapKeepBlanks);
      if (eol == p && p < end && *p != '\n') {
        unsigned int ch = 0;
        eol = p + ImTextCharFromUtf8(&ch, p, end);
      }
    } else {
      while (eol < end && *eol != '\n') ++eol;
    }
    if (pos.y + size >= cr.Min.y && pos.y <= cr.Max.y) {
      float x = pos.x;
      for (const char* q = p; q < eol;) {
        const int off = static_cast<int>(q - text);
        ImU32 col = base;
        const char* seg_end = eol;
        if (spans_ok) {
          for (const FormulaRefSpan& sp : e.ref_spans) {
            if (sp.end <= off) continue;
            if (sp.begin <= off) col = sp.color, seg_end = std::min(eol, text + sp.end);
            else seg_end = std::min(eol, text + sp.begin);
            break;
          }
        }
        font->RenderText(child->DrawList, size, ImVec2(x, pos.y), col, clip, q, seg_end);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, q, seg_end).x;
        q = seg_end;
      }
    }
    if (eol >= end) break;
    p = *eol == '\n' ? eol + 1 : eol;
    pos.y += size;
  }
}

// F4 on an R1C1 reference (Excel): R[-1]C[-1] -> R1C1 -> R[-1]C1 -> R1C[-1] -> R[-1]C[-1], relative to the edited
// cell (row, col); the reference at or just before the caret. Returns false when there is none.
bool CycleR1C1Ref(std::string& t, int caret, uint32_t row, uint32_t col, int* out_b, int* out_e) {
  struct Part {
    bool abs = false;
    int64_t v = 0;  // absolute: 1-based number; relative: offset
  };
  auto parse_part = [&](size_t i, char k, Part* p, size_t* end) {
    if (i >= t.size() || std::toupper(static_cast<unsigned char>(t[i])) != k) return false;
    size_t j = i + 1;
    p->abs = false;
    p->v = 0;
    if (j < t.size() && t[j] == '[') {
      size_t q = t.find(']', j);
      if (q == std::string::npos) return false;
      p->v = std::atoll(t.substr(j + 1, q - j - 1).c_str());
      j = q + 1;
    } else if (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) {
      p->abs = true;
      while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) p->v = p->v * 10 + (t[j++] - '0');
    }
    *end = j;
    return true;
  };
  bool in_str = false;
  int best_b = -1, best_e = -1;
  Part br, bc;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] == '"') in_str = !in_str;
    if (in_str || (i > 0 && IsIdentChar(static_cast<unsigned char>(t[i - 1])))) continue;
    Part r, c;
    size_t e1 = 0, e2 = 0;
    if (!parse_part(i, 'R', &r, &e1) || !parse_part(e1, 'C', &c, &e2)) continue;
    if (e2 < t.size() && (IsIdentChar(static_cast<unsigned char>(t[e2])) || t[e2] == '(')) continue;
    if (static_cast<int>(i) <= caret) best_b = static_cast<int>(i), best_e = static_cast<int>(e2), br = r, bc = c;
  }
  if (best_b < 0) return false;
  const int64_t ar = br.abs ? br.v : static_cast<int64_t>(row) + 1 + br.v, ac = bc.abs ? bc.v : static_cast<int64_t>(col) + 1 + bc.v;
  bool nr, nc;  // the next state: absolute row / column
  if (!br.abs && !bc.abs) nr = true, nc = true;
  else if (br.abs && bc.abs) nr = false, nc = true;
  else if (!br.abs && bc.abs) nr = true, nc = false;
  else nr = false, nc = false;
  auto part = [](char k, bool abs, int64_t a, int64_t base) {
    if (abs) return std::string(1, k) + std::to_string(a);
    const int64_t off = a - base;
    return off == 0 ? std::string(1, k) : std::string(1, k) + "[" + std::to_string(off) + "]";
  };
  const std::string rep = part('R', nr, ar, static_cast<int64_t>(row) + 1) + part('C', nc, ac, static_cast<int64_t>(col) + 1);
  t.replace(static_cast<size_t>(best_b), static_cast<size_t>(best_e - best_b), rep);
  *out_b = best_b;
  *out_e = best_b + static_cast<int>(rep.size());
  return true;
}

}  // namespace

void SetFormulaCompletions(std::vector<Completion> items) { ExtraCompletions() = std::move(items); }

bool CanInsertRef(const EditState& e) {
  // "=", or "+" / "-": keypad users start formulas so (Excel points after them too)
  if (e.text.empty() || !(e.text[0] == '=' || e.text[0] == '+' || e.text[0] == '-')) return false;
  if (e.ref_begin >= 0) return true;  // dragging / replacing the reference just inserted
  int i = std::min<int>(e.caret, static_cast<int>(e.text.size())) - 1;
  while (i >= 0 && e.text[static_cast<size_t>(i)] == ' ') --i;
  if (i < 0) return false;
  const char ch = e.text[static_cast<size_t>(i)];
  return std::strchr("=+-*/^&(,;:<>", ch) != nullptr;
}

bool EditField(EditState& e, const char* id, const ImVec2& size, GridActions& act, bool in_formula_bar, bool word_wrap) {
  ImGuiIO& io = ImGui::GetIO();
  const ImGuiID owner = ImGui::GetID("##yc_edit_keys");
  static CbState cb;
  cb.e = &e;
  // a dialog opened while editing (F3 Paste Name, Insert Function) has the keyboard: Enter is its OK, not the end of the edit
  const bool mine = e.active && e.in_formula_bar == in_formula_bar && ImGui::GetTopMostPopupModal() == nullptr;
  const ImGuiID fid = ImGui::GetID(id);
  const bool field_active = ImGui::GetActiveID() != 0 && ImGui::GetActiveID() == fid;
  // the box takes the keyboard on the frame the editor opens or wants it back (see before InputText below); when ImGui's
  // text state is this box's from before, it reloads with the caret where the editor wants it and reads this frame's keys
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const bool activate = mine && !field_active && (e.focus_request || e.focus_waiting);
  const bool reload = activate && g.InputTextState.ID == fid;
  if (mine && !field_active) {
    // the edit box only gets focus on the next frame: handle characters and Backspace typed meanwhile so nothing is lost (fast typing or low frame rates)
    // the box opens with its caret set (AutoSum's range selected, a percent cell's % after the caret): what is typed
    // meanwhile goes there, replacing the selection; otherwise it goes to the end
    auto drop_selection = [&] {
      const int at = std::clamp(e.pending_caret, 0, static_cast<int>(e.text.size()));
      if (e.pending_sel_from < 0 || e.pending_sel_from >= at) return false;
      e.text.erase(static_cast<size_t>(e.pending_sel_from), static_cast<size_t>(at - e.pending_sel_from));
      e.pending_caret = e.pending_sel_from;
      e.pending_sel_from = -1;
      return true;
    };
    for (ImWchar ch : io.InputQueueCharacters) {
      if (ch < 32 || ch == 127) continue;
      char buf[8];
      const int n = ImTextCharToUtf8(buf, static_cast<unsigned int>(ch));
      if (e.pending_caret >= 0) {
        drop_selection();
        const int at = std::clamp(e.pending_caret, 0, static_cast<int>(e.text.size()));
        if (ch == '%' && e.auto_pct && at + 1 == static_cast<int>(e.text.size()) && e.text[static_cast<size_t>(at)] == '%') {
          e.auto_pct = false;  // over the automatic %, as when the box has the keyboard
          e.pending_caret = at + 1;
        } else {
          e.text.insert(static_cast<size_t>(at), buf, static_cast<size_t>(n));
          e.pending_caret = at + n;
        }
      } else {
        e.text.append(buf, static_cast<size_t>(n));
        e.cursor_to_end = true;
      }
    }
    io.InputQueueCharacters.resize(0);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !io.KeyCtrl && !e.text.empty() && !(e.pending_caret >= 0 && drop_selection())) {
      // delete the UTF-8 character before the caret (the last one, when the box puts the caret at the end)
      const size_t at = e.pending_caret >= 0 ? static_cast<size_t>(std::clamp(e.pending_caret, 0, static_cast<int>(e.text.size()))) : e.text.size();
      if (at > 0) {
        size_t k = at - 1;
        while (k > 0 && (static_cast<unsigned char>(e.text[k]) & 0xC0) == 0x80) --k;
        e.text.erase(k, at - k);
        if (e.pending_caret >= 0) e.pending_caret = static_cast<int>(k);
      }
    }
    // text typed before the box has focus goes to the end, and so does the caret (arrow keys typed meanwhile point from there)
    if (e.cursor_to_end) {
      e.caret = e.sel_start = e.sel_end = static_cast<int>(e.text.size());
    } else if (e.pending_caret >= 0) {
      e.caret = e.sel_end = std::clamp(e.pending_caret, 0, static_cast<int>(e.text.size()));
      e.sel_start = e.pending_sel_from >= 0 ? std::min(e.pending_sel_from, e.caret) : e.caret;
    }
  }
  const Assist assist = mine ? ComputeAssist(e) : Assist{};
  // keys used here are hidden from InputText this frame: it claims the arrows, Home / End and Delete for itself every frame
  // (SetKeyOwner without a lock), so the owner lock alone doesn't stop it moving the caret (a pointed reference then lands
  // one character too far left). They are put back after it, so key repeat and later code see the usual state.
  ImGuiKey hidden[32];
  int n_hidden = 0;
  if (mine) {
    auto take = [&](ImGuiKey k) {
      const bool p = ImGui::IsKeyPressed(k, true);
      ImGui::SetKeyOwner(k, owner, ImGuiInputFlags_LockThisFrame);
      if (p && n_hidden < 32) {
        ImGui::GetKeyData(k)->Down = false;
        hidden[n_hidden++] = k;
      }
      return p;
    };
    // the autocomplete list is open: Up, Down, Tab and Esc go to the list (later handling ignores these keys)
    const bool list_open = !assist.items.empty() && field_active;
    bool list_tab = false, list_esc = false;
    if (list_open) {
      const int n = static_cast<int>(assist.items.size());
      // with nothing typed yet (the full list after "="), the arrow keys keep pointing at cells like Excel
      if (!assist.token.empty()) {
        if (take(ImGuiKey_DownArrow)) e.ac_sel = (e.ac_sel + 1) % n, e.ac_scroll = true;
        if (take(ImGuiKey_UpArrow)) e.ac_sel = (e.ac_sel + n - 1) % n, e.ac_scroll = true;
      }
      if ((list_tab = take(ImGuiKey_Tab))) AcceptCompletion(e, assist, assist.items[static_cast<size_t>(std::clamp(e.ac_sel, 0, n - 1))]);
      if ((list_esc = take(ImGuiKey_Escape))) e.ac_hidden_token = assist.token.empty() ? std::string("\x01") : assist.token;
    } else if (assist.tok_begin < 0) {
      e.ac_sel = 0;
    }
    const bool esc = !list_esc && take(ImGuiKey_Escape);
    const bool enter = take(ImGuiKey_Enter) | take(ImGuiKey_KeypadEnter);
    const bool tab = !list_tab && take(ImGuiKey_Tab);
    if (esc) act.cancel_edit = true;
    if (enter) {
      if (io.KeyAlt) {
        cb.insert_newline = true;
      } else if (io.KeyCtrl && io.KeyShift) {
        act.commit_edit = act.commit_cse = true;
        act.commit_move = 0;
      } else if (io.KeyCtrl) {
        act.commit_edit = act.commit_array = true;
        act.commit_move = 0;
      } else {
        act.commit_edit = true;
        act.commit_move = io.KeyShift ? 2 : 1;
      }
    }
    if (tab) {
      act.commit_edit = true;
      act.commit_move = io.KeyShift ? 4 : 3;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) e.enter_mode = !e.enter_mode;
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && e.r1c1 && formula::LooksLikeFormula(e.text) && take(ImGuiKey_F4)) {
      // R1C1 style: the reference at the caret cycles between relative and absolute, as in Excel
      std::string next = e.text;
      int b = 0, en = 0;
      if (CycleR1C1Ref(next, e.caret, e.r, e.c, &b, &en)) {
        const bool pointed = e.ref_begin >= 0 && b == e.ref_begin;
        SetEditText(e, std::move(next), en);
        if (pointed) e.ref_end = en;
        else e.ref_begin = e.ref_end = -1;
      }
    }
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && take(ImGuiKey_F4) && !e.r1c1 && formula::LooksLikeFormula(e.text)) {
      // F4 cycles through $A$1, A$1, $A1 and A1 the cell reference at the caret (an end of a range on its own), every
      // one selected (they stay selected), or all of one just pointed at, as in Excel (16, measured)
      std::string next = e.text;
      const bool pointed = e.ref_begin >= 0 && e.ref_end > e.ref_begin;
      const bool has_sel = e.sel_start != e.sel_end;
      const int lo = pointed ? e.ref_begin : has_sel ? std::min(e.sel_start, e.sel_end) : e.caret;
      const int hi = pointed ? e.ref_end : has_sel ? std::max(e.sel_start, e.sel_end) : e.caret;
      int b = 0, en = 0;
      if (formula::CycleAbsoluteRef(next, lo, hi, &b, &en)) {
        SetEditText(e, std::move(next), en);
        if (pointed) {
          e.ref_begin = b;  // arrow keys or a click still replace the reference just pointed at
          e.ref_end = en;
        } else {
          e.ref_begin = e.ref_end = -1;
          if (has_sel) e.pending_sel_from = e.sel_start = b;
        }
      }
    }
    // the full list after "=" leaves the arrows to pointing (see above)
    if (e.enter_mode && !in_formula_bar && !(list_open && !assist.token.empty())) {
      // Enter mode: when the formula can take a reference, arrow keys pick cells (Excel's Point mode); otherwise commit and move
      const bool point = CanInsertRef(e) || (e.kpoint && e.ref_begin >= 0);
      // a key that ends the entry and then acts on the sheet as it does there (Excel 16, measured: abc, Ctrl+Left jumps
      // to C1; Shift+Right selects E1:F1; Ctrl+Home, then x, puts x in A1)
      auto nav = [&](ImGuiKey k) {
        act.commit_edit = true;
        act.commit_move = 0;
        act.nav_key = k;
        act.nav_ctrl = io.KeyCtrl;
        act.nav_shift = io.KeyShift;
      };
      const ImGuiKey arrows[] = {ImGuiKey_DownArrow, ImGuiKey_UpArrow, ImGuiKey_RightArrow, ImGuiKey_LeftArrow};
      for (int i = 0; i < 4; ++i) {
        if (take(arrows[i])) {
          if (point) {
            act.point_move = i + 1;
            act.point_extend = io.KeyShift;
            act.point_ctrl = io.KeyCtrl || e.kp_end;  // Ctrl+arrow (or End, then an arrow): to the edge of the data
            e.kp_end = false;
          } else if (io.KeyCtrl || io.KeyShift) {
            nav(arrows[i]);
          } else {
            act.commit_edit = true;
            act.commit_move = i + 5;  // an arrow: navigates (no Tab chain, the selection collapses), unlike Enter / Tab
          }
        }
      }
      if (!point) {
        for (const ImGuiKey k : {ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown}) {
          if (!io.KeyAlt && take(k)) nav(k);
        }
      }
      if (point) {
        // Point mode takes the other moving keys too: PgDn / PgUp, Home, Ctrl+Home, Ctrl+End, End (the next arrow
        // jumps), and Ctrl+Space / Shift+Space (whole columns / rows) once a reference is pointed at (before that,
        // Shift+Space types a space). Excel 16, measured: =B2+, Home points at A1; =SUM(, Left, Shift+Home is A1:D1
        if (!io.KeyCtrl && !io.KeyAlt && take(ImGuiKey_PageDown)) act.point_move = 5, act.point_extend = io.KeyShift;
        if (!io.KeyCtrl && !io.KeyAlt && take(ImGuiKey_PageUp)) act.point_move = 6, act.point_extend = io.KeyShift;
        if (io.KeyCtrl && !io.KeyAlt && take(ImGuiKey_Home)) act.point_move = 7, act.point_extend = io.KeyShift;
        if (!io.KeyCtrl && !io.KeyAlt && take(ImGuiKey_Home)) act.point_move = 10, act.point_extend = io.KeyShift;
        if (io.KeyCtrl && !io.KeyAlt && take(ImGuiKey_End)) act.point_move = 11, act.point_extend = io.KeyShift;
        if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && take(ImGuiKey_End)) e.kp_end = !e.kp_end;
        if (e.ref_begin >= 0 && !io.KeyAlt && (io.KeyCtrl != io.KeyShift) && take(ImGuiKey_Space)) {
          act.point_move = io.KeyCtrl ? 8 : 9;
          for (int k = io.InputQueueCharacters.Size - 1; k >= 0; --k) {
            if (io.InputQueueCharacters[k] == ' ') io.InputQueueCharacters.erase(io.InputQueueCharacters.Data + k);
          }
        }
      }
    }
    // Ctrl+A right after a function's name (=SUM or =SUM( ): Function Arguments; Ctrl+Shift+A types the argument names
    // (=SUM(number1,[number2],...)), as in Excel. Elsewhere Ctrl+A selects all of the text, as before.
    if (io.KeyCtrl && !io.KeyAlt && formula::LooksLikeFormula(e.text) && e.caret > 0 && e.caret <= static_cast<int>(e.text.size())) {
      int end = e.caret;
      const bool paren = e.text[static_cast<size_t>(end) - 1] == '(';
      if (paren) --end;
      int b = end;
      while (b > 1 && IsIdentChar(static_cast<unsigned char>(e.text[static_cast<size_t>(b) - 1]))) --b;
      const formula::FuncHelp* fh = b < end ? formula::FindFuncHelp(UpperAscii(std::string_view(e.text).substr(static_cast<size_t>(b), static_cast<size_t>(end - b)))) : nullptr;
      if (fh && take(ImGuiKey_A)) {
        if (!io.KeyShift) {
          act.fn_args = true;
        } else {
          std::string names = fh->args;
          names.erase(std::remove(names.begin(), names.end(), ' '), names.end());
          std::string next = e.text;
          const std::string ins = (paren ? "" : "(") + names + ")";
          next.insert(static_cast<size_t>(e.caret), ins);
          SetEditText(e, std::move(next), e.caret + static_cast<int>(ins.size()));
          e.focus_request = true;
        }
      }
    }
    // F9 on a formula: the selected part (or the whole formula) is replaced by its value, as in Excel
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && formula::LooksLikeFormula(e.text) && take(ImGuiKey_F9)) {
      const int n = static_cast<int>(e.text.size());
      const int b = std::clamp(std::min(e.sel_start, e.sel_end), 0, n), en = std::clamp(std::max(e.sel_start, e.sel_end), 0, n);
      act.eval_part = true;
      act.eval_begin = b < en ? b : (e.text[0] == '=' ? 1 : 0);
      act.eval_end = b < en ? en : n;
    }
    const int n_text = static_cast<int>(e.text.size());
    const int caret = std::clamp(e.caret, 0, n_text);
    const bool sel = e.sel_start != e.sel_end;
    auto move_caret = [&](int to, bool extend) {  // the caret (Shift: the selection's end) to `to`
      const int anchor = sel ? e.sel_start : caret;
      e.pending_caret = e.caret = e.sel_end = to;
      e.pending_sel_from = e.sel_start = extend ? anchor : to;
      e.cursor_to_end = false;
      e.ref_begin = e.ref_end = -1;
      e.kpoint = false;
    };
    // Ctrl+Left / Right move by Excel's words in Edit mode (Shift selects; in Enter mode they end the entry, above);
    // Ctrl+Delete deletes to the end of the line and Ctrl+Backspace the word before the caret (Excel 16, measured:
    // ImGui's words made =B2+C2 one word, and Ctrl+Delete deleted a word)
    if (io.KeyCtrl && !io.KeyAlt && (!e.enter_mode || in_formula_bar)) {
      if (take(ImGuiKey_LeftArrow)) move_caret(WordLeft(e.text, caret), io.KeyShift);
      if (take(ImGuiKey_RightArrow)) move_caret(WordRight(e.text, caret), io.KeyShift);
    }
    if (io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
      const int lo = sel ? std::clamp(std::min(e.sel_start, e.sel_end), 0, n_text) : caret;
      const int hi = sel ? std::clamp(std::max(e.sel_start, e.sel_end), 0, n_text) : caret;
      if (take(ImGuiKey_Delete)) {
        const size_t nl = e.text.find('\n', static_cast<size_t>(hi));
        const int to = nl == std::string::npos ? n_text : static_cast<int>(nl);
        if (to > lo) SetEditText(e, std::string(e.text).erase(static_cast<size_t>(lo), static_cast<size_t>(to - lo)), lo);
      }
      if (take(ImGuiKey_Backspace)) {
        const int from = sel ? lo : WordLeft(e.text, caret);
        if (hi > from) SetEditText(e, std::string(e.text).erase(static_cast<size_t>(from), static_cast<size_t>(hi - from)), from);
      }
      // Ctrl+Z / Ctrl+Y: the last run of typing back, and again (ImGui's own undo, a character at a time, is off)
      const bool undo = take(ImGuiKey_Z), redo = take(ImGuiKey_Y);
      std::vector<EditState::UndoStep>& from_list = undo ? e.undo : e.redo;
      std::vector<EditState::UndoStep>& to_list = undo ? e.redo : e.undo;
      if ((undo || redo) && !from_list.empty()) {
        to_list.push_back(EditState::UndoStep{e.text, caret});
        const EditState::UndoStep step = from_list.back();
        from_list.pop_back();
        e.undo_last = step.text;  // (the box's next callback sees no change of its own)
        e.undo_last_caret = step.caret;
        e.undo_run = false;
        SetEditText(e, step.text, step.caret);
        if (step.sel_from >= 0) e.pending_sel_from = e.sel_start = step.sel_from;
        e.ref_begin = e.ref_end = -1;
        e.kpoint = false;
        e.ac_full.clear();
      }
    }
    // an arrow pressed before the box has the keyboard (F2, then Left at once) would be lost: it moves the caret the
    // box starts with (edit mode only; in enter mode the arrows leave the cell). A box that reloads its own state this
    // frame reads the arrows itself
    if ((e.focus_request || e.focus_waiting) && !field_active && !reload && !e.enter_mode && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
      const int n = static_cast<int>(e.text.size());
      int at = e.pending_caret >= 0 ? e.pending_caret : (e.cursor_to_end ? n : std::clamp(e.caret, 0, n));
      bool moved = false;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && at > 0) {
        do --at;
        while (at > 0 && (static_cast<unsigned char>(e.text[static_cast<size_t>(at)]) & 0xC0) == 0x80);
        moved = true;
      } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && at < n) {
        do ++at;
        while (at < n && (static_cast<unsigned char>(e.text[static_cast<size_t>(at)]) & 0xC0) == 0x80);
        moved = true;
      } else if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        at = 0;
        moved = true;
      }
      if (moved) {
        e.pending_caret = at;
        e.cursor_to_end = false;
      }
    }
    // the frame the box takes the keyboard without its old state (another text box had it last): ImGui starts it with
    // the caret at the start, so this frame's keys are left out rather than applied there (the characters and Backspace
    // went into the text above); from the next frame it reads every key
    if (activate && !reload) {
      for (const ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Home, ImGuiKey_End,
                               ImGuiKey_PageUp, ImGuiKey_PageDown, ImGuiKey_Delete, ImGuiKey_Backspace, ImGuiKey_Insert, ImGuiKey_A,
                               ImGuiKey_C, ImGuiKey_V, ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z})
        take(k);
    } else if (reload) {
      take(ImGuiKey_Backspace);  // (its Backspace already deleted above)
    }
  }
  if (mine && field_active) e.focus_waiting = false;
  if (mine && e.focus_request) {
    e.focus_request = false;
    e.focus_waiting = true;  // until the box has the keyboard (taken below; a frame it isn't drawn tries again)
  }
  if (e.text.capacity() < 16) e.text.reserve(16);
  // (the editor keeps its own undo of whole runs of typing, as Excel's: ImGui's undoes a character at a time)
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackEdit |
                                    ImGuiInputTextFlags_NoUndoRedo | (word_wrap ? ImGuiInputTextFlags_WordWrap : 0);
  // the references of a formula in the colors of their boxes (the field's own text is drawn transparent)
  const bool colorize = mine && field_active && ImGui::GetInputTextState(fid) != nullptr && !e.ref_spans.empty() && formula::LooksLikeFormula(e.text);
  const ImU32 base_col = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 pad = ImGui::GetStyle().FramePadding;
  if (colorize) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
  // the box takes the keyboard now, not a frame later through SetKeyboardFocusHere: keys sent in the frames after the
  // editor opened (Shift+Home, Ctrl+Left, Delete, the Enter after an AutoComplete letter) were lost or met a caret at
  // the start. Its own state (this box was the last one active) reloads from the text with the caret and selection the
  // editor wants before it reads this frame's keys (ImGui's WantReloadUserBuf)
  const ImGuiID nav_before = g.NavActivateId;
  const ImGuiActivateFlags nav_flags_before = g.NavActivateFlags;
  if (activate) {
    if (reload) {
      const int n = static_cast<int>(e.text.size());
      const int to = e.cursor_to_end ? n : e.pending_caret >= 0 ? std::clamp(e.pending_caret, 0, n) : std::clamp(e.caret, 0, n);
      const int from = !e.cursor_to_end && e.pending_caret >= 0 && e.pending_sel_from >= 0 ? std::clamp(e.pending_sel_from, 0, n) : to;
      g.InputTextState.WantReloadUserBuf = true;
      g.InputTextState.ReloadSelectionStart = from;
      g.InputTextState.ReloadSelectionEnd = to;
      e.cursor_to_end = e.pending_replace = false;  // (the text is e.text already)
      e.pending_caret = e.pending_sel_from = -1;
      e.caret = e.sel_end = to;
      e.sel_start = from;
    }
    g.NavActivateId = fid;
    g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
  }
  const bool changed = ImGui::InputTextMultiline(id, e.text.data(), e.text.capacity() + 1, size, flags, EditCallback, &cb);
  if (activate) {
    g.NavActivateId = nav_before;
    g.NavActivateFlags = nav_flags_before;
  }
  if (colorize) {
    ImGui::PopStyleColor();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiWindow* child = nullptr;
    for (int k = win->DC.ChildWindows.Size - 1; k >= 0 && !child; --k) {
      if (win->DC.ChildWindows[k]->ChildId == fid) child = win->DC.ChildWindows[k];
    }
    const ImGuiInputTextState& st = ImGui::GetCurrentContext()->InputTextState;
    if (child) DrawFormulaText(e, child, ImVec2(child->Pos.x + pad.x - st.Scroll.x, child->Pos.y + pad.y - child->Scroll.y), word_wrap ? st.WrapWidth : 0.0f, base_col);
  }
  for (int i = 0; i < n_hidden; ++i) ImGui::GetKeyData(hidden[i])->Down = true;
  if (ImGui::IsItemActivated() && e.active) e.in_formula_bar = in_formula_bar;
  const ImRect field(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  if (mine && e.active && (field_active || ImGui::IsItemActive())) DrawAssist(e, assist, field);
  return changed;
}

}  // namespace yc::render
