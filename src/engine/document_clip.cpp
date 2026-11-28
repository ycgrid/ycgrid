// SPDX-License-Identifier: Apache-2.0
// document_clip.cpp - copy and paste, fill, AutoFill, sort, find and replace
#include "engine/document.h"
#include "core/i18n.h"

#include "engine/custom_lists.h"
#include "engine/rule_eval.h"
#include "engine/tile_iter.h"
#include "core/decimal.h"
#include "formula/collate.h"
#include "formula/datetime.h"
#include "formula/eval.h"
#include "formula/fn_util.h"
#include "formula/numfmt.h"
#include "model/theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace yc::engine {

using model::Sheet;
using model::Style;

namespace {

Rect ClampToUsed(const Sheet& s, Rect r) {
  if (!s.has_used) return Rect{r.r0, r.c0, r.r0, r.c0};
  r.r1 = std::max(r.r0, std::min(r.r1, s.used.r1));
  r.c1 = std::max(r.c0, std::min(r.c1, s.used.c1));
  return r;
}

// after moving a formula to its new origin, references that fall outside the sheet become #REF!
bool FixOutOfBounds(formula::Node& root, core::CellPos origin, uint32_t row_limit) {
  bool changed = false;
  formula::ForEachRef(root, [&](formula::RefSpec& ref) {
    // a reference whose sheet was deleted (#REF!A1:A3) still has its cells: moved off the sheet it is #REF! alone, as
    // its text reads back. It was left as it was, shared its formula with the cell it was copied from, and a save
    // wrote that formula's text at the copy: the original read #REF! after an open (doc_fuzz seed 10460222)
    if (ref.bad && !ref.lost_sheet) return;
    auto bad_row = [&](const formula::RefCoord& c) {
      const int64_t v = c.Resolve(origin.row);
      return v < 0 || v >= int64_t{row_limit};
    };
    auto bad_col = [&](const formula::RefCoord& c) {
      const int64_t v = c.Resolve(origin.col);
      return v < 0 || v >= int64_t{core::kMaxCols};
    };
    bool bad = false;
    if (!ref.whole_cols) bad = bad || bad_row(ref.r0) || (ref.is_range && bad_row(ref.r1));
    if (!ref.whole_rows) bad = bad || bad_col(ref.c0) || (ref.is_range && bad_col(ref.c1));
    if (bad) {
      ref.bad = true;
      ref.lost_sheet = false;
      changed = true;
    }
  });
  return changed;
}

// Paste Special > Transpose: the formula of copied cell (i, j) going to (r + j, c + i). Excel (measured): references
// to the copied cells turn with them, absolute ones too, and a $ on the row moves to the column ($A1 -> E$1 when A1 is
// copied to E1); relative references elsewhere turn about the cell (one column left becomes one row up); absolute and
// mixed ones elsewhere keep the cells they name
void TransposeRefs(formula::Node& root, const ClipData& clip, uint32_t i, uint32_t j, uint32_t r, uint32_t c) {
  const int64_t sr = int64_t{clip.src.r0} + i, sc = int64_t{clip.src.c0} + j;  // where the formula was
  const int64_t dr = int64_t{r} + j, dc = int64_t{c} + i;                        // where it goes
  formula::ForEachRef(root, [&](formula::RefSpec& ref) {
    if (ref.whole_cols || ref.whole_rows || !ref.HasCells() || ref.sheet2 >= 0) return;
    const int64_t R0 = ref.r0.Resolve(sr), C0 = ref.c0.Resolve(sc);
    const int64_t R1 = ref.is_range ? ref.r1.Resolve(sr) : R0, C1 = ref.is_range ? ref.c1.Resolve(sc) : C0;
    const bool own_sheet = ref.TargetSheet(static_cast<int32_t>(clip.src_sheet)) == static_cast<int32_t>(clip.src_sheet);
    const bool inside = own_sheet && std::max(R0, R1) >= clip.src.r0 && std::min(R0, R1) <= clip.src.r1 && std::max(C0, C1) >= clip.src.c0 &&
                        std::min(C0, C1) <= clip.src.c1;
    const bool rel = !ref.r0.abs && !ref.c0.abs && (!ref.is_range || (!ref.r1.abs && !ref.c1.abs));
    if (rel || inside) {
      auto turn = [&](formula::RefCoord& row, formula::RefCoord& col, int64_t R, int64_t C) {
        const int64_t nr = int64_t{r} + (C - clip.src.c0), nc = int64_t{c} + (R - clip.src.r0);
        const bool row_abs = col.abs, col_abs = row.abs;
        row = formula::RefCoord{row_abs ? nr : nr - dr, row_abs};
        col = formula::RefCoord{col_abs ? nc : nc - dc, col_abs};
      };
      turn(ref.r0, ref.c0, R0, C0);
      if (ref.is_range) turn(ref.r1, ref.c1, R1, C1);
      return;
    }
    auto keep = [](formula::RefCoord& x, int64_t at, int64_t origin) {
      if (!x.abs) x.v = at - origin;
    };
    keep(ref.r0, R0, dr);
    keep(ref.c0, C0, dc);
    if (ref.is_range) {
      keep(ref.r1, R1, dr);
      keep(ref.c1, C1, dc);
    }
  });
}

}  // namespace

Rect CopyAreaOf(const model::Sheet& s, const Rect& rect) {
  Rect a = rect;
  if (a.r1 >= s.row_limit - 1) a.r1 = core::kMaxRows - 1;
  if (a.c1 >= core::kMaxCols - 1) a.c1 = core::kMaxCols - 1;
  return a;
}

bool AreaRunsOffSheet(const Rect& area, uint32_t r, uint32_t c, bool transpose) {
  const uint64_t rows = transpose ? area.Cols() : area.Rows(), cols = transpose ? area.Rows() : area.Cols();
  return uint64_t{r} + rows > core::kMaxRows || uint64_t{c} + cols > core::kMaxCols;
}

bool PasteRunsOffSheet(const ClipData& clip, uint32_t r, uint32_t c, bool transpose) {
  const Rect area = clip.area.r1 >= clip.area.r0 && clip.area.c1 >= clip.area.c0 && (clip.area.r1 | clip.area.c1) != 0
                        ? clip.area
                        : Rect{0, 0, std::max<uint32_t>(clip.rows, 1) - 1, std::max<uint32_t>(clip.cols, 1) - 1};
  return AreaRunsOffSheet(area, r, c, transpose);
}

uint64_t Document::CopyCellCount(uint32_t sheet, const Rect& rect_in) const {
  const Sheet* s = S(sheet);
  if (!s) return 0;
  const bool to_end = rect_in.r1 >= s->row_limit - 1 || rect_in.c1 >= core::kMaxCols - 1 || rect_in.Count() > 4'000'000;
  return (to_end ? ClampToUsed(*s, rect_in) : rect_in).Count();
}

ClipData Document::Copy(uint32_t sheet, const Rect& rect_in) const {
  ClipData clip;
  const Sheet* s = S(sheet);
  if (!s) return clip;
  // whole rows / columns (or a huge block) are cut down to the used range; any other block is copied as selected, so
  // its empty cells clear the destination's (Excel: A1:D10 copied pastes 10 x 4 cells wherever the data ends)
  const bool to_end = rect_in.r1 >= s->row_limit - 1 || rect_in.c1 >= core::kMaxCols - 1 || rect_in.Count() > 4'000'000;
  const Rect rect = to_end ? ClampToUsed(*s, rect_in) : rect_in;
  clip.src_sheet = sheet;
  clip.src = rect;
  clip.area = CopyAreaOf(*s, rect_in);
  clip.rows = static_cast<uint32_t>(rect.Rows());
  clip.cols = static_cast<uint32_t>(rect.Cols());
  if (uint64_t{clip.rows} * clip.cols > 20000000) {
    clip.rows = clip.cols = 0;
    return clip;
  }
  clip.cells.resize(static_cast<size_t>(clip.rows) * clip.cols);
  for (uint32_t r = 0; r < clip.rows; ++r) {
    for (uint32_t c = 0; c < clip.cols; ++c) {
      ClipCell& cc = clip.cells[static_cast<size_t>(r) * clip.cols + c];
      const uint32_t sr = rect.r0 + r, scol = rect.c0 + c;
      cc.value = s->GetValue(sr, scol);
      if (const uint32_t fid = s->GetFormula(sr, scol)) cc.formula = wb_.formulas->Share(fid);
      const uint32_t sid = s->GetStyle(sr, scol);
      if (sid) {
        cc.style = wb_.styles->Get(sid);
        cc.has_style = true;
        // string ids become portable contents: font names and format codes would be cleaner outside the style; within one workbook ids work as is
      }
    }
  }
  for (const Rect& m : *s->merges) {
    if (rect.Contains(m)) clip.merges.push_back(Rect{m.r0 - rect.r0, m.c0 - rect.c0, m.r1 - rect.r0, m.c1 - rect.c0});
  }
  for (const auto& [k, sp] : *s->spills) {
    if (!sp.cse || !sp.rect.Intersects(rect)) continue;
    const uint32_t fid = s->GetFormula(static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF));
    if (!fid) continue;
    const Rect in{std::max(sp.rect.r0, rect.r0), std::max(sp.rect.c0, rect.c0), std::min(sp.rect.r1, rect.r1), std::min(sp.rect.c1, rect.c1)};
    clip.arrays.push_back(ClipData::ArrayPart{Rect{in.r0 - rect.r0, in.c0 - rect.c0, in.r1 - rect.r0, in.c1 - rect.c0}, wb_.formulas->Share(fid)});
  }
  for (uint32_t c = rect.c0; c <= rect.c1; ++c) clip.col_widths.push_back(s->cols->Size(c));
  if (clip.area.c0 == 0 && clip.area.c1 >= core::kMaxCols - 1 && clip.area.Rows() <= 65536) {
    for (uint32_t r = clip.area.r0; r <= clip.area.r1; ++r) clip.line_heights.push_back(s->rows->IsHidden(r) ? -1 : s->rows->Size(r));
  }
  if (clip.area.r0 == 0 && clip.area.r1 >= core::kMaxRows - 1) {
    for (uint32_t c = clip.area.c0; c <= clip.area.c1; ++c) clip.line_widths.push_back(s->cols->IsHidden(c) ? -1 : s->cols->Size(c));
  }
  for (const auto& [k, rt] : *s->rich) {
    const uint32_t rr = static_cast<uint32_t>(k >> 16), cc = static_cast<uint32_t>(k & 0xFFFF);
    if (!rect.Contains(rr, cc)) continue;
    if (const model::RichText* live = RichTextAt(sheet, rr, cc)) clip.rich.emplace_back(static_cast<size_t>(rr - rect.r0) * clip.cols + (cc - rect.c0), *live);
  }
  for (auto it = s->links->lower_bound(uint64_t{rect.r0} << 16); it != s->links->end() && (it->first >> 16) <= rect.r1; ++it) {
    const uint32_t rr = static_cast<uint32_t>(it->first >> 16), cc = static_cast<uint32_t>(it->first & 0xFFFF);
    if (rect.Contains(rr, cc)) clip.links.emplace_back(static_cast<size_t>(rr - rect.r0) * clip.cols + (cc - rect.c0), it->second);
  }
  for (const model::CfRule& rule : *s->cond_formats) {
    if (rule.ranges.empty()) continue;
    ClipData::Rule cr{rule, core::CellPos{rule.ranges.front().r0, rule.ranges.front().c0}};
    cr.rule.ranges.clear();
    for (const Rect& x : rule.ranges) {
      if (!x.Intersects(rect)) continue;
      const Rect in{std::max(x.r0, rect.r0), std::max(x.c0, rect.c0), std::min(x.r1, rect.r1), std::min(x.c1, rect.c1)};
      cr.rule.ranges.push_back(Rect{in.r0 - rect.r0, in.c0 - rect.c0, in.r1 - rect.r0, in.c1 - rect.c0});
    }
    if (!cr.rule.ranges.empty()) clip.rules.push_back(std::move(cr));
  }
  for (const auto& [k, n] : *s->notes) {
    const uint32_t nr = static_cast<uint32_t>(k >> 16), nc = static_cast<uint32_t>(k & 0xFFFF);
    if (rect.Contains(nr, nc)) clip.notes.emplace_back((uint64_t{nr - rect.r0} << 16) | (nc - rect.c0), n);
  }
  for (const model::DvRule& rule : *s->validations) {
    if (rule.ranges.empty()) continue;
    ClipData::Validation cv{rule, core::CellPos{rule.ranges.front().r0, rule.ranges.front().c0}};
    cv.rule.ranges.clear();
    for (const Rect& x : rule.ranges) {
      if (!x.Intersects(rect)) continue;
      const Rect in{std::max(x.r0, rect.r0), std::max(x.c0, rect.c0), std::min(x.r1, rect.r1), std::min(x.c1, rect.c1)};
      cv.rule.ranges.push_back(Rect{in.r0 - rect.r0, in.c0 - rect.c0, in.r1 - rect.r0, in.c1 - rect.c0});
    }
    if (!cv.rule.ranges.empty()) clip.validations.push_back(std::move(cv));
  }
  return clip;
}

void Document::PasteRepeated(uint32_t sheet, uint32_t r, uint32_t c, uint32_t tile_r, uint32_t tile_c, const ClipData& clip, PasteMode mode,
                             bool transpose, bool skip_blanks, PasteOp op) {
  const uint32_t rows = transpose ? clip.cols : clip.rows, cols = transpose ? clip.rows : clip.cols;
  if (!rows || !cols || !tile_r || !tile_c) return;
  const Sheet* s = S(sheet);
  if (s && rows == 1 && cols == 1 && tile_r > 1) {
    const Rect dst{r, c, r + tile_r - 1, c + tile_c - 1};
    const std::vector<Rect> shown = FilterShownRuns(*s, {dst});
    if (shown.size() != 1 || !(shown[0] == dst)) {
      for (const Rect& run : shown) {
        for (uint32_t rr = run.r0; rr <= run.r1; ++rr) {
          for (uint32_t j = 0; j < tile_c; ++j) Paste(sheet, rr, c + j, clip, mode, transpose, skip_blanks, op);
        }
      }
      return;
    }
  }
  // a table's headers made text once after the tiles, not after each (a paste per tile over a wide table synced every
  // header at each one, as Fill Right did: basics seq308046)
  const bool was = syncing_tables_;
  syncing_tables_ = true;
  for (uint32_t i = 0; i < tile_r; ++i) {
    for (uint32_t j = 0; j < tile_c; ++j) Paste(sheet, r + i * rows, c + j * cols, clip, mode, transpose, skip_blanks, op);
  }
  syncing_tables_ = was;
  if (!was) SyncTableHeaders(sheet);
}

