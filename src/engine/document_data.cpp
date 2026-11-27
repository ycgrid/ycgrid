// SPDX-License-Identifier: Apache-2.0
// document_data.cpp - data tools: Remove Duplicates, Text to Columns, outline (group rows / columns), moving cells,
// Evaluate Formula steps
#include "engine/document.h"
#include "core/i18n.h"

#include "engine/external.h"
#include "engine/tile_iter.h"
#include "model/ref_texts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>

namespace yc::engine {

using model::Sheet;

namespace {

// a cell as a comparable key: Excel judges duplicates by the value shown (1.001 and 1.002 shown as "1.00" are the
// same; one date shown two ways is not), text case-insensitively; TRUE and the text "true" are one, while 1 and the
// text "1" or #N/A and the text "#N/A" are not (Excel 16, measured)
void ValueKey(const Value& v, std::string_view numfmt, std::string& k) {
  k += v.IsNumeric() ? 'n' : (v.IsString() || v.IsBool()) ? 's' : v.IsError() ? 'e' : '_';
  if (!v.IsEmpty()) k += Lower(DisplayTextOf(v, numfmt));
  k += '\x1f';
}

// UTF-8 code points of s (for fixed-width splitting)
std::vector<std::string> CodePoints(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(' ') - a + 1);
}

// a formula that calls one of the functions Excel reads as a total (anywhere in it: =IF(TRUE,SUM(B2:B5)) too)
bool CallsTotalFunction(const formula::Node& n) {
  if (n.kind == formula::NK::Func) {
    static const char* const kTotals[] = {"SUM", "AVERAGE", "COUNT", "COUNTA", "MAX", "MIN", "SUBTOTAL", "AGGREGATE"};
    for (const char* f : kTotals) {
      if (n.str == f) return true;
    }
  }
  return std::any_of(n.kids.begin(), n.kids.end(), [](const formula::Node& k) { return CallsTotalFunction(k); });
}

}  // namespace

// ---- Remove Duplicates ----

uint32_t Document::RemoveDuplicates(uint32_t sheet, const Rect& rect_in, const std::vector<uint32_t>& cols, bool header) {
  if (Refuse(sheet, &rect_in)) return 0;
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return 0;
  Rect rect = rect_in;
  rect.r1 = std::min(rect.r1, s->used.r1);
  rect.c1 = std::min(rect.c1, s->used.c1);
  if (RefuseTableOutsideList(sheet, rect)) return 0;
  const uint32_t first = rect.r0 + (header ? 1 : 0);
  if (first > rect.r1 || rect.c0 > rect.c1) return 0;
  // a last row with a total (a formula calling SUM, AVERAGE, COUNT, COUNTA, MAX, MIN, SUBTOTAL or AGGREGATE, whatever
  // it reads) is left out: it is neither compared nor moved, and the rows above close up under it. Rows above it with
  // such formulas are compared as any; PRODUCT, ROWS or =B2+B3 make no total (Excel 16, measured: dedupe_tot_ cases)
  if (rect.r1 > first && !TableAt(sheet, first, rect.c0)) {
    bool total = false;
    ForEachCellInTiles(s->meta, Rect{rect.r1, rect.c0, rect.r1, rect.c1}, [&](uint32_t, uint32_t, const model::MetaTile& t, uint32_t idx) {
      if (!total && t.formula[idx]) {
        const formula::CompiledFormula* f = Formula(t.formula[idx]);
        total = f && CallsTotalFunction(f->root);
      }
    });
    if (total) --rect.r1;
  }
  // the columns that hold something in the rows compared: an empty cell's key is the same in every row, so only those
  // with values tell rows apart, and only those with values, formulas or formats of their own have cells to move up.
  // Over the whole sheet with a value in XFD1048576 every row was compared and copied across 16,384 columns (17
  // billion cells; scale_sweep: Remove Duplicates' OK never answered)
  const Rect rows_rect{first, rect.c0, rect.r1, rect.c1};
  std::set<uint32_t> with_values;
  std::map<uint32_t, uint32_t> with_cells;  // column -> its last row that holds something
  auto note = [&](uint32_t r, uint32_t c) {
    auto [it, fresh] = with_cells.try_emplace(c, r);
    if (!fresh) it->second = std::max(it->second, r);
  };
  ForEachCellInTiles(s->values, rows_rect, [&](uint32_t r, uint32_t c, const model::ValueTile& t, uint32_t idx) {
    if (t.Occupied(idx)) with_values.insert(c), note(r, c);
  });
  ForEachCellInTiles(s->meta, rows_rect, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.style[idx] || t.formula[idx]) note(r, c);
  });
  std::vector<uint32_t> keys;
  if (cols.empty()) keys.assign(with_values.begin(), with_values.end());
  for (uint32_t c : cols) {
    if (with_values.count(c)) keys.push_back(c);
  }
  std::unordered_set<std::string> seen;
  std::vector<uint32_t> keep;
  std::string k;
  for (uint32_t r = first; r <= rect.r1; ++r) {
    k.clear();
    for (uint32_t c : keys) {
      if (c >= rect.c0 && c <= rect.c1) ValueKey(s->GetValue(r, c), wb_.styles->NumberFormat(wb_.styles->Get(s->GetStyle(r, c))), k);
    }
    if (seen.insert(k).second) keep.push_back(r - first);
  }
  const uint32_t total = rect.r1 - first + 1;
  const uint32_t removed = total - static_cast<uint32_t>(keep.size());
  if (removed == 0) return 0;
  // inside a table's data body whole table rows go: the table gets shorter and its totals row moves up (Excel)
  const model::Table* t = TableAt(sheet, first, rect.c0);
  if (t && t->HasData() && t->Data().r0 <= first && rect.r1 <= t->Data().r1 && rect.c0 >= t->range.c0 && rect.c1 <= t->range.c1 && !keep.empty()) {
    const Rect body{first, t->range.c0, rect.r1, t->range.c1};
    const ClipData all = Copy(sheet, body);
    ClipData out;
    out.rows = static_cast<uint32_t>(keep.size());
    out.cols = all.cols;
    out.src_sheet = sheet;
    out.src = body;
    for (uint32_t i : keep) {
      for (uint32_t j = 0; j < all.cols; ++j) out.cells.push_back(all.cells[static_cast<size_t>(i) * all.cols + j]);
    }
    Paste(sheet, first, body.c0, out, PasteMode::All);
    DeleteCells(sheet, Rect{first + out.rows, body.c0, rect.r1, body.c1}, true);
    return removed;
  }
  // the unique rows move up inside the range (contents and formats); the rows left at the bottom are cleared. Each run
  // of columns that hold something moves; the others have nothing to move
  // ... as far down as the run holds something: the kept rows below it are empty there, and land on rows it clears
  struct Run {
    uint32_t c0, c1, last;
  };
  std::vector<Run> runs;
  for (const auto& [c, last] : with_cells) {
    if (!runs.empty() && runs.back().c1 + 1 == c) runs.back().c1 = c, runs.back().last = std::max(runs.back().last, last);
    else runs.push_back(Run{c, c, last});
  }
  for (const auto& [c0, c1, last] : runs) {
    const Rect body{first, c0, std::min(last, rect.r1), c1};
    const ClipData all = Copy(sheet, body);
    ClipData out;
    out.cols = all.cols;
    out.src_sheet = sheet;
    out.src = body;
    for (uint32_t i : keep) {
      if (first + i > body.r1) break;
      for (uint32_t j = 0; j < all.cols; ++j) out.cells.push_back(all.cells[static_cast<size_t>(i) * all.cols + j]);
      ++out.rows;
    }
    ClearAll(sheet, body, kSrcUser, false, /*to_lines=*/true);
    if (out.rows) Paste(sheet, first, c0, out, PasteMode::All);
  }
  return removed;
}

// ---- Text to Columns ----

std::vector<std::string> SplitColumns(const std::string& text, const TextToColumnsOptions& o) {
  std::vector<std::string> parts;
  if (o.fixed) {
    const auto cps = CodePoints(text);
    std::vector<uint32_t> br = o.breaks;
    std::sort(br.begin(), br.end());
    br.erase(std::unique(br.begin(), br.end()), br.end());
    size_t start = 0;
    for (uint32_t b : br) {
      if (b <= start || b >= cps.size()) continue;
      std::string p;
      for (size_t i = start; i < b; ++i) p += cps[i];
      parts.push_back(Trim(p));
      start = b;
    }
    std::string p;
    for (size_t i = start; i < cps.size(); ++i) p += cps[i];
    parts.push_back(Trim(p));
    return parts;
  }
  auto is_delim = [&](char ch) {
    return (o.tab && ch == '\t') || (o.semicolon && ch == ';') || (o.comma && ch == ',') || (o.space && ch == ' ') ||
           (!o.other.empty() && ch == o.other[0]);
  };
  // Excel (measured): a qualifier opens a quoted field only as the field's first character; inside, a doubled one is
  // the character itself and a single one closes it, and the rest of the field follows unquoted ("a,b"c is a,bc); a
  // qualifier anywhere else is an ordinary character (ab"cd stays ab"cd, ' "a' too)
  std::string cur;
  bool quoted = false, last_delim = false, field_start = true;
  for (size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (o.qualifier && ch == o.qualifier && (quoted || field_start)) {
      if (!quoted) {
        quoted = true;
      } else if (i + 1 < text.size() && text[i + 1] == o.qualifier) {
        cur += ch;  // doubled qualifier inside a quoted field
        ++i;
      } else {
        quoted = false;
      }
      field_start = false;
      last_delim = false;
      continue;
    }
    if (!quoted && is_delim(ch)) {
      if (o.consecutive && last_delim) continue;
      parts.push_back(cur);
      cur.clear();
      last_delim = true;
      field_start = true;
      continue;
    }
    cur += ch;
    last_delim = false;
    field_start = false;
  }
  parts.push_back(cur);
  return parts;
}

