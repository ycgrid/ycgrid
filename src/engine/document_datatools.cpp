// SPDX-License-Identifier: Apache-2.0
// document_datatools.cpp - Data tab tools built on the engine's blocks: Subtotal (Outline group), Scenario Manager's
// Merge and PivotTable report, Solver reports, Get Data > From Text/CSV into a sheet and Forecast Sheet
#include "core/i18n.h"
#include "engine/document.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"
#include "model/table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <tuple>

namespace yc::engine {

using core::Rect;
using core::Value;
using model::Sheet;

namespace {

Rect Clamp(const Sheet& s, Rect r) {
  if (!s.has_used) return r;
  r.r1 = std::min(r.r1, std::max(r.r0, s.used.r1));
  r.c1 = std::min(r.c1, std::max(r.c0, s.used.c1));
  return r;
}

// the word of a subtotal row: "East Total", "East Average"... (Excel's labels for each function)
std::string SubtotalWord(int func) {
  switch (func) {
    case 1: return TR("Average");
    case 2: return TR("Count");
    case 3: return TR("Count");
    case 4: return TR("Max");
    case 5: return TR("Min");
    case 6: return TR("Product");
    case 7: return TR("StdDev");
    case 8: return TR("StdDevp");
    case 10: return TR("Var");
    case 11: return TR("Varp");
    default: return TR("Total");
  }
}

// the grand row's label: "Grand Total", "Grand Average"... (Excel's, one phrase each so it translates whole)
std::string SubtotalGrand(int func) {
  switch (func) {
    case 1: return TR("Grand Average");
    case 2:
    case 3: return TR("Grand Count");
    case 4: return TR("Grand Max");
    case 5: return TR("Grand Min");
    case 6: return TR("Grand Product");
    case 7: return TR("Grand StdDev");
    case 8: return TR("Grand StdDevp");
    case 10: return TR("Grand Var");
    case 11: return TR("Grand Varp");
    default: return TR("Grand Total");
  }
}

bool SameKey(const Value& a, const Value& b) {
  if (a.IsEmpty() || b.IsEmpty()) return a.IsEmpty() && b.IsEmpty();
  if (a.IsString() != b.IsString() || a.IsNumeric() != b.IsNumeric()) return false;
  bool ok = true;
  return formula::Evaluator::Compare(a, b, &ok) == 0;
}

std::string Upper(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

}  // namespace

bool Document::IsSubtotalRow(uint32_t sheet, uint32_t r, uint32_t c0, uint32_t c1) const {
  const Sheet* s = S(sheet);
  if (!s) return false;
  for (uint32_t c = c0; c <= c1 && c - c0 < 1024; ++c) {
    if (!s->GetFormula(r, c)) continue;
    if (Upper(GetFormulaA1(sheet, r, c)).rfind("=SUBTOTAL(", 0) == 0) return true;
  }
  return false;
}

// ---- Subtotal ----

bool Document::Subtotal(uint32_t sheet, const Rect& list_in, uint32_t group_col, int func, const std::vector<uint32_t>& total_cols, bool replace,
                        bool page_breaks, bool summary_below, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return fail(TR("Select a cell in the list first: the list needs a header row and rows of data."));
  Rect list = Clamp(*s, list_in);
  if (list.r1 <= list.r0) return fail(TR("Select a cell in the list first: the list needs a header row and rows of data."));
  if (group_col < list.c0 || group_col > list.c1) return fail(TR("The column to group by isn't in the list."));
  std::vector<uint32_t> cols;
  for (uint32_t c : total_cols) {
    if (c >= list.c0 && c <= list.c1 && std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c); // lesson-ok: find-push the columns a person picked
  }
  if (cols.empty()) return fail(TR("Choose at least one column in Add subtotal to."));
  for (const model::Table& t : *s->tables) {
    if (t.range.Intersects(list)) return fail(TR("Subtotals can't be added to a table. Convert the table to a range first (Table Design > Convert to Range)."));
  }
  if (func < 1 || func > 11) func = 9;
  if (Refuse(sheet, &list, Allow::InsertRows)) return false;
  if (replace) list.r1 -= RemoveSubtotal(sheet, list);
  s = S(sheet);
  // the groups: runs of one value in the group column; rows of an earlier subtotal end a run (nested subtotals)
  struct Grp {
    uint32_t a, b;
    Value key;
    int level;
  };
  std::vector<Grp> groups;
  bool nested = false, open = false;
  for (uint32_t r = list.r0 + 1; r <= list.r1; ++r) {
    if (IsSubtotalRow(sheet, r, list.c0, list.c1)) {
      nested = true;
      open = false;
      continue;
    }
    const Value& v = s->GetValue(r, group_col);
    if (open && SameKey(groups.back().key, v)) {
      groups.back().b = r;
    } else {
      groups.push_back(Grp{r, r, v, s->rows->Level(r)});
      open = true;
    }
  }
  if (groups.empty()) return fail(TR("The list has no rows of data."));
  const std::string word = SubtotalWord(func), grand = SubtotalGrand(func), fn = std::to_string(func);
  const auto& styles = *wb_.styles;
  auto key_text = [&](const Grp& g) {
    return DisplayTextOf(g.key, styles.NumberFormat(styles.Get(s->GetStyle(g.a, group_col))));
  };
  std::vector<std::string> labels;
  for (const Grp& g : groups) labels.push_back(key_text(g) + " " + word);
  auto write_total = [&](uint32_t row, const std::string& label, uint32_t a, uint32_t b) {
    SetValue(sheet, row, group_col, Value::String(label));
    ModifyStyle(sheet, Rect::Cell(row, group_col), [](model::Style& st) { st.bold = true; });
    for (uint32_t c : cols) {
      if (c == group_col) continue;
      std::string e;
      SetFormula(sheet, row, c, "SUBTOTAL(" + fn + "," + core::CellA1(a, c) + ":" + core::CellA1(b, c) + ")", &e);
    }
  };
  // the rows first, bottom up so the rows of the groups above keep their numbers; then the totals at their final
  // places (a formula written before a later insertion above it would wait for a recalculation at its old place)
  // (summaries above their detail: a new row takes the formats of the detail under it, not of the last group's row
  // above, as Excel 16 does, measured)
  const uint32_t n = static_cast<uint32_t>(groups.size());
  for (uint32_t i = n; i-- > 0;) InsertRows(sheet, summary_below ? groups[i].b + 1 : groups[i].a, 1, summary_below ? 0 : 1);
  // the grand row goes under the list, or right above the grand total of an earlier subtotal kept (Replace off: the
  // list's last row), as Excel 16 does (measured: the Max grand total above the Sum one, under the last group's totals)
  const uint32_t last_detail = groups.back().b + n - 1;  // the last group's last row, once the totals are in
  const uint32_t grand_row = !summary_below ? list.r0 + 1 : nested ? list.r1 + n : list.r1 + n + 1;
  InsertRows(sheet, grand_row, 1, summary_below ? 0 : 1);
  const uint32_t shift = summary_below ? 0 : 1;  // summaries above: everything under the grand row moved down by one
  for (uint32_t i = 0; i < n; ++i) {
    const Grp& g = groups[i];
    if (summary_below) write_total(g.b + i + 1, labels[i], g.a + i, g.b + i);
    else write_total(g.a + i + shift, labels[i], g.a + i + shift + 1, g.b + i + 1 + shift);
  }
  // the grand row: below the list, or above it (under the header) when the summaries sit above their detail
  // (below: down to the last row of detail, the last group's total left out, as Excel writes it: =SUBTOTAL(9,C2:C10)
  // over a total in row 11, measured)
  // (above: from the first row of detail, the first group's total left out, as Excel writes it: =SUBTOTAL(9,C4:C12)
  // under a grand total in row 2 and the first total in row 3, measured)
  if (summary_below) write_total(grand_row, grand, list.r0 + 1, last_detail);
  else write_total(grand_row, grand, grand_row + 2, list.r1 + n + 1);
  // the outline: each group's detail one level below its summary row (Excel's levels 1 grand, 2 groups, 3 detail)
  Sheet* ms = M(sheet);
  auto& dim = ms->rows.mut();
  const uint32_t shift_all = shift;
  uint32_t first = UINT32_MAX, last = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Grp& g = groups[i];
    const uint32_t total = summary_below ? g.b + i + 1 : g.a + i + shift_all;
    const uint32_t d0 = summary_below ? g.a + i : total + 1, d1 = summary_below ? g.b + i : g.b + i + 1 + shift_all;
    const int base = g.level;
    const int tl = std::min(nested ? base : base + 1, model::DimIndex::kMaxOutline);
    const int dl = std::min(tl + 1, model::DimIndex::kMaxOutline);
    dim.SetLevel(total, tl);
    for (uint32_t r = d0; r <= d1; ++r) dim.SetLevel(r, dl);
    first = std::min(first, std::min(total, d0));
    last = std::max(last, std::max(total, d1));
  }
  NoteTouched();
  changes_.push_back(CellChange{sheet, Rect{std::min(first, grand_row), 0, std::max(last, grand_row), core::kMaxCols - 1}, kSrcUser});
  SetOutlineSettings(sheet, summary_below, ms->outline_right, ms->outline_styles);
  if (page_breaks && n > 1) {
    UpdatePageSetup(sheet, [&](model::PageSetup& ps) {
      for (uint32_t i = 0; i + 1 < n; ++i) {
        const Grp& g = groups[i];
        // a new page starts with the next group (its first row, or its summary row when summaries are above)
        const uint32_t at = summary_below ? g.b + i + 2 : groups[i + 1].a + i + 1 + shift_all;
        if (std::find(ps.row_breaks.begin(), ps.row_breaks.end(), at) == ps.row_breaks.end()) ps.row_breaks.push_back(at);
      }
      std::sort(ps.row_breaks.begin(), ps.row_breaks.end());
    });
  }
  wb_.dirty = true;
  return true;
}

uint32_t Document::RemoveSubtotal(uint32_t sheet, const Rect& list_in) {
  if (RefuseReadOnly(sheet)) return 0;  // read-only mode: the rows, the outline and the page breaks stay
  const Sheet* s = S(sheet);
  if (!s || !s->has_used) return 0;
  const Rect list = Clamp(*s, list_in);
  uint32_t removed = 0;
  for (uint32_t r = list.r1; r > list.r0; --r) {
    if (!IsSubtotalRow(sheet, r, list.c0, list.c1)) continue;
    DeleteRows(sheet, r, 1);
    ++removed;
  }
  // the list's outline and the page breaks inside it go too (Remove All)
  const uint32_t end = list.r1 - removed;
  Sheet* ms = M(sheet);
  if (!ms) return removed;
  auto& dim = ms->rows.mut();
  for (uint32_t r = list.r0; r <= end + 1; ++r) {
    if (dim.Level(r) == 0) continue;
    dim.SetLevel(r, 0);
    if (dim.IsHidden(r)) dim.Hide(r, false);
  }
  UpdatePageSetup(sheet, [&](model::PageSetup& ps) {
    ps.row_breaks.erase(std::remove_if(ps.row_breaks.begin(), ps.row_breaks.end(), [&](uint32_t b) { return b > list.r0 && b <= end + 1; }), ps.row_breaks.end());
  });
  NoteTouched();
  changes_.push_back(CellChange{sheet, Rect{list.r0, 0, end + 1, core::kMaxCols - 1}, kSrcUser});
  wb_.dirty = true;
  return removed;
}

// ---- Consolidate: the dialog's settings kept with the sheet ----

void Document::SetConsolidateSettings(uint32_t sheet, model::ConsolidateSettings co) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || *s->consolidate == co) return;
  NoteTouched();
  s->consolidate.mut() = std::move(co);
  wb_.dirty = true;
}

