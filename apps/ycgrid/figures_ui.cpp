// SPDX-License-Identifier: Apache-2.0
// figures_ui.cpp - [YC] yc.plot: fast plot windows from any Python script (ROADMAP phase 4)
//
// A figure is a dockable window with one or more plots (subplots). Items (lines, scatter, bars, histograms, candles,
// heatmaps, shaded areas, lines across the plot, text) hold their data as plain double arrays sent through the
// shared-memory ring (engine/bind.h RawFrame), so millions of points cost no per-point allocation. Long lines are drawn
// from a min / max pyramid built when the data change (or LTTB on request), so a frame only touches about two points
// per pixel column whatever the zoom. A figure can follow a table: the active row's x value is marked on the plots, and
// clicking a plot selects the table row nearest to that x. Figures belong to this session (they aren't saved).
#include "app_state.h"
#include "core/i18n.h"
#include "dock/dock_layout.h"
#include "python_bridge.h"

#include "engine/bind.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "grid/grid.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "implot_internal.h"
#include "npy.h"
#include "plot_theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <limits>
#include <deque>
#include <map>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>

namespace yc::app {

using io::MV;
using ui::S;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Pyramid {
  int shift = 0;  // bucket = 2^shift points
  std::vector<double> x, lo, hi;
};

struct ScatterResult {
  std::vector<double> x, y;
  double key[6] = {0, 0, 0, 0, 0, 0};
  uint64_t version = 0;
  bool fit = false, has_bounds = false;
  ImPlotRect bounds;
};

struct PlotItem {
  std::string id, kind, label;
  uint32_t color = 0;  // ARGB; 0 = automatic
  float width = 1.5f, size = 3.0f;
  int marker = 0;  // 0 none, 1 circle, 2 square, 3 diamond, 4 up, 5 down, 6 cross, 7 plus
  uint32_t edge = 0;   // rects / polygons: outline color (ARGB; 0 = none)
  bool tag = false;    // lines: the last value as a tag on the y axis; hline / vline: each value
  std::string tag_text;  // hline / vline tags: text before the value ("STOP")
  int dash = 0;        // line / step / hline / vline: 0 solid, 1 dash, 2 dot
  bool fit_band = false;  // hband / vband: count in the auto-fit of their axis
  bool horizontal = false;  // errorbars drawn along x
  std::vector<ImU32> colors;  // per point / bar / rect, from the "c" column (ARGB)
  std::map<std::string, std::vector<double>> cols;
  std::vector<std::string> labels;
  int rows = 0, cols_n = 0;  // heatmap
  bool has_extent = false;   // heatmap placed in data coordinates (x0, x1, y0, y1) instead of cell indices
  double extent[4] = {0, 0, 0, 0};
  double vmin = 0, vmax = 0;
  bool vauto = true;
  int cmap = 0;  // ImPlotColormap
  int bins = -2;  // ImPlotBin_Sturges
  double bar_width = 0;  // 0 = automatic
  double ref = 0;        // shaded: y reference
  std::string text;
  size_t capacity = 0;     // appends keep the newest points (0 = all)
  std::string downsample = "minmax";  // "minmax", "lttb", "none"
  bool sorted = true;      // x ascending (enables visible-range search and downsampling)
  std::vector<Pyramid> pyr;
  std::vector<double> lttb_x, lttb_y;
  bool dirty = true;
  double built = 0;
  // large scatter plots: one point per occupied pixel cell of the current view (outliers stay visible)
  std::vector<double> sc_x, sc_y;
  std::shared_ptr<const std::vector<double>> sc_src_x, sc_src_y;  // the data the worker reads
  uint64_t sc_src_version = ~0ull, sc_bounds_version = ~0ull;
  ImPlotRect sc_bounds;
  std::shared_ptr<std::future<ScatterResult>> sc_job;
  bool want_fit = false;  // fit the plot again on the next frame (the data bounds came from the worker)
  double sc_key[6] = {0, 0, 0, 0, 0, 0};  // view limits and plot size the cells were built for
  uint64_t version = 0, sc_version = ~0ull;  // data changes
  double sc_built = 0;
  // [YC] fed by topics (candles_live / line_live / step_live): YCGrid builds the data from the updates itself
  std::string live_topic, live_vol_topic, live_vol_item;
  double live_seconds = 0;    // candles: the bar length
  int live_vol_ax = -1;       // candles: the plot of the volume bars (-1 = none)
  bool live_vol_cum = false;  // the volume topic is a running total (each update adds the difference)
  double live_vol_last = std::numeric_limits<double>::quiet_NaN(), live_vol_pending = 0;
  size_t live_keep = 0;       // bars (candles) or points (line / step) kept
  bool live_changed = false;
};

struct PlotAxes {
  std::string title, xlabel, ylabel, zlabel;
  float yaw = -0.85f, pitch = 0.5f, zoom = 1.0f;  // 3-D view of a surface (drag to turn, wheel to zoom)
  bool time_x = false, log_y = false, legend = true, equal = false;
  bool has_xlim = false, has_ylim = false;
  bool y_set = false;  // limits given from Python: a following plot keeps them instead of fitting y
  double x0 = 0, x1 = 1, y0 = 0, y1 = 1;
  double view[4] = {0, 1, 0, 1};  // the limits drawn last frame (x0, x1, y0, y1), reported in ui.state()
  float plot_rect[4] = {0, 0, 0, 0};  // the plot area drawn last frame, in window pixels (left, top, right, bottom)
  bool user_view = false;  // the user zoomed / panned or Python set limits: new data stop refitting (double-click fits again)
  double follow = 0;  // seconds (or x units) of the newest data to keep in view; 0 = off
  bool yfit_stable = false;  // following: y changes only when the data near the edges or shrink (no jitter)
  bool dragging = false;     // a handle was held last frame: the limits stay put while the user drags
  float ywidth = 0;          // the y axis's width in points (0 = as wide as its labels and tags)
  int ywidth_mode = 0;       // 0 auto (stable with yfit="stable"), 1 stable: it only grows, 2 as wide as the labels
  float ywidth_seen = 0;     // stable: the widest seen (pixels)
  std::vector<PlotItem> items;
  // draggable handles (lines and points) that report to Python while the user drags them
  struct Handle {
    std::string id, kind, label;  // kind: "hline", "vline", "point", "band" (y .. y1, moved by its body, resized by its edges)
    double x = 0, y = 0;
    double y1 = 0;  // band: the top
    bool resize = true;  // band: the edges can be dragged
    bool closable = false;  // lines: an "x" button at the plot's left edge sends a "close" event (cancel the order)
    bool fit = false;       // counts in the plot's fit (by default handles never move the view)
    int grab = 0;  // band being dragged: 1 body, 2 bottom edge, 3 top edge
    uint32_t color = 0;
    float width = 2.0f;
    bool held = false;
    double sent_at = 0;
  };
  std::vector<Handle> handles;
  bool selecting = false;  // Ctrl + drag: a selection rectangle
  ImPlotPoint sel0, sel1;
  double hover_at = 0;
  double synced_x = kNaN;  // the group's cursor drawn in this plot on the last frame (NaN = none)
  std::string cursor_tag;  // the linked row's tag text when YCGrid chose it (a day), for plot.figures
};

PlotTheme g_draw_theme = PlotTheme::Light;  // the figure being drawn (candle colors)

struct Figure {
  int64_t id = 0;
  std::string title;
  PlotTheme theme = PlotTheme::Light;  // "dark": the dark trading theme (black, red up / green down)
  int rows = 1, cols = 1;
  bool open = true, link_x = false, fit = true;
  std::string sync;  // cursor group: figures in one group show the hovered x in each other's plots
  std::string key;   // a name to find it again (plot.figure(key=...), plot.figures(), plot.close(key=...))
  int64_t owner = 0;      // the connection that made it (0 = YCGrid itself: a replay)
  std::string owner_name;  // "host", "script 1234"
  bool persist = true;     // false: closes when its owner goes away (host figures by default)
  bool alive = true;       // its owner is still connected
  std::vector<float> row_ratios, col_ratios;  // relative plot heights / widths (empty = equal)
  int dock_dir = -1;       // plot.dock into the main window: 0 left, 1 right, 2 top, 3 bottom, 4 tab (beside the sheets)
  float dock_ratio = 0.3f;
  int64_t dock_anchor = 0;  // plot.dock beside another figure (its id; 0 = beside the sheets); 4 = a tab in its place
  int dock_tries = 0;       // the anchor isn't docked yet (made this frame): tried again a few frames
  bool dock_pending = false;
  ImVec2 size{760, 480};
  bool place = false;  // move to pos (in points from the window's top-left) on the next frame
  ImVec2 pos{0, 0};
  std::vector<PlotAxes> axes;
  // the table it follows (cursor): sheet / table / x column
  std::string link_table, link_col;
  double cursor = kNaN;
  // events for Python: pushed to the host (callbacks registered there) or kept for polling (scripts)
  bool push = false;
  uint32_t want = 0;  // kEv* bits
  std::deque<MV> events;
  // right clicks go to Python (fig.on("click", right=True) or a context menu); ImPlot's own menu needs Shift then
  bool right = false;
  std::vector<std::string> menu;  // fig.context_menu: item texts ("" = a separator)
  bool menu_request = false;      // open the menu on this frame
  bool menu_close = false;        // plot.input chose an item: close the menu like a click would
  // following plots stop following while the user looks around: wheel, pan, box zoom; the "Paused" badge resumes
  bool follow_paused = false;
  int menu_input = 0;             // plot.input opened it: 1 = place it at menu_at, 2 = placed (menu_px)
  ImVec2 menu_px;
  int menu_ax = 0;
  ImPlotPoint menu_at;
};

// the connection a figure request comes from (set by the bridge around FigureRequest, on the UI thread)
int64_t g_request_owner = 0;
std::string g_request_owner_name;
bool g_request_owner_host = false;

constexpr uint32_t kEvClick = 1, kEvDouble = 2, kEvHover = 4, kEvSelect = 8, kEvDrag = 16;
PythonBridge* g_events_to = nullptr;  // the bridge that pushes events to the host (set while drawing)

void Emit(Figure& f, MV ev) {
  ev.Set("fig", MV::Int(f.id));
  if (f.push && g_events_to && g_events_to->HostReady()) {
    g_events_to->NotifyHost("plot.event", ev);
    return;
  }
  f.events.push_back(std::move(ev));
  while (f.events.size() > 2000) f.events.pop_front();
}

struct FigureStore {
  std::map<int64_t, Figure> figs;
  int64_t next = 1;
};

FigureStore& G() {
  static FigureStore g;
  return g;
}

// its window name (keyed figures: "###yc_figk_<key>", so their place survives in layouts)
std::string FigureTitle(const Figure& f) {
  return f.title + (f.alive ? "" : TR(" (stopped)")) + (f.key.empty() ? "###yc_fig" + std::to_string(f.id) : "###yc_figk_" + f.key);
}

ImVec4 Col(uint32_t argb) {
  return ImVec4(((argb >> 16) & 0xFF) / 255.f, ((argb >> 8) & 0xFF) / 255.f, (argb & 0xFF) / 255.f, ((argb >> 24) & 0xFF) / 255.f);
}

const std::vector<double>& ColOf(const PlotItem& it, const char* name) {
  static const std::vector<double> kEmpty;
  auto f = it.cols.find(name);
  return f == it.cols.end() ? kEmpty : f->second;
}

void BuildPyramid(PlotItem& it) {
  it.pyr.clear();
  const auto& x = ColOf(it, "x");
  const auto& y = ColOf(it, "y");
  const size_t n = std::min(x.size(), y.size());
  it.sorted = true;
  for (size_t i = 1; i < x.size() && it.sorted; ++i) it.sorted = !(x[i] < x[i - 1]);
  if (n < 200000 || !it.sorted || it.kind != "line") return;
  // level 0: buckets of 64 points; each next level merges two buckets
  Pyramid p;
  p.shift = 6;
  const size_t nb = (n + 63) >> 6;
  p.x.resize(nb);
  p.lo.resize(nb);
  p.hi.resize(nb);
  for (size_t b = 0; b < nb; ++b) {
    const size_t a = b << 6, e = std::min(n, a + 64);
    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
    for (size_t i = a; i < e; ++i) {
      if (std::isnan(y[i])) continue;
      lo = std::min(lo, y[i]);
      hi = std::max(hi, y[i]);
    }
    p.x[b] = x[a];
    p.lo[b] = lo;
    p.hi[b] = hi;
  }
  it.pyr.push_back(std::move(p));
  while (it.pyr.back().x.size() > 2048) {
    const Pyramid& q = it.pyr.back();
    Pyramid r;
    r.shift = q.shift + 1;
    const size_t m = (q.x.size() + 1) / 2;
    r.x.resize(m);
    r.lo.resize(m);
    r.hi.resize(m);
    for (size_t b = 0; b < m; ++b) {
      const size_t a = b * 2, c = std::min(q.x.size() - 1, a + 1);
      r.x[b] = q.x[a];
      r.lo[b] = std::min(q.lo[a], q.lo[c]);
      r.hi[b] = std::max(q.hi[a], q.hi[c]);
    }
    it.pyr.push_back(std::move(r));
  }
}

// Largest-Triangle-Three-Buckets (Steinarsson): `out` points that keep the shape of the line
void Lttb(const std::vector<double>& x, const std::vector<double>& y, size_t out, std::vector<double>* ox, std::vector<double>* oy) {
  const size_t n = std::min(x.size(), y.size());
  ox->clear();
  oy->clear();
  if (out >= n || out < 3) {
    ox->assign(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
    oy->assign(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(n));
    return;
  }
  const double every = static_cast<double>(n - 2) / static_cast<double>(out - 2);
  size_t a = 0;
  ox->push_back(x[0]);
  oy->push_back(y[0]);
  for (size_t i = 0; i < out - 2; ++i) {
    size_t avg_start = static_cast<size_t>(std::floor((i + 1) * every)) + 1, avg_end = std::min(n, static_cast<size_t>(std::floor((i + 2) * every)) + 1);
    double ax = 0, ay = 0;
    for (size_t j = avg_start; j < avg_end; ++j) {
      ax += x[j];
      ay += y[j];
    }
    const double cnt = static_cast<double>(std::max<size_t>(1, avg_end - avg_start));
    ax /= cnt;
    ay /= cnt;
    const size_t r0 = static_cast<size_t>(std::floor(i * every)) + 1, r1 = static_cast<size_t>(std::floor((i + 1) * every)) + 1;
    double best = -1;
    size_t pick = r0;
    for (size_t j = r0; j < r1 && j < n; ++j) {
      const double area = std::fabs((x[a] - ax) * (y[j] - y[a]) - (x[a] - x[j]) * (ay - y[a]));
      if (area > best) {
        best = area;
        pick = j;
      }
    }
    ox->push_back(x[pick]);
    oy->push_back(y[pick]);
    a = pick;
  }
  ox->push_back(x[n - 1]);
  oy->push_back(y[n - 1]);
}

void Prepare(PlotItem& it) {
  if (!it.dirty) return;
  // a long stream rebuilds at most twice a second
  const double now = ImGui::GetTime();
  if (!it.pyr.empty() && now - it.built < 0.5) return;
  it.dirty = false;
  it.built = now;
  ++it.version;
  BuildPyramid(it);
  if (it.kind == "line" && it.downsample == "lttb" && ColOf(it, "x").size() > 20000) Lttb(ColOf(it, "x"), ColOf(it, "y"), 4000, &it.lttb_x, &it.lttb_y);
  else it.lttb_x.clear(), it.lttb_y.clear();
}

ImPlotSpec SpecOf(const PlotItem& it) {
  ImPlotSpec sp;
  if (it.color) {
    sp.LineColor = Col(it.color);
    sp.FillColor = Col(it.color);
    sp.MarkerLineColor = Col(it.color);  // scatter takes its legend color from the marker edge
    sp.MarkerFillColor = Col(it.color);
  }
  sp.LineWeight = it.width;
  sp.MarkerSize = it.size;
  static const ImPlotMarker kMarkers[] = {ImPlotMarker_None, ImPlotMarker_Circle, ImPlotMarker_Square, ImPlotMarker_Diamond, ImPlotMarker_Up,
                                         ImPlotMarker_Down, ImPlotMarker_Cross, ImPlotMarker_Plus};
  sp.Marker = kMarkers[std::clamp(it.marker, 0, 7)];
  if (it.kind == "scatter" && it.marker == 0) sp.Marker = ImPlotMarker_Circle;
  return sp;
}

// a spec whose per-point colors are the item's "c" column (bars, scatter) when it has one for every point
ImPlotSpec SpecWithColors(PlotItem& it, int n) {
  ImPlotSpec sp = SpecOf(it);
  if (!it.colors.empty() && static_cast<int>(it.colors.size()) >= n) {
    sp.FillColors = it.colors.data();
    sp.MarkerFillColors = it.colors.data();
    sp.MarkerLineColors = it.colors.data();
  }
  return sp;
}

ImU32 Argb(uint32_t argb) {
  return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF ? (argb >> 24) & 0xFF : 255);
}

// rects: many rectangles in data coordinates, each with its color and text (footprint, volume profile, zones)
void DrawRects(const PlotItem& it, const char* label) {
  const auto& x0 = ColOf(it, "x0");
  const auto& x1 = ColOf(it, "x1");
  const auto& y0 = ColOf(it, "y0");
  const auto& y1 = ColOf(it, "y1");
  const auto& v = ColOf(it, "v");
  const size_t n = std::min({x0.size(), x1.size(), y0.size(), y1.size()});
  ImPlotSpec legend;
  legend.FillColor = it.color ? Col(it.color) : ImVec4(0.3f, 0.5f, 0.9f, 0.6f);
  ImPlot::PlotDummy(label, legend);  // a legend entry (toggling it hides the rects)
  if (n == 0 || !ImPlot::BeginItem(label)) return;
  if (ImPlot::FitThisFrame()) {
    for (size_t i = 0; i < n; ++i) {
      ImPlot::FitPoint(ImPlotPoint(x0[i], y0[i]));
      ImPlot::FitPoint(ImPlotPoint(x1[i], y1[i]));
    }
  }
  const ImPlotRect lim = ImPlot::GetPlotLimits();
  ImDrawList* dl = ImPlot::GetPlotDrawList();
  const ImU32 fill = it.color ? Argb(it.color) : IM_COL32(77, 128, 230, 150);
  const ImU32 edge = Argb(it.edge);
  const char* fmt = it.text.empty() ? "%g" : it.text.c_str();
  const bool has_text = !it.labels.empty() || !v.empty();
  const float th = ImGui::GetTextLineHeight();
  for (size_t i = 0; i < n; ++i) {
    const double ax0 = std::min(x0[i], x1[i]), ax1 = std::max(x0[i], x1[i]), ay0 = std::min(y0[i], y1[i]), ay1 = std::max(y0[i], y1[i]);
    if (ax1 < lim.X.Min || ax0 > lim.X.Max || ay1 < lim.Y.Min || ay0 > lim.Y.Max) continue;  // off the view
    ImVec2 p0 = ImPlot::PlotToPixels(ax0, ay1), p1 = ImPlot::PlotToPixels(ax1, ay0);
    if (p1.x - p0.x < 1.0f) p1.x = p0.x + 1.0f;  // thinner than a pixel: still seen
    if (p1.y - p0.y < 1.0f) p1.y = p0.y + 1.0f;
    dl->AddRectFilled(p0, p1, i < it.colors.size() ? it.colors[i] : fill);
    if (it.edge) dl->AddRect(p0, p1, edge);
    if (!has_text || p1.y - p0.y < th * 0.9f) continue;  // the text only where it fits
    char buf[96];
    const char* t = nullptr;
    if (i < it.labels.size() && !it.labels[i].empty()) t = it.labels[i].c_str();
    else if (i < v.size() && std::isfinite(v[i])) std::snprintf(buf, sizeof(buf), fmt, v[i]), t = buf;
    if (!t) continue;
    const ImVec2 ts = ImGui::CalcTextSize(t);
    if (ts.x > p1.x - p0.x - 2) continue;
    const ImU32 bg = i < it.colors.size() ? it.colors[i] : fill;
    const float lum = (0.299f * (bg & 0xFF) + 0.587f * ((bg >> 8) & 0xFF) + 0.114f * ((bg >> 16) & 0xFF)) / 255.0f;
    const bool dark_bg = lum < 0.5f && ((bg >> 24) & 0xFF) > 96;
    const ImU32 ink = dark_bg ? IM_COL32(240, 240, 240, 255) : ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f), ink, t);
  }
  ImPlot::EndItem();
}

