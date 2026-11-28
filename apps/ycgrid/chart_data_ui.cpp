// SPDX-License-Identifier: Apache-2.0
// chart_data_ui.cpp - choosing a chart's data the way Excel does (PLAN §9)
//
// - Range boxes (Excel's RefEdit): a box is filled by selecting cells on the sheet while it is armed (clicked, or its
//   button pressed; the button also shrinks the dialog to the box so the cells can be seen). Typing still works.
// - Select Data Source: the chart data range, the series (Add / Edit / Remove / Move Up / Move Down), the category
//   labels and Switch Row/Column. The chart follows every change live; OK makes them one undoable change, Cancel
//   drops them.
// - The selected chart's data is outlined on its sheet like Excel: series names red, categories purple, values blue.
//   Dragging a corner of the blue outline changes the data (the whole chart, or one series when a series is selected).
// - An empty chart shows a Select Data button in its middle.
#include "app_state.h"
#include "core/i18n.h"
#include "engine/chart_layout.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "grid/grid.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "yc_ui_items.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace yc::app {

using engine::Document;
using model::ChartObj;
using model::ChartType;
using ui::S;

namespace {

constexpr ImU32 kNameCol = IM_COL32(0xE0, 0x39, 0x3E, 255);  // series names
constexpr ImU32 kCatCol = IM_COL32(0x8E, 0x4F, 0xC0, 255);   // categories / X values
constexpr ImU32 kValCol = IM_COL32(0x2F, 0x6E, 0xD0, 255);   // values
constexpr ImU32 kArmedCol = IM_COL32(0x21, 0x73, 0x46, 255);  // the armed range box (Excel green)

// ---- range boxes ----

struct RefState {
  ImGuiID armed = 0;  // the box that cell selections go to
  uint32_t seen_sheet = 0;
  std::vector<Rect> seen;  // the selection when it was armed or last copied (a change fills the box)
  std::string armed_text;  // the armed box's text last frame (outlined on the sheet with moving dashes)
  int armed_frame = -10;   // the last frame the armed box was drawn (a box no longer shown lets go)
  bool once = false;       // let go after one selection (boxes outside a dialog, so later clicks on the sheet are free)
  bool filled = false;
};

RefState& RS() {
  static RefState r;
  return r;
}

void Arm(AppState& s, ImGuiID id) {
  RefState& rs = RS();
  if (rs.armed == id) return;
  rs.armed = id;
  rs.seen = SelAreas(s);
  rs.seen_sheet = s.focus_sheet;
  rs.armed_frame = ImGui::GetFrameCount();
  rs.once = rs.filled = false;
}

// a range box; returns true when its text changed (typed, or cells selected while it is armed). picked: its button
// was pressed (the dialog shrinks to this box or grows back). key names the box wherever it is drawn (the shrunk
// dialog draws the same box)
bool RefBox(AppState& s, const char* key, std::string* text, float width, bool* picked = nullptr, bool once = false) {
  RefState& rs = RS();
  ImGui::PushID(key);
  const ImGuiID me = ImHashStr(key);
  bool changed = false;
  if (rs.armed == me && rs.armed_frame < ImGui::GetFrameCount() - 1) rs.armed = 0;  // it wasn't shown: let go
  if (rs.armed == me) {
    std::vector<Rect> cur = SelAreas(s);
    if (s.focus_sheet != rs.seen_sheet || cur != rs.seen) {
      rs.seen_sheet = s.focus_sheet;
      rs.seen = cur;
      const model::Sheet* sh = s.snap->Sheet(s.focus_sheet);
      if (sh && !sh->chart_sheet) {
        *text = "=" + AreasAddress(s.snap->wb, s.focus_sheet, cur);
        changed = true;
        rs.filled = true;
      }
    }
    rs.armed_text = *text;
    rs.armed_frame = ImGui::GetFrameCount();
    if (rs.once && rs.filled && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) rs.armed = 0;  // the cells are chosen
  }
  char buf[2048];
  std::snprintf(buf, sizeof buf, "%s", text->c_str());
  const float bw = ImGui::GetFrameHeight() * 1.3f, sp = S(4);
  const bool armed = rs.armed == me;
  if (armed) {
    ImGui::PushStyleColor(ImGuiCol_Border, kArmedCol);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(1.0f, S(1.5f)));
  }
  ImGui::SetNextItemWidth(std::max(S(60), width - bw - sp));
  if (ImGui::InputText("##ref", buf, sizeof buf)) {
    *text = buf;
    changed = true;
  }
  if (ImGui::IsItemActivated()) {
    Arm(s, me);
    rs.once = once;
  }
  if (armed) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }
  ImGui::SameLine(0, sp);
  const float fh = ImGui::GetFrameHeight();
  ImGui::PushFont(ui::F().icons, ImGui::GetFontSize() * 0.95f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
  ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);  // Tab goes from box to box, as in Excel
  const bool press = ImGui::Button(ui::icon::RangePick, ImVec2(bw, fh));
  ImGui::PopItemFlag();
  ImGui::PopStyleVar();
  ImGui::PopFont();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Select the cells on the sheet"));
  if (press) {
    Arm(s, me);
    rs.once = once;
    if (picked) *picked = true;
  }
  ImGui::PopID();
  return changed;
}

std::string StripEq(std::string t) {
  size_t i = 0;
  while (i < t.size() && (t[i] == ' ' || t[i] == '=')) ++i;
  t.erase(0, i);
  while (!t.empty() && t.back() == ' ') t.pop_back();
  return t;
}

std::string WithEq(const std::string& t) { return t.empty() ? std::string() : "=" + StripEq(t); }

// "= 10, 20, 30, ..." beside a range box (Excel shows the values the box refers to)
std::string PreviewList(const std::vector<std::string>& items) {
  std::string out = "= ";
  size_t n = 0;
  for (const auto& it : items) {
    if (n++) out += ", ";
    if (out.size() > 60) {
      out += "...";
      break;
    }
    out += it;
  }
  return n ? out : std::string();
}

std::string Num(double v) {
  if (!std::isfinite(v)) return "";
  char b[48];
  std::snprintf(b, sizeof b, "%.10g", v);
  return b;
}

// the values a series box refers to, as text
std::vector<std::string> RangeTexts(AppState& s, uint32_t sheet, const std::string& text, size_t limit = 12) {
  std::vector<std::string> out;
  uint32_t sid = 0;
  std::vector<Rect> areas;
  if (!ResolveAreasText(s.snap->wb, sheet, StripEq(text), &sid, &areas)) return out;
  const model::Sheet* sh = s.snap->Sheet(sid);
  if (!sh) return out;
  for (const Rect& r : areas) {
    for (uint32_t row = r.r0; row <= r.r1 && out.size() < limit; ++row) {
      for (uint32_t col = r.c0; col <= r.c1 && out.size() < limit; ++col) {
        const core::Value v = sh->GetValue(row, col);
        const model::Style& st = s.snap->wb.styles->Get(sh->GetStyle(row, col));
        out.push_back(engine::DisplayTextOf(v, s.snap->wb.styles->NumberFormat(st)));
      }
    }
  }
  return out;
}

// an array constant ({1,2,3}): a series' values typed in Edit Series
bool ArrayConst(const std::string& text) {
  const std::string t = StripEq(text);
  return t.size() >= 2 && t.front() == '{' && t.back() == '}' && !engine::ParseNumberList(t).empty();
}

bool RangeOk(AppState& s, uint32_t sheet, const std::string& text) {
  uint32_t sid = 0;
  std::vector<Rect> areas;
  return ResolveAreasText(s.snap->wb, sheet, StripEq(text), &sid, &areas);
}

// ---- Select Data Source ----

struct SelectData {
  bool open = false;
  uint32_t sheet = 0, id = 0;
  ChartObj work;            // the chart as the dialog changes it (drawn instead of the saved one)
  bool started_empty = false;  // an empty chart takes its title and legend from the range chosen
  std::string range;        // the chart data range box ("=Sheet1!$A$1:$C$10")
  bool range_bad = false;
  int cur = 0;              // the series chosen in the list
  bool collapsed = false;   // shrunk to the range box being picked
  int collapsed_box = 0;    // 0 chart data range, 1 series name, 2 X values, 3 values, 4 axis labels
  bool focus = false;
  // Edit Series (a series added with Add is removed again by Cancel)
  bool edit = false, edit_new = false;
  int edit_index = -1;
  model::ChartSeries edit_backup;
  std::string e_name, e_x, e_y;
  // Axis Labels
  bool axis = false;
  std::string axis_text;
  std::vector<model::ChartSeries> axis_backup;
  // Hidden and Empty Cells (its settings as the dialog opened, for Cancel)
  bool hidden_open = false;
  bool h_visible = true, h_na = true;
  int h_blanks = 1;
};