std::string TextToColumnsDate(const std::string& field, int type) {
  if (type < 3 || type > 8) return "";
  // three numbers separated by anything else (1/2/2026, 2026-01-02, 2.1.26), or one run of 6 or 8 digits
  std::vector<std::string> nums;
  std::string cur;
  for (char ch : field) {
    if (ch >= '0' && ch <= '9') {
      cur += ch;
    } else if (!cur.empty()) {
      nums.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) nums.push_back(cur);
  // where the year, the month and the day are in the order (Excel's xlMDYFormat ... xlYDMFormat)
  static const char* kOrder[6] = {"MDY", "DMY", "YMD", "MYD", "DYM", "YDM"};
  const char* order = kOrder[type - 3];
  int yi = 0, mi = 0, di = 0;
  for (int k = 0; k < 3; ++k) {
    if (order[k] == 'Y') yi = k;
    if (order[k] == 'M') mi = k;
    if (order[k] == 'D') di = k;
  }
  if (nums.size() == 1 && (nums[0].size() == 8 || nums[0].size() == 6)) {
    const std::string d = nums[0];
    const size_t ylen = d.size() == 8 ? 4 : 2;
    nums.clear();
    size_t at = 0;
    for (int k = 0; k < 3; ++k) {
      const size_t len = order[k] == 'Y' ? ylen : 2;
      nums.push_back(d.substr(at, len));
      at += len;
    }
  }
  if (nums.size() != 3) return "";
  for (const auto& n : nums) {
    if (n.size() > 4) return "";
  }
  int64_t y = std::stoll(nums[static_cast<size_t>(yi)]);
  const int64_t m = std::stoll(nums[static_cast<size_t>(mi)]), d = std::stoll(nums[static_cast<size_t>(di)]);
  if (nums[static_cast<size_t>(yi)].size() <= 2) y += y < 30 ? 2000 : 1900;  // two-digit years, as Excel reads them
  static const int kDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1 || d > kDays[m - 1]) return "";
  if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return "";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld", static_cast<long long>(y), static_cast<long long>(m), static_cast<long long>(d));
  return buf;
}

namespace {

// Text to Columns > Advanced: a number written with other separators or a trailing minus, as the usual "1234.5"
std::string TextToColumnsNumber(const std::string& field, const TextToColumnsOptions& o) {
  const char dec = o.decimal ? o.decimal : '.', th = o.thousands ? o.thousands : ',';
  std::string t = Trim(field);
  bool minus = false;
  if (o.trailing_minus && t.size() > 1 && t.back() == '-') {
    minus = true;
    t.pop_back();
  }
  if (!minus && dec == '.' && th == ',') return field;  // the usual reading
  std::string out;
  bool digit = false;
  for (size_t i = 0; i < t.size(); ++i) {
    const char ch = t[i];
    if (ch >= '0' && ch <= '9') {
      out += ch;
      digit = true;
    } else if (ch == th && th != dec) {
      continue;
    } else if (ch == dec) {
      out += '.';
    } else if ((ch == '-' || ch == '+') && i == 0 && !minus) {
      out += ch;
    } else {
      return field;  // not a number: as it is
    }
  }
  if (!digit) return field;
  return minus ? "-" + out : out;
}

}  // namespace

uint32_t Document::TextToColumns(uint32_t sheet, const Rect& src_in, uint32_t dest_r, uint32_t dest_c, const TextToColumnsOptions& o) {
  if (RefuseReadOnly(sheet)) return 0;
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return 0;
  Rect src = src_in;
  src.c1 = src.c0;  // one column, like Excel
  src.r1 = std::min(src.r1, s->used.r1);
  if (src.r0 > src.r1) return 0;
  struct Row {
    std::vector<std::string> parts;
    bool text = false;
    Value value;
  };
  std::vector<Row> rows;
  for (uint32_t r = src.r0; r <= src.r1; ++r) {
    Row row;
    const Value& v = s->GetValue(r, src.c0);
    if (v.IsString()) {
      row.text = true;
      row.parts = SplitColumns(std::string(v.str()), o);
    } else if (v.IsNumeric() && !s->GetFormula(r, src.c0)) {
      // a number is split as it shows (Excel: 20240115 at fixed widths 4, 2 gives 2024 | 1 | 15; 12.5 at "." 12 | 5)
      const auto& st = *wb_.styles;
      row.text = true;
      row.parts = SplitColumns(DisplayTextOf(v, st.NumberFormat(st.Get(s->GetStyle(r, src.c0)))), o);
    } else {
      row.value = v;  // blanks, logical values and formulas stay as they are
    }
    rows.push_back(std::move(row));
  }
  // the block the text goes into is as wide as the row with the most fields (empty ones at its end don't count): a row
  // with fewer fields gets the rest of the block emptied (Excel 16, measured: 1;2;3 split at commas beside a,b,c,d,e
  // clears what was in its row's D; ,lead,,trail, with consecutive commas as one is three fields wide)
  uint32_t width = 1;
  for (const Row& row : rows) {
    if (!row.text) continue;
    uint32_t n = 0, filled = 0;
    for (size_t j = 0; j < row.parts.size(); ++j) {
      if (j < o.types.size() && o.types[j] == 9) continue;
      ++n;
      if (!row.parts[j].empty()) filled = n;
    }
    width = std::max(width, filled);
  }
  width = std::min<uint32_t>(width, core::kMaxCols - std::min<uint32_t>(dest_c, core::kMaxCols));
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const uint32_t r = dest_r + i;
    if (!rows[i].text) {
      if (!(r == src.r0 + i && dest_c == src.c0)) SetValue(sheet, r, dest_c, rows[i].value);
      continue;
    }
    uint32_t col = dest_c;
    for (size_t j = 0; j < rows[i].parts.size(); ++j) {
      const int type = j < o.types.size() ? o.types[j] : 1;
      if (type == 9) continue;  // skip this column
      if (col >= core::kMaxCols || col >= dest_c + width) break;  // (only empty fields lie past the block)
      const std::string& p = rows[i].parts[j];
      if (p.empty()) {
        ClearContents(sheet, Rect::Cell(r, col));
      } else if (type == 2) {
        SetValue(sheet, r, col, Value::String(p));  // a Text column
        // a Text column's cells are formatted as Text, as Excel's (and so what is typed there later stays text)
        ModifyStyle(sheet, Rect::Cell(r, col), [&](model::Style& x) { x.numfmt = wb_.styles.mut().InternString("@"); });
      } else if (p[0] == '=') {
        // a General field that is a formula becomes one (Excel: =1+1 gives 2, =D5*2 refers to D5); one that isn't
        // (=SUM(1 cut at a comma) stays text
        std::string why;
        if (p.size() < 2 || !SetFormula(sheet, r, col, std::string_view(p).substr(1), &why)) SetValue(sheet, r, col, Value::String(p));
      } else if (type >= 3 && type <= 8) {
        // Date (MDY, DMY...): the field's numbers in that order; a field that isn't such a date stays text, as in Excel
        const std::string iso = TextToColumnsDate(p, type);
        if (iso.empty()) SetValue(sheet, r, col, Value::String(p));
        else SetInput(sheet, r, col, iso);
      } else if (p[0] == '\'') {
        SetValue(sheet, r, col, Value::String(p));  // an apostrophe is part of the text here ('5 stays '5: Excel 16, measured)
      } else {
        SetInput(sheet, r, col, TextToColumnsNumber(p, o));  // General: numbers and dates are recognized
      }
      ++col;
    }
    if (width > 0 && col <= dest_c + width - 1) ClearContents(sheet, Rect{r, col, r, dest_c + width - 1});
  }
  return width;
}

// ---- Outline (group rows / columns) ----

void Document::GroupOutline(uint32_t sheet, bool rows, uint32_t a, uint32_t b_in, bool group) {
  if (Refuse(sheet, nullptr, Allow::Never)) return;
  Sheet* s = M(sheet);
  // a whole column's end marker (0xFFFFFFFF) set four billion levels and took the memory
  const uint32_t b = std::min<uint32_t>(b_in, rows ? s ? s->row_limit - 1 : 0 : core::kMaxCols - 1);
  if (!s || a > b) return;
  NoteTouched();
  auto& d = rows ? s->rows.mut() : s->cols.mut();
  for (uint64_t i = a; i <= b; ++i) {
    const int l = d.Level(static_cast<int64_t>(i));
    if (group) {
      d.SetLevel(static_cast<int64_t>(i), std::min(l + 1, model::DimIndex::kMaxOutline));
    } else if (l > 0) {
      // a hidden row that leaves its last group stays hidden (Excel 16, measured: Alt+Shift+Left and Range.Ungroup
      // over collapsed rows); Clear Outline shows them
      d.SetLevel(static_cast<int64_t>(i), l - 1);
    }
  }
  changes_.push_back(CellChange{sheet, rows ? Rect{a, 0, b, core::kMaxCols - 1} : Rect{0, a, core::kRowEnd, b}, kSrcUser});
  wb_.dirty = true;
}

