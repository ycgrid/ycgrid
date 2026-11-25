// SPDX-License-Identifier: Apache-2.0
// sparkline.cpp - see sparkline.h
#include "engine/sparkline.h"

#include "model/sheet.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace yc::engine {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// the cells of r in reading order, without the hidden ones unless asked for
template <class F>
void EachCell(const model::Sheet& s, const core::Rect& r, bool hidden_too, F&& fn) {
  for (uint32_t rr = r.r0; rr <= r.r1; ++rr) {
    if (!hidden_too && s.rows->IsHidden(rr)) continue;
    for (uint32_t cc = r.c0; cc <= r.c1; ++cc) {
      if (!hidden_too && s.cols->IsHidden(cc)) continue;
      fn(rr, cc);
    }
  }
}

}  // namespace

std::vector<double> SparklineValues(const model::Workbook& wb, const model::SparklineGroup& g, const model::Sparkline& it, std::vector<double>* xfrac) {
  std::vector<double> v;
  if (xfrac) xfrac->clear();
  const model::Sheet* src = wb.SheetById(it.src_sheet);
  if (!src || it.src.Count() > 10000) return v;
  std::vector<std::pair<uint32_t, uint32_t>> cells;
  EachCell(*src, it.src, g.show_hidden, [&](uint32_t r, uint32_t c) { cells.emplace_back(r, c); });
  for (const auto& [r, c] : cells) {
    const core::Value& x = src->GetValue(r, c);
    v.push_back(x.IsNumeric() ? x.AsDouble() : x.IsEmpty() && g.display_blanks == 2 ? 0.0 : kNaN);
  }
  // Connect data points with line: a line runs across the empty cells
  if (g.display_blanks == 3 && g.type == 0) {
    size_t prev = SIZE_MAX;
    for (size_t i = 0; i < v.size(); ++i) {
      if (std::isnan(v[i])) continue;
      if (prev != SIZE_MAX && i > prev + 1) {
        for (size_t k = prev + 1; k < i; ++k) v[k] = v[prev] + (v[i] - v[prev]) * static_cast<double>(k - prev) / static_cast<double>(i - prev);
      }
      prev = i;
    }
  }
  // Date Axis Type: the dates' range gives each point's place (as many dates as values)
  if (xfrac && !g.date_range.empty()) {
    const model::Sheet* ds = nullptr;
    core::Rect dr{};
    const size_t bang = g.date_range.rfind('!');
    std::string sheet_name = bang == std::string::npos ? std::string() : g.date_range.substr(0, bang);
    if (sheet_name.size() >= 2 && sheet_name.front() == '\'' && sheet_name.back() == '\'') sheet_name = sheet_name.substr(1, sheet_name.size() - 2);
    for (const auto& sc : wb.sheets) {
      if (bang == std::string::npos ? sc.get().id == it.src_sheet : sc.get().name == sheet_name) ds = &sc.get();
    }
    std::string a1 = bang == std::string::npos ? g.date_range : g.date_range.substr(bang + 1);
    a1.erase(std::remove(a1.begin(), a1.end(), '$'), a1.end());
    if (ds && core::ParseRectA1(a1, &dr)) {
      std::vector<double> d;
      EachCell(*ds, dr, g.show_hidden, [&](uint32_t r, uint32_t c) {
        const core::Value& x = ds->GetValue(r, c);
        d.push_back(x.IsNumeric() ? x.AsDouble() : kNaN);
      });
      if (d.size() == v.size() && !d.empty()) {
        double lo = 1e300, hi = -1e300;
        for (double x : d) {
          if (!std::isnan(x)) lo = std::min(lo, x), hi = std::max(hi, x);
        }
        if (hi > lo) {
          for (double x : d) xfrac->push_back(std::isnan(x) ? kNaN : (x - lo) / (hi - lo));
        }
      }
    }
  }
  if (g.right_to_left) {
    std::reverse(v.begin(), v.end());
    if (xfrac) {
      std::reverse(xfrac->begin(), xfrac->end());
      for (double& x : *xfrac) x = 1 - x;
    }
  }
  return v;
}

void SparklineAxis(const model::Workbook& wb, const model::SparklineGroup& g, double* lo, double* hi) {
  *lo = *hi = kNaN;
  if (g.min_scale == 3) *lo = g.min_value;
  if (g.max_scale == 3) *hi = g.max_value;
  if (g.min_scale != 1 && g.max_scale != 1) return;
  double mn = 1e300, mx = -1e300;
  for (const auto& it : g.items) {
    for (double x : SparklineValues(wb, g, it)) {
      if (!std::isnan(x)) mn = std::min(mn, x), mx = std::max(mx, x);
    }
  }
  if (mn > mx) return;
  if (g.min_scale == 1) *lo = mn;
  if (g.max_scale == 1) *hi = mx;
}

}  // namespace yc::engine
