// SPDX-License-Identifier: Apache-2.0
// document_struct.cpp - insert / delete rows, columns and cells (with formula reference offsets), sheets, defined names
#include "engine/document.h"
#include "formula/functions.h"
#include "core/i18n.h"
#include "core/geometry.h"
#include "core/utf8.h"

#include "engine/tile_iter.h"
#include "model/ref_texts.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <tuple>
#include <cctype>
#include <unordered_map>

namespace yc::engine {

using model::Sheet;

namespace {

struct ShiftSpec {
  uint32_t sheet;
  bool rows;        // true = offset along rows
  int64_t at;
  int64_t delta;    // > 0 insert; < 0 delete
  bool has_band = false;
  int64_t b0 = 0, b1 = 0;  // range in the other dimension (when inserting / deleting cells)
};

bool MapIdx(const ShiftSpec& sp, int64_t x, int64_t* out) {
  if (sp.delta > 0) {
    *out = x >= sp.at ? x + sp.delta : x;
    return true;
  }
  const int64_t n = -sp.delta;
  if (x < sp.at) {
    *out = x;
    return true;
  }
  if (x >= sp.at + n) {
    *out = x - n;
    return true;
  }
  return false;
}

// range end point: false means the whole span was deleted
bool MapSpan(const ShiftSpec& sp, int64_t lo, int64_t hi, int64_t* olo, int64_t* ohi) {
  if (sp.delta > 0) {
    *olo = lo >= sp.at ? lo + sp.delta : lo;
    *ohi = hi >= sp.at ? hi + sp.delta : hi;
    return true;
  }
  const int64_t n = -sp.delta, end = sp.at + n;
  if (hi < sp.at) {
    *olo = lo;
    *ohi = hi;
    return true;
  }
  if (lo >= end) {
    *olo = lo - n;
    *ohi = hi - n;
    return true;
  }
  if (lo >= sp.at && hi < end) return false;
  *olo = lo < sp.at ? lo : sp.at;
  *ohi = hi >= end ? hi - n : sp.at - 1;
  return true;
}

bool InBand(const ShiftSpec& sp, int64_t lo, int64_t hi) { return !sp.has_band || (lo >= sp.b0 && hi <= sp.b1); }

// an insertion pushes a reference past the sheet's edge: a cell (or a range's start) pushed off is #REF!, a range's end
// stops at the edge. Kept as an offset past XFD, =B3*2 read #REF!*2 and only a save and an open made it one (two
// formulas of one text until then: doc_fuzz seed 33240081, cells A3:XFD5 inserted to the right)
bool PastEdge(const ShiftSpec& sp, bool range, int64_t* lo, int64_t* hi) {
  if (sp.delta <= 0) return false;
  // (rows: the hard limit, as ShiftSheet moves cells: a sheet grows past its rows when cells are pushed down)
  const int64_t limit = sp.rows ? int64_t{core::kMaxRows} : int64_t{core::kMaxCols};
  if (*lo >= limit) return true;
  if (range && *hi >= limit) *hi = limit - 1;
  return false;
}

// Cells deleted with a shift trim a range that reaches past the band when one of its edges lies wholly in the deleted
// cells, as in Excel (measured): deleting B2:B5 (shift up) turns B2:C3 into C2:C3 and A2:B3 into A2:A3, deleting B2:D3
// (shift left) turns B3:D4 into B4:D4. A range that only crosses the deleted cells keeps its text (B1:C3 there), and
// so do whole rows and columns. Returns whether the reference (read at old_o) is trimmed; with `write` the trimmed edge
// is stored, still relative to old_o.
bool TrimDeletedEdge(formula::RefSpec& ref, core::CellPos old_o, const ShiftSpec& sp, bool write) {
  if (!sp.has_band || sp.delta >= 0 || !ref.is_range || ref.whole_rows || ref.whole_cols || ref.spill) return false;
  formula::RefCoord& xa = sp.rows ? ref.r0 : ref.c0;
  formula::RefCoord& xb = sp.rows ? ref.r1 : ref.c1;
  formula::RefCoord& ya = sp.rows ? ref.c0 : ref.r0;
  formula::RefCoord& yb = sp.rows ? ref.c1 : ref.r1;
  const int64_t ox = sp.rows ? old_o.row : old_o.col, oy = sp.rows ? old_o.col : old_o.row;
  const int64_t x0 = std::min(xa.Resolve(ox), xb.Resolve(ox)), x1 = std::max(xa.Resolve(ox), xb.Resolve(ox));
  const int64_t ya_v = ya.Resolve(oy), yb_v = yb.Resolve(oy);
  const int64_t y0 = std::min(ya_v, yb_v), y1 = std::max(ya_v, yb_v);
  if (InBand(sp, y0, y1) || x0 < sp.at || x1 >= sp.at - sp.delta) return false;  // inside the band, or not all deleted
  int64_t n0 = y0, n1 = y1;
  if (y0 >= sp.b0 && y0 <= sp.b1 && y1 > sp.b1) n0 = sp.b1 + 1;       // its first columns (rows) were deleted
  else if (y1 >= sp.b0 && y1 <= sp.b1 && y0 < sp.b0) n1 = sp.b0 - 1;  // its last ones
  else return false;
  if (write) {
    formula::RefCoord& lo = ya_v <= yb_v ? ya : yb;
    formula::RefCoord& hi = ya_v <= yb_v ? yb : ya;
    lo.v = lo.abs ? n0 : n0 - oy;
    hi.v = hi.abs ? n1 : n1 - oy;
  }
  return true;
}

// adjust one formula's AST; returns whether anything changed
// whether AdjustAst would change any reference of these (a formula's references, cached): most formulas move with
// what they read, and copying and interning each of millions of them took most of the time of an inserted row
bool SpecsChange(const std::vector<formula::RefSpec>& refs, uint32_t fsheet, core::CellPos old_o, core::CellPos new_o, const ShiftSpec& sp) {
  for (const formula::RefSpec& ref : refs) {
    if (!ref.HasCells()) continue;
    const uint32_t target = static_cast<uint32_t>(ref.TargetSheet(static_cast<int32_t>(fsheet)));
    if (target == sp.sheet && ref.sheet2 < 0) {
      formula::RefSpec probe = ref;
      if (TrimDeletedEdge(probe, old_o, sp, false)) return true;
    }
    for (int dim = 0; dim < 2; ++dim) {
      const bool row_dim = dim == 0;
      if (row_dim && ref.whole_cols) continue;
      if (!row_dim && ref.whole_rows) continue;
      const formula::RefCoord& a = row_dim ? ref.r0 : ref.c0;
      const formula::RefCoord& b = row_dim ? ref.r1 : ref.c1;
      const int64_t oo = row_dim ? old_o.row : old_o.col, no = row_dim ? new_o.row : new_o.col;
      const int64_t x0 = a.Resolve(oo), x1 = ref.is_range ? b.Resolve(oo) : x0;
      int64_t n0 = x0, n1 = x1;
      const bool shift_dim = target == sp.sheet && row_dim == sp.rows && ref.sheet2 < 0;
      if (shift_dim) {
        bool band_ok = true;
        if (sp.has_band) {
          const formula::RefCoord& oa = row_dim ? ref.c0 : ref.r0;
          const formula::RefCoord& ob = row_dim ? ref.c1 : ref.r1;
          const int64_t ooo = row_dim ? old_o.col : old_o.row;
          int64_t y0 = oa.Resolve(ooo), y1 = ref.is_range ? ob.Resolve(ooo) : y0;
          if (y0 > y1) std::swap(y0, y1);
          if (row_dim ? ref.whole_rows : ref.whole_cols) band_ok = false;
          else band_ok = InBand(sp, y0, y1);
        }
        if (band_ok) {
          const bool swapped = x0 > x1;
          int64_t lo = std::min(x0, x1), hi = std::max(x0, x1);
          if (ref.is_range) {
            if (!MapSpan(sp, lo, hi, &lo, &hi)) return true;
          } else if (!MapIdx(sp, lo, &lo)) {
            return true;
          } else {
            hi = lo;
          }
          if (PastEdge(sp, ref.is_range, &lo, &hi)) return true;
          n0 = swapped ? hi : lo;
          n1 = swapped ? lo : hi;
        }
      }
      if ((a.abs ? n0 : n0 - no) != a.v) return true;
      if (ref.is_range && (b.abs ? n1 : n1 - no) != b.v) return true;
    }
  }
  return false;
}

bool AdjustAst(formula::Node& root, uint32_t fsheet, core::CellPos old_o, core::CellPos new_o, const ShiftSpec& sp) {
  bool changed = old_o.row != new_o.row || old_o.col != new_o.col;
  formula::ForEachRef(root, [&](formula::RefSpec& ref) {
    if (!ref.HasCells()) return;
    const uint32_t target = static_cast<uint32_t>(ref.TargetSheet(static_cast<int32_t>(fsheet)));
    // an edge deleted whole goes first: the range is then outside the band and doesn't shift
    if (target == sp.sheet && ref.sheet2 < 0 && TrimDeletedEdge(ref, old_o, sp, true)) changed = true;
    // the row and column dimensions are handled separately
    for (int dim = 0; dim < 2; ++dim) {
      const bool row_dim = dim == 0;
      if (row_dim && ref.whole_cols) continue;
      if (!row_dim && ref.whole_rows) continue;
      formula::RefCoord& a = row_dim ? ref.r0 : ref.c0;
      formula::RefCoord& b = row_dim ? ref.r1 : ref.c1;
      const int64_t oo = row_dim ? old_o.row : old_o.col, no = row_dim ? new_o.row : new_o.col;
      int64_t x0 = a.Resolve(oo), x1 = ref.is_range ? b.Resolve(oo) : x0;
      int64_t n0 = x0, n1 = x1;
      const bool shift_dim = target == sp.sheet && row_dim == sp.rows && ref.sheet2 < 0;  // 3D references don't follow one sheet
      if (shift_dim) {
        // whether the other dimension is inside the band
        bool band_ok = true;
        if (sp.has_band) {
          const formula::RefCoord& oa = row_dim ? ref.c0 : ref.r0;
          const formula::RefCoord& ob = row_dim ? ref.c1 : ref.r1;
          const int64_t ooo = row_dim ? old_o.col : old_o.row;
          int64_t y0 = oa.Resolve(ooo), y1 = ref.is_range ? ob.Resolve(ooo) : y0;
          if (y0 > y1) std::swap(y0, y1);
          if (row_dim ? ref.whole_rows : ref.whole_cols) band_ok = false;
          else band_ok = InBand(sp, y0, y1);
        }
        if (band_ok) {
          const bool swapped = x0 > x1;
          int64_t lo = std::min(x0, x1), hi = std::max(x0, x1);
          if (ref.is_range) {
            if (!MapSpan(sp, lo, hi, &lo, &hi)) {
              ref.bad = true;
              changed = true;
              return;
            }
          } else if (!MapIdx(sp, lo, &lo)) {
            ref.bad = true;
            changed = true;
            return;
          } else {
            hi = lo;
          }
          if (PastEdge(sp, ref.is_range, &lo, &hi)) {
            ref.bad = true;
            changed = true;
            return;
          }
          n0 = swapped ? hi : lo;
          n1 = swapped ? lo : hi;
        }
      }
      const int64_t v0 = a.abs ? n0 : n0 - no;
      if (v0 != a.v) {
        a.v = v0;
        changed = true;
      }
      if (ref.is_range) {
        const int64_t v1 = b.abs ? n1 : n1 - no;
        if (v1 != b.v) {
          b.v = v1;
          changed = true;
        }
      }
    }
  });
  return changed;
}

}  // namespace

// formula offsets and moving the data
void Document::AdjustFormulas(uint32_t sheet, bool rows, uint32_t at, int64_t delta, const Rect* band) {
  // What has to be calculated again afterwards: formulas keep their values when they move with what they read.
  // `again` holds new positions, `again_old` positions before the shift (mapped at the end). Recalculating every
  // formula of the workbook took seconds with millions of them.
  std::vector<uint64_t> again, again_old;
  if (const Sheet* s0 = S(sheet)) {
    for (const auto& [k, spr] : *s0->spills) {  // dynamic arrays spill again
      if (spr.cse) continue;
      again_old.push_back(Key(sheet, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF)));
      // and what read their spilled values: the spills are cleared below, and an anchor deleted with its row never
      // spills again, so =B3*2 kept the value it had read from =SEQUENCE(3) in B2 (doc_fuzz seed 10660583)
      deps_.QueryRect(sheet, spr.rect, [&](uint64_t d) { again_old.push_back(d); });
    }
  }
  for (const auto& [k, want] : spill_blocked_) again_old.push_back(k);  // #SPILL! anchors: room may have changed
  // circular references are found again at their new places
  again_old.insert(again_old.end(), circular_cells.begin(), circular_cells.end());
  circular_cells.clear();
  // formulas written but not calculated yet stay pending at their new places: the rebuild below empties the pending
  // set, and a script that wrote formulas and deleted a row at once read blanks from all of them
  again_old.insert(again_old.end(), pending_.begin(), pending_.end());
  if (band) {
    // cells inserted or deleted with a shift: everything from `at` on in the band moves, and a range that reaches
    // past the band (=SUM(A11:H28) when only A:C shift) keeps its text but reads other cells now
    const Rect moved = rows ? Rect{at, band->c0, core::kMaxRows - 1, band->c1} : Rect{band->r0, at, band->r1, core::kMaxCols - 1};
    deps_.QueryRect(sheet, moved, [&](uint64_t k) { again_old.push_back(k); });
  } else if (delta < 0) {  // what reads the deleted rows / columns (A:A keeps its text but loses the values)
    const uint32_t last = static_cast<uint32_t>(int64_t{at} - delta - 1);
    const Rect gone = rows ? Rect{at, 0, last, core::kMaxCols - 1} : Rect{0, at, core::kMaxRows - 1, last};
    deps_.QueryRect(sheet, gone, [&](uint64_t k) { again_old.push_back(k); });
  }
  std::unordered_map<uint32_t, bool> positional;  // formula id -> uses ROW() / COLUMN() of its own cell
  auto uses_position = [&](uint32_t id) {
    auto it = positional.find(id);
    if (it != positional.end()) return it->second;
    bool found = false;
    if (const auto* f = Formula(id)) {
      std::vector<const formula::Node*> todo{&f->root};
      while (!todo.empty() && !found) {
        const formula::Node* n = todo.back();
        todo.pop_back();
        found = n->kind == formula::NK::Func && n->kids.empty() && (n->str == "ROW" || n->str == "COLUMN");
        for (const auto& k : n->kids) todo.push_back(&k);
      }
    }
    return positional.emplace(id, found).first->second;
  };
  ClearSheetSpills(sheet);  // dynamic arrays spill again after the move
  ShiftSpec sp{sheet, rows, at, delta};
  if (band) {
    sp.has_band = true;
    sp.b0 = rows ? band->c0 : band->r0;
    sp.b1 = rows ? band->c1 : band->r1;
  }

