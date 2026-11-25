// SPDX-License-Identifier: Apache-2.0
// document_format.cpp - formatting, borders, merging, row heights and column widths
#include "engine/document.h"
#include "engine/external.h"
#include "formula/parser.h"

#include "engine/tile_iter.h"

#include <algorithm>
#include <cctype>
#include <tuple>
#include <unordered_map>

namespace yc::engine {

using model::Sheet;
using model::Style;

namespace {

// apply fn to every cell in the rectangle (style id -> new style id; each old id is interned only once)
template <class Fn>
void RemapStyles(Sheet& s, const Rect& rect, model::StyleTable& table, Fn&& fn) {
  std::unordered_map<uint32_t, uint32_t> memo;
  auto map = [&](uint32_t old) {
    auto it = memo.find(old);
    if (it != memo.end()) return it->second;
    Style st = table.Get(old);
    fn(st);
    const uint32_t id = table.Intern(st);
    memo.emplace(old, id);
    return id;
  };
  for (uint32_t r = rect.r0; r <= rect.r1; ++r) {
    for (uint32_t c = rect.c0; c <= rect.c1; ++c) {
      const uint32_t old = s.GetStyle(r, c);
      const uint32_t nw = map(old);
      s.ShowStyle(r, c, nw);
    }
  }
}

}  // namespace

void Document::ModifyStyle(uint32_t sheet, const Rect& rect_in, const std::function<void(Style&)>& fn) {
  if (Refuse(sheet, nullptr, Allow::FormatCells)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  Rect rect = rect_in;
  rect.r1 = std::min(rect.r1, s->row_limit - 1);
  rect.c1 = std::min(rect.c1, s->col_limit - 1);
  // entire columns / rows: change the column/row style, then the cells that have their own style (PLAN §5.4)
  const bool whole_cols = rect.r0 == 0 && rect.r1 == s->row_limit - 1;
  const bool whole_rows = rect.c0 == 0 && rect.c1 == s->col_limit - 1;
  if (whole_cols && rect.Cols() <= 16384 && rect.Rows() > 65536) {
    ModifyColStyle(sheet, rect.c0, rect.c1, fn);
    return;
  }
  // columns' cells but a few rows (all but the last row's, or from row 2 down: B2:XFD1048576, Ctrl+Shift+End from B2):
  // through the columns' styles. B2:XFD1048576 was past the 20,000,000 cells below and took no format at all
  if (!whole_cols && rect.Rows() + kFewRowsLeft >= s->row_limit &&
      (whole_rows ? rect.Count() > 1000000 : rect.r1 == s->row_limit - 1 && rect.Count() > 20000000)) {
    ModifyAllButRows(sheet, rect, fn);
    return;
  }
  if (whole_rows && rect.Count() > 1000000) {
    ModifyRowStyle(sheet, rect.r0, rect.r1, fn);
    return;
  }
  if (rect.Count() > 20000000) return;  // safety net against accidental huge operations
  const auto rich_before = RichStyleIds(sheet, rect);
  RemapStyles(*s, rect, wb_.styles.mut(), fn);
  RunsFollowFont(sheet, rich_before, fn);
  NoteFormat(sheet, rect);
  changes_.push_back(CellChange{sheet, rect, kSrcUser});
  wb_.dirty = true;
}

std::vector<std::pair<uint64_t, uint32_t>> Document::RichStyleIds(uint32_t sheet, const Rect& rect) const {
  std::vector<std::pair<uint64_t, uint32_t>> out;
  const Sheet* s = S(sheet);
  if (!s || s->rich->empty()) return out;
  for (const auto& [k, rt] : *s->rich) {
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    if (rect.Contains(r, c) && RichTextAt(sheet, r, c)) out.emplace_back(k, s->GetStyle(r, c));
  }
  return out;
}

void Document::RunsFollowFont(uint32_t sheet, const std::vector<std::pair<uint64_t, uint32_t>>& before,
                              const std::function<void(Style&)>& fn) {
  if (before.empty()) return;
  Sheet* cs = M(sheet);
  if (!cs) return;
  // the font properties fn sets whatever the cell had (a toggle or a step is seen per cell, by what changed)
  Style a, b;
  b.font_name = 1;
  b.font_size = 333.0f;
  b.font_color = 0xFF123456;
  b.bold = b.italic = b.strike = true;
  b.underline = model::Underline::Double;
  b.script = 2;
  Style fa = a, fb = b;
  fn(fa);
  fn(fb);
  const auto& table = *wb_.styles;
  bool any = false;
  std::vector<std::pair<uint64_t, model::RichText>> out;
  for (const auto& [key, old_id] : before) {
    const uint32_t r = static_cast<uint32_t>(key >> 16), c = static_cast<uint32_t>(key & 0xFFFF);
    const Style& was = table.Get(old_id);
    const Style& now = table.Get(cs->GetStyle(r, c));
    const bool name = fa.font_name == fb.font_name || was.font_name != now.font_name;
    const bool size = fa.font_size == fb.font_size || was.font_size != now.font_size;
    const bool color = fa.font_color == fb.font_color || was.font_color != now.font_color;
    const bool bold = fa.bold == fb.bold || was.bold != now.bold;
    const bool italic = fa.italic == fb.italic || was.italic != now.italic;
    const bool strike = fa.strike == fb.strike || was.strike != now.strike;
    const bool underline = fa.underline == fb.underline || was.underline != now.underline;
    const bool script = fa.script == fb.script || was.script != now.script;
    if (!(name || size || color || bold || italic || strike || underline || script)) continue;
    const auto it = cs->rich->find(key);
    if (it == cs->rich->end()) continue;
    model::RichText rt = it->second;
    for (model::RichRun& run : rt.runs) {
      if (name) run.font.clear();
      if (size) run.size = 0;
      if (color) run.color = 0;
      if (bold) run.bold = -1;
      if (italic) run.italic = -1;
      if (strike) run.strike = -1;
      if (underline) run.underline = -1;
      if (script) run.script = -1;
    }
    if (rt == it->second) continue;
    out.emplace_back(key, std::move(rt));
    any = true;
  }
  if (!any) return;
  NoteTouched();
  auto& m = cs->rich.mut();
  for (auto& [key, rt] : out) {
    if (!model::Normalize(&rt)) m.erase(key);
    else m[key] = std::move(rt);
  }
}

void Document::ModifyAllButRows(uint32_t sheet, const Rect& rect, const std::function<void(Style&)>& fn) {
  // every row but a few (the inside horizontal borders of the whole sheet are rows 1:1048575): the whole sheet takes
  // the change through its columns' styles, and the rows left out are given back what they showed. As a style for
  // each of a million rows, every later format of whole columns gave each cell where they crossed a style of its own
  // (17 billion: Center over the whole sheet after its borders froze YCGrid).
  Sheet* s = M(sheet);
  struct Kept {
    uint32_t row;
    bool had_line;
    uint32_t line;
    std::vector<uint32_t> own, shown;
  };
  std::vector<Kept> kept;
  const uint32_t c0 = rect.c0, cols = rect.c1 - rect.c0 + 1;  // the columns changed, k.own[i] is column c0 + i
  auto keep = [&](uint32_t r) {
    Kept k{r, false, 0, std::vector<uint32_t>(cols), std::vector<uint32_t>(cols)};
    if (auto it = s->row_styles->find(r); it != s->row_styles->end()) {
      k.had_line = true;
      k.line = it->second;
    }
    for (uint32_t i = 0; i < cols; ++i) {
      k.own[i] = s->GetOwnStyle(r, c0 + i);
      k.shown[i] = s->GetStyle(r, c0 + i);
    }
    kept.push_back(std::move(k));
  };
  for (uint32_t r = 0; r < rect.r0; ++r) keep(r);
  for (uint32_t r = rect.r1 + 1; r < s->row_limit; ++r) keep(r);
  const size_t refused = protect_blocks_;
  ModifyColStyle(sheet, c0, rect.c1, fn);
  if (protect_blocks_ != refused) return;
  s = M(sheet);
  for (const Kept& k : kept) {
    if (k.had_line) s->row_styles.mut()[k.row] = k.line;
    NoteDims(sheet, true, k.row, k.row);
    for (uint32_t i = 0; i < cols; ++i) {
      if (k.own[i] || k.had_line) s->SetStyle(k.row, c0 + i, k.own[i]);
      else s->ShowStyle(k.row, c0 + i, k.shown[i]);
    }
  }
}

void Document::ModifyRowStyle(uint32_t sheet, uint32_t r0, uint32_t r1, const std::function<void(Style&)>& fn) {
  if (Refuse(sheet, nullptr, Allow::FormatRows)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  const auto rich_before = RichStyleIds(sheet, Rect{r0, 0, r1, core::kMaxCols - 1});
  auto& table = wb_.styles.mut();
  auto& rs = s->row_styles.mut();
  for (uint32_t r = r0; r <= r1; ++r) {
    Style st = table.Get(rs.count(r) ? rs[r] : 0);
    fn(st);
    rs[r] = table.Intern(st);
  }
  // cells with their own style need changing too
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  const Rect band{r0, 0, r1, core::kMaxCols - 1};
  ForEachCellInTiles(s->meta, band, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.style[idx]) cells.emplace_back(r, c);
  });
  for (auto [r, c] : cells) {
    Style st = table.Get(s->GetOwnStyle(r, c));
    fn(st);
    s->ShowStyle(r, c, table.Intern(st));
  }
  RunsFollowFont(sheet, rich_before, fn);
  NoteDims(sheet, true, r0, r1);
  NoteFormat(sheet, band);
  changes_.push_back(CellChange{sheet, band, kSrcUser});
  wb_.dirty = true;
}

void Document::ModifyColStyle(uint32_t sheet, uint32_t c0, uint32_t c1, const std::function<void(Style&)>& fn) {
  if (Refuse(sheet, nullptr, Allow::FormatColumns)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  const auto rich_before = RichStyleIds(sheet, Rect{0, c0, s->row_limit - 1, c1});
  auto& table = wb_.styles.mut();
  auto& cs = s->col_styles.mut();
  for (uint32_t c = c0; c <= c1; ++c) {
    Style st = table.Get(cs.count(c) ? cs[c] : 0);
    fn(st);
    cs[c] = table.Intern(st);
  }
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  const Rect band{0, c0, s->row_limit - 1, c1};
  ForEachCellInTiles(s->meta, band, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.style[idx]) cells.emplace_back(r, c);
  });
  for (auto [r, c] : cells) {
    Style st = table.Get(s->GetOwnStyle(r, c));
    fn(st);
    s->ShowStyle(r, c, table.Intern(st));
  }
  // every column: each row's own style takes the change, which is what its cells would get one by one (over the whole
  // sheet after borders on rows 1:1048575, the crossing cells were 17 billion and YCGrid froze)
  const bool all_cols = c0 == 0 && c1 >= s->col_limit - 1;
  if (all_cols && !s->row_styles->empty()) {
    auto& rs = s->row_styles.mut();
    NoteDims(sheet, true, rs.begin()->first, rs.rbegin()->first);
    for (auto& [r, sid] : rs) {
      Style st = table.Get(sid);
      fn(st);
      sid = table.Intern(st);
    }
  }
  // where a row style meets a column: the row style wins, so the crossing cells get their own style (like Excel)
  if (!all_cols) for (const auto& [r, sid] : *s->row_styles) {
    for (uint32_t c = c0; c <= c1; ++c) {
      if (s->GetOwnStyle(r, c)) continue;
      Style st = table.Get(sid);
      fn(st);
      s->ShowStyle(r, c, table.Intern(st));
    }
  }
  RunsFollowFont(sheet, rich_before, fn);
  NoteDims(sheet, false, c0, c1);
  NoteFormat(sheet, band);
  if (!all_cols) for (const auto& [r, sid] : *s->row_styles) NoteFormat(sheet, Rect{r, c0, r, c1});  // the crossing cells
  changes_.push_back(CellChange{sheet, band, kSrcUser});
  wb_.dirty = true;
}

