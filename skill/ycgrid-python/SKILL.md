---
name: ycgrid-python
description: "Write, debug and test Python code that controls YCGrid (a C++ spreadsheet that closely mimics Excel): show live market data, scraped or other data in YCGrid; buttons / combo boxes and sheet events, per-cell OnChange, macros, UDFs, charts, custom panels and layouts; and port Excel macros to YCGrid Python. The API uses Excel's names. Use when the workspace has ycgrid.toml, main.py, sheets/*.py, or the user mentions YCGrid, porting macros, sheet events (Worksheet_Change etc.), button Click, combo box Change, macros, UDFs (user-defined functions), live quotes, scraping, data feed processes, Design Mode, charts, panels, containers, saving/loading files, packing a folder for distribution. Use together with the YCGrid MCP tools to inspect and verify the running UI. Also for developing and testing YCGrid itself (build, check_all, feature checks)."
---

# YCGrid Python development

## 1. The system you are working with

- YCGrid is a spreadsheet that closely mimics Excel; the program is `ycgrid.exe` (C++).
- The user only runs ycgrid.exe (or double-clicks a `.ycg`): it starts Python automatically, loads this folder's code starting from **`main.py`**, and reloads automatically after a `.py` file is saved. Never run module files yourself with `python`.
- **Always start reading the project at `main.py`**: it is the entry point (= Excel's ThisWorkbook), and `Workbook_Open` lives there.
- The most common jobs: show live market data or scraped data in YCGrid (data feed processes, §6), trigger actions with buttons, write sheet events.
- Python is **not** inside ycgrid.exe: the `.py` files of this folder are loaded and run by a separate **Python host process**. The two talk both ways over the YCP protocol:
  - Python drives the UI through an **Excel-compatible object model**.
  - The UI sends events, control Click / Change, per-cell OnChange, UDF calculations and stream subscriptions to Python.
- **Anything you can do in the Excel UI, Python can do too** (add and delete sheets, scroll, switch sheets, borders, fill colors, fonts, alignment, merging...), with the same names as Excel.
- Classes, members, parameter names and constant values of the API are the same as Excel's: `Range("A1").Value`, `Cells(r, c)`, `xlUp`.
- You inspect and operate the running YCGrid through the **YCGrid MCP tools** (`yc_*`). After writing code, always reload and verify with the tools.
- **Building an app (a desk, a quote board, a strategy, a screen)? Follow the workflow in [references/app-development.md](references/app-development.md) §0** (find out what the source streams -> structure -> screen -> data by push -> orders and risk -> test like the user -> go live -> report; each stage ends with a check you can show), then its sections: structure, threads, charts, orders, application settings, and the mistakes that already shipped once. Screen layout rules (alignment, the key figure, panels) are in [references/screens.md](references/screens.md). **Then test it like its user with [references/testing.md](references/testing.md)**: a hidden YCGrid on a copy, the same keys, clicks, drags and dialogs a person uses, and a screenshot review list.
- When unsure whether an API exists, look it up with `yc_api_lookup` or read [references/object-model.md](references/object-model.md). **Don't invent members.**
- To do from Python what a ribbon button or menu item does, look the command up in [references/ui-coverage.md](references/ui-coverage.md) (every UI command and its Python members); any command can also be run by id with `yc.ui.command(id)`.

## 2. Project structure (maps to an Excel macro project)

A workbook and its Python code live in the same folder. **Load order: `main.py` -> `sheets\*.py` -> `modules\*.py` -> call `Workbook_Open`.**

| File | Excel equivalent | What goes in it |
|---|---|---|
| `main.py` | ThisWorkbook module | entry point: workbook events such as `Workbook_Open`, startup settings, per-cell OnChange registration, building panels and UI |
| `sheets\Sheet1.py` (file name = the sheet's **CodeName**) | sheet module | sheet events such as `Worksheet_Change`, control events (`CommandButton1_Click`, `ComboBox1_Change`) |
| `modules\*.py` (e.g. `modules\Module1.py`) | standard modules, class modules | macros (public functions without parameters), `@udf`, `@stream`, classes, shared functions |
| `feeds\*.py` | — | data feed processes (live quotes, scrapers...): separate Python processes that YCGrid starts with the host once registered under `[[python.feeds]]` in `ycgrid.toml` (`name`, `script`, `restart = "always"` / `"never"`, `enabled`); their print() shows in the Immediate window (§6) |
| `lib\`, files starting with an underscore | — | helper code, not loaded automatically, can be imported |
| `ycgrid.toml`, `*.ycg` | — | settings and the workbook file (`.ycg` is a single file); never edit `.ycgrid/` by hand |
| `requirements.txt` | — | dependency list. After you add an import, YCGrid detects and installs missing packages and writes them back here |
| `runtime\python\` | — | this project's own portable Python 3.12 (other Pythons on the machine are not affected); never edit files inside it |

A CodeName is not the name on the sheet tab. Use `yc_get_workbook_info` to see each sheet's CodeName and module file.

**Trust (Excel's macro security).** A project runs without a prompt when YCGrid made it (Alt+F11 / the VS Code button), when its
folder is in a Trusted Location (YCGrid's `examples` and projects folder `Documents\YCGrid` by default; add your
own in File > Options > Trust Center Settings > Trusted Locations) or after someone clicked **Enable Content** once (a
Trusted Document). Anyone else's project opens with its code disabled and the yellow Message Bar: nothing runs, Python
functions show `#NAME?` and `=PY` `#BLOCKED!` until Enable Content. Scripts, `yc.launch` and the MCP tools open workbooks
with `Application.AutomationSecurity = msoAutomationSecurityLow` (their code runs, as in Excel); `yc_python_status`
reports `code_disabled` when a person's window kept it from running (ask the user to click Enable Content; never
enable it yourself).

**Module header** (copy it as is, don't leave it out):

```python
# sheet module (e.g. sheets/Sheet1.py)
from yc.excel import *
Me = SheetModule(__name__)
Range, Cells, Rows, Columns = Me.Range, Me.Cells, Me.Rows, Me.Columns   # an unqualified Range means this sheet, like Excel
```

```python
# standard modules in main.py and modules/
from yc.excel import *
```

- Every sheet's CodeName (e.g. `Sheet1`) is a global object in all modules: `Sheet1.Range("A1")`, `Sheet1.CommandButton1`, `Sheet1.ComboBox1.Value`.
- Never `import Sheet1`: that shadows the global object.

## 3. Golden rules

1. **Use Excel's names**: the object model is PascalCase with the same names as Excel. Collections and coordinates are 1-based: `Worksheets("Market")`, `Cells(2, 3)`.
2. **Spell out default properties**: `Range("A1").Value = 5`, not `Range("A1") = 5`; read with `.Value` too.
3. **Use arrays for large amounts of data**:
   - read at once: `v = Range("A2:F5001").Value`; write at once: `Range(...).Value = rows`.
   - numeric data: `ToNumpy()`, and pass numpy arrays directly when writing.
   - cell-by-cell loops only for small amounts (up to about 1,000 cells).
4. **Events must be fast** (target under 50 ms): no `time.sleep`, no network I/O, no `Application.Wait` in handlers. Put long work on a background thread or use `Application.OnTime`.
5. **Prevent recursion**: set `Application.EnableEvents = False` before writing cells inside an event, and set it back to True in `finally`.
6. **UDFs are pure functions**: they must not modify the workbook. Use `@udf(vectorize=True)` for ones called from many cells.
7. **Live market data goes through Topics**: `Range.BindTopics` (no formula cost) or `=QUOTE("name")` cells, and the data source calls `Application.Topics.Push`; never write quotes with `Range.Value` in a loop. Heavy market data and scrapers belong in separate data feed processes (§6), not in the host.
8. **Cancelling events**: Excel's `Cancel = True` is `Cancel.Value = True` or `return True` in a `Before*` handler.
9. **Blank values**: blank cells read as `None`; use `IsEmpty()`, `CDbl()`, `CStr()` before comparing or computing.
10. **Verify after changes**: after changing `.py` files always `yc_reload_python`, then confirm there are no errors with `yc_python_status` and `yc_read_console`.
11. **Safety**: this system may be connected to real trading. Without the user's explicit consent, never run, click or trigger macros, buttons or events that may send orders, change positions or connect to production. Ask when unsure.
12. **Portable**: the whole folder gets copied to other people.
    - Never hard-code absolute paths; base file paths on `ThisWorkbook.Path` (the folder of the workbook and its code).
    - When you need a third-party package just write the `import`: after saving, YCGrid detects and installs missing packages (or use `yc_install_packages`), and the package goes into `requirements.txt`. Don't use the system `pip`, and don't create a venv.
13. **Don't touch**: `ycgrid.exe`, files in `.ycgrid/` and `runtime/`, and modules the user didn't ask you to change.
14. **The UI must be rebuildable**: panels, controls, charts and containers created by code are saved in the workbook. When you change the code that builds the UI, bump `UI_VERSION` in `main.py` so `Workbook_Open` rebuilds the UI (`ThisWorkbook.Store` records the version; `KeepValues=True` keeps values the user entered). Put the same check in `Workbook_AfterReload(Files)`: saving the file then rebuilds the running screen, no Developer > Reload needed.
15. **This skill is YCGrid's**: the copies in a project (`ycgrid-python` in `.claude/skills`, `.agents/skills` and `.github/skills`) are replaced with the version of the YCGrid that opens the project. Write project notes in `AGENTS.md` / `CLAUDE.md`, never in the skill copy.

## 4. Development loop (follow it for every change)

1. **Understand the current state**: `yc_connect` -> `yc_get_workbook_info`; then `yc_read_range`, `yc_list_controls`, `yc_list_charts`, `yc_python_status` as needed.
2. **Confirm the API**: `yc_api_lookup("Range.Find")` and so on.
3. **Edit** the `.py` files.
4. **Reload**: YCGrid reloads automatically after a `.py` file is saved; call `yc_reload_python` to wait for it and get an error summary. Fix syntax errors until it is clean (with a syntax error the old code keeps running); then confirm with `yc_python_status` that macros, UDFs, events, control Click / Change and data feed processes are all fine.
5. **Test**:

   | What to test | Tool |
   |---|---|
   | macros | `yc_run_macro` |
   | button Click, combo box Change | `yc_trigger_control` |
   | per-cell OnChange | `yc_write_range` (write the target cell) or `yc_ui_input` |
   | charts | `yc_list_charts`, `yc_screenshot` |
   | scrolling, layout, panels | `yc_scroll_view`, `yc_manage_layout`, `yc_manage_panels` |
   | Worksheet_Change, SelectionChange, double-click, shortcuts | `yc_ui_input` |
   | UDFs and formulas | `yc_evaluate_formula` |
   | market data events and related formulas | `yc_topics` (action=push, push test values) |
   | live tables, their freshness and contents | `yc_bound_tables` |
   | recorded sessions (replay into the same screen) | `yc_replay` |
   | the whole UI state in one call (dialogs, alarms, figures, selection) | `yc_ui_state` |
   | a complete screen to start from | the example projects in YCGrid's `examples` folder (read `examples\README.md` and the closest `main.py`) |

6. **Check the results**: values with `yc_read_range`; styles and dependencies with `yc_inspect_cells`; output and exceptions with `yc_read_console`; layout and formatting visually with `yc_screenshot`. From a script, `yc.ui.state()` returns the whole state in one call and `yc.launch(hidden=True)` starts a YCGrid without a window for tests.
   **Look at every screenshot as the user would** (`#####`, cut labels, raw floats, a chart over its table, an empty plot): the review list is in [references/testing.md](references/testing.md) §6.
   For an app, write the user's session as a script ([references/testing.md](references/testing.md)) and rerun it after every change.
7. **Report**: what you changed, how you verified it, and what is still untested.

Operations without a dedicated tool (conditional formatting, data validation, defined names...): run object model code with `yc_exec_python`. Tool parameters are in [references/mcp-tools.md](references/mcp-tools.md).

## 5. API quick reference (Excel -> YCGrid Python)

| Task | Excel | YCGrid Python |
|---|---|---|
| read / write one cell | `x = Range("A1")` / `Range("A1") = 5` | `x = Range("A1").Value` / `Range("A1").Value = 5` |
| a specific sheet | `Worksheets("Market").Range("B2")` | same |
| last row | `Cells(Rows.Count, "A").End(xlUp).Row` | same |
| read a block | `arr = Range("A2:F100").Value` -> `arr(i, j)` | same; or `Range(...).ToNumpy()` |
| write a block | `Range("A2:C4").Value = arr` | same (list of lists, `VArray` or numpy) |
| formulas (relative references adjusted) | `Range("D2:D100").Formula = "=B2*C2"` | same |
| font | `.Font.Bold = True`, `.Font.Color = RGB(255, 0, 0)` | same (colors also accept `"#FF0000"`) |
| fill color | `.Interior.Color = RGB(255, 255, 0)` | same |
| borders | `.Borders(xlEdgeBottom).LineStyle = xlContinuous` | same |
| outline | `.BorderAround xlContinuous, xlMedium` | `.BorderAround(xlContinuous, xlMedium)` |
| number format | `.NumberFormat = "#,##0.00"` | same |
| merge | `.Merge` | `.Merge()` |
| column width, autofit | `Columns("A:C").ColumnWidth = 12`, `.AutoFit` | same; `.AutoFit()` |
| clear | `.ClearContents` | `.ClearContents()` |
| insert rows | `Rows(5).Insert` | `Rows(5).Insert()` |
| find | `Set c = Range("A:A").Find("TXO")` | `c = Range("A:A").Find("TXO")` (`None` when not found) |
| intersection | `If Not Intersect(Target, Range("B:B")) Is Nothing` | `if Intersect(Target, Range("B:B")) is not None:` |
| iterate | `For Each c In Range("A1:A10")` | `for c in Range("A1:A10"):` |
| message | `MsgBox "Done", vbInformation` | `MsgBox("Done", vbInformation)` |
| worksheet functions | `WorksheetFunction.VLookup(x, rng, 2, False)` | same |
| run a macro | `Application.Run "Module1.Foo"` | `Application.Run("Module1.Foo")` |
| timers | `Application.OnTime Now + TimeValue("0:00:05"), "Tick"` | `Application.OnTime(Now() + TimeValue("0:00:05"), "Module1.Tick")` |
| debug output | `Debug.Print x` | `Debug.Print(x)` |
| save | `ThisWorkbook.Save` | `ThisWorkbook.Save()` (`.ycg` is a single file) |
| the workbook's folder | `ThisWorkbook.Path` | same (`Application.Path` is the folder of ycgrid.exe) |

**YCGrid extensions [YC]**

| Purpose | How |
|---|---|
| numpy read / write | `a = Range("B2:K1001").ToNumpy()`, `Range("B2:K1001").Value = a` |
| push market data | `ids = Application.Topics.Register(names)`, `Application.Topics.Push(ids, values)` |
| bind market data to cells | `Range("B2:B1001").BindTopics(ids)` (or `=QUOTE("2330.last")` formulas) |
| data refresh interval | `Application.PublishInterval = 250` |
| market data change event | `def Worksheet_LiveChange(Target):` (intersect `Target` with the cells you watch) |
| per-cell OnChange | `Range("B2:B100").OnChange = "main.OnPrice"`, or `@Me.OnChange("B2:B100")` |
| built-in workbook KV | `ThisWorkbook.Store["ui.version"] = 3`, `ThisWorkbook.Store.get("k")` (Excel compatible: `CustomDocumentProperties`) |
| nanosecond timestamps | `Range("A2").ValueNs = 1729658400123456789`; format `"[epoch][tz=+08:00]yyyy-mm-dd hh:mm:ss.000000000"` |
| chart data | `ch.SeriesCollection(1).SetData(x, y, Capacity=100000)`, `.AppendData(x, y)` as ticks arrive |
| custom panels | `Application.Panels.Add("Risk", Dock=ycDockRight)`, `p.Controls.Add(ycButton, "btnGo", OnAction="Module1.Go")` |
| UserForms | `class OrderForm(UserForm):` with `UserForm_Initialize` adding `self.Controls.Add("Forms.TextBox.1", "txtQty")` and `btnOK_Click(self)` methods; `OrderForm().Show()` (modal), `Unload(self)` — see object-model §14B |
| form controls | `ws.Shapes.AddFormControl(xlButtonControl, 10, 10, 80, 24).OnAction = "Module1.Go"`, `ws.DropDowns().Add(...)`, `.ControlFormat.LinkedCell` (the linked cell gets the item number) |
| custom ribbon tabs | `customUI.xml` in the project folder (Excel's RibbonX format; callbacks in `modules/*.py`, see object-model.md §16A2) |
| floating windows and containers | `Worksheets("Quotes").Float(100, 100, 800, 500)`, `Application.Containers.Add("Monitor")`, `Application.Layout.Save("Trading")` |
| more rows | `Worksheets("Log").RowLimit = 10_000_000` (also grows automatically when a write goes past the end) |

## 6. Macros, events, controls, UDFs, streams, data feed processes

**Macros**: top-level functions in standard modules whose names don't start with an underscore and that have no required parameters. They appear in YCGrid's Macros dialog and the Assign Macro list as `module.function`.

**Events**: define a function with Excel's event name in the right module; there is no other registration step. Common events:

```python
# main.py
def Workbook_Open(): ...                          # called every time the host connects: start timers and light market data here
def Workbook_BeforeClose(Cancel): ...             # Cancel.Value = True cancels closing
def Workbook_HostShutdown(): ...                  # [YC] before the host exits or reloads: close connections
def Workbook_AfterReload(Files): ...              # [YC] after saving code reloaded it in place (Workbook_Open doesn't rerun)

# sheets/Sheet1.py
def Worksheet_Change(Target): ...                 # fired by user edits and by code writes
def Worksheet_SelectionChange(Target): ...
def Worksheet_LiveChange(Target): ...             # [YC] market data or recalculation changes, at most once per cycle
```

The full event list, when events fire and their order: [references/events-and-controls.md](references/events-and-controls.md).
Excel's other events fire too, with Excel's arguments and order (BeforeSave / AfterSave, NewSheet, SheetActivate /
Deactivate, SelectionChange, Calculate, BeforeDoubleClick, BeforeRightClick, FollowHyperlink); the Before* ones take
`Cancel.Value = True`.

**Per-cell OnChange** [YC] (more precise and faster than `Worksheet_Change` when only certain cells matter):

```python
# main.py
def OnPrice(Target):                                # Target: the cells that actually changed
    ...
def Workbook_Open():
    Range("Quotes!B2:B100").OnChange = OnPrice           # default: fired by user edits and by code writes
```

- Order: per-cell OnChange -> `Worksheet_Change` -> `Workbook_SheetChange`; `EnableEvents = False` blocks it too.
- To also fire on market data or recalculation changes: `Sources=ycUser | ycPython | ycLive | ycCalc` (merged into one call per publish cycle).

**Controls** (buttons and combo boxes drawn in Design Mode; they can float anywhere):

- Events go in the module of the control's sheet: buttons `def CommandButton1_Click():`, combo boxes `def ComboBox1_Change():` (in `sheets/Sheet1.py`). They can call any Python function.
- Names must be valid Python identifiers; when renaming, rename the function too.
- Combo box lists: `ListFillRange = "Settings!A2:A20"` (reads the cells directly, fastest) or `List = [...]`; the chosen value is `.Value` / `.ListIndex`, and `LinkedCell` syncs both ways with a cell.
- `Placement = ycFloatWindow` fixes a control to the window so it doesn't move when scrolling.
- Add with `yc_add_control` (`kind` = button / combobox, `cover_range` makes it cover given cells); modify or delete with `yc_modify_control`; test with `yc_trigger_control`.
- Check boxes, option and toggle buttons, text boxes, labels, list boxes, spin buttons, scroll bars and images: `OLEObjects.Add("Forms.CheckBox.1", ...)` etc., or `yc_add_control` with `kind`.

**UDFs**:

```python
@udf
def MidPrice(bid: float, ask: float) -> float:   # in a cell: =MidPrice(B2, C2)
    return (bid + ask) / 2

@udf(vectorize=True)                              # many cells: every parameter is a numpy array, called once per batch
def Spread(bid, ask):
    return ask - bid

@udf
def SumPositive(r: Range) -> float:               # Range parameters are read-only snapshots
    return sum(v for v in (c.Value for c in r) if isinstance(v, float) and v > 0)   # numeric cells read as float
```

**Streams**:

```python
@stream("QUOTE")                                  # in a cell: =QUOTE("TXO202610W2C23000", "bid")
class Quote:
    def subscribe(self, topic):   feed.subscribe(topic.Args, topic.Id)
    def unsubscribe(self, topic): feed.unsubscribe(topic.Id)
# when a quote arrives: Application.Topics.Push(topic_id, price)
```

**Data feed processes** (use these to keep sending live market data, scraped data etc. into YCGrid):

Live market data:

```python
# feeds/quotes.py: register it in ycgrid.toml and YCGrid starts it with the Python host
#   [[python.feeds]]
#   name = "quotes"
#   script = "feeds/quotes.py"
import yc
from yc.excel import *

yc.connect()                                              # a data feed is a separate process and connects by itself (controller role)
symbols = load_symbols()                                  # your symbol list, e.g. 1,000 symbols
names = [f"{s}.{f}" for s in symbols for f in ("bid", "ask", "last")]
ids = Application.Topics.Register(names)                  # a list of ids; the same name always gets the same id
Worksheets("Quotes").Range("B2:D1001").BindTopics([ids[i:i + 3] for i in range(0, len(ids), 3)])   # same shape as the range
for batch_ids, batch_values in quote_batches():           # your market data source (e.g. a broker API), one batch at a time
    Application.Topics.Push(batch_ids, batch_values)      # batched push; never blocks (over 1 million cell updates / s)
```

Scraped or API data (tables, low frequency):

```python
# feeds/news.py: register [[python.feeds]] in ycgrid.toml with name = "news", script = "feeds/news.py"
import time
import requests                                           # third-party packages go into runtime\python and into requirements.txt
import yc
from yc.excel import *

yc.connect()
ws = Worksheets("News")
while True:
    try:
        items = requests.get("https://example.com/api/news", timeout=10).json()
        rows = [[it["time"], it["title"], it["source"]] for it in items[:200]]
        rows += [[None, None, None]] * (200 - len(rows))  # pad with blank rows so old data is cleared too
        ws.Range("A2:C201").Value = rows                  # write the whole block at once, not cell by cell
        ws.Range("E1").Value = Now()                      # last update time
    except Exception as ex:
        print("fetch failed:", ex)                        # shows up in ycgrid's Immediate window console
    time.sleep(60)
```

- Data feed processes are started automatically by YCGrid and restarted when they crash; they are separate from the host and don't compete for the GIL, so data keeps updating while the host reloads or is paused in the debugger.
- They can't define events, UDFs or streams and have no CodeName globals: use `Worksheets("name")`.
- High-frequency numbers go through Topics (`Topics.Push`); low-frequency data such as tables and text is written as a block with `Range.Value = 2D array`.
- When the host needs the same topic: `Application.Topics.Lookup("TXO202610W2C23000.bid")`.
- After the script is saved YCGrid restarts the process automatically; confirm with `yc_reload_python`, then check the feed state with `yc_python_status` and the update rate with `yc_topics` (action=stats).
- Small amounts of data can also be handled by a background thread started in `Workbook_Open`, but stop the thread in `Workbook_HostShutdown` so reloads don't leave old threads behind.

## 7. Performance rules

- Writes are buffered and sent in batches, so write-only loops are fast.
- Reading constant cells uses a local cache and is fast too.
- Reading **formula cells** affected by earlier writes has to wait for the recalculation. Avoid "write a cell, read a formula result" in big loops; use arrays, or switch to manual calculation first. As in Excel, manual calculation computes a formula when it is entered but leaves its dependents (and what you read) at their old values until `Application.Calculate()` / F9 (the status bar says Calculate):
  ```python
  Application.Calculation = xlCalculationManual
  try:
      ...
  finally:
      Application.Calculation = xlCalculationAutomatic
  ```
- Every `WorksheetFunction` call in a loop is a round trip: read into an array or dict first and compute in Python.
- Don't position with `Select` / `Activate`; reference Ranges directly.
- Heavy computation (e.g. pricing a whole option chain): use `@udf(vectorize=True)` with numpy for the whole batch, or write formulas so the engine computes in parallel; never call Python cell by cell.
- Live tables (option chains, quote boards, positions): `t = ws.bind(df, key="strike", formats=...)`, then `t.update(changed_rows)`. Data go through shared memory by key, millions of cells per second; see [references/data-binding.md](references/data-binding.md).
- Screens that trade: parameter cells, heartbeats, `stale_after`, a kill switch and logged orders (`yc.safety`, [references/safety.md](references/safety.md)).
- Research plots: `yc.plot` (lines, candles, scatter, histograms, heatmaps; ten million points zoom smoothly; live
  `append` with `follow`), see [references/plot.md](references/plot.md). Record live data and replay it into the same
  screen with `yc.replay` (Trading > Record / Replay), see [references/data-binding.md](references/data-binding.md).
- Trading screens and research are built from the pieces, not from ready-made screens (ADR 0012): bound tables
  (`ws.bind`, flash, renderers, `stale_after`), conditional formats and `GROUPBY` over tables, `yc.interact.on_click`
  for click-to-trade, `yc.alerts`, plot blocks (`candles`, `signals`, `rects`, `drag_line`, events), `yc.trading`
  (OrderManager with risk limits, `on_order` / `on_fill`, PaperGateway), `yc.bars` (ticks to candles), `yc.sim`. The
  maths are numpy / pandas or the user's packages. [references/screens.md](references/screens.md) lists the pieces and
  a recipe per screen (option chain, ladder, blotter, backtest report, research to live, algorithms); complete screens
  are the project folders in YCGrid's `examples` folder (next to ycgrid.exe; `examples\README.md` lists them; read their `main.py` and `examples\lib`), `examples\txo_market_maker` among them.
- Research the user can rerun: a script (or `# %%` cells) that connects with `yc.connect()`; YCGrid has no notebook
  (ADR 0011). Python in cells: `=PY(code, ranges...)` spills DataFrames. Millions of rows: `yc.data.view(df)` (Data
  View) and `Axes.plot_files`.
- Build trading tools from the building blocks instead of asking for features: `rects` (footprint, profiles, zones),
  `fig.on(...)` events and `drag_line` handles (orders you drag), table `render=` (size bars, trends), `yc.feeds.Feed`
  for data sources, `yc.replay.record(everything=True)` for a time machine of the whole screen.

## 8. Porting Excel macros

1. `yc_extract_macros(path)` extracts the modules and controls (no macro is executed).
2. Sheet contents: open the `.xlsx` / `.xlsm` in YCGrid (an old `.xls`: save it as `.xlsx` in Excel first) (values, formulas, formatting and names come along) and save it as `.ycg`; or copy and paste sheets from Excel. Sheet names and CodeNames must match the original (`yc_manage_sheets`).
3. Map the files: ThisWorkbook -> `main.py`; sheet modules -> `sheets/<CodeName>.py`; standard modules -> a `.py` with the same name in `modules/`; class modules -> Python classes in `modules/`; UserForms -> Python classes deriving from `UserForm`.
4. Translate module by module. Keep the original names and comments, and follow the mapping table and pitfall list in [references/excel-migration.md](references/excel-migration.md).
5. Rebuild buttons and combo boxes with `yc_add_control` (same names and positions). OnAction macros of form buttons become calls inside `_Click`; check boxes, option buttons, list boxes, spin buttons and scroll bars are rebuilt as the ActiveX controls of the same kind.
6. `yc_reload_python` -> `yc_python_status` (no missing handlers) -> test them one by one.
7. Report: what was ported, behavior differences, what wasn't tested, and what the user must decide (UserForms, Win32 API, DDE, macros that place orders).

The most common porting mistakes:

- A missing `.Value`.
- `Set ws = ActiveSheet` written as `ws = ActiveSheet`: the bare name follows the active sheet; keep one with `ws = Application.ActiveSheet`.
- Blank cells (Excel's Empty equals both 0 and `""`; Python's `None` equals neither).
- Forgetting +1 on the upper bound of `range()`.
- Concatenating numbers with `&` without converting them to strings.
- `EnableEvents` not restored in `finally`.

## 9. Checklist before you finish

- [ ] `yc_reload_python` has no errors; `yc_python_status` shows no load errors and no controls with a missing Click / Change function.
- [ ] Every changed macro, event, control, UDF and chart was actually tested, and `yc_read_console` was checked.
- [ ] No cell-by-cell read/write loops over large ranges; nothing blocks inside events.
- [ ] Heavy market data and scrapers live in data feed processes: market data is pushed in batches with `Topics.Push`, table data is written in blocks; none of it is in host events or macros.
- [ ] Every place that disables `EnableEvents` in an event restores it in `try` / `finally`.
- [ ] No APIs that don't exist (everything uncertain was checked with `yc_api_lookup`).
- [ ] Nothing that may place orders or change positions was triggered without consent.
- [ ] No hard-coded absolute paths; new third-party packages are in `requirements.txt` (`yc_python_status` shows no missing packages).
- [ ] When the code that builds the UI changed, `UI_VERSION` in `main.py` was bumped, and saving it rebuilt the running screen (`Workbook_AfterReload`).
- [ ] Apps: each stage of the workflow ([references/app-development.md](references/app-development.md) §0) ended with its check: data the source streams comes by push (REST calls per minute measured), click-to-display time measured, screenshots at the user's window size pass the layout rules ([references/screens.md](references/screens.md)).
- [ ] Apps that trade: queued and unanswered orders count against the limits; an order without an answer is UNKNOWN and looked up, never resent; tests ran on a paper copy; nothing went live without the going-live checklist and the user's yes.
- [ ] An app was driven like its user (typing, clicks, panel controls, chart actions, dialogs with Enter / Escape) in a hidden YCGrid, the screenshots were reviewed with [references/testing.md](references/testing.md) §6, and `ui.errors()` is empty.
- [ ] Trading screens: Stop cancels the strategy's orders, limits refuse with a reason, positions reconcile ([references/testing.md](references/testing.md) §7).
- [ ] The report explains the changes, how they were verified and what wasn't verified.

## 10. Reference files (read when needed)

- [references/object-model.md](references/object-model.md): every class, member, parameter, constant and exception. Check it before using an unfamiliar API.
- [references/events-and-controls.md](references/events-and-controls.md): event signatures and when they fire, per-cell OnChange, events and properties of controls (buttons, combo boxes), Design Mode and creating controls in code.
- [references/excel-migration.md](references/excel-migration.md): the full Excel -> Python mapping, performance rewrites, porting examples, pitfall list.
- [references/mcp-tools.md](references/mcp-tools.md): parameters, defaults and nature of every MCP tool.
- [references/testing.md](references/testing.md): testing an app like its user: the session skeleton, every input (`ui.input` tokens, pixels, drags, dialogs), what to check, the screenshot review list, trading checks, smoke tests, reporting YCGrid bugs.
- [references/data-binding.md](references/data-binding.md): live tables bound by key (`ws.bind`, `BoundTable.update`), column formats, flash, freshness, performance.
- [references/safety.md](references/safety.md): safety for live trading: parameter cells, heartbeats and alarms, stale data, operator mode, kill switch, change log.
- [references/plot.md](references/plot.md): fast plots from Python (`yc.plot`): figures, plot types, live appends, linking a plot to a table.
- [references/app-development.md](references/app-development.md): the app workflow (§0), structure, threads, charts, market data by push, orders and live safety, the going-live checklist, testing, and the mistakes table.
- [references/ui-coverage.md](references/ui-coverage.md): every ribbon button, menu item and shortcut with the Python members that do the same (generated).
- [references/developing-ycgrid.md](references/developing-ycgrid.md): working on YCGrid itself: where things are, the build, the test modules, `check_all` / `check_parallel` and the single checks, what every change brings with it (tests, `commands.yaml`, a feature check, translations), pitfalls of UI tests, keeping this skill right.
- [references/screens.md](references/screens.md): screen layout rules (alignment, decimals, the key figure, panels) and trading screens and research from the pieces: a recipe per screen (option chain, price ladder, blotter and positions, backtest report, research to live, execution algorithms, order flow), orders and gateways, click actions and hotkeys, alerts, the simulator and live bars, the examples.