  // 1. every formula cell: compute the new def (using old coordinates)
  struct Fix {
    uint32_t fsheet, r, c;  // old position
    uint32_t id;
  };
  // Only two kinds of formula can change: those in the cells that move, and those reading them (the dependency index
  // knows these, on every sheet). Looking at every formula of the workbook made deleting an empty column of a sheet
  // with two million formulas take most of a second.
  Rect region = rows ? Rect{at, 0, core::kMaxRows - 1, core::kMaxCols - 1} : Rect{0, at, core::kMaxRows - 1, core::kMaxCols - 1};
  if (band) {
    if (rows) region.c0 = band->c0, region.c1 = band->c1;
    else region.r0 = band->r0, region.r1 = band->r1;
  }
  std::vector<Fix> fixes;
  if (const Sheet* s0 = S(sheet)) {
    ForEachTileInRect(s0->meta, region, [&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (!t.formula[idx]) continue;
        const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
        if (region.Contains(r, c)) fixes.push_back(Fix{sheet, r, c, t.formula[idx]});
      }
    });
  }
  {
    std::vector<uint64_t> readers;
    deps_.QueryRect(sheet, region, [&](uint64_t k) {
      if (!(KeySheet(k) == sheet && region.Contains(KeyRow(k), KeyCol(k)))) readers.push_back(k);  // not counted twice
    });
    std::sort(readers.begin(), readers.end());
    readers.erase(std::unique(readers.begin(), readers.end()), readers.end());
    for (uint64_t k : readers) {
      const Sheet* rs = S(KeySheet(k));
      if (const uint32_t id = rs ? rs->GetFormula(KeyRow(k), KeyCol(k)) : 0) fixes.push_back(Fix{KeySheet(k), KeyRow(k), KeyCol(k), id});
    }
  }
  struct NewDef {
    uint32_t fsheet, r, c;  // new position
    uint32_t id;
  };
  std::vector<NewDef> results;
  results.reserve(fixes.size());
  bool any_moved = false;              // a formula moved or its definition changed: the index is rebuilt
  std::vector<uint64_t> dropped;       // formulas deleted with their rows / columns
  std::vector<uint64_t> table_cells;   // formulas reading tables (their table may have moved or grown)
  for (const Fix& fx : fixes) {
    const core::CellPos oo{fx.r, fx.c};
    core::CellPos no = oo;
    if (fx.fsheet == sheet) {
      const int64_t main = rows ? fx.r : fx.c, other = rows ? fx.c : fx.r;
      if (InBand(sp, other, other)) {
        int64_t m;
        if (!MapIdx(sp, main, &m)) {  // the formula cell itself was deleted
          dropped.push_back(Key(fx.fsheet, fx.r, fx.c));
          continue;
        }
        if (rows) no.row = static_cast<uint32_t>(m);
        else no.col = static_cast<uint32_t>(m);
      }
    }
    const auto* f = Formula(fx.id);
    if (!f) continue;
    uint32_t id = fx.id;
    if (SpecsChange(RefInfoOf(fx.id, f).refs, fx.fsheet, oo, no, sp)) {
      formula::Node copy = f->root;
      if (AdjustAst(copy, fx.fsheet, oo, no, sp)) id = InternFormula(std::move(copy));
    }
    results.push_back(NewDef{fx.fsheet, no.row, no.col, id});
    const bool moved = no.row != oo.row || no.col != oo.col;
    const bool table = f->key.find("T'") != std::string::npos;
    any_moved = any_moved || moved || id != fx.id;
    if (table) table_cells.push_back(Key(fx.fsheet, no.row, no.col));
    // A:A keeps its text while rows are inserted into it or deleted (MATCH, INDEX, LOOKUP give other answers), and
    // 1:1 while columns are
    bool across = false;
    for (const formula::RefSpec& rs : RefInfoOf(fx.id, f).refs) {
      const uint32_t target = static_cast<uint32_t>(rs.TargetSheet(static_cast<int32_t>(fx.fsheet)));
      across = across || (target == sheet && (rows ? rs.whole_cols : rs.whole_rows));
    }
    if (id != fx.id || table || across || (moved && uses_position(id))) {
      again.push_back(Key(fx.fsheet, no.row, no.col));  // its references changed, a table's, or its own position
    }
  }

  // conditional formats' and data validation's formulas (any sheet's) follow the cells they point at, as cell
  // formulas do: each is relative to its rule's first cell, which may move too (=$A$10 becomes =$A$12 when two rows go
  // in above row 10). Done before the move, while the rules' ranges are where the formulas were written.
  for (auto& sc : wb_.sheets) {
    if (sc->cond_formats->empty() && sc->validations->empty()) continue;
    const uint32_t sid = sc->id;
    // the rule's first cell after the move (its first range mapped like the ranges are in ShiftSheet)
    auto new_base = [&](const Rect& first, core::CellPos* out) {
      *out = core::CellPos{first.r0, first.c0};
      if (sid != sheet) return true;
      if (band && !(rows ? (first.c0 >= band->c0 && first.c1 <= band->c1) : (first.r0 >= band->r0 && first.r1 <= band->r1))) return true;
      int64_t lo = 0, hi = 0;
      if (!MapSpan(sp, rows ? first.r0 : first.c0, rows ? first.r1 : first.c1, &lo, &hi)) return false;
      (rows ? out->row : out->col) = static_cast<uint32_t>(lo);
      return true;
    };
    auto adjust_rule_text = [&](std::string& text, core::CellPos oo, core::CellPos no, bool constants_ok) {
      if (text.empty()) return;
      std::string_view t = text;
      const bool eq = t[0] == '=';
      if (!eq && constants_ok) return;  // a list of values (Buy,Sell) is no formula
      if (eq) t.remove_prefix(1);
      formula::ParseResult pr = formula::Parse(t, oo, this);
      if (!pr.ok) return;
      bool refs = false;
      formula::ForEachRef(pr.root, [&](formula::RefSpec&) { refs = true; });
      if (!refs) return;
      if (AdjustAst(pr.root, sid, oo, no, sp)) text = (eq ? "=" : "") + formula::ToA1(pr.root, no, this);
    };
    if (!sc->cond_formats->empty()) {
      bool any = false;
      std::vector<model::CfRule> v = *sc->cond_formats;
      for (auto& r : v) {
        core::CellPos no;
        if (r.ranges.empty() || !new_base(r.ranges.front(), &no)) continue;
        const core::CellPos oo{r.ranges.front().r0, r.ranges.front().c0};
        const model::CfRule before = r;
        model::ForEachCfRefText(r, [&](std::string& f) { adjust_rule_text(f, oo, no, false); });
        any = any || !(r == before);
      }
      if (any) sc.mut().cond_formats.mut() = std::move(v);
    }
    if (!sc->validations->empty()) {
      bool any = false;
      std::vector<model::DvRule> v = *sc->validations;
      for (auto& r : v) {
        core::CellPos no;
        if (r.ranges.empty() || !new_base(r.ranges.front(), &no)) continue;
        const core::CellPos oo{r.ranges.front().r0, r.ranges.front().c0};
        const std::string f1 = r.formula1, f2 = r.formula2;
        model::ForEachDvRefText(r, [&](std::string& f, bool constants_ok) { adjust_rule_text(f, oo, no, constants_ok); });
        any = any || f1 != r.formula1 || f2 != r.formula2;
      }
      if (any) sc.mut().validations.mut() = std::move(v);
    }
  }

  // 2. move the data
  ShiftSheet(sheet, rows, at, delta, band);
  SyncTableHeaders(sheet);  // inserted table columns get names
  // sparklines: the cells they are in (this sheet) and their data (any sheet's sparklines reading this one)
  for (auto& sc : wb_.sheets) {
    if (sc->sparklines->empty()) continue;
    const bool here = sc->id == sheet;
    bool touched = false;
    for (const auto& g : *sc->sparklines) {
      for (const auto& it : g.items) touched = touched || here || it.src_sheet == sheet;
    }
    if (!touched) continue;
    auto& groups = sc.mut().sparklines.mut();
    for (auto& g : groups) {
      std::vector<model::Sparkline> keep;
      for (auto it : g.items) {
        if (here) {
          const int64_t main = rows ? it.r : it.c, other = rows ? it.c : it.r;
          int64_t m2 = main;
          if (InBand(sp, other, other) && !MapIdx(sp, main, &m2)) continue;  // its cell was deleted
          if (rows) it.r = static_cast<uint32_t>(m2);
          else it.c = static_cast<uint32_t>(m2);
        }
        if (it.src_sheet == sheet) {
          const int64_t lo = rows ? it.src.r0 : it.src.c0, hi = rows ? it.src.r1 : it.src.c1;
          const int64_t olo = rows ? it.src.c0 : it.src.r0, ohi = rows ? it.src.c1 : it.src.r1;
          int64_t a = lo, b = hi;
          if (InBand(sp, olo, ohi) && !MapSpan(sp, lo, hi, &a, &b)) continue;  // its data was deleted
          if (rows) {
            it.src.r0 = static_cast<uint32_t>(a);
            it.src.r1 = static_cast<uint32_t>(b);
          } else {
            it.src.c0 = static_cast<uint32_t>(a);
            it.src.c1 = static_cast<uint32_t>(b);
          }
        }
        keep.push_back(it);
      }
      g.items = std::move(keep);
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const model::SparklineGroup& g) { return g.items.empty(); }), groups.end());
  }
  // scenarios: their changing cells move with the cells (Excel keeps them as references); a deleted cell leaves them
  if (const Sheet* ss = S(sheet); ss && !ss->scenarios->empty()) {
    auto& scs = M(sheet)->scenarios.mut();
    for (auto& sc : scs) {
      std::vector<core::CellPos> cells;
      std::vector<core::Value> values;
      for (size_t i = 0; i < sc.cells.size(); ++i) {
        core::CellPos p = sc.cells[i];
        const int64_t main = rows ? p.row : p.col, other = rows ? p.col : p.row;
        int64_t m2 = main;
        if (InBand(sp, other, other) && !MapIdx(sp, main, &m2)) continue;  // its cell was deleted
        if (rows) p.row = static_cast<uint32_t>(m2);
        else p.col = static_cast<uint32_t>(m2);
        cells.push_back(p);
        if (i < sc.values.size()) values.push_back(sc.values[i]);
      }
      sc.cells = std::move(cells);
      sc.values = std::move(values);
    }
  }
  // data tables (What-If > Data Table): the table and its input cells move with the cells, as in Excel; a table whose
  // cells are all deleted goes
  if (const Sheet* ts = S(sheet); ts && !ts->data_tables->empty()) {
    std::vector<model::DataTable> keep;
    for (model::DataTable t : *ts->data_tables) {
      const int64_t lo = rows ? t.rect.r0 : t.rect.c0, hi = rows ? t.rect.r1 : t.rect.c1;
      const int64_t olo = rows ? t.rect.c0 : t.rect.r0, ohi = rows ? t.rect.c1 : t.rect.r1;
      if (InBand(sp, olo, ohi)) {
        int64_t a = lo, b = hi;
        if (!MapSpan(sp, lo, hi, &a, &b)) continue;
        if (rows) t.rect.r0 = static_cast<uint32_t>(a), t.rect.r1 = static_cast<uint32_t>(b);
        else t.rect.c0 = static_cast<uint32_t>(a), t.rect.c1 = static_cast<uint32_t>(b);
      }
      auto input = [&](uint32_t* r, uint32_t* c) {
        const int64_t main = rows ? *r : *c, other = rows ? *c : *r;
        int64_t m2 = main;
        if (!InBand(sp, other, other) || !MapIdx(sp, main, &m2)) return;  // a deleted input cell keeps its place
        (rows ? *r : *c) = static_cast<uint32_t>(m2);
      };
      if (t.has_row) input(&t.row_r, &t.row_c);
      if (t.has_col) input(&t.col_r, &t.col_c);
      keep.push_back(t);
    }
    M(sheet)->data_tables.mut() = std::move(keep);
    tables_dirty_ = true;
  }
  // PivotTables: the report on this sheet moves with the cells around it (what would cut it was refused before it:
  // RefusePivotShift), and a report whose lines are all deleted goes with them, its PivotCharts turned static, as in
  // Excel (measured); sources on this sheet follow whole rows / columns only
  {
    std::vector<std::string> gone;
    for (auto& sc : wb_.sheets) {
      if (sc->pivots->empty()) continue;
      const bool here = sc->id == sheet;
      bool touched = false;
      for (const auto& p : *sc->pivots) touched = touched || here || (!band && p.src_sheet == sheet);
      if (!touched) continue;
      auto& v = sc.mut().pivots.mut();
      for (auto it = v.begin(); it != v.end();) {
        model::PivotTable& p = *it;
        auto span = [&](uint32_t& lo, uint32_t& hi) {
          int64_t a = lo, b = hi;
          if (!MapSpan(sp, lo, hi, &a, &b)) return false;
          lo = static_cast<uint32_t>(a);
          hi = static_cast<uint32_t>(b);
          return true;
        };
        if (here) {
          const Rect area = PivotArea(p);
          if (InBand(sp, rows ? area.c0 : area.r0, rows ? area.c1 : area.r1)) {
            int64_t a = rows ? area.r0 : area.c0, b = rows ? area.r1 : area.c1;
            if (!MapSpan(sp, a, b, &a, &b)) {
              gone.push_back(p.name);
              it = v.erase(it);
              continue;
            }
            uint32_t& pos = rows ? p.r0 : p.c0;
            int64_t m = pos;
            if (MapIdx(sp, pos, &m)) pos = static_cast<uint32_t>(m);
            if (p.has_output && !(rows ? span(p.output.r0, p.output.r1) : span(p.output.c0, p.output.c1))) p.has_output = false;
          }
        }
        if (!band && p.src_sheet == sheet && p.src_table.empty()) {
          if (rows) span(p.src.r0, p.src.r1);
          else span(p.src.c0, p.src.c1);
        }
        ++it;
      }
    }
    for (const std::string& name : gone) {
      for (auto& sc : wb_.sheets) {
        bool any = false;
        for (const auto& ch : *sc->charts) any = any || ch.pivot == name;
        if (!any) continue;
        for (auto& ch : sc.mut().charts.mut()) {
          if (ch.pivot == name) ch.pivot.clear();
        }
      }
    }
  }
  // page setup: the print area, print titles and manual page breaks follow whole inserted / deleted rows and columns
  if (!band) {
    if (Sheet* pss = M(sheet)) {
      model::PageSetup ps = *pss->page_setup;
      auto span = [&](uint32_t& lo, uint32_t& hi, bool& has) {
        if (!has) return;
        int64_t a = lo, b = hi;
        if (!MapSpan(sp, lo, hi, &a, &b)) {
          has = false;
          return;
        }
        lo = static_cast<uint32_t>(a);
        hi = static_cast<uint32_t>(b);
      };
      if (rows) {
        span(ps.area.r0, ps.area.r1, ps.has_area);
        span(ps.title_r0, ps.title_r1, ps.has_title_rows);
      } else {
        span(ps.area.c0, ps.area.c1, ps.has_area);
        span(ps.title_c0, ps.title_c1, ps.has_title_cols);
      }
      // the print area's other areas too (Excel's Print_Area name holds them all)
      std::vector<core::Rect> more;
      for (core::Rect m : ps.more_areas) {
        bool has = true;
        if (rows) span(m.r0, m.r1, has);
        else span(m.c0, m.c1, has);
        if (has) more.push_back(m);
      }
      ps.more_areas = std::move(more);
      std::vector<uint32_t>& br = rows ? ps.row_breaks : ps.col_breaks;
      std::vector<uint32_t> nb;
      for (const uint32_t b : br) {
        int64_t m = 0;
        if (MapIdx(sp, b, &m) && m > 0) nb.push_back(static_cast<uint32_t>(m));
      }
      br = std::move(nb);
      if (!(ps == *pss->page_setup)) pss->page_setup.mut() = std::move(ps);
    }
  }

  // 3. write back the formula ids (at the moved positions)
  for (const NewDef& nd : results) {
    if (nd.c >= core::kMaxCols || nd.r >= core::kMaxRows) continue;
    Sheet* s = M(nd.fsheet);
    if (s && s->GetFormula(nd.r, nd.c) != nd.id) s->SetFormula(nd.r, nd.c, nd.id);
  }
  // deleted table columns (ShiftSheet): what pointed at them is #REF!, once the formulas are at their places
  if (!gone_table_cols_.empty()) {
    const auto gone = std::move(gone_table_cols_);
    gone_table_cols_.clear();
    for (const auto& [table, col] : gone) RewriteTableRefs(table, col.empty() ? std::string() : table, col, std::string());
  }

  // 4. reference text of defined names and chart series
  auto adjust_text = [&](std::string& text, uint32_t ctx_sheet, bool name = false) {
    std::string_view t = text;
    const bool eq = !t.empty() && t[0] == '=';
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this, false, name);  // a name's ends as it keeps them
    if (!pr.ok) return;
    if (name && formula::HasRelativeRef(pr.root)) {
      // a name with relative references: its relative offsets don't move, as in Excel, but its absolute parts do
      // ($B2:B$5 is $B2:B$6 after a row inserted at 3, $C2:B$5 after a column at B), and it is #REF! once the cells
      // they read are gone (Excel 16, measured: named_ cases). Each reference is moved as if every part were
      // absolute (relative ones count from A1, where names keep them), then only its absolute parts are taken.
      // (cells shifted inside a band leave such a name as it is: Excel 16, measured)
      if (sp.has_band) return;
      bool moved = false;
      formula::ForEachRef(pr.root, [&](formula::RefSpec& r) {
        if (r.bad) return;  // lesson-ok: ref-has-cells a name's #REF!A1 names a deleted sheet: no row of this one moves it
        const bool rows_abs = !r.whole_cols && (r.r0.abs || (r.is_range && r.r1.abs));
        const bool cols_abs = !r.whole_rows && (r.c0.abs || (r.is_range && r.c1.abs));
        if (!rows_abs && !cols_abs) return;
        formula::Node one;
        one.kind = formula::NK::Ref;
        one.ref = r;
        for (formula::RefCoord* c : {&one.ref.r0, &one.ref.r1, &one.ref.c0, &one.ref.c1}) c->abs = true;
        AdjustAst(one, ctx_sheet, core::CellPos{0, 0}, core::CellPos{0, 0}, sp);
        if (one.ref.bad) {
          r.bad = true;
          moved = true;
          return;
        }
        auto take = [&](formula::RefCoord& o, const formula::RefCoord& n) {
          if (o.abs && o.v != n.v) o.v = n.v, moved = true;
        };
        take(r.r0, one.ref.r0);
        take(r.c0, one.ref.c0);
        if (r.is_range) take(r.r1, one.ref.r1), take(r.c1, one.ref.c1);
      });
      if (moved) text = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this, true);
      return;
    }
    if (AdjustAst(pr.root, ctx_sheet, core::CellPos{0, 0}, core::CellPos{0, 0}, sp)) {
      text = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
    }
  };
  for (auto& n : wb_.names.mut()) adjust_text(n.refers_to, n.sheet_scope >= 0 ? static_cast<uint32_t>(n.sheet_scope) : sheet + 0x8000u, true);
  for (auto& sc : wb_.sheets) {
    const uint32_t sid = sc->id;
    model::EditObjectRefTexts(sc, [&](std::string& t) { adjust_text(t, sid); });
  }
  model::EditWorkbookRefTexts(wb_, [&](std::string& t) { adjust_text(t, 0xFFFFFFFFu); });  // texts that name their sheet
  if (any_moved) {
    RebuildAll(false);  // the dependency index at the new positions; what to calculate is marked below
    ClearPending();
  } else {
    // no formula moved and none changed (columns deleted to the right of everything, rows inserted below it): the
    // index stays as it is, without the deleted formulas, with the table readers registered for their tables now
    const bool hook = spill_hook_;
    spill_hook_ = false;
    for (uint64_t k : dropped) UnregisterFormulaCell(KeySheet(k), KeyRow(k), KeyCol(k));
    spill_hook_ = hook;
    for (uint64_t k : table_cells) {
      const Sheet* ts = S(KeySheet(k));
      if (const uint32_t fid = ts ? ts->GetFormula(KeyRow(k), KeyCol(k)) : 0) RegisterFormulaCell(KeySheet(k), KeyRow(k), KeyCol(k), fid);
    }
    RebuildSpillIndex();
  }
  auto add = [&](uint32_t fs, uint32_t r, uint32_t c) {
    const Sheet* fsh = S(fs);
    if (fsh && fsh->GetFormula(r, c)) AddPending(Key(fs, r, c));
  };
  for (uint64_t k : again) add(KeySheet(k), KeyRow(k), KeyCol(k));
  for (uint64_t k : again_old) {
    uint32_t r = KeyRow(k), c = KeyCol(k);
    if (KeySheet(k) == sheet) {
      const int64_t main = rows ? r : c, other = rows ? c : r;
      if (InBand(sp, other, other)) {
        int64_t m;
        if (!MapIdx(sp, main, &m)) continue;  // deleted with its row / column
        if (rows) r = static_cast<uint32_t>(m);
        else c = static_cast<uint32_t>(m);
      }
    }
    add(KeySheet(k), r, c);
  }
  MarkAllVolatileDirty();
}

