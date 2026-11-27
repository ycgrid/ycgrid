// SPDX-License-Identifier: Apache-2.0
// datatools_ui.cpp - Data tab tools working like Excel's: Subtotal (Outline), Forecast Sheet, Get Data > From Text/CSV
// (the preview window and Load To...), the live Flash Fill preview with its options button, and the tables' resize
// handle. The engine does the work (document_datatools.cpp); this file is the dialogs and the overlays.
#include "app_state.h"
#include "core/i18n.h"
#include "core/utf8.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/snapshot.h"
#include "fonts/fonts.h"
#include "formula/numfmt.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "io/csv.h"
#include "model/table.h"
#include "settings.h"
#include "yc_ui_items.h"

#include "imgui.h"

#include <windows.h>
#include <commdlg.h>
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

struct ToolsUi {
  // Subtotal
  Rect st_list{};
  int st_group = 0, st_func = 0;
  std::vector<char> st_cols;
  bool st_replace = true, st_breaks = false, st_below = true;
  // Forecast Sheet
  uint32_t fc_sheet = 0;
  char fc_timeline[160] = "", fc_values[160] = "", fc_end[64] = "", fc_start[64] = "";
  bool fc_ci = true, fc_stats = false, fc_column = false, fc_options = false, fc_header = true;
  float fc_conf = 95;
  int fc_season_mode = 0, fc_season = 12, fc_fill = 0, fc_agg = 0;
  std::string fc_key;  // the preview below was worked out for these settings
  std::vector<double> fc_ht, fc_hv, fc_ft, fc_fv, fc_lo, fc_hi;
  std::string fc_error;
  // From Text/CSV
  std::wstring im_path;
  std::string im_raw;  // the file's bytes
  int im_origin = 0, im_delim = 0, im_detect = 0;
  char im_custom[8] = "";
  std::vector<std::vector<std::string>> im_rows;  // the fields (the header promoted when there is one)
  std::vector<std::string> im_head;
  std::vector<int> im_types;  // per column: 0 detected (numbers, dates), 1 text
  std::string im_key;
  int id_view = 0, id_where = 1;
  char id_cell[64] = "";
  // Flash Fill: the live preview while typing in a column next to data, and the options button after a fill
  bool ff_live = false;
  uint32_t ff_sheet = 0, ff_r = 0, ff_c = 0;
  std::string ff_text;
  const void* ff_snap = nullptr;
  std::vector<std::pair<uint32_t, std::string>> ff_sugg;
  // the suggestions are worked out by the engine off the frame, one question at a time (they depend on the cell and
  // the column, not on the text typed): asked again when the cell or the snapshot changes, shown while the text typed
  // starts its row's suggestion (a Call here waited 1.5 s in the frame behind a million formulas filled just before)
  struct FfAsk {
    std::atomic<bool> done{false};
    uint32_t sheet = 0, r = 0, c = 0;
    std::vector<std::pair<uint32_t, std::string>> got;
  };
  std::shared_ptr<FfAsk> ff_ask;
  std::shared_ptr<FfAsk> ff_answer;  // the last answer, for its cell
  const void* ff_sugg_src = nullptr; // the answer ff_live / ff_sugg were worked out from, for ff_text
  uint32_t ff_q_sheet = 0, ff_q_r = 0, ff_q_c = 0;  // the cell last asked about, with ff_snap
  bool ffo_show = false;
  uint32_t ffo_sheet = 0, ffo_r = 0, ffo_c = 0;
  std::vector<uint32_t> ffo_rows;
  std::vector<std::string> ffo_texts;  // what was filled: the button goes once a filled cell changes
  size_t ffo_depth = 0;
  // the table resize handle being dragged
  bool tr_drag = false;
  uint32_t tr_sheet = 0, tr_id = 0;
  Rect tr_rect{};
};

ToolsUi& U() {
  static ToolsUi u;
  return u;
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
  if (!open || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
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

std::string Abs(const Rect& r) { return core::CellA1(r.r0, r.c0, true, true) + ":" + core::CellA1(r.r1, r.c1, true, true); }

std::string CellText(AppState& s, const model::Sheet& sh, uint32_t r, uint32_t c) {
  const auto& tbl = *s.snap->wb.styles;
  return engine::DisplayTextOf(sh.GetValue(r, c), tbl.NumberFormat(tbl.Get(sh.GetStyle(r, c))));
}

// a combo noted for the UI item recorder (tests find it by its id, as a person by its place)
bool Combo(const char* id, int* v, const char* const* items, int n, float w) {
  ImGui::SetNextItemWidth(w);
  ui::NoteUiItem(id, ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(w, ImGui::GetFrameHeight())));
  return ui::Combo(id, v, items, n);
}

// ---- Subtotal ----

const char* const* SubtotalFuncs() {
  static const char* k[] = {TR("Sum"), TR("Count"), TR("Average"), TR("Max"), TR("Min"), TR("Product"), TR("Count Numbers"), TR("StdDev"), TR("StdDevp"), TR("Var"), TR("Varp")};
  return k;
}
// the dialog's list -> SUBTOTAL's function number
const int kSubtotalNum[] = {9, 3, 1, 4, 5, 6, 2, 7, 8, 10, 11};

void OpenSubtotal(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap) return;
  const auto& sel = s.FocusView().sel;
  if (model::TableAt(s.snap->wb, s.focus_sheet, sel.act_r, sel.act_c)) {
    s.ShowMessage("YCGrid", TR("Subtotals can't be added to a table. Convert the table to a range first (Table Design > Convert to Range)."));
    return;
  }
  Rect r = SelRect(s);
  if (r.IsCell()) r = engine::CurrentRegion(*sh, r.r0, r.c0);
  if (sh->has_used) r.r1 = std::min(r.r1, std::max(r.r0, sh->used.r1)), r.c1 = std::min(r.c1, std::max(r.c0, sh->used.c1));
  if (r.Rows() < 2) {
    s.ShowMessage("YCGrid", TR("Select a cell in the list first: the list needs a header row and rows of data."));
    return;
  }
  ToolsUi& u = U();
  u.st_list = r;
  u.st_group = 0;
  u.st_func = 0;
  u.st_cols.assign(r.Cols(), 0);
  u.st_cols.back() = 1;  // Excel ticks the last column
  u.st_replace = true;
  u.st_breaks = false;
  u.st_below = sh->outline_below;
  s.OpenDialog(Dialog::Subtotal);
}

