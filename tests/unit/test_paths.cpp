// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "core/paths.h"

using namespace yc::core;

TEST_CASE("AbbreviatePath replaces the longest matching root") {
  const std::vector<PathRoot> roots = {{"%USERPROFILE%", "C:\\Users\\me"},
                                       {"%LOCALAPPDATA%", "C:\\Users\\me\\AppData\\Local"},
                                       {"%TEMP%", "C:\\Users\\me\\AppData\\Local\\Temp\\"}};
  CHECK(AbbreviatePath("C:\\Users\\me\\AppData\\Local\\Temp\\YCGrid-immediate", roots) == "%TEMP%\\YCGrid-immediate");
  CHECK(AbbreviatePath("C:\\Users\\me\\AppData\\Local\\ycgrid\\logs", roots) == "%LOCALAPPDATA%\\ycgrid\\logs");
  CHECK(AbbreviatePath("C:\\Users\\me\\Documents\\Desk", roots) == "%USERPROFILE%\\Documents\\Desk");
  CHECK(AbbreviatePath("C:\\Users\\me", roots) == "%USERPROFILE%");
}

TEST_CASE("AbbreviatePath ignores ASCII case and separator style, and matches whole folders only") {
  const std::vector<PathRoot> roots = {{"%TEMP%", "C:\\Temp"}};
  CHECK(AbbreviatePath("c:\\temp\\a.ycg", roots) == "%TEMP%\\a.ycg");
  CHECK(AbbreviatePath("C:/Temp/a.ycg", roots) == "%TEMP%/a.ycg");
  CHECK(AbbreviatePath("C:\\Temp2\\a.ycg", roots) == "C:\\Temp2\\a.ycg");
  CHECK(AbbreviatePath("D:\\Projects\\Desk", roots) == "D:\\Projects\\Desk");
  CHECK(AbbreviatePath("", roots).empty());
  CHECK(AbbreviatePath("C:\\Temp\\x", {}) == "C:\\Temp\\x");
  CHECK(AbbreviatePath("C:\\Temp\\x", {{"%EMPTY%", ""}, {"%SEP%", "\\"}}) == "C:\\Temp\\x");
}

TEST_CASE("VsCodeCandidates looks where VS Code's installers, PATH, Scoop and Windows put it, most likely first") {
  VsCodeSearch in;
  in.path = L"C:\\Windows;\"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\bin\";;C:\\Tools\\";
  in.local_app_data = L"C:\\Users\\me\\AppData\\Local";
  in.program_files = L"C:\\Program Files";
  in.program_files_x86 = L"C:\\Program Files (x86)";
  in.program_data = L"C:\\ProgramData";
  in.user_profile = L"C:\\Users\\me";
  in.registered = {L"D:\\Apps\\VS Code\\",                                   // Uninstall's InstallLocation
                   L"\"E:\\Portable\\Code.exe\" --open-url -- \"%1\"",       // the vscode: link
                   L"c:\\program files\\microsoft vs code\\code.exe"};      // App Paths: already listed
  const std::vector<std::wstring> c = VsCodeCandidates(in);
  const std::vector<std::wstring> want = {
      L"C:\\Windows\\code.cmd",
      L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\bin\\code.cmd",
      L"C:\\Tools\\code.cmd",
      L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe",
      L"C:\\Program Files\\Microsoft VS Code\\Code.exe",
      L"C:\\Program Files (x86)\\Microsoft VS Code\\Code.exe",
      L"C:\\Users\\me\\scoop\\apps\\vscode\\current\\Code.exe",
      L"C:\\ProgramData\\scoop\\apps\\vscode\\current\\Code.exe",
      L"D:\\Apps\\VS Code\\Code.exe",
      L"E:\\Portable\\Code.exe",
      L"C:\\Windows\\code-insiders.cmd",
      L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code\\bin\\code-insiders.cmd",
      L"C:\\Tools\\code-insiders.cmd",
      L"C:\\Users\\me\\AppData\\Local\\Programs\\Microsoft VS Code Insiders\\Code - Insiders.exe",
      L"C:\\Program Files\\Microsoft VS Code Insiders\\Code - Insiders.exe",
      L"C:\\Program Files (x86)\\Microsoft VS Code Insiders\\Code - Insiders.exe",
      L"C:\\Users\\me\\scoop\\apps\\vscode-insiders\\current\\Code - Insiders.exe",
      L"C:\\ProgramData\\scoop\\apps\\vscode-insiders\\current\\Code - Insiders.exe"};
  CHECK(c == want);
}

TEST_CASE("VsCodeCandidates leaves out the places the environment doesn't name") {
  VsCodeSearch in;
  in.scoop = L"D:\\scoop";
  const std::vector<std::wstring> c = VsCodeCandidates(in);
  REQUIRE(c.size() == 2);
  CHECK(c[0] == L"D:\\scoop\\apps\\vscode\\current\\Code.exe");
  CHECK(c[1] == L"D:\\scoop\\apps\\vscode-insiders\\current\\Code - Insiders.exe");
  CHECK(VsCodeCandidates({}).empty());
}

TEST_CASE("CommandProgram takes the program a command line starts") {
  CHECK(CommandProgram(L"\"C:\\Program Files\\Microsoft VS Code\\Code.exe\" --open-url -- \"%1\"") ==
        L"C:\\Program Files\\Microsoft VS Code\\Code.exe");
  CHECK(CommandProgram(L"  C:\\Program Files\\Microsoft VS Code\\Code.exe \"%1\"") == L"C:\\Program Files\\Microsoft VS Code\\Code.exe");
  CHECK(CommandProgram(L"C:\\VSCode\\Code.EXE") == L"C:\\VSCode\\Code.EXE");
  CHECK(CommandProgram(L"code %1") == L"code");
  CHECK(CommandProgram(L"\"unterminated") == L"unterminated");
  CHECK(CommandProgram(L"").empty());
}

TEST_CASE("IsProgramPath takes programs and command scripts only") {
  CHECK(IsProgramPath(L"C:\\x\\Code.exe"));
  CHECK(IsProgramPath(L"C:\\x\\bin\\code.CMD"));
  CHECK(IsProgramPath(L"run.bat"));
  CHECK_FALSE(IsProgramPath(L"C:\\x\\Code.exe.lnk"));
  CHECK_FALSE(IsProgramPath(L"C:\\x\\README.md"));
  CHECK_FALSE(IsProgramPath(L""));
}