void Document::SetIgnoreErrors(uint32_t sheet, const Rect& rect, bool ignore) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  auto& v = s->ignored_errors.mut();
  // what was there for these cells goes; ignoring adds the rectangle back
  std::vector<Rect> kept;
  for (const Rect& a : v) {
    if (!a.Intersects(rect)) {
      kept.push_back(a);
      continue;
    }
    // the parts of a outside rect stay (up to four bands)
    if (a.r0 < rect.r0) kept.push_back(Rect{a.r0, a.c0, rect.r0 - 1, a.c1});
    if (a.r1 > rect.r1) kept.push_back(Rect{rect.r1 + 1, a.c0, a.r1, a.c1});
    const uint32_t r0 = std::max(a.r0, rect.r0), r1 = std::min(a.r1, rect.r1);
    if (a.c0 < rect.c0) kept.push_back(Rect{r0, a.c0, r1, rect.c0 - 1});
    if (a.c1 > rect.c1) kept.push_back(Rect{r0, rect.c1 + 1, r1, a.c1});
  }
  if (ignore) kept.push_back(rect);
  v = std::move(kept);
  changes_.push_back(CellChange{sheet, rect, kSrcUser});
  wb_.dirty = true;
}

void Document::SetOutlineSettings(uint32_t sheet, bool summary_below, bool summary_right, bool automatic_styles) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  if (s->outline_below == summary_below && s->outline_right == summary_right && s->outline_styles == automatic_styles) return;
  NoteTouched();
  s->outline_below = summary_below;
  s->outline_right = summary_right;
  s->outline_styles = automatic_styles;
  changes_.push_back(CellChange{sheet, Rect{0, 0, core::kRowEnd, core::kMaxCols - 1}, kSrcUser});
  wb_.dirty = true;
}

int Document::AutoOutline(uint32_t sheet, const Rect& area_in) {
  if (RefuseReadOnly(sheet)) return 0;
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return 0;
  Rect area = area_in;
  area.r1 = std::min(area.r1, s->used.r1);
  area.c1 = std::min(area.c1, s->used.c1);
  if (area.r0 > area.r1 || area.c0 > area.c1) return 0;
  // Excel's Auto Outline (measured): a formula that refers to two or more cells above it in its column (left of it in
  // its row), with only blank cells between them and it, makes the rows (columns) from the first of them to the one
  // before it a group; a referenced cell that is itself such a total brings its group in (=SUM(A3,A6) over the totals
  // of rows 1-2 and 4-5 groups rows 1-6, nesting the two). One cell (=A3*2) or a value in between makes no group
  std::set<std::pair<uint32_t, uint32_t>> row_groups, col_groups;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> row_tops, col_tops;  // a total's (line, place) -> its group's start
  auto blank = [&](uint32_t r, uint32_t c) { return !s->GetFormula(r, c) && s->GetValue(r, c).IsEmpty(); };
  std::vector<formula::ResolvedRef> refs;
  // the formula cells only, in reading order (a total takes in the groups of the totals found before it): asking every
  // cell of the whole sheet took 12 s (scale_sweep)
  std::vector<std::pair<uint32_t, uint32_t>> formula_cells;
  ForEachCellInTiles(s->meta, area, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.formula[idx]) formula_cells.emplace_back(r, c);
  });
  std::sort(formula_cells.begin(), formula_cells.end());
  for (const auto& [r, c] : formula_cells) {
    {
      const formula::CompiledFormula* f = Formula(s->GetFormula(r, c));
      if (!f) continue;
      refs.clear();
      formula::CollectRefs(f->root, core::CellPos{r, c}, &refs);
      if (refs.empty()) continue;
      for (const bool by_rows : {true, false}) {
        uint64_t cells = 0;
        uint32_t first = UINT32_MAX, last = 0;
        bool ok = true;
        for (const auto& ref : refs) {
          const Rect& x = ref.rect;
          const bool own = ref.sheet < 0 || ref.sheet == static_cast<int32_t>(sheet);
          if (!own || (by_rows ? (x.c0 != c || x.c1 != c || x.r1 >= r) : (x.r0 != r || x.r1 != r || x.c1 >= c))) {
            ok = false;
            break;
          }
          cells += by_rows ? x.Rows() : x.Cols();
          first = std::min(first, by_rows ? x.r0 : x.c0);
          last = std::max(last, by_rows ? x.r1 : x.c1);
        }
        if (!ok || cells < 2) continue;
        const uint32_t at = by_rows ? r : c;
        for (uint32_t k = last + 1; k < at && ok; ++k) ok = by_rows ? blank(k, c) : blank(r, k);
        if (!ok) continue;
        auto& tops = by_rows ? row_tops : col_tops;
        const uint32_t line = by_rows ? c : r;
        uint32_t top = first;
        for (auto it = tops.lower_bound({line, first}); it != tops.end() && it->first.first == line && it->first.second <= last; ++it) {
          top = std::min(top, it->second);
        }
        tops[{line, at}] = top;
        (by_rows ? row_groups : col_groups).insert({top, at - 1});
        break;
      }
    }
  }
  if (row_groups.empty() && col_groups.empty()) return 0;
  ClearOutline(sheet, true);
  ClearOutline(sheet, false);
  for (const auto& [a, b] : row_groups) GroupOutline(sheet, true, a, b, true);
  for (const auto& [a, b] : col_groups) GroupOutline(sheet, false, a, b, true);
  return static_cast<int>(row_groups.size() + col_groups.size());
}

void Document::ClearOutline(uint32_t sheet, bool rows, uint32_t a, uint32_t b) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  auto& d = rows ? s->rows.mut() : s->cols.mut();
  std::vector<int64_t> idx;
  for (auto it = d.Levels().lower_bound(a); it != d.Levels().end() && it->first <= b; ++it) idx.push_back(it->first);
  for (int64_t i : idx) {
    if (d.IsHidden(i)) d.Hide(i, false);
    d.SetLevel(i, 0);
  }
  changes_.push_back(CellChange{sheet, Rect{0, 0, core::kRowEnd, core::kMaxCols - 1}, kSrcUser});
  wb_.dirty = true;
}

void Document::ShowOutlineLevel(uint32_t sheet, bool rows, int level) {
  if (RefuseReadOnly(sheet)) return;
  outline_toggle_ = true;
  struct Off {
    bool* f;
    ~Off() { *f = false; }
  } off{&outline_toggle_};
  const Sheet* s = S(sheet);
  if (!s) return;
  std::vector<std::pair<int64_t, bool>> todo;
  for (const auto& [i, l] : (rows ? *s->rows : *s->cols).Levels()) todo.emplace_back(i, l >= level);
  HideLineRuns(sheet, rows, todo);
}

// consecutive lines given the same choice are hidden or shown in one call: a line at a time, the million rows of a
// whole sheet's group took 8 s to show
void Document::HideLineRuns(uint32_t sheet, bool rows, const std::vector<std::pair<int64_t, bool>>& lines) {
  for (size_t k = 0; k < lines.size();) {
    size_t e = k;
    while (e + 1 < lines.size() && lines[e + 1].first == lines[e].first + 1 && lines[e + 1].second == lines[k].second) ++e;
    const auto a = static_cast<uint32_t>(lines[k].first), b = static_cast<uint32_t>(lines[e].first);
    if (rows) HideRows(sheet, a, b, lines[k].second);
    else HideCols(sheet, a, b, lines[k].second);
    k = e + 1;
  }
}

bool Document::OutlineBlock(uint32_t sheet, bool rows, uint32_t index, int level, uint32_t* a, uint32_t* b) const {
  const Sheet* s = S(sheet);
  if (!s) return false;
  const auto& d = rows ? *s->rows : *s->cols;
  if (d.Level(index) < level) return false;
  if (level <= 0) {  // every line is at level 0 or deeper
    *a = 0;
    *b = (rows ? s->row_limit : core::kMaxCols) - 1;
    return true;
  }
  // walked along the levels kept (consecutive keys), not a lookup a line: a million rows grouped took 2 s on a debug build
  const auto& lv = d.Levels();
  auto it = lv.find(index);
  uint32_t lo = index, hi = index;
  for (auto bk = it; bk != lv.begin();) {
    --bk;
    if (bk->first != int64_t{lo} - 1 || bk->second < level) break;
    --lo;
  }
  for (auto f = std::next(it); f != lv.end() && f->first == int64_t{hi} + 1 && f->second >= level; ++f) ++hi;
  *a = lo;
  *b = hi;
  return true;
}

void Document::SetGroupCollapsed(uint32_t sheet, bool rows, uint32_t index, int level, bool collapse) {
  if (RefuseReadOnly(sheet)) return;
  outline_toggle_ = true;
  struct Off {
    bool* f;
    ~Off() { *f = false; }
  } off{&outline_toggle_};
  uint32_t a, b;
  if (!OutlineBlock(sheet, rows, index, level, &a, &b)) return;
  if (collapse) {
    if (rows) HideRows(sheet, a, b, true);
    else HideCols(sheet, a, b, true);
    return;
  }
  // expanding shows the rows of this level; deeper groups keep their state (a collapsed inner group stays collapsed)
  const Sheet* s = S(sheet);
  const auto& d = rows ? *s->rows : *s->cols;
  std::vector<std::pair<int64_t, bool>> show;
  for (auto it = d.Levels().lower_bound(a); it != d.Levels().end() && it->first <= b; ++it) {
    if (it->second == level) show.emplace_back(it->first, false);
  }
  HideLineRuns(sheet, rows, show);
}

// ---- Moving cells (drag and drop, cut and paste) ----