SelectData& SD() {
  static SelectData d;
  return d;
}

// where each chart was drawn last (screen): the dialogs open over their chart, away from the data being selected
std::map<uint64_t, ImRect>& ChartRects() {
  static std::map<uint64_t, ImRect> m;
  return m;
}

void PlaceNearChart(const SelectData& d, ImVec2 size) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 pos = vp->GetCenter() - size * 0.5f;
  const auto it = ChartRects().find((static_cast<uint64_t>(d.sheet) << 32) | d.id);
  if (it != ChartRects().end()) pos = it->second.Min + ImVec2(S(8), S(8));
  pos.x = std::clamp(pos.x, vp->WorkPos.x, std::max(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - size.x));
  pos.y = std::clamp(pos.y, vp->WorkPos.y, std::max(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - size.y));
  ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
}

const ChartObj* LiveChart(const AppState& s, uint32_t sheet, uint32_t id) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return nullptr;
  for (const auto& c : *sh->charts) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

bool ScatterLike(const ChartObj& c) { return engine::IsScatterType(c.type); }
bool NoCategories(const ChartObj& c) {
  return c.type == ChartType::Histogram || c.type == ChartType::Pareto || c.type == ChartType::BoxWhisker;
}

// a series' name as shown in the list
std::string SeriesLabel(AppState& s, uint32_t sheet, const model::ChartSeries& se, size_t i) {
  if (!se.name.empty() && se.name[0] == '=') {
    const auto t = RangeTexts(s, sheet, se.name, 1);
    if (!t.empty() && !t[0].empty()) return t[0];
  } else if (!se.name.empty()) {
    return se.name;
  }
  return TR("Series") + std::to_string(i + 1);
}

// a new chart data range: the series are laid out again from it, each keeping its formatting
void ApplyRange(AppState& s, SelectData& d) {
  uint32_t sid = 0;
  std::vector<Rect> areas;
  const std::string t = StripEq(d.range);
  d.range_bad = !t.empty() && !ResolveAreasText(s.snap->wb, d.sheet, t, &sid, &areas);
  if (t.empty() || d.range_bad) return;
  const model::Sheet* sh = s.snap->Sheet(sid);
  if (!sh || sh->chart_sheet) {
    d.range_bad = true;
    return;
  }
  const bool fresh = d.started_empty;
  ChartObj tmp = d.work;
  const bool by_rows = fresh || d.work.series.empty() || d.work.source.empty() ? PlotByRows(areas) : d.work.source_by_rows;
  if (!BuildSeriesFromAreas(s.snap->wb, *sh, areas, d.work.type, &tmp, by_rows)) {
    d.range_bad = true;
    return;
  }
  KeepSeriesFormat(d.work.series, &tmp.series);
  d.work.series = tmp.series;
  d.work.source = tmp.source;
  d.work.source_by_rows = tmp.source_by_rows;
  if (fresh) {
    d.work.has_legend = tmp.has_legend;
    d.work.has_title = tmp.has_title;
    d.work.title = tmp.title;
  }
  d.cur = std::clamp(d.cur, 0, std::max(0, static_cast<int>(d.work.series.size()) - 1));
}

void SwitchRowCol(AppState& s, SelectData& d) {
  uint32_t sid = 0;
  std::vector<Rect> areas;
  if (d.work.source.empty() || !ResolveAreasText(s.snap->wb, d.sheet, d.work.source, &sid, &areas)) return;
  const model::Sheet* sh = s.snap->Sheet(sid);
  if (!sh) return;
  ChartObj tmp = d.work;
  if (!BuildSeriesFromAreas(s.snap->wb, *sh, areas, d.work.type, &tmp, !d.work.source_by_rows)) return;
  KeepSeriesFormat(d.work.series, &tmp.series);
  d.work.series = tmp.series;
  d.work.source_by_rows = tmp.source_by_rows;
  d.cur = 0;
}

void OpenEditSeries(AppState& s, SelectData& d, int index, bool is_new) {
  d.edit = true;
  d.edit_new = is_new;
  d.edit_index = index;
  const model::ChartSeries& se = d.work.series[static_cast<size_t>(index)];
  d.edit_backup = se;
  d.e_name = se.name;
  d.e_x = WithEq(se.x_values);
  d.e_y = WithEq(se.values);
  d.focus = true;
  // a new series starts with its values box armed: select them on the sheet; an edit starts with the name, as Excel
  RS().armed = 0;
  Arm(s, ImHashStr(is_new ? (ScatterLike(d.work) ? "chart.x" : "chart.y") : "chart.name"));
}

void CloseSelectData() {
  SelectData& d = SD();
  d.open = d.edit = d.axis = d.collapsed = false;
  RS().armed = 0;
}

// the label and a range box on one row; the box shrinks the dialog to itself when its button is pressed
bool LabeledRef(AppState& s, SelectData& d, const char* label, const char* id, std::string* text, int box, float label_w, float width) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(label_w);
  bool picked = false;
  const bool changed = RefBox(s, id, text, width - label_w, &picked);
  if (picked) {
    d.collapsed = !d.collapsed;
    d.collapsed_box = box;
  }
  return changed;
}

void ApplyEditSeries(AppState& s, SelectData& d) {
  if (d.edit_index < 0 || static_cast<size_t>(d.edit_index) >= d.work.series.size()) return;
  model::ChartSeries& se = d.work.series[static_cast<size_t>(d.edit_index)];
  // a name box with a reference keeps it (=Sheet1!$B$1); other text is the name itself
  std::string n = d.e_name;
  while (!n.empty() && n.front() == ' ') n.erase(0, 1);
  se.name = n;
  if (StripEq(d.e_y).empty() || RangeOk(s, d.sheet, d.e_y) || ArrayConst(d.e_y)) se.values = StripEq(d.e_y);
  if (StripEq(d.e_x).empty() || RangeOk(s, d.sheet, d.e_x)) se.x_values = StripEq(d.e_x);
  se.python_data = false;
  se.data = nullptr;
}

// one row of a range box in the shrunk dialog
void DrawCollapsed(AppState& s, SelectData& d) {
  const char* title = TR("Select Data Source###yc_select_data_mini");
  ImGui::SetNextWindowSize(ImVec2(S(420), 0), ImGuiCond_Always);
  PlaceNearChart(d, ImVec2(S(420), S(70)));
  bool open = true;
  if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
    NoteDialogKeyboard();  // Alt + a letter is the dialog's, not the ribbon's
    const float w = ImGui::GetContentRegionAvail().x;
    bool picked = false;
    switch (d.collapsed_box) {
      case 0:
        if (RefBox(s, "chart.range", &d.range, w, &picked)) ApplyRange(s, d);
        break;
      case 1:
        if (RefBox(s, "chart.name", &d.e_name, w, &picked)) ApplyEditSeries(s, d);
        break;
      case 2:
        if (RefBox(s, "chart.x", &d.e_x, w, &picked)) ApplyEditSeries(s, d);
        break;
      case 3:
        if (RefBox(s, "chart.y", &d.e_y, w, &picked)) ApplyEditSeries(s, d);
        break;
      default:
        if (RefBox(s, "chart.axis", &d.axis_text, w, &picked)) {
          for (auto& se : d.work.series) se.x_values = StripEq(d.axis_text);
        }
        break;
    }
    if (picked || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
        (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)))
      d.collapsed = false;
  }
  ImGui::End();
  if (!open) d.collapsed = false;
}

void Commit(AppState& s, SelectData& d) {
  const uint32_t sheet = d.sheet, id = d.id;
  const ChartObj w = d.work;
  s.engine->PostUser(TR("Select Data"), [sheet, id, w](Document& doc) {
    doc.UpdateChart(sheet, id, [&](ChartObj& c) {
      c.series = w.series;
      c.source = w.source;
      c.source_by_rows = w.source_by_rows;
      c.has_legend = w.has_legend;
      c.has_title = w.has_title;
      c.title = w.title;
      c.hidden_cats = w.hidden_cats;
      c.plot_visible_only = w.plot_visible_only;
      c.display_blanks = w.display_blanks;
      c.na_as_blank = w.na_as_blank;
      c.pivot.clear();
    });
  });
}

