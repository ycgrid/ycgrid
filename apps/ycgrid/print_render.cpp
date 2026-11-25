// SPDX-License-Identifier: Apache-2.0
// print_render.cpp - printed pages drawn with GDI: printers, PDF (Microsoft Print to PDF) and Print Preview share it
//
// Coordinates are twips (1/20 pt) of the paper; the device context maps them to printer or preview pixels.
#include "print.h"

#include "app_state.h"

#include "engine/chart_layout.h"
#include "engine/page_layout.h"
#include "engine/rule_eval.h"
#include "engine/shape_geometry.h"
#include "engine/sparkline.h"
#include "formula/numfmt.h"

#include <windows.h>
#include <winspool.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>
#include <unordered_map>

namespace yc::app {

using core::Rect;

namespace {

constexpr double kTw = 20.0;   // twips per point
constexpr double kPt = 0.75;   // sheet pixels -> points

int Tw(double pt) { return static_cast<int>(std::lround(pt * kTw)); }
COLORREF Cr(uint32_t rgb) {
  rgb = model::ResolveColor(rgb);
  return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
COLORREF Mix(COLORREF a, COLORREF b, double f) {
  auto ch = [&](int v1, int v2) { return static_cast<int>(v1 + (v2 - v1) * f + 0.5); };
  return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

class Gdi {
 public:
  Gdi(HDC dc, double device_px_per_inch, bool bw) : dc_(dc), bw_(bw) {
    // one device pixel in twips: thin lines never vanish on low-resolution previews
    min_tw_ = std::max(1, static_cast<int>(std::ceil(1440.0 / std::max(1.0, device_px_per_inch))));
    ::SetBkMode(dc_, TRANSPARENT);
  }
  ~Gdi() {
    for (auto& [k, f] : fonts_) ::DeleteObject(f);
  }
  Gdi(const Gdi&) = delete;
  Gdi& operator=(const Gdi&) = delete;

  HDC dc() const { return dc_; }
  bool bw() const { return bw_; }
  COLORREF Ink(COLORREF c) const { return bw_ ? RGB(0, 0, 0) : c; }

  void Fill(double x0, double y0, double x1, double y1, COLORREF c) {
    RECT r{Tw(x0), Tw(y0), Tw(x1), Tw(y1)};
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = ::CreateSolidBrush(c);
    ::FillRect(dc_, &r, b);
    ::DeleteObject(b);
  }
  // a horizontal / vertical line as a filled band (exact on every device), at least one device pixel thick
  void HLine(double x0, double x1, double y, double w_pt, COLORREF c) {
    const int w = std::max(min_tw_, Tw(w_pt));
    RECT r{Tw(x0), Tw(y) - w / 2, Tw(x1), Tw(y) - w / 2 + w};
    Band(r, c);
  }
  void VLine(double x, double y0, double y1, double w_pt, COLORREF c) {
    const int w = std::max(min_tw_, Tw(w_pt));
    RECT r{Tw(x) - w / 2, Tw(y0), Tw(x) - w / 2 + w, Tw(y1)};
    Band(r, c);
  }
  void Line(double x0, double y0, double x1, double y1, double w_pt, COLORREF c) {
    LOGBRUSH lb{BS_SOLID, c, 0};
    HPEN pen = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT, static_cast<DWORD>(std::max(min_tw_, Tw(w_pt))), &lb, 0, nullptr);
    HGDIOBJ old = ::SelectObject(dc_, pen);
    ::MoveToEx(dc_, Tw(x0), Tw(y0), nullptr);
    ::LineTo(dc_, Tw(x1), Tw(y1));
    ::SelectObject(dc_, old);
    ::DeleteObject(pen);
  }
  // pen + brush for shapes (nullopt-like: visible == false selects the null object)
  HGDIOBJ UsePen(bool visible, COLORREF c, double w_pt) {
    if (!visible) return ::SelectObject(dc_, ::GetStockObject(NULL_PEN));
    LOGBRUSH lb{BS_SOLID, c, 0};
    pen_ = ::ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_MITER, static_cast<DWORD>(std::max(min_tw_, Tw(w_pt))), &lb, 0, nullptr);
    return ::SelectObject(dc_, pen_);
  }
  HGDIOBJ UseBrush(bool visible, COLORREF c) {
    if (!visible) return ::SelectObject(dc_, ::GetStockObject(NULL_BRUSH));
    brush_ = ::CreateSolidBrush(c);
    return ::SelectObject(dc_, brush_);
  }
  void Restore(HGDIOBJ old_pen, HGDIOBJ old_brush) {
    ::SelectObject(dc_, old_pen);
    ::SelectObject(dc_, old_brush);
    if (pen_) ::DeleteObject(pen_);
    if (brush_) ::DeleteObject(brush_);
    pen_ = nullptr;
    brush_ = nullptr;
  }

  HFONT Font(const std::string& name, double size_pt, bool bold, bool italic, bool underline = false, bool strike = false) {
    const int h = std::max(1, Tw(size_pt));
    const std::string key = name + "|" + std::to_string(h) + (bold ? "b" : "") + (italic ? "i" : "") + (underline ? "u" : "") + (strike ? "s" : "");
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;
    const std::wstring face = FromUtf8(name.empty() ? std::string("Calibri") : name);
    HFONT f = ::CreateFontW(-h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic, underline, strike, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
    fonts_.emplace(key, f);
    return f;
  }
  // text width in points with the selected font
  double Width(const std::wstring& s) {
    SIZE sz{};
    ::GetTextExtentPoint32W(dc_, s.c_str(), static_cast<int>(s.size()), &sz);
    return sz.cx / kTw;
  }
  double LineHeight() {
    TEXTMETRICW tm{};
    ::GetTextMetricsW(dc_, &tm);
    return tm.tmHeight / kTw;
  }
  // text in a box: halign 0 left / 1 center / 2 right, valign 0 top / 1 middle / 2 bottom; clip = the visible box
  void Text(const std::wstring& s, double x0, double y0, double x1, double y1, int halign, int valign, bool wrap, COLORREF c, const RECT* clip = nullptr) {
    if (s.empty()) return;
    ::SetTextColor(dc_, Ink(c));
    RECT r{Tw(x0), Tw(y0), Tw(x1), Tw(y1)};
    UINT fl = DT_NOPREFIX | (halign == 1 ? DT_CENTER : halign == 2 ? DT_RIGHT : DT_LEFT);
    if (wrap) {
      RECT m = r;
      ::DrawTextW(dc_, s.c_str(), static_cast<int>(s.size()), &m, fl | DT_WORDBREAK | DT_CALCRECT);
      const int th = m.bottom - m.top, bh = r.bottom - r.top;
      if (valign == 1) r.top += (bh - th) / 2;
      else if (valign == 2) r.top = r.bottom - th;
      fl |= DT_WORDBREAK;
    } else {
      fl |= DT_SINGLELINE | (valign == 0 ? DT_TOP : valign == 1 ? DT_VCENTER : DT_BOTTOM);
    }
    const int saved = ::SaveDC(dc_);
    if (clip) ::IntersectClipRect(dc_, clip->left, clip->top, clip->right, clip->bottom);
    ::DrawTextW(dc_, s.c_str(), static_cast<int>(s.size()), &r, fl | (clip ? DT_NOCLIP : 0));
    ::RestoreDC(dc_, saved);
  }

  // rotated text (Orientation: -90..90 degrees counterclockwise, 255 = stacked) with the selected font
  void RotatedText(const std::wstring& s, double x0, double y0, double x1, double y1, int halign, int valign, int rotation, COLORREF c, const RECT* clip) {
    if (s.empty()) return;
    ::SetTextColor(dc_, Ink(c));
    const int saved = ::SaveDC(dc_);
    if (clip) ::IntersectClipRect(dc_, clip->left, clip->top, clip->right, clip->bottom);
    ::SetBkMode(dc_, TRANSPARENT);
    ::SetTextAlign(dc_, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    if (rotation == 255) {
      const double lh = LineHeight(), total = lh * static_cast<double>(s.size());
      double y = valign == 0 ? y0 : valign == 1 ? (y0 + y1 - total) / 2 : y1 - total;
      for (wchar_t ch : s) {
        const std::wstring one(1, ch);
        const double w = Width(one);
        const double x = halign == 0 ? x0 : halign == 2 ? x1 - w : (x0 + x1 - w) / 2;
        ::TextOutW(dc_, Tw(x), Tw(y), one.c_str(), 1);
        y += lh;
      }
      ::RestoreDC(dc_, saved);
      return;
    }
    const double w = Width(s), h = LineHeight();
    LOGFONTW lf{};
    ::GetObjectW(::GetCurrentObject(dc_, OBJ_FONT), sizeof lf, &lf);
    lf.lfEscapement = lf.lfOrientation = rotation * 10;
    HFONT f = ::CreateFontIndirectW(&lf);
    ::SelectObject(dc_, f);
    const double a = rotation * 3.14159265358979 / 180.0, ca = std::cos(a), sa = std::sin(a);
    const double bw = std::fabs(w * ca) + std::fabs(h * sa), bh = std::fabs(w * sa) + std::fabs(h * ca);
    const double cx = halign == 0 ? x0 + bw / 2 : halign == 2 ? x1 - bw / 2 : (x0 + x1) / 2;
    const double cy = valign == 0 ? y0 + bh / 2 : valign == 1 ? (y0 + y1) / 2 : y1 - bh / 2;
    // the unrotated text's top-left corner, turned around the center
    const double dx = -w / 2, dy = -h / 2;
    ::TextOutW(dc_, Tw(cx + dx * ca + dy * sa), Tw(cy - dx * sa + dy * ca), s.c_str(), static_cast<int>(s.size()));
    ::RestoreDC(dc_, saved);
    ::DeleteObject(f);
  }

 private:
  void Band(RECT r, COLORREF c) {
    HBRUSH b = ::CreateSolidBrush(Ink(c));
    ::FillRect(dc_, &r, b);
    ::DeleteObject(b);
  }
  HDC dc_;
  bool bw_;
  int min_tw_ = 1;
  HPEN pen_ = nullptr;
  HBRUSH brush_ = nullptr;
  std::unordered_map<std::string, HFONT> fonts_;
};

// cell fill on paper: solid, patterns (grays as a mix of the two colors, stripes as lines) and gradients (bands)
void FillCell(Gdi& g, double x0, double y0, double x1, double y1, const model::Style& st, double sc) {
  using model::FillGradient;
  using model::FillPattern;
  const COLORREF c1 = (st.fill_color >> 24) ? Cr(st.fill_color) : RGB(255, 255, 255);
  if (st.gradient != FillGradient::None) {
    const COLORREF c2 = (st.gradient_color2 >> 24) ? Cr(st.gradient_color2) : RGB(255, 255, 255);
    const int steps = 32;
    const int a = ((st.gradient_angle % 360) + 360) % 360;
    if (st.gradient == FillGradient::FromCenter) {
      for (int i = 0; i < steps; ++i) {  // shrinking rectangles from the edge (color 2) to the center (color 1)
        const double f = static_cast<double>(i) / steps, dx = (x1 - x0) * f / 2, dy = (y1 - y0) * f / 2;
        g.Fill(x0 + dx, y0 + dy, x1 - dx, y1 - dy, Mix(c2, c1, f));
      }
      return;
    }
    if (st.gradient == FillGradient::FromCorner) {  // bands around the corner (first color) out to the far sides
      const int corner = std::clamp<int>(st.gradient_angle, 0, 3);
      const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
      for (int i = 0; i < steps; ++i) {
        const double f = 1.0 - static_cast<double>(i) / steps, w = (x1 - x0) * f, h = (y1 - y0) * f;
        const double ax = right ? x1 - w : x0, ay = bottom ? y1 - h : y0;
        g.Fill(ax, ay, ax + w, ay + h, Mix(c1, c2, f));
      }
      return;
    }
    const bool vertical = a == 90 || a == 270;
    const bool reverse = a == 180 || a == 270;
    for (int i = 0; i < steps; ++i) {
      const double f0 = static_cast<double>(i) / steps, f1 = static_cast<double>(i + 1) / steps;
      const COLORREF c = Mix(c1, c2, reverse ? 1 - (f0 + f1) / 2 : (f0 + f1) / 2);
      if (vertical) g.Fill(x0, y0 + (y1 - y0) * f0, x1, y0 + (y1 - y0) * f1, c);
      else g.Fill(x0 + (x1 - x0) * f0, y0, x0 + (x1 - x0) * f1, y1, c);
    }
    return;
  }
  const COLORREF pc = (st.pattern_color >> 24) ? Cr(st.pattern_color) : RGB(0, 0, 0);
  double mix = 0;
  switch (st.pattern) {
    case FillPattern::Solid: break;
    case FillPattern::Gray75: mix = 0.75; break;
    case FillPattern::Gray50: mix = 0.5; break;
    case FillPattern::Gray25: mix = 0.25; break;
    case FillPattern::Gray125: mix = 0.125; break;
    case FillPattern::Gray0625: mix = 0.0625; break;
    case FillPattern::SemiGray75: mix = 0.6; break;
    case FillPattern::Down:
    case FillPattern::Up:
    case FillPattern::Checker: mix = 0.4; break;
    case FillPattern::LightDown:
    case FillPattern::LightUp:
    case FillPattern::CrissCross: mix = 0.25; break;
    default: break;
  }
  g.Fill(x0, y0, x1, y1, mix > 0 ? Mix(c1, pc, mix) : c1);
  const bool light = st.pattern >= FillPattern::LightHorizontal;
  const double step = 3 * sc, w = (light ? 0.5 : 1.0) * sc;
  if (st.pattern == FillPattern::Horizontal || st.pattern == FillPattern::LightHorizontal || st.pattern == FillPattern::Grid) {
    for (double y = y0; y < y1; y += step) g.Fill(x0, y, x1, std::min(y1, y + w), pc);
  }
  if (st.pattern == FillPattern::Vertical || st.pattern == FillPattern::LightVertical || st.pattern == FillPattern::Grid) {
    for (double x = x0; x < x1; x += step) g.Fill(x, y0, std::min(x1, x + w), y1, pc);
  }
}

// one row or column printed on the page: its index, position and size in points (paper coordinates)
struct Slot {
  uint32_t index;
  double pos, size;
};

const Slot* FindSlot(const std::vector<Slot>& v, uint32_t index) {
  for (const Slot& s : v) {
    if (s.index == index) return &s;
  }
  return nullptr;
}

// a merged area clipped to the printed slots: false when none of its rows / columns are printed
bool SlotSpan(const std::vector<Slot>& v, uint32_t lo, uint32_t hi, double* a, double* b) {
  bool any = false;
  for (const Slot& s : v) {
    if (s.index < lo || s.index > hi) continue;
    if (!any) *a = s.pos;
    *b = s.pos + s.size;
    any = true;
  }
  return any;
}

void DrawBorder(Gdi& g, model::LineStyle ls, COLORREF c, double x0, double y0, double x1, double y1, bool horizontal, double sc) {
  using model::LineStyle;
  const double thin = 0.5 * std::max(0.6, sc), med = 1.0 * std::max(0.6, sc), thick = 1.5 * std::max(0.6, sc);
  auto solid = [&](double w, double off) {
    if (horizontal) g.HLine(x0, x1, y0 + off, w, c);
    else g.VLine(x0 + off, y0, y1, w, c);
  };
  auto dashed = [&](double dash, double gap, double w) {
    const double len = horizontal ? x1 - x0 : y1 - y0;
    for (double t = 0; t < len; t += dash + gap) {
      const double e = std::min(len, t + dash);
      if (horizontal) g.HLine(x0 + t, x0 + e, y0, w, c);
      else g.VLine(x0, y0 + t, y0 + e, w, c);
    }
  };
  switch (ls) {
    case LineStyle::None: break;
    case LineStyle::Thin: solid(thin, 0); break;
    case LineStyle::Hair: dashed(0.75, 0.75, thin); break;
    case LineStyle::Dotted: dashed(thin, thin * 2, thin); break;
    case LineStyle::Dashed: dashed(2.25, 0.75, thin); break;
    case LineStyle::DashDot: dashed(4.5, 1.5, thin); break;
    case LineStyle::DashDotDot: dashed(4.5, 2.25, thin); break;
    case LineStyle::Medium: solid(med, 0); break;
    case LineStyle::MediumDashed:
    case LineStyle::MediumDashDot:
    case LineStyle::MediumDashDotDot:
    case LineStyle::SlantDashDot: dashed(4.5, 1.5, med); break;
    case LineStyle::Thick: solid(thick, 0); break;
    case LineStyle::Double:
      solid(thin, -thin);
      solid(thin, thin);
      break;
  }
}

std::wstring Wide(const std::string& s) { return FromUtf8(s); }

// ---- charts (a printable rendering of the chart model) ----
double NiceStep(double range) {
  if (!(range > 0)) return 1;
  const double raw = range / 5, mag = std::pow(10.0, std::floor(std::log10(raw))), f = raw / mag;
  return (f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10) * mag;
}

std::wstring Num(double v) {
  wchar_t b[64];
  if (std::fabs(v) >= 1e6 || (std::fabs(v) < 1e-3 && v != 0)) swprintf(b, 64, L"%.3g", v);
  else swprintf(b, 64, L"%.10g", std::round(v * 1e6) / 1e6);
  return b;
}

// scene colors are ABGR: the low 24 bits are a COLORREF; alpha blends with white paper
COLORREF SceneInk(uint32_t abgr) {
  const COLORREF c = abgr & 0x00FFFFFFu;
  const double a = static_cast<double>((abgr >> 24) & 0xFF) / 255.0;
  return a >= 0.999 ? c : Mix(RGB(255, 255, 255), c, a);
}

// the chart kinds drawn from an engine scene (radar, waterfall, histogram, treemap...)
void DrawScene(Gdi& g, const model::ChartObj& ch, const engine::ChartValues& v, double x0, double y0, double x1, double y1, double sc) {
  ::SelectObject(g.dc(), g.Font("Calibri", 9 * sc, false, false));
  const double lh = g.LineHeight();
  auto measure = [&](const std::string& t, float s) {
    ::SelectObject(g.dc(), g.Font("Calibri", 9 * sc * s, false, false));
    const double w = g.Width(Wide(t));
    ::SelectObject(g.dc(), g.Font("Calibri", 9 * sc, false, false));
    return static_cast<float>(w);
  };
  engine::ChartScene scene;
  engine::BuildChartScene(ch, v, static_cast<float>(x1 - x0), static_cast<float>(y1 - y0), static_cast<float>(lh), measure, &scene);
  std::vector<POINT> pts;
  for (const auto& p : scene.prims) {
    switch (p.kind) {
      case engine::ChartPrim::Rect:
        if (p.fill) g.Fill(x0 + p.pts[0], y0 + p.pts[1], x0 + p.pts[2], y0 + p.pts[3], g.Ink(SceneInk(p.fill)));
        if (p.stroke) {
          const COLORREF c = g.Ink(SceneInk(p.stroke));
          const double w = p.width * 0.75;
          g.HLine(x0 + p.pts[0], x0 + p.pts[2], y0 + p.pts[1], w, c);
          g.HLine(x0 + p.pts[0], x0 + p.pts[2], y0 + p.pts[3], w, c);
          g.VLine(x0 + p.pts[0], y0 + p.pts[1], y0 + p.pts[3], w, c);
          g.VLine(x0 + p.pts[2], y0 + p.pts[1], y0 + p.pts[3], w, c);
        }
        break;
      case engine::ChartPrim::Poly:
      case engine::ChartPrim::Line: {
        pts.clear();
        for (size_t k = 0; k + 1 < p.pts.size(); k += 2) pts.push_back(POINT{Tw(x0 + p.pts[k]), Tw(y0 + p.pts[k + 1])});
        if (pts.size() < 2) break;
        if (p.kind == engine::ChartPrim::Poly) {
          HGDIOBJ op = g.UsePen(p.stroke != 0, g.Ink(SceneInk(p.stroke)), p.width * 0.75), ob = g.UseBrush(p.fill != 0, g.Ink(SceneInk(p.fill)));
          ::Polygon(g.dc(), pts.data(), static_cast<int>(pts.size()));
          g.Restore(op, ob);
        } else {
          HGDIOBJ op = g.UsePen(true, g.Ink(SceneInk(p.stroke)), p.width * 0.75), ob = g.UseBrush(false, 0);
          ::Polyline(g.dc(), pts.data(), static_cast<int>(pts.size()));
          g.Restore(op, ob);
        }
        break;
      }
      case engine::ChartPrim::Circle: {
        const double cx = x0 + p.pts[0], cy = y0 + p.pts[1], r = p.pts[2] * 0.75;
        HGDIOBJ op = g.UsePen(p.stroke != 0, g.Ink(SceneInk(p.stroke)), 0.75), ob = g.UseBrush(p.fill != 0, g.Ink(SceneInk(p.fill)));
        ::Ellipse(g.dc(), Tw(cx - r), Tw(cy - r), Tw(cx + r), Tw(cy + r));
        g.Restore(op, ob);
        break;
      }
      case engine::ChartPrim::Text: {
        ::SelectObject(g.dc(), g.Font("Calibri", 9 * sc * p.scale, p.bold, false));
        const std::wstring t = Wide(p.text);
        const double w = g.Width(t), h = g.LineHeight();
        if (p.rotate == 90) {  // reading upwards, centered on its anchor
          const double cx = x0 + p.pts[0], cy = y0 + p.pts[1];
          g.RotatedText(t, cx - h / 2, cy - w / 2 - h, cx + h / 2, cy + w / 2 + h, 1, 1, 90, SceneInk(p.stroke), nullptr);
          ::SelectObject(g.dc(), g.Font("Calibri", 9 * sc, false, false));
          break;
        }
        double tx = x0 + p.pts[0], ty = y0 + p.pts[1];
        tx -= p.halign == 1 ? w / 2 : (p.halign == 2 ? w : 0);
        ty -= p.valign == 1 ? h / 2 : (p.valign == 2 ? h : 0);
        g.Text(t, tx, ty, tx + w + 1, ty + h, 0, 0, false, SceneInk(p.stroke));
        ::SelectObject(g.dc(), g.Font("Calibri", 9 * sc, false, false));
        break;
      }
    }
  }
}

// an element color (ARGB, 0 = automatic)
COLORREF ArgbRef(uint32_t argb, COLORREF automatic) { return argb ? RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF) : automatic; }

void DrawChart(Gdi& g, const model::ChartObj& ch, const engine::ChartValues& v, double x0, double y0, double x1, double y1, double sc) {
  using model::ChartType;
  // data labels on paper as on the screen: each point's own (shown or not, its own text) and dragged by the same offset
  // (the series' labels dragged together, then the one label: model::DataLabelOffset), in fractions of the whole chart
  const double box_w = x1 - x0, box_h = y1 - y0;
  auto label_on = [&](const model::ChartSeries& d, int k) {
    const model::ChartPointFormat* pf = model::PointFormatOf(d, k);
    return pf && pf->label >= 0 ? pf->label == 1 : d.data_labels;
  };
  auto label_text = [&](size_t si, int k, std::wstring contents) {
    const auto it = v.point_labels.find((static_cast<uint64_t>(si) << 32) | static_cast<uint32_t>(k));
    return it != v.point_labels.end() ? Wide(it->second) : contents;
  };
  auto label_shift = [&](const model::ChartSeries& d, int k, double* dx, double* dy) {
    float fx = 0, fy = 0;
    model::DataLabelOffset(d, k, &fx, &fy);
    *dx = fx * box_w;
    *dy = fy * box_h;
  };
  const engine::PaletteScope palette(ch.color_scheme);  // the chart's colors (Change Colors)
  const COLORREF axis = RGB(0x80, 0x80, 0x80);
  const COLORREF text = ArgbRef(ch.axis_font.color, RGB(0x40, 0x40, 0x40));
  g.Fill(x0, y0, x1, y1, g.Ink(ArgbRef(ch.chart_fill, RGB(255, 255, 255))));
  if (!ch.chart_line_none) {
    const COLORREF border = ArgbRef(ch.chart_line, RGB(0xD9, 0xD9, 0xD9));
    g.HLine(x0, x1, y0, 0.75, border);
    g.HLine(x0, x1, y1, 0.75, border);
    g.VLine(x0, y0, y1, 0.75, border);
    g.VLine(x1, y0, y1, 0.75, border);
  }
  if (engine::IsSceneChart(ch.type)) {
    DrawScene(g, ch, v, x0, y0, x1, y1, sc);
    return;
  }
  const double pad = 8 * sc;
  double top = y0 + pad, bottom = y1 - pad;
  if (ch.has_title && !v.title.empty()) {
    ::SelectObject(g.dc(), g.Font("Calibri", (ch.title_font.size > 0 ? ch.title_font.size : 14) * sc, ch.title_font.bold, false));
    const double h = g.LineHeight();
    g.Text(Wide(v.title), x0, top, x1, top + h, 1, 0, false, ArgbRef(ch.title_font.color, RGB(0x40, 0x40, 0x40)));
    top += h + 4 * sc;
  }
  auto color_of = [&](size_t i) { return SceneInk(engine::SeriesColor(ch, v, i)); };
  const bool pie = ch.type == ChartType::Pie || ch.type == ChartType::Doughnut;
  auto cat_name = [&](size_t i) { return i < v.cats.size() ? Wide(v.cats[i]) : std::to_wstring(i + 1); };
  // legend: bottom, top, or a column on the right / left (Legend.Position)
  if (ch.has_legend && !v.series.empty()) {
    ::SelectObject(g.dc(), g.Font("Calibri", (ch.legend_font.size > 0 ? ch.legend_font.size : 9) * sc, ch.legend_font.bold, false));
    const double llh = g.LineHeight();
    const COLORREF ltext = ArgbRef(ch.legend_font.color, RGB(0x40, 0x40, 0x40));
    const size_t n = pie ? static_cast<size_t>(std::max(0, v.series[0].N())) : v.series.size();
    double w = 0, wmax = 0;
    std::vector<std::wstring> names;
    for (size_t i = 0; i < n && i < 30; ++i) {
      names.push_back(pie ? cat_name(i) : Wide(v.series[i].name));
      w += g.Width(names.back()) + llh * 1.6;
      wmax = std::max(wmax, g.Width(names.back()) + llh * 0.9);
    }
    auto key = [&](size_t i, double x, double y) {
      const COLORREF c = pie ? SceneInk(engine::ChartPalette(i)) : color_of(i);
      g.Fill(x, y + llh * 0.25, x + llh * 0.5, y + llh * 0.75, g.Ink(c));
      g.Text(names[i], x + llh * 0.7, y, x + llh * 0.7 + g.Width(names[i]) + 2, y + llh, 0, 1, false, ltext);
    };
    if (ch.legend_pos == 1 || ch.legend_pos == 3 || ch.legend_pos == 4) {
      const double colw = wmax + pad;
      const double lx = ch.legend_pos == 3 ? x0 + pad : x1 - colw;
      double y = ch.legend_pos == 4 ? top : (top + bottom - llh * static_cast<double>(names.size())) / 2;
      for (size_t i = 0; i < names.size(); ++i, y += llh) key(i, lx, y);
      if (ch.legend_pos == 3) x0 += colw + pad;
      else x1 -= colw;
    } else {
      double x = (x0 + x1 - w) / 2;
      const double y = ch.legend_pos == 2 ? top : bottom - llh;
      for (size_t i = 0; i < names.size(); ++i) {
        key(i, x, y);
        x += g.Width(names[i]) + llh * 1.6;
      }
      if (ch.legend_pos == 2) top += llh + 4 * sc;
      else bottom -= llh + 4 * sc;
    }
  }
  ::SelectObject(g.dc(), g.Font("Calibri", (ch.axis_font.size > 0 ? ch.axis_font.size : 9) * sc, ch.axis_font.bold, false));
  const double lh = g.LineHeight();
  if (pie) {
    if (v.series.empty() || v.series[0].N() <= 0) return;
    const auto& se = v.series[0];
    const int n = se.N();
    double total = 0;
    for (int i = 0; i < n; ++i) total += std::isfinite(se.Y()[i]) && se.Y()[i] > 0 ? se.Y()[i] : 0;
    if (!(total > 0)) return;
    const double cx = (x0 + x1) / 2, cy = (top + bottom) / 2, r = std::max(1.0, std::min(x1 - x0, bottom - top) / 2 - pad);
    const bool labels = !ch.series.empty();
    double ang = 90;  // degrees, counterclockwise from 3 o'clock; slices go clockwise from 12 o'clock
    for (int i = 0; i < n; ++i) {
      const double val = std::isfinite(se.Y()[i]) && se.Y()[i] > 0 ? se.Y()[i] : 0;
      if (val <= 0) continue;
      const double sweep = 360 * val / total;
      HGDIOBJ op = g.UsePen(true, RGB(255, 255, 255), 0.75), ob = g.UseBrush(true, g.Ink(SceneInk(engine::ChartPalette(static_cast<size_t>(i)))));
      const double a0 = ang * 3.14159265358979 / 180, a1 = (ang - sweep) * 3.14159265358979 / 180;
      // Pie draws counterclockwise from the first radial to the second: start at the slice's end
      ::Pie(g.dc(), Tw(cx - r), Tw(cy - r), Tw(cx + r), Tw(cy + r), Tw(cx + r * std::cos(a1)), Tw(cy - r * std::sin(a1)), Tw(cx + r * std::cos(a0)),
            Tw(cy - r * std::sin(a0)));
      g.Restore(op, ob);
      if (labels && label_on(ch.series[0], i)) {
        const double am = (a0 + a1) / 2, lr = ch.type == ChartType::Doughnut ? r * 0.75 : r * 0.65;
        double dx = 0, dy = 0;
        label_shift(ch.series[0], i, &dx, &dy);
        const double lx = cx + lr * std::cos(am) + dx, ly = cy - lr * std::sin(am) + dy;
        g.Text(label_text(0, i, Wide(engine::FormatChartNumber(val, se.fmt))), lx - 40 * sc, ly - lh / 2, lx + 40 * sc, ly + lh / 2, 1, 1, false, RGB(255, 255, 255));
      }
      ang -= sweep;
    }
    if (ch.type == ChartType::Doughnut) {
      HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, RGB(255, 255, 255));
      ::Ellipse(g.dc(), Tw(cx - r * 0.5), Tw(cy - r * 0.5), Tw(cx + r * 0.5), Tw(cy + r * 0.5));
      g.Restore(op, ob);
    }
    return;
  }
  // value range
  const bool horizontal = engine::IsHorizontalBar(ch.type), stock = engine::IsStockType(ch.type);
  const size_t ns = v.series.size();
  std::vector<ChartType> types(ns);
  size_t nbars = 0;
  bool stacked = false;
  for (size_t i = 0; i < ns; ++i) {
    types[i] = engine::EffectiveSeriesType(ch, i);
    if (engine::IsBarType(types[i])) {
      if (!nbars) stacked = engine::IsStackedType(types[i]);
      ++nbars;
    }
  }
  const bool scatter = engine::IsScatterType(ch.type) && nbars == 0;
  size_t n = 0;
  double lo = 1e300, hi = -1e300, xlo = 1e300, xhi = -1e300;
  std::vector<double> stack_pos, stack_neg;
  const bool vstock = ch.type == ChartType::StockVHLC || ch.type == ChartType::StockVOHLC;  // the volume has its own scale
  for (size_t si = 0; si < ns; ++si) {
    const auto& se = v.series[si];
    const int m = se.N();
    n = std::max(n, static_cast<size_t>(std::max(0, m)));
    if (vstock && si == 0) continue;
    const bool st = stacked && engine::IsBarType(types[si]);
    for (int i = 0; i < m; ++i) {
      const double y = se.Y()[i];
      if (!std::isfinite(y)) continue;
      if (st) {
        if (stack_pos.size() <= static_cast<size_t>(i)) stack_pos.resize(static_cast<size_t>(i) + 1, 0), stack_neg.resize(static_cast<size_t>(i) + 1, 0);
        (y >= 0 ? stack_pos[static_cast<size_t>(i)] : stack_neg[static_cast<size_t>(i)]) += y;
      } else {
        lo = std::min(lo, y);
        hi = std::max(hi, y);
      }
      const double x = se.X()[i];
      if (std::isfinite(x)) xlo = std::min(xlo, x), xhi = std::max(xhi, x);
    }
  }
  for (double s : stack_pos) hi = std::max(hi, s);
  for (double s : stack_neg) lo = std::min(lo, s);
  if (n == 0 || lo > hi) return;
  if (nbars || ch.type == ChartType::Area) lo = std::min(lo, 0.0), hi = std::max(hi, 0.0);
  if (!ch.val_axis.min_auto) lo = ch.val_axis.min;
  if (!ch.val_axis.max_auto) hi = ch.val_axis.max;
  if (hi <= lo) hi = lo + 1;
  const double step = ch.val_axis.major > 0 && (hi - lo) / ch.val_axis.major <= 60 ? ch.val_axis.major : NiceStep(hi - lo);
  if (ch.val_axis.min_auto) lo = std::floor(lo / step) * step;
  if (ch.val_axis.max_auto) hi = std::ceil(hi / step) * step;
  if (scatter && (xhi <= xlo)) xlo = 1, xhi = static_cast<double>(n);
  // plot area: room for the value labels
  double label_w = 0;
  for (double t = lo; t <= hi + step * 0.01 && ch.val_axis.visible; t += step)
    label_w = std::max(label_w, g.Width(ch.val_axis.number_format.empty() ? Num(t) : Wide(engine::FormatChartNumber(t, ch.val_axis.number_format))));
  // Multi-level Category Labels: the inner labels by the axis, the outer levels in rows below (columns beside)
  const std::vector<std::vector<std::string>> levels = scatter ? std::vector<std::vector<std::string>>{} : engine::ChartCategoryLevels(ch, v, n);
  size_t nlev = 0;
  for (const auto& lv : levels) nlev = std::max(nlev, lv.size());
  std::vector<double> lev_w(nlev, 0.0);
  for (const auto& lv : levels) {
    for (size_t k = 0; k + 1 < lv.size(); ++k) lev_w[k] = std::max(lev_w[k], g.Width(Wide(lv[k])));
  }
  double outer_w = 0;  // a vertical category axis: the outer columns' room
  for (size_t k = 0; k + 1 < nlev; ++k) outer_w += lev_w[k] + lh * 0.6;
  auto inner_name = [&](size_t i) {
    if (!levels.empty()) return i < levels.size() && !levels[i].empty() ? Wide(levels[i].back()) : std::wstring();
    return Wide(engine::ChartCategoryLabel(v, i));
  };
  double cat_w = 0;
  if (horizontal) {
    for (size_t i = 0; i < n && i < 200; ++i) cat_w = std::max(cat_w, g.Width(inner_name(i)));
    cat_w += outer_w;
  }
  // axis titles: the vertical axis' title turned on its side at the left, the horizontal axis' title under its labels
  const model::ChartAxisFormat& left_axis = horizontal ? ch.cat_axis : ch.val_axis;
  const model::ChartAxisFormat& bottom_axis = horizontal ? ch.val_axis : ch.cat_axis;
  const std::string& left_text = horizontal ? v.cat_title : v.val_title;  // as resolved (a linked cell's text)
  const std::string& bottom_text = horizontal ? v.val_title : v.cat_title;
  const bool left_title = left_axis.has_title && !left_text.empty(), bottom_title = bottom_axis.has_title && !bottom_text.empty();
  const double title_room = lh * 1.2;
  const double px0 = x0 + pad + (left_title ? title_room : 0) + (horizontal ? std::min(cat_w, (x1 - x0) / 3) + 4 * sc : label_w + 4 * sc), px1 = x1 - pad,
               py0 = top + lh * 0.5, py1 = bottom - lh - 2 * sc - (bottom_title ? title_room : 0) - (horizontal || nlev < 2 ? 0.0 : lh * static_cast<double>(nlev - 1));
  if (px1 <= px0 || py1 <= py0) return;
  if (left_title) g.RotatedText(Wide(left_text), x0 + pad, py0, x0 + pad + lh, py1, 1, 1, 90, text, nullptr);
  if (bottom_title) g.Text(Wide(bottom_text), px0, py1 + lh + 4 * sc, px1, py1 + lh + 4 * sc + lh, 1, 0, false, text);
  if (ch.plot_fill) g.Fill(px0, py0, px1, py1, g.Ink(ArgbRef(ch.plot_fill, RGB(255, 255, 255))));
  auto val_text = [&](double t) { return ch.val_axis.number_format.empty() ? Num(t) : Wide(engine::FormatChartNumber(t, ch.val_axis.number_format)); };
  auto vy = [&](double t) { return py1 - (t - lo) / (hi - lo) * (py1 - py0); };
  auto vx = [&](double t) { return px0 + (t - lo) / (hi - lo) * (px1 - px0); };
  // gridlines and value labels
  for (double t = lo; t <= hi + step * 0.01; t += step) {
    if (horizontal) {
      if (ch.val_axis.gridlines) g.VLine(vx(t), py0, py1, 0.5, RGB(0xD9, 0xD9, 0xD9));
      if (ch.val_axis.visible) g.Text(val_text(t), vx(t) - 30 * sc, py1 + 2 * sc, vx(t) + 30 * sc, py1 + 2 * sc + lh, 1, 0, false, text);
    } else {
      if (ch.val_axis.gridlines) g.HLine(px0, px1, vy(t), 0.5, RGB(0xD9, 0xD9, 0xD9));
      if (ch.val_axis.visible) g.Text(val_text(t), x0 + pad, vy(t) - lh / 2, px0 - 4 * sc, vy(t) + lh / 2, 2, 1, false, text);
    }
  }
  if (horizontal) g.VLine(vx(std::clamp(0.0, lo, hi)), py0, py1, 0.75, axis);
  else g.HLine(px0, px1, vy(std::clamp(0.0, lo, hi)), 0.75, axis);
  // a date axis (as on the screen): each point at its date, the days (months, years) between them kept as gaps
  const engine::DatePlan dplan = !horizontal && !scatter ? engine::PlanDateAxis(ch, v, n) : engine::DatePlan{};
  const double slots = dplan.on ? static_cast<double>(dplan.u1 - dplan.u0 + 1) : static_cast<double>(n);
  const double slot = ((horizontal ? py1 - py0 : px1 - px0)) / slots;
  auto date_at = [&](size_t k) { return k < dplan.pos.size() && std::isfinite(dplan.pos[k]) ? dplan.pos[k] - 1 : static_cast<double>(k); };
  auto cat = [&](double i) {  // 0-based point -> its center on paper
    double at = i;
    if (dplan.on) {
      const size_t k = static_cast<size_t>(std::max(0.0, std::floor(i)));
      const double f = i - static_cast<double>(k);
      at = date_at(k) + (f > 0 ? (date_at(k + 1) - date_at(k)) * f : 0.0);
    }
    return (horizontal ? py0 : px0) + slot * (at + 0.5);
  };
  auto sx = [&](double x) { return px0 + (x - xlo) / (xhi - xlo) * (px1 - px0); };
  // category labels (at most ~12)
  double maxw = 0;
  for (size_t i = 0; i < n && i < 200; ++i) maxw = std::max(maxw, g.Width(inner_name(i)));
  const size_t every = !levels.empty() ? 1 : horizontal ? std::max<size_t>(1, static_cast<size_t>(std::ceil(lh / std::max(1e-6, slot))))
                                  : std::max<size_t>(1, static_cast<size_t>(std::ceil((maxw + 6 * sc) / std::max(1e-6, slot))));
  if (dplan.on && ch.cat_axis.visible) {  // a date axis' labels: every few days (months, years), as on the screen
    const int tu = std::max(dplan.unit, ch.major_unit_scale);
    const std::string dfmt = !ch.cat_axis.number_format.empty() ? ch.cat_axis.number_format : !v.cat_fmt.empty() ? v.cat_fmt : std::string("yyyy/m/d");
    const int64_t t0 = engine::DateUnit(engine::UnitSerial(dplan.u0, dplan.unit), tu), t1 = engine::DateUnit(engine::UnitSerial(dplan.u1, dplan.unit), tu);
    const double lw = g.Width(Wide(engine::FormatChartNumber(engine::UnitSerial(dplan.u1, dplan.unit), dfmt))) + 8 * sc;
    const double unit_w = slot * std::max(1.0, slots / static_cast<double>(std::max<int64_t>(1, t1 - t0 + 1)));
    const int64_t every_t = ch.cat_axis.major >= 1 ? static_cast<int64_t>(ch.cat_axis.major) : std::max<int64_t>(1, static_cast<int64_t>(std::ceil(lw / std::max(1e-6, unit_w))));
    for (int64_t t = t0; t <= t1; t += every_t) {
      const double serial = engine::UnitSerial(t, tu);
      const double at = static_cast<double>(engine::DateUnit(serial, dplan.unit) - dplan.u0);
      if (at < 0 || at > slots) continue;
      const double c = px0 + slot * (at + 0.5);
      g.Text(Wide(engine::FormatChartNumber(serial, dfmt)), c - lw / 2, py1 + 2 * sc, c + lw / 2, py1 + 2 * sc + lh, 1, 0, false, text);
    }
  }
  for (size_t i = 0; i < n && !scatter && !dplan.on && ch.cat_axis.visible; i += every) {
    const std::wstring t = inner_name(i);
    const double c = cat(static_cast<double>(i));
    if (horizontal) g.Text(t, x0 + pad + outer_w, c - lh / 2, px0 - 2 * sc, c + lh / 2, 2, 1, false, text);
    else g.Text(t, c - slot * every / 2.0, py1 + 2 * sc, c + slot * every / 2.0, py1 + 2 * sc + lh, 1, 0, false, text);
  }
  // the outer levels: each group's label centered on it, lines between the groups (as on the screen)
  for (size_t k = 0; k + 1 < nlev && !dplan.on && ch.cat_axis.visible; ++k) {
    double b0 = 0, b1 = 0;
    if (!horizontal) {
      b0 = py1 + 2 * sc + lh * static_cast<double>(nlev - 1 - k);
      b1 = b0 + lh;
    } else {
      b0 = x0 + pad;
      for (size_t q = 0; q < k; ++q) b0 += lev_w[q] + lh * 0.6;
      b1 = b0 + lev_w[k];
    }
    size_t i = 0;
    while (i < levels.size()) {
      size_t j = i + 1;
      auto same = [&](size_t a, size_t b) {
        if (levels[a].size() <= k + 1 || levels[b].size() <= k + 1) return false;
        return std::equal(levels[a].begin(), levels[a].begin() + static_cast<std::ptrdiff_t>(k + 1), levels[b].begin());
      };
      while (j < levels.size() && same(i, j)) ++j;
      const double g0 = cat(static_cast<double>(i) - 0.5), g1 = cat(static_cast<double>(j) - 0.5);
      const std::wstring t = levels[i].size() > k + 1 ? Wide(levels[i][k]) : std::wstring();
      const COLORREF sep = RGB(0xBF, 0xBF, 0xBF);
      if (!horizontal) {
        g.Text(t, g0, b0, g1, b1, 1, 0, false, text);
        g.VLine(g0, py1, b1, 0.5, sep);
        g.VLine(g1, py1, b1, 0.5, sep);
      } else {
        g.Text(t, b0, g0, b1, g1, 2, 1, false, text);
        g.HLine(b0, px0, g0, 0.5, sep);
        g.HLine(b0, px0, g1, 0.5, sep);
      }
      i = j;
    }
  }
  if (scatter) {
    const double xs = NiceStep(xhi - xlo);
    for (double t = std::ceil(xlo / xs) * xs; t <= xhi + xs * 0.01; t += xs) g.Text(Num(t), sx(t) - 30 * sc, py1 + 2 * sc, sx(t) + 30 * sc, py1 + 2 * sc + lh, 1, 0, false, text);
  }
  if (stock) {  // [volume,] [open,] high, low, close: as on the screen (up / down bars, high-low lines, ticks)
    const bool ohlc = ch.type == ChartType::StockOHLC || ch.type == ChartType::StockVOHLC;
    const size_t off = vstock ? 1 : 0;
    if (ns < off + (ohlc ? 4 : 3)) return;
    if (vstock) {  // the volume: columns from the bottom, on a scale of their own
      double vmax = 0;
      for (int i = 0; i < v.series[0].N(); ++i) {
        if (std::isfinite(v.series[0].Y()[i])) vmax = std::max(vmax, v.series[0].Y()[i]);
      }
      const COLORREF vc = g.Ink(color_of(0));
      for (int i = 0; i < v.series[0].N() && vmax > 0; ++i) {
        const double y = v.series[0].Y()[i];
        if (!std::isfinite(y) || y <= 0) continue;
        g.Fill(cat(i) - slot * 0.2, py1 - y / vmax * (py1 - py0) * 0.95, cat(i) + slot * 0.2, py1, vc);
      }
    }
    const auto& so = v.series[off];
    const auto& sh = v.series[off + (ohlc ? 1 : 0)];
    const auto& sl = v.series[off + (ohlc ? 2 : 1)];
    const auto& sc2 = v.series[off + (ohlc ? 3 : 2)];
    const int m = std::min({sh.N(), sl.N(), sc2.N(), ohlc ? so.N() : sh.N()});
    const bool bars = ohlc && ch.up_down_bars;
    const double half = slot * 0.5 / (1.0 + std::clamp(ch.updown_gap, 0, 500) / 100.0);
    const COLORREF up = g.Ink(ch.up_fill ? ArgbRef(ch.up_fill, RGB(0xE5, 0x38, 0x3B)) : RGB(0xE5, 0x38, 0x3B));  // red up, green down, as on screen
    const COLORREF down = g.Ink(ch.down_fill ? ArgbRef(ch.down_fill, RGB(0x16, 0xA3, 0x4A)) : RGB(0x16, 0xA3, 0x4A));
    const COLORREF tick = g.Ink(RGB(0x40, 0x40, 0x40));
    for (int i = 0; i < m; ++i) {
      const double h = sh.Y()[i], l = sl.Y()[i], c = sc2.Y()[i], o = ohlc ? so.Y()[i] : c;
      if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;
      const COLORREF col = c >= o ? up : down;
      const double xc = cat(i);
      if (ch.hi_lo_lines) g.VLine(xc, vy(h), vy(l), 0.75, ch.hilo_line ? g.Ink(ArgbRef(ch.hilo_line, RGB(0x40, 0x40, 0x40))) : bars ? col : tick);
      if (bars) {
        g.Fill(xc - half, vy(std::max(o, c)), xc + half, std::max(vy(std::min(o, c)), vy(std::max(o, c)) + 0.5), col);
      } else {
        g.Fill(xc, vy(c) - 0.4 * sc, xc + slot * 0.25, vy(c) + 0.4 * sc, tick);
        if (ohlc) g.Fill(xc - slot * 0.25, vy(o) - 0.4 * sc, xc, vy(o) + 0.4 * sc, tick);
      }
    }
    return;
  }
  // a point's position on paper: (along the category / x axis, along the value axis)
  auto place = [&](size_t si, double x_data, double i0, double y) {
    double a;
    if (scatter || v.series[si].numeric_x) a = sx(x_data);
    else a = cat(i0);
    const double b = horizontal ? vx(y) : vy(y);
    return std::pair<double, double>{horizontal ? b : a, horizontal ? a : b};
  };
  std::vector<double> acc_pos(n, 0), acc_neg(n, 0);
  const double group = slot * 0.7, bw = stacked ? group : group / static_cast<double>(std::max<size_t>(1, nbars));
  size_t bar_k = 0;
  // bars first, then lines on top
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t si = 0; si < ns; ++si) {
      const auto& se = v.series[si];
      const bool bar = engine::IsBarType(types[si]);
      if ((pass == 0) != bar) continue;
      const COLORREF c = g.Ink(color_of(si));
      const int m = se.N();
      const model::ChartSeries* def = si < ch.series.size() ? &ch.series[si] : nullptr;
      std::vector<std::pair<double, double>> centers(static_cast<size_t>(std::max(0, m)), {std::nan(""), std::nan("")});  // drawn point (x, y) for labels / error bars
      if (bar) {
        const double off = stacked ? -group / 2 : -group / 2 + bw * static_cast<double>(bar_k++);
        for (int i = 0; i < m && static_cast<size_t>(i) < n; ++i) {
          const double val = se.Y()[i];
          if (!std::isfinite(val)) continue;
          double a = 0, b = val;
          if (stacked) {
            double& base = val >= 0 ? acc_pos[static_cast<size_t>(i)] : acc_neg[static_cast<size_t>(i)];
            a = base;
            b = base + val;
            base = b;
          }
          const double c0 = cat(i) + off;
          if (horizontal) g.Fill(vx(std::min(a, b)), c0, vx(std::max(a, b)), c0 + bw, c);
          else g.Fill(c0, vy(std::max(a, b)), c0 + bw, vy(std::min(a, b)), c);
          const double mid = stacked ? (a + b) / 2 : b;
          centers[static_cast<size_t>(i)] = horizontal ? std::pair<double, double>{vx(mid), c0 + bw / 2} : std::pair<double, double>{c0 + bw / 2, vy(mid)};
        }
      } else {
        std::vector<POINT> pts;
        for (int i = 0; i < m; ++i) {
          if (!std::isfinite(se.Y()[i])) continue;
          const auto p = place(si, se.X()[i], i, se.Y()[i]);
          centers[static_cast<size_t>(i)] = p;
          pts.push_back(POINT{Tw(p.first), Tw(p.second)});
        }
        if (!pts.empty()) {
          if (types[si] == ChartType::Area) {
            std::vector<POINT> poly = pts;
            poly.push_back(POINT{pts.back().x, Tw(vy(std::max(lo, 0.0)))});
            poly.push_back(POINT{pts.front().x, Tw(vy(std::max(lo, 0.0)))});
            HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, c);
            ::Polygon(g.dc(), poly.data(), static_cast<int>(poly.size()));
            g.Restore(op, ob);
          } else {
            // the series' marker: its own (Series.MarkerStyle) or the chart type's
            int marker = types[si] == ChartType::Scatter || types[si] == ChartType::LineMarkers ? 1 : 0;
            if (def && def->marker >= 0) marker = def->marker;
            if (types[si] != ChartType::Scatter || marker == 0) {
              const double weight = def && def->line_weight > 0 ? def->line_weight * sc : 2.25 * sc;
              HGDIOBJ op = g.UsePen(true, c, weight), ob = g.UseBrush(false, 0);
              ::Polyline(g.dc(), pts.data(), static_cast<int>(pts.size()));
              g.Restore(op, ob);
            }
            if (marker > 0) {
              const int r = Tw((def && def->marker_size > 0 ? def->marker_size / 2 : 2.5) * sc);
              const bool lines = marker == 5 || marker == 6;  // X and + are strokes
              HGDIOBJ op = g.UsePen(lines, c, 1.0 * sc), ob = g.UseBrush(!lines, c);
              for (const POINT& p : pts) {
                switch (marker) {
                  case 2: ::Rectangle(g.dc(), p.x - r, p.y - r, p.x + r, p.y + r); break;
                  case 3: {
                    const POINT d[4] = {{p.x, p.y - r}, {p.x + r, p.y}, {p.x, p.y + r}, {p.x - r, p.y}};
                    ::Polygon(g.dc(), d, 4);
                    break;
                  }
                  case 4: {
                    const POINT t[3] = {{p.x, p.y - r}, {p.x + r, p.y + r}, {p.x - r, p.y + r}};
                    ::Polygon(g.dc(), t, 3);
                    break;
                  }
                  case 5:
                    ::MoveToEx(g.dc(), p.x - r, p.y - r, nullptr);
                    ::LineTo(g.dc(), p.x + r, p.y + r);
                    ::MoveToEx(g.dc(), p.x - r, p.y + r, nullptr);
                    ::LineTo(g.dc(), p.x + r, p.y - r);
                    break;
                  case 6:
                    ::MoveToEx(g.dc(), p.x - r, p.y, nullptr);
                    ::LineTo(g.dc(), p.x + r, p.y);
                    ::MoveToEx(g.dc(), p.x, p.y - r, nullptr);
                    ::LineTo(g.dc(), p.x, p.y + r);
                    break;
                  default: ::Ellipse(g.dc(), p.x - r, p.y - r, p.x + r, p.y + r); break;
                }
              }
              g.Restore(op, ob);
            }
          }
        }
      }
      if (!def) continue;
      // trendline
      if (def->trend != model::TrendKind::None && m > 1) {
        std::vector<POINT> tp;
        // drawing offset of this series' points from the category center (clustered columns)
        double shift = 0;
        for (int i = 0; i < m; ++i) {
          if (std::isfinite(centers[static_cast<size_t>(i)].first) && !scatter && !se.numeric_x) {
            shift = (horizontal ? centers[static_cast<size_t>(i)].second : centers[static_cast<size_t>(i)].first) - cat(i);
            break;
          }
        }
        auto to_paper = [&](double xd, double y) {
          const double a = (scatter || se.numeric_x) ? sx(xd) : cat(xd - 1) + shift;
          const double b = horizontal ? vx(std::clamp(y, lo, hi)) : vy(std::clamp(y, lo, hi));
          return POINT{Tw(horizontal ? b : a), Tw(horizontal ? a : b)};
        };
        if (def->trend == model::TrendKind::MovingAverage) {
          const std::vector<double> ma = engine::MovingAverage(se.Y(), m, def->trend_period);
          for (int i = 0; i < m; ++i) {
            if (std::isfinite(ma[static_cast<size_t>(i)])) tp.push_back(to_paper(se.X()[i], ma[static_cast<size_t>(i)]));
          }
        } else {
          const engine::TrendFit f = engine::FitTrend(se.X(), se.Y(), m, def->trend, def->trend_order);
          if (f.ok) {
            double a = 1e300, b = -1e300;
            for (int i = 0; i < m; ++i) {
              if (std::isfinite(se.X()[i]) && std::isfinite(se.Y()[i])) a = std::min(a, se.X()[i]), b = std::max(b, se.X()[i]);
            }
            a -= std::max(0.0, def->trend_backward);
            b += std::max(0.0, def->trend_forward);
            if ((def->trend == model::TrendKind::Logarithmic || def->trend == model::TrendKind::Power) && a <= 0) a = std::max(b * 1e-3, 1e-9);
            for (int k = 0; k <= 60 && b >= a; ++k) {
              const double xd = a + (b - a) * k / 60, y = f.Eval(xd);
              if (std::isfinite(y)) tp.push_back(to_paper(xd, y));
            }
            if ((def->trend_eq || def->trend_r2) && !tp.empty()) {
              std::string t = def->trend_eq ? f.Equation() : std::string();
              if (def->trend_r2) {
                char r2[64];
                std::snprintf(r2, sizeof r2, "R\xC2\xB2 = %.4f", f.r2);
                t += (t.empty() ? "" : "  ") + std::string(r2);
              }
              const double ex = tp.back().x / kTw, ey = tp.back().y / kTw;
              g.Text(Wide(t), ex - 160 * sc, ey - lh - 2 * sc, ex, ey - 2 * sc, 2, 2, false, text);
            }
          }
        }
        if (tp.size() >= 2) {
          HGDIOBJ op = g.UsePen(true, c, 1.25 * sc), ob = g.UseBrush(false, 0);
          ::Polyline(g.dc(), tp.data(), static_cast<int>(tp.size()));
          g.Restore(op, ob);
        }
      }
      // error bars
      if (def->err != model::ErrorBarKind::None && !(bar && stacked)) {
        bool at_mean = false;
        double mean = 0;
        std::vector<double> neg, pos;
        engine::ErrorExtents(*def, se, m, &neg, &pos, &at_mean, &mean);
        const COLORREF ec = g.Ink(RGB(0x40, 0x40, 0x40));
        for (int i = 0; i < m; ++i) {
          const auto& p = centers[static_cast<size_t>(i)];
          if (!std::isfinite(p.first)) continue;
          const double y = at_mean ? mean : se.Y()[i], dn = neg[static_cast<size_t>(i)], dp = pos[static_cast<size_t>(i)];
          if (horizontal) {
            const double a = vx(std::clamp(y - dn, lo, hi)), b = vx(std::clamp(y + dp, lo, hi));
            g.HLine(a, b, p.second, 0.75, ec);
            g.VLine(a, p.second - 2 * sc, p.second + 2 * sc, 0.75, ec);
            g.VLine(b, p.second - 2 * sc, p.second + 2 * sc, 0.75, ec);
          } else {
            const double a = vy(std::clamp(y - dn, lo, hi)), b = vy(std::clamp(y + dp, lo, hi));
            g.VLine(p.first, b, a, 0.75, ec);
            g.HLine(p.first - 2 * sc, p.first + 2 * sc, a, 0.75, ec);
            g.HLine(p.first - 2 * sc, p.first + 2 * sc, b, 0.75, ec);
          }
        }
      }
      // data labels
      if (def) {
        for (int i = 0; i < m && i < 500; ++i) {
          if (!label_on(*def, i)) continue;
          double dx = 0, dy = 0;
          label_shift(*def, i, &dx, &dy);
          const std::pair<double, double> p(centers[static_cast<size_t>(i)].first + dx, centers[static_cast<size_t>(i)].second + dy);
          const double y = se.Y()[i];
          if (!std::isfinite(p.first) || !std::isfinite(y)) continue;
          const std::wstring t = label_text(si, i, Wide(engine::FormatChartNumber(y, se.fmt)));
          if (bar && stacked) g.Text(t, p.first - 40 * sc, p.second - lh / 2, p.first + 40 * sc, p.second + lh / 2, 1, 1, false, RGB(255, 255, 255));
          else if (horizontal) g.Text(t, p.first + 2 * sc, p.second - lh / 2, p.first + 80 * sc, p.second + lh / 2, 0, 1, false, text);
          else if (y >= 0) g.Text(t, p.first - 40 * sc, p.second - lh - 2 * sc, p.first + 40 * sc, p.second - 2 * sc, 1, 2, false, text);
          else g.Text(t, p.first - 40 * sc, p.second + 2 * sc, p.first + 40 * sc, p.second + lh + 2 * sc, 1, 0, false, text);
        }
      }
    }
  }
}

