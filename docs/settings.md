# Settings

YCGrid keeps two kinds of settings:

| Where | Scope | Changed from |
|---|---|---|
| `%LOCALAPPDATA%\ycgrid\settings.json` | this computer, every workbook | File > Options, the menus named below, Python (`Application.*`) |
| `ycgrid.toml` in a project folder | that project; travels with the folder | a text editor |

## settings.json

The file is written as soon as a value changes. File > Options changes most keys; the status bar's right-click menu,
the Quick Access Toolbar and the error checking options button change their own; the rest are set from Python (the
`Application` members named below) or in the file. Missing keys take the defaults below, values of the wrong type are
ignored, and keys YCGrid doesn't know are kept when the file is written again, so different YCGrid versions can share
it. The file is plain JSON; `//` comments are accepted when reading, but are lost when YCGrid rewrites the file.

```json
{
  "language": "zh-TW",
  "general": { "show_quick_analysis": true, "standard_font": "", "standard_font_size": 0, "sheets_in_new_workbook": 1 },
  "view": { "gridlines": true, "headings": true, "formula_bar": true, "formula_bar_lines": 1 },
  "status_bar": { "average": true, "count": true, "numerical_count": false, "minimum": false, "maximum": false, "sum": true },
  "calculation": { "automatic": true, "excel_15_digit_input": false, "iterative": false, "max_iterations": 100, "max_change": 0.001 },
  "formulas": { "r1c1_reference_style": false, "autocomplete": true, "function_tooltips": true, "error_checking": true },
  "live_data": { "publish_interval_ms": 100 },
  "projects": { "folder": "" },
  "editor": { "vscode": "", "restart_on_save": false },
  "editing": { "cell_drag_and_drop": true, "move_after_return": true, "move_after_return_direction": 0, "edit_directly_in_cell": true,
               "enable_autocomplete": true, "follow_links_on_click": true, "show_paste_options": true, "custom_lists": [["Low", "Medium", "High"]] },
  "data": { "pivot_auto_group_dates": true },
  "save": { "autorecover": true, "autorecover_minutes": 10, "autorecover_folder": "" },
  "ribbon": { "quick_access": ["Save", "Undo", "Redo", "Screenshot"], "display": 0 },
  "errors": { "log_to_file": true, "log_folder": "" },
  "trust_center": { "macro_settings": "disable_with_notification", "message_bar": true,
                    "trusted_locations": [{ "path": "%YCGRID_DIR%\\examples", "subfolders": true, "description": "YCGrid examples", "modified": "" }] }
}
```