void DrawEditSeries(AppState& s, SelectData& d) {
  const bool scatter = ScatterLike(d.work);
  ImGui::SetNextWindowSize(ImVec2(S(420), 0), ImGuiCond_Always);
  PlaceNearChart(d, ImVec2(S(420), S(260)));
  bool open = true;
  bool ok = false, cancel = false;
  if (ImGui::Begin(TR("Edit Series###yc_edit_series"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
    NoteDialogKeyboard();  // Alt + a letter is the dialog's, not the ribbon's
    if (d.focus) {
      ImGui::SetWindowFocus();
      d.focus = false;
    }
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("%s", TR("Click a box, then select the cells on the sheet (or type)."));
    ImGui::Spacing();
    auto row = [&](const char* label, const char* id, std::string* text, int box, bool name) {
      ImGui::TextUnformatted(label);
      bool picked = false;
      if (RefBox(s, id, text, w, &picked)) ApplyEditSeries(s, d);
      if (picked) {
        d.collapsed = true;
        d.collapsed_box = box;
      }
      std::string pv;
      if (name) {
        if (!text->empty() && (*text)[0] == '=') {
          const auto t = RangeTexts(s, d.sheet, *text, 1);
          pv = t.empty() ? std::string() : "= " + t[0];
        }
      } else {
        pv = PreviewList(RangeTexts(s, d.sheet, *text));
      }
      const bool bad = !name && !StripEq(*text).empty() && !RangeOk(s, d.sheet, *text) && !ArrayConst(*text);
      if (bad) ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", TR("That isn't a range of cells: select the cells on the sheet."));
      else ImGui::TextDisabled("%s", pv.empty() ? " " : pv.c_str());
      ImGui::Spacing();
    };
    row(TR("Series name:"), "chart.name", &d.e_name, 1, true);
    if (scatter) row(TR("Series X values:"), "chart.x", &d.e_x, 2, false);
    row(scatter ? TR("Series Y values:") : TR("Series values:"), "chart.y", &d.e_y, 3, false);
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - S(176) - ImGui::GetStyle().ItemSpacing.x);
    ok = ImGui::Button(TR("OK"), ImVec2(S(88), 0));
    ImGui::SameLine();
    cancel = ImGui::Button(TR("Cancel"), ImVec2(S(88), 0));
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) ok = true;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
    }
  }
  ImGui::End();
  if (!open) cancel = true;
  if (ok) {
    ApplyEditSeries(s, d);
    d.edit = false;
    d.cur = d.edit_index;
    RS().armed = 0;
  } else if (cancel) {
    if (d.edit_index >= 0 && static_cast<size_t>(d.edit_index) < d.work.series.size()) {
      if (d.edit_new) d.work.series.erase(d.work.series.begin() + d.edit_index);
      else d.work.series[static_cast<size_t>(d.edit_index)] = d.edit_backup;
    }
    d.edit = false;
    d.cur = std::clamp(d.cur, 0, std::max(0, static_cast<int>(d.work.series.size()) - 1));
    RS().armed = 0;
  }
}

void DrawAxisLabels(AppState& s, SelectData& d) {
  ImGui::SetNextWindowSize(ImVec2(S(400), 0), ImGuiCond_Always);
  PlaceNearChart(d, ImVec2(S(400), S(160)));
  bool open = true;
  bool ok = false, cancel = false;
  if (ImGui::Begin(TR("Axis Labels###yc_axis_labels"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
    NoteDialogKeyboard();  // Alt + a letter is the dialog's, not the ribbon's
    if (d.focus) {
      ImGui::SetWindowFocus();
      d.focus = false;
    }
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(TR("Axis label range:"));
    bool picked = false;
    if (RefBox(s, "chart.axis", &d.axis_text, w, &picked)) {
      if (StripEq(d.axis_text).empty() || RangeOk(s, d.sheet, d.axis_text)) {
        for (auto& se : d.work.series) se.x_values = StripEq(d.axis_text);
      }
    }
    if (picked) {
      d.collapsed = true;
      d.collapsed_box = 4;
    }
    const std::string pv = PreviewList(RangeTexts(s, d.sheet, d.axis_text));
    ImGui::TextDisabled("%s", pv.empty() ? " " : pv.c_str());
    ImGui::Separator();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - S(176) - ImGui::GetStyle().ItemSpacing.x);
    ok = ImGui::Button(TR("OK"), ImVec2(S(88), 0));
    ImGui::SameLine();
    cancel = ImGui::Button(TR("Cancel"), ImVec2(S(88), 0));
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) ok = true;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
    }
  }
  ImGui::End();
  if (!open) cancel = true;
  if (ok || cancel) {
    if (cancel) {
      for (size_t i = 0; i < d.work.series.size() && i < d.axis_backup.size(); ++i) d.work.series[i].x_values = d.axis_backup[i].x_values;
    }
    d.axis = false;
    RS().armed = 0;
  }
}

// ---- outlines on the sheet ----

struct Box {
  Rect r;
  ImU32 col;
};

struct HandleDrag {
  bool on = false;
  uint32_t chart_sheet = 0, chart = 0, data_sheet = 0;
  int series = -1;  // -1: the whole chart (its source range)
  int corner = 0;   // 0 top left, 1 top right, 2 bottom right, 3 bottom left
  Rect v0, cur, src;
  int box = 0;        // the outline dragged: 0 the values, 1 the categories (X values), 2 the series name
  bool move = false;  // its border dragged: the outline moves to other cells (else its corner sizes it)
  uint32_t row0 = 0, col0 = 0;  // the cell the move started on
};

HandleDrag& HD() {
  static HandleDrag h;
  return h;
}

ImRect CellsScreen(const model::Sheet& sh, const render::OverlayGeom& g, const Rect& r) {
  const ImVec2 a = g.to_screen(static_cast<double>(sh.cols->Offset(r.c0)), static_cast<double>(sh.rows->Offset(r.r0)));
  const ImVec2 b = g.to_screen(static_cast<double>(sh.cols->Offset(static_cast<int64_t>(r.c1) + 1)), static_cast<double>(sh.rows->Offset(static_cast<int64_t>(r.r1) + 1)));
  return ImRect(ImFloor(a), ImFloor(b));
}

void Ants(ImDrawList* dl, const ImRect& sr, ImU32 col) {
  const float th = std::max(2.0f, std::floor(S(2.0f)));
  const float dash = S(4), off = std::fmod(static_cast<float>(ImGui::GetTime()) * S(16), dash * 2);
  auto ants = [&](ImVec2 a, ImVec2 b) {
    const bool hz = a.y == b.y;
    const float len = hz ? b.x - a.x : b.y - a.y;
    dl->AddRectFilled(hz ? ImVec2(a.x, a.y - th * 0.5f) : ImVec2(a.x - th * 0.5f, a.y), hz ? ImVec2(b.x, a.y + th * 0.5f) : ImVec2(a.x + th * 0.5f, b.y), IM_COL32_WHITE);
    for (float k = -off; k < len; k += dash * 2) {
      const float d0 = std::max(0.0f, k), d1 = std::min(len, k + dash);
      if (d1 <= d0) continue;
      if (hz) dl->AddRectFilled(ImVec2(a.x + d0, a.y - th * 0.5f), ImVec2(a.x + d1, a.y + th * 0.5f), col);
      else dl->AddRectFilled(ImVec2(a.x - th * 0.5f, a.y + d0), ImVec2(a.x + th * 0.5f, a.y + d1), col);
    }
  };
  const ImVec2 p0(sr.Min.x - 0.5f, sr.Min.y - 0.5f), p1(sr.Max.x - 0.5f, sr.Max.y - 0.5f);
  ants(p0, ImVec2(p1.x, p0.y));
  ants(ImVec2(p0.x, p1.y), p1);
  ants(p0, ImVec2(p0.x, p1.y));
  ants(ImVec2(p1.x, p0.y), p1);
  ui::RequestRedrawIn(0.08);
}

ImVec2 Corner(const ImRect& r, int k) {
  return k == 0 ? r.Min : k == 1 ? ImVec2(r.Max.x, r.Min.y) : k == 2 ? r.Max : ImVec2(r.Min.x, r.Max.y);
}

// a single area of a series box on one sheet
bool OneArea(AppState& s, uint32_t sheet, const std::string& text, uint32_t* sid, Rect* r) {
  std::vector<Rect> areas;
  if (text.empty() || !ResolveAreasText(s.snap->wb, sheet, StripEq(text), sid, &areas) || areas.size() != 1) return false;
  *r = areas[0];
  return true;
}