// ---- pictures: decoded once per print job ----
struct Pixels {
  std::vector<uint8_t> bgr;  // 32-bit BGRX over white, top-down
  int w = 0, h = 0;
};

const Pixels* PixelsOf(const std::string& image, std::map<const void*, std::unique_ptr<Pixels>>& cache) {
  auto it = cache.find(image.data());
  if (it != cache.end()) return it->second.get();
  auto px = std::make_unique<Pixels>();
  std::vector<uint8_t> rgba;
  if (!DecodeImageRgba(image, &rgba, &px->w, &px->h) || px->w <= 0 || px->h <= 0) px.reset();
  if (px) {
    px->bgr.resize(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const unsigned a = rgba[i + 3];
      px->bgr[i + 0] = static_cast<uint8_t>((rgba[i + 2] * a + 255 * (255 - a)) / 255);
      px->bgr[i + 1] = static_cast<uint8_t>((rgba[i + 1] * a + 255 * (255 - a)) / 255);
      px->bgr[i + 2] = static_cast<uint8_t>((rgba[i + 0] * a + 255 * (255 - a)) / 255);
      px->bgr[i + 3] = 0;
    }
  }
  return cache.emplace(image.data(), std::move(px)).first->second.get();
}

void DrawPixels(HDC dc, const Pixels& px, double x0, double y0, double x1, double y1) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = px.w;
  bi.bmiHeader.biHeight = -px.h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  ::SetStretchBltMode(dc, HALFTONE);
  ::SetBrushOrgEx(dc, 0, 0, nullptr);
  ::StretchDIBits(dc, Tw(x0), Tw(y0), Tw(x1) - Tw(x0), Tw(y1) - Tw(y0), 0, 0, px.w, px.h, px.bgr.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

const Pixels* PictureOf(const model::ShapeObj& s, std::map<const void*, std::unique_ptr<Pixels>>& cache) {
  auto it = cache.find(s.image.data());
  if (it != cache.end()) return it->second.get();
  auto px = std::make_unique<Pixels>();
  std::vector<uint8_t> rgba;
  if (!DecodeImageRgba(s.image, &rgba, &px->w, &px->h) || px->w <= 0 || px->h <= 0) px.reset();
  if (px) {
    px->bgr.resize(rgba.size());
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const unsigned a = rgba[i + 3];
      px->bgr[i + 0] = static_cast<uint8_t>((rgba[i + 2] * a + 255 * (255 - a)) / 255);
      px->bgr[i + 1] = static_cast<uint8_t>((rgba[i + 1] * a + 255 * (255 - a)) / 255);
      px->bgr[i + 2] = static_cast<uint8_t>((rgba[i + 0] * a + 255 * (255 - a)) / 255);
      px->bgr[i + 3] = 0;
    }
  }
  return cache.emplace(s.image.data(), std::move(px)).first->second.get();
}

