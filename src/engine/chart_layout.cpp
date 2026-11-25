// SPDX-License-Identifier: Apache-2.0
#include "engine/chart_layout.h"

#include "core/i18n.h"
#include "engine/document.h"
#include "engine/geo.h"
#include "formula/datetime.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <string_view>

namespace yc::engine {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

constexpr uint32_t Abgr(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }
constexpr uint32_t kText = Abgr(0x59, 0x59, 0x59), kGrid = Abgr(0xD9, 0xD9, 0xD9), kAxis = Abgr(0xBF, 0xBF, 0xBF), kWhite = Abgr(255, 255, 255);

uint32_t Mix(uint32_t a, uint32_t b, float f) {
  auto ch = [&](int sh) {
    const float x = static_cast<float>((a >> sh) & 0xFF), y = static_cast<float>((b >> sh) & 0xFF);
    return static_cast<uint32_t>(std::lround(x + (y - x) * f)) << sh;
  };
  return ch(0) | ch(8) | ch(16) | ch(24);
}
uint32_t Alpha(uint32_t c, uint32_t a) { return (c & 0x00FFFFFFu) | (a << 24); }

// "%.Ng" with trailing zeros removed
std::string Short(double v, int digits = 4) {
  if (!std::isfinite(v)) return "#NUM!";
  char b[64];
  std::snprintf(b, sizeof b, "%.*g", digits, v);
  return b;
}

double Mean(const std::vector<double>& v) { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size()); }
double StdevS(const std::vector<double>& v) {
  if (v.size() < 2) return 0;
  const double m = Mean(v);
  double s = 0;
  for (double x : v) s += (x - m) * (x - m);
  return std::sqrt(s / static_cast<double>(v.size() - 1));
}

// ---- category tree (treemap, sunburst) ----
struct Node {
  std::string label;
  double value = 0, direct = 0;
  std::vector<int> kids;
};

std::vector<Node> BuildTree(const std::vector<std::vector<std::string>>& paths, const std::vector<double>& values) {
  std::vector<Node> t(1);
  for (size_t r = 0; r < values.size() && r < paths.size(); ++r) {
    const double v = values[r];
    if (!std::isfinite(v) || v <= 0 || paths[r].empty()) continue;
    int cur = 0;
    t[0].value += v;
    for (const std::string& part : paths[r]) {
      int next = -1;
      for (int k : t[static_cast<size_t>(cur)].kids) {
        if (t[static_cast<size_t>(k)].label == part) next = k;
      }
      if (next < 0) {
        next = static_cast<int>(t.size());
        t.push_back(Node{part, 0, 0, {}});
        t[static_cast<size_t>(cur)].kids.push_back(next);
      }
      cur = next;
      t[static_cast<size_t>(cur)].value += v;
    }
    t[static_cast<size_t>(cur)].direct += v;
  }
  // a row that ends at a node with children: its own share becomes a leaf of the same name
  const size_t n = t.size();
  for (size_t i = 1; i < n; ++i) {
    if (!t[i].kids.empty() && t[i].direct > 0) {
      const int k = static_cast<int>(t.size());
      t.push_back(Node{t[i].label, t[i].direct, t[i].direct, {}});
      t[i].kids.push_back(k);
    }
  }
  for (auto& nd : t) {
    std::stable_sort(nd.kids.begin(), nd.kids.end(), [&](int a, int b) { return t[static_cast<size_t>(a)].value > t[static_cast<size_t>(b)].value; });
  }
  return t;
}

struct Box {
  double x0, y0, x1, y1;
};

// squarified layout (Bruls, Huizing, van Wijk) of values sorted in descending order
std::vector<Box> Squarify(const std::vector<double>& vals, Box r) {
  std::vector<Box> out(vals.size(), Box{r.x0, r.y0, r.x0, r.y0});
  const double total = std::accumulate(vals.begin(), vals.end(), 0.0);
  const double area = (r.x1 - r.x0) * (r.y1 - r.y0);
  if (!(total > 0) || !(area > 0)) return out;
  std::vector<double> a(vals.size());
  for (size_t i = 0; i < vals.size(); ++i) a[i] = vals[i] / total * area;
  auto worst = [](double sum, double mx, double mn, double side) {
    const double s2 = sum * sum, w2 = side * side;
    return std::max(w2 * mx / s2, s2 / (w2 * mn));
  };
  size_t i = 0;
  while (i < a.size()) {
    const double w = r.x1 - r.x0, h = r.y1 - r.y0;
    const double side = std::min(w, h);
    if (!(side > 0)) break;
    double sum = a[i], mx = a[i], mn = a[i];
    double cur = worst(sum, mx, mn, side);
    size_t j = i + 1;
    while (j < a.size()) {
      const double s2 = sum + a[j], mx2 = std::max(mx, a[j]), mn2 = std::min(mn, a[j]);
      const double nw = worst(s2, mx2, mn2, side);
      if (nw > cur) break;
      sum = s2, mx = mx2, mn = mn2, cur = nw;
      ++j;
    }
    if (w >= h) {  // a column on the left
      const double cw = sum / h;
      double y = r.y0;
      for (size_t k = i; k < j; ++k) {
        const double hh = a[k] / cw;
        out[k] = Box{r.x0, y, r.x0 + cw, y + hh};
        y += hh;
      }
      r.x0 += cw;
    } else {  // a row at the top
      const double rh = sum / w;
      double x = r.x0;
      for (size_t k = i; k < j; ++k) {
        const double ww = a[k] / rh;
        out[k] = Box{x, r.y0, x + ww, r.y0 + rh};
        x += ww;
      }
      r.y0 += rh;
    }
    i = j;
  }
  return out;
}

void LayoutTree(const std::vector<Node>& t, int node, int level, int group, Box r, double banner, std::vector<TreeRect>* out) {
  const Node& nd = t[static_cast<size_t>(node)];
  std::vector<double> vals;
  for (int k : nd.kids) vals.push_back(t[static_cast<size_t>(k)].value);
  const std::vector<Box> boxes = Squarify(vals, r);
  for (size_t i = 0; i < nd.kids.size(); ++i) {
    const Node& kid = t[static_cast<size_t>(nd.kids[i])];
    const int g = level == 0 ? static_cast<int>(i) : group;
    const Box& b = boxes[i];
    TreeRect tr;
    tr.x0 = b.x0, tr.y0 = b.y0, tr.x1 = b.x1, tr.y1 = b.y1;
    tr.level = level;
    tr.group = g;
    tr.leaf = kid.kids.empty();
    tr.label = kid.label;
    tr.value = kid.value;
    out->push_back(tr);
    if (!tr.leaf) {
      Box inner = b;
      if (banner > 0 && b.y1 - b.y0 > banner * 2) inner.y0 += banner;
      LayoutTree(t, nd.kids[i], level + 1, g, inner, banner, out);
    }
  }
}

void LayoutSun(const std::vector<Node>& t, int node, int level, int group, double a0, double a1, int* levels, std::vector<SunSegment>* out) {
  const Node& nd = t[static_cast<size_t>(node)];
  if (!(nd.value > 0)) return;
  double a = a0;
  for (size_t i = 0; i < nd.kids.size(); ++i) {
    const Node& kid = t[static_cast<size_t>(nd.kids[i])];
    const double span = (a1 - a0) * kid.value / nd.value;
    SunSegment s;
    s.level = level;
    s.group = level == 0 ? static_cast<int>(i) : group;
    s.a0 = a;
    s.a1 = a + span;
    s.label = kid.label;
    s.value = kid.value;
    s.leaf = kid.kids.empty();
    out->push_back(s);
    *levels = std::max(*levels, level + 1);
    if (!s.leaf) LayoutSun(t, nd.kids[i], level + 1, s.group, s.a0, s.a1, levels, out);
    a += span;
  }
}

