// SPDX-License-Identifier: Apache-2.0
// file_ops.cpp - New, Open, Save, Save As, Export CSV, recent files, Backstage content (PLAN §12, §15)
#include "app_state.h"
#include "error_log.h"
#include "python_bridge.h"
#include "trust.h"
#include "settings.h"
#include "app/shell_integration.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "io/csv.h"
#include "io/xlsx.h"
#include "io/ycg.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"
#include "titlebar/titlebar.h"
#include "core/parallel.h"
#include "icons/yc_icons.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace yc::app {

using engine::Document;

namespace {

// comparison key: absolute path, backslashes, lowercase (the same file never appears twice in the list)
std::wstring RecentKey(const std::wstring& p) {
  std::error_code ec;
  std::wstring k = std::filesystem::absolute(std::filesystem::path(p), ec).lexically_normal().wstring();
  if (ec) k = p;
  for (wchar_t& ch : k) {
    if (ch == L'/') ch = L'\\';
    ch = static_cast<wchar_t>(::towlower(ch));
  }
  return k;
}

std::wstring LocalAppDir() {
  wchar_t* local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) dir = std::wstring(local) + L"\\ycgrid";
  if (local) ::CoTaskMemFree(local);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::wstring Lower(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(::towlower(c));
  return s;
}

bool IsCsv(const std::wstring& path) {
  const std::wstring ext = Lower(std::filesystem::path(path).extension().wstring());
  return ext == L".csv" || ext == L".txt" || ext == L".tsv";
}

// Save and Save As write the file on a worker thread (PLAN §15.1): the screen and live data go on meanwhile. One save
// at a time; the frame loop finishes it, and anything that replaces the workbook or exits waits for it first.
struct BackgroundSave {
  YcgSave plan;
  std::thread th;
  std::atomic<bool> done{false};
  bool ok = false;
  std::string err;
};
std::unique_ptr<BackgroundSave> g_bg_save;

void SaveRecent() {
  nlohmann::json j = nlohmann::json::array();
  for (const auto& p : RecentFiles()) j.push_back(ToUtf8(p));
  std::ofstream f(std::filesystem::path(LocalAppDir() + L"\\recent.json"), std::ios::binary | std::ios::trunc);
  f << j.dump(2);
}

void AddRecent(const std::wstring& path) {
  auto& r = RecentFiles();
  const std::wstring key = RecentKey(path);
  r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& x) { return RecentKey(x) == key; }), r.end());
  r.insert(r.begin(), path);
  if (r.size() > 20) r.resize(20);
  if (UserStateSaved()) SaveRecent();  // a hidden instance's (test) files stay in its own list, not the user's file
}

// File > Open / Home: the workbooks pinned to the top of Recent (recent_pinned.json beside recent.json)
std::vector<std::wstring>& PinnedFiles() {
  static std::vector<std::wstring> list = [] {
    std::vector<std::wstring> out;
    std::ifstream f(std::filesystem::path(LocalAppDir() + L"\\recent_pinned.json"), std::ios::binary);
    if (f) {
      const auto j = nlohmann::json::parse(f, nullptr, false);
      if (j.is_array()) {
        for (const auto& e : j) {
          if (e.is_string()) out.push_back(FromUtf8(e.get<std::string>()));
        }
      }
    }
    return out;
  }();
  return list;
}

void SavePinned() {
  if (!UserStateSaved()) return;
  nlohmann::json j = nlohmann::json::array();
  for (const auto& p : PinnedFiles()) j.push_back(ToUtf8(p));
  std::ofstream f(std::filesystem::path(LocalAppDir() + L"\\recent_pinned.json"), std::ios::binary | std::ios::trunc);
  f << j.dump(2);
}

bool IsPinned(const std::wstring& path) {
  const std::wstring key = RecentKey(path);
  return std::any_of(PinnedFiles().begin(), PinnedFiles().end(), [&](const std::wstring& x) { return RecentKey(x) == key; });
}

void SetPinned(const std::wstring& path, bool pin) {
  auto& p = PinnedFiles();
  const std::wstring key = RecentKey(path);
  p.erase(std::remove_if(p.begin(), p.end(), [&](const std::wstring& x) { return RecentKey(x) == key; }), p.end());
  if (pin) p.insert(p.begin(), path);
  SavePinned();
}

void RemoveRecent(const std::wstring& path) {
  auto& r = RecentFiles();
  const std::wstring key = RecentKey(path);
  r.erase(std::remove_if(r.begin(), r.end(), [&](const std::wstring& x) { return RecentKey(x) == key; }), r.end());
  SetPinned(path, false);
  if (UserStateSaved()) SaveRecent();
}

bool ContainsNoCaseW(const std::wstring& hay, const std::string& needle) {
  if (needle.empty()) return true;
  return Lower(hay).find(Lower(FromUtf8(needle))) != std::wstring::npos;
}

// Windows file dialog (IFileDialog)
std::wstring FileDialog(HWND owner, bool save, const std::wstring& initial_name, int* filter_index) {
  if (std::wstring a; NoFileDialog(owner, save ? TR("Save As") : TR("Open"), &a)) return a;
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::wstring result;
  IFileDialog* dlg = nullptr;
  if (SUCCEEDED(::CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   save ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
    const COMDLG_FILTERSPEC open_types[] = {{TRW(L"All Workbooks"), L"*.ycg;*.xlsx;*.xlsm;*.csv;*.txt;*.tsv"},
                                            {TRW(L"YCGrid Workbook (*.ycg)"), L"*.ycg"},
                                            {TRW(L"Excel Workbook (*.xlsx; *.xlsm)"), L"*.xlsx;*.xlsm"},
                                            {L"CSV (*.csv)", L"*.csv;*.txt;*.tsv"}};
    const COMDLG_FILTERSPEC save_types[] = {{TRW(L"YCGrid Workbook (*.ycg)"), L"*.ycg"}, {TRW(L"CSV UTF-8 (Comma delimited) (*.csv)"), L"*.csv"},
                                            {TRW(L"Excel Workbook (*.xlsx)"), L"*.xlsx"}};
    if (save) {
      dlg->SetFileTypes(3, save_types);
      dlg->SetFileTypeIndex(filter_index && *filter_index >= 2 && *filter_index <= 3 ? static_cast<UINT>(*filter_index) : 1);
      dlg->SetDefaultExtension(L"ycg");
      if (!initial_name.empty()) dlg->SetFileName(initial_name.c_str());
    } else {
      dlg->SetFileTypes(4, open_types);
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (SUCCEEDED(dlg->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item)) && item) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
          result = p;
          ::CoTaskMemFree(p);
        }
        item->Release();
      }
      UINT idx = 1;
      dlg->GetFileTypeIndex(&idx);
      if (filter_index) *filter_index = static_cast<int>(idx);
    }
    dlg->Release();
  }
  if (SUCCEEDED(init)) ::CoUninitialize();
  return result;
}

void RestoreSavedGroup(AppState& s);

void ResetViews(AppState& s) {
  s.wins.clear();
  s.edit.active = false;
  s.marquee = false;
  s.has_clip = false;
  s.format_painter = false;
  s.containers.clear();
  s.engine->Sync();
  s.snap = s.engine->Current();
  s.active_sheet = s.focus_sheet = s.snap->wb.sheets[static_cast<size_t>(std::clamp<int32_t>(s.snap->wb.active_sheet, 0, static_cast<int32_t>(s.snap->wb.sheets.size()) - 1))]->id;
  RestoreSavedGroup(s);
}

// the sheets saved grouped (their tabs selected) are grouped again, as Excel opens them (after the layout has chosen
// the active sheet)
void RestoreSavedGroup(AppState& s) {
  std::vector<uint32_t> group;
  for (const auto& sh : s.snap->wb.sheets) {
    if (sh->tab_selected && sh->visible) group.push_back(sh->id);
  }
  s.group_sheets.clear();
  if (group.size() > 1 && std::find(group.begin(), group.end(), s.active_sheet) != group.end()) SetSheetGroup(s, group, s.active_sheet);
}

}  // namespace

std::vector<std::wstring>& RecentFiles() {
  static std::vector<std::wstring> list = [] {
    std::vector<std::wstring> out;
    std::ifstream f(std::filesystem::path(LocalAppDir() + L"\\recent.json"), std::ios::binary);
    if (f) {
      const auto j = nlohmann::json::parse(f, nullptr, false);
      if (j.is_array()) {
        for (const auto& e : j) {
          if (!e.is_string()) continue;
          const std::wstring p = FromUtf8(e.get<std::string>());
          const std::wstring key = RecentKey(p);
          if (std::none_of(out.begin(), out.end(), [&](const std::wstring& x) { return RecentKey(x) == key; })) out.push_back(p);
        }
      }
    }
    return out;
  }();
  return list;
}

