// SPDX-License-Identifier: Apache-2.0
// border_draw.cpp - Home > Borders > Draw Borders: Draw Border, Draw Border Grid, Erase Border, Line Color and Line
// Style, as in Excel. While a mode is on, a click on a cell edge draws (or erases) that edge and a drag draws the
// outline (Draw Border), every line (Draw Border Grid) or erases every line (Erase Border) of the cells dragged over;
// the selection stays where it was. Esc, or the same button again, ends the mode.
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace yc::app {


namespace {

using engine::Document;
using ui::S;

struct BorderPen {
  int mode = 0;  // 0 off, 1 Draw Border, 2 Draw Border Grid, 3 Erase Border
  model::LineStyle line = model::LineStyle::Thin;
  uint32_t color = 0;  // 0 = automatic
  bool armed = false;  // a click on the cells started: act on the mouse release
  uint32_t sheet = 0;
  render::Selection keep;  // the selection before the click (put back afterwards)
  bool have_keep = false;
} g_pen;

// the 13 line styles in the order of Excel's Line Style list
const model::LineStyle kStyles[] = {model::LineStyle::Hair,         model::LineStyle::Dotted,           model::LineStyle::DashDotDot,
                                    model::LineStyle::DashDot,      model::LineStyle::Dashed,           model::LineStyle::Thin,
                                    model::LineStyle::MediumDashDotDot, model::LineStyle::SlantDashDot, model::LineStyle::MediumDashDot,
                                    model::LineStyle::MediumDashed, model::LineStyle::Medium,           model::LineStyle::Thick,
                                    model::LineStyle::Double};

void SetMode(AppState& s, int mode) {
  g_pen.mode = g_pen.mode == mode ? 0 : mode;  // the same button again ends it
  g_pen.armed = false;
  if (g_pen.mode) {
    s.format_painter = false;
    g_pen.keep = s.FocusView().sel;
    g_pen.have_keep = true;
  }
}

// a line drawn like the cell border it stands for (a picture in the Line Style list)
void DrawLineSample(ImDrawList* dl, ImVec2 a, float w, model::LineStyle ls, ImU32 col) {
  using L = model::LineStyle;
  const float t = (ls == L::Medium || ls == L::MediumDashed || ls == L::MediumDashDot || ls == L::MediumDashDotDot || ls == L::SlantDashDot) ? S(2) : ls == L::Thick ? S(3) : 1.0f;
  if (ls == L::Double) {
    dl->AddLine(ImVec2(a.x, a.y - S(1.5f)), ImVec2(a.x + w, a.y - S(1.5f)), col, 1.0f);
    dl->AddLine(ImVec2(a.x, a.y + S(1.5f)), ImVec2(a.x + w, a.y + S(1.5f)), col, 1.0f);
    return;
  }
  std::vector<float> dash;  // on, off, on, off...
  switch (ls) {
    case L::Hair: dash = {S(1), S(1)}; break;
    case L::Dotted: dash = {S(2), S(2)}; break;
    case L::Dashed:
    case L::MediumDashed: dash = {S(6), S(3)}; break;
    case L::DashDot:
    case L::MediumDashDot:
    case L::SlantDashDot: dash = {S(8), S(3), S(3), S(3)}; break;
    case L::DashDotDot:
    case L::MediumDashDotDot: dash = {S(8), S(3), S(3), S(3), S(3), S(3)}; break;
    default: break;
  }
  if (dash.empty()) {
    dl->AddLine(a, ImVec2(a.x + w, a.y), col, t);
    return;
  }
  float x = a.x;
  for (size_t k = 0; x < a.x + w; k = (k + 1) % dash.size()) {
    const float len = dash[k];
    if (k % 2 == 0) dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + len, a.x + w), a.y), col, t);
    x += len;
  }
}

}  // namespace

bool BorderDrawOn() { return g_pen.mode != 0; }

void BorderDrawOff() {
  g_pen.mode = 0;
  g_pen.armed = false;
}

const char* BorderDrawStatus() {
  switch (g_pen.mode) {
    case 1: return TR("Draw Border: click a cell's edge, or drag over cells to outline them");
    case 2: return TR("Draw Border Grid: click or drag over cells to draw every line");
    case 3: return TR("Erase Border: click a cell's edge, or drag over cells to erase their lines");
    default: return nullptr;
  }
}