void DrawSubtotal(AppState& s) {
  ToolsUi& u = U();
  if (!BeginDlg(s, Dialog::Subtotal, TR("Subtotal###yc_dlg_subtotal"), 330)) return;
  const model::Sheet* sh = s.FocusSheet();
  bool close = false;
  std::vector<std::string> names;
  for (uint32_t c = u.st_list.c0; c <= u.st_list.c1; ++c) {
    std::string n = sh ? CellText(s, *sh, u.st_list.r0, c) : std::string();
    names.push_back(n.empty() ? TR("Column ") + core::ColumnName(c) : n);
  }
  const float w = S(300);
  ImGui::TextUnformatted(TR("At each change in:"));
  ImGui::SetNextItemWidth(w);
  ui::NoteUiItem("##st_group", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(w, ImGui::GetFrameHeight())));
  u.st_group = std::clamp(u.st_group, 0, static_cast<int>(names.size()) - 1);
  if (ui::BeginCombo("##st_group", "")) {
    for (size_t k = 0; k < names.size(); ++k) {
      ImGui::PushID(static_cast<int>(k));
      if (ImGui::Selectable("##g", u.st_group == static_cast<int>(k))) u.st_group = static_cast<int>(k);
      ui::NoteUiText(names[k].c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));  // ui.items finds it by its text
      ImGui::SameLine(0, 0);
      ImGui::TextUnformatted(names[k].c_str());  // the header as it is ("C##1" too)
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  {  // the chosen heading drawn in the closed box
    const ImVec2 at = ImGui::GetItemRectMin() + ImGui::GetStyle().FramePadding;
    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), names[static_cast<size_t>(u.st_group)].c_str());
  }
  ImGui::TextUnformatted(TR("Use function:"));
  Combo("##st_func", &u.st_func, SubtotalFuncs(), 11, w);
  ImGui::TextUnformatted(TR("Add subtotal to:"));
  if (ImGui::BeginChild("##st_cols", ImVec2(w, S(110)), ImGuiChildFlags_Borders)) {
    for (size_t k = 0; k < names.size(); ++k) {
      ImGui::PushID(static_cast<int>(k));
      bool on = u.st_cols[k] != 0;
      if (ImGui::Checkbox("##c", &on)) u.st_cols[k] = on ? 1 : 0;
      ImGui::SameLine();
      ImGui::TextUnformatted(names[k].c_str());
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  // Excel 16's letters (UI Automation's AccessKey of its Subtotal dialog): Replace current subtotals C, Page break
  // between groups P, Summary below data S, Remove All R
  if (DlgKey(ImGuiKey_C)) u.st_replace = !u.st_replace;
  if (DlgKey(ImGuiKey_P)) u.st_breaks = !u.st_breaks;
  if (DlgKey(ImGuiKey_S)) u.st_below = !u.st_below;
  ImGui::Checkbox(TR("Replace current subtotals"), &u.st_replace);
  ImGui::Checkbox(TR("Page break between groups"), &u.st_breaks);
  ImGui::Checkbox(TR("Summary below data"), &u.st_below);
  ImGui::Separator();
  const uint32_t sheet = s.focus_sheet;
  const Rect list = u.st_list;
  if (ImGui::Button(TR("Remove All"), ImVec2(S(90), 0)) || DlgKey(ImGuiKey_R)) {
    if (!RefuseReadOnlyUi(s)) {
      s.engine->Call([&](Document& d) {
        d.Begin(TR("Remove Subtotals"));
        d.RemoveSubtotal(sheet, list);
        d.Commit();
        return 0;
      });
    }
    close = true;
  }
  ImGui::SameLine();
  const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(90), 0));
  if (ok_pressed && RefuseReadOnlyUi(s)) {
    close = true;  // read-only mode: the dialog closes with the status bar's message (it stayed open over a blank message box)
  } else if (ok_pressed) {
    std::vector<uint32_t> cols;
    for (size_t k = 0; k < u.st_cols.size(); ++k) {
      if (u.st_cols[k]) cols.push_back(list.c0 + static_cast<uint32_t>(k));
    }
    std::string err;
    const uint32_t group = list.c0 + static_cast<uint32_t>(u.st_group);
    const int func = kSubtotalNum[std::clamp(u.st_func, 0, 10)];
    const bool replace = u.st_replace, breaks = u.st_breaks, below = u.st_below;
    const bool ok = s.engine->Call([&](Document& d) {
      d.Begin(TR("Subtotal"));
      const bool r = d.Subtotal(sheet, list, group, func, cols, replace, breaks, below, &err);
      d.Commit();
      return r;
    });
    if (ok) close = true;
    else if (!err.empty()) s.ShowMessage("YCGrid", err);  // never a blank message (a refusal the engine counted says why itself)
    else close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  EndDlg(s, close);
}

// ---- Forecast Sheet ----

bool ParseRange(AppState& s, const char* text, uint32_t* sheet, Rect* r) {
  std::vector<std::pair<uint32_t, Rect>> refs;
  const std::string t = text[0] == '=' ? text + 1 : text;
  const uint32_t def = s.focus_sheet;
  if (!s.engine->Call([&](Document& d) { return d.ParseRefList(t, def, &refs); }) || refs.size() != 1) return false;
  *sheet = refs[0].first;
  *r = refs[0].second;
  return true;
}

bool ParseNumber(const char* text, double* out) {
  const auto p = formula::ParseInput(text);
  if (!p.value.IsNumeric()) return false;
  *out = p.value.AsDouble();
  return true;
}

void OpenForecast(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || !s.snap) return;
  ToolsUi& u = U();
  Rect r = SelRect(s);
  if (r.IsCell()) r = engine::CurrentRegion(*sh, r.r0, r.c0);
  if (sh->has_used) r.r1 = std::min(r.r1, std::max(r.r0, sh->used.r1));
  // Excel takes the selection's two columns: the timeline (dates or numbers) and the values
  if (r.Cols() < 2 || r.Rows() < 3) {
    s.ShowMessage("YCGrid", TR("Select the timeline and its values (two columns, at least 3 points), then try Forecast Sheet again."));
    return;
  }
  u.fc_header = !sh->GetValue(r.r0, r.c0).IsNumeric();
  const uint32_t d0 = r.r0 + (u.fc_header ? 1 : 0);
  if (!sh->GetValue(d0, r.c0).IsNumeric()) {
    s.ShowMessage("YCGrid", TR("The timeline (the first column) must hold dates or numbers."));
    return;
  }
  u.fc_sheet = s.focus_sheet;
  const std::string q = core::QuoteSheetName(sh->name) + "!";
  std::snprintf(u.fc_timeline, sizeof(u.fc_timeline), "=%s%s", q.c_str(), Abs(Rect{r.r0, r.c0, r.r1, r.c0}).c_str());
  std::snprintf(u.fc_values, sizeof(u.fc_values), "=%s%s", q.c_str(), Abs(Rect{r.r0, r.c0 + 1, r.r1, r.c0 + 1}).c_str());
  // Forecast End: as far ahead as a third of the data, on the timeline's own format
  const auto& tbl = *s.snap->wb.styles;
  const std::string fmt = tbl.NumberFormat(tbl.Get(sh->GetStyle(d0, r.c0)));
  const double first = sh->GetValue(d0, r.c0).AsDouble(), last = sh->GetValue(r.r1, r.c0).AsDouble();
  const double step = (last - first) / std::max<double>(1, r.r1 - d0);
  const double end = last + step * std::max(1.0, std::round((r.r1 - d0 + 1) / 3.0));
  std::snprintf(u.fc_end, sizeof(u.fc_end), "%s", engine::DisplayTextOf(core::Value::Number(end), fmt).c_str());
  std::snprintf(u.fc_start, sizeof(u.fc_start), "%s", engine::DisplayTextOf(core::Value::Number(last), fmt).c_str());
  u.fc_key.clear();
  u.fc_options = false;
  s.OpenDialog(Dialog::ForecastSheet);
}