Rect Document::Paste(uint32_t sheet, uint32_t r, uint32_t c, const ClipData& clip, PasteMode mode, bool transpose, bool skip_blanks, PasteOp op) {
  {
    const Rect want{r, c, r + std::max<uint32_t>(1, transpose ? clip.cols : clip.rows) - 1, c + std::max<uint32_t>(1, transpose ? clip.rows : clip.cols) - 1};
    if (Refuse(sheet, &want) || (clip.cut && Refuse(clip.src_sheet, &clip.src))) return Rect::Cell(r, c);
    // an operation or Skip Blanks writes the array's cells one by one, or only some of them (doc_fuzz --more seed
    // 22650345: Skip Blanks over all of an array wrote one cell of it, and the array stayed over the rest)
    if ((op != PasteOp::None || skip_blanks) && mode != PasteMode::Formats && RefuseArrayOperation(sheet, want)) return Rect::Cell(r, c);
  }
  if (clip.rows == 0 || clip.cols == 0) return Rect::Cell(r, c);
  const uint32_t rows = transpose ? clip.cols : clip.rows, cols = transpose ? clip.rows : clip.cols;
  if (uint64_t{c} + cols > core::kMaxCols) return Rect::Cell(r, c);
  EnsureRowLimit(sheet, r + rows - 1);
  Sheet* s = M(sheet);
  if (!s) return Rect::Cell(r, c);
  const Rect dest{r, c, r + rows - 1, c + cols - 1};
  // a rule formula written for one first cell, written for another (relative references move with it)
  auto moved = [&](const std::string& f, core::CellPos from, core::CellPos to) -> std::string {
    if (f.empty()) return f;
    std::string_view t = f;
    const bool eq = t[0] == '=';
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, from, this);
    if (!pr.ok) return f;
    return (eq ? "=" : "") + formula::ToA1(pr.root, to, this);
  };
  // Paste Special > Comments and Notes / Validation, and the notes and validation that come with All
  const bool all_like = mode == PasteMode::All || mode == PasteMode::NoBorders || mode == PasteMode::AllSourceTheme || mode == PasteMode::AllMergingCf;
  auto paste_notes = [&] {
    if (clip.notes.empty()) return;
    NoteTouched();
    auto& notes = M(sheet)->notes.mut();
    for (const auto& [k, n] : clip.notes) {
      const uint32_t i = static_cast<uint32_t>(k >> 16), j = static_cast<uint32_t>(k & 0xFFFF);
      const uint32_t dr = r + (transpose ? j : i), dc = c + (transpose ? i : j);
      notes[(uint64_t{dr} << 16) | dc] = n;
    }
    wb_.dirty = true;
  };
  auto paste_validation = [&] {
    ClearValidation(sheet, dest);
    if (clip.validations.empty()) return;
    NoteTouched();
    for (const ClipData::Validation& cv : clip.validations) {
      model::DvRule nr = cv.rule;
      for (Rect& x : nr.ranges) {
        x = transpose ? Rect{r + x.c0, c + x.r0, r + x.c1, c + x.r1} : Rect{x.r0 + r, x.c0 + c, x.r1 + r, x.c1 + c};
      }
      const core::CellPos nb{nr.ranges.front().r0, nr.ranges.front().c0};
      if (!transpose) {
        nr.formula1 = moved(nr.formula1, cv.base, nb);
        nr.formula2 = moved(nr.formula2, cv.base, nb);
      }
      // the same rule as one already there (its formulas read from that rule's first cell) grows that one, its range
      // beside the last one making it longer: a paste a tile (one cell over 50,000) added a rule at each, and every
      // tile's ClearValidation walked them all
      auto& vs = M(sheet)->validations.mut();
      model::DvRule* same = nullptr;
      for (auto it = vs.rbegin(); it != vs.rend() && !same; ++it) {
        if (it->ranges.empty()) continue;
        const core::CellPos eb{it->ranges.front().r0, it->ranges.front().c0};
        model::DvRule a = nr, b = *it;
        a.ranges.clear();
        b.ranges.clear();
        a.formula1 = moved(a.formula1, nb, eb);
        a.formula2 = moved(a.formula2, nb, eb);
        if (a == b) same = &*it;
      }
      if (!same) {
        vs.push_back(std::move(nr));
        continue;
      }
      for (const Rect& x : nr.ranges) {
        Rect* last = &same->ranges.back();
        if (last->r0 == x.r0 && last->r1 == x.r1 && uint64_t{last->c1} + 1 == x.c0) last->c1 = x.c1;
        else if (last->c0 == x.c0 && last->c1 == x.c1 && uint64_t{last->r1} + 1 == x.r0) last->r1 = x.r1;
        else same->ranges.push_back(x);
      }
    }
    wb_.dirty = true;
  };
  if (mode == PasteMode::Comments || mode == PasteMode::Validation) {
    if (mode == PasteMode::Comments) paste_notes();
    else paste_validation();
    MarkChanged(sheet, dest, kSrcUser);
    return dest;
  }
  if (mode == PasteMode::ColumnWidths) {
    // only the widths of the copied columns
    for (uint32_t j = 0; j < cols && j < clip.col_widths.size(); ++j) {
      if (!transpose) SetColWidth(sheet, c + j, c + j, clip.col_widths[j]);
    }
    return dest;
  }
  const bool want_values = mode != PasteMode::Formats;
  const bool want_formulas = all_like || mode == PasteMode::Formulas || mode == PasteMode::FormulasAndFormats;
  const bool numfmt_only = mode == PasteMode::FormulasAndFormats;  // only the number format of the copied style
  const bool want_styles = all_like || mode == PasteMode::Formats || mode == PasteMode::ValuesAndFormats || numfmt_only;

  if (want_styles && mode != PasteMode::NoBorders) {
    // existing merged areas at the destination are removed
    auto& m = s->merges.mut();
    m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return x.Intersects(dest); }), m.end());
  }
  auto& table = wb_.styles.mut();
  for (uint32_t i = 0; i < clip.rows; ++i) {
    for (uint32_t j = 0; j < clip.cols; ++j) {
      const ClipCell& cc = clip.cells[static_cast<size_t>(i) * clip.cols + j];
      const uint32_t dr = r + (transpose ? j : i), dc = c + (transpose ? i : j);
      if (skip_blanks && cc.value.IsEmpty() && !cc.formula) continue;
      s = M(sheet);
      // the copied formula as it goes into this cell
      const bool src_formula = cc.formula && want_formulas;
      const auto* f = src_formula ? static_cast<const formula::CompiledFormula*>(cc.formula.get()) : nullptr;
      formula::Node copy;
      bool own = false;  // copy differs from the copied formula (transposed, #REF!): interned on its own
      if (f) {
        copy = f->root;
        if (transpose) {
          TransposeRefs(copy, clip, i, j, r, c);
          own = true;
        }
        own = FixOutOfBounds(copy, core::CellPos{dr, dc}, s->row_limit) || own;
      }
      bool combined = false;
      if (want_values && op != PasteOp::None) {
        // Excel (measured): the cell's number (text that reads as a number, a blank as 0) or its formula, as (formula),
        // is combined with the copied number, or with the copied formula as (formula): 5 and =1+1 give =5+(1+1). Other
        // text, TRUE / FALSE and errors stay where they are, in the cell and in the copy (the cell keeps its value)
        combined = true;
        const char sign = op == PasteOp::Add ? '+' : op == PasteOp::Subtract ? '-' : op == PasteOp::Multiply ? '*' : '/';
        auto number_text = [](double x) {
          char num[40];
          std::snprintf(num, sizeof(num), "%.15g", x);
          return std::string(num);
        };
        auto as_number = [](const Value& v, double* out) {
          if (v.IsEmpty()) {
            *out = 0;
            return true;
          }
          if (v.IsNumeric()) {
            *out = v.AsDouble();
            return true;
          }
          if (v.IsString()) {
            const Value n = formula::ParseInput(v.str()).value;
            if (n.IsNumeric()) {
              *out = n.AsDouble();
              return true;
            }
          }
          return false;
        };
        const bool dest_formula = s->GetFormula(dr, dc) != 0;
        double x = 0;
        std::string dest_text;
        if (dest_formula) dest_text = "(" + GetFormulaA1(sheet, dr, dc).substr(1) + ")";  // text without the =
        else if (as_number(s->GetValue(dr, dc), &x)) dest_text = number_text(x);
        double b = 0;
        if (dest_text.empty()) {
          // text, a logical value or an error in the cell: it stays
        } else if (f) {
          SetFormula(sheet, dr, dc, dest_text + sign + "(" + formula::ToA1(copy, core::CellPos{dr, dc}, this) + ")");
          s = M(sheet);
        } else if (as_number(cc.value, &b)) {
          if (dest_formula) {
            SetFormula(sheet, dr, dc, dest_text + sign + number_text(b));
            s = M(sheet);
          } else {
            Value out;
            switch (op) {
              case PasteOp::Add: out = Value::Number(x + b); break;
              case PasteOp::Subtract: out = Value::Number(x - b); break;
              case PasteOp::Multiply: out = Value::Number(x * b); break;
              default: out = b == 0 ? Value::Error(core::Err::Div0) : Value::Number(x / b); break;
            }
            s->SetValue(dr, dc, out);
          }
        }
      }
      if (want_values && !combined) {
        if (s->GetFormula(dr, dc)) {
          UnregisterFormulaCell(sheet, dr, dc);
          s->SetFormula(dr, dc, 0);
        }
        if (f) {
          uint32_t id;
          if (own) {
            id = InternFormula(std::move(copy));
          } else {
            auto it = formula_ids_.find(f->key);
            id = it != formula_ids_.end() ? it->second : InternFormula(std::move(copy));
          }
          s = M(sheet);
          s->SetValue(dr, dc, cc.value);
          s->SetFormula(dr, dc, id);
          RegisterFormulaCell(sheet, dr, dc, id);
          // a copied formula a circular reference keeps uncalculated shows the value it was copied with, not a typed
          // formula's 0 (Excel 16, measured: =$A$1*2 copied beside a cycle shows 10)
          fresh_formulas_.erase(Key(sheet, dr, dc));
          if (!auto_calc) entered_.push_back(Key(sheet, dr, dc));  // manual calculation: a pasted formula is computed too
        } else {
          s->SetValue(dr, dc, cc.value);
        }
      }
      if (want_styles) {
        Style st = cc.has_style ? cc.style : Style{};
        if (numfmt_only) {
          const uint32_t fmt = st.numfmt;
          st = table.Get(s->GetStyle(dr, dc));
          st.numfmt = fmt;
        }
        if (mode == PasteMode::NoBorders) {
          const Style& cur = table.Get(s->GetStyle(dr, dc));
          for (int e = 0; e < model::kEdgeCount; ++e) st.border[e] = cur.border[e];
        }
        if (mode == PasteMode::AllSourceTheme) {
          // the copied theme colors stay the colors they showed where they were copied
          for (uint32_t* col : {&st.font_color, &st.fill_color, &st.pattern_color, &st.gradient_color2}) {
            if (*col >> 24) *col = model::ResolveColor(*col);
          }
          for (auto& b : st.border) {
            if (b.color >> 24) b.color = model::ResolveColor(b.color);
          }
        }
        const uint32_t id = table.Intern(st);
        s->ShowStyle(dr, dc, id);
      }
    }
  }
  if (want_values && want_formulas && op == PasteOp::None && !transpose && !skip_blanks) {
    // the parts of Ctrl+Shift+Enter arrays: an array each, as Excel pastes them (its cells were written as values)
    for (const ClipData::ArrayPart& part : clip.arrays) {
      const Rect to{r + part.rect.r0, c + part.rect.c0, r + part.rect.r1, c + part.rect.c1};
      const auto* f = static_cast<const formula::CompiledFormula*>(part.formula.get());
      if (!f) continue;
      formula::Node copy = f->root;
      FixOutOfBounds(copy, core::CellPos{to.r0, to.c0}, M(sheet)->row_limit);
      SetArrayFormula(sheet, to, formula::ToA1(copy, core::CellPos{to.r0, to.c0}, this), nullptr, kSrcUser);
    }
    s = M(sheet);
  }
  if (want_styles && mode != PasteMode::NoBorders) {
    // a merged area that lands on a table's cells isn't made: the table holds no merged cells, and Excel 16 pastes
    // or moves the cells there unmerged (measured: Copy and Cut Destination of G7:H9 merged onto a table's cells)
    const auto& tabs = *s->tables;
    auto& m = s->merges.mut();
    for (const Rect& mr : clip.merges) {
      const Rect x = transpose ? Rect{r + mr.c0, c + mr.r0, r + mr.c1, c + mr.r1} : Rect{r + mr.r0, c + mr.c0, r + mr.r1, c + mr.c1};
      if (std::none_of(tabs.begin(), tabs.end(), [&](const model::Table& t) { return t.range.Intersects(x); })) m.push_back(x);
    }
  }
  // conditional formatting comes with the formats (not transposed): the destination's rules make way, each copied
  // rule is placed on the pasted cells, and a rule that is the same as one already there grows that one
  if (want_styles && mode != PasteMode::ValuesAndFormats && !numfmt_only && !transpose && (!clip.rules.empty() || !s->cond_formats->empty())) {
    // All merging conditional formats: the destination's rules stay and the copied ones are added
    if (mode != PasteMode::AllMergingCf) ClearCondFormats(sheet, dest);
    auto rebased = [&](model::CfRule x, core::CellPos from, core::CellPos to) {
      x.formula1 = moved(x.formula1, from, to);
      x.formula2 = moved(x.formula2, from, to);
      for (auto& t : x.icon_thresholds) t.formula = moved(t.formula, from, to);
      return x;
    };
    for (const ClipData::Rule& cr : clip.rules) {
      model::CfRule nr = cr.rule;
      for (Rect& x : nr.ranges) x = Rect{x.r0 + r, x.c0 + c, x.r1 + r, x.c1 + c};
      const core::CellPos nb{nr.ranges.front().r0, nr.ranges.front().c0};
      nr = rebased(nr, cr.base, nb);
      bool grown = false;
      Sheet* ws = M(sheet);
      for (const model::CfRule& ex : *ws->cond_formats) {
        if (ex.ranges.empty()) continue;
        const core::CellPos eb{ex.ranges.front().r0, ex.ranges.front().c0};
        if (!model::SameRuleLook(rebased(nr, nb, eb), rebased(ex, eb, eb))) continue;
        const uint32_t eid = ex.id;  // ex goes away when the rules are copied for writing
        NoteTouched();
        for (auto& y : ws->cond_formats.mut()) {
          if (y.id != eid) continue;
          // a range beside the rule's last one, as high (wide), makes it longer: a fill pastes a line at a time,
          // and a rect a column across Fill Right of the whole width made 16,383 ranges that every paste's
          // ClearCondFormats walked (seconds, doc_fuzz --wide --more seed 91473)
          for (const Rect& x : nr.ranges) {
            Rect* last = y.ranges.empty() ? nullptr : &y.ranges.back();
            if (last && last->r0 == x.r0 && last->r1 == x.r1 && uint64_t{last->c1} + 1 == x.c0) last->c1 = x.c1;
            else if (last && last->c0 == x.c0 && last->c1 == x.c1 && uint64_t{last->r1} + 1 == x.r0) last->r1 = x.r1;
            else y.ranges.push_back(x);
          }
        }
        grown = true;
        break;
      }
      if (!grown) {
        nr.id = 0;
        AddCondFormat(sheet, std::move(nr));
      }
    }
    s = M(sheet);
  }
  // fonts inside the texts come with values + formats
  if (want_values && want_styles) {
    EraseRich(sheet, dest);
    if (!clip.rich.empty()) {
      NoteRegion(sheet, dest);
      auto& rich = M(sheet)->rich.mut();
      for (const auto& [idx, rt] : clip.rich) {
        const uint32_t i = static_cast<uint32_t>(idx / clip.cols), j = static_cast<uint32_t>(idx % clip.cols);
        const uint32_t dr = r + (transpose ? j : i), dc = c + (transpose ? i : j);
        rich[(uint64_t{dr} << 16) | dc] = rt;
      }
    }
    s = M(sheet);
  }
  if (all_like) {  // the copied cells' hyperlinks, in place of those under the paste (Excel); Skip Blanks keeps them
    if (!skip_blanks) EraseLinksIn(sheet, dest);
    std::vector<std::pair<uint64_t, model::Hyperlink>> put;
    for (const auto& [idx, l] : clip.links) {
      const uint32_t i = static_cast<uint32_t>(idx / clip.cols), j = static_cast<uint32_t>(idx % clip.cols);
      const uint32_t dr = r + (transpose ? j : i), dc = c + (transpose ? i : j);
      if (dest.Contains(dr, dc)) put.emplace_back((uint64_t{dr} << 16) | dc, l);
    }
    PutLinks(sheet, put);
    s = M(sheet);
  }
  // whole rows (columns) from the clipboard bring their heights (widths), hidden ones hidden (Excel 16, measured
  // 2026-11-28: Rows(1).Copy Rows(5), Columns(1).Copy Columns(4), one row over two)
  if (all_like && clip.carry_notes && !transpose) {
    if (c == 0 && !clip.line_heights.empty()) PutLineSizes(sheet, true, r, clip.line_heights);
    if (r == 0 && !clip.line_widths.empty()) PutLineSizes(sheet, false, c, clip.line_widths);
    s = M(sheet);
  }
  if (all_like && !clip.cut && clip.carry_notes) {
    paste_notes();
    paste_validation();
    s = M(sheet);
  }
  s->Touch(dest.r0, dest.c0);
  s->Touch(dest.r1, dest.c1);
  if (clip.cut && clip.src_sheet == sheet && !clip.src.Intersects(dest)) {
    ClearAll(clip.src_sheet, clip.src, kSrcUser, false, /*to_lines=*/true);
  } else if (clip.cut) {
    // source and destination overlap: only clear the part that doesn't overlap
    for (uint32_t rr = clip.src.r0; rr <= clip.src.r1; ++rr) {
      for (uint32_t cc2 = clip.src.c0; cc2 <= clip.src.c1; ++cc2) {
        if (clip.src_sheet == sheet && dest.Contains(rr, cc2)) continue;
        ClearAll(clip.src_sheet, Rect::Cell(rr, cc2), kSrcUser, false, /*to_lines=*/true);
      }
    }
  }
  MarkChanged(sheet, dest, kSrcUser);
  return dest;
}

bool CutsMerge(const model::Sheet& s, const Rect& area) {
  for (const Rect& mg : *s.merges) {
    if (mg.Intersects(area) && !area.Contains(mg)) return true;
  }
  return false;
}

std::vector<Rect> FilterShownRuns(const model::Sheet& s, const std::vector<Rect>& areas) {
  // the rows the sheet's AutoFilter hid, and those a table's own filter hid (a table other than the first, whose
  // buttons are the sheet's): Delete, Ctrl+D and Python's writes wiped a second table's hidden rows
  const std::map<uint32_t, int32_t>* only = &s.autofilter->hidden;
  std::map<uint32_t, int32_t> merged;
  for (const auto& t : *s.tables) {
    if (t.filter.hidden.empty()) continue;
    if (merged.empty()) merged = s.autofilter->hidden;
    merged.insert(t.filter.hidden.begin(), t.filter.hidden.end());
    only = &merged;
  }
  const auto& hidden = *only;
  if (hidden.empty()) return areas;
  std::vector<Rect> out;
  for (const Rect& a : areas) {
    uint32_t from = a.r0;
    for (auto it = hidden.lower_bound(a.r0); it != hidden.end() && it->first <= a.r1; ++it) {
      if (!s.rows->IsHidden(it->first)) continue;  // shown again by hand
      if (it->first > from) out.push_back(Rect{from, a.c0, it->first - 1, a.c1});
      from = it->first + 1;
    }
    if (from <= a.r1 && from >= a.r0) out.push_back(Rect{from, a.c0, a.r1, a.c1});
  }
  // every row of the cells hidden by the filter: the cells themselves, as Excel 16 does (measured 2026-11-21: Replace,
  // Clear and FillLeft over a row the filter hides change it), as a copy of them does
  if (out.empty()) return areas;
  return out;
}

bool Document::FillDownShown(uint32_t sheet, const Rect& a_in, bool up) {
  const Sheet* fs = S(sheet);
  if (!fs) return false;
  Rect a = a_in;
  a.r1 = std::min(a.r1, fs->row_limit - 1);
  if (a.r0 >= a.r1) return false;
  const std::vector<Rect> runs = FilterShownRuns(*fs, {a});
  if (runs.empty() || (runs.size() == 1 && runs[0] == a)) return false;
  // the first (Fill Up: last) row shown goes over each run of rows shown at once (Excel 16, measured: FillUp skips the
  // rows a filter hides as FillDown does). A row at a time, a whole filtered column was a million pastes and ran out
  // of memory
  const uint32_t src = up ? runs.back().r1 : runs.front().r0;
  const ClipData top = Copy(sheet, Rect{src, a.c0, src, a.c1});
  for (Rect run : runs) {
    if (run.r0 == src) ++run.r0;
    else if (run.r1 == src) --run.r1;
    if (run.r0 > run.r1) continue;
    PasteRepeated(sheet, run.r0, a.c0, run.r1 - run.r0 + 1, 1, top);
  }
  return true;
}

void Document::DeleteCellsShown(uint32_t sheet, const Rect& rect, bool shift_up) {
  const Sheet* fs = S(sheet);
  const std::vector<Rect> runs = fs && rect.r1 < fs->row_limit ? FilterShownRuns(*fs, {rect}) : std::vector<Rect>{rect};
  // shifting up inside a filtered list takes the rows it shows whole, as Excel 16 does (measured: C3:C4 deletes row 4,
  // C8:C9 rows 8 and 9): its records stay whole
  bool rows = shift_up && fs && fs->autofilter->on && !fs->autofilter->hidden.empty() && fs->autofilter->range.Intersects(rect);
  if (shift_up && fs) {  // a table's own filter likewise
    for (const auto& t : *fs->tables) rows = rows || (!t.filter.hidden.empty() && t.range.Intersects(rect));
  }
  for (size_t i = runs.size(); i-- > 0;) {
    if (rows) DeleteRows(sheet, runs[i].r0, static_cast<uint32_t>(runs[i].Rows()));
    else DeleteCells(sheet, runs[i], shift_up);
  }
}

ClipData Document::CopyShown(uint32_t sheet, const Rect& rect) const {
  const Sheet* s = S(sheet);
  if (!s) return {};
  const std::vector<Rect> runs = FilterShownRuns(*s, {rect});
  // every row hidden by the filter (a filtered-out cell picked by the Name Box): Excel copies the cells themselves
  // (measured); an empty copy read as "the selection is too large to copy"
  if (runs.empty() || (runs.size() == 1 && runs[0] == rect)) return Copy(sheet, rect);
  ClipData out;
  for (const Rect& run : runs) {
    ClipData part = Copy(sheet, run);
    if (part.rows == 0 || part.cols == 0) continue;
    if (out.rows == 0) {
      out = std::move(part);
      continue;
    }
    if (part.cols != out.cols) continue;  // whole columns clamped differently: keep the first width
    const size_t base = static_cast<size_t>(out.rows) * out.cols;
    for (const Rect& m : part.merges) out.merges.push_back(Rect{m.r0 + out.rows, m.c0, m.r1 + out.rows, m.c1});
    for (auto& [idx, rt] : part.rich) out.rich.emplace_back(base + idx, std::move(rt));
    for (auto& [idx, l] : part.links) out.links.emplace_back(base + idx, std::move(l));
    for (auto& [k, n] : part.notes) out.notes.emplace_back(k + (uint64_t{out.rows} << 16), std::move(n));
    out.cells.insert(out.cells.end(), std::make_move_iterator(part.cells.begin()), std::make_move_iterator(part.cells.end()));
    out.rows += part.rows;
  }
  out.src_sheet = sheet;
  out.src = rect;
  return out;
}

namespace {

// Fill Down / Right from cells a dynamic array spilled into: they fill as blanks (Excel 16, measured: F5 of
// =SEQUENCE(3) in F3, filled down over F6:F7, empties them; B4:D5 filled down through the spill of B3 leaves the spill
// as it is). Copied as values they blocked the array: #SPILL!
void BlankSpilledCells(const model::Sheet& s, const Rect& src, ClipData* clip) {
  if (s.spills->empty() || clip->rows == 0 || clip->cols == 0) return;
  for (uint32_t i = 0; i < clip->rows; ++i) {
    for (uint32_t j = 0; j < clip->cols; ++j) {
      uint32_t ar = 0, ac = 0;
      const uint32_t r = src.r0 + i, c = src.c0 + j;
      const model::SpillRange* sp = s.SpillAt(r, c, &ar, &ac);
      if (!sp || sp->cse || (ar == r && ac == c)) continue;
      ClipCell& cc = clip->cells[static_cast<size_t>(i) * clip->cols + j];
      cc.value = Value();
      cc.formula.reset();
    }
  }
}

}  // namespace

void Document::FillDown(uint32_t sheet, const Rect& rect, bool whole_target) { FillEdge(sheet, rect, 0, whole_target); }

model::CfRule Document::RebasedRule(model::CfRule x, core::CellPos from, core::CellPos to) const {
  auto moved = [&](const std::string& f) -> std::string {
    if (f.empty()) return f;
    std::string_view t = f;
    const bool eq = t[0] == '=';
    if (eq) t.remove_prefix(1);
    formula::ParseResult pr = formula::Parse(t, from, this);
    if (!pr.ok) return f;
    return (eq ? "=" : "") + formula::ToA1(pr.root, to, this);
  };
  x.formula1 = moved(x.formula1);
  x.formula2 = moved(x.formula2);
  for (auto& t : x.icon_thresholds) t.formula = moved(t.formula);
  return x;
}