// the outlines of chart c's data (one series, or all of them when series < 0)
void ChartBoxes(AppState& s, uint32_t chart_sheet, const ChartObj& c, int series, uint32_t sheet_id, std::vector<Box>* out) {
  auto add = [&](const std::string& text, ImU32 col) {
    uint32_t sid = 0;
    std::vector<Rect> areas;
    if (text.empty() || !ResolveAreasText(s.snap->wb, chart_sheet, StripEq(text), &sid, &areas) || sid != sheet_id) return;
    for (const Rect& r : areas) {
      bool dup = false;
      for (const Box& b : *out) dup = dup || (b.r == r && b.col == col);
      if (!dup) out->push_back({r, col});
    }
  };
  for (size_t i = 0; i < c.series.size(); ++i) {
    if (series >= 0 && static_cast<size_t>(series) != i) continue;
    const model::ChartSeries& se = c.series[i];
    if (se.python_data) continue;
    if (!se.name.empty() && se.name[0] == '=') add(se.name, kNameCol);
    add(se.x_values, kCatCol);
    add(se.values, kValCol);
  }
}

// the box of all the series' values and the source range it sits in (whole-chart corner drags)
bool WholeValues(AppState& s, uint32_t chart_sheet, const ChartObj& c, uint32_t* sid, Rect* v, Rect* src) {
  if (c.series.empty() || !c.pivot.empty() || !OneArea(s, chart_sheet, c.source, sid, src)) return false;
  bool first = true;
  for (const auto& se : c.series) {
    uint32_t vs = 0;
    Rect r;
    if (se.python_data || !OneArea(s, chart_sheet, se.values, &vs, &r) || vs != *sid) return false;
    *v = first ? r : Rect{std::min(v->r0, r.r0), std::min(v->c0, r.c0), std::max(v->r1, r.r1), std::max(v->c1, r.c1)};
    first = false;
  }
  return v->r0 >= src->r0 && v->c0 >= src->c0 && v->r1 <= src->r1 && v->c1 <= src->c1;
}

Rect MoveCorner(const Rect& r, int k, uint32_t row, uint32_t col) {
  uint32_t r0 = r.r0, c0 = r.c0, r1 = r.r1, c1 = r.c1;
  if (k == 0 || k == 1) r0 = row;
  if (k == 2 || k == 3) r1 = row;
  if (k == 0 || k == 3) c0 = col;
  if (k == 1 || k == 2) c1 = col;
  return Rect{std::min(r0, r1), std::min(c0, c1), std::max(r0, r1), std::max(c0, c1)};
}

void CommitDrag(AppState& s, const HandleDrag& h) {
  const model::Sheet* data = s.snap->Sheet(h.data_sheet);
  const ChartObj* live = LiveChart(s, h.chart_sheet, h.chart);
  if (!data || !live || h.cur == h.v0) return;
  ChartObj c = *live;
  if (h.series < 0) {
    // an outline of the whole chart moved or sized: the source range moves its sides as much (it keeps its header rows
    // and category columns around the values)
    const Rect& v = h.v0;
    const Rect& n = h.cur;
    const int64_t r0 = static_cast<int64_t>(h.src.r0) + (static_cast<int64_t>(n.r0) - v.r0), c0 = static_cast<int64_t>(h.src.c0) + (static_cast<int64_t>(n.c0) - v.c0);
    const int64_t r1 = static_cast<int64_t>(h.src.r1) + (static_cast<int64_t>(n.r1) - v.r1), c1 = static_cast<int64_t>(h.src.c1) + (static_cast<int64_t>(n.c1) - v.c1);
    if (r0 < 0 || c0 < 0 || r1 < r0 || c1 < c0) return;
    const Rect src{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(r1), static_cast<uint32_t>(c1)};
    ChartObj tmp = c;
    if (!BuildSeriesFromAreas(s.snap->wb, *data, {src}, c.type, &tmp, c.source_by_rows)) return;
    KeepSeriesFormat(c.series, &tmp.series);
    c.series = tmp.series;
    c.source = tmp.source;
  } else {
    if (static_cast<size_t>(h.series) >= c.series.size()) return;
    model::ChartSeries& se = c.series[static_cast<size_t>(h.series)];
    const std::string sref = core::QuoteSheetName(data->name) + "!";
    auto a1 = [&](const Rect& r) { return sref + core::CellA1(r.r0, r.c0, true, true) + (r.IsCell() ? "" : ":" + core::CellA1(r.r1, r.c1, true, true)); };
    if (h.box == 1) {
      se.x_values = a1(h.cur);
    } else if (h.box == 2) {
      se.name = "=" + a1(h.cur);
    } else {
      se.values = a1(h.cur);
    }
    // the X values / categories follow the values' new length
    uint32_t xs = 0;
    Rect x;
    if (h.box == 0 && !h.move && OneArea(s, h.chart_sheet, se.x_values, &xs, &x) && xs == h.data_sheet) {
      if (h.v0.c0 == h.v0.c1 && x.c0 == x.c1) x = Rect{h.cur.r0, x.c0, h.cur.r1, x.c1};
      else if (h.v0.r0 == h.v0.r1 && x.r0 == x.r1) x = Rect{x.r0, h.cur.c0, x.r1, h.cur.c1};
      se.x_values = a1(x);
    }
    c.source.clear();  // one series no longer matches the chart data range
  }
  const uint32_t sheet = h.chart_sheet, id = h.chart;
  const auto series = c.series;
  const std::string source = c.source;
  s.engine->PostUser(TR("Change Chart Data"), [sheet, id, series, source](Document& d) {
    d.UpdateChart(sheet, id, [&](ChartObj& x) {
      x.series = series;
      x.source = source;
    });
  });
}

}  // namespace

// ---- entry points ----

bool RefEdit(AppState& s, const char* id, std::string* text, float width) { return RefBox(s, id, text, width, nullptr, true); }

void OpenSelectData(AppState& s, uint32_t sheet, uint32_t id) {
  const ChartObj* c = LiveChart(s, sheet, id);
  if (!c) return;
  SelectData& d = SD();
  d = SelectData{};
  d.open = true;
  d.sheet = sheet;
  d.id = id;
  d.work = *c;
  d.started_empty = c->series.empty();
  d.range = WithEq(c->source);
  d.focus = true;
  // the chart data range box starts armed: selecting cells on the sheet fills it (like Excel)
  RS().armed = 0;
  if (c->pivot.empty()) Arm(s, ImHashStr("chart.range"));
}

bool SelectDataOpen() { return SD().open; }

const ChartObj* ChartDataPreview(uint32_t sheet, uint32_t id) {
  const SelectData& d = SD();
  return d.open && d.sheet == sheet && d.id == id ? &d.work : nullptr;
}