// the forecast for the preview: FORECAST.ETS at each future point, worked out by the engine
void ForecastPreview(AppState& s) {
  ToolsUi& u = U();
  char key[1024];
  std::snprintf(key, sizeof(key), "%s|%s|%s|%d|%g|%d|%d|%d|%d|%p", u.fc_timeline, u.fc_values, u.fc_end, u.fc_ci, u.fc_conf, u.fc_season_mode, u.fc_season, u.fc_fill,
                u.fc_agg, static_cast<const void*>(s.snap.get()));
  if (u.fc_key == key) return;
  u.fc_key = key;
  u.fc_ht.clear(), u.fc_hv.clear(), u.fc_ft.clear(), u.fc_fv.clear(), u.fc_lo.clear(), u.fc_hi.clear();
  u.fc_error.clear();
  uint32_t ts = 0, vs = 0;
  Rect tr, vr;
  double end = 0;
  if (!ParseRange(s, u.fc_timeline, &ts, &tr) || !ParseRange(s, u.fc_values, &vs, &vr) || tr.Rows() != vr.Rows()) {
    u.fc_error = TR("The timeline and the values must be ranges of the same size.");
    return;
  }
  if (!ParseNumber(u.fc_end, &end)) {
    u.fc_error = TR("Forecast End must be a date or a number.");
    return;
  }
  const model::Sheet* tsh = s.snap->Sheet(ts);
  const model::Sheet* vsh = s.snap->Sheet(vs);
  if (!tsh || !vsh) return;
  const uint32_t h = tsh->GetValue(tr.r0, tr.c0).IsNumeric() ? 0 : 1;
  for (uint32_t i = h; i < tr.Rows() && i < 5000; ++i) {
    const auto& t = tsh->GetValue(tr.r0 + i, tr.c0);
    const auto& v = vsh->GetValue(vr.r0 + i, vr.c0);
    if (!t.IsNumeric()) continue;
    u.fc_ht.push_back(t.AsDouble());
    u.fc_hv.push_back(v.IsNumeric() ? v.AsDouble() : NAN);
  }
  if (u.fc_ht.size() < 3) {
    u.fc_error = TR("The timeline and the values must be ranges of the same size with at least 3 points.");
    return;
  }
  const double last = u.fc_ht.back();
  double step = INFINITY;
  for (size_t i = 1; i < u.fc_ht.size(); ++i) {
    if (u.fc_ht[i] > u.fc_ht[i - 1]) step = std::min(step, u.fc_ht[i] - u.fc_ht[i - 1]);
  }
  if (!std::isfinite(step) || end <= last) {
    u.fc_error = TR("Forecast End must be after the last point of the timeline.");
    return;
  }
  const auto rq = [&](uint32_t sh, Rect r) {
    const model::Sheet* x = s.snap->Sheet(sh);
    return core::QuoteSheetName(x ? x->name : std::string()) + "!" + Abs(Rect{r.r0 + h, r.c0, r.r1, r.c1});
  };
  const std::string vals = rq(vs, vr), tl = rq(ts, tr);
  const std::string tail = "," + std::to_string(u.fc_season_mode == 0 ? 1 : std::max(1, u.fc_season)) + "," + (u.fc_fill == 1 ? "0" : "1") + "," +
                           std::to_string(u.fc_agg + 1) + ")";
  char conf[32];
  std::snprintf(conf, sizeof(conf), "%g", u.fc_conf / 100.0);
  const uint32_t sheet = ts;
  s.engine->Call([&](Document& d) {
    for (int k = 0; k <= 400; ++k) {
      const double x = last + step * k;
      if (x > end + step * 1e-9) break;
      char xs[64];
      std::snprintf(xs, sizeof(xs), "%.15g", x);
      std::string e;
      const core::Value f = k == 0 ? core::Value::Number(u.fc_hv.back()) : d.EvaluateFormula(sheet, std::string("FORECAST.ETS(") + xs + "," + vals + "," + tl + tail, &e);
      const core::Value ci = k == 0 ? core::Value::Number(0) : d.EvaluateFormula(sheet, std::string("FORECAST.ETS.CONFINT(") + xs + "," + vals + "," + tl + "," + conf + tail, &e);
      if (!f.IsNumeric()) {
        u.fc_error = TR("The forecast can't be worked out from this data.");
        return 0;
      }
      u.fc_ft.push_back(x);
      u.fc_fv.push_back(f.AsDouble());
      const double c = ci.IsNumeric() ? ci.AsDouble() : 0;
      u.fc_lo.push_back(f.AsDouble() - c);
      u.fc_hi.push_back(f.AsDouble() + c);
    }
    return 0;
  });
}

void DrawForecastChart(const ImVec2& size) {
  ToolsUi& u = U();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p0 + size, IM_COL32_WHITE);
  dl->AddRect(p0, p0 + size, IM_COL32(0xC8, 0xC8, 0xC8, 255));
  if (!u.fc_error.empty() || u.fc_ht.empty()) {
    dl->AddText(p0 + ImVec2(S(10), size.y / 2 - S(8)), IM_COL32(0x60, 0x60, 0x60, 255), u.fc_error.c_str());
    return;
  }
  double x0 = u.fc_ht.front(), x1 = u.fc_ft.empty() ? u.fc_ht.back() : u.fc_ft.back();
  double y0 = INFINITY, y1 = -INFINITY;
  auto span = [&](const std::vector<double>& v) {
    for (double y : v) {
      if (std::isfinite(y)) y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
  };
  span(u.fc_hv);
  span(u.fc_fv);
  if (u.fc_ci) span(u.fc_lo), span(u.fc_hi);
  if (!(y1 > y0)) y1 = y0 + 1;
  if (!(x1 > x0)) x1 = x0 + 1;
  const ImVec2 a = p0 + ImVec2(S(12), S(10)), b = p0 + size - ImVec2(S(12), S(24));
  auto pt = [&](double x, double y) {
    return ImVec2(a.x + static_cast<float>((x - x0) / (x1 - x0)) * (b.x - a.x), b.y - static_cast<float>((y - y0) / (y1 - y0)) * (b.y - a.y));
  };
  auto line = [&](const std::vector<double>& xs, const std::vector<double>& ys, ImU32 col, float th, bool bars) {
    for (size_t i = 0; i < xs.size() && i < ys.size(); ++i) {
      if (!std::isfinite(ys[i])) continue;
      if (bars) {
        const float bw = std::max(1.0f, (b.x - a.x) / static_cast<float>(u.fc_ht.size() + u.fc_ft.size()) * 0.6f);
        const ImVec2 t = pt(xs[i], ys[i]), z = pt(xs[i], std::max(y0, 0.0));
        dl->AddRectFilled(ImVec2(t.x - bw / 2, std::min(t.y, z.y)), ImVec2(t.x + bw / 2, std::max(t.y, z.y)), col);
      } else if (i > 0 && std::isfinite(ys[i - 1])) {
        dl->AddLine(pt(xs[i - 1], ys[i - 1]), pt(xs[i], ys[i]), col, th);
      }
    }
  };
  const ImU32 blue = IM_COL32(0x44, 0x72, 0xC4, 255), orange = IM_COL32(0xED, 0x7D, 0x31, 255);
  line(u.fc_ht, u.fc_hv, blue, S(2), u.fc_column);
  line(u.fc_ft, u.fc_fv, orange, S(2), u.fc_column);
  if (u.fc_ci && !u.fc_column) {
    line(u.fc_ft, u.fc_lo, orange, S(1), false);
    line(u.fc_ft, u.fc_hi, orange, S(1), false);
  }
  // the legend, as Excel's preview: Values, Forecast, Lower / Upper Confidence Bound
  float lx = p0.x + S(12);
  const float ly = p0.y + size.y - S(18);
  auto key = [&](ImU32 col, const char* t) {
    dl->AddLine(ImVec2(lx, ly + S(7)), ImVec2(lx + S(16), ly + S(7)), col, S(2));
    dl->AddText(ImVec2(lx + S(20), ly), IM_COL32(0x40, 0x40, 0x40, 255), t);
    lx += S(28) + ImGui::CalcTextSize(t).x;
  };
  key(blue, TR("Values"));
  key(orange, TR("Forecast"));
  if (u.fc_ci) key(orange, TR("Confidence Bound"));
}

