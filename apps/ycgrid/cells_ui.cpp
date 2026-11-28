// SPDX-License-Identifier: Apache-2.0
// cells_ui.cpp - cells and sheets by mouse and keyboard, as in Excel: grouped sheets ([Group]), the menus of a
// right-drag of the selection's border and of the fill handle, the fill handle's tip, Shift-drops that insert, the
// Activate list of sheets, the status bar's view buttons, Pick From Drop-down List, Filter by Selected Cell's Value,
// Select Objects, Fill > Justify and Across Worksheets, Paste Options > Picture / Linked Picture, and the keys
// Ctrl+6, Ctrl+8, Ctrl+\, Ctrl+Shift+PgUp / PgDn, F6 and Scroll Lock
#include "app_state.h"
#include "core/i18n.h"
#include "print.h"
#include "screenshot.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "model/table.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

constexpr double kPx = 4.0 / 3.0;  // points -> sheet pixels (100%)

struct CellsUi {
  // the right-drag of the selection's border: where the cells were dropped
  bool move_open = false;
  uint32_t move_sheet = 0, move_r = 0, move_c = 0;
  Rect move_src{};
  // the right-drag of the fill handle
  bool fill_open = false;
  uint32_t fill_sheet = 0;
  Rect fill_src{}, fill_dest{};
  // Pick From Drop-down List
  bool pick_open = false;
  uint32_t pick_sheet = 0, pick_r = 0, pick_c = 0;
  std::vector<std::string> pick_items;
  int pick_sel = 0;
  // the Activate list
  int act_sel = 0;
  // Fill Across Worksheets: 0 All, 1 Contents, 2 Formats
  int across = 0;
  // Find & Select > Select Objects: the pointer selects objects; a drag over the sheet draws a box around them
  bool select_objects = false;
  bool lasso = false;
  ImVec2 lasso0{};
  // Scroll Lock (the keyboard's light; a hidden instance counts its own key presses)
  bool scroll_lock = false;
  bool scroll_known = false;
  // linked pictures: what the source cells looked like when last drawn (sheet << 32 | shape -> hash of the cells)
  std::map<uint64_t, size_t> linked_seen;
  double linked_checked = 0.0;
};

CellsUi& C() {
  static CellsUi c;
  return c;
}

ui::Command Make(const char* id, const char* label, const char* shortcut = "", const char* desc = "", const char* icon = nullptr) {
  ui::Command c;
  c.id = id;
  c.label = label;
  c.shortcut = shortcut;
  c.tip_desc = desc;
  c.icon = icon;
  return c;
}

// the sheets a person can see on the tab bar, in tab order
std::vector<uint32_t> TabSheets(AppState& s) {
  std::vector<uint32_t> out;
  if (!s.snap) return out;
  for (const auto& sh : s.snap->wb.sheets) {
    if (sh->visible && !s.Win(sh->id).floating) out.push_back(sh->id);
  }
  return out;
}

bool InGroup(const AppState& s, uint32_t id) { return std::find(s.group_sheets.begin(), s.group_sheets.end(), id) != s.group_sheets.end(); }

void Activate(AppState& s, uint32_t id) {
  if (!s.snap || !s.snap->Sheet(id)) return;
  s.active_sheet = id;
  s.focus_sheet = id;
  s.focus_win = 0;
}

std::string CellText(const AppState& s, const model::Sheet& sh, uint32_t r, uint32_t c) {
  const core::Value& v = sh.GetValue(r, c);
  if (v.IsEmpty()) return {};
  const auto& tbl = *s.snap->wb.styles;
  return engine::DisplayTextOf(v, tbl.NumberFormat(tbl.Get(sh.GetStyle(r, c))));
}

// Ctrl+\ / Ctrl+Shift+| : Go To Special's Row / Column differences in the selection. One cell selected finds none
// (Excel 16 says "No cells were found." even where the cell's region differs, measured by keys)
void SelectDifferences(AppState& s, bool rows) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  auto& view = s.FocusView();
  Rect w = SelRect(s);
  if (sh->has_used) {
    w.r1 = std::min(w.r1, std::max(sh->used.r1, w.r0));
    w.c1 = std::min(w.c1, std::max(sh->used.c1, w.c0));
  }
  std::vector<Rect> areas = w.IsCell() ? std::vector<Rect>{} : engine::DifferenceCells(*sh, w, view.sel.act_r, view.sel.act_c, rows);
  if (areas.empty()) {
    s.ShowMessage("YCGrid", TR("No cells were found."));
    return;
  }
  if (areas.size() > 10000) areas.resize(10000);
  // the active cell is the first cell found, top to bottom (Excel 16, measured)
  render::SetAreasActiveFirst(view.sel, areas);
  render::EnsureVisible(*sh, view, view.sel.act_r, view.sel.act_c);
}

// F6 / Shift+F6: the next pane of a split window gets the active cell (Excel); without a split, the ribbon's KeyTips
void NextPane(AppState& s, bool back) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  auto& v = s.FocusView();
  if (!v.split || (!v.freeze_rows && !v.freeze_cols)) {
    ui::ribbon::St().keytip_level = 1;
    return;
  }
  const uint32_t fr = v.frozen_top + v.freeze_rows, fc = v.frozen_left + v.freeze_cols;
  // the panes in Excel's order (clockwise from the top left), each by the first cell it shows
  struct P {
    uint32_t r, c;
  };
  std::vector<P> panes;
  const uint32_t main_r = static_cast<uint32_t>(std::max<int64_t>(v.v.first, fr)), main_c = static_cast<uint32_t>(std::max<int64_t>(v.h.first, fc));
  if (v.freeze_rows && v.freeze_cols) panes = {{v.frozen_top, v.frozen_left}, {v.frozen_top, main_c}, {main_r, main_c}, {main_r, v.frozen_left}};
  else if (v.freeze_rows) panes = {{v.frozen_top, main_c}, {main_r, main_c}};
  else panes = {{main_r, v.frozen_left}, {main_r, main_c}};
  const bool top = v.freeze_rows && v.sel.act_r < fr, left = v.freeze_cols && v.sel.act_c < fc;
  size_t at = 0;
  if (v.freeze_rows && v.freeze_cols) at = top ? (left ? 0 : 1) : (left ? 3 : 2);
  else if (v.freeze_rows) at = top ? 0 : 1;
  else at = left ? 0 : 1;
  const size_t next = back ? (at + panes.size() - 1) % panes.size() : (at + 1) % panes.size();
  render::SelectCell(v, panes[next].r, panes[next].c, false);
}