// restart YCGrid (after switching the language): ask to save first, start a new instance with the same file, close this window
void RestartApp(AppState& s) {
  // a hidden instance belongs to the script that started it: a restart would leave the script and open a visible
  // YCGrid on the user's desktop
  if (s.headless) return;
  if (!ConfirmDiscard(s)) return;
  std::wstring args = L"--no-handoff";
  if (!s.file_path.empty()) args += L" \"" + s.file_path + L"\"";
  const std::wstring exe = app::shell::ExePath();
  // lesson-ok: shell-open a hidden instance returned above (s.headless); the restart needs the shell's own answer
  if (reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) <= 32) {
    s.ShowError("YCGrid", TR("Couldn't restart. Please close YCGrid and open it again."));
    return;
  }
  ::DestroyWindow(s.hwnd);
}

void UpdateWindowTitle(AppState& s) {
  std::string name = s.file_path.empty() ? TR("Book1") : ToUtf8(std::filesystem::path(s.file_path).filename().wstring());
  if (s.snap && s.snap->wb.read_only) name += TR(" [Read-Only]");
  if (s.group_sheets.size() > 1) name += TR(" [Group]");  // grouped sheets, as Excel's title shows
  if (trust::InProtectedView()) name += TR(" [Protected View]");  // as Excel's title bar
  if (s.win_caption_set) name = s.win_caption;  // Window.Caption replaces the workbook's part
  const std::string app = s.app_caption.empty() ? std::string("YCGrid") : s.app_caption;
  ui::titlebar::St().title = name.empty() ? app : name + " - " + app;
  ::SetWindowTextW(s.hwnd, FromUtf8(ui::titlebar::St().title).c_str());
  if (s.python) s.python->WriteInstanceFile();  // other windows and Python find the workbook by its file
}

