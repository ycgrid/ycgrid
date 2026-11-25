// SPDX-License-Identifier: Apache-2.0
// chart_type_ui.cpp - Insert Chart and Change Chart Type, the way Excel shows them (PLAN §9)
//
// The chart kinds on the left (Recommended, Column, Line, Pie...), their variants across the top as small pictures of
// the chart, a live preview of the chosen one drawn from the selected cells (Insert) or from the chart itself (Change),
// and under it the data laid out by rows and by columns to pick from. Combo lists every series with its own chart type
// and a Secondary Axis box.
#include "app_state.h"
#include "core/i18n.h"
#include "engine/chart_layout.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace yc::app {

using engine::Document;
using model::ChartObj;
using model::ChartType;
using ui::S;

namespace {

using T = ChartType;

struct Kind {
  const char* name;
  const char* icon;
  std::vector<T> types;  // empty: Combo (or the recommended charts, chosen from the data)
  bool recommended = false;
};

const std::vector<Kind>& Kinds() {
  static const std::vector<Kind> k = {
      {TR("Recommended"), ui::icon::ChartCombo, {}, true},
      {TR("Column"), ui::icon::ChartColumn, {T::ColumnClustered, T::ColumnStacked, T::ColumnStacked100, T::Column3D, T::ColumnClustered3D, T::ColumnStacked3D}},
      {TR("Line"), ui::icon::ChartLine, {T::Line, T::LineStacked, T::LineStacked100, T::LineMarkers, T::LineMarkersStacked, T::LineMarkersStacked100, T::Line3D}},
      {TR("Pie"), ui::icon::ChartPie, {T::Pie, T::Pie3D, T::PieOfPie, T::BarOfPie, T::Doughnut}},
      {TR("Bar"), ui::icon::ChartColumn, {T::BarClustered, T::BarStacked, T::BarStacked100, T::BarClustered3D}},
      {TR("Area"), ui::icon::ChartLine, {T::Area, T::AreaStacked, T::AreaStacked100, T::Area3D}},
      {TR("X Y (Scatter)"), ui::icon::ChartScatter, {T::Scatter, T::ScatterSmooth, T::ScatterSmoothNoMarkers, T::ScatterLines, T::ScatterLinesNoMarkers, T::Bubble}},
      {TR("Map"), ui::icon::ChartTreemap, {T::FilledMap}},
      {TR("Stock"), ui::icon::ChartColumn, {T::StockHLC, T::StockOHLC, T::StockVHLC, T::StockVOHLC}},
      {TR("Surface"), ui::icon::ChartRadar, {T::Surface, T::SurfaceWireframe, T::SurfaceContour}},
      {TR("Radar"), ui::icon::ChartRadar, {T::Radar, T::RadarMarkers, T::RadarFilled}},
      {TR("Treemap"), ui::icon::ChartTreemap, {T::Treemap}},
      {TR("Sunburst"), ui::icon::ChartSunburst, {T::Sunburst}},
      {TR("Histogram"), ui::icon::ChartHistogram, {T::Histogram, T::Pareto}},
      {TR("Box & Whisker"), ui::icon::ChartBoxWhisker, {T::BoxWhisker}},
      {TR("Waterfall"), ui::icon::ChartWaterfall, {T::Waterfall}},
      {TR("Funnel"), ui::icon::ChartFunnel, {T::Funnel}},
      {TR("Combo"), ui::icon::ChartCombo, {}},
  };
  return k;
}

// the types a series of a combo chart can take
const T kComboTypes[] = {T::ColumnClustered, T::ColumnStacked, T::Line, T::LineMarkers, T::Area, T::Scatter};

struct TypeDialog {
  bool change = false;  // Change Chart Type (else Insert Chart)
  uint32_t sheet = 0, id = 0;
  int kind = 0, variant = 0;
  // the preview: the chart as it would be
  ChartObj base;  // Change: the chart; Insert: built from the selection for built_type
  T built_type = T::Line;
  bool built = false, has_data = false;
  Rect anchor{};
  int by_rows = -1;  // the data laid out by rows (1) or by columns (0), as picked under the preview; -1 Excel's choice
  std::vector<int> combo_type;
  std::vector<char> combo_y2;
  std::vector<T> recommended;  // the charts Recommended offers for this data
  int highlight = -1;          // Change Series Chart Type: the series shown first in Combo
  // the variants' small pictures, made again when the kind or the layout changes
  std::vector<ChartObj> thumbs;
  int thumbs_kind = -1, thumbs_rows = -2;
  std::string pivot;  // Insert Chart for a new PivotChart of this PivotTable (base: its chart, placed beside the report)
};

TypeDialog& TD() {
  static TypeDialog d;
  return d;
}

const std::vector<T>& TypesOf(const TypeDialog& d, int kind) {
  const Kind& k = Kinds()[static_cast<size_t>(kind)];
  return k.recommended ? d.recommended : k.types;
}

void FindKind(T t, int* kind, int* variant) {
  const auto& ks = Kinds();
  for (size_t k = 0; k < ks.size(); ++k) {
    if (ks[k].recommended) continue;
    for (size_t v = 0; v < ks[k].types.size(); ++v) {
      if (ks[k].types[v] == t) {
        *kind = static_cast<int>(k);
        *variant = static_cast<int>(v);
        return;
      }
    }
  }
}

bool IsCombo(const TypeDialog& d) {
  const Kind& k = Kinds()[static_cast<size_t>(d.kind)];
  return !k.recommended && k.types.empty();
}

T Chosen(const TypeDialog& d) {
  const auto& types = TypesOf(d, d.kind);
  if (types.empty()) return T::ColumnClustered;
  return types[static_cast<size_t>(std::clamp(d.variant, 0, static_cast<int>(types.size()) - 1))];
}

void InitCombo(TypeDialog& d, const ChartObj& c) {
  const size_t n = c.series.size();
  d.combo_type.assign(n, static_cast<int>(T::ColumnClustered));
  d.combo_y2.assign(n, 0);
  bool any = false;
  for (size_t i = 0; i < n; ++i) {
    if (c.series[i].type >= 0) {
      d.combo_type[i] = c.series[i].type;
      any = true;
    }
    d.combo_y2[i] = c.series[i].axis_group == 1;
  }
  // Excel's Clustered Column - Line: the last series a line
  if (!any && n >= 2) d.combo_type[n - 1] = static_cast<int>(T::Line);
}

// the data laid out again by rows or by columns (Insert Chart's two small previews)
void Orient(AppState& s, ChartObj* c, bool rows) {
  if (c->source.empty() || c->source_by_rows == rows) return;
  uint32_t sid = 0;
  std::vector<Rect> areas;
  if (!ResolveAreasText(s.snap->wb, s.focus_sheet, c->source, &sid, &areas)) return;
  const model::Sheet* sh = s.snap->Sheet(sid);
  ChartObj tmp = *c;
  if (sh && BuildSeriesFromAreas(s.snap->wb, *sh, areas, c->type, &tmp, rows)) *c = tmp;
}

// the chart the dialog would make
ChartObj PreviewChart(AppState& s, TypeDialog& d, int by_rows_override = -2) {
  const T t = Chosen(d);
  ChartObj c;
  if (d.change || !d.pivot.empty()) {
    c = d.base;
    ChangeChartType(s.snap->wb, c, t);
  } else {
    if (!d.built || d.built_type != t) {
      d.has_data = BuildChartFromSelection(s, t, &d.base, &d.anchor);
      d.built = true;
      d.built_type = t;
    }
    c = d.base;
    const int rows = by_rows_override != -2 ? by_rows_override : d.by_rows;
    if (rows >= 0) Orient(s, &c, rows == 1);
  }
  for (size_t i = 0; i < c.series.size(); ++i) {
    if (IsCombo(d)) {
      c.series[i].type = i < d.combo_type.size() ? d.combo_type[i] : -1;
      c.series[i].axis_group = i < d.combo_y2.size() && d.combo_y2[i] ? 1 : 0;
    } else {
      c.series[i].type = -1;
    }
  }
  return c;
}

// Recommended Charts: a few charts that suit the shape of the data (Excel's suggestions)
std::vector<T> Recommend(AppState& s) {
  ChartObj base;
  Rect anchor;
  std::vector<T> r;
  auto add = [&](T t) {
    if (std::find(r.begin(), r.end(), t) == r.end() && r.size() < 6) r.push_back(t);
  };
  if (!BuildChartFromSelection(s, T::ColumnClustered, &base, &anchor)) return {T::ColumnClustered, T::Line, T::Pie, T::BarClustered};
  engine::ChartValues v;
  ResolveChartValues(*s.snap, s.focus_sheet, base, &v);
  const size_t ns = v.series.size();
  const int n = ns ? v.series[0].N() : 0;
  // open, high, low, close: a stock chart
  if (ns >= 3 && ns <= 5) {
    std::string names;
    for (const auto& se : v.series) {
      for (char ch : se.name) names.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      names += '|';
    }
    const bool hi_lo = names.find("high") != std::string::npos && names.find("low") != std::string::npos && names.find("close") != std::string::npos;
    const bool open = names.find("open") != std::string::npos, volume = names.find("volume") != std::string::npos;
    if (hi_lo && ns == 4 && open) add(T::StockOHLC);
    if (hi_lo && ns == 3) add(T::StockHLC);
    if (hi_lo && volume && ns == 4) add(T::StockVHLC);
    if (hi_lo && volume && open && ns == 5) add(T::StockVOHLC);
  }
  // numbers only, two or more columns: X and Y values
  if (v.cats.empty() && ns >= 1) {
    ChartObj sc;
    if (BuildChartFromSelection(s, T::Scatter, &sc, &anchor) && !sc.series.empty() && !sc.series[0].x_values.empty()) {
      add(T::Scatter);
      add(T::ScatterLines);
    }
  }
  bool positive = ns == 1;
  if (ns == 1) {
    for (int i = 0; i < n; ++i) positive = positive && !(v.series[0].Y()[i] < 0);
  }
  if (n > 12) {  // a long series: its trend
    add(T::Line);
    add(T::LineMarkers);
  }
  if (positive && n >= 2 && n <= 8) add(T::Pie);
  add(T::ColumnClustered);
  add(T::BarClustered);
  if (ns > 1) add(T::ColumnStacked);
  if (positive && n >= 2 && n <= 8) add(T::Doughnut);
  if (n > 12) add(T::Area);
  add(T::Line);
  return r;
}

void Finish(AppState& s, TypeDialog& d) {
  ChartObj c = PreviewChart(s, d);
  if (!d.pivot.empty()) {
    // a PivotChart: it follows its PivotTable's fields and filters
    c.id = 0;
    c.pivot = d.pivot;
    const uint32_t sheet = d.sheet;
    s.engine->Call([&](Document& doc) {
      doc.Begin(TR("Insert PivotChart"));
      doc.AddChart(sheet, c);
      doc.UpdatePivotCharts();
      doc.Commit();
      return 0;
    });
    return;
  }
  if (!d.change) {
    c.id = 0;
    InsertBuiltChart(s, std::move(c), d.anchor);
    return;
  }
  const uint32_t sheet = d.sheet, id = d.id;
  const T t = Chosen(d);
  const bool combo = IsCombo(d);
  const std::vector<int> types = d.combo_type;
  const std::vector<char> y2 = d.combo_y2;
  s.engine->PostUser(TR("Change Chart Type"), [sheet, id, t, combo, types, y2](Document& doc) {
    doc.UpdateChart(sheet, id, [&](ChartObj& x) {
      ChangeChartType(doc.wb(), x, t);
      for (size_t i = 0; i < x.series.size(); ++i) {
        x.series[i].type = combo && i < types.size() ? types[i] : -1;
        if (combo) x.series[i].axis_group = i < y2.size() && y2[i] ? 1 : 0;
      }
    });
  });
}

int KindIndex(bool recommended, bool combo) {
  const auto& ks = Kinds();
  for (size_t k = 0; k < ks.size(); ++k) {
    if (recommended && ks[k].recommended) return static_cast<int>(k);
    if (combo && !ks[k].recommended && ks[k].types.empty()) return static_cast<int>(k);
  }
  return 0;
}

// a small picture of a chart (a variant, or the data by rows / by columns): its button carries the name for tools
bool Thumb(AppState& s, const TypeDialog& d, ChartObj c, const char* name, uint32_t cache_id, bool on, ImVec2 size) {
  // the picture first, then the button over it: moving the cursor back with no item after it is an ImGui error
  // (ErrorCheckUsingSetCursorPosToExtendParentBoundaries: a debug build stopped on it in this dialog)
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, p + size, IM_COL32_WHITE);
  c.id = cache_id;
  c.interactive = false;
  c.has_title = false;
  c.has_legend = false;
  c.cat_axis.has_title = c.val_axis.has_title = false;
  for (auto& se : c.series) se.data_labels = false;
  if (!c.series.empty()) DrawChartBody(s, d.sheet, c, p + ImVec2(2, 2), size - ImVec2(4, 4), false, 0.55f);
  ImGui::SetCursorScreenPos(p);
  // its name is for tools (ui.items) and the tooltip; the frame below shows which one is chosen
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x21, 0x73, 0x46, 0x18));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x21, 0x73, 0x46, 0x30));
  // (a selectable in the dialog itself would close it: the dialog is a popup)
  const bool hit = ImGui::Selectable(name, on, ImGuiSelectableFlags_NoAutoClosePopups, size);
  ImGui::PopStyleColor(4);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name);
  dl->AddRect(p, p + size, on ? IM_COL32(0x21, 0x73, 0x46, 255) : IM_COL32(0xC8, 0xC8, 0xC8, 255), 0.0f, on ? 2.0f : 1.0f);
  return hit;
}

}  // namespace