// Fill > Justify: the text of the first column's cells is joined and broken again into lines as wide as the selected
// columns, one line per row (Excel's Range.Justify)
void Justify(AppState& s, uint32_t sheet, Rect a) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || !s.engine) return;
  if (sh->has_used) a.r1 = std::min(a.r1, std::max(a.r0, sh->used.r1));
  std::vector<std::string> words;
  for (uint32_t r = a.r0; r <= a.r1 && r - a.r0 < 10000; ++r) {
    const core::Value& v = sh->GetValue(r, a.c0);
    if (sh->GetFormula(r, a.c0) || v.IsNumeric()) {
      s.ShowError("YCGrid", TR("Cannot justify: the cells hold numbers or formulas."));
      return;
    }
    const std::string t = CellText(s, *sh, r, a.c0);
    size_t i = 0;
    while (i < t.size()) {
      while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t')) ++i;
      size_t j = i;
      while (j < t.size() && t[j] != ' ' && t[j] != '\n' && t[j] != '\t') ++j;
      if (j > i) words.push_back(t.substr(i, j - i));
      i = j;
    }
  }
  if (words.empty()) return;
  // the width of the selected columns in characters of the standard font (7 pixels each, as a column's width)
  int64_t px = 0;
  for (uint32_t c = a.c0; c <= a.c1 && c - a.c0 < 1000; ++c) px += sh->cols->Size(c);
  const size_t width = static_cast<size_t>(std::max<int64_t>(1, (px - 5) / 7));
  std::vector<std::string> lines;
  for (const std::string& w : words) {
    if (!lines.empty() && lines.back().size() + 1 + w.size() <= width) lines.back() += " " + w;
    else lines.push_back(w);
  }
  const uint32_t rows = static_cast<uint32_t>(a.Rows());
  const uint32_t r0 = a.r0, c0 = a.c0;
  auto run = [&s, sheet, r0, c0, rows, lines]() {
    s.engine->PostUser(TR("Justify"), [=](Document& d) {
      for (uint32_t i = 0; i < std::max<uint32_t>(rows, static_cast<uint32_t>(lines.size())); ++i) {
        if (i < lines.size()) d.SetInput(sheet, r0 + i, c0, lines[i]);
        else d.ClearContents(sheet, Rect::Cell(r0 + i, c0));
      }
    });
  };
  if (rows > 1 && lines.size() > rows) AskYesNo(s, "YCGrid", TR("Text will extend below selected range."), run);
  else run();
}

// Fill Across Worksheets: the selection of the active sheet goes to the same cells of every grouped sheet
void FillAcross(AppState& s, const std::vector<uint32_t>& sheets, uint32_t from, const std::vector<Rect>& areas, int kind) {
  if (!s.engine || sheets.size() < 2) return;
  s.engine->PostUser(TR("Fill Across Worksheets"), [=](Document& d) {
    for (const Rect& a : areas) {
      const engine::ClipData clip = d.Copy(from, a);
      for (const uint32_t g : sheets) {
        if (g == from) continue;
        d.Paste(g, a.r0, a.c0, clip, kind == 0 ? engine::PasteMode::All : kind == 1 ? engine::PasteMode::Formulas : engine::PasteMode::Formats);
      }
    }
  });
}

// the cell menu's Filter: the active cell's value, fill color or font color becomes its column's filter; a list
// without a filter gets one first (Excel)
void FilterBySelected(AppState& s, int kind) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.engine) return;
  const uint32_t sheet = sh->id;
  const auto& sel = s.FocusView().sel;
  const uint32_t r = sel.act_r, c = sel.act_c;
  model::FilterColumn fc;
  fc.kind = kind;
  if (kind == 1) {
    const std::string t = CellText(s, *sh, r, c);
    fc.blanks = t.empty();
    if (!t.empty()) fc.values = {t};
  } else if (kind == 7) {
    // the icon conditional formatting shows in the cell (Filter by Cell Icon)
    engine::CondFormatEval ev(s.snap->wb, sheet);
    engine::CfVisual v;
    if (!ev.Eval(r, c, &v) || v.icon < 0) return;
    fc.icon_set = v.icon_style;
    fc.icon = v.icon;
  } else {
    const model::Style& st = s.snap->wb.styles->Get(sh->GetStyle(r, c));
    const uint32_t col = kind == 4 ? st.fill_color : st.font_color;
    fc.color = (col >> 24) ? col : 0u;
  }
  Rect range{};
  uint32_t table = 0;
  bool make = false;
  if (const model::Table* t = model::TableAt(s.snap->wb, sheet, r, c); t && t->header) {
    table = t->id;
  } else if (sh->autofilter->on && sh->autofilter->range.c0 <= c && c <= sh->autofilter->range.c1) {
    range = sh->autofilter->range;
  } else {
    range = engine::CurrentRegion(*sh, r, c);
    if (range.IsCell() && sh->IsEmptyCell(r, c)) return;
    make = true;
  }
  s.engine->PostUser(TR("Filter"), [=](Document& d) {
    model::FilterColumn f = fc;
    if (table) {
      const model::Sheet* ds = d.S(sheet);
      const model::Table* t = nullptr;
      for (const auto& x : *ds->tables) t = x.id == table ? &x : t;
      if (!t) return;
      if (!engine::TableHasOwnFilter(*ds, *t) && !engine::TableOwnsSheetFilter(*ds, *t)) d.SetTableFilterButton(sheet, table, true);
      ds = d.S(sheet);
      for (const auto& x : *ds->tables) t = x.id == table ? &x : t;
      f.field = c - t->range.c0;
      if (engine::TableHasOwnFilter(*ds, *t)) d.SetTableFilterColumn(sheet, table, f);
      else d.SetFilterColumn(sheet, f);
      return;
    }
    if (make) d.SetAutoFilter(sheet, range);
    f.field = c - range.c0;
    d.SetFilterColumn(sheet, f);
  });
}

// the words of the column around the active cell (up to the first empty cell each way), A to Z: Pick From Drop-down List
std::vector<std::string> ColumnWords(AppState& s, const model::Sheet& sh, uint32_t r, uint32_t c) {
  std::set<std::string> seen;
  std::vector<std::string> out;
  auto take = [&](uint32_t rr) {
    const core::Value& v = sh.GetValue(rr, c);
    if (v.IsEmpty()) return false;
    if (v.IsString() && !sh.GetFormula(rr, c)) {
      const std::string t(v.str());
      if (seen.insert(t).second) out.push_back(t);
    }
    return true;
  };
  for (uint32_t rr = r; rr > 0 && out.size() < 4000 && take(rr - 1); --rr) {
  }
  for (uint32_t rr = r + 1; rr < sh.row_limit && out.size() < 8000 && take(rr); ++rr) {
  }
  std::sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) { return engine::Lower(x) < engine::Lower(y); });
  (void)s;
  return out;
}

