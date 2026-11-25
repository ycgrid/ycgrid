// SPDX-License-Identifier: Apache-2.0
// paths.h - showing file paths without the user's own folders
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yc::core {

// A folder that can stand for the start of a path: {"%TEMP%", "C:\Users\<you>\AppData\Local\Temp"}.
using PathRoot = std::pair<std::string, std::string>;

// Replaces the longest root that starts `path` with its name: "C:\Users\<you>\AppData\Local\Temp\YCGrid-immediate" with
// the root above gives "%TEMP%\YCGrid-immediate". Roots match without regard to ASCII case and only at a folder
// boundary ("C:\Temp2" doesn't start with "C:\Temp"); a trailing separator on a root is ignored. Paths are UTF-8.
// Messages that show a folder use it, so screenshots and pasted logs don't reveal the user name or folder layout.
std::string AbbreviatePath(std::string_view path, const std::vector<PathRoot>& roots);

// Where Alt+F11 looks for VS Code: the folders and the places Windows lists it, as the environment gives them.
struct VsCodeSearch {
  std::wstring path;  // %PATH%
  std::wstring local_app_data, program_files, program_files_x86, program_data, user_profile, scoop;
  // what Windows says about an installed VS Code (App Paths, Uninstall's InstallLocation, the vscode: link's and
  // "Open with" commands): a program, a folder holding Code.exe, or a command line starting with the program
  std::vector<std::wstring> registered;
};

// The programs VS Code may be, most likely first: code.cmd on PATH, the user and system installers' folders, Scoop,
// what Windows lists, then the same for VS Code Insiders. Without repeats (ASCII case ignored); nothing is checked on
// disk: the caller takes the first one that exists.
std::vector<std::wstring> VsCodeCandidates(const VsCodeSearch& in);

// The program a command line starts: `"C:\VS Code\Code.exe" --open-url -- "%1"` gives `C:\VS Code\Code.exe`.
std::wstring CommandProgram(std::wstring_view command);

// A file a person may pick as VS Code: a program (.exe) or a command script (.cmd, .bat).
bool IsProgramPath(std::wstring_view path);

}  // namespace yc::core