// Go To Special > Data validation > Same: the cells whose rule has every setting of the one at (r, c), messages too,
// its formulas the same relative to each rule's first cell (Excel 16, measured 2026-11-28: a list "a,b" added to G1:G3
// and to G8 is one rule, "a, b" or another input message isn't; =A6>0 in J6 is J1's =A1>0)
std::vector<Rect> SameValidationAreas(const model::Sheet& sh, uint32_t r, uint32_t c, const formula::SheetResolver* sheets) {
  const model::Sheet* s = &sh;
  const model::DvRule* own = nullptr;
  for (const auto& v : *s->validations) {
    for (const Rect& q : v.ranges) {
      if (q.Contains(r, c)) own = &v;
    }
  }
  if (!own || own->ranges.empty()) return {};
  auto moved = [&](const std::string& f, core::CellPos from, core::CellPos to) -> std::string {
    if (f.empty() || from == to) return f;
    std::string_view t = f;
    const bool eq = t[0] == '=';
    if (!eq) return f;  // a list typed into the rule, a number
    formula::ParseResult pr = formula::Parse(t.substr(1), from, sheets);
    return pr.ok ? "=" + formula::ToA1(pr.root, to, sheets) : f;
  };
  model::DvRule mine = *own;
  mine.ranges.clear();
  const core::CellPos at{own->ranges.front().r0, own->ranges.front().c0};
  std::vector<Rect> out;
  for (const auto& v : *s->validations) {
    if (v.ranges.empty()) continue;
    if (&v != own) {
      model::DvRule x = v;
      x.ranges.clear();
      const core::CellPos base{v.ranges.front().r0, v.ranges.front().c0};
      x.formula1 = moved(x.formula1, base, at);
      x.formula2 = moved(x.formula2, base, at);
      if (!(x == mine)) continue;
    }
    out.insert(out.end(), v.ranges.begin(), v.ranges.end());
  }
  return out;
}

void Document::FillEdge(uint32_t sheet, const Rect& rect_in, int dir, bool whole_target) {
  if (RefuseReadOnly(sheet)) return;  // once, not once per row (and no Change event for the first row)
  const Sheet* s0 = S(sheet);
  if (!s0) return;
  // a whole column from Python ends at the end marker: stop at the sheet's last row (a fill past it grew the sheet
  // to 689 million rows and ran out of memory)
  Rect rect = rect_in;
  rect.r1 = std::min(rect.r1, s0->row_limit - 1);
  rect.c1 = std::min(rect.c1, core::kMaxCols - 1);
  if (rect.r0 > rect.r1 || rect.c0 > rect.c1) return;
  const bool vertical = dir == 0 || dir == 2;
  if (vertical ? rect.r0 == rect.r1 : rect.c0 == rect.c1) return;
  const Rect edge = dir == 0   ? Rect{rect.r0, rect.c0, rect.r0, rect.c1}
                    : dir == 2 ? Rect{rect.r1, rect.c0, rect.r1, rect.c1}
                    : dir == 1 ? Rect{rect.r0, rect.c0, rect.r1, rect.c0}
                               : Rect{rect.r0, rect.c1, rect.r1, rect.c1};
  ClipData line = Copy(sheet, edge);
  if (line.rows == 0 || line.cols == 0) return;
  BlankSpilledCells(*s0, edge, &line);
  // the cells the line goes into
  const Rect dest = dir == 0   ? Rect{rect.r0 + 1, rect.c0, rect.r1, rect.c1}
                    : dir == 2 ? Rect{rect.r0, rect.c0, rect.r1 - 1, rect.c1}
                    : dir == 1 ? Rect{rect.r0, rect.c0 + 1, rect.r1, rect.c1}
                               : Rect{rect.r0, rect.c0, rect.r1, rect.c1 - 1};
  // the target asked once for all of it: a locked cell in it leaves every cell as it was (Excel 16, measured
  // 2026-11-20: FillRight over A1:E1 with B:D unlocked raises and writes nothing); the cells were refused one by one
  // and the unlocked ones filled
  if (Refuse(sheet, &dest)) return;
  // a fill down (right) past a table's last row (column) grows the table, as a write there does (Excel 16, measured
  // 2026-11-20: FillDown over B6:B10 grows A1:D6 to A1:D10, its calculated column with it); it stayed
  if ((dir == 0 || dir == 1) && !s0->tables->empty()) {
    // a fill down inside a table's columns that reaches past its last row makes room for it where the rows below hold
    // something: from the first such row to the fill's end, table rows are inserted (the cells below its whole width
    // move down), and the empty rows before it are taken in (Excel 16, measured: tblfill_down_ cases; a fill wider than
    // the table, AutoFill, Fill Right and a write there write over the cells instead)
    if (dir == 0) {
      std::vector<Rect> room;
      for (const auto& tb : *s0->tables) {
        if (tb.totals || tb.range.r1 >= dest.r1 || tb.range.r1 + 1 < dest.r0 || dest.c0 < tb.range.c0 || dest.c1 > tb.range.c1) continue;
        const Rect band{tb.range.r1 + 1, tb.range.c0, dest.r1, tb.range.c1};
        uint32_t first = UINT32_MAX;
        ForEachCellInTiles(s0->values, band, [&](uint32_t r, uint32_t, const model::ValueTile& t, uint32_t idx) {
          if (t.Occupied(idx)) first = std::min(first, r);
        });
        ForEachCellInTiles(s0->meta, band, [&](uint32_t r, uint32_t, const model::MetaTile& t, uint32_t idx) {
          if (t.formula[idx]) first = std::min(first, r);
        });
        if (first != UINT32_MAX) room.push_back(Rect{first, tb.range.c0, dest.r1, tb.range.c1});
      }
      for (const Rect& r : room) {
        if (!InsertCells(sheet, r, /*shift_down=*/true)) return;
      }
      s0 = S(sheet);
      if (!s0) return;
      if (!room.empty()) {  // the line as the insertion left it (its references to the moved cells moved)
        line = Copy(sheet, edge);
        BlankSpilledCells(*s0, edge, &line);
      }
    }
    std::vector<Rect> grow;
    for (const auto& tb : *s0->tables) {
      if (dir == 0 && tb.range.r1 + 1 >= dest.r0 && tb.range.r1 + 1 <= dest.r1 && tb.range.c0 <= dest.c1 && dest.c0 <= tb.range.c1)
        grow.push_back(Rect{tb.range.r1 + 1, std::max(dest.c0, tb.range.c0), dest.r1, dest.c1});
      if (dir == 1 && tb.range.c1 + 1 >= dest.c0 && tb.range.c1 + 1 <= dest.c1 && tb.range.r0 <= dest.r1 && dest.r0 <= tb.range.r1)
        grow.push_back(Rect{std::max(dest.r0, tb.range.r0), tb.range.c1 + 1, dest.r1, dest.c1});
    }
    for (const Rect& g : grow) AutoExpandTable(sheet, g);
    s0 = S(sheet);  // the growth changed the sheet
    if (!s0) return;
  }
  // what the fill writes: the line's cells that hold something, once a line. Fill Right then Fill Down over the whole
  // sheet with a value in its last cell wrote 17 billion values (basics seq297242: a freeze); past 20,000,000, as a
  // copy that large, it is refused (formats alone go to the lines' own styles and cost nothing)
  {
    uint64_t filled = 0;
    for (const ClipCell& cc : line.cells) filled += cc.formula || !cc.value.IsEmpty() ? 1 : 0;
    if (filled * (vertical ? dest.Rows() : dest.Cols()) > 20'000'000) {
      CountBlock(BlockReason::TooLarge);
      block_too_large_ = true;
      block_sheet_ = sheet;
      block_cells_ = rect;
      block_range_ = -1;
      block_python_ = python_call;
      return;
    }
  }
  // a fill takes no notes (only the clipboard's copy carries them; its validation comes after, FillValidations);
  // merged cells in the line are pasted a line at a time, as is a protected sheet's fill (refused cell by cell)
  // merged cells in the line go the bulk way too (their copies are added after it in one go): a paste a line, each
  // walking every merged area for those under it while adding one, was a million squared steps down a whole column
  // (doc_fuzz --wide seed 24120027: a step that never ended)
  const bool simple = (!line.carry_notes || (line.notes.empty() && line.validations.empty())) && !s0->protection->on;
  if (!simple || dest.Count() <= 100'000) {
    // a paste per line, as Excel's fill copies the line with everything it holds. A table's headers are made text
    // once after the lines, not after each: a paste per column of a table as wide as the sheet synced all 16,384
    // headers at every one (basics seq308046: Ctrl+R froze YCGrid)
    const bool was = syncing_tables_;
    syncing_tables_ = true;
    if (vertical) {
      for (uint32_t r = dest.r0; r <= dest.r1; ++r) Paste(sheet, r, rect.c0, line);
    } else {
      for (uint32_t c = dest.c0; c <= dest.c1; ++c) Paste(sheet, rect.r0, c, line);
    }
    syncing_tables_ = was;
    if (!was) SyncTableHeaders(sheet);
  } else {
    FillLinesInBulk(sheet, edge, dest, vertical, line);
    if (!line.merges.empty()) {
      // the line's merged cells repeat on every line filled, as a paste a line made them (none over a table's cells)
      Sheet* ms = M(sheet);
      const auto& tabs = *ms->tables;
      std::vector<Rect> made;
      const uint64_t lines = vertical ? dest.Rows() : dest.Cols();
      made.reserve(static_cast<size_t>(lines * line.merges.size()));
      for (uint64_t k = 0; k < lines; ++k) {
        for (const Rect& m : line.merges) {
          const Rect x = vertical ? Rect{static_cast<uint32_t>(dest.r0 + k) + m.r0, edge.c0 + m.c0, static_cast<uint32_t>(dest.r0 + k) + m.r1, edge.c0 + m.c1}
                                  : Rect{edge.r0 + m.r0, static_cast<uint32_t>(dest.c0 + k) + m.c0, edge.r0 + m.r1, static_cast<uint32_t>(dest.c0 + k) + m.c1};
          if (std::none_of(tabs.begin(), tabs.end(), [&](const model::Table& tb) { return tb.range.Intersects(x); })) made.push_back(x);
        }
      }
      if (!made.empty()) {
        NoteTouched();
        NoteFormat(sheet, dest);
        auto& mg = M(sheet)->merges.mut();
        mg.insert(mg.end(), made.begin(), made.end());
      }
    }
  }
  FillValidations(sheet, edge, dest, vertical, dir == 2 || dir == 3);
  if (whole_target) MarkChanged(sheet, edge, kSrcUser);
}

// what pasting `line` (one row going down / up, one column going sideways) over every line of dest does, a source
// cell at a time: its content with FillFrom (one shared formula, millions of cells), blanks cleared in one go, its style
// set on the cells that don't have it. The whole sheet's Ctrl+D took no time where a paste per row took minutes
void Document::FillLinesInBulk(uint32_t sheet, const Rect& edge, const Rect& dest, bool vertical, const ClipData& line) {
  if (Refuse(sheet, &dest)) return;
  if (dest.Count() > 1'000'000 && txn_depth_ > 0) pending_undo_.structural = true;  // undone with the whole version
  EnsureRowLimit(sheet, dest.r1);
  Sheet* s = M(sheet);
  if (!s) return;
  {
    auto& m = s->merges.mut();  // merged areas under the fill go, as under a paste
    m.erase(std::remove_if(m.begin(), m.end(), [&](const Rect& x) { return x.Intersects(dest); }), m.end());
  }
  EraseRich(sheet, dest);
  if (!M(sheet)->cond_formats->empty()) ClearCondFormats(sheet, dest);
  const uint32_t n = vertical ? line.cols : line.rows;
  // the part of dest a source cell goes into
  auto lane = [&](uint32_t k) {
    return vertical ? Rect{dest.r0, edge.c0 + k, dest.r1, edge.c0 + k} : Rect{edge.r0 + k, dest.c0, edge.r0 + k, dest.c1};
  };
  // contents: a run of blank source cells clears its lanes at once
  for (uint32_t k = 0; k < n;) {
    const ClipCell& cc = line.cells[k];
    if (cc.formula || !cc.value.IsEmpty()) {
      FillFrom(sheet, vertical ? edge.r0 : edge.r0 + k, vertical ? edge.c0 + k : edge.c0, {lane(k)});
      ++k;
      continue;
    }
    uint32_t e = k;
    while (e + 1 < n && !line.cells[e + 1].formula && line.cells[e + 1].value.IsEmpty()) ++e;
    const Rect a = lane(k), b = lane(e);
    ClearContents(sheet, Rect{std::min(a.r0, b.r0), std::min(a.c0, b.c0), std::max(a.r1, b.r1), std::max(a.c1, b.c1)});
    k = e + 1;
  }
  // fonts inside the texts go with the cells, as a paste of values and formats takes them
  if (!line.rich.empty()) {
    NoteRegion(sheet, dest);
    auto& rich = M(sheet)->rich.mut();
    for (const auto& [idx, rt] : line.rich) {
      const Rect a = lane(static_cast<uint32_t>(idx));
      for (uint32_t r = a.r0; r <= a.r1; ++r) {
        for (uint32_t c = a.c0; c <= a.c1; ++c) rich[(uint64_t{r} << 16) | c] = rt;
      }
    }
  }
  // hyperlinks go with the cells too, as a paste takes them (Excel 16, measured: FillDown)
  EraseLinksIn(sheet, dest);
  if (!line.links.empty()) {
    std::vector<std::pair<uint64_t, model::Hyperlink>> put;
    for (const auto& [idx, l] : line.links) {
      const Rect a = lane(static_cast<uint32_t>(idx));
      for (uint32_t r = a.r0; r <= a.r1 && put.size() < kMaxSheetLinks; ++r) {
        for (uint32_t c = a.c0; c <= a.c1 && put.size() < kMaxSheetLinks; ++c) put.emplace_back((uint64_t{r} << 16) | c, l);
      }
    }
    std::sort(put.begin(), put.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    PutLinks(sheet, put);
  }
  // the copied cells' conditional formats cover their lanes: a rule that looks the same as one already on the sheet
  // grows that one, as a paste per line grows it a line at a time (a million times over a whole column)
  for (const ClipData::Rule& cr : line.rules) {
    model::CfRule nr = cr.rule;
    for (Rect& x : nr.ranges) {
      x = vertical ? Rect{dest.r0, edge.c0 + x.c0, dest.r1, edge.c0 + x.c1} : Rect{edge.r0 + x.r0, dest.c0, edge.r0 + x.r1, dest.c1};
    }
    if (nr.ranges.empty()) continue;
    const core::CellPos nb{nr.ranges.front().r0, nr.ranges.front().c0};
    nr = RebasedRule(nr, cr.base, nb);
    bool grown = false;
    Sheet* ws = M(sheet);
    for (const model::CfRule& ex : *ws->cond_formats) {
      if (ex.ranges.empty()) continue;
      const core::CellPos eb{ex.ranges.front().r0, ex.ranges.front().c0};
      if (!model::SameRuleLook(RebasedRule(nr, nb, eb), RebasedRule(ex, eb, eb))) continue;
      const uint32_t eid = ex.id;
      NoteTouched();
      for (auto& y : ws->cond_formats.mut()) {
        if (y.id == eid) y.ranges.insert(y.ranges.end(), nr.ranges.begin(), nr.ranges.end());
      }
      grown = true;
      break;
    }
    if (!grown) {
      nr.id = 0;
      AddCondFormat(sheet, std::move(nr));
    }
  }
  // styles: each source cell's on its lane. A line that is a whole row (column) with a row (column) style gives that
  // style to the filled rows (columns), so only the cells styled otherwise need one of their own
  s = M(sheet);
  auto& table = wb_.styles.mut();
  uint32_t line_style = 0;
  bool by_lines = false;
  if (vertical && dest.c0 == 0 && dest.c1 == core::kMaxCols - 1) {
    if (auto it = s->row_styles->find(edge.r0); it != s->row_styles->end()) {
      line_style = it->second;
      auto& rs = s->row_styles.mut();
      for (uint32_t r = dest.r0; r <= dest.r1; ++r) rs[r] = line_style;
      NoteDims(sheet, true, dest.r0, dest.r1);
      by_lines = true;
    }
  } else if (!vertical && dest.r0 == 0 && dest.r1 == s->row_limit - 1) {
    if (auto it = s->col_styles->find(edge.c0); it != s->col_styles->end()) {
      line_style = it->second;
      auto& cs = s->col_styles.mut();
      for (uint32_t c = dest.c0; c <= dest.c1; ++c) cs[c] = line_style;
      NoteDims(sheet, false, dest.c0, dest.c1);
      by_lines = true;
    }
  }
  // a fill that covers whole rows from column A (whole columns from row 1): every cell of those lines is the source's
  // or the fill's, so each line takes the source cell's style as its own line style and only the cells styled
  // otherwise are written (Ctrl+R over whole rows whose column A had borders wrote 17 billion cells, basics seq283522)
  s = M(sheet);
  const bool whole_rows = !vertical && !by_lines && edge.c0 == 0 && dest.c1 >= s->col_limit - 1;
  const bool whole_cols = vertical && !by_lines && edge.r0 == 0 && dest.r1 >= s->row_limit - 1;
  if ((whole_rows || whole_cols) && n > 0) {
    NoteDims(sheet, whole_rows, whole_rows ? edge.r0 : edge.c0, (whole_rows ? edge.r0 : edge.c0) + n - 1);
    auto& lines = whole_rows ? s->row_styles.mut() : s->col_styles.mut();
    for (uint32_t k = 0; k < n; ++k) {
      const ClipCell& cc = line.cells[k];
      const uint32_t id = table.Intern(cc.has_style ? cc.style : Style{});
      const uint32_t at = (whole_rows ? edge.r0 : edge.c0) + k;
      if (id) lines[at] = id;
      else lines.erase(at);
    }
  }
  // the rows past the used range show column A's own format there (the line was cut to the used range): every column
  // takes it, and the few rows above the fill are given back what they showed. B500 stayed plain under rows 3:1048576
  // filled right from a column A formatted whole
  if (whole_rows && dest.r1 >= s->row_limit - 1 && dest.r0 <= kFewRowsLeft) {
    const auto& cs0 = *s->col_styles;
    const auto ai = cs0.find(edge.c0);
    const uint32_t sa = ai != cs0.end() ? ai->second : 0;
    bool differs = false;
    for (uint32_t c = dest.c0; c <= dest.c1 && c < s->col_limit && !differs; ++c) {
      const auto ci = cs0.find(c);
      differs = (ci != cs0.end() ? ci->second : 0) != sa;
    }
    if (differs) {
      const uint32_t c1 = std::min(dest.c1, s->col_limit - 1);
      std::vector<std::vector<uint32_t>> shown(dest.r0);  // rows above: what each of their cells showed
      for (uint32_t r = 0; r < dest.r0; ++r) {
        shown[r].resize(c1 - dest.c0 + 1);
        for (uint32_t c = dest.c0; c <= c1; ++c) shown[r][c - dest.c0] = s->GetStyle(r, c);
      }
      auto& cm = s->col_styles.mut();
      for (uint32_t c = dest.c0; c <= c1; ++c) {
        if (sa) cm[c] = sa;
        else cm.erase(c);
      }
      NoteDims(sheet, false, dest.c0, c1);
      for (uint32_t r = 0; r < dest.r0; ++r) {
        for (uint32_t c = dest.c0; c <= c1; ++c) s->ShowStyle(r, c, shown[r][c - dest.c0]);
      }
    }
  }
  // each cell is written only where it doesn't show the style already: a cell shows its own style, else its row's,
  // else its column's (a whole sheet in italics with a formula in XFD1048576 wrote 17 billion cells)
  s = M(sheet);
  const auto& rs = *s->row_styles;
  const auto& cs = *s->col_styles;
  auto inherited = [&](uint32_t r, uint32_t c) -> uint32_t {
    if (auto it = rs.find(r); it != rs.end()) return it->second;
    if (auto it = cs.find(c); it != cs.end()) return it->second;
    return 0;
  };
  std::unordered_map<uint32_t, std::vector<uint32_t>> other_cols;  // style -> the lane's columns whose style isn't it
  for (uint32_t k = 0; k < n; ++k) {
    const ClipCell& cc = line.cells[k];
    const uint32_t id = table.Intern(cc.has_style ? cc.style : Style{});
    const Rect a = lane(k);
    // the cells with a style of their own take the copied one (a paste writes it where it differs)
    std::vector<std::pair<uint32_t, uint32_t>> own;
    ForEachCellInTiles(s->meta, a, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (t.style[idx] && t.style[idx] != id) own.emplace_back(r, c);
    });
    for (auto [r, c] : own) s->ShowStyle(r, c, id);
    // the others: where what they show isn't the copied style (the default under a row's or column's formats is the
    // cell's own Normal, as in Excel)
    auto fix = [&](uint32_t r, uint32_t c) {
      if (!s->GetOwnStyle(r, c) && inherited(r, c) != id) s->ShowStyle(r, c, id);
    };
    if (vertical) {
      const uint32_t c = a.c0;
      const auto ci = cs.find(c);
      const uint32_t base = by_lines ? line_style : ci != cs.end() ? ci->second : 0;
      if (base != id) {
        for (uint32_t r = a.r0; r <= a.r1; ++r) fix(r, c);
      } else if (!by_lines) {
        for (auto it = rs.lower_bound(a.r0); it != rs.end() && it->first <= a.r1; ++it) fix(it->first, c);
      }
    } else {
      const uint32_t r = a.r0;
      if (auto ri = rs.find(r); ri != rs.end()) {
        if (ri->second != id) {
          for (uint32_t c = a.c0; c <= a.c1; ++c) fix(r, c);
        }
      } else {
        auto [it, fresh] = other_cols.try_emplace(id);
        if (fresh) {
          for (uint32_t c = a.c0; c <= a.c1; ++c) {
            const auto ci = cs.find(c);
            if ((ci != cs.end() ? ci->second : 0) != id) it->second.push_back(c);
          }
        }
        for (const uint32_t c : it->second) fix(r, c);
      }
    }
  }
  NoteFormat(sheet, dest);
  // the used range grows by the lanes filled, not by dest: a whole sheet filled down from a row of four cells made
  // the whole sheet used, and Ctrl+C of it then refused 17 billion cells (basics seed 2390). The line was copied cut
  // to the used range, so its n lanes are all the fill wrote
  if (n > 0) {
    const Rect filled = vertical ? Rect{dest.r0, edge.c0, dest.r1, edge.c0 + n - 1} : Rect{edge.r0, dest.c0, edge.r0 + n - 1, dest.c1};
    s->Touch(filled.r0, filled.c0);
    s->Touch(filled.r1, filled.c1);
  }
  MarkChanged(sheet, dest, kSrcUser);
}

