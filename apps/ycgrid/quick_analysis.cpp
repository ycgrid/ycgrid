// SPDX-License-Identifier: Apache-2.0
// quick_analysis.cpp - Excel's Quick Analysis: the button at the corner of a selection with data (and Ctrl+Q) opens
// tabs of one-click tools: Formatting, Charts, Totals, Tables, Sparklines. Every tile runs a command YCGrid already has
// (conditional formats, charts, AutoSum, tables, sparklines); nothing here computes on its own.
#include "app_state.h"
#include "core/i18n.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

struct Tile {
  const char* cmd;
  const char* label;
  const char* icon;
};
struct Page {
  const char* name;
  std::vector<Tile> tiles;
};

const std::vector<Page>& Pages() {
  static const std::vector<Page> k = {
      {TR("Formatting"),
       {{"CfBarBlue", TR("Data Bars"), ui::icon::CondFormat},
        {"CfScaleGYR", TR("Color Scale"), ui::icon::ThemeColors},
        {"CfIcon3Arrows", TR("Icon Set"), ui::icon::SortFilter},
        {"CfGreater", TR("Greater Than"), ui::icon::Filter},
        {"CfTop10Percent", TR("Top 10%"), ui::icon::Forecast},
        {"CfClearSelection", TR("Clear Format"), ui::icon::Clear}}},
      {TR("Charts"),
       {{"InsertChartColumn", TR("Clustered Column"), ui::icon::ChartColumn},
        {"InsertChartColumnStacked", TR("Stacked Column"), ui::icon::ChartColumn},
        {"InsertChartLine", TR("Line"), ui::icon::ChartLine},
        {"InsertChartPie", TR("Pie"), ui::icon::ChartPie},
        {"InsertChartScatter", TR("Scatter"), ui::icon::ChartScatter},
        {"RecommendedCharts", TR("More Charts"), ui::icon::ChartCombo}}},
      {TR("Totals"),
       {{"AutoSum", TR("Sum"), ui::icon::AutoSum},
        {"AutoAverage", TR("Average"), ui::icon::AutoSum},
        {"AutoCount", TR("Count"), ui::icon::AutoSum},
        {"AutoMax", TR("Max"), ui::icon::AutoSum},
        {"AutoMin", TR("Min"), ui::icon::AutoSum}}},
      {TR("Tables"),
       {{"InsertTable", TR("Table"), ui::icon::Table},
        {"RecommendedPivotTables", TR("PivotTable"), ui::icon::PivotTable},
        {"InsertPivotTable", TR("More"), ui::icon::PivotTable}}},
      {TR("Sparklines"),
       {{"QaSparkLine", TR("Line##ctx_spark"), ui::icon::SparkLine},
        {"QaSparkColumn", TR("Column##ctx_spark"), ui::icon::SparkColumn},
        {"QaSparkWinLoss", TR("Win/Loss"), ui::icon::SparkWinLoss}}},
  };
  return k;
}

struct QaState {
  int page = 0;
  bool open_request = false;
  ImVec2 at{};  // where the popup opens: under the button
};
QaState& Q() {
  static QaState q;
  return q;
}

// the selection Quick Analysis offers its tools for: one area of more than one cell, with something in it
bool Offered(AppState& s, Rect* out) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap || s.edit.active) return false;
  const auto areas = SelAreas(s);
  if (areas.size() != 1 || areas[0].IsCell() || areas[0].Count() > 2000000) return false;
  const Rect a = areas[0];
  if (a.c1 - a.c0 + 1 >= core::kMaxCols || a.r1 - a.r0 + 1 >= core::kMaxRows) return false;  // whole rows / columns
  // runs every frame: the answer is kept until the workbook or the selection changes
  static struct {
    uint64_t version = ~0ull;
    uint32_t sheet = 0;
    Rect a{};
    bool data = false;
  } cache;
  if (cache.version != s.snap->version || cache.sheet != sh->id || !(cache.a == a)) {
    bool data = false;
    uint64_t seen = 0;
    for (uint32_t r = a.r0; !data && r <= a.r1 && seen < 200000; ++r) {
      for (uint32_t c = a.c0; !data && c <= a.c1 && seen < 200000; ++c, ++seen) data = !sh->GetValue(r, c).IsEmpty();
    }
    cache = {s.snap->version, sh->id, a, data};
  }
  if (!cache.data) return false;
  *out = a;
  return true;
}