void Document::ShiftSheet(uint32_t sheet, bool rows, uint32_t at, int64_t delta, const Rect* band) {
  Sheet* s = M(sheet);
  if (!s) return;
  {
    StructShift ss;
    ss.seq = ++shift_seq;
    ss.sheet = sheet;
    ss.rows = rows;
    ss.at = at;
    ss.delta = delta;
    ss.has_band = band != nullptr;
    if (band) ss.band = *band;
    shift_log.push_back(ss);
    if (shift_log.size() > 256) shift_log.erase(shift_log.begin(), shift_log.begin() + 64);
  }
  Rect region = rows ? Rect{at, 0, core::kMaxRows, core::kMaxCols - 1} : Rect{0, at, core::kMaxRows, core::kMaxCols - 1};
  if (band) {
    if (rows) {
      region.c0 = band->c0;
      region.c1 = band->c1;
    } else {
      region.r0 = band->r0;
      region.r1 = band->r1;
    }
  }
  // collect (two flat lists: a hash map of millions of cells was most of the time)
  struct MovedValue {
    uint32_t r, c;
    Value v;
  };
  struct MovedMeta {
    uint32_t r, c, f, st;
  };
  std::vector<MovedValue> vals;
  std::vector<MovedMeta> metas;
  // only the occupied cells of each tile (its bitmap), and only tiles with formulas or styles: visiting all 4,096
  // cells of every tile was half of the move
  ForEachTileInRect(s->values, region, [&](uint32_t rt, uint32_t ct, const model::ValueTile& t) {
    if (!t.count) return;
    for (uint32_t w = 0; w < model::kTileCells / 64; ++w) {
      for (uint64_t m = t.occ[w]; m; m &= m - 1) {
        const uint32_t idx = w * 64 + static_cast<uint32_t>(std::countr_zero(m));
        const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
        if (region.Contains(r, c) && !t.v[idx].IsEmpty()) vals.push_back(MovedValue{r, c, t.v[idx]});
      }
    }
  });
  ForEachTileInRect(s->meta, region, [&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
    if (!t.formula_count && !t.style_count) return;
    for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
      if (!t.formula[idx] && !t.style[idx]) continue;
      const uint32_t r = rt * model::kTileRows + idx % model::kTileRows, c = ct * model::kTileCols + idx / model::kTileRows;
      if (region.Contains(r, c)) metas.push_back(MovedMeta{r, c, t.formula[idx], t.style[idx]});
    }
  });
  // The cells are written through the tile they are in, remembered from one cell to the next (the cells come tile
  // by tile): looking every one up in the tile directory was most of the move. A tile reached through MutableCell is
  // the sheet's own from then on (the copy on write happened then), so its address stays good.
  model::ValueTile* vt = nullptr;
  model::MetaTile* mt = nullptr;
  uint64_t vkey = ~0ull, mkey = ~0ull;
  auto tkey = [](uint32_t r, uint32_t c) { return (uint64_t{r / model::kTileRows} << 32) | (c / model::kTileCols); };
  auto vtile = [&](uint32_t r, uint32_t c) -> model::ValueTile& {
    if (const uint64_t k = tkey(r, c); k != vkey || !vt) {
      vt = &s->values.MutableCell(r, c);
      vkey = k;
    }
    return *vt;
  };
  auto mtile = [&](uint32_t r, uint32_t c) -> model::MetaTile& {
    if (const uint64_t k = tkey(r, c); k != mkey || !mt) {
      mt = &s->meta.MutableCell(r, c);
      mkey = k;
    }
    return *mt;
  };
  auto put_meta = [](model::MetaTile& t, uint32_t idx, uint32_t f, uint32_t st) {
    uint32_t& fs = t.formula[idx];
    if ((fs != 0) != (f != 0)) t.formula_count += f ? 1 : static_cast<uint32_t>(-1);
    fs = f;
    uint32_t& ss = t.style[idx];
    if ((ss != 0) != (st != 0)) t.style_count += st ? 1 : static_cast<uint32_t>(-1);
    ss = st;
  };
  // One pass: every cell goes to its new place, and its old place is cleared unless another moved cell lands there.
  // Inserting, the cells go from the bottom up (the reverse of the tile order), deleting from the top down, so the
  // cell that would land on a place is still unmoved when that place is looked at.
  ShiftSpec sp{sheet, rows, at, delta};
  uint64_t max_row = 0;
  bool wrote = false;
  uint32_t lo_r = 0, lo_c = 0, hi_r = 0, hi_c = 0;
  auto moved = [&](uint32_t r, uint32_t c, uint32_t* nr, uint32_t* nc) {
    int64_t x;
    if (!MapIdx(sp, rows ? r : c, &x)) return false;
    *nr = rows ? static_cast<uint32_t>(x) : r;
    *nc = rows ? c : static_cast<uint32_t>(x);
    if (*nc >= core::kMaxCols || *nr >= core::kMaxRows) return false;  // pushed past the edge
    max_row = std::max<uint64_t>(max_row, *nr);
    if (!wrote) lo_r = hi_r = *nr, lo_c = hi_c = *nc, wrote = true;
    lo_r = std::min(lo_r, *nr), hi_r = std::max(hi_r, *nr), lo_c = std::min(lo_c, *nc), hi_c = std::max(hi_c, *nc);
    return true;
  };
  // the cell whose move lands on (r, c), if one does (it is unmoved yet)
  auto feeder = [&](uint32_t r, uint32_t c, uint32_t* fr, uint32_t* fc) {
    const int64_t main = (rows ? int64_t{r} : int64_t{c}) - delta;
    if (main < 0) return false;
    if (delta < 0 ? false : main < at) return false;  // inserting: only cells at or after the insertion point move
    *fr = rows ? static_cast<uint32_t>(main) : r;
    *fc = rows ? c : static_cast<uint32_t>(main);
    if (*fc >= core::kMaxCols || *fr >= core::kMaxRows) return false;
    return region.Contains(*fr, *fc);
  };
  auto value_at = [&](uint32_t r, uint32_t c) {
    const model::ValueTile* t = s->values.GetCell(r, c);
    return t && t->Occupied(model::TileIndex(r, c));
  };
  auto meta_at = [&](uint32_t r, uint32_t c) {
    const model::MetaTile* t = s->meta.GetCell(r, c);
    const uint32_t idx = model::TileIndex(r, c);
    return t && (t->formula[idx] || t->style[idx]);
  };
  auto move_value = [&](MovedValue& m) {
    uint32_t nr, nc, fr, fc;
    if (moved(m.r, m.c, &nr, &nc)) vtile(nr, nc).Set(model::TileIndex(nr, nc), std::move(m.v));
    if (!(feeder(m.r, m.c, &fr, &fc) && value_at(fr, fc))) vtile(m.r, m.c).Set(model::TileIndex(m.r, m.c), Value());
  };
  auto move_meta = [&](const MovedMeta& m) {
    uint32_t nr, nc, fr, fc;
    if (moved(m.r, m.c, &nr, &nc)) put_meta(mtile(nr, nc), model::TileIndex(nr, nc), m.f, m.st);
    if (!(feeder(m.r, m.c, &fr, &fc) && meta_at(fr, fc))) put_meta(mtile(m.r, m.c), model::TileIndex(m.r, m.c), 0, 0);
  };
  if (delta > 0) {
    for (auto it = vals.rbegin(); it != vals.rend(); ++it) move_value(*it);
    for (auto it = metas.rbegin(); it != metas.rend(); ++it) move_meta(*it);
  } else {
    for (auto& m : vals) move_value(m);
    for (const auto& m : metas) move_meta(m);
  }
  if (wrote) {  // the used range, once
    s->Touch(lo_r, lo_c);
    s->Touch(hi_r, hi_c);
  }
  if (rows && max_row >= s->row_limit) {
    uint64_t want = (max_row / core::kDefaultRows + 1) * core::kDefaultRows;
    s->row_limit = static_cast<uint32_t>(std::min<uint64_t>(want, core::kMaxRows));
  }

  // merged areas
  {
    auto& mg = s->merges.mut();
    std::vector<Rect> out;
    for (Rect m : mg) {
      const int64_t lo = rows ? m.r0 : m.c0, hi = rows ? m.r1 : m.c1;
      const int64_t olo = rows ? m.c0 : m.r0, ohi = rows ? m.c1 : m.r1;
      if (band && !(olo >= (rows ? band->c0 : band->r0) && ohi <= (rows ? band->c1 : band->r1))) {
        // cells shifted across part of it (B3 deleted up, or A3 inserted right, through B2:C3) leave the rest behind:
        // Excel 16 unmerges it (measured); one beside the cells that move, or before them, stays
        const int64_t b0 = rows ? band->c0 : band->r0, b1 = rows ? band->c1 : band->r1;
        if (!(ohi < b0 || olo > b1) && hi >= int64_t{at}) continue;
        out.push_back(m);
        continue;
      }
      int64_t a, b;
      if (!MapSpan(sp, lo, hi, &a, &b)) continue;
      if (rows) {
        m.r0 = static_cast<uint32_t>(a);
        m.r1 = static_cast<uint32_t>(b);
      } else {
        m.c0 = static_cast<uint32_t>(a);
        m.c1 = static_cast<uint32_t>(b);
      }
      if (!m.IsCell()) out.push_back(m);
    }
    mg = std::move(out);
  }
  // ranges of conditional formats, data validation and AutoFilter (mapped like merged areas; fully deleted ranges are removed)
  auto map_rect = [&](Rect m, Rect* res) {
    const int64_t lo = rows ? m.r0 : m.c0, hi = rows ? m.r1 : m.c1;
    const int64_t olo = rows ? m.c0 : m.r0, ohi = rows ? m.c1 : m.r1;
    if (band && !(olo >= (rows ? band->c0 : band->r0) && ohi <= (rows ? band->c1 : band->r1))) {
      *res = m;
      return true;
    }
    int64_t a, b;
    if (!MapSpan(sp, lo, hi, &a, &b)) return false;
    if (rows) {
      m.r0 = static_cast<uint32_t>(a);
      m.r1 = static_cast<uint32_t>(b);
    } else {
      m.c0 = static_cast<uint32_t>(a);
      m.c1 = static_cast<uint32_t>(b);
    }
    *res = m;
    return true;
  };
  auto map_ranges = [&](std::vector<Rect>& v) {
    std::vector<Rect> out;
    for (const Rect& m : v) {
      Rect r2;
      if (map_rect(m, &r2)) out.push_back(r2);
    }
    v = std::move(out);
  };
  if (!s->cond_formats->empty()) {
    auto& v = s->cond_formats.mut();
    for (auto& r : v) map_ranges(r.ranges);
    v.erase(std::remove_if(v.begin(), v.end(), [](const model::CfRule& r) { return r.ranges.empty(); }), v.end());
  }
  if (!s->validations->empty()) {
    auto& v = s->validations.mut();
    for (auto& r : v) map_ranges(r.ranges);
    v.erase(std::remove_if(v.begin(), v.end(), [](const model::DvRule& r) { return r.ranges.empty(); }), v.end());
  }
  // whatever else keeps cells as rectangles or cells moves with them too (Excel 16, measured: the ranges a protected
  // sheet lets people edit, the cells error checking leaves alone, the cells a linked picture shows; and YCGrid's
  // alert rules and parameter cells), and goes when its cells are deleted. Left behind, a linked picture showed the
  // rows above its cells and an allowed range let people edit the wrong cells after rows went in.
  if (!s->ignored_errors->empty()) {
    std::vector<Rect> v = *s->ignored_errors;
    map_ranges(v);
    if (v != *s->ignored_errors) s->ignored_errors.mut() = std::move(v);
  }
  if (!s->protection->ranges.empty()) {
    std::vector<model::AllowEditRange> keep;
    for (model::AllowEditRange e : s->protection->ranges) {
      if (map_rect(e.rect, &e.rect)) keep.push_back(std::move(e));
    }
    if (!(keep == s->protection->ranges)) s->protection.mut().ranges = std::move(keep);
  }
  for (auto& sc : wb_.sheets) {
    const auto& lps = *sc->linked_pictures;
    if (std::none_of(lps.begin(), lps.end(), [&](const Sheet::LinkedPicture& lp) { return lp.sheet == sheet; })) continue;
    std::vector<Sheet::LinkedPicture> keep;
    for (Sheet::LinkedPicture lp : lps) {
      if (lp.sheet != sheet || map_rect(lp.rect, &lp.rect)) keep.push_back(lp);  // cells deleted: the picture stays as it is
    }
    sc.mut().linked_pictures.mut() = std::move(keep);
  }
  if (std::any_of(wb_.alerts->begin(), wb_.alerts->end(), [&](const model::AlertRule& a) { return a.sheet == sheet; })) {
    std::vector<model::AlertRule> keep;
    for (model::AlertRule a : *wb_.alerts) {
      if (a.sheet != sheet || map_rect(a.range, &a.range)) keep.push_back(std::move(a));
    }
    wb_.alerts.mut() = std::move(keep);
  }
  if (std::any_of(wb_.parameters->begin(), wb_.parameters->end(), [&](const model::ParameterDef& p) { return p.sheet == sheet; })) {
    std::vector<model::ParameterDef> keep;
    for (model::ParameterDef p : *wb_.parameters) {
      Rect cell = Rect::Cell(p.row, p.col);
      if (p.sheet == sheet && !map_rect(cell, &cell)) continue;
      p.row = cell.r0;
      p.col = cell.c0;
      keep.push_back(std::move(p));
    }
    wb_.parameters.mut() = std::move(keep);
  }
  // the rows a filter hid follow their rows (whole rows inserted or deleted): left at their old numbers, a row hidden
  // by hand read as filtered and a filtered one as hidden by hand (SUBTOTAL(9) counted it after a row went in above)
  auto shift_hidden = [&](std::map<uint32_t, int32_t>& hidden) {
    if (hidden.empty() || !rows || band) return;
    std::map<uint32_t, int32_t> o;
    for (const auto& [r, h] : hidden) {
      int64_t x;
      if (MapIdx(sp, r, &x) && x < core::kMaxRows) o[static_cast<uint32_t>(x)] = h;
    }
    hidden = std::move(o);
  };
  if (s->autofilter->on || !s->autofilter->hidden.empty()) {
    Rect r2;
    if (!s->autofilter->on || map_rect(s->autofilter->range, &r2)) {
      model::AutoFilter& af = s->autofilter.mut();
      if (af.on) af.range = r2;
      shift_hidden(af.hidden);
    } else {
      s->autofilter.mut() = model::AutoFilter{};
    }
  }
  // an advanced filter's list (filtered in place) moves with its cells whether or not it hides rows now, and is
  // forgotten when they are deleted
  if (!(s->autofilter->adv_range == Rect{})) {
    Rect r2;
    model::AutoFilter& af = s->autofilter.mut();
    af.adv_range = map_rect(af.adv_range, &r2) ? r2 : Rect{};
  }
  // tables move and grow with the rows / columns (a table whose range was deleted goes away)
  if (!s->tables->empty()) {
    std::vector<model::Table> o;
    for (const auto& t : *s->tables) {
      Rect r2;
      if (!map_rect(t.range, &r2)) {
        gone_table_cols_.emplace_back(t.name, std::string());  // the whole table: what names it is #REF! (Excel 16)
        continue;
      }
      model::Table nt = t;
      nt.range = r2;
      shift_hidden(nt.filter.hidden);
      // its columns deleted (whole columns, or cells shifted left across all its rows): their entries go, so the
      // others keep their names, and what points at them becomes #REF! once the cells have moved (Excel 16, measured:
      // =SUM(Table1[w]) is =SUM(#REF!) however the column went, and a column named w later doesn't take it over)
      if (!rows && delta < 0 && (!band || (t.range.r0 >= band->r0 && t.range.r1 <= band->r1))) {
        std::vector<model::TableColumn> keep;
        for (size_t j = 0; j < t.columns.size(); ++j) {
          const int64_t c = int64_t{t.range.c0} + static_cast<int64_t>(j);
          if (c >= int64_t{at} && c < int64_t{at} - delta) {
            if (!t.columns[j].name.empty()) gone_table_cols_.emplace_back(t.name, t.columns[j].name);
          } else {
            keep.push_back(t.columns[j]);
          }
        }
        nt.columns = std::move(keep);
      }
      nt.columns.resize(r2.Cols());
      o.push_back(std::move(nt));
    }
    s->tables.reset(std::move(o));
  }
  // array formulas keep their fixed range (dynamic spills were dropped before the move)
  if (!s->spills->empty()) {
    std::map<uint64_t, model::SpillRange> o;
    for (const auto& [k, spr] : *s->spills) {
      Rect r2;
      if (map_rect(spr.rect, &r2)) o[(uint64_t{r2.r0} << 16) | r2.c0] = model::SpillRange{r2, spr.cse};
    }
    s->spills.reset(std::move(o));
  }
  if (!band) {
    // entire row / column styles, row heights and column widths
    auto shift_map = [&](std::map<uint32_t, uint32_t>& mp) {
      std::map<uint32_t, uint32_t> o;
      for (auto [k, v] : mp) {
        int64_t x;
        if (MapIdx(sp, k, &x) && x < core::kMaxRows) o[static_cast<uint32_t>(x)] = v;
      }
      mp = std::move(o);
    };
    if (rows) {
      if (!s->row_styles->empty()) shift_map(s->row_styles.mut());
      if (delta > 0) s->rows.mut().Insert(at, delta);
      else s->rows.mut().Remove(at, -delta);
    } else {
      if (!s->col_styles->empty()) shift_map(s->col_styles.mut());
      if (delta > 0) s->cols.mut().Insert(at, delta);
      else s->cols.mut().Remove(at, -delta);
    }
    // notes
    if (!s->notes->empty()) {
      std::map<uint64_t, model::CellNote> o;
      for (auto& [k, v] : *s->notes) {
        uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        int64_t x;
        if (!MapIdx(sp, rows ? r : c, &x)) continue;
        if (rows) r = static_cast<uint32_t>(x);
        else c = static_cast<uint32_t>(x);
        o[(uint64_t{r} << 16) | c] = v;
      }
      s->notes.reset(std::move(o));
    }
    // hyperlinks
    if (!s->links->empty()) {
      std::map<uint64_t, model::Hyperlink> o;
      for (auto& [k, v] : *s->links) {
        uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        int64_t x;
        if (!MapIdx(sp, rows ? r : c, &x)) continue;
        if (rows) r = static_cast<uint32_t>(x);
        else c = static_cast<uint32_t>(x);
        o[(uint64_t{r} << 16) | c] = v;
      }
      s->links.reset(std::move(o));
    }
    // fonts inside cell texts
    if (!s->rich->empty()) {
      std::map<uint64_t, model::RichText> o;
      for (auto& [k, v] : *s->rich) {
        uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        int64_t x;
        if (!MapIdx(sp, rows ? r : c, &x)) continue;
        if (rows) r = static_cast<uint32_t>(x);
        else c = static_cast<uint32_t>(x);
        o[(uint64_t{r} << 16) | c] = v;
      }
      s->rich.reset(std::move(o));
    }
  }
  // rows grow on demand (PLAN 5.1): what the inserted rows push past the sheet's last row grows the sheet, merged
  // areas and tables as the cells do (a table or a merged area on whole columns ran past the sheet: doc_fuzz --wide)
  if (rows && delta > 0) {
    uint32_t last = 0;
    for (const Rect& m : *s->merges) last = std::max(last, m.r1);
    for (const auto& t : *s->tables) last = std::max(last, t.range.r1);
    if (last >= s->row_limit) {
      const uint64_t want = (uint64_t{last} / core::kDefaultRows + 1) * core::kDefaultRows;
      s->row_limit = static_cast<uint32_t>(std::min<uint64_t>(want, core::kMaxRows));
    }
  }
  s->ComputeUsedRange();
}