bool ConfirmDiscard(AppState& s) {
  FinishBackgroundSave(s, true);  // a save still writing finishes first (it may be the answer)
  if (!s.snap || !s.snap->wb.dirty) return true;
  if (s.headless) return true;  // a hidden instance (tests, agents) has nobody to ask
  const std::string name = s.file_path.empty() ? TR("Book1") : ToUtf8(std::filesystem::path(s.file_path).filename().wstring());
  const std::wstring msg = FromUtf8(TR("Do you want to save changes to \"") + name + TR("\"?"));
  // topmost and in front: floating figure and sheet windows must not hide it (YCGrid would look frozen)
  ::ShowWindow(s.hwnd, ::IsIconic(s.hwnd) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(s.hwnd);
  const int r = ::MessageBoxW(s.hwnd, msg.c_str(), L"YCGrid", MB_YESNOCANCEL | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
  if (r == IDCANCEL) return false;
  if (r == IDYES) return SaveWorkbook(s, false);
  return true;
}

// a blank workbook as File > Options > General says: its default font and size, its number of sheets (Excel)
model::Workbook NewUserWorkbook() {
  model::Workbook wb = model::Workbook::NewDefault(TR("Sheet"));
  const Settings& c = Config();
  if (!c.standard_font.empty() || c.standard_font_size > 0) {
    auto& st = wb.styles.mut();
    if (!c.standard_font.empty()) st.default_font = c.standard_font;
    if (c.standard_font_size > 0) st.default_font_size = static_cast<float>(std::clamp(c.standard_font_size, 1.0, 409.0));
  }
  for (int k = 2; k <= std::clamp(c.sheets_in_new_workbook, 1, 255); ++k) {
    model::Sheet sh;
    sh.id = wb.next_sheet_id++;
    sh.name = std::string(TR("Sheet")) + std::to_string(k);
    sh.code_name = "Sheet" + std::to_string(k);
    wb.sheets.emplace_back(std::make_shared<model::Sheet>(std::move(sh)));
  }
  return wb;
}

bool NewWorkbook(AppState& s) {
  if (!ConfirmDiscard(s)) return false;
  s.engine->Replace(NewUserWorkbook());
  s.show_sheet_tabs = s.show_hscroll = s.show_vscroll = true;  // a new workbook's own display options
  s.circ_warned = s.circ_check = false;
  s.file_path.clear();
  s.opened_from.clear();
  SafetyOnOpen(s, false);
  ResetViews(s);
  UpdateWindowTitle(s);
  trust::OpenedFile(s, L"");  // a new workbook is never in Protected View
  StartPythonForWorkbook(s);
  return true;
}

bool OpenWorkbook(AppState& s, const std::wstring& path, const std::string& password, bool prompt, std::string* error_out) {
  FinishBackgroundSave(s, true);  // its path and "saved" state belong to the workbook it saved
  model::Workbook wb;
  std::string error;
  std::vector<std::string> warnings;
  io::XlsxSkipped skipped;  // what the Excel workbook holds that isn't carried over: told below
  bool ok;
  const bool xlsx = io::IsXlsxPath(path);
  if (IsCsv(path) || xlsx) {
    try {
      ok = xlsx ? io::ImportXlsx(path, &wb, &error, &warnings, &skipped) : io::ImportCsv(path, &wb, &error);
    } catch (const std::exception& e) {  // a crafted file (sizes that don't fit in memory): an error, not a crash
      ok = false;
      error = TR("The file can't be read: ") + std::string(e.what());
    }
  } else {
    io::LoadResult r;
    try {
      r = io::LoadYcg(path, password);
    } catch (const std::exception& e) {  // a crafted file (sizes that don't fit in memory): an error, not a crash
      r.ok = false;
      r.error = TR("The file can't be read: ") + std::string(e.what());
    }
    if ((r.needs_password || r.wrong_password) && prompt) {
      // encrypted: ask for the password, then open again (DrawFileDialogs)
      s.password_path = path;
      s.password_wrong = r.wrong_password;
      // started with this file: until the password is given the blank workbook isn't that file (Save mustn't overwrite it)
      if (s.file_path == path && s.engine && s.engine->Current()->wb.path.empty()) {
        s.file_path.clear();
        UpdateWindowTitle(s);
      }
      s.OpenDialog(Dialog::PasswordOpen);
      return false;
    }
    ok = r.ok;
    error = r.error;
    warnings = r.warnings;
    wb = std::move(r.wb);
  }
  if (error_out) *error_out = error;
  if (!ok && !prompt) return false;
  if (!ok) {
    s.ShowError(TR("Can't open the file"), ToUtf8(path) + "\n\n" + error);
    return false;
  }
  // CSV and Excel files open as a new, untitled YCGrid workbook (Save keeps a .ycg; Save As can write xlsx again)
  const bool csv = IsCsv(path) || xlsx;
  wb.path = csv ? std::string() : ToUtf8(path);
  wb.dirty = false;
  const std::string layout = wb.layout_json;
  const bool read_only = wb.read_only;
  const bool start_read_only = wb.safety->start_read_only;
  const std::string reason = wb.read_only_reason;
  // a file saved with iterative calculation settings sets them for the session, as in Excel: File > Options shows
  // them (settings.json keeps the user's own until OK there)
  if (wb.calc_iterate >= 0) {
    Settings& c = Config();
    c.iterative = wb.calc_iterate > 0;
    c.max_iterations = wb.calc_max_iterations;
    c.max_change = wb.calc_max_change;
  }
  if (wb.calc_mode >= 0) {
    Config().auto_calc = wb.calc_mode != 1;
    Config().auto_data_tables = wb.calc_mode != 2;
  }
  // Excel warns about circular references in a workbook it opens (once); Python opening a file stays quiet
  s.circ_warned = false;
  if (prompt) ArmCircularWarning(s), s.circ_before = 0;
  else s.circ_check = false;
  s.engine->Replace(std::move(wb));
  s.file_path = csv ? std::wstring() : path;
  s.opened_from = path;
  SafetyOnOpen(s, start_read_only);
  ResetViews(s);
  // layout: window positions, floating sheets, containers, docking tree, scroll and selection of each sheet (PLAN §10.5)
  if (!layout.empty()) ApplyLayoutJson(s, layout, true);
  RestoreSavedGroup(s);
  UpdateWindowTitle(s);
  AddRecent(path);
  trust::NoteOpen(!prompt);  // opened by Python / MCP: Application.AutomationSecurity decides whether its code runs
  trust::OpenedFile(s, path);  // Protected View for a file from the Internet (before its Python code is decided)
  StartPythonForWorkbook(s);
  if (read_only) {
    s.ShowMessage("YCGrid", reason);
  } else if (xlsx && (skipped.Any() || !warnings.empty())) {
    // Excel files are an exchange format (ADR 0004): the person is told what the file held that didn't come along,
    // every time, so nothing goes missing silently. A person's open gets the message box; Python's stays quiet
    // (as Excel's alerts do for scripts): the status bar and the Errors window carry it.
    const std::vector<std::string> lines = skipped.Lines();
    std::string text = TR("The values, formulas and formatting of the Excel workbook were opened as a new YCGrid workbook. YCGrid reads Excel files as an exchange format and doesn't carry these over:");
    for (const std::string& l : lines) text += "\n    " + l;
    for (const std::string& w : warnings) text += "\n    " + w;
    text += std::string("\n\n") + TR("Save as a .ycg workbook to keep what you add in YCGrid.");
    if (prompt) {
      s.ShowMessage("YCGrid", text);
    } else {
      ReportError("YCGrid", TR("Opened an Excel workbook"), text, false);
      std::string brief;
      for (const std::string& l : lines) brief += (brief.empty() ? "" : ", ") + l;
      for (const std::string& w : warnings) brief += (brief.empty() ? "" : ", ") + w;
      s.SetStatus(TR("Opened an Excel workbook; not carried over: ") + brief, 10.0);
    }
  } else if (!warnings.empty()) {
    s.SetStatus(TR("Opening reported ") + std::to_string(warnings.size()) + TR(" warnings: ") + warnings[0], 10.0);
  } else {
    s.SetStatus(TR("Opened ") + ToUtf8(std::filesystem::path(path).filename().wstring()));
  }
  return true;
}

bool OpenPasswordOk(const std::wstring& path, const std::string& password) {
  if (IsCsv(path) || io::IsXlsxPath(path)) return true;
  try {
    const io::LoadResult r = io::LoadYcg(path, password);
    return !r.needs_password && !r.wrong_password;
  } catch (const std::exception&) {
    return true;  // not a password problem: opening it reports the error
  }
}

bool OpenWorkbookDialog(AppState& s) {
  const std::wstring path = FileDialog(s.hwnd, false, L"", nullptr);
  if (path.empty()) return false;
  // like Excel: another workbook opens in a window of its own; an empty Book1 is replaced
  if (WorkbookInUse(s)) {
    std::error_code ec;
    if (!s.file_path.empty() && std::filesystem::equivalent(s.file_path, path, ec)) return true;  // already open here
    return OpenInNewWindow(path);
  }
  if (!ConfirmDiscard(s)) return false;
  return OpenWorkbook(s, path);
}

namespace {

// A person's Save As to an Excel workbook: what this workbook holds that the copy won't (ADR 0004) is listed first,
// as Excel's Compatibility Checker does before a save to a lesser format; OK writes the copy, Cancel keeps everything
// as it was. Other formats, and a workbook with nothing to lose, save at once.
bool SaveAsExcelChecked(AppState& s, const std::wstring& path, int filter, bool background) {
  if (!(io::IsXlsxPath(path) || filter == 3) || !s.snap) return SaveWorkbookTo(s, path, filter, background);
  const std::vector<std::string> lines = io::XlsxExportSkipped(s.snap->wb).Lines();
  const bool project = s.python && !s.python->NoProject();
  if (lines.empty() && !project) return SaveWorkbookTo(s, path, filter, background);
  std::string text = TR("These parts of the workbook aren't written to an Excel workbook (YCGrid writes values, formulas and formatting; everything stays in the .ycg):");
  for (const std::string& l : lines) text += "\n    " + l;
  if (project) text += std::string("\n    ") + TR("The Python project (it stays beside the .ycg)");
  text += std::string("\n\n") + TR("Save the copy anyway?");
  AskYesNo(s, TR("Compatibility Checker"), text, [&s, path, filter, background] { SaveWorkbookTo(s, path, filter, background); });
  return false;  // not saved yet: OK saves
}

}  // namespace

bool SaveWorkbook(AppState& s, bool save_as, bool background) {
  if (!s.engine || !s.snap) return false;
  if (s.edit.active) CommitEdit(s, false, 0);
  if (s.snap->wb.read_only && !save_as) {
    s.ShowMessage("YCGrid", TR("This file was opened read-only (it needs a newer version of YCGrid). Use Save As."));
    return false;
  }
  std::wstring path = s.file_path;
  int filter = 1;
  if (path.empty() || save_as) {
    const std::wstring initial = s.file_path.empty() ? TRW(L"Book1.ycg") : std::filesystem::path(s.file_path).filename().wstring();
    path = FileDialog(s.hwnd, true, initial, &filter);
    if (path.empty()) return false;
  }
  return SaveAsExcelChecked(s, path, filter, background);
}

std::wstring NewProjectWorkbookPath() {
  // like Excel's Book1, Book2...: the first name whose project folder doesn't exist yet
  const std::filesystem::path root = ProjectsDir();
  std::wstring base = std::filesystem::path(TRW(L"Book1.ycg")).stem().wstring();
  while (!base.empty() && iswdigit(base.back())) base.pop_back();
  std::error_code ec;
  for (int n = 1; n < 10000; ++n) {
    const std::wstring name = base + std::to_wstring(n);
    const std::filesystem::path dir = root / name;
    if (!std::filesystem::exists(dir, ec)) return (dir / (name + L".ycg")).wstring();
  }
  return (root / (base + L".ycg")).wstring();
}

bool IsYcgSavePath(const std::wstring& path) { return !io::IsXlsxPath(path) && !IsCsv(path); }

void FinishBackgroundSave(AppState& s, bool wait) {
  if (!g_bg_save) return;
  if (!wait && !g_bg_save->done.load()) {
    ui::RequestRedrawIn(0.05);  // look again soon, even when nothing else happens
    return;
  }
  std::unique_ptr<BackgroundSave> b = std::move(g_bg_save);
  b->th.join();
  FinishYcgSave(s, b->plan, b->ok, b->err);
}

bool SaveWorkbookTo(AppState& s, std::wstring path, int filter, bool background) {
  if (!s.engine || !s.snap) return false;
  FinishBackgroundSave(s, true);  // saves happen in order
  if (s.edit.active) CommitEdit(s, false, 0);
  // File > Options > Formulas > Recalculate workbook before saving (Application.CalculateBeforeSave): in manual
  // calculation the file gets calculated values, as Excel saves them
  if (Config().calculate_before_save) {
    s.engine->Call([](Document& d) {
      if (!d.auto_calc) d.CalculateNow();
    });
  }
  std::error_code dir_ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), dir_ec);
  if (io::IsXlsxPath(path) || filter == 3) {
    std::string err;
    if (!io::IsXlsxPath(path)) path += L".xlsx";
    s.engine->Sync();
    s.snap = s.engine->Current();
    if (!io::ExportXlsx(s.snap->wb, path, &err)) {
      s.ShowError("YCGrid", TR("Can't save the Excel workbook: ") + err);
      FireAfterSave(s, false);
      return false;
    }
    FireAfterSave(s, true);
    // what the copy doesn't hold is said every time (ADR 0004); a person's Save As was asked first (SaveAsExcelChecked)
    std::string left_out;
    for (const std::string& l : io::XlsxExportSkipped(s.snap->wb).Lines()) left_out += (left_out.empty() ? "" : ", ") + l;
    if (s.python && !s.python->NoProject()) left_out += (left_out.empty() ? "" : ", ") + std::string(TR("the Python project"));
    if (left_out.empty()) s.SetStatus(TR("Saved a copy as an Excel workbook"), 8.0);
    else s.SetStatus(TR("Saved a copy as an Excel workbook; not written (they stay in the .ycg): ") + left_out, 8.0);
    return true;
  }
  if (IsCsv(path) || filter == 2) {
    std::string err;
    s.engine->Sync();  // include the edit just committed and Python's latest writes
    s.snap = s.engine->Current();
    if (!io::ExportCsv(*s.snap, s.focus_sheet, path, &err)) {
      s.ShowError("YCGrid", TR("Can't save CSV: ") + err);
      FireAfterSave(s, false);
      return false;
    }
    FireAfterSave(s, true);
    s.SetStatus(TR("Saved the current sheet as CSV (values only, no formulas or formatting)"), 8.0);
    return true;
  }
  if (background) {
    auto b = std::make_unique<BackgroundSave>();
    b->plan = PrepareYcgSave(s, path);
    BackgroundSave* job = b.get();
    b->th = std::thread([job] {
      job->ok = WriteYcgSave(job->plan, &job->err);
      job->done.store(true);
    });
    g_bg_save = std::move(b);
    s.SetStatus(TR("Saving ") + ToUtf8(std::filesystem::path(g_bg_save->plan.path).filename().wstring()) + "...");
    ui::RequestRedrawIn(0.05);
    return true;
  }
  std::string err;
  const YcgSave plan = PrepareYcgSave(s, path);
  const bool ok = WriteYcgSave(plan, &err);
  return FinishYcgSave(s, plan, ok, err);
}