// least squares polynomial of the given degree; false when singular. from = 1: no constant term (a set intercept,
// already taken out of Y): c[0] is 0
bool PolyFit(const std::vector<double>& X, const std::vector<double>& Y, int deg, std::vector<double>* c, int from = 0) {
  const int m = deg + 1 - from;
  if (static_cast<int>(X.size()) < m || m < 1) return false;
  std::vector<double> A(static_cast<size_t>(m * (m + 1)), 0.0);
  std::vector<double> pw(static_cast<size_t>(2 * deg + 1));
  for (size_t i = 0; i < X.size(); ++i) {
    pw[0] = 1;
    for (int k = 1; k <= 2 * deg; ++k) pw[static_cast<size_t>(k)] = pw[static_cast<size_t>(k - 1)] * X[i];
    for (int r = 0; r < m; ++r) {
      for (int q = 0; q < m; ++q) A[static_cast<size_t>(r * (m + 1) + q)] += pw[static_cast<size_t>(r + q + 2 * from)];
      A[static_cast<size_t>(r * (m + 1) + m)] += pw[static_cast<size_t>(r + from)] * Y[i];
    }
  }
  auto at = [&](int r, int q) -> double& { return A[static_cast<size_t>(r * (m + 1) + q)]; };
  for (int col = 0; col < m; ++col) {
    int piv = col;
    for (int r = col + 1; r < m; ++r) {
      if (std::fabs(at(r, col)) > std::fabs(at(piv, col))) piv = r;
    }
    if (!(std::fabs(at(piv, col)) > 1e-300)) return false;
    if (piv != col) {
      for (int q = 0; q <= m; ++q) std::swap(at(piv, q), at(col, q));
    }
    for (int r = 0; r < m; ++r) {
      if (r == col) continue;
      const double f = at(r, col) / at(col, col);
      for (int q = col; q <= m; ++q) at(r, q) -= f * at(col, q);
    }
  }
  c->assign(static_cast<size_t>(m + from), 0.0);
  for (int r = 0; r < m; ++r) (*c)[static_cast<size_t>(r + from)] = at(r, m) / at(r, r);
  for (double v : *c) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

// ---- scene building ----

// model::ChartElem as stored in a primitive
enum : uint8_t {
  kElemChartArea = 1,
  kElemPlotArea = 2,
  kElemTitle = 3,
  kElemLegend = 4,
  kElemCatAxis = 5,
  kElemValAxis = 6,
  kElemSeries = 7,
  kElemCatAxisTitle = 10,
  kElemValAxisTitle = 11,
};

// a text color as ABGR from an element font's ARGB (0 = the scene's default)
uint32_t TextInk(uint32_t argb, uint32_t automatic = kText) {
  return argb ? Abgr((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF) : automatic;
}
uint32_t FillInk(uint32_t argb) { return Abgr((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF); }

struct S {
  ChartScene* out;
  float fpx;
  const MeasureText& measure;
  uint8_t elem = kElemPlotArea;  // the element the next primitives belong to
  int16_t series = -1;
  int32_t point = -1;         // the data point the next primitives draw (-1 = none)
  uint32_t axis_ink = kText;  // tick label color and size (Axis.TickLabels.Font)
  float axis_scale = 1;
  float Width(const std::string& t, float sc = 1) const { return measure(t, sc); }
  void Tag(uint8_t e, int s = -1) {
    elem = e;
    series = static_cast<int16_t>(s);
  }
  void Add(ChartPrim&& p) {
    p.elem = elem;
    p.series = series;
    p.point = point;
    out->prims.push_back(std::move(p));
  }
  void Rect(float x0, float y0, float x1, float y1, uint32_t fill, uint32_t stroke = 0, float w = 1) {
    ChartPrim p;
    p.kind = ChartPrim::Rect;
    p.pts = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
    p.fill = fill;
    p.stroke = stroke;
    p.width = w;
    Add(std::move(p));
  }
  void Line(std::vector<float> pts, uint32_t col, float w = 1) {
    ChartPrim p;
    p.kind = ChartPrim::Line;
    p.pts = std::move(pts);
    p.stroke = col;
    p.width = w;
    Add(std::move(p));
  }
  void Poly(std::vector<float> pts, uint32_t fill, uint32_t stroke = 0, float w = 1) {
    ChartPrim p;
    p.kind = ChartPrim::Poly;
    p.pts = std::move(pts);
    p.fill = fill;
    p.stroke = stroke;
    p.width = w;
    Add(std::move(p));
  }
  void Circle(float x, float y, float r, uint32_t fill, uint32_t stroke = 0) {
    ChartPrim p;
    p.kind = ChartPrim::Circle;
    p.pts = {x, y, r};
    p.fill = fill;
    p.stroke = stroke;
    Add(std::move(p));
  }
  void Text(const std::string& t, float x, float y, int ha, int va, uint32_t col = kText, float sc = 1, bool bold = false) {
    if (t.empty()) return;
    ChartPrim p;
    p.kind = ChartPrim::Text;
    p.pts = {x, y};
    p.text = t;
    p.halign = static_cast<int8_t>(ha);
    p.valign = static_cast<int8_t>(va);
    p.stroke = col;
    p.scale = sc;
    p.bold = bold;
    Add(std::move(p));
  }
  // an axis' tick label (its element, color and size)
  void Tick(uint8_t e, const std::string& t, float x, float y, int ha, int va) {
    const uint8_t was = elem;
    const int16_t ws = series;
    Tag(e);
    Text(t, x, y, ha, va, axis_ink, axis_scale);
    elem = was;
    series = ws;
  }
};

struct LegendItem {
  std::string name;
  uint32_t color;
  bool line = false;
};

// legend style (Legend.Position, Legend.Font)
struct LegendStyle {
  int pos = 0;  // 0 bottom, 1 right, 2 top, 3 left, 4 top right corner
  uint32_t ink = kText;
  float scale = 1;
};

// the legend at its side of [x0, x1] x [y0, y1]; shrinks that box by the room it takes
void Legend(S& s, const std::vector<LegendItem>& items, float* x0, float* y0, float* x1, float* y1, const LegendStyle& st) {
  if (items.empty()) return;
  s.Tag(kElemLegend);
  const float fh = s.fpx * st.scale, box = fh * 0.6f, gap = fh * 0.8f;
  if (st.pos == 1 || st.pos == 3 || st.pos == 4) {  // a column
    float w = 0;
    size_t n = 0;
    for (; n < items.size() && n < 40; ++n) w = std::max(w, box + 4 + s.Width(items[n].name, st.scale));
    w = std::min(w, (*x1 - *x0) * 0.45f);
    const float h = static_cast<float>(n) * fh;
    const float lx = st.pos == 3 ? *x0 : *x1 - w;
    float y = st.pos == 4 ? *y0 : (*y0 + *y1 - h) / 2;
    for (size_t i = 0; i < n; ++i, y += fh) {
      const LegendItem& it = items[i];
      if (it.line) s.Line({lx, y + fh / 2, lx + box, y + fh / 2}, it.color, 2);
      else s.Rect(lx, y + (fh - box) / 2, lx + box, y + (fh + box) / 2, it.color);
      s.Text(it.name, lx + box + 4, y + fh / 2, 0, 1, st.ink, st.scale);
    }
    if (st.pos == 3) *x0 += w + 6;
    else *x1 -= w + 6;
    s.Tag(kElemPlotArea);
    return;
  }
  std::vector<std::vector<size_t>> rows(1);
  std::vector<float> widths(1, 0);
  for (size_t i = 0; i < items.size() && i < 60; ++i) {
    const float w = box + 4 + s.Width(items[i].name, st.scale) + gap;
    if (!rows.back().empty() && widths.back() + w > *x1 - *x0) {
      if (rows.size() >= 3) break;
      rows.emplace_back();
      widths.push_back(0);
    }
    rows.back().push_back(i);
    widths.back() += w;
  }
  const float h = static_cast<float>(rows.size()) * fh;
  float y = st.pos == 2 ? *y0 : *y1 - h;
  for (size_t r = 0; r < rows.size(); ++r, y += fh) {
    float x = (*x0 + *x1 - (widths[r] - gap)) / 2;
    for (size_t i : rows[r]) {
      const LegendItem& it = items[i];
      if (it.line) s.Line({x, y + fh / 2, x + box, y + fh / 2}, it.color, 2);
      else s.Rect(x, y + (fh - box) / 2, x + box, y + (fh + box) / 2, it.color);
      s.Text(it.name, x + box + 4, y + fh / 2, 0, 1, st.ink, st.scale);
      x += box + 4 + s.Width(it.name, st.scale) + gap;
    }
  }
  if (st.pos == 2) *y0 += h + 4;
  else *y1 -= h + 4;
  s.Tag(kElemPlotArea);
}

// a value axis on the left, category slots along the bottom, an optional 0-100% axis on the right
struct Cart {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0, slot = 1;
  double lo = 0, hi = 1;
  float Y(double v) const { return y1 - static_cast<float>((v - lo) / (hi - lo)) * (y1 - y0); }
  float CX(size_t i) const { return x0 + slot * (static_cast<float>(i) + 0.5f); }
  float PY(double pct) const { return y1 - static_cast<float>(pct) * (y1 - y0); }  // right axis, 0..1
};

Cart MakeCart(S& s, const model::ChartObj& ch, float L, float T, float R, float B, double lo, double hi, const std::vector<std::string>& cats, size_t n,
              const std::string& fmt, bool pct_right) {
  Cart c;
  const model::ChartAxisFormat& va = ch.val_axis;
  if (!va.min_auto) lo = va.min;
  if (!va.max_auto) hi = va.max;
  if (!(hi > lo)) hi = lo + 1;
  const double step = va.major > 0 && (hi - lo) / va.major <= 60 ? va.major : NiceStep(hi - lo);
  if (va.min_auto) lo = std::floor(lo / step + 1e-9) * step;
  if (va.max_auto) hi = std::ceil(hi / step - 1e-9) * step;
  if (!(hi > lo)) hi = lo + step;
  c.lo = lo, c.hi = hi;
  std::vector<std::pair<double, std::string>> ticks;
  float lw = 0;
  for (int k = 0; k <= 60; ++k) {
    const double v = lo + step * k;
    if (v > hi + step * 1e-6) break;
    ticks.emplace_back(v, FormatChartNumber(std::fabs(v) < step * 1e-9 ? 0.0 : v, ch.val_axis.number_format.empty() ? fmt : ch.val_axis.number_format));
    lw = std::max(lw, s.Width(ticks.back().second));
  }
  const float pw = pct_right ? s.Width("100%") + 6 : 4;
  c.x0 = L + lw + 6;
  c.x1 = R - pw;
  c.y0 = T + s.fpx * 0.5f;
  c.y1 = B - (n > 0 ? s.fpx + 4 : 0);
  if (c.x1 <= c.x0 + 4 || c.y1 <= c.y0 + 4) {
    c.x1 = c.x0 + 4;
    c.y1 = c.y0 + 4;
  }
  if (ch.plot_fill) s.Rect(c.x0, c.y0, c.x1, c.y1, FillInk(ch.plot_fill));
  for (const auto& [v, t] : ticks) {
    if (ch.val_axis.gridlines) s.Line({c.x0, c.Y(v), c.x1, c.Y(v)}, kGrid);
    if (ch.val_axis.visible) s.Tick(kElemValAxis, t, c.x0 - 4, c.Y(v), 2, 1);
  }
  s.Line({c.x0, c.Y(std::clamp(0.0, lo, hi)), c.x1, c.Y(std::clamp(0.0, lo, hi))}, kAxis);
  if (pct_right) {
    for (int k = 0; k <= 5; ++k) s.Text(std::to_string(k * 20) + "%", c.x1 + 4, c.PY(k * 0.2), 0, 1);
  }
  c.slot = (c.x1 - c.x0) / static_cast<float>(std::max<size_t>(1, n));
  float maxw = 0;
  for (size_t i = 0; i < n; ++i) maxw = std::max(maxw, s.Width(i < cats.size() ? cats[i] : std::to_string(i + 1)));
  const size_t every = std::max<size_t>(1, static_cast<size_t>(std::ceil((maxw + 8) / std::max(1.0f, c.slot))));
  for (size_t i = 0; i < n && ch.cat_axis.visible; i += every) s.Tick(kElemCatAxis, i < cats.size() ? cats[i] : std::to_string(i + 1), c.CX(i), c.y1 + 3, 1, 0);
  return c;
}

const ChartSeriesValues* First(const ChartValues& v) {
  for (const auto& se : v.series) {
    if (se.N() > 0) return &se;
  }
  return nullptr;
}

std::vector<double> YOf(const ChartSeriesValues& se) {
  std::vector<double> out(static_cast<size_t>(std::max(0, se.N())));
  for (size_t i = 0; i < out.size(); ++i) out[i] = se.Y()[i];
  return out;
}

const model::ChartSeries* Def(const model::ChartObj& ch, size_t i) { return i < ch.series.size() ? &ch.series[i] : nullptr; }

std::string Cat(const ChartValues& v, size_t i) { return i < v.cats.size() ? v.cats[i] : std::to_string(i + 1); }

// ---- the chart kinds ----

// a chart that draws a mark a category (3D columns, bars, lines and areas, a funnel, a surface) over more categories than
// it can show apart is drawn from every stride-th one: over whole columns that was a box or a bar for each of a million
// rows every frame (scale_sweep: Insert 3D Line took 22 s). The kinds whose result needs every point (a histogram's
// bins, a box plot, a pie, a waterfall's running total) keep them all.
constexpr size_t kMaxDrawnCategories = 2000;
size_t MostPoints(const ChartValues& v) {
  size_t n = v.cats.size();
  for (const auto& se : v.series) n = std::max(n, static_cast<size_t>(std::max(0, se.N())));
  return n;
}
ChartValues Thinned(const ChartValues& v, size_t stride) {
  // built from every stride-th element, never a copy of the whole first (a million labels a frame)
  auto pick = [stride](const auto& vec) {
    std::remove_cvref_t<decltype(vec)> out;
    out.reserve(vec.size() / stride + 1);
    for (size_t i = 0; i < vec.size(); i += stride) out.push_back(vec[i]);
    return out;
  };
  ChartValues t;
  t.cats = pick(v.cats);
  t.paths = pick(v.paths);
  t.cat_vals = pick(v.cat_vals);
  t.cat_dates = v.cat_dates;
  t.cat_fmt = v.cat_fmt;
  t.title = v.title;
  t.cat_title = v.cat_title;
  t.val_title = v.val_title;
  t.val2_title = v.val2_title;
  for (const auto& se : v.series) {
    ChartSeriesValues o;
    o.name = se.name;
    o.fmt = se.fmt;
    o.color = se.color;
    o.axis = se.axis;
    o.numeric_x = se.numeric_x;
    const int n = se.N();
    const double* x = se.X();
    const double* y = se.Y();
    for (int i = 0; i < n; i += static_cast<int>(stride)) {
      o.x.push_back(se.numeric_x ? x[i] : static_cast<double>(o.x.size() + 1));
      o.y.push_back(y[i]);
    }
    o.err_plus = pick(se.err_plus);
    o.err_minus = pick(se.err_minus);
    o.xerr_plus = pick(se.xerr_plus);
    o.xerr_minus = pick(se.xerr_minus);
    o.sizes = pick(se.sizes);
    o.texts = pick(se.texts);
    o.label_texts = pick(se.label_texts);
    t.series.push_back(std::move(o));
  }
  t.points = MostPoints(t);
  for (const auto& [key, text] : v.point_labels) {
    const uint64_t point = key & 0xFFFFFFFFull;
    if (point % stride == 0) t.point_labels.emplace((key & ~0xFFFFFFFFull) | (point / stride), text);
  }
  return t;
}

void SceneRadar(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  size_t n = 0;
  double lo = 0, hi = -1e300;
  for (const auto& se : v.series) {
    n = std::max(n, static_cast<size_t>(std::max(0, se.N())));
    for (int i = 0; i < se.N(); ++i) {
      const double y = se.Y()[i];
      if (std::isfinite(y)) lo = std::min(lo, y), hi = std::max(hi, y);
    }
  }
  if (n == 0) return;
  if (!ch.val_axis.min_auto) lo = ch.val_axis.min;
  if (!ch.val_axis.max_auto) hi = ch.val_axis.max;
  if (!(hi > lo)) hi = lo + 1;
  const double step = ch.val_axis.major > 0 && (hi - lo) / ch.val_axis.major <= 60 ? ch.val_axis.major : NiceStep(hi - lo);
  if (ch.val_axis.min_auto) lo = std::floor(lo / step + 1e-9) * step;
  if (ch.val_axis.max_auto) hi = std::ceil(hi / step - 1e-9) * step;
  if (!(hi > lo)) hi = lo + step;
  // more categories than can be told apart around the circle (a series over whole columns: a million) are drawn every
  // stride-th one, spokes, labels, rings and points alike: each was a spoke, a measured label and a ring vertex every
  // frame, and the frame took seconds (scale_sweep)
  const size_t stride = n > 720 ? (n + 359) / 360 : 1;
  float lw = 0;
  for (size_t i = 0; i < n; i += stride) lw = std::min(std::max(lw, s.Width(Cat(v, i))), 120.0f);
  const float cx = (L + R) / 2, cy = (T + B) / 2;
  const float rad = std::max(4.0f, std::min((R - L) / 2 - lw - 6, (B - T) / 2 - s.fpx - 4));
  auto pt = [&](size_t i, double r) {
    const double a = 2 * kPi * static_cast<double>(i) / static_cast<double>(n);
    return std::pair<float, float>{cx + static_cast<float>(r * std::sin(a)), cy - static_cast<float>(r * std::cos(a))};
  };
  auto rr = [&](double val) { return std::isfinite(val) ? std::clamp((val - lo) / (hi - lo), 0.0, 1.0) * rad : 0.0; };
  // grid rings and spokes
  const std::string& fmt = v.series.empty() ? std::string() : v.series[0].fmt;
  for (int k = 0; k <= 60; ++k) {
    const double val = lo + step * k;
    if (val > hi + step * 1e-6) break;
    std::vector<float> ring;
    for (size_t i = 0; i <= n; i += stride) {
      const auto [x, y] = pt(i % n, rr(val));
      ring.push_back(x);
      ring.push_back(y);
    }
    if (k > 0) s.Line(ring, kGrid);
    s.Text(FormatChartNumber(std::fabs(val) < step * 1e-9 ? 0.0 : val, fmt), cx - 3, cy - static_cast<float>(rr(val)), 2, 1, kText, 0.85f);
  }
  for (size_t i = 0; i < n; i += stride) {
    const auto [x, y] = pt(i, rad);
    s.Line({cx, cy, x, y}, kGrid);
    const double a = 2 * kPi * static_cast<double>(i) / static_cast<double>(n);
    const double sn = std::sin(a), cs = std::cos(a);
    const int ha = sn > 0.2 ? 0 : (sn < -0.2 ? 2 : 1), va = cs > 0.2 ? 2 : (cs < -0.2 ? 0 : 1);
    s.Text(Cat(v, i), x + static_cast<float>(sn * 5), y - static_cast<float>(cs * 5), ha, va);
  }
  const bool filled = ch.type == model::ChartType::RadarFilled, markers = ch.type == model::ChartType::RadarMarkers;
  for (size_t si = 0; si < v.series.size(); ++si) {
    const auto& se = v.series[si];
    const int m = se.N();
    if (m <= 0) continue;
    const uint32_t col = SeriesColor(ch, v, si);
    s.Tag(kElemSeries, static_cast<int>(si));
    std::vector<float> poly;
    std::vector<int> at;  // the point each vertex is
    for (int i = 0; i < m && static_cast<size_t>(i) < n; i += static_cast<int>(stride)) {
      const auto [x, y] = pt(static_cast<size_t>(i), rr(se.Y()[i]));
      poly.push_back(x);
      poly.push_back(y);
      at.push_back(i);
    }
    if (filled) {
      s.Poly(poly, Alpha(col, 150), col, 1.5f);
    } else {
      std::vector<float> closed = poly;
      closed.push_back(poly[0]);
      closed.push_back(poly[1]);
      s.Line(closed, col, 2);
    }
    if (markers) {
      for (size_t k = 0; k + 1 < poly.size(); k += 2) s.Circle(poly[k], poly[k + 1], 3, col);
    }
    if (const auto* d = Def(ch, si); d && d->data_labels) {
      for (size_t k = 0; k < at.size(); ++k) {
        const double y = se.Y()[at[k]];
        if (std::isfinite(y)) s.Text(FormatChartNumber(y, se.fmt), poly[2 * k], poly[2 * k + 1] - 4, 1, 2);
      }
    }
  }
}

void SceneWaterfall(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  const model::ChartSeries* d = Def(ch, si);
  std::vector<double> ys = YOf(*se);
  for (double& y : ys) y = std::isfinite(y) ? y : 0;
  const auto bars = Waterfall(ys, d ? d->totals : std::vector<int>{});
  double lo = 0, hi = 0;
  for (const auto& b : bars) lo = std::min({lo, b.start, b.end}), hi = std::max({hi, b.start, b.end});
  const Cart c = MakeCart(s, ch, L, T, R, B, lo, hi, v.cats, bars.size(), se->fmt, false);
  const uint32_t cols[3] = {ChartPalette(0), ChartPalette(1), ChartPalette(2)};
  // Gap Width: the space between columns in % of a column's width (Excel's waterfall default is 50%)
  const float gap = static_cast<float>(std::clamp(ch.gap_width == 219 ? 50 : ch.gap_width, 0, 500)) / 100.0f;
  const float bw = c.slot / (1.0f + gap);
  s.Tag(kElemSeries, static_cast<int>(si));
  for (size_t i = 0; i < bars.size(); ++i) {
    const auto& b = bars[i];
    const float x = c.CX(i);
    uint32_t fill = cols[b.kind];
    if (d) {
      for (const auto& pf : d->points) {
        if (pf.index == static_cast<int>(i) && pf.fill) fill = Abgr(pf.fill & 0xFF, (pf.fill >> 8) & 0xFF, (pf.fill >> 16) & 0xFF);
      }
    }
    s.point = static_cast<int32_t>(i);
    s.Rect(x - bw / 2, c.Y(std::max(b.start, b.end)), x + bw / 2, c.Y(std::min(b.start, b.end)), fill);
    s.point = -1;
    if (ch.connectors && i + 1 < bars.size()) s.Line({x + bw / 2, c.Y(b.end), c.CX(i + 1) - bw / 2, c.Y(b.end)}, Abgr(0x80, 0x80, 0x80), 1);
    if (d && d->data_labels) {
      const double val = b.kind == 2 ? b.end : b.end - b.start;
      if (val >= 0) s.Text(FormatChartNumber(val, se->fmt), x, c.Y(std::max(b.start, b.end)) - 2, 1, 2);
      else s.Text(FormatChartNumber(val, se->fmt), x, c.Y(std::min(b.start, b.end)) + 2, 1, 0);
    }
  }
}

void SceneHistogram(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  const HistBins h = HistogramOf(se->Finite(), ch);
  if (h.counts.empty()) return;
  const double hi = *std::max_element(h.counts.begin(), h.counts.end());
  const Cart c = MakeCart(s, ch, L, T, R, B, 0, std::max(1.0, hi), h.labels, h.counts.size(), "General", false);
  const uint32_t col = SeriesColor(ch, v, si);
  const model::ChartSeries* d = Def(ch, si);
  s.Tag(kElemSeries, static_cast<int>(si));
  for (size_t i = 0; i < h.counts.size(); ++i) {
    const float x0 = c.x0 + c.slot * static_cast<float>(i), x1 = x0 + c.slot;
    s.Rect(x0 + 0.5f, c.Y(h.counts[i]), x1 - 0.5f, c.Y(0), col);
    if (d && d->data_labels) s.Text(Short(h.counts[i], 10), (x0 + x1) / 2, c.Y(h.counts[i]) - 2, 1, 2);
  }
}

void ScenePareto(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  std::vector<std::pair<std::string, double>> items;
  if (v.cats.empty()) {  // numbers only: histogram bins, sorted
    const HistBins h = HistogramOf(se->Finite(), ch);
    for (size_t i = 0; i < h.counts.size(); ++i) items.emplace_back(h.labels[i], h.counts[i]);
  } else {  // categories: the same category's values add up
    std::map<std::string, size_t> at;
    for (int i = 0; i < se->N(); ++i) {
      const double y = se->Y()[i];
      if (!std::isfinite(y)) continue;
      const std::string k = Cat(v, static_cast<size_t>(i));
      auto it = at.find(k);
      if (it == at.end()) {
        at[k] = items.size();
        items.emplace_back(k, y);
      } else {
        items[it->second].second += y;
      }
    }
  }
  std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  if (items.empty()) return;
  double total = 0, hi = 0;
  for (const auto& it : items) total += std::max(0.0, it.second), hi = std::max(hi, it.second);
  std::vector<std::string> cats;
  for (const auto& it : items) cats.push_back(it.first);
  const Cart c = MakeCart(s, ch, L, T, R, B, 0, std::max(hi, 1e-12), cats, items.size(), v.cats.empty() ? "General" : se->fmt, true);
  const uint32_t col = SeriesColor(ch, v, si), line = ChartPalette(1);
  const model::ChartSeries* d = Def(ch, si);
  s.Tag(kElemSeries, static_cast<int>(si));
  std::vector<float> cum;
  double acc = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    const float x0 = c.x0 + c.slot * static_cast<float>(i), x1 = x0 + c.slot;
    s.Rect(x0 + 0.5f, c.Y(items[i].second), x1 - 0.5f, c.Y(0), col);
    if (d && d->data_labels) s.Text(FormatChartNumber(items[i].second, v.cats.empty() ? "General" : se->fmt), (x0 + x1) / 2, c.Y(items[i].second) - 2, 1, 2);
    acc += std::max(0.0, items[i].second);
    cum.push_back(c.CX(i));
    cum.push_back(c.PY(total > 0 ? acc / total : 0));
  }
  s.Line(cum, line, 2);
  for (size_t k = 0; k + 1 < cum.size(); k += 2) s.Circle(cum[k], cum[k + 1], 2.5f, line);
}

void SceneBox(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  // groups: the distinct categories in order of appearance (one group without categories)
  std::vector<std::string> groups;
  std::map<std::string, size_t> gi;
  std::vector<size_t> point_group;
  size_t n = 0;
  for (const auto& se : v.series) n = std::max(n, static_cast<size_t>(std::max(0, se.N())));
  for (size_t i = 0; i < n; ++i) {
    const std::string k = v.cats.empty() ? std::string() : Cat(v, i);
    auto it = gi.find(k);
    if (it == gi.end()) {
      gi[k] = groups.size();
      point_group.push_back(groups.size());
      groups.push_back(k);
    } else {
      point_group.push_back(it->second);
    }
  }
  if (groups.empty()) return;
  const size_t ns = v.series.size();
  std::vector<BoxStats> st(groups.size() * ns);
  std::vector<std::vector<double>> raw(groups.size() * ns);  // each box's values (Show inner points)
  double lo = 1e300, hi = -1e300;
  for (size_t si = 0; si < ns; ++si) {
    std::vector<std::vector<double>> per(groups.size());
    const auto& se = v.series[si];
    for (int i = 0; i < se.N(); ++i) {
      if (std::isfinite(se.Y()[i])) per[point_group[static_cast<size_t>(i)]].push_back(se.Y()[i]);
    }
    for (size_t g = 0; g < groups.size(); ++g) {
      raw[g * ns + si] = per[g];
      const model::ChartSeries* sd = Def(ch, si);
      BoxStats b = BoxWhisker(per[g], sd && sd->inclusive_median);
      if (b.ok) {
        lo = std::min(lo, b.lo), hi = std::max(hi, b.hi);
        for (double o : b.outliers) lo = std::min(lo, o), hi = std::max(hi, o);
      }
      st[g * ns + si] = std::move(b);
    }
  }
  if (lo > hi) return;
  const bool labels = !(groups.size() == 1 && groups[0].empty());
  const Cart c = MakeCart(s, ch, L, T, R, B, lo, hi, labels ? groups : std::vector<std::string>{std::string()}, groups.size(), v.series.empty() ? std::string() : v.series[0].fmt, false);
  const float group_w = c.slot * 0.7f, bw = group_w / static_cast<float>(std::max<size_t>(1, ns));
  for (size_t g = 0; g < groups.size(); ++g) {
    for (size_t si = 0; si < ns; ++si) {
      const BoxStats& b = st[g * ns + si];
      if (!b.ok) continue;
      s.Tag(kElemSeries, static_cast<int>(si));
      const uint32_t col = SeriesColor(ch, v, si);
      const float xc = c.CX(g) - group_w / 2 + bw * (static_cast<float>(si) + 0.5f), half = bw * 0.4f;
      s.Line({xc, c.Y(b.q3), xc, c.Y(b.hi)}, col, 1);
      s.Line({xc, c.Y(b.q1), xc, c.Y(b.lo)}, col, 1);
      s.Line({xc - half / 2, c.Y(b.hi), xc + half / 2, c.Y(b.hi)}, col, 1);
      s.Line({xc - half / 2, c.Y(b.lo), xc + half / 2, c.Y(b.lo)}, col, 1);
      s.Rect(xc - half, c.Y(b.q3), xc + half, c.Y(b.q1), Mix(col, kWhite, 0.35f), col, 1);
      s.Line({xc - half, c.Y(b.median), xc + half, c.Y(b.median)}, Mix(col, Abgr(0, 0, 0), 0.4f), 2);
      // Format Data Series: inner points, outlier points, mean markers (Excel's defaults: outliers and markers)
      const model::ChartSeries* def = Def(ch, si);
      const int flags = def ? def->box_flags : 6;
      const float my = c.Y(b.mean), mk = std::min(4.0f, half * 0.5f);
      if (flags & 4) {
        s.Line({xc - mk, my - mk, xc + mk, my + mk}, Abgr(0x40, 0x40, 0x40), 1.5f);
        s.Line({xc - mk, my + mk, xc + mk, my - mk}, Abgr(0x40, 0x40, 0x40), 1.5f);
      }
      if (flags & 2) {
        for (double o : b.outliers) s.Circle(xc, c.Y(o), 2.5f, 0, col);
      }
      if (flags & 1) {
        for (double p : raw[g * ns + si]) {
          if (p >= b.lo && p <= b.hi) s.Circle(xc, c.Y(p), 2.0f, kWhite, col);
        }
      }
      // Show mean line: a line joining the means of the series' boxes
      if ((flags & 8) && g + 1 < groups.size() && st[(g + 1) * ns + si].ok) {
        const float xn = c.CX(g + 1) - group_w / 2 + bw * (static_cast<float>(si) + 0.5f);
        s.Line({xc, my, xn, c.Y(st[(g + 1) * ns + si].mean)}, col, 1.5f);
      }
      if (const auto* d = Def(ch, si); d && d->data_labels) {
        const std::string& f = v.series[si].fmt;
        s.Text(FormatChartNumber(b.median, f), xc + half + 2, c.Y(b.median), 0, 1, kText, 0.85f);
        s.Text(FormatChartNumber(b.q1, f), xc + half + 2, c.Y(b.q1), 0, 1, kText, 0.85f);
        s.Text(FormatChartNumber(b.q3, f), xc + half + 2, c.Y(b.q3), 0, 1, kText, 0.85f);
      }
    }
  }
}

std::vector<std::vector<std::string>> PathsOf(const ChartValues& v, size_t n) {
  if (!v.paths.empty()) return v.paths;
  std::vector<std::vector<std::string>> p;
  for (size_t i = 0; i < n; ++i) p.push_back({Cat(v, i)});
  return p;
}

void SceneTreemap(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B, std::vector<LegendItem>* legend) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const std::vector<double> ys = YOf(*se);
  const auto paths = PathsOf(v, ys.size());
  const model::ChartSeries* d = Def(ch, static_cast<size_t>(se - v.series.data()));
  // Label Options: a banner above each parent (Excel's default), the parent's name over its rectangles, or none
  const int parent_labels = d ? d->parent_labels : 1;
  const float banner = parent_labels == 1 ? s.fpx + 4 : 0.0f;
  const auto rects = TreemapLayout(paths, ys, L, T, R, B, banner);
  std::vector<const TreeRect*> parents;
  s.Tag(kElemSeries, static_cast<int>(se - v.series.data()));
  for (const auto& r : rects) {
    const uint32_t base = ChartPalette(static_cast<size_t>(r.group));
    const float x0 = static_cast<float>(r.x0), y0 = static_cast<float>(r.y0), x1 = static_cast<float>(r.x1), y1 = static_cast<float>(r.y1);
    if (r.level == 0 && legend) legend->push_back(LegendItem{r.label, base});
    if (r.leaf) {
      s.Rect(x0, y0, x1, y1, base, kWhite, 1);
      const bool fit = x1 - x0 > 12 && y1 - y0 > s.fpx + 4;
      if (fit && s.Width(r.label) < x1 - x0 - 6) {
        s.Text(r.label, x0 + 3, y0 + 2, 0, 0, kWhite);
        if (d && d->data_labels && y1 - y0 > 2 * s.fpx + 6 && s.Width(FormatChartNumber(r.value, se->fmt)) < x1 - x0 - 6) {
          s.Text(FormatChartNumber(r.value, se->fmt), x0 + 3, y0 + 2 + s.fpx, 0, 0, kWhite);
        }
      }
    } else {
      s.Rect(x0, y0, x1, y1, Mix(base, Abgr(0, 0, 0), 0.18f), kWhite, 1);
      if (parent_labels == 1 && y1 - y0 > banner * 2 && s.Width(r.label, 1) < x1 - x0 - 6) s.Text(r.label, x0 + 3, y0 + 2, 0, 0, kWhite, 1, true);
      if (parent_labels == 2) parents.push_back(&r);
    }
  }
  // Overlapping: the parent's name large over the middle of its rectangles, on top of the children
  for (const TreeRect* r : parents) {
    const float x0 = static_cast<float>(r->x0), y0 = static_cast<float>(r->y0), x1 = static_cast<float>(r->x1), y1 = static_cast<float>(r->y1);
    if (s.Width(r->label, 1.4f) < x1 - x0 - 6 && y1 - y0 > s.fpx * 2) s.Text(r->label, (x0 + x1) / 2, (y0 + y1) / 2, 1, 1, kWhite, 1.4f, true);
  }
}

void SceneSunburst(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B, std::vector<LegendItem>* legend) {
  (void)ch;
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const std::vector<double> ys = YOf(*se);
  int levels = 0;
  const auto segs = SunburstLayout(PathsOf(v, ys.size()), ys, &levels);
  if (segs.empty() || levels <= 0) return;
  const float cx = (L + R) / 2, cy = (T + B) / 2, rad = std::max(4.0f, std::min(R - L, B - T) / 2 - 2), hole = rad * 0.12f;
  const float ring = (rad - hole) / static_cast<float>(levels);
  s.Tag(kElemSeries, static_cast<int>(se - v.series.data()));
  for (const auto& g : segs) {
    const uint32_t base = ChartPalette(static_cast<size_t>(g.group));
    if (g.level == 0 && legend) legend->push_back(LegendItem{g.label, base});
    const float r0 = hole + ring * static_cast<float>(g.level), r1 = g.leaf ? rad : r0 + ring;
    const int steps = std::max(2, static_cast<int>((g.a1 - g.a0) / (2 * kPi) * 120));
    std::vector<float> pts;
    for (int k = 0; k <= steps; ++k) {
      const double a = g.a0 + (g.a1 - g.a0) * k / steps;
      pts.push_back(cx + r1 * static_cast<float>(std::sin(a)));
      pts.push_back(cy - r1 * static_cast<float>(std::cos(a)));
    }
    for (int k = steps; k >= 0; --k) {
      const double a = g.a0 + (g.a1 - g.a0) * k / steps;
      pts.push_back(cx + r0 * static_cast<float>(std::sin(a)));
      pts.push_back(cy - r0 * static_cast<float>(std::cos(a)));
    }
    s.Poly(std::move(pts), Mix(base, kWhite, 0.12f * static_cast<float>(g.level)), kWhite, 1);
    const double am = (g.a0 + g.a1) / 2;
    const float rm = (r0 + r1) / 2;
    const float arc = static_cast<float>((g.a1 - g.a0) * rm);
    const float tw = s.Width(g.label, 0.9f);
    if (r1 - r0 > s.fpx && std::min(arc, r1 - r0 + 40.0f) > tw + 4 && arc > s.fpx) {
      s.Text(g.label, cx + rm * static_cast<float>(std::sin(am)), cy - rm * static_cast<float>(std::cos(am)), 1, 1, kWhite, 0.9f);
    }
  }
}

void SceneFunnel(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  const int n = se->N();
  if (n <= 0) return;
  double mx = 0;
  for (int i = 0; i < n; ++i) {
    if (std::isfinite(se->Y()[i])) mx = std::max(mx, std::fabs(se->Y()[i]));
  }
  if (!(mx > 0)) mx = 1;
  float lw = 0;
  if (!v.cats.empty()) {
    for (int i = 0; i < n; ++i) lw = std::max(lw, s.Width(Cat(v, static_cast<size_t>(i))));
  }
  const float x0 = L + (lw > 0 ? lw + 8 : 0), x1 = R - 4, xc = (x0 + x1) / 2;
  const float row = (B - T) / static_cast<float>(n);
  const uint32_t col = SeriesColor(ch, v, si);
  const model::ChartSeries* d = Def(ch, si);
  for (int i = 0; i < n; ++i) {
    const double y = se->Y()[i];
    const float yc = T + row * (static_cast<float>(i) + 0.5f), hh = row * 0.42f;
    if (!v.cats.empty() && ch.cat_axis.visible) s.Tick(kElemCatAxis, Cat(v, static_cast<size_t>(i)), x0 - 6, yc, 2, 1);
    s.Tag(kElemSeries, static_cast<int>(si));
    if (!std::isfinite(y)) continue;
    const float half = static_cast<float>(std::fabs(y) / mx) * (x1 - x0) / 2;
    s.Rect(xc - half, yc - hh, xc + half, yc + hh, col);
    if (!d || d->data_labels) {
      const std::string t = FormatChartNumber(y, se->fmt);
      if (s.Width(t) + 6 < half * 2) s.Text(t, xc, yc, 1, 1, kWhite);
      else s.Text(t, xc + half + 4, yc, 0, 1);
    }
  }
}


// ---- pie of pie and bar of pie ----

}  // namespace