namespace {

// Excel: inserted rows / columns / cells take the formats of their neighbor (the row above or the column on the left,
// or below / right); a row height or column width set by hand comes along. band: the inserted cells; from: the
// neighbor's first row (rows) or column (!rows); lo..hi: the columns (rows) of the band.
void CopyNeighborFormats(model::Sheet& s, bool rows, uint32_t from, uint32_t first, uint32_t count, uint32_t lo, uint32_t hi) {
  if (!s.has_used && s.row_styles->empty() && s.col_styles->empty()) return;
  const uint32_t end = s.has_used ? (rows ? std::min(hi, s.used.c1) : std::min(hi, s.used.r1)) : lo;
  const bool whole = rows ? (lo == 0 && hi >= core::kMaxCols - 1) : (lo == 0 && hi >= s.row_limit - 1);
  auto& dim_styles = rows ? s.row_styles : s.col_styles;
  uint64_t styled = 0;
  for (uint32_t k = lo; s.has_used && k <= end; ++k) styled += (rows ? s.GetOwnStyle(from, k) : s.GetOwnStyle(k, from)) != 0;
  if (whole && styled * count > 4'000'000) {
    // many whole lines next to one whose cells have formats of their own (a million rows inserted under a row with a
    // border in each of its 16,384 cells: 17 billion cell styles, out of memory, basics seq316723): the new lines take
    // as their own line style what most of the neighbor's cells show, and a style of their own only where a cell
    // shows another; what every cell shows is the same
    const uint32_t n = rows ? core::kMaxCols : s.row_limit;
    std::vector<uint32_t> shows(n);
    std::unordered_map<uint32_t, uint32_t> tally;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t id = rows ? s.GetStyle(from, k) : s.GetStyle(k, from);
      if (id == model::kStyleOwnNormal) id = 0;
      shows[k] = id;
      ++tally[id];
    }
    // the line style that leaves the fewest cells a style of their own: none (a cell shows the crossing line's style),
    // or one the neighbor's cells show, Normal too (its own Normal: under columns with Wrap Text, rows 1:4 kept
    // Normal and the million rows inserted under them took it cell by cell, out of memory again: basics seq327428)
    const auto& crossing = rows ? *s.col_styles : *s.row_styles;
    auto cross = [&](uint32_t k) {
      auto it = crossing.find(k);
      return it != crossing.end() ? it->second : 0u;
    };
    uint64_t best = 0;
    for (uint32_t k = 0; k < n; ++k) best += shows[k] != cross(k);
    uint32_t line = 0;  // 0: none
    for (const auto& [id, c] : tally) {
      const uint64_t cost = n - c;
      const uint32_t as_line = id ? id : model::kStyleOwnNormal;
      if (cost < best || (cost == best && line != 0 && as_line < line)) best = cost, line = as_line;
    }
    if (line) {
      auto& m = dim_styles.mut();
      for (uint32_t i = 0; i < count; ++i) m[first + i] = line;
    }
    for (uint32_t k = 0; k < n; ++k) {
      // a cell without a style of its own shows the new line's style, else the crossing line's
      const uint32_t plain = line == model::kStyleOwnNormal ? 0 : line ? line : cross(k);
      if (shows[k] == plain) continue;
      for (uint32_t i = 0; i < count; ++i) {
        if (rows) s.ShowStyle(first + i, k, shows[k]);
        else s.ShowStyle(k, first + i, shows[k]);
      }
    }
    return;
  }
  for (uint32_t k = lo; s.has_used && k <= end; ++k) {
    const uint32_t id = rows ? s.GetOwnStyle(from, k) : s.GetOwnStyle(k, from);
    if (!id) continue;
    for (uint32_t i = 0; i < count; ++i) {
      if (rows) s.SetStyle(first + i, k, id);
      else s.SetStyle(k, first + i, id);
    }
  }
  // entire-row / entire-column formats (only for whole rows / columns)
  if (whole) {
    auto it = dim_styles->find(from);
    if (it != dim_styles->end()) {
      const uint32_t id = it->second;
      auto& m = dim_styles.mut();
      for (uint32_t i = 0; i < count; ++i) m[first + i] = id;
    }
  }
}

}  // namespace

