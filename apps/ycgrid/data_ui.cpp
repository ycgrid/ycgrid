// SPDX-License-Identifier: Apache-2.0
// data_ui.cpp - Data tools and formula auditing: Remove Duplicates, Text to Columns, Group / Ungroup (outline),
// Trace Precedents / Dependents arrows, Evaluate Formula, Watch Window
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "engine/error_check.h"
#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "formula/numfmt.h"
#include "settings.h"
#include "engine/snapshot.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <tuple>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

struct DataUi {
  // Remove Duplicates
  Rect rd_rect{};
  bool rd_header = true;
  std::vector<char> rd_cols;
  // Text to Columns
  Rect tc_rect{};
  int tc_kind = 0;  // 0 delimited, 1 fixed width
  bool tc_tab = true, tc_semi = false, tc_comma = false, tc_space = false, tc_other = false, tc_consec = false;
  char tc_other_ch[8] = "";
  int tc_qual = 0;  // 0 ", 1 ', 2 none
  char tc_breaks[256] = "";
  char tc_dest[64] = "";
  std::vector<int> tc_types;  // 0 General, 1 Text, 2 skip, 3..8 Date (MDY, DMY, YMD, MYD, DYM, YDM)
  int tc_step = 0;            // the wizard's page: 0 data type, 1 delimiters / break lines, 2 column data format
  int tc_col = 0;             // the column chosen in step 3's preview
  int tc_date = 5;            // the date order offered for Date (the system's short date order)
  bool tc_adv = false;        // Advanced... shown
  int tc_dec = 0, tc_th = 1;  // Advanced: decimal / thousands separator (. , ' space)
  bool tc_trail = true;       // Advanced: trailing minus for negative numbers
  // Group / Ungroup dialog
  int grp_rows = 0;  // 0 rows, 1 columns
  bool grp_group = true;
  // Evaluate Formula: the formula of the active cell, then one more box per Step In (the cell a reference points to)
  uint32_t ev_sheet = 0, ev_r = 0, ev_c = 0;
  std::vector<engine::EvalStep> ev_steps;
  size_t ev_pos = 0;
  struct EvFrame {
    uint32_t sheet = 0, r = 0, c = 0;
    std::vector<engine::EvalStep> steps;
    size_t pos = 0;
  };
  std::vector<EvFrame> ev_inner;
  // Watch Window
  int watch_sel = -1;
  char watch_ref[512] = "";  // Add Watch: the cells to watch
  // Sort Warning: the action waiting for the answer (0 sort A to Z, 1 Z to A, 2 Custom Sort, 3 Remove Duplicates)
  int sw_action = 0, sw_choice = 0;
  DataRange sw_range;
  // Outline settings
  bool ol_below = true, ol_right = true, ol_styles = false;
};

DataUi& D() {
  static DataUi d;
  return d;
}

bool BeginDlg(AppState& s, Dialog d, const char* title, float width) {
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(width), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return false;
  }
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

std::string CellText(AppState& s, const model::Sheet& sh, uint32_t r, uint32_t c) {
  const auto& tbl = *s.snap->wb.styles;
  return engine::DisplayTextOf(sh.GetValue(r, c), tbl.NumberFormat(tbl.Get(sh.GetStyle(r, c))));
}

engine::TextToColumnsOptions TcOptions() {
  DataUi& d = D();
  engine::TextToColumnsOptions o;
  o.fixed = d.tc_kind == 1;
  o.tab = d.tc_tab;
  o.semicolon = d.tc_semi;
  o.comma = d.tc_comma;
  o.space = d.tc_space;
  if (d.tc_other && d.tc_other_ch[0]) o.other = std::string(1, d.tc_other_ch[0]);
  o.consecutive = d.tc_consec;
  o.qualifier = d.tc_qual == 0 ? '"' : (d.tc_qual == 1 ? '\'' : 0);
  for (const char* p = d.tc_breaks; *p;) {
    while (*p && !(*p >= '0' && *p <= '9')) ++p;
    if (!*p) break;
    o.breaks.push_back(static_cast<uint32_t>(std::strtoul(p, const_cast<char**>(&p), 10)));
  }
  for (int t : d.tc_types) o.types.push_back(t == 1 ? 2 : (t == 2 ? 9 : (t >= 3 && t <= 8 ? t : 1)));
  static const char kSep[] = {'.', ',', '\'', ' '};
  o.decimal = kSep[std::clamp(d.tc_dec, 0, 3)];
  o.thousands = kSep[std::clamp(d.tc_th, 0, 3)];
  o.trailing_minus = d.tc_trail;
  return o;
}

// the system's short date order as a Text to Columns date type (3 MDY, 4 DMY, 5 YMD)
int SystemDateOrder() {
  wchar_t buf[4] = {};
  if (::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IDATE, buf, 4) > 0) return buf[0] == L'1' ? 4 : (buf[0] == L'2' ? 5 : 3);
  return 3;
}

// Text to Columns, fixed width: the data under a ruler, as Excel shows it. A click adds a break line, a drag moves
// one, a double-click takes it away; the first time, breaks are suggested where every row has a space
void DrawBreakRuler(AppState& s, DataUi& d) {
  const model::Sheet* sh = s.FocusSheet();
  std::vector<std::string> rows;
  size_t longest = 0;
  if (sh) {
    for (uint32_t r = d.tc_rect.r0; r <= d.tc_rect.r1 && rows.size() < 8; ++r) {
      rows.push_back(CellText(s, *sh, r, d.tc_rect.c0));
      longest = std::max(longest, rows.back().size());
    }
  }
  std::vector<uint32_t> breaks;
  for (const char* q = d.tc_breaks; *q;) {
    while (*q && !(*q >= '0' && *q <= '9')) ++q;
    if (!*q) break;
    breaks.push_back(static_cast<uint32_t>(std::strtoul(q, const_cast<char**>(&q), 10)));
  }
  static bool suggested = false;
  if (ImGui::IsWindowAppearing()) suggested = false;
  if (breaks.empty() && !suggested && !rows.empty()) {
    // a break after a run of spaces that every row has (the start of the next column)
    suggested = true;
    for (size_t k = 1; k < longest; ++k) {
      bool all = true;
      for (const auto& r : rows) all = all && k < r.size() && r[k - 1] == ' ' && r[k] != ' ';
      if (all) breaks.push_back(static_cast<uint32_t>(k));
    }
  }
  auto store = [&]() {
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
    std::string txt;
    for (uint32_t b : breaks) txt += (txt.empty() ? "" : ", ") + std::to_string(b);
    std::snprintf(d.tc_breaks, sizeof(d.tc_breaks), "%s", txt.c_str());
  };
  store();
  ImGui::TextDisabled("%s", TR("Click the ruler or the data to add a break line; drag a line to move it; double-click it to take it away."));
  const float cw = ImGui::CalcTextSize("M").x, lh = ImGui::GetTextLineHeightWithSpacing();
  const size_t cols = std::max<size_t>(longest + 4, 40);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = std::min(S(520), cw * static_cast<float>(cols) + S(8)), h = lh * (static_cast<float>(rows.size()) + 1.2f) + S(6);
  ImGui::InvisibleButton("##ruler", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p0 + ImVec2(w, h), IM_COL32_WHITE);
  dl->AddRect(p0, p0 + ImVec2(w, h), IM_COL32(0xA0, 0xA0, 0xA0, 255));
  const float x0 = p0.x + S(4), ruler_h = lh;
  // the ruler: a tick per character, a number every ten
  for (size_t k = 0; k <= cols && x0 + cw * k < p0.x + w; ++k) {
    const float x = x0 + cw * static_cast<float>(k);
    const float th = k % 10 == 0 ? ruler_h * 0.6f : k % 5 == 0 ? ruler_h * 0.4f : ruler_h * 0.2f;
    dl->AddLine(ImVec2(x, p0.y + ruler_h - th), ImVec2(x, p0.y + ruler_h), IM_COL32(0x60, 0x60, 0x60, 255));
    if (k % 10 == 0 && k) dl->AddText(ImVec2(x + S(2), p0.y), IM_COL32(0x60, 0x60, 0x60, 255), std::to_string(k).c_str());
  }
  // the data, one character per ruler step
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t k = 0; k < rows[r].size() && x0 + cw * k < p0.x + w; ++k) {
      const char ch[2] = {rows[r][k], 0};
      dl->AddText(ImVec2(x0 + cw * static_cast<float>(k), p0.y + ruler_h + S(4) + lh * static_cast<float>(r)), IM_COL32_BLACK, ch);
    }
  }
  // the break lines, with Excel's arrow at the top
  static int dragging = -1;
  const ImVec2 m = ImGui::GetIO().MousePos;
  const int at = static_cast<int>(std::lround((m.x - x0) / cw));
  int on_line = -1;
  for (size_t i = 0; i < breaks.size(); ++i) {
    const float x = x0 + cw * static_cast<float>(breaks[i]);
    dl->AddLine(ImVec2(x, p0.y + S(2)), ImVec2(x, p0.y + h - S(2)), IM_COL32_BLACK, S(1.5f));
    dl->AddTriangleFilled(ImVec2(x - S(4), p0.y + S(2)), ImVec2(x + S(4), p0.y + S(2)), ImVec2(x, p0.y + S(8)), IM_COL32_BLACK);
    if (std::fabs(m.x - x) < cw * 0.5f) on_line = static_cast<int>(i);
  }
  if (hovered && on_line >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && on_line >= 0) {
    breaks.erase(breaks.begin() + on_line);
    dragging = -1;
    store();
  } else if (ImGui::IsItemActivated()) {
    if (on_line >= 0) dragging = on_line;
    else if (at > 0) {
      breaks.push_back(static_cast<uint32_t>(at));
      store();
    }
  }
  if (dragging >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && static_cast<size_t>(dragging) < breaks.size()) {
    breaks[static_cast<size_t>(dragging)] = static_cast<uint32_t>(std::max(1, at));
    std::string txt;
    for (uint32_t b : breaks) txt += (txt.empty() ? "" : ", ") + std::to_string(b);
    std::snprintf(d.tc_breaks, sizeof(d.tc_breaks), "%s", txt.c_str());
  } else if (dragging >= 0) {
    dragging = -1;
    store();
  }
}

// rows or columns to group: whole rows / columns selected, otherwise ask
void Outline(AppState& s, bool group) {
  if (PivotOutline(s, group)) return;  // in a PivotTable: Group Selection / Ungroup (Excel)
  const auto mode = s.FocusView().sel.mode;
  const Rect r = SelRect(s);
  if (mode == render::SelectMode::Rows || mode == render::SelectMode::Cols) {
    const bool rows = mode == render::SelectMode::Rows;
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(group ? TR("Group") : TR("Ungroup"),
                       [=](Document& d) { d.GroupOutline(sheet, rows, rows ? r.r0 : r.c0, rows ? r.r1 : r.c1, group); });
    return;
  }
  D().grp_group = group;
  D().grp_rows = r.Rows() >= r.Cols() ? 0 : 1;
  s.OpenDialog(Dialog::GroupOutline);
}

// Show Detail / Hide Detail for the group at the active cell (or the group just above / left of a summary row)
void Detail(AppState& s, bool show) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  const auto& sel = s.FocusView().sel;
  const uint32_t sheet = s.focus_sheet;
  for (bool rows : {true, false}) {
    const auto& dim = rows ? *sh->rows : *sh->cols;
    if (dim.MaxLevel() == 0) continue;
    const uint32_t i = rows ? sel.act_r : sel.act_c;
    uint32_t index = i;
    int level = dim.Level(i);
    // a summary row: the group before it (after it when the sheet's summary rows / columns sit above / left of detail)
    const bool after = rows ? sh->outline_below : sh->outline_right;
    if (after && i > 0 && dim.Level(i - 1) > level) {
      index = i - 1;
      level = level + 1;
    } else if (!after && dim.Level(i + 1) > level) {
      index = i + 1;
      level = level + 1;
    }
    if (level <= 0) continue;
    s.engine->PostUser(show ? TR("Show Detail") : TR("Hide Detail"), [=](Document& d) { d.SetGroupCollapsed(sheet, rows, index, level, !show); });
    return;
  }
}

// ---- Trace Precedents / Dependents ----