// ---- Scenario Manager: Merge and the PivotTable report ----

int Document::MergeScenarios(uint32_t sheet, uint32_t src_sheet) {
  if (RefuseReadOnly(sheet)) return 0;
  const Sheet* src = S(src_sheet);
  const Sheet* dst = S(sheet);
  if (!src || !dst || src_sheet == sheet) return 0;
  const std::vector<model::Scenario> from = *src->scenarios;
  int n = 0;
  for (model::Scenario sc : from) {
    // a name already used gets a number, so no scenario is lost
    auto taken = [&](const std::string& name) {
      for (const auto& x : *S(sheet)->scenarios) {
        if (Upper(x.name) == Upper(name)) return true;
      }
      return false;
    };
    if (taken(sc.name)) {
      const std::string base = sc.name;
      for (int k = 2; taken(sc.name); ++k) sc.name = base + " " + std::to_string(k);
    }
    std::string err;
    if (AddScenario(sheet, sc, &err)) ++n;
  }
  return n;
}

uint32_t Document::ScenarioPivotReport(uint32_t sheet, const std::vector<core::CellPos>& results, std::string* error) {
  if (RefuseReadOnly(sheet)) return 0;
  const Sheet* s = S(sheet);
  if (!s || s->scenarios->empty()) {
    if (error) *error = TR("The sheet has no scenarios.");
    return 0;
  }
  if (results.empty()) {
    if (error) *error = TR("Enter the result cells: a Scenario PivotTable report shows their values for each scenario.");
    return 0;
  }
  if (results.size() > 32) {
    if (error) *error = TR("A scenario summary has at most 32 result cells.");
    return 0;
  }
  const std::vector<model::Scenario> scs = *s->scenarios;
  std::vector<core::CellPos> cells;
  for (const auto& sc : scs) {
    for (const auto& p : sc.cells) {
      if (std::none_of(cells.begin(), cells.end(), [&](const core::CellPos& q) { return q.row == p.row && q.col == p.col; })) cells.push_back(p);
    }
  }
  std::vector<Value> current;
  for (const auto& p : cells) current.push_back(GetValue(sheet, p.row, p.col));
  std::vector<std::vector<Value>> res(scs.size());
  for (size_t k = 0; k < scs.size(); ++k) {
    for (size_t j = 0; j < scs[k].cells.size(); ++j) {
      const auto& p = scs[k].cells[j];
      SetValue(sheet, p.row, p.col, j < scs[k].values.size() ? scs[k].values[j] : Value(), kSrcCalc | kSrcNoEvents);
    }
    Recalc();
    for (const auto& p : results) res[k].push_back(GetValue(sheet, p.row, p.col));
    for (size_t i = 0; i < cells.size(); ++i) SetValue(sheet, cells[i].row, cells[i].col, current[i], kSrcCalc | kSrcNoEvents);
  }
  Recalc();
  int32_t index = 0;
  for (size_t i = 0; i < wb_.sheets.size(); ++i) {
    if (wb_.sheets[i]->id == sheet) index = static_cast<int32_t>(i);
  }
  const uint32_t out = AddSheet("Scenario PivotTable", index);
  if (!out) return 0;
  // the report's source: one row per scenario (the changing cells' field, then each result cell), right of the report
  std::string field;
  for (const auto& p : cells) field += (field.empty() ? "" : ",") + core::CellA1(p.row, p.col, true, true);
  field += " by";
  const uint32_t sc0 = static_cast<uint32_t>(results.size()) + 4;
  SetValue(out, 0, sc0, Value::String(field));
  for (size_t i = 0; i < results.size(); ++i) SetValue(out, 0, sc0 + 1 + static_cast<uint32_t>(i), Value::String(core::CellA1(results[i].row, results[i].col, true, true)));
  for (size_t k = 0; k < scs.size(); ++k) {
    const uint32_t r = static_cast<uint32_t>(k) + 1;
    SetValue(out, r, sc0, Value::String(scs[k].name));
    for (size_t i = 0; i < results.size(); ++i) SetValue(out, r, sc0 + 1 + static_cast<uint32_t>(i), res[k][i]);
  }
  model::PivotTable pt;
  pt.src_sheet = out;
  pt.src = Rect{0, sc0, static_cast<uint32_t>(scs.size()), sc0 + static_cast<uint32_t>(results.size())};
  pt.r0 = 2;
  pt.c0 = 0;
  pt.rows.push_back(model::PivotAxisField{});
  for (size_t i = 0; i < results.size(); ++i) {
    model::PivotDataField df;
    df.field = static_cast<int>(i) + 1;
    df.caption = core::CellA1(results[i].row, results[i].col, true, true) + " ";
    pt.data.push_back(df);
  }
  std::string err;
  if (!AddPivot(out, pt, &err)) {
    if (error) *error = err;
    return 0;
  }
  return out;
}

