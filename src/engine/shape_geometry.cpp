// SPDX-License-Identifier: Apache-2.0
// shape_geometry.cpp - the outlines of Excel's preset shapes (see shape_geometry.h)
#include "engine/shape_geometry.h"

#include "core/i18n.h"
#include "model/objects.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>

namespace yc::engine {

namespace {

constexpr double kPi = 3.14159265358979323846;
using Pts = std::vector<ShapePt>;

// points of an ellipse arc around (cx, cy) from a0 to a1 degrees (clockwise from 3 o'clock, y down), ends included
void Arc(Pts& v, double cx, double cy, double rx, double ry, double a0, double a1, int n = 0) {
  if (n <= 0) n = std::max(4, static_cast<int>(std::abs(a1 - a0) / 6));
  for (int i = 0; i <= n; ++i) {
    const double a = (a0 + (a1 - a0) * i / n) * kPi / 180;
    v.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
}

Pts Ellipse(double cx, double cy, double rx, double ry) {
  Pts v;
  Arc(v, cx, cy, rx, ry, 0, 360, 72);
  v.pop_back();
  return v;
}

Pts Rect(double x0, double y0, double x1, double y1) { return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; }

// stretches the points to fill the w x h box
void Fit(Pts& v, double w, double h) {
  if (v.empty()) return;
  double x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
  for (const auto& p : v) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
  const double sx = x1 > x0 ? w / (x1 - x0) : 1, sy = y1 > y0 ? h / (y1 - y0) : 1;
  for (auto& p : v) p = {(p.x - x0) * sx, (p.y - y0) * sy};
}

// a polygon given in Office's 21600 x 21600 square
Pts Vml(std::initializer_list<int> xy, double w, double h) {
  Pts v;
  for (auto it = xy.begin(); it != xy.end() && it + 1 != xy.end(); it += 2) v.push_back({*it * w / 21600.0, *(it + 1) * h / 21600.0});
  return v;
}

// a regular polygon (inner = 0) or a star (inner: the inner points' radius as a fraction), first point up, filling the box
Pts Star(int n, double inner, double w, double h) {
  Pts v;
  for (int i = 0; i < n * (inner > 0 ? 2 : 1); ++i) {
    const double a = -kPi / 2 + i * kPi / (inner > 0 ? n : n / 2.0);
    const double r = inner > 0 && (i % 2) ? inner : 1.0;
    v.push_back({r * std::cos(a), r * std::sin(a)});
  }
  Fit(v, w, h);
  return v;
}

// a rectangle whose corners (top-left, top-right, bottom-right, bottom-left) are square (0), rounded (1) or snipped (2)
Pts Corners(double w, double h, int tl, int tr, int br, int bl, double r) {
  Pts v;
  auto corner = [&](int k, double cx, double cy, double sx, double sy, double a0) {
    // (cx, cy) the box corner; (sx, sy) the directions into the box; a0 the arc's start angle
    if (k == 0) {
      v.push_back({cx, cy});
    } else if (k == 1) {
      Arc(v, cx + sx * r, cy + sy * r, r, r, a0, a0 + 90, 12);
    } else {
      // the cut runs from the previous side to the next one
      if (a0 == 180) v.push_back({cx, cy + sy * r}), v.push_back({cx + sx * r, cy});
      else if (a0 == 270) v.push_back({cx + sx * r, cy}), v.push_back({cx, cy + sy * r});
      else if (a0 == 0) v.push_back({cx, cy + sy * r}), v.push_back({cx + sx * r, cy});
      else v.push_back({cx + sx * r, cy}), v.push_back({cx, cy + sy * r});
    }
  };
  corner(tl, 0, 0, 1, 1, 180);
  corner(tr, w, 0, -1, 1, 270);
  corner(br, w, h, -1, -1, 0);
  corner(bl, 0, h, 1, -1, 90);
  return v;
}

// an arrow pointing along +u in a box (len along u, thick across v): shaft is the shaft's thickness (fraction of
// thick), head the head's length; tail_notch cuts a V into the tail; heads 2: one at both ends
Pts ArrowUV(double len, double thick, double shaft, double head, double tail_notch = 0, int heads = 1) {
  const double s0 = thick * (1 - shaft) / 2, s1 = thick - s0, c = thick / 2;
  Pts v;
  if (heads == 2) {
    v = {{0, c}, {head, 0}, {head, s0}, {len - head, s0}, {len - head, 0}, {len, c}, {len - head, thick}, {len - head, s1}, {head, s1}, {head, thick}};
  } else {
    v = {{0, s0}, {len - head, s0}, {len - head, 0}, {len, c}, {len - head, thick}, {len - head, s1}, {0, s1}};
    if (tail_notch > 0) v.push_back({tail_notch, c});
  }
  return v;
}

// maps points built along +u (u, v) to the box: dir 0 right, 1 left, 2 up, 3 down
Pts Orient(const Pts& uv, int dir, double w, double h) {
  Pts v;
  for (const auto& p : uv) {
    switch (dir) {
      case 0: v.push_back({p.x, p.y}); break;
      case 1: v.push_back({w - p.x, p.y}); break;
      case 2: v.push_back({p.y, h - p.x}); break;
      default: v.push_back({p.y, p.x}); break;
    }
  }
  return v;
}

// a thick band along an ellipse arc (a0 -> a1 degrees) ending in an arrow head
Pts CurvedArrow(double cx, double cy, double rx, double ry, double a0, double a1, double thick, double head_len, double head_w) {
  Pts outer, inner;
  const int n = 32;
  for (int i = 0; i <= n; ++i) {
    const double a = (a0 + (a1 - a0) * i / n) * kPi / 180, ca = std::cos(a), sa = std::sin(a);
    outer.push_back({cx + (rx + thick / 2) * ca, cy + (ry + thick / 2) * sa});
    inner.push_back({cx + (rx - thick / 2) * ca, cy + (ry - thick / 2) * sa});
  }
  const double a = a1 * kPi / 180, ca = std::cos(a), sa = std::sin(a);
  const ShapePt mid{cx + rx * ca, cy + ry * sa};
  // the direction of travel at the end, and across it (outward)
  double tx = -rx * sa, ty = ry * ca;
  if (a1 < a0) tx = -tx, ty = -ty;
  const double tl = std::hypot(tx, ty);
  tx /= tl > 0 ? tl : 1, ty /= tl > 0 ? tl : 1;
  double nx = ca, ny = sa;
  const double nl = std::hypot(nx, ny);
  nx /= nl, ny /= nl;
  Pts v = outer;
  v.push_back({mid.x + nx * head_w, mid.y + ny * head_w});
  v.push_back({mid.x + tx * head_len, mid.y + ty * head_len});
  v.push_back({mid.x - nx * head_w, mid.y - ny * head_w});
  v.insert(v.end(), inner.rbegin(), inner.rend());
  return v;
}

Pts Transpose(Pts v) {
  for (auto& p : v) std::swap(p.x, p.y);
  return v;
}

Pts MirrorX(Pts v, double w) {
  for (auto& p : v) p.x = w - p.x;
  return v;
}

Pts MirrorY(Pts v, double h) {
  for (auto& p : v) p.y = h - p.y;
  return v;
}

struct Builder {
  ShapeGeom* g;
  void Shape(const Pts& p, int shade = 0) {
    g->fills.push_back({p, shade});
    g->strokes.push_back({p, true});
  }
  void Fill(const Pts& p, int shade = 0) { g->fills.push_back({p, shade}); }
  void Line(const Pts& p, bool closed = false) { g->strokes.push_back({p, closed}); }
  // a ring between two ellipses, cut into quads (fills can't have holes)
  void Ring(double cx, double cy, double rx, double ry, double t, double a0 = 0, double a1 = 360) {
    const int n = 48;
    Pts o, in;
    Arc(o, cx, cy, rx, ry, a0, a1, n);
    Arc(in, cx, cy, rx - t, ry - t, a0, a1, n);
    for (int i = 0; i < n; ++i) Fill({o[i], o[i + 1], in[i + 1], in[i]});
    if (a1 - a0 >= 360) {
      o.pop_back(), in.pop_back();
      Line(o, true), Line(in, true);
    } else {
      Pts v = o;
      v.insert(v.end(), in.rbegin(), in.rend());
      Line(v, true);
    }
  }
};

// the cloud: bumps around an ellipse
Pts Cloud(double x0, double y0, double w, double h) {
  Pts v;
  const int n = 180;
  for (int i = 0; i < n; ++i) {
    const double a = 2 * kPi * i / n;
    const double r = 0.86 + 0.14 * std::abs(std::sin(5.5 * a));
    v.push_back({x0 + w / 2 + w / 2 * r * std::cos(a), y0 + h / 2 + h / 2 * r * std::sin(a)});
  }
  return v;
}

// a wavy side: y = base + amp * sin(2 pi periods x / w), from x0 to x1
void Wave(Pts& v, double x0, double x1, double w, double base, double amp, double periods) {
  const int n = 40;
  for (int i = 0; i <= n; ++i) {
    const double x = x0 + (x1 - x0) * i / n;
    v.push_back({x, base + amp * std::sin(2 * kPi * periods * x / w)});
  }
}

// the box and leader of a line callout: border (box outline), accent (a bar left of the box), bends 0..2
void LineCallout(Builder& b, double w, double h, bool border, bool accent, int bends, double ex, double ey) {
  const Pts box = Rect(0, 0, w, h);
  b.Fill(box);
  if (border) b.Line(box, true);
  Pts lead = {{-0.0833 * w, 0.1875 * h}};
  if (bends >= 1) lead.push_back({-0.1667 * w, 0.1875 * h});
  if (bends >= 2) lead.push_back({-0.1667 * w, ey * h});
  lead.push_back({ex * w, ey * h});
  if (!accent) lead.front().x = 0;
  b.Line(lead);
  if (accent) b.Line({{-0.0833 * w, 0}, {-0.0833 * w, h}});
}

// a can (cylinder): the top ellipse lighter, ry its half height
void Can(Builder& b, double w, double h, double ry) {
  Pts body = {{0, ry}};
  Arc(body, w / 2, h - ry, w / 2, ry, 180, 0, 36);
  body.push_back({w, ry});
  Arc(body, w / 2, ry, w / 2, ry, 0, 180, 36);
  b.Fill(body);
  b.Fill(Ellipse(w / 2, ry, w / 2, ry), 1);
  Pts side = {{w, ry}};
  Arc(side, w / 2, h - ry, w / 2, ry, 0, 180, 36);
  side.push_back({0, ry});
  b.Line(side);
  b.Line(Ellipse(w / 2, ry, w / 2, ry), true);
}

}  // namespace

bool AutoShapeGeometry(int type, double w, double h, ShapeGeom* g, const std::vector<double>* adj) {
  g->fills.clear();
  g->strokes.clear();
  Builder b{g};
  const double m = std::min(w, h), cx = w / 2, cy = h / 2;
  // the shape's adjustments (Shape.Adjustments, its yellow handles), else Excel's defaults
  const std::vector<double> defs = AutoShapeAdjustDefaults(type);
  auto A = [&](size_t i) { return adj && i < adj->size() ? (*adj)[i] : i < defs.size() ? defs[i] : 0.0; };
  switch (type) {
    // ---- rectangles ----
    case 1: case 61: b.Shape(Rect(0, 0, w, h)); break;
    case 5: b.Shape(Corners(w, h, 1, 1, 1, 1, A(0) * m)); break;
    case 62: b.Shape(Corners(w, h, 1, 1, 1, 1, 0.1667 * m)); break;
    case 155: b.Shape(Corners(w, h, 0, 2, 0, 0, A(0) * m)); break;
    case 156: b.Shape(Corners(w, h, 2, 2, 0, 0, A(0) * m)); break;
    case 157: b.Shape(Corners(w, h, 2, 0, 2, 0, A(0) * m)); break;
    case 154: b.Shape(Corners(w, h, 1, 2, 0, 0, A(0) * m)); break;
    case 151: b.Shape(Corners(w, h, 0, 1, 0, 0, A(0) * m)); break;
    case 152: b.Shape(Corners(w, h, 1, 1, 0, 0, A(0) * m)); break;
    case 153: b.Shape(Corners(w, h, 1, 0, 1, 0, A(0) * m)); break;
    // ---- basic shapes ----
    case 9: case 73: b.Shape(Ellipse(cx, cy, cx, cy)); break;
    case 7: b.Shape({{A(0) * w, 0}, {w, h}, {0, h}}); break;
    case 8: b.Shape({{0, 0}, {w, h}, {0, h}}); break;
    case 2: b.Shape({{A(0) * m, 0}, {w, 0}, {w - A(0) * m, h}, {0, h}}); break;
    case 3: b.Shape({{A(0) * m, 0}, {w - A(0) * m, 0}, {w, h}, {0, h}}); break;
    case 4: case 63: b.Shape({{cx, 0}, {w, cy}, {cx, h}, {0, cy}}); break;
    case 12: b.Shape(Star(5, 0, w, h)); break;
    case 10: b.Shape({{A(0) * m, 0}, {w - A(0) * m, 0}, {w, cy}, {w - A(0) * m, h}, {A(0) * m, h}, {0, cy}}); break;
    case 145: b.Shape(Star(7, 0, w, h)); break;
    case 6: {
      const double a = A(0) * m;
      b.Shape({{a, 0}, {w - a, 0}, {w, a}, {w, h - a}, {w - a, h}, {a, h}, {0, h - a}, {0, a}});
      break;
    }
    case 144: b.Shape(Star(10, 0, w, h)); break;
    case 146: b.Shape(Star(12, 0, w, h)); break;
    case 142: {  // Partial Circle: from angle 1 to angle 2 (degrees clockwise from 3 o'clock)
      Pts v = {{cx, cy}};
      const double a0 = A(0), a1 = A(1) <= a0 ? A(1) + 360 : A(1);
      Arc(v, cx, cy, cx, cy, a0, a1);
      b.Shape(v);
      break;
    }
    case 161: {  // Chord
      Pts v;
      const double a0 = A(0), a1 = A(1) <= a0 ? A(1) + 360 : A(1);
      Arc(v, cx, cy, cx, cy, a0, a1);
      b.Shape(v);
      break;
    }
    case 160: {  // Teardrop: the top-right corner pointed (adjustment 1: how far the point reaches)
      Pts v;
      Arc(v, cx, cy, cx, cy, 0, 270);
      v.push_back({cx + cx * A(0), cy - cy * A(0)});
      b.Shape(v);
      break;
    }
    case 158: {  // Frame
      const double a = A(0) * m;
      b.Fill(Rect(0, 0, w, a)), b.Fill(Rect(0, h - a, w, h)), b.Fill(Rect(0, a, a, h - a)), b.Fill(Rect(w - a, a, w, h - a));
      b.Line(Rect(0, 0, w, h), true), b.Line(Rect(a, a, w - a, h - a), true);
      break;
    }
    case 159: {  // Half Frame: the top and the left bars
      const double t = A(0) * m, l = A(1) * m;
      b.Shape({{0, 0}, {w, 0}, {w - t * w / std::max(w, h), t}, {l, t}, {l, h - l * h / std::max(w, h)}, {0, h}});
      break;
    }
    case 162: b.Shape({{0, 0}, {A(0) * m, 0}, {A(0) * m, h - A(0) * m}, {w, h - A(0) * m}, {w, h}, {0, h}}); break;  // L-Shape
    case 141: b.Shape({{0, A(0) * h}, {A(0) * w, 0}, {w, 0}, {0, h}}); break;                                                 // Diagonal Stripe
    case 11: {  // Cross
      const double a = A(0) * m;
      b.Shape({{a, 0}, {w - a, 0}, {w - a, a}, {w, a}, {w, h - a}, {w - a, h - a}, {w - a, h}, {a, h}, {a, h - a}, {0, h - a}, {0, a}, {a, a}});
      break;
    }
    case 28: {  // Plaque: the corners cut by quarter circles
      const double r = A(0) * m;
      Pts v = {{r, 0}, {w - r, 0}};
      Arc(v, w, 0, r, r, 180, 90, 10);
      v.push_back({w, h - r});
      Arc(v, w, h, r, r, 270, 180, 10);
      v.push_back({r, h});
      Arc(v, 0, h, r, r, 0, -90, 10);
      v.push_back({0, r});
      Arc(v, 0, 0, r, r, 90, 0, 10);
      b.Shape(v);
      break;
    }
    case 13: Can(b, w, h, A(0) * m / 2); break;
    case 86: Can(b, w, h, h / 6); break;
    case 14: {  // Cube
      const double a = A(0) * m;
      b.Shape(Rect(0, a, w - a, h));
      b.Shape({{0, a}, {a, 0}, {w, 0}, {w - a, a}}, 1);
      b.Shape({{w - a, a}, {w, 0}, {w, h - a}, {w - a, h}}, -1);
      break;
    }
    case 15: {  // Bevel
      const double a = A(0) * m;
      b.Fill({{0, 0}, {w, 0}, {w - a, a}, {a, a}}, 1);
      b.Fill({{0, 0}, {a, a}, {a, h - a}, {0, h}}, 1);
      b.Fill({{w, 0}, {w, h}, {w - a, h - a}, {w - a, a}}, -1);
      b.Fill({{0, h}, {a, h - a}, {w - a, h - a}, {w, h}}, -2);
      b.Shape(Rect(a, a, w - a, h - a));
      b.Line(Rect(0, 0, w, h), true);
      const double diag[4][4] = {{0, 0, a, a}, {w, 0, w - a, a}, {w, h, w - a, h - a}, {0, h, a, h - a}};
      for (const auto& d : diag) b.Line({{d[0], d[1]}, {d[2], d[3]}});
      break;
    }
    case 18: b.Ring(cx, cy, cx, cy, A(0) * m); break;  // Donut
    case 19: {                                            // "No" Symbol
      const double t = A(0) * m;
      b.Ring(cx, cy, cx, cy, t);
      const double rx = cx - t, ry = cy - t, d = 18;
      auto at = [&](double deg) { return ShapePt{cx + rx * std::cos(deg * kPi / 180), cy + ry * std::sin(deg * kPi / 180)}; };
      b.Shape({at(225 + d), at(45 - d), at(45 + d), at(225 - d)});
      break;
    }
    case 20: {  // Block Arc: a ring from angle 1 to angle 2, adjustment 3 its thickness
      Pts v;
      const double t = A(2) * m, a0 = A(0), a1 = A(1) <= a0 ? A(1) + 360 : A(1);
      Arc(v, cx, cy, cx, cy, a0, a1);
      Arc(v, cx, cy, std::max(0.0, cx - t), std::max(0.0, cy - t), a1, a0);
      b.Shape(v);
      break;
    }
    case 16: {  // Folded Corner
      const double a = A(0) * m;
      b.Shape({{0, 0}, {w, 0}, {w, h - a}, {w - a, h}, {0, h}});
      b.Shape({{w, h - a}, {w - a, h}, {w - a * 0.8, h - a * 0.8}}, -1);
      break;
    }
    case 17: {  // Smiley Face: the smile's curve (negative: a frown)
      b.Shape(Ellipse(cx, cy, cx, cy));
      b.Shape(Ellipse(w * 0.35, h * 0.37, m * 0.05, m * 0.05), -1);
      b.Shape(Ellipse(w * 0.65, h * 0.37, m * 0.05, m * 0.05), -1);
      Pts smile;
      const double y0 = h * 0.7, ctrl = y0 + A(0) * h * 4;
      for (int i = 0; i <= 16; ++i) {
        const double t = i / 16.0, u = 1 - t;
        smile.push_back({w * (0.28 + 0.44 * t), u * u * y0 + 2 * u * t * ctrl + t * t * y0});
      }
      b.Line(smile);
      break;
    }
    case 21: {  // Heart
      Pts v;
      for (int i = 0; i < 72; ++i) {
        const double t = 2 * kPi * i / 72, st = std::sin(t);
        v.push_back({16 * st * st * st, -(13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t))});
      }
      Fit(v, w, h);
      b.Shape(v);
      break;
    }
    case 22:  // Lightning Bolt
      b.Shape(Vml({8458, 0, 0, 3923, 7564, 8416, 4993, 9720, 12197, 13904, 9987, 14934, 21600, 21600, 14768, 12911, 16558, 12016, 11030, 6840, 12831, 6120},
                  w, h));
      break;
    case 23: {  // Sun: adjustment 1 the inner circle's radius (of the half size)
      const double rin = std::clamp(A(0), 0.1, 0.75), rbase = std::min(0.95, rin + 0.18);
      for (int i = 0; i < 8; ++i) {
        const double a = i * 45.0, d = 9.0;
        auto at = [&](double deg, double r) { return ShapePt{cx + cx * r * std::cos(deg * kPi / 180), cy + cy * r * std::sin(deg * kPi / 180)}; };
        b.Shape({at(a, 1.0), at(a + d, rbase), at(a - d, rbase)});
      }
      b.Shape(Ellipse(cx, cy, cx * rin, cy * rin));
      break;
    }
    case 24: {  // Moon
      Pts v;
      Arc(v, w, cy, w, cy, 90, 270);
      Arc(v, w, cy, w * (1 - A(0)), cy, 270, 90);
      b.Shape(v);
      break;
    }
    case 179: b.Shape(Cloud(0, 0, w, h)); break;
    case 25: {  // Arc: a line only, from angle 1 to angle 2
      Pts v;
      const double a0 = A(0), a1 = A(1) <= a0 ? A(1) + 360 : A(1);
      Arc(v, cx, cy, cx, cy, a0, a1);
      b.Line(v);
      break;
    }
    case 26: {  // Double Bracket
      const double r = A(0) * m;
      b.Fill(Rect(0, 0, w, h));
      Pts l = {{r, 0}};
      Arc(l, r, r, r, r, 270, 180, 8);
      Arc(l, r, h - r, r, r, 180, 90, 8);
      b.Line(l);
      b.Line(MirrorX(l, w));
      break;
    }
    case 27: {  // Double Brace
      const double r = A(0) * m;
      b.Fill(Rect(0, 0, w, h));
      Pts l = {{2 * r, 0}};
      Arc(l, 2 * r, r, r, r, 270, 180, 6);
      Arc(l, 0, cy - r, r, r, 0, 90, 6);
      Arc(l, 0, cy + r, r, r, 270, 360, 6);
      Arc(l, 2 * r, h - r, r, r, 180, 90, 6);
      b.Line(l);
      b.Line(MirrorX(l, w));
      break;
    }
    case 29: case 30: {  // Left / Right Bracket
      const double r = A(0) * h;
      Pts l = {{w, 0}};
      Arc(l, w, r, w, r, 270, 180, 8);
      Arc(l, w, h - r, w, r, 180, 90, 8);
      if (type == 30) l = MirrorX(l, w);
      b.Fill(l);
      b.Line(l);
      break;
    }
    case 31: case 32: {  // Left / Right Brace: adjustment 1 the curl, 2 where the point is
      const double r = A(0) * h, pc = std::clamp(A(1), 0.1, 0.9) * h;
      Pts l = {{w, 0}};
      Arc(l, w, r, w / 2, r, 270, 180, 6);
      Arc(l, 0, pc - r, w / 2, r, 0, 90, 6);
      Arc(l, 0, pc + r, w / 2, r, 270, 360, 6);
      Arc(l, w, h - r, w / 2, r, 180, 90, 6);
      if (type == 32) l = MirrorX(l, w);
      b.Fill(l);
      b.Line(l);
      break;
    }
    // ---- block arrows ----
    case 33: b.Shape(Orient(ArrowUV(w, h, A(0), A(1) * m), 0, w, h)); break;
    case 34: b.Shape(Orient(ArrowUV(w, h, A(0), A(1) * m), 1, w, h)); break;
    case 35: b.Shape(Orient(ArrowUV(h, w, A(0), A(1) * m), 2, w, h)); break;
    case 36: b.Shape(Orient(ArrowUV(h, w, A(0), A(1) * m), 3, w, h)); break;
    case 37: b.Shape(Orient(ArrowUV(w, h, A(0), A(1) * m, 0, 2), 0, w, h)); break;
    case 38: b.Shape(Orient(ArrowUV(h, w, A(0), A(1) * m, 0, 2), 3, w, h)); break;
    case 49: {  // Striped Right
      const double s0 = h * 0.25, s1 = h * 0.75, k = 0.03125 * m;
      b.Shape(Rect(0, s0, k, s1));
      b.Shape(Rect(2 * k, s0, 4 * k, s1));
      Pts v = Orient(ArrowUV(w - 5 * k, h, A(0), A(1) * m), 0, w, h);
      for (auto& p : v) p.x += 5 * k;
      b.Shape(v);
      break;
    }
    case 50: b.Shape(Orient(ArrowUV(w, h, A(0), A(1) * m, 0.25 * m), 0, w, h)); break;  // Notched Right
    case 51: b.Shape({{0, 0}, {w - A(0) * m, 0}, {w, cy}, {w - A(0) * m, h}, {0, h}}); break;  // Pentagon
    case 52: b.Shape({{0, 0}, {w - A(0) * m, 0}, {w, cy}, {w - A(0) * m, h}, {0, h}, {A(0) * m, cy}}); break;  // Chevron
    case 39: case 40: case 43: {  // Quad, Left-Right-Up, Left-Up
      const double a = 0.1125 * m, hb = 0.225 * m, c = A(0) * m;
      const double qx = type == 43 ? w - hb : cx, qy = type == 39 ? cy : h - hb;
      Pts v = {{qx, 0}, {qx + hb, c}, {qx + a, c}};
      if (type == 43) {
        v.push_back({qx + a, qy + a});
      } else {
        v.insert(v.end(), {{qx + a, qy - a}, {w - c, qy - a}, {w - c, qy - hb}, {w, qy}, {w - c, qy + hb}, {w - c, qy + a}});
        if (type == 39) v.insert(v.end(), {{qx + a, qy + a}, {qx + a, h - c}, {qx + hb, h - c}, {qx, h}, {qx - hb, h - c}, {qx - a, h - c}, {qx - a, qy + a}});
      }
      v.insert(v.end(), {{c, qy + a}, {c, qy + hb}, {0, qy}, {c, qy - hb}, {c, qy - a}, {qx - a, qy - a}, {qx - a, c}, {qx - hb, c}});
      b.Shape(v);
      break;
    }
    case 41: {  // Bent: shaft thickness, head width, head length
      const double t = A(0) * m, hw = A(1) * m, hl = A(2) * m;
      b.Shape({{0, h}, {0, hw - t / 2}, {w - hl, hw - t / 2}, {w - hl, 0}, {w, hw}, {w - hl, 2 * hw}, {w - hl, hw + t / 2}, {t, hw + t / 2}, {t, h}});
      break;
    }
    case 42: {  // U-Turn: shaft thickness, head width, head length
      const double t = A(0) * m, hw = A(1) * m, hl = A(2) * m;
      const double xr = w - hw + t / 2, R = xr / 2, yh = h * 0.75 - hl;
      Pts v = {{0, h}};
      Arc(v, R, std::min(R, yh), R, std::min(R, yh), 180, 360);
      v.insert(v.end(), {{xr, yh}, {w, yh}, {w - hw, yh + hl}, {w - 2 * hw, yh}, {xr - t, yh}});
      Arc(v, R, std::min(R, yh), std::max(0.0, R - t), std::max(0.0, std::min(R, yh) - t), 360, 180);
      v.push_back({t, h});
      b.Shape(v);
      break;
    }
    case 44: {  // Bent-Up: shaft thickness, head width, head length
      const double t = A(0) * m, hw = A(1) * m, hl = A(2) * m;
      b.Shape({{0, h - t}, {w - hw - t / 2, h - t}, {w - hw - t / 2, hl}, {w - 2 * hw, hl}, {w - hw, 0}, {w, hl}, {w - hw + t / 2, hl}, {w - hw + t / 2, h}, {0, h}});
      break;
    }
    case 45: case 46: case 47: case 48: {  // Curved Right / Left / Up / Down: thickness, head width, head length
      const bool across = type == 47 || type == 48;
      const double L = across ? h : w, T = across ? w : h, mm = std::min(L, T);
      const double th = A(0) * T, hw = A(1) * T, hl = A(2) * mm;
      Pts v = CurvedArrow(L - hl, T / 2, std::max(1e-6, L - hl - th / 2), std::max(1e-6, T / 2 - hw), 270, 90, th, hl, hw);
      if (type == 46 || type == 47) v = MirrorX(v, L);
      if (across) v = Transpose(v);
      b.Shape(v);
      break;
    }
    case 60: {  // Circular: thickness, where it ends and where it starts (degrees)
      const double t = A(0) * m, hw = 0.125 * m;
      const double a0 = A(2), a1 = A(1) <= a0 ? A(1) + 360 : A(1);
      b.Shape(CurvedArrow(cx, cy, cx - hw, cy - hw, a0, a1, t, 0.2 * m, hw));
      break;
    }
    case 53: case 54: case 55: case 56: {  // Right / Left / Up / Down Arrow Callout
      const bool across = type == 55 || type == 56;
      const double L = across ? h : w, T = across ? w : h, mm = std::min(L, T);
      const double bw = L * A(0), a = 0.125 * mm, hb = 0.25 * mm, hl = 0.25 * mm, c = T / 2;
      Pts v = {{0, 0}, {bw, 0}, {bw, c - a}, {L - hl, c - a}, {L - hl, c - hb}, {L, c}, {L - hl, c + hb}, {L - hl, c + a}, {bw, c + a}, {bw, T}, {0, T}};
      b.Shape(Orient(v, type == 53 ? 0 : type == 54 ? 1 : type == 55 ? 2 : 3, w, h));
      break;
    }
    case 57: case 59: {  // Left-Right / Quad Arrow Callout
      const double a = 0.1 * m, hb = 0.2 * m, hl = 0.2 * m;
      const double bx = w * A(0), by = type == 57 ? 0 : h * A(0);
      Pts v;
      if (type == 59) v.insert(v.end(), {{bx, by}, {cx - a, by}, {cx - a, hl}, {cx - hb, hl}, {cx, 0}, {cx + hb, hl}, {cx + a, hl}, {cx + a, by}});
      else v.push_back({bx, by});
      v.insert(v.end(), {{w - bx, by}, {w - bx, cy - a}, {w - hl, cy - a}, {w - hl, cy - hb}, {w, cy}, {w - hl, cy + hb}, {w - hl, cy + a}, {w - bx, cy + a}, {w - bx, h - by}});
      if (type == 59) v.insert(v.end(), {{cx + a, h - by}, {cx + a, h - hl}, {cx + hb, h - hl}, {cx, h}, {cx - hb, h - hl}, {cx - a, h - hl}, {cx - a, h - by}});
      v.insert(v.end(), {{bx, h - by}, {bx, cy + a}, {hl, cy + a}, {hl, cy + hb}, {0, cy}, {hl, cy - hb}, {hl, cy - a}, {bx, cy - a}});
      b.Shape(v);
      break;
    }
    // ---- equation shapes ----
    case 163: case 165: {  // Plus, Multiply
      const double t = 0.2352 * m / 2, mx = 0.12 * w, my = 0.12 * h;
      Pts v = {{cx - t, my}, {cx + t, my}, {cx + t, cy - t}, {w - mx, cy - t}, {w - mx, cy + t}, {cx + t, cy + t}, {cx + t, h - my}, {cx - t, h - my}, {cx - t, cy + t}, {mx, cy + t}, {mx, cy - t}, {cx - t, cy - t}};
      if (type == 165) {
        for (auto& p : v) {
          const double dx = (p.x - cx) * 0.8, dy = (p.y - cy) * 0.8;
          p = {cx + (dx - dy) * 0.7071, cy + (dx + dy) * 0.7071};
        }
      }
      b.Shape(v);
      break;
    }
    case 164: b.Shape(Rect(0.12 * w, cy - 0.1176 * m, 0.88 * w, cy + 0.1176 * m)); break;  // Minus
    case 166: {                                                                         // Division
      const double t = 0.1176 * m;
      b.Shape(Rect(0.12 * w, cy - t / 2, 0.88 * w, cy + t / 2));
      b.Shape(Ellipse(cx, cy - 2.2 * t, t, t));
      b.Shape(Ellipse(cx, cy + 2.2 * t, t, t));
      break;
    }
    case 167: case 168: {  // Equal, Not Equal
      const double t = 0.1176 * m;
      b.Shape(Rect(0.12 * w, cy - 1.5 * t, 0.88 * w, cy - 0.5 * t));
      b.Shape(Rect(0.12 * w, cy + 0.5 * t, 0.88 * w, cy + 1.5 * t));
      if (type == 168) b.Shape({{0.62 * w, 0.08 * h}, {0.72 * w, 0.08 * h}, {0.38 * w, 0.92 * h}, {0.28 * w, 0.92 * h}});
      break;
    }
    // ---- flowchart ----
    case 64: b.Shape({{0.2 * w, 0}, {w, 0}, {0.8 * w, h}, {0, h}}); break;
    case 65: b.Shape(Rect(0, 0, w, h)), b.Line({{w / 8, 0}, {w / 8, h}}), b.Line({{w * 7 / 8, 0}, {w * 7 / 8, h}}); break;
    case 66: b.Shape(Rect(0, 0, w, h)), b.Line({{w / 8, 0}, {w / 8, h}}), b.Line({{0, h / 8}, {w, h / 8}}); break;
    case 67: case 68: {  // Document, Multidocument
      auto doc = [&](double x0, double y0, double dw, double dh) {
        Pts v = {{x0, y0}, {x0 + dw, y0}};
        Pts wave;
        Wave(wave, x0 + dw, x0, dw, y0 + dh * 0.84, dh * 0.13, 1);
        for (auto& p : wave) p.y = y0 + dh * 0.84 + dh * 0.13 * std::sin(2 * kPi * (p.x - x0) / dw);
        v.insert(v.end(), wave.begin(), wave.end());
        return v;
      };
      if (type == 67) {
        b.Shape(doc(0, 0, w, h));
      } else {
        const double dx = 0.1 * w, dy = 0.1 * h;
        b.Shape(doc(2 * dx, 0, w - 2 * dx, h - 2 * dy));
        b.Shape(doc(dx, dy, w - 2 * dx, h - 2 * dy));
        b.Shape(doc(0, 2 * dy, w - 2 * dx, h - 2 * dy));
      }
      break;
    }
    case 69: b.Shape(Corners(w, h, 1, 1, 1, 1, std::min(cx, cy))); break;  // Terminator
    case 70: b.Shape({{0.2 * w, 0}, {0.8 * w, 0}, {w, cy}, {0.8 * w, h}, {0.2 * w, h}, {0, cy}}); break;
    case 71: b.Shape({{0, 0.2 * h}, {w, 0}, {w, h}, {0, h}}); break;
    case 72: b.Shape({{0, 0}, {w, 0}, {0.8 * w, h}, {0.2 * w, h}}); break;
    case 74: b.Shape({{0, 0}, {w, 0}, {w, 0.8 * h}, {cx, h}, {0, 0.8 * h}}); break;
    case 75: b.Shape({{0.2 * w, 0}, {w, 0}, {w, h}, {0, h}, {0, 0.2 * h}}); break;
    case 76: {  // Punched Tape
      Pts v;
      Wave(v, 0, w, w, 0.1 * h, -0.1 * h, 1);
      Wave(v, w, 0, w, 0.9 * h, -0.1 * h, 1);
      b.Shape(v);
      break;
    }
    case 77: case 78: {  // Summing Junction, Or
      b.Shape(Ellipse(cx, cy, cx, cy));
      if (type == 77) {
        b.Line({{cx - cx * 0.7071, cy - cy * 0.7071}, {cx + cx * 0.7071, cy + cy * 0.7071}});
        b.Line({{cx + cx * 0.7071, cy - cy * 0.7071}, {cx - cx * 0.7071, cy + cy * 0.7071}});
      } else {
        b.Line({{cx, 0}, {cx, h}}), b.Line({{0, cy}, {w, cy}});
      }
      break;
    }
    case 79: b.Fill({{0, 0}, {w, 0}, {cx, cy}}), b.Fill({{cx, cy}, {w, h}, {0, h}}), b.Line({{0, 0}, {w, 0}, {0, h}, {w, h}}, true); break;
    case 80: b.Shape({{cx, 0}, {w, cy}, {cx, h}, {0, cy}}), b.Line({{0, cy}, {w, cy}}); break;
    case 81: b.Shape({{cx, 0}, {w, h}, {0, h}}); break;
    case 82: b.Shape({{0, 0}, {w, 0}, {cx, h}}); break;
    case 83: {  // Stored Data
      Pts v = {{w / 6, 0}, {w, 0}};
      Arc(v, w, cy, w / 6, cy, 270, 90, 16);
      v.push_back({w / 6, h});
      Arc(v, w / 6, cy, w / 6, cy, 90, 270, 16);
      b.Shape(v);
      break;
    }
    case 84: {  // Delay
      Pts v = {{0, 0}, {cx, 0}};
      Arc(v, cx, cy, cx, cy, 270, 450, 24);
      v.push_back({0, h});
      b.Shape(v);
      break;
    }
    case 85: b.Shape(Ellipse(cx, cy, cx, cy)), b.Line({{cx, h}, {w, h}, {w, h * 0.85}}); break;
    case 87: {  // Direct Access Storage: a can lying down
      const double rx = w / 6;
      Pts body = {{rx, 0}, {w - rx, 0}};
      Arc(body, w - rx, cy, rx, cy, 270, 450, 24);
      body.push_back({rx, h});
      Arc(body, rx, cy, rx, cy, 90, 270, 24);
      b.Fill(body);
      b.Fill(Ellipse(w - rx, cy, rx, cy), 1);
      b.Line(body, true);
      b.Line(Ellipse(w - rx, cy, rx, cy), true);
      break;
    }
    case 88: {  // Display
      Pts v = {{0, cy}, {w / 6, 0}, {w * 5 / 6, 0}};
      Arc(v, w * 5 / 6, cy, w / 6, cy, 270, 450, 16);
      v.push_back({w / 6, h});
      b.Shape(v);
      break;
    }
    // ---- stars and banners ----
    case 89:
      b.Shape(Vml({10800, 5800, 14522, 0, 14155, 5325, 18380, 4457, 16702, 7315, 21097, 8137, 17607, 10475, 21600, 13290, 16837, 12942, 18145, 18095, 14020, 14457,
                   13247, 19737, 10532, 14935, 8485, 21600, 7715, 15627, 4762, 17617, 5667, 13937, 135, 14587, 3722, 11775, 0, 8615, 4627, 7617, 370, 2295, 7312, 6320,
                   8352, 2295},
                  w, h));
      break;
    case 90:
      b.Shape(Vml({11462, 4342, 14790, 0, 14525, 5777, 18007, 3172, 16380, 6532, 21600, 6645, 16985, 9402, 18270, 11290, 16380, 12310, 18877, 15632, 14640, 14350,
                   14942, 17370, 12180, 15935, 11612, 18842, 9872, 17370, 8700, 19712, 7527, 18125, 4917, 21600, 4805, 18240, 1285, 17825, 3330, 15370, 0, 12877,
                   3935, 11592, 1172, 8270, 5372, 7817, 4502, 3625, 8550, 6382, 9722, 1887},
                  w, h));
      break;
    case 91: b.Shape(Star(4, A(0), w, h)); break;
    case 92: b.Shape(Star(5, A(0), w, h)); break;
    case 147: b.Shape(Star(6, A(0), w, h)); break;
    case 148: b.Shape(Star(7, A(0), w, h)); break;
    case 93: b.Shape(Star(8, A(0), w, h)); break;
    case 149: b.Shape(Star(10, A(0), w, h)); break;
    case 150: b.Shape(Star(12, A(0), w, h)); break;
    case 94: b.Shape(Star(16, A(0), w, h)); break;
    case 95: b.Shape(Star(24, A(0), w, h)); break;
    case 96: b.Shape(Star(32, A(0), w, h)); break;
    case 97: case 98: case 99: case 100: {  // Ribbons: tilted down / up, curved
      // the down ribbon: ends folded behind and down; up: mirrored top to bottom
      auto bend = [&](Pts v) {
        if (type == 99 || type == 100) {
          for (auto& p : v) p.y = p.y * 0.85 + 0.15 * h * 4 * (p.x / w - 0.5) * (p.x / w - 0.5);
        }
        return type == 97 || type == 99 ? MirrorY(v, h) : v;
      };
      // adjustment 1: where the middle band starts (of the width); 2: the band's height (of the height)
      const double e = A(0) * w, f = e + 0.075 * w, bh = A(1) * h;
      b.Shape(bend({{0, h - bh}, {f, h - bh}, {f, h}, {0, h}, {0.07 * w, (h - bh + h) / 2}}), -1);
      b.Shape(bend({{w, h - bh}, {w - f, h - bh}, {w - f, h}, {w, h}, {0.93 * w, (h - bh + h) / 2}}), -1);
      b.Shape(bend({{e, bh}, {f, bh}, {f, h}}), -2);
      b.Shape(bend({{w - e, bh}, {w - f, bh}, {w - f, h}}), -2);
      b.Shape(bend(Rect(e, 0, w - e, bh)));
      break;
    }
    case 101: case 102: {  // Scroll: Vertical / Horizontal
      const bool horiz = type == 102;
      const double W = horiz ? h : w, H = horiz ? w : h, r = A(0) * std::min(W, H);
      Pts body = Rect(r, r, W - r * 0.2, H - r);
      Pts top = Rect(r * 0.2, 0, W, 2 * r), bot = Rect(0, H - 2 * r, W - r * 0.8, H);
      if (horiz) body = Transpose(body), top = Transpose(top), bot = Transpose(bot);
      b.Shape(body);
      b.Shape(top, 1);
      b.Shape(bot, -1);
      break;
    }
    case 103: case 104: {  // Wave, Double Wave
      const double amp = A(0) * h, per = type == 103 ? 1 : 2;
      Pts v;
      Wave(v, 0, w, w, amp, amp, per);
      Wave(v, w, 0, w, h - amp, amp, per);
      b.Shape(v);
      break;
    }
    // ---- callouts ----
    case 105: case 106: {  // Speech Bubble: Rectangle / Rectangle with Corners Rounded
      Pts v = type == 105 ? Rect(0, 0, w, h) : Corners(w, h, 1, 1, 1, 1, 0.1667 * m);
      // the pointer leaves the bottom side
      Pts out;
      bool done = false;
      for (size_t i = 0; i < v.size(); ++i) {
        out.push_back(v[i]);
        const ShapePt& n = v[(i + 1) % v.size()];
        if (!done && v[i].y >= h - 1e-9 && n.y >= h - 1e-9 && v[i].x > n.x && v[i].x >= w / 3 && n.x <= w / 6 + 1e-9) {
          out.insert(out.end(), {{w / 3, h}, {A(0) * w, A(1) * h}, {w / 6, h}});
          done = true;
        }
      }
      if (!done) out.insert(out.end(), {{w / 3, h}, {A(0) * w, A(1) * h}, {w / 6, h}});
      b.Shape(out);
      break;
    }
    case 107: {  // Speech Bubble: Oval
      Pts v;
      Arc(v, cx, cy, cx, cy, 128, 465);
      v.push_back({A(0) * w, A(1) * h});
      b.Shape(v);
      break;
    }
    case 108: {  // Thought Bubble: Cloud
      b.Shape(Cloud(0, 0, w, h));
      // two bubbles on the way to the tip
      b.Shape(Ellipse(cx + (A(0) * w - cx) * 0.7, cy + (A(1) * h - cy) * 0.75, 0.045 * w, 0.045 * h));
      b.Shape(Ellipse(cx + (A(0) * w - cx) * 0.9, cy + (A(1) * h - cy) * 0.93, 0.03 * w, 0.03 * h));
      break;
    }
    case 109: LineCallout(b, w, h, true, false, 0, A(0), A(1)); break;
    case 110: LineCallout(b, w, h, true, false, 1, A(0), A(1)); break;
    case 111: LineCallout(b, w, h, true, false, 2, A(0), A(1)); break;
    case 113: LineCallout(b, w, h, false, true, 0, A(0), A(1)); break;
    case 114: LineCallout(b, w, h, false, true, 1, A(0), A(1)); break;
    case 115: LineCallout(b, w, h, false, true, 2, A(0), A(1)); break;
    case 117: LineCallout(b, w, h, false, false, 0, A(0), A(1)); break;
    case 118: LineCallout(b, w, h, false, false, 1, A(0), A(1)); break;
    case 119: LineCallout(b, w, h, false, false, 2, A(0), A(1)); break;
    case 121: LineCallout(b, w, h, true, true, 0, A(0), A(1)); break;
    case 122: LineCallout(b, w, h, true, true, 1, A(0), A(1)); break;
    case 123: LineCallout(b, w, h, true, true, 2, A(0), A(1)); break;
    default: return false;
  }
  return true;
}

std::vector<double> AutoShapeAdjustDefaults(int type) {
  switch (type) {
    case 5: case 151: case 152: case 153: case 154: case 155: case 156: case 157: case 28: case 16: case 26: return {0.16667};
    case 27: return {0.0833};
    case 2: case 3: case 10: case 11: case 13: case 14: case 18: return {0.25};
    case 20: return {180, 0, 0.25};
    case 6: return {0.29289};
    case 15: case 158: return {0.125};
    case 19: return {0.1875};
    case 162: case 141: case 24: case 51: case 52: case 7: return {0.5};
    case 33: case 34: case 35: case 36: case 37: case 38: case 49: case 50: return {0.5, 0.5};
    case 39: case 40: case 43: return {0.225};
    case 53: case 54: case 55: case 56: return {0.65};
    case 91: return {0.25};
    case 92: return {0.382};
    case 147: return {0.577};
    case 148: return {0.692};
    case 93: case 150: case 94: case 95: case 96: return {0.75};
    case 149: return {0.85};
    case 103: return {0.125};
    case 142: return {0, 270};
    case 161: return {45, 270};
    case 25: return {270, 0};
    case 160: return {1.0};
    case 159: return {0.3333, 0.3333};
    case 17: return {0.0465};
    case 23: return {0.5};
    case 29: case 30: return {0.0833};
    case 31: case 32: return {0.0833, 0.5};
    case 41: case 42: case 44: return {0.25, 0.25, 0.25};
    case 45: case 46: case 47: case 48: return {0.25, 0.25, 0.3};
    case 60: return {0.125, 345, 180};
    case 57: return {0.25};
    case 59: return {0.3};
    case 97: case 98: case 99: case 100: return {0.125, 0.75};
    case 101: case 102: return {0.125};
    case 104: return {0.0625};
    case 105: case 106: case 107: case 108: return {0.29, 1.125};
    case 109: case 113: case 117: case 121: return {-0.3833, 1.125};
    case 110: case 111: case 114: case 115: case 118: case 119: case 122: case 123: return {-0.4667, 1.125};
    default: return {};
  }
}

namespace {

// one yellow handle: where it is for the adjustments, and the adjustments when it is dragged to p
struct AdjHandleDef {
  std::function<ShapePt(const std::vector<double>&)> pos;
  std::function<void(ShapePt, std::vector<double>&)> drag;
};

std::vector<AdjHandleDef> AdjHandles(int type, double w, double h) {
  const double m = std::max(1e-9, std::min(w, h));
  std::vector<AdjHandleDef> v;
  auto clampv = [](double x, double lo, double hi) { return std::clamp(x, lo, hi); };
  // a value read along x (or y) from the left (top), scaled by s
  auto along_x = [&](double y, double s, double lo, double hi, bool from_right = false) {
    v.push_back({[=](const std::vector<double>& a) { return ShapePt{from_right ? w - a[0] * s : a[0] * s, y}; },
                 [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((from_right ? w - p.x : p.x) / s, lo, hi); }});
  };
  auto along_y = [&](double x, double s, double lo, double hi) {
    v.push_back({[=](const std::vector<double>& a) { return ShapePt{x, a[0] * s}; },
                 [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.y / s, lo, hi); }});
  };
  // a point on the ellipse at angle a (degrees), and the angle of a point
  const double cx = w / 2, cy = h / 2;
  auto on_ellipse = [=](double a, double rx, double ry) { return ShapePt{cx + rx * std::cos(a * kPi / 180), cy + ry * std::sin(a * kPi / 180)}; };
  // the angle of a point on the ellipse of radii rx, ry (the one the handle runs on: an inset one is rounder or flatter
  // than the shape's, and reading the angle on the shape's own moved a handle when it was only clicked)
  auto angle_of = [=](ShapePt p, double rx = -1, double ry = -1) {
    if (rx < 0) rx = cx, ry = cy;
    const double a = std::atan2((p.y - cy) / std::max(1e-9, ry), (p.x - cx) / std::max(1e-9, rx)) * 180 / kPi;
    return std::fmod(a + 360.0, 360.0);
  };
  auto angle_handle = [&](size_t i, double rx, double ry) {
    v.push_back({[=](const std::vector<double>& a) { return on_ellipse(a[i], rx, ry); },
                 [=](ShapePt p, std::vector<double>& a) { a[i] = std::round(angle_of(p, rx, ry)); }});
  };
  // how far p is inward from `from` along the ray at angle deg (a thickness handle moves along that ray)
  auto inward = [](ShapePt from, double deg, ShapePt p) {
    const double c = std::cos(deg * kPi / 180), sn = std::sin(deg * kPi / 180);
    return (from.x - p.x) * c + (from.y - p.y) * sn;
  };
  switch (type) {
    case 142: case 161: case 25: angle_handle(0, cx, cy), angle_handle(1, cx, cy); break;
    case 20:
      angle_handle(0, cx, cy), angle_handle(1, cx, cy);
      v.push_back({[=](const std::vector<double>& a) { return on_ellipse(a[0], cx - a[2] * m, cy - a[2] * m); },
                   [=](ShapePt p, std::vector<double>& a) { a[2] = clampv(inward(on_ellipse(a[0], cx, cy), a[0], p) / m, 0, 0.5); }});
      break;
    case 60:
      v.push_back({[=](const std::vector<double>& a) { return on_ellipse(a[2], cx - 0.125 * m - a[0] * m / 2, cy - 0.125 * m - a[0] * m / 2); },
                   [=](ShapePt p, std::vector<double>& a) {
                     a[0] = clampv(2 * inward(on_ellipse(a[2], cx - 0.125 * m, cy - 0.125 * m), a[2], p) / m, 0.02, 0.4);
                   }});
      angle_handle(1, cx - 0.125 * m, cy - 0.125 * m);
      angle_handle(2, cx - 0.125 * m, cy - 0.125 * m);
      v.erase(v.end() - 1);  // the start angle is moved by the thickness handle's place
      v.push_back({[=](const std::vector<double>& a) { return on_ellipse(a[2], cx - 0.125 * m + a[0] * m / 2, cy - 0.125 * m + a[0] * m / 2); },
                   [=](ShapePt p, std::vector<double>& a) { a[2] = std::round(angle_of(p, cx - 0.125 * m + a[0] * m / 2, cy - 0.125 * m + a[0] * m / 2)); }});
      break;
    case 160:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{cx + cx * a[0], cy - cy * a[0]}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((p.x - cx) / std::max(1e-9, cx), 0, 2); }});
      break;
    case 159:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w / 2, a[0] * m}; }, [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.y / m, 0, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{a[1] * m, h / 2}; }, [=](ShapePt p, std::vector<double>& a) { a[1] = clampv(p.x / m, 0, 0.5); }});
      break;
    case 17:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{cx, h * 0.7 + a[0] * h * 2}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((p.y - h * 0.7) / h / 2, -0.0465, 0.0465); }});
      break;
    case 23:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{cx + cx * a[0], cy}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((p.x - cx) / std::max(1e-9, cx), 0.1, 0.75); }});
      break;
    case 29: case 30:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{type == 29 ? w : 0, a[0] * h}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.y / h, 0, 0.5); }});
      break;
    case 31: case 32:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{type == 31 ? w : 0, a[0] * h}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.y / h, 0, 0.25); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{type == 31 ? 0 : w, a[1] * h}; },
                   [=](ShapePt p, std::vector<double>& a) { a[1] = clampv(p.y / h, 0.1, 0.9); }});
      break;
    case 41:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{a[0] * m, h}; }, [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.x / m, 0.02, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - a[2] * m, 2 * a[1] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[1] = clampv(p.y / 2 / m, 0.02, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - a[2] * m, a[1] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[2] = clampv((w - p.x) / m, 0, w / m * 0.8); }});
      break;
    case 42:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{a[0] * m, h}; }, [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.x / m, 0.02, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - 2 * a[1] * m, h * 0.75 - a[2] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[1] = clampv((w - p.x) / 2 / m, 0.02, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - a[1] * m, h * 0.75 - a[2] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[2] = clampv((h * 0.75 - p.y) / m, 0, 0.6); }});
      break;
    case 44:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{0, h - a[0] * m}; }, [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((h - p.y) / m, 0.02, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - 2 * a[1] * m, a[2] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[1] = clampv((w - p.x) / 2 / m, 0.02, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - a[1] * m, a[2] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[2] = clampv(p.y / m, 0, h / m * 0.8); }});
      break;
    case 45: case 46: case 47: case 48: {
      // in the curved-right frame (L along, T across), mirrored and transposed as the drawing is
      const bool across = type == 47 || type == 48, mirror = type == 46 || type == 47;
      const double L = across ? h : w, T = across ? w : h, mm = std::min(L, T);
      auto out = [=](ShapePt q) {
        if (mirror) q.x = L - q.x;
        return across ? ShapePt{q.y, q.x} : q;
      };
      auto in = [=](ShapePt q) {
        if (across) std::swap(q.x, q.y);
        if (mirror) q.x = L - q.x;
        return q;
      };
      v.push_back({[=](const std::vector<double>& a) { return out({a[0] * T, T / 2}); }, [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(in(p).x / T, 0.02, 0.45); }});
      v.push_back({[=](const std::vector<double>& a) { return out({L - a[2] * mm, T - 2 * a[1] * T}); },
                   [=](ShapePt p, std::vector<double>& a) { a[1] = clampv((T - in(p).y) / 2 / T, 0.05, 0.5); }});
      v.push_back({[=](const std::vector<double>& a) { return out({L - a[2] * mm, T - a[1] * T}); },
                   [=](ShapePt p, std::vector<double>& a) { a[2] = clampv((L - in(p).x) / mm, 0.05, 0.9); }});
      break;
    }
    case 57: along_x(0, w, 0.05, 0.45); break;
    case 59: along_x(h * 0.3, w, 0.05, 0.45); break;
    case 97: case 98: case 99: case 100: {
      const bool up = type == 97 || type == 99;
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{a[0] * w, up ? h - a[1] * h : a[1] * h}; },
                   [=](ShapePt p, std::vector<double>& a) {
                     a[0] = clampv(p.x / w, 0.02, 0.4);
                     a[1] = clampv((up ? h - p.y : p.y) / h, 0.4, 0.95);
                   }});
      break;
    }
    case 101: case 102: {
      const bool horiz = type == 102;
      v.push_back({[=](const std::vector<double>& a) { return horiz ? ShapePt{2 * a[0] * m, 0} : ShapePt{0, 2 * a[0] * m}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((horiz ? p.x : p.y) / 2 / m, 0.02, 0.25); }});
      break;
    }
    case 5: case 152: case 153: case 154: case 156: case 157: case 2: case 3: case 10: case 6: case 11: case 162: along_x(0, m, 0, w / m * (type == 162 ? 1 : 0.5)); break;
    case 151: case 155: along_x(0, m, 0, 0.5, true); break;
    case 158: case 15: along_x(h / 2, m, 0, 0.5); break;
    case 18: case 19: along_x(h / 2, m, 0, 0.5); break;
    case 28: case 26: case 27: along_y(0, m, 0, 0.5); break;
    case 14: along_y(0, m, 0, 1); break;
    case 13: along_y(w / 2, m, 0, h / m); break;
    case 16:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w - a[0] * m, h}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((w - p.x) / m, 0, 0.5); }});
      break;
    case 141: case 7: along_x(0, w, 0, 1); break;
    case 24: v.push_back({[=](const std::vector<double>& a) { return ShapePt{w * (1 - (1 - a[0])) , h / 2}; },
                          [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.x / w, 0, 0.875); }});
      break;
    case 51: case 52: along_x(0, m, 0, w / m, true); break;
    case 39: case 40: case 43: along_x(h / 2, m, 0, 0.5); break;
    case 33: case 34: case 35: case 36: case 37: case 38: case 49: case 50: {
      // in the arrow's own frame (u along it, v across): the shaft's edge and the head's start
      const int dir = type == 34 ? 1 : type == 35 ? 2 : type == 36 || type == 38 ? 3 : 0;
      const bool across = dir >= 2;
      const double L = across ? h : w, T = across ? w : h;
      auto to_box = [=](double u, double vv) {
        switch (dir) {
          case 0: return ShapePt{u, vv};
          case 1: return ShapePt{w - u, vv};
          case 2: return ShapePt{vv, h - u};
          default: return ShapePt{vv, u};
        }
      };
      auto to_uv = [=](ShapePt p) {
        switch (dir) {
          case 0: return ShapePt{p.x, p.y};
          case 1: return ShapePt{w - p.x, p.y};
          case 2: return ShapePt{h - p.y, p.x};
          default: return ShapePt{p.y, p.x};
        }
      };
      v.push_back({[=](const std::vector<double>& a) { return to_box(L - a[1] * m, T * (1 - a[0]) / 2); },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(1 - 2 * to_uv(p).y / T, 0, 1); }});
      v.push_back({[=](const std::vector<double>& a) { return to_box(L - a[1] * m, 0); },
                   [=](ShapePt p, std::vector<double>& a) { a[1] = clampv((L - to_uv(p).x) / m, 0, L / m); }});
      break;
    }
    case 53: case 54: case 55: case 56: {
      const bool across = type == 55 || type == 56;
      const double L = across ? h : w;
      if (across) {
        const bool up = type == 55;
        v.push_back({[=](const std::vector<double>& a) { return ShapePt{0, up ? h - a[0] * L : a[0] * L}; },
                     [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((up ? h - p.y : p.y) / L, 0.1, 0.95); }});
      } else {
        along_x(0, L, 0.1, 0.95, type == 54);
      }
      break;
    }
    case 91: case 92: case 147: case 148: case 93: case 149: case 150: case 94: case 95: case 96:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{w / 2, h / 2 - a[0] * h / 2}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv((h / 2 - p.y) / (h / 2), 0.05, 0.95); }});
      break;
    case 103: case 104:
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{0, a[0] * h * 2}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = clampv(p.y / h / 2, 0, 0.2); }});
      break;
    case 105: case 106: case 107: case 108: case 109: case 110: case 111: case 113: case 114: case 115: case 117: case 118: case 119: case 121: case 122:
    case 123:
      // the pointer's tip (the leader's end), anywhere
      v.push_back({[=](const std::vector<double>& a) { return ShapePt{a[0] * w, a[1] * h}; },
                   [=](ShapePt p, std::vector<double>& a) { a[0] = p.x / std::max(1e-9, w), a[1] = p.y / std::max(1e-9, h); }});
      break;
    default: break;
  }
  return v;
}

}  // namespace