void OpenPickList(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  const auto& sel = s.FocusView().sel;
  CellsUi& u = C();
  u.pick_sheet = sh->id;
  u.pick_r = sel.act_r;
  u.pick_c = sel.act_c;
  u.pick_items = ColumnWords(s, *sh, sel.act_r, sel.act_c);
  u.pick_sel = -1;  // nothing chosen yet: the first Down takes the first entry (Excel 16, measured by keys)
  u.pick_open = true;
}

size_t CellsHash(const model::Sheet& sh, const Rect& r) {
  size_t h = 1469598103934665603ull;
  uint64_t n = 0;
  for (uint32_t rr = r.r0; rr <= r.r1 && n < 20000; ++rr) {
    for (uint32_t cc = r.c0; cc <= r.c1 && n < 20000; ++cc, ++n) {
      const core::Value& v = sh.GetValue(rr, cc);
      h = (h ^ std::hash<std::string>{}(v.IsString() ? std::string(v.str()) : std::to_string(v.IsNumeric() ? v.AsDouble() : 0.0))) * 1099511628211ull;
      h = (h ^ sh.GetStyle(rr, cc)) * 1099511628211ull;
    }
  }
  for (uint32_t cc = r.c0; cc <= r.c1 && cc - r.c0 < 500; ++cc) h = (h ^ static_cast<size_t>(sh.cols->Size(cc))) * 1099511628211ull;
  for (uint32_t rr = r.r0; rr <= r.r1 && rr - r.r0 < 2000; ++rr) h = (h ^ static_cast<size_t>(sh.rows->Size(rr))) * 1099511628211ull;
  return h;
}

// the cells drawn as a picture (PNG bytes), as Copy as Picture draws them
bool CellsPicture(AppState& s, uint32_t sheet, const Rect& r, std::string* png, int* w, int* h) {
  if (!s.snap) return false;
  ImagePart part;
  part.sheet = sheet;
  part.area = r;
  ImageOptions o;
  o.gridlines = false;
  Image img;
  if (!RenderImage(*s.snap, {part}, o, &img.rgba, &img.w, &img.h)) return false;
  *w = img.w;
  *h = img.h;
  return EncodePng(img, png);
}

// linked pictures follow their cells: drawn again when the cells change (at most a few times a second)
void UpdateLinkedPictures(AppState& s) {
  CellsUi& u = C();
  if (!s.snap || !s.engine || ImGui::GetTime() - u.linked_checked < 0.3) return;
  u.linked_checked = ImGui::GetTime();
  for (const auto& sh : s.snap->wb.sheets) {
    for (const auto& lp : *sh->linked_pictures) {
      const model::Sheet* src = s.snap->Sheet(lp.sheet);
      bool has = false;
      for (const auto& x : *sh->shapes) has = has || x.id == lp.shape;
      if (!src || !has) continue;
      const uint64_t key = (uint64_t{sh->id} << 32) | lp.shape;
      const size_t h = CellsHash(*src, lp.rect);
      auto it = u.linked_seen.find(key);
      if (it == u.linked_seen.end()) {
        u.linked_seen[key] = h;  // as drawn when it was pasted (or loaded)
        continue;
      }
      if (it->second == h) continue;
      it->second = h;
      std::string png;
      int w = 0, ht = 0;
      if (!CellsPicture(s, lp.sheet, lp.rect, &png, &w, &ht)) continue;
      const uint32_t sid = sh->id, shape = lp.shape;
      // derived from the cells, as a recalculation: read-only mode lets it through (app_refresh)
      s.engine->Post([sid, shape, png](Document& d) {
        d.app_refresh = true;
        d.UpdateShape(sid, shape, [&](model::ShapeObj& x) { x.image = png; });
        d.app_refresh = false;
      });
    }
  }
}

void DrawActivateList(AppState& s) {
  if (s.dialog != Dialog::ActivateSheetList) return;
  const char* title = TR("Activate###yc_activate");
  CellsUi& u = C();
  const std::vector<uint32_t> ids = TabSheets(s);
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
    u.act_sel = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
      if (ids[i] == s.active_sheet) u.act_sel = static_cast<int>(i);
    }
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return;
  }
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape), ok = false;
  ImGui::TextUnformatted(TR("Activate:"));
  DlgListKeys(&u.act_sel, static_cast<int>(ids.size()), [&](int i) { return s.snap->Sheet(ids[static_cast<size_t>(i)])->name.c_str(); });
  if (ImGui::BeginListBox("##sheets", ImVec2(S(260), S(220)))) {
    for (size_t i = 0; i < ids.size(); ++i) {
      const model::Sheet* sh = s.snap->Sheet(ids[i]);
      if (!sh) continue;
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(sh->name.c_str(), u.act_sel == static_cast<int>(i), ImGuiSelectableFlags_AllowDoubleClick)) {
        u.act_sel = static_cast<int>(i);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ok = true;
      }
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }
  if (DlgOkButton(TR("OK"), ImVec2(S(90), 0))) ok = true;
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  if (ok && u.act_sel >= 0 && u.act_sel < static_cast<int>(ids.size())) {
    ActivateSheet(s, ids[static_cast<size_t>(u.act_sel)]);
    close = true;
  }
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