// ---- Get Data > From Text/CSV into a sheet ----

Rect Document::ImportRows(uint32_t sheet, uint32_t r0, uint32_t c0, const std::vector<std::vector<std::string>>& rows, const std::vector<int>& types) {
  uint32_t width = 0;
  for (const auto& row : rows) width = std::max<uint32_t>(width, static_cast<uint32_t>(row.size()));
  if (rows.empty() || !width) return Rect{r0, c0, r0, c0};
  const Rect area{r0, c0, r0 + static_cast<uint32_t>(rows.size()) - 1, std::min(c0 + width - 1, core::kMaxCols - 1)};
  if (Refuse(sheet, &area)) return Rect{r0, c0, r0, c0};
  for (size_t i = 0; i < rows.size() && r0 + i < core::kMaxRows; ++i) {
    for (size_t j = 0; j < rows[i].size() && c0 + j < core::kMaxCols; ++j) {
      const std::string& p = rows[i][j];
      const uint32_t r = r0 + static_cast<uint32_t>(i), c = c0 + static_cast<uint32_t>(j);
      if (p.empty()) continue;
      const int type = j < types.size() ? types[j] : 0;
      if (type == 1 || p[0] == '=' || p[0] == '@') SetValue(sheet, r, c, Value::String(p));  // a Text column; text that looks like a formula
      else SetInput(sheet, r, c, p);                                                        // numbers and dates are recognized
    }
  }
  return area;
}

// ---- Forecast Sheet ----