// polygons: x, y with NaN between polygons; arrows: x0, y0 -> x1, y1
void DrawShapes(const PlotItem& it, const char* label) {
  ImPlotSpec legend;
  legend.FillColor = it.color ? Col(it.color) : ImVec4(0.3f, 0.5f, 0.9f, 0.5f);
  ImPlot::PlotDummy(label, legend);
  if (!ImPlot::BeginItem(label)) return;
  ImDrawList* dl = ImPlot::GetPlotDrawList();
  const ImU32 col = it.color ? Argb(it.color) : IM_COL32(77, 128, 230, 130);
  if (it.kind == "polygon") {
    const auto& x = ColOf(it, "x");
    const auto& y = ColOf(it, "y");
    const size_t n = std::min(x.size(), y.size());
    if (ImPlot::FitThisFrame()) {
      for (size_t i = 0; i < n; ++i) {
        if (std::isfinite(x[i]) && std::isfinite(y[i])) ImPlot::FitPoint(ImPlotPoint(x[i], y[i]));
      }
    }
    std::vector<ImVec2> pts;
    auto flush = [&] {
      if (pts.size() >= 3) {
        dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), col);
        if (it.edge) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), Argb(it.edge), it.width, ImDrawFlags_Closed);
      }
      pts.clear();
    };
    for (size_t i = 0; i < n; ++i) {
      if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
        flush();
        continue;
      }
      pts.push_back(ImPlot::PlotToPixels(x[i], y[i]));
    }
    flush();
  } else {  // arrows
    const auto& x0 = ColOf(it, "x0");
    const auto& y0 = ColOf(it, "y0");
    const auto& x1 = ColOf(it, "x1");
    const auto& y1 = ColOf(it, "y1");
    const size_t n = std::min({x0.size(), y0.size(), x1.size(), y1.size()});
    const ImU32 line = it.color ? Argb(it.color) : ImGui::GetColorU32(ImGuiCol_Text);
    const float head = S(4.0f) + it.width * 2.5f;
    for (size_t i = 0; i < n; ++i) {
      if (ImPlot::FitThisFrame()) ImPlot::FitPoint(ImPlotPoint(x0[i], y0[i])), ImPlot::FitPoint(ImPlotPoint(x1[i], y1[i]));
      const ImVec2 a = ImPlot::PlotToPixels(x0[i], y0[i]), b = ImPlot::PlotToPixels(x1[i], y1[i]);
      const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
      if (len < 1.0f) continue;
      const float ux = dx / len, uy = dy / len;
      const ImVec2 base(b.x - ux * head, b.y - uy * head);
      dl->AddLine(a, base, line, it.width);
      dl->AddTriangleFilled(b, ImVec2(base.x - uy * head * 0.5f, base.y + ux * head * 0.5f), ImVec2(base.x + uy * head * 0.5f, base.y - ux * head * 0.5f), line);
    }
  }
  ImPlot::EndItem();
}

// buy / sell marks on a candlestick chart: side > 0 an up triangle just under y (the price), side < 0 a down triangle
// just above it; the label under / over the triangle. color = buy, edge = sell (ARGB), size = the triangle's height
void DrawSignals(const PlotItem& it, const char* label) {
  const auto& x = ColOf(it, "x");
  const auto& y = ColOf(it, "y");
  const auto& sd = ColOf(it, "side");
  const size_t n = std::min({x.size(), y.size(), sd.size()});
  const ImU32 buy = it.color ? Argb(it.color) : IM_COL32(224, 64, 64, 255), sell = it.edge ? Argb(it.edge) : IM_COL32(32, 176, 80, 255);
  ImPlotSpec legend;
  legend.FillColor = ImGui::ColorConvertU32ToFloat4(buy);
  ImPlot::PlotDummy(label, legend);
  if (!ImPlot::BeginItem(label)) return;
  if (ImPlot::FitThisFrame()) {
    for (size_t i = 0; i < n; ++i) {
      if (std::isfinite(x[i]) && std::isfinite(y[i])) ImPlot::FitPoint(ImPlotPoint(x[i], y[i]));
    }
  }
  const ImPlotRect lim = ImPlot::GetPlotLimits();
  ImDrawList* dl = ImPlot::GetPlotDrawList();
  const float h = S(std::clamp(it.size, 3.0f, 30.0f)), half = h * 0.6f, gap = S(3.0f);
  const ImU32 ink = ImPlot::GetStyleColorU32(ImPlotCol_InlayText);
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(x[i]) || !std::isfinite(y[i]) || x[i] < lim.X.Min || x[i] > lim.X.Max) continue;
    const ImVec2 p = ImPlot::PlotToPixels(x[i], y[i]);
    const bool is_buy = sd[i] > 0;
    const float tip = is_buy ? p.y + gap : p.y - gap, base = is_buy ? tip + h : tip - h;
    dl->AddTriangleFilled(ImVec2(p.x, tip), ImVec2(p.x - half, base), ImVec2(p.x + half, base), is_buy ? buy : sell);
    if (i < it.labels.size() && !it.labels[i].empty()) {
      const ImVec2 ts = ImGui::CalcTextSize(it.labels[i].c_str());
      dl->AddText(ImVec2(p.x - ts.x * 0.5f, is_buy ? base + S(1.0f) : base - S(1.0f) - ts.y), ink, it.labels[i].c_str());
    }
  }
  ImPlot::EndItem();
}

std::pair<size_t, size_t> Visible(const std::vector<double>& x, double x0, double x1) {
  const size_t a = static_cast<size_t>(std::lower_bound(x.begin(), x.end(), x0) - x.begin());
  const size_t b = static_cast<size_t>(std::upper_bound(x.begin(), x.end(), x1) - x.begin());
  return {a > 0 ? a - 1 : 0, std::min(x.size(), b + 1)};
}

// a polyline in pixels with a dash (1) or dot (2) pattern that runs on across segments
void DashedPolyline(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col, float width, int dash) {
  const float on = dash == 2 ? std::max(1.5f, width) : S(7.0f), off = dash == 2 ? S(3.5f) : S(4.5f);
  float phase = 0;  // where in the on / off pattern
  for (int i = 0; i + 1 < n; ++i) {
    ImVec2 a = pts[i];
    const ImVec2 b = pts[i + 1];
    float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    if (!(len > 0)) continue;
    const ImVec2 u((b.x - a.x) / len, (b.y - a.y) / len);
    while (len > 0) {
      const bool drawing = phase < on;
      const float step = std::min(len, drawing ? on - phase : on + off - phase);
      const ImVec2 e(a.x + u.x * step, a.y + u.y * step);
      if (drawing) dl->AddLine(a, e, col, width);
      a = e;
      len -= step;
      phase += step;
      if (phase >= on + off) phase = 0;
    }
  }
}

// PlotLine / PlotStairs, or a dashed / dotted line drawn by YCGrid (ImPlot has no dashes)
void PlotLineStyled(const PlotItem& it, const char* label, const double* x, const double* y, int n, const ImPlotSpec& sp) {
  if (it.dash == 0) {
    if (it.kind == "step") ImPlot::PlotStairs(label, x, y, n, sp);
    else ImPlot::PlotLine(label, x, y, n, sp);
    return;
  }
  ImPlotSpec legend = sp;
  ImPlot::PlotDummy(label, legend);
  if (!ImPlot::BeginItem(label)) return;
  if (ImPlot::FitThisFrame()) {
    for (int i = 0; i < n; ++i) {
      if (std::isfinite(x[i]) && std::isfinite(y[i])) ImPlot::FitPoint(ImPlotPoint(x[i], y[i]));
    }
  }
  const ImU32 col = it.color ? Argb(it.color) : ImGui::GetColorU32(ImPlot::GetLastItemColor());
  static std::vector<ImVec2> pts;
  pts.clear();
  for (int i = 0; i < n; ++i) {
    if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
      DashedPolyline(ImPlot::GetPlotDrawList(), pts.data(), static_cast<int>(pts.size()), col, it.width, it.dash);
      pts.clear();
      continue;
    }
    const ImVec2 q = ImPlot::PlotToPixels(x[i], y[i]);
    if (it.kind == "step" && !pts.empty()) pts.push_back(ImVec2(q.x, pts.back().y));
    pts.push_back(q);
  }
  DashedPolyline(ImPlot::GetPlotDrawList(), pts.data(), static_cast<int>(pts.size()), col, it.width, it.dash);
  ImPlot::EndItem();
}

// hband / vband: bands across the whole plot (y0..y1 at every x, or x0..x1 at every y), with a label inside
void DrawBands(const PlotItem& it, const char* label) {
  const bool h = it.kind == "hband";
  const auto& a0 = ColOf(it, h ? "y0" : "x0");
  const auto& a1 = ColOf(it, h ? "y1" : "x1");
  const size_t n = std::min(a0.size(), a1.size());
  ImPlotSpec legend;
  legend.FillColor = it.color ? Col(it.color) : ImVec4(0.3f, 0.5f, 0.9f, 0.25f);
  ImPlot::PlotDummy(label, legend);
  if (!ImPlot::BeginItem(label)) return;
  if (it.fit_band && ImPlot::FitThisFrame()) {
    ImPlotPlot* plot = ImPlot::GetCurrentPlot();
    for (size_t i = 0; i < n; ++i) {
      plot->Axes[h ? ImAxis_Y1 : ImAxis_X1].ExtendFit(a0[i]);
      plot->Axes[h ? ImAxis_Y1 : ImAxis_X1].ExtendFit(a1[i]);
    }
  }
  const ImPlotRect lim = ImPlot::GetPlotLimits();
  ImDrawList* dl = ImPlot::GetPlotDrawList();
  const ImU32 fill = it.color ? Argb(it.color) : IM_COL32(77, 128, 230, 64);
  for (size_t i = 0; i < n; ++i) {
    const double lo = std::min(a0[i], a1[i]), hi = std::max(a0[i], a1[i]);
    const ImVec2 p0 = h ? ImPlot::PlotToPixels(lim.X.Min, hi) : ImPlot::PlotToPixels(lo, lim.Y.Max);
    const ImVec2 p1 = h ? ImPlot::PlotToPixels(lim.X.Max, lo) : ImPlot::PlotToPixels(hi, lim.Y.Min);
    dl->AddRectFilled(p0, p1, i < it.colors.size() ? it.colors[i] : fill);
    if (it.edge) dl->AddRect(p0, p1, Argb(it.edge));
    if (i < it.labels.size() && !it.labels[i].empty()) {
      const ImVec2 ts = ImGui::CalcTextSize(it.labels[i].c_str());
      if ((h && p1.y - p0.y >= ts.y) || (!h && p1.x - p0.x >= ts.x)) {
        dl->AddText(ImVec2(p0.x + S(4), h ? (p0.y + p1.y - ts.y) * 0.5f : p0.y + S(4)), ImPlot::GetStyleColorU32(ImPlotCol_InlayText),
                    it.labels[i].c_str());
      }
    }
  }
  ImPlot::EndItem();
}

void DrawLine(PlotItem& it, const char* label, float px_width) {
  const auto& x = ColOf(it, "x");
  const auto& y = ColOf(it, "y");
  const int n = static_cast<int>(std::min(x.size(), y.size()));
  if (n == 0) {
    ImPlot::PlotDummy(label);
    return;
  }
  const ImPlotSpec sp = SpecOf(it);
  if (!it.sorted || it.downsample == "none") {
    PlotLineStyled(it, label, x.data(), y.data(), n, sp);
    return;
  }
  const ImPlotRect lim = ImPlot::GetPlotLimits();
  // while the x axis fits (first frame, double-click, fig.fit()), every point counts, not just the ones in view
  const bool fit_x = ImPlot::GetCurrentPlot()->Axes[ImAxis_X1].FitThisFrame;
  const auto [a, b] = fit_x ? std::pair<size_t, size_t>{0, static_cast<size_t>(n)} : Visible(x, lim.X.Min, lim.X.Max);
  const size_t count = b > a ? b - a : 0;
  const size_t budget = static_cast<size_t>(std::max(256.f, px_width)) * 2;
  if (count <= budget * 32 || it.kind == "step") {
    PlotLineStyled(it, label, x.data() + a, y.data() + a, static_cast<int>(count), sp);
    return;
  }
  if (it.downsample == "lttb" && !it.lttb_x.empty() && count > x.size() / 2) {
    PlotLineStyled(it, label, it.lttb_x.data(), it.lttb_y.data(), static_cast<int>(it.lttb_x.size()), sp);
    return;
  }
  // min / max per bucket from the pyramid level that gives about two points per pixel column
  const Pyramid* lv = nullptr;
  for (const auto& p : it.pyr) {
    lv = &p;
    if ((count >> p.shift) <= budget) break;
  }
  if (!lv) {
    PlotLineStyled(it, label, x.data() + a, y.data() + a, static_cast<int>(count), sp);
    return;
  }
  static std::vector<double> dx, dy;
  dx.clear();
  dy.clear();
  const size_t b0 = a >> lv->shift, b1 = std::min(lv->x.size(), (b >> lv->shift) + 1);
  for (size_t k = b0; k < b1; ++k) {
    if (!std::isfinite(lv->lo[k])) continue;
    const double xn = k + 1 < lv->x.size() ? lv->x[k + 1] : x[static_cast<size_t>(n - 1)];
    dx.push_back(lv->x[k]);
    dy.push_back(lv->lo[k]);
    dx.push_back((lv->x[k] + xn) * 0.5);
    dy.push_back(lv->hi[k]);
  }
  PlotLineStyled(it, label, dx.data(), dy.data(), static_cast<int>(dx.size()), sp);
}

void DrawCandles(const PlotItem& it, const char* label, float px_width) {
  const auto& t = ColOf(it, "x");
  const auto& o = ColOf(it, "open");
  const auto& h = ColOf(it, "high");
  const auto& l = ColOf(it, "low");
  const auto& c = ColOf(it, "close");
  const size_t n = std::min({t.size(), o.size(), h.size(), l.size(), c.size()});
  ImPlotSpec legend;  // the legend swatch in the up color (not the palette's next color)
  legend.FillColor = it.color ? Col(it.color) : ImGui::ColorConvertU32ToFloat4(CandleUpColor(g_draw_theme));
  ImPlot::PlotDummy(label, legend);
  if (n == 0 || !ImPlot::BeginItem(label)) return;
  if (ImPlot::FitThisFrame()) {
    for (size_t i = 0; i < n; ++i) {
      ImPlot::FitPoint(ImPlotPoint(t[i], l[i]));
      ImPlot::FitPoint(ImPlotPoint(t[i], h[i]));
    }
  }
  ImDrawList* dl = ImPlot::GetPlotDrawList();
  const ImPlotRect lim = ImPlot::GetPlotLimits();
  auto [a, b] = Visible(t, lim.X.Min, lim.X.Max);
  const size_t count = b > a ? b - a : 0;
  // several candles per pixel: merge them (first open, max high, min low, last close)
  const size_t group = std::max<size_t>(1, count / static_cast<size_t>(std::max(64.f, px_width / 3)));
  const double dt = n > 1 ? (t[n - 1] - t[0]) / static_cast<double>(n - 1) * static_cast<double>(group) : 1.0;
  // red up, green down (Taiwanese convention); a color given for the item replaces the up color
  const ImU32 up = it.color ? ImGui::GetColorU32(Col(it.color)) : CandleUpColor(g_draw_theme);
  const ImU32 down = CandleDownColor(g_draw_theme);
  for (size_t i = a; i < b; i += group) {
    const size_t e = std::min(b, i + group);
    double hi = -std::numeric_limits<double>::infinity(), lo = -hi;
    for (size_t j = i; j < e; ++j) {
      hi = std::max(hi, h[j]);
      lo = std::min(lo, l[j]);
    }
    const double op = o[i], cl = c[e - 1];
    const ImU32 col = cl >= op ? up : down;
    const ImVec2 ph = ImPlot::PlotToPixels(t[i], hi), pl = ImPlot::PlotToPixels(t[i], lo);
    dl->AddLine(ph, pl, col, 1.0f);
    const ImVec2 p0 = ImPlot::PlotToPixels(t[i] - dt * 0.3, std::max(op, cl)), p1 = ImPlot::PlotToPixels(t[i] + dt * 0.3, std::min(op, cl));
    dl->AddRectFilled(p0, ImVec2(std::max(p1.x, p0.x + 1), std::max(p1.y, p0.y + 1)), col);
  }
  ImPlot::EndItem();
}

