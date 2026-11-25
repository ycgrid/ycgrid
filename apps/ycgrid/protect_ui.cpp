// SPDX-License-Identifier: Apache-2.0
// protect_ui.cpp - Review tab: Protect Sheet, Protect Workbook, Allow Edit Ranges; the protected-sheet message
#include "trust.h"
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "io/ycg.h"

#include <windows.h>
#define SECURITY_WIN32
#include <security.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#pragma comment(lib, "secur32.lib")

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

struct ProtectUi {
  char pw[128] = "", pw2[128] = "";
  model::SheetProtection opts;
  bool structure = true, windows = false;
  // Allow Edit Ranges
  std::vector<model::AllowEditRange> ranges;
  std::vector<std::string> range_pw;  // new passwords typed in the dialog ("" = keep the old one)
  int sel = -1;
  char r_title[128] = "", r_ref[128] = "", r_pw[128] = "";
  int r_edit = -1;             // the range the New Range / Modify Range dialog changes (-1 = a new one)
  bool r_open = false;         // that dialog is to be opened
  bool protect_after = false;  // Allow Edit Ranges > Protect Sheet...: Protect Sheet opens once the ranges are kept
  bool contents = true;        // Protect worksheet and contents of locked cells
  bool confirm = false;        // Confirm Password is to be opened (Protect Sheet / Workbook, a range password)
  char confirm_pw[128] = "";
  int nested_closed = -100;  // the frame a dialog over this one closed: its Enter isn't this one's OK
  // Permissions...: the accounts of the range being edited (New / Modify Range) or of the one selected
  std::vector<model::AllowEditRange::User> r_users;
  std::vector<model::AllowEditRange::User>* perm_target = nullptr;
  std::string perm_title;
  bool perm_open = false;
  int perm_ctx = 0;  // 1 the main dialog, 2 New / Modify Range
  int perm_sel = -1;
  char perm_name[256] = "";
  std::string perm_msg;
  bool paste_perm = false;  // Paste permissions information into a new workbook
  // unlock a range (password prompt)
  uint32_t unlock_sheet = 0;
  int unlock_range = -1;
  bool wrong = false;
};

ProtectUi& P() {
  static ProtectUi p;
  return p;
}

bool BeginDlg(AppState& s, Dialog d, const char* title, float width) {
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  // a fixed width, as Excel's dialogs have (the text wraps and the boxes fill it); only the height follows the contents
  ImGui::SetNextWindowSizeConstraints(ImVec2(S(width), 0), ImVec2(S(width), FLT_MAX));
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return false;
  }
  // Escape belongs to a dialog opened over this one (Confirm Password, New Range)
  if (!open || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))) {
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

bool Password(const char* label, char* buf, size_t n) {
  return ImGui::InputText(label, buf, n, ImGuiInputTextFlags_Password);
}

void Clear() {
  ProtectUi& p = P();
  std::memset(p.pw, 0, sizeof(p.pw));
  std::memset(p.pw2, 0, sizeof(p.pw2));
  p.wrong = false;
  p.confirm = false;
}

// Excel's Confirm Password, opened over the dialog that set a password: 1 when the same password was typed again
// (OK), -1 when cancelled or different, 0 while it is open (or not open)
int ConfirmPassword(AppState& s, const char* typed) {
  ProtectUi& p = P();
  const char* title = TR("Confirm Password###yc_confirm_pw");
  if (p.confirm) {
    p.confirm = false;
    p.confirm_pw[0] = 0;
    ImGui::OpenPopup(title);
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(S(300), 0), ImVec2(S(300), FLT_MAX));  // fixed width, height to fit
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return 0;
  int result = 0;
  if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) result = -1;
  ImGui::TextUnformatted(TR("Reenter password to proceed."));
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##confirm", p.confirm_pw, sizeof(p.confirm_pw), ImGuiInputTextFlags_Password);
  ImGui::PushTextWrapPos(0.0f);  // wraps at the dialog's fixed width
  ImGui::TextDisabled("%s", TR("Caution: If you lose or forget the password, it cannot be recovered. It is advisable to keep a list of passwords and "
                               "their corresponding workbook and sheet names in a safe place. (Remember that passwords are case-sensitive.)"));
  ImGui::PopTextWrapPos();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    if (std::strcmp(p.confirm_pw, typed) == 0) {
      result = 1;
    } else {
      result = -1;
      s.ShowMessage("YCGrid", TR("The confirmation password is not identical."));
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) result = -1;
  if (result) ImGui::CloseCurrentPopup(), p.nested_closed = ImGui::GetFrameCount();
  ImGui::EndPopup();
  return result;
}

