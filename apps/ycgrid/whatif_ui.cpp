// SPDX-License-Identifier: Apache-2.0
// whatif_ui.cpp - Data tab: What-If Analysis (Goal Seek, Scenario Manager, Data Table), Solver, Consolidate, Advanced
// Filter and Flash Fill (Ctrl+E)
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/snapshot.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "yc_ui_items.h"

#include "formula/numfmt.h"

#include "imgui.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

struct WhatIfUi {
  std::string sc_editing;  // Scenario Manager > Edit: the scenario's name before the edit ("" = Add)
  // Goal Seek
  char gs_set[64] = "", gs_to[64] = "", gs_by[64] = "";
  bool gs_done = false;
  bool gs_changed = false;  // the search changed the cell (an undo step to take back on Cancel)
  uint32_t gs_sheet = 0;
  core::CellPos gs_cell{};  // the Set cell (its number format shows the values)
  Document::GoalSeekResult gs_result;
  // Data Table
  char dt_row[64] = "", dt_col[64] = "";
  Rect dt_rect{};
  // Scenario Manager
  int sc_sel = -1;
  int sc_mode = 0;  // 0 list, 1 edit (add / change), 2 values, 3 summary, 4 merge
  int sc_report = 0;     // Summary: 0 Scenario summary, 1 Scenario PivotTable report
  int sc_merge_sel = -1;  // Merge: the source sheet (index into the workbook's sheets)
  char sc_name[128] = "", sc_cells[256] = "", sc_comment[256] = "", sc_results[256] = "";
  bool sc_locked = true, sc_hidden = false;  // Protection: Prevent changes, Hide
  std::vector<std::string> sc_values;
  bool sc_values_ok = false;
  std::vector<core::CellPos> sc_cell_list;
  // Consolidate
  int co_func = 0;
  char co_ref[256] = "";
  std::vector<std::string> co_refs;
  int co_sel = -1;
  bool co_top = false, co_left = false, co_links = false;
  // Advanced Filter
  int af_action = 0;  // 0 in place, 1 copy
  char af_list[256] = "", af_crit[256] = "", af_dest[128] = "";
  bool af_unique = false;
  // Solver
  model::SolverModel sv;
  char sv_obj[128] = "", sv_value[64] = "0", sv_changing[256] = "";
  int sv_sel = -1;
  char sv_cref[128] = "", sv_crhs[128] = "";
  int sv_crel = 0;
  bool sv_done = false, sv_keep = true;
  bool sv_changed = false;  // the solve changed the variable cells (Restore takes back that step only)
  int sv_page = 0;    // 0 Solver Parameters, 1 Solver Results, 2 Options, 3 Add / Change Constraint, 4 Load/Save Model, 5 Save Scenario
  int sv_cedit = -1;  // the constraint being changed (-1: Add)
  model::SolverModel sv_opts;  // the Options dialog's copy until OK
  int sv_opt_tab = 0;
  bool sv_opt_set = false;
  char sv_ls[128] = "";   // Load/Save Model's range
  char sv_scn[128] = "";  // Save Scenario's name
  bool sv_reports[3] = {false, false, false};  // Answer, Sensitivity, Limits
  bool sv_return = false;  // Return to Solver Parameters Dialog
  Document::SolverResult sv_result;
  uint32_t sv_sheet = 0;
};

WhatIfUi& W() {
  static WhatIfUi w;
  return w;
}

// `cancel` given: Esc doesn't close the dialog but sets it, so the dialog does what its Cancel does (Goal Seek Status
// and Solver Results put the original values back, Solver's other pages go back a page); its close box, which closes
// it at once, sets it too
bool BeginDlg(AppState& s, Dialog d, const char* title, float width, bool* cancel = nullptr) {
  if (cancel) *cancel = false;
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(width), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!open && cancel) *cancel = true;  // its close box
    if (!RangePicking() && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return false;
  }
  const bool esc = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  if (cancel && esc) {
    *cancel = true;
    return true;
  }
  if (!open || esc) {
    // the close box or Esc, like Excel's dialogs
    if (cancel) *cancel = true;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    s.dialog = Dialog::None;
    return false;
  }
  return true;
}

void EndDlg(AppState& s, bool close) {
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

std::string Abs(const Rect& r) {
  if (r.IsCell()) return core::CellA1(r.r0, r.c0, true, true);
  return core::CellA1(r.r0, r.c0, true, true) + ":" + core::CellA1(r.r1, r.c1, true, true);
}

void Copy(char* dst, size_t n, const std::string& s) { std::snprintf(dst, n, "%s", s.c_str()); }

// a reference typed in a dialog -> (sheet, rect) parts; false shows the message
bool Refs(AppState& s, const char* text, std::vector<std::pair<uint32_t, Rect>>* out) {
  const uint32_t sheet = s.focus_sheet;
  const std::string t = text;
  const bool ok = s.engine->Call([&](Document& d) { return d.ParseRefList(t, sheet, out); });
  if (!ok) s.ShowError("YCGrid", TR("The reference isn't valid: ") + t);
  return ok;
}

// the cells of refs in reading order, at most max + 1: a list past max is refused anyway, and a whole sheet typed
// as the cells (A:XFD) would be 17 billion of them
std::vector<core::CellPos> CellsOf(const std::vector<std::pair<uint32_t, Rect>>& refs, size_t max) {
  std::vector<core::CellPos> out;
  for (const auto& [rs, rc] : refs) {
    for (uint32_t r = rc.r0; r <= rc.r1 && out.size() <= max; ++r) {
      for (uint32_t c = rc.c0; c <= rc.c1 && out.size() <= max; ++c) out.push_back(core::CellPos{r, c});
    }
  }
  return out;
}

bool OneCell(AppState& s, const char* text, uint32_t* sheet, core::CellPos* p) {
  std::vector<std::pair<uint32_t, Rect>> r;
  if (!Refs(s, text, &r)) return false;
  if (r.size() != 1 || !r[0].second.IsCell()) {
    s.ShowMessage("YCGrid", TR("Enter a single cell: ") + std::string(text));
    return false;
  }
  *sheet = r[0].first;
  *p = core::CellPos{r[0].second.r0, r[0].second.c0};
  return true;
}

std::string CellText(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  return s.snap ? engine::InputTextOf(s.snap->wb, sheet, r, c) : std::string();
}

// a cell or range box: typed, or picked on the sheet with its button (the dialog shrinks while the user selects)
void RefField(AppState& s, const char* label, char* buf, size_t n, float w = 260, int flags = 0) {
  ImGui::TextUnformatted(label);
  RangeBox(s, label, buf, n, S(w), flags, label);
}

// a value box (a number, a name)
void TextField(const char* label, char* buf, size_t n, float w = 260) {
  ImGui::TextUnformatted(label);
  ImGui::SetNextItemWidth(S(w));
  ImGui::InputText((std::string("##") + label).c_str(), buf, n);
}

const char* kRel[] = {"<=", "=", ">=", "int", "bin", "dif"};

}  // namespace

// ---- Commands ----