// Scatter plots with more points than pixels: keep the first point of every 2 x 2 pixel cell in view. Every occupied cell
// is drawn, so the shape, density edges and outliers look the same as drawing all points. Rebuilt when the view or the
// data changes (at most 20 times a second while panning; the previous cells stay drawn meanwhile).
// the cells of a big scatter plot, worked out on a worker thread from a copy of the data (hard rule 1: the UI thread
// never scans millions of points); lim with NaN = fit: the data bounds are the view
ScatterResult Decimate(std::shared_ptr<const std::vector<double>> xs, std::shared_ptr<const std::vector<double>> ys, ImPlotRect lim,
                       ImVec2 sz, bool log_y, bool bounds, uint64_t version, bool fit, const double* key) {
  ScatterResult r;
  r.version = version;
  r.fit = fit;
  std::copy(key, key + 6, r.key);
  const auto& x = *xs;
  const auto& y = *ys;
  const size_t n = std::min(x.size(), y.size());
  if (bounds) {
    ImPlotRect b(INFINITY, -INFINITY, INFINITY, -INFINITY);
    for (size_t i = 0; i < n; ++i) {
      if (!std::isfinite(x[i]) || !std::isfinite(y[i])) continue;
      b.X.Min = std::min(b.X.Min, x[i]);
      b.X.Max = std::max(b.X.Max, x[i]);
      b.Y.Min = std::min(b.Y.Min, y[i]);
      b.Y.Max = std::max(b.Y.Max, y[i]);
    }
    if (!(b.X.Max >= b.X.Min)) b = ImPlotRect(0, 1, 0, 1);
    r.has_bounds = true;
    r.bounds = b;
    if (std::isnan(lim.X.Min)) lim = b;
  }
  if (std::isnan(lim.X.Min)) return r;
  const int gw = std::max(1, static_cast<int>(sz.x / 2)), gh = std::max(1, static_cast<int>(sz.y / 2));
  const double sx = gw / std::max(lim.X.Max - lim.X.Min, 1e-300), sy = gh / std::max(lim.Y.Max - lim.Y.Min, 1e-300);
  std::vector<uint8_t> seen(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
  for (size_t i = 0; i < n; ++i) {
    const double px = x[i], py = y[i];
    if (!(px >= lim.X.Min && px <= lim.X.Max && py >= lim.Y.Min && py <= lim.Y.Max)) continue;
    double fy = py;
    if (log_y && py > 0 && lim.Y.Min > 0) fy = lim.Y.Min + (std::log10(py) - std::log10(lim.Y.Min)) / (std::log10(lim.Y.Max) - std::log10(lim.Y.Min)) * (lim.Y.Max - lim.Y.Min);
    const int cx = std::min(gw - 1, static_cast<int>((px - lim.X.Min) * sx));
    const int cy = std::min(gh - 1, static_cast<int>((fy - lim.Y.Min) * sy));
    uint8_t& c = seen[static_cast<size_t>(cy) * static_cast<size_t>(gw) + static_cast<size_t>(cx)];
    if (c) continue;
    c = 1;
    r.x.push_back(px);
    r.y.push_back(py);
  }
  return r;
}

// Scatter plots with more points than pixels: keep the first point of every 2 x 2 pixel cell in view. Every occupied cell
// is drawn, so the shape, density edges and outliers look the same as drawing all points. Rebuilt on a worker when the
// view or the data changes (at most 20 times a second while panning; the previous cells stay drawn meanwhile).
void DrawBigScatter(PlotItem& it, const char* label, const ImPlotSpec& sp, int n) {
  ImPlotPlot* plot = ImPlot::GetCurrentPlot();
  const bool fit = plot->Axes[ImAxis_X1].FitThisFrame || plot->Axes[ImAxis_Y1].FitThisFrame;
  if (it.sc_job && it.sc_job->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    ScatterResult r = it.sc_job->get();
    it.sc_job.reset();
    it.sc_x = std::move(r.x);
    it.sc_y = std::move(r.y);
    std::copy(r.key, r.key + 6, it.sc_key);
    it.sc_version = r.version;
    if (r.has_bounds) {
      it.sc_bounds = r.bounds;
      it.sc_bounds_version = r.version;
      if (r.fit) it.want_fit = true;  // the bounds weren't known when the plot fitted: fit again
    }
  }
  const bool bounds_known = it.sc_bounds_version == it.version;
  ImPlotRect lim = ImPlot::GetPlotLimits();
  if (fit) lim = bounds_known ? it.sc_bounds : ImPlotRect(NAN, NAN, NAN, NAN);  // the cells cover all data
  const ImVec2 sz = ImPlot::GetPlotSize();
  const double key[6] = {lim.X.Min, lim.X.Max, lim.Y.Min, lim.Y.Max, sz.x, sz.y};
  const bool same = std::equal(key, key + 6, it.sc_key) && it.sc_version == it.version;
  const double now = ImGui::GetTime();
  if (!same && !it.sc_job && (fit || it.sc_x.empty() || it.sc_version != it.version || now - it.sc_built > 0.05)) {
    if (it.sc_src_version != it.version) {  // a copy the worker can read while new data arrive
      it.sc_src_x = std::make_shared<const std::vector<double>>(ColOf(it, "x"));
      it.sc_src_y = std::make_shared<const std::vector<double>>(ColOf(it, "y"));
      it.sc_src_version = it.version;
    }
    it.sc_built = now;
    const bool log_y = plot->Axes[ImAxis_Y1].Scale == ImPlotScale_Log10;
    auto prom = std::make_shared<std::promise<ScatterResult>>();
    it.sc_job = std::make_shared<std::future<ScatterResult>>(prom->get_future());
    std::thread([prom, xs = it.sc_src_x, ys = it.sc_src_y, lim, sz, log_y, bounds = !bounds_known, version = it.version, fit,
                 k = std::array<double, 6>{key[0], key[1], key[2], key[3], key[4], key[5]}] {
      prom->set_value(Decimate(xs, ys, lim, sz, log_y, bounds, version, fit, k.data()));
    }).detach();  // dropping the future never waits for it
  }
  ImPlot::PlotScatter(label, it.sc_x.data(), it.sc_y.data(), static_cast<int>(it.sc_x.size()), sp);
  if (fit && bounds_known) {
    ImPlot::FitPoint(ImPlotPoint(it.sc_bounds.X.Min, it.sc_bounds.Y.Min));
    ImPlot::FitPoint(ImPlotPoint(it.sc_bounds.X.Max, it.sc_bounds.Y.Max));
  }
  (void)n;
}

// y-axis tags of one plot (line / hline tags, handle labels) are drawn together at the end of the plot: tags whose
// prices are close are stacked apart instead of covering each other (their lines stay at the true price)
struct YTag {
  double y;
  ImVec4 col;
  std::string text;
};
std::vector<YTag> g_ytags;

void QueueTagY(double y, ImVec4 col, std::string text) {
  if (std::isfinite(y)) g_ytags.push_back({y, col, std::move(text)});
}

void FlushTagsY() {
  if (g_ytags.empty()) return;
  const ImPlotRect lim = ImPlot::GetPlotLimits();
  std::vector<std::pair<float, size_t>> at;  // pixel y, index
  for (size_t i = 0; i < g_ytags.size(); ++i) {
    if (g_ytags[i].y < lim.Y.Min || g_ytags[i].y > lim.Y.Max) continue;  // off the plot: no tag, like ImPlot
    at.emplace_back(ImPlot::PlotToPixels(lim.X.Min, g_ytags[i].y).y, i);
  }
  std::sort(at.begin(), at.end());
  const float h = ImGui::GetTextLineHeight() + ImPlot::GetStyle().AnnotationPadding.y * 2 + 1;
  const float top = ImPlot::GetPlotPos().y + h * 0.5f, bottom = ImPlot::GetPlotPos().y + ImPlot::GetPlotSize().y - h * 0.5f;
  for (size_t k = 1; k < at.size(); ++k) at[k].first = std::max(at[k].first, at[k - 1].first + h);  // push down
  if (!at.empty() && at.back().first > bottom) {  // over the bottom: push up from there
    at.back().first = bottom;
    for (size_t k = at.size() - 1; k-- > 0;) at[k].first = std::min(at[k].first, at[k + 1].first - h);
  }
  for (auto& [py, i] : at) {
    py = std::clamp(py, top, std::max(top, bottom));
    const double y = ImPlot::PixelsToPlot(ImVec2(ImPlot::GetPlotPos().x, py)).y;
    ImPlot::TagY(y, g_ytags[i].col, "%s", g_ytags[i].text.c_str());
  }
  g_ytags.clear();
}

void DrawItem(PlotItem& it, const std::string& uid, float px_width) {
  const std::string label = (it.label.empty() ? std::string() : it.label) + "##" + uid;
  const ImPlotSpec sp = SpecOf(it);
  const auto& x = ColOf(it, "x");
  const auto& y = ColOf(it, "y");
  const int n = static_cast<int>(std::min(x.size(), y.size()));
  if (it.kind == "line" || it.kind == "step") {
    DrawLine(it, label.c_str(), px_width);
    if (it.tag && n > 0 && std::isfinite(y[static_cast<size_t>(n - 1)])) {  // the last value on the y axis
      const ImVec4 c = it.color ? Col(it.color) : ImPlot::GetLastItemColor();
      char tag[96];
      std::snprintf(tag, sizeof(tag), it.text.empty() ? "%g" : it.text.c_str(), y[static_cast<size_t>(n - 1)]);
      QueueTagY(y[static_cast<size_t>(n - 1)], c, tag);
    }
  } else if (it.kind == "scatter") {
    if (n > 100000 && it.downsample != "none") DrawBigScatter(it, label.c_str(), sp, n);
    else ImPlot::PlotScatter(label.c_str(), x.data(), y.data(), n, SpecWithColors(it, n));
  } else if (it.kind == "bar" || it.kind == "barh") {
    // barh: x is where each bar is on the y axis, y its length
    double w = it.bar_width;
    if (w <= 0) w = n > 1 ? (x[static_cast<size_t>(n - 1)] - x[0]) / (n - 1) * 0.8 : 0.8;
    ImPlotSpec bs = SpecWithColors(it, n);
    if (it.kind == "barh") {
      bs.Flags |= ImPlotBarsFlags_Horizontal;
      ImPlot::PlotBars(label.c_str(), y.data(), x.data(), n, w, bs);
    } else {
      ImPlot::PlotBars(label.c_str(), x.data(), y.data(), n, w, bs);
    }
  } else if (it.kind == "errorbar") {
    const auto& neg = ColOf(it, "neg");
    const auto& pos = ColOf(it, "pos");
    const int m = std::min({n, static_cast<int>(neg.size()), static_cast<int>(pos.size())});
    ImPlotSpec es = sp;
    if (it.horizontal) es.Flags |= ImPlotErrorBarsFlags_Horizontal;
    ImPlot::PlotErrorBars(label.c_str(), x.data(), y.data(), neg.data(), pos.data(), m, es);
  } else if (it.kind == "rects") {
    DrawRects(it, label.c_str());
  } else if (it.kind == "polygon" || it.kind == "arrows") {
    DrawShapes(it, label.c_str());
  } else if (it.kind == "signals") {
    DrawSignals(it, label.c_str());
  } else if (it.kind == "annotations") {
    const ImVec4 bg = it.color ? Col(it.color) : ImVec4(0.25f, 0.25f, 0.25f, 0.9f);
    for (int i = 0; i < n; ++i) {
      const char* t = static_cast<size_t>(i) < it.labels.size() ? it.labels[static_cast<size_t>(i)].c_str() : "";
      ImPlot::Annotation(x[static_cast<size_t>(i)], y[static_cast<size_t>(i)], bg, ImVec2(S(8), -S(8)), true, "%s", t);
    }
  } else if (it.kind == "hist") {
    const auto& v = ColOf(it, "values");
    ImPlot::PlotHistogram(label.c_str(), v.data(), static_cast<int>(v.size()), it.bins, 1.0, ImPlotRange(), sp);
  } else if (it.kind == "fill") {
    const auto& y2 = ColOf(it, "y2");
    if (!y2.empty()) ImPlot::PlotShaded(label.c_str(), x.data(), y.data(), y2.data(), std::min(n, static_cast<int>(y2.size())), sp);
    else ImPlot::PlotShaded(label.c_str(), x.data(), y.data(), n, it.ref, sp);
  } else if (it.kind == "hline" || it.kind == "vline") {
    const auto& v = ColOf(it, "values");
    const bool hz = it.kind == "hline";
    ImPlotSpec s2 = sp;
    if (hz) s2.Flags |= ImPlotInfLinesFlags_Horizontal;
    if (it.dash == 0) {
      ImPlot::PlotInfLines(label.c_str(), v.data(), static_cast<int>(v.size()), s2);
    } else if (ImPlot::PlotDummy(label.c_str(), s2), ImPlot::BeginItem(label.c_str())) {
      const ImPlotRect lim = ImPlot::GetPlotLimits();
      const ImU32 col = it.color ? Argb(it.color) : ImGui::GetColorU32(ImPlot::GetLastItemColor());
      for (double val : v) {
        const ImVec2 pts[2] = {hz ? ImPlot::PlotToPixels(lim.X.Min, val) : ImPlot::PlotToPixels(val, lim.Y.Min),
                               hz ? ImPlot::PlotToPixels(lim.X.Max, val) : ImPlot::PlotToPixels(val, lim.Y.Max)};
        DashedPolyline(ImPlot::GetPlotDrawList(), pts, 2, col, it.width, it.dash);
      }
      ImPlot::EndItem();
    }
    if (it.tag) {  // each value on its axis: "STOP 23,074"
      const ImVec4 c = it.color ? Col(it.color) : ImPlot::GetLastItemColor();
      const char* fmt = it.text.empty() ? "%g" : it.text.c_str();
      for (double val : v) {
        char num[64];
        std::snprintf(num, sizeof(num), fmt, val);
        const std::string t = it.tag_text.empty() ? std::string(num) : it.tag_text + " " + num;
        if (hz) QueueTagY(val, c, t);
        else ImPlot::TagX(val, c, "%s", t.c_str());
      }
    }
  } else if (it.kind == "hband" || it.kind == "vband") {
    DrawBands(it, label.c_str());
  } else if (it.kind == "candles") {
    DrawCandles(it, label.c_str(), px_width);
  } else if (it.kind == "heatmap") {
    const auto& z = ColOf(it, "z");
    if (it.rows > 0 && it.cols_n > 0 && z.size() >= static_cast<size_t>(it.rows) * static_cast<size_t>(it.cols_n)) {
      double lo = it.vmin, hi = it.vmax;
      if (it.vauto) {
        lo = std::numeric_limits<double>::infinity();
        hi = -lo;
        for (double v : z) {
          if (std::isnan(v)) continue;
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
        if (!(hi > lo)) hi = lo + 1;
      }
      ImPlot::PushColormap(static_cast<ImPlotColormap>(it.cmap));
      const size_t ncell = static_cast<size_t>(it.rows) * static_cast<size_t>(it.cols_n);
      const bool labels = ncell <= 400;
      const char* fmt = it.text.empty() ? "%.2f" : it.text.c_str();
      const bool has_nan = std::any_of(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(ncell), [](double v) { return !std::isfinite(v); });
      // cell c, r covers [x0 + c * cw, x0 + (c + 1) * cw] x [y0 + (rows - r - 1) * ch, ...] (row 0 at the top)
      const double x0 = it.has_extent ? it.extent[0] : 0.0, y0b = it.has_extent ? it.extent[2] : 0.0;
      const double cw = it.has_extent ? (it.extent[1] - it.extent[0]) / it.cols_n : 1.0;
      const double ch = it.has_extent ? (it.extent[3] - it.extent[2]) / it.rows : 1.0;
      const ImPlotPoint bmin(x0, y0b), bmax(x0 + cw * it.cols_n, y0b + ch * it.rows);
      if (!has_nan) {
        ImPlot::PlotHeatmap(label.c_str(), z.data(), it.rows, it.cols_n, lo, hi, labels ? fmt : nullptr, bmin, bmax, sp);
      } else {
        // missing values (NaN): ImPlot can't color them; they are drawn as gray cells without a label
        std::vector<double> zz(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(ncell));
        for (double& v : zz) {
          if (!std::isfinite(v)) v = lo;
        }
        ImPlot::PlotHeatmap(label.c_str(), zz.data(), it.rows, it.cols_n, lo, hi, nullptr, bmin, bmax, sp);
        ImDrawList* dl = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        for (int r = 0; r < it.rows; ++r) {
          for (int c = 0; c < it.cols_n; ++c) {
            const double v = z[static_cast<size_t>(r) * static_cast<size_t>(it.cols_n) + static_cast<size_t>(c)];
            const double cy = y0b + ch * (it.rows - r - 1), cx = x0 + cw * c;
            if (!std::isfinite(v)) {
              dl->AddRectFilled(ImPlot::PlotToPixels(cx, cy + ch), ImPlot::PlotToPixels(cx + cw, cy), IM_COL32(205, 205, 205, 255));
            } else if (labels) {
              char buf[64];
              std::snprintf(buf, sizeof(buf), fmt, v);
              const ImVec2 ctr = ImPlot::PlotToPixels(cx + cw * 0.5, cy + ch * 0.5), ts = ImGui::CalcTextSize(buf);
              const ImVec4 col = ImPlot::SampleColormap(static_cast<float>(std::clamp((v - lo) / (hi - lo), 0.0, 1.0)));
              const float lum = 0.299f * col.x + 0.587f * col.y + 0.114f * col.z;
              dl->AddText(ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f), lum > 0.5f ? IM_COL32_BLACK : IM_COL32_WHITE, buf);
            }
          }
        }
        ImPlot::PopPlotClipRect();
      }
      ImPlot::PopColormap();
    }
  } else if (it.kind == "text") {
    ImPlot::PlotText(it.text.c_str(), it.ref, it.bar_width);
  }
}

// the x value of the table row of the active cell (figure linked to a table)
// Sheet date serials are local wall-clock times; time axes hold Unix seconds shown in local time.
double SerialToEpoch(double serial) {
  const double wall = std::round((serial - 25569.0) * 86400000.0) / 1000.0;  // to the millisecond
  const double whole = std::floor(wall);
  const std::time_t t = static_cast<std::time_t>(whole);
  std::tm tm{};
  if (gmtime_s(&tm, &t) != 0) return wall;
  tm.tm_isdst = -1;
  const std::time_t e = std::mktime(&tm);
  return e == -1 ? wall : static_cast<double>(e) + (wall - whole);
}

double EpochToSerial(double epoch) {
  const double whole = std::floor(epoch);
  const std::time_t t = static_cast<std::time_t>(whole);
  std::tm tm{};
  if (localtime_s(&tm, &t) != 0) return epoch / 86400.0 + 25569.0;
  const double wall = static_cast<double>(_mkgmtime(&tm)) + (epoch - whole);
  return wall / 86400.0 + 25569.0;
}

// synced crosshairs: the x under the mouse in one plot shows in the other plots of the same group (figures with the
// same sync_cursor, or the plots of one figure with link_x); Python can set it too (plot.cursor)
struct SyncCursor {
  std::string group;
  double x = kNaN;
  bool time = false;  // the x is Unix seconds (a time axis)
  bool any = false;   // set from Python: shown on every axis of the group
  int64_t fig = -1;
  int axis = -1;
};
SyncCursor g_sync_prev, g_sync_cur;
std::unordered_map<std::string, SyncCursor> g_sync_set;

std::string SyncGroup(const Figure& f) {
  if (!f.sync.empty()) return f.sync;
  return f.link_x && f.rows * f.cols > 1 ? "#fig" + std::to_string(f.id) : std::string();
}

// inside BeginPlot / EndPlot
// The tag of a marked x on a time axis: at local midnight (daily bars) the date alone; "7/2 00:00" read as a time
// nobody traded at. Empty: the axis's own text (other times, other axes).
std::string DayTagText(double x, bool time_axis) {
  if (!time_axis || !std::isfinite(x) || x != std::floor(x)) return {};
  const std::time_t t = static_cast<std::time_t>(x);
  std::tm tm{};
  if (localtime_s(&tm, &t) != 0 || tm.tm_hour || tm.tm_min || tm.tm_sec) return {};
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

// draws the tag; returns its text when YCGrid chose it, else "" (ImPlot's axis text)
std::string TagTimeX(double x, bool time_axis, ImVec4 col) {
  std::string day = DayTagText(x, time_axis);
  if (!day.empty()) ImPlot::TagX(x, col, "%s", day.c_str());
  else ImPlot::TagX(x, col, true);
  return day;
}

void DrawSyncedCursor(const Figure& f, PlotAxes& ax, int a) {
  ax.synced_x = kNaN;
  const std::string grp = SyncGroup(f);
  if (grp.empty()) return;
  if (ImPlot::IsPlotHovered()) {
    g_sync_cur = SyncCursor{grp, ImPlot::GetPlotMousePos().x, ax.time_x, false, f.id, a};
    g_sync_set.erase(grp);  // the mouse takes over from a cursor set by Python
    return;
  }
  const SyncCursor* sc = nullptr;
  if (g_sync_prev.group == grp && !(g_sync_prev.fig == f.id && g_sync_prev.axis == a)) {
    sc = &g_sync_prev;
  } else if (auto it = g_sync_set.find(grp); it != g_sync_set.end()) {
    sc = &it->second;
  }
  if (!sc || !std::isfinite(sc->x) || (!sc->any && sc->time != ax.time_x)) return;
  ImPlotSpec cs;
  cs.LineColor = ImVec4(0.55f, 0.55f, 0.6f, 0.9f);
  cs.LineWeight = 1.0f;
  cs.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
  ImPlot::PlotInfLines("##synced", &sc->x, 1, cs);
  TagTimeX(sc->x, ax.time_x, ImVec4(0.35f, 0.35f, 0.4f, 1));
  ax.synced_x = sc->x;
}

double LinkedX(AppState& s, const Figure& f, uint32_t* sheet_out, const model::Table** table_out, int* col_out) {
  if (f.link_table.empty() || !s.snap) return kNaN;
  uint32_t sid = 0;
  const model::Table* t = model::FindTable(s.snap->wb, f.link_table, &sid);
  if (!t) return kNaN;
  const int c = t->ColumnIndex(f.link_col);
  if (c < 0) return kNaN;
  *sheet_out = sid;
  *table_out = t;
  *col_out = c;
  const auto& sel = s.FocusView().sel;
  const core::Rect d = t->Data();
  if (s.focus_sheet != sid || !t->HasData() || sel.act_r < d.r0 || sel.act_r > d.r1) return kNaN;
  const core::Value& v = s.snap->Sheet(sid)->GetValue(sel.act_r, d.c0 + static_cast<uint32_t>(c));
  return v.IsNumeric() ? v.AsDouble() : kNaN;
}

// a click on a plot of a linked figure: select the table row whose x is nearest
void SelectNearestRow(AppState& s, uint32_t sid, const model::Table& t, int col, double x) {
  const model::Sheet* sh = s.snap->Sheet(sid);
  const core::Rect d = t.Data();
  if (!sh || !t.HasData()) return;
  uint32_t best = d.r0;
  double bd = std::numeric_limits<double>::infinity();
  for (uint32_t r = d.r0; r <= d.r1; ++r) {
    const core::Value& v = sh->GetValue(r, d.c0 + static_cast<uint32_t>(col));
    if (!v.IsNumeric()) continue;
    const double dd = std::fabs(v.AsDouble() - x);
    if (dd < bd) {
      bd = dd;
      best = r;
    }
  }
  if (s.focus_sheet != sid) ActivateSheet(s, sid);
  render::SelectCell(s.FocusView(), best, d.c0 + static_cast<uint32_t>(col), false);
  render::EnsureVisible(*sh, s.FocusView(), best, d.c0 + static_cast<uint32_t>(col));
}

void ApplyStyle(PlotItem& it, const MV& p) {
  if (p.Has("label")) it.label = p.GetStr("label");
  if (p.Has("color")) it.color = static_cast<uint32_t>(p.GetInt("color"));
  if (p.Has("width")) it.width = static_cast<float>(p.GetDouble("width", 1.5));
  if (p.Has("size")) it.size = static_cast<float>(p.GetDouble("size", 3));
  if (p.Has("marker")) it.marker = static_cast<int>(p.GetInt("marker"));
  if (p.Has("bins")) it.bins = static_cast<int>(p.GetInt("bins"));
  if (p.Has("bar_width")) it.bar_width = p.GetDouble("bar_width");
  if (p.Has("ref")) it.ref = p.GetDouble("ref");
  if (p.Has("text")) it.text = p.GetStr("text");
  if (const MV* cm = p.Get("cmap")) it.cmap = cm->t == MV::T::Str ? PlotColormapByName(cm->s) : static_cast<int>(cm->Num());
  if (p.Has("capacity")) it.capacity = static_cast<size_t>(std::max<int64_t>(0, p.GetInt("capacity")));
  if (p.Has("downsample")) it.downsample = p.GetStr("downsample", "minmax");
  if (p.Has("rows")) it.rows = static_cast<int>(p.GetInt("rows"));
  if (p.Has("cols")) it.cols_n = static_cast<int>(p.GetInt("cols"));
  if (const MV* e = p.Get("extent")) {
    it.has_extent = e->a.size() == 4 && e->a[0].Num() != e->a[1].Num() && e->a[2].Num() != e->a[3].Num();
    for (size_t k = 0; k < 4 && k < e->a.size(); ++k) it.extent[k] = e->a[k].Num();
  }
  if (const MV* v = p.Get("vmin"); v && !v->IsNil()) it.vmin = v->Num(), it.vauto = false;
  if (const MV* v = p.Get("vmax"); v && !v->IsNil()) it.vmax = v->Num(), it.vauto = false;
  if (p.Has("edge")) it.edge = static_cast<uint32_t>(p.GetInt("edge"));
  if (p.Has("tag")) it.tag = p.GetBool("tag");
  if (p.Has("tag_text")) it.tag_text = p.GetStr("tag_text");
  if (p.Has("style")) {
    const std::string st = p.GetStr("style");
    it.dash = st == "dash" || st == "dashed" ? 1 : st == "dot" || st == "dotted" ? 2 : 0;
  }
  if (p.Has("fit")) it.fit_band = p.GetBool("fit");
  if (p.Has("horizontal")) it.horizontal = p.GetBool("horizontal");
}

// the "c" column (ARGB numbers) as draw colors
void TakeColors(PlotItem& it) {
  auto c = it.cols.find("c");
  it.colors.clear();
  if (c == it.cols.end()) return;
  it.colors.reserve(c->second.size());
  for (double v : c->second) it.colors.push_back(Argb(static_cast<uint32_t>(static_cast<int64_t>(v))));
}


// ---- 3-D surfaces (volatility surfaces, parameter sweeps): a projected mesh drawn in the plot area ----

bool HasSurface(const PlotAxes& ax) {
  return std::any_of(ax.items.begin(), ax.items.end(), [](const PlotItem& i) { return i.kind == "surface"; });
}

std::string Tick(double v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.4g", v);
  return b;
}

void DrawSurface3D(PlotAxes& ax) {
  ImDrawList* dl = ImPlot::GetPlotDrawList();
  const ImVec2 pos = ImPlot::GetPlotPos(), size = ImPlot::GetPlotSize();
  const float bar_w = S(14), bar_gap = S(52);
  const ImVec2 c(pos.x + (size.x - bar_gap) * 0.5f, pos.y + size.y * 0.52f);
  const float scale = std::min(size.x - bar_gap, size.y) * 0.34f * ax.zoom;
  // the data ranges of every surface
  double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY, z0 = INFINITY, z1 = -INFINITY;
  for (const auto& it : ax.items) {
    if (it.kind != "surface") continue;
    for (double v : ColOf(it, "x")) {
      if (std::isfinite(v)) x0 = std::min(x0, v), x1 = std::max(x1, v);
    }
    for (double v : ColOf(it, "y")) {
      if (std::isfinite(v)) y0 = std::min(y0, v), y1 = std::max(y1, v);
    }
    for (double v : ColOf(it, "z")) {
      if (std::isfinite(v)) z0 = std::min(z0, v), z1 = std::max(z1, v);
    }
  }
  if (!(x1 >= x0) || !(y1 >= y0) || !(z1 >= z0)) return;
  auto span = [](double a, double b) { return b > a ? b - a : 1.0; };
  const float cy = std::cos(ax.yaw), sy = std::sin(ax.yaw), cp = std::cos(ax.pitch), sp = std::sin(ax.pitch);
  // world: u, v across, w up, each in [-1, 1] (w scaled to 0.75)
  auto project = [&](double x, double y, double z, float* depth) {
    const float u = static_cast<float>(2 * (x - x0) / span(x0, x1) - 1), v = static_cast<float>(2 * (y - y0) / span(y0, y1) - 1);
    const float w = static_cast<float>((2 * (z - z0) / span(z0, z1) - 1) * 0.75);
    const float X = u * cy - v * sy, Y = u * sy + v * cy;
    if (depth) *depth = Y * cp - w * sp;
    return ImVec2(c.x + X * scale, c.y - (w * cp + Y * sp) * scale);
  };
  // the box: floor edges and the three axes with their ranges
  const ImU32 grid = IM_COL32(150, 150, 150, 140), text = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 f00 = project(x0, y0, z0, nullptr), f10 = project(x1, y0, z0, nullptr), f11 = project(x1, y1, z0, nullptr), f01 = project(x0, y1, z0, nullptr);
  dl->AddQuad(f00, f10, f11, f01, grid);
  // the vertical axis at the floor corner farthest back
  struct Corner {
    double x, y;
  } corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  int back = 0;
  float best = -1e9f;
  for (int i = 0; i < 4; ++i) {
    float d = 0;
    project(corners[i].x, corners[i].y, z0, &d);
    if (d > best) best = d, back = i;
  }
  const ImVec2 zb = project(corners[back].x, corners[back].y, z0, nullptr), zt = project(corners[back].x, corners[back].y, z1, nullptr);
  dl->AddLine(zb, zt, grid);
  for (int k = 1; k <= 4; ++k) {
    const double z = z0 + (z1 - z0) * k / 4;
    const ImVec2 a = project(corners[back].x, corners[back].y, z, nullptr);
    const ImVec2 b = project(corners[(back + 1) % 4].x, corners[(back + 1) % 4].y, z, nullptr);
    const ImVec2 b2 = project(corners[(back + 3) % 4].x, corners[(back + 3) % 4].y, z, nullptr);
    dl->AddLine(a, b, IM_COL32(150, 150, 150, 60));
    dl->AddLine(a, b2, IM_COL32(150, 150, 150, 60));
  }
  // the meshes, far quads first
  struct Quad {
    ImVec2 p[4];
    float depth;
    ImU32 fill, line;
  };
  std::vector<Quad> quads;
  struct Vertex {
    ImVec2 p;
    double x, y, z;
  };
  std::vector<Vertex> verts;
  for (const auto& it : ax.items) {
    if (it.kind != "surface") continue;
    const auto& xs = ColOf(it, "x");
    const auto& ys = ColOf(it, "y");
    const auto& zs = ColOf(it, "z");
    const size_t nx = it.cols_n > 0 ? std::min<size_t>(static_cast<size_t>(it.cols_n), xs.size()) : xs.size();
    const size_t ny = it.rows > 0 ? std::min<size_t>(static_cast<size_t>(it.rows), ys.size()) : ys.size();
    if (nx < 2 || ny < 2 || zs.size() < nx * ny) continue;
    const double lo = it.vauto ? z0 : it.vmin, hi = it.vauto ? z1 : it.vmax;
    const bool wire = it.text == "wireframe";
    for (size_t j = 0; j < ny; ++j) {
      for (size_t i = 0; i < nx; ++i) verts.push_back({project(xs[i], ys[j], zs[j * nx + i], nullptr), xs[i], ys[j], zs[j * nx + i]});
    }
    for (size_t j = 0; j + 1 < ny; ++j) {
      for (size_t i = 0; i + 1 < nx; ++i) {
        const double zz[4] = {zs[j * nx + i], zs[j * nx + i + 1], zs[(j + 1) * nx + i + 1], zs[(j + 1) * nx + i]};
        if (!std::isfinite(zz[0]) || !std::isfinite(zz[1]) || !std::isfinite(zz[2]) || !std::isfinite(zz[3])) continue;
        Quad q;
        float d[4];
        q.p[0] = project(xs[i], ys[j], zz[0], &d[0]);
        q.p[1] = project(xs[i + 1], ys[j], zz[1], &d[1]);
        q.p[2] = project(xs[i + 1], ys[j + 1], zz[2], &d[2]);
        q.p[3] = project(xs[i], ys[j + 1], zz[3], &d[3]);
        q.depth = (d[0] + d[1] + d[2] + d[3]) * 0.25f;
        const double t = std::clamp(((zz[0] + zz[1] + zz[2] + zz[3]) * 0.25 - lo) / span(lo, hi), 0.0, 1.0);
        const ImVec4 col = ImPlot::SampleColormap(static_cast<float>(t), it.cmap);
        q.fill = wire ? 0 : ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, 0.95f));
        q.line = wire ? ImGui::ColorConvertFloat4ToU32(col) : IM_COL32(40, 40, 40, 70);
        quads.push_back(q);
      }
    }
  }
  std::sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.depth > b.depth; });
  for (const auto& q : quads) {
    if (q.fill) dl->AddQuadFilled(q.p[0], q.p[1], q.p[2], q.p[3], q.fill);
    if (quads.size() < 20000) dl->AddQuad(q.p[0], q.p[1], q.p[2], q.p[3], q.line, 1.0f);
  }
  // the axis labels over the mesh
  auto label = [&](ImVec2 p, const std::string& t, float dx = 0, float dy = 0) {
    const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    dl->AddText(ImVec2(p.x - ts.x * 0.5f + dx, p.y - ts.y * 0.5f + dy), text, t.c_str());
  };
  const float off = S(12);
  label(f00, Tick(x0), 0, off);
  label(f10, Tick(x1), 0, off);
  label(ImVec2((f00.x + f10.x) * 0.5f, (f00.y + f10.y) * 0.5f), ax.xlabel.empty() ? "x" : ax.xlabel, 0, off * 2);
  label(f11, Tick(y1), 0, off);
  label(f10, Tick(y0), S(30), 0);
  label(ImVec2((f10.x + f11.x) * 0.5f, (f10.y + f11.y) * 0.5f), ax.ylabel.empty() ? "y" : ax.ylabel, 0, off * 2);
  label(zb, Tick(z0), -S(28));
  label(zt, Tick(z1), -S(28));
  label(ImVec2((zb.x + zt.x) * 0.5f, (zb.y + zt.y) * 0.5f), ax.zlabel.empty() ? "z" : ax.zlabel, -S(34));
  // the color bar
  int cmap = 0;
  for (const auto& it : ax.items) {
    if (it.kind == "surface") cmap = it.cmap;
  }
  const float bx = pos.x + size.x - bar_gap + S(10), by0 = pos.y + size.y * 0.15f, by1 = pos.y + size.y * 0.85f;
  const int steps = 32;
  for (int k = 0; k < steps; ++k) {
    const ImVec4 col = ImPlot::SampleColormap(1.0f - (k + 0.5f) / steps, cmap);
    dl->AddRectFilled(ImVec2(bx, by0 + (by1 - by0) * k / steps), ImVec2(bx + bar_w, by0 + (by1 - by0) * (k + 1) / steps), ImGui::ColorConvertFloat4ToU32(col));
  }
  dl->AddText(ImVec2(bx, by0 - S(18)), text, Tick(z1).c_str());
  dl->AddText(ImVec2(bx, by1 + S(2)), text, Tick(z0).c_str());
  // mouse: drag turns, wheel zooms, double-click resets, hover shows the nearest point
  if (ImPlot::IsPlotHovered()) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
      ax.yaw += io.MouseDelta.x * 0.01f;
      ax.pitch = std::clamp(ax.pitch + io.MouseDelta.y * 0.01f, -1.55f, 1.55f);
    }
    if (io.MouseWheel != 0) ax.zoom = std::clamp(ax.zoom * std::pow(1.1f, io.MouseWheel), 0.2f, 8.0f);
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ax.yaw = -0.85f, ax.pitch = 0.5f, ax.zoom = 1.0f;
    const ImVec2 m = io.MousePos;
    const Vertex* hit = nullptr;
    float nd = S(10) * S(10);
    for (const auto& v : verts) {
      const float dx = v.p.x - m.x, dy = v.p.y - m.y;
      if (dx * dx + dy * dy < nd) nd = dx * dx + dy * dy, hit = &v;
    }
    if (hit && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      dl->AddCircleFilled(hit->p, S(3.5f), IM_COL32(255, 255, 255, 255));
      dl->AddCircle(hit->p, S(3.5f), IM_COL32(0, 0, 0, 255));
      ImGui::SetTooltip("%s = %s\n%s = %s\n%s = %s", ax.xlabel.empty() ? "x" : ax.xlabel.c_str(), Tick(hit->x).c_str(),
                        ax.ylabel.empty() ? "y" : ax.ylabel.c_str(), Tick(hit->y).c_str(), ax.zlabel.empty() ? "z" : ax.zlabel.c_str(),
                        Tick(hit->z).c_str());
    }
  }
}

}  // namespace