void DrawSelectDataWindow(AppState& s) {
  SelectData& d = SD();
  if (!d.open) return;
  if (!s.snap || !LiveChart(s, d.sheet, d.id)) {
    CloseSelectData();
    return;
  }
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 1.0f);
  if (d.collapsed) {
    DrawCollapsed(s, d);
    ImGui::PopFont();
    return;
  }
  if (d.edit) {
    DrawEditSeries(s, d);
    ImGui::PopFont();
    return;
  }
  if (d.axis) {
    DrawAxisLabels(s, d);
    ImGui::PopFont();
    return;
  }
  ImGui::SetNextWindowSize(ImVec2(S(500), 0), ImGuiCond_Always);
  PlaceNearChart(d, ImVec2(S(500), S(360)));
  bool open = true, ok = false, cancel = false;
  if (ImGui::Begin(TR("Select Data Source###yc_select_data"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
    NoteDialogKeyboard();  // Alt + a letter is the dialog's, not the ribbon's
    if (d.focus) {
      ImGui::SetWindowFocus();
      d.focus = false;
    }
    ImGui::PushID("##yc_select_data_body");
    const float w = ImGui::GetContentRegionAvail().x;
    const bool scatter = ScatterLike(d.work);
    const bool pivot = !d.work.pivot.empty();
    if (pivot) {
      // a PivotChart shows its PivotTable's report: its fields are changed in the PivotTable (as in Excel)
      ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1), "%s", TR("This is a PivotChart: change its data with the PivotTable's fields."));
      ImGui::BeginDisabled();
    } else {
      ImGui::TextDisabled("%s", TR("Click in a box, then drag over the cells on the sheet: the chart follows."));
    }
    ImGui::Spacing();
    if (LabeledRef(s, d, TR("Chart data range:"), "chart.range", &d.range, 0, S(120), w)) ApplyRange(s, d);
    if (d.range_bad) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(120));
      ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", TR("That isn't a range of cells: select the cells on the sheet."));
    } else if (d.range.empty() && !d.work.series.empty()) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(120));
      ImGui::TextDisabled("%s", TR("The series come from different ranges; a new range replaces them all."));
    }
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - S(180)) * 0.5f);
    ImGui::BeginDisabled(d.work.source.empty());
    if (ImGui::Button(TR("Switch Row/Column"), ImVec2(S(180), 0))) SwitchRowCol(s, d);
    ImGui::EndDisabled();
    ImGui::Spacing();
    const float list_h = S(150);
    if (ImGui::BeginTable("##sd", 2, ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableNextRow();
      // ---- series ----
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(TR("Legend Entries (Series)"));
      const int n = static_cast<int>(d.work.series.size());
      d.cur = std::clamp(d.cur, 0, std::max(0, n - 1));
      if (ImGui::Button(TR("Add"))) {
        model::ChartSeries se;
        d.work.series.push_back(se);
        d.cur = n;
        OpenEditSeries(s, d, n, true);
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(n == 0 || d.work.series[static_cast<size_t>(d.cur)].python_data);
      if (ImGui::Button(TR("Edit##series"))) OpenEditSeries(s, d, d.cur, false);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(n == 0);
      if (ImGui::Button(TR("Remove"))) {
        d.work.series.erase(d.work.series.begin() + d.cur);
        d.work.source.clear();
        d.range.clear();
        d.cur = std::max(0, d.cur - 1);
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::PushFont(ui::F().icons, ImGui::GetFontSize());
      ImGui::BeginDisabled(d.cur <= 0);
      const bool up = ImGui::Button(ui::icon::ChevronUp);
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(d.cur >= n - 1);
      const bool down = ImGui::Button(ui::icon::ChevronDown);
      ImGui::EndDisabled();
      ImGui::PopFont();
      if (up && d.cur > 0) {
        std::swap(d.work.series[static_cast<size_t>(d.cur)], d.work.series[static_cast<size_t>(d.cur - 1)]);
        --d.cur;
      }
      if (down && d.cur < n - 1) {
        std::swap(d.work.series[static_cast<size_t>(d.cur)], d.work.series[static_cast<size_t>(d.cur + 1)]);
        ++d.cur;
      }
      if (ImGui::BeginChild("##series_list", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(d.work.series.size()); ++i) {
          ImGui::PushID(i);
          const std::string label = SeriesLabel(s, d.sheet, d.work.series[static_cast<size_t>(i)], static_cast<size_t>(i));
          bool shown = !d.work.series[static_cast<size_t>(i)].filtered;
          if (ImGui::Checkbox("##shown", &shown)) d.work.series[static_cast<size_t>(i)].filtered = !shown;
          ImGui::SameLine();
          if (ImGui::Selectable(label.c_str(), d.cur == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            d.cur = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !d.work.series[static_cast<size_t>(i)].python_data) OpenEditSeries(s, d, i, false);
          }
          ImGui::PopID();
        }
        if (d.work.series.empty()) ImGui::TextDisabled("%s", TR("(no series: select the chart data range, or Add)"));
      }
      ImGui::EndChild();
      // ---- categories ----
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(scatter ? TR("Horizontal (X) values: set for each series with Edit") : TR("Horizontal (Category) Axis Labels"));
      ImGui::BeginDisabled(scatter || NoCategories(d.work) || d.work.series.empty());
      if (ImGui::Button(TR("Edit##axis"))) {
        d.axis = true;
        d.focus = true;
        d.axis_backup = d.work.series;
        d.axis_text = d.work.series.empty() ? std::string() : WithEq(d.work.series[static_cast<size_t>(d.cur)].x_values);
        RS().armed = 0;
        Arm(s, ImHashStr("chart.axis"));
      }
      ImGui::EndDisabled();
      if (ImGui::BeginChild("##cat_list", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
        if (!d.work.series.empty()) {
          const model::ChartSeries& se = d.work.series[static_cast<size_t>(d.cur)];
          std::vector<std::string> cats = RangeTexts(s, d.sheet, se.x_values, 400);
          if (cats.empty()) {
            const size_t np = RangeTexts(s, d.sheet, se.values, 400).size();
            for (size_t i = 0; i < np; ++i) cats.push_back(std::to_string(i + 1));
          }
          for (size_t k = 0; k < cats.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            auto& hc = d.work.hidden_cats;
            bool shown = !std::binary_search(hc.begin(), hc.end(), static_cast<int>(k));
            if (ImGui::Checkbox(cats[k].empty() ? " " : cats[k].c_str(), &shown)) {
              hc.erase(std::remove(hc.begin(), hc.end(), static_cast<int>(k)), hc.end());
              if (!shown) hc.push_back(static_cast<int>(k));
              std::sort(hc.begin(), hc.end());
            }
            ImGui::PopID();
          }
        }
      }
      ImGui::EndChild();
      ImGui::EndTable();
    }
    if (pivot) ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::Button(TR("Hidden and Empty Cells"))) {
      d.hidden_open = true;
      d.h_visible = d.work.plot_visible_only;
      d.h_na = d.work.na_as_blank;
      d.h_blanks = d.work.display_blanks;
      ImGui::OpenPopup("##hidden_empty");
    }
    if (ImGui::BeginPopup("##hidden_empty")) {
      ImGui::TextUnformatted(TR("Hidden and Empty Cell Settings"));
      ImGui::Separator();
      ImGui::TextUnformatted(TR("Show empty cells as:"));
      if (ui::RadioButton(TR("Gaps"), d.work.display_blanks == 1)) d.work.display_blanks = 1;
      if (ui::RadioButton(TR("Zero"), d.work.display_blanks == 2)) d.work.display_blanks = 2;
      if (ui::RadioButton(TR("Connect data points with line"), d.work.display_blanks == 3)) d.work.display_blanks = 3;
      ImGui::Checkbox(TR("Show #N/A as an empty cell"), &d.work.na_as_blank);
      bool hidden = !d.work.plot_visible_only;
      if (ImGui::Checkbox(TR("Show data in hidden rows and columns"), &hidden)) d.work.plot_visible_only = !hidden;
      ImGui::Separator();
      if (ImGui::Button(TR("OK##hidden"), ImVec2(S(80), 0))) {
        d.hidden_open = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel##hidden"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        d.work.plot_visible_only = d.h_visible;
        d.work.na_as_blank = d.h_na;
        d.work.display_blanks = d.h_blanks;
        d.hidden_open = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    } else {
      d.hidden_open = false;
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - S(176) - ImGui::GetStyle().ItemSpacing.x));
    ok = ImGui::Button(TR("OK"), ImVec2(S(88), 0));
    ImGui::SameLine();
    cancel = ImGui::Button(TR("Cancel"), ImVec2(S(88), 0));
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !d.hidden_open) {
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) ok = true;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape) && !d.hidden_open) cancel = true;
    ImGui::PopID();
  }
  ImGui::End();
  ImGui::PopFont();
  if (!open) cancel = true;
  if (ok) {
    if (!d.range_bad && d.work.pivot.empty()) Commit(s, d);
    CloseSelectData();
  } else if (cancel) {
    CloseSelectData();
  }
}

void NoteChartRect(uint32_t sheet, uint32_t id, const ImRect& r) { ChartRects()[(static_cast<uint64_t>(sheet) << 32) | id] = r; }

bool DrawEmptyChartPrompt(AppState& s, uint32_t sheet, uint32_t id, const ImRect& r) {
  const char* label = TR("Select Data...");
  const ImVec2 ts = ImGui::CalcTextSize(label);
  const ImVec2 bs(ts.x + S(24), ts.y + S(12));
  if (r.GetWidth() < bs.x + S(8) || r.GetHeight() < bs.y * 2.5f) return false;
  const ImVec2 c = r.GetCenter();
  const char* hint = TR("then drag over the data on the sheet");
  const ImVec2 hs = ImGui::CalcTextSize(hint);
  ImGui::SetCursorScreenPos(ImVec2(c.x - bs.x * 0.5f, c.y - bs.y));
  ImGui::PushID(static_cast<int>(id));
  const bool press = ImGui::Button(label, bs);
  ImGui::PopID();
  ImGui::GetWindowDrawList()->AddText(ImVec2(c.x - hs.x * 0.5f, c.y + S(6)), IM_COL32(0x80, 0x80, 0x80, 255), hint);
  if (press) OpenSelectData(s, sheet, id);
  return press;
}