void Document::SetBorders(uint32_t sheet, const Rect& rect, BorderPreset p, model::LineStyle line, uint32_t color) {
  if (Refuse(sheet, nullptr, Allow::FormatCells)) return;
  using model::BorderSide;
  using model::LineStyle;
  const BorderSide thin{line, color};
  const BorderSide none{LineStyle::None, 0};
  const BorderSide thick{LineStyle::Thick, color};
  const BorderSide dbl{LineStyle::Double, color};
  Sheet* s = M(sheet);
  if (!s) return;
  // each preset is a few rectangles that take the same sides (the outside: four strips one cell wide), each set
  // through ModifyStyle, which changes whole columns' and rows' own styles: a loop over every cell took Ctrl+Shift+7
  // over the whole sheet through 17 billion cells (basics seq268600), and over whole columns B:C two million
  Rect rc = rect;
  rc.r1 = std::min(rc.r1, s->row_limit - 1);
  rc.c1 = std::min(rc.c1, s->col_limit - 1);
  if (rc.r0 > rc.r1 || rc.c0 > rc.c1) return;
  using Sides = std::vector<std::pair<int, BorderSide>>;
  auto apply = [&](const Rect& a, const Sides& sides) {
    if (a.r0 > a.r1 || a.c0 > a.c1) return;
    ModifyStyle(sheet, a, [sides](Style& st) {
      for (const auto& [e, b] : sides) st.border[e] = b;
    });
  };
  const Rect top_row{rc.r0, rc.c0, rc.r0, rc.c1}, bottom_row{rc.r1, rc.c0, rc.r1, rc.c1};
  const Rect left_col{rc.r0, rc.c0, rc.r1, rc.c0}, right_col{rc.r0, rc.c1, rc.r1, rc.c1};
  switch (p) {
    case BorderPreset::None: {
      Sides all;
      for (int e = 0; e < model::kEdgeCount; ++e) all.emplace_back(e, none);
      apply(rc, all);
      // also clear the facing edges of neighbouring cells (in Excel a border belongs to both cells)
      if (rc.r0 > 0) apply(Rect{rc.r0 - 1, rc.c0, rc.r0 - 1, rc.c1}, {{model::kBottom, none}});
      if (rc.c0 > 0) apply(Rect{rc.r0, rc.c0 - 1, rc.r1, rc.c0 - 1}, {{model::kRight, none}});
      if (rc.r1 + 1 < s->row_limit) apply(Rect{rc.r1 + 1, rc.c0, rc.r1 + 1, rc.c1}, {{model::kTop, none}});
      if (rc.c1 + 1 < s->col_limit) apply(Rect{rc.r0, rc.c1 + 1, rc.r1, rc.c1 + 1}, {{model::kLeft, none}});
      break;
    }
    case BorderPreset::All:
      apply(rc, {{model::kTop, thin}, {model::kBottom, thin}, {model::kLeft, thin}, {model::kRight, thin}});
      break;
    case BorderPreset::Outside:
    case BorderPreset::ThickOutside: {
      const BorderSide& b = p == BorderPreset::Outside ? thin : thick;
      apply(top_row, {{model::kTop, b}});
      apply(bottom_row, {{model::kBottom, b}});
      apply(left_col, {{model::kLeft, b}});
      apply(right_col, {{model::kRight, b}});
      break;
    }
    case BorderPreset::Inside:
    case BorderPreset::InsideH:
    case BorderPreset::InsideV: {
      // whole columns take the horizontal lines on every cell, top and bottom, the first and last rows too, and whole
      // rows the vertical ones (Excel 16, measured: B1's top and D1048576's bottom after B:D's inside horizontal
      // borders); a block has them between its cells only
      const bool whole_cols = rc.r0 == 0 && rc.r1 == s->row_limit - 1;
      const bool whole_rows = rc.c0 == 0 && rc.c1 == s->col_limit - 1;
      if (p != BorderPreset::InsideV && whole_cols) apply(rc, {{model::kTop, thin}, {model::kBottom, thin}});
      else if (p != BorderPreset::InsideV && rc.r1 > rc.r0) apply(Rect{rc.r0, rc.c0, rc.r1 - 1, rc.c1}, {{model::kBottom, thin}});
      if (p != BorderPreset::InsideH && whole_rows) apply(rc, {{model::kLeft, thin}, {model::kRight, thin}});
      else if (p != BorderPreset::InsideH && rc.c1 > rc.c0) apply(Rect{rc.r0, rc.c0, rc.r1, rc.c1 - 1}, {{model::kRight, thin}});
      break;
    }
    case BorderPreset::Top: apply(top_row, {{model::kTop, thin}}); break;
    case BorderPreset::Bottom: apply(bottom_row, {{model::kBottom, thin}}); break;
    case BorderPreset::Left: apply(left_col, {{model::kLeft, thin}}); break;
    case BorderPreset::Right: apply(right_col, {{model::kRight, thin}}); break;
    case BorderPreset::DoubleBottom: apply(bottom_row, {{model::kBottom, dbl}}); break;
    case BorderPreset::ThickBottom: apply(bottom_row, {{model::kBottom, thick}}); break;
    case BorderPreset::TopBottom:
      apply(top_row, {{model::kTop, thin}});
      apply(bottom_row, {{model::kBottom, thin}});
      break;
    case BorderPreset::TopThickBottom:
      apply(top_row, {{model::kTop, thin}});
      apply(bottom_row, {{model::kBottom, thick}});
      break;
    case BorderPreset::TopDoubleBottom:
      apply(top_row, {{model::kTop, thin}});
      apply(bottom_row, {{model::kBottom, dbl}});
      break;
    case BorderPreset::DiagDown: apply(rc, {{model::kDiagDown, thin}}); break;
    case BorderPreset::DiagUp: apply(rc, {{model::kDiagUp, thin}}); break;
  }
  Rect touched = rc;
  if (touched.r0 > 0) --touched.r0;
  if (touched.c0 > 0) --touched.c0;
  touched.r1 = std::min(touched.r1 + 1, s->row_limit - 1);
  touched.c1 = std::min(touched.c1 + 1, s->col_limit - 1);
  NoteFormat(sheet, touched);
  changes_.push_back(CellChange{sheet, touched, kSrcUser});
  wb_.dirty = true;
}

