// SPDX-License-Identifier: Apache-2.0
// editing_ui.cpp - custom lists, the AutoFill Options button, AutoRecover and in-place IME composition
#include "app_state.h"
#include "core/i18n.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "engine/custom_lists.h"
#include "fonts/fonts.h"
#include "formula/numfmt.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "io/ycg.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <imm.h>
#include <tlhelp32.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace yc::app {

using engine::Document;
using ui::S;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Custom lists (File > Options > Edit Custom Lists)
// ---------------------------------------------------------------------------

void ApplyCustomListSettings() { engine::SetUserFillLists(Config().custom_lists); }

namespace {

struct ListsDlg {
  int sel = -1;  // -1 = NEW LIST; else an index into AllFillLists
  char entries[16384] = {};
  bool loaded = false;
};
ListsDlg g_lists;

std::vector<std::string> SplitEntries(const char* text) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* p = text;; ++p) {
    if (*p == '\n' || *p == '\r' || *p == ',' || *p == 0) {
      while (!cur.empty() && cur.back() == ' ') cur.pop_back();
      size_t a = 0;
      while (a < cur.size() && cur[a] == ' ') ++a;
      if (a < cur.size()) out.push_back(cur.substr(a));
      cur.clear();
      if (*p == 0) break;
    } else {
      cur += *p;
    }
  }
  return out;
}

void SaveLists(AppState& s) {
  ApplyCustomListSettings();
  if (!SaveSettings()) s.SetStatus(TR("Couldn't save the settings: ") + ToUtf8(SettingsPath()), 8.0);
}

}  // namespace