void DrawForecast(AppState& s) {
  ToolsUi& u = U();
  if (!BeginDlg(s, Dialog::ForecastSheet, TR("Create Forecast Worksheet###yc_dlg_forecast"), 640)) return;
  bool close = false;
  ForecastPreview(s);
  // the chart types (line, column) at the top right
  ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - S(150));
  if (ui::RadioButton(TR("Line"), !u.fc_column)) u.fc_column = false;
  ImGui::SameLine();
  if (ui::RadioButton(TR("Column"), u.fc_column)) u.fc_column = true;
  DrawForecastChart(ImVec2(S(620), S(260)));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Forecast End"));
  ImGui::SameLine(S(140));
  ImGui::SetNextItemWidth(S(140));
  ImGui::InputText("##fc_end", u.fc_end, sizeof(u.fc_end));
  ImGui::SameLine();
  if (ImGui::ArrowButton("##fc_opts", u.fc_options ? ImGuiDir_Up : ImGuiDir_Down)) u.fc_options = !u.fc_options;
  ImGui::SameLine();
  ImGui::TextUnformatted(TR("Options"));
  if (u.fc_options) {
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Forecast Start"));
    ImGui::SameLine(S(160));
    ImGui::SetNextItemWidth(S(130));
    ImGui::InputText("##fc_start", u.fc_start, sizeof(u.fc_start));
    ImGui::Checkbox(TR("Confidence Interval"), &u.fc_ci);
    ImGui::SameLine(S(160));
    ImGui::SetNextItemWidth(S(90));
    ImGui::BeginDisabled(!u.fc_ci);
    ImGui::InputFloat("##fc_conf", &u.fc_conf, 1, 5, "%.0f%%");
    u.fc_conf = std::clamp(u.fc_conf, 1.0f, 99.0f);
    ImGui::EndDisabled();
    ImGui::TextUnformatted(TR("Seasonality"));
    ui::RadioButton(TR("Detect Automatically"), &u.fc_season_mode, 0);
    ui::RadioButton(TR("Set Manually"), &u.fc_season_mode, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(80));
    ImGui::BeginDisabled(u.fc_season_mode == 0);
    ImGui::InputInt("##fc_season", &u.fc_season);
    u.fc_season = std::clamp(u.fc_season, 1, 8760);
    ImGui::EndDisabled();
    ImGui::Checkbox(TR("Include forecast statistics"), &u.fc_stats);
    ImGui::EndGroup();
    ImGui::SameLine(S(330));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(TR("Timeline Range"));
    RangeBox(s, "forecast.timeline", u.fc_timeline, sizeof(u.fc_timeline), S(270), RangeBox_Sheet | RangeBox_Equals, TR("Timeline Range"));
    ImGui::TextUnformatted(TR("Values Range"));
    RangeBox(s, "forecast.values", u.fc_values, sizeof(u.fc_values), S(270), RangeBox_Sheet | RangeBox_Equals, TR("Values Range"));
    ImGui::TextUnformatted(TR("Fill Missing Points Using"));
    static const char* kFill[] = {TR("Interpolation"), TR("Zeros")};
    Combo("##fc_fill", &u.fc_fill, kFill, 2, S(270));
    ImGui::TextUnformatted(TR("Aggregate Duplicates Using"));
    static const char* kAgg[] = {TR("Average"), TR("Count"), TR("CountA"), TR("Max"), TR("Median"), TR("Min"), TR("Sum")};
    Combo("##fc_agg", &u.fc_agg, kAgg, 7, S(270));
    ImGui::EndGroup();
  }
  ImGui::Separator();
  ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - S(180));
  ImGui::BeginDisabled(!u.fc_error.empty());
  if (DlgOkButton(TR("Create"), ImVec2(S(90), 0))) {
    Document::ForecastRequest rq;
    uint32_t ts = 0, vs = 0;
    double end = 0, start = 0;
    if (ParseRange(s, u.fc_timeline, &ts, &rq.timeline) && ParseRange(s, u.fc_values, &vs, &rq.values) && ParseNumber(u.fc_end, &end)) {
      rq.sheet = ts;
      rq.has_header = s.snap->Sheet(ts) && !s.snap->Sheet(ts)->GetValue(rq.timeline.r0, rq.timeline.c0).IsNumeric();
      if (vs != ts) {
        s.ShowMessage("YCGrid", TR("The timeline and the values must be on the same sheet."));
      } else {
        rq.end = end;
        rq.start = ParseNumber(u.fc_start, &start) ? start : 0;
        rq.confidence = u.fc_conf / 100.0;
        rq.show_ci = u.fc_ci;
        rq.seasonality = u.fc_season_mode == 0 ? 0 : u.fc_season;
        rq.fill_zeros = u.fc_fill == 1;
        rq.aggregate = u.fc_agg + 1;
        rq.statistics = u.fc_stats;
        rq.column_chart = u.fc_column;
        std::string err;
        const uint32_t id = s.engine->Call([&](Document& d) {
          d.Begin(TR("Forecast Sheet"));
          const uint32_t r = d.ForecastSheet(rq, &err);
          d.Commit();
          return r;
        });
        if (!id) {
          s.ShowMessage("YCGrid", err);
        } else {
          s.engine->Sync();
          s.snap = s.engine->Current();
          ActivateSheet(s, id);
          close = true;
        }
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  EndDlg(s, close);
}

// ---- Get Data > From Text/CSV ----

struct Origin {
  unsigned cp;
  const char* name;
};
const Origin kOrigins[] = {{65001, "65001: Unicode (UTF-8)"},      {1252, "1252: Western European (Windows)"}, {950, "950: Traditional Chinese (Big5)"},
                           {936, "936: Simplified Chinese (GB2312)"}, {932, "932: Japanese (Shift-JIS)"},        {949, "949: Korean"},
                           {1200, "1200: Unicode"}};
const char* kDelims[] = {"Comma", "Semicolon", "Tab", "Space", "Colon", "Equals Sign", "--Custom--", "--Fixed Width--"};

std::string Decode(const std::string& raw, unsigned cp) {
  if (cp == 65001 || cp == 1200) return io::DecodeText(raw);  // a BOM and UTF-16 are recognized too
  const int n = ::MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
  if (n <= 0) return io::DecodeText(raw);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n);
  return ToUtf8(w);
}

void ParseImport(ToolsUi& u) {
  const std::string text = Decode(u.im_raw, kOrigins[std::clamp(u.im_origin, 0, 6)].cp);
  u.im_rows.clear();
  u.im_head.clear();
  if (u.im_delim <= 5 || (u.im_delim == 6 && u.im_custom[0])) {
    const char ch = u.im_delim == 6 ? u.im_custom[0] : ",;\t :="[u.im_delim];
    u.im_rows = io::ParseCsv(text, ch).rows;
  } else {
    // fixed width: breaks where every line has a space column (the Text to Columns guess)
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(l);
    }
    size_t width = 0;
    for (const auto& l : lines) width = std::max(width, l.size());
    engine::TextToColumnsOptions o;
    o.fixed = true;
    for (size_t k = 1; k < width; ++k) {
      bool blank = true, after = false;
      for (const auto& l : lines) {
        blank = blank && (k >= l.size() || l[k - 1] == ' ');
        after = after || (k < l.size() && l[k] != ' ');
      }
      if (blank && after) o.breaks.push_back(static_cast<uint32_t>(k));
    }
    for (const auto& l : lines) {
      auto parts = engine::SplitColumns(l, o);
      for (auto& p : parts) {
        while (!p.empty() && p.back() == ' ') p.pop_back();
        while (!p.empty() && p.front() == ' ') p.erase(0, 1);
      }
      u.im_rows.push_back(std::move(parts));
    }
  }
  while (!u.im_rows.empty() && u.im_rows.back().size() <= 1 && (u.im_rows.back().empty() || u.im_rows.back()[0].empty())) u.im_rows.pop_back();
  size_t cols = 0;
  for (const auto& r : u.im_rows) cols = std::max(cols, r.size());
  // Data Type Detection: a column whose values all read as numbers or dates is typed, the others stay text
  u.im_types.assign(cols, u.im_detect == 2 ? 1 : 0);
  auto numeric = [](const std::string& f) { return !f.empty() && formula::ParseInput(f).value.IsNumeric(); };
  const size_t sample = u.im_detect == 0 ? 201 : u.im_rows.size();
  if (u.im_detect != 2) {
    for (size_t c = 0; c < cols; ++c) {
      bool all = true, any = false;
      for (size_t r = 1; r < u.im_rows.size() && r < sample; ++r) {
        const std::string f = c < u.im_rows[r].size() ? u.im_rows[r][c] : std::string();
        if (f.empty()) continue;
        any = true;
        all = all && numeric(f);
      }
      u.im_types[c] = any && all ? 0 : 1;
    }
  }
  // the first row becomes the headers when it is text over typed columns (as Power Query promotes headers)
  bool promote = !u.im_rows.empty() && u.im_rows.size() > 1;
  for (size_t c = 0; promote && c < cols; ++c) {
    const std::string f = c < u.im_rows[0].size() ? u.im_rows[0][c] : std::string();
    promote = !f.empty() && !numeric(f);
  }
  promote = promote && (u.im_detect == 2 || std::find(u.im_types.begin(), u.im_types.end(), 0) != u.im_types.end());
  if (promote) {
    u.im_head = u.im_rows[0];
    u.im_rows.erase(u.im_rows.begin());
  }
  for (size_t c = u.im_head.size(); c < cols; ++c) u.im_head.push_back("Column" + std::to_string(c + 1));
}