std::vector<char> SecondPlotPoints(const model::ChartObj& ch, const double* ys, int n) {
  std::vector<char> second(static_cast<size_t>(std::max(0, n)), 0);
  if (n <= 1) return second;
  double total = 0;
  for (int k = 0; k < n; ++k) {
    if (std::isfinite(ys[k])) total += std::fabs(ys[k]);
  }
  switch (ch.split_type) {
    case 2:  // by value: the points less than the value
    case 3: {  // by percentage: the points less than that share of the total
      const double lim = ch.split_type == 2 ? ch.split_value : (ch.split_value > 0 ? ch.split_value : 10) / 100.0 * total;
      for (int k = 0; k < n; ++k) {
        const double y = std::isfinite(ys[k]) ? std::fabs(ys[k]) : 0.0;
        second[static_cast<size_t>(k)] = y < lim;
      }
      break;
    }
    case 4:  // custom: the points moved there one by one
      for (int k : ch.second_points) {
        if (k >= 0 && k < n) second[static_cast<size_t>(k)] = 1;
      }
      break;
    default: {  // by position: the last n points (Excel's default: 3, never all of them)
      const int cnt = std::clamp(ch.split_value > 0 ? static_cast<int>(std::lround(ch.split_value)) : 3, 1, n - 1);
      for (int k = n - cnt; k < n; ++k) second[static_cast<size_t>(k)] = 1;
      break;
    }
  }
  return second;
}

namespace {

void ScenePieOfPie(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  const int n = std::min(se->N(), 255);
  if (n <= 0) return;
  const std::vector<char> second = SecondPlotPoints(ch, se->Y(), n);
  double total = 0, other = 0;
  for (int k = 0; k < n; ++k) {
    const double y = std::isfinite(se->Y()[k]) ? std::fabs(se->Y()[k]) : 0.0;
    total += y;
    if (second[static_cast<size_t>(k)]) other += y;
  }
  if (!(total > 0)) return;
  const bool bar = ch.type == model::ChartType::BarOfPie;
  const double kk = std::clamp(ch.second_size, 5, 200) / 100.0, gap = std::clamp(ch.pie_gap, 0, 500) / 100.0;
  // the first pie (radius r), the gap, and the second plot (radius r * kk, a bar r * kk * 0.6 wide)
  const double W = R - L, H = B - T;
  const double wfac = 2 + gap * 0.5 + (bar ? kk * 0.6 : 2 * kk);
  const float r = static_cast<float>(std::max(4.0, std::min(W / wfac, H / 2) * 0.92));
  const float r2 = static_cast<float>(r * kk);
  const float total_w = static_cast<float>(r * wfac);
  const float cx = L + static_cast<float>((W - total_w) / 2) + r, cy = (T + B) / 2;
  const float cx2 = cx + r + static_cast<float>(gap * 0.5 * r) + (bar ? r2 * 0.3f : r2);
  const model::ChartSeries* d = Def(ch, si);
  const bool labels = d && d->data_labels;
  auto col_of = [&](int k) {
    if (d) {
      for (const auto& pf : d->points) {
        if (pf.index == k && pf.fill) return Abgr(pf.fill & 0xFF, (pf.fill >> 8) & 0xFF, (pf.fill >> 16) & 0xFF);
      }
    }
    return ChartPalette(static_cast<size_t>(k));
  };
  auto label = [&](int k, double y, float x, float yy, uint32_t ink) {
    if (!labels) return;
    std::string t;
    if (d->label_show & 2) t = k < n ? Cat(v, static_cast<size_t>(k)) : std::string(TR("Other"));
    if (d->label_show & 4) t += (t.empty() ? "" : d->label_sep) + FormatChartNumber(y, se->fmt);
    if (d->label_show & 8) t += (t.empty() ? "" : d->label_sep) + FormatChartNumber(y / total, "0%");
    if (t.empty()) t = FormatChartNumber(y, se->fmt);
    s.Text(t, x, yy, 1, 1, ink, 0.85f);
  };
  auto wedge = [&](float ox, float oy, float rad, double a0, double a1, uint32_t fill) {
    std::vector<float> pts = {ox, oy};
    const int steps = std::max(2, static_cast<int>((a1 - a0) / (2 * kPi) * 120));
    for (int q = 0; q <= steps; ++q) {
      const double a = a0 + (a1 - a0) * q / steps;
      pts.push_back(ox + rad * static_cast<float>(std::sin(a)));
      pts.push_back(oy - rad * static_cast<float>(std::cos(a)));
    }
    s.Poly(std::move(pts), fill, kWhite, 1.5f);
  };
  // the first pie: "Other" (the second plot's total) faces the second plot, the other points follow it clockwise
  const double o_sweep = other / total * 2 * kPi;
  double a = kPi / 2 + o_sweep / 2;
  for (int k = 0; k < n; ++k) {
    if (second[static_cast<size_t>(k)]) continue;
    const double y = std::isfinite(se->Y()[k]) ? std::fabs(se->Y()[k]) : 0.0;
    if (!(y > 0)) continue;
    const double sw = y / total * 2 * kPi;
    s.Tag(kElemSeries, static_cast<int>(si));
    s.point = k;
    wedge(cx, cy, r, a, a + sw, col_of(k));
    label(k, y, cx + r * 0.65f * static_cast<float>(std::sin(a + sw / 2)), cy - r * 0.65f * static_cast<float>(std::cos(a + sw / 2)), kWhite);
    a += sw;
  }
  s.Tag(kElemSeries, static_cast<int>(si));
  s.point = n;  // "Other"
  const double oa0 = kPi / 2 - o_sweep / 2, oa1 = kPi / 2 + o_sweep / 2;
  if (other > 0) {
    wedge(cx, cy, r, oa0, oa1, ChartPalette(static_cast<size_t>(n)));
    label(n, other, cx + r * 0.65f, cy, kWhite);
  }
  // the second plot, top to bottom (a bar) or clockwise from where the series lines meet it (a pie)
  float top_x = cx2, top_y = cy - r2, bot_x = cx2, bot_y = cy + r2;
  if (other > 0) {
    if (bar) {
      const float bw = r2 * 0.6f;
      float y0 = cy - r2;
      for (int k = 0; k < n; ++k) {
        if (!second[static_cast<size_t>(k)]) continue;
        const double y = std::isfinite(se->Y()[k]) ? std::fabs(se->Y()[k]) : 0.0;
        const float h = static_cast<float>(y / other) * 2 * r2;
        s.Tag(kElemSeries, static_cast<int>(si));
        s.point = k;
        s.Rect(cx2 - bw / 2, y0, cx2 + bw / 2, y0 + h, col_of(k), kWhite, 1.5f);
        label(k, y, cx2, y0 + h / 2, kWhite);
        y0 += h;
      }
      top_x = bot_x = cx2 - bw / 2;
    } else {
      const double total2 = other;
      double b = 1.5 * kPi;  // from 9 o'clock (facing the first pie), clockwise over the top
      for (int k = 0; k < n; ++k) {
        if (!second[static_cast<size_t>(k)]) continue;
        const double y = std::isfinite(se->Y()[k]) ? std::fabs(se->Y()[k]) : 0.0;
        if (!(y > 0)) continue;
        const double sw = y / total2 * 2 * kPi;
        s.Tag(kElemSeries, static_cast<int>(si));
        s.point = k;
        wedge(cx2, cy, r2, b, b + sw, col_of(k));
        label(k, y, cx2 + r2 * 0.6f * static_cast<float>(std::sin(b + sw / 2)), cy - r2 * 0.6f * static_cast<float>(std::cos(b + sw / 2)), kWhite);
        b += sw;
      }
    }
    // series lines from the edges of "Other" to the second plot
    s.Tag(kElemSeries, static_cast<int>(si));
    s.point = -1;
    const float ax0 = cx + r * static_cast<float>(std::sin(oa0)), ay0 = cy - r * static_cast<float>(std::cos(oa0));
    const float ax1 = cx + r * static_cast<float>(std::sin(oa1)), ay1 = cy - r * static_cast<float>(std::cos(oa1));
    s.Line({ax0, ay0, top_x, top_y}, Abgr(0xA6, 0xA6, 0xA6), 1);
    s.Line({ax1, ay1, bot_x, bot_y}, Abgr(0xA6, 0xA6, 0xA6), 1);
  }
  s.point = -1;
  s.Tag(kElemPlotArea);
}

// ---- filled maps ----

}  // namespace

MapMatch MatchMapRegions(const ChartValues& v, size_t n) {
  MapMatch m;
  m.region.assign(n, -1);
  auto leaf = [&](size_t i) -> std::string {
    if (i < v.paths.size() && !v.paths[i].empty()) return v.paths[i].back();
    return i < v.cats.size() ? v.cats[i] : std::string();
  };
  auto context = [&](size_t i) -> std::string {  // a country written in an earlier category column
    if (i < v.paths.size() && v.paths[i].size() >= 2) {
      for (size_t k = v.paths[i].size() - 1; k-- > 0;) {
        const int c = FindGeoRegion(v.paths[i][k], 0, "");
        if (c >= 0) return GeoRegions()[static_cast<size_t>(c)].country;
      }
    }
    return {};
  };
  // countries when most names are countries, otherwise subdivisions
  size_t as_country = 0, as_region = 0;
  for (size_t i = 0; i < n; ++i) {
    const std::string name = leaf(i);
    if (name.empty()) continue;
    if (FindGeoRegion(name, 0, "") >= 0) ++as_country;
    else if (FindGeoRegion(name, 1, context(i)) >= 0) ++as_region;
  }
  m.level = as_region > as_country ? 1 : 0;
  // subdivisions without a country column: the countries most of the names belong to decide unclear ones
  std::map<std::string, int> votes;
  if (m.level == 1) {
    for (size_t i = 0; i < n; ++i) {
      bool amb = false;
      const int r = FindGeoRegion(leaf(i), 1, context(i), &amb);
      if (r >= 0 && !amb) ++votes[GeoRegions()[static_cast<size_t>(r)].country];
    }
  }
  std::string main_country;
  int best = 0;
  for (const auto& [c, k] : votes) {
    if (k > best) best = k, main_country = c;
  }
  for (size_t i = 0; i < n; ++i) {
    const std::string name = leaf(i);
    if (name.empty()) continue;
    int r = -1;
    if (m.level == 0) {
      r = FindGeoRegion(name, 0, "");
    } else {
      std::string ctx = context(i);
      if (ctx.empty()) ctx = main_country;
      r = FindGeoRegion(name, 1, ctx);
      if (r < 0) r = FindGeoRegion(name, 1, "");
    }
    m.region[i] = r;
    if (r >= 0) ++m.matched;
  }
  return m;
}

