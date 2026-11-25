// SPDX-License-Identifier: Apache-2.0
// document_analysis.cpp - Data > Data Analysis (Analysis ToolPak): inputs from the sheet, the output written back
#include "core/i18n.h"
#include "engine/document.h"

#include <algorithm>

namespace yc::engine {

using core::Rect;
using core::Value;

bool Document::RunAnalysis(AnalysisRequest req, uint32_t* out_sheet, Rect* out_rect, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  // the output sheet first (formulas name other sheets)
  uint32_t osheet = req.out_sheet;
  if (!req.new_sheet && !S(osheet)) return fail(TR("The output range isn't valid."));
  auto sheet_prefix = [&](uint32_t sid) -> std::string {
    if (!req.new_sheet && sid == osheet) return {};
    const model::Sheet* s = S(sid);
    return s ? core::QuoteSheetName(s->name) + "!" : std::string();
  };
  auto fill = [&](uint32_t sid, Rect r, analysis::Block* b) -> bool {
    const model::Sheet* s = S(sid);
    if (!s) return false;
    // whole columns or rows: up to the used range
    if (s->has_used) {
      r.r1 = std::min(r.r1, std::max(r.r0, s->used.r1));
      r.c1 = std::min(r.c1, std::max(r.c0, s->used.c1));
    }
    b->rows = r.r1 - r.r0 + 1;
    b->cols = r.c1 - r.c0 + 1;
    if (static_cast<uint64_t>(b->rows) * b->cols > 10'000'000) return false;
    b->v.resize(static_cast<size_t>(b->rows) * b->cols);
    for (uint32_t i = 0; i < b->rows; ++i) {
      for (uint32_t j = 0; j < b->cols; ++j) b->v[static_cast<size_t>(i) * b->cols + j] = s->GetValue(r.r0 + i, r.c0 + j);
    }
    const std::string pre = sheet_prefix(sid);
    b->ref = [pre, r](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1) {
      std::string t = pre + core::CellA1(r.r0 + r0, r.c0 + c0, false, false);
      if (r0 != r1 || c0 != c1) t += ":" + core::CellA1(r.r0 + r1, r.c0 + c1, false, false);
      return t;
    };
    return true;
  };
  if (req.has_in && !fill(req.in_sheet, req.in, &req.p.in)) return fail(TR("The input range isn't valid."));
  if (req.has_in2 && !fill(req.in2_sheet, req.in2, &req.p.in2)) return fail(TR("The input range isn't valid."));
  if (req.has_bins && !fill(req.bins_sheet, req.bins, &req.p.bins)) return fail(TR("The bin range isn't valid."));
  const uint32_t r0 = req.new_sheet ? 0 : req.out_r, c0 = req.new_sheet ? 0 : req.out_c;
  req.p.out_ref = [r0, c0](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    std::string t = core::CellA1(r0 + a, c0 + b, false, false);
    if (a != c || b != d) t += ":" + core::CellA1(r0 + c, c0 + d, false, false);
    return t;
  };
  // the plots need the tables they are drawn from
  if (req.residual_plots || req.line_fit_plots) req.p.residuals = true;
  if (req.normal_plots) req.p.normal_probability = true;
  analysis::Result res;
  std::string why;
  if (!analysis::Run(req.p, &res, &why)) return fail(why);
  const uint32_t rows = static_cast<uint32_t>(res.rows.size()), cols = res.Cols();
  if (!rows || !cols) return fail(TR("The tool produced no output."));
  if (r0 + rows > core::kMaxRows || c0 + cols > core::kMaxCols) return fail(TR("The output doesn't fit on the sheet."));
  if (req.dry_run) {
    if (out_sheet) *out_sheet = osheet;
    if (out_rect) *out_rect = Rect{r0, c0, r0 + rows - 1, c0 + cols - 1};
    return true;
  }
  Begin(analysis::ToolName(req.p.tool));
  if (req.new_sheet) {
    // a new sheet before the input's sheet, like Excel's "New Worksheet Ply"
    std::string name = req.sheet_name;
    if (!name.empty() && wb_.FindSheet(name) >= 0) {
      Commit();
      return fail(TR("A sheet with this name already exists: ") + name);
    }
    const model::Sheet* in_s = S(req.in_sheet);
    const int32_t at = in_s ? wb_.FindSheet(in_s->name) : -1;
    osheet = AddSheet(name, at);
    if (!osheet) {
      Commit();
      return fail(TR("The new sheet couldn't be added."));
    }
  }
  const Rect dest{r0, c0, r0 + rows - 1, c0 + cols - 1};
  if (Refuse(osheet, &dest)) {
    Commit();
    return fail(TR("The sheet is protected"));
  }
  ClearContents(osheet, dest, kSrcUser);
  for (uint32_t i = 0; i < rows; ++i) {
    for (uint32_t j = 0; j < res.rows[i].size(); ++j) {
      const analysis::Cell& cell = res.rows[i][j];
      if (!cell.formula.empty()) {
        std::string e;
        const std::string_view f = cell.formula[0] == '=' ? std::string_view(cell.formula).substr(1) : std::string_view(cell.formula);
        SetFormula(osheet, r0 + i, c0 + j, f, &e, kSrcUser);
      } else if (!cell.v.IsEmpty()) {
        SetValue(osheet, r0 + i, c0 + j, cell.v, kSrcUser);
      }
      if (!cell.fmt.empty()) {
        const uint32_t fmt = wb_.styles.mut().InternString(cell.fmt);
        ModifyStyle(osheet, Rect{r0 + i, c0 + j, r0 + i, c0 + j}, [fmt](model::Style& st) { st.numfmt = fmt; });
      }
    }
  }
  AnalysisCharts(req, res, osheet, dest);
  Commit();
  if (out_sheet) *out_sheet = osheet;
  if (out_rect) *out_rect = dest;
  return true;
}

void Document::AnalysisCharts(const AnalysisRequest& req, const analysis::Result& res, uint32_t osheet, const Rect& dest) {
  const bool hist = req.p.tool == analysis::Tool::Histogram && req.chart_output;
  const bool reg = req.p.tool == analysis::Tool::Regression && (req.residual_plots || req.line_fit_plots || req.normal_plots);
  if (!hist && !reg) return;
  const model::Sheet* os = S(osheet);
  if (!os) return;
  const std::string q = core::QuoteSheetName(os->name) + "!";
  auto at = [&](uint32_t r, uint32_t c) { return q + core::CellA1(dest.r0 + r, dest.c0 + c, true, true); };
  auto span = [&](uint32_t c, uint32_t r0, uint32_t r1) { return at(r0, c) + ":" + core::CellA1(dest.r0 + r1, dest.c0 + c, true, true); };
  auto find = [&](const char* text, uint32_t* r, uint32_t* c) {
    for (uint32_t i = 0; i < res.rows.size(); ++i) {
      for (uint32_t j = 0; j < res.rows[i].size(); ++j) {
        const Value& v = res.rows[i][j].v;
        if (v.IsString() && v.str() == text) {
          *r = i;
          *c = j;
          return true;
        }
      }
    }
    return false;
  };
  // charts stand right of the output, one under the other (points from the sheet's top-left corner)
  double left = 0;
  for (uint32_t c = 0; c < dest.c1 + 2; ++c) left += os->cols->Size(c) * 0.75;
  double top = 0;
  for (uint32_t r = 0; r < dest.r0; ++r) top += os->rows->Size(r) * 0.75;
  auto place = [&](model::ChartObj ch) {
    ch.left = left;
    ch.top = top;
    ch.width = 360;
    ch.height = 216;
    ch.has_title = true;
    top += 216 + 12;
    AddChart(osheet, std::move(ch));
  };
  if (hist) {
    // Frequency per bin (and Cumulative % on the secondary axis)
    const uint32_t n = static_cast<uint32_t>(res.rows.size());
    if (n < 2) return;
    model::ChartObj ch;
    ch.type = model::ChartType::ColumnClustered;
    ch.title = "Histogram";
    model::ChartSeries f;
    f.name = "Frequency";
    f.values = span(1, 1, n - 1);
    f.x_values = span(0, 1, n - 1);
    ch.series.push_back(f);
    if (req.p.cumulative) {
      model::ChartSeries cum;
      cum.name = "Cumulative %";
      cum.values = span(2, 1, n - 1);
      cum.x_values = f.x_values;
      cum.type = static_cast<int>(model::ChartType::LineMarkers);
      cum.axis_group = 1;
      ch.series.push_back(cum);
    }
    place(ch);
    return;
  }
  // Regression: the X ranges of the input (without their labels), the residual and probability tables of the output
  const model::Sheet* xs = S(req.in2_sheet);
  const model::Sheet* ys = S(req.in_sheet);
  if (!xs || !ys) return;
  Rect xr = req.in2, yr = req.in;
  if (xs->has_used) xr.r1 = std::min(xr.r1, std::max(xr.r0, xs->used.r1));
  if (ys->has_used) yr.r1 = std::min(yr.r1, std::max(yr.r0, ys->used.r1));
  if (req.p.labels) ++xr.r0, ++yr.r0;
  const std::string x_sheet = core::QuoteSheetName(xs->name) + "!", y_sheet = core::QuoteSheetName(ys->name) + "!";
  auto in_col = [&](const std::string& pre, const Rect& r, uint32_t c) { return pre + core::CellA1(r.r0, c, true, true) + ":" + core::CellA1(r.r1, c, true, true); };
  const uint32_t rows = xr.r1 >= xr.r0 ? xr.r1 - xr.r0 + 1 : 0;
  uint32_t rr = 0, rc = 0;
  const bool has_res = find("RESIDUAL OUTPUT", &rr, &rc);
  const uint32_t first = rr + 3;  // the first observation's row (under the headings)
  auto x_name = [&](uint32_t j) {
    if (req.p.labels) {
      const Value& v = xs->GetValue(req.in2.r0, req.in2.c0 + j);
      if (v.IsString() && !v.str().empty()) return std::string(v.str());
    }
    return xr.c1 > xr.c0 ? "X Variable " + std::to_string(j + 1) : std::string("X Variable 1");
  };
  for (uint32_t j = 0; has_res && rows && j <= xr.c1 - xr.c0; ++j) {
    const std::string xv = in_col(x_sheet, xr, xr.c0 + j);
    if (req.residual_plots) {
      model::ChartObj ch;
      ch.type = model::ChartType::Scatter;
      ch.title = x_name(j) + " Residual Plot";
      ch.has_legend = false;
      model::ChartSeries s1;
      s1.name = "Residuals";
      s1.values = span(rc + 2, first, first + rows - 1);
      s1.x_values = xv;
      ch.series.push_back(s1);
      place(ch);
    }
    if (req.line_fit_plots) {
      model::ChartObj ch;
      ch.type = model::ChartType::Scatter;
      ch.title = x_name(j) + " Line Fit Plot";
      model::ChartSeries y;
      y.name = "Y";
      y.values = in_col(y_sheet, yr, yr.c0);
      y.x_values = xv;
      ch.series.push_back(y);
      model::ChartSeries pred;
      pred.name = "Predicted Y";
      pred.values = span(rc + 1, first, first + rows - 1);
      pred.x_values = xv;
      ch.series.push_back(pred);
      place(ch);
    }
  }
  uint32_t pr = 0, pc = 0;
  if (req.normal_plots && find("PROBABILITY OUTPUT", &pr, &pc) && rows) {
    model::ChartObj ch;
    ch.type = model::ChartType::Scatter;
    ch.title = "Normal Probability Plot";
    ch.has_legend = false;
    model::ChartSeries y;
    y.name = "Y";
    y.values = span(pc + 1, pr + 3, pr + 3 + rows - 1);
    y.x_values = span(pc, pr + 3, pr + 3 + rows - 1);
    ch.series.push_back(y);
    place(ch);
  }
}

}  // namespace yc::engine