void DrawChartDataHighlights(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh || sh->chart_sheet) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  std::vector<Box> boxes;
  const SelectData& d = SD();
  uint32_t csheet = 0, cid = 0;
  int elem = 0, series = -1;
  const ChartObj* c = nullptr;
  bool handles = false;
  if (d.open) {
    c = &d.work;
    csheet = d.sheet;
    cid = d.id;
    series = d.edit ? d.edit_index : -1;
  } else if (SelectedChartElem(&csheet, &cid, &elem, &series)) {
    c = LiveChart(s, csheet, cid);
    if (elem != static_cast<int>(model::ChartElem::Series)) series = -1;
    handles = c != nullptr;
  }
  if (c) ChartBoxes(s, csheet, *c, series, sheet_id, &boxes);
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  for (const Box& b : boxes) {
    const ImRect sr = CellsScreen(*sh, g, b.r);
    dl->AddRectFilled(sr.Min, sr.Max, (b.col & 0x00FFFFFF) | 0x1C000000);
    dl->AddRect(ImVec2(sr.Min.x - 1, sr.Min.y - 1), sr.Max, b.col, 0.0f, std::max(1.5f, S(1.5f)));
  }
  // the armed range box: moving dashes around what it refers to
  const RefState& rs = RS();
  if (rs.armed && rs.armed_frame >= ImGui::GetFrameCount() - 1 && !rs.armed_text.empty()) {
    uint32_t sid = 0;
    std::vector<Rect> areas;
    if (ResolveAreasText(s.snap->wb, d.open ? d.sheet : sheet_id, StripEq(rs.armed_text), &sid, &areas) && sid == sheet_id) {
      for (const Rect& r : areas) Ants(dl, CellsScreen(*sh, g, r), kArmedCol);
    }
  }
  // the outlines' handles: a corner sizes an outline, its border moves it to other cells (Excel's colored outlines:
  // values blue, categories purple, series names red)
  HandleDrag& h = HD();
  ImGuiIO& io = ImGui::GetIO();
  if (handles && c) {
    struct Outline {
      Rect r;
      ImU32 col;
      int box;
    };
    std::vector<Outline> outs;
    uint32_t dsid = 0;
    Rect v, src;
    bool have = false;
    auto add_area = [&](const std::string& text, ImU32 col, int box) {
      uint32_t sid = 0;
      Rect r;
      if (OneArea(s, csheet, text, &sid, &r) && sid == sheet_id) {
        for (Outline& o : outs) {  // the whole chart: one outline of each kind around all the series'
          if (o.box == box) {
            o.r = Rect{std::min(o.r.r0, r.r0), std::min(o.r.c0, r.c0), std::max(o.r.r1, r.r1), std::max(o.r.c1, r.c1)};
            return;
          }
        }
        outs.push_back({r, col, box});
      }
    };
    if (series < 0) {
      have = WholeValues(s, csheet, *c, &dsid, &v, &src) && dsid == sheet_id;
      if (have) {
        outs.push_back({v, kValCol, 0});
        for (const auto& se : c->series) {
          add_area(se.x_values, kCatCol, 1);
          if (!se.name.empty() && se.name[0] == '=') add_area(se.name, kNameCol, 2);
        }
      }
    } else if (static_cast<size_t>(series) < c->series.size() && !c->series[static_cast<size_t>(series)].python_data) {
      const model::ChartSeries& se = c->series[static_cast<size_t>(series)];
      have = OneArea(s, csheet, se.values, &dsid, &v) && dsid == sheet_id;
      if (have) {
        outs.push_back({v, kValCol, 0});
        add_area(se.x_values, kCatCol, 1);
        if (!se.name.empty() && se.name[0] == '=') add_area(se.name, kNameCol, 2);
      }
    }
    auto start = [&](int box, const Rect& r0, bool move, int corner) {
      h = HandleDrag{};
      h.on = true;
      h.chart_sheet = csheet;
      h.chart = cid;
      h.data_sheet = sheet_id;
      h.series = series;
      h.box = box;
      h.move = move;
      h.corner = corner;
      h.v0 = h.cur = r0;
      h.src = src;
      const ImVec2 sp = g.to_sheet(io.MousePos);
      h.row0 = static_cast<uint32_t>(std::max<int64_t>(0, sh->rows->IndexAt(static_cast<int64_t>(std::max(0.0f, sp.y)))));
      h.col0 = static_cast<uint32_t>(std::max<int64_t>(0, sh->cols->IndexAt(static_cast<int64_t>(std::max(0.0f, sp.x)))));
    };
    for (size_t oi = 0; oi < outs.size() && have; ++oi) {
      const Outline& o = outs[oi];
      const bool mine = h.on && h.chart == cid && h.chart_sheet == csheet && h.series == series && h.box == o.box;
      const Rect shown = mine ? h.cur : o.r;
      const ImRect sr = CellsScreen(*sh, g, shown);
      if (mine) Ants(dl, sr, o.col);
      ImGui::PushID(1700 + static_cast<int>(oi) * 16);
      // the border: dragged, the outline moves (inside the corners, a few pixels wide)
      const float bw = std::max(S(3.0f), 3.0f), hs = std::max(S(3.5f), 3.0f);
      const ImRect edges[4] = {ImRect(ImVec2(sr.Min.x + hs * 2, sr.Min.y - bw), ImVec2(sr.Max.x - hs * 2, sr.Min.y + bw)),
                               ImRect(ImVec2(sr.Max.x - bw, sr.Min.y + hs * 2), ImVec2(sr.Max.x + bw, sr.Max.y - hs * 2)),
                               ImRect(ImVec2(sr.Min.x + hs * 2, sr.Max.y - bw), ImVec2(sr.Max.x - hs * 2, sr.Max.y + bw)),
                               ImRect(ImVec2(sr.Min.x - bw, sr.Min.y + hs * 2), ImVec2(sr.Min.x + bw, sr.Max.y - hs * 2))};
      for (int e = 0; e < 4; ++e) {
        if (edges[e].GetWidth() < 2 || edges[e].GetHeight() < 2) continue;
        ImGui::SetCursorScreenPos(edges[e].Min);
        ImGui::PushID(8 + e);
        ImGui::InvisibleButton("##data_edge", edges[e].GetSize());
        if (ImGui::IsItemHovered() || (mine && h.move)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Drag to move the chart's data to other cells"));
        if (ImGui::IsItemActivated()) start(o.box, o.r, true, 0);
        ImGui::PopID();
      }
      // the corners: dragged, the outline takes more or fewer cells
      for (int k = 0; k < 4; ++k) {
        const ImVec2 p = Corner(sr, k);
        dl->AddRectFilled(p - ImVec2(hs, hs), p + ImVec2(hs, hs), o.col);
        dl->AddRect(p - ImVec2(hs, hs), p + ImVec2(hs, hs), IM_COL32_WHITE);
        ImGui::SetCursorScreenPos(p - ImVec2(hs + S(2), hs + S(2)));
        ImGui::PushID(k);
        ImGui::InvisibleButton("##data_corner", ImVec2((hs + S(2)) * 2, (hs + S(2)) * 2));
        const bool hov = ImGui::IsItemHovered();
        if (hov || (mine && !h.move && h.corner == k)) ImGui::SetMouseCursor((k == 0 || k == 2) ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
        if (hov) ImGui::SetTooltip("%s", TR("Drag to change the chart's data"));
        if (ImGui::IsItemActivated()) start(o.box, o.r, false, k);
        ImGui::PopID();
      }
      ImGui::PopID();
      if (mine) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          const ImVec2 sp = g.to_sheet(io.MousePos);
          const uint32_t row = static_cast<uint32_t>(std::max<int64_t>(0, sh->rows->IndexAt(static_cast<int64_t>(std::max(0.0f, sp.y)))));
          const uint32_t col = static_cast<uint32_t>(std::max<int64_t>(0, sh->cols->IndexAt(static_cast<int64_t>(std::max(0.0f, sp.x)))));
          if (h.move) {
            const int64_t dr = static_cast<int64_t>(row) - h.row0, dc = static_cast<int64_t>(col) - h.col0;
            const int64_t r0 = std::max<int64_t>(0, static_cast<int64_t>(h.v0.r0) + dr), c0 = std::max<int64_t>(0, static_cast<int64_t>(h.v0.c0) + dc);
            h.cur = Rect{static_cast<uint32_t>(r0), static_cast<uint32_t>(c0), static_cast<uint32_t>(r0 + (h.v0.r1 - h.v0.r0)), static_cast<uint32_t>(c0 + (h.v0.c1 - h.v0.c0))};
          } else {
            Rect n = MoveCorner(h.v0, h.corner, row, col);
            if (series >= 0 || h.box != 0) {  // one series (its categories, its name) stays one column (or one row)
              if (h.v0.c0 == h.v0.c1 && !(h.box == 2 && h.v0.r0 == h.v0.r1)) n.c0 = n.c1 = h.v0.c0;
              else if (h.v0.r0 == h.v0.r1) n.r0 = n.r1 = h.v0.r0;
            }
            h.cur = n;
          }
        } else {
          CommitDrag(s, h);
          h.on = false;
        }
      }
    }
  }
  dl->PopClipRect();
}

