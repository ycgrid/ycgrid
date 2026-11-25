// SPDX-License-Identifier: Apache-2.0
// document_whatif.cpp - What-If Analysis and data tools: Goal Seek, Data Tables, Scenarios, Consolidate, Advanced
// Filter, Flash Fill and Solver
//
// They all work by changing input cells, recalculating and reading results, on the engine thread. Callers wrap the
// user-visible ones in Begin / Commit so Undo restores the original values.
#include "engine/document.h"
#include "core/i18n.h"
#include "engine/navigate.h"
#include "formula/criteria.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <set>

namespace yc::engine {

using core::Value;
using model::Sheet;

namespace {

constexpr double kInf = 1e300;

std::string LowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return s;
}

}  // namespace

// ---- references in text ("Sheet1!$A$1:$B$2,$C$3") ----

bool Document::ParseRefList(std::string_view text, uint32_t default_sheet, std::vector<std::pair<uint32_t, Rect>>* out) const {
  out->clear();
  size_t i = 0;
  while (i < text.size()) {
    // a comma outside quotes ends a part
    size_t j = i;
    bool quoted = false;
    for (; j < text.size(); ++j) {
      if (text[j] == '\'') quoted = !quoted;
      else if (text[j] == ',' && !quoted) break;
    }
    std::string part(text.substr(i, j - i));
    while (!part.empty() && part.front() == ' ') part.erase(0, 1);
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (!part.empty() && part[0] == '=') part.erase(0, 1);
    uint32_t sheet = default_sheet;
    const size_t bang = part.rfind('!');
    if (bang != std::string::npos) {
      std::string name = part.substr(0, bang);
      if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') name = name.substr(1, name.size() - 2);
      const int32_t id = FindSheet(name);
      if (id < 0) return false;
      sheet = static_cast<uint32_t>(id);
      part = part.substr(bang + 1);
    }
    Rect r;
    if (!core::ParseRectA1(part, &r)) {
      // a defined name
      Rect nr;
      uint32_t ns = 0;
      if (!ResolveNameRect(part, sheet, &ns, &nr)) return false;
      sheet = ns;
      r = nr;
    }
    out->emplace_back(sheet, r);
    i = j + 1;
  }
  return !out->empty();
}

bool Document::ResolveNameRect(const std::string& name, uint32_t sheet, uint32_t* out_sheet, Rect* out) const {
  return const_cast<Document*>(this)->ResolveNameRef(name, sheet, out_sheet, out);
}

// ---- Goal Seek ----

Document::GoalSeekResult Document::GoalSeek(uint32_t sheet, uint32_t set_r, uint32_t set_c, double target, uint32_t ch_sheet, uint32_t ch_r, uint32_t ch_c) {
  GoalSeekResult res;
  const Sheet* s = S(sheet);
  const Sheet* cs = S(ch_sheet);
  if (!s || !cs) return res;
  if (!s->GetFormula(set_r, set_c)) {
    res.error = TR("The Set cell must contain a formula.");
    return res;
  }
  if (cs->GetFormula(ch_r, ch_c)) {
    res.error = TR("The By changing cell must contain a value, not a formula.");
    return res;
  }
  const Rect cell = Rect::Cell(ch_r, ch_c);
  if (Refuse(ch_sheet, &cell)) {
    res.error = TR("The changing cell is protected.");
    return res;
  }
  const double tol = max_change > 0 ? max_change : 0.001;
  auto f = [&](double x) {
    SetValue(ch_sheet, ch_r, ch_c, Value::Number(x), kSrcUser);
    Recalc();
    const Value& v = GetValue(sheet, set_r, set_c);
    return v.IsNumeric() ? v.AsDouble() - target : NAN;
  };
  const Value& v0 = GetValue(ch_sheet, ch_r, ch_c);
  double x0 = v0.IsNumeric() ? v0.AsDouble() : 0;
  double f0 = f(x0);
  double best_x = x0, best_f = f0;
  if (std::isfinite(f0) && std::fabs(f0) <= tol) {
    res.found = true;
  } else {
    double x1 = x0 != 0 ? x0 * 1.01 : 0.01;
    double f1 = f(x1);
    for (int it = 0; it < 100; ++it) {
      res.iterations = it + 1;
      if (std::isfinite(f1) && std::fabs(f1) < std::fabs(best_f)) best_x = x1, best_f = f1;
      if (std::isfinite(f1) && std::fabs(f1) <= tol) {
        res.found = true;
        break;
      }
      double x2;
      if (!std::isfinite(f1)) {
        x2 = (x0 + x1) / 2;
      } else if (f1 == f0 || !std::isfinite(f0)) {
        x2 = x1 + (x1 - x0 == 0 ? 0.01 : (x1 - x0)) * 2;
      } else {
        x2 = x1 - f1 * (x1 - x0) / (f1 - f0);
      }
      if (!std::isfinite(x2)) break;
      x0 = x1;
      f0 = f1;
      x1 = x2;
      f1 = f(x1);
    }
    if (!res.found) f(best_x);  // leave the closest value found
    else best_x = x1, best_f = f1;
  }
  res.value = best_x;
  res.result = (std::isfinite(best_f) ? best_f : 0) + target;
  if (!res.found && res.error.empty()) res.error = TR("Goal Seek may not have found a solution.");
  return res;
}

// ---- Data tables ----

bool Document::AddDataTable(uint32_t sheet, const Rect& rect, const core::CellPos* row_input, const core::CellPos* col_input, std::string* error) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  if (!row_input && !col_input) {
    if (error) *error = TR("Enter a row input cell, a column input cell or both.");
    return false;
  }
  if (rect.Rows() < 2 || rect.Cols() < 2) {
    if (error) *error = TR("Select the formulas and the input values (at least two rows and two columns).");
    return false;
  }
  // its results are calculated a cell at a time and kept: B2:XFD1048576 asked for 17 billion of them at once (a bad
  // allocation); past 20,000,000 cells it is refused, as a fill or an array formula is
  if (rect.Count() > 20'000'000) {
    if (error) *error = TR("The data table is too large (at most 20,000,000 cells).");
    return false;
  }
  for (const core::CellPos* p : {row_input, col_input}) {
    if (!p) continue;
    if (rect.Contains(p->row, p->col)) {
      if (error) *error = TR("The input cell can't be inside the data table.");
      return false;
    }
    if (s->GetFormula(p->row, p->col)) {
      if (error) *error = TR("The input cell must contain a value, not a formula.");
      return false;
    }
  }
  model::DataTable t;
  t.rect = rect;
  if (row_input) {
    t.has_row = true;
    t.row_r = row_input->row;
    t.row_c = row_input->col;
  }
  if (col_input) {
    t.has_col = true;
    t.col_r = col_input->row;
    t.col_c = col_input->col;
  }
  NoteTouched();
  auto& v = s->data_tables.mut();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::DataTable& x) { return x.Body().Intersects(t.Body()); }), v.end());
  v.push_back(t);
  wb_.dirty = true;
  tables_dirty_ = true;
  RecalcDataTables();
  return true;
}

bool Document::DataTableAt(uint32_t sheet, uint32_t r, uint32_t c, model::DataTable* out) const {
  const Sheet* s = S(sheet);
  if (!s) return false;
  for (const auto& t : *s->data_tables) {
    if (t.Body().Contains(r, c)) {
      if (out) *out = t;
      return true;
    }
  }
  return false;
}

void Document::RecalcDataTables() {
  if (in_tables_) return;
  in_tables_ = true;
  tables_dirty_ = false;
  // the input cells and the body are written as a calculation: read-only mode doesn't refuse them (it counted a
  // refusal at every recalculation and the table stopped following its inputs)
  const bool refresh0 = app_refresh;
  app_refresh = true;
  for (const auto& sp : wb_.sheets) {
    const uint32_t sheet = sp->id;
    if (sp->data_tables->empty()) continue;
    const std::vector<model::DataTable> tables = *sp->data_tables;
    for (const model::DataTable& t : tables) {
      const Rect body = t.Body();
      const Value row_old = t.has_row ? GetValue(sheet, t.row_r, t.row_c) : Value();
      const Value col_old = t.has_col ? GetValue(sheet, t.col_r, t.col_c) : Value();
      std::vector<Value> out(static_cast<size_t>(body.Count()));
      auto at = [&](uint32_t r, uint32_t c) -> Value& { return out[static_cast<size_t>(r - body.r0) * body.Cols() + (c - body.c0)]; };
      const uint8_t src = kSrcCalc | kSrcNoEvents;
      if (t.has_row && t.has_col) {
        for (uint32_t r = body.r0; r <= body.r1; ++r) {
          for (uint32_t c = body.c0; c <= body.c1; ++c) {
            SetValue(sheet, t.row_r, t.row_c, GetValue(sheet, t.rect.r0, c), src);
            SetValue(sheet, t.col_r, t.col_c, GetValue(sheet, r, t.rect.c0), src);
            Recalc();
            at(r, c) = GetValue(sheet, t.rect.r0, t.rect.c0);
          }
        }
      } else if (t.has_col) {
        // input values down the left column, formulas across the top row
        for (uint32_t r = body.r0; r <= body.r1; ++r) {
          SetValue(sheet, t.col_r, t.col_c, GetValue(sheet, r, t.rect.c0), src);
          Recalc();
          for (uint32_t c = body.c0; c <= body.c1; ++c) at(r, c) = GetValue(sheet, t.rect.r0, c);
        }
      } else {
        // input values across the top row, formulas down the left column
        for (uint32_t c = body.c0; c <= body.c1; ++c) {
          SetValue(sheet, t.row_r, t.row_c, GetValue(sheet, t.rect.r0, c), src);
          Recalc();
          for (uint32_t r = body.r0; r <= body.r1; ++r) at(r, c) = GetValue(sheet, r, t.rect.c0);
        }
      }
      if (t.has_row) SetValue(sheet, t.row_r, t.row_c, row_old, src);
      if (t.has_col) SetValue(sheet, t.col_r, t.col_c, col_old, src);
      table_writing_ = true;
      SetValues(sheet, body, out, kSrcCalc);
      table_writing_ = false;
      Recalc();
    }
  }
  app_refresh = refresh0;
  tables_dirty_ = false;
  in_tables_ = false;
}