bool Document::Merge(uint32_t sheet, const Rect& rect_in, bool across, bool center, bool property) {
  // a range that cuts a merged cell grows to take all of it, as Excel 16 does (measured: B3:D4 merged over B2:C3
  // merges B2:D4, keeping B2's value)
  Rect rect = rect_in;
  if (const Sheet* s0 = S(sheet); s0 && !s0->merges->empty()) {
    for (bool grew = true; grew;) {
      grew = false;
      for (const Rect& x : *s0->merges) {
        if (x.Intersects(rect) && !rect.Contains(x)) {
          rect = rect.Union(x);
          grew = true;
        }
      }
    }
  }
  if (Refuse(sheet, &rect, Allow::FormatCells)) return false;
  if (!S(sheet) || rect.IsCell()) return false;
  // a table holds no merged cells: Excel 16 refuses Range.Merge over any of a table's cells (measured) and grays the
  // ribbon's merge buttons there. A merge after rows moved a table under it made one that cut the table (doc_fuzz
  // seed 31000041, found by its invariants)
  for (const auto& t : *S(sheet)->tables) {
    if (!t.range.Intersects(rect)) continue;
    CountBlock(BlockReason::TableMerge);
    block_table_merge_ = true;
    block_sheet_ = sheet;
    block_cells_ = rect;
    block_range_ = -1;
    block_python_ = python_call;
    return false;
  }
  // the sheet's own rows and columns (a whole column's end marker would make Merge Across two billion rows)
  rect.r1 = std::min(rect.r1, S(sheet)->row_limit - 1);
  rect.c1 = std::min(rect.c1, S(sheet)->col_limit - 1);
  if (rect.IsCell() || rect.r0 > rect.r1 || rect.c0 > rect.c1) return false;
  std::vector<Rect> parts;
  if (across) {
    for (uint32_t r = rect.r0; r <= rect.r1; ++r) parts.push_back(Rect{r, rect.c0, r, rect.c1});
  } else {
    parts.push_back(rect);
  }
  // arrays entered with Ctrl+Shift+Enter (Excel 16, measured): a merge empties the cells after its first as two
  // blocks, the rest of the first row and the rows below. One that cuts an array is refused ("You can't change part of
  // an array.": over part of it, over exactly it, or with the array in its first row); an array a block holds whole
  // is cleared, and its value isn't the one the merged cell keeps (doc_fuzz seed 10110000: the merged cell's value
  // was the array's, and the array came back on open)
  std::vector<Rect> blocks;
  for (const Rect& p : parts) {
    if (p.c1 > p.c0) blocks.push_back(Rect{p.r0, p.c0 + 1, p.r0, p.c1});
    if (p.r1 > p.r0) blocks.push_back(Rect{p.r0 + 1, p.c0, p.r1, p.c1});
  }
  for (const Rect& b : blocks) {
    if (RefusePartOfArray(sheet, b)) return false;
  }
  {
    std::vector<Rect> whole;
    for (const auto& [k, sp] : *S(sheet)->spills) {
      if (!sp.cse || sp.rect.IsCell()) continue;
      if (std::any_of(blocks.begin(), blocks.end(), [&](const Rect& b) { return b.Contains(sp.rect); })) whole.push_back(sp.rect);
    }
    for (const Rect& a : whole) ClearContents(sheet, a, kSrcUser);
  }
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& m = s->merges.mut();
  // existing merged areas that overlap are removed first
  m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return x.Intersects(rect); }), m.end());
  // the cells that hold something, by the part they are in (a row for Merge Across): one walk over the area's tiles.
  // A loop over every cell never ended for a whole-sheet merge (basics seq280931), and a walk a part took Merge Across
  // through a million rows of tiles
  std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> filled;
  {
    auto note = [&](uint32_t r, uint32_t c) { filled[across ? r - rect.r0 : 0].emplace_back(r, c); };
    ForEachCellInTiles(s->values, rect, [&](uint32_t r, uint32_t c, const model::ValueTile& t, uint32_t idx) {
      if (t.Occupied(idx)) note(r, c);
    });
    ForEachCellInTiles(s->meta, rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (t.formula[idx]) note(r, c);
    });
    for (auto& [k, v] : filled) {
      std::sort(v.begin(), v.end());  // reading order: row, then column
      v.erase(std::unique(v.begin(), v.end()), v.end());
    }
  }
  // each cell's formats and borders are worked out for an area of up to a million cells; past it they stay
  const bool per_cell_formats = rect.Count() <= 1000000;
  for (const Rect& p : parts) {
    if (p.IsCell()) continue;
    const auto fit = filled.find(across ? p.r0 - rect.r0 : 0);
    static const std::vector<std::pair<uint32_t, uint32_t>> kNone;
    const auto& cells = fit == filled.end() ? kNone : fit->second;
    // Excel 16 (measured): the merged cell keeps the first value in reading order (row by row), moved to the
    // upper-left cell when that one is empty, and every cell of the area takes the formats of the cell the value
    // came from (the upper-left one when all are empty): number format, font, fill, alignment, protection. That is
    // what B1 shows after an UnMerge, a Clear or a copy. Borders follow the area's outline (below).
    const uint32_t vr = cells.empty() ? p.r0 : cells.front().first, vc = cells.empty() ? p.c0 : cells.front().second;
    const uint32_t value_style = s->GetStyle(vr, vc);  // the formats the area takes (the move below clears them)
    if (vr != p.r0 || vc != p.c0) {
      // the value moves as a cut would, and what read it follows it to the upper-left cell (Excel 16, measured
      // 2026-11-21: =C8*2 reads =C7*2 once C7:D8 is merged around C8's value); it was copied and the references
      // stayed on the emptied cell
      // the content moves, the formats stay where they were: the merge's own rules below read them
      const uint32_t anchor_style = s->GetOwnStyle(p.r0, p.c0), own_value_style = s->GetOwnStyle(vr, vc);
      MoveCells(sheet, Rect::Cell(vr, vc), sheet, p.r0, p.c0);
      s = M(sheet);
      if (!s) return false;
      s->SetStyle(p.r0, p.c0, anchor_style);
      s->SetStyle(vr, vc, own_value_style);
    }
    // the merged area is recorded after the value's move, which would have cut it
    s->merges.mut().push_back(p);
    if (per_cell_formats) {
      const uint32_t from_style = property ? s->GetOwnStyle(p.r0, p.c0) : value_style;
      auto& table = wb_.styles.mut();
      const Style from = table.Get(from_style);
      // Borders (Excel 16, measured for Merge, Merge Across, Merge & Center and MergeCells = True alike): a side of
      // the area keeps its line only when every cell along it shows the same one (style, weight and color; the
      // cell's own edge, or the facing edge of the cell outside when it has none), and loses it otherwise; the edges
      // between the merged cells are cleared; the diagonals stay only when every cell has the same pair. The cells
      // outside keep theirs, and an UnMerge leaves the borders as they are.
      using model::BorderSide;
      auto norm = [](const BorderSide& b) { return b.style == model::LineStyle::None ? BorderSide{} : b; };
      auto shown = [&](uint32_t r, uint32_t c, int e) {
        const BorderSide b = norm(table.Get(s->GetStyle(r, c)).border[e]);
        if (b.style != model::LineStyle::None) return b;
        if (e == model::kLeft && c == p.c0 && c > 0) return norm(table.Get(s->GetStyle(r, c - 1)).border[model::kRight]);
        if (e == model::kTop && r == p.r0 && r > 0) return norm(table.Get(s->GetStyle(r - 1, c)).border[model::kBottom]);
        if (e == model::kRight && c == p.c1 && c + 1 < s->col_limit) return norm(table.Get(s->GetStyle(r, c + 1)).border[model::kLeft]);
        if (e == model::kBottom && r == p.r1 && r + 1 < s->row_limit) return norm(table.Get(s->GetStyle(r + 1, c)).border[model::kTop]);
        return b;
      };
      auto uniform = [&](int e, uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
        const BorderSide first = shown(r0, c0, e);
        for (uint32_t r = r0; r <= r1; ++r) {
          for (uint32_t c = c0; c <= c1; ++c) {
            if (!(shown(r, c, e) == first)) return false;
          }
        }
        return true;
      };
      bool keep[4];
      keep[model::kLeft] = uniform(model::kLeft, p.r0, p.c0, p.r1, p.c0);
      keep[model::kTop] = uniform(model::kTop, p.r0, p.c0, p.r0, p.c1);
      keep[model::kRight] = uniform(model::kRight, p.r0, p.c1, p.r1, p.c1);
      keep[model::kBottom] = uniform(model::kBottom, p.r1, p.c0, p.r1, p.c1);
      const bool keep_diag = uniform(model::kDiagDown, p.r0, p.c0, p.r1, p.c1) && uniform(model::kDiagUp, p.r0, p.c0, p.r1, p.c1);
      // a cell's style and its place on the outline -> the style it takes
      std::unordered_map<uint64_t, uint32_t> made;
      for (uint32_t r = p.r0; r <= p.r1; ++r) {
        for (uint32_t c = p.c0; c <= p.c1; ++c) {
          const bool corner = r == p.r0 && c == p.c0;
          const bool on[4] = {c == p.c0, r == p.r0, c == p.c1, r == p.r1};  // left, top, right, bottom
          const uint32_t own = s->GetStyle(r, c);
          uint64_t key = (static_cast<uint64_t>(own) << 5) | (corner ? 16u : 0u);
          for (int e = 0; e < 4; ++e) key |= (on[e] ? 1u : 0u) << e;
          auto it = made.find(key);
          if (it == made.end()) {
            // MergeCells = True: the upper-left cell keeps its formats and the others are Normal; Merge: every cell
            // takes the value cell's formats
            Style st = property ? table.Get(corner ? own : 0) : from;
            const Style was = table.Get(own);
            for (int e = 0; e < 4; ++e) st.border[e] = on[e] && keep[e] ? was.border[e] : BorderSide{};
            st.border[model::kDiagDown] = keep_diag ? was.border[model::kDiagDown] : BorderSide{};
            st.border[model::kDiagUp] = keep_diag ? was.border[model::kDiagUp] : BorderSide{};
            const uint32_t id = st == table.Get(0) ? 0 : table.Intern(st);
            it = made.emplace(key, id).first;
          }
          s->ShowStyle(r, c, it->second);
        }
      }
    }
    // only keep the upper-left value (like Excel: the other cells are cleared): the cells that held something
    for (auto [r, c] : cells) {
      if (r == p.r0 && c == p.c0) continue;
      if (s->GetFormula(r, c)) {
        UnregisterFormulaCell(sheet, r, c);
        s->SetFormula(r, c, 0);
      }
      if (!s->GetValue(r, c).IsEmpty()) s->SetValue(r, c, Value());
    }
  }
  s->Touch(rect.r0, rect.c0);
  s->Touch(rect.r1, rect.c1);
  if (center) {
    ModifyStyle(sheet, rect, [](Style& st) { st.halign = model::HAlign::Center; });
  }
  NoteFormat(sheet, rect);
  MarkChanged(sheet, rect, kSrcUser);
  return true;
}

