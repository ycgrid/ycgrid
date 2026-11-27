# Testing an app the way its user uses it

Unit tests prove the logic; only driving the real screen finds what a trader or researcher runs into: a dialog that
Enter doesn't close, a chart that covers the table, a strategy that keeps quoting after Stop, `#####` in a cell. This
is the workflow that found those bugs. Follow it for every app (desk, quote board, research screen) before saying it
works, and keep the script: run it after every change.

## 1. The loop

1. **Copy** the project folder to a temporary folder (never test on the user's files or their running YCGrid).
2. **Launch a hidden YCGrid** on the copy and wait for `Workbook_Open` (§2).
3. **Act like the user**, through the same inputs a person uses (§3): typing into cells, keys, clicks and drags,
   panel controls, chart mouse actions, dialogs. Code that writes cells directly skips the events and the UI paths
   the user goes through.
4. **Check what the user would look at** after every step (§6): the cells' shown text, the status bar, the dialog
   state, the figures' view and handles, alarms, the Error Log, and the screenshot itself.
5. **Look at the screenshots.** Most user-facing bugs are visual (§6). Read each picture as the trader would.
6. **Fix, rerun everything, keep the script** next to the app (`tests\session.py`).

## 2. A session skeleton

```python
import os, shutil, sys, tempfile, time
EXE = r"<YCGrid folder>\ycgrid.exe"
sys.path.insert(0, os.path.join(os.path.dirname(EXE), "python"))
import yc

dst = os.path.join(tempfile.mkdtemp(), "App")
shutil.copytree(r"<project>", dst, ignore=shutil.ignore_patterns("__pycache__", "tests"))
p = yc.launch(os.path.join(dst, "App.ycg"), hidden=True, exe=EXE, lang="en")
conn = yc._conn.current()          # the connection launch made (120 s request timeout, as yc.connect gets for a hidden instance)
from yc import excel, ui
from yc.mcp import tools
end = time.time() + 60             # Workbook_Open has run (also True for a workbook without Python)
while not conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
    time.sleep(0.2)
results = []

def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("OK   " if ok else "FAIL ") + name + ("" if ok else f"   <- {detail}"), flush=True)

def wait(cond, secs=4.0):          # poll a condition instead of fixed sleeps (live screens update 4x a second)
    end = time.time() + secs
    while time.time() < end:
        try:
            if cond():
                return True
        except Exception:
            pass
        time.sleep(0.1)
    return cond()

def inp(script):                   # raw input, see §3
    conn.request("ui.input", {"script": script, "timeout_ms": 8000}, timeout=20)
    time.sleep(0.3)

try:
    ...                            # the steps
finally:
    check("no errors in the Error Log", not ui.errors(), ui.errors())
    p.terminate()
```

- Every wait has a time limit, and the script always reaches `p.terminate()` (`try` / `finally`; a
  `faulthandler.dump_traceback_later(..., exit=True)` guard skips it): a hidden YCGrid left running keeps
  `ycgrid.exe` locked, and the next build can't copy over it.
- `lang="en"` keeps texts stable for checks; run once with the user's language too and look at the screenshots.
- `size=(1920, 1080)` (the user's screen) for screenshots that show what they see; panels and frozen layouts depend
  on it.
- An app that can trade: overwrite the copy's settings to paper right after copying (`config.ini` -> `mode = paper`),
  so a test never reaches a live account.
- A hidden instance leaves the user's own YCGrid alone: it doesn't write their settings, Recent list or default window
  layout, and the project folders it makes for unsaved workbooks (Book1, Book2...) and its recordings go to
  `%TEMP%\YCGrid-hidden-projects`, not to Documents. It also starts from a clean layout, not the user's.
- A hidden instance started by `yc.launch(hidden=True)` ends when the process that launched it ends
  (`--parent-pid`), even when the script is killed. One started another way (`ycgrid.exe --hidden` from a shell, or
  by an older YCGrid) runs until logoff, and `scripts\build.cmd` then fails at staging with `file COPY cannot copy
  file`: find it with `Get-CimInstance Win32_Process -Filter "Name='ycgrid.exe'"` (its command line has `--hidden`)
  and close it before building again. Hold the `Popen` that `yc.launch` returns and `terminate()` it in a `finally`.
- Saving writes the file on a worker thread (Python's `SaveAs` / `Save` / `SaveCopyAs` and the user's Ctrl+S, Save
  As): the screen and live data go on. After Ctrl+S in a test, wait for the file's time to change (and for
  `ActiveWorkbook.FullName` after Save As) instead of reading it at once.
- Operator Mode off makes the UI read-only: every change the user makes is refused with the status bar's
  "Read-only mode: ..." message, never a message box. Commands that change the workbook (also `yc.ui.command` /
  `ExecuteMso` of them, Undo included) don't run and open no dialog; a dialog, tab name box or note box open when the
  mode comes on closes unsaved. Python's own writes are not affected. A test that toggles it puts it back with
  `yc.safety.set_operator_mode(True)`.
- To test a new workbook: `yc.launch(hidden=True)`, build it, `excel.ThisWorkbook.SaveAs(path)`, close, launch the
  saved file (that also tests saving and reopening).
- A hidden instance opens no VS Code window: Alt+F11, View Code and Debug in VS Code note what they would open in
  `ui.state()["vscode_request"]` without looking for VS Code. Started with `YCGRID_NO_VSCODE=1` it looks nowhere but
  the settings (`editor.vscode`), as on a machine without VS Code: Alt+F11 then asks "VS Code not found" (Browse...
  takes a path queued with `ui.answer_file_dialogs`, and keeps it in the settings; give the instance its own
  `YCGRID_SETTINGS` file).
- Nor does it open a link, file or folder on the desktop (a click on a web link, Open Hyperlink, a file's folder):
  `ui.state()["desktop_open"]` is the last target it would have opened and `["desktop_opens"]` how many times it was
  asked, so a test of a click that follows a link compares the count before and after.
- A project from scratch: `python -m yc init <folder>` (run with YCGrid's own Python from `<YCGrid>\python`; the other
  `python -m yc` commands are in [app-development.md](app-development.md) §1).

`ycgrid.exe` command line (what `yc.launch` passes, and quick checks without Python):

| Option | Meaning |
|---|---|
| `<file>` | open a `.ycg`, `.xlsx` / `.xlsm` or `.csv` |
| `--hidden` | no window on screen (CI, agents); screenshots still work; no AutoRecover, settings, Recent list or default layout written |
| `--parent-pid <pid>` | end with that process (`yc.launch(hidden=True)` passes its own) |
| `--automation-security <n>` | the workbook given on the command line follows `Application.AutomationSecurity`: 1 Low (its code runs; the `yc.launch` default), 2 ByUI (the Trust Center decides), 3 ForceDisable |
| `--new-window` | a process of its own even when YCGrid already runs (else the file is handed to the running one; `--no-handoff` only skips that) |
| `--lang en` / `zh-TW` | the UI language (over `YCGRID_LANG` and the settings) |
| `--width <px>`, `--height <px>` | the window size |
| `--frames <n>` | run n frames and exit (a smoke test); `--screenshot <file.png>` saves the last frame (30 frames when `--frames` isn't given) |
| `--keys "<script>"` | a `ui.input` script run after start (§3); `--type "<text>"` types into A1 |
| `--save-as <file>` | save the workbook before exiting (with `--frames`) |
| `--report <file.json>` | on exit, write frame statistics (`frames`, `frame_cpu_ms` avg / p50 / p99 / max...) |
| `--warp` | draw with the WARP software renderer (machines without a GPU) |
| `--demo`, `--big-data` | fill the sample market sheet / a million rows (rendering checks) |
| `--ribbon-tab <n>`, `--collapsed`, `--backstage`, `--top-row <n>`, `--float-sheet <n>`, `--new-container`, `--dock-into-container`, `--save-layout`, `--load-layout`, `--expand-rows` | start in that UI state (screenshots of one state) |
| `--install` / `--uninstall` [`--silent`] | register / remove the `.ycg` file association (the only thing that writes the registry) |

```bat
ycgrid.exe book.ycg --frames 60 --screenshot shot.png --lang en --width 1600 --height 1000
```

## 3. Inputs a person uses

| What | How |
|---|---|
| select, type, keys, click cells | `tools.yc_ui_input([{"type": "select", "address": "B2"}, {"type": "type", "text": "=A1*2"}, {"type": "keys", "keys": "Ctrl+D"}])` |
| panel buttons, spins, combos, check boxes | `tools.yc_panel_input("Panel", "btnBuy")`, `tools.yc_panel_input("Panel", "spQty", "set_value", "3")` (a spin or slider lands where the user could put it: 9 on a spin with Max 5 gives 5) |
| sheet controls (buttons drawn in Design Mode) | click the cell under them: `inp("A:D2\|w:8")`; or `tools.yc_trigger_control` |
| chart mouse actions | `fig.input(...)` / `tools.yc_plot_input(key, [...])`: click, double_click, context_menu, drag_handle, close_handle, wheel, live |
| ribbon / menu commands | `ui.command("InsertTable")` runs exactly what the button runs; clicking the ribbon itself: §4 |
| dialogs | §5 |

`ui.input` scripts (`conn.request("ui.input", {"script": ..., "timeout_ms": 15000, "dialogs": "auto"})`), steps
separated by `|`, one step per frame. The request returns `{"done": True}` once every step ran (`False` when
`timeout_ms` passed first). While it runs, a MsgBox / InputBox from Python is answered with its default
(`"dialogs": "show"` leaves them on screen; a hidden YCGrid answers them anyway). A script with an unknown key is
refused (`YcError`, `E_ARG`) before any step runs. Each step is a letter, a colon and its argument; a step whose letter
is none of those below is a key (`k:` by convention). `--keys "..."` on the command line takes the same script.

| Token | Meaning |
|---|---|
| `t:text` | type characters (into the cell editor, a text box...) |
| `k:Enter`, `k:c-T`, `k:s-F2`, `k:c-a-K` | a key with modifiers `c-` Ctrl, `s-` Shift, `a-` Alt; ImGui key names in any case: `Enter`, `Escape`, `Tab`, `Delete`, `F2`, `DownArrow`, `PageDown`, `A`, `1`, `Semicolon`, `Equal`, `GraveAccent`, plus `Down` / `Up` / `Left` / `Right`, `Esc`, `Del`, `Ins`, `Return`, `PgUp` / `PgDn`, `Space`, `Ctrl`, `Shift`, `Alt` (`k:Alt` alone shows the KeyTips) and the characters `;` `=` `-` `,` `.` `/` `'` `[` `]` `\` (and the backtick, `GraveAccent`). An unknown key name makes `ui.input` raise before any step runs. `k:s-Space` also types the space Windows sends for it, as a keyboard does (the grid must not take it as typing) |
| `g:B2:E21` | select an address (as the Name Box does) |
| `A:C5` / `Q:C5` / `R:C5` | click / double-click / right-click the center of a cell (scrolled into view) |
| `m:x,y` | move the mouse (window pixels) |
| `c:x,y` / `D:x,y` | click / double-click at a pixel |
| `r:` | right-click where the mouse is |
| `d:x,y,x2,y2` | press at x,y, drag to x2,y2 in 8 frames, hold 4 frames there, release |
| `p:x,y` / `u:` | press the left button at x,y and keep it down / release it: with `m:` steps between, a drag a hand makes (window docking, see §4) |
| `P:x,y` / `U:` | the same with the right button: a right-drag (the drop menus of the selection's border and the fill handle open when `U:` lets go) |
| `W:n` | turn the mouse wheel n notches where the mouse is (`W:-5` scrolls down 15 rows; put the mouse over the grid with `m:` first) |
| `H:ctrl` / `H:shift` / `H:alt`, `h:` | hold a modifier down through the following steps / release the held ones: Ctrl+drag (copy a sheet tab or cells), Shift+click |
| `x:CommandId` | run a command inside the script |
| `I:text` / `J:text` | an IME composition in progress / its committed result (CJK input tests) |
| `w:n` / `s:ms` | wait n frames / sleep ms milliseconds (give the UI frames to react: `w:3` after a click, `w:8` after opening a window) |

## 4. Pixels: ribbon, headers, drags

- `ui.screenshot(path, max_width=3000)` saves the window in its own pixels: positions read from it are the
  coordinates `ui.input` takes. Smaller screenshots are scaled (a 2160-pixel window at `max_width=1600` is 0.74x).
- The ribbon's tab and arrow positions are in `ui.state()["ribbon"]` (`tabs`, `arrow`, `collapsed`, `peek`,
  `popup_open`). Wait more than half a second between two clicks on the same tab (a double-click pins the ribbon).
- **Hover before you drag**, as a hand does: `inp("m:145,230|w:4|d:145,230,145,471|w:10")`. The fill handle and
  other hot spots react to the hover of the frame before the press.
- Column widths: drag the border in the header row; check `Columns("B").ColumnWidth`.
- Positions to drag from: `ui.cell_rect("B2")` (a range of the active sheet, scrolled into view) and
  `ui.windows()` (`rect`, `docked` of every window). A selection's border is `rect[2] - 1`, a column's border in the
  header row is above `rect[1]`; a dock splitter is the gap between two windows' rects. Hover 6 frames before a
  splitter drag (`m:x,y|w:6|d:...`), and drag where no floating window covers the spot (it takes the press).
- Check a lock both ways: the drag works unlocked, then does nothing locked (a test that only checks "didn't move"
  passes when the drag missed).
- **Click what a person clicks, found by name**: `ui.ribbon_items()` lists the ribbon buttons, open menu entries
  (submenus too), gallery tiles and Backstage entries on the screen now, as `{cmd, where, label, rect}` (label: the text shown); `where` is
  `"ribbon"`, `"dropdown"` (click near its right edge or bottom to open its menu), `"combo"`, `"menu"`, `"submenu"`
  (hover it to open), `"tile"` or `"backstage"`. Click a tab first (positions in `ui.state()["ribbon"]["tabs"]`).
  `ui.items(window="")` lists the controls drawn in the last frame, `{label, id, window, rect, disabled}`, and
  `checked` (True, False or `"mixed"`) for a check box, an option button (the one chosen is True) or a checkable menu item: dialog
  buttons (`"Replace All"`), check boxes, list entries, sheet tabs (label = the sheet's name), the status bar's REC
  chip (`id == "##rec_chip"`); with a window filter it also lists that window's unlabelled controls (color swatches).
  Clicking these is how `scripts\feature_check.py` reaches every command (§8).

### Windows: dock them and take them out again

Users dock windows wherever they like, YCGrid's own too (Error Log, Immediate, Latency Monitor...). Every window that
goes into a dock must come out again; test it for each kind of window on the app's real screen, with the app's locks
on. A docked window shows its tab; the user takes it out by dragging the tab off, double-clicking it, or the tab's
right-click menu > Float (also in the tab bar's arrow menu). Windows the workbook locks
(`Workbook.Protect(Windows=True)`, `.Locked`) stay; YCGrid's own windows never lock.

```python
def hand_drag(x, y, tx, ty):       # press, move in steps, hold over the target, release: docking needs the hold
    steps = "|".join(f"m:{x + (tx - x) * k // 10},{y + (ty - y) * k // 10}|w:2" for k in range(1, 11))
    inp(f"m:{x},{y}|w:4|p:{x},{y}|w:3|{steps}|w:6|u:|w:8")

ui.command("ErrorLog")                                   # shows / hides it
w = next(x for x in ui.windows() if "yc_errors" in x["name"])
docked = [x for x in ui.windows() if x["docked"]]        # the dock space's edges have the drop targets
bottom = max(x["rect"][3] for x in docked)
mid = (min(x["rect"][0] for x in docked) + max(x["rect"][2] for x in docked)) // 2
hand_drag(w["rect"][0] + 60, w["rect"][1] + 12, mid, bottom - 22)   # dock it under everything
w = next(x for x in ui.windows() if "yc_errors" in x["name"]); assert w["docked"]
tx, ty = w["rect"][0] + 60, w["rect"][1] + 13            # its tab
inp(f"m:{tx},{ty}|w:4|D:{tx},{ty}|w:10")                 # double-click: it floats
# or: hand_drag(tx, ty, 700, 300); or: inp(f"m:{tx},{ty}|w:4|r:|w:6|k:DownArrow|w:2|k:UpArrow|w:2|k:Enter|w:10")
```

- The drop targets are at the edges of the dock space (top, left, right, bottom, 22 px in), not over the sheet:
  the sheets' window takes no window over it.
- `ui.windows()` gives each window's `rect` (the tab strip is its top 26 px when docked) and `docked`.

### Charts on the sheet: select, outline, Select Data

A chart is chosen with the mouse like any object, and its data is chosen on the sheet, as in Excel:

```python
objs = [i for i in ui.items("Workbook") if i["id"] == "##obj"]   # every object on the sheet, the last drawn last
x0, y0, x1, y1 = objs[-1]["rect"]
inp(f"m:{int(x1) - 6},{int(y0) + 6}|w:3|c:{int(x1) - 6},{int(y0) + 6}|w:5")   # select it (its chart area)
# its data is now outlined: names red, categories purple, values blue; the values outline has corner handles
r, t = ui.cell_rect("B13"), ui.cell_rect("B9")                    # drag the bottom right corner up to B9
inp(f"m:{int(r[2])},{int(r[3])}|w:6|d:{int(r[2])},{int(r[3])},{int(t[2])},{int(t[3])}|w:8")
ui.command("ChartSelectData")                                      # Select Data Source (also right-click > Select Data...)
# its "Chart data range" box is armed: a drag over the cells fills it and the chart follows; OK keeps it
a, b = ui.cell_rect("A1"), ui.cell_rect("C9")
inp(f"m:{int(a[0]) + 4},{int(a[1]) + 4}|w:4|d:{int(a[0]) + 4},{int(a[1]) + 4},{int(b[2]) - 4},{int(b[3]) - 4}|w:8")
ok = next(i for i in ui.items("Select Data Source") if i["label"] == "OK")
inp(f"c:{int((ok['rect'][0] + ok['rect'][2]) / 2)},{int((ok['rect'][1] + ok['rect'][3]) / 2)}|w:6")   # click OK
```

- An empty chart (Insert with no data selected) shows a `Select Data...` button in its middle (`ui.items()` label).
- The windows are `###yc_select_data` (Select Data Source), `###yc_edit_series`, `###yc_axis_labels`; they are not
  modal (`ui.state()["dialog"]` stays 0) so the sheet takes the clicks and drags; look for them in `ui.windows()`.
- Beside a selected chart: Excel's Chart Elements (+) and Chart Filters buttons, `id == "##fly"` in that order.
- Insert Chart / Change Chart Type is a dialog (`ui.state()["dialog"]`): kinds on the left by label (`"Pie"`,
  `"Combo"`), Up / Down move through them, Enter is OK.
- Double-click the chart title to type a new one in place (Enter keeps it, Escape leaves it).

## 5. Dialogs, message boxes, file dialogs

- Open a dialog the way the user does (`ui.command(id)`, its shortcut, or the ribbon). It opens with the keyboard
  in its first field, like Excel: `Tab` moves to the next control (text boxes, check boxes, option buttons), `Space`
  ticks a check box, `Enter` presses the default button (OK, Solve...) wherever the keyboard is, `Escape` cancels.
  In a dialog's list (Data Analysis tools, chart types) the arrows, Home / End and a letter (the next item starting
  with it) choose. `ui.state()["dialog"]` is the open dialog's number, 0 when none is open.
  ```python
  inp("g:B7|w:2"); ui.command("GoalSeek")            # Set cell is filled with B7
  inp("k:Tab|t:300|k:Tab|t:B5|k:Enter|w:10")         # To value, By changing cell, OK
  inp("k:Enter|w:6")                                 # the result: OK
  ui.command("DataAnalysis")                         # a regression, keys only
  inp("k:R|w:2|k:R|w:2|k:R|w:2|k:Enter|w:8")         # Random..., Rank..., Regression (w:2 between repeated keys)
  inp("t:$B$1:$B$121|k:Tab|t:$A$1:$A$121|k:Tab|k:Space|k:Enter|w:15")   # Y, X, Labels, OK: results on a new sheet
  ```
- Menus and drop-downs (ribbon galleries, font boxes, context menus, a filter's menu, a validation list): one `Escape`
  closes them, and shortcuts work again. `Alt+Down` opens a cell's validation list or a filtered header's menu;
  Up / Down and Enter choose. `Shift+F10` opens the cell's context menu at the active cell with its first item lit
  (`k:s-F10|w:4|k:DownArrow|k:Enter` runs Copy).
- Typing a formula: `ui.state()["edit"]` holds the text typed so far (absent when no cell is being edited), so a
  test can follow each key. Right after `=`, `+`, `(` or `,` the arrow keys point at cells (Shift extends the range),
  even while the full function list is showing; `F4` cycles the reference at the caret through `$A$1`, `A$1`, `$A1`,
  `A1`; `Tab` accepts the highlighted function; a missing `)` is added on Enter. `F2` switches Enter / Edit mode
  (arrows move the caret instead of pointing).
  ```python
  inp("g:C4|w:2|t:=|w:2|k:LeftArrow|w:2|k:LeftArrow|w:2|k:UpArrow|w:2")
  assert ui.state()["edit"]["text"] == "=A3"
  inp("k:F4|w:2|t:*2|k:Enter|w:4")                   # C4 is =$A$3*2
  ```
- Some dialogs keep their window for a second page (Goal Seek's and Solver's results): check the cells, not only
  whether the dialog number changed.
- A hidden YCGrid answers message boxes (MsgBox, confirmations, the kill switch's question) with their default button
  and **cancels file dialogs** (Open, Save As on a new workbook, Insert Picture, Export PDF, Replay...) at once; each
  is listed in `ui.errors()` with "answered automatically". Expected entries you caused on purpose are fine; any
  other entry is a bug. A change a protected sheet refuses opens no box in any instance: its message is an error in
  `ui.errors()` and `ui.state()["error_log"]["refusal"]` holds the red line shown beside the status bar's Error Log
  entry for six seconds (`"Protected sheet, not changed: Sheet1!B2"`). To go through a file dialog the way the user does, queue what it should choose first:
  `ui.answer_file_dialogs(path)` then click Replay (or Open, Insert Picture, Export PDF, Save Screenshot, Change
  Source, `GetOpenFilename`...); `""` is Cancel. File > New in a hidden instance opens another hidden window that
  ends with it (nothing appears on the user's screen).
- A request that YCGrid never answers (a dialog nobody can click, a dropped frame) raises `YcError` with
  `Code == "E_TIMEOUT"` after 120 s on a connection to a hidden instance (600 s otherwise; see
  `REQUEST_TIMEOUT` in object-model.md). Pass `timeout=` on long requests (`host.job`, screenshots) and keep a
  `faulthandler.dump_traceback_later(...)` guard in long test runs so a hang shows where it waited.
- Two clicks on the same cell from consecutive `ui.input` calls less than half a second apart are a double-click:
  the cell editor opens and takes the keys after it (as in Excel). Click another cell first, or `w:` between them.
- Tests that share one instance: delete what a test opens on top of the grid (containers, floating figures, panels) before it ends. A floating window over the cells takes the next test's `A:` clicks, which then select nothing and fail far from the cause.
- Close what a test's keys opened, too: Excel shortcuts open questions where a test doesn't expect them
  (Alt+Shift+Right on a cell is Group and asks Rows / Columns). Left open, it takes the next test's keys (a Ctrl+Z
  that undoes nothing). End such a test with `k:Escape` and check `ui.state()["dialog"] == 0` and
  `ui.state()["ribbon"]["popup_open"]` is False.
- A hidden YCGrid has a **clipboard of its own**: Copy / Cut / Paste (cells, text boxes, screenshots) work inside it
  and never replace what the user copied, so tests may use `k:c-C` / `k:c-V` freely. Nothing pasted from
  outside reaches it either: put test data in with Python, not through the system clipboard.

## 6. What to check, and what to look for in the screenshots

Checks in code:

- Cells: `.Value` for the number, **`.Text` for what is shown** (formats, `#####`).
- After Copy / Cut, `ui.state()["copy_mode"]` is True while the copied cells are outlined (Excel's CutCopyMode):
  Enter pastes them and ends it, Escape ends it.
- Undo / Redo show what they change: the sheet of the change becomes active and its cells are selected, so check
  `active_sheet` / `active_cell` after a Ctrl+Z like the user would look at them.
- `ui.state()`: `dialog`, `status`, `alarms`, `active_cell`, `selection`, `figures` (each figure's `docked`,
  `follow_paused`, `menu` and `plots`: each plot's `items` with `points`, `handles` with `label` and `x` / `y`,
  `view` = `[x0, x1, y0, y1]` drawn last frame), `bound_tables`.
- `Application.StatusBar`, your activity log, the orders / fills tables, `safety.status()["alarms"]`.
- `ui.errors()` empty at the end; `tools.yc_read_console()` for prints and tracebacks; `tools.yc_python_status()`
  for load errors and controls without a handler.

Read every screenshot for these (each one shipped once):

| Look for | Usually means | Fix |
|---|---|---|
| `#####` in a cell | the column is too narrow for the biggest value | widen it for the largest value it will hold (Greeks, P&L in the hundred thousands) |
| a label cut by the value next to it ("Underlyin") | text that can't spill into a filled cell | right-align the label (it spills left) or widen the column |
| a chart or figure over the table it explains | fixed positions | place it beside / under the data, or dock it; check at the user's window size |
| an empty plot at 0..1, or a live plot that never moves | data arrived after the first frame and nothing follows | `follow=` with `yfit="stable"` for live plots; `xlim` / `ylim` when a range grows |
| the newest candle cut off | an old YCGrid; current versions leave room | update YCGrid |
| raw floats (1520.6000000000001, 0.521561782828453) | no number format | `NumberFormat` per column: price ticks, `#,##0` for P&L, `0.0%` for IV |
| `1.76754e+09`, `8e+06` | numbers shown without formats | date formats for times, thousands separators |
| a meaningless axis title ("index") | a placeholder left in | name the axis (Underlying price, Strike, Time) |
| categories 1, 2, 3 instead of names | numbers passed as x | pass the names as labels |
| a count twice in a title ("Ticks (20,000,000) (20000000 rows)") | the window adds its own | let YCGrid add it |
| green up / red down on one chart and the opposite on another | two conventions | one convention (Taiwan: red up, green down; `plot.TRADING`, the `dark` theme) |
| columns that look out of line (Bid / Ask) | headers aligned differently from their numbers | header alignment = content alignment ([screens.md](screens.md) "Screen layout rules") |
| a cut word at the window's left edge ("TCUSDT") | text in a column that scrolls under a frozen or centered layout | move it to the center columns or the panel |
| the one number the user needs is gone (the underlying) | the rows holding it were hidden | put it above the center column, keep it visible |
| order buttons cut off at the bottom of a panel | tested at another window size | `size=` of the user's screen; primary actions first |
| `5.` or `1,250.` | `#,##0.####` on whole numbers | decimals from the tick size |

## 7. Trading behavior to test

- Every order button and chart action refuses beyond the limits with a reason in the status bar and the log.
- **Stop** stops the strategy **and cancels its working orders**; check the orders table right after Stop.
- The kill switch / Flatten leaves no working orders and a flat position; the stop orders go too.
- Fill prices are plausible: a marketable limit fills at the market (PaperGateway does this), resting limits at
  their price.
- Positions in your table, the gateway's (`gw.positions()`) and the sum of your fills agree.
- History (back-dated ticks) is drawn, never traded; nothing trades before the user starts it.
- Quotes stop: the heartbeat alarm turns on; they resume: it clears.
- Click-to-trade on a moving ladder: the price under the cursor changes when the ladder recenters; check that the
  order went at the price that was shown when clicked.

## 7A. Basic operations in every state

`scripts\basics.py` (in `check_parallel.py` as `basics:K/6`) is the net under the basic operations: each case starts a
fresh sheet with the same small table, puts YCGrid into a state the way a person gets there (`STATES`: editing,
pointing in a formula, read-only mode switched on mid-edit, merged cells, a filter, protection, grouped sheets, cut
mode, a table, a shape selected...), runs one operation with a person's keys or mouse (`OPS`), and checks:

| Check | How |
|---|---|
| no message Excel wouldn't show | every Error Log entry of the case matches `ALLOWED` (state, operation, text, reason) |
| Undo gives back what was there | `conn.request("debug.dump")` before, after the operation, after Ctrl+Z as many times as Undo steps were added: the dumps match line for line (`io::ContentDump`: cells, formats, rows, columns, merges, and a checksum per .ycg chunk) |
| Redo gives back what it made | Ctrl+Y as many times: the dump matches the one after the operation |
| no change without an Undo step | a changed dump with no new step fails (except `NO_UNDO`: what Excel can't undo either) |
| read-only mode changes nothing | the dump stays the same, whatever the keys |
| nothing left behind | after two Escapes: no `edit` in `ui.state()`, `dialog` 0, no popup |
| no ImGui check failed | `imgui_asserts` didn't grow, no shrinking or clipped windows |
| the sheet still takes typing | afterwards `probe` typed into Z50 lands there (refused only in read-only and protected states) |

```
<YCGrid>\runtime\python\python.exe -E -s scripts\basics.py --states read_only_typing,pointing --ops type_number,undo
<YCGrid>\runtime\python\python.exe -E -s scripts\basics.py --only filtered:paste_values      # one case
<YCGrid>\runtime\python\python.exe -E -s scripts\basics.py --list                            # the states and operations
```

A failure prints the case, what broke, the dump's difference and a screenshot under `%TEMP%\yc_basics\`. What goes wrong only
after several operations is found by `--sequences N --seed S` (in `check_parallel.py` as `basics:seqS`, S the seed:
`basics:seq1`, `basics:seq1001`): a state, eight
random operations, Undo back to the start and Redo to the end, each dump compared. A failing sequence prints its name
(`seq21:plain:spill_formula,insert_cells,...`); `--sequence plain:spill_formula,insert_cells,...` replays it, and
removing operations one at a time while it still fails gives the few steps to put in a test. Fix a bug in
a basic operation with its case: add the state or operation it needed, see it fail, fix, run the whole matrix.
`--commands` runs every command of the Home tab (as the ribbon lists it) in eleven basic states (`COMMAND_STATES`) with the same checks
(`--tab others`: every other tab's);
`--lang zh-TW` runs any of it in the Chinese UI. `debug.dump` is also the quickest check in a test of your own that an action and its Undo leave the workbook as it was.

The results of the operations themselves are compared with Excel by `python/tests/test_excel_basics.py`: cases in
`scripts/excel_basics_cases.py` (a start table, one object-model call), Excel's results in
`tests/data/excel_basics_measured.json` (measure new cases with `scripts\measure_excel_basics.py --only <id>`; it
starts its own hidden Excel). `KNOWN_DIFFERENCES` lists what still differs, each with its reason.

To look for differences nobody has written a case for, `scripts\excel_diff.py` runs a set of object-model calls (`--ops calls` or `reads`)
over ranges you choose (`--ranges "A1|B2:C3|B:B|A1:B2,D4:E5"`) in a state (`--state table`, `filter`, `merged`, `protected`,
`arrays`, `hidden`) in a hidden Excel of its own and in a hidden YCGrid, then `compare` lists the calls one side refused,
the cells only one side changed and the values read differently. It needs comtypes installed into a folder
(`pip install comtypes --target <dir>`, given with `--comtypes`). Each difference found is measured on its own before a fix,
and the fix gets a test with the measured values.

## 8. Smoke tests worth automating

- **Every feature, its result checked**: `scripts\feature_check.py [--only section,...] [--exe ...]` (YCGrid's own;
  run by `scripts\check_all.cmd`). The sweeps below only prove a command doesn't crash or hang; this one clicks each
  command where the user finds it (its tab, the menu or submenu it is in, its gallery, the dialog's buttons by name,
  the status bar chip) and checks what it did: cells, formats, files written, windows, the recording replaying. A
  command that has no check fails the run. A new command gets its check in `scripts\feature_sections.py` the same
  day; an app does the same for its own buttons and panels (`tests\features.py`).

- **Every command once**: `for c in ui.commands(): ui.command(c["id"])` on a sample workbook (skip Quit, Close,
  Open / Save As, Print, Install), checking after each that the process is alive, `ui.state()` answers, and
  `ui.errors()` holds nothing unexpected; press `Escape` when a dialog is left open. This found a crash in Insert >
  Pie.
- **Every dialog with Enter and with Escape**: open, `k:Enter`, check it closed or moved to its result; reopen,
  `k:Escape`, check it closed.
- **Every example's missing-packages flow** in a runtime without numpy and pandas (`scripts\example_packages.py`:
  Skip, a failed Install then Try Again, Always install, and the automatic install a later open does; needs the network).
- **Every example / screen built once** (its workbook `examples\<name>\<name>.ycg` opened in a hidden instance: `ui_sweep.py examples`), run for 20 s, screenshot, Error Log empty.
- **A hurried user, at random, for minutes**: YCGrid itself has `scripts\chaos.py --seed N --minutes M [--exe ...]
  [--lang zh-TW]` (cells, formulas good and bad, keys, drags, random ribbon buttons and context-menu items, every
  command, dialogs, undo storms, sheets, Python writing, figures, panes, docking, examples, save and reopen; run it on
  `dist_debug` too, its asserts catch more). An app gets its own (`tests\chaos.py SEED MINUTES` next to the app;
  `scripts\chaos.py` shows the pattern). A seeded `random.Random` picks weighted actions the way a trader works: clicks on the
  live table (also bursts of them), double / right clicks, keys on locked cells, panel buttons including order spam,
  odd values in text boxes (0, -1, `abc`, 1e9, empty), switching the instrument and expiry quickly, typing into input
  cells, Ctrl+Z / Ctrl+Y, chart input, sheet switching, reload, hot reload (touch a module), the kill switch, and
  window handling: dock YCGrid's windows at the edges and take them out again every way, pull at the app's locked
  windows (they stay). Leave out a kind of action and its bugs stay hidden: docking was missing once, and a window
  the user docked could not come out. After
  every action check: the process is alive, `ui.state()` answers within 2 s (a slower answer is a frozen screen),
  the status line still changes (stale data), no new Error Log entry that isn't an automatically answered question,
  no traceback in the console. Log each action with its time, so a seed replays a problem. A few thousand actions
  found a numpy warning from a calculator cell typed 0, an error that stayed in the status line after the
  connection came back, and a retry that ignored its back-off.
- **Checked at random**: random actions only find what crashes or freezes; a wrong value needs an answer to compare
  with. `scripts\calc_fuzz.py --mode recalc|undo|files --seed N --steps S` has three: `recalc` makes random Python edits
  (formulas over ranges, whole columns and rows, other sheets, OFFSET, ROW(), dynamic arrays; inserted and deleted
  rows, columns and cells; cut, copy, sheets copied and deleted) and after each one the values calculated step by
  step must equal `CalculateFull`'s; `undo` makes the same kinds of changes in the UI and Ctrl+Z / Ctrl+Y must give
  back exactly the cells before and after each step (`ui.state()["undo"]["made"]`, the steps ever made, tells which actions made a step; `depth` stays at 100 once
  the list is full:
  clearing empty cells makes one, a cancelled dialog doesn't), then save / reopen must give the same cells; `files` saves cells of every kind as `.ycg` and `.xlsx` and opens them again. Take out
  circular references as they appear (`Worksheet.CircularReference`), or every full recalculation changes them. It
  found readers of a range that a shifted band moved under (`=SUM(A11:H28)` when only A:C shift down), an undone
  `=SEQUENCE(1,3)` leaving its spilled values as typed ones, TRUE saved to `.xlsx` as FALSE, dynamic arrays coming back from `.xlsx` as #SPILL!, and the circular-reference note dropped by the next
  edit. A reader of a spilled value on another sheet kept a value calculated before the anchor spilled again
  after a shift (seed 3102). `scripts\formula_fuzz.py --seed N --n COUNT` checks single formulas: hundreds picked
  from templates (precedence, rounding, MOD, aggregates over mixed ranges, criteria, text and coercion, lookups,
  dates, dynamic arrays and lambdas) against answers worked out in Python the way Excel defines them; a mismatch
  names the formula, the answer expected and the answer given. It found MODE returning the wrong one of tied
  values. An app's own model (positions, P&L) gets the same: random orders and fills, compared with a slow, simple
  recomputation. `scripts\gaps_fuzz.py --seed N --rounds R [--only KIND,...] [--lang zh-TW]` does it for the
  object model's newer members: each round a random PivotTable, pane layout, text file, rounding format, Top N or
  threads setting, checked against answers worked out in Python; `--only KIND --seed S --rounds 1` replays a round.
  It found GETPIVOTDATA ignoring a Top 10, frozen panes numbered as split ones and Workbooks.Add writing into a
  YCGrid started meanwhile. Two runs side by side are a check of their own: they found that last one.
- **YCGrid's own checks in one command**: `scripts\check_all.cmd` (build, unit and Python tests, translations,
  command coverage, boundary, model fields, lessons, then side by side feature_check, basics, ui_sweep, calc_fuzz,
  formula_fuzz and doc_fuzz, then `scripts\trader_session.py`, and on the debug build the chaos seeds that once found
  a problem, listed in `scripts\chaos_seeds.txt`). Run it before every commit; `/quick` skips the long UI runs. The whole
  workflow for YCGrid's own developers (build, test modules, `check_parallel.py`, what a change brings with it) is in
  [developing-ycgrid.md](developing-ycgrid.md).
- **A trader's and a researcher's day, timed** (`scripts\trader_session.py` for YCGrid itself): build the live screens (quote board, ladder, order flow, candles) and
  the research examples, use each the way its user does (quick clicks on the ladder, dragging the order line, wheel
  and Live on a chart, fill-handle double-clicks, filters, sorts, PivotTables and Data View sorts on 100,000+ rows)
  and time every step. Watch the UI from **another process** (`ui.state` four times a second on its own connection):
  a thread in the test's own process waits for that process's GIL and reports stalls that aren't YCGrid's. Read
  `perf.stats` (`frame_cpu_max_ms` over the last frames) after each step: a frame over a few hundred milliseconds is
  work done on the UI thread (file reads and source scans belong on the request's thread or the engine thread).
  Close or dock the example charts before typing into a sheet: a floating figure over the cells takes the clicks.
- **Resources over time**: repeat reload, hot reload and instrument switches for some rounds and watch the working
  set, handles and threads of `ycgrid.exe` and its Python host (PowerShell `Get-Process`); they level off, they don't
  grow with every round.
- **Network down at start and back**: point the app's proxy at a closed port (`http://127.0.0.1:9`), start, click
  and press buttons (nothing may freeze; the reason shows), restore the setting and reload; then save, quit (it
  must exit within seconds while threads run) and reopen.

## 8A. Pictures that go into the repository

- The README pictures are made by tools, never by hand: `tools/make_readme_shots.py` (the quote board, the
  Immediate window and the PivotTable report in English and Traditional Chinese, and the gallery of example screens;
  `--only NAME` for one) and `tools/make_readme_gif.py` (the animation at the top: the market watch example live, in
  both languages; it needs Pillow, so install it to a folder with `pip install pillow --target <dir>` and run the
  bundled Python with `PYTHONPATH=<dir>` and `-s`). Run them again after a change to YCGrid's look (colors, icons,
  the ribbon).
- A screenshot must not show a personal path, a user name or the folder layout of your machine. YCGrid's messages
  show the user's folders as `%TEMP%`, `%LOCALAPPDATA%`, `%APPDATA%`, `%USERPROFILE%` (`core::AbbreviatePath`; the
  Immediate window prints the folder of its Python host), and `make_readme_shots.py` stops when the console still
  holds the profile folder. A message that prints a path goes through the same function. `scripts\check_boundary.py`
  reads text only, so look at every picture before committing it.
- Hidden instances start with the ribbon shown (unless `YCGRID_SETTINGS` gives other settings); when
  `ui.state()["ribbon"]["collapsed"]` is True, click its arrow (`ui.state()["ribbon"]["arrow"]`) to show it. Then move the mouse to the status bar so no tooltip or chart crosshair lands in the picture. A 2000 x 1221 window
  scaled down to 1600 wide matches the README's pictures.

## 8B. Excel's object model, member by member

`scripts\check_excel_parity.py` (run by `tests.test_core` and check_all) compares every member Excel 16's type library
lists for the objects of `schema/excel_parity.toml` with YCGrid's Python classes: a member is YCGrid's, not planned
(with the reason) or a gap (with its priority), and `--write` regenerates `docs/excel-object-model.md`. A member you
add takes its line off the toml (the check fails until then); a new object goes into `[objects]` and its members are
measured with `scripts\measure_excel_object_model.py --comtypes <dir>` (it reads EXCEL.EXE, starting no Excel). How
a member behaves is measured in a hidden Excel of the probe's own (AGENTS.md, Tests and checks), and its test says
"measured" beside each value (`python/tests/test_live_gaps.py`). `debug.func_help` reads what Insert Function shows
for a function, an @udf's MacroOptions included.

## 9. When YCGrid itself misbehaves

- A crash ends `yc.launch`'s process (`p.poll()` is not None): reduce it to the smallest script (open, a few cells,
  one command) and report it with that script. The Error Log's files are in `ui.errors_folder()` (by default
  `%LOCALAPPDATA%\ycgrid\logs`; `""` in a hidden instance, which writes none unless `YCGRID_ERROR_LOG=<folder>` is
  set) and so are the crash / hang dumps of a normal instance; a hidden one writes its dumps to `%TEMP%\ycgrid-dumps`.
- A freeze: `ui.state()` times out with "The UI is not responding"; a hang dump is written to the same folder.
- A stutter (a step that takes seconds, a frame that takes hundreds of milliseconds): `perf.stats` gives each frame's
  work (`conn.request("perf.stats", {"last": n})["frame_cpu_max_ms"]` over the step's frames), and
  `scripts\ui_sampler.py` says where the time goes. Sample around the step, in a hidden instance of your own:

  ```python
  import ui_sampler                              # scripts\ on sys.path
  run = ui_sampler.start(proc.pid, 6.0)          # the UI thread; thread="yc-engine" for the engine
  conn.request("ui.input", {"script": "g:A1:F1000001|w:2|k:c-C|w:6"})
  ui_sampler.report(run.result())                # functions by share of samples, and the commonest stacks
  ```

  A UI thread waiting in `std::future<...>::get` under `Engine::Call` is behind the engine: sample `yc-engine` next.
  Report what you find with the step and the sampler's stacks.
- After a test run, no hidden `ycgrid.exe` may be left (`tasklist /FI "IMAGENAME eq ycgrid.exe"`): terminate every
  `yc.launch` process in `finally`. Instances left over from a crashed run are listed by `yc.find_instances()` (their
  `file` is in a temporary folder); ask the user before closing any instance you didn't start in this run.
- Something only C++ can fix (drawing, a dialog, a crash) is reported, not worked around in Python
  ([app-development.md](app-development.md)).
