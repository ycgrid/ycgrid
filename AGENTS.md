# YCGrid development rules (for AI agents and contributors)

YCGrid is an Excel-like spreadsheet front end in C++ (Dear ImGui + ImPlot) controlled by Python. The main specification
is [docs/YCGRID_PLAN.md](docs/YCGRID_PLAN.md); design decisions that deviate from it are recorded as ADRs in
[docs/adr/](docs/adr/). Global settings are described in [docs/settings.md](docs/settings.md). People new to the project
start with [CONTRIBUTING.md](CONTRIBUTING.md); this file holds the rules in full and the lessons learned from bugs.

## Language

- Talk to the person you work with in the language they use with you.
- **Everything in the repository is English**: code, comments, identifiers, string literals, samples, templates, scripts,
  docs, MCP tool descriptions, skill docs and commit messages, so people anywhere can read and contribute.
- Chinese appears only in `i18n/zh-TW.json` (the UI translation table; `src/core/i18n_zh.inc` is generated from it with
  octal escapes), in `README.zh-TW.md` (the Traditional Chinese README, kept in step with `README.md`), in tests that
  check CJK behavior, and as escaped functional data (`\u...` / octal), e.g. Chinese date tokens.
- UI text is written as `TR("English")` / `TRW(L"English")`. After adding strings run
  `python scripts/i18n_build.py extract`, fill in `i18n/zh-TW.json`, then `python scripts/i18n_build.py build`;
  `python scripts/i18n_build.py check` must report 0 missing.

## Naming

The product is **YCGrid** (ADR 0014). Every name follows from it in one of five forms; pick from this table, don't
invent a sixth:

| Form | Where | Examples |
|---|---|---|
| `YCGrid` | text people read: UI, window titles, dialogs, docs, README, registry display names, ProgID, folders under Documents, Win32 object names | `YCGrid.Workbook`, `Software\YCGrid`, `Local\YCGrid.Instance.*` |
| `ycgrid` | file, folder, package and pipe names | `ycgrid.exe`, `apps/ycgrid/`, pip `ycgrid`, `ycgrid.toml`, `\\.\pipe\ycgrid.<pid>`, `%LOCALAPPDATA%\ycgrid`, skill `ycgrid-python` |
| `YCGRID_` | environment variables | `YCGRID_EXE`, `YCGRID_PYTHON`, `YCGRID_PID` |
| `yc` / `Yc` / `YC` | the prefix of code identifiers | `import yc`, `namespace yc::`, `YC_NUM`, `ycDockRight`, `YcError`, MCP tools `yc_*`, CMake targets `yc_*`, the doc marker `[YC]` |
| files and protocol | extensions, binary tags, protocol | workbook `.ycg` (header `YCG1`, encrypted `YCGE`), recordings `.yctape`, control protocol YCP |

- No name contains `cel`, `xcel` or `Excel`. Describing compatibility ("works like Excel", Excel's object model names)
  is fine; naming the product, a file type, a package or a logo after Excel is not.
- The brand is always one word, `YCGrid`: never "YC Grid", and never `YC` alone as the brand (it reads as
  Y Combinator). The two-letter code prefix is not the brand.
- Never use the word "VBA" anywhere: Python takes the place of Excel's macro language, and its module is `yc.excel`.

## Git

- Work on a branch from `dev` and bring it in through a pull request to `dev`, never a direct push; `main` holds the
  released versions and moves only through a pull request from `dev`. Every step (branch names, the pull request,
  waiting for CI, merging with a merge commit, the release) is in [docs/git-workflow.md](docs/git-workflow.md).
- Don't push, open a pull request or merge unless asked.
- Commit messages are English: a first line saying what changes for the user, then the details when they help. No
  `Co-Authored-By` trailer or any other AI-agent attribution.
- Every source file of YCGrid's own (C++, Python, `.cmd`, CMake) starts with `SPDX-License-Identifier: Apache-2.0`
  in a comment (in a `.cmd`, `rem` on line 3, after `chcp`); `check_boundary.py` fails without it. A generated file
  says "generated" in its first lines instead, and its generator doesn't write the line.

## Design rules

- **Excel parity converges.** Before listing how YCGrid differs from Excel, read "Kept on purpose" and "Not planned" in
  [docs/excel-parity-backlog.md](docs/excel-parity-backlog.md) and don't propose those again. What matters is Excel's
  habits and interface where people work every day (keys, KeyTips, menus, ribbon, dialogs) and basic features without
  bugs; a feature few traders or researchers need is added to "Not planned" with its reason instead of being built.
- **Building blocks only.** YCGrid ships low-level APIs and capabilities, never ready-made features on top of them.
  Order managers' strategies (brackets, OCO, P&L reports), chart widgets that bundle trading logic, journals, desks,
  screens, scanners and the like are left to developers, who build them from the blocks. A block belongs in YCGrid
  only when developers can't build it well themselves: it needs the C++ side (rendering, data paths, the engine), or
  speed, or stability that Python on top can't give. Every block must be fast and stable, and proven by tests and by
  an example that composes it into what a developer would build. When a request asks for a feature, provide the
  blocks it needs and show the composition in an example, not in the library. Examples are project folders in
  `examples/<name>` (its `.ycg`, a `main.py` whose `Workbook_Open` builds the screen, `ycgrid.toml`), listed in
  `examples/README.md`, with the code several share in `examples/lib` (`[python] paths`); never a package of `yc`
  or a gallery in the UI.
- **Anything the Excel UI can do, Python can do**: every feature is reachable from the UI through the
  `CommandRegistry` (ribbon, context menus, shortcuts), from Python with Excel's names, and from the MCP tools where
  one exists. A new UI command gets its Python entry point in `schema/commands.yaml`; `scripts/check_commands.py
  --write` checks the coverage (also run by `tests.test_core`) and regenerates
  `skill/ycgrid-python/references/ui-coverage.md`. New Python members are documented in
  `skill/ycgrid-python/references/object-model.md`.
- **Excel's object model is accounted for member by member**: every member Excel 16's type library lists for the
  objects in `schema/excel_parity.toml` (`tests/data/excel_object_model.json`, measured from EXCEL.EXE by
  `scripts/measure_excel_object_model.py`) is YCGrid's, not planned with its reason, or an open gap with its priority.
  `scripts/check_excel_parity.py` (run by `tests.test_core` and check_all) fails on a member in none of them and on a
  listed one YCGrid now has, so a new Python member takes its line off the gaps; `--write` regenerates the gap list,
  `docs/excel-object-model.md`. A missing Excel setting a person finds (Show page breaks) was in no check before: a new
  object a feature adds gets its line in `[objects]`, and its members are measured, not guessed.
- `.ycg` files stay backward compatible (hard rule 21): new saved state goes into new integer-keyed fields or new chunks
  (`preserve_if_unknown`), with a save / load round-trip test.
- Build and packaging details, version pinning (ImGui / ImPlot submodules, `third_party/deps.lock`) and the hard rules
  are in the PLAN; read §0 before larger changes.

## Building and testing

- Build only with `scripts\build.cmd release /silent` (or `debug`); `build_release.cmd` at the root is the same release
  build for a person (a double-click: it sets up the portable Python when missing and opens `dist_release`). It configures with the deps.lock version checks,
  builds, runs the Catch2 unit tests and stages `dist_release` / `dist_debug`.
- Python tests are the modules in `python/tests/`. Run one in `python\` with the bundled Python:
  `..\dist_release\runtime\python\python.exe -E -s -m unittest tests.test_core`. Those that need no running YCGrid
  (`test_core`, `test_mcp`, `test_data`, `test_plot`, `test_analysis`, `test_pure`) also run on GitHub for every push and pull request.
- New engine or formula behavior gets Catch2 tests in `tests/unit/`; new Python client members get Python tests.
- Coverage: `scripts\coverage.cmd` (debug) runs the Catch2 tests under OpenCppCoverage and prints the per-directory
  line coverage with the least covered files of `src` (`build\coverage\debug\index.html` has every line); the
  `apps/ycgrid` UI has no unit tests and is covered by the live checks only. Python: `coverage` isn't bundled;
  install it to a folder (`pip install coverage --target <dir>`), then `PYTHONPATH=<dir>` and the bundled Python
  with `-s` (not `-E`): `-m coverage run --source=yc -m unittest tests.<module>`, one process per module (every
  module imported into one process stacks `tests\..` onto the exe path past 260 characters).
- Checks against a running YCGrid without a window: `yc.launch(hidden=True)`, `yc.ui.state()`, `yc.ui.screenshot()`.
  Verify UI changes in the running app with its own screenshots (`ycgrid.exe --screenshot`, or `ui.screenshot` over
  YCP). Never capture the user's screen.
- Use the UI the way a trader or researcher does before calling a feature done: keys, clicks, drags and dialogs in a
  hidden instance (`skill/ycgrid-python/references/testing.md`), and read the screenshots. After UI changes run
  `scripts\ui_sweep.py` (every command, every dialog with Enter / Escape, every example screen) with the bundled Python.
- **Basic operations come first, in every state.** Most bugs people meet are a basic operation in a state nobody
  tried it in (typing after read-only mode came on mid-edit, Undo after a paste into a filter). `scripts\basics.py`
  runs every basic operation (typing, F2, Enter / Tab / arrows, Delete, copy / cut / paste, Undo / Redo, fill, insert /
  delete, formats, merge, sort, the context menu, the fill handle, drags) in every state a person reaches (editing,
  pointing, ranges and whole rows, merged cells, a filter, frozen panes, protection, read-only mode, read-only mode
  switched on during an edit, grouped sheets, copy / cut mode, a table, a spill, an array, a shape selected, Page
  Layout, zoom, a value in the sheet's last cell) and checks what every operation must keep: no message Excel wouldn't show (`ALLOWED`, each with its
  reason), Undo gives back exactly what was there and Redo exactly what the operation made (`debug.dump`, from
  `io::ContentDump`), no change without an Undo step, read-only mode changes nothing, two Escapes leave no editor,
  dialog or menu, no ImGui check fails. `python/tests/test_excel_basics.py` replays 1,699 operations measured in Excel
  (`tests/data/excel_basics_measured.json`, cases in `scripts/excel_basics_cases.py`, measured by
  `scripts\measure_excel_basics.py`) and compares every cell's formula, value and format; `KNOWN_DIFFERENCES` there
  only shrinks.
  The matrix and its random sequences run in English and in the Traditional Chinese UI (`--lang zh-TW`, the messages
  read back into English through i18n/zh-TW.json), since that is the UI people use.
  `--commands` runs every command of the Home tab (`tab` in `ui.commands()`, from the ribbon itself) in the basic
  states: a command added to Home is tried without a line in basics.py; `--tab others` does the same for every other
  tab (job `basics_tabs`), one of each numbered family (Paper9, InsertShape12, InsertFn_SUM) and without what
  leaves the session, prints or writes files.
- **A bug in a basic operation is fixed so it stays fixed**: (1) make it a case first: the state it needed in
  `basics.py`'s `STATES`, the operation in `OPS`, a sequence (`--sequence state:op,op`, cut down to the steps that
  still fail), or a measured case in `excel_basics_cases.py`; (2) watch the case
  fail; (3) fix the cause where every path meets (the Document, not one key's handler); (4) run the whole matrix and
  the replay (`check_parallel.py --only basics:0/6,...,basics:5/6,python:test_excel_basics`): a fix for one state that
  breaks another is what this catches; (5) a lesson line below when the mistake could happen elsewhere. A new mode, a
  new kind of selection or a new place a person can be adds its state to `STATES`; a new basic operation adds itself to
  `OPS`; a message added to `ALLOWED` names the Excel behavior it matches.
- **The sheet's limits are a state too**: `scripts\scale_sweep.py` runs every command over the whole sheet, whole
  columns, whole rows and B2:XFD1048576 (a value in the last cell), accepts its dialog, undoes it, and fails a step
  over 10 s; run it after a change to what a command does over a selection (it takes hours: `--only REGEX`). The
  freezes of borders, Merge, Fill Right and a format of the whole sheet were each one such step found late by random
  rounds.
- **Every feature is checked by its result, not only for not crashing**: `scripts\feature_check.py` clicks each command
  where the user finds it (tab, menu, submenu, gallery, dialog button, status bar chip) and checks what it did (cells,
  formats, files, windows, a recording that replays). A new command or feature gets its check in
  `scripts\feature_sections.py` with it; the run fails while any registered command has none.
- **Before every commit: `scripts\check_all.cmd`** (`/quick` for build, unit and Python tests, translations, command
  coverage, boundary, model fields and lessons only). The full run adds ui_sweep, feature_check, `scripts\trader_session.py` (a trader's and a
  researcher's day, timed: frozen screens, lost clicks) and a replay of `scripts\chaos_seeds.txt` on the debug build. A
  fix is done when it has a test that fails without it, and every check still passes: a fix that breaks something else
  is not a fix. A problem found by `scripts\chaos.py` adds its seed to `chaos_seeds.txt`.
- The slow checks run side by side: `scripts\check_parallel.py` (called by `check_all.cmd`) runs the Python test
  modules (test_live per class), feature_check in shards (`--shard K/N`, then one coverage check), ui_sweep and the
  fuzzers at once, each in its own process, hidden YCGrid and TEMP folder, then the timed trader_session alone.
  `--only python:test_live.UserFlowTest,feature` runs part of it; each job's log is `%TEMP%\yc_check_<job>.txt`.
  A new test module or test class is picked up by itself; tests must not depend on other tests or on shared files.
- Don't run the full check after every small change: finish the round of changes (with their tests), then run
  `check_all.cmd` once. While working, run only the tests of what you touched.
- **Random testing is a process with a stopping rule** ([docs/random-testing.md](docs/random-testing.md)): rounds are
  run with `scripts\random_rounds.py run` and logged in `docs/random-runs.csv`, every finding gets a line in
  `docs/random-findings.csv` (real or the tool's own, its severity, open or fixed), and `random_rounds.py report`
  draws the curve. **While it reports "not converged" (20 hours of rounds since the last real finding, none open), no
  new features**: fix the findings and close their classes. The fastest tool is `yc_doc_fuzz` (the Document
  in-process): `--replay FILE --minimize` cuts a failing stream down, the fix gets its case in
  `tests/unit/test_fuzz_found.cpp`, and the stream goes into `tests/fuzz/corpus/` (check_all replays it).
- **A bug is closed by closing its class.** Before calling a fix done, ask what else has its shape and make a check
  that fails for all of it, as these do: every reference kind against every operation (`model/ref_texts.h`,
  `test_ref_registry.cpp`), every model field saved or followed (`schema/model_fields.yaml`,
  `scripts/check_model_fields.py`, `test_model_roundtrip.cpp`), one table for what a write's source means
  (`engine::PolicyFor`), and the lessons below a pattern can catch (`scripts/check_lessons.py`, with
  `// lesson-ok: <rule> <why>` where a line breaks one on purpose). A lesson that can become a check becomes one.
- Before a release: `scripts\check_boundary.py --history` (no credentials, internal addresses or private files in the
  tree or in any commit of `main` / `dev`), and the release's entry at the top of `CHANGELOG.md`. Every step of a
  release (the version in five files, the zip and wheel with their SHA-256, the tag; nothing goes to PyPI) is in
  [docs/releasing.md](docs/releasing.md).
- A feature check uses every control a person uses in the window, not only the one that gets the result: Insert
  Symbol's check typed a code and pressed Insert, and a click on a character, the way people pick one, crashed
  (its code box called itself, chaos seed 5094). A debug build's failed CRT check writes an `assert` dump too.
- A crash dump is written by the watchdog's own thread (`yc-dump-writer`), never by the thread that crashed:
  MiniDumpWriteDump from the faulting thread left an empty file in chaos round 5093. `debug.crash` (a hidden
  instance only) checks it (`test_live.CrashDumpTest`).
- A crash without a dump (fast-fail, `0xC0000409`): the Application event log has the offset; a debugger attached to
  the hidden instance (`scripts\crash_stack.py <pid>`: dbghelp `StackWalk64` with `dist_*\ycgrid.pdb`) gives the stack.
  A dump (crash, hang, a debug build's assert or abort) reads without a debugger installed: `scripts\dump_stack.py
  <file.dmp>` walks it with the same pdb. A random round's dumps are in its own TEMP folder, deleted with the round:
  replay the seed with TEMP / TMP set to a folder that exists (a missing one was a crash of its own).
- A stutter or a freeze: `scripts\ui_sampler.py <pid>` samples the hidden instance's UI thread and names the functions
  it spends its frames in (`--thread yc-engine` samples the engine); a frame waiting in `Engine::Call` is behind the
  engine, so sample the engine next. `perf.stats` gives each frame's work (`frame_cpu_max_ms`), and
  `scripts\trader_session.py`'s big workbook times a million rows step by step. `debug.engine` gives the engine's
  index sizes (formula cells, dependencies live and stale, pending, spills): read them before and after the slow step,
  and against a fresh open of a copy saved just before it (`SaveCopyAs`). A chaos step that took 17.5 s was a
  Ctrl+Enter over four whole columns (four million formulas, from 127; Excel 16 took over five minutes, measured),
  not a stuck index: the click meant for I40 had closed a ribbon menu and the selection stayed.

## Scripts

- Build, packaging and installation scripts are `.cmd` only, never PowerShell `.ps1`.
- `.cmd` files: UTF-8 without BOM, CRLF line endings, `chcp 65001 >nul` on the second line, errors checked with
  `if not "!errorlevel!"=="0"` (linkers can return -1). See PLAN §13.16.

## Safety

- Don't close programs the user is running (for example `dist_debug\ycgrid.exe`); wait until they exit.
- Don't write the registry (only `ycgrid.exe --install` does, for file associations) and don't touch the system Python.

## Lessons from bugs (keep adding)

Every bug fixed gets a test that fails without the fix, and, when the mistake could happen again elsewhere, one line
here, under its area, saying how to avoid it. Read the area before changing it.

### Tests and checks

- UI tests: the mouse stays where the last step left it, and a menu opened by keyboard under it lights the item
  under the mouse. Park the mouse (`m:2,2`) before keyboard menu steps, and at the end of a test that drags.
- A check that resets the sheet between commands (ui_sweep's `sample`) undoes what a command may have done to it
  first: Hide Sheet left sheet 1 hidden, and `Activate` of a hidden sheet does nothing (Excel 16, measured; `Select`
  raises), so the sheet never came back; and it keeps the shapes
  and charts commands insert from piling up (every frame draws them all: the sweep slowed to seconds a command and ran
  past its 60 minutes).
- A check of the undo list counts on what fills it: Python's writes drop only the steps undone with their whole
  version, so the list reaches its 100 steps, where a new step drops the oldest and the depth stays (calc_fuzz seed
  3028 reported a typed cell as no step). At the limit the depth can't tell whether an action made a step: read
  `ui.state()["undo"]["made"]` (`Document::StepsMade`), or a sort that made none was "undone" by taking back the
  typing before it (seed 3130).
- A cache asked on every call of a formula is free on its hot path: `CompileFormat` took one mutex and made a string
  at every TEXT, and a calculation wave of 160,000 TEXT formulas spent most of its time waiting there (a per-thread
  last entry and a shared lock now; chaos seed 5043). Sample the engine (`ui_sampler.py --thread yc-engine`) before
  calling a slow step the workload's.
- Random runs given a time (`yc_doc_fuzz --seconds`) run as many cases as fit, from the seed up: runs side by side get
  seeds farther apart than one run's cases (about 18,000 in five minutes), or two of them run the same cases. A
  scenario tool draws each scenario from its own name and the seed (`gaps_fuzz`), so adding a scenario doesn't change
  what the old seeds replay; and two runs of a tool side by side are a test of their own (shared windows, files).
- A round of random testing tries what no round before it tried: a tool that derives many cases from one seed
  (basics' `--sequences N --seed S` runs S..S+N-1) is given seeds N apart (`random_rounds.py`), never consecutive
  ones. Given S, S+1, S+2, basics repeated 59 of its 60 sequences a round, and its hours counted toward convergence
  while they tried about one new sequence each.
- chaos runs the debug build for its asserts and the release build is what is timed: its time limits are 50 times
  wider on a debug build (`Chaos.slow_scale`), where a step of 3.6 s released took 412 s.
- A slow step means something only on a machine with CPU to spare: chaos counts a step that took too long only while
  the machine had idle CPU (`Chaos.busy_since`), since rounds beside a build reopened in 12.7 s that took 0.3 s alone.
  A frozen YCGrid spins one thread, under 10% of this machine, so a freeze still counts.
- A floating ImGui window takes the mouse a few pixels outside its rectangle (its resize border): a UI test that
  grabs something beside one keeps clear of it by a margin, or the press resizes that window (chaos grabbed a docked
  tab 5 px above a floating window and reported the tab stuck).
- A test that passes alone but fails in its class depends on state another test left (mouse, selection, clipboard,
  an open popup): fix the dependency, don't reorder the tests.
- Unit tests share one process, and the engines' release thread (`ReleaseOnWorker`) frees what earlier tests'
  Documents dropped at any moment: a test that checks which pooled tile block comes back calls
  `engine::WaitForReleases()` and empties the pool first, or a tile freed in between is handed out instead (2% of
  runs, seen once on CI).
- A test run never takes the machine's memory: every random round runs inside a job object that limits each of its
  processes to 6 GB (`random_rounds.run_capped`; the YCGrid a script starts is in the job too), and a fuzzer started
  by hand gets such a limit too. Two `--wide` doc_fuzz rounds held 80 GB each, Windows ran out of memory and a YCGrid
  beside them died of an uncaught bad_alloc; the cause was one step (an array formula over the whole sheet, now
  refused past 20,000,000 cells), found by running the stream under the limit with `--verbose`, which names each
  operation before it runs.
- Stop every background check before starting another, and make sure no hidden `ycgrid.exe` is left: a second run
  competing for the CPU makes timing tests fail at random.
- UI tests never assume 4/3 pixels per point: a hidden instance may run at another screen scale; measure it from
  `ui.cell_rect` and `Range.Width`.
- A hidden instance never asks Windows about the real mouse or keyboard: ImGui's Win32 backend calls WindowFromPoint
  at the cursor every frame, and on a desktop slow to answer it (the screen asleep at night) that took 500 ms, so
  every frame of every hidden instance did and trader_session reported frozen big workbooks. After its first frame
  it sets the window's size and time step itself (`HeadlessPlatformFrame`, main.cpp). Sample the UI thread
  (`ui_sampler.py`) before calling a slow frame YCGrid's: one spent in a system call is the system's.
- A hidden instance reads the user's settings.json: a personal UI choice (the ribbon's "Show tabs only") made tests fail
  on that machine only. Hidden instances start from what tests assume (the ribbon shown); add other such choices there.
- `Workbooks.Add()` in a test whose workbook holds data starts another YCGrid and the Python connection follows it: the
  later tests talk to the wrong instance. Test a new workbook in a hidden instance of its own (`YCGRID_SETTINGS` for
  its options), and reconnect (`yc.connect(target=...)`) after reading a second instance.
- Workbooks opened by scripts and every hidden instance follow `Application.AutomationSecurity` (Low: the project
  runs), a person's opens follow the Trust Center: a test of what a person sees (the Message Bar, Enable Content)
  launches with `yc.launch(..., automation_security=2)`, and code that starts the Python host for a folder goes through
  `StartPythonForWorkbook` / `trust::AllowProject`, never `StartHost` on the workbook's folder directly.
- When what Excel does is uncertain, measure it instead of guessing: a script can start a hidden Excel of its own
  through COM (PowerShell's `New-Object -ComObject Excel.Application` never attaches to one the user runs), write
  the inputs (`Range.Formula`), read `Value2`, `NumberFormat` and `Text`, close the workbook unsaved, release every
  COM object and quit that instance only (kill it by the process id of its own `Hwnd` if it stays). It reads with
  the machine's regional settings (Taiwan's: negatives as -$5, times as hh:mm, format codes in the UI language), so
  tell a regional choice from Excel's rule; YCGrid writes US Excel's codes. Typed formats guessed for months were
  wrong in three of four cases (`12.5%` got `0.0%`), and so were rules marked "as in Excel" without a measurement
  (`-0.4` in `0` shows `0`, not `-0`; `[DBNum1]` reads 12 with "one ten"): measure before writing "as Excel". A
  built-in format id (14, 20-22, 37-44) shows the region's own form while `NumberFormat` (en-US) reports US
  Excel's code: `m/d/yyyy` shows 2024/1/7 and `$#,##0_);($#,##0)` -$1 on Taiwan's Windows. Every such regional
  difference found is listed under "Regional settings, not differences" in docs/excel-parity-backlog.md: read it
  before reporting a difference, and add a new one there instead of changing YCGrid.
- What Excel leaves in its cell editor (AutoSum's proposal) can't be read through COM, which refuses every call while
  a cell is edited: show the probe's own Excel (`Visible`, with the user's consent), run the command
  (`CommandBars.ExecuteMso`), read the formula bar through UI Automation (its TextPattern), then kill that Excel by
  its process id. A hidden Excel has no UI Automation tree (0 elements), and the formula bar's ValuePattern is empty:
  only `XLFormulaBarEditor`'s TextPattern gives the text.
- What a key does in Excel's cell editor and around it (typing, F2, F4, AutoSum, the keys that end an entry) is
  measured by posting the keys to the sheet window (`EXCEL7`) of a hidden Excel of the probe's own, then reading the
  cells through COM (`Application.Ready` is False while a cell is edited). Its ScreenTips, Formula AutoComplete list
  and Quick Analysis button are windows that show even when that Excel is hidden: turn them off for the run and put
  them back before it quits. Never send it Ctrl+C / X / V (Windows' clipboard is the user's), avoid entries that spill
  or make a circular reference (a callout or a modal state that eats later keys), warm it up on a new sheet (its first
  key with a modifier arrives without it), and type a percent cell one character a key (it lost the first one). Run
  the same keys in YCGrid three frames apart and one frame apart: a difference only at one frame is the editor's
  frame race, not a rule.
- A key with modifiers reaches a hidden Excel's sheet window (`EXCEL7`, found among `Application.Hwnd`'s child windows)
  when the probe attaches to its thread (`AttachThreadInput`), sets Ctrl / Shift in the shared keyboard state
  (`SetKeyboardState`), posts `WM_KEYDOWN` / `WM_KEYUP` and puts the state back. In PowerShell set
  `Application.CutCopyMode = False` by late binding (`InvokeMember`): the interop type refuses False and 0. Measuring a
  paste uses Windows' clipboard: ask the user first. Alt goes in the state too (VK_MENU) with `WM_SYSKEYDOWN` /
  `WM_SYSKEYUP`; try a chord whose effect is known first (Alt+PgDn moves a screen right).
- A hidden Excel's menus and dialogs are windows of their own (`NetUIToolWindow` for the filter menu, `NUIDialog`
  for Sort, Remove Duplicates, the highlight rules) and are in UI Automation, though the sheet isn't: each entry's
  `AccessKey` is Excel's letter (the filter menu's S, O, T, C, I, F; Number Filters' E, N, G, O, L, Q, W, T, A, O, F),
  a submenu opens through `ExpandCollapsePattern.Expand`, and a dialog's OK or Cancel through `InvokePattern` from a
  second thread (a job) while `CommandBars.ExecuteMso` waits for it; read the result through COM after. Its edit
  boxes give no text: press OK and read what the command made (`FormatConditions(1).Formula1`). Compare names by
  character codes ([char]0x78BA): a script file PowerShell 5.1 reads without a BOM loses its Chinese.
