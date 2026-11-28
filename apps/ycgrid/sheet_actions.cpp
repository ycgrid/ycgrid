// SPDX-License-Identifier: Apache-2.0
// sheet_actions.cpp - engine operations on the current selection (shared by the ribbon, shortcuts, context menus and the grid)
#include "app_state.h"
#include "commands/command_registry.h"
#include "core/utf8.h"
#include "frame/frame_context.h"
#include "settings.h"
#include "trust.h"
#include "screenshot.h"
#include "core/i18n.h"

#include "engine/navigate.h"
#include "engine/tile_iter.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "formula/datetime.h"
#include "formula/functions.h"
#include "formula/numfmt.h"
#include "formula/parser.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <tuple>

namespace yc::app {

using engine::Document;

std::vector<Rect> SelAreas(AppState& s) {
  std::vector<Rect> out;
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return out;
  const auto& sel = s.FocusView().sel;
  for (Rect a : sel.areas) {
    if (sel.mode == render::SelectMode::Rows || sel.mode == render::SelectMode::All) {
      a.c0 = 0;
      a.c1 = sh->col_limit - 1;
    }
    if (sel.mode == render::SelectMode::Cols || sel.mode == render::SelectMode::All) {
      a.r0 = 0;
      a.r1 = sh->row_limit - 1;
    }
    // expand to the whole merged area
    for (bool changed = true; changed;) {
      changed = false;
      for (const Rect& m : *sh->merges) {
        if (a.Intersects(m) && !a.Contains(m)) {
          a = a.Union(m);
          changed = true;
        }
      }
    }
    out.push_back(a);
  }
  return out;
}

Rect SelRect(AppState& s) {
  auto a = SelAreas(s);
  return a.empty() ? Rect{} : a.back();
}

namespace {

// a font's line height for one pixel of em size (its ascent + descent from the font file, as Windows and Excel measure
// it: Calibri 1.22, a script or CJK font more); a font that isn't installed uses the default font's
float LineHeightPerEm(const std::string& name, bool bold, bool italic) {
  static std::map<std::string, float> cache;
  std::string family = name;
  const auto& fams = ui::SystemFontFamilies();
  if (std::find(fams.begin(), fams.end(), family) == fams.end()) family = "Calibri";
  const std::string key = family + (bold ? "|b" : "|") + (italic ? "i" : "");
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  float ratio = 1.0f;
  if (HDC dc = ::CreateCompatibleDC(nullptr)) {
    const std::wstring w = FromUtf8(family);
    HFONT f = ::CreateFontW(-100, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, w.c_str());
    if (f) {
      HGDIOBJ old = ::SelectObject(dc, f);
      TEXTMETRICW tm{};
      if (::GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) ratio = static_cast<float>(tm.tmHeight) / 100.0f;
      ::SelectObject(dc, old);
      ::DeleteObject(f);
    }
    ::DeleteDC(dc);
  }
  cache[key] = ratio;
  return ratio;
}

// the height of a cell's text at px (the em size): its lines times the font's own line height, so a taller font
// such as a CJK or script font needs a taller row at the same size, as in Excel
float TextBlockHeight(ImFont* font, float px, float wrap_w, const std::string& text, const std::string& name, bool bold, bool italic) {
  const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, wrap_w, text.c_str());
  const float lines = std::max(1.0f, std::round(ts.y / std::max(1.0f, px)));
  // each line takes a whole one-line row (its pixels and the 2 around them): Excel 16 fits wrapped Calibri 11 at a
  // multiple of one line's height (measured: 29, 43.5, 58, 72.5 for 2 to 5 lines where one line is 14.5), where
  // the lines rounded together came 1.5 points short at two lines and 3.5 at five. The caller adds the last 2
  const float line = std::ceil(std::max(px, px * LineHeightPerEm(name, bold, italic))) + 2.0f;
  return lines * line - 2.0f;
}

// Excel: a row that sizes itself fits its text again after a change (wrapped text, line breaks, a bigger font).
// A row sizes itself when it has the standard height or exactly the height its text needed before the change; a
// height set by hand stays. restyle: the style change about to be made to areas; typed: the text about to go into
// (typed_r, typed_c). Returns the rows whose height changes, with their new heights.
std::vector<std::pair<uint32_t, int32_t>> RowsToFit(AppState& s, uint32_t sheet, const std::vector<Rect>& areas,
                                                    const std::function<void(model::Style&)>* restyle, const std::string* typed = nullptr,
                                                    uint32_t typed_r = 0, uint32_t typed_c = 0, const std::string* new_font = nullptr,
                                                    bool cleared = false, bool cleared_formats = false,
                                                    const std::map<uint64_t, std::string>* writes = nullptr) {
  std::vector<std::pair<uint32_t, int32_t>> out;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  // a protected sheet keeps its row heights unless it lets the user format rows
  if (!sh || (sh->protection->on && !sh->protection->format_rows)) return out;
  const model::StyleTable& styles = *s.snap->wb.styles;
  auto in_areas = [&](uint32_t r, uint32_t c) {
    for (const Rect& a : areas) {
      if (a.Contains(r, c)) return true;
    }
    return false;
  };
  int cells = 0;  // cells measured: a big selection stops early (the rows done so far still fit)
  auto measure = [&](uint32_t row, bool after) {
    float best = 0;
    auto cell = [&](uint32_t col, model::Style st, const std::string& text, bool numeric) {
      const bool changed = after && restyle && in_areas(row, col);
      if (changed) (*restyle)(st);
      if (st.rotation || text.empty()) return;  // rotated text: AutoFit Row Height measures it
      // a font name chosen in Format Cells may not be in the table yet: measured by its name
      ImFont* font = ui::CellFont(changed && new_font ? *new_font : styles.FontName(st), st.bold, st.italic);
      const float px = styles.FontSize(st) * (96.0f / 72.0f);
      const float wrap_w = st.wrap && !numeric ? std::max(1.0f, static_cast<float>(sh->cols->Size(col)) - 8.0f) : 0.0f;
      best = std::max(best, TextBlockHeight(font, px, wrap_w, text, changed && new_font ? *new_font : styles.FontName(st), st.bold, st.italic));
    };
    if (sh->has_used) {
      engine::ForEachValueRowMajor(*sh, Rect{row, sh->used.c0, row, sh->used.c1}, [&](uint32_t, uint32_t col, const core::Value& v) {
        for (const Rect& m : *sh->merges) {
          if (m.Contains(row, col) && !m.IsCell()) return true;
        }
        // the typed text is measured below (a formula's result isn't known yet: the old one stands in)
        if (after && typed && row == typed_r && col == typed_c && !formula::LooksLikeFormula(*typed)) return true;
        if (after && writes && writes->count((uint64_t{row} << 16) | col)) return true;  // (below too)
        // cells being cleared (cleared: areas are what Delete or Clear empties) keep one line of their font, or of
        // the standard one when their formats go too (Excel 16: a 28 pt cell cleared keeps its row's 39 points)
        if (after && cleared && in_areas(row, col)) {
          cell(col, cleared_formats ? styles.Get(0) : styles.Get(sh->GetStyle(row, col)), "x", false);
          return ++cells < 200000;
        }
        const model::Style& st = styles.Get(sh->GetStyle(row, col));
        cell(col, st, render::FormatCellText(*s.snap, st, v, -1, nullptr, nullptr, nullptr), v.IsNumeric());
        return ++cells < 200000;
      });
    }
    if (after && typed && row == typed_r && !formula::LooksLikeFormula(*typed)) {
      model::Style st = styles.Get(sh->GetStyle(typed_r, typed_c));
      if (typed->find('\n') != std::string::npos) st.wrap = true;  // CommitEdit turns Wrap Text on for line breaks
      cell(typed_c, st, *typed, false);
    }
    if (after && writes) {  // what a script writes into the row: each value's text (an empty cell keeps one line)
      for (auto it = writes->lower_bound(uint64_t{row} << 16); it != writes->end() && (it->first >> 16) == row; ++it) {
        const uint32_t col = static_cast<uint32_t>(it->first & 0xFFFF);
        cell(col, styles.Get(sh->GetStyle(row, col)), it->second.empty() ? std::string("x") : it->second, false);
      }
    }
    const int32_t def = sh->rows->default_size();
    return best > 0 ? std::max(def, static_cast<int32_t>(std::ceil(best)) + 2) : def;
  };
  int budget = 2000;  // rows measured (a whole-column selection fits the used rows only)
  for (const Rect& a : areas) {
    uint32_t r0 = a.r0, r1 = a.r1;
    if (!typed && !writes) {
      if (!sh->has_used) break;
      r0 = std::max(r0, sh->used.r0);
      r1 = std::min(r1, sh->used.r1);
    }
    for (uint32_t row = r0; r0 <= r1 && row <= r1; ++row) {
      if (--budget < 0 || cells >= 200000) return out;
      if (sh->rows->IsHidden(row)) continue;
      const int32_t cur = sh->rows->Size(row);
      if (sh->rows->IsCustom(row) && cur != measure(row, false)) continue;  // a height set by hand
      const int32_t want = measure(row, true);
      if (want != cur) out.emplace_back(row, want);
    }
  }
  return out;
}

}  // namespace

void ApplyStyle(AppState& s, const std::string& label, std::function<void(model::Style&)> fn) {
  if (!s.engine) return;
  EndCopyMode(s);  // Excel: formatting cells ends cut / copy mode (a later Enter mustn't paste over the selection)
  const auto areas = SelAreas(s);
  const uint32_t sheet = s.focus_sheet;
  const auto fit = RowsToFit(s, sheet, areas, &fn);
  const std::vector<uint32_t> group = GroupTargets(s, sheet);  // grouped sheets get the format too (Excel)
  s.engine->PostUser(label, [areas, sheet, fn, fit, group](Document& d) {
    for (const uint32_t g : group) {
      for (const Rect& a : areas) d.ModifyStyle(g, a, fn);
    }
    for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
  });
}

std::vector<std::pair<uint32_t, int32_t>> FitRowsForStyle(AppState& s, uint32_t sheet, const std::vector<Rect>& areas,
                                                          const std::function<void(model::Style&)>& restyle, const std::string* font) {
  return RowsToFit(s, sheet, areas, &restyle, nullptr, 0, 0, font);
}

// a row that sizes itself fits what is left once areas are cleared (Excel 16, measured 2026-11-28: two lines typed
// with Alt+Enter, then Delete, and the row is back at the standard height; ClearContents alike)
std::vector<std::pair<uint32_t, int32_t>> FitRowsAfterClear(AppState& s, uint32_t sheet, const std::vector<Rect>& areas, bool formats) {
  return RowsToFit(s, sheet, areas, nullptr, nullptr, 0, 0, nullptr, true, formats);
}

// a row that sizes itself fits the values a script writes (Excel 16's Range.Value, measured 2026-11-28: two lines
// replaced by "c" put the row back at the standard height)
std::vector<std::pair<uint32_t, int32_t>> FitRowsForWrite(AppState& s, uint32_t sheet, const Rect& rect, const std::map<uint64_t, std::string>& texts) {
  return RowsToFit(s, sheet, {rect}, nullptr, nullptr, 0, 0, nullptr, false, false, &texts);
}

namespace {

Rect AnchorOf(const model::Sheet& sh, uint32_t r, uint32_t c) {
  for (const Rect& m : *sh.merges) {
    if (m.Contains(r, c)) return m;
  }
  return Rect::Cell(r, c);
}

// Excel: missing closing parentheses are added when a formula is confirmed
std::string AutoCloseParens(std::string t) {
  if (t.size() < 2 || t[0] != '=') return t;
  int depth = 0;
  bool in_str = false;
  for (char ch : t) {
    if (ch == '"') in_str = !in_str;
    else if (!in_str && ch == '(') ++depth;
    else if (!in_str && ch == ')') --depth;
  }
  if (!in_str && depth > 0) t.append(static_cast<size_t>(depth), ')');
  return t;
}

// move: 1 Enter, 2 Shift+Enter, 3 Tab, 4 Shift+Tab, 5-8 an arrow key (down, up, right, left) that ended the entry
// Tab in a table's last cell (render::TabTarget's add_row, on the sheet or after an entry): a row at the table's end,
// the active cell already on the row's first cell (Excel)
void AddTableRowAfterTab(AppState& s, uint32_t sheet_id, int64_t table) {
  if (table < 0 || !s.engine || RefuseReadOnlyUi(s)) return;
  const uint32_t id = static_cast<uint32_t>(table);
  s.engine->PostUser(TR("Insert Table Rows"), [sheet_id, id](Document& d) { d.TableInsertRow(sheet_id, id, -1); });
}

}  // namespace

void ProtectionCellRules(const model::Sheet& sh, const model::StyleTable* styles, std::function<bool(uint32_t, uint32_t)>* unlocked,
                         std::function<bool(uint32_t, uint32_t)>* selectable) {
  *unlocked = nullptr;
  *selectable = nullptr;
  const model::SheetProtection& prot = *sh.protection;
  if (!prot.on || !styles) return;
  const model::Sheet* p = &sh;
  const bool sel_locked = prot.select_locked && prot.select_unlocked, sel_unlocked = prot.select_unlocked;
  *unlocked = [p, styles](uint32_t r, uint32_t c) { return !styles->Get(p->GetStyle(r, c)).locked; };
  if (!sel_locked) *selectable = [p, styles, sel_unlocked](uint32_t r, uint32_t c) { return sel_unlocked && !styles->Get(p->GetStyle(r, c)).locked; };
}

namespace {

void MoveActive(AppState& s, int move) {
  if (move == 0) return;
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  // every row or every column hidden: Enter and Tab after an entry leave the active cell where it is, and with every
  // row hidden no arrow moves it either (Excel 16, measured); with every column hidden Down and Up still go on
  const bool no_rows = sh->rows->Offset(sh->row_limit) == 0, no_cols = sh->cols->Offset(sh->col_limit) == 0;
  if (no_rows || (no_cols && move < 5)) return;
  auto& v = s.FocusView();
  auto& sel = v.sel;
  const Rect anchor = AnchorOf(*sh, sel.act_r, sel.act_c);
  // one visible row / column on from the edge of the (merged) active cell
  auto step = [&](int dir, int64_t* r, int64_t* c) {
    if (dir == 1) *r = sh->rows->NextVisible(int64_t{anchor.r1} + 1, +1, sh->row_limit);
    if (dir == 2) *r = anchor.r0 == 0 ? -1 : sh->rows->NextVisible(int64_t{anchor.r0} - 1, -1, sh->row_limit);
    if (dir == 3) *c = sh->cols->NextVisible(int64_t{anchor.c1} + 1, +1, sh->col_limit);
    if (dir == 4) *c = anchor.c0 == 0 ? -1 : sh->cols->NextVisible(int64_t{anchor.c0} - 1, -1, sh->col_limit);
  };
  if (move >= 5) {
    // an arrow key ends the entry like a move (Excel): one cell that way, the selection collapses to it, no Tab chain
    int64_t r = anchor.r0, c = anchor.c0;
    step(move - 4, &r, &c);
    if (r < 0) r = anchor.r0;
    if (c < 0) c = anchor.c0;
    render::SelectCell(v, static_cast<uint32_t>(r), static_cast<uint32_t>(c), false);
    sel.tab_from = UINT32_MAX;
    render::EnsureVisible(*sh, v, sel.act_r, sel.act_c);
    return;
  }
  const bool tab = move == 3 || move == 4;
  if (!tab) {
    // File > Options: After pressing Enter, move selection (and which way; Shift+Enter goes the other way)
    if (!Config().move_after_return) return;
    static const int kDir[4] = {1, 3, 2, 4};  // down, right, up, left
    static const int kBack[5] = {0, 2, 1, 4, 3};
    const int d = kDir[std::clamp(Config().move_after_return_dir, 0, 3)];
    move = move == 2 ? kBack[d] : d;
  }
  // whole rows / columns (the whole sheet) are walked within the used range, and stay selected (Excel)
  auto used_part = [&](Rect x) {
    if (x.c0 == 0 && x.c1 >= sh->col_limit - 1) x.c1 = std::max<uint32_t>(sh->has_used ? sh->used.c1 : 0, sel.act_c);
    if (x.r0 == 0 && x.r1 >= sh->row_limit - 1) x.r1 = std::max<uint32_t>(sh->has_used ? sh->used.r1 : 0, sel.act_r);
    return x;
  };
  const Rect a = used_part(SelRect(s));
  int64_t r = sel.act_r, c = sel.act_c;
  if (sel.areas.size() > 1) {
    // a multiple selection: through the active area, then on to the next one (Excel); the areas stay selected
    bool wrap = false;
    if (move == 1 || move == 2) {
      r += move == 1 ? 1 : -1;
      if (r > a.r1) r = a.r0, ++c, wrap = c > a.c1;
      if (r < a.r0) r = a.r1, --c, wrap = c < a.c0;
    } else {
      c += move == 3 ? 1 : -1;
      if (c > a.c1) c = a.c0, ++r, wrap = r > a.r1;
      if (c < a.c0) c = a.c1, --r, wrap = r < a.r0;
    }
    if (wrap) {
      const bool fwd = move == 1 || move == 3;
      if (fwd) std::rotate(sel.areas.begin(), sel.areas.begin() + 1, sel.areas.end());
      else std::rotate(sel.areas.rbegin(), sel.areas.rbegin() + 1, sel.areas.rend());
      const Rect na = used_part(sel.areas.back());
      r = fwd ? na.r0 : na.r1;
      c = fwd ? na.c0 : na.c1;
      sel.anc_r = na.r0;
      sel.anc_c = na.c0;
    }
    sel.act_r = static_cast<uint32_t>(r);
    sel.act_c = static_cast<uint32_t>(c);
  } else if (a.IsCell() || a == anchor) {
    // the sheet's own rules for Tab (a protected sheet's unlocked cells, a table's rows), as Tab on the sheet does
    if (tab) {
      std::function<bool(uint32_t, uint32_t)> unlocked, selectable;
      ProtectionCellRules(*sh, s.snap ? &s.snap->wb.styles.get() : nullptr, &unlocked, &selectable);
      uint32_t tr = 0, tc = 0;
      int64_t add_row = -1;
      if (render::TabTarget(*sh, sel.act_r, sel.act_c, move == 4, unlocked, selectable, &tr, &tc, &add_row)) {
        render::SelectCell(v, tr, tc, false);
        sel.tab_from = UINT32_MAX;
        AddTableRowAfterTab(s, sh->id, add_row);
        render::EnsureVisible(*sh, v, sel.act_r, sel.act_c);
        return;
      }
      if (unlocked) return;  // a protected sheet without another unlocked cell: the active cell stays
    }
    const uint32_t tab_from = sel.TabFrom();
    if (tab) sel.NoteTab(move == 3);
    switch (move) {
      case 1:
        r = anchor.r1 + 1;
        if (tab_from != UINT32_MAX) c = tab_from;  // the row typed with Tab: Enter starts the next one (Excel)
        break;
      case 2: r = static_cast<int64_t>(anchor.r0) - 1; break;
      case 3: c = anchor.c1 + 1; break;
      default: c = static_cast<int64_t>(anchor.c0) - 1; break;
    }
    r = std::clamp<int64_t>(r, 0, sh->row_limit - 1);
    c = std::clamp<int64_t>(c, 0, sh->col_limit - 1);
    render::SelectCell(v, static_cast<uint32_t>(r), static_cast<uint32_t>(c), false);
    if (tab) sel.NoteTabDone();
    else sel.tab_from = UINT32_MAX;
  } else {
    // move within the selection
    if (move == 1 || move == 2) {
      r += move == 1 ? 1 : -1;
      if (r > a.r1) {
        r = a.r0;
        c = c + 1 > a.c1 ? a.c0 : c + 1;
      }
      if (r < a.r0) {
        r = a.r1;
        c = c - 1 < static_cast<int64_t>(a.c0) ? a.c1 : c - 1;
      }
    } else {
      c += move == 3 ? 1 : -1;
      if (c > a.c1) {
        c = a.c0;
        r = r + 1 > a.r1 ? a.r0 : r + 1;
      }
      if (c < a.c0) {
        c = a.c1;
        r = r - 1 < static_cast<int64_t>(a.r0) ? a.r1 : r - 1;
      }
    }
    sel.act_r = static_cast<uint32_t>(r);
    sel.act_c = static_cast<uint32_t>(c);
  }
  render::EnsureVisible(*sh, v, sel.act_r, sel.act_c);
}

}  // namespace