// ---- requests (UI thread) ----

bool g_replaying_plot = false;  // FigureRequest is applying a request from a tape

io::MV ReplayFigureRequest(AppState& s, const std::string& m, const io::MV& p, const std::shared_ptr<engine::RawFrame>& data, std::string* err) {
  g_replaying_plot = true;
  io::MV r = FigureRequest(s, m, p, data, err);
  g_replaying_plot = false;
  return r;
}

// the figure's context menu (fig.context_menu): opened by a right click in a plot, drawn at the window level
void DrawFigureMenu(Figure& f) {
  const std::string id = "##yc_figmenu" + std::to_string(f.id);
  if (f.menu_request) {
    if (f.menu_input == 2) ImGui::SetNextWindowPos(f.menu_px);
    f.menu_input = 0;
    ImGui::OpenPopup(id.c_str());
    f.menu_request = false;
  }
  if (!ImGui::BeginPopup(id.c_str())) {
    f.menu_close = false;
    return;
  }
  if (f.menu_close) {
    f.menu_close = false;
    ImGui::CloseCurrentPopup();
  }
  for (size_t i = 0; i < f.menu.size(); ++i) {
    if (f.menu[i].empty()) {
      ImGui::Separator();
      continue;
    }
    if (ImGui::MenuItem((f.menu[i] + "##" + std::to_string(i)).c_str())) {
      MV ev = MV::Map();
      ev.Set("type", MV::Str("menu"));
      ev.Set("ax", MV::Int(f.menu_ax));
      ev.Set("item", MV::Int(static_cast<int64_t>(i)));
      ev.Set("text", MV::Str(f.menu[i]));
      ev.Set("x", MV::Double(f.menu_at.x));
      ev.Set("y", MV::Double(f.menu_at.y));
      Emit(f, std::move(ev));
    }
  }
  ImGui::EndPopup();
}

