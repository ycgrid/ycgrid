// SPDX-License-Identifier: Apache-2.0
// trust.cpp - the Trust Center for a workbook's Python code: Message Bar, File > Info's Security Warning, Trust Center
// Settings (Trusted Locations, Trusted Documents, Macro Settings, Message Bar), Application.AutomationSecurity (trust.h)
#include "trust.h"

#include "app_state.h"
#include "core/i18n.h"
#include "engine/document.h"
#include "error_log.h"
#include "python_bridge.h"
#include "settings.h"
#include "yc_ui_items.h"

#include "app/shell_integration.h"
#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"

#include <nlohmann/json.hpp>
#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace yc::app::trust {

namespace fs = std::filesystem;
using io::MV;
using nlohmann::json;
using ui::S;

namespace {

struct State {
  bool hidden = false;
  int automation = kAutomationLow;
  bool launch_automation = false;  // the command line's workbook was given by a script
  bool opened_by_automation = false;
  int in_scope = 0;                // AutomationScope depth
  std::wstring decided;            // NormalizeDir of the project the decision is for ("" = none)
  std::wstring dir;                // the same, as given
  std::atomic<bool> disabled{false};  // (python.status reads it on a controller's thread)
  bool bar = false, bar_closed = false;
  std::string reason;
  std::set<std::wstring> created;  // projects YCGrid made in this session (NormalizeDir): trusted until it exits
  // the open workbook's project, enabled with a click while it stays open (Excel's "Enable content for this session":
  // opening it again asks again, unless Enable Content made it a Trusted Document)
  std::wstring enabled_once;
  json mem = json::object();       // hidden instances: trust.json, in memory only
  // Protected View (Excel's): the open file came from the Internet, an unsafe location or an Outlook attachment; it is
  // read-only and nothing of its code runs until Enable Editing
  bool protected_view = false, pv_bar_closed = false;
  std::string pv_reason;  // "internet", "unsafe_location", "outlook"
  std::wstring pv_path;
  bool script_open = false;  // the open workbook was opened by Workbooks.Open / MCP (never in Protected View)
};

State& St() {
  static State s;
  return s;
}

// ---- Trusted Documents: %LOCALAPPDATA%\ycgrid\trust.json ----
fs::path TrustFile() { return fs::path(SettingsPath()).parent_path() / L"trust.json"; }

json LoadStore() {
  if (St().hidden) return St().mem;
  std::ifstream f(TrustFile(), std::ios::binary);
  if (!f) return json::object();
  try {
    json j = json::parse(f, nullptr, true, true);
    if (j.is_object()) return j;
  } catch (const json::exception&) {
    // a broken file trusts nothing (it is written again on the next change)
  }
  return json::object();
}

bool SaveStore(const json& j) {
  if (St().hidden) {
    St().mem = j;
    return true;
  }
  const fs::path path = TrustFile();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  const fs::path tmp = path.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    if (!f) return false;
  }
  return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// a list of records in trust.json: "trusted_documents" (project folders, Enable Content) and "editing_enabled"
// (files that left Protected View with Enable Editing)
std::vector<json> LoadList(const char* key) {
  std::vector<json> out;
  const json j = LoadStore();
  if (j.contains(key) && j[key].is_array()) {
    for (const auto& d : j[key]) {
      if (d.is_object()) out.push_back(d);
    }
  }
  return out;
}

bool SaveList(const char* key, const std::vector<json>& list) {
  json j = LoadStore();
  j[key] = list;
  return SaveStore(j);
}

std::vector<json> LoadDocs() {
  std::vector<json> out;
  for (auto& d : LoadList("trusted_documents")) {
    if (d.contains("folder") && d["folder"].is_string()) out.push_back(std::move(d));
  }
  return out;
}

bool SaveDocs(const std::vector<json>& docs) {
  if (docs.empty()) SaveList("editing_enabled", {});  // Clear: no document is trusted any more, Protected View ones neither
  return SaveList("trusted_documents", docs);
}

// the folder's identity: the same path holding another folder (deleted and replaced, copied from elsewhere) isn't
// the trusted one, as Excel's trust record goes with the file and not only its name
std::string FolderId(const std::wstring& dir) {
  HANDLE h = ::CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  BY_HANDLE_FILE_INFORMATION fi{};
  std::string out;
  if (::GetFileInformationByHandle(h, &fi)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08lX-%08lX%08lX-%08lX%08lX", fi.dwVolumeSerialNumber, fi.nFileIndexHigh, fi.nFileIndexLow,
                  fi.ftCreationTime.dwHighDateTime, fi.ftCreationTime.dwLowDateTime);
    out = buf;
  }
  ::CloseHandle(h);
  return out;
}

std::string Now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

bool IsTrustedDocument(const std::wstring& key, const std::wstring& dir) {
  const std::string k = ToUtf8(key), id = FolderId(dir);
  for (const auto& d : LoadDocs()) {
    if (d.value("folder", std::string()) == k && !id.empty() && d.value("id", std::string()) == id) return true;
  }
  return false;
}

void AddDocument(const std::wstring& dir, const std::wstring& workbook) {
  const std::wstring key = NormalizeDir(dir);
  const std::string k = ToUtf8(key);
  std::vector<json> docs = LoadDocs();
  std::erase_if(docs, [&](const json& d) { return d.value("folder", std::string()) == k; });
  docs.push_back({{"folder", k}, {"id", FolderId(dir)}, {"workbook", ToUtf8(workbook)}, {"trusted", Now()}});
  SaveDocs(docs);
}

bool IsNetwork(const std::wstring& path) {
  if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return !(path.size() >= 4 && path[2] == L'?' && path[3] == L'\\');
  if (path.size() >= 2 && path[1] == L':') {
    const wchar_t root[4] = {path[0], L':', L'\\', 0};
    return ::GetDriveTypeW(root) == DRIVE_REMOTE;
  }
  return false;
}