bool EditHasTextSelection(const AppState& s) {
  const auto& e = s.edit;
  return e.active && e.sel_start != e.sel_end && !(e.text.size() > 0 && e.text[0] == '=');
}

bool EditSelectionFormat(AppState& s, const std::function<void(model::RichRun&, const model::RunFont& at_start)>& fn) {
  if (!EditHasTextSelection(s)) return false;
  auto& e = s.edit;
  const size_t b0 = static_cast<size_t>(std::clamp(std::min(e.sel_start, e.sel_end), 0, static_cast<int>(e.text.size())));
  const size_t b1 = static_cast<size_t>(std::clamp(std::max(e.sel_start, e.sel_end), 0, static_cast<int>(e.text.size())));
  model::RichText rt = model::RemapRuns(s.edit_rich, e.text);
  const size_t a = model::CharCount(std::string_view(e.text).substr(0, b0)), n = model::CharCount(std::string_view(e.text).substr(b0, b1 - b0));
  const model::Sheet* sh = s.snap ? s.snap->Sheet(e.sheet) : nullptr;
  const model::Style cell = sh ? s.snap->wb.styles->Get(sh->GetStyle(e.r, e.c)) : model::Style{};
  const model::RunFont at = model::ResolveRunFont(*s.snap->wb.styles, cell, model::RunAt(rt, a));
  model::ApplyToChars(&rt, a, n, [&](model::RichRun& r) { fn(r, at); });
  s.edit_rich = std::move(rt);
  e.focus_request = true;
  return true;
}

void StartEdit(AppState& s, bool replace, std::string initial, bool in_formula_bar) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap) return;
  const auto& sel = s.FocusView().sel;
  const Rect anchor = AnchorOf(*sh, sel.act_r, sel.act_c);
  if (RefuseLockedEdit(s, s.focus_sheet, anchor.r0, anchor.c0)) return;  // protected sheet: like Excel, refuse before typing
  if (trust::InProtectedView()) return trust::ProtectedViewBlocked(s);  // Protected View: no cell goes into editing
  if (!s.operator_mode) {  // read-only mode: no cell goes into editing; clicking and selecting still work
    s.SetStatus(TR("Read-only mode: the sheet can't be changed. Turn on Trading > Operator Mode to edit."), 5.0);
    return;
  }
  auto& e = s.edit;
  e = render::EditState{};
  e.active = true;
  e.sheet = s.focus_sheet;
  e.r = anchor.r0;
  e.c = anchor.c0;
  e.r1c1 = Config().r1c1;
  e.view_id = s.focus_win;
  e.text = replace ? std::move(initial) : engine::InputTextOf(s.snap->wb, e.sheet, e.r, e.c, e.r1c1);
  uint32_t ar = 0, ac = 0;
  if (!replace && ArrayFormulaAt(s, e.sheet, e.r, e.c, &ar, &ac)) e.text = engine::FormulaTextOf(s.snap->wb, e.sheet, ar, ac, e.r1c1);
  e.original = e.text;
  // the fonts inside the text come along (typing over the cell starts plain)
  s.edit_rich = model::RichText{};
  if (!replace) {
    auto it = sh->rich->find((uint64_t{e.r} << 16) | e.c);
    const core::Value& v = sh->GetValue(e.r, e.c);
    if (it != sh->rich->end() && v.IsString() && v.str() == it->second.text) s.edit_rich = it->second;
  }
  e.enter_mode = replace;
  e.focus_request = true;
  e.cursor_to_end = true;
  e.in_formula_bar = in_formula_bar;
  e.caret = static_cast<int>(e.text.size());
  // a percent cell: a number typed is a percentage, Excel's automatic percent entry; the % goes in after the first digit
  // with the caret before it, and stays when the entry turns out not to be a number (Excel 16, measured: 5 then 1
  // shows 51%, 5a is the text 5a%). The text that starts an entry can hold several characters at once (a Chinese IME
  // commits 12.5% together): one with its own % gets none more, or it was the text 12.5%%
  if (replace && !e.text.empty() && e.text[0] >= '0' && e.text[0] <= '9' && e.text.find('%') == std::string::npos) {
    const std::string code = s.snap->wb.styles->NumberFormat(s.snap->wb.styles->Get(sh->GetStyle(e.r, e.c)));
    if (formula::IsPercentFormat(code)) {
      e.text += '%';
      e.pending_caret = e.caret;
      e.cursor_to_end = false;
      e.auto_pct = true;
    }
  }
  // Ctrl+Z in the editor: typing over a cell is one run from the cell's own content (Excel 16, measured: abc, Ctrl+Z,
  // Enter keeps 10); editing it starts with nothing to take back
  if (replace) {
    e.undo.push_back(render::EditState::UndoStep{engine::InputTextOf(s.snap->wb, e.sheet, e.r, e.c, e.r1c1), 0, 0});
    e.undo.back().caret = static_cast<int>(e.undo.back().text.size());
    e.undo_run = true;
  }
  e.undo_last = e.text;
  e.undo_last_caret = e.pending_caret >= 0 ? e.pending_caret : e.caret;
  e.autocomplete = Config().enable_autocomplete;
  e.formula_list = Config().formula_autocomplete;
  e.formula_tips = Config().function_tooltips;
  EndCopyMode(s);  // Excel: typing in a cell ends cut / copy mode
  render::EnsureVisible(*sh, s.FocusView(), e.r, e.c);
}

const model::SpillRange* ArrayFormulaAt(const AppState& s, uint32_t sheet, uint32_t r, uint32_t c, uint32_t* anchor_r, uint32_t* anchor_c) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || sh->spills->empty()) return nullptr;
  const model::SpillRange* sp = sh->SpillAt(r, c, anchor_r, anchor_c);
  return sp && sp->cse ? sp : nullptr;
}

bool RefusePartOfArray(AppState& s, uint32_t sheet, const std::vector<Rect>& areas) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || sh->spills->empty()) return false;
  for (const auto& [k, sp] : *sh->spills) {
    if (!sp.cse) continue;
    for (const Rect& a : areas) {
      if (a.Intersects(sp.rect) && !a.Contains(sp.rect)) {
        s.ShowError("YCGrid", TR("You can't change part of an array."));
        return true;
      }
    }
  }
  return false;
}

// Ctrl+Enter fills the selection from the active cell. An entry that grew a table over the active cell made it a
// header, whose text is the entry's as text: the fill then starts from the first cell of the selection that isn't a
// header, typed again there, so the data cells get the entry as typed (1 Ctrl+Enter over E2:E6 beside a table: E2 the
// header "1", E3:E6 the number 1, Excel 16 measured by keys); the header is made text again at Commit
static bool FillSourceCell(Document& d, uint32_t sheet, const std::vector<Rect>& areas, std::string_view text, bool r1c1, uint32_t* r, uint32_t* c) {
  const model::Sheet* s = d.S(sheet);
  if (!s || s->tables->empty()) return true;
  auto header = [&](uint32_t hr, uint32_t hc) {
    for (const model::Table& t : *s->tables) {
      if (t.header && t.range.r0 == hr && t.range.c0 <= hc && hc <= t.range.c1) return true;
    }
    return false;
  };
  if (!header(*r, *c)) return true;
  for (const Rect& a : areas) {
    for (uint32_t rr = a.r0; rr <= a.r1 && rr - a.r0 < 2; ++rr) {  // a table's header row is one row
      for (uint32_t cc = a.c0; cc <= a.c1; ++cc) {
        if (header(rr, cc)) continue;
        std::string ignored;
        if (!d.SetInput(sheet, rr, cc, text, &ignored, engine::kSrcUser, r1c1)) return false;
        *r = rr, *c = cc;
        return true;
      }
    }
  }
  return true;  // every cell filled is a header: they all take the active cell's text
}

// Ctrl+Enter: the format the entry gave its cell (5%, a date, =TODAY()) goes to every cell filled, as in Excel
void FillEntryFormat(Document& d, uint32_t sheet, uint32_t r, uint32_t c, uint32_t before, const std::vector<Rect>& areas) {
  const model::Sheet* s = d.S(sheet);
  if (!s) return;
  const uint32_t now = d.wb().styles->Get(s->GetStyle(r, c)).numfmt;
  if (now == before) return;
  for (const Rect& a : areas) {
    d.ModifyStyle(sheet, a, [now](model::Style& x) { x.numfmt = now; });
  }
}

// a number typed into a percent cell without its % is a percentage too, Excel's automatic percent entry (Excel 16,
// measured: .5 is 0.5%, and F2 on 10%, Backspace, Enter keeps 10%); only typing, never Range.Value from Python
static std::string PercentTyped(const AppState& s, const render::EditState& e, std::string text) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(e.sheet) : nullptr;
  if (!sh) return text;
  return formula::PercentEntry(text, s.snap->wb.styles->NumberFormat(s.snap->wb.styles->Get(sh->GetStyle(e.r, e.c))));
}

void CommitEdit(AppState& s, bool array, int move, bool cse) {
  auto& e = s.edit;
  if (!e.active || !s.engine) return;
  // AutoComplete's entry as the column has it (APP, then Enter, is Apple), AutoCorrect's words (Spelling >
  // AutoCorrect), then a percent cell's percentage
  const std::string typed = !e.ac_full.empty() && formula::FoldCase(e.text) == formula::FoldCase(e.ac_full) ? e.ac_full : e.text;
  const std::string text = PercentTyped(s, e, ApplyAutoCorrect(AutoCloseParens(typed)));
  const uint32_t sheet = e.sheet, r = e.r, c = e.c;
  const bool r1c1 = e.r1c1;
  // Enter on Flash Fill's grey suggestions fills the column with them (datatools_ui.cpp)
  if (!array && !cse && FlashFillAccept(s)) {
    e.active = false;
    MoveActive(s, move);
    return;
  }
  // Ctrl+Shift+Enter: an array formula over the array being edited, or else the selection
  if (cse && text.size() > 1 && text[0] == '=') {
    Rect target = Rect::Cell(r, c);
    if (const model::SpillRange* sp = ArrayFormulaAt(s, sheet, r, c)) {
      target = sp->rect;
    } else if (const auto areas = SelAreas(s); !areas.empty() && areas.back().Contains(r, c) && areas.back().Count() <= 1'000'000) {
      target = areas.back();
    }
    std::string error;
    ArmCircularWarning(s);
    if (EngineBehind(s) && !s.in_request) {  // handed over, as an entry below (ApplyPendingCommit)
      auto pc = std::make_shared<AppState::PendingCommit>();
      pc->edit = e;
      pc->hint = false;
      s.pending_commit = pc;
      s.engine->Post([pc, sheet, target, text, r1c1](Document& d) {
        d.Begin(TR("Enter"));
        pc->ok = d.SetArrayFormula(sheet, target, std::string_view(text).substr(1), &pc->error, engine::kSrcUser, r1c1);
        d.Commit();
        pc->done = true;
      });
      e.active = false;
      return;
    }
    const bool ok = s.engine->Call([&](Document& d) {
      d.Begin(TR("Enter"));
      const bool r_ok = d.SetArrayFormula(sheet, target, std::string_view(text).substr(1), &error, engine::kSrcUser, r1c1);
      d.Commit();
      return r_ok;
    });
    if (!ok) {
      s.ShowError("YCGrid", TR("There's a problem with this formula.\n\n") + error);
      e.focus_request = true;
      return;
    }
    e.active = false;
    return;
  }
  // a cell of a multi-cell array formula can't be changed on its own (like Excel)
  if (const model::SpillRange* sp = ArrayFormulaAt(s, sheet, r, c); sp && !sp->rect.IsCell()) {
    s.ShowError("YCGrid", TR("You can't change part of an array."));
    e.focus_request = true;
    return;
  }
  if (array && RefusePartOfArray(s, sheet, SelAreas(s))) {
    e.focus_request = true;
    return;
  }
  // a PivotTable report: typing over a caption or a label renames it (Excel); other cells are refused below
  if (!array && PivotCommitText(s, sheet, r, c, text)) {
    e.active = false;
    return;
  }
  // data validation (Stop / Warning / Information)
  switch (CheckValidation(s, sheet, r, c, text)) {
    case ValidationResult::Retry:
      e.focus_request = true;
      return;
    case ValidationResult::Cancel:
      e.active = false;
      return;
    default:
      break;
  }
  // [YC] parameter cells: operator mode, type and range, confirmation (safety_ui.cpp)
  bool param = false, cancel = false;
  if (!array && !SafetyBeforeCommit(s, sheet, r, c, text, &param, &cancel)) {
    if (cancel) e.active = false;
    else e.focus_request = true;
    return;
  }
  std::vector<Rect> areas;
  if (array) areas = SelAreas(s);
  // Ctrl+Enter in a filtered list fills the rows it shows (Excel)
  if (const model::Sheet* fsh = s.snap ? s.snap->Sheet(sheet) : nullptr; array && fsh) areas = engine::FilterShownRuns(*fsh, areas);
  std::string error;
  const model::RichText rich = model::RemapRuns(s.edit_rich, e.text);
  // a line break (Alt+Enter) turns Wrap Text on, and the row grows to fit the text, as in Excel
  const model::Sheet* esh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  const bool wrap_on = !array && text.find('\n') != std::string::npos && !formula::LooksLikeFormula(text) && esh &&
                       !(esh->protection->on && !esh->protection->format_cells);
  const auto fit = array ? std::vector<std::pair<uint32_t, int32_t>>{} : RowsToFit(s, sheet, {Rect::Cell(r, c)}, nullptr, &text, r, c);
  // Ctrl+Enter over a large selection (whole columns: millions of cells) fills it on the engine thread, like Excel
  // fills all of it, while the screen stays live. The formula is checked first, so a typo still comes back here.
  uint64_t fill_cells = 0;
  for (const Rect& a : areas) fill_cells += a.Count();
  if (array && fill_cells > 20'000'000) {  // the whole sheet (Ctrl+A) has 17 billion cells: memory and time run out
    s.ShowMessage("YCGrid", TR("The selection is too large to fill: ") + std::to_string(fill_cells) + TR(" cells. Select fewer cells, such as whole columns."));
    e.focus_request = true;
    return;
  }
  if (array && fill_cells > 100'000) {
    if (formula::LooksLikeFormula(text)) {
      const bool parses = s.engine->Call([&](Document& d) {
        const auto pr = formula::Parse(std::string_view(text).substr(text[0] == '=' ? 1 : 0), core::CellPos{r, c}, &d, r1c1);
        if (!pr.ok) error = pr.error;
        return pr.ok;
      });
      if (!parses) {
        s.ShowError("YCGrid", TR("There's a problem with this formula.\n\n") + error +
                                 TR("\n\nNot trying to type a formula? Start with an apostrophe (') and the cell will be treated as text."));
        e.focus_request = true;
        return;
      }
    }
    s.engine->PostUser(TR("Enter"), [sheet, r, c, text, r1c1, areas](Document& d) {
      std::string err;
      const uint32_t fmt0 = d.S(sheet) ? d.wb().styles->Get(d.S(sheet)->GetStyle(r, c)).numfmt : 0;
      if (!d.SetInput(sheet, r, c, text, &err, engine::kSrcUser, r1c1)) return;
      uint32_t fr = r, fc = c;
      if (!FillSourceCell(d, sheet, areas, text, r1c1, &fr, &fc)) return;
      d.FillFrom(sheet, fr, fc, areas);
      FillEntryFormat(d, sheet, fr, fc, fmt0, areas);
    });
    s.SetStatus(TR("Filling ") + std::to_string(fill_cells) + TR(" cells..."), 3.0);
    e.active = false;
    return;
  }
  ArmCircularWarning(s);
  // the undo list names the typing, as Excel does: Typing "1250" in B4
  std::string shown = text.size() > 24 ? std::string(core::Utf8Prefix(text, 21)) + "..." : text;
  const std::string label = TR("Typing \"") + shown + TR("\" in ") + core::CellA1(r, c);
  std::vector<uint32_t> group = GroupTargets(s, sheet);  // grouped sheets: the same entry in the same cells (Excel)
  group.erase(group.begin());
  // what a table does by itself after the typing (growing, filling a calculated column) is a step of its own: the
  // first Ctrl+Z takes back only that (Excel 16, measured by keys)
  const bool defer_growth = true;
  // AutoFormat As You Type: a web or e-mail address typed into one cell becomes a link with the Hyperlink style, in
  // a step of its own after the typing (Excel 16: the first Ctrl+Z takes back the link and keeps the text, measured)
  const std::string link_to = !array && Config().autoformat_hyperlinks ? engine::AutoLinkAddress(text) : std::string();
  // Extend data range formats and formulas: a value typed in the row under a list (Excel's, in the typing's step)
  const bool extend = !array && Config().extend_list && !formula::LooksLikeFormula(text);
  auto work = [=](Document& d, std::string* error) {
    d.defer_table_growth = defer_growth;
    d.Begin(label);
    d.param_commit = param;
    const uint32_t fmt_before = d.S(sheet) ? d.wb().styles->Get(d.S(sheet)->GetStyle(r, c)).numfmt : 0;
    const bool r_ok = d.SetInput(sheet, r, c, text, error, engine::kSrcUser, r1c1);
    d.param_commit = false;
    if (r_ok && extend) d.ExtendList(sheet, r, c);
    for (const uint32_t g : group) {
      if (!r_ok) break;
      std::string ignored;
      if (d.SetInput(g, r, c, text, &ignored, engine::kSrcUser, r1c1) && array) {
        uint32_t fr = r, fc = c;
        if (FillSourceCell(d, g, areas, text, r1c1, &fr, &fc)) d.FillFrom(g, fr, fc, areas);
      }
    }
    // fonts inside the text: follow the edits, or go away when the cell no longer holds text
    if (r_ok && !array) {
      const model::Sheet* ds = d.S(sheet);
      const bool had = ds && ds->rich->count((uint64_t{r} << 16) | c);
      const core::Value& v = d.GetValue(sheet, r, c);
      if (!rich.runs.empty() && v.IsString()) d.SetRichText(sheet, r, c, model::RemapRuns(rich, std::string(v.str())));
      else if (had) d.SetRichText(sheet, r, c, model::RichText{});
      // a typed date, time or formatted number wider than a column of the standard width widens it, as in Excel
      // (General numbers shrink to fit instead; formulas and columns the user sized are left alone)
      if (ds && v.IsNumber() && !text.empty() && !formula::LooksLikeFormula(text) && !ds->cols->IsCustom(c)) {
        const std::string& code = d.wb().styles->NumberFormat(d.wb().styles->Get(ds->GetStyle(r, c)));
        if (!code.empty() && code != "General") {
          const int32_t want = static_cast<int32_t>(engine::DisplayTextOf(v, code).size() * 7 + 12);
          if (want > ds->cols->Size(c)) d.SetColWidth(sheet, c, c, want);
        }
      }
    }
    if (r_ok && !array) {
      if (wrap_on) d.ModifyStyle(sheet, Rect::Cell(r, c), [](model::Style& x) { x.wrap = true; });
      for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
    }
    if (r_ok && array) {
      uint32_t fr = r, fc = c;
      if (FillSourceCell(d, sheet, areas, text, r1c1, &fr, &fc)) {
        d.FillFrom(sheet, fr, fc, areas);
        FillEntryFormat(d, sheet, fr, fc, fmt_before, areas);
      }
    }
    d.Commit();
    d.GrowDeferredTables();
    if (r_ok && !link_to.empty()) {
      const model::Sheet* ls = d.S(sheet);
      const core::Value& v = d.GetValue(sheet, r, c);
      if (ls && v.IsString() && !ls->links->count((uint64_t{r} << 16) | c)) {
        d.Begin(TR("Hyperlink"));
        model::Hyperlink link;
        link.address = link_to;
        d.SetHyperlink(sheet, r, c, link);
        d.ApplyCellStyle(sheet, Rect::Cell(r, c), "Hyperlink");
        d.Commit();
      }
    }
    return r_ok;
  };
  // the engine is busy (recalculating millions of formulas after a reload, a fill a frame before): the entry is handed
  // over and the editor closes at once, instead of the screen waiting for the engine; a refused formula comes back
  // with its message and the editor (ApplyPendingCommit). Busy for any time at all: Enter a frame after a million
  // formulas were filled waited 1.5 s while the engine had been busy for less than the 0.1 s asked before
  if (EngineBehind(s) && !s.in_request) {
    auto pc = std::make_shared<AppState::PendingCommit>();
    pc->edit = e;
    s.pending_commit = pc;
    s.engine->Post([work, pc](Document& d) {
      pc->ok = work(d, &pc->error);
      pc->done = true;
    });
    e.active = false;
    if (!array) MoveActive(s, move);
    return;
  }
  const bool ok = s.engine->Call([&](Document& d) { return work(d, &error); });
  if (!ok) {
    // an entry the engine refused without a reason (a data table's cell) has its refusal's own message (Cannot change
    // part of a data table.), as Excel shows only that; a formula problem says what it is
    if (!error.empty()) {
      s.ShowError("YCGrid", TR("There's a problem with this formula.\n\n") + error + TR("\n\nNot trying to type a formula? Start with an apostrophe (') and the cell will be treated as text."));
    }
    e.focus_request = true;
    return;
  }
  // (typing isn't repeated by F4 / Ctrl+Y and doesn't end what they repeat: Excel 16, measured)
  e.active = false;
  if (!array) MoveActive(s, move);
}