// a shape as it is on the sheet, before it is turned (DrawShape turns the page under it)
void DrawShapeFlat(Gdi& g, const model::ShapeObj& s, double x0, double y0, double x1, double y1, double sc, std::map<const void*, std::unique_ptr<Pixels>>& pics) {
  using model::ShapeKind;
  HDC dc = g.dc();
  if (s.kind == ShapeKind::Picture) {
    const Pixels* px = PictureOf(s, pics);
    if (!px) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = px->w;
    bi.bmiHeader.biHeight = -px->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    ::SetStretchBltMode(dc, HALFTONE);
    ::SetBrushOrgEx(dc, 0, 0, nullptr);
    ::StretchDIBits(dc, Tw(x0), Tw(y0), Tw(x1) - Tw(x0), Tw(y1) - Tw(y0), 0, 0, px->w, px->h, px->bgr.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    if (s.line_visible && s.line_weight > 0) {
      HGDIOBJ op = g.UsePen(true, g.Ink(Cr(s.line_color)), s.line_weight * sc), ob = g.UseBrush(false, 0);
      ::Rectangle(dc, Tw(x0), Tw(y0), Tw(x1), Tw(y1));
      g.Restore(op, ob);
    }
    return;
  }
  const COLORREF fill = Cr(s.fill_color), line = g.Ink(Cr(s.line_color));
  const bool fill_on = s.fill_visible && !g.bw(), line_on = s.line_visible && s.line_weight > 0;
  auto arrow_head = [&](double ax, double ay, double bx, double by) {  // an arrowhead at (bx, by) coming from (ax, ay)
    const double len = std::hypot(bx - ax, by - ay);
    if (len <= 0) return;
    const double ux = (bx - ax) / len, uy = (by - ay) / len, hl = std::max(4.0, static_cast<double>(s.line_weight) * 4) * sc;
    POINT tri[3] = {{Tw(bx), Tw(by)}, {Tw(bx - ux * hl - uy * hl * 0.5), Tw(by - uy * hl + ux * hl * 0.5)}, {Tw(bx - ux * hl + uy * hl * 0.5), Tw(by - uy * hl - ux * hl * 0.5)}};
    HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, line);
    ::Polygon(dc, tri, 3);
    g.Restore(op, ob);
  };
  // an elbow or curved connector: its path, with its heads
  if ((s.kind == ShapeKind::Line || s.kind == ShapeKind::Arrow) && s.connector >= 2) {
    const double ax = s.flip_h ? x1 : x0, ay = s.flip_v ? y1 : y0, bx = s.flip_h ? x0 : x1, by = s.flip_v ? y0 : y1;
    const auto path = engine::ConnectorPath(s.connector, {ax, ay}, {bx, by});
    std::vector<POINT> pts;
    for (const auto& q : path) pts.push_back({Tw(q.x), Tw(q.y)});
    HGDIOBJ op = g.UsePen(line_on, line, s.line_weight * sc), ob = g.UseBrush(false, 0);
    ::Polyline(dc, pts.data(), static_cast<int>(pts.size()));
    g.Restore(op, ob);
    const size_t n = path.size();
    if (s.kind == ShapeKind::Arrow || s.end_arrow) arrow_head(path[n - 2].x, path[n - 2].y, path[n - 1].x, path[n - 1].y);
    if (s.begin_arrow) arrow_head(path[1].x, path[1].y, path[0].x, path[0].y);
    return;
  }
  // a preset shape of the Shapes gallery or a freeform: its polygons and lines, flipped as on the sheet
  if (engine::ShapeGeom geo; (s.auto_type || !s.path.empty()) && engine::ShapeObjGeometry(s, x1 - x0, y1 - y0, &geo)) {
    auto at = [&](const engine::ShapePt& q) {
      const double x = s.flip_h ? x1 - q.x : x0 + q.x, y = s.flip_v ? y1 - q.y : y0 + q.y;
      return POINT{Tw(x), Tw(y)};
    };
    for (const auto& f : geo.fills) {
      if (!fill_on || f.pts.size() < 3) continue;
      std::vector<POINT> pts;
      for (const auto& q : f.pts) pts.push_back(at(q));
      const COLORREF c = f.shade < 0 ? Mix(fill, RGB(0, 0, 0), -0.2 * f.shade) : f.shade > 0 ? Mix(fill, RGB(255, 255, 255), 0.3) : fill;
      HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, c);
      ::Polygon(dc, pts.data(), static_cast<int>(pts.size()));
      g.Restore(op, ob);
    }
    for (const auto& st : geo.strokes) {
      if (!line_on || st.pts.size() < 2) continue;
      std::vector<POINT> pts;
      for (const auto& q : st.pts) pts.push_back(at(q));
      if (st.closed) pts.push_back(pts.front());
      HGDIOBJ op = g.UsePen(true, line, s.line_weight * sc), ob = g.UseBrush(false, 0);
      ::Polyline(dc, pts.data(), static_cast<int>(pts.size()));
      g.Restore(op, ob);
    }
    if (!s.path.empty() && !s.path_closed && line_on && !geo.strokes.empty() && geo.strokes[0].pts.size() >= 2) {
      const auto& v = geo.strokes[0].pts;
      const POINT a = at(v[v.size() - 2]), b = at(v.back()), c0 = at(v[1]), c1 = at(v[0]);
      if (s.end_arrow) arrow_head(a.x / kTw, a.y / kTw, b.x / kTw, b.y / kTw);
      if (s.begin_arrow) arrow_head(c0.x / kTw, c0.y / kTw, c1.x / kTw, c1.y / kTw);
    }
    if (!s.text.empty() && (s.path.empty() || s.path_closed)) {
      ::SelectObject(dc, g.Font(s.font_name.empty() ? "Calibri" : s.font_name, s.font_size * sc, s.bold, s.italic));
      g.Text(Wide(s.text), x0 + s.margin_l * sc, y0 + s.margin_t * sc, x1 - s.margin_r * sc, y1 - s.margin_b * sc, s.halign, s.valign, true, Cr(s.font_color));
    }
    return;
  }
  if (s.kind == ShapeKind::Line || s.kind == ShapeKind::Arrow) {
    const double ax = s.flip_h ? x1 : x0, ay = s.flip_v ? y1 : y0, bx = s.flip_h ? x0 : x1, by = s.flip_v ? y0 : y1;
    g.Line(ax, ay, bx, by, s.line_weight * sc, line);
    if (s.kind == ShapeKind::Arrow) {
      const double len = std::hypot(bx - ax, by - ay);
      if (len > 0) {
        const double ux = (bx - ax) / len, uy = (by - ay) / len, hl = std::max(4.0, static_cast<double>(s.line_weight) * 4) * sc;
        POINT tri[3] = {{Tw(bx), Tw(by)}, {Tw(bx - ux * hl - uy * hl * 0.5), Tw(by - uy * hl + ux * hl * 0.5)}, {Tw(bx - ux * hl + uy * hl * 0.5), Tw(by - uy * hl - ux * hl * 0.5)}};
        HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, line);
        ::Polygon(dc, tri, 3);
        g.Restore(op, ob);
      }
    }
    return;
  }
  HGDIOBJ op = g.UsePen(line_on, line, s.line_weight * sc), ob = g.UseBrush(fill_on, fill);
  const int l = Tw(x0), t = Tw(y0), r = Tw(x1), b = Tw(y1);
  switch (s.kind) {
    case ShapeKind::RoundedRectangle: {
      const int rr = static_cast<int>(std::min(r - l, b - t) * 0.33);
      ::RoundRect(dc, l, t, r, b, rr, rr);
      break;
    }
    case ShapeKind::Oval: ::Ellipse(dc, l, t, r, b); break;
    case ShapeKind::Triangle: {
      POINT p[3] = {{(l + r) / 2, t}, {r, b}, {l, b}};
      ::Polygon(dc, p, 3);
      break;
    }
    case ShapeKind::RightArrow: {
      const int head = std::min(r - l, (b - t)) / 2, sh0 = t + (b - t) / 4, sh1 = b - (b - t) / 4, hx = r - head;
      POINT p[7] = {{l, sh0}, {hx, sh0}, {hx, t}, {r, (t + b) / 2}, {hx, b}, {hx, sh1}, {l, sh1}};
      ::Polygon(dc, p, 7);
      break;
    }
    default: ::Rectangle(dc, l, t, r, b); break;
  }
  g.Restore(op, ob);
  if (!s.text.empty()) {
    ::SelectObject(dc, g.Font(s.font_name.empty() ? "Calibri" : s.font_name, s.font_size * sc, s.bold, s.italic));
    const double ix = 7.2 * sc, iy = 3.6 * sc;
    // WordArt: the shadow and the letters' outline under the letters
    if (s.text_shadow && !g.bw()) {
      const double o = std::max(0.5, s.font_size * 0.04) * sc;
      g.Text(Wide(s.text), x0 + ix + o, y0 + iy + o, x1 - ix + o, y1 - iy + o, s.halign, s.valign, true, RGB(0xA0, 0xA0, 0xA0));
    }
    if (s.text_outline) {
      const double o = std::max(0.3, s.font_size * 0.02) * sc;
      for (const auto& [dx, dy] : {std::pair{-o, 0.0}, std::pair{o, 0.0}, std::pair{0.0, -o}, std::pair{0.0, o}})
        g.Text(Wide(s.text), x0 + ix + dx, y0 + iy + dy, x1 - ix + dx, y1 - iy + dy, s.halign, s.valign, true, Cr(s.text_outline));
    }
    g.Text(Wide(s.text), x0 + ix, y0 + iy, x1 - ix, y1 - iy, s.halign, s.valign, true, Cr(s.font_color));
  }
}