std::vector<ShapePt> AutoShapeHandles(int type, double w, double h, const std::vector<double>& adj) {
  std::vector<double> a = AutoShapeAdjustDefaults(type);
  for (size_t i = 0; i < a.size() && i < adj.size(); ++i) a[i] = adj[i];
  std::vector<ShapePt> out;
  for (const auto& d : AdjHandles(type, w, h)) out.push_back(d.pos(a));
  return out;
}

bool AutoShapeDragHandle(int type, size_t handle, double w, double h, ShapePt p, std::vector<double>* adj) {
  const auto hs = AdjHandles(type, w, h);
  if (handle >= hs.size()) return false;
  std::vector<double> a = AutoShapeAdjustDefaults(type);
  for (size_t i = 0; i < a.size() && i < adj->size(); ++i) a[i] = (*adj)[i];
  hs[handle].drag(p, a);
  *adj = a;
  return true;
}

const std::vector<ShapeGalleryEntry>& ShapeGallery() {
  static const std::vector<ShapeGalleryEntry> kAll = {
      // Lines
      {kLine, "Line", TR("Line"), TR("Lines")}, {kLineArrow, "Line Arrow", TR("Line Arrow"), TR("Lines")}, {kLineDoubleArrow, "Line Arrow: Double", TR("Line Arrow: Double"), TR("Lines")},
      {kElbow, "Connector: Elbow", TR("Connector: Elbow"), TR("Lines")}, {kElbowArrow, "Connector: Elbow Arrow", TR("Connector: Elbow Arrow"), TR("Lines")}, {kElbowDoubleArrow, "Connector: Elbow Double-Arrow", TR("Connector: Elbow Double-Arrow"), TR("Lines")},
      {kCurved, "Connector: Curved", TR("Connector: Curved"), TR("Lines")}, {kCurvedArrow, "Connector: Curved Arrow", TR("Connector: Curved Arrow"), TR("Lines")}, {kCurvedDoubleArrow, "Connector: Curved Double-Arrow", TR("Connector: Curved Double-Arrow"), TR("Lines")},
      {kCurve, "Curve", TR("Curve"), TR("Lines")}, {kFreeform, "Freeform: Shape", TR("Freeform: Shape"), TR("Lines")}, {kScribble, "Freeform: Scribble", TR("Freeform: Scribble"), TR("Lines")},
      // Rectangles
      {1, "Rectangle", TR("Rectangle"), TR("Rectangles")}, {5, "Rectangle: Rounded Corners", TR("Rectangle: Rounded Corners"), TR("Rectangles")}, {155, "Rectangle: Single Corner Snipped", TR("Rectangle: Single Corner Snipped"), TR("Rectangles")},
      {156, "Rectangle: Top Corners Snipped", TR("Rectangle: Top Corners Snipped"), TR("Rectangles")}, {157, "Rectangle: Diagonal Corners Snipped", TR("Rectangle: Diagonal Corners Snipped"), TR("Rectangles")},
      {154, "Rectangle: Top Corners One Rounded and One Snipped", TR("Rectangle: Top Corners One Rounded and One Snipped"), TR("Rectangles")}, {151, "Rectangle: Single Corner Rounded", TR("Rectangle: Single Corner Rounded"), TR("Rectangles")},
      {152, "Rectangle: Top Corners Rounded", TR("Rectangle: Top Corners Rounded"), TR("Rectangles")}, {153, "Rectangle: Diagonal Corners Rounded", TR("Rectangle: Diagonal Corners Rounded"), TR("Rectangles")},
      // Basic Shapes (the Text Box is the gallery's first entry; 0 here)
      {0, "Text Box", TR("Text Box"), TR("Basic Shapes")}, {9, "Oval", TR("Oval"), TR("Basic Shapes")}, {7, "Isosceles Triangle", TR("Isosceles Triangle"), TR("Basic Shapes")}, {8, "Right Triangle", TR("Right Triangle"), TR("Basic Shapes")},
      {2, "Parallelogram", TR("Parallelogram"), TR("Basic Shapes")}, {3, "Trapezoid", TR("Trapezoid"), TR("Basic Shapes")}, {4, "Diamond", TR("Diamond"), TR("Basic Shapes")}, {12, "Regular Pentagon", TR("Regular Pentagon"), TR("Basic Shapes")},
      {10, "Hexagon", TR("Hexagon"), TR("Basic Shapes")}, {145, "Heptagon", TR("Heptagon"), TR("Basic Shapes")}, {6, "Octagon", TR("Octagon"), TR("Basic Shapes")}, {144, "Decagon", TR("Decagon"), TR("Basic Shapes")},
      {146, "Dodecagon", TR("Dodecagon"), TR("Basic Shapes")}, {142, "Partial Circle", TR("Partial Circle"), TR("Basic Shapes")}, {161, "Chord", TR("Chord"), TR("Basic Shapes")}, {160, "Teardrop", TR("Teardrop"), TR("Basic Shapes")},
      {158, "Frame", TR("Frame"), TR("Basic Shapes")}, {159, "Half Frame", TR("Half Frame"), TR("Basic Shapes")}, {162, "L-Shape", TR("L-Shape"), TR("Basic Shapes")}, {141, "Diagonal Stripe", TR("Diagonal Stripe"), TR("Basic Shapes")},
      {11, "Cross", TR("Cross"), TR("Basic Shapes")}, {28, "Plaque", TR("Plaque"), TR("Basic Shapes")}, {13, "Cylinder", TR("Cylinder"), TR("Basic Shapes")}, {14, "Cube", TR("Cube"), TR("Basic Shapes")},
      {15, "Bevel", TR("Bevel"), TR("Basic Shapes")}, {18, "Circle: Hollow", TR("Circle: Hollow"), TR("Basic Shapes")}, {19, "\"No\" Symbol", TR("\"No\" Symbol"), TR("Basic Shapes")}, {20, "Block Arc", TR("Block Arc"), TR("Basic Shapes")},
      {16, "Folded Corner", TR("Folded Corner"), TR("Basic Shapes")}, {17, "Smiley Face", TR("Smiley Face"), TR("Basic Shapes")}, {21, "Heart", TR("Heart"), TR("Basic Shapes")}, {22, "Lightning Bolt", TR("Lightning Bolt"), TR("Basic Shapes")},
      {23, "Sun", TR("Sun"), TR("Basic Shapes")}, {24, "Moon", TR("Moon"), TR("Basic Shapes")}, {179, "Cloud", TR("Cloud"), TR("Basic Shapes")}, {25, "Arc", TR("Arc"), TR("Basic Shapes")},
      {26, "Double Bracket", TR("Double Bracket"), TR("Basic Shapes")}, {27, "Double Brace", TR("Double Brace"), TR("Basic Shapes")}, {29, "Left Bracket", TR("Left Bracket"), TR("Basic Shapes")}, {30, "Right Bracket", TR("Right Bracket"), TR("Basic Shapes")},
      {31, "Left Brace", TR("Left Brace"), TR("Basic Shapes")}, {32, "Right Brace", TR("Right Brace"), TR("Basic Shapes")},
      // Block Arrows
      {33, "Arrow: Right", TR("Arrow: Right"), TR("Block Arrows")}, {34, "Arrow: Left", TR("Arrow: Left"), TR("Block Arrows")}, {35, "Arrow: Up", TR("Arrow: Up"), TR("Block Arrows")}, {36, "Arrow: Down", TR("Arrow: Down"), TR("Block Arrows")},
      {37, "Arrow: Left-Right", TR("Arrow: Left-Right"), TR("Block Arrows")}, {38, "Arrow: Up-Down", TR("Arrow: Up-Down"), TR("Block Arrows")}, {39, "Arrow: Quad", TR("Arrow: Quad"), TR("Block Arrows")},
      {40, "Arrow: Left-Right-Up", TR("Arrow: Left-Right-Up"), TR("Block Arrows")}, {41, "Arrow: Bent", TR("Arrow: Bent"), TR("Block Arrows")}, {42, "Arrow: U-Turn", TR("Arrow: U-Turn"), TR("Block Arrows")},
      {43, "Arrow: Left-Up", TR("Arrow: Left-Up"), TR("Block Arrows")}, {44, "Arrow: Bent-Up", TR("Arrow: Bent-Up"), TR("Block Arrows")}, {45, "Arrow: Curved Right", TR("Arrow: Curved Right"), TR("Block Arrows")},
      {46, "Arrow: Curved Left", TR("Arrow: Curved Left"), TR("Block Arrows")}, {47, "Arrow: Curved Up", TR("Arrow: Curved Up"), TR("Block Arrows")}, {48, "Arrow: Curved Down", TR("Arrow: Curved Down"), TR("Block Arrows")},
      {49, "Arrow: Striped Right", TR("Arrow: Striped Right"), TR("Block Arrows")}, {50, "Arrow: Notched Right", TR("Arrow: Notched Right"), TR("Block Arrows")}, {51, "Arrow: Pentagon", TR("Arrow: Pentagon"), TR("Block Arrows")},
      {52, "Arrow: Chevron", TR("Arrow: Chevron"), TR("Block Arrows")}, {53, "Callout: Right Arrow", TR("Callout: Right Arrow"), TR("Block Arrows")}, {56, "Callout: Down Arrow", TR("Callout: Down Arrow"), TR("Block Arrows")},
      {54, "Callout: Left Arrow", TR("Callout: Left Arrow"), TR("Block Arrows")}, {55, "Callout: Up Arrow", TR("Callout: Up Arrow"), TR("Block Arrows")}, {57, "Callout: Left-Right Arrow", TR("Callout: Left-Right Arrow"), TR("Block Arrows")},
      {59, "Callout: Quad Arrow", TR("Callout: Quad Arrow"), TR("Block Arrows")}, {60, "Arrow: Circular", TR("Arrow: Circular"), TR("Block Arrows")},
      // Equation Shapes
      {163, "Plus Sign", TR("Plus Sign"), TR("Equation Shapes")}, {164, "Minus Sign", TR("Minus Sign"), TR("Equation Shapes")}, {165, "Multiplication Sign", TR("Multiplication Sign"), TR("Equation Shapes")},
      {166, "Division Sign", TR("Division Sign"), TR("Equation Shapes")}, {167, "Equals", TR("Equals"), TR("Equation Shapes")}, {168, "Not Equal", TR("Not Equal"), TR("Equation Shapes")},
      // Flowchart
      {61, "Flowchart: Process", TR("Flowchart: Process"), TR("Flowchart")}, {62, "Flowchart: Alternate Process", TR("Flowchart: Alternate Process"), TR("Flowchart")}, {63, "Flowchart: Decision", TR("Flowchart: Decision"), TR("Flowchart")},
      {64, "Flowchart: Data", TR("Flowchart: Data"), TR("Flowchart")}, {65, "Flowchart: Predefined Process", TR("Flowchart: Predefined Process"), TR("Flowchart")}, {66, "Flowchart: Internal Storage", TR("Flowchart: Internal Storage"), TR("Flowchart")},
      {67, "Flowchart: Document", TR("Flowchart: Document"), TR("Flowchart")}, {68, "Flowchart: Multidocument", TR("Flowchart: Multidocument"), TR("Flowchart")}, {69, "Flowchart: Terminator", TR("Flowchart: Terminator"), TR("Flowchart")},
      {70, "Flowchart: Preparation", TR("Flowchart: Preparation"), TR("Flowchart")}, {71, "Flowchart: Manual Input", TR("Flowchart: Manual Input"), TR("Flowchart")}, {72, "Flowchart: Manual Operation", TR("Flowchart: Manual Operation"), TR("Flowchart")},
      {73, "Flowchart: Connector", TR("Flowchart: Connector"), TR("Flowchart")}, {74, "Flowchart: Off-page Connector", TR("Flowchart: Off-page Connector"), TR("Flowchart")}, {75, "Flowchart: Card", TR("Flowchart: Card"), TR("Flowchart")},
      {76, "Flowchart: Punched Tape", TR("Flowchart: Punched Tape"), TR("Flowchart")}, {77, "Flowchart: Summing Junction", TR("Flowchart: Summing Junction"), TR("Flowchart")}, {78, "Flowchart: Or", TR("Flowchart: Or"), TR("Flowchart")},
      {79, "Flowchart: Collate", TR("Flowchart: Collate"), TR("Flowchart")}, {80, "Flowchart: Sort", TR("Flowchart: Sort"), TR("Flowchart")}, {81, "Flowchart: Extract", TR("Flowchart: Extract"), TR("Flowchart")},
      {82, "Flowchart: Merge", TR("Flowchart: Merge"), TR("Flowchart")}, {83, "Flowchart: Stored Data", TR("Flowchart: Stored Data"), TR("Flowchart")}, {84, "Flowchart: Delay", TR("Flowchart: Delay"), TR("Flowchart")},
      {85, "Flowchart: Sequential Access Storage", TR("Flowchart: Sequential Access Storage"), TR("Flowchart")}, {86, "Flowchart: Magnetic Disk", TR("Flowchart: Magnetic Disk"), TR("Flowchart")},
      {87, "Flowchart: Direct Access Storage", TR("Flowchart: Direct Access Storage"), TR("Flowchart")}, {88, "Flowchart: Display", TR("Flowchart: Display"), TR("Flowchart")},
      // Stars and Banners
      {89, "Explosion: 8 Points", TR("Explosion: 8 Points"), TR("Stars and Banners")}, {90, "Explosion: 14 Points", TR("Explosion: 14 Points"), TR("Stars and Banners")}, {91, "Star: 4 Points", TR("Star: 4 Points"), TR("Stars and Banners")},
      {92, "Star: 5 Points", TR("Star: 5 Points"), TR("Stars and Banners")}, {147, "Star: 6 Points", TR("Star: 6 Points"), TR("Stars and Banners")}, {148, "Star: 7 Points", TR("Star: 7 Points"), TR("Stars and Banners")},
      {93, "Star: 8 Points", TR("Star: 8 Points"), TR("Stars and Banners")}, {149, "Star: 10 Points", TR("Star: 10 Points"), TR("Stars and Banners")}, {150, "Star: 12 Points", TR("Star: 12 Points"), TR("Stars and Banners")},
      {94, "Star: 16 Points", TR("Star: 16 Points"), TR("Stars and Banners")}, {95, "Star: 24 Points", TR("Star: 24 Points"), TR("Stars and Banners")}, {96, "Star: 32 Points", TR("Star: 32 Points"), TR("Stars and Banners")},
      {99, "Ribbon: Curved and Tilted Up", TR("Ribbon: Curved and Tilted Up"), TR("Stars and Banners")}, {100, "Ribbon: Curved and Tilted Down", TR("Ribbon: Curved and Tilted Down"), TR("Stars and Banners")},
      {97, "Ribbon: Tilted Up", TR("Ribbon: Tilted Up"), TR("Stars and Banners")}, {98, "Ribbon: Tilted Down", TR("Ribbon: Tilted Down"), TR("Stars and Banners")}, {101, "Scroll: Vertical", TR("Scroll: Vertical"), TR("Stars and Banners")},
      {102, "Scroll: Horizontal", TR("Scroll: Horizontal"), TR("Stars and Banners")}, {103, "Wave", TR("Wave"), TR("Stars and Banners")}, {104, "Double Wave", TR("Double Wave"), TR("Stars and Banners")},
      // Callouts
      {105, "Speech Bubble: Rectangle", TR("Speech Bubble: Rectangle"), TR("Callouts")}, {106, "Speech Bubble: Rectangle with Corners Rounded", TR("Speech Bubble: Rectangle with Corners Rounded"), TR("Callouts")},
      {107, "Speech Bubble: Oval", TR("Speech Bubble: Oval"), TR("Callouts")}, {108, "Thought Bubble: Cloud", TR("Thought Bubble: Cloud"), TR("Callouts")}, {109, "Callout: Line", TR("Callout: Line"), TR("Callouts")},
      {110, "Callout: Bent Line", TR("Callout: Bent Line"), TR("Callouts")}, {111, "Callout: Double Bent Line", TR("Callout: Double Bent Line"), TR("Callouts")}, {113, "Callout: Line with Accent Bar", TR("Callout: Line with Accent Bar"), TR("Callouts")},
      {114, "Callout: Bent Line with Accent Bar", TR("Callout: Bent Line with Accent Bar"), TR("Callouts")}, {115, "Callout: Double Bent Line with Accent Bar", TR("Callout: Double Bent Line with Accent Bar"), TR("Callouts")},
      {117, "Callout: Line with No Border", TR("Callout: Line with No Border"), TR("Callouts")}, {118, "Callout: Bent Line with No Border", TR("Callout: Bent Line with No Border"), TR("Callouts")},
      {119, "Callout: Double Bent Line with No Border", TR("Callout: Double Bent Line with No Border"), TR("Callouts")}, {121, "Callout: Line with Border and Accent Bar", TR("Callout: Line with Border and Accent Bar"), TR("Callouts")},
      {122, "Callout: Bent Line with Border and Accent Bar", TR("Callout: Bent Line with Border and Accent Bar"), TR("Callouts")}, {123, "Callout: Double Bent Line with Border and Accent Bar", TR("Callout: Double Bent Line with Border and Accent Bar"), TR("Callouts")},
  };
  return kAll;
}