// an entry handed to a busy engine (CommitEdit): when it was refused, its message and the editor come back
void ApplyPendingCommit(AppState& s) {
  if (!s.pending_commit) return;
  if (!s.pending_commit->done) {
    ui::RequestRedrawIn(1.0 / 30);
    return;
  }
  const std::shared_ptr<AppState::PendingCommit> pc = std::move(s.pending_commit);
  if (pc->ok) return;
  s.edit = pc->edit;
  s.edit.focus_request = true;
  if (pc->error.empty()) return;  // refused: the refusal says why (as above)
  if (!pc->hint) {
    s.ShowError("YCGrid", TR("There's a problem with this formula.\n\n") + pc->error);
    return;
  }
  s.ShowError("YCGrid", TR("There's a problem with this formula.\n\n") + pc->error +
                           TR("\n\nNot trying to type a formula? Start with an apostrophe (') and the cell will be treated as text."));
}

bool EngineBehind(const AppState& s) {
  if (!s.engine) return false;
  const auto now = s.engine->Current();
  return s.engine->BusySeconds() > 0 || (now && s.engine->UserActionsPending(*now));
}

bool DeferWhileEngineBusy(AppState& s, std::function<void(AppState&)> run, bool keep_selection) {
  if (!s.engine || s.in_request || s.running_deferred) return false;
  if (s.deferred.empty() && !EngineBehind(s)) return false;  // (behind one put off already: after it, in order)
  auto ready = std::make_shared<std::atomic<bool>>(false);
  s.engine->Post([ready](Document&) { ready->store(true, std::memory_order_release); });  // after all sent so far
  AppState::Deferred d;
  d.ready = std::move(ready);
  d.sheet = s.focus_sheet;
  d.sel = s.FocusView().sel;
  d.keep_selection = keep_selection;
  d.run = std::move(run);
  s.deferred.push_back(std::move(d));
  ui::RequestRedrawIn(1.0 / 30);
  return true;
}

void RunDeferred(AppState& s) {
  while (!s.deferred.empty() && s.deferred.front().ready->load(std::memory_order_acquire)) {
    AppState::Deferred d = std::move(s.deferred.front());
    s.deferred.erase(s.deferred.begin());
    if (d.sheet != s.focus_sheet) continue;  // the person went to another sheet meanwhile: nothing acts out of sight
    if (d.keep_selection) s.FocusView().sel = d.sel;
    s.running_deferred = true;
    d.run(s);
    s.running_deferred = false;
  }
  if (!s.deferred.empty()) ui::RequestRedrawIn(1.0 / 30);
}

bool UiWaitsForEngine(const AppState& s) { return !s.deferred.empty() || s.after_copy != nullptr; }

void CancelEdit(AppState& s) { s.edit.active = false; }

void SelectSpecial(AppState& s, int kind, int values) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  auto& view = s.FocusView();
  // within the selection, or the used range when one cell is selected (as Go To Special)
  Rect within = SelRect(s);
  // notes, conditional formats and validation can sit on empty cells, outside the used range: the whole sheet
  const auto sk = static_cast<engine::Special>(kind);
  const bool rules = sk == engine::Special::Comments || sk == engine::Special::AllFormatConditions || sk == engine::Special::AllValidation;
  if (within.IsCell() && rules) {
    within = Rect{0, 0, sh->row_limit - 1, sh->col_limit - 1};
  } else {
    if (within.IsCell() && sh->has_used) within = Rect{0, 0, sh->used.r1, sh->used.c1};
    if (sh->has_used) {
      within.r1 = std::min(within.r1, sh->used.r1);
      within.c1 = std::min(within.c1, sh->used.c1);
    }
  }
  std::vector<Rect> areas = engine::SpecialCells(*sh, within, sk, values);
  if (areas.empty()) {
    s.ShowMessage("YCGrid", TR("No cells were found."));
    return;
  }
  if (areas.size() > 10000) areas.resize(10000);
  std::rotate(areas.begin(), areas.begin() + 1, areas.end());  // the first area holds the active cell (the last one is active)
  view.sel.areas = areas;
  view.sel.anc_r = view.sel.act_r = areas.back().r0;
  view.sel.anc_c = view.sel.act_c = areas.back().c0;
  view.sel.mode = render::SelectMode::Cells;
  render::EnsureVisible(*sh, view, view.sel.act_r, view.sel.act_c);
}

void EditInsertText(AppState& s, const std::string& text) {
  auto& e = s.edit;
  if (!e.active) return;
  const int n = static_cast<int>(e.text.size());
  int a = std::clamp(e.caret, 0, n), b = a;
  if (e.sel_start != e.sel_end) {
    a = std::clamp(std::min(e.sel_start, e.sel_end), 0, n);
    b = std::clamp(std::max(e.sel_start, e.sel_end), 0, n);
  }
  std::string next = e.text;
  next.replace(static_cast<size_t>(a), static_cast<size_t>(b - a), text);
  render::SetEditText(e, std::move(next), a + static_cast<int>(text.size()));
  e.focus_request = true;
}

std::string TodayText() {
  int y = 0, m = 0, d = 0;
  formula::SerialToYmd(std::floor(formula::NowSerial()), &y, &m, &d);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d/%d/%d", y, m, d);
  return buf;
}

std::string NowTimeText() {
  int h = 0, mi = 0, se = 0, ms = 0;
  formula::SerialToHms(formula::NowSerial(), &h, &mi, &se, &ms);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d", h, mi);
  return buf;
}

void ArmCircularWarning(AppState& s) {
  if (!s.engine) return;
  const auto cur = s.engine->Current();
  s.circ_check = true;
  s.circ_after = cur ? cur->version : 0;
  s.circ_before = cur ? cur->circular : 0;
}

// Excel: "Circular References: B2" for one on the sheet shown, just "Circular References" when they are elsewhere
std::string CircularStatusText(const AppState& s) {
  if (!s.snap || !s.snap->circular) return {};
  std::string msg = TR("Circular References");
  for (const uint64_t k : s.snap->circular_cells) {
    if (engine::KeySheet(k) != s.focus_sheet) continue;
    msg += ": " + core::CellA1(engine::KeyRow(k), engine::KeyCol(k));
    break;
  }
  return msg;
}

// Excel: a formula that makes a circular reference (iterative calculation off) brings a message, once per workbook;
// the status bar then names the cell
void CheckCircularWarning(AppState& s) {
  if (!s.circ_check || !s.snap) return;
  if (s.snap->version <= s.circ_after || s.snap->calculating) {
    ui::RequestRedrawIn(1.0 / 30);
    return;
  }
  s.circ_check = false;
  if (s.circ_warned || Config().iterative || s.snap->circular <= s.circ_before) return;
  s.circ_warned = true;
  // clang-format off
  s.ShowMessage("YCGrid", TR("There are one or more circular references where a formula refers to its own cell either directly or indirectly. This might cause them to calculate incorrectly.\n\nTry removing or changing these references, or moving the formulas to different cells."));
  // clang-format on
}

namespace {

// F9 while editing a formula (Excel): the selected part, or the whole formula, is replaced by its value written as a
// constant (42, "text", TRUE, #N/A); Escape still throws the whole edit away
void EvaluateEditPart(AppState& s, int b, int en) {
  auto& e = s.edit;
  if (!e.active || !s.engine) return;
  // the engine still works: F9 acts once it has caught up, on the same text (typing on meanwhile drops it)
  if (DeferWhileEngineBusy(s, [b, en, text = e.text](AppState& st) {
        if (st.edit.active && st.edit.text == text) EvaluateEditPart(st, b, en);
      }, false)) {
    return;
  }
  const int n = static_cast<int>(e.text.size());
  b = std::clamp(b, 0, n);
  en = std::clamp(en, b, n);
  while (b < en && e.text[static_cast<size_t>(b)] == ' ') ++b;
  while (en > b && e.text[static_cast<size_t>(en) - 1] == ' ') --en;
  if (b >= en) return;
  const std::string part = AutoCloseParens(e.text.substr(static_cast<size_t>(b), static_cast<size_t>(en - b)));
  const uint32_t sheet = e.sheet;
  std::string err;
  const core::Value v = s.engine->Call([&](Document& d) { return d.EvaluateFormula(sheet, part, &err); });
  if (!err.empty()) {
    s.ShowError("YCGrid", TR("There's a problem with this formula.\n\n") + err);
    e.focus_request = true;
    return;
  }
  std::string lit;
  if (v.IsString()) {
    lit = "\"";
    for (const char ch : v.str()) {
      lit += ch;
      if (ch == '"') lit += '"';
    }
    lit += "\"";
  } else if (v.IsEmpty()) {
    lit = "0";
  } else {
    lit = engine::InputTextOf(v, "");
  }
  std::string next = e.text;
  next.replace(static_cast<size_t>(b), static_cast<size_t>(en - b), lit);
  render::SetEditText(e, std::move(next), b + static_cast<int>(lit.size()));
  e.ref_begin = e.ref_end = -1;
  e.focus_request = true;
}

}  // namespace

// ---- Clipboard ----

namespace {

std::wstring Wide(const std::string& u) { return FromUtf8(u); }

// a hidden instance (tests, agents) keeps a clipboard of its own: copying there never replaces what the user copied
std::string g_private_clip;
std::string g_private_image;  // a picture copied in a hidden instance (PNG)
DWORD g_private_seq = 1;

std::string ClipToTsv(const engine::Snapshot& snap, const engine::ClipData& clip);

// copied cells whose text isn't written yet: a large copy puts its text on the clipboard only when a program (or
// YCGrid's own paste of text, the Clipboard pane) reads it, as Excel does. Writing a million copied rows out at every
// Ctrl+C froze the window for over a second, though nothing ever read that text.
struct LazyClipText {
  std::shared_ptr<const engine::Snapshot> snap;  // what the cells showed when they were copied
  std::shared_ptr<const engine::ClipData> clip;
  HWND owner = nullptr;
  bool rendered = false;
  std::string text;
  const std::string& Text() {
    if (!rendered && snap && clip) text = ClipToTsv(*snap, *clip);
    rendered = true;
    return text;
  }
};
LazyClipText g_lazy;

DWORD ClipSeq() { return UserStateSaved() ? ::GetClipboardSequenceNumber() : g_private_seq; }

HGLOBAL GlobalText(const std::string& text) {
  const std::wstring w = Wide(text);
  HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (!h) return nullptr;
  void* p = ::GlobalLock(h);
  if (!p) {
    ::GlobalFree(h);
    return nullptr;
  }
  std::memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
  ::GlobalUnlock(h);
  return h;
}

void SetSystemClipboardText(HWND hwnd, const std::string& text) {
  if (!UserStateSaved()) {
    g_private_clip = text;
    g_private_image.clear();
    g_lazy = {};
    ++g_private_seq;
    return;
  }
  if (!::OpenClipboard(hwnd)) return;
  ::EmptyClipboard();  // tells the previous owner (perhaps this window, ClipboardReleased) that its copy is gone
  g_lazy = {};
  if (HGLOBAL h = GlobalText(text)) {
    if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
  }
  ::CloseClipboard();
}

// copied cells. A usual copy writes its text at once; one of more cells than this promises it and writes it when a
// program asks for it (WM_RENDERFORMAT, RenderClipboardText). A hidden instance's own clipboard always waits.
constexpr uint64_t kTextAtOnceCells = 250'000;

void SetSystemClipboardCopy(HWND hwnd, std::shared_ptr<const engine::Snapshot> snap, std::shared_ptr<const engine::ClipData> clip) {
  if (!snap || !clip) return;
  if (!UserStateSaved()) {
    g_private_clip.clear();
    g_private_image.clear();
    g_lazy = {};
    g_lazy.snap = std::move(snap), g_lazy.clip = std::move(clip);
    ++g_private_seq;
    return;
  }
  if (static_cast<uint64_t>(clip->rows) * clip->cols <= kTextAtOnceCells) {
    SetSystemClipboardText(hwnd, ClipToTsv(*snap, *clip));  // Windows' clipboard history (Win+V) keeps it
    return;
  }
  if (!::OpenClipboard(hwnd)) return;
  ::EmptyClipboard();
  g_lazy = {};
  g_lazy.snap = std::move(snap), g_lazy.clip = std::move(clip), g_lazy.owner = hwnd;
  ::SetClipboardData(CF_UNICODETEXT, nullptr);  // delayed rendering
  // too large for Windows' clipboard history (4 MB an item) anyway: kept out of it, or its service asks for the text
  // at once and the copy waits for it after all
  for (const wchar_t* f : {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
    if (HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
      if (void* p = ::GlobalLock(h)) {
        *static_cast<DWORD*>(p) = 0;
        ::GlobalUnlock(h);
      }
      if (!::SetClipboardData(::RegisterClipboardFormatW(f), h)) ::GlobalFree(h);
    }
  }
  ::CloseClipboard();
}

std::string GetSystemClipboardText(HWND hwnd) {
  if (!UserStateSaved()) return g_lazy.clip ? g_lazy.Text() : g_private_clip;
  // this window's own copy, still the clipboard's: its text without a round trip through Windows
  if (g_lazy.clip && g_lazy.owner && ::GetClipboardOwner() == g_lazy.owner) return g_lazy.Text();
  std::string out;
  if (!::OpenClipboard(hwnd)) return out;
  if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
      out = ToUtf8(p);
      ::GlobalUnlock(h);
    }
  }
  ::CloseClipboard();
  return out;
}

}  // namespace

void SetPrivateClipboardImage(const std::string& png) {
  g_private_image = png;
  g_private_clip.clear();
  g_lazy = {};
  ++g_private_seq;
}

void RenderClipboardText(AppState& s, bool all_formats) {
  if (!g_lazy.clip || !g_lazy.owner) return;
  if (all_formats) {
    // the window closes while its copy is still on the clipboard: write the text so other programs can paste it later
    if (!::OpenClipboard(g_lazy.owner)) return;
    if (::GetClipboardOwner() == g_lazy.owner) {
      if (HGLOBAL h = GlobalText(g_lazy.Text())) {
        if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
      }
    }
    ::CloseClipboard();
    return;
  }
  // a program pastes: it holds the clipboard open while this answers (no OpenClipboard here)
  const bool ours = s.clip_seq == ::GetClipboardSequenceNumber();
  if (HGLOBAL h = GlobalText(g_lazy.Text())) {
    if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
  }
  if (ours) s.clip_seq = ::GetClipboardSequenceNumber();  // writing the promised text keeps the copy mode
}

void ClipboardReleased() { g_lazy = {}; }

// a picture on the clipboard (a screenshot, a picture copied in a browser or Office): "PNG", else CF_DIB as a PNG
bool ClipboardImage(HWND hwnd, std::string* png) {
  if (!UserStateSaved()) {
    *png = g_private_image;
    return !png->empty();
  }
  bool ok = false;
  if (!::OpenClipboard(hwnd)) return false;
  if (HANDLE h = ::GetClipboardData(::RegisterClipboardFormatW(L"PNG"))) {
    if (const void* p = ::GlobalLock(h)) {
      png->assign(static_cast<const char*>(p), ::GlobalSize(h));
      ::GlobalUnlock(h);
      ok = !png->empty();
    }
  }
  if (!ok) {
    if (HANDLE h = ::GetClipboardData(CF_DIB)) {
      if (const auto* bih = static_cast<const BITMAPINFOHEADER*>(::GlobalLock(h))) {
        const int w = bih->biWidth, hh = std::abs(bih->biHeight), bpp = bih->biBitCount;
        if (w > 0 && hh > 0 && (bpp == 24 || bpp == 32) && (bih->biCompression == BI_RGB || bih->biCompression == BI_BITFIELDS) &&
            static_cast<int64_t>(w) * hh < 50'000'000) {
          const BYTE* px = reinterpret_cast<const BYTE*>(bih) + bih->biSize + (bih->biCompression == BI_BITFIELDS ? 12 : 0) + bih->biClrUsed * 4;
          const size_t stride = ((static_cast<size_t>(w) * bpp + 31) / 32) * 4;
          Image img;
          img.w = w, img.h = hh;
          img.rgba.resize(static_cast<size_t>(w) * hh * 4);
          for (int y = 0; y < hh; ++y) {
            const BYTE* row = px + stride * static_cast<size_t>(bih->biHeight > 0 ? hh - 1 - y : y);
            for (int x = 0; x < w; ++x) {
              uint8_t* d = &img.rgba[(static_cast<size_t>(y) * w + x) * 4];
              d[0] = row[x * (bpp / 8) + 2], d[1] = row[x * (bpp / 8) + 1], d[2] = row[x * (bpp / 8)], d[3] = 255;
            }
          }
          ok = EncodePng(img, png);
        }
        ::GlobalUnlock(h);
      }
    }
  }
  ::CloseClipboard();
  return ok;
}