void OpenImportText(AppState& s) {
  if (!s.FocusSheet()) return;
  wchar_t file[MAX_PATH * 4] = L"";
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = s.hwnd;
  ofn.lpstrFilter = L"Text Files (*.prn;*.txt;*.csv)\0*.prn;*.txt;*.csv\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = static_cast<DWORD>(std::size(file));
  ofn.lpstrTitle = L"Import Data";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  std::wstring answer;
  const bool hidden = NoFileDialog(s.hwnd, TR("Import Data"), &answer);
  if (!(hidden ? !answer.empty() : ::GetOpenFileNameW(&ofn) != 0)) return;
  ToolsUi& u = U();
  u.im_path = hidden ? answer : std::wstring(file);
  std::ifstream in(std::filesystem::path(u.im_path), std::ios::binary);
  if (!in) {
    s.ShowError("YCGrid", TR("The file can't be read: ") + ToUtf8(u.im_path));
    return;
  }
  u.im_raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  // File Origin: UTF-8 when the bytes are UTF-8 (or have a BOM), else the system's code page
  const bool utf8 = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u.im_raw.data(), static_cast<int>(u.im_raw.size()), nullptr, 0) > 0 ||
                    (u.im_raw.size() >= 2 && (static_cast<unsigned char>(u.im_raw[0]) == 0xFF || static_cast<unsigned char>(u.im_raw[0]) == 0xFE));
  u.im_origin = 0;
  if (!utf8) {
    for (int k = 0; k < 7; ++k) {
      if (kOrigins[k].cp == ::GetACP()) u.im_origin = k;
    }
  }
  // Delimiter: the one the first lines share
  const std::string text = Decode(u.im_raw, kOrigins[u.im_origin].cp);
  const io::CsvTable guess = io::ParseCsv(text, 0);
  u.im_delim = guess.delimiter == ';' ? 1 : guess.delimiter == '\t' ? 2 : 0;
  u.im_detect = 0;
  u.im_custom[0] = 0;
  u.im_key.clear();
  ParseImport(u);
  s.OpenDialog(Dialog::ImportText);
}

std::string TableNameFor(const std::wstring& path) {
  std::string n = ToUtf8(std::filesystem::path(path).stem().wstring());
  for (char& ch : n) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' || (static_cast<unsigned char>(ch) & 0x80))) ch = '_';
  }
  if (n.empty() || std::isdigit(static_cast<unsigned char>(n[0]))) n = "_" + n;
  return n;
}

