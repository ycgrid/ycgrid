// SPDX-License-Identifier: Apache-2.0
// table_style.cpp - Excel's built-in table styles, the workbook's own, and how a style looks on a cell
#include "model/table_style.h"

#include "model/table.h"
#include "model/theme.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace yc::model {

bool TableStyleElement::Empty() const {
  const BorderSide none{};
  return !fill && !font_color && bold < 0 && italic < 0 && top == none && bottom == none && left == none && right == none && inner_h == none &&
         inner_v == none;
}

bool operator==(const TableStyleElement& a, const TableStyleElement& b) {
  return a.fill == b.fill && a.font_color == b.font_color && a.bold == b.bold && a.italic == b.italic && a.top == b.top && a.bottom == b.bottom &&
         a.left == b.left && a.right == b.right && a.inner_h == b.inner_h && a.inner_v == b.inner_v;
}

namespace {

// the style's color k: 0 Text 1 (black), 1-6 Accent 1-6
uint32_t Tc(int k, double tint = 0) { return MakeThemeColor(k == 0 ? kDark1 : kAccent1 + k - 1, tint); }
uint32_t White() { return MakeThemeColor(kLight1, 0); }
BorderSide Line(uint32_t color, LineStyle ls = LineStyle::Thin) { return BorderSide{ls, color}; }

TableStyleDef Make(const char* family, int n, int group) {
  TableStyleDef d;
  d.name = std::string("TableStyle") + family + std::to_string(n);
  d.builtin = true;
  d.group = group;
  return d;
}

void Outline(TableStyleElement& e, BorderSide b) { e.top = e.bottom = e.left = e.right = b; }

std::vector<TableStyleDef> BuildBuiltins() {
  std::vector<TableStyleDef> v;
  // ---- Light 1-21 ----
  for (int n = 1; n <= 21; ++n) {
    TableStyleDef d = Make("Light", n, 1);
    const int k = (n - 1) % 7;
    const double band = k == 0 ? 0.85 : 0.8;  // black's lightest shade is 85% lighter
    auto& W = d.el[kTeWholeTable];
    auto& H = d.el[kTeHeaderRow];
    auto& T = d.el[kTeTotalRow];
    if (n <= 7) {
      // lines above and below, the text in a darker shade of the color, light stripes
      W.top = W.bottom = Line(Tc(k));
      if (k) W.font_color = Tc(k, -0.25);
      H.bold = 1;
      H.bottom = Line(Tc(k));
      d.el[kTeFirstRowStripe].fill = Tc(k, band);
      d.el[kTeFirstColumnStripe].fill = Tc(k, band);
      T.bold = 1;
      T.top = Line(Tc(k), LineStyle::Double);
    } else if (n <= 14) {
      // a filled header, an outline, lines around the stripes
      Outline(W, Line(Tc(k)));
      H.fill = Tc(k);
      H.font_color = White();
      H.bold = 1;
      d.el[kTeFirstRowStripe].top = d.el[kTeFirstRowStripe].bottom = Line(Tc(k));
      d.el[kTeFirstColumnStripe].left = d.el[kTeFirstColumnStripe].right = Line(Tc(k));
      T.bold = 1;
      T.top = Line(Tc(k), LineStyle::Double);
    } else {
      // every line, a heavier line under the header, light stripes
      Outline(W, Line(Tc(k)));
      W.inner_h = W.inner_v = Line(Tc(k));
      H.bold = 1;
      H.bottom = Line(Tc(k), LineStyle::Medium);
      d.el[kTeFirstRowStripe].fill = Tc(k, band);
      d.el[kTeFirstColumnStripe].fill = Tc(k, band);
      T.bold = 1;
      T.top = Line(Tc(k), LineStyle::Double);
    }
    d.el[kTeFirstColumn].bold = d.el[kTeLastColumn].bold = 1;
    v.push_back(d);
  }
  // ---- Medium 1-28 ----
  for (int n = 1; n <= 28; ++n) {
    TableStyleDef d = Make("Medium", n, 2);
    const int k = (n - 1) % 7;
    const double band = k == 0 ? 0.85 : 0.8, light = k == 0 ? 0.5 : 0.4, mid = k == 0 ? 0.65 : 0.6;
    auto& W = d.el[kTeWholeTable];
    auto& H = d.el[kTeHeaderRow];
    auto& T = d.el[kTeTotalRow];
    if (n <= 7) {
      // Medium 2 is Excel's default: a filled header, light lines between the rows, light stripes
      Outline(W, Line(Tc(k, light)));
      W.inner_h = Line(Tc(k, light));
      H.fill = Tc(k);
      H.font_color = White();
      H.bold = 1;
      d.el[kTeFirstRowStripe].fill = Tc(k, band);
      d.el[kTeFirstColumnStripe].fill = Tc(k, band);
      T.bold = 1;
      T.top = Line(Tc(k), LineStyle::Double);
      d.el[kTeFirstColumn].bold = d.el[kTeLastColumn].bold = 1;
    } else if (n <= 14) {
      // shaded body with white lines; header, total row and first / last column filled with the color
      W.fill = Tc(k, band);
      Outline(W, Line(White()));
      W.inner_h = W.inner_v = Line(White());
      H.fill = Tc(k);
      H.font_color = White();
      H.bold = 1;
      H.bottom = Line(White(), LineStyle::Thick);
      d.el[kTeFirstRowStripe].fill = Tc(k, mid);
      d.el[kTeFirstColumnStripe].fill = Tc(k, mid);
      T.fill = Tc(k);
      T.font_color = White();
      T.bold = 1;
      T.top = Line(White(), LineStyle::Thick);
      for (int e : {kTeFirstColumn, kTeLastColumn}) {
        d.el[e].fill = Tc(k);
        d.el[e].font_color = White();
        d.el[e].bold = 1;
      }
    } else if (n <= 21) {
      // a filled header over gray stripes, black lines above and below
      W.top = W.bottom = Line(Tc(0));
      H.fill = Tc(k);
      H.font_color = White();
      H.bold = 1;
      H.bottom = Line(Tc(0));
      d.el[kTeFirstRowStripe].fill = Tc(0, 0.85);
      d.el[kTeFirstColumnStripe].fill = Tc(0, 0.85);
      T.bold = 1;
      T.top = Line(Tc(0), LineStyle::Double);
      d.el[kTeFirstColumn].bold = d.el[kTeLastColumn].bold = 1;
    } else {
      // shaded body with every line in the color, a bold header
      W.fill = Tc(k, band);
      Outline(W, Line(Tc(k, light)));
      W.inner_h = W.inner_v = Line(Tc(k, light));
      H.bold = 1;
      d.el[kTeFirstRowStripe].fill = Tc(k, mid);
      d.el[kTeFirstColumnStripe].fill = Tc(k, mid);
      T.bold = 1;
      T.top = Line(Tc(k), LineStyle::Double);
      d.el[kTeFirstColumn].bold = d.el[kTeLastColumn].bold = 1;
    }
    v.push_back(d);
  }
  // ---- Dark 1-11 ----
  for (int n = 1; n <= 11; ++n) {
    TableStyleDef d = Make("Dark", n, 3);
    auto& W = d.el[kTeWholeTable];
    auto& H = d.el[kTeHeaderRow];
    auto& T = d.el[kTeTotalRow];
    if (n <= 7) {
      // dark body with white text; a black header
      const int k = n - 1;
      W.fill = k == 0 ? Tc(0, 0.35) : Tc(k);
      W.font_color = White();
      H.fill = Tc(0);
      H.font_color = White();
      H.bold = 1;
      H.bottom = Line(White(), LineStyle::Medium);
      d.el[kTeFirstRowStripe].fill = k == 0 ? Tc(0, 0.25) : Tc(k, -0.25);
      d.el[kTeFirstColumnStripe].fill = k == 0 ? Tc(0, 0.25) : Tc(k, -0.25);
      T.fill = k == 0 ? Tc(0, 0.15) : Tc(k, -0.5);
      T.bold = 1;
      T.top = Line(White(), LineStyle::Double);
      for (int e : {kTeFirstColumn, kTeLastColumn}) {
        d.el[e].fill = k == 0 ? Tc(0, 0.15) : Tc(k, -0.5);
        d.el[e].bold = 1;
      }
    } else {
      // two colors: light stripes of the first, the header in the second
      static const int kPairs[4][2] = {{0, 0}, {1, 2}, {3, 4}, {5, 6}};
      const int a = kPairs[n - 8][0], b = kPairs[n - 8][1];
      W.fill = Tc(a, a == 0 ? 0.65 : 0.6);
      H.fill = b == 0 ? Tc(0) : Tc(b);
      H.font_color = White();
      H.bold = 1;
      d.el[kTeFirstRowStripe].fill = Tc(a, a == 0 ? 0.85 : 0.8);
      d.el[kTeFirstColumnStripe].fill = Tc(a, a == 0 ? 0.85 : 0.8);
      T.bold = 1;
      T.top = Line(Tc(0), LineStyle::Double);
      d.el[kTeFirstColumn].bold = d.el[kTeLastColumn].bold = 1;
    }
    v.push_back(d);
  }
  // [YC] the black trading look (Taiwanese trading terminals): black rows, light text, a dark header
  {
    TableStyleDef d;
    d.name = "TableStyleTrading";
    d.builtin = true;
    d.group = 4;
    auto rgb = [](uint32_t x) { return 0xFF000000u | x; };
    auto& W = d.el[kTeWholeTable];
    W.fill = rgb(0x000000);
    W.font_color = rgb(0xE6E6E6);
    W.inner_h = Line(rgb(0x333333));
    auto& H = d.el[kTeHeaderRow];
    H.fill = rgb(0x262626);
    H.font_color = rgb(0xC8C8C8);
    H.bold = 1;
    d.el[kTeFirstRowStripe].fill = rgb(0x0B0B0B);
    d.el[kTeTotalRow].bold = 1;
    d.el[kTeTotalRow].top = Line(rgb(0x262626), LineStyle::Double);
    v.push_back(d);
  }
  return v;
}

// ---- the store value: a msgpack string holding one line per style ----
std::string Hex(uint32_t x) {
  char b[16];
  std::snprintf(b, sizeof b, "%x", x);
  return b;
}

std::string Side(const BorderSide& s) { return Hex(static_cast<uint32_t>(s.style)) + "." + Hex(s.color); }

BorderSide ReadSide(const std::string& t) {
  const size_t dot = t.find('.');
  if (dot == std::string::npos) return {};
  const unsigned long st = std::strtoul(t.substr(0, dot).c_str(), nullptr, 16);
  return BorderSide{static_cast<LineStyle>(std::min<unsigned long>(st, 13)), static_cast<uint32_t>(std::strtoul(t.substr(dot + 1).c_str(), nullptr, 16))};
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  while (true) {
    const size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

}  // namespace

std::string EncodeStoreString(const std::string& s) {
  std::string out;
  out.push_back(static_cast<char>(0xDB));
  const uint32_t n = static_cast<uint32_t>(s.size());
  for (int sh = 24; sh >= 0; sh -= 8) out.push_back(static_cast<char>((n >> sh) & 0xFF));
  return out + s;
}

std::string DefaultTableStyle(const std::map<std::string, std::string>& store) {
  auto it = store.find(kDefaultTableStyleKey);
  if (it == store.end() || it->second.empty()) return "TableStyleMedium2";
  const std::string_view v = it->second;
  const unsigned char h = static_cast<unsigned char>(v[0]);
  size_t skip = (h & 0xE0) == 0xA0 ? 1 : h == 0xD9 ? 2 : h == 0xDA ? 3 : h == 0xDB ? 5 : 0;
  if (!skip || v.size() < skip) return "TableStyleMedium2";
  std::string name(v.substr(skip));
  return name.empty() ? std::string("TableStyleMedium2") : name;
}

const std::vector<TableStyleDef>& BuiltinTableStyles() {
  static const std::vector<TableStyleDef> v = BuildBuiltins();
  return v;
}

const TableStyleDef* BuiltinTableStyle(std::string_view name) {
  for (const auto& d : BuiltinTableStyles()) {
    if (d.name == name) return &d;
  }
  return nullptr;
}

std::string EncodeTableStyles(const std::vector<TableStyleDef>& defs) {
  std::string text;
  for (const auto& d : defs) {
    std::string name = d.name;
    for (char& ch : name) {
      if (ch == '|' || ch == '\n' || ch == ';') ch = ' ';
    }
    text += name + "|" + std::to_string(d.row_stripe) + "|" + std::to_string(d.col_stripe);
    for (const auto& e : d.el) {
      text += "|" + Hex(e.fill) + ";" + Hex(e.font_color) + ";" + std::to_string(e.bold) + ";" + std::to_string(e.italic) + ";" + Side(e.top) + ";" +
              Side(e.bottom) + ";" + Side(e.left) + ";" + Side(e.right) + ";" + Side(e.inner_h) + ";" + Side(e.inner_v);
    }
    text += "\n";
  }
  // a msgpack str (str32), so the value reads like any other store value
  std::string out;
  out.push_back(static_cast<char>(0xDB));
  const uint32_t n = static_cast<uint32_t>(text.size());
  for (int sh = 24; sh >= 0; sh -= 8) out.push_back(static_cast<char>((n >> sh) & 0xFF));
  return out + text;
}

std::vector<TableStyleDef> DecodeTableStyles(std::string_view stored) {
  std::vector<TableStyleDef> out;
  if (stored.empty()) return out;
  // the msgpack str header (fixstr, str8, str16 or str32)
  const unsigned char h = static_cast<unsigned char>(stored[0]);
  size_t skip = 0;
  if ((h & 0xE0) == 0xA0) skip = 1;
  else if (h == 0xD9) skip = 2;
  else if (h == 0xDA) skip = 3;
  else if (h == 0xDB) skip = 5;
  else return out;
  if (stored.size() < skip) return out;
  const std::string text(stored.substr(skip));
  for (const std::string& line : Split(text, '\n')) {
    if (line.empty()) continue;
    const auto parts = Split(line, '|');
    if (parts.size() < 3 + kTeCount) continue;
    TableStyleDef d;
    d.name = parts[0];
    d.row_stripe = std::clamp(std::atoi(parts[1].c_str()), 1, 9);
    d.col_stripe = std::clamp(std::atoi(parts[2].c_str()), 1, 9);
    for (int e = 0; e < kTeCount; ++e) {
      const auto f = Split(parts[3 + static_cast<size_t>(e)], ';');
      if (f.size() < 10) continue;
      TableStyleElement& x = d.el[e];
      x.fill = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 16));
      x.font_color = static_cast<uint32_t>(std::strtoul(f[1].c_str(), nullptr, 16));
      x.bold = static_cast<int8_t>(std::clamp(std::atoi(f[2].c_str()), -1, 1));
      x.italic = static_cast<int8_t>(std::clamp(std::atoi(f[3].c_str()), -1, 1));
      x.top = ReadSide(f[4]);
      x.bottom = ReadSide(f[5]);
      x.left = ReadSide(f[6]);
      x.right = ReadSide(f[7]);
      x.inner_h = ReadSide(f[8]);
      x.inner_v = ReadSide(f[9]);
    }
    out.push_back(std::move(d));
  }
  return out;
}

