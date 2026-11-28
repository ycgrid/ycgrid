// SPDX-License-Identifier: Apache-2.0
// python_ui.cpp - Python-related UI: Developer commands, Alt+F11, Immediate window, missing-packages bar, Macros dialog
#include "app_state.h"
#include "core/i18n.h"

#include "code_complete.h"
#include "python_bridge.h"
#include "error_log.h"
#include "proc_util.h"
#include "settings.h"
#include "trust.h"

#include "app/shell_integration.h"
#include "core/paths.h"
#include "core/py_source.h"
#include "core/utf8.h"
#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "theme/theme.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>

#include <commdlg.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

namespace yc::app {

using ui::Command;
using ui::Commands;
using ui::S;
namespace fs = std::filesystem;

namespace {

std::wstring ProjectDir(AppState& s) { return s.file_path.empty() ? std::wstring() : fs::path(s.file_path).parent_path().wstring(); }

bool HasMain(const std::wstring& dir) {
  std::error_code ec;
  return !dir.empty() && fs::exists(fs::path(dir) / L"main.py", ec);
}

// Python code the host loads from the folder besides main.py (sheets\*.py, modules\*.py)
bool HasProjectCode(const std::wstring& dir) {
  if (dir.empty()) return false;
  std::error_code ec;
  for (const wchar_t* sub : {L"sheets", L"modules"}) {
    const fs::path d = fs::path(dir) / sub;
    if (!fs::is_directory(d, ec)) continue;
    for (const auto& e : fs::directory_iterator(d, fs::directory_options::skip_permission_denied, ec)) {
      if (e.path().extension() == L".py") return true;
    }
  }
  return false;
}

int64_t ProjectStamp(const std::wstring& dir) {
  int64_t latest = 0;
  std::error_code ec;
  auto scan = [&](const fs::path& p, bool recursive) {
    if (!fs::exists(p, ec)) return;
    auto visit = [&](const fs::directory_entry& e) {
      if (e.path().extension() == L".py" || e.path().filename() == L"requirements.txt") {
        const auto t = fs::last_write_time(e.path(), ec).time_since_epoch().count();
        latest = std::max<int64_t>(latest, t);
      }
    };
    if (recursive) {
      for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) visit(e);
    } else {
      for (const auto& e : fs::directory_iterator(p, ec)) visit(e);
    }
  };
  scan(dir, false);
  for (const wchar_t* sub : {L"sheets", L"modules", L"lib"}) scan(fs::path(dir) / sub, true);
  return latest;
}

// run yc.cli with this Python and yc package (any thread)
bool RunYcCliWith(const std::wstring& py, const std::wstring& ycdir, const std::wstring& args, std::string* out, DWORD timeout_ms = 60000) {
  if (py.empty() || ycdir.empty()) {
    if (out) *out = TR("YCGrid's Python runtime (runtime\\python) wasn't found.");
    return false;
  }
  std::wstring cmd = L"\"" + py + L"\" -E -s -X utf8 -c \"import sys;sys.path.insert(0,r'" + ycdir + L"');from yc.cli import main;sys.exit(main())\" " + args;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  ::CreatePipe(&rd, &wr, &sa, 0);
  ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  const bool ok = CreateProcessInheriting(py.c_str(), buf.data(), {wr}, CREATE_NO_WINDOW, nullptr, nullptr, si, &pi);
  ::CloseHandle(wr);
  std::string text;
  if (ok) {
    char tmp[4096];
    DWORD n = 0;
    while (::ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n) text.append(tmp, n);
    ::WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(rd);
    if (out) *out = text;
    return code == 0;
  }
  ::CloseHandle(rd);
  if (out) *out = TR("Couldn't run Python.");
  return false;
}

// run yc.cli with the runtime's Python (create the project template)
bool RunYcCli(AppState& s, const std::wstring& args, std::string* out) {
  return RunYcCliWith(PythonBridge::FindPython(ProjectDir(s)), PythonBridge::FindYcPackage(), args, out);
}

// results of the VS Code jobs that run on a worker thread, shown by the UI thread
std::mutex g_vs_mu;
std::vector<std::pair<std::string, std::string>> g_vs_msgs;  // title ("" = status bar), text
void VsMessage(const std::string& title, const std::string& text) {
  std::lock_guard<std::mutex> lk(g_vs_mu);
  g_vs_msgs.emplace_back(title, text);
}

std::wstring Env(const wchar_t* name) {
  const DWORD n = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  v.resize(::GetEnvironmentVariableW(name, v.data(), n));
  return v;
}

// a registry string, read only (YCGrid never writes the registry outside --install)
std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) {
  DWORD size = 0;
  if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
    return {};
  }
  std::wstring v(size / sizeof(wchar_t), L'\0');
  if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view, nullptr, v.data(), &size) != ERROR_SUCCESS) return {};
  v.resize(std::wcslen(v.c_str()));
  return v;
}