// a control on the page as it shows on the sheet (Format Control > Properties > Print object): its box, its state and
// its caption, drawn plainly
void DrawControlGdi(Gdi& g, const model::Control& c, double x0, double y0, double x1, double y1, double sc) {
  using K = model::ControlKind;
  HDC dc = g.dc();
  auto bgr = [](uint32_t v, COLORREF def) { return v ? RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF) : def; };
  const COLORREF face = g.bw() ? RGB(255, 255, 255) : bgr(c.back_color, RGB(0xF0, 0xF0, 0xF0)), ink = g.Ink(bgr(c.fore_color, RGB(0, 0, 0)));
  const COLORREF border = g.Ink(RGB(0x80, 0x80, 0x80)), white = RGB(255, 255, 255);
  auto box = [&](double a, double b, double cc, double d, COLORREF fill) {
    HGDIOBJ op = g.UsePen(true, border, 0.75 * sc), ob = g.UseBrush(true, fill);
    ::Rectangle(dc, Tw(a), Tw(b), Tw(cc), Tw(d));
    g.Restore(op, ob);
  };
  ::SelectObject(dc, g.Font(c.font_name.empty() ? "Calibri" : c.font_name, (c.font_size > 0 ? c.font_size : 11) * sc, c.bold, c.italic));
  const bool on = model::ControlIsOn(c);
  const double h = y1 - y0, sq = std::min(h, 9.0 * sc);
  switch (c.kind) {
    case K::Button:
    case K::ToggleButton:
      box(x0, y0, x1, y1, on && c.kind == K::ToggleButton ? Mix(face, RGB(0, 0, 0), 0.15) : face);
      g.Text(Wide(c.caption), x0, y0, x1, y1, 1, 1, c.word_wrap, ink);
      break;
    case K::CheckBox:
    case K::OptionButton: {
      const double cy = (y0 + y1) / 2;
      if (c.kind == K::CheckBox) {
        box(x0 + 1 * sc, cy - sq / 2, x0 + 1 * sc + sq, cy + sq / 2, white);
        if (on) {
          g.Line(x0 + 2.5 * sc, cy, x0 + 1 * sc + sq * 0.4, cy + sq * 0.3, 1.0 * sc, ink);
          g.Line(x0 + 1 * sc + sq * 0.4, cy + sq * 0.3, x0 + sq, cy - sq * 0.3, 1.0 * sc, ink);
        }
      } else {
        HGDIOBJ op = g.UsePen(true, border, 0.75 * sc), ob = g.UseBrush(true, white);
        ::Ellipse(dc, Tw(x0 + 1 * sc), Tw(cy - sq / 2), Tw(x0 + 1 * sc + sq), Tw(cy + sq / 2));
        g.Restore(op, ob);
        if (on) {
          HGDIOBJ op2 = g.UsePen(false, 0, 0), ob2 = g.UseBrush(true, ink);
          ::Ellipse(dc, Tw(x0 + 1 * sc + sq * 0.3), Tw(cy - sq * 0.2), Tw(x0 + 1 * sc + sq * 0.7), Tw(cy + sq * 0.2));
          g.Restore(op2, ob2);
        }
      }
      g.Text(Wide(c.caption), x0 + sq + 4 * sc, y0, x1, y1, 0, 1, c.word_wrap, ink);
      break;
    }
    case K::ComboBox:
    case K::TextBox: {
      box(x0, y0, x1, y1, white);
      std::string text = c.value;
      if (c.kind == K::ComboBox && c.list_index >= 0 && static_cast<size_t>(c.list_index) < c.items.size() && text.empty()) text = c.items[static_cast<size_t>(c.list_index)];
      const double bw = c.kind == K::ComboBox ? std::min(h, 12.0 * sc) : 0;
      if (bw > 0) box(x1 - bw, y0, x1, y1, face);
      g.Text(Wide(text), x0 + 2 * sc, y0, x1 - bw - 2 * sc, y1, 0, 1, false, ink);
      break;
    }
    case K::ListBox: {
      box(x0, y0, x1, y1, white);
      const double lh = std::max(1.0, g.LineHeight());
      double y = y0 + 1 * sc;
      for (const auto& it : c.items) {
        if (y + lh > y1) break;
        g.Text(Wide(it), x0 + 2 * sc, y, x1 - 2 * sc, y + lh, 0, 0, false, ink);
        y += lh;
      }
      break;
    }
    case K::Label: g.Text(Wide(c.caption), x0, y0, x1, y1, 0, 0, c.word_wrap, ink); break;
    case K::GroupBox: {
      const double ty = y0 + 5 * sc;
      HGDIOBJ op = g.UsePen(true, border, 0.75 * sc), ob = g.UseBrush(false, 0);
      ::Rectangle(dc, Tw(x0), Tw(ty), Tw(x1), Tw(y1));
      g.Restore(op, ob);
      const double tw = g.Width(Wide(c.caption));
      g.Fill(x0 + 6 * sc, y0, x0 + 8 * sc + tw, y0 + 10 * sc, white);
      g.Text(Wide(c.caption), x0 + 7 * sc, y0, x1, y0 + 10 * sc, 0, 1, false, ink);
      break;
    }
    case K::SpinButton:
    case K::ScrollBar:
      box(x0, y0, x1, y1, face);
      break;
    default: box(x0, y0, x1, y1, face); break;
  }
}