uint32_t Document::ForecastSheet(const ForecastRequest& rq, std::string* error) {
  if (RefuseReadOnly(rq.sheet)) return 0;
  auto fail = [&](const std::string& why) -> uint32_t {
    if (error) *error = why;
    return 0;
  };
  const Sheet* s = S(rq.sheet);
  if (!s) return fail(TR("The data isn't valid."));
  const uint32_t h = rq.has_header ? 1 : 0;
  if (rq.timeline.Rows() != rq.values.Rows() || rq.timeline.Rows() < 3 + h) return fail(TR("The timeline and the values must be ranges of the same size with at least 3 points."));
  std::vector<double> t, v;
  for (uint32_t i = h; i < rq.timeline.Rows(); ++i) {
    const Value& a = s->GetValue(rq.timeline.r0 + i, rq.timeline.c0);
    const Value& b = s->GetValue(rq.values.r0 + i, rq.values.c0);
    if (!a.IsNumeric()) return fail(TR("The timeline must hold numbers or dates."));
    t.push_back(a.AsDouble());
    v.push_back(b.IsNumeric() ? b.AsDouble() : NAN);
  }
  std::vector<double> sorted = t;
  std::sort(sorted.begin(), sorted.end());
  double step = 0;
  for (size_t i = 1; i < sorted.size(); ++i) {
    const double d = sorted[i] - sorted[i - 1];
    if (d > 0 && (step == 0 || d < step)) step = d;
  }
  if (step <= 0) return fail(TR("The timeline needs a constant step between its points."));
  const double last = sorted.back();
  if (rq.end <= last) return fail(TR("Forecast End must be after the last point of the timeline."));
  const size_t ahead = static_cast<size_t>(std::floor((rq.end - last) / step + 1e-9));
  if (ahead == 0 || ahead > 100000) return fail(TR("Forecast End must be at least one step after the last point of the timeline."));
  const auto& styles = *wb_.styles;
  auto header = [&](const Rect& r, const char* dflt) {
    if (!rq.has_header) return std::string(dflt);
    const std::string x = DisplayTextOf(s->GetValue(r.r0, r.c0), styles.NumberFormat(styles.Get(s->GetStyle(r.r0, r.c0))));
    return x.empty() ? std::string(dflt) : x;
  };
  const std::string th = header(rq.timeline, "Timeline"), vh = header(rq.values, "Values");
  const uint32_t t_fmt = styles.Get(s->GetStyle(rq.timeline.r0 + h, rq.timeline.c0)).numfmt;
  const uint32_t v_fmt = styles.Get(s->GetStyle(rq.values.r0 + h, rq.values.c0)).numfmt;
  // Forecast Start: the data up to it make the forecast (the default: all of it). Checked before the new sheet is
  // added: a refused forecast leaves no empty sheet behind
  const uint32_t n = static_cast<uint32_t>(t.size());
  uint32_t used = n;
  if (rq.start > 0 && rq.start < last) {
    used = 0;
    while (used < n && t[used] <= rq.start + 1e-9) ++used;
    if (used < 3) return fail(TR("Forecast Start must leave at least 3 points of data before it."));
  }
  // the new sheet goes before the data's sheet, as Excel's
  int32_t index = 0;
  for (size_t i = 0; i < wb_.sheets.size(); ++i) {
    if (wb_.sheets[i]->id == rq.sheet) index = static_cast<int32_t>(i);
  }
  const uint32_t out = AddSheet("", index);
  if (!out) return fail(TR("The new sheet couldn't be added."));
  SetValue(out, 0, 0, Value::String(th));
  SetValue(out, 0, 1, Value::String(vh));
  SetValue(out, 0, 2, Value::String("Forecast(" + vh + ")"));
  if (rq.show_ci) {
    SetValue(out, 0, 3, Value::String("Lower Confidence Bound(" + vh + ")"));
    SetValue(out, 0, 4, Value::String("Upper Confidence Bound(" + vh + ")"));
  }
  for (uint32_t i = 0; i < n; ++i) {
    SetValue(out, i + 1, 0, Value::Number(t[i]));
    if (std::isfinite(v[i])) SetValue(out, i + 1, 1, Value::Number(v[i]));
  }
  // the forecast starts at the start point (the lines meet), then one row per step up to Forecast End
  if (std::isfinite(v[used - 1])) {
    for (uint32_t c = 2; c <= (rq.show_ci ? 4u : 2u); ++c) SetValue(out, used, c, Value::Number(v[used - 1]));
  }
  const std::string vals = "$B$2:$B$" + std::to_string(used + 1), tl = "$A$2:$A$" + std::to_string(used + 1);
  const std::string tail = "," + std::to_string(rq.seasonality > 0 ? rq.seasonality : 1) + "," + (rq.fill_zeros ? "0" : "1") + "," + std::to_string(std::clamp(rq.aggregate, 1, 7)) + ")";
  char conf[32];
  std::snprintf(conf, sizeof(conf), "%g", rq.confidence);
  for (uint32_t r = used + 1; r <= n + static_cast<uint32_t>(ahead); ++r) {
    const std::string a = "A" + std::to_string(r + 1), cc = "C" + std::to_string(r + 1);
    if (r > n) SetValue(out, r, 0, Value::Number(last + step * static_cast<double>(r - n)));
    std::string e;
    SetFormula(out, r, 2, "FORECAST.ETS(" + a + "," + vals + "," + tl + tail, &e);
    if (rq.show_ci) {
      const std::string ci = "FORECAST.ETS.CONFINT(" + a + "," + vals + "," + tl + "," + conf + tail;
      SetFormula(out, r, 3, cc + "-" + ci, &e);
      SetFormula(out, r, 4, cc + "+" + ci, &e);
    }
  }
  const uint32_t last_row = n + static_cast<uint32_t>(ahead), last_col = rq.show_ci ? 4 : 2;
  if (t_fmt) ModifyStyle(out, Rect{1, 0, last_row, 0}, [t_fmt](model::Style& st) { st.numfmt = t_fmt; });
  if (v_fmt) ModifyStyle(out, Rect{1, 1, last_row, last_col}, [v_fmt](model::Style& st) { st.numfmt = v_fmt; });
  std::string terr;
  AddTable(out, Rect{0, 0, last_row, last_col}, true, "TableStyleMedium2", &terr);
  // Options > Include forecast statistics: FORECAST.ETS.STAT beside the table
  uint32_t chart_col = last_col + 2;
  if (rq.statistics) {
    const uint32_t sc = last_col + 2;
    SetValue(out, 0, sc, Value::String("Statistic"));
    SetValue(out, 0, sc + 1, Value::String("Value"));
    const char* names[] = {"Alpha", "Beta", "Gamma", "MASE", "SMAPE", "MAE", "RMSE"};
    for (int k = 0; k < 7; ++k) {
      SetValue(out, static_cast<uint32_t>(k) + 1, sc, Value::String(names[k]));
      std::string e;
      SetFormula(out, static_cast<uint32_t>(k) + 1, sc + 1, "FORECAST.ETS.STAT(" + vals + "," + tl + "," + std::to_string(k + 1) + tail, &e);
      ModifyStyle(out, Rect::Cell(static_cast<uint32_t>(k) + 1, sc + 1), [](model::Style& st) { st.numfmt = 0; });
    }
    chart_col = sc + 3;
  }
  // the chart: the values and the forecast (with its bounds) over the timeline
  const Sheet* os = S(out);
  const std::string q = core::QuoteSheetName(os ? os->name : std::string()) + "!";
  auto col_ref = [&](uint32_t c, uint32_t r0, uint32_t r1) { return q + core::CellA1(r0, c, true, true) + ":" + core::CellA1(r1, c, true, true); };
  model::ChartObj ch;
  ch.type = rq.column_chart ? model::ChartType::ColumnClustered : model::ChartType::Line;
  double left = 0;
  for (uint32_t c = 0; c < chart_col; ++c) left += os ? os->cols->Size(c) * 0.75 : 48;
  ch.left = left;
  ch.top = 0;
  ch.width = 480;
  ch.height = 288;
  const char* names[] = {nullptr, nullptr, "Forecast", "Lower Confidence Bound", "Upper Confidence Bound"};
  for (uint32_t c = 1; c <= last_col; ++c) {
    model::ChartSeries se;
    se.name = c == 1 ? vh : std::string(names[c]) + "(" + vh + ")";
    se.values = col_ref(c, 1, last_row);
    se.x_values = col_ref(0, 1, last_row);
    ch.series.push_back(se);
  }
  AddChart(out, ch);
  return out;
}

// ---- Solver reports ----