void Document::AddMergedArea(uint32_t sheet, const Rect& rect) {
  Sheet* s = M(sheet);
  if (!s || rect.IsCell()) return;
  auto& m = s->merges.mut();
  m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return x.Intersects(rect); }), m.end());
  m.push_back(rect);
  s->Touch(rect.r0, rect.c0);
  s->Touch(rect.r1, rect.c1);
  NoteFormat(sheet, rect);
  MarkChanged(sheet, rect, kSrcUser);
}

void Document::Unmerge(uint32_t sheet, const Rect& rect) {
  if (Refuse(sheet, &rect, Allow::FormatCells)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  auto& m = s->merges.mut();
  m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return x.Intersects(rect); }), m.end());
  NoteFormat(sheet, rect);
  changes_.push_back(CellChange{sheet, rect, kSrcUser});
  wb_.dirty = true;
}

void Document::SetRowHeight(uint32_t sheet, uint32_t r0, uint32_t r1, int32_t px) {
  if (Refuse(sheet, nullptr, Allow::FormatRows)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  r1 = std::min(r1, s->row_limit - 1);  // rows past the sheet's last one aren't there (see HideRows)
  if (r0 > r1) return;
  // the same height again (a click on a row border without a drag) changes nothing and leaves no Undo step: a
  // double-click to AutoFit made two, "Row Height" and "AutoFit Row Height" (Excel: one)
  bool same = true;
  for (uint32_t r = r0; r <= r1 && same; ++r) same = s->rows->Size(r) == std::max(0, px);
  if (same) return;
  auto& d = s->rows.mut();
  for (uint32_t r = r0; r <= r1; ++r) d.Set(r, std::max(0, px));
  NoteDims(sheet, true, r0, r1);
  changes_.push_back(CellChange{sheet, Rect{r0, 0, r1, core::kMaxCols - 1}, kSrcUser});
  wb_.dirty = true;
}

void Document::SetColWidth(uint32_t sheet, uint32_t c0, uint32_t c1, int32_t px) {
  if (Refuse(sheet, nullptr, Allow::FormatColumns)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  c1 = std::min(c1, core::kMaxCols - 1);
  if (c0 > c1) return;
  bool same = true;  // the same width again: nothing changes, no Undo step (see SetRowHeight)
  for (uint32_t c = c0; c <= c1 && same; ++c) same = s->cols->Size(c) == std::max(0, px);
  if (same) return;
  auto& d = s->cols.mut();
  for (uint32_t c = c0; c <= c1; ++c) d.Set(c, std::max(0, px));
  NoteDims(sheet, false, c0, c1);
  changes_.push_back(CellChange{sheet, Rect{0, c0, s->row_limit - 1, c1}, kSrcUser});
  wb_.dirty = true;
}

void Document::SetStandardWidth(uint32_t sheet, int32_t px) {
  if (Refuse(sheet, nullptr, Allow::FormatColumns)) return;
  Sheet* s = M(sheet);
  if (!s || px <= 0 || s->cols->default_size() == px) return;
  NoteTouched();  // every column that follows the default changes: undone with the whole version
  s->cols.mut().set_default_size(px);
  changes_.push_back(CellChange{sheet, Rect{0, 0, s->row_limit - 1, core::kMaxCols - 1}, kSrcUser});
  wb_.dirty = true;
}

void Document::HideRows(uint32_t sheet, uint32_t r0, uint32_t r1, bool hidden) {
  if (Refuse(sheet, nullptr, Allow::FormatRows)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  // rows past the sheet's last one aren't there: Rows.Hidden = False sent rows to 2^31 (seconds) or to the end
  // marker 0xFFFFFFFF, where r <= r1 never ended
  r1 = std::min(r1, s->row_limit - 1);
  if (r0 > r1) return;
  auto& d = s->rows.mut();
  for (uint32_t r = r0; r <= r1; ++r) d.Hide(r, hidden);
  NoteDims(sheet, true, r0, r1);
  const Rect band{r0, 0, r1, core::kMaxCols - 1};
  // SUBTOTAL 101-111 depends on hidden rows: recalculate dependents (no cell was written: spills stay)
  MarkRowsColsChanged(sheet, band);
}

void Document::HideCols(uint32_t sheet, uint32_t c0, uint32_t c1, bool hidden) {
  if (Refuse(sheet, nullptr, Allow::FormatColumns)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  c1 = std::min(c1, core::kMaxCols - 1);
  if (c0 > c1) return;
  auto& d = s->cols.mut();
  for (uint32_t c = c0; c <= c1; ++c) d.Hide(c, hidden);
  NoteDims(sheet, false, c0, c1);
  changes_.push_back(CellChange{sheet, Rect{0, c0, s->row_limit - 1, c1}, kSrcUser});
  wb_.dirty = true;
}

void Document::AutoFitCols(uint32_t sheet, uint32_t c0, uint32_t c1, const std::function<int32_t(const std::string&, const Style&)>& measure) {
  if (Refuse(sheet, nullptr, Allow::FormatColumns)) return;
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return;
  std::vector<int32_t> widths(c1 - c0 + 1, 0);
  const Rect rect{s->used.r0, c0, s->used.r1, c1};
  ForEachValueRowMajor(*s, rect, [&](uint32_t r, uint32_t c, const Value& v) {
    if (s->MergeAt(r, c)) return true;
    const Style& st = wb_.styles->Get(s->GetStyle(r, c));
    const std::string text = DisplayTextOf(v, wb_.styles->NumberFormat(st));
    widths[c - c0] = std::max(widths[c - c0], measure(text, st));
    return true;
  });
  Sheet* ms = M(sheet);
  auto& d = ms->cols.mut();
  for (uint32_t c = c0; c <= c1; ++c) {
    if (widths[c - c0] > 0) d.Set(c, widths[c - c0] + 8);
  }
  NoteDims(sheet, false, c0, c1);
  changes_.push_back(CellChange{sheet, Rect{0, c0, s->row_limit - 1, c1}, kSrcUser});
  wb_.dirty = true;
}


// ---- Themes and named cell styles ----

void Document::SetThemeEffects(const std::string& value) {
  const auto it = wb_.store->find("view.theme_effects");
  if (it != wb_.store->end() && it->second == value) return;
  if (RefuseReadOnly()) return;
  NoteTouched();
  wb_.store.mut()["view.theme_effects"] = value;
  wb_.dirty = true;
}

void Document::SetTheme(const model::Theme& t) {
  if (*wb_.theme == t) return;
  if (RefuseReadOnly()) return;  // read-only mode: themes, cell styles and links stay as they are
  NoteTouched();
  const bool fonts = wb_.theme->minor_font != t.minor_font;
  wb_.theme.reset(t);
  if (fonts) wb_.styles.mut().default_font = t.minor_font;
  for (const auto& sc : wb_.sheets) {
    if (sc->has_used) changes_.push_back(CellChange{sc->id, sc->used, kSrcUser});
  }
  wb_.dirty = true;
}


namespace {
bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}
}  // namespace

std::optional<model::NamedStyle> Document::CellStyleDef(std::string_view name) const {
  for (const auto& ns : *wb_.cell_styles) {
    if (SameName(ns.name, name)) return ns;
  }
  for (const auto& ns : model::BuiltinCellStyles()) {
    if (SameName(ns.name, name)) return ns;
  }
  return std::nullopt;
}

namespace {
// the style ready to apply: its number format code interned, its name id
model::NamedStyle Prepared(model::NamedStyle ns, model::StyleTable& t, uint32_t* name_id) {
  if (!ns.number_format.empty()) ns.style.numfmt = t.InternString(ns.number_format);
  *name_id = ns.name == "Normal" ? 0u : t.InternString(ns.name);
  return ns;
}
}  // namespace

bool Document::ApplyCellStyle(uint32_t sheet, const Rect& rect, std::string_view name) {
  if (RefuseReadOnly(sheet)) return false;
  const auto def = CellStyleDef(name);
  if (!def) return false;
  uint32_t id = 0;
  const model::NamedStyle ns = Prepared(*def, wb_.styles.mut(), &id);
  ModifyStyle(sheet, rect, [&](Style& x) { model::ApplyNamedStyle(x, ns, id); });
  return true;
}

void Document::RestyleNamed(uint32_t name_id, const std::function<void(Style&)>& fn) {
  auto& table = wb_.styles.mut();
  std::unordered_map<uint32_t, uint32_t> memo;
  auto follow = [&](uint32_t sid) -> uint32_t {
    if (sid == 0 || sid == model::kStyleOwnNormal || table.Get(sid).cell_style != name_id) return sid;
    auto it = memo.find(sid);
    if (it != memo.end()) return it->second;
    Style x = table.Get(sid);
    fn(x);
    return memo[sid] = table.Intern(x);
  };
  for (auto& sc : wb_.sheets) {
    const Sheet& cs = sc.get();
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> moves;
    ForEachCellInTiles(cs.meta, Rect{0, 0, cs.row_limit - 1, cs.col_limit - 1}, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (const uint32_t n = follow(t.style[idx]); n != t.style[idx]) moves.emplace_back(r, c, n);
    });
    bool rowcol = false;
    for (const auto& [k, sid] : *cs.row_styles) rowcol = rowcol || follow(sid) != sid;
    for (const auto& [k, sid] : *cs.col_styles) rowcol = rowcol || follow(sid) != sid;
    if (moves.empty() && !rowcol) continue;
    Sheet* s = M(cs.id);
    for (auto [r, c, n] : moves) s->SetStyle(r, c, n);
    if (rowcol) {
      for (auto& [k, sid] : s->row_styles.mut()) sid = follow(sid);
      for (auto& [k, sid] : s->col_styles.mut()) sid = follow(sid);
    }
    if (s->has_used) {
      NoteFormat(cs.id, s->used);
      changes_.push_back(CellChange{cs.id, s->used, kSrcUser});
    }
  }
}

void Document::SaveCellStyle(model::NamedStyle ns) {
  if (RefuseReadOnly()) return;
  NoteTouched();
  ns.style.cell_style = 0;
  // a number style (Comma, Currency...) given another code in Format...: that code is the style's from now on
  if (!ns.number_format.empty() && ns.style.numfmt) ns.number_format = wb_.styles->Str(ns.style.numfmt);
  for (const auto& b : model::BuiltinCellStyles()) {
    if (SameName(b.name, ns.name)) {
      ns.name = b.name;
      ns.builtin = true;
      ns.section = b.section;
    }
  }
  auto& v = wb_.cell_styles.mut();
  auto it = std::find_if(v.begin(), v.end(), [&](const model::NamedStyle& x) { return SameName(x.name, ns.name); });
  if (it != v.end()) *it = ns;
  else v.push_back(ns);
  // cells with this style take its new formats (Normal: the cells without a named style keep their own formats)
  if (ns.name != "Normal") {
    uint32_t id = 0;
    const model::NamedStyle prepared = Prepared(ns, wb_.styles.mut(), &id);
    RestyleNamed(id, [&](Style& x) { model::ApplyNamedStyle(x, prepared, id); });
  }
  wb_.dirty = true;
}

bool Document::DeleteCellStyle(std::string_view name) {
  if (RefuseReadOnly()) return false;
  if (SameName(name, "Normal")) return false;
  bool builtin = false;
  std::string real(name);
  for (const auto& b : model::BuiltinCellStyles()) {
    if (SameName(b.name, name)) {
      builtin = true;
      real = b.name;
    }
  }
  auto& v = wb_.cell_styles.mut();
  auto it = std::find_if(v.begin(), v.end(), [&](const model::NamedStyle& x) { return SameName(x.name, name); });
  if (it == v.end() && !builtin) return false;
  NoteTouched();
  if (it != v.end()) {
    real = it->name;
    v.erase(it);
  }
  const uint32_t id = wb_.styles.mut().InternString(real);
  if (builtin) {
    // a changed built-in style goes back to its original formats
    uint32_t pid = 0;
    const model::NamedStyle prepared = Prepared(*CellStyleDef(real), wb_.styles.mut(), &pid);
    RestyleNamed(id, [&](Style& x) { model::ApplyNamedStyle(x, prepared, id); });
  } else {
    // like Excel: the cells get the Normal formats
    RestyleNamed(id, [](Style& x) {
      const bool locked = x.locked, hidden = x.hidden;
      x = Style{};
      x.locked = locked;
      x.hidden = hidden;
    });
  }
  wb_.dirty = true;
  return true;
}


// ---- links to other workbooks ----

void Document::SetExternalValues(size_t link, std::vector<std::map<uint64_t, Value>> values, double updated, const std::string& error,
                                 const std::string& path, const std::string* read_from) {
  if (link >= wb_.external_links->size()) return;
  if (read_from && (*wb_.external_links)[link].path != *read_from) return;  // the source was changed meanwhile
  auto& l = wb_.external_links.mut()[link];
  if (!path.empty()) l.path = path;
  l.error = error;
  if (error.empty()) {
    l.values = std::move(values);
    l.values.resize(l.sheets.size());
    l.updated = updated;
  }
  for (size_t si = 0; si < l.sheets.size(); ++si) MarkDependentsOf(static_cast<uint32_t>(ExternalSheetId(link, si)), Rect{0, 0, 1048575, 16383});
  wb_.dirty = true;
  Recalc();
}

void Document::ChangeLinkSource(size_t link, const std::string& path) {
  if (RefuseReadOnly()) return;
  if (link >= wb_.external_links->size()) return;
  wb_.external_links.mut()[link].path = path;
  wb_.dirty = true;
}

void Document::BreakLink(size_t link) {
  if (RefuseReadOnly()) return;
  if (link >= wb_.external_links->size()) return;
  NoteTouched();
  // every formula that reads one of the link's sheets becomes its current value
  for (auto& sc : wb_.sheets) {
    const uint32_t sid = sc->id;
    std::vector<std::pair<uint32_t, uint32_t>> cells;
    ForEachCellInTiles(sc->meta, Rect{0, 0, sc->row_limit - 1, sc->col_limit - 1}, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      const uint32_t fid = t.formula[idx];
      if (!fid) return;
      auto def = std::static_pointer_cast<const formula::CompiledFormula>(wb_.formulas->Share(fid));
      if (!def) return;
      bool uses = false;
      formula::ForEachRef(const_cast<formula::Node&>(def->root), [&](formula::RefSpec& ref) {
        uses = uses || (IsExternalSheet(ref.sheet) && ExternalLinkOf(ref.sheet) == link);
      });
      if (uses) cells.emplace_back(r, c);
    });
    for (auto [r, c] : cells) {
      const Value v = S(sid)->GetValue(r, c);
      UnregisterFormulaCell(sid, r, c);
      Sheet* s = M(sid);
      s->SetFormula(r, c, 0);
      s->SetValue(r, c, v);
      NoteRegion(sid, Rect::Cell(r, c));
      changes_.push_back(CellChange{sid, Rect::Cell(r, c), kSrcUser});
    }
  }
  // the slot stays (other links keep their sheet ids); without a source it is no longer listed
  auto& l = wb_.external_links.mut()[link];
  l.path.clear();
  l.values.assign(l.sheets.size(), {});
  l.error.clear();
  wb_.dirty = true;
}

}  // namespace yc::engine
