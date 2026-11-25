// SPDX-License-Identifier: Apache-2.0
// layout_io.cpp - saving and loading the layout (PLAN §10.5, §15)
//
// Currently written as a JSON file; once .ycg is complete the same JSON goes into the LAYOUT chunk (engine Workbook::layout_json).
// Compatibility rule: fields are only added, never changed; readers ignore unknown fields and use defaults for missing ones.
#include "app_state.h"
#include "core/i18n.h"

#include "dock/dock_layout.h"
#include "frame/frame_context.h"
#include "ribbon/ribbon.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace yc::app {

using nlohmann::json;

namespace {

std::vector<std::string> KnownWindows(const AppState& s) {
  std::vector<std::string> names = {kGridHostWindow, kChartWindow};
  if (s.snap) {
    for (const auto& sh : s.snap->wb.sheets) names.push_back(s.SheetWindowName(sh->id));
  }
  for (const Container& c : s.containers) names.push_back(s.ContainerWindowName(c));
  for (auto& f : FigureWindowNames()) names.push_back(std::move(f));  // keyed figures: placed again by key
  return names;
}

json ViewToJson(const render::GridView& v) {
  json a = json::array();
  for (const Rect& r : v.sel.areas) a.push_back({r.r0, r.c0, r.r1, r.c1});
  return {{"top_row", v.v.first}, {"left_col", v.h.first}, {"act_r", v.sel.act_r}, {"act_c", v.sel.act_c}, {"anc_r", v.sel.anc_r},
          {"anc_c", v.sel.anc_c}, {"mode", static_cast<int>(v.sel.mode)}, {"areas", a}, {"zoom", v.zoom}, {"page_view", v.page_view}, {"freeze_rows", v.freeze_rows},
          {"freeze_cols", v.freeze_cols}, {"frozen_top", v.frozen_top}, {"frozen_left", v.frozen_left}, {"split", v.split},
          {"mirror_left", v.mirror_left}};
}

void JsonToView(const json& jv, render::GridView& v, uint32_t row_limit, uint32_t col_limit) {
  auto clampr = [&](int64_t x) { return static_cast<uint32_t>(std::clamp<int64_t>(x, 0, int64_t{row_limit} - 1)); };
  auto clampc = [&](int64_t x) { return static_cast<uint32_t>(std::clamp<int64_t>(x, 0, int64_t{col_limit} - 1)); };
  v.v.first = clampr(jv.value("top_row", int64_t{0}));
  v.h.first = clampc(jv.value("left_col", int64_t{0}));
  v.sel.act_r = clampr(jv.value("act_r", int64_t{0}));
  v.sel.act_c = clampc(jv.value("act_c", int64_t{0}));
  v.sel.anc_r = clampr(jv.value("anc_r", int64_t{v.sel.act_r}));
  v.sel.anc_c = clampc(jv.value("anc_c", int64_t{v.sel.act_c}));
  v.sel.mode = static_cast<render::SelectMode>(std::clamp(jv.value("mode", 0), 0, 3));
  v.sel.areas.clear();
  if (jv.contains("areas")) {
    for (const auto& a : jv["areas"]) {
      if (!a.is_array() || a.size() != 4) continue;
      v.sel.areas.push_back(Rect{clampr(a[0].get<int64_t>()), clampc(a[1].get<int64_t>()), clampr(a[2].get<int64_t>()), clampc(a[3].get<int64_t>())});
    }
  }
  if (v.sel.areas.empty()) v.sel.areas.push_back(Rect::Cell(v.sel.act_r, v.sel.act_c));
  v.zoom = std::clamp(jv.value("zoom", 1.0f), 0.1f, 4.0f);
  v.page_view = std::clamp(jv.value("page_view", 0), 0, 2);
  v.freeze_rows = jv.value("freeze_rows", 0u);
  v.freeze_cols = jv.value("freeze_cols", 0u);
  v.split = jv.value("split", false);
  v.frozen_top = jv.value("frozen_top", 0u);
  v.frozen_left = jv.value("frozen_left", 0u);
  v.mirror_left = jv.value("mirror_left", false);  // since 2026-10 (T layout); older layouts don't have it
}

}  // namespace

std::string CaptureLayoutJson(AppState& s) {
  json j;
  j["schema"] = "ycgrid.layout";
  j["version"] = ui::dock::kLayoutSchemaVersion;
  j["main"] = ui::dock::CaptureMainWindow(s.hwnd);
  j["app"] = {{"active_sheet", s.active_sheet},       {"focus_sheet", s.focus_sheet},     {"tab_split", s.tab_split},
              {"show_gridlines", s.show_gridlines},   {"show_headings", s.show_headings}, {"show_formula_bar", s.show_formula_bar},
              {"show_chart", s.show_chart},           {"chart_type", s.chart_type},       {"ribbon_tab", ui::ribbon::St().selected_tab},
              {"ribbon_collapsed", ui::ribbon::St().collapsed}, {"sheet_tabs", s.show_sheet_tabs},
              {"hscroll", s.show_hscroll},             {"vscroll", s.show_vscroll}};
  json sheets = json::array();
  if (s.snap) {
    for (const auto& sh : s.snap->wb.sheets) {
      const SheetWin& w = s.Win(sh->id);
      sheets.push_back({{"id", sh->id}, {"name", sh->name}, {"floating", w.floating}, {"gridlines", w.gridlines}, {"headings", w.headings},
                        {"zeros", w.zeros}, {"gridline_color", w.gridline_color}, {"outline", w.outline}, {"formulas", w.formulas},
                        {"view", ViewToJson(w.view)}});
    }
  }
  j["sheets"] = sheets;
  json containers = json::array();
  std::vector<int> ids;
  for (const Container& c : s.containers) {
    containers.push_back({{"id", c.id}, {"name", c.name}});
    ids.push_back(c.id);
  }
  j["containers"] = containers;
  j["next_container_id"] = s.next_container_id;
  j["locked"] = std::vector<std::string>(s.locked_windows.begin(), s.locked_windows.end());  // since 2026-10
  j["panes"] = PaneStates();  // since 0.1.4: Immediate, Error Log, Watch Window... shown or not
  j["dock"] = ui::dock::CaptureDockTrees(KnownWindows(s), ids);
  return j.dump();
}