void Document::FillFrom(uint32_t sheet, uint32_t r, uint32_t c, const std::vector<Rect>& areas) {
  const ClipData one = Copy(sheet, Rect::Cell(r, c));
  if (one.rows != 1 || one.cols != 1) return;
  const ClipCell& cell = one.cells[0];
  const auto* f = cell.formula ? static_cast<const formula::CompiledFormula*>(cell.formula.get()) : nullptr;
  // Ctrl+Enter beside a table whose growth waits (defer_table_growth): the cells filled are the entry's too
  if (defer_table_growth && S(sheet) && !S(sheet)->tables->empty()) {
    for (const Rect& a : areas) {
      if (deferred_growth_.size() < 4096) deferred_growth_.emplace_back(sheet, a);
    }
  }
  uint64_t total = 0;
  for (const Rect& a : areas) total += a.Count();
  // undone with the whole version: undoing millions of cells one region at a time took the screen for a second
  if (total > 1'000'000 && txn_depth_ > 0) pending_undo_.structural = true;
  for (const Rect& want : areas) {
    if (Refuse(sheet, &want)) continue;
    EnsureRowLimit(sheet, want.r1);
    Sheet* s = M(sheet);
    if (!s) return;
    const Rect a{want.r0, want.c0, std::min<uint32_t>(want.r1, s->row_limit - 1), std::min<uint32_t>(want.c1, core::kMaxCols - 1)};
    // where every relative reference stays on the sheet the formula is the same (its R1C1 key): one id for all
    int64_t rlo = 0, rhi = int64_t{s->row_limit} - 1, clo = 0, chi = int64_t{core::kMaxCols} - 1;
    uint32_t shared = 0;
    if (f) {
      formula::Node probe = f->root;
      formula::ForEachRef(probe, [&](formula::RefSpec& ref) {
        if (!ref.HasCells()) return;  // #REF!A1 moves off the sheet as any reference (FixOutOfBounds)
        auto rows = [&](const formula::RefCoord& k) {
          if (!k.abs) rlo = std::max(rlo, -k.v), rhi = std::min(rhi, int64_t{s->row_limit} - 1 - k.v);
        };
        auto cols = [&](const formula::RefCoord& k) {
          if (!k.abs) clo = std::max(clo, -k.v), chi = std::min(chi, int64_t{core::kMaxCols} - 1 - k.v);
        };
        if (!ref.whole_cols) rows(ref.r0), ref.is_range ? rows(ref.r1) : void();
        if (!ref.whole_rows) cols(ref.c0), ref.is_range ? cols(ref.c1) : void();
      });
      auto it = formula_ids_.find(f->key);
      shared = it != formula_ids_.end() ? it->second : InternFormula(f->root);
    }
    for (uint32_t dc = a.c0; dc <= a.c1; ++dc) {
      for (uint32_t dr = a.r0; dr <= a.r1; ++dr) {
        if (dr == r && dc == c) continue;
        s = M(sheet);
        if (s->GetFormula(dr, dc)) {
          UnregisterFormulaCell(sheet, dr, dc);
          s->SetFormula(dr, dc, 0);
        }
        if (!f) {
          s->SetValue(dr, dc, cell.value);
          continue;
        }
        uint32_t id = shared;
        if (dr < rlo || dr > rhi || dc < clo || dc > chi) {  // near an edge: references off the sheet become #REF!
          formula::Node copy = f->root;
          if (FixOutOfBounds(copy, core::CellPos{dr, dc}, s->row_limit)) id = InternFormula(std::move(copy));
          s = M(sheet);
        }
        s->SetValue(dr, dc, cell.value);
        s->SetFormula(dr, dc, id);
        RegisterFormulaCell(sheet, dr, dc, id);
        fresh_formulas_.erase(Key(sheet, dr, dc));  // a copy: shows the copied value while a cycle keeps it (see Paste)
        if (!auto_calc) entered_.push_back(Key(sheet, dr, dc));
      }
    }
    s = M(sheet);
    s->Touch(a.r0, a.c0);
    s->Touch(a.r1, a.c1);
    MarkChanged(sheet, a, kSrcUser);
  }
}

void Document::FillRight(uint32_t sheet, const Rect& rect, bool whole_target) { FillEdge(sheet, rect, 1, whole_target); }

namespace {

// the last run of digits in a text counts on, as in Excel: "Item7" -> prefix "Item", number 7, 1 digit; "x1y" ->
// "x", 1, suffix "y" ("Day1 PM" -> Day2 PM). A number past a long's range (Item99999999999) is no series: it repeats
bool SplitTrailingNumber(const std::string& s, std::string* prefix, int64_t* n, size_t* width, std::string* suffix = nullptr) {
  size_t e = s.size();
  if (suffix) {
    while (e > 0 && !std::isdigit(static_cast<unsigned char>(s[e - 1]))) --e;
  }
  size_t i = e;
  while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
  if (i == e || e - i > 10) return false;
  const int64_t v = std::stoll(s.substr(i, e - i));
  if (v > 2147483647) return false;
  *prefix = s.substr(0, i);
  *n = v;
  *width = e - i;
  if (suffix) *suffix = s.substr(e);
  return true;
}

// "Q3", "Qtr3", "Qtr 3", "Quarter 3" (any case): the prefix (with its space) and the quarter 1-4
bool SplitQuarter(const std::string& s, std::string* prefix, int* q) {
  if (s.size() < 2) return false;
  const char last = s.back();
  if (last < '1' || last > '4') return false;
  std::string head = Lower(std::string_view(s).substr(0, s.size() - 1));
  while (!head.empty() && head.back() == ' ') head.pop_back();
  if (head != "q" && head != "qtr" && head != "quarter") return false;
  *prefix = s.substr(0, s.size() - 1);
  *q = last - '0';
  return true;
}

// "1st Period", "2nd", "3rd place" (a leading number with its English ordinal suffix): the number and what follows
bool SplitOrdinal(const std::string& s, int64_t* n, std::string* rest) {
  size_t i = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  if (i == 0 || i > 15 || i + 2 > s.size()) return false;
  const std::string suf = Lower(std::string_view(s).substr(i, 2));
  if (suf != "st" && suf != "nd" && suf != "rd" && suf != "th") return false;
  if (i + 2 < s.size() && std::isalpha(static_cast<unsigned char>(s[i + 2]))) return false;  // "1stop"
  *n = std::stoll(s.substr(0, i));
  *rest = s.substr(i + 2);
  return true;
}

std::string Ordinal(int64_t n) {
  const int64_t t = n % 100, u = n % 10;
  const char* suf = (t >= 11 && t <= 13) ? "th" : u == 1 ? "st" : u == 2 ? "nd" : u == 3 ? "rd" : "th";
  return std::to_string(n) + suf;
}

// the least-squares line through ys at 0, 1, 2...: Excel's trend fill. Evenly spaced values keep the first as the
// base (1, 3 -> 5, 7 exactly); others take the fitted line's (1, 3, 4 -> 5.67, 7.17, 8.67)
void TrendLine(const std::vector<double>& ys, double* base, double* step) {
  const double n = static_cast<double>(ys.size());
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t k = 0; k < ys.size(); ++k) {
    const double x = static_cast<double>(k);
    sx += x;
    sy += ys[k];
    sxx += x * x;
    sxy += x * ys[k];
  }
  *step = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  *base = ys[0];
  double scale = 1, off = 0;
  for (size_t k = 0; k < ys.size(); ++k) {
    scale = std::max(scale, std::fabs(ys[k]));
    off = std::max(off, std::fabs(ys[k] - (ys[0] + *step * static_cast<double>(k))));
  }
  if (off > 1e-9 * scale) *base = (sy - *step * sx) / n;
}

// a lane of two or more cells that are all one kind of text series (one list's items, quarters after one prefix,
// ordinals before one text, one text before a number) continues by the step between them, as in Excel: Jan, Feb ->
// Mar, Apr; Jan, Apr -> Jul, Oct; Item 1, Item 3 -> Item 5. A lane of mixed kinds repeats, each cell counting on
// by one (text1, textA -> text2, textA)
struct TextSeries {
  int kind = 0;                     // 0 none, 1 list item, 2 quarter, 3 ordinal, 4 trailing number
  std::vector<std::string> items;   // the list
  std::string affix;                // the quarter's or trailing number's prefix, the ordinal's rest
  std::string suffix;               // what follows the number (x1y)
  size_t width = 0;                 // a trailing number keeps its leading zeros to this many digits
  double base = 0, step = 0;        // the position (list index, quarter, number) at the first cell, per cell
};

bool LaneTextSeries(const std::vector<const ClipCell*>& seq, TextSeries* out, std::vector<double>* positions = nullptr) {
  const size_t n = seq.size();
  if (n < 2) return false;
  TextSeries ts;
  std::vector<double> pos(n);
  int list0 = -1;
  int64_t cycle = 0, prev = 0;
  for (size_t k = 0; k < n; ++k) {
    if (seq[k]->formula || !seq[k]->value.IsString()) return false;
    const std::string str(seq[k]->value.str());
    int kind = 0, list = -1, idx = 0, q = 0;
    std::string affix, suffix;
    int64_t num = 0;
    size_t width = 0;
    if (FindInFillLists(str, &list, &idx)) {
      kind = 1;
      num = idx;
    } else if (SplitQuarter(str, &affix, &q)) {
      kind = 2;
      num = q - 1;
    } else if (SplitOrdinal(str, &num, &affix)) {
      kind = 3;
    } else if (SplitTrailingNumber(str, &affix, &num, &width, &suffix)) {
      kind = 4;
    } else {
      return false;
    }
    if (k == 0) {
      ts.kind = kind;
      ts.affix = affix;
      ts.suffix = suffix;
      ts.width = width;
      list0 = list;
      if (kind == 1) ts.items = AllFillLists()[static_cast<size_t>(list)];
      cycle = kind == 1 ? static_cast<int64_t>(ts.items.size()) : kind == 2 ? 4 : 0;
      pos[0] = static_cast<double>(num);
    } else {
      if (kind != ts.kind || list != list0 || affix != ts.affix || suffix != ts.suffix) return false;
      ts.width = std::min(ts.width, width);
      if (cycle > 0) {  // the shorter way round the list: Nov, Dec, Jan count on by one
        int64_t d = ((num - prev) % cycle + cycle) % cycle;
        if (d * 2 > cycle) d -= cycle;
        pos[k] = pos[k - 1] + static_cast<double>(d);
      } else {
        pos[k] = static_cast<double>(num);
      }
    }
    prev = num;
  }
  TrendLine(pos, &ts.base, &ts.step);
  *out = std::move(ts);
  if (positions) *positions = std::move(pos);
  return true;
}

// the series' text at position x (0 = the first source cell)
std::string TextSeriesAt(const TextSeries& ts, double x) {
  const int64_t p = std::llround(ts.base + ts.step * x);
  switch (ts.kind) {
    case 1: {
      const int64_t cycle = static_cast<int64_t>(ts.items.size());
      return ts.items[static_cast<size_t>((p % cycle + cycle) % cycle)];
    }
    case 2: return ts.affix + std::to_string((p % 4 + 4) % 4 + 1);
    case 3: return Ordinal(p < 0 ? -p : p) + ts.affix;
    default: {
      std::string digits = std::to_string(p < 0 ? -p : p);
      if (digits.size() < ts.width) digits.insert(0, ts.width - digits.size(), '0');
      return ts.affix + digits + ts.suffix;
    }
  }
}

}  // namespace

// the first section shows the day of the month (d or dd; ddd and dddd are the weekday)
bool FormatShowsDay(std::string_view code) {
  bool in_q = false;
  int bracket = 0;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i])));
    if (code[i] == '\\') {
      ++i;
      continue;
    }
    if (ch == '"') in_q = !in_q;
    if (in_q) continue;
    if (ch == '[') ++bracket;
    else if (ch == ']') bracket = std::max(0, bracket - 1);
    else if (ch == ';') break;
    else if (!bracket && ch == 'd') {
      size_t run = 1;
      while (i + run < code.size() && std::tolower(static_cast<unsigned char>(code[i + run])) == 'd') ++run;
      if (run <= 2) return true;
      i += run - 1;
    }
  }
  return false;
}

bool IsTimeOnlyFormat(std::string_view code) {
  if (!formula::IsDateFormat(code)) return false;
  bool date = false, time = false, in_q = false;
  int bracket = 0;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i])));
    if (code[i] == '\\') {
      ++i;
      continue;
    }
    if (ch == '"') in_q = !in_q;
    if (in_q) continue;
    if (ch == '[') ++bracket;
    else if (ch == ']') bracket = std::max(0, bracket - 1);
    else if (ch == 'h' || ch == 's') time = true;  // also [h] elapsed hours
    else if (!bracket && (ch == 'y' || ch == 'd' || ch == 'e' || ch == 'g' || ch == 'b')) date = true;
    if (ch == ';') break;  // the first section decides
  }
  return time && !date;
}

bool FillTextSeries(std::string_view text) {
  const std::string str(text);
  int list, idx, q;
  std::string prefix, rest;
  int64_t num;
  size_t width;
  return FindInFillLists(str, &list, &idx) || SplitQuarter(str, &prefix, &q) || SplitOrdinal(str, &num, &rest) ||
         SplitTrailingNumber(str, &prefix, &num, &width, &rest);
}

namespace {

// ---- AutoFill of dates, times, blanks and text series: Excel 16's rules, measured --------------------------------

int64_t FloorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// the date months on as Fill Months moves it: the same day, or the month's last day when the month is shorter (Jan 30
// -> Feb 29, Mar 30); with keep_end the last day of a month stays the last day (Feb 29 -> Mar 31, Apr 30), as Fill
// Months does and Fill Years doesn't (Feb 28, 2023 -> Feb 28, 2024)
double ShiftMonths(double day, int64_t months, bool keep_end) {
  int y = 0, m = 0, d = 0;
  if (!formula::SerialToYmd(day, &y, &m, &d)) return day;
  const int64_t mm = (m - 1) + months;
  const int64_t ny = y + FloorDiv(mm, 12), nm = mm - FloorDiv(mm, 12) * 12 + 1;
  if (ny < 0 || ny > 99999) return day;
  const int last = formula::DaysInMonth(static_cast<int>(ny), static_cast<int>(nm));
  const int nd = keep_end && d == formula::DaysInMonth(y, m) ? last : std::min(d, last);
  return formula::DateSerial(ny, nm, nd);
}

// a seed months on with the first one, counted as a day of the first's month (Mar 1 after Feb 29 is Feb 30, Feb 1
// after Jan 15 is Jan 32) and moved as that day: a day up to 30 the new month lacks is its last day (Feb 30, 2025 is
// Feb 28), a later one runs on into the next month as DATE does (Feb 31, 2025 is Mar 3, Apr 31 is May 1); with
// keep_end the first's month's last day stays the last day (Excel 16, measured)
double ShiftWithFirst(double first, double day, int64_t months, bool keep_end) {
  int y = 0, m = 0, d = 0;
  if (!formula::SerialToYmd(first, &y, &m, &d)) return day;
  const int64_t v = d + static_cast<int64_t>(day - first);
  const int64_t mm = (m - 1) + months;
  const int64_t ny = y + FloorDiv(mm, 12), nm = mm - FloorDiv(mm, 12) * 12 + 1;
  if (ny < 0 || ny > 99999) return day;
  const int last = formula::DaysInMonth(static_cast<int>(ny), static_cast<int>(nm));
  if (keep_end && v == formula::DaysInMonth(y, m)) return formula::DateSerial(ny, nm, last);
  if (v >= 1 && v <= 30) return formula::DateSerial(ny, nm, std::min<int64_t>(v, last));
  return formula::DateSerial(ny, nm, 1) + static_cast<double>(v - 1);
}

// a Saturday or Sunday Fill Weekdays steps onto (by weeks, months or years) moves back to the Friday before it
double WeekdayBack(double day) {
  const int wd = formula::Weekday(day);
  return wd == 7 ? day - 1 : wd == 1 ? day - 2 : day;
}

// count weekdays on (back when negative), weekends skipped: Friday + 1 is Monday, and so is Saturday + 1
double AddWeekdays(double day, int64_t count) {
  if (count == 0) return day;
  const int dirn = count > 0 ? 1 : -1;
  const int wd = formula::Weekday(day);
  if (wd == 7 || wd == 1) day += dirn > 0 ? (wd == 7 ? -1 : -2) : (wd == 7 ? 2 : 1);  // from the Friday / Monday
  int64_t left = count < 0 ? -count : count;
  day += static_cast<double>(dirn * 7 * (left / 5));
  left %= 5;
  while (left > 0) {
    day += dirn;
    const int w = formula::Weekday(day);
    if (w != 1 && w != 7) --left;
  }
  return day;
}

bool Close(double a, double b, double scale) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, scale); }

double ScaleOf(const std::vector<double>& v) {
  double s = 1;
  for (double x : v) s = std::max(s, std::fabs(x));
  return s;
}

// values evenly spaced (two always are): the step between them
// what AutoFill counts a number on as, by its format (Excel 16, measured): a number in General joins numbers in any
// other number format, but numbers (1,000), currency and percentages each in their own format are a series of their
// own ([50%, 1,000] count on 0.5 and 1000 apart, [10%, 20.5%] together), and so are dates and times ([1, 9:30] by 1
// and by an hour, [9:30, 2024/1/1] by an hour and by a day)
enum class FillKind { General, Number, Currency, Percent, Scientific, Date, Time };