// OK of a dialog that opens others over it (Confirm Password, New Range): Enter pressed in those isn't its own
bool ParentOk() {
  const bool recent = ImGui::GetFrameCount() - P().nested_closed < 4 || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
  return recent ? ImGui::Button(TR("OK"), ImVec2(S(80), 0)) : DlgOkButton(TR("OK"), ImVec2(S(80), 0));
}

// the Windows account working here ("DOMAIN\\user"), as Excel's Permissions names it
std::string WindowsAccount() {
  wchar_t buf[512];
  ULONG n = 512;
  if (::GetUserNameExW(NameSamCompatible, buf, &n)) return ToUtf8(buf);
  DWORD m = 512;
  return ::GetUserNameW(buf, &m) ? ToUtf8(buf) : std::string();
}

// a typed account checked against Windows (Check Names): its full "DOMAIN\\user" name, empty when there's no such one
std::string CheckAccount(const std::string& typed) {
  const std::wstring w = FromUtf8(typed);
  BYTE sid[SECURITY_MAX_SID_SIZE];
  DWORD sid_n = sizeof(sid), dom_n = 256;
  wchar_t dom[256];
  SID_NAME_USE use{};
  if (w.empty() || !::LookupAccountNameW(nullptr, w.c_str(), sid, &sid_n, dom, &dom_n, &use)) return {};
  wchar_t name[256], dom2[256];
  DWORD name_n = 256, dom2_n = 256;
  if (!::LookupAccountSidW(nullptr, sid, name, &name_n, dom2, &dom2_n, &use)) return typed;
  return ToUtf8(dom2) + "\\" + ToUtf8(name);
}

// Permissions for a range: Windows accounts that edit it without its password (Allow) or not (Deny), as Excel's
void DrawPermissions(AppState& s, int ctx) {
  ProtectUi& p = P();
  const char* title = TR("Permissions###yc_range_perm");
  if (p.perm_open && p.perm_ctx == ctx) {
    p.perm_open = false;
    p.perm_sel = p.perm_target && !p.perm_target->empty() ? 0 : -1;
    std::snprintf(p.perm_name, sizeof(p.perm_name), "%s", WindowsAccount().c_str());
    p.perm_msg.clear();
    ImGui::OpenPopup(title);
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!BeginModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
  bool close = ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive();
  auto& users = *p.perm_target;
  ImGui::Text(TR("Permissions for %s"), p.perm_title.c_str());
  ImGui::TextUnformatted(TR("Group or user names:"));
  if (ImGui::BeginListBox("##users", ImVec2(S(380), ImGui::GetTextLineHeightWithSpacing() * 6))) {
    for (int i = 0; i < static_cast<int>(users.size()); ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(users[static_cast<size_t>(i)].name.c_str(), p.perm_sel == i)) p.perm_sel = i;
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }
  // Add: a name typed (the current account to start with), checked against Windows
  ImGui::SetNextItemWidth(S(260));
  ImGui::InputText("##permname", p.perm_name, sizeof(p.perm_name));
  ImGui::SameLine();
  if (ImGui::Button(TR("Add..."), ImVec2(S(110), 0))) {
    const std::string full = CheckAccount(p.perm_name);
    if (full.empty()) {
      p.perm_msg = std::string(TR("An object named \"")) + p.perm_name + TR("\" cannot be found. Check the name and try again.");
    } else if (std::none_of(users.begin(), users.end(), [&](const auto& u) { return u.name == full; })) {
      users.push_back({full, true});
      p.perm_sel = static_cast<int>(users.size()) - 1;
      p.perm_msg.clear();
    }
  }
  ImGui::BeginDisabled(p.perm_sel < 0 || p.perm_sel >= static_cast<int>(users.size()));
  if (ImGui::Button(TR("Remove"), ImVec2(S(110), 0)) && p.perm_sel >= 0 && p.perm_sel < static_cast<int>(users.size())) {
    users.erase(users.begin() + p.perm_sel);
    p.perm_sel = users.empty() ? -1 : std::min(p.perm_sel, static_cast<int>(users.size()) - 1);
  }
  ImGui::EndDisabled();
  if (!p.perm_msg.empty()) ImGui::TextColored(ImVec4(0.75f, 0.1f, 0.1f, 1), "%s", p.perm_msg.c_str());
  if (p.perm_sel >= 0 && p.perm_sel < static_cast<int>(users.size())) {
    auto& u = users[static_cast<size_t>(p.perm_sel)];
    ImGui::Text(TR("Permissions for %s"), u.name.c_str());
    ImGui::TextUnformatted(TR("Edit range without a password"));
    ImGui::SameLine(S(260));
    bool allow = u.allow, deny = !u.allow;
    if (ImGui::Checkbox(TR("Allow"), &allow)) u.allow = allow;
    ImGui::SameLine();
    if (ImGui::Checkbox(TR("Deny"), &deny)) u.allow = !deny;
  }
  ImGui::Separator();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) close = true;
  if (close) ImGui::CloseCurrentPopup(), p.nested_closed = ImGui::GetFrameCount();
  ImGui::EndPopup();
  (void)s;
}