void OpenChartTypeDialog(AppState& s, bool change, int combo_series, int insert_type, bool recommended) {
  TypeDialog& d = TD();
  d = TypeDialog{};
  d.change = change;
  d.recommended = Recommend(s);
  if (change) {
    if (!SelectedChartObj(s, &d.sheet, &d.id)) return;
    const model::Sheet* sh = s.snap->Sheet(d.sheet);
    const ChartObj* c = nullptr;
    for (const auto& x : *sh->charts) {
      if (x.id == d.id) c = &x;
    }
    if (!c) return;
    d.base = *c;
    bool combo = false;
    for (const auto& se : c->series) combo = combo || se.type >= 0;
    d.kind = 1;
    FindKind(c->type, &d.kind, &d.variant);
    // Change Series Chart Type (a series' right-click menu): Combo, with that series first in view
    if (combo || combo_series >= 0) d.kind = KindIndex(false, true);
    d.highlight = combo_series;
    InitCombo(d, *c);
  } else {
    d.sheet = s.focus_sheet;
    d.kind = recommended ? KindIndex(true, false) : 1;
    if (insert_type >= 0) FindKind(static_cast<T>(insert_type), &d.kind, &d.variant);
    if (insert_type == -2) d.kind = KindIndex(false, true);  // Insert > Combo > Create Custom Combo Chart
    PreviewChart(s, d);
    InitCombo(d, d.base);
  }
  s.OpenDialog(Dialog::InsertChart);
}

