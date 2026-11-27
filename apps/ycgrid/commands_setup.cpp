// SPDX-License-Identifier: Apache-2.0
// commands_setup.cpp - command registration (PLAN §11.5: ribbon, shortcuts, context menus and Python all go through here)
#include "app_state.h"
#include "frame/frame_context.h"
#include "error_log.h"
#include "settings.h"
#include "core/i18n.h"
#include "core/utf8.h"

#include "commands/command_registry.h"
#include "dock/dock_layout.h"
#include "engine/navigate.h"
#include "formula/datetime.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "titlebar/titlebar.h"

#include "imgui.h"

#include <shlobj.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <tuple>

namespace yc::app {

using engine::Document;
using ui::Command;
using ui::Commands;
namespace icon = ui::icon;

std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

void AppState::SetStatus(std::string s, double seconds) {
  status = std::move(s);
  status_until = ImGui::GetTime() + seconds;
}

void AppState::ShowMessage(std::string title, std::string text) {
  if (text.empty()) return;  // a refusal with nothing to say has said it on the status bar: never a blank box
  if (headless) {  // nobody can close it: recorded in the Errors window (ui.errors()) and over
    ReportError("YCGrid", title, text + "\n(" + TR("answered automatically: nobody can see this window") + ")", false);
    return;
  }
  ReportError("YCGrid", title, text, false);
  message_title = std::move(title);
  message_text = std::move(text);
  OpenDialog(Dialog::Message);
}

void AppState::ShowError(std::string title, std::string text) {
  if (text.empty()) return;
  if (quiet_error) {  // the Python request raises it (Errors window too)
    ReportError("YCGrid", title, text, true);
    *quiet_error = text;
    return;
  }
  if (headless) {
    ReportError("YCGrid", title, text + "\n(" + TR("answered automatically: nobody can see this window") + ")", true);
    return;
  }
  ReportError("YCGrid", title, text, true);
  message_title = std::move(title);
  message_text = std::move(text);
  OpenDialog(Dialog::Message);
}

void AppState::NoteRefusal(std::string text, std::string brief) {
  if (text.empty()) return;
  ReportRefusal(text);
  refusal = brief.empty() ? text.substr(0, text.find('\n')) : std::move(brief);
  refusal_until = ImGui::GetTime() + 6.0;
}

bool AppState::WindowsProtected() const { return snap && snap->wb.protect_windows; }

void AppState::SayWindowsLocked() {
  SetStatus(WindowsProtected() ? TR("The workbook's windows are protected (Review > Protect Workbook)")
                               : TR("This window is locked by the workbook's code"));
}

std::wstring AppState::LayoutPath() const {
  if (!file_path.empty()) return file_path + L".layout.json";
  wchar_t* local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) dir = std::wstring(local) + L"\\ycgrid";
  if (local) ::CoTaskMemFree(local);
  // a hidden instance (tests, agents) keeps its window layout to itself: containers and zoom made in a test must not
  // open in the user's next YCGrid
  if (!UserStateSaved()) dir = (std::filesystem::path(TempFolder()) / (L"YCGrid-hidden-" + std::to_wstring(::GetCurrentProcessId()))).wstring();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir + L"\\default_layout.json";
}

std::string AppState::SheetWindowName(uint32_t id) const {
  const model::Sheet* sh = snap ? snap->Sheet(id) : nullptr;
  return (sh ? sh->name : std::string(TR("Sheet"))) + "###yc_sheet_" + std::to_string(id);
}

std::string AppState::ContainerWindowName(const Container& c) const { return c.name + "###yc_container_" + std::to_string(c.id); }