std::string AbsRef(const Rect& r);

// Allow Edit Ranges > Paste permissions information into a new workbook: a new workbook (its own window, as File > New)
// with a sheet listing each range's title, address, whether it has a password, and its accounts with Allow / Deny
void PastePermissionsReport(AppState& s, const std::string& sheet_name, const std::vector<model::AllowEditRange>& ranges) {
  engine::Document d(model::Workbook::NewDefault(TR("Sheet")));
  const uint32_t sh = d.wb().sheets[0]->id;
  auto put = [&](uint32_t r, uint32_t c, const std::string& v) { d.SetValue(sh, r, c, core::Value::String(v), engine::kSrcUser); };
  put(0, 0, std::string(TR("Permissions for ")) + sheet_name);
  const char* heads[] = {TR("Range Title"), TR("Range"), TR("Password"), TR("User"), TR("Permission")};
  for (uint32_t c = 0; c < 5; ++c) put(2, c, heads[c]);
  d.ModifyStyle(sh, Rect{2, 0, 2, 4}, [](model::Style& x) { x.bold = true; });
  uint32_t row = 3;
  for (const auto& er : ranges) {
    const size_t n = std::max<size_t>(1, er.users.size());
    for (size_t k = 0; k < n; ++k, ++row) {
      put(row, 0, er.title);
      put(row, 1, sheet_name + "!" + AbsRef(er.rect).substr(1));
      put(row, 2, er.hash ? TR("Yes") : TR("No"));
      if (k < er.users.size()) {
        put(row, 3, er.users[k].name);
        put(row, 4, er.users[k].allow ? TR("Allow") : TR("Deny"));
      }
    }
  }
  wchar_t tmp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tmp);
  const std::wstring path = std::wstring(tmp) + L"YCGrid Permissions " + std::to_wstring(::GetTickCount64()) + L".ycg";
  const std::string bytes = io::SaveYcgToBytes(d.wb(), {});
  {
    std::ofstream f(path, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  if (!OpenInNewWindow(path, {}, true)) s.ShowError("YCGrid", TR("A new window couldn't be opened."));
}

std::string AbsRef(const Rect& r) {
  return "=" + core::CellA1(r.r0, r.c0, true, true) + (r.IsCell() ? "" : ":" + core::CellA1(r.r1, r.c1, true, true));
}

const char* kProtectedMsg = TR(
    "The cell or chart you're trying to change is on a protected sheet. To make a change, unprotect the sheet. You might be requested to enter a password.");

// the second line of the message: which cells (the user and the Error Log see what was refused)
std::string Where(const AppState& s, uint32_t sheet, const core::Rect& r) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  return sh ? "\n" + sh->name + "!" + core::RectA1(r) : std::string();
}

// the status bar's red line for a refusal on a protected sheet: which cells (the Error Log has the whole message)
std::string Brief(const AppState& s, uint32_t sheet, const core::Rect& r) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  return std::string(TR("Protected sheet, not changed: ")) + (sh ? sh->name + "!" + core::RectA1(r) : std::string());
}

}  // namespace

void ShowProtectedMessage(AppState& s) { s.NoteRefusal(kProtectedMsg, TR("Protected sheet: the object is locked")); }

bool SheetProtected(const AppState& s, uint32_t sheet) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  return sh && sh->protection->on;
}

bool RefuseLockedEdit(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || !sh->protection->on) return false;
  // cells inside an Allow Edit Range may be edited (a password, when needed, is asked for when the edit is refused)
  for (const auto& er : sh->protection->ranges) {
    if (er.rect.Contains(r, c)) return false;
  }
  if (!s.snap->wb.styles->Get(sh->GetStyle(r, c)).locked) return false;
  s.NoteRefusal(kProtectedMsg + Where(s, sheet, core::Rect::Cell(r, c)), Brief(s, sheet, core::Rect::Cell(r, c)));
  return true;
}

