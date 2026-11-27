// SPDX-License-Identifier: Apache-2.0
// names_ui.cpp - UI for defined names (named ranges), like Excel:
//   Name Box (typing a new name defines it, the drop-down jumps to a name, shows the name when the selection is exactly a name)
//   Name Manager, Define Name, Create from Selection, Use in Formula / Paste Name (F3)
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/snapshot.h"
#include "formula/parser.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "yc_ui_items.h"
#include "model/table.h"
#include "ribbon/ribbon.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

// the cell a relative name is shown and typed against (Excel's Name Manager and Name Box use the active cell)
core::CellPos ActiveAt(AppState& s) { return core::CellPos{s.FocusView().sel.act_r, s.FocusView().sel.act_c}; }

struct NamesUi {
  // define / edit name
  bool editing = false, from_manager = false;
  std::string old_name;
  int32_t old_scope = -1;
  char name[256] = {}, refers[1024] = {}, comment[512] = {};
  int scope_index = 0;  // 0 workbook, 1.. sheets
  std::string error;
  // Name Manager
  std::string sel_name;
  int32_t sel_scope = -1;
  char filter[128] = {};
  int flt_scope = 0;  // Name Manager > Filter: 0 every name, 1 scoped to a worksheet, 2 to the workbook
  int flt_err = 0;    // 0 every name, 1 with errors, 2 without errors
  int flt_kind = 0;   // 0 every name, 1 defined names, 2 table names
  bool sel_table = false;  // the row selected is a table (renamed only)
  bool table_edit = false;  // Edit Name for a table: only its name and comment change
  uint32_t table_sheet = 0, table_id = 0;
  bool confirm_delete = false;
  char mgr_refers[1024] = {};  // the selected name's Refers to, being edited in the Name Manager
  std::string mgr_for;         // which name mgr_refers belongs to
  // Create from Selection
  bool top = true, left = false, bottom = false, right = false;
  // Paste Name
  int paste_sel = 0;
  // Apply Names: the names chosen, Ignore Relative/Absolute
  std::vector<std::string> apply_sel;
  bool apply_ignore_rel = true, apply_row_col = true, apply_options = false, apply_omit_col = true, apply_omit_row = true;
  int apply_order = 0;  // 0 Row Column, 1 Column Row
};

NamesUi& N() {
  static NamesUi n;
  return n;
}

std::string Lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string SelectionRef(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return {};
  std::string out;
  const std::string q = core::QuoteSheetName(sh->name) + "!";
  for (const Rect& r : SelAreas(s)) {
    std::string one;
    if (r.r0 == 0 && r.r1 >= sh->row_limit - 1) one = q + "$" + core::ColumnName(r.c0) + ":$" + core::ColumnName(r.c1);  // entire column
    else one = q + core::CellA1(r.r0, r.c0, true, true) + (r.IsCell() ? "" : ":" + core::CellA1(r.r1, r.c1, true, true));
    out += (out.empty() ? "" : ",") + one;
  }
  return out;
}

std::string ScopeName(const AppState& s, int32_t scope) {
  if (scope < 0) return TR("Workbook");
  const model::Sheet* sh = s.snap->Sheet(static_cast<uint32_t>(scope));
  return sh ? sh->name : TR("(deleted)");
}

// a name's refers-to text as Excel shows it: relative references relative to the active cell (they are kept relative to A1)
std::string ShownRefers(AppState& s, const std::string& refers_to) {
  engine::WorkbookResolver res(s.snap->wb);
  return engine::ShownNameUnion(formula::MoveRelativeText(refers_to, core::CellPos{0, 0}, ActiveAt(s), &res), &res);  // a union without its ((..))
}

// refers-to text typed while a cell is active, as it is kept
std::string KeptRefers(AppState& s, const std::string& typed) {
  engine::WorkbookResolver res(s.snap->wb);
  return formula::MoveRelativeText(typed, ActiveAt(s), core::CellPos{0, 0}, &res);
}

// current value of a name (for a range, the first few cell values, like the Value column of Excel's Name Manager)
std::string NameValue(AppState& s, const model::NameDef& n) {
  uint32_t sid = 0;
  Rect r;
  if (ResolveRangeText(s.snap->wb, n.sheet_scope >= 0 ? static_cast<uint32_t>(n.sheet_scope) : s.focus_sheet, ShownRefers(s, n.refers_to), &sid, &r)) {
    const model::Sheet* sh = s.snap->Sheet(sid);
    if (!sh) return "#REF!";
    const auto& tbl = *s.snap->wb.styles;
    std::string out = "{";
    int k = 0;
    for (uint32_t row = r.r0; row <= r.r1 && k < 6; ++row) {
      for (uint32_t col = r.c0; col <= r.c1 && k < 6; ++col, ++k) {
        out += (k ? ";" : "") + std::string("\"") + engine::DisplayTextOf(sh->GetValue(row, col), tbl.NumberFormat(tbl.Get(sh->GetStyle(row, col)))) + "\"";
      }
    }
    if (r.Count() > 6) out += ";…";
    return r.IsCell() ? out.substr(2, out.size() - 3) : out + "}";
  }
  // a constant or a formula: its value, worked out once per change of the workbook (Excel shows it too)
  static std::map<std::string, std::pair<uint64_t, std::string>> cache;
  const std::string key = std::to_string(n.sheet_scope) + "|" + n.refers_to;
  auto it = cache.find(key);
  if (it != cache.end() && it->second.first == s.snap->version) return it->second.second;
  const uint32_t sheet = n.sheet_scope >= 0 ? static_cast<uint32_t>(n.sheet_scope) : s.focus_sheet;
  const std::string text = n.refers_to;
  const std::string shown = s.engine->Call([&](Document& d) {
    std::string err;
    const core::Value v = d.EvaluateFormula(sheet, text, &err);
    return err.empty() ? engine::DisplayTextOf(v, "General") : std::string("#NAME?");
  });
  if (cache.size() > 500) cache.clear();
  cache[key] = {s.snap->version, shown};
  return shown;
}