void RegisterWhatIfCommands(AppState& s) {
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
  R.Add(make("WhatIfMenu", TR("What-If\nAnalysis"), ui::icon::WhatIf, "", TR("Try different values for the formulas: scenarios, goal seek and data tables."))).execute = [](ui::Command&) {};
  R.Add(make("GoalSeek", TR("Goal Seek..."), ui::icon::GoalSeek, "", TR("Find the input value that makes a formula return the result you want."))).execute = [&s](ui::Command&) {
    WhatIfUi& w = W();
    const auto& sel = s.FocusView().sel;
    Copy(w.gs_set, sizeof(w.gs_set), core::CellA1(sel.act_r, sel.act_c, true, true));
    w.gs_to[0] = 0;
    w.gs_by[0] = 0;
    w.gs_done = false;
    s.OpenDialog(Dialog::GoalSeek);
  };
  R.Add(make("ScenarioManager", TR("Scenario Manager..."), ui::icon::Scenario, "", TR("Save groups of input values as scenarios and switch between them."))).execute = [&s](ui::Command&) {
    WhatIfUi& w = W();
    w.sc_mode = 0;
    w.sc_sel = -1;
    s.OpenDialog(Dialog::ScenarioManager);
  };
  R.Add(make("DataTableDialog", TR("Data Table..."), ui::icon::DataTable, "", TR("See the results of formulas for many values of one or two input cells."))).execute = [&s](ui::Command&) {
    WhatIfUi& w = W();
    w.dt_rect = SelRect(s);
    if (w.dt_rect.Rows() < 2 || w.dt_rect.Cols() < 2) {
      s.ShowMessage("YCGrid", TR("Select the range of the data table first: the input values and the formulas, and the cells for the results."));
      return;
    }
    w.dt_row[0] = w.dt_col[0] = 0;
    s.OpenDialog(Dialog::DataTableDlg);
  };
  R.Add(make("Solver", TR("Solver"), ui::icon::Solver, "", TR("Find the best values for variable cells, subject to constraints (linear, nonlinear and integer models)."))).execute = [&s](ui::Command&) {
    WhatIfUi& w = W();
    w.sv_sheet = s.focus_sheet;
    const model::Sheet* sh = s.FocusSheet();
    w.sv = sh && sh->solver->defined ? *sh->solver : model::SolverModel{};
    if (!w.sv.defined) {
      const auto& sel = s.FocusView().sel;
      w.sv.objective = core::CellA1(sel.act_r, sel.act_c, true, true);
    }
    Copy(w.sv_obj, sizeof(w.sv_obj), w.sv.objective);
    Copy(w.sv_changing, sizeof(w.sv_changing), w.sv.changing);
    std::snprintf(w.sv_value, sizeof(w.sv_value), "%g", w.sv.value);
    w.sv_sel = -1;
    w.sv_done = false;
    w.sv_page = 0;
    s.OpenDialog(Dialog::Solver);
  };
  R.Add(make("Consolidate", TR("Consolidate"), ui::icon::Consolidate, "", TR("Combine the values of several ranges into one, by position or by labels."))).execute = [&s](ui::Command&) {
    WhatIfUi& w = W();
    w.co_ref[0] = 0;
    w.co_sel = -1;
    // the settings and references used last on this sheet (kept with the workbook, as Excel)
    if (const model::Sheet* sh = s.FocusSheet()) {
      const model::ConsolidateSettings& co = *sh->consolidate;
      w.co_func = std::clamp(co.func, 0, 10);
      w.co_top = co.top_row;
      w.co_left = co.left_col;
      w.co_links = co.links;
      w.co_refs = co.refs;
    }
    s.OpenDialog(Dialog::Consolidate);
  };
  R.Add(make("AdvancedFilter", TR("Advanced"), ui::icon::AdvancedFilter, "", TR("Filter with a criteria range, in place or by copying the matching rows elsewhere."))).execute = [&s](ui::Command&) {
    WhatIfUi& w = W();
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    Rect r = SelRect(s);
    if (r.IsCell()) r = engine::CurrentRegion(*sh, r.r0, r.c0);
    Copy(w.af_list, sizeof(w.af_list), Abs(r));
    s.OpenDialog(Dialog::AdvancedFilter);
  };
  R.Add(make("FlashFill", TR("Flash Fill"), ui::icon::FlashFill, "Ctrl+E", TR("Fill in values automatically when it senses a pattern in the examples you typed."))).execute = [&s](ui::Command&) {
    if (s.edit.active) return;
    const uint32_t sheet = s.focus_sheet;
    const auto& sel = s.FocusView().sel;
    const uint32_t r = sel.act_r, c = sel.act_c;
    std::string err;
    std::vector<std::pair<uint32_t, std::string>> filled;
    size_t depth = 0;
    const uint32_t n = s.engine->Call([&](Document& d) {
      d.Begin(TR("Flash Fill"));
      const uint32_t k = d.FlashFill(sheet, r, c, &err, &filled);
      d.Commit();
      depth = d.UndoDepth();
      return k;
    });
    std::vector<uint32_t> rows;
    std::vector<std::string> texts;
    for (const auto& f : filled) rows.push_back(f.first), texts.push_back(f.second);
    if (!n) s.ShowError(TR("Flash Fill"), err.empty() ? TR("Flash Fill didn't recognize a pattern.") : err);
    else FlashFillDone(s, sheet, r, c, rows, depth, texts);  // the Flash Fill Options button
  };
  RegisterDataToolCommands(s);
}

// ---- Dialogs ----