// ---- =SERIES(name, X values, values, order): a series as one formula (the formula bar, Series.Formula) ----

std::string SeriesFormula(const ChartObj& c, size_t i) {
  if (i >= c.series.size()) return {};
  const model::ChartSeries& se = c.series[i];
  auto ref = [](const std::string& r) { return r.find(',') != std::string::npos ? "(" + r + ")" : r; };
  std::string name;
  if (!se.name.empty() && se.name[0] == '=') {
    name = se.name.substr(1);
  } else if (!se.name.empty()) {
    name = "\"";
    for (char ch : se.name) name += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    name += "\"";
  }
  return "=SERIES(" + name + "," + ref(se.x_values) + "," + ref(se.values) + "," + std::to_string(i + 1) + ")";
}

bool ParseSeriesFormula(std::string_view text, std::string* name, std::string* x, std::string* values, int* order) {
  auto trim = [](std::string_view v) {
    while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
    while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
    return v;
  };
  text = trim(text);
  if (!text.empty() && text.front() == '=') text.remove_prefix(1);
  text = trim(text);
  if (text.size() < 8) return false;
  std::string head(text.substr(0, 7));
  for (char& ch : head) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (head != "SERIES(" || text.back() != ')') return false;
  text = text.substr(7, text.size() - 8);
  // the arguments: commas outside quotes, quoted sheet names and parentheses
  std::vector<std::string_view> args;
  bool dq = false, sq = false;
  int depth = 0;
  size_t from = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char ch = i < text.size() ? text[i] : ',';
    if (i < text.size()) {
      if (ch == '"' && !sq) dq = !dq;
      else if (ch == 39 && !dq) sq = !sq;  // a quote around a sheet name
      else if (!dq && !sq && ch == '(') ++depth;
      else if (!dq && !sq && ch == ')') --depth;
    }
    if (ch == ',' && !dq && !sq && depth == 0) {
      args.push_back(trim(text.substr(from, i - from)));
      from = i + 1;
    }
  }
  if (args.size() < 3 || args.size() > 5) return false;
  auto unparen = [&](std::string_view v) {
    v = trim(v);
    if (v.size() >= 2 && v.front() == '(' && v.back() == ')') v = v.substr(1, v.size() - 2);
    return std::string(v);
  };
  const std::string_view n = args[0];
  if (n.size() >= 2 && n.front() == '"' && n.back() == '"') {
    name->clear();
    for (size_t i = 1; i + 1 < n.size(); ++i) {
      name->push_back(n[i]);
      if (n[i] == '"' && i + 2 < n.size() && n[i + 1] == '"') ++i;
    }
  } else {
    *name = n.empty() ? std::string() : "=" + std::string(n);
  }
  *x = unparen(args[1]);
  *values = unparen(args[2]);
  *order = 0;
  if (args.size() >= 4) {
    for (char ch : args[3]) {
      if (ch < '0' || ch > '9') return false;
      *order = *order * 10 + (ch - '0');
    }
  }
  return !values->empty();
}

namespace {

struct SeriesBar {
  bool editing = false;
  bool focus = false;
  char buf[2048] = {};
  uint32_t sheet = 0, id = 0;
  int series = 0;
};

SeriesBar& SB() {
  static SeriesBar b;
  return b;
}

// the selected chart, when one of its series (not one from Python data) is selected
const ChartObj* SelectedSeries(AppState& s, uint32_t* sheet, uint32_t* id, int* series) {
  int elem = 0;
  if (!SelectedChartElem(sheet, id, &elem, series) || elem != static_cast<int>(model::ChartElem::Series)) return nullptr;
  const ChartObj* c = LiveChart(s, *sheet, *id);
  return c && *series >= 0 && static_cast<size_t>(*series) < c->series.size() && !c->series[static_cast<size_t>(*series)].python_data ? c : nullptr;
}

}  // namespace

bool ApplySeriesFormula(AppState& s, uint32_t sheet, uint32_t id, int series, const std::string& text) {
  std::string name, x, values;
  int order = 0;
  if (!ParseSeriesFormula(text, &name, &x, &values, &order)) return false;
  uint32_t sid = 0;
  std::vector<Rect> areas;
  if (!ResolveAreasText(s.snap->wb, sheet, values, &sid, &areas)) return false;
  if (!x.empty() && !ResolveAreasText(s.snap->wb, sheet, x, &sid, &areas)) return false;
  s.engine->PostUser(TR("Edit Series"), [sheet, id, series, name, x, values, order](Document& d) {
    d.UpdateChart(sheet, id, [&](ChartObj& c) {
      if (series < 0 || static_cast<size_t>(series) >= c.series.size()) return;
      model::ChartSeries& se = c.series[static_cast<size_t>(series)];
      se.name = name;
      se.x_values = x;
      se.values = values;
      se.python_data = false;
      se.data = nullptr;
      c.source.clear();
      const int to = order >= 1 ? std::min(order, static_cast<int>(c.series.size())) - 1 : series;
      if (to != series) {
        model::ChartSeries moved = std::move(se);
        c.series.erase(c.series.begin() + series);
        c.series.insert(c.series.begin() + to, std::move(moved));
      }
    });
  });
  return true;
}

bool SeriesNameBoxText(AppState& s, std::string* out) {
  uint32_t sheet = 0, id = 0;
  int series = 0;
  const ChartObj* c = SelectedSeries(s, &sheet, &id, &series);
  if (!c) return false;
  const std::string n = SeriesLabel(s, sheet, c->series[static_cast<size_t>(series)], static_cast<size_t>(series));
  *out = std::string(TR("Series")) + " \"" + n + "\"";
  return true;
}

// ---- a selected chart title or axis title in the formula bar: its text, or =Sheet1!$A$1 linking it to a cell ----
struct TitleBar {
  bool editing = false, focus = false;
  uint32_t sheet = 0, id = 0;
  int elem = 0;
  int series = 0, point = -1;  // one data label: its series and point
  char buf[1024] = {};
  std::vector<Rect> seen;  // the cells selected when typing began: a click on another cell after "=" points at it
  uint32_t seen_sheet = 0;
};
TitleBar& TB() {
  static TitleBar t;
  return t;
}

bool IsTitleElem(int e) {
  using E = model::ChartElem;
  return e == static_cast<int>(E::Title) || e == static_cast<int>(E::CatAxisTitle) || e == static_cast<int>(E::ValAxisTitle) ||
         e == static_cast<int>(E::ValAxis2Title) || e == static_cast<int>(E::DataLabel);
}

// the text the formula bar shows and changes: a title's, or one data label's own text (its point record is added)
std::string& TitleOf(ChartObj& c, int e, int series = 0, int point = -1) {
  using E = model::ChartElem;
  if (e == static_cast<int>(E::DataLabel)) {
    static std::string none;
    none.clear();
    if (series < 0 || static_cast<size_t>(series) >= c.series.size() || point < 0) return none;
    auto& pts = c.series[static_cast<size_t>(series)].points;
    for (auto& q : pts) {
      if (q.index == point) return q.label_text;
    }
    model::ChartPointFormat q;
    q.index = point;
    pts.push_back(q);
    return pts.back().label_text;
  }
  if (e == static_cast<int>(E::CatAxisTitle)) return c.cat_axis.title;
  if (e == static_cast<int>(E::ValAxisTitle)) return c.val_axis.title;
  if (e == static_cast<int>(E::ValAxis2Title)) return c.val_axis2.title;
  return c.title;
}