// Load: the data as a table at (r, c) of a sheet (a new one named after the file when new_sheet)
void LoadImport(AppState& s, bool new_sheet, uint32_t sheet, uint32_t r, uint32_t c) {
  ToolsUi& u = U();
  std::vector<std::vector<std::string>> rows;
  rows.push_back(u.im_head);
  rows.insert(rows.end(), u.im_rows.begin(), u.im_rows.end());
  const std::string name = TableNameFor(u.im_path);
  const std::vector<int> types = u.im_types;
  std::string err;
  uint32_t target = sheet;
  const bool ok = s.engine->Call([&](Document& d) {
    d.Begin(TR("Import Data"));
    if (new_sheet) {
      int32_t at = -1;
      for (size_t i = 0; i < d.wb().sheets.size(); ++i) {
        if (d.wb().sheets[i]->id == s.focus_sheet) at = static_cast<int32_t>(i) + 1;
      }
      std::string sname(core::Utf8First(name, 31));  // Excel's 31 characters (never half a character)
      for (int k = 2; d.wb().FindSheet(sname) >= 0; ++k) sname = std::string(core::Utf8First(name, 27)) + " (" + std::to_string(k) + ")";
      target = d.AddSheet(sname, at);
    }
    // the header row stays text
    std::vector<std::vector<std::string>> head(rows.begin(), rows.begin() + 1), body(rows.begin() + 1, rows.end());
    const Rect h = d.ImportRows(target, r, c, head, std::vector<int>(types.size(), 1));
    Rect b = body.empty() ? h : d.ImportRows(target, r + 1, c, body, types);
    const Rect area{r, c, std::max(h.r1, b.r1), std::max(h.c1, b.c1)};
    const uint32_t id = d.AddTable(target, area, true, "TableStyleMedium2", &err);
    if (id) {
      std::string tname = name;
      for (int k = 2; !d.RenameTable(target, id, tname, nullptr) && k < 1000; ++k) tname = name + "_" + std::to_string(k);
    }
    d.Commit();
    return id != 0;
  });
  if (!ok) {
    s.ShowError("YCGrid", err.empty() ? TR("The data couldn't be loaded here.") : err);
    return;
  }
  s.engine->Sync();
  s.snap = s.engine->Current();
  if (target != s.focus_sheet) ActivateSheet(s, target);
}

void DrawImportText(AppState& s) {
  ToolsUi& u = U();
  const std::string title = ToUtf8(std::filesystem::path(u.im_path).filename().wstring()) + "###yc_dlg_importtext";
  if (!BeginDlg(s, Dialog::ImportText, title.c_str(), 760)) return;
  bool close = false;
  bool changed = false;
  const float w = S(230);
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("File Origin"));
  const char* origins[7];
  for (int k = 0; k < 7; ++k) origins[k] = kOrigins[k].name;
  changed |= Combo("##im_origin", &u.im_origin, origins, 7, w);
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Delimiter"));
  const char* delims[8];
  for (int k = 0; k < 8; ++k) delims[k] = TR(kDelims[k]);
  changed |= Combo("##im_delim", &u.im_delim, delims, 8, w);
  if (u.im_delim == 6) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(40));
    changed |= ImGui::InputText("##im_custom", u.im_custom, 2);
  }
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Data Type Detection"));
  static const char* kDetect[] = {TR("Based on first 200 rows"), TR("Based on entire dataset"), TR("Do not detect data types")};
  changed |= Combo("##im_detect", &u.im_detect, kDetect, 3, w);
  ImGui::EndGroup();
  if (changed) ParseImport(u);
  // the preview: the first 200 rows under the headers
  const int cols = static_cast<int>(std::min<size_t>(u.im_head.size(), 64));
  if (cols > 0 && ImGui::BeginTable("##im_preview", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
                                    ImVec2(S(740), S(320)))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for (int c = 0; c < cols; ++c) ImGui::TableSetupColumn((u.im_head[static_cast<size_t>(c)] + "##h" + std::to_string(c)).c_str(), ImGuiTableColumnFlags_WidthFixed, S(110));
    ImGui::TableHeadersRow();
    for (size_t r = 0; r < u.im_rows.size() && r < 200; ++r) {
      ImGui::TableNextRow();
      for (int c = 0; c < cols; ++c) {
        ImGui::TableNextColumn();
        const auto& row = u.im_rows[r];
        if (static_cast<size_t>(c) < row.size()) ImGui::TextUnformatted(row[static_cast<size_t>(c)].c_str());
      }
    }
    ImGui::EndTable();
  }
  if (u.im_rows.size() > 200) ImGui::TextDisabled("%s", TR("The data in the preview has been truncated due to size limits."));
  ImGui::Separator();
  ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - S(330));
  if (DlgOkButton(TR("Load"), ImVec2(S(90), 0))) {
    close = true;
    EndDlg(s, close);
    LoadImport(s, true, 0, 0, 0);
    return;
  }
  ImGui::SameLine(0, 0);
  if (ImGui::ArrowButton("##im_loadmenu", ImGuiDir_Down)) ImGui::OpenPopup("##im_load");
  if (ImGui::BeginPopup("##im_load")) {
    if (ImGui::Selectable(TR("Load"))) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      EndDlg(s, true);
      LoadImport(s, true, 0, 0, 0);
      return;
    }
    if (ImGui::Selectable(TR("Load To..."))) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      EndDlg(s, true);
      const auto& sel = s.FocusView().sel;
      std::snprintf(u.id_cell, sizeof(u.id_cell), "=%s", core::CellA1(sel.act_r, sel.act_c, true, true).c_str());
      u.id_view = 0;
      u.id_where = 0;
      s.OpenDialog(Dialog::ImportData);
      return;
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled();  // Power Query's editor isn't part of YCGrid
  ImGui::Button(TR("Transform Data"), ImVec2(S(120), 0));
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", TR("YCGrid has no Power Query editor: load the data, then shape it with formulas or in Python (pandas)."));
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  EndDlg(s, close);
}