namespace {

// TSV (Excel's plain-text clipboard format): fields with tabs, line breaks or quotes are wrapped in double quotes
std::string ClipToTsv(const engine::Snapshot& snap, const engine::ClipData& clip) {
  std::string out;
  for (uint32_t i = 0; i < clip.rows; ++i) {
    for (uint32_t j = 0; j < clip.cols; ++j) {
      if (j) out += '\t';
      const auto& cc = clip.cells[static_cast<size_t>(i) * clip.cols + j];
      std::string t = render::FormatCellText(snap, cc.style, cc.value, -1, nullptr, nullptr, nullptr);
      if (cc.value.IsDecimal()) t = core::DecToString(core::DecFromValue(cc.value));  // all digits (PLAN §5.2a)
      if (t.find_first_of("\t\r\n\"") != std::string::npos) {
        std::string q = "\"";
        for (char ch : t) {
          if (ch == '"') q += '"';
          q += ch;
        }
        t = q + "\"";
      }
      out += t;
    }
    out += "\r\n";
  }
  return out;
}

std::vector<std::vector<std::string>> ParseTsv(const std::string& s) {
  std::vector<std::vector<std::string>> rows(1);
  std::string cell;
  bool quoted = false;
  // a quote opens a quoted field only when it is closed later; otherwise it is text (it used to swallow the rest)
  auto closes = [&](size_t from) {
    for (size_t j = from + 1; j < s.size(); ++j) {
      if (s[j] != '"') continue;
      if (j + 1 < s.size() && s[j + 1] == '"') {
        ++j;
        continue;
      }
      return true;
    }
    return false;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const char ch = s[i];
    if (quoted) {
      if (ch == '"') {
        if (i + 1 < s.size() && s[i + 1] == '"') {
          cell += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else {
        cell += ch;
      }
      continue;
    }
    if (ch == '"' && cell.empty() && closes(i)) quoted = true;
    else if (ch == '\t') {
      rows.back().push_back(cell);
      cell.clear();
    } else if (ch == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
    } else if (ch == '\n' || ch == '\r') {  // LF, CRLF or a lone CR (old Mac text) ends the line
      rows.back().push_back(cell);
      cell.clear();
      rows.emplace_back();
    } else {
      cell += ch;
    }
  }
  if (!cell.empty() || !rows.back().empty()) rows.back().push_back(cell);
  while (!rows.empty() && rows.back().empty()) rows.pop_back();
  return rows;
}

}  // namespace

void SetClipboardTextUtf8(HWND hwnd, const std::string& text) { SetSystemClipboardText(hwnd, text); }
std::string ClipboardTextUtf8(HWND hwnd) { return GetSystemClipboardText(hwnd); }
bool ClipboardHasText(HWND hwnd) {
  // copied cells always give text (each row ends with a line break): no need to write it out to know
  if (g_lazy.clip && g_lazy.clip->rows > 0 && (!UserStateSaved() || (g_lazy.owner && ::GetClipboardOwner() == g_lazy.owner))) return true;
  return !GetSystemClipboardText(hwnd).empty();
}

bool CopyModeOn(const AppState& s) { return s.marquee && s.has_clip && ClipSeq() == s.clip_seq; }

void EndCopyMode(AppState& s) { s.marquee = false; }

uint32_t ClipboardSequence() { return static_cast<uint32_t>(ClipSeq()); }

uint32_t ClaimClipboard(AppState& s) {
  SetSystemClipboardText(s.hwnd, "");
  s.has_clip = false;
  s.marquee = false;
  return static_cast<uint32_t>(ClipSeq());
}

namespace {

// the copied cells become the clipboard's: outlined, and on Windows' clipboard (or the hidden instance's own)
void FinishCopy(AppState& s, engine::ClipData copied) {
  if (copied.rows == 0 || copied.cols == 0) {  // more than 20 million cells: the engine refused it
    s.has_clip = false;
    s.marquee = false;
    s.ShowError("YCGrid", TR("The selection is too large to copy (at most 20,000,000 cells)."));
    return;
  }
  s.clip = std::make_shared<const engine::ClipData>(std::move(copied));
  // the text is written when something reads it; the Clipboard pane collects it while it is shown (misc_ui.cpp)
  SetSystemClipboardCopy(s.hwnd, s.snap, s.clip);
  s.clip_seq = ClipSeq();
}

}  // namespace

void ApplyPendingCopy(AppState& s) {
  if (!s.pending_copy) return;
  if (!s.pending_copy->done.load(std::memory_order_acquire)) {
    ui::RequestRedrawIn(1.0 / 30);
    return;
  }
  const std::shared_ptr<AppState::PendingCopy> pc = std::move(s.pending_copy);
  s.pending_copy = nullptr;
  // something else took the clipboard meanwhile (a copy in another program): that copy came last and stays
  if (ClipSeq() != s.clip_seq) {
    s.clip = std::make_shared<const engine::ClipData>(std::move(pc->data));
    s.after_copy = nullptr;
    return;
  }
  FinishCopy(s, std::move(pc->data));
  if (std::function<void(AppState&)> paste = std::move(s.after_copy)) {
    s.after_copy = nullptr;
    if (CopyModeOn(s)) paste(s);
  }
}

void WaitForPendingCopy(AppState& s) {
  if (!s.pending_copy) return;
  if (s.engine) s.engine->Sync();  // the copy was posted before: it is done once everything sent so far is
  ApplyPendingCopy(s);
}

bool PasteAfterCopy(AppState& s, std::function<void(AppState&)> paste) {
  if (s.pending_copy && s.pending_copy->done.load(std::memory_order_acquire)) ApplyPendingCopy(s);
  if (!s.pending_copy) return false;
  // Excel pastes where the selection was when the key was pressed, on that sheet
  const uint32_t sheet = s.focus_sheet;
  const render::Selection sel = s.FocusView().sel;
  s.after_copy = [sheet, sel, paste = std::move(paste)](AppState& st) {
    if (st.focus_sheet != sheet) return;  // the person went to another sheet meanwhile: no paste lands out of sight
    st.FocusView().sel = sel;
    paste(st);
  };
  return true;
}

void CopySelection(AppState& s, bool cut, bool wait) {
  if (!s.engine || !s.snap) return;
  // a copy still with the engine and a paste waiting for it: the paste takes that copy, as it was asked first
  if (s.after_copy) WaitForPendingCopy(s);
  auto areas = SelAreas(s);
  // Excel copies a multiple selection whose areas share the same rows (or the same columns): side by side as one block
  bool same_rows = areas.size() > 1, same_cols = areas.size() > 1;
  for (const Rect& a : areas) {
    same_rows = same_rows && a.r0 == areas[0].r0 && a.r1 == areas[0].r1;
    same_cols = same_cols && a.c0 == areas[0].c0 && a.c1 == areas[0].c1;
  }
  if (areas.size() != 1 && (cut || !(same_rows || same_cols))) {
    s.ShowError("YCGrid", TR("This command can't be used on multiple selections."));
    return;
  }
  const uint32_t sheet = s.focus_sheet;
  std::function<engine::ClipData(Document&)> make;
  if (areas.size() > 1) {
    std::sort(areas.begin(), areas.end(), [&](const Rect& x, const Rect& y) { return same_rows ? x.c0 < y.c0 : x.r0 < y.r0; });
    make = [areas, sheet, same_rows, same_cols](Document& d) {
      engine::ClipData out;
      for (const Rect& a : areas) {
        engine::ClipData part = d.CopyShown(sheet, a);
        if (out.rows == 0) {
          out = std::move(part);
          continue;
        }
        if (same_rows && part.rows == out.rows) {  // columns side by side
          std::vector<engine::ClipCell> cells;
          cells.reserve(static_cast<size_t>(out.rows) * (out.cols + part.cols));
          for (uint32_t i = 0; i < out.rows; ++i) {
            cells.insert(cells.end(), out.cells.begin() + static_cast<ptrdiff_t>(i) * out.cols, out.cells.begin() + static_cast<ptrdiff_t>(i + 1) * out.cols);
            cells.insert(cells.end(), part.cells.begin() + static_cast<ptrdiff_t>(i) * part.cols, part.cells.begin() + static_cast<ptrdiff_t>(i + 1) * part.cols);
          }
          out.cells = std::move(cells);
          const uint32_t wide = out.cols + part.cols;  // the hyperlinks keep their cells
          for (auto& [idx, l] : out.links) idx = idx / out.cols * wide + idx % out.cols;
          for (auto& [idx, l] : part.links) out.links.emplace_back(idx / part.cols * wide + out.cols + idx % part.cols, std::move(l));
          out.cols += part.cols;
          out.col_widths.insert(out.col_widths.end(), part.col_widths.begin(), part.col_widths.end());
          out.merges.clear();
          out.rich.clear();
        } else if (same_cols && part.cols == out.cols) {  // rows one under the other
          out.cells.insert(out.cells.end(), part.cells.begin(), part.cells.end());
          for (auto& [idx, l] : part.links) out.links.emplace_back(static_cast<size_t>(out.rows) * out.cols + idx, std::move(l));
          out.rows += part.rows;
          out.merges.clear();
          out.rich.clear();
        }
      }
      out.src = Rect{areas.front().r0, areas.front().c0, areas.back().r1, areas.back().c1};
      return out;
    };
  } else {
    const Rect a = areas[0];
    // a filtered list: the rows it shows (Excel); a cut moves all of them
    make = [a, sheet, cut](Document& d) {
      engine::ClipData out = cut ? d.Copy(sheet, a) : d.CopyShown(sheet, a);
      out.carry_notes = true;
      return out;
    };
  }
  const Rect r = areas.size() == 1 ? areas[0] : Rect{areas.front().r0, areas.front().c0, areas.back().r1, areas.back().c1};
  s.has_clip = true;
  s.marquee = true;
  s.marquee_rect = r;
  s.marquee_sheet = sheet;
  s.status_until = 0.0;  // the status bar shows the paste hint while the copied cells are outlined (main.cpp)
  // the engine is still working on what came before (a million formulas filled, an Undo): the copy waits in its queue,
  // behind that work, and the screen goes on (Ctrl+C waited up to 2.4 s in a frame). The copied cells are outlined at
  // once; until they are ready `clip` says only where they came from, and a paste waits for them (PasteAfterCopy).
  if (!wait && !s.in_request && EngineBehind(s)) {
    auto pc = std::make_shared<AppState::PendingCopy>();
    s.engine->Post([pc, make, cut](Document& d) {
      pc->data = make(d);
      pc->data.cut = cut;
      pc->done.store(true, std::memory_order_release);
    });
    auto where = std::make_shared<engine::ClipData>();
    where->src = r, where->src_sheet = sheet, where->cut = cut;
    s.clip = std::move(where);
    s.pending_copy = std::move(pc);
    s.after_copy = nullptr;
    s.clip_seq = ClipSeq();
    ui::RequestRedrawIn(1.0 / 30);
    return;
  }
  s.pending_copy = nullptr;
  s.after_copy = nullptr;
  engine::ClipData copied = s.engine->Call([&](Document& d) { return make(d); });
  copied.cut = cut;
  FinishCopy(s, std::move(copied));
}

void PasteLink(AppState& s) {
  if (!s.engine || !s.snap || !CopyModeOn(s)) return;
  const auto areas = SelAreas(s);
  if (areas.empty()) return;
  const uint32_t sheet = s.focus_sheet;
  const Rect dst = areas.back();
  const engine::ClipData& clip = *s.clip;
  const model::Sheet* src = s.snap->Sheet(clip.src_sheet);
  if (!src) return;
  // Excel: one cell links absolutely (=$A$1), a block relatively (=A1, =A2...); another sheet's name comes first
  const std::string prefix = clip.src_sheet == sheet ? std::string() : core::QuoteSheetName(src->name) + "!";
  const bool one = clip.src.r0 == clip.src.r1 && clip.src.c0 == clip.src.c1;
  const Rect from = clip.src;
  s.engine->PostUser(TR("Paste Link"), [=](Document& d) {
    for (uint32_t i = 0; i <= from.r1 - from.r0; ++i) {
      for (uint32_t j = 0; j <= from.c1 - from.c0; ++j) d.SetFormula(sheet, dst.r0 + i, dst.c0 + j, prefix + core::CellA1(from.r0 + i, from.c0 + j, one, one));
    }
  });
  render::SelectRange(s.FocusView(), Rect{dst.r0, dst.c0, dst.r0 + (from.r1 - from.r0), dst.c0 + (from.c1 - from.c0)});
}

// File > Options > Advanced > "Alert the user when a potentially time consuming operation occurs"
// (Application.EnableLargeOperationAlert, LargeOperationCellThousandCount): a person's paste or Fill Down / Right over more cells
// than the count asks first, and runs again once it is answered Yes; a script's (inside a request) is never asked
bool AskBeforeLargeOperation(AppState& s, uint64_t cells, std::function<void()> again) {
  static bool answered = false;
  if (answered) {
    answered = false;
    return false;
  }
  const Settings& c = Config();
  if (!c.large_operation_alert || s.in_request || cells <= static_cast<uint64_t>(c.large_operation_thousands) * 1000u) return false;
  AskYesNo(s, "YCGrid",
           TR("This operation will affect a large number of cells and may take a significant amount of time to complete. Are you sure you want to continue?"),
           [again = std::move(again)]() {
             answered = true;
             again();
           });
  return true;
}

namespace {
// the cells of the selection's areas, each clamped to the sheet
uint64_t SelectionCells(AppState& s) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(s.focus_sheet) : nullptr;
  uint64_t n = 0;
  for (Rect a : SelAreas(s)) {
    if (sh) a.r1 = std::min<uint32_t>(a.r1, static_cast<uint32_t>(sh->row_limit - 1)), a.c1 = std::min<uint32_t>(a.c1, static_cast<uint32_t>(sh->col_limit - 1));
    n += static_cast<uint64_t>(a.r1 - a.r0 + 1) * (a.c1 - a.c0 + 1);
  }
  return n;
}
}  // namespace

void PasteClipboard(AppState& s, engine::PasteMode mode, bool transpose, bool skip_blanks, engine::PasteOp op) {
  if (!s.engine || !s.snap) return;
  if (PasteAfterCopy(s, [mode, transpose, skip_blanks, op](AppState& st) { PasteClipboard(st, mode, transpose, skip_blanks, op); })) return;
  {
    // the cells the paste reaches: the selection, or the copied block at one cell
    uint64_t cells = SelectionCells(s);
    if (CopyModeOn(s) && s.clip) cells = std::max<uint64_t>(cells, static_cast<uint64_t>(s.clip->rows) * s.clip->cols);
    if (AskBeforeLargeOperation(s, cells, [&s, mode, transpose, skip_blanks, op]() { PasteClipboard(s, mode, transpose, skip_blanks, op); })) return;
  }
  const auto areas = SelAreas(s);
  if (areas.empty()) return;
  const uint32_t sheet = s.focus_sheet;
  const Rect dst = areas.back();
  // copied cells paste while they are outlined; after Esc (or typing, an insert...) there is nothing of them to paste
  if (s.has_clip && ClipSeq() == s.clip_seq && !CopyModeOn(s)) return;
  if (CopyModeOn(s)) {
    const std::shared_ptr<const engine::ClipData> held = s.clip;  // (shared: the step below and Paste Options hold it)
    const engine::ClipData& clip = *held;
    const uint32_t rows = transpose ? clip.cols : clip.rows, cols = transpose ? clip.rows : clip.cols;
    // paste repeatedly where the target is a whole multiple of the clipboard's size, each direction on its own (Excel:
    // a column of two pasted on I1:J1 fills I1:J2, two copies side by side)
    uint32_t tile_r = 1, tile_c = 1;
    if (rows && cols && dst.Count() < 1000000) {
      if (dst.Rows() % rows == 0) tile_r = static_cast<uint32_t>(dst.Rows() / rows);
      if (dst.Cols() % cols == 0) tile_c = static_cast<uint32_t>(dst.Cols() / cols);
    }
    // the copy area runs past the sheet's last row or column from here: Excel refuses (measured), YCGrid pasted the
    // used part and selected cells past XFD (basics seq 268656)
    if (engine::PasteRunsOffSheet(clip, dst.r0, dst.c0, transpose)) {
      s.ShowError("YCGrid", TR("We can't paste here because the Copy area and paste area aren't the same size.\n\nSelect just one cell in the "
                               "paste area or an area that's the same size, and try pasting again."));
      return;
    }
    // a merged cell cut by the paste's edge: Excel refuses ("We can't do that to a merged cell."), YCGrid split it
    if (const model::Sheet* dsh = s.snap->Sheet(sheet)) {
      const Rect out{dst.r0, dst.c0, dst.r0 + rows * tile_r - 1, dst.c0 + cols * tile_c - 1};
      for (const Rect& m : *dsh->merges) {
        if (m.Intersects(out) && !(m.r0 >= out.r0 && m.c0 >= out.c0 && m.r1 <= out.r1 && m.c1 <= out.c1)) {
          s.ShowError("YCGrid", TR("We can't do that to a merged cell."));
          return;
        }
      }
    }
    if (clip.cut && mode == engine::PasteMode::All && !transpose && !skip_blanks && op == engine::PasteOp::None) {
      const uint32_t src_sheet = clip.src_sheet;
      const Rect src = clip.src;
      s.engine->PostUser(TR("Cut"), [=](Document& d) { d.MoveCells(src_sheet, src, sheet, dst.r0, dst.c0); });
      render::SelectRange(s.FocusView(), Rect{dst.r0, dst.c0, dst.r0 + rows - 1, dst.c0 + cols - 1});
      // the cut cells are used up: has_clip stays, so a second Ctrl+V pastes nothing, as in Excel (it pasted the
      // copied text from the system clipboard again, formulas as values)
      s.marquee = false;
      return;
    }
    const std::vector<uint32_t> group = clip.cut ? std::vector<uint32_t>{sheet} : GroupTargets(s, sheet);  // grouped sheets (Excel)
    s.engine->PostUser(clip.cut ? TR("Cut") : TR("Paste"), [=](Document& d) {
      for (const uint32_t g : group) d.PasteRepeated(g, dst.r0, dst.c0, tile_r, tile_c, *held, mode, transpose, skip_blanks, op);
    });
    auto& v = s.FocusView();
    render::SelectRange(v, Rect{dst.r0, dst.c0, dst.r0 + rows * tile_r - 1, dst.c0 + cols * tile_c - 1});
    // the Paste Options button: the same cells can be pasted again another way (values, formulas, transposed...)
    if (!clip.cut && mode == engine::PasteMode::All && !transpose && !skip_blanks && op == engine::PasteOp::None && tile_r == 1 && tile_c == 1) {
      s.last_paste = AppState::LastPaste{};
      s.last_paste.active = true;
      s.last_paste.sheet = sheet;
      s.last_paste.dest = Rect{dst.r0, dst.c0, dst.r0 + rows - 1, dst.c0 + cols - 1};
      s.last_paste.clip = held;
    }
    if (clip.cut) s.marquee = false;  // used up (see above)
    return;
  }
  // external source: plain text (TSV); without text, a picture goes on the sheet at the active cell (Excel)
  const std::string text = GetSystemClipboardText(s.hwnd);
  if (text.empty() && mode == engine::PasteMode::All) {
    std::string png;
    if (ClipboardImage(s.hwnd, &png)) InsertPictureBytes(s, std::move(png));
    return;
  }
  if (text.empty()) return;
  const auto rows = ParseTsv(text);
  if (rows.empty()) return;
  size_t ncols = 0;
  for (const auto& r : rows) ncols = std::max(ncols, r.size());
  s.engine->PostUser(TR("Paste"), [=](Document& d) {
    for (size_t i = 0; i < rows.size(); ++i) {
      for (size_t j = 0; j < rows[i].size(); ++j) {
        if (rows[i][j].empty()) {  // an empty field empties its cell (the format stays), as in Excel
          const uint32_t er = dst.r0 + static_cast<uint32_t>(i), ec = dst.c0 + static_cast<uint32_t>(j);
          if (!d.GetValue(sheet, er, ec).IsEmpty() || (d.S(sheet) && d.S(sheet)->GetFormula(er, ec))) d.ClearContents(sheet, Rect::Cell(er, ec));
          continue;
        }
        d.SetInput(sheet, dst.r0 + static_cast<uint32_t>(i), dst.c0 + static_cast<uint32_t>(j), rows[i][j]);
      }
    }
  });
  render::SelectRange(s.FocusView(), Rect{dst.r0, dst.c0, dst.r0 + static_cast<uint32_t>(rows.size()) - 1, dst.c0 + static_cast<uint32_t>(std::max<size_t>(1, ncols)) - 1});
  s.marquee = false;
}

void ClearSelection(AppState& s, int what) {
  if (!s.engine) return;
  EndCopyMode(s);  // Excel: Delete ends cut / copy mode
  auto areas = SelAreas(s);
  const uint32_t sheet = s.focus_sheet;
  if (what != 1 && RefusePartOfArray(s, sheet, areas)) return;
  // a filtered list: only the rows it shows are cleared (Excel)
  if (const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr) areas = engine::FilterShownRuns(*sh, areas);
  const std::vector<uint32_t> group = GroupTargets(s, sheet);  // grouped sheets are cleared too (Excel)
  const auto fit = what == 1 ? std::vector<std::pair<uint32_t, int32_t>>{} : FitRowsAfterClear(s, sheet, areas, what == 2);
  s.engine->PostUser(what == 0 ? TR("Clear Contents") : (what == 1 ? TR("Clear Formats") : TR("Clear All")), [=](Document& d) {
    for (const uint32_t g : group) {
      for (const Rect& a : areas) {
        if (what != 1) d.ClearTablesIn(g, a);  // a whole table cleared goes away (Excel)
        if (what == 0) d.ClearContents(g, a);
        else if (what == 1) d.ClearFormats(g, a);
        else d.ClearAll(g, a, engine::kSrcUser, /*everything=*/true);
      }
    }
    for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
  });
}

namespace {

// Insert Copied Cells / Insert Cut Cells: room is made at the selection for the copied cells (whole rows, whole columns,
// or cells shifted right / down), then they are pasted there, or moved there and their old place closed up, in one step
void InsertCellsAt(AppState& s, int what, const Rect& at, uint32_t src_sheet, const Rect& src, bool cut, std::shared_ptr<const engine::ClipData> clip) {
  const uint32_t sheet = s.focus_sheet;
  const uint32_t n_rows = static_cast<uint32_t>(src.Rows()), n_cols = static_cast<uint32_t>(src.Cols());
  Rect dest;
  if (what == 1) dest = Rect{at.r0, src.c0, at.r0 + n_rows - 1, src.c1};
  else if (what == 2) dest = Rect{src.r0, at.c0, src.r1, at.c0 + n_cols - 1};
  else dest = Rect{at.r0, at.c0, at.r0 + n_rows - 1, at.c0 + n_cols - 1};
  s.engine->PostUser(cut ? TR("Insert Cut Cells") : TR("Insert Copied Cells"), [=](Document& d) {
    Rect from = src;  // where the source is after the insert
    const bool same = src_sheet == sheet;
    if (what == 1) {
      d.InsertRows(sheet, at.r0, n_rows);
      if (same && from.r0 >= at.r0) from.r0 += n_rows, from.r1 += n_rows;
    } else if (what == 2) {
      d.InsertCols(sheet, at.c0, n_cols);
      if (same && from.c0 >= at.c0) from.c0 += n_cols, from.c1 += n_cols;
    } else {
      d.InsertCells(sheet, dest, what == 3);
      if (same && what == 3 && from.c0 >= dest.c0 && from.c1 <= dest.c1 && from.r0 >= dest.r0) from.r0 += n_rows, from.r1 += n_rows;
      if (same && what == 4 && from.r0 >= dest.r0 && from.r1 <= dest.r1 && from.c0 >= dest.c0) from.c0 += n_cols, from.c1 += n_cols;
    }
    if (!cut) {
      d.Paste(sheet, dest.r0, dest.c0, *clip);
      return;
    }
    d.MoveCells(src_sheet, from, sheet, dest.r0, dest.c0);
    // the cut cells' old place closes up (Excel moves them: no empty row / column is left behind)
    if (what == 1) d.DeleteRows(src_sheet, from.r0, n_rows);
    else if (what == 2) d.DeleteCols(src_sheet, from.c0, n_cols);
    else d.DeleteCells(src_sheet, from, what == 3);
  });
  // the inserted cells end up where they were inserted (a source above them closed up first)
  Rect sel = dest;
  if (cut && src_sheet == sheet) {
    if (what == 1 && src.r0 < at.r0) sel.r0 -= n_rows, sel.r1 -= n_rows;
    if (what == 2 && src.c0 < at.c0) sel.c0 -= n_cols, sel.c1 -= n_cols;
  }
  if (cut && src_sheet == sheet && what == 3 && src.c0 == dest.c0 && src.c1 == dest.c1 && src.r0 < at.r0) sel.r0 -= n_rows, sel.r1 -= n_rows;
  if (cut && src_sheet == sheet && what == 4 && src.r0 == dest.r0 && src.r1 == dest.r1 && src.c0 < at.c0) sel.c0 -= n_cols, sel.c1 -= n_cols;
  if (what == 1) sel.c0 = 0, sel.c1 = s.FocusSheet() ? s.FocusSheet()->col_limit - 1 : sel.c1;
  if (what == 2) sel.r0 = 0, sel.r1 = s.FocusSheet() ? s.FocusSheet()->row_limit - 1 : sel.r1;
  render::SelectRange(s.FocusView(), sel);
  if (what == 1) s.FocusView().sel.mode = render::SelectMode::Rows;
  if (what == 2) s.FocusView().sel.mode = render::SelectMode::Cols;
}

void InsertClip(AppState& s, int what, const Rect& at) {
  if (PasteAfterCopy(s, [what, at](AppState& st) { InsertClip(st, what, at); })) return;
  const std::shared_ptr<const engine::ClipData> held = s.clip;
  InsertCellsAt(s, what, at, s.marquee_sheet, s.marquee_rect, held->cut, held);
  if (held->cut) EndCopyMode(s);
}

}  // namespace

void InsertDrop(AppState& s, uint32_t sheet, const Rect& src, int dir, uint32_t r, uint32_t c, bool copy) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || !s.engine) return;
  const bool full_rows = src.c0 == 0 && src.c1 >= sh->col_limit - 1, full_cols = src.r0 == 0 && src.r1 >= sh->row_limit - 1;
  const int what = full_rows ? 1 : full_cols ? 2 : dir == 1 ? 3 : 4;
  // inserted right where the cells are (or just after them): nothing moves
  if (!copy && ((what == 1 || what == 3) ? (r >= src.r0 && r <= src.r1 + 1 && (what == 1 || c == src.c0))
                                         : (c >= src.c0 && c <= src.c1 + 1 && (what == 2 || r == src.r0)))) {
    return;
  }
  engine::ClipData clip;
  if (copy) clip = s.engine->Call([&](Document& d) { return d.Copy(sheet, src); });
  clip.cut = !copy;
  s.focus_sheet = sheet;
  InsertCellsAt(s, what, Rect{r, c, r, c}, sheet, src, !copy, std::make_shared<const engine::ClipData>(std::move(clip)));
}