YcgSave PrepareYcgSave(AppState& s, std::wstring path) {
  if (s.edit.active) CommitEdit(s, false, 0);
  std::error_code dir_ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), dir_ec);
  if (Lower(std::filesystem::path(path).extension().wstring()) != L".ycg") path += L".ycg";
  // keep a backup before an old-format file is first saved in the new format (PLAN §15.5)
  const auto& wb = s.snap->wb;
  if (!s.file_path.empty() && path == s.file_path && wb.file_format_major && wb.file_format_major < io::kFormatMajor) {
    std::error_code ec;
    std::filesystem::copy_file(path, path + L".v" + std::to_wstring(wb.file_format_major) + L".bak", std::filesystem::copy_options::overwrite_existing, ec);
  }
  YcgSave plan;
  plan.path = path;
  plan.layout_json = CaptureLayoutJson(s);
  // saving uses the (immutable) snapshot. Clear the "unsaved" flag on the engine thread first, after everything
  // already queued (Python's writes, the edit just committed), then take the snapshot: a change made after this
  // point sets the flag again by itself, so closing still asks about it
  // the selected sheets' tabs are saved, so the group comes back when the file is opened (Excel)
  const std::vector<uint32_t> selected = s.group_sheets.size() > 1 ? s.group_sheets : std::vector<uint32_t>{s.active_sheet};
  s.engine->Call([selected](Document& d) {
    StampDocProperties(d);  // File > Info's Created / Last Modified / Last Modified By
    d.SetTabsSelected(selected);
    d.wb_mut().dirty = false;
  });
  s.engine->Sync();
  plan.snap = s.engine->Current();
  return plan;
}

bool WriteYcgSave(const YcgSave& plan, std::string* err) {
  io::SaveOptions opt;
  opt.layout_json = plan.layout_json;
  opt.app_version = YC_VERSION_STRING;
  return plan.snap && io::SaveYcg(plan.snap->wb, plan.path, opt, err);
}

bool FinishYcgSave(AppState& s, const YcgSave& plan, bool ok, const std::string& err) {
  const std::wstring& path = plan.path;
  if (!ok) {
    s.engine->Post([](Document& d) { d.wb_mut().dirty = true; });
    s.ShowError(TR("Can't save"), ToUtf8(path) + "\n\n" + err);
    FireAfterSave(s, false);  // Workbook_AfterSave(Success:=False)
    return false;
  }
  const std::string upath = ToUtf8(path);
  s.engine->Post([upath, layout = plan.layout_json](Document& d) {
    d.wb_mut().path = upath;
    d.wb_mut().layout_json = layout;
    d.wb_mut().file_format_major = io::kFormatMajor;
    d.wb_mut().file_format_minor = io::kFormatMinor;
  });
  const bool moved = s.file_path != path;
  s.file_path = path;
  UpdateWindowTitle(s);
  AddRecent(path);
  s.SetStatus(TR("Saved ") + ToUtf8(std::filesystem::path(path).filename().wstring()));
  FireAfterSave(s, true);  // before a move to another folder stops this project's host
  if (moved) StartPythonForWorkbook(s);  // saved to another folder: switch to the Python project there
  return true;
}

bool ExportCsvDialog(AppState& s) {
  int filter = 2;
  std::wstring name = s.file_path.empty() ? TRW(L"Book1.csv") : std::filesystem::path(s.file_path).stem().wstring() + L".csv";
  const std::wstring path = FileDialog(s.hwnd, true, name, &filter);
  if (path.empty()) return false;
  std::string err;
  if (!io::ExportCsv(*s.snap, s.focus_sheet, path, &err)) {
    s.ShowError("YCGrid", TR("Export failed: ") + err);
    return false;
  }
  s.SetStatus(TR("Exported ") + ToUtf8(path));
  return true;
}

