# Contributing to YCGrid

Thank you for helping. YCGrid is a real-time spreadsheet for research and trading screens: a C++ front end
(Dear ImGui + ImPlot) that works like Excel, driven by Python with Excel's object model names. This guide covers what
to work on, how to build and test, and what a pull request needs. [AGENTS.md](AGENTS.md) holds the full rules
(for people and AI agents alike) and the lessons learned from past bugs; read the parts about the area you change.

## Before you start

- **Bugs** are always welcome. Open an issue with the steps, or a pull request with the fix and its test.
- **Excel behavior**: YCGrid follows Excel's habits where people work every day (keys, KeyTips, menus, ribbon,
  dialogs). If YCGrid does something differently from Excel, that is a bug unless
  [docs/excel-parity-backlog.md](docs/excel-parity-backlog.md) lists it under "Kept on purpose" or "Not planned".
  Please read those two lists before proposing a feature; the reasons are there.
- **Building blocks, not finished screens**: YCGrid ships low-level capabilities that need the C++ side, speed or
  stability (rendering, data paths, the engine). Order managers, strategies, scanners, journals and similar screens
  are built by developers from those blocks, so they belong in examples or your own project, not in the library.
- **Larger changes**: open an issue first so the design can be agreed before you spend time on it. The main
  specification is [docs/YCGRID_PLAN.md](docs/YCGRID_PLAN.md); decisions that changed it are in [docs/adr/](docs/adr/).
- **Security problems**: don't open a public issue; see [SECURITY.md](SECURITY.md).

## Setting up

You need Windows 10 or 11 (64-bit). In the repository folder run:

```bat
setup.cmd /noinstall
```

It installs what is missing with winget (asking first): Git and the Visual Studio 2026 Build Tools with the C++
tools and vcpkg. Then it fetches ImGui and ImPlot at the pinned commits, sets up YCGrid's own portable Python
(the system Python is never touched), builds and runs the unit tests. The result is in `dist_release\`.

With Visual Studio and Git already installed, step by step:

```bat
git clone --recursive https://github.com/ycgrid/ycgrid.git YCGrid
cd YCGrid
scripts\setup_runtime.cmd
scripts\build.cmd release
```

To try your build as an installed YCGrid, double-click `build_release.cmd` (it builds through `scripts\build.cmd`,
then opens `dist_release\`), close YCGrid and double-click `install.cmd` there.

Always build through `scripts\build.cmd` (`release` or `debug`, `/silent` for no prompts): it sets the console code
page the compiler's dependency tracking needs.

## Making a change

1. Fork the repository and branch from `dev`; pull requests go to `dev`. `main` holds the released versions.
2. Make the change with its tests (below). Keep a pull request to one topic.
3. Run the checks, then open the pull request and fill in its template.

### Tests

Every fix comes with a test that fails without it, and every feature is checked by its result, not only for not
crashing.

| What you change | Where its test goes |
|---|---|
| The engine, formulas, file format (C++) | Catch2 tests in `tests/unit/`, run by `scripts\build.cmd` |
| The `yc` Python package | `python/tests/` (`test_core.py` for parts that need no running YCGrid) |
| UI behavior | a test that drives a hidden YCGrid (`yc.launch(hidden=True)`) with keys and clicks and reads the result; see [skill/ycgrid-python/references/testing.md](skill/ycgrid-python/references/testing.md) |
| A new UI command | its check in `scripts/feature_sections.py` (the run fails while a command has none) |

Tests run YCGrid hidden and read its own screenshots (`yc.ui.screenshot()`); they never capture your screen.

### Checks

```bat
scripts\check_all.cmd /quick     :: build, unit tests, Python tests, translations, command coverage, boundary, model fields, lessons
scripts\check_all.cmd            :: everything: also the UI sweep, the feature checks, a timed trader session, chaos seeds
```

Run the full `check_all.cmd` once your change is finished (it runs its slow parts side by side, each in its own
hidden YCGrid). GitHub runs the fast checks that need no build on every pull request.

### Rules that catch people out

- **English in the repository**: code, comments, docs, string literals and commit messages. Chinese lives only in
  `i18n/zh-TW.json`, `README.zh-TW.md` and tests of CJK behavior.
- **UI text** is written as `TR("English")`. After adding strings run `python scripts/i18n_build.py extract`, add the
  Traditional Chinese in `i18n/zh-TW.json`, then `python scripts/i18n_build.py build`; `check` must report 0 missing.
  If you can't write the Chinese, say so in the pull request and a maintainer adds it.
- **Names** follow the table in [AGENTS.md](AGENTS.md#naming): the product is always `YCGrid`, code identifiers use the
  `yc` prefix, and no name contains "Excel".
- **Every UI feature is reachable from Python** with Excel's name: add the command's Python entry point to
  `schema/commands.yaml` (`python scripts/check_commands.py --write` checks it) and document new Python members in
  [skill/ycgrid-python/references/object-model.md](skill/ycgrid-python/references/object-model.md).
- **`.ycg` files stay backward compatible**: new saved state goes into new fields or chunks, with a save / load
  round-trip test ([docs/ycg-format.md](docs/ycg-format.md)).
- **Scripts** are `.cmd`, never PowerShell: UTF-8 without BOM, CRLF line endings, `chcp 65001 >nul` on line 2.
- **Dependencies** are pinned in `third_party/deps.lock` and `vcpkg.json`; a new one needs a reason in the pull
  request and its license added to [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
- **A new source file** (C++, Python, `.cmd`, CMake) starts with the license line
  `SPDX-License-Identifier: Apache-2.0` in a comment (`//`, `#`, or `rem` on line 3 of a `.cmd`, after `chcp`);
  `scripts/check_boundary.py` fails without it. Generated files say so in their first line instead.

## Commit messages

One line saying what changes for the user (`The title bar's X closes YCGrid again`), then a blank line and the
details when they help: what was wrong, why, how it was tested.

## Releases

Maintainers publish a version as [docs/releasing.md](docs/releasing.md) describes: the version in five files, the
changelog, the checks, the zip and the wheel with their checksums, the tag and the release page.

## License

YCGrid is licensed under the [Apache License 2.0](LICENSE). By submitting a contribution you agree that it is
licensed under the same terms (section 5 of the license); there is no separate agreement to sign.

## Conduct

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