// ---- Scenarios ----

bool Document::AddScenario(uint32_t sheet, model::Scenario sc, std::string* error) {
  Sheet* s = M(sheet);
  if (!s) return false;
  if (Refuse(sheet, nullptr, Allow::Scenarios)) return false;  // a protected sheet without Edit scenarios
  if (sc.name.empty()) {
    if (error) *error = TR("Enter a scenario name.");
    return false;
  }
  if (sc.cells.empty() || sc.cells.size() > 32) {
    if (error) *error = TR("A scenario has 1 to 32 changing cells.");
    return false;
  }
  sc.values.resize(sc.cells.size());
  NoteTouched();
  auto& v = s->scenarios.mut();
  auto it = std::find_if(v.begin(), v.end(), [&](const model::Scenario& x) { return LowerAscii(x.name) == LowerAscii(sc.name); });
  if (it != v.end()) *it = std::move(sc);
  else v.push_back(std::move(sc));
  wb_.dirty = true;
  return true;
}

bool Document::DeleteScenario(uint32_t sheet, const std::string& name) {
  Sheet* s = M(sheet);
  if (!s) return false;
  if (Refuse(sheet, nullptr, Allow::Scenarios)) return false;
  auto& v = s->scenarios.mut();
  const size_t before = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::Scenario& x) { return LowerAscii(x.name) == LowerAscii(name); }), v.end());
  NoteTouched();
  wb_.dirty = true;
  return v.size() != before;
}

bool Document::ShowScenario(uint32_t sheet, const std::string& name) {
  if (RefuseReadOnly(sheet)) return false;
  const Sheet* s = S(sheet);
  if (!s) return false;
  for (const auto& sc : *s->scenarios) {
    if (LowerAscii(sc.name) != LowerAscii(name)) continue;
    for (size_t i = 0; i < sc.cells.size(); ++i) SetValue(sheet, sc.cells[i].row, sc.cells[i].col, i < sc.values.size() ? sc.values[i] : Value(), kSrcUser);
    Recalc();
    return true;
  }
  return false;
}

uint32_t Document::ScenarioSummary(uint32_t sheet, const std::vector<core::CellPos>& results, std::string* error) {
  if (RefuseReadOnly(sheet)) return 0;
  const Sheet* s = S(sheet);
  if (!s || s->scenarios->empty()) {
    if (error) *error = TR("The sheet has no scenarios.");
    return 0;
  }
  // Excel 16 takes 32 result cells (measured: 33 and more fail)
  if (results.size() > 32) {
    if (error) *error = TR("A scenario summary has at most 32 result cells.");
    return 0;
  }
  const std::vector<model::Scenario> scs = *s->scenarios;
  // every changing cell, in order
  std::vector<core::CellPos> cells;
  for (const auto& sc : scs) {
    for (const auto& p : sc.cells) {
      if (std::none_of(cells.begin(), cells.end(), [&](const core::CellPos& q) { return q.row == p.row && q.col == p.col; })) cells.push_back(p);
    }
  }
  std::vector<Value> current;
  for (const auto& p : cells) current.push_back(GetValue(sheet, p.row, p.col));
  std::vector<Value> current_results;
  for (const auto& p : results) current_results.push_back(GetValue(sheet, p.row, p.col));
  // the results of each scenario
  std::vector<std::vector<Value>> res(scs.size());
  for (size_t k = 0; k < scs.size(); ++k) {
    for (size_t i = 0; i < cells.size(); ++i) {
      const auto& sc = scs[k];
      for (size_t j = 0; j < sc.cells.size(); ++j) {
        if (sc.cells[j].row == cells[i].row && sc.cells[j].col == cells[i].col) SetValue(sheet, cells[i].row, cells[i].col, sc.values[j], kSrcCalc | kSrcNoEvents);
      }
    }
    Recalc();
    for (const auto& p : results) res[k].push_back(GetValue(sheet, p.row, p.col));
    for (size_t i = 0; i < cells.size(); ++i) SetValue(sheet, cells[i].row, cells[i].col, current[i], kSrcCalc | kSrcNoEvents);
  }
  Recalc();
  // the report sheet, before the scenario sheet
  int32_t index = 0;
  for (size_t i = 0; i < wb_.sheets.size(); ++i) {
    if (wb_.sheets[i]->id == sheet) index = static_cast<int32_t>(i);
  }
  const uint32_t out = AddSheet("Scenario Summary", index);
  auto set = [&](uint32_t r, uint32_t c, const Value& v) { SetValue(out, r, c, v, kSrcUser); };
  auto text = [&](uint32_t r, uint32_t c, const std::string& t) { set(r, c, Value::String(t)); };
  text(0, 1, "Scenario Summary");
  text(2, 2, "Current Values:");
  for (size_t k = 0; k < scs.size(); ++k) text(2, static_cast<uint32_t>(3 + k), scs[k].name);
  text(3, 1, "Changing Cells:");
  uint32_t row = 4;
  for (size_t i = 0; i < cells.size(); ++i, ++row) {
    text(row, 1, core::CellA1(cells[i].row, cells[i].col, true, true));
    set(row, 2, current[i]);
    for (size_t k = 0; k < scs.size(); ++k) {
      Value v;
      for (size_t j = 0; j < scs[k].cells.size(); ++j) {
        if (scs[k].cells[j].row == cells[i].row && scs[k].cells[j].col == cells[i].col) v = scs[k].values[j];
      }
      set(row, static_cast<uint32_t>(3 + k), v);
    }
  }
  if (!results.empty()) {
    text(row++, 1, "Result Cells:");
    for (size_t i = 0; i < results.size(); ++i, ++row) {
      text(row, 1, core::CellA1(results[i].row, results[i].col, true, true));
      set(row, 2, current_results[i]);
      for (size_t k = 0; k < scs.size(); ++k) set(row, static_cast<uint32_t>(3 + k), res[k][i]);
    }
  }
  text(row + 1, 1, "Notes: Current Values column represents values of changing cells at");
  text(row + 2, 1, "time Scenario Summary Report was created. Changing cells for each");
  text(row + 3, 1, "scenario are highlighted in gray.");
  // header style
  ModifyStyle(out, Rect{2, 1, 2, static_cast<uint32_t>(2 + scs.size())}, [](model::Style& x) {
    x.bold = true;
    x.fill_color = 0xFF808080;
    x.font_color = 0xFFFFFFFF;
  });
  ModifyStyle(out, Rect{0, 1, 0, 1}, [](model::Style& x) {
    x.bold = true;
    x.font_size = 14;
  });
  ModifyStyle(out, Rect{4, 3, 3 + static_cast<uint32_t>(cells.size()), static_cast<uint32_t>(2 + scs.size())}, [](model::Style& x) { x.fill_color = 0xFFD9D9D9; });
  return out;
}

// ---- Consolidate ----