// plot.input: what a mouse would do, in data coordinates, through the same events (tests, agents: yc_plot_input).
// ---- [YC] items fed by topics ----
namespace {
struct LiveTap {
  std::mutex mu;
  std::unordered_set<std::string> topics;  // followed by some item
  std::vector<PlotLiveUpdate> pending;
  std::atomic<bool> wanted{false};
};
LiveTap& Live() {
  static LiveTap t;
  return t;
}
}  // namespace

bool PlotLiveWants() { return Live().wanted.load(std::memory_order_relaxed); }

void PlotLiveFeed(const std::vector<PlotLiveUpdate>& updates) {
  LiveTap& t = Live();
  bool any = false;
  {
    std::lock_guard<std::mutex> lk(t.mu);
    for (const auto& u : updates) {
      if (!t.topics.count(u.topic)) continue;
      t.pending.push_back(u);
      any = true;
    }
    constexpr size_t kMax = 4u << 20;  // the window isn't drawing (minimized): keep the newest updates
    if (t.pending.size() > kMax) t.pending.erase(t.pending.begin(), t.pending.begin() + static_cast<std::ptrdiff_t>(t.pending.size() - kMax));
  }
  if (any) ui::RequestRedrawIn(0.0);
}

void SetLive(PlotItem& it, const MV& lv) {
  it.live_topic = lv.GetStr("topic");
  it.live_vol_topic = lv.GetStr("volume_topic");
  it.live_vol_item = lv.GetStr("volume_item");
  it.live_vol_ax = static_cast<int>(lv.GetInt("volume_ax", -1));
  it.live_vol_cum = lv.GetBool("cumulative", false);
  it.live_seconds = std::max(1e-6, lv.GetDouble("seconds", 60));
  it.live_keep = static_cast<size_t>(std::max<int64_t>(0, lv.GetInt("keep", 0)));
  LiveTap& t = Live();
  std::lock_guard<std::mutex> lk(t.mu);
  if (!it.live_topic.empty()) t.topics.insert(it.live_topic);
  if (!it.live_vol_topic.empty()) t.topics.insert(it.live_vol_topic);
  t.wanted = !t.topics.empty();
}

MV LiveToMV(const PlotItem& it) {
  MV lv = MV::Map();
  lv.Set("topic", MV::Str(it.live_topic));
  lv.Set("volume_topic", MV::Str(it.live_vol_topic));
  lv.Set("volume_item", MV::Str(it.live_vol_item));
  lv.Set("volume_ax", MV::Int(it.live_vol_ax));
  lv.Set("cumulative", MV::Bool(it.live_vol_cum));
  lv.Set("seconds", MV::Double(it.live_seconds));
  lv.Set("keep", MV::Int(static_cast<int64_t>(it.live_keep)));
  return lv;
}

namespace {
void TrimFront(PlotItem& it, size_t keep) {
  if (!keep) return;
  const auto& x = ColOf(it, "x");
  if (x.size() <= keep + keep / 4) return;  // trimmed in steps, not on every update
  const auto drop = static_cast<std::ptrdiff_t>(x.size() - keep);
  for (auto& [name, col] : it.cols) {
    if (col.size() >= static_cast<size_t>(drop)) col.erase(col.begin(), col.begin() + drop);
  }
}

// the rules of yc.bars.BarBuilder: bars start at multiples of `seconds`; a late tick counts in the forming bar
void LivePrice(PlotItem& it, double t, double px) {
  auto& X = it.cols["x"];
  auto& O = it.cols["open"];
  auto& H = it.cols["high"];
  auto& L = it.cols["low"];
  auto& C = it.cols["close"];
  auto& V = it.cols["volume"];
  double start = std::floor(t / it.live_seconds) * it.live_seconds;
  if (!X.empty() && start < X.back()) start = X.back();
  if (X.empty() || start > X.back()) {
    X.push_back(start), O.push_back(px), H.push_back(px), L.push_back(px), C.push_back(px);
    V.resize(X.size() - 1, 0.0);
    V.push_back(it.live_vol_pending);
    it.live_vol_pending = 0;
  } else {
    H.back() = std::max(H.back(), px);
    L.back() = std::min(L.back(), px);
    C.back() = px;
  }
  TrimFront(it, it.live_keep);
}

void LiveVolume(PlotItem& it, double t, double v) {
  double d = v;
  if (it.live_vol_cum) {
    d = std::isnan(it.live_vol_last) ? 0.0 : std::max(0.0, v - it.live_vol_last);
    it.live_vol_last = v;
  }
  auto& X = it.cols["x"];
  auto& V = it.cols["volume"];
  V.resize(X.size(), 0.0);
  const double start = std::floor(t / it.live_seconds) * it.live_seconds;
  if (X.empty() || start > X.back()) it.live_vol_pending += d;  // the bar it belongs to hasn't started yet
  else V.back() += d;
}

void LivePoint(PlotItem& it, double t, double v) {
  it.cols["x"].push_back(t);
  it.cols["y"].push_back(v);
  TrimFront(it, it.live_keep);
}

// the volume bars of a live candle item, in the candle colors
void SyncLiveVolume(Figure& f, PlotItem& it) {
  if (it.live_vol_ax < 0 || static_cast<size_t>(it.live_vol_ax) >= f.axes.size() || it.live_vol_item.empty()) return;
  PlotAxes& vax = f.axes[static_cast<size_t>(it.live_vol_ax)];
  auto vit = std::find_if(vax.items.begin(), vax.items.end(), [&](const PlotItem& x) { return x.id == it.live_vol_item; });
  if (vit == vax.items.end()) {
    vax.items.push_back(PlotItem{});
    vit = vax.items.end() - 1;
    vit->id = it.live_vol_item;
    vit->kind = "bar";
    vit->label = "Volume";
  }
  const auto& X = ColOf(it, "x");
  const auto& O = ColOf(it, "open");
  const auto& C = ColOf(it, "close");
  std::vector<double> V = ColOf(it, "volume");
  V.resize(X.size(), 0.0);
  vit->cols["x"] = X;
  vit->cols["y"] = std::move(V);
  vit->bar_width = it.live_seconds * 0.8;
  vit->colors.resize(X.size());
  const ImU32 up = CandleUpColor(f.theme), dn = CandleDownColor(f.theme);
  for (size_t i = 0; i < X.size() && i < O.size() && i < C.size(); ++i) vit->colors[i] = C[i] >= O[i] ? up : dn;
  vit->dirty = true;
}
}  // namespace

void ApplyLiveUpdates(FigureStore& g) {
  LiveTap& t = Live();
  if (!t.wanted.load(std::memory_order_relaxed)) return;
  std::vector<PlotLiveUpdate> ups;
  {
    std::lock_guard<std::mutex> lk(t.mu);
    if (t.pending.empty()) return;
    ups.swap(t.pending);
  }
  struct Target {
    Figure* f;
    PlotItem* it;
  };
  std::unordered_map<std::string, std::vector<Target>> price, volume;
  for (auto& [id, f] : g.figs) {
    for (auto& ax : f.axes) {
      for (auto& it : ax.items) {
        if (it.live_topic.empty()) continue;
        price[it.live_topic].push_back({&f, &it});
        if (!it.live_vol_topic.empty()) volume[it.live_vol_topic].push_back({&f, &it});
      }
    }
  }
  std::vector<Figure*> empty_before;  // a live item's first data: fit the figure to it
  for (auto& [topic, tgs] : price) {
    for (auto& tg : tgs) {
      if (ColOf(*tg.it, "x").empty()) empty_before.push_back(tg.f);
    }
  }
  for (const auto& u : ups) {
    const double ts = static_cast<double>(u.time_ns) / 1e9;
    if (auto v = volume.find(u.topic); v != volume.end()) {
      for (auto& tg : v->second) LiveVolume(*tg.it, ts, u.value), tg.it->live_changed = true;
    }
    if (auto pr = price.find(u.topic); pr != price.end()) {
      for (auto& tg : pr->second) {
        if (tg.it->kind == "candles") LivePrice(*tg.it, ts, u.value);
        else LivePoint(*tg.it, ts, u.value);
        tg.it->live_changed = true;
      }
    }
  }
  for (auto& [topic, tgs] : price) {
    for (auto& tg : tgs) {
      if (!tg.it->live_changed) continue;
      tg.it->live_changed = false;
      tg.it->dirty = true;
      if (tg.it->kind == "candles") SyncLiveVolume(*tg.f, *tg.it);
    }
  }
  for (Figure* f : empty_before) f->fit = true;
}

// actions: click (button, ctrl, shift, alt), double_click, select (x0, x1, y0, y1), drag_handle (handle id or label,
// x / y), drag_band_edge (handle, edge "top" / "bottom", y), close_handle (handle: its "x" button), context_menu (text)
io::MV FigureInput(Figure& f, const io::MV& p, std::string* err) {
  const std::string action = p.GetStr("action");
  const int a = static_cast<int>(p.GetInt("ax", 0));
  if (a < 0 || static_cast<size_t>(a) >= f.axes.size()) return *err = "The figure has no plot " + std::to_string(a), MV();
  PlotAxes& ax = f.axes[static_cast<size_t>(a)];
  auto base = [&](const char* type) {
    MV ev = MV::Map();
    ev.Set("type", MV::Str(type));
    ev.Set("ax", MV::Int(a));
    ev.Set("ctrl", MV::Bool(p.GetBool("ctrl", false)));
    ev.Set("shift", MV::Bool(p.GetBool("shift", false)));
    ev.Set("alt", MV::Bool(p.GetBool("alt", false)));
    return ev;
  };
  const double x = p.GetDouble("x", 0), y = p.GetDouble("y", 0);
  auto with_point = [&](MV& ev) {
    ev.Set("x", MV::Double(x));
    ev.Set("y", MV::Double(y));
    for (const auto& it : ax.items) {
      if (it.kind != "line" && it.kind != "scatter" && it.kind != "bar" && it.kind != "step" && it.kind != "candles") continue;
      const auto& xs = ColOf(it, "x");
      if (xs.empty()) continue;
      size_t k = 0;
      if (it.sorted) {
        k = static_cast<size_t>(std::lower_bound(xs.begin(), xs.end(), x) - xs.begin());
        if (k >= xs.size() || (k > 0 && std::abs(xs[k - 1] - x) < std::abs(xs[k] - x))) k = k ? k - 1 : 0;
      }
      ev.Set("item", MV::Str(it.id));
      ev.Set("index", MV::Int(static_cast<int64_t>(k)));
      break;
    }
  };
  if (action == "wheel" || action == "live") {  // a following plot: look around (as the wheel would) / back live (the badge)
    const bool pause = action == "wheel";
    ax.user_view = pause;  // any plot: the wheel keeps the user's view
    if (pause != f.follow_paused) {
      f.follow_paused = pause;
      if (!pause) f.fit = true;
      MV ev = base("follow");
      ev.Set("paused", MV::Bool(pause));
      Emit(f, std::move(ev));
    }
    return MV::Bool(true);
  }
  if (action == "click" || action == "double_click") {
    const std::string button = p.GetStr("button", "left");
    if (action == "click" && button == "right") {
      if (!f.menu.empty()) {  // the context menu opens at that point, as for a real right click
        f.menu_request = true;
        f.menu_ax = a;
        f.menu_at = ImPlotPoint(x, y);
        f.menu_input = 1;
      }
      if (f.right && (f.want & kEvClick)) {
        MV ev = base("click");
        ev.Set("button", MV::Str("right"));
        with_point(ev);
        Emit(f, std::move(ev));
      }
      return MV::Bool(true);
    }
    MV ev = base(action == "click" ? "click" : "dblclick");
    if (action == "click") ev.Set("button", MV::Str(button));
    with_point(ev);
    if ((action == "click" && (f.want & kEvClick)) || (action == "double_click" && (f.want & kEvDouble))) Emit(f, std::move(ev));
    return MV::Bool(true);
  }
  if (action == "select") {
    MV ev = base("select");
    ev.Set("x0", MV::Double(std::min(p.GetDouble("x0", 0), p.GetDouble("x1", 0))));
    ev.Set("x1", MV::Double(std::max(p.GetDouble("x0", 0), p.GetDouble("x1", 0))));
    ev.Set("y0", MV::Double(std::min(p.GetDouble("y0", 0), p.GetDouble("y1", 0))));
    ev.Set("y1", MV::Double(std::max(p.GetDouble("y0", 0), p.GetDouble("y1", 0))));
    if (f.want & kEvSelect) Emit(f, std::move(ev));
    return MV::Bool(true);
  }
  if (action == "close_handle") {
    const std::string hid = p.GetStr("handle");
    auto h = std::find_if(ax.handles.begin(), ax.handles.end(), [&](const PlotAxes::Handle& x2) { return x2.id == hid || x2.label == hid; });
    if (h == ax.handles.end()) return *err = "No handle " + hid, MV();
    if (!h->closable) return *err = hid + " has no close button", MV();
    MV ev = base("close");
    ev.Set("handle", MV::Str(h->id));
    ev.Set("x", MV::Double(h->x));
    ev.Set("y", MV::Double(h->y));
    Emit(f, std::move(ev));
    return MV::Bool(true);
  }
  if (action == "drag_handle" || action == "drag_band_edge") {
    const std::string hid = p.GetStr("handle");
    auto h = std::find_if(ax.handles.begin(), ax.handles.end(), [&](const PlotAxes::Handle& x2) { return x2.id == hid || x2.label == hid; });
    if (h == ax.handles.end()) return *err = "No handle " + hid, MV();
    if (action == "drag_band_edge") {
      if (h->kind != "band") return *err = hid + " isn't a band", MV();
      (p.GetStr("edge", "top") == "top" ? h->y1 : h->y) = y;
      if (h->y > h->y1) std::swap(h->y, h->y1);
    } else if (h->kind == "band") {
      const double half = (h->y1 - h->y) / 2;
      h->y = y - half, h->y1 = y + half;
    } else {
      if (p.Has("x")) h->x = x;
      if (p.Has("y")) h->y = y;
    }
    for (const char* type : {"drag", "release"}) {
      MV ev = base(type);
      ev.Set("handle", MV::Str(h->id));
      ev.Set("x", MV::Double(h->x));
      ev.Set("y", MV::Double(h->y));
      if (h->kind == "band") ev.Set("y0", MV::Double(h->y)), ev.Set("y1", MV::Double(h->y1));
      Emit(f, std::move(ev));
    }
    return MV::Bool(true);
  }
  if (action == "context_menu") {
    const std::string text = p.GetStr("text");
    for (size_t i = 0; i < f.menu.size(); ++i) {
      if (f.menu[i].empty() || f.menu[i] != text) continue;
      MV ev = base("menu");
      ev.Set("item", MV::Int(static_cast<int64_t>(i)));
      ev.Set("text", MV::Str(text));
      ev.Set("x", MV::Double(x));
      ev.Set("y", MV::Double(y));
      Emit(f, std::move(ev));
      f.menu_close = true;
      return MV::Bool(true);
    }
    return *err = "The figure's context menu has no item " + text, MV();
  }
  return *err = "Unknown plot input: " + action, MV();
}