void OpenPivotChartDialog(AppState& s, uint32_t sheet, const ChartObj& base) {
  TypeDialog& d = TD();
  d = TypeDialog{};
  d.sheet = sheet;
  d.pivot = base.pivot;
  d.base = base;
  d.built = true;
  d.has_data = true;
  d.built_type = base.type;
  d.kind = 1;
  FindKind(base.type, &d.kind, &d.variant);
  InitCombo(d, d.base);
  s.OpenDialog(Dialog::InsertChart);
}

void DrawInsertChartDialog(AppState& s) {
  if (s.dialog != Dialog::InsertChart) return;
  TypeDialog& d = TD();
  const char* title = d.change ? TR("Change Chart Type###yc_dlg_chart") : TR("Insert Chart###yc_dlg_chart");
  if (s.dialog_open_request) {
    ImGui::OpenPopup("###yc_dlg_chart");
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(820), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !open) s.dialog = Dialog::None;
    return;
  }
  const auto& ks = Kinds();
  const int nk = static_cast<int>(ks.size());
  bool ok = false, cancel = false;
  // the kinds, chosen with the mouse or Up / Down
  const int before = d.kind;
  const bool moved = DlgListKeys(&d.kind, nk, [&ks](int i) { return ks[static_cast<size_t>(i)].name; });
  if (ImGui::BeginChild("##kinds", ImVec2(S(180), S(470)), ImGuiChildFlags_Borders)) {
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < nk; ++i) {
      ImGui::PushID(i);
      if (moved && d.kind == i) ImGui::SetScrollHereY();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      // the item carries the kind's name (tools find it by its label); its icon and text are drawn below
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
      const bool hit = ImGui::Selectable(ks[static_cast<size_t>(i)].name, d.kind == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, ImGui::GetFrameHeight()));
      ImGui::PopStyleColor();
      if (hit) {
        d.kind = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ok = true;
      }
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float fh = ImGui::GetFrameHeight();
      dl->AddText(ui::F().icons, fh * 0.7f, p + ImVec2(S(4), fh * 0.15f), ImGui::GetColorU32(ImGuiCol_Text), ks[static_cast<size_t>(i)].icon);
      dl->AddText(p + ImVec2(S(4) + fh, (fh - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), ks[static_cast<size_t>(i)].name);
      ImGui::PopID();
    }
    ImGui::PopItemFlag();
  }
  ImGui::EndChild();
  if (d.kind != before) d.variant = 0;
  ImGui::SameLine();
  ImGui::BeginGroup();
  const auto& types = TypesOf(d, d.kind);
  const float pw = S(600);
  // the variants of the kind: a small picture of each, drawn from the data
  if (!types.empty()) {
    const ImVec2 tsz(S(92), S(64));
    if (d.thumbs_kind != d.kind || d.thumbs_rows != d.by_rows || d.thumbs.size() != types.size()) {
      d.thumbs.clear();
      for (size_t v = 0; v < types.size(); ++v) {
        TypeDialog probe = d;  // (a copy: building another type's chart leaves the dialog's own as it is)
        probe.variant = static_cast<int>(v);
        d.thumbs.push_back(PreviewChart(s, probe));
      }
      d.thumbs_kind = d.kind;
      d.thumbs_rows = d.by_rows;
    }
    for (size_t v = 0; v < types.size(); ++v) {
      if (v) ImGui::SameLine();
      ImGui::PushID(static_cast<int>(v));
      if (Thumb(s, d, d.thumbs[v], ChartTypeName(types[v]), 0xFFFFFF10u + static_cast<uint32_t>(v), d.variant == static_cast<int>(v), tsz)) d.variant = static_cast<int>(v);
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ok = true;
      ImGui::PopID();
    }
  } else if (Kinds()[static_cast<size_t>(d.kind)].recommended) {
    ImGui::TextUnformatted(TR("No chart is recommended for this selection."));
  } else {
    ImGui::TextUnformatted(TR("Choose the chart type and axis for each series:"));
  }
  ImGui::Spacing();
  ImGui::TextUnformatted(IsCombo(d) ? TR("Custom Combination") : ChartTypeName(Chosen(d)));
  // the preview
  ChartObj pv = PreviewChart(s, d);
  pv.id = 0xFFFFFF01u;  // its own drawing cache
  pv.interactive = false;
  const bool orient = !d.change && !IsCombo(d) && !pv.source.empty() && !pv.series.empty();
  const float ph = IsCombo(d) ? S(240) : orient ? S(270) : S(360);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRect(p0 - ImVec2(1, 1), p0 + ImVec2(pw + 1, ph + 1), IM_COL32(0xC8, 0xC8, 0xC8, 255));
  if (pv.series.empty() && !d.change) {
    ImGui::Dummy(ImVec2(pw, ph));
    const char* msg = TR("No data selected: an empty chart is inserted; fill it with Select Data.");
    const ImVec2 ts = ImGui::CalcTextSize(msg, nullptr, false, pw - S(20));
    ImGui::GetWindowDrawList()->AddText(nullptr, 0, p0 + ImVec2((pw - ts.x) * 0.5f, (ph - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), msg, nullptr, pw - S(20));
  } else {
    DrawChartBody(s, d.sheet, pv, p0, ImVec2(pw, ph), false);
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(pw, ph));
  }
  // the data by rows and by columns, side by side: click one to use it (Excel's two previews)
  if (orient) {
    const ImVec2 tsz(S(150), S(84));
    const bool now_rows = pv.source_by_rows;
    for (int r = 0; r < 2; ++r) {
      if (r) ImGui::SameLine();
      ImGui::PushID(100 + r);
      const bool rows = r == 0;
      ChartObj c = PreviewChart(s, d, rows ? 1 : 0);
      if (Thumb(s, d, c, rows ? TR("Series in Rows") : TR("Series in Columns"), 0xFFFFFF20u + static_cast<uint32_t>(r), now_rows == rows, tsz)) d.by_rows = rows ? 1 : 0;
      ImGui::PopID();
    }
  }
  // Combo: each series' type and axis
  if (IsCombo(d)) {
    engine::ChartValues vals;
    ResolveChartValues(*s.snap, d.sheet, pv, &vals);
    if (d.combo_type.size() != pv.series.size()) InitCombo(d, pv);
    if (ImGui::BeginTable("##combo", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY, ImVec2(pw, S(110)))) {
      ImGui::TableSetupColumn(TR("Series Name"), ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn(TR("Chart Type"), ImGuiTableColumnFlags_WidthFixed, S(200));
      ImGui::TableSetupColumn(TR("Secondary Axis"), ImGuiTableColumnFlags_WidthFixed, S(110));
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < pv.series.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        if (static_cast<int>(i) == d.highlight) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(0xDD, 0xEB, 0xF7, 255));
          if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        }
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(i < vals.series.size() ? vals.series[i].name.c_str() : "");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##t", ChartTypeName(static_cast<T>(d.combo_type[i])))) {
          for (T ct : kComboTypes) {
            if (ImGui::Selectable(ChartTypeName(ct), d.combo_type[i] == static_cast<int>(ct))) d.combo_type[i] = static_cast<int>(ct);
          }
          ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        bool y2 = d.combo_y2[i] != 0;
        if (ImGui::Checkbox("##y2", &y2)) d.combo_y2[i] = y2 ? 1 : 0;
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndGroup();
  ImGui::Separator();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - S(176) - ImGui::GetStyle().ItemSpacing.x);
  if (DlgOkButton(TR("OK"), ImVec2(S(88), 0))) ok = true;
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(88), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
  if (ok) Finish(s, d);
  if (ok || cancel || !open) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

}  // namespace yc::app