void DrawFillAcrossDialog(AppState& s) {
  if (s.dialog != Dialog::FillAcrossSheets) return;
  const char* title = TR("Fill Across Worksheets###yc_fill_across");
  CellsUi& u = C();
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
    u.across = 0;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return;
  }
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SeparatorText(TR("Fill"));
  ui::RadioButton(TR("All"), &u.across, 0);
  ui::RadioButton(TR("Contents"), &u.across, 1);
  ui::RadioButton(TR("Formats"), &u.across, 2);
  if (DlgOkButton(TR("OK"), ImVec2(S(90), 0))) {
    FillAcross(s, GroupTargets(s, s.focus_sheet), s.focus_sheet, SelAreas(s), u.across);
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

void SelectAfter(AppState& s, uint32_t sheet, const Rect& r) {
  if (s.focus_sheet != sheet) return;
  render::SelectRange(s.FocusView(), r);
}

void DrawMoveMenu(AppState& s) {
  CellsUi& u = C();
  if (u.move_open) {
    ImGui::OpenPopup("##yc_move_drop");
    u.move_open = false;
  }
  if (!ImGui::BeginPopup("##yc_move_drop")) return;
  const uint32_t sheet = u.move_sheet, r = u.move_r, c = u.move_c;
  const Rect src = u.move_src;
  const Rect dest{r, c, r + (src.r1 - src.r0), c + (src.c1 - src.c0)};
  auto copy_as = [&](engine::PasteMode mode, const char* label) {
    s.engine->PostUser(label, [=](Document& d) { d.Paste(sheet, r, c, d.Copy(sheet, src), mode); });
    SelectAfter(s, sheet, dest);
  };
  if (ImGui::MenuItem(TR("Move Here"))) {
    if (!RefusePartOfArray(s, sheet, {src})) {
      s.engine->PostUser(TR("Move"), [=](Document& d) { d.MoveCells(sheet, src, sheet, r, c); });
      SelectAfter(s, sheet, dest);
    }
  }
  if (ImGui::MenuItem(TR("Copy Here"))) copy_as(engine::PasteMode::All, TR("Copy"));
  if (ImGui::MenuItem(TR("Copy Here as Values Only"))) copy_as(engine::PasteMode::Values, TR("Copy"));
  if (ImGui::MenuItem(TR("Copy Here as Formats Only"))) copy_as(engine::PasteMode::Formats, TR("Copy"));
  if (ImGui::MenuItem(TR("Link Here"))) {
    // formulas pointing at the dragged cells (one cell absolutely, a block relatively, as Paste Link)
    const bool one = src.IsCell();
    s.engine->PostUser(TR("Link Here"), [=](Document& d) {
      for (uint32_t i = 0; i <= src.r1 - src.r0; ++i) {
        for (uint32_t j = 0; j <= src.c1 - src.c0; ++j) d.SetFormula(sheet, r + i, c + j, core::CellA1(src.r0 + i, src.c0 + j, one, one));
      }
    });
    SelectAfter(s, sheet, dest);
  }
  if (ImGui::MenuItem(TR("Create Hyperlink Here"))) {
    // a link to each dragged cell, showing its text (Excel)
    const model::Sheet* sh = s.snap->Sheet(sheet);
    std::vector<std::string> texts;
    for (uint32_t i = 0; sh && i <= src.r1 - src.r0 && i < 1000; ++i) {
      for (uint32_t j = 0; j <= src.c1 - src.c0 && j < 200; ++j) texts.push_back(CellText(s, *sh, src.r0 + i, src.c0 + j));
    }
    const std::string name = sh ? core::QuoteSheetName(sh->name) : std::string();
    const uint32_t cols = src.c1 - src.c0 + 1;
    s.engine->PostUser(TR("Create Hyperlink Here"), [=](Document& d) {
      for (size_t k = 0; k < texts.size(); ++k) {
        const uint32_t i = static_cast<uint32_t>(k / cols), j = static_cast<uint32_t>(k % cols);
        model::Hyperlink link;
        link.sub_address = name + "!" + core::CellA1(src.r0 + i, src.c0 + j);
        d.SetHyperlink(sheet, r + i, c + j, link);
        d.SetInput(sheet, r + i, c + j, texts[k].empty() ? link.sub_address : texts[k]);
      }
    });
    SelectAfter(s, sheet, dest);
  }
  ImGui::Separator();
  if (ImGui::MenuItem(TR("Shift Down and Copy"))) InsertDrop(s, sheet, src, 1, r, c, true);
  if (ImGui::MenuItem(TR("Shift Right and Copy"))) InsertDrop(s, sheet, src, 2, r, c, true);
  if (ImGui::MenuItem(TR("Shift Down and Move"))) InsertDrop(s, sheet, src, 1, r, c, false);
  if (ImGui::MenuItem(TR("Shift Right and Move"))) InsertDrop(s, sheet, src, 2, r, c, false);
  ImGui::Separator();
  ImGui::MenuItem(TR("Cancel"));
  ImGui::EndPopup();
}

void DrawFillMenu(AppState& s) {
  CellsUi& u = C();
  if (u.fill_open) {
    ImGui::OpenPopup("##yc_fill_drop");
    u.fill_open = false;
  }
  if (!ImGui::BeginPopup("##yc_fill_drop")) return;
  const uint32_t sheet = u.fill_sheet;
  const Rect src = u.fill_src, dest = u.fill_dest;
  bool date = false;
  FillMakesSeries(s, sheet, src, &date);
  bool numbers = false;
  if (const model::Sheet* sh = s.snap->Sheet(sheet)) numbers = sh->GetValue(src.r0, src.c0).IsNumber();
  auto fill = [&](int type) {
    NoteAutoFill(s, sheet, src, dest);
    s.last_fill.type = type;
    s.engine->PostUser(TR("AutoFill"), [=](Document& d) { d.AutoFill(sheet, src, dest, type); });
    SelectAfter(s, sheet, dest);
  };
  if (ImGui::MenuItem(TR("Copy Cells"))) fill(1);
  if (ImGui::MenuItem(TR("Fill Series"))) fill(2);
  if (ImGui::MenuItem(TR("Fill Formatting Only"))) fill(3);
  if (ImGui::MenuItem(TR("Fill Without Formatting"))) fill(4);
  ImGui::Separator();
  if (ImGui::MenuItem(TR("Fill Days"), nullptr, false, date)) fill(5);
  if (ImGui::MenuItem(TR("Fill Weekdays"), nullptr, false, date)) fill(6);
  if (ImGui::MenuItem(TR("Fill Months"), nullptr, false, date)) fill(7);
  if (ImGui::MenuItem(TR("Fill Years"), nullptr, false, date)) fill(8);
  ImGui::Separator();
  if (ImGui::MenuItem(TR("Linear Trend"), nullptr, false, numbers && !src.IsCell())) fill(9);
  if (ImGui::MenuItem(TR("Growth Trend"), nullptr, false, numbers && !src.IsCell())) fill(10);
  if (ImGui::MenuItem(TR("Flash Fill"))) {
    SelectAfter(s, sheet, dest);
    ui::Commands().Execute("FlashFill");
  }
  if (ImGui::MenuItem(TR("Series..."))) {
    SelectAfter(s, sheet, dest);
    ui::Commands().Execute("FillSeries");
  }
  ImGui::EndPopup();
}

void DrawPickList(AppState& s) {
  CellsUi& u = C();
  const char* id = "##yc_pick_list";
  if (u.pick_open) {
    u.pick_open = false;
    const model::Sheet* sh = s.snap ? s.snap->Sheet(u.pick_sheet) : nullptr;
    if (!sh || u.pick_items.empty()) return;
    const ImRect cell = CellsScreenRect(*sh, s.FocusView(), Rect::Cell(u.pick_r, u.pick_c));
    ImGui::SetNextWindowPos(ImVec2(cell.Min.x, cell.Max.y + 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(cell.GetWidth(), S(120)), 0), ImVec2(S(400), S(260)));
    ImGui::OpenPopup(id);
  }
  if (!ImGui::BeginPopup(id)) return;
  // the arrows and Enter choose, as in Excel's list: Alt+Down, Down, Enter takes the first entry (the Down of the
  // Alt+Down that opened it was counted too, and the list began on the first entry: one Down took the third)
  const int n = static_cast<int>(u.pick_items.size());
  const bool alt = ImGui::GetIO().KeyAlt;
  if (!alt && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) u.pick_sel = std::min(n - 1, u.pick_sel + 1);
  if (!alt && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) u.pick_sel = std::max(0, u.pick_sel - 1);
  int chosen = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
    if (u.pick_sel >= 0) chosen = u.pick_sel;
    else ImGui::CloseCurrentPopup();  // nothing chosen: the list goes, the cell stays as it was
  }
  for (int i = 0; i < n; ++i) {
    ImGui::PushID(i);
    if (ImGui::Selectable(u.pick_items[static_cast<size_t>(i)].c_str(), i == u.pick_sel)) chosen = i;
    ImGui::PopID();
  }
  if (chosen >= 0) {
    const uint32_t sheet = u.pick_sheet, r = u.pick_r, c = u.pick_c;
    const std::string text = u.pick_items[static_cast<size_t>(chosen)];
    if (s.engine) s.engine->PostUser(TR("Pick From Drop-down List"), [=](Document& d) { d.SetInput(sheet, r, c, text); });
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace

// ---- grouped sheets ----

std::vector<uint32_t> GroupTargets(AppState& s, uint32_t sheet) {
  std::vector<uint32_t> out{sheet};
  if (s.group_sheets.size() < 2 || sheet != s.active_sheet || !InGroup(s, sheet) || !s.snap) return out;
  for (const uint32_t id : s.group_sheets) {
    const model::Sheet* sh = s.snap->Sheet(id);
    if (id != sheet && sh && !sh->chart_sheet) out.push_back(id);
  }
  return out;
}

void SetSheetGroup(AppState& s, std::vector<uint32_t> ids, uint32_t active) {
  std::vector<uint32_t> order;
  for (const uint32_t id : TabSheets(s)) {
    if (std::find(ids.begin(), ids.end(), id) != ids.end()) order.push_back(id);
  }
  const bool was = s.group_sheets.size() > 1;
  s.group_sheets = order.size() > 1 ? order : std::vector<uint32_t>{};
  Activate(s, active);
  if (was != (s.group_sheets.size() > 1)) UpdateWindowTitle(s);  // "[Group]" in the title bar, as Excel
}

void SheetTabClicked(AppState& s, uint32_t id, int how) {
  std::vector<uint32_t> g = s.group_sheets.empty() ? std::vector<uint32_t>{s.active_sheet} : s.group_sheets;
  if (how == 1) {
    // Ctrl+click: the sheet joins the group and is shown; a grouped one (not the active one) leaves it
    if (std::find(g.begin(), g.end(), id) != g.end()) {
      if (id != s.active_sheet) g.erase(std::find(g.begin(), g.end(), id));
      SetSheetGroup(s, g, s.active_sheet);
    } else {
      g.push_back(id);
      SetSheetGroup(s, g, id);
    }
    return;
  }
  if (how == 2) {
    // Shift+click: every sheet from the active one to this one
    const std::vector<uint32_t> tabs = TabSheets(s);
    const auto a = std::find(tabs.begin(), tabs.end(), s.active_sheet), b = std::find(tabs.begin(), tabs.end(), id);
    if (a == tabs.end() || b == tabs.end()) return;
    std::vector<uint32_t> span(std::min(a, b), std::max(a, b) + 1);
    SetSheetGroup(s, span, id);
    return;
  }
  // a click: inside a group that isn't every sheet it only shows the sheet; elsewhere (or all grouped) it ungroups (Excel)
  if (s.group_sheets.size() > 1 && InGroup(s, id) && s.group_sheets.size() < TabSheets(s).size()) {
    Activate(s, id);
    return;
  }
  SetSheetGroup(s, {}, id);
}

// ---- drop menus and the fill handle's tip ----

void OpenMoveDropMenu(AppState& s, uint32_t sheet, const Rect& src, uint32_t r, uint32_t c) {
  (void)s;
  CellsUi& u = C();
  u.move_open = true;
  u.move_sheet = sheet;
  u.move_src = src;
  u.move_r = r;
  u.move_c = c;
}

void OpenFillDropMenu(AppState& s, uint32_t sheet, const Rect& src, const Rect& dest) {
  (void)s;
  CellsUi& u = C();
  u.fill_open = true;
  u.fill_sheet = sheet;
  u.fill_src = src;
  u.fill_dest = dest;
}

std::string FillTipFor(AppState& s, uint32_t sheet, const Rect& src, const Rect& dest, bool ctrl) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return {};
  const int type = ctrl ? (FillMakesSeries(s, sheet, src) ? 1 : 2) : 0;  // Ctrl: the other kind of fill (Excel)
  return engine::Document::AutoFillTip(s.snap->wb, *sh, src, dest, type);
}

void PasteCellsPicture(AppState& s, uint32_t sheet, const Rect& at, uint32_t src_sheet, const Rect& src, bool linked) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || !s.engine) return;
  std::string png;
  int w = 0, h = 0;
  if (!CellsPicture(s, src_sheet, src, &png, &w, &h)) {
    s.SetStatus(TR("The picture couldn't be made"));
    return;
  }
  model::ShapeObj pic;
  pic.kind = model::ShapeKind::Picture;
  pic.left = static_cast<double>(sh->cols->Offset(at.c0)) / kPx;
  pic.top = static_cast<double>(sh->rows->Offset(at.r0)) / kPx;
  pic.width = w * 0.75;
  pic.height = h * 0.75;
  pic.image = std::move(png);
  const uint32_t id = s.engine->Call([&](Document& d) {
    d.Begin(linked ? TR("Linked Picture") : TR("Picture"));
    const uint32_t v = d.AddShape(sheet, pic);
    if (linked && v) d.SetLinkedPicture(sheet, v, src_sheet, src);
    d.Commit();
    return v;
  });
  if (linked) C().linked_seen[(uint64_t{sheet} << 32) | id] = CellsHash(*s.snap->Sheet(src_sheet), src);
  SelectInsertedShape(s, sheet, id);
}