namespace {

// Miller cylindrical projection: y grows northwards
double MillerY(double lat) {
  const double phi = std::clamp(lat, -85.0, 85.0) * kPi / 180;
  return 1.25 * std::log(std::tan(kPi / 4 + 0.4 * phi)) * 180 / kPi;
}

void SceneMap(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  const size_t n = static_cast<size_t>(std::max(0, se->N()));
  const MapMatch m = MatchMapRegions(v, n);
  const auto& regions = GeoRegions();
  // numbers: a two-color scale; text: a color per category
  bool numeric = false;
  double lo = 1e300, hi = -1e300;
  for (size_t i = 0; i < n; ++i) {
    if (m.region[i] >= 0 && std::isfinite(se->Y()[i])) numeric = true, lo = std::min(lo, se->Y()[i]), hi = std::max(hi, se->Y()[i]);
  }
  std::vector<std::string> cats;  // categories in order
  if (!numeric) {
    std::unordered_set<std::string_view> seen;  // each once (a find a point took points times categories, each frame)
    for (size_t i = 0; i < n && i < se->texts.size(); ++i) {
      if (m.region[i] >= 0 && !se->texts[i].empty() && seen.insert(se->texts[i]).second) cats.push_back(se->texts[i]);
    }
  }
  const uint32_t base = SeriesColor(ch, v, si);
  // Series Color: sequential (2-color, lightest to darkest of the series' color) or diverging (3-color: red, yellow,
  // green at the lowest, middle and highest values), each color chosen or automatic
  const model::ChartSeries* md = Def(ch, si);
  const bool diverging = md && md->geo_colors == 1;
  auto bgr = [](uint32_t c) { return Abgr(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF); };
  const uint32_t c_lo = md && md->geo_min ? bgr(md->geo_min) : diverging ? Abgr(0xF8, 0x69, 0x6B) : Mix(base, kWhite, 0.85f);
  const uint32_t c_hi = md && md->geo_max ? bgr(md->geo_max) : diverging ? Abgr(0x63, 0xBE, 0x7B) : Mix(base, Abgr(0, 0, 0), 0.25f);
  const uint32_t c_mid = md && md->geo_mid ? bgr(md->geo_mid) : Abgr(0xFF, 0xEB, 0x84);
  auto scale_at = [&](float t) {
    if (!diverging) return Mix(c_lo, c_hi, t);
    return t < 0.5f ? Mix(c_lo, c_mid, t * 2) : Mix(c_mid, c_hi, (t - 0.5f) * 2);
  };
  auto color_of = [&](size_t i) -> uint32_t {
    if (numeric) {
      const double y = se->Y()[i];
      if (!std::isfinite(y)) return 0;
      const float t = hi > lo ? static_cast<float>((y - lo) / (hi - lo)) : 1.0f;
      return scale_at(t);
    }
    if (i >= se->texts.size()) return 0;
    const auto it = std::find(cats.begin(), cats.end(), se->texts[i]);
    return it == cats.end() ? 0 : ChartPalette(static_cast<size_t>(it - cats.begin()));
  };
  // the color scale or the categories (the legend)
  const float fh = s.fpx;
  if (ch.has_legend && (numeric || !cats.empty())) {
    s.Tag(kElemLegend);
    if (numeric) {
      const std::string fmt = se->fmt;
      const std::string a = FormatChartNumber(hi, fmt), b = FormatChartNumber(lo, fmt);
      const float w = std::max(s.Width(a), s.Width(b)) + fh + 8;
      const float x0 = R - w, bar_h = std::min(B - T - 2 * fh, fh * 8), y0 = (T + B - bar_h) / 2;
      for (int k = 0; k < 24; ++k) {
        const float a0 = y0 + bar_h * static_cast<float>(k) / 24, a1 = y0 + bar_h * static_cast<float>(k + 1) / 24;
        s.Rect(x0, a0, x0 + fh * 0.8f, a1 + 0.5f, scale_at(1.0f - (static_cast<float>(k) + 0.5f) / 24));
      }
      s.Text(a, x0 + fh, y0, 0, 1, kText);
      s.Text(b, x0 + fh, y0 + bar_h, 0, 1, kText);
      R = x0 - 6;
    } else {
      std::vector<LegendItem> items;
      for (size_t k = 0; k < cats.size(); ++k) items.push_back({cats[k], ChartPalette(k)});
      LegendStyle st;
      st.pos = 1;
      Legend(s, items, &L, &T, &R, &B, st);
    }
    s.Tag(kElemPlotArea);
  }
  if (R - L < 20 || B - T < 20) return;
  // the area shown: the regions with data (countries of the subdivisions), or the world
  double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  std::set<std::string> countries;
  for (size_t i = 0; i < n; ++i) {
    if (m.region[i] < 0) continue;
    const GeoRegion& g = regions[static_cast<size_t>(m.region[i])];
    countries.insert(g.country);
    x0 = std::min<double>(x0, g.lon0), x1 = std::max<double>(x1, g.lon1), y0 = std::min<double>(y0, g.lat0), y1 = std::max<double>(y1, g.lat1);
  }
  // Map area (Series.GeoMappingLevel): automatic, only the regions with data, their whole countries, or the world
  const int area = md ? md->geo_area : 0;
  if (area == 7 || m.matched == 0 || (area == 0 && m.level == 0 && x1 - x0 > 120)) {
    x0 = -170, x1 = 190, y0 = -58, y1 = 84;  // the world
  } else if (m.level == 1 && area != 1) {  // the whole of the countries
    for (const auto& g : regions) {
      if (g.level == 0 && countries.count(g.country)) x0 = std::min<double>(x0, g.lon0), x1 = std::max<double>(x1, g.lon1), y0 = std::min<double>(y0, g.lat0), y1 = std::max<double>(y1, g.lat1);
    }
  }
  const double pad_x = std::max(0.5, (x1 - x0) * 0.05), pad_y = std::max(0.5, (y1 - y0) * 0.05);
  x0 -= pad_x, x1 += pad_x, y0 -= pad_y, y1 += pad_y;
  // Map projection: Mercator, Miller (automatic), Albers (equal-area conic, standard parallels inside the area) or
  // Robinson
  const int proj = md ? md->geo_projection : 0;
  const double lon_c = (x0 + x1) / 2, phi1 = (y0 + (y1 - y0) / 6) * kPi / 180, phi2 = (y1 - (y1 - y0) / 6) * kPi / 180;
  const double an = std::fabs(std::sin(phi1) + std::sin(phi2)) > 1e-3 ? (std::sin(phi1) + std::sin(phi2)) / 2 : 0.5;
  const double aC = std::cos(phi1) * std::cos(phi1) + 2 * an * std::sin(phi1);
  constexpr double kDeg = 180 / kPi;
  auto project = [&](double lon, double lat, double* px, double* py) {
    switch (proj) {
      case 1: {  // Mercator
        const double phi = std::clamp(lat, -85.0, 85.0) / kDeg;
        *px = lon;
        *py = std::log(std::tan(kPi / 4 + phi / 2)) * kDeg;
        return;
      }
      case 3: {  // Albers
        const double phi = lat / kDeg, lam = (lon - lon_c) / kDeg;
        const double rho = std::sqrt(std::max(0.0, aC - 2 * an * std::sin(phi))) / an, th = an * lam;
        *px = rho * std::sin(th) * kDeg;
        *py = -rho * std::cos(th) * kDeg;
        return;
      }
      case 4: {  // Robinson: its table every 5 degrees of latitude
        static const double kX[] = {1.0, 0.9986, 0.9954, 0.99, 0.9822, 0.973, 0.96, 0.9427, 0.9216, 0.8962, 0.8679, 0.835, 0.7986, 0.7597, 0.7186, 0.6732, 0.6213, 0.5722, 0.5322};
        static const double kY[] = {0.0, 0.062, 0.124, 0.186, 0.248, 0.31, 0.372, 0.434, 0.4958, 0.5571, 0.6176, 0.6769, 0.7346, 0.7903, 0.8435, 0.8936, 0.9394, 0.9761, 1.0};
        const double a = std::min(90.0, std::fabs(lat)) / 5;
        const size_t i = std::min<size_t>(17, static_cast<size_t>(a));
        const double f = a - static_cast<double>(i);
        const double X = kX[i] + (kX[i + 1] - kX[i]) * f, Y = kY[i] + (kY[i + 1] - kY[i]) * f;
        *px = 0.8487 * X * (lon - lon_c);
        *py = (lat < 0 ? -1.0 : 1.0) * 1.3523 * Y * kDeg;
        return;
      }
      default:  // Miller
        *px = lon;
        *py = MillerY(lat);
        return;
    }
  };
  double qx0 = 1e300, qx1 = -1e300, qy0 = 1e300, qy1 = -1e300;
  for (int a = 0; a <= 8; ++a) {
    for (int b = 0; b <= 8; ++b) {
      double px = 0, py = 0;
      project(x0 + (x1 - x0) * a / 8, y0 + (y1 - y0) * b / 8, &px, &py);
      qx0 = std::min(qx0, px), qx1 = std::max(qx1, px), qy0 = std::min(qy0, py), qy1 = std::max(qy1, py);
    }
  }
  const double k = std::min((R - L) / std::max(1e-9, qx1 - qx0), (B - T) / std::max(1e-9, qy1 - qy0));
  const float ox = L + static_cast<float>(((R - L) - (qx1 - qx0) * k) / 2), oy = T + static_cast<float>(((B - T) - (qy1 - qy0) * k) / 2);
  auto P = [&](float lon, float lat) {
    double px = 0, py = 0;
    project(lon, lat, &px, &py);
    return std::pair<float, float>{ox + static_cast<float>((px - qx0) * k), oy + static_cast<float>((qy1 - py) * k)};
  };
  auto visible = [&](const GeoRegion& g) { return g.lon1 >= x0 && g.lon0 <= x1 && g.lat1 >= y0 && g.lat0 <= y1; };
  auto shape = [&](const GeoRegion& g, uint32_t fill, uint32_t stroke) {
    for (const auto& part : g.parts) {
      std::vector<float> pts;
      pts.reserve(part.size() * 2);
      for (const auto& [lon, lat] : part) {
        const auto [x, y] = P(lon, lat);
        pts.push_back(x);
        pts.push_back(y);
      }
      s.Poly(std::move(pts), fill, stroke, 0.75f);
    }
  };
  const uint32_t land = Abgr(0xE8, 0xE8, 0xE8), border = kWhite;
  // the background: countries around (and, for subdivisions, the countries' own subdivisions without data)
  s.Tag(kElemPlotArea);
  for (const auto& g : regions) {
    if (g.level == 0 && visible(g) && !(m.level == 1 && countries.count(g.country))) shape(g, land, border);
  }
  std::vector<char> has(regions.size(), 0);
  for (int r : m.region) {
    if (r >= 0) has[static_cast<size_t>(r)] = 1;
  }
  if (m.level == 1) {
    for (size_t r = 0; r < regions.size(); ++r) {
      const GeoRegion& g = regions[r];
      if (g.level == 1 && !has[r] && countries.count(g.country)) shape(g, land, border);
    }
  }
  // the regions with data
  s.Tag(kElemSeries, static_cast<int>(si));
  for (size_t i = 0; i < n; ++i) {
    if (m.region[i] < 0) continue;
    const uint32_t c = color_of(i);
    shape(regions[static_cast<size_t>(m.region[i])], c ? c : land, border);
  }
  if (const auto* d = Def(ch, si); d && d->data_labels) {
    for (size_t i = 0; i < n; ++i) {
      if (m.region[i] < 0) continue;
      const GeoRegion& g = regions[static_cast<size_t>(m.region[i])];
      const auto [x, y] = P(g.label_lon, g.label_lat);
      const std::string t = numeric ? FormatChartNumber(se->Y()[i], se->fmt) : (i < se->texts.size() ? se->texts[i] : std::string());
      s.Text(t, x, y + (md && md->geo_labels ? s.fpx * 0.45f : 0.0f), 1, 1, kText, 0.85f);
    }
  }
  // Map labels: the regions' names, every one or only those that fit inside their region
  if (md && md->geo_labels > 0) {
    s.Tag(kElemSeries, static_cast<int>(si));
    for (size_t i = 0; i < n; ++i) {
      if (m.region[i] < 0) continue;
      const GeoRegion& g = regions[static_cast<size_t>(m.region[i])];
      const std::string name = i < v.cats.size() && !v.cats[i].empty() ? v.cats[i] : g.names.empty() ? g.iso : g.names.front();
      const auto [x, y] = P(g.label_lon, g.label_lat);
      const auto [ax, ay] = P(g.lon0, g.lat0);
      const auto [bx, by] = P(g.lon1, g.lat1);
      if (md->geo_labels == 1 && (s.Width(name, 0.8f) > std::fabs(bx - ax) || s.fpx > std::fabs(by - ay))) continue;
      s.Text(name, x, y - (md->data_labels ? s.fpx * 0.45f : 0.0f), 1, 1, kText, 0.8f);
    }
  }
  s.Tag(kElemPlotArea);
}

// ---- 3-D charts: a world of boxes, ribbons and slabs projected on the page and drawn back to front ----
// World axes: x to the right, y up, z into the page (away from the viewer). Rotation turns the world around y,
// elevation tilts the viewer above it, perspective shrinks what is farther away.

struct Proj {
  double cp = 1, sp = 0, ct = 1, st = 0, persp = 0;
  double ccx = 0, ccy = 0, cd = 0, dspan = 1;  // the projected world center and depth range (perspective)
  double minx = 0, miny = 0, k = 1;
  float L0 = 0, B0 = 0;
  void Base(double x, double y, double z, double* sx, double* sy, double* d) const {
    const double rx = x * cp + z * sp, rz = -x * sp + z * cp;
    *sx = rx;
    *sy = y * ct + rz * st;
    *d = rz * ct - y * st;
  }
  void Raw(double x, double y, double z, double* sx, double* sy, double* d) const {
    Base(x, y, z, sx, sy, d);
    if (persp > 0) {
      const double f = 1.0 / std::max(0.2, 1.0 + persp * (*d - cd) / dspan);
      *sx = ccx + (*sx - ccx) * f;
      *sy = ccy + (*sy - ccy) * f;
    }
  }
  std::pair<float, float> P(double x, double y, double z) const {
    double sx, sy, d;
    Raw(x, y, z, &sx, &sy, &d);
    return {L0 + static_cast<float>((sx - minx) * k), B0 - static_cast<float>((sy - miny) * k)};
  }
  double D(double x, double y, double z) const {
    double sx, sy, d;
    Base(x, y, z, &sx, &sy, &d);
    return d;
  }
};

// Chart.Rotation / Elevation / Perspective / DepthPercent, or the chart type's defaults (like Excel's)
void View3DOf(const model::ChartObj& ch, double* rot, double* elev, double* persp, double* depth) {
  if (ch.view3d) {
    *rot = ch.rotation, *elev = ch.elevation, *persp = ch.perspective, *depth = ch.depth_percent;
    return;
  }
  *depth = 100;
  if (ch.type == model::ChartType::Pie3D) {
    *rot = 0, *elev = 30, *persp = 0;
  } else {
    *rot = 20, *elev = 15, *persp = 15;
  }
}

// fit the world box [0, W] x [0, H] x [0, D] into a screen rectangle
Proj MakeProj(double W, double H, double D, double rot, double elev, double persp, float L, float T, float R, float B) {
  Proj p;
  const double a = rot * kPi / 180, e = std::clamp(elev, -90.0, 90.0) * kPi / 180;
  p.cp = std::cos(a), p.sp = std::sin(a), p.ct = std::cos(e), p.st = std::sin(e);
  double dlo = 1e300, dhi = -1e300;
  const double corners[8][3] = {{0, 0, 0}, {W, 0, 0}, {0, H, 0}, {W, H, 0}, {0, 0, D}, {W, 0, D}, {0, H, D}, {W, H, D}};
  for (const auto& c : corners) {
    const double d = p.D(c[0], c[1], c[2]);
    dlo = std::min(dlo, d), dhi = std::max(dhi, d);
  }
  p.dspan = std::max(1e-9, dhi - dlo);
  double sx, sy, d;
  p.Base(W / 2, H / 2, D / 2, &sx, &sy, &d);
  p.ccx = sx, p.ccy = sy, p.cd = d;
  p.persp = std::clamp(persp, 0.0, 100.0) / 100.0 * 0.6;
  double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
  for (const auto& c : corners) {
    p.Raw(c[0], c[1], c[2], &sx, &sy, &d);
    x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
  }
  const double bw = std::max(1e-9, x1 - x0), bh = std::max(1e-9, y1 - y0);
  p.k = std::max(1e-6, std::min((R - L) / bw, (B - T) / bh));
  p.minx = x0, p.miny = y0;
  p.L0 = L + static_cast<float>(((R - L) - bw * p.k) / 2);
  p.B0 = B - static_cast<float>(((B - T) - bh * p.k) / 2);
  return p;
}

struct Face3 {
  std::vector<float> pts;
  uint32_t fill = 0, stroke = 0;
};
struct Item3 {
  double depth = 0;
  int series = -1;  // -1: walls
  std::vector<Face3> faces;
};

void Quad(const Proj& p, Item3* it, std::initializer_list<std::array<double, 3>> q, uint32_t fill, uint32_t stroke = 0) {
  Face3 f;
  for (const auto& v : q) {
    const auto [x, y] = p.P(v[0], v[1], v[2]);
    f.pts.push_back(x);
    f.pts.push_back(y);
  }
  f.fill = fill;
  f.stroke = stroke;
  it->faces.push_back(std::move(f));
}

// a box's visible faces (the far ones are hidden): the side towards the viewer, the top or bottom, then the front
Item3 Box3(const Proj& p, double x0, double x1, double y0, double y1, double z0, double z1, uint32_t col, int series) {
  Item3 it;
  it.series = series;
  if (y1 < y0) std::swap(y0, y1);
  // order by where the box stands on the floor (its height must not make it look nearer); stacked boxes go up
  it.depth = p.D((x0 + x1) / 2, 0, (z0 + z1) / 2) - 1e-6 * y0;
  const uint32_t side = Mix(col, Abgr(0, 0, 0), 0.22f), top = Mix(col, kWhite, 0.18f), edge = Mix(col, Abgr(0, 0, 0), 0.35f);
  const double sx = p.sp >= 0 ? x1 : x0;
  Quad(p, &it, {{sx, y0, z0}, {sx, y0, z1}, {sx, y1, z1}, {sx, y1, z0}}, side, edge);
  const double ty = p.st >= 0 ? y1 : y0;
  Quad(p, &it, {{x0, ty, z0}, {x1, ty, z0}, {x1, ty, z1}, {x0, ty, z1}}, top, edge);
  Quad(p, &it, {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}}, col, edge);
  return it;
}

// a closed polygon without its vertices that lie on the line between their neighbours: a filled concave polygon is cut
// into triangles by ImGui every frame, and a run of points on one line (an area's base, empty cells along its top) made
// that take minutes for a 3D area over whole columns (scale_sweep)
void DropCollinear(std::vector<float>& pts) {
  const size_t n = pts.size() / 2;
  if (n < 4) return;
  std::vector<float> out;
  out.reserve(pts.size());
  for (size_t i = 0; i < n; ++i) {
    const size_t a = (i + n - 1) % n, b = (i + 1) % n;
    const double ax = pts[2 * a], ay = pts[2 * a + 1], x = pts[2 * i], y = pts[2 * i + 1], bx = pts[2 * b], by = pts[2 * b + 1];
    // the point's distance from the line through its neighbours, in pixels (a fiftieth of one: what rounding leaves)
    const double cross = (x - ax) * (by - y) - (y - ay) * (bx - x);
    const double len = std::hypot(bx - ax, by - ay);
    const bool between = (x - ax) * (bx - x) + (y - ay) * (by - y) >= 0;
    if (between && (len < 1e-9 ? std::hypot(x - ax, y - ay) < 0.02 : std::fabs(cross) / len < 0.02)) continue;  // on the way
    out.push_back(pts[2 * i]);
    out.push_back(pts[2 * i + 1]);
  }
  if (out.size() >= 6) pts = std::move(out);
}

void EmitItems(S& s, std::vector<Item3>& items) {
  std::stable_sort(items.begin(), items.end(), [](const Item3& a, const Item3& b) { return a.depth > b.depth; });
  for (auto& it : items) {
    s.Tag(it.series >= 0 ? kElemSeries : kElemPlotArea, it.series);
    for (auto& f : it.faces) s.Poly(std::move(f.pts), f.fill, f.stroke, 0.75f);
  }
  s.Tag(kElemPlotArea);
}

// value axis bounds: round steps, zero included for columns and areas
void ValueRange3D(const model::ChartObj& ch, double lo, double hi, bool zero, double* a, double* b, double* step) {
  if (zero) lo = std::min(lo, 0.0), hi = std::max(hi, 0.0);
  if (!ch.val_axis.min_auto) lo = ch.val_axis.min;
  if (!ch.val_axis.max_auto) hi = ch.val_axis.max;
  if (!(hi > lo)) hi = lo + 1;
  *step = ch.val_axis.major > 0 && (hi - lo) / ch.val_axis.major <= 60 ? ch.val_axis.major : NiceStep(hi - lo);
  if (ch.val_axis.min_auto) lo = std::floor(lo / *step + 1e-9) * *step;
  if (ch.val_axis.max_auto) hi = std::ceil(hi / *step - 1e-9) * *step;
  if (!(hi > lo)) hi = lo + *step;
  *a = lo, *b = hi;
}

// the back wall and the side wall (Walls), then the floor (Floor): their own elements, so they can be clicked and
// formatted; what is drawn after them belongs to the plot area again
void EmitWalls(S& s, Item3& w) {
  for (size_t k = 0; k < w.faces.size(); ++k) {
    s.Tag(static_cast<uint8_t>(k < 2 ? model::ChartElem::Walls : model::ChartElem::Floor));
    s.Poly(std::move(w.faces[k].pts), w.faces[k].fill, w.faces[k].stroke, 0.75f);
  }
  s.Tag(kElemPlotArea);
}

