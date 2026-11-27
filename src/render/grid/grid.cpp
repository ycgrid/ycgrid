// SPDX-License-Identifier: Apache-2.0
// grid.cpp - the grid: drawing (panes, formats, borders, merged cells, overflowing text) and input (selection, editing, scrolling)
#include "grid/grid.h"
#include "core/i18n.h"

#include "engine/error_check.h"
#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "engine/sparkline.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "formula/functions.h"
#include "formula/numfmt.h"
#include "formula/parser.h"
#include "frame/frame_context.h"
#include "yc_ui_items.h"
#include "theme/theme.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <unordered_map>

namespace yc::render {

std::unordered_set<uint32_t>& CfFrozen() {
  static std::unordered_set<uint32_t> s;
  return s;
}


using ui::Dpi;
using ui::F;
using ui::RequestRedrawIn;
using ui::S;
using ui::T;
using ui::Theme;

// a table's style on one of its cells: header, totals row, banded rows / columns, first / last column; the cell's own
// fill and font color win
void ApplyTableLook(const model::Table& t, uint32_t r, uint32_t col, model::Style* s, const std::map<std::string, std::string>* store) {
  static const std::map<std::string, std::string> kNone;
  if (const model::TableStyleDef* d = model::FindTableStyle(store ? *store : kNone, t.style)) model::ApplyTableStyle(t, *d, r, col, s);
  else if (t.header && r == t.range.r0) s->bold = true;  // TableStyleNone: only the bold header
}

namespace {

constexpr float kScrollW = 17.0f;
constexpr float kBottomH = 26.0f;
constexpr float kHeaderH = 20.0f;
constexpr double kRepeatDelay = 0.40;
constexpr double kRepeatInterval = 0.05;
constexpr int kOverflowLookback = 48;  // look left for text cells that may overflow into the visible area

// reference highlight colors (the colored boxes while editing a formula in Excel, to be calibrated)
constexpr ImU32 kRefColors[] = {IM_COL32(0x41, 0x72, 0xC4, 255), IM_COL32(0xC0, 0x3B, 0x3B, 255), IM_COL32(0x7B, 0x3F, 0xA6, 255),
                                IM_COL32(0x2E, 0x8B, 0x57, 255), IM_COL32(0xD3, 0x7A, 0x00, 255), IM_COL32(0x00, 0x89, 0x9A, 255)};

ImU32 ToCol(uint32_t argb) {
  // model color 0xAARRGGBB -> ImGui ABGR (theme colors resolved first)
  argb = model::ResolveColor(argb);
  const uint32_t a = (argb >> 24) & 0xFF, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
  return IM_COL32(r, g, b, a ? a : 255);
}

struct Line {
  int64_t index;
  float pos;
  float size;
};

struct Pane {
  ImRect rect;
  float col_x = 0;  // screen x of first_col (rect.Min.x, except the T layout's left pane, right-aligned)
  int64_t first_row = 0, first_col = 0;
  int64_t row_end = 0, col_end = 0;  // exclusive
  bool scroll_rows = true, scroll_cols = true;
  std::vector<Line> rows, cols;
  bool Valid() const { return !rows.empty() && !cols.empty() && rect.GetWidth() > 0 && rect.GetHeight() > 0; }
};

void Collect(const model::DimIndex& dim, int64_t first, int64_t end, float start, float stop, double sc, std::vector<Line>& out) {
  out.clear();
  // every line from first on hidden (all 1,048,576 rows): nothing to walk through one by one every frame
  if (first >= end || dim.Offset(end) == dim.Offset(first)) return;
  double pos = start;
  for (int64_t i = first; i < end && pos < stop && out.size() < 4096; ++i) {
    const int32_t sz = dim.Size(i);
    if (sz <= 0) continue;
    const double next = pos + sz * sc;
    const float a = static_cast<float>(std::floor(pos + 0.5));
    const float b = static_cast<float>(std::floor(next + 0.5));
    out.push_back({i, a, b - a});
    pos = next;
  }
}

int WheelLines() {
  UINT lines = 3;
  ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  return lines == WHEEL_PAGESCROLL ? 20 : static_cast<int>(lines);
}

void DrawArrow(ImDrawList* dl, ImVec2 c, int dir, float s, ImU32 col) {
  const float w = s, h = s * 0.6f;
  switch (dir) {
    case 0: dl->AddTriangleFilled(ImVec2(c.x - w, c.y + h * 0.5f), ImVec2(c.x + w, c.y + h * 0.5f), ImVec2(c.x, c.y - h * 0.5f), col); break;
    case 1: dl->AddTriangleFilled(ImVec2(c.x - w, c.y - h * 0.5f), ImVec2(c.x + w, c.y - h * 0.5f), ImVec2(c.x, c.y + h * 0.5f), col); break;
    case 2: dl->AddTriangleFilled(ImVec2(c.x + h * 0.5f, c.y - w), ImVec2(c.x + h * 0.5f, c.y + w), ImVec2(c.x - h * 0.5f, c.y), col); break;
    default: dl->AddTriangleFilled(ImVec2(c.x - h * 0.5f, c.y - w), ImVec2(c.x - h * 0.5f, c.y + w), ImVec2(c.x + h * 0.5f, c.y), col); break;
  }
}

void ScrollBy(ScrollAxis& ax, const model::DimIndex& dim, int64_t lo, int64_t limit, int64_t delta) {
  int64_t target = std::clamp<int64_t>(ax.first + delta, lo, limit - 1);
  const int dir = delta >= 0 ? 1 : -1;
  int64_t vis = dim.NextVisible(target, dir, limit);
  if (vis < 0) vis = dim.NextVisible(target, -dir, limit);
  if (vis >= lo) ax.first = vis;
}

std::string ColName(int64_t c) { return core::ColumnName(static_cast<uint32_t>(c)); }

// Excel scroll bars (PLAN §8.2)
void Scrollbar(const char* id, bool vertical, const ImRect& r, ScrollAxis& ax, const model::DimIndex& dim, int64_t lo, int64_t limit, int64_t used,
               int64_t last_visible, int64_t active, float view_len, double sc, int64_t page) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Theme& t = T();
  ui::NoteUiItem(id, r);  // tests see the scroll bar ("##vscroll" / "##hscroll") as a person does (Options can hide it)
  dl->AddRectFilled(r.Min, r.Max, t.scroll_track);
  const float thick = vertical ? r.GetWidth() : r.GetHeight();
  const ImRect a1 = vertical ? ImRect(r.Min, ImVec2(r.Max.x, r.Min.y + thick)) : ImRect(r.Min, ImVec2(r.Min.x + thick, r.Max.y));
  const ImRect a2 = vertical ? ImRect(ImVec2(r.Min.x, r.Max.y - thick), r.Max) : ImRect(ImVec2(r.Max.x - thick, r.Min.y), r.Max);
  const ImRect track = vertical ? ImRect(ImVec2(r.Min.x, a1.Max.y), ImVec2(r.Max.x, a2.Min.y)) : ImRect(ImVec2(a1.Max.x, r.Min.y), ImVec2(a2.Min.x, r.Max.y));
  if (!ax.thumb_drag) ax.extent = std::clamp<int64_t>(std::max({used, last_visible + 1, active + 1}), 1, limit);
  const int64_t base = dim.Offset(lo);
  const int64_t content = dim.Offset(ax.extent) - base;
  const int64_t view_b = static_cast<int64_t>(view_len / sc);
  const int64_t max_top = std::max<int64_t>(0, content - view_b);
  const int64_t top = dim.Offset(ax.first) - base;
  const float track_len = vertical ? track.GetHeight() : track.GetWidth();
  const double frac_view = content > 0 ? std::min(1.0, static_cast<double>(view_b) / static_cast<double>(std::max(content, view_b))) : 1.0;
  const float thumb_len = std::min(track_len, std::max(S(16), static_cast<float>(track_len * frac_view)));
  const double frac_pos = max_top > 0 ? std::clamp(static_cast<double>(top) / static_cast<double>(max_top), 0.0, 1.0) : 0.0;
  const float thumb0 = (vertical ? track.Min.y : track.Min.x) + static_cast<float>((track_len - thumb_len) * frac_pos);
  const ImRect thumb = vertical ? ImRect(ImVec2(track.Min.x + S(3), thumb0), ImVec2(track.Max.x - S(3), thumb0 + thumb_len))
                                : ImRect(ImVec2(thumb0, track.Min.y + S(3)), ImVec2(thumb0 + thumb_len, track.Max.y - S(3)));
  const double now = ImGui::GetTime();
  auto step = [&](int part) {
    switch (part) {
      case 1: ScrollBy(ax, dim, lo, limit, -1); break;
      case 2: ScrollBy(ax, dim, lo, limit, +1); break;
      case 3: ScrollBy(ax, dim, lo, limit, -page); break;
      case 4: ScrollBy(ax, dim, lo, limit, +page); break;
      default: break;
    }
  };
  ImGui::PushID(id);
  for (int i = 0; i < 2; ++i) {
    const ImRect& ar = i == 0 ? a1 : a2;
    ImGui::SetCursorScreenPos(ar.Min);
    ImGui::PushID(i);
    ImGui::InvisibleButton("##arrow", ar.GetSize());
    const int part = i == 0 ? 1 : 2;
    if (ImGui::IsItemActivated()) {
      step(part);
      ax.repeat_part = part;
      ax.repeat_next = now + kRepeatDelay;
    }
    if (ImGui::IsItemActive() && ax.repeat_part == part) {
      if (now >= ax.repeat_next) {
        step(part);
        ax.repeat_next = now + kRepeatInterval;
      }
      RequestRedrawIn(std::max(0.0, ax.repeat_next - now));
    }
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (hov) dl->AddRectFilled(ar.Min, ar.Max, IM_COL32(0xE0, 0xE0, 0xE0, 255));
    DrawArrow(dl, ar.GetCenter(), vertical ? (i == 0 ? 0 : 1) : (i == 0 ? 2 : 3), S(3.5f), t.scroll_arrow);
  }
  ImGui::SetCursorScreenPos(track.Min);
  ImGui::InvisibleButton("##track", ImVec2(std::max(1.0f, track.GetWidth()), std::max(1.0f, track.GetHeight())));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float m = vertical ? mouse.y : mouse.x;
  if (ImGui::IsItemActivated()) {
    if (vertical ? (m >= thumb.Min.y && m < thumb.Max.y) : (m >= thumb.Min.x && m < thumb.Max.x)) {
      ax.thumb_drag = true;
      ax.drag_mouse0 = m;
      ax.drag_px0 = top;
    } else {
      ax.repeat_part = m < (vertical ? thumb.Min.y : thumb.Min.x) ? 3 : 4;
      step(ax.repeat_part);
      ax.repeat_next = now + kRepeatDelay;
    }
  }
  if (ImGui::IsItemActive()) {
    if (ax.thumb_drag) {
      const float usable = std::max(1.0f, track_len - thumb_len);
      const double px = static_cast<double>(ax.drag_px0) + (m - ax.drag_mouse0) * static_cast<double>(max_top) / usable;
      const int64_t clamped = std::clamp<int64_t>(static_cast<int64_t>(px), 0, max_top);
      ax.first = std::clamp<int64_t>(dim.IndexAt(clamped + base), lo, limit - 1);
      char tip[64];
      if (vertical) std::snprintf(tip, sizeof(tip), TR("Row: %lld"), static_cast<long long>(ax.first + 1));
      else std::snprintf(tip, sizeof(tip), TR("Column: %s"), ColName(ax.first).c_str());
      ImGui::SetNextWindowPos(vertical ? ImVec2(r.Min.x - S(8), mouse.y) : ImVec2(mouse.x, r.Min.y - S(8)), ImGuiCond_Always,
                              vertical ? ImVec2(1.0f, 0.5f) : ImVec2(0.5f, 1.0f));
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(tip);
      ImGui::EndTooltip();
    } else if (ax.repeat_part == 3 || ax.repeat_part == 4) {
      const bool reached = ax.repeat_part == 3 ? m >= (vertical ? thumb.Min.y : thumb.Min.x) : m <= (vertical ? thumb.Max.y : thumb.Max.x);
      if (!reached && now >= ax.repeat_next) {
        step(ax.repeat_part);
        ax.repeat_next = now + kRepeatInterval;
      }
      RequestRedrawIn(std::max(0.0, ax.repeat_next - now));
    }
  } else {
    ax.thumb_drag = false;
    if (ax.repeat_part >= 3) ax.repeat_part = 0;
  }
  if (!ImGui::IsAnyItemActive()) ax.repeat_part = 0;
  const bool thumb_hover = ImGui::IsItemHovered() && thumb.Contains(mouse);
  ImGui::PopID();
  dl->AddRectFilled(thumb.Min, thumb.Max, ax.thumb_drag ? t.scroll_thumb_active : (thumb_hover ? t.scroll_thumb_hover : t.scroll_thumb), S(4));
}

// ---- Number format cache (by numfmt string id; rebuilt when the style table changes) ----
struct FmtCache {
  const void* owner = nullptr;
  std::vector<std::shared_ptr<const formula::NumberFormat>> by_id;
};
FmtCache g_fmt;

const formula::NumberFormat& FormatFor(const model::StyleTable& table, uint32_t numfmt) {
  const void* owner = &table;
  if (g_fmt.owner != owner) {
    g_fmt.owner = owner;
    g_fmt.by_id.clear();
  }
  if (numfmt >= g_fmt.by_id.size()) g_fmt.by_id.resize(numfmt + 1);
  auto& p = g_fmt.by_id[numfmt];
  if (!p) p = formula::CompileFormat(numfmt ? table.Str(numfmt) : std::string("General"));
  return *p;
}

std::vector<Line> g_rows_tmp, g_cols_tmp;

// drawing context of one frame
struct Ctx {
  const engine::Snapshot& snap;
  const model::Sheet& sheet;
  const model::StyleTable& styles;
  GridView& view;
  const GridOptions& opt;
  ImDrawList* dl;
  double sc;
  float zoom;
  float pad;
  GridStats& stats;
  std::vector<Rect> merges;  // merged areas intersecting the visible area
  // conditional formatting: per-cell results cached for the frame (shared by the fill and text passes)
  engine::CondFormatEval* cf = nullptr;
  struct CfCell {
    bool has = false;
    engine::CfVisual vis;
    model::Style style;
  };
  std::unordered_map<uint64_t, CfCell> cf_cells;
  std::unordered_map<uint64_t, model::Style> preview_cells;  // styles with the live preview applied (this frame)
};

bool InPreview(const Ctx& c, uint32_t r, uint32_t col) {
  if (c.opt.preview_table && c.opt.preview_table->range.Contains(r, col)) return true;  // a table style under the mouse
  if (!c.opt.preview_style || !c.opt.preview_areas) return false;
  for (const Rect& a : *c.opt.preview_areas) {
    if (a.Contains(r, col)) return true;
  }
  return false;
}

// the cell's own style, or the live-preview version of it
// a sparkline in cell box b: line, column or win / loss of the values (NaN = a gap)
void DrawSparkline(ImDrawList* dl, const ImRect& cell, const model::SparklineGroup& g, const std::vector<double>& v, float scale, double lo_fix = NAN,
                   double hi_fix = NAN, const std::vector<double>* xfrac = nullptr) {
  if (v.empty()) return;
  const float pad = std::max(2.0f, 2.0f * scale);
  const ImRect b(ImVec2(cell.Min.x + pad, cell.Min.y + pad), ImVec2(cell.Max.x - pad, cell.Max.y - pad));
  if (b.GetWidth() < 4 || b.GetHeight() < 3) return;
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
  // the high / low points are the data's; the scale may be the group's or custom (Axis > Vertical Axis options)
  const double dlo = lo, dhi = hi;
  if (!std::isnan(lo_fix)) lo = lo_fix;
  if (!std::isnan(hi_fix)) hi = hi_fix;
  (void)dlo, (void)dhi;
  const bool dated = xfrac && xfrac->size() == n;
  auto xat = [&](size_t i) {
    if (dated && !std::isnan((*xfrac)[i])) return b.Min.x + b.GetWidth() * static_cast<float>((*xfrac)[i]);
    return n > 1 ? b.Min.x + b.GetWidth() * static_cast<float>(i) / static_cast<float>(n - 1) : b.GetCenter().x;
  };
  auto special = [&](size_t i, uint32_t* col) {
    if (g.high && i == ihi) return *col = g.high_color, true;
    if (g.low && i == ilo) return *col = g.low_color, true;
    if (g.first && i == ifirst) return *col = g.first_color, true;
    if (g.last && i == ilast) return *col = g.last_color, true;
    if (g.negative && v[i] < 0) return *col = g.negative_color, true;
    return false;
  };
  if (g.type == 0) {
    const double span = hi > lo ? hi - lo : 1.0;
    auto pt = [&](size_t i) {
      const float x = xat(i);
      const float y = hi > lo ? b.Max.y - static_cast<float>((std::clamp(v[i], lo, hi) - lo) / span) * b.GetHeight() : b.GetCenter().y;
      return ImVec2(x, y);
    };
    const float w = std::max(1.0f, g.line_weight * 4.0f / 3.0f * scale);
    if (g.axis && lo < 0 && hi > 0) {
      const float y0 = b.Max.y - static_cast<float>((0 - lo) / span) * b.GetHeight();
      dl->AddLine(ImVec2(b.Min.x, y0), ImVec2(b.Max.x, y0), ToCol(g.axis_color), 1.0f);
    }
    for (size_t i = 1; i < n; ++i) {
      if (std::isnan(v[i - 1]) || std::isnan(v[i])) continue;
      dl->AddLine(pt(i - 1), pt(i), ToCol(g.color), w);
    }
    const float r = std::max(1.5f, 2.0f * scale);
    for (size_t i = 0; i < n; ++i) {
      if (std::isnan(v[i])) continue;
      uint32_t col = g.marker_color;
      if (special(i, &col) || g.markers) dl->AddCircleFilled(pt(i), r, ToCol(col));
    }
    return;
  }
  // column / win-loss: bars from the zero line
  const float slot = b.GetWidth() / static_cast<float>(n), bw = std::max(1.0f, slot * 0.75f);
  const double top = g.type == 2 ? 1.0 : std::isnan(hi_fix) ? std::max(hi, 0.0) : hi, bottom = g.type == 2 ? -1.0 : std::isnan(lo_fix) ? std::min(lo, 0.0) : lo;
  const double span = top > bottom ? top - bottom : 1.0;
  const float y0 = b.Min.y + static_cast<float>(top / span) * b.GetHeight();
  for (size_t i = 0; i < n; ++i) {
    if (std::isnan(v[i])) continue;
    const double x = g.type == 2 ? (v[i] > 0 ? 1.0 : v[i] < 0 ? -1.0 : 0.0) : v[i];
    if (x == 0 && g.type == 2) continue;
    const float y = b.Min.y + static_cast<float>((top - x) / span) * b.GetHeight();
    uint32_t col = v[i] < 0 && g.type == 2 ? g.negative_color : g.color;
    special(i, &col);
    const float x0 = dated && !std::isnan((*xfrac)[i]) ? b.Min.x + (b.GetWidth() - bw) * static_cast<float>((*xfrac)[i]) : b.Min.x + slot * static_cast<float>(i) + (slot - bw) * 0.5f;
    dl->AddRectFilled(ImVec2(x0, std::clamp(std::min(y, y0), b.Min.y, b.Max.y)), ImVec2(x0 + bw, std::clamp(std::max(y, y0) + (y == y0 ? 1.0f : 0.0f), b.Min.y, b.Max.y)), ToCol(col));
  }
  if (g.axis) dl->AddLine(ImVec2(b.Min.x, y0), ImVec2(b.Max.x, y0), ToCol(g.axis_color), 1.0f);
}

const model::Style& BaseStyle(Ctx& c, uint32_t r, uint32_t col) {
  const model::Style& own = c.styles.Get(c.sheet.GetStyle(r, col));
  const model::Table* table = nullptr;
  if (!c.sheet.tables->empty()) {
    for (const auto& t : *c.sheet.tables) {
      if (t.range.Contains(r, col)) table = &t;
    }
  }
  if (!table && !InPreview(c, r, col)) return own;
  const uint64_t key = (static_cast<uint64_t>(r) << 14) | col;
  auto it = c.preview_cells.find(key);
  if (it == c.preview_cells.end()) {
    model::Style s = own;
    // a table style previewed (Format as Table's gallery): its look on its range, instead of the table's own style
    const bool table_preview = c.opt.preview_table && c.opt.preview_table->range.Contains(r, col);
    if (table_preview) ApplyTableLook(*c.opt.preview_table, r, col, &s, &*c.snap.wb.store);
    else if (table) ApplyTableLook(*table, r, col, &s, &*c.snap.wb.store);
    if (c.opt.preview_style && !table_preview && InPreview(c, r, col)) c.opt.preview_style(s);
    it = c.preview_cells.emplace(key, std::move(s)).first;
  }
  return it->second;
}

// effective style (with conditional formatting); vis returns the conditional-format result (nullptr when none)
// an icon of a conditional-format icon set in box b (index 0 = the lowest icon of the set)
void DrawCfIcon(ImDrawList* dl, const ImRect& b, int style, int index) {
  const int n = model::IconSetCount(style);
  const ImU32 red = IM_COL32(0xE0, 0x3C, 0x31, 255), yellow = IM_COL32(0xF4, 0xB4, 0x00, 255), green = IM_COL32(0x2E, 0x9E, 0x44, 255);
  const ImU32 gray = IM_COL32(0x80, 0x80, 0x80, 255), black = IM_COL32(0x30, 0x30, 0x30, 255), pink = IM_COL32(0xF0, 0x9A, 0x9A, 255);
  const ImU32 blue = IM_COL32(0x2F, 0x6E, 0xC4, 255);
  const ImVec2 c = b.GetCenter();
  const float s = std::min(b.GetWidth(), b.GetHeight()) * 0.5f;
  // color from red (lowest) to green (highest)
  auto ryg = [&](int i) { return n == 3 ? (i == 0 ? red : i == 1 ? yellow : green) : (i == 0 ? red : i == n - 1 ? green : yellow); };
  // arrow pointing at angle (radians, 0 = right, -pi/2 = up)
  auto arrow = [&](float ang, ImU32 col) {
    const ImVec2 d(std::cos(ang), std::sin(ang)), nrm(-d.y, d.x);
    const ImVec2 tip(c.x + d.x * s, c.y + d.y * s), base(c.x - d.x * s, c.y - d.y * s);
    const ImVec2 neck(c.x + d.x * s * 0.05f, c.y + d.y * s * 0.05f);
    dl->AddTriangleFilled(tip, ImVec2(neck.x + nrm.x * s * 0.85f, neck.y + nrm.y * s * 0.85f), ImVec2(neck.x - nrm.x * s * 0.85f, neck.y - nrm.y * s * 0.85f), col);
    dl->AddLine(base, neck, col, std::max(2.0f, s * 0.55f));
  };
  const float pi = 3.14159265f;
  switch (style) {
    case model::kIcon3Arrows: case model::kIcon3ArrowsGray: case model::kIcon4Arrows: case model::kIcon4ArrowsGray: case model::kIcon5Arrows: case model::kIcon5ArrowsGray: {
      const bool g = style == model::kIcon3ArrowsGray || style == model::kIcon4ArrowsGray || style == model::kIcon5ArrowsGray;
      const float ang = n == 3 ? (index == 0 ? pi / 2 : index == 1 ? 0.0f : -pi / 2) : (pi / 2 - pi * static_cast<float>(index) / static_cast<float>(n - 1));
      arrow(ang, g ? gray : ryg(index));
      break;
    }
    case model::kIcon3Flags: {
      const ImU32 col = ryg(index);
      dl->AddLine(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x - s * 0.6f, c.y + s), black, std::max(1.0f, s * 0.15f));
      dl->AddTriangleFilled(ImVec2(c.x - s * 0.55f, c.y - s), ImVec2(c.x + s * 0.9f, c.y - s * 0.45f), ImVec2(c.x - s * 0.55f, c.y + s * 0.1f), col);
      break;
    }
    case model::kIcon3TrafficLights1: case model::kIcon3TrafficLights2: case model::kIcon4TrafficLights: case model::kIcon4RedToBlack: {
      ImU32 col = ryg(index);
      if (style == model::kIcon4TrafficLights) col = index == 0 ? black : index == 1 ? red : index == 2 ? yellow : green;
      if (style == model::kIcon4RedToBlack) col = index == 0 ? black : index == 1 ? gray : index == 2 ? pink : red;
      if (style == model::kIcon3TrafficLights2) dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), black, s * 0.35f);
      dl->AddCircleFilled(c, s * (style == model::kIcon3TrafficLights2 ? 0.72f : 0.95f), col);
      break;
    }
    case model::kIcon3Signs: {
      if (index == 2) dl->AddCircleFilled(c, s * 0.95f, green);
      else if (index == 1) dl->AddTriangleFilled(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s * 0.85f), ImVec2(c.x - s, c.y + s * 0.85f), yellow);
      else dl->AddQuadFilled(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y), ImVec2(c.x, c.y + s), ImVec2(c.x - s, c.y), red);
      break;
    }
    case model::kIcon3Symbols: case model::kIcon3Symbols2: {
      const bool circled = style == model::kIcon3Symbols;
      const ImU32 col = ryg(index), mark = circled ? IM_COL32_WHITE : col;
      if (circled) dl->AddCircleFilled(c, s * 0.95f, col);
      const float w = std::max(1.5f, s * 0.28f), k = s * 0.45f;
      if (index == 2) {
        dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x - k * 0.2f, c.y + k * 0.8f), mark, w);
        dl->AddLine(ImVec2(c.x - k * 0.2f, c.y + k * 0.8f), ImVec2(c.x + k, c.y - k * 0.8f), mark, w);
      } else if (index == 1) {
        dl->AddLine(ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k * 0.3f), mark, w);
        dl->AddCircleFilled(ImVec2(c.x, c.y + k * 0.85f), w * 0.6f, mark);
      } else {
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), mark, w);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), mark, w);
      }
      break;
    }
    case model::kIcon4Ratings: case model::kIcon5Ratings: {
      // signal bars: index + 1 of n bars filled
      const int bars = n - 1;
      const float bw = s * 2 / static_cast<float>(bars * 2 - 1);
      for (int i = 0; i < bars; ++i) {
        const float x0 = c.x - s + bw * 2 * static_cast<float>(i), h = s * 2 * static_cast<float>(i + 1) / static_cast<float>(bars);
        dl->AddRectFilled(ImVec2(x0, c.y + s - h), ImVec2(x0 + bw, c.y + s), i < index ? blue : IM_COL32(0xC8, 0xC8, 0xC8, 255));
      }
      break;
    }
    case model::kIcon5Quarters: {
      dl->AddCircle(c, s * 0.9f, black, 0, std::max(1.0f, s * 0.12f));
      if (index > 0) {
        dl->PathLineTo(c);
        dl->PathArcTo(c, s * 0.9f, -pi / 2, -pi / 2 + 2 * pi * static_cast<float>(index) / 4.0f, 24);
        dl->PathFillConvex(black);
      }
      break;
    }
    case model::kIcon3Stars: {
      ImVec2 pts[10];
      for (int i = 0; i < 10; ++i) {
        const float a = -pi / 2 + pi * static_cast<float>(i) / 5.0f, rr = i % 2 ? s * 0.42f : s;
        pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
      }
      const ImU32 gold = IM_COL32(0xF4, 0xB4, 0x00, 255);
      if (index > 0) {
        if (index == 1) dl->PushClipRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x, c.y + s), true);
        dl->AddConcavePolyFilled(pts, 10, gold);
        if (index == 1) dl->PopClipRect();
      }
      dl->AddPolyline(pts, 10, gold, std::max(1.0f, s * 0.1f), ImDrawFlags_Closed);
      break;
    }
    case model::kIcon3Triangles: {
      if (index == 2) dl->AddTriangleFilled(ImVec2(c.x, c.y - s * 0.8f), ImVec2(c.x + s * 0.8f, c.y + s * 0.5f), ImVec2(c.x - s * 0.8f, c.y + s * 0.5f), green);
      else if (index == 1) dl->AddRectFilled(ImVec2(c.x - s * 0.7f, c.y - s * 0.18f), ImVec2(c.x + s * 0.7f, c.y + s * 0.18f), yellow);
      else dl->AddTriangleFilled(ImVec2(c.x - s * 0.8f, c.y - s * 0.5f), ImVec2(c.x + s * 0.8f, c.y - s * 0.5f), ImVec2(c.x, c.y + s * 0.8f), red);
      break;
    }
    case model::kIcon5Boxes: {
      const float q = s * 0.95f;
      for (int i = 0; i < 4; ++i) {
        const float x0 = i % 2 ? c.x + 0.5f : c.x - q, y0 = i / 2 ? c.y + 0.5f : c.y - q;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + q - 0.5f, y0 + q - 0.5f), i < index ? blue : IM_COL32(0xC8, 0xC8, 0xC8, 255));
      }
      break;
    }
    default: dl->AddCircleFilled(c, s * 0.9f, ryg(index)); break;
  }
}

const model::Style& StyleAt(Ctx& c, uint32_t r, uint32_t col, const engine::CfVisual** vis = nullptr) {
  const model::Style& base = BaseStyle(c, r, col);
  if (vis) *vis = nullptr;
  if (!c.cf) return base;
  const uint64_t key = (static_cast<uint64_t>(r) << 14) | col;
  auto it = c.cf_cells.find(key);
  if (it == c.cf_cells.end()) {
    Ctx::CfCell e;
    e.has = c.cf->Eval(r, col, &e.vis);
    if (e.has) {
      e.style = base;
      if (e.vis.has_fill) e.style.fill_color = e.vis.fill;
      if (e.vis.has_font) e.style.font_color = e.vis.font;
      if (e.vis.bold >= 0) e.style.bold = e.vis.bold != 0;
      if (e.vis.italic >= 0) e.style.italic = e.vis.italic != 0;
      if (e.vis.strike >= 0) e.style.strike = e.vis.strike != 0;
      // the rule's Format...: number format (interned by the engine), underline, pattern, lines
      if (!e.vis.numfmt.empty()) e.style.numfmt = e.vis.numfmt == "General" ? 0 : c.styles.FindString(e.vis.numfmt);
      if (e.vis.underline >= 0) e.style.underline = static_cast<model::Underline>(e.vis.underline);
      if (e.vis.pattern >= 0) {
        e.style.pattern = static_cast<model::FillPattern>(e.vis.pattern);
        e.style.pattern_color = e.vis.pattern_color;
        e.style.gradient = model::FillGradient::None;
      }
      static const int kSide[4] = {model::kLeft, model::kTop, model::kRight, model::kBottom};
      for (int k = 0; k < 4; ++k) {
        if (e.vis.side_style[k]) e.style.border[kSide[k]] = model::BorderSide{static_cast<model::LineStyle>(e.vis.side_style[k]), e.vis.side_color[k]};
      }
    }
    it = c.cf_cells.emplace(key, std::move(e)).first;
  }
  if (!it->second.has) return base;
  if (vis) *vis = &it->second.vis;
  return it->second.style;
}