bool Document::Consolidate(uint32_t dest_sheet, uint32_t dr, uint32_t dc, const std::vector<std::pair<uint32_t, Rect>>& sources, int func, bool top_row,
                           bool left_col, std::string* error, bool create_links) {
  if (sources.empty()) {
    if (error) *error = TR("Add at least one reference.");
    return false;
  }
  // what Worksheet.ConsolidationFunction / Options / Sources read back (Excel keeps the last one on the sheet)
  last_consolidation[dest_sheet] = LastConsolidation{func, top_row, left_col, create_links, sources};
  if (create_links) {
    for (const auto& src : sources) {
      if (src.first == dest_sheet) {
        if (error) *error = TR("Links can't be created to source areas on the same sheet as the destination area.");
        return false;
      }
    }
  }
  // labels -> slot; values per (row slot, col slot), with the cell each came from (Create links to source data)
  struct Src {
    Value v;
    uint32_t sheet, r, c;
    size_t source;
  };
  std::vector<std::string> row_labels, col_labels;
  std::map<std::string, size_t> row_index, col_index;
  std::map<std::pair<size_t, size_t>, std::vector<Src>> cells;
  auto slot = [](std::vector<std::string>& labels, std::map<std::string, size_t>& idx, const std::string& key) {
    auto it = idx.find(LowerAscii(key));
    if (it != idx.end()) return it->second;
    idx[LowerAscii(key)] = labels.size();
    labels.push_back(key);
    return labels.size() - 1;
  };
  for (size_t si = 0; si < sources.size(); ++si) {
    const auto& [sh, rect0] = sources[si];
    const Sheet* s = S(sh);
    if (!s) continue;
    Rect rect = rect0;
    if (s->has_used) {
      rect.r1 = std::min(rect.r1, std::max(rect.r0, s->used.r1));
      rect.c1 = std::min(rect.c1, std::max(rect.c0, s->used.c1));
    }
    const uint32_t r0 = rect.r0 + (top_row ? 1 : 0), c0 = rect.c0 + (left_col ? 1 : 0);
    for (uint32_t r = r0; r <= rect.r1; ++r) {
      const std::string rl = left_col ? formula::FormatValue(s->GetValue(r, rect.c0), "General").text : std::to_string(r - r0);
      if (left_col && rl.empty()) continue;
      for (uint32_t c = c0; c <= rect.c1; ++c) {
        const std::string cl = top_row ? formula::FormatValue(s->GetValue(rect.r0, c), "General").text : std::to_string(c - c0);
        if (top_row && cl.empty()) continue;
        const size_t ri = slot(row_labels, row_index, rl), ci = slot(col_labels, col_index, cl);
        cells[{ri, ci}].push_back(Src{s->GetValue(r, c), sh, r, c, si});
      }
    }
  }
  auto aggregate = [&](const std::vector<Src>& vs) -> Value {
    std::vector<double> n;
    size_t nonblank = 0;
    for (const Src& x : vs) {
      if (!x.v.IsEmpty()) ++nonblank;
      if (x.v.IsNumeric()) n.push_back(x.v.AsDouble());
    }
    const double sum = std::accumulate(n.begin(), n.end(), 0.0);
    auto var = [&](bool sample) {
      const double m = sum / static_cast<double>(n.size());
      double s2 = 0;
      for (double x : n) s2 += (x - m) * (x - m);
      return s2 / static_cast<double>(sample ? n.size() - 1 : n.size());
    };
    // no number to work with leaves the cell blank, as Excel 16 does (measured: a label's cells that are empty or
    // text give a blank Sum, Average, Max... and Count Numbers); Count counts what isn't empty
    if (func == 1) return nonblank ? Value::Number(static_cast<double>(nonblank)) : Value();
    if (n.empty()) return Value();
    switch (func) {
      case 1: return Value::Number(static_cast<double>(nonblank));                                 // Count
      case 2: return n.empty() ? Value::Error(core::Err::Div0) : Value::Number(sum / static_cast<double>(n.size()));  // Average
      case 3: return n.empty() ? Value::Number(0) : Value::Number(*std::max_element(n.begin(), n.end()));
      case 4: return n.empty() ? Value::Number(0) : Value::Number(*std::min_element(n.begin(), n.end()));
      case 5: {
        double p = n.empty() ? 0 : 1;
        for (double x : n) p *= x;
        return Value::Number(p);
      }
      case 6: return Value::Number(static_cast<double>(n.size()));                                 // Count Numbers
      case 7: return n.size() > 1 ? Value::Number(std::sqrt(var(true))) : Value::Error(core::Err::Div0);
      case 8: return !n.empty() ? Value::Number(std::sqrt(var(false))) : Value::Error(core::Err::Div0);
      case 9: return n.size() > 1 ? Value::Number(var(true)) : Value::Error(core::Err::Div0);
      case 10: return !n.empty() ? Value::Number(var(false)) : Value::Error(core::Err::Div0);
      default: return Value::Number(sum);                                                          // Sum
    }
  };
  if (row_labels.empty() || col_labels.empty()) {
    if (error) *error = TR("The references contain no data.");
    return false;
  }
  const uint32_t lead = left_col ? 1 : 0;
  if (!create_links) {
    const uint32_t out_rows = static_cast<uint32_t>(row_labels.size()) + (top_row ? 1 : 0);
    const uint32_t out_cols = static_cast<uint32_t>(col_labels.size()) + lead;
    const Rect dest{dr, dc, dr + out_rows - 1, dc + out_cols - 1};
    if (Refuse(dest_sheet, &dest)) return false;
    std::vector<Value> out(static_cast<size_t>(dest.Count()));
    auto at = [&](uint32_t r, uint32_t c) -> Value& { return out[static_cast<size_t>(r) * out_cols + c]; };
    if (top_row) {
      for (size_t j = 0; j < col_labels.size(); ++j) at(0, static_cast<uint32_t>(j + lead)) = Value::String(col_labels[j]);
    }
    for (size_t i = 0; i < row_labels.size(); ++i) {
      const uint32_t r = static_cast<uint32_t>(i + (top_row ? 1 : 0));
      if (left_col) at(r, 0) = Value::String(row_labels[i]);
      for (size_t j = 0; j < col_labels.size(); ++j) {
        auto it = cells.find({i, j});
        if (it != cells.end()) at(r, static_cast<uint32_t>(j + lead)) = aggregate(it->second);
      }
    }
    SetValues(dest_sheet, dest, out, kSrcUser);
    return true;
  }
  // Create links to source data, as Excel: under each label, one hidden row per source holding links to its cells
  // (with the workbook's name in an extra column), then the summary row of formulas over them; the detail rows are
  // grouped and collapsed
  static const char* kFuncs[] = {"SUM", "COUNTA", "AVERAGE", "MAX", "MIN", "PRODUCT", "COUNT", "STDEV", "STDEVP", "VAR", "VARP"};
  const std::string fname = kFuncs[std::clamp(func, 0, 10)];
  std::string book = wb_.path;
  if (const size_t slash = book.find_last_of("/\\"); slash != std::string::npos) book = book.substr(slash + 1);
  if (const size_t dot = book.find_last_of('.'); dot != std::string::npos && dot > 0) book = book.substr(0, dot);
  if (book.empty()) book = "Book1";
  const uint32_t first_val = lead + 1;  // the book column comes after the labels
  uint32_t out_rows = top_row ? 1 : 0;
  for (size_t i = 0; i < row_labels.size(); ++i) {
    std::vector<char> used(sources.size(), 0);
    for (size_t j = 0; j < col_labels.size(); ++j) {
      auto it = cells.find({i, j});
      if (it == cells.end()) continue;
      for (const Src& x : it->second) used[x.source] = 1;
    }
    out_rows += static_cast<uint32_t>(std::count(used.begin(), used.end(), 1)) + 1;
  }
  const Rect dest{dr, dc, dr + out_rows - 1, dc + first_val + static_cast<uint32_t>(col_labels.size()) - 1};
  if (Refuse(dest_sheet, &dest)) return false;
  ClearContents(dest_sheet, dest, kSrcUser);
  uint32_t r = dr;
  if (top_row) {
    for (size_t j = 0; j < col_labels.size(); ++j) SetValue(dest_sheet, r, dc + first_val + static_cast<uint32_t>(j), Value::String(col_labels[j]));
    ++r;
  }
  for (size_t i = 0; i < row_labels.size(); ++i) {
    const uint32_t d0 = r;
    for (size_t si = 0; si < sources.size(); ++si) {
      bool any = false;
      for (size_t j = 0; j < col_labels.size(); ++j) {
        auto it = cells.find({i, j});
        if (it == cells.end()) continue;
        for (const Src& x : it->second) {
          if (x.source != si) continue;
          const Sheet* xs = S(x.sheet);
          std::string e;
          SetFormula(dest_sheet, r, dc + first_val + static_cast<uint32_t>(j),
                     (xs ? core::QuoteSheetName(xs->name) + "!" : std::string()) + core::CellA1(x.r, x.c, true, true), &e, kSrcUser);
          any = true;
        }
      }
      if (any) {
        SetValue(dest_sheet, r, dc + lead, Value::String(book));
        ++r;
      }
    }
    const uint32_t d1 = r - 1;
    if (left_col) SetValue(dest_sheet, r, dc, Value::String(row_labels[i]));
    for (size_t j = 0; j < col_labels.size(); ++j) {
      if (cells.find({i, j}) == cells.end() || d1 < d0) continue;
      const uint32_t c = dc + first_val + static_cast<uint32_t>(j);
      std::string e;
      SetFormula(dest_sheet, r, c, fname + "(" + core::CellA1(d0, c) + ":" + core::CellA1(d1, c) + ")", &e, kSrcUser);
    }
    if (d1 >= d0) {
      GroupOutline(dest_sheet, true, d0, d1, true);
      SetGroupCollapsed(dest_sheet, true, d0, S(dest_sheet)->rows->Level(d0), true);
    }
    ++r;
  }
  return true;
}

// ---- Advanced Filter ----

namespace {

std::shared_ptr<formula::Array> ReadArray(const Sheet& s, const Rect& r) {
  auto a = std::make_shared<formula::Array>();
  a->rows = static_cast<uint32_t>(r.Rows());
  a->cols = static_cast<uint32_t>(r.Cols());
  a->v.reserve(static_cast<size_t>(r.Count()));
  for (uint32_t i = r.r0; i <= r.r1; ++i) {
    for (uint32_t j = r.c0; j <= r.c1; ++j) a->v.push_back(s.GetValue(i, j));
  }
  return a;
}

}  // namespace

