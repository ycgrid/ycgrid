// SPDX-License-Identifier: Apache-2.0
// latency_ui.cpp - [YC] Trading > Latency Monitor: tick-to-screen latency, frame time, throughput and engine load over
// the last minutes (the counters of perf.stats, sampled four times a second while the window is open)
#include "app_state.h"
#include "core/i18n.h"
#include "plot_theme.h"
#include "python_bridge.h"

#include "commands/command_registry.h"
#include "engine/engine.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "implot.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <vector>

namespace yc::app {

using ui::S;

namespace {

constexpr size_t kKeep = 4 * 60 * 10;  // ten minutes of samples

struct Sample {
  double t = 0;  // seconds since the window opened
  double lat_p50 = 0, lat_p99 = 0, frame_avg = 0, frame_p99 = 0, fps = 0, cells_per_s = 0, engine_busy = 0, mem_mb = 0;
};

struct Monitor {
  bool open = false;
  std::deque<Sample> samples;
  double t0 = 0, last = 0;
  uint64_t frames0 = 0, cells0 = 0;
  int64_t busy0 = 0;
  size_t lat_seen = 0;
};

Monitor& M() {
  static Monitor m;
  return m;
}

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double Pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))];
}

void TakeSample(AppState& s) {
  Monitor& m = M();
  const double now = Now();
  const double dt = now - m.last;
  if (m.last > 0 && dt < 0.25) return;
  const uint64_t cells = s.snap ? s.snap->bind_cells : 0;
  const int64_t busy = s.engine ? s.engine->us_tasks.load() + s.engine->us_recalc.load() + s.engine->us_publish.load() : 0;
  if (m.last > 0) {
    Sample x;
    x.t = now - m.t0;
    // latency samples that arrived since the last sample (the vector is trimmed: count from the end)
    const size_t n = s.input_latency_ms.size();
    const size_t fresh = std::min(n, m.lat_seen <= n ? n - m.lat_seen : n);
    std::vector<double> lat(s.input_latency_ms.end() - static_cast<std::ptrdiff_t>(fresh), s.input_latency_ms.end());
    x.lat_p50 = Pct(lat, 0.5);
    x.lat_p99 = Pct(lat, 0.99);
    const size_t nf = static_cast<size_t>(std::min<uint64_t>(s.frames_presented - m.frames0, s.frame_ms.size()));
    std::vector<double> fr(s.frame_ms.end() - static_cast<std::ptrdiff_t>(nf), s.frame_ms.end());
    double sum = 0;
    for (double v : fr) sum += v;
    x.frame_avg = fr.empty() ? 0 : sum / static_cast<double>(fr.size());
    x.frame_p99 = Pct(fr, 0.99);
    x.fps = static_cast<double>(s.frames_presented - m.frames0) / dt;
    x.cells_per_s = cells >= m.cells0 ? static_cast<double>(cells - m.cells0) / dt : 0;
    x.engine_busy = std::clamp(static_cast<double>(busy - m.busy0) / 1e6 / dt * 100.0, 0.0, 100.0);
    PROCESS_MEMORY_COUNTERS pm{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pm, sizeof(pm))) x.mem_mb = static_cast<double>(pm.WorkingSetSize) / 1048576.0;
    m.samples.push_back(x);
    while (m.samples.size() > kKeep) m.samples.pop_front();
  } else {
    m.t0 = now;
  }
  m.last = now;
  m.frames0 = s.frames_presented;
  m.cells0 = cells;
  m.busy0 = busy;
  m.lat_seen = s.input_latency_ms.size();
}

template <typename F>
std::vector<double> Col(const std::deque<Sample>& v, F f) {
  std::vector<double> out;
  out.reserve(v.size());
  for (const auto& x : v) out.push_back(f(x));
  return out;
}

}  // namespace

void RegisterLatencyCommands(AppState& s) {
  RegisterPane("latency", "LatencyMonitor", []() -> bool& { return M().open; });
  ui::Command c;
  c.id = "LatencyMonitor";
  c.label = TR("Latency\nMonitor");
  c.icon = ui::icon::Forecast;
  c.toggle = true;
  c.tip_desc = TR("Watch the time from Python to the screen, frame times, cells written per second and the engine's load, live.");
  ui::Commands().Add(c).execute = [&s](ui::Command& cmd) {
    Monitor& m = M();
    m.open = !m.open;
    cmd.pressed = m.open;
    if (m.open) {
      m.samples.clear();
      m.last = 0;
    }
    (void)s;
  };
}