bool Document::RefuseRowsCols(uint32_t sheet, bool rows, bool insert, uint32_t at, uint32_t count) {
  if (rows && !insert) {
    const Rect band{at, 0, at + std::max<uint32_t>(count, 1) - 1, core::kMaxCols - 1};
    if (Refuse(sheet, S(sheet) && S(sheet)->has_used ? &band : nullptr, Allow::DeleteRows)) return true;
  } else if (Refuse(sheet, nullptr, rows ? Allow::InsertRows : insert ? Allow::InsertColumns : Allow::DeleteColumns)) {
    return true;
  }
  if (!S(sheet)) return true;
  if (count == 0) return false;
  if (RefuseArrayShift(sheet, rows, at, count, insert, nullptr)) return true;  // "You can't change part of an array."
  if (RefusePivotShift(sheet, rows, at, count, insert, nullptr)) return true;
  if (insert) return RefuseOffSheet(sheet, rows, count, nullptr);
  return rows && RefuseTableHeaderDelete(sheet, at, at + count - 1, 0, core::kMaxCols - 1);
}

void Document::InsertRows(uint32_t sheet, uint32_t at, uint32_t count, int origin) {
  if (RefuseRowsCols(sheet, true, true, at, count) || count == 0) return;
  RecordStruct(StructOp{StructOp::InsertRows, S(sheet)->name, at, count});
  NoteStructural();
  AdjustFormulas(sheet, true, at, count, nullptr);
  // the new rows take the formats (and a height set by hand) of the row above, or below
  const int64_t from = origin == 0 ? int64_t{at} - 1 : origin == 1 ? int64_t{at} + count : -1;
  if (Sheet* s = M(sheet); s && from >= 0 && from < int64_t{s->row_limit}) {
    CopyNeighborFormats(*s, true, static_cast<uint32_t>(from), at, count, 0, core::kMaxCols - 1);
    if (s->rows->IsCustom(from) && !s->rows->IsHidden(from)) SetRowHeight(sheet, at, at + count - 1, s->rows->Size(static_cast<uint32_t>(from)));
  }
  FillCalculatedColumns(sheet, at, count);
  changes_.push_back(CellChange{sheet, Rect{at, 0, core::kMaxRows, core::kMaxCols - 1}, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  wb_.dirty = true;
}

// rows inserted inside a table's data take its calculated columns: a column whose rows above and below the new ones
// hold one formula (the same in R1C1) gets it in the new rows too (Excel 16, measured 2026-11-20: rows 3:4 inserted
// into A1:E6 with =B2*C2 down D get =B3*C3 and =B4*C4); they were left empty
void Document::FillCalculatedColumns(uint32_t sheet, uint32_t at, uint32_t count) {
  const Sheet* s = S(sheet);
  if (!s || s->tables->empty() || count == 0) return;
  std::vector<std::pair<uint32_t, uint32_t>> cols;  // (column, the row above) to fill
  for (const auto& tb : *s->tables) {
    const Rect body = tb.Data();
    if (at == 0 || at - 1 < body.r0 || uint64_t{at} + count > body.r1) continue;  // not between two of its data rows
    const uint32_t above = at - 1, below = at + count;
    for (uint32_t c = body.c0; c <= body.c1; ++c) {
      const std::string fa = GetFormulaA1(sheet, above, c), fb = GetFormulaA1(sheet, below, c);
      if (fa.empty() || fb.empty()) continue;
      formula::ParseResult pr = formula::Parse(std::string_view(fa).substr(1), core::CellPos{above, c}, this);
      if (pr.ok && "=" + formula::ToA1(pr.root, core::CellPos{below, c}, this) == fb) cols.emplace_back(c, above);
    }
  }
  for (const auto& [c, above] : cols) {
    const ClipData one = Copy(sheet, Rect::Cell(above, c));
    PasteRepeated(sheet, at, c, count, 1, one, PasteMode::Formulas);
  }
}

void Document::DeleteRows(uint32_t sheet, uint32_t at, uint32_t count) {
  if (RefuseRowsCols(sheet, true, false, at, count) || count == 0) return;
  RecordStruct(StructOp{StructOp::DeleteRows, S(sheet)->name, at, count});
  // every data row of a table: its first row stays, emptied, as in Excel
  if (KeepTableRow(sheet, at, at + count - 1, 0, core::kMaxCols - 1, false)) {
    ++at;
    if (--count == 0) {
      wb_.dirty = true;
      return;
    }
  }
  NoteStructural();
  AdjustFormulas(sheet, true, at, -static_cast<int64_t>(count), nullptr);
  changes_.push_back(CellChange{sheet, Rect{at, 0, core::kMaxRows, core::kMaxCols - 1}, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  wb_.dirty = true;
}

void Document::InsertCols(uint32_t sheet, uint32_t at, uint32_t count, int origin) {
  if (RefuseRowsCols(sheet, false, true, at, count) || count == 0) return;
  RecordStruct(StructOp{StructOp::InsertCols, S(sheet)->name, at, count});
  NoteStructural();
  AdjustFormulas(sheet, false, at, count, nullptr);
  const int64_t from = origin == 0 ? int64_t{at} - 1 : origin == 1 ? int64_t{at} + count : -1;
  if (Sheet* s = M(sheet); s && from >= 0 && from < int64_t{core::kMaxCols}) {
    CopyNeighborFormats(*s, false, static_cast<uint32_t>(from), at, count, 0, s->row_limit - 1);
    if (s->cols->IsCustom(from) && !s->cols->IsHidden(from)) SetColWidth(sheet, at, at + count - 1, s->cols->Size(static_cast<uint32_t>(from)));
  }
  changes_.push_back(CellChange{sheet, Rect{0, at, core::kMaxRows, core::kMaxCols - 1}, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  wb_.dirty = true;
}

void Document::DeleteCols(uint32_t sheet, uint32_t at, uint32_t count) {
  if (RefuseRowsCols(sheet, false, false, at, count) || count == 0) return;
  RecordStruct(StructOp{StructOp::DeleteCols, S(sheet)->name, at, count});
  NoteStructural();
  AdjustFormulas(sheet, false, at, -static_cast<int64_t>(count), nullptr);
  changes_.push_back(CellChange{sheet, Rect{0, at, core::kMaxRows, core::kMaxCols - 1}, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  wb_.dirty = true;
}

bool Document::RefuseInsertCells(uint32_t sheet, const Rect& rect, bool shift_down, bool sheet_filter_goes) {
  if (Refuse(sheet, nullptr, Allow::Never)) return true;
  if (!S(sheet)) return true;
  if (RefuseArrayShift(sheet, shift_down, shift_down ? rect.r0 : rect.c0, static_cast<uint32_t>(shift_down ? rect.Rows() : rect.Cols()), true, &rect)) return true;
  if (RefusePivotShift(sheet, shift_down, shift_down ? rect.r0 : rect.c0, static_cast<uint32_t>(shift_down ? rect.Rows() : rect.Cols()), true, &rect)) return true;
  if (RefuseFilterShift(sheet, rect, shift_down, sheet_filter_goes)) return true;
  if (RefuseTablePartShift(sheet, rect, shift_down, true)) return true;
  if (RefuseOffSheet(sheet, shift_down, static_cast<uint32_t>(shift_down ? rect.Rows() : rect.Cols()), &rect)) return true;
  // the new cells take the formats of the line above (on the left) cell by cell when they don't span whole lines:
  // B2 to the sheet's last cell under a row with a style in every cell is 17 billion styles (out of memory). Whole
  // lines take a line style instead (CopyNeighborFormats); past 20,000,000 cells a part of the lines is refused
  const Sheet* s = S(sheet);
  const bool whole = shift_down ? (rect.c0 == 0 && rect.c1 >= core::kMaxCols - 1) : (rect.r0 == 0 && rect.r1 >= s->row_limit - 1);
  const uint32_t from = shift_down ? rect.r0 : rect.c0;
  if (!whole && from > 0 && s->has_used) {
    const uint32_t lo = shift_down ? rect.c0 : rect.r0;
    const uint32_t hi = std::min(shift_down ? rect.c1 : rect.r1, shift_down ? s->used.c1 : s->used.r1);
    uint64_t styled = 0;
    for (uint32_t k = lo; k <= hi; ++k) styled += (shift_down ? s->GetOwnStyle(from - 1, k) : s->GetOwnStyle(k, from - 1)) != 0;
    if (styled * static_cast<uint64_t>(shift_down ? rect.Rows() : rect.Cols()) > 20'000'000) {
      CountBlock(BlockReason::TooLarge);
      block_sheet_ = sheet;
      block_cells_ = rect;
      block_range_ = -1;
      block_python_ = python_call;
      block_too_large_ = true;
      return true;
    }
  }
  return false;
}

bool Document::InsertCells(uint32_t sheet, const Rect& rect, bool shift_down, int origin) {
  // cells as wide as the sheet shifted right are whole rows, as tall as it shifted down whole columns: Excel 16 inserts
  // those (measured, edge_insert_cells_ cases); shifted along, every cell of them went off the sheet's edge
  if (const Sheet* s0 = S(sheet); s0 && (shift_down ? rect.r0 == 0 && rect.r1 >= s0->row_limit - 1 : rect.c0 == 0 && rect.c1 >= core::kMaxCols - 1)) {
    const uint32_t blocks = protect_blocks_;
    if (shift_down) InsertCols(sheet, rect.c0, static_cast<uint32_t>(std::min<uint64_t>(rect.Cols(), core::kMaxCols - rect.c0)), origin);
    else InsertRows(sheet, rect.r0, static_cast<uint32_t>(std::min<uint64_t>(rect.Rows(), uint64_t{s0->row_limit} - rect.r0)), origin);
    return protect_blocks_ == blocks;
  }
  if (RefuseInsertCells(sheet, rect, shift_down)) return false;
  RecordStruct(StructOp{StructOp::InsertCells, S(sheet)->name, 0, 0, rect, shift_down});
  NoteStructural();
  if (shift_down) AdjustFormulas(sheet, true, rect.r0, rect.Rows(), &rect);
  else AdjustFormulas(sheet, false, rect.c0, rect.Cols(), &rect);
  // the inserted cells take the formats of the cells above (shifted down) or on the left (shifted right)
  if (Sheet* s = M(sheet); s && origin != 2) {
    const uint32_t n = static_cast<uint32_t>(shift_down ? rect.Rows() : rect.Cols());
    const int64_t from = shift_down ? (origin == 0 ? int64_t{rect.r0} - 1 : int64_t{rect.r1} + 1) : (origin == 0 ? int64_t{rect.c0} - 1 : int64_t{rect.c1} + 1);
    if (from >= 0) CopyNeighborFormats(*s, shift_down, static_cast<uint32_t>(from), shift_down ? rect.r0 : rect.c0, n, shift_down ? rect.c0 : rect.r0,
                                       shift_down ? rect.c1 : rect.r1);
  }
  changes_.push_back(CellChange{sheet, Rect{rect.r0, rect.c0, core::kMaxRows, core::kMaxCols - 1}, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  wb_.dirty = true;
  return true;
}

bool Document::RefuseDeleteCells(uint32_t sheet, const Rect& rect_in, bool shift_up, Rect* grown) {
  Rect rect = rect_in;
  if (Refuse(sheet, nullptr, Allow::Never)) return true;
  if (!S(sheet)) return true;
  if (RefuseArrayShift(sheet, shift_up, shift_up ? rect.r0 : rect.c0, static_cast<uint32_t>(shift_up ? rect.Rows() : rect.Cols()), false, &rect)) return true;
  if (RefusePivotShift(sheet, shift_up, shift_up ? rect.r0 : rect.c0, static_cast<uint32_t>(shift_up ? rect.Rows() : rect.Cols()), false, &rect)) return true;
  if (shift_up && RefuseTableHeaderDelete(sheet, rect.r0, rect.r1, rect.c0, rect.c1)) return true;
  // cells deleted inside a table across part of its width (height) delete its whole rows (columns), as Excel 16 does
  // (measured: C9 of C7:D10 deleted up takes D9 too, D9 deleted left empties column D): shifted alone they put a
  // formula into its header row (doc_fuzz seed 10260083)
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& t : *S(sheet)->tables) {
      const Rect& g = t.range;
      if (!g.Intersects(rect)) continue;
      if (shift_up && (rect.c0 > g.c0 || rect.c1 < g.c1)) {
        rect.c0 = std::min(rect.c0, g.c0), rect.c1 = std::max(rect.c1, g.c1);
        grew = true;
      } else if (!shift_up && (rect.r0 > g.r0 || rect.r1 < g.r1)) {
        rect.r0 = std::min(rect.r0, g.r0), rect.r1 = std::max(rect.r1, g.r1);
        grew = true;
      }
    }
  }
  if (!(rect == rect_in)) {
    // the grown cells answer the same questions
    if (RefuseArrayShift(sheet, shift_up, shift_up ? rect.r0 : rect.c0, static_cast<uint32_t>(shift_up ? rect.Rows() : rect.Cols()), false, &rect)) return true;
    if (RefusePivotShift(sheet, shift_up, shift_up ? rect.r0 : rect.c0, static_cast<uint32_t>(shift_up ? rect.Rows() : rect.Cols()), false, &rect)) return true;
    if (shift_up && RefuseTableHeaderDelete(sheet, rect.r0, rect.r1, rect.c0, rect.c1)) return true;
  }
  if (RefuseTablePartShift(sheet, rect, shift_up, false)) return true;
  if (grown) *grown = rect;
  return false;
}

void Document::DeleteCells(uint32_t sheet, const Rect& rect_in, bool shift_up) {
  // cells as wide as the sheet shifted left are whole rows, as tall as it shifted up whole columns (Excel 16, measured:
  // edge_delete_cells_ cases)
  if (const Sheet* s0 = S(sheet); s0 && (shift_up ? rect_in.r0 == 0 && rect_in.r1 >= s0->row_limit - 1 : rect_in.c0 == 0 && rect_in.c1 >= core::kMaxCols - 1)) {
    if (shift_up) DeleteCols(sheet, rect_in.c0, static_cast<uint32_t>(std::min<uint64_t>(rect_in.Cols(), core::kMaxCols - rect_in.c0)));
    else DeleteRows(sheet, rect_in.r0, static_cast<uint32_t>(std::min<uint64_t>(rect_in.Rows(), uint64_t{s0->row_limit} - rect_in.r0)));
    return;
  }
  Rect rect = rect_in;
  if (RefuseDeleteCells(sheet, rect_in, shift_up, &rect)) return;
  RecordStruct(StructOp{StructOp::DeleteCells, S(sheet)->name, 0, 0, rect, shift_up});
  // every data row of a table, across its columns: its first row stays, emptied, as in Excel
  if (shift_up && KeepTableRow(sheet, rect.r0, rect.r1, rect.c0, rect.c1, true)) {
    if (++rect.r0 > rect.r1) {
      wb_.dirty = true;
      return;
    }
  }
  NoteStructural();
  if (shift_up) AdjustFormulas(sheet, true, rect.r0, -static_cast<int64_t>(rect.Rows()), &rect);
  else AdjustFormulas(sheet, false, rect.c0, -static_cast<int64_t>(rect.Cols()), &rect);
  changes_.push_back(CellChange{sheet, Rect{rect.r0, rect.c0, core::kMaxRows, core::kMaxCols - 1}, static_cast<uint8_t>(kSrcUser | extra_src_bits)});
  wb_.dirty = true;
}

// ---- Sheets ----

namespace {
bool ValidSheetName(std::string_view n, std::string* error, bool reserved_ok = false) {
  if (n.empty()) {
    if (error) *error = TR("The sheet name can't be blank.");
    return false;
  }
  if (core::Utf8Length(n) > 31) {
    if (error) *error = TR("Sheet names can't be longer than 31 characters.");
    return false;
  }
  if (n.find_first_of("[]:*?/\\") != std::string_view::npos || n.front() == '\'' || n.back() == '\'') {
    if (error) *error = TR("Sheet names can't contain : \\ / ? * [ ] and can't begin or end with an apostrophe.");
    return false;
  }
  // Excel keeps it for the change history sheet: its UI refuses it, its object model takes it (Excel 16, measured
  // 2026-11-28: Worksheet.Name = "History" names the sheet)
  if (!reserved_ok && n.size() == 7 && Lower(n) == "history") {
    if (error) *error = TR("History is a reserved name. Type a different name.");
    return false;
  }
  return true;
}
}  // namespace

uint32_t Document::AddSheet(std::string name, int32_t index) {
  if (RefuseStructure()) return 0;
  // a new sheet is named in the UI's language, as a new workbook's first sheet is (Excel names it with
  // the translated word too); the Chinese UI named the first sheet in Chinese and every later one Sheet<n>
  if (name.empty()) name = wb_.NextSheetName(TR("Sheet"));
  if (wb_.FindSheet(name) >= 0 || !ValidSheetName(name, nullptr)) name = wb_.NextSheetName(TR("Sheet"));
  Sheet s;
  s.id = wb_.next_sheet_id++;
  s.name = std::move(name);
  // CodeName: Sheet<n> (unique)
  for (uint32_t n = s.id;; ++n) {
    std::string cn = "Sheet" + std::to_string(n);
    bool dup = false;
    for (const auto& x : wb_.sheets) dup = dup || x->code_name == cn;
    if (!dup) {
      s.code_name = cn;
      break;
    }
  }
  const uint32_t id = s.id;
  auto ptr = std::make_shared<Sheet>(std::move(s));
  if (index < 0 || index > static_cast<int32_t>(wb_.sheets.size())) index = static_cast<int32_t>(wb_.sheets.size());
  wb_.sheets.insert(wb_.sheets.begin() + index, core::Cow<Sheet>(std::move(ptr)));
  {
    StructOp op{StructOp::AddSheet, wb_.sheets[static_cast<size_t>(index)]->name};
    op.index = index;
    RecordStruct(std::move(op));
  }
  Reindex();
  NoteStructural();
  MarkAllVolatileDirty();  // SHEETS(), SHEET() and 3-D references over the new sheet
  wb_.dirty = true;
  added_sheets_.push_back(id);  // Workbook_NewSheet
  return id;
}

namespace {

// a 3-D reference with the deleted sheet at one end ends at the next sheet inside instead, as Excel's help says
// (Sheet1:Sheet3 without Sheet3 is Sheet1:Sheet2; one sheet left, a reference to it); true when it changed
bool ShrinkThreeD(formula::RefSpec& r, uint32_t id, const model::Workbook& wb) {
  const int32_t gone = static_cast<int32_t>(id);
  if (r.bad || r.sheet2 < 0 || (r.sheet != gone && r.sheet2 != gone)) return false;
  int32_t a = wb.IndexOfId(static_cast<uint32_t>(r.sheet)), b = wb.IndexOfId(static_cast<uint32_t>(r.sheet2));
  if (a < 0 || b < 0 || a == b) return false;  // the deleted sheet alone: #REF!A1 with it
  const int32_t step = a < b ? 1 : -1;
  if (r.sheet == gone) a += step;
  else b -= step;
  r.sheet = static_cast<int32_t>(wb.sheets[static_cast<size_t>(a)]->id);
  r.sheet2 = a == b ? -1 : static_cast<int32_t>(wb.sheets[static_cast<size_t>(b)]->id);
  return true;
}

}  // namespace

bool Document::DeleteSheet(uint32_t id) {
  if (RefuseStructure()) return false;
  const int32_t i = wb_.IndexOfId(id);
  if (i < 0) return false;
  int visible = 0;
  for (const auto& s : wb_.sheets) visible += s->visible ? 1 : 0;
  if (visible <= 1 && wb_.sheets[static_cast<size_t>(i)]->visible) return false;
  {
    StructOp op{StructOp::DeleteSheet, wb_.sheets[static_cast<size_t>(i)]->name};
    op.index = i;
    RecordStruct(std::move(op));
  }
  NoteStructural();
  // its sheet-level names go with it, and what refers to it by name (names, series, rules) becomes #REF!, as in Excel
  {
    auto& names = wb_.names.mut();
    names.erase(std::remove_if(names.begin(), names.end(), [&](const model::NameDef& n) { return n.sheet_scope == static_cast<int32_t>(id); }), names.end());
  }
  BreakReferenceTexts(id);
  RewriteThreeDCells(id, [&](formula::RefSpec& ref) { return ShrinkThreeD(ref, id, wb_); }, id);
  // what has to be calculated again: the cells already waiting, and the formulas that read the deleted sheet (they
  // become #REF!; what depends on them follows). 3-D references, names, INDIRECT and SHEET() are volatile and are
  // marked below. Everything else keeps its value: a workbook with millions of formulas recalculated all of them for
  // every deleted sheet (seconds).
  std::vector<uint64_t> again;
  deps_.QueryRect(id, Rect{0, 0, 0xFFFFFFFFu, core::kMaxCols - 1}, [&](uint64_t k) {
    if (KeySheet(k) != id) again.push_back(k);
  });
  {  // its own formulas leave the dependency index (the rest of the index stays: no rebuild of the workbook)
    std::vector<std::pair<uint32_t, uint32_t>> own;
    wb_.sheets[static_cast<size_t>(i)]->meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (t.formula[idx]) own.emplace_back(rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows);
      }
    });
    const bool hook = spill_hook_;
    spill_hook_ = false;  // the sheet goes with its spilled values
    for (auto [r, c] : own) UnregisterFormulaCell(id, r, c);
    spill_hook_ = hook;
  }
  wb_.sheets.erase(wb_.sheets.begin() + i);
  if (wb_.active_sheet >= static_cast<int32_t>(wb_.sheets.size())) wb_.active_sheet = static_cast<int32_t>(wb_.sheets.size()) - 1;
  Reindex();
  {
    // what a cell's formula says of the deleted sheet is what its text reads back as: Data!B2 is #REF!B2 and
    // Data!#REF! is #REF!, the same formula as one typed so. Kept by the sheet's id, two formulas of one text were two
    // formulas until a save and open made them one (doc_fuzz seed 10420333). Each is read back at a cell that uses it,
    // as .ycg stores it.
    // (a name read through the sheet, Data!Rate or Data!A of a Replace, is keyed S<id>N'...': its text reads #REF! too,
    // and kept as it was it showed its old value until a save and an open: doc_fuzz --xlsx seed 34112022)
    const std::string sid = std::to_string(id);
    const std::string named = "S" + sid + "!", broken = "@S" + sid, by_name = "S" + sid + "N'", by_param = "S" + sid + "O'";
    std::unordered_set<uint32_t> candidates;
    for (const auto& [key, fid] : formula_ids_) {
      if (key.find(named) != std::string::npos || key.find(broken) != std::string::npos || key.find(by_name) != std::string::npos ||
          key.find(by_param) != std::string::npos)
        candidates.insert(fid);
    }
    if (!candidates.empty()) {
      struct Use {
        uint32_t sheet, r, c, fid;
      };
      std::vector<Use> uses;
      for (const auto& sc : wb_.sheets) {
        sc->meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
          if (t.formula_count == 0) return;
          for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
            if (t.formula[idx] && candidates.count(t.formula[idx]))
              uses.push_back(Use{sc->id, rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows, t.formula[idx]});
          }
        });
      }
      std::unordered_map<uint32_t, uint32_t> changed;  // formula id -> the one its text reads back as
      for (const Use& u : uses) {
        if (changed.count(u.fid)) continue;
        const formula::CompiledFormula* f = Formula(u.fid);
        if (!f) continue;
        const core::CellPos at{u.r, u.c};
        formula::ParseResult pr = formula::Parse(formula::ToA1(f->root, at, this), at, this);
        changed[u.fid] = pr.ok ? InternFormula(std::move(pr.root)) : u.fid;
      }
      for (const Use& u : uses) {
        const uint32_t nid = changed[u.fid];
        if (nid == u.fid) continue;
        UnregisterFormulaCell(u.sheet, u.r, u.c);
        M(u.sheet)->SetFormula(u.r, u.c, nid);
        RegisterFormulaCell(u.sheet, u.r, u.c, nid);
        again.push_back(Key(u.sheet, u.r, u.c));  // calculated as what it reads now (a name's reader read no cell of the sheet)
      }
    }
  }
  for (uint64_t k : again) {
    const Sheet* s = S(KeySheet(k));
    if (s && s->GetFormula(KeyRow(k), KeyCol(k))) AddPending(k);
  }
  MarkAllVolatileDirty();
  wb_.dirty = true;
  return true;
}

bool Document::RenameSheet(uint32_t id, std::string_view name, std::string* error, bool reserved_ok) {
  if (RefuseStructure()) {
    if (error) *error = "The workbook is protected";
    return false;
  }
  if (!ValidSheetName(name, error, reserved_ok)) return false;
  const int32_t other = wb_.FindSheet(name);
  if (other >= 0 && wb_.sheets[static_cast<size_t>(other)]->id != id) {
    if (error) *error = TR("That name is already taken. Try a different one.");  // Excel 16's message, measured
    return false;
  }
  Sheet* s = M(id);
  if (!s) return false;
  const std::string old_name = s->name, new_name(name);
  if (old_name != new_name) {
    StructOp op{StructOp::RenameSheet, old_name};
    op.name = new_name;
    RecordStruct(std::move(op));
  }
  NoteStructural();
  if (old_name != new_name) RenameInReferenceTexts(id, old_name, new_name);
  s = M(id);
  s->name = new_name;
  // references are stored by id, so text automatically shows the new name; name-based formulas like INDIRECT need recalculation
  MarkAllVolatileDirty();
  wb_.dirty = true;
  return true;
}

// defined names, chart series and sources, conditional formats and validation keep their references as text that
// names the sheet: each text naming the old name is read with it and written back with the new one (cell formulas
// keep the sheet's id and need nothing; hyperlinks keep the old text, as in Excel)
void Document::RenameInReferenceTexts(uint32_t id, const std::string& old_name, const std::string& new_name) {
  const std::string low_old = Lower(old_name);
  ForEachReferenceText([&](std::string& text) {
    if (text.empty() || Lower(text).find(low_old) == std::string::npos) return;
    std::string_view t = text;
    const bool eq = t[0] == '=';
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);  // the old name still finds the sheet
    if (!pr.ok) return;
    Sheet* sh = M(id);
    sh->name = new_name;
    std::string out = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
    sh->name = old_name;
    text = std::move(out);
  });
}

// a sheet about to be deleted (it still exists): the texts that refer to it get #REF! there, as in Excel. Left as they
// were, a sheet added later under the same name took the deleted sheet's place in every name, series and rule.
void Document::BreakReferenceTexts(uint32_t id) {
  const Sheet* gone = S(id);
  if (!gone) return;
  const std::string low = Lower(gone->name);
  ForEachReferenceText([&](std::string& text) {
    if (text.empty() || Lower(text).find(low) == std::string::npos) return;
    std::string_view t = text;
    const bool eq = t[0] == '=';
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);
    if (!pr.ok) return;
    bool hit = false;
    formula::ForEachRef(pr.root, [&](formula::RefSpec& r) {
      if (ShrinkThreeD(r, id, wb_)) {
        hit = true;
      } else if (r.sheet == static_cast<int32_t>(id) && r.sheet2 < 0) {
        r.bad = true;
        r.lost_sheet = true;  // =#REF!$A$1, as Excel's Name Manager shows it
        hit = true;
      }
    });
    if (hit) text = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
  });
}

