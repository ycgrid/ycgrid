// SPDX-License-Identifier: Apache-2.0
#include "engine/navigate.h"

#include "engine/tile_iter.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>

namespace yc::engine {

namespace {

bool Has(const model::Sheet& s, uint32_t r, uint32_t c) { return !s.IsEmptyCell(r, c); }

// next non-blank cell (excluding the start); false when there is none
bool NextNonEmpty(const model::Sheet& s, uint32_t r, uint32_t c, bool vertical, int dir, uint32_t* out) {
  const uint32_t limit = vertical ? s.row_limit : s.col_limit;
  int64_t i = vertical ? r : c;
  while (true) {
    i += dir;
    if (i < 0 || i >= int64_t{limit}) return false;
    const uint32_t rr = vertical ? static_cast<uint32_t>(i) : r, cc = vertical ? c : static_cast<uint32_t>(i);
    // the whole tile doesn't exist: jump straight to the tile boundary
    if (!s.values.GetCell(rr, cc) && !s.meta.GetCell(rr, cc)) {
      const uint32_t span = vertical ? model::kTileRows : model::kTileCols;
      const int64_t base = (i / span) * span;
      i = dir > 0 ? base + span - 1 : base;
      if (!s.has_used) return false;
      // past the used range there is nothing more to find
      if (vertical ? (dir > 0 ? i > s.used.r1 : i < s.used.r0) : (dir > 0 ? i > s.used.c1 : i < s.used.c0)) return false;
      continue;
    }
    if (Has(s, rr, cc)) {
      *out = static_cast<uint32_t>(i);
      return true;
    }
  }
}

}  // namespace

uint32_t CtrlJump(const model::Sheet& s, uint32_t r, uint32_t c, bool vertical, int dir) {
  // hidden rows (columns) are passed over as if they weren't there, as in Excel: Ctrl+Down in a filtered list stops
  // at its last shown row, never on a hidden cell
  const model::DimIndex& dims = vertical ? *s.rows : *s.cols;
  const int64_t limit = vertical ? s.row_limit : s.col_limit;
  auto shown_from = [&](int64_t i) {  // i, or the next shown line past it in dir (out of range when none)
    while (i >= 0 && i < limit && dims.IsHidden(i)) i += dir;
    return i;
  };
  auto at = [&](int64_t i) { return vertical ? Has(s, static_cast<uint32_t>(i), c) : Has(s, r, static_cast<uint32_t>(i)); };
  const uint32_t cur = vertical ? r : c;
  // the last shown line in dir (the edge Ctrl+arrow stops at when nothing is found)
  int64_t edge = dir > 0 ? limit - 1 : 0;
  while (edge != cur && dims.IsHidden(edge)) edge -= dir;
  if (int64_t{cur} == edge) return cur;
  const int64_t next = shown_from(int64_t{cur} + dir);
  if (next < 0 || next >= limit) return cur;
  if (at(cur) && at(next)) {
    // walk to the last cell of the contiguous block
    int64_t i = next;
    while (true) {
      const int64_t n = shown_from(i + dir);
      if (n < 0 || n >= limit || !at(n)) break;
      i = n;
    }
    return static_cast<uint32_t>(i);
  }
  uint32_t rr = r, cc = c, out;
  while (NextNonEmpty(s, rr, cc, vertical, dir, &out)) {
    if (!dims.IsHidden(out)) return out;
    (vertical ? rr : cc) = out;  // a value in a hidden row: look on past it
  }
  return static_cast<uint32_t>(edge);
}

namespace {

// A row or column next to the growing region, with the span already known to be empty: the region grows by one
// line per pass, so the same neighbour line is asked again and again with a span one cell longer each time.
// Scanning only the new cells keeps CurrentRegion linear in the region's area (it was quadratic in its height:
// minutes for 100k rows).
struct Strip {
  int64_t line = -2, lo = 0, hi = -1;
  template <class Probe>
  bool Has(int64_t ln, int64_t a, int64_t b, Probe probe) {
    if (ln != line) {
      line = ln;
      lo = a;
      hi = a - 1;
    }
    for (int64_t i = a; i < lo; ++i)
      if (probe(i)) return true;
    for (int64_t i = std::max(hi + 1, a); i <= b; ++i)
      if (probe(i)) return true;
    lo = std::min(lo, a);
    hi = std::max(hi, b);
    return false;
  }
};

}  // namespace

core::Rect CurrentRegion(const model::Sheet& sh, uint32_t r, uint32_t c) {
  core::Rect a = core::Rect::Cell(r, c);
  if (!sh.has_used) return a;
  const core::Rect& used = sh.used;  // nothing outside it has data
  Strip top, bottom, left, right;
  auto row_has = [&](Strip& st, int64_t rr, uint32_t c0, uint32_t c1) {
    if (rr < 0 || rr >= sh.row_limit || rr < used.r0 || rr > used.r1) return false;
    return st.Has(rr, std::max<int64_t>(c0, used.c0), std::min<int64_t>(c1, used.c1),
                  [&](int64_t cc) { return !sh.IsEmptyCell(static_cast<uint32_t>(rr), static_cast<uint32_t>(cc)); });
  };
  auto col_has = [&](Strip& st, int64_t cc, uint32_t r0, uint32_t r1) {
    if (cc < 0 || cc >= sh.col_limit || cc < used.c0 || cc > used.c1) return false;
    return st.Has(cc, std::max<int64_t>(r0, used.r0), std::min<int64_t>(r1, used.r1),
                  [&](int64_t rr) { return !sh.IsEmptyCell(static_cast<uint32_t>(rr), static_cast<uint32_t>(cc)); });
  };
  while (true) {
    bool grew = false;
    const uint32_t c0 = a.c0 ? a.c0 - 1 : 0, c1 = std::min(a.c1 + 1, sh.col_limit - 1);
    const uint32_t r0 = a.r0 ? a.r0 - 1 : 0, r1 = std::min(a.r1 + 1, sh.row_limit - 1);
    if (a.r0 > 0 && row_has(top, int64_t{a.r0} - 1, c0, c1)) {
      --a.r0;
      grew = true;
    }
    if (row_has(bottom, int64_t{a.r1} + 1, c0, c1)) {
      ++a.r1;
      grew = true;
    }
    if (a.c0 > 0 && col_has(left, int64_t{a.c0} - 1, r0, r1)) {
      --a.c0;
      grew = true;
    }
    if (col_has(right, int64_t{a.c1} + 1, r0, r1)) {
      ++a.c1;
      grew = true;
    }
    if (!grew) break;
  }
  return a;
}

std::string AutoLinkAddress(std::string_view t) {
  if (t.empty() || t.size() > 2000 || t.find_first_of(" \t\r\n") != std::string_view::npos) return {};
  std::string low(t);
  for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto starts = [&](std::string_view p) { return low.size() > p.size() && low.compare(0, p.size(), p) == 0; };
  for (std::string_view scheme : {"http://", "https://", "ftp://"}) {
    if (!starts(scheme)) continue;
    const std::string_view rest = t.substr(scheme.size());
    return std::string(t) + (rest.find('/') == std::string_view::npos ? "/" : "");
  }
  if (starts("www.")) return "http://" + std::string(t) + (t.find('/') == std::string_view::npos ? "/" : "");
  if (starts("mailto:")) return std::string(t);
  const size_t at = t.find('@');
  if (at != std::string_view::npos && at > 0 && at + 1 < t.size() && t.find('@', at + 1) == std::string_view::npos) return "mailto:" + std::string(t);
  return {};
}

bool GuessHeaderRow(const model::StyleTable& styles, const model::Sheet& s, const core::Rect& r) {
  if (r.r1 <= r.r0) return false;
  const uint32_t c1 = std::min<uint32_t>(r.c1, r.c0 + 16383), last = std::min<uint32_t>(r.r1, r.r0 + 1000);
  auto filled = [&](uint32_t rr, uint32_t cc) { return s.GetFormula(rr, cc) != 0 || !s.GetValue(rr, cc).IsEmpty(); };
  // text (a string) or not: a formula counts by its value, a date is a number
  auto text = [&](uint32_t rr, uint32_t cc) { return s.GetValue(rr, cc).IsString(); };
  for (uint32_t c = r.c0; c <= c1; ++c) {
    if (!filled(r.r0, c) || !filled(r.r0 + 1, c)) return false;
  }
  for (uint32_t c = r.c0; c <= c1; ++c) {
    const bool head = text(r.r0, c);
    for (uint32_t rr = r.r0 + 1; rr <= last; ++rr) {
      if (filled(rr, c) && text(rr, c) != head) return true;
    }
    const model::Style& a = styles.Get(s.GetStyle(r.r0, c));
    const model::Style& b = styles.Get(s.GetStyle(r.r0 + 1, c));
    if (a.bold != b.bold || a.italic != b.italic || a.fill_color != b.fill_color || a.font_color != b.font_color) return true;
  }
  return false;
}

core::CellPos LastCell(const model::Sheet& s) {
  if (!s.has_used) return core::CellPos{0, 0};
  return core::CellPos{s.used.r1, s.used.c1};
}

RangeStats ComputeStats(const model::Sheet& s, const core::Rect& r_in, bool visible_only) {
  RangeStats st;
  if (!s.has_used) return st;
  core::Rect r = r_in;
  r.r1 = std::min(r.r1, s.used.r1);
  r.c1 = std::min(r.c1, s.used.c1);
  if (r.r0 > r.r1 || r.c0 > r.c1) return st;
  st.min = std::numeric_limits<double>::infinity();
  st.max = -std::numeric_limits<double>::infinity();
  // a row or column is hidden only where it has a size of its own: without any, every value is shown (a lookup a value
  // took most of the status bar's time over millions of filled cells)
  const bool check = visible_only && (s.rows->custom_count() > 0 || s.cols->custom_count() > 0);
  ForEachValueRowMajor(s, r, [&](uint32_t rr, uint32_t cc, const core::Value& v) {
    if (check && (s.rows->IsHidden(rr) || s.cols->IsHidden(cc))) return true;
    ++st.count;
    if (v.IsNumeric()) {
      const double d = v.AsDouble();
      ++st.numbers;
      st.sum += d;
      st.min = std::min(st.min, d);
      st.max = std::max(st.max, d);
    }
    return true;
  });
  if (st.numbers == 0) st.min = st.max = 0;
  return st;
}

std::vector<core::Rect> CellsToRects(std::vector<core::CellPos> cells) {
  std::sort(cells.begin(), cells.end(), [](const core::CellPos& a, const core::CellPos& b) { return a.row != b.row ? a.row < b.row : a.col < b.col; });
  std::vector<core::Rect> runs;
  for (size_t i = 0; i < cells.size();) {
    size_t j = i;
    while (j + 1 < cells.size() && cells[j + 1].row == cells[i].row && cells[j + 1].col == cells[j].col + 1) ++j;
    runs.push_back(core::Rect{cells[i].row, cells[i].col, cells[i].row, cells[j].col});
    i = j + 1;
  }
  std::vector<core::Rect> out;
  for (const core::Rect& r : runs) {
    bool merged = false;
    // only compare with the runs of the previous row (sorted by row, so search backwards until the previous row)
    for (size_t k = out.size(); k-- > 0;) {
      core::Rect& o = out[k];
      if (o.r1 + 1 < r.r0) break;
      if (o.r1 + 1 == r.r0 && o.c0 == r.c0 && o.c1 == r.c1) {
        o.r1 = r.r0;
        merged = true;
        break;
      }
    }
    if (!merged) out.push_back(r);
  }
  return out;
}

std::vector<core::Rect> SpecialCells(const model::Sheet& s, core::Rect within, Special kind, int values) {
  std::vector<core::Rect> out;
  std::vector<core::CellPos> cells;
  constexpr size_t kMax = 1000000;
  auto type_ok = [&](const core::Value& v) {
    return (v.IsNumeric() && (values & 1)) || (v.IsString() && (values & 2)) || (v.IsBool() && (values & 4)) || (v.IsError() && (values & 16));
  };
  auto clip = [&](const core::Rect& r, core::Rect* o) {
    if (r.r1 < within.r0 || r.r0 > within.r1 || r.c1 < within.c0 || r.c0 > within.c1) return false;
    *o = core::Rect{std::max(r.r0, within.r0), std::max(r.c0, within.c0), std::min(r.r1, within.r1), std::min(r.c1, within.c1)};
    return true;
  };
  switch (kind) {
    case Special::Comments:
      for (const auto& [k, n] : *s.notes) {
        const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        if (within.Contains(r, c)) cells.push_back(core::CellPos{r, c});
      }
      break;
    case Special::LastCell:
      if (s.has_used) out.push_back(core::Rect::Cell(s.used.r1, s.used.c1));
      return out;
    case Special::AllFormatConditions:
    case Special::AllValidation: {
      std::vector<core::Rect> src;
      if (kind == Special::AllFormatConditions) {
        for (const auto& rule : *s.cond_formats) src.insert(src.end(), rule.ranges.begin(), rule.ranges.end());
      } else {
        for (const auto& rule : *s.validations) src.insert(src.end(), rule.ranges.begin(), rule.ranges.end());
      }
      for (const core::Rect& r : src) {
        core::Rect o;
        if (clip(r, &o)) out.push_back(o);
      }
      return out;
    }
    case Special::Visible: {
      // visible cells aren't limited to the used range (Excel: B1:B4 with rows 2:3 hidden is B1,B4 even when B is
      // empty): the runs of visible rows times the runs of visible columns
      std::vector<std::pair<uint32_t, uint32_t>> rr, cr;
      for (uint32_t r = within.r0; r <= within.r1; ++r) {
        if (!s.rows->IsHidden(r)) {
          if (!rr.empty() && rr.back().second + 1 == r) rr.back().second = r;
          else rr.emplace_back(r, r);
        }
        if (r == UINT32_MAX) break;
      }
      for (uint32_t c = within.c0; c <= within.c1; ++c) {
        if (s.cols->IsHidden(c)) continue;
        if (!cr.empty() && cr.back().second + 1 == c) cr.back().second = c;
        else cr.emplace_back(c, c);
      }
      for (const auto& [r0, r1] : rr) {
        for (const auto& [c0, c1] : cr) out.push_back(core::Rect{r0, c0, r1, c1});
      }
      return out;
    }
    case Special::Blanks: {
      // blanks are not limited to tiles with values: limit them to the used range (like Excel)
      if (!s.has_used) return out;
      core::Rect w;
      if (!clip(core::Rect{0, 0, s.used.r1, s.used.c1}, &w)) return out;
      for (uint32_t r = w.r0; r <= w.r1 && cells.size() < kMax; ++r) {
        for (uint32_t c = w.c0; c <= w.c1; ++c) {
          if (s.GetValue(r, c).IsEmpty() && s.GetFormula(r, c) == 0) cells.push_back(core::CellPos{r, c});
        }
      }
      break;
    }
    case Special::Constants:
    case Special::Formulas: {
      if (!s.has_used) return out;
      core::Rect w;
      if (!clip(core::Rect{0, 0, s.used.r1, s.used.c1}, &w)) return out;
      const bool want_f = kind == Special::Formulas;
      auto scan = [&](uint32_t rt, uint32_t ct) {
        const uint32_t br = rt * model::kTileRows, bc = ct * model::kTileCols;
        for (uint32_t r = std::max(br, w.r0); r <= std::min(br + model::kTileRows - 1, w.r1); ++r) {
          for (uint32_t c = std::max(bc, w.c0); c <= std::min(bc + model::kTileCols - 1, w.c1); ++c) {
            const core::Value& v = s.GetValue(r, c);
            const bool f = s.GetFormula(r, c) != 0;
            if (f != want_f || (!f && v.IsEmpty())) continue;
            if (type_ok(v) && cells.size() < kMax) cells.push_back(core::CellPos{r, c});
          }
        }
      };
      // formulas go through MetaTile (a formula cell's value may be empty); constants through ValueTile
      if (want_f) {
        ForEachTileInRect(s.meta, w, [&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
          if (t.formula_count) scan(rt, ct);
        });
      } else {
        ForEachTileInRect(s.values, w, [&](uint32_t rt, uint32_t ct, const model::ValueTile&) { scan(rt, ct); });
      }
      break;
    }
  }
  return CellsToRects(std::move(cells));
}

std::vector<core::Rect> DifferenceCells(const model::Sheet& s, core::Rect within, uint32_t cmp_r, uint32_t cmp_c, bool rows) {
  std::vector<core::CellPos> cells;
  if (!s.has_used) return {};
  within.r1 = std::min(within.r1, s.used.r1);
  within.c1 = std::min(within.c1, s.used.c1);
  if (within.r0 > within.r1 || within.c0 > within.c1) return {};
  // a formula as the cell's offset to it would read in the comparison cell (the formula id is shared by copies)
  auto same = [&](uint32_t r, uint32_t c, uint32_t r2, uint32_t c2) {
    const uint32_t f1 = s.GetFormula(r, c), f2 = s.GetFormula(r2, c2);
    if (f1 || f2) return f1 == f2;
    const core::Value& a = s.GetValue(r, c);
    const core::Value& b = s.GetValue(r2, c2);
    if (a.IsEmpty() || b.IsEmpty()) return a.IsEmpty() == b.IsEmpty();
    if (a.IsNumeric() && b.IsNumeric()) return a.AsDouble() == b.AsDouble();
    if (a.IsString() && b.IsString()) return a.str() == b.str();
    if (a.IsBool() && b.IsBool()) return a.boolean() == b.boolean();
    return false;
  };
  // two empty cells are the same: a line (row for row differences, column for column differences) needs a look only
  // where it holds something, or where its comparison cell does. Comparing every cell of the whole sheet with a value
  // in XFD1048576 was 17 billion comparisons (scale_sweep: Column Differences froze YCGrid)
  auto filled = [&](uint32_t r, uint32_t c) { return s.GetFormula(r, c) != 0 || !s.GetValue(r, c).IsEmpty(); };
  std::map<uint32_t, std::vector<uint32_t>> by_line;  // line -> the places along it that hold something
  auto note = [&](uint32_t r, uint32_t c) { by_line[rows ? r : c].push_back(rows ? c : r); };
  ForEachCellInTiles(s.values, within, [&](uint32_t r, uint32_t c, const model::ValueTile& t, uint32_t idx) {
    if (t.Occupied(idx)) note(r, c);
  });
  ForEachCellInTiles(s.meta, within, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.formula[idx] && s.GetValue(r, c).IsEmpty()) note(r, c);  // a formula without a value yet
  });
  // the lines whose comparison cell holds something: every place along them is compared
  const uint32_t lo = rows ? within.r0 : within.c0, hi = rows ? within.r1 : within.c1;
  const core::Rect cmp_line = rows ? core::Rect{within.r0, cmp_c, within.r1, cmp_c} : core::Rect{cmp_r, within.c0, cmp_r, within.c1};
  std::set<uint32_t> full;
  ForEachCellInTiles(s.values, cmp_line, [&](uint32_t r, uint32_t c, const model::ValueTile& t, uint32_t idx) {
    if (t.Occupied(idx)) full.insert(rows ? r : c);
  });
  ForEachCellInTiles(s.meta, cmp_line, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.formula[idx]) full.insert(rows ? r : c);
  });
  for (uint32_t line : full) by_line.try_emplace(line);
  const uint32_t a0 = rows ? within.c0 : within.r0, a1 = rows ? within.c1 : within.r1;  // the places along a line
  for (auto& [line, places] : by_line) {
    if (cells.size() >= 1000000) break;
    if (line < lo || line > hi || (rows ? s.rows->IsHidden(line) : s.cols->IsHidden(line))) continue;
    const uint32_t r2 = rows ? line : cmp_r, c2 = rows ? cmp_c : line;
    auto check = [&](uint32_t at) {
      const uint32_t r = rows ? line : at, c = rows ? at : line;
      if ((rows ? s.cols->IsHidden(c) : s.rows->IsHidden(r)) || (r == r2 && c == c2)) return;
      if (!same(r, c, r2, c2)) cells.push_back(core::CellPos{r, c});
    };
    if (filled(r2, c2)) {
      for (uint32_t at = a0; at <= a1 && cells.size() < 1000000; ++at) check(at);
    } else {
      std::sort(places.begin(), places.end());
      places.erase(std::unique(places.begin(), places.end()), places.end());
      for (uint32_t at : places) check(at);
    }
  }
  return CellsToRects(std::move(cells));
}

}  // namespace yc::engine