bool SaveLayout(AppState& s) {
  std::ofstream f(s.LayoutPath(), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << CaptureLayoutJson(s);
  return static_cast<bool>(f);
}

bool LoadLayout(AppState& s, bool at_startup) {
  std::ifstream f(s.LayoutPath(), std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  return ApplyLayoutJson(s, ss.str(), at_startup);
}

bool ApplyLayoutJson(AppState& s, const std::string& text, bool restore_main_window) {
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || j.value("schema", "") != "ycgrid.layout") return false;
  // (never in a hidden instance: SetWindowPlacement shows the window, on the user's screen, and a file dialog then
  // waits there for nobody)
  if (j.contains("main") && restore_main_window && !s.headless) ui::dock::RestoreMainWindow(s.hwnd, j["main"]);
  if (j.contains("sheets") && s.snap) {
    for (const auto& js : j["sheets"]) {
      const uint32_t id = js.value("id", 0u);
      const model::Sheet* sh = s.snap->Sheet(id);
      if (!sh) continue;  // a sheet in the layout no longer exists: skip it
      SheetWin& w = s.Win(id);
      w.floating = js.value("floating", false);
      // per sheet since 2026-10; older layouts had one setting for all sheets (in "app")
      const bool all_g = j.contains("app") ? j["app"].value("show_gridlines", true) : true;
      const bool all_h = j.contains("app") ? j["app"].value("show_headings", true) : true;
      w.gridlines = js.value("gridlines", all_g);
      w.headings = js.value("headings", all_h);
      w.zeros = js.value("zeros", true);
      w.gridline_color = js.value("gridline_color", 0u);
      // the outline symbols (Ctrl+8) and Show Formulas (Ctrl+`) are kept with the workbook, as Excel keeps them in the
      // sheet's view: hidden outline symbols came back after a save
      w.outline = js.value("outline", true);
      w.formulas = js.value("formulas", false);
      if (js.contains("view")) JsonToView(js["view"], w.view, sh->row_limit, sh->col_limit);
    }
  }
  if (j.contains("app")) {
    const auto& a = j["app"];
    const uint32_t act = a.value("active_sheet", s.active_sheet);
    if (s.snap && s.snap->Sheet(act) && !s.Win(act).floating) s.active_sheet = act;
    const uint32_t foc = a.value("focus_sheet", s.active_sheet);
    if (s.snap && s.snap->Sheet(foc)) s.focus_sheet = foc;
    s.tab_split = std::clamp(a.value("tab_split", s.tab_split), 0.15f, 0.85f);
    s.view_synced_sheet = UINT32_MAX;  // the ribbon's check boxes take the focused sheet's settings again
    s.show_formula_bar = a.value("show_formula_bar", true);
    s.show_chart = a.value("show_chart", false);
    s.chart_type = std::clamp(a.value("chart_type", 1), 0, 3);
    ui::ribbon::St().selected_tab = std::clamp(a.value("ribbon_tab", 1), 1, static_cast<int>(ui::ribbon::Tabs().size()) - 1);
    ui::ribbon::St().collapsed = a.value("ribbon_collapsed", false);
    s.show_sheet_tabs = a.value("sheet_tabs", true);
    s.show_hscroll = a.value("hscroll", true);
    s.show_vscroll = a.value("vscroll", true);
  }
  if (j.contains("panes") && j["panes"].is_object()) {  // older layouts don't have it: the panes stay as they are
    for (const auto& [name, shown] : j["panes"].items()) {
      if (shown.is_boolean()) SetPaneState(name, shown.get<bool>());
    }
  }
  s.containers.clear();
  if (j.contains("containers")) {
    for (const auto& jc : j["containers"]) {
      Container c;
      c.id = jc.value("id", 0);
      c.name = jc.value("name", TR("Container ") + std::to_string(c.id));
      s.containers.push_back(c);
    }
  }
  s.next_container_id = std::max(j.value("next_container_id", 1), static_cast<int>(s.containers.size()) + 1);
  s.locked_windows.clear();  // older layouts don't have it: nothing locked
  if (j.contains("locked") && j["locked"].is_array()) {
    for (const auto& k : j["locked"]) {
      if (k.is_string()) s.locked_windows.insert(k.get<std::string>());
    }
  }
  if (j.contains("dock")) {
    ui::dock::QueueRestore(j["dock"]);
    ui::RequestRedrawIn(0.0);  // applied on the next frame, not with the next click (a hidden instance draws on demand)
  }
  s.layout_restored = true;
  return true;
}

}  // namespace yc::app