std::vector<Settings::TrustedLocation> Locations(const Settings& c) {
  if (c.trusted_locations_set) return c.trusted_locations;
  // YCGrid's own folders, as Excel trusts its own (Templates, XLSTART): the examples next to ycgrid.exe and the
  // projects folder where Alt+F11 creates projects
  return {{"%YCGRID_DIR%\\examples", true, "YCGrid examples", ""},
          {"%YCGRID_PROJECTS%", true, "YCGrid projects (Alt+F11)", ""}};
}

struct Decision {
  bool run = false, bar = false;
  std::string reason;
};

Decision TrustCenterDecision(const std::wstring& key, const std::wstring& dir) {
  const Settings& c = Config();
  State& st = St();
  if (st.created.count(key)) return {true, false, "created"};
  if (!st.enabled_once.empty() && st.enabled_once == key) return {true, false, "session"};
  if (!c.disable_trusted_locations) {
    for (const auto& l : Locations(c)) {
      const std::wstring loc = ExpandLocation(l.path);
      if (loc.empty() || (IsNetwork(loc) && !c.allow_network_locations)) continue;
      if (InLocation(key, loc, l.subfolders)) return {true, false, "trusted_location"};
    }
  }
  if (!c.disable_trusted_documents && (c.allow_network_documents || !IsNetwork(dir)) && IsTrustedDocument(key, dir)) return {true, false, "trusted_document"};
  if (c.macro_setting == 2) return {true, false, "enable_all"};
  if (c.macro_setting == 1) return {false, false, "disabled_without_notification"};
  return {false, c.message_bar, "disabled"};
}

void Enable(AppState& s, bool always) {
  State& st = St();
  if (!st.disabled) return;
  st.enabled_once = st.decided;
  if (always && !Config().disable_trusted_documents) AddDocument(st.dir, s.file_path);
  st.disabled = st.bar = false;
  st.reason = always ? "trusted_document" : "session";
  engine::Document::python_blocked = false;
  s.SetStatus(TR("Python code enabled"));
  StartPythonForWorkbook(s);
}

std::wstring PickFolder(HWND owner, const std::wstring& start) {
  std::wstring answer;
  if (NoFileDialog(owner, "Browse (Trusted Location)", &answer)) return answer;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) return {};
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  if (!start.empty()) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
      dlg->SetFolder(item);
      item->Release();
    }
  }
  std::wstring out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = p;
        ::CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// ---- the Trust Center dialog's working copy (OK keeps it, Cancel drops it) ----
struct CenterDlg {
  bool loaded = false;
  int page = 0;  // 0 Trusted Locations, 1 Trusted Documents, 2 Macro Settings, 3 Message Bar
  Settings c;
  bool locations_changed = false;
  int sel = -1;
  // the Trusted Location dialog (Add new location / Modify)
  bool edit_open = false, edit_request = false;
  int edit_index = -1;  // -1 = new
  char path[1024] = {}, desc[512] = {};
  bool subfolders = false;
  bool clear_request = false;
  std::string error;
};
CenterDlg g_dlg;

struct OptionsDlg {
  int choice = 0;  // 0 Help protect me (recommended), 1 Enable content for this session
};
OptionsDlg g_opt;