void DrawCustomListsDialog(AppState& s) {
  if (s.dialog != Dialog::CustomLists) {
    g_lists.loaded = false;
    return;
  }
  const char* title = TR("Custom Lists");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(620), S(420)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) return;
  ListsDlg& d = g_lists;
  const auto builtin = engine::BuiltinFillLists();
  const size_t nb = builtin.size();
  auto& user = Config().custom_lists;
  auto all = engine::AllFillLists();
  auto load = [&](int i) {
    d.sel = i;
    std::string t;
    if (i >= 0 && static_cast<size_t>(i) < all.size()) {
      for (const auto& e : all[static_cast<size_t>(i)]) t += e + "\n";
    }
    std::snprintf(d.entries, sizeof d.entries, "%s", t.c_str());
  };
  if (!d.loaded) {
    load(-1);
    d.loaded = true;
  }
  ImGui::BeginChild("##lists", ImVec2(S(260), S(300)), ImGuiChildFlags_Borders);
  if (ImGui::Selectable(TR("NEW LIST"), d.sel == -1)) load(-1);
  for (size_t i = 0; i < all.size(); ++i) {
    std::string label;
    for (const auto& e : all[i]) {
      if (!label.empty()) label += ", ";
      label += e;
      if (label.size() > 40) {
        label += "...";
        break;
      }
    }
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(label.c_str(), d.sel == static_cast<int>(i))) load(static_cast<int>(i));
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("List entries:"));
  const bool is_builtin = d.sel >= 0 && static_cast<size_t>(d.sel) < nb;
  ImGui::InputTextMultiline("##entries", d.entries, sizeof d.entries, ImVec2(S(220), S(220)), is_builtin ? ImGuiInputTextFlags_ReadOnly : 0);
  ImGui::BeginDisabled(is_builtin);
  if (ImGui::Button(d.sel == -1 ? TR("Add") : TR("Update"), ImVec2(S(100), 0))) {
    auto items = SplitEntries(d.entries);
    if (items.size() >= 2) {
      if (d.sel == -1) {
        user.push_back(items);
        d.sel = static_cast<int>(nb + user.size() - 1);
      } else {
        user[static_cast<size_t>(d.sel) - nb] = items;
      }
      SaveLists(s);
    } else {
      s.ShowMessage("YCGrid", TR("A list needs at least two entries."));
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(d.sel < 0);
  if (ImGui::Button(TR("Delete"), ImVec2(S(100), 0))) {
    user.erase(user.begin() + (d.sel - static_cast<int>(nb)));
    SaveLists(s);
    load(-1);
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  // Import list from cells: the selection's texts in order
  if (ImGui::Button(TR("Import from Selection"), ImVec2(S(208), 0))) {
    const model::Sheet* sh = s.FocusSheet();
    std::vector<std::string> items;
    if (sh) {
      const Rect r = SelRect(s);
      for (uint32_t rr = r.r0; rr <= r.r1 && rr - r.r0 < 1000; ++rr) {
        for (uint32_t cc = r.c0; cc <= r.c1 && cc - r.c0 < 1000; ++cc) {
          const core::Value& v = sh->GetValue(rr, cc);
          if (v.IsEmpty()) continue;
          items.push_back(v.IsString() ? std::string(v.str()) : engine::DisplayTextOf(v, "General"));
        }
      }
    }
    if (items.size() >= 2) {
      user.push_back(items);
      SaveLists(s);
      load(static_cast<int>(nb + user.size() - 1));
    } else {
      s.ShowMessage("YCGrid", TR("Select at least two cells with the list's entries first."));
    }
  }
  ImGui::EndGroup();
  ImGui::Separator();
  ImGui::TextDisabled("%s", TR("AutoFill continues these lists, and Sort can order by them."));
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(112));
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (DlgOkButton(TR("OK"), ImVec2(S(96), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// AutoFill Options (the button after dragging the fill handle)
// ---------------------------------------------------------------------------

void NoteAutoFill(AppState& s, uint32_t sheet, const Rect& src, const Rect& dest) {
  s.last_fill = AppState::LastFill{};
  if (!dest.Contains(src) || dest == src) return;
  s.last_fill.active = true;
  s.last_fill.sheet = sheet;
  s.last_fill.src = src;
  s.last_fill.dest = dest;
  // dates get Fill Days / Weekdays / Months / Years
  s.last_fill.series = FillMakesSeries(s, sheet, src, &s.last_fill.date);
}

bool FillMakesSeries(AppState& s, uint32_t sheet, const Rect& src, bool* date) {
  if (date) *date = false;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return false;
  const core::Value& v = sh->GetValue(src.r0, src.c0);
  const auto& st = s.snap->wb.styles->Get(sh->GetStyle(src.r0, src.c0));
  const std::string code = s.snap->wb.styles->NumberFormat(st);
  // a time alone (h:mm) has no date part: no Fill Days / Months, and one time fills by hours
  const bool time_only = v.IsNumber() && engine::IsTimeOnlyFormat(code);
  const bool is_date = v.IsNumber() && formula::IsDateFormat(code) && !time_only;
  if (date) *date = is_date;
  // what the default fill does: a single number or plain text is copied; a date, a time, Item1, Q1, Mon... make a series
  return !src.IsCell() || sh->GetFormula(src.r0, src.c0) || is_date || time_only || (v.IsString() && engine::FillTextSeries(v.str()));
}

void DrawFillOptions(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  auto& lf = s.last_fill;
  if (!lf.active || lf.sheet != sheet_id || s.edit.active) return;
  if (!Config().show_paste_options) return;  // Options: "Show Paste Options button" hides Auto Fill Options too (Excel)
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  // the button stays while the filled range is still selected (like Excel); Escape puts it away (it sits over the
  // cell below and to the right, and took the next click there)
  const Rect sel = SelRect(s);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("##fillopts") && !EscClosedPopup()) {
    lf.active = false;
    return;
  }
  if (s.focus_sheet != sheet_id || !(sel == lf.dest)) {
    if (!ImGui::IsPopupOpen("##fillopts")) lf.active = false;
    return;
  }
  const ImVec2 br = g.to_screen(static_cast<double>(sh->cols->Offset(static_cast<int64_t>(lf.dest.c1) + 1)),
                                static_cast<double>(sh->rows->Offset(static_cast<int64_t>(lf.dest.r1) + 1)));
  const float sz = S(20);
  const ImVec2 p(br.x + S(3), br.y + S(3));
  if (!g.cells.Contains(br)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID("fillopts");
  const bool clicked = ImGui::InvisibleButton("##btn", ImVec2(sz, sz));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), hovered ? IM_COL32(0xE1, 0xEC, 0xF7, 255) : IM_COL32(255, 255, 255, 255));
  dl->AddRect(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(0xA0, 0xA0, 0xA0, 255));
  // a small grid with a plus: Excel's Auto Fill Options glyph
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      const ImVec2 a(p.x + S(4) + i * S(5), p.y + S(4) + j * S(5));
      dl->AddRect(a, ImVec2(a.x + S(5), a.y + S(5)), IM_COL32(0x21, 0x73, 0x46, 255));
    }
  }
  dl->AddLine(ImVec2(p.x + S(14), p.y + S(11)), ImVec2(p.x + S(18), p.y + S(15)), IM_COL32(0x40, 0x40, 0x40, 255));
  if (hovered) ImGui::SetTooltip("%s", TR("Auto Fill Options"));
  if (clicked) ImGui::OpenPopup("##fillopts");
  if (ImGui::BeginPopup("##fillopts")) {
    struct Opt {
      const char* label;
      int type;
    };
    std::vector<Opt> opts = {{TR("Copy Cells"), 1}, {TR("Fill Series"), 2}, {TR("Fill Formatting Only"), 3}, {TR("Fill Without Formatting"), 4}};
    if (lf.date) {
      opts.push_back({TR("Fill Days"), 5});
      opts.push_back({TR("Fill Weekdays"), 6});
      opts.push_back({TR("Fill Months"), 7});
      opts.push_back({TR("Fill Years"), 8});
    }
    opts.push_back({TR("Flash Fill"), 11});
    for (const auto& o : opts) {
      // the default fill: a single number or text is copied, anything else becomes a series (Excel's choice)
      const int def = lf.series ? 2 : 1;
      if (ImGui::RadioButton(o.label, lf.type == o.type || (lf.type == 0 && o.type == def))) {
        const int type = o.type;
        const uint32_t sht = lf.sheet;
        const Rect src = lf.src, dest = lf.dest;
        lf.type = type;
        // like Excel: the fill is redone the chosen way (one undo step replaces the other)
        s.engine->Post([](Document& d) { d.Undo(); });
        if (type == 11) {
          ui::Commands().Execute("FlashFill");
        } else {
          s.engine->PostUser(TR("AutoFill"), [=](Document& d) { d.AutoFill(sht, src, dest, type); });
        }
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// Excel's Insert Options button beside inserted rows, columns or cells: Format Same As Above / Below, Clear Formatting
void DrawInsertOptions(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  auto& li = s.last_insert;
  if (!li.active || li.sheet != sheet_id || s.edit.active) return;
  if (!Config().show_insert_options) return;  // Options > Advanced: "Show Insert Options buttons"
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  // the button stays while the selection is the one the insert was made with
  if (s.focus_sheet != sheet_id || !(SelRect(s) == li.sel)) {
    if (!ImGui::IsPopupOpen("##insopts")) li.active = false;
    return;
  }
  const auto& sel = s.FocusView().sel;
  // beside the inserted cells: under the active cell's column for rows, right of the active row for columns
  const bool cols = li.what == 2 || li.what == 4;
  const int64_t bc = li.what == 1 ? int64_t{sel.act_c} + 1 : int64_t{li.rect.c1} + 1;
  const int64_t br = li.what == 2 ? int64_t{sel.act_r} + 1 : int64_t{li.rect.r1} + 1;
  const ImVec2 at = g.to_screen(static_cast<double>(sh->cols->Offset(bc)), static_cast<double>(sh->rows->Offset(br)));
  if (!g.cells.Contains(at)) return;
  const float w = S(34), h = S(20);
  const ImVec2 p(at.x + S(3), at.y + S(3));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID("insopts");
  const bool clicked = ImGui::InvisibleButton("##insbtn", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hovered ? IM_COL32(0xE1, 0xEC, 0xF7, 255) : IM_COL32(255, 255, 255, 255));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(0xA0, 0xA0, 0xA0, 255));
  dl->AddText(ui::F().icons, h * 0.75f, ImVec2(p.x + S(2), p.y + h * 0.12f), IM_COL32(0x40, 0x40, 0x40, 255), ui::icon::FormatPainter);
  dl->AddText(ImVec2(p.x + h * 0.85f, p.y + (h - ImGui::GetFontSize() * 0.8f) * 0.5f), IM_COL32(0x40, 0x40, 0x40, 255), "\xe2\x96\xbe");
  if (hovered) ImGui::SetTooltip("%s", TR("Insert Options"));
  if (clicked) {
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + h + S(2)));
    ImGui::OpenPopup("##insopts");
  }
  if (ImGui::BeginPopup("##insopts")) {
    const char* labels[3] = {cols ? TR("Format Same As Left") : TR("Format Same As Above"), cols ? TR("Format Same As Right") : TR("Format Same As Below"),
                             TR("Clear Formatting")};
    for (int k = 0; k < 3; ++k) {
      if (!ImGui::RadioButton(labels[k], li.origin == k)) continue;
      li.origin = k;
      const uint32_t sheet = li.sheet;
      const Rect a = li.rect;
      const int what = li.what, origin = k;
      // like Excel: the insert is redone with the formats chosen (one undo step replaces the other)
      s.engine->Post([](Document& d) { d.Undo(); });
      s.engine->PostUser(TR("Insert"), [sheet, a, what, origin](Document& d) {
        if (what == 1) d.InsertRows(sheet, a.r0, static_cast<uint32_t>(a.Rows()), origin);
        else if (what == 2) d.InsertCols(sheet, a.c0, static_cast<uint32_t>(a.Cols()), origin);
        else d.InsertCells(sheet, a, what == 3, origin);
      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void DrawPasteOptions(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  auto& lp = s.last_paste;
  if (!lp.active || lp.sheet != sheet_id || s.edit.active) return;
  if (!Config().show_paste_options) return;  // Options > Advanced: "Show Paste Options button when content is pasted"
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  // the button stays while the pasted cells are still selected (like Excel); Escape puts it away, as in Excel (it
  // sits over the cell below and to the right, and took the next click there)
  const Rect sel = SelRect(s);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen("##pasteopts") && !EscClosedPopup()) {
    lp.active = false;
    return;
  }
  if (s.focus_sheet != sheet_id || !(sel == lp.dest || (lp.option == 4 && sel.Rows() == lp.dest.Cols() && sel.Cols() == lp.dest.Rows()))) {
    if (!ImGui::IsPopupOpen("##pasteopts")) lp.active = false;
    return;
  }
  const ImVec2 br = g.to_screen(static_cast<double>(sh->cols->Offset(static_cast<int64_t>(sel.c1) + 1)),
                                static_cast<double>(sh->rows->Offset(static_cast<int64_t>(sel.r1) + 1)));
  if (!g.cells.Contains(br)) return;
  const float w = S(34), h = S(20);
  const ImVec2 p(br.x + S(3), br.y + S(3));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID("pasteopts");
  const bool clicked = ImGui::InvisibleButton("##btn", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hovered ? IM_COL32(0xE1, 0xEC, 0xF7, 255) : IM_COL32(255, 255, 255, 255));
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(0xA0, 0xA0, 0xA0, 255));
  // a clipboard with "(Ctrl)" beside it: Excel's Paste Options glyph
  ImGui::GetWindowDrawList()->AddText(ui::F().icons, h * 0.75f, ImVec2(p.x + S(2), p.y + h * 0.12f), IM_COL32(0x40, 0x40, 0x40, 255), ui::icon::PasteSmall);
  dl->AddText(ImVec2(p.x + h * 0.85f, p.y + (h - ImGui::GetFontSize() * 0.8f) * 0.5f), IM_COL32(0x40, 0x40, 0x40, 255), "\xe2\x96\xbe");
  if (hovered) ImGui::SetTooltip("%s", TR("Paste Options (Ctrl)"));
  // as in Excel, Ctrl alone (pressed and released) opens the options too
  static bool ctrl_alone = false;
  if (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl) || ImGui::IsKeyPressed(ImGuiKey_RightCtrl)) ctrl_alone = true;
  else if (ImGui::GetIO().InputQueueCharacters.Size || ImGui::IsKeyPressed(ImGuiKey_C) || ImGui::IsKeyPressed(ImGuiKey_V) || ImGui::IsKeyPressed(ImGuiKey_Z)) ctrl_alone = false;
  const bool ctrl_up = ctrl_alone && !ImGui::GetIO().KeyCtrl;
  if (ctrl_up) ctrl_alone = false;
  if (clicked || ctrl_up) {
    ImGui::SetNextWindowPos(ImVec2(p.x, p.y + h + S(2)));
    ImGui::OpenPopup("##pasteopts");
  }
  if (ImGui::BeginPopup("##pasteopts")) {
    // Excel's three rows of pictures: Paste, Paste Values, Other Paste Options
    const int id = PasteGallery(lp.option);
    if (id == 10 || id == 11) {
      // a picture of the copied cells over the pasted ones; Linked Picture follows the cells as they change (Excel)
      lp.option = id;
      const uint32_t sheet = lp.sheet;
      const Rect dest = lp.dest;
      const std::shared_ptr<const engine::ClipData> clip = lp.clip;
      s.engine->Post([](Document& d) { d.Undo(); });
      PasteCellsPicture(s, sheet, dest, clip->src_sheet, clip->src, id == 11);
      ImGui::CloseCurrentPopup();
    } else if (id >= 0) {
      lp.option = id;
      const uint32_t sheet = lp.sheet;
      const Rect dest = lp.dest;
      const std::shared_ptr<const engine::ClipData> clip = lp.clip;
      // like Excel: the paste is redone the chosen way (one undo step replaces the other)
      s.engine->Post([](Document& d) { d.Undo(); });
      s.engine->PostUser(TR("Paste"), [sheet, dest, clip, id](Document& d) { ApplyPasteWay(d, sheet, dest, *clip, id); });
      if (id == 4) render::SelectRange(s.FocusView(), Rect{dest.r0, dest.c0, dest.r0 + clip->cols - 1, dest.c0 + clip->rows - 1});
      else render::SelectRange(s.FocusView(), dest);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// ---------------------------------------------------------------------------
// AutoRecover
// ---------------------------------------------------------------------------

namespace {

struct Recovered {
  std::wstring file, info;  // the saved copy and its description
  std::string original, name, when;
};
std::vector<Recovered> g_recovered;
bool g_recover_pane = false, g_recover_scanned = false;
std::wstring g_autosave_file;  // this session's AutoRecover copy
double g_last_autosave = 0;
uint64_t g_saved_version = 0;
std::atomic<bool> g_saving{false};

bool ProcessAlive(DWORD pid) {
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return false;
  DWORD code = 0;
  const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  ::CloseHandle(h);
  return alive;
}

void ScanRecovered() {
  g_recover_scanned = true;
  std::error_code ec;
  const fs::path dir = AutoRecoverDir();
  if (!fs::exists(dir, ec)) return;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != L".json") continue;
    std::ifstream f(e.path(), std::ios::binary);
    nlohmann::json j;
    try {
      f >> j;
    } catch (...) {
      continue;
    }
    const DWORD pid = j.value("pid", 0u);
    if (pid && ProcessAlive(pid) && pid != ::GetCurrentProcessId()) continue;  // still being edited by another YCGrid
    Recovered r;
    r.info = e.path().wstring();
    r.file = fs::path(e.path()).replace_extension(L".ycg").wstring();
    if (!fs::exists(r.file, ec)) continue;
    r.original = j.value("original", std::string());
    r.name = j.value("name", std::string("Book1"));
    r.when = j.value("time", std::string());
    g_recovered.push_back(std::move(r));
  }
  g_recover_pane = !g_recovered.empty();
}

void RemoveAutosave() {
  if (g_autosave_file.empty()) return;
  std::error_code ec;
  fs::remove(g_autosave_file, ec);
  fs::remove(fs::path(g_autosave_file).replace_extension(L".json"), ec);
  g_autosave_file.clear();
}

std::string NowText() {
  const std::time_t t = std::time(nullptr);
  std::tm lt{};
  localtime_s(&lt, &t);
  char b[64];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
  return b;
}

}  // namespace

bool OpenRecoveryPane() {
  g_recovered.clear();
  ScanRecovered();
  g_recover_scanned = true;
  return g_recover_pane;
}

void UpdateAutoRecover(AppState& s) {
  if (s.headless) return;
  if (!g_recover_scanned) ScanRecovered();
  if (!s.snap || !s.engine) return;
  const auto& cfg = Config();
  const bool dirty = s.snap->wb.dirty;
  // saved or discarded: the copy is no longer needed
  if (!dirty) {
    if (!g_autosave_file.empty() && !g_saving) RemoveAutosave();
    g_last_autosave = ImGui::GetTime();
    return;
  }
  if (!cfg.autorecover || g_saving) return;
  const double due = g_last_autosave + cfg.autorecover_minutes * 60.0;
  if (ImGui::GetTime() < due) {
    ui::RequestRedrawIn(due - ImGui::GetTime() + 0.05);  // wake up for it even when nothing else happens
    return;
  }
  if (s.snap->version == g_saved_version) return;
  g_last_autosave = ImGui::GetTime();
  g_saved_version = s.snap->version;
  std::error_code ec;
  fs::create_directories(AutoRecoverDir(), ec);
  const std::wstring stem = s.file_path.empty() ? L"Book1" : fs::path(s.file_path).stem().wstring();
  if (g_autosave_file.empty()) g_autosave_file = (fs::path(AutoRecoverDir()) / (stem + L"_" + std::to_wstring(::GetCurrentProcessId()) + L".ycg")).wstring();
  nlohmann::json info = {{"pid", ::GetCurrentProcessId()}, {"original", ToUtf8(s.file_path)}, {"name", ToUtf8(stem)}, {"time", NowText()}};
  io::SaveOptions opt;
  opt.layout_json = CaptureLayoutJson(s);
  // saved from the immutable snapshot on a worker thread: editing goes on
  g_saving = true;
  std::thread([snap = s.engine->Current(), opt, file = g_autosave_file, text = info.dump()] {
    std::string err;
    const std::wstring tmp = file + L".tmp";
    if (io::SaveYcg(snap->wb, tmp, opt, &err)) {
      ::MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
      std::ofstream(fs::path(file).replace_extension(L".json"), std::ios::binary) << text;
    }
    g_saving = false;
  }).detach();
}

void AutoRecoverOnExit() {
  for (int i = 0; i < 200 && g_saving; ++i) ::Sleep(10);
  RemoveAutosave();
}

void DrawRecoveryPane(AppState& s) {
  if (s.headless) return;
  if (!g_recover_pane) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + S(12), vp->WorkPos.y + S(200)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(S(320), S(300)), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(TR("Document Recovery###yc_recovery"), &g_recover_pane)) {
    ImGui::TextWrapped("%s", TR("YCGrid closed before these workbooks were saved. Open the versions you want to keep and save them."));
    ImGui::Separator();
    int remove = -1;
    for (size_t i = 0; i < g_recovered.size(); ++i) {
      const auto& r = g_recovered[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TextUnformatted(r.name.c_str());
      ImGui::TextDisabled(TR("AutoRecover version, %s"), r.when.c_str());
      if (!r.original.empty()) ImGui::TextDisabled("%s", r.original.c_str());
      if (ImGui::SmallButton(TR("Open"))) {
        if (ConfirmDiscard(s) && OpenWorkbook(s, r.file)) {
          // it continues as the original file (or untitled), with unsaved changes
          s.file_path = FromUtf8(r.original);
          const std::string orig = r.original;
          s.engine->Post([orig](Document& d) {
            d.wb_mut().path = orig;
            d.wb_mut().dirty = true;
          });
          s.SetStatus(TR("Recovered: ") + r.name);
          remove = static_cast<int>(i);
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton(TR("Delete"))) remove = static_cast<int>(i);
      ImGui::Separator();
      ImGui::PopID();
    }
    if (remove >= 0) {
      std::error_code ec;
      fs::remove(g_recovered[static_cast<size_t>(remove)].file, ec);
      fs::remove(g_recovered[static_cast<size_t>(remove)].info, ec);
      g_recovered.erase(g_recovered.begin() + remove);
      if (g_recovered.empty()) g_recover_pane = false;
    }
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------
// IME: the composition is drawn where the text goes (in the cell, the formula bar or a box) instead of in Windows' window
// ---------------------------------------------------------------------------

namespace {
std::string g_comp;
int g_comp_cursor = 0;
bool g_ime_start_edit = false;
}  // namespace

bool HandleImeMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result) {
  (void)s;
  switch (msg) {
    case WM_IME_SETCONTEXT:
      // no system composition window: we draw it (the candidate list stays)
      *result = ::DefWindowProcW(hwnd, msg, wparam, lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW));
      return true;
    case WM_IME_STARTCOMPOSITION:
      g_comp.clear();
      if (!ImGui::GetIO().WantTextInput) g_ime_start_edit = true;  // typing on a cell starts editing it (like Excel)
      *result = 0;
      return true;
    case WM_IME_COMPOSITION: {
      if (HIMC himc = ::ImmGetContext(hwnd)) {
        if (lparam & GCS_COMPSTR) {
          const LONG bytes = ::ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
          std::wstring w(static_cast<size_t>(std::max<LONG>(0, bytes)) / sizeof(wchar_t), L'\0');
          if (bytes > 0) ::ImmGetCompositionStringW(himc, GCS_COMPSTR, w.data(), static_cast<DWORD>(bytes));
          g_comp = ToUtf8(w);
          g_comp_cursor = static_cast<int>(::ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0));
        }
        if (lparam & GCS_RESULTSTR) g_comp.clear();
        ::ImmReleaseContext(hwnd, himc);
      }
      return false;  // DefWindowProc turns the result into WM_CHAR
    }
    case WM_IME_ENDCOMPOSITION:
      g_comp.clear();
      return false;
    default:
      return false;
  }
}

void UpdateIme(AppState& s) {
  if (g_ime_start_edit) {
    g_ime_start_edit = false;
    if (!s.edit.active && s.FocusSheet() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) StartEdit(s, true, {});
  }
}

void DrawImeComposition(AppState& s) {
  (void)s;
  if (g_comp.empty()) return;
  ImGuiContext& g = *GImGui;
  const ImGuiPlatformImeData& ime = g.PlatformImeData.WantVisible ? g.PlatformImeData : g.PlatformImeDataPrev;
  if (!ime.WantVisible) return;
  const float h = std::max(ime.InputLineHeight, S(14));
  ImFont* font = ui::F().grid;
  const float px = h;
  const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, g_comp.c_str());
  const ImVec2 p = ime.InputPos;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(p.x - 1, p.y), ImVec2(p.x + size.x + 2, p.y + h), IM_COL32(255, 255, 255, 255));
  dl->AddText(font, px, ImVec2(p.x, p.y + (h - size.y) * 0.5f), IM_COL32(0, 0, 0, 255), g_comp.c_str());
  // dotted underline under the composition, a caret at the IME's cursor
  for (float x = p.x; x < p.x + size.x; x += S(3)) dl->AddLine(ImVec2(x, p.y + h - 1), ImVec2(x + S(1.5f), p.y + h - 1), IM_COL32(0, 0, 0, 255));
  const std::wstring w = FromUtf8(g_comp);
  const std::string before = ToUtf8(w.substr(0, static_cast<size_t>(std::clamp(g_comp_cursor, 0, static_cast<int>(w.size())))));
  const float cx = p.x + font->CalcTextSizeA(px, FLT_MAX, 0.0f, before.c_str()).x;
  dl->AddLine(ImVec2(cx, p.y + 2), ImVec2(cx, p.y + h - 2), IM_COL32(0, 0, 0, 255));
}

bool ImeComposing() { return !g_comp.empty(); }

void SimulateIme(const std::string& text, bool commit) {
  if (commit) {
    g_comp.clear();
    ImGui::GetIO().AddInputCharactersUTF8(text.c_str());
    return;
  }
  if (g_comp.empty() && !ImGui::GetIO().WantTextInput) g_ime_start_edit = true;
  g_comp = text;
  g_comp_cursor = static_cast<int>(FromUtf8(text).size());
}

}  // namespace yc::app