// 3-D column, clustered / stacked column, clustered bar, line and area charts: walls, gridlines and labels around the data
void SceneCartesian3D(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  using MT = model::ChartType;
  const MT t = ch.type;
  const size_t ns = v.series.size();
  size_t n = 0;
  for (const auto& se : v.series) n = std::max(n, static_cast<size_t>(std::max(0, se.N())));
  if (!n || !ns) return;
  const bool rows = t == MT::Column3D || t == MT::Line3D || t == MT::Area3D;  // each series in its own row along the depth
  const bool stacked = t == MT::ColumnStacked3D, horizontal = t == MT::BarClustered3D, columns = t != MT::Line3D && t != MT::Area3D;
  double lo = 1e300, hi = -1e300;
  if (stacked) {
    for (size_t i = 0; i < n; ++i) {
      double pos = 0, neg = 0;
      for (const auto& se : v.series) {
        const double y = static_cast<int>(i) < se.N() ? se.Y()[i] : kNaN;
        if (std::isfinite(y)) (y >= 0 ? pos : neg) += y;
      }
      lo = std::min(lo, neg), hi = std::max(hi, pos);
    }
  } else {
    for (const auto& se : v.series) {
      for (int i = 0; i < se.N(); ++i) {
        if (std::isfinite(se.Y()[i])) lo = std::min(lo, se.Y()[i]), hi = std::max(hi, se.Y()[i]);
      }
    }
  }
  if (lo > hi) lo = 0, hi = 1;
  double vlo, vhi, step;
  ValueRange3D(ch, lo, hi, columns || t == MT::Area3D, &vlo, &vhi, &step);
  double rot, elev, persp, depth;
  View3DOf(ch, &rot, &elev, &persp, &depth);
  const double cat_len = static_cast<double>(n), val_len = horizontal ? std::max(2.0, cat_len * 1.3) : std::max(2.0, cat_len * 0.6);
  const double Z = (rows ? static_cast<double>(ns) : 1.0) * std::clamp(depth, 20.0, 2000.0) / 100.0;
  const double X = horizontal ? val_len : cat_len, Y = horizontal ? cat_len : val_len;
  auto vw = [&](double val) { return (std::clamp(val, vlo, vhi) - vlo) / (vhi - vlo) * val_len; };
  auto W3 = [&](double c, double u, double z) { return horizontal ? std::array<double, 3>{u, c, z} : std::array<double, 3>{c, u, z}; };
  // ticks and the room their labels take
  const std::string fmt = !ch.val_axis.number_format.empty() ? ch.val_axis.number_format : v.series[0].fmt;
  std::vector<std::pair<double, std::string>> ticks;
  for (int k = 0; k <= 60; ++k) {
    const double val = vlo + step * k;
    if (val > vhi + step * 1e-6) break;
    ticks.emplace_back(val, FormatChartNumber(std::fabs(val) < step * 1e-9 ? 0.0 : val, fmt));
  }
  float tick_w = 0, cat_w = 0, ser_w = 0;
  for (const auto& tk : ticks) tick_w = std::max(tick_w, s.Width(tk.second, s.axis_scale));
  for (size_t i = 0; i < n; ++i) cat_w = std::max(cat_w, s.Width(Cat(v, i), s.axis_scale));
  if (rows) {
    for (const auto& se : v.series) ser_w = std::max(ser_w, s.Width(se.name, s.axis_scale));
  }
  const float fh = s.fpx * s.axis_scale;
  const float lm = (horizontal ? std::min(cat_w, (R - L) / 3) : tick_w) + 8, bm = fh + 6, rm = rows ? std::min(ser_w, (R - L) / 4) + 8 : 4;
  const Proj p = MakeProj(X, Y, Z, rot, elev, persp, L + lm, T + fh * 0.5f, R - rm, B - bm);
  // walls and floor
  {
    std::vector<Item3> walls(1);
    Item3& w = walls[0];
    const uint32_t wall = ch.wall_fill ? FillInk(ch.wall_fill) : ch.plot_fill ? FillInk(ch.plot_fill) : kWhite;
    const double side_x = p.sp >= 0 ? 0 : X;
    Quad(p, &w, {{0, 0, Z}, {X, 0, Z}, {X, Y, Z}, {0, Y, Z}}, wall, kGrid);
    Quad(p, &w, {{side_x, 0, 0}, {side_x, 0, Z}, {side_x, Y, Z}, {side_x, Y, 0}}, wall, kGrid);
    const double floor_y = p.st >= 0 ? 0 : Y;
    Quad(p, &w, {{0, floor_y, 0}, {X, floor_y, 0}, {X, floor_y, Z}, {0, floor_y, Z}},
         ch.floor_fill ? FillInk(ch.floor_fill) : Mix(wall, Abgr(0xD9, 0xD9, 0xD9), 0.35f), kGrid);
    EmitWalls(s, w);
    // gridlines across the back wall and the side wall (or the floor, for bars)
    if (ch.val_axis.gridlines) {
      for (const auto& tk : ticks) {
        const double u = vw(tk.first);
        std::vector<float> line;
        for (const auto& q : {W3(0, u, Z), W3(cat_len, u, Z)}) {
          const auto [x, y] = p.P(q[0], q[1], q[2]);
          line.push_back(x);
          line.push_back(y);
        }
        s.Line(line, kGrid);
        const auto a = horizontal ? p.P(u, floor_y, 0) : p.P(side_x, u, 0);
        const auto b = horizontal ? p.P(u, floor_y, Z) : p.P(side_x, u, Z);
        s.Line({a.first, a.second, b.first, b.second}, kGrid);
      }
    }
    // value labels along the front edge of the side wall (bars: of the floor)
    if (ch.val_axis.visible) {
      for (const auto& tk : ticks) {
        const double u = vw(tk.first);
        if (horizontal) {
          const auto [x, y] = p.P(u, floor_y, 0);
          s.Tick(kElemValAxis, tk.second, x, y + 3, 1, 0);
        } else {
          const auto [x, y] = p.P(side_x, u, 0);
          s.Tick(kElemValAxis, tk.second, side_x == 0 ? x - 4 : x + 4, y, side_x == 0 ? 2 : 0, 1);
        }
      }
    }
    if (ch.cat_axis.visible) {
      const size_t every = horizontal ? 1 : std::max<size_t>(1, static_cast<size_t>(std::ceil((cat_w + 8) / std::max(1.0, p.k * p.cp))));
      for (size_t i = 0; i < n; i += every) {
        if (horizontal) {
          const auto [x, y] = p.P(side_x, static_cast<double>(i) + 0.5, 0);
          s.Tick(kElemCatAxis, Cat(v, i), x - 4, y, 2, 1);
        } else {
          const auto [x, y] = p.P(static_cast<double>(i) + 0.5, floor_y, 0);
          s.Tick(kElemCatAxis, Cat(v, i), x, y + 3, 1, 0);
        }
      }
    }
    if (rows) {  // the series names along the depth axis
      const double dz = Z / static_cast<double>(ns);
      for (size_t k = 0; k < ns; ++k) {
        const auto [x, y] = p.P(p.sp >= 0 ? X : 0, floor_y, dz * (static_cast<double>(k) + 0.5));
        s.Tick(kElemCatAxis, v.series[k].name, p.sp >= 0 ? x + 4 : x - 4, y, p.sp >= 0 ? 0 : 2, 1);
      }
    }
  }
  // the data
  std::vector<Item3> items;
  std::vector<std::tuple<std::array<double, 3>, std::string, int>> labels;  // data labels: where, text, series
  const double base = vw(std::clamp(0.0, vlo, vhi));
  if (columns) {
    std::vector<double> pos(n, 0), neg(n, 0);
    for (size_t k = 0; k < ns; ++k) {
      const auto& se = v.series[k];
      const uint32_t col = SeriesColor(ch, v, k);
      const auto* d = Def(ch, k);
      for (int i = 0; i < se.N() && static_cast<size_t>(i) < n; ++i) {
        const double val = se.Y()[i];
        if (!std::isfinite(val)) continue;
        double c0, c1, z0, z1, u0, u1;
        if (t == MT::Column3D) {
          const double dz = Z / static_cast<double>(ns);
          c0 = i + 0.2, c1 = i + 0.8, z0 = dz * (static_cast<double>(k) + 0.2), z1 = dz * (static_cast<double>(k) + 0.8);
          u0 = base, u1 = vw(val);
        } else if (stacked) {
          c0 = i + 0.2, c1 = i + 0.8, z0 = Z * 0.2, z1 = Z * 0.8;
          double& acc = val >= 0 ? pos[static_cast<size_t>(i)] : neg[static_cast<size_t>(i)];
          u0 = vw(acc), u1 = vw(acc + val);
          acc += val;
        } else {
          const double gw = 0.7 / static_cast<double>(ns);
          c0 = i + 0.15 + gw * static_cast<double>(k), c1 = c0 + gw * 0.95, z0 = Z * 0.2, z1 = Z * 0.8;
          u0 = base, u1 = vw(val);
        }
        const auto a = W3(c0, u0, z0), b = W3(c1, u1, z1);
        items.push_back(Box3(p, std::min(a[0], b[0]), std::max(a[0], b[0]), std::min(a[1], b[1]), std::max(a[1], b[1]), z0, z1, col, static_cast<int>(k)));
        if (d && d->data_labels) labels.emplace_back(W3((c0 + c1) / 2, std::max(u0, u1), z0), FormatChartNumber(val, se.fmt), static_cast<int>(k));
      }
    }
  } else {
    const double dz = Z / static_cast<double>(ns);
    for (size_t k = 0; k < ns; ++k) {
      const auto& se = v.series[k];
      const uint32_t col = SeriesColor(ch, v, k);
      const auto* d = Def(ch, k);
      const int m = std::min(se.N(), static_cast<int>(n));
      std::vector<double> xs, us;
      for (int i = 0; i < m; ++i) {
        const double val = se.Y()[i];
        xs.push_back(i + 0.5);
        us.push_back(std::isfinite(val) ? vw(val) : base);
        if (d && d->data_labels && std::isfinite(val)) labels.emplace_back(std::array<double, 3>{i + 0.5, vw(val), dz * k + dz * 0.25}, FormatChartNumber(val, se.fmt), static_cast<int>(k));
      }
      if (xs.empty()) continue;
      if (t == MT::Line3D) {
        // a ribbon: one slanted strip per segment, with a thin front edge
        const double z0 = dz * (static_cast<double>(k) + 0.25), z1 = dz * (static_cast<double>(k) + 0.75), th = val_len * 0.025;
        for (size_t i = 0; i + 1 < xs.size(); ++i) {
          Item3 it;
          it.series = static_cast<int>(k);
          it.depth = p.D((xs[i] + xs[i + 1]) / 2, 0, (z0 + z1) / 2);
          const uint32_t top = us[i + 1] >= us[i] ? Mix(col, kWhite, 0.15f) : Mix(col, Abgr(0, 0, 0), 0.12f);
          Quad(p, &it, {{xs[i], us[i], z0}, {xs[i + 1], us[i + 1], z0}, {xs[i + 1], us[i + 1], z1}, {xs[i], us[i], z1}}, top, Mix(col, Abgr(0, 0, 0), 0.3f));
          Quad(p, &it, {{xs[i], us[i], z0}, {xs[i + 1], us[i + 1], z0}, {xs[i + 1], us[i + 1] - th, z0}, {xs[i], us[i] - th, z0}}, Mix(col, Abgr(0, 0, 0), 0.25f));
          items.push_back(std::move(it));
        }
      } else {
        // an area slab: the top strips, the end cap facing the viewer, then the front
        const double z0 = dz * (static_cast<double>(k) + 0.1), z1 = dz * (static_cast<double>(k) + 0.9);
        Item3 it;
        it.series = static_cast<int>(k);
        it.depth = p.D(cat_len / 2, base, (z0 + z1) / 2);
        const uint32_t top = Mix(col, kWhite, 0.18f), side = Mix(col, Abgr(0, 0, 0), 0.22f), edge = Mix(col, Abgr(0, 0, 0), 0.35f);
        for (size_t i = 0; i + 1 < xs.size(); ++i)
          Quad(p, &it, {{xs[i], us[i], z0}, {xs[i + 1], us[i + 1], z0}, {xs[i + 1], us[i + 1], z1}, {xs[i], us[i], z1}}, top, edge);
        const size_t cap = p.sp >= 0 ? xs.size() - 1 : 0;
        Quad(p, &it, {{xs[cap], base, z0}, {xs[cap], base, z1}, {xs[cap], us[cap], z1}, {xs[cap], us[cap], z0}}, side, edge);
        Face3 front;
        for (size_t i = 0; i < xs.size(); ++i) {
          const auto [x, y] = p.P(xs[i], us[i], z0);
          front.pts.push_back(x);
          front.pts.push_back(y);
        }
        for (size_t i = xs.size(); i-- > 0;) {
          const auto [x, y] = p.P(xs[i], base, z0);
          front.pts.push_back(x);
          front.pts.push_back(y);
        }
        DropCollinear(front.pts);
        front.fill = col;
        front.stroke = edge;
        it.faces.push_back(std::move(front));
        items.push_back(std::move(it));
      }
    }
  }
  EmitItems(s, items);
  for (const auto& [w, text, k] : labels) {
    s.Tag(kElemSeries, k);
    const auto [x, y] = p.P(w[0], w[1], w[2]);
    s.Text(text, x, y - 2, 1, 2);
  }
  s.Tag(kElemPlotArea);
}

// a tilted pie with a rim; Rotation turns the first slice
void ScenePie3D(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B) {
  const ChartSeriesValues* se = First(v);
  if (!se) return;
  const size_t si = static_cast<size_t>(se - v.series.data());
  double total = 0;
  const int n = std::min(se->N(), 64);
  for (int i = 0; i < n; ++i) total += std::isfinite(se->Y()[i]) && se->Y()[i] > 0 ? se->Y()[i] : 0;
  if (!(total > 0)) return;
  double rot, elev, persp, depth;
  View3DOf(ch, &rot, &elev, &persp, &depth);
  elev = std::clamp(elev, 10.0, 90.0);
  const double se_ = std::sin(elev * kPi / 180);
  const float cx = (L + R) / 2;
  float rx = std::max(4.0f, (R - L) / 2 - 4);
  float thick = rx * 0.18f * static_cast<float>(std::cos(elev * kPi / 180)) * static_cast<float>(depth / 100.0);
  float ry = rx * static_cast<float>(se_);
  if (ry * 2 + thick > B - T - 4) {
    const float f = (B - T - 4) / (ry * 2 + thick);
    rx *= f, ry *= f, thick *= f;
  }
  const float cy = (T + B - thick) / 2;
  // slice angles clockwise from 12 o'clock
  auto at = [&](double a, float dy) { return std::pair<float, float>{cx + rx * static_cast<float>(std::sin(a)), cy - ry * static_cast<float>(std::cos(a)) + dy}; };
  std::vector<std::pair<double, double>> slices;
  double a = rot * kPi / 180;
  for (int i = 0; i < n; ++i) {
    const double val = std::isfinite(se->Y()[i]) && se->Y()[i] > 0 ? se->Y()[i] : 0;
    const double sweep = 2 * kPi * val / total;
    slices.emplace_back(a, a + sweep);
    a += sweep;
  }
  s.Tag(kElemSeries, static_cast<int>(si));
  // the rim: the parts of the edge facing the viewer (the lower half of the ellipse), far ones first
  struct Rim {
    double mid;
    std::vector<float> pts;
    uint32_t col;
  };
  std::vector<Rim> rims;
  for (size_t i = 0; i < slices.size(); ++i) {
    const uint32_t col = Mix(ChartPalette(i), Abgr(0, 0, 0), 0.25f);
    // visible arc: angles whose cos is negative (pi/2 .. 3pi/2, modulo 2pi)
    for (int wrap = -1; wrap <= 2; ++wrap) {
      const double lo = std::max(slices[i].first, kPi / 2 + 2 * kPi * wrap), hi = std::min(slices[i].second, 3 * kPi / 2 + 2 * kPi * wrap);
      if (hi <= lo) continue;
      Rim r;
      r.mid = (lo + hi) / 2;
      r.col = col;
      const int steps = std::max(2, static_cast<int>((hi - lo) / (2 * kPi) * 96));
      for (int k = 0; k <= steps; ++k) {
        const auto [x, y] = at(lo + (hi - lo) * k / steps, 0);
        r.pts.push_back(x);
        r.pts.push_back(y);
      }
      for (int k = steps; k >= 0; --k) {
        const auto [x, y] = at(lo + (hi - lo) * k / steps, thick);
        r.pts.push_back(x);
        r.pts.push_back(y);
      }
      rims.push_back(std::move(r));
    }
  }
  // nearer rim pieces (closer to 6 o'clock) last
  std::stable_sort(rims.begin(), rims.end(), [](const Rim& x, const Rim& y) { return std::fabs(std::remainder(x.mid - kPi, 2 * kPi)) > std::fabs(std::remainder(y.mid - kPi, 2 * kPi)); });
  for (auto& r : rims) s.Poly(std::move(r.pts), r.col, Mix(r.col, Abgr(0, 0, 0), 0.2f), 0.75f);
  const bool labels = !ch.series.empty() && ch.series[si < ch.series.size() ? si : 0].data_labels;
  for (size_t i = 0; i < slices.size(); ++i) {
    const auto [a0, a1] = slices[i];
    if (a1 - a0 <= 0) continue;
    std::vector<float> pts = {cx, cy};
    const int steps = std::max(2, static_cast<int>((a1 - a0) / (2 * kPi) * 120));
    for (int k = 0; k <= steps; ++k) {
      const auto [x, y] = at(a0 + (a1 - a0) * k / steps, 0);
      pts.push_back(x);
      pts.push_back(y);
    }
    s.Poly(std::move(pts), ChartPalette(i), kWhite, 1);
    if (labels) {
      const double am = (a0 + a1) / 2;
      s.Text(FormatChartNumber(se->Y()[i], se->fmt), cx + rx * 0.62f * static_cast<float>(std::sin(am)), cy - ry * 0.62f * static_cast<float>(std::cos(am)), 1, 1, kWhite);
    }
  }
  s.Tag(kElemPlotArea);
}

// surface charts: value bands (the value axis steps) give the colors
struct Bands {
  double lo = 0, hi = 1, step = 1;
  size_t count = 1;
  size_t Of(double val) const { return std::min(count - 1, static_cast<size_t>(std::max(0.0, std::floor((val - lo) / step)))); }
};

Bands SurfaceBands(const model::ChartObj& ch, const ChartValues& v) {
  double lo = 1e300, hi = -1e300;
  for (const auto& se : v.series) {
    for (int i = 0; i < se.N(); ++i) {
      if (std::isfinite(se.Y()[i])) lo = std::min(lo, se.Y()[i]), hi = std::max(hi, se.Y()[i]);
    }
  }
  if (lo > hi) lo = 0, hi = 1;
  Bands b;
  ValueRange3D(ch, lo, hi, false, &b.lo, &b.hi, &b.step);
  b.count = std::max<size_t>(1, static_cast<size_t>(std::llround((b.hi - b.lo) / b.step)));
  return b;
}

std::vector<LegendItem> BandLegend(const Bands& b, const std::string& fmt) {
  std::vector<LegendItem> out;
  for (size_t k = 0; k < b.count && k < 30; ++k) {
    const double a = b.lo + b.step * static_cast<double>(k);
    out.push_back({FormatChartNumber(a, fmt) + "-" + FormatChartNumber(a + b.step, fmt), ChartPalette(k)});
  }
  return out;
}