// ---- status bar ----

bool ScrollLockOn() { return C().scroll_lock; }

float DrawStatusViewButtons(AppState& s, const ImRect& bar, float right) {
  const auto& t = ui::T();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto& v = s.FocusView();
  struct B {
    const char* cmd;
    const char* label;
    const char* icon;
    int view;
  };
  // Excel's order, left to right: Normal, Page Layout, Page Break Preview
  const B buttons[] = {{"ViewNormal", TR("Normal"), ui::icon::ViewNormal, 0},
                       {"ViewPageLayout", TR("Page Layout"), ui::icon::ViewPageLayout, 2},
                       {"ViewPageBreakPreview", TR("Page Break Preview"), ui::icon::ViewPageBreak, 1}};
  const float bw = S(22), h = bar.GetHeight() - S(4);
  float x = right - bw * 3.0f - S(4);
  const float left = x;
  for (const B& b : buttons) {
    const ImRect r(ImVec2(x, bar.Min.y + S(2)), ImVec2(x + bw, bar.Min.y + S(2) + h));
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::PushID(b.cmd);
    const bool clicked = ImGui::InvisibleButton("##view", r.GetSize());
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ui::NoteUiItem((std::string(b.label) + "##statusview").c_str(), r);
    const bool on = v.page_view == b.view;
    if (on) dl->AddRectFilled(r.Min, r.Max, IM_COL32(0xC8, 0xC8, 0xC8, 255), S(2));
    else if (hov) dl->AddRectFilled(r.Min, r.Max, IM_COL32(0xDE, 0xDE, 0xDE, 255), S(2));
    const float fs = h * 0.75f;
    const ImVec2 ts = ui::F().icons->CalcTextSizeA(fs, FLT_MAX, 0.0f, b.icon);
    dl->AddText(ui::F().icons, fs, ImVec2(r.GetCenter().x - ts.x * 0.5f, r.GetCenter().y - ts.y * 0.5f), t.statusbar_text, b.icon);
    if (hov) ImGui::SetTooltip("%s", b.label);
    if (clicked) ui::Commands().Execute(b.cmd);
    x += bw;
  }
  return left - S(8);
}