void Document::RewriteThreeDCells(uint32_t id, const std::function<bool(formula::RefSpec&)>& fix, uint32_t skip) {
  // found by their keys ("S<id>:S" / ":S<id>!"), so a workbook without any costs one look at each distinct formula,
  // not at every cell
  const std::string at_start = "S" + std::to_string(id) + ":S", at_end = ":S" + std::to_string(id) + "!";
  std::vector<uint32_t> candidates;
  for (const auto& [key, fid] : formula_ids_) {
    if (key.find(at_start) != std::string::npos || key.find(at_end) != std::string::npos) candidates.push_back(fid);
  }
  std::unordered_map<uint32_t, uint32_t> changed;  // formula id -> the one with its 3-D references rewritten
  for (uint32_t fid : candidates) {
    const formula::CompiledFormula* f = Formula(fid);
    if (!f) continue;
    formula::Node root = f->root;
    bool hit = false;
    formula::ForEachRef(root, [&](formula::RefSpec& ref) { hit = fix(ref) || hit; });
    if (hit) changed[fid] = InternFormula(std::move(root));
  }
  if (changed.empty()) return;
  for (const auto& sc : wb_.sheets) {
    if (sc->id == skip) continue;
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> cells;
    sc->meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        const auto it = t.formula[idx] ? changed.find(t.formula[idx]) : changed.end();
        if (it != changed.end()) cells.emplace_back(rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows, it->second);
      }
    });
    const uint32_t sid = sc->id;
    for (const auto& [r, c, nid] : cells) {
      UnregisterFormulaCell(sid, r, c);
      M(sid)->SetFormula(r, c, nid);
      RegisterFormulaCell(sid, r, c, nid);
      AddPending(Key(sid, r, c));
    }
  }
}