const TableStyleDef* FindTableStyle(const std::map<std::string, std::string>& store, std::string_view name) {
  if (name.empty() || name == "TableStyleNone") return nullptr;
  auto it = store.find(kTableStylesKey);
  if (it != store.end()) {
    // decoded once per value (per thread: the renderer and the engine each keep their own)
    thread_local std::string seen;
    thread_local std::vector<TableStyleDef> defs;
    if (seen != it->second) {
      seen = it->second;
      defs = DecodeTableStyles(seen);
    }
    for (const auto& d : defs) {
      if (d.name == name) return &d;
    }
  }
  if (const TableStyleDef* d = BuiltinTableStyle(name)) return d;
  return BuiltinTableStyle("TableStyleMedium2");  // an unknown name looks like the default
}

std::string TableStyleLabel(const TableStyleDef& d) {
  if (!d.builtin) return d.name;
  if (d.name == "TableStyleTrading") return "Trading (black)";
  static const char* const kColors[7] = {"Black", "Blue", "Orange", "Gray", "Gold", "Light Blue", "Green"};
  std::string family = d.group == 1 ? "Light" : d.group == 2 ? "Medium" : "Dark";
  const int n = std::atoi(d.name.c_str() + 10 + family.size());
  int k = (n - 1) % 7;
  if (d.group == 3) k = n <= 7 ? n - 1 : (n == 8 ? 0 : 2 * (n - 8) - 1);
  return std::string(kColors[k]) + ", Table Style " + family + " " + std::to_string(n);
}