// the value at a fractional grid position (bilinear between the four corners)
double GridAt(const ChartValues& v, double i, double k) {
  auto val = [&](int ii, int kk) {
    const auto& se = v.series[static_cast<size_t>(kk)];
    return ii < se.N() && std::isfinite(se.Y()[ii]) ? se.Y()[ii] : 0.0;
  };
  if (v.series.empty()) return 0.0;
  // one series spans a grid of one cell (k runs to 1.0): both corners are clamped to the series there are
  const int last_k = static_cast<int>(v.series.size()) - 1;
  const int i0 = std::max(0, static_cast<int>(std::floor(i))), k0 = std::clamp(static_cast<int>(std::floor(k)), 0, last_k);
  const int n = v.series[0].N();
  const int i1 = std::min(i0 + 1, std::max(0, n - 1)), k1 = std::min(k0 + 1, last_k);
  const double fi = i - i0, fk = k - k0;
  return (val(i0, k0) * (1 - fi) + val(i1, k0) * fi) * (1 - fk) + (val(i0, k1) * (1 - fi) + val(i1, k1) * fi) * fk;
}

void SceneSurface(S& s, const model::ChartObj& ch, const ChartValues& v, float L, float T, float R, float B, const Bands& bands) {
  using MT = model::ChartType;
  const size_t ns = v.series.size();
  int n = 0;
  for (const auto& se : v.series) n = std::max(n, se.N());
  if (n < 1 || ns < 1) return;
  const std::string fmt = !ch.val_axis.number_format.empty() ? ch.val_axis.number_format : v.series[0].fmt;
  const int nx = std::max(1, n - 1), nz = std::max(1, static_cast<int>(ns) - 1);
  const int sub = std::clamp(48 / std::max(nx, nz), 2, 8);  // sub-cells per grid cell, so the bands show
  if (ch.type == MT::SurfaceContour) {
    // seen from the top: the value bands as colored cells, categories across, series up
    float lw = 0;
    for (const auto& se : v.series) lw = std::max(lw, s.Width(se.name, s.axis_scale));
    const float x0 = L + std::min(lw, (R - L) / 4) + 8, x1 = R - 4, y0 = T + 4, y1 = B - s.fpx * s.axis_scale - 6;
    if (x1 - x0 < 8 || y1 - y0 < 8) return;
    const float cw = (x1 - x0) / static_cast<float>(nx * sub), chh = (y1 - y0) / static_cast<float>(nz * sub);
    s.Tag(kElemSeries, 0);
    for (int a = 0; a < nx * sub; ++a) {
      for (int b = 0; b < nz * sub; ++b) {
        const double val = GridAt(v, (a + 0.5) / sub, (b + 0.5) / sub);
        s.Rect(x0 + cw * static_cast<float>(a), y1 - chh * static_cast<float>(b + 1), x0 + cw * static_cast<float>(a + 1) + 0.5f, y1 - chh * static_cast<float>(b) + 0.5f,
               ChartPalette(bands.Of(val)));
      }
    }
    s.Tag(kElemPlotArea);
    s.Rect(x0, y0, x1, y1, 0, kAxis);
    if (ch.cat_axis.visible) {
      for (int i = 0; i < n; ++i) s.Tick(kElemCatAxis, Cat(v, static_cast<size_t>(i)), x0 + (x1 - x0) * (n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.5f), y1 + 3, 1, 0);
    }
    for (size_t k = 0; k < ns; ++k)
      s.Tick(kElemCatAxis, v.series[k].name, x0 - 4, y1 - (y1 - y0) * (ns > 1 ? static_cast<float>(k) / static_cast<float>(ns - 1) : 0.5f), 2, 1);
    return;
  }
  double rot, elev, persp, depth;
  View3DOf(ch, &rot, &elev, &persp, &depth);
  const double X = nx, Z = nz * std::clamp(depth, 20.0, 2000.0) / 100.0, Y = std::max(1.0, std::max(X, Z) * 0.6);
  float tick_w = 0;
  for (int k = 0; k <= static_cast<int>(bands.count); ++k) tick_w = std::max(tick_w, s.Width(FormatChartNumber(bands.lo + bands.step * k, fmt), s.axis_scale));
  const float fh = s.fpx * s.axis_scale;
  const Proj p = MakeProj(X, Y, Z, rot, elev, persp, L + tick_w + 8, T + fh * 0.5f, R - 60, B - fh - 6);
  auto yw = [&](double val) { return (std::clamp(val, bands.lo, bands.hi) - bands.lo) / (bands.hi - bands.lo) * Y; };
  auto zw = [&](double k) { return k / nz * Z; };
  // walls with the band gridlines and labels
  {
    Item3 w;
    const uint32_t wall = ch.wall_fill ? FillInk(ch.wall_fill) : ch.plot_fill ? FillInk(ch.plot_fill) : kWhite;
    const double side_x = p.sp >= 0 ? 0 : X;
    Quad(p, &w, {{0, 0, Z}, {X, 0, Z}, {X, Y, Z}, {0, Y, Z}}, wall, kGrid);
    Quad(p, &w, {{side_x, 0, 0}, {side_x, 0, Z}, {side_x, Y, Z}, {side_x, Y, 0}}, wall, kGrid);
    Quad(p, &w, {{0, 0, 0}, {X, 0, 0}, {X, 0, Z}, {0, 0, Z}}, ch.floor_fill ? FillInk(ch.floor_fill) : Mix(wall, Abgr(0xD9, 0xD9, 0xD9), 0.35f), kGrid);
    EmitWalls(s, w);
    for (size_t k = 0; k <= bands.count; ++k) {
      const double val = bands.lo + bands.step * static_cast<double>(k), y = yw(val);
      if (ch.val_axis.gridlines) {
        const auto a = p.P(0, y, Z), b = p.P(X, y, Z), c = p.P(side_x, y, 0);
        s.Line({c.first, c.second, p.P(side_x, y, Z).first, p.P(side_x, y, Z).second}, kGrid);
        s.Line({a.first, a.second, b.first, b.second}, kGrid);
      }
      if (ch.val_axis.visible) {
        const auto [x, yy] = p.P(side_x, y, 0);
        s.Tick(kElemValAxis, FormatChartNumber(val, fmt), side_x == 0 ? x - 4 : x + 4, yy, side_x == 0 ? 2 : 0, 1);
      }
    }
    if (ch.cat_axis.visible) {
      for (int i = 0; i < n; ++i) {
        const auto [x, y] = p.P(static_cast<double>(i), 0, 0);
        s.Tick(kElemCatAxis, Cat(v, static_cast<size_t>(i)), x, y + 3, 1, 0);
      }
    }
    for (size_t k = 0; k < ns; ++k) {
      const auto [x, y] = p.P(p.sp >= 0 ? X : 0, 0, zw(static_cast<double>(k)));
      s.Tick(kElemCatAxis, v.series[k].name, p.sp >= 0 ? x + 4 : x - 4, y, p.sp >= 0 ? 0 : 2, 1);
    }
  }
  // the surface: small quads colored by band, far ones first
  const bool wire = ch.type == MT::SurfaceWireframe;
  std::vector<Item3> items;
  for (int a = 0; a < nx * sub; ++a) {
    for (int b = 0; b < nz * sub; ++b) {
      const double i0 = static_cast<double>(a) / sub, i1 = static_cast<double>(a + 1) / sub, k0 = static_cast<double>(b) / sub, k1 = static_cast<double>(b + 1) / sub;
      const double v00 = GridAt(v, i0, k0), v10 = GridAt(v, i1, k0), v11 = GridAt(v, i1, k1), v01 = GridAt(v, i0, k1);
      const uint32_t col = ChartPalette(bands.Of((v00 + v10 + v11 + v01) / 4));
      Item3 it;
      it.series = 0;
      it.depth = p.D((i0 + i1) / 2, 0, zw((k0 + k1) / 2));
      Quad(p, &it, {{i0, yw(v00), zw(k0)}, {i1, yw(v10), zw(k0)}, {i1, yw(v11), zw(k1)}, {i0, yw(v01), zw(k1)}}, wire ? 0 : col,
           wire ? col : Mix(col, Abgr(0, 0, 0), 0.08f));
      items.push_back(std::move(it));
    }
  }
  EmitItems(s, items);
}

}  // namespace

// ---- calculations ----

std::vector<double> ChartSeriesValues::Finite() const {
  std::vector<double> out;
  const int n = N();
  const double* ys = Y();
  for (int i = 0; i < n; ++i) {
    if (std::isfinite(ys[i])) out.push_back(ys[i]);
  }
  return out;
}

double NiceStep(double range) {
  if (!(range > 0) || !std::isfinite(range)) return 1;
  const double raw = range / 5, mag = std::pow(10.0, std::floor(std::log10(raw))), f = raw / mag;
  return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}

HistBins Histogram(const std::vector<double>& values, double bin_width, int bin_count) {
  HistBins h;
  std::vector<double> v;
  for (double x : values) {
    if (std::isfinite(x)) v.push_back(x);
  }
  if (v.empty()) return h;
  const double mn = *std::min_element(v.begin(), v.end()), mx = *std::max_element(v.begin(), v.end());
  double w = 0;
  int k = 0;
  if (bin_width > 0) {
    w = bin_width;
  } else if (bin_count > 0) {
    k = std::min(bin_count, 1000);
    w = mx > mn ? (mx - mn) / k : 1;
  } else {
    w = 3.5 * StdevS(v) / std::cbrt(static_cast<double>(v.size()));  // Scott's rule
  }
  if (!(w > 0) || !std::isfinite(w)) w = 1;
  if (k == 0) k = mx > mn ? static_cast<int>(std::ceil((mx - mn) / w - 1e-9)) : 1;
  k = std::clamp(k, 1, 1000);
  h.edges.resize(static_cast<size_t>(k) + 1);
  for (int i = 0; i <= k; ++i) h.edges[static_cast<size_t>(i)] = mn + w * i;
  h.counts.assign(static_cast<size_t>(k), 0.0);
  for (double x : v) {
    int i = x <= mn + w ? 0 : static_cast<int>(std::ceil((x - mn) / w - 1e-12)) - 1;
    i = std::clamp(i, 0, k - 1);
    h.counts[static_cast<size_t>(i)] += 1;
  }
  for (int i = 0; i < k; ++i) {
    h.labels.push_back((i == 0 ? "[" : "(") + Short(h.edges[static_cast<size_t>(i)]) + ", " + Short(h.edges[static_cast<size_t>(i) + 1]) + "]");
  }
  return h;
}

HistBins HistogramOf(const std::vector<double>& values, const model::ChartObj& ch) {
  if (ch.bins_by_category) {
    // By Category: each distinct value is a bin of its own, counted (Excel's Bins > By Category)
    std::map<double, double> n;
    for (double x : values) {
      if (std::isfinite(x)) n[x] += 1;
    }
    HistBins h;
    for (const auto& [x, c] : n) {
      h.edges.push_back(x);
      h.counts.push_back(c);
      h.labels.push_back(Short(x));
    }
    if (!n.empty()) h.edges.push_back(n.rbegin()->first);
    return h;
  }
  if (!ch.bins_overflow && !ch.bins_underflow) return Histogram(values, ch.bin_width, ch.bin_count);
  // the values beyond the overflow / underflow values go into their own bins; the others are binned between them
  std::vector<double> mid;
  double over = 0, under = 0;
  for (double x : values) {
    if (!std::isfinite(x)) continue;
    if (ch.bins_overflow && x > ch.overflow_at) over += 1;
    else if (ch.bins_underflow && x <= ch.underflow_at) under += 1;
    else mid.push_back(x);
  }
  HistBins h = mid.empty() ? HistBins{} : Histogram(mid, ch.bin_width, ch.bin_count);
  if (ch.bins_underflow) {
    h.counts.insert(h.counts.begin(), under);
    h.labels.insert(h.labels.begin(), "<=" + Short(ch.underflow_at));
    h.edges.insert(h.edges.begin(), ch.underflow_at);
    if (h.labels.size() > 1 && h.labels[1].size() > 1 && h.labels[1][0] == '[') h.labels[1][0] = '(';  // the next bin starts above it
  }
  if (ch.bins_overflow) {
    h.counts.push_back(over);
    h.labels.push_back(">" + Short(ch.overflow_at));
    h.edges.push_back(ch.overflow_at);
  }
  return h;
}

BoxStats BoxWhisker(std::vector<double> v, bool inclusive) {
  BoxStats b;
  v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
  if (v.empty()) return b;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  auto q = [&](double p) {
    if (inclusive) {  // QUARTILE.INC: the median counted in both halves
      const double pos = p * static_cast<double>(n - 1);
      const size_t i = static_cast<size_t>(std::floor(pos));
      if (i + 1 >= n) return v.back();
      return v[i] + (pos - static_cast<double>(i)) * (v[i + 1] - v[i]);
    }
    const double pos = p * static_cast<double>(n + 1);
    if (pos <= 1) return v.front();
    if (pos >= static_cast<double>(n)) return v.back();
    const size_t i = static_cast<size_t>(std::floor(pos));
    const double f = pos - static_cast<double>(i);
    return v[i - 1] + f * (v[i] - v[i - 1]);
  };
  b.q1 = q(0.25);
  b.q3 = q(0.75);
  b.median = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
  b.mean = Mean(v);
  const double iqr = b.q3 - b.q1, lf = b.q1 - 1.5 * iqr, hf = b.q3 + 1.5 * iqr;
  b.lo = b.q1;
  b.hi = b.q3;
  bool lo_set = false;
  for (double x : v) {
    if (x < lf || x > hf) {
      b.outliers.push_back(x);
      continue;
    }
    if (!lo_set) b.lo = x, lo_set = true;
    b.hi = x;
  }
  b.lo = std::min(b.lo, b.q1);
  b.hi = std::max(b.hi, b.q3);
  b.ok = true;
  return b;
}

std::vector<WaterfallBar> Waterfall(const std::vector<double>& values, const std::vector<int>& totals) {
  std::vector<WaterfallBar> out;
  double run = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    const double v = std::isfinite(values[i]) ? values[i] : 0;
    WaterfallBar b;
    if (std::find(totals.begin(), totals.end(), static_cast<int>(i)) != totals.end()) {
      b.start = 0;
      b.end = v;
      b.kind = 2;
      run = v;
    } else {
      b.start = run;
      b.end = run + v;
      b.kind = v >= 0 ? 0 : 1;
      run = b.end;
    }
    out.push_back(b);
  }
  return out;
}

TrendFit FitTrend(const double* xs, const double* ys, int n, model::TrendKind kind, int order, const double* intercept) {
  using K = model::TrendKind;
  TrendFit f;
  f.kind = kind;
  if (kind == K::None || kind == K::MovingAverage) return f;
  // Set Intercept: linear, polynomial and exponential trendlines (an exponential one's factor must be positive)
  const bool icept = intercept && (kind == K::Linear || kind == K::Polynomial || (kind == K::Exponential && *intercept > 0));
  const double b0 = icept ? (kind == K::Exponential ? std::log(*intercept) : *intercept) : 0.0;
  std::vector<double> X, Y;
  for (int i = 0; i < n; ++i) {
    double x = xs[i], y = ys[i];
    if (!std::isfinite(x) || !std::isfinite(y)) continue;
    if (kind == K::Logarithmic || kind == K::Power) {
      if (x <= 0) return f;  // Excel: not available with zero or negative X
      x = std::log(x);
    }
    if (kind == K::Exponential || kind == K::Power) {
      if (y <= 0) return f;
      y = std::log(y);
    }
    X.push_back(x);
    Y.push_back(y);
  }
  const int deg = kind == K::Polynomial ? std::clamp(order, 2, 6) : 1;
  std::vector<double> c;
  if (icept) {
    std::vector<double> Y0(Y);
    for (double& y : Y0) y -= b0;
    if (!PolyFit(X, Y0, deg, &c, 1)) return f;
    c[0] = b0;
  } else if (!PolyFit(X, Y, deg, &c)) {
    return f;
  }
  // R-squared in the fitted (possibly logarithmic) space, as Excel reports it
  const double my = Mean(Y);
  double ss_res = 0, ss_tot = 0;
  for (size_t i = 0; i < X.size(); ++i) {
    double p = 0, pw = 1;
    for (double ck : c) p += ck * pw, pw *= X[i];
    ss_res += (Y[i] - p) * (Y[i] - p);
    ss_tot += (Y[i] - my) * (Y[i] - my);
  }
  f.r2 = ss_tot > 0 ? 1 - ss_res / ss_tot : 1;
  if (kind == K::Exponential || kind == K::Power) c[0] = std::exp(c[0]);
  f.c = std::move(c);
  f.ok = true;
  return f;
}

double TrendFit::Eval(double x) const {
  using K = model::TrendKind;
  if (!ok || c.size() < 2) return kNaN;
  switch (kind) {
    case K::Linear: return c[0] + c[1] * x;
    case K::Exponential: return c[0] * std::exp(c[1] * x);
    case K::Logarithmic: return x > 0 ? c[0] + c[1] * std::log(x) : kNaN;
    case K::Power: return x > 0 ? c[0] * std::pow(x, c[1]) : kNaN;
    case K::Polynomial: {
      double p = 0, pw = 1;
      for (double ck : c) p += ck * pw, pw *= x;
      return p;
    }
    default: return kNaN;
  }
}

std::string TrendFit::Equation() const {
  using K = model::TrendKind;
  if (!ok || c.size() < 2) return {};
  auto num = [](double v) { return Short(v, 5); };
  // " + 3" / " - 3" (nothing for a zero constant: a trendline with its intercept set to 0 reads "y = 2x")
  auto term = [&](double v, const std::string& suffix) { return v == 0 && suffix.empty() ? std::string() : (v < 0 ? " - " : " + ") + num(std::fabs(v)) + suffix; };
  switch (kind) {
    case K::Linear: return "y = " + num(c[1]) + "x" + term(c[0], "");
    case K::Exponential: return "y = " + num(c[0]) + "e^(" + num(c[1]) + "x)";
    case K::Logarithmic: return "y = " + num(c[1]) + "ln(x)" + term(c[0], "");
    case K::Power: return "y = " + num(c[0]) + "x^" + num(c[1]);
    case K::Polynomial: {
      std::string s = "y = ";
      bool first = true;
      for (size_t k = c.size(); k-- > 0;) {
        const std::string suf = k == 0 ? "" : (k == 1 ? "x" : "x^" + std::to_string(k));
        if (first) {
          s += num(c[k]) + suf;
          first = false;
        } else {
          s += term(c[k], suf);
        }
      }
      return s;
    }
    default: return {};
  }
}

std::vector<double> MovingAverage(const double* ys, int n, int period) {
  std::vector<double> out(static_cast<size_t>(std::max(0, n)), kNaN);
  period = std::max(2, period);
  for (int i = period - 1; i < n; ++i) {
    double s = 0;
    bool ok = true;
    for (int k = i - period + 1; k <= i; ++k) {
      if (!std::isfinite(ys[k])) ok = false;
      s += ys[k];
    }
    if (ok) out[static_cast<size_t>(i)] = s / period;
  }
  return out;
}

std::vector<double> ErrorAmounts(model::ErrorBarKind kind, double amount, const double* ys, int n, bool* at_mean, double* mean) {
  using K = model::ErrorBarKind;
  *at_mean = false;
  std::vector<double> fin;
  for (int i = 0; i < n; ++i) {
    if (std::isfinite(ys[i])) fin.push_back(ys[i]);
  }
  *mean = Mean(fin);
  std::vector<double> e(static_cast<size_t>(std::max(0, n)), 0.0);
  for (int i = 0; i < n; ++i) {
    switch (kind) {
      case K::Fixed: e[static_cast<size_t>(i)] = std::fabs(amount); break;
      case K::Percent: e[static_cast<size_t>(i)] = std::isfinite(ys[i]) ? std::fabs(ys[i]) * std::fabs(amount) / 100 : 0; break;
      case K::StDev:
        e[static_cast<size_t>(i)] = std::fabs(amount) * StdevS(fin);
        *at_mean = true;
        break;
      case K::StError: e[static_cast<size_t>(i)] = fin.size() > 1 ? StdevS(fin) / std::sqrt(static_cast<double>(fin.size())) : 0; break;
      default: break;
    }
  }
  return e;
}

