// SPDX-License-Identifier: Apache-2.0
// chart_window.cpp - ImPlot smoke test and chart prototype (PLAN §9)
//
// - 1M-point line: min/max downsampling by default (PLAN §9.3); cost only scales with the plot width in pixels.
//   Downsampling can be turned off to draw every point with PlotLineG (getter) and compare frame times.
// - Data is prepared in contiguous arrays, simulating the SeriesCache the engine publishes in the snapshot (PLAN §9.2); zero allocations per frame.
#include "app_state.h"
#include "core/i18n.h"

#include "fonts/fonts.h"
#include "frame/frame_context.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace yc::app {
namespace {

struct Series {
  std::vector<double> x, y;
  std::vector<double> dx, dy;  // downsampling buffer (reused)
};

Series& LineData() {
  static Series s;
  if (s.x.empty()) {
    const int n = 1'000'000;
    s.x.resize(n);
    s.y.resize(n);
    double v = 23000.0;
    uint32_t seed = 12345;
    for (int i = 0; i < n; ++i) {
      seed = seed * 1664525u + 1013904223u;
      v += (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 4.0;
      s.x[static_cast<size_t>(i)] = i * 0.01;  // one point every 10 ms
      s.y[static_cast<size_t>(i)] = v + 40.0 * std::sin(i * 0.0005);
    }
  }
  return s;
}

ImPlotPoint Getter(int idx, void* data) {
  const Series* s = static_cast<const Series*>(data);
  return ImPlotPoint(s->x[static_cast<size_t>(idx)], s->y[static_cast<size_t>(idx)]);
}

// keep the min and max of each pixel column (in index order); looks the same as drawing every point.
int Decimate(Series& s, double xmin, double xmax, int buckets) {
  const auto b = std::lower_bound(s.x.begin(), s.x.end(), xmin);
  const auto e = std::upper_bound(s.x.begin(), s.x.end(), xmax);
  size_t i0 = static_cast<size_t>(std::max<std::ptrdiff_t>(0, (b - s.x.begin()) - 1));
  size_t i1 = static_cast<size_t>(std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(s.x.size()), (e - s.x.begin()) + 1));
  const size_t count = i1 > i0 ? i1 - i0 : 0;
  s.dx.clear();
  s.dy.clear();
  if (count <= static_cast<size_t>(buckets) * 2) {
    s.dx.insert(s.dx.end(), s.x.begin() + static_cast<std::ptrdiff_t>(i0), s.x.begin() + static_cast<std::ptrdiff_t>(i1));
    s.dy.insert(s.dy.end(), s.y.begin() + static_cast<std::ptrdiff_t>(i0), s.y.begin() + static_cast<std::ptrdiff_t>(i1));
    return static_cast<int>(s.dx.size());
  }
  const double per = static_cast<double>(count) / buckets;
  for (int k = 0; k < buckets; ++k) {
    const size_t a = i0 + static_cast<size_t>(k * per);
    const size_t z = std::min(i1, i0 + static_cast<size_t>((k + 1) * per));
    if (a >= z) continue;
    size_t imin = a, imax = a;
    for (size_t i = a; i < z; ++i) {
      if (s.y[i] < s.y[imin]) imin = i;
      if (s.y[i] > s.y[imax]) imax = i;
    }
    const size_t first = std::min(imin, imax), second = std::max(imin, imax);
    s.dx.push_back(s.x[first]);
    s.dy.push_back(s.y[first]);
    if (second != first) {
      s.dx.push_back(s.x[second]);
      s.dy.push_back(s.y[second]);
    }
  }
  return static_cast<int>(s.dx.size());
}

}  // namespace