// a hidden instance started with YCGRID_NO_VSCODE=1 looks for VS Code nowhere but the settings, as on a machine
// without it (tests of what Alt+F11 asks then)
bool NoVsCodeSearch() { return Env(L"YCGRID_NO_VSCODE") == L"1"; }

std::wstring FindVsCode() {
  // settings.json editor.vscode first, then every place VS Code is usually installed (core::VsCodeCandidates)
  std::error_code ec;
  const std::wstring& configured = Config().vscode_path;
  if (!configured.empty() && fs::is_regular_file(configured, ec)) return configured;
  if (NoVsCodeSearch()) return {};
  core::VsCodeSearch in;
  in.path = Env(L"PATH");
  in.local_app_data = Env(L"LOCALAPPDATA");
  in.program_files = Env(L"ProgramW6432").empty() ? Env(L"ProgramFiles") : Env(L"ProgramW6432");
  in.program_files_x86 = Env(L"ProgramFiles(x86)");
  in.program_data = Env(L"ProgramData");
  in.user_profile = Env(L"USERPROFILE");
  in.scoop = Env(L"SCOOP");
  static const wchar_t* const kUninstall[] = {
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{771FD6B0-FA20-440A-A002-3B3BAC16DC50}_is1",  // user setup
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{EA457B21-F73E-494C-ACAB-524FDE069978}_is1",  // system setup
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{D628A17A-9713-46BF-8D57-E671B46A741E}_is1"};  // system, 32-bit
  auto note = [&](std::wstring v) {
    if (!v.empty()) in.registered.push_back(std::move(v));
  };
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    note(RegString(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Code.exe", nullptr));
    for (const wchar_t* key : kUninstall) {
      note(RegString(root, key, L"InstallLocation", RRF_SUBKEY_WOW6464KEY));
      note(RegString(root, key, L"InstallLocation", RRF_SUBKEY_WOW6432KEY));
    }
  }
  note(RegString(HKEY_CLASSES_ROOT, L"vscode\\shell\\open\\command", nullptr));
  note(RegString(HKEY_CLASSES_ROOT, L"Applications\\Code.exe\\shell\\open\\command", nullptr));
  for (const std::wstring& p : core::VsCodeCandidates(in)) {
    if (fs::is_regular_file(p, ec)) return p;
  }
  return {};
}

std::wstring SkillDir() {
  fs::path dir = app::shell::ExeDir();
  std::error_code ec;
  for (int i = 0; i < 6 && !dir.empty(); ++i) {
    if (fs::exists(dir / L"skill" / L"ycgrid-python" / L"SKILL.md", ec)) return (dir / L"skill" / L"ycgrid-python").wstring();
    const fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

bool CreateProject(AppState& s, const std::wstring& dir) {
  std::wstring args = L"init \"" + dir + L"\"";
  if (s.snap) {
    for (const auto& sh : s.snap->wb.sheets) args += L" --sheet \"" + FromUtf8(sh->code_name + "=" + sh->name) + L"\"";
  }
  const std::wstring skill = SkillDir();
  if (!skill.empty()) args += L" --skill \"" + skill + L"\"";
  std::string out;
  if (!RunYcCli(s, args, &out)) {
    s.ShowError(TR("Couldn't create the Python project"), out);
    return false;
  }
  trust::TrustCreated(dir);  // YCGrid made it: it runs without asking
  s.SetStatus(TR("Python project created: main.py, sheets\\, modules\\, AI agent and VS Code settings"), 8.0);
  return true;
}

// VS Code / Pylance environment (pyrightconfig.json, .vscode\\settings.json, CodeName types): refreshed before VS Code opens,
// so the editor shows no error squiggles after moving the folder, adding sheets or changing the YCGrid version
void UpdateDevEnv(AppState& s, const std::wstring& dir) {
  std::wstring args = L"devenv \"" + dir + L"\"";
  if (s.snap) {
    for (const auto& sh : s.snap->wb.sheets) args += L" --sheet \"" + FromUtf8(sh->code_name + "=" + sh->name) + L"\"";
  }
  std::string out;
  if (!RunYcCli(s, args, &out)) {
    s.SetStatus(TR("Couldn't update the VS Code settings: ") + out, 8.0);
    ReportError("VS Code", TR("Couldn't update the VS Code settings"), out);
  }
}

}  // namespace

void StartPythonForWorkbook(AppState& s) {
  s.interactive = true;  // a script that turned input off can't leave it off across a restart
  s.cursor = -4143;      // nor a wait cursor
  if (!s.python) return;
  const std::wstring dir = ProjectDir(s);
  s.python->SetNoProject(!HasMain(dir));
  if (!HasMain(dir) && !HasProjectCode(dir)) {
    trust::NoProject();
    if (s.python->HostRunning()) s.python->StopHost();
    return;
  }
  // the Trust Center (trust.cpp): code that isn't trusted stays disabled until Enable Content (also modules without
  // a main.py: the Immediate window's host would load them)
  if (!trust::AllowProject(s, dir) || !HasMain(dir)) {
    if (s.python->HostRunning()) s.python->StopHost();
    return;
  }
  if (s.python->HostRunning() && s.python->HostAppDir() == dir) return;
  std::string err;
  if (!s.python->StartHost(dir, &err)) {
    s.SetStatus(TR("Python couldn't start: ") + err, 10.0);
    ReportError("Python", TR("Python couldn't start"), err);
  }
  s.project_stamp = ProjectStamp(dir);
}

// Alt+F11 and the other VS Code commands. The workbook and its Python code live in one folder, so a workbook that was
// never saved is saved first, where the user chooses; unsaved changes are offered for saving; a folder without main.py
// gets the project files (after asking when the folder already holds other files); then VS Code opens on the folder.
namespace {
// a question in front of everything (floating figure and sheet windows must not hide it)
int Ask(AppState& s, const std::string& text, UINT buttons) {
  ::ShowWindow(s.hwnd, ::IsIconic(s.hwnd) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(s.hwnd);
  return ::MessageBoxW(s.hwnd, FromUtf8(text).c_str(), L"YCGrid", buttons | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
}

// files in the workbook's folder other than the workbook (and its backups) and YCGrid's own .ycgrid; a few are enough
std::vector<std::wstring> OtherFiles(const std::wstring& dir, const std::wstring& workbook) {
  std::vector<std::wstring> names;
  std::error_code ec;
  const std::wstring stem = fs::path(workbook).stem().wstring();
  for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    const std::wstring name = e.path().filename().wstring();
    if (name == L".ycgrid" || e.path().stem().wstring() == stem) continue;
    names.push_back(name);
    if (names.size() >= 6) break;
  }
  return names;
}

// the workbook in a project folder with main.py (saved / created first), the VS Code settings refreshed; "" = cancelled
std::wstring PrepareProject(AppState& s) {
  if (s.file_path.empty()) {
    if (s.headless) {  // a hidden instance (tests, agents) has nobody to ask: a new folder in the projects folder
      if (!SaveWorkbookTo(s, NewProjectWorkbookPath())) return {};
    } else {
      if (Ask(s, TR("Save this workbook first: VS Code opens the workbook's folder, and its Python code (main.py, modules) is kept in that folder.\n\nA new, empty folder is best."),
              MB_OKCANCEL) != IDOK) {
        return {};
      }
      if (!SaveWorkbook(s, false)) return {};  // the Save As dialog: cancelled or failed
    }
  } else if (!ConfirmDiscard(s)) {  // unsaved changes: Yes saves, No goes on, Cancel stops
    return {};
  }
  const std::wstring dir = ProjectDir(s);
  if (!HasMain(dir)) {
    const std::vector<std::wstring> others = s.headless ? std::vector<std::wstring>{} : OtherFiles(dir, s.file_path);
    if (!others.empty()) {
      std::string list;
      for (const auto& n : others) list += "\n    " + ToUtf8(n);
      if (Ask(s, TR("This folder already has other files:\n") + ToUtf8(dir) + list +
                     TR("\n\nAdd the Python project files (main.py, modules, sheets, .vscode) to it? To keep them apart, choose Cancel and save the workbook into a new folder (File > Save As)."),
              MB_OKCANCEL) != IDOK) {
        return {};
      }
    }
    if (!CreateProject(s, dir)) return {};
    StartPythonForWorkbook(s);
  }
  UpdateDevEnv(s, dir);
  return dir;
}

// what a hidden instance would have opened in VS Code (ui.state's "vscode_request")
std::mutex g_vscode_mu;
std::string g_vscode_request;

// VS Code on the folder, with a file open (at `line` and `column` when given); false when VS Code isn't installed (any
// thread). A hidden instance (tests, agents) opens no VS Code window: every test that ran a VS Code command left a
// window open, and one VS Code process holds all its windows, so YCGrid can't close the ones it opened without closing
// the user's. It notes the request ("file:line:column" with a place).
bool LaunchVsCode(HWND owner, bool headless, const std::wstring& dir, const std::wstring& file, int line = 0, int column = 0) {
  std::wstring target = file;
  if (!file.empty() && line > 0) target += L":" + std::to_wstring(line) + (column > 0 ? L":" + std::to_wstring(column) : L"");
  if (headless) {
    std::lock_guard<std::mutex> lk(g_vscode_mu);
    g_vscode_request = ToUtf8(target.empty() ? dir : target);
    return true;
  }
  const std::wstring code = FindVsCode();
  if (code.empty()) return false;
  // no questions in VS Code either: YCGrid already runs this folder's code, so VS Code's
  // "Do you trust the authors of the files in this folder?" adds nothing
  std::wstring params = L"--disable-workspace-trust \"" + dir + L"\"";
  if (!file.empty()) params += (file.ends_with(L".py") ? L" -g \"" : L" \"") + target + L"\"";
  // lesson-ok: shell-open a hidden instance returned above (headless: the request is noted); VS Code needs its folder
  ::ShellExecuteW(owner, L"open", code.c_str(), params.c_str(), dir.c_str(), code.ends_with(L".cmd") ? SW_HIDE : SW_SHOWNORMAL);
  return true;
}

// a VS Code to open: a hidden instance opens none and notes the request instead, unless it was started as on a machine
// without VS Code (YCGRID_NO_VSCODE) and nobody has picked one yet
bool HaveVsCode(const AppState& s) {
  if (s.headless && !NoVsCodeSearch()) return true;
  return !FindVsCode().empty();
}

// the file dialog for VS Code's program ("" = cancelled; a hidden instance takes a test's queued answer)
std::wstring BrowseForVsCode(AppState& s) {
  if (std::wstring a; NoFileDialog(s.hwnd, TR("Locate VS Code"), &a)) return a;
  const std::wstring nul(1, L'\0');
  const std::wstring filter = std::wstring(L"Visual Studio Code (Code.exe)") + nul + L"Code.exe;Code - Insiders.exe;code.cmd;code-insiders.cmd" + nul +
                              TRW(L"Programs") + L" (*.exe; *.cmd)" + nul + L"*.exe;*.cmd;*.bat" + nul + nul;
  std::wstring start = Env(L"LOCALAPPDATA");
  if (!start.empty()) start = (fs::path(start) / L"Programs").wstring();
  wchar_t file[MAX_PATH * 4] = L"";
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.hwndOwner = s.hwnd;
  ofn.lpstrFilter = filter.c_str();
  ofn.lpstrFile = file;
  ofn.nMaxFile = static_cast<DWORD>(std::size(file));
  ofn.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
  ofn.lpstrTitle = TRW(L"Locate VS Code");
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
  return ::GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

// VS Code wasn't found where it is usually installed: Browse picks its program, which is kept in the settings
// (settings.json's editor.vscode, File > Options > Advanced > VS Code) and then opened (`go`); Download VS Code opens
// the download page
void LocateVsCode(AppState& s, std::function<void(AppState&)> go) {
  AskChoice(
      s, TR("VS Code not found"),
      TR("YCGrid didn't find Visual Studio Code where it is usually installed.\n\nBrowse to its program (Code.exe) and YCGrid "
         "remembers where it is (File > Options > Advanced > VS Code), or download VS Code, install it and try again."),
      TR("Browse..."),
      [&s, go] {
        const std::wstring pick = BrowseForVsCode(s);
        if (pick.empty()) return;
        std::error_code ec;
        if (!core::IsProgramPath(pick) || !fs::is_regular_file(pick, ec)) {
          s.ShowError(TR("VS Code not found"), TR("Choose VS Code's program (Code.exe):\n") + ToUtf8(pick));
          return;
        }
        Config().vscode_path = pick;
        if (!SaveSettings()) ReportError("VS Code", TR("Couldn't save the settings"), ToUtf8(SettingsPath()));
        go(s);
        s.SetStatus(TR("VS Code opened. Its place is kept in the settings (File > Options > Advanced): ") + ToUtf8(pick), 10.0);
      },
      TR("Download VS Code"),
      [&s] {
        OpenOnDesktop(L"https://code.visualstudio.com/download", L"", s.hwnd);
        s.SetStatus(TR("VS Code's download page: install VS Code, then try again."), 10.0);
      });
}

// runs `go` once there is a VS Code to open, after asking for it when it isn't found
void WithVsCode(AppState& s, std::function<void(AppState&)> go) {
  if (HaveVsCode(s)) return go(s);
  LocateVsCode(s, std::move(go));
}
}  // namespace

std::string LastVsCodeRequest() {
  std::lock_guard<std::mutex> lk(g_vscode_mu);
  return g_vscode_request;
}

void OpenVSCode(AppState& s) {
  const std::wstring dir = PrepareProject(s);
  if (dir.empty()) return;
  WithVsCode(s, [dir](AppState& s) {
    if (!LaunchVsCode(s.hwnd, s.headless, dir, (fs::path(dir) / L"main.py").wstring())) return;
    s.SetStatus(TR("Project opened in VS Code: ") + ToUtf8(dir) + TR(" (saving a file updates its functions in YCGrid)"), 10.0);
  });
}

namespace {
// a sheet's module, sheets\<CodeName>.py, in the workbook's project (made first when needed); a module not written yet
// starts with a line saying what goes in it. Empty when there is no project (the user cancelled saving the workbook).
fs::path SheetModule(AppState& s, const model::Sheet& sh, std::wstring* dir) {
  const std::string code = sh.code_name.empty() ? std::string("Sheet") + std::to_string(sh.id) : sh.code_name;
  *dir = PrepareProject(s);
  if (dir->empty()) return {};
  const fs::path file = fs::path(*dir) / L"sheets" / (FromUtf8(code) + L".py");
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  if (!fs::exists(file, ec)) {
    std::ofstream f(file, std::ios::binary);
    f << "# " << code << " (" << sh.name << "): this sheet's events (Worksheet_Change, Worksheet_SelectionChange...) and its controls' events\n";
  }
  return file;
}
}  // namespace

// View Code (the sheet tab menu, Developer > View Code): the active sheet's module, sheets\<CodeName>.py, in VS Code,
// as Excel opens the sheet's code window
void ViewSheetCode(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  std::wstring dir;
  const fs::path file = SheetModule(s, *sh, &dir);
  if (file.empty()) return;
  WithVsCode(s, [dir, path = file.wstring()](AppState& s) {
    s.SetStatus(TR("Sheet code: ") + ToUtf8(path), 10.0);
    LaunchVsCode(s.hwnd, s.headless, dir, path);
  });
}

// View Code on a control (a double-click in Design Mode, its menu, Developer > View Code with it selected), as Excel
// opens the control's event procedure: the sheet's module in VS Code with the caret in <Name>_<Event>(), added empty
// when it isn't there yet. The host loads the module again once it is written, as after any save.
void ViewControlCode(AppState& s, uint32_t sheet_id, const std::string& control, const std::string& event) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  const std::string fn = control + "_" + event;
  if (!core::IsPythonName(fn)) {
    s.ShowError(TR("View Code"), TR("The control's name can't name a Python function: ") + control +
                                     TR("\n\nRename the control (Properties > (Name)) with letters, digits and _, then try again."));
    return;
  }
  std::wstring dir;
  const fs::path file = SheetModule(s, *sh, &dir);
  if (file.empty()) return;
  std::string text;
  {
    std::ifstream in(file, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const core::HandlerPlace at = core::EnsurePythonHandler(text, fn);
  if (at.added) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
    if (!out) {
      s.ShowError(TR("View Code"), TR("Couldn't write the event procedure into ") + ToUtf8(file.wstring()));
      return;
    }
  }
  WithVsCode(s, [dir, path = file.wstring(), fn, at](AppState& s) {
    s.SetStatus(TR("Event procedure in VS Code: ") + fn + "()  " + ToUtf8(path), 10.0);
    LaunchVsCode(s.hwnd, s.headless, dir, path, at.line, at.column);
  });
}

// Developer > Debug in VS Code: the host listens for the debugger (debugpy), launch.json gets "Attach to YCGrid", VS Code opens:
// press F5 there. Breakpoints stop the Python code; the YCGrid window keeps drawing.
namespace {
void StartDebugger(AppState& s, const std::wstring& dir);
}

void DebugInVSCode(AppState& s) {
  const std::wstring dir = PrepareProject(s);
  if (dir.empty() || !s.python) return;
  WithVsCode(s, [dir](AppState& s) { StartDebugger(s, dir); });
}

namespace {
// the debugger, then VS Code to attach to it (on a worker thread: the host may still be starting)
void StartDebugger(AppState& s, const std::wstring& dir) {
  if (!s.python) return;
  if (!s.python->HostRunning()) StartPythonForWorkbook(s);
  PythonBridge* py = s.python.get();
  const HWND owner = s.hwnd;
  const bool headless = s.headless;
  s.SetStatus(TR("Starting the debugger..."), 30.0);
  std::thread([py, dir, owner, headless] {
    for (int k = 0; k < 300 && !py->HostReady(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    io::MV job = io::MV::Map();
    job.Set("kind", io::MV::Str("exec"));
    job.Set("code", io::MV::Str("import yc.debug\nyc.debug.start()[\"port\"]"));
    const io::MV r = py->RunHostJob(job, 300000);
    if (!r.GetBool("ok", false)) return VsMessage(TR("The debugger couldn't start"), r.GetStr("error") + "\n" + r.GetStr("console"));
    LaunchVsCode(owner, headless, dir, {});
    const io::MV* port = r.Get("result");
    VsMessage("", TR("Debugger ready on port ") + std::to_string(port ? port->i : 5678) + TR(": in VS Code, Run and Debug > \"Attach to YCGrid\" (F5)"));
  }).detach();
}
}  // namespace

void PollProjectChanges(AppState& s) {
  {
    std::vector<std::pair<std::string, std::string>> msgs;
    {
      std::lock_guard<std::mutex> lk(g_vs_mu);
      msgs.swap(g_vs_msgs);
    }
    for (auto& [title, text] : msgs) {
      if (title.empty()) s.SetStatus(text, 12.0);
      else s.ShowError(title, text);
    }
  }
  if (!s.python || !s.python->HostRunning()) return;
  const double now = ImGui::GetTime();
  ui::RequestRedrawIn(1.0);
  if (now < s.project_poll_at) return;
  s.project_poll_at = now + 1.0;
  const int64_t stamp = ProjectStamp(s.python->HostAppDir());
  if (s.project_stamp && stamp != s.project_stamp) {
    s.project_stamp = stamp;
    if (Config().restart_on_save) {
      s.SetStatus(TR("Code change detected, reloading Python…"));
      s.python->RestartHost();
    } else {  // hot reload: the host swaps the changed functions and classes in place (new or deleted files restart it)
      s.SetStatus(TR("Code change detected: updating the functions (state stays)"));
      s.python->NotifyHost("code_changed", io::MV::Map());
    }
  }
  s.project_stamp = stamp;
}

void RegisterPythonCommands(AppState& s) {
  auto& R = Commands();
  auto make = [](const char* id, const char* label, const char* ic, const char* shortcut, const char* desc) {
    Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  // View > Macros (Excel's View Macros) and Developer > Macro Security (the Trust Center's code settings)
  R.Add(make("ViewMacros", TR("Macros"), ui::icon::Play, "", TR("See the list of macros: run, debug or edit one."))).execute = [](Command&) { Commands().Execute("Macros"); };
  R.Add(make("MacroSecurity", TR("Macro\nSecurity"), ui::icon::Shield, "", TR("Choose when a workbook's Python code may run (Trust Center)."))).execute =
      [](Command&) { Commands().Execute("TrustCenterSettings"); };
  R.Add(make("ViewCode", TR("View Code"), ui::icon::ViewCode, "",
             TR("Open this sheet's Python module (its events and its controls' events) in VS Code; with a control selected, at its event procedure.")))
      .execute = [&s](Command&) {
    if (!ViewSelectedControlCode(s)) ViewSheetCode(s);
  };
  R.Add(make("OpenVSCode", "VS Code", ui::icon::Code, "Alt+F11", TR("Open this workbook's Python project in VS Code (saving the workbook and creating the project first if needed)."))).execute = [&s](Command&) { OpenVSCode(s); };
  R.Add(make("DebugInVSCode", TR("Debug in\nVS Code"), ui::icon::Bug, "",
             TR("Attach VS Code's debugger to this YCGrid's Python: breakpoints in strategies, handlers and UDFs while the window keeps drawing.")))
      .execute = [&s](Command&) { DebugInVSCode(s); };
  R.Add(make("Macros", TR("Macros"), ui::icon::Play, "Alt+F8", TR("Run the functions without parameters in modules\\ and main.py."))).execute = [&s](Command&) { s.OpenDialog(Dialog::Macros); };
  R.Add(make("ReloadPython", TR("Reload"), ui::icon::Reload, "", TR("Restart the Python host (saving a file already updates its functions without a restart)."))).execute = [&s](Command&) {
    if (!s.python) return;
    if (s.python->HostRunning()) s.python->RestartHost();
    else StartPythonForWorkbook(s);
  };
  R.Add(make("ImmediateWindow", TR("Immediate"), ui::icon::Console, "", TR("Show Debug.Print and print output and errors; you can type Python directly."))).execute = [&s](Command&) { s.show_console = !s.show_console; };
  RegisterPane("immediate", "ImmediateWindow", [&s]() -> bool& { return s.show_console; });
}

float DrawPackageBar(AppState& s, const ImRect& r, bool measure_only) {
  if (!s.python) return 0;
  const auto bar = s.python->PackageBar();
  if (bar.packages.empty()) return 0;
  const float h = S(34);
  if (measure_only) return h;
  const bool failed = !bar.error.empty() && !bar.installing;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(r.Min, ImVec2(r.Max.x, r.Min.y + h), failed ? IM_COL32(0xFD, 0xE7, 0xE9, 255) : IM_COL32(0xFF, 0xF4, 0xCE, 255));
  dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + h - 1), ImVec2(r.Max.x, r.Min.y + h),
                    failed ? IM_COL32(0xE0, 0x90, 0x90, 255) : IM_COL32(0xE0, 0xC0, 0x60, 255));
  std::string list;
  for (const auto& m : bar.packages) list += (list.empty() ? "" : TR(", ")) + m;
  ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(12), r.Min.y + S(6)));
  ImGui::AlignTextToFramePadding();
  if (bar.installing) {  // no buttons while it works: the host restarts with the packages, or the bar says why not
    ImGui::TextUnformatted((TR("Installing Python packages: ") + list + TR("...")).c_str());
    return h;
  }
  if (failed) {
    // the reason in a line (the Error Log keeps it whole), and the buttons after it
    std::string why = bar.error;
    if (const size_t nl = why.find('\n'); nl != std::string::npos) why.resize(nl);
    if (core::Utf8Length(why) > 140) why = std::string(core::Utf8First(why, 140)) + "...";
    ImGui::TextUnformatted((TR("Couldn't install Python packages: ") + list + " (" + why + ")").c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Try Again"))) s.python->InstallPackages(false);
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Skip"))) s.python->DismissMissing();
    return h;
  }
  ImGui::TextUnformatted((TR("Missing Python packages: ") + list).c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton(TR("Install"))) s.python->InstallPackages(false);
  ImGui::SameLine();
  if (ImGui::SmallButton(TR("Always install"))) s.python->InstallPackages(true);
  ImGui::SameLine();
  if (ImGui::SmallButton(TR("Skip"))) s.python->DismissMissing();
  return h;
}

namespace {

// the Immediate window works in any workbook: without a running Python host (no main.py yet, or a workbook
// never saved) one is started for it, in the workbook's folder or a scratch folder; no project files are created
void RunImmediate(AppState& s, const std::string& code, bool shown = true) {
  if (!s.python->HostRunning()) {
    std::wstring dir = ProjectDir(s);
    std::error_code ec;
    if (!dir.empty() && !fs::is_directory(dir, ec)) {
      // the workbook's folder was moved or deleted since it was opened: Python can't start there, it runs in the
      // scratch folder (and says so) instead of failing with a system error number
      s.python->LogSystem(TR("[YCGrid] The workbook's folder no longer exists; Python runs in a scratch folder."));
      dir.clear();
    }
    if (trust::CodeDisabled()) dir.clear();  // the project's code is disabled: not even its folder's modules are importable
    if (dir.empty()) {
      const fs::path scratch = fs::path(TempFolder()) / L"YCGrid-immediate";
      fs::create_directories(scratch, ec);
      dir = scratch.wstring();
    }
    s.python->LogSystem(TR("[YCGrid] Starting Python for the Immediate window..."));
    std::string err;
    if (!s.python->StartHost(dir, &err)) {
      s.python->LogError(std::string(TR("[YCGrid] Python couldn't start: ")) + err);
      return;
    }
    s.project_stamp = ProjectStamp(dir);
  }
  s.python->ConsoleEval(code, shown);
}

}  // namespace

void RunPythonInHost(AppState& s, const std::string& code, bool shown) {
  if (s.python) RunImmediate(s, code, shown);
}

void DrawPythonUi(AppState& s) {
  if (!s.python || !s.show_console) return;
  ImGui::SetNextWindowSize(ImVec2(S(720), S(300)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Bottom);
  if (ImGui::Begin(TR("Immediate###yc_console"), &s.show_console)) {
    const auto lines = s.python->ConsoleSnapshot();
    const float input_h = ImGui::GetFrameHeightWithSpacing() + S(4);
    ImGui::BeginChild("##log", ImVec2(0, -input_h), ImGuiChildFlags_Borders);
    ImGui::PushFont(ui::F().grid, 14.0f);
    static size_t last_count = 0;
    if (lines.empty()) {
      // how to use it, until something is printed
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x80, 0x80, 0x80, 255));
      ImGui::TextUnformatted(TR("Type Python below and press Enter. Examples:"));
      ImGui::TextUnformatted("  ?Range(\"A1\").Value");
      ImGui::TextUnformatted("  Range(\"A1\").Value = 123");
      ImGui::TextUnformatted("  for i in range(1, 11): Cells(i, 2).Value = i * i");
      ImGui::TextUnformatted("  ActiveSheet.Name");
      ImGui::TextUnformatted(TR("print() and Debug.Print output from your macros and events also shows here."));
      ImGui::PopStyleColor();
    }
    for (const auto& ln : lines) {
      const ImU32 col = ln.kind == 1 ? IM_COL32(0xC0, 0x20, 0x20, 255) : ln.kind == 2 ? IM_COL32(0x40, 0x60, 0xA0, 255) : IM_COL32(0x20, 0x20, 0x20, 255);
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      ImGui::TextUnformatted(ln.text.c_str());
      ImGui::PopStyleColor();
    }
    if (lines.size() != last_count) {
      ImGui::SetScrollHereY(1.0f);
      last_count = lines.size();
    }
    ImGui::PopFont();
    ImGui::EndChild();
    static char buf[1024];
    static CodeComplete complete;
    ImGui::SetNextItemWidth(-S(70));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();  // type right away after opening it
    if (CodeInput(s, "##eval", TR("Type Python (? expression shows the result), Tab completes, Enter runs"), buf, sizeof(buf), complete) && buf[0]) {
      if (!trust::ImmediateBlocked(s)) RunImmediate(s, buf);  // the project's code is disabled: Excel's message instead
      buf[0] = 0;
      ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"))) s.python->ClearConsole();
  }
  ImGui::End();
}

void DrawPythonDialogs(AppState& s) {
  // InputBox (Python InputBox / Application.InputBox)
  if (s.dialog == Dialog::InputBox && s.dialog_open_request) {
    ImGui::OpenPopup((s.inputbox_title + "###yc_inputbox").c_str());
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (BeginModal((s.inputbox_title + "###yc_inputbox").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PushTextWrapPos(S(420));
    ImGui::TextUnformatted(s.inputbox_prompt.c_str());
    ImGui::PopTextWrapPos();
    static char buf[1024];
    if (ImGui::IsWindowAppearing()) {
      std::snprintf(buf, sizeof(buf), "%s", s.inputbox_text.c_str());
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(S(420));
    const bool enter = ImGui::InputText("##in", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool ok = ImGui::Button(TR("OK"), ImVec2(S(88), 0)) || enter;
    ImGui::SameLine();
    const bool cancel = ImGui::Button(TR("Cancel"), ImVec2(S(88), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (ok || cancel) {
      if (s.inputbox_done) s.inputbox_done(ok, buf);
      s.inputbox_done = nullptr;
      s.dialog = Dialog::None;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  // Macros (Alt+F8)
  if (s.dialog == Dialog::Macros && s.dialog_open_request) {
    ImGui::OpenPopup(TR("Macros###yc_macros"));
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(460), S(380)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (BeginModal(TR("Macros###yc_macros"), &open, ImGuiWindowFlags_NoSavedSettings)) {
    static int selected = 0;
    const auto macros = s.python ? s.python->Macros() : std::vector<std::string>{};
    if (!s.python || !s.python->HostReady()) {
      ImGui::TextWrapped(TR("Python isn't running. This workbook's folder needs a main.py (press Alt+F11 to create the project)."));
    } else if (macros.empty()) {
      ImGui::TextWrapped(TR("No macros. Functions without parameters in modules\\*.py or main.py show up here."));
    }
    ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - S(6)), ImGuiChildFlags_Borders);
    for (int i = 0; i < static_cast<int>(macros.size()); ++i) {
      if (ImGui::Selectable(macros[static_cast<size_t>(i)].c_str(), selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
        selected = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          s.python->RunMacro(macros[static_cast<size_t>(i)]);
          ImGui::CloseCurrentPopup();
          s.dialog = Dialog::None;
        }
      }
    }
    ImGui::EndChild();
    const bool run = ImGui::Button(TR("Run"), ImVec2(S(88), 0)) && selected < static_cast<int>(macros.size());
    if (run) {
      s.python->RunMacro(macros[static_cast<size_t>(selected)]);
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Edit"), ImVec2(S(88), 0))) OpenVSCode(s);
    ImGui::SameLine();
    const bool close = ImGui::Button(TR("Close"), ImVec2(S(88), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (run || close || !open) {
      ImGui::CloseCurrentPopup();
      s.dialog = Dialog::None;
    }
    ImGui::EndPopup();
  }
}

}  // namespace yc::app
