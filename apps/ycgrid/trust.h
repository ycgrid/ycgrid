// SPDX-License-Identifier: Apache-2.0
// trust.h - the Trust Center for a workbook's Python code (PLAN §13.15 (e)), the way Excel's macro security works
//
// A workbook's project (main.py, sheets\, modules\ in its folder) runs only when it is trusted: the folder is in a
// Trusted Location, the workbook was made a Trusted Document (Enable Content), it was enabled for this session, or the
// macro setting enables all code. Otherwise its code stays disabled (no Workbook_Open, no events, Python functions
// give #NAME?, =PY gives #BLOCKED!) and the yellow Message Bar under the ribbon offers Enable Content.
// Workbooks opened by scripts (yc.launch, Workbooks.Open, MCP) follow Application.AutomationSecurity instead
// (msoAutomationSecurityLow by default, as in Excel); a hidden instance always does (nobody could click the bar).
//
// Settings: settings.json "trust_center" (settings.h); Trusted Documents: %LOCALAPPDATA%\ycgrid\trust.json (hidden
// instances keep both in memory and start from the defaults).
#pragma once

#include "io/msgvalue.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <string>

namespace yc::app {

struct AppState;

namespace trust {

// Application.AutomationSecurity: Office's MsoAutomationSecurity values
enum : int { kAutomationLow = 1, kAutomationByUI = 2, kAutomationForceDisable = 3 };

// main, after LoadSettings: hidden instance, started by a script (yc.launch passes --automation-security), its value
void Init(bool hidden, bool launched_by_automation, int automation_security);
void StartupDone();  // the command line's workbook is open: later opens from the UI are a person's
int AutomationSecurity();
void SetAutomationSecurity(int v);
// OpenWorkbook: the workbook now opening was opened by a script (Python, MCP) or by a person
void NoteOpen(bool automation);
// a script is opening a workbook in another window (Workbooks.Open): the new window follows AutomationSecurity
struct AutomationScope {
  AutomationScope();
  ~AutomationScope();
};
std::wstring ChildArgs();  // " --automation-security N" for a window this one starts, or ""

// StartPythonForWorkbook: may the project in dir run? false: its code stays disabled (the Message Bar shows when the
// settings say so). The answer is kept until the next workbook opens.
bool AllowProject(AppState& s, const std::wstring& dir);
void NoProject();      // the workbook has no Python code
bool CodeDisabled();   // the open workbook's code is disabled (also in Protected View)

// Protected View (Excel's): OpenWorkbook calls OpenedFile for every workbook it opens ("" = a new one). A file with the
// Mark of the Web (Zone.Identifier ZoneId 3 / 4), in a browser cache or an Outlook attachment folder opens read-only
// with nothing of its code running, unless it is in a trusted location or its editing was enabled before
void OpenedFile(AppState& s, const std::wstring& path);
bool InProtectedView();
void EnableEditing(AppState& s);        // Enable Editing (the bar, File > Info, ProtectedViewWindow.Edit)
void ProtectedViewBlocked(AppState& s);  // an edit was tried: the status bar says how to edit, the bar comes back
std::string ProtectedViewReason(const std::wstring& path);  // "internet", "unsafe_location", "outlook" or ""
int ZoneOf(const std::wstring& path);   // the Mark of the Web's ZoneId (0 = none)
// Enable Content: always = make it a Trusted Document (the Message Bar, File > Info > Enable All Content); false = this
// session only (File > Info > Advanced Options)
void EnableContent(AppState& s, bool always);
void TrustCreated(const std::wstring& dir);  // YCGrid made this project (Alt+F11, the VS Code button): trusted
// a macro (a button's OnAction) clicked while the code is disabled: Excel's "Cannot run the macro" message; false when
// the code isn't disabled
bool MacroBlocked(AppState& s, const std::string& macro);
// the Immediate window's input while the code is disabled: Excel's message instead of running it
bool ImmediateBlocked(AppState& s);

float DrawMessageBar(AppState& s, const ImRect& r, bool measure_only);  // under the ribbon; returns its height
void DrawInfoSection(AppState& s);     // File > Info: Security Warning
void DrawOptionsSection(AppState& s);  // File > Options: Trust Center
void DrawDialogs(AppState& s);         // Trust Center, Security Options
void RegisterCommands(AppState& s);

io::MV StateMV();  // ui.state()["trust"], python.status
// [YC] Application.TrustCenter: get (nil params) or change the Trust Center settings; err on a refused value
io::MV CenterRequest(AppState& s, const io::MV& p, std::string* err);

// helpers (unit tests)
std::wstring NormalizeDir(const std::wstring& path);  // absolute, no trailing backslash, lower case
bool InLocation(const std::wstring& dir, const std::wstring& location, bool subfolders);
std::wstring ExpandLocation(const std::string& stored);  // %YCGRID_DIR%, %YCGRID_PROJECTS%, %VARIABLES%

}  // namespace trust
}  // namespace yc::app
