// SPDX-License-Identifier: Apache-2.0
// document_trace.cpp - formula auditing: precedents and dependents (Range.Precedents / DirectPrecedents / Dependents / DirectDependents)
// Like Excel: only cells on the same sheet are returned (OtherSheet*: the ones on other sheets, for the sheet icon arrow).
#include "engine/document.h"
#include "engine/navigate.h"
#include "engine/tile_iter.h"
#include "formula/parser.h"

#include <algorithm>
#include <unordered_set>
#include <array>
#include <set>

namespace yc::engine {

namespace {
constexpr size_t kTraceLimit = 200000;  // limit on visited formula cells (so huge workbooks don't hang)
}

std::vector<Rect> Document::Precedents(uint32_t sheet, const Rect& range, bool direct) {
  const model::Sheet* s = S(sheet);
  std::vector<Rect> out;
  if (!s || !s->has_used) return out;
  std::unordered_set<uint64_t> seen;
  std::set<std::array<uint32_t, 4>> listed;  // each range once: a find over the list so far was quadratic in a long chain
  std::vector<Rect> frontier{range};
  auto add = [&](Rect r) {
    if (r.r0 >= s->row_limit) return;                 // entirely past the sheet: nothing to select (r0 > r1 otherwise)
    if (r.r1 >= s->row_limit) r.r1 = core::kRowEnd;  // entire columns stay entire columns
    if (listed.insert({r.r0, r.c0, r.r1, r.c1}).second) out.push_back(r);
    if (!direct) frontier.push_back(r);
  };
  while (!frontier.empty() && seen.size() < kTraceLimit) {
    Rect cur = frontier.back();
    frontier.pop_back();
    cur.r1 = std::min(cur.r1, s->used.r1);
    cur.c1 = std::min(cur.c1, s->used.c1);
    if (cur.r0 > cur.r1 || cur.c0 > cur.c1) continue;
    std::vector<core::CellPos> cells;
    ForEachTileInRect(s->meta, cur, [&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      const uint32_t br = rt * model::kTileRows, bc = ct * model::kTileCols;
      for (uint32_t r = std::max(br, cur.r0); r <= std::min(br + model::kTileRows - 1, cur.r1); ++r) {
        for (uint32_t c = std::max(bc, cur.c0); c <= std::min(bc + model::kTileCols - 1, cur.c1); ++c) {
          if (t.formula[model::TileIndex(r, c)] && seen.insert(core::CellKey(sheet, r, c)).second) cells.push_back(core::CellPos{r, c});
        }
      }
    });
    for (const core::CellPos& p : cells) {
      const formula::CompiledFormula* f = Formula(s->GetFormula(p.row, p.col));
      if (!f) continue;
      std::vector<formula::ResolvedRef> refs;
      formula::CollectRefs(f->root, p, &refs);
      for (const auto& ref : refs) {
        if (ref.sheet < 0 || static_cast<uint32_t>(ref.sheet) == sheet) add(ref.rect);
      }
      // names used in formulas: resolved to ranges (dynamic names too)
      std::vector<std::string> names;
      formula::CollectNames(f->root, &names);
      for (const std::string& n : names) {
        uint32_t sid = 0;
        Rect r;
        if (ResolveNameRef(n, sheet, &sid, &r, nullptr, p) && sid == sheet) add(r);
      }
    }
  }
  return out;
}

std::vector<Rect> Document::Dependents(uint32_t sheet, const Rect& range, bool direct) {
  std::vector<core::CellPos> cells;
  std::vector<Rect> arrays;  // a multi-cell array whole: listed a cell at a time, 4,000,000 of them took 2.4 s
  std::unordered_set<uint64_t> seen;
  std::vector<Rect> frontier{range};
  while (!frontier.empty() && seen.size() < kTraceLimit) {
    const Rect cur = frontier.back();
    frontier.pop_back();
    deps_.QueryRect(sheet, cur, [&](uint64_t key) {
      if (static_cast<uint32_t>(key >> 48) != sheet) return;
      const uint32_t r = static_cast<uint32_t>((key >> 16) & 0xFFFFFFFFull), c = static_cast<uint32_t>(key & 0xFFFF);
      if (!seen.insert(key).second) return;
      // an array formula (Ctrl+Shift+Enter) is registered at its first cell and is every one of its cells (Excel 16,
      // measured: B2's dependents are $G$1:$G$2 for {=B2:B3*2} in G1:G2)
      Rect whole = Rect::Cell(r, c);
      if (const model::Sheet* s = S(sheet); s && !s->spills->empty()) {
        if (auto it = s->spills->find((uint64_t{r} << 16) | c); it != s->spills->end() && it->second.cse) whole = it->second.rect;
      }
      // only its first cell is in the dependency index, so its other cells never come back as keys
      if (whole.IsCell()) cells.push_back(core::CellPos{r, c});
      else arrays.push_back(whole);
      if (!direct) frontier.push_back(whole);
    });
  }
  std::vector<Rect> out = CellsToRects(std::move(cells));
  out.insert(out.end(), arrays.begin(), arrays.end());
  return out;
}

std::vector<std::pair<int32_t, Rect>> Document::OtherSheetPrecedents(uint32_t sheet, uint32_t r, uint32_t c) {
  std::vector<std::pair<int32_t, Rect>> out;
  const model::Sheet* s = S(sheet);
  const formula::CompiledFormula* f = s ? Formula(s->GetFormula(r, c)) : nullptr;
  if (!f) return out;
  auto add = [&](int32_t sid, Rect q) {
    if (const model::Sheet* qs = sid >= 0 ? S(static_cast<uint32_t>(sid)) : nullptr; qs && q.r1 >= qs->row_limit) q.r1 = core::kRowEnd;
    const std::pair<int32_t, Rect> x{sid, q};
    if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);  // lesson-ok: find-push one formula's references
  };
  std::vector<formula::ResolvedRef> refs;
  formula::CollectRefs(f->root, core::CellPos{r, c}, &refs);
  for (const auto& ref : refs) {
    if (ref.sheet >= 0 && static_cast<uint32_t>(ref.sheet) != sheet) add(ref.sheet, ref.rect);
  }
  std::vector<std::string> names;
  formula::CollectNames(f->root, &names);
  for (const std::string& n : names) {
    uint32_t sid = 0;
    Rect q;
    if (ResolveNameRef(n, sheet, &sid, &q, nullptr, core::CellPos{r, c}) && sid != sheet) add(static_cast<int32_t>(sid), q);
  }
  return out;
}

std::vector<std::pair<int32_t, Rect>> Document::OtherSheetDependents(uint32_t sheet, uint32_t r, uint32_t c) {
  std::vector<std::pair<int32_t, Rect>> out;
  deps_.QueryRect(sheet, Rect::Cell(r, c), [&](uint64_t key) {
    const uint32_t ks = static_cast<uint32_t>(key >> 48);
    if (ks == sheet || out.size() >= 1000) return;
    const std::pair<int32_t, Rect> x{static_cast<int32_t>(ks), Rect::Cell(static_cast<uint32_t>((key >> 16) & 0xFFFFFFFFull), static_cast<uint32_t>(key & 0xFFFF))};
    if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);  // lesson-ok: find-push at most 1,000
  });
  return out;
}

}  // namespace yc::engine