- A change to the number format engine must keep every case of tests/unit/excel_measured_formats.inc (TEXT of 159
  codes x 18 values measured in Excel 16): a fix for one code that breaks another fails the build. A test that
  disagrees with a measurement is wrong, not Excel: change it only after measuring, and say "measured" beside it.
- A change to the evaluator or to a function must keep every case of tests/unit/excel_measured_formulas.inc (8,554
  formulas measured in Excel 16, each set entered together over one grid, as on a person's sheet). The table is only
  ever regenerated from measurements, never edited to fit YCGrid; a formula that still differs is a `YC_SKIP` and
  its reason is in docs/excel-parity-backlog.md. A difference far behind the decimal point is written there, not
  chased: guessing Excel's order of operations takes days, and nobody at a sheet sees it.
- Some of Excel's methods keep the last call's settings for the arguments left out (Text to Columns' delimiters, Find's
  and Replace's LookAt, SearchOrder, MatchByte): a measured case gives every one, or it inherits the case before it
  (six Text to Columns cases split at semicolons they never asked for).
- Python's writes are calculated together, Excel calculates after each: a replay of Excel's steps whose difference
  goes away with a read between two steps (`Value`) is that, not the rule under test (rand3_112: a formula written
  just before the write that closed a cycle showed 0). `YC_BASICS_ONLY=<id>,<id>` replays only those cases of
  test_excel_basics, `YCGRID_EXE` an older build to tell a regression from a difference that was always there.
- Compare whole families of object-model calls with Excel, not one call at a time: `scripts\excel_diff.py` runs the
  same calls in its own hidden Excel and a hidden YCGrid, over ranges and states (several areas, one cell, a filtered
  list, a table, merged cells, a protected sheet, arrays) and lists what differs. Its first runs found 40-odd
  differences the random rounds, converged, never reached (a fill over a merged cell emptying it, a whole column's
  formula through a table writing "0", Validation.Add on a protected sheet): run it for every state a new feature adds.
- A probe script releases COM objects with `if ($null -ne $o)`: `$o -ne $null` on a Range makes PowerShell walk
  every cell (a whole column: Excel hung for two hours at 1 GB). Run each probe with a time limit, a progress log
  and `Start-Process ... WaitForExit`, write values a row or column at a time (one `object[,]` for a block ran out
  of memory), and read `Formula` / `NumberFormat` with the en-US culture (`InvokeMember(..., [CultureInfo]'en-US')`).
- A unit test with a time limit (the whole-sheet fills and row operations in `test_basics_dims.cpp`) guards against a
  freeze of minutes, not a second: give the limit through `Limit()` there, which allows a debug build three times
  the release limit and nothing under a debugger (`IsDebuggerPresent`: the coverage runner debugs the tests and ran
  one case for 152 s). A limit written for the release build failed the debug build and every coverage run.
- A message box (`ShowMessage`) never opens in a hidden instance (nobody could close it): it is recorded in the Errors
  window as a message, so a test of what a message says reads `ui.errors()` there; a question (`AskYesNo`) does open
  and is answered with Enter / Escape. `ui.state()["dialog_text"]` gives the open message's or question's text.
- GitHub's runners are English Windows, this machine is Chinese: what a test gets from the system language differs. `i18n::SetCurrent(ZhTW)` built no Chinese table where YCGrid had started in English, and a translation test passed here and failed only on CI. Run the unit tests with `YCGRID_LANG=en` (and `zh-TW`) to see both.
- Every unit test case starts in English (`test_listener.cpp`, a Catch2 listener): Catch2 runs the cases in random
  order, and on a Chinese Windows a case that read UI text ("Sum of Sales") passed only when a case that switched to
  English ran before it (one seed in a few failed). A case that wants Chinese switches to it, back after.
- A reader that keeps a view (`MsgReader(std::string_view)`) is never built from a temporary string: the test
  `MsgReader deep(std::string(...) + ...)` read freed memory, passed by luck for months, and failed only under the
  coverage runner (a debugger's heap fills freed memory). Keep the string in a local; the same goes for any
  `string_view` member.
- A hidden instance keeps a clipboard of its own, so tests never overwrite the user's: test a Copy by pasting into a
  cell (`k:c-V`) and reading the cell, never by reading Windows' clipboard.
- A modal dialog dims the sheet behind it: a screenshot check of a live preview under a dialog compares the cell with
  a cell without the preview, not with the plain color.
- Tests that send input to ImGui (`ui.input`) don't pass through `WM_NCHITTEST`: check what Windows is told about the
  title bar (`ui.state()["titlebar_clickable"]`).
- An Excel cursor ImGui has no shape for is a `render::GridCursor` value passed to `ImGui::SetMouseCursor` (main.cpp
  shows it); `ui.state()["cursor"]` names the last frame's cursor, so a test hovers and checks it.

### The UI item recorder

- `ui.items` must see what a person sees: when a control can't be found by its label (tabs), fix the recorder, not the
  test. A `Title###id` window keeps its first title as ImGui's name: report the title noted with `NoteWindowTitle`
  (the Format pane goes from "Format Data Series" to "Format Axis").
- It reports only what a person can see: a control scrolled or clipped out of view (a sheet tab past the tab bar's
  end) is left out, or a test clicks something hidden behind it, and one cut by its window's edge is reported by its
  visible part (both `ui.items` and the ribbon's `Seen`: the middle of a long menu's last, half shown entry was outside
  the menu). So is a docked window's tab in `ui.windows` (`tab`): a tab wider than its narrow node,
  reported whole, had its middle on the splitter beside it (chaos seed 5338). Keep what the user works on in view (the active sheet's tab), as Excel does.
- ImGui's `DockIsActive` reads false for the only window of a node in a floating dock group while it shows there with its tab: the recorder tells a docked window by `InDock` (screenshot_ui.cpp) and `ui.windows()` marks a group that floats over the sheets (`floating`). Missing from the list, a floating Clipboard group over the Error Log's tab let chaos double-click a tab nobody could see and report it stuck.
- ImGui's implicit Debug window gets its title bar at every frame and is hidden at the frame's end when nothing was
  drawn into it: the recorder lists its items only while it is written to (`WriteAccessed`), or tests see a
  Debug window nobody can.
- ImGui combo boxes report no label: every one goes through `ui::Combo` / `ui::BeginCombo` (yc_ui_items.h), which
  name it, so tests find it by its id as a person finds it by its place (`check_lessons.py`'s `combo-name`: noted one
  by one, most were nameless, Data Validation's Allow among them). A range box is also listed as `##rb.<key>`.
- `ui.items` also lists each id-only button by its bare id (every sheet tab's `##sheettab`, at the tab's place):
  find a control by its whole id (`"Data##sheettab"`), never by a suffix alone, or a test picks the nameless twin
  whenever the map's order puts it first.
- A control without visible text (the formula bar's Cancel / Enter / fx) gets an id-only label (`##fb_cancel`): a
  label "Cancel" there is found by tests (sorted top to bottom) before the open dialog's Cancel.

### ImGui

- `BeginPopupEx` on a closed popup clears `SetNextWindowPos`; draw each popup right after the button that opens it.
  When items overlap, the one submitted first gets the mouse: submit small handles before big objects.
- A window that appears because of a click (a pane following the active cell) takes the focus and closes the popup
  that click opened: give it `ImGuiWindowFlags_NoFocusOnAppearing`.
- State shared between a command and a window (an armed range box, an open dialog) is keyed by a global id
  (`ImHashStr("chart.range")`), not by `ImGui::GetID`, whose value depends on the window drawing it.
- Read the current window with `ImGui::GetCurrentWindowRead()`: `GetCurrentWindow()` marks it written, and outside
  every window (a helper called before its dialog opens) that makes ImGui's implicit Debug window appear over the
  sheet and take its clicks and drags (half the UI tests failed).
- A window that fits its contents (`AlwaysAutoResize`) must not size anything from its own width (`TextWrapped`,
  `SetNextItemWidth(-1)`, `SameLine(GetContentRegionAvail().x ...)`): it measures itself narrower every frame and the
  user watches it shrink. Dialogs go through `BeginModal`, which keeps the width they opened at; other windows call
  `KeepOpenedWidth` / `NoteOpenedWidth`. `ui.state()["shrinking_windows"]` names any that still do (ui_sweep fails).
- A dialog given a width (`SetNextWindowSize(ImVec2(w, 0))` before `BeginModal` with `AlwaysAutoResize`) keeps exactly
  that width and only its height follows the contents, as Excel's dialogs: a fitted width took the longest sentence's
  and stretched every full-width box (Encrypt Document opened twice too wide). Pick `w` near Excel's dialog and wide
  enough for its widest table or fixed-width box: `ui.state()["clipped_windows"]` names a dialog that cuts off its
  contents (the Sort dialog's table did; ui_sweep and feature_check fail on it). A dialog whose contents change with a
  choice (Data Validation's Allow) keeps one height: pad the changing part to its tallest, measured from that part's
  top, never from the window's height (that grows the window frame after frame). A resizable dialog (`BeginDialog`)
  gets its height from its contents when it opens (`ImVec2(w, 0)`), with lists of a fixed height inside: a height
  guessed in pixels left empty space under the buttons. A part of it that appears later (Name Manager's Refers to
  and its question, Insert Function's description) keeps one height too, or the dialog scrolls and hides its
  buttons; `clipped_windows` names a resizable dialog that shows a scroll bar. Review dialogs by their pictures:
  `YCGRID_DIALOG_SHOTS=<folder>` makes feature_check save one of every dialog it opens.
- `SameLine(x)` inside `BeginGroup` counts from the group's start, not the window's: label / box rows that must line
  up are laid out without groups.
- A failed ImGui check (`IM_ASSERT`) only prints in a release build and stops a debug build: tests read
  `ui.state()["imgui_asserts"]` (ui_sweep fails a command or dialog that raises it); never move the cursor
  (`SetCursorScreenPos`) without submitting an item after it, also after the last entry of a menu or list.
- A text box given the keyboard with `SetKeyboardFocusHere` takes it a frame later and starts with its caret at 0, so
  keys sent right after it opens are lost or land at the start. The cell editor activates its box on the frame it
  opens (`NavActivateId` around `InputTextMultiline`) and, when ImGui's text state is its own from before, reloads
  it (`WantReloadUserBuf`, `ReloadSelectionStart` / `End`) with the caret it wants before the box reads the keys;
  otherwise it hides that one frame's keys. ImGui runs one callback a frame (Edit when the text changed, else
  Always): what Always applies waits for a frame without typing.
- `AddConcavePolyFilled` cuts the polygon into triangles at every call, and a long run of points on one line (an area
  chart's base and the empty cells along its top) made it take minutes a frame: a polygon a chart builds from data
  drops its collinear vertices (`DropCollinear`), and a chart that draws a mark a category draws at most 2,000 of
  them (`Thinned`), as the radar draws 720.
- `ImDrawList::AddRect` and `AddPolyline` take thickness before flags in this ImGui version (`rounding, thickness,
  flags`; `col, thickness, flags`): the other order is a deleted overload.
- ImGui owns Ctrl+Tab / Ctrl+Shift+Tab (its window switcher, even without keyboard navigation) unless
  `ConfigNavWindowingKeyNext` / `Prev` are 0: YCGrid sets them to 0 so the keys switch workbook windows as in Excel.
- `IsWindowHovered` is false while any item is active, the window's own too: the grid's wheel waited for the end of
  its own selection drag. What must work during a drag or an entry (the wheel over the sheet) adds
  `ImGuiHoveredFlags_AllowWhenBlockedByActiveItem`.
- `IsWindowHovered(ImGuiHoveredFlags_ChildWindows)` is also true over a popup opened from that window (a chart's flyout
  opened by the sheet): add `ImGuiHoveredFlags_NoPopupHierarchy` where the window acts on the mouse (the sheet's wheel).
- The sheet calls its overlays (`opt.overlay`) inside its cells' font, zoomed: a menu or popup opened there (the filter
  menu, Paste Options, a slicer's menu) pushes the UI's font (`ui::F().ui`) around it, or it grows with the zoom (the
  Paste Options headings were 30 pixels high at 200%). What sits on the cells and zooms with them (a slicer, a shape's
  text) keeps the cells' font.
- ImGui scales a font so that ascent + descent equals its size: a font's real line height (rows that fit a taller
  font) comes from its file through GDI (`tmHeight` at a known em size, sheet_actions.cpp `LineHeightPerEm`), not
  `ImFontBaked`.

### Windows, dialogs and the ribbon

- A modal dialog (`BeginModal`) blocks the sheet: a box that takes cells must use `RefEdit` and let the user reach
  the sheet (a non-modal window, or the dialog shrunk to the box while picking).
- A range box takes a cell clicked on the sheet while it has the keyboard (Excel's RefEdit), not only through its
  button: the press the modal dialog kept is handed to the grid (`Pick::direct`, its click time reset so it isn't a
  double-click that edits the cell) and the dialog comes back when the button goes up.
- A letter taken from a label (a KeyTip without Excel's fixed key) comes from the English label
  (`i18n::SourceOf`): a Chinese label has none, and Alt, H, L, H, G reached nothing in the Chinese UI.
- Picking cells for a range box (RefEdit) puts the sheet's selection back afterwards, as Excel does, and the grid
  leaves a cell entry in progress alone meanwhile (Insert Function's arguments): a dialog that reads the selection
  at OK (Data Validation) applied its rule to the cells picked for its list, and a drag pointed in the cell's
  formula instead of filling the box.
- An Escape that closes a drop-down (a combo, a menu) over a dialog closes nothing else: main.cpp locks the key for
  the rest of that frame (`SetKeyOwner(..., LockThisFrame)`), since ImGui closes the drop-down inside NewFrame and
  every dialog's own Escape check then closed the dialog too.
- A box armed to take cell selections must let go when it isn't drawn and after one pick outside a dialog, or later
  clicks on the sheet silently change it.
- Live updates (a range box filled while the mouse drags) run on every frame of the drag: derive titles, legends and
  defaults from the final state, never from the first event.
- A dialog that leaves the sheet usable (a window, not `BeginModal`: Insert Symbol, Mark Parameter) closes on Escape
  like any dialog: both stayed open, since ui_sweep pressed Escape only on modal dialogs. Its dialogs pass also
  takes every new floating window with a Cancel or Close button and fails one Escape leaves open.
- Escape and the close box are a dialog's Cancel, also where Cancel does more than close: Goal Seek Status and Solver
  Results put the original values back, and a page shown over another (Add Constraint, Add Scenario) goes back to it.
  `BeginDlg(..., &cancel)` (whatif_ui.cpp) leaves the dialog open on Escape and sets `cancel` for the page to act on;
  closing every such dialog at once kept the values Goal Seek had found.
- A dialog's tabs go through `BeginDlgTabBar` (dialogs.cpp): Ctrl+Tab / Ctrl+PgDn and Ctrl+Shift+Tab / Ctrl+PgUp move
  between them as in Excel. Every dialog called `ImGui::BeginTabBar` itself and none took the keys (`check_lessons.py`'s
  `dlg-tabs`).
- A dialog opened over another (Confirm Password, New Range) takes Enter; the frames after it closes, the dialog under
  it must not take that Enter as its own OK (protect_ui's `ParentOk`), or both close at once.
- File > Options edits a copy that OK applies and Cancel drops: a new option binds to `cfg` (the copy) and gets its
  `YC_OPT(...)` line in the OK handler (file_ops.cpp), or OK doesn't apply it; side effects (engine posts) go in OK.
- Only windows that show a sheet pass typing to the cells (`KeyboardTarget`, main.cpp): a new sheet-like window is
  added to its list; every other pane keeps the keyboard when clicked, or a click on it then typing fills the selection.
  A window that only hosts others (a container) is not sheet-like: what it holds takes the focus when clicked, and a
  click on its free part then typing wrote the active cell behind it (chaos seed 5501, ten million cells by Ctrl+Enter).
- A hidden instance never shows its window: code that restores a window placement (`SetWindowPlacement` shows the
  window) or activates it checks `s.headless` first; a visible hidden window makes the next file dialog real and wait.
  Nor does it open another program's window (VS Code, Explorer): every test that ran Developer > VS Code left a VS
  Code window behind, and one VS Code process holds all its windows, so YCGrid can't close the ones it opened without
  the user's. It notes what it would open instead (`ui.state()["vscode_request"]`, the status bar), and tests read that.
  Nor does it list or activate the user's YCGrid windows (Switch Windows, Ctrl+F6): with the user's YCGrid open,
  Ctrl+F6 in a test went to it, and the test failed only on that machine. Every file, folder, web page or program
  opened on the desktop goes through `OpenOnDesktop` (settings.h; Python's `excel._open_on_desktop`), which opens
  nothing from a hidden instance: a click on the screenshot toast's picture opened the picture viewer from chaos
  rounds over and over and filled the user's screen. `check_lessons.py` fails on a `ShellExecute` or `startfile`
  anywhere else (`shell-open`, `py-startfile`).
- A file written for as long as something runs (a recording) reaches the disk as it goes: its header at once and
  its data at least every second (`TapeWriter::flush_every_ns`), and it opens shared for reading. A 1 MB stdio
  buffer kept even the header of a recording whose YCGrid was ended, and the replay said it wasn't a recording.
- No fixed-size list for things the UI adds to (the title bar's clickable areas were `RECT[8]`): adding the Undo /
  Redo arrows pushed minimize and the X off the end, Windows took the X as caption and it no longer closed YCGrid.
- A ribbon button runs on the mouse release: a double-click is `IsMouseReleased(0) && io.MouseClickedLastCount[0] >= 2`
  (`IsMouseDoubleClicked` is only true on the second press, so the release saw a second plain click). A large split
  button opens its menu from its lower part; its middle runs the command.
- The ribbon's app hooks (`menu_picture`, `menu_entry_height`, `tile_menu`, `submenu_body`) are shared by several
  files: always chain the one already set (`[prev = St().tile_menu]`, call `prev` for ids that aren't yours), never
  assign over it (Cell Styles' tile menu silently replaced Format as Table's).
- Excel's KeyTips (`FixedKeyTips`, `MenuKeyTips` in ribbon.cpp) are keyed by the command id the ribbon item or menu
  entry really uses (`ConditionalFormatting`, not a made-up `CondFormatMenu`): a wrong or renamed id silently falls
  back to two letters of the label. Test the sequence a person types (Alt, H, O, I), not the map.
- A modal dialog owns the keyboard: the ribbon's KeyTips stay off while one is open (`GetTopMostPopupModal`; the
  "visible" variant reads false there, since the ribbon draws before the dialog), or Alt, H in Paste Special ran Home's
  commands under it. Excel 2003's menu keys (Alt, E, S, V) are `LegacyKeys` in ribbon.cpp, taken when no tab has the
  letter; a test sends a dialog's letter as a key (`k:V`), since its options read keys, not typed characters.
  So does a dialog that isn't modal (Find and Replace, Select Data Source, Symbol) while it has the keyboard: it calls
  `NoteDialogKeyboard()` after its Begin, or Alt+A (Replace All) opened the Data tab's KeyTips behind it. Its options
  take Excel's letters through `DlgKey` (the letter alone off a text box, Alt+letter always): Ctrl+-, R, Enter and
  F5, Alt+S, K, Enter are how people delete a row and select the blanks.
- KeyTips register while they show: a held Alt + a letter (Alt+W pressed together) turns them on and is matched the
  next frame, once the tabs' are there; matched at once against none, it did nothing. ui.input's chords hold the
  modifier keys too (`ImGuiKey_LeftAlt`), as Windows sends them: with only the flag, no test pressed Alt as a person.
- A menu people open from the keyboard takes Excel's letters (its underlined ones): Alt+Down, E, a few letters, Enter
  filters a list, Shift+F10, V and Ctrl+V, Ctrl, V paste values; the filter menu, the cell, column and row menus and
  the Paste Options pictures ignored every letter. A letter acts only while that menu has the keyboard (`IsWindowFocused`, no item active), and only a letter
  no other entry of the menu shares runs its entry (a shared one only moves Excel's highlight). A new menu a person
  opens with a key gets its letters, tested by the keys a person presses.
- ImGui opens a popup where the mouse is: a drop-down sets its place under its button before its Begin (the ribbon's
  `PopupBelow`), or one opened from the keyboard shows wherever the mouse was left (Alt, H, N put the Number Format
  list in the screen's corner). It takes the arrows and Enter too (`St().menu_frame` turns ImGui's keyboard
  navigation on while one shows), and the Enter that picks an entry is not the sheet's (the active cell moved down).
- A shortcut is handled in one place: Ctrl+F1 was switched by the ribbon's own key check and again by main.cpp's in
  the same frame, so the ribbon never collapsed (since the first release). Before adding a key, grep for its chord;
  test the key a person presses and that the state changed (`test_parity_habits.ShortcutsTest`). `check_lessons.py`'s
  `key-once` fails on a chord both main.cpp and a widget in src/ui take.
- `TR("text##id")` cuts the text at `##`: don't put `##` in UI text (number format examples like `#,##0`).
- zh-TW.json's keys and values are C string literals: a backslash in the text is written as two there, as in the key
  (`sheets\\\\`). One backslash before a Chinese comma escaped the comma's first byte, the status line after Alt+F11
  was invalid UTF-8 and every `ui.state()` in the Chinese UI failed to decode. `i18n_build.py check` fails on such an
  escape and `test_i18n.cpp` checks every translation is valid UTF-8.
- UI text an engine helper returns (error rule titles) is wrapped in `TR` where it is written, so
  `i18n_build.py extract` finds it; a `TR(variable)` at the caller is never extracted.
- Shortening text for the UI (`substr(0, n) + "..."`) must cut at a UTF-8 character boundary: half a Chinese
  character in a list entry (the built-in Chinese day lists) made `ui.items` fail to decode for every later call.
  Never `substr` a text by bytes: `core::Utf8Prefix` (bytes) / `core::Utf8First` (characters) in `core/utf8.h`; a
  limit Excel counts in characters (31 for a sheet name) is `core::Utf8Length`, not `size()` (a CSV's long Chinese
  name cut at 93 bytes made every Python request fail to decode, and 93 ASCII letters passed as a sheet name).
- A request that moves the active sheet away (deleting or hiding it) moves the focus within the same request (`OnUi`):
  `view.focus` follows only on the UI's next frame, and `ActiveSheet` read right after a delete raised "The sheet has
  been deleted" (Excel activates the next visible sheet to the right at once, else the left).
- A refusal a person meets while working a sheet built for use (a protected sheet, a protected workbook structure)
  opens no message box: `AppState::NoteRefusal` puts the message in the Error Log as an error and a red line beside
  its status bar entry (`ui.state()["error_log"]["refusal"]`), as read-only mode says its refusals on the status bar.
  A new refusal of that kind goes through it, not `ShowMessage` (a box at every touch of a locked cell, decided
  2026-11-25; kept on purpose in docs/excel-parity-backlog.md).
- A dialog refuses a value outside Excel's range with Excel's message and keeps its text (`RefuseSize`), never
  clamps it silently: a row height of -5 clamped to 0 hid the rows. There is one modal at a time (`s.dialog`): a
  message shown from a dialog replaces it, so the dialog notes to come back (`g_dim_return`) once the message closes.
- A key never waits in the frame for the engine's earlier work: Ctrl+C asked `Engine::Call` for the copied cells,
  queued behind the million formulas filled a frame before, and the frame took 2.4 s (only when the keys came fast,
  once in a check_all). A busy engine (`BusySeconds`, `UserActionsPending`) gets the work posted and the UI takes the
  result on a later frame (`ApplyPendingCopy`, as `ApplyPendingCommit`); what needs that result waits in a
  continuation (`PasteAfterCopy`), never in the frame, and Python's requests, which wait for an answer anyway, wait
  (`WaitForPendingCopy`, `AppState::in_request`). Every `Engine::Call` a key or a click reaches has this shape: an
  entry under a cell with text (the Flash Fill preview asked the engine every frame), an array formula, a name in
  the Name Box, F9 in the editor, Format Painter and Trace Precedents each waited 1.4 s to 1.7 s; a command that
  needs the answer goes through `DeferWhileEngineBusy` (it runs once a marker posted after it comes back, and a
  script's next key waits for it, as typing ahead does in Excel), what is asked every frame is posted and shown
  when its answer comes. Time a key a frame after a heavy step, not only on an idle engine
  (`test_live.UserFlowTest.test_keys_that_ask_the_engine_keep_the_screen_live_while_it_works`).
- Work that grows with the selection doesn't run on the UI thread for every key when nothing needs it yet: Ctrl+C of a
  million rows wrote their text out (1.5 s) for a clipboard nobody read. A large copy promises its text
  (`SetClipboardData(CF_UNICODETEXT, nullptr)`, written in `WM_RENDERFORMAT`) and keeps out of Windows' clipboard
  history, whose service would ask for it at once; the Clipboard pane collects only while it is shown, as Excel's
  Office Clipboard. Time a new command over a million rows (`trader_session.py`'s big workbook) before calling it done.

### The sheet: selection, editing, objects

- Enter and Tab move the active cell; Shift reverses them and never extends the selection: a step helper that
  extends on Shift (`step_to`, for Shift+arrows) is called from Enter / Tab with `step_moves` set, or Shift+Enter
  on one cell selected two cells and stayed put.
- A shortcut written on a command (`Make("PickFromList", ..., "Alt+Down")`) is only its label: the key works once
  the shortcut table or a handler runs it. Test the key a person presses, not the menu: Alt+Down did nothing.
- An editor opened by a double-click must not treat that same click as "clicked away" on its first frame.
- A switch that stops editing (read-only mode, whatever turns it on: the button, the idle timeout, Python) drops the
  entry in progress as Escape does, and offers no drags it would refuse: an entry left open took every later click as
  a reference and every key as text, and Enter only answered "There's a problem with this formula"; a drag asked
  "There's already data here" for a move that was then refused.
- Read-only mode refuses at two levels: every Document entry point that changes the workbook calls `Refuse`,
  `RefuseStructure` or, for what protection doesn't govern (rules, notes, names, protection itself, Undo),
  `RefuseReadOnly`; and the command guard (`ReadOnlyRefusesCommand`) keeps such commands' dialogs shut. A dialog
  open when the mode comes on asks `RefuseReadOnlyUi` at OK and never shows an empty `err`: the engine's refusal (0,
  false, "") read as a result said "No duplicate values found." or showed a blank message box. An in-place box (a
  tab's name, a note) doesn't open in read-only mode and closes unsaved when it comes on.
- A button floating over the cells (Paste Options, Auto Fill Options) goes away on Escape, as Excel's: left there it
  covered the cell below and to the right, and the next click on that cell opened its menu instead of selecting.
- An in-place editor (a note's box, a sheet tab's name) takes the sheet's keyboard only while it has the focus, and
  Escape always closes it: one left open by a test that stopped half-way made every later typing test fail.
- The character that starts an entry is put into the editor, not typed into it: no edit event comes for it, so what
  reacts to typing (AutoComplete, `EditState::ac_first`) also runs once for it, or one Chinese character never
  completes.
- The text that starts an entry can be several characters (a Chinese IME commits `12.5%` in one frame): what the
  first character decides (automatic percent entry adds a %) looks at all of it, or 12.5% was the text 12.5%%. Test
  a typed entry as one `t:` step too, not only one character a step.
- The cell editor's keys work only while its `EditField` is drawn: keep it alive (invisible) when its cell scrolls out
  of view, as for any widget that owns a live entry.
- The editor's caret (`EditState::caret`) follows every change of its text in the same frame: InputText calls its
  Always callback before it inserts a typed character, so the Edit callback updates the caret too, and `SetEditText`
  sets it at once. An arrow the frame after a typed `,` read the caret before the comma and committed `=SUM(F2,)`
  instead of pointing. Test keys one frame apart, as a fast typist sends them (`t:,|k:UpArrow` in one script).
- A window drawn over the cells while a formula is typed (AutoComplete, argument hints) takes the clicks meant for
  pointing at cells: show it only when there is something to choose, and give a hint `ImGuiWindowFlags_NoInputs`.
- State the grid keeps for a reference being typed (its text span, `$` signs, whole rows / columns) is reset when the
  mouse is released, or the next keyboard pointing inherits it.
- A click on the sheet while a formula is typed takes the edit box's focus for a frame, and the box writes its own
  text back: what the click wrote into the text is lost and only what the drag writes again on the next frames stays.
  State the click adds (Ctrl+click's comma, `EditState::comma_at`) is kept until the mouse is released and put back.
- What the grid draws is what it keeps: a selection over a merged cell is stored grown to the whole merge (`expand_merges`
  at the frame's start), not only drawn so, or `Selection.Address` and the keys that extend it see a smaller area.
- Keys that walk the sheet (Ctrl+arrows, `CtrlJump`, Enter / Tab in a selection) treat hidden rows and columns as if
  they weren't there; a walk that lands on a hidden cell leaves the active cell where nobody sees it.
- A sheet may show no cell at all (every row or every column hidden): the grid still handles the keys, hosts the
  entry's invisible box and draws the headers, scroll bars and tabs (the blank main pane). An early return there
  when no pane had cells left every key going nowhere; code that needs a drawn cell checks `Pane::Valid()` instead.
- A selection has one shape: whole rows / columns are stored as full-width / full-height rectangles with the matching
  `SelectMode` (header click, Shift+Space, Name Box, Python alike), so the keys that extend them see the same thing.
- What a person does to the selection of a filtered list acts on the rows it shows (`engine::FilterShownRuns`):
  clearing, copying, Ctrl+Enter and Ctrl+D did, but deleting whole rows deleted the hidden rows between them. A new
  action on a selection asks whether a filter hides rows in it. So does Excel's object model on an AutoFilter's list
  (measured, `data14_` to `data16_` cases): one value or formula written, `ClearContents`, `Clear`, formats, `Replace`,
  `FillDown`, `FillRight` and `Rows.Delete` touch the rows it shows (a formula moved to each run of them); deleting
  cells shifted up takes the shown rows whole, and inserting cells that shift the list is refused. Rows hidden by
  hand get everything. An array of values and `Copy Destination` are their own rules. A table other than the first
  filters with its own `Table::filter`, not the sheet's: `FilterShownRuns` reads both, and a new rule about filtered
  rows does too (Delete and Ctrl+D wiped a second table's hidden rows). "Python acts on every
  row, as Excel's" was written here without a measurement, and a script clearing a filtered column wiped the rows it
  hid. The rows a filter hid (`AutoFilter::hidden`) follow inserted and deleted rows (`ShiftSheet`).
- A paste that repeats the copied cells over a selection goes through `Document::PasteRepeated` (Ctrl+V,
  `Worksheet.Paste`, `Range.Copy Destination`): one copied cell over a filtered list fills the rows the filter shows,
  a block of cells fills every row, hidden or not (Excel 16, measured all three ways). Each path tiled with a loop of
  its own and wrote the copied cell into the hidden rows too.
- Cut / copy mode is `CopyModeOn` (the outlined cells): code that pastes the copied cells checks it, and an action
  Excel ends it with (Esc, typing, insert / delete) calls `EndCopyMode`, or a later Ctrl+V moves cells the user let go.
  Measured in Excel 16: the ribbon's Insert / Delete Sheet Rows and Columns end it, and so do the object model's
  Rows / Columns / Range.Delete, while its Rows / Columns.Insert inserts the copied cells and keeps it (Python's
  Delete kept it until then). `Worksheet.Paste` without a Destination fills the selection (measured too).
- State kept for the button after a paste, a fill or an insert (`last_paste`, `last_fill`, `last_insert`) belongs to
  one sheet: code on another sheet checks `.sheet` too, or one paste blocks that corner on every sheet.
- Grouped sheets (`[Group]`) get every edit made on the active sheet: a command that changes cells, formats, rows or
  columns loops over `GroupTargets(s, sheet)`, or the other grouped sheets silently miss it.
- A drag the right button started ends on the right button's release: code that ends drags on `!IsMouseDown(Left)`
  checks the button the drag began with (`view.right_drag`), or a right-drag ends on its first frame.
- Page Layout view draws the printed pages, not the grid: what the grid does on cells (fill handle, drag and drop,
  selection, editing) is redone on the pages through `render::GridActions` / `HandleGridActions`, never by assuming
  the grid underneath sees the mouse.
- A shape's handle reads a drag on the same curve it is drawn on, and a handle dropped where it is changes nothing
  (`test_coverage_shapes.cpp` checks every handle of every shape): the Block Arc's thickness and the Circular
  Arrow's handles read the shape's own ellipse or a circle while they sat on an inset one, and jumped up to 7 points
  when they were only clicked on a shape wider than tall.
- Code that runs while an object is selected (keys, ribbon commands) must act on the object, not on the hidden cells:
  route shortcuts through `ObjectShortcuts` and wrap Home's commands (Copy, Bold...) the way `RegisterShapeCommands`
  does.
- An object selection (chart, shape, slicer) belongs to its sheet: code that reads it for the ribbon or a command
  checks it is on the focus sheet, or a contextual tab and its commands stay on another sheet.
- State picked per frame from the active cell (the active PivotTable) must stay while a dialog or drop-down opened from
  an overlay button is open, or its OK acts on nothing.
- A view's frozen or split part starts at `frozen_top` / `frozen_left`, and `v.first` / `h.first` is the lower
  pane's: code that freezes, splits or unfreezes keeps the one and moves the other (`UnfreezeView` scrolls back to
  where the frozen part began). Reading the origin from `v.first` while panes were already split put rows 1:3 out of
  sight, and unfreezing left them there. Excel's `ScrollRow` is the scrolling pane's when frozen, the top-left pane's
  when split (measured).
- What acts on "the selection" for a sheet (the Name Box, Go To, Python's Select / Selection) takes
  `AppState::ViewFor(sheet)`, the focused window's view, never `Win(sheet).view`: with a second window (New Window,
  Side by Side) the Name Box selected in the first window while the keys typed into the second one's cell.
- An event that follows a state the UI changes in many places (the selection, the active sheet) is found by comparing
  that state once per frame (`TrackSheetEvents`), not fired from each code path: a click, the Name Box and Python's
  `Select` each missed `SelectionChange`. A Before* event never makes the UI thread wait for Python: `AskThenAct`
  acts on the host's answer a few frames later (2 s at most), and runs at once when no handler exists.
- A command that writes results beside the selection (AutoSum's totals) finds the cells it may write first and never
  writes over a value or another formula: AutoSum over B2:C4 wrote its totals over the label in B5 and over D4's
  formula. Measure where Excel puts them instead (the first free row, a total already there redone).
- A command over a selection is measured on whole columns, whole rows and the whole sheet too: AutoSum over the
  whole sheet wrote a total in every row and every column (a million formulas), where Excel 16 writes none and
  edits the active cell with that cell's own proposal (`=SUM(C5:C9)` in C10 under numbers, `=SUM()` in A1, read from
  its formula bar); whole columns get their totals in the last row, whole rows in the last column (measured,
  chaos seed 5324). Measuring it took a hidden Excel left editing: COM refuses every call then, and a posted
  Escape to its `EXCEL7` window ends the edit, so the probe could read what was written. Each of a command's cases
  is measured on them, not only its first: whole columns whose last one is empty got a row total in every row (a
  million again, chaos seed 5643), where Excel writes them as far as the data (`CommandBars.ExecuteMso("AutoSum")`
  in a hidden Excel writes a range's totals, readable through COM).
- A key the grid handles checks the modifiers a shortcut adds to it: Ctrl+PgDn switched the sheet (main.cpp) and the
  grid's PageDown, which asked only about Alt, paged the sheet it left a screen down at every switch, unseen until a
  person came back to it. Alt+Enter moved down as Enter and Alt+Home went to column A, where Excel 16 does nothing
  (measured by keys posted to its sheet window). Test a shortcut by the state it leaves behind as well as the one it
  makes, and measure a key with each modifier before letting it fall through to the plain key's action.
- Typing is not a command: F4 / Ctrl+Y repeat the last command (`AppState::repeat_id`, noted by
  `CommandRegistry::on_run`), and an entry is neither repeated nor ends what they repeat. A "Repeat Typing" written
  as Excel's without a measurement typed a cell's old value over the next cell after F2, Enter, F4.
- Where Tab goes from one cell is `render::TabTarget`, for Tab on the sheet and Tab ending an entry (`MoveActive`)
  alike: a protected sheet's next unlocked cell, a table's next row from its last column, a row added from its last
  cell. Tab after an entry kept a rule of its own and went to the locked cell beside it, and neither knew tables.
- In Enter mode a key that moves on the sheet ends the entry, then does on the sheet what it does there
  (`GridActions::nav_key`, replayed through `GridView::nav_replay` once the editor is gone): the sheet's key code is
  the one place a move is made, never a copy of it in the editor (Ctrl+Home moved the caret, Shift+Right moved one
  cell and the next typing went into the wrong cell).

### Engine, commands and the file format

- What only one workbook per YCGrid makes global (the 1904 date system, `formula::SetDate1904`; the calculation's
  thread limit) is set by the Engine from the Document it runs, never by every Document: an importer's temporary
  Document would set it back, and unit tests reset it in `test_listener.cpp` so one case's setting doesn't reach the
  next. It is set before the Document is made (`WithItsDateSystem`): the constructor calculates every formula, and a
  1904 workbook opened in a 1900 session kept =DATE(2024,1,1) at the 1900 system's number.
- A setting that changes what a number is (precision as displayed) is applied by every path that stores one: a typed
  value (`SetValue`, `SetValues`), a result (`StoreResult`), each cell an array spills (`UpdateSpill`) and a step's
  cells at Commit; turned off, it calculates the formulas again (Excel 16, measured). Rounding to what a cell shows
  reads the formatter's own digits (`RoundAsShown` through `SplitDigits`), never a second rounding of its own: the
  binary `round(x * 100)` stored -75.19 for a cell showing -75.20 (gaps_fuzz seed 61311).
- What only one path does goes where that path is, not where every path meets: a formula entered on a sheet whose
  calculation is off shows 0, and that 0 set in `RegisterFormulaCell` (Undo, moves, copies and inserts register
  formulas too) made Undo there show 0 (doc_fuzz --gaps 700022). Ask which paths reach the shared code first.
- The other way round, a rule measured for every path is not patched onto one: Python's Ungroup put back the hidden
  rows `Document::GroupOutline` had shown (the bridge's `HideAgain`), so Alt+Shift+Left kept showing them, and the
  same patch made Python's Clear Outline keep rows hidden that Excel shows. Measure the rule through the UI and the
  object model both, and change the Document.
- A calculation that stops part way (an interrupt) leaves nothing taken out half done: the cells a cycle set aside
  (`cycle_aborted_`, out of the pending set) wait again, or the calculation that goes on finds no circular reference
  (doc_fuzz --gaps 763382). A new early return from `Recalc` puts back what the pass took out.
- What a report shows and what reads it are one computation: GETPIVOTDATA under a Top 10 summed every item while the
  report showed the filtered ones (`PivotReport::kept_records`). A new filter of the report is applied in the lookup
  too, and measured for drill-down apart (Show Details ignores value filters in Excel 16).

- A choice the UI makes on what the engine will have done (F4 and Ctrl+Y: Redo or Repeat) reads
  `Engine::UserActionsPending` and the engine's latest snapshot, not the frame's: a format posted the frame before is
  in no snapshot yet, and a Redo "enabled because the frame is behind" swallowed the key.
- Undo only the step your own action made: read `Document::UndoDepth()` before `Begin` and after `Commit`; an action
  that failed changed nothing and left no step, and a blind `Undo()` takes back the user's previous edit.
- A counter read before and after a command (refusals, changes) is per Document: a command that replaces the workbook
  (`Engine::Replace`) starts a new one at 0, so compare the Document too or the unsigned difference reports old events.
- A result handed back across threads goes back after everything the caller reads with it is done: `Engine::Call`
  set its promise inside the command, before the engine added the command's refusals to the request's
  `BlockScope`, so the bridge sometimes saw 0 and a write refused by protection reached Python as done (a unit test
  failed only under the coverage runner). `Call` answers through `PostThen` now.
- A thread id that says "this is the worker thread" is set before the thread starts (`Engine::Start` stores
  `thread_.get_id()`), not inside it: until the new thread ran, the caller still counted as the engine, so a
  `Call` right after `Start` ran beside the engine and was never published.
- A merged cell is one thing to every operation, measured in Excel (`merge2_` cases): cells shifted across part of it
  unmerge it (`ShiftSheet`), a merge over part of it grows to take it (`Document::Merge`), the fill handle repeats it
  (`AutoFill`), and Python's ClearContents or FillDown over part of it is refused or does nothing. A new operation
  on a range asks what it does to a merge it cuts.
- `Document::Merge` is Excel's command: the first value in reading order goes to the upper-left cell and every cell
  takes that value cell's formats (measured). An importer records a file's merged areas with `AddMergedArea`, never
  `Merge`, or the cells the file saved under a merge lose their values and formats.
- Sort, Remove Duplicates and filters find their range with `FindDataRange` (a table's data body, the AutoFilter's
  list, the region, the selection with Excel's Sort Warning): never a fresh `CurrentRegion` guess per command.
- A data tool is measured in all its modes before calling it done, not only its default: `scripts/excel_basics_cases.py`'s
  `data12_` cases found Subtotal's summaries above (the grand total's range, the formats of inserted rows), a second
  Subtotal kept beside the first (its grand total's place) and Advanced Filter's computed criteria (never worked out
  per record) wrong, and Consolidate's functions other than Sum wrote 0 where Excel leaves a blank.
- A tool that makes a new sheet (Forecast Sheet, reports) checks everything it can refuse before it adds the sheet:
  Forecast Sheet refused an early Forecast Start after adding its sheet, and an empty sheet stayed before the data's
  (where the next command on "the first sheet" then ran).
- A tool that inserts rows and writes formulas (Subtotal) inserts every row first, then writes the formulas at their
  final places: a formula written before an insertion above it moved but kept waiting for a calculation at its old
  place.
- A circular reference without iterative calculation isn't calculated: its cells keep the values they show, a formula
  typed (the one closing the cycle, or one reading it) shows 0 (`fresh_formulas_`), a formula copied or filled shows
  the value it was copied with, and what reads the cycle isn't calculated either (`cycle_block_`) and keeps its value
  (Excel 16, measured 2026-11-15: COUNTA, SUM and COUNT over their own cell are 0, ROWS isn't a cycle); otherwise
  each calculation read its own last value and `=C1+1` grew by one every time.
  With iteration the cycle's cells are calculated again at every recalculation after an edit or F9: MaxIterations
  rounds, each cell once in order (`ResolveCycles`), as Excel (measured to the last bit).
- A reference a shift moves is checked against the sheet's edge where it is moved (`PastEdge` in `AdjustAst`): pushed
  past XFD it is #REF! (a range's end stops there), as Excel's, or its text reads #REF! while the formula keeps an
  offset past the sheet, and two formulas of one text became one only after a save and an open (doc_fuzz seed
  33240081). Rows follow `ShiftSheet`, which grows the sheet past its rows instead.
- Finding what to calculate grows with the formulas, not with their chain's length: `Propagate` marks a cell it finds
  in the round's own tiles at once (`ChangedCells::Mark`), so a chain of a million formulas down a column takes a
  round per tile; a round per cell, each scanning its tile's entries, made Ctrl+' over the chain's first cell take
  7.5 s (basics seq430649). Time an edit at the top of a long chain, not only a wide recalculation.
- An array's cells are blocked with its anchor: what reads any cell of an array whose anchor is in a cycle waits as a
  reader of the anchor does (`CycleSpillAt`, in the cell and range reads alike). Only the anchor's key was in
  `cycle_block_`, and `=SUM(D2:D5)` over part of a circular `{=C2:D3+1}` was calculated from its zeros where Excel 16
  keeps the value it shows (cyc_array_self). An index keyed by a cell (blocked, pending) says what it means for the
  cells an array covers.
- Part of an array formula (Ctrl+Shift+Enter) can't be changed, by the UI, Python or a structural change: the engine
  refuses (`BlockReason::Array`), not only the UI's typing; a row inserted through the array resized it. Its anchor
  alone is part of it too (Excel 16 changes nothing there, measured): let through as "a range written cell by cell",
  a plain formula took the anchor while the array's record and its other values stayed, and a formula reading them
  was never calculated again (doc_fuzz seed 17241693). A write that replaces the array covers all of it.
- So is an insertion or deletion of rows or columns through a PivotTable report (`RefusePivotShift`, Excel's "...it
  will affect a PivotTable"): lines that hold all of it move it or delete it with them (Excel 16's object model,
  measured 2026-11-16: Rows.Insert / Delete and Columns.Insert inside the report raise, Rows(1).Insert above it moves
  it a row down). A row inserted inside the report stretched it over an empty row. A new object that owns cells (a report, a spill of its own) asks the same
  question in all six structure entry points, not only where the UI calls them.
- Text is UTF-8: a wildcard `?` is one character and a scan for a match starts only at a character's first byte
  (`WildcardMatch`, `FindAt`); `?` taken as one byte made COUNTIF of a family name and `?` find nothing. Case is folded by
  `formula::FoldCase` (beyond ASCII, same UTF-8 length) wherever text is compared, in formulas and in the engine
  (`engine::Lower`, Find / Replace, filters, rules); a local ASCII-only lowercase drifts from it.
- A command made of several writes (AutoFill: a paste a cell, then the source's merged cells repeated) asks once
  for all it writes before any of it (`Refuse` over the fill's target): its pastes refused one by one over part of
  an array while the merges were made anyway, a change without an Undo step (doc_fuzz seed 11652641). Excel refuses
  a fill as a whole too: one locked cell in a protected target leaves the unlocked ones empty (measured).
  So does a command that inserts cells as one of its steps: `InsertCells` answers false when it refused, and the
  command asks `RefuseInsertCells` before its first change. Ctrl+T without headers over part of a filtered list had
  its header row's insertion refused and made the table anyway, a formula turned into header text (basics seq2708);
  a table's header and totals rows, ListRows.Add and a bound table's growth wrote over the cells in the way the same
  way. A table over part of the sheet's AutoFilter list is refused, one over all of its header row and columns takes
  it over, its criteria kept only when it is the list (Excel 16, measured). The list is what Excel reads back:
  an AutoFilter set on whole rows or columns goes as far as the data when it is set (`SetAutoFilter` applies
  `AutoFilterExtent`; `3:4` over data in A1:H7 is A3:H7), or the table rule refused a table Excel makes.
  A command over several areas asks every area first too (`RefuseRowsCols`, `RefuseInsertCells`,
  `RefuseDeleteCells`, as `InsertOrDelete` does): Delete Sheet Rows over two areas deleted the first and refused the
  next. And what picks a refusal's message (the `block_*_` kinds) is set by a refusal only: `CountBlock` clears every
  kind and the refusal sets its own; `Refuse` cleared some on each call, refusing or not, and the second area's allowed
  check made the UI say "protected sheet" on a sheet without protection (basics seq270330).
- AutoFill reads a lane of two or more cells of one kind (one list's items, one prefix before a number) as one
  series whose step the cells show (`LaneTextSeries`, numbers through `TrendLine`): stepping each cell by one a
  round turned `Jan`, `Feb` into `Feb`, `Mar`, `Mar`. Test a fill from two or more cells, not only from one.
- AutoFill splits a lane of mixed seeds into runs of one kind (`FillKind`: General joins any number format, but
  numbers, currency, percentages, dates and times in their own formats don't join; texts of one series; a formula
  ends a run and is only copied), each going on by its own step: 103 of 300 random fills differed from Excel while
  the lane was taken as one series or a formula stopped it. Test fills that mix kinds, not only one kind a lane.
- AutoFill of dates by a unit (Fill Months, Years, Weekdays) moves seeds that aren't a series in it one unit a round
  (the first by the unit, the others by as many days); blanks repeat and don't split a series. Test fills with two
  or more seeds, with blanks, and with each fill type, against Excel's measured results (`test_basics_fill.cpp`).
- Everything a command changes goes into its one Undo step: a second `engine->Post` beside the `PostUser`
  (the rows a bigger font grew) changes the workbook where no Undo reaches it. Post without User only what
  isn't the workbook's content. A part of the workbook changed directly (`merges.mut()`) is noted
  (`NoteTouched`, `NoteFormat`) even when nothing else in the step changed: the merged cells AutoFill repeats over
  empty cells changed the workbook without an Undo step (doc_fuzz seed 17480302).
- Undo keeps the grow-only tables (styles, formulas) and the store, so what in them is the workbook's look goes
  back by hand in `RestoreFrom` (the default font a theme's fonts set, the theme's effects): Undo of a theme left
  its font. A new such value is restored there, and its command notes the step (`NoteTouched`, through a
  Document method, never a direct store write in a `PostUser`, which leaves no Undo step).
- A change to rows or columns only (a width, a height, hiding) is not a write to their cells: it goes through
  `MarkRowsColsChanged`, never `MarkChanged`, which takes written cells out of the spills over them. Undo of a
  column width cut B3's spill down to typed values and B3 showed #SPILL!. Setting the size a row or column already has
  changes nothing and leaves no Undo step (a double-click to AutoFit made two).
- A unit test takes the changes between steps (`t.d.TakeChanges()`), as the engine publishes them after every
  command: `MarkChanged` skips a second change of the same cells before a publish, and a test that never published
  passed where the app failed.
- What the Document keeps beside the workbook about its cells (`spill_blocked_`: the #SPILL! anchors waiting for
  room) is kept by every undo entry with the version it describes (`UndoEntry::blocked`): Undo / Redo put the cells
  back without a calculation, and an anchor brought back as #SPILL! never spilled when the cell in its way was deleted
  (doc_fuzz seed 247). So are the circular references found (`UndoEntry::circular`): a version taken settled came
  back without its cycle, and a VLOOKUP reading `=COUNT(A:A)` in A5 was calculated to 0 where it had kept #N/A
  (seed 10882828). A new such index is restored in Undo / Redo too, or rebuilt from the version.
- Two arrays never share a cell: an array entered with Ctrl+Shift+Enter over a dynamic array's spill takes the spill's
  cells (`SetArrayFormula` drops the spill and calculates its anchor again, `#SPILL!`), and `UpdateSpill` counts
  another array's range as in the way whether or not its cells hold values yet. The spill record stayed over the
  array, spilled back over it, and an insertion cleared the array's cells with it (doc_fuzz seed 10130034).
- The parts of an array entered with Ctrl+Shift+Enter that a copy, Fill Down / Right or the fill handle takes are
  arrays at their destination, each its formula moved from the array's first cell to the part's (Excel 16, measured:
  `cse_` cases); `ClipData::arrays` carries them. Pasted as their values they became constants beside a dynamic
  formula, which spilled differently after a reopen (doc_fuzz seed 10030116). Every cell of such an array reads the
  array's formula in Python (`Range.Formula`), a spilled cell `""`.
- An object that owns cells refuses what would give it cells it can't hold, both ways round: a multi-cell array
  over a table and a table over a multi-cell array (Excel's message, measured), a merge that empties part of an
  array (it empties the cells after its first as two blocks, the rest of the first row and the rows below: one that
  cuts an array is refused, measured), cells inserted or deleted with a shift that moves part of a table's width
  (`RefuseTablePartShift`, measured), and cells deleted inside a table across part of its width delete its whole rows
  or columns (measured). Each of these put a formula into a table's header or an array's cells (doc_fuzz seeds
  10090004, 10110000, 10250095, 10260083). A new object that owns cells asks the same of every operation.
- What the Document keeps about a sheet's cells beside the sheet itself (`spill_anchor_` and `spill_tiles_`, the #SPILL!
  anchors in `spill_blocked_`) is made for a copied sheet too: `CopySheet` shares the source's cells and formulas but
  indexed none of its arrays, and a #SPILL! on the copy never spilled when the cell in its way went (doc_fuzz seed
  10240003).
- The pending set and its indexes (`pending_tiles_`, `new_pending_`, `pending_spills_`) are emptied together, by
  `ClearPending` only (`check_lessons.py`'s `pending-clear`): Undo emptied the set and left an anchor in
  `pending_spills_`, and every range over that spill waited for a cell nobody calculated, so `SUM` over it stayed 0
  until a full calculation (doc_fuzz seed 10040032). An index of another set is checked against it where it is read
  (`PendingSpillAt` asks `IsPending`), so a stale entry can't stop a calculation.
- A table holds no merged cells whatever brings them (Excel 16, measured): `Merge` refuses a range that meets a table
  (`BlockReason::TableMerge`; the ribbon's merge buttons are grayed there), and a paste, a cut and the fill handle make
  no merged area that lands on one (the cells come unmerged). doc_fuzz checks after every step that no table holds a
  merged area or part of a multi-cell array and that no two tables, merges or arrays overlap: each path that let one
  in was found within minutes of the check (seeds 31000041, 32000257, 32027683).
- A table holds text in its header, and nothing a table can't hold: one question (`TableCantHold`: another table,
  merged cells, any of a multi-cell array) for every way a table gets cells (AddTable, ResizeTable, AutoExpandTable),
  cells it takes over from a spill given up first (`TakeSpillsUnder`: the headers are Column1, Column2, the array
  shows #SPILL!, Excel 16 measured), and its headers made text again at every step's `Commit` whatever path wrote
  them (a fill of blanks, a cut from another sheet), inside the step so Undo takes it back. Each path that kept a rule
  of its own let a header hold a number, a formula or an array's value (doc_fuzz seeds 10280369, 10290940, 10312766,
  10320462).
- A table grows by itself (typing or code writing beside it) only over a line that holds nothing besides what is
  written, and never for an entry that is refused (Excel 16, measured by keys posted to its sheet window): grown over
  `=SUM(B2:B6)` under it, the SUM's range followed to the new row, a circular reference (basics seq470822). Measure a
  rule that decides when Excel does something by itself with the cells around it empty and not.
- Whether an array can spill is one rule (`Document::SpillBlocked`: a value or formula, a merged area, another array,
  a table), and a #SPILL! anchor is woken by what frees its range whatever freed it (`RecheckBlockedSpills` before
  every calculation, as a spill a merged area now crosses is calculated again): the paths that woke anchors by the
  cells they wrote missed a merged area a fill removed and a merge Undo put back (doc_fuzz seed 10301020).
- What cuts a reference down never depends on what the formula itself writes: whole columns in an array formula are
  cut to the last row their own columns hold (`LastRowIn`), not the sheet's used range, which the formula's spill
  widens (=B:B*2 spilled further at each calculation, doc_fuzz seed 26213328), as whole rows are kept whole (seed
  20920008).
- A binary search over whole rows or columns (approximate MATCH, VLOOKUP, LOOKUP, XMATCH's search modes 2 / -2) runs
  over the range's whole length, the cells past the used range empty (`Grid::full_rows`), as Excel's (measured: the
  answer over unsorted data doesn't change when other columns grow). Cut to the used range, a paste elsewhere changed
  it, and the formula, which doesn't read those cells, kept its old answer (calc_fuzz seed 3022).
- An argument that takes only a reference refuses a constant or an operator's result as it is typed (`RefOnlyArg` in
  the parser: ROW("A1"), COUNTIF({1,2},1), SUMIF(A1:A2+0,1), measured), as do LET and LAMBDA without a name where
  one goes; errors (#REF! after a deletion) and functions pass.
- A test's expected value is a claim about Excel: taken from a help page or a guess it can be wrong while the test
  passes. ADDRESS's help page quotes '[Book1]Sheet1' where Excel 16 doesn't, and XMATCH's modes were read as given
  where Excel takes INT (-1.5 is -2). `scripts\measure_test_formulas.py` calculates every cell-free `t.Eval` claim of
  the unit tests in Excel (972 of them): run it after adding such claims.
- A formula is what its text reads back as, at every cell that uses it: a save writes one text per formula and an
  open makes two formulas of one text one. When a sheet is deleted, the formulas naming it become what their text
  (#REF!B2, #REF!) reads back as (`DeleteSheet`), and a reference whose sheet was deleted still has cells
  (`RefSpec::HasCells`, `TargetSheet`): it moves with its formula as one to another sheet does, and moved off the
  sheet it is #REF! alone. Every walk that moves, rebases, sorts or fills references asks `HasCells`, never `!bad`
  (`check_lessons.py`'s `ref-has-cells`; doc_fuzz seeds 10420333, 10460222, 88001420). Its key is one whatever
  its sheet field holds (`KeyRef`: -2 parsed, -1 or the old id after a move), or two cells of one text were two
  formulas until a save and an open (seed 11682258).
- What a formula reads through a name, OFFSET or INDIRECT is its dependencies once it is calculated
  (`SetDynamicDeps`, recorded by the calculation host, in the parallel waves too), as Excel keeps them: a spill
  appearing in cells it read calculates it again, and a cycle through them is met in every order. Read as volatile
  only, such a formula kept an answer from before a spill in the same calculation, and a cycle was found after Redo
  only (doc_fuzz seeds 10431002, 10503625). A spill that appears or moves also follows the formulas that read it
  (`MeetSpillCycle`): a path back to the anchor is a cycle, though no value changed (seed 10553941).
- Undo that puts cells back clears first every spill over them, its anchor inside them or not (`RestoreSelective`):
  a spill from outside cleared after the copy took the value put back with it, and Undo of a fill that had freed a
  #SPILL! anchor lost the cell that had blocked it (doc_fuzz seed 10573473).
- A write noted twice before a publish (`MarkChanged`'s repeat of the last change) still does all a write does:
  taking the cells out of a spill, waking #SPILL! anchors, a table's headers. Returning early after the first
  steps left a formula typed into a spill, after a fill through it, under the array's values (doc_fuzz seed 10592817).
- What a step marks for calculation survives the rebuild after it: `AdjustFormulas` clears every spill and then
  `RebuildAll(false)` / `ClearPending` empty the pending set, so what must be calculated at the new places is
  collected first (`again_old`): the anchors, and what read their spilled values, since an anchor deleted with its
  row never spills again (`=B3*2` kept 4 from a deleted `=SEQUENCE(3)`, doc_fuzz seed 10660583).
- A formula Undo or Redo puts back isn't a formula just entered: in a cycle it keeps the value it had
  (`RebuildDepsInRegion` takes it out of `fresh_formulas_`), except a dynamic array, which comes back without its
  spilled values and is set as its step set it (`restored_formulas_`; doc_fuzz seeds 580 and 23).
- A cell the engine writes on its own (a table's header made text, `SyncTableHeaders`) marks what reads it, as any
  write does (`MarkDependentsOf`): noted only as a change to redraw, `=B1:C5*2` kept 0 from the header's formula
  until a reopen read its text (doc_fuzz seed 11051768).
- Values a dynamic array spilled are not typed values: a region copied back for Undo / Redo (`CopyRegionFrom`) or the
  source of Fill Down / Right leaves them out and the array spills again (Excel, measured). Copied as values they
  blocked the array: Redo after a Ctrl+D through a spill showed #SPILL!. Undo also takes back the rows a spill grew
  the sheet by.
- What acts on the rows a filter shows acts on the cells themselves when the filter hides every one of them (Excel,
  measured): `FilterShownRuns` gives the areas back then, so a copy, Replace, Clear and a write over a hidden row change
  it. An empty copy read as "the selection is too large to copy", and Replace left a hidden row's text (rand2_085).
- Typed input follows Excel's grammar to the end of each part: an exponent needs its digits (`1e` is text, not 1),
  a 12-hour time takes `a` / `p` after a space or the hour alone (`9:00 p`, `3 PM`, `ParseTimePart`) and an hour up
  to 12 (`13:00 pm` is text), and a number takes one negative mark, with spaces between its marks (`$ -5`, `( 5 )`;
  `(-5)` is text). One function reads typed numbers (`ParseNumberInput`), and `LooksLikeFormula` asks it first, as
  Excel reads an amount before a formula: `-NT$5` is -5, though `NT$5` is also a cell.
- A grammar checked only on what people type right misses what they mistype: generate a few hundred inputs (spaces
  where they don't belong, marks combined, values past the limits) and compare typing, `="..."+0` and VALUE with
  Excel's, all in one probe. 127 of 703 typed inputs differed (`TRUE `, `1:30PM`, `9:30:`, `-$1 1/2`, `1,2345`), and
  three tests had written a guess as Excel's rule (`10:30PM` a time, `1,2345` text, `1e308` a number).
- Text a formula reads isn't always read as typed: Excel takes a month and year in numbers (`1/2026`) only typed
  (`InputParse::typed_only`, `ParseDateText`). A new place that reads text as a number or date in a formula checks it.
- Excel corrects one thing in its arithmetic: a formula's last addition or subtraction that cancels to 15 digits
  is 0 (`Evaluator::EvalTop`). Only the formula's top node counts, and parentheses are a node: a test that needs
  floating-point residue writes `=1*(...)` or `=(...)`.
- A formula entered in a General cell takes a format once, as Excel's (`InferResultFormat` in document.cpp): `=A1+1`
  shows as A1 does. A test that wants a formula's result in General (an error check of a misleading format) sets
  the cell back to General after entering it, or reads cells whose references are General.
- A number format code Excel refuses is refused where it comes in (`formula::ValidFormatCode`: Format Cells,
  `Range.NumberFormat`, a style's format), not only shown as General: a new place that takes a code calls it.
- Measure an iterative function (RATE, IRR) with large amounts too: its last steps are in the rounding noise, and
  only Excel's own loop gives its result there (RATE keeps three points and returns slot 0). 1,075 small cases
  matched while every 1E9 mortgage was #NUM!.
- A result written as the difference of two near values loses its digits where they cancel: PPMT as PMT - IPMT was
  wrong from the fourth digit in a long loan's first periods, and `(1 + r)^k - 1` over few periods at a small rate
  from the fourteenth. Compare with an exact value (Python's `fractions`) as well as with Excel, near the
  cancellation too, and use a form without it there (`GrowthM1`, the principal grown by `1 + r` a period).
- One reading of numbers in Chinese (`ChineseNumerals`) serves `[DBNum1]` / `[DBNum2]` and NUMBERSTRING: two copies
  read 10 differently.
- A number format code made only of modifiers (`[DBNum1]`, `[$-404]`, a color) still shows the value, as General: a
  bracket the parser doesn't know left an empty section, and the cell showed nothing.
- Conditional formatting formulas are relative to the first cell of the rule's first range: a rule copied, painted or
  clipped to other cells gets its formulas re-based (`formula::Parse` at the old first cell, `ToA1` at the new one).
- A scan that skips to a closing delimiter (a quote, a bracket) steps past it only when it was found: text being typed
  often has none (`=SUM(A1)'0012`), and one step too many read `text[n]` (a release build hides it; debug stops).
- A font set on cells is set for every character of their text (Excel): `ModifyStyle` and the row / column versions
  drop the runs' own value of what changed (`RunsFollowFont`), and a read of the cell's font sees its characters
  (`range.style_get`: Null where they differ). Bold on, then off, left the one bold character bold, and
  `Range.Font.Bold` of a partly bold cell read False.
- Colors picked from a palette are theme references (`model::MakeThemeColor`): resolve them (`model::ResolveColor`)
  before blending or comparing RGB (color scales, data bars, Python's `Tab.Color`).
- What a formula reads from a report that refreshes (GETPIVOTDATA) is the report as it was last refreshed, not its
  source now: an edit of the source changes nothing until Refresh, for a formula entered after the edit too (Excel 16,
  measured). GETPIVOTDATA read the source at every calculation and disagreed with the report beside it; the Document
  keeps each refresh's cache (`pivot_caches_`).
- A PivotTable's fields are referenced by index but follow their names (`RemapPivotFields`): an edit that adds a
  calculated field or a group passes indexes relative to the old `fields`; never compare them with the new cache
  directly.
- A sheet has one AutoFilter besides every table's buttons, and the first table's buttons are the sheet's
  (`TableOwnsSheetFilter`): code that turns the sheet's AutoFilter on or off asks whether a table owns it.
  `SetAutoFilter` hands a table's criteria over to the table first; Ctrl+Shift+L beside a table turned the table's
  filter off instead of filtering the list under the cursor.
- Excel compares text without case where a person picks or matches values (filter lists, a validation list of cells,
  duplicates, Find, criteria), and a criterion a person types takes `*` `?` `~` (Text that Contains, filters): keep
  such sets lowered (`engine::Lower`) and match with `WildFind`; an exact `std::find` left `aapl` hidden when `AAPL`
  was checked. A validation list typed into the rule (`AAPL,MSFT`) is the exception: it matches with case, its items
  as written, the entry without the spaces around it (Excel 16, measured). This lesson said "list validation" for both
  without a measurement, and typed lists let `aapl` in.
- A setting Excel keeps in the workbook (the calculation mode and iterative calculation: `calc_mode`, `calc_iterate`,
  WKBK key 7) is saved with it and set by opening it (measured 2026-11-16: a file saved manual with iteration opens
  manual with iteration in a new Excel), not only kept in settings.json: a model built on a cycle opened on another machine showed the circular
  reference warning. Check where Excel keeps a setting (the file or the machine) before adding it to `Settings`; a
  save / reopen of everything a person sets (probe: build, SaveAs, `yc.launch(path)`, compare) finds what isn't kept.
- A sheet's `SHET` chunk is a map with a fixed entry count (`w.Map(N ...)` in ycg.cpp) that counts every key written:
  a new key is written every time and N grows by one; a key written only sometimes, or not counted, makes the reader
  stop before it and the value is silently lost (a round-trip test catches it).
- Never assume a fixed TCP port is free (the debugger's 5678): another YCGrid or a debug session may hold it; fall back
  to a port the system picks and tell the client the one used.
- A bridge request that reads `eng.Current()` off the UI thread calls `eng.Sync()` first (`OnUi` does it for
  you): `Validation.Value` read the snapshot from before the rule and value the same script had just written.
- A window started for a file (Workbooks.Open beside a workbook in use) names that file in its instance file only
  once it is read (`SetStartupOpen`; it says `"opening": true` until then, and `yc.launch(path)` waits for that),
  and a failure is noted there (`NoteOpenFailed`): Python matched the window by its file before the read failed and
  worked with its blank Book1, and a launch that returned before the read made the file look unopened.
- A request that reads what cells show (Go To Special's kinds, a sort's order) calls `Settle(d)` first (python_bridge
  .cpp): a formula written by the same connection just before has no value until it is calculated, and was skipped.
- Every text that names cells outside a cell formula is walked through `model/ref_texts.h` (`EditObjectRefTexts`,
  `EditRuleRefTexts`, `EditWorkbookRefTexts`, `ForEachCfRefText`, `ForEachDvRefText`), never kind by kind: five
  operations kept lists of their own and each missed a different kind (a copied sheet's Solver model, an icon set's
  formula thresholds on insert, a color scale's formulas on a rename, a scatter chart's X error bars everywhere, a
  panel control's linked cell). `check_lessons.py` fails on a per-kind walker called anywhere else, and a new field of
  the model fails `check_model_fields.py` until `schema/model_fields.yaml` says what keeps it right.
- So does everything kept as a rectangle or a cell (Excel 16, measured): the ranges a protected sheet lets people
  edit, the cells error checking leaves alone, the cells a linked picture shows, the print area's other areas, an
  advanced filter's list, YCGrid's alert rules and parameter cells follow inserted and deleted rows and columns
  (`ShiftSheet`), and the AutoFilter, sparklines' data, PivotTable sources, the print area, allowed ranges and linked
  pictures follow a cut of all their cells (`MoveCells`), where an array entered with Ctrl+Shift+Enter stays one
  array (cut cell by cell, its results became constants and the anchor showed #SPILL!). Each is a slot of
  `test_ref_registry.cpp`.
- References kept as text (defined names, chart series, conditional formats, validation) name sheets by name: every
  deletion of a sheet breaks them (`BreakReferenceTexts`: #REF!, or a sheet added later under that name takes them
  over), every copy of a sheet copies its sheet-level names onto the copy, and every
  change of a sheet's name or of the cells they point at rewrites them (`RenameInReferenceTexts`, `adjust_text` in
  InsertCells, and for rules `adjust_rule_text`, parsed at the rule's first cell before and after the move: a
  validation's `=$A$10` stayed put while its rows moved); cell formulas keep the sheet's id and follow by
  themselves, except that a copied sheet's formulas naming the source are pointed at the copy (`CopySheet`).
  So is everything else the copy holds that names cells or a table: its charts, controls and sparkline dates name the
  copy, its rules and validation no sheet, and its formulas the copy's tables (`=SUM(Table1[v])` on the copy summed
  the source's table). A new kind of text on a sheet is pointed at the copy in `CopySheet` too.
- What a command does to a table's columns is done where every path meets (`ShiftSheet`), not in the command:
  Delete Table Columns made `=SUM(Table1[v])` #REF!, while deleting the sheet column or the cells left the text and only
  its value said #REF! (a column named v later took it over). Test a table change by every way a person makes it.
  The exception is what only a person's action does: Clear over a whole table deletes it (`ClearTablesIn`, called by
  Delete, Home > Clear and `Range.Clear`), never `ClearContents` itself, which a cut and a move call on their source.
- A fill is a copy for data validation and not for notes (Excel 16, measured 2026-11-19: FillDown, FillRight and
  AutoFill bring a list rule and leave the note; a source without a rule takes the destination's away). Every fill path
  (a paste a line, `FillLinesInBulk`, AutoFill's cell at a time) leaves it to `FillValidations`, which grows the
  source's rule instead of adding one a line: YCGrid's fills had dropped validation since they were written, and a
  rule added a line is the O(n^2) Fill Right of a conditional format had (doc_fuzz seed 91473).
- The fill handle and a copy differ for table references: AutoFill across moves the column names
  (`ShiftTableColumns`), a paste and Fill Right keep them. Test the fill handle (`AutoFill`), not only a paste.
- Typed input respects the cell it goes into: a Text (`@`) cell keeps the text as typed (`IsTextFormatted` in
  `SetInput`). Every way text comes in as typing (the cell editor, Python's `Value` / `Formula`, one formula over
  many cells) goes through `SetInput`, or one of them turns codes like 00123 into numbers.
- A whole table inside cut cells goes with them (`MoveCells`, `CutHoldsTable`): left behind, its emptied header
  renamed the columns Column1, Column2 and every formula followed that rename. One cut onto a table that stays goes
  into it as cells, and what named it is #REF! (Excel 16, measured): kept as a table, the two overlapped and fought
  over their headers' names (doc_fuzz seed 20411091). An object that sits on cells (a table, a report) is asked
  about in every operation that moves cells.
- Moving cells (cut and paste, drag and drop: `MoveCells`) changes the cells references point at: cell formulas,
  names, chart series and every rule's formulas follow (texts read for many cells only by their fixed references),
  the rules move with the cells, and a reference to the cells written over is #REF!. A move that rewrote only the
  cell formulas left names and charts on emptied cells and dropped the validation.
- Every text the formula writer produces reads back through the parser: .ycg keeps formulas as text. A reference
  to a deleted sheet is written `#REF!A1` (`RefSpec::lost_sheet`), and the parser takes `#REF!A1` back. It reads
  back as the same formula too, not only the same text: what the writer adds for the parser's sake (the parentheses
  in `(A):Data!E13`) the parser drops again, or the text read back is another formula with another key, and two
  cells of one text are two formulas until a save and an open (doc_fuzz seed 35490977; `--verbose` lists each
  formula's id and key).
- A defined name's relative references are kept relative to A1 and wrap around the sheet (Excel's files do the
  same): text a person or Python types is relative to the active cell and goes through `formula::MoveRelativeText`
  before it is kept, and back before it is shown; evaluating a name parses it at A1 and calls `RelativeTo` for the
  cell that uses it. Parsing it at the using cell made every relative name an absolute one. Inserted or deleted rows
  and columns move a name's absolute parts only (`$B2:B$5` is `$C2:B$5` after a column at B, measured; a shift of
  cells leaves it), and a name keeps its ends in the order they are (`Parse` / `ToA1` with `keep_order`): a cell's
  formula is written from its upper-left corner, a name isn't.
- A defined name that is a union (`=Data!$A$1,Data!$A$3`, which Excel takes) is kept as `((...))`, since the
  formula language reads a union only inside parentheses: every place that shows a Refers To goes through
  `engine::ShownNameUnion` (the Name Manager and Paste List showed the kept pairs), and a formula that uses the name
  reads it as its areas (`SUM(U)`, `AREAS(U)`), never as one value (#VALUE!).
- A reader of a file format never allocates what the file says (a ZIP entry's uncompressed size, a chunk's length)
  without a cap, does its offset arithmetic in 64 bits, and every importer (`ImportXlsx`, `ImportCsv`, `LoadYcg`) is
  called inside `try` in `OpenWorkbook`: a crafted file is an error message, not the end of YCGrid. Sizes multiplied
  from a header (a `.npy` file's shape times its item size) are checked for overflow and against the file's length
  before anything is allocated or read: a wrapped product allocated nothing and the reads ran past it.
- An importer keeps what the file says about who holds cells, as the .ycg loader does: an .xlsx's dynamic array
  keeps the range its `ref` names and the values of its other cells (`spills`), or the full calculation on open
  decides by reading order, and of two arrays that met the one that had shown #SPILL! took the cells (doc_fuzz
  --xlsx seed 110480). `doc_fuzz --xlsx` saves and opens an Excel copy at every save check: run it after a change
  to the .xlsx reader or writer.
- A file's cells come back as the file saved them: the .xlsx reader writes with `kSrcFile`, so a formula takes no format from its references and a value next to a table doesn't grow it: a General `=SUM(A1:A5)` reopened in A1's red / blue format. What a write does by its source is decided in one table (`engine::PolicyFor`: input, table rules, format inference, outside Undo); a new source picks its row there, and `check_lessons.py` fails on a `src & kSrc...` test anywhere else.
- A dynamic array that spills over cells it reads is a circular reference, the first time as after Undo or a reopen: its spill marks the anchor pending again and `StoreResult` keeps it pending; an anchor whose reads aren't in the dependency index (a name, OFFSET, INDIRECT: `kDynamicRef`) is calculated once more when its spill appears or moves. The first calculation spilled as if nothing read the cells, and Redo found the cycle (doc_fuzz seed 23).
- A multi-cell array formula over a table is refused, as in Excel ("Multi-cell array formulas aren't allowed in tables."): its header held a result as text and turned into a number on reopen (doc_fuzz seed 112).
- The same workbook is the same bytes: what the model keeps in edit order (merged areas) is written in reading order, and `io::ContentDump` hashes a style by its strings, not the string table's numbers, so a dump compares across a save and an open (doc_fuzz seeds 1, 102).
- A sort moves each line's formulas as a copy would, with two exceptions (Excel 16, measured): references to other sheets keep naming the same cells, and a range moved partly past the sheet's edge is cut to it (`=SUM(B0:B2)` is `=SUM(B1:B2)`; one cell past it is #REF!). Shifting every reference made 40 of 46 random sequences with a sort differ from Excel; single-operation sort cases never showed it, since their formulas read their own row.
- How an operation rewrites the references it touches is measured over a grid of cases and a random set, not over
  the cases a round showed: the cut and paste rule for a range's edges (`MoveEdges`) was written from a few and was
  wrong in 236 of 5,029 cuts over B2:D5. `scripts\measure_excel_moves.py` measures 13,617 cuts and
  `test_measured_moves.cpp` keeps every one; a candidate rule is checked against the whole table before it is written
  down, and against a set measured after it (2,500 of 2,500 agreed).
- An array entered with Ctrl+Shift+Enter is one formula with many values: it is a circular reference only when a value
  reads itself or waits for one that does (Excel 16, measured), so its values are settled one by one, its own cells
  read as #BLOCKED! until each is set and the result calculated once more to check it
  (`SettleArrayReadingItself`), and one entered after a cycle shows 0 only in the values that read it. A rule about
  cycles written for single cells gets its array case measured. Two such arrays that read each other's cells
  (B3 {=A1:B3*2} beside A2:A4 {=A1:B3*2}) are settled together (`SettleArraysTogether`): one at a time, each waited for
  the other, the pair was taken for a cycle in one order of calculation and not in another, and C4 reading A4 kept a
  value from before (doc_fuzz --xlsx seed 24002581569; a full calculation gave the right one).
- `formula::NeedCell` (a read of a cell still waiting) isn't a `std::exception`: code that evaluates a formula outside
  the calculation's own loop catches it by name, or the read of a cell a cycle blocks ends the process (doc_fuzz
  --gaps seed 26, the array settling added that day).
- A command that finds nothing to change is still a step when Excel makes one: a sort of lines already in order leaves an Undo step in Excel 16 (measured 2026-11-16, `Undo` enabled after sorting 1, 2, 3), and YCGrid's made none, so the next Ctrl+Z took back the edit before it (`NoteRegion` over the sorted range). Measure whether Excel's no-op leaves a step before skipping it.
- A copy-on-write table is read and changed in separate statements: `wb_.formulas->Share(InternFormula(x))` reads the `->` first, InternFormula copies the table a snapshot shares and appends there, and the lookup in the old table finds nothing (a sorted formula was pasted as its value, only in a running YCGrid, whose frames hold snapshots). A unit test of such code holds a copy of the workbook first (`const model::Workbook snapshot = t.d.wb();`); `check_lessons.py` (`cow-order`) fails on the pattern.
- Arithmetic reads its operands left to right, each as a number: `="Qty"*#REF!` is #VALUE! (Excel 16, measured), the left text failing before the right side's error; `&` and comparisons give the first error.
- An importer decides once what may become a formula (`ImportCsv`: `=` and whatever `LooksLikeFormula` takes stay
  text), never by a first character alone: `=` was kept as text while `+2+3` and a phone number `+886-...` went through
  typing and became formulas. It reads each field once (`ParseInput`) and writes a block at a time (`SetValues`, the
  formats as runs down a column): a field at a time through `SetInput`, after a `ParseInput` of its own, took four
  seconds for a million rows, all of them on the UI thread. A test compares its cells with typing's.
- A loop over rows or columns clamps its end to the sheet first: `for (r = r0; r <= r1; ++r)` with the end marker
  0xFFFFFFFF (a whole column's last row) never ends, and 2^31 takes seconds; `Rows.Hidden = False` from Python
  froze YCGrid. Document methods that take a range (`HideRows`, `SetRowHeight`...) clamp it themselves.
- An operation repeated per row or per column (a paste per row for Fill Down) is fine for a selection and never for
  whole columns or the whole sheet: Ctrl+D down a whole column took minutes, the whole sheet ran past the sheet's end
  and out of memory. Large ones work a lane at a time (`FillLinesInBulk`: one shared formula, blanks cleared at
  once, a style written only where a cell doesn't already show it through its row or column), and their test runs
  on the whole sheet with a time limit, with column styles and a formula in the last cell (`test_basics_dims.cpp`).
  So does a format that differs by a cell's place in the selection (borders): `SetBorders` looped over every cell
  for each preset, and Outside Borders over the whole sheet never ended (basics seq268600); each preset is now a few
  rectangles (four one-cell strips for the outside) set through `ModifyStyle`, which writes whole columns' and rows'
  own styles. So does what an operation reads or clears among a range's cells: Merge & Center over the whole sheet
  cleared its 17 billion cells one by one, and Merge Across made a part for each of two billion rows (the end
  marker) and walked each part's tiles (basics seq280931, 281218). Clamp the range to the sheet, walk the cells that
  hold something once (`ForEachCellInTiles` over the values and the formulas), and group them by part. A style that
  fills whole rows from column A (whole columns from row 1) is the line's own style (`row_styles`, `col_styles`),
  not one per cell: Ctrl+R over whole rows whose column A had borders of their own wrote 17 billion cells (basics
  seq283522), as `FillLinesInBulk` now does for those lines.
- What a line copies from its neighbor is copied line-wise when the lines are many: inserted rows take the formats
  of the row above, and under a row whose 16,384 cells each had a border (Outside Borders over the whole sheet)
  the million rows Ctrl+Shift+= inserted took them cell by cell, 17 billion styles and out of memory (basics
  seq316723). `CopyNeighborFormats` gives them as a line style what most of the neighbor shows and a cell style
  only where a cell shows another, the choice that leaves the fewest (Normal is a line style too: rows keeping Normal
  over columns with Wrap Text gave a million inserted rows Normal cell by cell, seq327428); a new copy from a neighbor
  line counts its cost as cells times lines.
- What an undo entry keeps beside its version is shared while it doesn't change (`SharedBlocked`, `SharedCircular`):
  the version itself is a copy-on-write workbook, but each entry copied the #SPILL! anchors whole, and a sheet of a
  million of them (=SEQUENCE(3) filled down) took 64 MB a step and 6 GB over the undo list (doc_fuzz seed 26000048).
  Anything new an entry keeps is shared the same way or bounded.
- What a write does to the whole of an object (a table's headers made text: `SyncTableHeaders`) isn't done per write
  inside a loop of writes: a fill or a repeated paste a line (a tile) at a time holds it (`syncing_tables_`) and does
  it once after the loop. Ctrl+R over a table as wide as the sheet made all 16,384 headers text at each of its 16,384
  pastes and froze YCGrid (basics seq308046, in the state of a value in the sheet's last cell).
- A paste checks the copy area as selected, not the cells it holds: a copy of whole rows or columns keeps only the used
  part (`ClipData::src`), and pasting B5:XFD6 at J2 pasted that part and selected J2:XFL3, past XFD, where Excel refuses
  ("We can't paste here...", measured). `ClipData::area` keeps the selection, and `PasteRunsOffSheet` asks it.
- The used range grows by the cells an operation wrote, never by the rectangle it was given: the bulk fill touched
  its whole target's corners, so Ctrl+A, Ctrl+D over four cells in row 1 made the whole sheet used and Ctrl+C
  refused 17 billion cells (basics seed 2390; Excel's used range is A1:D1048576).
- A cell shows its own style, else its row's, else its column's: a cell set back to the default under a row's or
  column's formats gets its own Normal (`model::kStyleOwnNormal`, through `Sheet::ShowStyle`), never style 0, which
  means "none of its own" and shows the line's again (Clear Formats on C5 under a bold column C, Bold off on C6 and a
  plain cell pasted kept the bold; Bold off changed nothing; Excel 16 shows Normal, measured). Code that writes what
  a cell shows calls `ShowStyle`; `SetStyle(r, c, 0)` is for putting a state back. What whole lines hold stays on
  lines: Clear Formats over whole columns or rows removes their own styles (Cells.Clear left the whole sheet's
  borders), a format of all columns but a few rows goes through the columns' styles (`ModifyAllButRows`), and inside
  borders on whole columns are every cell's top and bottom (measured). A style for each of a million rows made the
  next format of whole columns give every crossing cell a style of its own: 17 billion, a freeze.
- A row or column operation over the whole sheet never touches a sorted per-index array one entry at a time:
  Ctrl+Shift+9 with whole columns selected unhid 1,048,576 rows one by one, each erase shifting `DimIndex`'s arrays,
  and YCGrid froze for minutes. Entries set back to the default stay as stale ones and are compacted in bulk; a
  new per-row structure gets a test on all 1,048,576 rows with a time limit (`test_basics_dims.cpp`).
- Whether a function reads TRUE / FALSE as 1 / 0 is measured per function: the date functions that came from the
  Analysis ToolPak (EDATE, WORKDAY, NETWORKDAYS, WEEKNUM, YEARFRAC) refuse them while WEEKDAY, DATEDIF and DAYS
  take them (`ArgDate`'s `no_bool`), and which argument is checked first too (YEARFRAC's basis, NETWORKDAYS.INTL's
  weekend): one shared reader for all of them gave 1,400 date formulas 67 differences from Excel.
- What a function does with an array of references (`OFFSET(A1,{0,1},0)`) is measured per function and set by
  `FuncInfo::ref_mode` (functions.cpp); a function that hands back part of its argument (INDEX with a 0, IF, CHOOSE,
  LET) passes `EV::refs` along with the values, or the next function sees #VALUE! where Excel sees references
  (`SUMPRODUCT(INDEX(OFFSET(...),0))` was #VALUE!, Excel's 0).
- A format code measured through TEXT in a localized Excel reads General and the colors in the UI's language: check a
  code with General or `[Red]` as a cell's `NumberFormat` (en-US) before calling TEXT's #VALUE! Excel's rule.
- An upper bound computed from a size (`row_limit - 1 - box_h`, `limit - s.Rows()`) can be below the lower one:
  `std::clamp` with lo > hi is undefined (a debug build stops on it, chaos seed 5099 in a drag). Floor it
  (`std::max<int64_t>(0, ...)`) before clamping.
- A number an argument gives is clamped before it becomes an index or a count: a `double` cast to an integer past
  its range is undefined, and `CHOOSEROWS({1;2;3},1E+300)` read rows that weren't there. Test every index and
  count argument with 1E+300, -1E+300 and 2^63, not only with numbers a little too large.
- What hands back part of a reference keeps it a reference (TAKE, DROP, `@`, what the LAMBDA helpers pass on,
  REDUCE's start, IF's branch): the next function may tell a reference from a value (ROW, CELL, ISREF, COUNT of an
  empty cell, COUNTBLANK), and a value there was 1 where Excel says 0. An array's empty element stays empty in the
  array (COUNTA counts it, `&""` gives nothing); only the spill shows 0.
- Numbers are compared at 15 significant digits (`Compare15`: `=`, `<`, `>`, criteria), and the functions that came
  from the Analysis ToolPak refuse logicals through `FuncInfo::no_logical`, each measured: a new comparison or such
  a function uses these, not `==` or a reader of its own.
- What an action drops can be the last owner of millions of cells: typing after undoing a paste of six million cells
  waited 0.8 s, inside the Call the UI waited on, while the redo list's tiles were freed. Dropped undo entries go to
  `Document::release_later` (a thread of the engine's), as the snapshots a frame stops using go to `ReleaseLater`
  (main.cpp); a new place that drops large state hands it over the same way, and a paste shares the copied cells
  (`std::shared_ptr<const ClipData>`) instead of copying them for the Undo button's later use.
- A cache kept per snapshot is rebuilt at every frame when a snapshot comes every frame (live data, a recalculation
  in slices): a color scale over a million cells sorted a million numbers a frame. What costs as much as the cells (a
  rule's statistics, the status bar's sum) is kept across snapshots while its inputs are the same object (the rules'
  Cow) and refreshed on a clock (`CondFormatEval::KeepStatsFrom`, main.cpp's `StatsCache`). A value a frame asks for
  is kept up to date as its data changes, never found by a scan: the outline's depth (`DimIndex::MaxLevel`) walked
  every grouped row each frame, a million after Subtotal.
- What a frame draws over the cells is cut to the cells shown before it is drawn: the page breaks were dashed
  from the print area's first row to its last, millions of hidden dashes a frame on a small paper size, and the
  layout behind them (`LayoutPages`, 600 ms on a debug build) was made again at every snapshot. It is kept while
  its inputs are the same objects and refreshed on a clock that waits for a multiple of its own cost (chaos seed
  5117: every step took 10 s). What is made per page is bounded too: a value at XFD1048576 lays the used range out
  over tens of millions of pages (page rows times page columns), and the Page Layout view made them all at every
  edit (18 s, gigabytes; basics seq277302); `LayoutPages` makes the first `kMaxLayoutPages`. Test a layout, a page
  count or a page walk with a value in the sheet's last cell.
- A list of cells typed into a dialog (a scenario's changing cells, its result cells) stops one past its limit while
  it is built, never after: the Scenario Manager collected every cell of A:XFD (17 billion) to say "at most 32".
  Ask Excel what the limit is and enforce it in the Document, where Python's path meets the dialog's.
- A list built from a selection's rows or columns never checks each new entry against the list so far
  (`std::find` before `push_back`): over whole columns with a value in the last row that is a million squared steps,
  and a chart took 25 s to insert (`BuildSeriesFromAreas`). Collect, then sort and `unique` once. What a selection
  can make has a bound too (a chart over the whole sheet: 255 series of a million points; 8 million points at most).
- A comparison a sort calls millions of times works on keys made once a line (`Document::Sort` lowers the texts
  first), never converting both sides at each call (two lowered copies a comparison: a million rows took 4 s); the
  keys keep `Evaluator::Compare`'s order, and a test compares the two on random lists.

### Charts

- Rebuilding derived data (a chart's series from its range) keeps what the user formatted (`KeepSeriesFormat`).
- A chart's texts that refer to cells (ranges, and a series' name, a title or a data label linked as `=Sheet1!$A$1`)
  are walked by `model::ForEachChartRefText` wherever references move, rename, break or are copied: a series' name
  stayed on A1 when a row was inserted above while its values moved. A new such text is added there; a control's
  linked cell and list range go through `model::ForEachControlRefText` (a check box kept writing to $A$5 after a row
  was inserted above it, and its list read the old rows), a sparkline group's date axis through
  `model::ForEachSparklineRefText`, a Solver model through `model::ForEachSolverRefText` (its variable cells are areas
  joined by commas, which the formula parser reads only in parentheses: rewritten one area at a time). Look for every
  `std::string` that holds a range when adding one, and every `CellPos` or `Rect` kept for later (a scenario's cells,
  a data table and its input cells): rows inserted above a data table left it computing the old cells.
- `engine::ChartPalette` gives the palette of the chart being drawn: hold an `engine::PaletteScope(ch.color_scheme)`
  around every drawing of a chart (screen, print, thumbnails), and keep a chart's axis bounds in its
  `ChartAxisFormat` (`min_auto` / `max_auto` each), not in one Automatic flag for both ends.
- Chart code runs every frame (a date axis' plan, the hit boxes): keep it linear in the points, no sort per frame;
  and new `ChartType` / `ChartElem` values are appended (stored numbers keep their meaning), with Python's
  `_XL_TO_TYPE` / `_ELEM_*` numbers following.

### C++

- Code that converts pixels between layouts (RGBA to the BGRA a Windows encoder wants) copies every channel, alpha
  too: an alpha written as 255 made every transparent picture opaque (Insert Icons showed dark squares).
- C++ evaluates function arguments in no set order: `f(ImGui::MenuItem(.., &v), [v] {..})` can capture `v` before the
  click toggles it. Call the widget first (`const bool k = ImGui::MenuItem(...)`), then build the lambda from `v`.
- A range-for over `cond ? *list : std::vector<T>{}` loops over a copy of the list: a pointer kept to an element
  dangles after the loop (a slicer "found" that way made Report Connections fail). Use a static empty list's reference.
- `near` and `far` are macros in `windows.h`: never name a variable so.
- An index into an object's cells is checked against that object's own size, never against a rectangle that holds
  it: a PivotTable's `output` takes in its filter area above the report, the mouse there made `ar - r0` wrap round,
  and `PivotReport::at` read past its cells (a debug build stopped on it, chaos seed 5806). `InfoAt` checks
  `rep.rows` / `rep.cols`; every `at(r, c)` does the same.
- JSON that comes from outside (a web answer, a file) is read with `find` / `contains` / `value`, never with a const
  `operator[]` on a key that may be missing: nlohmann asserts there in a debug build and reads past the map in a
  release one. STOCKHISTORY read `d["timestamp"]`, which Yahoo leaves out of a range without trading days
  (`Member` / `Element` in fn_online.cpp); a non-const `j["key"]` on an array or a string throws instead.
- A decision made per list is counted per list: the tile pool's idle trim summed the idle bytes of every block size
  into one number and spent it list by list, so a big block idle all window paid for the small block another list
  had just freed (the reuse test failed at random, when the 10 s window rolled over at that moment). A test of such
  a trim uses two sizes, with the idle block in the list that comes later.

### Python and the skill

- A click action (`yc.interact.on_click`) runs on the host's thread beside a screen's live loop: what both change (a
  figure's items, a shared position) is changed under one lock. market_watch drew its chart from the click and from
  the loop at once, each saw the symbol change and added its lines, and a dozen fast clicks left seven copies of every
  line. A click action also runs on every click: an order button that a double-click must not send twice uses
  `RiskLimits.duplicate_window`, and a quantity typed wrong sends nothing rather than a guess. It runs on a press and
  a release on the same cell, as a button: run on the press, a drag selecting a ladder's sizes sent an order. What a
  screen shows per price (the ladder's My Bid) adds up every order there (`Ladder.working`). A line that stands for an
  order stays in view (`drag_line(..., fit=True)`) and says when the order is gone (filled, refused). A timer chain
  (`Application.OnTime`) that a button pauses and resumes keeps one tick waiting at most, and a live table finds its
  rows by key, never by position: a person sorts it (demo tripled its updates and measured TSMC against MediaTek's
  open). A screen with several figures docks them (`Dock`, `Anchor=`): floating, each opened over the one before and
  the first, the screen's own chart, lay under the rest, and a single one floated over the cells the screen shows
  (backtest_report's trade list): ui_sweep's examples fail figures that cover each other or most of the sheet.
  The example screens are
  used as a trader uses them in `tests.test_live_screens` (tabs, rows, buttons, mistakes, while the data streams),
  checked by what they show (`plot.figures()` lists each plot's items): a new example screen gets its class there.
  The converged random tools never clicked an example's own controls on purpose and found none of these.
- What a button starts in the background (Install on the package bar) shows that it is working and ends in a state
  the person sees: the bar went away on Install, a failed install said only "Installation failed (code 1)" in the
  Error Log, and the example stayed blank. The bar says "Installing...", and a failure brings it back with the
  installer's reason (`env.install_failed`). Test each button of such a flow to its end, a failure too
  (`tests.test_live_packages`; every example in a runtime without numpy and pandas: `scripts\example_packages.py`,
  which found an example that needs numpy only at run time and got an error without the bar), and open the examples with the YCGrid they ship with: an older YCGrid registered for
  .ycg files offered their shared `examples/lib` modules as packages to install (`FolderExeFor` hands them over).
- A property takes back what it reads: `None` (Excel's Null: not set, not a theme color, no outline), Mixed (-2) and
  text written back change nothing. `tests.test_object_sweep` writes every read-write property back on a hidden
  YCGrid; it found `FormulaArray` rewriting a whole table as one array formula, a rule's `Underline = None`
  underlining and a chart title's text turned into a formula. A new property keeps that test passing.
- A border is the line between two cells, kept on either: a read of an edge takes the facing edge of the cell beside it
  when the cell has none (`shown_borders` in `range.style_get`, as Merge's `shown` does), and clearing it clears both
  sides. B3's top read None under B2's bottom line, where Excel reads the line.
- A property of a multi-cell range reads one value, as Excel's: the value the cells share, `None` when they differ.
  The bridge's per-cell answer (`range.get` of `numfmt`, `text`) is collapsed in Python; `NumberFormat` and `Text`
  returned the list itself, and `NumberFormat = None` wrote the format "None".
- A Python method acts on every area of a range (Delete, Insert, Copy, one formula for several areas: relative to the
  first area's corner), and an object that forwards members (`Range.Rows`, `.Columns`, `.Cells`) forwards to the range
  itself, assignments too (`__setattr__`): `Range("B2:C3").Rows.Delete()` deleted whole rows 2:3, `Union(...).Delete()`
  only its first area, and `rng.Rows.Hidden = True` was stored as a Python attribute and did nothing. The same goes
  for `Worksheet.Rows` / `Columns` without an index (`Rows.Hidden = False` did nothing), and a range keeps what made
  it whole rows or columns (`_by`, `_whole`): the whole sheet's `Cells.EntireRow` is the same rectangle as its
  `EntireColumn`, and `Hidden = False` on it showed the columns, not the rows.
- What the UI does after a format (rows that size themselves fit a bigger font or Wrap Text) is done after the same
  format from Python: the bridge calls the UI's helper (`FitRowsForStyle` through `OnUi`). `Range.Font.Size = 20` left
  the row at 15 points while the ribbon grew it, as Excel's object model does. Columns too: a number format that
  shows a number wider than a column nobody sized widens it (`ColumnsToWidenForFormat`, Excel 16 measured through
  ExecuteMso and `Range.NumberFormat`); YCGrid showed ######## after two Increase Decimals. A column a format widened
  is noted (`AppState::auto_widened`) and widens again, one sized by hand since keeps its width.
- A whole column's end marker (`ROW_END`, `kRowEnd`) never opts a request out of a rule: clamp it to the sheet and
  apply the rule. `req.r1 != kRowEnd` kept the filter out of `Range("B:B").Value`, `r.r1 < s->row_limit` out of
  EntireRow.Delete of every row, and `a[2] == ROW_END` read the whole sheet's rows as its columns (all measured in Excel).
- A method whose Excel default is the selection (`Worksheet.Paste` without a Destination) takes the selection's area of
  the active cell, not the active cell alone: one copied cell pasted into B1:B3 filled B1 only.
- A Python method that acts on a range sends the whole range, never only its first cell: `PasteSpecial` sent row
  and column, the bridge selected one cell, and a copy never filled the destination as Excel's does (`ui.paste`
  takes `rect`).
- What a call needs from the cell that made it is sent with it: a worksheet function in Python reads
  `Application.Caller` / `ThisCell`, and the engine's `RequestUdf` never sent the cell (Caller was always None). A
  request keyed by its arguments for sharing says what differs by caller (`per_cell_udfs`: the functions whose code
  reads it are called per cell).
- A Python setter whose None means "put it back" sends a flag of its own: None arrives as nil, and `MV::Has` is
  false for a nil key, so `Window.Caption = None` changed nothing until it sent `win_caption_reset`.
- A method of a sheet object (bound to its own YCGrid window) asks through that object, never the global
  `ActiveWorkbook` or `data.write_frame`'s channel: `Worksheet.SaveAs` of a sheet `Workbooks.OpenText` had opened in
  another window saved the first window's empty sheet.
- Python's warnings are messages, not errors: the host sends them as `log` of kind `warning` (`warnings.showwarning`),
  which the Error Log keeps with its messages. Left on stderr, numpy's RuntimeWarning from a PY cell fed zeros was
  logged as an error (chaos seed 5111).
- The skill documents only what runs: check each member against `python/yc` and an event against a hidden YCGrid
  before writing it down, and mark planned ones (P2). An object that accepts any name (`__getattr__` returning a
  caller, `__setattr__` storing anything) hides a planned or misspelled member: refuse names it doesn't know, as
  `Application` now does (worksheet functions come from YCGrid's list), and keep `yc_python_status` to the events
  YCGrid fires (`host.FIRED_EVENTS`).
- A bridge request that evaluates a formula for Python passes `EvaluateFormula`'s `array` out and returns rows:
  the bare call keeps the top-left value only, and `Evaluate("{1,2,3}*2")` / `WorksheetFunction.Transpose` gave 2.
- An error value's number in Python is Excel's CVErr number (`XlError.Number`: #SPILL! is 2045, #CALC! 2050), from
  the one table in `yc/types.py`, with its `xlErr...` constant: a new error value gets its line there and in
  `tests.test_core`, or it reads as 2000 plus an internal number no script expects.
- A collection Excel returns from a method (`PivotCaches`, `PivotTables`, `Scenarios`, `Trendlines`, `DataLabels`,
  `ChartObjects`) is a property returning a callable collection (`__call__()` is itself, `__call__(i)` its Item):
  recorded macros write `PivotCaches.Create(...)` without parentheses, and a bound method has no `Create`.
- A method that takes a reference as text takes R1C1 too, as recorded macros write it (`PivotCaches.Create(SourceData:=
  "Sheet1!R1C1:R5C2")`, `CreatePivotTable("Sheet2!R3C1")`), and a property Excel reads in R1C1 (`PivotTable.SourceData`)
  reads so: measure what Excel hands back before choosing the form.
- A change event's Target goes through `core::TidyAreas` (no area repeated or inside another, neighbours joined):
  the engine notes a change per write, and Ctrl+Enter reported its active cell and the selection as two areas.
- A call that starts a YCGrid window and then works with it names it by the process it started (`wb.new_window`'s
  `pid`), never as "an instance that wasn't there before": Workbooks.Add took a YCGrid another script started at that
  moment, and Workbooks.OpenText wrote its file into it (two gaps_fuzz runs side by side found it; `Open` matches its
  file).
- A member that writes and reads a place uses one origin both ways: `Chart.Shapes.AddLine` took points from the
  chart's corner and `Shape.Left` read them from the sheet's, so the line "moved" when the chart did (Excel 16 reads
  the chart's, measured). Frozen panes and split panes are numbered differently by Excel (measured both): a rule
  measured on one layout is measured on the others before it is written down.
- Code that asks every other YCGrid window something (`Workbooks.Open`, `Workbooks(name)`) expects any of them to close
  between the listing and the request (`_workbook_names`): a window its user closed at that moment made Open raise
  `YcConnectionError` (parallel tests hit it whenever another test's YCGrid exited).
- A bridge request that answers a question the evaluator also answers (`Range("A1#")` and `=A1#`) applies the
  evaluator's rule (`SpillRange`: a formula whose result is one value is its own spill range), never a narrower one of
  its own: `Range("E1#")` of `=DROP({1;2;3},2)` raised while `=E1#` worked (Excel 16 gives $E$1, measured).
- Which format a save path means is decided once (`IsYcgSavePath`): every way a file is written (Save As, SaveCopyAs,
  the bridge's `wb.save`) asks it, or one of them writes `.ycg` bytes under an `.xlsx` name, as SaveCopyAs did.
- `excel.ActiveSheet` is a proxy that follows later activations (`ws = Application.ActiveSheet` keeps one sheet):
  a test that adds sheets after taking `ws = excel.ActiveSheet` writes and reads another sheet than it meant.
- A bridge request whose work grows with the workbook (writing a file of it) runs on the bridge's thread from a
  snapshot, with only what needs the UI (the sheet shown) asked through `OnUi`: SaveCopyAs to `.csv` wrote a million
  rows inside `OnUi` and the window stood still for 1.9 s.
- A bridge request that fails raises (a `throw` or `E_APP` with the reason), never answers False: Python's `Save`,
  `SaveAs`, `SaveCopyAs` and `Close(SaveChanges=True)` ignored the answer, so a save that wrote nothing looked done.
  An error the UI's code would show as a message box during such a request goes into `AppState::quiet_error` (no box,
  as Excel's object model raises without one), and only a deliberate Cancel (`Workbook_BeforeSave`) answers False.

### Build and toolchain

- Build only through `scripts\build.cmd` (it sets `chcp 65001`): ninja run from a console in another code page reads
  no header dependencies from cl's localized `/showIncludes` lines, those objects never rebuild when a header changes,
  and the app crashes in unrelated code (garbage vectors). Delete objects that `ninja -t deps` lists without deps.
- `LNK1104` on the unit test exe with nobody holding it: the incremental link of that large binary with its manifest
  resource fails (MSVC 14.51), so `yc_unit_tests` links with `/INCREMENTAL:NO`; link to another name to tell such a
  toolchain failure from a locked file.
- A staged folder keeps what an older build put there: `dist_*\licenses` still held the license of ImGui's embedded
  fonts, long since left out, and the wheel shipped it. Staging empties a folder it fills from a list
  (`YcStageDist.cmake`), and the wheel refuses a license `THIRD-PARTY-NOTICES.md` doesn't list.
- In a `.cmd`, `for /f ... in ('"C:\path\tool.exe" "arg"')` fails ("The filename, directory name, or volume label
  syntax is incorrect"): cmd strips the outer quotes. Leave a path without spaces unquoted
  (`%SystemRoot%\System32\certutil.exe`), and write a file without a trailing CRLF with `<nul set /p "=text"`.

### Editing files (agents)

- Source files on disk are CRLF even when Git Bash's `grep -c $'\r$'` says 0: patch scripts read with `newline=''`,
  normalize to `\n` and write CRLF back, or a search for `"...;\n}"` silently finds nothing.
- Writing files from a Bash heredoc turns `\\` into `\` and `\n` into a newline, quoted delimiter or not (`\b` of
  `scripts\basics.py` became a backspace, `\x00` a NUL): write patch scripts with the editor, never in a heredoc.
  `check_boundary.py` fails on a control character in any tracked text file.
- A tool call's text is JSON: a \u escape written in it arrives as the character itself, so a comment meant as
  `\u4e00` lands as a Chinese character in the source (write the backslash twice); a double-quoted
  `python -c "..."` in Bash expands `$*` (the Currency style lost its `$*`). After patching, grep the diff for
  non-ASCII (`grep -nP '[^\x00-\x7F]'`) and for the strings that hold `$`.
- Chain patch steps with `&&`: a failed step followed by `;` reran an old patch script and broke a file.
- Git Bash turns an argument that starts with `//` into a path (`/`): a patch script given `// ---- marker` as an
  argument matched one character late and cut a slash off the file. Put markers inside the script, or start
  them with something else.
- After `Write` / `Edit` of a `.cmd`, keep CRLF, `chcp 65001 >nul` on line 2, and check labels are not duplicated.