bool InsertCopiedAt(AppState& s, const Rect& at, int what) {
  // Python's Range.Insert while cells are copied or cut: Insert Copied Cells / Insert Cut Cells there (Excel)
  if (!CopyModeOn(s)) return false;
  InsertClip(s, what, at);
  return true;
}

const model::Table* SelectionTable(AppState& s, Rect* area) {
  const model::Sheet* sh = s.FocusSheet();
  const auto& sel = s.FocusView().sel;
  if (!sh || sh->tables->empty() || sel.areas.size() != 1 || sel.mode != render::SelectMode::Cells) return nullptr;
  const Rect a = sel.areas.back();
  for (const model::Table& t : *sh->tables) {
    if (t.range.Contains(a.r0, a.c0) && t.range.Contains(a.r1, a.c1)) {
      if (area) *area = a;
      return &t;
    }
  }
  return nullptr;
}

bool TableRowsCols(AppState& s, bool insert, int what) {
  Rect a{};
  const model::Table* t = SelectionTable(s, &a);
  if (!t || !s.engine) return false;
  if (RefuseReadOnlyUi(s)) return true;
  s.repeat_command.clear();
  EndCopyMode(s);  // Excel: an insert or delete ends cut / copy mode
  s.last_insert = AppState::LastInsert{};
  const uint32_t sheet = s.focus_sheet, id = t->id;
  std::vector<uint32_t> lines;  // the table's data rows or columns the selection takes, as indexes
  if (what == 1) {
    const Rect d = t->Data();
    for (uint32_t r = std::max(a.r0, d.r0); d.r0 <= d.r1 && r <= std::min(a.r1, d.r1); ++r) lines.push_back(r - d.r0);
  } else {
    for (uint32_t c = a.c0; c <= a.c1; ++c) lines.push_back(c - t->range.c0);
  }
  if (lines.empty()) return true;  // the header or totals row alone: nothing (Excel 16, measured)
  const char* label = what == 1 ? (insert ? TR("Insert Table Rows") : TR("Delete Table Rows"))
                                : (insert ? TR("Insert Table Columns") : TR("Delete Table Columns"));
  s.engine->PostUser(label, [=](Document& d) {
    if (insert) {
      // as many lines as are selected, above (left of) the first one
      for (size_t i = 0; i < lines.size(); ++i) {
        if (!(what == 1 ? d.TableInsertRow(sheet, id, lines.front()) : d.TableInsertColumn(sheet, id, lines.front()))) break;
      }
    } else {
      for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (!(what == 1 ? d.TableDeleteRow(sheet, id, *it) : d.TableDeleteColumn(sheet, id, *it))) break;
      }
    }
  });
  return true;
}

void InsertOrDelete(AppState& s, bool insert, int what) {
  if (!s.engine) return;
  s.repeat_command.clear();
  const auto areas = SelAreas(s);
  if (areas.empty()) return;
  const auto mode = s.FocusView().sel.mode;
  const model::Sheet* sh = s.FocusSheet();
  // in a table Ctrl+- and Ctrl+Shift+= insert and delete its rows or columns without a dialog: columns when the
  // selection is taller than wide, else rows; deleting across every column deletes rows, and the whole table its
  // cells (Excel 16, measured by keys posted to its sheet window)
  Rect ta{};
  if (what == 0 && !(insert && CopyModeOn(s))) {
    if (const model::Table* t = SelectionTable(s, &ta)) {
      if (!insert && ta == t->range) {
        what = 4;
      } else {
        const bool all_cols = ta.c0 == t->range.c0 && ta.c1 == t->range.c1;
        TableRowsCols(s, insert, ta.Rows() > ta.Cols() && (insert || !all_cols) ? 2 : 1);
        return;
      }
    }
  }
  // whole rows / columns selected any way (header, Name Box, Go To, Python) insert and delete rows / columns
  const Rect sa = areas.back();
  const bool all_rows = sh && sa.r0 == 0 && sa.r1 >= sh->row_limit - 1, all_cols = sh && sa.c0 == 0 && sa.c1 >= sh->col_limit - 1;
  // cells copied or cut (outlined): Insert becomes Insert Copied Cells / Insert Cut Cells
  const bool paste_in = insert && CopyModeOn(s) && sh;
  const model::Sheet* src_sh = paste_in && s.snap ? s.snap->Sheet(s.marquee_sheet) : nullptr;
  const bool clip_rows = src_sh && s.marquee_rect.c0 == 0 && s.marquee_rect.c1 >= src_sh->col_limit - 1;
  const bool clip_cols = src_sh && s.marquee_rect.r0 == 0 && s.marquee_rect.r1 >= src_sh->row_limit - 1;
  if (what == 0) {
    if (paste_in && clip_rows) what = 1;
    else if (paste_in && clip_cols) what = 2;
    else if (paste_in) {
      s.OpenDialog(Dialog::InsertCells);  // Insert Paste: shift the cells right or down
      return;
    } else if (mode == render::SelectMode::Rows || (all_cols && !all_rows)) what = 1;
    else if (mode == render::SelectMode::Cols || (all_rows && !all_cols)) what = 2;
    else {
      s.OpenDialog(insert ? Dialog::InsertCells : Dialog::DeleteCells);
      return;
    }
  }
  // F4 repeats what this did: whole rows / columns again, not Insert... with its dialog on a single cell (Excel)
  if (what == 1 || what == 2) s.repeat_command = std::string(insert ? "Insert" : "Delete") + (what == 1 ? "Rows" : "Columns");
  const uint32_t sheet = s.focus_sheet;
  const Rect a = areas.back();
  if (paste_in && ((what == 1 && clip_rows) || (what == 2 && clip_cols) || ((what == 3 || what == 4) && !clip_rows && !clip_cols))) {
    InsertClip(s, what, a);
    return;
  }
  EndCopyMode(s);  // Excel: an insert or delete ends cut / copy mode
  s.last_insert = AppState::LastInsert{};
  if (insert) {  // the Insert Options button: the inserted cells can take their formats from the other side, or none
    s.last_insert.active = true;
    s.last_insert.sheet = sheet;
    s.last_insert.what = what;
    s.last_insert.rect = a;
    s.last_insert.sel = SelRect(s);
  }
  const char* label = insert ? TR("Insert") : TR("Delete");
  // a multiple selection: every area (Excel), the last ones first so the others stay where they are; rows / columns
  // that overlap are one span
  std::vector<Rect> order = areas;
  if (what == 1 || what == 2) {
    std::sort(order.begin(), order.end(), [&](const Rect& x, const Rect& y) { return what == 1 ? x.r0 < y.r0 : x.c0 < y.c0; });
    std::vector<Rect> merged;
    for (const Rect& x : order) {
      if (!merged.empty() && (what == 1 ? x.r0 <= merged.back().r1 + 1 : x.c0 <= merged.back().c1 + 1)) {
        merged.back().r1 = std::max(merged.back().r1, x.r1);
        merged.back().c1 = std::max(merged.back().c1, x.c1);
      } else {
        merged.push_back(x);
      }
    }
    order = std::move(merged);
  }
  // rows of a filtered list: only the rows it shows are deleted, the ones it hides between them stay (Excel); every
  // row picked hidden (the Name Box): those rows themselves. Python's Rows(...).Delete deletes them all, as Excel's
  if (!insert && what == 1 && sh) {
    std::vector<Rect> shown = engine::FilterShownRuns(*sh, order);
    if (!shown.empty()) order = std::move(shown);
  }
  std::sort(order.begin(), order.end(), [&](const Rect& x, const Rect& y) {
    if (what == 2 || what == 4) return x.c0 != y.c0 ? x.c0 > y.c0 : x.r0 > y.r0;
    return x.r0 != y.r0 ? x.r0 > y.r0 : x.c0 > y.c0;
  });
  if (order.size() > 1) s.last_insert.active = false;
  const std::vector<uint32_t> group = GroupTargets(s, sheet);  // grouped sheets too (Excel)
  s.engine->PostUser(label, [=](Document& d) {
    // every area of every grouped sheet is asked first: one refused refuses the command, as in Excel (the first areas
    // were deleted, then a later one refused with a message: a change half made)
    for (const uint32_t g : group) {
      for (const Rect& x : order) {
        const bool refused = what == 1   ? d.RefuseRowsCols(g, true, insert, x.r0, static_cast<uint32_t>(x.Rows()))
                             : what == 2 ? d.RefuseRowsCols(g, false, insert, x.c0, static_cast<uint32_t>(x.Cols()))
                             : insert    ? d.RefuseInsertCells(g, x, what == 3)
                                         : d.RefuseDeleteCells(g, x, what == 3);
        if (refused) return;
      }
    }
    for (const uint32_t g : group) {
      for (const Rect& x : order) {
        switch (what) {
          case 1:
            if (insert) d.InsertRows(g, x.r0, static_cast<uint32_t>(x.Rows()));
            else d.DeleteRows(g, x.r0, static_cast<uint32_t>(x.Rows()));
            break;
          case 2:
            if (insert) d.InsertCols(g, x.c0, static_cast<uint32_t>(x.Cols()));
            else d.DeleteCols(g, x.c0, static_cast<uint32_t>(x.Cols()));
            break;
          case 3:
            if (insert) d.InsertCells(g, x, true);
            else d.DeleteCells(g, x, true);
            break;
          default:
            if (insert) d.InsertCells(g, x, false);
            else d.DeleteCells(g, x, false);
            break;
        }
      }
    }
  });
}

