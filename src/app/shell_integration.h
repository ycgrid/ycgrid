// SPDX-License-Identifier: Apache-2.0
// shell_integration.h - .ycg file association, desktop and Start menu shortcuts, startup hand-off (PLAN §13.16)
//
// Rule 15 (revised): ycgrid.exe never writes the registry during normal use. Only when the user runs install.cmd (--install)
// or explicitly asks for it in Options is HKCU written (current user, no administrator needed); --uninstall removes everything.
#pragma once

#include <windows.h>

#include <string>

namespace yc::app::shell {

// What --install does with the desktop shortcut: makes it (install.cmd), leaves it out (install.cmd /nodesktop), or
// only points one already there at this exe (the association moved: a shortcut the user deleted stays deleted).
enum class DesktopShortcut { Create, Skip, UpdateIfThere };

// --install: registers HKCU\Software\Classes\.ycg and related keys, creates the Start menu shortcut and the desktop
// shortcut. Returns the process exit code.
int Install(bool silent, DesktopShortcut desktop = DesktopShortcut::Create);
// --uninstall: removes every key and shortcut created by Install.
int Uninstall(bool silent);

// Shortcuts (.lnk): `lnk` starting `target` (its folder as the working folder, its icon); the program a shortcut starts
// ("" when it isn't a shortcut); and the removal of `lnk` only when it starts a ycgrid.exe (a file the user put there
// under that name stays). RemoveYcGridShortcut is true when no such shortcut is left.
bool CreateShortcut(const std::wstring& lnk, const std::wstring& target);
std::wstring ShortcutTarget(const std::wstring& lnk);
bool RemoveYcGridShortcut(const std::wstring& lnk);

// Hand-off when a .ycg is double-clicked: if the file's folder has its own ycgrid.exe that isn't this one, or the
// file is one of a YCGrid folder's examples (<YCGrid>\examples\<name>\<name>.ycg) and that folder's ycgrid.exe
// isn't this one, pass the file to it and return true (the caller should exit right away). The examples are made
// for the YCGrid they ship with: an older YCGrid installed for .ycg files didn't know their shared code.
bool HandOffToFolderExe(const std::wstring& file);
// The ycgrid.exe a .ycg belongs to (HandOffToFolderExe's rule), "" when none: exported for the unit tests.
std::wstring FolderExeFor(const std::wstring& file);

// An instance for the same folder is already running: pass the file to it (WM_COPYDATA), bring it to the front, return true.
bool ForwardToRunningInstance(const std::wstring& app_dir, const std::wstring& file);
// Register this instance as the folder's instance (named mutex, property on the window).
void RegisterInstance(HWND hwnd, const std::wstring& app_dir);

// When the associated exe has been moved or deleted, ask whether to point the association at this exe.
void CheckAssociationMoved(bool silent);

std::wstring ExePath();
std::wstring ExeDir();
std::wstring ParentDir(const std::wstring& path);

inline constexpr wchar_t kWindowClass[] = L"YCGrid.MainWindow";
inline constexpr ULONG_PTR kCopyDataOpenFile = 0x59434F50;  // 'YCOP'

}  // namespace yc::app::shell
