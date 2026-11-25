// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "app/shell_integration.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace yc::app::shell;

// Install's desktop and Start menu shortcuts, made in a folder of the test's own: the test never writes the user's
// desktop, Start menu or registry.
TEST_CASE("Install's shortcuts start YCGrid, and uninstall removes only a shortcut that starts YCGrid") {
  const fs::path dir = fs::temp_directory_path() / ("yc_shortcuts_" + std::to_string(::GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "app");
  const fs::path exe = dir / "app" / "ycgrid.exe";
  { std::ofstream(exe, std::ios::binary) << "MZ"; }
  const std::wstring lnk = (dir / "YCGrid.lnk").wstring();

  REQUIRE(CreateShortcut(lnk, exe.wstring()));
  CHECK(fs::exists(lnk));
  CHECK(fs::equivalent(ShortcutTarget(lnk), exe));

  // made again over the old one (a reinstall, or the association moved): it starts the new place
  const fs::path moved = dir / "moved" / "ycgrid.exe";
  fs::create_directories(moved.parent_path());
  { std::ofstream(moved, std::ios::binary) << "MZ"; }
  REQUIRE(CreateShortcut(lnk, moved.wstring()));
  CHECK(fs::equivalent(ShortcutTarget(lnk), moved));

  // uninstall removes it, and nothing left counts as removed too
  CHECK(RemoveYcGridShortcut(lnk));
  CHECK_FALSE(fs::exists(lnk));
  CHECK(RemoveYcGridShortcut(lnk));
  CHECK(RemoveYcGridShortcut(L""));

  // a YCGrid.lnk of the user's that starts another program stays, and so does a file that isn't a shortcut
  const fs::path other = dir / "notes.exe";
  { std::ofstream(other, std::ios::binary) << "MZ"; }
  REQUIRE(CreateShortcut(lnk, other.wstring()));
  CHECK_FALSE(RemoveYcGridShortcut(lnk));
  CHECK(fs::exists(lnk));
  fs::remove(lnk);
  { std::ofstream(lnk, std::ios::binary) << "not a shortcut"; }
  CHECK(ShortcutTarget(lnk).empty());
  CHECK_FALSE(RemoveYcGridShortcut(lnk));
  CHECK(fs::exists(lnk));

  fs::remove_all(dir, ec);
}

// A double-clicked .ycg goes to the YCGrid it belongs to: the ycgrid.exe beside it, or for one of a YCGrid folder's
// examples (<YCGrid>/examples/<name>/<name>.ycg) that folder's ycgrid.exe. An older YCGrid registered for .ycg files
// didn't know the examples' shared code (examples/lib) and offered it as missing packages to install.
TEST_CASE("A double-clicked example opens in the YCGrid it ships with") {
  const fs::path dir = fs::temp_directory_path() / ("yc_handoff_" + std::to_string(::GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "YCGrid" / "examples" / "backtest_report");
  fs::create_directories(dir / "projects" / "desk");
  fs::create_directories(dir / "other" / "examples" / "x");
  const fs::path exe = dir / "YCGrid" / "ycgrid.exe";
  { std::ofstream(exe, std::ios::binary) << "MZ"; }
  const fs::path example = dir / "YCGrid" / "examples" / "backtest_report" / "backtest_report.ycg";
  CHECK(fs::equivalent(FolderExeFor(example.wstring()), exe));
  CHECK(fs::equivalent(FolderExeFor((dir / "YCGrid" / "Book1.ycg").wstring()), exe));  // beside ycgrid.exe, as before
  CHECK(FolderExeFor((dir / "projects" / "desk" / "desk.ycg").wstring()).empty());      // a project: the registered one
  CHECK(FolderExeFor((dir / "other" / "examples" / "x" / "x.ycg").wstring()).empty());  // an examples folder without one
  // a project's own ycgrid.exe (a packed project) wins over the YCGrid folder above it
  const fs::path packed = dir / "YCGrid" / "examples" / "backtest_report" / "ycgrid.exe";
  { std::ofstream(packed, std::ios::binary) << "MZ"; }
  CHECK(fs::equivalent(FolderExeFor(example.wstring()), packed));
  fs::remove_all(dir, ec);
}