std::vector<uint32_t> Document::SolverReports(uint32_t sheet, const model::SolverModel& m, const SolverResult& res, bool answer, bool sensitivity,
                                              bool limits, std::string* error) {
  if (RefuseReadOnly(sheet)) return {};
  std::vector<uint32_t> made;
  struct Cell {
    uint32_t sheet, r, c;
  };
  auto cells_of = [&](const std::string& text, std::vector<Cell>* out) {
    std::vector<std::pair<uint32_t, Rect>> refs;
    if (!ParseRefList(text, sheet, &refs)) return false;
    for (const auto& [sh, rc] : refs) {
      for (uint32_t r = rc.r0; r <= rc.r1 && out->size() < 2000; ++r) {
        for (uint32_t c = rc.c0; c <= rc.c1; ++c) out->push_back(Cell{sh, r, c});
      }
    }
    return true;
  };
  std::vector<Cell> vars;
  if (!cells_of(m.changing, &vars) || vars.empty() || res.status == 13 || res.status == 5) {
    if (error) *error = TR("Solver didn't find a solution: there is nothing to report.");
    return made;
  }
  std::vector<Cell> obj;
  if (!m.objective.empty()) cells_of(m.objective, &obj);
  const bool has_obj = obj.size() == 1;
  // the constraints, one row per cell; int / bin / dif mark variables
  struct Row {
    Cell lhs;
    int rel;
    std::string rhs;  // a number, a reference or a formula (without '=')
  };
  std::vector<Row> rows;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int> kind;  // variable -> 4 integer, 5 binary
  bool has_int = false;
  for (const auto& con : m.constraints) {
    std::vector<Cell> lc;
    if (!cells_of(con.ref, &lc)) continue;
    if (con.rel >= 4) {
      if (m.ignore_integer) continue;
      has_int = true;
      for (const Cell& x : lc) kind[{x.sheet, x.r, x.c}] = con.rel == 5 ? 5 : 4;
      continue;
    }
    std::string rhs = con.rhs;
    while (!rhs.empty() && (rhs.front() == ' ' || rhs.front() == '=')) rhs.erase(0, 1);
    char* end = nullptr;
    std::strtod(rhs.c_str(), &end);
    std::vector<std::pair<uint32_t, Rect>> rr;
    std::vector<Cell> rc;
    if (!(end && *end == 0) && ParseRefList(rhs, sheet, &rr)) cells_of(rhs, &rc);
    for (size_t k = 0; k < lc.size(); ++k) {
      std::string side = rhs;
      if (!rc.empty()) {
        const Cell& x = rc[rc.size() == lc.size() ? k : 0];
        const Sheet* xs = S(x.sheet);
        side = (x.sheet != sheet && xs ? core::QuoteSheetName(xs->name) + "!" : std::string()) + core::CellA1(x.r, x.c, true, true);
      }
      rows.push_back(Row{lc[k], con.rel, side});
    }
  }
  if ((sensitivity || limits) && has_int) {
    if (error) *error = TR("Sensitivity and Limits Reports are not meaningful for problems with integer constraints.");
    sensitivity = limits = false;
  }
  auto num = [&](const Cell& c) {
    const Value& v = GetValue(c.sheet, c.r, c.c);
    return v.IsNumeric() ? v.AsDouble() : 0.0;
  };
  auto rhs_value = [&](const Row& row) {
    std::string e;
    const Value v = EvaluateFormula(sheet, row.rhs, &e);
    return v.IsNumeric() ? v.AsDouble() : NAN;
  };
  auto a1 = [&](const Cell& c) {
    const Sheet* cs = S(c.sheet);
    return (c.sheet != sheet && cs ? core::QuoteSheetName(cs->name) + "!" : std::string()) + core::CellA1(c.r, c.c, true, true);
  };
  // Excel's Name column: the text left of the cell and the text above it
  auto name_of = [&](const Cell& c) {
    const Sheet* cs = S(c.sheet);
    if (!cs) return std::string();
    std::string left, above;
    for (uint32_t k = c.c; k-- > 0 && c.c - k < 50;) {
      const Value& v = cs->GetValue(c.r, k);
      if (v.IsString()) {
        left = std::string(v.str());
        break;
      }
    }
    for (uint32_t k = c.r; k-- > 0 && c.r - k < 200;) {
      const Value& v = cs->GetValue(k, c.c);
      if (v.IsString()) {
        above = std::string(v.str());
        break;
      }
    }
    return left.empty() ? above : above.empty() ? left : left + " " + above;
  };
  std::vector<double> final_x;
  for (const Cell& v : vars) final_x.push_back(num(v));
  auto set_x = [&](const std::vector<double>& x) {
    for (size_t i = 0; i < vars.size() && i < x.size(); ++i) SetValue(vars[i].sheet, vars[i].r, vars[i].c, Value::Number(x[i]), kSrcCalc | kSrcNoEvents);
    Recalc();
  };
  const double final_obj = has_obj ? num(obj[0]) : 0;
  double original_obj = final_obj;
  if (has_obj && res.original.size() == vars.size()) {
    set_x(res.original);
    original_obj = num(obj[0]);
    set_x(final_x);
  }
  auto feasible_now = [&]() {
    for (const Row& row : rows) {
      const double l = num(row.lhs), r = rhs_value(row);
      const double tol = std::max(1e-9, m.precision) * std::max(1.0, std::fabs(r));
      if (!std::isfinite(r)) return false;
      if (row.rel == 1 && l > r + tol) return false;
      if (row.rel == 3 && l < r - tol) return false;
      if (row.rel == 2 && std::fabs(l - r) > tol) return false;
    }
    if (m.non_negative) {
      for (const Cell& v : vars) {
        if (num(v) < -1e-9) return false;
      }
    }
    return true;
  };
  const char* rel_text[] = {"", "<=", "=", ">="};
  int32_t index = 0;
  for (size_t i = 0; i < wb_.sheets.size(); ++i) {
    if (wb_.sheets[i]->id == sheet) index = static_cast<int32_t>(i);
  }
  const Sheet* ms = S(sheet);
  const std::string sheet_name = ms ? ms->name : std::string();
  auto new_report = [&](const std::string& title) -> uint32_t {
    std::string name;
    for (int k = 1; k < 1000; ++k) {
      name = title + " " + std::to_string(k);
      if (wb_.FindSheet(name) < 0) break;
    }
    const uint32_t id = AddSheet(name, index);
    if (!id) return 0;
    made.push_back(id);
    SetValue(id, 0, 0, Value::String("YCGrid " + title));
    SetValue(id, 1, 0, Value::String(std::string(TR("Worksheet: ")) + sheet_name));
    SetValue(id, 2, 0, Value::String(std::string(TR("Report Created: ")) + DisplayTextOf(Value::Number(formula::NowSerial()), "m/d/yyyy h:mm:ss AM/PM")));
    ModifyStyle(id, Rect::Cell(0, 0), [](model::Style& st) { st.bold = true; });
    return id;
  };
  auto head = [&](uint32_t id, uint32_t r, std::initializer_list<const char*> cols) {
    uint32_t c = 1;
    for (const char* t : cols) SetValue(id, r, c++, Value::String(t));
    ModifyStyle(id, Rect{r, 1, r, c - 1}, [](model::Style& st) {
      st.bold = true;
      st.border[model::kBottom].style = model::LineStyle::Thin;
    });
  };
  auto put = [&](uint32_t id, uint32_t r, uint32_t c, double x) { SetValue(id, r, c, std::isfinite(x) ? Value::Number(x) : Value::String("1E+30")); };
  auto text = [&](uint32_t id, uint32_t r, uint32_t c, const std::string& t) { SetValue(id, r, c, Value::String(t)); };
  const char* engines[] = {"", "GRG Nonlinear", "Simplex LP", "Evolutionary"};
  const bool lp = m.method == 2;
  if (answer) {
    const uint32_t id = new_report("Answer Report");
    if (id) {
      uint32_t r = 3;
      text(id, r++, 0, std::string(TR("Result: ")) + res.message);
      text(id, r++, 0, TR("Solver Engine"));
      text(id, r++, 1, std::string(TR("Engine: ")) + engines[std::clamp(m.method, 1, 3)]);
      text(id, r++, 1, std::string(TR("Iterations: ")) + std::to_string(res.iterations));
      text(id, r++, 0, TR("Solver Options"));
      char opt[256];
      std::snprintf(opt, sizeof(opt), "Max Time %g sec, Iterations %d, Precision %g%s", m.max_time, m.max_iterations, m.precision,
                    m.auto_scaling ? ", Use Automatic Scaling" : "");
      text(id, r++, 1, opt);
      const std::string subs = m.max_subproblems ? std::to_string(m.max_subproblems) : std::string("Unlimited");
      const std::string sols = m.max_feasible ? std::to_string(m.max_feasible) : std::string("Unlimited");
      std::snprintf(opt, sizeof(opt), "Max Subproblems %s, Max Integer Sols %s, Integer Tolerance %g%%%s", subs.c_str(), sols.c_str(), m.integer_optimality,
                    m.non_negative ? ", Assume NonNegative" : "");
      text(id, r++, 1, opt);
      ++r;
      if (has_obj) {
        const char* goal = m.goal == 1 ? "Objective Cell (Max)" : m.goal == 2 ? "Objective Cell (Min)" : "Objective Cell (Value Of)";
        text(id, r++, 0, goal);
        head(id, r++, {"Cell", "Name", "Original Value", "Final Value"});
        text(id, r, 1, a1(obj[0]));
        text(id, r, 2, name_of(obj[0]));
        put(id, r, 3, original_obj);
        put(id, r++, 4, final_obj);
        ++r;
      }
      text(id, r++, 0, TR("Variable Cells"));
      head(id, r++, {"Cell", "Name", "Original Value", "Final Value", "Integer"});
      for (size_t i = 0; i < vars.size(); ++i, ++r) {
        text(id, r, 1, a1(vars[i]));
        text(id, r, 2, name_of(vars[i]));
        put(id, r, 3, i < res.original.size() ? res.original[i] : 0);
        put(id, r, 4, final_x[i]);
        auto it = kind.find({vars[i].sheet, vars[i].r, vars[i].c});
        text(id, r, 5, it == kind.end() ? "Contin" : it->second == 5 ? "Binary" : "Integer");
      }
      ++r;
      text(id, r++, 0, TR("Constraints"));
      head(id, r++, {"Cell", "Name", "Cell Value", "Formula", "Status", "Slack"});
      for (const Row& row : rows) {
        const double l = num(row.lhs), rv = rhs_value(row);
        const double slack = std::fabs(rv - l);
        const bool binding = slack <= std::max(1e-6, m.precision) * std::max(1.0, std::fabs(rv));
        text(id, r, 1, a1(row.lhs));
        text(id, r, 2, name_of(row.lhs));
        put(id, r, 3, l);
        text(id, r, 4, a1(row.lhs) + rel_text[std::clamp(row.rel, 1, 3)] + row.rhs);
        text(id, r, 5, binding ? "Binding" : "Not Binding");
        put(id, r, 6, binding ? 0.0 : slack);
        ++r;
      }
    }
  }
  // re-solving a changed model (sensitivity): the variable cells go back to the solution afterwards
  auto solve_with = [&](const model::SolverModel& m2, double* f) {
    SolverResult r2 = Solve(sheet, m2);
    const bool ok = r2.status == 0 || r2.status == 1 || r2.status == 14;
    if (ok && f) *f = has_obj ? num(obj[0]) : 0;
    set_x(final_x);
    return ok;
  };
  if (sensitivity && has_obj) {
    const uint32_t id = new_report("Sensitivity Report");
    if (id) {
      // the model with each constraint cell on its own row, so one right side can move
      model::SolverModel base = m;
      base.constraints.clear();
      for (const auto& con : m.constraints) {
        if (con.rel >= 4) base.constraints.push_back(con);
      }
      const size_t keep = base.constraints.size();
      for (const Row& row : rows) base.constraints.push_back(model::SolverConstraint{a1(row.lhs), row.rel, row.rhs});
      auto at_bound = [&](size_t j) {
        if (m.non_negative && std::fabs(final_x[j]) <= 1e-9) return true;
        for (const Row& row : rows) {
          if (row.lhs.sheet == vars[j].sheet && row.lhs.r == vars[j].r && row.lhs.c == vars[j].c && std::fabs(num(row.lhs) - rhs_value(row)) <= 1e-9) return true;
        }
        return false;
      };
      const double h = 1e-4;
      uint32_t r = 4;
      text(id, r++, 0, TR("Variable Cells"));
      if (lp) head(id, r++, {"Cell", "Name", "Final Value", "Reduced Cost", "Objective Coefficient", "Allowable Increase", "Allowable Decrease"});
      else head(id, r++, {"Cell", "Name", "Final Value", "Reduced Gradient"});
      for (size_t j = 0; j < vars.size(); ++j, ++r) {
        text(id, r, 1, a1(vars[j]));
        text(id, r, 2, name_of(vars[j]));
        put(id, r, 3, final_x[j]);
        // the objective's slope along this variable
        std::vector<double> x = final_x;
        x[j] += h;
        set_x(x);
        const double slope = (num(obj[0]) - final_obj) / h;
        set_x(final_x);
        double reduced = 0;
        if (at_bound(j)) {
          // forced one step into the solution: how much the objective gives up
          model::SolverModel m2 = base;
          m2.constraints.push_back(model::SolverConstraint{a1(vars[j]), 3, std::to_string(final_x[j] + h)});
          double f2 = final_obj;
          reduced = solve_with(m2, &f2) ? (f2 - final_obj) / h : slope;
        }
        put(id, r, 4, std::fabs(reduced) < 1e-7 ? 0 : reduced);
        if (lp) {
          put(id, r, 5, std::fabs(slope) < 1e-9 ? 0 : slope);
          put(id, r, 6, INFINITY);
          put(id, r, 7, INFINITY);
        }
      }
      ++r;
      text(id, r++, 0, TR("Constraints"));
      if (lp) head(id, r++, {"Cell", "Name", "Final Value", "Shadow Price", "Constraint R.H. Side", "Allowable Increase", "Allowable Decrease"});
      else head(id, r++, {"Cell", "Name", "Final Value", "Lagrange Multiplier"});
      for (size_t i = 0; i < rows.size(); ++i, ++r) {
        const Row& row = rows[i];
        const double l = num(row.lhs), rv = rhs_value(row);
        text(id, r, 1, a1(row.lhs));
        text(id, r, 2, name_of(row.lhs));
        put(id, r, 3, l);
        const bool binding = std::fabs(rv - l) <= std::max(1e-6, m.precision) * std::max(1.0, std::fabs(rv));
        // the objective's change for one more unit of the right side
        auto price_at = [&](double d, double* f) {
          model::SolverModel m2 = base;
          char delta[64];
          std::snprintf(delta, sizeof(delta), "%.17g", d);
          m2.constraints[keep + i].rhs = "=(" + row.rhs + ")+(" + delta + ")";
          return solve_with(m2, f);
        };
        double price = 0;
        if (binding) {
          double f2 = final_obj;
          if (price_at(h, &f2)) price = (f2 - final_obj) / h;
        }
        put(id, r, 4, std::fabs(price) < 1e-7 ? 0 : price);
        if (lp) {
          put(id, r, 5, rv);
          // how far the right side can move with the same shadow price: doubling, then halving the step
          auto range = [&](double dir) -> double {
            if (!binding) {
              const bool open_side = (dir > 0 && row.rel == 1) || (dir < 0 && row.rel == 3);
              return open_side ? INFINITY : std::fabs(rv - l);
            }
            double good = 0, bad = INFINITY, d = std::max(1.0, std::fabs(rv)) * 0.01;
            for (int k = 0; k < 24; ++k) {
              const double test = std::isfinite(bad) ? (good + bad) / 2 : d;
              double f2 = final_obj;
              const bool same = price_at(dir * test, &f2) && std::fabs((f2 - final_obj) / (dir * test) - price) <= 1e-6 * std::max(1.0, std::fabs(price));
              if (same) good = test;
              else bad = test;
              if (!std::isfinite(bad)) {
                d *= 4;
                if (d > 1e12) return INFINITY;
              }
            }
            return good;
          };
          put(id, r, 6, range(1));
          put(id, r, 7, range(-1));
        }
      }
    }
  }
  if (limits && has_obj) {
    const uint32_t id = new_report("Limits Report");
    if (id) {
      uint32_t r = 4;
      head(id, r++, {"Cell", "Objective Name", "Value"});
      text(id, r, 1, a1(obj[0]));
      text(id, r, 2, name_of(obj[0]));
      put(id, r++, 3, final_obj);
      ++r;
      head(id, r++, {"Cell", "Variable Name", "Value", "Lower Limit", "Objective Result", "Upper Limit", "Objective Result"});
      for (size_t j = 0; j < vars.size(); ++j, ++r) {
        text(id, r, 1, a1(vars[j]));
        text(id, r, 2, name_of(vars[j]));
        put(id, r, 3, final_x[j]);
        // the variable alone moves (the others keep their values) as far as the constraints allow
        auto limit = [&](double dir, double* objective) -> double {
          std::vector<double> x = final_x;
          double good = 0, bad = INFINITY, d = std::max(1.0, std::fabs(final_x[j]));
          for (int k = 0; k < 80; ++k) {
            const double test = std::isfinite(bad) ? (good + bad) / 2 : d;
            x[j] = final_x[j] + dir * test;
            set_x(x);
            if (feasible_now()) good = test;
            else bad = test;
            if (!std::isfinite(bad)) {
              d *= 2;
              if (d > 1e15) break;
            }
            if (std::isfinite(bad) && bad - good <= 1e-9 * std::max(1.0, std::fabs(final_x[j]))) break;
          }
          x[j] = final_x[j] + dir * good;
          set_x(x);
          *objective = num(obj[0]);
          set_x(final_x);
          return std::isfinite(bad) ? x[j] : dir * INFINITY;
        };
        double fo = 0;
        const double lo = limit(-1, &fo);
        put(id, r, 4, lo);
        put(id, r, 5, fo);
        const double hi = limit(1, &fo);
        put(id, r, 6, hi);
        put(id, r, 7, fo);
      }
    }
  }
  set_x(final_x);
  return made;
}