// the password prompt of an encrypted workbook and File -> Info -> Encrypt with Password
void DrawFileDialogs(AppState& s) {
  static char pw[256] = {}, pw2[256] = {};
  auto begin = [&](Dialog d, const char* title) {
    if (s.dialog != d) return false;
    if (s.dialog_open_request) {
      ImGui::OpenPopup(title);
      s.dialog_open_request = false;
      pw[0] = pw2[0] = 0;
    }
    // Excel's password dialogs are narrow: a fixed width, the sentences wrap in it and the boxes fill it; only the
    // height follows the contents (a fitted width took the longest sentence's and stretched the boxes with it)
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::S(300), 0), ImVec2(ui::S(300), FLT_MAX));
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
  };
  auto end = [&](bool close) {
    if (close) {
      ImGui::CloseCurrentPopup();
      s.dialog = Dialog::None;
      SecureZeroMemory(pw, sizeof pw);
      SecureZeroMemory(pw2, sizeof pw2);
    }
    ImGui::EndPopup();
  };
  if (begin(Dialog::PasswordOpen, TR("Password###yc_dlg_pwopen"))) {
    ImGui::TextWrapped(TR("'%s' is protected."), ToUtf8(std::filesystem::path(s.password_path).filename().wstring()).c_str());
    if (s.password_wrong) ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.2f, 1), "%s", TR("The password you supplied is not correct."));
    ImGui::TextUnformatted(TR("Password:"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputText("##pw", pw, sizeof pw, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    bool close = false;
    if (ImGui::Button(TR("OK"), ImVec2(ui::S(80), 0)) || enter) {
      const std::wstring path = s.password_path;
      const std::string p = pw;
      end(true);
      OpenWorkbook(s, path, p);  // a wrong password opens the prompt again
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(ui::S(80), 0))) close = true;
    end(close);
  }
  if (begin(Dialog::EncryptWorkbook, TR("Encrypt Document###yc_dlg_encrypt"))) {
    ImGui::TextWrapped("%s", TR("Encrypt the contents of this file. Leave the password empty to remove the encryption."));
    ImGui::TextWrapped("%s", TR("Caution: if you lose or forget the password, it cannot be recovered."));
    ImGui::TextUnformatted(TR("Password:"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##pw", pw, sizeof pw, ImGuiInputTextFlags_Password);
    ImGui::TextUnformatted(TR("Reenter password:"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##pw2", pw2, sizeof pw2, ImGuiInputTextFlags_Password);
    const bool match = std::strcmp(pw, pw2) == 0;
    if (!match && pw2[0]) ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.2f, 1), "%s", TR("The passwords don't match."));
    bool close = false;
    ImGui::BeginDisabled(!match);
    if (DlgOkButton(TR("OK"), ImVec2(ui::S(80), 0))) {
      const std::string p = pw;
      s.engine->PostUser(p.empty() ? TR("Remove Encryption") : TR("Encrypt with Password"), [p](Document& d) { d.SetOpenPassword(p); });
      s.SetStatus(p.empty() ? TR("The workbook is no longer encrypted (from the next save).") : TR("The workbook will be saved encrypted."));
      close = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(ui::S(80), 0))) close = true;
    end(close);
  }
}

namespace {
bool g_scroll_to_formulas = false;  // File > Options opened by Error Checking Options...
}  // namespace

// [YC] the About menu on the tab row: the version and where the running program is
std::string AboutText() { return std::string("YCGrid ") + YC_VERSION_STRING + "\r\n" + ToUtf8(app::shell::ExePath()); }

void DrawAboutMenu(AppState&) {
  using ui::S;
  ImGui::Dummy(ImVec2(S(320), 0));  // at least this wide; a long path widens it (nothing in it sizes itself from the menu)
  // the lines a person reads are recorded too (ui.items), so a test reads the same
  const std::string version = std::string("YCGrid ") + YC_VERSION_STRING, path = ToUtf8(app::shell::ExePath());
  ImGui::TextUnformatted(version.c_str());
  ui::NoteUiText(version.c_str(), ImGui::GetCurrentContext()->LastItemData.Rect);
  ImGui::TextDisabled("%s", TR("Version"));
  ImGui::Spacing();
  ImGui::TextUnformatted(path.c_str());
  ui::NoteUiText(path.c_str(), ImGui::GetCurrentContext()->LastItemData.Rect);
  ImGui::TextDisabled("%s", TR("Program location"));
  ImGui::Separator();
  for (const char* id : {"AboutCopyInfo", "OpenProgramLocation"}) {
    const ui::Command* c = ui::Commands().Find(id);
    if (c && ImGui::MenuItem(c->label.c_str())) ui::Commands().Execute(id);
  }
}

void OpenProgramLocation(AppState& s) {
  const std::wstring exe = app::shell::ExePath();
  if (s.headless) {  // a hidden instance (tests) opens no Explorer window: it says where it would go
    s.SetStatus(TR("Program location: ") + ToUtf8(exe));
    return;
  }
  const std::wstring arg = L"/select,\"" + exe + L"\"";
  OpenOnDesktop(L"explorer.exe", arg);
}

void OpenFormulaOptions(AppState&) {
  ui::ribbon::St().backstage = true;
  ui::ribbon::St().backstage_page = 4;
  g_scroll_to_formulas = true;
}

// File > Open and Home: a search box over the recent workbooks, the pinned ones first; each can be pinned, unpinned
// or removed (its pin button, its right-click menu), as Excel's Recent list
void DrawRecentList(AppState& s) {
  using ui::S;
  static char filter[128] = "";
  ImGui::SetNextItemWidth(S(360));
  ImGui::InputTextWithHint("##recent_search", TR("Search"), filter, sizeof filter);
  ui::NoteUiItem("Search recent", ImGui::GetCurrentContext()->LastItemData.Rect);
  ImGui::Dummy(ImVec2(1, S(6)));
  std::vector<std::wstring> pinned, others;
  for (const auto& p : RecentFiles()) {
    if (!ContainsNoCaseW(p, filter)) continue;
    (IsPinned(p) ? pinned : others).push_back(p);
  }
  for (const auto& p : PinnedFiles()) {  // pinned files that dropped out of the last 20 stay listed
    if (ContainsNoCaseW(p, filter) && std::find(pinned.begin(), pinned.end(), p) == pinned.end()) pinned.push_back(p); // lesson-ok: find-push the recent and pinned files
  }
  std::wstring to_open, to_pin, to_remove;
  bool pin_value = false;
  int row = 0;
  auto list = [&](const char* heading, const std::vector<std::wstring>& files) {
    if (files.empty()) return;
    ImGui::TextUnformatted(heading);
    ImGui::Separator();
    for (const std::wstring& p : files) {
      const std::string name = ToUtf8(std::filesystem::path(p).filename().wstring());
      const std::string dir = ToUtf8(std::filesystem::path(p).parent_path().wstring());
      const bool pin = IsPinned(p);
      ImGui::PushID(row++);  // position as the ID: files with the same name don't collide
      const ImVec2 at = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable((name + "\n" + dir + "##recent").c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(S(560), S(40)))) to_open = p;
      if (ImGui::BeginPopupContextItem("##recent_menu")) {
        if (ImGui::MenuItem(TR("Open"))) to_open = p;
        if (ImGui::MenuItem(TR("Copy path to clipboard"))) ImGui::SetClipboardText(ToUtf8(p).c_str());
        if (ImGui::MenuItem(pin ? TR("Unpin from list") : TR("Pin to list"))) to_pin = p, pin_value = !pin;
        if (ImGui::MenuItem(TR("Remove from list"))) to_remove = p;
        ImGui::EndPopup();
      }
      // the pin at the row's right end: pins it to the top (Excel)
      ImGui::SetCursorScreenPos(ImVec2(at.x + S(524), at.y + S(8)));
      if (ImGui::InvisibleButton("##pin", ImVec2(S(26), S(24)))) to_pin = p, pin_value = !pin;
      {
        const ImRect pr(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ui::NoteUiItem((std::string(pin ? "Unpin " : "Pin ") + name).c_str(), pr);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered()) {
          dl->AddRectFilled(pr.Min, pr.Max, IM_COL32(0xD5, 0xE8, 0xD4, 255), S(2));
          ImGui::SetTooltip("%s", pin ? TR("Unpin this item from the list") : TR("Pin this item to the list"));
        }
        const char* g = pin ? ui::icon::Unpin : ui::icon::Pin;
        const ImVec2 ts = ui::F().icons->CalcTextSizeA(S(16), FLT_MAX, 0, g);
        dl->AddText(ui::F().icons, S(16), pr.GetCenter() - ts * 0.5f, IM_COL32(0x40, 0x40, 0x40, 255), g);
      }
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + S(40) + ImGui::GetStyle().ItemSpacing.y));
      ImGui::Dummy(ImVec2(0, 0));
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, S(8)));
  };
  list(TR("Pinned"), pinned);
  list(TR("Recent"), others);
  if (pinned.empty() && others.empty()) ImGui::TextDisabled("%s", TR("No recent workbooks."));
  // act after the lists (opening, pinning and removing change them)
  if (!to_pin.empty()) SetPinned(to_pin, pin_value);
  if (!to_remove.empty()) RemoveRecent(to_remove);
  if (!to_open.empty()) {
    std::error_code ec;
    if (!s.file_path.empty() && std::filesystem::equivalent(s.file_path, to_open, ec)) {
      ui::ribbon::St().backstage = false;  // already open here: back to it
    } else if (!std::filesystem::exists(to_open, ec)) {  // the Backstage stays: the list is seen without the entry
      const std::wstring gone = to_open;
      AskYesNo(s, "YCGrid", TR("Sorry, we couldn't find ") + ToUtf8(gone) + TR(". Is it possible it was moved, renamed or deleted?\n\nRemove it from the Recent list?"),
               [gone] { RemoveRecent(gone); });
    } else if (WorkbookInUse(s)) {
      ui::ribbon::St().backstage = false;
      OpenInNewWindow(to_open);
    } else {
      ui::ribbon::St().backstage = false;
      if (ConfirmDiscard(s)) OpenWorkbook(s, to_open);
    }
  }
}