namespace {

// A block of cells as references and moves see it, rows (index 0) and columns (1): whole rows / columns reach kEnd
struct Box {
  int64_t lo[2], hi[2];
};
constexpr int64_t kEnd = int64_t{1} << 40;

bool Meets(const Box& a, const Box& b) {
  return a.lo[0] <= b.hi[0] && b.lo[0] <= a.hi[0] && a.lo[1] <= b.hi[1] && b.lo[1] <= a.hi[1];
}
bool Within(const Box& a, const Box& b) {
  return a.lo[0] >= b.lo[0] && a.hi[0] <= b.hi[0] && a.lo[1] >= b.lo[1] && a.hi[1] <= b.hi[1];
}
bool Empty(const Box& a) { return a.lo[0] > a.hi[0] || a.lo[1] > a.hi[1]; }

// the bounding box of the cells of a that are not in b
Box Minus(Box a, const Box& b) {
  if (!Meets(a, b)) return a;
  for (int x = 0; x < 2; ++x) {
    const int y = 1 - x;
    if (b.lo[y] > a.lo[y] || b.hi[y] < a.hi[y]) continue;  // b leaves some of a's extent across
    if (b.lo[x] <= a.lo[x] && b.hi[x] >= a.hi[x]) {
      a.lo[x] = 1;
      a.hi[x] = 0;
      return a;
    }
    if (b.lo[x] <= a.lo[x]) a.lo[x] = b.hi[x] + 1;
    else if (b.hi[x] >= a.hi[x]) a.hi[x] = b.lo[x] - 1;
  }
  return a;
}

// a move of the cells `s` (on src_sheet) to `d` (s + off, on dst_sheet)
struct MoveSpec {
  uint32_t src_sheet, dst_sheet;
  Box s, d;
  int64_t off[2];
};

bool Has(const Box& a, int64_t r, int64_t c) { return a.lo[0] <= r && r <= a.hi[0] && a.lo[1] <= c && c <= a.hi[1]; }

// What MoveEdges makes of a range
enum class EdgeMove { kKeep, kChanged, kRef };

// How a range not wholly in the moved cells changes, as in Excel 16 (measured: 13,617 cuts over one column, over B2:D5,
// over C2:C11 and over random ranges, tests/unit/excel_measured_moves.inc). r is changed when kChanged is returned;
// on_src / on_dst: the range is on that sheet; dim_ok[k]: the range has its own extent along k (not whole rows / columns).
// - The moved cells hold a whole edge of it (all its columns of its first or last rows, or all its rows of its first
//   or last columns):
//   - moved along it, it becomes the bounding box of that edge where it went and what stays of it, the cells written
//     over at the stay's far end left out (B2 of B2:B5 cut to B3 gives B3:B5, B4 of B2:B4 cut to B9 gives B2:B9); all
//     of what stays when the cells written over leave nothing of it or only its middle (C1:C3 cut to C4 makes C2:C5
//     into C4:C6, C2 cut to C4 makes it C3:C5), except that cells from beyond the edge landing inside what stays leave
//     only the part past them (C1:C2 cut to C4 makes C2:C11 into C5:C11). Moved to beyond its other edge with the
//     cells it lands on all outside it (B2 to B7), it keeps its cells. This holds where it lies wholly in the cells
//     written over (C1:C4 cut to C2 makes C2:C5 into C3:C5, not #REF!).
//   - moved across (diagonally) onto cells covering its far edge, it is the moved part where it went (C4:D6 cut to B1
//     makes C2:C5 into B1:B2); moved straight sideways, or diagonally elsewhere, it keeps its cells.
//   - moved to another sheet it loses that edge (B2 gone: B3:B5).
// - The moved cells hold one corner of it: moved straight it keeps its cells (no cells written over leave it), and
//   moved diagonally onto cells covering all of it it is #REF!. Diagonally onto cells that span all of it one way (y)
//   and reach its far edge the other way (x), it reaches along y from the moved corner, where it went, to the opposite
//   corner, and along x the moved part (its top left or bottom right corner moved: A1:C3 cut to C5 makes B2:D5 into
//   C5:D6) or what the cells written over leave of it past that edge (its top right or bottom left: C2:E4 cut to B4
//   makes B2:D5 into B2:C3). Elsewhere it keeps its cells.
// - Otherwise cells written over at its edge by a block that reaches past that edge leave it (B2:B3 cut to B4 makes
//   B5:B9 into B6:B9); cells written over inside it don't change it (B5 cut to B4 leaves B2:B4).
EdgeMove MoveEdges(Box& r, const bool dim_ok[2], bool on_src, bool on_dst, const MoveSpec& mv) {
  Box stay = r;
  auto moved_part = [&] {
    Box m;
    for (int k = 0; k < 2; ++k) {
      m.lo[k] = std::max(r.lo[k], mv.s.lo[k]) + mv.off[k];
      m.hi[k] = std::min(r.hi[k], mv.s.hi[k]) + mv.off[k];
    }
    return m;
  };
  if (on_src && Meets(r, mv.s)) {
    for (int x = 0; x < 2; ++x) {
      const int y = 1 - x;
      if (!dim_ok[x] || mv.s.lo[y] > r.lo[y] || mv.s.hi[y] < r.hi[y]) continue;  // not a whole edge along x
      const bool low = mv.s.lo[x] <= r.lo[x] && mv.s.hi[x] < r.hi[x];
      const bool high = mv.s.lo[x] > r.lo[x] && mv.s.hi[x] >= r.hi[x];
      if (!low && !high) continue;  // the middle of it
      if (low) stay.lo[x] = mv.s.hi[x] + 1;
      else stay.hi[x] = mv.s.lo[x] - 1;
      if (mv.src_sheet != mv.dst_sheet) {
        r = stay;
        return EdgeMove::kChanged;
      }
      if (mv.off[y] != 0) {  // moved sideways: the moved part when it went across onto cells covering its far edge
        const int64_t far = low ? r.hi[x] : r.lo[x];
        if (mv.off[x] == 0 || mv.d.lo[y] > r.lo[y] || mv.d.hi[y] < r.hi[y] || far < mv.d.lo[x] || far > mv.d.hi[x]) return EdgeMove::kKeep;
        r = moved_part();
        return EdgeMove::kChanged;
      }
      if (low ? mv.d.lo[x] > r.hi[x] : mv.d.hi[x] < r.lo[x]) return EdgeMove::kKeep;  // past its other edge
      Box moved = moved_part();
      Box left = Minus(stay, mv.d);
      const bool same = left.lo[0] == stay.lo[0] && left.hi[0] == stay.hi[0] && left.lo[1] == stay.lo[1] && left.hi[1] == stay.hi[1];
      if (Empty(left) || same) {
        left = stay;  // nothing of it left, or only its middle written over: all of it
        // unless cells from beyond its moved edge land inside what stays: then only the part past them stays
        const bool across = mv.d.lo[y] <= stay.lo[y] && mv.d.hi[y] >= stay.hi[y];
        const bool within = stay.lo[x] < mv.d.lo[x] && mv.d.hi[x] < stay.hi[x];
        const bool beyond = low ? mv.s.lo[x] < r.lo[x] : mv.s.hi[x] > r.hi[x];
        if (across && within && beyond) {
          if (low) left.lo[x] = mv.d.hi[x] + 1;
          else left.hi[x] = mv.d.lo[x] - 1;
        }
      }
      if (!Empty(left)) {
        for (int k = 0; k < 2; ++k) {
          moved.lo[k] = std::min(moved.lo[k], left.lo[k]);
          moved.hi[k] = std::max(moved.hi[k], left.hi[k]);
        }
      }
      r = moved;
      return EdgeMove::kChanged;
    }
    // the moved cells hold one corner of it, not a whole edge
    const bool tl = Has(mv.s, r.lo[0], r.lo[1]), br = Has(mv.s, r.hi[0], r.hi[1]);
    const bool tr = Has(mv.s, r.lo[0], r.hi[1]), bl = Has(mv.s, r.hi[0], r.lo[1]);
    if (dim_ok[0] && dim_ok[1] && on_dst && (tl || br || tr || bl)) {
      if (Within(r, mv.d)) return EdgeMove::kRef;
      if (mv.off[0] == 0 || mv.off[1] == 0 || !Meets(r, mv.d)) return EdgeMove::kKeep;
      const Box moved = moved_part();
      for (int x = 0; x < 2; ++x) {
        const int y = 1 - x;
        if (mv.d.lo[y] > r.lo[y] || mv.d.hi[y] < r.hi[y]) continue;  // the cells written over don't span it across
        int64_t edge;  // its far edge along x from the corner moved
        if (tl || br) edge = tl ? r.hi[x] : r.lo[x];
        else if (x == 0) edge = tr ? r.hi[0] : r.lo[0];
        else edge = tr ? r.lo[1] : r.hi[1];
        if (edge < mv.d.lo[x] || edge > mv.d.hi[x]) continue;
        // across (y) it reaches from the moved corner, where it went, to the opposite one
        int64_t corner[2], opp[2];
        if (tl || br) {
          corner[0] = tl ? r.lo[0] : r.hi[0];
          corner[1] = tl ? r.lo[1] : r.hi[1];
        } else {
          corner[0] = tr ? r.lo[0] : r.hi[0];
          corner[1] = tr ? r.hi[1] : r.lo[1];
        }
        opp[0] = corner[0] == r.lo[0] ? r.hi[0] : r.lo[0];
        opp[1] = corner[1] == r.lo[1] ? r.hi[1] : r.lo[1];
        Box out;
        out.lo[y] = std::min(corner[y] + mv.off[y], opp[y]);
        out.hi[y] = std::max(corner[y] + mv.off[y], opp[y]);
        if (tl || br) {  // along x: the moved part
          out.lo[x] = moved.lo[x];
          out.hi[x] = moved.hi[x];
        } else {  // along x: what the cells written over leave of it past that edge
          out.lo[x] = r.lo[x];
          out.hi[x] = r.hi[x];
          if (mv.d.lo[x] < r.lo[x] && mv.d.hi[x] < r.hi[x]) out.lo[x] = mv.d.hi[x] + 1;
          else if (mv.d.hi[x] > r.hi[x] && mv.d.lo[x] > r.lo[x]) out.hi[x] = mv.d.lo[x] - 1;
        }
        r = out;
        return EdgeMove::kChanged;
      }
      return EdgeMove::kKeep;
    }
  }
  if (!on_dst || !Meets(r, mv.d)) return EdgeMove::kKeep;
  bool trimmed = false;
  for (int x = 0; x < 2; ++x) {
    const int y = 1 - x;
    if (!dim_ok[x] || mv.d.lo[y] > r.lo[y] || mv.d.hi[y] < r.hi[y]) continue;
    if (mv.d.lo[x] < r.lo[x] && r.lo[x] <= mv.d.hi[x] && mv.d.hi[x] < r.hi[x]) {
      stay.lo[x] = std::max(stay.lo[x], mv.d.hi[x] + 1);
      trimmed = true;
    } else if (r.lo[x] < mv.d.lo[x] && mv.d.lo[x] <= r.hi[x] && r.hi[x] < mv.d.hi[x]) {
      stay.hi[x] = std::min(stay.hi[x], mv.d.lo[x] - 1);
      trimmed = true;
    }
  }
  if (!trimmed) return EdgeMove::kKeep;
  r = stay;
  return EdgeMove::kChanged;
}

// re-encode the references of a formula that moves from (old sheet, old origin) to (new sheet, new origin): references
// into the moved cells follow them, those wholly in the cells written over are #REF!, ranges whose edge moves or is
// written over change as MoveEdges says; all others keep pointing at the same cells. only_absolute: references with
// a relative part are left as they are (a rule or a name read from cells that didn't move points from each of them)
bool RebaseRefs(formula::Node& root, uint32_t old_sheet, uint32_t new_sheet, core::CellPos old_o, core::CellPos new_o, const MoveSpec& mv,
                bool only_absolute = false) {
  bool changed = false;
  formula::ForEachRef(root, [&](formula::RefSpec& ref) {
    if (!ref.HasCells()) return;
    if (only_absolute && !(ref.r0.abs && ref.c0.abs && (!ref.is_range || (ref.r1.abs && ref.c1.abs)))) return;
    const int32_t target_sheet = ref.TargetSheet(static_cast<int32_t>(old_sheet));
    const formula::RefCoord* lo_c[2] = {&ref.r0, &ref.c0};
    const formula::RefCoord* hi_c[2] = {ref.is_range ? &ref.r1 : &ref.r0, ref.is_range ? &ref.c1 : &ref.c0};
    const int64_t origin_old[2] = {old_o.row, old_o.col}, origin_new[2] = {new_o.row, new_o.col};
    const bool whole[2] = {ref.whole_cols, ref.whole_rows};  // the dimension it spans whole
    Box b;
    bool swapped[2];
    for (int k = 0; k < 2; ++k) {
      const int64_t a = lo_c[k]->Resolve(origin_old[k]), z = hi_c[k]->Resolve(origin_old[k]);
      swapped[k] = a > z;
      b.lo[k] = whole[k] ? 0 : std::min(a, z);
      b.hi[k] = whole[k] ? kEnd : std::max(a, z);
    }
    const bool plain = ref.sheet2 < 0;  // 3-D references don't follow one sheet's cells
    const bool on_src = plain && target_sheet == static_cast<int32_t>(mv.src_sheet);
    const bool on_dst = plain && target_sheet == static_cast<int32_t>(mv.dst_sheet);
    const bool inside = on_src && Within(b, mv.s);
    // a range whose edge or corner moves or is written over changes as MoveEdges says
    EdgeMove edges = EdgeMove::kKeep;
    if (!inside && ref.is_range && !ref.spill && (on_src || on_dst)) {
      const bool dim_ok[2] = {!whole[0], !whole[1]};
      edges = MoveEdges(b, dim_ok, on_src, on_dst, mv);
    }
    // cells the move writes over are gone: a reference to them (not to the moved cells) is #REF!, as in Excel, unless
    // an edge of the range moved
    if (edges == EdgeMove::kRef || (!inside && edges == EdgeMove::kKeep && on_dst && Within(b, mv.d))) {
      ref.bad = true;
      changed = true;
      return;
    }
    int32_t new_target = target_sheet;
    if (inside) {
      for (int k = 0; k < 2; ++k) {
        b.lo[k] += mv.off[k];
        b.hi[k] += mv.off[k];
      }
      new_target = static_cast<int32_t>(mv.dst_sheet);
    }
    auto enc = [&](formula::RefCoord& rc, int64_t abs_v, int64_t origin) {
      const int64_t v = rc.abs ? abs_v : abs_v - origin;
      if (v != rc.v) {
        rc.v = v;
        changed = true;
      }
    };
    formula::RefCoord* lo_m[2] = {&ref.r0, &ref.c0};
    formula::RefCoord* hi_m[2] = {&ref.r1, &ref.c1};
    for (int k = 0; k < 2; ++k) {
      if (whole[k]) continue;  // 1:3 keeps reaching every column
      if (!ref.is_range) {
        enc(*lo_m[k], b.lo[k], origin_new[k]);
        continue;
      }
      enc(*lo_m[k], swapped[k] ? b.hi[k] : b.lo[k], origin_new[k]);
      enc(*hi_m[k], swapped[k] ? b.lo[k] : b.hi[k], origin_new[k]);
    }
    const int32_t want = (ref.sheet < 0 && new_target == static_cast<int32_t>(new_sheet)) ? -1 : new_target;
    if (want != ref.sheet) {
      ref.sheet = want;
      changed = true;
    }
  });
  return changed;
}

}  // namespace