// one conditional-format evaluator per sheet and snapshot version (statistics are computed once)
// the evaluator with a previewed rule on top of the sheet's (built again when the rule or the snapshot changes)
engine::CondFormatEval* CfPreviewFor(const engine::Snapshot& snap, const model::Sheet& sheet, const model::CfRule& rule) {
  static uint64_t version = ~0ull;
  static uint32_t sheet_id = ~0u;
  static model::CfRule last;
  static std::unique_ptr<engine::CondFormatEval> eval;
  if (!eval || version != snap.version || sheet_id != sheet.id || !(last == rule)) {
    version = snap.version;
    sheet_id = sheet.id;
    last = rule;
    model::Workbook wb = snap.wb;
    for (auto& sh : wb.sheets) {
      if (sh->id != sheet.id) continue;
      auto& rules = sh.mut().cond_formats.mut();
      for (auto& x : rules) ++x.priority;
      model::CfRule r = rule;
      r.priority = 0;
      rules.insert(rules.begin(), r);
    }
    eval = std::make_unique<engine::CondFormatEval>(std::move(wb), sheet.id);
  }
  return eval.get();
}

engine::CondFormatEval* CfFor(const engine::Snapshot& snap, const model::Sheet& sheet) {
  if (sheet.cond_formats->empty()) return nullptr;
  static std::unordered_map<uint32_t, std::pair<uint64_t, std::unique_ptr<engine::CondFormatEval>>> cache;
  auto& e = cache[sheet.id];
  // Worksheet.EnableFormatConditionsCalculation = False: the formats stay as they were last evaluated
  if (e.second && CfFrozen().count(sheet.id)) return e.second.get();
  if (!e.second || e.first != snap.version) {
    auto next = std::make_unique<engine::CondFormatEval>(snap.wb, sheet.id);
    if (e.second) next->KeepStatsFrom(*e.second);  // large ranges: their statistics a moment longer (see there)
    e.first = snap.version;
    e.second = std::move(next);
  }
  return e.second.get();
}

float FontPxFor(const Ctx& c, const model::Style& st) {
  const float pt = c.styles.FontSize(st);
  return pt * (96.0f / 72.0f) * Dpi() * c.zoom;
}

ImFont* FontFor(const Ctx& c, const model::Style& st) { return ui::CellFont(c.styles.FontName(st), st.bold, st.italic); }

// the fonts inside a cell's text, while they still match its value
const model::RichText* RichFor(const Ctx& c, uint32_t r, uint32_t col, const core::Value& v) {
  if (c.sheet.rich->empty() || !v.IsString()) return nullptr;
  auto it = c.sheet.rich->find((uint64_t{r} << 16) | col);
  return it != c.sheet.rich->end() && it->second.text == v.str() ? &it->second : nullptr;
}

const Rect* MergeContaining(const Ctx& c, uint32_t r, uint32_t col) {
  for (const Rect& m : c.merges) {
    if (m.Contains(r, col)) return &m;
  }
  return nullptr;
}

// coordinates: screen y of row r in the pane (may be outside the visible range)
float RowY(const Ctx& c, const Pane& p, int64_t r) {
  return p.rect.Min.y + static_cast<float>(static_cast<double>(c.sheet.rows->Offset(r) - c.sheet.rows->Offset(p.first_row)) * c.sc);
}
float ColX(const Ctx& c, const Pane& p, int64_t col) {
  return p.col_x + static_cast<float>(static_cast<double>(c.sheet.cols->Offset(col) - c.sheet.cols->Offset(p.first_col)) * c.sc);
}

void DrawBorderLine(ImDrawList* dl, model::LineStyle ls, ImU32 col, ImVec2 a, ImVec2 b, bool horizontal, float s) {
  using model::LineStyle;
  auto rect = [&](float off0, float off1) {
    if (horizontal) dl->AddRectFilled(ImVec2(a.x, a.y + off0), ImVec2(b.x, a.y + off1), col);
    else dl->AddRectFilled(ImVec2(a.x + off0, a.y), ImVec2(a.x + off1, b.y), col);
  };
  auto dashed = [&](float dash, float gap, float w0, float w1) {
    const float len = horizontal ? b.x - a.x : b.y - a.y;
    for (float t = 0; t < len; t += dash + gap) {
      const float e = std::min(len, t + dash);
      if (horizontal) dl->AddRectFilled(ImVec2(a.x + t, a.y + w0), ImVec2(a.x + e, a.y + w1), col);
      else dl->AddRectFilled(ImVec2(a.x + w0, a.y + t), ImVec2(a.x + w1, a.y + e), col);
    }
  };
  const float one = std::max(1.0f, std::floor(s));
  switch (ls) {
    case LineStyle::None: break;
    case LineStyle::Thin: rect(0, one); break;
    case LineStyle::Hair: dashed(1, 1, 0, 1); break;
    case LineStyle::Dotted: dashed(one, one * 2, 0, one); break;
    case LineStyle::Dashed: dashed(3 * one, one, 0, one); break;
    case LineStyle::DashDot: dashed(6 * one, 2 * one, 0, one); break;
    case LineStyle::DashDotDot: dashed(6 * one, 3 * one, 0, one); break;
    case LineStyle::Medium: rect(-one, one); break;
    case LineStyle::MediumDashed:
    case LineStyle::MediumDashDot:
    case LineStyle::MediumDashDotDot:
    case LineStyle::SlantDashDot: dashed(6 * one, 2 * one, -one, one); break;
    case LineStyle::Thick: rect(-one, 2 * one); break;
    case LineStyle::Double:
      rect(-one, 0);
      rect(one, 2 * one);
      break;
  }
}

// the four edges and diagonal borders of a cell; x0..x1, y0..y1 is the cell's screen area (right and bottom edges are gridline positions)
void DrawCellBorders(const Ctx& c, const model::Style& st, float x0, float y0, float x1, float y1, bool top, bool left, bool bottom, bool right) {
  using model::BorderEdge;
  const float s = Dpi() * c.zoom;
  auto col = [&](const model::BorderSide& b) { return b.color ? ToCol(b.color) : IM_COL32(0, 0, 0, 255); };
  const auto& B = st.border;
  if (top && B[model::kTop].style != model::LineStyle::None) DrawBorderLine(c.dl, B[model::kTop].style, col(B[model::kTop]), ImVec2(x0 - 1, y0 - 1), ImVec2(x1, y0 - 1), true, s);
  if (bottom && B[model::kBottom].style != model::LineStyle::None) DrawBorderLine(c.dl, B[model::kBottom].style, col(B[model::kBottom]), ImVec2(x0 - 1, y1 - 1), ImVec2(x1, y1 - 1), true, s);
  if (left && B[model::kLeft].style != model::LineStyle::None) DrawBorderLine(c.dl, B[model::kLeft].style, col(B[model::kLeft]), ImVec2(x0 - 1, y0 - 1), ImVec2(x0 - 1, y1), false, s);
  if (right && B[model::kRight].style != model::LineStyle::None) DrawBorderLine(c.dl, B[model::kRight].style, col(B[model::kRight]), ImVec2(x1 - 1, y0 - 1), ImVec2(x1 - 1, y1), false, s);
  if (B[model::kDiagDown].style != model::LineStyle::None) c.dl->AddLine(ImVec2(x0, y0), ImVec2(x1 - 1, y1 - 1), col(B[model::kDiagDown]), std::max(1.0f, s));
  if (B[model::kDiagUp].style != model::LineStyle::None) c.dl->AddLine(ImVec2(x0, y1 - 1), ImVec2(x1 - 1, y0), col(B[model::kDiagUp]), std::max(1.0f, s));
}

ImU32 Blend(ImU32 a, ImU32 b, float f) {
  auto ch = [&](int sh) {
    const float x = static_cast<float>((a >> sh) & 0xFF), y = static_cast<float>((b >> sh) & 0xFF);
    return static_cast<ImU32>(x + (y - x) * f + 0.5f) << sh;
  };
  return ch(0) | ch(8) | ch(16) | (0xFFu << 24);
}

}  // namespace

const char* CursorName(int cursor) {
  switch (cursor) {
    case kCursorCell: return "cell";
    case kCursorFill: return "fill";
    case kCursorColHeader: return "column_header";
    case kCursorRowHeader: return "row_header";
    case kCursorColUnhide: return "column_unhide";
    case kCursorRowUnhide: return "row_unhide";
    case ImGuiMouseCursor_None: return "none";
    case ImGuiMouseCursor_Arrow: return "arrow";
    case ImGuiMouseCursor_TextInput: return "text";
    case ImGuiMouseCursor_ResizeAll: return "move";
    case ImGuiMouseCursor_ResizeEW: return "resize_ew";
    case ImGuiMouseCursor_ResizeNS: return "resize_ns";
    case ImGuiMouseCursor_ResizeNESW: return "resize_nesw";
    case ImGuiMouseCursor_ResizeNWSE: return "resize_nwse";
    case ImGuiMouseCursor_Hand: return "hand";
    case ImGuiMouseCursor_NotAllowed: return "not_allowed";
    default: return "other";
  }
}

int& LastFrameCursor() {
  static int c = ImGuiMouseCursor_Arrow;
  return c;
}

// Excel's cursors on their 32 x 32 grid (measured from Excel's own cursor resources: the fill handle's thin cross,
// the header arrows, the resize arrows), each pixel '#' the shape, 'o' its 1 px outline, '.' clear
std::vector<std::string> CursorDesign(int cursor, int* hot_x, int* hot_y) {
  std::vector<std::string> g(32, std::string(32, '.'));
  auto fill = [&](int x, int y) {
    if (x >= 0 && y >= 0 && x < 32 && y < 32) g[static_cast<size_t>(y)][static_cast<size_t>(x)] = '#';
  };
  int hx = 15, hy = 15;
  switch (cursor) {
    case kCursorCell:  // the white cross over cells: 3 px arms, 15 px across (17 with the outline)
      for (int d = -7; d <= 7; ++d)
        for (int w = -1; w <= 1; ++w) fill(15 + d, 15 + w), fill(15 + w, 15 + d);
      break;
    case kCursorFill:  // the fill handle's thin black cross: 1 px, 11 px across
      for (int d = -5; d <= 5; ++d) fill(15 + d, 15), fill(15, 15 + d);
      break;
    case kCursorColHeader: {  // a black arrow pointing down: a 3 px stem, a head 7 px wide, its tip the hotspot
      for (int y = 11; y <= 17; ++y)
        for (int x = 13; x <= 15; ++x) fill(x, y);
      const int half[4] = {3, 2, 1, 0};
      for (int k = 0; k < 4; ++k)
        for (int x = 14 - half[k]; x <= 14 + half[k]; ++x) fill(x, 17 + k);
      hx = 14, hy = 20;
      break;
    }
    case kCursorRowHeader: {  // pointing right
      for (int x = 11; x <= 17; ++x)
        for (int y = 16; y <= 18; ++y) fill(x, y);
      const int half[4] = {3, 2, 1, 0};
      for (int k = 0; k < 4; ++k)
        for (int y = 17 - half[k]; y <= 17 + half[k]; ++y) fill(17 + k, y);
      hx = 21, hy = 17;
      break;
    }
    case kCursorColUnhide:
    case kCursorRowUnhide: {  // two bars with an arrow out of each side (Excel's resize arrows, 19 px across)
      const bool cols = cursor == kCursorColUnhide;
      auto f = [&](int a, int b) { cols ? fill(a, b) : fill(b, a); };  // a along the arrows, b across
      for (int b = 8; b <= 22; ++b) f(14, b), f(17, b);
      for (int a = 8; a <= 13; ++a) f(a, 15);
      for (int a = 18; a <= 23; ++a) f(a, 15);
      for (int k = 1; k <= 3; ++k)
        for (int b = 15 - k; b <= 15 + k; ++b) f(8 + k, b), f(23 - k, b);
      break;
    }
    default: break;
  }
  // the outline: every clear pixel next to the shape
  std::vector<std::string> out = g;
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) {
      if (g[static_cast<size_t>(y)][static_cast<size_t>(x)] != '.') continue;
      bool edge = false;
      for (int ky = -1; ky <= 1 && !edge; ++ky)
        for (int kx = -1; kx <= 1 && !edge; ++kx) {
          const int xx = x + kx, yy = y + ky;
          edge = xx >= 0 && yy >= 0 && xx < 32 && yy < 32 && g[static_cast<size_t>(yy)][static_cast<size_t>(xx)] == '#';
        }
      if (edge) out[static_cast<size_t>(y)][static_cast<size_t>(x)] = 'o';
    }
  if (hot_x) *hot_x = hx;
  if (hot_y) *hot_y = hy;
  return out;
}

void* NativeCursor(int cursor) {
  if (cursor < kCursorCell || cursor > kCursorRowUnhide) return nullptr;
  static HCURSOR made[8] = {};
  HCURSOR& h = made[cursor - kCursorCell];
  if (h) return h;
  // Excel's 32 x 32 design, scaled to the system cursor size (48 at 150%) as Windows scales Excel's own cursors
  int design_hx = 15, design_hy = 15;
  const std::vector<std::string> design = CursorDesign(cursor, &design_hx, &design_hy);
  const int n = std::max(32, ::GetSystemMetrics(SM_CXCURSOR));
  const float u = 32.0f / static_cast<float>(n);
  const bool white_fill = cursor == kCursorCell;
  auto at = [&](int x, int y) {
    const int dx = std::min(31, static_cast<int>((x + 0.5f) * u)), dy = std::min(31, static_cast<int>((y + 0.5f) * u));
    return design[static_cast<size_t>(dy)][static_cast<size_t>(dx)];
  };
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = n;
  bi.bV5Height = -n;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC dc = ::GetDC(nullptr);
  HBITMAP color = ::CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, dc);
  if (!color || !bits) return nullptr;
  auto* px = static_cast<uint32_t*>(bits);
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const char c = at(x, y);
      uint32_t v = 0;
      if (c == '#') v = white_fill ? 0xFFFFFFFFu : 0xFF000000u;
      else if (c == 'o') v = white_fill ? 0xFF000000u : 0xFFFFFFFFu;
      px[static_cast<size_t>(y) * n + x] = v;
    }
  }
  std::vector<uint8_t> zeros(static_cast<size_t>((n + 15) / 16) * 2 * n, 0);
  HBITMAP mask = ::CreateBitmap(n, n, 1, 1, zeros.data());
  ICONINFO ii{};
  ii.fIcon = FALSE;
  ii.xHotspot = static_cast<DWORD>((design_hx + 0.5f) / u);  // the design's hotspot pixel (an arrow's tip), scaled
  ii.yHotspot = static_cast<DWORD>((design_hy + 0.5f) / u);
  ii.hbmMask = mask;
  ii.hbmColor = color;
  h = reinterpret_cast<HCURSOR>(::CreateIconIndirect(&ii));
  ::DeleteObject(mask);
  ::DeleteObject(color);
  return h;
}

// cell fill: solid, one of Excel's patterns (in the pattern color over the fill color) or a two-color gradient
void DrawCellFill(ImDrawList* dl, const ImRect& r, const model::Style& st, float scale, ImU32 background) {
  using model::FillGradient;
  using model::FillPattern;
  const ImU32 c1 = (st.fill_color >> 24) ? ToCol(st.fill_color) : background;
  if (st.gradient != FillGradient::None) {
    const ImU32 c2 = (st.gradient_color2 >> 24) ? ToCol(st.gradient_color2) : IM_COL32_WHITE;
    if (st.gradient == FillGradient::FromCenter) {
      const ImVec2 c = r.GetCenter();
      const ImVec2 pts[4] = {r.Min, ImVec2(r.Max.x, r.Min.y), r.Max, ImVec2(r.Min.x, r.Max.y)};
      const ImVec2 uv = dl->_Data->TexUvWhitePixel;
      dl->PrimReserve(12, 12);
      for (int i = 0; i < 4; ++i) {
        const auto idx = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx(c, uv, c1);
        dl->PrimWriteVtx(pts[i], uv, c2);
        dl->PrimWriteVtx(pts[(i + 1) % 4], uv, c2);
        dl->PrimWriteIdx(idx);
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 1));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
      }
      return;
    }
    if (st.gradient == FillGradient::FromCorner) {
      // bands around the corner: the first color in the corner, the second at the far sides
      const int corner = std::clamp<int>(st.gradient_angle, 0, 3);
      const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
      const int steps = 24;
      for (int i = 0; i < steps; ++i) {
        const float f = 1.0f - static_cast<float>(i) / steps;
        const float w = r.GetWidth() * f, h = r.GetHeight() * f;
        const ImVec2 a0(right ? r.Max.x - w : r.Min.x, bottom ? r.Max.y - h : r.Min.y);
        dl->AddRectFilled(a0, ImVec2(a0.x + w, a0.y + h), Blend(c1, c2, f));
      }
      return;
    }
    const int a = ((st.gradient_angle % 360) + 360) % 360;
    const ImU32 mid = Blend(c1, c2, 0.5f);
    ImU32 tl = c1, tr = c2, br = c2, bl = c1;  // 0: left to right
    switch (a) {
      case 90: tl = tr = c1, bl = br = c2; break;
      case 180: tl = bl = c2, tr = br = c1; break;
      case 270: tl = tr = c2, bl = br = c1; break;
      case 45: tl = c1, br = c2, tr = bl = mid; break;
      case 135: tr = c1, bl = c2, tl = br = mid; break;
      case 225: br = c1, tl = c2, tr = bl = mid; break;
      case 315: bl = c1, tr = c2, tl = br = mid; break;
      default: break;
    }
    dl->AddRectFilledMultiColor(r.Min, r.Max, tl, tr, br, bl);
    return;
  }
  const ImU32 pc = (st.pattern_color >> 24) ? ToCol(st.pattern_color) : IM_COL32(0, 0, 0, 255);
  // dotted grays look like an even mix of the two colors at screen resolution
  float gray = 0;
  switch (st.pattern) {
    case FillPattern::Gray75: gray = 0.75f; break;
    case FillPattern::Gray50: gray = 0.5f; break;
    case FillPattern::Gray25: gray = 0.25f; break;
    case FillPattern::Gray125: gray = 0.125f; break;
    case FillPattern::Gray0625: gray = 0.0625f; break;
    case FillPattern::SemiGray75: gray = 0.6f; break;
    default: break;
  }
  dl->AddRectFilled(r.Min, r.Max, gray > 0 ? Blend(c1, pc, gray) : c1);
  if (st.pattern == FillPattern::Solid || gray > 0) return;
  const float step = std::max(3.0f, 4.0f * scale);
  const bool light = st.pattern >= FillPattern::LightHorizontal;
  const float th = std::max(1.0f, (light ? 1.0f : 2.0f) * scale);
  const bool horiz = st.pattern == FillPattern::Horizontal || st.pattern == FillPattern::LightHorizontal || st.pattern == FillPattern::Grid;
  const bool vert = st.pattern == FillPattern::Vertical || st.pattern == FillPattern::LightVertical || st.pattern == FillPattern::Grid;
  const bool down = st.pattern == FillPattern::Down || st.pattern == FillPattern::LightDown || st.pattern == FillPattern::Checker ||
                    st.pattern == FillPattern::CrissCross;
  const bool up = st.pattern == FillPattern::Up || st.pattern == FillPattern::LightUp || st.pattern == FillPattern::Checker ||
                  st.pattern == FillPattern::CrissCross;
  dl->PushClipRect(r.Min, r.Max, true);
  if (horiz) {
    for (float y = r.Min.y; y < r.Max.y; y += step) dl->AddRectFilled(ImVec2(r.Min.x, y), ImVec2(r.Max.x, std::min(r.Max.y, y + th)), pc);
  }
  if (vert) {
    for (float x = r.Min.x; x < r.Max.x; x += step) dl->AddRectFilled(ImVec2(x, r.Min.y), ImVec2(std::min(r.Max.x, x + th), r.Max.y), pc);
  }
  const float h = r.GetHeight();
  if (down) {
    for (float k = -h; k < r.GetWidth(); k += step) dl->AddLine(ImVec2(r.Min.x + k, r.Min.y), ImVec2(r.Min.x + k + h, r.Max.y), pc, th);
  }
  if (up) {
    for (float k = -h; k < r.GetWidth(); k += step) dl->AddLine(ImVec2(r.Min.x + k, r.Max.y), ImVec2(r.Min.x + k + h, r.Min.y), pc, th);
  }
  dl->PopClipRect();
}

namespace {

struct CellText {
  std::string text;
  bool numeric = false;
  uint32_t color = 0;
  bool hashes = false;
  bool image = false;  // an IMAGE() value: drawn as the picture (text = its alt text)
  formula::ImageSpec img;
};

CellText MakeText(const Ctx& c, const model::Style& st, const core::Value& v, float cell_w, ImFont* font, float px, uint32_t r, uint32_t col) {
  CellText out;
  if (c.opt.show_formulas) {
    std::string f = engine::FormulaTextOf(c.snap.wb, c.sheet.id, r, col, c.opt.r1c1);
    if (!f.empty()) {
      out.text = std::move(f);
      return out;
    }
  }
  if (v.IsString() && formula::ParseImageValue(v.str(), &out.img)) {
    out.image = true;
    out.text = out.img.alt;
    return out;
  }
  // Show Formulas: constants lose their number formats too (a date shows its serial number), as in Excel
  const uint32_t numfmt = c.opt.show_formulas ? 0 : st.numfmt;
  const formula::NumberFormat& fmt = FormatFor(c.styles, numfmt);
  const float digit_w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, "0").x;
  const double max_chars = v.IsNumeric() && numfmt == 0 ? std::floor((cell_w - 2 * c.pad) / std::max(1.0f, digit_w)) : -1;
  formula::FormatResult fr = formula::FormatValue(v, fmt, max_chars);
  if (max_chars > 0 && max_chars < 11) {
    // the budget counts every character as a digit, but "." and "-" are narrower: one or two more characters may fit
    // (Excel shows 0.333333 and -12000.8 in a default column, not 0.33333 and -12001)
    const float room = cell_w - 2 * c.pad;
    for (double extra = 2; extra >= 1; --extra) {
      formula::FormatResult wider = formula::FormatValue(v, fmt, max_chars + extra);
      if (wider.hashes || (!fr.hashes && wider.text.size() <= fr.text.size())) continue;
      if (font->CalcTextSizeA(px, FLT_MAX, 0.0f, wider.text.c_str()).x <= room) {
        fr = std::move(wider);
        break;
      }
    }
  }
  ++c.stats.formats;
  out.text = std::move(fr.text);
  out.numeric = fr.numeric || v.IsNumeric();
  out.color = fr.color;
  out.hashes = fr.hashes;
  if ((out.numeric && !out.text.empty() && max_chars < 0) || fr.bad_date) {
    // numbers with a non-General format can't overflow: too wide shows # across the cell, as does a date outside
    // the calendar
    const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, out.text.c_str()).x;
    if (fr.bad_date || w > cell_w - 2 * c.pad + 1) {
      const int n = std::max(1, static_cast<int>((cell_w - 2 * c.pad) / std::max(1.0f, font->CalcTextSizeA(px, FLT_MAX, 0.0f, "#").x)));
      out.text.assign(static_cast<size_t>(n), '#');
      out.hashes = true;
    }
  }
  if (!out.hashes && fr.fill_at >= 0 && !fr.fill.empty() && fr.fill_at <= static_cast<int>(out.text.size())) {
    // *x: x repeats where the code puts it until the text spans the cell, so an accounting format's $ sits at the
    // left edge and the number at the right (Excel)
    const float room = cell_w - 2 * c.pad;
    const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, out.text.c_str()).x;
    const float fw = font->CalcTextSizeA(px, FLT_MAX, 0.0f, fr.fill.c_str()).x;
    if (fw > 0 && w < room) {
      const int n = std::min(4096, static_cast<int>((room - w) / fw));
      std::string run;
      for (int k = 0; k < n; ++k) run += fr.fill;
      out.text.insert(static_cast<size_t>(fr.fill_at), run);
    }
  }
  return out;
}

// an IMAGE() picture inside its cell (sizing: 0 fit keeping the aspect ratio, 1 fill, 2 original size, 3 height x width);
// false while it isn't available (the alt text is drawn instead)
bool DrawCellImage(const Ctx& c, const CellText& ct, const ImRect& cell, const ImVec4& clip) {
  if (!c.opt.image_for) return false;
  ImVec2 size;
  const ImTextureID tex = c.opt.image_for(ct.img.url, &size);
  if (!tex || size.x <= 0 || size.y <= 0) return false;
  const float s = Dpi() * c.zoom;
  const float cw = cell.GetWidth() - 2, ch = cell.GetHeight() - 2;
  float w = size.x * s, h = size.y * s;
  switch (ct.img.sizing) {
    case 1:
      w = cw;
      h = ch;
      break;
    case 2: break;
    case 3: {
      const double hh = ct.img.height > 0 ? ct.img.height : ct.img.width * size.y / size.x;
      const double ww = ct.img.width > 0 ? ct.img.width : ct.img.height * size.x / size.y;
      w = static_cast<float>(ww) * s;
      h = static_cast<float>(hh) * s;
      break;
    }
    default: {
      const float k = std::min(cw / size.x, ch / size.y);
      w = size.x * k;
      h = size.y * k;
    }
  }
  const ImVec2 p0(cell.Min.x + 1 + (cw - w) * 0.5f, cell.Min.y + 1 + (ch - h) * 0.5f);
  c.dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z + 1, clip.w + 1), true);
  c.dl->AddImage(tex, p0, ImVec2(p0.x + w, p0.y + h));
  c.dl->PopClipRect();
  return true;
}

// rotated text (Format Cells -> Alignment -> Orientation): -90..90 degrees counterclockwise, or 255 = vertical (stacked)
void DrawRotatedText(const Ctx& c, const model::Style& st, const CellText& ct, ImFont* font, float px, const ImRect& cell, const ImVec4& clip, ImU32 col) {
  const float pad = c.pad;
  c.dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z + 1, clip.w + 1), true);
  if (st.rotation == 255) {
    // one character per line, centered
    std::vector<std::string> chars;
    for (size_t i = 0; i < ct.text.size();) {
      const unsigned char b = static_cast<unsigned char>(ct.text[i]);
      const size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
      chars.push_back(ct.text.substr(i, n));
      i += n;
    }
    const float lh = px * 1.05f, total = lh * static_cast<float>(chars.size());
    float y;
    switch (st.valign) {
      case model::VAlign::Top: y = cell.Min.y + 1; break;
      case model::VAlign::Center:
      case model::VAlign::Justify:
      case model::VAlign::Distributed: y = (cell.Min.y + cell.Max.y - total) * 0.5f; break;
      default: y = cell.Max.y - 2 - total; break;
    }
    for (const auto& ch : chars) {
      const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, ch.c_str()).x;
      float x;
      switch (st.halign) {
        case model::HAlign::Left: x = cell.Min.x + pad; break;
        case model::HAlign::Right: x = cell.Max.x - pad - 1 - w; break;
        default: x = (cell.Min.x + cell.Max.x - 1 - w) * 0.5f; break;
      }
      c.dl->AddText(font, px, ImVec2(IM_ROUND(x), IM_ROUND(y)), col, ch.c_str());
      y += lh;
    }
    c.dl->PopClipRect();
    ++c.stats.texts_drawn;
    return;
  }
  const float a = static_cast<float>(st.rotation) * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a);
  const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, ct.text.c_str());
  // the rotated text's bounding box, aligned in the cell
  const float bw = std::fabs(ts.x * ca) + std::fabs(ts.y * sa), bh = std::fabs(ts.x * sa) + std::fabs(ts.y * ca);
  model::HAlign ha = st.halign;
  if (ha == model::HAlign::General) ha = (ct.numeric != (st.reading_order == 2)) ? model::HAlign::Right : model::HAlign::Left;  // Right-to-Left mirrors General
  float cx;
  switch (ha) {
    case model::HAlign::Right: cx = cell.Max.x - pad - 1 - bw * 0.5f; break;
    case model::HAlign::Center:
    case model::HAlign::CenterAcross:
    case model::HAlign::Distributed:
    case model::HAlign::Justify: cx = (cell.Min.x + cell.Max.x - 1) * 0.5f; break;
    default: cx = cell.Min.x + pad + bw * 0.5f; break;
  }
  float cy;
  switch (st.valign) {
    case model::VAlign::Top: cy = cell.Min.y + 1 + bh * 0.5f; break;
    case model::VAlign::Center:
    case model::VAlign::Justify:
    case model::VAlign::Distributed: cy = (cell.Min.y + cell.Max.y - 1) * 0.5f; break;
    default: cy = cell.Max.y - 2 - bh * 0.5f; break;
  }
  // draw horizontally around the center, then turn the new vertices (counterclockwise on screen)
  const int v0 = c.dl->VtxBuffer.Size;
  const ImVec2 pos(cx - ts.x * 0.5f, cy - ts.y * 0.5f);
  c.dl->AddText(font, px, pos, col, ct.text.c_str());
  if (st.underline != model::Underline::None || st.strike) {
    const float th = std::max(1.0f, std::floor(px / 14.0f));
    if (st.underline != model::Underline::None) c.dl->AddRectFilled(ImVec2(pos.x, pos.y + ts.y - th - 1), ImVec2(pos.x + ts.x, pos.y + ts.y - 1), col);
    if (st.strike) c.dl->AddRectFilled(ImVec2(pos.x, pos.y + ts.y * 0.55f), ImVec2(pos.x + ts.x, pos.y + ts.y * 0.55f + th), col);
  }
  for (int i = v0; i < c.dl->VtxBuffer.Size; ++i) {
    ImVec2& v = c.dl->VtxBuffer[i].pos;
    const float dx = v.x - cx, dy = v.y - cy;
    v = ImVec2(cx + dx * ca + dy * sa, cy - dx * sa + dy * ca);
  }
  c.dl->PopClipRect();
  ++c.stats.texts_drawn;
}