// the column right of the data: one sparkline per row (Excel's Quick Analysis > Sparklines)
void QuickSparklines(AppState& s, int type) {
  Rect a{};
  if (!Offered(s, &a) || a.c1 + 1 >= core::kMaxCols) return;
  const uint32_t sheet = s.focus_sheet;
  const Rect loc{a.r0, a.c1 + 1, a.r1, a.c1 + 1};
  std::vector<model::Sparkline> items;
  if (!engine::BuildSparklines(sheet, a, loc, &items)) return;
  model::SparklineGroup g;
  g.type = type;
  g.items = std::move(items);
  s.engine->PostUser(TR("Insert Sparklines"), [sheet, g](Document& d) { d.AddSparklineGroup(sheet, g); });
}

}  // namespace

void RegisterQuickAnalysisCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon, const char* shortcut, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("QuickAnalysis", TR("Quick Analysis"), ui::icon::FlashFill, "Ctrl+Q", TR("Format, chart, total or turn the selected data into a table in one click."))).execute =
      [&s](ui::Command&) {
        Rect a{};
        if (!Offered(s, &a)) return;
        Q().open_request = true;
      };
  R.Add(make("QaSparkLine", TR("Line##ctx_spark"), ui::icon::SparkLine, "", TR("Line sparklines in the column right of the data."))).execute =
      [&s](ui::Command&) { QuickSparklines(s, 0); };
  R.Add(make("QaSparkColumn", TR("Column##ctx_spark"), ui::icon::SparkColumn, "", TR("Column sparklines in the column right of the data."))).execute =
      [&s](ui::Command&) { QuickSparklines(s, 1); };
  R.Add(make("QaSparkWinLoss", TR("Win/Loss"), ui::icon::SparkWinLoss, "", TR("Win / loss sparklines in the column right of the data."))).execute =
      [&s](ui::Command&) { QuickSparklines(s, 2); };
}