// the grid saw a click on the cells while a mode is on
void BorderDrawArm(AppState& s) {
  if (!g_pen.mode) return;
  g_pen.armed = true;
  g_pen.sheet = s.focus_sheet;
}

// every frame: the selection to put back, and the drawing when the mouse goes up
void BorderDrawFrame(AppState& s) {
  if (!g_pen.mode) return;
  if (!g_pen.armed) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      g_pen.keep = s.FocusView().sel;
      g_pen.have_keep = true;
    }
    return;
  }
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
  g_pen.armed = false;
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.engine || s.focus_sheet != g_pen.sheet) return;
  auto& v = s.FocusView();
  const Rect r = SelRect(s);
  const int mode = g_pen.mode;
  const model::LineStyle line = g_pen.line;
  const uint32_t color = g_pen.color, sheet = s.focus_sheet;
  if (r.IsCell() && mode != 2) {
    // one cell: the edge nearest to where the mouse went down
    const ImRect cell = CellsScreenRect(*sh, v, r);
    const ImVec2 m = ImGui::GetIO().MouseClickedPos[0];
    const float d[4] = {std::fabs(m.x - cell.Min.x), std::fabs(m.y - cell.Min.y), std::fabs(cell.Max.x - m.x), std::fabs(cell.Max.y - m.y)};
    const int edge = static_cast<int>(std::min_element(d, d + 4) - d);  // left, top, right, bottom
    static const engine::BorderPreset kSide[4] = {engine::BorderPreset::Left, engine::BorderPreset::Top, engine::BorderPreset::Right, engine::BorderPreset::Bottom};
    s.engine->PostUser(mode == 3 ? TR("Erase Border") : TR("Draw Border"), [=](Document& d) {
      if (mode == 3) {
        d.SetBorders(sheet, r, kSide[edge], model::LineStyle::None, 0);
        // the facing edge of the cell next to it is the same line
        if (edge == 0 && r.c0 > 0) d.SetBorders(sheet, Rect::Cell(r.r0, r.c0 - 1), engine::BorderPreset::Right, model::LineStyle::None, 0);
        if (edge == 1 && r.r0 > 0) d.SetBorders(sheet, Rect::Cell(r.r0 - 1, r.c0), engine::BorderPreset::Bottom, model::LineStyle::None, 0);
        if (edge == 2) d.SetBorders(sheet, Rect::Cell(r.r0, r.c0 + 1), engine::BorderPreset::Left, model::LineStyle::None, 0);
        if (edge == 3) d.SetBorders(sheet, Rect::Cell(r.r0 + 1, r.c0), engine::BorderPreset::Top, model::LineStyle::None, 0);
      } else {
        d.SetBorders(sheet, r, kSide[edge], line, color);
      }
    });
  } else {
    const engine::BorderPreset p = mode == 1 ? engine::BorderPreset::Outside : mode == 2 ? engine::BorderPreset::All : engine::BorderPreset::None;
    s.engine->PostUser(mode == 3 ? TR("Erase Border") : mode == 2 ? TR("Draw Border Grid") : TR("Draw Border"), [=](Document& d) {
      d.SetBorders(sheet, r, p, mode == 3 ? model::LineStyle::None : line, mode == 3 ? 0 : color);
    });
  }
  if (g_pen.have_keep) v.sel = g_pen.keep;  // the cells drawn on aren't selected (Excel)
}

uint32_t BorderPenColor() { return g_pen.color; }

void RegisterBorderDrawCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    c.toggle = true;
    return c;
  };
  R.Add(make("BorderDraw", TR("Draw Border"), ui::icon::BorderOutside, TR("Draw borders on cells: click an edge, or drag to outline cells."))).execute =
      [&s](ui::Command&) { SetMode(s, 1); };
  R.Add(make("BorderDrawGrid", TR("Draw Border Grid"), ui::icon::BorderAll, TR("Draw every line of the cells you click or drag over."))).execute =
      [&s](ui::Command&) { SetMode(s, 2); };
  R.Add(make("BorderErase", TR("Erase Border"), ui::icon::BorderNone, TR("Erase borders: click an edge, or drag over cells."))).execute =
      [&s](ui::Command&) { SetMode(s, 3); };
  {
    ui::Command c = make("BorderLineColor", TR("Line Color"), ui::icon::FontColor, TR("Change the color of the borders you draw."));
    c.toggle = false;
    R.Add(c).execute = [](ui::Command&) {};
  }
  {
    ui::Command c = make("BorderLineStyle", TR("Line Style"), ui::icon::BorderBottom, TR("Change the style of the borders you draw."));
    c.toggle = false;
    R.Add(c).execute = [](ui::Command&) {};
  }
  // one command per line style (the Line Style list); picking one turns Draw Border on, as in Excel
  for (int k = 0; k < static_cast<int>(std::size(kStyles)); ++k) {
    ui::Command c = make(("BorderLineStyle" + std::to_string(k + 1)).c_str(), "", nullptr, "");
    const char* kNames[] = {TR("Hair Line"), TR("Dotted Line"), TR("Dash Dot Dot Line"), TR("Dash Dot Line"), TR("Dashed Line"), TR("Thin Line"),
                            TR("Medium Dash Dot Dot Line"), TR("Slanted Dash Dot Line"), TR("Medium Dash Dot Line"), TR("Medium Dashed Line"),
                            TR("Medium Line"), TR("Thick Line"), TR("Double Line")};
    c.label = kNames[k];
    const model::LineStyle ls = kStyles[k];
    R.Add(c).execute = [&s, ls](ui::Command&) {
      g_pen.line = ls;
      if (g_pen.mode == 0 || g_pen.mode == 3) SetMode(s, 1);
    };
  }
  // the pictures of the Line Style list
  auto& st = ui::ribbon::St();
  st.menu_picture = [prev = st.menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
    if (cmd.rfind("BorderLineStyle", 0) == 0 && cmd.size() > 15) {
      const int k = std::atoi(cmd.c_str() + 15) - 1;
      if (k < 0 || k >= static_cast<int>(std::size(kStyles))) return prev ? prev(cmd, dl, at, h) : 0.0f;
      const float w = S(90);
      if (dl) {
        DrawLineSample(dl, ImVec2(at.x, at.y + h * 0.5f), w, kStyles[k], IM_COL32(0x20, 0x20, 0x20, 255));
        if (g_pen.line == kStyles[k]) dl->AddRect(ImVec2(at.x - S(3), at.y + S(2)), ImVec2(at.x + w + S(3), at.y + h - S(2)), IM_COL32(0x21, 0x73, 0x46, 255));
      }
      return w;
    }
    return prev ? prev(cmd, dl, at, h) : 0.0f;
  };
  // Line Color: Excel's palette in the submenu, Automatic first; picking a color turns Draw Border on
  st.submenu_body = [prev = st.submenu_body, &s](const std::string& id) -> bool {
    if (id == "TabColor") {
      // Home > Format > Tab Color: Excel's palette in the submenu (No Color, theme colors kept as theme colors,
      // standard colors), then More Colors...
      ImGui::Dummy(ImVec2(1, S(4)));
      ImGui::Indent(S(6));
      uint32_t ref = 0;
      if (ui::ribbon::PaletteSwatches(&ref, TR("No Color")) && s.engine) {
        const uint32_t sheet = s.focus_sheet;
        s.engine->PostUser(TR("Tab Color"), [sheet, ref](Document& d) { d.SetTabColor(sheet, ref); });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::Selectable(TR("More Colors..."))) {
        ImGui::CloseCurrentPopup();
        ui::Commands().Execute("TabColor");
      }
      ImGui::Unindent(S(6));
      return true;
    }
    if (id != "BorderLineColor") return prev ? prev(id) : false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
    ImGui::Dummy(ImVec2(1, S(4)));
    ImGui::Indent(S(6));
    uint32_t ref = 0;
    if (ui::ribbon::PaletteSwatches(&ref, TR("Automatic"))) {
      g_pen.color = ref;
      if (g_pen.mode == 0 || g_pen.mode == 3) SetMode(s, 1);
      ImGui::CloseCurrentPopup();
    }
    ImGui::Unindent(S(6));
    ImGui::PopStyleVar();
    return true;
  };
}

void UpdateBorderDrawCommands() {
  auto& R = ui::Commands();
  if (auto* c = R.Find("BorderDraw")) c->pressed = g_pen.mode == 1;
  if (auto* c = R.Find("BorderDrawGrid")) c->pressed = g_pen.mode == 2;
  if (auto* c = R.Find("BorderErase")) c->pressed = g_pen.mode == 3;
}

}  // namespace yc::app