// a shape on the page, turned about its center as on the sheet (Format Shape > Size > Rotation)
void DrawShape(Gdi& g, const model::ShapeObj& s, double x0, double y0, double x1, double y1, double sc, std::map<const void*, std::unique_ptr<Pixels>>& pics) {
  HDC dc = g.dc();
  const int saved = ::SaveDC(dc);
  if (s.rotation != 0) {
    ::SetGraphicsMode(dc, GM_ADVANCED);
    const double a = s.rotation * 3.14159265358979323846 / 180.0, ca = std::cos(a), sa = std::sin(a);
    const double cx = Tw((x0 + x1) / 2), cy = Tw((y0 + y1) / 2);
    XFORM xf{};
    xf.eM11 = static_cast<FLOAT>(ca);
    xf.eM12 = static_cast<FLOAT>(sa);
    xf.eM21 = static_cast<FLOAT>(-sa);
    xf.eM22 = static_cast<FLOAT>(ca);
    xf.eDx = static_cast<FLOAT>(cx - cx * ca + cy * sa);
    xf.eDy = static_cast<FLOAT>(cy - cx * sa - cy * ca);
    ::ModifyWorldTransform(dc, &xf, MWT_LEFTMULTIPLY);
  }
  DrawShapeFlat(g, s, x0, y0, x1, y1, sc, pics);
  ::RestoreDC(dc, saved);
}

// Format Shape > Effects on paper: glow and shadows as tinted copies of the shape (mixed with the white paper), the
// bevel as a light inner rim, soft edges fading into the paper, the reflection below, the 3-D rotation as the nearest
// flat transform
void DrawShapeWithEffects(Gdi& g, const model::ShapeObj& s, double x0, double y0, double x1, double y1, double sc,
                          std::map<const void*, std::unique_ptr<Pixels>>& pics) {
  const model::ShapeEffects fx = model::EffectiveEffects(s.fx);
  model::ShapeObj plain = s;
  plain.fx = model::ShapeEffects{};
  const bool any = fx.shadow || fx.refl_size > 0 || fx.glow_size > 0 || fx.soft_edge > 0 || fx.bevel_top || fx.bevel_bottom || fx.rot_x != 0 ||
                   fx.rot_y != 0 || fx.rot_z != 0;
  if (!any || g.bw()) {
    DrawShape(g, plain, x0, y0, x1, y1, sc, pics);
    return;
  }
  HDC dc = g.dc();
  const int saved = ::SaveDC(dc);
  if (fx.rot_x != 0 || fx.rot_y != 0 || fx.rot_z != 0) {
    // turned around the shape's center: z in the page, x and y foreshorten the width / height and lean it
    const double ax = fx.rot_x * 3.14159265358979 / 180.0, ay = fx.rot_y * 3.14159265358979 / 180.0, az = fx.rot_z * 3.14159265358979 / 180.0;
    const double cz = std::cos(az), sz = std::sin(az), kx = std::cos(ax), ky = std::cos(ay), lean = std::sin(ax) * std::sin(ay) * 0.5;
    XFORM xf{};
    xf.eM11 = static_cast<FLOAT>(cz * kx);
    xf.eM12 = static_cast<FLOAT>(sz * ky + lean);
    xf.eM21 = static_cast<FLOAT>(-sz * kx);
    xf.eM22 = static_cast<FLOAT>(cz * ky);
    const double cx = Tw((x0 + x1) / 2), cy = Tw((y0 + y1) / 2);
    xf.eDx = static_cast<FLOAT>(cx - cx * xf.eM11 - cy * xf.eM21);
    xf.eDy = static_cast<FLOAT>(cy - cx * xf.eM12 - cy * xf.eM22);
    ::SetGraphicsMode(dc, GM_ADVANCED);
    ::ModifyWorldTransform(dc, &xf, MWT_LEFTMULTIPLY);
  }
  auto mix = [](uint32_t argb, double opacity) -> uint32_t {  // the color over white paper
    auto ch = [&](int shift) { return static_cast<uint32_t>(255 + (static_cast<double>((argb >> shift) & 0xFF) - 255) * std::clamp(opacity, 0.0, 1.0)); };
    return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
  };
  auto silhouette = [&](uint32_t argb) {
    model::ShapeObj c = plain;
    c.text.clear();
    if (c.kind == model::ShapeKind::Picture) {
      c.kind = model::ShapeKind::Rectangle;
      c.image.clear();
    }
    c.fill_visible = c.line_visible = true;
    c.fill_transparency = 0;
    c.fill_color = c.line_color = argb;
    return c;
  };
  if (fx.glow_size > 0) {
    for (int layer = 2; layer >= 1; --layer) {
      const double e = fx.glow_size * layer / 2.0 * sc;
      DrawShape(g, silhouette(mix(fx.glow_color, (1 - fx.glow_transparency) * (layer == 2 ? 0.35 : 0.6))), x0 - e, y0 - e, x1 + e, y1 + e, sc, pics);
    }
  }
  if (fx.shadow == 1 || fx.shadow == 3) {
    const double a = fx.shadow_angle * 3.14159265358979 / 180.0;
    const double dx = std::cos(a) * fx.shadow_distance * sc, dy = std::sin(a) * fx.shadow_distance * sc;
    const double k = std::max(0.1, fx.shadow_size / 100.0), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double sx0 = cx + (x0 - cx) * k + dx, sy0 = cy + (y0 - cy) * k + dy, sx1 = cx + (x1 - cx) * k + dx, sy1 = cy + (y1 - cy) * k + dy;
    if (fx.shadow == 3) sy0 = sy1 - (sy1 - sy0) * 0.33;  // on the ground behind the shape
    // the blur: a wider, lighter copy under a narrower, darker one
    const double b = fx.shadow_blur * sc * 0.5;
    if (b > 0.2) DrawShape(g, silhouette(mix(fx.shadow_color, (1 - fx.shadow_transparency) * 0.5)), sx0 - b, sy0 - b, sx1 + b, sy1 + b, sc, pics);
    DrawShape(g, silhouette(mix(fx.shadow_color, 1 - fx.shadow_transparency)), sx0, sy0, sx1, sy1, sc, pics);
  }
  DrawShape(g, plain, x0, y0, x1, y1, sc, pics);
  auto ring = [&](uint32_t argb, double inset, double weight) {
    model::ShapeObj c = plain;
    c.text.clear();
    c.fill_visible = false;
    c.line_visible = true;
    c.line_color = argb;
    c.line_weight = static_cast<float>(weight);
    c.line_dash = 0;
    if (c.kind == model::ShapeKind::Picture) c.kind = model::ShapeKind::Rectangle, c.image.clear();
    if (x1 - x0 > 2 * inset && y1 - y0 > 2 * inset) DrawShape(g, c, x0 + inset, y0 + inset, x1 - inset, y1 - inset, sc, pics);
  };
  if (fx.shadow == 2) ring(mix(fx.shadow_color, (1 - fx.shadow_transparency) * 0.45), (fx.shadow_distance + fx.shadow_blur) * sc * 0.5,
                           std::max(0.5, (fx.shadow_distance + fx.shadow_blur) * 0.5));
  if (fx.bevel_top) ring(0xFFFFFFFFu, fx.bevel_top_w * sc * 0.5, std::max(0.5f, fx.bevel_top_w * 0.35f));
  if (fx.bevel_bottom) ring(mix(0xFF000000u, 0.25), fx.bevel_bottom_w * sc * 0.25, std::max(0.5f, fx.bevel_bottom_w * 0.25f));
  if (fx.soft_edge > 0) ring(0xFFFFFFFFu, 0, fx.soft_edge * 1.2);  // the edge fades into the paper
  if (fx.refl_size > 0 && s.kind != model::ShapeKind::Picture) {
    model::ShapeObj m = plain;
    m.flip_v = !m.flip_v;
    m.text.clear();
    m.fill_color = mix(m.fill_color, (1 - fx.refl_transparency) * 0.6);
    m.line_color = mix(m.line_color, (1 - fx.refl_transparency) * 0.6);
    const double gap = fx.refl_distance * sc, h = y1 - y0;
    const int clip = ::SaveDC(dc);
    ::IntersectClipRect(dc, Tw(x0 - 2), Tw(y1 + gap), Tw(x1 + 2), Tw(y1 + gap + h * fx.refl_size / 100.0));
    DrawShape(g, m, x0, y1 + gap, x1, y1 + gap + h, sc, pics);
    ::RestoreDC(dc, clip);
  }
  if (fx.refl_size > 0 && s.kind == model::ShapeKind::Picture) {
    // a picture's reflection: its rows flipped into a 32-bit premultiplied-alpha bitmap whose alpha fades out down the
    // reflected part, blended onto the page (printer, PDF and the preview bitmap alike)
    if (const Pixels* px = PictureOf(s, pics)) {
      const int rows = std::max(1, static_cast<int>(px->h * std::clamp(fx.refl_size, 1.0f, 100.0f) / 100.0f));
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = px->w;
      bi.bmiHeader.biHeight = -rows;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      HDC mem = ::CreateCompatibleDC(dc);
      HBITMAP bmp = mem ? ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
      if (bmp && bits) {
        auto* out = static_cast<uint8_t*>(bits);
        const double top = std::clamp(1.0 - fx.refl_transparency, 0.0, 1.0);
        for (int y = 0; y < rows; ++y) {
          const uint8_t* src = px->bgr.data() + static_cast<size_t>(px->h - 1 - y) * static_cast<size_t>(px->w) * 4;
          uint8_t* dst = out + static_cast<size_t>(y) * static_cast<size_t>(px->w) * 4;
          const double a = top * (1.0 - static_cast<double>(y) / rows);
          const uint8_t a8 = static_cast<uint8_t>(std::lround(a * 255));
          for (int x = 0; x < px->w; ++x) {
            dst[x * 4 + 0] = static_cast<uint8_t>(src[x * 4 + 0] * a);
            dst[x * 4 + 1] = static_cast<uint8_t>(src[x * 4 + 1] * a);
            dst[x * 4 + 2] = static_cast<uint8_t>(src[x * 4 + 2] * a);
            dst[x * 4 + 3] = a8;
          }
        }
        HGDIOBJ old = ::SelectObject(mem, bmp);
        const double gap = fx.refl_distance * sc, h = (y1 - y0) * rows / px->h;
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ::GdiAlphaBlend(dc, Tw(x0), Tw(y1 + gap), Tw(x1) - Tw(x0), std::max(1, Tw(y1 + gap + h) - Tw(y1 + gap)), mem, 0, 0, px->w, rows, bf);
        ::SelectObject(mem, old);
      }
      if (bmp) ::DeleteObject(bmp);
      if (mem) ::DeleteDC(mem);
    }
  }
  ::RestoreDC(dc, saved);
}

// ---- cells ----
struct FmtCache {
  std::unordered_map<uint32_t, std::shared_ptr<const formula::NumberFormat>> by_id;
  const formula::NumberFormat& Get(const model::StyleTable& t, uint32_t id) {
    auto& p = by_id[id];
    if (!p) p = formula::CompileFormat(id ? t.Str(id) : std::string("General"));
    return *p;
  }
};