// Enter: the typed text, or a reference to one cell (stored as =Sheet1!$A$1, followed as the cell changes)
bool CommitTitleBar(AppState& s, TitleBar& t) {
  std::string text = t.buf;
  if (!text.empty() && text[0] == '=') {
    uint32_t rs = 0;
    Rect r;
    if (!ResolveRangeText(s.snap->wb, t.sheet, text.substr(1), &rs, &r)) {
      s.ShowError("YCGrid", TR("A chart title can show the text of one cell: type = and click the cell."));
      return false;
    }
    text = "=" + AreasAddress(s.snap->wb, rs, {Rect::Cell(r.r0, r.c0)});
  }
  const int e = t.elem, ls = t.series, lp = t.point;
  const uint32_t sheet = t.sheet, id = t.id;
  s.engine->PostUser(TR("Chart Title"), [sheet, id, e, ls, lp, text](Document& d) {
    d.UpdateChart(sheet, id, [&](ChartObj& c) {
      TitleOf(c, e, ls, lp) = text;
      using E = model::ChartElem;
      if (e == static_cast<int>(E::DataLabel)) {  // the label shows its own text (or its cell's)
        for (auto& q : c.series[static_cast<size_t>(ls)].points) {
          if (q.index == lp) q.label = 1;
        }
      } else if (e == static_cast<int>(E::Title)) c.has_title = true;
      else if (e == static_cast<int>(E::CatAxisTitle)) c.cat_axis.has_title = true;
      else if (e == static_cast<int>(E::ValAxisTitle)) c.val_axis.has_title = true;
      else c.val_axis2.has_title = true;
    });
  });
  if (s.focus_sheet != sheet) ActivateSheet(s, sheet);
  SelectChartElement(s, sheet, id, e, ls, false, lp);  // the title (label) stays selected, as in Excel
  return true;
}

bool DrawTitleFormulaBar(AppState& s, const ImRect& fb) {
  TitleBar& t = TB();
  uint32_t sheet = 0, id = 0;
  int elem = 0, series = 0;
  const bool sel = SelectedChartElem(&sheet, &id, &elem, &series) && IsTitleElem(elem);
  if (t.editing && !LiveChart(s, t.sheet, t.id)) t.editing = false;
  if (!t.editing) {
    if (!sel) return false;
    const ChartObj* c = LiveChart(s, sheet, id);
    if (!c) return false;
    const int point = SelectedChartPoint();
    std::string f;
    if (elem == static_cast<int>(model::ChartElem::DataLabel)) {
      if (series >= 0 && static_cast<size_t>(series) < c->series.size()) {
        if (const model::ChartPointFormat* q = model::PointFormatOf(c->series[static_cast<size_t>(series)], point)) f = q->label_text;
      }
    } else {
      f = TitleOf(const_cast<ChartObj&>(*c), elem);  // (read only)
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(fb.Min, fb.Max, true);
    dl->AddText(ImVec2(fb.Min.x + S(6), fb.GetCenter().y - ImGui::GetFontSize() * 0.5f), IM_COL32(0x20, 0x20, 0x20, 255), f.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(fb.Min);
    bool start = ImGui::InvisibleButton("##title_formula", fb.GetSize());
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    std::string typed;
    // typing with the title selected starts in the formula bar (Excel: select the title, type =, click a cell)
    const ImGuiIO& io = ImGui::GetIO();
    if (!start && !io.KeyCtrl && !io.KeyAlt && !ImGui::IsAnyItemActive() && !io.InputQueueCharacters.empty()) {
      for (ImWchar ch : io.InputQueueCharacters) {
        if (ch >= 32 && ch < 0x80) typed += static_cast<char>(ch);
      }
      start = !typed.empty();  // Excel: what is typed replaces the title (= then a click links it to a cell)
    }
    if (start) {
      t.editing = t.focus = true;
      t.sheet = sheet;
      t.id = id;
      t.elem = elem;
      t.series = series;
      t.point = point;
      std::snprintf(t.buf, sizeof t.buf, "%s", typed.empty() ? f.c_str() : typed.c_str());
      t.seen = SelAreas(s);
      t.seen_sheet = s.focus_sheet;
      if (!typed.empty()) ImGui::GetIO().InputQueueCharacters.resize(0);
    }
    return true;
  }
  // pointing: after "=", a click on a cell (or a drag over cells) puts its reference in, as in a cell's formula
  const bool pointing = t.buf[0] == '=';
  {
    std::vector<Rect> cur = SelAreas(s);
    if (pointing && (cur != t.seen || s.focus_sheet != t.seen_sheet) && !cur.empty()) {
      t.seen = cur;
      t.seen_sheet = s.focus_sheet;
      std::snprintf(t.buf, sizeof t.buf, "=%s", AreasAddress(s.snap->wb, s.focus_sheet, {Rect::Cell(cur[0].r0, cur[0].c0)}).c_str());
      t.focus = true;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(fb.Min.x + 1, fb.Min.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(5), (fb.GetHeight() - 2 - ImGui::GetFontSize()) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
  const bool opening = t.focus;
  if (t.focus) {
    ImGui::SetKeyboardFocusHere();
    t.focus = false;
  }
  ImGui::SetNextItemWidth(fb.GetWidth() - 2);
  const bool enter = ImGui::InputText("##title_formula_edit", t.buf, sizeof t.buf, ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemActivated()) {  // the caret after what was typed (not all of it selected)
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID())) {
      const int n = static_cast<int>(std::strlen(t.buf));
      st->SetSelection(n, n);
    }
  }
  const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
  const bool left = !opening && ImGui::IsItemDeactivated() && !enter;
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (enter || (ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsItemActive())) {
    if (CommitTitleBar(s, t)) t.editing = false;
  } else if (esc) {
    t.editing = false;
    SelectChartElement(s, t.sheet, t.id, t.elem, t.series, false, t.point);
  } else if (left) {
    // a click on the sheet while pointing keeps editing (the reference goes in); anywhere else ends it, keeping the text
    const bool on_sheet = pointing && !fb.Contains(ImGui::GetIO().MousePos);
    if (on_sheet) {
      t.focus = true;
    } else if (CommitTitleBar(s, t)) {
      t.editing = false;
    }
  }
  return true;
}

bool DrawSeriesFormulaBar(AppState& s, const ImRect& fb) {
  SeriesBar& b = SB();
  uint32_t sheet = 0, id = 0;
  int series = 0;
  const ChartObj* c = SelectedSeries(s, &sheet, &id, &series);
  if (!c || TB().editing) {
    b.editing = false;
    return DrawTitleFormulaBar(s, fb);  // a selected chart title or axis title
  }
  if (b.editing && (b.sheet != sheet || b.id != id || b.series != series)) b.editing = false;
  if (!b.editing) {
    const std::string f = SeriesFormula(*c, static_cast<size_t>(series));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(fb.Min, fb.Max, true);
    dl->AddText(ImVec2(fb.Min.x + S(6), fb.GetCenter().y - ImGui::GetFontSize() * 0.5f), IM_COL32(0x20, 0x20, 0x20, 255), f.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(fb.Min);
    if (ImGui::InvisibleButton("##series_formula", fb.GetSize())) {
      b.editing = b.focus = true;
      b.sheet = sheet;
      b.id = id;
      b.series = series;
      std::snprintf(b.buf, sizeof b.buf, "%s", f.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    return true;
  }
  ImGui::SetCursorScreenPos(ImVec2(fb.Min.x + 1, fb.Min.y + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(5), (fb.GetHeight() - 2 - ImGui::GetFontSize()) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
  const bool opening = b.focus;
  if (b.focus) {
    ImGui::SetKeyboardFocusHere();
    b.focus = false;
  }
  ImGui::SetNextItemWidth(fb.GetWidth() - 2);
  const bool enter = ImGui::InputText("##series_formula_edit", b.buf, sizeof b.buf, ImGuiInputTextFlags_EnterReturnsTrue);
  const bool esc = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);
  const bool left = !opening && ImGui::IsItemDeactivated() && !enter;
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (enter) {
    if (ApplySeriesFormula(s, sheet, id, series, b.buf)) b.editing = false;
    else s.ShowError("YCGrid", TR("That isn't a SERIES formula: =SERIES(name, X values, values, order) with ranges of cells."));
  } else if (esc || left) {
    b.editing = false;
  }
  return true;
}

}  // namespace yc::app