void DrawImportData(AppState& s) {
  ToolsUi& u = U();
  if (!BeginDlg(s, Dialog::ImportData, TR("Import Data###yc_dlg_importdata"), 360)) return;
  bool close = false;
  ImGui::TextUnformatted(TR("Select how you want to view this data in your workbook."));
  ui::RadioButton(TR("Table"), &u.id_view, 0);
  ImGui::BeginDisabled();  // these show a query's connection: YCGrid has no Power Query connections
  ui::RadioButton(TR("PivotTable Report"), &u.id_view, 1);
  ui::RadioButton(TR("PivotChart"), &u.id_view, 2);
  ui::RadioButton(TR("Only Create Connection"), &u.id_view, 3);
  ImGui::EndDisabled();
  ImGui::TextUnformatted(TR("Where do you want to put the data?"));
  ui::RadioButton(TR("Existing worksheet:"), &u.id_where, 0);
  ImGui::Indent();
  ImGui::BeginDisabled(u.id_where != 0);
  RangeBox(s, "importdata.cell", u.id_cell, sizeof(u.id_cell), S(260), RangeBox_Cell | RangeBox_Equals, TR("Existing worksheet:"));
  ImGui::EndDisabled();
  ImGui::Unindent();
  ui::RadioButton(TR("New worksheet"), &u.id_where, 1);
  ImGui::Separator();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    if (u.id_where == 1) {
      EndDlg(s, true);
      LoadImport(s, true, 0, 0, 0);
      return;
    }
    uint32_t sh = 0;
    Rect r;
    if (!ParseRange(s, u.id_cell, &sh, &r)) {
      s.ShowMessage("YCGrid", TR("Enter a valid cell reference."));
    } else {
      EndDlg(s, true);
      LoadImport(s, false, sh, r.r0, r.c0);
      return;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
  EndDlg(s, close);
}

// ---- Flash Fill: the live preview and the options button ----

void FlashFillPreview(AppState& s, uint32_t sheet_id, const model::Sheet& sh, const render::OverlayGeom& g) {
  ToolsUi& u = U();
  const auto& e = s.edit;
  const bool want = e.active && e.sheet == sheet_id && Config().auto_flash_fill && !e.text.empty() && e.text[0] != '=' && e.r > 0 &&
                    e.text.find('\n') == std::string::npos && !sh.GetValue(e.r - 1, e.c).IsEmpty() && sh.GetValue(e.r, e.c).IsEmpty();
  if (!want) {
    u.ff_live = false;
    u.ff_sugg_src = nullptr;
    return;
  }
  if (u.ff_ask && u.ff_ask->done.load(std::memory_order_acquire)) {
    u.ff_answer = std::move(u.ff_ask);
    u.ff_ask = nullptr;
  }
  const bool asked = u.ff_q_sheet == sheet_id && u.ff_q_r == e.r && u.ff_q_c == e.c && u.ff_snap == s.snap.get();
  if (!u.ff_ask && !asked) {
    u.ff_q_sheet = sheet_id, u.ff_q_r = e.r, u.ff_q_c = e.c, u.ff_snap = s.snap.get();
    auto a = std::make_shared<ToolsUi::FfAsk>();
    a->sheet = sheet_id, a->r = e.r, a->c = e.c;
    u.ff_ask = a;
    s.engine->Post([a](Document& d) {
      std::string err;
      d.FlashFill(a->sheet, a->r, a->c, &err, &a->got, true);
      a->done.store(true, std::memory_order_release);
    });
  }
  if (u.ff_ask) ui::RequestRedrawIn(1.0 / 30);
  // live when the entry being typed starts the suggestion for its own row, and more rows follow (worked out again
  // only when the answer, the cell or the text changes: the list holds a row per cell of the column)
  const ToolsUi::FfAsk* ans = u.ff_answer.get();
  if (u.ff_sugg_src == ans && u.ff_sheet == sheet_id && u.ff_r == e.r && u.ff_c == e.c && u.ff_text == e.text) {
    ans = nullptr;
  } else {
    u.ff_sugg_src = ans;
    u.ff_sheet = sheet_id, u.ff_r = e.r, u.ff_c = e.c, u.ff_text = e.text;
    u.ff_live = false;
    u.ff_sugg.clear();
  }
  if (ans && ans->sheet == sheet_id && ans->r == e.r && ans->c == e.c && ans->got.size() >= 2) {
    auto lower = [](std::string x) {
      for (char& ch : x) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return x;
    };
    for (const auto& [row, text] : ans->got) {
      if (row == e.r && lower(text).rfind(lower(e.text), 0) == 0) u.ff_live = true;
    }
    if (u.ff_live) u.ff_sugg = ans->got;
  }
  if (!u.ff_live) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  for (const auto& [row, text] : u.ff_sugg) {
    if (row == e.r) continue;
    const ImVec2 a = g.to_screen(static_cast<double>(sh.cols->Offset(e.c)), static_cast<double>(sh.rows->Offset(row)));
    const float h = static_cast<float>(sh.rows->Size(row) * g.sc);
    if (a.y > g.cells.Max.y) break;
    dl->AddText(ImVec2(a.x + S(3), a.y + h - ImGui::GetFontSize() - S(2)), IM_COL32(0x9A, 0x9A, 0x9A, 255), text.c_str());
  }
  dl->PopClipRect();
}

void FlashFillOptions(AppState& s, uint32_t sheet_id, const model::Sheet& sh, const render::OverlayGeom& g) {
  ToolsUi& u = U();
  if (!u.ffo_show || u.ffo_sheet != sheet_id || s.edit.active) return;
  const auto& sel = s.FocusView().sel;
  // the button goes when the work goes on elsewhere: the active cell leaves the column, or a filled cell changes
  bool same = true;
  for (size_t k = 0; k < u.ffo_rows.size() && k < u.ffo_texts.size() && k < 64 && same; ++k) {
    const core::Value& v = sh.GetValue(u.ffo_rows[k], u.ffo_c);
    same = v.IsString() ? v.str() == u.ffo_texts[k] : engine::DisplayTextOf(v, "General") == u.ffo_texts[k];
  }
  if (s.focus_sheet != sheet_id || sel.act_c != u.ffo_c || !same) {
    if (!ImGui::IsPopupOpen("##ffopts")) u.ffo_show = false;
    return;
  }
  const ImVec2 br = g.to_screen(static_cast<double>(sh.cols->Offset(static_cast<int64_t>(u.ffo_c) + 1)), static_cast<double>(sh.rows->Offset(u.ffo_r)));
  const float w = S(34), h = S(20);
  const ImVec2 p(br.x + S(3), br.y + S(2));
  if (!g.cells.Contains(p)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID("ffopts");
  const bool clicked = ImGui::InvisibleButton("##btn", ImVec2(w, h));
  ui::NoteUiItem("##flashfill_options", ImRect(p, p + ImVec2(w, h)));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  dl->AddRectFilled(p, p + ImVec2(w, h), hovered ? IM_COL32(0xE1, 0xEC, 0xF7, 255) : IM_COL32_WHITE);
  dl->AddRect(p, p + ImVec2(w, h), IM_COL32(0xA0, 0xA0, 0xA0, 255));
  dl->AddText(ui::F().icons, h * 0.75f, ImVec2(p.x + S(2), p.y + h * 0.12f), IM_COL32(0x40, 0x40, 0x40, 255), ui::icon::FlashFill);
  dl->AddText(ImVec2(p.x + h * 0.85f, p.y + (h - ImGui::GetFontSize() * 0.8f) * 0.5f), IM_COL32(0x40, 0x40, 0x40, 255), "\xe2\x96\xbe");
  if (hovered) ImGui::SetTooltip("%s", TR("Flash Fill Options"));
  if (clicked) {
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + h + S(2)));
    ImGui::OpenPopup("##ffopts");
  }
  if (ImGui::BeginPopup("##ffopts")) {
    if (ImGui::Selectable(TR("Undo Flash Fill"))) {
      const size_t depth = u.ffo_depth;
      s.engine->Call([&](Document& d) { return d.UndoDepth() == depth ? d.Undo() : false; });  // only the fill's own step
      u.ffo_show = false;
    }
    if (ImGui::Selectable(TR("Accept suggestions"))) u.ffo_show = false;
    const std::string sel_text = TR("Select all ") + std::to_string(u.ffo_rows.size()) + TR(" changed cells");
    if (ImGui::Selectable(sel_text.c_str())) {
      auto& view = s.FocusView();
      std::vector<Rect> areas;
      for (uint32_t r : u.ffo_rows) {
        if (!areas.empty() && areas.back().r1 + 1 == r) areas.back().r1 = r;
        else areas.push_back(Rect::Cell(r, u.ffo_c));
      }
      if (!areas.empty()) {
        std::rotate(areas.begin(), areas.begin() + 1, areas.end());
        view.sel.areas = areas;
        view.sel.anc_r = view.sel.act_r = areas.back().r0;
        view.sel.anc_c = view.sel.act_c = u.ffo_c;
        view.sel.mode = render::SelectMode::Cells;
      }
    }
    ImGui::EndPopup();
  }
}