void DrawSparklineGdi(Gdi& g, double x0, double y0, double x1, double y1, const model::SparklineGroup& sg, const std::vector<double>& v, double sc,
                      double lo_fix = NAN, double hi_fix = NAN, const std::vector<double>* xfrac = nullptr) {
  const double pad = 1.5 * sc;
  x0 += pad, y0 += pad, x1 -= pad, y1 -= pad;
  if (v.empty() || x1 - x0 < 3 || y1 - y0 < 2) return;
  double lo = 1e300, hi = -1e300;
  size_t ilo = 0, ihi = 0, ifirst = SIZE_MAX, ilast = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (std::isnan(v[i])) continue;
    if (v[i] < lo) lo = v[i], ilo = i;
    if (v[i] > hi) hi = v[i], ihi = i;
    if (ifirst == SIZE_MAX) ifirst = i;
    ilast = i;
  }
  if (ifirst == SIZE_MAX) return;
  const size_t n = v.size();
  if (!std::isnan(lo_fix)) lo = lo_fix;
  if (!std::isnan(hi_fix)) hi = hi_fix;
  const bool dated = xfrac && xfrac->size() == n;
  auto special = [&](size_t i, uint32_t* col) {
    if (sg.high && i == ihi) return *col = sg.high_color, true;
    if (sg.low && i == ilo) return *col = sg.low_color, true;
    if (sg.first && i == ifirst) return *col = sg.first_color, true;
    if (sg.last && i == ilast) return *col = sg.last_color, true;
    if (sg.negative && v[i] < 0) return *col = sg.negative_color, true;
    return false;
  };
  if (sg.type == 0) {
    const double span = hi > lo ? hi - lo : 1.0;
    auto px = [&](size_t i) {
      if (dated && !std::isnan((*xfrac)[i])) return x0 + (x1 - x0) * (*xfrac)[i];
      return n > 1 ? x0 + (x1 - x0) * static_cast<double>(i) / static_cast<double>(n - 1) : (x0 + x1) / 2;
    };
    auto py = [&](size_t i) { return hi > lo ? y1 - (std::clamp(v[i], lo, hi) - lo) / span * (y1 - y0) : (y0 + y1) / 2; };
    if (sg.axis && lo < 0 && hi > 0) g.HLine(x0, x1, y1 - (0 - lo) / span * (y1 - y0), 0.5, RGB(0, 0, 0));
    for (size_t i = 1; i < n; ++i) {
      if (std::isnan(v[i - 1]) || std::isnan(v[i])) continue;
      g.Line(px(i - 1), py(i - 1), px(i), py(i), std::max(0.5, sg.line_weight * sc), g.Ink(Cr(sg.color)));
    }
    const int r = Tw(1.5 * sc);
    for (size_t i = 0; i < n; ++i) {
      if (std::isnan(v[i])) continue;
      uint32_t col = sg.marker_color;
      if (!special(i, &col) && !sg.markers) continue;
      HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, g.Ink(Cr(col)));
      ::Ellipse(g.dc(), Tw(px(i)) - r, Tw(py(i)) - r, Tw(px(i)) + r, Tw(py(i)) + r);
      g.Restore(op, ob);
    }
    return;
  }
  const double slot = (x1 - x0) / static_cast<double>(n), bw = std::max(0.5, slot * 0.75);
  const double top = sg.type == 2 ? 1.0 : std::isnan(hi_fix) ? std::max(hi, 0.0) : hi, bottom = sg.type == 2 ? -1.0 : std::isnan(lo_fix) ? std::min(lo, 0.0) : lo;
  const double span = top > bottom ? top - bottom : 1.0;
  const double yz = y0 + top / span * (y1 - y0);
  for (size_t i = 0; i < n; ++i) {
    if (std::isnan(v[i])) continue;
    const double x = sg.type == 2 ? (v[i] > 0 ? 1.0 : v[i] < 0 ? -1.0 : 0.0) : v[i];
    if (x == 0 && sg.type == 2) continue;
    const double y = y0 + (top - x) / span * (y1 - y0);
    uint32_t col = v[i] < 0 && sg.type == 2 ? sg.negative_color : sg.color;
    special(i, &col);
    const double bx = x0 + slot * static_cast<double>(i) + (slot - bw) / 2;
    g.Fill(bx, std::min(y, yz), bx + bw, std::max(y, yz) + (y == yz ? 0.5 : 0.0), g.Ink(Cr(col)));
  }
  if (sg.axis) g.HLine(x0, x1, yz, 0.5, RGB(0, 0, 0));
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<PrintSheetJob> BuildPrintJobs(const engine::Snapshot& snap, const std::vector<uint32_t>& sheets, const Rect* selection, bool ignore_area) {
  std::vector<PrintSheetJob> jobs;
  for (uint32_t id : sheets) {
    const model::Sheet* sh = snap.Sheet(id);
    if (!sh) continue;
    PrintSheetJob j;
    j.sheet = id;
    j.ps = *sh->page_setup;
    if (ignore_area) j.ps.has_area = false;
    if (selection) {
      j.ps.has_area = true;
      j.ps.area = *selection;
    }
    if (sh->chart_sheet) {  // a chart sheet: one page, its chart fills the margins
      j.ps.has_area = true;
      j.ps.area = Rect{0, 0, 0, 0};
      j.ps.headings = j.ps.gridlines = false;
      j.ps.has_title_rows = j.ps.has_title_cols = false;
      if (sh->charts->empty()) continue;
    }
    j.layout = engine::LayoutPages(*sh, j.ps);
    if (j.layout.empty) continue;
    AddNotesPages(*sh, &j);
    jobs.push_back(std::move(j));
  }
  return jobs;
}

void AddNotesPages(const model::Sheet& sh, PrintSheetJob* j) {
  // Page Setup > Sheet > Comments: At end of sheet: pages listing every note ("Cell: A1" / "Comment: ...") after the cells
  if (j->ps.print_comments != 1 || j->layout.empty || sh.notes->empty()) return;
  const int n = static_cast<int>(sh.notes->size());
  const int per_page = std::max(1, static_cast<int>(j->layout.box_h / 54.0));
  for (int from = 0; from < n; from += per_page) {
    engine::PrintLayout::Page p;
    p.notes_from = from;
    p.notes_to = std::min(n, from + per_page);
    j->layout.pages.push_back(p);
  }
}

std::vector<PrintSheetJob> BuildChartJob(const engine::Snapshot& snap, uint32_t sheet, int chart) {
  std::vector<PrintSheetJob> jobs;
  const model::Sheet* sh = snap.Sheet(sheet);
  if (!sh || chart < 0 || chart >= static_cast<int>(sh->charts->size())) return jobs;
  // File > Print > Print Selected Chart: the chart alone on a page, filling the margins (as a chart sheet prints)
  PrintSheetJob j;
  j.sheet = sheet;
  j.chart = chart;
  j.ps = *sh->page_setup;
  j.ps.has_area = true;
  j.ps.area = Rect{0, 0, 0, 0};
  j.ps.more_areas.clear();
  j.ps.headings = j.ps.gridlines = false;
  j.ps.has_title_rows = j.ps.has_title_cols = false;
  j.ps.print_comments = 0;
  j.ps.landscape = true;  // Excel prints a chart on a landscape page
  j.layout = engine::LayoutPages(*sh, j.ps);
  if (!j.layout.empty) jobs.push_back(std::move(j));
  return jobs;
}

