// SPDX-License-Identifier: Apache-2.0
// rules_ui.cpp - UI for AutoFilter, data validation and conditional formatting (matches Excel)
//
// - filter buttons and validation drop-down arrows are drawn in the grid overlay (they get the mouse first)
// - filter menu: sort, clear, search, checked values, custom criteria
// - data validation is checked on input (Stop / Warning / Information)
// - ribbon commands and dialogs: highlight rules, top/bottom N, data bars, color scales, formula rules, rules manager, data validation
#include "app_state.h"
#include "core/i18n.h"
#include "core/utf8.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "engine/tile_iter.h"
#include "formula/datetime.h"
#include "formula/functions.h"
#include "formula/numfmt.h"
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
#include <map>
#include <set>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

const ImGuiID kFilterPopup = ImHashStr("##yc_filter_popup");
const ImGuiID kListPopup = ImHashStr("##yc_dvlist_popup");

struct FilterUi {
  uint32_t sheet = 0, field = 0;
  uint32_t table = 0;  // a table's own AutoFilter (0 = the sheet's)
  ImVec2 pos;
  std::vector<std::string> values;
  std::vector<char> checked;
  std::vector<double> serials;  // each value's number: a date column's list is grouped Year > Month > Day, as Excel's
  bool blanks_present = false, blanks_checked = true;
  char search[128] = {};
  bool focus_search = false;  // E pressed in the menu: the search box takes the keyboard
  // while searching (Excel): the results are ticked on their own, and OK shows only them (or adds them to the filter)
  std::string searched;
  std::vector<char> found_checked;
  bool add_current = false;
  int op1 = 0, op2 = 0;
  char v1[128] = {}, v2[128] = {};
  bool or_op = false;
  bool custom = false;
  // the column's kind of data (which filter submenu to show) and its colors
  bool col_num = false, col_date = false;
  std::vector<uint32_t> fills, fonts;  // distinct ARGB colors (0 = no fill / automatic)
  std::vector<int> icons;  // conditional formatting icons shown in the column (icon set << 8 | icon; -1 = No Cell Icon)
  // Top 10 dialog
  int t_top = 0, t_count = 10, t_pct = 0;
};

struct ListUi {
  uint32_t sheet = 0, r = 0, c = 0;
  ImVec2 pos;
  float width = 0;
  std::vector<std::string> items;
  int hl = -1;  // the item the keyboard is on (Up / Down, Enter chooses)
};

struct RulesUi {
  FilterUi filter;
  ListUi list;
  // quick conditional formatting dialog
  // 1 greater than, 2 less than, 3 between, 4 equal to, 5 text contains, 6 duplicates, 7 top 10, 8 bottom 10, 9 above
  // average, 10 below average, 11 top 10%, 12 bottom 10%, 13 a date occurring
  int cf_kind = 0;
  char cf_v1[256] = {}, cf_v2[256] = {};
  int cf_preset = 0;
  int cf_dupe = 0;    // Duplicate Values: 0 duplicate, 1 unique
  int cf_period = 1;  // A Date Occurring: Excel's XlTimePeriods (yesterday first, as Excel's dialog)
  // the Rules Manager edits a copy of the sheet's rules: OK / Apply write it in one step, Cancel drops it
  std::vector<model::CfRule> cfm_rules;
  bool cfm_loaded = false, cfm_dirty = false;
  uint32_t cfm_sheet = 0, cfm_next_id = 0x80000000u;  // rules made in the manager get ids from here until applied
  // formula rule
  char cf_formula[512] = {};
  // New / Edit Formatting Rule
  model::CfRule cfe;
  uint32_t cfe_id = 0;  // the rule being edited (0 = new)
  int cfe_kind = 0;     // Excel's rule types, 0..5
  bool cfe_from_manager = false;
  bool cfe_reopen = false;  // Format... is open: the editor comes back when Format Cells closes
  std::string cfe_f1, cfe_f2;
  int cfm_scope = 0;  // the manager: 0 current selection, 1 this worksheet
  float cf_fill[3] = {1.0f, 0.78f, 0.81f};
  float cf_font[3] = {0.61f, 0.0f, 0.02f};
  bool cf_use_fill = true, cf_use_font = true, cf_bold = false;
  // data validation
  model::DvRule dv;
  char dv_f1[512] = {}, dv_f2[512] = {}, dv_it[128] = {}, dv_im[512] = {}, dv_et[128] = {}, dv_em[512] = {};
  // Apply these changes to all other cells with the same settings: the cells of the rule the dialog opened on
  std::vector<Rect> dv_same_cells;
  bool dv_same = false;
  int dv_tab = 0;
};

RulesUi& R() {
  static RulesUi r;
  return r;
}

constexpr double kPx = 4.0 / 3.0;

ImRect CellScreen(const model::Sheet& sh, const render::OverlayGeom& g, uint32_t r, uint32_t c) {
  const double x = static_cast<double>(sh.cols->Offset(c)), y = static_cast<double>(sh.rows->Offset(r));
  const ImVec2 a = g.to_screen(x, y);
  return ImRect(a, ImVec2(a.x + static_cast<float>(sh.cols->Size(c) * g.sc), a.y + static_cast<float>(sh.rows->Size(r) * g.sc)));
}

void DrawArrow(ImDrawList* dl, const ImRect& b, ImU32 col, bool funnel) {
  const ImVec2 c = b.GetCenter();
  const float a = std::max(2.5f, b.GetWidth() * 0.22f);
  if (funnel) {
    // filtered: funnel + small arrow
    dl->AddTriangleFilled(ImVec2(c.x - a * 1.3f, c.y - a), ImVec2(c.x + a * 0.3f, c.y - a), ImVec2(c.x - a * 0.5f, c.y + a * 0.1f), col);
    dl->AddRectFilled(ImVec2(c.x - a * 0.65f, c.y), ImVec2(c.x - a * 0.35f, c.y + a), col);
    dl->AddTriangleFilled(ImVec2(c.x + a * 0.4f, c.y), ImVec2(c.x + a * 1.4f, c.y), ImVec2(c.x + a * 0.9f, c.y + a * 0.7f), col);
  } else {
    dl->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.45f), ImVec2(c.x + a, c.y - a * 0.45f), ImVec2(c.x, c.y + a * 0.55f), col);
  }
}

// the small arrow of a sorted column, beside the button's own glyph (Excel)
void DrawSortMark(ImDrawList* dl, const ImRect& b, ImU32 col, bool desc) {
  const float x = b.Min.x + b.GetWidth() * 0.24f, a = std::max(1.5f, b.GetWidth() * 0.12f);
  const float top = b.Min.y + b.GetHeight() * 0.2f, bottom = b.Max.y - b.GetHeight() * 0.2f;
  dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), col, 1.0f);
  if (desc) dl->AddTriangleFilled(ImVec2(x - a, bottom - a * 1.4f), ImVec2(x + a + 1, bottom - a * 1.4f), ImVec2(x + 0.5f, bottom + 0.5f), col);
  else dl->AddTriangleFilled(ImVec2(x + 0.5f, top - 0.5f), ImVec2(x - a, top + a * 1.4f), ImVec2(x + a + 1, top + a * 1.4f), col);
}

std::string Lower(const std::string& s) { return formula::FoldCase(s); }  // the filter list as the filter compares

// a check box (or list item) whose text is a cell's: shown as it is, "C##1" too (## is ImGui's id marker)
bool CellTextCheckbox(const std::string& text, bool* v) {
  if (text.find("##") == std::string::npos) return ImGui::Checkbox(text.c_str(), v);
  bool changed = ImGui::Checkbox("##cell", v);
  ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::TextUnformatted(text.c_str());
  ui::NoteUiText(text.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
  if (ImGui::IsItemClicked()) {
    *v = !*v;
    changed = true;
  }
  return changed;
}
bool CellTextSelectable(const std::string& text, bool selected) {
  if (text.find("##") == std::string::npos) return ImGui::Selectable(text.empty() ? " " : text.c_str(), selected);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::Selectable("##cell", selected);
  const ImVec2 q = ImGui::GetItemRectMax();
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
  ui::NoteUiText(text.c_str(), ImRect(p, q));
  return clicked;
}

// the filter a menu works on: the sheet's AutoFilter (table 0) or a table's own; *range: its header row and data
const model::AutoFilter* FilterOf(const model::Sheet& sh, uint32_t table, Rect* range) {
  if (!table) {
    if (!sh.autofilter->on) return nullptr;
    *range = sh.autofilter->range;
    return &*sh.autofilter;
  }
  for (const auto& t : *sh.tables) {
    if (t.id == table && engine::TableHasOwnFilter(sh, t)) {
      *range = engine::TableFilterRange(t);
      return &t.filter;
    }
  }
  return nullptr;
}

void PostFilterColumn(AppState& s, const char* label, uint32_t sheet, uint32_t table, const model::FilterColumn& fc) {
  s.engine->PostUser(label, [sheet, table, fc](Document& d) {
    if (table) d.SetTableFilterColumn(sheet, table, fc);
    else d.SetFilterColumn(sheet, fc);
  });
}

void OpenFilterMenu(AppState& s, uint32_t sheet, uint32_t field, ImVec2 pos, uint32_t table = 0) {
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh) return;
  Rect frange{};
  const model::AutoFilter* afp = FilterOf(*sh, table, &frange);
  if (!afp) return;
  const auto& af = *afp;
  FilterUi& f = R().filter;
  f = FilterUi{};
  f.sheet = sheet;
  f.field = field;
  f.table = table;
  f.pos = pos;
  const uint32_t col = frange.c0 + field;
  // the list takes in the rows typed below it (Excel); rows the other columns' criteria hide are not offered
  const Rect rg = table ? frange : engine::AutoFilterExtent(*sh, frange);
  const engine::FilterTester others(s.snap->wb, *sh, af, rg);
  // distinct display texts in Excel's order: numbers by value, then text A to Z (case-insensitive), logicals, errors
  std::map<std::string, std::pair<int, double>> uniq;  // text -> (0 number / 1 text / 2 logical / 3 error, value)
  std::set<std::string> seen_lower;
  const auto& tbl = *s.snap->wb.styles;
  size_t nums = 0, texts = 0, dates = 0;
  std::set<uint32_t> fills, fonts;
  for (uint32_t r = rg.r0 + 1; r <= rg.r1 && r - rg.r0 < 100000; ++r) {
    if (!others.Passes(r, static_cast<int>(field))) continue;
    const model::Style& st = tbl.Get(sh->GetStyle(r, col));
    fills.insert((st.fill_color >> 24) ? st.fill_color : 0u);
    fonts.insert((st.font_color >> 24) ? st.font_color : 0u);
  }
  for (uint32_t r = rg.r0 + 1; r <= rg.r1 && uniq.size() < 10000; ++r) {
    if (!others.Passes(r, static_cast<int>(field))) continue;
    const core::Value& v = sh->GetValue(r, col);
    const std::string& code = tbl.NumberFormat(tbl.Get(sh->GetStyle(r, col)));
    if (v.IsNumeric()) {
      ++nums;
      if (formula::IsDateFormat(code)) ++dates;
    } else if (v.IsString()) {
      ++texts;
    }
    const std::string t = engine::DisplayTextOf(v, code);
    const int kind = v.IsNumeric() ? 0 : v.IsBool() ? 2 : v.IsError() ? 3 : 1;
    if (t.empty()) f.blanks_present = true;
    else if (seen_lower.insert(Lower(t)).second) uniq.emplace(t, std::make_pair(kind, v.IsNumeric() ? v.AsDouble() : 0.0));  // AAPL and aapl: one entry, as in Excel
  }
  std::vector<std::pair<std::string, std::pair<int, double>>> items(uniq.begin(), uniq.end());
  std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    if (a.second.first != b.second.first) return a.second.first < b.second.first;
    if (a.second.first == 0) return a.second.second < b.second.second;
    const std::string la = Lower(a.first), lb = Lower(b.first);
    return la != lb ? la < lb : a.first < b.first;
  });
  for (auto& it : items) {
    f.serials.push_back(it.second.first == 0 ? it.second.second : -1.0);
    f.values.push_back(std::move(it.first));
  }
  f.checked.assign(f.values.size(), 1);
  f.col_num = nums > texts;
  f.col_date = f.col_num && dates * 2 > nums;
  // the icons conditional formatting shows in the column: Filter by Color > Filter by Cell Icon (Excel)
  if (!sh->cond_formats->empty()) {
    engine::CondFormatEval ev(s.snap->wb, sheet);
    std::set<int> seen;
    for (uint32_t r = rg.r0 + 1; r <= rg.r1 && r - rg.r0 < 100000; ++r) {
      if (!others.Passes(r, static_cast<int>(field))) continue;
      engine::CfVisual v;
      seen.insert(ev.Eval(r, col, &v) && v.icon >= 0 ? (int{v.icon_style} << 8) | v.icon : -1);
    }
    if (seen.size() > 1 || (seen.size() == 1 && *seen.begin() >= 0)) {
      for (const int code : seen) {
        if (code >= 0) f.icons.push_back(code);
      }
      if (seen.count(-1)) f.icons.push_back(-1);  // No Cell Icon comes last
    }
  }
  // colors are offered only when the column has more than one
  if (fills.size() > 1) f.fills.assign(fills.begin(), fills.end());
  if (fonts.size() > 1) f.fonts.assign(fonts.begin(), fonts.end());
  // current criteria
  for (const auto& fc : af.columns) {
    if (fc.field != field) continue;
    if (fc.kind == 1) {
      std::set<std::string> on;
      for (const std::string& v : fc.values) on.insert(Lower(v));
      for (size_t i = 0; i < f.values.size(); ++i) f.checked[i] = on.count(Lower(f.values[i])) != 0;
      f.blanks_checked = fc.blanks;
    } else if (fc.kind == 2) {
      f.custom = true;
      auto split = [](const std::string& crit, int* op, char* buf) {
        static const char* kOps[] = {"=", "<>", ">", ">=", "<", "<="};
        std::string c = crit;
        *op = 0;
        for (int i = 5; i >= 0; --i) {
          const size_t n = std::strlen(kOps[i]);
          if (c.compare(0, n, kOps[i]) == 0) {
            *op = i;
            c = c.substr(n);
            break;
          }
        }
        std::snprintf(buf, 128, "%s", c.c_str());
      };
      split(fc.crit1, &f.op1, f.v1);
      if (!fc.crit2.empty()) split(fc.crit2, &f.op2, f.v2);
      f.or_op = fc.or_op;
    }
  }
  // keep it inside the main window (move up when it doesn't fit), so it doesn't become a separate OS window
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float need = S(430);
  if (f.pos.y + need > vp->Pos.y + vp->Size.y) f.pos.y = std::max(vp->Pos.y + S(40), vp->Pos.y + vp->Size.y - need);
  if (f.pos.x + S(280) > vp->Pos.x + vp->Size.x) f.pos.x = std::max(vp->Pos.x, vp->Pos.x + vp->Size.x - S(280));
  ImGui::OpenPopupEx(kFilterPopup);
}

// sort_on: 0 values, 1 cell color, 2 font color (Sort by Color: that color's rows on top)
void SortRange(AppState& s, uint32_t sheet, const Rect& data, uint32_t col, bool desc, int sort_on = 0, uint32_t color = 0, uint32_t table = 0) {
  s.engine->PostUser(sort_on ? TR("Sort by Color") : desc ? TR("Sort Descending") : TR("Sort Ascending"), [sheet, data, col, desc, sort_on, color, table](Document& d) {
    d.ApplyAutoFilter(sheet);  // takes in rows typed below the list first
    const model::Sheet* sh = d.S(sheet);
    if (!sh) return;
    Rect frange{};
    if (!FilterOf(*sh, table, &frange)) return;
    Rect body = frange;
    body.r0 += 1;
    engine::SortKey key{col, desc};
    key.sort_on = sort_on;
    key.color = color;
    d.Sort(sheet, body.r0 <= body.r1 ? body : data, {key}, false);
    const std::vector<model::SortLevel> levels = sort_on ? std::vector<model::SortLevel>{} : std::vector<model::SortLevel>{model::SortLevel{col - frange.c0, desc}};
    if (table) d.SetTableFilterSort(sheet, table, levels);
    else d.SetFilterSort(sheet, levels);
    d.ApplyAutoFilter(sheet);
  });
}

// the color swatches of Filter by Color and Sort by Color; returns the clicked color (kind 4 fill / 5 font) or -1
int64_t ColorSwatches(const std::vector<uint32_t>& cols, int kind, const char* none) {
  int64_t picked = -1;
  for (uint32_t argb : cols) {
    ImGui::PushID(static_cast<int>(argb) ^ (kind << 28));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const bool clicked = ImGui::Selectable(argb ? "##sw" : none, false, 0, ImVec2(argb ? S(120) : 0, 0));
    if (argb) {
      const ImU32 col = IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255);
      if (kind == 4) ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + S(116), p.y + h - 1), col);
      else ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 4, p.y), col, "Abc");
    }
    if (clicked) picked = argb;
    ImGui::PopID();
  }
  return picked;
}