// ---- Tables: the resize handle at the bottom-right corner ----

void TableResizeHandles(AppState& s, uint32_t sheet_id, const model::Sheet& sh, const render::OverlayGeom& g) {
  ToolsUi& u = U();
  if (sh.tables->empty() || s.edit.active) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const model::Table& t : *sh.tables) {
    const ImVec2 br = g.to_screen(static_cast<double>(sh.cols->Offset(static_cast<int64_t>(t.range.c1) + 1)), static_cast<double>(sh.rows->Offset(static_cast<int64_t>(t.range.r1) + 1)));
    const float k = S(7);
    const ImRect hr(br - ImVec2(k, k), br + ImVec2(S(1), S(1)));
    if (!g.cells.Contains(hr.Min)) continue;
    ImGui::SetCursorScreenPos(hr.Min);
    ImGui::PushID(static_cast<int>(t.id));
    ImGui::InvisibleButton("##tresize", hr.GetSize());
    const bool hov = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ui::NoteUiItem((std::string("##table_resize_") + t.name).c_str(), hr);
    ImGui::PopID();
    if (hov || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    // Excel's handle: a small corner mark
    dl->AddTriangleFilled(ImVec2(hr.Max.x - S(1), hr.Min.y + S(1)), ImVec2(hr.Max.x - S(1), hr.Max.y - S(1)), ImVec2(hr.Min.x + S(1), hr.Max.y - S(1)),
                          IM_COL32(0x44, 0x72, 0xC4, 255));
    if (active && ImGui::IsMouseDragging(0, 2.0f)) {
      u.tr_drag = true;
      u.tr_sheet = sheet_id;
      u.tr_id = t.id;
      // the cell under the mouse is the new corner (at least the header row and one row, the first column)
      const ImVec2 sp = g.to_sheet(ImGui::GetIO().MousePos);
      const int64_t r = std::max<int64_t>(sh.rows->IndexAt(static_cast<int64_t>(sp.y)), t.range.r0 + 1);
      const int64_t c = std::max<int64_t>(sh.cols->IndexAt(static_cast<int64_t>(sp.x)), t.range.c0);
      u.tr_rect = Rect{t.range.r0, t.range.c0, static_cast<uint32_t>(r), static_cast<uint32_t>(c)};
    }
    if (u.tr_drag && u.tr_id == t.id && u.tr_sheet == sheet_id) {
      // the dashed outline of the new size
      const ImVec2 a = g.to_screen(static_cast<double>(sh.cols->Offset(u.tr_rect.c0)), static_cast<double>(sh.rows->Offset(u.tr_rect.r0)));
      const ImVec2 b = g.to_screen(static_cast<double>(sh.cols->Offset(static_cast<int64_t>(u.tr_rect.c1) + 1)), static_cast<double>(sh.rows->Offset(static_cast<int64_t>(u.tr_rect.r1) + 1)));
      const ImU32 col = IM_COL32(0x40, 0x40, 0x40, 255);
      auto dash = [&](ImVec2 p, ImVec2 q) {
        const float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        for (float d = 0; d < len; d += S(8)) {
          const float e = std::min(len, d + S(4));
          dl->AddLine(p + (q - p) * (d / len), p + (q - p) * (e / len), col, S(1.5f));
        }
      };
      dash(a, ImVec2(b.x, a.y));
      dash(ImVec2(b.x, a.y), b);
      dash(b, ImVec2(a.x, b.y));
      dash(ImVec2(a.x, b.y), a);
      if (!active) {
        u.tr_drag = false;
        const uint32_t id = t.id;
        const Rect nr = u.tr_rect;
        if (!(nr == t.range)) {
          std::string err;
          const bool ok = s.engine->Call([&](Document& d) {
            d.Begin(TR("Resize Table"));
            const bool r = d.ResizeTable(sheet_id, id, nr, &err);
            d.Commit();
            return r;
          });
          if (!ok && !err.empty()) s.ShowError("YCGrid", err);
        }
      }
    }
  }
}

}  // namespace

// ---- Hooks ----

bool FlashFillAccept(AppState& s) {
  ToolsUi& u = U();
  const auto& e = s.edit;
  if (!u.ff_live || !e.active || e.sheet != u.ff_sheet || e.r != u.ff_r || e.c != u.ff_c || e.text != u.ff_text) return false;
  u.ff_live = false;
  const uint32_t sheet = e.sheet, c = e.c;
  const auto sugg = u.ff_sugg;
  size_t depth = 0;
  s.engine->Call([&](Document& d) {
    d.Begin(TR("Flash Fill"));
    for (const auto& [row, text] : sugg) d.SetInput(sheet, row, c, text.empty() || text[0] == '=' ? "'" + text : text);
    d.Commit();
    depth = d.UndoDepth();
    return 0;
  });
  std::vector<uint32_t> rows;
  std::vector<std::string> texts;
  for (const auto& x : sugg) rows.push_back(x.first), texts.push_back(x.second);
  FlashFillDone(s, sheet, e.r, c, rows, depth, texts);
  return true;
}

void FlashFillDone(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::vector<uint32_t>& rows, size_t undo_depth,
                   const std::vector<std::string>& texts) {
  ToolsUi& u = U();
  u.ffo_show = !rows.empty();
  u.ffo_sheet = sheet;
  u.ffo_r = r;
  u.ffo_c = c;
  u.ffo_rows = rows;
  u.ffo_depth = undo_depth;
  u.ffo_texts = texts;
  s.engine->Sync();
  s.snap = s.engine->Current();
  s.SetStatus(TR("Flash Fill changed cells: ") + std::to_string(rows.size()));
}

void DrawDataToolOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (!s.snap) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  TableResizeHandles(s, sheet_id, *sh, g);
  FlashFillPreview(s, sheet_id, *sh, g);
  FlashFillOptions(s, sheet_id, *sh, g);
}

void RegisterDataToolCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("Subtotal", TR("Subtotal"), ui::icon::AutoSum, TR("Total related rows of data by inserting subtotals and totals, and outline them."))).execute =
      [&s](ui::Command&) { OpenSubtotal(s); };
  R.Add(make("ForecastSheet", TR("Forecast\nSheet"), ui::icon::Forecast, TR("Create a new worksheet that predicts data trends from the historical data."))).execute =
      [&s](ui::Command&) { OpenForecast(s); };
  R.Add(make("ImportText", TR("From\nText/CSV"), ui::icon::ImportText, TR("Import data from a text, comma-separated value, or formatted text (space delimited) file."))).execute =
      [&s](ui::Command&) { OpenImportText(s); };
  R.Add(make("GetDataMenu", TR("Get\nData"), ui::icon::Data, TR("Import data from files: From File > From Text/CSV."))).execute = [](ui::Command&) {};
}

void DrawDataToolDialogs(AppState& s) {
  DrawSubtotal(s);
  DrawForecast(s);
  DrawImportText(s);
  DrawImportData(s);
}

}  // namespace yc::app