bool Document::MoveCells(uint32_t src_sheet, const Rect& src_in, uint32_t dst_sheet, uint32_t dr, uint32_t dc) {
  if (!S(src_sheet)) return false;
  // whole columns (rows open-ended, as Python's D:D sends them) end at the sheet's last row
  Rect src = src_in;
  if (src.r1 >= S(src_sheet)->row_limit) src.r1 = S(src_sheet)->row_limit - 1;
  if (src.r0 > src.r1) return false;
  if (Refuse(src_sheet, &src)) return false;
  {
    const Rect want{dr, dc, static_cast<uint32_t>(dr + src.Rows() - 1), static_cast<uint32_t>(dc + src.Cols() - 1)};
    if (Refuse(dst_sheet, &want)) return false;
  }
  if (!S(src_sheet) || !S(dst_sheet)) return false;
  if (uint64_t{dr} + src.Rows() > core::kMaxRows || uint64_t{dc} + src.Cols() > core::kMaxCols) return false;
  const int64_t drow = int64_t{dr} - src.r0, dcol = int64_t{dc} - src.c0;
  if (drow == 0 && dcol == 0 && src_sheet == dst_sheet) return true;
  const Rect dst{dr, dc, static_cast<uint32_t>(dr + src.Rows() - 1), static_cast<uint32_t>(dc + src.Cols() - 1)};
  // an array entered over several cells doesn't land in a table, as it can't be entered there: its value became a
  // header's text, and a calculation on open made it a number again (doc_fuzz seed 99007204). A table cut with it
  // goes along and isn't in the way.
  for (const auto& [k, sp] : *S(src_sheet)->spills) {
    if (!sp.cse || sp.rect.IsCell() || !src.Contains(sp.rect)) continue;
    const Rect to{static_cast<uint32_t>(sp.rect.r0 + drow), static_cast<uint32_t>(sp.rect.c0 + dcol), static_cast<uint32_t>(sp.rect.r1 + drow),
                  static_cast<uint32_t>(sp.rect.c1 + dcol)};
    for (const auto& t : *S(dst_sheet)->tables) {
      if (src_sheet == dst_sheet && src.Contains(t.range)) continue;
      if (!t.range.Intersects(to)) continue;
      CountBlock(BlockReason::Array);
      block_sheet_ = dst_sheet;
      block_cells_ = to;
      block_range_ = -1;
      block_python_ = python_call;
      block_pivot_ = block_table_ = block_param_ = block_read_only_ = block_off_sheet_ = block_merge_size_ = block_table_shift_ = false;
      return false;
    }
  }
  // the move as references see it: cut whole columns (rows) reach the end of a whole column's (row's) reference
  MoveSpec mv{src_sheet, dst_sheet, {}, {}, {drow, dcol}};
  {
    const bool all_rows = src.r0 == 0 && uint64_t{src.r1} + 1 >= S(src_sheet)->row_limit;
    const bool all_cols = src.c0 == 0 && uint64_t{src.c1} + 1 >= core::kMaxCols;
    mv.s = Box{{src.r0, src.c0}, {all_rows ? kEnd : int64_t{src.r1}, all_cols ? kEnd : int64_t{src.c1}}};
    mv.d = Box{{dst.r0, dst.c0}, {all_rows ? kEnd : int64_t{dst.r1}, all_cols ? kEnd : int64_t{dst.c1}}};
  }
  // undo puts back the two rectangles and every formula the move rewrote; spilled arrays on the sheets (cleared and
  // spilled again) change more than that: then the whole version goes back
  {
    auto has = [&](uint32_t id) {
      const Sheet* s = S(id);
      return s && !s->spills->empty();
    };
    if (has(src_sheet) || has(dst_sheet) || CutHoldsTable(src_sheet, src)) NoteStructural();
  }
  NoteRegion(src_sheet, src);
  NoteRegion(dst_sheet, dst);
  ClearSheetSpills(src_sheet);
  if (dst_sheet != src_sheet) ClearSheetSpills(dst_sheet);
  EnsureRowLimit(dst_sheet, dst.r1);

  // 1. the new definition of every formula
  struct Fix {
    uint32_t sheet, r, c, id;
  };
  std::vector<Fix> fixes;
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> all;  // sheet, r, c, id
  for (const auto& sc : wb_.sheets) {
    const Sheet& sh = sc.get();
    sh.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (t.formula[idx]) all.emplace_back(sh.id, rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows, t.formula[idx]);
      }
    });
  }
  for (const auto& [fs, r, c, id] : all) {
    const bool moved = fs == src_sheet && src.Contains(r, c);
    if (!moved && fs == dst_sheet && dst.Contains(r, c)) continue;  // overwritten by the move
    const formula::CompiledFormula* f = Formula(id);
    if (!f) continue;
    formula::Node copy = f->root;
    const core::CellPos old_o{r, c};
    const core::CellPos new_o = moved ? core::CellPos{static_cast<uint32_t>(r + drow), static_cast<uint32_t>(c + dcol)} : old_o;
    const uint32_t new_sheet = moved ? dst_sheet : fs;
    const bool changed = RebaseRefs(copy, fs, new_sheet, old_o, new_o, mv);
    if (!changed && !moved) continue;
    if (!moved) NoteRegion(new_sheet, Rect::Cell(new_o.row, new_o.col));  // a formula elsewhere pointing at the cells
    fixes.push_back(Fix{new_sheet, new_o.row, new_o.col, changed ? InternFormula(std::move(copy)) : id});
  }

  // 1b. what reads the cells as text follows them too, as in Excel: defined names and chart series (their fixed
  // references) at once; conditional formats and data validation move with the cells, the rules of the cells written
  // over go, and every rule's formulas follow the moved cells (worked out from the rules as they are, set after the
  // cells are cleared). Left alone, a cut and paste left the names and charts on the emptied cells, the formats'
  // rules behind, and dropped the validation.
  constexpr uint32_t kNoSheet = 0xFFFFFFFFu;  // a global name's references without a sheet: never the moved cells
  // text read at parse_at; references resolved at old_o (sheet ctx) and written for new_o (sheet new_ctx), at `at`
  auto rebase_text = [&](std::string& text, core::CellPos parse_at, uint32_t ctx, core::CellPos old_o, uint32_t new_ctx, core::CellPos new_o,
                         core::CellPos at, bool only_absolute, bool constants_ok) {
    if (text.empty()) return;
    std::string_view t = text;
    const bool eq = t[0] == '=';
    if (!eq && constants_ok) return;  // a list of values (Buy,Sell) is no formula
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, parse_at, this);
    if (!pr.ok) return;
    const bool changed = RebaseRefs(pr.root, ctx, new_ctx, old_o, new_o, mv, only_absolute);
    if (changed || at.row != parse_at.row || at.col != parse_at.col) text = (eq ? "=" : "") + formula::ToA1(pr.root, at, this);
  };
  {
    std::vector<model::NameDef> names = *wb_.names;
    bool any = false;
    for (auto& n : names) {
      const std::string before = n.refers_to;
      const uint32_t ctx = n.sheet_scope >= 0 ? static_cast<uint32_t>(n.sheet_scope) : kNoSheet;
      rebase_text(n.refers_to, core::CellPos{0, 0}, ctx, core::CellPos{0, 0}, ctx, core::CellPos{0, 0}, core::CellPos{0, 0}, true, false);
      any = any || n.refers_to != before;
    }
    if (any) wb_.names.mut() = std::move(names);
  }
  for (auto& sc : wb_.sheets) {  // the objects' fixed references: charts, controls, sparklines' date axes, Solver
    const uint32_t sid = sc->id;
    model::EditObjectRefTexts(sc, [&](std::string& s) {
      rebase_text(s, core::CellPos{0, 0}, sid, core::CellPos{0, 0}, sid, core::CellPos{0, 0}, core::CellPos{0, 0}, true, false);
    });
  }
  model::EditWorkbookRefTexts(wb_, [&](std::string& s) {  // custom panels' texts, which name their sheet
    rebase_text(s, core::CellPos{0, 0}, kNoSheet, core::CellPos{0, 0}, kNoSheet, core::CellPos{0, 0}, core::CellPos{0, 0}, true, false);
  });
  // what keeps a block of cells as a rectangle follows it when all of it moves, as a reference does (Excel 16,
  // measured: a range a protected sheet lets people edit and the cells a linked picture shows; YCGrid's alert rules
  // and parameter cells). The cells error checking leaves alone don't (Excel: the cut cell is checked again).
  auto follows = [&](uint32_t sid, Rect* r) {
    if (sid != src_sheet || !src.Contains(*r)) return false;
    *r = Rect{static_cast<uint32_t>(r->r0 + drow), static_cast<uint32_t>(r->c0 + dcol), static_cast<uint32_t>(r->r1 + drow),
              static_cast<uint32_t>(r->c1 + dcol)};
    return true;
  };
  if (src_sheet == dst_sheet && S(src_sheet)->page_setup->has_area) {  // the print area: Excel's Print_Area name
    model::PageSetup ps = *S(src_sheet)->page_setup;
    bool any = follows(src_sheet, &ps.area);
    for (auto& m : ps.more_areas) any = follows(src_sheet, &m) || any;
    if (any) M(src_sheet)->page_setup.mut() = std::move(ps);
  }
  if (src_sheet == dst_sheet && !S(src_sheet)->protection->ranges.empty()) {
    std::vector<model::AllowEditRange> v = S(src_sheet)->protection->ranges;
    bool any = false;
    for (auto& e : v) any = follows(src_sheet, &e.rect) || any;
    if (any) M(src_sheet)->protection.mut().ranges = std::move(v);
  }
  for (auto& sc : wb_.sheets) {
    if (sc->linked_pictures->empty()) continue;
    std::vector<Sheet::LinkedPicture> v = *sc->linked_pictures;
    bool any = false;
    for (auto& lp : v) {
      if (follows(lp.sheet, &lp.rect)) lp.sheet = dst_sheet, any = true;
    }
    if (any) sc.mut().linked_pictures.mut() = std::move(v);
  }
  {
    std::vector<model::AlertRule> v = *wb_.alerts;
    bool any = false;
    for (auto& a : v) {
      if (follows(a.sheet, &a.range)) a.sheet = dst_sheet, any = true;
    }
    if (any) wb_.alerts.mut() = std::move(v);
  }
  {
    std::vector<model::ParameterDef> v = *wb_.parameters;
    bool any = false;
    for (auto& p : v) {
      Rect cell = Rect::Cell(p.row, p.col);
      if (!follows(p.sheet, &cell)) continue;
      p.sheet = dst_sheet, p.row = cell.r0, p.col = cell.c0;
      any = true;
    }
    if (any) wb_.parameters.mut() = std::move(v);
  }
  // a rule's ranges: the part on the moved cells goes with them, the part on the cells written over goes
  auto split = [&](const std::vector<Rect>& ranges, uint32_t sid, std::vector<Rect>* stay, std::vector<Rect>* moved) {
    for (const Rect& x : ranges) {
      std::vector<Rect> rest{x};
      if (sid == src_sheet && x.Intersects(src)) {
        const Rect i{std::max(x.r0, src.r0), std::max(x.c0, src.c0), std::min(x.r1, src.r1), std::min(x.c1, src.c1)};
        moved->push_back(Rect{static_cast<uint32_t>(i.r0 + drow), static_cast<uint32_t>(i.c0 + dcol), static_cast<uint32_t>(i.r1 + drow),
                              static_cast<uint32_t>(i.c1 + dcol)});
        rest = SubtractRect(x, src);
      }
      for (const Rect& y : rest) {
        if (sid != dst_sheet) stay->push_back(y);
        else
          for (const Rect& z : SubtractRect(y, dst)) stay->push_back(z);
      }
    }
  };
  // a rule's formulas for its cells that stayed (written for their first cell) and for the moved ones (read as at the
  // first moved cell, as a cell formula moved there)
  auto stay_text = [&](std::string& f, core::CellPos o, uint32_t sid, core::CellPos at, bool constants_ok) {
    rebase_text(f, o, sid, o, sid, o, at, true, constants_ok);
  };
  auto moved_text = [&](std::string& f, core::CellPos o, uint32_t sid, const Rect& first_moved, bool constants_ok) {
    const core::CellPos at{first_moved.r0, first_moved.c0};
    const core::CellPos from{static_cast<uint32_t>(first_moved.r0 - drow), static_cast<uint32_t>(first_moved.c0 - dcol)};
    rebase_text(f, o, sid, from, dst_sheet, at, at, false, constants_ok);
  };
  auto cf_texts = [](model::CfRule& r, const std::function<void(std::string&)>& fn) { model::ForEachCfRefText(r, fn); };
  std::vector<std::pair<uint32_t, std::vector<model::CfRule>>> new_cf;
  std::vector<std::pair<uint32_t, std::vector<model::DvRule>>> new_dv;
  std::vector<model::CfRule> cf_across;  // moved to the other sheet
  std::vector<model::DvRule> dv_across;
  for (const auto& sc : wb_.sheets) {
    const uint32_t sid = sc->id;
    if (!sc->cond_formats->empty()) {
      std::vector<model::CfRule> out;
      bool any = false;
      for (const model::CfRule& r : *sc->cond_formats) {
        if (r.ranges.empty()) {
          out.push_back(r);
          continue;
        }
        std::vector<Rect> stay, moved;
        split(r.ranges, sid, &stay, &moved);
        const core::CellPos o{r.ranges.front().r0, r.ranges.front().c0};
        if (!stay.empty()) {
          model::CfRule k = r;
          k.ranges = stay;
          cf_texts(k, [&](std::string& f) { stay_text(f, o, sid, core::CellPos{stay.front().r0, stay.front().c0}, false); });
          any = any || !(k == r);
          out.push_back(std::move(k));
        } else {
          any = true;
        }
        if (!moved.empty()) {
          model::CfRule k = r;
          k.ranges = moved;
          cf_texts(k, [&](std::string& f) { moved_text(f, o, sid, moved.front(), false); });
          if (!stay.empty() || dst_sheet != sid) k.id = 0;  // another rule (SetCondFormats numbers it)
          any = true;
          if (dst_sheet == sid) out.push_back(std::move(k));
          else cf_across.push_back(std::move(k));
        }
      }
      if (any) new_cf.emplace_back(sid, std::move(out));
    }
    if (!sc->validations->empty()) {
      std::vector<model::DvRule> out;
      bool any = false;
      for (const model::DvRule& r : *sc->validations) {
        if (r.ranges.empty()) {
          out.push_back(r);
          continue;
        }
        std::vector<Rect> stay, moved;
        split(r.ranges, sid, &stay, &moved);
        const core::CellPos o{r.ranges.front().r0, r.ranges.front().c0};
        if (!stay.empty()) {
          model::DvRule k = r;
          k.ranges = stay;
          const core::CellPos at{stay.front().r0, stay.front().c0};
          model::ForEachDvRefText(k, [&](std::string& f, bool constants_ok) { stay_text(f, o, sid, at, constants_ok); });
          any = any || k.ranges != r.ranges || k.formula1 != r.formula1 || k.formula2 != r.formula2;
          out.push_back(std::move(k));
        } else {
          any = true;
        }
        if (!moved.empty()) {
          model::DvRule k = r;
          k.ranges = moved;
          model::ForEachDvRefText(k, [&](std::string& f, bool constants_ok) { moved_text(f, o, sid, moved.front(), constants_ok); });
          any = true;
          if (dst_sheet == sid) out.push_back(std::move(k));
          else dv_across.push_back(std::move(k));
        }
      }
      if (any) new_dv.emplace_back(sid, std::move(out));
    }
  }
  // rules moved to another sheet go first there (as a new rule does)
  if (!cf_across.empty()) {
    auto it = std::find_if(new_cf.begin(), new_cf.end(), [&](const auto& e) { return e.first == dst_sheet; });
    if (it == new_cf.end()) it = new_cf.emplace(new_cf.end(), dst_sheet, *S(dst_sheet)->cond_formats);
    for (auto& r : cf_across) r.priority = -1;
    it->second.insert(it->second.begin(), cf_across.begin(), cf_across.end());
  }
  if (!dv_across.empty()) {
    auto it = std::find_if(new_dv.begin(), new_dv.end(), [&](const auto& e) { return e.first == dst_sheet; });
    if (it == new_dv.end()) it = new_dv.emplace(new_dv.end(), dst_sheet, *S(dst_sheet)->validations);
    it->second.insert(it->second.end(), dv_across.begin(), dv_across.end());
  }

  // 2. move contents, formats, merges, notes and hyperlinks
  const ClipData clip = Copy(src_sheet, src);
  std::vector<std::pair<uint64_t, model::CellNote>> notes;
  std::vector<std::pair<uint64_t, model::Hyperlink>> links;
  {
    const Sheet* s = S(src_sheet);
    for (const auto& [k, n] : *s->notes) {
      if (src.Contains(static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF))) notes.emplace_back(k, n);
    }
    for (const auto& [k, l] : *s->links) {
      if (src.Contains(static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF))) links.emplace_back(k, l);
    }
  }
  // tables wholly inside the cut cells go with them, to this sheet or another, under their names (Excel 16, measured):
  // left behind, the emptied header renamed the columns Column1, Column2 and the formulas followed that rename. One
  // whose buttons were the sheet's AutoFilter takes it along on this sheet.
  std::vector<model::Table> moving;
  bool moving_filter = false;
  model::AutoFilter moved_af;  // the sheet's AutoFilter, moved with its table on this sheet
  if (CutHoldsTable(src_sheet, src)) {
    Sheet* ss = M(src_sheet);
    auto& v = ss->tables.mut();
    for (const model::Table& t : v) {
      if (!src.Contains(t.range)) continue;
      moving_filter = moving_filter || TableOwnsSheetFilter(*ss, t);
      moving.push_back(t);
    }
    v.erase(std::remove_if(v.begin(), v.end(), [&](const model::Table& t) { return src.Contains(t.range); }), v.end());
    if (moving_filter) {
      model::AutoFilter af = *ss->autofilter;
      ss->autofilter.mut() = model::AutoFilter{};
      if (dst_sheet == src_sheet) {
        af.range = Rect{static_cast<uint32_t>(af.range.r0 + drow), static_cast<uint32_t>(af.range.c0 + dcol), static_cast<uint32_t>(af.range.r1 + drow),
                        static_cast<uint32_t>(af.range.c1 + dcol)};
        moved_af = af;
      }
    }
  }
  // arrays entered with Ctrl+Shift+Enter inside the cut go with it as arrays (Excel 16, measured): pasted cell by cell,
  // their results became constants under the anchor's formula, which then showed #SPILL!
  std::vector<model::SpillRange> cse_moving;
  for (const auto& [k, sp] : *S(src_sheet)->spills) {
    if (sp.cse && src.Contains(sp.rect)) cse_moving.push_back(sp);
  }
  ClearAll(src_sheet, src, kSrcUser, false, /*to_lines=*/true);  // what a cut leaves shows its row's or column's formats
  ClearAll(dst_sheet, dst);
  // a merged area the moved cells land on part of is undone (Excel 16, measured: A2:B3 merged cut to E3 over F3:G5
  // merged leaves E3:F4 merged and G5 alone); kept, the two merged areas overlapped (doc_fuzz seed 33003351)
  {
    const auto& ms = *S(dst_sheet)->merges;
    if (std::any_of(ms.begin(), ms.end(), [&](const Rect& m) { return m.Intersects(dst); })) {
      NoteTouched();  // a part of the workbook changed directly: in the step, for Undo
      for (const Rect& m : ms) {
        if (m.Intersects(dst)) NoteFormat(dst_sheet, m);
      }
      auto& mm = M(dst_sheet)->merges.mut();
      mm.erase(std::remove_if(mm.begin(), mm.end(), [&](const Rect& m) { return m.Intersects(dst); }), mm.end());
    }
  }
  std::vector<std::string> merged_away;  // tables cut into another table: their cells are its cells now
  if (!moving.empty()) {
    auto& v = M(dst_sheet)->tables.mut();
    const size_t staying = v.size();
    for (model::Table t : moving) {
      t.range = Rect{static_cast<uint32_t>(t.range.r0 + drow), static_cast<uint32_t>(t.range.c0 + dcol), static_cast<uint32_t>(t.range.r1 + drow),
                     static_cast<uint32_t>(t.range.c1 + dcol)};
      // a table cut onto a table that stays goes into it as cells (Excel 16, measured: one table left, its header
      // taking the moved one's text, and what named the moved table #REF!); kept as a table, the two overlapped and
      // fought over their headers' names, and Redo gave back the other name (doc_fuzz seed 20411091)
      if (std::any_of(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(staying), [&](const model::Table& o) { return o.range.Intersects(t.range); })) {
        merged_away.push_back(t.name);
        continue;
      }
      v.push_back(std::move(t));
    }
    if (moved_af.on) M(dst_sheet)->autofilter.mut() = moved_af;
  }
  // what reads the moved cells as a block follows them too (Excel 16, measured): the sheet's AutoFilter and an
  // advanced filter's list cut whole within the sheet, sparklines' data and PivotTables' sources anywhere
  if (src_sheet == dst_sheet && !moving_filter) {
    model::AutoFilter af = *S(src_sheet)->autofilter;
    bool any = af.on && follows(src_sheet, &af.range);
    if (!(af.adv_range == Rect{})) any = follows(src_sheet, &af.adv_range) || any;
    if (any) M(src_sheet)->autofilter.mut() = std::move(af);
  }
  for (auto& sc : wb_.sheets) {
    if (!sc->sparklines->empty()) {
      std::vector<model::SparklineGroup> v = *sc->sparklines;
      bool any = false;
      for (auto& g : v) {
        for (auto& it : g.items) {
          if (follows(it.src_sheet, &it.src)) it.src_sheet = dst_sheet, any = true;
        }
      }
      if (any) sc.mut().sparklines.mut() = std::move(v);
    }
    if (!sc->pivots->empty()) {
      std::vector<model::PivotTable> v = *sc->pivots;
      bool any = false;
      for (auto& p : v) {
        if (p.src_table.empty() && follows(p.src_sheet, &p.src)) p.src_sheet = dst_sheet, any = true;
      }
      if (any) sc.mut().pivots.mut() = std::move(v);
    }
  }
  for (auto& [sid, rules] : new_cf) {
    std::stable_sort(rules.begin(), rules.end(), [](const model::CfRule& a, const model::CfRule& b) { return a.priority < b.priority; });
    SetCondFormats(sid, std::move(rules));
  }
  for (auto& [sid, rules] : new_dv) {
    NoteTouched();
    rules.erase(std::remove_if(rules.begin(), rules.end(), [](const model::DvRule& r) { return r.ranges.empty(); }), rules.end());
    M(sid)->validations.mut() = std::move(rules);
  }
  Sheet* d = M(dst_sheet);
  auto& table = wb_.styles.mut();
  for (uint32_t i = 0; i < clip.rows; ++i) {
    for (uint32_t j = 0; j < clip.cols; ++j) {
      const ClipCell& cc = clip.cells[static_cast<size_t>(i) * clip.cols + j];
      if (!cc.value.IsEmpty()) d->SetValue(dst.r0 + i, dst.c0 + j, cc.value);
      if (cc.has_style) {
        const uint32_t sid = table.Intern(cc.style);
        if (sid) d->SetStyle(dst.r0 + i, dst.c0 + j, sid);
      }
    }
  }
  // merged cells cut onto a table's cells come unmerged (Excel 16, measured): the table holds no merged cells
  for (const Rect& m : clip.merges) {
    const Rect x{dst.r0 + m.r0, dst.c0 + m.c0, dst.r0 + m.r1, dst.c0 + m.c1};
    const auto& tabs = *d->tables;
    if (std::none_of(tabs.begin(), tabs.end(), [&](const model::Table& t) { return t.range.Intersects(x); })) d->merges.mut().push_back(x);
  }
  if (!clip.rich.empty()) {
    for (const auto& [idx, rt] : clip.rich) {
      const uint64_t rr = dst.r0 + idx / clip.cols, cc = dst.c0 + idx % clip.cols;
      d->rich.mut()[(rr << 16) | cc] = rt;
    }
  }
  if (!notes.empty() || !links.empty()) {
    Sheet* s = M(src_sheet);
    for (const auto& [k, n] : notes) s->notes.mut().erase(k);
    for (const auto& [k, l] : links) s->links.mut().erase(k);
    d = M(dst_sheet);
    for (const auto& [k, n] : notes) d->notes.mut()[((k >> 16) + drow) << 16 | ((k & 0xFFFF) + dcol)] = n;
    for (const auto& [k, l] : links) d->links.mut()[((k >> 16) + drow) << 16 | ((k & 0xFFFF) + dcol)] = l;
  }
  d->Touch(dst.r0, dst.c0);
  d->Touch(dst.r1, dst.c1);

  // 3. write the formulas back and rebuild the dependencies
  for (const Fix& fx : fixes) {
    Sheet* s = M(fx.sheet);
    if (s) s->SetFormula(fx.r, fx.c, fx.id);
  }
  for (model::SpillRange sp : cse_moving) {
    M(src_sheet)->spills.mut().erase((uint64_t{sp.rect.r0} << 16) | sp.rect.c0);
    sp.rect = Rect{static_cast<uint32_t>(sp.rect.r0 + drow), static_cast<uint32_t>(sp.rect.c0 + dcol), static_cast<uint32_t>(sp.rect.r1 + drow),
                   static_cast<uint32_t>(sp.rect.c1 + dcol)};
    Sheet* ds = M(dst_sheet);
    for (uint32_t r = sp.rect.r0; r <= sp.rect.r1; ++r) {
      for (uint32_t c = sp.rect.c0; c <= sp.rect.c1; ++c) {
        if (!(r == sp.rect.r0 && c == sp.rect.c0)) ds->SetValue(r, c, Value());  // the array writes them again
      }
    }
    ds->spills.mut()[(uint64_t{sp.rect.r0} << 16) | sp.rect.c0] = sp;
  }
  RebuildAll();
  for (const model::SpillRange& sp : cse_moving) AddPending(Key(dst_sheet, sp.rect.r0, sp.rect.c0));
  // the cells were written without MarkChanged: a table header they cover or leave holds text again (a number or a
  // formula cut onto it, a name cut away), as any other write makes it (doc_fuzz seeds 10280369, 10270301)
  SyncTableHeaders(dst_sheet);
  if (src_sheet != dst_sheet) SyncTableHeaders(src_sheet);
  for (const std::string& name : merged_away) RewriteTableRefs(name, std::string(), std::string(), std::string());  // #REF!
  changes_.push_back(CellChange{src_sheet, src, kSrcUser});
  changes_.push_back(CellChange{dst_sheet, dst, kSrcUser});
  wb_.dirty = true;
  return true;
}