void DrawLocationEditor(AppState& s) {
  CenterDlg& d = g_dlg;
  const char* title = TR("Trusted Location");
  if (d.edit_request) {
    ImGui::OpenPopup(title);
    d.edit_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(560), 0), ImGuiCond_Appearing);
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    d.edit_open = false;
    return;
  }
  ImGui::PushTextWrapPos(S(540));
  ImGui::TextUnformatted(TR("Warning: This location will be treated as a trusted source for opening files. If you change or add a location, make sure that the new location is secure."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::TextUnformatted(TR("Path:"));
  ImGui::SetNextItemWidth(S(440));
  ImGui::InputText("##tl_path", d.path, sizeof d.path);
  ImGui::SameLine();
  if (ImGui::Button(TR("Browse..."))) {
    const std::wstring got = PickFolder(s.hwnd, FromUtf8(d.path));
    if (!got.empty()) std::snprintf(d.path, sizeof d.path, "%s", ToUtf8(got).c_str());
  }
  ImGui::Checkbox(TR("Subfolders of this location are also trusted"), &d.subfolders);
  ImGui::TextUnformatted(TR("Description:"));
  ImGui::SetNextItemWidth(S(540));
  ImGui::InputText("##tl_desc", d.desc, sizeof d.desc);
  if (d.edit_index >= 0 && static_cast<size_t>(d.edit_index) < d.c.trusted_locations.size()) {
    ImGui::TextDisabled(TR("Date and Time Created: %s"), d.c.trusted_locations[static_cast<size_t>(d.edit_index)].modified.c_str());
  }
  if (!d.error.empty()) {
    ImGui::PushTextWrapPos(S(540));
    ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.2f, 1), "%s", d.error.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Separator();
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(216));
  if (DlgOkButton(TR("OK"), ImVec2(S(96), 0))) {
    std::string p = d.path;
    while (!p.empty() && (p.back() == ' ' || p.back() == '\\' || p.back() == '/')) p.pop_back();
    while (!p.empty() && p.front() == ' ') p.erase(p.begin());
    const std::wstring expanded = ExpandLocation(p);
    std::error_code ec;
    if (p.empty()) {
      d.error = TR("Type the path of a folder.");
    } else if (IsNetwork(expanded) && !d.c.allow_network_locations) {
      d.error = TR("The remote or network path you have selected is not allowed as a trusted location by your current security settings. To choose this path, select the 'Allow Trusted Locations on my network' check box.");
    } else if (!fs::is_directory(expanded, ec)) {
      d.error = TR("The path you have entered cannot be used as a trusted location: the folder doesn't exist.");
    } else {
      Settings::TrustedLocation t{p, d.subfolders, d.desc, Now()};
      if (d.edit_index >= 0 && static_cast<size_t>(d.edit_index) < d.c.trusted_locations.size()) {
        d.c.trusted_locations[static_cast<size_t>(d.edit_index)] = t;
        d.sel = d.edit_index;
      } else {
        d.c.trusted_locations.push_back(t);
        d.sel = static_cast<int>(d.c.trusted_locations.size()) - 1;
      }
      d.locations_changed = true;
      close = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(96), 0))) close = true;
  if (close) {
    d.edit_open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DrawClearConfirm(AppState& s) {
  CenterDlg& d = g_dlg;
  const char* title = TR("Clear Trusted Documents");
  if (d.clear_request) {
    ImGui::OpenPopup(title);
    d.clear_request = false;
  }
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted(TR("Are you sure you want to clear all Trusted Documents so they are no longer trusted?"));
  ImGui::Separator();
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (DlgOkButton(TR("Yes"), ImVec2(S(96), 0))) {
    SaveDocs({});
    // workbooks enabled by a click (this session) stay enabled until they close, as in Excel
    s.SetStatus(TR("Trusted Documents cleared"));
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("No"), ImVec2(S(96), 0))) close = true;
  if (close) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void DrawLocationsPage(AppState& s) {
  CenterDlg& d = g_dlg;
  ImGui::TextUnformatted(TR("Trusted Locations"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", TR("Warning: All these locations are treated as trusted sources for opening files. If you change or add a location, make sure that the new location is secure."));
  ImGui::PopTextWrapPos();
  if (!d.locations_changed && !d.c.trusted_locations_set) d.c.trusted_locations = Locations(d.c);
  auto& locs = d.c.trusted_locations;
  if (ImGui::BeginTable("##tl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                        ImVec2(0, S(170)))) {
    ImGui::TableSetupColumn(TR("Path"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(TR("Description"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(TR("Date Modified"), ImGuiTableColumnFlags_WidthFixed, S(110));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < locs.size(); ++i) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(i));
      const std::string shown = ToUtf8(ExpandLocation(locs[i].path)) + "\\";
      if (ImGui::Selectable(shown.c_str(), d.sel == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
        d.sel = static_cast<int>(i);
        if (ImGui::IsMouseDoubleClicked(0)) {
          d.edit_index = d.sel;
          std::snprintf(d.path, sizeof d.path, "%s", locs[i].path.c_str());
          std::snprintf(d.desc, sizeof d.desc, "%s", locs[i].description.c_str());
          d.subfolders = locs[i].subfolders;
          d.error.clear();
          d.edit_open = d.edit_request = true;
        }
      }
      ImGui::PopID();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(locs[i].description.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(locs[i].modified.c_str());
    }
    ImGui::EndTable();
  }
  const bool has = d.sel >= 0 && static_cast<size_t>(d.sel) < locs.size();
  if (has) {
    const auto& l = locs[static_cast<size_t>(d.sel)];
    ImGui::Text(TR("Path: %s"), ToUtf8(ExpandLocation(l.path)).c_str());
    ImGui::Text(TR("Description: %s"), l.description.c_str());
    ImGui::Text(TR("Sub Folders: %s"), l.subfolders ? TR("Allowed") : TR("Disallowed"));
  } else {
    ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeightWithSpacing() * 3));
  }
  if (ImGui::Button(TR("Add new location..."))) {
    d.edit_index = -1;
    d.path[0] = d.desc[0] = 0;
    d.subfolders = false;
    d.error.clear();
    d.edit_open = d.edit_request = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!has);
  if (ImGui::Button(TR("Remove"))) {
    locs.erase(locs.begin() + d.sel);
    d.sel = -1;
    d.locations_changed = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Modify..."))) {
    const auto& l = locs[static_cast<size_t>(d.sel)];
    d.edit_index = d.sel;
    std::snprintf(d.path, sizeof d.path, "%s", l.path.c_str());
    std::snprintf(d.desc, sizeof d.desc, "%s", l.description.c_str());
    d.subfolders = l.subfolders;
    d.error.clear();
    d.edit_open = d.edit_request = true;
  }
  ImGui::EndDisabled();
  ImGui::Spacing();
  ImGui::Checkbox(TR("Allow Trusted Locations on my network (not recommended)"), &d.c.allow_network_locations);
  ImGui::Checkbox(TR("Disable all Trusted Locations"), &d.c.disable_trusted_locations);
  DrawLocationEditor(s);
}

void DrawDocumentsPage(AppState& s) {
  CenterDlg& d = g_dlg;
  ImGui::TextUnformatted(TR("Trusted Documents"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", TR("Trusted Documents open without any security prompts for Python code. Clicking Enable Content on a workbook's Message Bar makes it a Trusted Document. Before trusting a document, make sure you trust its source."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::Checkbox(TR("Allow documents on a network to be trusted"), &d.c.allow_network_documents);
  ImGui::Checkbox(TR("Disable Trusted Documents"), &d.c.disable_trusted_documents);
  ImGui::Spacing();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Clear all Trusted Documents so that they are no longer trusted"));
  ImGui::SameLine();
  if (ImGui::Button(TR("Clear"))) d.clear_request = true;
  ImGui::TextDisabled(TR("Trusted Documents: %zu"), LoadDocs().size());
  DrawClearConfirm(s);
}

void DrawMacroPage() {
  CenterDlg& d = g_dlg;
  ImGui::TextUnformatted(TR("Macro Settings"));
  ImGui::TextDisabled("%s", TR("For Python code in workbooks' projects (main.py, sheets, modules) not in a trusted location:"));
  ImGui::Spacing();
  if (ui::RadioButton(TR("Disable Python code without notification"), d.c.macro_setting == 1)) d.c.macro_setting = 1;
  if (ui::RadioButton(TR("Disable Python code with notification"), d.c.macro_setting == 0)) d.c.macro_setting = 0;
  if (ui::RadioButton(TR("Enable Python code (not recommended; potentially dangerous code can run)"), d.c.macro_setting == 2)) d.c.macro_setting = 2;
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", TR("Workbooks opened by scripts, yc.launch and the MCP tools follow Application.AutomationSecurity instead (Low by default: their code runs)."));
  ImGui::PopTextWrapPos();
}

void DrawProtectedViewPage() {
  CenterDlg& d = g_dlg;
  ImGui::TextUnformatted(TR("Protected View"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", TR("Protected View opens potentially dangerous files read-only, without running their Python code."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::Checkbox(TR("Enable Protected View for files originating from the Internet"), &d.c.pv_internet);
  ImGui::Checkbox(TR("Enable Protected View for files located in potentially unsafe locations"), &d.c.pv_unsafe_locations);
  ImGui::Checkbox(TR("Enable Protected View for Outlook attachments"), &d.c.pv_outlook);
}

void DrawMessageBarPage() {
  CenterDlg& d = g_dlg;
  ImGui::TextUnformatted(TR("Message Bar Settings"));
  ImGui::TextDisabled("%s", TR("Showing the Message Bar"));
  ImGui::Spacing();
  if (ui::RadioButton(TR("Show the Message Bar when content such as Python code has been blocked"), d.c.message_bar)) d.c.message_bar = true;
  if (ui::RadioButton(TR("Never show information about blocked content"), !d.c.message_bar)) d.c.message_bar = false;
}

void DrawCenterDialog(AppState& s) {
  CenterDlg& d = g_dlg;
  if (s.dialog != Dialog::TrustCenter) {
    d.loaded = false;
    return;
  }
  const char* title = TR("Trust Center");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  if (!d.loaded) {
    d.c = Config();
    d.locations_changed = false;
    d.sel = -1;
    d.edit_open = d.edit_request = d.clear_request = false;
    d.loaded = true;
  }
  ImGui::SetNextWindowSize(ImVec2(S(820), S(470)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) return;
  const float foot = ImGui::GetFrameHeightWithSpacing() + S(10);
  ImGui::BeginChild("##tc_pages", ImVec2(S(190), -foot), ImGuiChildFlags_Borders);
  const char* pages[] = {TR("Trusted Locations"), TR("Trusted Documents"), TR("Macro Settings"), TR("Protected View"), TR("Message Bar")};
  for (int i = 0; i < 5; ++i) {
    ImGui::PushID(i);
    if (ImGui::Selectable(pages[i], d.page == i)) d.page = i;
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##tc_page", ImVec2(0, -foot), ImGuiChildFlags_Borders);
  switch (d.page) {
    case 0: DrawLocationsPage(s); break;
    case 1: DrawDocumentsPage(s); break;
    case 2: DrawMacroPage(); break;
    case 3: DrawProtectedViewPage(); break;
    default: DrawMessageBarPage(); break;
  }
  ImGui::EndChild();
  const bool child_open = d.edit_open || ImGui::IsPopupOpen(TR("Clear Trusted Documents"));
  bool close = !open || (!child_open && ImGui::IsKeyPressed(ImGuiKey_Escape));
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - S(216));
  const bool ok = child_open ? ImGui::Button(TR("OK"), ImVec2(S(96), 0)) : DlgOkButton(TR("OK"), ImVec2(S(96), 0));
  if (ok) {
    Settings& c = Config();
    c.macro_setting = d.c.macro_setting;
    c.message_bar = d.c.message_bar;
    c.allow_network_locations = d.c.allow_network_locations;
    c.disable_trusted_locations = d.c.disable_trusted_locations;
    c.allow_network_documents = d.c.allow_network_documents;
    c.disable_trusted_documents = d.c.disable_trusted_documents;
    c.pv_internet = d.c.pv_internet;
    c.pv_unsafe_locations = d.c.pv_unsafe_locations;
    c.pv_outlook = d.c.pv_outlook;
    if (d.locations_changed) {
      c.trusted_locations = d.c.trusted_locations;
      c.trusted_locations_set = true;
    }
    if (!SaveSettings()) s.SetStatus(TR("Couldn't save the settings: ") + ToUtf8(SettingsPath()), 8.0);
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(96), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

void DrawSecurityOptions(AppState& s) {
  if (s.dialog != Dialog::SecurityOptions) return;
  const char* title = TR("Security Options");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
    g_opt.choice = 0;
  }
  ImGui::SetNextWindowSize(ImVec2(S(560), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::PushFont(ui::F().ui_bold, 0.0f);
  ImGui::TextUnformatted(TR("Security Alert - Python code"));
  ImGui::PopFont();
  ImGui::PushTextWrapPos(S(540));
  if (St().disabled) {
    ImGui::TextUnformatted(TR("Python code has been disabled. This code might contain viruses or other security hazards. Do not enable this content unless you trust the source of this file."));
  } else {
    ImGui::TextUnformatted(TR("This workbook's Python code isn't disabled."));
  }
  ImGui::Text(TR("File Path: %s"), ToUtf8(s.file_path).c_str());
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::BeginDisabled(!St().disabled);
  if (ui::RadioButton(TR("Help protect me from unknown content (recommended)"), g_opt.choice == 0)) g_opt.choice = 0;
  if (ui::RadioButton(TR("Enable content for this session"), g_opt.choice == 1)) g_opt.choice = 1;
  ImGui::EndDisabled();
  ImGui::Separator();
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (DlgOkButton(TR("OK"), ImVec2(S(96), 0))) {
    if (g_opt.choice == 1) Enable(s, false);
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(96), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

MV LocationsMV(const Settings& c) {
  MV a = MV::Array();
  for (const auto& l : Locations(c)) {
    MV x = MV::Map();
    x.Set("path", MV::Str(ToUtf8(ExpandLocation(l.path))));
    x.Set("stored", MV::Str(l.path));
    x.Set("subfolders", MV::Bool(l.subfolders));
    x.Set("description", MV::Str(l.description));
    x.Set("modified", MV::Str(l.modified));
    a.Push(std::move(x));
  }
  return a;
}

}  // namespace

// ---- helpers ----
std::wstring NormalizeDir(const std::wstring& path) {
  if (path.empty()) return {};
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(path), ec);
  if (ec) p = fs::path(path);
  std::wstring out = p.lexically_normal().wstring();
  for (auto& ch : out) {
    if (ch == L'/') ch = L'\\';
  }
  while (out.size() > 3 && out.back() == L'\\') out.pop_back();
  for (auto& ch : out) ch = static_cast<wchar_t>(::towlower(ch));
  return out;
}

bool InLocation(const std::wstring& dir, const std::wstring& location, bool subfolders) {
  const std::wstring d = NormalizeDir(dir), l = NormalizeDir(location);
  if (d.empty() || l.empty()) return false;
  if (d == l) return true;
  if (!subfolders) return false;
  const std::wstring prefix = l.back() == L'\\' ? l : l + L"\\";
  return d.size() > prefix.size() && d.compare(0, prefix.size(), prefix) == 0;
}

std::wstring ExpandLocation(const std::string& stored) {
  std::wstring s = FromUtf8(stored);
  auto replace = [&](const std::wstring& token, const std::wstring& value) {
    for (size_t at = 0;;) {
      std::wstring up = s;
      for (auto& ch : up) ch = static_cast<wchar_t>(::towupper(ch));
      at = up.find(token, at);
      if (at == std::wstring::npos) break;
      s.replace(at, token.size(), value);
      at += value.size();
    }
  };
  replace(L"%YCGRID_DIR%", app::shell::ExeDir());
  replace(L"%YCGRID_PROJECTS%", ProjectsDir());
  if (s.find(L'%') != std::wstring::npos) {
    wchar_t buf[2048];
    const DWORD n = ::ExpandEnvironmentStringsW(s.c_str(), buf, 2048);
    if (n > 0 && n <= 2048) s = buf;
  }
  while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
  return s;
}

// ---- state ----
void Init(bool hidden, bool launched_by_automation, int automation_security) {
  State& st = St();
  st.hidden = hidden;
  st.launch_automation = launched_by_automation || hidden;
  st.automation = automation_security >= 1 && automation_security <= 3 ? automation_security : kAutomationLow;
  if (hidden) {
    // a hidden instance (tests, agents) starts from the defaults: the person's own Trust Center settings and Trusted
    // Documents neither change what it does nor are changed by it
    Settings& c = Config();
    const Settings def;
    c.macro_setting = def.macro_setting;
    c.message_bar = def.message_bar;
    c.trusted_locations.clear();
    c.trusted_locations_set = false;
    c.allow_network_locations = def.allow_network_locations;
    c.disable_trusted_locations = def.disable_trusted_locations;
    c.allow_network_documents = def.allow_network_documents;
    c.disable_trusted_documents = def.disable_trusted_documents;
    c.pv_internet = def.pv_internet;
    c.pv_unsafe_locations = def.pv_unsafe_locations;
    c.pv_outlook = def.pv_outlook;
  }
}

void StartupDone() { St().launch_automation = false; }
int AutomationSecurity() { return St().automation; }
void SetAutomationSecurity(int v) {
  if (v >= 1 && v <= 3) St().automation = v;
}

void NoteOpen(bool automation) {
  State& st = St();
  st.script_open = automation || st.in_scope > 0;
  st.opened_by_automation = automation || st.launch_automation || st.in_scope > 0;
  st.decided.clear();
  st.enabled_once.clear();  // a workbook enabled for the session asks again when it opens again (Excel)
}

AutomationScope::AutomationScope() { ++St().in_scope; }
AutomationScope::~AutomationScope() { --St().in_scope; }

std::wstring ChildArgs() {
  const State& st = St();
  if (!st.hidden && st.in_scope == 0) return {};
  return L" --automation-security " + std::to_wstring(st.automation);
}

bool AllowProject(AppState& s, const std::wstring& dir) {
  State& st = St();
  if (st.protected_view) {
    // Protected View: nothing runs until Enable Editing; the macro security decision comes after it
    st.decided.clear();
    engine::Document::python_blocked = true;
    if (s.engine) s.engine->Post([](engine::Document& doc) { doc.RecalcUdfCells(); });
    return false;
  }
  const std::wstring key = NormalizeDir(dir);
  if (!st.decided.empty() && key == st.decided) return !st.disabled;
  Decision d;
  if (st.hidden || st.opened_by_automation || st.launch_automation || st.in_scope > 0) {
    if (st.automation == kAutomationLow) d = {true, false, "automation"};
    else if (st.automation == kAutomationForceDisable) d = {false, false, "automation_force_disable"};
    else d = TrustCenterDecision(key, dir);
  } else {
    d = TrustCenterDecision(key, dir);
  }
  st.decided = key;
  st.dir = dir;
  st.disabled = !d.run;
  st.bar = d.bar;
  st.bar_closed = false;
  st.reason = d.reason;
  engine::Document::python_blocked = st.disabled.load();
  // formulas calculated while the workbook loaded, before this decision (=PY asked for a host): calculated again, now
  // #BLOCKED! / #NAME?
  if (st.disabled && s.engine) s.engine->Post([](engine::Document& doc) { doc.RecalcUdfCells(); });
  if (st.disabled && s.python) s.python->LogSystem(std::string(TR("[YCGrid] Python code has been disabled (Trust Center): ")) + ShownPath(dir));
  return d.run;
}

void NoProject() {
  State& st = St();
  st.decided.clear();
  st.disabled = st.bar = false;
  st.reason.clear();
  engine::Document::python_blocked = st.protected_view;  // (=PY doesn't run in Protected View either)
}

bool CodeDisabled() { return St().disabled || St().protected_view; }

std::string ProtectedViewReason(const std::wstring& path) {
  if (path.empty()) return {};
  const Settings& c = Config();
  std::error_code ec;
  const std::wstring dir = fs::path(path).parent_path().wstring();
  // Outlook's attachment folders (classic and new Outlook), then the browser caches (Excel's "potentially unsafe
  // locations": Temporary Internet Files), then the Mark of the Web
  std::wstring local;
  {
    wchar_t buf[MAX_PATH + 1];
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH + 1);
    if (n > 0 && n <= MAX_PATH) local = buf;
  }
  if (!local.empty()) {
    const fs::path l(local);
    for (const fs::path& o : {l / L"Microsoft" / L"Windows" / L"INetCache" / L"Content.Outlook", l / L"Microsoft" / L"Windows" / L"Temporary Internet Files" / L"Content.Outlook",
                              l / L"Microsoft" / L"Olk"}) {
      if (InLocation(dir, o.wstring(), true)) return c.pv_outlook ? "outlook" : std::string();
    }
    for (const fs::path& u : {l / L"Microsoft" / L"Windows" / L"INetCache", l / L"Microsoft" / L"Windows" / L"Temporary Internet Files"}) {
      if (InLocation(dir, u.wstring(), true)) return c.pv_unsafe_locations ? "unsafe_location" : std::string();
    }
  }
  if (c.pv_internet && ZoneOf(path) >= 3) return "internet";
  return {};
}

int ZoneOf(const std::wstring& path) {
  // the Mark of the Web: the Zone.Identifier stream browsers and mail programs add ("[ZoneTransfer]\r\nZoneId=3")
  HANDLE h = ::CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0;
  char buf[4096];
  DWORD got = 0;
  std::string text;
  if (::ReadFile(h, buf, sizeof buf, &got, nullptr)) text.assign(buf, got);
  ::CloseHandle(h);
  for (size_t at = 0; (at = text.find("ZoneId", at)) != std::string::npos; ++at) {
    size_t eq = text.find('=', at);
    if (eq == std::string::npos) break;
    ++eq;
    while (eq < text.size() && text[eq] == ' ') ++eq;
    if (eq < text.size() && text[eq] >= '0' && text[eq] <= '9') return text[eq] - '0';
  }
  return 0;
}

namespace {
// a file's identity for "editing enabled": its path, size and last write (saving it again writes a new file without
// the Mark of the Web anyway)
json FileStamp(const std::wstring& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  const auto t = fs::last_write_time(path, ec).time_since_epoch().count();
  return {{"path", ToUtf8(NormalizeDir(path))}, {"size", static_cast<int64_t>(ec ? 0 : size)}, {"time", static_cast<int64_t>(t)}};
}

bool EditingEnabledBefore(const std::wstring& path) {
  if (Config().disable_trusted_documents) return false;
  const json me = FileStamp(path);
  for (const auto& d : LoadList("editing_enabled")) {
    if (d.value("path", std::string()) == me["path"].get<std::string>() && d.value("size", int64_t{-1}) == me["size"].get<int64_t>() &&
        d.value("time", int64_t{-1}) == me["time"].get<int64_t>())
      return true;
  }
  return false;
}
}  // namespace

void OpenedFile(AppState& s, const std::wstring& path) {
  State& st = St();
  st.protected_view = st.pv_bar_closed = false;
  st.pv_reason.clear();
  st.pv_path = path;
  // Excel: a script's Workbooks.Open never opens in Protected View (ProtectedViewWindows.Open does); neither does a
  // workbook a script's YCGrid opens unless its AutomationSecurity is ByUI (a test of what a person sees)
  const bool automation = st.hidden || st.launch_automation || st.opened_by_automation;
  if (!path.empty() && !st.script_open && (!automation || st.automation == kAutomationByUI)) {
    std::string why = ProtectedViewReason(path);
    if (!why.empty()) {
      // trusted locations and files whose editing was enabled before open normally (Excel)
      const std::wstring dir = fs::path(path).parent_path().wstring();
      if (!Config().disable_trusted_locations) {
        for (const auto& l : Locations(Config())) {
          const std::wstring loc = ExpandLocation(l.path);
          if (!loc.empty() && (!IsNetwork(loc) || Config().allow_network_locations) && InLocation(dir, loc, l.subfolders)) why.clear();
        }
      }
      if (!why.empty() && EditingEnabledBefore(path)) why.clear();
    }
    st.protected_view = !why.empty();
    st.pv_reason = why;
  }
  engine::Document::python_blocked = st.protected_view || st.disabled;
  if (s.engine) s.engine->Post([pv = st.protected_view](engine::Document& d) { d.protected_view = pv; });
  UpdateWindowTitle(s);
}

bool InProtectedView() { return St().protected_view; }

void EnableEditing(AppState& s) {
  State& st = St();
  if (!st.protected_view) return;
  st.protected_view = false;
  if (!Config().disable_trusted_documents && !st.pv_path.empty()) {
    std::vector<json> list = LoadList("editing_enabled");
    const json me = FileStamp(st.pv_path);
    std::erase_if(list, [&](const json& d) { return d.value("path", std::string()) == me["path"].get<std::string>(); });
    list.push_back(me);
    SaveList("editing_enabled", list);
  }
  if (s.engine) s.engine->Post([](engine::Document& d) { d.protected_view = false; });
  engine::Document::python_blocked = false;
  s.SetStatus(TR("Editing enabled"));
  UpdateWindowTitle(s);
  // then the workbook's Python code is decided as for any workbook (the Security Warning bar may follow)
  StartPythonForWorkbook(s);
  if (s.engine) s.engine->Post([](engine::Document& doc) { doc.RecalcUdfCells(); });
}

void ProtectedViewBlocked(AppState& s) {
  St().pv_bar_closed = false;  // the bar comes back, as Excel's flashes
  s.SetStatus(TR("Protected View: this file can't be changed. Click Enable Editing to edit it."), 5.0);
}

void EnableContent(AppState& s, bool always) { Enable(s, always); }

void TrustCreated(const std::wstring& dir) {
  State& st = St();
  const std::wstring key = NormalizeDir(dir);
  st.created.insert(key);
  if (!Config().disable_trusted_documents) AddDocument(dir, L"");
  if (st.decided == key) {
    st.decided.clear();
    st.disabled = st.bar = false;
    engine::Document::python_blocked = false;
  }
}

bool MacroBlocked(AppState& s, const std::string& macro) {
  if (!St().disabled) return false;
  s.ShowMessage("YCGrid", TR("Cannot run the macro '") + macro + TR("'. The macro may not be available in this workbook or all macros may be disabled."));
  return true;
}

bool ImmediateBlocked(AppState& s) {
  if (!St().disabled) return false;
  if (s.python) {
    s.python->LogError(TR("The Python code in this project is disabled. Click Enable Content on the Message Bar or in File > Info to run it."));
  }
  return true;
}

// ---- drawing ----
namespace {
// Excel's yellow Message Bar: icon, bold heading, the message, one button, the close X at the right end
// returns 1 when the button was clicked, 2 when the X was
int DrawBar(const ImRect& r, float h, const char* head, const char* msg, const char* button, const char* close_key) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(r.Min, ImVec2(r.Max.x, r.Min.y + h), IM_COL32(0xFF, 0xF4, 0xCE, 255));
  dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + h - 1), ImVec2(r.Max.x, r.Min.y + h), IM_COL32(0xE0, 0xC0, 0x60, 255));
  const float cy = r.Min.y + h * 0.5f;
  float x = r.Min.x + S(12);
  const float isz = S(18);
  dl->AddText(ui::F().icons, isz, ImVec2(x, cy - isz * 0.5f), IM_COL32(0xA0, 0x60, 0x00, 255), ui::icon::Warning);
  x += isz + S(8);
  const ImU32 text = IM_COL32(0x20, 0x20, 0x20, 255);
  ImFont* bold = ui::F().ui_bold ? ui::F().ui_bold : ImGui::GetFont();
  const float fs = ImGui::GetFontSize();
  dl->AddText(bold, fs, ImVec2(x, cy - fs * 0.5f), text, head);
  x += bold->CalcTextSizeA(fs, FLT_MAX, 0, head).x + S(14);
  dl->AddText(ImVec2(x, cy - fs * 0.5f), text, msg);
  x += ImGui::CalcTextSize(msg).x + S(14);
  int result = 0;
  ImGui::SetCursorScreenPos(ImVec2(x, cy - ImGui::GetFrameHeight() * 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0xFF, 0xFF, 0xFF, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0x8A, 0x88, 0x86, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  if (ImGui::Button(button)) result = 1;
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  // the close X: hides the bar; nothing else changes (File > Info still offers the button)
  const ImRect xr(ImVec2(r.Max.x - S(34), cy - S(12)), ImVec2(r.Max.x - S(10), cy + S(12)));
  ImGui::SetCursorScreenPos(xr.Min);
  const std::string close_id = std::string(TR("Close Message Bar")) + close_key;
  if (ImGui::InvisibleButton(close_id.c_str(), xr.GetSize())) result = 2;
  if (ImGui::IsItemHovered()) {
    dl->AddRectFilled(xr.Min, xr.Max, IM_COL32(0, 0, 0, 0x18));
    ImGui::SetTooltip("%s", TR("Close Message Bar"));
  }
  ui::NoteUiItem(close_id.c_str(), xr);
  const float xs = S(14);
  dl->AddText(ui::F().icons, xs, ImVec2(xr.GetCenter().x - xs * 0.5f, xr.GetCenter().y - xs * 0.5f), text, ui::icon::Dismiss);
  return result;
}

const char* PvMessage(const std::string& reason) {
  if (reason == "outlook") return TR("Be careful—email attachments can contain viruses. Unless you need to edit, it's safer to stay in Protected View.");
  if (reason == "unsafe_location") return TR("This file was opened from a potentially unsafe location. Unless you need to edit, it's safer to stay in Protected View.");
  return TR("Be careful—files from the Internet can contain viruses. Unless you need to edit, it's safer to stay in Protected View.");
}
}  // namespace

float DrawMessageBar(AppState& s, const ImRect& r, bool measure_only) {
  State& st = St();
  const bool pv = st.protected_view && !st.pv_bar_closed;
  const bool sec = !st.protected_view && st.disabled && st.bar && !st.bar_closed;
  if (!pv && !sec) return 0;
  const float h = S(36);
  if (measure_only) return h;
  if (pv) {
    const int got = DrawBar(r, h, TR("PROTECTED VIEW"), PvMessage(st.pv_reason), TR("Enable Editing"), "##pvbar_close");
    if (got == 1) EnableEditing(s);
    if (got == 2) st.pv_bar_closed = true;
  } else {
    const int got = DrawBar(r, h, TR("SECURITY WARNING"), TR("Python code has been disabled."), TR("Enable Content"), "##secbar_close");
    if (got == 1) Enable(s, true);
    if (got == 2) st.bar_closed = true;
  }
  return h;
}

void DrawInfoSection(AppState& s) {
  if (St().protected_view) {
    ImGui::PushFont(ui::F().ui_bold, 0.0f);
    ImGui::TextUnformatted(TR("Protected View"));
    ImGui::PopFont();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(640));
    ImGui::TextDisabled("%s", St().pv_reason == "outlook"           ? TR("This file is an email attachment and might be unsafe. It is open read-only, and its Python code doesn't run.")
                              : St().pv_reason == "unsafe_location" ? TR("This file was opened from a potentially unsafe location. It is open read-only, and its Python code doesn't run.")
                                                                    : TR("This file originated from an Internet location and might be unsafe. It is open read-only, and its Python code doesn't run."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(TR("Enable Editing"))) {
      ui::ribbon::St().backstage = false;
      EnableEditing(s);
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Trust Center Settings..."))) {
      ui::ribbon::St().backstage = false;
      s.OpenDialog(Dialog::TrustCenter);
    }
    ImGui::Dummy(ImVec2(1, S(12)));
    return;
  }
  if (!St().disabled) return;
  ImGui::PushFont(ui::F().ui_bold, 0.0f);
  ImGui::TextUnformatted(TR("Security Warning"));
  ImGui::PopFont();
  ImGui::TextDisabled("%s", TR("Python code has been disabled. Enable it only when you trust the source of this workbook."));
  if (ImGui::Button(TR("Enable Content"))) ImGui::OpenPopup("##enable_content_menu");
  // (drawn right after its button: BeginPopupEx on a closed popup clears SetNextWindowPos)
  if (ImGui::BeginPopup("##enable_content_menu")) {
    if (ImGui::Selectable(TR("Enable All Content"))) {
      ImGui::CloseCurrentPopup();
      ui::ribbon::St().backstage = false;
      Enable(s, true);
    }
    ImGui::TextDisabled("%s", TR("Always enable this workbook's Python code"));
    ImGui::Separator();
    if (ImGui::Selectable(TR("Advanced Options"))) {
      ImGui::CloseCurrentPopup();
      ui::ribbon::St().backstage = false;
      s.OpenDialog(Dialog::SecurityOptions);
    }
    ImGui::TextDisabled("%s", TR("Enable it for this session only"));
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Trust Center Settings..."))) {
    ui::ribbon::St().backstage = false;
    s.OpenDialog(Dialog::TrustCenter);
  }
  ImGui::Dummy(ImVec2(1, S(12)));
}

void DrawOptionsSection(AppState& s) {
  ImGui::SeparatorText(TR("Trust Center"));
  ImGui::TextDisabled("%s", TR("The Trust Center decides when a workbook's Python code runs: trusted locations, trusted documents and the macro settings."));
  if (ImGui::Button(TR("Trust Center Settings..."))) s.OpenDialog(Dialog::TrustCenter);
}

void DrawDialogs(AppState& s) {
  DrawCenterDialog(s);
  DrawSecurityOptions(s);
}

void RegisterCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("TrustCenterSettings", TR("Trust Center Settings..."),
             TR("Trusted Locations, Trusted Documents, Macro Settings and the Message Bar: when a workbook's Python code runs.")))
      .execute = [&s](ui::Command&) { s.OpenDialog(Dialog::TrustCenter); };
  R.Add(make("EnableContent", TR("Enable Content"), TR("Run this workbook's disabled Python code and make it a Trusted Document.")))
      .execute = [&s](ui::Command&) { Enable(s, true); };
  R.Add(make("EnableEditing", TR("Enable Editing"), TR("Leave Protected View: the workbook can be edited, and its Python code is decided by the Trust Center.")))
      .execute = [&s](ui::Command&) { EnableEditing(s); };
  R.Add(make("SecurityOptions", TR("Advanced Options"), TR("Enable this workbook's disabled Python code for this session only.")))
      .execute = [&s](ui::Command&) { s.OpenDialog(Dialog::SecurityOptions); };
}

MV StateMV() {
  const State& st = St();
  MV o = MV::Map();
  o.Set("code", MV::Str(st.decided.empty() ? "none" : st.disabled ? "disabled" : "enabled"));
  o.Set("disabled", MV::Bool(st.disabled));
  o.Set("bar", MV::Bool(st.disabled && st.bar && !st.bar_closed));
  o.Set("reason", MV::Str(st.reason));
  o.Set("automation_security", MV::Int(st.automation));
  o.Set("folder", MV::Str(ToUtf8(st.dir)));
  o.Set("protected_view", MV::Bool(st.protected_view));
  o.Set("protected_view_reason", MV::Str(st.pv_reason));
  o.Set("protected_view_bar", MV::Bool(st.protected_view && !st.pv_bar_closed));
  return o;
}

MV CenterRequest(AppState& s, const MV& p, std::string* err) {
  Settings& c = Config();
  bool changed = false;
  if (p.Has("macro_setting")) {
    const int64_t v = p.GetInt("macro_setting");
    if (v < 0 || v > 2) {
      *err = "MacroSettings: use ycMacroDisableWithNotification, ycMacroDisableWithoutNotification or ycMacroEnableAll";
      return {};
    }
    c.macro_setting = static_cast<int>(v);
    changed = true;
  }
  auto flag = [&](const char* k, bool* v) {
    if (p.Has(k)) {
      *v = p.GetBool(k);
      changed = true;
    }
  };
  flag("message_bar", &c.message_bar);
  flag("allow_network_locations", &c.allow_network_locations);
  flag("disable_trusted_locations", &c.disable_trusted_locations);
  flag("allow_network_documents", &c.allow_network_documents);
  flag("disable_trusted_documents", &c.disable_trusted_documents);
  if (p.Has("add_location")) {
    const std::string path = p.GetStr("add_location");
    const std::wstring w = ExpandLocation(path);
    std::error_code ec;
    if (IsNetwork(w) && !c.allow_network_locations) {
      *err = "The remote or network path is not allowed as a trusted location: set AllowNetworkLocations = True first";
      return {};
    }
    if (!fs::is_directory(w, ec)) {
      *err = "The folder doesn't exist: " + path;
      return {};
    }
    auto locs = Locations(c);
    std::erase_if(locs, [&](const Settings::TrustedLocation& l) { return NormalizeDir(ExpandLocation(l.path)) == NormalizeDir(w); });
    locs.push_back({path, p.GetBool("subfolders", false), p.GetStr("description"), Now()});
    c.trusted_locations = std::move(locs);
    c.trusted_locations_set = changed = true;
  }
  if (p.Has("remove_location")) {
    const std::wstring w = NormalizeDir(ExpandLocation(p.GetStr("remove_location")));
    auto locs = Locations(c);
    const size_t n = locs.size();
    std::erase_if(locs, [&](const Settings::TrustedLocation& l) { return NormalizeDir(ExpandLocation(l.path)) == w; });
    if (locs.size() == n) {
      *err = "Not a trusted location: " + p.GetStr("remove_location");
      return {};
    }
    c.trusted_locations = std::move(locs);
    c.trusted_locations_set = changed = true;
  }
  flag("pv_internet", &c.pv_internet);
  flag("pv_unsafe_locations", &c.pv_unsafe_locations);
  flag("pv_outlook", &c.pv_outlook);
  if (p.GetBool("clear_documents")) SaveDocs({});
  if (changed && !SaveSettings()) s.SetStatus(TR("Couldn't save the settings: ") + ToUtf8(SettingsPath()), 8.0);
  MV o = MV::Map();
  o.Set("macro_setting", MV::Int(c.macro_setting));
  o.Set("message_bar", MV::Bool(c.message_bar));
  o.Set("allow_network_locations", MV::Bool(c.allow_network_locations));
  o.Set("disable_trusted_locations", MV::Bool(c.disable_trusted_locations));
  o.Set("allow_network_documents", MV::Bool(c.allow_network_documents));
  o.Set("disable_trusted_documents", MV::Bool(c.disable_trusted_documents));
  o.Set("locations", LocationsMV(c));
  MV docs = MV::Array();
  for (const auto& d : LoadDocs()) docs.Push(MV::Str(d.value("folder", std::string())));
  o.Set("documents", std::move(docs));
  o.Set("pv_internet", MV::Bool(c.pv_internet));
  o.Set("pv_unsafe_locations", MV::Bool(c.pv_unsafe_locations));
  o.Set("pv_outlook", MV::Bool(c.pv_outlook));
  return o;
}

}  // namespace yc::app::trust