void DrawWhatIfDialogs(AppState& s) {
  WhatIfUi& w = W();
  DrawDataToolDialogs(s);

  // Goal Seek
  const bool gs_status = s.dialog == Dialog::GoalSeek && w.gs_done;
  bool gs_cancel = false;
  if (BeginDlg(s, Dialog::GoalSeek, TR("Goal Seek###yc_goalseek"), 320, &gs_cancel)) {
    bool close = false;
    if (!w.gs_done) {
      RefField(s, TR("Set cell:"), w.gs_set, sizeof(w.gs_set), 260, RangeBox_Cell);
      TextField(TR("To value:"), w.gs_to, sizeof(w.gs_to));
      RefField(s, TR("By changing cell:"), w.gs_by, sizeof(w.gs_by), 260, RangeBox_Cell);
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        uint32_t ss = 0, cs = 0;
        core::CellPos sp{}, cp{};
        char* end = nullptr;
        const double target = std::strtod(w.gs_to, &end);
        if (!end || *end || !w.gs_to[0]) {
          s.ShowMessage("YCGrid", TR("To value must be a number."));
        } else if (OneCell(s, w.gs_set, &ss, &sp) && OneCell(s, w.gs_by, &cs, &cp)) {
          bool changed = false;
          w.gs_result = s.engine->Call([&](Document& d) {
            const size_t before = d.UndoDepth();
            d.Begin(TR("Goal Seek"));
            auto r = d.GoalSeek(ss, sp.row, sp.col, target, cs, cp.row, cp.col);
            d.Commit();
            changed = d.UndoDepth() > before;
            return r;
          });
          w.gs_changed = changed;
          w.gs_sheet = ss;
          w.gs_cell = sp;
          if (w.gs_result.iterations == 0 && !w.gs_result.found && !w.gs_result.error.empty() && !changed) {
            // the model can't be searched ("Cell must contain a formula."): say so and stay on this page, as Excel does
            AppMessageBox(s, w.gs_result.error, TR("Goal Seek"), MB_OK | MB_ICONERROR, IDOK);
          } else {
            w.gs_done = true;
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || gs_cancel) close = true;
    } else {
      ImGui::TextUnformatted((std::string(TR("Goal Seeking with Cell ")) + w.gs_set + (w.gs_result.found ? TR(" found a solution.") : TR(" may not have found a solution."))).c_str());
      if (!w.gs_result.error.empty() && !w.gs_result.found) ImGui::TextWrapped("%s", w.gs_result.error.c_str());
      // the values as the Set cell shows them (its number format)
      std::string code = "General";
      if (const model::Sheet* gsh = s.snap ? s.snap->Sheet(w.gs_sheet) : nullptr) code = s.snap->wb.styles->NumberFormat(s.snap->wb.styles->Get(gsh->GetStyle(w.gs_cell.row, w.gs_cell.col)));
      const std::string target_text = engine::DisplayTextOf(core::Value::Number(std::strtod(w.gs_to, nullptr)), code);
      const std::string current_text = engine::DisplayTextOf(core::Value::Number(w.gs_result.result), code);
      ImGui::Text("%s %s", TR("Target value:"), target_text.c_str());
      ImGui::Text("%s %s", TR("Current value:"), current_text.c_str());
      // Step / Pause act while a search runs; it has ended here, so they are greyed, as in Excel
      ImGui::BeginDisabled();
      ImGui::Button(TR("Step"), ImVec2(S(80), 0));
      ImGui::SameLine();
      ImGui::Button(TR("Pause"), ImVec2(S(80), 0));
      ImGui::EndDisabled();
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) close = true;
      ImGui::SameLine();
      // Cancel, Esc and the close box put the original value back, as Excel's (Esc kept the value found)
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || gs_cancel) {
        if (w.gs_changed) s.engine->Call([](Document& d) { return d.Undo(); });  // back to the original value (only the search's own step)
        w.gs_changed = false;
        close = true;
      }
    }
    EndDlg(s, close);
  } else if (gs_cancel && gs_status && w.gs_changed) {
    s.engine->Call([](Document& d) { return d.Undo(); });  // closed by its box
    w.gs_changed = false;
  }

  // Data Table
  if (BeginDlg(s, Dialog::DataTableDlg, TR("Data Table###yc_datatable"), 320)) {
    bool close = false;
    RefField(s, TR("Row input cell:"), w.dt_row, sizeof(w.dt_row), 260, RangeBox_Cell);
    RefField(s, TR("Column input cell:"), w.dt_col, sizeof(w.dt_col), 260, RangeBox_Cell);
    const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
    if (ok_pressed && RefuseReadOnlyUi(s)) {
      close = true;
    } else if (ok_pressed) {
      uint32_t rs = 0, cs = 0;
      core::CellPos rp{}, cp{};
      const bool has_row = w.dt_row[0] != 0, has_col = w.dt_col[0] != 0;
      if ((!has_row || OneCell(s, w.dt_row, &rs, &rp)) && (!has_col || OneCell(s, w.dt_col, &cs, &cp))) {
        const uint32_t sheet = s.focus_sheet;
        if ((has_row && rs != sheet) || (has_col && cs != sheet)) {
          s.ShowMessage("YCGrid", TR("Input cell reference is not valid: it must be on the sheet of the data table."));
        } else {
          std::string err;
          const Rect rect = w.dt_rect;
          const bool ok = s.engine->Call([&](Document& d) {
            d.Begin(TR("Data Table"));
            const bool r = d.AddDataTable(sheet, rect, has_row ? &rp : nullptr, has_col ? &cp : nullptr, &err);
            d.Commit();
            return r;
          });
          if (ok) close = true;
          else if (!err.empty()) s.ShowError("YCGrid", err);  // a refusal (read-only mode, protection) says why itself
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }

  // Scenario Manager
  bool sc_cancel = false;  // Esc: the page's Cancel (back to the list), or Close on the list, as in Excel
  if (BeginDlg(s, Dialog::ScenarioManager, TR("Scenario Manager###yc_scenarios"), 460, &sc_cancel)) {
    bool close = false;
    const uint32_t sheet = s.focus_sheet;
    const model::Sheet* sh = s.FocusSheet();
    const std::vector<model::Scenario> list = sh ? *sh->scenarios : std::vector<model::Scenario>{};
    if (w.sc_mode == 0) {
      ImGui::TextUnformatted(TR("Scenarios:"));
      if (ImGui::BeginChild("##sclist", ImVec2(S(260), S(150)), ImGuiChildFlags_Borders)) {
        if (list.empty()) ImGui::TextDisabled("%s", TR("No Scenarios defined. Choose Add to add scenarios."));
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
          if (ImGui::Selectable(list[static_cast<size_t>(i)].name.c_str(), w.sc_sel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            w.sc_sel = i;
            if (ImGui::IsMouseDoubleClicked(0)) s.engine->PostUser(TR("Show Scenario"), [sheet, n = list[static_cast<size_t>(i)].name](Document& d) { d.ShowScenario(sheet, n); });
          }
        }
      }
      ImGui::EndChild();
      ImGui::SameLine();
      ImGui::BeginGroup();
      const bool has = w.sc_sel >= 0 && w.sc_sel < static_cast<int>(list.size());
      if (ImGui::Button(TR("Add..."), ImVec2(S(120), 0))) {
        w.sc_name[0] = 0;
        // Excel's comment: "Created by <user> on <date>"
        wchar_t user[256];
        DWORD un = 256;
        const std::string who = ::GetUserNameW(user, &un) && un > 1 ? ToUtf8(std::wstring(user, un - 1)) : std::string("YCGrid");
        Copy(w.sc_comment, sizeof(w.sc_comment), std::string(TR("Created by ")) + who + TR(" on ") + TodayText());
        Copy(w.sc_cells, sizeof(w.sc_cells), Abs(SelRect(s)));
        w.sc_editing.clear();
        w.sc_locked = true;
        w.sc_hidden = false;
        w.sc_mode = 1;
      }
      ImGui::BeginDisabled(!has);
      if (ImGui::Button(TR("Delete"), ImVec2(S(120), 0))) {
        const std::string n = list[static_cast<size_t>(w.sc_sel)].name;
        s.engine->PostUser(TR("Delete Scenario"), [sheet, n](Document& d) { d.DeleteScenario(sheet, n); });
        w.sc_sel = -1;
      }
      if (ImGui::Button(TR("Edit..."), ImVec2(S(120), 0))) {
        const auto& sc = list[static_cast<size_t>(w.sc_sel)];
        Copy(w.sc_name, sizeof(w.sc_name), sc.name);
        Copy(w.sc_comment, sizeof(w.sc_comment), sc.comment);
        std::string cells;
        for (const auto& p : sc.cells) cells += (cells.empty() ? "" : ",") + core::CellA1(p.row, p.col, true, true);
        Copy(w.sc_cells, sizeof(w.sc_cells), cells);
        w.sc_editing = sc.name;
        w.sc_locked = sc.locked;
        w.sc_hidden = sc.hidden;
        w.sc_mode = 1;
      }
      ImGui::EndDisabled();
      if (ImGui::Button(TR("Merge..."), ImVec2(S(120), 0))) {
        w.sc_merge_sel = -1;
        w.sc_mode = 4;
      }
      ImGui::BeginDisabled(list.empty());
      if (ImGui::Button(TR("Summary..."), ImVec2(S(120), 0))) {
        Copy(w.sc_results, sizeof(w.sc_results), Abs(SelRect(s)));  // Excel starts with the selection
        w.sc_report = 0;
        w.sc_mode = 3;
      }
      ImGui::EndDisabled();
      ImGui::EndGroup();
      if (has) {
        const auto& sc = list[static_cast<size_t>(w.sc_sel)];
        std::string cells;
        for (const auto& p : sc.cells) cells += (cells.empty() ? "" : ",") + core::CellA1(p.row, p.col, true, true);
        ImGui::Text("%s %s", TR("Changing cells:"), cells.c_str());
        if (!sc.comment.empty()) ImGui::TextWrapped("%s %s", TR("Comment:"), sc.comment.c_str());
      }
      ImGui::BeginDisabled(!has);
      if (ImGui::Button(TR("Show"), ImVec2(S(80), 0))) {
        const std::string n = list[static_cast<size_t>(w.sc_sel)].name;
        s.engine->PostUser(TR("Show Scenario"), [sheet, n](Document& d) { d.ShowScenario(sheet, n); });
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Close"), ImVec2(S(80), 0)) || sc_cancel) close = true;
    } else if (w.sc_mode == 1) {
      TextField(TR("Scenario name:"), w.sc_name, sizeof(w.sc_name));
      RefField(s, TR("Changing cells:"), w.sc_cells, sizeof(w.sc_cells), 380);
      ImGui::TextUnformatted(TR("Comment:"));
      ImGui::InputTextMultiline("##sccomment", w.sc_comment, sizeof(w.sc_comment), ImVec2(S(380), S(50)));
      ImGui::SeparatorText(TR("Protection"));
      ImGui::Checkbox(TR("Prevent changes"), &w.sc_locked);
      ImGui::SameLine();
      ImGui::Checkbox(TR("Hide"), &w.sc_hidden);
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        std::vector<std::pair<uint32_t, Rect>> refs;
        bool taken = false;  // another scenario has the name (Excel refuses it; renaming to itself is fine)
        for (const auto& sc : list) taken = taken || (sc.name == w.sc_name && sc.name != w.sc_editing);
        if (w.sc_name[0] == 0) {
          s.ShowMessage("YCGrid", TR("Enter a scenario name."));
        } else if (taken) {
          s.ShowMessage("YCGrid", TR("A scenario with this name already exists. Enter another name."));
        } else if (Refs(s, w.sc_cells, &refs)) {
          w.sc_cell_list = CellsOf(refs, 32);
          if (w.sc_cell_list.size() > 32) {
            s.ShowMessage("YCGrid", TR("A scenario can have at most 32 changing cells."));
          } else {
            // values: the scenario's own when editing, otherwise the cells' current values
            w.sc_values.clear();
            const model::Scenario* old = nullptr;
            for (const auto& sc : list) {
              if (!w.sc_editing.empty() && sc.name == w.sc_editing) old = &sc;
            }
            for (const auto& p : w.sc_cell_list) {
              std::string v = CellText(s, sheet, p.row, p.col);
              if (old) {
                for (size_t k = 0; k < old->cells.size(); ++k) {
                  if (old->cells[k].row == p.row && old->cells[k].col == p.col && k < old->values.size())
                    v = engine::InputTextOf(old->values[k], "General");
                }
              }
              w.sc_values.push_back(v);
            }
            w.sc_mode = 2;
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sc_cancel) w.sc_mode = 0;
    } else if (w.sc_mode == 2) {
      ImGui::TextUnformatted(TR("Enter values for each of the changing cells."));
      for (size_t i = 0; i < w.sc_cell_list.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", core::CellA1(w.sc_cell_list[i].row, w.sc_cell_list[i].col, true, true).c_str());
        ImGui::SameLine(S(80));
        char buf[128];
        Copy(buf, sizeof(buf), w.sc_values[i]);
        ImGui::SetNextItemWidth(S(220));
        if (ImGui::InputText("##v", buf, sizeof(buf))) w.sc_values[i] = buf;
        ImGui::PopID();
      }
      const bool add_next = [&] {
        ImGui::BeginGroup();
        const bool ok = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
        ImGui::SameLine();
        const bool cancel = ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sc_cancel;
        ImGui::SameLine();
        const bool add = ImGui::Button(TR("Add"), ImVec2(S(80), 0));
        ImGui::EndGroup();
        if (cancel) w.sc_mode = 0;
        w.sc_values_ok = ok || add;
        return add;
      }();
      if (w.sc_values_ok) {
        model::Scenario sc;
        sc.name = w.sc_name;
        sc.comment = w.sc_comment;
        sc.locked = w.sc_locked;
        sc.hidden = w.sc_hidden;
        sc.cells = w.sc_cell_list;
        for (const auto& v : w.sc_values) sc.values.push_back(formula::ParseInput(v).value);
        std::string err;
        const std::string was = w.sc_editing;
        const bool ok = s.engine->Call([&](Document& d) {
          d.Begin(was.empty() ? TR("Add Scenario") : TR("Edit Scenario"));
          if (!was.empty() && was != sc.name) d.DeleteScenario(sheet, was);  // renamed: the old one goes
          const bool r = d.AddScenario(sheet, sc, &err);
          d.Commit();
          return r;
        });
        if (!ok && !err.empty()) s.ShowError("YCGrid", err);
        w.sc_mode = 0;
        if (ok && add_next) {
          // Add: the next scenario, with a new name and the same changing cells (Excel's Scenario Values > Add)
          w.sc_name[0] = 0;
          w.sc_editing.clear();
          w.sc_mode = 1;
        }
      }
    } else if (w.sc_mode == 4) {
      // Merge Scenarios: the sheets of this workbook, and how many scenarios each has
      ImGui::TextUnformatted(TR("Merge scenarios from"));
      ImGui::TextUnformatted(TR("Sheet:"));
      int count = 0;
      if (ImGui::BeginChild("##scmerge", ImVec2(S(260), S(150)), ImGuiChildFlags_Borders) && s.snap) {
        const auto& sheets = s.snap->wb.sheets;
        for (int i = 0; i < static_cast<int>(sheets.size()); ++i) {
          const model::Sheet& x = sheets[static_cast<size_t>(i)].get();
          if (x.id == sheet) continue;
          ImGui::PushID(i);
          if (ImGui::Selectable((x.name + "##sh").c_str(), w.sc_merge_sel == i)) w.sc_merge_sel = i;
          ImGui::PopID();
          if (w.sc_merge_sel == i) count = static_cast<int>(x.scenarios->size());
        }
      }
      ImGui::EndChild();
      ImGui::Text(TR("There are %d scenarios on source sheet."), count);
      const bool has = s.snap && w.sc_merge_sel >= 0 && w.sc_merge_sel < static_cast<int>(s.snap->wb.sheets.size());
      ImGui::BeginDisabled(!has);
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) && has) {
        const uint32_t src = s.snap->wb.sheets[static_cast<size_t>(w.sc_merge_sel)]->id;
        s.engine->Call([&](Document& d) {
          d.Begin(TR("Merge Scenarios"));
          const int n = d.MergeScenarios(sheet, src);
          d.Commit();
          return n;
        });
        w.sc_mode = 0;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sc_cancel) w.sc_mode = 0;
    } else {
      ImGui::SeparatorText(TR("Report type"));
      ImGui::RadioButton(TR("Scenario summary"), &w.sc_report, 0);
      ImGui::RadioButton(TR("Scenario PivotTable report"), &w.sc_report, 1);
      RefField(s, TR("Result cells:"), w.sc_results, sizeof(w.sc_results), 380);
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        std::vector<std::pair<uint32_t, Rect>> refs;
        std::vector<core::CellPos> res;
        if (!w.sc_results[0] || Refs(s, w.sc_results, &refs)) {
          res = CellsOf(refs, 32);
          std::string err;
          const bool pivot = w.sc_report == 1;
          const uint32_t id = s.engine->Call([&](Document& d) {
            d.Begin(TR("Scenario Summary"));
            const uint32_t r = pivot ? d.ScenarioPivotReport(sheet, res, &err) : d.ScenarioSummary(sheet, res, &err);
            d.Commit();
            return r;
          });
          if (!id) {
            if (!err.empty()) s.ShowError("YCGrid", err);
          } else {
            s.engine->Sync();
            s.snap = s.engine->Current();
            ActivateSheet(s, id);
            close = true;
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sc_cancel) w.sc_mode = 0;
    }
    EndDlg(s, close);
  }

  // Consolidate
  if (BeginDlg(s, Dialog::Consolidate, TR("Consolidate###yc_consolidate"), 440)) {
    bool close = false;
    const char* kFuncs[] = {TR("Sum"), TR("Count"), TR("Average"), TR("Max"), TR("Min"), TR("Product"), TR("Count Numbers"), TR("StdDev"), TR("StdDevp"), TR("Var"), TR("Varp")};
    ImGui::TextUnformatted(TR("Function:"));
    ImGui::SetNextItemWidth(S(200));
    ImGui::Combo("##cofunc", &w.co_func, kFuncs, 11);
    RefField(s, TR("Reference:"), w.co_ref, sizeof(w.co_ref), 300, RangeBox_Sheet);
    ImGui::SameLine();
    if (ImGui::Button(TR("Browse..."))) {
      // another workbook's file: its name goes in the box, to be followed by the sheet and the range ([Book.xlsx]Sheet1!A1:B5)
      wchar_t file[MAX_PATH * 4] = L"";
      OPENFILENAMEW ofn{sizeof(ofn)};
      ofn.hwndOwner = s.hwnd;
      ofn.lpstrFilter = L"Workbooks (*.ycg;*.xlsx;*.xlsm;*.csv)\0*.ycg;*.xlsx;*.xlsm;*.csv\0All files\0*.*\0";
      ofn.lpstrFile = file;
      ofn.nMaxFile = static_cast<DWORD>(std::size(file));
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
      std::wstring answer;
      const bool hidden = NoFileDialog(s.hwnd, TR("Browse"), &answer);
      if (hidden ? !answer.empty() : ::GetOpenFileNameW(&ofn) != 0) {
        const std::wstring path = hidden ? answer : std::wstring(file);
        const std::filesystem::path fp(path);
        Copy(w.co_ref, sizeof(w.co_ref), "'" + ToUtf8(fp.parent_path().wstring()) + "\\[" + ToUtf8(fp.filename().wstring()) + "]Sheet1'!");
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Add"))) {
      std::vector<std::pair<uint32_t, Rect>> refs;
      if (w.co_ref[0] && Refs(s, w.co_ref, &refs)) {
        w.co_refs.push_back(w.co_ref);
        w.co_ref[0] = 0;
      }
    }
    ImGui::TextUnformatted(TR("All references:"));
    if (ImGui::BeginChild("##corefs", ImVec2(S(300), S(110)), ImGuiChildFlags_Borders)) {
      for (int i = 0; i < static_cast<int>(w.co_refs.size()); ++i) {
        if (ImGui::Selectable(w.co_refs[static_cast<size_t>(i)].c_str(), w.co_sel == i)) {
          w.co_sel = i;
          Copy(w.co_ref, sizeof(w.co_ref), w.co_refs[static_cast<size_t>(i)]);  // Excel shows it in the Reference box
        }
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginDisabled(w.co_sel < 0 || w.co_sel >= static_cast<int>(w.co_refs.size()));
    if (ImGui::Button(TR("Delete"))) {
      w.co_refs.erase(w.co_refs.begin() + w.co_sel);
      w.co_sel = -1;
    }
    ImGui::EndDisabled();
    ImGui::TextUnformatted(TR("Use labels in"));
    ImGui::Checkbox(TR("Top row"), &w.co_top);
    ImGui::SameLine(S(200));
    ImGui::Checkbox(TR("Create links to source data"), &w.co_links);
    ImGui::Checkbox(TR("Left column"), &w.co_left);
    // the settings stay with the sheet whichever way the dialog closes
    auto keep = [&]() {
      model::ConsolidateSettings co;
      co.func = w.co_func;
      co.top_row = w.co_top;
      co.left_col = w.co_left;
      co.links = w.co_links;
      co.refs = w.co_refs;
      const uint32_t sheet = s.focus_sheet;
      s.engine->Post([sheet, co](Document& d) { d.SetConsolidateSettings(sheet, co); });
    };
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      std::vector<std::pair<uint32_t, Rect>> all;
      bool ok = !w.co_refs.empty();
      for (const auto& r : w.co_refs) {
        std::vector<std::pair<uint32_t, Rect>> p;
        ok = ok && Refs(s, r.c_str(), &p);
        all.insert(all.end(), p.begin(), p.end());
      }
      if (!ok && w.co_refs.empty()) s.ShowMessage("YCGrid", TR("Add at least one reference."));
      if (ok) {
        const uint32_t sheet = s.focus_sheet;
        const auto& sel = s.FocusView().sel;
        const uint32_t r = sel.act_r, c = sel.act_c;
        std::string err;
        const int func = w.co_func;
        const bool top = w.co_top, left = w.co_left, links = w.co_links;
        const bool done = s.engine->Call([&](Document& d) {
          d.Begin(TR("Consolidate"));
          const bool x = d.Consolidate(sheet, r, c, all, func, top, left, &err, links);
          d.Commit();
          return x;
        });
        if (done) keep(), close = true;
        else if (!err.empty()) s.ShowError("YCGrid", err);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Close"), ImVec2(S(80), 0))) keep(), close = true;
    EndDlg(s, close);
  }

  // Advanced Filter
  if (BeginDlg(s, Dialog::AdvancedFilter, TR("Advanced Filter###yc_advfilter"), 360)) {
    bool close = false;
    ImGui::TextUnformatted(TR("Action"));
    ImGui::RadioButton(TR("Filter the list, in-place"), &w.af_action, 0);
    ImGui::RadioButton(TR("Copy to another location"), &w.af_action, 1);
    RefField(s, TR("List range:"), w.af_list, sizeof(w.af_list));
    RefField(s, TR("Criteria range:"), w.af_crit, sizeof(w.af_crit));
    ImGui::BeginDisabled(w.af_action == 0);
    RefField(s, TR("Copy to:"), w.af_dest, sizeof(w.af_dest));
    ImGui::EndDisabled();
    ImGui::Checkbox(TR("Unique records only"), &w.af_unique);
    const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
    if (ok_pressed && RefuseReadOnlyUi(s)) {
      close = true;  // read-only mode: the status bar says why, not "The filter couldn't be applied."
    } else if (ok_pressed) {
      std::vector<std::pair<uint32_t, Rect>> list, crit, dest;
      const bool copy = w.af_action == 1;
      bool ok = Refs(s, w.af_list, &list) && list.size() == 1;
      if (ok && w.af_crit[0]) ok = Refs(s, w.af_crit, &crit) && crit.size() == 1;
      if (ok && copy) ok = Refs(s, w.af_dest, &dest) && dest.size() == 1;
      if (ok) {
        std::string err;
        const bool unique = w.af_unique;
        const bool done = s.engine->Call([&](Document& d) {
          d.Begin(TR("Advanced Filter"));
          const bool x = d.AdvancedFilter(list[0].first, list[0].second, crit.empty() ? 0 : crit[0].first, crit.empty() ? Rect{} : crit[0].second, copy,
                                          dest.empty() ? 0 : dest[0].first, dest.empty() ? Rect{} : dest[0].second, unique, &err);
          d.Commit();
          return x;
        });
        if (done) close = true;
        else s.ShowError("YCGrid", err.empty() ? TR("The filter couldn't be applied.") : err);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }

  // Solver: Solver Parameters, and its own dialogs as pages of the same window (Add / Change Constraint, Options,
  // Load/Save Model, Solver Results, Save Scenario), as Excel shows them over the parameters
  {
    const char* titles[] = {TR("Solver Parameters###yc_solver"), TR("Solver Results###yc_solver"), TR("Options###yc_solver"),
                            w.sv_cedit < 0 ? TR("Add Constraint###yc_solver") : TR("Change Constraint###yc_solver"), TR("Load/Save Model###yc_solver"),
                            TR("Save Scenario###yc_solver")};
    const int page = std::clamp(w.sv_page, 0, 5);
    if (s.dialog == Dialog::Solver) ui::NoteWindowTitle(titles[page]);
    bool sv_cancel = false;
    const bool sv_results = s.dialog == Dialog::Solver && page == 1;
    if (BeginDlg(s, Dialog::Solver, titles[page], page == 0 ? 560.0f : 420.0f, &sv_cancel)) {
      bool close = false;
      const uint32_t sheet = w.sv_sheet;
      auto read_model = [&]() {
        w.sv.defined = true;
        w.sv.objective = w.sv_obj;
        w.sv.changing = w.sv_changing;
        w.sv.value = std::strtod(w.sv_value, nullptr);
      };
      auto save_model = [&]() {
        read_model();
        model::SolverModel m = w.sv;
        s.engine->PostUser(TR("Solver"), [sheet, m](Document& d) { d.SetSolverModel(sheet, m); });
      };
      auto constraint_text = [](const model::SolverConstraint& c) {
        // Excel's list: "$B$2 <= 10", "$B$3 = integer", "$B$4 = binary", "$B$5:$B$9 = AllDifferent"
        static const char* kShown[] = {"", "", "", "= integer", "= binary", "= AllDifferent"};
        const int rk = std::clamp(c.rel, 1, 6) - 1;
        return c.ref + " " + (rk <= 2 ? std::string(kRel[rk]) + " " + c.rhs : std::string(kShown[rk]));
      };
      if (page == 0) {
        RefField(s, TR("Set Objective:"), w.sv_obj, sizeof(w.sv_obj), 260, RangeBox_Cell);
        ImGui::TextUnformatted(TR("To:"));
        ImGui::SameLine();
        ImGui::RadioButton(TR("Max"), &w.sv.goal, 1);
        ImGui::SameLine();
        ImGui::RadioButton(TR("Min"), &w.sv.goal, 2);
        ImGui::SameLine();
        ImGui::RadioButton(TR("Value Of:"), &w.sv.goal, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(90));
        ImGui::BeginDisabled(w.sv.goal != 3);  // Value Of only counts with its option
        ImGui::InputText("##svvalue", w.sv_value, sizeof(w.sv_value));
        ImGui::EndDisabled();
        RefField(s, TR("By Changing Variable Cells:"), w.sv_changing, sizeof(w.sv_changing), 420);
        ImGui::TextUnformatted(TR("Subject to the Constraints:"));
        if (ImGui::BeginChild("##svcons", ImVec2(S(400), S(150)), ImGuiChildFlags_Borders)) {
          for (int i = 0; i < static_cast<int>(w.sv.constraints.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(constraint_text(w.sv.constraints[static_cast<size_t>(i)]).c_str(), w.sv_sel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
              w.sv_sel = i;
              if (ImGui::IsMouseDoubleClicked(0)) {  // a double-click changes it, as Excel
                const auto& c = w.sv.constraints[static_cast<size_t>(i)];
                Copy(w.sv_cref, sizeof(w.sv_cref), c.ref);
                Copy(w.sv_crhs, sizeof(w.sv_crhs), c.rhs);
                w.sv_crel = std::clamp(c.rel, 1, 6) - 1;
                w.sv_cedit = i;
                w.sv_page = 3;
              }
            }
            ImGui::PopID();
          }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        const ImVec2 bw(S(110), 0);
        if (ImGui::Button(TR("Add"), bw)) {
          w.sv_cref[0] = w.sv_crhs[0] = 0;
          w.sv_crel = 0;
          w.sv_cedit = -1;
          w.sv_page = 3;
        }
        ImGui::BeginDisabled(w.sv_sel < 0 || w.sv_sel >= static_cast<int>(w.sv.constraints.size()));
        if (ImGui::Button(TR("Change"), bw)) {
          const auto& c = w.sv.constraints[static_cast<size_t>(w.sv_sel)];
          Copy(w.sv_cref, sizeof(w.sv_cref), c.ref);
          Copy(w.sv_crhs, sizeof(w.sv_crhs), c.rhs);
          w.sv_crel = std::clamp(c.rel, 1, 6) - 1;
          w.sv_cedit = w.sv_sel;
          w.sv_page = 3;
        }
        if (ImGui::Button(TR("Delete"), bw)) {
          w.sv.constraints.erase(w.sv.constraints.begin() + w.sv_sel);
          w.sv_sel = -1;
        }
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(1, S(6)));
        if (ImGui::Button(TR("Reset All"), bw)) {
          // Excel asks first: "Reset all Solver options and cell selections?"
          if (AppMessageBox(s, TR("Reset all Solver options and cell selections?"), TR("Solver"), MB_OKCANCEL | MB_ICONQUESTION, IDOK) == IDOK) {
            w.sv = model::SolverModel{};
            w.sv_obj[0] = w.sv_changing[0] = 0;
            std::snprintf(w.sv_value, sizeof(w.sv_value), "0");
            w.sv_sel = -1;
          }
        }
        if (ImGui::Button(TR("Load/Save"), bw)) {
          const auto& sel = s.FocusView().sel;
          Copy(w.sv_ls, sizeof(w.sv_ls), core::CellA1(sel.act_r, sel.act_c, true, true));
          w.sv_page = 4;
        }
        ImGui::EndGroup();
        ImGui::Checkbox(TR("Make Unconstrained Variables Non-Negative"), &w.sv.non_negative);
        const char* kMethods[] = {TR("GRG Nonlinear"), TR("Simplex LP"), TR("Evolutionary")};
        int method = std::clamp(w.sv.method, 1, 3) - 1;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Select a Solving Method:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(200));
        ui::NoteUiItem("##svmethod", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(200), ImGui::GetFrameHeight())));
        if (ImGui::Combo("##svmethod", &method, kMethods, 3)) w.sv.method = method + 1;
        ImGui::SameLine();
        if (ImGui::Button(TR("Options"), ImVec2(S(90), 0))) {
          w.sv_opts = w.sv;
          w.sv_opt_tab = w.sv.method == 3 ? 2 : w.sv.method == 1 ? 1 : 0;
          w.sv_opt_set = true;
          w.sv_page = 2;
        }
        ImGui::SeparatorText(TR("Solving Method"));
        ImGui::PushTextWrapPos(S(540));
        ImGui::TextUnformatted(TR("Select the GRG Nonlinear engine for Solver Problems that are smooth nonlinear. Select the LP Simplex engine for linear Solver "
                                  "Problems, and select the Evolutionary engine for Solver problems that are non-smooth."));
        ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - S(190));
        if (DlgOkButton(TR("Solve"), ImVec2(S(90), 0))) {
          read_model();
          model::SolverModel m = w.sv;
          bool changed = false;
          w.sv_result = s.engine->Call([&](Document& d) {
            // the model is saved as its own step; the solve is another, the only one Restore Original Values takes back
            d.Begin(TR("Solver"));
            d.SetSolverModel(sheet, m);
            d.Commit();
            const size_t before = d.UndoDepth();
            d.Begin(TR("Solver"));
            auto r = d.Solve(sheet, m);
            d.Commit();
            changed = d.UndoDepth() > before;
            return r;
          });
          w.sv_changed = changed;
          if (w.sv_result.status == 13 && !changed) {
            // an error in the model (variable cells with formulas...): a message, and the parameters stay open
            AppMessageBox(s, w.sv_result.message, TR("Solver"), MB_OK | MB_ICONERROR, IDOK);
          } else {
            w.sv_page = 1;
            w.sv_keep = w.sv_result.status != 5 && w.sv_result.status != 13 && w.sv_result.status != 7;
            w.sv_reports[0] = w.sv_reports[1] = w.sv_reports[2] = false;
            w.sv_return = false;
          }
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Close"), ImVec2(S(90), 0)) || sv_cancel) {  // Esc is Close here, as in Excel
          save_model();
          close = true;
        }
      } else if (page == 3) {
        // Add Constraint / Change Constraint: Cell Reference, the relation, Constraint
        ImGui::BeginGroup();
        ImGui::TextUnformatted(TR("Cell Reference:"));
        RangeBox(s, "solver.cref", w.sv_cref, sizeof(w.sv_cref), S(160), 0, TR("Cell Reference:"));
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(" ");
        ImGui::SetNextItemWidth(S(60));
        ui::NoteUiItem("##crel", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(60), ImGui::GetFrameHeight())));
        if (ImGui::Combo("##crel", &w.sv_crel, kRel, 6)) {
          static const char* kWords[] = {"", "", "", "integer", "binary", "AllDifferent"};
          if (w.sv_crel >= 3) Copy(w.sv_crhs, sizeof(w.sv_crhs), kWords[w.sv_crel]);  // Excel shows the word in the box
          else if (!std::strcmp(w.sv_crhs, "integer") || !std::strcmp(w.sv_crhs, "binary") || !std::strcmp(w.sv_crhs, "AllDifferent")) w.sv_crhs[0] = 0;
        }
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(TR("Constraint:"));
        ImGui::BeginDisabled(w.sv_crel >= 3);
        RangeBox(s, "solver.crhs", w.sv_crhs, sizeof(w.sv_crhs), S(160), RangeBox_Formula, TR("Constraint:"));
        ImGui::EndDisabled();
        ImGui::EndGroup();
        const model::SolverConstraint cur{w.sv_cref, w.sv_crel + 1, w.sv_crel >= 3 ? std::string() : std::string(w.sv_crhs)};
        auto valid = [&]() {
          if (!w.sv_cref[0] || (w.sv_crel < 3 && !w.sv_crhs[0])) {
            s.ShowMessage(TR("Solver"), TR("Enter a cell reference and a constraint."));
            return false;
          }
          return true;
        };
        ImGui::Separator();
        if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
          if (valid()) {
            if (w.sv_cedit >= 0 && w.sv_cedit < static_cast<int>(w.sv.constraints.size())) w.sv.constraints[static_cast<size_t>(w.sv_cedit)] = cur;
            else w.sv.constraints.push_back(cur);
            w.sv_page = 0;
          }
        }
        if (w.sv_cedit < 0) {
          ImGui::SameLine();
          if (ImGui::Button(TR("Add"), ImVec2(S(80), 0)) && valid()) {  // keep adding: the boxes empty again
            w.sv.constraints.push_back(cur);
            w.sv_cref[0] = w.sv_crhs[0] = 0;
          }
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sv_cancel) w.sv_page = 0;  // Esc goes back to the parameters, as Cancel
      } else if (page == 2) {
        // Options: All Methods, GRG Nonlinear, Evolutionary
        model::SolverModel& o = w.sv_opts;
        auto num = [](const char* label, const char* id, double* v, const char* fmt) {
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(label);
          ImGui::SameLine(S(250));
          ImGui::SetNextItemWidth(S(120));
          ImGui::InputDouble(id, v, 0, 0, fmt);
        };
        auto inum = [](const char* label, const char* id, int* v, bool blank_zero) {
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(label);
          ImGui::SameLine(S(250));
          ImGui::SetNextItemWidth(S(120));
          char b[32];
          std::snprintf(b, sizeof(b), "%s", blank_zero && *v == 0 ? "" : std::to_string(*v).c_str());
          if (ImGui::InputText(id, b, sizeof(b), ImGuiInputTextFlags_CharsDecimal)) *v = std::max(0, std::atoi(b));
        };
        if (BeginDlgTabBar("##svopts")) {
          if (ImGui::BeginTabItem(TR("All Methods"), nullptr, w.sv_opt_tab == 0 && w.sv_opt_set ? ImGuiTabItemFlags_SetSelected : 0)) {
            num(TR("Constraint Precision:"), "##o_prec", &o.precision, "%g");
            ImGui::Checkbox(TR("Use Automatic Scaling"), &o.auto_scaling);
            ImGui::Checkbox(TR("Show Iteration Results"), &o.show_iterations);
            ImGui::SeparatorText(TR("Solving with Integer Constraints"));
            ImGui::Checkbox(TR("Ignore Integer Constraints"), &o.ignore_integer);
            num(TR("Integer Optimality (%):"), "##o_intopt", &o.integer_optimality, "%g");
            ImGui::SeparatorText(TR("Solving Limits"));
            num(TR("Max Time (Seconds):"), "##o_time", &o.max_time, "%g");
            inum(TR("Iterations:"), "##o_iter", &o.max_iterations, false);
            ImGui::SeparatorText(TR("Evolutionary and Integer Constraints:"));
            inum(TR("Max Subproblems:"), "##o_subs", &o.max_subproblems, true);
            inum(TR("Max Feasible Solutions:"), "##o_feas", &o.max_feasible, true);
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(TR("GRG Nonlinear"), nullptr, w.sv_opt_tab == 1 && w.sv_opt_set ? ImGuiTabItemFlags_SetSelected : 0)) {
            num(TR("Convergence:"), "##o_conv", &o.convergence, "%g");
            ImGui::SeparatorText(TR("Derivatives"));
            ImGui::RadioButton(TR("Forward"), &o.derivatives, 1);
            ImGui::SameLine();
            ImGui::RadioButton(TR("Central"), &o.derivatives, 2);
            ImGui::SeparatorText(TR("Multistart"));
            ImGui::Checkbox(TR("Use Multistart"), &o.multistart);
            inum(TR("Population Size:"), "##o_pop", &o.population, false);
            inum(TR("Random Seed:"), "##o_seed", &o.random_seed, false);
            ImGui::Checkbox(TR("Require Bounds on Variables"), &o.require_bounds);
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(TR("Evolutionary"), nullptr, w.sv_opt_tab == 2 && w.sv_opt_set ? ImGuiTabItemFlags_SetSelected : 0)) {
            num(TR("Convergence:"), "##o_conv2", &o.convergence, "%g");
            num(TR("Mutation Rate:"), "##o_mut", &o.mutation_rate, "%g");
            inum(TR("Population Size:"), "##o_pop2", &o.population, false);
            inum(TR("Random Seed:"), "##o_seed2", &o.random_seed, false);
            num(TR("Maximum Time without improvement:"), "##o_noimp", &o.max_time_no_improve, "%g");
            ImGui::Checkbox(TR("Require Bounds on Variables##evo"), &o.require_bounds);
            ImGui::EndTabItem();
          }
          ImGui::EndTabBar();
        }
        w.sv_opt_set = false;
        ImGui::Separator();
        if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
          o.precision = std::clamp(o.precision, 1e-12, 1.0);
          o.max_time = std::max(1.0, o.max_time);
          o.max_iterations = std::max(1, o.max_iterations);
          o.population = std::max(10, o.population);
          // the options only: the parameters' boxes stay as the user left them
          model::SolverModel& m = w.sv;
          m.precision = o.precision, m.auto_scaling = o.auto_scaling, m.show_iterations = o.show_iterations, m.ignore_integer = o.ignore_integer;
          m.integer_optimality = o.integer_optimality, m.max_time = o.max_time, m.max_iterations = o.max_iterations, m.max_subproblems = o.max_subproblems;
          m.max_feasible = o.max_feasible, m.convergence = o.convergence, m.derivatives = o.derivatives, m.multistart = o.multistart;
          m.population = o.population, m.random_seed = o.random_seed, m.require_bounds = o.require_bounds, m.mutation_rate = o.mutation_rate;
          m.max_time_no_improve = o.max_time_no_improve;
          w.sv_page = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sv_cancel) w.sv_page = 0;  // Esc goes back to the parameters, as Cancel
      } else if (page == 4) {
        // Load/Save Model: the cells to read a model from, or the first cell to write it to
        ImGui::PushTextWrapPos(S(400));
        ImGui::TextUnformatted(TR("Specify a cell range of the model's size to save (or its first cell), or the whole model range to load."));
        ImGui::PopTextWrapPos();
        RangeBox(s, "solver.loadsave", w.sv_ls, sizeof(w.sv_ls), S(380), 0, TR("Load/Save Model"));
        ImGui::Separator();
        std::vector<std::pair<uint32_t, Rect>> area;
        if (DlgOkButton(TR("Load"), ImVec2(S(80), 0)) && Refs(s, w.sv_ls, &area) && area.size() == 1) {
          read_model();
          model::SolverModel m = w.sv;
          std::string err;
          const bool ok = s.engine->Call([&](Document& d) { return d.LoadSolverModel(area[0].first, area[0].second, &m, &err); });
          if (!ok) {
            s.ShowMessage(TR("Solver"), err);
          } else {
            w.sv = m;
            Copy(w.sv_obj, sizeof(w.sv_obj), m.objective);
            Copy(w.sv_changing, sizeof(w.sv_changing), m.changing);
            std::snprintf(w.sv_value, sizeof(w.sv_value), "%g", m.value);
            w.sv_sel = -1;
            w.sv_page = 0;
          }
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Save"), ImVec2(S(80), 0)) && Refs(s, w.sv_ls, &area) && area.size() == 1) {
          read_model();
          const model::SolverModel m = w.sv;
          std::string err;
          s.engine->Call([&](Document& d) {
            d.Begin(TR("Save Model"));
            d.SaveSolverModel(area[0].first, m, area[0].second.r0, area[0].second.c0, &err);
            d.Commit();
            return 0;
          });
          if (!err.empty()) s.ShowMessage(TR("Solver"), err);
          else w.sv_page = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sv_cancel) w.sv_page = 0;  // Esc goes back to the parameters, as Cancel
      } else if (page == 5) {
        // Save Scenario: the variable cells' values under a name (Scenario Manager lists it)
        TextField(TR("Scenario Name:"), w.sv_scn, sizeof(w.sv_scn), 300);
        if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) && w.sv_scn[0]) {
          std::vector<std::pair<uint32_t, Rect>> refs;
          if (Refs(s, w.sv_changing, &refs)) {
            model::Scenario sc;
            sc.name = w.sv_scn;
            sc.cells = CellsOf(refs, 32);
            std::string err;
            const bool ok = s.engine->Call([&](Document& d) {
              for (const auto& p : sc.cells) sc.values.push_back(d.GetValue(sheet, p.row, p.col));
              d.Begin(TR("Add Scenario"));
              const bool r = d.AddScenario(sheet, sc, &err);
              d.Commit();
              return r;
            });
            if (!ok) s.ShowMessage(TR("Solver"), err);
            else w.sv_page = 1;
          }
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sv_cancel) w.sv_page = 1;
      } else {
        // Solver Results: the message, Keep / Restore, the reports, Return to Solver Parameters Dialog
        ImGui::PushTextWrapPos(S(400));
        ImGui::TextUnformatted(w.sv_result.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::BeginGroup();
        ImGui::RadioButton(TR("Keep Solver Solution"), w.sv_keep);
        if (ImGui::IsItemClicked()) w.sv_keep = true;
        ImGui::RadioButton(TR("Restore Original Values"), !w.sv_keep);
        if (ImGui::IsItemClicked()) w.sv_keep = false;
        ImGui::Dummy(ImVec2(1, S(8)));
        ImGui::Checkbox(TR("Return to Solver Parameters Dialog"), &w.sv_return);
        ImGui::EndGroup();
        ImGui::SameLine(S(250));
        ImGui::BeginGroup();
        ImGui::TextUnformatted(TR("Reports"));
        const bool can_report = w.sv_result.status != 5 && w.sv_result.status != 13 && w.sv_result.status != 7;
        ImGui::BeginDisabled(!can_report);
        if (ImGui::BeginChild("##svreports", ImVec2(S(150), S(70)), ImGuiChildFlags_Borders)) {
          const char* kReports[] = {TR("Answer"), TR("Sensitivity"), TR("Limits")};
          for (int k = 0; k < 3; ++k) {
            // a click picks one; Ctrl+click several, as Excel's list
            if (ImGui::Selectable(kReports[k], w.sv_reports[k])) {
              if (!ImGui::GetIO().KeyCtrl) w.sv_reports[0] = w.sv_reports[1] = w.sv_reports[2] = false, w.sv_reports[k] = true;
              else w.sv_reports[k] = !w.sv_reports[k];
            }
          }
        }
        ImGui::EndChild();
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::Separator();
        auto finish = [&](bool keep) {
          if (!keep && w.sv_changed) s.engine->Call([](Document& d) { return d.Undo(); });
          // the reports: new sheets before the model's sheet, made from the kept solution
          if (keep && (w.sv_reports[0] || w.sv_reports[1] || w.sv_reports[2])) {
            read_model();
            const model::SolverModel m = w.sv;
            const Document::SolverResult res = w.sv_result;
            const bool a = w.sv_reports[0], b = w.sv_reports[1], c = w.sv_reports[2];
            std::string err;
            s.engine->Call([&](Document& d) {
              d.Begin(TR("Solver Reports"));
              d.SolverReports(sheet, m, res, a, b, c, &err);
              d.Commit();
              return 0;
            });
            if (!err.empty()) s.ShowMessage(TR("Solver"), err);
          }
          w.sv_changed = false;
          if (w.sv_return) w.sv_page = 0;
          else close = true;
        };
        if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) finish(w.sv_keep);
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || sv_cancel) finish(false);  // Excel's Cancel (and Esc) restores the original values
        ImGui::SameLine(S(250));
        if (ImGui::Button(TR("Save Scenario..."), ImVec2(S(150), 0))) {
          w.sv_scn[0] = 0;
          w.sv_page = 5;
        }
      }
      EndDlg(s, close);
    } else if (sv_cancel && sv_results && w.sv_changed) {
      s.engine->Call([](Document& d) { return d.Undo(); });  // Solver Results closed by its box: the original values
      w.sv_changed = false;
    }
  }
}

}  // namespace yc::app