| Key | Default | Meaning |
|---|---|---|
| `language` | `""` | `"en"`, `"zh-TW"`, or `""` for the Windows display language. Takes effect after a restart. `--lang` and the `YCGRID_LANG` environment variable override it. |
| `general.show_quick_analysis` | `true` | the Quick Analysis button at a selection's corner (Ctrl+Q works either way; `Application.ShowQuickAnalysis`) |
| `general.standard_font`, `standard_font_size`, `sheets_in_new_workbook` | `""` (Calibri), `0` (11), `1` | what a new workbook starts with (`Application.StandardFont`, `StandardFontSize`, `SheetsInNewWorkbook`) |
| `view.gridlines`, `view.headings`, `view.formula_bar` | `true` | show gridlines, row and column headings, the formula bar |
| `view.formula_bar_lines` | `1` | the formula bar's height in lines (its chevron or Ctrl+Shift+U: 1 or 3; dragging its bottom edge: up to 20; `Application.FormulaBarHeight`) |
| `status_bar.average`, `count`, `numerical_count`, `minimum`, `maximum`, `sum` | Average, Count and Sum on | the selection's statistics in the status bar (right-click the status bar to choose) |
| `status_bar.cell_mode`, `caps_lock`, `num_lock`, `scroll_lock`, `end_mode`, `selection_mode`, `view_shortcuts`, `zoom_slider`, `zoom` | all on but Num Lock | the status bar's other entries, as in Excel's Customize Status Bar |
| `status_bar.error_log` | on | the Error Log's entry on the status bar: a click opens the window; red with the count while errors are unseen, the number of new messages otherwise |
| `calculation.automatic` | `true` | automatic calculation (`false`: manual; F9 calculates). A workbook is saved with its mode, and opening it sets the session's, as in Excel; this is the mode of new workbooks and older files |
| `calculation.data_tables` | `true` | `false` with `calculation.automatic` on: Automatic except for Data Tables (What-If Analysis' data tables are calculated by F9 only) |
| `calculation.excel_15_digit_input` | `false` | `true`: digits past the 15th significant digit become 0, as in Excel; `false`: such numbers are kept exactly, as decimals |
| `calculation.iterative`, `max_iterations`, `max_change` | `false`, `100`, `0.001` | iterative calculation: circular references are recalculated until the largest change of a round is below `max_change`, at most `max_iterations` rounds (1 to 32767). A workbook is saved with the settings it was calculated with, and opening it sets them for the session, as in Excel (settings.json keeps these defaults for new workbooks and older files) |
| `formulas.autocomplete`, `function_tooltips` | `true` | Formula AutoComplete (the list of functions, names and table columns while a formula is typed) and the argument hint (`Application.DisplayFormulaAutoComplete`, `DisplayFunctionToolTips`) |
| `formulas.error_checking`, `error_evaluate_to_error`, `error_number_as_text`, `error_inconsistent_formula`, `error_text_date`, `error_omitted_cells`, `error_unlocked_formula`, `error_list_data_validation`, `error_inconsistent_list_formula`, `error_misleading_format`; `error_empty_cell_reference` | `true`; `false` | background error checking: a green triangle on cells that give an error, hold a number stored as text or a formula unlike its neighbors' formulas, and the error options button beside the selected cell (`Application.ErrorCheckingOptions`) |
| `formulas.recent_functions` | `["SUM", "AVERAGE", "IF", "HYPERLINK", "COUNT", "MAX", "SIN", "SUMIF", "PMT", "STDEV"]` | Insert Function's Most Recently Used list (also the Name Box's list while a formula is typed), newest first, at most 10 |
| `calculation.before_save`, `format_stale_values` | `true`, `true` | a manual-calculation workbook is calculated when it is saved; its cells waiting to be calculated show struck through (`Application.CalculateBeforeSave`, `FormatStaleValues`) |
| `calculation.multi_threaded`, `threads` | `true`, `0` | multi-threaded calculation, and its threads (0: every processor but the UI's and Python's; `Application.MultiThreadedCalculation`) |
| `formulas.generate_getpivotdata` | `true` | pointing at a PivotTable's value while typing a formula writes `GETPIVOTDATA(...)` (`Application.GenerateGetPivotData`) |
| `formulas.r1c1_reference_style` | `false` | R1C1 reference style: column headings are numbers, and formulas are shown and typed as `R[-1]C` (`Application.ReferenceStyle`) |
| `live_data.publish_interval_ms` | `100` | the shortest time between screen updates caused by Python and data feeds (0 to 5000; 0: every change). The first change after a quiet period is shown at once; typing and clicking are never delayed. `Application.PublishInterval` changes it for the running session. |
| `projects.folder` | `""` | where a hidden instance (`yc.launch(hidden=True)`) creates the project folder of a never-saved workbook when a VS Code command runs (`""`: `Documents\YCGrid`); with a window, YCGrid asks where to save the workbook first |
| `editor.vscode` | `""` | the path of `Code.exe` / `code.cmd` (`""`: found where VS Code is usually installed: `code.cmd` on PATH, the user and system installers' folders, Scoop, what Windows lists for it, then VS Code Insiders). When it isn't found, Alt+F11 asks for it (Browse...) and keeps the answer here |
| `editor.restart_on_save` | `false` | what saving a project file does. `false`: its functions and classes are swapped in the running Python host (live tables, threads, figures and variables stay; the file's other top-level code doesn't run again; adding or deleting a file restarts the host). `true`: the host restarts |
| `editing.cell_drag_and_drop` | `true` | drag the selection's border to move cells, and the fill handle to fill (`Application.CellDragAndDrop`). A protected sheet allows neither |
| `editing.move_after_return`, `move_after_return_direction` | `true`, `0` | Enter (Shift+Enter: the other way) moves the selection after an entry: 0 down, 1 right, 2 up, 3 left (`Application.MoveAfterReturn`, `MoveAfterReturnDirection` with `xlDown` / `xlToRight` / `xlUp` / `xlToLeft`) |
| `editing.edit_directly_in_cell` | `true` | F2 and a double-click edit inside the cell; `false`: in the formula bar (`Application.EditDirectlyInCell`) |
| `editing.enable_autocomplete` | `true` | typing the start of a text already in the column completes it (`Application.EnableAutoComplete`) |
| `editing.follow_links_on_click` | `true` | one click on a cell with a hyperlink follows it, and click-and-hold selects the cell, as in Excel; `false`: only Ctrl+click follows it (for trading screens, where a stray click must not open a page) |
| `editing.show_paste_options`, `show_insert_options`, `alert_before_overwriting` | `true` | the Paste Options / Auto Fill Options and Insert Options buttons; the question before dropped cells replace data (`Application.AlertBeforeOverwriting`) |
| `editing.large_operation_alert`, `large_operation_thousands` | `true`, `33554` | the question before a person's paste or Fill Down / Right over more than that many thousand cells (`Application.EnableLargeOperationAlert`, `LargeOperationCellThousandCount`; a hidden instance starts with it off) |
| `chart.show_tip_names`, `show_tip_values` | `true` | the tip over a chart names the element under the mouse and shows a point's value (`Application.ShowChartTipNames`, `ShowChartTipValues`) |
| `editing.auto_flash_fill` | `true` | Advanced > Automatically Flash Fill: grey suggestions while typing beside a column that shows a pattern (`Application.FlashFill`) |
| `editing.custom_lists` | `[]` | the user's custom lists (File > Options > Edit Custom Lists; `Application.AddCustomList`): AutoFill continues them and Sort can order by them. Excel's built-in lists (days, months, Chinese days / months / quarters, earthly branches, heavenly stems) come first and aren't stored |
| `insert.recent_symbols`, `recent_shapes` | `[]` | Insert > Symbol's recently used symbols (at most 20) and Insert > Shapes' Recently Used Shapes (command ids, at most 24) |
| `proofing.ignore_uppercase`, `ignore_numbers`, `ignore_internet`, `flag_repeated` | `true` | File > Options > Proofing: words in uppercase, words with numbers and Internet and file addresses are skipped by the spelling checker; repeated words are flagged |
| `proofing.autocorrect_replace`, `autocorrect`, `autocorrect_removed` | `true`, `[]`, `[]` | AutoCorrect: Replace text as you type (`Application.AutoCorrect.ReplaceText`), the user's own replacements (`[[from, to]]`) and the built-in ones the user deleted |
| `page.last_margins` | none | Page Layout > Margins > Last Custom Setting: left, right, top, bottom, header and footer in points (written once custom margins were set) |
| `data.pivot_auto_group_dates` | `true` | a date field added to a PivotTable's Rows or Columns is grouped by years, quarters and months (months alone when the dates fall in one year), as in Excel; File > Options > Data > "Disable automatic grouping of Date/Time columns in PivotTables" turns it off |
| `save.autorecover`, `autorecover_minutes`, `autorecover_folder` | `true`, `10`, `""` | AutoRecover: an unsaved workbook is copied every N minutes (1 to 120) into the folder (`""`: `%LOCALAPPDATA%\ycgrid\AutoRecover`); a clean exit or a save removes the copy, and after a crash YCGrid shows the Document Recovery pane (`Application.AutoRecover`) |
| `ribbon.quick_access` | `["Save", "Undo", "Redo", "Screenshot"]` | the Quick Access Toolbar's command ids (right-click a ribbon command: Add to Quick Access Toolbar; or the toolbar's drop-down: More Commands) |
| `ribbon.display` | `0` | 0: always show the ribbon; 1: show tabs only (Ctrl+F1); 2: auto-hide (a thin bar brings it back until the next click on the sheet) |
| `errors.log_to_file`, `log_folder` | `true`, `""` | every error YCGrid shows or receives (message boxes, Python's errors and tracebacks, the Python host or VS Code failing) is listed in View > Error Log and appended to `errors-YYYY-MM-DD.log` in the folder (`""`: `%LOCALAPPDATA%\ycgrid\logs`; files older than 30 days are removed). Hidden instances (tests, agents) don't write it; the environment variable `YCGRID_ERROR_LOG=<folder>` writes there in any case. Hang and crash dumps (`ycgrid-hang-*.dmp`, `ycgrid-crash-*.dmp`) go to the same folder (hidden instances: `%TEMP%\ycgrid-dumps`) |
| `trust_center.macro_settings` | `"disable_with_notification"` | Trust Center > Macro Settings, for a workbook's Python project that isn't trusted: `"disable_with_notification"` (its code doesn't run, and the Message Bar offers Enable Content), `"disable_without_notification"`, or `"enable_all"` (not recommended). `Application.TrustCenter.MacroSettings` |
| `trust_center.message_bar` | `true` | show the Message Bar when Python code has been disabled (`false`: "Never show information about blocked content"; File > Info still offers Enable Content) |
| `trust_center.trusted_locations` | YCGrid's own folders | Trusted Locations: workbooks in these folders run their code without asking. Each has `path` (`%YCGRID_DIR%` is ycgrid.exe's folder, `%YCGRID_PROJECTS%` the projects folder; other `%VARIABLES%` are expanded too), `subfolders`, `description` and `modified`. When the key is missing: `%YCGRID_DIR%\examples` and `%YCGRID_PROJECTS%`, with subfolders, so the examples (the demo among them) and the projects you make with Alt+F11 run without a prompt. `Application.TrustCenter.AddTrustedLocation` / `RemoveTrustedLocation` |
| `trust_center.allow_network_locations`, `disable_trusted_locations` | `false`, `false` | Allow Trusted Locations on my network (UNC paths and network drives are refused otherwise); Disable all Trusted Locations |
| `trust_center.protected_view_internet`, `protected_view_unsafe_locations`, `protected_view_outlook` | `true` | Trust Center > Protected View: files a person opens that come from the Internet (the Mark of the Web: `Zone.Identifier` ZoneId 3 or 4), lie in a browser cache (potentially unsafe locations) or in Outlook's attachment folders open read-only, with their Python code off, until Enable Editing. `Application.TrustCenter.ProtectedViewInternet`, `ProtectedViewUnsafeLocations`, `ProtectedViewOutlook` |
| `trust_center.allow_network_documents`, `disable_trusted_documents` | `true`, `false` | Allow documents on a network to be trusted; Disable Trusted Documents (Enable Content then enables the code for this session only) |

The Trust Center applies to workbooks a person opens. Workbooks opened by scripts (`Workbooks.Open`, `yc.launch(file)`,
the MCP tools) follow `Application.AutomationSecurity` instead (`msoAutomationSecurityLow` by default, as in Excel:
their code runs). So does everything in a hidden instance. A hidden instance also ignores the `trust_center` keys and
never writes `settings.json`: it keeps its changes in memory, unless `YCGRID_SETTINGS` names a file.

Older versions stored the language in `%LOCALAPPDATA%\ycgrid\language` (one line). It is still read when
`settings.json` has no `language`, and moved into `settings.json` on the next save.

Other files in `%LOCALAPPDATA%\ycgrid\`:

- `trust.json`: the Trusted Documents (each project folder trusted with Enable Content, by its path and identity;
  Trust Center > Trusted Documents > Clear empties it) and, under `editing_enabled`, the files that left Protected View
  with Enable Editing (by path, size and time).
- `recent.json` and `recent_pinned.json`: the recent workbooks and the pinned ones.
- `instances\`: the running instances, which Python and the MCP server use to find YCGrid.
- `default_layout.json`: View > Save Layout / Load Layout for a never-saved workbook, and the layout YCGrid starts
  with when no workbook is given. For a saved workbook they use `<file>.ycg.layout.json` next to it. Every saved
  workbook also keeps its layout inside the file (the `LAYT` chunk) and opens with it.

## ycgrid.toml

The project settings YCGrid reads:

| Key | Meaning |
|---|---|
| `[python] interpreter` | the Python to use (relative to the project folder); empty: the project's `runtime\python`, then the one next to `ycgrid.exe` |
| `[python] auto_install` | `ask`, `always` or `never`: what to do about missing packages |
| `[python] index_url` | the package index, for example a company mirror |
| `[python] find_links` | a folder of wheels for offline installation (relative to the project); default `runtime\wheels` |
| `[python] paths` | folders of shared code the project imports as if they were in its `lib\` (relative to the project, e.g. `["../lib"]`, as YCGrid's examples share `examples\lib`): added to `sys.path` after `lib\`, their modules are the project's own (never offered as missing packages) and their imports count for package detection; `python -m yc pack` copies them into the packed project's `lib\`, and the VS Code settings name them for Pylance |
| `[python] version` | written by the project template (`"3.12"`) as a note of the Python version the project was made for; `python -m yc python upgrade <version>` downloads another and tells you to update it. YCGrid doesn't read it |
| `[[python.feeds]]` | a data feed process, one table per feed: `name`, `script` (relative to the project), `restart` (`"always"`, the default: restarted 3 seconds after it exits, at most 5 times a minute; or `"never"`), `enabled` (`true`). Each feed runs in its own process of the project's Python, starts and stops with the Python host, and gets `YCGRID_PID` (so `yc.connect()` connects to this YCGrid) and `YCGRID_FEED` (its name); its `print()` output and errors show in the Immediate window |

Missing packages: a module whose name differs from its package's name on PyPI (`cv2` is `opencv-python`, `PIL` is
`Pillow`...) is looked up in a built-in table in `python/yc/env.py`.

## Environment variables

| Variable | Meaning |
|---|---|
| `YCGRID_LANG` | the UI language (`en`, `zh-TW`), over `settings.json`; `--lang` on the command line comes first |
| `YCGRID_SETTINGS` | another settings file to read and write instead of `settings.json`, hidden instances included (tests that restart a hidden instance read back what it saved) |
| `YCGRID_ERROR_LOG` | a folder where the error log is written, hidden instances included |
| `YCGRID_EXE` | the `ycgrid.exe` that `yc.launch()` and `python -m yc` start (default: the wheel's copy, then the YCGrid folder the `yc` package is in) |
| `YCGRID_PYTHON` | the Python the Python host runs on; set by `yc.launch()` and `python -m yc run` for a pip-installed YCGrid |
| `YCGRID_PID`, `YCGRID_FEED` | set for data feed processes: the YCGrid that started them, and the feed's name |
| `YCGRID_RELOAD` | `restart`: saving a project file restarts the Python host instead of swapping its functions |
| `YCGRID_REQUEST_TIMEOUT` | how long, in seconds, a Python request waits for YCGrid's reply before it raises `E_TIMEOUT` (default 600; 120 for `yc.launch(hidden=True)`; 0 or less: no limit) |
| `YCGRID_OPEN_PASSWORD` | set by `Workbooks.Open(..., Password)` for the YCGrid it starts; read once and cleared at startup |