// ---- Solver: Load / Save Model ----

namespace {

// "$B$3" from "MAX($B$3)" (case-insensitive head, no spaces)
bool Inside(const std::string& f, const char* head, std::string* out) {
  const std::string u = Upper(f), h = head;
  if (u.rfind(h + "(", 0) != 0 || u.back() != ')') return false;
  *out = f.substr(h.size() + 1, f.size() - h.size() - 2);
  return true;
}

}  // namespace

Rect Document::SaveSolverModel(uint32_t sheet, const model::SolverModel& m_in, uint32_t r0, uint32_t c0, std::string* error) {
  // Excel's layout: the objective, the variable count, one cell per constraint, then the options as an array; the
  // model's references name its sheet only when it isn't the sheet the model goes to
  const model::Sheet* own = S(sheet);
  auto local = [&](std::string t) {
    if (!own) return t;
    for (const std::string& pre : {core::QuoteSheetName(own->name) + "!", "'" + own->name + "'!", own->name + "!"}) {
      for (size_t at = t.find(pre); at != std::string::npos; at = t.find(pre, at)) {
        const bool start = at == 0 || std::string(",(=<>+-*/ ").find(t[at - 1]) != std::string::npos;
        if (start) t.erase(at, pre.size());
        else at += pre.size();
      }
    }
    return t;
  };
  model::SolverModel m = m_in;
  m.objective = local(m.objective);
  m.changing = local(m.changing);
  for (auto& c : m.constraints) c.ref = local(c.ref), c.rhs = local(c.rhs);
  std::vector<std::string> f;
  const std::string obj = m.objective.empty() ? std::string("0") : m.objective;
  char num[64];
  std::snprintf(num, sizeof(num), "%.15g", m.value);
  f.push_back(m.goal == 2 ? "MIN(" + obj + ")" : m.goal == 3 ? obj + "=" + num : "MAX(" + obj + ")");
  f.push_back("COUNT(" + (m.changing.empty() ? std::string("0") : m.changing) + ")");
  for (const auto& c : m.constraints) {
    std::string rhs = c.rhs;
    while (!rhs.empty() && (rhs.front() == '=' || rhs.front() == ' ')) rhs.erase(0, 1);
    switch (c.rel) {
      case 4: f.push_back("INT(" + c.ref + ")=" + c.ref); break;
      case 5: f.push_back("OR(" + c.ref + "=0," + c.ref + "=1)"); break;
      case 6: f.push_back("ALLDIFFERENT(" + c.ref + ")"); break;
      case 2: f.push_back(c.ref + "=" + rhs); break;
      case 3: f.push_back(c.ref + ">=" + rhs); break;
      default: f.push_back(c.ref + "<=" + rhs); break;
    }
  }
  char opts[512];
  std::snprintf(opts, sizeof(opts), "{%.15g,%d,%.15g,%d,%s,%s,%s,%.15g,%.15g,%d,%d,%d,%s,%s,%d,%d,%.15g,%.15g}", m.max_time, m.max_iterations, m.precision, m.method,
                m.non_negative ? "TRUE" : "FALSE", m.auto_scaling ? "TRUE" : "FALSE", m.ignore_integer ? "TRUE" : "FALSE", m.integer_optimality, m.convergence,
                m.derivatives, m.max_subproblems, m.max_feasible, m.multistart ? "TRUE" : "FALSE", m.require_bounds ? "TRUE" : "FALSE", m.population, m.random_seed,
                m.mutation_rate, m.max_time_no_improve);
  f.push_back(opts);
  const Rect area{r0, c0, r0 + static_cast<uint32_t>(f.size()) - 1, c0};
  if (area.r1 >= core::kMaxRows) {
    if (error) *error = TR("The model doesn't fit below this cell.");
    return Rect{};
  }
  if (Refuse(sheet, &area)) {
    if (error) *error = TR("The sheet is protected.");
    return Rect{};
  }
  for (size_t i = 0; i < f.size(); ++i) {
    const uint32_t r = r0 + static_cast<uint32_t>(i);
    std::string e;
    // the options are kept as text (an array constant would spill over the cells beside it)
    if (i + 1 == f.size()) SetValue(sheet, r, c0, Value::String(f[i]));
    else if (!SetFormula(sheet, r, c0, f[i], &e)) SetValue(sheet, r, c0, Value::String("=" + f[i]));
  }
  return area;
}