// a date column's values grouped Year > Month > Day, each level with a box that is ticked, empty or mixed (Excel)
void DateTree(FilterUi& f) {
  std::map<int, std::map<int, std::map<int, std::vector<size_t>>>> tree;
  std::vector<size_t> other;  // values that aren't dates (text in a date column)
  for (size_t i = 0; i < f.values.size(); ++i) {
    int y = 0, m = 0, d = 0;
    if (i < f.serials.size() && f.serials[i] >= 0 && formula::SerialToYmd(std::floor(f.serials[i]), &y, &m, &d)) tree[y][m][d].push_back(i);
    else other.push_back(i);
  }
  // a box over several values: ticked (all), empty (none) or mixed; a click ticks all, or clears all when all were
  auto group_box = [&](const char* label, const std::vector<size_t>& idx) {
    size_t on = 0;
    for (size_t i : idx) on += f.checked[i] ? 1 : 0;
    bool all = on == idx.size();
    const bool mixed = on > 0 && on < idx.size();
    if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool clicked = ImGui::Checkbox(label, &all);
    if (mixed) ImGui::PopItemFlag();
    if (clicked) {
      const char v = on == idx.size() ? 0 : 1;
      for (size_t i : idx) f.checked[i] = v;
    }
  };
  static const char* kMonths[] = {TR("January"), TR("February"), TR("March"),     TR("April"),   TR("May"),      TR("June"),
                                  TR("July"),    TR("August"),   TR("September"), TR("October"), TR("November"), TR("December")};
  for (auto& [y, months] : tree) {
    std::vector<size_t> yi;
    for (auto& [m, days] : months) {
      for (auto& [d, idx] : days) yi.insert(yi.end(), idx.begin(), idx.end());
    }
    ImGui::PushID(y);
    const bool open = ImGui::TreeNodeEx("##y", ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::SameLine(0, 0);
    group_box(std::to_string(y).c_str(), yi);
    if (open) {
      for (auto& [m, days] : months) {
        std::vector<size_t> mi;
        for (auto& [d, idx] : days) mi.insert(mi.end(), idx.begin(), idx.end());
        ImGui::PushID(m);
        const bool mopen = ImGui::TreeNodeEx("##m", ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(0, 0);
        group_box(kMonths[std::clamp(m, 1, 12) - 1], mi);
        if (mopen) {
          for (auto& [d, idx] : days) {
            char day[8];
            std::snprintf(day, sizeof(day), "%02d", d);
            ImGui::PushID(d);
            ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
            group_box(day, idx);
            ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
            ImGui::PopID();
          }
          ImGui::TreePop();
        }
        ImGui::PopID();
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  for (size_t i : other) {
    bool v = f.checked[i] != 0;
    ImGui::PushID(static_cast<int>(i) + 100000);
    if (CellTextCheckbox(f.values[i], &v)) f.checked[i] = v;
    ImGui::PopID();
  }
}

void DrawFilterPopup(AppState& s) {
  FilterUi& f = R().filter;
  ImGui::SetNextWindowPos(f.pos);
  ImGui::SetNextWindowSizeConstraints(ImVec2(S(260), 0), ImVec2(S(360), S(560)));
  if (!ImGui::BeginPopupEx(kFilterPopup, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) return;
  const model::Sheet* sh = s.snap->Sheet(f.sheet);
  const ImGuiKeyData* esc = ImGui::GetKeyData(ImGuiKey_Escape);
  Rect frange{};
  const model::AutoFilter* afp = sh ? FilterOf(*sh, f.table, &frange) : nullptr;
  if (!afp || (esc->Down && esc->DownDuration == 0.0f)) {  // Escape closes it even from the search box
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const auto& af = *afp;
  Rect data = f.table ? frange : engine::AutoFilterExtent(*sh, frange);
  data.r0 += 1;
  const uint32_t col = frange.c0 + f.field;
  // Excel words the sort by the column's kind of data
  const char* asc = f.col_date ? TR("Sort Oldest to Newest") : f.col_num ? TR("Sort Smallest to Largest") : TR("Sort A to Z");
  const char* desc = f.col_date ? TR("Sort Newest to Oldest") : f.col_num ? TR("Sort Largest to Smallest") : TR("Sort Z to A");
  // Excel's letters in the menu (its underlined ones, the same in every language): S sorts ascending, O descending,
  // C clears the column's filter, E goes to the search box (Alt+Down, E, a few letters, Enter is how people filter
  // from the keyboard). Only while the menu itself has the keyboard: not in the search box or an open submenu
  const ImGuiIO& kio = ImGui::GetIO();
  const bool letters = ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive() && !kio.KeyCtrl && !kio.KeyAlt && !kio.KeyShift;
  auto letter = [&](ImGuiKey k) { return letters && ImGui::IsKeyPressed(k, false); };
  if (letter(ImGuiKey_E)) f.focus_search = true;
  if (ImGui::Selectable(asc) || letter(ImGuiKey_S)) {
    SortRange(s, f.sheet, data, col, false, 0, 0, f.table);
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::Selectable(desc) || letter(ImGuiKey_O)) {
    SortRange(s, f.sheet, data, col, true, 0, 0, f.table);
    ImGui::CloseCurrentPopup();
  }
  // Sort by Color: the column's colors; one of them puts its rows on top
  if (ImGui::BeginMenu(TR("Sort by Color"), !f.fills.empty() || !f.fonts.empty())) {
    if (!f.fills.empty()) {
      ImGui::TextDisabled("%s", TR("Sort by Cell Color"));
      if (const int64_t c = ColorSwatches(f.fills, 4, TR("No Fill")); c >= 0) {
        SortRange(s, f.sheet, data, col, false, 1, static_cast<uint32_t>(c), f.table);
        ImGui::CloseCurrentPopup();
      }
    }
    if (!f.fonts.empty()) {
      ImGui::TextDisabled("%s", TR("Sort by Font Color"));
      if (const int64_t c = ColorSwatches(f.fonts, 5, TR("Automatic")); c >= 0) {
        SortRange(s, f.sheet, data, col, false, 2, static_cast<uint32_t>(c), f.table);
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Custom Sort..."))) {
      ImGui::CloseCurrentPopup();
      ui::Commands().Execute("CustomSort");
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  bool filtered = false;
  for (const auto& fc : af.columns) filtered = filtered || fc.field == f.field;
  ImGui::BeginDisabled(!filtered);
  // Excel names the column by its header: Clear Filter From "Sym"
  std::string header = engine::DisplayTextOf(sh->GetValue(frange.r0, col), "General");
  if (header.empty()) header = core::ColumnName(col);
  for (size_t at = header.find("##"); at != std::string::npos; at = header.find("##", at + 3)) header.replace(at, 2, "# #");  // ## is ImGui's id marker
  if (ImGui::Selectable((TR("Clear Filter From \"") + header + TR("\"##ctx_filter")).c_str()) || (filtered && letter(ImGuiKey_C))) {
    model::FilterColumn fc;
    fc.field = f.field;
    PostFilterColumn(s, TR("Clear Filter"), f.sheet, f.table, fc);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  auto apply = [&](model::FilterColumn fc) {
    fc.field = f.field;
    PostFilterColumn(s, TR("Filter"), f.sheet, f.table, fc);
    ImGui::CloseCurrentPopup();
  };
  // Filter by Color
  if (ImGui::BeginMenu(TR("Filter by Color"), !f.fills.empty() || !f.fonts.empty() || !f.icons.empty())) {
    auto swatches = [&](const std::vector<uint32_t>& cols, int kind, const char* none) {
      if (const int64_t c = ColorSwatches(cols, kind, none); c >= 0) {
        model::FilterColumn fc;
        fc.kind = kind;
        fc.color = static_cast<uint32_t>(c);
        apply(fc);
      }
    };
    if (!f.fills.empty()) {
      ImGui::TextDisabled("%s", TR("Filter by Cell Color"));
      swatches(f.fills, 4, TR("No Fill"));
    }
    if (!f.fonts.empty()) {
      ImGui::TextDisabled("%s", TR("Filter by Font Color"));
      swatches(f.fonts, 5, TR("Automatic"));
    }
    if (!f.icons.empty()) {
      ImGui::TextDisabled("%s", TR("Filter by Cell Icon"));
      for (const int code : f.icons) {
        ImGui::PushID(code + 100000);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        const bool clicked = ImGui::Selectable(code < 0 ? TR("No Cell Icon") : "##icon", false, 0, ImVec2(code < 0 ? 0.0f : S(60), 0));
        if (code >= 0) {
          const ImRect ir(ImVec2(p.x + 2, p.y), ImVec2(p.x + 2 + h, p.y + h));
          render::DrawRuleIcon(ImGui::GetWindowDrawList(), ir, code >> 8, code & 0xFF);
          // tests find an icon by its set and place: "##filtericon1_0" (xl3Arrows, the lowest values' icon)
          ui::NoteUiItem(("##filtericon" + std::to_string(code >> 8) + "_" + std::to_string(code & 0xFF)).c_str(),
                         ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        }
        ImGui::PopID();
        if (clicked) {
          model::FilterColumn fc;
          fc.kind = 7;
          fc.icon_set = code < 0 ? 0 : code >> 8;
          fc.icon = code < 0 ? -1 : code & 0xFF;
          apply(fc);
        }
      }
    }
    ImGui::EndMenu();
  }
  // Text / Number / Date Filters: most open the Custom AutoFilter dialog with the operator chosen
  auto custom = [&](int op1, int op2 = 0, bool both = false) {
    f.op1 = op1;
    f.op2 = op2;
    f.v1[0] = f.v2[0] = 0;
    f.or_op = false;
    f.custom = both;
    ImGui::CloseCurrentPopup();
    s.OpenDialog(Dialog::CustomAutoFilter);
  };
  auto dynamic = [&](int code) {
    model::FilterColumn fc;
    fc.kind = 6;
    fc.dynamic = code;
    apply(fc);
  };
  const char* menu = f.col_date ? TR("Date Filters") : f.col_num ? TR("Number Filters") : TR("Text Filters");
  if (ImGui::BeginMenu(menu)) {
    if (f.col_date) {
      if (ImGui::MenuItem(TR("Equals..."))) custom(0);
      if (ImGui::MenuItem(TR("Before..."))) custom(4);
      if (ImGui::MenuItem(TR("After..."))) custom(2);
      if (ImGui::MenuItem(TR("Between..."))) custom(3, 5, true);
      ImGui::Separator();
      static const std::pair<const char*, int> kDyn[] = {
          {TR("Tomorrow"), 3}, {TR("Today"), 1}, {TR("Yesterday"), 2}, {nullptr, 0},
          {TR("Next Week"), 6}, {TR("This Week"), 4}, {TR("Last Week"), 5}, {nullptr, 0},
          {TR("Next Month"), 9}, {TR("This Month"), 7}, {TR("Last Month"), 8}, {nullptr, 0},
          {TR("Next Quarter"), 12}, {TR("This Quarter"), 10}, {TR("Last Quarter"), 11}, {nullptr, 0},
          {TR("Next Year"), 15}, {TR("This Year"), 13}, {TR("Last Year"), 14}, {nullptr, 0}, {TR("Year to Date"), 16}};
      for (const auto& [label, code] : kDyn) {
        if (!label) ImGui::Separator();
        else if (ImGui::MenuItem(label)) dynamic(code);
      }
      if (ImGui::BeginMenu(TR("All Dates in the Period"))) {
        static const char* kQ[] = {TR("Quarter 1"), TR("Quarter 2"), TR("Quarter 3"), TR("Quarter 4")};
        for (int q = 0; q < 4; ++q) {
          if (ImGui::MenuItem(kQ[q])) dynamic(17 + q);
        }
        ImGui::Separator();
        static const char* kM[] = {TR("January"), TR("February"), TR("March"), TR("April"), TR("May"), TR("June"), TR("July"), TR("August"),
                                   TR("September"), TR("October"), TR("November"), TR("December")};
        for (int m = 0; m < 12; ++m) {
          if (ImGui::MenuItem(kM[m])) dynamic(21 + m);
        }
        ImGui::EndMenu();
      }
    } else if (f.col_num) {
      if (ImGui::MenuItem(TR("Equals..."))) custom(0);
      if (ImGui::MenuItem(TR("Does Not Equal..."))) custom(1);
      if (ImGui::MenuItem(TR("Greater Than..."))) custom(2);
      if (ImGui::MenuItem(TR("Greater Than Or Equal To..."))) custom(3);
      if (ImGui::MenuItem(TR("Less Than..."))) custom(4);
      if (ImGui::MenuItem(TR("Less Than Or Equal To..."))) custom(5);
      if (ImGui::MenuItem(TR("Between..."))) custom(3, 5, true);
      if (ImGui::MenuItem(TR("Top 10..."))) {
        f.t_top = 0;
        f.t_count = 10;
        f.t_pct = 0;
        ImGui::CloseCurrentPopup();
        s.OpenDialog(Dialog::Top10Filter);
      }
      if (ImGui::MenuItem(TR("Above Average"))) dynamic(33);
      if (ImGui::MenuItem(TR("Below Average"))) dynamic(34);
    } else {
      if (ImGui::MenuItem(TR("Equals..."))) custom(0);
      if (ImGui::MenuItem(TR("Does Not Equal..."))) custom(1);
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Begins With..."))) custom(6);
      if (ImGui::MenuItem(TR("Ends With..."))) custom(8);
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Contains..."))) custom(10);
      if (ImGui::MenuItem(TR("Does Not Contain..."))) custom(11);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Custom Filter..."))) custom(f.op1, f.op2, true);
    ImGui::EndMenu();
  }
  {
    ImGui::SetNextItemWidth(-1);
    if (f.focus_search) {
      ImGui::SetKeyboardFocusHere();
      f.focus_search = false;
    }
    ImGui::InputTextWithHint("##search", TR("Search"), f.search, sizeof(f.search));
    const std::string q = Lower(f.search);
    ImGui::BeginChild("##vals", ImVec2(S(240), S(220)), ImGuiChildFlags_Borders);
    if (q != f.searched) {  // a new search: every result starts ticked
      f.searched = q;
      f.found_checked.assign(f.values.size(), 1);
    }
    if (q.empty()) {
      bool all = f.blanks_checked || !f.blanks_present, any = f.blanks_present && f.blanks_checked;
      for (char c : f.checked) all = all && c, any = any || c;
      if (CheckboxMixed(TR("(Select All)"), &all, any && !all)) {
        std::fill(f.checked.begin(), f.checked.end(), static_cast<char>(all));
        f.blanks_checked = all;
      }
      if (f.col_date) {
        DateTree(f);
      } else {
        for (size_t i = 0; i < f.values.size(); ++i) {
          bool v = f.checked[i] != 0;
          ImGui::PushID(static_cast<int>(i));
          if (CellTextCheckbox(f.values[i], &v)) f.checked[i] = v;
          ImGui::PopID();
        }
      }
      if (f.blanks_present) ImGui::Checkbox(TR("(Blanks)"), &f.blanks_checked);
    } else {
      bool all = true, any = false;
      for (size_t i = 0; i < f.values.size(); ++i) {
        if (Lower(f.values[i]).find(q) != std::string::npos) all = all && f.found_checked[i], any = any || f.found_checked[i];
      }
      if (CheckboxMixed(TR("(Select All Search Results)"), &all, any && !all)) {
        for (size_t i = 0; i < f.values.size(); ++i) {
          if (Lower(f.values[i]).find(q) != std::string::npos) f.found_checked[i] = all;
        }
      }
      ImGui::Checkbox(TR("Add current selection to filter"), &f.add_current);
      for (size_t i = 0; i < f.values.size(); ++i) {
        if (Lower(f.values[i]).find(q) == std::string::npos) continue;
        bool v = f.found_checked[i] != 0;
        ImGui::PushID(static_cast<int>(i));
        if (CellTextCheckbox(f.values[i], &v)) f.found_checked[i] = v;
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
  }
  ImGui::Separator();
  // Excel greys OK while nothing is ticked (it would hide every row)
  bool any = false;
  if (!f.searched.empty()) {
    for (size_t i = 0; i < f.values.size() && !any; ++i) {
      any = (Lower(f.values[i]).find(f.searched) != std::string::npos && f.found_checked[i]) || (f.add_current && f.checked[i]);
    }
    any = any || (f.add_current && f.blanks_present && f.blanks_checked);
  } else {
    for (char c : f.checked) any = any || c;
    any = any || (f.blanks_present && f.blanks_checked) || (f.values.empty() && !f.blanks_present);
  }
  ImGui::BeginDisabled(!any);
  const bool ok = DlgOkButton(TR("OK"), ImVec2(S(80), 0)) && any;
  ImGui::EndDisabled();
  if (ok) {
    model::FilterColumn fc;
    fc.field = f.field;
    if (!f.searched.empty()) {
      // OK while searching: the ticked results are shown (with what the filter showed before, when adding to it)
      fc.kind = 1;
      for (size_t i = 0; i < f.values.size(); ++i) {
        const bool found = Lower(f.values[i]).find(f.searched) != std::string::npos && f.found_checked[i];
        if (found || (f.add_current && f.checked[i])) fc.values.push_back(f.values[i]);
      }
      fc.blanks = f.add_current && f.blanks_checked;
    } else {
      bool all = f.blanks_checked || !f.blanks_present;
      for (char c : f.checked) all = all && c;
      if (!all) {
        fc.kind = 1;
        for (size_t i = 0; i < f.values.size(); ++i) {
          if (f.checked[i]) fc.values.push_back(f.values[i]);
        }
        fc.blanks = f.blanks_checked;
      }
    }
    PostFilterColumn(s, TR("Filter"), f.sheet, f.table, fc);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void DrawListPopup(AppState& s) {
  ListUi& l = R().list;
  ImGui::SetNextWindowPos(l.pos);
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(S(80), l.width), 0), ImVec2(std::max(S(300), l.width), S(260)));
  if (!ImGui::BeginPopupEx(kListPopup, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) return;
  const int n = static_cast<int>(l.items.size());
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  if (ImGui::IsWindowAppearing()) {  // on the cell's current value, as Excel does (none for a blank cell: Down goes to the first)
    l.hl = -1;
    const model::Sheet* sh = s.snap ? s.snap->Sheet(l.sheet) : nullptr;
    const std::string cur = sh ? engine::DisplayTextOf(sh->GetValue(l.r, l.c), "General") : std::string();
    for (int i = 0; i < n; ++i)
      if (l.items[static_cast<size_t>(i)] == cur) l.hl = i;
  }
  bool moved = false, pick = false;
  if (n > 0 && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) l.hl = std::min(l.hl + 1, n - 1), moved = true;
  if (n > 0 && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) l.hl = std::max(l.hl - 1, 0), moved = true;
  if (n > 0 && l.hl >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) pick = true;
  for (size_t i = 0; i < l.items.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    const bool on = static_cast<int>(i) == l.hl;
    if (on && moved) ImGui::SetScrollHereY();
    if (CellTextSelectable(l.items[i], on) || (pick && on)) {
      const uint32_t sheet = l.sheet, r = l.r, c = l.c;
      const std::string v = l.items[i];
      s.engine->PostUser(TR("Enter"), [sheet, r, c, v](Document& d) { d.SetInput(sheet, r, c, v); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopID();
  }
  if (l.items.empty()) ImGui::TextDisabled(TR("(the list is empty)"));
  ImGui::EndPopup();
}

}  // namespace

// ---- Overlay: filter buttons, validation drop-down, input message ----

namespace {
// the filter button's tooltip as Excel words it: "Name: (Showing All)", "Qty: Greater Than "5"", "Date: This Month"
std::string FilterTooltip(AppState& s, const model::Sheet& sh, const model::AutoFilter& af, const Rect& frange, uint32_t field, int sorted) {
  const uint32_t c = frange.c0 + field;
  std::string head = engine::DisplayTextOf(sh.GetValue(frange.r0, c), s.snap->wb.styles->NumberFormat(s.snap->wb.styles->Get(sh.GetStyle(frange.r0, c))));
  if (head.empty()) head = TR("(Column ") + core::ColumnName(c) + ")";
  std::string tip = head + ": ";
  auto crit = [](const std::string& k) {
    static const std::pair<const char*, const char*> kOps[] = {{">=", TR("Greater Than Or Equal To")}, {"<=", TR("Less Than Or Equal To")},
                                                                {"<>", TR("Does Not Equal")},          {">", TR("Greater Than")},
                                                                {"<", TR("Less Than")},                {"=", TR("Equals")}};
    for (const auto& [op, words] : kOps) {
      if (k.compare(0, std::strlen(op), op) == 0) return std::string(words) + " \"" + k.substr(std::strlen(op)) + "\"";
    }
    return std::string(TR("Equals")) + " \"" + k + "\"";
  };
  bool any = false;
  for (const auto& fc : af.columns) {
    if (fc.field != field) continue;
    any = true;
    if (fc.kind == 1) {
      tip += TR("Equals");
      for (size_t i = 0; i < fc.values.size() && i < 3; ++i) tip += (i ? TR(" or ") : " ") + ("\"" + fc.values[i] + "\"");
      if (fc.values.size() > 3) tip += TR(" or ...");
      if (fc.blanks) tip += fc.values.empty() ? TR(" (Blanks)") : TR(" or (Blanks)");
    } else if (fc.kind == 2) {
      tip += crit(fc.crit1);
      if (!fc.crit2.empty()) tip += (fc.or_op ? TR(" or ") : TR(" and ")) + crit(fc.crit2);
    } else if (fc.kind == 3) {
      tip += (fc.top ? TR("Top ") : TR("Bottom ")) + std::to_string(fc.count) + (fc.percent ? TR(" Percent") : TR(" Items"));
    } else if (fc.kind == 4) {
      tip += TR("Cell Color");
    } else if (fc.kind == 5) {
      tip += TR("Font Color");
    } else if (fc.kind == 6) {
      static const char* kDyn[] = {"",           TR("Today"),        TR("Yesterday"),    TR("Tomorrow"),     TR("This Week"),   TR("Last Week"),
                                   TR("Next Week"), TR("This Month"), TR("Last Month"),  TR("Next Month"),   TR("This Quarter"), TR("Last Quarter"),
                                   TR("Next Quarter"), TR("This Year"), TR("Last Year"), TR("Next Year"),    TR("Year to Date")};
      if (fc.dynamic >= 1 && fc.dynamic <= 16) tip += kDyn[fc.dynamic];
      else if (fc.dynamic >= 17 && fc.dynamic <= 20) tip += TR("Quarter ") + std::to_string(fc.dynamic - 16);
      else if (fc.dynamic == 33) tip += TR("Above Average");
      else if (fc.dynamic == 34) tip += TR("Below Average");
      else tip += TR("Dynamic Filter");
    }
  }
  if (!any) tip += TR("(Showing All)");
  if (sorted) tip += std::string("\n") + (sorted == 2 ? TR("Sorted Descending") : TR("Sorted Ascending"));
  return tip;
}
// the input message of the active cell's validation, noted by DrawRuleOverlays and drawn by DrawInputMessage
struct InputMessage {
  bool on = false;
  uint32_t sheet = 0;
  ImRect cell;
  std::string title, message;
};
InputMessage g_input_msg;
}  // namespace

void DrawRuleOverlays(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& t = ui::T();
  // filter buttons: the sheet's AutoFilter, and each table's own (Excel gives every table its buttons)
  auto buttons = [&](const model::AutoFilter& af, const Rect& frange, uint32_t table) {
    ImGui::PushID(static_cast<int>(table));
    for (uint32_t c = frange.c0; c <= frange.c1; ++c) {
      if (sh->cols->IsHidden(c)) continue;
      const ImRect cell = CellScreen(*sh, g, frange.r0, c);
      if (!cell.Overlaps(g.cells)) continue;
      const float bs = std::floor(std::min(cell.GetHeight() - 2, S(17) * static_cast<float>(g.sc / ui::Dpi())));
      const ImRect b(ImVec2(cell.Max.x - bs - 2, cell.Max.y - bs - 2), ImVec2(cell.Max.x - 2, cell.Max.y - 2));
      bool filtered = false;
      for (const auto& fc : af.columns) filtered = filtered || fc.field == c - frange.c0;
      ImGui::SetCursorScreenPos(b.Min);
      ImGui::PushID(static_cast<int>(c) + 1000000);
      ImGui::InvisibleButton("##flt", ImMax(b.GetSize(), ImVec2(1, 1)));
      const bool hov = ImGui::IsItemHovered();
      // the button, or Alt+Down on the header cell (Excel), opens the filter menu
      const auto& fv = s.FocusView();
      const bool alt_down = s.focus_sheet == sheet_id && !s.edit.active && fv.sel.act_r == frange.r0 && fv.sel.act_c == c &&
                            ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && !ImGui::GetIO().WantTextInput &&
                            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
      if (ImGui::IsItemClicked() || alt_down) OpenFilterMenu(s, sheet_id, c - frange.c0, ImVec2(b.Min.x, b.Max.y + 2), table);
      ImGui::PopID();
      dl->AddRectFilled(b.Min, b.Max, hov ? t.btn_hover : IM_COL32_WHITE);
      dl->AddRect(b.Min, b.Max, IM_COL32(0xA6, 0xA6, 0xA6, 255));
      // a sorted column shows its order beside the glyph
      int sorted = 0;  // 1 ascending, 2 descending
      for (const auto& l : af.sort_levels) {
        if (!sorted && l.col == c - frange.c0) sorted = l.desc ? 2 : 1;
      }
      if (sorted) {
        DrawSortMark(dl, b, IM_COL32(0x40, 0x40, 0x40, 255), sorted == 2);
        DrawArrow(dl, ImRect(ImVec2(b.Min.x + b.GetWidth() * 0.3f, b.Min.y), b.Max), IM_COL32(0x40, 0x40, 0x40, 255), filtered);
      } else {
        DrawArrow(dl, b, IM_COL32(0x40, 0x40, 0x40, 255), filtered);
      }
      if (hov) {
        const std::string tip = FilterTooltip(s, *sh, af, frange, c - frange.c0, sorted);
        ImGui::SetTooltip("%s", tip.c_str());
        ui::NoteUiText(tip.c_str(), b);
      }
    }
    ImGui::PopID();
  };
  if (sh->autofilter->on) buttons(*sh->autofilter, sh->autofilter->range, 0);
  for (const auto& tb : *sh->tables) {
    if (engine::TableHasOwnFilter(*sh, tb)) buttons(tb.filter, engine::TableFilterRange(tb), tb.id);
  }
  // menu opened on this sheet (opened and drawn in the same window), in the UI's font as Excel's menus: the sheet's
  // window draws in the cells' font, zoomed, and the filter menu grew with the zoom
  if (R().filter.sheet == sheet_id) {
    ImGui::PushFont(ui::F().ui, ui::F().ui_size);
    DrawFilterPopup(s);
    ImGui::PopFont();
  }
  if (R().list.sheet == sheet_id) DrawListPopup(s);
  // validation of the active cell: list drop-down arrow and input message
  if (s.focus_sheet != sheet_id || s.edit.active || sh->validations->empty()) return;
  const auto& v = s.FocusView();
  const uint32_t r = v.sel.act_r, c = v.sel.act_c;
  const model::DvRule* rule = engine::FindValidation(*sh, r, c);
  if (!rule) return;
  const ImRect cell = CellScreen(*sh, g, r, c);
  if (!cell.Overlaps(g.cells)) return;
  if (rule->type == model::DvType::List && rule->in_cell_dropdown) {
    const float bs = std::floor(std::min(cell.GetHeight(), S(17) * static_cast<float>(g.sc / ui::Dpi())));
    const ImRect b(ImVec2(cell.Max.x + 1, cell.Max.y - bs), ImVec2(cell.Max.x + 1 + bs, cell.Max.y));
    ImGui::SetCursorScreenPos(b.Min);
    ImGui::InvisibleButton("##dvlist", ImMax(b.GetSize(), ImVec2(1, 1)));
    const bool hov = ImGui::IsItemHovered();
    // the arrow, or Alt+Down as in Excel, opens the list
    const bool alt_down = ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && !ImGui::GetIO().WantTextInput &&
                          !ImGui::IsPopupOpen(kListPopup, ImGuiPopupFlags_AnyPopupLevel);
    if (ImGui::IsItemClicked() || alt_down) {
      ListUi& l = R().list;
      l.sheet = sheet_id;
      l.r = r;
      l.c = c;
      l.pos = ImVec2(cell.Min.x, cell.Max.y + 1);
      l.width = cell.GetWidth() + bs;
      l.items = engine::ValidationList(s.snap->wb, sheet_id, *rule, r, c);
      ImGui::OpenPopupEx(kListPopup);
    }
    dl->AddRectFilled(b.Min, b.Max, hov ? t.btn_hover : IM_COL32(0xF0, 0xF0, 0xF0, 255));
    dl->AddRect(b.Min, b.Max, IM_COL32(0xA6, 0xA6, 0xA6, 255));
    DrawArrow(dl, b, IM_COL32(0x40, 0x40, 0x40, 255), false);
  }
  if (rule->show_input && (!rule->input_message.empty() || !rule->input_title.empty())) {
    // drawn last (DrawInputMessage): Excel's input message floats over the circles, tables and objects on the cells
    g_input_msg = {true, sheet_id, cell, rule->input_title, rule->input_message};
  }
}

void DrawInputMessage(AppState&, uint32_t sheet_id, const render::OverlayGeom&) {
  InputMessage m = std::move(g_input_msg);
  g_input_msg = {};
  if (!m.on || m.sheet != sheet_id) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(ui::F().ui, ui::F().ui_size);
  const float w = S(200);
  const ImVec2 p(m.cell.Min.x + S(12), m.cell.Max.y + S(6));
  const ImVec2 ts = ImGui::CalcTextSize(m.message.c_str(), nullptr, false, w - S(12));
  const float th = m.title.empty() ? 0.0f : ImGui::GetTextLineHeight() + S(2);
  const ImRect box(p, ImVec2(p.x + w, p.y + ts.y + th + S(12)));
  dl->AddRectFilled(box.Min, box.Max, IM_COL32(0xFF, 0xFF, 0xE1, 255));
  dl->AddRect(box.Min, box.Max, IM_COL32(0x76, 0x76, 0x76, 255));
  if (th > 0) dl->AddText(ui::F().ui_bold, ImGui::GetFontSize(), ImVec2(p.x + S(6), p.y + S(5)), IM_COL32(0, 0, 0, 255), m.title.c_str());
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + S(6), p.y + S(5) + th), IM_COL32(0, 0, 0, 255), m.message.c_str(), nullptr, w - S(12));
  ui::NoteUiText((m.title.empty() ? m.message : m.title + "\n" + m.message).c_str(), box);
  ImGui::PopFont();
}

void DrawRulePopups(AppState&) {}

// ---- Data validation on input (called by CommitEdit) ----

ValidationResult CheckValidation(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || sh->validations->empty()) return ValidationResult::Accept;
  const model::DvRule* rule = engine::FindValidation(*sh, r, c);
  if (!rule || !rule->show_error) return ValidationResult::Accept;
  std::string msg;
  if (engine::ValidateInput(s.snap->wb, sheet, r, c, text, *rule, &msg)) return ValidationResult::Accept;
  const std::string title = rule->error_title.empty() ? std::string("YCGrid") : rule->error_title;
  switch (rule->alert) {
    case model::DvAlert::Stop: {
      const int a = AppMessageBox(s, msg, title, MB_RETRYCANCEL | MB_ICONERROR, IDCANCEL);  // automatic: the entry is refused
      return a == IDRETRY ? ValidationResult::Retry : ValidationResult::Cancel;
    }
    case model::DvAlert::Warning: {
      const int a = AppMessageBox(s, msg + TR("\n\nDo you want to continue?"), title, MB_YESNOCANCEL | MB_ICONWARNING, IDYES);
      return a == IDYES ? ValidationResult::Accept : a == IDNO ? ValidationResult::Retry : ValidationResult::Cancel;
    }
    default: {
      const int a = AppMessageBox(s, msg, title, MB_OKCANCEL | MB_ICONINFORMATION, IDOK);
      return a == IDOK ? ValidationResult::Accept : ValidationResult::Cancel;
    }
  }
}

// ---- Commands ----

namespace {

std::vector<Rect> SelectionAreasClipped(AppState& s) {
  std::vector<Rect> out;
  const model::Sheet* sh = s.FocusSheet();
  for (Rect a : SelAreas(s)) {
    if (sh && a.r1 >= sh->row_limit - 1) a.r1 = sh->has_used ? std::max(a.r0, sh->used.r1) : a.r0;
    out.push_back(a);
  }
  return out;
}

uint32_t ToArgb(const float c[3]) {
  auto b = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return 0xFF000000u | (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
}

struct Preset {
  const char* name;
  uint32_t fill, font;
  bool bold;
  uint32_t border = 0;
};
// Excel's list, in its order; "Custom Format..." after them opens the rule's own format
const Preset kPresets[] = {
    {TR("Light Red Fill with Dark Red Text"), 0xFFFFC7CE, 0xFF9C0006, false}, {TR("Yellow Fill with Dark Yellow Text"), 0xFFFFEB9C, 0xFF9C5700, false},
    {TR("Green Fill with Dark Green Text"), 0xFFC6EFCE, 0xFF006100, false},   {TR("Light Red Fill"), 0xFFFFC7CE, 0, false},
    {TR("Red Text"), 0, 0xFF9C0006, false},                                   {TR("Red Border"), 0, 0, false, 0xFFFF0000},
};

// A Date Occurring: Excel's XlTimePeriods names, and the order Excel's dialog lists them in
const char* PeriodName(int p) {
  const char* names[] = {TR("Today"), TR("Yesterday"), TR("In the last 7 days"), TR("This week"), TR("Last week"),
                         TR("Last month"), TR("Tomorrow"), TR("Next week"), TR("Next month"), TR("This month")};
  return names[std::clamp(p, 0, 9)];
}
const int kPeriodOrder[10] = {1, 0, 6, 2, 4, 3, 7, 5, 9, 8};

// the Color Scales gallery (Excel's 12): the name lists the highest value's color first
struct ScalePreset {
  const char* id;
  const char* label;
  int points;
  uint32_t lo, mid, hi;
};
const std::vector<ScalePreset>& ScalePresets() {
  static const std::vector<ScalePreset> v = {
      {"CfScaleGYR", TR("Green - Yellow - Red Color Scale"), 3, 0xFFF8696B, 0xFFFFEB84, 0xFF63BE7B},
      {"CfScaleRYG", TR("Red - Yellow - Green Color Scale"), 3, 0xFF63BE7B, 0xFFFFEB84, 0xFFF8696B},
      {"CfScaleGWR", TR("Green - White - Red Color Scale"), 3, 0xFFF8696B, 0xFFFCFCFF, 0xFF63BE7B},
      {"CfScaleRWG", TR("Red - White - Green Color Scale"), 3, 0xFF63BE7B, 0xFFFCFCFF, 0xFFF8696B},
      {"CfScaleBWR", TR("Blue - White - Red Color Scale"), 3, 0xFFF8696B, 0xFFFCFCFF, 0xFF5A8AC6},
      {"CfScaleRWB", TR("Red - White - Blue Color Scale"), 3, 0xFF5A8AC6, 0xFFFCFCFF, 0xFFF8696B},
      {"CfScaleWR", TR("White - Red Color Scale"), 2, 0xFFF8696B, 0, 0xFFFCFCFF},
      {"CfScaleRW", TR("Red - White Color Scale"), 2, 0xFFFCFCFF, 0, 0xFFF8696B},
      {"CfScaleGW", TR("Green - White Color Scale"), 2, 0xFFFCFCFF, 0, 0xFF63BE7B},
      {"CfScaleWG", TR("White - Green Color Scale"), 2, 0xFF63BE7B, 0, 0xFFFCFCFF},
      {"CfScaleGY", TR("Green - Yellow Color Scale"), 2, 0xFFFFEF9C, 0, 0xFF63BE7B},
      {"CfScaleYG", TR("Yellow - Green Color Scale"), 2, 0xFF63BE7B, 0, 0xFFFFEF9C},
  };
  return v;
}
// the Data Bars gallery: Excel's six colors, Gradient Fill then Solid Fill
struct BarPreset {
  const char* id;
  const char* label;
  uint32_t color;
  bool gradient = true;
};
const std::vector<BarPreset>& BarPresets() {
  static const std::vector<BarPreset> v = {
      {"CfBarBlue", TR("Blue Data Bar"), 0xFF638EC6},  {"CfBarGreen", TR("Green Data Bar"), 0xFF63C384},
      {"CfBarRed", TR("Red Data Bar"), 0xFFFF555A},    {"CfBarOrange", TR("Orange Data Bar"), 0xFFFFB628},
      {"CfBarLightBlue", TR("Light Blue Data Bar"), 0xFF008AEF}, {"CfBarPurple", TR("Purple Data Bar"), 0xFFD6007B},
      {"CfBarSolidBlue", TR("Blue Data Bar (Solid Fill)"), 0xFF638EC6, false},  {"CfBarSolidGreen", TR("Green Data Bar (Solid Fill)"), 0xFF63C384, false},
      {"CfBarSolidRed", TR("Red Data Bar (Solid Fill)"), 0xFFFF555A, false},    {"CfBarSolidOrange", TR("Orange Data Bar (Solid Fill)"), 0xFFFFB628, false},
      {"CfBarSolidLightBlue", TR("Light Blue Data Bar (Solid Fill)"), 0xFF008AEF, false},
      {"CfBarSolidPurple", TR("Purple Data Bar (Solid Fill)"), 0xFFD6007B, false},
  };
  return v;
}
// the Icon Sets gallery: Excel's 20 sets, grouped as Directional, Shapes, Indicators, Ratings
struct IconPreset {
  const char* id;
  const char* label;
  int style;
};
const std::vector<IconPreset>& IconPresets() {
  static const std::vector<IconPreset> v = {
      {"CfIcon3Arrows", TR("3 Arrows (Colored)"), model::kIcon3Arrows},
      {"CfIcon3ArrowsGray", TR("3 Arrows (Gray)"), model::kIcon3ArrowsGray},
      {"CfIcon3Triangles", TR("3 Triangles"), model::kIcon3Triangles},
      {"CfIcon4ArrowsGray", TR("4 Arrows (Gray)"), model::kIcon4ArrowsGray},
      {"CfIcon4Arrows", TR("4 Arrows (Colored)"), model::kIcon4Arrows},
      {"CfIcon5ArrowsGray", TR("5 Arrows (Gray)"), model::kIcon5ArrowsGray},
      {"CfIcon5Arrows", TR("5 Arrows (Colored)"), model::kIcon5Arrows},
      {"CfIcon3TrafficLights", TR("3 Traffic Lights (Unrimmed)"), model::kIcon3TrafficLights1},
      {"CfIcon3TrafficLightsRimmed", TR("3 Traffic Lights (Rimmed)"), model::kIcon3TrafficLights2},
      {"CfIcon3Signs", TR("3 Signs"), model::kIcon3Signs},
      {"CfIcon4TrafficLights", TR("4 Traffic Lights"), model::kIcon4TrafficLights},
      {"CfIcon4RedToBlack", TR("Red To Black"), model::kIcon4RedToBlack},
      {"CfIcon3Symbols", TR("3 Symbols (Circled)"), model::kIcon3Symbols},
      {"CfIcon3Symbols2", TR("3 Symbols (Uncircled)"), model::kIcon3Symbols2},
      {"CfIcon3Flags", TR("3 Flags"), model::kIcon3Flags},
      {"CfIcon3Stars", TR("3 Stars"), model::kIcon3Stars},
      {"CfIcon4Ratings", TR("4 Ratings"), model::kIcon4Ratings},
      {"CfIcon5Quarters", TR("5 Quarters"), model::kIcon5Quarters},
      {"CfIcon5Ratings", TR("5 Ratings"), model::kIcon5Ratings},
      {"CfIcon5Boxes", TR("5 Boxes"), model::kIcon5Boxes},
  };
  return v;
}

ImU32 Col32(uint32_t argb) {
  argb = model::ResolveColor(argb);
  return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255);
}
// a color scale as a strip (lowest at the left)
void DrawScaleStrip(ImDrawList* dl, ImVec2 a, ImVec2 b, int points, uint32_t lo, uint32_t mid, uint32_t hi) {
  if (points >= 3) {
    const float mx = (a.x + b.x) * 0.5f;
    dl->AddRectFilledMultiColor(a, ImVec2(mx, b.y), Col32(lo), Col32(mid), Col32(mid), Col32(lo));
    dl->AddRectFilledMultiColor(ImVec2(mx, a.y), b, Col32(mid), Col32(hi), Col32(hi), Col32(mid));
  } else {
    dl->AddRectFilledMultiColor(a, b, Col32(lo), Col32(hi), Col32(hi), Col32(lo));
  }
  dl->AddRect(a, b, IM_COL32(0xA0, 0xA0, 0xA0, 255));
}
// a data bar fading to white, as Excel's gradient bars
void DrawBarSample(ImDrawList* dl, ImVec2 a, ImVec2 b, uint32_t color) {
  const ImU32 c = Col32(color);
  dl->AddRectFilledMultiColor(a, b, c, IM_COL32_WHITE, IM_COL32_WHITE, c);
  dl->AddRect(a, b, c);
}
// the icons of a set, highest first (as Excel's gallery)
float DrawIconRow(ImDrawList* dl, ImVec2 at, float h, int style) {
  const int n = model::IconSetCount(style);
  const float sz = std::min(h - S(6), S(16)), gap = S(2);
  if (dl) {
    for (int i = 0; i < n; ++i) {
      const ImVec2 p(at.x + i * (sz + gap), at.y + (h - sz) * 0.5f);
      render::DrawRuleIcon(dl, ImRect(p, p + ImVec2(sz, sz)), style, n - 1 - i);
    }
  }
  return n * (sz + gap);
}

// the Color Scale / Data Bar / Icon Set look of a rule, in a box (the Rules Manager's Format column)
void DrawRuleLook(ImDrawList* dl, const model::CfRule& r, ImVec2 a, ImVec2 b) {
  using T = model::CfType;
  if (r.type == T::ColorScale) {
    DrawScaleStrip(dl, a, b, r.points, r.colors[0], r.colors[1], r.colors[2]);
  } else if (r.type == T::DataBar) {
    DrawBarSample(dl, a, ImVec2(a.x + (b.x - a.x) * 0.7f, b.y), r.bar_color);
  } else if (r.type == T::IconSet) {
    DrawIconRow(dl, a, b.y - a.y + S(4), r.icon_style);
  } else {
    const uint32_t fill = r.fmt.has_fill ? r.fmt.fill_color : 0;
    if (fill) dl->AddRectFilled(a, b, Col32(fill));
    const ImU32 fc = r.fmt.has_font_color ? Col32(r.fmt.font_color) : IM_COL32_BLACK;
    ImFont* font = r.fmt.bold == 1 && ui::F().ui_bold ? ui::F().ui_bold : ui::F().ui;
    dl->AddText(font, ImGui::GetFontSize(), ImVec2(a.x + S(4), a.y), fc, "AaBbCcYyZz");
  }
}

void OpenRuleEditor(AppState& s, const model::CfRule* rule, bool from_manager);

// the quick rule dialog's rule while it is open (previewed on the sheet)
model::CfRule g_quick_preview;
bool g_quick_preview_on = false;

void AddRule(AppState& s, model::CfRule rule, const char* label) {
  const uint32_t sheet = s.focus_sheet;
  rule.ranges = SelectionAreasClipped(s);
  s.engine->PostUser(label, [sheet, rule](Document& d) {
    if (!d.RefuseProtectedSheet(sheet)) d.AddCondFormat(sheet, rule);
  });
}

}  // namespace

const model::CfRule* CfPreviewRule(AppState& s, const std::string& hovered) {
  // Highlight Cells Rules / Top Bottom dialogs: the rule as it stands while the dialog is open
  if (s.dialog == Dialog::CfQuick) return g_quick_preview_on ? &g_quick_preview : nullptr;
  if (hovered.rfind("Cf", 0) != 0) return nullptr;
  // a Data Bars / Color Scales / Icon Sets entry under the mouse: that rule on the selection
  static model::CfRule r;
  r = model::CfRule{};
  bool found = false;
  for (const auto& b : BarPresets()) {
    if (hovered != b.id) continue;
    r.type = model::CfType::DataBar;
    r.bar_color = b.color;
    r.bar_gradient = b.gradient;
    found = true;
  }
  for (const auto& p : ScalePresets()) {
    if (hovered != p.id) continue;
    r.type = model::CfType::ColorScale;
    r.points = p.points;
    r.colors[0] = p.lo;
    r.colors[1] = p.points >= 3 ? p.mid : p.lo;
    r.colors[2] = p.hi;
    found = true;
  }
  for (const auto& ip : IconPresets()) {
    if (hovered != ip.id) continue;
    r.type = model::CfType::IconSet;
    r.icon_style = ip.style;
    r.icon_thresholds = model::DefaultIconThresholds(ip.style);
    found = true;
  }
  if (!found) return nullptr;
  r.ranges = SelectionAreasClipped(s);
  return &r;
}

void RegisterRuleCommands(AppState& s) {
  auto& Rg = ui::Commands();
  auto make = [](const char* id, const char* label, const char* ic = nullptr, const char* shortcut = "", const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  // ---- Filter ----
  Rg.Add(make("ToggleFilter", TR("Filter"), ui::icon::Filter, "Ctrl+Shift+L", TR("Turn AutoFilter on or off: the header row gets dropdown buttons."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    const uint32_t sheet = sh->id;
    // inside a table: its own filter buttons (Excel's Ctrl+Shift+L in a table)
    const auto& fsel = s.FocusView().sel;
    if (const model::Table* t = model::TableAt(s.snap->wb, sheet, fsel.act_r, fsel.act_c); t && t->header) {
      const bool on = engine::TableOwnsSheetFilter(*sh, *t) || engine::TableHasOwnFilter(*sh, *t);
      const uint32_t id = t->id;
      s.engine->PostUser(on ? TR("Turn Off Filter") : TR("Filter"), [sheet, id, on](Document& d) { d.SetTableFilterButton(sheet, id, !on); });
      return;
    }
    // outside every table: the sheet's AutoFilter goes off, unless a table's buttons are what use it (SetAutoFilter
    // gives them their own then, and the list here gets the sheet's, as in Excel)
    const bool table_owns = std::any_of(sh->tables->begin(), sh->tables->end(),
                                        [&](const model::Table& t) { return t.header && engine::TableOwnsSheetFilter(*sh, t); });
    if (sh->autofilter->on && !table_owns) {
      s.engine->PostUser(TR("Turn Off Filter"), [sheet](Document& d) { d.ClearAutoFilter(sheet); });
      return;
    }
    Rect r = SelRect(s);
    if (r.IsCell()) r = engine::CurrentRegion(*sh, r.r0, r.c0);
    if (sh->has_used && r.r1 >= sh->row_limit - 1) r.r1 = std::max(r.r0, sh->used.r1);
    s.engine->PostUser(TR("Filter"), [sheet, r](Document& d) { d.SetAutoFilter(sheet, r); });
  };
  Rg.Add(make("ClearFilter", TR("Clear"), ui::icon::FilterClear, "", TR("Clear all filters and show all data."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Clear Filter"), [sheet](Document& d) {
      d.ShowAllData(sheet);
      d.SetFilterSort(sheet, {});  // Excel's Clear: "the filter and sort state"
    });
  };
  Rg.Add(make("ReapplyFilter", TR("Reapply"), ui::icon::FilterReapply, "Ctrl+Alt+L", TR("Reapply the filter and sort after the data changes."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Reapply Filter"), [sheet](Document& d) { d.ApplyAutoFilter(sheet, true); });
  };
  // ---- Data validation ----
  Rg.Add(make("DataValidation", TR("Data Validation"), ui::icon::Validation, "", TR("Restrict what data can be entered, or provide a dropdown list."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    auto open = [&s](const model::DvRule& rule) {
      RulesUi& u = R();
      u.dv = rule;
      u.dv_same_cells = rule.ranges;  // empty for a new rule: the box is greyed, as in Excel
      u.dv_same = false;
      u.dv.ranges.clear();
      std::snprintf(u.dv_f1, sizeof(u.dv_f1), "%s", u.dv.formula1.c_str());
      std::snprintf(u.dv_f2, sizeof(u.dv_f2), "%s", u.dv.formula2.c_str());
      std::snprintf(u.dv_it, sizeof(u.dv_it), "%s", u.dv.input_title.c_str());
      std::snprintf(u.dv_im, sizeof(u.dv_im), "%s", u.dv.input_message.c_str());
      std::snprintf(u.dv_et, sizeof(u.dv_et), "%s", u.dv.error_title.c_str());
      std::snprintf(u.dv_em, sizeof(u.dv_em), "%s", u.dv.error_message.c_str());
      s.OpenDialog(Dialog::DataValidation);
    };
    // the rules the selection meets, and whether some of its cells have none (Excel asks before mixing them)
    const auto areas = SelectionAreasClipped(s);
    std::vector<const model::DvRule*> met;
    bool uncovered = false;
    for (const Rect& a : areas) {
      std::vector<Rect> rest{a};
      for (const auto& d : *sh->validations) {
        bool hit = false;
        for (const Rect& x : d.ranges) {
          if (!x.Intersects(a)) continue;
          hit = true;
          std::vector<Rect> next;
          for (const Rect& y : rest) {
            for (const Rect& z : engine::SubtractRect(y, x)) next.push_back(z);
          }
          rest = std::move(next);
        }
        if (!hit) continue;
        bool same = false;
        for (const model::DvRule* m : met) {
          same = same || (m->type == d.type && m->op == d.op && m->formula1 == d.formula1 && m->formula2 == d.formula2 && m->alert == d.alert &&
                          m->ignore_blank == d.ignore_blank && m->in_cell_dropdown == d.in_cell_dropdown);
        }
        if (!same) met.push_back(&d);
      }
      uncovered = uncovered || !rest.empty();
    }
    if (met.size() > 1) {
      AskYesNo(s, "YCGrid", TR("The selection contains more than one type of validation. Erase current settings and continue?"),
               [open]() { open(model::DvRule{}); });
      return;
    }
    if (met.size() == 1 && uncovered) {
      const model::DvRule rule = *met[0];
      AskYesNo(s, "YCGrid", TR("The selection contains some cells without Data Validation settings. Do you want to extend Data Validation to these cells?"),
               [open, rule]() { open(rule); });
      return;
    }
    open(met.empty() ? model::DvRule{} : *met[0]);
  };
  // ---- Conditional formatting ----
  // the quick dialogs, titled as Excel's (Greater Than...), pre-filled with values from the selection
  struct Quick {
    const char* id;
    const char* label;
    int kind;
  };
  static const Quick kQuick[] = {{"CfGreater", TR("Greater Than..."), 1},   {"CfLess", TR("Less Than..."), 2},           {"CfBetween", TR("Between..."), 3},
                                 {"CfEqual", TR("Equal To..."), 4},         {"CfTextContains", TR("Text that Contains..."), 5}, {"CfDuplicate", TR("Duplicate Values..."), 6},
                                 {"CfTop10", TR("Top 10 Items..."), 7},     {"CfBottom10", TR("Bottom 10 Items..."), 8}, {"CfAboveAverage", TR("Above Average..."), 9},
                                 {"CfBelowAverage", TR("Below Average..."), 10}, {"CfTop10Percent", TR("Top 10%..."), 11}, {"CfBottom10Percent", TR("Bottom 10%..."), 12},
                                 {"CfDateOccurring", TR("A Date Occurring..."), 13}};
  for (const auto& q : kQuick) {
    const int kind = q.kind;
    Rg.Add(make(q.id, q.label)).execute = [&s, kind](ui::Command&) {
      RulesUi& u = R();
      u.cf_kind = kind;
      u.cf_v1[0] = u.cf_v2[0] = 0;
      if (kind >= 7 && kind <= 8) std::snprintf(u.cf_v1, sizeof(u.cf_v1), "10");
      if (kind >= 11 && kind <= 12) std::snprintf(u.cf_v1, sizeof(u.cf_v1), "10");
      u.cf_preset = 0;
      u.cf_dupe = 0;
      u.cf_period = 1;
      // Text That Contains starts with the active cell's text (Excel 16, measured: MSFT with B3 active in B2:B4)
      if (kind == 5) {
        if (const model::Sheet* sh = s.FocusSheet()) {
          const auto& sel = s.FocusView().sel;
          const core::Value& v = sh->GetValue(sel.act_r, sel.act_c);
          if (v.IsString()) {
            const std::string_view text = core::Utf8Prefix(v.str(), sizeof(u.cf_v1) - 1);
            std::snprintf(u.cf_v1, sizeof(u.cf_v1), "%.*s", static_cast<int>(text.size()), text.data());
          }
        }
      }
      // Excel starts the value boxes with values from the selection's numbers (Excel 16, measured): halfway
      // between the lowest and the highest, and for Between a quarter and three quarters of the way, rounded to
      // whole numbers (3 and 88 give 45.5, then 24 and 67)
      if (kind >= 1 && kind <= 4) {
        if (const model::Sheet* sh = s.FocusSheet()) {
          double lo = 0, hi = 0;
          size_t n = 0;
          for (const Rect& a : SelectionAreasClipped(s)) {
            engine::ForEachValueRowMajor(*sh, a, [&](uint32_t, uint32_t, const core::Value& v) {
              if (!v.IsNumeric()) return true;
              const double x = v.AsDouble();
              lo = n ? std::min(lo, x) : x;
              hi = n ? std::max(hi, x) : x;
              return ++n < 100000;
            });
          }
          if (n) {
            auto put = [](char* buf, size_t size, double x) {
              std::string t = formula::FormatGeneral(x, 11);
              std::snprintf(buf, size, "%s", t.c_str());
            };
            if (kind == 3) {
              put(u.cf_v1, sizeof(u.cf_v1), std::round(lo + (hi - lo) * 0.25));
              put(u.cf_v2, sizeof(u.cf_v2), std::round(lo + (hi - lo) * 0.75));
            } else {
              put(u.cf_v1, sizeof(u.cf_v1), lo + (hi - lo) / 2);
            }
          }
        }
      }
      s.OpenDialog(Dialog::CfQuick);
    };
  }
  // Data Bars, Color Scales, Icon Sets: one command per gallery entry, drawn as it looks in the menu
  Rg.Add(make("CfDataBars", TR("Data Bars"), nullptr, "", TR("Show values as bars."))).execute = [&s](ui::Command&) {
    model::CfRule r;
    r.type = model::CfType::DataBar;
    AddRule(s, r, TR("Data Bars"));
  };
  for (const auto& b : BarPresets()) {
    const uint32_t color = b.color;
    const bool gradient = b.gradient;
    Rg.Add(make(b.id, b.label, nullptr, "", TR("Show values as bars."))).execute = [&s, color, gradient](ui::Command&) {
      model::CfRule r;
      r.type = model::CfType::DataBar;
      r.bar_color = color;
      r.bar_gradient = gradient;
      if (!gradient) r.bar_border = 0;
      AddRule(s, r, TR("Data Bars"));
    };
  }
  // Clear Rules from This Table / This PivotTable: shown when the active cell is in one
  Rg.Add(make("CfClearTable", TR("Clear Rules from This Table"))).execute = [&s](ui::Command&) {
    if (!s.snap) return;
    const auto& sel = s.FocusView().sel;
    const model::Table* t = model::TableAt(s.snap->wb, s.focus_sheet, sel.act_r, sel.act_c);
    if (!t) return;
    const uint32_t sheet = s.focus_sheet;
    const Rect area = t->range;
    s.engine->PostUser(TR("Clear Rules"), [sheet, area](Document& d) { d.ClearCondFormats(sheet, area); });
  };
  Rg.Add(make("CfClearPivot", TR("Clear Rules from This PivotTable"))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    const auto& sel = s.FocusView().sel;
    for (const auto& p : *sh->pivots) {
      if (!p.output.Contains(sel.act_r, sel.act_c)) continue;
      const uint32_t sheet = s.focus_sheet;
      const Rect area = p.output;
      s.engine->PostUser(TR("Clear Rules"), [sheet, area](Document& d) { d.ClearCondFormats(sheet, area); });
      return;
    }
  };
  for (const auto& ip : IconPresets()) {
    const int style = ip.style;
    Rg.Add(make(ip.id, ip.label, nullptr, "", TR("Show an icon in each cell according to its value."))).execute = [&s, style](ui::Command&) {
      model::CfRule r;
      r.type = model::CfType::IconSet;
      r.icon_style = style;
      r.icon_thresholds = model::DefaultIconThresholds(style);
      AddRule(s, r, TR("Icon Sets"));
    };
  }
  Rg.Add(make("CfColorScales", TR("Color Scales"), nullptr, "", TR("Green - Yellow - Red color scale (higher values are green)."))).execute = [&s](ui::Command&) {
    model::CfRule r;
    r.type = model::CfType::ColorScale;
    r.points = 3;
    AddRule(s, r, TR("Color Scales"));
  };
  for (const auto& sp : ScalePresets()) {
    const ScalePreset p = sp;
    Rg.Add(make(sp.id, sp.label, nullptr, "", TR("Color each cell by where its value falls within the range."))).execute = [&s, p](ui::Command&) {
      model::CfRule r;
      r.type = model::CfType::ColorScale;
      r.points = p.points;
      r.colors[0] = p.lo;
      r.colors[1] = p.points >= 3 ? p.mid : p.lo;
      r.colors[2] = p.hi;
      AddRule(s, r, TR("Color Scales"));
    };
  }
  // More Rules...: New Formatting Rule on the matching rule type
  auto more = [&](const char* id, int kind, model::CfType type) {
    Rg.Add(make(id, TR("More Rules..."))).execute = [&s, kind, type](ui::Command&) {
      OpenRuleEditor(s, nullptr, false);
      RulesUi& u = R();
      u.cfe_kind = kind;
      u.cfe.type = type;
      if (type == model::CfType::ColorScale) u.cfe.points = 3, u.cfe.colors[0] = 0xFFF8696B, u.cfe.colors[1] = 0xFFFFEB84, u.cfe.colors[2] = 0xFF63BE7B;
      if (type == model::CfType::IconSet) u.cfe.icon_thresholds = model::DefaultIconThresholds(u.cfe.icon_style);
    };
  };
  more("CfMoreRulesHighlight", 1, model::CfType::CellValue);
  more("CfMoreRulesTopBottom", 2, model::CfType::Top10);
  more("CfMoreRulesBars", 0, model::CfType::DataBar);
  more("CfMoreRulesScales", 0, model::CfType::ColorScale);
  more("CfMoreRulesIcons", 0, model::CfType::IconSet);
  // the menus show each entry as it looks: a bar, a color strip, the icons
  // (other menus' pictures, registered earlier, are kept: chart styles and layouts)
  ui::ribbon::St().menu_picture = [prev = ui::ribbon::St().menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
    if (cmd.rfind("Cf", 0) != 0) return prev ? prev(cmd, dl, at, h) : 0.0f;
    const float w = S(44);
    for (const auto& b : BarPresets()) {
      if (cmd == b.id) {
        if (dl && b.gradient) DrawBarSample(dl, ImVec2(at.x, at.y + S(5)), ImVec2(at.x + w, at.y + h - S(5)), b.color);
        else if (dl) dl->AddRectFilled(ImVec2(at.x, at.y + S(5)), ImVec2(at.x + w, at.y + h - S(5)), Col32(b.color));
        return w;
      }
    }
    for (const auto& p : ScalePresets()) {
      if (cmd == p.id) {
        if (dl) DrawScaleStrip(dl, ImVec2(at.x, at.y + S(5)), ImVec2(at.x + w, at.y + h - S(5)), p.points, p.lo, p.mid, p.hi);
        return w;
      }
    }
    for (const auto& ip : IconPresets()) {
      if (cmd == ip.id) return DrawIconRow(dl, at, h, ip.style);
    }
    return 0.0f;
  };
  Rg.Add(make("CfNewRule", TR("New Rule..."), nullptr, "", TR("A rule of any type: color scales, values, top / bottom, above average, duplicates or a formula."))).execute =
      [&s](ui::Command&) { OpenRuleEditor(s, nullptr, false); };
  Rg.Add(make("CfClearSelection", TR("Clear rules from selected cells"))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelectionAreasClipped(s);
    s.engine->PostUser(TR("Clear Rules"), [sheet, areas](Document& d) {
      for (const Rect& a : areas) d.ClearCondFormats(sheet, a);
    });
  };
  Rg.Add(make("CfClearSheet", TR("Clear rules from the entire sheet"))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Clear Rules"), [sheet](Document& d) { d.ClearCondFormats(sheet, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1}); });
  };
  Rg.Add(make("CfClearRules", TR("Clear Rules"))).execute = [](ui::Command&) { ui::Commands().Execute("CfClearSelection"); };
  Rg.Add(make("CfManageRules", TR("Manage Rules..."))).execute = [&s](ui::Command&) { s.OpenDialog(Dialog::CfManage); };
  Rg.Add(make("CfHighlightRules", TR("Highlight Cells Rules"))).execute = [](ui::Command&) { ui::Commands().Execute("CfGreater"); };
  Rg.Add(make("CfTopBottomRules", TR("Top/Bottom Rules"))).execute = [](ui::Command&) { ui::Commands().Execute("CfTop10"); };
  Rg.Add(make("CfIconSets", TR("Icon Sets"))).execute = [](ui::Command&) { ui::Commands().Execute("CfIcon3Arrows"); };
}

// ---- Dialogs ----

namespace {

bool BeginDlg(AppState& s, Dialog d, const char* title, float width) {
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(width), 0), ImGuiCond_Appearing);
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!RangePicking() && (!open || !ImGui::IsPopupOpen(title))) s.dialog = Dialog::None;
    return false;
  }
  // Escape cancels, as in every other dialog (a list open in the dialog takes the Escape first)
  if (!open || (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) {
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

// ---- New / Edit Formatting Rule: Excel's six rule types, each with its own controls, and the format preview ----
void DrawRuleEditor(AppState& s) {
  RulesUi& u = R();
  model::CfRule& r = u.cfe;
  const char* title = u.cfe_id ? TR("Edit Formatting Rule###yc_dlg_cfnew") : TR("New Formatting Rule###yc_dlg_cfnew");
  // back from Format... (Format Cells closed, with OK or Cancel)
  if (u.cfe_reopen && s.dialog == Dialog::None) {
    u.cfe_reopen = false;
    s.OpenDialog(Dialog::CfNewRule);
  }
  if (u.cfe_reopen) return;
  if (!BeginDlg(s, Dialog::CfNewRule, title, 560)) {
    if (s.dialog == Dialog::None && u.cfe_from_manager && !RangePicking()) {
      u.cfe_from_manager = false;
      s.OpenDialog(Dialog::CfManage);
    }
    return;
  }
  using T = model::CfType;
  ImGui::TextUnformatted(TR("Select a Rule Type:"));
  const char* kinds[] = {TR("Format all cells based on their values"), TR("Format only cells that contain"), TR("Format only top or bottom ranked values"),
                         TR("Format only values that are above or below average"), TR("Format only unique or duplicate values"),
                         TR("Use a formula to determine which cells to format")};
  if (ImGui::BeginChild("##kinds", ImVec2(-1, S(132)), ImGuiChildFlags_Borders)) {
    for (int k = 0; k < 6; ++k) {
      if (ImGui::Selectable(kinds[k], u.cfe_kind == k)) u.cfe_kind = k;
    }
  }
  ImGui::EndChild();
  ImGui::SeparatorText(TR("Edit the Rule Description:"));
  bool has_format = true;  // the kinds that format cells (not scales, bars or icons)
  switch (u.cfe_kind) {
    case 0: {
      has_format = false;
      const char* styles[] = {TR("2-Color Scale"), TR("3-Color Scale"), TR("Data Bar"), TR("Icon Sets")};
      int st = r.type == T::DataBar ? 2 : r.type == T::IconSet ? 3 : (r.points == 2 ? 0 : 1);
      ImGui::SetNextItemWidth(S(200));
      if (ui::Combo(TR("Format Style"), &st, styles, 4)) {
        r.type = st == 2 ? T::DataBar : st == 3 ? T::IconSet : T::ColorScale;
        r.points = st == 0 ? 2 : 3;
        if (r.type == T::IconSet && r.icon_thresholds.size() + 1 != static_cast<size_t>(model::IconSetCount(r.icon_style))) r.icon_thresholds = model::DefaultIconThresholds(r.icon_style);
      }
      if (r.type != T::DataBar && r.type != T::IconSet) r.type = T::ColorScale;
      if (r.type == T::ColorScale) {
        // Minimum / (Midpoint) / Maximum: Type, Value and Color for each, and a preview strip (Excel)
        const int n = r.points == 3 ? 3 : 2;
        const char* heads[] = {TR("Minimum"), TR("Midpoint"), TR("Maximum")};
        if (ImGui::BeginTable("##points", n + 1, ImGuiTableFlags_SizingFixedFit)) {
          auto col = [&](int k) { return n == 3 ? k : (k == 0 ? 0 : 2); };  // the model's point of a column
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          for (int k = 0; k < n; ++k) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(heads[n == 3 ? k : (k == 0 ? 0 : 2)]);
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Type:"));
          for (int k = 0; k < n; ++k) {
            ImGui::TableNextColumn();
            const int i = col(k);
            // Excel's types: the ends offer Lowest / Highest Value, the midpoint doesn't
            const char* names[] = {TR("Number"), TR("Lowest Value"), TR("Highest Value"), TR("Percent"), TR("Formula"), TR("Percentile")};
            const int def = i == 0 ? 1 : i == 2 ? 2 : 5;
            int t = r.point_type[i] < 0 ? def : r.point_type[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(S(130));
            if (ui::BeginCombo("##type", names[std::clamp(t, 0, 5)])) {
              for (int v : {i == 0 ? 1 : i == 2 ? 2 : -1, 0, 3, 4, 5}) {
                if (v < 0) continue;
                if (ImGui::Selectable(names[v], v == t)) {
                  if (r.point_type[i] < 0 && i == 1) r.point_value[i] = 50;
                  if (r.point_type[i] < 0 && i != 1) r.point_value[i] = i == 0 ? 0 : 100;
                  r.point_type[i] = v;
                }
              }
              ImGui::EndCombo();
            }
            ImGui::PopID();
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Value:"));
          for (int k = 0; k < n; ++k) {
            ImGui::TableNextColumn();
            const int i = col(k);
            const int def = i == 0 ? 1 : i == 2 ? 2 : 5;
            const int t = r.point_type[i] < 0 ? def : r.point_type[i];
            ImGui::PushID(10 + i);
            ImGui::SetNextItemWidth(S(130));
            if (t == 1 || t == 2) {
              ImGui::TextDisabled("%s", TR("(Lowest / highest)"));
            } else if (t == 4) {
              char buf[256];
              std::snprintf(buf, sizeof(buf), "%s%s", r.point_formula[i].empty() ? "" : "=", r.point_formula[i].c_str());
              if (ImGui::InputText("##f", buf, sizeof(buf))) r.point_formula[i] = buf[0] == '=' ? buf + 1 : buf;
            } else {
              double v = r.point_type[i] < 0 ? 50.0 : r.point_value[i];
              if (ImGui::InputDouble("##v", &v, 0, 0, "%.6g")) {
                if (r.point_type[i] < 0) r.point_type[i] = def;
                r.point_value[i] = v;
              }
            }
            ImGui::PopID();
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Color:"));
          for (int k = 0; k < n; ++k) {
            ImGui::TableNextColumn();
            const int i = col(k);
            ImGui::PushID(20 + i);
            PaletteColor("##c", &r.colors[i], false, "");
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
        ImGui::TextUnformatted(TR("Preview:"));
        ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        DrawScaleStrip(ImGui::GetWindowDrawList(), p, p + ImVec2(S(300), ImGui::GetFrameHeight()), n, r.colors[0], r.colors[1], r.colors[2]);
        ImGui::Dummy(ImVec2(S(300), ImGui::GetFrameHeight()));
      } else if (r.type == T::DataBar) {
        // Excel's Data Bar: Show Bar Only; Minimum / Maximum (Type, Value); Bar Appearance: Fill, Color, Border,
        // its color, Bar Direction; the preview
        bool only = !r.bar_show_value;
        if (ImGui::Checkbox(TR("Show Bar Only"), &only)) r.bar_show_value = !only;
        const char* names[] = {TR("Number"), TR("Lowest Value"), TR("Highest Value"), TR("Percent"), TR("Formula"), TR("Percentile"), TR("Automatic")};
        if (ImGui::BeginTable("##bar_points", 3, ImGuiTableFlags_SizingFixedFit)) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(TR("Minimum"));
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(TR("Maximum"));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Type:"));
          for (int i = 0; i < 2; ++i) {
            ImGui::TableNextColumn();
            int& type = i ? r.bar_max_type : r.bar_min_type;
            const int shown = type < 0 ? 6 : std::clamp(type, 0, 5);
            ImGui::PushID(40 + i);
            ImGui::SetNextItemWidth(S(140));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ui::NoteUiItem(i ? "##bar_max_type" : "##bar_min_type", ImRect(at, at + ImVec2(S(140), ImGui::GetFrameHeight())));
            if (ui::BeginCombo("##type", names[shown])) {
              for (int v : {6, i == 0 ? 1 : 2, 0, 3, 4, 5}) {
                if (ImGui::Selectable(names[v], v == shown)) {
                  type = v == 6 ? -1 : v;
                  if (v == 3 || v == 5) (i ? r.bar_max_value : r.bar_min_value) = i ? 100 : 0;
                }
              }
              ImGui::EndCombo();
            }
            ImGui::PopID();
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Value:"));
          for (int i = 0; i < 2; ++i) {
            ImGui::TableNextColumn();
            const int type = i ? r.bar_max_type : r.bar_min_type;
            ImGui::PushID(50 + i);
            ImGui::SetNextItemWidth(S(140));
            if (type < 0 || type == 1 || type == 2) {
              ImGui::TextDisabled("%s", type < 0 ? TR("(Automatic)") : TR("(Lowest / highest)"));
            } else if (type == 4) {
              std::string& fx = i ? r.bar_max_formula : r.bar_min_formula;
              char buf[256];
              std::snprintf(buf, sizeof(buf), "%s%s", fx.empty() ? "" : "=", fx.c_str());
              if (ImGui::InputText("##f", buf, sizeof(buf))) fx = buf[0] == '=' ? buf + 1 : buf;
            } else {
              ImGui::InputDouble("##v", i ? &r.bar_max_value : &r.bar_min_value, 0, 0, "%.6g");
            }
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
        ImGui::SeparatorText(TR("Bar Appearance:"));
        const char* fills[] = {TR("Solid Fill"), TR("Gradient Fill")};
        int fill = r.bar_gradient ? 1 : 0;
        ImGui::SetNextItemWidth(S(140));
        {
          const ImVec2 at = ImGui::GetCursorScreenPos();
          ui::NoteUiItem("##bar_fill", ImRect(at, at + ImVec2(S(140), ImGui::GetFrameHeight())));
        }
        if (ui::Combo(TR("Fill"), &fill, fills, 2)) r.bar_gradient = fill == 1;
        ImGui::SameLine();
        PaletteColor(TR("Color"), &r.bar_color, false, "");
        const char* borders[] = {TR("No Border"), TR("Solid Border")};
        int border = r.bar_border ? 1 : 0;
        ImGui::SetNextItemWidth(S(140));
        {
          const ImVec2 at = ImGui::GetCursorScreenPos();
          ui::NoteUiItem("##bar_border", ImRect(at, at + ImVec2(S(140), ImGui::GetFrameHeight())));
        }
        if (ui::Combo(TR("Border"), &border, borders, 2)) r.bar_border = border ? r.bar_color : 0;
        if (r.bar_border) {
          ImGui::SameLine();
          PaletteColor(TR("Border Color"), &r.bar_border, false, "");
        }
        const char* dirs[] = {TR("Context"), TR("Left-to-Right"), TR("Right-to-Left")};
        ImGui::SetNextItemWidth(S(140));
        ui::Combo(TR("Bar Direction"), &r.bar_direction, dirs, 3);
        ImGui::TextUnformatted(TR("Preview:"));
        ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        DrawBarSample(ImGui::GetWindowDrawList(), p, p + ImVec2(S(200), ImGui::GetFrameHeight()), r.bar_color);
        ImGui::Dummy(ImVec2(S(200), ImGui::GetFrameHeight()));
      } else {
        // Icon Style as pictures, then one row per icon: "when value is >= [value] [type]" (Excel)
        const char* cur = "";
        for (const auto& k : IconPresets()) {
          if (k.style == r.icon_style) cur = k.label;
        }
        ImGui::SetNextItemWidth(S(260));
        if (ui::BeginCombo(TR("Icon Style"), cur)) {
          for (const auto& k : IconPresets()) {
            ImGui::PushID(k.style);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##set", k.style == r.icon_style, 0, ImVec2(S(240), S(20)))) {
              r.icon_style = k.style;
              r.icon_thresholds = model::DefaultIconThresholds(k.style);
            }
            ui::NoteUiItem(k.label, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
            const float w = DrawIconRow(ImGui::GetWindowDrawList(), p, S(20), k.style);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + w + S(8), p.y + (S(20) - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), k.label);
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
        ImGui::Checkbox(TR("Reverse Icon Order"), &r.icon_reverse);
        ImGui::SameLine();
        ImGui::Checkbox(TR("Show Icon Only"), &r.icon_only);
        const int n = model::IconSetCount(r.icon_style);
        if (r.icon_thresholds.size() + 1 != static_cast<size_t>(n)) r.icon_thresholds = model::DefaultIconThresholds(r.icon_style);
        const char* types[] = {TR("Percent"), TR("Number"), TR("Percentile"), TR("Formula")};
        for (int icon = n - 1; icon >= 0; --icon) {
          ImGui::PushID(100 + icon);
          const ImVec2 p = ImGui::GetCursorScreenPos();
          const float sz = S(16);
          render::DrawRuleIcon(ImGui::GetWindowDrawList(), ImRect(p + ImVec2(0, S(3)), p + ImVec2(sz, sz + S(3))), r.icon_style, r.icon_reverse ? n - 1 - icon : icon);
          ImGui::Dummy(ImVec2(sz, sz + S(4)));
          ImGui::SameLine();
          if (icon == 0) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("when value is lower than the one above"));
          } else {
            model::IconThreshold& t = r.icon_thresholds[static_cast<size_t>(icon - 1)];
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("when value is"));
            ImGui::SameLine();
            int g = t.gte ? 0 : 1;
            const char* cmp[] = {">=", ">"};
            ImGui::SetNextItemWidth(S(50));
            if (ui::Combo("##gte", &g, cmp, 2)) t.gte = g == 0;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(110));
            if (t.type == 3) {
              char buf[256];
              std::snprintf(buf, sizeof(buf), "=%s", t.formula.c_str());
              if (ImGui::InputText("##val", buf, sizeof(buf))) t.formula = buf[0] == '=' ? buf + 1 : buf;
            } else {
              ImGui::InputDouble("##val", &t.value, 0, 0, "%.6g");
            }
            ImGui::SameLine();
            int ty = std::clamp(t.type, 0, 3);
            ImGui::SetNextItemWidth(S(110));
            if (ui::Combo("##type", &ty, types, 4)) t.type = ty;
          }
          ImGui::PopID();
        }
      }
      break;
    }
    case 1: {
      // Cell Value / Specific Text / Dates Occurring / Blanks / No Blanks / Errors / No Errors
      const char* what[] = {TR("Cell Value"), TR("Specific Text"), TR("Dates Occurring"), TR("Blanks"), TR("No Blanks"), TR("Errors"), TR("No Errors")};
      int w = r.type == T::TextContains ? 1 : r.type == T::TimePeriod ? 2 : r.type == T::Blanks ? 3 : r.type == T::NoBlanks ? 4 : r.type == T::Errors ? 5 : r.type == T::NoErrors ? 6 : 0;
      ImGui::TextUnformatted(TR("Format only cells with:"));
      ImGui::SetNextItemWidth(S(140));
      ui::Combo("##what", &w, what, 7);
      r.type = w == 1 ? T::TextContains : w == 2 ? T::TimePeriod : w == 3 ? T::Blanks : w == 4 ? T::NoBlanks : w == 5 ? T::Errors : w == 6 ? T::NoErrors : T::CellValue;
      if (w == 0) {
        static const char* kOps[] = {TR("between"), TR("not between"), TR("equal to"), TR("not equal to"), TR("greater than"), TR("less than"), TR("greater than or equal to"), TR("less than or equal to")};
        int op = std::clamp(static_cast<int>(r.op), 1, 8) - 1;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(170));
        if (ui::Combo("##op", &op, kOps, 8)) r.op = static_cast<model::CmpOp>(op + 1);
        const bool two = r.op == model::CmpOp::Between || r.op == model::CmpOp::NotBetween;
        RangeBox(s, "cfe.f1", &u.cfe_f1, two ? S(240) : S(500), RangeBox_Formula | RangeBox_Equals, TR("Value"));
        if (two) {
          ImGui::SameLine();
          ImGui::TextUnformatted(TR("and##ctx_with"));
          ImGui::SameLine();
          RangeBox(s, "cfe.f2", &u.cfe_f2, S(240), RangeBox_Formula | RangeBox_Equals, TR("Value"));
        }
      } else if (w == 1) {
        // Excel's text operators
        const char* ops[] = {TR("containing"), TR("not containing"), TR("beginning with"), TR("ending with")};
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(140));
        ui::Combo("##textop", &r.text_op, ops, 4);
        ImGui::SameLine();
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", r.text.c_str());
        ImGui::SetNextItemWidth(S(200));
        if (ImGui::InputText("##text", buf, sizeof buf)) r.text = buf;
      } else if (w == 2) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(180));
        ui::NoteUiItem("##period", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight())));  // combos aren't recorded
        if (ui::BeginCombo("##period", PeriodName(r.time_period))) {
          for (int k : kPeriodOrder) {
            if (ImGui::Selectable(PeriodName(k), r.time_period == k)) r.time_period = k;
          }
          ImGui::EndCombo();
        }
      }
      break;
    }
    case 2: {
      r.type = T::Top10;
      const char* tb[] = {TR("Top"), TR("Bottom")};
      int b = r.bottom ? 1 : 0;
      ImGui::SetNextItemWidth(S(100));
      if (ui::Combo("##tb", &b, tb, 2)) r.bottom = b == 1;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(120));
      ImGui::InputInt("##rank", &r.rank, 1, 10);
      r.rank = std::clamp(r.rank, 1, r.percent ? 100 : 1000);
      ImGui::SameLine();
      // "%" kept apart: the translation check reads "% o" as a printf format
      ImGui::Checkbox(("% " + std::string(TR("of the selected range"))).c_str(), &r.percent);
      break;
    }
    case 3: {
      r.type = T::AboveAverage;
      // Excel's ten choices: above / below, equal or above / below, 1 to 3 standard deviations above / below
      const char* ab[] = {TR("above"), TR("below"), TR("equal or above"), TR("equal or below"), TR("1 std dev above"), TR("1 std dev below"),
                          TR("2 std dev above"), TR("2 std dev below"), TR("3 std dev above"), TR("3 std dev below")};
      int b = r.std_dev ? 2 + r.std_dev * 2 + (r.below ? 1 : 0) : (r.equal_average ? 2 : 0) + (r.below ? 1 : 0);
      ImGui::TextUnformatted(TR("Format values that are:"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(160));
      if (ui::Combo("##ab", &b, ab, 10)) {
        r.below = b % 2 == 1;
        r.equal_average = b == 2 || b == 3;
        r.std_dev = b >= 4 ? (b - 2) / 2 : 0;
      }
      ImGui::SameLine();
      ImGui::TextUnformatted(TR("the average for the selected range"));
      break;
    }
    case 4: {
      r.type = T::Unique;
      const char* du[] = {TR("duplicate"), TR("unique")};
      int d = r.duplicate ? 0 : 1;
      ImGui::TextUnformatted(TR("Format all:"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(140));
      if (ui::Combo("##du", &d, du, 2)) r.duplicate = d == 0;
      ImGui::SameLine();
      ImGui::TextUnformatted(TR("values in the selected range"));
      break;
    }
    default:
      r.type = T::Expression;
      ImGui::TextUnformatted(TR("Format values where this formula is true:"));
      RangeBox(s, "cfe.formula", &u.cfe_f1, 0, RangeBox_Formula | RangeBox_Equals, TR("Format values where this formula is true:"));
      ImGui::TextDisabled("%s", TR("Relative references follow each cell, from the selection's top-left cell."));
      break;
  }
  if (has_format) {
    // the format: a preview, and its fill, font color and font style (Excel's Format... button)
    ImGui::SeparatorText(TR("Format"));
    ImGui::TextUnformatted(TR("Preview:"));
    ImGui::SameLine();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(S(260), ImGui::GetFrameHeight());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const uint32_t fill = r.fmt.has_fill ? model::ResolveColor(r.fmt.fill_color) : 0xFFFFFFFF;
    dl->AddRectFilled(p, p + sz, IM_COL32((fill >> 16) & 0xFF, (fill >> 8) & 0xFF, fill & 0xFF, 255));
    dl->AddRect(p, p + sz, IM_COL32(0xB0, 0xB0, 0xB0, 255));
    const uint32_t fc = r.fmt.has_font_color ? model::ResolveColor(r.fmt.font_color) : 0xFF000000u;
    ImFont* font = r.fmt.bold == 1 && ui::F().ui_bold ? ui::F().ui_bold : ui::F().ui;
    // Excel: "No Format Set" until a format is chosen
    const bool none = !r.fmt.has_fill && !r.fmt.has_font_color && r.fmt.bold != 1 && r.fmt.italic != 1 && r.fmt.strike != 1 && !(r.fmt.border_color >> 24);
    const char* sample = none ? TR("No Format Set") : TR("AaBbCcYyZz");
    const ImVec2 ts = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, sample);
    const ImVec2 tp(p.x + (sz.x - ts.x) * 0.5f, p.y + (sz.y - ts.y) * 0.5f);
    dl->AddText(font, ImGui::GetFontSize(), tp, IM_COL32((fc >> 16) & 0xFF, (fc >> 8) & 0xFF, fc & 0xFF, 255), sample);
    if (r.fmt.strike == 1) dl->AddLine(ImVec2(tp.x, tp.y + ts.y * 0.55f), ImVec2(tp.x + ts.x, tp.y + ts.y * 0.55f), IM_COL32((fc >> 16) & 0xFF, (fc >> 8) & 0xFF, fc & 0xFF, 255));
    if (r.fmt.border_color >> 24) {
      const uint32_t bc = model::ResolveColor(r.fmt.border_color);
      dl->AddRect(p, p + sz, IM_COL32((bc >> 16) & 0xFF, (bc >> 8) & 0xFF, bc & 0xFF, 255), 0.0f, S(1.5f));
    }
    ImGui::Dummy(sz);
    // Excel's Format...: Format Cells with the Number, Font, Border and Fill tabs; the editor comes back after it
    ImGui::SameLine();
    if (ImGui::Button(TR("Format..."))) {
      const model::CfFormat& x = r.fmt;
      engine::FormatSpec sp;
      sp.style.fill_color = x.has_fill ? x.fill_color : 0;
      sp.style.font_color = x.has_font_color ? x.font_color : 0;
      sp.style.bold = x.bold == 1;
      sp.style.italic = x.italic == 1;
      sp.style.strike = x.strike == 1;
      if (x.underline >= 0) sp.style.underline = static_cast<model::Underline>(x.underline);
      if (x.pattern >= 0) sp.style.pattern = static_cast<model::FillPattern>(x.pattern), sp.style.pattern_color = x.pattern_color;
      static const int kSide[4] = {model::kLeft, model::kTop, model::kRight, model::kBottom};
      for (int k = 0; k < 4; ++k) {
        if (x.side_style[k]) sp.style.border[kSide[k]] = model::BorderSide{static_cast<model::LineStyle>(x.side_style[k]), x.side_color[k]};
        else if (x.border_color >> 24) sp.style.border[kSide[k]] = model::BorderSide{model::LineStyle::Thin, x.border_color};
      }
      sp.numfmt = x.numfmt;
      if (!x.numfmt.empty()) sp.mask |= engine::kFmNumber;
      if (x.has_fill || x.pattern >= 0) sp.mask |= engine::kFmFill;
      if (x.has_font_color) sp.mask |= engine::kFmFontColor;
      if (x.bold >= 0) sp.mask |= engine::kFmBold;
      if (x.italic >= 0) sp.mask |= engine::kFmItalic;
      if (x.strike >= 0) sp.mask |= engine::kFmStrike;
      if (x.underline >= 0) sp.mask |= engine::kFmUnderline;
      u.cfe_reopen = true;
      ImGui::CloseCurrentPopup();
      OpenFormatCellsFor(
          s, TR("Format Cells"), sp, 1u | 4u | 8u | 16u, model::BorderSide{}, model::BorderSide{},
          [](const engine::FormatSpec& out, model::BorderSide, model::BorderSide) {
            model::CfFormat& f = R().cfe.fmt;
            const model::Style& st = out.style;
            f.has_fill = (out.mask & engine::kFmFill) && (st.fill_color >> 24);
            f.fill_color = f.has_fill ? st.fill_color : 0;
            f.pattern = (out.mask & engine::kFmFill) && st.pattern != model::FillPattern::Solid ? static_cast<int8_t>(st.pattern) : int8_t{-1};
            f.pattern_color = f.pattern >= 0 ? st.pattern_color : 0;
            f.has_font_color = (out.mask & engine::kFmFontColor) && (st.font_color >> 24);
            f.font_color = f.has_font_color ? st.font_color : 0;
            f.bold = (out.mask & engine::kFmBold) ? (st.bold ? 1 : 0) : -1;
            f.italic = (out.mask & engine::kFmItalic) ? (st.italic ? 1 : 0) : -1;
            f.strike = (out.mask & engine::kFmStrike) ? (st.strike ? 1 : 0) : -1;
            f.underline = (out.mask & engine::kFmUnderline) ? static_cast<int8_t>(st.underline) : int8_t{-1};
            f.numfmt = (out.mask & engine::kFmNumber) ? out.numfmt : std::string();
            if (f.numfmt == "General") f.numfmt.clear();
            static const int kSide2[4] = {model::kLeft, model::kTop, model::kRight, model::kBottom};
            f.border_color = 0;  // the lines are kept side by side now
            for (int k = 0; k < 4; ++k) {
              const model::BorderSide& b = st.border[kSide2[k]];
              f.side_style[k] = static_cast<uint8_t>(b.style);
              f.side_color[k] = b.style == model::LineStyle::None ? 0u : (b.color ? b.color : 0xFF000000u);
            }
          });
    }
    uint32_t fillc = r.fmt.has_fill ? r.fmt.fill_color : 0;
    if (PaletteColor(TR("Fill"), &fillc, true, TR("No Color"))) {
      r.fmt.has_fill = fillc != 0;
      r.fmt.fill_color = fillc;
    }
    uint32_t fontc = r.fmt.has_font_color ? r.fmt.font_color : 0;
    if (PaletteColor(TR("Font Color"), &fontc, true, TR("Automatic"))) {
      r.fmt.has_font_color = fontc != 0;
      r.fmt.font_color = fontc;
    }
    uint32_t borderc = (r.fmt.border_color >> 24) ? r.fmt.border_color : 0;
    if (PaletteColor(TR("Border"), &borderc, true, TR("No Border"))) r.fmt.border_color = borderc;
    bool b = r.fmt.bold == 1, it = r.fmt.italic == 1, st = r.fmt.strike == 1;
    if (ImGui::Checkbox(TR("Bold"), &b)) r.fmt.bold = b ? 1 : -1;
    ImGui::SameLine();
    if (ImGui::Checkbox(TR("Italic"), &it)) r.fmt.italic = it ? 1 : -1;
    ImGui::SameLine();
    if (ImGui::Checkbox(TR("Strikethrough"), &st)) r.fmt.strike = st ? 1 : -1;
  }
  ImGui::Separator();
  bool close = false;
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    model::CfRule rule = r;
    auto strip = [](std::string v) {
      while (!v.empty() && (v[0] == '=' || v[0] == ' ')) v.erase(0, 1);
      return v;
    };
    rule.formula1 = strip(u.cfe_f1);
    rule.formula2 = strip(u.cfe_f2);
    const uint32_t sheet = s.focus_sheet;
    if (u.cfe_from_manager && u.cfm_loaded) {
      // from the Rules Manager: the change goes into its copy (OK / Apply there writes it)
      if (u.cfe_id) {
        for (auto& x : u.cfm_rules) {
          if (x.id != u.cfe_id) continue;
          const auto ranges = x.ranges;
          x = rule;
          x.id = u.cfe_id;
          x.ranges = ranges;
        }
      } else {
        rule.id = u.cfm_next_id++;
        rule.ranges = SelectionAreasClipped(s);
        u.cfm_rules.insert(u.cfm_rules.begin(), rule);
      }
      u.cfm_dirty = true;
    } else if (u.cfe_id) {
      const uint32_t id = u.cfe_id;
      s.engine->PostUser(TR("Edit Formatting Rule"), [sheet, id, rule](Document& d) {
        d.UpdateCondFormat(sheet, id, [&](model::CfRule& x) {
          const auto ranges = x.ranges;
          const int pr = x.priority;
          x = rule;
          x.id = id;
          x.ranges = ranges;
          x.priority = pr;
        });
      });
    } else {
      AddRule(s, rule, TR("New Formatting Rule"));
    }
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
  EndDlg(s, close);
  if (close && u.cfe_from_manager) {
    u.cfe_from_manager = false;
    s.OpenDialog(Dialog::CfManage);
  }
}

// a rule in the editor (Edit Rule / New Rule from the manager or the menu)
void OpenRuleEditor(AppState& s, const model::CfRule* rule, bool from_manager) {
  RulesUi& u = R();
  u.cfe = rule ? *rule : model::CfRule{};
  u.cfe_id = rule ? rule->id : 0;
  u.cfe_from_manager = from_manager;
  u.cfe_f1 = rule ? (rule->formula1.empty() ? std::string() : "=" + rule->formula1) : std::string();
  u.cfe_f2 = rule ? (rule->formula2.empty() ? std::string() : "=" + rule->formula2) : std::string();
  using T = model::CfType;
  const T t = u.cfe.type;
  u.cfe_kind = !rule ? 0 : t == T::ColorScale || t == T::DataBar || t == T::IconSet ? 0 : t == T::Top10 ? 2 : t == T::AboveAverage ? 3 : t == T::Unique ? 4 : t == T::Expression ? 5 : 1;
  if (!rule) {
    // Excel's new rule: a 2-color scale on the first type; the other types start with "No Format Set" and an
    // empty formula
    u.cfe.type = T::ColorScale;
    u.cfe.points = 2;
    u.cfe.colors[0] = 0xFFF8696B;
    u.cfe.colors[2] = 0xFF63BE7B;
    u.cfe_f1.clear();
    u.cfe_kind = 0;
  }
  s.OpenDialog(Dialog::CfNewRule);
}

std::string Operand(const char* v) {
  std::string t = v;
  if (t.empty()) return t;
  if (t[0] == '=') return t.substr(1);
  char* end = nullptr;
  std::strtod(t.c_str(), &end);
  if (end && *end == '\0') return t;
  // text: add quotes
  std::string q = "\"";
  for (char ch : t) {
    q += ch;
    if (ch == '"') q += '"';
  }
  return q + "\"";
}

std::string Describe(const model::CfRule& r) {
  switch (r.type) {
    case model::CfType::CellValue: {
      static const char* kOps[] = {"", TR("between"), TR("not between"), TR("equal to"), TR("not equal to"), TR("greater than"), TR("less than"), TR("greater than or equal to"), TR("less than or equal to")};
      const int op = static_cast<int>(r.op);
      std::string d = std::string(TR("Cell value ")) + (op >= 1 && op <= 8 ? kOps[op] : "?") + " " + r.formula1;
      if (r.op == model::CmpOp::Between || r.op == model::CmpOp::NotBetween) d += TR(" and ##ctx_with") + r.formula2;
      return d;
    }
    case model::CfType::Expression: return TR("Formula: =") + r.formula1;
    case model::CfType::ColorScale: return std::to_string(r.points) + TR("-Color Scale");
    case model::CfType::DataBar: return TR("Data Bars");
    case model::CfType::IconSet: return std::to_string(model::IconSetCount(r.icon_style)) + TR("-Icon Set");
    case model::CfType::Top10: return std::string(r.bottom ? TR("Bottom ") : TR("Top ")) + std::to_string(r.rank) + (r.percent ? "%" : TR(" items"));
    case model::CfType::Unique: return r.duplicate ? TR("Duplicate values") : TR("Unique values");
    case model::CfType::TextContains: {
      const char* ops[] = {TR("Cell value contains \""), TR("Cell value does not contain \""), TR("Cell value begins with \""), TR("Cell value ends with \"")};
      return ops[std::clamp(r.text_op, 0, 3)] + r.text + TR("\"##ctx_close");
    }
    case model::CfType::TimePeriod: return std::string(TR("Dates Occurring: ")) + PeriodName(r.time_period);
    case model::CfType::AboveAverage:
      if (r.std_dev) return std::to_string(r.std_dev) + (r.below ? TR(" std dev below average") : TR(" std dev above average"));
      if (r.equal_average) return r.below ? TR("Equal to or below average") : TR("Equal to or above average");
      return r.below ? TR("Below average") : TR("Above average");
    case model::CfType::Blanks: return TR("Blanks##ctx_empty");
    case model::CfType::NoBlanks: return TR("No blanks");
    case model::CfType::Errors: return TR("Errors");
    case model::CfType::NoErrors: return TR("No errors");
  }
  return "?";
}

}  // namespace

void DrawRuleDialogs(AppState& s) {
  RulesUi& u = R();
  // Custom AutoFilter (from the Text / Number / Date Filters submenus)
  if (BeginDlg(s, Dialog::CustomAutoFilter, TR("Custom AutoFilter###yc_dlg_customaf"), 480)) {
    FilterUi& f = u.filter;
    static const char* kOps[] = {TR("equals"), TR("does not equal"), TR("is greater than"), TR("is greater than or equal to"), TR("is less than"),
                                 TR("is less than or equal to"), TR("begins with"), TR("does not begin with"), TR("ends with"), TR("does not end with"),
                                 TR("contains"), TR("does not contain")};
    const model::Sheet* sh = s.snap->Sheet(f.sheet);
    Rect frange{};
    const uint32_t col = sh && FilterOf(*sh, f.table, &frange) ? frange.c0 + f.field : 0;
    // the column's heading, as Excel names it (the letter only when the heading is empty)
    std::string heading = core::ColumnName(col);
    if (sh) {
      const auto& tbl = *s.snap->wb.styles;
      const std::string h = engine::DisplayTextOf(sh->GetValue(frange.r0, col), tbl.NumberFormat(tbl.Get(sh->GetStyle(frange.r0, col))));
      if (!h.empty()) heading = h;
    }
    ImGui::TextUnformatted(TR("Show rows where:"));
    ImGui::TextUnformatted(heading.c_str());
    ui::NoteUiText(heading.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    // operator, value, and a list of the column's values to pick from
    auto row = [&](const char* id, int* op, char* v) {
      ImGui::PushID(id);
      ImGui::SetNextItemWidth(S(180));
      ui::Combo("##op", op, kOps, 12);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(200));
      // the keyboard starts in the first value box, as in Excel (Begins With..., then type A and Enter)
      if (v == f.v1 && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::InputText("##v", v, 128);
      ImGui::SameLine(0, 0);
      if (ui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_HeightLarge | ImGuiComboFlags_PopupAlignLeft)) {
        for (size_t k = 0; k < f.values.size(); ++k) {
          const auto& val = f.values[k];
          ImGui::PushID(static_cast<int>(k));
          const bool picked = CellTextSelectable(val, false);  // the text as it is ("C##1" too)
          ImGui::PopID();
          if (picked) std::snprintf(v, 128, "%s", val.c_str());
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    };
    row("a", &f.op1, f.v1);
    if (ImGui::RadioButton(TR("And"), !f.or_op)) f.or_op = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Or"), f.or_op)) f.or_op = true;
    row("b", &f.op2, f.v2);
    ImGui::TextDisabled("%s", TR("Use ? to represent any single character. Use * to represent any series of characters."));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      auto crit = [](int op, const char* v) -> std::string {
        if (!v[0]) return {};
        const std::string x = v;
        switch (op) {
          case 0: return "=" + x;
          case 1: return "<>" + x;
          case 2: return ">" + x;
          case 3: return ">=" + x;
          case 4: return "<" + x;
          case 5: return "<=" + x;
          case 6: return "=" + x + "*";
          case 7: return "<>" + x + "*";
          case 8: return "=*" + x;
          case 9: return "<>*" + x;
          case 10: return "=*" + x + "*";
          default: return "<>*" + x + "*";
        }
      };
      model::FilterColumn fc;
      fc.field = f.field;
      fc.kind = 2;
      fc.crit1 = crit(f.op1, f.v1);
      fc.crit2 = crit(f.op2, f.v2);
      if (fc.crit1.empty()) std::swap(fc.crit1, fc.crit2);
      fc.or_op = f.or_op;
      if (fc.crit1.empty()) fc.kind = 0;
      PostFilterColumn(s, TR("Filter"), f.sheet, f.table, fc);
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Top 10 AutoFilter
  if (BeginDlg(s, Dialog::Top10Filter, TR("Top 10 AutoFilter###yc_dlg_top10af"), 380)) {
    FilterUi& f = u.filter;
    static const char* kTop[] = {TR("Top"), TR("Bottom")};
    static const char* kKind[] = {TR("Items"), TR("Percent")};
    ImGui::SetNextItemWidth(S(90));
    ui::Combo("##top", &f.t_top, kTop, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(90));
    ImGui::InputInt("##n", &f.t_count);
    f.t_count = std::clamp(f.t_count, 1, 500);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(90));
    ui::Combo("##kind", &f.t_pct, kKind, 2);
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      model::FilterColumn fc;
      fc.field = f.field;
      fc.kind = 3;
      fc.top = f.t_top == 0;
      fc.count = f.t_count;
      fc.percent = f.t_pct == 1;
      PostFilterColumn(s, TR("Filter"), f.sheet, f.table, fc);
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // highlight rules and top/bottom: each dialog titled as Excel's, with a live text of what it formats
  {
    const int k = std::clamp(u.cf_kind, 0, 13);
    static const char* kNames[] = {"", TR("Greater Than"), TR("Less Than"), TR("Between"), TR("Equal To"), TR("Text That Contains"), TR("Duplicate Values"),
                                   TR("Top 10 Items"), TR("Bottom 10 Items"), TR("Above Average"), TR("Below Average"), TR("Top 10%"), TR("Bottom 10%"),
                                   TR("A Date Occurring")};
    const std::string title = std::string(kNames[k]) + "###yc_dlg_cfquick";
    if (BeginDlg(s, Dialog::CfQuick, title.c_str(), 420)) {
      static const char* kTitles[] = {"", TR("Format cells that are GREATER THAN: "), TR("Format cells that are LESS THAN: "), TR("Format cells that are BETWEEN: "), TR("Format cells that are EQUAL TO: "),
                                      TR("Format cells that contain the text: "), TR("Format cells that contain: "), TR("Format cells that rank in the TOP: "), TR("Format cells that rank in the BOTTOM: "),
                                      TR("Format cells that are above average"), TR("Format cells that are below average"), TR("Format cells that rank in the TOP: "),
                                      TR("Format cells that rank in the BOTTOM: "), TR("Format cells that contain a date occurring: ")};
      ImGui::TextUnformatted(kTitles[k]);
      if (k <= 5 || k == 7 || k == 8 || k == 11 || k == 12) {
        // a value, or a cell picked on the sheet with the box's button (=$B$1), as in Excel
        const bool rank = k == 7 || k == 8 || k == 11 || k == 12;
        const int fl = rank ? 0 : RangeBox_Formula | RangeBox_Equals;
        RangeBox(s, "cf.v1", u.cf_v1, sizeof(u.cf_v1), k == 3 ? S(180) : rank ? S(120) : S(260), fl, kTitles[k]);
        if (k == 3) {
          ImGui::SameLine();
          ImGui::TextUnformatted(TR("and##ctx_with"));
          ImGui::SameLine();
          RangeBox(s, "cf.v2", u.cf_v2, sizeof(u.cf_v2), S(180), fl, kTitles[3]);
        }
        if (k == 11 || k == 12) {
          ImGui::SameLine();
          ImGui::TextUnformatted("%");
        }
      }
      if (k == 6) {
        // Duplicate Values: "Format cells that contain: [Duplicate / Unique] values with"
        const char* du[] = {TR("Duplicate"), TR("Unique")};
        ImGui::SetNextItemWidth(S(140));
        ui::NoteUiItem("##dupe", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight())));  // combos aren't recorded
        ui::Combo("##dupe", &u.cf_dupe, du, 2);
        ImGui::SameLine();
        ImGui::TextUnformatted(TR("values with"));
      }
      if (k == 13) {
        ImGui::SetNextItemWidth(S(200));
        ui::NoteUiItem("##period", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(ImGui::CalcItemWidth(), ImGui::GetFrameHeight())));  // combos aren't recorded
        if (ui::BeginCombo("##period", PeriodName(u.cf_period))) {
          for (int p : kPeriodOrder) {
            if (ImGui::Selectable(PeriodName(p), u.cf_period == p)) u.cf_period = p;
          }
          ImGui::EndCombo();
        }
      }
      ImGui::TextUnformatted(TR("Display as"));
      ImGui::SetNextItemWidth(-1);
      u.cf_preset = std::clamp(u.cf_preset, 0, static_cast<int>(std::size(kPresets)) - 1);
      bool custom = false;
      ui::NoteUiItem("##preset", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())));
      if (ui::BeginCombo("##preset", kPresets[u.cf_preset].name)) {
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
          if (ImGui::Selectable(kPresets[i].name, u.cf_preset == i)) u.cf_preset = i;
        }
        if (ImGui::Selectable(TR("Custom Format..."))) custom = true;
        ImGui::EndCombo();
      }
      bool close = false;
      // the rule as it stands, previewed on the selection while the dialog is open (Excel)
      const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
      {
        model::CfRule r;
        const Preset& p = kPresets[u.cf_preset];
        r.fmt.has_fill = p.fill != 0;
        r.fmt.fill_color = p.fill;
        r.fmt.has_font_color = p.font != 0;
        r.fmt.font_color = p.font;
        r.fmt.border_color = p.border;
        if (p.bold) r.fmt.bold = 1;
        switch (k) {
          case 1: case 2: case 3: case 4:
            r.type = model::CfType::CellValue;
            r.op = k == 1 ? model::CmpOp::Greater : k == 2 ? model::CmpOp::Less : k == 3 ? model::CmpOp::Between : model::CmpOp::Equal;
            r.formula1 = Operand(u.cf_v1);
            r.formula2 = Operand(u.cf_v2);
            break;
          case 5: r.type = model::CfType::TextContains; r.text = u.cf_v1; break;
          case 6: r.type = model::CfType::Unique; r.duplicate = u.cf_dupe == 0; break;
          case 7: case 8: case 11: case 12:
            r.type = model::CfType::Top10;
            r.bottom = k == 8 || k == 12;
            r.percent = k >= 11;
            r.rank = std::clamp(std::atoi(u.cf_v1), 1, r.percent ? 100 : 1000);
            break;
          case 13: r.type = model::CfType::TimePeriod; r.time_period = u.cf_period; break;
          default: r.type = model::CfType::AboveAverage; r.below = k == 10; break;
        }
        g_quick_preview = r;
        g_quick_preview.ranges = SelectionAreasClipped(s);
        g_quick_preview_on = !((k <= 4 && r.formula1.empty()) || (k == 3 && r.formula2.empty()));
        if (ok_pressed || custom) {
        g_quick_preview_on = false;
        if (custom) {
          // Custom Format...: the same rule in the rule editor, whose Format part sets fill, font, style and border
          r.fmt = model::CfFormat{};
          r.ranges = SelectionAreasClipped(s);
          EndDlg(s, true);
          OpenRuleEditor(s, &r, false);
          return;
        }
        if ((k <= 4 && r.formula1.empty()) || (k == 3 && r.formula2.empty())) {
          s.SetStatus(TR("Enter a value to compare"));
        } else {
          AddRule(s, r, TR("Conditional formats"));
          close = true;
        }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
      if (close) g_quick_preview_on = false;
      EndDlg(s, close);
    }
  }
  DrawRuleEditor(s);
  // rules manager: edits a copy of the sheet's rules; OK / Apply write it as one undo step, Cancel drops it (Excel)
  if (s.dialog != Dialog::CfManage && s.dialog != Dialog::CfNewRule && !RangePicking()) u.cfm_loaded = false;
  if (s.dialog == Dialog::CfManage && !u.cfm_loaded) {
    u.cfm_rules.clear();
    u.cfm_sheet = s.focus_sheet;
    if (const model::Sheet* sh0 = s.FocusSheet()) u.cfm_rules = *sh0->cond_formats;
    std::stable_sort(u.cfm_rules.begin(), u.cfm_rules.end(), [](const model::CfRule& a, const model::CfRule& b) { return a.priority < b.priority; });
    u.cfm_loaded = true;
    u.cfm_dirty = false;
  }
  if (BeginDlg(s, Dialog::CfManage, TR("Conditional Formatting Rules Manager###yc_dlg_cfman"), 720)) {
    // Show formatting rules for: the current selection, this worksheet, or another sheet (Excel lists them all);
    // another sheet's rules are edited the same way and written to that sheet
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Show formatting rules for:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(180));
    {
      std::vector<std::pair<std::string, uint32_t>> others;  // the other sheets: name, id
      if (s.snap) {
        for (const auto& x : s.snap->wb.sheets) {
          if (x->id != s.focus_sheet && x->visible) others.emplace_back(x->name, x->id);
        }
      }
      std::string shown = u.cfm_sheet != s.focus_sheet ? std::string() : u.cfm_scope == 0 ? TR("Current Selection") : TR("This Worksheet");
      for (const auto& [name, id] : others) {
        if (id == u.cfm_sheet) shown = name;
      }
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ui::NoteUiItem("##scope", ImRect(at, at + ImVec2(S(180), ImGui::GetFrameHeight())));
      auto load = [&](uint32_t id) {
        if (u.cfm_dirty) {
          // the edits so far go to their sheet first (as Apply)
          std::vector<model::CfRule> out = u.cfm_rules;
          for (auto& r : out) {
            if (r.id >= 0x80000000u) r.id = 0;
          }
          const uint32_t old = u.cfm_sheet;
          s.engine->PostUser(TR("Conditional Formatting Rules Manager"), [old, out](Document& d) { d.SetCondFormats(old, out); });
          s.engine->Sync();
          s.snap = s.engine->Current();
          u.cfm_dirty = false;
        }
        u.cfm_sheet = id;
        u.cfm_rules.clear();
        if (const model::Sheet* x = s.snap ? s.snap->Sheet(id) : nullptr) u.cfm_rules = *x->cond_formats;
        std::stable_sort(u.cfm_rules.begin(), u.cfm_rules.end(), [](const model::CfRule& a, const model::CfRule& b) { return a.priority < b.priority; });
      };
      if (ui::BeginCombo("##scope", shown.c_str())) {
        if (ImGui::Selectable(TR("Current Selection"), u.cfm_sheet == s.focus_sheet && u.cfm_scope == 0)) {
          if (u.cfm_sheet != s.focus_sheet) load(s.focus_sheet);
          u.cfm_scope = 0;
        }
        if (ImGui::Selectable(TR("This Worksheet"), u.cfm_sheet == s.focus_sheet && u.cfm_scope == 1)) {
          if (u.cfm_sheet != s.focus_sheet) load(s.focus_sheet);
          u.cfm_scope = 1;
        }
        for (const auto& [name, id] : others) {
          if (ImGui::Selectable(name.c_str(), u.cfm_sheet == id)) {
            if (u.cfm_sheet != id) load(id);
            u.cfm_scope = 1;
          }
        }
        ImGui::EndCombo();
      }
    }
    const auto sel_areas = SelectionAreasClipped(s);
    std::vector<model::CfRule*> rules;
    for (auto& r : u.cfm_rules) {
      bool show = u.cfm_scope == 1 || u.cfm_sheet != s.focus_sheet;
      for (const Rect& x : r.ranges) {
        for (const Rect& a2 : sel_areas) show = show || x.Intersects(a2);
      }
      if (show) rules.push_back(&r);
    }
    static uint32_t sel_id = 0;
    model::CfRule* sel = nullptr;
    for (auto* r : rules) {
      if (r->id == sel_id) sel = r;
    }
    const uint32_t sheet = u.cfm_sheet;
    if (ImGui::Button(TR("New Rule..."))) OpenRuleEditor(s, nullptr, true);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(TR("Edit Rule...")) && sel) OpenRuleEditor(s, sel, true);
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete Rule")) && sel) {
      const uint32_t id = sel->id;
      u.cfm_rules.erase(std::remove_if(u.cfm_rules.begin(), u.cfm_rules.end(), [id](const model::CfRule& x) { return x.id == id; }), u.cfm_rules.end());
      u.cfm_dirty = true;
      sel = nullptr;
      sel_id = 0;
      rules.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Duplicate Rule")) && sel) {
      model::CfRule copy = *sel;
      copy.id = u.cfm_next_id++;
      const uint32_t id = sel->id;
      for (size_t i = 0; i < u.cfm_rules.size(); ++i) {
        if (u.cfm_rules[i].id == id) {
          u.cfm_rules.insert(u.cfm_rules.begin() + static_cast<std::ptrdiff_t>(i), copy);
          break;
        }
      }
      u.cfm_dirty = true;
      sel = nullptr;
      rules.clear();
    }
    ImGui::SameLine();
    auto move = [&](int delta) {
      if (!sel) return;
      const uint32_t id = sel->id;
      for (size_t i = 0; i < u.cfm_rules.size(); ++i) {
        if (u.cfm_rules[i].id != id) continue;
        const int64_t j = static_cast<int64_t>(i) + delta;
        if (j >= 0 && j < static_cast<int64_t>(u.cfm_rules.size())) std::swap(u.cfm_rules[i], u.cfm_rules[static_cast<size_t>(j)]);
        break;
      }
      u.cfm_dirty = true;
      sel = nullptr;
      rules.clear();
    };
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move(-1);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move(+1);
    ImGui::EndDisabled();
    if (ImGui::BeginTable("##rules", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(S(700), S(240)))) {
      ImGui::TableSetupColumn(TR("Rule (applied in order shown)"));
      ImGui::TableSetupColumn(TR("Format"), ImGuiTableColumnFlags_WidthFixed, S(110));
      ImGui::TableSetupColumn(TR("Applies to"), ImGuiTableColumnFlags_WidthFixed, S(160));
      ImGui::TableSetupColumn(TR("Stop If True"), ImGuiTableColumnFlags_WidthFixed, S(90));
      ImGui::TableHeadersRow();
      for (auto* r : rules) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(r->id));
        if (ImGui::Selectable(Describe(*r).c_str(), sel_id == r->id, ImGuiSelectableFlags_AllowDoubleClick)) {
          sel_id = r->id;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenRuleEditor(s, r, true);
        }
        ImGui::TableNextColumn();
        // the rule's look: its fill and font, or the color scale, the bar, the icons
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 q(p.x + S(100), p.y + ImGui::GetTextLineHeight());
        DrawRuleLook(ImGui::GetWindowDrawList(), *r, p, q);
        ImGui::Dummy(ImVec2(S(100), ImGui::GetTextLineHeight()));
        ImGui::TableNextColumn();
        std::string ap;
        for (const Rect& x : r->ranges) ap += (ap.empty() ? "=" : ",") + core::CellA1(x.r0, x.c0, true, true) + (x.IsCell() ? std::string() : ":" + core::CellA1(x.r1, x.c1, true, true));
        ImGui::TextUnformatted(ap.c_str());
        ImGui::TableNextColumn();
        // Excel: Stop If True doesn't apply to color scales, data bars and icon sets
        const bool visual = r->type == model::CfType::ColorScale || r->type == model::CfType::DataBar || r->type == model::CfType::IconSet;
        ImGui::BeginDisabled(visual);
        bool stop = r->stop_if_true && !visual;
        if (ImGui::Checkbox("##stop", &stop)) {
          r->stop_if_true = stop;
          u.cfm_dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    // Applies to: the selected rule's cells, picked on the sheet with the box's button
    if (sel) {
      static std::string applies, applies_for;
      std::string ap;
      for (const Rect& x : sel->ranges) ap += (ap.empty() ? "=" : ",") + core::CellA1(x.r0, x.c0, true, true) + (x.IsCell() ? std::string() : ":" + core::CellA1(x.r1, x.c1, true, true));
      const std::string owner = std::to_string(sel->id) + "|" + ap;
      if (applies_for != owner && !RangePicking()) {
        applies_for = owner;
        applies = ap;
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Applies to:"));
      ImGui::SameLine();
      RangeBox(s, "cfm.applies", &applies, S(400), RangeBox_Equals, TR("Applies to:"));
      if (applies != ap && !RangePicking() && !ImGui::IsAnyItemActive()) {
        uint32_t sid = 0;
        std::vector<Rect> areas;
        if (ResolveAreasText(s.snap->wb, sheet, applies, &sid, &areas) && sid == sheet) {
          sel->ranges = areas;
          u.cfm_dirty = true;
        } else {
          applies = ap;
          s.SetStatus(TR("Applies to must be cells of this sheet"));
        }
      }
    }
    ImGui::Separator();
    auto apply = [&] {
      if (!u.cfm_dirty) return;
      std::vector<model::CfRule> out = u.cfm_rules;
      for (auto& r : out) {
        if (r.id >= 0x80000000u) r.id = 0;  // made here: the document gives it an id
      }
      s.engine->PostUser(TR("Conditional Formatting Rules Manager"), [sheet, out](Document& d) { d.SetCondFormats(sheet, out); });
      u.cfm_dirty = false;
    };
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(440));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      apply();
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!u.cfm_dirty);
    if (ImGui::Button(TR("Apply"), ImVec2(S(80), 0))) apply();
    ImGui::EndDisabled();
    EndDlg(s, close);
    if (close) u.cfm_loaded = false;
  }
  // data validation
  if (BeginDlg(s, Dialog::DataValidation, TR("Data Validation###yc_dlg_dv"), 460)) {
    model::DvRule& d = u.dv;
    const float tabs_top = ImGui::GetCursorPosY();
    if (BeginDlgTabBar("##dvtabs")) {
      if (ImGui::BeginTabItem(TR("Settings"))) {
        static const char* kTypes[] = {TR("Any value"), TR("Whole number"), TR("Decimal"), TR("List"), TR("Date"), TR("Time"), TR("Text length"), TR("Custom")};
        int t = static_cast<int>(d.type);
        ImGui::TextUnformatted(TR("Allow"));
        ImGui::SetNextItemWidth(S(200));
        if (ui::Combo("##type", &t, kTypes, 8)) d.type = static_cast<model::DvType>(t);
        ImGui::SameLine();
        ImGui::BeginDisabled(d.type == model::DvType::Any);
        ImGui::Checkbox(TR("Ignore blank"), &d.ignore_blank);
        ImGui::EndDisabled();
        const bool cmp = d.type != model::DvType::Any && d.type != model::DvType::List && d.type != model::DvType::Custom;
        if (cmp) {
          static const char* kOps[] = {TR("between"), TR("not between"), TR("equal to"), TR("not equal to"), TR("greater than"), TR("less than"), TR("greater than or equal to"), TR("less than or equal to")};
          int op = static_cast<int>(d.op) - 1;
          ImGui::TextUnformatted(TR("Data"));
          ImGui::SetNextItemWidth(S(200));
          if (ui::Combo("##op", &op, kOps, 8)) d.op = static_cast<model::CmpOp>(op + 1);
          const bool two = d.op == model::CmpOp::Between || d.op == model::CmpOp::NotBetween;
          // Excel's labels follow the type and the operator: Minimum / Maximum, Start date / End date, Length...
          const bool date = d.type == model::DvType::Date, time = d.type == model::DvType::Time, len = d.type == model::DvType::TextLength;
          const char* lo = date ? TR("Start date:") : time ? TR("Start time:") : TR("Minimum:");
          const char* hi = date ? TR("End date:") : time ? TR("End time:") : TR("Maximum:");
          const char* one = date ? TR("Date:") : time ? TR("Time:") : len ? TR("Length:") : TR("Value:");
          const char* first = two ? lo : (d.op == model::CmpOp::Greater || d.op == model::CmpOp::GreaterEqual) ? lo : (d.op == model::CmpOp::Less || d.op == model::CmpOp::LessEqual) ? hi : one;
          ImGui::TextUnformatted(first);
          RangeBox(s, "dv.f1", u.dv_f1, sizeof(u.dv_f1), 0, RangeBox_Formula | RangeBox_Equals, first);
          if (two) {
            ImGui::TextUnformatted(hi);
            RangeBox(s, "dv.f2", u.dv_f2, sizeof(u.dv_f2), 0, RangeBox_Formula | RangeBox_Equals, hi);
          }
        } else if (d.type == model::DvType::List) {
          ImGui::Checkbox(TR("In-cell dropdown"), &d.in_cell_dropdown);
          ImGui::TextUnformatted(TR("Source:"));
          RangeBox(s, "dv.source", u.dv_f1, sizeof(u.dv_f1), 0, RangeBox_Formula | RangeBox_Equals, TR("Source:"));
          ImGui::TextDisabled("%s", TR("Select the cells with the list, or type the items separated by commas."));
        } else if (d.type == model::DvType::Custom) {
          ImGui::TextUnformatted(TR("Formula:"));
          RangeBox(s, "dv.formula", u.dv_f1, sizeof(u.dv_f1), 0, RangeBox_Formula | RangeBox_Equals, TR("Formula:"));
          ImGui::TextDisabled("%s", TR("A value is allowed when the formula is TRUE."));
        }
        ImGui::Separator();
        ImGui::BeginDisabled(u.dv_same_cells.empty());
        if (ImGui::Checkbox(TR("Apply these changes to all other cells with the same settings"), &u.dv_same) && u.dv_same) {
          // Excel selects those cells too
          auto& view = s.FocusView();
          for (const Rect& a : u.dv_same_cells) {
            if (std::find(view.sel.areas.begin(), view.sel.areas.end(), a) == view.sel.areas.end()) view.sel.areas.insert(view.sel.areas.begin(), a);
          }
        }
        ImGui::EndDisabled();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Input Message"))) {
        ImGui::Checkbox(TR("Show input message when cell is selected"), &d.show_input);
        ImGui::TextUnformatted(TR("Title:"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##it", u.dv_it, sizeof(u.dv_it));
        ImGui::TextUnformatted(TR("Input message:"));
        ImGui::InputTextMultiline("##im", u.dv_im, sizeof(u.dv_im), ImVec2(-1, S(80)));
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Error Alert"))) {
        ImGui::Checkbox(TR("Show error alert after invalid data is entered"), &d.show_error);
        static const char* kAlerts[] = {TR("Stop"), TR("Warning"), TR("Information")};
        int a = static_cast<int>(d.alert) - 1;
        ImGui::TextUnformatted(TR("Style"));
        ImGui::SetNextItemWidth(S(160));
        if (ui::Combo("##al", &a, kAlerts, 3)) d.alert = static_cast<model::DvAlert>(a + 1);
        ImGui::TextUnformatted(TR("Title:"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##et", u.dv_et, sizeof(u.dv_et));
        ImGui::TextUnformatted(TR("Error message:"));
        ImGui::InputTextMultiline("##em", u.dv_em, sizeof(u.dv_em), ImVec2(-1, S(80)));
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    // one size whatever the tab and Allow show, as Excel's dialog (Any value has one row, Decimal four: it jumped and
    // its buttons moved): the tabs take the height of the tallest one, measured from the tabs, not from the window
    // (a height taken from the window would grow it frame after frame)
    {
      static float tallest = 0.0f;
      const float used = ImGui::GetCursorPosY() - tabs_top;
      tallest = std::max({tallest, used, S(250)});
      if (used < tallest) ImGui::Dummy(ImVec2(1.0f, tallest - used));
    }
    ImGui::Separator();
    bool close = false;
    const uint32_t sheet = s.focus_sheet;
    if (ImGui::Button(TR("Clear All"))) {
      // Excel: the dialog goes back to "Any value" and empty boxes; nothing changes until OK
      u.dv = model::DvRule{};
      u.dv_f1[0] = u.dv_f2[0] = u.dv_it[0] = u.dv_im[0] = u.dv_et[0] = u.dv_em[0] = 0;
    }
    ImGui::SameLine(S(270));
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      model::DvRule rule = d;
      rule.formula1 = u.dv_f1;
      rule.formula2 = u.dv_f2;
      rule.input_title = u.dv_it;
      rule.input_message = u.dv_im;
      rule.error_title = u.dv_et;
      rule.error_message = u.dv_em;
      auto areas = SelectionAreasClipped(s);
      if (u.dv_same) areas.insert(areas.end(), u.dv_same_cells.begin(), u.dv_same_cells.end());
      s.engine->PostUser(TR("Data validation"), [sheet, areas, rule](Document& doc) {
        for (const Rect& a : areas) {
          if (rule.type == model::DvType::Any && rule.input_message.empty()) doc.ClearValidation(sheet, a);
          else doc.SetValidation(sheet, a, rule);
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