void PaintPage(HDC dc, double device_px_per_inch, const engine::Snapshot& snap, const PrintSheetJob& job, int page, const PrintInfo& info) {
  const model::Sheet* shp = snap.Sheet(job.sheet);
  if (!shp || page < 0 || page >= static_cast<int>(job.layout.pages.size())) return;
  const model::Sheet& sh = *shp;
  const model::StyleTable& styles = *snap.wb.styles;
  const engine::PrintLayout& L = job.layout;
  const model::PageSetup& ps = job.ps;
  const auto& P = L.pages[static_cast<size_t>(page)];
  const double sc = L.scale;
  Gdi g(dc, device_px_per_inch, ps.black_and_white);
  auto header_footer = [&] {
    // Scale with document: the text follows the page's zoom; Align with page margins: it spans the left and right margins
    // (else Excel's own 0.7")
    ::SelectObject(dc, g.Font(styles.default_font, 11 * (ps.no_scale_with_doc ? 1.0 : std::clamp(sc, 0.1, 4.0)), false, false));
    const double lh = g.LineHeight();
    const double xl = ps.no_align_margins ? 50.4 : ps.left, xr = ps.no_align_margins ? 50.4 : ps.right;
    const double hf_scale = ps.no_scale_with_doc ? 1.0 : std::clamp(sc, 0.1, 4.0);
    std::map<const void*, std::unique_ptr<Pixels>> hf_pics;
    auto hf = [&](int which, int align, bool header) {
      // Different first page / Different odd and even pages pick the page's own texts
      const std::string& code = model::HeaderFooterText(ps, which, page, info.page_number);
      if (code.empty()) return;
      // Format Text: runs of their own font; &G: the section's picture (Insert Picture)
      const auto runs = engine::HeaderFooterRuns(code, info.page_number, info.page_count, info.file, info.path, sh.name, info.date, info.time);
      struct Piece {
        std::wstring text;
        HFONT font = nullptr;
        COLORREF color = 0;
        const Pixels* pic = nullptr;
        double w = 0, h = 0;
      };
      std::vector<std::vector<Piece>> lines(1);
      for (const auto& r : runs) {
        if (r.picture) {
          const auto& hp = ps.hf_pictures[which];
          const Pixels* px = hp.image.empty() ? nullptr : PixelsOf(hp.image, hf_pics);
          if (!px) continue;
          Piece pc;
          pc.pic = px;
          pc.w = (hp.w > 0 ? hp.w : px->w * 0.75) * hf_scale;
          pc.h = (hp.h > 0 ? hp.h : px->h * 0.75) * hf_scale;
          lines.back().push_back(pc);
          continue;
        }
        HFONT f = g.Font(r.font.empty() ? styles.default_font : r.font, (r.size > 0 ? r.size : 11.0) * hf_scale, r.bold, r.italic, r.underline, r.strike);
        ::SelectObject(dc, f);
        const std::wstring wt = Wide(r.text);
        size_t at = 0;
        while (true) {
          const size_t nl = wt.find(L'\n', at);
          Piece pc;
          pc.text = wt.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
          pc.font = f;
          pc.color = r.color ? Cr(r.color & 0xFFFFFFu) : RGB(0, 0, 0);
          pc.w = g.Width(pc.text);
          pc.h = g.LineHeight();
          if (!pc.text.empty()) lines.back().push_back(pc);
          if (nl == std::wstring::npos) break;
          lines.emplace_back();
          at = nl + 1;
        }
      }
      double total_h = 0;
      std::vector<double> line_h(lines.size(), lh);
      for (size_t i = 0; i < lines.size(); ++i) {
        for (const Piece& pc : lines[i]) line_h[i] = std::max(line_h[i], pc.h);
        total_h += line_h[i];
      }
      double y = header ? ps.header : L.paper_h - ps.footer - total_h;
      const double x0 = xl, x1 = L.paper_w - xr;
      for (size_t i = 0; i < lines.size(); ++i) {
        double w = 0;
        for (const Piece& pc : lines[i]) w += pc.w;
        double x = align == 0 ? x0 : align == 1 ? (x0 + x1 - w) / 2 : x1 - w;
        for (const Piece& pc : lines[i]) {
          if (pc.pic) {
            DrawPixels(dc, *pc.pic, x, y + line_h[i] - pc.h, x + pc.w, y + line_h[i]);
          } else {
            ::SelectObject(dc, pc.font);
            g.Text(pc.text, x, y + line_h[i] - pc.h, x + pc.w + 2, y + line_h[i], 0, 2, false, pc.color);
          }
          x += pc.w;
        }
        y += line_h[i];
      }
    };
    for (int k = 0; k < 6; ++k) hf(k, k % 3, k < 3);
  };
  if (P.notes_from >= 0) {
    // Comments: At end of sheet: "Cell: A1" and the note's text, one after another
    ::SelectObject(dc, g.Font(styles.default_font, 10, false, false));
    const double line = g.LineHeight();
    double y = L.box_y;
    int k = 0;
    for (const auto& [key, note] : *sh.notes) {
      if (k >= P.notes_to) break;
      if (k++ < P.notes_from) continue;
      const std::string cell = core::CellA1(static_cast<uint32_t>(key >> 16), static_cast<uint32_t>(key & 0xFFFF), false, false);
      ::SelectObject(dc, g.Font(styles.default_font, 10, true, false));
      g.Text(Wide(std::string("Cell: ") + cell), L.box_x, y, L.box_x + L.box_w, y + line, 0, 0, false, RGB(0, 0, 0));
      ::SelectObject(dc, g.Font(styles.default_font, 10, false, false));
      const std::string text = (note.author.empty() ? std::string() : note.author + ":\n") + note.text;
      g.Text(Wide(std::string("Comment: ") + text), L.box_x, y + line, L.box_x + L.box_w, y + 54.0 - 4, 0, 0, true, RGB(0, 0, 0));
      y += 54.0;
    }
    header_footer();
    return;
  }
  if (sh.chart_sheet || job.chart >= 0) {
    if (job.chart >= 0 ? job.chart < static_cast<int>(sh.charts->size()) : !sh.charts->empty()) {
      const model::ChartObj& ch = (*sh.charts)[static_cast<size_t>(std::max(0, job.chart))];
      engine::ChartValues cv;
      ResolveChartValues(snap, sh.id, ch, &cv);
      DrawChart(g, ch, cv, L.box_x, L.box_y, L.box_x + L.box_w, L.box_y + L.box_h, 1.0);
    }
    header_footer();
    return;
  }

  // rows and columns on this page (print titles first)
  std::vector<Slot> rows, cols;
  double content_w = L.heading_w, content_h = L.heading_h;
  auto add_rows = [&](uint32_t a, uint32_t b) {
    for (uint32_t r = a; r <= b; ++r) {
      const double h = engine::RowPt(sh, r) * sc;
      if (h > 0) rows.push_back(Slot{r, content_h, h}), content_h += h;
    }
  };
  auto add_cols = [&](uint32_t a, uint32_t b) {
    for (uint32_t c = a; c <= b; ++c) {
      const double w = engine::ColPt(sh, c) * sc;
      if (w > 0) cols.push_back(Slot{c, content_w, w}), content_w += w;
    }
  };
  if (P.title_rows) add_rows(ps.title_r0, ps.title_r1);
  const size_t main_row0 = rows.size();
  add_rows(P.r0, P.r1);
  if (P.title_cols) add_cols(ps.title_c0, ps.title_c1);
  const size_t main_col0 = cols.size();
  add_cols(P.c0, P.c1);
  // where the page's content starts (centered on the page when asked)
  double ox = L.box_x, oy = L.box_y;
  if (ps.center_h) ox += std::max(0.0, (L.box_w - content_w) / 2);
  if (ps.center_v) oy += std::max(0.0, (L.box_h - content_h) / 2);
  for (Slot& s : rows) s.pos += oy;
  for (Slot& s : cols) s.pos += ox;
  const double gx0 = ox + L.heading_w, gy0 = oy + L.heading_h, gx1 = ox + content_w, gy1 = oy + content_h;

  // styles: own, table look, conditional formatting
  std::unique_ptr<engine::CondFormatEval> cf;
  if (!sh.cond_formats->empty()) cf = std::make_unique<engine::CondFormatEval>(snap.wb, sh.id);
  auto style_at = [&](uint32_t r, uint32_t c, engine::CfVisual* vis) {
    model::Style st = styles.Get(sh.GetStyle(r, c));
    for (const auto& t : *sh.tables) {
      if (t.range.Contains(r, c)) render::ApplyTableLook(t, r, c, &st, &*snap.wb.store);
    }
    if (vis) *vis = engine::CfVisual{};
    if (cf) {
      engine::CfVisual v;
      if (cf->Eval(r, c, &v)) {
        if (v.has_fill) st.fill_color = v.fill;
        if (v.has_font) st.font_color = v.font;
        if (v.bold >= 0) st.bold = v.bold != 0;
        if (v.italic >= 0) st.italic = v.italic != 0;
        if (v.strike >= 0) st.strike = v.strike != 0;
        if (v.border >> 24) {  // a rule's outline (Red Border) prints as thin borders
          for (int e : {model::kLeft, model::kTop, model::kRight, model::kBottom}) st.border[e] = {model::LineStyle::Thin, v.border};
        }
        // the rule's Format...: number format, underline, pattern, lines
        if (!v.numfmt.empty()) st.numfmt = v.numfmt == "General" ? 0 : styles.FindString(v.numfmt);
        if (v.underline >= 0) st.underline = static_cast<model::Underline>(v.underline);
        if (v.pattern >= 0) st.pattern = static_cast<model::FillPattern>(v.pattern), st.pattern_color = v.pattern_color;
        static const int kSide[4] = {model::kLeft, model::kTop, model::kRight, model::kBottom};
        for (int k = 0; k < 4; ++k) {
          if (v.side_style[k]) st.border[kSide[k]] = model::BorderSide{static_cast<model::LineStyle>(v.side_style[k]), v.side_color[k]};
        }
        if (vis) *vis = v;
      }
    }
    return st;
  };
  auto merge_at = [&](uint32_t r, uint32_t c) -> const Rect* { return sh.MergeAt(r, c); };
  FmtCache fmts;
  const double pad = 2.0 * sc;
  auto font_of = [&](const model::Style& st) {
    return g.Font(styles.FontName(st), styles.FontSize(st) * sc, st.bold, st.italic, st.underline != model::Underline::None, st.strike);
  };
  struct CellOut {
    std::wstring text;
    bool numeric = false;
    uint32_t color = 0;
  };
  auto cell_text = [&](const model::Style& st, const core::Value& v, double cell_w) {
    CellOut o;
    if (ps.print_errors && v.IsError()) {
      // Page Setup > Sheet > Cell errors as: <blank>, --, #N/A
      o.text = ps.print_errors == 1 ? L"" : ps.print_errors == 2 ? L"--" : L"#N/A";
      o.numeric = ps.print_errors != 1;
      return o;
    }
    const formula::NumberFormat& fmt = fmts.Get(styles, st.numfmt);
    const double digit = g.Width(L"0");
    const double max_chars = v.IsNumeric() && st.numfmt == 0 ? std::floor((cell_w - 2 * pad) / std::max(0.1, digit)) : -1;
    formula::FormatResult fr = formula::FormatValue(v, fmt, max_chars);
    o.text = Wide(fr.text);
    o.numeric = fr.numeric || v.IsNumeric();
    o.color = fr.color;
    if (o.numeric && max_chars < 0 && g.Width(o.text) > cell_w - 2 * pad) o.text.assign(static_cast<size_t>(std::max(1.0, (cell_w - 2 * pad) / std::max(0.1, g.Width(L"#")))), L'#');
    return o;
  };
  auto text_color = [&](const model::Style& st, const CellOut& o) -> COLORREF {
    if (o.color) return Cr(o.color);
    return (st.font_color >> 24) ? Cr(st.font_color) : RGB(0, 0, 0);
  };
  auto halign_of = [&](const model::Style& st, bool numeric) {
    switch (st.halign) {
      case model::HAlign::Right: return 2;
      case model::HAlign::Center:
      case model::HAlign::CenterAcross:
      case model::HAlign::Distributed:
      case model::HAlign::Justify: return 1;
      case model::HAlign::General: return (numeric != (st.reading_order == 2)) ? 2 : 0;  // Right-to-Left mirrors General
      default: return 0;
    }
  };
  auto valign_of = [&](const model::Style& st) { return st.valign == model::VAlign::Top ? 0 : st.valign == model::VAlign::Bottom ? 2 : 1; };
  const RECT page_clip{Tw(gx0), Tw(gy0), Tw(gx1), Tw(gy1)};
  // the fonts inside a cell's text (or a superscript / subscript cell); false = draw it as plain text
  auto rich_of = [&](uint32_t r, uint32_t c, const core::Value& v) -> const model::RichText* {
    if (sh.rich->empty() || !v.IsString()) return nullptr;
    auto it = sh.rich->find((uint64_t{r} << 16) | c);
    return it != sh.rich->end() && it->second.text == v.str() ? &it->second : nullptr;
  };
  auto draw_rich = [&](const model::Style& st, const model::RichText& rt, bool numeric, double x0, double y0, double x1, double y1, COLORREF def,
                       const RECT* clip) {
    const size_t nruns = std::max<size_t>(1, rt.runs.size());
    std::vector<model::RunFont> fonts(nruns);
    std::vector<HFONT> faces(nruns);
    for (size_t i = 0; i < nruns; ++i) {
      fonts[i] = model::ResolveRunFont(styles, st, rt.runs.empty() ? nullptr : &rt.runs[i]);
      faces[i] = g.Font(fonts[i].name, fonts[i].size * sc * (fonts[i].script ? 0.65 : 1.0), fonts[i].bold, fonts[i].italic,
                        fonts[i].underline != model::Underline::None, fonts[i].strike);
    }
    const int ha = halign_of(st, numeric), va = valign_of(st);
    const bool wrap = st.wrap && !numeric;
    const model::RichLayout lay = model::LayoutRich(rt, wrap, static_cast<float>(x1 - x0), ha, static_cast<float>(styles.FontSize(st) * sc * 1.2),
                                                    [&](const std::string& t, size_t i) {
                                                      ::SelectObject(dc, faces[i]);
                                                      return std::pair<float, float>(static_cast<float>(g.Width(Wide(t))),
                                                                                     static_cast<float>(fonts[i].size * sc * 1.2));
                                                    });
    const double bx = wrap ? x0 : ha == 2 ? x1 - lay.width : ha == 1 ? (x0 + x1 - lay.width) / 2 : x0;
    const double by = va == 0 ? y0 : va == 1 ? (y0 + y1 - lay.height) / 2 : y1 - lay.height;
    for (const auto& pc : lay.pieces) {
      const model::RunFont& f = fonts[pc.run];
      ::SelectObject(dc, faces[pc.run]);
      double y = by + pc.y;
      if (f.script == 2) y += pc.h * 0.4;
      g.Text(Wide(pc.text), bx + pc.x, y, bx + pc.x + pc.w + 1, y + pc.h, 0, 0, false, f.color ? Cr(f.color) : def, clip);
    }
  };

  // ---- fills, data bars, icons ----
  for (const Slot& rs : rows) {
    for (const Slot& cs : cols) {
      if (merge_at(rs.index, cs.index)) continue;
      engine::CfVisual vis;
      const model::Style st = style_at(rs.index, cs.index, &vis);
      if (st.HasFill() && !g.bw()) FillCell(g, cs.pos, rs.pos, cs.pos + cs.size, rs.pos + rs.size, st, sc);
      if (vis.bar >= 0 && !g.bw()) {
        const double m = 1.5 * sc;
        const double w = cs.size - 2 * m, axis = cs.pos + m + w * vis.bar_axis, len = w * vis.bar;
        const double x0 = vis.bar_negative ? axis - len : axis, x1 = vis.bar_negative ? axis : axis + len;
        g.Fill(x0, rs.pos + m, x1, rs.pos + rs.size - m, Cr(vis.bar_negative ? 0xFFFF0000u : vis.bar_color));
      }
      if (vis.icon >= 0) {
        const int n = model::IconSetCount(vis.icon_style);
        const COLORREF c = vis.icon == 0 ? RGB(0xE0, 0x3C, 0x31) : vis.icon == n - 1 ? RGB(0x2E, 0x9E, 0x44) : RGB(0xF4, 0xB4, 0x00);
        const double sz = std::min(rs.size - 3 * sc, 11.0 * sc), ix = vis.icon_only ? cs.pos + (cs.size - sz) / 2 : cs.pos + 1.5 * sc, iy = rs.pos + (rs.size - sz) / 2;
        HGDIOBJ op = g.UsePen(false, 0, 0), ob = g.UseBrush(true, g.Ink(c));
        ::Ellipse(dc, Tw(ix), Tw(iy), Tw(ix + sz), Tw(iy + sz));
        g.Restore(op, ob);
      }
    }
  }
  // ---- gridlines (Draft quality leaves them out) ----
  if (ps.gridlines && !ps.draft) {
    const COLORREF gc = Cr(job.gridline_rgb);
    g.HLine(gx0, gx1, gy0, 0.25, gc);
    g.VLine(gx0, gy0, gy1, 0.25, gc);
    for (const Slot& rs : rows) g.HLine(gx0, gx1, rs.pos + rs.size, 0.25, gc);
    for (const Slot& cs : cols) g.VLine(cs.pos + cs.size, gy0, gy1, 0.25, gc);
  }
  // ---- merged areas ----
  for (const Rect& m : *sh.merges) {
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    if (!SlotSpan(cols, m.c0, m.c1, &x0, &x1) || !SlotSpan(rows, m.r0, m.r1, &y0, &y1)) continue;
    const model::Style st = style_at(m.r0, m.c0, nullptr);
    if (st.HasFill() && !g.bw()) FillCell(g, x0, y0, x1, y1, st, sc);
    else g.Fill(x0, y0, x1, y1, RGB(255, 255, 255));
    if (ps.gridlines) {
      const COLORREF gc = Cr(job.gridline_rgb);
      g.HLine(x0, x1, y0, 0.25, gc);
      g.HLine(x0, x1, y1, 0.25, gc);
      g.VLine(x0, y0, y1, 0.25, gc);
      g.VLine(x1, y0, y1, 0.25, gc);
    }
    const core::Value& v = sh.GetValue(m.r0, m.c0);
    if (v.IsEmpty()) continue;
    ::SelectObject(dc, font_of(st));
    const CellOut o = cell_text(st, v, x1 - x0);
    const model::RichText* mrt = rich_of(m.r0, m.c0, v);
    if (!st.rotation && (mrt || st.script)) draw_rich(st, mrt ? *mrt : model::RichText{ToUtf8(o.text), {}}, o.numeric, x0 + pad, y0, x1 - pad, y1, text_color(st, o), &page_clip);
    else if (st.rotation) g.RotatedText(o.text, x0 + pad, y0, x1 - pad, y1, halign_of(st, o.numeric), valign_of(st), st.rotation, text_color(st, o), &page_clip);
    else g.Text(o.text, x0 + pad, y0, x1 - pad, y1, halign_of(st, o.numeric), valign_of(st), st.wrap && !o.numeric, text_color(st, o), &page_clip);
  }
  // ---- text (overflowing into empty cells on the page) ----
  auto empty_at = [&](uint32_t r, uint32_t c) { return sh.IsEmptyCell(r, c) && !merge_at(r, c); };
  for (const Slot& rs : rows) {
    for (size_t ci = 0; ci < cols.size(); ++ci) {
      const Slot& cs = cols[ci];
      const core::Value& v = sh.GetValue(rs.index, cs.index);
      if (v.IsEmpty() || merge_at(rs.index, cs.index)) continue;
      engine::CfVisual vis;
      const model::Style st = style_at(rs.index, cs.index, &vis);
      if (vis.icon >= 0 && vis.icon_only) continue;
      ::SelectObject(dc, font_of(st));
      const CellOut o = cell_text(st, v, cs.size);
      if (o.text.empty()) continue;
      double x0 = cs.pos, x1 = cs.pos + cs.size;
      if (vis.icon >= 0) x0 += std::min(rs.size - 3 * sc, 11.0 * sc) + 1.5 * sc;
      RECT clip{Tw(x0), Tw(rs.pos), Tw(x1), Tw(rs.pos + rs.size)};
      const int ha = halign_of(st, o.numeric);
      const bool wrap = st.wrap && !o.numeric;
      if (const model::RichText* rt = rich_of(rs.index, cs.index, v); !st.rotation && (rt || st.script)) {
        draw_rich(st, rt ? *rt : model::RichText{ToUtf8(o.text), {}}, o.numeric, x0 + pad, rs.pos, x1 - pad, rs.pos + rs.size - 0.5 * sc, text_color(st, o), &clip);
        continue;
      }
      if (st.rotation) {
        g.RotatedText(o.text, x0 + pad, rs.pos, x1 - pad, rs.pos + rs.size - 0.5 * sc, ha, valign_of(st), st.rotation, text_color(st, o), &clip);
        continue;
      }
      if (!o.numeric && !wrap) {
        const double w = g.Width(o.text) + 2 * pad;
        if (w > cs.size) {
          double left = x0, right = x1;
          if (ha == 0 || ha == 1) {
            const double need = ha == 0 ? cs.pos + w : cs.pos + cs.size / 2 + w / 2;
            for (size_t k = ci + 1; k < cols.size() && right < need && empty_at(rs.index, cols[k].index) && cols[k].pos <= right + 0.01; ++k) right = cols[k].pos + cols[k].size;
          }
          if (ha == 2 || ha == 1) {
            const double need = ha == 2 ? cs.pos + cs.size - w : cs.pos + cs.size / 2 - w / 2;
            for (size_t k = ci; k-- > 0 && left > need && empty_at(rs.index, cols[k].index);) left = cols[k].pos;
          }
          clip = RECT{Tw(left), Tw(rs.pos), Tw(right), Tw(rs.pos + rs.size)};
          if (ha == 0) x1 = std::max(x1, cs.pos + w);
          if (ha == 1) {
            x0 = cs.pos + cs.size / 2 - w / 2;
            x1 = cs.pos + cs.size / 2 + w / 2;
          }
          if (ha == 2) x0 = std::min(x0, cs.pos + cs.size - w);
        }
      }
      const double indent = st.indent * styles.FontSize(st) * sc * 0.75;
      g.Text(o.text, x0 + pad + (ha == 0 ? indent : 0), rs.pos, x1 - pad - (ha == 2 ? indent : 0), rs.pos + rs.size - 0.5 * sc, ha, valign_of(st), wrap,
             text_color(st, o), &clip);
    }
  }
  // ---- borders ----
  for (const Slot& rs : rows) {
    for (const Slot& cs : cols) {
      const model::Style st = style_at(rs.index, cs.index, nullptr);
      bool any = false;
      for (const auto& b : st.border) any = any || b.style != model::LineStyle::None;
      if (!any) continue;
      const Rect* m = merge_at(rs.index, cs.index);
      const bool top = !m || rs.index == m->r0, bottom = !m || rs.index == m->r1, left = !m || cs.index == m->c0, right = !m || cs.index == m->c1;
      auto col = [&](const model::BorderSide& b) { return b.color ? Cr(b.color) : RGB(0, 0, 0); };
      const auto& B = st.border;
      const double x0 = cs.pos, x1 = cs.pos + cs.size, y0 = rs.pos, y1 = rs.pos + rs.size;
      if (top && B[model::kTop].style != model::LineStyle::None) DrawBorder(g, B[model::kTop].style, col(B[model::kTop]), x0, y0, x1, y0, true, sc);
      if (bottom && B[model::kBottom].style != model::LineStyle::None) DrawBorder(g, B[model::kBottom].style, col(B[model::kBottom]), x0, y1, x1, y1, true, sc);
      if (left && B[model::kLeft].style != model::LineStyle::None) DrawBorder(g, B[model::kLeft].style, col(B[model::kLeft]), x0, y0, x0, y1, false, sc);
      if (right && B[model::kRight].style != model::LineStyle::None) DrawBorder(g, B[model::kRight].style, col(B[model::kRight]), x1, y0, x1, y1, false, sc);
      if (B[model::kDiagDown].style != model::LineStyle::None) g.Line(x0, y0, x1, y1, 0.5 * sc, g.Ink(col(B[model::kDiagDown])));
      if (B[model::kDiagUp].style != model::LineStyle::None) g.Line(x0, y1, x1, y0, 0.5 * sc, g.Ink(col(B[model::kDiagUp])));
    }
  }
  // ---- sparklines ----
  for (const auto& sg : *sh.sparklines) {
    for (const auto& it : sg.items) {
      const Slot* rs = FindSlot(rows, it.r);
      const Slot* cs = FindSlot(cols, it.c);
      const model::Sheet* src = snap.wb.SheetById(it.src_sheet);
      if (!rs || !cs || !src || it.src.Count() > 10000) continue;
      double lo = NAN, hi = NAN;
      engine::SparklineAxis(snap.wb, sg, &lo, &hi);
      std::vector<double> xfrac;
      const std::vector<double> vals = engine::SparklineValues(snap.wb, sg, it, &xfrac);
      DrawSparklineGdi(g, cs->pos, rs->pos, cs->pos + cs->size, rs->pos + rs->size, sg, vals, sc, lo, hi, &xfrac);
    }
  }
  // ---- headings ----
  if (ps.headings) {
    const COLORREF line = RGB(0x80, 0x80, 0x80), fill = RGB(0xF2, 0xF2, 0xF2);
    ::SelectObject(dc, g.Font(styles.default_font, 10 * sc, false, false));
    g.Fill(ox, oy, gx1, gy0, g.bw() ? RGB(255, 255, 255) : fill);
    g.Fill(ox, gy0, gx0, gy1, g.bw() ? RGB(255, 255, 255) : fill);
    for (const Slot& cs : cols) {
      g.Text(Wide(core::ColumnName(cs.index)), cs.pos, oy, cs.pos + cs.size, gy0, 1, 1, false, RGB(0, 0, 0));
      g.VLine(cs.pos + cs.size, oy, gy0, 0.25, line);
    }
    for (const Slot& rs : rows) {
      g.Text(std::to_wstring(rs.index + 1), ox, rs.pos, gx0, rs.pos + rs.size, 1, 1, false, RGB(0, 0, 0));
      g.HLine(ox, gx0, rs.pos + rs.size, 0.25, line);
    }
    g.HLine(ox, gx1, oy, 0.25, line);
    g.HLine(ox, gx1, gy0, 0.25, line);
    g.VLine(ox, oy, gy1, 0.25, line);
    g.VLine(gx0, oy, gy1, 0.25, line);
  }
  // ---- shapes and charts (on the page's own cells, not the print titles); Draft quality leaves them out ----
  if (main_row0 < rows.size() && main_col0 < cols.size() && !ps.draft) {
    const double mx0 = cols[main_col0].pos, my0 = rows[main_row0].pos;
    const double sx0 = static_cast<double>(sh.cols->Offset(P.c0)) * kPt, sy0 = static_cast<double>(sh.rows->Offset(P.r0)) * kPt;
    const double sx1 = static_cast<double>(sh.cols->Offset(static_cast<int64_t>(P.c1) + 1)) * kPt, sy1 = static_cast<double>(sh.rows->Offset(static_cast<int64_t>(P.r1) + 1)) * kPt;
    auto px = [&](double sxp) { return mx0 + (sxp - sx0) * sc; };
    auto py = [&](double syp) { return my0 + (syp - sy0) * sc; };
    const int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, Tw(mx0), Tw(my0), Tw(gx1), Tw(gy1));
    struct Obj {
      int z;
      const model::ShapeObj* shape;
      const model::ChartObj* chart;
      const model::Control* control = nullptr;
    };
    std::vector<Obj> objs;
    for (const auto& s : *sh.shapes) {
      if (s.visible && s.print_object && s.left < sx1 && s.left + s.width > sx0 && s.top < sy1 && s.top + s.height > sy0) objs.push_back(Obj{s.z, &s, nullptr});
    }
    for (const auto& c : *sh.charts) {
      if (!c.floating && c.print_object && c.left < sx1 && c.left + c.width > sx0 && c.top < sy1 && c.top + c.height > sy0) objs.push_back(Obj{c.z, nullptr, &c});
    }
    for (const auto& c : *sh.controls) {
      if (c.visible && c.print_object && c.placement != model::Placement::FloatWindow && c.left < sx1 && c.left + c.width > sx0 && c.top < sy1 && c.top + c.height > sy0)
        objs.push_back(Obj{c.z, nullptr, nullptr, &c});
    }
    std::stable_sort(objs.begin(), objs.end(), [](const Obj& a, const Obj& b) { return a.z < b.z; });
    std::map<const void*, std::unique_ptr<Pixels>> pics;
    for (const Obj& o : objs) {
      if (o.control) DrawControlGdi(g, *o.control, px(o.control->left), py(o.control->top), px(o.control->left + o.control->width), py(o.control->top + o.control->height), sc);
      else if (o.shape) DrawShapeWithEffects(g, *o.shape, px(o.shape->left), py(o.shape->top), px(o.shape->left + o.shape->width), py(o.shape->top + o.shape->height), sc, pics);
      else {
        engine::ChartValues cv;
        ResolveChartValues(snap, sh.id, *o.chart, &cv);
        DrawChart(g, *o.chart, cv, px(o.chart->left), py(o.chart->top), px(o.chart->left + o.chart->width), py(o.chart->top + o.chart->height), sc);
      }
    }
    ::RestoreDC(dc, saved);
  }
  // ---- notes shown on the sheet (Comments: As displayed on sheet): the note's box beside its cell ----
  if (ps.print_comments == 2) {
    for (const auto& [key, note] : *sh.notes) {
      if (!note.visible || note.threaded) continue;
      const Slot* rs = FindSlot(rows, static_cast<uint32_t>(key >> 16));
      const Slot* cs = FindSlot(cols, static_cast<uint32_t>(key & 0xFFFF));
      if (!rs || !cs) continue;
      const double x0 = cs->pos + cs->size + 8 * sc, y0 = rs->pos, x1 = x0 + 108 * sc, y1 = y0 + 59 * sc;
      HGDIOBJ op = g.UsePen(true, RGB(0, 0, 0), 0.5 * sc), ob = g.UseBrush(!g.bw(), RGB(0xFF, 0xFF, 0xE1));
      ::Rectangle(dc, Tw(x0), Tw(y0), Tw(x1), Tw(y1));
      g.Restore(op, ob);
      g.Line(cs->pos + cs->size, rs->pos + 2 * sc, x0, y0 + 4 * sc, 0.5 * sc, RGB(0, 0, 0));
      ::SelectObject(dc, g.Font("Tahoma", 9 * sc, false, false));
      const std::string text = (note.author.empty() ? std::string() : note.author + ":\n") + note.text;
      g.Text(Wide(text), x0 + 3 * sc, y0 + 2 * sc, x1 - 3 * sc, y1 - 2 * sc, 0, 0, true, RGB(0, 0, 0));
    }
  }
  // ---- header and footer ----
  header_footer();
}