void OpenDefine(AppState& s, const model::NameDef* existing) {
  NamesUi& n = N();
  n.error.clear();
  n.from_manager = s.dialog == Dialog::NameManager;
  n.editing = existing != nullptr;
  n.table_edit = false;
  if (existing) {
    n.old_name = existing->name;
    n.old_scope = existing->sheet_scope;
    std::snprintf(n.name, sizeof(n.name), "%s", existing->name.c_str());
    std::snprintf(n.refers, sizeof(n.refers), "=%s", ShownRefers(s, existing->refers_to).c_str());
    std::snprintf(n.comment, sizeof(n.comment), "%s", existing->comment.c_str());
    n.scope_index = 0;
    if (existing->sheet_scope >= 0) n.scope_index = s.snap->wb.IndexOfId(static_cast<uint32_t>(existing->sheet_scope)) + 1;
  } else {
    // default name: the text left of or above the active cell (like Excel)
    n.name[0] = 0;
    const model::Sheet* sh = s.FocusSheet();
    const Rect sel = SelRect(s);
    if (sh) {
      const auto& tbl = *s.snap->wb.styles;
      auto text = [&](uint32_t r, uint32_t c) { return engine::DisplayTextOf(sh->GetValue(r, c), tbl.NumberFormat(tbl.Get(sh->GetStyle(r, c)))); };
      std::string guess;
      if (sel.r0 > 0 && sh->GetValue(sel.r0 - 1, sel.c0).IsString()) guess = text(sel.r0 - 1, sel.c0);
      else if (sel.c0 > 0 && sh->GetValue(sel.r0, sel.c0 - 1).IsString()) guess = text(sel.r0, sel.c0 - 1);
      else if (sh->GetValue(sel.r0, sel.c0).IsString() && !sel.IsCell()) guess = text(sel.r0, sel.c0);
      std::string err;
      if (!guess.empty() && engine::ValidateName(guess, &err)) std::snprintf(n.name, sizeof(n.name), "%s", guess.c_str());
    }
    std::snprintf(n.refers, sizeof(n.refers), "=%s", SelectionRef(s).c_str());
    n.comment[0] = 0;
    n.scope_index = 0;
  }
  s.OpenDialog(Dialog::DefineName);
}

// Name Manager > Edit... on a table: the Edit Name dialog with the table's name (its range and scope fixed)
void OpenTableEdit(AppState& s, uint32_t sheet, const model::Table& t) {
  NamesUi& n = N();
  n.error.clear();
  n.from_manager = s.dialog == Dialog::NameManager;
  n.editing = true;
  n.table_edit = true;
  n.table_sheet = sheet;
  n.table_id = t.id;
  n.old_name = t.name;
  std::snprintf(n.name, sizeof(n.name), "%s", t.name.c_str());
  const model::Sheet* sh = s.snap->Sheet(sheet);
  const Rect d = t.HasData() ? t.Data() : t.range;
  std::snprintf(n.refers, sizeof(n.refers), "=%s!%s:%s", sh ? core::QuoteSheetName(sh->name).c_str() : "", core::CellA1(d.r0, d.c0, true, true).c_str(),
                core::CellA1(d.r1, d.c1, true, true).c_str());
  n.comment[0] = 0;
  n.scope_index = 0;
  s.OpenDialog(Dialog::DefineName);
}

void InsertIntoFormula(AppState& s, const std::string& text) {
  if (s.edit.active) {
    std::string t = s.edit.text;
    const int caret = std::clamp(s.edit.caret, 0, static_cast<int>(t.size()));
    t.insert(static_cast<size_t>(caret), text);
    render::SetEditText(s.edit, t, caret + static_cast<int>(text.size()));
    s.edit.focus_request = true;
  } else {
    StartEdit(s, true, "=" + text);
  }
}

bool BeginDlg(AppState& s, Dialog d, const char* title, float width, float height) {
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(width), height > 0 ? S(height) : 0.0f), ImGuiCond_Appearing);  // height 0 = fit the contents
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) return false;
  if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
    ImGui::EndPopup();
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

}  // namespace