bool Document::LoadSolverModel(uint32_t sheet, const Rect& area, model::SolverModel* out, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  std::vector<std::string> f;
  for (uint32_t r = area.r0; r <= area.r1 && r - area.r0 < 2000; ++r) {
    for (uint32_t c = area.c0; c <= area.c1; ++c) {
      std::string t = GetFormulaA1(sheet, r, c);
      if (t.empty()) {
        const Value& v = GetValue(sheet, r, c);
        if (v.IsString() && !v.str().empty() && v.str()[0] == '=') t = std::string(v.str());
        if (v.IsString() && !v.str().empty() && v.str()[0] == '{') t = "=" + std::string(v.str());  // the options
      }
      if (t.empty()) continue;
      std::string x;
      for (char ch : t.substr(1)) {
        if (ch != ' ') x += ch;
      }
      f.push_back(x);
    }
  }
  if (f.size() < 2) return fail(TR("Select the cells of a saved model: the objective, the variable cells and the constraints."));
  model::SolverModel m = *out;
  m.defined = true;
  m.constraints.clear();
  std::string inner;
  if (Inside(f[0], "MAX", &inner)) {
    m.goal = 1;
    m.objective = inner;
  } else if (Inside(f[0], "MIN", &inner)) {
    m.goal = 2;
    m.objective = inner;
  } else if (const size_t eq = f[0].find('='); eq != std::string::npos) {
    m.goal = 3;
    m.objective = f[0].substr(0, eq);
    m.value = std::strtod(f[0].c_str() + eq + 1, nullptr);
  } else {
    return fail(TR("The first cell of a saved model holds its objective (=MAX(cell), =MIN(cell) or =cell=value)."));
  }
  if (m.objective == "0") m.objective.clear();
  if (!Inside(f[1], "COUNT", &inner)) return fail(TR("The second cell of a saved model counts its variable cells (=COUNT(cells))."));
  m.changing = inner == "0" ? std::string() : inner;
  for (size_t i = 2; i < f.size(); ++i) {
    const std::string& t = f[i];
    if (t.front() == '{') {
      // the options
      std::vector<std::string> o;
      std::string cur;
      for (char ch : t.substr(1, t.size() - 2)) {
        if (ch == ',' || ch == ';') {
          o.push_back(cur);
          cur.clear();
        } else {
          cur += ch;
        }
      }
      o.push_back(cur);
      auto d = [&](size_t k, double dflt) { return k < o.size() && !o[k].empty() ? std::strtod(o[k].c_str(), nullptr) : dflt; };
      auto b = [&](size_t k, bool dflt) { return k < o.size() && !o[k].empty() ? Upper(o[k]) == "TRUE" : dflt; };
      m.max_time = d(0, m.max_time);
      m.max_iterations = static_cast<int>(d(1, m.max_iterations));
      m.precision = d(2, m.precision);
      m.method = std::clamp(static_cast<int>(d(3, m.method)), 1, 3);
      m.non_negative = b(4, m.non_negative);
      m.auto_scaling = b(5, m.auto_scaling);
      m.ignore_integer = b(6, m.ignore_integer);
      m.integer_optimality = d(7, m.integer_optimality);
      m.convergence = d(8, m.convergence);
      m.derivatives = static_cast<int>(d(9, m.derivatives));
      m.max_subproblems = static_cast<int>(d(10, m.max_subproblems));
      m.max_feasible = static_cast<int>(d(11, m.max_feasible));
      m.multistart = b(12, m.multistart);
      m.require_bounds = b(13, m.require_bounds);
      m.population = static_cast<int>(d(14, m.population));
      m.random_seed = static_cast<int>(d(15, m.random_seed));
      m.mutation_rate = d(16, m.mutation_rate);
      m.max_time_no_improve = d(17, m.max_time_no_improve);
      continue;
    }
    if (Inside(t, "ALLDIFFERENT", &inner)) {
      m.constraints.push_back({inner, 6, ""});
      continue;
    }
    if (Inside(t, "OR", &inner)) {
      const size_t eq = inner.find('=');
      if (eq != std::string::npos) m.constraints.push_back({inner.substr(0, eq), 5, ""});
      continue;
    }
    if (Upper(t).rfind("INT(", 0) == 0) {
      const size_t close = t.find(')');
      if (close != std::string::npos) m.constraints.push_back({t.substr(4, close - 4), 4, ""});
      continue;
    }
    // ref<=rhs, ref>=rhs, ref=rhs
    size_t at = t.find("<=");
    int rel = 1;
    size_t len = 2;
    if (at == std::string::npos) {
      at = t.find(">=");
      rel = 3;
    }
    if (at == std::string::npos) {
      at = t.find('=');
      rel = 2;
      len = 1;
    }
    if (at == std::string::npos) return fail(TR("A cell of the saved model isn't a constraint: =") + t);
    m.constraints.push_back({t.substr(0, at), rel, t.substr(at + len)});
  }
  *out = std::move(m);
  return true;
}

}  // namespace yc::engine