namespace {

Command Make(const char* id, const char* label, const char* ic = nullptr, const char* shortcut = "", const char* desc = "", bool toggle = false) {
  Command c;
  c.id = id;
  c.label = label;
  c.icon = ic;
  c.shortcut = shortcut;
  c.tip_desc = desc;
  c.toggle = toggle;
  return c;
}

// ImGui ABGR -> model ARGB
uint32_t ToArgb(ImU32 c) {
  const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

struct NamedFormat {
  const char* name;
  const char* code;
};

// Currency and Accounting in the currency of the UI language: NT$ in Traditional Chinese, as Excel shows it in Taiwan
// (format codes can't go through TR: "##" marks an ImGui id there)
const char* LocalCode(const char* code) {
  if (i18n::Current() != i18n::Lang::ZhTW) return code;
  if (std::string_view(code) == "$#,##0.00") return "\"NT$\"#,##0.00";
  if (std::string_view(code) == "$#,##0.00_);[Red]($#,##0.00)") return "\"NT$\"#,##0.00_);[Red](\"NT$\"#,##0.00)";
  if (std::string_view(code) == "_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)")
    return "_(\"NT$\"* #,##0.00_);_(\"NT$\"* (#,##0.00);_(\"NT$\"* \"-\"??_);_(@_)";
  return code;
}
const NamedFormat kFormats[] = {
    {TR("General"), "General"},
    {TR("Number"), "0.00"},
    {TR("Currency"), "$#,##0.00"},
    {TR("Accounting"), "_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)"},
    {TR("Short Date"), "yyyy/m/d"},
    {TR("Long Date"), TR("mmmm d, yyyy")},
    {TR("Time"), TR("h:mm:ss AM/PM")},
    {TR("Percentage"), "0.00%"},
    {TR("Fraction"), "# ?/?"},
    {TR("Scientific"), "0.00E+00"},
    {TR("Text"), "@"},
    {TR("Timestamp (ns)"), "[epoch][local]yyyy-mm-dd hh:mm:ss.000000000"},
};

bool Editing(AppState& s) { return s.edit.active; }

void SetFontName(AppState& s, const std::string& name) {
  const auto areas = SelAreas(s);
  const uint32_t sheet = s.focus_sheet;
  s.engine->PostUser(TR("Font"), [=](Document& d) {
    const uint32_t id = d.wb_mut().styles.mut().InternString(name);
    for (const Rect& a : areas) d.ModifyStyle(sheet, a, [id](model::Style& x) { x.font_name = id; });
  });
}

void SetFontSize(AppState& s, float pt) {
  if (pt < 1 || pt > 409) return;
  if (!s.engine) return;
  EndCopyMode(s);  // Excel: formatting cells ends cut / copy mode
  const auto areas = SelAreas(s);
  const uint32_t sheet = s.focus_sheet;
  const std::function<void(model::Style&)> fn = [pt](model::Style& x) { x.font_size = pt; };
  const auto fit = FitRowsForStyle(s, sheet, areas, fn, nullptr);
  const std::vector<uint32_t> group = GroupTargets(s, sheet);  // grouped sheets get the format too (Excel)
  const int32_t h = static_cast<int32_t>(std::ceil(pt * 96.0f / 72.0f * 1.36f));
  // one Undo step: the font, the rows its text needs, and the rows a bigger font grows (empty cells too, Excel
  // auto-adjusts rows without a custom height). The rows were a step of their own that Undo never took back.
  s.engine->PostUser(TR("Font Size"), [=](Document& d) {
    for (const uint32_t g : group) {
      for (const Rect& a : areas) d.ModifyStyle(g, a, fn);
    }
    for (const auto& [row, rh] : fit) d.SetRowHeight(sheet, row, row, rh);
    const model::Sheet* sh = d.S(sheet);
    if (!sh) return;
    for (const Rect& a : areas) {
      if (a.Rows() > 100000) continue;
      for (uint32_t r = a.r0; r <= a.r1; ++r) {
        if (!sh->rows->IsCustom(r) && h > sh->rows->default_size()) d.SetRowHeight(sheet, r, r, h);
        else if (sh->rows->IsCustom(r) && h <= sh->rows->default_size() && pt <= 11.5f) d.SetRowHeight(sheet, r, r, sh->rows->default_size());
      }
    }
  });
}

float StepFontSize(float pt, int dir) {
  static const float kSizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
  if (dir > 0) {
    for (float s : kSizes) {
      if (s > pt + 0.01f) return s;
    }
    return pt + 10;
  }
  for (int i = static_cast<int>(std::size(kSizes)) - 1; i >= 0; --i) {
    if (kSizes[i] < pt - 0.01f) return kSizes[i];
  }
  return std::max(1.0f, pt - 1);
}

const model::Style* ActiveStyle(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return nullptr;
  const auto& sel = s.FocusView().sel;
  return &s.snap->wb.styles->Get(sh->GetStyle(sel.act_r, sel.act_c));
}

}  // namespace

void SyncCommandState(AppState& s) {
  auto set = [](const char* id, bool v) {
    if (auto* c = Commands().Find(id)) c->pressed = v;
  };
  auto enable = [](const char* id, bool v) {
    if (auto* c = Commands().Find(id)) c->enabled = v;
  };
  // what grouped sheets grayed out last frame comes back first; the rules below (protection) and the group's own at
  // the end decide again
  static std::vector<Command*> off_for_group;
  for (Command* c : off_for_group) c->enabled = true;
  off_for_group.clear();
  if (s.snap) {
    // the frame's version can be behind the engine (an undo still running, edits queued): Ctrl+Z / Ctrl+Y pressed
    // quickly stay enabled and the engine decides (an empty list does nothing), or the keys were lost
    const bool behind = s.undo_move || (s.engine && (s.engine->BusySeconds() > 0 || s.engine->Current() != s.snap));
    enable("Undo", (s.snap->can_undo || behind) && !s.edit.active);
    enable("Redo", (s.snap->can_redo || behind) && !s.edit.active);
    enable("PasteSpecial", !(CopyModeOn(s) && s.clip->cut));  // Excel: Paste Special isn't offered for cut cells
    if (auto* c = Commands().Find("Undo")) c->tip_desc = s.snap->can_undo ? TR("Undo \"") + s.snap->undo_label + TR("\"##ctx_close") : TR("Can't Undo");
    if (auto* c = Commands().Find("Redo")) c->tip_desc = s.snap->can_redo ? TR("Redo \"") + s.snap->redo_label + TR("\"##ctx_close") : TR("Can't Redo");
  }
  {
    // a protected sheet grays out what its protection doesn't allow (ribbon, menus and shortcuts), as in Excel
    const model::Sheet* psh = s.FocusSheet();
    const model::SheetProtection* pr = psh && psh->protection->on ? &*psh->protection : nullptr;
    static const char* const kCellFormat[] = {
        "Bold", "Italic", "Underline", "UnderlineDouble", "Strikethrough", "BorderBottom", "BorderTop", "BorderLeft", "BorderRight",
        "BorderNone", "BorderAll", "BorderOutside", "BorderThickOutside", "BorderDoubleBottom", "BorderThickBottom", "BorderTopBottom", "BorderTopThickBottom", "BorderTopDoubleBottom", "MoreBorders",
        "FillColor", "FontColor", "FontSizeIncrease", "FontSizeDecrease", "WrapText", "IndentDecrease", "IndentIncrease", "AlignLeft",
        "AlignCenter", "AlignRight", "AlignTop", "AlignMiddle", "AlignBottom", "NumberFormat", "AccountingFormat", "CurrencyFormat",
        "PercentStyle", "CommaStyle", "GeneralFormat", "NumberStyle", "TimeFormat", "DateFormat", "ScientificFormat", "DecimalIncrease",
        "DecimalDecrease", "ClearFormats", "ClearAll", "FormatCellsDialog", "FormatPainter"};
    for (const char* id : kCellFormat) enable(id, !pr || pr->format_cells);
    for (const char* id : {"MergeCenter", "MergeAcross", "MergeCells", "UnmergeCells", "ToggleFilter"}) enable(id, !pr);
    // a table holds no merged cells: Excel grays the merge buttons when the selection is in one
    if (psh && !psh->tables->empty()) {
      const Rect sel = SelRect(s);
      const bool in_table = std::any_of(psh->tables->begin(), psh->tables->end(), [&](const model::Table& t) { return t.range.Intersects(sel); });
      if (in_table) {
        for (const char* id : {"MergeCenter", "MergeAcross", "MergeCells"}) enable(id, false);
      }
    }
    for (const char* id : {"ColumnWidth", "AutoFitColumns", "HideColumns", "UnhideColumns", "DefaultWidth"}) enable(id, !pr || pr->format_columns);
    enable("LockCell", !pr);
    for (const char* id : {"RowHeight", "AutoFitRows", "HideRows", "UnhideRows"}) enable(id, !pr || pr->format_rows);
    enable("InsertRows", !pr || pr->insert_rows);
    enable("InsertColumns", !pr || pr->insert_columns);
    enable("InsertCells", !pr || pr->insert_rows || pr->insert_columns);
    enable("DeleteRows", !pr || pr->delete_rows);
    enable("DeleteColumns", !pr || pr->delete_columns);
    enable("DeleteCells", !pr || pr->delete_rows || pr->delete_columns);
    for (const char* id : {"SortAscending", "SortDescending", "CustomSort", "SortCellColorTop", "SortFontColorTop", "SortIconTop"}) enable(id, !pr || pr->sort);
    for (const char* id : {"NewComment", "InsertComment"}) enable(id, !pr || pr->objects);
  }
  if (s.group_sheets.size() > 1 && std::find(s.group_sheets.begin(), s.group_sheets.end(), s.focus_sheet) != s.group_sheets.end()) {
    // grouped sheets ([Group]) gray out what acts on one sheet's objects and lists, as in Excel 16 (measured 2026-11-16,
    // CommandBars.GetEnabledMso with two sheets selected): tables, sorting and filters, conditional formats, the
    // objects of the Insert tab, data tools, auditing, comments, protection, the print area, the background and the
    // Selection Pane. Ctrl+T made a table on the active sheet only (basics seq268426).
    static const char* const kExact[] = {
        "FormatAsTable", "ConditionalFormatting", "SortFilter", "CustomSort", "ToggleFilter", "AdvancedFilter", "InsertTable",
        "InsertPicture", "InsertShapesMenu", "RecommendedCharts", "InsertSlicer", "InsertHyperlink", "InsertSymbol",
        "InsertWordArtMenu", "DataValidation", "RemoveDuplicates", "WhatIfMenu", "GoalSeek", "ScenarioManager", "DataTableDialog",
        "GroupOutline", "UngroupOutline", "Subtotal", "TracePrecedents", "TraceDependents", "ErrorChecking", "TraceError",
        "NewComment", "InsertComment", "ProtectSheet", "PrintAreaMenu", "SetPrintArea", "ClearPrintArea", "AddToPrintArea",
        "SheetBackground", "SelectionPane", "PasteLink"};
    static const char* const kFamilies[] = {"Cf", "Sort", "FilterBySelected", "InsertChart", "InsertSpark", "InsertShape"};
    for (const auto& c : Commands().all()) {
      if (!c->enabled) continue;
      const std::string& id = c->id;
      bool off = std::any_of(std::begin(kExact), std::end(kExact), [&](const char* x) { return id == x; });
      off = off || std::any_of(std::begin(kFamilies), std::end(kFamilies), [&](const char* x) { return id.rfind(x, 0) == 0; });
      if (!off) continue;
      c->enabled = false;
      off_for_group.push_back(c.get());
    }
  }
  {
    const bool manual = s.snap ? s.snap->manual_calc : !Config().auto_calc;  // also set from Python (Application.Calculation)
    const bool semi = !manual && (s.snap ? s.snap->semiauto_calc : !Config().auto_data_tables);
    set("CalcAutomatic", !manual && !semi);
    set("CalcSemiautomatic", semi);
    set("CalcManual", manual);
  }
  SyncCircularMenu(s);
  SyncUseInFormulaMenu(s);
  if (const model::Style* st = ActiveStyle(s)) {
    set("Bold", st->bold);
    set("Italic", st->italic);
    set("Underline", st->underline != model::Underline::None);
    set("UnderlineDouble", st->underline == model::Underline::Double);
    set("Strikethrough", st->strike);
    set("LockCell", st->locked);
    set("WrapText", st->wrap);
    set("AlignLeft", st->halign == model::HAlign::Left);
    set("AlignCenter", st->halign == model::HAlign::Center || st->halign == model::HAlign::CenterAcross);
    set("AlignRight", st->halign == model::HAlign::Right);
    set("AlignTop", st->valign == model::VAlign::Top);
    set("AlignMiddle", st->valign == model::VAlign::Center);
    set("AlignBottom", st->valign == model::VAlign::Bottom);
    auto& rs = ui::ribbon::St();
    rs.font_name = s.snap->wb.styles->FontName(*st);
    char buf[16];
    const float pt = s.snap->wb.styles->FontSize(*st);
    if (std::fabs(pt - std::round(pt)) < 0.01f) std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(pt)));
    else std::snprintf(buf, sizeof(buf), "%.1f", pt);
    rs.font_size = buf;
    const std::string& code = s.snap->wb.styles->NumberFormat(*st);
    rs.number_format = TR("Custom");
    for (const auto& f : kFormats) {
      if (code == LocalCode(f.code)) rs.number_format = f.name;
    }
    if (code.find("[epoch]") != std::string::npos) rs.number_format = TR("Timestamp (ns)");
    {  // the Number Format list's samples: the active cell's value in each format (worked out when the cell changes)
      static uint64_t seen_version = ~0ull;
      static uint64_t seen_cell = ~0ull;
      const auto& sel = s.FocusView().sel;
      const uint64_t cell = (uint64_t{s.focus_sheet} << 48) ^ (uint64_t{sel.act_r} << 16) ^ sel.act_c;
      if (seen_version != s.snap->version || seen_cell != cell) {
        seen_version = s.snap->version;
        seen_cell = cell;
        rs.number_format_samples.clear();
        const model::Sheet* fsh = s.FocusSheet();
        const core::Value v = fsh ? fsh->GetValue(sel.act_r, sel.act_c) : core::Value{};
        for (const auto& f : kFormats) {
          std::string t;
          if (v.IsNumeric() || std::string_view(f.code) == "@" || std::string_view(f.code) == "General") t = engine::DisplayTextOf(v, LocalCode(f.code));
          if (t.size() > 40) t = std::string(core::Utf8Prefix(t, 37)) + "...";  // whole characters
          rs.number_format_samples.push_back(t);
        }
      }
    }
  }
  set("ToggleGridlines", s.show_gridlines);
  s.show_formulas = s.Win(s.focus_sheet).formulas;  // Show Formulas belongs to the sheet shown (Excel's window)
  set("ShowFormulas", s.show_formulas);
  set("ToggleChartWindow", s.show_chart);
  set("ToggleImGuiDemo", s.show_imgui_demo);
  set("ToggleImPlotDemo", s.show_implot_demo);
  set("FormatPainter", s.format_painter);
  UpdateBorderDrawCommands();
  {
    // Conditional Formatting > Clear Rules: from This Table / This PivotTable only when the active cell is in one
    const model::Sheet* csh = s.FocusSheet();
    const auto& csel = s.FocusView().sel;
    bool in_pivot = false;
    if (csh) {
      for (const auto& p : *csh->pivots) in_pivot = in_pivot || p.output.Contains(csel.act_r, csel.act_c);
    }
    enable("CfClearTable", s.snap && model::TableAt(s.snap->wb, s.focus_sheet, csel.act_r, csel.act_c) != nullptr);
    enable("CfClearPivot", in_pivot);
  }
  set("TogglePerf", s.show_perf);
  const auto& v = s.FocusView();
  if (auto* c = Commands().Find("FreezePanes")) c->label = (v.freeze_rows || v.freeze_cols) && !v.split ? TR("Unfreeze Panes") : TR("Freeze Panes");
  set("SplitWindow", v.split);
  set("ViewNormal", v.page_view == 0);
  set("ViewPageBreakPreview", v.page_view == 1);
  set("ViewPageLayout", v.page_view == 2);
  SyncProtectCommands(s);
}