void Trace(AppState& s, bool precedents) {
  if (!s.engine || !s.snap) return;
  if (DeferWhileEngineBusy(s, [precedents](AppState& st) { Trace(st, precedents); })) return;  // the engine still works
  const uint32_t sheet = s.focus_sheet;
  const auto& sel = s.FocusView().sel;
  const Rect cell = Rect::Cell(sel.act_r, sel.act_c);
  auto& frontier = precedents ? s.audit_prec_frontier : s.audit_dep_frontier;
  auto& origin = precedents ? s.audit_prec_origin : s.audit_dep_origin;
  if (frontier.empty() || origin.first != sheet || !(origin.second == cell)) {
    frontier.assign(1, cell);
    origin = {sheet, cell};
  }
  const std::vector<Rect> from = frontier;
  std::vector<AppState::AuditArrow> arrows;
  std::vector<Rect> next;
  s.engine->Call([&](Document& d) {
    d.Recalc();
    const model::Sheet* sh = d.S(sheet);
    if (!sh) return;
    size_t budget = 500;
    // references to other sheets: one dashed arrow per cell, to / from the sheet icon (Go To lists them)
    auto other = [&](uint32_t r, uint32_t c) {
      const auto refs = precedents ? d.OtherSheetPrecedents(sheet, r, c) : d.OtherSheetDependents(sheet, r, c);
      if (refs.empty()) return;
      AppState::AuditArrow a{sheet, Rect::Cell(r, c), r, c};
      for (const auto& [sid, q] : refs) {
        const std::string sn = d.SheetName(sid);
        const std::string addr = q.IsCell() ? core::CellA1(q.r0, q.c0, true, true) : core::CellA1(q.r0, q.c0, true, true) + ":" + core::CellA1(q.r1, q.c1, true, true);
        a.other_refs.push_back(core::QuoteSheetName(sn) + "!" + addr);
      }
      arrows.push_back(std::move(a));
    };
    for (const Rect& f : from) {
      for (uint32_t r = f.r0; r <= f.r1 && budget; ++r) {
        for (uint32_t c = f.c0; c <= f.c1 && budget; ++c) {
          if (precedents) {
            if (!sh->GetFormula(r, c)) continue;
            --budget;
            other(r, c);
            for (const Rect& p : d.Precedents(sheet, Rect::Cell(r, c), true)) {
              arrows.push_back({sheet, p, r, c});
              next.push_back(p);
            }
          } else {
            --budget;
            other(r, c);
            for (const Rect& dep : d.Dependents(sheet, Rect::Cell(r, c), true)) {
              for (uint32_t rr = dep.r0; rr <= dep.r1 && rr - dep.r0 < 200; ++rr) {
                for (uint32_t cc = dep.c0; cc <= dep.c1 && cc - dep.c0 < 50; ++cc) {
                  if (!sh->GetFormula(rr, cc)) continue;
                  arrows.push_back({sheet, Rect::Cell(r, c), rr, cc});
                  next.push_back(Rect::Cell(rr, cc));
                }
              }
            }
          }
        }
      }
    }
  });
  if (arrows.empty()) {
    s.SetStatus(precedents ? TR("No more precedents to trace") : TR("No more dependents to trace"));
    return;
  }
  for (auto& a : arrows) {
    if (const model::Sheet* ash = s.snap->Sheet(a.sheet)) {
      a.to_formula = ash->GetFormula(a.to_r, a.to_c);
      a.to_text = engine::FormulaTextOf(s.snap->wb, a.sheet, a.to_r, a.to_c);
      a.rows = &*ash->rows;
      a.cols = &*ash->cols;
    }
    a.precedent = precedents;
  }
  for (const auto& a : arrows) {
    bool dup = false;
    for (const auto& b : s.audit_arrows) {
      dup = dup || (b.sheet == a.sheet && b.from == a.from && b.to_r == a.to_r && b.to_c == a.to_c && b.other_refs.empty() == a.other_refs.empty() &&
                    b.precedent == a.precedent);
    }
    if (!dup) s.audit_arrows.push_back(a);
  }
  frontier = next;
}

ImRect CellRect(const model::Sheet& sh, const render::OverlayGeom& g, const Rect& r) {
  const ImVec2 a = g.to_screen(static_cast<double>(sh.cols->Offset(r.c0)), static_cast<double>(sh.rows->Offset(r.r0)));
  const ImVec2 b = g.to_screen(static_cast<double>(sh.cols->Offset(int64_t{r.c1} + 1)), static_cast<double>(sh.rows->Offset(int64_t{r.r1} + 1)));
  return ImRect(a, b);
}

// ---- Watch Window ----

// a defined name that is exactly this cell (the Watch Window's Name column)
std::string NameOfCell(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return {};
  const std::string a = core::CellA1(r, c, true, true);
  for (const auto& n : *s.snap->wb.names) {
    if (n.hidden || (n.sheet_scope >= 0 && static_cast<uint32_t>(n.sheet_scope) != sheet)) continue;
    std::string_view t = n.refers_to;
    if (!t.empty() && t[0] == '=') t.remove_prefix(1);
    const size_t bang = t.rfind('!');
    if (bang == std::string_view::npos || t.substr(bang + 1) != a) continue;
    std::string sn(t.substr(0, bang));
    if (sn.size() >= 2 && sn.front() == '\'' && sn.back() == '\'') sn = sn.substr(1, sn.size() - 2);
    if (sn == sh->name) return n.name;
  }
  return {};
}

void AddWatches(AppState& s, uint32_t sheet, const std::vector<Rect>& areas) {
  for (const Rect& a : areas) {
    for (uint32_t r = a.r0; r <= a.r1 && s.watches.size() < 500; ++r) {
      for (uint32_t c = a.c0; c <= a.c1 && s.watches.size() < 500; ++c) {
        bool dup = false;
        for (const auto& w : s.watches) dup = dup || (w.sheet == sheet && w.r == r && w.c == c);
        if (!dup) s.watches.push_back({sheet, r, c});
      }
    }
  }
}

void DrawWatchWindow(AppState& s) {
  if (!s.show_watch || !s.snap) return;
  ImGui::SetNextWindowSize(ImVec2(S(700), S(220)), ImGuiCond_FirstUseEver);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(740), vp->WorkPos.y + vp->WorkSize.y - S(300)), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(TR("Watch Window###yc_watch"), &s.show_watch)) {
    DataUi& d = D();
    // Add Watch...: Excel's small dialog with the cells to watch (the selection, changeable on the sheet)
    if (ImGui::Button(TR("Add Watch..."))) {
      const model::Sheet* sh = s.FocusSheet();
      std::string ref;
      for (const Rect& a : SelAreas(s)) {
        if (!ref.empty()) ref += ",";
        ref += "=" + (sh ? core::QuoteSheetName(sh->name) + "!" : std::string()) + (a.IsCell() ? core::CellA1(a.r0, a.c0, true, true) : core::CellA1(a.r0, a.c0, true, true) + ":" + core::CellA1(a.r1, a.c1, true, true));
      }
      std::snprintf(d.watch_ref, sizeof(d.watch_ref), "%s", ref.c_str());
      s.OpenDialog(Dialog::AddWatch);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(d.watch_sel < 0 || d.watch_sel >= static_cast<int>(s.watches.size()));
    if (ImGui::Button(TR("Delete Watch"))) {
      s.watches.erase(s.watches.begin() + d.watch_sel);
      d.watch_sel = -1;
    }
    ImGui::EndDisabled();
    const std::string book = s.file_path.empty() ? std::string(TR("Book1")) : ToUtf8(std::filesystem::path(s.file_path).filename().wstring());
    if (ImGui::BeginTable("##watches", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn(TR("Book"));
      ImGui::TableSetupColumn(TR("Sheet"));
      ImGui::TableSetupColumn(TR("Name"));
      ImGui::TableSetupColumn(TR("Cell"));
      ImGui::TableSetupColumn(TR("Value"));
      ImGui::TableSetupColumn(TR("Formula"));
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < s.watches.size(); ++i) {
        const auto& w = s.watches[i];
        const model::Sheet* sh = s.snap->Sheet(w.sheet);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(book.c_str(), d.watch_sel == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
          d.watch_sel = static_cast<int>(i);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && sh) Goto(s, core::QuoteSheetName(sh->name) + "!" + core::CellA1(w.r, w.c));
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sh ? sh->name.c_str() : "#REF!");
        ImGui::TableNextColumn();
        const std::string wname = NameOfCell(s, w.sheet, w.r, w.c);
        ImGui::TextUnformatted(wname.c_str());
        if (!wname.empty()) ui::NoteUiText(wname.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(core::CellA1(w.r, w.c, true, true).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sh ? CellText(s, *sh, w.r, w.c).c_str() : "");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sh ? engine::FormulaTextOf(s.snap->wb, w.sheet, w.r, w.c).c_str() : "");
      }
      ImGui::EndTable();
    }
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete) && d.watch_sel >= 0 && d.watch_sel < static_cast<int>(s.watches.size())) {
      s.watches.erase(s.watches.begin() + d.watch_sel);
      d.watch_sel = -1;
    }
  }
  ImGui::End();
}

}  // namespace

// ---- The range Sort and Remove Duplicates work on ----

bool GuessHeader(AppState& s, const model::Sheet& sh, const Rect& a) {
  if (a.Rows() < 2 || !s.snap) return false;
  // Excel: the first row is a header when it is text over other kinds of values, or formatted unlike the row below
  const auto& tbl = *s.snap->wb.styles;
  bool first_text = true, any_text = false, second_nontext = false, styled = false;
  for (uint32_t c = a.c0; c <= a.c1 && c - a.c0 < 256; ++c) {
    const auto& v1 = sh.GetValue(a.r0, c);
    first_text = first_text && (v1.IsString() || v1.IsEmpty());
    any_text = any_text || v1.IsString();
    const auto& v2 = sh.GetValue(a.r0 + 1, c);
    second_nontext = second_nontext || (!v2.IsString() && !v2.IsEmpty());
    const model::Style& s1 = tbl.Get(sh.GetStyle(a.r0, c));
    const model::Style& s2 = tbl.Get(sh.GetStyle(a.r0 + 1, c));
    styled = styled || s1.bold != s2.bold || s1.italic != s2.italic || s1.underline != s2.underline || s1.fill_color != s2.fill_color ||
             s1.font_color != s2.font_color;
  }
  return first_text && any_text && (second_nontext || styled);
}

DataRange FindDataRange(AppState& s) {
  DataRange out;
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap) return out;
  const auto& v = s.FocusView();
  const Rect sel = SelRect(s);
  const uint32_t ar = v.sel.act_r, ac = v.sel.act_c;
  auto clamp_used = [&](Rect r) {
    if (sh->has_used && r.r1 >= sh->row_limit - 1) r.r1 = std::max(r.r0, sh->used.r1);
    if (sh->has_used && r.c1 >= core::kMaxCols - 1) r.c1 = std::max(r.c0, sh->used.c1);
    return r;
  };
  // a table: its data body (the header and the totals row stay where they are)
  if (const model::Table* t = model::TableAt(s.snap->wb, s.focus_sheet, ar, ac); t && t->HasData() && (sel.IsCell() || t->range.Contains(sel))) {
    out.rect = t->Data();
    out.table = t->id;
    return out;
  }
  // the AutoFilter's list (with the rows typed below it), header included
  const model::AutoFilter& af = *sh->autofilter;
  if (af.on && sel.IsCell()) {
    const Rect fr = engine::AutoFilterExtent(*sh, af.range);
    if (fr.Contains(ar, ac) && fr.r1 > fr.r0) {
      out.rect = fr;
      out.header = true;
      return out;
    }
  }
  if (sel.IsCell()) {
    out.rect = engine::CurrentRegion(*sh, sel.r0, sel.c0);
    out.header = GuessHeader(s, *sh, out.rect);
    return out;
  }
  out.rect = clamp_used(sel);
  out.header = GuessHeader(s, *sh, out.rect);
  // Sort Warning: the list around the selection is wider (data in the columns next to it)
  if (SelAreas(s).size() == 1 && sh->has_used) {
    Rect reg = engine::CurrentRegion(*sh, out.rect.r0, out.rect.c0);
    reg.r0 = std::min(reg.r0, out.rect.r0);
    reg.r1 = std::max(reg.r1, out.rect.r1);
    const bool left = out.rect.c0 > 0 && reg.c0 < out.rect.c0, right = reg.c1 > out.rect.c1;
    if (reg.Contains(out.rect) && (left || right)) {
      out.warn = true;
      out.expanded = reg;
      out.expanded_header = GuessHeader(s, *sh, reg);
    }
  }
  return out;
}

void RunDataAction(AppState& s, int action, const DataRange& r) {
  // a lone empty cell has no list around it: Excel says so instead of sorting or opening its dialog on one cell
  if (const model::Sheet* sh = s.FocusSheet(); sh && r.table == 0 && r.rect.IsCell() && sh->IsEmptyCell(r.rect.r0, r.rect.c0)) {
    s.ShowError("YCGrid", TR("This can't be applied to the selected range. Select a single cell in a range and try again."));
    return;
  }
  if (r.warn) {
    DataUi& d = D();
    d.sw_action = action;
    d.sw_range = r;
    d.sw_choice = 0;  // Expand the selection
    s.OpenDialog(Dialog::SortWarning);
    return;
  }
  switch (action) {
    case 0:
    case 1: SortDataRange(s, action == 1, r); break;
    case 2: OpenCustomSort(s, r); break;
    default: OpenRemoveDuplicates(s, r); break;
  }
}