// ---- Select Objects ----

void DrawSelectObjectsOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  CellsUi& u = C();
  if (!u.select_objects || sheet_id != s.focus_sheet) return;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetCursorScreenPos(g.cells.Min);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##yc_select_objects", g.cells.GetSize());
  ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
  if (ImGui::IsItemActivated()) {
    u.lasso = true;
    u.lasso0 = io.MousePos;
  }
  if (u.lasso) {
    const ImVec2 a(std::min(u.lasso0.x, io.MousePos.x), std::min(u.lasso0.y, io.MousePos.y));
    const ImVec2 b(std::max(u.lasso0.x, io.MousePos.x), std::max(u.lasso0.y, io.MousePos.y));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(a, b, IM_COL32(0x40, 0x40, 0x40, 255), 0.0f, 1.0f);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      u.lasso = false;
      // every object wholly inside the box is selected (Excel)
      auto inside = [&](double l, double t, double w, double h) {
        const ImVec2 p0 = g.to_screen(l * kPx, t * kPx), p1 = g.to_screen((l + w) * kPx, (t + h) * kPx);
        return p0.x >= a.x && p0.y >= a.y && p1.x <= b.x && p1.y <= b.y;
      };
      bool first = true;
      auto pick = [&](int kind, uint32_t id) {
        SelectObject(s, sheet_id, kind, id, first);
        first = false;
      };
      for (const auto& c : *sh->controls) {
        if (inside(c.left, c.top, c.width, c.height)) pick(1, c.id);
      }
      for (const auto& ch : *sh->charts) {
        if (inside(ch.left, ch.top, ch.width, ch.height)) pick(2, ch.id);
      }
      for (const auto& x : *sh->shapes) {
        if (x.visible && inside(x.left, x.top, x.width, x.height)) pick(3, x.id);
      }
    }
  }
}

// ---- the cell menu's Filter ----

void DrawCellMenuFilter(AppState& s) {
  if (!ImGui::BeginMenu(TR("Filter"))) return;
  for (const char* id : {"ClearFilterColumn", "ReapplyFilter"}) {
    ui::Command* c = ui::Commands().Find(id);
    if (c && ImGui::MenuItem(c->label.c_str(), nullptr, false, c->enabled)) ui::Commands().Execute(id);
  }
  ImGui::Separator();
  // Filter by Selected Cell's Icon: only for a cell that shows a conditional formatting icon (Excel)
  bool icon = false;
  if (const model::Sheet* sh = s.FocusSheet(); sh && !sh->cond_formats->empty()) {
    engine::CondFormatEval ev(s.snap->wb, sh->id);
    engine::CfVisual v;
    const auto& sel = s.FocusView().sel;
    icon = ev.Eval(sel.act_r, sel.act_c, &v) && v.icon >= 0;
  }
  for (const char* id : {"FilterBySelectedValue", "FilterBySelectedColor", "FilterBySelectedFontColor", "FilterBySelectedIcon"}) {
    ui::Command* c = ui::Commands().Find(id);
    const bool on = std::strcmp(id, "FilterBySelectedIcon") != 0 || icon;
    if (c && ImGui::MenuItem(c->label.c_str(), nullptr, false, on)) ui::Commands().Execute(id);
  }
  ImGui::EndMenu();
}

// ---- every frame ----