bool Document::AdvancedFilter(uint32_t sheet, const Rect& list0, uint32_t crit_sheet, const Rect& criteria0, bool copy, uint32_t dest_sheet, const Rect& dest,
                              bool unique, std::string* error) {
  const Sheet* s = S(sheet);
  const Sheet* cs = S(crit_sheet);
  if (!s) return false;
  Rect list = list0;
  if (list.IsCell()) list = CurrentRegion(*s, list.r0, list.c0);
  if (s->has_used) list.r1 = std::min(list.r1, std::max(list.r0, s->used.r1));
  if (list.Rows() < 2) {
    if (error) *error = TR("The list range needs a header row and data.");
    return false;
  }
  // the list is read whole: the whole sheet with a value in its last cell is 17 billion cells (scale_sweep: the OK
  // never answered), so a list past 20,000,000 cells is refused, as a copy is
  if (list.c1 >= s->col_limit) list.c1 = s->col_limit - 1;
  if (list.Count() > 20000000) {
    if (error) *error = TR("The list range is too large (at most 20,000,000 cells).");
    return false;
  }
  const auto data = ReadArray(*s, list);
  std::vector<char> match(data->rows - 1, 1);
  if (cs && criteria0.Rows() >= 1 && !(criteria0.r0 == 0 && criteria0.c0 == 0 && criteria0.r1 == 0 && criteria0.c1 == 0 && crit_sheet == 0)) {
    Rect criteria = criteria0;
    if (cs->has_used) criteria.r1 = std::min(criteria.r1, std::max(criteria.r0, cs->used.r1));
    if (criteria.c1 >= cs->col_limit) criteria.c1 = cs->col_limit - 1;
    if (criteria.Count() > 20000000) {  // read whole too, as the list
      if (error) *error = TR("The list range is too large (at most 20,000,000 cells).");
      return false;
    }
    const auto crit = ReadArray(*cs, criteria);
    // a computed criterion: a formula under a label that isn't one of the list's (or none), worked out for every
    // record with its relative references moved to that record, the first record being the one it names (Excel 16,
    // measured: Big / =C2*D2>40000 keeps the rows whose Qty x Px passes). Each criteria row is matched apart: its
    // other conditions and its formulas all hold.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> computed(crit->rows);  // per criteria row: (row, col) of formulas
    bool any_computed = false;
    for (uint32_t i = 1; i < crit->rows; ++i) {
      for (uint32_t j = 0; j < crit->cols; ++j) {
        const uint32_t cr = criteria.r0 + i, cc = criteria.c0 + j;
        if (!cs->GetFormula(cr, cc)) continue;
        const std::string label = LowerAscii(formula::FormatValue(crit->At(0, j), "General").text);
        bool is_column = false;
        for (uint32_t k = 0; k < data->cols && !label.empty(); ++k) {
          is_column = is_column || LowerAscii(formula::FormatValue(data->At(0, k), "General").text) == label;
        }
        if (is_column) continue;
        computed[i].emplace_back(cr, cc);
        any_computed = true;
      }
    }
    if (!any_computed) {
      match = formula::MatchCriteriaRows(*data, *crit);
    } else {
      Recalc();
      std::fill(match.begin(), match.end(), static_cast<char>(0));
      for (uint32_t i = 1; i < crit->rows; ++i) {
        formula::Array one;
        one.rows = 2;
        one.cols = crit->cols;
        one.v.resize(static_cast<size_t>(2) * crit->cols);
        for (uint32_t j = 0; j < crit->cols; ++j) {
          one.v[j] = crit->At(0, j);
          const bool is_formula = std::any_of(computed[i].begin(), computed[i].end(), [&](const auto& p) { return p.second == criteria.c0 + j; });
          if (!is_formula) one.v[crit->cols + j] = crit->At(i, j);
        }
        std::vector<char> m = formula::MatchCriteriaRows(*data, one);
        for (uint32_t k = 0; k + 1 < data->rows; ++k) {
          if (!m[k]) continue;
          for (const auto& [cr, cc] : computed[i]) {
            const formula::CompiledFormula* f = Formula(cs->GetFormula(cr, cc));
            const Value v = f ? EvaluateAt(crit_sheet, f->root, core::CellPos{cr + k, cc}) : Value();
            bool ok = false;
            core::Err e;
            if (!formula::Evaluator::ToBool(v, &ok, &e) || !ok || v.IsString()) {
              m[k] = 0;
              break;
            }
          }
          if (m[k]) match[k] = 1;
        }
      }
    }
  }
  // the columns to copy: the labels in the first row of the destination, or every column
  std::vector<uint32_t> cols;
  if (copy) {
    const Sheet* ds = S(dest_sheet);
    if (!ds) return false;
    for (uint32_t c = dest.c0; c <= dest.c1 && dest.Cols() > 1; ++c) {
      const std::string label = LowerAscii(formula::FormatValue(ds->GetValue(dest.r0, c), "General").text);
      if (label.empty()) continue;
      for (uint32_t k = 0; k < data->cols; ++k) {
        if (LowerAscii(formula::FormatValue(data->At(0, k), "General").text) == label) cols.push_back(k);
      }
    }
  }
  if (cols.empty()) {
    for (uint32_t k = 0; k < data->cols; ++k) cols.push_back(k);
  }
  if (unique) {
    std::set<std::string> seen;
    for (uint32_t r = 1; r < data->rows; ++r) {
      if (!match[r - 1]) continue;
      std::string key;
      for (uint32_t k : cols) key += LowerAscii(formula::FormatValue(data->At(r, k), "General").text) + '\x1F';
      if (!seen.insert(key).second) match[r - 1] = 0;
    }
  }
  if (!copy) {
    // filter in place: hide the rows that don't match (ShowAllData shows them again)
    if (Refuse(sheet, nullptr, Allow::AutoFilter)) return false;
    ShowAllData(sheet);
    Sheet* ms = M(sheet);
    auto& af = ms->autofilter.mut();
    auto& dim = ms->rows.mut();
    for (uint32_t r = 1; r < data->rows; ++r) {
      if (match[r - 1]) continue;
      const uint32_t row = list.r0 + r;
      af.hidden[row] = static_cast<int32_t>(dim.Size(row));
      dim.Hide(row, true);
    }
    af.adv_range = list;
    MarkChanged(sheet, Rect{list.r0, 0, list.r1, core::kMaxCols - 1}, kSrcCalc | kSrcNoEvents);
    NoteTouched();
    wb_.dirty = true;
    return true;
  }
  std::vector<Value> out;
  uint32_t rows = 0;
  for (uint32_t r = 0; r < data->rows; ++r) {
    if (r > 0 && !match[r - 1]) continue;
    for (uint32_t k : cols) out.push_back(data->At(r, k));
    ++rows;
  }
  const Rect target{dest.r0, dest.c0, dest.r0 + rows - 1, dest.c0 + static_cast<uint32_t>(cols.size()) - 1};
  if (Refuse(dest_sheet, &target)) return false;
  // clear what an earlier copy left below
  if (const Sheet* ds = S(dest_sheet); ds && ds->has_used && ds->used.r1 > target.r1) ClearContents(dest_sheet, Rect{target.r1 + 1, target.c0, ds->used.r1, target.c1}, kSrcUser);
  SetValues(dest_sheet, target, out, kSrcUser);
  // the rows keep their formats (number formats, fonts, fills), as Excel copies them
  const ClipData all = Copy(sheet, list);
  ClipData fmt;
  fmt.rows = rows;
  fmt.cols = static_cast<uint32_t>(cols.size());
  fmt.src_sheet = sheet;
  fmt.src = list;
  for (uint32_t r = 0; r < data->rows && r < all.rows; ++r) {
    if (r > 0 && !match[r - 1]) continue;
    for (uint32_t k : cols) fmt.cells.push_back(all.cells[static_cast<size_t>(r) * all.cols + k]);
  }
  if (fmt.cells.size() == static_cast<size_t>(fmt.rows) * fmt.cols) Paste(dest_sheet, target.r0, target.c0, fmt, PasteMode::Formats);
  return true;
}

// ---- Flash Fill ----