void AutoSum(AppState& s, const char* fn) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.engine || !s.snap) return;
  const Rect sel = SelRect(s);
  const uint32_t sheet = s.focus_sheet;
  const std::string f = fn;
  // an earlier total: a formula of the same function (=SUM(...)); numbers are the other numeric cells, also those
  // formulas give (a Total column of =B2*C2), as in Excel
  auto is_total = [&](uint32_t r, uint32_t c) {
    if (!sh->GetFormula(r, c)) return false;
    const std::string t = engine::FormulaTextOf(s.snap->wb, sheet, r, c);
    return t.size() > f.size() + 1 && t.compare(1, f.size(), f) == 0 && t[f.size() + 1] == '(';
  };
  auto numeric = [&](uint32_t r, uint32_t c) { return sh->GetValue(r, c).IsNumeric() && !is_total(r, c); };
  // a filtered list: SUBTOTAL leaves the rows the filter hides out, as Excel writes it (=SUBTOTAL(9,B2:B20))
  const int sub = f == "SUM" ? 9 : f == "AVERAGE" ? 1 : f == "COUNT" ? 2 : f == "MAX" ? 4 : f == "MIN" ? 5 : 0;
  std::map<uint32_t, int32_t> filter_hidden;
  if (sub && sh->autofilter->on) filter_hidden = sh->autofilter->hidden;
  auto call = [f, sub, filter_hidden](uint32_t r0, uint32_t r1, const std::string& range) {
    const auto it = filter_hidden.lower_bound(r0);
    if (it != filter_hidden.end() && it->first <= r1) return "SUBTOTAL(" + std::to_string(sub) + "," + range + ")";
    return f + "(" + range + ")";
  };
  // the table a cell is right under, without its totals row: AutoSum there makes the row the totals row and sums
  // with SUBTOTAL(109...) as Excel 16 does (measured: Alt+= in C7 under B2:D6 is =SUBTOTAL(109,[Qty]), over C7:D7
  // =SUBTOTAL(109,C3:C6) and =SUBTOTAL(109,D3:D6))
  auto under_table = [&](uint32_t r, uint32_t c) -> const model::Table* {
    for (const model::Table& t : *sh->tables) {
      if (!t.totals && r == t.range.r1 + 1 && c >= t.range.c0 && c <= t.range.c1 && t.HasData()) return &t;
    }
    return nullptr;
  };
  const int sub_total = sub ? 100 + sub : 0;  // SUBTOTAL's code that leaves hidden rows out, as a totals row's
  // B2, or B2:B4 (Excel writes =SUM(C2) for one cell, not C2:C2)
  auto ref = [](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
    return r0 == r1 && c0 == c1 ? core::CellA1(r0, c0) : core::CellA1(r0, c0) + ":" + core::CellA1(r1, c1);
  };
  const int64_t row_limit = sh->row_limit, col_limit = sh->col_limit;
  // the last row and column that hold a value (a formula holds its result): the used range counts formats too, and a
  // number format over XFD4:XFD1048576 made AutoSum's empty column a million totals long (basics seq417545: 18 s)
  uint32_t used_r1 = 0, used_c1 = 0;
  if (sh->has_used) {
    engine::ForEachCellInTiles(sh->values, sh->used, [&](uint32_t rr, uint32_t cc, const model::ValueTile& tile, uint32_t idx) {
      if (!tile.Occupied(idx)) return;
      used_r1 = std::max(used_r1, rr);
      used_c1 = std::max(used_c1, cc);
    });
  }

  // ---- one cell's proposal (Excel 16, measured): upwards first, past the blanks and labels just above (x in B5,
  // then AutoSum in B6 sums B2:B5; TRUE or an error there stops it), to the first number. A plain number starts a run that a
  // text, a blank or an earlier total ends, and the range goes from its top to the cell above, blanks included (B2:B7
  // numbers, AutoSum in B9: =SUM(B2:B8); 5, 6 under a total: =SUM(B5:B6)). A total first: the totals of the numbers
  // above are added, the lowest first (=SUM(B7,B4)). No number above: the same to the left (=SUM(B5:E5)).
  struct Proposal {
    bool ok = false, up = true;
    Rect range{};                      // the run summed
    std::vector<core::CellPos> totals;  // or the earlier totals added
  };
  auto any_number = [&](uint32_t rr, uint32_t cc) { return sh->GetValue(rr, cc).IsNumeric(); };
  auto num_total = [&](uint32_t rr, uint32_t cc) { return any_number(rr, cc) && is_total(rr, cc); };
  auto propose = [&](uint32_t r, uint32_t c, bool up) {
    Proposal p;
    p.up = up;
    const int64_t from = (up ? static_cast<int64_t>(r) : static_cast<int64_t>(c)) - 1;
    auto at = [&](int64_t k) { return up ? core::CellPos{static_cast<uint32_t>(k), c} : core::CellPos{r, static_cast<uint32_t>(k)}; };
    int64_t k = std::min<int64_t>(from, up ? used_r1 : used_c1);
    while (k >= 0 && (sh->IsEmptyCell(at(k).row, at(k).col) || sh->GetValue(at(k).row, at(k).col).IsString())) --k;
    if (k < 0 || !any_number(at(k).row, at(k).col)) return p;
    if (num_total(at(k).row, at(k).col)) {
      while (k >= 0 && any_number(at(k).row, at(k).col)) {
        if (num_total(at(k).row, at(k).col)) p.totals.push_back(at(k));
        --k;
      }
    } else {
      while (k >= 0 && any_number(at(k).row, at(k).col) && !num_total(at(k).row, at(k).col)) --k;
    }
    const core::CellPos t0 = at(k + 1), t1 = at(from);
    p.range = Rect{t0.row, t0.col, t1.row, t1.col};
    p.ok = true;
    return p;
  };
  auto propose_cell = [&](uint32_t r, uint32_t c) {
    Proposal p;
    if (r > 0) p = propose(r, c, true);
    if (!p.ok && c > 0) p = propose(r, c, false);
    return p;
  };
  // its formula for the cell (dr, dc) away: the same range moved, as a formula copied there (Excel: AutoSum over
  // B5:C5 writes =SUM(B2:B4) and =SUM(C2:C4), over C5:C6 =SUM(C2:C4) and =SUM(C3:C5))
  auto proposal_text = [&](const Proposal& p, uint32_t dr, uint32_t dc) -> std::string {
    if (!p.totals.empty()) {
      std::string list;
      for (size_t i = 0; i < p.totals.size(); ++i) list += (i ? "," : "") + core::CellA1(p.totals[i].row + dr, p.totals[i].col + dc);
      return "=" + f + "(" + list + ")";
    }
    const Rect g{p.range.r0 + dr, p.range.c0 + dc, p.range.r1 + dr, p.range.c1 + dc};
    const std::string range = ref(g.r0, g.c0, g.r1, g.c1);
    return "=" + (p.up ? call(g.r0, g.r1, range) : f + "(" + range + ")");
  };
  // one cell, as Excel: the proposal in the cell's editor for the user to confirm, the range selected (typing replaces
  // it) and pointed at (the arrows move it from its first cell, Shift its other end; a drag replaces it)
  auto edit_cell = [&](uint32_t r, uint32_t c) {
    const Proposal p = propose_cell(r, c);
    if (const model::Table* t = sub_total ? under_table(r, c) : nullptr; t && p.ok && p.up && p.totals.empty()) {
      const uint32_t j = c - t->range.c0;
      const std::string name = j < t->columns.size() ? t->columns[j].name : std::string();
      if (!name.empty()) {
        std::string esc;
        for (char ch : name) {
          if (ch == '[' || ch == ']' || ch == '#' || ch == '\'') esc += '\'';
          esc += ch;
        }
        StartEdit(s, true, "=SUBTOTAL(" + std::to_string(sub_total) + ",[" + esc + "])");
        return;
      }
    }
    if (!p.ok) {
      StartEdit(s, true, std::string("=") + fn + "()");
      s.edit.pending_caret = static_cast<int>(s.edit.text.size()) - 1;
      s.edit.cursor_to_end = false;
      return;
    }
    std::string text = proposal_text(p, 0, 0);
    // a column's part of a table, written as the table's (Excel 16, measured: Alt+= under a totals row proposes
    // =SUM(Table1[[#Data],[#Totals],[Qty]]))
    if (p.up && p.totals.empty()) {
      const std::string a1 = ref(p.range.r0, p.range.c0, p.range.r1, p.range.c1);
      if (const std::string st = TableRefFor(s, sheet, p.range, r); !st.empty() && text.find("(" + a1 + ")") != std::string::npos) {
        text.replace(text.find("(" + a1 + ")") + 1, a1.size(), st);
      }
    }
    StartEdit(s, true, text);
    const size_t open = text.find('(');
    const size_t b = p.up && text.compare(1, 9, "SUBTOTAL(") == 0 ? text.find(',') + 1 : open + 1;
    if (!s.edit.active || open == std::string::npos || b >= text.size()) return;
    s.edit.ref_begin = static_cast<int>(b);
    s.edit.ref_end = static_cast<int>(text.size()) - 1;
    const core::CellPos first = p.totals.empty() ? core::CellPos{p.range.r0, p.range.c0} : p.totals.front();
    const core::CellPos last = p.totals.empty() ? core::CellPos{p.range.r1, p.range.c1} : p.totals.front();
    s.edit.kpoint = true;
    s.edit.kp_ar = s.edit.ref_r0 = first.row;
    s.edit.kp_ac = s.edit.ref_c0 = first.col;
    s.edit.kp_r = last.row;
    s.edit.kp_c = last.col;
    s.edit.pending_sel_from = s.edit.sel_start = s.edit.ref_begin;
    s.edit.pending_caret = s.edit.sel_end = s.edit.caret = s.edit.ref_end;
    s.edit.cursor_to_end = false;
  };
  if (sel.IsCell()) return edit_cell(sel.r0, sel.c0);
  // the whole sheet: no row below it and no column beside it for totals, and Excel 16 writes none (measured
  // 2026-11-15: it starts editing the active cell, and Escape leaves the sheet as it was); whole columns get their totals
  // in the last row and whole rows in the last column, as below. Totals in both wrote a million formulas (chaos 5324)
  if (int64_t{sel.r0} == 0 && int64_t{sel.r1} + 1 >= row_limit && sel.c0 == 0 && int64_t{sel.c1} + 1 >= col_limit) {
    const auto& av = s.FocusView().sel;
    return edit_cell(av.act_r, av.act_c);
  }

  // ---- a range (Excel 16, measured) ----
  const auto& v = s.FocusView().sel;
  auto row_any = [&](uint32_t r, uint32_t c0, uint32_t c1, auto&& pred) {
    if (r > used_r1) return false;
    for (uint32_t c = c0; c <= std::min(c1, used_c1); ++c) {
      if (pred(r, c)) return true;
    }
    return false;
  };
  auto col_any = [&](uint32_t c, uint32_t r0, uint32_t r1, auto&& pred) {
    if (c > used_c1) return false;
    for (uint32_t r = r0; r <= std::min(r1, used_r1); ++r) {
      if (pred(r, c)) return true;
    }
    return false;
  };
  auto taken = [&](uint32_t r, uint32_t c) { return !sh->IsEmptyCell(r, c) && !is_total(r, c); };  // not free for a total
  bool any_filled = false, any_numeric = false;
  for (uint32_t r = sel.r0; r <= std::min(sel.r1, used_r1) && !any_numeric; ++r) {
    any_filled = any_filled || row_any(r, sel.c0, sel.c1, taken);  // (earlier totals count as empty: they are redone)
    any_numeric = row_any(r, sel.c0, sel.c1, numeric);
  }
  // empty cells: a row or a column of them gets the first cell's proposal, moved for each cell; a block of them, or
  // a first cell without a proposal, the editor as for one cell (and so do cells with no number at all)
  if (!any_filled && (sel.r0 == sel.r1 || sel.c0 == sel.c1)) {
    const Proposal p = propose_cell(sel.r0, sel.c0);
    if (!p.ok) return edit_cell(v.act_r, v.act_c);
    // (a whole empty column or row: down to the used area's end, not a million formulas)
    const uint32_t r_end = sel.Rows() > 65536 ? std::max(sel.r0, used_r1 + 1) : sel.r1;
    const uint32_t c_end = sel.Cols() > 4096 ? std::max(sel.c0, used_c1 + 1) : sel.c1;
    std::vector<std::pair<core::CellPos, std::string>> out;
    for (uint32_t r = sel.r0; r <= r_end; ++r) {
      for (uint32_t c = sel.c0; c <= c_end; ++c) {
        std::string text = proposal_text(p, r - sel.r0, c - sel.c0);
        // under a table: the row becomes its totals row, summed with SUBTOTAL (=SUBTOTAL(109,C3:C6))
        if (sub_total && p.up && p.totals.empty() && under_table(r, c) && text.compare(0, f.size() + 2, "=" + f + "(") == 0) {
          text = "=SUBTOTAL(" + std::to_string(sub_total) + "," + text.substr(f.size() + 2);
        }
        out.push_back({core::CellPos{r, c}, text});
      }
    }
    // entered as typed: a total under a table makes its row the totals row (as typing it does)
    s.engine->PostUser(TR("AutoSum"), [sheet, out](Document& d) {
      for (const auto& [at, text] : out) d.SetInput(sheet, at.row, at.col, text);
    });
    return;
  }
  if (!any_numeric) return edit_cell(v.act_r, v.act_c);
  // the labels at its start are left out: leading rows and columns without a number (A1:C4 with a header row and a
  // label column sums B2:C4 and selects from B2)
  Rect a = sel;
  for (int pass = 0; pass < 2; ++pass) {
    while (a.r0 < a.r1 && !row_any(a.r0, a.c0, a.c1, numeric)) ++a.r0;
    while (a.c0 < a.c1 && !col_any(a.c0, a.r0, a.r1, numeric)) ++a.c0;
  }
  // a last row (column) without a number gets the column (row) totals: inside it when it is empty, below it (to its
  // right) when it holds a label; both: the grand total in the corner too. Otherwise the totals go below each column
  // (one row selected: to its right), in the first row (column) where they overwrite nothing but earlier totals.
  const bool multi_row = a.r1 > a.r0, multi_col = a.c1 > a.c0;
  const bool last_row = multi_row && !row_any(a.r1, a.c0, a.c1, numeric);
  const bool last_row_in = last_row && !row_any(a.r1, a.c0, a.c1, taken);
  const bool last_col = multi_col && !col_any(a.c1, a.r0, a.r1, numeric);
  const bool last_col_in = last_col && !col_any(a.c1, a.r0, a.r1, taken);
  auto free_row = [&](int64_t from) -> int64_t {
    for (int64_t r = from; r < row_limit; ++r) {
      if (!row_any(static_cast<uint32_t>(r), a.c0, a.c1, taken)) return r;
    }
    return -1;
  };
  auto free_col = [&](int64_t from, uint32_t r0, uint32_t r1) -> int64_t {
    for (int64_t c = from; c < col_limit; ++c) {
      if (!col_any(static_cast<uint32_t>(c), r0, r1, taken)) return c;
    }
    return -1;
  };
  auto col_total = [&](uint32_t c, uint32_t r0, uint32_t r1) { return call(r0, r1, ref(r0, c, r1, c)); };
  auto row_total = [&](uint32_t r, uint32_t c0, uint32_t c1) { return f + "(" + ref(r, c0, r, c1) + ")"; };
  std::vector<std::pair<core::CellPos, std::string>> out;
  Rect grown = a;
  // whole columns (rows): the totals beside each row (under each column) go as far as the data, not to the sheet's end
  // (Excel 16, measured 2026-11-16: E:I with I empty writes I5:I9 for data in rows 5:9 and the totals in row
  // 1048576, ten formulas; 5:9 with row 9 empty writes E9:I9 and XFD5:XFD9). Every row of E:I got one, a million
  // formulas that kept a debug build busy for 80 s (chaos seed 5643)
  // So does a selection that reaches the sheet's last row (column) from further in: B10:J1048576 wrote a row total in
  // every row to the end (chaos seed 6089, a million formulas); Excel 16 totals B and C, the columns with numbers, in
  // row 1048576 and the grand total in J (measured with CommandBars.ExecuteMso("AutoSum"))
  const bool whole_cols = int64_t{sel.r1} + 1 >= row_limit, whole_rows = int64_t{sel.c1} + 1 >= col_limit;
  // the data's last row (column): the values that aren't totals, not the used range, which the totals in the sheet's
  // last row reach after a first Alt+= (the second wrote a row total in every row again: chaos seed 6089)
  uint32_t data_last_r = a.r0, data_last_c = a.c0;
  if (whole_cols || whole_rows) {
    engine::ForEachValueRowMajor(*sh, Rect{a.r0, a.c0, std::min(a.r1, used_r1), std::min(a.c1, used_c1)}, [&](uint32_t r, uint32_t c, const core::Value&) {
      if (!is_total(r, c)) data_last_r = std::max(data_last_r, r), data_last_c = std::max(data_last_c, c);
      return true;
    });
  }
  auto rows_to = [&](uint32_t r1) { return whole_cols ? std::max(a.r0, std::min(r1, data_last_r)) : r1; };
  auto cols_to = [&](uint32_t c1) { return whole_rows ? std::max(a.c0, std::min(c1, data_last_c)) : c1; };
  if (last_row || last_col) {
    const uint32_t data_r1 = rows_to(last_row ? a.r1 - 1 : a.r1), data_c1 = cols_to(last_col ? a.c1 - 1 : a.c1);
    const int64_t tr = !last_row ? -1 : last_row_in ? static_cast<int64_t>(a.r1) : free_row(int64_t{a.r1} + 1);
    const int64_t tc = !last_col ? -1 : last_col_in ? static_cast<int64_t>(a.c1) : free_col(int64_t{a.c1} + 1, a.r0, a.r1);
    if (tr >= 0) {
      const uint32_t sr1 = last_row_in ? a.r1 - 1 : a.r1;
      for (uint32_t c = a.c0; c <= data_c1; ++c) {
        if (whole_cols && !col_any(c, a.r0, sr1, numeric)) continue;  // an empty column to the sheet's end: no total (measured)
        out.push_back({core::CellPos{static_cast<uint32_t>(tr), c}, col_total(c, a.r0, sr1)});
      }
      if (!last_row_in) grown.r1 = a.r1 + 1;
    }
    if (tc >= 0) {
      const uint32_t sc1 = last_col_in ? a.c1 - 1 : a.c1;
      for (uint32_t r = a.r0; r <= data_r1; ++r) out.push_back({core::CellPos{r, static_cast<uint32_t>(tc)}, row_total(r, a.c0, sc1)});
      if (!last_col_in) grown.c1 = a.c1 + 1;
    }
    if (tr >= 0 && tc >= 0) out.push_back({core::CellPos{static_cast<uint32_t>(tr), static_cast<uint32_t>(tc)}, row_total(static_cast<uint32_t>(tr), a.c0, static_cast<uint32_t>(tc) - 1)});
  } else if (multi_row) {
    if (const int64_t tr = free_row(int64_t{a.r1} + 1); tr >= 0) {
      for (uint32_t c = a.c0; c <= cols_to(a.c1); ++c) out.push_back({core::CellPos{static_cast<uint32_t>(tr), c}, col_total(c, a.r0, a.r1)});
      grown.r1 = a.r1 + 1;
    }
  } else if (const int64_t tc = free_col(int64_t{a.c1} + 1, a.r0, a.r0); tc >= 0) {
    out.push_back({core::CellPos{a.r0, static_cast<uint32_t>(tc)}, row_total(a.r0, a.c0, a.c1)});
    grown.c1 = a.c1 + 1;
  }
  grown.r1 = static_cast<uint32_t>(std::min<int64_t>(grown.r1, row_limit - 1));
  grown.c1 = static_cast<uint32_t>(std::min<int64_t>(grown.c1, col_limit - 1));
  if (out.empty()) return;
  // entered as typed: a total written under a table makes its row the totals row (Excel 16, measured: Alt+= over
  // C3:C6 writes =SUM(C3:C6) in C7 and the table shows its totals row)
  s.engine->PostUser(TR("AutoSum"), [sheet, out](Document& d) {
    for (const auto& [at, text] : out) d.SetInput(sheet, at.row, at.col, "=" + text);
  });
  // the selection takes in the row (column) of totals below (beside) it, from its first number (Excel)
  render::SelectRange(s.FocusView(), grown);
}

void FillSelection(AppState& s, int dir) {
  if (!s.engine) return;
  if (AskBeforeLargeOperation(s, SelectionCells(s), [&s, dir]() { FillSelection(s, dir); })) return;
  EndCopyMode(s);  // Excel: Ctrl+D / Ctrl+R end cut / copy mode
  const Rect a = SelRect(s);
  const uint32_t focus = s.focus_sheet;
  const std::vector<uint32_t> group = GroupTargets(s, focus);  // grouped sheets fill too (Excel)
  s.engine->PostUser(TR("Fill"), [=](Document& d) {
   for (const uint32_t sheet : group) {
    if (dir == 0 && d.FillDownShown(sheet, a)) {
      // a filtered list: the top row went into the rows it shows only (Excel)
    } else if (dir == 0) {
      if (a.r0 == a.r1 && a.r0 > 0) d.FillDown(sheet, Rect{a.r0 - 1, a.c0, a.r1, a.c1}, false);  // Ctrl+D on a single row: copy the row above
      else d.FillDown(sheet, a);
    } else if (dir == 1) {
      // Fill Right beside a table grows it in a step of its own after the fill's (Excel 16's first Ctrl+Z takes back
      // the growth only; Fill Down's one Ctrl+Z takes back both, measured by keys): the next post makes it
      d.defer_table_growth = true;
      if (a.c0 == a.c1 && a.c0 > 0) d.FillRight(sheet, Rect{a.r0, a.c0 - 1, a.r1, a.c1}, false);
      else d.FillRight(sheet, a);
      d.defer_table_growth = false;
    } else if (dir == 2 && d.FillDownShown(sheet, a, true)) {
      // a filtered list: the bottom row went into the rows it shows only (Excel, as Fill Down)
    } else {
      d.FillEdge(sheet, a, dir);  // up: the bottom row up; left: the right column left
    }
   }
  });
  if (dir == 1) s.engine->PostUser(TR("Table AutoExpansion"), [](Document& d) { d.GrowDeferredTables(); });
}


void SortSelection(AppState& s, bool descending) {
  if (!s.FocusSheet() || !s.engine) return;
  RunDataAction(s, descending ? 1 : 0, FindDataRange(s));
}

void SortDataRange(AppState& s, bool descending, const DataRange& t) {
  if (!s.FocusSheet() || !s.engine) return;
  const Rect a = t.rect;
  const bool header = t.header;
  const auto& sel = s.FocusView().sel;
  const uint32_t sheet = s.focus_sheet, key = std::clamp(sel.act_c, a.c0, a.c1);
  const model::AutoFilter& af = *s.FocusSheet()->autofilter;
  // sorting the AutoFilter's list (or a table's) records the sort: the column's button shows it, Reapply sorts again
  const bool in_filter = af.on && a.c0 == af.range.c0 && a.c1 == af.range.c1 && a.r0 == af.range.r0 + (header ? 0 : 1);
  const uint32_t table = t.table;
  s.engine->PostUser(TR("Sort"), [=](Document& d) {
    d.Sort(sheet, a, {{key, descending}}, header);
    if (in_filter) d.SetFilterSort(sheet, {model::SortLevel{key - a.c0, descending}});
    else if (table) d.SetTableFilterSort(sheet, table, {model::SortLevel{key - a.c0, descending}});  // a table's own filter
  });
  if (!t.table && !in_filter) render::SelectRange(s.FocusView(), a);
}

void SplitWindow(AppState& s) {
  auto& v = s.FocusView();
  v.mirror_left = false;  // a split replaces a T layout
  if (v.split) {
    // the window shows its top-left pane's cells again, as Excel's (measured: ScrollRow 1 after the split goes)
    v.split = false;
    UnfreezeView(v);
    return;
  }
  // like Excel: above and left of the active cell; from the top-left cell the window splits into four at its middle
  const uint32_t r = v.sel.act_r, c = v.sel.act_c;
  v.frozen_top = static_cast<uint32_t>(v.v.first);
  v.frozen_left = static_cast<uint32_t>(v.h.first);
  uint32_t nr = r > v.v.first ? static_cast<uint32_t>(r - v.v.first) : 0, nc = c > v.h.first ? static_cast<uint32_t>(c - v.h.first) : 0;
  if (nr == 0 && nc == 0) {
    nr = static_cast<uint32_t>(std::max<int64_t>(1, v.vis_rows / 2));
    nc = static_cast<uint32_t>(std::max<int64_t>(1, v.vis_cols / 2));
  }
  v.freeze_rows = nr;
  v.freeze_cols = nc;
  v.split = true;
  v.v.first = v.frozen_top + nr;
  v.h.first = v.frozen_left + nc;
}