void DrawCellsUi(AppState& s) {
  CellsUi& u = C();
  // Scroll Lock: the keyboard's own light while YCGrid has it; a hidden instance counts the key presses it gets
  if (!u.scroll_known) {
    u.scroll_lock = (::GetKeyState(VK_SCROLL) & 1) != 0 && !s.headless;
    u.scroll_known = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_ScrollLock, false)) u.scroll_lock = !u.scroll_lock;
  // a grouped sheet that went away (deleted, hidden) leaves the group; the active sheet outside it ends it
  if (!s.group_sheets.empty() && s.snap) {
    std::vector<uint32_t> keep;
    for (const uint32_t id : s.group_sheets) {
      const model::Sheet* sh = s.snap->Sheet(id);
      if (sh && sh->visible && !s.Win(id).floating) keep.push_back(id);
    }
    if (keep.size() < 2 || std::find(keep.begin(), keep.end(), s.active_sheet) == keep.end()) keep.clear();
    if (keep != s.group_sheets) SetSheetGroup(s, keep, s.active_sheet);
  }
  if (ui::Command* c = ui::Commands().Find("FillAcrossWorksheets")) c->enabled = s.group_sheets.size() > 1;
  if (ui::Command* c = ui::Commands().Find("UngroupSheets")) c->enabled = s.group_sheets.size() > 1;
  if (ui::Command* c = ui::Commands().Find("SelectObjects")) c->pressed = u.select_objects;
  if (ui::Command* c = ui::Commands().Find("ClearFilterColumn")) {
    // Clear Filter From "Header": the active cell's column, when it is filtered
    c->enabled = false;
    c->label = TR("Clear Filter");
    if (const model::Sheet* sh = s.FocusSheet(); sh && sh->autofilter->on) {
      const auto& af = *sh->autofilter;
      const uint32_t col = s.FocusView().sel.act_c;
      if (col >= af.range.c0 && col <= af.range.c1) {
        const std::string head = CellText(s, *sh, af.range.r0, col);
        c->label = std::string(TR("Clear Filter From \"")) + (head.empty() ? core::CellA1(0, col).substr(0, core::CellA1(0, col).size() - 1) : head) + "\"";
        for (const auto& f : af.columns) c->enabled = c->enabled || (f.field == col - af.range.c0 && f.kind != 0);
      }
    }
  }
  if (u.select_objects && (ImGui::IsKeyPressed(ImGuiKey_Escape) || s.edit.active)) u.select_objects = false;
  UpdateLinkedPictures(s);
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 1.0f);
  DrawMoveMenu(s);
  DrawFillMenu(s);
  DrawPickList(s);
  DrawActivateList(s);
  DrawFillAcrossDialog(s);
  ImGui::PopFont();
}

// ---- commands ----

void RegisterCellCommands(AppState& s) {
  auto& R = ui::Commands();
  R.Add(Make("SelectAllSheets", TR("Select All Sheets"), "", TR("Group every sheet: what you type or format on one goes to all of them."))).execute = [&s](ui::Command&) {
    SetSheetGroup(s, TabSheets(s), s.active_sheet);
  };
  R.Add(Make("UngroupSheets", TR("Ungroup Sheets"))).execute = [&s](ui::Command&) { SetSheetGroup(s, {}, s.active_sheet); };
  // Ctrl+Shift+PgDn / PgUp: the next / previous sheet joins the group and is shown (Excel)
  auto extend = [&s](int dir) {
    const std::vector<uint32_t> tabs = TabSheets(s);
    const auto it = std::find(tabs.begin(), tabs.end(), s.active_sheet);
    if (it == tabs.end()) return;
    const ptrdiff_t i = (it - tabs.begin()) + dir;
    if (i < 0 || i >= static_cast<ptrdiff_t>(tabs.size())) return;
    std::vector<uint32_t> g = s.group_sheets.empty() ? std::vector<uint32_t>{s.active_sheet} : s.group_sheets;
    g.push_back(tabs[static_cast<size_t>(i)]);
    SetSheetGroup(s, g, tabs[static_cast<size_t>(i)]);
  };
  R.Add(Make("SelectNextSheet", TR("Select Next Sheet"), "Ctrl+Shift+PageDown")).execute = [extend](ui::Command&) { extend(+1); };
  R.Add(Make("SelectPrevSheet", TR("Select Previous Sheet"), "Ctrl+Shift+PageUp")).execute = [extend](ui::Command&) { extend(-1); };
  R.Add(Make("ActivateSheetList", TR("Activate..."), "", TR("List every sheet to go to one (right-click the arrows left of the sheet tabs)."))).execute = [&s](ui::Command&) {
    s.OpenDialog(Dialog::ActivateSheetList);
  };
  R.Add(Make("SelectRowDifferences", TR("Row Differences"), "Ctrl+\\", TR("Select the cells of each row that differ from the cell in the active cell's column."))).execute = [&s](ui::Command&) {
    SelectDifferences(s, true);
  };
  R.Add(Make("SelectColumnDifferences", TR("Column Differences"), "Ctrl+Shift+|", TR("Select the cells of each column that differ from the cell in the active cell's row."))).execute =
      [&s](ui::Command&) { SelectDifferences(s, false); };
  R.Add(Make("ToggleObjectsDisplay", TR("Hide / Show Objects"), "Ctrl+6", TR("Hide or show the shapes, pictures, charts and controls on the sheets."))).execute = [&s](ui::Command&) {
    s.hide_objects = !s.hide_objects;
  };
  R.Add(Make("ToggleOutlineSymbols", TR("Outline Symbols"), "Ctrl+8", TR("Show or hide the outline's bars and level buttons."))).execute = [&s](ui::Command&) {
    auto& w = s.Win(s.focus_sheet);
    w.outline = !w.outline;
  };
  R.Add(Make("NextPane", TR("Next Pane"), "F6")).execute = [&s](ui::Command&) { NextPane(s, false); };
  R.Add(Make("PrevPane", TR("Previous Pane"), "Shift+F6")).execute = [&s](ui::Command&) { NextPane(s, true); };
  R.Add(Make("FillJustify", TR("Justify"), "", TR("Spread the text of the selected cells over lines as wide as the selection."))).execute = [&s](ui::Command&) {
    if (s.edit.active) return;
    Justify(s, s.focus_sheet, SelRect(s));
  };
  R.Add(Make("FillAcrossWorksheets", TR("Across Worksheets..."), "", TR("Copy the selection to the same cells of the other grouped sheets."))).execute = [&s](ui::Command&) {
    if (s.group_sheets.size() < 2) return;
    s.OpenDialog(Dialog::FillAcrossSheets);
  };
  {
    ui::Command c = Make("SelectObjects", TR("Select Objects"), "", TR("Select shapes, pictures, charts and controls: click them, or drag a box around them. Esc ends it."),
                         ui::icon::Shapes);
    c.toggle = true;
    R.Add(std::move(c)).execute = [](ui::Command&) {
      C().select_objects = !C().select_objects;
      C().lasso = false;
    };
  }
  R.Add(Make("FilterBySelectedValue", TR("Filter by Selected Cell's Value"))).execute = [&s](ui::Command&) { FilterBySelected(s, 1); };
  R.Add(Make("FilterBySelectedColor", TR("Filter by Selected Cell's Color"))).execute = [&s](ui::Command&) { FilterBySelected(s, 4); };
  R.Add(Make("FilterBySelectedFontColor", TR("Filter by Selected Cell's Font Color"))).execute = [&s](ui::Command&) { FilterBySelected(s, 5); };
  R.Add(Make("FilterBySelectedIcon", TR("Filter by Selected Cell's Icon"))).execute = [&s](ui::Command&) { FilterBySelected(s, 7); };
  R.Add(Make("ClearFilterColumn", TR("Clear Filter"))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh || !sh->autofilter->on || !s.engine) return;
    const auto& af = *sh->autofilter;
    const uint32_t col = s.FocusView().sel.act_c;
    if (col < af.range.c0 || col > af.range.c1) return;
    model::FilterColumn fc;
    fc.field = col - af.range.c0;
    fc.kind = 0;
    const uint32_t sheet = sh->id;
    s.engine->PostUser(TR("Clear Filter"), [sheet, fc](Document& d) { d.SetFilterColumn(sheet, fc); });
  };
  R.Add(Make("PickFromList", TR("Pick From Drop-down List..."), "Alt+Down", TR("Choose an entry already typed in the column."))).execute = [&s](ui::Command&) {
    if (!s.edit.active) OpenPickList(s);
  };
}