// ---- Name Box ----

std::string NameForSelection(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || s.snap->wb.names->empty()) return {};
  const auto areas = SelAreas(s);
  if (areas.size() != 1) return {};
  Rect sel = areas[0];
  if (sel.r1 >= sh->row_limit - 1) sel.r1 = core::kMaxRows - 1;
  for (const auto& n : *s.snap->wb.names) {
    if (n.hidden || (n.sheet_scope >= 0 && static_cast<uint32_t>(n.sheet_scope) != sh->id)) continue;
    uint32_t sid = 0;
    Rect r;
    if (!ResolveRangeText(s.snap->wb, sh->id, n.refers_to, &sid, &r) || sid != sh->id) continue;
    Rect cmp = areas[0];
    if (r.r1 >= sh->used.r1 && cmp.r1 >= sh->row_limit - 1) cmp.r1 = r.r1;  // entire column
    if (r == cmp) return n.name;
  }
  return {};
}

void NameBoxEnter(AppState& s, const std::string& text_in) {
  std::string text = text_in;
  while (!text.empty() && text.back() == ' ') text.pop_back();
  while (!text.empty() && text.front() == ' ') text.erase(text.begin());
  if (text.empty()) return;
  // 1) an address or an existing name: go there
  Rect r;
  std::string sname;
  std::string_view addr;
  const bool has_sheet = core::SplitSheetRef(text, &sname, &addr) && !sname.empty();
  std::string clean(has_sheet ? addr : std::string_view(text));
  clean.erase(std::remove(clean.begin(), clean.end(), '$'), clean.end());
  const bool is_ref = core::ParseRectA1(clean, &r);
  uint32_t sid = 0;
  Rect nr;
  const uint32_t focus = s.focus_sheet;
  const core::CellPos at = ActiveAt(s);
  const bool is_name = !is_ref && s.engine->Call([&](Document& d) { return d.ResolveNameRef(text, focus, &sid, &nr, nullptr, at); });
  if (is_ref || is_name || text.find(',') != std::string::npos) {
    Goto(s, text);
    return;
  }
  // 2) a new valid name: define it for the current selection (like Excel); read-only mode keeps the names as they are
  if (RefuseReadOnlyUi(s)) return;
  std::string err;
  if (!engine::ValidateName(text, &err)) {
    s.ShowError("YCGrid", TR("Invalid name: ") + text + "\n\n" + err);
    return;
  }
  const std::string ref = SelectionRef(s);
  const bool ok = s.engine->Call([&](Document& d) {
    d.Begin(TR("Define Name"));
    const bool k = d.DefineName(model::NameDef{text, ref, -1}, &err);
    d.Commit();
    return k;
  });
  if (ok) s.SetStatus(TR("Defined name \"") + text + TR("\" =") + ref);
  else if (!err.empty()) s.ShowError("YCGrid", err);
}

// drop-down list to the right of the Name Box
void DrawNameBoxList(AppState& s) {
  std::vector<const model::NameDef*> list;
  for (const auto& n : *s.snap->wb.names) {
    if (n.hidden) continue;
    if (n.sheet_scope >= 0 && static_cast<uint32_t>(n.sheet_scope) != s.focus_sheet) continue;
    // Excel lists only names that refer to cells (there is nowhere to go for Rate = 0.05)
    uint32_t sid = 0;
    Rect r{};
    if (!ResolveRangeText(s.snap->wb, n.sheet_scope >= 0 ? static_cast<uint32_t>(n.sheet_scope) : s.focus_sheet, n.refers_to, &sid, &r)) continue;
    list.push_back(&n);
  }
  std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return Lower(a->name) < Lower(b->name); });
  // tables are listed too (Goto selects the data body)
  std::vector<std::pair<std::string, std::string>> tables;  // name, where Goto goes
  for (const auto& sh : s.snap->wb.sheets) {
    for (const auto& t : *sh->tables) {
      const Rect d = t.Data().r0 <= t.Data().r1 ? t.Data() : t.range;
      tables.emplace_back(t.name, core::QuoteSheetName(sh->name) + "!" + core::CellA1(d.r0, d.c0) + ":" + core::CellA1(d.r1, d.c1));
    }
  }
  // one alphabetical list of names and tables, as Excel's
  for (const auto* n : list) tables.emplace_back(n->name, n->sheet_scope >= 0 ? core::QuoteSheetName(ScopeName(s, n->sheet_scope)) + "!" + n->name : n->name);
  std::stable_sort(tables.begin(), tables.end(), [](const auto& a, const auto& b) { return Lower(a.first) < Lower(b.first); });
  if (tables.empty()) ImGui::TextDisabled(TR("(no defined names)"));
  for (size_t i = 0; i < tables.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(tables[i].first.c_str())) Goto(s, tables[i].second);
    ImGui::PopID();
  }
}

// ---- Commands ----

