# Developing and testing YCGrid itself

For contributors to the YCGrid repository (the C++ program, the `yc` package, the MCP tools and this skill). Building
an app on YCGrid is [app-development.md](app-development.md); testing an app is [testing.md](testing.md). The rules
of the repository (English everywhere in it, the naming table, `.cmd` scripts, building blocks only, `.ycg` backward
compatibility, Excel's names) are in `AGENTS.md` at the repository root, with its "Lessons from bugs": read both before
changing code. `CONTRIBUTING.md` is the short version for people new to the project.

## 1. Where things are

| Path | What |
|---|---|
| `apps/ycgrid/` | the program: UI (ImGui), commands, dialogs, the YCP bridge to Python (`python_bridge*.cpp` dispatches every request a `yc` member sends; some are handled beside their UI: `errors.*` in `error_log.cpp`, `plot.*` in `figures_ui.cpp`, `image.windows` in `screenshot_ui.cpp`) |
| `src/` | `app` (shell integration: file associations), `core`, `model`, `engine` (document, calculation, undo), `formula`, `io` (`.ycg`, `.xlsx`, `.csv`), `render`, `ui` |
| `python/yc/` | the Python package (`excel` is the object model; `ui`, `rules`, `data`, `plot`, `trading`, `safety`...); `python/yc/mcp/` the MCP server |
| `mcp/tools.json` | the single source of the MCP tools; `mcp/gen_tools_doc.py` writes [mcp-tools.md](mcp-tools.md) from it |
| `schema/commands.yaml` | every UI command's Python entry point; `scripts/check_commands.py --write` writes [ui-coverage.md](ui-coverage.md) |
| `i18n/zh-TW.json` | the UI translation table; `src/core/i18n_zh.inc` is generated from it |
| `docs/YCGRID_PLAN.md`, `docs/adr/`, `docs/settings.md` | the specification, the decisions that differ from it, the settings |
| `tests/unit/` | Catch2 unit tests (listed in `tests/CMakeLists.txt`) |
| `python/tests/` | Python tests (most drive a hidden YCGrid) |
| `scripts/` | build, packaging and the checks below |

## 2. Build

- `scripts\build.cmd release /silent` (or `debug`): configures with the version checks of `third_party\deps.lock`,
  builds, runs the Catch2 tests (`build\<preset>\tests\yc_unit_tests.exe`, which also takes a test name or `[tag]`)
  and stages `dist_release` / `dist_debug` (the program, `runtime\python`, `python\yc`, the skill). Don't call cmake
  or ninja yourself.
- `scripts\make_sln.cmd [/open]` makes a Visual Studio solution (`ycgrid.slnx`).
- Staging fails with `file COPY cannot copy file` while a `ycgrid.exe` from `dist_*` runs: a hidden instance left by a
  test (`tasklist /FI "IMAGENAME eq ycgrid.exe"`; its command line has `--hidden`). Close only the ones you started;
  never close the user's YCGrid.
- `scripts\package.cmd [--with-python]` builds the release zip; `scripts\setup_runtime.cmd` installs the portable
  Python; `scripts\build_wheel.cmd` the pip wheel.

## 3. Tests

Run the Python tests from `python\` with the bundled Python:

```bat
cd python
..\dist_release\runtime\python\python.exe -E -s -m unittest tests.test_core tests.test_live.UserFlowTest
```

| Module | Covers |
|---|---|
| `test_core` | the parts of the `yc` package that need no running YCGrid; also runs `check_commands.py` (command coverage) |
| `test_mcp` | the MCP tools |
| `test_data`, `test_plot`, `test_analysis`, `test_trading`, `test_examples` | `yc.data` / bound tables, `yc.plot`, the Analysis ToolPak, `yc.trading`, every example screen built |
| `test_live` | a hidden YCGrid driven like a person (one job per test class in `check_parallel.py`) |
| `test_excel_parity`, `test_parity_*`, `test_round2_*`, `test_round3_*` | Excel's behavior, checked the way a person works: one module per area (`cells`, `charts`, `data`, `formatting`, `formulas`, `habits`, `insert`, `pivots`...) |
| `test_live_range` | a manual test against a running YCGrid; skipped unless `YCGRID_LIVE_PID` is set |

`check_parallel.py` runs every `python/tests/test_*.py` module (`test_live` one class at a time), so a new module or
test class is picked up by itself.

What every change brings with it:

- New engine or formula behavior: Catch2 tests in `tests/unit/`.
- A new Python member: a Python test, and its entry in [object-model.md](object-model.md) (or the module's reference);
  `yc_api_lookup` reads these files.
- A new UI command: its entry in `schema/commands.yaml`, a check in `scripts/feature_sections.py` (a `@section`
  function that does what a person does: `app.run("CommandId")` clicks it where the user finds it, `app.press("OK")`
  clicks a dialog button by its label, `app.inp(...)` takes a `ui.input` script, `check(name, ok, detail)` checks the
  result, `cover(...)` names commands the section checked another way), and its UI text as `TR("English")` followed by
  `python scripts\i18n_build.py extract`, the translation in `i18n/zh-TW.json`, `build`, and `check` (0 missing).
- New saved state: a new integer key or chunk in `.ycg` with a save / load round-trip test (old files must open).
- A fixed bug: a test that fails without the fix, and one line in AGENTS.md "Lessons from bugs" when the mistake could
  happen again elsewhere. A problem `scripts\chaos.py` found adds its seed to `scripts\chaos_seeds.txt`
  (`seed  language  actions  # what it found`).

## 4. The checks

- **While working**: only the tests of what you touched. **Once per round of changes, before a commit**:
  `scripts\check_all.cmd` (`/quick`: build, unit and Python tests, translations, command coverage, boundary, model
  fields, lessons).
- `check_all.cmd` runs: the release build with the unit tests; `i18n_build.py check`, `check_commands.py`,
  `check_boundary.py`, `check_model_fields.py`, `check_lessons.py`; then `scripts\check_parallel.py` (below); then the debug build and every seed of
  `chaos_seeds.txt` on it (`check_parallel.py --chaos`).
- `scripts\check_parallel.py [--quick | --chaos] [--jobs N] [--only name,...] [--list]` runs the slow jobs side by
  side, each in its own process with its own hidden YCGrid and TEMP folder: one job per Python test module (per class
  for `test_live`), `basics:K/6`, `basics:seq1` / `basics:seq1001`, `basics_cmd:K/4`, `basics_tabs:K/4`,
  `basics_zh:K/3`, `basics_zh:seq7` (these with `--quick` too), `feature:K/N` shards of `feature_check.py` and then one
  coverage check over them, `ui_sweep:commands` / `ui_sweep:dialogs`, `fuzz:recalc`, `fuzz:undo`, `fuzz:files`,
  `formula_fuzz`, `doc_fuzz` (and `doc_fuzz_wide`, `doc_fuzz_more`, `doc_fuzz_wide_more`); `trader_session` runs
  alone at the end (it is timed).
  `--only python:test_live.UserFlowTest,feature` runs the jobs whose names start so; `--list` shows them. Each job
  logs to `%TEMP%\yc_check_<job>.txt`, every character of the name that isn't a letter or digit written as `_`
  (`yc_check_basics_0_6.txt`); the summary names the failed jobs. Tests must not depend on each other or on
  shared files.
- The single checks (bundled Python, `--exe` defaults to `dist_release\ycgrid.exe`):

| Script | Does |
|---|---|
| `feature_check.py [--only sections] [--shard K/N] [--keep]` | every command clicked where the user finds it, its result checked; fails while a registered command has no check |
| `ui_sweep.py [commands] [dialogs] [examples]` | every command once, every dialog with Enter and Escape, every example screen |
| `trader_session.py [--only ...] [--lang]` | a trader's and a researcher's day, timed; frame times from `perf.stats` |
| `calc_fuzz.py --mode recalc / undo / files --seed N --steps S` | random edits against `CalculateFull`, Ctrl+Z / Ctrl+Y, save and reopen (`.ycg`, `.xlsx`) |
| `formula_fuzz.py --seed N --n COUNT [--eval]` | single formulas against answers worked out in Python |
| `chaos.py --seed N --minutes M [--actions K] [--lang zh-TW]` | a hurried user at random; run seeds on `dist_debug` too |
| `build\msvc-release\tests\yc_doc_fuzz.exe [--seconds N] [--seed S] [--cases N] [--corpus DIR] [--replay FILE [--minimize] [--every-step] [--verbose]]` | the Document in-process, thousands of steps a second: Undo / Redo exact, every step undoable, `.ycg` round trip with a full recalculation; `--minimize` cuts a failing stream down, `--every-step` finds the step a save / open first differs at, `--verbose` prints both dumps |
| `random_rounds.py run [--rounds N] [--snapshot] / report` | rounds of every random tool with fresh seeds, logged in `docs/random-runs.csv` (`--snapshot`: copies of the builds, so a fix can be built meanwhile); `report` draws the curve and says whether random testing has converged |
| `measure_test_formulas.py [--only FILE_PART]` | every cell-free `t.Eval("...") == "..."` claim of the unit tests calculated in a hidden Excel of its own: a test's expected value written from a help page or a guess shows up here |
| `check_model_fields.py [--missing]` | every member of Sheet and Workbook saved or transient, every field that may name cells walked or kept (`schema/model_fields.yaml`) |
| `check_lessons.py [--list]` | the AGENTS.md lessons a pattern catches; `// lesson-ok: <rule> <why>` where a line breaks one on purpose |
| `check_commands.py [--write] [--live]` | command coverage; `--write` regenerates [ui-coverage.md](ui-coverage.md) (never edit it by hand) |
| `i18n_build.py extract / build / check` | the translation table |
| `check_boundary.py [--history]` | no credentials, internal addresses or private files in the tree (`--history`: in any commit either; before a release) |
| `crash_stack.py <pid> [pdb folder]` | the stack of a crash that leaves no dump (fast-fail `0xC0000409`): attaches to a hidden instance as a debugger |

- Stop every background check before starting another and leave no hidden `ycgrid.exe` behind: two runs competing for
  the CPU make the timed checks fail at random.
- **Random testing has a stopping rule** ([docs/random-testing.md](../../../docs/random-testing.md)): every finding
  gets a line in `docs/random-findings.csv`, and while `random_rounds.py report` says "not converged" no new features
  go in. A finding is cut down (`yc_doc_fuzz --replay FILE --minimize`), made a test (`tests/unit/test_fuzz_found.cpp`),
  fixed where every path meets, and then its class is closed by a check that fails for all of it: a new model field
  goes into `schema/model_fields.yaml` (and the round trip or the reference matrix proves it), a new text that names
  cells into `src/model/ref_texts.h`, a new write source into `engine::PolicyFor`, a lesson a pattern can catch into
  `check_lessons.py`. A doc_fuzz stream that failed goes into `tests/fuzz/corpus/` once fixed.
- Excel itself is asked at random too: the `rand_*` cases of `scripts/excel_basics_cases.py` are random sequences of
  object-model steps, measured in a hidden Excel (`measure_excel_basics.py --only rand_`) and replayed by
  `tests.test_excel_basics`; raise `RANDOM_CASES` to look further. The first 150 found a sort rule (references to
  other sheets stay) that 732 single-operation cases never showed.
- Verify UI changes in the running app with its own screenshots (`ycgrid.exe --screenshot`, `ui.screenshot`; the
  command line is in [testing.md](testing.md) §2), never the user's screen, and read them.

## 5. Pitfalls when writing UI tests (from AGENTS.md "Lessons from bugs")

- The mouse stays where the last step left it, and a keyboard-opened menu lights the item under it: park it
  (`m:2,2`) before keyboard menu steps and at the end of a test that drags.
- A test that passes alone but fails in its class depends on state another test left (mouse, selection, clipboard,
  an open popup, a floating window over the cells): fix the dependency (a fresh sheet per test, `k:Escape` and a check
  that `dialog` is 0 and no popup is open at the end), don't reorder tests.
- Two clicks on one cell less than half a second apart are a double-click (the cell editor opens); an in-place editor
  takes the keyboard only while it has the focus.
- `ui.items()` reports only what a person can see; when a control can't be found by its label, fix the recorder
  (combo boxes need `ui::NoteUiItem`), not the test.
- A modal dialog blocks the sheet; boxes that take cells (`RefEdit`) let the user reach it: test picking cells with
  drags, as a person does.
- Live updates run on every frame of a drag: check titles, legends and defaults from the final state.
- Undo only your own action's step: read `ui.state()["undo"]["made"]` (the steps ever made) before and after;
  `depth` stays at 100 once the list is full, so it can't tell.
- A crash without a dump: the Application event log has the offset; `crash_stack.py` gives the stack. `ui.state()`'s
  `imgui_asserts` counts ImGui checks that failed (a debug build stops on the first). `ui.input` goes straight to
  ImGui, past Windows' hit test: for the title bar, check `titlebar_clickable` too.

## 6. Keeping this skill right

- The skill is shipped with YCGrid and copied into projects (`.claude/skills`, `.agents/skills`, `.github/skills`; replaced by the version
  of the YCGrid that opens the project), so it describes what the installed version does.
- Document what exists: check a claim against `python/yc` and a hidden instance before writing it; a planned member
  is marked (P2) and says what to use meanwhile. Examples must run.
- Generated files are regenerated, not edited: [mcp-tools.md](mcp-tools.md) (`python mcp\gen_tools_doc.py`) and
  [ui-coverage.md](ui-coverage.md) (`scripts\check_commands.py --write`).
- Development and testing workflows and their pitfalls belong here and in [testing.md](testing.md), so the next person
  (or agent) doesn't rediscover them.