FillKind KindOfFormat(std::string_view code) {
  if (code.empty() || code == "General") return FillKind::General;
  if (IsTimeOnlyFormat(code)) return FillKind::Time;
  if (formula::IsDateFormat(code)) return FillKind::Date;
  bool in_q = false, pct = false, sci = false, cur = false, general = false;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (ch == '\\') {
      ++i;
      continue;
    }
    if (ch == '"') {
      in_q = !in_q;
      continue;
    }
    if (ch == ';') break;
    if (in_q) {
      cur = cur || ch == '$';
      continue;
    }
    if (ch == '[') {
      const size_t e = code.find(']', i);
      if (e == std::string_view::npos) break;
      cur = cur || (i + 1 < code.size() && code[i + 1] == '$');
      i = e;
      continue;
    }
    pct = pct || ch == '%';
    sci = sci || ((ch == 'E' || ch == 'e') && i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-'));
    cur = cur || ch == '$';
    general = general || (ch == 'G' && code.substr(i, 7) == "General");
  }
  if (pct) return FillKind::Percent;
  if (sci) return FillKind::Scientific;
  if (cur) return FillKind::Currency;
  return general ? FillKind::General : FillKind::Number;
}

bool SameFillKind(FillKind a, FillKind b) {
  auto plain = [](FillKind k) { return k <= FillKind::Scientific; };
  return a == b || (a == FillKind::General && plain(b)) || (b == FillKind::General && plain(a));
}

// Fill Days, Weekdays, Months, Years and the trends repeat every text but the names Fill Days and Weekdays step (the
// built-in lists of days) and Fill Months steps (of months): Excel 16, measured (Fill Days of Jan repeats it)
bool ListStepsUnder(int list, int type) {
  const bool days = list == 0 || list == 1 || list == 4 || list == 5, months = list == 2 || list == 3 || list == 6;
  return (days && (type == 5 || type == 6)) || (months && type == 7);
}

// the day name `weekdays` weekdays after the one at idx of a list of days (Sunday first): Fri -> Mon
int WeekdayNameAfter(int idx, int64_t weekdays) {
  if (idx < 1 || idx > 5) return static_cast<int>(((idx + weekdays) % 7 + 7) % 7);  // a weekend day: as days
  return 1 + static_cast<int>(((idx - 1 + weekdays) % 5 + 5) % 5);
}

// a time AutoFill counts back past midnight goes round to the day before (1:00, 0:00 -> 23:00), as Excel shows no
// negative time
double WrapTime(double v) { return v < 0 ? v - std::floor(v) : v; }

bool Evenly(const std::vector<double>& v, double* step) {
  if (v.size() < 2) return false;
  *step = v[1] - v[0];
  const double scale = ScaleOf(v);
  for (size_t i = 2; i < v.size(); ++i) {
    if (!Close(v[i] - v[i - 1], *step, scale)) return false;
  }
  return true;
}

// values on one straight line over their positions: the line's value at position 0 and its step a position
bool OnLine(const std::vector<double>& xs, const std::vector<double>& ys, double* a, double* b) {
  if (xs.size() < 2) return false;
  *b = (ys.back() - ys.front()) / (xs.back() - xs.front());
  *a = ys.front() - *b * xs.front();
  const double scale = ScaleOf(ys);
  for (size_t i = 0; i < xs.size(); ++i) {
    if (!Close(ys[i], *a + *b * xs[i], scale)) return false;
  }
  return true;
}

// the least-squares line through the points: its value at position 0 and its step a position
void FitLine(const std::vector<double>& xs, const std::vector<double>& ys, double* a, double* b) {
  const double n = static_cast<double>(xs.size());
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < xs.size(); ++i) {
    sx += xs[i];
    sy += ys[i];
    sxx += xs[i] * xs[i];
    sxy += xs[i] * ys[i];
  }
  *b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  *a = (sy - *b * sx) / n;
}

// dates a whole number of months apart, on one day of the month or all on their month's last day, at one time of day:
// the step in months (0 when they aren't, or are all in one month)
int64_t MonthStep(const std::vector<double>& v, bool* ends) {
  int y0 = 0, m0 = 0, d0 = 0;
  if (v.size() < 2 || !formula::SerialToYmd(std::floor(v[0]), &y0, &m0, &d0)) return 0;
  const double frac = v[0] - std::floor(v[0]);
  bool same_day = true, all_ends = d0 == formula::DaysInMonth(y0, m0);
  int64_t prev = int64_t{y0} * 12 + m0, step = 0;
  for (size_t i = 1; i < v.size(); ++i) {
    int y = 0, m = 0, d = 0;
    if (!formula::SerialToYmd(std::floor(v[i]), &y, &m, &d) || v[i] - std::floor(v[i]) != frac) return 0;
    same_day = same_day && d == d0;
    all_ends = all_ends && d == formula::DaysInMonth(y, m);
    if (!same_day && !all_ends) return 0;
    const int64_t cur = int64_t{y} * 12 + m;
    if (i == 1) step = cur - prev;
    else if (cur - prev != step) return 0;
    prev = cur;
  }
  *ends = all_ends && !same_day;
  return step;
}

// three or more dates, each in a later or earlier month than the one before on the same day or a month end, that
// MonthStep doesn't take as one series (Jan 31, Feb 29, Mar 29; Jun 30, Jul 30, Sep 30): every fill repeats them
bool MonthlyPairs(const std::vector<double>& v) {
  if (v.size() < 3) return false;
  const double frac = v[0] - std::floor(v[0]);
  for (size_t i = 1; i < v.size(); ++i) {
    int y1 = 0, m1 = 0, d1 = 0, y2 = 0, m2 = 0, d2 = 0;
    if (!formula::SerialToYmd(std::floor(v[i - 1]), &y1, &m1, &d1) || !formula::SerialToYmd(std::floor(v[i]), &y2, &m2, &d2) ||
        v[i] - std::floor(v[i]) != frac || (y1 == y2 && m1 == m2)) {
      return false;
    }
    if (d1 != d2 && (d1 != formula::DaysInMonth(y1, m1) || d2 != formula::DaysInMonth(y2, m2))) return false;
  }
  return true;
}

// What AutoFill writes into one lane past its source, worked out once for the lane: out[t - n] for the lane's cell t
// (t >= n counted from the source's first cell, or back from its last going up or left). Excel 16, measured:
//  - Blank cells repeat, and the cells around them form one series: 1, blank, 5 go on 9, blank, 13, 17 (the numbers
//    by their order) when the numbers are evenly spaced in value and in place; on the line through their places when
//    only that is straight (1, 2, blank, 4 -> 5, 6, blank, 8); otherwise numbers follow the best-fit line over the
//    places, blank places too, and dates, times and text repeat as they are.
//  - Dates: two or more on one day of the month go on by months (or years), others by their step in days; three or
//    more unevenly spaced repeat, and so do three or more each whole months after the one before but without one
//    step (Jan 31, Feb 29, Mar 29), whatever the fill. Fill Days, Weekdays, Months and Years drop the time of day.
//    Seeds that aren't a series in that unit move on a unit a round (Jan 1, Jan 2 -> Feb 1, Feb 2): the first one by
//    the unit, the others as days of the first's month moved with it (Feb 1 after Jan 15 is Jan 32 and becomes Feb
//    32, Mar 3; Mar 1 after Feb 29 is Feb 30 and becomes Feb 28, 2025; Jan 31 after Jan 1 becomes Feb 29). Fill
//    Years of month ends keeps them month ends (Jan 31, Feb 28, 2023 -> Feb 29, 2024). Fill Weekdays steps dates a day
//    apart by weekdays, dates two to seven days apart by weeks a round, others by months, and moves a weekend it
//    lands on back to Friday. Linear and growth trends of dates and times repeat them.
//  - A time steps by an hour; Fill Days and the like repeat times.
//  - Text series (Item1, Jan, Q1) must be evenly spaced, or they repeat (Jan, Feb, Apr -> Jan, Feb, Apr).
// Lanes with formulas, of mixed kinds (numbers and text) and of plain numbers without blanks are left to the cell by
// cell rules (false).
bool PlanFillLane(const std::vector<const ClipCell*>& seq, std::string_view fmt, int type, uint32_t total, bool back,
                  std::vector<Value>* out) {
  const int64_t n = static_cast<int64_t>(seq.size());
  if (n == 0 || type == 1 || type == 3) return false;
  std::vector<int64_t> item_of(seq.size(), -1);
  std::vector<double> pos;  // the places of the cells that aren't blank
  bool nums = true, texts = true;
  for (int64_t k = 0; k < n; ++k) {
    const ClipCell& cc = *seq[k];
    if (cc.formula) return false;
    if (cc.value.IsEmpty()) continue;
    item_of[k] = static_cast<int64_t>(pos.size());
    pos.push_back(static_cast<double>(k));
    nums = nums && cc.value.IsNumber();
    texts = texts && cc.value.IsString();
  }
  const int64_t m = static_cast<int64_t>(pos.size());
  if (m == 0 || (!nums && !texts)) return false;
  const bool blanks = m != n;
  const bool inner = static_cast<int64_t>(pos.back() - pos.front()) + 1 != m;  // blanks between the items
  double pos_step = 0;
  const bool pos_even = m < 2 || Evenly(pos, &pos_step);
  std::vector<double> ys;
  for (double p : pos) ys.push_back(seq[static_cast<size_t>(p)]->value.IsNumber() ? seq[static_cast<size_t>(p)]->value.number() : 0);

  // fill every cell past the source with f(x, k, r, i): x the place counted from the first source cell, k the
  // source cell it repeats, r the round (1 the first one past the source, -1 going back), i k's item (-1 blank)
  auto emit = [&](auto&& f) {
    out->assign(total > n ? static_cast<size_t>(total - n) : 0, Value{});
    for (int64_t t = n; t < static_cast<int64_t>(total); ++t) {
      const int64_t x = back ? -(t - n + 1) : t;
      const int64_t r = FloorDiv(x, n), k = x - r * n;
      (*out)[static_cast<size_t>(t - n)] = f(x, k, r, item_of[static_cast<size_t>(k)]);
    }
    return true;
  };
  auto repeat = [&]() {
    return emit([&](int64_t, int64_t k, int64_t, int64_t) { return seq[static_cast<size_t>(k)]->value; });
  };
  // the items as one series by their order (g counts the items, the source's first 0), blanks repeating
  auto by_order = [&](auto&& at) {
    return emit([&](int64_t, int64_t k, int64_t r, int64_t i) {
      return i < 0 ? seq[static_cast<size_t>(k)]->value : at(r * m + i);
    });
  };
  // the items on the line through their places, blanks repeating
  auto by_place = [&](auto&& at) {
    return emit([&](int64_t x, int64_t k, int64_t, int64_t i) { return i < 0 ? seq[static_cast<size_t>(k)]->value : at(x); });
  };
  // evenly spaced by order, with blanks only around them or in an even pattern: the series by order
  double step = 0;
  const bool even = m >= 2 && Evenly(ys, &step);
  const bool order_ok = !inner || pos_even;

  if (texts && type >= 5) {
    std::vector<int> idxs;
    int list0 = -1;
    for (double p : pos) {
      int list = -1, idx = 0;
      // texts of several kinds: cell by cell (Fill Days of Mon, Feb steps Mon and repeats Feb)
      if (!FindInFillLists(std::string(seq[static_cast<size_t>(p)]->value.str()), &list, &idx) || (list0 >= 0 && list != list0)) return false;
      list0 = list;
      idxs.push_back(idx);
    }
    if (!ListStepsUnder(list0, type)) return repeat();
    const bool weekdays = type == 6 && std::all_of(idxs.begin(), idxs.end(), [](int i) { return i >= 1 && i <= 5; });
    if (weekdays) {  // Mon, Wed -> Fri, Tue, Thu: counted in weekdays
      std::vector<double> wk;
      for (int i : idxs) wk.push_back(i - 1);
      double wstep = 1;
      if (m >= 2 && !Evenly(wk, &wstep)) return repeat();
      const FillList L = AllFillLists()[static_cast<size_t>(list0)];
      return by_order([&](int64_t g) {
        const int64_t w = static_cast<int64_t>(std::llround(wk[0] + wstep * static_cast<double>(g)));
        return Value::String(L[static_cast<size_t>(1 + ((w % 5) + 5) % 5)]);
      });
    }
  }
  if (texts) {
    if (m < 2) return false;  // one text counts on by one a round, cell by cell
    std::vector<const ClipCell*> items;
    for (double p : pos) items.push_back(seq[static_cast<size_t>(p)]);
    TextSeries ts;
    std::vector<double> tpos;
    if (!LaneTextSeries(items, &ts, &tpos)) return false;  // not one kind of series: cell by cell
    double tstep = 0, a = 0, b = 0;
    if (Evenly(tpos, &tstep) && order_ok) {
      ts.base = tpos[0];
      ts.step = tstep;
      return by_order([&](int64_t g) { return Value::String(TextSeriesAt(ts, static_cast<double>(g))); });
    }
    if (inner && OnLine(pos, tpos, &a, &b)) {
      ts.base = a;
      ts.step = b;
      return by_place([&](int64_t x) { return Value::String(TextSeriesAt(ts, static_cast<double>(x))); });
    }
    return repeat();
  }

  const bool is_time = IsTimeOnlyFormat(fmt);
  const bool is_date = !is_time && formula::IsDateFormat(fmt);
  const double y0 = ys[0];
  double a = 0, b = 0;
  if (!is_date && !is_time) {  // plain numbers with blanks among them
    if (!blanks) return false;
    if (type >= 5 && type <= 8) return repeat();  // days, months... of numbers repeat them
    if ((type == 9 || type == 10) && m >= 2) {    // a trend over the places, blank ones too
      if (type == 10) {
        std::vector<double> logs;
        for (double y : ys) {
          if (y <= 0) return repeat();
          logs.push_back(std::log(y));
        }
        FitLine(pos, logs, &a, &b);
        return emit([&](int64_t x, int64_t, int64_t, int64_t) { return Value::Number(std::exp(a + b * static_cast<double>(x))); });
      }
      FitLine(pos, ys, &a, &b);
      return emit([&](int64_t x, int64_t, int64_t, int64_t) { return Value::Number(a + b * static_cast<double>(x)); });
    }
    if (m == 1) return by_order([&](int64_t g) { return Value::Number(y0 + static_cast<double>(g)); });
    if (!inner) {  // 1, 2, 4, blank: the best-fit line by order
      double base = 0, slope = 0;
      TrendLine(ys, &base, &slope);
      return by_order([&](int64_t g) { return Value::Number(base + slope * static_cast<double>(g)); });
    }
    if (even && pos_even) return by_order([&](int64_t g) { return Value::Number(y0 + step * static_cast<double>(g)); });
    if (OnLine(pos, ys, &a, &b)) return by_place([&](int64_t x) { return Value::Number(a + b * static_cast<double>(x)); });
    FitLine(pos, ys, &a, &b);
    return emit([&](int64_t x, int64_t, int64_t, int64_t) { return Value::Number(a + b * static_cast<double>(x)); });
  }

  // dates and times: the linear and growth trends repeat them, as Fill Days and the like repeat times
  if (type == 9 || type == 10 || (is_time && type >= 5 && type <= 8)) return repeat();
  if (is_time) {
    if (m == 1) return by_order([&](int64_t g) { return Value::Number(WrapTime(y0 + static_cast<double>(g) / 24.0)); });
    if (even && order_ok) return by_order([&](int64_t g) { return Value::Number(WrapTime(y0 + step * static_cast<double>(g))); });
    if (inner && OnLine(pos, ys, &a, &b)) {
      return by_place([&](int64_t x) { return Value::Number(WrapTime(a + b * static_cast<double>(x))); });
    }
    return repeat();
  }
  bool ends = false;
  if (type < 5 || type > 8) {  // the fill handle, Fill Series, Fill Without Formatting
    const double frac = y0 - std::floor(y0);
    if (m == 1) {
      if (!FormatShowsDay(fmt)) {  // a date shown without its day (mmm-yy) goes on by months
        return by_order([&](int64_t g) { return Value::Number(ShiftMonths(std::floor(y0), g, true) + frac); });
      }
      return by_order([&](int64_t g) { return Value::Number(y0 + static_cast<double>(g)); });
    }
    const int64_t months = MonthStep(ys, &ends);
    if (months != 0 && order_ok) {
      return by_order([&](int64_t g) { return Value::Number(ShiftMonths(std::floor(y0), months * g, ends) + frac); });
    }
    if (months == 0 && MonthlyPairs(ys)) return repeat();
    if (even && order_ok) return by_order([&](int64_t g) { return Value::Number(y0 + step * static_cast<double>(g)); });
    if (inner && OnLine(pos, ys, &a, &b)) return by_place([&](int64_t x) { return Value::Number(a + b * static_cast<double>(x)); });
    return repeat();
  }

  // Fill Days (5), Weekdays (6), Months (7), Years (8): whole days
  std::vector<double> days;
  for (double y : ys) days.push_back(std::floor(y));
  const double d0 = days[0];
  auto num = [](double v) { return Value::Number(v); };
  if (m == 1) {
    return by_order([&](int64_t g) {
      const double v = type == 5 ? d0 + static_cast<double>(g) : type == 6 ? AddWeekdays(d0, g)
                                                                           : ShiftMonths(d0, type == 7 ? g : 12 * g, type == 7);
      return num(v);
    });
  }
  if (!order_ok) return repeat();
  const int64_t months = MonthStep(days, &ends);
  if (months == 0 && MonthlyPairs(days)) return repeat();
  double dstep = 0;
  const bool deven = Evenly(days, &dstep);
  // the seeds r rounds on by a month (or a year) a round: the first by the unit, the others as days of the first's
  // month (ShiftWithFirst); each date of seeds a whole number of months apart moves by itself (Fill Years), a month
  // end staying one when they all are (Jan 31, Feb 28, 2023 -> Feb 29, 2024)
  auto round_on = [&](int64_t i, int64_t r, bool years, bool per_seed) {
    const double di = days[static_cast<size_t>(i)];
    const int64_t units = years ? 12 * r : r;
    if (i == 0 || per_seed) return ShiftMonths(di, units, !years || ends);
    return ShiftWithFirst(d0, di, units, !years);
  };
  switch (type) {
    case 5:
      if (deven) return by_order([&](int64_t g) { return num(d0 + dstep * static_cast<double>(g)); });
      if (months != 0) return by_order([&](int64_t g) { return num(ShiftMonths(d0, months * g, ends)); });
      return repeat();
    case 6:
      if (months != 0) return by_order([&](int64_t g) { return num(WeekdayBack(ShiftMonths(d0, months * g, ends))); });
      if (!deven || dstep == 0) return repeat();
      if (std::fabs(dstep) == 1) return by_order([&](int64_t g) { return num(AddWeekdays(d0, static_cast<int64_t>(dstep) * g)); });
      if (std::fabs(dstep) <= 7) {
        return emit([&](int64_t, int64_t k, int64_t r, int64_t i) {
          return i < 0 ? seq[static_cast<size_t>(k)]->value : num(WeekdayBack(days[static_cast<size_t>(i)] + 7.0 * static_cast<double>(r)));
        });
      }
      return emit([&](int64_t, int64_t k, int64_t r, int64_t i) {
        return i < 0 ? seq[static_cast<size_t>(k)]->value : num(WeekdayBack(round_on(i, r, false, false)));
      });
    case 7:
      if (months != 0) return by_order([&](int64_t g) { return num(ShiftMonths(d0, months * g, ends)); });
      if (!deven) return repeat();
      return emit([&](int64_t, int64_t k, int64_t r, int64_t i) {
        return i < 0 ? seq[static_cast<size_t>(k)]->value : num(round_on(i, r, false, false));
      });
    default:
      if (months != 0 && months % 12 == 0) return by_order([&](int64_t g) { return num(ShiftMonths(d0, months * g, ends)); });
      if (months == 0 && !deven) return repeat();
      return emit([&](int64_t, int64_t k, int64_t r, int64_t i) {
        return i < 0 ? seq[static_cast<size_t>(k)]->value : num(round_on(i, r, true, months != 0));
      });
  }
}

}  // namespace