void Document::ForEachReferenceText(const std::function<void(std::string&)>& rewrite) {
  for (auto& n : wb_.names.mut()) rewrite(n.refers_to);
  model::EditWorkbookRefTexts(wb_, rewrite);
  for (auto& sc : wb_.sheets) {
    model::EditObjectRefTexts(sc, rewrite);
    model::EditRuleRefTexts(sc, [&](std::string& t, core::CellPos, bool constants_ok) {
      if (!(constants_ok && t[0] != '=')) rewrite(t);  // a list of values (Buy,Sell) is no reference
    });
  }
}

bool Document::MoveSheet(uint32_t id, int32_t new_index) {
  if (RefuseStructure()) return false;
  const int32_t i = wb_.IndexOfId(id);
  if (i < 0) return false;
  new_index = std::clamp<int32_t>(new_index, 0, static_cast<int32_t>(wb_.sheets.size()) - 1);
  if (i == new_index) return true;
  {
    StructOp op{StructOp::MoveSheet, wb_.sheets[static_cast<size_t>(i)]->name, static_cast<uint32_t>(i)};
    op.index = new_index;
    RecordStruct(std::move(op));
  }
  NoteStructural();
  {
    // a 3-D reference's end sheet moved past its other end leaves the span, which then ends at the next sheet inside
    // (Excel: with Data, Other, S3, S4, Other:S4 is Other:S3 once S4 moves before Data); an end moved outward widens it
    std::vector<uint32_t> after;
    for (const auto& sc : wb_.sheets) after.push_back(sc->id);
    after.erase(after.begin() + i);
    after.insert(after.begin() + new_index, id);
    auto pos_after = [&](int32_t sid) {
      return static_cast<int32_t>(std::find(after.begin(), after.end(), static_cast<uint32_t>(sid)) - after.begin());
    };
    auto leaves = [&](formula::RefSpec& r) {
      if (r.bad || r.sheet2 < 0 || (r.sheet != static_cast<int32_t>(id) && r.sheet2 != static_cast<int32_t>(id))) return false;
      const int32_t other = r.sheet == static_cast<int32_t>(id) ? r.sheet2 : r.sheet;
      const int32_t oi = wb_.IndexOfId(static_cast<uint32_t>(other));
      if (oi < 0) return false;
      const bool low = i < oi;  // the moved sheet was the span's left end
      const bool out = low ? pos_after(static_cast<int32_t>(id)) > pos_after(other) : pos_after(static_cast<int32_t>(id)) < pos_after(other);
      return out && ShrinkThreeD(r, id, wb_);
    };
    RewriteThreeDCells(id, leaves, UINT32_MAX);
    const std::string low_name = Lower(wb_.sheets[static_cast<size_t>(i)]->name);
    ForEachReferenceText([&](std::string& text) {
      if (text.empty() || Lower(text).find(low_name) == std::string::npos) return;
      std::string_view t = text;
      const bool eq = t[0] == '=';
      if (eq) t.remove_prefix(1);
      formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);
      if (!pr.ok) return;
      bool hit = false;
      formula::ForEachRef(pr.root, [&](formula::RefSpec& r) { hit = leaves(r) || hit; });
      if (hit) text = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
    });
  }
  auto item = wb_.sheets[static_cast<size_t>(i)];
  wb_.sheets.erase(wb_.sheets.begin() + i);
  wb_.sheets.insert(wb_.sheets.begin() + new_index, item);
  Reindex();
  MarkAllVolatileDirty();  // SHEET() and the like
  wb_.dirty = true;
  return true;
}

uint32_t Document::CopySheet(uint32_t id, int32_t new_index) {
  if (RefuseStructure()) return 0;
  const Sheet* src = S(id);
  if (!src) return 0;
  NoteStructural();
  Sheet copy = *src;  // COW: tiles are shared and copied on the next write
  copy.id = wb_.next_sheet_id++;
  std::string base = src->name;
  std::string name;
  {
    // Excel's name for the copy: "Q" -> "Q (2)", "Q (2)" -> "Q (3)", "R(2)" -> "R(3)", cut to 31 characters with the
    // number kept ("(n)" replaces the name's end)
    std::string stem = base, sep = " ";
    int n = 2;
    if (!base.empty() && base.back() == ')') {
      const size_t open = base.rfind('(');
      if (open != std::string::npos && open + 2 < base.size()) {
        const std::string digits = base.substr(open + 1, base.size() - open - 2);
        if (digits.size() <= 6 && std::all_of(digits.begin(), digits.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
          stem = base.substr(0, open);
          sep.clear();
          if (!stem.empty() && stem.back() == ' ') stem.pop_back(), sep = " ";
          n = std::stoi(digits) + 1;
        }
      }
    }
    for (;; ++n) {
      const std::string tail = sep + "(" + std::to_string(n) + ")";
      const size_t room = 31 - std::min<size_t>(31, tail.size());
      name = (core::Utf8Length(stem) > room ? std::string(core::Utf8First(stem, room)) : stem) + tail;
      if (wb_.FindSheet(name) < 0) break;
    }
  }
  copy.name = name;
  copy.code_name = "Sheet" + std::to_string(copy.id);
  // controls and charts need new ids
  for (auto& c : copy.controls.mut()) c.id = wb_.next_object_id++;
  for (auto& c : copy.charts.mut()) c.id = wb_.next_object_id++;
  for (auto& c : copy.shapes.mut()) c.id = wb_.next_object_id++;
  for (auto& g : copy.sparklines.mut()) {
    g.id = wb_.next_object_id++;
    for (auto& it : g.items) {
      if (it.src_sheet == id) it.src_sheet = copy.id;  // data on the copied sheet: the copy's own data
    }
  }
  for (auto& sl : copy.slicers.mut()) sl.id = wb_.next_object_id++;
  for (auto& p : copy.pivots.mut()) {
    p.id = wb_.next_object_id++;
    p.name = NextPivotName();  // PivotTable names are unique in the workbook
    for (int k = 2; std::any_of(copy.pivots->begin(), copy.pivots->end(), [&](const model::PivotTable& o) { return &o != &p && o.name == p.name; }); ++k) {
      p.name = TR("PivotTable") + std::to_string(k);
    }
  }
  std::unordered_map<std::string, std::string> table_names;  // the source's table (lowered) -> the copy's
  for (auto& t : copy.tables.mut()) {
    const std::string was = Lower(t.name);
    t.id = wb_.next_object_id++;
    t.name = NextTableName();  // table names are unique in the workbook
    for (int k = 2; std::any_of(copy.tables->begin(), copy.tables->end(), [&](const model::Table& o) { return &o != &t && o.name == t.name; }); ++k) {
      t.name = TR("Table") + std::to_string(k);
    }
    table_names[was] = t.name;
  }
  const uint32_t nid = copy.id;
  if (new_index < 0 || new_index > static_cast<int32_t>(wb_.sheets.size())) new_index = wb_.IndexOfId(id) + 1;
  wb_.sheets.insert(wb_.sheets.begin() + new_index, core::Cow<Sheet>(std::make_shared<Sheet>(std::move(copy))));
  {
    StructOp op{StructOp::CopySheet, base};
    op.name = wb_.sheets[static_cast<size_t>(new_index)]->name;
    op.index = new_index;
    RecordStruct(std::move(op));
  }
  Reindex();
  // the source's sheet-level names come with the copy, as in Excel: references to the source sheet point at the copy
  {
    std::vector<model::NameDef> local;
    for (const auto& n : *wb_.names) {
      if (n.sheet_scope == static_cast<int32_t>(id)) local.push_back(n);
    }
    // a workbook name that refers to the source sheet gets a sheet-level twin on the copy, referring to the copy
    // (Excel: Glob =Data!$A$1 copied with Data gives 'Data (2)'!Glob ='Data (2)'!$A$1)
    {
      std::vector<model::NameDef> twins;
      for (const auto& n : *wb_.names) {
        if (n.sheet_scope >= 0) continue;
        const bool taken = std::any_of(local.begin(), local.end(), [&](const model::NameDef& l) { return Lower(l.name) == Lower(n.name); });
        if (taken) continue;
        std::string_view t = n.refers_to;
        if (!t.empty() && t[0] == '=') t.remove_prefix(1);
        formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);
        if (!pr.ok) continue;
        bool names_src = false;
        formula::ForEachRef(pr.root, [&](formula::RefSpec& r) { names_src = names_src || (r.sheet == static_cast<int32_t>(id) && r.sheet2 < 0); });
        if (!names_src) continue;
        model::NameDef twin = n;
        twin.sheet_scope = static_cast<int32_t>(id);  // retargeted below with the sheet-level names
        twins.push_back(std::move(twin));
      }
      local.insert(local.end(), twins.begin(), twins.end());
    }
    for (auto& n : local) {
      n.sheet_scope = static_cast<int32_t>(nid);
      std::string_view t = n.refers_to;
      const bool eq = !t.empty() && t[0] == '=';
      if (eq) t.remove_prefix(1);
      formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);
      if (!pr.ok) continue;
      bool moved = false;
      formula::ForEachRef(pr.root, [&](formula::RefSpec& r) {
        if (r.sheet == static_cast<int32_t>(id) && r.sheet2 < 0) {
          r.sheet = static_cast<int32_t>(nid);
          moved = true;
        }
      });
      if (moved) n.refers_to = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
    }
    if (!local.empty()) {
      auto& names = wb_.names.mut();
      names.insert(names.end(), local.begin(), local.end());
    }
  }
  // the copy's objects show the copy's cells where the source's showed the source's, as in Excel (measured): charts,
  // a form control's linked cell and list ('Data (2)'!$F$2), a sparkline group's date axis; its conditional formats
  // and validation name no sheet then (=S!$B2>1 on S is =$B2>1 on the copy). Texts are read at A1 and written back
  // there, so relative references keep their text.
  auto point_at_copy = [&](std::string& text, int32_t to) {
    if (text.empty()) return;
    std::string_view t = text;
    const bool eq = t[0] == '=';
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, core::CellPos{0, 0}, this);
    if (!pr.ok) return;
    bool moved = false;
    formula::ForEachRef(pr.root, [&](formula::RefSpec& r) {
      if (r.sheet == static_cast<int32_t>(id) && r.sheet2 < 0) {
        r.sheet = to;
        moved = true;
      }
    });
    if (moved) text = (eq ? "=" : "") + formula::ToA1(pr.root, core::CellPos{0, 0}, this);
  };
  for (auto& sc : wb_.sheets) {
    if (sc->id != nid) continue;
    const int32_t to = static_cast<int32_t>(nid);
    model::EditObjectRefTexts(sc, [&](std::string& text) { point_at_copy(text, to); });
    model::EditRuleRefTexts(sc, [&](std::string& text, core::CellPos, bool constants_ok) {
      if (!(constants_ok && text[0] != '=')) point_at_copy(text, -1);
    });
  }
  // the copy's formulas come with their values: registered, and calculated again only when volatile (3-D references,
  // names, INDIRECT, SHEET). Rebuilding and recalculating the workbook took seconds with millions of formulas.
  if (const Sheet* ns = S(nid)) {
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> cells;
    ns->meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t idx = 0; idx < model::kTileCells; ++idx) {
        if (t.formula[idx]) cells.emplace_back(rt * model::kTileRows + idx % model::kTileRows, ct * model::kTileCols + idx / model::kTileRows, t.formula[idx]);
      }
    });
    // a formula naming the source sheet (=Src!A1 on Src) names the copy in the copy, as in Excel ('Src (2)'!A1)
    std::unordered_map<uint32_t, uint32_t> retarget;
    for (auto& [r, c, fid] : cells) {
      auto it = retarget.find(fid);
      if (it == retarget.end()) {
        uint32_t to = fid;
        if (const auto* f = Formula(fid)) {
          formula::Node root = f->root;
          bool named = false;
          formula::ForEachRef(root, [&](formula::RefSpec& ref) {
            if (ref.sheet == static_cast<int32_t>(id) && ref.sheet2 < 0) {
              ref.sheet = static_cast<int32_t>(nid);
              named = true;
            }
          });
          // and a sheet-level name of the source (=Data!Loc on Data) the copy's own ('Data (2)'!Loc)
          // and a table of the source its copy on the copy (=SUM(Table1[v]) is =SUM(Table2[v]) there, as in Excel)
          std::function<void(formula::Node&)> walk = [&](formula::Node& nd) {
            if (nd.kind == formula::NK::Name && nd.ref.sheet == static_cast<int32_t>(id)) {
              nd.ref.sheet = static_cast<int32_t>(nid);
              named = true;
            }
            if (nd.kind == formula::NK::StructRef && !nd.str.empty()) {
              if (auto tn = table_names.find(Lower(nd.str)); tn != table_names.end()) {
                nd.str = tn->second;
                named = true;
              }
            }
            for (auto& k : nd.kids) walk(k);
          };
          walk(root);
          if (named) to = InternFormula(std::move(root));
        }
        it = retarget.emplace(fid, to).first;
      }
      if (it->second != fid) {
        fid = it->second;
        if (Sheet* ms = M(nid)) ms->SetFormula(r, c, fid);
      }
    }
    register_pending_ = false;
    for (auto [r, c, fid] : cells) RegisterFormulaCell(nid, r, c, fid, true);  // new cells: nothing to take out first
    register_pending_ = true;
    // a copy of a sheet whose calculation is off calculates (Excel 16, measured: the copy's EnableCalculation is True
    // and a formula the source shows as 0 shows its value there): its cells hold the values the source kept, and a
    // formula reading no cell (=SUM(#REF!)) was never calculated again (doc_fuzz --gaps 700139)
    if (calc_off_.count(id)) MarkDirty(nid, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1});
    // the copy's arrays join the spill index, and its #SPILL! anchors wait for room as the source's do: without
    // them a range over the copy's array didn't wait for it, and deleting the cell in a copied anchor's way left
    // it #SPILL! until the workbook was opened again (doc_fuzz seed 10240003)
    for (const auto& [k, sp] : *ns->spills) {
      const uint64_t a = Key(nid, static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF));
      spill_anchor_[a] = sp.rect;
      IndexSpill(a, sp.rect, true);
    }
    std::vector<std::pair<uint64_t, Rect>> waiting;
    for (const auto& [k, want] : spill_blocked_) {
      if (KeySheet(k) == id) waiting.emplace_back(Key(nid, KeyRow(k), KeyCol(k)), want);
    }
    for (const auto& [k, want] : waiting) spill_blocked_[k] = want;
  }
  MarkAllVolatileDirty();
  wb_.dirty = true;
  return nid;
}