const char* AutoShapeName(int type) {
  if (type <= 0) return nullptr;
  for (const auto& e : ShapeGallery()) {
    if (e.type == type) return e.name;
  }
  return nullptr;
}

std::vector<ShapePt> ConnectorPath(int connector, ShapePt a, ShapePt b) {
  const double mx = (a.x + b.x) / 2;
  if (connector == 2) return {a, {mx, a.y}, {mx, b.y}, b};
  if (connector == 3) {
    Pts v;
    const int n = 24;
    for (int i = 0; i <= n; ++i) {
      const double t = static_cast<double>(i) / n, u = 1 - t;
      // a cubic Bezier leaving and entering horizontally
      v.push_back({u * u * u * a.x + 3 * u * u * t * mx + 3 * u * t * t * mx + t * t * t * b.x, u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * b.y + t * t * t * b.y});
    }
    return v;
  }
  return {a, b};
}

std::vector<ShapePt> FreeformPath(const std::vector<double>& pts, bool smooth, double w, double h) {
  Pts p;
  for (size_t i = 0; i + 1 < pts.size(); i += 2) p.push_back({pts[i] * w, pts[i + 1] * h});
  if (!smooth || p.size() < 3) return p;
  // a Catmull-Rom curve through the points
  Pts v;
  for (size_t i = 0; i + 1 < p.size(); ++i) {
    const ShapePt& p0 = p[i ? i - 1 : 0];
    const ShapePt& p1 = p[i];
    const ShapePt& p2 = p[i + 1];
    const ShapePt& p3 = p[std::min(i + 2, p.size() - 1)];
    for (int k = 0; k < 8; ++k) {
      const double t = k / 8.0, t2 = t * t, t3 = t2 * t;
      v.push_back({0.5 * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3),
                   0.5 * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3)});
    }
  }
  v.push_back(p.back());
  return v;
}

bool ShapeObjGeometry(const model::ShapeObj& sh, double w, double h, ShapeGeom* g) {
  if (!sh.path.empty()) {
    g->fills.clear();
    g->strokes.clear();
    const Pts v = FreeformPath(sh.path, sh.path_smooth, w, h);
    if (v.size() < 2) return false;
    if (sh.path_closed) g->fills.push_back({v, 0});
    g->strokes.push_back({v, sh.path_closed});
    return true;
  }
  return sh.auto_type > 0 && AutoShapeGeometry(sh.auto_type, w, h, g, sh.adj.empty() ? nullptr : &sh.adj);
}

bool InShapeGeometry(const ShapeGeom& g, ShapePt p) {
  for (const auto& f : g.fills) {
    bool in = false;
    const size_t n = f.pts.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
      const ShapePt& a = f.pts[i];
      const ShapePt& b = f.pts[j];
      if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    if (in) return true;
  }
  return false;
}

}  // namespace yc::engine