namespace {
constexpr int kUseNameMenu = 40;  // Use in Formula lists up to this many names

// the names Use in Formula offers: the workbook's and the active sheet's, not hidden, sorted
std::vector<std::string> NamesInScope(AppState& s) {
  std::vector<std::string> out;
  if (!s.snap) return out;
  for (const auto& x : *s.snap->wb.names) {
    if (!x.hidden && (x.sheet_scope < 0 || static_cast<uint32_t>(x.sheet_scope) == s.focus_sheet)) out.push_back(x.name);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
}  // namespace

void SyncUseInFormulaMenu(AppState& s) {
  const std::vector<std::string> names = NamesInScope(s);
  std::vector<std::string> menu;
  for (size_t i = 0; i < names.size() && i < static_cast<size_t>(kUseNameMenu); ++i) {
    if (auto* c = ui::Commands().Find("UseName" + std::to_string(i))) c->label = names[i];
    menu.push_back("UseName" + std::to_string(i));
  }
  if (!menu.empty()) menu.push_back("-");
  menu.push_back("PasteName");
  static std::vector<std::string> last_names;
  static bool built = false;
  if (built && names == last_names) return;
  built = true;
  last_names = names;
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "UseInFormula") it.menu = menu;
        }
      }
    }
  }
}

void RegisterNameCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* ic = nullptr, const char* shortcut = "", const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("NameManager", TR("Name\nManager"), ui::icon::NameManager, "Ctrl+F3", TR("Create, edit, delete and find all the names used in the workbook."))).execute = [&s](ui::Command&) {
    N().sel_name.clear();
    s.OpenDialog(Dialog::NameManager);
  };
  R.Add(make("DefineName", TR("Define Name"), ui::icon::Add, "", TR("Define a name for the selection so formulas can use the name instead of the address."))).execute = [&s](ui::Command&) { OpenDefine(s, nullptr); };
  R.Add(make("ApplyNames", TR("Apply Names..."), nullptr, "", TR("Replace the references in the selected formulas with the names that refer to them."))).execute =
      [&s](ui::Command&) {
        if (!s.FocusSheet()) return;
        NamesUi& n = N();
        n.apply_sel.clear();
        n.apply_ignore_rel = n.apply_row_col = n.apply_omit_col = n.apply_omit_row = true;
        n.apply_options = false;
        n.apply_order = 0;
        s.OpenDialog(Dialog::ApplyNames);
      };
  R.Add(make("CreateNamesFromSelection", TR("Create from Selection"), ui::icon::Table, "Ctrl+Shift+F3", TR("Automatically create names from the text in the top row, left column, etc. of the selection."))).execute = [&s](ui::Command&) {
    NamesUi& n = N();
    const model::Sheet* sh = s.FocusSheet();
    const Rect r = SelRect(s);
    n.top = n.left = n.bottom = n.right = false;
    if (sh) {
      // Excel's defaults: tick Top row when the top row is text, tick Left column when the left column is text
      n.top = sh->GetValue(r.r0, std::min(r.c1, r.c0 + 1)).IsString();
      n.left = r.Cols() > 1 && sh->GetValue(std::min(r.r1, r.r0 + 1), r.c0).IsString();
      if (!n.top && !n.left) n.top = true;
    }
    s.OpenDialog(Dialog::CreateNames);
  };
  // Formulas > Use in Formula: a drop-down of the names (SyncUseInFormulaMenu) and Paste Names... at its end
  R.Add(make("UseInFormula", TR("Use in Formula"), ui::icon::UseInFormula, "", TR("Choose a name used in this workbook and insert it into the current formula."))).execute =
      [](ui::Command&) {};
  for (int i = 0; i < kUseNameMenu; ++i) {
    R.Add(make(("UseName" + std::to_string(i)).c_str(), "")).execute = [&s, i](ui::Command&) {
      const std::vector<std::string> names = NamesInScope(s);
      if (static_cast<size_t>(i) < names.size()) InsertIntoFormula(s, names[static_cast<size_t>(i)]);
    };
  }
  R.Add(make("PasteName", TR("Paste Names..."), ui::icon::Formula, "F3", TR("Insert a defined name into a formula."))).execute = [&s](ui::Command&) {
    N().paste_sel = 0;
    // Excel: F3 with no names to paste does nothing
    bool any = false;
    for (const auto& n : *s.snap->wb.names) {
      any = any || (!n.hidden && (n.sheet_scope < 0 || static_cast<uint32_t>(n.sheet_scope) == s.focus_sheet));
    }
    if (!any) {
      s.SetStatus(TR("There are no names to paste"));
      return;
    }
    s.OpenDialog(Dialog::PasteName);
  };
}

// ---- Dialogs ----