io::MV FigureRequest(AppState& s, const std::string& m, const io::MV& p, const std::shared_ptr<engine::RawFrame>& data, std::string* err) {
  FigureStore& g = G();
  ui::RequestRedrawIn(0.0);
  const bool changes = m == "plot.figure" || m == "plot.axes" || m == "plot.item" || m == "plot.close" || m == "plot.handle" || m == "plot.reset";
  if (!g_replaying_plot && changes && s.python) {
    // a replay owns this figure: its live updates wait until the replay ends (like the tables on the tape)
    if (s.python->PlotOnTape(p.GetInt("fig", 0))) return m == "plot.figure" ? MV::Int(p.GetInt("fig", 0)) : MV();
  }
  // the time machine records what changes the figures: before the request runs (its data are moved into the figure),
  // except a new figure, recorded with the id it gets
  const bool record = changes && !g_replaying_plot && s.python;
  if (record && m != "plot.figure") s.python->TapePlot(m, p, data);
  struct FigureRecorder {
    AppState& s;
    const io::MV& p;
    bool on = false;
    io::MV fig_id;
    ~FigureRecorder() {
      if (!on || fig_id.t != io::MV::T::Int) return;
      io::MV q = p;
      q.Set("fig", fig_id);
      s.python->TapePlot("plot.figure", q, nullptr);
    }
  } rec{s, p, record && m == "plot.figure"};
  if (m == "plot.figures") {  // every figure of the session
    MV a = MV::Array();
    for (const auto& [fid, x] : g.figs) {
      MV fm = MV::Map();
      fm.Set("id", MV::Int(fid));
      fm.Set("key", MV::Str(x.key));
      fm.Set("title", MV::Str(x.title));
      fm.Set("rows", MV::Int(x.rows));
      fm.Set("cols", MV::Int(x.cols));
      fm.Set("owner", MV::Str(x.owner_name));
      fm.Set("alive", MV::Bool(x.alive));
      fm.Set("follow_paused", MV::Bool(x.follow_paused));
      if (ImGuiWindow* w = ImGui::FindWindowByName(FigureTitle(x).c_str())) fm.Set("docked", MV::Bool(w->DockIsActive));
      fm.Set("persist", MV::Bool(x.persist));
      // what each plot shows, for tests: its title and items (a line added twice by two threads shows as two)
      MV axs = MV::Array();
      for (const PlotAxes& ax : x.axes) {
        MV am = MV::Map();
        am.Set("title", MV::Str(ax.title));
        MV its = MV::Array();
        for (const PlotItem& it : ax.items) {
          MV im = MV::Map();
          im.Set("kind", MV::Str(it.kind));
          im.Set("label", MV::Str(it.label));
          its.Push(std::move(im));
        }
        am.Set("items", std::move(its));
        if (!ax.cursor_tag.empty()) am.Set("cursor_tag", MV::Str(ax.cursor_tag));  // the linked row's day
        MV view = MV::Array(), rect = MV::Array(), hs = MV::Array();  // what it shows and where: a test points at a price
        for (double v : ax.view) view.Push(MV::Double(v));
        for (float v : ax.plot_rect) rect.Push(MV::Double(v));
        am.Set("view", std::move(view));
        am.Set("rect", std::move(rect));
        for (const auto& h : ax.handles) {  // drag lines (an order), points, bands: their place and label
          MV hm = MV::Map();
          hm.Set("kind", MV::Str(h.kind));
          hm.Set("label", MV::Str(h.label));
          hm.Set("x", MV::Double(h.x));
          hm.Set("y", MV::Double(h.y));
          hs.Push(std::move(hm));
        }
        am.Set("handles", std::move(hs));
        axs.Push(std::move(am));
      }
      fm.Set("axes", std::move(axs));
      a.Push(std::move(fm));
    }
    return a;
  }
  if (m == "plot.close" && p.Has("key")) {  // by key, from any connection
    const std::string key = p.GetStr("key");
    for (auto it2 = g.figs.begin(); it2 != g.figs.end(); ++it2) {
      if (!key.empty() && it2->second.key == key) {
        g.figs.erase(it2);
        return MV::Bool(true);
      }
    }
    return MV::Bool(false);
  }
  if (m == "plot.figure") {
    int64_t id = p.GetInt("fig", 0);
    const std::string key = p.GetStr("key");
    bool taken_over = false;
    if (id == 0 && !key.empty()) {
      // a key names one window: a new figure with it takes the existing window over (its place and size stay);
      // reuse keeps its title and layout unless given, replace sets them like a new figure
      for (auto& [fid, x] : g.figs) {
        if (x.key != key) continue;
        id = fid;
        for (auto& ax : x.axes) ax.items.clear(), ax.handles.clear();
        x.events.clear();
        x.want = 0;
        x.push = false;
        x.link_table.clear();
        x.link_col.clear();
        x.fit = true;
        taken_over = true;
        break;
      }
    }
    if (id == 0 || !g.figs.count(id)) {
      id = id ? id : g.next++;
      g.next = std::max(g.next, id + 1);
      g.figs[id].id = id;
    }
    Figure& f = g.figs[id];
    if (!key.empty()) f.key = key;
    if (g_request_owner || !taken_over) {  // the connection that made (or took over) the figure owns it
      f.owner = g_request_owner;
      f.owner_name = g_request_owner_name;
      f.alive = true;
      f.persist = p.GetBool("persist", !g_request_owner_host);  // host figures close with the host by default
    }
    const bool reuse = taken_over && p.GetBool("reuse", false);
    if (reuse && p.GetStr("title").empty()) {
      // keep the title and the layout
    } else {
      f.title = p.GetStr("title", f.title.empty() ? "Figure " + std::to_string(id) : f.title);
    }
    if (p.Has("height_ratios") || p.Has("width_ratios") || !reuse) {
      auto ratios = [&](const char* name, std::vector<float>* out) {
        out->clear();
        if (const MV* r = p.Get(name)) {
          for (const MV& v : r->a) out->push_back(static_cast<float>(std::max(0.01, v.Num())));
        }
      };
      ratios("height_ratios", &f.row_ratios);
      ratios("width_ratios", &f.col_ratios);
    }
    f.rows = static_cast<int>(std::clamp<int64_t>(p.GetInt("rows", f.rows), 1, 8));
    f.cols = static_cast<int>(std::clamp<int64_t>(p.GetInt("cols", f.cols), 1, 8));
    f.link_x = p.GetBool("link_x", f.link_x);
    if (p.Has("sync_cursor")) f.sync = p.GetStr("sync_cursor");
    if (p.Has("theme")) f.theme = PlotThemeFromName(p.GetStr("theme").c_str());
    if (const MV* sz = p.Get("size"); sz && sz->a.size() == 2) f.size = ImVec2(static_cast<float>(sz->a[0].Num()), static_cast<float>(sz->a[1].Num()));
    if (const MV* ps = p.Get("place"); ps && ps->a.size() == 2) {  // not "pos": that names a frame in the data channel
      f.pos = ImVec2(static_cast<float>(ps->a[0].Num()), static_cast<float>(ps->a[1].Num()));
      f.place = true;
    }
    f.axes.resize(static_cast<size_t>(f.rows * f.cols));
    f.open = true;
    rec.fig_id = MV::Int(id);
    return MV::Int(id);
  }
  auto it = g.figs.find(p.GetInt("fig", 0));
  if (it == g.figs.end()) {
    *err = "No figure " + std::to_string(p.GetInt("fig", 0)) + " (it was closed)";
    return MV();
  }
  Figure& f = it->second;
  if (m == "plot.close") {
    g.figs.erase(it);
    return MV::Bool(true);
  }
  if (m == "plot.reset") {  // a tape going back to its start: the figure's items and handles are rebuilt from the snapshot
    for (auto& ax : f.axes) ax.items.clear(), ax.handles.clear();
    return MV();
  }
  if (m == "plot.fit") {
    f.fit = true;
    for (auto& ax : f.axes) ax.user_view = false;
    return MV();
  }
  if (m == "plot.link") {
    f.link_table = p.GetStr("table");
    f.link_col = p.GetStr("column");
    return MV();
  }
  if (m == "plot.subscribe") {  // the events Python wants: click, dblclick, hover, select, drag; push = to the host
    f.want = 0;
    if (const MV* ev = p.Get("events")) {
      for (const MV& e : ev->a) {
        f.want |= e.s == "click" ? kEvClick : e.s == "dblclick" ? kEvDouble : e.s == "hover" ? kEvHover : e.s == "select" ? kEvSelect : e.s == "drag" ? kEvDrag : 0;
      }
    }
    f.push = p.GetBool("push", false);
    f.right = p.GetBool("right", f.right);
    return MV();
  }
  if (m == "plot.menu") {  // fig.context_menu(items): texts, "" = separator; choosing one sends a "menu" event
    f.menu.clear();
    if (const MV* items = p.Get("items")) {
      for (const MV& x : items->a) f.menu.push_back(x.t == MV::T::Str ? x.s : std::string());
    }
    return MV();
  }
  if (m == "plot.input") return FigureInput(f, p, err);  // tests and agents: the events a mouse would make
  if (m == "plot.dock") {  // Figure.Dock: into the main window beside the sheets (containers: container.dock)
    const std::string pos = p.GetStr("position", "right");
    f.dock_dir = pos == "left" ? 0 : pos == "right" ? 1 : pos == "top" ? 2 : pos == "bottom" ? 3 : 4;
    f.dock_ratio = static_cast<float>(std::clamp(p.GetDouble("ratio", 0.3), 0.05, 0.95));
    f.dock_anchor = p.GetInt("anchor", 0);
    f.dock_tries = 0;
    f.dock_pending = true;
    return MV::Bool(true);
  }
  if (m == "plot.item_data") {  // an item's columns as YCGrid has them (live items: the bars it built)
    const size_t xi = static_cast<size_t>(std::max<int64_t>(0, p.GetInt("ax", 0)));
    if (xi >= f.axes.size()) return *err = "The figure has no plot " + std::to_string(xi), MV();
    const std::string id = p.GetStr("item");
    for (const auto& pi : f.axes[xi].items) {
      if (pi.id != id) continue;
      MV o = MV::Map();
      for (const auto& [name, col] : pi.cols) {
        MV a = MV::Array();
        a.a.reserve(col.size());
        for (double v : col) a.Push(MV::Double(v));
        o.Set(name, std::move(a));
      }
      if (!pi.labels.empty()) {
        MV a = MV::Array();
        for (const auto& t : pi.labels) a.Push(MV::Str(t));
        o.Set("labels", std::move(a));
      }
      return o;
    }
    return *err = "No item " + id, MV();
  }
  if (m == "plot.events") {  // polling: the events since the last call
    MV a = MV::Array();
    for (auto& e : f.events) a.Push(std::move(e));
    f.events.clear();
    return a;
  }
  const size_t ai = static_cast<size_t>(std::max<int64_t>(0, p.GetInt("ax", 0)));
  if (ai >= f.axes.size()) {
    *err = "The figure has no plot " + std::to_string(ai);
    return MV();
  }
  PlotAxes& ax = f.axes[ai];
  if (m == "plot.axes") {
    if (p.Has("title")) ax.title = p.GetStr("title");
    if (p.Has("xlabel")) ax.xlabel = p.GetStr("xlabel");
    if (p.Has("ylabel")) ax.ylabel = p.GetStr("ylabel");
    if (p.Has("zlabel")) ax.zlabel = p.GetStr("zlabel");
    if (const MV* v = p.Get("view"); v && v->a.size() == 2) {
      ax.yaw = static_cast<float>(v->a[0].Num() * 3.14159265358979 / 180);
      ax.pitch = static_cast<float>(std::clamp(v->a[1].Num(), -89.0, 89.0) * 3.14159265358979 / 180);
    }
    if (p.Has("time")) ax.time_x = p.GetBool("time");
    if (p.Has("log_y")) ax.log_y = p.GetBool("log_y");
    if (p.Has("legend")) ax.legend = p.GetBool("legend");
    if (p.Has("equal")) ax.equal = p.GetBool("equal");
    if (p.Has("follow")) ax.follow = p.GetDouble("follow"), f.follow_paused = false;  // set from Python: follow again
    if (p.Has("yfit")) ax.yfit_stable = p.GetStr("yfit") == "stable";
    if (const MV* w = p.Get("ywidth")) {
      // a number of points, "stable" (only grows: tags coming and going don't move the plot) or "auto"
      if (w->IsNum()) ax.ywidth = static_cast<float>(w->Num()), ax.ywidth_mode = 2;
      else ax.ywidth = 0, ax.ywidth_mode = w->s == "stable" ? 1 : w->s == "labels" ? 2 : 0;
      ax.ywidth_seen = 0;
    }
    if (const MV* v = p.Get("xlim"); v && v->a.size() == 2) ax.has_xlim = ax.user_view = true, ax.x0 = v->a[0].Num(), ax.x1 = v->a[1].Num();
    if (const MV* v = p.Get("ylim"); v && v->a.size() == 2) ax.has_ylim = ax.y_set = ax.user_view = true, ax.y0 = v->a[0].Num(), ax.y1 = v->a[1].Num();
    if (p.GetBool("ylim_auto", false)) ax.y_set = false;  // set(ylim="auto"): y fits the data again
    return MV();
  }
  if (m == "plot.cursor") {  // the group's cursor set from Python (NaN clears it); a hover takes over
    auto fi = g.figs.find(p.GetInt("fig", 0));
    if (fi == g.figs.end()) return *err = "No such figure", MV();
    const std::string grp = SyncGroup(fi->second);
    if (grp.empty()) return *err = "The figure has no cursor group: plot.figure(sync_cursor=...)", MV();
    const double x = p.GetDouble("x", kNaN);
    if (std::isfinite(x)) {
      g_sync_set[grp] = SyncCursor{grp, x, false, true, -1, -1};
    } else {
      g_sync_set.erase(grp);
    }
    return MV();
  }
  if (m == "plot.handle") {  // a draggable line / point: set, move or remove
    const std::string id = p.GetStr("id"), op = p.GetStr("op", "set");
    auto h = std::find_if(ax.handles.begin(), ax.handles.end(), [&](const PlotAxes::Handle& x) { return x.id == id; });
    if (op == "remove") {
      if (h != ax.handles.end()) ax.handles.erase(h);
      return MV();
    }
    if (h == ax.handles.end()) {
      ax.handles.push_back({});
      h = ax.handles.end() - 1;
      h->id = id;
    }
    if (p.Has("kind")) h->kind = p.GetStr("kind");
    if (p.Has("label")) h->label = p.GetStr("label");
    if (p.Has("x")) h->x = p.GetDouble("x");
    if (p.Has("y")) h->y = p.GetDouble("y");
    if (p.Has("y1")) h->y1 = p.GetDouble("y1");
    if (p.Has("resize")) h->resize = p.GetBool("resize");
    if (p.Has("closable")) h->closable = p.GetBool("closable");
    if (h->kind == "band" && h->y > h->y1) std::swap(h->y, h->y1);
    if (p.Has("color")) h->color = static_cast<uint32_t>(p.GetInt("color"));
    if (p.Has("width")) h->width = static_cast<float>(p.GetDouble("width", 2));
    if (p.Has("fit")) h->fit = p.GetBool("fit");
    f.want |= kEvDrag;
    return MV();
  }
  if (m == "plot.item") {
    const std::string id = p.GetStr("item");
    const std::string op = p.GetStr("op", "set");
    auto pit = std::find_if(ax.items.begin(), ax.items.end(), [&](const PlotItem& x) { return x.id == id; });
    if (op == "remove") {
      if (pit != ax.items.end()) ax.items.erase(pit);
      return MV();
    }
    if (pit == ax.items.end()) {
      ax.items.push_back(PlotItem{});
      pit = ax.items.end() - 1;
      pit->id = id;
      f.fit = true;
    }
    PlotItem& item = *pit;
    if (p.Has("kind")) item.kind = p.GetStr("kind");
    ApplyStyle(item, p);
    if (const MV* lv = p.Get("live"); lv && lv->t == MV::T::Map) SetLive(item, *lv);
    if (const MV* files = p.Get("files"); files && files->t == MV::T::Map) {
      // columns read from disk by C++: hundreds of millions of points without passing through Python
      item.cols.clear();
      for (const auto& [name, path] : files->m) {
        NpyColumn col;
        std::string why;
        if (!ReadNpy(FromUtf8(path.s), &col, &why)) {
          *err = name + ": " + why + " (" + path.s + ")";
          return MV();
        }
        if (col.text) item.labels = std::move(col.str);
        else item.cols[name] = std::move(col.num);
      }
      TakeColors(item);
      item.dirty = true;
      return MV();
    }
    if (data) {
      // an item made empty and filled later (a live screen draws as its first ticks arrive): the plot fitted to nothing
      // on its first frame, so fit again when the first data come
      auto points_of = [](const PlotItem& it) {
        size_t n = 0;
        for (const auto& [name, col] : it.cols) n = std::max(n, col.size());
        return n;
      };
      const size_t before = points_of(item);  // 0 or 1 point: no usable range yet
      if (op == "append") {
        for (size_t c = 0; c < data->names.size(); ++c) {
          if (data->num[c].empty()) continue;
          auto& dst = item.cols[data->names[c]];
          dst.insert(dst.end(), data->num[c].begin(), data->num[c].end());
          if (item.capacity && dst.size() > item.capacity + item.capacity / 4) dst.erase(dst.begin(), dst.begin() + static_cast<std::ptrdiff_t>(dst.size() - item.capacity));
        }
      } else {
        item.cols.clear();
        for (size_t c = 0; c < data->names.size(); ++c) {
          if (!data->num[c].empty()) item.cols[data->names[c]] = std::move(data->num[c]);
          else if (!data->text[c].empty()) item.labels = data->text[c];
        }
      }
      TakeColors(item);
      item.dirty = true;
      // the view follows the data (a payoff recomputed, a smile refitted) until the user zooms or pans; following
      // plots set their own window; otherwise fit when the first data come
      if (!ax.user_view && ax.follow <= 0 && !ax.y_set) item.want_fit = true;
      else if (before < 2 && points_of(item) > before) f.fit = true;
      if (item.kind == "candles" && !item.live_topic.empty()) {
        SyncLiveVolume(f, *pit);  // history loaded into live candles: their volume bars at once
        f.fit = true;
      }
    }
    return MV();
  }
  *err = "Unknown plot request: " + m;
  return MV();
}

std::vector<std::tuple<std::string, io::MV, std::shared_ptr<engine::RawFrame>>> FigureSnapshot() {
  std::vector<std::tuple<std::string, io::MV, std::shared_ptr<engine::RawFrame>>> out;
  for (const auto& [id, f] : G().figs) {
    MV fp = MV::Map();
    fp.Set("fig", MV::Int(id));
    fp.Set("title", MV::Str(f.title));
    fp.Set("rows", MV::Int(f.rows));
    fp.Set("cols", MV::Int(f.cols));
    fp.Set("link_x", MV::Bool(f.link_x));
    if (!f.sync.empty()) fp.Set("sync_cursor", MV::Str(f.sync));
    fp.Set("theme", MV::Str(f.theme == PlotTheme::Dark ? "dark" : "light"));
    out.emplace_back("plot.figure", fp, nullptr);
    MV rp = MV::Map();
    rp.Set("fig", MV::Int(id));
    out.emplace_back("plot.reset", rp, nullptr);
    for (size_t a = 0; a < f.axes.size(); ++a) {
      const PlotAxes& ax = f.axes[a];
      MV ap = MV::Map();
      ap.Set("fig", MV::Int(id));
      ap.Set("ax", MV::Int(static_cast<int64_t>(a)));
      ap.Set("title", MV::Str(ax.title));
      ap.Set("time", MV::Bool(ax.time_x));
      ap.Set("follow", MV::Double(ax.follow));
      out.emplace_back("plot.axes", ap, nullptr);
      for (const auto& it : ax.items) {
        MV ip = MV::Map();
        ip.Set("fig", MV::Int(id));
        ip.Set("ax", MV::Int(static_cast<int64_t>(a)));
        ip.Set("item", MV::Str(it.id));
        ip.Set("kind", MV::Str(it.kind));
        ip.Set("op", MV::Str("set"));
        ip.Set("label", MV::Str(it.label));
        if (it.color) ip.Set("color", MV::Int(it.color));
        ip.Set("width", MV::Double(it.width));
        ip.Set("text", MV::Str(it.text));
        if (!it.live_topic.empty()) ip.Set("live", LiveToMV(it));
        auto data = std::make_shared<engine::RawFrame>();
        for (const auto& [name, col] : it.cols) {
          data->names.push_back(name);
          data->num.push_back(col);
          data->text.emplace_back();
          data->nrows = std::max<uint32_t>(data->nrows, static_cast<uint32_t>(col.size()));
        }
        if (!it.labels.empty()) {
          data->names.push_back("labels");
          data->num.emplace_back();
          data->text.push_back(it.labels);
        }
        out.emplace_back("plot.item", ip, data->names.empty() ? nullptr : data);
      }
    }
  }
  return out;
}

namespace {
std::map<int64_t, Figure> g_backup;  // the figures a replay took over, as they were
}  // namespace

void FigureBackup(const std::unordered_set<int64_t>& figs, bool restore) {
  FigureStore& g = G();
  if (!restore) {
    g_backup.clear();
    for (int64_t id : figs) {
      if (auto it = g.figs.find(id); it != g.figs.end()) g_backup[id] = it->second;
    }
    return;
  }
  for (int64_t id : figs) {
    if (auto b = g_backup.find(id); b != g_backup.end()) g.figs[id] = b->second;
    else g.figs.erase(id);  // made by the replay
  }
  g_backup.clear();
}

// the open figures for ui.state: id, title, and per plot its title and items (kind, label, points)
void SetFigureOwner(int64_t owner, const std::string& name, bool host) {
  g_request_owner = owner;
  g_request_owner_name = name;
  g_request_owner_host = host;
}

// a connection closed (the host exited or restarted, a script ended): its figures close, or show "(stopped)" when they
// were made with persist=True
void FiguresOwnerGone(int64_t owner) {
  auto& figs = G().figs;
  for (auto it = figs.begin(); it != figs.end();) {
    Figure& f = it->second;
    if (owner && f.owner == owner) {
      f.alive = false;
      f.push = false;
      if (!f.persist) {
        it = figs.erase(it);
        continue;
      }
    }
    ++it;
  }
}