// Ctrl+Y and F4: Redo when an Undo left something to redo, otherwise Repeat (Excel 16, measured: Ctrl+Shift+4, Ctrl+Z,
// F4 formats the same cell again). Decided on what the engine will have done, not on a frame that is behind: an Undo
// still running may leave something to redo (the engine decides), a user action still queued leaves nothing
void RedoOrRepeat(AppState& s) {
  bool redo = s.undo_move != nullptr;
  if (!redo && s.engine) {
    const auto cur = s.engine->Current();
    redo = cur && !s.engine->UserActionsPending(*cur) && cur->can_redo;
  }
  if (!(redo && Commands().Execute("Redo"))) Commands().Execute("Repeat");
}

// the cells the last Undo / Redo changed: their sheet and selection, as soon as the engine has done it
void ApplyUndoMove(AppState& s) {
  if (!s.undo_move) return;
  // done, and the frame has the version it made (the main loop takes it, releasing the old one off this thread)
  if (!s.undo_move->done || s.engine->Current() != s.snap) {
    ui::RequestRedrawIn(1.0 / 30);
    return;
  }
  const std::shared_ptr<AppState::UndoMove> m = std::move(s.undo_move);
  const model::Sheet* sh = m->ok && s.snap ? s.snap->Sheet(m->sheet) : nullptr;
  if (!sh || !sh->visible) return;
  if (m->sheet != s.focus_sheet) ActivateSheet(s, m->sheet);
  auto& v = s.Win(m->sheet).view;
  render::SelectRange(v, m->rect);
  render::EnsureVisible(*sh, v, m->rect.r0, m->rect.c0);
}