bool RefuseReadOnlyUi(AppState& s) {
  if (trust::InProtectedView()) {
    trust::ProtectedViewBlocked(s);
    return true;
  }
  if (s.operator_mode) return false;
  s.SetStatus(TR("Read-only mode: the sheet can't be changed. Turn on Trading > Operator Mode to edit."), 5.0);
  return true;
}

void CheckProtectBlocks(AppState& s) {
  static bool user_set = false;
  if (!user_set && s.engine) {
    user_set = true;
    const std::string who = WindowsAccount();
    s.engine->Call([who](Document& d) { d.current_user = who; });
  }
  static uint32_t seen = 0;
  if (!s.snap) return;
  if (s.snap->protect_blocks < seen) seen = s.snap->protect_blocks;  // another workbook
  if (s.snap->protect_blocks == seen) return;
  seen = s.snap->protect_blocks;
  if (s.snap->block_python) return;  // Python got an E_PROTECTED error instead
  if (s.snap->block_read_only && trust::InProtectedView()) return trust::ProtectedViewBlocked(s);
  if (s.snap->block_read_only) {
    s.SetStatus(TR("Read-only mode: the sheet can't be changed. Turn on Trading > Operator Mode to edit."), 5.0);
    return;
  }
  if (s.snap->block_pivot && s.snap->block_pivot_shift) {
    s.ShowError("YCGrid", TR("We can't make this change for the selected cells because it will affect a PivotTable. Use the field list to "
                             "change the report. If you are trying to insert or delete cells, move the PivotTable and try again."));
    return;
  }
  if (s.snap->block_pivot) {
    s.ShowError("YCGrid", TR("Cannot change this part of a PivotTable report."));
    return;
  }
  if (s.snap->block_table) {
    s.ShowError("YCGrid", TR("Cannot change part of a data table."));
    return;
  }
  if (s.snap->block_table_shift && s.snap->block_filter_shift) {
    s.ShowError("YCGrid", TR("This operation is not allowed. The operation is attempting to shift cells in a filtered range or table on your "
                             "worksheet."));
    return;
  }
  if (s.snap->block_table_shift) {
    s.ShowError("YCGrid", TR("The operation is not allowed. The operation is attempting to shift cells in a table on your worksheet."));
    return;
  }
  if (s.snap->block_array) {
    s.ShowError("YCGrid", TR("You can't change part of an array."));
    return;
  }
  if (s.snap->block_merge_size) {
    s.ShowError("YCGrid", TR("To do this, all the merged cells need to be the same size."));
    return;
  }
  if (s.snap->block_table_merge) {
    s.ShowError("YCGrid", TR("Cells in a table can't be merged."));
    return;
  }
  if (s.snap->block_too_large) {
    s.ShowError("YCGrid", TR("The selection is too large: it would write more than 20,000,000 cells. Select fewer cells."));
    return;
  }
  if (s.snap->block_off_sheet) {
    s.ShowError("YCGrid", TR("To prevent possible loss of data, YCGrid can't shift nonblank cells off of the worksheet. Select another "
                             "location in which to insert new cells, or delete data from the end of your worksheet."));
    return;
  }
  if (s.snap->block_param) {
    s.ShowMessage("YCGrid", TR("This is a parameter cell: type the new value in it (in operator mode). Pasting, filling and clearing don't change parameters."));
    return;
  }
  if (s.snap->block_range >= 0) {
    // the cell is in an edit range with a password: ask for it
    ProtectUi& p = P();
    Clear();
    p.unlock_sheet = s.snap->block_sheet;
    p.unlock_range = s.snap->block_range;
    s.OpenDialog(Dialog::UnlockRange);
    return;
  }
  // a protected sheet or workbook: no message box over the work, the Error Log and a red line on the status bar
  if (s.snap->block_sheet == 0) {
    s.NoteRefusal(TR("The workbook's structure is protected: sheets can't be added, deleted, renamed, moved or hidden."),
                  TR("Protected workbook structure, not changed"));
    return;
  }
  s.NoteRefusal(kProtectedMsg + Where(s, s.snap->block_sheet, s.snap->block_cells),
                Brief(s, s.snap->block_sheet, s.snap->block_cells));
}