void Document::SetLinkedPicture(uint32_t sheet, uint32_t shape, uint32_t src_sheet, const Rect& src) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  auto& v = s->linked_pictures.mut();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Sheet::LinkedPicture& x) { return x.shape == shape; }), v.end());
  if (src.r0 <= src.r1 && src.c0 <= src.c1) v.push_back(Sheet::LinkedPicture{shape, src_sheet, src});
  wb_.dirty = true;
}

void Document::SetTabsSelected(const std::vector<uint32_t>& ids) {
  for (const auto& sc : wb_.sheets) {
    const bool on = std::find(ids.begin(), ids.end(), sc->id) != ids.end();
    if (sc->tab_selected == on) continue;
    if (Sheet* s = M(sc->id)) s->tab_selected = on;
  }
}

std::string Document::AutoFillTip(const model::Workbook& wb, const model::Sheet& s, const Rect& src, const Rect& dest, int type) {
  if (!dest.Contains(src) || dest == src) return {};
  const bool vertical = dest.r0 != src.r0 || dest.r1 != src.r1;
  const Rect from = vertical ? Rect{src.r0, src.c0, src.r1, src.c0} : Rect{src.r0, src.c0, src.r0, src.c1};
  const Rect to = vertical ? Rect{dest.r0, src.c0, dest.r1, src.c0} : Rect{src.r0, dest.c0, src.r0, dest.c1};
  if (to.Count() > 200000) return {};
  Document d;
  const uint32_t sh = d.wb().sheets[0]->id;
  for (uint32_t r = from.r0; r <= from.r1; ++r) {
    for (uint32_t c = from.c0; c <= from.c1; ++c) {
      if (s.GetFormula(r, c)) return {};
      d.SetValue(sh, r, c, s.GetValue(r, c));
      const std::string& code = wb.styles->NumberFormat(wb.styles->Get(s.GetStyle(r, c)));
      if (!code.empty() && code != "General") {
        const uint32_t id = d.wb_.styles.mut().InternString(code);
        d.ModifyStyle(sh, Rect::Cell(r, c), [id](Style& x) { x.numfmt = id; });
      }
    }
  }
  d.AutoFill(sh, from, to, type);
  const uint32_t lr = vertical ? (dest.r1 > src.r1 ? dest.r1 : dest.r0) : src.r0;
  const uint32_t lc = vertical ? src.c0 : (dest.c1 > src.c1 ? dest.c1 : dest.c0);
  const Value& v = d.GetValue(sh, lr, lc);
  if (v.IsEmpty()) return {};
  return DisplayTextOf(v, d.wb().styles->NumberFormat(d.wb().styles->Get(d.S(sh)->GetStyle(lr, lc))));
}

void Document::AutoFill(uint32_t sheet, const Rect& src, const Rect& dest, int type) {
  if (RefuseReadOnly(sheet)) return;
  const Sheet* s0 = S(sheet);
  if (!s0) return;
  // direction
  int dir;  // 0 down, 1 right, 2 up, 3 left
  if (dest.r1 > src.r1) dir = 0;
  else if (dest.c1 > src.c1) dir = 1;
  else if (dest.r0 < src.r0) dir = 2;
  else if (dest.c0 < src.c0) dir = 3;
  else return;
  const bool vertical = dir == 0 || dir == 2;
  // what the fill writes is refused as a whole before anything changes (part of an array, protection...): the
  // pastes below refused their cells one by one, and the source's merged cells were repeated over the array anyway,
  // a change without an Undo step (doc_fuzz seed 11652641)
  {
    Rect target = dest;
    if (dir == 0) target.r0 = src.r1 + 1;
    else if (dir == 1) target.c0 = src.c1 + 1;
    else if (dir == 2) target.r1 = src.r0 - 1;
    else target.c1 = src.c0 - 1;
    if (target.r0 <= target.r1 && target.c0 <= target.c1 && Refuse(sheet, &target)) return;
  }
  // whole rows (columns) filled down (right): only the columns (rows) that hold something anywhere have lanes; Copy
  // cut whole rows down to the used range and the lanes past it read outside the copy (a crash, dragging rows 3:4)
  Rect lanes_src = src;
  if (vertical && src.c1 >= core::kMaxCols - 1) lanes_src.c1 = s0->has_used ? std::min(src.c1, s0->used.c1) : src.c0;
  if (!vertical && src.r1 >= s0->row_limit - 1) lanes_src.r1 = s0->has_used ? std::min(src.r1, s0->used.r1) : src.r0;
  if (lanes_src.c1 < lanes_src.c0 || lanes_src.r1 < lanes_src.r0) return;
  const uint32_t lanes = vertical ? static_cast<uint32_t>(lanes_src.Cols()) : static_cast<uint32_t>(lanes_src.Rows());
  const uint32_t n = vertical ? static_cast<uint32_t>(src.Rows()) : static_cast<uint32_t>(src.Cols());
  const uint32_t total = vertical ? static_cast<uint32_t>(dest.Rows()) : static_cast<uint32_t>(dest.Cols());
  const ClipData clip = Copy(sheet, lanes_src);
  if (clip.rows * clip.cols == 0 || (vertical ? clip.cols : clip.rows) != lanes || (vertical ? clip.rows : clip.cols) != n) return;

  for (uint32_t lane = 0; lane < lanes; ++lane) {
    // source sequence
    std::vector<const ClipCell*> seq(n);
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t i = vertical ? k : lane, j = vertical ? lane : k;
      seq[k] = &clip.cells[static_cast<size_t>(i) * clip.cols + j];
    }
    // the source cells' hyperlinks repeat with them (Excel 16, measured: AutoFill of a linked cell)
    std::vector<const model::Hyperlink*> seq_link(n, nullptr);
    for (const auto& [idx, l] : clip.links) {
      const uint32_t i = static_cast<uint32_t>(idx / clip.cols), j = static_cast<uint32_t>(idx % clip.cols);
      if ((vertical ? j : i) == lane) seq_link[vertical ? i : j] = &l;
    }
    // detect the kind of sequence
    bool all_num = true, any_formula = false, mixed_kinds = false;
    std::vector<FillKind> kinds(n, FillKind::General);
    FillKind lane_kind = FillKind::General;
    bool have_kind = false;
    for (uint32_t k = 0; k < n; ++k) {
      const ClipCell* cc = seq[k];
      all_num = all_num && cc->value.IsNumber() && !cc->formula;
      any_formula = any_formula || cc->formula;
      if (!cc->value.IsNumber() || cc->formula) continue;
      kinds[k] = cc->has_style ? KindOfFormat(wb_.styles->Str(cc->style.numfmt)) : FillKind::General;
      if (!have_kind) {
        lane_kind = kinds[k];
        have_kind = true;
      } else if (!SameFillKind(lane_kind, kinds[k])) {
        mixed_kinds = true;
      } else if (lane_kind == FillKind::General) {
        lane_kind = kinds[k];
      }
    }
    // [1, 9:30]: run by run, not one trend ([2023/12/31, 9:00] with Fill Weekdays: the date moves, the time repeats)
    if (type == 1 || type == 3) mixed_kinds = false;
    if (mixed_kinds) all_num = false;
    double step = 0, base0 = 0;
    bool linear = false;
    if (all_num && n >= 2) {  // Excel's trend fill for two or more numbers
      std::vector<double> ys(n);
      for (uint32_t k = 0; k < n; ++k) ys[k] = seq[k]->value.number();
      TrendLine(ys, &base0, &step);
      linear = true;
    }
    // dates, times, blanks among the cells and text series: worked out for the whole lane
    std::string fmt_item;  // the number format of the first cell that isn't blank
    for (auto* cc : seq) {
      if (cc->value.IsEmpty()) continue;
      if (cc->has_style) fmt_item = std::string(wb_.styles->Str(cc->style.numfmt));
      break;
    }
    std::vector<Value> planned;
    const bool plan = !mixed_kinds && PlanFillLane(seq, fmt_item, type, total, dir >= 2, &planned);
    // growth trend: a least-squares fit of log(y): the values follow the fitted curve a * ratio^x (Excel: 1, 3, 4 go
    // on with 9.1577, 18.3154), not the last value times the ratio
    double ratio = 0, growth_a = 0;
    if (type == 10 && all_num && n >= 2) {
      bool positive = true;
      for (auto* cc : seq) positive = positive && cc->value.number() > 0;
      if (positive) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (uint32_t k = 0; k < n; ++k) {
          const double x = k, y = std::log(seq[k]->value.number());
          sx += x;
          sy += y;
          sxx += x * x;
          sxy += x * y;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        ratio = std::exp(slope);
        growth_a = std::exp((sy - slope * sx) / n);
      }
    }
    // mixed patterns (e.g. 1, 2, Jan): each run of numbers of one kind forms its own linear series, advancing by
    // "slope x run length" per round (Excel behavior); blanks inside a run don't end it (1, blank, 5, a -> 9, blank,
    // 13, a), text, a formula or a number of another kind does. One number alone counts on by 1 a round, a time by an
    // hour, a date shown without its day by a month (Excel 16, measured)
    std::vector<double> run_step(n, 0.0);
    std::vector<char> by_month(n, 0);
    if (!plan && !all_num) {
      uint32_t i = 0;
      while (i < n) {
        if (!seq[i]->value.IsNumber() || seq[i]->formula) {
          ++i;
          continue;
        }
        std::vector<uint32_t> run{i};
        FillKind run_kind = kinds[i];
        uint32_t j = i + 1;
        for (; j < n && !seq[j]->formula && (seq[j]->value.IsNumber() || seq[j]->value.IsEmpty()); ++j) {
          if (!seq[j]->value.IsNumber()) continue;
          if (!SameFillKind(run_kind, kinds[j])) break;
          if (run_kind == FillKind::General) run_kind = kinds[j];
          run.push_back(j);
        }
        const uint32_t L = static_cast<uint32_t>(run.size());
        if (L >= 2) {
          double sx = 0, sy = 0, sxx = 0, sxy = 0;
          for (uint32_t k2 = 0; k2 < L; ++k2) {
            const double x = k2, y = seq[run[k2]]->value.number();
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
          }
          const double slope = (L * sxy - sx * sy) / (L * sxx - sx * sx);
          for (uint32_t k2 : run) run_step[k2] = slope * L;
        } else if (kinds[i] == FillKind::Time) {
          run_step[i] = 1.0 / 24;  // 9:30, a -> 10:30, a
        } else if (kinds[i] == FillKind::Date && seq[i]->has_style && !FormatShowsDay(wb_.styles->Str(seq[i]->style.numfmt))) {
          by_month[i] = 1;  // 12/2024 (Dec-24), a -> Jan-25, a
        } else {
          run_step[i] = 1;  // one number among texts counts on by one a round (1, a -> 2, a, 3)
        }
        i = j;
      }
    }
    // Fill Days, Weekdays, Months, Years: a date among other cells moves by the unit a round (2024/1/31, a -> Feb 29,
    // a), other numbers repeat; the growth trend repeats them all (Excel 16, measured)
    std::vector<char> date_unit(n, 0);
    if (!plan && !all_num && type >= 5 && type != 9) {
      for (uint32_t k2 = 0; k2 < n; ++k2) {
        if (type <= 8 && kinds[k2] == FillKind::Date) date_unit[k2] = static_cast<char>(type - 4);
        run_step[k2] = 0;
        by_month[k2] = 0;
      }
    }
    // runs of texts of one series among other cells ([Item10, Item2, a99] -> Item18 above, a98): each run goes on by
    // its own step, as numbers do (Item1, Item1 repeat)
    std::vector<TextSeries> text_runs;
    std::vector<int> text_run_of(n, -1);
    std::vector<double> text_run_pos(n, 0), text_run_len(n, 0);
    if (!plan && !all_num && type < 5) {
      uint32_t i = 0;
      while (i < n) {
        uint32_t j = i + 1;
        if (seq[i]->value.IsString() && !seq[i]->formula) {
          TextSeries ts;
          while (j < n) {
            std::vector<const ClipCell*> items(seq.begin() + i, seq.begin() + j + 1);
            if (!LaneTextSeries(items, &ts)) break;
            ++j;
          }
          if (j - i >= 2) {
            std::vector<const ClipCell*> items(seq.begin() + i, seq.begin() + j);
            LaneTextSeries(items, &ts);
            text_runs.push_back(ts);
            for (uint32_t k2 = i; k2 < j; ++k2) {
              text_run_of[k2] = static_cast<int>(text_runs.size()) - 1;
              text_run_pos[k2] = k2 - i;
              text_run_len[k2] = j - i;
            }
          }
        }
        i = j;
      }
    }

    for (uint32_t t = n; t < total; ++t) {
      // destination cell position
      uint32_t dr, dc;
      const uint32_t off = t;
      if (dir == 0) {
        dr = src.r0 + off;
        dc = src.c0 + lane;
      } else if (dir == 1) {
        dr = src.r0 + lane;
        dc = src.c0 + off;
      } else if (dir == 2) {
        dr = src.r1 - off;
        dc = src.c0 + lane;
      } else {
        dr = src.r0 + lane;
        dc = src.c1 - off;
      }
      const uint32_t k = (dir == 0 || dir == 1) ? t % n : (n - 1) - (t % n);
      const ClipCell& base = *seq[k];
      // single-cell paste (with formatting and relative formulas)
      ClipData one;
      one.rows = one.cols = 1;
      one.cells.push_back(base);
      if (seq_link[k]) one.links.emplace_back(0, *seq_link[k]);
      const int64_t sign = (dir == 0 || dir == 1) ? 1 : -1;
      // Copy Cells / Fill Formatting Only: the source repeats
      if (type == 1 || type == 3) {
        Paste(sheet, dr, dc, one, type == 3 ? PasteMode::Formats : PasteMode::All);
        continue;
      }
      // the destination's position counted from the first source cell
      const double xpos = (dir == 0 || dir == 1) ? static_cast<double>(t) : -static_cast<double>(t - n + 1);
      if (plan) {
        one.cells[0].value = planned[t - n];
        Paste(sheet, dr, dc, one, type == 4 ? PasteMode::Values : PasteMode::All);
        continue;
      }
      const bool num0 = seq[0]->value.IsNumber();
      const double first = num0 ? seq[0]->value.number() : 0;
      if (!any_formula && all_num && type >= 5 && type <= 8) {
        // Fill Days / Weekdays / Months / Years on numbers that aren't dates: they repeat (Excel)
        Paste(sheet, dr, dc, one);
        continue;
      }
      if (!any_formula && all_num && (type == 2 || type == 9) && n == 1) {
        // Fill Series / linear trend from one number: steps of 1
        one.cells[0].value = Value::Number(first + xpos);
        Paste(sheet, dr, dc, one, type == 4 ? PasteMode::Values : PasteMode::All);
        continue;
      }
      if (!any_formula && type == 10 && all_num && ratio > 0) {
        one.cells[0].value = Value::Number(growth_a * std::pow(ratio, xpos));
        Paste(sheet, dr, dc, one);
        continue;
      }
      if (!base.formula) {
        Value v = base.value;
        const int64_t rounds = sign * static_cast<int64_t>(t / n);
        if (linear) {
          v = Value::Number(base0 + step * xpos);
        } else if (base.value.IsNumber() && date_unit[k]) {
          const double d = base.value.number(), day = std::floor(d);
          const double moved = date_unit[k] == 1 ? day + static_cast<double>(rounds)
                               : date_unit[k] == 2 ? AddWeekdays(day, rounds)
                                                   : ShiftMonths(day, date_unit[k] == 3 ? rounds : 12 * rounds, true);
          v = Value::Number(moved + (d - day));
        } else if (type >= 5 && base.value.IsString()) {
          // Fill Days... among other cells: a day or month name the type steps moves, any other text repeats
          int list = -1, idx = 0;
          if (FindInFillLists(std::string(base.value.str()), &list, &idx) && ListStepsUnder(list, type)) {
            const auto L = AllFillLists()[static_cast<size_t>(list)];
            const int64_t cycle = static_cast<int64_t>(L.size());
            const int64_t ni = type == 6 ? WeekdayNameAfter(idx, rounds) : ((idx + rounds) % cycle + cycle) % cycle;
            v = Value::String(L[static_cast<size_t>(ni)]);
          }
        } else if (base.value.IsNumber() && by_month[k]) {
          const double d = base.value.number();
          v = Value::Number(ShiftMonths(std::floor(d), rounds, true) + (d - std::floor(d)));
        } else if (base.value.IsNumber() && run_step[k] != 0.0) {
          const double y = base.value.number() + run_step[k] * static_cast<double>(rounds);
          v = Value::Number(kinds[k] == FillKind::Time ? WrapTime(y) : y);
        } else if (text_run_of[k] >= 0) {
          const TextSeries& ts = text_runs[static_cast<size_t>(text_run_of[k])];
          v = Value::String(TextSeriesAt(ts, text_run_pos[k] + text_run_len[k] * static_cast<double>(rounds)));
        } else if (base.value.IsString()) {
          const std::string str(base.value.str());
          int list, idx;
          std::string prefix;
          int64_t num;
          size_t width;
          if (FindInFillLists(str, &list, &idx)) {
            const auto L = AllFillLists()[static_cast<size_t>(list)];
            const int64_t cycle = static_cast<int64_t>(L.size());
            const int64_t inc = static_cast<int64_t>(t / n);
            const int64_t ni = ((idx + sign * inc) % cycle + cycle) % cycle;
            v = Value::String(L[static_cast<size_t>(ni)]);
          } else if (int q = 0; SplitQuarter(str, &prefix, &q)) {
            // quarters cycle Q1..Q4 (Qtr1, Quarter 1)
            const int64_t inc = static_cast<int64_t>(t / n);
            v = Value::String(prefix + std::to_string(((q - 1 + sign * inc) % 4 + 4) % 4 + 1));
          } else if (std::string rest; SplitOrdinal(str, &num, &rest)) {
            const int64_t inc = static_cast<int64_t>(t / n);
            int64_t nv = num + sign * inc;
            if (nv < 0) nv = -nv;
            v = Value::String(Ordinal(nv) + rest);
          } else if (std::string after; SplitTrailingNumber(str, &prefix, &num, &width, &after)) {
            const int64_t inc = static_cast<int64_t>(t / n);
            int64_t nv = num + sign * inc;
            if (nv < 0) nv = -nv;
            std::string digits = std::to_string(nv);
            if (digits.size() < width) digits.insert(0, width - digits.size(), '0');
            v = Value::String(prefix + digits + after);
          }
        }
        one.cells[0].value = v;
      }
      // Fill Without Formatting: values (or formulas) only
      Paste(sheet, dr, dc, one, type == 4 ? (any_formula ? PasteMode::Formulas : PasteMode::Values) : PasteMode::All);
      if (base.formula && !vertical) {
        const uint32_t sr = src.r0 + lane, sc = src.c0 + k;  // the source cell of this one
        ShiftTableColumns(sheet, dr, dc, sr, sc, int64_t{dc} - int64_t{sc});
      }
    }
  }
  // the parts of Ctrl+Shift+Enter arrays in the source repeat as arrays, as a paste makes them: each its formula moved
  // from the array's first cell to the part's (Excel 16, measured: cse_autofill_ cases; written above as values)
  for (const ClipData::ArrayPart& part : clip.arrays) {
    const auto* f = static_cast<const formula::CompiledFormula*>(part.formula.get());
    if (!f) continue;
    const int64_t len = static_cast<int64_t>(n);
    for (int64_t k = 1; k * len < static_cast<int64_t>(total) + len; ++k) {
      const int64_t off = (dir == 0 || dir == 1) ? k * len : -k * len;
      int64_t r0 = int64_t{lanes_src.r0} + part.rect.r0, r1 = int64_t{lanes_src.r0} + part.rect.r1;
      int64_t c0 = int64_t{lanes_src.c0} + part.rect.c0, c1 = int64_t{lanes_src.c0} + part.rect.c1;
      if (vertical) r0 += off, r1 += off;
      else c0 += off, c1 += off;
      r0 = std::max<int64_t>(r0, dest.r0), c0 = std::max<int64_t>(c0, dest.c0);
      r1 = std::min<int64_t>(r1, dest.r1), c1 = std::min<int64_t>(c1, dest.c1);
      if (r0 > r1 || c0 > c1) continue;
      const Rect x{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(r1), static_cast<uint32_t>(c1)};
      if (src.Intersects(x)) continue;
      formula::Node copy = f->root;
      FixOutOfBounds(copy, core::CellPos{x.r0, x.c0}, M(sheet)->row_limit);
      SetArrayFormula(sheet, x, formula::ToA1(copy, core::CellPos{x.r0, x.c0}, this), nullptr, kSrcUser);
    }
  }
  // the source's data validation repeats with it (FillValidations; Fill Formatting Only and Fill Without Formatting,
  // not measured, leave it)
  if (type != 3 && type != 4) {
    Rect target = dest;
    if (dir == 0) target.r0 = src.r1 + 1;
    else if (dir == 1) target.c0 = src.c1 + 1;
    else if (dir == 2) target.r1 = src.r0 - 1;
    else target.c1 = src.c0 - 1;
    FillValidations(sheet, src, target, vertical, dir == 2 || dir == 3);
  }
  // the source's merged cells repeat with it, as Excel 16's fill handle does (measured: B2:C3 merged and filled down
  // to C7 merges B4:C5 and B6:C7); Fill Without Formatting leaves them
  if (type == 4) return;
  const Sheet* s1 = S(sheet);
  if (!s1 || s1->merges->empty()) return;
  std::vector<Rect> from;
  for (const Rect& m : *s1->merges) {
    if (src.Contains(m)) from.push_back(m);
  }
  if (from.empty()) return;
  const int64_t len = static_cast<int64_t>(n);
  std::vector<Rect> made;
  // none over a table's cells, as a paste's (the table holds no merged cells; doc_fuzz seed 32027683)
  const auto& tabs = *s1->tables;
  auto on_table = [&](const Rect& x) { return std::any_of(tabs.begin(), tabs.end(), [&](const model::Table& t) { return t.range.Intersects(x); }); };
  for (int64_t k = 1; k * len < static_cast<int64_t>(total) + len; ++k) {
    const int64_t off = (dir == 0 || dir == 1) ? k * len : -k * len;
    for (const Rect& m : from) {
      const int64_t r0 = vertical ? int64_t{m.r0} + off : m.r0, r1 = vertical ? int64_t{m.r1} + off : m.r1;
      const int64_t c0 = vertical ? m.c0 : int64_t{m.c0} + off, c1 = vertical ? m.c1 : int64_t{m.c1} + off;
      if (r0 < 0 || c0 < 0) continue;
      const Rect x{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(r1), static_cast<uint32_t>(c1)};
      if (dest.Contains(x) && !src.Intersects(x) && !on_table(x)) made.push_back(x);
    }
  }
  if (made.empty()) return;
  // the merged areas are part of the step even when every value the fill wrote was already there: unnoted, the
  // merges changed without an Undo step (doc_fuzz seed 17480302)
  // the merged areas in the cells filled make way (the pastes there removed them too), compared with the filled cells
  // as one rectangle: each against every area made was a million squared steps down a whole column, as the fill's
  // paste a line was (doc_fuzz --wide seed 24120027)
  Rect target = dest;
  if (dir == 0) target.r0 = src.r1 + 1;
  else if (dir == 1) target.c0 = src.c1 + 1;
  else if (dir == 2) target.r1 = src.r0 - 1;
  else target.c1 = src.c0 - 1;
  NoteTouched();
  NoteFormat(sheet, target);
  auto& mg = M(sheet)->merges.mut();
  mg.erase(std::remove_if(mg.begin(), mg.end(), [&](const Rect& x) { return x.Intersects(target); }), mg.end());
  mg.insert(mg.end(), made.begin(), made.end());
}