std::string FigureWindowName(const std::string& key_or_id) {
  for (const auto& [id, f] : G().figs) {
    if ((!f.key.empty() && f.key == key_or_id) || std::to_string(id) == key_or_id) return FigureTitle(f);
  }
  return {};
}

std::vector<std::string> FigureWindowNames() {
  std::vector<std::string> out;
  for (const auto& [id, f] : G().figs) {
    if (!f.key.empty() && f.open) out.push_back(FigureTitle(f));
  }
  return out;
}

io::MV FiguresState() {
  MV out = MV::Array();
  for (const auto& [id, f] : G().figs) {
    MV fm = MV::Map();
    fm.Set("id", MV::Int(id));
    if (ImGuiWindow* w = ImGui::FindWindowByName(FigureTitle(f).c_str())) {
      fm.Set("docked", MV::Bool(w->DockIsActive));
      if (w->DockNode) {
        ImGuiDockNode* root = w->DockNode;
        while (root->ParentNode) root = root->ParentNode;
        fm.Set("dock_root", MV::Int(static_cast<int64_t>(root->ID)));
      }
    }
    fm.Set("title", MV::Str(f.title));
    fm.Set("key", MV::Str(f.key));
    fm.Set("owner", MV::Str(f.owner_name));
    fm.Set("alive", MV::Bool(f.alive));
    fm.Set("follow_paused", MV::Bool(f.follow_paused));
    if (!f.menu.empty()) {
      MV mi = MV::Array();
      for (const auto& t : f.menu) mi.Push(MV::Str(t));
      fm.Set("menu", std::move(mi));
    }
    if (!f.sync.empty()) fm.Set("sync_cursor", MV::Str(f.sync));
    MV axes = MV::Array();
    for (const auto& ax : f.axes) {
      MV am = MV::Map();
      am.Set("title", MV::Str(ax.title));
      if (std::isfinite(ax.synced_x)) am.Set("synced_x", MV::Double(ax.synced_x));
      if (!ax.handles.empty()) {
        MV hs = MV::Array();
        for (const auto& h : ax.handles) {
          MV hm = MV::Map();
          hm.Set("id", MV::Str(h.id));
          hm.Set("kind", MV::Str(h.kind));
          hm.Set("label", MV::Str(h.label));
          hm.Set("x", MV::Double(h.x));
          hm.Set("y", MV::Double(h.y));
          if (h.kind == "band") hm.Set("y1", MV::Double(h.y1));
          if (h.closable) hm.Set("closable", MV::Bool(true));
          hs.Push(std::move(hm));
        }
        am.Set("handles", std::move(hs));
      }
      MV items = MV::Array();
      for (const auto& it : ax.items) {
        MV im = MV::Map();
        im.Set("kind", MV::Str(it.kind));
        im.Set("label", MV::Str(it.label));
        size_t n = 0;
        for (const auto& [name, col] : it.cols) n = std::max(n, col.size());
        im.Set("points", MV::Int(static_cast<int64_t>(n)));
        items.Push(std::move(im));
      }
      am.Set("items", std::move(items));
      MV view = MV::Array();
      for (double v : ax.view) view.Push(MV::Double(v));
      am.Set("view", std::move(view));
      axes.Push(std::move(am));
    }
    fm.Set("plots", std::move(axes));
    out.Push(std::move(fm));
  }
  return out;
}

// ---- drawing (every frame) ----

// Axis tick labels: ImPlot's "%g" prints 8e+06 for an equity of 8,000,000; numbers from the threshold (the pointer
// value) up get thousands separators ("8,000,000", "23,001.5"), smaller ones keep "%g".
int AxisNumberFormatter(double v, char* buf, int size, void* threshold) {
  const double a = std::fabs(v);
  if (std::isfinite(v) && a >= static_cast<double>(reinterpret_cast<intptr_t>(threshold)) && a < 1e10) {
    char digits[48];
    std::snprintf(digits, sizeof(digits), "%.10g", a);  // 10 significant digits: tick arithmetic noise disappears
    const char* dot = std::strchr(digits, '.');
    const int n = dot ? static_cast<int>(dot - digits) : static_cast<int>(std::strlen(digits));
    std::string out = v < 0 ? "-" : "";
    for (int i = 0; i < n; ++i) {
      if (i > 0 && (n - i) % 3 == 0) out += ',';
      out += digits[i];
    }
    if (dot) out += dot;
    std::snprintf(buf, static_cast<size_t>(size), "%s", out.c_str());
    return static_cast<int>(out.size());
  }
  return std::snprintf(buf, static_cast<size_t>(size), "%g", v);
}

// time axes spanning up to two days: ticks on whole local seconds / minutes / hours with short labels ("22:45",
// "22:45:30"); ImPlot's own time ticks put the date on the first label, where it runs into the next one
void IntradayTicks(bool show_labels) {
  const ImPlotPlot* plot = ImPlot::GetCurrentPlot();
  const ImPlotRange& r = plot->Axes[ImAxis_X1].Range;  // last frame's range
  const double span = r.Max - r.Min;
  const float px = plot->PlotRect.GetWidth();
  if (!(span > 1.0 && span <= 2 * 86400.0) || px < 50) return;
  static constexpr double kSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200};
  double step = kSteps[std::size(kSteps) - 1];
  for (double st : kSteps) {
    const float label_px = st < 60 ? 78.0f : 58.0f;
    if (span / st * S(label_px) <= px) {
      step = st;
      break;
    }
  }
  // the local time zone's offset, so ticks fall on local minutes and hours
  const time_t now = static_cast<time_t>(r.Max);
  tm lt{}, gt{};
  localtime_s(&lt, &now);
  gmtime_s(&gt, &now);
  const double off = static_cast<double>(_mkgmtime(&lt) - _mkgmtime(&gt));
  std::vector<double> ticks;
  std::vector<std::string> text;
  for (double t = std::ceil((r.Min + off) / step) * step - off; t <= r.Max && ticks.size() < 200; t += step) {
    ticks.push_back(t);
    const time_t tt = static_cast<time_t>(std::llround(t));
    tm x{};
    localtime_s(&x, &tt);
    char buf[16];
    if (step < 60) std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", x.tm_hour, x.tm_min, x.tm_sec);
    else std::snprintf(buf, sizeof(buf), "%02d:%02d", x.tm_hour, x.tm_min);
    text.emplace_back(show_labels ? buf : "");
  }
  if (ticks.size() < 2) return;
  std::vector<const char*> labels;
  for (const auto& t : text) labels.push_back(t.c_str());
  ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(), static_cast<int>(ticks.size()), labels.data());
}

// ax.set(yfit="stable") on a following plot: the y range moves only when the visible data come near its edges or
// fill less than a third of it - then it is set with room around the data - so a live chart doesn't jitter
void StableY(PlotAxes& ax, double x0, double x1) {
  double lo = std::numeric_limits<double>::infinity(), hi = -lo;
  auto scan = [&](const std::vector<double>& xs, const std::vector<double>& ys) {
    const size_t n = std::min(xs.size(), ys.size());
    if (!n) return;
    const auto from = static_cast<size_t>(std::lower_bound(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(n), x0) - xs.begin());
    for (size_t i = from; i < n && xs[i] <= x1; ++i) {
      if (std::isfinite(ys[i])) lo = std::min(lo, ys[i]), hi = std::max(hi, ys[i]);
    }
  };
  for (const auto& it : ax.items) {
    const auto& x = ColOf(it, "x");
    if (it.kind == "candles") {
      scan(x, ColOf(it, "low"));
      scan(x, ColOf(it, "high"));
    } else if (it.kind == "line" || it.kind == "step" || it.kind == "scatter" || it.kind == "bar") {
      scan(x, ColOf(it, "y"));
      if (it.kind == "bar") lo = std::min(lo, 0.0), hi = std::max(hi, 0.0);
    } else if (it.kind == "rects") {  // footprint cells, zones: the ones overlapping the window (not sorted by x)
      const auto &a0 = ColOf(it, "x0"), &a1 = ColOf(it, "x1"), &b0 = ColOf(it, "y0"), &b1 = ColOf(it, "y1");
      const size_t n = std::min({a0.size(), a1.size(), b0.size(), b1.size()});
      for (size_t i = 0; i < n; ++i) {
        if (a1[i] < x0 || a0[i] > x1 || !std::isfinite(b0[i]) || !std::isfinite(b1[i])) continue;
        lo = std::min({lo, b0[i], b1[i]}), hi = std::max({hi, b0[i], b1[i]});
      }
    } else if (it.kind == "hband" && it.fit_band) {
      for (double v : ColOf(it, "y0")) lo = std::min(lo, v), hi = std::max(hi, v);
      for (double v : ColOf(it, "y1")) lo = std::min(lo, v), hi = std::max(hi, v);
    }
  }
  for (const auto& h : ax.handles) {
    if (!h.fit) continue;
    lo = std::min(lo, h.y), hi = std::max(hi, h.kind == "band" ? h.y1 : h.y);
  }
  if (!std::isfinite(lo) || !std::isfinite(hi)) return;
  // last frame's range (GetPlotLimits would end the setup)
  const ImPlotRange& cur = ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1].Range;
  const double span = cur.Max - cur.Min, data = std::max(hi - lo, 1e-9);
  const bool inside = (lo >= cur.Min + span * 0.06 || (lo >= 0 && cur.Min == 0)) && hi <= cur.Max - span * 0.06;
  if (inside && data >= span * 0.33 && span > 0) return;
  const double pad = std::max(data * 0.35, 1e-6);
  const double y0 = lo >= 0 && lo - pad < 0 ? 0.0 : lo - pad;  // volumes and counts don't go below zero
  ImPlot::SetupAxisLimits(ImAxis_Y1, y0, hi + pad, ImPlotCond_Always);
}

// handles and events of one plot (between BeginPlot and EndPlot)
// the y axis keeps its width: ImPlot sizes it for its tick labels and, a frame later, its widest tag, so a tag that
// comes or goes (an order line, the average cost) moved the whole plot sideways
void KeepYWidth(PlotAxes& ax) {
  ImPlotAxis& y = ImPlot::GetCurrentPlot()->Axes[ImAxis_Y1];
  const bool stable = ax.ywidth_mode == 1 || (ax.ywidth_mode == 0 && ax.yfit_stable);
  float want = ax.ywidth > 0 ? S(ax.ywidth * 4.0f / 3.0f) : 0.0f;
  if (stable) {
    ax.ywidth_seen = std::max(ax.ywidth_seen, y.Ticker.MaxSize.x);
    want = std::max(want, ax.ywidth_seen);
  }
  if (want > 0) y.Ticker.OverrideSizeLate(ImVec2(want, 0));
}

// a plot the user zooms (wheel, box) or pans keeps that view when its data change; a double-click fits it to the data
// and lets it follow its data again
void UserView(PlotAxes& ax) {
  if (ax.dragging || ax.selecting) return;
  const ImGuiIO& io = ImGui::GetIO();
  const ImPlotPlot* plot = ImPlot::GetCurrentPlot();
  const bool over = ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1);
  if ((over && io.MouseWheel != 0) || plot->Selecting ||
      ((plot->Held || plot->Axes[ImAxis_X1].Held || plot->Axes[ImAxis_Y1].Held) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)))
    ax.user_view = true;
  if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ax.user_view = false;
}

// a following plot (follow=N) the user zooms or pans stops following, so they can look closely (set a level, read an
// old bar); a click on the "Paused" badge goes back to the live end (a double-click stays free for the user's code).
// A "follow" event tells Python (paused True / False).
template <class Base>
void FollowPause(Figure& f, PlotAxes& ax, int a, Base&& base) {
  const ImGuiIO& io = ImGui::GetIO();
  const bool over = ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1);
  bool pause = false;
  if (!ax.dragging && !ax.selecting) {
    const ImPlotPlot* plot = ImPlot::GetCurrentPlot();
    if (over && io.MouseWheel != 0) pause = true;
    // panning the plot or dragging an axis, a box zoom: ImPlot's own state, so a handle being dragged doesn't count
    if ((plot->Held || plot->Axes[ImAxis_X1].Held || plot->Axes[ImAxis_Y1].Held) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) pause = true;
    // a box zoom the mouse has drawn (ImPlot starts "selecting" at the press: a right-click that opens the figure's
    // menu isn't a zoom and kept the chart following)
    if (plot->Selecting) {
      const ImVec2 d(io.MousePos.x - plot->SelectStart.x, io.MousePos.y - plot->SelectStart.y);
      if (d.x * d.x + d.y * d.y > 16.0f) pause = true;
    }
  }
  if (pause && !f.follow_paused) {
    f.follow_paused = true;
    MV ev = base("follow");
    ev.Set("paused", MV::Bool(f.follow_paused));
    Emit(f, std::move(ev));
  }
  if (f.follow_paused && a == 0) {  // a badge in the corner; clicking it goes back live
    const char* text = TR("Paused - click here to follow live");
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p1(ImPlot::GetPlotPos().x + ImPlot::GetPlotSize().x - S(8), ImPlot::GetPlotPos().y + S(8));
    const ImVec2 p0(p1.x - ts.x - S(12), p1.y);
    const ImVec2 p2(p1.x, p1.y + ts.y + S(6));
    const ImVec2 m = io.MousePos;
    const bool hot = m.x >= p0.x && m.x <= p2.x && m.y >= p0.y && m.y <= p2.y && ImPlot::IsPlotHovered();
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const bool dark = g_draw_theme == PlotTheme::Dark;
    const ImU32 bg = hot ? IM_COL32(30, 90, 160, 235) : (dark ? IM_COL32(40, 40, 40, 220) : IM_COL32(235, 235, 235, 230));
    dl->AddRectFilled(p0, p2, bg, S(3));
    dl->AddText(ImVec2(p0.x + S(6), p0.y + S(3)), hot || dark ? IM_COL32(230, 230, 230, 255) : IM_COL32(40, 40, 40, 255), text);
    if (hot) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        f.follow_paused = false;
        f.fit = true;
        MV ev = base("follow");
        ev.Set("paused", MV::Bool(false));
        Emit(f, std::move(ev));
      }
    }
  }
}