void FreezePanes(AppState& s, int kind) {
  auto& v = s.FocusView();
  if (kind == 3) {
    // [YC] T layout: the selected columns stay in the middle, rows above the active cell on top (an option chain:
    // calls | strike | puts with the strike frozen)
    const Rect a = v.sel.areas.empty() ? Rect::Cell(v.sel.act_r, v.sel.act_c) : v.sel.areas.back();
    if (a.c0 == 0) {
      s.SetStatus(TR("Select the columns to keep in the middle (not column A): the columns on their left scroll the other way"));
      return;
    }
    v.split = false;
    const uint32_t r = v.sel.act_r;
    v.frozen_top = static_cast<uint32_t>(std::min<int64_t>(v.v.first, r));
    v.freeze_rows = r - v.frozen_top;
    v.v.first = r;
    v.frozen_left = a.c0;
    v.freeze_cols = std::min<uint32_t>(a.c1, core::kMaxCols - 2) - a.c0 + 1;
    v.h.first = v.frozen_left + v.freeze_cols;
    v.mirror_left = true;
    s.SetStatus(TR("Panes frozen with the selected columns in the middle"));
    return;
  }
  v.mirror_left = false;
  if (v.split) {
    // freezing replaces a split (kept where it was)
    v.split = false;
    if (kind == 0 && (v.freeze_rows || v.freeze_cols)) {
      s.SetStatus(TR("Panes frozen"));
      return;
    }
  }
  if (kind == 0 && (v.freeze_rows || v.freeze_cols)) {
    UnfreezeView(v);
    s.SetStatus(TR("Panes unfrozen"));
    return;
  }
  if (kind == 1) {
    v.frozen_top = static_cast<uint32_t>(v.v.first);
    v.freeze_rows = 1;
    v.freeze_cols = 0;
    v.v.first = v.frozen_top + 1;
  } else if (kind == 2) {
    v.frozen_left = static_cast<uint32_t>(v.h.first);
    v.freeze_cols = 1;
    v.freeze_rows = 0;
    v.h.first = v.frozen_left + 1;
  } else {
    const uint32_t r = v.sel.act_r, c = v.sel.act_c;
    v.frozen_top = static_cast<uint32_t>(std::min<int64_t>(v.v.first, r));
    v.frozen_left = static_cast<uint32_t>(std::min<int64_t>(v.h.first, c));
    v.freeze_rows = r - v.frozen_top;
    v.freeze_cols = c - v.frozen_left;
    v.v.first = r;
    v.h.first = c;
  }
  s.SetStatus(TR("Panes frozen"));
}

void ActivateSheet(AppState& s, uint32_t id) {
  if (!s.snap || !s.snap->Sheet(id)) return;
  if (!s.group_sheets.empty() && std::find(s.group_sheets.begin(), s.group_sheets.end(), id) == s.group_sheets.end()) SetSheetGroup(s, {}, id);
  auto& w = s.Win(id);
  if (w.floating) {
    ImGui::SetWindowFocus(s.SheetWindowName(id).c_str());
  } else {
    s.active_sheet = id;
  }
  s.focus_sheet = id;
}

namespace {

// split at top-level commas (not inside quotes or parentheses)
std::vector<std::string> SplitUnion(const std::string& t) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool q = false;
  for (char ch : t) {
    if (ch == 0x27) q = !q;  // single quote (sheet names)
    else if (!q && ch == '(') ++depth;
    else if (!q && ch == ')') --depth;
    if (!q && depth == 0 && ch == ',') {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += ch;
  }
  out.push_back(cur);
  for (auto& x : out) {
    while (!x.empty() && x.back() == ' ') x.pop_back();
    while (!x.empty() && x.front() == ' ') x.erase(x.begin());
  }
  return out;
}

// one address or name -> sheet + range (names may be dynamic, e.g. OFFSET(...))
bool ResolveGotoPart(AppState& s, const std::string& part, uint32_t* sheet, Rect* r) {
  std::string sheet_name;
  std::string_view addr;
  *sheet = s.focus_sheet;
  if (core::SplitSheetRef(part, &sheet_name, &addr) && !sheet_name.empty()) {
    const int32_t i = s.snap->wb.FindSheet(sheet_name);
    if (i < 0) return false;
    *sheet = s.snap->wb.sheets[static_cast<size_t>(i)]->id;
  } else {
    addr = part;
  }
  std::string clean(addr);
  clean.erase(std::remove(clean.begin(), clean.end(), '$'), clean.end());
  if (core::ParseRectA1(clean, r)) return true;
  if (!s.engine) return false;
  const uint32_t focus = s.focus_sheet;
  const core::CellPos at{s.FocusView().sel.act_r, s.FocusView().sel.act_c};  // a relative name reads from the active cell
  if (s.engine->Call([&](engine::Document& d) { return d.ResolveNameRef(part, focus, sheet, r, nullptr, at); })) return true;
  // a table's name or a structured reference to it (Trades, Trades[Qty], Trades[#All]): Excel 16 goes there (measured
  // through Application.Goto, the Name Box's and Go To's path)
  return ResolveRangeText(s.snap->wb, focus, part, sheet, r);
}

}  // namespace

void Goto(AppState& s, const std::string& ref_in) {
  if (!s.snap) return;
  std::string ref = ref_in;
  while (!ref.empty() && ref.back() == ' ') ref.pop_back();
  while (!ref.empty() && (ref.front() == ' ' || ref.front() == '=')) ref.erase(ref.begin());
  if (ref.empty()) return;
  // a name asks the engine what it refers to: while the engine works, Go To waits for it out of the frame
  bool names = false;
  for (const std::string& part : SplitUnion(ref)) {
    std::string sheet_name;
    std::string_view addr;
    if (!(core::SplitSheetRef(part, &sheet_name, &addr) && !sheet_name.empty())) addr = part;
    std::string clean(addr);
    clean.erase(std::remove(clean.begin(), clean.end(), '$'), clean.end());
    Rect r;
    names = names || !core::ParseRectA1(clean, &r);
  }
  if (names && DeferWhileEngineBusy(s, [ref_in](AppState& st) { Goto(st, ref_in); }, false)) return;
  std::vector<Rect> rects;
  uint32_t sheet = s.focus_sheet;
  for (const std::string& part : SplitUnion(ref)) {
    uint32_t sid = 0;
    Rect r;
    if (part.empty() || !ResolveGotoPart(s, part, &sid, &r)) {
      s.ShowError("YCGrid", TR("Invalid reference: ") + ref_in);
      return;
    }
    if (rects.empty()) sheet = sid;
    else if (sid != sheet) continue;  // a multiple selection must be on one sheet
    rects.push_back(r);
  }
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh || rects.empty()) return;
  for (Rect& r : rects) {
    if (r.r1 == core::kRowEnd || r.r1 >= sh->row_limit) r.r1 = sh->row_limit - 1;
    if (r.c1 == core::kColEnd || r.c1 >= sh->col_limit) r.c1 = sh->col_limit - 1;
    if (r.r0 >= sh->row_limit) {
      s.ShowMessage("YCGrid", TR("The reference is outside the sheet (currently ") + std::to_string(sh->row_limit) + TR(" rows)."));
      return;
    }
  }
  ActivateSheet(s, sheet);
  ClearObjectSelection();  // Excel: the Name Box / Go To selects cells, a selected chart or shape lets go
  auto& v = s.ViewFor(sheet);
  const bool extra = &v != &s.Win(sheet).view;
  render::SelectRange(v, rects.back());
  if (rects.size() > 1) v.sel.areas = rects;
  render::NormalizeWholeSelection(v, *sh);  // C:C, 5:5: whole columns / rows, as a header click selects them
  render::EnsureVisible(*sh, v, rects.back().r0, rects.back().c0);
  // Excel: after the Name Box or Go To, the keyboard is the sheet's (a pane clicked before keeps it no longer)
  if (extra) ImGui::SetWindowFocus(("###yc_xwin" + std::to_string(s.focus_win)).c_str());
  else ImGui::SetWindowFocus(s.Win(sheet).floating ? s.SheetWindowName(sheet).c_str() : "###yc_gridhost");
}

namespace {

// Increase / Decrease Decimal on one section of a format code (Excel 16, measured): only a number section's
// decimals change, in scientific notation the mantissa's (0.00E+00 -> 0.000E+00, ##0.0E+0 -> ##0E+0); a date or
// time section (mm:ss.0), a fraction (# ?/?), General, text and a section without digit placeholders ("-") stay as
// they are, except accounting's zero section ("-"?? after a * fill), whose question marks keep the dash lined up
// with the decimals. A new decimal repeats the kind of the last one (0.? -> 0.??, #.## -> #.###).
std::string ShiftSectionDecimals(std::string sec, int delta) {
  constexpr size_t npos = std::string::npos;
  std::vector<size_t> ph;  // digit placeholders (0 # ?) outside quotes, brackets and escapes
  size_t dot = npos, exp = npos, dash_end = npos;
  bool skip = false, fill = false, digits = false;
  for (size_t i = 0; i < sec.size(); ++i) {
    const char ch = sec[i];
    if (ch == '"') {
      const size_t close = sec.find('"', i + 1);
      if (close == npos) break;
      if (close == i + 2 && sec[i + 1] == '-') dash_end = close;
      i = close;
      continue;
    }
    if (ch == '\\' || ch == '_') {  // an escaped character, a space as wide as one
      ++i;
      continue;
    }
    if (ch == '*') {  // the fill character follows
      fill = true;
      ++i;
      continue;
    }
    if (ch == '[') {  // a color, a condition, a locale or an elapsed time ([h])
      const size_t close = sec.find(']', i + 1);
      if (close == npos) break;
      if (close == i + 2 && (sec[i + 1] == 'h' || sec[i + 1] == 'H' || sec[i + 1] == 'm' || sec[i + 1] == 'M' ||
                             sec[i + 1] == 's' || sec[i + 1] == 'S')) {
        skip = true;
      }
      i = close;
      continue;
    }
    const char lc = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    if (lc == 'g' && i + 7 <= sec.size()) {
      std::string w = sec.substr(i, 7);
      for (char& c : w) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      if (w == "general") {
        skip = true;
        break;
      }
    }
    if (lc == 'e' && exp == npos && i + 1 < sec.size() && (sec[i + 1] == '+' || sec[i + 1] == '-')) {
      exp = i;
      ++i;
      continue;
    }
    if (lc == 'y' || lc == 'm' || lc == 'd' || lc == 'h' || lc == 's' || ch == '/' || ch == '@') {
      skip = true;  // a date, a time, a fraction or text
      break;
    }
    if (ch == '0' || ch == '#' || ch == '?') {
      if (exp == npos) ph.push_back(i);
      if (ch != '?') digits = true;
    }
    if (ch == '.' && dot == npos && exp == npos) dot = i;
  }
  if (skip) return sec;
  if (!digits && dash_end != npos) {
    // accounting's zero section ("-"??): one ? per decimal
    size_t last_q = npos;
    for (size_t p : ph) {
      if (p > dash_end) last_q = p;
    }
    if (last_q == npos && !fill) return sec;  // a plain "-" (#,##0.00;-#,##0.00;"-") stays
    if (delta > 0) sec.insert((last_q != npos ? last_q : dash_end) + 1, "?");
    else if (last_q != npos) sec.erase(last_q, 1);
    return sec;
  }
  if (ph.empty()) return sec;
  const size_t last = ph.back();  // the mantissa's last placeholder
  if (delta > 0) {
    if (dot == npos) sec.insert(last + 1, ".0");
    else if (last > dot) sec.insert(last + 1, 1, sec[last]);
    else sec.insert(dot + 1, "0");
  } else if (dot != npos && last > dot) {
    const bool more = ph.size() >= 2 && ph[ph.size() - 2] > dot;
    sec.erase(last, 1);
    if (!more) sec.erase(dot, 1);
  }
  return sec;
}

// the code Increase / Decrease Decimal makes of a format code (not General): each section on its own
std::string ShiftCodeDecimals(const std::string& code, int delta) {
  std::string out, sec;
  bool in_q = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (!in_q && ch == '\\' && i + 1 < code.size()) {
      sec += ch;
      sec += code[++i];
      continue;
    }
    if (ch == '"') in_q = !in_q;
    if (!in_q && ch == ';') {
      out += ShiftSectionDecimals(sec, delta) + ";";
      sec.clear();
      continue;
    }
    sec += ch;
  }
  return out + ShiftSectionDecimals(sec, delta);
}

// the code Increase / Decrease Decimal gives a General cell, from what General shows (`shown`, Excel 16, measured):
// one decimal more or less than it shows (1234.5678 -> 0.00000 / 0.000), in E notation when General shows that
// (2.5E-10 -> 0.00E+00 / 0E+00, 1.23457E+11 -> 0.000000E+00); "" keeps General: a text, or fewer decimals than none
// (1, 100 and 1E+20 decreased)
std::string GeneralShiftedCode(const std::string& shown, int delta) {
  const size_t e = shown.find('E');
  const std::string mant = shown.substr(0, e);
  const size_t p = mant.find('.');
  const int dec = (p == std::string::npos ? 0 : static_cast<int>(mant.size() - p - 1)) + delta;
  if (dec < 0) return "";
  std::string code = dec ? "0." + std::string(static_cast<size_t>(dec), '0') : "0";
  if (e != std::string::npos) code += "E+00";
  return code;
}

}  // namespace

void ChangeDecimals(AppState& s, int delta) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap) return;
  const auto& sel = s.FocusView().sel;
  const model::Style& st = s.snap->wb.styles->Get(sh->GetStyle(sel.act_r, sel.act_c));
  const std::string old_code = s.snap->wb.styles->NumberFormat(st);
  std::string code;
  if (old_code == "General" || old_code.empty()) {
    // based on the decimals General shows; a text, a logical value or an error keeps General (Excel)
    const auto& v = sh->GetValue(sel.act_r, sel.act_c);
    if (!v.IsNumeric() && !v.IsEmpty()) return;
    // what General shows at the column's width (0.333333 in a default column, so 7 decimals up and 5 down; 0.3333333
    // where it is wider), as the grid drew it lately; eleven characters when the cell isn't on the screen
    const auto& view = s.FocusView();
    const bool drawn = v.IsNumeric() && ImGui::GetFrameCount() - view.active_shown_frame <= 2 && !view.active_shown.empty() &&
                       view.active_shown.find('#') == std::string::npos;
    code = GeneralShiftedCode(drawn ? view.active_shown : v.IsNumeric() ? formula::FormatGeneral(v.AsDouble(), 11) : "0", delta);
    if (code.empty()) return;
  } else {
    code = ShiftCodeDecimals(old_code, delta);
    if (code == old_code) return;  // a date, a time, a fraction or text: Excel changes nothing
  }
  ApplyNumberFormat(s, code, delta > 0 ? TR("Increase Decimal") : TR("Decrease Decimal"));
}

// the columns of areas a number format widens, as Excel does (16, measured: Increase Decimal, Comma Style, Percent
// Style, Range.NumberFormat): a number the new code shows wider than its column widens a column whose width nobody set
// (or that a format widened before); a column of a set width shows ####. Measured on the UI thread (the font atlas)
std::vector<std::pair<uint32_t, int32_t>> ColumnsToWidenForFormat(AppState& s, uint32_t sheet, const std::vector<Rect>& areas, const std::string& code) {
  std::vector<std::pair<uint32_t, int32_t>> out;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || !sh->has_used) return out;
  const auto fmt = formula::CompileFormat(code);
  if (!fmt) return out;
  std::set<uint32_t> seen;
  int64_t cells = 0;
  for (const Rect& a0 : areas) {
    const Rect a{std::max(a0.r0, sh->used.r0), std::max(a0.c0, sh->used.c0), std::min(a0.r1, sh->used.r1), std::min(a0.c1, sh->used.c1)};
    if (a.r0 > a.r1 || a.c0 > a.c1) continue;
    for (uint32_t c = a.c0; c <= a.c1 && seen.size() < 1000; ++c) {
      if (!seen.insert(c).second || sh->cols->IsHidden(c)) continue;
      const int32_t now = sh->cols->Size(c);
      const auto aw = s.auto_widened.find({sheet, c});
      if (sh->cols->IsCustom(c) && (aw == s.auto_widened.end() || aw->second != now)) continue;
      int32_t best = now;
      engine::ForEachValueRowMajor(*sh, Rect{a.r0, c, a.r1, c}, [&](uint32_t row, uint32_t, const core::Value& v) {
        if (!v.IsNumeric()) return ++cells < 200000;
        for (const Rect& m : *sh->merges) {
          if (m.Contains(row, c) && !m.IsCell()) return ++cells < 200000;
        }
        const model::Style& st = s.snap->wb.styles->Get(sh->GetStyle(row, c));
        const std::string text = formula::FormatValue(v, *fmt).text;
        ImFont* font = ui::CellFont(s.snap->wb.styles->FontName(st), st.bold, st.italic);
        const float px = s.snap->wb.styles->FontSize(st) * (96.0f / 72.0f);
        best = std::max(best, static_cast<int32_t>(std::ceil(font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x)) + 8);
        return ++cells < 200000;
      });
      if (best > now) out.emplace_back(c, best);
    }
  }
  return out;
}

void ApplyNumberFormat(AppState& s, const std::string& code, const std::string& label) {
  if (!s.engine) return;
  EndCopyMode(s);  // Excel: formatting ends cut / copy mode
  // format code strings must be interned on the engine thread (the style table is only written there)
  const auto areas = SelAreas(s);
  const uint32_t sheet = s.focus_sheet;
  const auto widen = ColumnsToWidenForFormat(s, sheet, areas, code);
  for (const auto& [c, px] : widen) s.auto_widened[{sheet, c}] = px;
  // one Undo step: the format and the columns it widened
  s.engine->PostUser(label, [areas, sheet, code, widen](Document& d) {
    const uint32_t id = code == "General" ? 0 : d.wb_mut().styles.mut().InternString(code);
    for (const Rect& a : areas) d.ModifyStyle(sheet, a, [id](model::Style& x) { x.numfmt = id; });
    for (const auto& [c, px] : widen) d.SetColWidth(sheet, c, c, px);
  });
}

std::string SelectionAddress(AppState& s) {
  const auto& sel = s.FocusView().sel;
  return core::CellA1(sel.act_r, sel.act_c);
}

void ApplyFormatPainter(AppState& s) {
  if (!s.format_painter || !s.engine) return;
  const uint32_t sheet = s.focus_sheet;
  const engine::ClipData clip = s.painter;
  // the cells the formats came from, painted again from their own corner, keep them: each takes its own format back,
  // nothing to do (the whole sheet merged, painted onto itself by a click, which selects the merged cell)
  std::vector<Rect> areas;
  const Rect& from = clip.area;
  for (const Rect& a : SelAreas(s)) {
    const bool itself = clip.src_sheet == sheet && a.r0 == from.r0 && a.c0 == from.c0 && a.r1 <= from.r1 && a.c1 <= from.c1;
    if (!itself) areas.push_back(a);
  }
  if (areas.empty()) {
    if (!s.format_painter_sticky) s.format_painter = false;
    return;
  }
  // a paste a copy of the painted cells: over a large selection that is billions of pastes (a click on a sheet
  // merged whole selects all of it: Format Painter froze YCGrid, basics seq338133). One cell's formats go over the
  // selection at once, as a format of the cells does (whole lines take line styles); a larger copy that many times is
  // refused with the reason
  {
    const uint64_t rr = std::max<uint32_t>(1, clip.rows), cc = std::max<uint32_t>(1, clip.cols);
    uint64_t tiles = 0;
    for (const Rect& a : areas) tiles += ((uint64_t{a.Rows()} + rr - 1) / rr) * ((uint64_t{a.Cols()} + cc - 1) / cc);
    if (tiles > 100'000) {
      const bool one = clip.rows == 1 && clip.cols == 1 && clip.merges.empty() && clip.rules.empty() && !clip.cells.empty();
      if (!one) {
        s.ShowMessage("YCGrid", TR("The selection is too large to paint: the copied formats would be pasted more than 100,000 times. Select fewer cells."));
        if (!s.format_painter_sticky) s.format_painter = false;
        return;
      }
      const model::Style st = clip.cells[0].has_style ? clip.cells[0].style : model::Style{};
      s.engine->PostUser(TR("Format Painter"), [=](Document& d) {
        for (const Rect& a : areas) d.ModifyStyle(sheet, a, [st](model::Style& x) { x = st; });
      });
      if (!s.format_painter_sticky) s.format_painter = false;
      return;
    }
  }
  s.engine->PostUser(TR("Format Painter"), [=](Document& d) {
    for (const Rect& a : areas) {
      for (uint32_t r = a.r0; r <= a.r1; r += std::max<uint32_t>(1, clip.rows)) {
        for (uint32_t c = a.c0; c <= a.c1; c += std::max<uint32_t>(1, clip.cols)) d.Paste(sheet, r, c, clip, engine::PasteMode::Formats);
      }
    }
  });
  if (!s.format_painter_sticky) s.format_painter = false;
}