// the fill handle dragged across moves a formula's table column names by the columns it moved, as Excel does:
// =SUM(Sales[Jan]) filled right is =SUM(Sales[Feb]), [@Qty] is [@Px]; past the table's last column it is #REF!.
// A copy, a paste and Ctrl+R keep the names.
void Document::ShiftTableColumns(uint32_t sheet, uint32_t r, uint32_t c, uint32_t src_r, uint32_t src_c, int64_t delta) {
  const Sheet* s = S(sheet);
  if (!s || delta == 0) return;
  const uint32_t fid = s->GetFormula(r, c);
  const formula::CompiledFormula* f = fid ? Formula(fid) : nullptr;
  if (!f || f->key.find("T'") == std::string::npos) return;  // no table reference
  formula::Node root = f->root;
  bool changed = false;
  std::function<void(formula::Node&)> walk = [&](formula::Node& n) {
    for (auto& k : n.kids) walk(k);
    if (n.kind != formula::NK::StructRef || n.kids.size() < 2 || n.kids[0].str.empty()) return;
    const model::Table* tb = n.str.empty() ? model::TableAt(wb_, sheet, src_r, src_c) : model::FindTable(wb_, n.str);
    if (!tb) return;
    std::string names[2] = {n.kids[0].str, n.kids[1].str};
    for (std::string& col : names) {
      if (col.empty()) continue;
      const int at = tb->ColumnIndex(col);
      if (at < 0) return;
      const int64_t to = at + delta;
      if (to < 0 || to >= static_cast<int64_t>(tb->columns.size())) {
        n = formula::Node{};
        n.kind = formula::NK::Err;
        n.err = core::Err::Ref;
        changed = true;
        return;
      }
      col = tb->columns[static_cast<size_t>(to)].name;
    }
    n.kids[0].str = names[0];
    n.kids[1].str = names[1];
    changed = true;
  };
  walk(root);
  if (!changed) return;
  const uint32_t nid = InternFormula(std::move(root));
  UnregisterFormulaCell(sheet, r, c);
  M(sheet)->SetFormula(r, c, nid);
  RegisterFormulaCell(sheet, r, c, nid);
}

void Document::Sort(uint32_t sheet, const Rect& rect_in, const std::vector<SortKey>& keys, bool has_header, bool visible_only, bool match_case,
                    bool left_to_right) {
  if (Refuse(sheet, &rect_in, Allow::Sort)) return;
  const Sheet* s = S(sheet);
  if (!s || keys.empty()) return;
  Rect rect = ClampToUsed(*s, rect_in);
  if (RefuseTableOutsideList(sheet, rect)) return;
  // cells inside a table sort the table: its whole data body, whatever part of it was given and whether a header was
  // named (Excel 16's Range.Sort, measured: C2:C4 of a table over A1:G6 sorts A2:G6 by C; A2:G4 with a header too)
  if (!left_to_right) {
    for (const model::Table& t : *s->tables) {
      if (!t.range.Contains(rect) || !t.HasData()) continue;
      rect = t.Data();
      has_header = false;
      break;
    }
  }
  if (has_header) {
    if (left_to_right) ++rect.c0;  // a left-to-right list's first column holds its labels
    else ++rect.r0;
  }
  if (left_to_right ? rect.c0 >= rect.c1 : rect.r0 >= rect.r1) return;
  if (RefuseMergedSort(sheet, rect)) return;  // the merges would be split up (Excel refuses too)
  // an array formula entered with Ctrl+Shift+Enter in the list, whole or in part: Excel 16 sorts nothing (measured
  // 2026-11-20: G:G, G2:G4 or A1:G4 over G2:G4's array raise). Whole, its cells were sorted as values and the array
  // moved with its references shifted (=B1:B3*2 in G1). An array of one cell is sorted as a cell (measured)
  if (RefuseArrayOperation(sheet, rect, /*multi_cell_only=*/true)) return;
  const ClipData data = Copy(sheet, rect);
  if (data.rows == 0) return;
  // the lines moved: rows, or columns when sorting left to right; the keys name a column (a row) of the list
  const uint32_t lines = left_to_right ? data.cols : data.rows;
  auto cell = [&](uint32_t line, uint32_t key) -> const ClipCell& {
    return left_to_right ? data.cells[static_cast<size_t>(key) * data.cols + line] : data.cells[static_cast<size_t>(line) * data.cols + key];
  };
  const uint32_t k0 = left_to_right ? rect.r0 : rect.c0, k1 = left_to_right ? rect.r1 : rect.c1;
  // hidden rows (a filtered list) don't move, as in Excel: the visible rows are sorted among themselves
  std::vector<uint32_t> slots;
  slots.reserve(lines);
  for (uint32_t i = 0; i < lines; ++i) {
    const bool hidden = left_to_right ? s->cols->IsHidden(rect.c0 + i) : s->rows->IsHidden(rect.r0 + i);
    if (!visible_only || !hidden) slots.push_back(i);
  }
  if (slots.size() < 2) return;
  std::vector<uint32_t> order = slots;
  auto rank = [](const Value& v) {
    if (v.IsNumeric()) return 0;
    if (v.IsString()) return 1;
    if (v.IsBool()) return 2;
    if (v.IsError()) return 3;
    return 4;  // blanks always last
  };
  // custom list orders: position of each text in its list
  const auto lists = AllFillLists();
  auto list_pos = [&](const SortKey& k, const Value& v) -> int {
    const bool own = !k.custom_items.empty();
    if ((!own && (k.custom_list < 0 || k.custom_list >= static_cast<int>(lists.size()))) || !v.IsString()) return -1;
    // Case sensitive takes the list's items as they are written: high isn't High (Excel 16, measured 2026-11-28)
    const std::string low = match_case ? std::string(v.str()) : Lower(v.str());
    const auto& L = own ? k.custom_items : lists[static_cast<size_t>(k.custom_list)];
    for (size_t i = 0; i < L.size(); ++i) {
      if ((match_case ? L[i] : Lower(L[i])) == low) return static_cast<int>(i);
    }
    return -1;
  };
  // Sort On Conditional Formatting Icon: the icon each line shows in the key's cell
  std::map<std::pair<size_t, uint32_t>, int> icons;  // (key index, line) -> icon_set * 100 + icon, -1 none
  for (size_t ki = 0; ki < keys.size(); ++ki) {
    const SortKey& k = keys[ki];
    if (k.sort_on != 3 || k.col < k0 || k.col > k1) continue;
    CondFormatEval cf(wb_, sheet);
    for (uint32_t line : slots) {
      const uint32_t r = left_to_right ? k.col : rect.r0 + line, c = left_to_right ? rect.c0 + line : k.col;
      CfVisual vis;
      int got = -1;
      if (!cf.Empty() && cf.Eval(r, c, &vis) && vis.icon >= 0) got = vis.icon_style * 100 + vis.icon;
      icons[{ki, line}] = got;
    }
  }
  // each value key's texts lowered once: the comparison lowered (and copied) both sides every time, and a million
  // rows took four seconds to sort
  std::vector<std::vector<std::string>> lowered(keys.size());
  for (size_t ki = 0; ki < keys.size() && !match_case; ++ki) {
    const SortKey& k = keys[ki];
    if (k.col < k0 || k.col > k1 || k.sort_on != 0) continue;
    std::vector<std::string>& low = lowered[ki];
    low.resize(lines);
    for (uint32_t line : slots) {
      const Value& v = cell(line, k.col - k0).value;
      if (v.IsString()) low[line] = formula::fn::Lower(v.str());
    }
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    for (size_t ki = 0; ki < keys.size(); ++ki) {
      const SortKey& k = keys[ki];
      if (k.col < k0 || k.col > k1) continue;
      const uint32_t j = k.col - k0;
      if (k.sort_on == 3) {
        const int want = k.icon_set * 100 + k.icon;
        const bool ha = icons[{ki, a}] == want, hb = icons[{ki, b}] == want;
        if (ha != hb) return k.descending ? hb : ha;
        continue;
      }
      if (k.sort_on == 1 || k.sort_on == 2) {
        // Sort by Color: the rows with the color on top (on bottom when descending)
        auto has = [&](uint32_t line) {
          const ClipCell& cc = cell(line, j);
          const uint32_t have = k.sort_on == 1 ? cc.style.fill_color : cc.style.font_color;
          const bool none = (have >> 24) == 0, want_none = (k.color >> 24) == 0;
          return none ? want_none : (!want_none && (have & 0xFFFFFF) == (k.color & 0xFFFFFF));
        };
        const bool ha = has(a), hb = has(b);
        if (ha != hb) return k.descending ? hb : ha;
        continue;
      }
      const Value& va = cell(a, j).value;
      const Value& vb = cell(b, j).value;
      if (k.custom_list >= 0 || !k.custom_items.empty()) {
        // the list's order; values not in the list come after it, and before it when descending (the whole order
        // reversed); blanks stay last (Excel 16, measured 2026-11-28)
        const int pa = list_pos(k, va), pb = list_pos(k, vb);
        if (pa >= 0 || pb >= 0) {
          if (pa < 0) return k.descending && rank(va) != 4;
          if (pb < 0) return !(k.descending && rank(vb) != 4);
          if (pa != pb) return k.descending ? pa > pb : pa < pb;
          continue;
        }
      }
      const int ra = rank(va), rb = rank(vb);
      if (ra == 4 || rb == 4) {
        if (ra != rb) return rb == 4;
        continue;
      }
      int cmp;
      if (ra != rb) {
        cmp = ra < rb ? -1 : 1;
      } else if (match_case && ra == 1) {
        // Case sensitive (Sort Options): Windows' order with case, which weighs case before hyphens and apostrophes
        // (a'b before AB, ab- before Ab), as Excel does
        cmp = formula::CollateCased(va.str(), vb.str());
      } else if (ra == 1 && !lowered[ki].empty()) {
        cmp = formula::CollateLowered(lowered[ki][a], lowered[ki][b]);  // Evaluator::Compare's order for texts
      } else if (ra == 0) {  // Evaluator::Compare's order for numbers (15 significant digits)
        cmp = va.IsDecimal() && vb.IsDecimal() ? core::DecCompare(core::DecFromValue(va), core::DecFromValue(vb))
                                               : formula::Compare15(va.AsDouble(), vb.AsDouble());
      } else {
        bool ok;
        cmp = formula::Evaluator::Compare(va, vb, &ok);
      }
      if (cmp != 0) return k.descending ? cmp > 0 : cmp < 0;
    }
    return false;
  });
  // a formula moves with its line as a copy does, except that its references to other sheets keep naming the same
  // cells, and a range moved partly past the sheet's edge is cut to it (Excel 16, measured: =A3 two rows down is =A5,
  // =Other!A2 stays =Other!A2, absolute parts stay; =SUM(B2:B4) two rows up is =SUM(B1:B2), =A2 there is #REF!)
  const int64_t limit = left_to_right ? int64_t{core::kMaxCols} : int64_t{S(sheet)->row_limit};
  auto fit_line = [&](ClipCell& cc, int64_t delta, int64_t at) {  // at: the line's row (column) where it goes
    if (!cc.formula || delta == 0) return;
    formula::Node root = static_cast<const formula::CompiledFormula*>(cc.formula.get())->root;
    bool changed = false;
    formula::ForEachRef(root, [&](formula::RefSpec& ref) {
      if (!ref.HasCells() || (left_to_right ? ref.whole_rows : ref.whole_cols)) return;
      formula::RefCoord& lo = left_to_right ? ref.c0 : ref.r0;
      formula::RefCoord& hi = left_to_right ? ref.c1 : ref.r1;
      if (ref.TargetSheet(static_cast<int32_t>(sheet)) != static_cast<int32_t>(sheet)) {
        for (formula::RefCoord* x : {&lo, &hi}) {
          if (x == &hi && !ref.is_range) break;
          if (!x->abs) x->v -= delta, changed = true;
        }
        return;
      }
      if (!ref.is_range) return;  // one cell past the edge: #REF! (the paste's FixOutOfBounds)
      const int64_t a = lo.Resolve(at), z = hi.Resolve(at);
      if (z < 0 || a >= limit) return;  // all of it past the edge: #REF! too
      if (a < 0) lo.v = lo.abs ? 0 : -at, changed = true;
      if (z >= limit) hi.v = hi.abs ? limit - 1 : limit - 1 - at, changed = true;
    });
    if (!changed) return;
    // two statements: in one, `wb_.formulas->` is read before InternFormula copies a table a snapshot shares, and
    // the new formula was looked up in the old table (nothing: the cell was pasted as its value)
    const uint32_t id = InternFormula(std::move(root));
    cc.formula = wb_.formulas->Share(id);
  };
  // a sort that leaves every line where it was is still a step, as Excel's (measured 2026-11-16: Undo is enabled after
  // sorting 1, 2, 3): a Ctrl+Z after it took back the edit before it
  if (std::equal(order.begin(), order.end(), slots.begin(), slots.end())) NoteRegion(sheet, rect);
  // the fonts inside a line's texts and its hyperlinks move with it (Excel 16, measured 2026-11-28 for the links): the
  // paste of the line takes those of the cells under it away. at(i, j): the cell's index in the line, -1 off it
  auto carry = [&](ClipData& line, const auto& at) {
    for (const auto& [idx, rt] : data.rich) {
      const int64_t x = at(static_cast<uint32_t>(idx / data.cols), static_cast<uint32_t>(idx % data.cols));
      if (x >= 0) line.rich.emplace_back(static_cast<size_t>(x), rt);
    }
    for (const auto& [idx, l] : data.links) {
      const int64_t x = at(static_cast<uint32_t>(idx / data.cols), static_cast<uint32_t>(idx % data.cols));
      if (x >= 0) line.links.emplace_back(static_cast<size_t>(x), l);
    }
  };
  for (size_t k = 0; k < slots.size(); ++k) {
    if (order[k] == slots[k]) continue;
    ClipData line;
    const int64_t delta = static_cast<int64_t>(slots[k]) - static_cast<int64_t>(order[k]);
    if (left_to_right) {
      line.rows = data.rows;
      line.cols = 1;
      for (uint32_t i = 0; i < data.rows; ++i) line.cells.push_back(data.cells[static_cast<size_t>(i) * data.cols + order[k]]);
      for (auto& cc : line.cells) fit_line(cc, delta, int64_t{rect.c0} + static_cast<int64_t>(slots[k]));
      for (const ClipData::ArrayPart& part : data.arrays) {  // a one-cell array stays an array (as below)
        if (part.rect.c0 != order[k] || !part.rect.IsCell()) continue;
        ClipCell tmp;
        tmp.formula = part.formula;
        fit_line(tmp, delta, int64_t{rect.c0} + static_cast<int64_t>(slots[k]));
        line.arrays.push_back(ClipData::ArrayPart{Rect{part.rect.r0, 0, part.rect.r0, 0}, tmp.formula});
      }
      carry(line, [&](uint32_t i, uint32_t j) { return j == order[k] ? static_cast<int64_t>(i) : -1; });
      Paste(sheet, rect.r0, rect.c0 + slots[k], line);
    } else {
      line.rows = 1;
      line.cols = data.cols;
      line.cells.assign(data.cells.begin() + static_cast<ptrdiff_t>(order[k]) * data.cols, data.cells.begin() + static_cast<ptrdiff_t>(order[k] + 1) * data.cols);
      for (auto& cc : line.cells) fit_line(cc, delta, int64_t{rect.r0} + static_cast<int64_t>(slots[k]));
      // a one-cell array of the line stays an array (the copy writes its cell as a value): pasted alone, it came back
      // as a plain formula, which spilled (#SPILL!) where Excel 16 shows the array's first value (measured)
      for (const ClipData::ArrayPart& part : data.arrays) {
        if (part.rect.r0 != order[k] || !part.rect.IsCell()) continue;
        ClipCell tmp;
        tmp.formula = part.formula;
        fit_line(tmp, delta, int64_t{rect.r0} + static_cast<int64_t>(slots[k]));
        line.arrays.push_back(ClipData::ArrayPart{Rect{0, part.rect.c0, 0, part.rect.c0}, tmp.formula});
      }
      carry(line, [&](uint32_t i, uint32_t j) { return i == order[k] ? static_cast<int64_t>(j) : -1; });
      Paste(sheet, rect.r0 + slots[k], rect.c0, line);
    }
  }
}