bool Document::SetSheetVisible(uint32_t id, bool visible, bool very) {
  if (RefuseStructure()) return false;
  const Sheet* cur = S(id);
  if (!cur) return false;
  if (!visible && cur->visible) {
    int vis = 0;
    for (const auto& s : wb_.sheets) vis += s->visible ? 1 : 0;
    if (vis <= 1) return false;  // a workbook keeps at least one visible sheet
  }
  if (cur->visible == visible && cur->very_hidden == (!visible && very)) return true;
  Sheet* s = M(id);
  NoteTouched();
  s->visible = visible;
  s->very_hidden = !visible && very;
  wb_.dirty = true;
  return true;
}

void Document::SetTabColor(uint32_t id, uint32_t color) {
  if (RefuseStructure()) return;
  Sheet* s = M(id);
  if (!s) return;
  NoteTouched();
  s->tab_color = color;
  wb_.dirty = true;
}

// ---- Controls and charts ----

uint32_t Document::AddControl(uint32_t sheet, model::Control c) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  NoteTouched();
  c.id = wb_.next_object_id++;
  if (c.name.empty()) {
    // ActiveX controls: CommandButton1; form controls as Excel names them: Button 1, Drop Down 2 (as Shapes.AddFormControl)
    const std::string base = c.form ? std::string(model::FormControlTypeName(c.kind)) + " " : std::string(model::ControlTypeName(c.kind));
    for (int n = 1;; ++n) {
      const std::string name = base + std::to_string(n);
      bool dup = false;
      for (const auto& x : *s->controls) dup = dup || x.name == name;
      if (!dup) {
        c.name = name;
        break;
      }
    }
  }
  using K = model::ControlKind;
  if (c.caption.empty() && (c.kind == K::Button || c.kind == K::CheckBox || c.kind == K::OptionButton || c.kind == K::ToggleButton || c.kind == K::Label))
    c.caption = c.name;
  if (c.value.empty() && (c.kind == K::CheckBox || c.kind == K::OptionButton || c.kind == K::ToggleButton)) c.value = "False";
  if (c.value.empty() && (c.kind == K::SpinButton || c.kind == K::ScrollBar)) c.value = std::to_string(c.min);
  int z = 0;
  for (const auto& x : *s->controls) z = std::max(z, x.z + 1);
  for (const auto& x : *s->charts) z = std::max(z, x.z + 1);
  for (const auto& x : *s->shapes) z = std::max(z, x.z + 1);
  c.z = z;
  const uint32_t id = c.id;
  s->controls.mut().push_back(std::move(c));
  wb_.dirty = true;
  return id;
}

bool Document::UpdateControl(uint32_t sheet, uint32_t id, const std::function<void(model::Control&)>& fn) {
  if (RefuseReadOnly(sheet)) return false;  // read-only mode: a control's value, place and look stay
  Sheet* s = M(sheet);
  if (!s) return false;
  for (auto& c : s->controls.mut()) {
    if (c.id != id) continue;
    NoteTouched();
    fn(c);
    wb_.dirty = true;
      return true;
  }
  return false;
}

bool Document::DeleteControl(uint32_t sheet, uint32_t id) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& v = s->controls.mut();
  const auto n = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::Control& c) { return c.id == id; }), v.end());
  if (v.size() == n) return false;
  NoteTouched();
  wb_.dirty = true;
  return true;
}

uint32_t Document::AddChart(uint32_t sheet, model::ChartObj c) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  NoteTouched();
  c.id = wb_.next_object_id++;
  if (c.name.empty()) {
    for (int n = 1;; ++n) {
      const std::string name = TR("Chart ") + std::to_string(n);
      bool dup = false;
      for (const auto& x : *s->charts) dup = dup || x.name == name;
      if (!dup) {
        c.name = name;
        break;
      }
    }
  }
  int z = 0;
  for (const auto& x : *s->controls) z = std::max(z, x.z + 1);
  for (const auto& x : *s->charts) z = std::max(z, x.z + 1);
  for (const auto& x : *s->shapes) z = std::max(z, x.z + 1);
  c.z = z;
  const uint32_t id = c.id;
  s->charts.mut().push_back(std::move(c));
  wb_.dirty = true;
  return id;
}

bool Document::UpdateChart(uint32_t sheet, uint32_t id, const std::function<void(model::ChartObj&)>& fn) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  for (auto& c : s->charts.mut()) {
    if (c.id != id) continue;
    NoteTouched();
    const double left = c.left, top = c.top;
    fn(c);
    // the shapes drawn on it (Chart.Shapes) move with it
    if ((c.left != left || c.top != top) && !s->shapes->empty()) {
      const double dx = c.left - left, dy = c.top - top;
      for (auto& sh : s->shapes.mut()) {
        if (sh.on_chart == id) sh.left += dx, sh.top += dy;
      }
    }
    wb_.dirty = true;
    return true;
  }
  return false;
}

bool Document::DeleteChart(uint32_t sheet, uint32_t id) {
  if (Refuse(sheet, nullptr, Allow::Objects)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& v = s->charts.mut();
  const auto n = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::ChartObj& c) { return c.id == id; }), v.end());
  if (v.size() == n) return false;
  // ... and go with it
  if (std::any_of(s->shapes->begin(), s->shapes->end(), [&](const model::ShapeObj& sh) { return sh.on_chart == id; })) {
    auto& shapes = s->shapes.mut();
    shapes.erase(std::remove_if(shapes.begin(), shapes.end(), [&](const model::ShapeObj& sh) { return sh.on_chart == id; }), shapes.end());
  }
  NoteTouched();
  wb_.dirty = true;
  return true;
}

// ---- Chart sheets ----

namespace {

// "B2:B9" or "$B$2:$B$9" (not a defined name)
bool IsPlainA1(std::string_view s) {
  size_t i = 0;
  auto cell = [&] {
    if (i < s.size() && s[i] == '$') ++i;
    const size_t l0 = i;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
    if (i == l0 || i - l0 > 3) return false;
    if (i < s.size() && s[i] == '$') ++i;
    const size_t d0 = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    return i > d0;
  };
  if (!cell()) return false;
  if (i < s.size() && s[i] == ':') {
    ++i;
    if (!cell()) return false;
  }
  return i == s.size();
}

// a series reference without its sheet still points at that sheet from another one
std::string QualifyRef(const std::string& ref, const std::string& sheet, bool formula_only) {
  if (ref.empty() || ref.find('!') != std::string::npos) return ref;
  const bool eq = ref[0] == '=';
  if (formula_only && !eq) return ref;  // a series name without '=' is literal text
  const std::string body = eq ? ref.substr(1) : ref;
  if (!IsPlainA1(body)) return ref;
  return (eq ? "=" : "") + core::QuoteSheetName(sheet) + "!" + body;
}

}  // namespace

uint32_t Document::AddChartSheet(std::string name, int32_t index, model::ChartObj c) {
  if (name.empty() || wb_.FindSheet(name) >= 0) {
    for (int n = 1;; ++n) {
      name = TR("Chart") + std::to_string(n);
      if (wb_.FindSheet(name) < 0) break;
    }
  }
  const uint32_t id = AddSheet(name, index);
  Sheet* s = id ? M(id) : nullptr;
  if (!s) return 0;
  s->chart_sheet = true;
  s->page_setup.mut().landscape = true;  // like Excel: chart sheets print landscape
  c.left = c.top = 0;
  c.floating = false;
  c.interactive = false;
  AddChart(id, std::move(c));
  return id;
}

uint32_t Document::MoveChart(uint32_t from, uint32_t id, uint32_t to, const std::string& new_name, double left, double top, uint32_t* new_id) {
  if (RefuseReadOnly(from)) return 0;
  const Sheet* src = S(from);
  if (!src) return 0;
  const model::ChartObj* found = nullptr;
  for (const auto& c : *src->charts) {
    if (c.id == id) found = &c;
  }
  if (!found) return 0;
  model::ChartObj c = *found;
  const bool from_chart_sheet = src->chart_sheet;
  const std::string src_name = src->name;
  const int32_t src_index = wb_.IndexOfId(from);
  if (!from_chart_sheet) {
    for (auto& se : c.series) {
      se.values = QualifyRef(se.values, src_name, false);
      se.x_values = QualifyRef(se.x_values, src_name, false);
      se.name = QualifyRef(se.name, src_name, true);
    }
    if (!c.source.empty()) c.source = QualifyRef(c.source, src_name, false);
  }
  uint32_t dest = 0;
  if (to == 0) {
    if (from_chart_sheet) {  // already on a chart sheet: just rename it
      if (!new_name.empty() && new_name != src_name && !RenameSheet(from, new_name)) return 0;
      if (new_id) *new_id = id;
      return from;
    }
    dest = AddChartSheet(new_name, src_index + 1, c);
    if (!dest) return 0;
    for (const auto& x : *S(dest)->charts) {
      if (new_id) *new_id = x.id;
    }
  } else {
    const Sheet* t = S(to);
    if (!t || t->chart_sheet || to == from) return 0;
    c.left = std::max(0.0, left);
    c.top = std::max(0.0, top);
    c.placement = model::Placement::MoveAndSize;
    for (const auto& x : *t->charts) {
      if (x.name == c.name) c.name.clear();  // "Chart N" again
    }
    const uint32_t nid = AddChart(to, std::move(c));
    if (!nid) return 0;
    if (new_id) *new_id = nid;
    dest = to;
  }
  DeleteChart(from, id);
  if (from_chart_sheet) DeleteSheet(from);
  return dest;
}

// ---- Custom panels ----

void Document::UpsertPanel(model::Panel p) {
  NoteTouched();
  auto& v = wb_.panels.mut();
  for (auto& x : v) {
    if (x.name == p.name) {
      x = std::move(p);
      wb_.dirty = true;
      return;
    }
  }
  v.push_back(std::move(p));
  wb_.dirty = true;
}

bool Document::UpdatePanel(const std::string& name, const std::function<void(model::Panel&)>& fn) {
  for (auto& x : wb_.panels.mut()) {
    if (x.name != name) continue;
    NoteTouched();
    fn(x);
    wb_.dirty = true;
    return true;
  }
  return false;
}

bool Document::DeletePanel(const std::string& name) {
  auto& v = wb_.panels.mut();
  const auto n = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::Panel& p) { return p.name == name; }), v.end());
  if (v.size() == n) return false;
  NoteTouched();
  wb_.dirty = true;
  return true;
}

// ---- Defined names ----

bool Document::AddName(const std::string& name, const std::string& refers_to, int32_t scope, std::string* error) {
  // keep the existing comment and hidden flag
  model::NameDef def{name, refers_to, scope};
  if (const model::NameDef* old = scope >= 0 ? FindName(name, static_cast<uint32_t>(scope)) : FindName(name, UINT32_MAX)) {
    if (old->sheet_scope == scope) {
      def.comment = old->comment;
      def.hidden = old->hidden;
    }
  }
  return DefineName(std::move(def), error);
}

bool Document::DeleteName(const std::string& name, int32_t scope) {
  if (RefuseReadOnly()) return false;
  auto& names = wb_.names.mut();
  const auto before = names.size();
  names.erase(std::remove_if(names.begin(), names.end(), [&](const model::NameDef& n) { return n.sheet_scope == scope && Lower(n.name) == Lower(name); }), names.end());
  if (names.size() == before) return false;
  NoteStructural();
  MarkAllVolatileDirty();
  wb_.dirty = true;
  return true;
}

// Find / Replace, filters and Remove Duplicates ignore case as formulas do: accented and full-width letters too
std::string Lower(std::string_view s) { return formula::FoldCase(s); }

}  // namespace yc::engine