void ApplyTableStyle(const Table& t, const TableStyleDef& d, uint32_t r, uint32_t c, Style* s) {
  const core::Rect all = t.range, data = t.Data();
  const bool in_header = t.header && r == all.r0, in_total = t.totals && r == all.r1, in_data = r >= data.r0 && r <= data.r1;
  Style look;  // what the elements set, in Excel's order (later ones win)
  bool has_fill = false, has_font = false;
  int bold = -1, italic = -1;
  BorderSide edge[4];  // left, top, right, bottom
  auto apply = [&](const TableStyleElement& e, const core::Rect& area) {
    if (e.fill) look.fill_color = e.fill, has_fill = true;
    if (e.font_color) look.font_color = e.font_color, has_font = true;
    if (e.bold >= 0) bold = e.bold;
    if (e.italic >= 0) italic = e.italic;
    auto put = [&](int side, const BorderSide& b) {
      if (b.style != LineStyle::None) edge[side] = b;
    };
    put(1, r == area.r0 ? e.top : e.inner_h);
    put(3, r == area.r1 ? e.bottom : e.inner_h);
    put(0, c == area.c0 ? e.left : e.inner_v);
    put(2, c == area.c1 ? e.right : e.inner_v);
  };
  apply(d.el[kTeWholeTable], all);
  if (in_data && t.banded_cols) {
    const uint32_t k = (c - all.c0) / static_cast<uint32_t>(std::max(1, d.col_stripe));
    apply(d.el[k % 2 == 0 ? kTeFirstColumnStripe : kTeSecondColumnStripe], core::Rect{data.r0, c, data.r1, c});
  }
  if (in_data && t.banded_rows) {
    const uint32_t k = (r - data.r0) / static_cast<uint32_t>(std::max(1, d.row_stripe));
    apply(d.el[k % 2 == 0 ? kTeFirstRowStripe : kTeSecondRowStripe], core::Rect{r, all.c0, r, all.c1});
  }
  if (t.last_col && c == all.c1) apply(d.el[kTeLastColumn], core::Rect{all.r0, c, all.r1, c});
  if (t.first_col && c == all.c0) apply(d.el[kTeFirstColumn], core::Rect{all.r0, c, all.r1, c});
  if (in_header) apply(d.el[kTeHeaderRow], core::Rect{r, all.c0, r, all.c1});
  if (in_total) apply(d.el[kTeTotalRow], core::Rect{r, all.c0, r, all.c1});
  // the cell's own formats win
  if (has_fill && !s->HasFill()) s->fill_color = look.fill_color;
  if (has_font && !(s->font_color >> 24)) s->font_color = look.font_color;
  if (bold == 1) s->bold = true;
  if (italic == 1) s->italic = true;
  static const int kEdge[4] = {kLeft, kTop, kRight, kBottom};
  for (int k = 0; k < 4; ++k) {
    if (edge[k].style != LineStyle::None && s->border[kEdge[k]].style == LineStyle::None) s->border[kEdge[k]] = edge[k];
  }
}

}  // namespace yc::model