// text with several fonts (Characters().Font) or a superscript / subscript cell
void DrawRichText(const Ctx& c, const model::Style& st, const model::RichText& rt, bool numeric, const ImRect& cell, const ImVec4& clip, ImU32 def_col) {
  const float k = (96.0f / 72.0f) * Dpi() * c.zoom;
  const size_t nruns = std::max<size_t>(1, rt.runs.size());
  std::vector<model::RunFont> fonts(nruns);
  std::vector<ImFont*> faces(nruns);
  for (size_t i = 0; i < nruns; ++i) {
    fonts[i] = model::ResolveRunFont(c.styles, st, rt.runs.empty() ? nullptr : &rt.runs[i]);
    faces[i] = ui::CellFont(fonts[i].name, fonts[i].bold, fonts[i].italic);
  }
  auto draw_px = [&](size_t i) { return fonts[i].size * k * (fonts[i].script ? 0.65f : 1.0f); };
  model::HAlign ha = st.halign;
  if (ha == model::HAlign::General) ha = (numeric != (st.reading_order == 2)) ? model::HAlign::Right : model::HAlign::Left;
  const int hai = ha == model::HAlign::Right ? 2 : (ha == model::HAlign::Left || ha == model::HAlign::Fill) ? 0 : 1;
  const bool wrap = st.wrap && !numeric;
  const float avail_w = cell.GetWidth() - 2 * c.pad - 1;
  const model::RichLayout lay = model::LayoutRich(rt, wrap, avail_w, hai, c.styles.FontSize(st) * k, [&](const std::string& t, size_t i) {
    const float w = faces[i]->CalcTextSizeA(draw_px(i), FLT_MAX, 0.0f, t.c_str()).x;
    return std::pair<float, float>(w, fonts[i].size * k);  // superscripts keep the full line height
  });
  const float indent = st.indent * c.styles.FontSize(st) * k * 0.75f;
  float x0;
  if (wrap) x0 = cell.Min.x + c.pad;
  else if (hai == 2) x0 = cell.Max.x - c.pad - 1 - lay.width - indent;
  else if (hai == 1) x0 = (cell.Min.x + cell.Max.x - 1 - lay.width) * 0.5f;
  else x0 = cell.Min.x + c.pad + indent;
  float y0;
  switch (st.valign) {
    case model::VAlign::Top: y0 = cell.Min.y + std::max(1.0f, S(1) * c.zoom); break;
    case model::VAlign::Center:
    case model::VAlign::Justify:
    case model::VAlign::Distributed: y0 = (cell.Min.y + cell.Max.y - 1 - lay.height) * 0.5f; break;
    default: y0 = cell.Max.y - 1 - lay.height - std::max(0.0f, S(1) * c.zoom); break;
  }
  for (const auto& p : lay.pieces) {
    const model::RunFont& f = fonts[p.run];
    const ImU32 col = f.color ? ToCol(f.color) : def_col;
    const float px = draw_px(p.run);
    float y = y0 + p.y;
    if (f.script == 2) y += p.h - px;  // subscript: on the bottom
    const ImVec2 pos(IM_ROUND(x0 + p.x), IM_ROUND(y));
    c.dl->AddText(faces[p.run], px, pos, col, p.text.c_str(), nullptr, 0.0f, &clip);
    if (f.underline != model::Underline::None || f.strike) {
      const float th = std::max(1.0f, std::floor(px / 14.0f));
      auto hline = [&](float yy) {
        const float a = std::max(pos.x, clip.x), b = std::min(pos.x + p.w, clip.z);
        if (b > a && yy >= clip.y && yy <= clip.w) c.dl->AddRectFilled(ImVec2(a, yy), ImVec2(b, yy + th), col);
      };
      if (f.underline != model::Underline::None) {
        hline(pos.y + px - th - 1);
        if (f.underline == model::Underline::Double || f.underline == model::Underline::DoubleAccounting) hline(pos.y + px - 3 * th - 1);
      }
      if (f.strike) hline(pos.y + px * 0.55f);
    }
  }
  ++c.stats.texts_drawn;
}

// text drawing (horizontal / vertical alignment, wrap text, underline, strikethrough)
void DrawText(const Ctx& c, const model::Style& st, const CellText& ct, ImFont* font, float px, const ImRect& cell, const ImVec4& clip,
              const model::RichText* rich = nullptr) {
  if (ct.text.empty()) return;
  ImU32 col = st.font_color ? ToCol(st.font_color) : T().cell_text;
  if (ct.color) col = ToCol(ct.color);
  if ((rich || st.script) && st.rotation == 0 && !c.opt.show_formulas) {
    if (rich) {
      DrawRichText(c, st, *rich, ct.numeric, cell, clip, col);
    } else {
      model::RichText one;
      one.text = ct.text;
      DrawRichText(c, st, one, ct.numeric, cell, clip, col);
    }
    return;
  }
  if (st.rotation != 0) {
    DrawRotatedText(c, st, ct, font, px, cell, clip, col);
    return;
  }
  model::HAlign ha = st.halign;
  if (ha == model::HAlign::General) ha = (ct.numeric != (st.reading_order == 2)) ? model::HAlign::Right : model::HAlign::Left;  // Right-to-Left mirrors General
  if (ha == model::HAlign::CenterAcross) ha = model::HAlign::Center;
  const float avail_w = cell.GetWidth() - 2 * c.pad - 1;
  const bool wrap = st.wrap && !ct.numeric;
  const float wrap_w = wrap ? std::max(1.0f, avail_w) : 0.0f;
  const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, wrap_w, ct.text.c_str());
  const float indent = st.indent * px * 0.75f;
  float x;
  switch (ha) {
    case model::HAlign::Right: x = cell.Max.x - c.pad - 1 - ts.x - indent; break;
    case model::HAlign::Center:
    case model::HAlign::Distributed:
    case model::HAlign::Justify: x = (cell.Min.x + cell.Max.x - 1 - ts.x) * 0.5f; break;
    default: x = cell.Min.x + c.pad + indent; break;
  }
  float y;
  switch (st.valign) {
    case model::VAlign::Top: y = cell.Min.y + std::max(1.0f, S(1) * c.zoom); break;
    case model::VAlign::Center:
    case model::VAlign::Justify:
    case model::VAlign::Distributed: y = (cell.Min.y + cell.Max.y - 1 - ts.y) * 0.5f; break;
    default: y = cell.Max.y - 1 - ts.y - std::max(0.0f, S(1) * c.zoom); break;
  }
  const ImVec2 pos(IM_ROUND(x), IM_ROUND(y));
  c.dl->AddText(font, px, pos, col, ct.text.c_str(), nullptr, wrap_w, &clip);
  ++c.stats.texts_drawn;
  if ((st.underline != model::Underline::None || st.strike) && !wrap) {
    const float th = std::max(1.0f, std::floor(px / 14.0f));
    const float x1 = pos.x + ts.x;
    auto hline = [&](float yy) {
      const float a = std::max(pos.x, clip.x), b = std::min(x1, clip.z);
      if (b > a && yy >= clip.y && yy <= clip.w) c.dl->AddRectFilled(ImVec2(a, yy), ImVec2(b, yy + th), col);
    };
    if (st.underline != model::Underline::None) {
      const float uy = pos.y + ts.y - th - 1;
      hline(uy);
      if (st.underline == model::Underline::Double || st.underline == model::Underline::DoubleAccounting) hline(uy - 2 * th);
    }
    if (st.strike) hline(pos.y + ts.y * 0.55f);
  }
}

// Page Layout > Background: the picture tiled from the sheet's top-left corner, under the gridlines and fills
void DrawSheetBackground(Ctx& c, const Pane& p) {
  if (!c.opt.background || c.opt.background_size.x <= 0 || c.opt.background_size.y <= 0 || p.cols.empty() || p.rows.empty()) return;
  const float tw = c.opt.background_size.x * c.zoom * Dpi(), th = c.opt.background_size.y * c.zoom * Dpi();
  if (tw < 2 || th < 2) return;
  // where the sheet's origin would be on screen: from the first visible column / row and their offsets
  const auto& fc = p.cols.front();
  const auto& fr = p.rows.front();
  const double sx = static_cast<double>(c.sheet.cols->Offset(fc.index)), sy = static_cast<double>(c.sheet.rows->Offset(fr.index));
  const double k = static_cast<double>(c.zoom * Dpi());
  const float ox = fc.pos - static_cast<float>(sx * k), oy = fr.pos - static_cast<float>(sy * k);
  const float x0 = ox + std::floor((p.rect.Min.x - ox) / tw) * tw, y0 = oy + std::floor((p.rect.Min.y - oy) / th) * th;
  for (float y = y0; y < p.rect.Max.y; y += th) {
    for (float x = x0; x < p.rect.Max.x; x += tw) c.dl->AddImage(c.opt.background, ImVec2(x, y), ImVec2(x + tw, y + th));
  }
}

// cell contents of one pane
void DrawPane(Ctx& c, const Pane& p) {
  const Theme& t = T();
  ImDrawList* dl = c.dl;
  dl->PushClipRect(p.rect.Min, p.rect.Max, true);
  dl->AddRectFilled(p.rect.Min, p.rect.Max, t.cell_bg);
  DrawSheetBackground(c, p);
  if (c.opt.show_gridlines) {
    const ImU32 gl = c.opt.gridline_color ? ToCol(c.opt.gridline_color | 0xFF000000u) : t.gridline;  // Options > Gridline color
    for (const Line& l : p.cols) dl->AddRectFilled(ImVec2(l.pos + l.size - 1, p.rect.Min.y), ImVec2(l.pos + l.size, p.rect.Max.y), gl);
    for (const Line& l : p.rows) dl->AddRectFilled(ImVec2(p.rect.Min.x, l.pos + l.size - 1), ImVec2(p.rect.Max.x, l.pos + l.size), gl);
  }
  const Rect vis{static_cast<uint32_t>(p.rows.front().index), static_cast<uint32_t>(p.cols.front().index), static_cast<uint32_t>(p.rows.back().index),
                 static_cast<uint32_t>(p.cols.back().index)};

  // ---- Pass 1: fills (including conditional-format fills and data bars) ----
  for (const Line& rl : p.rows) {
    const uint32_t r = static_cast<uint32_t>(rl.index);
    for (const Line& cl : p.cols) {
      const uint32_t col = static_cast<uint32_t>(cl.index);
      ++c.stats.cells_processed;
      const uint32_t sid = c.sheet.GetStyle(r, col);
      if (!sid && !c.cf && !InPreview(c, r, col) && c.sheet.tables->empty()) continue;
      const engine::CfVisual* cfv = nullptr;
      const model::Style& st = StyleAt(c, r, col, &cfv);
      if (st.HasFill() && !MergeContaining(c, r, col)) DrawCellFill(dl, ImRect(cl.pos, rl.pos, cl.pos + cl.size, rl.pos + rl.size), st, Dpi() * c.zoom, t.cell_bg);
      if (cfv && cfv->bar >= 0) {
        const float m = std::max(1.0f, 2.0f * c.zoom * Dpi());
        const float w = cl.size - 2 * m, axis = cl.pos + m + w * cfv->bar_axis;
        const float y0 = rl.pos + m, y1 = rl.pos + rl.size - m - 1;
        const float len = w * cfv->bar;
        // positive bars grow right from the axis, negative ones left (red, gradient fading towards the axis);
        // Right-to-Left mirrors the bar in its cell
        float x0 = cfv->bar_negative ? axis - len : axis, x1 = cfv->bar_negative ? axis : axis + len;
        if (cfv->bar_rtl) {
          const float mid = cl.pos + cl.size * 0.5f;
          const float a = 2 * mid - x1, b = 2 * mid - x0;
          x0 = a;
          x1 = b;
        }
        const uint32_t argb = cfv->bar_negative ? 0xFFFF0000u : cfv->bar_color;
        if (x1 > x0 + 0.5f) {
          const ImU32 solid = ToCol(argb), fade = ToCol((argb & 0xFF000000u) | 0x00FFFFFFu);
          const bool grow_left = cfv->bar_negative != cfv->bar_rtl;
          const ImU32 left = grow_left ? fade : solid, right = grow_left ? solid : fade;
          if (cfv->bar_gradient) dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), left, right, right, left);
          else dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), solid);
          // Excel: a gradient bar has a border in its color; a solid one only when Border is Solid Border
          if (cfv->bar_border) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), ToCol(cfv->bar_border));
          else if (cfv->bar_gradient) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), solid);
        }
        if (cfv->bar_axis > 0.0f && cfv->bar_axis < 1.0f) {
          // the axis: a dashed black line
          for (float y = y0; y < y1; y += 4.0f) dl->AddLine(ImVec2(axis, y), ImVec2(axis, std::min(y + 2.0f, y1)), IM_COL32(0, 0, 0, 255));
        }
      }
      if (cfv && cfv->icon >= 0) {
        // icon set: the icon at the left of the cell (centered when the value is hidden)
        const float sz = std::min(rl.size - 4.0f, 16.0f * c.zoom * Dpi());
        if (sz >= 4.0f) {
          const float x0 = cfv->icon_only ? cl.pos + (cl.size - sz) * 0.5f : cl.pos + 2.0f, y0 = rl.pos + (rl.size - sz) * 0.5f;
          DrawCfIcon(dl, ImRect(x0, y0, x0 + sz, y0 + sz), cfv->icon_style, cfv->icon);
        }
      }
    }
  }

  // ---- Sparklines ----
  for (const auto& g : *c.sheet.sparklines) {
    double lo = NAN, hi = NAN;
    bool axis_done = false;
    for (const auto& it : g.items) {
      if (it.r < vis.r0 || it.r > vis.r1 || it.c < vis.c0 || it.c > vis.c1) continue;
      if (c.sheet.rows->IsHidden(it.r) || c.sheet.cols->IsHidden(it.c)) continue;
      if (!axis_done) engine::SparklineAxis(c.snap.wb, g, &lo, &hi), axis_done = true;
      std::vector<double> xfrac;
      const std::vector<double> vals = engine::SparklineValues(c.snap.wb, g, it, &xfrac);
      if (vals.empty()) continue;
      const ImRect cell(ColX(c, p, it.c), RowY(c, p, it.r), ColX(c, p, static_cast<int64_t>(it.c) + 1), RowY(c, p, static_cast<int64_t>(it.r) + 1));
      DrawSparkline(dl, cell, g, vals, c.zoom * Dpi(), lo, hi, &xfrac);
    }
  }

  // ---- Merged areas: background (covers the inner gridlines), text, outline ----
  for (const Rect& m : c.merges) {
    if (!m.Intersects(vis)) continue;
    const float x0 = ColX(c, p, m.c0), x1 = ColX(c, p, m.c1 + 1), y0 = RowY(c, p, m.r0), y1 = RowY(c, p, m.r1 + 1);
    const model::Style& st = StyleAt(c, m.r0, m.c0);
    if (st.HasFill()) DrawCellFill(dl, ImRect(x0, y0, x1, y1), st, Dpi() * c.zoom, t.cell_bg);
    else if (!c.opt.background) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1 - 1, y1 - 1), t.cell_bg);
    const core::Value& v = c.sheet.GetValue(m.r0, m.c0);
    if (!v.IsEmpty()) {
      ImFont* font = FontFor(c, st);
      const float px = FontPxFor(c, st);
      const CellText ct = MakeText(c, st, v, x1 - x0, font, px, m.r0, m.c0);
      const ImVec4 clip(std::max(x0, p.rect.Min.x), std::max(y0, p.rect.Min.y), std::min(x1 - 1, p.rect.Max.x), std::min(y1 - 1, p.rect.Max.y));
      if (!ct.image || !DrawCellImage(c, ct, ImRect(x0, y0, x1, y1), clip)) DrawText(c, st, ct, font, px, ImRect(x0, y0, x1, y1), clip, RichFor(c, m.r0, m.c0, v));
    }
    ++c.stats.merges_drawn;
  }

  // ---- Pass 2: text (with overflow) ----
  auto empty_at = [&](uint32_t r, uint32_t col) { return c.sheet.IsEmptyCell(r, col) && !MergeContaining(c, r, col); };
  for (const Line& rl : p.rows) {
    const uint32_t r = static_cast<uint32_t>(rl.index);
    // cells left of the visible area overflowing to the right into it
    int64_t start_col = p.cols.front().index;
    for (int64_t k = start_col - 1; k >= std::max<int64_t>(0, start_col - kOverflowLookback); --k) {
      if (c.sheet.IsEmptyCell(r, static_cast<uint32_t>(k))) continue;
      start_col = k;
      break;
    }
    for (int64_t ci = start_col; ci < p.cols.front().index; ++ci) {
      const uint32_t col = static_cast<uint32_t>(ci);
      const core::Value& v = c.sheet.GetValue(r, col);
      if (v.IsEmpty() || !v.IsString() || MergeContaining(c, r, col)) continue;
      if (!v.str().empty() && v.str()[0] == '\x1b') continue;  // IMAGE() value
      const model::Style& st = StyleAt(c, r, col);
      if (st.wrap || st.rotation || (st.halign != model::HAlign::General && st.halign != model::HAlign::Left)) continue;
      ImFont* font = FontFor(c, st);
      const float px = FontPxFor(c, st);
      const float x0 = ColX(c, p, col);
      const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, std::string(v.str()).c_str()).x;
      float x1 = ColX(c, p, col + 1);
      uint32_t k = col + 1;
      while (x1 < x0 + w + 2 * c.pad && k < c.sheet.col_limit && empty_at(r, k)) x1 = ColX(c, p, ++k);
      if (x1 <= p.rect.Min.x) continue;
      // cover the gridlines the text crosses
      for (const Line& cl : p.cols) {
        if (cl.pos + cl.size >= x1) break;
        if (!c.sheet.GetStyle(r, static_cast<uint32_t>(cl.index)) || !c.styles.Get(c.sheet.GetStyle(r, static_cast<uint32_t>(cl.index))).HasFill())
          if (!c.opt.background) dl->AddRectFilled(ImVec2(cl.pos + cl.size - 1, rl.pos), ImVec2(cl.pos + cl.size, rl.pos + rl.size - 1), t.cell_bg);
      }
      CellText ct;
      ct.text = std::string(v.str());
      DrawText(c, st, ct, font, px, ImRect(x0, rl.pos, x0 + std::max(w + 2 * c.pad + 2, ColX(c, p, col + 1) - x0), rl.pos + rl.size),
               ImVec4(p.rect.Min.x, rl.pos, x1 - 1, rl.pos + rl.size - 1), RichFor(c, r, col, v));
    }
    for (size_t ci = 0; ci < p.cols.size(); ++ci) {
      const Line& cl = p.cols[ci];
      const uint32_t col = static_cast<uint32_t>(cl.index);
      const core::Value& v = c.sheet.GetValue(r, col);
      if (v.IsEmpty()) continue;
      if (!c.opt.show_zeros && !c.opt.show_formulas && v.IsNumeric() && v.AsDouble() == 0.0) continue;  // "Show a zero in cells that have zero value" off
      if (MergeContaining(c, r, col)) continue;
      const engine::CfVisual* cfv = nullptr;
      const model::Style& st = StyleAt(c, r, col, &cfv);
      if (cfv && cfv->icon >= 0 && cfv->icon_only) continue;  // "Show Icon Only"
      if (cfv && cfv->bar >= 0 && cfv->bar_only) continue;    // data bar "Show Bar Only"
      ImFont* font = FontFor(c, st);
      const float px = FontPxFor(c, st);
      CellText ct = MakeText(c, st, v, cl.size, font, px, r, col);
      if (ct.image && DrawCellImage(c, ct, ImRect(cl.pos, rl.pos, cl.pos + cl.size, rl.pos + rl.size),
                                    ImVec4(cl.pos, rl.pos, cl.pos + cl.size - 1, rl.pos + rl.size - 1)))
        continue;
      if (ct.text.empty()) continue;
      ImRect cell(cl.pos, rl.pos, cl.pos + cl.size, rl.pos + rl.size);
      if (cfv && cfv->icon >= 0) cell.Min.x += std::min(rl.size - 4.0f, 16.0f * c.zoom * Dpi()) + 2.0f;  // after the icon
      ImVec4 clip(cl.pos, rl.pos, cl.pos + cl.size - 1, rl.pos + rl.size - 1);
      // text overflows into blank cells to the right (left-aligned), the left (right-aligned) or both sides (centered)
      if (!ct.numeric && !st.wrap && !st.rotation && !c.opt.show_formulas) {
        const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, ct.text.c_str()).x + 2 * c.pad;
        if (w > cl.size) {
          model::HAlign ha = st.halign == model::HAlign::General ? model::HAlign::Left : st.halign;
          float right = cl.pos + cl.size, left = cl.pos;
          if (ha == model::HAlign::Left || ha == model::HAlign::Center || ha == model::HAlign::CenterAcross) {
            const float need = ha == model::HAlign::Left ? cl.pos + w : (cl.pos + cl.size * 0.5f + w * 0.5f);
            uint32_t k = col + 1;
            while (right < need && k < c.sheet.col_limit && empty_at(r, k)) {
              right = ColX(c, p, k + 1);
              ++k;
            }
          }
          if (ha == model::HAlign::Right || ha == model::HAlign::Center || ha == model::HAlign::CenterAcross) {
            const float need = ha == model::HAlign::Right ? cl.pos + cl.size - w : (cl.pos + cl.size * 0.5f - w * 0.5f);
            int64_t k = static_cast<int64_t>(col) - 1;
            while (left > need && k >= 0 && empty_at(r, static_cast<uint32_t>(k))) {
              left = ColX(c, p, k);
              --k;
            }
          }
          if (right > cl.pos + cl.size || left < cl.pos) {
            for (const Line& ol : p.cols) {
              const float gx = ol.pos + ol.size - 1;
              if (gx <= left || gx >= right - 1) continue;
              const uint32_t oc = static_cast<uint32_t>(ol.index);
              const uint32_t osid = c.sheet.GetStyle(r, oc);
              if (!osid || !c.styles.Get(osid).HasFill()) dl->AddRectFilled(ImVec2(gx, rl.pos), ImVec2(gx + 1, rl.pos + rl.size - 1), t.cell_bg);
            }
            if (ha == model::HAlign::Left) cell.Max.x = std::max(cell.Max.x, cl.pos + w + 1);
            clip = ImVec4(left, rl.pos, right - 1, rl.pos + rl.size - 1);
          }
        }
      }
      DrawText(c, st, ct, font, px, cell, clip, RichFor(c, r, col, v));
    }
  }

  // ---- Pass 3: borders ----
  for (const Line& rl : p.rows) {
    const uint32_t r = static_cast<uint32_t>(rl.index);
    for (const Line& cl : p.cols) {
      const uint32_t col = static_cast<uint32_t>(cl.index);
      const uint32_t sid = c.sheet.GetStyle(r, col);
      if (!sid && !InPreview(c, r, col) && c.sheet.tables->empty() && !c.cf) continue;
      const model::Style& st = StyleAt(c, r, col);  // with the lines of a conditional format
      bool any = false;
      for (const auto& b : st.border) any = any || b.style != model::LineStyle::None;
      if (!any) continue;
      const Rect* m = MergeContaining(c, r, col);
      const bool top = !m || r == m->r0, bottom = !m || r == m->r1, left = !m || col == m->c0, right = !m || col == m->c1;
      DrawCellBorders(c, st, cl.pos, rl.pos, cl.pos + cl.size, rl.pos + rl.size, top, left, bottom, right);
    }
  }
  // conditional formats with a border (Red Border, the rule's Border): an outline around each cell that meets it
  if (c.cf) {
    for (const Line& rl : p.rows) {
      for (const Line& cl : p.cols) {
        const engine::CfVisual* cfv = nullptr;
        StyleAt(c, static_cast<uint32_t>(rl.index), static_cast<uint32_t>(cl.index), &cfv);
        if (!cfv || !(cfv->border >> 24)) continue;
        const uint32_t bc = model::ResolveColor(cfv->border);
        const ImU32 col = IM_COL32((bc >> 16) & 0xFF, (bc >> 8) & 0xFF, bc & 0xFF, 255);
        dl->AddRect(ImVec2(cl.pos - 1, rl.pos - 1), ImVec2(cl.pos + cl.size, rl.pos + rl.size), col, 0.0f, std::max(1.0f, c.zoom * Dpi()));
      }
    }
  }
  // ---- Background error checking: a green triangle in the upper-left corner (Excel) ----
  if (c.opt.error_check && c.opt.error_check->background && !c.opt.show_formulas) {
    const float tri = std::max(4.0f, 6.0f * c.zoom * Dpi());
    for (const Line& rl : p.rows) {
      const uint32_t r = static_cast<uint32_t>(rl.index);
      for (const Line& cl : p.cols) {
        const uint32_t col = static_cast<uint32_t>(cl.index);
        if (engine::ErrorCheckRule(c.snap.wb, c.sheet, r, col, *c.opt.error_check) == engine::kErrNone) continue;
        const Rect* m = MergeContaining(c, r, col);
        if (m && (r != m->r0 || col != m->c0)) continue;
        const float x0 = cl.pos + 1, y0 = rl.pos + 1;
        dl->AddTriangleFilled(ImVec2(x0, y0), ImVec2(x0 + tri, y0), ImVec2(x0, y0 + tri), IM_COL32(0x10, 0x80, 0x30, 255));
      }
    }
  }
  // ---- Notes: red triangle in the upper-right corner; threaded comments: a purple bubble ----
  if (!c.sheet.notes->empty()) {
    const auto& notes = *c.sheet.notes;
    const float tri = std::max(4.0f, 6.0f * c.zoom * Dpi());
    for (auto it = notes.lower_bound(uint64_t{vis.r0} << 16); it != notes.end() && (it->first >> 16) <= vis.r1; ++it) {
      const uint32_t r = static_cast<uint32_t>(it->first >> 16), col = static_cast<uint32_t>(it->first & 0xFFFF);
      if (col < vis.c0 || col > vis.c1 || c.sheet.rows->IsHidden(r) || c.sheet.cols->IsHidden(col)) continue;
      const Rect* m = MergeContaining(c, r, col);
      const float x1 = ColX(c, p, (m ? m->c1 : col) + 1) - 1, y0 = RowY(c, p, m ? m->r0 : r);
      if (it->second.threaded) {
        // a threaded comment: a small purple speech bubble (gray once resolved)
        const ImU32 bubble = it->second.resolved ? IM_COL32(0x9E, 0x9E, 0x9E, 255) : IM_COL32(0x77, 0x19, 0xAA, 255);
        const float w = tri * 1.25f, h = tri * 0.95f, bx0 = x1 - w - 1, by0 = y0 + 1;
        dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx0 + w, by0 + h), bubble, tri * 0.2f);
        dl->AddTriangleFilled(ImVec2(bx0 + w * 0.2f, by0 + h - 0.5f), ImVec2(bx0 + w * 0.5f, by0 + h - 0.5f), ImVec2(bx0 + w * 0.2f, by0 + h + tri * 0.4f), bubble);
      } else {
        dl->AddTriangleFilled(ImVec2(x1 - tri, y0), ImVec2(x1, y0), ImVec2(x1, y0 + tri), IM_COL32(0xC0, 0x00, 0x00, 255));
      }
    }
  }
  dl->PopClipRect();
}

}  // namespace

// ---------------------------------------------------------------------------

std::string FormatCellText(const engine::Snapshot& snap, const model::Style& st, const core::Value& v, double max_chars, bool* numeric, uint32_t* color,
                           bool* hashes) {
  if (formula::ImageSpec img; v.IsString() && formula::ParseImageValue(v.str(), &img)) return img.alt;  // IMAGE(): its alt text
  if (std::string cap; v.IsString() && formula::ParseCubeValue(v.str(), &cap)) return cap;            // CUBEMEMBER(): its caption
  const formula::NumberFormat& fmt = FormatFor(*snap.wb.styles, st.numfmt);
  formula::FormatResult fr = formula::FormatValue(v, fmt, max_chars);
  if (numeric) *numeric = fr.numeric || v.IsNumeric();
  if (color) *color = fr.color;
  if (hashes) *hashes = fr.hashes;
  return fr.text;
}

void SelectCell(GridView& view, uint32_t r, uint32_t c, bool extend) {
  Selection& s = view.sel;
  if (!extend) {
    s.anc_r = r;
    s.anc_c = c;
    s.areas.assign(1, Rect::Cell(r, c));
  }
  s.act_r = r;
  s.act_c = c;
  s.mode = SelectMode::Cells;
  s.areas.back() = Rect{std::min(s.anc_r, r), std::min(s.anc_c, c), std::max(s.anc_r, r), std::max(s.anc_c, c)};
}

void SelectRange(GridView& view, const Rect& r) {
  view.sel.areas.assign(1, r);
  view.sel.anc_r = r.r0;
  view.sel.anc_c = r.c0;
  view.sel.act_r = r.r0;
  view.sel.act_c = r.c0;
  view.sel.mode = SelectMode::Cells;
}

void NormalizeWholeSelection(GridView& view, const model::Sheet& sheet) {
  Selection& s = view.sel;
  if (s.areas.empty() || s.mode != SelectMode::Cells) return;
  bool rows_all = true, cols_all = true;  // every area spans all rows / all columns
  for (const Rect& a : s.areas) {
    rows_all = rows_all && a.r0 == 0 && a.r1 >= sheet.row_limit - 1;
    cols_all = cols_all && a.c0 == 0 && a.c1 >= sheet.col_limit - 1;
  }
  if (rows_all && cols_all) s.mode = SelectMode::All;
  else if (rows_all) s.mode = SelectMode::Cols;
  else if (cols_all) s.mode = SelectMode::Rows;
}

void EnsureVisible(const model::Sheet& sheet, GridView& view, uint32_t r, uint32_t c) {
  // the scale the grid was last drawn with: Dpi() outside a frame (scripts, Python requests run before NewFrame) can
  // be the unscaled 1.0, which made the window look wider than it is and left the cell off the screen
  const double sc = view.px_scale > 0 ? view.px_scale : static_cast<double>(Dpi()) * view.zoom;
  const ImRect& cells = view.cells_rect;
  const uint32_t fr = view.frozen_top + view.freeze_rows, fc = view.frozen_left + view.freeze_cols;
  // cells in the frozen area are always visible (split panes scroll like the main pane)
  if (view.split || view.freeze_rows == 0 || r >= fr) {
    const int64_t frozen_px = view.freeze_rows ? sheet.rows->Offset(fr) - sheet.rows->Offset(view.frozen_top) : 0;
    const int64_t view_b = static_cast<int64_t>(std::max(1.0f, cells.GetHeight()) / sc) - frozen_px;
    if (static_cast<int64_t>(r) < view.v.first) {
      view.v.first = r;
    } else {
      const int64_t bottom = sheet.rows->Offset(r + 1) - sheet.rows->Offset(view.v.first);
      if (bottom > view_b) {
        // minimal scrolling: make r the last row
        int64_t first = r;
        int64_t acc = sheet.rows->Size(r);
        while (first > static_cast<int64_t>(view.split ? 0 : fr) && acc + sheet.rows->Size(first - 1) <= view_b) acc += sheet.rows->Size(--first);
        view.v.first = first;
      }
    }
  }
  const bool tee = view.TLayout();
  const int64_t mir_px = tee ? static_cast<int64_t>(view.mirror_w / sc) : 0;
  if (tee && c < view.frozen_left) {
    // the T layout's left pane ends at column frozen_left - 1 - s, s being how far the main pane is scrolled
    int64_t s = std::max<int64_t>(0, view.h.first - fc);
    if (static_cast<int64_t>(c) > int64_t{view.frozen_left} - 1 - s) {
      s = int64_t{view.frozen_left} - 1 - c;  // hidden next to the frozen columns: it becomes the last one
    } else {
      // cut off at the window's left edge: scroll outward until it fits
      while (s < int64_t{view.frozen_left} - 1 - static_cast<int64_t>(c) &&
             sheet.cols->Offset(int64_t{view.frozen_left} - s) - sheet.cols->Offset(c) > mir_px)
        ++s;
    }
    view.h.first = fc + s;
  } else if (view.split || view.freeze_cols == 0 || c >= fc) {
    const int64_t frozen_px = view.freeze_cols ? sheet.cols->Offset(fc) - sheet.cols->Offset(view.frozen_left) : 0;
    const int64_t view_b = static_cast<int64_t>(std::max(1.0f, cells.GetWidth()) / sc) - frozen_px - mir_px;
    if (static_cast<int64_t>(c) < view.h.first) {
      view.h.first = c;
    } else {
      const int64_t right = sheet.cols->Offset(c + 1) - sheet.cols->Offset(view.h.first);
      if (right > view_b) {
        int64_t first = c;
        int64_t acc = sheet.cols->Size(c);
        while (first > static_cast<int64_t>(view.split ? 0 : fc) && acc + sheet.cols->Size(first - 1) <= view_b) acc += sheet.cols->Size(--first);
        view.h.first = first;
      }
    }
  }
}