// every frame, over the sheet: the button at the selection's corner and the tools it opens
void DrawQuickAnalysis(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  QaState& q = Q();
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh || s.focus_sheet != sheet_id || s.focus_win) {
    q.open_request = false;
    return;
  }
  Rect a{};
  const bool popup = ImGui::IsPopupOpen("##quick_analysis");
  // the corner is taken by the button of a fill, a paste or an insert (Excel shows those instead)
  // (those of this sheet: a paste on another sheet keeps its own state there)
  const bool other = (s.last_fill.active && s.last_fill.sheet == sheet_id) || (s.last_paste.active && s.last_paste.sheet == sheet_id) ||
                     (s.last_insert.active && s.last_insert.sheet == sheet_id);
  if (!Offered(s, &a) || (other && !popup)) {
    q.open_request = false;
    return;
  }
  const ImVec2 br = g.to_screen(static_cast<double>(sh->cols->Offset(static_cast<int64_t>(a.c1) + 1)),
                                static_cast<double>(sh->rows->Offset(static_cast<int64_t>(a.r1) + 1)));
  if (!g.cells.Contains(br)) {
    q.open_request = false;
    return;
  }
  const float sz = S(20);
  const ImVec2 p(br.x + S(3), br.y + S(3));
  // shown once the mouse lets go of the selection, as Excel does (a press on the button itself keeps it, so it can be
  // clicked); Options > General: "Show Quick Analysis options on selection" off hides it (Ctrl+Q still opens it)
  const ImRect box(p, p + ImVec2(sz, sz));
  const bool shown = Config().show_quick_analysis || popup;
  if (shown && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || popup || box.Contains(ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left]))) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID("quick_analysis");
    const bool clicked = ImGui::InvisibleButton("##btn", ImVec2(sz, sz));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ui::NoteUiItem(TR("Quick Analysis"), ImRect(p, p + ImVec2(sz, sz)));
    dl->AddRectFilled(p, p + ImVec2(sz, sz), hovered ? IM_COL32(0xE1, 0xEC, 0xF7, 255) : IM_COL32(255, 255, 255, 255));
    dl->AddRect(p, p + ImVec2(sz, sz), IM_COL32(0xA0, 0xA0, 0xA0, 255));
    const ImVec2 ts = ui::F().icons->CalcTextSizeA(sz * 0.8f, FLT_MAX, 0, ui::icon::FlashFill);
    dl->AddText(ui::F().icons, sz * 0.8f, ImVec2(p.x + (sz - ts.x) * 0.5f, p.y + (sz - ts.y) * 0.5f), IM_COL32(0x21, 0x73, 0x46, 255), ui::icon::FlashFill);
    if (hovered) ImGui::SetTooltip("%s", TR("Quick Analysis (Ctrl+Q)"));
    if (clicked) q.open_request = true;
  }
  if (q.open_request) {
    q.open_request = false;
    q.page = 0;  // it opens on Formatting each time, as Excel's does
    q.at = ImVec2(p.x, p.y + sz + S(2));
    ImGui::SetNextWindowPos(q.at);
    ImGui::OpenPopup("##quick_analysis");
  }
  if (!ImGui::BeginPopup("##quick_analysis")) return;
  const auto& pages = Pages();
  q.page = std::clamp(q.page, 0, static_cast<int>(pages.size()) - 1);
  // the tabs: Formatting, Charts, Totals, Tables, Sparklines
  for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
    if (i) ImGui::SameLine(0, S(14));
    const char* name = pages[static_cast<size_t>(i)].name;
    const ImVec2 ts = ImGui::CalcTextSize(name);
    if (ImGui::Selectable(name, q.page == i, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(ts.x, 0))) q.page = i;
    if (q.page == i) {
      const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ImGui::GetWindowDrawList()->AddLine(ImVec2(r.Min.x, r.Max.y + S(1)), ImVec2(r.Max.x, r.Max.y + S(1)), IM_COL32(0x21, 0x73, 0x46, 255), S(2));
    }
  }
  ImGui::Dummy(ImVec2(1, S(6)));
  // the tiles: an icon over its name; a click runs the command on the selection
  const float tw = S(74), th = S(62);
  const auto& tiles = pages[static_cast<size_t>(q.page)].tiles;
  std::string ran;
  for (size_t i = 0; i < tiles.size(); ++i) {
    if (i) ImGui::SameLine(0, S(4));
    const Tile& t = tiles[i];
    ImGui::PushID(t.cmd);
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(tw, th));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    std::string label = t.label;
    if (const auto k = label.find("##"); k != std::string::npos) label.erase(k);
    ui::NoteUiItem(label.c_str(), ImRect(tp, tp + ImVec2(tw, th)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) dl->AddRectFilled(tp, tp + ImVec2(tw, th), IM_COL32(0xD5, 0xE8, 0xD4, 255), S(2));
    const float isz = S(26);
    const ImVec2 is = ui::F().icons->CalcTextSizeA(isz, FLT_MAX, 0, t.icon);
    dl->AddText(ui::F().icons, isz, ImVec2(tp.x + (tw - is.x) * 0.5f, tp.y + S(6)), IM_COL32(0x21, 0x73, 0x46, 255), t.icon);
    const ImVec2 ls = ImGui::CalcTextSize(label.c_str(), nullptr, false, tw - S(4));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tp.x + std::max(S(2), (tw - ls.x) * 0.5f), tp.y + S(38)), ImGui::GetColorU32(ImGuiCol_Text),
                label.c_str(), nullptr, tw - S(4));
    if (clicked) ran = t.cmd;
  }
  if (!ran.empty()) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    ui::Commands().Execute(ran);
    return;
  }
  ImGui::Dummy(ImVec2(1, S(2)));
  ImGui::TextDisabled("%s", pages[static_cast<size_t>(q.page)].name == pages[0].name ? TR("Conditional Formatting uses rules to highlight interesting data.")
                                                                                      : TR("Click a tool to apply it to the selected data."));
  ImGui::EndPopup();
}

}  // namespace yc::app