void RegisterCommands(AppState& s) {
  auto& R = Commands();
  auto add = [&](Command c) -> Command& { return R.Add(std::move(c)); };
  auto style_toggle = [&s](const char* label, std::function<void(model::Style&, bool)> fn) {
    return [&s, label, fn](Command& c) {
      if (Editing(s)) return;
      const bool on = c.pressed;
      ApplyStyle(s, label, [fn, on](model::Style& x) { fn(x, on); });
    };
  };
  auto style_set = [&s](const char* label, std::function<void(model::Style&)> fn) {
    return [&s, label, fn](Command&) {
      if (Editing(s)) return;
      ApplyStyle(s, label, fn);
    };
  };

  // ---- Quick Access Toolbar, File ----
  add(Make("Save", TR("Save"), icon::Save, "Ctrl+S", TR("Save the workbook."))).execute = [](Command&) { Commands().Execute("FileSave"); };
  // like Excel, the cells an Undo or Redo changes are shown and selected (also on another sheet)
  auto undo = [&s](bool redo) {
    if (!s.engine) return;
    // undone on the engine thread, after what is queued before it: putting back millions of formulas or a structural
    // change took the screen for seconds (and a few quick Ctrl+Z waited for each other)
    auto m = std::make_shared<AppState::UndoMove>();
    s.undo_move = m;
    s.engine->Post([redo, m](Document& d) {
      m->ok = d.UndoTarget(redo, &m->sheet, &m->rect);
      if (!(redo ? d.Redo() : d.Undo())) m->ok = false;
      m->done = true;
    });
  };
  add(Make("Undo", TR("Undo"), icon::Undo, "Ctrl+Z")).execute = [undo](Command&) { undo(false); };
  // the Undo / Redo drop-down lists on the Quick Access Toolbar
  ui::titlebar::St().history = [&s](bool redo) { return s.snap ? (redo ? s.snap->redo_labels : s.snap->undo_labels) : std::vector<std::string>{}; };
  ui::titlebar::St().history_run = [undo](bool redo, int count) {
    for (int k = 0; k < count; ++k) undo(redo);  // queued on the engine thread one after another
  };
  add(Make("Redo", TR("Redo"), icon::Redo, "Ctrl+Y")).execute = [undo](Command&) { undo(true); };
  // F4 outside a cell: Excel's Repeat runs the last formatting, insert / delete, fill or clear command again on the selection.
  // What it repeats is noted as commands run: typing never takes its place, and the commands that change nothing in the
  // workbook (another sheet, Copy, Find), Undo and Redo leave it, as in Excel (16, measured: Ctrl+Shift+4, then typing
  // in a cell, then F4 formats the next cell; Delete, Down, F4 clears the next cell)
  static const char* const kRepeatable[] = {
      "Bold", "Italic", "Underline", "UnderlineDouble", "Strikethrough", "BorderBottom", "BorderTop", "BorderLeft", "BorderRight",
      "BorderNone", "BorderAll", "BorderOutside", "BorderThickOutside", "BorderDoubleBottom", "BorderThickBottom", "BorderTopBottom", "BorderTopThickBottom", "BorderTopDoubleBottom",
      "FillColor", "FontColor", "FontName", "FontSize", "FontSizeIncrease", "FontSizeDecrease", "WrapText", "IndentDecrease",
      "IndentIncrease", "AlignLeft", "AlignCenter", "AlignRight", "AlignTop", "AlignMiddle", "AlignBottom", "MergeCenter",
      "MergeAcross", "MergeCells", "UnmergeCells", "NumberFormat", "AccountingFormat", "CurrencyFormat", "PercentStyle",
      "CommaStyle", "GeneralFormat", "NumberStyle", "TimeFormat", "DateFormat", "ScientificFormat", "DecimalIncrease",
      "DecimalDecrease", "InsertRows", "InsertColumns", "DeleteRows", "DeleteColumns", "DeleteCells", "HideRows", "HideColumns",
      "UnhideRows", "UnhideColumns", "AutoFitRows", "AutoFitColumns", "ClearAll", "ClearFormats", "ClearContents", "FillDown", "InsertCells",
      "FillRight", "FillUp", "FillLeft"};
  R.on_run = [&s](const std::string& id) {
    if (id == "Repeat" || id == "Undo" || id == "Redo") return;
    // an object inserted opens its contextual tab once it is there (the engine answers a few frames later)
    if (id.rfind("Insert", 0) == 0 || id.rfind("FormatAsTable", 0) == 0 || id.rfind("PivotTable", 0) == 0)
      ui::ribbon::St().open_contextual_until = ImGui::GetFrameCount() + 60;
    for (const char* k : kRepeatable) {
      if (id != k) continue;
      s.repeat_id = id;
      // Insert / Delete on whole rows or columns repeat as Insert / Delete Sheet Rows (Columns), as Excel's F4 does
      if ((id == "InsertCells" || id == "DeleteCells") && !s.repeat_command.empty() && s.repeat_command.rfind(id.substr(0, 6), 0) == 0)
        s.repeat_id = s.repeat_command;
      return;
    }
    if (ReadOnlyRefusesCommand(id)) s.repeat_id.clear();  // another change: F4 has nothing to repeat
  };
  add(Make("Repeat", TR("Repeat"), nullptr, "F4", TR("Repeat the last formatting, insert, delete, fill or clear command on the selection."))).execute = [&s](Command&) {
    const std::string id = s.repeat_id;  // (running it notes it again)
    if (!Editing(s) && !id.empty()) Commands().Execute(id);
  };
  add(Make("FileNew", TR("New"), icon::New, "Ctrl+N")).execute = [&s](Command&) {
    // like Excel: a new workbook gets a window of its own
    if (WorkbookInUse(s)) OpenInNewWindow(L"");
    else NewWorkbook(s);
  };
  add(Make("FileOpen", TR("Open"), icon::Open, "Ctrl+O")).execute = [&s](Command&) { OpenWorkbookDialog(s); };
  add(Make("FileSave", TR("Save"), icon::Save, "Ctrl+S")).execute = [&s](Command&) { SaveWithEvents(s, false); };  // Workbook_BeforeSave, then a background save
  add(Make("FileSaveAs", TR("Save As"), icon::SaveAs, "F12")).execute = [&s](Command&) { SaveWithEvents(s, true); };
  add(Make("ExportCsv", TR("Export CSV"), icon::SaveAs)).execute = [&s](Command&) { ExportCsvDialog(s); };
  add(Make("FileClose", TR("Close"), icon::Close, "Ctrl+W")).execute = [&s](Command&) { ::PostMessageW(s.hwnd, WM_CLOSE, 0, 0); };
  // [YC] About on the tab row: the version and where the program is
  add(Make("About", TR("About YCGrid"), icon::Info, "", TR("The version of YCGrid and where its program is."))).execute =
      [](Command&) { ui::ribbon::St().about_open_request = true; };
  add(Make("AboutCopyInfo", TR("Copy Version and Location"), icon::Copy)).execute = [](Command&) {
    ImGui::SetClipboardText(AboutText().c_str());
  };
  add(Make("OpenProgramLocation", TR("Open File Location"), icon::Open)).execute = [&s](Command&) { OpenProgramLocation(s); };

  // ---- Home -> Clipboard ----
  add(Make("Paste", TR("Paste"), icon::Paste, "Ctrl+V", TR("Paste content from the Clipboard."))).execute = [&s](Command&) { PasteClipboard(s); };
  add(Make("PasteValues", TR("Paste Values"), icon::PasteSmall)).execute = [&s](Command&) { PasteClipboard(s, engine::PasteMode::Values); };
  add(Make("PasteFormulas", TR("Formulas"), icon::Formula)).execute = [&s](Command&) { PasteClipboard(s, engine::PasteMode::Formulas); };
  add(Make("PasteFormats", TR("Formatting"), icon::FormatPainter)).execute = [&s](Command&) { PasteClipboard(s, engine::PasteMode::Formats); };
  add(Make("PasteTranspose", TR("Transpose"), icon::PasteSmall)).execute = [&s](Command&) { PasteClipboard(s, engine::PasteMode::All, true); };
  add(Make("PasteSpecial", TR("Paste Special..."), nullptr, "Ctrl+Alt+V")).execute = [&s](Command&) { s.OpenDialog(Dialog::PasteSpecial); };
  add(Make("Cut", TR("Cut"), icon::Cut, "Ctrl+X")).execute = [&s](Command&) { CopySelection(s, true); };
  add(Make("Copy", TR("Copy"), icon::Copy, "Ctrl+C")).execute = [&s](Command&) { CopySelection(s, false); };
  add(Make("FormatPainter", TR("Format Painter"), icon::FormatPainter, "", TR("Apply the formatting of one cell to other cells. Double-click to apply repeatedly."))).execute = [&s](Command&) {
    // ribbon buttons run on the mouse release: the second release of a double-click is the release of a click that
    // counted two (io.MouseClickedLastCount stays valid after the release). Excel: a double-click locks it on.
    const bool dbl = ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseClickedLastCount[ImGuiMouseButton_Left] >= 2;
    if (s.format_painter) {
      if (dbl) s.format_painter_sticky = true;  // the first click of the double-click turned it on
      else s.format_painter = false;
      return;
    }
    auto start = [dbl](AppState& st) {
      const auto areas = SelAreas(st);
      if (areas.empty() || !st.engine) return;
      const uint32_t sheet = st.focus_sheet;
      const Rect r = areas.back();
      // the painter keeps a copy of the cells it took the formats of, about 160 bytes a cell: millions of them took
      // gigabytes, and painting them took the rest (basics seq396008: out of memory). Past 4,000,000 cells, as an
      // array formula's limit, it says so instead
      if (st.engine->Call([&](Document& d) { return d.CopyCellCount(sheet, r); }) > 4'000'000) {
        st.ShowError("YCGrid", TR("The selection is too large to copy the formats of (at most 4,000,000 cells)."));
        return;
      }
      st.painter = st.engine->Call([&](Document& d) { return d.Copy(sheet, r); });
      st.format_painter = true;
      st.format_painter_sticky = dbl;
      st.painter_rect = r;
      st.painter_sheet = sheet;
      st.marquee = false;
      st.SetStatus(TR("Select the cells to apply the format to"));
    };
    if (DeferWhileEngineBusy(s, start)) return;  // the engine still works: picked up once it has caught up
    start(s);
  };

  // ---- Home -> Font ----
  add(Make("FontName", TR("Font"), nullptr, "Ctrl+Shift+F", TR("Pick a new font."))).execute = [&s](Command&) {
    if (!Editing(s)) SetFontName(s, ui::ribbon::St().font_name);
    else EditSelectionFormat(s, [](model::RichRun& r, const model::RunFont&) { r.font = ui::ribbon::St().font_name; });
  };
  add(Make("FontSize", TR("Font Size"), nullptr, "Ctrl+Shift+P", TR("Change the size of your text."))).execute = [&s](Command&) {
    if (!Editing(s)) SetFontSize(s, std::strtof(ui::ribbon::St().font_size.c_str(), nullptr));
    else if (const float pt = std::strtof(ui::ribbon::St().font_size.c_str(), nullptr); pt >= 1 && pt <= 409)
      EditSelectionFormat(s, [pt](model::RichRun& r, const model::RunFont&) { r.size = pt; });
  };
  add(Make("FontSizeIncrease", TR("Increase Font Size"), icon::FontIncrease, "Ctrl+Shift+>")).execute = [&s](Command&) {
    if (const model::Style* st = ActiveStyle(s)) SetFontSize(s, StepFontSize(s.snap->wb.styles->FontSize(*st), +1));
  };
  add(Make("FontSizeDecrease", TR("Decrease Font Size"), icon::FontDecrease, "Ctrl+Shift+<")).execute = [&s](Command&) {
    if (const model::Style* st = ActiveStyle(s)) SetFontSize(s, StepFontSize(s.snap->wb.styles->FontSize(*st), -1));
  };
  // while editing with selected characters, the font buttons change only those characters (like Excel)
  auto char_toggle = [&s, style_toggle](const char* label, std::function<void(model::Style&, bool)> fn, std::function<bool(const model::RunFont&)> get,
                                         std::function<void(model::RichRun&, bool)> set) {
    auto plain = style_toggle(label, fn);
    return [&s, plain, get, set](Command& c) {
      if (Editing(s)) {
        EditSelectionFormat(s, [&](model::RichRun& r, const model::RunFont& at) { set(r, !get(at)); });
        return;
      }
      plain(c);
    };
  };
  add(Make("Bold", TR("Bold"), icon::Bold, "Ctrl+B", TR("Make your text bold."), true)).execute =
      char_toggle(TR("Bold"), [](model::Style& x, bool on) { x.bold = on; }, [](const model::RunFont& f) { return f.bold; },
                  [](model::RichRun& r, bool on) { r.bold = on ? 1 : 0; });
  add(Make("Italic", TR("Italic"), icon::Italic, "Ctrl+I", TR("Italicize your text."), true)).execute =
      char_toggle(TR("Italic"), [](model::Style& x, bool on) { x.italic = on; }, [](const model::RunFont& f) { return f.italic; },
                  [](model::RichRun& r, bool on) { r.italic = on ? 1 : 0; });
  add(Make("Underline", TR("Underline"), icon::Underline, "Ctrl+U", TR("Underline your text."), true)).execute =
      char_toggle(TR("Underline"), [](model::Style& x, bool on) { x.underline = on ? model::Underline::Single : model::Underline::None; },
                  [](const model::RunFont& f) { return f.underline != model::Underline::None; },
                  [](model::RichRun& r, bool on) { r.underline = static_cast<int8_t>(on ? model::Underline::Single : model::Underline::None); });
  // a toggle like Underline (Excel): Double Underline on a double-underlined cell takes it off
  add(Make("UnderlineDouble", TR("Double Underline"), icon::Underline, "", "", true)).execute =
      char_toggle(TR("Double Underline"), [](model::Style& x, bool on) { x.underline = on ? model::Underline::Double : model::Underline::None; },
                  [](const model::RunFont& f) { return f.underline == model::Underline::Double; },
                  [](model::RichRun& r, bool on) { r.underline = static_cast<int8_t>(on ? model::Underline::Double : model::Underline::None); });
  add(Make("Strikethrough", TR("Strikethrough"), nullptr, "Ctrl+5", "", true)).execute =
      char_toggle(TR("Strikethrough"), [](model::Style& x, bool on) { x.strike = on; }, [](const model::RunFont& f) { return f.strike; },
                  [](model::RichRun& r, bool on) { r.strike = on ? 1 : 0; });
  auto border = [&s](engine::BorderPreset p, model::LineStyle ls = model::LineStyle::Thin) {
    return [&s, p, ls](Command&) {
      if (Editing(s)) return;
      const auto areas = SelAreas(s);
      const uint32_t sheet = s.focus_sheet;
      const uint32_t color = BorderPenColor();  // Line Color colors the menu's borders too (Excel)
      s.engine->PostUser(TR("Borders"), [=](Document& d) {
        for (const Rect& a : areas) d.SetBorders(sheet, a, p, ls, color);
      });
    };
  };
  add(Make("Borders", TR("Bottom Border"), icon::BorderBottom, "", TR("Add borders to the selected cells."))).execute = border(engine::BorderPreset::Bottom);
  add(Make("BorderBottom", TR("Bottom Border"), icon::BorderBottom)).execute = border(engine::BorderPreset::Bottom);
  add(Make("BorderTop", TR("Top Border"), icon::BorderTop)).execute = border(engine::BorderPreset::Top);
  add(Make("BorderLeft", TR("Left Border"), icon::BorderLeft)).execute = border(engine::BorderPreset::Left);
  add(Make("BorderRight", TR("Right Border"), icon::BorderRight)).execute = border(engine::BorderPreset::Right);
  add(Make("BorderNone", TR("No Border"), icon::BorderNone, "Ctrl+Shift+_")).execute = border(engine::BorderPreset::None);
  add(Make("BorderAll", TR("All Borders"), icon::BorderAll)).execute = border(engine::BorderPreset::All);
  add(Make("BorderOutside", TR("Outside Borders"), icon::BorderOutside, "Ctrl+Shift+&")).execute = border(engine::BorderPreset::Outside);
  add(Make("BorderThickOutside", TR("Thick Outside Borders"), icon::BorderThick)).execute = border(engine::BorderPreset::ThickOutside);
  add(Make("BorderDoubleBottom", TR("Bottom Double Border"), icon::BorderBottom)).execute = border(engine::BorderPreset::DoubleBottom);
  add(Make("BorderThickBottom", TR("Thick Bottom Border"), icon::BorderBottom)).execute = border(engine::BorderPreset::ThickBottom);
  add(Make("BorderTopBottom", TR("Top and Bottom Border"), icon::BorderBottom)).execute = border(engine::BorderPreset::TopBottom);
  add(Make("BorderTopThickBottom", TR("Top and Thick Bottom Border"), icon::BorderBottom)).execute = border(engine::BorderPreset::TopThickBottom);
  add(Make("BorderTopDoubleBottom", TR("Top and Double Bottom Border"), icon::BorderBottom)).execute = border(engine::BorderPreset::TopDoubleBottom);
  add(Make("MoreBorders", TR("More Borders..."), icon::BorderAll, "", TR("Open the Border tab of Format Cells."))).execute = [&s](Command&) {
    s.format_tab = 3;
    s.OpenDialog(Dialog::FormatCells);
  };
  add(Make("FillColor", TR("Fill Color"), icon::FillColor, "", TR("Color the background of cells."))).execute = [&s](Command&) {
    if (Editing(s)) return;
    const ImU32 c = ui::ribbon::St().fill_color;
    // a theme color picked from the palette stays a theme color (it changes with the theme)
    const uint32_t argb = (c >> 24) == 0 ? 0 : ui::ribbon::St().fill_theme ? ui::ribbon::St().fill_theme : ToArgb(c);  // transparent = no fill
    ApplyStyle(s, TR("Fill Color"), [argb](model::Style& x) { x.fill_color = argb; });
  };
  add(Make("FontColor", TR("Font Color"), icon::FontColor, "", TR("Change the color of your text."))).execute = [&s](Command&) {
    const ImU32 c = ui::ribbon::St().font_color;
    const uint32_t argb = (c >> 24) == 0 ? 0 : ui::ribbon::St().font_theme ? ui::ribbon::St().font_theme : ToArgb(c);
    if (Editing(s)) {
      EditSelectionFormat(s, [argb](model::RichRun& r, const model::RunFont&) { r.color = argb; });
      return;
    }
    ApplyStyle(s, TR("Font Color"), [argb](model::Style& x) { x.font_color = argb; });
  };
  add(Make("FormatCellsFontDialog", TR("Font Settings"), nullptr, "Ctrl+Shift+F", TR("Open the Font tab of Format Cells."))).execute = [&s](Command&) {
    s.format_tab = 2;
    s.OpenDialog(Dialog::FormatCells);
  };

  // ---- Home -> Alignment ----
  auto halign = [&](const char* id, const char* label, const char* ic, model::HAlign a) {
    add(Make(id, label, ic, "", "", true)).execute = style_toggle(label, [a](model::Style& x, bool on) { x.halign = on ? a : model::HAlign::General; });
  };
  auto valign = [&](const char* id, const char* label, const char* ic, model::VAlign a) {
    add(Make(id, label, ic, "", "", true)).execute = style_toggle(label, [a](model::Style& x, bool on) { x.valign = on ? a : model::VAlign::Bottom; });
  };
  valign("AlignTop", TR("Top Align"), icon::AlignTop, model::VAlign::Top);
  valign("AlignMiddle", TR("Center##ctx_align"), icon::AlignMiddle, model::VAlign::Center);
  valign("AlignBottom", TR("Bottom Align"), icon::AlignBottom, model::VAlign::Bottom);
  halign("AlignLeft", TR("Align Left"), icon::AlignLeft, model::HAlign::Left);
  halign("AlignCenter", TR("Center"), icon::AlignCenter, model::HAlign::Center);
  halign("AlignRight", TR("Align Right"), icon::AlignRight, model::HAlign::Right);
  add(Make("WrapText", TR("Wrap Text"), icon::WrapText, "", TR("Wrap extra-long text into multiple lines."), true)).execute = style_toggle(TR("Wrap Text"), [](model::Style& x, bool on) { x.wrap = on; });
  // Orientation menu: the same angle again turns it back to horizontal (like Excel)
  add(Make("TextOrientationMenu", TR("Orientation"), icon::Orientation, "", TR("Rotate your text diagonally or vertically."))).execute = [](Command&) {};
  auto orient = [&](const char* id, const char* label, int16_t deg) {
    add(Make(id, label)).execute = [&s, label, deg](Command&) {
      if (Editing(s)) return;
      const model::Sheet* sh = s.FocusSheet();
      const auto& sel = s.FocusView().sel;
      const bool same = sh && s.snap->wb.styles->Get(sh->GetStyle(sel.act_r, sel.act_c)).rotation == deg;
      ApplyStyle(s, label, [deg, same](model::Style& x) { x.rotation = same ? 0 : deg; });
    };
  };
  orient("AngleCounterclockwise", TR("Angle Counterclockwise"), 45);
  orient("AngleClockwise", TR("Angle Clockwise"), -45);
  orient("VerticalText", TR("Vertical Text"), 255);
  orient("RotateTextUp", TR("Rotate Text Up"), 90);
  orient("RotateTextDown", TR("Rotate Text Down"), -90);
  add(Make("IndentDecrease", TR("Decrease Indent"), icon::IndentDecrease)).execute = style_set(TR("Decrease Indent"), [](model::Style& x) {
    if (x.indent) --x.indent;
  });
  add(Make("IndentIncrease", TR("Increase Indent"), icon::IndentIncrease)).execute = style_set(TR("Increase Indent"), [](model::Style& x) {
    if (x.indent < 15) ++x.indent;
    // Excel: centered (filled, justified) text becomes left-aligned with the indent; Right and Distributed keep theirs
    if (x.halign != model::HAlign::Right && x.halign != model::HAlign::Distributed) x.halign = model::HAlign::Left;
  });
  auto merge = [&s](bool across, bool center, bool unmerge) {
    return [&s, across, center, unmerge](Command&) {
      if (Editing(s)) return;
      const auto areas = SelAreas(s);
      const uint32_t sheet = s.focus_sheet;
      const model::Sheet* sh = s.FocusSheet();
      auto run = [&s, areas, sheet, across, center, unmerge] {
        s.engine->PostUser(unmerge ? TR("Unmerge") : TR("Merge"), [=](Document& d) {
          for (const Rect& a : areas) {
            bool merged = false;
            if (!unmerge && center) {
              for (const Rect& m : *d.S(sheet)->merges) merged = merged || m == a;
            }
            if (unmerge || merged) d.Unmerge(sheet, a);  // Merge & Center again = unmerge
            else d.Merge(sheet, a, across, center);
          }
        });
      };
      // when more than one cell of an area to merge holds data, Excel asks first (OK / Cancel): only the upper-left
      // value is kept. One value anywhere is kept without asking (Document::Merge moves it up-left).
      if (!unmerge && sh) {
        bool lose = false;
        for (const Rect& a : areas) {
          if (a.Count() > 1000000) continue;
          bool already = false;
          for (const Rect& m : *sh->merges) already = already || m == a;
          if (already && center) continue;  // Merge & Center on a merged area unmerges it
          // Merge Across merges each row on its own
          int count = 0;
          for (uint32_t r = a.r0; r <= a.r1 && !lose; ++r) {
            if (across) count = 0;
            for (uint32_t c = a.c0; c <= a.c1 && count < 2; ++c) count += sh->IsEmptyCell(r, c) ? 0 : 1;
            lose = count > 1;
          }
          if (lose) break;
        }
        if (lose) {
          AskYesNo(s, "YCGrid", TR("Merging cells only keeps the upper-left value and discards other values."), run);
          return;
        }
      }
      run();
    };
  };
  add(Make("MergeCenter", TR("Merge & Center"), icon::Merge, "", TR("Combine and center the contents of the selected cells in a new larger cell."))).execute = merge(false, true, false);
  add(Make("MergeAcross", TR("Merge Across"), icon::Merge)).execute = merge(true, false, false);
  add(Make("MergeCells", TR("Merge Cells"), icon::Merge)).execute = merge(false, false, false);
  add(Make("UnmergeCells", TR("Unmerge Cells"), icon::Merge)).execute = merge(false, false, true);
  add(Make("FormatCellsAlignmentDialog", TR("Alignment Settings"), nullptr, "", TR("Open the Alignment tab of Format Cells."))).execute = [&s](Command&) {
    s.format_tab = 1;
    s.OpenDialog(Dialog::FormatCells);
  };

  // ---- Home -> Number ----
  add(Make("NumberFormat", TR("Number Format"), nullptr, "", TR("Choose how cell values are displayed, including [YC] timestamp (ns)."))).execute = [&s](Command&) {
    if (Editing(s)) return;
    const std::string name = ui::ribbon::St().number_format;
    for (const auto& f : kFormats) {
      if (name == f.name) ApplyNumberFormat(s, LocalCode(f.code));
    }
  };
  add(Make("AccountingFormat", TR("Accounting Number Format"), icon::Currency)).execute = [&s](Command&) { ApplyNumberFormat(s, LocalCode(kFormats[3].code)); };
  // Excel's Ctrl+Shift+$: currency with two decimals, negatives in parentheses
  add(Make("CurrencyFormat", TR("Currency"), icon::Currency, "Ctrl+Shift+$")).execute = [&s](Command&) { ApplyNumberFormat(s, LocalCode("$#,##0.00_);[Red]($#,##0.00)")); };
  // the Accounting Number Format arrow: Excel's currencies (as the Number tab's Accounting builds them)
  struct AccountingCur {
    const char* id;
    const char* label;
    const char* symbol;  // as it goes in the code
  };
  static const AccountingCur kAccounting[] = {
      {"AccountingDollar", TR("$ English (United States)"), "$"},
      {"AccountingPound", TR("\xC2\xA3 English (United Kingdom)"), "\"\xC2\xA3\""},
      {"AccountingEuro", TR("\xE2\x82\xAC Euro (\xE2\x82\xAC 123)"), "\"\xE2\x82\xAC\""},
      {"AccountingYuan", TR("\xC2\xA5 Chinese (PRC)"), "\"\xC2\xA5\""},
      {"AccountingNTD", TR("NT$ Chinese (Taiwan)"), "\"NT$\""},
      {"AccountingCHF", TR("CHF French (Switzerland)"), "\"CHF\" "},
  };
  for (const auto& a : kAccounting) {
    const std::string sym = a.symbol;
    add(Make(a.id, a.label, nullptr, "", TR("Accounting number format in this currency."))).execute = [&s, sym](Command&) {
      ApplyNumberFormat(s, "_(" + sym + "* #,##0.00_);_(" + sym + "* (#,##0.00);_(" + sym + "* \"-\"??_);_(@_)");
    };
  }
  add(Make("MoreAccountingFormats", TR("More Accounting Formats..."), nullptr, "", TR("Open the Number tab of Format Cells on Accounting."))).execute = [&s](Command&) {
    s.format_tab = 0;
    s.format_category = 3;  // Accounting
    s.OpenDialog(Dialog::FormatCells);
  };
  add(Make("PercentStyle", TR("Percent Style"), icon::Percent, "Ctrl+Shift+%")).execute = [&s](Command&) { ApplyNumberFormat(s, "0%"); };
  // Excel's Comma Style applies the Comma cell style (Accounting without a symbol: zero as "-", negatives in parentheses)
  add(Make("CommaStyle", TR("Comma Style"), icon::Comma)).execute = [&s](Command&) {
    if (Editing(s) || !s.engine) return;
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Comma Style"), [=](Document& d) {
      for (const Rect& a : areas) d.ApplyCellStyle(sheet, a, "Comma");
    });
  };
  // Excel's number format keys (Ctrl+Shift+~ ! @ # ^)
  add(Make("GeneralFormat", TR("General Format"), nullptr, "Ctrl+Shift+~")).execute = [&s](Command&) { ApplyNumberFormat(s, "General"); };
  add(Make("NumberStyle", TR("Number Format (2 decimals)"), nullptr, "Ctrl+Shift+!")).execute = [&s](Command&) { ApplyNumberFormat(s, "#,##0.00"); };
  add(Make("TimeFormat", TR("Time Format"), nullptr, "Ctrl+Shift+@")).execute = [&s](Command&) { ApplyNumberFormat(s, "h:mm AM/PM"); };
  add(Make("DateFormat", TR("Date Format"), nullptr, "Ctrl+Shift+#")).execute = [&s](Command&) { ApplyNumberFormat(s, "d-mmm-yy"); };
  add(Make("ScientificFormat", TR("Scientific Format"), nullptr, "Ctrl+Shift+^")).execute = [&s](Command&) { ApplyNumberFormat(s, "0.00E+00"); };
  add(Make("DecimalIncrease", TR("Increase Decimal"), icon::DecimalIncrease)).execute = [&s](Command&) { ChangeDecimals(s, +1); };
  add(Make("DecimalDecrease", TR("Decrease Decimal"), icon::DecimalDecrease)).execute = [&s](Command&) { ChangeDecimals(s, -1); };
  add(Make("FormatCellsNumberDialog", TR("Number Format Settings"), nullptr, "", TR("Open the Number tab of Format Cells."))).execute = [&s](Command&) {
    s.format_tab = 0;
    s.OpenDialog(Dialog::FormatCells);
  };
  add(Make("FormatCellsDialog", TR("Format Cells..."), nullptr, "Ctrl+1")).execute = [&s](Command&) { s.OpenDialog(Dialog::FormatCells); };

  // ---- Home -> Styles: menu buttons (items are registered in rules_ui.cpp and misc_ui.cpp) ----
  add(Make("ConditionalFormatting", TR("Conditional\nFormatting"), icon::CondFormat)).execute = [](Command&) {};

  // ---- Home -> Cells ----
  add(Make("InsertCells", TR("Insert"), icon::InsertCells, "Ctrl++")).execute = [&s](Command&) { InsertOrDelete(s, true, 0); };
  add(Make("InsertRows", TR("Insert Sheet Rows"))).execute = [&s](Command&) { InsertOrDelete(s, true, 1); };
  add(Make("InsertColumns", TR("Insert Sheet Columns"))).execute = [&s](Command&) { InsertOrDelete(s, true, 2); };
  add(Make("DeleteCells", TR("Delete"), icon::DeleteCells, "Ctrl+-")).execute = [&s](Command&) { InsertOrDelete(s, false, 0); };
  add(Make("DeleteRows", TR("Delete Sheet Rows"))).execute = [&s](Command&) { InsertOrDelete(s, false, 1); };
  add(Make("DeleteColumns", TR("Delete Sheet Columns"))).execute = [&s](Command&) { InsertOrDelete(s, false, 2); };
  // like Excel: Shift+F11, Home > Insert > Insert Sheet and the tab menu put the new sheet before the active one; the
  // (+) button after it (NewSheetAfter)
  auto insert_sheet = [&s](bool after) {
    if (!s.engine) return;
    const uint32_t id = s.engine->Call([&](Document& d) {
      d.Begin(TR("Insert Sheet"));
      const int32_t idx = d.wb().IndexOfId(s.active_sheet);
      const uint32_t nid = d.AddSheet({}, idx < 0 ? -1 : idx + (after ? 1 : 0));
      d.Commit();
      return nid;
    });
    s.engine->Sync();
    s.snap = s.engine->Current();
    ActivateSheet(s, id);
  };
  add(Make("InsertSheet", TR("Insert Sheet"), nullptr, "Shift+F11")).execute = [insert_sheet](Command&) { insert_sheet(false); };
  add(Make("NewSheetAfter", TR("New sheet"), nullptr, "", TR("Add a sheet after the active one."))).execute = [insert_sheet](Command&) { insert_sheet(true); };
  add(Make("DeleteSheet", TR("Delete Sheet"))).execute = [&s](Command&) {
    if (!s.engine) return;
    const uint32_t id = s.focus_sheet;
    bool refused = false;  // by protection or read-only mode: that has its own message (it said "at least one sheet")
    const bool ok = s.engine->Call([id, &refused](Document& d) {
      const uint32_t b0 = d.protect_blocks();
      d.Begin(TR("Delete Sheet"));
      const bool r = d.DeleteSheet(id);
      d.Commit();
      refused = d.protect_blocks() != b0;
      return r;
    });
    if (!ok && !refused) s.ShowMessage("YCGrid", TR("A workbook must contain at least one visible sheet."));
  };
  add(Make("FormatCellsMenu", TR("Format"), icon::FormatCells)).execute = [](Command&) {};
  add(Make("RowHeight", TR("Row Height..."))).execute = [&s](Command&) { s.OpenDialog(Dialog::RowHeight); };
  add(Make("ColumnWidth", TR("Column Width..."))).execute = [&s](Command&) { s.OpenDialog(Dialog::ColumnWidth); };
  add(Make("DefaultWidth", TR("Default Width..."), nullptr, "", TR("Set the standard width of the sheet's columns."))).execute = [&s](Command&) {
    s.OpenDialog(Dialog::StandardWidth);
  };
  // Excel's Lock Cell: a toggle that shows whether the active cell is locked
  add(Make("LockCell", TR("Lock Cell"), nullptr, "", TR("Lock or unlock the selected cells (it works once the sheet is protected)."), true)).execute =
      style_toggle(TR("Lock Cell"), [](model::Style& x, bool on) { x.locked = on; });
  // every row (column) of every area of the selection, measured once: one undo step (Excel)
  add(Make("AutoFitRows", TR("AutoFit Row Height"))).execute = [&s](Command&) {
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    for (const Rect& r : SelAreas(s)) spans.emplace_back(r.r0, r.r1);
    AutoFitDims(s, s.focus_sheet, false, spans);
  };
  // Excel: each selected column fits the selected cells only (select the data rows to leave a long title out)
  add(Make("AutoFitColumns", TR("AutoFit Column Width"))).execute = [&s](Command&) {
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    const auto areas = SelAreas(s);
    for (const Rect& r : areas) spans.emplace_back(r.c0, r.c1);
    AutoFitDims(s, s.focus_sheet, true, spans, &areas);
  };
  // every area of the selection (Ctrl+selected rows too); Unhide shows the hidden ones inside the selection
  add(Make("HideRows", TR("Hide Rows"), nullptr, "Ctrl+9")).execute = [&s](Command&) {
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Hide Rows"), [=](Document& d) {
      for (const Rect& r : areas) d.HideRows(sheet, r.r0, r.r1, true);
    });
  };
  add(Make("UnhideRows", TR("Unhide Rows"), nullptr, "Ctrl+Shift+(")).execute = [&s](Command&) {
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Unhide Rows"), [=](Document& d) {
      for (const Rect& r : areas) d.HideRows(sheet, r.r0, r.r1, false);
    });
  };
  add(Make("HideColumns", TR("Hide Columns"), nullptr, "Ctrl+0")).execute = [&s](Command&) {
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Hide Columns"), [=](Document& d) {
      for (const Rect& r : areas) d.HideCols(sheet, r.c0, r.c1, true);
    });
  };
  add(Make("UnhideColumns", TR("Unhide Columns"), nullptr, "Ctrl+Shift+)")).execute = [&s](Command&) {
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Unhide Columns"), [=](Document& d) {
      for (const Rect& r : areas) d.HideCols(sheet, r.c0, r.c1, false);
    });
  };
  add(Make("RenameSheet", TR("Rename Sheet"))).execute = [&s](Command&) { BeginRenameSheet(s, s.focus_sheet); };
  // the Format menu's Hide & Unhide submenu (its title; a KeyTip opens it: Alt, H, O, U)
  add(Make("HideUnhideMenu", TR("Hide & Unhide"))).execute = [](Command&) {};
  add(Make("TabColor", TR("Tab Color"))).execute = [&s](Command&) { s.OpenDialog(Dialog::TabColor); };
  add(Make("HideSheet", TR("Hide Sheet"))).execute = [&s](Command&) {
    const uint32_t id = s.focus_sheet;
    s.engine->PostUser(TR("Hide Sheet"), [id](Document& d) { d.SetSheetVisible(id, false); });
  };
  // Excel: a list of the hidden sheets to choose from
  add(Make("UnhideSheet", TR("Unhide Sheet..."))).execute = [&s](Command&) {
    bool any = false;
    for (const auto& sh : s.snap->wb.sheets) any = any || (!sh->visible && !sh->very_hidden);
    if (any) s.OpenDialog(Dialog::UnhideSheet);
    else s.SetStatus(TR("No sheet is hidden"));
  };
  add(Make("CopySheet", TR("Move or Copy..."))).execute = [&s](Command&) { s.OpenDialog(Dialog::MoveCopySheet); };

  // ---- Home -> Editing ----
  add(Make("AutoSum", TR("AutoSum"), icon::AutoSum, "Alt+=", TR("Automatically add up the numbers below or to the right of the selected cell."))).execute = [&s](Command&) { AutoSum(s, "SUM"); };
  add(Make("AutoAverage", TR("Average"))).execute = [&s](Command&) { AutoSum(s, "AVERAGE"); };
  add(Make("AutoCount", TR("Count Numbers"))).execute = [&s](Command&) { AutoSum(s, "COUNT"); };
  add(Make("AutoMax", TR("Max"))).execute = [&s](Command&) { AutoSum(s, "MAX"); };
  add(Make("AutoMin", TR("Min"))).execute = [&s](Command&) { AutoSum(s, "MIN"); };
  // the AutoSum menu's last entry, as in Excel: Insert Function with the cell started
  add(Make("AutoSumMoreFunctions", TR("More Functions..."), nullptr, "", TR("Choose any function in Insert Function."))).execute = [](Command&) {
    ui::Commands().Execute("InsertFunction");
  };
  add(Make("FillMenu", TR("Fill"), icon::FillDown)).execute = [](Command&) {};
  add(Make("FillDown", TR("Down"), icon::FillDown, "Ctrl+D")).execute = [&s](Command&) { FillSelection(s, 0); };
  add(Make("FillRight", TR("Right##ctx_dir"), nullptr, "Ctrl+R")).execute = [&s](Command&) { FillSelection(s, 1); };
  add(Make("FillUp", TR("Up"))).execute = [&s](Command&) { FillSelection(s, 2); };
  add(Make("FillLeft", TR("Left##ctx_dir"))).execute = [&s](Command&) { FillSelection(s, 3); };
  add(Make("ClearMenu", TR("Clear"), icon::Clear)).execute = [](Command&) {};
  add(Make("ClearAll", TR("Clear All"))).execute = [&s](Command&) { ClearSelection(s, 2); };
  add(Make("ClearFormats", TR("Clear Formats"))).execute = [&s](Command&) { ClearSelection(s, 1); };
  add(Make("ClearContents", TR("Clear Contents"), nullptr, "Delete")).execute = [&s](Command&) { ClearSelection(s, 0); };
  add(Make("SortFilter", TR("Sort &\nFilter"), icon::SortFilter)).execute = [](Command&) {};
  add(Make("SortAscending", TR("Sort A to Z"), icon::SortAscending)).execute = [&s](Command&) { SortSelection(s, false); };
  add(Make("SortDescending", TR("Sort Z to A"), icon::SortDescending)).execute = [&s](Command&) { SortSelection(s, true); };
  add(Make("FindSelect", TR("Find &\nSelect"), icon::FindSelect)).execute = [](Command&) {};
  add(Make("Find", TR("Find..."), icon::Search, "Ctrl+F")).execute = [&s](Command&) { OpenFind(s, false); };
  add(Make("Replace", TR("Replace..."), nullptr, "Ctrl+H")).execute = [&s](Command&) { OpenFind(s, true); };
  add(Make("GoTo", TR("Go To..."), nullptr, "Ctrl+G")).execute = [&s](Command&) { s.OpenDialog(Dialog::GoTo); };
  add(Make("FindNext", TR("Find Next"), nullptr, "Shift+F4", TR("Find the next match of the last search."))).execute = [&s](Command&) { FindNextKey(s); };
  // Go To Special's kinds straight from Find & Select (and Excel's keys)
  add(Make("SelectFormulas", TR("Formulas"), nullptr, "", TR("Select the cells with formulas."))).execute = [&s](Command&) {
    SelectSpecial(s, static_cast<int>(engine::Special::Formulas));
  };
  add(Make("SelectConstants", TR("Constants"), nullptr, "", TR("Select the cells with constants (no formulas)."))).execute = [&s](Command&) {
    SelectSpecial(s, static_cast<int>(engine::Special::Constants));
  };
  add(Make("SelectNotes", TR("Notes"), nullptr, "Ctrl+Shift+O", TR("Select the cells with notes."))).execute = [&s](Command&) {
    SelectSpecial(s, static_cast<int>(engine::Special::Comments));
  };
  add(Make("SelectConditionalFormats", TR("Conditional Formatting"), nullptr, "", TR("Select the cells with conditional formatting."))).execute = [&s](Command&) {
    SelectSpecial(s, static_cast<int>(engine::Special::AllFormatConditions));
  };
  add(Make("SelectDataValidation", TR("Data Validation"), nullptr, "", TR("Select the cells with data validation."))).execute = [&s](Command&) {
    SelectSpecial(s, static_cast<int>(engine::Special::AllValidation));
  };
  add(Make("SelectVisibleCells", TR("Select Visible Cells"), nullptr, "Alt+;", TR("Select only the cells that are shown (not hidden rows or columns)."))).execute =
      [&s](Command&) { SelectSpecial(s, static_cast<int>(engine::Special::Visible)); };
  add(Make("InsertDate", TR("Insert Today's Date"), nullptr, "Ctrl+;")).execute = [&s](Command&) { StartEdit(s, true, TodayText()); };
  // Ctrl+' / Ctrl+Shift+": start typing in the active cell with the formula (unchanged) or the value of the cell above
  auto copy_above = [&s](bool value) {
    return [&s, value](Command&) {
      const model::Sheet* sh = s.FocusSheet();
      const auto& sel = s.FocusView().sel;
      if (!sh || sel.act_r == 0 || Editing(s)) return;
      const uint32_t r = sel.act_r - 1, c = sel.act_c;
      std::string text;
      if (!value) text = engine::InputTextOf(s.snap->wb, s.focus_sheet, r, c, Config().r1c1);
      else if (const core::Value& v = sh->GetValue(r, c); !v.IsEmpty()) text = engine::DisplayTextOf(v, "General");
      StartEdit(s, true, text);
      s.edit.enter_mode = false;  // Edit mode, as in Excel: the arrows move the caret
    };
  };
  add(Make("CopyFormulaAbove", TR("Copy Formula from Above"), nullptr, "Ctrl+'")).execute = copy_above(false);
  add(Make("CopyValueAbove", TR("Copy Value from Above"), nullptr, "Ctrl+Shift+\"")).execute = copy_above(true);
  add(Make("InsertTime", TR("Insert Current Time"), nullptr, "Ctrl+Shift+:")).execute = [&s](Command&) { StartEdit(s, true, NowTimeText()); };

  // ---- Formulas ----
  add(Make("Calculate", TR("Calculate Now"), icon::Calculator, "F9")).execute = [&s](Command&) {
    // also with manual calculation, where nothing else recalculates, and the data tables "Automatic except for data
    // tables" leaves out
    s.engine->Post([](Document& d) { d.CalculateNow(); });
  };
  add(Make("CalculateFull", TR("Calculate Full"), icon::CalculateFull, "Ctrl+Alt+F9")).execute = [&s](Command&) { s.engine->Post([](Document& d) { d.RecalcAll(); }); };
  add(Make("CalculateSheet", TR("Calculate Sheet"), icon::CalculateFull, "Shift+F9", TR("Calculate the active sheet now."))).execute = [&s](Command&) {
    // only the active sheet, as in Excel: with manual calculation the other sheets keep waiting
    const uint32_t sheet = s.focus_sheet;
    s.engine->Post([sheet](Document& d) { d.RecalcSheet(sheet); });
  };
  // Calculation Options: Automatic or Manual, as Excel's dropdown (also File > Options > Calculation)
  auto calc_mode = [&s](bool automatic, bool tables) {
    Config().auto_calc = automatic;
    Config().auto_data_tables = tables;
    s.engine->Post([automatic, tables](Document& d) {
      if (!automatic && d.auto_calc) d.Recalc();  // what was entered before is calculated first
      d.auto_calc = automatic;
      d.auto_data_tables = tables;
    });
    if (!SaveSettings()) s.SetStatus(TR("Couldn't save the settings: ") + ToUtf8(SettingsPath()), 8.0);
  };
  add(Make("CalculationOptions", TR("Calculation\nOptions"), icon::CalcOptions, "", TR("Calculate formulas automatically, or only when you ask (F9)."))).execute = [](Command&) {};
  add(Make("CalcAutomatic", TR("Automatic"), nullptr, "", "", true)).execute = [calc_mode](Command&) { calc_mode(true, true); };
  add(Make("CalcSemiautomatic", TR("Automatic Except for Data Tables"), nullptr, "", "", true)).execute = [calc_mode](Command&) { calc_mode(true, false); };
  add(Make("CalcManual", TR("Manual"), nullptr, "", "", true)).execute = [calc_mode](Command&) { calc_mode(false, true); };
  add(Make("ShowFormulas", TR("Show Formulas"), icon::ShowFormulas, "Ctrl+`", "", true)).execute = [&s](Command& c) { s.show_formulas = s.Win(s.focus_sheet).formulas = c.pressed; };
  // the formula bar's chevron (Ctrl+Shift+U): three lines, or back to one (Application.FormulaBarHeight)
  add(Make("ExpandFormulaBar", TR("Expand Formula Bar"), nullptr, "Ctrl+Shift+U")).execute = [](Command&) {
    Config().formula_bar_lines = Config().formula_bar_lines > 1 ? 1 : 3;
    SaveSettings();
  };
  // Ctrl+/: the whole array of the active cell (a spill range or an array formula), as in Excel
  add(Make("SelectCurrentArray", TR("Select Current Array"), nullptr, "Ctrl+/")).execute = [&s](Command&) {
    const model::Sheet* sh = s.FocusSheet();
    auto& v = s.FocusView();
    const model::SpillRange* sp = sh && !sh->spills->empty() ? sh->SpillAt(v.sel.act_r, v.sel.act_c) : nullptr;
    if (!sp) {
      s.SetStatus(TR("The active cell isn't part of an array"));
      return;
    }
    const uint32_t ar = v.sel.act_r, ac = v.sel.act_c;
    render::SelectRange(v, sp->rect);
    v.sel.act_r = ar, v.sel.act_c = ac;
  };
  // a #SPILL! formula's error button: the cells in the range it wants that hold something
  add(Make("SelectObstructingCells", TR("Select Obstructing Cells"))).execute = [&s](Command&) {
    const model::Sheet* sh = s.FocusSheet();
    auto& v = s.FocusView();
    if (!sh || !s.snap) return;
    for (const auto& [k, want] : s.snap->spill_blocked) {
      if (engine::KeySheet(k) != s.focus_sheet || engine::KeyRow(k) != v.sel.act_r || engine::KeyCol(k) != v.sel.act_c) continue;
      std::vector<Rect> areas;
      for (uint32_t r = want.r0; r <= want.r1 && areas.size() < 10000; ++r) {
        for (uint32_t c = want.c0; c <= want.c1 && areas.size() < 10000; ++c) {
          if ((r != want.r0 || c != want.c0) && !sh->IsEmptyCell(r, c)) areas.push_back(Rect::Cell(r, c));
        }
      }
      if (areas.empty()) return;
      v.sel.areas = areas;
      v.sel.anc_r = v.sel.act_r = areas.front().r0;
      v.sel.anc_c = v.sel.act_c = areas.front().c0;
      v.sel.mode = render::SelectMode::Cells;
      render::EnsureVisible(*sh, v, v.sel.act_r, v.sel.act_c);
      return;
    }
    s.SetStatus(TR("The active cell isn't a formula that can't spill"));
  };
  add(Make("InsertFunction", TR("Insert Function"), icon::Formula, "Shift+F3")).execute = [&s](Command&) {
    // a cell holding one function call: its Function Arguments (as Excel's fx); otherwise the function list
    if (!OpenFunctionArgsForCell(s)) {
      BeginFunctionEdit(s);  // Excel: the cell shows "=" while Insert Function is open
      s.OpenDialog(Dialog::InsertFunction);
    }
  };

  // ---- Insert ----
  auto chart = [&s](int type, const char* name) {
    return [&s, type, name](Command&) {
      s.show_chart = true;
      s.chart_type = type;
      s.SetStatus(std::string(TR("Insert chart: ")) + name);
    };
  };
  add(Make("InsertChartColumn", TR("Column##ctx_chart"), icon::ChartColumn)).execute = chart(0, TR("Column##ctx_chart"));
  add(Make("InsertChartLine", TR("Line"), icon::ChartLine)).execute = chart(1, TR("Line"));
  add(Make("InsertChartPie", TR("Pie"), icon::ChartPie)).execute = chart(2, TR("Pie"));
  add(Make("InsertChartScatter", TR("Scatter"), icon::ChartScatter)).execute = chart(3, TR("Scatter"));

  // ---- View ----
  add(Make("ToggleGridlines", TR("Gridlines"))).execute = [](Command&) {};
  add(Make("ToggleHeadings", TR("Headings"))).execute = [](Command&) {};
  add(Make("ToggleFormulaBar", TR("Formula Bar"))).execute = [](Command&) {};
  add(Make("ZoomDialog", TR("Zoom"), icon::Zoom, "", TR("Set the zoom level."))).execute = [&s](Command&) { s.OpenDialog(Dialog::ZoomDialog); };
  add(Make("Zoom100", "100%", icon::Zoom100)).execute = [&s](Command&) { s.FocusView().zoom = 1.0f; };
  add(Make("ZoomToSelection", TR("Zoom to\nSelection"), icon::ZoomSelection, "", TR("Zoom the sheet so the selected cells fill the window."))).execute = [&s](Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    auto& v = s.FocusView();
    const Rect r = SelRect(s);
    const double w = static_cast<double>(sh->cols->Offset(r.c1 + 1) - sh->cols->Offset(r.c0));
    const double h = static_cast<double>(sh->rows->Offset(r.r1 + 1) - sh->rows->Offset(r.r0));
    const double zw = v.cells_rect.GetWidth() / ImGui::GetStyle().FontScaleDpi / std::max(1.0, w);
    const double zh = v.cells_rect.GetHeight() / ImGui::GetStyle().FontScaleDpi / std::max(1.0, h);
    v.zoom = static_cast<float>(std::clamp(std::min(zw, zh), 0.1, 4.0));
    v.v.first = r.r0;
    v.h.first = r.c0;
  };
  add(Make("NewContainer", TR("New\nContainer"), icon::NewContainer, "", TR("[YC] Create an empty container that sheet windows and charts can dock into (PLAN §10.5)."))).execute = [&s](Command&) {
    Container c;
    c.id = s.next_container_id++;
    c.name = TR("Container ") + std::to_string(c.id);
    s.containers.push_back(c);
    s.SetStatus(TR("Created ") + c.name + TR(": drag a sheet tab out, then drag it into the container to dock it"));
  };
  add(Make("FreezePanes", TR("Freeze Panes"), icon::Freeze)).execute = [&s](Command&) { FreezePanes(s, 0); };
  add(Make("SplitWindow", TR("Split"), icon::SplitWindow, "", TR("Divide the window into panes that each scroll on their own."), true)).execute = [&s](Command&) { SplitWindow(s); };
  add(Make("FreezeTopRow", TR("Freeze Top Row"))).execute = [&s](Command&) { FreezePanes(s, 1); };
  add(Make("FreezeFirstColumn", TR("Freeze First Column"))).execute = [&s](Command&) { FreezePanes(s, 2); };
  add(Make("FreezeCenterColumns", TR("Freeze Columns in the Middle"), nullptr, "",
           TR("[YC] Keep the selected columns in the middle and the rows above the active cell on top; the columns on their left scroll outward with the ones on their right (an option chain with the strike frozen)."))).execute =
      [&s](Command&) { FreezePanes(s, 3); };
  add(Make("LayoutSave", TR("Save Layout"), icon::LayoutSave, "", TR("Save window positions, sizes, docking tree, scroll positions and which panes are open."))).execute = [&s](Command&) { s.req_layout_save = true; };
  add(Make("LayoutLoad", TR("Load Layout"), icon::LayoutLoad)).execute = [&s](Command&) { s.req_layout_load = true; };
  add(Make("LayoutReset", TR("Reset Layout"), icon::LayoutReset)).execute = [&s](Command&) {
    if (s.WindowsProtected()) {  // Python resets with Application.Layout.Reset(), which isn't refused
      s.SayWindowsLocked();
      return;
    }
    s.req_layout_reset = true;
  };
  add(Make("ExpandRows100M", TR("Expand to\n100M Rows"), icon::Rows, "", TR("Expand the current sheet to 100 million rows (rounded up to a multiple of 1,048,576, PLAN §5.1)."))).execute = [&s](Command&) {
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Expand Rows"), [sheet](Document& d) {
      const uint32_t unit = 1048576;
      d.SetRowLimit(sheet, (100000000 + unit - 1) / unit * unit);
    });
    s.SetStatus(TR("Expanded to 100,663,296 rows: try Ctrl+↓ and dragging the scroll box"), 6.0);
  };
  add(Make("ToggleChartWindow", TR("Chart Window"), icon::Chart, "", "", true)).execute = [&s](Command& c) { s.show_chart = c.pressed; };
  add(Make("ToggleImGuiDemo", TR("ImGui Demo"), icon::Demo, "", "", true)).execute = [&s](Command& c) { s.show_imgui_demo = c.pressed; };
  add(Make("ToggleImPlotDemo", TR("ImPlot Demo"), icon::Chart, "", "", true)).execute = [&s](Command& c) { s.show_implot_demo = c.pressed; };
  RegisterPane("chart_window", "ToggleChartWindow", [&s]() -> bool& { return s.show_chart; });
  RegisterPane("perf", "TogglePerf", [&s]() -> bool& { return s.show_perf; });
  RegisterPane("formula_bar", "", [&s]() -> bool& { return s.show_formula_bar; });
  add(Make("TogglePerf", TR("Performance Panel"), icon::Data, "", TR("Show cells processed, format calls and recalc time per frame (PLAN §8.0)."), true)).execute = [&s](Command& c) { s.show_perf = c.pressed; };
  add(Make("NextSheet", TR("Next Sheet"), nullptr, "Ctrl+PageDown")).execute = [&s](Command&) {
    if (!s.snap) return;
    const auto& v = s.snap->wb.sheets;
    const int32_t i = s.snap->wb.IndexOfId(s.active_sheet);
    for (size_t k = static_cast<size_t>(i) + 1; k < v.size(); ++k) {
      if (v[k]->visible && !s.Win(v[k]->id).floating) {
        ActivateSheet(s, v[k]->id);
        break;
      }
    }
  };
  add(Make("PrevSheet", TR("Previous Sheet"), nullptr, "Ctrl+PageUp")).execute = [&s](Command&) {
    if (!s.snap) return;
    const auto& v = s.snap->wb.sheets;
    const int32_t i = s.snap->wb.IndexOfId(s.active_sheet);
    for (int32_t k = i - 1; k >= 0; --k) {
      if (v[static_cast<size_t>(k)]->visible && !s.Win(v[static_cast<size_t>(k)]->id).floating) {
        ActivateSheet(s, v[static_cast<size_t>(k)]->id);
        break;
      }
    }
  };
}

}  // namespace yc::app