void PlotInteraction(Figure& f, PlotAxes& ax, int a) {
  const double now = ImGui::GetTime();
  KeepYWidth(ax);
  if (f.menu_input == 1 && a == f.menu_ax) f.menu_px = ImPlot::PlotToPixels(f.menu_at), f.menu_input = 2;
  auto base = [&](const char* type) {
    const ImGuiIO& kio = ImGui::GetIO();
    MV ev = MV::Map();
    ev.Set("type", MV::Str(type));
    ev.Set("ax", MV::Int(a));
    ev.Set("ctrl", MV::Bool(kio.KeyCtrl));
    ev.Set("shift", MV::Bool(kio.KeyShift));
    ev.Set("alt", MV::Bool(kio.KeyAlt));
    return ev;
  };
  // draggable handles: "drag" while held (at most 30 a second), "release" when let go
  int hid = 0;
  for (auto& h : ax.handles) {
    bool clicked = false, hovered = false, held = false;
    const ImVec4 col = h.color ? Col(h.color) : ImVec4(1.0f, 0.55f, 0.0f, 1.0f);
    const int id = 7000 + hid++;
    bool moved = false;
    if (h.kind == "band") {
      // the band spans the plot's width; DragRect moves it by its body and resizes it by its edges
      const ImPlotRect lim = ImPlot::GetPlotLimits();
      double x0 = lim.X.Min, x1 = lim.X.Max, y0 = h.y, y1 = h.y1;
      const double height = h.y1 - h.y;
      ImVec4 fill = col;
      fill.w = 0.18f;
      ImDrawList* dl = ImPlot::GetPlotDrawList();
      dl->AddRectFilled(ImPlot::PlotToPixels(x0, y1), ImPlot::PlotToPixels(x1, y0), ImGui::ColorConvertFloat4ToU32(fill));
      moved = ImPlot::DragRect(id, &x0, &y0, &x1, &y1, col, ImPlotDragToolFlags_NoFit, &clicked, &hovered, &held);
      if (h.fit && ImPlot::FitThisFrame()) ImPlot::FitPointY(h.y), ImPlot::FitPointY(h.y1);
      if (held) ax.dragging = true;
      if (moved) {
        if (y0 > y1) std::swap(y0, y1);
        if (!h.resize) {  // move only: keep its height
          const double mid = (y0 + y1) / 2;
          y0 = mid - height / 2, y1 = mid + height / 2;
        }
        h.y = y0, h.y1 = y1;
      }
      if (!h.label.empty()) {
        const ImVec2 tp = ImPlot::PlotToPixels(lim.X.Min, h.y1);
        dl->AddText(ImVec2(tp.x + S(4), tp.y + S(2)), ImGui::ColorConvertFloat4ToU32(col), h.label.c_str());
      }
      const bool released = h.held && !held;
      if ((moved && now - h.sent_at > 1.0 / 30) || released) {
        MV ev = base(released ? "release" : "drag");
        ev.Set("handle", MV::Str(h.id));
        ev.Set("x", MV::Double(h.x));
        ev.Set("y", MV::Double(h.y));
        ev.Set("y0", MV::Double(h.y));
        ev.Set("y1", MV::Double(h.y1));
        Emit(f, std::move(ev));
        h.sent_at = now;
      }
      h.held = held;
      continue;
    }
    const ImPlotDragToolFlags dflags = h.fit ? ImPlotDragToolFlags_None : ImPlotDragToolFlags_NoFit;
    if (h.kind == "vline") moved = ImPlot::DragLineX(id, &h.x, col, h.width, dflags, &clicked, &hovered, &held);
    else if (h.kind == "point") moved = ImPlot::DragPoint(id, &h.x, &h.y, col, h.width * 2.5f, dflags, &clicked, &hovered, &held);
    else moved = ImPlot::DragLineY(id, &h.y, col, h.width, dflags, &clicked, &hovered, &held);
    if (held) ax.dragging = true;
    if (!h.label.empty()) {
      if (h.kind == "vline") ImPlot::TagX(h.x, col, "%s", h.label.c_str());
      else if (h.kind == "point") ImPlot::Annotation(h.x, h.y, col, ImVec2(S(10), -S(10)), true, "%s", h.label.c_str());
      else QueueTagY(h.y, col, h.label);
    }
    if (h.closable && h.kind != "point") {
      // an "x" button at the left (hline) or top (vline) end: a click sends "close"
      const ImPlotRect lim = ImPlot::GetPlotLimits();
      const ImVec2 at = h.kind == "vline" ? ImPlot::PlotToPixels(h.x, lim.Y.Max) : ImPlot::PlotToPixels(lim.X.Min, h.y);
      const float bs = S(13);
      const ImVec2 b0 = h.kind == "vline" ? ImVec2(at.x - bs / 2, at.y + S(2)) : ImVec2(at.x + S(3), at.y - bs / 2);
      const ImVec2 b1(b0.x + bs, b0.y + bs);
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const bool over = ImPlot::IsPlotHovered() && mouse.x >= b0.x && mouse.x <= b1.x && mouse.y >= b0.y && mouse.y <= b1.y;
      ImDrawList* dl = ImPlot::GetPlotDrawList();
      ImVec4 bg = col;
      bg.w = over ? 1.0f : 0.8f;
      dl->AddRectFilled(b0, b1, ImGui::ColorConvertFloat4ToU32(bg), S(2));
      const ImU32 fg = IM_COL32(255, 255, 255, 255);
      const float m = S(3.5f);
      dl->AddLine(ImVec2(b0.x + m, b0.y + m), ImVec2(b1.x - m, b1.y - m), fg, S(1.5f));
      dl->AddLine(ImVec2(b1.x - m, b0.y + m), ImVec2(b0.x + m, b1.y - m), fg, S(1.5f));
      if (over) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          MV ev = base("close");
          ev.Set("handle", MV::Str(h.id));
          ev.Set("x", MV::Double(h.x));
          ev.Set("y", MV::Double(h.y));
          Emit(f, std::move(ev));
        }
      }
    }
    const bool released = h.held && !held;
    if ((moved && now - h.sent_at > 1.0 / 30) || released) {
      MV ev = base(released ? "release" : "drag");
      ev.Set("handle", MV::Str(h.id));
      ev.Set("x", MV::Double(h.x));
      ev.Set("y", MV::Double(h.y));
      Emit(f, std::move(ev));
      h.sent_at = now;
    }
    h.held = held;
  }
  if (ax.follow > 0) FollowPause(f, ax, a, base);
  else UserView(ax);
  if (!ImPlot::IsPlotHovered() && !ax.selecting) return;
  const ImPlotPoint mp = ImPlot::GetPlotMousePos();
  const ImGuiIO& io = ImGui::GetIO();
  // Ctrl + drag: a selection rectangle, reported on release (the plain drag still pans)
  if ((f.want & kEvSelect) && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ax.selecting = true, ax.sel0 = ax.sel1 = mp;
  if (ax.selecting) {
    ax.sel1 = mp;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    const ImVec2 p0 = ImPlot::PlotToPixels(ax.sel0), p1 = ImPlot::PlotToPixels(ax.sel1);
    dl->AddRectFilled(ImMin(p0, p1), ImMax(p0, p1), IM_COL32(80, 140, 255, 50));
    dl->AddRect(ImMin(p0, p1), ImMax(p0, p1), IM_COL32(80, 140, 255, 200));
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ax.selecting = false;
      MV ev = base("select");
      ev.Set("x0", MV::Double(std::min(ax.sel0.x, ax.sel1.x)));
      ev.Set("x1", MV::Double(std::max(ax.sel0.x, ax.sel1.x)));
      ev.Set("y0", MV::Double(std::min(ax.sel0.y, ax.sel1.y)));
      ev.Set("y1", MV::Double(std::max(ax.sel0.y, ax.sel1.y)));
      Emit(f, std::move(ev));
    }
    return;
  }
  auto with_point = [&](MV& ev) {
    ev.Set("x", MV::Double(mp.x));
    ev.Set("y", MV::Double(mp.y));
    // the nearest point of the first line / scatter / bar / candle item along x (its row in the data)
    for (const auto& it : ax.items) {
      if (it.kind != "line" && it.kind != "scatter" && it.kind != "bar" && it.kind != "step" && it.kind != "candles") continue;
      const auto& x = ColOf(it, "x");
      if (x.empty()) continue;
      size_t k = 0;
      if (it.sorted) {
        k = static_cast<size_t>(std::lower_bound(x.begin(), x.end(), mp.x) - x.begin());
        if (k >= x.size() || (k > 0 && std::abs(x[k - 1] - mp.x) < std::abs(x[k] - mp.x))) k = k ? k - 1 : 0;
      }
      ev.Set("item", MV::Str(it.id));
      ev.Set("index", MV::Int(static_cast<int64_t>(k)));
      break;
    }
  };
  if ((f.want & (kEvClick | kEvDouble)) && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 16 &&
      !ImPlot::IsAxisHovered(ImAxis_X1)) {
    MV ev = base("click");
    ev.Set("button", MV::Str("left"));
    with_point(ev);
    if (f.want & kEvClick) Emit(f, std::move(ev));
  }
  if ((f.want & kEvClick) && ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
    MV ev = base("click");
    ev.Set("button", MV::Str("middle"));
    with_point(ev);
    Emit(f, std::move(ev));
  }
  // a right click (not Shift: that opens ImPlot's own menu): to Python, and the figure's context menu
  if ((f.right || !f.menu.empty()) && !io.KeyShift && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      io.MouseDragMaxDistanceSqr[1] < 16) {
    if (f.right && (f.want & kEvClick)) {
      MV ev = base("click");
      ev.Set("button", MV::Str("right"));
      with_point(ev);
      Emit(f, std::move(ev));
    }
    if (!f.menu.empty()) {
      f.menu_request = true;
      f.menu_ax = a;
      f.menu_at = mp;
    }
  }
  if ((f.want & kEvDouble) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    MV ev = base("dblclick");
    with_point(ev);
    Emit(f, std::move(ev));
  }
  if ((f.want & kEvHover) && now - ax.hover_at > 1.0 / 20 && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
    ax.hover_at = now;
    MV ev = base("hover");
    with_point(ev);
    Emit(f, std::move(ev));
  }
}

void DrawFigures(AppState& s) {
  g_events_to = s.python.get();
  FigureStore& g = G();
  ApplyLiveUpdates(g);
  g_sync_prev = g_sync_cur;  // the hover of the last frame is drawn in the other plots of its group this frame
  g_sync_cur = SyncCursor{};
  for (auto it = g.figs.begin(); it != g.figs.end();) {
    Figure& f = it->second;
    if (!f.open) {
      it = g.figs.erase(it);
      continue;
    }
    const std::string title = FigureTitle(f);
    if (f.dock_pending) {  // Figure.Dock: beside the sheets in the main window, or beside / as a tab of another figure
      f.dock_pending = false;
      std::string anchor = kGridHostWindow;
      if (f.dock_anchor) {
        auto a = g.figs.find(static_cast<decltype(g.figs)::key_type>(f.dock_anchor));
        if (a != g.figs.end() && a->second.open) anchor = FigureTitle(a->second);
      }
      if (!ui::dock::DockBesideWindow(title.c_str(), anchor.c_str(), f.dock_dir, f.dock_ratio) && ++f.dock_tries < 30)
        f.dock_pending = true;  // the anchor was made this frame and isn't docked yet
    }
    // a keyed figure placed by a restored layout keeps that place
    const bool restored = !f.place && !f.key.empty() && ui::dock::ApplyNextWindowRect(title.c_str());
    if (!restored) {
      // new figures cascade instead of stacking on one spot
      const ImVec2 vp = ImGui::GetMainViewport()->WorkPos, ws = ImGui::GetMainViewport()->WorkSize;
      const float step = S(36.0f) * static_cast<float>((f.id - 1) % 8);
      if (f.place) {
        ImGui::SetNextWindowPos(ImVec2(vp.x + S(f.pos.x), vp.y + S(f.pos.y)), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(S(f.size.x), S(f.size.y)), ImGuiCond_Always);
        f.place = false;
      } else {
        // the first place stays inside the main window: at a high DPI or in a small window the default size
        // shrinks and the cascade moves up / left, instead of the window hanging off the screen
        const ImVec2 sz(std::min(S(f.size.x), ws.x * 0.85f), std::min(S(f.size.y), ws.y * 0.85f));
        ImVec2 pos(vp.x + S(120.0f) + step, vp.y + S(140.0f) + step);
        pos.x = std::max(vp.x, std::min(pos.x, vp.x + ws.x - sz.x));
        pos.y = std::max(vp.y, std::min(pos.y, vp.y + ws.y - sz.y));
        ImGui::SetNextWindowSize(sz, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
      }
    }
    g_draw_theme = f.theme;
    PushPlotTheme(f.theme);
    // figures belong to the session; a keyed one keeps its window settings (dock place) for a layout or a figure
    // made again with that key
    const bool locked = s.WindowLocked("figure:" + (f.key.empty() ? "#" + std::to_string(f.id) : f.key));
    const int lock_flags = LockWindow(locked);
    const bool shown = ImGui::Begin(title.c_str(), locked ? nullptr : &f.open, (f.key.empty() ? ImGuiWindowFlags_NoSavedSettings : ImGuiWindowFlags_None) | lock_flags);
    // the user clicked this figure: it has the keyboard (a figure that only popped up doesn't take the keys typed
    // into the sheet)
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
      ImGuiWindow* cw = ImGui::GetCurrentWindow();
      s.figure_keys = (cw->RootWindowDockTree ? cw->RootWindow : cw)->ID;
    }
    if (shown) {
      uint32_t lsheet = 0;
      const model::Table* ltab = nullptr;
      int lcol = -1;
      const double cursor = LinkedX(s, f, &lsheet, &ltab, &lcol);
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      const bool multi = f.rows * f.cols > 1;
      bool in_subplots = false;
      if (multi) {
        ImPlotSubplotFlags sf = ImPlotSubplotFlags_None;
        if (f.link_x) sf |= ImPlotSubplotFlags_LinkAllX;
        if ((f.right || !f.menu.empty()) && !ImGui::GetIO().KeyShift) sf |= ImPlotSubplotFlags_NoMenus;
        float* rr = static_cast<int>(f.row_ratios.size()) == f.rows ? f.row_ratios.data() : nullptr;
        float* cr = static_cast<int>(f.col_ratios.size()) == f.cols ? f.col_ratios.data() : nullptr;
        in_subplots = ImPlot::BeginSubplots(("##sub" + std::to_string(f.id)).c_str(), f.rows, f.cols, avail, sf, rr, cr);
      }
      if (!multi || in_subplots) {
        // time axes show local time, like the sheet, on a 24-hour clock (shorter labels that don't collide)
        const bool local = ImPlot::GetStyle().UseLocalTime, h24 = ImPlot::GetStyle().Use24HourClock;
        ImPlot::GetStyle().UseLocalTime = true;
        ImPlot::GetStyle().Use24HourClock = true;
        for (size_t a = 0; a < f.axes.size(); ++a) {
          PlotAxes& ax = f.axes[a];
          bool refit = f.fit;
          for (auto& item : ax.items) {
            Prepare(item);
            refit = refit || item.want_fit;
            item.want_fit = false;
          }
          if (refit && ax.y_set) {
            // limits given with ylim stay when new data make the plot fit: only x fits
            if (!ax.has_xlim) ImPlot::SetNextAxisToFit(ImAxis_X1);
            ax.has_ylim = true;
          } else if (refit && !ax.has_xlim && !ax.has_ylim) {
            ImPlot::SetNextAxesToFit();  // explicit limits win over the fit
          }
          const std::string pid = (ax.title.empty() ? std::string() : ax.title) + "##plot" + std::to_string(a);
          ImPlotFlags pf = ax.legend ? ImPlotFlags_None : ImPlotFlags_NoLegend;
          if ((f.right || !f.menu.empty()) && !ImGui::GetIO().KeyShift) pf |= ImPlotFlags_NoMenus;  // Shift + right click: ImPlot's menu
          if (ax.equal) pf |= ImPlotFlags_Equal;
          // a time axis with nothing on it yet (no candles for a symbol, data still loading) shows no times: its
          // default 0..1 would read 1/1/70 00:00 under the axis and at the mouse
          const bool empty_time =
              ax.time_x && std::none_of(ax.items.begin(), ax.items.end(), [](const PlotItem& i) { return !ColOf(i, "x").empty(); });
          if (empty_time) pf |= ImPlotFlags_NoMouseText;
          if (HasSurface(ax)) {
            // a 3-D view: the plot area is a canvas
            if (ImPlot::BeginPlot(pid.c_str(), multi ? ImVec2(-1, 0) : ImVec2(-1, -1),
                                  ImPlotFlags_NoInputs | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
              ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
              ImPlot::SetupFinish();
              DrawSurface3D(ax);
              ImPlot::EndPlot();
            }
            continue;
          }
          // 5% room above and below the data, so a flat maximum or minimum isn't hidden under the frame (heatmaps fill
          // their plot)
          const bool fills = std::any_of(ax.items.begin(), ax.items.end(), [](const PlotItem& i) { return i.kind == "heatmap"; });
          ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding, ImVec2(0.0f, fills ? 0.0f : 0.05f));
          const bool plot_open = ImPlot::BeginPlot(pid.c_str(), multi ? ImVec2(-1, 0) : ImVec2(-1, -1), pf);
          if (plot_open) {
            const bool was_dragging = ax.dragging;
            ax.dragging = false;
            ImPlotAxisFlags yf = ImPlotAxisFlags_None;
            if (ax.follow > 0 && !ax.y_set && !ax.yfit_stable && !was_dragging && !f.follow_paused) yf |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;
            // linked x axes (a candle chart over its strips): the time labels once, under the bottom row
            ImPlotAxisFlags xf = ImPlotAxisFlags_None;
            if (multi && f.link_x && static_cast<int>(a) / std::max(1, f.cols) < f.rows - 1) xf |= ImPlotAxisFlags_NoTickLabels;
            if (empty_time) xf |= ImPlotAxisFlags_NoTickLabels;
            ImPlot::SetupAxes(ax.xlabel.empty() ? nullptr : ax.xlabel.c_str(), ax.ylabel.empty() ? nullptr : ax.ylabel.c_str(), xf, yf);
            if (ax.time_x) {
              ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
              IntradayTicks(!(xf & ImPlotAxisFlags_NoTickLabels));
            }
            ImPlot::SetupAxisFormat(ImAxis_Y1, AxisNumberFormatter, reinterpret_cast<void*>(static_cast<intptr_t>(1000)));
            if (!ax.time_x) ImPlot::SetupAxisFormat(ImAxis_X1, AxisNumberFormatter, reinterpret_cast<void*>(static_cast<intptr_t>(1000000)));
            if (ax.log_y) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            // limits from Python apply once per set(); the user pans and zooms freely in between
            if (ax.has_xlim) ImPlot::SetupAxisLimits(ImAxis_X1, ax.x0, ax.x1, ImPlotCond_Always), ax.has_xlim = false;
            if (ax.has_ylim) ImPlot::SetupAxisLimits(ImAxis_Y1, ax.y0, ax.y1, ImPlotCond_Always), ax.has_ylim = false;
            if (ax.follow > 0 && !f.follow_paused) {
              double xmax = -std::numeric_limits<double>::infinity();
              double room = ax.follow * 0.02;  // the newest point isn't drawn on the frame
              for (const auto& item : ax.items) {
                const auto& x = ColOf(item, "x");
                if (!x.empty()) xmax = std::max(xmax, x.back());
                // candles and bars are drawn around their x: the forming bar (the one the trader watches) needs half a
                // bar more, or half of it is cut off and the y fit leaves its high out
                if ((item.kind == "candles" || item.kind == "bar") && x.size() >= 2) room = std::max(room, (x.back() - x[x.size() - 2]) * 0.7);
              }
              if (std::isfinite(xmax)) xmax += room;
              if (std::isfinite(xmax)) ImPlot::SetupAxisLimits(ImAxis_X1, xmax - ax.follow, xmax, ImPlotCond_Always);
              if (std::isfinite(xmax) && ax.yfit_stable && !ax.y_set && !was_dragging) StableY(ax, xmax - ax.follow, xmax);
            }
            // heatmap labels on the axes
            for (const auto& item : ax.items) {
              if (item.kind != "heatmap" || item.labels.empty() || item.has_extent) continue;  // placed in data units: the axes show them
              std::vector<const char*> xl, yl;
              std::vector<double> xt, yt;
              for (int c = 0; c < item.cols_n && static_cast<size_t>(c) < item.labels.size(); ++c) {
                xl.push_back(item.labels[static_cast<size_t>(c)].c_str());
                xt.push_back(c + 0.5);
              }
              for (int r = 0; r < item.rows && static_cast<size_t>(item.cols_n + r) < item.labels.size(); ++r) {
                yl.push_back(item.labels[static_cast<size_t>(item.cols_n + r)].c_str());
                yt.push_back(item.rows - r - 0.5);
              }
              if (!xl.empty()) ImPlot::SetupAxisTicks(ImAxis_X1, xt.data(), static_cast<int>(xt.size()), xl.data());
              if (!yl.empty()) ImPlot::SetupAxisTicks(ImAxis_Y1, yt.data(), static_cast<int>(yt.size()), yl.data());
            }
            const float pxw = ImPlot::GetPlotSize().x;
            const ImPlotRect drawn = ImPlot::GetPlotLimits();
            ax.view[0] = drawn.X.Min, ax.view[1] = drawn.X.Max, ax.view[2] = drawn.Y.Min, ax.view[3] = drawn.Y.Max;
            {
              const ImVec2 pp = ImPlot::GetPlotPos(), ps = ImPlot::GetPlotSize();
              ax.plot_rect[0] = pp.x, ax.plot_rect[1] = pp.y, ax.plot_rect[2] = pp.x + ps.x, ax.plot_rect[3] = pp.y + ps.y;
            }
            for (size_t i = 0; i < ax.items.size(); ++i) DrawItem(ax.items[i], std::to_string(f.id) + "_" + std::to_string(a) + "_" + ax.items[i].id, pxw);
            // the linked table row
            const double cx = ax.time_x && std::isfinite(cursor) ? SerialToEpoch(cursor) : cursor;
            ax.cursor_tag.clear();
            if (std::isfinite(cx)) {
              ImPlotSpec cs;
              cs.LineColor = ImVec4(0.85f, 0.2f, 0.2f, 0.9f);
              cs.LineWeight = 1.5f;
              cs.Flags = ImPlotItemFlags_NoLegend;
              ImPlot::PlotInfLines("##cursor", &cx, 1, cs);
              ax.cursor_tag = TagTimeX(cx, ax.time_x, ImVec4(0.85f, 0.2f, 0.2f, 1));
            }
            DrawSyncedCursor(f, ax, static_cast<int>(a));
            if (ltab && ImPlot::IsPlotHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16)
              SelectNearestRow(s, lsheet, *ltab, lcol, ax.time_x ? EpochToSerial(ImPlot::GetPlotMousePos().x) : ImPlot::GetPlotMousePos().x);
            PlotInteraction(f, ax, static_cast<int>(a));
            FlushTagsY();
            ImPlot::EndPlot();  // applies the fit (with the padding pushed above)
          }
          ImPlot::PopStyleVar();
        }
        ImPlot::GetStyle().UseLocalTime = local;
        ImPlot::GetStyle().Use24HourClock = h24;
        if (in_subplots) ImPlot::EndSubplots();
        DrawFigureMenu(f);
        f.fit = false;
      }
    }
    ImGui::End();
    PopPlotTheme(f.theme);
    ++it;
  }
}

}  // namespace yc::app