namespace {

struct FfPart {
  int kind = 0;  // 0 constant, 1 token, 2 whole cell
  std::string text;
  int col = 0, index = 0;  // token index (negative = from the end)
  int xf = 0;              // 0 as is, 1 upper, 2 lower, 3 proper, 4 first character
};
using FfProgram = std::vector<FfPart>;

std::vector<std::string> Tokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto word = [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x80; };
  for (unsigned char c : s) {
    if (word(c)) {
      cur += static_cast<char>(c);
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string Xf(const std::string& s, int xf) {
  std::string o = s;
  switch (xf) {
    case 1:
      for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      break;
    case 2:
      for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      break;
    case 3: {
      bool start = true;
      for (char& c : o) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalpha(u)) {
          c = static_cast<char>(start ? std::toupper(u) : std::tolower(u));
          start = false;
        } else {
          start = true;
        }
      }
      break;
    }
    case 4: {
      // the first character (a whole UTF-8 sequence)
      if (o.empty()) return o;
      size_t n = 1;
      while (n < o.size() && (static_cast<unsigned char>(o[n]) & 0xC0) == 0x80) ++n;
      o = o.substr(0, n);
      break;
    }
    default: break;
  }
  return o;
}

bool RunProgram(const FfProgram& p, const std::vector<std::string>& src, std::string* out) {
  out->clear();
  for (const FfPart& part : p) {
    if (part.kind == 0) {
      *out += part.text;
      continue;
    }
    if (part.col < 0 || static_cast<size_t>(part.col) >= src.size()) return false;
    if (part.kind == 2) {
      if (src[static_cast<size_t>(part.col)].empty()) return false;
      *out += Xf(src[static_cast<size_t>(part.col)], part.xf);
      continue;
    }
    const auto toks = Tokens(src[static_cast<size_t>(part.col)]);
    const int n = static_cast<int>(toks.size());
    const int k = part.index >= 0 ? part.index : n + part.index;
    if (k < 0 || k >= n) return false;
    *out += Xf(toks[static_cast<size_t>(k)], part.xf);
  }
  return true;
}

// every program that turns src into out (constants merged), at most `limit`
void Synthesize(const std::string& out, const std::vector<std::string>& src, size_t limit, std::vector<FfProgram>* programs) {
  struct Cand {
    FfPart part;
    size_t len;
  };
  std::vector<std::vector<Cand>> at(out.size());
  for (size_t i = 0; i < out.size(); ++i) {
    for (int col = 0; col < static_cast<int>(src.size()); ++col) {
      const auto toks = Tokens(src[static_cast<size_t>(col)]);
      const int n = static_cast<int>(toks.size());
      for (int xf = 0; xf <= 4; ++xf) {
        const std::string whole = Xf(src[static_cast<size_t>(col)], xf);
        if (xf != 4 && !whole.empty() && out.compare(i, whole.size(), whole) == 0) at[i].push_back(Cand{FfPart{2, {}, col, 0, xf}, whole.size()});
        for (int k = 0; k < n; ++k) {
          const std::string t = Xf(toks[static_cast<size_t>(k)], xf);
          if (t.empty() || out.compare(i, t.size(), t) != 0) continue;
          at[i].push_back(Cand{FfPart{1, {}, col, k, xf}, t.size()});
          at[i].push_back(Cand{FfPart{1, {}, col, k - n, xf}, t.size()});
        }
      }
    }
  }
  FfProgram cur;
  std::function<void(size_t)> walk = [&](size_t i) {
    if (programs->size() >= limit) return;
    if (i == out.size()) {
      programs->push_back(cur);
      return;
    }
    for (const Cand& c : at[i]) {
      cur.push_back(c.part);
      walk(i + c.len);
      cur.pop_back();
    }
    // one constant character (merged into a preceding constant)
    size_t n = 1;
    while (i + n < out.size() && (static_cast<unsigned char>(out[i + n]) & 0xC0) == 0x80) ++n;
    const bool merge = !cur.empty() && cur.back().kind == 0;
    if (merge) cur.back().text += out.substr(i, n);
    else cur.push_back(FfPart{0, out.substr(i, n), 0, 0, 0});
    walk(i + n);
    if (merge) cur.back().text.resize(cur.back().text.size() - n);
    else cur.pop_back();
  };
  walk(0);
}

double Score(const FfProgram& p) {
  double s = 0;
  for (const auto& part : p) {
    s += 1;  // fewer parts
    if (part.kind == 0) s += 2.0 * static_cast<double>(part.text.size());
    if (part.kind == 1 && part.index < 0) s += 0.1;  // prefer counting from the start
    if (part.xf) s += 0.05;
  }
  return s;
}

}  // namespace

uint32_t Document::FlashFill(uint32_t sheet, uint32_t r, uint32_t c, std::string* error, std::vector<std::pair<uint32_t, std::string>>* filled_cells,
                             bool dry_run) {
  const Sheet* s = S(sheet);
  if (!s) return 0;
  // the data next to the column: the current region around it (including the columns on both sides)
  Rect region = CurrentRegion(*s, r, c);
  if (c > 0 && region.c0 == c) region = CurrentRegion(*s, r, c - 1).Union(region);
  if (region.c0 > c || region.c1 < c) region = region.Union(Rect::Cell(r, c));
  // the live preview, asked as a person types, reads the rows around the entry only: over a region of many rows
  // (each cell made text) it kept the engine a minute, and the typing behind it waited (chaos seed 6821). Ctrl+E
  // itself reads the whole region
  if (dry_run) {
    constexpr uint32_t kAround = 5000;
    region.r0 = std::max(region.r0, r > kAround ? r - kAround : 0u);
    region.r1 = std::min(region.r1, r + kAround);
    constexpr uint32_t kBeside = 64;  // and the columns beside it (a region as wide as the sheet: 16,384 a row)
    region.c0 = std::max(region.c0, c > kBeside ? c - kBeside : 0u);
    region.c1 = std::min(region.c1, c + kBeside);
  }
  std::vector<uint32_t> src_cols;
  for (uint32_t k = region.c0; k <= region.c1; ++k) {
    if (k != c) src_cols.push_back(k);
  }
  if (src_cols.empty()) {
    if (error) *error = TR("Flash Fill needs data in the columns next to it.");
    return 0;
  }
  auto text = [&](uint32_t row, uint32_t col) {
    const auto& st = wb_.styles->Get(s->GetStyle(row, col));
    return formula::FormatValue(s->GetValue(row, col), wb_.styles->NumberFormat(st)).text;
  };
  struct Row {
    uint32_t r;
    std::vector<std::string> src;
    std::string out;
  };
  std::vector<Row> examples, todo;
  for (uint32_t row = region.r0; row <= region.r1; ++row) {
    Row x{row, {}, text(row, c)};
    bool any = false;
    for (uint32_t k : src_cols) {
      x.src.push_back(text(row, k));
      any = any || !x.src.back().empty();
    }
    if (!any) continue;
    if (!x.out.empty()) examples.push_back(std::move(x));
    else todo.push_back(std::move(x));
  }
  if (examples.empty() || todo.empty()) {
    if (error) *error = TR("Type an example of what you want in the first cell, then use Flash Fill.");
    return 0;
  }
  // the first example could be a header: try with it, then without
  for (size_t first = 0; first < std::min<size_t>(2, examples.size()); ++first) {
    std::vector<FfProgram> programs;
    Synthesize(examples[first].out, examples[first].src, 4000, &programs);
    std::sort(programs.begin(), programs.end(), [](const FfProgram& a, const FfProgram& b) { return Score(a) < Score(b); });
    for (const FfProgram& p : programs) {
      bool ok = true;
      std::string o;
      for (size_t e = first; e < examples.size() && ok; ++e) ok = RunProgram(p, examples[e].src, &o) && o == examples[e].out;
      if (!ok) continue;
      // a program made only of constants copies the example: no pattern
      if (std::all_of(p.begin(), p.end(), [](const FfPart& x) { return x.kind == 0; })) continue;
      uint32_t filled = 0;
      for (const Row& t : todo) {
        if (t.r < examples[first].r) continue;
        if (!RunProgram(p, t.src, &o)) continue;
        const Rect cell = Rect::Cell(t.r, c);
        if (filled_cells) filled_cells->emplace_back(t.r, o);
        ++filled;
        if (dry_run) continue;  // the live preview: only what would be filled
        if (Refuse(sheet, &cell)) return filled - 1;
        SetInput(sheet, t.r, c, o.empty() || o[0] == '=' ? "'" + o : o, nullptr, kSrcUser);
      }
      return filled;
    }
  }
  if (error) *error = TR("Flash Fill didn't recognize a pattern. Type one or two more examples, then try again.");
  return 0;
}

// ---- Solver ----

namespace {

// dense LP: minimize c.x subject to rows (a.x rel b), x >= 0; two-phase simplex. Returns 0 optimal, 1 infeasible,
// 2 unbounded
int Simplex(const std::vector<double>& c, const std::vector<std::vector<double>>& a, const std::vector<int>& rel, const std::vector<double>& b,
            std::vector<double>* x, double* value) {
  const size_t m = a.size(), n = c.size();
  // standard form with slack / surplus and artificial variables
  std::vector<std::vector<double>> rows = a;
  std::vector<double> rhs = b;
  std::vector<int> r = rel;
  for (size_t i = 0; i < m; ++i) {
    if (rhs[i] < 0) {
      for (double& v : rows[i]) v = -v;
      rhs[i] = -rhs[i];
      r[i] = r[i] == 1 ? 3 : r[i] == 3 ? 1 : 2;
    }
  }
  size_t slack = 0, art = 0;
  for (size_t i = 0; i < m; ++i) {
    if (r[i] != 2) ++slack;
    if (r[i] != 1) ++art;
  }
  const size_t cols = n + slack + art;
  std::vector<std::vector<double>> t(m + 1, std::vector<double>(cols + 1, 0));
  std::vector<size_t> basis(m);
  size_t si = n, ai = n + slack;
  std::vector<char> is_art(cols, 0);
  for (size_t i = 0; i < m; ++i) {
    for (size_t j = 0; j < n; ++j) t[i][j] = rows[i][j];
    t[i][cols] = rhs[i];
    if (r[i] == 1) {
      t[i][si] = 1;
      basis[i] = si++;
    } else if (r[i] == 3) {
      t[i][si++] = -1;
      t[i][ai] = 1;
      is_art[ai] = 1;
      basis[i] = ai++;
    } else {
      t[i][ai] = 1;
      is_art[ai] = 1;
      basis[i] = ai++;
    }
  }
  auto pivot = [&](size_t pr, size_t pc) {
    const double p = t[pr][pc];
    for (double& v : t[pr]) v /= p;
    for (size_t i = 0; i <= m; ++i) {
      if (i == pr || t[i][pc] == 0) continue;
      const double f = t[i][pc];
      for (size_t j = 0; j <= cols; ++j) t[i][j] -= f * t[pr][j];
    }
    basis[pr] = pc;
  };
  auto run = [&](const std::vector<char>& allowed) -> int {
    for (int it = 0; it < 50000; ++it) {
      size_t pc = cols;
      double best = -1e-10;
      for (size_t j = 0; j < cols; ++j) {
        if (allowed[j] && t[m][j] < best) {
          best = t[m][j];
          pc = j;
          break;  // Bland's rule: the first improving column (no cycling)
        }
      }
      if (pc == cols) return 0;
      size_t pr = m;
      double ratio = INFINITY;
      for (size_t i = 0; i < m; ++i) {
        if (t[i][pc] > 1e-12) {
          const double q = t[i][cols] / t[i][pc];
          if (q < ratio - 1e-12) ratio = q, pr = i;
        }
      }
      if (pr == m) return 2;
      pivot(pr, pc);
    }
    return 0;
  };
  std::vector<char> all(cols, 1);
  // phase 1: minimize the sum of the artificial variables
  if (art) {
    std::fill(t[m].begin(), t[m].end(), 0.0);
    for (size_t j = 0; j < cols; ++j) {
      if (is_art[j]) t[m][j] = 1;
    }
    for (size_t i = 0; i < m; ++i) {
      if (!is_art[basis[i]]) continue;
      for (size_t j = 0; j <= cols; ++j) t[m][j] -= t[i][j];
    }
    run(all);
    if (-t[m][cols] > 1e-7) return 1;
    // drive the remaining artificial variables out of the basis
    for (size_t i = 0; i < m; ++i) {
      if (!is_art[basis[i]]) continue;
      for (size_t j = 0; j < n + slack; ++j) {
        if (std::fabs(t[i][j]) > 1e-12) {
          pivot(i, j);
          break;
        }
      }
    }
  }
  // phase 2
  std::fill(t[m].begin(), t[m].end(), 0.0);
  for (size_t j = 0; j < n; ++j) t[m][j] = c[j];
  for (size_t i = 0; i < m; ++i) {
    const size_t bj = basis[i];
    if (t[m][bj] == 0) continue;
    const double f = t[m][bj];
    for (size_t j = 0; j <= cols; ++j) t[m][j] -= f * t[i][j];
  }
  std::vector<char> allowed(cols, 1);
  for (size_t j = 0; j < cols; ++j) allowed[j] = !is_art[j];
  if (run(allowed) == 2) return 2;
  x->assign(n, 0);
  for (size_t i = 0; i < m; ++i) {
    if (basis[i] < n) (*x)[basis[i]] = t[i][cols];
  }
  *value = -t[m][cols];
  return 0;
}

}  // namespace