namespace {

// AutoComplete leaves out a list's header (Excel 16, measured on 24 lists): the first row of the list a column's
// entries are in is one when, across the region around it, it holds text (or nothing) over a row with something else
// in some column, or formatted unlike that row, as Sort guesses a header. The cell being typed counts as empty.
bool ListHeaderRow(const model::Sheet& sheet, const model::StyleTable& styles, uint32_t top, uint32_t col, uint32_t edit_r) {
  if (uint64_t{top} + 1 >= sheet.row_limit) return false;
  const Rect reg = engine::CurrentRegion(sheet, top, col);
  bool any_text = false, second_other = false, styled = false;
  for (uint32_t c = reg.c0; c <= reg.c1 && c - reg.c0 < 256; ++c) {
    const core::Value& v1 = sheet.GetValue(top, c);
    if (!v1.IsString() && !v1.IsEmpty()) return false;  // a number in the first row: no header
    any_text = any_text || v1.IsString();
    const core::Value& v2 = sheet.GetValue(top + 1, c);
    second_other = second_other || (!(top + 1 == edit_r && c == col) && !v2.IsString() && !v2.IsEmpty());
    const model::Style& s1 = styles.Get(sheet.GetStyle(top, c));
    const model::Style& s2 = styles.Get(sheet.GetStyle(top + 1, c));
    styled = styled || s1.bold != s2.bold || s1.italic != s2.italic || s1.underline != s2.underline || s1.fill_color != s2.fill_color ||
             s1.font_color != s2.font_color;
  }
  return any_text && (second_other || styled);
}

// references in the formula (colored boxes while editing)
struct RefBox {
  Rect rect;
  ImU32 color;
  bool pointing = false;  // the reference being pointed with the mouse or arrow keys: draw a marching dashed box (like Excel)
  int begin = -1, end = -1;  // where its text is in the formula (a drag on the box rewrites that text)
};

}  // namespace

// the references of a formula's text in order, with where each is in the text: every word outside strings (sheet
// names, brackets and ranges included) that isn't a function name and parses as one reference
std::vector<FormulaRefSpan> FormulaRefSpans(const std::string& text, const formula::SheetResolver* res, core::CellPos origin, bool r1c1) {
  std::vector<FormulaRefSpan> out;
  // one color for each different reference (Excel: =A1+B1+A1 has two boxes, both "A1" blue)
  for (const formula::RefSpan& sp : formula::FindRefSpans(text, origin, res, r1c1))
    out.push_back({sp.begin, sp.end, sp.ref.sheet, sp.ref.rect, kRefColors[static_cast<size_t>(sp.group) % std::size(kRefColors)]});
  return out;
}

namespace {

std::vector<RefBox> FormulaRefBoxes(const engine::Snapshot& snap, EditState& edit, uint32_t sheet_id) {
  std::vector<RefBox> out;
  // "=", or "+" / "-" (keypad users start formulas so)
  if (!edit.active || edit.text.empty() || !(edit.text[0] == '=' || edit.text[0] == '+' || edit.text[0] == '-')) {
    if (!edit.active) edit.ref_spans.clear();
    return out;
  }
  engine::WorkbookResolver res(snap.wb);
  const core::CellPos origin{edit.r, edit.c};
  const std::vector<FormulaRefSpan> spans = FormulaRefSpans(edit.text, &res, origin, edit.r1c1);
  if (sheet_id == edit.sheet) {  // the edit box colors the text of each reference
    edit.ref_spans = spans;
    edit.ref_spans_text = edit.text;
  }
  for (const FormulaRefSpan& sp : spans) {
    const uint32_t sid = sp.sheet < 0 ? edit.sheet : static_cast<uint32_t>(sp.sheet);
    if (sid != sheet_id) continue;
    // one box per different reference
    bool dup = false;
    for (RefBox& o : out) {
      if (o.rect == sp.rect && o.color == sp.color) {
        dup = true;
        if (edit.ref_begin >= 0 && sp.begin == edit.ref_begin) o.pointing = true, o.begin = sp.begin, o.end = sp.end;
      }
    }
    if (dup) continue;
    RefBox b{sp.rect, sp.color};
    b.begin = sp.begin;
    b.end = sp.end;
    b.pointing = edit.ref_begin >= 0 && sp.begin == edit.ref_begin;
    out.push_back(b);
  }
  return out;
}


}  // namespace

// the sheet tabs, the add-sheet button and the splitter of the bottom row; returns where the horizontal scroll bar starts
static float DrawSheetTabs(TabBarIO* tabs, const ImRect& bottom, GridActions& act) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const Theme& t = T();
  ImGui::PushFont(F().ui, F().ui_size);
  const float ratio = tabs->split_ratio ? *tabs->split_ratio : 0.55f;
  const float tabs_w = std::clamp(bottom.GetWidth() * ratio, S(140), std::max(S(140), bottom.GetWidth() - S(120)));
  tabs->bar_rect = ImRect(bottom.Min, ImVec2(bottom.Min.x + tabs_w, bottom.Max.y));
  float x = bottom.Min.x + S(8);
  // left/right scroll arrows: scroll the tabs (index of the first tab shown)
  float* first = tabs->first_tab_scroll;
  for (int k = 0; k < 2; ++k) {
    const ImRect ar(ImVec2(x + S(16) * static_cast<float>(k), bottom.Min.y + S(4)), ImVec2(x + S(16) * static_cast<float>(k + 1), bottom.Max.y - S(4)));
    ImGui::SetCursorScreenPos(ar.Min);
    ImGui::PushID(1000 + k);
    if (ImGui::InvisibleButton("##tabscroll", ar.GetSize(), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight) && first &&
        !ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      // Ctrl+click: to the first / last sheet (Excel)
      const float last = static_cast<float>(std::max<size_t>(1, tabs->tabs.size()) - 1);
      *first = io.KeyCtrl ? (k == 0 ? 0.0f : last) : std::clamp(*first + (k == 0 ? -1.0f : 1.0f), 0.0f, last);
    }
    // right-click on the arrows: the Activate list of every sheet (Excel)
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) tabs->activate_list = true;
    ui::NoteUiItem(k == 0 ? "##tabscroll_left" : "##tabscroll_right", ar);
    if (ImGui::IsItemHovered()) dl->AddRectFilled(ar.Min, ar.Max, IM_COL32(0xE0, 0xE0, 0xE0, 255));
    ImGui::PopID();
    DrawArrow(dl, ar.GetCenter(), k == 0 ? 2 : 3, S(3.5f), t.scroll_arrow);
  }
  x += S(36);
  dl->PushClipRect(ImVec2(x, bottom.Min.y), ImVec2(bottom.Min.x + tabs_w - S(8), bottom.Max.y), true);
  int docked = 0;
  for (const auto& tb : tabs->tabs) docked += tb.floating ? 0 : 1;
  const size_t first_idx = first ? static_cast<size_t>(std::clamp(*first, 0.0f, static_cast<float>(tabs->tabs.size() ? tabs->tabs.size() - 1 : 0))) : 0;
  static int64_t drag_tab = -1;
  std::vector<ImRect> tab_rects(tabs->tabs.size());
  for (size_t i = first_idx; i < tabs->tabs.size(); ++i) {
    const SheetTab& tb = tabs->tabs[i];
    const std::string label = tb.floating ? tb.name + " ↗" : tb.name;
    const bool renaming = tabs->renaming == static_cast<int64_t>(tb.id) && tabs->rename_buf;
    const float tw = renaming ? std::max(ImGui::CalcTextSize(tabs->rename_buf).x + S(16), S(80)) : ImGui::CalcTextSize(label.c_str()).x;
    const ImRect tr(ImVec2(x, bottom.Min.y + 1), ImVec2(x + tw + S(24), bottom.Max.y - S(2)));
    tab_rects[i] = tr;
    if (renaming) {
      // the name is edited on the tab, as in Excel
      tabs->rename_drawn = true;
      dl->AddRectFilled(tr.Min, tr.Max, t.sheettab_active_bg);
      ImGui::SetCursorScreenPos(ImVec2(tr.Min.x + S(6), tr.GetCenter().y - ImGui::GetFrameHeight() * 0.5f));
      if (tabs->rename_focus) {
        ImGui::SetKeyboardFocusHere();
        tabs->rename_focus = false;
      }
      ImGui::SetNextItemWidth(tr.GetWidth() - S(12));
      ImGui::PushID(static_cast<int>(tb.id));
      const bool enter = ImGui::InputText("##tabname", tabs->rename_buf, tabs->rename_cap, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      ImGui::PopID();
      if (enter) tabs->rename_commit = true;
      else if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape)) tabs->rename_cancel = true;
      else if (ImGui::IsItemDeactivated()) tabs->rename_commit = true;  // a click elsewhere keeps the new name
      x = tr.Max.x + 1;
      continue;
    }
    ImGui::SetCursorScreenPos(tr.Min);
    ImGui::PushID(static_cast<int>(tb.id));
    ImGui::InvisibleButton("##sheettab", tr.GetSize(), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    // tests find a tab by its sheet's name (ui.items), only when a person can see it
    if (tr.Max.x <= bottom.Min.x + tabs_w - S(8)) ui::NoteUiItem((tb.name + "##sheettab").c_str(), tr);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = !tb.floating && tabs->active == tb.id;
    const bool grouped = std::find(tabs->selected.begin(), tabs->selected.end(), tb.id) != tabs->selected.end();
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (tb.floating) tabs->activate_floating = tb.id;
      else if (io.KeyCtrl && !io.KeyShift) tabs->group_toggle = tb.id;  // Ctrl+click: group / ungroup this sheet (Excel)
      else if (io.KeyShift && !io.KeyCtrl) tabs->group_extend = tb.id;  // Shift+click: the sheets from the active one to here
      else tabs->activate = tb.id;
      act.clicked = true;
      if (!io.KeyCtrl && !io.KeyShift) drag_tab = tb.id;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      // a grouped tab keeps the group (its menu offers Ungroup Sheets)
      if (!tb.floating && !grouped) tabs->activate = tb.id;
      tabs->context = tb.id;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (tb.floating) tabs->redock = tb.id;
      else tabs->rename = tb.id;
    }
    if (ImGui::IsItemActive() && !tb.floating && docked > 1 && io.MousePos.y < tabs->bar_rect.Min.y - S(16)) tabs->tear_off = tb.id;
    if (ImGui::IsItemActive() && !tb.floating) {
      const bool out = io.MousePos.y < tabs->bar_rect.Min.y - S(4);
      ImGui::SetMouseCursor(out && docked > 1 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow);
    }
    ImGui::PopID();
    // like Excel: an inactive tab with a color is filled with it (its name in black or white, whichever reads), the
    // active one shows the color as an underline
    ImU32 tab_text = 0;
    if (grouped && !active) {
      dl->AddRectFilled(tr.Min, tr.Max, t.sheettab_active_bg);  // a grouped sheet's tab is white too (Excel)
      tab_text = t.sheettab_active_text;
      if (tb.color) dl->AddRectFilled(ImVec2(tr.Min.x + S(8), tr.Max.y - S(3)), ImVec2(tr.Max.x - S(8), tr.Max.y), ToCol(tb.color), S(1.5f));
    } else if (tb.color && !active) {
      const ImU32 fill = ToCol(tb.color);
      dl->AddRectFilled(tr.Min, tr.Max, fill);
      const float lum = 0.299f * static_cast<float>(fill & 0xFF) + 0.587f * static_cast<float>((fill >> 8) & 0xFF) + 0.114f * static_cast<float>((fill >> 16) & 0xFF);
      tab_text = lum < 140.0f ? IM_COL32_WHITE : IM_COL32_BLACK;
    }
    if (active) {
      dl->AddRectFilled(tr.Min, tr.Max, t.sheettab_active_bg);
      dl->AddRectFilled(ImVec2(tr.Min.x + S(8), tr.Max.y - S(3)), ImVec2(tr.Max.x - S(8), tr.Max.y), tb.color ? ToCol(tb.color) : t.accent, S(1.5f));
    } else if (hovered && !tb.color && !grouped) {
      dl->AddRectFilled(tr.Min, tr.Max, IM_COL32(0xE6, 0xE6, 0xE6, 255));
    }
    ImFont* f = active ? F().ui_bold : F().ui;
    dl->AddText(f, ImGui::GetFontSize(), ImVec2(tr.Min.x + S(12), tr.GetCenter().y - ImGui::GetFontSize() * 0.5f),
                active ? t.sheettab_active_text : tab_text ? tab_text : (tb.floating ? IM_COL32(0x80, 0x80, 0x80, 255) : t.sheettab_text), label.c_str());
    dl->AddRectFilled(ImVec2(tr.Max.x, tr.Min.y + S(5)), ImVec2(tr.Max.x + 1, tr.Max.y - S(5)), t.sheettab_border);
    x = tr.Max.x + 1;
  }
  // the active sheet's tab is kept in view (Excel): a sheet activated by a command, Python or Ctrl+PgDn scrolls the tabs
  if (first && drag_tab < 0) {
    static uint32_t shown_active = 0;
    if (tabs->active != shown_active) {
      for (size_t i = 0; i < tabs->tabs.size(); ++i) {
        if (tabs->tabs[i].id != tabs->active) continue;
        if (i < first_idx) {
          *first = static_cast<float>(i);
        } else if (tab_rects[i].Max.x > bottom.Min.x + tabs_w - S(8) && i > first_idx) {
          *first += 1.0f;  // one tab a frame until it fits
          RequestRedrawIn(0.0);
          break;
        }
        shown_active = tabs->active;
      }
    }
  }
  // drag to reorder: which tab it is dropped on
  if (drag_tab >= 0) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (io.MousePos.y >= tabs->bar_rect.Min.y - S(4) && io.MouseDragMaxDistanceSqr[0] > S(6) * S(6)) {
        for (size_t i = first_idx; i < tab_rects.size(); ++i) {
          if (io.MousePos.x >= tab_rects[i].Min.x && io.MousePos.x < tab_rects[i].Max.x) {
            dl->AddTriangleFilled(ImVec2(tab_rects[i].Min.x - S(4), bottom.Min.y), ImVec2(tab_rects[i].Min.x + S(4), bottom.Min.y),
                                  ImVec2(tab_rects[i].Min.x, bottom.Min.y + S(6)), IM_COL32(0x30, 0x30, 0x30, 255));
          }
        }
        if (!tab_rects.empty() && io.MousePos.x >= tab_rects.back().Max.x && io.MousePos.x < tabs->bar_rect.Max.x) {
          const float ex = tab_rects.back().Max.x;
          dl->AddTriangleFilled(ImVec2(ex - S(4), bottom.Min.y), ImVec2(ex + S(4), bottom.Min.y), ImVec2(ex, bottom.Min.y + S(6)), IM_COL32(0x30, 0x30, 0x30, 255));
        }
        // Ctrl: a copy (Excel's page with a +)
        if (io.KeyCtrl) {
          const ImVec2 m = io.MousePos + ImVec2(S(12), S(10));
          dl->AddRectFilled(m, m + ImVec2(S(12), S(14)), IM_COL32_WHITE);
          dl->AddRect(m, m + ImVec2(S(12), S(14)), IM_COL32(0x30, 0x30, 0x30, 255));
          dl->AddText(m + ImVec2(S(2), 0), IM_COL32(0x30, 0x30, 0x30, 255), "+");
        }
      }
    } else {
      if (io.MousePos.y >= tabs->bar_rect.Min.y - S(4) && io.MouseDragMaxDistanceSqr[0] > S(6) * S(6)) {
        for (size_t i = first_idx; i < tab_rects.size(); ++i) {
          if (io.MousePos.x >= tab_rects[i].Min.x && io.MousePos.x < tab_rects[i].Max.x && (tabs->tabs[i].id != drag_tab || io.KeyCtrl)) {
            tabs->move_from = drag_tab;
            tabs->move_to = static_cast<int64_t>(i);
          }
        }
        // dropped past the last tab: it goes to the end
        if (tabs->move_from < 0 && !tab_rects.empty() && io.MousePos.x >= tab_rects.back().Max.x && io.MousePos.x < tabs->bar_rect.Max.x) {
          tabs->move_from = drag_tab;
          tabs->move_to = static_cast<int64_t>(tab_rects.size());
        }
        tabs->move_copy = tabs->move_from >= 0 && io.KeyCtrl;
      }
      drag_tab = -1;
    }
  }
  {
    const ImRect ar(ImVec2(x + S(4), bottom.Min.y + S(3)), ImVec2(x + S(26), bottom.Max.y - S(3)));
    ImGui::SetCursorScreenPos(ar.Min);
    if (ImGui::InvisibleButton("##addsheet", ar.GetSize())) tabs->add_sheet = true;
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(ar.Min, ar.Max, IM_COL32(0xE0, 0xE0, 0xE0, 255), S(11));
      ImGui::SetTooltip(TR("New Sheet (Shift+F11)"));
    }
    dl->AddCircle(ar.GetCenter(), S(8), t.scroll_arrow, 0, 1.2f);
    dl->AddRectFilled(ImVec2(ar.GetCenter().x - S(4), ar.GetCenter().y - S(0.6f)), ImVec2(ar.GetCenter().x + S(4), ar.GetCenter().y + S(0.6f)), t.scroll_arrow);
    dl->AddRectFilled(ImVec2(ar.GetCenter().x - S(0.6f), ar.GetCenter().y - S(4)), ImVec2(ar.GetCenter().x + S(0.6f), ar.GetCenter().y + S(4)), t.scroll_arrow);
  }
  dl->PopClipRect();
  const ImRect sp(ImVec2(bottom.Min.x + tabs_w - S(6), bottom.Min.y), ImVec2(bottom.Min.x + tabs_w + S(2), bottom.Max.y));
  ImGui::SetCursorScreenPos(sp.Min);
  ImGui::InvisibleButton("##splitter", sp.GetSize());
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActive() && tabs->split_ratio) *tabs->split_ratio = std::clamp(*tabs->split_ratio + io.MouseDelta.x / std::max(1.0f, bottom.GetWidth()), 0.15f, 0.85f);
  for (int i = 0; i < 3; ++i) dl->AddCircleFilled(ImVec2(sp.GetCenter().x, bottom.GetCenter().y + S(4) * static_cast<float>(i - 1)), S(1.1f), t.scroll_arrow);
  ImGui::PopFont();
  return sp.Max.x + S(2);
}