std::vector<core::CellPos> Document::FindAll(uint32_t sheet, const Rect& within_in, std::string_view what, bool by_rows, bool match_case, bool whole_cell,
                                             bool look_in_formulas, size_t max, const FormatSpec* fmt) const {
  std::vector<core::CellPos> out;
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return out;
  const Rect within = ClampToUsed(*s, within_in);
  uint32_t r = within.r1, c = within.c1;  // start after the last cell (= at the first cell)
  while (out.size() < max) {
    uint32_t nr, nc;
    if (!Find(sheet, within, what, r, c, by_rows, match_case, whole_cell, look_in_formulas, &nr, &nc, fmt)) break;
    if (!out.empty() && nr == out.front().row && nc == out.front().col) break;  // wrap around to the first one
    out.push_back(core::CellPos{nr, nc});
    r = nr;
    c = nc;
  }
  return out;
}

namespace {
// a series' line written as two parts where it leaves a table past its last row (column), so the part beyond grows it
// as the cells written one by one did (Excel 16, measured: B2:B20's series grows A1:D6 to A1:D20)
void WriteLine(Document& d, uint32_t sheet, const Rect& line, const std::vector<Value>& vals, bool by_cols) {
  const model::Sheet* s = d.S(sheet);
  uint32_t cut = 0;  // values before the cut go in the first part
  if (s) {
    for (const model::Table& tb : *s->tables) {
      if (by_cols && line.c0 >= tb.range.c0 && line.c0 <= tb.range.c1 && line.r0 <= tb.range.r1 && line.r1 > tb.range.r1)
        cut = tb.range.r1 - line.r0 + 1;
      if (!by_cols && line.r0 >= tb.range.r0 && line.r0 <= tb.range.r1 && line.c0 <= tb.range.c1 && line.c1 > tb.range.c1)
        cut = tb.range.c1 - line.c0 + 1;
    }
  }
  if (cut == 0) {
    d.SetValues(sheet, line, vals, kSrcUser);
    return;
  }
  const Rect a = by_cols ? Rect{line.r0, line.c0, line.r0 + cut - 1, line.c0} : Rect{line.r0, line.c0, line.r0, line.c0 + cut - 1};
  const Rect b = by_cols ? Rect{line.r0 + cut, line.c0, line.r1, line.c0} : Rect{line.r0, line.c0 + cut, line.r0, line.c1};
  d.SetValues(sheet, a, std::vector<Value>(vals.begin(), vals.begin() + cut), kSrcUser);
  d.SetValues(sheet, b, std::vector<Value>(vals.begin() + cut, vals.end()), kSrcUser);
}
}  // namespace

void Document::FillSeries(uint32_t sheet, const Rect& rect, bool by_cols, int type, int unit, double step, bool has_stop, double stop) {
  if (RefuseReadOnly(sheet)) return;
  const Sheet* s0 = S(sheet);
  if (!s0) return;
  const uint32_t lines = static_cast<uint32_t>(by_cols ? rect.Cols() : rect.Rows());
  // a selection one cell long with a Stop value goes on past it to the Stop value, as far as the sheet's edge
  // (Excel 16, measured: 1 in A1, step 3, stop 10 fills A1:A4); a longer one stops at its end or the Stop value
  const bool to_stop = has_stop && (by_cols ? rect.Rows() : rect.Cols()) == 1;
  const uint32_t edge = by_cols ? s0->row_limit - rect.r0 : s0->col_limit - rect.c0;
  const uint32_t len = to_stop ? edge : static_cast<uint32_t>(by_cols ? rect.Rows() : rect.Cols());
  for (uint32_t li = 0; li < lines && li < 16384; ++li) {
    const uint32_t r0 = by_cols ? rect.r0 : rect.r0 + li, c0 = by_cols ? rect.c0 + li : rect.c0;
    const Value v0 = S(sheet)->GetValue(r0, c0);
    if (!v0.IsNumeric()) continue;
    const uint32_t style0 = S(sheet)->GetStyle(r0, c0);
    double x = v0.AsDouble();
    // the line's values are written at once: a cell at a time, each one below a table grew it by a row (its
    // calculated columns copied, its headers synced), and a series down a whole column took minutes
    std::vector<Value> vals;
    for (uint32_t k = 1; k < len && k < 10000000; ++k) {
      if (type == 0) {
        x += step;
      } else if (type == 1) {
        x *= step;
      } else if (unit == 0) {
        x += step;
      } else if (unit == 1) {
        // weekdays: skip Saturday and Sunday
        int n = static_cast<int>(std::max(1.0, std::fabs(step)));
        const int sgn = step < 0 ? -1 : 1;
        while (n > 0) {
          x += sgn;
          const int wd = formula::Weekday(std::floor(x));  // 1 = Sunday, 7 = Saturday
          if (wd != 1 && wd != 7) --n;
        }
      } else {
        // months, years: the k-th step from the start (like EDATE: 1/31 -> 2/28 -> 3/31)
        const double start = v0.AsDouble();
        int y = 0, mo = 0, dd = 0;
        const double frac = start - std::floor(start);
        if (formula::SerialToYmd(std::floor(start), &y, &mo, &dd)) {
          const int add = static_cast<int>(k) * (unit == 2 ? static_cast<int>(step) : static_cast<int>(step) * 12);
          const int total = y * 12 + (mo - 1) + add;
          const int ny = total / 12, nm = total % 12 + 1;
          x = formula::DateSerial(ny, nm, std::min(dd, formula::DaysInMonth(ny, nm))) + frac;
        }
      }
      if (has_stop && ((step >= 0 && x > stop) || (step < 0 && x < stop))) break;
      // past the selection only while each value comes nearer the Stop value: step 0 or a growth by 1 fills nothing
      // (Excel 16, measured), where it would have filled the sheet down to its last row
      if (to_stop && std::fabs(stop - x) >= std::fabs(stop - (vals.empty() ? v0.AsDouble() : vals.back().AsDouble()))) break;
      vals.push_back(Value::Number(x));
    }
    if (vals.empty()) continue;
    const auto n = static_cast<uint32_t>(vals.size());
    const Rect line = by_cols ? Rect{r0 + 1, c0, r0 + n, c0} : Rect{r0, c0 + 1, r0, c0 + n};
    WriteLine(*this, sheet, line, vals, by_cols);
    // date series: keep the start's number format
    if (type == 2 && style0) {
      const uint32_t fmt = wb_.styles->Get(style0).numfmt;
      if (fmt) ModifyStyle(sheet, line, [fmt](model::Style& st) { st.numfmt = fmt; });
    }
  }
}

bool Document::FillTrend(uint32_t sheet, const Rect& rect, bool by_cols, bool growth) {
  if (RefuseReadOnly(sheet)) return false;
  const Sheet* s0 = S(sheet);
  if (!s0) return false;
  const uint32_t lines = static_cast<uint32_t>(by_cols ? rect.Cols() : rect.Rows());
  const uint32_t len = static_cast<uint32_t>(by_cols ? rect.Rows() : rect.Cols());
  bool any = false;
  for (uint32_t li = 0; li < lines && li < 16384; ++li) {
    const uint32_t r0 = by_cols ? rect.r0 : rect.r0 + li, c0 = by_cols ? rect.c0 + li : rect.c0;
    // the values at the start of the line are the known points (x = 1, 2, ...), as Excel's Series > Trend reads them
    std::vector<double> ys;
    for (uint32_t k = 0; k < len && k < 1000000; ++k) {
      const Value& v = S(sheet)->GetValue(by_cols ? r0 + k : r0, by_cols ? c0 : c0 + k);
      if (!v.IsNumeric()) break;
      const double y = v.AsDouble();
      if (growth && y <= 0) break;
      ys.push_back(growth ? std::log(y) : y);
    }
    if (ys.size() < 2) continue;  // one point has no trend
    double mx = 0, my = 0;
    for (size_t i = 0; i < ys.size(); ++i) mx += static_cast<double>(i + 1), my += ys[i];
    mx /= static_cast<double>(ys.size());
    my /= static_cast<double>(ys.size());
    double sxy = 0, sxx = 0;
    for (size_t i = 0; i < ys.size(); ++i) {
      const double dx = static_cast<double>(i + 1) - mx;
      sxy += dx * (ys[i] - my);
      sxx += dx * dx;
    }
    const double b = sxx > 0 ? sxy / sxx : 0, a = my - b * mx;
    std::vector<Value> vals;  // written at once, as FillSeries does
    for (uint32_t k = 0; k < len && k < 10000000; ++k) {
      const double fit = a + b * static_cast<double>(k + 1);
      vals.push_back(Value::Number(growth ? std::exp(fit) : fit));
    }
    const auto n = static_cast<uint32_t>(vals.size());
    WriteLine(*this, sheet, by_cols ? Rect{r0, c0, r0 + n - 1, c0} : Rect{r0, c0, r0, c0 + n - 1}, vals, by_cols);
    any = true;
  }
  return any;
}

bool WildFind(std::string_view pattern, std::string_view text, size_t from, bool whole, size_t* start, size_t* len) {
  // the pattern as tokens: 0 a literal byte, 1 one character (?), 2 any run (*)
  struct Tok {
    int kind;
    char ch;
  };
  std::vector<Tok> toks;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (ch == '~' && i + 1 < pattern.size()) toks.push_back({0, pattern[++i]});
    else if (ch == '?') toks.push_back({1, 0});
    else if (ch == '*') {
      if (toks.empty() || toks.back().kind != 2) toks.push_back({2, 0});
    } else toks.push_back({0, ch});
  }
  const size_t n = text.size();
  auto char_len = [&](size_t p) {  // one UTF-8 character
    size_t q = p + 1;
    while (q < n && (static_cast<unsigned char>(text[q]) & 0xC0) == 0x80) ++q;
    return q - p;
  };
  // the ends reachable from s after all tokens (a set of positions, kept as flags)
  std::vector<char> cur(n + 1), next(n + 1);
  for (size_t s = from; s <= n; ++s) {
    std::fill(cur.begin(), cur.end(), char{0});
    cur[s] = 1;
    bool any = true;
    for (const Tok& t : toks) {
      std::fill(next.begin(), next.end(), char{0});
      any = false;
      if (t.kind == 2) {
        size_t lo = n + 1;
        for (size_t p = 0; p <= n; ++p) {
          if (cur[p]) {
            lo = p;
            break;
          }
        }
        for (size_t p = lo; p <= n; ++p) next[p] = 1, any = true;
      } else {
        for (size_t p = 0; p < n; ++p) {
          if (!cur[p]) continue;
          if (t.kind == 1) next[p + char_len(p)] = 1, any = true;
          else if (text[p] == t.ch) next[p + 1] = 1, any = true;
        }
      }
      cur.swap(next);
      if (!any) break;
    }
    if (toks.empty()) any = true;
    if (any) {
      if (whole) {
        if (cur[n]) {
          *start = s;
          *len = n - s;
          return true;
        }
      } else if (!toks.empty() && toks.back().kind == 2) {
        // a pattern ending in * runs to the end of the text
        for (size_t p = n + 1; p-- > s;) {
          if (cur[p]) {
            *start = s;
            *len = p - s;
            return true;
          }
        }
      } else {
        // otherwise the shortest match: Replace "a*e" in "Apple pie" gives "Z pie", as Excel
        for (size_t p = s; p <= n; ++p) {
          if (cur[p]) {
            *start = s;
            *len = p - s;
            return true;
          }
        }
      }
    }
    if (whole) return false;  // anchored at the start
  }
  return false;
}

// a ~ with nothing after it matches nothing in Find and Replace ("~" finds no tilde, "~~" does: Excel 16, measured)
bool UnpairedTilde(std::string_view p) {
  size_t k = 0;
  while (k < p.size() && p[p.size() - 1 - k] == '~') ++k;
  return k % 2 == 1;
}

// what Find and Replace look at in a cell's formula: a text's leading apostrophe isn't part of it (Excel searches
// '123 as 123 and keeps it text)
std::string SearchedInput(const Document& d, uint32_t sheet, uint32_t r, uint32_t c, bool* apostrophe) {
  std::string text = d.GetInputText(sheet, r, c);
  *apostrophe = !text.empty() && text[0] == '\'' && d.S(sheet)->GetValue(r, c).IsString() && !d.S(sheet)->GetFormula(r, c);
  if (*apostrophe) text.erase(0, 1);
  return text;
}

bool Document::Find(uint32_t sheet, const Rect& within_in, std::string_view what, uint32_t start_r, uint32_t start_c, bool by_rows, bool match_case, bool whole_cell,
                    bool look_in_formulas, uint32_t* out_r, uint32_t* out_c, const FormatSpec* fmt) const {
  const Sheet* s = S(sheet);
  if (fmt && !fmt->Any()) fmt = nullptr;
  if (!s || !s->has_used || (what.empty() && !fmt)) return false;
  const Rect within = ClampToUsed(*s, within_in);
  const std::string needle = match_case ? std::string(what) : Lower(what);
  const bool wild = what.find_first_of("*?~") != std::string_view::npos;
  if (wild && UnpairedTilde(what)) return false;
  // rows a filter hides are passed over, values and formulas alike (Excel 16, measured 2026-11-20: Find("AAPL") in a
  // list whose filter hides AAPL's row finds nothing); they were found
  auto filtered = [&](uint32_t r) {
    if (!s->rows->IsHidden(r)) return false;
    if (s->autofilter->hidden.count(r)) return true;
    for (const auto& tb : *s->tables) {
      if (tb.filter.hidden.count(r)) return true;
    }
    return false;
  };
  auto matches = [&](uint32_t r, uint32_t c) {
    if (filtered(r)) return false;
    // Look in Values searches what the sheet shows: a hidden row or column is passed over, where Look in Formulas
    // finds it (Excel 16, measured 2026-11-28: Find("TSMC", LookIn:=xlValues) skips D2 in a hidden column)
    if (!look_in_formulas && (s->rows->IsHidden(r) || s->cols->IsHidden(c))) return false;
    if (fmt && !FormatMatches(wb_.styles->Get(s->GetStyle(r, c)), *wb_.styles, *fmt)) return false;
    if (what.empty()) return true;  // by format only
    std::string text;
    bool apostrophe = false;
    if (look_in_formulas) text = SearchedInput(*this, sheet, r, c, &apostrophe);
    else {
      const Value& v = s->GetValue(r, c);
      if (v.IsEmpty()) return false;
      text = DisplayTextOf(v, wb_.styles->NumberFormat(wb_.styles->Get(s->GetStyle(r, c))));
    }
    if (text.empty()) return false;
    if (!match_case) text = Lower(text);
    if (wild) {
      size_t at = 0, len = 0;
      return WildFind(needle, text, 0, whole_cell, &at, &len);
    }
    return whole_cell ? text == needle : text.find(needle) != std::string::npos;
  };
  // start after start and wrap around once
  const uint64_t rows = within.Rows(), cols = within.Cols(), total = rows * cols;
  if (total > 50000000) return false;
  uint64_t start;
  if (by_rows) start = (uint64_t{std::clamp(start_r, within.r0, within.r1) - within.r0}) * cols + (std::clamp(start_c, within.c0, within.c1) - within.c0);
  else start = (uint64_t{std::clamp(start_c, within.c0, within.c1) - within.c0}) * rows + (std::clamp(start_r, within.r0, within.r1) - within.r0);
  for (uint64_t k = 1; k <= total; ++k) {
    const uint64_t i = (start + k) % total;
    uint32_t r, c;
    if (by_rows) {
      r = within.r0 + static_cast<uint32_t>(i / cols);
      c = within.c0 + static_cast<uint32_t>(i % cols);
    } else {
      c = within.c0 + static_cast<uint32_t>(i / rows);
      r = within.r0 + static_cast<uint32_t>(i % rows);
    }
    if (s->IsEmptyCell(r, c) && !(what.empty() && s->GetStyle(r, c))) continue;  // by format only: formatted empty cells too
    if (matches(r, c)) {
      *out_r = r;
      *out_c = c;
      return true;
    }
  }
  return false;
}

uint32_t Document::Replace(uint32_t sheet, const Rect& within_in, std::string_view what, std::string_view with, bool match_case, bool whole_cell,
                           const FormatSpec* find_fmt, const FormatSpec* repl_fmt) {
  if (RefuseReadOnly(sheet)) return 0;  // read-only mode: refused once (the UI tells it from "nothing found")
  const Sheet* s = S(sheet);
  if (find_fmt && !find_fmt->Any()) find_fmt = nullptr;
  if (repl_fmt && !repl_fmt->Any()) repl_fmt = nullptr;
  if (!s || !s->has_used || (what.empty() && !find_fmt && !repl_fmt)) return 0;
  if (UnpairedTilde(what)) return 0;
  const Rect within = ClampToUsed(*s, within_in);
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  ForEachValueRowMajor(*s, within, [&](uint32_t r, uint32_t c, const Value&) {
    cells.emplace_back(r, c);
    return true;
  });
  // includes formula cells too (their value may be empty)
  ForEachCellInTiles(s->meta, within, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
    if (t.formula[idx] && s->GetValue(r, c).IsEmpty()) cells.emplace_back(r, c);
  });
  if (what.empty()) {
    // by format only: the formatted empty cells too
    ForEachCellInTiles(s->meta, within, [&](uint32_t r, uint32_t c, const model::MetaTile& t, uint32_t idx) {
      if (t.style[idx] && s->IsEmptyCell(r, c)) cells.emplace_back(r, c);
    });
  }
  uint32_t count = 0;
  const std::string needle = match_case ? std::string(what) : Lower(what);
  uint32_t font_id = 0, numfmt_id = 0;
  if (repl_fmt) {
    auto& tbl = wb_.styles.mut();
    if (repl_fmt->mask & kFmFontName) font_id = repl_fmt->font.empty() || repl_fmt->font == tbl.default_font ? 0 : tbl.InternString(repl_fmt->font);
    if (repl_fmt->mask & kFmNumber) numfmt_id = repl_fmt->numfmt.empty() || repl_fmt->numfmt == "General" ? 0 : tbl.InternString(repl_fmt->numfmt);
  }
  auto reformat = [&](uint32_t r, uint32_t c) {
    if (repl_fmt) ModifyStyle(sheet, Rect::Cell(r, c), [&](model::Style& x) { ApplyFormatSpec(x, *repl_fmt, font_id, numfmt_id); });
  };
  // the wildcards Find uses (* ? and ~), so Replace changes the cells Find Next stops on. The new texts are found
  // first and written after: a locked cell among them on a protected sheet leaves every cell as it was (Excel 16,
  // measured 2026-11-20: Replace over locked A3 and unlocked C3 changes neither and raises nothing); the unlocked ones
  // were changed
  struct Change {
    uint32_t r, c;
    std::string text;
    bool format_only;
  };
  std::vector<Change> todo;
  for (auto [r, c] : cells) {
    if (find_fmt && !FormatMatches(wb_.styles->Get(S(sheet)->GetStyle(r, c)), *wb_.styles, *find_fmt)) continue;
    if (what.empty()) {
      todo.push_back(Change{r, c, std::string(), true});
      continue;
    }
    bool apostrophe = false;
    std::string text = SearchedInput(*this, sheet, r, c, &apostrophe);
    std::string cmp = match_case ? text : Lower(text);
    std::string out = apostrophe ? "'" : "";
    size_t at = 0, len = 0;
    if (whole_cell) {
      if (!WildFind(needle, cmp, 0, true, &at, &len)) continue;
      out += std::string(with);
    } else {
      size_t pos = 0, from = 0;
      bool any = false;
      while (from <= cmp.size() && WildFind(needle, cmp, from, false, &at, &len) && len > 0) {  // (a lone * at the end matches nothing more)
        any = true;
        out.append(text, pos, at - pos);
        out += with;
        from = pos = at + len;
      }
      if (!any) continue;
      out.append(text, pos, std::string::npos);
    }
    todo.push_back(Change{r, c, std::move(out), false});
  }
  if (ProtectionApplies(sheet)) {
    for (const Change& ch : todo) {
      const Rect cell = Rect::Cell(ch.r, ch.c);
      if (Refuse(sheet, &cell, ch.format_only ? Allow::FormatCells : Allow::Cells)) return 0;
    }
  }
  for (const Change& ch : todo) {
    if (ch.format_only || SetInput(sheet, ch.r, ch.c, ch.text)) {
      reformat(ch.r, ch.c);
      ++count;
    }
  }
  return count;
}

}  // namespace yc::engine