// the Protect group follows the state, as in Excel: Protect Sheet reads Unprotect Sheet on a protected sheet, Protect
// Workbook shows pressed while the workbook is protected, Allow Edit Ranges waits until the sheet is unprotected
void SyncProtectCommands(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  const bool on = sh && sh->protection->on;
  if (auto* c = ui::Commands().Find("ProtectSheet")) {
    c->label = on ? TR("Unprotect\nSheet") : TR("Protect\nSheet");
    c->tip_desc = on ? TR("Unprotect the sheet, so its locked cells and objects can be changed again.")
                     : TR("Prevent unwanted changes by limiting what can be edited.");
  }
  if (auto* c = ui::Commands().Find("ProtectWorkbook")) {
    c->toggle = true;
    c->pressed = s.snap && (s.snap->wb.protect_structure || s.snap->wb.protect_windows);
  }
  if (auto* c = ui::Commands().Find("AllowEditRanges")) c->enabled = sh && !on;
}

void RegisterProtectCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("ProtectSheet", TR("Protect\nSheet"), ui::icon::ProtectSheet, TR("Prevent unwanted changes by limiting what can be edited (click again to unprotect)."))).execute =
      [&s](ui::Command&) {
        const model::Sheet* sh = s.FocusSheet();
        if (!sh) return;
        Clear();
        if (sh->protection->on) {
          if (sh->protection->hash == 0) {
            const uint32_t sheet = s.focus_sheet;
            s.engine->Call([sheet](Document& d) { d.UnprotectSheet(sheet, ""); });
          } else {
            s.OpenDialog(Dialog::UnprotectSheet);
          }
          return;
        }
        // like Excel, the dialog starts with the options this sheet was protected with last time
        P().opts = *sh->protection;
        P().contents = true;
        s.OpenDialog(Dialog::ProtectSheet);
      };
  R.Add(make("ProtectWorkbook", TR("Protect\nWorkbook"), ui::icon::ProtectWorkbook, TR("Prevent changes to the structure of the workbook, such as adding or deleting sheets (click again to unprotect)."))).execute =
      [&s](ui::Command&) {
        if (!s.snap) return;
        Clear();
        if (s.snap->wb.protect_structure || s.snap->wb.protect_windows) {
          if (s.snap->wb.protect_hash == 0) s.engine->Call([](Document& d) { d.UnprotectWorkbook(""); });
          else s.OpenDialog(Dialog::UnprotectWorkbook);
          return;
        }
        P().structure = true;
        s.OpenDialog(Dialog::ProtectWorkbook);
      };
  R.Add(make("AllowEditRanges", TR("Allow Edit\nRanges"), ui::icon::AllowEditRanges, TR("Choose ranges that may be edited while the sheet is protected, with an optional password."))).execute =
      [&s](ui::Command&) {
        const model::Sheet* sh = s.FocusSheet();
        if (!sh) return;
        ProtectUi& p = P();
        p.ranges = sh->protection->ranges;
        p.range_pw.assign(p.ranges.size(), std::string());
        p.sel = p.ranges.empty() ? -1 : 0;
        p.r_title[0] = p.r_ref[0] = p.r_pw[0] = 0;
        p.r_open = p.protect_after = false;
        s.OpenDialog(Dialog::AllowEditRanges);
      };
}