// ---- Python ----

io::MV CellsRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err) {
  using io::MV;
  if (m == "cells.group_get") {
    MV a = MV::Array();
    if (s.group_sheets.size() > 1) {
      for (const uint32_t id : s.group_sheets) a.Push(MV::Int(id));
    } else {
      a.Push(MV::Int(s.active_sheet));
    }
    return a;
  }
  if (m == "cells.group_set") {
    // Worksheets(Array(...)).Select / Worksheet.Select(Replace:=False)
    std::vector<uint32_t> ids;
    if (const MV* a = p.Get("ids")) {
      for (const MV& x : a->a) ids.push_back(static_cast<uint32_t>(x.i));
    }
    if (p.GetBool("add", false)) {
      std::vector<uint32_t> g = s.group_sheets.empty() ? std::vector<uint32_t>{s.active_sheet} : s.group_sheets;
      g.insert(g.end(), ids.begin(), ids.end());
      SetSheetGroup(s, g, s.active_sheet);
      return MV();
    }
    if (ids.empty()) return MV();
    const uint32_t active = p.Has("active") ? static_cast<uint32_t>(p.GetInt("active")) : ids.front();
    SetSheetGroup(s, ids, active);
    return MV();
  }
  if (m == "cells.fill_across") {
    std::vector<uint32_t> ids;
    if (const MV* a = p.Get("ids")) {
      for (const MV& x : a->a) ids.push_back(static_cast<uint32_t>(x.i));
    }
    const uint32_t from = static_cast<uint32_t>(p.GetInt("sheet"));
    std::vector<Rect> areas;
    if (const MV* a = p.Get("areas")) {
      for (const MV& x : a->a) {
        if (x.a.size() >= 4) areas.push_back(Rect{static_cast<uint32_t>(x.a[0].i), static_cast<uint32_t>(x.a[1].i), static_cast<uint32_t>(x.a[2].i), static_cast<uint32_t>(x.a[3].i)});
      }
    }
    if (std::find(ids.begin(), ids.end(), from) == ids.end()) ids.insert(ids.begin(), from);
    FillAcross(s, ids, from, areas, static_cast<int>(p.GetInt("kind", 0)));
    return MV();
  }
  if (m == "cells.justify") {
    const MV* a = p.Get("rect");
    if (!a || a->a.size() < 4) return MV();
    Justify(s, static_cast<uint32_t>(p.GetInt("sheet")),
            Rect{static_cast<uint32_t>(a->a[0].i), static_cast<uint32_t>(a->a[1].i), static_cast<uint32_t>(a->a[2].i), static_cast<uint32_t>(a->a[3].i)});
    return MV();
  }
  if (m == "cells.insert_copied") {
    // Range.Insert while cells are copied or cut: Insert Copied Cells / Insert Cut Cells (Excel)
    const MV* a = p.Get("rect");
    if (!a || a->a.size() < 4) return MV::Bool(false);
    const Rect at{static_cast<uint32_t>(a->a[0].i), static_cast<uint32_t>(a->a[1].i), static_cast<uint32_t>(a->a[2].i), static_cast<uint32_t>(a->a[3].i)};
    const std::string mode = p.GetStr("mode");
    const int what = mode == "rows" ? 1 : mode == "cols" ? 2 : mode == "right" ? 4 : 3;
    const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet"));
    if (s.snap && s.snap->Sheet(sheet) && sheet != s.focus_sheet) ActivateSheet(s, sheet);
    return MV::Bool(InsertCopiedAt(s, at, what));
  }
  if (m == "cells.display_objects") {
    if (p.Has("hide")) s.hide_objects = p.GetBool("hide");
    return MV::Bool(!s.hide_objects);
  }
  if (m == "cells.display_outline") {
    const uint32_t sheet = p.Has("sheet") ? static_cast<uint32_t>(p.GetInt("sheet")) : s.focus_sheet;
    auto& w = s.Win(sheet);
    if (p.Has("value")) w.outline = p.GetBool("value");
    return MV::Bool(w.outline);
  }
  if (m == "cells.paste_picture") {
    // Worksheet.Pictures.Paste(Link): a picture of the copied cells at the active cell
    if (ObjectClipboardPending() && !p.GetBool("link", false)) {  // a copied chart: its picture
      PasteAsPicture(s);
      return MV();
    }
    if (!CopyModeOn(s)) {
      *err = "Nothing is copied: copy cells first (Range.Copy)";
      return MV();
    }
    const auto& sel = s.FocusView().sel;
    PasteCellsPicture(s, s.focus_sheet, Rect::Cell(sel.act_r, sel.act_c), s.marquee_sheet, s.marquee_rect, p.GetBool("link", false));
    return MV();
  }
  *err = "unknown request " + m;
  return MV();
}

}  // namespace yc::app