std::vector<double> ParseNumberList(const std::string& text) {
  std::vector<double> out;
  std::string t = text;
  if (!t.empty() && t[0] == '=') t.erase(0, 1);
  const bool braces = !t.empty() && t.front() == '{' && t.back() == '}';
  if (braces) t = t.substr(1, t.size() - 2);
  size_t p = 0;
  while (p <= t.size()) {
    size_t e = t.find_first_of(",;", p);
    if (e == std::string::npos) e = t.size();
    const std::string part = t.substr(p, e - p);
    char* end = nullptr;
    const double v = std::strtod(part.c_str(), &end);
    if (part.empty() || !end || *end != 0) {
      bool blank = true;
      for (char c : part) blank = blank && c == ' ';
      if (!blank || !braces) return {};
    } else {
      out.push_back(v);
    }
    p = e + 1;
  }
  return out;
}

void ErrorExtents(const model::ChartSeries& d, const ChartSeriesValues& b, int n, std::vector<double>* minus, std::vector<double>* plus, bool* at_mean, double* mean) {
  const size_t m = static_cast<size_t>(std::max(0, n));
  *at_mean = false;
  *mean = 0;
  if (d.err == model::ErrorBarKind::Custom) {
    // one value applies to every point
    auto take = [&](const std::vector<double>& v, std::vector<double>* out) {
      out->assign(m, 0.0);
      for (size_t i = 0; i < m && !v.empty(); ++i) {
        const double x = v.size() == 1 ? v[0] : (i < v.size() ? v[i] : 0.0);
        (*out)[i] = std::isfinite(x) ? std::fabs(x) : 0.0;
      }
    };
    take(b.err_plus, plus);
    take(b.err_minus, minus);
  } else {
    *plus = ErrorAmounts(d.err, d.err_amount, b.Y(), n, at_mean, mean);
    *minus = *plus;
  }
  if (d.err_include == 2) std::fill(minus->begin(), minus->end(), 0.0);
  if (d.err_include == 3) std::fill(plus->begin(), plus->end(), 0.0);
}

void ErrorExtentsX(const model::ChartSeries& d, const ChartSeriesValues& b, int n, std::vector<double>* minus, std::vector<double>* plus, bool* at_mean, double* mean) {
  model::ChartSeries t;
  t.err = d.xerr;
  t.err_amount = d.xerr_amount;
  t.err_include = d.xerr_include;
  ChartSeriesValues v;
  const int m = std::max(0, n);
  v.y.assign(b.X(), b.X() + m);
  v.x = v.y;
  v.err_plus = b.xerr_plus;
  v.err_minus = b.xerr_minus;
  ErrorExtents(t, v, n, minus, plus, at_mean, mean);
}

std::vector<TreeRect> TreemapLayout(const std::vector<std::vector<std::string>>& paths, const std::vector<double>& values, double x0, double y0, double x1, double y1,
                                    double banner_h) {
  std::vector<TreeRect> out;
  const std::vector<Node> t = BuildTree(paths, values);
  if (!(t[0].value > 0) || x1 <= x0 || y1 <= y0) return out;
  LayoutTree(t, 0, 0, 0, Box{x0, y0, x1, y1}, banner_h, &out);
  return out;
}

std::vector<SunSegment> SunburstLayout(const std::vector<std::vector<std::string>>& paths, const std::vector<double>& values, int* levels) {
  std::vector<SunSegment> out;
  *levels = 0;
  const std::vector<Node> t = BuildTree(paths, values);
  LayoutSun(t, 0, 0, 0, 0, 2 * kPi, levels, &out);
  return out;
}

namespace {
thread_local int g_scheme = 0;  // the palette of the chart being drawn
}  // namespace

PaletteScope::PaletteScope(int scheme) : prev(g_scheme) { g_scheme = std::clamp(scheme, 0, kChartColorSchemes - 1); }
PaletteScope::~PaletteScope() { g_scheme = prev; }

const char* ChartColorSchemeName(int scheme) {
  static const char* kNames[] = {TR("Colorful Palette 1"),      TR("Colorful Palette 2"),      TR("Colorful Palette 3"),      TR("Colorful Palette 4"),
                                 TR("Monochromatic Palette 1"), TR("Monochromatic Palette 2"), TR("Monochromatic Palette 3"), TR("Monochromatic Palette 4"),
                                 TR("Monochromatic Palette 5"), TR("Monochromatic Palette 6")};
  return kNames[std::clamp(scheme, 0, kChartColorSchemes - 1)];
}