namespace {

// AutoFit column width: measured on the UI thread (the font atlas can only be used there)
// rows: only these rows' cells (Home > Format > AutoFit Column Width fits the selected cells); nullptr: the column
int32_t MeasureColumn(AppState& s, const model::Sheet& sh, uint32_t c, const std::vector<Rect>* rows = nullptr) {
  if (!sh.has_used) return -1;
  int32_t best = -1;
  std::vector<Rect> parts;
  if (rows) {
    for (const Rect& a : *rows) {
      if (a.c0 > c || a.c1 < c) continue;
      const Rect x{std::max(a.r0, sh.used.r0), c, std::min(a.r1, sh.used.r1), c};
      if (x.r0 <= x.r1) parts.push_back(x);
    }
    if (parts.empty()) return -1;
  } else {
    parts.push_back(Rect{sh.used.r0, c, sh.used.r1, c});
  }
  int count = 0;
  for (const Rect& r : parts)
  engine::ForEachValueRowMajor(sh, r, [&](uint32_t row, uint32_t col, const core::Value& v) {
    for (const Rect& m : *sh.merges) {
      if (m.Contains(row, col) && !m.IsCell()) return true;
    }
    const model::Style& st = s.snap->wb.styles->Get(sh.GetStyle(row, col));
    const std::string text = render::FormatCellText(*s.snap, st, v, -1, nullptr, nullptr, nullptr);
    ImFont* font = ui::CellFont(s.snap->wb.styles->FontName(st), st.bold, st.italic);
    const float px = s.snap->wb.styles->FontSize(st) * (96.0f / 72.0f);
    float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str()).x;
    // AutoFilter header cells also hold the 17 px drop-down button (rules_ui.cpp), keep the text clear of it
    const auto& af = *sh.autofilter;
    if (af.on && row == af.range.r0 && col >= af.range.c0 && col <= af.range.c1) w += 19.0f;
    best = std::max(best, static_cast<int32_t>(std::ceil(w)) + 8);
    return ++count < 200000;
  });
  return best;
}

// the height a row needs for its text: font sizes, wrapped text, line breaks and rotated text (-1 = nothing to fit)
int32_t MeasureRow(AppState& s, const model::Sheet& sh, uint32_t row) {
  if (!sh.has_used) return -1;
  float best = 0;
  bool small = false;  // a font smaller than the standard one
  const Rect r{row, sh.used.c0, row, sh.used.c1};
  engine::ForEachValueRowMajor(sh, r, [&](uint32_t, uint32_t col, const core::Value& v) {
    for (const Rect& m : *sh.merges) {
      if (m.Contains(row, col) && !m.IsCell()) return true;
    }
    const model::Style& st = s.snap->wb.styles->Get(sh.GetStyle(row, col));
    const std::string text = render::FormatCellText(*s.snap, st, v, -1, nullptr, nullptr, nullptr);
    ImFont* font = ui::CellFont(s.snap->wb.styles->FontName(st), st.bold, st.italic);
    const float px = s.snap->wb.styles->FontSize(st) * (96.0f / 72.0f);
    float h;
    if (st.rotation == 255) {
      size_t chars = 0;
      for (unsigned char ch : text) chars += (ch & 0xC0) != 0x80;
      h = px * 1.05f * static_cast<float>(std::max<size_t>(1, chars));
    } else if (st.rotation) {
      const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.c_str());
      const float a = static_cast<float>(st.rotation) * 3.14159265f / 180.0f;
      h = std::fabs(ts.x * std::sin(a)) + std::fabs(ts.y * std::cos(a));
    } else {
      const float wrap_w = st.wrap && !v.IsNumeric() ? std::max(1.0f, static_cast<float>(sh.cols->Size(col)) - 8.0f) : 0.0f;
      h = TextBlockHeight(font, px, wrap_w, text, s.snap->wb.styles->FontName(st), st.bold, st.italic);
    }
    best = std::max(best, h);
    small = small || s.snap->wb.styles->FontSize(st) < std::max(1.0f, s.snap->wb.styles->FontSize(model::Style{}));
    return true;
  });
  // a row of small text fits below the standard height (8 pt: 11.25 pt); otherwise the standard height is the least
  if (best <= 0) return -1;
  const int32_t fit = static_cast<int32_t>(std::ceil(best)) + 2;
  return small ? fit : std::max<int32_t>(sh.rows->default_size(), fit);
}

}  // namespace

void AutoFitDims(AppState& s, uint32_t sheet, bool cols, const std::vector<std::pair<uint32_t, uint32_t>>& spans, const std::vector<Rect>* rows) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || !s.engine) return;
  // every row / column once, limited to the used range (a whole sheet selected fits at once), one undo step
  std::vector<std::pair<uint32_t, uint32_t>> todo;
  for (auto [lo, hi] : spans) {
    if (cols) {
      if (sh->has_used) hi = std::min(hi, sh->used.c1);
    } else {
      hi = std::min(hi, sh->has_used ? sh->used.r1 : lo);
    }
    if (lo <= hi) todo.emplace_back(lo, hi);
  }
  std::sort(todo.begin(), todo.end());
  std::vector<std::pair<uint32_t, int32_t>> sizes;
  int64_t next = 0;  // overlapping spans are measured once
  for (auto [lo, hi] : todo) {
    for (int64_t i = std::max<int64_t>(lo, next); i <= hi && sizes.size() < 100000; ++i) {
      const uint32_t k = static_cast<uint32_t>(i);
      if (cols) {
        const int32_t w = MeasureColumn(s, *sh, k, rows);
        if (w > 0) sizes.emplace_back(k, w);
      } else if (!sh->rows->IsHidden(k)) {
        // each row to its tallest cell (rows without text go back to the standard height)
        const int32_t h = MeasureRow(s, *sh, k);
        sizes.emplace_back(k, h > 0 ? h : sh->rows->default_size());
      }
    }
    next = std::max<int64_t>(next, int64_t{hi} + 1);
  }
  if (sizes.empty()) return;
  s.engine->PostUser(cols ? TR("AutoFit Column Width") : TR("AutoFit Row Height"), [=](Document& d) {
    for (auto [k, px] : sizes) {
      if (cols) d.SetColWidth(sheet, k, k, px);
      else d.SetRowHeight(sheet, k, k, px);
    }
  });
}

namespace {

}  // namespace

void HandleGridActions(AppState& s, uint32_t sheet_id, const render::GridActions& act) {
  if (act.clicked) s.focus_sheet = sheet_id;
  // Enter or Escape for a formula typed on another sheet (pointing at this one): back to that sheet first, where the
  // entry is written and the active cell moves (Excel)
  if ((act.commit_edit || act.cancel_edit) && s.edit.active && s.edit.sheet != sheet_id && s.snap && s.snap->Sheet(s.edit.sheet)) {
    s.active_sheet = s.focus_sheet = s.edit.sheet;
  }
  if (act.commit_edit) {
    CommitEdit(s, act.commit_array, act.commit_move, act.commit_cse);
    // a navigation key ended it in Enter mode: it acts on the sheet once the editor is gone (Excel commits, then moves)
    if (act.nav_key != ImGuiKey_None && !s.edit.active) s.FocusView().nav_replay = {act.nav_key, act.nav_ctrl, act.nav_shift, ImGui::GetFrameCount()};
  }
  if (act.cancel_edit) CancelEdit(s);
  AddTableRowAfterTab(s, sheet_id, act.table_add_row);
  if (act.eval_part) EvaluateEditPart(s, act.eval_begin, act.eval_end);
  if (act.fn_args) OpenFunctionArgsForCell(s);
  if (act.clear_contents) {
    ClearSelection(s, 0);
    ui::Commands().NoteLast("ClearContents");  // F4 repeats it (Excel 16, measured: Delete, Down, F4 clears that cell)
  }
  if (act.paste_enter) {
    PasteClipboard(s, engine::PasteMode::All, false, false);
    s.marquee = false;
  }
  if (act.move_menu && s.engine) OpenMoveDropMenu(s, sheet_id, act.move_src, act.move_r, act.move_c);
  if (act.move_cells && s.engine && act.move_insert) {
    // Shift: the cells go in between rows / columns there, the others make room (Excel); a move closes up their place
    if (act.move_copy || !RefusePartOfArray(s, sheet_id, {act.move_src})) InsertDrop(s, sheet_id, act.move_src, act.move_insert, act.move_r, act.move_c, act.move_copy);
  } else if (act.move_cells && s.engine && !act.move_copy && PivotMoveCells(s, sheet_id, act.move_src, act.move_r, act.move_c)) {
    // a PivotTable's label dragged to another label: the item moves there (Excel)
  } else if (act.move_cells && s.engine) {
    // drag and drop: move (formulas pointing at the cells follow them) or, with Ctrl, copy
    const Rect src = act.move_src;
    const uint32_t r = act.move_r, c = act.move_c;
    AppState* sp = &s;
    std::function<void()> drop;
    if (act.move_copy) {
      drop = [sp, sheet_id, r, c, src]() { sp->engine->PostUser(TR("Copy"), [=](Document& d) { d.Paste(sheet_id, r, c, d.Copy(sheet_id, src)); }); };
    } else if (!RefusePartOfArray(s, sheet_id, {src})) {
      drop = [sp, sheet_id, r, c, src]() { sp->engine->PostUser(TR("Move"), [=](Document& d) { d.MoveCells(sheet_id, src, sheet_id, r, c); }); };
    }
    // Excel asks before dropping onto cells that hold something (outside the cells being moved)
    bool filled = false;
    if (const model::Sheet* dsh = s.snap ? s.snap->Sheet(sheet_id) : nullptr; drop && dsh) {
      for (uint32_t rr = r; rr <= r + (src.r1 - src.r0) && !filled && rr - r < 2000; ++rr) {
        for (uint32_t cc = c; cc <= c + (src.c1 - src.c0) && !filled && cc - c < 500; ++cc) {
          if (!act.move_copy && src.Contains(rr, cc)) continue;
          filled = !dsh->IsEmptyCell(rr, cc);
        }
      }
    }
    // (unless Options > Advanced > "Alert before overwriting cells" is off: Application.AlertBeforeOverwriting)
    if (drop && filled && Config().alert_before_overwriting) AskYesNo(s, "YCGrid", TR("There's already data here. Do you want to replace it?"), drop);
    else if (drop) drop();
    s.audit_arrows.clear();  // like Excel: editing removes the tracer arrows
  }
  if (act.outline_level > 0 && s.engine) {
    const bool rows = act.outline_level_rows;
    const int level = act.outline_level;
    s.engine->PostUser(TR("Show Levels"), [=](Document& d) { d.ShowOutlineLevel(sheet_id, rows, level); });
  }
  if (act.outline_toggle && s.engine) {
    const bool rows = act.outline_rows, collapse = act.outline_collapse;
    const uint32_t index = act.outline_index;
    const int level = act.outline_toggle_level;
    s.engine->PostUser(collapse ? TR("Hide Detail") : TR("Show Detail"), [=](Document& d) { d.SetGroupCollapsed(sheet_id, rows, index, level, collapse); });
  }
  if (act.fill_menu && s.engine) OpenFillDropMenu(s, sheet_id, act.autofill_src, act.autofill_dest);
  if (act.fill_shift && s.engine) {
    // Shift-drag of the fill handle: the band dragged over is inserted (out) or deleted (back in), as in Excel
    const Rect band = act.fill_shift_rect;
    const bool ins = act.fill_shift == 1, down = act.fill_shift_down;
    s.engine->PostUser(ins ? TR("Insert") : TR("Delete"), [=](Document& d) {
      if (ins) d.InsertCells(sheet_id, band, down);
      else d.DeleteCells(sheet_id, band, down);
    });
  }
  if (act.autofill && s.engine) {
    const Rect src = act.autofill_src, dst = act.autofill_dest;
    NoteAutoFill(s, sheet_id, src, dst);
    // Ctrl as the button goes up: the other kind of fill (a copy becomes a series and a series copies), as in Excel
    const int type = act.autofill_ctrl && dst.Contains(src) ? (s.last_fill.series ? 1 : 2) : 0;
    if (type) s.last_fill.type = type;
    // a filtered list: the rows the filter hides keep what they hold (Excel fills the cells it shows)
    std::vector<Rect> hidden;
    if (const model::Sheet* fsh = s.snap ? s.snap->Sheet(sheet_id) : nullptr; fsh && dst.Contains(src) && fsh->autofilter->on) {
      const Rect added = dst.r1 > src.r1 ? Rect{src.r1 + 1, dst.c0, dst.r1, dst.c1} : dst.r0 < src.r0 ? Rect{dst.r0, dst.c0, src.r0 - 1, dst.c1} : Rect{};
      if (added.r1 >= added.r0 && (dst.r1 > src.r1 || dst.r0 < src.r0)) {
        for (uint32_t r = added.r0; r <= added.r1; ++r) {
          if (!engine::FilterHidRow(*fsh, r)) continue;
          if (!hidden.empty() && hidden.back().r1 + 1 == r) hidden.back().r1 = r;
          else hidden.push_back(Rect{r, added.c0, r, added.c1});
        }
      }
    }
    s.engine->PostUser(TR("AutoFill"), [=](Document& d) {
      if (dst.Contains(src)) {
        std::vector<engine::ClipData> kept;
        for (const Rect& h : hidden) kept.push_back(d.Copy(sheet_id, h));
        d.AutoFill(sheet_id, src, dst, type);
        for (size_t i = 0; i < hidden.size(); ++i) {
          d.ClearAll(sheet_id, hidden[i], engine::kSrcUser, true);
          d.Paste(sheet_id, hidden[i].r0, hidden[i].c0, kept[i]);
        }
      } else {
        // dragged back inside the selection: only the cells dragged over are cleared (Excel)
        if (dst.r1 < src.r1) d.ClearContents(sheet_id, Rect{dst.r1 + 1, src.c0, src.r1, src.c1});
        else if (dst.c1 < src.c1) d.ClearContents(sheet_id, Rect{src.r0, dst.c1 + 1, src.r1, src.c1});
      }
    });
  }
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (act.resize_kind && sh && s.engine) {
    // the dragged row/column is inside selected entire rows/columns: apply to all selected
    auto& sel = (s.focus_sheet == sheet_id ? s.FocusView() : s.Win(sheet_id).view).sel;
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    const bool cols = act.resize_kind == 1;
    bool in_sel = false;
    if ((cols && sel.mode == render::SelectMode::Cols) || (!cols && sel.mode == render::SelectMode::Rows) || sel.mode == render::SelectMode::All) {
      for (const Rect& a : sel.areas) {
        const uint32_t lo = cols ? a.c0 : a.r0, hi = cols ? a.c1 : a.r1;
        if (act.resize_index >= lo && act.resize_index <= hi) in_sel = true;
      }
      if (in_sel) {
        for (const Rect& a : sel.areas) spans.emplace_back(cols ? a.c0 : a.r0, cols ? a.c1 : a.r1);
        if (sel.mode == render::SelectMode::All) spans.assign(1, {0, cols ? sh->col_limit - 1 : sh->row_limit - 1});
      }
    }
    if (!in_sel || act.resize_hidden) spans.assign(1, {act.resize_index, act.resize_index});
    const std::vector<uint32_t> group = GroupTargets(s, sheet_id);  // grouped sheets get the width / height too (Excel)
    if (act.autofit && act.resize_hidden) {
      // a double-click just past hidden columns / rows unhides the last of them, fitted to its contents (Excel)
      const uint32_t k = act.resize_index;
      const int32_t fit = cols ? MeasureColumn(s, *sh, k, nullptr) : MeasureRow(s, *sh, k);
      const int32_t px = fit > 0 ? fit : (cols ? sh->cols->default_size() : sh->rows->default_size());
      s.engine->PostUser(cols ? TR("Unhide Columns") : TR("Unhide Rows"), [=](Document& d) {
        if (cols) d.SetColWidth(sheet_id, k, k, px);
        else d.SetRowHeight(sheet_id, k, k, px);
      });
    } else if (act.autofit) {
      AutoFitDims(s, sheet_id, cols, spans);
    } else {
      const int32_t px = act.resize_px;
      s.engine->PostUser(cols ? TR("Column Width") : TR("Row Height"), [=](Document& d) {
        for (const uint32_t g : group) {
          for (auto [lo, hi] : spans) {
            if (cols) d.SetColWidth(g, lo, hi, px);
            else d.SetRowHeight(g, lo, hi, px);
          }
        }
      });
    }
  }
  if (act.context_cells && !s.suppress_cell_menu) {
    // Worksheet_BeforeRightClick(Target = the selection, Cancel): the menu opens unless a handler cancels
    const std::vector<Rect> target = (s.focus_sheet == sheet_id ? s.FocusView() : s.Win(sheet_id).view).sel.areas;
    AskThenAct(s, "Worksheet_BeforeRightClick", sheet_id, &target, io::MV::Map(), [sheet_id](AppState& st) {
      if (st.focus_sheet == sheet_id) OpenCellContextMenu();
    });
  }
  s.suppress_cell_menu = false;
  if (act.context_col_header) OpenColHeaderContextMenu();
  if (act.context_row_header) OpenRowHeaderContextMenu();
  // a double-click in a PivotTable report shows the details of a value instead of editing
  // a cell with a click action (click-to-trade) works like a button: quick clicks are each a click (an order), and
  // a double-click must not open the editor, which would take the clicks after it
  bool click_action = false;
  if (act.double_click_cell) {
    const auto& sel = s.FocusView().sel;
    for (const auto& cr : s.click_ranges) click_action = click_action || (cr.left && cr.sheet == s.focus_sheet && cr.rect.Contains(sel.act_r, sel.act_c));
  }
  if (click_action) {
  } else if (act.double_click_cell) {
    // Worksheet_BeforeDoubleClick(Target = the cell, Cancel) comes first: a handler that sets Cancel keeps the cell out of
    // edit mode (and a PivotTable from showing details), as in Excel
    const auto& sel = s.FocusView().sel;
    const uint32_t r = sel.act_r, c = sel.act_c;
    Rect cell = Rect::Cell(r, c);
    if (sh) {
      if (const Rect* mg = sh->MergeAt(r, c)) cell = *mg;
    }
    const std::vector<Rect> target{cell};
    const float click_x = act.click_x;
    AskThenAct(s, "Worksheet_BeforeDoubleClick", sheet_id, &target, io::MV::Map(), [sheet_id, r, c, click_x](AppState& st) {
      const auto& now = st.FocusView().sel;
      if (st.focus_sheet != sheet_id || now.act_r != r || now.act_c != c) return;  // moved on meanwhile
      if (PivotDoubleClick(st)) return;
      StartEdit(st, false, {}, !Config().edit_directly_in_cell);
      // a double-click puts the caret on the character clicked (the grid works it out on the editor's first frame)
      if (st.edit.active && !st.edit.in_formula_bar) st.edit.click_x = click_x;
    });
  } else if (act.start_edit_f2) {
    // File > Options: with editing directly in cells off, F2 and a double-click edit in the formula bar
    StartEdit(s, false, {}, !Config().edit_directly_in_cell);
  }
  if (act.start_edit_typing) StartEdit(s, true, act.start_edit_text);
  if (act.clicked && s.format_painter) s.painter_armed = true;
  if (act.clicked && BorderDrawOn()) BorderDrawArm(s);
}

}  // namespace yc::app