// the series colors of the trading palette: cyan / yellow for latency, green / orange for frames, magenta / white load
const ImVec4 kLatP50 = PlotPaletteColor(PlotTheme::Dark, 2), kLatP99 = PlotPaletteColor(PlotTheme::Dark, 0);
const ImVec4 kFrameAvg = PlotPaletteColor(PlotTheme::Dark, 3), kFrameP99 = PlotPaletteColor(PlotTheme::Dark, 4);
const ImVec4 kEngine = PlotPaletteColor(PlotTheme::Dark, 1), kCells = PlotPaletteColor(PlotTheme::Dark, 5);

void DrawLatencyMonitor(AppState& s) {
  Monitor& m = M();
  if (ui::Command* c = ui::Commands().Find("LatencyMonitor")) c->pressed = m.open;
  if (!m.open) return;
  TakeSample(s);
  ui::RequestRedrawIn(0.25);
  ImGui::SetNextWindowSize(ImVec2(S(720), S(560)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Right);
  PushPlotTheme(PlotTheme::Dark);  // a trading monitor: the black terminal look
  if (ImGui::Begin(TR("Latency Monitor###yc_latency"), &m.open, ImGuiWindowFlags_NoSavedSettings)) {
    const Sample last = m.samples.empty() ? Sample{} : m.samples.back();
    // the numbers: labels dim, values bright in their series' colors
    const ImVec4 dim(0.55f, 0.55f, 0.55f, 1.0f);
    auto stat = [&](const char* label, const char* fmt, double v, const ImVec4& col, bool last_one = false) {
      ImGui::TextColored(dim, "%s", label);
      ImGui::SameLine();
      ImGui::TextColored(col, fmt, v);
      if (!last_one) ImGui::SameLine(0, S(18));
    };
    stat(TR("Python to screen p50"), "%.1f ms", last.lat_p50, kLatP50);
    stat("p99", "%.1f ms", last.lat_p99, kLatP99);
    stat(TR("Frame CPU avg"), "%.2f ms", last.frame_avg, kFrameAvg);
    stat("p99", "%.2f ms", last.frame_p99, kFrameP99);
    stat(TR("FPS:"), "%.0f", last.fps, ImVec4(1, 1, 1, 1), true);
    stat(TR("Cells:"), "%.2f M/s", last.cells_per_s / 1e6, kCells);
    stat(TR("Engine busy:"), "%.0f%%", last.engine_busy, kEngine);
    stat(TR("Memory:"), "%.0f MB", last.mem_mb, ImVec4(0.85f, 0.85f, 0.85f, 1), true);
    const auto t = Col(m.samples, [](const Sample& x) { return x.t; });
    const float h = (ImGui::GetContentRegionAvail().y - S(8)) / 3.0f;
    const double tmax = t.empty() ? 1 : t.back();
    struct Series {
      const char* name;
      std::vector<double> ys;
      ImVec4 color;
    };
    auto plot = [&](const char* id, const char* ylabel, std::initializer_list<Series> series) {
      if (ImPlot::BeginPlot(id, ImVec2(-1, h), ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes(nullptr, ylabel, ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_AutoFit);
        ImPlot::SetupAxisLimits(ImAxis_X1, std::max(0.0, tmax - 120), std::max(tmax, 10.0), ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthWest);
        for (const auto& sr : series) {
          ImPlotSpec sp;
          sp.LineColor = sr.color;
          sp.LineWeight = 2.0f;
          ImPlot::PlotLine(sr.name, t.data(), sr.ys.data(), static_cast<int>(sr.ys.size()), sp);
        }
        ImPlot::EndPlot();
      }
    };
    plot("##lat", "ms", {{TR("latency p50"), Col(m.samples, [](const Sample& x) { return x.lat_p50; }), kLatP50},
                         {TR("latency p99"), Col(m.samples, [](const Sample& x) { return x.lat_p99; }), kLatP99}});
    plot("##frame", "ms", {{TR("frame CPU avg"), Col(m.samples, [](const Sample& x) { return x.frame_avg; }), kFrameAvg},
                           {TR("frame CPU p99"), Col(m.samples, [](const Sample& x) { return x.frame_p99; }), kFrameP99}});
    plot("##load", "%", {{TR("engine busy %"), Col(m.samples, [](const Sample& x) { return x.engine_busy; }), kEngine},
                         {TR("cells / s (millions)"), Col(m.samples, [](const Sample& x) { return x.cells_per_s / 1e6; }), kCells}});
  }
  ImGui::End();
  PopPlotTheme(PlotTheme::Dark);
}

}  // namespace yc::app