GridResult DrawGrid(const engine::Snapshot& snap, const model::Sheet& sheet, GridView& view, EditState& edit, const ImRect& outer,
                    const GridOptions& opt, TabBarIO* tabs) {
  // outline (grouped rows / columns): bars and buttons in a gutter left of the row numbers and above the column letters
  const int row_levels = opt.show_headings && opt.show_outline ? sheet.rows->MaxLevel() : 0, col_levels = opt.show_headings && opt.show_outline ? sheet.cols->MaxLevel() : 0;
  const float lvl_w = std::floor(S(16));
  const float gutter_w = row_levels ? lvl_w * static_cast<float>(row_levels + 1) + S(2) : 0.0f;
  const float gutter_h = col_levels ? lvl_w * static_cast<float>(col_levels + 1) + S(2) : 0.0f;
  const ImRect rect(ImVec2(outer.Min.x + gutter_w, outer.Min.y + gutter_h), outer.Max);
  GridResult result;
  GridActions& act = result.act;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const Theme& t = T();
  const double sc = static_cast<double>(Dpi()) * view.zoom;
  const float zoomf = view.zoom;
  const model::StyleTable& styles = *snap.wb.styles;
  const int64_t row_limit = sheet.row_limit, col_limit = sheet.col_limit;

  ImGui::PushID(static_cast<int>(sheet.id));

  // ---- Layout ----
  const float sb = opt.show_vscroll ? S(kScrollW) : 0.0f;
  const float bottom_h = tabs || opt.show_hscroll ? S(kBottomH) : 0.0f;  // the sheet tabs and the horizontal scroll bar's row
  const float header_h = opt.show_headings ? std::floor(S(kHeaderH) * zoomf) : 0.0f;
  ImGui::PushFont(F().grid, F().grid_size * zoomf);
  int digits = 1;
  for (int64_t v = view.last_bottom_row + 1; v >= 10; v /= 10) ++digits;
  digits = std::max(digits, 3);
  const float digit_w = ImGui::CalcTextSize("0").x * 0.82f;
  const float row_hdr_w = opt.show_headings ? std::floor(digit_w * static_cast<float>(digits) + S(14) * zoomf) : 0.0f;
  const ImRect cells(ImVec2(rect.Min.x + row_hdr_w, rect.Min.y + header_h), ImVec2(rect.Max.x - sb, rect.Max.y - bottom_h));
  view.cells_rect = cells;
  view.grid_rect = rect;
  view.px_scale = sc;
  if (opt.chart_sheet) {
    // a chart sheet: no cells; the overlay draws its chart over the whole area, then the tab row
    ImGui::PopFont();
    const ImRect area(outer.Min, ImVec2(outer.Max.x, outer.Max.y - bottom_h));
    dl->AddRectFilled(area.Min, area.Max, t.sheettab_bg);
    if (opt.overlay) {
      OverlayGeom g;
      g.cells = area;
      g.sc = sc;
      g.to_screen = [=](double x, double y) { return ImVec2(area.Min.x + static_cast<float>(x * sc), area.Min.y + static_cast<float>(y * sc)); };
      g.to_sheet = [=](ImVec2 p) { return ImVec2(static_cast<float>((p.x - area.Min.x) / sc), static_cast<float>((p.y - area.Min.y) / sc)); };
      ImGui::PushClipRect(area.Min, area.Max, true);
      opt.overlay(g);
      ImGui::PopClipRect();
    }
    const ImRect bottom(ImVec2(outer.Min.x, outer.Max.y - bottom_h), outer.Max);
    dl->AddRectFilled(bottom.Min, bottom.Max, t.sheettab_bg);
    dl->AddRectFilled(bottom.Min, ImVec2(bottom.Max.x, bottom.Min.y + 1), t.sheettab_border);
    if (tabs) DrawSheetTabs(tabs, bottom, act);
    ImGui::PopID();
    return result;
  }
  if (cells.GetWidth() < 4 || cells.GetHeight() < 4) {
    ImGui::PopFont();
    ImGui::PopID();
    return result;
  }

  // ---- Panes (up to 4 with frozen panes) ----
  const uint32_t fr_end = view.frozen_top + view.freeze_rows, fc_end = view.frozen_left + view.freeze_cols;
  // frozen panes: the main pane starts after them; split panes scroll on their own
  view.v.first = std::max<int64_t>(view.v.first, view.split ? 0 : fr_end);
  view.h.first = std::max<int64_t>(view.h.first, view.split ? 0 : fc_end);
  view.v.first = std::min<int64_t>(view.v.first, row_limit - 1);
  view.h.first = std::min<int64_t>(view.h.first, col_limit - 1);
  const float top_h = view.freeze_rows ? std::min(cells.GetHeight() - 4, static_cast<float>((sheet.rows->Offset(fr_end) - sheet.rows->Offset(view.frozen_top)) * sc)) : 0.0f;
  const float left_w = view.freeze_cols ? std::min(cells.GetWidth() - 4, static_cast<float>((sheet.cols->Offset(fc_end) - sheet.cols->Offset(view.frozen_left)) * sc)) : 0.0f;
  // T layout: a left pane for the columns before the frozen ones, as wide as they need up to half of what the frozen
  // columns leave (more when the right side needs less); its last column is the one next to the frozen columns,
  // moved outward as the main pane scrolls (one scroll bar for both sides)
  const bool tee = view.TLayout();
  float mir_w = 0.0f;
  int64_t mir_last = -1;
  if (tee) {
    const float avail = std::max(0.0f, cells.GetWidth() - std::floor(left_w) - 4);
    const float need_l = static_cast<float>(sheet.cols->Offset(view.frozen_left) * sc);
    const int64_t used_end = sheet.has_used ? std::max<int64_t>(fc_end, int64_t{sheet.used.c1} + 1) : fc_end;
    const float need_r = static_cast<float>((sheet.cols->Offset(used_end) - sheet.cols->Offset(fc_end)) * sc);
    mir_w = need_l + need_r <= avail ? need_l : std::clamp(avail * 0.5f, std::max(0.0f, avail - need_r), need_l);
    mir_w = std::floor(std::max(0.0f, mir_w));
    mir_last = int64_t{view.frozen_left} - 1 - (view.h.first - fc_end);
  }
  view.mirror_w = mir_w;
  std::vector<Pane> panes;
  Pane blank;  // the main pane when every row or every column in it is hidden: no cell to draw
  // region: 0 the T layout's left pane, 1 the frozen columns, 2 the main pane
  auto make_pane = [&](bool frozen_r, int region) {
    Pane p;
    const float xm = cells.Min.x + mir_w, xf = xm + std::floor(left_w);
    p.rect.Min.x = region == 0 ? cells.Min.x : region == 1 ? xm : xf;
    p.rect.Max.x = region == 0 ? xm : region == 1 ? xf : cells.Max.x;
    p.rect.Min.y = frozen_r ? cells.Min.y : cells.Min.y + std::floor(top_h);
    p.rect.Max.y = frozen_r ? cells.Min.y + std::floor(top_h) : cells.Max.y;
    p.first_row = frozen_r ? view.frozen_top : view.v.first;
    p.row_end = frozen_r ? fr_end : row_limit;
    p.scroll_rows = !frozen_r;
    Collect(*sheet.rows, p.first_row, p.row_end, p.rect.Min.y, p.rect.Max.y, sc, p.rows);
    if (region == 0) {
      // right-aligned against the frozen columns, whole columns only (a number cut at the window's left edge reads as
      // another number): the outermost column that doesn't fit is left out and its space stays empty
      int64_t first = mir_last;
      double acc = 0;
      while (first >= 0) {
        acc += sheet.cols->Size(first);
        if (acc * sc > mir_w + 0.5) {
          ++first;
          break;
        }
        if (first == 0) break;
        --first;
      }
      if (mir_last >= 0) {
        first = std::max<int64_t>(0, first);
        const double total = static_cast<double>(sheet.cols->Offset(mir_last + 1) - sheet.cols->Offset(first)) * sc;
        p.first_col = first;
        p.col_end = mir_last + 1;
        p.col_x = static_cast<float>(p.rect.Max.x - total);
        Collect(*sheet.cols, p.first_col, p.col_end, p.col_x, p.rect.Max.x, sc, p.cols);
      }
      p.scroll_cols = false;
    } else {
      p.first_col = region == 1 ? view.frozen_left : view.h.first;
      p.col_end = region == 1 ? fc_end : col_limit;
      p.scroll_cols = region == 2;
      p.col_x = p.rect.Min.x;
      Collect(*sheet.cols, p.first_col, p.col_end, p.rect.Min.x, p.rect.Max.x, sc, p.cols);
    }
    if (p.Valid()) panes.push_back(std::move(p));
    else if (region == 2 && !frozen_r) blank = std::move(p);
  };
  if (view.freeze_rows) {
    if (tee) make_pane(true, 0);
    if (view.freeze_cols) make_pane(true, 1);
    make_pane(true, 2);
  }
  if (tee) make_pane(false, 0);
  if (view.freeze_cols) make_pane(false, 1);
  make_pane(false, 2);
  // every row or every column hidden: no cell is drawn, but the keys still reach the active cell (Excel 16, measured:
  // typing, F2, Delete and Undo act on the hidden cell), and the headers, scroll bars and sheet tabs stay. The grid
  // returned here before, and no key reached the sheet
  const bool no_cells = panes.empty() || !panes.back().scroll_rows || !panes.back().scroll_cols;
  const Pane& main = no_cells ? blank : panes.back();
  // what the headers and the edit box go through: the panes drawn, and the blank main pane
  std::vector<const Pane*> all_panes;
  for (const Pane& p : panes) all_panes.push_back(&p);
  if (no_cells) {
    all_panes.push_back(&blank);
    dl->AddRectFilled(blank.rect.Min, blank.rect.Max, t.cell_bg);
  }
  // no shown row (column) at all: the keys that move the active cell have nowhere to go (Excel 16, measured)
  const bool no_rows = sheet.rows->Offset(row_limit) == 0, no_cols = sheet.cols->Offset(col_limit) == 0;
  view.last_bottom_row = main.rows.empty() ? main.first_row : main.rows.back().index;
  view.last_right_col = main.cols.empty() ? main.first_col : main.cols.back().index;
  view.vis_rows = static_cast<int64_t>(main.rows.size());
  view.vis_cols = static_cast<int64_t>(main.cols.size());

  Ctx ctx{snap, sheet, styles, view, opt, dl, sc, zoomf, std::max(1.0f, S(3) * zoomf), result.stats, {}};
  ctx.cf = opt.preview_rule && !opt.preview_rule->ranges.empty() ? CfPreviewFor(snap, sheet, *opt.preview_rule) : CfFor(snap, sheet);
  // merged areas intersecting the visible area of any pane
  for (const Rect& m : *sheet.merges) {
    for (const Pane& p : panes) {
      const Rect vis{static_cast<uint32_t>(p.rows.front().index), static_cast<uint32_t>(p.cols.front().index), static_cast<uint32_t>(p.rows.back().index),
                     static_cast<uint32_t>(p.cols.back().index)};
      if (m.Intersects(vis)) {
        ctx.merges.push_back(m);
        break;
      }
    }
  }

  // ---- Selection (expanded to fully contain intersecting merged areas) ----
  auto expand_merges = [&](Rect s) {
    for (bool changed = true; changed;) {
      changed = false;
      for (const Rect& m : *sheet.merges) {
        if (s.Intersects(m) && !s.Contains(m)) {
          s = s.Union(m);
          changed = true;
        }
      }
    }
    return s;
  };
  Selection& selst = view.sel;
  std::vector<Rect> sel_areas;
  if (selst.mode == SelectMode::Cells && !sheet.merges->empty()) {
    // the selection kept takes in the merged cells it touches, whole, however it was made (a drag, Shift+arrows, the
    // Name Box): Excel's Selection over A1 and a merged B2:C3 is A1:C3
    for (Rect& a : selst.areas) a = expand_merges(a);
  }
  for (Rect a : selst.areas) {
    if (selst.mode == SelectMode::Rows || selst.mode == SelectMode::All) {
      a.c0 = 0;
      a.c1 = static_cast<uint32_t>(col_limit - 1);
    }
    if (selst.mode == SelectMode::Cols || selst.mode == SelectMode::All) {
      a.r0 = 0;
      a.r1 = static_cast<uint32_t>(row_limit - 1);
    }
    sel_areas.push_back(selst.mode == SelectMode::Cells ? expand_merges(a) : a);
  }
  Rect act_cell = Rect::Cell(selst.act_r, selst.act_c);
  for (const Rect& mm : *sheet.merges) {
    if (mm.Contains(selst.act_r, selst.act_c)) act_cell = mm;
  }

  // ---- Drawing the panes ----
  for (const Pane& p : panes) DrawPane(ctx, p);

  // ---- Overlay: reference boxes, copy marquee, selection, fill preview ----
  const std::vector<RefBox> refboxes = FormulaRefBoxes(snap, edit, sheet.id);
  const bool editing_here = edit.active && edit.sheet == sheet.id && edit.view_id == opt.view_id;
  for (const Pane& p : panes) {
    const Rect vis{static_cast<uint32_t>(p.rows.front().index), static_cast<uint32_t>(p.cols.front().index), static_cast<uint32_t>(p.rows.back().index),
                   static_cast<uint32_t>(p.cols.back().index)};
    dl->PushClipRect(p.rect.Min, p.rect.Max, true);
    auto screen_rect = [&](const Rect& s) {
      const ImVec2 s0(s.c0 < vis.c0 ? p.rect.Min.x - S(4) : ColX(ctx, p, s.c0), s.r0 < vis.r0 ? p.rect.Min.y - S(4) : RowY(ctx, p, s.r0));
      const ImVec2 s1(s.c1 > vis.c1 ? p.rect.Max.x + S(4) : ColX(ctx, p, static_cast<int64_t>(s.c1) + 1), s.r1 > vis.r1 ? p.rect.Max.y + S(4) : RowY(ctx, p, static_cast<int64_t>(s.r1) + 1));
      return ImRect(s0, s1);
    };
    for (const RefBox& rb : refboxes) {
      if (!rb.rect.Intersects(vis)) continue;
      const ImRect sr = screen_rect(rb.rect);
      dl->AddRectFilled(sr.Min, sr.Max, (rb.color & 0x00FFFFFF) | 0x20000000);
      if (!rb.pointing) {
        dl->AddRect(ImVec2(sr.Min.x - 1, sr.Min.y - 1), sr.Max, rb.color, 0.0f, std::max(1.5f, S(1.5f)));
        // Excel's handles at the corners: drag one to resize the reference, drag the border to move it
        if (editing_here && rb.begin >= 0) {
          const float hs = std::max(2.0f, S(2.5f));
          for (const ImVec2 cp : {sr.Min, ImVec2(sr.Max.x, sr.Min.y), ImVec2(sr.Min.x, sr.Max.y), sr.Max})
            dl->AddRectFilled(ImVec2(cp.x - hs, cp.y - hs), ImVec2(cp.x + hs, cp.y + hs), rb.color);
        }
        continue;
      }
      // pointing: marching dashed box (like Excel's point mode)
      const float th = std::max(2.0f, std::floor(S(2.0f)));
      const float dash = S(4), off = std::fmod(static_cast<float>(ImGui::GetTime()) * S(16), dash * 2);
      auto ants = [&](ImVec2 a, ImVec2 b) {
        const bool hz = a.y == b.y;
        const float len = hz ? b.x - a.x : b.y - a.y;
        dl->AddRectFilled(hz ? ImVec2(a.x, a.y - th * 0.5f) : ImVec2(a.x - th * 0.5f, a.y), hz ? ImVec2(b.x, a.y + th * 0.5f) : ImVec2(a.x + th * 0.5f, b.y),
                          IM_COL32_WHITE);
        for (float d = -off; d < len; d += dash * 2) {
          const float d0 = std::max(0.0f, d), d1 = std::min(len, d + dash);
          if (d1 <= d0) continue;
          if (hz) dl->AddRectFilled(ImVec2(a.x + d0, a.y - th * 0.5f), ImVec2(a.x + d1, a.y + th * 0.5f), rb.color);
          else dl->AddRectFilled(ImVec2(a.x - th * 0.5f, a.y + d0), ImVec2(a.x + th * 0.5f, a.y + d1), rb.color);
        }
      };
      const ImVec2 p0(sr.Min.x - 0.5f, sr.Min.y - 0.5f), p1(sr.Max.x - 0.5f, sr.Max.y - 0.5f);
      ants(p0, ImVec2(p1.x, p0.y));
      ants(ImVec2(p0.x, p1.y), p1);
      ants(p0, ImVec2(p0.x, p1.y));
      ants(ImVec2(p1.x, p0.y), p1);
      RequestRedrawIn(0.08);
    }
    if (opt.copy_marquee && opt.copy_sheet == sheet.id && opt.copy_marquee->Intersects(vis)) {
      const ImRect sr = screen_rect(*opt.copy_marquee);
      // marching ants (Excel's copy marquee)
      const float dash = S(4), off = std::fmod(static_cast<float>(ImGui::GetTime()) * S(16), dash * 2);
      const ImU32 c1 = t.selection_border, c2 = IM_COL32_WHITE;
      auto ants = [&](ImVec2 a, ImVec2 b) {
        const bool hz = a.y == b.y;
        const float len = hz ? b.x - a.x : b.y - a.y;
        for (float s = -off; s < len; s += dash * 2) {
          const float s0 = std::max(0.0f, s), s1 = std::min(len, s + dash);
          if (s1 <= s0) continue;
          if (hz) {
            dl->AddRectFilled(ImVec2(a.x + s0, a.y - 1), ImVec2(a.x + s1, a.y + 1), c1);
          } else {
            dl->AddRectFilled(ImVec2(a.x - 1, a.y + s0), ImVec2(a.x + 1, a.y + s1), c1);
          }
        }
        (void)c2;
      };
      ants(ImVec2(sr.Min.x, sr.Min.y), ImVec2(sr.Max.x, sr.Min.y));
      ants(ImVec2(sr.Min.x, sr.Max.y - 1), ImVec2(sr.Max.x, sr.Max.y - 1));
      ants(ImVec2(sr.Min.x, sr.Min.y), ImVec2(sr.Min.x, sr.Max.y));
      ants(ImVec2(sr.Max.x - 1, sr.Min.y), ImVec2(sr.Max.x - 1, sr.Max.y));
      RequestRedrawIn(0.08);
    }
    // dynamic array: a thin blue border around the spill range of the active cell (like Excel)
    if (const model::SpillRange* sp = sheet.spills->empty() ? nullptr : sheet.SpillAt(selst.act_r, selst.act_c); sp && !sp->cse && sp->rect.Intersects(vis)) {
      const ImRect sr = screen_rect(sp->rect);
      dl->AddRect(ImVec2(sr.Min.x - 1, sr.Min.y - 1), sr.Max, t.spill_border, 0.0f, std::max(1.0f, S(1)));
    }
    // a #SPILL! formula selected: a dashed border around the range it wants to spill into (Excel)
    for (const auto& [k, want] : snap.spill_blocked) {
      if (engine::KeySheet(k) != sheet.id || engine::KeyRow(k) != selst.act_r || engine::KeyCol(k) != selst.act_c || !want.Intersects(vis)) continue;
      const ImRect sr = screen_rect(want);
      const float dash = S(4), th = std::max(1.0f, S(1));
      auto dashes = [&](ImVec2 a, ImVec2 b) {
        const bool hz = a.y == b.y;
        const float len = hz ? b.x - a.x : b.y - a.y;
        for (float d = 0; d < len; d += dash * 2) {
          const float d1 = std::min(len, d + dash);
          if (hz) dl->AddRectFilled(ImVec2(a.x + d, a.y - th * 0.5f), ImVec2(a.x + d1, a.y + th * 0.5f), t.spill_border);
          else dl->AddRectFilled(ImVec2(a.x - th * 0.5f, a.y + d), ImVec2(a.x + th * 0.5f, a.y + d1), t.spill_border);
        }
      };
      const ImVec2 p0(sr.Min.x - 0.5f, sr.Min.y - 0.5f), p1(sr.Max.x - 0.5f, sr.Max.y - 0.5f);
      dashes(p0, ImVec2(p1.x, p0.y));
      dashes(ImVec2(p0.x, p1.y), p1);
      dashes(p0, ImVec2(p0.x, p1.y));
      dashes(ImVec2(p1.x, p0.y), p1);
    }
    // selection: every area of a multiple selection gets the tint, only the active area gets the thick border and the fill handle
    for (size_t ai = 0; ai < sel_areas.size(); ++ai) {
      const Rect& s = sel_areas[ai];
      if (!s.Intersects(vis)) continue;
      const ImRect sr = screen_rect(s);
      const bool is_active = ai + 1 == sel_areas.size();
      const bool single = s == act_cell && sel_areas.size() == 1;
      if (!single) {
        if (is_active && act_cell.Intersects(vis)) {
          const ImRect ar = screen_rect(act_cell);
          dl->AddRectFilled(sr.Min, ImVec2(sr.Max.x, ar.Min.y), t.selection_fill);
          dl->AddRectFilled(ImVec2(sr.Min.x, ar.Max.y), sr.Max, t.selection_fill);
          dl->AddRectFilled(ImVec2(sr.Min.x, ar.Min.y), ImVec2(ar.Min.x, ar.Max.y), t.selection_fill);
          dl->AddRectFilled(ImVec2(ar.Max.x, ar.Min.y), ImVec2(sr.Max.x, ar.Max.y), t.selection_fill);
        } else {
          dl->AddRectFilled(sr.Min, sr.Max, t.selection_fill);
        }
      }
      if (is_active) {
        const float bw = std::max(2.0f, S(2));
        dl->AddRect(ImVec2(sr.Min.x - 1, sr.Min.y - 1), ImVec2(sr.Max.x, sr.Max.y), t.selection_border, 0.0f, bw);
        if (opt.cell_drag_drop && sel_areas.size() == 1 && !editing_here && s.r1 <= vis.r1 && s.c1 <= vis.c1) {
          const float fh = std::max(5.0f, S(6));
          dl->AddRectFilled(ImVec2(sr.Max.x - fh * 0.5f - 1, sr.Max.y - fh * 0.5f - 1), ImVec2(sr.Max.x + fh * 0.5f, sr.Max.y + fh * 0.5f), t.fill_handle_border);
          dl->AddRectFilled(ImVec2(sr.Max.x - fh * 0.5f, sr.Max.y - fh * 0.5f), ImVec2(sr.Max.x + fh * 0.5f - 1, sr.Max.y + fh * 0.5f - 1), t.selection_border);
        }
      } else if (!is_active) {
        dl->AddRect(ImVec2(sr.Min.x - 1, sr.Min.y - 1), sr.Max, t.selection_border, 0.0f, 1.0f);
      }
    }
    if (view.move_dragging && view.move_insert) {
      // Shift: Excel's gray insertion bar on the boundary the cells go in at
      const Rect& ms = view.move_src;
      const Rect bar = view.move_insert == 1 ? Rect{view.ins_r, view.ins_c, view.ins_r, view.ins_c + (ms.c1 - ms.c0)}
                                             : Rect{view.ins_r, view.ins_c, view.ins_r + (ms.r1 - ms.r0), view.ins_c};
      if (Rect{bar.r0 > 0 ? bar.r0 - 1 : 0, bar.c0 > 0 ? bar.c0 - 1 : 0, bar.r1, bar.c1}.Intersects(vis)) {
        const ImRect br = screen_rect(bar);
        const float th = std::max(3.0f, S(3));
        if (view.move_insert == 1) dl->AddRectFilled(ImVec2(br.Min.x, br.Min.y - th * 0.5f), ImVec2(br.Max.x, br.Min.y + th * 0.5f), IM_COL32(0x70, 0x70, 0x70, 255));
        else dl->AddRectFilled(ImVec2(br.Min.x - th * 0.5f, br.Min.y), ImVec2(br.Min.x + th * 0.5f, br.Max.y), IM_COL32(0x70, 0x70, 0x70, 255));
      }
    } else if (view.move_dragging && view.move_target.Intersects(vis) && !(view.move_target == view.move_src)) {
      // drag and drop: a thick gray box where the cells will go (like Excel)
      const ImRect mr = screen_rect(view.move_target);
      dl->AddRect(ImVec2(mr.Min.x - 1, mr.Min.y - 1), mr.Max, IM_COL32(0x70, 0x70, 0x70, 255), 0.0f, std::max(2.0f, S(2.5f)));
    }
    if (view.fill_dragging && view.fill_target.Intersects(vis)) {
      const ImRect fr = screen_rect(view.fill_target);
      // gray dashed box (AutoFill preview)
      const float dash = S(3);
      for (float x = fr.Min.x; x < fr.Max.x; x += dash * 2) {
        dl->AddRectFilled(ImVec2(x, fr.Min.y - 1), ImVec2(std::min(fr.Max.x, x + dash), fr.Min.y + 1), IM_COL32(0x60, 0x60, 0x60, 255));
        dl->AddRectFilled(ImVec2(x, fr.Max.y - 1), ImVec2(std::min(fr.Max.x, x + dash), fr.Max.y + 1), IM_COL32(0x60, 0x60, 0x60, 255));
      }
      for (float y = fr.Min.y; y < fr.Max.y; y += dash * 2) {
        dl->AddRectFilled(ImVec2(fr.Min.x - 1, y), ImVec2(fr.Min.x + 1, std::min(fr.Max.y, y + dash)), IM_COL32(0x60, 0x60, 0x60, 255));
        dl->AddRectFilled(ImVec2(fr.Max.x - 1, y), ImVec2(fr.Max.x + 1, std::min(fr.Max.y, y + dash)), IM_COL32(0x60, 0x60, 0x60, 255));
      }
    }
    dl->PopClipRect();
  }
  // freeze lines; split bars are wider and can be dragged (to the edge: no split)
  if (view.split) {
    const float bw = std::max(2.0f, S(4));
    const ImU32 bar = IM_COL32(0xB8, 0xB8, 0xB8, 255);
    if (view.freeze_rows) dl->AddRectFilled(ImVec2(cells.Min.x, cells.Min.y + std::floor(top_h) - bw * 0.5f), ImVec2(cells.Max.x, cells.Min.y + std::floor(top_h) + bw * 0.5f), bar);
    if (view.freeze_cols) dl->AddRectFilled(ImVec2(cells.Min.x + std::floor(left_w) - bw * 0.5f, cells.Min.y), ImVec2(cells.Min.x + std::floor(left_w) + bw * 0.5f, cells.Max.y), bar);
    const ImVec2 m = io.MousePos;
    const bool on_h = view.freeze_rows && std::fabs(m.y - (cells.Min.y + std::floor(top_h))) <= bw && m.x >= cells.Min.x && m.x <= cells.Max.x;
    const bool on_v = view.freeze_cols && std::fabs(m.x - (cells.Min.x + std::floor(left_w))) <= bw && m.y >= cells.Min.y && m.y <= cells.Max.y;
    if ((on_h || on_v) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_NoPopupHierarchy) && ImGui::IsMouseClicked(0)) view.split_drag = on_v ? 2 : 1;
    if (on_h || view.split_drag == 1) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (on_v || view.split_drag == 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (view.split_drag && ImGui::IsMouseDown(0)) {
      // snap to row / column boundaries from the top / left pane's first row / column
      if (view.split_drag == 1) {
        const double want = (m.y - cells.Min.y) / sc;
        uint32_t n = 0;
        double acc = 0;
        while (view.frozen_top + n < row_limit - 1 && acc + sheet.rows->Size(view.frozen_top + n) * 0.5 < want) acc += sheet.rows->Size(view.frozen_top + n++);
        view.freeze_rows = n;
      } else {
        const double want = (m.x - cells.Min.x) / sc;
        uint32_t n = 0;
        double acc = 0;
        while (view.frozen_left + n < col_limit - 1 && acc + sheet.cols->Size(view.frozen_left + n) * 0.5 < want) acc += sheet.cols->Size(view.frozen_left + n++);
        view.freeze_cols = n;
      }
    } else if (view.split_drag) {
      view.split_drag = 0;
      if (!view.freeze_rows && !view.freeze_cols) view.split = false;  // both bars dragged away
    }
  } else {
    if (view.freeze_rows) dl->AddRectFilled(ImVec2(cells.Min.x, cells.Min.y + std::floor(top_h) - 1), ImVec2(cells.Max.x, cells.Min.y + std::floor(top_h)), IM_COL32(0x80, 0x80, 0x80, 255));
    const float fx = cells.Min.x + mir_w + std::floor(left_w);
    if (view.freeze_cols) dl->AddRectFilled(ImVec2(fx - 1, cells.Min.y), ImVec2(fx, cells.Max.y), IM_COL32(0x80, 0x80, 0x80, 255));
    if (tee) dl->AddRectFilled(ImVec2(cells.Min.x + mir_w - 1, cells.Min.y), ImVec2(cells.Min.x + mir_w, cells.Max.y), IM_COL32(0x80, 0x80, 0x80, 255));
  }

  // ---- Headers ----
  auto row_selected = [&](int64_t r) {
    for (const Rect& a : sel_areas) {
      if (r >= a.r0 && r <= a.r1) return true;
    }
    return false;
  };
  auto col_selected = [&](int64_t c) {
    for (const Rect& a : sel_areas) {
      if (c >= a.c0 && c <= a.c1) return true;
    }
    return false;
  };
  if (opt.show_headings) {
    ImGui::PushFont(F().grid, (F().grid_size - 2.0f) * zoomf);
    const float hfont = ImGui::GetFontSize();
    char num[24];
    for (const Pane* pp : all_panes) {
      const Pane& p = *pp;
      if (p.rect.Min.y == cells.Min.y) {  // top pane: draw the column names
        dl->PushClipRect(ImVec2(p.rect.Min.x, rect.Min.y), ImVec2(p.rect.Max.x, cells.Min.y), true);
        dl->AddRectFilled(ImVec2(p.rect.Min.x, rect.Min.y), ImVec2(p.rect.Max.x, cells.Min.y), t.header_bg);
        for (const Line& l : p.cols) {
          const bool on = col_selected(l.index);
          const ImVec2 h0(l.pos, rect.Min.y), h1(l.pos + l.size, cells.Min.y);
          if (on) {
            dl->AddRectFilled(h0, h1, t.header_sel_bg);
            dl->AddRectFilled(ImVec2(h0.x, h1.y - std::max(2.0f, S(2))), h1, t.header_sel_bar);
          }
          dl->AddRectFilled(ImVec2(h1.x - 1, h0.y + S(3)), ImVec2(h1.x, h1.y), t.header_border);
          if (l.index > 0 && sheet.cols->IsHidden(l.index - 1)) {  // hidden columns before it: Excel's double line
            dl->AddRectFilled(ImVec2(h0.x - 1, h0.y), ImVec2(h0.x, h1.y), t.header_text);
            dl->AddRectFilled(ImVec2(h0.x + 1, h0.y), ImVec2(h0.x + 2, h1.y), t.header_text);
          }
          const std::string name = opt.r1c1 ? std::to_string(l.index + 1) : ColName(l.index);
          const float tw = ImGui::CalcTextSize(name.c_str()).x;
          dl->AddText(ImVec2(IM_ROUND(h0.x + (l.size - tw) * 0.5f), IM_ROUND(h0.y + (header_h - hfont) * 0.5f)), on ? t.header_sel_text : t.header_text, name.c_str());
        }
        dl->AddRectFilled(ImVec2(p.rect.Min.x, cells.Min.y - 1), ImVec2(p.rect.Max.x, cells.Min.y), t.header_border);
        dl->PopClipRect();
      }
      if (p.rect.Min.x == cells.Min.x) {  // leftmost pane: draw the row numbers
        dl->PushClipRect(ImVec2(rect.Min.x, p.rect.Min.y), ImVec2(cells.Min.x, p.rect.Max.y), true);
        dl->AddRectFilled(ImVec2(rect.Min.x, p.rect.Min.y), ImVec2(cells.Min.x, p.rect.Max.y), t.header_bg);
        for (const Line& l : p.rows) {
          const bool on = row_selected(l.index);
          const ImVec2 h0(rect.Min.x, l.pos), h1(cells.Min.x, l.pos + l.size);
          if (on) {
            dl->AddRectFilled(h0, h1, t.header_sel_bg);
            dl->AddRectFilled(ImVec2(h1.x - std::max(2.0f, S(2)), h0.y), h1, t.header_sel_bar);
          }
          dl->AddRectFilled(ImVec2(h0.x + S(3), h1.y - 1), ImVec2(h1.x, h1.y), t.header_border);
          if (l.index > 0 && sheet.rows->IsHidden(l.index - 1)) {  // hidden rows above it: a double line
            dl->AddRectFilled(ImVec2(h0.x, h0.y - 1), ImVec2(h1.x, h0.y), t.header_text);
            dl->AddRectFilled(ImVec2(h0.x, h0.y + 1), ImVec2(h1.x, h0.y + 2), t.header_text);
          }
          std::snprintf(num, sizeof(num), "%lld", static_cast<long long>(l.index + 1));
          const float tw = ImGui::CalcTextSize(num).x;
          // when AutoFilter hides rows, the row numbers inside the filter range are blue (like Excel)
          const auto& af = *sheet.autofilter;
          const bool filtered_row = af.on && !af.hidden.empty() && l.index > af.range.r0 && l.index <= af.range.r1;
          dl->AddText(ImVec2(IM_ROUND(h0.x + (row_hdr_w - tw) * 0.5f), IM_ROUND(h0.y + (l.size - hfont) * 0.5f)),
                      filtered_row ? IM_COL32(0x00, 0x70, 0xC0, 255) : (on ? t.header_sel_text : t.header_text), num);
        }
        dl->AddRectFilled(ImVec2(cells.Min.x - 1, p.rect.Min.y), ImVec2(cells.Min.x, p.rect.Max.y), t.header_border);
        dl->PopClipRect();
      }
    }
    const ImRect corner(rect.Min, ImVec2(cells.Min.x, cells.Min.y));
    dl->AddRectFilled(corner.Min, corner.Max, t.header_bg);
    dl->AddTriangleFilled(ImVec2(corner.Max.x - S(4), corner.Min.y + S(6)), ImVec2(corner.Max.x - S(4), corner.Max.y - S(4)),
                          ImVec2(corner.Min.x + S(6), corner.Max.y - S(4)), IM_COL32(0xB4, 0xB4, 0xB4, 255));
    ImGui::PopFont();
  }

  // ---- Outline gutters: level buttons, group bars, +/- buttons ----
  if (row_levels || col_levels) {
    const ImU32 line = IM_COL32(0x60, 0x60, 0x60, 255), btn_bg = IM_COL32_WHITE, btn_border = IM_COL32(0x90, 0x90, 0x90, 255);
    const float bs = std::floor(S(12));
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                       !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    ImGui::PushFont(F().grid, std::max(9.0f, (F().grid_size - 3.0f)));
    // ui.items lists each by an id a test finds it by, as a person finds it by its place: ##outline_rows_2 (the rows'
    // level 2 button), ##outline_row_5 (the +/- on row 5); drawn only, they weren't listed
    auto button = [&](ImVec2 center, const char* label, const std::string& id) {
      const ImRect b(ImVec2(std::floor(center.x - bs * 0.5f), std::floor(center.y - bs * 0.5f)), ImVec2(std::floor(center.x + bs * 0.5f), std::floor(center.y + bs * 0.5f)));
      ui::NoteUiItem(id.c_str(), b);
      const bool hot = b.Contains(io.MousePos);
      dl->AddRectFilled(b.Min, b.Max, hot ? t.btn_hover : btn_bg);
      dl->AddRect(b.Min, b.Max, btn_border);
      const ImVec2 ts = ImGui::CalcTextSize(label);
      dl->AddText(ImVec2(IM_ROUND(b.GetCenter().x - ts.x * 0.5f), IM_ROUND(b.GetCenter().y - ts.y * 0.5f)), t.header_text, label);
      return hot && click;
    };
    auto sign = [&](ImVec2 center, bool plus, const std::string& id) {
      const ImRect b(ImVec2(std::floor(center.x - bs * 0.5f), std::floor(center.y - bs * 0.5f)), ImVec2(std::floor(center.x + bs * 0.5f), std::floor(center.y + bs * 0.5f)));
      ui::NoteUiItem(id.c_str(), b);
      const bool hot = b.Contains(io.MousePos);
      dl->AddRectFilled(b.Min, b.Max, hot ? t.btn_hover : btn_bg);
      dl->AddRect(b.Min, b.Max, btn_border);
      const ImVec2 c = b.GetCenter();
      dl->AddRectFilled(ImVec2(b.Min.x + S(3), c.y - 0.5f), ImVec2(b.Max.x - S(3), c.y + 0.5f), line);
      if (plus) dl->AddRectFilled(ImVec2(c.x - 0.5f, b.Min.y + S(3)), ImVec2(c.x + 0.5f, b.Max.y - S(3)), line);
      return hot && click;
    };
    char lbl[4];
    if (row_levels) {
      const ImRect g(ImVec2(outer.Min.x, rect.Min.y), ImVec2(rect.Min.x, cells.Max.y));
      dl->AddRectFilled(g.Min, g.Max, t.header_bg);
      for (int k = 1; k <= row_levels + 1; ++k) {
        std::snprintf(lbl, sizeof(lbl), "%d", k);
        if (button(ImVec2(outer.Min.x + lvl_w * (static_cast<float>(k) - 0.5f), rect.Min.y + header_h * 0.5f), lbl, std::string("##outline_rows_") + lbl)) {
          act.outline_level = k;
          act.outline_level_rows = true;
        }
      }
      const auto& d = *sheet.rows;
      for (const Pane& p : panes) {
        if (p.rect.Min.x != cells.Min.x) continue;
        dl->PushClipRect(ImVec2(outer.Min.x, p.rect.Min.y), ImVec2(rect.Min.x, p.rect.Max.y), true);
        for (const Line& l : p.rows) {
          const int lv = d.Level(l.index), prev = l.index > 0 ? d.Level(l.index - 1) : 0, next = d.Level(l.index + 1);
          for (int k = 1; k <= row_levels; ++k) {
            const float x = outer.Min.x + lvl_w * (static_cast<float>(k) - 0.5f);
            if (lv >= k) dl->AddRectFilled(ImVec2(x - 0.5f, l.pos), ImVec2(x + 0.5f, l.pos + l.size), line);
            // the summary row after a group (before it when Outline Settings say summary rows are above): its +/- button
            if (lv < k && (sheet.outline_below ? prev >= k : next >= k)) {
              const auto gi = sheet.outline_below ? l.index - 1 : l.index + 1;
              const bool collapsed = d.IsHidden(gi);
              if (sign(ImVec2(x, l.pos + l.size * 0.5f), collapsed, "##outline_row_" + std::to_string(l.index + 1))) {
                act.outline_toggle = true;
                act.outline_rows = true;
                act.outline_index = static_cast<uint32_t>(gi);
                act.outline_toggle_level = k;
                act.outline_collapse = !collapsed;
              }
            }
          }
        }
        dl->PopClipRect();
      }
    }
    if (col_levels) {
      const ImRect g(ImVec2(rect.Min.x, outer.Min.y), ImVec2(cells.Max.x, rect.Min.y));
      dl->AddRectFilled(g.Min, g.Max, t.header_bg);
      for (int k = 1; k <= col_levels + 1; ++k) {
        std::snprintf(lbl, sizeof(lbl), "%d", k);
        if (button(ImVec2(rect.Min.x + row_hdr_w * 0.5f, outer.Min.y + lvl_w * (static_cast<float>(k) - 0.5f)), lbl, std::string("##outline_cols_") + lbl)) {
          act.outline_level = k;
          act.outline_level_rows = false;
        }
      }
      const auto& d = *sheet.cols;
      for (const Pane& p : panes) {
        if (p.rect.Min.y != cells.Min.y) continue;
        dl->PushClipRect(ImVec2(p.rect.Min.x, outer.Min.y), ImVec2(p.rect.Max.x, rect.Min.y), true);
        for (const Line& l : p.cols) {
          const int lv = d.Level(l.index), prev = l.index > 0 ? d.Level(l.index - 1) : 0, next = d.Level(l.index + 1);
          for (int k = 1; k <= col_levels; ++k) {
            const float y = outer.Min.y + lvl_w * (static_cast<float>(k) - 0.5f);
            if (lv >= k) dl->AddRectFilled(ImVec2(l.pos, y - 0.5f), ImVec2(l.pos + l.size, y + 0.5f), line);
            if (lv < k && (sheet.outline_right ? prev >= k : next >= k)) {
              const auto gi = sheet.outline_right ? l.index - 1 : l.index + 1;
              const bool collapsed = d.IsHidden(gi);
              if (sign(ImVec2(l.pos + l.size * 0.5f, y), collapsed, "##outline_col_" + core::ColumnName(static_cast<uint32_t>(l.index)))) {
                act.outline_toggle = true;
                act.outline_rows = false;
                act.outline_index = static_cast<uint32_t>(gi);
                act.outline_toggle_level = k;
                act.outline_collapse = !collapsed;
              }
            }
          }
        }
        dl->PopClipRect();
      }
    }
    if (row_levels && col_levels) dl->AddRectFilled(outer.Min, ImVec2(rect.Min.x, rect.Min.y), t.header_bg);
    ImGui::PopFont();
  }

  // ---- Hit testing ----
  auto pane_at = [&](ImVec2 m) -> const Pane* {
    for (const Pane& p : panes) {
      if (m.x >= p.rect.Min.x && m.x < p.rect.Max.x && m.y >= p.rect.Min.y && m.y < p.rect.Max.y) return &p;
    }
    // outside the grid: the nearest pane by coordinates (while drag-selecting)
    const Pane* best = &main;
    for (const Pane& p : panes) {
      const bool in_x = m.x >= p.rect.Min.x && m.x < p.rect.Max.x, in_y = m.y >= p.rect.Min.y && m.y < p.rect.Max.y;
      if (in_x || in_y) {
        if ((in_x && ((m.y < cells.Min.y && p.rect.Min.y == cells.Min.y) || (m.y >= cells.Max.y && p.rect.Max.y == cells.Max.y))) ||
            (in_y && ((m.x < cells.Min.x && p.rect.Min.x == cells.Min.x) || (m.x >= cells.Max.x && p.rect.Max.x == cells.Max.x))))
          best = &p;
      }
    }
    return best;
  };
  auto hit_row = [&](const Pane& p, float y) -> int64_t {
    if (p.rows.empty()) return std::clamp<int64_t>(p.first_row, 0, row_limit - 1);  // the blank main pane
    if (y < p.rect.Min.y) return p.scroll_rows ? std::max<int64_t>(fr_end, p.rows.front().index - 1) : p.rows.front().index;
    for (const Line& l : p.rows) {
      if (y < l.pos + l.size) return l.index;
    }
    return p.scroll_rows ? std::min(row_limit - 1, p.rows.back().index + 1) : p.rows.back().index;
  };
  auto hit_col = [&](const Pane& p, float x) -> int64_t {
    if (p.cols.empty()) return std::clamp<int64_t>(p.first_col, 0, col_limit - 1);
    if (x < p.rect.Min.x) return p.scroll_cols ? std::max<int64_t>(fc_end, p.cols.front().index - 1) : p.cols.front().index;
    for (const Line& l : p.cols) {
      if (x < l.pos + l.size) return l.index;
    }
    return p.scroll_cols ? std::min(col_limit - 1, p.cols.back().index + 1) : p.cols.back().index;
  };
  // cell under the mouse (across panes)
  auto cell_at = [&](ImVec2 m, uint32_t* r, uint32_t* c) {
    const Pane* p = pane_at(m);
    *r = static_cast<uint32_t>(hit_row(*p, m.y));
    *c = static_cast<uint32_t>(hit_col(*p, m.x));
  };

  // point mode: clicking a cell while typing a formula inserts a reference
  const bool pointing = edit.active && CanInsertRef(edit);
  // whole: 1 whole columns (B:D), 2 whole rows (3:5), as a header click inserts them
  auto insert_ref = [&](uint32_t r0, uint32_t c0, uint32_t r1, uint32_t c1, int whole = 0) {
    std::string ref;
    const Rect rr{std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)};
    const core::CellPos from{edit.r, edit.c};
    auto rc = [](char k, int64_t off) { return off == 0 ? std::string(1, k) : std::string(1, k) + "[" + std::to_string(off) + "]"; };
    if (whole == 1) {
      ref = edit.r1c1 ? rc('C', int64_t{rr.c0} - from.col) + (rr.c0 == rr.c1 ? std::string() : ":" + rc('C', int64_t{rr.c1} - from.col))
                      : core::ColumnName(rr.c0) + ":" + core::ColumnName(rr.c1);
    } else if (whole == 2) {
      ref = edit.r1c1 ? rc('R', int64_t{rr.r0} - from.row) + (rr.r0 == rr.r1 ? std::string() : ":" + rc('R', int64_t{rr.r1} - from.row))
                      : std::to_string(rr.r0 + 1) + ":" + std::to_string(rr.r1 + 1);
    } else if (edit.r1c1) {
      ref = formula::RangeR1C1(rr, &from);  // relative to the edited cell, like Excel's R1C1 point mode
    } else {
      // a colored box dragged to a new place keeps the $ signs of its reference
      bool ca0 = false, ra0 = false, ca1 = false, ra1 = false;
      if (edit.box_drag && !edit.box_orig.empty()) {
        std::string_view o = edit.box_orig;
        if (const size_t bang = o.rfind('!'); bang != std::string_view::npos) o = o.substr(bang + 1);
        const size_t colon = o.find(':');
        auto flags = [](std::string_view part, bool* ca, bool* ra) {
          *ca = !part.empty() && part[0] == '$';
          *ra = part.size() > 1 && part.find('$', 1) != std::string_view::npos;
        };
        flags(o.substr(0, colon), &ca0, &ra0);
        if (colon != std::string_view::npos) flags(o.substr(colon + 1), &ca1, &ra1);
        else ca1 = ca0, ra1 = ra0;
      }
      ref = rr.IsCell() ? core::CellA1(rr.r0, rr.c0, ra0, ca0) : core::CellA1(rr.r0, rr.c0, ra0, ca0) + ":" + core::CellA1(rr.r1, rr.c1, ra1, ca1);
    }
    if (sheet.id != edit.sheet) ref = core::QuoteSheetName(sheet.name) + "!" + ref;
    if (r0 == r1 && c0 == c1 && whole == 0 && opt.point_text) {
      if (std::string t = opt.point_text(sheet.id, r0, c0); !t.empty()) ref = std::move(t);
    }
    // Ctrl+click's comma: the edit box writes back its own text the frame the click takes its focus, dropping the
    // comma (and the reference) put in that frame; the drag goes on from the text with the comma again
    if (edit.comma_at >= 0 && edit.comma_at <= static_cast<int>(edit.text.size())) {
      if (edit.comma_at == static_cast<int>(edit.text.size()) || edit.text[static_cast<size_t>(edit.comma_at)] != ',') {
        edit.text.insert(static_cast<size_t>(edit.comma_at), ",");
      }
      edit.ref_begin = std::max(edit.ref_begin, edit.comma_at + 1);
      edit.ref_end = std::max(edit.ref_end, edit.ref_begin);
    }
    if (edit.ref_begin < 0) {
      edit.ref_begin = std::clamp(edit.caret, 0, static_cast<int>(edit.text.size()));
      edit.ref_end = edit.ref_begin;
    }
    // the text may have changed under the span (an edit committed or replaced meanwhile): stay inside it
    edit.ref_begin = std::clamp(edit.ref_begin, 0, static_cast<int>(edit.text.size()));
    edit.ref_end = std::clamp(edit.ref_end, edit.ref_begin, static_cast<int>(edit.text.size()));
    std::string next = edit.text;
    next.replace(static_cast<size_t>(edit.ref_begin), static_cast<size_t>(std::min<int>(edit.ref_end, static_cast<int>(next.size())) - edit.ref_begin), ref);
    edit.ref_end = edit.ref_begin + static_cast<int>(ref.size());
    SetEditText(edit, std::move(next), edit.ref_end);
    edit.focus_request = true;
  };

  // Ctrl+click while pointing: one more reference after a comma (=SUM(A1,B3)), as a multiple selection adds an area.
  // The comma goes into the text the reference is then written into (one change for the edit box), after the
  // reference pointed at (its span) or, when the span was let go, at the caret
  auto ctrl_comma = [&]() {
    if (!io.KeyCtrl || io.KeyShift) return;
    const int n = static_cast<int>(edit.text.size());
    const int at = edit.ref_begin >= 0 && edit.ref_end >= edit.ref_begin && edit.ref_end <= n ? edit.ref_end : std::clamp(edit.caret, 0, n);
    if (at > 0 && !std::strchr("=+-*/^&(,;:<> ", edit.text[static_cast<size_t>(at - 1)])) {
      edit.text.insert(static_cast<size_t>(at), ",");
      edit.ref_begin = edit.ref_end = at + 1;
      edit.comma_at = at;  // kept while the mouse is down (insert_ref)
    }
  };

  // ---- Mouse: cells ----
  // point mode: while typing a formula the edit box stays the active item, so the grid button gets no clicks -> read the mouse state directly
  bool pointed_now = false;
  const bool mouse_on_cells = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && cells.Contains(io.MousePos) && !panes.empty() &&
                              ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                              !(editing_here && edit.field_rect.Contains(io.MousePos));
  // Excel: a colored reference box has handles at its corners (drag: resize the reference) and a border (drag: move it)
  if (edit.active && !refboxes.empty() && mouse_on_cells) {
    const Pane* p = pane_at(io.MousePos);
    const float m = S(4), mx = io.MousePos.x, my = io.MousePos.y;
    for (auto it = refboxes.rbegin(); it != refboxes.rend() && p && p->Valid(); ++it) {
      const RefBox& rb = *it;
      if (rb.begin < 0 || rb.end > static_cast<int>(edit.text.size())) continue;
      const Rect vis{static_cast<uint32_t>(p->rows.front().index), static_cast<uint32_t>(p->cols.front().index), static_cast<uint32_t>(p->rows.back().index),
                     static_cast<uint32_t>(p->cols.back().index)};
      if (!rb.rect.Intersects(vis)) continue;
      const float x0 = rb.rect.c0 < vis.c0 ? -1e9f : ColX(ctx, *p, rb.rect.c0), x1 = rb.rect.c1 > vis.c1 ? 1e9f : ColX(ctx, *p, static_cast<int64_t>(rb.rect.c1) + 1);
      const float y0 = rb.rect.r0 < vis.r0 ? -1e9f : RowY(ctx, *p, rb.rect.r0), y1 = rb.rect.r1 > vis.r1 ? 1e9f : RowY(ctx, *p, static_cast<int64_t>(rb.rect.r1) + 1);
      const bool near_l = std::fabs(mx - x0) <= m, near_r = std::fabs(mx - x1) <= m, near_t = std::fabs(my - y0) <= m, near_b = std::fabs(my - y1) <= m;
      const bool in_x = mx >= x0 - m && mx <= x1 + m, in_y = my >= y0 - m && my <= y1 + m;
      const bool corner = (near_l || near_r) && (near_t || near_b);
      const bool border = !corner && ((in_y && (near_l || near_r)) || (in_x && (near_t || near_b)));
      if (!corner && !border) continue;
      uint32_t r, c;
      cell_at(io.MousePos, &r, &c);
      edit.ref_begin = rb.begin;
      edit.ref_end = rb.end;
      edit.box_orig = edit.text.substr(static_cast<size_t>(rb.begin), static_cast<size_t>(rb.end - rb.begin));
      edit.ref_whole = 0;
      if (corner) {  // the opposite corner stays
        edit.box_drag = 1;
        edit.ref_r0 = near_t ? rb.rect.r1 : rb.rect.r0;
        edit.ref_c0 = near_l ? rb.rect.c1 : rb.rect.c0;
      } else {
        edit.box_drag = 2;
        edit.box_grab_r = std::clamp(r, rb.rect.r0, rb.rect.r1) - rb.rect.r0;
        edit.box_grab_c = std::clamp(c, rb.rect.c0, rb.rect.c1) - rb.rect.c0;
        edit.box_h = rb.rect.r1 - rb.rect.r0;
        edit.box_w = rb.rect.c1 - rb.rect.c0;
      }
      edit.ref_dragging = true;
      edit.kpoint = false;
      edit.focus_request = true;
      pointed_now = true;
      act.clicked = true;
      break;
    }
  }
  if (pointing && !pointed_now && mouse_on_cells) {
    uint32_t r, c;
    cell_at(io.MousePos, &r, &c);
    if (edit.sheet != sheet.id || !(r == edit.r && c == edit.c)) {
      edit.ref_whole = 0;
      edit.box_drag = 0;
      if (io.KeyShift && edit.kpoint && edit.ref_begin >= 0) {
        // Shift+click: the reference pointed at grows from its anchor to the clicked cell (A1 -> A1:A5)
        edit.ref_dragging = true;
        edit.ref_r0 = edit.kp_ar;
        edit.ref_c0 = edit.kp_ac;
        edit.kp_r = r;
        edit.kp_c = c;
        insert_ref(edit.kp_ar, edit.kp_ac, r, c);
      } else {
        ctrl_comma();
        edit.ref_dragging = true;
        edit.ref_r0 = r;
        edit.ref_c0 = c;
        edit.kpoint = true;
        edit.kp_r = edit.kp_ar = r;
        edit.kp_c = edit.kp_ac = c;
        insert_ref(r, c, r, c);
      }
      pointed_now = true;
      act.clicked = true;
    }
  }
  if (edit.ref_dragging && !edit.active) edit.ref_dragging = false;
  if (!edit.active || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (!pointed_now) edit.comma_at = -1;  // the Ctrl+click is over
  }
  if (edit.ref_dragging && !pointed_now && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    uint32_t r, c;
    cell_at(io.MousePos, &r, &c);
    if (edit.ref_whole == 1) {
      const Pane* hp = pane_at(ImVec2(io.MousePos.x, cells.Min.y + 1));
      insert_ref(0, edit.ref_c0, 0, static_cast<uint32_t>(hit_col(*hp, io.MousePos.x)), 1);
    } else if (edit.ref_whole == 2) {
      const Pane* hp = pane_at(ImVec2(cells.Min.x + 1, io.MousePos.y));
      insert_ref(edit.ref_r0, 0, static_cast<uint32_t>(hit_row(*hp, io.MousePos.y)), 0, 2);
    } else if (edit.box_drag == 2) {
      // a box taller or wider than the sheet's cells (A:A on a sheet with fewer rows) stays where it starts: a negative
      // upper bound made std::clamp fail in a debug build (chaos seed 5099)
      const uint32_t nr = static_cast<uint32_t>(std::clamp<int64_t>(int64_t{r} - edit.box_grab_r, 0, std::max<int64_t>(0, row_limit - 1 - edit.box_h)));
      const uint32_t nc = static_cast<uint32_t>(std::clamp<int64_t>(int64_t{c} - edit.box_grab_c, 0, std::max<int64_t>(0, col_limit - 1 - edit.box_w)));
      insert_ref(nr, nc, nr + edit.box_h, nc + edit.box_w);
    } else {
      edit.kp_r = r;
      edit.kp_c = c;
      insert_ref(edit.ref_r0, edit.ref_c0, r, c);
    }
    // dragged outside the grid: autoscroll
    const float ddy = io.MousePos.y < cells.Min.y ? io.MousePos.y - cells.Min.y : (io.MousePos.y > cells.Max.y ? io.MousePos.y - cells.Max.y : 0.0f);
    const float ddx = io.MousePos.x < cells.Min.x ? io.MousePos.x - cells.Min.x : (io.MousePos.x > cells.Max.x ? io.MousePos.x - cells.Max.x : 0.0f);
    if (ddy != 0.0f) ScrollBy(view.v, *sheet.rows, view.split ? 0 : fr_end, row_limit, static_cast<int64_t>(ddy / S(20)) + (ddy > 0 ? 1 : -1));
    if (ddx != 0.0f) ScrollBy(view.h, *sheet.cols, view.split ? 0 : fc_end, col_limit, static_cast<int64_t>(ddx / S(60)) + (ddx > 0 ? 1 : -1));
    if (ddx != 0.0f || ddy != 0.0f) RequestRedrawIn(0.03);
  }

  ImGui::SetCursorScreenPos(cells.Min);
  if (opt.overlay) ImGui::SetNextItemAllowOverlap();
  // no cell drawn at all: nothing under the mouse to click, select or drag
  if (panes.empty()) ImGui::BeginDisabled();
  ImGui::InvisibleButton("##cells", cells.GetSize(), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (panes.empty()) ImGui::EndDisabled();
  const bool cells_hovered = ImGui::IsItemHovered() && !view.split_drag;  // dragging a split bar doesn't select cells
  // fill handle hit (bottom-right corner of the selection)
  bool over_fill_handle = false;
  if (opt.cell_drag_drop && cells_hovered && selst.areas.size() == 1 && !edit.active) {
    const Rect& s = sel_areas.back();
    for (const Pane& p : panes) {
      const Rect vis{static_cast<uint32_t>(p.rows.front().index), static_cast<uint32_t>(p.cols.front().index), static_cast<uint32_t>(p.rows.back().index),
                     static_cast<uint32_t>(p.cols.back().index)};
      if (s.r1 > vis.r1 || s.c1 > vis.c1 || s.r1 < vis.r0 || s.c1 < vis.c0) continue;
      const ImVec2 hp(ColX(ctx, p, static_cast<int64_t>(s.c1) + 1), RowY(ctx, p, static_cast<int64_t>(s.r1) + 1));
      if (std::fabs(io.MousePos.x - hp.x) <= S(4) && std::fabs(io.MousePos.y - hp.y) <= S(4)) over_fill_handle = true;
    }
  }
  // the border of the selection: drag it to move the cells (Ctrl: copy)
  bool over_border = false;
  int border_edge = 0;  // the edge under the mouse: 1 top, 2 bottom, 3 left, 4 right (a double-click jumps that way)
  if (opt.cell_drag_drop && cells_hovered && !over_fill_handle && selst.areas.size() == 1 && !edit.active && !pointing && selst.mode != SelectMode::All) {
    const Rect& s = sel_areas.back();
    for (const Pane& p : panes) {
      const Rect vis{static_cast<uint32_t>(p.rows.front().index), static_cast<uint32_t>(p.cols.front().index), static_cast<uint32_t>(p.rows.back().index),
                     static_cast<uint32_t>(p.cols.back().index)};
      if (!s.Intersects(vis) || !p.rect.Contains(io.MousePos)) continue;
      const float x0 = s.c0 < vis.c0 ? -1e9f : ColX(ctx, p, s.c0), x1 = s.c1 > vis.c1 ? 1e9f : ColX(ctx, p, static_cast<int64_t>(s.c1) + 1);
      const float y0 = s.r0 < vis.r0 ? -1e9f : RowY(ctx, p, s.r0), y1 = s.r1 > vis.r1 ? 1e9f : RowY(ctx, p, static_cast<int64_t>(s.r1) + 1);
      const float m = S(3), mx = io.MousePos.x, my = io.MousePos.y;
      const bool in_x = mx >= x0 - m && mx <= x1 + m, in_y = my >= y0 - m && my <= y1 + m;
      if ((in_y && (std::fabs(mx - x0) <= m || std::fabs(mx - x1) <= m)) || (in_x && (std::fabs(my - y0) <= m || std::fabs(my - y1) <= m))) {
        over_border = true;
        if (in_x && std::fabs(my - y0) <= m) border_edge = 1;
        else if (in_x && std::fabs(my - y1) <= m) border_edge = 2;
        else if (std::fabs(mx - x0) <= m) border_edge = 3;
        else border_edge = 4;
      }
    }
  }
  // Excel's cursors: the white cross over cells (selecting or pointing), the thin black cross on the fill handle, the
  // four-headed arrow on the selection's border
  if (cells_hovered || (ImGui::IsItemActive() && view.dragging)) ImGui::SetMouseCursor(kCursorCell);
  if (over_fill_handle || view.fill_dragging) ImGui::SetMouseCursor(kCursorFill);
  if (over_border || view.move_dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  // Escape during a drag of the border or the fill handle cancels it: nothing happens when the button goes up (Excel)
  if ((view.move_dragging || view.fill_dragging) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    view.move_dragging = view.fill_dragging = false;
    view.move_insert = 0;
    view.right_drag = false;
  }
  // Ctrl while dragging: a copy (the border) or the other kind of fill (the fill handle); Excel's cursor gets a small "+"
  if ((view.move_dragging || view.fill_dragging) && io.KeyCtrl && !view.move_insert) {
    const ImVec2 m = io.MousePos + ImVec2(S(9), -S(12));
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(m - ImVec2(S(1), S(1)), m + ImVec2(S(8), S(8)), IM_COL32_WHITE);
    fg->AddRectFilled(m + ImVec2(S(3), 0), m + ImVec2(S(4.5f), S(7)), IM_COL32_BLACK);
    fg->AddRectFilled(m + ImVec2(0, S(3)), m + ImVec2(S(7), S(4.5f)), IM_COL32_BLACK);
  }
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    uint32_t r, c;
    cell_at(io.MousePos, &r, &c);
    act.clicked = true;
    if (over_fill_handle && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      // a double-click on the fill handle fills down as far as the data in the column beside it go (Excel: the
      // column on the left, else the one on the right), stopping before a cell that already holds something
      const Rect s = sel_areas.back();
      auto run_end = [&](int64_t col) -> int64_t {
        if (col < 0 || col >= col_limit) return -1;
        const uint32_t cc = static_cast<uint32_t>(col);
        int64_t end = -1;
        for (int64_t rr = int64_t{s.r1} + 1; rr < row_limit && !sheet.IsEmptyCell(static_cast<uint32_t>(rr), cc); ++rr) end = rr;
        return end;
      };
      int64_t end = run_end(int64_t{s.c0} - 1);
      if (end < 0) end = run_end(int64_t{s.c1} + 1);
      for (int64_t rr = int64_t{s.r1} + 1; rr <= end; ++rr) {
        bool filled = false;
        for (uint32_t cc = s.c0; cc <= s.c1 && !filled; ++cc) filled = !sheet.IsEmptyCell(static_cast<uint32_t>(rr), cc);
        if (filled) {
          end = rr - 1;
          break;
        }
      }
      view.fill_dragging = false;
      if (end > static_cast<int64_t>(s.r1)) {
        const Rect dest{s.r0, s.c0, static_cast<uint32_t>(end), s.c1};
        act.autofill = true;
        act.autofill_src = s;
        act.autofill_dest = dest;
        selst.areas.back() = dest;
        act.selection_changed = true;
      }
    } else if (over_fill_handle) {
      view.fill_dragging = true;
      view.fill_target = sel_areas.back();
    } else if (over_border && border_edge && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      // Excel: a double-click on the selection's edge jumps like Ctrl+arrow that way (Shift: and selects up to there)
      view.move_dragging = false;
      const bool rows = border_edge <= 2;
      const int dir = (border_edge == 2 || border_edge == 4) ? +1 : -1;
      const uint32_t fr = selst.act_r, fc = selst.act_c;
      const int64_t to = engine::CtrlJump(sheet, fr, fc, rows, dir);
      const uint32_t nr = rows ? static_cast<uint32_t>(std::clamp<int64_t>(to, 0, row_limit - 1)) : fr;
      const uint32_t nc = rows ? fc : static_cast<uint32_t>(std::clamp<int64_t>(to, 0, col_limit - 1));
      if (io.KeyShift) {
        const Rect a = selst.areas.back();
        selst.areas.back() = Rect{std::min(a.r0, nr), std::min(a.c0, nc), std::max(a.r1, nr), std::max(a.c1, nc)};
      } else {
        SelectCell(view, nr, nc, false);
      }
      EnsureVisible(sheet, view, nr, nc);
      act.selection_changed = true;
    } else if (over_border) {
      const Rect s = sel_areas.back();
      view.move_dragging = true;
      view.move_src = view.move_target = s;
      view.move_grab_r = std::clamp(r, s.r0, s.r1) - s.r0;
      view.move_grab_c = std::clamp(c, s.c0, s.c1) - s.c0;
    } else if (pointed_now) {
      // already handled above
    } else if (pointing && (edit.sheet != sheet.id || !(r == edit.r && c == edit.c))) {
      edit.ref_dragging = true;
      edit.ref_r0 = r;
      edit.ref_c0 = c;
      ctrl_comma();
      insert_ref(r, c, r, c);
    } else {
      if (edit.active) {
        act.commit_edit = true;
      }
      if (opt.selectable && !opt.selectable(r, c)) {
        // a protected sheet that doesn't let this cell be selected: the click does nothing (Excel)
      } else if ((io.KeyCtrl || view.add_mode) && !io.KeyShift) {
        // add a selection area (Ctrl+click, or Shift+F8's Add to Selection); a Ctrl+click on selected cells takes
        // them out of the selection when the button goes up (Excel)
        bool on_selected = false;
        for (const Rect& sa : sel_areas) on_selected = on_selected || sa.Contains(r, c);
        view.deselecting = on_selected && io.KeyCtrl && !view.add_mode;
        selst.areas.push_back(Rect::Cell(r, c));
        selst.anc_r = r;
        selst.anc_c = c;
        selst.act_r = r;
        selst.act_c = c;
        selst.mode = SelectMode::Cells;
      } else {
        SelectCell(view, r, c, io.KeyShift || view.extend_mode);
      }
      if (!opt.selectable || opt.selectable(r, c)) {
        view.dragging = true;
        act.selection_changed = true;
      }
    }
  }
  // (a double-click on the selection's edge jumps, above; it doesn't also edit the cell it lands on)
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !over_fill_handle && !over_border && !pointing &&
      [&] {
        uint32_t r, c;
        cell_at(io.MousePos, &r, &c);
        return !opt.selectable || opt.selectable(r, c);
      }()) {
    act.double_click_cell = true;
    act.click_x = io.MousePos.x;
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && (over_fill_handle || over_border) && !view.move_dragging && !view.fill_dragging) {
    uint32_t r, c;
    cell_at(io.MousePos, &r, &c);
    const Rect s = sel_areas.back();
    view.right_drag = true;
    act.clicked = true;
    if (over_fill_handle) {
      view.fill_dragging = true;
      view.fill_target = s;
    } else {
      view.move_dragging = true;
      view.move_src = view.move_target = s;
      view.move_grab_r = std::clamp(r, s.r0, s.r1) - s.r0;
      view.move_grab_c = std::clamp(c, s.c0, s.c1) - s.c0;
    }
  } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
    uint32_t r, c;
    cell_at(io.MousePos, &r, &c);
    bool inside = false;
    for (const Rect& a : sel_areas) inside = inside || a.Contains(r, c);
    if (!inside && (!opt.selectable || opt.selectable(r, c))) {
      SelectCell(view, r, c, false);
      act.selection_changed = true;
    }
    act.context_cells = true;
    act.clicked = true;
  }
  const ImGuiMouseButton drag_btn = view.right_drag ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
  const bool cells_active = ImGui::IsItemActive() && ImGui::IsMouseDown(drag_btn);
  if (cells_active) {
    uint32_t r, c;
    cell_at(io.MousePos, &r, &c);
    if (view.move_dragging) {
      const Rect s = view.move_src;
      const uint32_t nr = static_cast<uint32_t>(std::clamp<int64_t>(int64_t{r} - view.move_grab_r, 0, std::max<int64_t>(0, row_limit - static_cast<int64_t>(s.Rows()))));
      const uint32_t nc = static_cast<uint32_t>(std::clamp<int64_t>(int64_t{c} - view.move_grab_c, 0, std::max<int64_t>(0, col_limit - static_cast<int64_t>(s.Cols()))));
      view.move_target = Rect{nr, nc, nr + s.r1 - s.r0, nc + s.c1 - s.c0};
      // Shift: insert between rows or columns, at the cell boundary nearest the mouse (Excel); whole rows / columns
      // always go between rows / columns
      view.move_insert = 0;
      if (io.KeyShift && !view.right_drag) {
        const Pane* mp = pane_at(io.MousePos);
        const float x0 = ColX(ctx, *mp, c), x1 = ColX(ctx, *mp, int64_t{c} + 1), y0 = RowY(ctx, *mp, r), y1 = RowY(ctx, *mp, int64_t{r} + 1);
        const float dt = io.MousePos.y - y0, db = y1 - io.MousePos.y, dlf = io.MousePos.x - x0, drt = x1 - io.MousePos.x;
        const bool full_rows = s.c0 == 0 && s.c1 >= col_limit - 1, full_cols = s.r0 == 0 && s.r1 >= row_limit - 1;
        const bool between_rows = full_rows ? true : full_cols ? false : std::min(dt, db) <= std::min(dlf, drt);
        if (between_rows) {
          view.move_insert = 1;
          view.ins_r = static_cast<uint32_t>(std::min<int64_t>(dt <= db ? r : int64_t{r} + 1, row_limit - 1));
          view.ins_c = nc;
        } else {
          view.move_insert = 2;
          view.ins_c = static_cast<uint32_t>(std::min<int64_t>(dlf <= drt ? c : int64_t{c} + 1, col_limit - 1));
          view.ins_r = nr;
        }
        // at the cells' own place it changes nothing: no bar
        const bool own = between_rows ? (view.ins_r >= s.r0 && view.ins_r <= s.r1 + 1 && view.ins_c == s.c0)
                                      : (view.ins_c >= s.c0 && view.ins_c <= s.c1 + 1 && view.ins_r == s.r0);
        if (own) view.move_insert = 0, view.move_target = s;
      }
      // Excel's tip: where the cells go (the address of the drop)
      if (!(view.move_target == s) || view.move_insert) {
        Rect at = view.move_target;
        if (view.move_insert == 1) at = Rect{view.ins_r, view.ins_c, view.ins_r + s.r1 - s.r0, view.ins_c + s.c1 - s.c0};
        if (view.move_insert == 2) at = Rect{view.ins_r, view.ins_c, view.ins_r + s.r1 - s.r0, view.ins_c + s.c1 - s.c0};
        const bool full_rows = at.c0 == 0 && at.c1 >= col_limit - 1, full_cols = at.r0 == 0 && at.r1 >= row_limit - 1;
        std::string addr;
        if (full_rows) addr = std::to_string(at.r0 + 1) + ":" + std::to_string(at.r1 + 1);
        else if (full_cols) {
          const std::string a0 = core::CellA1(0, at.c0), a1 = core::CellA1(0, at.c1);
          addr = a0.substr(0, a0.size() - 1) + ":" + a1.substr(0, a1.size() - 1);
        } else addr = core::CellA1(at.r0, at.c0) + (at.IsCell() ? std::string() : ":" + core::CellA1(at.r1, at.c1));
        ImGui::SetTooltip("%s", addr.c_str());
      }
    } else if (view.fill_dragging) {
      // extends in one direction only (the one farther from the selection)
      const Rect s = sel_areas.back();
      Rect tgt = s;
      const int64_t dr = r > s.r1 ? int64_t{r} - s.r1 : (r < s.r0 ? int64_t{r} - s.r0 : 0);
      const int64_t dc = c > s.c1 ? int64_t{c} - s.c1 : (c < s.c0 ? int64_t{c} - s.c0 : 0);
      if (dr == 0 && dc == 0) {
        // back inside the selection (Excel): the cells after the mouse are cleared on release
        if (int64_t{s.r1} - r >= int64_t{s.c1} - c) tgt.r1 = r;
        else tgt.c1 = c;
      } else if (std::llabs(dr) >= std::llabs(dc)) {
        if (dr > 0) tgt.r1 = r;
        else if (dr < 0) tgt.r0 = r;
      } else {
        if (dc > 0) tgt.c1 = c;
        else if (dc < 0) tgt.c0 = c;
      }
      view.fill_target = tgt;
      if (opt.fill_tip && tgt.Contains(s) && !(tgt == s) && !io.KeyShift) {
        if (!(view.fill_tip_for == tgt) || view.fill_tip_ctrl != io.KeyCtrl) {
          view.fill_tip_for = tgt;
          view.fill_tip_ctrl = io.KeyCtrl;
          view.fill_tip = opt.fill_tip(s, tgt, io.KeyCtrl);
        }
        if (!view.fill_tip.empty()) ImGui::SetTooltip("%s", view.fill_tip.c_str());
      }
    } else if (edit.ref_dragging) {
      insert_ref(edit.ref_r0, edit.ref_c0, r, c);
    } else if (view.dragging) {
      selst.act_r = r;
      selst.act_c = c;
      selst.areas.back() = Rect{std::min(selst.anc_r, r), std::min(selst.anc_c, c), std::max(selst.anc_r, r), std::max(selst.anc_c, c)};
      // the active cell stays at the anchor while dragging (Excel behavior)
      selst.act_r = selst.anc_r;
      selst.act_c = selst.anc_c;
      act.selection_changed = true;
    }
    const float dy = io.MousePos.y < cells.Min.y ? io.MousePos.y - cells.Min.y : (io.MousePos.y > cells.Max.y ? io.MousePos.y - cells.Max.y : 0.0f);
    const float dx = io.MousePos.x < cells.Min.x ? io.MousePos.x - cells.Min.x : (io.MousePos.x > cells.Max.x ? io.MousePos.x - cells.Max.x : 0.0f);
    if (dy != 0.0f) ScrollBy(view.v, *sheet.rows, fr_end, row_limit, static_cast<int64_t>(dy / S(20)) + (dy > 0 ? 1 : -1));
    if (dx != 0.0f) ScrollBy(view.h, *sheet.cols, fc_end, col_limit, static_cast<int64_t>(dx / S(60)) + (dx > 0 ? 1 : -1));
    if (dx != 0.0f || dy != 0.0f) RequestRedrawIn(0.03);
  }
  if (!ImGui::IsMouseDown(drag_btn)) {
    const bool right = view.right_drag;
    view.right_drag = false;
    if (view.move_dragging) {
      view.move_dragging = false;
      const int ins = view.move_insert;
      view.move_insert = 0;
      if (ins) {
        // Shift: inserted there (the app works out where they end up and selects them)
        act.move_cells = true;
        act.move_copy = io.KeyCtrl;
        act.move_insert = ins;
        act.move_src = view.move_src;
        act.move_r = view.ins_r;
        act.move_c = view.ins_c;
      } else if (!(view.move_target == view.move_src)) {
        act.move_cells = !right;
        act.move_menu = right;  // the right button: a menu of drop choices (the app selects after the choice)
        act.move_copy = io.KeyCtrl;
        act.move_src = view.move_src;
        act.move_r = view.move_target.r0;
        act.move_c = view.move_target.c0;
        if (!right) {
          selst.areas.back() = view.move_target;
          selst.anc_r = selst.act_r = view.move_target.r0;
          selst.anc_c = selst.act_c = view.move_target.c0;
          act.selection_changed = true;
        }
      } else if (right) {
        act.context_cells = true;  // a right-click on the border without a drag: the cell menu (Excel)
      }
    }
    if (view.fill_dragging) {
      view.fill_dragging = false;
      const Rect src = sel_areas.back(), tgt = view.fill_target;
      if (!(tgt == src) && right) {
        act.fill_menu = true;  // the right button: Copy Cells, Fill Series, Fill Days... (Excel)
        act.autofill_src = src;
        act.autofill_dest = tgt;
      } else if (!(tgt == src) && io.KeyShift) {
        // Shift: the dragged band of cells is inserted (dragging out) or deleted (dragging back in), as in Excel
        const bool vertical = tgt.r0 != src.r0 || tgt.r1 != src.r1;
        act.fill_shift_down = vertical;
        if (tgt.Contains(src)) {
          act.fill_shift = 1;
          if (vertical) act.fill_shift_rect = tgt.r1 > src.r1 ? Rect{src.r1 + 1, src.c0, tgt.r1, src.c1} : Rect{tgt.r0, src.c0, src.r0 - 1, src.c1};
          else act.fill_shift_rect = tgt.c1 > src.c1 ? Rect{src.r0, src.c1 + 1, src.r1, tgt.c1} : Rect{src.r0, tgt.c0, src.r1, src.c0 - 1};
        } else {
          act.fill_shift = 2;
          if (vertical) act.fill_shift_rect = Rect{tgt.r1 + 1, src.c0, src.r1, src.c1};
          else act.fill_shift_rect = Rect{src.r0, tgt.c1 + 1, src.r1, src.c1};
        }
      } else if (!(tgt == src)) {
        act.autofill = true;
        act.autofill_ctrl = io.KeyCtrl;
        act.autofill_src = src;
        act.autofill_dest = tgt;
        selst.areas.back() = tgt;
        act.selection_changed = true;
      } else if (right) {
        act.context_cells = true;
      }
      view.fill_tip_for = Rect{};
      view.fill_tip.clear();
    }
    if (view.dragging && view.deselecting && selst.areas.size() > 1) {
      // Ctrl+click / Ctrl+drag over selected cells: the dragged rectangle is cut out of every area
      const Rect cut = selst.areas.back();
      selst.areas.pop_back();
      std::vector<Rect> kept;
      for (const Rect& a : selst.areas) {
        if (!a.Intersects(cut)) {
          kept.push_back(a);
          continue;
        }
        const uint32_t ir0 = std::max(a.r0, cut.r0), ir1 = std::min(a.r1, cut.r1);
        if (cut.r0 > a.r0) kept.push_back(Rect{a.r0, a.c0, cut.r0 - 1, a.c1});
        if (cut.r1 < a.r1) kept.push_back(Rect{cut.r1 + 1, a.c0, a.r1, a.c1});
        if (cut.c0 > a.c0) kept.push_back(Rect{ir0, a.c0, ir1, cut.c0 - 1});
        if (cut.c1 < a.c1) kept.push_back(Rect{ir0, cut.c1 + 1, ir1, a.c1});
      }
      if (kept.empty()) kept.push_back(Rect::Cell(cut.r0, cut.c0));  // nothing left: the clicked cell stays selected
      selst.areas = std::move(kept);
      selst.mode = SelectMode::Cells;
      selst.anc_r = selst.act_r = selst.areas.back().r0;
      selst.anc_c = selst.act_c = selst.areas.back().c0;
      act.selection_changed = true;
    }
    view.deselecting = false;
    view.dragging = false;
    edit.ref_dragging = false;
    edit.comma_at = -1;
    edit.box_drag = 0;
    edit.ref_whole = 0;
  }

  // ---- Headers: select entire columns / rows, resize column widths / row heights ----
  if (opt.show_headings) {
    // point mode: a click (or a drag) on column / row headers inserts whole columns / rows (=SUM(B:B)), as in Excel;
    // the selection stays. The edit box is the active item, so the mouse is read directly (as for the cells).
    if (pointing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      const ImRect colhdr_r(ImVec2(cells.Min.x, rect.Min.y), ImVec2(cells.Max.x, rect.Min.y + header_h));
      const ImRect rowhdr_r(ImVec2(rect.Min.x, cells.Min.y), ImVec2(rect.Min.x + row_hdr_w, cells.Max.y));
      if (colhdr_r.Contains(io.MousePos) || rowhdr_r.Contains(io.MousePos)) {
        const bool cols = colhdr_r.Contains(io.MousePos);
        edit.ref_whole = cols ? 1 : 2;
        edit.box_drag = 0;
        edit.kpoint = false;
        edit.ref_dragging = true;
        if (cols) {
          const uint32_t c = static_cast<uint32_t>(hit_col(*pane_at(ImVec2(io.MousePos.x, cells.Min.y + 1)), io.MousePos.x));
          edit.ref_c0 = c;
          insert_ref(0, c, 0, c, 1);
        } else {
          const uint32_t r = static_cast<uint32_t>(hit_row(*pane_at(ImVec2(cells.Min.x + 1, io.MousePos.y)), io.MousePos.y));
          edit.ref_r0 = r;
          insert_ref(r, 0, r, 0, 2);
        }
        act.clicked = true;
      }
    }
    // column header row
    ImGui::SetCursorScreenPos(ImVec2(cells.Min.x, rect.Min.y));
    ImGui::InvisibleButton("##colhdr", ImVec2(cells.GetWidth(), std::max(1.0f, header_h)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool colhdr_hover = ImGui::IsItemHovered();
    // boundary: mouse within +-3 px of a column boundary
    int64_t edge_col = -1;
    bool edge_col_hidden = false;  // just right of hidden columns' double line: the last hidden one (unhide)
    if (opt.resize_cols && (colhdr_hover || view.resize_kind == 1)) {
      for (const Pane& p : panes) {
        if (p.rect.Min.y != cells.Min.y) continue;
        for (const Line& l : p.cols) {
          if (std::fabs(io.MousePos.x - (l.pos + l.size)) <= S(3)) edge_col = l.index, edge_col_hidden = false;
          if (l.index > 0 && sheet.cols->IsHidden(l.index - 1) && io.MousePos.x >= l.pos + 1.0f && io.MousePos.x <= l.pos + S(5))
            edge_col = l.index - 1, edge_col_hidden = true;
        }
      }
    }
    if (colhdr_hover && edge_col < 0 && view.resize_kind == 0) ImGui::SetMouseCursor(kCursorColHeader);
    if ((edge_col >= 0 && colhdr_hover) || view.resize_kind == 1)
      ImGui::SetMouseCursor((view.resize_kind == 1 ? view.resize_hidden : edge_col_hidden) ? kCursorColUnhide : ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      act.clicked = true;
      if (edge_col >= 0) {
        view.resize_kind = 1;
        view.resize_hidden = edge_col_hidden;
        view.resize_index = static_cast<uint32_t>(edge_col);
        view.resize_start_mouse = io.MousePos.x;
        view.resize_start_size = sheet.cols->Size(edge_col);
        view.resize_preview = view.resize_start_size;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          act.autofit = true;
          act.resize_kind = 1;
          act.resize_index = view.resize_index;
          act.resize_hidden = view.resize_hidden;
          view.resize_kind = 0;
        }
      } else {
        const Pane* p = pane_at(ImVec2(io.MousePos.x, cells.Min.y + 1));
        const uint32_t c = static_cast<uint32_t>(hit_col(*p, io.MousePos.x));
        if (!io.KeyShift) {
          selst.anc_c = c;
          selst.anc_r = static_cast<uint32_t>(view.v.first);
        }
        if (io.KeyCtrl) selst.areas.push_back(Rect{0, c, static_cast<uint32_t>(row_limit - 1), c});
        selst.act_c = c;
        selst.act_r = static_cast<uint32_t>(view.v.first);
        selst.mode = SelectMode::Cols;
        selst.areas.back() = Rect{0, std::min(selst.anc_c, c), static_cast<uint32_t>(row_limit - 1), std::max(selst.anc_c, c)};
        view.dragging = true;
        act.selection_changed = true;
      }
    }
    if (ImGui::IsItemActive() && view.resize_kind == 1) {
      view.resize_preview = std::max(0, view.resize_start_size + static_cast<int32_t>((io.MousePos.x - view.resize_start_mouse) / sc));
      char tip[64];
      std::snprintf(tip, sizeof(tip), TR("Width: %.2f (%d pixels)"), ColPxToChars(view.resize_preview), view.resize_preview);
      ImGui::SetTooltip("%s", tip);
      const float x = view.resize_start_mouse + static_cast<float>((view.resize_preview - view.resize_start_size) * sc);
      dl->AddRectFilled(ImVec2(x - 1, cells.Min.y), ImVec2(x, cells.Max.y), IM_COL32(0x40, 0x40, 0x40, 255));
    } else if (ImGui::IsItemActive() && selst.mode == SelectMode::Cols && view.dragging) {
      const Pane* p = pane_at(ImVec2(io.MousePos.x, cells.Min.y + 1));
      const uint32_t c = static_cast<uint32_t>(hit_col(*p, io.MousePos.x));
      selst.areas.back() = Rect{0, std::min(selst.anc_c, c), static_cast<uint32_t>(row_limit - 1), std::max(selst.anc_c, c)};
      act.selection_changed = true;
    }
    if (ImGui::IsItemDeactivated() && view.resize_kind == 1) {
      act.resize_kind = 1;
      act.resize_index = view.resize_index;
      act.resize_px = view.resize_preview;
      act.resize_hidden = view.resize_hidden;
      view.resize_kind = 0;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      const Pane* p = pane_at(ImVec2(io.MousePos.x, cells.Min.y + 1));
      const uint32_t c = static_cast<uint32_t>(hit_col(*p, io.MousePos.x));
      if (!col_selected(c) || selst.mode != SelectMode::Cols) {
        selst.areas.assign(1, Rect{0, c, static_cast<uint32_t>(row_limit - 1), c});
        selst.anc_c = selst.act_c = c;
        selst.mode = SelectMode::Cols;
        act.selection_changed = true;
      }
      act.context_col_header = true;
      act.clicked = true;
    }

    // row header column
    ImGui::SetCursorScreenPos(ImVec2(rect.Min.x, cells.Min.y));
    ImGui::InvisibleButton("##rowhdr", ImVec2(std::max(1.0f, row_hdr_w), cells.GetHeight()), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool rowhdr_hover = ImGui::IsItemHovered();
    int64_t edge_row = -1;
    bool edge_row_hidden = false;  // just below hidden rows' double line: the last hidden one (unhide)
    if (opt.resize_rows && (rowhdr_hover || view.resize_kind == 2)) {
      for (const Pane& p : panes) {
        if (p.rect.Min.x != cells.Min.x) continue;
        for (const Line& l : p.rows) {
          if (std::fabs(io.MousePos.y - (l.pos + l.size)) <= S(2.5f)) edge_row = l.index, edge_row_hidden = false;
          if (l.index > 0 && sheet.rows->IsHidden(l.index - 1) && io.MousePos.y >= l.pos + 1.0f && io.MousePos.y <= l.pos + S(4))
            edge_row = l.index - 1, edge_row_hidden = true;
        }
      }
    }
    if (rowhdr_hover && edge_row < 0 && view.resize_kind == 0) ImGui::SetMouseCursor(kCursorRowHeader);
    if ((edge_row >= 0 && rowhdr_hover) || view.resize_kind == 2)
      ImGui::SetMouseCursor((view.resize_kind == 2 ? view.resize_hidden : edge_row_hidden) ? kCursorRowUnhide : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      act.clicked = true;
      if (edge_row >= 0) {
        view.resize_kind = 2;
        view.resize_hidden = edge_row_hidden;
        view.resize_index = static_cast<uint32_t>(edge_row);
        view.resize_start_mouse = io.MousePos.y;
        view.resize_start_size = sheet.rows->Size(edge_row);
        view.resize_preview = view.resize_start_size;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          act.autofit = true;
          act.resize_kind = 2;
          act.resize_index = view.resize_index;
          act.resize_hidden = view.resize_hidden;
          view.resize_kind = 0;
        }
      } else {
        const Pane* p = pane_at(ImVec2(cells.Min.x + 1, io.MousePos.y));
        const uint32_t r = static_cast<uint32_t>(hit_row(*p, io.MousePos.y));
        if (!io.KeyShift) {
          selst.anc_r = r;
          selst.anc_c = static_cast<uint32_t>(view.h.first);
        }
        if (io.KeyCtrl) selst.areas.push_back(Rect{r, 0, r, static_cast<uint32_t>(col_limit - 1)});
        selst.act_r = r;
        selst.act_c = static_cast<uint32_t>(view.h.first);
        selst.mode = SelectMode::Rows;
        selst.areas.back() = Rect{std::min(selst.anc_r, r), 0, std::max(selst.anc_r, r), static_cast<uint32_t>(col_limit - 1)};
        view.dragging = true;
        act.selection_changed = true;
      }
    }
    if (ImGui::IsItemActive() && view.resize_kind == 2) {
      view.resize_preview = std::max(0, view.resize_start_size + static_cast<int32_t>((io.MousePos.y - view.resize_start_mouse) / sc));
      ImGui::SetTooltip(TR("Height: %.2f (%d pixels)"), view.resize_preview * 0.75, view.resize_preview);
      const float y = view.resize_start_mouse + static_cast<float>((view.resize_preview - view.resize_start_size) * sc);
      dl->AddRectFilled(ImVec2(cells.Min.x, y - 1), ImVec2(cells.Max.x, y), IM_COL32(0x40, 0x40, 0x40, 255));
    } else if (ImGui::IsItemActive() && selst.mode == SelectMode::Rows && view.dragging) {
      const Pane* p = pane_at(ImVec2(cells.Min.x + 1, io.MousePos.y));
      const uint32_t r = static_cast<uint32_t>(hit_row(*p, io.MousePos.y));
      selst.areas.back() = Rect{std::min(selst.anc_r, r), 0, std::max(selst.anc_r, r), static_cast<uint32_t>(col_limit - 1)};
      act.selection_changed = true;
    }
    if (ImGui::IsItemDeactivated() && view.resize_kind == 2) {
      act.resize_kind = 2;
      act.resize_index = view.resize_index;
      act.resize_px = view.resize_preview;
      act.resize_hidden = view.resize_hidden;
      view.resize_kind = 0;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      const Pane* p = pane_at(ImVec2(cells.Min.x + 1, io.MousePos.y));
      const uint32_t r = static_cast<uint32_t>(hit_row(*p, io.MousePos.y));
      if (!row_selected(r) || selst.mode != SelectMode::Rows) {
        selst.areas.assign(1, Rect{r, 0, r, static_cast<uint32_t>(col_limit - 1)});
        selst.anc_r = selst.act_r = r;
        selst.mode = SelectMode::Rows;
        act.selection_changed = true;
      }
      act.context_row_header = true;
      act.clicked = true;
    }
    ImGui::SetCursorScreenPos(rect.Min);
    if (ImGui::InvisibleButton("##all", ImVec2(std::max(1.0f, row_hdr_w), std::max(1.0f, header_h)))) {
      selst.areas.assign(1, Rect{0, 0, static_cast<uint32_t>(row_limit - 1), static_cast<uint32_t>(col_limit - 1)});
      selst.anc_r = selst.act_r = static_cast<uint32_t>(view.v.first);
      selst.anc_c = selst.act_c = static_cast<uint32_t>(view.h.first);
      selst.mode = SelectMode::All;
      act.clicked = true;
      act.selection_changed = true;
    }
  }

  // ---- Mouse wheel ----
  // (a popup opened from the sheet, a chart's flyout, counts as its child for ImGui: the wheel over it is the popup's)
  // It scrolls while a drag selects cells and while an entry is typed, as Excel's (the drag's or the editor's active
  // item blocked it: a long range couldn't be selected by a drag and the wheel)
  if (ImGui::IsMouseHoveringRect(rect.Min, ImVec2(rect.Max.x - sb, rect.Max.y - bottom_h)) &&
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_NoPopupHierarchy | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
    if (io.MouseWheel != 0.0f) {
      if (io.KeyCtrl) {
        // 10% a notch, every notch: a quick spin sends several in one frame, and they counted as one
        static float zoom_accum = 0.0f;
        zoom_accum += io.MouseWheel;
        const float notches = std::trunc(zoom_accum);
        zoom_accum -= notches;
        if (notches != 0.0f) view.zoom = std::clamp(std::round((view.zoom + 0.1f * notches) * 10.0f) / 10.0f, 0.1f, 4.0f);
      } else {
        ScrollAxis& ax = io.KeyShift ? view.h : view.v;
        ax.wheel_accum -= io.MouseWheel * static_cast<float>(WheelLines());
        const int64_t whole = static_cast<int64_t>(ax.wheel_accum);
        if (whole != 0) {
          ax.wheel_accum -= static_cast<float>(whole);
          // split: the top / left pane under the mouse scrolls by itself
          const bool in_top = view.split && view.freeze_rows && io.MousePos.y < cells.Min.y + top_h;
          const bool in_left = view.split && view.freeze_cols && io.MousePos.x < cells.Min.x + left_w;
          if (io.KeyShift && in_left) {
            ScrollAxis pane_ax;
            pane_ax.first = view.frozen_left;
            ScrollBy(pane_ax, *sheet.cols, 0, col_limit - view.freeze_cols, whole);
            view.frozen_left = static_cast<uint32_t>(pane_ax.first);
          } else if (!io.KeyShift && in_top) {
            ScrollAxis pane_ax;
            pane_ax.first = view.frozen_top;
            ScrollBy(pane_ax, *sheet.rows, 0, row_limit - view.freeze_rows, whole);
            view.frozen_top = static_cast<uint32_t>(pane_ax.first);
          } else if (io.KeyShift) {
            ScrollBy(view.h, *sheet.cols, view.split ? 0 : fc_end, col_limit, whole);
          } else {
            ScrollBy(view.v, *sheet.rows, view.split ? 0 : fr_end, row_limit, whole);
          }
        }
      }
    }
    if (io.MouseWheelH != 0.0f) {
      view.h.wheel_accum -= io.MouseWheelH * 3.0f;
      const int64_t whole = static_cast<int64_t>(view.h.wheel_accum);
      if (whole != 0) {
        view.h.wheel_accum -= static_cast<float>(whole);
        ScrollBy(view.h, *sheet.cols, fc_end, col_limit, whole);
      }
    }
  }

  // ---- Keyboard (Ready mode) ----
  const bool kb = opt.keyboard && !io.WantTextInput && !edit.active;
  if (kb) {
    const uint32_t r = selst.act_r, c = selst.act_c;
    // a navigation key that ended an entry in Enter mode acts now, with the Ctrl / Shift it was pressed with
    GridView::NavReplay nav = view.nav_replay;
    view.nav_replay = {};
    if (ImGui::GetFrameCount() - nav.frame > 4) nav = {};
    auto key_pressed = [&](ImGuiKey k, bool repeat = true) { return ImGui::IsKeyPressed(k, repeat) || nav.key == k; };
    const bool key_shift = nav.key != ImGuiKey_None ? nav.shift : io.KeyShift;
    const bool key_ctrl = nav.key != ImGuiKey_None ? nav.ctrl : io.KeyCtrl;
    auto move_to = [&](int64_t nr, int64_t nc, bool extend) {
      nr = std::clamp<int64_t>(nr, 0, row_limit - 1);
      nc = std::clamp<int64_t>(nc, 0, col_limit - 1);
      if (extend) {
        // Shift: extend the current area (the active cell stays, the "other corner" moves)
        Rect& a = selst.areas.back();
        static uint32_t far_r = 0, far_c = 0;
        (void)far_r;
        (void)far_c;
        a = Rect{std::min<uint32_t>(selst.anc_r, static_cast<uint32_t>(nr)), std::min<uint32_t>(selst.anc_c, static_cast<uint32_t>(nc)),
                 std::max<uint32_t>(selst.anc_r, static_cast<uint32_t>(nr)), std::max<uint32_t>(selst.anc_c, static_cast<uint32_t>(nc))};
        a = expand_merges(a);  // the merged cells it touches, whole (Excel's Selection is A1:C3, not A1:B2)
        selst.mode = SelectMode::Cells;
        EnsureVisible(sheet, view, static_cast<uint32_t>(nr), static_cast<uint32_t>(nc));
      } else {
        SelectCell(view, static_cast<uint32_t>(nr), static_cast<uint32_t>(nc), false);
        EnsureVisible(sheet, view, static_cast<uint32_t>(nr), static_cast<uint32_t>(nc));
      }
      act.selection_changed = true;
    };
    // when extending with Shift, the moving end is the corner of the area farthest from the anchor
    const Rect& a = selst.areas.back();
    const bool extending = key_shift || view.extend_mode;  // F8: the arrows extend as with Shift
    const uint32_t er = extending ? (a.r0 == selst.anc_r ? a.r1 : a.r0) : r;
    const uint32_t ec = extending ? (a.c0 == selst.anc_c ? a.c1 : a.c0) : c;
    auto next = [&](const model::DimIndex& dim, int64_t i, int dir, int64_t limit) {
      const int64_t n = dim.NextVisible(i + dir, dir, limit);
      return n < 0 ? i : n;
    };
    const bool ctrl = key_ctrl;
    const bool shift = extending;
    // F8 / Shift+F8 / End: Excel's modes (the status bar shows them); Escape ends them
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false) && !ctrl && !io.KeyAlt) {
      if (io.KeyShift) {
        view.add_mode = !view.add_mode;
        view.extend_mode = false;
      } else {
        view.extend_mode = !view.extend_mode;
        view.add_mode = false;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) view.extend_mode = view.add_mode = view.end_mode = false;
    const bool jump = ctrl || view.end_mode;  // End, then an arrow: as Ctrl+arrow
    bool arrow = false;
    // a merged cell is one cell for the keys: a step starts from its far edge, and lands on a merge's top-left cell
    auto merge_at = [&](uint32_t mr, uint32_t mc) {
      for (const Rect& mm : *sheet.merges) {
        if (mm.Contains(mr, mc)) return mm;
      }
      return Rect::Cell(mr, mc);
    };
    const Rect am = merge_at(r, c);
    bool step_moves = false;  // Enter / Tab: Shift reverses the step, it never extends the selection
    auto step_to = [&](int64_t nr, int64_t nc) {
      if (shift && !step_moves) {
        move_to(nr, nc, true);
        return;
      }
      if (opt.selectable) {
        // a protected sheet: the step goes on past the cells that can't be selected (none: it doesn't move)
        const int64_t dr = nr > r ? 1 : nr < r ? -1 : 0, dc = nc > c ? 1 : nc < c ? -1 : 0;
        int guard = 0;
        while (nr >= 0 && nc >= 0 && nr < row_limit && nc < col_limit && !opt.selectable(static_cast<uint32_t>(nr), static_cast<uint32_t>(nc)) &&
               (dr || dc) && ++guard < 20000) {
          nr += dr;
          nc += dc;
        }
        if (nr < 0 || nc < 0 || nr >= row_limit || nc >= col_limit || !opt.selectable(static_cast<uint32_t>(nr), static_cast<uint32_t>(nc))) return;
      }
      const Rect dm = merge_at(static_cast<uint32_t>(std::clamp<int64_t>(nr, 0, row_limit - 1)), static_cast<uint32_t>(std::clamp<int64_t>(nc, 0, col_limit - 1)));
      move_to(dm.r0, dm.c0, false);
    };
    // whole rows / columns selected (a header, Shift+Space, Ctrl+Space): Shift+arrows grow or shrink them in their own
    // direction only, and the window doesn't jump to the end of the sheet (Excel); the other direction does nothing
    const bool whole = shift && selst.mode != SelectMode::Cells;
    auto grow_whole = [&](bool rows_dim, int dir) {
      if (selst.mode == SelectMode::All || rows_dim != (selst.mode == SelectMode::Rows)) return;
      Rect& wa = selst.areas.back();
      const int64_t anc = rows_dim ? selst.anc_r : selst.anc_c;
      const int64_t lo = rows_dim ? wa.r0 : wa.c0, hi = rows_dim ? wa.r1 : wa.c1;
      const int64_t end_at = lo == anc ? hi : lo;  // the moving end (not "far": a windows.h macro)
      const int64_t lim = rows_dim ? row_limit : col_limit;
      int64_t nf = jump ? engine::CtrlJump(sheet, rows_dim ? static_cast<uint32_t>(end_at) : selst.act_r, rows_dim ? selst.act_c : static_cast<uint32_t>(end_at), rows_dim, dir)
                        : next(rows_dim ? *sheet.rows : *sheet.cols, end_at, dir, lim);
      nf = std::clamp<int64_t>(nf, 0, lim - 1);
      const uint32_t a0 = static_cast<uint32_t>(std::min(anc, nf)), a1 = static_cast<uint32_t>(std::max(anc, nf));
      if (rows_dim) {
        wa = Rect{a0, 0, a1, static_cast<uint32_t>(col_limit - 1)};
        EnsureVisible(sheet, view, static_cast<uint32_t>(nf), static_cast<uint32_t>(view.h.first));
      } else {
        wa = Rect{0, a0, static_cast<uint32_t>(row_limit - 1), a1};
        EnsureVisible(sheet, view, static_cast<uint32_t>(view.v.first), static_cast<uint32_t>(nf));
      }
      act.selection_changed = true;
    };
    // Scroll Lock: the arrows scroll the window by a row / column and the active cell stays where it is (Excel)
    const bool lock_scroll = opt.scroll_lock && !ctrl && !shift && !io.KeyAlt;
    // Alt + arrows are commands (Alt+Down opens a cell's list, Shift+Alt+Right groups), not moves, as in Excel
    if (lock_scroll) {
      if (key_pressed(ImGuiKey_DownArrow)) ScrollBy(view.v, *sheet.rows, view.split ? 0 : fr_end, row_limit, 1);
      if (key_pressed(ImGuiKey_UpArrow)) ScrollBy(view.v, *sheet.rows, view.split ? 0 : fr_end, row_limit, -1);
      if (key_pressed(ImGuiKey_RightArrow)) ScrollBy(view.h, *sheet.cols, view.split ? 0 : fc_end, col_limit, 1);
      if (key_pressed(ImGuiKey_LeftArrow)) ScrollBy(view.h, *sheet.cols, view.split ? 0 : fc_end, col_limit, -1);
    } else if (no_rows) {
      // every row hidden: no arrow moves the active cell, with Ctrl or Shift neither (Excel 16, measured); with every
      // column hidden Down and Up still go to the next shown row, Right and Left find none (below)
    } else if (whole && !io.KeyAlt) {
      if (key_pressed(ImGuiKey_DownArrow)) arrow = true, grow_whole(true, +1);
      if (key_pressed(ImGuiKey_UpArrow)) arrow = true, grow_whole(true, -1);
      if (key_pressed(ImGuiKey_RightArrow)) arrow = true, grow_whole(false, +1);
      if (key_pressed(ImGuiKey_LeftArrow)) arrow = true, grow_whole(false, -1);
    } else if (!io.KeyAlt) {
      const int64_t dn = shift ? er : am.r1, up = shift ? er : am.r0, rt = shift ? ec : am.c1, lf = shift ? ec : am.c0;
      // Ctrl+Shift+arrows from a range jump along the anchor's row (Left / Right) or column (Up / Down), starting at
      // the moving end (Excel 16, measured: A1, Ctrl+Shift+Down to A4, then Ctrl+Shift+Right follows row 1, not row 4)
      const uint32_t jr = shift ? selst.anc_r : er, jc = shift ? selst.anc_c : ec;
      if (key_pressed(ImGuiKey_DownArrow)) arrow = true, step_to(jump ? engine::CtrlJump(sheet, er, jc, true, +1) : next(*sheet.rows, dn, +1, row_limit), ec);
      if (key_pressed(ImGuiKey_UpArrow)) arrow = true, step_to(jump ? engine::CtrlJump(sheet, er, jc, true, -1) : next(*sheet.rows, up, -1, row_limit), ec);
      if (key_pressed(ImGuiKey_RightArrow)) arrow = true, step_to(er, jump ? engine::CtrlJump(sheet, jr, ec, false, +1) : next(*sheet.cols, rt, +1, col_limit));
      if (key_pressed(ImGuiKey_LeftArrow)) arrow = true, step_to(er, jump ? engine::CtrlJump(sheet, jr, ec, false, -1) : next(*sheet.cols, lf, -1, col_limit));
    }
    if (arrow) view.end_mode = false;
    const int64_t page = std::max<int64_t>(1, view.vis_rows - 1);
    if (key_pressed(ImGuiKey_PageDown) && !no_rows) {  // (every row hidden: Page Down / Up stay, measured)
      if (io.KeyAlt) {
        ScrollBy(view.h, *sheet.cols, fc_end, col_limit, std::max<int64_t>(1, view.vis_cols - 1));
        move_to(er, view.h.first, shift);
      } else {
        ScrollBy(view.v, *sheet.rows, fr_end, row_limit, page);
        move_to(std::min<int64_t>(row_limit - 1, er + page), ec, shift);
      }
    }
    if (key_pressed(ImGuiKey_PageUp) && !no_rows) {
      if (io.KeyAlt) {
        ScrollBy(view.h, *sheet.cols, fc_end, col_limit, -std::max<int64_t>(1, view.vis_cols - 1));
        move_to(er, view.h.first, shift);
      } else {
        ScrollBy(view.v, *sheet.rows, fr_end, row_limit, -page);
        move_to(std::max<int64_t>(0, static_cast<int64_t>(er) - page), ec, shift);
      }
    }
    // Ctrl+Shift+Home / Ctrl+Shift+End grow the selection to take in the first / last cell: the box around both, the
    // active cell staying (Excel 16, measured: C3:D3, then Ctrl+Shift+Home selects A1:D3, not A1:C3)
    auto grow_to = [&](int64_t tr, int64_t tc) {
      tr = std::clamp<int64_t>(tr, 0, row_limit - 1);
      tc = std::clamp<int64_t>(tc, 0, col_limit - 1);
      Rect& g = selst.areas.back();
      g = expand_merges(Rect{std::min<uint32_t>(g.r0, static_cast<uint32_t>(tr)), std::min<uint32_t>(g.c0, static_cast<uint32_t>(tc)),
                             std::max<uint32_t>(g.r1, static_cast<uint32_t>(tr)), std::max<uint32_t>(g.c1, static_cast<uint32_t>(tc))});
      selst.mode = SelectMode::Cells;
      EnsureVisible(sheet, view, static_cast<uint32_t>(tr), static_cast<uint32_t>(tc));
      act.selection_changed = true;
    };
    if (key_pressed(ImGuiKey_Home) && view.end_mode) {
      // End, then Home: the last cell of the used area, as Ctrl+End (Excel)
      const core::CellPos lc = engine::LastCell(sheet);
      if (shift && selst.mode == SelectMode::Cells) grow_to(lc.row, lc.col);
      else move_to(lc.row, lc.col, shift);
      view.end_mode = false;
    } else if (key_pressed(ImGuiKey_Home)) {
      const int64_t hr = ctrl ? (view.freeze_rows ? fr_end : 0) : er, hc = view.freeze_cols && !tee ? fc_end : 0;
      if (ctrl && shift && selst.mode == SelectMode::Cells) grow_to(hr, hc);
      else move_to(hr, hc, shift);
    }
    if (key_pressed(ImGuiKey_End) && ctrl) {
      const core::CellPos lc = engine::LastCell(sheet);
      if (shift && selst.mode == SelectMode::Cells) grow_to(lc.row, lc.col);
      else move_to(lc.row, lc.col, shift);
    } else if (key_pressed(ImGuiKey_End, false) && !io.KeyAlt) {
      view.end_mode = !view.end_mode;
    }
    // Ctrl+A (and Ctrl+Shift+*): the data around the active cell first, then (pressed again, or in an empty spot) the whole sheet
    const bool star = ctrl && ((io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_8)) || ImGui::IsKeyPressed(ImGuiKey_KeypadMultiply));
    // Ctrl+Shift+Space works as Ctrl+A (the data around the active cell first)
    const bool sel_all = ctrl && ((!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A)) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Space)));
    if (sel_all || star) {
      const Rect reg = engine::CurrentRegion(sheet, selst.act_r, selst.act_c);
      const bool lone = reg.IsCell() && sheet.IsEmptyCell(reg.r0, reg.c0);
      const bool already = selst.mode == SelectMode::Cells && selst.areas.size() == 1 && selst.areas[0] == reg;
      const bool region = !lone && (star || !(already || selst.mode == SelectMode::All));
      if (region) {
        selst.areas.assign(1, reg);
        selst.anc_r = reg.r0;
        selst.anc_c = reg.c0;
        // Ctrl+Shift+8 makes the region's first cell the active one; Ctrl+A keeps it (Excel 16, measured)
        if (star) selst.act_r = reg.r0, selst.act_c = reg.c0;
        selst.mode = SelectMode::Cells;
      } else if (!star) {
        selst.areas.assign(1, Rect{0, 0, static_cast<uint32_t>(row_limit - 1), static_cast<uint32_t>(col_limit - 1)});
        selst.mode = SelectMode::All;
      }
      act.selection_changed = true;
    }
    // Ctrl+Space / Shift+Space: the whole columns / rows of every area, stored as a header click stores them (so
    // Shift+arrows after them grow whole columns / rows)
    if (ctrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Space)) {
      for (Rect& wa : selst.areas) {
        if (selst.mode == SelectMode::Cells) wa = expand_merges(wa);  // a merged cell's columns, all of them
        wa = Rect{0, wa.c0, static_cast<uint32_t>(row_limit - 1), wa.c1};
      }
      selst.mode = selst.mode == SelectMode::Rows || selst.mode == SelectMode::All ? SelectMode::All : SelectMode::Cols;
      act.selection_changed = true;
    } else if (io.KeyShift && !ctrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Space)) {
      for (Rect& wa : selst.areas) {
        if (selst.mode == SelectMode::Cells) wa = expand_merges(wa);  // a merged cell's rows, all of them
        wa = Rect{wa.r0, 0, wa.r1, static_cast<uint32_t>(col_limit - 1)};
      }
      selst.mode = selst.mode == SelectMode::Cols || selst.mode == SelectMode::All ? SelectMode::All : SelectMode::Rows;
      act.selection_changed = true;
    }
    if (ctrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Period, false)) {
      const Rect s = sel_areas.back();
      uint32_t nr = s.r0, nc = s.c0;  // not at a corner: the top-left one
      if (r == s.r0 && c == s.c0) nr = s.r0, nc = s.c1;
      else if (r == s.r0 && c == s.c1) nr = s.r1, nc = s.c1;
      else if (r == s.r1 && c == s.c1) nr = s.r1, nc = s.c0;
      selst.act_r = nr;
      selst.act_c = nc;
      EnsureVisible(sheet, view, nr, nc);
      act.selection_changed = true;
    }
    // Enter / Tab: move the active cell within the selection (a multi-cell selection is kept)
    // whole rows / columns (or the whole sheet) are walked within the used range and stay selected (Excel)
    auto used_part = [&](Rect a) {
      if (a.c0 == 0 && a.c1 >= col_limit - 1) a.c1 = std::max<uint32_t>(sheet.has_used ? sheet.used.c1 : 0, c);
      if (a.r0 == 0 && a.r1 >= row_limit - 1) a.r1 = std::max<uint32_t>(sheet.has_used ? sheet.used.r1 : 0, r);
      return a;
    };
    auto move_in_selection = [&](int dir, bool tab) {
      // every row or every column hidden: no cell to land on, Enter and Tab leave the active cell where it is (Excel
      // 16, measured: also right after an entry)
      if (no_rows || no_cols) return;
      const Rect s = used_part(sel_areas.back());
      if (selst.areas.size() > 1) {
        // a multiple selection: through the active area, then on to the next area (Excel); the areas stay selected
        int64_t nr = r, nc = c;
        bool wrap = false;
        if (dir == 1 || dir == 2) {
          nr += dir == 1 ? 1 : -1;
          if (nr > s.r1) nr = s.r0, ++nc, wrap = nc > s.c1;
          if (nr < s.r0) nr = s.r1, --nc, wrap = nc < s.c0;
        } else {
          nc += dir == 3 ? 1 : -1;
          if (nc > s.c1) nc = s.c0, ++nr, wrap = nr > s.r1;
          if (nc < s.c0) nc = s.c1, --nr, wrap = nr < s.r0;
        }
        if (wrap) {
          const bool fwd = dir == 1 || dir == 3;
          if (fwd) std::rotate(selst.areas.begin(), selst.areas.begin() + 1, selst.areas.end());
          else std::rotate(selst.areas.rbegin(), selst.areas.rbegin() + 1, selst.areas.rend());
          const Rect na = used_part(selst.areas.back());
          nr = fwd ? na.r0 : na.r1;
          nc = fwd ? na.c0 : na.c1;
          selst.anc_r = na.r0;
          selst.anc_c = na.c0;
        }
        selst.act_r = static_cast<uint32_t>(nr);
        selst.act_c = static_cast<uint32_t>(nc);
        EnsureVisible(sheet, view, selst.act_r, selst.act_c);
        act.selection_changed = true;
        return;
      }
      // one merged cell selected is a single cell: Enter and Tab leave it from its edges
      if (s.IsCell() || s == am) {
        if (!tab && dir == 1 && selst.TabFrom() != UINT32_MAX) {  // the row typed with Tab: Enter starts the next one
          move_to(next(*sheet.rows, am.r1, +1, row_limit), selst.TabFrom(), false);
          selst.tab_from = UINT32_MAX;
          return;
        }
        if (tab) selst.NoteTab(dir == 3);
        // Shift+Enter / Shift+Tab move the active cell up / left (they extended the selection, as Shift+arrows do)
        step_moves = true;
        if (dir == 1) step_to(next(*sheet.rows, am.r1, +1, row_limit), c);
        if (dir == 2) step_to(next(*sheet.rows, am.r0, -1, row_limit), c);
        if (dir == 3) step_to(r, next(*sheet.cols, am.c1, +1, col_limit));
        if (dir == 4) step_to(r, next(*sheet.cols, am.c0, -1, col_limit));
        step_moves = false;
        if (tab) selst.NoteTabDone();
        return;
      }
      int64_t nr = r, nc = c;
      // hidden rows and columns (a filtered list) and the inside of a merged cell are passed over, as in Excel
      const uint64_t cells = static_cast<uint64_t>(s.Rows()) * s.Cols();
      for (uint64_t guard = 0; guard < std::min<uint64_t>(cells, 4000000); ++guard) {
        if (dir == 1 || dir == 2) {
          nr += dir == 1 ? 1 : -1;
          if (nr > s.r1) { nr = s.r0; nc = nc + 1 > s.c1 ? s.c0 : nc + 1; }
          if (nr < s.r0) { nr = s.r1; nc = nc - 1 < s.c0 ? s.c1 : nc - 1; }
        } else {
          nc += dir == 3 ? 1 : -1;
          if (nc > s.c1) { nc = s.c0; nr = nr + 1 > s.r1 ? s.r0 : nr + 1; }
          if (nc < s.c0) { nc = s.c1; nr = nr - 1 < s.r0 ? s.r1 : nr - 1; }
        }
        if (nr == r && nc == c) break;  // all the way round: nothing else to land on
        const Rect m = merge_at(static_cast<uint32_t>(nr), static_cast<uint32_t>(nc));
        if (!sheet.rows->IsHidden(nr) && !sheet.cols->IsHidden(nc) && m.r0 == nr && m.c0 == nc) break;
      }
      selst.act_r = static_cast<uint32_t>(nr);
      selst.act_c = static_cast<uint32_t>(nc);
      EnsureVisible(sheet, view, selst.act_r, selst.act_c);
      act.selection_changed = true;
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      if (opt.copy_marquee && !opt.marquee_no_paste && !io.KeyShift && !ctrl && !io.KeyAlt) act.paste_enter = true;
      else if (opt.enter_dir > 0) {
        // File > Options: the way Enter moves (Shift+Enter: the other way)
        static const int kBack[5] = {0, 2, 1, 4, 3};
        move_in_selection(io.KeyShift ? kBack[std::clamp(opt.enter_dir, 1, 4)] : std::clamp(opt.enter_dir, 1, 4), false);
      }
    }
    // a protected sheet: Tab / Shift+Tab go to the next / previous unlocked cell, in reading order, wrapping (Excel)
    auto next_unlocked = [&](bool back) {
      Rect area = sheet.has_used ? sheet.used : Rect::Cell(r, c);
      area = area.Union(Rect::Cell(r, c));
      const uint64_t w = area.Cols(), n = static_cast<uint64_t>(area.Rows()) * w;
      const uint64_t at = static_cast<uint64_t>(r - area.r0) * w + (c - area.c0);
      for (uint64_t k = 1; k <= n && k < 4000000; ++k) {
        const uint64_t i = back ? (at + n - k % n) % n : (at + k) % n;
        const uint32_t rr = area.r0 + static_cast<uint32_t>(i / w), cc = area.c0 + static_cast<uint32_t>(i % w);
        if (opt.unlocked(rr, cc) && (!opt.selectable || opt.selectable(rr, cc))) {
          move_to(rr, cc, false);
          return true;
        }
      }
      return false;
    };
    const bool single = sel_areas.size() == 1 && (sel_areas.back().IsCell() || sel_areas.back() == am);
    // (Ctrl+Tab switches windows: the active cell stays)
    if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !io.KeyCtrl && opt.unlocked && single) next_unlocked(io.KeyShift);
    else if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !io.KeyCtrl) move_in_selection(io.KeyShift ? 4 : 3, true);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) act.clear_contents = true;
    // F2 edits; Shift+F2 (a note) and Ctrl+F2 (print preview) are commands
    // (Alt+F2 is Save As, Alt+Shift+F2 Save: they don't start editing)
    if (ImGui::IsKeyPressed(ImGuiKey_F2) && !io.KeyShift && !ImGui::GetIO().KeyCtrl && !io.KeyAlt) act.start_edit_f2 = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && ctrl) {
      // Ctrl+Backspace: scroll the active cell into view (Excel)
      EnsureVisible(sheet, view, selst.act_r, selst.act_c);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && io.KeyShift) {
      // Shift+Backspace: the selection shrinks to the active cell (Excel), nothing is cleared
      selst.areas.assign(1, Rect::Cell(selst.act_r, selst.act_c));
      selst.anc_r = selst.act_r;
      selst.anc_c = selst.act_c;
      selst.mode = SelectMode::Cells;
      act.selection_changed = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
      act.start_edit_typing = true;
      act.start_edit_text.clear();
    }
    // typing starts editing (replacing the contents)
    if (!ctrl && !io.KeyAlt && io.InputQueueCharacters.Size > 0) {
      std::string s;
      for (ImWchar ch : io.InputQueueCharacters) {
        if (ch < 32 || ch == 127) continue;
        if (ch == ' ' && io.KeyShift) continue;  // Shift+Space selects the row (above); Windows also sends it as a space
        char buf[8];
        const int n = ImTextCharToUtf8(buf, static_cast<unsigned int>(ch));
        s.append(buf, static_cast<size_t>(n));
      }
      if (!s.empty()) {
        act.start_edit_typing = true;
        act.start_edit_text = s;
      }
    }
  }

  // ---- Overlay objects (controls, charts) ----
  if (opt.overlay) {
    const model::DimIndex& rows = *sheet.rows;
    const model::DimIndex& cols = *sheet.cols;
    const double main_y0 = static_cast<double>(rows.Offset(main.first_row)), main_x0 = static_cast<double>(cols.Offset(main.first_col));
    const double frz_y0 = static_cast<double>(rows.Offset(view.frozen_top)), frz_x0 = static_cast<double>(cols.Offset(view.frozen_left));
    const double frz_y1 = static_cast<double>(rows.Offset(fr_end)), frz_x1 = static_cast<double>(cols.Offset(fc_end));
    const float my = main.rect.Min.y, mx = main.rect.Min.x;
    OverlayGeom g;
    g.cells = cells;
    g.sc = sc;
    // T layout: sheet x before the frozen columns maps into the left pane, right-aligned at its last column
    const double mir_x1 = tee && mir_last >= 0 ? static_cast<double>(cols.Offset(mir_last + 1)) : 0.0;
    const float mir_r = cells.Min.x + mir_w, frz_sx = cells.Min.x + mir_w;
    g.to_screen = [=](double x, double y) {
      const float sx = (tee && x < frz_x0) ? mir_r + static_cast<float>((x - mir_x1) * sc)
                       : (view.freeze_cols && x < frz_x1) ? frz_sx + static_cast<float>((x - frz_x0) * sc) : mx + static_cast<float>((x - main_x0) * sc);
      const float sy = (view.freeze_rows && y < frz_y1) ? cells.Min.y + static_cast<float>((y - frz_y0) * sc) : my + static_cast<float>((y - main_y0) * sc);
      return ImVec2(sx, sy);
    };
    g.to_sheet = [=](ImVec2 p) {
      const double x = (tee && p.x < mir_r) ? mir_x1 + (p.x - mir_r) / sc
                       : (view.freeze_cols && p.x < mx) ? frz_x0 + (p.x - frz_sx) / sc : main_x0 + (p.x - mx) / sc;
      const double y = (view.freeze_rows && p.y < my) ? frz_y0 + (p.y - cells.Min.y) / sc : main_y0 + (p.y - my) / sc;
      return ImVec2(static_cast<float>(x), static_cast<float>(y));
    };
    ImGui::PushClipRect(cells.Min, cells.Max, true);
    opt.overlay(g);
    ImGui::PopClipRect();
  }

  // ---- In-cell edit box ----
  // a formula being typed on another sheet, pointing at this one (its tab was clicked): the entry stays live in an
  // invisible box here, so Enter, Escape, the arrows and typing still reach it (it was drawn by its own sheet's grid
  // only, and every key went nowhere)
  const bool editing_away = edit.active && edit.sheet != sheet.id && edit.view_id == opt.view_id && opt.host_away_edit;
  if (editing_here || editing_away) {
    // AutoComplete's words: the entries in the column above and below the cell, up to the first empty cell (as in
    // Excel): text, a formula's text and TRUE / FALSE (they make an entry ambiguous: tr with Trade and TRUE completes
    // nothing), without the list's header row (Item over Apple, 10 completes no i), measured in Excel 16
    const uint64_t key = (static_cast<uint64_t>(edit.sheet) << 52) ^ (static_cast<uint64_t>(edit.r) << 20) ^ edit.c;
    if (editing_here && edit.column_words_for != key + 1) {
      edit.column_words_for = key + 1;
      edit.column_words.clear();
      // begun by typing (the text isn't the cell's own): the first character typed may already complete, as in Excel
      const core::Value cur = sheet.GetValue(edit.r, edit.c);
      edit.ac_first = !edit.text.empty() && !(cur.IsString() && cur.str() == edit.text) && sheet.GetFormula(edit.r, edit.c) == 0;
      edit.ac_prev_len = edit.ac_first ? 0 : static_cast<int>(edit.text.size());
      uint32_t top = edit.r;  // the list's first row
      int top_word = -1;      // its word, if it gave one
      auto take = [&](uint32_t r) {
        const core::Value v = sheet.GetValue(r, edit.c);
        if (v.IsEmpty()) return false;
        if (v.IsString()) edit.column_words.emplace_back(v.str());
        else if (v.IsBool()) edit.column_words.emplace_back(v.boolean() ? "TRUE" : "FALSE");
        return true;
      };
      for (uint32_t r = edit.r; r > 0 && edit.column_words.size() < 2000; --r) {
        const size_t before = edit.column_words.size();
        if (!take(r - 1)) break;
        top = r - 1;
        top_word = edit.column_words.size() > before ? static_cast<int>(before) : -1;
      }
      if (top == edit.r) {  // nothing above: the list starts below the cell
        top = edit.r + 1;
        top_word = -1;
      }
      for (uint32_t r = edit.r + 1; r < sheet.row_limit && edit.column_words.size() < 4000; ++r) {
        const size_t before = edit.column_words.size();
        if (!take(r)) break;
        if (r == top && edit.column_words.size() > before) top_word = static_cast<int>(before);
      }
      if (top_word >= 0 && ListHeaderRow(sheet, ctx.styles, top, edit.c, edit.r)) edit.column_words.erase(edit.column_words.begin() + top_word);
    }
    for (const Pane* pp : all_panes) {
      const Pane& p = *pp;
      // the blank main pane (every row or column hidden) shows no cell: the entry lives in its invisible box
      const Rect vis = p.Valid() ? Rect{static_cast<uint32_t>(p.rows.front().index), static_cast<uint32_t>(p.cols.front().index),
                                        static_cast<uint32_t>(p.rows.back().index), static_cast<uint32_t>(p.cols.back().index)}
                                 : Rect{1, 1, 0, 0};
      Rect er = Rect::Cell(edit.r, edit.c);
      for (const Rect& mm : *sheet.merges) {
        if (mm.Contains(edit.r, edit.c)) er = mm;
      }
      // the cell scrolled out of view (pointing far away, the wheel): the entry stays live in an invisible box, so
      // Enter, Escape, the arrows and typing still work (Excel); the formula bar shows the text meanwhile
      // Page Layout view (page_view 2) draws the text on its page: the box stays invisible here
      const bool shown = editing_here && vis.Contains(er.r0, er.c0) && view.page_view != 2;
      if (!shown && pp != all_panes.back()) continue;
      const model::Style& st = styles.Get(sheet.GetStyle(edit.r, edit.c));
      ImFont* font = FontFor(ctx, st);
      const float px = FontPxFor(ctx, st);
      const float x0 = shown ? ColX(ctx, p, er.c0) : p.rect.Min.x, y0 = shown ? RowY(ctx, p, er.r0) : p.rect.Min.y;
      const float w0 = ColX(ctx, p, static_cast<int64_t>(er.c1) + 1) - x0, h0 = RowY(ctx, p, static_cast<int64_t>(er.r1) + 1) - y0;
      // width grows with the text (up to the pane's right edge), height with the number of lines
      const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, edit.text.c_str());
      // width: text + one character (the longest line for multiple lines); padding aligns the text to the cell bottom and keeps the content within the box (no scroll bar)
      const float line_h = font->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
      // the cell's alignment anchors the box (Excel): right-aligned text grows to the left, centered both ways; with
      // Wrap Text the box keeps the cell's width and grows down
      const model::HAlign ha = shown ? st.halign : model::HAlign::General;
      const float room = ha == model::HAlign::Right ? x0 + w0 - p.rect.Min.x - 2
                         : (ha == model::HAlign::Center || ha == model::HAlign::CenterAcross) ? p.rect.GetWidth() - 4
                                                                                               : p.rect.Max.x - x0 - 2;
      const float max_w = st.wrap ? w0 - 1 : std::max(w0 - 1, room);
      const float w = std::clamp(ts.x + ctx.pad * 2 + px, w0 - 1, max_w);
      float bx = x0;
      if (ha == model::HAlign::Right) bx = x0 + w0 - 1 - w;
      else if (ha == model::HAlign::Center || ha == model::HAlign::CenterAcross) bx = x0 + (w0 - 1 - w) * 0.5f;
      if (bx != x0) bx = std::clamp(bx, p.rect.Min.x + 1, std::max(p.rect.Min.x + 1, p.rect.Max.x - w - 1));
      // doesn't fit to the right: wrap and grow downwards (like Excel)
      const bool wrap = ts.x + ctx.pad * 2 + px > max_w;
      const float text_h = wrap ? font->CalcTextSizeA(px, FLT_MAX, w - ctx.pad * 2 - px * 0.5f, edit.text.c_str()).y : ts.y;
      const float pad_y = std::max(0.0f, std::floor((h0 - 1 - line_h) * 0.5f));
      const float h = std::max(h0 - 1, text_h + pad_y * 2 + 1);
      if (shown && edit.click_x >= 0.0f) {
        // opened by a double-click on its text: the caret goes between the characters under the mouse (Excel), measured
        // where the cell shows the text (its alignment); a formula or a number, whose entry isn't the text shown, is
        // measured where the editor shows the entry (Excel: the caret lands in the formula where it was clicked)
        const core::Value& cv = sheet.GetValue(edit.r, edit.c);
        const bool shown_text = cv.IsString() && !sheet.GetFormula(edit.r, edit.c) && cv.str() == edit.text;
        if (!edit.text.empty() && !wrap) {
          const float tw = ts.x;
          float tx = shown_text ? x0 + ctx.pad : bx + ctx.pad;
          if (shown_text && st.halign == model::HAlign::Right) tx = x0 + w0 - ctx.pad - tw;
          else if (shown_text && (st.halign == model::HAlign::Center || st.halign == model::HAlign::CenterAcross)) tx = x0 + (w0 - tw) * 0.5f;
          int best = static_cast<int>(edit.text.size());
          for (size_t i = 0; i <= edit.text.size(); ++i) {
            if (i < edit.text.size() && (static_cast<unsigned char>(edit.text[i]) & 0xC0) == 0x80) continue;  // inside a character
            const float cx = tx + font->CalcTextSizeA(px, FLT_MAX, 0.0f, edit.text.c_str(), edit.text.c_str() + i).x;
            if (cx >= edit.click_x) {
              // nearer to this boundary or to the one before
              size_t prev = i;
              while (prev > 0 && (static_cast<unsigned char>(edit.text[--prev]) & 0xC0) == 0x80) {
              }
              const float px_prev = tx + font->CalcTextSizeA(px, FLT_MAX, 0.0f, edit.text.c_str(), edit.text.c_str() + prev).x;
              best = static_cast<int>(i > 0 && edit.click_x - px_prev < cx - edit.click_x ? prev : i);
              break;
            }
          }
          if (edit.click_x < tx) best = 0;
          edit.pending_caret = best;
          edit.cursor_to_end = false;
        }
        edit.click_x = -1.0f;
      }
      ImGui::SetCursorScreenPos(ImVec2(bx, y0));
      if (!shown) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
      // PushFont takes the size before DPI scaling (px already includes DPI)
      ImGui::PushFont(font, px / std::max(0.01f, ImGui::GetStyle().FontScaleDpi * ImGui::GetStyle().FontScaleMain));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, st.fill_color ? ToCol(st.fill_color) : t.cell_bg);
      ImGui::PushStyleColor(ImGuiCol_Text, st.font_color ? ToCol(st.font_color) : t.cell_text);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ctx.pad, pad_y));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
      // always in wrap mode: no horizontal scrolling (so the frame where text is inserted before the box widens never sticks scrolled)
      EditField(edit, "##celledit", shown ? ImVec2(w, h) : ImVec2(1, 1), act, false, true);
      (void)wrap;
      ImGui::PopStyleVar(shown ? 4 : 5);
      ImGui::PopStyleColor(2);
      ImGui::PopFont();
      if (shown) {
        edit.field_rect = ImRect(ImVec2(bx, y0), ImVec2(bx + w, y0 + h));
        dl->AddRect(ImVec2(bx - 1, y0 - 1), ImVec2(bx + w + 1, y0 + h + 1), t.selection_border, 0.0f, std::max(2.0f, S(2)));
      } else {
        edit.field_rect = ImRect();  // clicks on the sheet point (or commit) as usual
      }
      // keyboard pointing: arrow keys move the reference (Shift extends)
      if (act.point_move) {
        if (!edit.kpoint) {
          edit.kpoint = true;
          edit.kp_r = edit.kp_ar = edit.r;
          edit.kp_c = edit.kp_ac = edit.c;
        }
        const int dirn = act.point_move;
        // an arrow without Shift moves on from the reference's anchor, as the active cell moves on the sheet; Shift
        // moves its other end (Excel 16, measured: AutoSum's B2:B5 in B6, then Down points at B3)
        if (!act.point_extend) {
          edit.kp_r = edit.kp_ar;
          edit.kp_c = edit.kp_ac;
        }
        int64_t nr = edit.kp_r, nc = edit.kp_c;
        // NextVisible finds the first visible index starting at (and including) its start: step one first; stay put when none
        auto step = [](const model::DimIndex& d, int64_t i, int dir, int64_t limit) {
          const int64_t n = d.NextVisible(i + dir, dir, limit);
          return n < 0 ? i : n;
        };
        const uint32_t kr = edit.kp_r, kc = edit.kp_c;
        if (dirn <= 4 && act.point_ctrl) {
          // Ctrl+arrow: to the edge of the data, as the active cell moves (Ctrl+Shift+Down: =SUM(A1:A500)); with Shift
          // along the anchor's column / row, as on the sheet
          if (dirn <= 2) nr = engine::CtrlJump(sheet, kr, act.point_extend ? edit.kp_ac : kc, true, dirn == 1 ? +1 : -1);
          else nc = engine::CtrlJump(sheet, act.point_extend ? edit.kp_ar : kr, kc, false, dirn == 3 ? +1 : -1);
        } else {
          if (dirn == 1) nr = step(*sheet.rows, nr, +1, row_limit);
          if (dirn == 2) nr = step(*sheet.rows, nr, -1, row_limit);
          if (dirn == 3) nc = step(*sheet.cols, nc, +1, col_limit);
          if (dirn == 4) nc = step(*sheet.cols, nc, -1, col_limit);
        }
        const int64_t page = std::max<int64_t>(1, view.vis_rows - 1);
        if (dirn == 5) nr = std::min<int64_t>(row_limit - 1, nr + page);
        if (dirn == 6) nr = std::max<int64_t>(0, nr - page);
        if (dirn == 7) nr = 0, nc = 0;
        if (dirn == 10) nc = 0;  // Home: column A of the row pointed at (Shift: the reference grows to it)
        if (dirn == 11) {        // Ctrl+End: the last cell of the used area
          const core::CellPos lc = engine::LastCell(sheet);
          nr = lc.row;
          nc = lc.col;
        }
        nr = std::clamp<int64_t>(nr, 0, row_limit - 1);
        nc = std::clamp<int64_t>(nc, 0, col_limit - 1);
        edit.kp_r = static_cast<uint32_t>(nr);
        edit.kp_c = static_cast<uint32_t>(nc);
        if (!act.point_extend) {
          edit.kp_ar = edit.kp_r;
          edit.kp_ac = edit.kp_c;
        }
        // Ctrl+Space / Shift+Space: the columns / rows of the reference pointed at (A:A, 1:1)
        insert_ref(edit.kp_ar, edit.kp_ac, edit.kp_r, edit.kp_c, dirn == 8 ? 1 : dirn == 9 ? 2 : 0);
        EnsureVisible(sheet, view, edit.kp_r, edit.kp_c);
        act.point_move = 0;
      }
      break;
    }
  }

  ImGui::PopFont();

  // ---- Vertical scroll bar ----
  if (opt.show_vscroll) {
    const ImRect vr(ImVec2(rect.Max.x - sb, outer.Min.y), ImVec2(rect.Max.x, rect.Max.y - bottom_h));
    const int64_t used_r = sheet.has_used ? sheet.used.r1 + 1 : 1;
    Scrollbar("##vscroll", true, vr, view.v, *sheet.rows, view.split ? 0 : fr_end, row_limit, used_r, view.last_bottom_row, selst.act_r, main.rect.GetHeight(), sc,
              std::max<int64_t>(1, view.vis_rows - 1));
  }

  // ---- Bottom row: sheet tabs + splitter + horizontal scroll bar ----
  const ImRect bottom(ImVec2(outer.Min.x, rect.Max.y - bottom_h), rect.Max);
  if (bottom_h <= 0.0f) {
    (void)cells_hovered;
    ImGui::PopID();
    return result;
  }
  dl->AddRectFilled(bottom.Min, bottom.Max, t.sheettab_bg);
  dl->AddRectFilled(bottom.Min, ImVec2(bottom.Max.x, bottom.Min.y + 1), t.sheettab_border);
  float hs_x0 = bottom.Min.x;
  if (tabs) hs_x0 = DrawSheetTabs(tabs, bottom, act);
  {
    const ImRect hr(ImVec2(hs_x0, bottom.Min.y + S(4)), ImVec2(bottom.Max.x - sb, bottom.Max.y - S(4)));
    if (opt.show_hscroll && hr.GetWidth() > S(40)) {
      const int64_t used_c = sheet.has_used ? sheet.used.c1 + 1 : 1;
      Scrollbar("##hscroll", false, hr, view.h, *sheet.cols, view.split ? 0 : fc_end, col_limit, used_c, view.last_right_col, selst.act_c, main.rect.GetWidth(), sc,
                std::max<int64_t>(1, view.vis_cols - 1));
    }
  }

  (void)cells_hovered;
  ImGui::PopID();
  return result;
}

void SetEditText(EditState& e, std::string text, int caret) {
  e.text = text;
  e.pending_text = std::move(text);
  e.pending_replace = true;
  e.pending_caret = caret;
  e.pending_sel_from = -1;  // (a selection the box was to open with belonged to the old text)
  // the caret is where it will be at once: a key the next frame (F4, an arrow, a typed operator) reads it before the box
  // has taken the new text
  if (caret >= 0) e.caret = e.sel_start = e.sel_end = std::min(caret, static_cast<int>(e.text.size()));
}

void DrawRuleIcon(ImDrawList* dl, const ImRect& b, int style, int index) { DrawCfIcon(dl, b, style, index); }

}  // namespace yc::render