// ---- Evaluate Formula ----

namespace {

formula::Node Literal(const Value& v) {
  formula::Node n;
  switch (v.type()) {
    case core::VType::Number: n.kind = formula::NK::Num; n.num = v.number(); break;
    case core::VType::Decimal: n.kind = formula::NK::Num; n.num = v.AsDouble(); break;
    case core::VType::Bool: n.kind = formula::NK::Bool; n.b = v.boolean(); break;
    case core::VType::Error: n.kind = formula::NK::Err; n.err = v.error(); break;
    case core::VType::Pending: n.kind = formula::NK::Err; n.err = core::Err::GettingData; break;
    case core::VType::StrInline:
    case core::VType::StrHeap: n.kind = formula::NK::Str; n.str = std::string(v.str()); break;
    default: n.kind = formula::NK::Num; n.num = 0; break;
  }
  return n;
}

// the parts of a formula Evaluate Formula steps through, innermost first (paths of child indexes)
void StepNodes(const formula::Node& n, std::vector<size_t>& path, std::vector<std::vector<size_t>>* out) {
  using formula::NK;
  switch (n.kind) {
    case NK::Num: case NK::Str: case NK::Bool: case NK::Err: case NK::Missing: case NK::Array: return;
    case NK::Ref:
      if (!n.ref.is_range && !n.ref.spill && !n.ref.bad) out->push_back(path);  // a single cell shows its value
      return;
    case NK::Name: out->push_back(path); return;
    case NK::Call: out->push_back(path); return;
    case NK::Binary:
      if (n.op == ':') return;  // A1:B2 built from parts: evaluated with its function
      break;
    case NK::Func:
      if (n.str == "LET" || n.str == "LAMBDA") {  // their names only mean something inside: one step
        out->push_back(path);
        return;
      }
      break;
    default: break;
  }
  for (size_t i = 0; i < n.kids.size(); ++i) {
    path.push_back(i);
    StepNodes(n.kids[i], path, out);
    path.pop_back();
  }
  if (n.kind != NK::Paren) out->push_back(path);
}

formula::Node& At(formula::Node& root, const std::vector<size_t>& path) {
  formula::Node* n = &root;
  for (size_t i : path) n = &n->kids[i];
  return *n;
}

}  // namespace