Document::SolverResult Document::Solve(uint32_t sheet, const model::SolverModel& m) {
  SolverResult res;
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  // variables
  std::vector<std::pair<uint32_t, Rect>> var_refs;
  if (!ParseRefList(m.changing, sheet, &var_refs)) {
    res.status = 13;
    res.message = TR("The variable cells are not valid references.");
    return res;
  }
  struct Cell {
    uint32_t sheet, r, c;
  };
  std::vector<Cell> vars;
  for (const auto& [sh, rc] : var_refs) {
    for (uint32_t r = rc.r0; r <= rc.r1; ++r) {
      for (uint32_t c = rc.c0; c <= rc.c1; ++c) vars.push_back(Cell{sh, r, c});
    }
  }
  if (vars.empty() || vars.size() > 200) {
    res.status = 13;
    res.message = TR("There must be 1 to 200 variable cells.");
    return res;
  }
  for (const Cell& v : vars) {
    const Sheet* s = S(v.sheet);
    if (!s || s->GetFormula(v.r, v.c)) {
      res.status = 13;
      res.message = TR("Variable cells must contain values, not formulas.");
      return res;
    }
    const Rect cell = Rect::Cell(v.r, v.c);
    if (Refuse(v.sheet, &cell)) {
      res.status = 13;
      res.message = TR("A variable cell is protected.");
      return res;
    }
  }
  const size_t n = vars.size();
  auto var_index = [&](uint32_t sh, uint32_t r, uint32_t c) -> int {
    for (size_t i = 0; i < n; ++i) {
      if (vars[i].sheet == sh && vars[i].r == r && vars[i].c == c) return static_cast<int>(i);
    }
    return -1;
  };
  // objective
  bool has_obj = !m.objective.empty();
  Cell obj{sheet, 0, 0};
  if (has_obj) {
    std::vector<std::pair<uint32_t, Rect>> o;
    if (!ParseRefList(m.objective, sheet, &o) || o.size() != 1 || !o[0].second.IsCell()) {
      res.status = 13;
      res.message = TR("The objective must be a single cell.");
      return res;
    }
    obj = Cell{o[0].first, o[0].second.r0, o[0].second.c0};
  }
  // constraints -> scalar rows (cell, rel, rhs cell or constant or formula); int / bin / dif mark variables
  struct Row {
    Cell lhs;
    int rel;
    bool rhs_cell = false;
    Cell rhs{};
    double rhs_value = 0;
    std::string rhs_formula;
  };
  std::vector<Row> rows;
  std::vector<char> is_int(n, 0), is_bin(n, 0);
  std::vector<std::vector<int>> dif_groups;
  for (const auto& con : m.constraints) {
    std::vector<std::pair<uint32_t, Rect>> lhs;
    if (!ParseRefList(con.ref, sheet, &lhs)) {
      res.status = 13;
      res.message = TR("A constraint has an invalid reference: ") + con.ref;
      return res;
    }
    std::vector<Cell> lc;
    for (const auto& [sh, rc] : lhs) {
      for (uint32_t r = rc.r0; r <= rc.r1; ++r) {
        for (uint32_t c = rc.c0; c <= rc.c1; ++c) lc.push_back(Cell{sh, r, c});
      }
    }
    if (con.rel >= 4 && m.ignore_integer) continue;  // Options > Ignore Integer Constraints
    if (con.rel >= 4) {
      std::vector<int> group;
      for (const Cell& x : lc) {
        const int i = var_index(x.sheet, x.r, x.c);
        if (i < 0) {
          res.status = 13;
          res.message = TR("int, bin and dif constraints can only be used on variable cells.");
          return res;
        }
        if (con.rel == 4) is_int[static_cast<size_t>(i)] = 1;
        if (con.rel == 5) is_bin[static_cast<size_t>(i)] = is_int[static_cast<size_t>(i)] = 1;
        group.push_back(i);
      }
      if (con.rel == 6) {
        for (int i : group) is_int[static_cast<size_t>(i)] = 1;
        dif_groups.push_back(group);
      }
      continue;
    }
    // right side: a number, a reference of the same size (or one cell), or a formula
    std::string rhs = con.rhs;
    while (!rhs.empty() && rhs.front() == ' ') rhs.erase(0, 1);
    char* end = nullptr;
    const double num = std::strtod(rhs.c_str(), &end);
    std::vector<std::pair<uint32_t, Rect>> rr;
    std::vector<Cell> rc_cells;
    if (!rhs.empty() && end && *end == 0) {
      // constant
    } else if (ParseRefList(rhs, sheet, &rr)) {
      for (const auto& [sh, rc] : rr) {
        for (uint32_t r = rc.r0; r <= rc.r1; ++r) {
          for (uint32_t c = rc.c0; c <= rc.c1; ++c) rc_cells.push_back(Cell{sh, r, c});
        }
      }
      if (rc_cells.size() != 1 && rc_cells.size() != lc.size()) {
        res.status = 13;
        res.message = TR("The right side of a constraint must be one cell or the same size as the left side.");
        return res;
      }
    } else if (rhs.empty()) {
      res.status = 13;
      res.message = TR("A constraint has no right side.");
      return res;
    }
    for (size_t k = 0; k < lc.size(); ++k) {
      Row row;
      row.lhs = lc[k];
      row.rel = con.rel;
      if (!rc_cells.empty()) {
        row.rhs_cell = true;
        row.rhs = rc_cells[rc_cells.size() == 1 ? 0 : k];
      } else if (end && *end == 0 && !rhs.empty()) {
        row.rhs_value = num;
      } else {
        row.rhs_formula = rhs;
      }
      rows.push_back(row);
    }
  }
  // original values (Restore Original Values)
  for (const Cell& v : vars) {
    const Value& x = GetValue(v.sheet, v.r, v.c);
    res.original.push_back(x.IsNumeric() ? x.AsDouble() : 0);
  }
  // evaluation
  int evals = 0;
  auto set_vars = [&](const std::vector<double>& x) {
    for (size_t i = 0; i < n; ++i) SetValue(vars[i].sheet, vars[i].r, vars[i].c, Value::Number(x[i]), kSrcCalc | kSrcNoEvents);
    Recalc();
    ++evals;
  };
  auto num_at = [&](const Cell& c) {
    const Value& v = GetValue(c.sheet, c.r, c.c);
    return v.IsNumeric() ? v.AsDouble() : (v.IsEmpty() ? 0.0 : NAN);
  };
  auto objective = [&]() {
    if (!has_obj) return 0.0;
    const double f = num_at(obj);
    return m.goal == 1 ? -f : m.goal == 2 ? f : (f - m.value) * (f - m.value);
  };
  // constraint values g (<= 0 is satisfied for <=, >=; = 0 for =)
  auto residuals = [&](std::vector<double>* g, std::vector<int>* rel) {
    g->clear();
    rel->clear();
    for (const Row& row : rows) {
      double rhs = row.rhs_value;
      if (row.rhs_cell) {
        rhs = num_at(row.rhs);
      } else if (!row.rhs_formula.empty()) {
        std::string e;
        const Value v = EvaluateFormula(sheet, row.rhs_formula, &e);
        rhs = v.IsNumeric() ? v.AsDouble() : NAN;
      }
      const double l = num_at(row.lhs);
      if (row.rel == 1) g->push_back(l - rhs);
      else if (row.rel == 3) g->push_back(rhs - l);
      else g->push_back(l - rhs);
      rel->push_back(row.rel == 2 ? 2 : 1);
    }
  };
  auto violation = [&](const std::vector<double>& g, const std::vector<int>& rel) {
    double v = 0;
    for (size_t i = 0; i < g.size(); ++i) {
      const double x = rel[i] == 2 ? std::fabs(g[i]) : std::max(0.0, g[i]);
      v += std::isfinite(x) ? x * x : 1e30;
    }
    return v;
  };
  std::vector<double> lo(n, m.non_negative ? 0.0 : -kInf), hi(n, kInf);
  for (size_t i = 0; i < n; ++i) {
    if (is_bin[i]) lo[i] = 0, hi[i] = 1;
  }
  // simple bounds from constraints "variable <= / >= constant"
  for (const Row& row : rows) {
    const int i = var_index(row.lhs.sheet, row.lhs.r, row.lhs.c);
    if (i < 0 || row.rhs_cell || !row.rhs_formula.empty()) continue;
    if (row.rel == 1) hi[static_cast<size_t>(i)] = std::min(hi[static_cast<size_t>(i)], row.rhs_value);
    if (row.rel == 3) lo[static_cast<size_t>(i)] = std::max(lo[static_cast<size_t>(i)], row.rhs_value);
    if (row.rel == 2) lo[static_cast<size_t>(i)] = hi[static_cast<size_t>(i)] = row.rhs_value;
  }
  auto clamp = [&](std::vector<double>& x) {
    for (size_t i = 0; i < n; ++i) x[i] = std::min(hi[i], std::max(lo[i], x[i]));
  };
  auto dif_penalty = [&](const std::vector<double>& x) {
    double p = 0;
    for (const auto& g : dif_groups) {
      const double k = static_cast<double>(g.size());
      std::vector<double> v;
      for (int i : g) v.push_back(std::round(x[static_cast<size_t>(i)]));
      std::sort(v.begin(), v.end());
      for (size_t j = 0; j < v.size(); ++j) p += std::fabs(v[j] - static_cast<double>(j + 1));
      (void)k;
    }
    return p;
  };
  std::vector<double> x0 = res.original;
  clamp(x0);
  std::vector<double> best_x = x0;
  double best_f = INFINITY, best_v = INFINITY;
  std::vector<double> g;
  std::vector<int> rel;
  auto consider = [&](const std::vector<double>& x) {
    set_vars(x);
    const double f = objective();
    residuals(&g, &rel);
    const double v = violation(g, rel) + dif_penalty(x);
    const double tol = std::max(1e-12, m.precision * m.precision);
    const bool feasible = v <= tol, best_feasible = best_v <= tol;
    if ((feasible && (!best_feasible || f < best_f)) || (!feasible && !best_feasible && v < best_v)) {
      best_x = x;
      best_f = f;
      best_v = v;
    }
    return std::make_pair(f, v);
  };
  auto has_int = std::any_of(is_int.begin(), is_int.end(), [](char c) { return c != 0; });

  // ---- Simplex LP ----
  if (m.method == 2) {
    // the model must be linear: check the slopes at two points
    set_vars(x0);
    const double f0 = objective();
    residuals(&g, &rel);
    const std::vector<double> g0 = g;
    std::vector<double> cvec(n);
    std::vector<std::vector<double>> amat(rows.size(), std::vector<double>(n));
    for (size_t j = 0; j < n; ++j) {
      std::vector<double> x = x0;
      x[j] += 1;
      set_vars(x);
      cvec[j] = objective() - f0;
      residuals(&g, &rel);
      for (size_t i = 0; i < rows.size(); ++i) amat[i][j] = g[i] - g0[i];
      x[j] += 1;
      set_vars(x);
      const double f2 = objective();
      residuals(&g, &rel);
      bool linear = std::fabs((f2 - f0) - 2 * cvec[j]) <= 1e-7 * std::max(1.0, std::fabs(f2));
      for (size_t i = 0; i < rows.size() && linear; ++i) linear = std::fabs((g[i] - g0[i]) - 2 * amat[i][j]) <= 1e-7 * std::max(1.0, std::fabs(g[i]));
      if (!linear || (has_obj && m.goal == 3)) {
        set_vars(res.original);
        res.status = 7;
        res.message = TR("The linearity conditions required by this LP Solver are not satisfied.");
        return res;
      }
    }
    // constant parts: g(x) = A x + (g0 - A x0)
    std::vector<double> bvec(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
      double ax0 = 0;
      for (size_t j = 0; j < n; ++j) ax0 += amat[i][j] * x0[j];
      bvec[i] = -(g0[i] - ax0);  // A x <= / = b
    }
    // variables with a lower bound: x = lo + y (y >= 0); free variables: x = y+ - y-
    struct Col {
      size_t var;
      double sign;
    };
    std::vector<Col> colmap;
    std::vector<double> shift(n, 0);
    for (size_t j = 0; j < n; ++j) {
      if (lo[j] > -kInf / 2) {
        shift[j] = lo[j];
        colmap.push_back(Col{j, 1});
      } else {
        colmap.push_back(Col{j, 1});
        colmap.push_back(Col{j, -1});
      }
    }
    auto solve_lp = [&](const std::vector<double>& lo2, const std::vector<double>& hi2, std::vector<double>* xs, double* fv) {
      std::vector<std::vector<double>> A;
      std::vector<double> B;
      std::vector<int> R;
      auto add_row = [&](const std::vector<double>& coef, int r, double b) {
        std::vector<double> rowc(colmap.size());
        double bb = b;
        for (size_t k = 0; k < colmap.size(); ++k) rowc[k] = coef[colmap[k].var] * colmap[k].sign;
        for (size_t j = 0; j < n; ++j) bb -= coef[j] * (lo2[j] > -kInf / 2 ? lo2[j] : 0);
        A.push_back(rowc);
        B.push_back(bb);
        R.push_back(r);
      };
      for (size_t i = 0; i < rows.size(); ++i) add_row(amat[i], rel[i] == 2 ? 2 : 1, bvec[i]);
      for (size_t j = 0; j < n; ++j) {
        if (hi2[j] < kInf / 2) {
          std::vector<double> e(n, 0);
          e[j] = 1;
          add_row(e, 1, hi2[j]);
        }
        if (lo2[j] > -kInf / 2 && lo2[j] != lo[j]) {
          // a branch raised the lower bound: already in the shift
        }
      }
      std::vector<double> cc(colmap.size());
      for (size_t k = 0; k < colmap.size(); ++k) cc[k] = cvec[colmap[k].var] * colmap[k].sign;
      std::vector<double> y;
      double v = 0;
      const int st = Simplex(cc, A, R, B, &y, &v);
      if (st) return st;
      xs->assign(n, 0);
      for (size_t j = 0; j < n; ++j) (*xs)[j] = lo2[j] > -kInf / 2 ? lo2[j] : 0;
      for (size_t k = 0; k < colmap.size(); ++k) (*xs)[colmap[k].var] += colmap[k].sign * y[k];
      // the objective of x itself (the simplex value leaves out the part of the shifted lower bounds)
      *fv = 0;
      for (size_t j = 0; j < n; ++j) *fv += cvec[j] * (*xs)[j];
      (void)v;
      return 0;
    };
    (void)shift;
    // branch and bound on integer variables
    double incumbent = INFINITY;
    std::vector<double> inc_x;
    int status = 1;
    std::vector<std::pair<std::vector<double>, std::vector<double>>> stack{{lo, hi}};
    int nodes = 0;
    while (!stack.empty() && nodes < 20000 && elapsed() < m.max_time) {
      auto [l2, h2] = stack.back();
      stack.pop_back();
      ++nodes;
      std::vector<double> xs;
      double fv = 0;
      const int st = solve_lp(l2, h2, &xs, &fv);
      if (st == 2) {
        status = 2;
        break;
      }
      if (st || fv >= incumbent - 1e-9) continue;
      int frac = -1;
      for (size_t j = 0; j < n; ++j) {
        if (is_int[j] && std::fabs(xs[j] - std::round(xs[j])) > 1e-6) {
          frac = static_cast<int>(j);
          break;
        }
      }
      if (frac < 0) {
        incumbent = fv;
        inc_x = xs;
        for (size_t j = 0; j < n; ++j) {
          if (is_int[j]) inc_x[j] = std::round(inc_x[j]);
        }
        status = 0;
        continue;
      }
      auto down = std::make_pair(l2, h2), up = std::make_pair(l2, h2);
      down.second[static_cast<size_t>(frac)] = std::floor(xs[static_cast<size_t>(frac)]);
      up.first[static_cast<size_t>(frac)] = std::ceil(xs[static_cast<size_t>(frac)]);
      stack.push_back(up);
      stack.push_back(down);
    }
    res.iterations = nodes;
    if (status == 2) {
      set_vars(res.original);
      res.status = 4;
      res.message = TR("The Objective Cell values do not converge.");
      return res;
    }
    if (inc_x.empty()) {
      set_vars(res.original);
      res.status = 5;
      res.message = TR("Solver could not find a feasible solution.");
      return res;
    }
    set_vars(inc_x);
    res.objective = has_obj ? num_at(obj) : 0;
    res.status = has_int ? 14 : 0;
    res.message = has_int ? TR("Solver found an integer solution within tolerance. All Constraints are satisfied.")
                          : TR("Solver found a solution. All Constraints and optimality conditions are satisfied.");
    return res;
  }

  // ---- GRG Nonlinear (penalty method with projected BFGS) and Evolutionary ----
  std::mt19937_64 rng(m.random_seed > 0 ? static_cast<uint64_t>(m.random_seed) : 12345u);
  auto local = [&](std::vector<double> x, const std::vector<double>& l2, const std::vector<double>& h2) {
    auto clamp2 = [&](std::vector<double>& y) {
      for (size_t i = 0; i < n; ++i) y[i] = std::min(h2[i], std::max(l2[i], y[i]));
    };
    clamp2(x);
    for (double mu = 10; mu <= 1e10 && elapsed() < m.max_time; mu *= 10) {
      auto P = [&](const std::vector<double>& y) {
        const auto [f, v] = consider(y);
        return std::isfinite(f) ? f + mu * v : 1e300;
      };
      double fx = P(x);
      std::vector<double> grad(n), H(n * n, 0);
      for (size_t i = 0; i < n; ++i) H[i * n + i] = 1;
      auto gradient = [&](const std::vector<double>& y, double fy) {
        std::vector<double> gr(n);
        for (size_t i = 0; i < n; ++i) {
          const double h = 1e-6 * std::max(1.0, std::fabs(y[i]));
          std::vector<double> z = y;
          z[i] += h;
          if (z[i] > h2[i]) z[i] = y[i] - h;
          gr[i] = (P(z) - fy) / (z[i] - y[i]);
        }
        return gr;
      };
      grad = gradient(x, fx);
      for (int it = 0; it < m.max_iterations && elapsed() < m.max_time; ++it) {
        ++res.iterations;
        // direction d = -H g
        std::vector<double> d(n, 0);
        for (size_t i = 0; i < n; ++i) {
          for (size_t j = 0; j < n; ++j) d[i] -= H[i * n + j] * grad[j];
        }
        // projected backtracking line search
        double step = 1, fnew = fx;
        std::vector<double> xn;
        bool moved = false;
        for (int ls = 0; ls < 40; ++ls) {
          xn = x;
          for (size_t i = 0; i < n; ++i) xn[i] += step * d[i];
          clamp2(xn);
          fnew = P(xn);
          if (fnew < fx - 1e-12 * std::fabs(fx)) {
            moved = true;
            break;
          }
          step *= 0.5;
        }
        if (!moved) {
          // reset the curvature and try steepest descent once
          bool any = false;
          for (size_t i = 0; i < n; ++i) any = any || H[i * n + i] != 1;
          if (!any) break;
          std::fill(H.begin(), H.end(), 0.0);
          for (size_t i = 0; i < n; ++i) H[i * n + i] = 1;
          continue;
        }
        const std::vector<double> gn = gradient(xn, fnew);
        // BFGS update
        std::vector<double> s(n), yv(n);
        double sy = 0;
        for (size_t i = 0; i < n; ++i) {
          s[i] = xn[i] - x[i];
          yv[i] = gn[i] - grad[i];
          sy += s[i] * yv[i];
        }
        if (sy > 1e-12) {
          std::vector<double> Hy(n, 0);
          double yHy = 0;
          for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) Hy[i] += H[i * n + j] * yv[j];
            yHy += yv[i] * Hy[i];
          }
          for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) H[i * n + j] += ((sy + yHy) * s[i] * s[j]) / (sy * sy) - (Hy[i] * s[j] + s[i] * Hy[j]) / sy;
          }
        }
        const double change = std::fabs(fnew - fx);
        x = xn;
        fx = fnew;
        grad = gn;
        if (change < m.precision * std::max(1.0, std::fabs(fx)) * 1e-3) break;
      }
      set_vars(x);
      residuals(&g, &rel);
      if (violation(g, rel) <= std::max(1e-12, m.precision * m.precision)) break;
    }
    return x;
  };

  if (m.method == 3) {
    // Evolutionary: differential evolution inside the bounds (all variables need finite bounds)
    for (size_t i = 0; i < n; ++i) {
      if (m.require_bounds && (lo[i] <= -kInf / 2 || hi[i] >= kInf / 2)) {
        res.status = 13;
        res.message = TR("All variables must have both upper and lower bounds for the Evolutionary method.");
        return res;
      }
    }
    // without required bounds a variable searches around its start (Excel's Evolutionary then works less well)
    for (size_t i = 0; i < n; ++i) {
      if (lo[i] <= -kInf / 2) lo[i] = x0[i] - 1e4;
      if (hi[i] >= kInf / 2) hi[i] = x0[i] + 1e4;
    }
    const size_t pop = std::max<size_t>(10, std::min<size_t>(static_cast<size_t>(std::max(1, m.population)), 10000));
    std::uniform_real_distribution<double> u01(0, 1);
    std::vector<std::vector<double>> P(pop, std::vector<double>(n));
    std::vector<double> score(pop);
    auto fit = [&](std::vector<double>& x) {
      for (size_t i = 0; i < n; ++i) {
        if (is_int[i]) x[i] = std::round(x[i]);
      }
      clamp(x);
      const auto [f, v] = consider(x);
      return (std::isfinite(f) ? f : 1e300) + 1e6 * v;
    };
    for (size_t k = 0; k < pop; ++k) {
      for (size_t i = 0; i < n; ++i) P[k][i] = k == 0 ? x0[i] : lo[i] + u01(rng) * (hi[i] - lo[i]);
      score[k] = fit(P[k]);
    }
    std::uniform_int_distribution<size_t> pick(0, pop - 1);
    for (int gen = 0; gen < m.max_iterations && elapsed() < m.max_time; ++gen) {
      ++res.iterations;
      for (size_t k = 0; k < pop; ++k) {
        size_t a = pick(rng), b = pick(rng), c = pick(rng);
        std::vector<double> trial = P[k];
        const size_t jr = static_cast<size_t>(u01(rng) * static_cast<double>(n)) % n;
        for (size_t i = 0; i < n; ++i) {
          if (u01(rng) < 0.9 || i == jr) trial[i] = P[a][i] + 0.7 * (P[b][i] - P[c][i]);
        }
        const double s = fit(trial);
        if (s <= score[k]) {
          P[k] = trial;
          score[k] = s;
        }
      }
    }
    set_vars(best_x);
  } else {
    // GRG: a local solve, with branch and bound on integer variables
    std::vector<double> x = local(x0, lo, hi);
    if (has_int) {
      double incumbent = INFINITY;
      std::vector<double> inc;
      std::vector<std::pair<std::vector<double>, std::vector<double>>> stack{{lo, hi}};
      int nodes = 0;
      while (!stack.empty() && nodes < 300 && elapsed() < m.max_time) {
        auto [l2, h2] = stack.back();
        stack.pop_back();
        ++nodes;
        std::vector<double> xs = local(x, l2, h2);
        set_vars(xs);
        const double f = objective();
        residuals(&g, &rel);
        if (violation(g, rel) > std::max(1e-8, m.precision) || f >= incumbent) continue;
        int frac = -1;
        for (size_t j = 0; j < n; ++j) {
          if (is_int[j] && std::fabs(xs[j] - std::round(xs[j])) > 1e-6) {
            frac = static_cast<int>(j);
            break;
          }
        }
        if (frac < 0) {
          incumbent = f;
          inc = xs;
          continue;
        }
        auto down = std::make_pair(l2, h2), up = std::make_pair(l2, h2);
        down.second[static_cast<size_t>(frac)] = std::floor(xs[static_cast<size_t>(frac)]);
        up.first[static_cast<size_t>(frac)] = std::ceil(xs[static_cast<size_t>(frac)]);
        stack.push_back(up);
        stack.push_back(down);
      }
      if (!inc.empty()) {
        for (size_t j = 0; j < n; ++j) {
          if (is_int[j]) inc[j] = std::round(inc[j]);
        }
        consider(inc);
        best_x = inc;
      }
    }
    set_vars(best_x);
  }
  residuals(&g, &rel);
  const bool feasible = violation(g, rel) + dif_penalty(best_x) <= std::max(1e-10, m.precision * m.precision * 100);
  res.objective = has_obj ? num_at(obj) : 0;
  if (!feasible) {
    set_vars(res.original);
    res.status = 5;
    res.message = TR("Solver could not find a feasible solution.");
  } else if (elapsed() >= m.max_time) {
    res.status = 3;
    res.message = TR("Solver stopped at the time limit; the best solution found is shown.");
  } else if (m.method == 3) {
    res.status = 1;
    res.message = TR("Solver converged to the current solution. All Constraints are satisfied.");
  } else {
    res.status = has_int ? 14 : 0;
    res.message = has_int ? TR("Solver found an integer solution within tolerance. All Constraints are satisfied.")
                          : TR("Solver found a solution. All Constraints and optimality conditions are satisfied.");
  }
  (void)evals;
  return res;
}

void Document::SetSolverModel(uint32_t sheet, model::SolverModel m) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  s->solver.mut() = std::move(m);
  wb_.dirty = true;
}

}  // namespace yc::engine