uint32_t ChartPalette(size_t i) {
  // the theme's six accents, then darker and lighter rounds of them (like Excel); other palettes take the accents in
  // another order, or shades of one accent
  const model::Theme& th = model::CurrentTheme();
  const int scheme = g_scheme;
  uint32_t c = 0;
  if (scheme >= 4) {
    static constexpr double kShades[] = {0.0, -0.5, 0.4, -0.25, 0.6, -0.75, 0.2, 0.8};
    c = model::ApplyTint(th.colors[model::kAccent1 + (scheme - 4) % 6], kShades[i % 8]);
  } else {
    static constexpr int kOrder[4][6] = {{0, 1, 2, 3, 4, 5}, {0, 2, 4, 1, 3, 5}, {1, 3, 5, 0, 2, 4}, {5, 4, 3, 2, 1, 0}};
    const size_t round = (i / 6) % 4;
    static constexpr double kTints[] = {0.0, -0.4, 0.4, -0.6};
    c = model::ApplyTint(th.colors[model::kAccent1 + kOrder[scheme][i % 6]], kTints[round]);
  }
  return Abgr((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

uint32_t SeriesColor(const model::ChartObj& ch, const ChartValues& v, size_t i) {
  uint32_t c = i < v.series.size() ? v.series[i].color : 0;
  if (!c && i < ch.series.size()) c = ch.series[i].color;
  return c ? ((c & 0x00FFFFFFu) | 0xFF000000u) : ChartPalette(i);
}

std::string FormatChartNumber(double v, const std::string& fmt) {
  if (!std::isfinite(v)) return "#NUM!";
  return DisplayTextOf(core::Value::Number(v), fmt.empty() ? std::string_view("General") : std::string_view(fmt));
}

// ---- the date axis ----

// a date counted in days, months or years since the start of the 1900 date system, and back
int64_t DateUnit(double serial, int unit) {
  if (unit <= 0) return static_cast<int64_t>(std::floor(serial));
  int y = 1900, m = 1, d = 1;
  formula::SerialToYmd(std::floor(serial), &y, &m, &d);
  return unit == 1 ? static_cast<int64_t>(y) * 12 + (m - 1) : y;
}
double UnitSerial(int64_t u, int unit) {
  if (unit <= 0) return static_cast<double>(u);
  if (unit == 1) return formula::DateSerial(u / 12, u % 12 + 1, 1);
  return formula::DateSerial(u, 1, 1);
}

DatePlan PlanDateAxis(const model::ChartObj& ch, const ChartValues& v, size_t n) {
  DatePlan p;
  const bool dates = ch.cat_type == 2 || (ch.cat_type == 0 && v.cat_dates);
  if (!dates || v.cat_vals.empty() || IsScatterType(ch.type) || IsSceneChart(ch.type)) return p;
  std::vector<double> d;
  for (size_t k = 0; k < v.cat_vals.size() && k < n; ++k) {
    if (std::isfinite(v.cat_vals[k]) && v.cat_vals[k] >= 1) d.push_back(std::floor(v.cat_vals[k]));
  }
  if (d.empty()) return p;
  // Base Unit automatic: the smallest step between the dates (a month or more: months; all on 1 January: years)
  int unit = ch.base_unit;
  if (unit < 0) {  // (one pass: drawn every frame, also for long series)
    double step = 1e300;
    bool new_year = true, several = false;
    for (size_t k = 0; k < d.size(); ++k) {
      if (k > 0 && d[k] != d[k - 1]) step = std::min(step, std::fabs(d[k] - d[k - 1])), several = true;
      if (new_year && step >= 365) {
        int y = 0, m = 0, dd = 0;
        formula::SerialToYmd(d[k], &y, &m, &dd);
        new_year = m == 1 && dd == 1;
      }
    }
    unit = several && new_year && step >= 365 ? 2 : (several && step >= 28 ? 1 : 0);
  }
  p.unit = unit;
  int64_t lo = std::numeric_limits<int64_t>::max(), hi = std::numeric_limits<int64_t>::min();
  for (double x : d) lo = std::min(lo, DateUnit(x, unit)), hi = std::max(hi, DateUnit(x, unit));
  // bounds as dates (Format Axis > Bounds on a date axis)
  if (!ch.cat_axis.min_auto && ch.cat_axis.min >= 1) lo = DateUnit(ch.cat_axis.min, unit);
  if (!ch.cat_axis.max_auto && ch.cat_axis.max >= 1) hi = DateUnit(ch.cat_axis.max, unit);
  if (hi < lo || hi - lo > 200000) return p;
  p.on = true;
  p.u0 = lo;
  p.u1 = hi;
  p.pos.assign(std::min(n, v.cat_vals.size()), std::numeric_limits<double>::quiet_NaN());
  for (size_t k = 0; k < p.pos.size(); ++k) {
    if (std::isfinite(v.cat_vals[k]) && v.cat_vals[k] >= 1) p.pos[k] = static_cast<double>(DateUnit(v.cat_vals[k], unit) - lo + 1);
  }
  return p;
}

bool IsSceneChart(model::ChartType t) {
  using T = model::ChartType;
  switch (t) {
    case T::Radar:
    case T::RadarMarkers:
    case T::RadarFilled:
    case T::Waterfall:
    case T::Histogram:
    case T::Pareto:
    case T::BoxWhisker:
    case T::Treemap:
    case T::Sunburst:
    case T::Funnel:
    case T::PieOfPie:
    case T::BarOfPie:
    case T::FilledMap: return true;
    default: return model::Is3DChart(t);
  }
}

namespace {
bool Comboable(model::ChartType t) {
  using T = model::ChartType;
  return t == T::Line || t == T::LineMarkers || t == T::ColumnClustered || t == T::ColumnStacked || t == T::Area || t == T::Scatter;
}
}  // namespace

model::ChartType EffectiveSeriesType(const model::ChartObj& ch, size_t i) {
  if (!Comboable(ch.type) || i >= ch.series.size()) return ch.type;
  const int t = ch.series[i].type;
  if (t < 0 || t >= model::kChartTypeCount || !Comboable(static_cast<model::ChartType>(t))) return ch.type;
  return static_cast<model::ChartType>(t);
}

bool IsBarType(model::ChartType t) {
  using T = model::ChartType;
  return t == T::ColumnClustered || t == T::ColumnStacked || t == T::BarClustered || t == T::ColumnStacked100 || t == T::BarStacked || t == T::BarStacked100;
}

bool IsHorizontalBar(model::ChartType t) {
  using T = model::ChartType;
  return t == T::BarClustered || t == T::BarStacked || t == T::BarStacked100;
}

bool IsStackedType(model::ChartType t) {
  using T = model::ChartType;
  switch (t) {
    case T::ColumnStacked:
    case T::ColumnStacked100:
    case T::BarStacked:
    case T::BarStacked100:
    case T::LineStacked:
    case T::LineStacked100:
    case T::LineMarkersStacked:
    case T::LineMarkersStacked100:
    case T::AreaStacked:
    case T::AreaStacked100: return true;
    default: return false;
  }
}

bool IsPercentType(model::ChartType t) {
  using T = model::ChartType;
  return t == T::ColumnStacked100 || t == T::BarStacked100 || t == T::LineStacked100 || t == T::LineMarkersStacked100 || t == T::AreaStacked100;
}

bool IsLineType(model::ChartType t) {
  using T = model::ChartType;
  return t == T::Line || t == T::LineMarkers || t == T::LineStacked || t == T::LineStacked100 || t == T::LineMarkersStacked || t == T::LineMarkersStacked100;
}

bool IsAreaType(model::ChartType t) {
  using T = model::ChartType;
  return t == T::Area || t == T::AreaStacked || t == T::AreaStacked100;
}

bool IsScatterType(model::ChartType t) {
  using T = model::ChartType;
  return t == T::Scatter || t == T::ScatterLines || t == T::ScatterLinesNoMarkers || t == T::ScatterSmooth || t == T::ScatterSmoothNoMarkers || t == T::Bubble;
}

bool HasMarkers(model::ChartType t) {
  using T = model::ChartType;
  return t == T::LineMarkers || t == T::LineMarkersStacked || t == T::LineMarkersStacked100 || t == T::Scatter || t == T::ScatterLines || t == T::ScatterSmooth;
}

bool IsStockType(model::ChartType t) {
  using T = model::ChartType;
  return t == T::StockOHLC || t == T::StockHLC || t == T::StockVHLC || t == T::StockVOHLC;
}

void BuildChartScene(const model::ChartObj& ch, const ChartValues& v, float w, float h, float font, const MeasureText& measure, ChartScene* out) {
  using T = model::ChartType;
  out->prims.clear();
  S s{out, font, measure};
  s.axis_ink = TextInk(ch.axis_font.color);
  s.axis_scale = ch.axis_font.size > 0 ? ch.axis_font.size / 9.0f : 1.0f;
  const float pad = std::max(4.0f, font * 0.6f);
  float L = pad, Tp = pad, R = w - pad, B = h - pad;
  const std::string& title = !v.title.empty() ? v.title : ch.title;
  if (ch.has_title && !title.empty()) {
    const float sc = (ch.title_font.size > 0 ? ch.title_font.size : 14.0f) / 9.0f;
    s.Tag(kElemTitle);
    s.Text(title, w / 2, Tp, 1, 0, TextInk(ch.title_font.color), sc, ch.title_font.bold);
    if (!ch.title_overlay) Tp += font * sc + 4;
    s.Tag(kElemPlotArea);
  }
  LegendStyle ls;
  ls.pos = ch.legend_pos;
  ls.ink = TextInk(ch.legend_font.color);
  ls.scale = ch.legend_font.size > 0 ? ch.legend_font.size / 9.0f : 1.0f;
  const bool surface = ch.type == T::Surface || ch.type == T::SurfaceWireframe || ch.type == T::SurfaceContour;
  Bands bands;
  if (surface) bands = SurfaceBands(ch, v);
  // legend entries known before layout
  std::vector<LegendItem> legend;
  const bool hier = ch.type == T::Treemap || ch.type == T::Sunburst;
  if (ch.has_legend && !hier) {
    if (ch.type == T::Waterfall) {
      legend = {{TR("Increase"), ChartPalette(0)}, {TR("Decrease"), ChartPalette(1)}, {TR("Total"), ChartPalette(2)}};
    } else if (ch.type == T::Pareto) {
      if (const ChartSeriesValues* se = First(v)) legend = {{se->name, SeriesColor(ch, v, static_cast<size_t>(se - v.series.data()))}, {TR("Cumulative Percent"), ChartPalette(1), true}};
    } else if (ch.type == T::Histogram || ch.type == T::Funnel) {
      if (const ChartSeriesValues* se = First(v)) legend = {{se->name, SeriesColor(ch, v, static_cast<size_t>(se - v.series.data()))}};
    } else if (ch.type == T::Pie3D || ch.type == T::PieOfPie || ch.type == T::BarOfPie) {
      if (const ChartSeriesValues* se = First(v)) {
        for (int i = 0; i < se->N() && i < 64; ++i) legend.push_back({Cat(v, static_cast<size_t>(i)), ChartPalette(static_cast<size_t>(i))});
      }
    } else if (ch.type == T::FilledMap) {
      // the map draws its own color scale or categories
    } else if (surface) {
      legend = BandLegend(bands, !ch.val_axis.number_format.empty() ? ch.val_axis.number_format : (v.series.empty() ? std::string() : v.series[0].fmt));
    } else {
      for (size_t i = 0; i < v.series.size(); ++i) {
        if (i < ch.series.size() && ch.series[i].filtered) continue;  // Chart Filters
        legend.push_back({v.series[i].name, SeriesColor(ch, v, i), ch.type == T::Radar || ch.type == T::RadarMarkers});
      }
    }
    Legend(s, legend, &L, &Tp, &R, &B, ls);
  }
  // axis titles: the vertical axis' turned on its side at the left, the horizontal axis' under the labels
  const bool axes = ch.type != T::Pie3D && ch.type != T::PieOfPie && ch.type != T::BarOfPie && ch.type != T::FilledMap && !hier && ch.type != T::Radar && ch.type != T::RadarMarkers && ch.type != T::RadarFilled;
  if (axes) {
    const bool bars = ch.type == T::BarClustered3D || ch.type == T::Funnel;
    const model::ChartAxisFormat& left = bars ? ch.cat_axis : ch.val_axis;
    const model::ChartAxisFormat& bottom = bars ? ch.val_axis : ch.cat_axis;
    const std::string& cat_t = v.cat_title.empty() ? ch.cat_axis.title : v.cat_title;
    const std::string& val_t = v.val_title.empty() ? ch.val_axis.title : v.val_title;
    const std::string& left_t = bars ? cat_t : val_t;
    const std::string& bottom_t = bars ? val_t : cat_t;
    const float th = font * s.axis_scale * 1.3f;
    if (bottom.has_title && !bottom_t.empty() && ch.type != T::Funnel) {
      s.Tag(bars ? kElemValAxisTitle : kElemCatAxisTitle);
      s.Text(bottom_t, (L + R) / 2, B, 1, 2, s.axis_ink, s.axis_scale);
      B -= th;
    }
    if (left.has_title && !left_t.empty()) {
      s.Tag(bars ? kElemCatAxisTitle : kElemValAxisTitle);
      s.Text(left_t, L + th / 2, (Tp + B) / 2, 1, 1, s.axis_ink, s.axis_scale);
      out->prims.back().rotate = 90;
      L += th;
    }
    s.Tag(kElemPlotArea);
  }
  if (R - L < 10 || B - Tp < 10) return;
  switch (ch.type) {
    case T::Radar:
    case T::RadarMarkers:
    case T::RadarFilled: SceneRadar(s, ch, v, L, Tp, R, B); break;
    case T::Waterfall: SceneWaterfall(s, ch, v, L, Tp, R, B); break;
    case T::Histogram: SceneHistogram(s, ch, v, L, Tp, R, B); break;
    case T::Pareto: ScenePareto(s, ch, v, L, Tp, R, B); break;
    case T::BoxWhisker: SceneBox(s, ch, v, L, Tp, R, B); break;
    case T::Funnel:
    case T::Column3D:
    case T::ColumnClustered3D:
    case T::ColumnStacked3D:
    case T::BarClustered3D:
    case T::Line3D:
    case T::Area3D: {
      const size_t n = MostPoints(v);
      const ChartValues thin = n > kMaxDrawnCategories ? Thinned(v, (n + kMaxDrawnCategories - 1) / kMaxDrawnCategories) : ChartValues{};
      const ChartValues& drawn = n > kMaxDrawnCategories ? thin : v;
      if (ch.type == T::Funnel) SceneFunnel(s, ch, drawn, L, Tp, R, B);
      else SceneCartesian3D(s, ch, drawn, L, Tp, R, B);
      break;
    }
    case T::Pie3D: ScenePie3D(s, ch, v, L, Tp, R, B); break;
    case T::PieOfPie:
    case T::BarOfPie: ScenePieOfPie(s, ch, v, L, Tp, R, B); break;
    case T::Surface:
    case T::SurfaceWireframe:
    case T::SurfaceContour: {
      const size_t n = MostPoints(v);
      if (n > kMaxDrawnCategories) SceneSurface(s, ch, Thinned(v, (n + kMaxDrawnCategories - 1) / kMaxDrawnCategories), L, Tp, R, B, bands);
      else SceneSurface(s, ch, v, L, Tp, R, B, bands);
      break;
    }
    case T::FilledMap: SceneMap(s, ch, v, L, Tp, R, B); break;
    case T::Treemap:
    case T::Sunburst: {
      // the legend (top-level categories) is only known after the layout: lay out once without it, then again with room for it
      std::vector<LegendItem> tops;
      ChartScene tmp;
      S probe{&tmp, font, measure};
      if (ch.type == T::Treemap) SceneTreemap(probe, ch, v, L, Tp, R, B, &tops);
      else SceneSunburst(probe, ch, v, L, Tp, R, B, &tops);
      if (ch.has_legend && !tops.empty()) {
        Legend(s, tops, &L, &Tp, &R, &B, ls);
        if (B - Tp < 10 || R - L < 10) return;
        tmp.prims.clear();
        if (ch.type == T::Treemap) SceneTreemap(probe, ch, v, L, Tp, R, B, nullptr);
        else SceneSunburst(probe, ch, v, L, Tp, R, B, nullptr);
      }
      out->prims.insert(out->prims.end(), std::make_move_iterator(tmp.prims.begin()), std::make_move_iterator(tmp.prims.end()));
      break;
    }
    default: break;
  }
}

// ---- Quick Layout and Chart Styles (Chart Design) ----

namespace {
struct QuickLayout {
  bool title;
  int legend;  // -1 none, else ChartObj::legend_pos
  bool cat_title, val_title;
  int labels;  // -2 none, -1 automatic position, else ChartSeries::label_pos
  bool val_axis, grid;
  int gap;  // -1 kept
};
const QuickLayout kLayouts[kChartQuickLayouts] = {
    {true, 1, false, false, -2, true, true, -1},
    {true, 2, false, false, 3, false, false, -1},
    {true, 0, false, false, -2, true, true, -1},
    {false, 0, false, false, 3, true, true, -1},
    {true, 0, false, true, -2, true, true, -1},
    {true, -1, false, true, -2, true, true, -1},
    {false, 1, true, true, -2, true, true, -1},
    {true, -1, true, true, -2, true, false, 0},
    {true, 1, true, true, -2, true, true, -1},
    {true, 1, false, false, 1, true, true, 50},
    {false, 4, false, false, -2, true, true, -1},
};

// a Chart Style's look (Excel's Chart Styles gallery: each chart type has its own set)
struct Look {
  int gap = -1;          // columns and bars: Gap Width (-1 kept)
  int labels = -2;       // data labels: -2 none, -1 automatic place, else a position (columns, bars, pies)
  bool val_axis = true, grid = true, minor = false;
  uint32_t chart_fill = 0, plot_fill = 0, text = 0;  // ARGB, 0 = automatic
  int transparency = 0;  // series fill (%)
  uint32_t border = 0;   // series border (Excel BGR, 0 none)
  float weight = 0;      // lines (points, 0 automatic)
  int marker = -1;       // lines and scatter: -1 the type's, 0 none, 1 circle, 2 square, 3 diamond, 4 triangle
  float marker_size = 0;
  int fill = 0;          // 0 solid, 1 gradient, 3 pattern
  bool outline = false;  // hollow: no fill, a border in the series' color
  int legend = -9;       // -9 kept, -1 none, else a legend position
  int explosion = -1;    // pies: every slice pulled out (%), -1 none
  bool smooth = false;
  int dash = 0;
};
constexpr uint32_t kGrayBg = 0xFFF2F2F2, kDark = 0xFF404040, kDarker = 0xFF262626, kWhiteText = 0xFFFFFFFF, kGrayLine = 0x404040, kWhiteLine = 0xFFFFFF;

// the sets, one per kind of chart (the number Excel's Chart.ChartStyle gives Style 1 of each, then one per style)
const std::vector<Look>& ColumnLooks() {
  static const std::vector<Look> v = [] {
    std::vector<Look> l(14);
    l[0].gap = 219;
    l[1].gap = 80, l[1].labels = 1, l[1].val_axis = false, l[1].grid = false;
    l[2].gap = 150, l[2].fill = 3;
    l[3].gap = 50, l[3].grid = false;
    l[4].gap = 150, l[4].plot_fill = kGrayBg;
    l[5].gap = 219, l[5].minor = true, l[5].border = kGrayLine;
    l[6].gap = 100, l[6].labels = 3, l[6].grid = false, l[6].chart_fill = kGrayBg;
    l[7].gap = 100, l[7].chart_fill = kDark, l[7].text = kWhiteText;
    l[8].gap = 100, l[8].outline = true;
    l[9].gap = 100, l[9].fill = 1;
    l[10].gap = 219, l[10].transparency = 40, l[10].border = kGrayLine;
    l[11].gap = 60, l[11].chart_fill = kDarker, l[11].text = kWhiteText, l[11].fill = 1;
    l[12].gap = 50, l[12].labels = 0, l[12].val_axis = false, l[12].grid = false;
    l[13].gap = 100, l[13].chart_fill = kDark, l[13].text = kWhiteText, l[13].fill = 3;
    return l;
  }();
  return v;
}
const std::vector<Look>& LineLooks() {
  static const std::vector<Look> v = [] {
    std::vector<Look> l(15);
    l[1].labels = -1, l[1].marker = 1, l[1].val_axis = false, l[1].grid = false;
    l[2].weight = 3.5f;
    l[3].weight = 1, l[3].smooth = true;
    l[4].plot_fill = kGrayBg, l[4].marker = 1;
    l[5].chart_fill = kDark, l[5].text = kWhiteText, l[5].weight = 2.25f;
    l[6].marker = 4, l[6].marker_size = 7, l[6].grid = false;
    l[7].minor = true, l[7].weight = 1.5f;
    l[8].chart_fill = kGrayBg, l[8].labels = -1, l[8].marker = 1;
    l[9].dash = 3;
    l[10].chart_fill = kDarker, l[10].text = kWhiteText, l[10].marker = 1;
    l[11].weight = 4.5f, l[11].grid = false;
    l[12].marker = 2, l[12].marker_size = 6, l[12].plot_fill = kGrayBg;
    l[13].weight = 2.25f, l[13].labels = -1, l[13].val_axis = false;
    l[14].smooth = true, l[14].weight = 2.75f, l[14].chart_fill = kGrayBg;
    return l;
  }();
  return v;
}
const std::vector<Look>& PieLooks() {
  static const std::vector<Look> v = [] {
    std::vector<Look> l(12);
    for (auto& x : l) x.border = kWhiteLine;
    l[1].labels = 8, l[1].legend = -1;
    l[2].explosion = 10;
    l[3].chart_fill = kDark, l[3].text = kWhiteText;
    l[4].weight = 3, l[4].labels = 0;
    l[5].chart_fill = kGrayBg, l[5].legend = 1;
    l[6].border = 0;
    l[7].fill = 1;
    l[8].fill = 3;
    l[9].labels = 3, l[9].legend = -1;
    l[10].transparency = 30;
    l[11].chart_fill = kDarker, l[11].text = kWhiteText, l[11].explosion = 8;
    return l;
  }();
  return v;
}
const std::vector<Look>& AreaLooks() {
  static const std::vector<Look> v = [] {
    std::vector<Look> l(11);
    l[1].transparency = 30;
    l[2].fill = 1;
    l[3].grid = false;
    l[4].plot_fill = kGrayBg;
    l[5].chart_fill = kDark, l[5].text = kWhiteText;
    l[6].fill = 3;
    l[7].border = kWhiteLine;
    l[8].labels = -1;
    l[9].chart_fill = kGrayBg, l[9].transparency = 20;
    l[10].chart_fill = kDarker, l[10].text = kWhiteText, l[10].fill = 1;
    return l;
  }();
  return v;
}
const std::vector<Look>& ScatterLooks() {
  static const std::vector<Look> v = [] {
    std::vector<Look> l(15);
    l[1].labels = -1, l[1].val_axis = false;
    l[2].marker = 1, l[2].marker_size = 9;
    l[3].marker = 2;
    l[4].plot_fill = kGrayBg;
    l[5].chart_fill = kDark, l[5].text = kWhiteText;
    l[6].minor = true;
    l[7].marker = 3, l[7].marker_size = 8;
    l[8].chart_fill = kGrayBg;
    l[9].marker = 4;
    l[10].chart_fill = kDarker, l[10].text = kWhiteText, l[10].marker = 1;
    l[11].grid = false, l[11].marker = 1, l[11].marker_size = 11;
    l[12].marker = 1, l[12].marker_size = 5;
    l[13].labels = -1, l[13].marker = 1, l[13].marker_size = 7;
    l[14].chart_fill = kDark, l[14].text = kWhiteText, l[14].minor = true;
    return l;
  }();
  return v;
}
// the chart types without a set of their own: Excel's first eight looks
const std::vector<Look>& OtherLooks() {
  static const std::vector<Look> v = [] {
    std::vector<Look> l(ColumnLooks().begin(), ColumnLooks().begin() + 8);
    l[2] = Look{};  // light fills with borders
    l[2].gap = 100, l[2].transparency = 40, l[2].border = kGrayLine;
    return l;
  }();
  return v;
}

enum class StyleSet { Column, Bar, Line, Pie, Doughnut, Area, Scatter, Other };

StyleSet SetOf(model::ChartType t) {
  using T = model::ChartType;
  if (IsHorizontalBar(t) || t == T::BarClustered3D) return StyleSet::Bar;
  if (IsBarType(t) || t == T::Column3D || t == T::ColumnClustered3D || t == T::ColumnStacked3D) return StyleSet::Column;
  if (IsLineType(t) || t == T::Line3D || t == T::Radar || t == T::RadarMarkers) return StyleSet::Line;
  if (t == T::Pie || t == T::Pie3D || t == T::PieOfPie || t == T::BarOfPie) return StyleSet::Pie;
  if (t == T::Doughnut) return StyleSet::Doughnut;
  if (IsAreaType(t) || t == T::Area3D || t == T::RadarFilled) return StyleSet::Area;
  if (IsScatterType(t)) return StyleSet::Scatter;
  return StyleSet::Other;
}

const Look* LooksOf(model::ChartType t, int* count) {
  const std::vector<Look>* v = &OtherLooks();
  switch (SetOf(t)) {
    case StyleSet::Column: v = &ColumnLooks(); break;
    case StyleSet::Bar: v = &ColumnLooks(); break;
    case StyleSet::Line: v = &LineLooks(); break;
    case StyleSet::Pie: v = &PieLooks(); break;
    case StyleSet::Doughnut: v = &PieLooks(); break;
    case StyleSet::Area: v = &AreaLooks(); break;
    case StyleSet::Scatter: v = &ScatterLooks(); break;
    default: break;
  }
  const StyleSet st = SetOf(t);
  *count = st == StyleSet::Bar ? 13 : st == StyleSet::Doughnut ? 10 : static_cast<int>(v->size());
  return v->data();
}
}  // namespace

const char* ChartQuickLayoutTip(int layout) {
  static const char* kTips[] = {
      TR("Chart title, legend at the right, gridlines"),
      TR("Chart title, legend at the top, data labels, no vertical axis or gridlines"),
      TR("Chart title, legend at the bottom, gridlines"),
      TR("Legend at the bottom, data labels, gridlines"),
      TR("Chart title, vertical axis title, legend at the bottom"),
      TR("Chart title, vertical axis title, no legend"),
      TR("Both axis titles, legend at the right, gridlines"),
      TR("Chart title, both axis titles, no legend, columns touching"),
      TR("Chart title, both axis titles, legend at the right"),
      TR("Chart title, legend at the right, data labels inside wide columns"),
      TR("Legend in the top right corner, gridlines")};
  return kTips[std::clamp(layout, 1, kChartQuickLayouts) - 1];
}
int ChartStyleCount(model::ChartType t) {
  int n = 0;
  LooksOf(t, &n);
  return n;
}

int ChartStyleNumber(model::ChartType t) {
  switch (SetOf(t)) {  // what Excel's Chart.ChartStyle reads for Style 1 of each kind of chart
    case StyleSet::Bar: return 216;
    case StyleSet::Line: return 227;
    case StyleSet::Pie:
    case StyleSet::Doughnut: return 251;
    case StyleSet::Area: return 276;
    case StyleSet::Scatter: return 240;
    default: return 201;
  }
}

std::string ChartStyleTip(model::ChartType t, int style) {
  int n = 0;
  const Look& l = LooksOf(t, &n)[std::clamp(style, 1, std::max(1, ChartStyleCount(t))) - 1];
  std::vector<std::string> parts;
  if (l.chart_fill == kDark || l.chart_fill == kDarker) parts.push_back(TR("dark background, white text"));
  else if (l.chart_fill) parts.push_back(TR("gray background"));
  if (l.plot_fill) parts.push_back(TR("shaded plot area"));
  if (l.fill == 1) parts.push_back(TR("gradient fills"));
  if (l.fill == 3) parts.push_back(TR("pattern fills"));
  if (l.outline) parts.push_back(TR("outlined"));
  if (l.transparency) parts.push_back(TR("light fills"));
  if (l.border && !l.outline && SetOf(t) != StyleSet::Pie && SetOf(t) != StyleSet::Doughnut) parts.push_back(TR("borders"));
  if (l.weight >= 3) parts.push_back(TR("thick lines"));
  else if (l.weight > 0 && l.weight < 1.6f) parts.push_back(TR("thin lines"));
  if (l.smooth) parts.push_back(TR("smoothed lines"));
  if (l.dash) parts.push_back(TR("dashed lines"));
  if (l.marker > 0) parts.push_back(TR("markers"));
  if (l.explosion > 0) parts.push_back(TR("slices pulled out"));
  if (l.labels >= -1) parts.push_back(TR("data labels"));
  if (!l.val_axis) parts.push_back(TR("no vertical axis"));
  if (!l.grid) parts.push_back(TR("no gridlines"));
  else if (l.minor) parts.push_back(TR("major and minor gridlines"));
  if (l.legend == -1) parts.push_back(TR("no legend"));
  if (l.gap >= 0 && l.gap <= 80 && SetOf(t) != StyleSet::Pie) parts.push_back(TR("wide columns"));
  std::string out;
  for (const auto& p : parts) out += (out.empty() ? "" : ", ") + p;
  if (out.empty()) return TR("White, gridlines");
  out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
  return out;
}
const char* ChartStyleTip(int style) {
  static std::vector<std::string> tips;
  if (tips.empty()) {
    for (int k = 1; k <= kChartStyles; ++k) tips.push_back(ChartStyleTip(model::ChartType::ColumnClustered, std::min(k, ChartStyleCount(model::ChartType::ColumnClustered))));
  }
  return tips[static_cast<size_t>(std::clamp(style, 1, kChartStyles) - 1)].c_str();
}

void ApplyChartQuickLayout(model::ChartObj& ch, int layout) {
  const QuickLayout& q = kLayouts[std::clamp(layout, 1, kChartQuickLayouts) - 1];
  ch.has_title = q.title;
  ch.title_overlay = false;
  ch.title_rect = {};
  ch.legend_rect = {};
  ch.plot_rect = {};
  ch.has_legend = q.legend >= 0;
  if (q.legend >= 0) ch.legend_pos = q.legend;
  ch.legend_overlay = false;
  ch.cat_axis.visible = true;
  ch.val_axis.visible = q.val_axis;
  ch.cat_axis.has_title = q.cat_title;
  ch.val_axis.has_title = q.val_title;
  if (q.cat_title && ch.cat_axis.title.empty()) ch.cat_axis.title = TR("Axis Title");
  if (q.val_title && ch.val_axis.title.empty()) ch.val_axis.title = TR("Axis Title");
  ch.val_axis.gridlines = q.grid;
  ch.cat_axis.gridlines = false;
  if (q.gap >= 0) ch.gap_width = q.gap;
  const bool bars = IsBarType(ch.type) || IsHorizontalBar(ch.type);
  for (auto& se : ch.series) {
    se.data_labels = q.labels >= -1;
    if (q.labels >= -1) se.label_pos = bars ? q.labels : -1;
  }
}

void ApplyChartStyle(model::ChartObj& ch, int style) {
  int count = 0;
  const Look* looks = LooksOf(ch.type, &count);
  const int n = std::clamp(style, 1, count);
  const Look& p = looks[n - 1];
  const StyleSet set = SetOf(ch.type);
  const bool bars = set == StyleSet::Column || set == StyleSet::Bar || set == StyleSet::Other;
  const bool pies = set == StyleSet::Pie || set == StyleSet::Doughnut;
  ch.style = n;
  if (p.gap >= 0 && bars) ch.gap_width = p.gap;
  ch.val_axis.visible = p.val_axis;
  ch.val_axis.gridlines = p.grid && !pies;
  ch.val_axis.minor_gridlines = p.minor && !pies;
  ch.chart_fill = p.chart_fill;
  ch.plot_fill = p.plot_fill;
  ch.chart_line = ch.plot_line = ch.legend_fill = ch.legend_line = ch.title_fill = ch.title_line = 0;
  ch.chart_line_none = false;
  ch.title_font.color = ch.legend_font.color = ch.axis_font.color = p.text;
  if (p.legend != -9) {
    ch.has_legend = p.legend >= 0;
    if (p.legend >= 0) ch.legend_pos = p.legend;
  }
  for (size_t i = 0; i < ch.series.size(); ++i) {
    auto& se = ch.series[i];
    se.color = 0;  // a style colors the series from the chart's palette again
    se.no_fill = p.outline;
    se.transparency = p.transparency;
    se.no_line = false;
    se.line_color = p.outline ? (ChartPalette(i) & 0xFFFFFF) : (bars || pies || set == StyleSet::Area ? p.border : 0);
    se.line_weight = p.outline ? 2.25f : (pies && p.weight > 0) || set == StyleSet::Line || set == StyleSet::Scatter ? p.weight : 0;
    se.dash = p.dash;
    se.fill_type = p.fill;
    se.pattern = p.fill == 3 ? 7 : se.pattern;
    se.pattern_back = 0;
    se.grad_color2 = 0;
    se.marker = p.marker;
    se.marker_size = p.marker_size;
    se.smooth = p.smooth;
    se.explosion = pies && p.explosion > 0 ? p.explosion : 0;
    se.points.clear();
    se.data_labels = p.labels >= -1;
    if (p.labels >= -1) se.label_pos = (bars || pies) ? p.labels : -1;
  }
}

std::vector<std::vector<std::string>> ChartCategoryLevels(const model::ChartObj& ch, const ChartValues& v, size_t n) {
  if (!ch.cat_axis.multi_level || n == 0 || v.paths.size() < n || IsScatterType(ch.type)) return {};
  size_t levels = 0;
  for (size_t i = 0; i < n; ++i) levels = std::max(levels, v.paths[i].size());
  if (levels < 2) return {};
  return std::vector<std::vector<std::string>>(v.paths.begin(), v.paths.begin() + static_cast<std::ptrdiff_t>(n));
}

std::string ChartCategoryLabel(const ChartValues& v, size_t i) {
  if (i < v.paths.size() && v.paths[i].size() >= 2) {
    std::string s;
    for (const auto& part : v.paths[i]) s += (s.empty() ? "" : " ") + part;
    return s;
  }
  return i < v.cats.size() ? v.cats[i] : std::to_string(i + 1);
}

}  // namespace yc::engine