void OpenRemoveDuplicates(AppState& s, const DataRange& t) {
  if (!s.FocusSheet()) return;
  DataUi& d = D();
  d.rd_rect = t.rect;
  if (const model::Sheet* sh = s.FocusSheet(); sh && sh->has_used) {
    d.rd_rect.r1 = std::min(d.rd_rect.r1, std::max(d.rd_rect.r0, sh->used.r1));
    d.rd_rect.c1 = std::min(d.rd_rect.c1, std::max(d.rd_rect.c0, sh->used.c1));
  }
  d.rd_cols.assign(d.rd_rect.Cols(), 1);
  d.rd_header = t.header;
  s.OpenDialog(Dialog::RemoveDuplicates);
}

// ---- Commands ----

namespace {
constexpr int kCircularMenu = 10;  // Error Checking > Circular References lists up to this many cells
void SelectCell(AppState& s, uint32_t sheet, uint32_t r, uint32_t c);
}  // namespace

// Formulas > Error Checking > Circular References: the cells of the circular references (Excel's submenu), each frame
void SyncCircularMenu(AppState& s) {
  std::vector<std::string> menu = {"ErrorChecking", "TraceError"};
  std::vector<uint64_t> cells;
  if (s.snap) {
    for (uint64_t k : s.snap->circular_cells) {
      if (std::find(cells.begin(), cells.end(), k) == cells.end()) cells.push_back(k); // lesson-ok: find-push at most kCircularMenu cells
      if (cells.size() >= static_cast<size_t>(kCircularMenu)) break;
    }
  }
  if (auto* c = ui::Commands().Find("CircularReferences")) c->enabled = !cells.empty();
  if (cells.empty()) {
    menu.push_back("CircularReferences");
  } else {
    menu.push_back(">CircularReferences");
    for (size_t i = 0; i < cells.size(); ++i) {
      const model::Sheet* sh = s.snap->Sheet(engine::KeySheet(cells[i]));
      const std::string label = (sh && engine::KeySheet(cells[i]) != s.focus_sheet ? sh->name + "!" : std::string()) +
                                core::CellA1(engine::KeyRow(cells[i]), engine::KeyCol(cells[i]), true, true);
      if (auto* c = ui::Commands().Find("CircularRef" + std::to_string(i))) c->label = label;
      menu.push_back("CircularRef" + std::to_string(i));
    }
    menu.push_back("<");
  }
  static std::vector<std::string> last;
  if (menu == last) return;
  last = menu;
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "ErrorChecking") it.menu = menu;
        }
      }
    }
  }
}

void RegisterDataCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon = nullptr, const char* shortcut = "", const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("RemoveDuplicates", TR("Remove\nDuplicates"), ui::icon::RemoveDuplicates, "", TR("Delete the rows that repeat values of earlier rows."))).execute = [&s](ui::Command&) {
    if (!s.FocusSheet()) return;
    RunDataAction(s, 3, FindDataRange(s));
  };
  R.Add(make("TextToColumns", TR("Text to\nColumns"), ui::icon::TextToColumns, "", TR("Split the text of one column into several columns."))).execute = [&s](ui::Command&) {
    if (!s.FocusSheet()) return;
    DataUi& d = D();
    d.tc_rect = SelRect(s);
    if (d.tc_rect.Cols() > 1) {
      s.ShowMessage("YCGrid", TR("Text to Columns works on one column at a time."));
      return;
    }
    if (const model::Sheet* sh = s.FocusSheet(); sh && sh->has_used) d.tc_rect.r1 = std::min(d.tc_rect.r1, std::max(d.tc_rect.r0, sh->used.r1));
    std::snprintf(d.tc_dest, sizeof(d.tc_dest), "%s", core::CellA1(d.tc_rect.r0, d.tc_rect.c0).c_str());
    d.tc_types.clear();
    d.tc_step = 0;
    d.tc_col = 0;
    d.tc_adv = false;
    d.tc_date = SystemDateOrder();
    // Excel's guess on the first page: delimited when the text has tabs, commas or semicolons, else fixed width when
    // it has spaces
    if (const model::Sheet* sh = s.FocusSheet()) {
      bool delim = false, spaces = false;
      for (uint32_t r = d.tc_rect.r0; r <= d.tc_rect.r1 && r - d.tc_rect.r0 < 200; ++r) {
        const auto& v = sh->GetValue(r, d.tc_rect.c0);
        if (!v.IsString()) continue;
        const std::string_view t = v.str();
        delim = delim || t.find_first_of("\t,;") != std::string_view::npos;
        spaces = spaces || t.find(' ') != std::string_view::npos;
      }
      d.tc_kind = !delim && spaces ? 1 : 0;
    }
    s.OpenDialog(Dialog::TextToColumns);
  };
  R.Add(make("CircleInvalidData", TR("Circle Invalid Data"), nullptr, "", TR("Circle the cells whose values break their data validation rule."))).execute = [&s](ui::Command&) {
    if (!s.FocusSheet()) return;
    CircleInvalidData(s, s.focus_sheet);
  };
  R.Add(make("ClearValidationCircles", TR("Clear Validation Circles"), nullptr, "", TR("Take away the circles drawn by Circle Invalid Data."))).execute =
      [&s](ui::Command&) { ClearValidationCircles(s, s.focus_sheet); };
  R.Add(make("GroupOutline", TR("Group"), ui::icon::Group, "Shift+Alt+Right", TR("Group the selected rows or columns so they can be collapsed and expanded."))).execute = [&s](ui::Command&) { Outline(s, true); };
  R.Add(make("UngroupOutline", TR("Ungroup"), ui::icon::Ungroup, "Shift+Alt+Left", TR("Ungroup the selected rows or columns."))).execute = [&s](ui::Command&) { Outline(s, false); };
  R.Add(make("ShowDetail", TR("Show Detail"), ui::icon::ShowDetail, "", TR("Expand the group at the active cell."))).execute = [&s](ui::Command&) { Detail(s, true); };
  R.Add(make("HideDetail", TR("Hide Detail"), ui::icon::HideDetail, "", TR("Collapse the group at the active cell."))).execute = [&s](ui::Command&) { Detail(s, false); };
  // the cell menu's Sort > Put Selected Cell (Font) Color On Top: the list's rows with the active cell's color first
  auto color_top = [&s](int sort_on) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh || !s.snap) return;
    const auto& sel = s.FocusView().sel;
    const model::Style& st = s.snap->wb.styles->Get(sh->GetStyle(sel.act_r, sel.act_c));
    const uint32_t c = sort_on == 1 ? st.fill_color : st.font_color;
    DataRange t = FindDataRange(s);
    if (t.warn) t.rect = t.expanded, t.header = t.expanded_header;
    engine::SortKey key{std::clamp(sel.act_c, t.rect.c0, t.rect.c1), false};
    key.sort_on = sort_on;
    key.color = (c >> 24) ? c : 0u;
    const uint32_t sheet = s.focus_sheet;
    const Rect r = t.rect;
    const bool header = t.header;
    s.engine->PostUser(TR("Sort"), [sheet, r, key, header](Document& d) { d.Sort(sheet, r, {key}, header); });
  };
  R.Add(make("SortCellColorTop", TR("Put Selected Cell Color On Top"), nullptr, "", TR("Sort the list so the rows with this cell's fill color come first."))).execute =
      [color_top](ui::Command&) { color_top(1); };
  R.Add(make("SortFontColorTop", TR("Put Selected Font Color On Top"), nullptr, "", TR("Sort the list so the rows with this cell's font color come first."))).execute =
      [color_top](ui::Command&) { color_top(2); };
  // Put Selected Cell Icon On Top: the rows whose cell shows the active cell's conditional formatting icon first
  R.Add(make("SortIconTop", TR("Put Selected Cell Icon On Top"), nullptr, "", TR("Sort the list so the rows with this cell's icon come first."))).execute =
      [&s](ui::Command&) {
        const model::Sheet* sh = s.FocusSheet();
        if (!sh || !s.snap) return;
        const auto& sel = s.FocusView().sel;
        engine::CondFormatEval ev(s.snap->wb, s.focus_sheet);
        engine::CfVisual vis;
        if (ev.Empty() || !ev.Eval(sel.act_r, sel.act_c, &vis) || vis.icon < 0) return;  // the cell shows no icon
        DataRange t = FindDataRange(s);
        if (t.warn) t.rect = t.expanded, t.header = t.expanded_header;
        engine::SortKey key{std::clamp(sel.act_c, t.rect.c0, t.rect.c1), false};
        key.sort_on = 3;
        key.icon_set = vis.icon_style;
        key.icon = vis.icon;
        const uint32_t sheet = s.focus_sheet;
        const Rect r = t.rect;
        const bool header = t.header;
        s.engine->PostUser(TR("Sort"), [sheet, r, key, header](Document& d) { d.Sort(sheet, r, {key}, header); });
      };
  R.Add(make("AutoOutline", TR("Auto Outline"), nullptr, "", TR("Group the rows and columns that SUM and SUBTOTAL formulas add up."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    Rect area = SelRect(s);
    if (area.IsCell() && sh->has_used) area = sh->used;  // one cell: the whole sheet's data, as Excel
    const uint32_t sheet = s.focus_sheet;
    const int n = s.engine->Call([&](Document& d) {
      d.Begin(TR("Auto Outline"));
      const int k = d.AutoOutline(sheet, area);
      d.Commit();
      return k;
    });
    if (!n) s.ShowError("YCGrid", TR("Cannot create an outline."));
  };
  R.Add(make("OutlineSettings", TR("Outline Settings"), nullptr, "", TR("Where the summary rows and columns are, and automatic styles."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    DataUi& d = D();
    d.ol_below = sh->outline_below;
    d.ol_right = sh->outline_right;
    d.ol_styles = sh->outline_styles;
    s.OpenDialog(Dialog::OutlineSettings);
  };
  R.Add(make("ClearOutline", TR("Clear Outline"), nullptr, "", TR("Remove every group of the sheet."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Clear Outline"), [sheet](Document& d) {
      d.ClearOutline(sheet, true);
      d.ClearOutline(sheet, false);
    });
  };
  R.Add(make("TracePrecedents", TR("Trace Precedents"), ui::icon::TracePrecedents, "", TR("Show arrows from the cells that affect the active cell's value (click again for the next level)."))).execute = [&s](ui::Command&) { Trace(s, true); };
  R.Add(make("TraceDependents", TR("Trace Dependents"), ui::icon::TraceDependents, "", TR("Show arrows to the formulas that use the active cell (click again for the next level)."))).execute = [&s](ui::Command&) { Trace(s, false); };
  R.Add(make("RemoveArrows", TR("Remove Arrows"), ui::icon::RemoveArrows, "", TR("Remove the tracer arrows."))).execute = [&s](ui::Command&) {
    s.audit_arrows.clear();
    s.audit_prec_frontier.clear();
    s.audit_dep_frontier.clear();
  };
  // Remove Arrows' split menu, as in Excel: only the arrows Trace Precedents or Trace Dependents drew
  auto remove_kind = [&s](bool prec) {
    return [&s, prec](ui::Command&) {
      auto& arr = s.audit_arrows;
      arr.erase(std::remove_if(arr.begin(), arr.end(), [prec](const AppState::AuditArrow& a) { return a.precedent == prec; }), arr.end());
      (prec ? s.audit_prec_frontier : s.audit_dep_frontier).clear();
    };
  };
  R.Add(make("RemovePrecedentArrows", TR("Remove Precedent Arrows"), nullptr, "", TR("Remove the arrows Trace Precedents drew."))).execute = remove_kind(true);
  R.Add(make("RemoveDependentArrows", TR("Remove Dependent Arrows"), nullptr, "", TR("Remove the arrows Trace Dependents drew."))).execute = remove_kind(false);
  // Formulas > Error Checking: the dialog that walks the errors; its menu has Trace Error and the circular references
  R.Add(make("ErrorChecking", TR("Error Checking"), ui::icon::Warning, "", TR("Check for common errors that occur in formulas."))).execute = [&s](ui::Command&) { OpenErrorChecking(s); };
  R.Add(make("TraceError", TR("Trace Error"), nullptr, "", TR("Show arrows from the cells that cause the active cell's error."))).execute = [&s](ui::Command&) { TraceError(s); };
  R.Add(make("CircularReferences", TR("Circular References"), nullptr, "", TR("Go to a cell that is part of a circular reference."))).execute = [&s](ui::Command&) {
    if (s.snap && !s.snap->circular_cells.empty()) {
      const uint64_t k = s.snap->circular_cells.front();
      SelectCell(s, engine::KeySheet(k), engine::KeyRow(k), engine::KeyCol(k));
    }
  };
  for (int i = 0; i < kCircularMenu; ++i) {
    R.Add(make(("CircularRef" + std::to_string(i)).c_str(), "")).execute = [&s, i](ui::Command&) {
      if (!s.snap || static_cast<size_t>(i) >= s.snap->circular_cells.size()) return;
      const uint64_t k = s.snap->circular_cells[static_cast<size_t>(i)];
      SelectCell(s, engine::KeySheet(k), engine::KeyRow(k), engine::KeyCol(k));
    };
  }
  // Ctrl+[ / Ctrl+] (with Shift: every level): select the cells the selection's formulas use, or the formulas that use it
  auto select_audit = [&s](bool prec, bool direct) {
    return [&s, prec, direct](ui::Command&) {
      const model::Sheet* sh = s.FocusSheet();
      if (!sh || !s.engine || s.edit.active) return;
      const uint32_t sid = s.focus_sheet;
      const Rect src = SelRect(s);
      std::vector<Rect> areas = s.engine->Call([&](engine::Document& d) { return prec ? d.Precedents(sid, src, direct) : d.Dependents(sid, src, direct); });
      if (areas.empty()) {
        s.SetStatus(TR("No cells were found."));
        return;
      }
      if (areas.size() > 10000) areas.resize(10000);
      for (Rect& a : areas) a.r1 = std::min(a.r1, sh->row_limit - 1);
      // cells side by side are one area, as Excel selects them (Ctrl+[ on =B3*C3 selects B3:C3, measured)
      core::TidyAreas(&areas);
      if (areas.empty()) return;
      auto& view = s.FocusView();
      // the active cell is the first area's corner; the active area is the last one, so the first goes last
      std::rotate(areas.begin(), areas.begin() + 1, areas.end());
      view.sel.areas = areas;
      view.sel.anc_r = view.sel.act_r = areas.back().r0;
      view.sel.anc_c = view.sel.act_c = areas.back().c0;
      view.sel.mode = render::SelectMode::Cells;
      render::EnsureVisible(*sh, view, view.sel.act_r, view.sel.act_c);
    };
  };
  R.Add(make("SelectPrecedents", TR("Select Direct Precedents"), nullptr, "Ctrl+[")).execute = select_audit(true, true);
  R.Add(make("SelectAllPrecedents", TR("Select All Precedents"), nullptr, "Ctrl+Shift+{")).execute = select_audit(true, false);
  R.Add(make("SelectDependents", TR("Select Direct Dependents"), nullptr, "Ctrl+]")).execute = select_audit(false, true);
  R.Add(make("SelectAllDependents", TR("Select All Dependents"), nullptr, "Ctrl+Shift+}")).execute = select_audit(false, false);
  R.Add(make("EvaluateFormula", TR("Evaluate\nFormula"), ui::icon::EvaluateFormula, "", TR("Evaluate the active cell's formula one step at a time."))).execute = [&s](ui::Command&) {
    const auto& sel = s.FocusView().sel;
    DataUi& d = D();
    d.ev_sheet = s.focus_sheet;
    d.ev_r = sel.act_r;
    d.ev_c = sel.act_c;
    d.ev_steps = s.engine->Call([&](Document& doc) { return doc.EvaluateSteps(d.ev_sheet, d.ev_r, d.ev_c); });
    d.ev_pos = 0;
    d.ev_inner.clear();
    if (d.ev_steps.empty()) {
      s.ShowMessage("YCGrid", TR("The active cell doesn't contain a formula."));
      return;
    }
    s.OpenDialog(Dialog::EvaluateFormula);
  };
  R.Add(make("WatchWindow", TR("Watch\nWindow"), ui::icon::WatchWindow, "", TR("Keep an eye on the values of cells while you work elsewhere."))).execute = [&s](ui::Command&) {
    s.show_watch = !s.show_watch;
  };
  RegisterPane("watch", "WatchWindow", [&s]() -> bool& { return s.show_watch; });
}

// ---- Dialogs ----

void DrawDataDialogs(AppState& s) {
  DataUi& d = D();
  // Sort Warning (also before Remove Duplicates): data next to the selection
  if (BeginDlg(s, Dialog::SortWarning, d.sw_action == 3 ? TR("Remove Duplicates Warning###yc_dlg_sortwarn") : TR("Sort Warning###yc_dlg_sortwarn"), 440)) {
    ImGui::PushTextWrapPos(S(420));
    ImGui::TextWrapped("%s", d.sw_action == 3 ? TR("YCGrid found data next to your selection. Since you have not selected this data, it will not be "
                                                   "taken into account when removing duplicates.")
                                              : TR("YCGrid found data next to your selection. Since you have not selected this data, it will not be sorted."));
    ImGui::PopTextWrapPos();
    ImGui::TextUnformatted(TR("What do you want to do?"));
    ImGui::RadioButton(TR("Expand the selection"), &d.sw_choice, 0);
    ImGui::RadioButton(TR("Continue with the current selection"), &d.sw_choice, 1);
    // Up / Down choose between the two, as in Excel's option group
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d.sw_choice = 1;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d.sw_choice = 0;
    }
    bool close = false;
    if (DlgOkButton(d.sw_action == 3 ? TR("Remove Duplicates...") : TR("Sort"), ImVec2(S(140), 0))) {
      DataRange r = d.sw_range;
      r.warn = false;
      if (d.sw_choice == 0) {
        r.rect = r.expanded;
        r.header = r.expanded_header;
        auto& sv = s.FocusView().sel;  // the active cell stays: its column is the sort key
        sv.areas.assign(1, r.rect);
        sv.anc_r = r.rect.r0;
        sv.anc_c = r.rect.c0;
      }
      const int action = d.sw_action;
      EndDlg(s, true);
      RunDataAction(s, action, r);
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Remove Duplicates
  if (BeginDlg(s, Dialog::RemoveDuplicates, TR("Remove Duplicates###yc_dlg_rmdup"), 420)) {
    const model::Sheet* sh = s.FocusSheet();
    const Rect r = d.rd_rect;
    ImGui::TextWrapped("%s", TR("To delete duplicate values, select one or more columns that contain duplicates."));
    if (ImGui::Button(TR("Select All"))) std::fill(d.rd_cols.begin(), d.rd_cols.end(), 1);
    ImGui::SameLine();
    if (ImGui::Button(TR("Unselect All"))) std::fill(d.rd_cols.begin(), d.rd_cols.end(), 0);
    ImGui::SameLine();
    ImGui::Checkbox(TR("My data has headers"), &d.rd_header);
    if (ImGui::BeginChild("##cols", ImVec2(S(380), S(160)), ImGuiChildFlags_Borders)) {
      for (uint32_t c = r.c0; c <= r.c1 && c - r.c0 < d.rd_cols.size(); ++c) {
        std::string name = TR("Column ") + core::ColumnName(c);
        if (d.rd_header && sh) {
          const std::string h = CellText(s, *sh, r.r0, c);
          if (!h.empty()) name = h;
        }
        bool on = d.rd_cols[c - r.c0] != 0;
        ImGui::PushID(static_cast<int>(c));
        if (name.find("##") == std::string::npos) {
          if (ImGui::Checkbox(name.c_str(), &on)) d.rd_cols[c - r.c0] = on ? 1 : 0;
        } else {
          // the header's text as it is ("C##1"): an id-only box and the text beside it
          if (ImGui::Checkbox("##col", &on)) d.rd_cols[c - r.c0] = on ? 1 : 0;
          ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
          ImGui::TextUnformatted(name.c_str());
          ui::NoteUiText(name.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
          if (ImGui::IsItemClicked()) d.rd_cols[c - r.c0] = on ? 0 : 1;
        }
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    bool close = false;
    std::vector<uint32_t> cols;
    for (uint32_t i = 0; i < d.rd_cols.size(); ++i) {
      if (d.rd_cols[i]) cols.push_back(r.c0 + i);
    }
    ImGui::BeginDisabled(cols.empty());
    const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
    if (ok_pressed && RefuseReadOnlyUi(s)) {
      close = true;  // read-only mode came on while the dialog was open: the status bar says why, not "No duplicate values found."
    } else if (ok_pressed) {
      const uint32_t sheet = s.focus_sheet;
      const bool header = d.rd_header;
      const uint32_t total = r.r1 - r.r0 + 1 - (header ? 1 : 0);
      const uint32_t removed = s.engine->Call([&](Document& doc) {
        doc.Begin(TR("Remove Duplicates"));
        const uint32_t n = doc.RemoveDuplicates(sheet, r, cols, header);
        doc.Commit();
        return n;
      });
      s.ShowMessage("YCGrid", removed ? std::to_string(removed) + TR(" duplicate values found and removed; ") + std::to_string(total - removed) + TR(" unique values remain.")
                                     : std::string(TR("No duplicate values found.")));
      close = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Text to Columns: Excel's three-step wizard (data type, delimiters or break lines, column data format)
  if (BeginDlg(s, Dialog::TextToColumns, (std::string(TR("Convert Text to Columns Wizard - Step ")) + std::to_string(d.tc_step + 1) + TR(" of 3") + "###yc_dlg_t2c").c_str(), 600)) {
    const model::Sheet* sh = s.FocusSheet();
    const engine::TextToColumnsOptions o = TcOptions();
    static const char* kDateOrders[] = {"MDY", "DMY", "YMD", "MYD", "DYM", "YDM"};
    auto type_name = [](int t) -> std::string {
      if (t == 1) return TR("Text");
      if (t == 2) return TR("Skip column");
      if (t >= 3 && t <= 8) return kDateOrders[t - 3];
      return TR("General");
    };
    if (d.tc_step == 0) {
      ImGui::TextWrapped("%s", d.tc_kind == 0 ? TR("The Text Wizard has determined that your data is Delimited.")
                                              : TR("The Text Wizard has determined that your data is Fixed Width."));
      ImGui::TextWrapped("%s", TR("If this is correct, choose Next, or choose the data type that best describes your data."));
      ImGui::SeparatorText(TR("Original data type"));
      ImGui::TextUnformatted(TR("Choose the file type that best describes your data:"));
      if (DlgKey(ImGuiKey_D)) d.tc_kind = 0;  // Excel's letters: (D)elimited, Fixed (W)idth
      if (DlgKey(ImGuiKey_W)) d.tc_kind = 1;
      ImGui::RadioButton(TR("Delimited"), &d.tc_kind, 0);
      ImGui::SameLine(S(150));
      ImGui::TextDisabled("%s", TR("- Characters such as commas or tabs separate each field."));
      ImGui::RadioButton(TR("Fixed width"), &d.tc_kind, 1);
      ImGui::SameLine(S(150));
      ImGui::TextDisabled("%s", TR("- Fields are aligned in columns with spaces between each field."));
      ImGui::SeparatorText(TR("Preview of selected data:"));
      if (ImGui::BeginChild("##t2c_raw", ImVec2(S(570), S(150)), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (uint32_t r = d.tc_rect.r0; sh && r <= d.tc_rect.r1 && r - d.tc_rect.r0 < 50; ++r) {
          ImGui::TextDisabled("%u", r + 1);
          ImGui::SameLine(S(40));
          ImGui::TextUnformatted(CellText(s, *sh, r, d.tc_rect.c0).c_str());
        }
      }
      ImGui::EndChild();
    } else if (d.tc_step == 1 && d.tc_kind == 0) {
      ImGui::TextWrapped("%s", TR("This screen lets you set the delimiters your data contains. You can see how your text is affected in the preview below."));
      ImGui::SeparatorText(TR("Delimiters"));
      // Excel's letters: (T)ab, Se(m)icolon, (C)omma, (S)pace, (O)ther, T(r)eat consecutive delimiters as one
      if (DlgKey(ImGuiKey_T)) d.tc_tab = !d.tc_tab;
      if (DlgKey(ImGuiKey_M)) d.tc_semi = !d.tc_semi;
      if (DlgKey(ImGuiKey_C)) d.tc_comma = !d.tc_comma;
      if (DlgKey(ImGuiKey_S)) d.tc_space = !d.tc_space;
      if (DlgKey(ImGuiKey_O)) d.tc_other = !d.tc_other;
      if (DlgKey(ImGuiKey_R)) d.tc_consec = !d.tc_consec;
      ImGui::Checkbox(TR("Tab"), &d.tc_tab);
      ImGui::Checkbox(TR("Semicolon"), &d.tc_semi);
      ImGui::Checkbox(TR("Comma"), &d.tc_comma);
      ImGui::Checkbox(TR("Space"), &d.tc_space);
      ImGui::Checkbox(TR("Other:"), &d.tc_other);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(30));
      if (ImGui::InputText("##other", d.tc_other_ch, 2) && d.tc_other_ch[0]) d.tc_other = true;  // typing a character ticks Other, as in Excel
      ImGui::Checkbox(TR("Treat consecutive delimiters as one"), &d.tc_consec);
      static const char* kQual[] = {"\"", "'", TR("{none}")};
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Text qualifier:"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(90));
      ui::NoteUiItem("##t2c_qual", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(90), ImGui::GetFrameHeight())));
      ui::Combo("##t2c_qual", &d.tc_qual, kQual, 3);
    } else if (d.tc_step == 1) {
      ImGui::TextWrapped("%s", TR("This screen lets you set field widths (column breaks)."));
      DrawBreakRuler(s, d);
    } else {
      ImGui::TextWrapped("%s", TR("This screen lets you select each column and set the Data Format."));
      ImGui::SeparatorText(TR("Column data format"));
    }
    // the split preview (steps 2 and 3); in step 3 a click on a column chooses it
    std::vector<std::vector<std::string>> preview;
    size_t width = 1;
    for (uint32_t r = d.tc_rect.r0; sh && r <= d.tc_rect.r1 && preview.size() < 50; ++r) {
      const auto& v = sh->GetValue(r, d.tc_rect.c0);
      preview.push_back(v.IsString() ? engine::SplitColumns(std::string(v.str()), o) : std::vector<std::string>{CellText(s, *sh, r, d.tc_rect.c0)});
      width = std::max(width, preview.back().size());
    }
    width = std::min<size_t>(width, 64);
    if (d.tc_types.size() < width) d.tc_types.resize(width, 0);
    d.tc_col = std::clamp(d.tc_col, 0, static_cast<int>(width) - 1);
    if (d.tc_step == 2) {
      int& t = d.tc_types[static_cast<size_t>(d.tc_col)];
      int kind = t >= 3 && t <= 8 ? 3 : (t == 2 ? 2 : (t == 1 ? 1 : 0));  // the option buttons' order: General, Text, Date, skip
      ImGui::BeginGroup();
      if (ImGui::RadioButton(TR("General"), kind == 0)) t = 0;
      if (ImGui::RadioButton(TR("Text"), kind == 1)) t = 1;
      if (ImGui::RadioButton(TR("Date:"), kind == 3)) t = d.tc_date;
      ImGui::SameLine();
      int order = (kind == 3 ? t : d.tc_date) - 3;
      ImGui::SetNextItemWidth(S(80));
      ui::NoteUiItem("##t2c_date", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(80), ImGui::GetFrameHeight())));
      if (ui::Combo("##t2c_date", &order, kDateOrders, 6)) {
        d.tc_date = order + 3;
        t = d.tc_date;
      }
      if (ImGui::RadioButton(TR("Do not import column (skip)"), kind == 2)) t = 2;
      ImGui::EndGroup();
      ImGui::SameLine(S(300));
      ImGui::BeginGroup();
      ImGui::TextWrapped("%s", TR("'General' converts numeric values to numbers, date values to dates, and all remaining values to text."));
      if (ImGui::Button(TR("Advanced..."))) d.tc_adv = !d.tc_adv;
      ImGui::EndGroup();
      if (d.tc_adv) {
        // Advanced Text Import Settings: how General columns read numbers
        static const char* kSeps[] = {".", ",", "'", TR("(space)")};
        ImGui::SeparatorText(TR("Settings used to recognize numeric data"));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Decimal separator:"));
        ImGui::SameLine(S(170));
        ImGui::SetNextItemWidth(S(80));
        ui::NoteUiItem("##t2c_dec", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(80), ImGui::GetFrameHeight())));
        ui::Combo("##t2c_dec", &d.tc_dec, kSeps, 4);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Thousands separator:"));
        ImGui::SameLine(S(170));
        ImGui::SetNextItemWidth(S(80));
        ui::NoteUiItem("##t2c_th", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(80), ImGui::GetFrameHeight())));
        ui::Combo("##t2c_th", &d.tc_th, kSeps, 4);
        ImGui::Checkbox(TR("Trailing minus for negative numbers"), &d.tc_trail);
        if (ImGui::Button(TR("Reset"))) {
          d.tc_dec = 0;
          d.tc_th = 1;
          d.tc_trail = true;
        }
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Destination:"));
      ImGui::SameLine();
      RangeBox(s, "ttc.dest", d.tc_dest, sizeof(d.tc_dest), S(200), RangeBox_Cell, TR("Destination"));
    }
    if (d.tc_step > 0) {
      ImGui::SeparatorText(TR("Data preview"));
      if (ImGui::BeginTable("##t2c", static_cast<int>(width), ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY, ImVec2(S(570), d.tc_adv && d.tc_step == 2 ? S(110) : S(170)))) {
        ImGui::TableSetupScrollFreeze(0, d.tc_step == 2 ? 1 : 0);
        if (d.tc_step == 2) {  // each column's format above it, as Excel's preview shows
          ImGui::TableNextRow();
          for (size_t j = 0; j < width; ++j) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(j));
            const std::string h = type_name(d.tc_types[j]) + "##hdr";
            if (ImGui::Selectable(h.c_str(), d.tc_col == static_cast<int>(j))) d.tc_col = static_cast<int>(j);
            ImGui::PopID();
          }
        }
        for (size_t r = 0; r < preview.size(); ++r) {
          ImGui::TableNextRow();
          for (size_t j = 0; j < width; ++j) {
            ImGui::TableNextColumn();
            const std::string cell = j < preview[r].size() ? preview[r][j] : std::string();
            if (d.tc_step == 2) {
              ImGui::PushID(static_cast<int>(r * 64 + j));
              const bool skip = d.tc_types[j] == 2;
              if (skip) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
              if (ImGui::Selectable("##c", d.tc_col == static_cast<int>(j))) d.tc_col = static_cast<int>(j);
              ui::NoteUiText(cell.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));  // ui.items finds it by its text
              ImGui::SameLine(0, 0);
              ImGui::TextUnformatted(cell.c_str());
              if (skip) ImGui::PopStyleColor();
              ImGui::PopID();
            } else {
              ImGui::TextUnformatted(cell.c_str());
            }
          }
        }
        ImGui::EndTable();
      }
    }
    ImGui::Separator();
    bool close = false, finish = false;
    if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(d.tc_step == 0);
    if (ImGui::Button(TR("< Back"), ImVec2(S(90), 0))) --d.tc_step;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (d.tc_step < 2) {
      if (DlgOkButton(TR("Next >"), ImVec2(S(90), 0))) ++d.tc_step;  // Enter goes on, as in Excel
      ImGui::SameLine();
      if (ImGui::Button(TR("Finish"), ImVec2(S(90), 0))) finish = true;
    } else {
      ImGui::BeginDisabled();
      ImGui::Button(TR("Next >"), ImVec2(S(90), 0));
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (DlgOkButton(TR("Finish"), ImVec2(S(90), 0))) finish = true;
    }
    if (finish) {
      core::CellPos dest{d.tc_rect.r0, d.tc_rect.c0};
      std::string dt = d.tc_dest;
      if (const size_t bang = dt.rfind('!'); bang != std::string::npos) dt = dt.substr(bang + 1);
      dt.erase(std::remove(dt.begin(), dt.end(), '$'), dt.end());
      if (const size_t colon = dt.find(':'); colon != std::string::npos) dt = dt.substr(0, colon);  // a range: its first cell
      if (!dt.empty() && !core::ParseCellA1(dt, &dest)) {
        s.ShowError("YCGrid", TR("The destination isn't a valid cell."));
      } else {
        const uint32_t sheet = s.focus_sheet;
        const Rect src = d.tc_rect;
        auto run = [&s, sheet, src, dest, o]() {
          s.engine->PostUser(TR("Text to Columns"), [=](Document& doc) { doc.TextToColumns(sheet, src, dest.row, dest.col, o); });
        };
        // Excel asks before writing over cells that hold data (the source column itself excepted)
        bool filled = false;
        if (sh) {
          uint32_t out_w = 1;
          for (uint32_t r = src.r0; r <= src.r1 && r - src.r0 < 100000; ++r) {
            const auto& v = sh->GetValue(r, src.c0);
            if (!v.IsString()) continue;
            const auto parts = engine::SplitColumns(std::string(v.str()), o);
            uint32_t n = 0;
            for (size_t j = 0; j < parts.size(); ++j) n += j < o.types.size() && o.types[j] == 9 ? 0 : 1;
            out_w = std::max(out_w, n);
          }
          for (uint32_t i = 0; i <= src.r1 - src.r0 && !filled; ++i) {
            for (uint32_t j = 0; j < out_w && !filled; ++j) {
              const uint32_t r = dest.row + i, c = dest.col + j;
              if (c == src.c0 && r >= src.r0 && r <= src.r1) continue;
              filled = c < core::kMaxCols && !sh->GetValue(r, c).IsEmpty();
            }
          }
        }
        EndDlg(s, true);
        if (filled) AskYesNo(s, "YCGrid", TR("There's already data here. Do you want to replace it?"), run);
        else run();
        return;
      }
    }
    EndDlg(s, close);
  }
  // Data > Outline settings
  if (BeginDlg(s, Dialog::OutlineSettings, TR("Settings###yc_dlg_outline"), 300)) {
    ImGui::TextUnformatted(TR("Direction"));
    ImGui::Checkbox(TR("Summary rows below detail"), &d.ol_below);
    ImGui::Checkbox(TR("Summary columns to right of detail"), &d.ol_right);
    ImGui::Checkbox(TR("Automatic styles"), &d.ol_styles);
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const uint32_t sheet = s.focus_sheet;
      const bool below = d.ol_below, right = d.ol_right, styles = d.ol_styles;
      s.engine->PostUser(TR("Outline Settings"), [=](Document& doc) { doc.SetOutlineSettings(sheet, below, right, styles); });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Group / Ungroup when cells (not whole rows or columns) are selected
  if (BeginDlg(s, Dialog::GroupOutline, d.grp_group ? TR("Group###yc_dlg_group") : TR("Ungroup###yc_dlg_group"), 240)) {
    ImGui::RadioButton(TR("Rows"), &d.grp_rows, 0);
    ImGui::RadioButton(TR("Columns"), &d.grp_rows, 1);
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const Rect r = SelRect(s);
      const bool rows = d.grp_rows == 0, group = d.grp_group;
      const uint32_t sheet = s.focus_sheet;
      s.engine->PostUser(group ? TR("Group") : TR("Ungroup"), [=](Document& doc) { doc.GroupOutline(sheet, rows, rows ? r.r0 : r.c0, rows ? r.r1 : r.c1, group); });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Add Watch: the cells to watch (the selection when it opens; picked on the sheet with the box's button)
  if (BeginDlg(s, Dialog::AddWatch, TR("Add Watch###yc_dlg_addwatch"), 420)) {
    ImGui::TextUnformatted(TR("Select the cells that you would like to watch the value of:"));
    RangeBox(s, "watch.add", d.watch_ref, sizeof(d.watch_ref), ImGui::GetContentRegionAvail().x, RangeBox_Equals | RangeBox_Sheet,
             TR("Select the cells that you would like to watch the value of:"));
    bool close = false;
    if (ImGui::Button(TR("Add"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      // each area of the box: Sheet!A1:B2 (the active sheet when no sheet is named)
      std::string t = d.watch_ref;
      size_t pos = 0;
      bool ok = true;
      std::vector<std::pair<uint32_t, Rect>> areas;
      while (pos <= t.size() && ok) {
        size_t comma = t.find(',', pos);
        std::string part = t.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? t.size() + 1 : comma + 1;
        while (!part.empty() && (part[0] == '=' || part[0] == ' ')) part.erase(0, 1);
        if (part.empty()) continue;
        uint32_t sid = s.focus_sheet;
        std::string addr = part;
        if (const size_t bang = part.rfind('!'); bang != std::string::npos) {
          std::string sn = part.substr(0, bang);
          if (sn.size() >= 2 && sn.front() == '\'' && sn.back() == '\'') sn = sn.substr(1, sn.size() - 2);
          const int32_t i = s.snap->wb.FindSheet(sn);
          if (i < 0) {
            ok = false;
            break;
          }
          sid = s.snap->wb.sheets[static_cast<size_t>(i)]->id;
          addr = part.substr(bang + 1);
        }
        Rect r;
        if (!core::ParseRectA1(addr, &r)) {
          ok = false;
          break;
        }
        areas.emplace_back(sid, r);
      }
      if (!ok || areas.empty()) {
        s.ShowError("YCGrid", TR("The reference isn't valid."));
      } else {
        for (const auto& [sid, r] : areas) AddWatches(s, sid, {r});
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Evaluate Formula: Evaluate, Step In / Step Out (a box per cell stepped into), Close, as Excel's dialog
  if (BeginDlg(s, Dialog::EvaluateFormula, TR("Evaluate Formula###yc_dlg_evalf"), 560)) {
    DataUi::EvFrame outer{d.ev_sheet, d.ev_r, d.ev_c, d.ev_steps, d.ev_pos};
    auto frame_at = [&](size_t i) -> DataUi::EvFrame& { return i == 0 ? outer : d.ev_inner[i - 1]; };
    const size_t depth = d.ev_inner.size();
    for (size_t i = 0; i <= depth; ++i) {
      DataUi::EvFrame& f = frame_at(i);
      if (f.steps.empty()) continue;
      const model::Sheet* sh = s.snap ? s.snap->Sheet(f.sheet) : nullptr;
      const std::string ref_line = std::string(TR("Reference: ")) + (sh ? core::QuoteSheetName(sh->name) + "!" : std::string()) + core::CellA1(f.r, f.c, true, true);
      ImGui::TextUnformatted(ref_line.c_str());
      ui::NoteUiText(ref_line.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
      if (i == 0) ImGui::TextUnformatted(TR("Evaluation:"));
      const engine::EvalStep& st = f.steps[std::min(f.pos, f.steps.size() - 1)];
      ImGui::PushID(static_cast<int>(i));
      const bool active = i == depth;
      if (ImGui::BeginChild("##eval", ImVec2(S(530), i == 0 ? S(90) : S(44)), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        const std::string& t = st.text;
        const size_t a0 = std::min(st.hl_begin, t.size()), b0 = std::min(a0 + st.hl_len, t.size());
        if (!active) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(t.c_str(), t.c_str() + a0);
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(t.c_str() + a0, t.c_str() + b0);
        if (b0 > a0) {
          // the part evaluated next is underlined (like Excel)
          const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
          ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), mx, ImGui::GetColorU32(ImGuiCol_Text), std::max(1.0f, S(1)));
        }
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(t.c_str() + b0);
        if (!active) ImGui::PopStyleColor();
      }
      ImGui::EndChild();
      ImGui::PopID();
    }
    DataUi::EvFrame& cur = frame_at(depth);
    const bool last = cur.steps.empty() || cur.pos + 1 >= cur.steps.size();
    const bool constant = depth > 0 && cur.steps.size() == 1;
    const engine::EvalStep* st = cur.steps.empty() ? nullptr : &cur.steps[std::min(cur.pos, cur.steps.size() - 1)];
    ImGui::TextDisabled("%s", constant ? TR("The cell currently being evaluated contains a constant.")
                              : last   ? TR("This is the result of the formula.")
                                       : TR("To show the result of the underlined expression, click Evaluate."));
    bool close = false;
    // Evaluate on the innermost box; at the end of the outer formula it starts again (Restart)
    ImGui::BeginDisabled(constant);
    if (ImGui::Button(last && depth == 0 ? TR("Restart") : TR("Evaluate"), ImVec2(S(100), 0))) {
      if (!last) ++cur.pos;
      else if (depth == 0) cur.pos = 0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Step In: the underlined part is a reference to one cell; its formula opens in a box below
    ImGui::BeginDisabled(!st || st->in_sheet < 0 || last || d.ev_inner.size() >= 16);
    if (ImGui::Button(TR("Step In"), ImVec2(S(90), 0)) && st && st->in_sheet >= 0) {
      DataUi::EvFrame in;
      in.sheet = static_cast<uint32_t>(st->in_sheet);
      in.r = st->in_r;
      in.c = st->in_c;
      in.steps = s.engine->Call([&](Document& doc) { return doc.EvaluateSteps(in.sheet, in.r, in.c, true); });
      if (!in.steps.empty()) {
        if (depth == 0) {
          d.ev_pos = outer.pos;
        }
        d.ev_inner.push_back(std::move(in));
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Step Out: back to the box above, with the reference replaced by its value
    ImGui::BeginDisabled(depth == 0);
    if (ImGui::Button(TR("Step Out"), ImVec2(S(90), 0)) && depth > 0) {
      d.ev_inner.pop_back();
      DataUi::EvFrame& up = d.ev_inner.empty() ? outer : d.ev_inner.back();
      if (up.pos + 1 < up.steps.size()) ++up.pos;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Close"), ImVec2(S(80), 0))) close = true;
    d.ev_pos = outer.pos;
    if (close) d.ev_inner.clear();
    EndDlg(s, close);
  }
}

// ---- Circle Invalid Data ----

namespace {
struct ValidationCircle {
  uint32_t sheet, r, c;
};
std::vector<ValidationCircle> g_circles;
uint64_t g_circles_checked = 0;  // the snapshot version the circles were last checked against

bool BreaksItsRule(AppState& s, const model::Sheet& sh, uint32_t r, uint32_t c) {
  const model::DvRule* rule = engine::FindValidation(sh, r, c);
  const core::Value& v = sh.GetValue(r, c);
  if (!rule || v.IsEmpty()) return false;
  const std::string text = v.IsString() ? std::string(v.str()) : engine::DisplayTextOf(v, "General");
  std::string why;
  return !engine::ValidateInput(s.snap->wb, sh.id, r, c, text, *rule, &why);
}
}  // namespace

int CircleInvalidData(AppState& s, uint32_t sheet_id) {
  ClearValidationCircles(s, sheet_id);
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh || !sh->has_used) return 0;
  int n = 0;
  for (const auto& rule : *sh->validations) {
    for (const Rect& q : rule.ranges) {
      const Rect w{q.r0, q.c0, std::min(q.r1, sh->used.r1), std::min(q.c1, sh->used.c1)};
      for (uint32_t r = w.r0; r <= w.r1 && n < 255; ++r) {
        for (uint32_t c = w.c0; c <= w.c1 && n < 255; ++c) {
          if (!BreaksItsRule(s, *sh, r, c)) continue;
          g_circles.push_back({sheet_id, r, c});
          ++n;
        }
      }
    }
  }
  g_circles_checked = s.snap->version;
  return n;
}

void ClearValidationCircles(AppState&, uint32_t sheet_id) {
  std::erase_if(g_circles, [&](const ValidationCircle& x) { return sheet_id == 0 || x.sheet == sheet_id; });
}

int ValidationCircleCount(uint32_t sheet_id) {
  return static_cast<int>(std::count_if(g_circles.begin(), g_circles.end(), [&](const ValidationCircle& x) { return x.sheet == sheet_id; }));
}

void DrawValidationCircles(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (g_circles.empty() || !s.snap) return;
  // a value corrected (or its rule removed) loses its circle, as in Excel
  if (g_circles_checked != s.snap->version) {
    g_circles_checked = s.snap->version;
    std::erase_if(g_circles, [&](const ValidationCircle& x) {
      const model::Sheet* sh = s.snap->Sheet(x.sheet);
      return !sh || !BreaksItsRule(s, *sh, x.r, x.c);
    });
  }
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  for (const auto& x : g_circles) {
    if (x.sheet != sheet_id || sh->rows->IsHidden(x.r) || sh->cols->IsHidden(x.c)) continue;
    const ImRect cell = CellRect(*sh, g, Rect::Cell(x.r, x.c));
    dl->AddEllipse(cell.GetCenter(), ImVec2(cell.GetWidth() * 0.5f + S(4), cell.GetHeight() * 0.5f + S(3)), IM_COL32(0xE0, 0x10, 0x10, 255), 0.0f, 0,
                   std::max(1.5f, S(1.5f)));
  }
  dl->PopClipRect();
}

// ---- Tracer arrows ----

void DrawAuditOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  DrawValidationCircles(s, sheet_id, g);
  if (s.audit_arrows.empty() || !s.snap) return;
  // the traced formula changed, or rows / columns were inserted or deleted: the arrows would point at the wrong cells
  uint64_t last_to = ~0ull;
  for (const auto& a : s.audit_arrows) {
    const model::Sheet* ash = s.snap->Sheet(a.sheet);
    const uint64_t to = engine::Key(a.sheet, a.to_r, a.to_c);
    const bool same_text = to == last_to || engine::FormulaTextOf(s.snap->wb, a.sheet, a.to_r, a.to_c) == a.to_text;
    last_to = to;
    if (!ash || &*ash->rows != a.rows || &*ash->cols != a.cols || ash->GetFormula(a.to_r, a.to_c) != a.to_formula || !same_text) {
      s.audit_arrows.clear();
      s.audit_prec_frontier.clear();
      s.audit_dep_frontier.clear();
      return;
    }
  }
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  const ImU32 blue_c = IM_COL32(0x00, 0x4F, 0xE0, 255);
  const float w = std::max(1.0f, S(1.5f));
  const ImU32 red = IM_COL32(0xE0, 0x10, 0x10, 255);
  const ImU32 black = IM_COL32(0x20, 0x20, 0x20, 255);
  for (size_t ai = 0; ai < s.audit_arrows.size(); ++ai) {
    const auto& a = s.audit_arrows[ai];
    if (a.sheet != sheet_id || a.other_refs.empty()) continue;
    // another sheet: a dashed black arrow between the cell and a worksheet icon up and to the left of it (Excel)
    const ImRect cell = CellRect(*sh, g, Rect::Cell(a.to_r, a.to_c));
    const ImVec2 pc(cell.Min.x + std::min(S(10), cell.GetWidth() * 0.3f), cell.GetCenter().y);
    const float iw = S(16), ih = S(13);
    const ImVec2 ic(std::max(g.cells.Min.x + iw, pc.x - S(70)), std::max(g.cells.Min.y + ih, pc.y - S(36)));
    const ImRect icon(ImVec2(ic.x - iw * 0.5f, ic.y - ih * 0.5f), ImVec2(ic.x + iw * 0.5f, ic.y + ih * 0.5f));
    const ImVec2 p0 = a.precedent ? ImVec2(icon.Max.x, icon.GetCenter().y) : pc, p1 = a.precedent ? pc : ImVec2(icon.Max.x, icon.GetCenter().y);
    const float len = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
    if (len > 1) {
      const ImVec2 u((p1.x - p0.x) / len, (p1.y - p0.y) / len), n(-u.y, u.x);
      for (float t = 0; t < len; t += S(8)) {
        const float t1 = std::min(len, t + S(4.5f));
        dl->AddLine(ImVec2(p0.x + u.x * t, p0.y + u.y * t), ImVec2(p0.x + u.x * t1, p0.y + u.y * t1), black, w);
      }
      const float h = std::max(6.0f, S(8)), hw = h * 0.45f;
      dl->AddTriangleFilled(p1, ImVec2(p1.x - u.x * h + n.x * hw, p1.y - u.y * h + n.y * hw), ImVec2(p1.x - u.x * h - n.x * hw, p1.y - u.y * h - n.y * hw), black);
    }
    if (!a.precedent) dl->AddCircleFilled(pc, std::max(2.5f, S(3)), black);
    // the worksheet icon: a small grid
    dl->AddRectFilled(icon.Min, icon.Max, IM_COL32_WHITE);
    dl->AddRect(icon.Min, icon.Max, black);
    for (int k = 1; k < 3; ++k) {
      const float y = icon.Min.y + ih * static_cast<float>(k) / 3.0f, x = icon.Min.x + iw * static_cast<float>(k) / 3.0f;
      dl->AddLine(ImVec2(icon.Min.x, y), ImVec2(icon.Max.x, y), IM_COL32(0x80, 0x80, 0x80, 255));
      dl->AddLine(ImVec2(x, icon.Min.y), ImVec2(x, icon.Max.y), IM_COL32(0x80, 0x80, 0x80, 255));
    }
    // a double-click on the icon (or the arrow's end at it) opens Go To with the references
    ImGui::PushID(static_cast<int>(ai));
    ImGui::SetCursorScreenPos(ImVec2(icon.Min.x - S(3), icon.Min.y - S(3)));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(TR("Trace to another sheet##yc_trace_sheet"), ImVec2(iw + S(6), ih + S(6)));
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", TR("Double-click to go to the cells on the other sheet."));
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenGoToWith(s, a.other_refs);
    }
    ImGui::PopID();
  }
  for (const auto& a : s.audit_arrows) {
    if (a.sheet != sheet_id || !a.other_refs.empty()) continue;
    // Excel: an arrow from a cell holding an error value is red
    const ImU32 blue = a.from.IsCell() && sh->GetValue(a.from.r0, a.from.c0).IsError() ? red : blue_c;
    const ImRect from = CellRect(*sh, g, a.from);
    const ImRect to = CellRect(*sh, g, Rect::Cell(a.to_r, a.to_c));
    if (!a.from.IsCell()) dl->AddRect(from.Min, from.Max, blue, 0.0f, w);  // a precedent range gets a box
    const ImRect first = CellRect(*sh, g, Rect::Cell(a.from.r0, a.from.c0));
    const ImVec2 p0(first.Min.x + std::min(S(10), first.GetWidth() * 0.3f), first.GetCenter().y);
    const ImVec2 p1(to.Min.x + std::min(S(10), to.GetWidth() * 0.3f), to.GetCenter().y);
    dl->AddCircleFilled(p0, std::max(2.5f, S(3)), blue);
    dl->AddLine(p0, p1, blue, w);
    const float len = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
    if (len > 1) {
      const ImVec2 u((p1.x - p0.x) / len, (p1.y - p0.y) / len), n(-u.y, u.x);
      const float h = std::max(6.0f, S(8)), hw = h * 0.45f;
      dl->AddTriangleFilled(p1, ImVec2(p1.x - u.x * h + n.x * hw, p1.y - u.y * h + n.y * hw), ImVec2(p1.x - u.x * h - n.x * hw, p1.y - u.y * h - n.y * hw), blue);
    }
  }
  dl->PopClipRect();
}

// ---- Background error checking: the error options button ----

engine::ErrorCheckOptions ErrorCheckingOptions() {
  const Settings& c = Config();
  engine::ErrorCheckOptions o;
  o.background = c.error_checking;
  o.evaluate_to_error = c.err_evaluate_to_error;
  o.text_date = c.err_text_date;
  o.number_as_text = c.err_number_as_text;
  o.inconsistent_formula = c.err_inconsistent_formula;
  o.omitted_cells = c.err_omitted_cells;
  o.unlocked_formula = c.err_unlocked_formula;
  o.empty_cell_reference = c.err_empty_cell_reference;
  o.list_data_validation = c.err_list_data_validation;
  o.inconsistent_list_formula = c.err_inconsistent_list_formula;
  o.misleading_format = c.err_misleading_format;
  return o;
}

namespace {

// Inconsistent Formula: Excel offers the formula of the neighbors that agree (above / below, else left / right)
bool CopyFromAbove(const model::Sheet& sh, uint32_t r, uint32_t c) {
  if (r == 0) return false;
  const uint32_t up = sh.GetFormula(r - 1, c);
  return up && up == sh.GetFormula(r + 1, c);
}

// the text date with its two-digit year written with four digits (19XX or 20XX)
std::string FourDigitYear(std::string_view t, bool twentieth) {
  size_t last = std::string_view::npos;
  for (size_t i = 0; i < t.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(t[i])) && (i == 0 || !std::isdigit(static_cast<unsigned char>(t[i - 1])))) last = i;
  }
  std::string out(t);
  if (last != std::string_view::npos) out.insert(last, twentieth ? "19" : "20");
  return out;
}

// what fixes the rule: the first entries of the error options menu, and the first button of the Error Checking dialog
struct ErrorFix {
  std::string label;
  int id;  // 1 calculation steps, 2 obstructing cells, 3 to 20XX, 4 to 19XX, 5 to number, 6 copy above, 7 copy left,
           // 8 include cells, 9 lock cell, 10 trace empty cell, 11 restore column formula, 12 validation rule,
           // 13 update format
};

std::vector<ErrorFix> ErrorFixes(const model::Workbook& wb, const model::Sheet& sh, uint32_t r, uint32_t c, int rule) {
  std::vector<ErrorFix> out;
  switch (rule) {
    case engine::kErrEvaluateToError:
      if (sh.GetValue(r, c).IsError() && sh.GetValue(r, c).error() == core::Err::Spill) out.push_back({TR("Select Obstructing Cells"), 2});
      out.push_back({TR("Show Calculation Steps..."), 1});
      break;
    case engine::kErrTextDate:
      out.push_back({TR("Convert XX to 20XX"), 3});
      out.push_back({TR("Convert XX to 19XX"), 4});
      break;
    case engine::kErrNumberAsText: out.push_back({TR("Convert to Number"), 5}); break;
    case engine::kErrInconsistentFormula:
      if (CopyFromAbove(sh, r, c)) out.push_back({TR("Copy Formula from Above"), 6});
      else out.push_back({TR("Copy Formula from Left"), 7});
      break;
    case engine::kErrOmittedCells: out.push_back({TR("Update Formula to Include Cells"), 8}); break;
    case engine::kErrUnlockedFormula: out.push_back({TR("Lock Cell"), 9}); break;
    case engine::kErrEmptyCellReference: out.push_back({TR("Trace Empty Cell"), 10}); break;
    case engine::kErrListDataValidation: out.push_back({TR("Display Type Information..."), 12}); break;
    case engine::kErrInconsistentListFormula: out.push_back({TR("Restore to Calculated Column Formula"), 11}); break;
    case engine::kErrMisleadingFormat: {
      // Update Format to Currency (the format of the cells the formula reads)
      int kind = 0;
      if (engine::MisleadingFormat(wb, sh, r, c, nullptr, &kind)) out.push_back({std::string(TR("Update Format to ")) + engine::FormatKindName(kind), 13});
      break;
    }
    default: break;
  }
  return out;
}

void SelectCell(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  if (sheet != s.focus_sheet) ActivateSheet(s, sheet);
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  auto& v = s.FocusView();
  render::SelectRange(v, Rect::Cell(r, c));
  if (sh) render::EnsureVisible(*sh, v, r, c);
}

void RunErrorFix(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, int id, const std::vector<Rect>& areas) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return;
  const engine::ErrorCheckOptions opts = ErrorCheckingOptions();
  switch (id) {
    case 1: ui::Commands().Execute("EvaluateFormula"); break;
    case 2: ui::Commands().Execute("SelectObstructingCells"); break;
    case 3:
    case 4: {
      // the selection's text dates with their four-digit year, entered as typed (a date)
      std::vector<std::tuple<uint32_t, uint32_t, std::string>> out;
      for (const Rect& a : areas) {
        for (uint32_t rr = a.r0; rr <= a.r1 && rr - a.r0 < 100000 && out.size() < 100000; ++rr) {
          for (uint32_t cc = a.c0; cc <= a.c1 && cc - a.c0 < 16384; ++cc) {
            if (engine::ErrorCheckRule(s.snap->wb, *sh, rr, cc, opts, false) != engine::kErrTextDate) continue;
            out.emplace_back(rr, cc, FourDigitYear(sh->GetValue(rr, cc).str(), id == 4));
          }
        }
      }
      s.engine->PostUser(id == 4 ? TR("Convert XX to 19XX") : TR("Convert XX to 20XX"), [sheet, out](Document& d) {
        for (const auto& [rr, cc, t] : out) d.SetInput(sheet, rr, cc, t);
      });
      break;
    }
    case 5: {
      // the numbers stored as text in the selection become numbers (their text read as typed)
      std::vector<std::tuple<uint32_t, uint32_t, core::Value>> out;
      for (const Rect& a : areas) {
        for (uint32_t rr = a.r0; rr <= a.r1 && rr - a.r0 < 100000 && out.size() < 1000000; ++rr) {
          for (uint32_t cc = a.c0; cc <= a.c1 && cc - a.c0 < 16384; ++cc) {
            if (engine::ErrorCheckRule(s.snap->wb, *sh, rr, cc, opts, false) != engine::kErrNumberAsText) continue;
            out.emplace_back(rr, cc, formula::ParseInput(sh->GetValue(rr, cc).str()).value);
          }
        }
      }
      s.engine->PostUser(TR("Convert to Number"), [sheet, out](Document& d) {
        for (const auto& [rr, cc, v] : out) d.SetValue(sheet, rr, cc, v);
      });
      break;
    }
    case 6:
      if (r > 0) s.engine->PostUser(TR("Copy Formula from Above"), [sheet, r, c](Document& d) { d.FillDown(sheet, Rect{r - 1, c, r, c}); });
      break;
    case 7:
      if (c > 0) s.engine->PostUser(TR("Copy Formula from Left"), [sheet, r, c](Document& d) { d.FillRight(sheet, Rect{r, c - 1, r, c}); });
      break;
    case 8:
      s.engine->PostUser(TR("Update Formula to Include Cells"), [sheet, r, c](Document& d) {
        const std::string text = engine::OmittedCellsFormulaText(d, sheet, r, c);
        if (text.size() > 1) d.SetFormula(sheet, r, c, std::string_view(text).substr(1));  // without "="
      });
      break;
    case 9:
      s.engine->PostUser(TR("Lock Cell"), [sheet, areas](Document& d) {
        for (const Rect& a : areas) d.ModifyStyle(sheet, a, [](model::Style& st) { st.locked = true; });
      });
      break;
    case 10: {
      // Excel: an arrow from each empty cell the formula refers to
      std::vector<Rect> empties = s.engine->Call([&](Document& d) {
        std::vector<Rect> e;
        for (const Rect& p : d.Precedents(sheet, Rect::Cell(r, c), true)) {
          if (p.IsCell() && sh->IsEmptyCell(p.r0, p.c0)) e.push_back(p);
        }
        return e;
      });
      for (const Rect& p : empties) {
        AppState::AuditArrow a{sheet, p, r, c};
        a.to_formula = sh->GetFormula(r, c);
        a.to_text = engine::FormulaTextOf(s.snap->wb, sheet, r, c);
        a.rows = &*sh->rows;
        a.cols = &*sh->cols;
        s.audit_arrows.push_back(a);
      }
      break;
    }
    case 11: {
      // the formula of the cells around it in the column
      const model::Table* t = model::TableAt(s.snap->wb, sheet, r, c);
      if (!t) break;
      const Rect d = t->Data();
      uint32_t src = UINT32_MAX;
      for (uint32_t rr = d.r0; rr <= d.r1 && rr - d.r0 < 100000; ++rr) {
        if (rr != r && sh->GetFormula(rr, c)) {
          src = rr;
          break;
        }
      }
      if (src == UINT32_MAX) break;
      s.engine->PostUser(TR("Restore to Calculated Column Formula"), [sheet, r, c, src](Document& doc) {
        const engine::ClipData clip = doc.Copy(sheet, Rect::Cell(src, c));
        doc.Paste(sheet, r, c, clip, engine::PasteMode::Formulas);
      });
      break;
    }
    case 13: {
      uint32_t fmt = 0;
      if (!engine::MisleadingFormat(s.snap->wb, *sh, r, c, &fmt, nullptr)) break;
      const std::string code = s.snap->wb.styles->Str(fmt);
      s.engine->PostUser(TR("Update Format"), [sheet, areas, code](Document& d) {
        const uint32_t id = d.wb_mut().styles.mut().InternString(code);
        for (const Rect& a : areas) d.ModifyStyle(sheet, a, [id](model::Style& st) { st.numfmt = id; });
      });
      break;
    }
    case 12: {
      const model::DvRule* rule = engine::FindValidation(*sh, r, c);
      std::string msg = TR("The value in this cell doesn't meet the data validation rule of the cell.");
      if (rule && !rule->error_message.empty()) msg += "\n\n" + rule->error_message;
      if (rule && !rule->input_message.empty()) msg += "\n\n" + rule->input_message;
      s.ShowMessage(TR("Display Type Information"), msg);
      break;
    }
    default: break;
  }
}

// Help on this Error: what the error means, as Excel's help page on it starts
void ShowErrorHelp(AppState& s, int rule, const core::Value& v) {
  s.ShowMessage(engine::ErrorRuleTitle(rule, v), engine::ErrorRuleHelp(rule, v));
}

}  // namespace

// Excel: a cell with a green triangle, selected, gets a button beside it naming the rule, with what can be done
void DrawErrorCheckButton(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (!s.snap || s.edit.active || sheet_id != s.focus_sheet) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh || sh->chart_sheet) return;
  const auto& sel = s.FocusView().sel;
  const engine::ErrorCheckOptions opts = ErrorCheckingOptions();
  const int rule = engine::ErrorCheckRule(s.snap->wb, *sh, sel.act_r, sel.act_c, opts);
  static int menu_rule = 0;
  const ImGuiID popup = ImHashStr("##yc_errcheck_menu");
  if (rule == engine::kErrNone && !ImGui::IsPopupOpen(popup, 0)) return;
  const ImRect cell = CellRect(*sh, g, Rect::Cell(sel.act_r, sel.act_c));
  const float bs = S(18);
  // to the left of the cell, as in Excel (to its right when the cell is at the window's left edge)
  float bx = cell.Min.x - bs - S(2);
  if (bx < g.cells.Min.x) bx = cell.Max.x + S(2);
  const ImRect b(ImVec2(bx, cell.Min.y), ImVec2(bx + bs, cell.Min.y + bs));
  const core::Value& v = sh->GetValue(sel.act_r, sel.act_c);
  if (rule != engine::kErrNone && g.cells.Contains(b.GetCenter())) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(b.Min);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton(TR("Error Options##yc_errbtn"), b.GetSize());
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(b.Min, b.Max, hov ? IM_COL32(0xE5, 0xF1, 0xFB, 255) : IM_COL32(0xFF, 0xFF, 0xFF, 255));
    dl->AddRect(b.Min, b.Max, IM_COL32(0xA0, 0xA0, 0xA0, 255));
    // a yellow diamond with "!"
    const ImVec2 c = b.GetCenter();
    const float d = bs * 0.36f;
    dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), IM_COL32(0xF2, 0xC8, 0x11, 255));
    dl->AddLine(ImVec2(c.x, c.y - d * 0.5f), ImVec2(c.x, c.y + d * 0.15f), IM_COL32(0x20, 0x20, 0x20, 255), std::max(1.0f, S(1.5f)));
    dl->AddCircleFilled(ImVec2(c.x, c.y + d * 0.45f), std::max(1.0f, S(1.0f)), IM_COL32(0x20, 0x20, 0x20, 255));
    if (hov) ImGui::SetTooltip("%s", engine::ErrorRuleHelp(rule, v));
    if (clicked) {
      menu_rule = rule;
      ImGui::OpenPopup(popup);
    }
    ImGui::SetNextWindowPos(ImVec2(b.Min.x, b.Max.y));
  }
  if (ImGui::BeginPopupEx(popup, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
    const int r = std::clamp(menu_rule, 0, engine::kErrRuleCount - 1);
    ImGui::TextDisabled("%s", engine::ErrorRuleTitle(r, v));
    ui::NoteUiText(engine::ErrorRuleTitle(r, v), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    ImGui::Separator();
    const uint32_t sheet = sheet_id, ar = sel.act_r, ac = sel.act_c;
    const std::vector<Rect> areas = SelAreas(s);
    for (const ErrorFix& f : ErrorFixes(s.snap->wb, *sh, ar, ac, r)) {
      if (ImGui::MenuItem(f.label.c_str())) RunErrorFix(s, sheet, ar, ac, f.id, areas);
    }
    if (ImGui::MenuItem(TR("Help on this Error"))) ShowErrorHelp(s, r, v);
    if (r == engine::kErrEvaluateToError && ImGui::MenuItem(TR("Trace Error"))) ui::Commands().Execute("TraceError");
    if (ImGui::MenuItem(TR("Ignore Error"))) {
      s.engine->PostUser(TR("Ignore Error"), [sheet, areas](Document& d) {
        for (const Rect& a : areas) d.SetIgnoreErrors(sheet, a, true);
      });
    }
    if (ImGui::MenuItem(TR("Edit in Formula Bar"))) StartEdit(s, false, {}, true);
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Error Checking Options..."))) OpenFormulaOptions(s);
    ImGui::EndPopup();
  }
}

// ---- Formulas > Error Checking: the dialog that walks the errors of the sheet; Trace Error ----

namespace {
struct ErrorWalk {
  bool open = false;
  bool started = false;   // a cell was shown (Next / Previous go from it)
  bool resume = false;    // Edit in Formula Bar: the dialog waits with a Resume button
  bool done = false;      // "The error check is complete for the entire sheet."
  uint32_t sheet = 0, r = 0, c = 0;
};
ErrorWalk g_walk;

bool WalkTo(AppState& s, bool forward) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(g_walk.sheet) : nullptr;
  if (!sh) return false;
  uint32_t r = 0, c = 0;
  const engine::ErrorCheckOptions o = ErrorCheckingOptions();
  // from the cell shown last; the first time from just before the active cell (it is checked too)
  uint32_t fr = g_walk.r, fc = g_walk.c;
  bool found;
  if (!g_walk.started) {
    found = engine::ErrorCheckRule(s.snap->wb, *sh, fr, fc, o) != engine::kErrNone;
    if (found) r = fr, c = fc;
    else found = engine::NextErrorCell(s.snap->wb, *sh, fr, fc, forward, o, &r, &c);
    // Excel goes on from the top of the sheet when it reaches the end
    if (!found) found = engine::NextErrorCell(s.snap->wb, *sh, 0, 0, true, o, &r, &c) || engine::ErrorCheckRule(s.snap->wb, *sh, 0, 0, o) != engine::kErrNone;
  } else {
    found = engine::NextErrorCell(s.snap->wb, *sh, fr, fc, forward, o, &r, &c);
  }
  if (!found) {
    g_walk.done = true;
    return false;
  }
  g_walk.started = true;
  g_walk.r = r;
  g_walk.c = c;
  SelectCell(s, g_walk.sheet, r, c);
  return true;
}
}  // namespace

void OpenErrorChecking(AppState& s) {
  if (!s.snap || !s.FocusSheet()) return;
  const auto& sel = s.FocusView().sel;
  g_walk = ErrorWalk{};
  g_walk.sheet = s.focus_sheet;
  g_walk.r = sel.act_r;
  g_walk.c = sel.act_c;
  if (!WalkTo(s, true)) {
    s.ShowMessage("YCGrid", TR("The error check is complete for the entire sheet."));
    return;
  }
  g_walk.open = true;
}

void DrawErrorCheckingWindow(AppState& s) {
  if (!g_walk.open || !s.snap) return;
  const model::Sheet* sh = s.snap->Sheet(g_walk.sheet);
  if (!sh) {
    g_walk.open = false;
    return;
  }
  ImGui::SetNextWindowSize(ImVec2(S(560), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (ImGui::Begin(TR("Error Checking###yc_errorchecking"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)) open = false;
    const engine::ErrorCheckOptions o = ErrorCheckingOptions();
    const int rule = engine::ErrorCheckRule(s.snap->wb, *sh, g_walk.r, g_walk.c, o);
    const core::Value& v = sh->GetValue(g_walk.r, g_walk.c);
    if (g_walk.done) {
      ImGui::TextUnformatted(TR("The error check is complete for the entire sheet."));
      ui::NoteUiText(TR("The error check is complete for the entire sheet."), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
      if (ImGui::Button(TR("OK"), ImVec2(S(90), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) open = false;
    } else if (g_walk.resume) {
      // Edit in Formula Bar: the user edits the cell; Resume goes on from it
      ImGui::TextUnformatted(TR("Edit the cell in the formula bar, then click Resume."));
      if (ImGui::Button(TR("Resume"), ImVec2(S(120), 0)) && !s.edit.active) {
        g_walk.resume = false;
        if (engine::ErrorCheckRule(s.snap->wb, *sh, g_walk.r, g_walk.c, o) == engine::kErrNone) WalkTo(s, true);
      }
    } else {
      {
        char where[64];
        std::snprintf(where, sizeof(where), TR("Error in cell %s"), core::CellA1(g_walk.r, g_walk.c).c_str());
        ImGui::TextUnformatted(where);
        ui::NoteUiText(where, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
      }
      const std::string text = sh->GetFormula(g_walk.r, g_walk.c) ? engine::FormulaTextOf(s.snap->wb, g_walk.sheet, g_walk.r, g_walk.c)
                                                                    : engine::InputTextOf(s.snap->wb, g_walk.sheet, g_walk.r, g_walk.c, Config().r1c1);
      ImGui::TextUnformatted(text.c_str());
      ImGui::Dummy(ImVec2(1, S(4)));
      if (rule == engine::kErrNone) {
        ImGui::TextDisabled("%s", TR("This cell no longer has an error. Click Next to go on."));
      } else {
        ImGui::TextUnformatted(engine::ErrorRuleTitle(rule, v));
        ui::NoteUiText(engine::ErrorRuleTitle(rule, v), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(300));
        ImGui::TextDisabled("%s", engine::ErrorRuleHelp(rule, v));
        ImGui::PopTextWrapPos();
      }
      // the buttons on the right, as in Excel: the fix, Help, Ignore, Edit in Formula Bar
      ImGui::SameLine(S(340));
      ImGui::BeginGroup();
      const float bw = S(200);
      const std::vector<Rect> one{Rect::Cell(g_walk.r, g_walk.c)};
      if (rule != engine::kErrNone) {
        for (const ErrorFix& f : ErrorFixes(s.snap->wb, *sh, g_walk.r, g_walk.c, rule)) {
          if (ImGui::Button((f.label + "##ecfix").c_str(), ImVec2(bw, 0))) RunErrorFix(s, g_walk.sheet, g_walk.r, g_walk.c, f.id, one);
        }
        if (ImGui::Button(TR("Help on this Error"), ImVec2(bw, 0))) ShowErrorHelp(s, rule, v);
        if (ImGui::Button(TR("Ignore Error"), ImVec2(bw, 0))) {
          const uint32_t sheet = g_walk.sheet, r = g_walk.r, c = g_walk.c;
          s.engine->Call([&](Document& d) {
            d.Begin(TR("Ignore Error"));
            d.SetIgnoreErrors(sheet, Rect::Cell(r, c), true);
            d.Commit();
          });
          WalkTo(s, true);
        }
        if (ImGui::Button(TR("Edit in Formula Bar"), ImVec2(bw, 0))) {
          SelectCell(s, g_walk.sheet, g_walk.r, g_walk.c);
          StartEdit(s, false, {}, true);
          g_walk.resume = true;
        }
      }
      ImGui::EndGroup();
      ImGui::Separator();
      if (ImGui::Button(TR("Options..."), ImVec2(S(90), 0))) OpenFormulaOptions(s);
      ImGui::SameLine(0, S(160));
      if (ImGui::Button(TR("Previous"), ImVec2(S(90), 0)) && !WalkTo(s, false)) g_walk.done = false;  // nothing before: stay
      ImGui::SameLine();
      if (ImGui::Button(TR("Next"), ImVec2(S(90), 0))) WalkTo(s, true);
    }
  }
  ImGui::End();
  if (!open) g_walk = ErrorWalk{};
}

// Trace Error: arrows back from the active cell's error to the cell where it starts (red), and from that cell's
// precedents (blue), as Excel draws them
void TraceError(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap) return;
  const auto& sel = s.FocusView().sel;
  const uint32_t sheet = s.focus_sheet;
  if (!sh->GetValue(sel.act_r, sel.act_c).IsError() || !sh->GetFormula(sel.act_r, sel.act_c)) {
    s.ShowMessage("YCGrid", TR("The active cell doesn't contain an error value."));
    return;
  }
  std::vector<AppState::AuditArrow> arrows;
  s.engine->Call([&](Document& d) {
    std::vector<core::CellPos> todo{core::CellPos{sel.act_r, sel.act_c}};
    std::set<uint64_t> seen;
    while (!todo.empty() && arrows.size() < 500) {
      const core::CellPos p = todo.back();
      todo.pop_back();
      if (!seen.insert(engine::Key(sheet, p.row, p.col)).second) continue;
      const std::vector<Rect> prec = d.Precedents(sheet, Rect::Cell(p.row, p.col), true);
      bool error_in = false;
      for (const Rect& q : prec) {
        if (q.IsCell() && sh->GetValue(q.r0, q.c0).IsError()) {
          error_in = true;
          arrows.push_back({sheet, q, p.row, p.col});
          if (sh->GetFormula(q.r0, q.c0)) todo.push_back(core::CellPos{q.r0, q.c0});
        }
      }
      // where the error starts: the cells its formula reads
      if (!error_in) {
        for (const Rect& q : prec) arrows.push_back({sheet, q, p.row, p.col});
      }
    }
  });
  if (arrows.empty()) {
    s.SetStatus(TR("No precedents to trace"));
    return;
  }
  for (auto& a : arrows) {
    a.to_formula = sh->GetFormula(a.to_r, a.to_c);
    a.to_text = engine::FormulaTextOf(s.snap->wb, a.sheet, a.to_r, a.to_c);
    a.rows = &*sh->rows;
    a.cols = &*sh->cols;
    bool dup = false;
    for (const auto& b : s.audit_arrows) dup = dup || (b.sheet == a.sheet && b.from == a.from && b.to_r == a.to_r && b.to_c == a.to_c);
    if (!dup) s.audit_arrows.push_back(a);
  }
}

// rows / columns inserted or deleted since the last frame: the watched cells move with them (a deleted one goes)
void FollowStructureChanges(AppState& s, const engine::Snapshot& snap) {
  if (!snap.shift_log || snap.shift_log->empty()) return;
  for (const engine::StructShift& x : *snap.shift_log) {
    if (x.seq <= s.struct_seq_seen) continue;
    s.struct_seq_seen = x.seq;
    for (size_t i = s.watches.size(); i-- > 0;) {
      auto& w = s.watches[i];
      if (w.sheet != x.sheet) continue;
      uint32_t& main = x.rows ? w.r : w.c;
      const uint32_t other = x.rows ? w.c : w.r;
      if (x.has_band && (other < (x.rows ? x.band.c0 : x.band.r0) || other > (x.rows ? x.band.c1 : x.band.r1))) continue;
      if (main < x.at) continue;
      if (x.delta < 0 && int64_t{main} < int64_t{x.at} - x.delta) {
        s.watches.erase(s.watches.begin() + static_cast<std::ptrdiff_t>(i));
        continue;
      }
      main = static_cast<uint32_t>(int64_t{main} + x.delta);
    }
  }
}

void DrawDataWindows(AppState& s) {
  if (s.snap) FollowStructureChanges(s, *s.snap);
  DrawWatchWindow(s);
  DrawErrorCheckingWindow(s);
}

}  // namespace yc::app