void DrawChartWindow(AppState& s) {
  static const char* kTypes[] = {TR("Column##ctx_chart"), TR("Line"), TR("Pie"), TR("Scatter")};
  ImGui::PushFont(ui::F().ui, ui::F().ui_size);
  for (int i = 0; i < 4; ++i) {
    if (i) ImGui::SameLine();
    ImGui::RadioButton(kTypes[i], &s.chart_type, i);
  }
  int submitted = 0;
  if (s.chart_type == 1) {
    ImGui::SameLine(0, ui::S(24));
    ImGui::Checkbox(TR("Downsampling (min/max, PLAN §9.3)"), &s.chart_decimate);
  }
  ImGui::PopFont();

  const ImVec2 size(-1, -1);
  switch (s.chart_type) {
    case 0: {
      static const double months[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
      static const double a[] = {12, 18, 15, 22, 28, 24, 31, 35, 29, 33, 38, 42};
      static const double b[] = {8, 11, 14, 13, 19, 21, 18, 25, 27, 24, 30, 34};
      if (ImPlot::BeginPlot("##column", size)) {
        ImPlot::SetupAxes(TR("Month##ctx_axis"), TR("Volume (K lots)"));
        ImPlot::SetupAxesLimits(0, 13, 0, 45, ImPlotCond_Once);
        ImPlot::PlotBars(TR("TAIEX Futures"), months, a, 12, 0.35);
        static double shifted[12];
        for (int i = 0; i < 12; ++i) shifted[i] = months[i] + 0.38;
        ImPlot::PlotBars(TR("Mini TAIEX"), shifted, b, 12, 0.35);
        ImPlot::EndPlot();
      }
      submitted = 24;
      break;
    }
    case 1: {
      Series& d = LineData();
      if (ImPlot::BeginPlot("##line", size)) {
        ImPlot::SetupAxes(TR("Time (s)"), TR("Price"));
        ImPlot::SetupAxesLimits(0, 10000, 22000, 24000, ImPlotCond_Once);
        ImPlot::SetupFinish();
        const ImPlotRect lim = ImPlot::GetPlotLimits();
        const int width = std::max(16, static_cast<int>(ImPlot::GetPlotSize().x));
        if (s.chart_decimate) {
          const int n = Decimate(d, lim.X.Min, lim.X.Max, width);
          ImPlot::PlotLine(TR("TXF (1M points)"), d.dx.data(), d.dy.data(), n);
          submitted = n;
        } else {
          ImPlot::PlotLineG(TR("TXF (1M points)"), Getter, &d, static_cast<int>(d.x.size()));
          submitted = static_cast<int>(d.x.size());
        }
        ImPlot::EndPlot();
      }
      break;
    }
    case 2: {
      static const char* labels[] = {TR("Futures"), TR("Options##ctx_fin"), TR("Stocks"), "ETF"};
      static const double values[] = {42, 28, 18, 12};
      if (ImPlot::BeginPlot("##pie", size, ImPlotFlags_Equal | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxesLimits(0, 1, 0, 1, ImPlotCond_Always);
        ImPlot::PlotPieChart(labels, values, 4, 0.5, 0.5, 0.4, "%.0f%%", 90);
        ImPlot::EndPlot();
      }
      submitted = 4;
      break;
    }
    default: {
      static std::vector<double> xs, ys;
      if (xs.empty()) {
        uint32_t seed = 7;
        for (int i = 0; i < 5000; ++i) {
          seed = seed * 1664525u + 1013904223u;
          const double u = static_cast<double>(seed >> 8) / 16777216.0;
          seed = seed * 1664525u + 1013904223u;
          const double w = static_cast<double>(seed >> 8) / 16777216.0;
          xs.push_back(u * 100.0);
          ys.push_back(0.6 * u * 100.0 + (w - 0.5) * 30.0);
        }
      }
      if (ImPlot::BeginPlot("##scatter", size)) {
        ImPlot::SetupAxes(TR("Implied Volatility (%)"), TR("Volume"));
        ImPlot::PlotScatter(TR("Contract"), xs.data(), ys.data(), static_cast<int>(xs.size()));
        ImPlot::EndPlot();
      }
      submitted = static_cast<int>(xs.size());
      break;
    }
  }
  (void)submitted;
}

}  // namespace yc::app