void SetPageMapping(HDC dc, double device_px_per_inch_x, double device_px_per_inch_y, int offset_x_px, int offset_y_px) {
  ::SetMapMode(dc, MM_ANISOTROPIC);
  ::SetWindowExtEx(dc, 144000, 144000, nullptr);  // 100 inches in twips: fractional preview resolutions stay exact
  ::SetViewportExtEx(dc, static_cast<int>(std::lround(device_px_per_inch_x * 100)), static_cast<int>(std::lround(device_px_per_inch_y * 100)), nullptr);
  ::SetWindowOrgEx(dc, 0, 0, nullptr);
  ::SetViewportOrgEx(dc, -offset_x_px, -offset_y_px, nullptr);
}

PrintInfo MakePrintInfo(const std::wstring& file_path) {
  PrintInfo info;
  const std::filesystem::path p(file_path);
  info.file = file_path.empty() ? std::string("Book1") : ToUtf8(p.filename().wstring());
  info.path = file_path.empty() ? std::string() : ToUtf8(p.parent_path().wstring()) + "\\";
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  char b[64];
  std::snprintf(b, sizeof(b), "%d/%d/%d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  info.date = b;
  std::snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
  info.time = b;
  return info;
}

bool PageCellMapOf(const engine::Snapshot& snap, const PrintSheetJob& job, int page, PageCellMap* out) {
  const model::Sheet* sh = snap.Sheet(job.sheet);
  if (!sh || page < 0 || page >= static_cast<int>(job.layout.pages.size())) return false;
  const engine::PrintLayout& L = job.layout;
  const model::PageSetup& ps = job.ps;
  const auto& P = L.pages[static_cast<size_t>(page)];
  out->rows.clear();
  out->cols.clear();
  if (P.notes_from >= 0 || job.chart >= 0) return false;  // a page of notes or a chart alone has no cells
  double w = L.heading_w, h = L.heading_h;
  auto add_rows = [&](uint32_t a, uint32_t b) {
    for (uint32_t r = a; r <= b; ++r) {
      const double x = engine::RowPt(*sh, r) * L.scale;
      if (x > 0) out->rows.push_back(PageCellMap::Slot{r, h, x}), h += x;
    }
  };
  auto add_cols = [&](uint32_t a, uint32_t b) {
    for (uint32_t c = a; c <= b; ++c) {
      const double x = engine::ColPt(*sh, c) * L.scale;
      if (x > 0) out->cols.push_back(PageCellMap::Slot{c, w, x}), w += x;
    }
  };
  // the same order as PaintPage
  if (P.title_rows) add_rows(ps.title_r0, ps.title_r1);
  add_rows(P.r0, P.r1);
  if (P.title_cols) add_cols(ps.title_c0, ps.title_c1);
  add_cols(P.c0, P.c1);
  double ox = L.box_x, oy = L.box_y;
  if (ps.center_h) ox += std::max(0.0, (L.box_w - w) / 2);
  if (ps.center_v) oy += std::max(0.0, (L.box_h - h) / 2);
  for (auto& s : out->rows) s.pos += oy;
  for (auto& s : out->cols) s.pos += ox;
  return true;
}

bool PageCellMap::CellAt(double x, double y, uint32_t* r, uint32_t* c) const {
  bool fr = false, fc = false;
  for (const auto& s : rows) {
    if (y >= s.pos && y < s.pos + s.size) *r = s.index, fr = true;
  }
  for (const auto& s : cols) {
    if (x >= s.pos && x < s.pos + s.size) *c = s.index, fc = true;
  }
  return fr && fc;
}

bool PageCellMap::CellRect(uint32_t r, uint32_t c, double* x0, double* y0, double* x1, double* y1) const {
  bool fr = false, fc = false;
  for (const auto& s : rows) {
    if (s.index == r && !fr) *y0 = s.pos, *y1 = s.pos + s.size, fr = true;
  }
  for (const auto& s : cols) {
    if (s.index == c && !fc) *x0 = s.pos, *x1 = s.pos + s.size, fc = true;
  }
  return fr && fc;
}

bool RenderPagePreview(const engine::Snapshot& snap, const PrintSheetJob& job, int page, const PrintInfo& info, int width_px, std::vector<uint8_t>* rgba, int* w,
                       int* h) {
  const double dpi = width_px / (job.layout.paper_w / 72.0);
  *w = width_px;
  *h = std::max(1, static_cast<int>(std::lround(job.layout.paper_h / 72.0 * dpi)));
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = *w;
  bi.bmiHeader.biHeight = -*h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC mem = ::CreateCompatibleDC(nullptr);
  HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    ::DeleteDC(mem);
    return false;
  }
  HGDIOBJ old = ::SelectObject(mem, bmp);
  ::PatBlt(mem, 0, 0, *w, *h, WHITENESS);
  SetPageMapping(mem, dpi, dpi, 0, 0);
  PaintPage(mem, dpi, snap, job, page, info);
  ::GdiFlush();
  const uint8_t* src = static_cast<const uint8_t*>(bits);
  rgba->resize(static_cast<size_t>(*w) * static_cast<size_t>(*h) * 4);
  for (size_t i = 0; i < rgba->size(); i += 4) {
    (*rgba)[i + 0] = src[i + 2];
    (*rgba)[i + 1] = src[i + 1];
    (*rgba)[i + 2] = src[i + 0];
    (*rgba)[i + 3] = 255;
  }
  ::SelectObject(mem, old);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  return true;
}

// ---- Screenshot ----
namespace {
// a print job of one page that is exactly the area: no margins, no header / footer, 100%
PrintSheetJob ImageJob(const model::Sheet& sh, const Rect& area, const ImageOptions& o, double* content_w, double* content_h) {
  PrintSheetJob job;
  job.sheet = sh.id;
  job.gridline_rgb = 0xD4D4D4;
  model::PageSetup& ps = job.ps;
  ps.left = ps.right = ps.top = ps.bottom = ps.header = ps.footer = 0;
  ps.gridlines = o.gridlines;
  ps.headings = o.headings;
  engine::PrintLayout& L = job.layout;
  L.scale = 1;
  L.empty = false;
  L.area = area;
  if (sh.chart_sheet) {
    L.box_w = L.paper_w = *content_w = 640;
    L.box_h = L.paper_h = *content_h = 400;
    L.pages.push_back({});
    return job;
  }
  L.heading_h = o.headings ? 15.0 : 0.0;
  L.heading_w = o.headings ? std::max(3.0, std::floor(std::log10(static_cast<double>(area.r1) + 1)) + 1) * 6.0 + 8.0 : 0.0;
  double w = L.heading_w, hgt = L.heading_h;
  for (uint32_t c = area.c0; c <= area.c1; ++c) w += engine::ColPt(sh, c);
  for (uint32_t r = area.r0; r <= area.r1; ++r) hgt += engine::RowPt(sh, r);
  L.box_w = L.paper_w = *content_w = std::max(1.0, w);
  L.box_h = L.paper_h = *content_h = std::max(1.0, hgt);
  engine::PrintLayout::Page pg;
  pg.r0 = area.r0, pg.r1 = area.r1, pg.c0 = area.c0, pg.c1 = area.c1;
  L.pages.push_back(pg);
  return job;
}
}  // namespace

bool RenderImage(const engine::Snapshot& snap, const std::vector<ImagePart>& parts, const ImageOptions& o, std::vector<uint8_t>* rgba, int* w, int* h,
                 bool* scaled_down) {
  constexpr double kCaption = 26, kGap = 14;  // points
  struct Placed {
    PrintSheetJob job;
    std::string caption;
    double y = 0, cw = 0, ch = 0;
  };
  std::vector<Placed> placed;
  double total_w = 0, total_h = 0;
  for (const ImagePart& p : parts) {
    const model::Sheet* sh = snap.Sheet(p.sheet);
    if (!sh) continue;
    Placed x;
    if (!placed.empty()) total_h += kGap;
    if (!p.caption.empty()) total_h += kCaption;
    x.job = ImageJob(*sh, p.area, o, &x.cw, &x.ch);
    x.caption = p.caption;
    x.y = total_h;
    total_h += x.ch;
    total_w = std::max(total_w, x.cw);
    placed.push_back(std::move(x));
  }
  if (placed.empty()) return false;
  // the resolution: as asked, unless the picture would be too large
  double ppt = o.px_per_pt;
  ppt = std::min(ppt, o.max_side / std::max(total_w, total_h));
  ppt = std::min(ppt, std::sqrt(o.max_pixels / (total_w * total_h)));
  if (scaled_down) *scaled_down = ppt < o.px_per_pt * 0.999;
  *w = std::max(1, static_cast<int>(std::ceil(total_w * ppt)));
  *h = std::max(1, static_cast<int>(std::ceil(total_h * ppt)));
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = *w;
  bi.bmiHeader.biHeight = -*h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC mem = ::CreateCompatibleDC(nullptr);
  HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp || !bits) {
    if (bmp) ::DeleteObject(bmp);
    ::DeleteDC(mem);
    return false;
  }
  HGDIOBJ old = ::SelectObject(mem, bmp);
  ::PatBlt(mem, 0, 0, *w, *h, WHITENESS);
  const double dpi = ppt * 72.0;
  const PrintInfo info = MakePrintInfo(L"");
  const model::StyleTable& styles = *snap.wb.styles;
  for (const Placed& x : placed) {
    if (!x.caption.empty()) {  // the sheet's name over its part
      SetPageMapping(mem, dpi, dpi, 0, -static_cast<int>(std::lround((x.y - kCaption) * ppt)));
      Gdi g(mem, dpi, false);
      g.Fill(0, 0, total_w, kCaption - 4, RGB(0xF3, 0xF3, 0xF3));
      ::SelectObject(mem, g.Font(styles.default_font, 12, true, false));
      g.Text(Wide(x.caption), 6, 0, total_w - 6, kCaption - 4, 0, 1, false, RGB(0x20, 0x20, 0x20));
      ::SelectObject(mem, ::GetStockObject(SYSTEM_FONT));  // before g deletes its font
    }
    SetPageMapping(mem, dpi, dpi, 0, -static_cast<int>(std::lround(x.y * ppt)));
    PaintPage(mem, dpi, snap, x.job, 0, info);
  }
  ::GdiFlush();
  const uint8_t* src = static_cast<const uint8_t*>(bits);
  rgba->resize(static_cast<size_t>(*w) * static_cast<size_t>(*h) * 4);
  for (size_t i = 0; i < rgba->size(); i += 4) {
    (*rgba)[i + 0] = src[i + 2];
    (*rgba)[i + 1] = src[i + 1];
    (*rgba)[i + 2] = src[i + 0];
    (*rgba)[i + 3] = 255;
  }
  ::SelectObject(mem, old);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  return true;
}

std::vector<std::wstring> PrinterNames(std::wstring* default_printer) {
  std::vector<std::wstring> out;
  DWORD need = 0, count = 0;
  ::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &need, &count);
  if (need) {
    std::vector<BYTE> buf(need);
    if (::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), need, &need, &count)) {
      const auto* info = reinterpret_cast<const PRINTER_INFO_4W*>(buf.data());
      for (DWORD i = 0; i < count; ++i) out.emplace_back(info[i].pPrinterName);
    }
  }
  if (default_printer) {
    wchar_t name[512];
    DWORD n = 512;
    *default_printer = ::GetDefaultPrinterW(name, &n) ? std::wstring(name) : (out.empty() ? std::wstring() : out.front());
  }
  return out;
}

bool PrintJobs(const engine::Snapshot& snap, const std::vector<PrintSheetJob>& jobs, const PrintRequest& rq, const PrintInfo& base_info, std::string* err) {
  if (jobs.empty()) {
    *err = "There's nothing to print.";
    return false;
  }
  HANDLE hp = nullptr;
  if (!::OpenPrinterW(const_cast<wchar_t*>(rq.printer.c_str()), &hp, nullptr)) {
    *err = "The printer can't be opened: " + ToUtf8(rq.printer);
    return false;
  }
  const LONG dm_size = ::DocumentPropertiesW(nullptr, hp, const_cast<wchar_t*>(rq.printer.c_str()), nullptr, nullptr, 0);
  std::vector<BYTE> dm_buf(static_cast<size_t>(std::max<LONG>(dm_size, sizeof(DEVMODEW))));
  auto* dm = reinterpret_cast<DEVMODEW*>(dm_buf.data());
  // Printer Properties: the settings chosen in the driver's own dialog are the starting point
  DEVMODEW* in = rq.devmode.size() >= sizeof(DEVMODEW) ? reinterpret_cast<DEVMODEW*>(const_cast<BYTE*>(rq.devmode.data())) : nullptr;
  const bool have_dm = dm_size > 0 && ::DocumentPropertiesW(nullptr, hp, const_cast<wchar_t*>(rq.printer.c_str()), dm, in,
                                                             DM_OUT_BUFFER | (in ? DM_IN_BUFFER : 0)) == IDOK;
  ::ClosePrinter(hp);
  if (have_dm && rq.duplex > 0) {  // Print on Both Sides: flip pages on the long edge / the short edge
    dm->dmFields |= DM_DUPLEX;
    dm->dmDuplex = static_cast<short>(rq.duplex == 1 ? DMDUP_VERTICAL : DMDUP_HORIZONTAL);
  } else if (have_dm && (dm->dmFields & DM_DUPLEX)) {
    dm->dmDuplex = DMDUP_SIMPLEX;
  }
  auto apply = [&](const model::PageSetup& ps) {
    if (!have_dm) return;
    dm->dmFields |= DM_ORIENTATION | DM_PAPERSIZE;
    dm->dmOrientation = static_cast<short>(ps.landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT);
    dm->dmPaperSize = static_cast<short>(ps.paper);
    dm->dmFields &= ~static_cast<DWORD>(DM_PAPERLENGTH | DM_PAPERWIDTH);
  };
  apply(jobs.front().ps);
  HDC dc = ::CreateDCW(L"WINSPOOL", rq.printer.c_str(), nullptr, have_dm ? dm : nullptr);
  if (!dc) {
    *err = "The printer can't be used: " + ToUtf8(rq.printer);
    return false;
  }
  const std::wstring doc_name = FromUtf8(base_info.file);
  DOCINFOW di{};
  di.cbSize = sizeof(di);
  di.lpszDocName = doc_name.c_str();
  if (!rq.output.empty()) di.lpszOutput = rq.output.c_str();
  if (::StartDocW(dc, &di) <= 0) {
    ::DeleteDC(dc);
    *err = "Printing couldn't start.";
    return false;
  }
  // pages of the whole job, numbered per sheet
  struct Item {
    size_t job;
    int page;
  };
  std::vector<Item> items;
  int number = 0;
  for (size_t j = 0; j < jobs.size(); ++j) {
    for (int p = 0; p < static_cast<int>(jobs[j].layout.pages.size()); ++p) {
      ++number;
      if (number < std::max(1, rq.from) || (rq.to > 0 && number > rq.to)) continue;
      items.push_back(Item{j, p});
    }
  }
  bool ok = !items.empty();
  if (!ok) *err = "There's nothing to print in those pages.";
  const int copies = std::clamp(rq.copies, 1, 999);
  size_t current_job = SIZE_MAX;
  for (int copy = 0; ok && copy < (rq.collate ? copies : 1); ++copy) {
    for (const Item& it : items) {
      for (int k = 0; ok && k < (rq.collate ? 1 : copies); ++k) {
        const PrintSheetJob& job = jobs[it.job];
        if (it.job != current_job && have_dm) {
          apply(job.ps);
          ::ResetDCW(dc, dm);
        }
        current_job = it.job;
        if (::StartPage(dc) <= 0) {
          ok = false;
          *err = "Printing failed.";
          break;
        }
        const double dpx = ::GetDeviceCaps(dc, LOGPIXELSX), dpy = ::GetDeviceCaps(dc, LOGPIXELSY);
        SetPageMapping(dc, dpx, dpy, ::GetDeviceCaps(dc, PHYSICALOFFSETX), ::GetDeviceCaps(dc, PHYSICALOFFSETY));
        PrintInfo info = base_info;
        info.page_number = (job.ps.first_page_number > 0 ? job.ps.first_page_number : 1) + it.page;
        info.page_count = static_cast<int>(job.layout.pages.size());
        PaintPage(dc, std::min(dpx, dpy), snap, job, it.page, info);
        if (::EndPage(dc) <= 0) {
          ok = false;
          *err = "Printing failed.";
        }
      }
    }
  }
  if (ok) ::EndDoc(dc);
  else ::AbortDoc(dc);
  ::DeleteDC(dc);
  return ok;
}

}  // namespace yc::app