std::vector<EvalStep> Document::EvaluateSteps(uint32_t sheet, uint32_t r, uint32_t c, bool step_in_constant) {
  std::vector<EvalStep> steps;
  const Sheet* s = S(sheet);
  const formula::CompiledFormula* f = s ? Formula(s->GetFormula(r, c)) : nullptr;
  if (!f) {
    // Step In on a cell holding a constant: its value, the only step (Excel: "contains a constant")
    if (s && step_in_constant) {
      const Value& v = s->GetValue(r, c);
      steps.push_back(EvalStep{formula::ToA1(Literal(v), core::CellPos{r, c}, this), 0, 0});
    }
    return steps;
  }
  Recalc();
  const core::CellPos origin{r, c};
  formula::Node work = f->root;
  std::vector<std::vector<size_t>> order;
  std::vector<size_t> path;
  StepNodes(work, path, &order);
  const uint64_t saved = current_cell_;
  current_cell_ = Key(sheet, r, c);
  for (const auto& p : order) {
    formula::Node& target = At(work, p);
    // the formula text with the part to evaluate next marked
    formula::Node keep = target;
    target = formula::Node{};
    target.kind = formula::NK::Name;
    target.str = "\x01\x01";
    std::string text = formula::ToA1(work, origin, this);
    target = std::move(keep);
    const std::string piece = formula::ToA1(target, origin, this);
    const size_t pos = text.find("\x01\x01");
    if (pos == std::string::npos) continue;
    text.replace(pos, 2, piece);
    steps.push_back(EvalStep{"=" + text, pos + 1, piece.size()});
    if (target.kind == formula::NK::Ref && !target.ref.is_range && !target.ref.spill && !target.ref.bad && target.ref.sheet2 < 0) {
      formula::ResolvedRef rr;
      if (formula::ResolveRef(target.ref, origin, &rr) && !IsExternalSheet(rr.sheet < 0 ? static_cast<int32_t>(sheet) : rr.sheet)) {
        steps.back().in_sheet = rr.sheet < 0 ? static_cast<int32_t>(sheet) : rr.sheet;
        steps.back().in_r = rr.rect.r0;
        steps.back().in_c = rr.rect.c0;
      }
    }
    // evaluate that part and put its value in its place
    const formula::EV e = EvaluateNodeAt(sheet, origin, target);
    if (e.k == formula::EV::K::Scalar) {
      target = Literal(e.s);
    } else if (e.k == formula::EV::K::Array && e.a && e.a->v.size() <= 100 && !e.a->v.empty()) {
      formula::Node arr;
      arr.kind = formula::NK::Array;
      arr.rows = e.a->rows;
      arr.cols = e.a->cols;
      for (const Value& v : e.a->v) arr.kids.push_back(Literal(v));
      target = std::move(arr);
    } else if (e.k == formula::EV::K::Lambda) {
      target = Literal(Value::Error(core::Err::Calc));
    }
    // a large range or array stays as written
  }
  current_cell_ = saved;
  steps.push_back(EvalStep{"=" + formula::ToA1(work, origin, this), 0, 0});
  return steps;
}

}  // namespace yc::engine

namespace yc::engine {

bool Document::CutHoldsTable(uint32_t sheet, const Rect& cells) const {
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty()) return false;
  return std::any_of(s->tables->begin(), s->tables->end(), [&](const model::Table& t) { return cells.Contains(t.range); });
}

}  // namespace yc::engine