void DrawProtectDialogs(AppState& s) {
  ProtectUi& p = P();
  // Protect Sheet (Excel's dialog: the contents box, the password, the 15 things users may still do)
  if (BeginDlg(s, Dialog::ProtectSheet, TR("Protect Sheet###yc_dlg_protect"), 320)) {
    ImGui::Checkbox(TR("Protect worksheet and contents of locked cells"), &p.contents);
    ImGui::TextUnformatted(TR("Password to unprotect sheet:"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();  // typing starts in the password, as in Excel
    Password("##pw", p.pw, sizeof(p.pw));
    ImGui::TextUnformatted(TR("Allow all users of this worksheet to:"));
    // a list of nine rows that scrolls, as Excel's: all fifteen made the dialog taller than the window
    if (ImGui::BeginChild("##allow", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 9.0f + ImGui::GetStyle().WindowPadding.y * 2.0f),
                          ImGuiChildFlags_Borders)) {
      auto& o = p.opts;
      ImGui::Checkbox(TR("Select locked cells"), &o.select_locked);
      ImGui::Checkbox(TR("Select unlocked cells"), &o.select_unlocked);
      ImGui::Checkbox(TR("Format cells"), &o.format_cells);
      ImGui::Checkbox(TR("Format columns"), &o.format_columns);
      ImGui::Checkbox(TR("Format rows"), &o.format_rows);
      ImGui::Checkbox(TR("Insert columns"), &o.insert_columns);
      ImGui::Checkbox(TR("Insert rows"), &o.insert_rows);
      ImGui::Checkbox(TR("Insert hyperlinks"), &o.insert_hyperlinks);
      ImGui::Checkbox(TR("Delete columns"), &o.delete_columns);
      ImGui::Checkbox(TR("Delete rows"), &o.delete_rows);
      ImGui::Checkbox(TR("Sort"), &o.sort);
      ImGui::Checkbox(TR("Use AutoFilter"), &o.autofilter);
      ImGui::Checkbox(TR("Use PivotTable & PivotChart"), &o.pivot);
      ImGui::Checkbox(TR("Edit objects"), &o.objects);
      ImGui::Checkbox(TR("Edit scenarios"), &o.scenarios);
      if (o.select_locked) o.select_unlocked = true;  // selecting locked cells takes unlocked ones too (Excel ticks both)
    }
    ImGui::EndChild();
    bool close = false;
    auto apply = [&] {
      close = true;
      if (RefuseReadOnlyUi(s)) return;  // read-only mode came on while the dialog was open: closes, nothing changes
      const uint32_t sheet = s.focus_sheet;
      const model::SheetProtection o = p.opts;
      const std::string pw = p.pw;
      s.engine->Call([&](Document& d) { d.ProtectSheet(sheet, o, pw); });
    };
    ImGui::BeginDisabled(!p.contents);  // Excel's OK waits for the contents box
    if (ParentOk()) {
      if (p.pw[0] && s.operator_mode) p.confirm = true;  // a password is typed twice (Confirm Password)
      else apply();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    if (ConfirmPassword(s, p.pw) == 1) apply();
    EndDlg(s, close);
  }
  // Unprotect Sheet / Workbook (password)
  auto unprotect = [&](Dialog dlg, const char* title, bool workbook) {
    if (!BeginDlg(s, dlg, title, 260)) return;
    ImGui::TextUnformatted(TR("Password:"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##pw", p.pw, sizeof(p.pw), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    if (p.wrong) ImGui::TextColored(ImVec4(0.75f, 0.1f, 0.1f, 1), "%s", TR("The password you supplied is not correct."));
    bool close = false;
    if (ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || enter) {
      const uint32_t sheet = s.focus_sheet;
      const std::string pw = p.pw;
      if (RefuseReadOnlyUi(s)) {
        close = true;  // refused by read-only mode, not a wrong password
      } else {
        const bool ok = s.engine->Call([&](Document& d) { return workbook ? d.UnprotectWorkbook(pw) : d.UnprotectSheet(sheet, pw); });
        if (ok) close = true;
        else p.wrong = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  };
  unprotect(Dialog::UnprotectSheet, TR("Unprotect Sheet###yc_dlg_unprotect"), false);
  unprotect(Dialog::UnprotectWorkbook, TR("Unprotect Workbook###yc_dlg_unprotectwb"), true);
  // Protect Workbook
  if (BeginDlg(s, Dialog::ProtectWorkbook, TR("Protect Structure and Windows###yc_dlg_protectwb"), 260)) {
    ImGui::TextUnformatted(TR("Password (optional):"));
    ImGui::SetNextItemWidth(-1);
    Password("##pw", p.pw, sizeof(p.pw));
    ImGui::TextUnformatted(TR("Protect workbook for:"));
    ImGui::Checkbox(TR("Structure"), &p.structure);
    ImGui::Checkbox(TR("Windows"), &p.windows);
    bool close = false;
    auto apply = [&] {
      close = true;
      if (RefuseReadOnlyUi(s)) return;
      const std::string pw = p.pw;
      const bool st = p.structure, wi = p.windows;
      s.engine->Call([&](Document& d) { d.ProtectWorkbook(pw, st, wi); });
    };
    if (ParentOk()) {
      if (p.pw[0] && s.operator_mode) p.confirm = true;
      else apply();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    if (ConfirmPassword(s, p.pw) == 1) apply();
    EndDlg(s, close);
  }
  // Allow Users to Edit Ranges: the list with New... / Modify... / Delete beside it, each range in its own New Range /
  // Modify Range dialog (Title, Refers to cells, Range password), Protect Sheet... (keeps the ranges, then protects)
  if (BeginDlg(s, Dialog::AllowEditRanges, TR("Allow Users to Edit Ranges###yc_dlg_editranges"), 520)) {
    ImGui::TextUnformatted(TR("Ranges unlocked by a password when sheet is protected:"));
    const float list_w = S(370);
    const ImVec2 top = ImGui::GetCursorScreenPos();
    if (ImGui::BeginTable("##ranges", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(list_w, S(150)))) {
      ImGui::TableSetupColumn(TR("Title"));
      ImGui::TableSetupColumn(TR("Refers to cells"));
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < p.ranges.size(); ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(p.ranges[i].title.c_str(), p.sel == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
          p.sel = static_cast<int>(i);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) p.r_edit = p.sel, p.r_open = true;
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(AbsRef(p.ranges[i].rect).c_str());
      }
      ImGui::EndTable();
    }
    const float bottom = ImGui::GetCursorScreenPos().y;
    // the buttons beside the list, as in Excel
    const bool has_sel = p.sel >= 0 && p.sel < static_cast<int>(p.ranges.size());
    const float bx = top.x + list_w + S(12);
    ImGui::SetCursorScreenPos(ImVec2(bx, top.y));
    if (ImGui::Button(TR("New..."), ImVec2(S(100), 0))) p.r_edit = -1, p.r_open = true;
    ImGui::SetCursorScreenPos(ImVec2(bx, ImGui::GetCursorScreenPos().y));
    ImGui::BeginDisabled(!has_sel);
    if (ImGui::Button(TR("Modify..."), ImVec2(S(100), 0))) p.r_edit = p.sel, p.r_open = true;
    ImGui::SetCursorScreenPos(ImVec2(bx, ImGui::GetCursorScreenPos().y));
    if (ImGui::Button(TR("Delete"), ImVec2(S(100), 0)) && has_sel) {
      p.ranges.erase(p.ranges.begin() + p.sel);
      p.sel = p.ranges.empty() ? -1 : std::min(p.sel, static_cast<int>(p.ranges.size()) - 1);
    }
    ImGui::EndDisabled();
    ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(bottom, ImGui::GetCursorScreenPos().y) + S(6)));
    ImGui::TextUnformatted(TR("Specify who may edit the range without a password:"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_sel);
    if (ImGui::Button(TR("Permissions..."), ImVec2(S(120), 0)) && has_sel) {
      p.perm_target = &p.ranges[static_cast<size_t>(p.sel)].users;
      p.perm_title = p.ranges[static_cast<size_t>(p.sel)].title;
      p.perm_open = true;
      p.perm_ctx = 1;
    }
    ImGui::EndDisabled();
    DrawPermissions(s, 1);
    ImGui::Checkbox(TR("Paste permissions information into a new workbook"), &p.paste_perm);
    ImGui::Separator();
    bool close = false;
    auto keep = [&] {
      const uint32_t sheet = s.focus_sheet;
      const auto ranges = p.ranges;
      if (RefuseReadOnlyUi(s)) return false;
      s.engine->Call([&](Document& d) { d.SetAllowEditRanges(sheet, ranges); });
      return true;
    };
    if (ImGui::Button(TR("Protect Sheet..."), ImVec2(S(120), 0))) {
      p.protect_after = keep();
      close = true;
    }
    ImGui::SameLine();
    if (ParentOk()) {
      keep();
      close = true;
      if (p.paste_perm) {
        const model::Sheet* sh = s.FocusSheet();
        PastePermissionsReport(s, sh ? sh->name : std::string(), p.ranges);
        p.paste_perm = false;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    ImGui::SameLine();
    if (ImGui::Button(TR("Apply"), ImVec2(S(80), 0))) keep();
    // New Range / Modify Range
    const char* rtitle = p.r_edit < 0 ? TR("New Range###yc_edit_range") : TR("Modify Range###yc_edit_range");
    if (p.r_open) {
      p.r_open = false;
      if (p.r_edit >= 0 && p.r_edit < static_cast<int>(p.ranges.size())) {
        const auto& er = p.ranges[static_cast<size_t>(p.r_edit)];
        std::snprintf(p.r_title, sizeof(p.r_title), "%s", er.title.c_str());
        std::snprintf(p.r_ref, sizeof(p.r_ref), "%s", AbsRef(er.rect).c_str());
        std::snprintf(p.r_pw, sizeof(p.r_pw), "%s", er.hash ? "********" : "");
        p.r_users = er.users;
      } else {
        p.r_users.clear();
        p.r_edit = -1;
        int n = 1;
        for (;; ++n) {
          bool taken = false;
          for (const auto& er : p.ranges) taken = taken || er.title == std::string(TR("Range")) + std::to_string(n);
          if (!taken) break;
        }
        std::snprintf(p.r_title, sizeof(p.r_title), "%s%d", TR("Range"), n);
        std::snprintf(p.r_ref, sizeof(p.r_ref), "%s", AbsRef(SelRect(s)).c_str());
        p.r_pw[0] = 0;
      }
      ImGui::OpenPopup(rtitle);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool ropen = true;
    if (BeginModal(rtitle, &ropen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
      bool rclose = !ropen || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
      ImGui::TextUnformatted(TR("Title:"));
      ImGui::SetNextItemWidth(S(300));
      ImGui::InputText("##rtitle", p.r_title, sizeof(p.r_title));
      ImGui::TextUnformatted(TR("Refers to cells:"));
      RangeBox(s, "protect.range", p.r_ref, sizeof(p.r_ref), S(300), RangeBox_Equals, TR("Refers to cells:"));
      ImGui::TextUnformatted(TR("Range password:"));
      ImGui::SetNextItemWidth(S(300));
      Password("##rpw", p.r_pw, sizeof(p.r_pw));
      if (ImGui::Button(TR("Permissions..."), ImVec2(S(120), 0))) {
        p.perm_target = &p.r_users;
        p.perm_title = p.r_title;
        p.perm_open = true;
        p.perm_ctx = 2;
      }
      DrawPermissions(s, 2);
      auto store = [&] {
        Rect r{};
        uint32_t rs = 0;
        const char* ref = p.r_ref[0] == '=' ? p.r_ref + 1 : p.r_ref;
        if (!ResolveRangeText(s.snap->wb, s.focus_sheet, ref, &rs, &r) || rs != s.focus_sheet) {
          s.ShowError("YCGrid", TR("The reference isn't valid."));
          return;
        }
        model::AllowEditRange er;
        const bool edit = p.r_edit >= 0 && p.r_edit < static_cast<int>(p.ranges.size());
        if (edit) er = p.ranges[static_cast<size_t>(p.r_edit)];
        er.title = p.r_title[0] ? p.r_title : std::string(TR("Range")) + std::to_string(p.ranges.size() + 1);
        er.rect = r;
        er.users = p.r_users;
        if (!edit || std::strcmp(p.r_pw, "********") != 0) er.hash = model::ProtectionHash(p.r_pw);
        if (edit) {
          p.ranges[static_cast<size_t>(p.r_edit)] = er;
        } else {
          p.ranges.push_back(er);
          p.sel = static_cast<int>(p.ranges.size()) - 1;
        }
        rclose = true;
      };
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        if (p.r_pw[0] && std::strcmp(p.r_pw, "********") != 0) p.confirm = true;
        else store();
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) rclose = true;
      if (ConfirmPassword(s, p.r_pw) == 1) store();
      if (rclose) ImGui::CloseCurrentPopup(), p.nested_closed = ImGui::GetFrameCount();
      ImGui::EndPopup();
    }
    EndDlg(s, close);
    if (close && p.protect_after) {
      p.protect_after = false;
      if (const model::Sheet* sh = s.FocusSheet(); sh && !sh->protection->on) {
        Clear();
        p.opts = *sh->protection;
        p.contents = true;
        s.OpenDialog(Dialog::ProtectSheet);
      }
    }
  }
  // password of an edit range
  if (BeginDlg(s, Dialog::UnlockRange, TR("Unlock Range###yc_dlg_unlockrange"), 300)) {
    const model::Sheet* sh = s.snap ? s.snap->Sheet(p.unlock_sheet) : nullptr;
    const auto* er = sh && p.unlock_range >= 0 && p.unlock_range < static_cast<int>(sh->protection->ranges.size()) ? &sh->protection->ranges[static_cast<size_t>(p.unlock_range)] : nullptr;
    ImGui::TextWrapped(TR("A password is required to change \"%s\". Enter the password:"), er ? er->title.c_str() : "");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##pw", p.pw, sizeof(p.pw), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    if (p.wrong) ImGui::TextColored(ImVec4(0.75f, 0.1f, 0.1f, 1), "%s", TR("The password you supplied is not correct."));
    bool close = false;
    if (ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || enter) {
      const uint32_t sheet = p.unlock_sheet;
      const size_t idx = static_cast<size_t>(std::max(0, p.unlock_range));
      const std::string pw = p.pw;
      if (s.engine->Call([&](Document& d) { return d.UnlockEditRange(sheet, idx, pw); })) {
        close = true;
        s.SetStatus(TR("The range is unlocked; enter the change again."));
      } else {
        p.wrong = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
}

}  // namespace yc::app