void DrawBackstageContent(AppState& s, int page, const ImRect& area) {
  using ui::S;
  (void)area;
  switch (page) {
    case 0: {
      // Info, laid out as Excel's: the file, Protect Workbook, Manage Workbook, the properties; details at the bottom
      const auto& wb = s.snap->wb;
      trust::DrawInfoSection(s);  // Security Warning: the workbook's Python code is disabled (Enable Content)
      ImGui::TextUnformatted(s.file_path.empty() ? TR("(not saved yet)") : ToUtf8(s.file_path).c_str());
      if (wb.read_only) ImGui::TextColored(ImVec4(0.75f, 0.2f, 0.2f, 1), "%s", wb.read_only_reason.c_str());
      ImGui::Dummy(ImVec2(1, S(12)));
      ImGui::SeparatorText(TR("Protect Workbook"));
      ImGui::TextDisabled("%s", wb.open_password.empty() ? TR("Anyone can open this workbook.") : TR("A password is required to open this workbook."));
      if (ImGui::Button(TR("Encrypt with Password..."))) {
        ui::ribbon::St().backstage = false;
        s.OpenDialog(Dialog::EncryptWorkbook);
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Protect Workbook Structure..."))) {
        ui::ribbon::St().backstage = false;
        ui::Commands().Execute("ProtectWorkbook");
      }
      ImGui::Dummy(ImVec2(1, S(8)));
      ImGui::SeparatorText(TR("Manage Workbook"));
      ImGui::TextDisabled("%s", wb.dirty ? TR("There are unsaved changes.") : TR("There are no unsaved changes."));
      if (ImGui::Button(TR("Recover Unsaved Workbooks"))) {
        ui::ribbon::St().backstage = false;
        if (!OpenRecoveryPane()) s.SetStatus(TR("There are no unsaved workbooks to recover."), 6.0);
      }
      ImGui::Dummy(ImVec2(1, S(8)));
      DrawInfoProperties(s);
      ImGui::Dummy(ImVec2(1, S(16)));
      if (ImGui::CollapsingHeader(TR("About"))) {
        ImGui::Text("YCGrid %s", YC_VERSION_STRING);
        ImGui::Text(TR("Sheets: %zu"), wb.sheets.size());
        ImGui::Text(TR("Formula definitions: %u  Styles: %zu"), wb.formulas->size(), wb.styles->size());
        ImGui::Text(TR("File format: %s"), wb.file_format_major ? ("YCG " + std::to_string(wb.file_format_major) + "." + std::to_string(wb.file_format_minor)).c_str() : TR("(new workbook)"));
        ImGui::Text(TR("Workbook KV (ThisWorkbook.Store): %zu entries"), wb.store->size());
        ImGui::TextDisabled(TR("Dear ImGui %s (docking) / ImPlot %s; versions pinned by third_party/deps.lock"), IMGUI_VERSION, IMPLOT_VERSION);
      }
      break;
    }
    case 6:  // Home: a new blank workbook, then the recent workbooks (Excel's Home)
    case 1: {
      if (ImGui::Button(TR("Blank workbook"), ImVec2(S(180), S(120)))) {
        ui::ribbon::St().backstage = false;
        NewWorkbook(s);
      }
      if (page == 6) {
        ImGui::Dummy(ImVec2(1, S(10)));
        DrawRecentList(s);
      }
      break;
    }
    case 2: {
      if (ImGui::Button(TR("Browse..."), ImVec2(S(140), S(36)))) {
        ui::ribbon::St().backstage = false;
        OpenWorkbookDialog(s);
      }
      ImGui::Dummy(ImVec2(1, S(10)));
      DrawRecentList(s);
      break;
    }
    case 3: {
      // Export: Create PDF, then Change File Type (Excel's two choices)
      ImGui::SeparatorText(TR("Create PDF"));
      DrawExportPdfButton(s);
      ImGui::Dummy(ImVec2(1, S(12)));
      ImGui::SeparatorText(TR("Change File Type"));
      struct Type {
        const char* label;
        const char* note;
        int filter;  // the Save As dialog's type: 1 YCGrid, 2 CSV, 3 Excel
      };
      static const Type kTypes[] = {{TR("YCGrid Workbook (*.ycg)"), TR("Everything: sheets, charts, objects, the Python project."), 1},
                                    {TR("Excel Workbook (*.xlsx)"), TR("Values, formulas and formatting, for Excel."), 3},
                                    {TR("CSV UTF-8 (Comma delimited) (*.csv)"), TR("The current sheet's values (displayed text)."), 2}};
      for (const Type& t : kTypes) {
        if (ImGui::Button(t.label, ImVec2(S(320), S(36)))) {
          ui::ribbon::St().backstage = false;
          if (t.filter == 2) {
            ExportCsvDialog(s);
          } else {
            int filter = t.filter;
            std::wstring initial = s.file_path.empty() ? TRW(L"Book1.ycg") : std::filesystem::path(s.file_path).filename().wstring();
            initial = std::filesystem::path(initial).replace_extension(t.filter == 3 ? L".xlsx" : L".ycg").wstring();
            const std::wstring path = FileDialog(s.hwnd, true, initial, &filter);
            if (!path.empty()) SaveAsExcelChecked(s, path, filter, false);
          }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", t.note);
      }
      break;
    }
    case 5: DrawPrintBackstage(s, area); break;
    default: {
      // UI language (like Excel: takes effect after a restart)
      {
        ImGui::TextUnformatted(i18n::LanguageLabel());
        const i18n::Lang saved = i18n::SavedLanguage();
        ImGui::SetNextItemWidth(S(220));
        if (ui::BeginCombo("##yc_lang", i18n::LangName(saved))) {
          for (i18n::Lang l : {i18n::Lang::En, i18n::Lang::ZhTW}) {
            if (ImGui::Selectable(i18n::LangName(l), l == saved) && l != saved) i18n::SaveLanguage(l);
          }
          ImGui::EndCombo();
        }
        // only after the user chose another language here: a window started with --lang (tests, agents) shows another
        // language than the saved one without anything to restart for
        static const i18n::Lang saved_at_start = saved;
        if (saved != saved_at_start && saved != i18n::Current() && !s.headless) {
          ImGui::SameLine();
          if (ImGui::Button(TR("Restart Now"))) RestartApp(s);
          ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.05f, 1), "%s", TR("The new language takes effect after YCGrid restarts."));
        }
        ImGui::Separator();
      }
      // File > Options as Excel's dialog: the boxes edit a copy, OK applies it (and saves settings.json), Cancel or
      // leaving the page drops it
      static Settings draft;
      static bool view_grid = true, view_head = true, view_fbar = true;
      // Display options for this workbook / this worksheet (they belong to the workbook, not to settings.json)
      static bool view_tabs = true, view_hscroll = true, view_vscroll = true, view_zeros = true, view_breaks = false;
      static bool wb_1904 = false, wb_precision = false;  // When calculating this workbook (saved with it)
      static uint32_t view_gridline = 0;
      static char std_font[128] = "";
      static bool options_opened = false;
      static int drawn_frame = -10;
      if (ImGui::GetFrameCount() - drawn_frame > 1) {
        draft = Config();
        view_grid = s.show_gridlines, view_head = s.show_headings, view_fbar = s.show_formula_bar;
        view_tabs = s.show_sheet_tabs, view_hscroll = s.show_hscroll, view_vscroll = s.show_vscroll;
        view_zeros = s.Win(s.focus_sheet).zeros;
        view_breaks = s.Win(s.focus_sheet).page_breaks;
        wb_1904 = s.snap && s.snap->wb.date1904;
        wb_precision = s.snap && s.snap->wb.precision_as_displayed;
        view_gridline = s.Win(s.focus_sheet).gridline_color;
        std::snprintf(std_font, sizeof std_font, "%s", draft.standard_font.empty() ? "Calibri" : draft.standard_font.c_str());
        options_opened = true;  // Excel's Options opens on General
      }
      drawn_frame = ImGui::GetFrameCount();
      // the options scroll; OK and Cancel stay at the bottom (Excel's dialog)
      // Excel's categories on the left; a click scrolls the options to that category
      static const char* const kCats[] = {TR("General"), TR("Formulas"), TR("Data"), TR("Proofing"), TR("Save"), TR("Advanced"), TR("Trust Center")};
      static int cat = 0, jump = -1;
      if (options_opened) cat = 0, jump = 0, options_opened = false;
      const float opt_h = std::max(S(120), ImGui::GetContentRegionAvail().y - S(48));
      ImGui::BeginChild("##yc_opt_nav", ImVec2(S(170), opt_h));
      for (int i = 0; i < static_cast<int>(std::size(kCats)); ++i) {
        if (ImGui::Selectable(kCats[i], cat == i, 0, ImVec2(0, S(26)))) cat = i, jump = i;
      }
      ImGui::EndChild();
      ImGui::SameLine();
      auto jump_here = [&](int k) {
        if (jump != k) return;
        ImGui::SetScrollHereY(0.0f);
        jump = -1;
      };
      ImGui::BeginChild("##yc_options", ImVec2(0, opt_h));
      Settings& cfg = draft;
      bool changed = false;
      if (jump == 0) ImGui::SetScrollY(0.0f), jump = -1;
      ImGui::SeparatorText(TR("General"));
      if (ImGui::Checkbox(TR("Show Quick Analysis options on selection"), &cfg.show_quick_analysis)) changed = true;
      ImGui::TextUnformatted(TR("When creating new workbooks"));
      ImGui::SetNextItemWidth(S(220));
      if (ImGui::InputText(TR("Use this as the default font"), std_font, sizeof std_font)) cfg.standard_font = std_font, changed = true;
      {
        float size = cfg.standard_font_size > 0 ? static_cast<float>(cfg.standard_font_size) : 11.0f;
        ImGui::SetNextItemWidth(S(120));
        if (ImGui::InputFloat(TR("Font size"), &size, 1.0f, 2.0f, "%.0f")) cfg.standard_font_size = std::clamp(static_cast<double>(size), 1.0, 409.0), changed = true;
      }
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputInt(TR("Include this many sheets"), &cfg.sheets_in_new_workbook, 1, 1)) {
        cfg.sheets_in_new_workbook = std::clamp(cfg.sheets_in_new_workbook, 1, 255);
        changed = true;
      }
      jump_here(1);
      ImGui::SeparatorText(TR("Calculation"));
      {
        // Workbook Calculation, as Excel's three choices (Formulas > Calculation Options)
        ImGui::TextUnformatted(TR("Workbook Calculation"));
        int mode = !cfg.auto_calc ? 2 : (cfg.auto_data_tables ? 0 : 1);
        bool mc = ui::RadioButton(TR("Automatic"), &mode, 0);
        mc = ui::RadioButton(TR("Automatic except for data tables"), &mode, 1) || mc;
        mc = ui::RadioButton(TR("Manual"), &mode, 2) || mc;
        if (mc) {
          cfg.auto_calc = mode != 2;
          cfg.auto_data_tables = mode == 0;
          changed = true;
        }
      }
      if (ImGui::Checkbox(TR("Truncate numbers longer than 15 significant digits like Excel (off = keep them exactly as Decimal)"), &cfg.excel_15_digit_input)) {
        changed = true;
      }
      if (ImGui::Checkbox(TR("Recalculate workbook before saving"), &cfg.calculate_before_save)) changed = true;
      if (ImGui::Checkbox(TR("Format stale values (manual calculation)"), &cfg.format_stale_values)) changed = true;
      if (ImGui::Checkbox(TR("Enable multi-threaded calculation"), &cfg.multi_threaded_calc)) changed = true;
      {
        // Number of calculation threads: every processor (but the UI's and Python's), or a count
        ImGui::BeginDisabled(!cfg.multi_threaded_calc);
        int mode = cfg.calc_thread_count > 0 ? 1 : 0;
        const int all = static_cast<int>(core::PoolSize());
        char lab[96];
        std::snprintf(lab, sizeof lab, TR("Use all processors on this computer: %d"), all);
        if (ui::RadioButton(lab, &mode, 0)) cfg.calc_thread_count = 0, changed = true;
        if (ui::RadioButton(TR("Manual##threads"), &mode, 1) && cfg.calc_thread_count == 0) cfg.calc_thread_count = all, changed = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(mode == 0);
        ImGui::SetNextItemWidth(S(90));
        int n = std::max(1, cfg.calc_thread_count > 0 ? cfg.calc_thread_count : all);
        if (ImGui::InputInt("##calc_threads", &n, 1, 4) && mode == 1) cfg.calc_thread_count = std::clamp(n, 1, 1024), changed = true;
        ImGui::EndDisabled();
        ImGui::EndDisabled();
      }
      if (ImGui::Checkbox(TR("Enable iterative calculation"), &cfg.iterative)) changed = true;
      ImGui::BeginDisabled(!cfg.iterative);
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputInt(TR("Maximum Iterations"), &cfg.max_iterations, 1, 10)) {
        cfg.max_iterations = std::clamp(cfg.max_iterations, 1, 32767);
        changed = true;
      }
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputDouble(TR("Maximum Change"), &cfg.max_change, 0.0, 0.0, "%.6g")) {
        if (!(cfg.max_change > 0)) cfg.max_change = 0.001;
        changed = true;
      }
      ImGui::EndDisabled();
      ImGui::SeparatorText(TR("Formulas"));
      if (g_scroll_to_formulas) {
        ImGui::SetScrollHereY(0.0f);  // Error Checking Options... opens here
        g_scroll_to_formulas = false;
      }
      if (ImGui::Checkbox(TR("R1C1 reference style"), &cfg.r1c1)) changed = true;
      if (ImGui::Checkbox(TR("Formula AutoComplete"), &cfg.formula_autocomplete)) changed = true;
      if (ImGui::Checkbox(TR("Use table names in formulas"), &cfg.generate_table_refs)) changed = true;
      if (ImGui::Checkbox(TR("Use GetPivotData functions for PivotTable references"), &cfg.generate_getpivotdata)) changed = true;
      // Error Checking and its rules (Application.ErrorCheckingOptions)
      ImGui::SeparatorText(TR("Error Checking"));
      if (ImGui::Checkbox(TR("Enable background error checking"), &cfg.error_checking)) changed = true;
      if (ImGui::Button(TR("Reset Ignored Errors"))) {
        // every cell's Ignore Error is taken back (Excel: the errors ignored in this workbook show again)
        s.engine->PostUser(TR("Reset Ignored Errors"), [](Document& d) {
          for (const auto& sh : d.wb().sheets) {
            if (!sh->ignored_errors->empty()) d.SetIgnoreErrors(sh->id, Rect{0, 0, core::kRowEnd, core::kMaxCols - 1}, false);
          }
        });
      }
      ImGui::TextUnformatted(TR("Error checking rules"));
      ImGui::BeginDisabled(!cfg.error_checking);
      {
        struct Rule {
          const char* label;
          bool* v;
        } rules[] = {{TR("Cells containing formulas that result in an error"), &cfg.err_evaluate_to_error},
                     {TR("Inconsistent calculated column formula in tables"), &cfg.err_inconsistent_list_formula},
                     {TR("Cells containing years represented as 2 digits"), &cfg.err_text_date},
                     {TR("Numbers formatted as text or preceded by an apostrophe"), &cfg.err_number_as_text},
                     {TR("Formulas inconsistent with other formulas in the region"), &cfg.err_inconsistent_formula},
                     {TR("Formulas which omit cells in a region"), &cfg.err_omitted_cells},
                     {TR("Unlocked cells containing formulas"), &cfg.err_unlocked_formula},
                     {TR("Formulas referring to empty cells"), &cfg.err_empty_cell_reference},
                     {TR("Data entered in a table is invalid"), &cfg.err_list_data_validation},
                     {TR("Misleading number formats"), &cfg.err_misleading_format}};
        for (const Rule& r : rules) {
          if (ImGui::Checkbox(r.label, r.v)) changed = true;
        }
      }
      ImGui::EndDisabled();
      if (ImGui::Checkbox(TR("Enable fill handle and cell drag-and-drop"), &cfg.cell_drag_and_drop)) changed = true;
      jump_here(5);
      ImGui::SeparatorText(TR("Editing"));
      if (ImGui::Checkbox(TR("After pressing Enter, move selection"), &cfg.move_after_return)) changed = true;
      ImGui::BeginDisabled(!cfg.move_after_return);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(90));
      const char* dirs[] = {TR("Down"), TR("Right##ctx_dir"), TR("Up"), TR("Left##ctx_dir")};
      if (ui::Combo(TR("Direction"), &cfg.move_after_return_dir, dirs, 4)) changed = true;
      ImGui::EndDisabled();
      if (ImGui::Checkbox(TR("Allow editing directly in cells"), &cfg.edit_directly_in_cell)) changed = true;
      if (ImGui::Checkbox(TR("Enable AutoComplete for cell values"), &cfg.enable_autocomplete)) changed = true;
      if (ImGui::Checkbox(TR("Automatically Flash Fill"), &cfg.auto_flash_fill)) changed = true;
      if (ImGui::Checkbox(TR("Alert the user when a potentially time consuming operation occurs"), &cfg.large_operation_alert)) changed = true;
      ImGui::BeginDisabled(!cfg.large_operation_alert);
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputInt(TR("When this number of cells (in thousands) is affected"), &cfg.large_operation_thousands, 1000, 10000)) {
        cfg.large_operation_thousands = std::clamp(cfg.large_operation_thousands, 1, 1000000000);
        changed = true;
      }
      ImGui::EndDisabled();
      if (ImGui::Button(TR("Edit Custom Lists..."))) s.OpenDialog(Dialog::CustomLists);
      ImGui::SameLine();
      ImGui::TextDisabled("%s", TR("Lists for AutoFill and Sort."));
      ImGui::TextUnformatted(TR("When calculating this workbook"));
      if (ImGui::Checkbox(TR("Set precision as displayed"), &wb_precision)) changed = true;
      if (ImGui::Checkbox(TR("Use 1904 date system"), &wb_1904)) changed = true;
      ImGui::TextUnformatted(TR("Display options for this workbook"));
      if (ImGui::Checkbox(TR("Show horizontal scroll bar"), &view_hscroll)) changed = true;
      if (ImGui::Checkbox(TR("Show vertical scroll bar"), &view_vscroll)) changed = true;
      if (ImGui::Checkbox(TR("Show sheet tabs"), &view_tabs)) changed = true;
      ImGui::TextUnformatted(TR("Display options for this worksheet"));
      if (ImGui::Checkbox(TR("Show page breaks"), &view_breaks)) changed = true;
      if (ImGui::Checkbox(TR("Show a zero in cells that have zero value"), &view_zeros)) changed = true;
      {
        uint32_t argb = view_gridline ? (view_gridline | 0xFF000000u) : 0;
        if (PaletteColor(TR("Gridline color"), &argb, true, TR("Automatic"))) view_gridline = argb ? (argb & 0x00FFFFFFu) : 0, changed = true;
      }
      ImGui::TextUnformatted(TR("Chart"));
      if (ImGui::Checkbox(TR("Show chart element names on hover"), &cfg.show_chart_tip_names)) changed = true;
      if (ImGui::Checkbox(TR("Show data point values on hover"), &cfg.show_chart_tip_values)) changed = true;
      ImGui::TextUnformatted(TR("Cut, copy, and paste"));
      if (ImGui::Checkbox(TR("Show Paste Options button when content is pasted"), &cfg.show_paste_options)) changed = true;
      if (ImGui::Checkbox(TR("Show Insert Options buttons"), &cfg.show_insert_options)) changed = true;
      if (ImGui::Checkbox(TR("Alert before overwriting cells"), &cfg.alert_before_overwriting)) changed = true;
      jump_here(2);
      ImGui::SeparatorText(TR("Data"));
      {
        bool off = !cfg.pivot_auto_group_dates;
        if (ImGui::Checkbox(TR("Disable automatic grouping of Date/Time columns in PivotTables"), &off)) {
          cfg.pivot_auto_group_dates = !off;
          changed = true;
        }
      }
      jump_here(3);
      ImGui::SeparatorText(TR("Proofing"));
      if (ImGui::Checkbox(TR("Ignore words in UPPERCASE"), &cfg.spell_ignore_upper)) changed = true;
      if (ImGui::Checkbox(TR("Ignore words that contain numbers"), &cfg.spell_ignore_numbers)) changed = true;
      if (ImGui::Checkbox(TR("Ignore Internet and file addresses"), &cfg.spell_ignore_internet)) changed = true;
      if (ImGui::Checkbox(TR("Flag repeated words"), &cfg.spell_repeated)) changed = true;
      if (ImGui::Checkbox(TR("Replace text as you type"), &cfg.autocorrect_replace)) changed = true;
      if (ImGui::Checkbox(TR("Internet and network paths with hyperlinks"), &cfg.autoformat_hyperlinks)) changed = true;
      DrawAutoCorrectList();  // AutoCorrect's replacements (a deletion applies at once, as in Excel's AutoCorrect dialog)
      jump_here(4);
      ImGui::SeparatorText(TR("Save"));
      if (ImGui::Checkbox(TR("Save AutoRecover information every"), &cfg.autorecover)) changed = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(90));
      if (ImGui::InputInt(TR("minutes"), &cfg.autorecover_minutes, 1, 5)) {
        cfg.autorecover_minutes = std::clamp(cfg.autorecover_minutes, 1, 120);
        changed = true;
      }
      ImGui::TextDisabled(TR("AutoRecover file location: %s"), ShownPath(AutoRecoverDir()).c_str());
      ImGui::SeparatorText(TR("View"));
      if (ImGui::Checkbox(TR("Show gridlines"), &view_grid)) changed = true;
      if (ImGui::Checkbox(TR("Show headings"), &view_head)) changed = true;
      if (ImGui::Checkbox(TR("Show formula bar"), &view_fbar)) changed = true;
      ImGui::SeparatorText(TR("Live data"));
      ImGui::SetNextItemWidth(S(140));
      if (ImGui::InputInt(TR("ms between screen updates from Python / live data"), &cfg.publish_interval_ms, 10, 100)) {
        cfg.publish_interval_ms = std::clamp(cfg.publish_interval_ms, 0, 5000);
        changed = true;
      }
      ImGui::TextDisabled("%s", TR("0 = update on every change. Typing and clicking always update at once."));
      ImGui::SeparatorText(TR("Projects"));
      {
        // the boxes show the saved values; they are refreshed only when those change (never while typing)
        static char folder[1024], vscode[1024];
        static std::wstring shown_folder = L"\x01", shown_vscode = L"\x01";
        if (shown_folder != cfg.projects_folder) {
          std::snprintf(folder, sizeof(folder), "%s", ToUtf8(cfg.projects_folder).c_str());
          shown_folder = cfg.projects_folder;
        }
        if (shown_vscode != cfg.vscode_path) {
          std::snprintf(vscode, sizeof(vscode), "%s", ToUtf8(cfg.vscode_path).c_str());
          shown_vscode = cfg.vscode_path;
        }
        ImGui::SetNextItemWidth(S(420));
        if (ImGui::InputTextWithHint(TR("Projects folder"), ToUtf8(ProjectsDir()).c_str(), folder, sizeof(folder), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
          cfg.projects_folder = shown_folder = FromUtf8(folder);
          changed = true;
        }
        ImGui::TextDisabled("%s", TR("Scripts and hidden instances create the project folder of a workbook that was never saved here. With a window, Alt+F11 asks you to save the workbook first."));
        ImGui::SetNextItemWidth(S(420));
        if (ImGui::InputTextWithHint(TR("VS Code"), TR("(found automatically)"), vscode, sizeof(vscode), ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
          cfg.vscode_path = shown_vscode = FromUtf8(vscode);
          changed = true;
        }
      }
      (void)changed;
      jump_here(6);
      trust::DrawOptionsSection(s);
      ImGui::EndChild();
      ImGui::Separator();
      if (DlgOkButton(TR("OK"), ImVec2(S(90), 0))) {
        // OK: the copy becomes the settings, what the engine and the window use follows, settings.json is written
        Settings& real = Config();
        draft.show_gridlines = view_grid, draft.show_headings = view_head, draft.show_formula_bar = view_fbar;
        // the boxes of this page (what its other buttons and dialogs set meanwhile, Custom Lists, Trust Center, stays)
#define YC_OPT(f) real.f = draft.f;
        YC_OPT(auto_calc) YC_OPT(auto_data_tables) YC_OPT(excel_15_digit_input) YC_OPT(iterative) YC_OPT(max_iterations) YC_OPT(max_change)
        YC_OPT(r1c1) YC_OPT(formula_autocomplete) YC_OPT(error_checking) YC_OPT(err_evaluate_to_error) YC_OPT(err_number_as_text)
        YC_OPT(err_inconsistent_formula) YC_OPT(err_text_date) YC_OPT(err_omitted_cells) YC_OPT(err_unlocked_formula) YC_OPT(err_empty_cell_reference)
        YC_OPT(err_list_data_validation) YC_OPT(err_inconsistent_list_formula) YC_OPT(cell_drag_and_drop) YC_OPT(move_after_return)
        YC_OPT(move_after_return_dir) YC_OPT(edit_directly_in_cell) YC_OPT(enable_autocomplete) YC_OPT(auto_flash_fill) YC_OPT(pivot_auto_group_dates)
        YC_OPT(autorecover) YC_OPT(autorecover_minutes) YC_OPT(publish_interval_ms) YC_OPT(projects_folder) YC_OPT(vscode_path)
        YC_OPT(show_gridlines) YC_OPT(show_headings) YC_OPT(show_formula_bar)
        YC_OPT(large_operation_alert) YC_OPT(large_operation_thousands) YC_OPT(show_chart_tip_names) YC_OPT(show_chart_tip_values)
        YC_OPT(calculate_before_save) YC_OPT(format_stale_values) YC_OPT(multi_threaded_calc) YC_OPT(calc_thread_count)
        YC_OPT(generate_getpivotdata) YC_OPT(generate_table_refs)
        YC_OPT(show_quick_analysis) YC_OPT(standard_font) YC_OPT(standard_font_size) YC_OPT(sheets_in_new_workbook) YC_OPT(show_paste_options)
        YC_OPT(show_insert_options) YC_OPT(alert_before_overwriting) YC_OPT(spell_ignore_upper) YC_OPT(spell_ignore_numbers)
        YC_OPT(spell_ignore_internet) YC_OPT(spell_repeated) YC_OPT(autocorrect_replace) YC_OPT(autoformat_hyperlinks)
#undef YC_OPT
        ApplyCalcThreads();  // Enable multi-threaded calculation, its threads
        s.engine->Post([a = real.auto_calc, t = real.auto_data_tables, x = real.excel_15_digit_input, it = real.iterative, n = real.max_iterations,
                        c = real.max_change](Document& d) {
          d.auto_calc = a;
          d.auto_data_tables = t;
          d.excel_15_digit_input = x;
          d.iterate = it;
          d.max_iterations = n;
          d.max_change = c;
        });
        s.engine->SetPublishInterval(real.publish_interval_ms);
        s.show_gridlines = s.default_gridlines = view_grid;
        s.show_headings = s.default_headings = view_head;
        s.show_formula_bar = view_fbar;
        // the workbook's and the active sheet's display options (saved with the workbook, in its layout)
        s.show_sheet_tabs = view_tabs, s.show_hscroll = view_hscroll, s.show_vscroll = view_vscroll;
        s.Win(s.focus_sheet).zeros = view_zeros;
        s.Win(s.focus_sheet).page_breaks = view_breaks;
        if (s.snap && wb_1904 != s.snap->wb.date1904) {
          s.engine->PostUser(TR("Use 1904 date system"), [on = wb_1904](Document& d) { d.SetDate1904(on); });
        }
        if (s.snap && wb_precision != s.snap->wb.precision_as_displayed) {
          auto apply = [&s, on = wb_precision]() {
            s.engine->PostUser(TR("Set precision as displayed"), [on](Document& d) { d.SetPrecisionAsDisplayed(on); });
          };
          // the numbers are rounded for good: asked first, as Excel does
          if (wb_precision) AskYesNo(s, "YCGrid", TR("Data will permanently lose accuracy."), apply);
          else apply();
        }
        s.Win(s.focus_sheet).gridline_color = view_gridline;
        if (!SaveSettings()) s.SetStatus(TR("Couldn't save the settings: ") + ToUtf8(SettingsPath()), 8.0);
        drawn_frame = -10;
        ui::ribbon::St().backstage = false;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) {
        drawn_frame = -10;  // the copy is dropped
        ui::ribbon::St().backstage = false;
      }
      ImGui::Dummy(ImVec2(1, S(10)));
      ImGui::TextDisabled(TR("Settings file: %s"), ToUtf8(SettingsPath()).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton(TR("Open Folder"))) {
        const std::wstring dir = std::filesystem::path(SettingsPath()).parent_path().wstring();
        if (!OpenOnDesktop(dir, L"", s.hwnd)) s.SetStatus(TR("Open: ") + ToUtf8(dir), 4.0);
      }
      break;
    }
  }
}

}  // namespace yc::app