void DrawNameDialogs(AppState& s) {
  NamesUi& n = N();
  // Name Manager
  if (BeginDlg(s, Dialog::NameManager, TR("Name Manager###yc_dlg_names"), 700, 0)) {
    if (ImGui::Button(TR("New..."))) OpenDefine(s, nullptr);
    ImGui::SameLine();
    const model::NameDef* sel = nullptr;
    for (const auto& x : *s.snap->wb.names) {
      if (!n.sel_table && x.name == n.sel_name && x.sheet_scope == n.sel_scope) sel = &x;
    }
    // a table selected: Edit renames it; a table can't be deleted here (Excel)
    uint32_t sel_tsheet = 0;
    const model::Table* sel_tbl = n.sel_table ? model::FindTable(s.snap->wb, n.sel_name, &sel_tsheet) : nullptr;
    ImGui::BeginDisabled(!sel && !sel_tbl);
    if (ImGui::Button(TR("Edit..."))) {
      if (sel) OpenDefine(s, sel);
      else if (sel_tbl) OpenTableEdit(s, sel_tsheet, *sel_tbl);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(TR("Delete")) && sel) n.confirm_delete = true;  // Excel asks first
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(200));
    ImGui::InputTextWithHint("##flt", TR("Filter names"), n.filter, sizeof(n.filter));
    // Excel's Filter menu: by scope and by whether the name's value is an error
    ImGui::SameLine();
    if (ImGui::Button((std::string(TR("Filter")) + (n.flt_scope || n.flt_err || n.flt_kind ? " *" : "") + "##nmflt").c_str())) ImGui::OpenPopup("##nm_filter");
    if (ImGui::BeginPopup("##nm_filter")) {
      if (ImGui::MenuItem(TR("Clear Filter"), nullptr, false, n.flt_scope || n.flt_err || n.flt_kind)) n.flt_scope = n.flt_err = n.flt_kind = 0;
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Names Scoped to Worksheet"), nullptr, n.flt_scope == 1)) n.flt_scope = n.flt_scope == 1 ? 0 : 1;
      if (ImGui::MenuItem(TR("Names Scoped to Workbook"), nullptr, n.flt_scope == 2)) n.flt_scope = n.flt_scope == 2 ? 0 : 2;
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Names with Errors"), nullptr, n.flt_err == 1)) n.flt_err = n.flt_err == 1 ? 0 : 1;
      if (ImGui::MenuItem(TR("Names without Errors"), nullptr, n.flt_err == 2)) n.flt_err = n.flt_err == 2 ? 0 : 2;
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Defined Names"), nullptr, n.flt_kind == 1)) n.flt_kind = n.flt_kind == 1 ? 0 : 1;
      if (ImGui::MenuItem(TR("Table Names"), nullptr, n.flt_kind == 2)) n.flt_kind = n.flt_kind == 2 ? 0 : 2;
      ImGui::EndPopup();
    }
    if (ImGui::BeginTable("##names", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(-1, S(290)))) {
      ImGui::TableSetupColumn(TR("Name"), ImGuiTableColumnFlags_WidthFixed, S(140));
      ImGui::TableSetupColumn(TR("Values"), ImGuiTableColumnFlags_WidthFixed, S(150));
      ImGui::TableSetupColumn(TR("Refers To"));
      ImGui::TableSetupColumn(TR("Scope"), ImGuiTableColumnFlags_WidthFixed, S(80));
      ImGui::TableSetupColumn(TR("Comment"), ImGuiTableColumnFlags_WidthFixed, S(120));
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();
      std::vector<const model::NameDef*> list;
      for (const auto& x : *s.snap->wb.names) {
        if (n.flt_kind == 2) break;  // Table Names only
        if (n.filter[0] && Lower(x.name).find(Lower(n.filter)) == std::string::npos) continue;
        if ((n.flt_scope == 1 && x.sheet_scope < 0) || (n.flt_scope == 2 && x.sheet_scope >= 0)) continue;
        if (n.flt_err) {
          const std::string v = NameValue(s, x);
          const bool err = !v.empty() && v[0] == '#';
          if ((n.flt_err == 1) != err) continue;
        }
        list.push_back(&x);
      }
      std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return Lower(a->name) < Lower(b->name); });
      // the tables, as Excel lists them with the names (workbook scope, their data rows)
      struct TableRow {
        const model::Table* t;
        const model::Sheet* sh;
      };
      std::vector<TableRow> tables;
      if (n.flt_kind != 1 && n.flt_scope != 1 && n.flt_err != 1) {
        for (const auto& sc : s.snap->wb.sheets) {
          for (const model::Table& t : *sc->tables) {
            if (n.filter[0] && Lower(t.name).find(Lower(n.filter)) == std::string::npos) continue;
            tables.push_back({&t, &*sc});
          }
        }
      }
      std::sort(tables.begin(), tables.end(), [](const TableRow& a, const TableRow& b) { return Lower(a.t->name) < Lower(b.t->name); });
      for (const TableRow& tr : tables) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(tr.t);
        const bool selected = n.sel_table && tr.t->name == n.sel_name;
        if (ImGui::Selectable(tr.t->name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
          n.sel_name = tr.t->name;
          n.sel_table = true;
          n.confirm_delete = false;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenTableEdit(s, tr.sh->id, *tr.t);
        }
        ImGui::PopID();
        const Rect d = tr.t->HasData() ? tr.t->Data() : tr.t->range;
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("{...}");
        ImGui::TableNextColumn();
        const std::string refers = "=" + core::QuoteSheetName(tr.sh->name) + "!" + core::CellA1(d.r0, d.c0, true, true) + ":" + core::CellA1(d.r1, d.c1, true, true);
        ImGui::TextUnformatted(refers.c_str());
        ui::NoteUiText(refers.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(TR("Workbook"));
        ImGui::TableNextColumn();
      }
      for (const auto* x : list) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(x);
        const bool selected = !n.sel_table && x->name == n.sel_name && x->sheet_scope == n.sel_scope;
        if (ImGui::Selectable((x->name + (x->hidden ? TR("(hidden)") : "")).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
          n.sel_name = x->name;
          n.sel_scope = x->sheet_scope;
          n.sel_table = false;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenDefine(s, x);
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(NameValue(s, *x).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(("=" + ShownRefers(s, x->refers_to)).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(ScopeName(s, x->sheet_scope).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(x->comment.c_str());
      }
      ImGui::EndTable();
    }
    if (sel && n.confirm_delete) {
      ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.0f, 1), "%s", (TR("Are you sure you want to delete the name ") + sel->name + "?").c_str());
      if (ImGui::Button(TR("Yes"), ImVec2(S(80), 0))) {
        const std::string nm = sel->name;
        const int32_t sc = sel->sheet_scope;
        s.engine->PostUser(TR("Delete Name"), [nm, sc](Document& d) { d.DeleteName(nm, sc); });
        n.sel_name.clear();
        n.confirm_delete = false;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("No"), ImVec2(S(80), 0))) n.confirm_delete = false;
    } else if (sel) {
      // Refers to: edited here, cells picked with its button; the check mark keeps it, the cross puts it back
      const std::string owner = sel->name + "|" + std::to_string(sel->sheet_scope);
      if (n.mgr_for != owner) {
        n.mgr_for = owner;
        std::snprintf(n.mgr_refers, sizeof(n.mgr_refers), "=%s", ShownRefers(s, sel->refers_to).c_str());
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Refers to:"));
      ImGui::SameLine();
      const float bw = ImGui::GetFrameHeight();
      RangeBox(s, "names.mgr.refers", n.mgr_refers, sizeof(n.mgr_refers), ImGui::GetContentRegionAvail().x - bw * 2 - S(12),
               RangeBox_Formula | RangeBox_Equals | RangeBox_Sheet, TR("Refers to:"));
      const bool dirty = std::string(n.mgr_refers) != "=" + ShownRefers(s, sel->refers_to);
      ImGui::BeginDisabled(!dirty);
      ImGui::SameLine();
      ImGui::PushFont(ui::F().icons, ImGui::GetFontSize());
      const bool keep = ImGui::Button((std::string(ui::icon::Check) + "##keep").c_str(), ImVec2(bw, 0));
      ImGui::PopFont();
      if (keep) {
        model::NameDef def = *sel;
        std::string r = n.mgr_refers;
        while (!r.empty() && (r[0] == '=' || r[0] == ' ')) r.erase(0, 1);
        def.refers_to = KeptRefers(s, r);
        std::string err;
        const std::string old = sel->name;
        const int32_t sc = sel->sheet_scope;
        const bool ok = s.engine->Call([&](Document& d) {
          d.Begin(TR("Edit Name"));
          const bool k = d.UpdateName(old, sc, def, &err);
          d.Commit();
          return k;
        });
        if (!ok && !err.empty()) s.ShowError("YCGrid", err);
        n.mgr_for.clear();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Keep the change"));
      ImGui::SameLine();
      ImGui::PushFont(ui::F().icons, ImGui::GetFontSize());
      if (ImGui::Button((std::string(ui::icon::Dismiss) + "##undo").c_str(), ImVec2(bw, 0))) n.mgr_for.clear();
      ImGui::PopFont();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Cancel the change"));
      ImGui::EndDisabled();
    }
    const bool close = ImGui::Button(TR("Close"), ImVec2(S(80), 0));
    EndDlg(s, close);
  }
  // define / edit name
  if (BeginDlg(s, Dialog::DefineName, n.editing ? TR("Edit Name###yc_dlg_defname") : TR("New Name###yc_dlg_defname"), 480, 0)) {
    ImGui::TextUnformatted(TR("Name"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##nm", n.name, sizeof(n.name));
    ImGui::TextUnformatted(TR("Scope"));
    std::vector<std::string> scopes{TR("Workbook")};
    for (const auto& sh : s.snap->wb.sheets) scopes.push_back(sh->name);
    n.scope_index = std::clamp(n.scope_index, 0, static_cast<int>(scopes.size()) - 1);
    ImGui::SetNextItemWidth(S(200));
    ImGui::BeginDisabled(n.editing || n.table_edit);  // Excel: a name's scope is chosen when it is made
    const bool scope_open = ui::BeginCombo("##scope", scopes[static_cast<size_t>(n.scope_index)].c_str());
    ImGui::EndDisabled();
    if (scope_open) {
      for (size_t i = 0; i < scopes.size(); ++i) {
        if (ImGui::Selectable(scopes[i].c_str(), static_cast<int>(i) == n.scope_index)) n.scope_index = static_cast<int>(i);
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted(TR("Comment"));
    ImGui::InputTextMultiline("##cm", n.comment, sizeof(n.comment), ImVec2(-1, S(50)));
    const bool comment_active = ImGui::IsItemActive();
    ImGui::TextUnformatted(TR("Refers to:"));
    // select the cells on the sheet with the box's button, or type a constant or a formula (=OFFSET(...))
    ImGui::BeginDisabled(n.table_edit);  // a table's range is changed with Resize Table
    RangeBox(s, "name.refers", n.refers, sizeof(n.refers), 0, RangeBox_Formula | RangeBox_Equals | RangeBox_Sheet, TR("Refers to:"));
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", TR("Select the cells with the button, or type a value or a formula."));
    if (!n.error.empty()) ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", n.error.c_str());
    bool close = false;
    const bool enter = (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) && !comment_active;
    const bool ok_pressed = ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || enter;
    if (ok_pressed && n.table_edit) {
      // a table: renamed (formulas follow)
      std::string err;
      const uint32_t ts = n.table_sheet, tid = n.table_id;
      const std::string nm = n.name;
      const bool ok = s.engine->Call([&](Document& d) {
        d.Begin(TR("Rename Table"));
        const bool k = d.RenameTable(ts, tid, nm, &err);
        d.Commit();
        return k;
      });
      if (ok) {
        n.sel_name = nm;
        n.sel_table = true;
        close = true;
      } else {
        n.error = err;
      }
    } else if (ok_pressed) {
      model::NameDef def;
      def.name = n.name;
      def.refers_to = KeptRefers(s, n.refers);
      def.comment = n.comment;
      def.sheet_scope = n.scope_index == 0 ? -1 : static_cast<int32_t>(s.snap->wb.sheets[static_cast<size_t>(n.scope_index - 1)]->id);
      std::string err;
      const bool editing = n.editing;
      const std::string old_name = n.old_name;
      const int32_t old_scope = n.old_scope;
      // a new name must be unique within its scope
      bool dup = false;
      if (!editing) {
        for (const auto& x : *s.snap->wb.names) dup = dup || (x.sheet_scope == def.sheet_scope && Lower(x.name) == Lower(def.name));
      }
      if (dup) {
        n.error = TR("This scope already has a name \"") + def.name + TR("\".");
      } else {
        const bool ok = s.engine->Call([&](Document& d) {
          d.Begin(editing ? TR("Edit Name") : TR("Define Name"));
          const bool k = editing ? d.UpdateName(old_name, old_scope, def, &err) : d.DefineName(def, &err);
          d.Commit();
          return k;
        });
        if (ok) {
          n.sel_name = def.name;
          n.sel_scope = def.sheet_scope;
          close = true;
        } else {
          n.error = err;
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
    // opened from the Name Manager: go back to the Name Manager
    if (close && s.dialog == Dialog::None && n.from_manager) s.OpenDialog(Dialog::NameManager);
  }
  // Create from Selection
  if (BeginDlg(s, Dialog::CreateNames, TR("Create Names from Selection###yc_dlg_createnames"), 300, 0)) {
    ImGui::TextUnformatted(TR("Create names from values in the: "));
    ImGui::Checkbox(TR("Top row"), &n.top);
    ImGui::Checkbox(TR("Left column"), &n.left);
    ImGui::Checkbox(TR("Bottom row"), &n.bottom);
    ImGui::Checkbox(TR("Right column"), &n.right);
    bool close = false;
    if (ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      const uint32_t sheet = s.focus_sheet;
      const Rect r = SelRect(s);
      const bool t = n.top, l = n.left, b = n.bottom, rt = n.right;
      size_t created = 0;
      std::string errs;
      s.engine->Call([&](Document& d) {
        d.Begin(TR("Create Names from Selection"));
        for (auto& def : d.NamesFromSelection(sheet, r, t, l, b, rt)) {
          std::string err;
          if (d.DefineName(def, &err)) ++created;
          else errs += def.name + TR(": ") + err + "\n";
        }
        d.Commit();
      });
      s.SetStatus(TR("Created ") + std::to_string(created) + TR(" names"));
      if (!errs.empty()) s.ShowError("YCGrid", TR("Some names couldn't be created:\n") + errs);
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Apply Names: the names whose references the selected formulas use become those names
  if (BeginDlg(s, Dialog::ApplyNames, TR("Apply Names###yc_dlg_applynames"), 340, 0)) {
    ImGui::TextUnformatted(TR("Apply names:"));
    std::vector<std::string> names;
    for (const auto& x : *s.snap->wb.names) {
      if (!x.hidden && (x.sheet_scope < 0 || static_cast<uint32_t>(x.sheet_scope) == s.focus_sheet)) names.push_back(x.name);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    ImGui::BeginChild("##an", ImVec2(-1, S(n.apply_options ? 110.0f : 180.0f)), ImGuiChildFlags_Borders);
    for (const std::string& nm : names) {
      const auto it = std::find(n.apply_sel.begin(), n.apply_sel.end(), nm);
      if (ImGui::Selectable(nm.c_str(), it != n.apply_sel.end())) {
        if (it != n.apply_sel.end()) n.apply_sel.erase(it);
        else n.apply_sel.push_back(nm);
      }
    }
    if (names.empty()) ImGui::TextDisabled("%s", TR("(no defined names)"));
    ImGui::EndChild();
    ImGui::Checkbox(TR("Ignore Relative/Absolute"), &n.apply_ignore_rel);
    ImGui::Checkbox(TR("Use row and column names"), &n.apply_row_col);
    // Options >>: what the row / column names leave out, and their order (Excel's dialog grows the same way)
    if (!n.apply_options && ImGui::Button(TR("Options >>"))) n.apply_options = true;
    if (n.apply_options) {
      ImGui::BeginDisabled(!n.apply_row_col);
      ImGui::Checkbox(TR("Omit column name if same column"), &n.apply_omit_col);
      ImGui::Checkbox(TR("Omit row name if same row"), &n.apply_omit_row);
      ImGui::TextUnformatted(TR("Name order"));
      ImGui::SameLine();
      ui::RadioButton(TR("Row Column"), &n.apply_order, 0);
      ImGui::SameLine();
      ui::RadioButton(TR("Column Row"), &n.apply_order, 1);
      ImGui::EndDisabled();
    }
    bool close = false;
    if (ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      const uint32_t sheet = s.focus_sheet;
      const std::vector<Rect> areas = SelAreas(s);
      const std::vector<std::string> chosen = n.apply_sel;
      Document::ApplyNamesOptions opt;
      opt.ignore_relative = n.apply_ignore_rel;
      opt.use_row_column = n.apply_row_col;
      opt.omit_column = n.apply_omit_col;
      opt.omit_row = n.apply_omit_row;
      opt.row_first = n.apply_order == 0;
      // one cell selected: Excel applies the names to the whole sheet
      const bool whole = areas.size() == 1 && areas[0].IsCell();
      const int changed = s.engine->Call([&](Document& d) {
        d.Begin(TR("Apply Names"));
        int k = 0;
        if (whole) k = d.ApplyNames(sheet, Rect{0, 0, core::kRowEnd, core::kMaxCols - 1}, chosen, opt);
        else
          for (const Rect& a : areas) k += d.ApplyNames(sheet, a, chosen, opt);
        d.Commit();
        return k;
      });
      if (changed == 0 && !chosen.empty()) s.ShowError("YCGrid", TR("YCGrid could not find any references to replace."));
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Paste Name (F3)
  if (BeginDlg(s, Dialog::PasteName, TR("Paste Name###yc_dlg_pastename"), 380, 0)) {
    std::vector<const model::NameDef*> list;
    for (const auto& x : *s.snap->wb.names) {
      if (!x.hidden && (x.sheet_scope < 0 || static_cast<uint32_t>(x.sheet_scope) == s.focus_sheet)) list.push_back(&x);
    }
    std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return Lower(a->name) < Lower(b->name); });
    bool close = false;
    ImGui::BeginChild("##pl", ImVec2(-1, S(230)), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < list.size(); ++i) {
      if (ImGui::Selectable(list[i]->name.c_str(), n.paste_sel == static_cast<int>(i), ImGuiSelectableFlags_AllowDoubleClick)) {
        n.paste_sel = static_cast<int>(i);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          InsertIntoFormula(s, list[i]->name);
          close = true;
        }
      }
    }
    if (list.empty()) ImGui::TextDisabled(TR("(no defined names)"));
    ImGui::EndChild();
    if ((ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) && !list.empty()) {
      InsertIntoFormula(s, list[static_cast<size_t>(std::clamp(n.paste_sel, 0, static_cast<int>(list.size()) - 1))]->name);
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Paste List"), ImVec2(S(90), 0)) && !list.empty() && !s.edit.active) {
      // paste a two-column "name | refers to" list at the active cell (like Excel)
      const uint32_t sheet = s.focus_sheet, r0 = s.FocusView().sel.act_r, c0 = s.FocusView().sel.act_c;
      std::vector<std::pair<std::string, std::string>> rows;
      for (const auto* x : list) rows.push_back({x->name, "'=" + ShownRefers(s, x->refers_to)});  // as the Name Manager shows it
      s.engine->PostUser(TR("Paste name list"), [sheet, r0, c0, rows](Document& d) {
        for (size_t i = 0; i < rows.size(); ++i) {
          d.SetInput(sheet, r0 + static_cast<uint32_t>(i), c0, rows[i].first);
          d.SetInput(sheet, r0 + static_cast<uint32_t>(i), c0 + 1, rows[i].second);
        }
      });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
}

}  // namespace yc::app
