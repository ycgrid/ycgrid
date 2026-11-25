# Events and controls reference

## 1. How to write event handlers

- **Where they go**:
  - Workbook events go in `main.py` (the project entry point, = Excel's ThisWorkbook).
  - Sheet events and the events of controls on that sheet (button Click, combo box Change) go in the module under `sheets/` named after the sheet's **CodeName**, e.g. `sheets/Sheet1.py`.
  - Per-cell OnChange (§7) can be registered in any module, usually in `Workbook_Open` of `main.py`.
- **The name is the binding**: the function name must match the table below exactly (same case), e.g. `Worksheet_Change`, `CommandButton1_Click`. There is no other registration step.
- **Parameters**:
  - Names and order as in Excel. `Target` is a Range, `Sh` is a Worksheet.
  - `Cancel`: Excel's `Cancel = True` is written one of two ways (Python can't rebind the caller's variable, so a bare
    `Cancel = True` only changes your local name and cancels nothing):

    | Excel | YCGrid Python |
    |---|---|
    | `Cancel = True` | `Cancel.Value = True` (Cancel is a `ByRef`; `bool(Cancel)` reads it) |
    | `Cancel = True: Exit Sub` | `return True` (any Before* handler; returning anything else, or nothing, doesn't cancel) |

    ```python
    def Worksheet_BeforeDoubleClick(Target, Cancel):
        if Target.Column == 1:
            return True            # = Cancel = True: no edit mode
    ```
  - `Application.EnableEvents = False` turns every event off (handlers aren't called; a Before* event goes on as not cancelled).
- **Threading**: every event, control event and per-cell OnChange runs in order on the host's macro thread; two handlers never run at the same time.
- **Be fast**: handlers should finish within 50 ms.
  - No `time.sleep`, no network I/O, no `Application.Wait`.
  - Put long work on a background thread, or schedule it with `Application.OnTime`.
- **Prevent recursion**: set `Application.EnableEvents = False` before writing cells in a handler, and set it back to True in `finally`.
  - The flag is recorded with every write at the time of the write, so setting it back to True doesn't fire events for earlier writes.
- **Exceptions**: exceptions raised by handlers are shown in the console (with file and line) and the status bar, and don't affect later events.
- **No nesting**: events fired by writes inside a handler run after the current handler finishes. Excel recurses immediately; YCGrid doesn't.

```python
# sheets/Sheet1.py
from yc.excel import *
Me = SheetModule(__name__)
Range, Cells, Rows, Columns = Me.Range, Me.Cells, Me.Rows, Me.Columns

def Worksheet_Change(Target):
    hit = Intersect(Target, Range("C2:C200"))
    if hit is None:
        return
    Application.EnableEvents = False
    try:
        for c in hit:
            c.Offset(0, 1).Value = Now()
    finally:
        Application.EnableEvents = True
```

## 2. Workbook events (main.py)

Every event below fires (checked by driving a hidden YCGrid in `python/tests/test_round2_events.py`). A sheet event
runs the sheet module's `Worksheet_*` handler first, then main.py's `Workbook_Sheet*` one with the sheet as `Sh`, as in
Excel. `yc_python_status` lists the handlers that run, and apart (`events_never_fired`) a `Workbook_*` / `Worksheet_*`
function that never runs: a typo, an Excel event YCGrid doesn't have, or one in the wrong module.

**Before\* events and Cancel**: YCGrid asks the host and acts on the answer without the window waiting: the double-click
edit, the context menu, the save or the close happens a few milliseconds later, unless a handler set
`Cancel.Value = True` or returned True. Only the sheets whose module has the handler (or every sheet when main.py has the
`Workbook_Sheet*` one) are asked; elsewhere the action happens at once. A handler that doesn't answer within 2 s counts as not cancelled (YCGrid goes on and says so in
the console), so a hung handler can't stop the user. With no handler for the event nothing is asked and nothing waits.

| Event | When it fires | Notes |
|---|---|---|
| `Workbook_Open()` | after the host connects and finishes loading; also after reloads and host restarts | initialization: start market data, subscriptions, set OnTime / OnKey |
| `Workbook_BeforeClose(Cancel)` | the user closes the window (before the save prompt, as in Excel) | `Cancel.Value = True` keeps it open; no answer within 2 s counts as not cancelled. `Workbook.Close`, opening another workbook or a new one also run it, but can't cancel there |
| `Workbook_BeforeSave(SaveAsUI, Cancel)` | before Save / Save As (Ctrl+S, F12, File), a script's or an MCP tool's save, the project's own `ThisWorkbook.Save()` / `SaveAs` | `SaveAsUI` is True when the Save As dialog will show (Save As, or a workbook never saved); `Cancel.Value = True`: nothing is saved |
| `Workbook_AfterSave(Success)` | after the file was written (.ycg, the .xlsx copy, the sheet's .csv) | `Success` is False when writing failed |
| `Workbook_NewSheet(Sh)` | after a sheet is added (Insert Worksheet, Shift+F11, `Worksheets.Add`, a PivotTable or a report put on a new sheet) | not for copies (as in Excel) |
| `Workbook_SheetActivate(Sh)`, `Workbook_SheetDeactivate(Sh)` | switching sheets (tabs, Ctrl+PageUp / PageDown, `Activate`, a new sheet) | the old sheet's Deactivate first |
| `Workbook_SheetChange(Sh, Target)` | Change on any sheet | fires after that sheet's `Worksheet_Change` |
| `Workbook_SheetSelectionChange(Sh, Target)` | the selection changes on any sheet | after that sheet's `Worksheet_SelectionChange` |
| `Workbook_SheetCalculate(Sh)` | after a sheet's formulas were calculated | after that sheet's `Worksheet_Calculate` |
| `Workbook_SheetBeforeDoubleClick(Sh, Target, Cancel)` | a cell is double-clicked, before editing starts | either handler can cancel |
| `Workbook_SheetBeforeRightClick(Sh, Target, Cancel)` | a cell is right-clicked, before the context menu appears | either handler can cancel |
| `Workbook_SheetFollowHyperlink(Sh, Target)` | after a hyperlink in a cell is followed | `Target` is the `Hyperlink` |
| [YC] `Workbook_SheetLiveChange(Sh, Target)` | any sheet changes because of market data or recalculation | after that sheet's `Worksheet_LiveChange` |
| [YC] `Workbook_AfterReload(Files)` | after saved code was reloaded in place (Files: project-relative paths, "modules/App.py"); not after a host restart, which runs `Workbook_Open` | re-check what `Workbook_Open` decides once, e.g. rebuild the screen when `UI_VERSION` changed. Constants edited in the source already have their new values |
| [YC] `Workbook_HostShutdown()` | before the host exits, restarts or reloads (also when YCGrid is gone) | close connections, stop the background threads you started, save state; cut off after 2 s. `Workbook_BeforeClose` runs only when the workbook really closes, before it |

## 3. Sheet events (sheet modules)

| Event | When it fires | Notes |
|---|---|---|
| `Worksheet_Activate()`, `Worksheet_Deactivate()` | switching to / away from this sheet | no `SelectionChange` for the selection the sheet comes back with (Excel) |
| `Worksheet_Change(Target)` | cell contents are changed (see §5) | recalculation results and live market data do **not** fire it |
| `Worksheet_SelectionChange(Target)` | the selection changes: a click, keys, the Name Box, Go To, Python's `Select` | once per change; a drag reports the final range when the button is released; nothing while a formula is typed (pointing at cells). As in Excel, Enter / Tab moving the active cell inside a selection fire nothing (the selection is the same; read `ActiveCell` in a timer or `Application.OnKey` if you need it) |
| `Worksheet_Calculate()` | after this sheet's formulas were calculated (a change they depend on, F9, volatile functions, live data) | at most once per publish cycle. As in Excel, a write made with `EnableEvents = False` is calculated during the write and fires no Calculate, so a handler that writes cells other formulas depend on doesn't run again |
| `Worksheet_BeforeDoubleClick(Target, Cancel)` | a cell is double-clicked (`Target` is the cell, or its merged area) | `Cancel.Value = True`: no edit mode, no PivotTable details |
| `Worksheet_BeforeRightClick(Target, Cancel)` | a cell is right-clicked (`Target` is the selection, after the click moved it) | `Cancel.Value = True`: no context menu. Row / column headers don't fire it. `yc.interact.on_click(range, fn, buttons=("right",))` is the [YC] alternative |
| `Worksheet_FollowHyperlink(Target)` | after a hyperlink in a cell of this sheet is followed (a click, Ctrl+click, Open Hyperlink) | `Target` is the `Hyperlink` (`Target.Range`, `Address`, `SubAddress`); `=HYPERLINK()` formulas don't fire it (Excel) |
| [YC] `Worksheet_LiveChange(Target)` | this sheet changes because of market data or recalculation | see below |

How `Worksheet_LiveChange` behaves:

- Defining it (or main.py's `Workbook_SheetLiveChange`) is enough: YCGrid then reports calculation and live-data changes to the host.
- Fires at most once per publish cycle. `Target` is the cells whose value changed in this cycle (a multi-area Range).
- `Target` holds every changed cell of the sheet: intersect it with what you watch (`Intersect(Target, Range("B2:K1000"))`) first. (`Me.LiveWatchRange`, which would limit it in YCGrid, is planned, not available yet.)
- While a handler is still running, the changes of later cycles are merged into one delivery and never pile up.
- For large data use `Target.ToNumpy()` or read the watched range as an array; don't iterate cell by cell.

## 4. Application level: OnTime, OnKey, Run

```python
# modules/Module1.py
from yc.excel import *

def StartTimer():
    global next_tick
    next_tick = Now() + TimeValue("00:00:05")
    Application.OnTime(next_tick, "Module1.Tick")

def Tick():
    Range("A1").Value = Now()
    StartTimer()                                              # reschedule, as is customary in Excel

def StopTimer():
    Application.OnTime(next_tick, "Module1.Tick", Schedule=False)  # cancel: time and name must match

def SetupKeys():
    Application.OnKey("^+r", "Module1.RefreshAll")           # Ctrl+Shift+R
```

- `Procedure` is a macro name string, as in Excel. [YC] A function object can be passed directly too.
- `OnKey` key syntax as in Excel:
  - modifiers: `^` is Ctrl, `+` is Shift, `%` is Alt.
  - special keys: `{F1}`...`{F15}`, `{ENTER}`, `{TAB}`, `{DEL}`, `{UP}`, `{DOWN}`, `{LEFT}`, `{RIGHT}`, `{HOME}`, `{END}`, `{PGUP}`, `{PGDN}`, `{ESC}`.
  - `Procedure=""` disables the key, `None` restores the default behavior.
- `Application.Run("Module1.Foo", 1, 2)` runs a macro and returns its return value.

## 5. Event trigger matrix

| Source | Change | Calculate | LiveChange | Other |
|---|---|---|---|---|
| user input, paste, fill, clear, delete contents, undo | ✓ | if formulas are affected | | per-cell OnChange (`ycUser`) |
| object model writes (any Python client, MCP tools) | ✓ (not when `EnableEvents=False`) | if formulas are affected | | per-cell OnChange (`ycPython`) |
| inserting or deleting rows, columns, cells | ✓ (Target is the affected range) | depends | | |
| formula recalculation results | | ✓ | ✓ (cells whose value changed) | per-cell OnChange (`ycCalc`, merged per cycle) |
| live market data (Topics, `=QUOTE()`) | | if formulas are affected | ✓ | per-cell OnChange (`ycLive`, merged per cycle) |
| clicking a button | | | | `<button name>_Click` |
| combo box selection | ✓ for the `LinkedCell` if any | | | `<name>_Change` (before the LinkedCell's Change) |
| panel controls | ✓ for the `LinkedCell` if any | | | `OnAction` |
| formatting changes | | | | no events at all (like Excel) |

`Worksheet_Calculate` fires for any calculation of the sheet's formulas, also when no value changed; LiveChange only
for cells whose value changed.

## 6. Controls (buttons, combo boxes)

Controls are drawn onto the sheet in Design Mode and can float anywhere. Clicking a button calls `<name>_Click()` in the module of its sheet; changing a combo box selection calls `<name>_Change()`.

### 6.1 Writing Click

```python
# sheets/Sheet1.py (CodeName=Sheet1, sheet name "Panel")
from yc.excel import *
import Feed                                    # standard module modules/Feed.py
Me = SheetModule(__name__)
Range, Cells, Rows, Columns = Me.Range, Me.Cells, Me.Rows, Me.Columns

def btnStart_Click():                          # button btnStart
    Feed.start(Range("B1").Value)              # B1: symbol; Click can call any Python function
    Me.btnStart.Enabled = False
    Me.btnStop.Enabled = True
    Application.StatusBar = "Subscribed to market data"

def btnStop_Click():                           # button btnStop
    Feed.stop()
    Me.btnStart.Enabled = True
    Me.btnStop.Enabled = False
    Application.StatusBar = False
```

- Function name = `<button name>_Click`, in the module of the button's sheet. No parameters.
- Button names must be valid Python identifiers (Unicode letters allowed), e.g. `btnStart`, `CommandButton1`, `PlaceOrder`.
- Standard modules (e.g. `modules/Feed.py`) can be imported with `import Feed`; never import sheet modules (use the CodeName globals such as `Sheet1`).

### 6.2 Button properties

| Member | Description |
|---|---|
| `Name` | name; after renaming, rename `<old name>_Click` too |
| `Caption` | text on the button |
| `Enabled`, `Visible` | enabled, visible |
| `BackColor`, `ForeColor` | BGR integers (`RGB()`) |
| `Font` | `Name`, `Size`, `Bold`, `Italic` |
| `WordWrap` | wrap the caption |

Position and size live on the OLEObject / Shape: `Left`, `Top`, `Width`, `Height` (in points), `Placement` (`xlMoveAndSize`, `xlMove`, `xlFreeFloating`, [YC] `ycFloatWindow` = fixed to the window, doesn't move when scrolling), `BringToFront()`, `SendToBack()`, `ZOrderPosition`, `Delete()`, `Select()` (`TopLeftCell` / `BottomRightCell` are planned, not available yet: place controls by cells as in §6.3).

Getting a button: `Me.btnStart`, `Sheet1.btnStart`, `Me.OLEObjects("btnStart").Object`; list them all: `for s in Me.Shapes:`.

### 6.3 Creating controls in code (the Excel way works as is)

```python
r = Me.Range("B2:C3")
ole = Me.OLEObjects.Add(ClassType="Forms.CommandButton.1", Left=r.Left, Top=r.Top, Width=r.Width, Height=r.Height)
ole.Name = "btnRefresh"
ole.Object.Caption = "Refresh"
# Click goes in this module: def btnRefresh_Click(): ...
```

- In AI workflows prefer the `yc_add_control` tool for creating controls: it returns the actual name and an event function template (like every Python change, it is not a step of the user's undo list).
- Only write creation code into a `.py` when controls must be created dynamically at run time.

### 6.4 Combo boxes (ComboBox)

```python
# sheets/Sheet1.py
def cbAccount_Change():                        # the selection of combo box cbAccount changed
    acct = Me.cbAccount.Value                  # .ListIndex works too (0-based; -1 = nothing selected)
    Range("B1").Value = acct
```

| Member | Description |
|---|---|
| `Value`, `Text`, `ListIndex`, `ListCount` | current value, displayed text, selected index, number of items |
| `List`, `AddItem(Item, Index=None)`, `RemoveItem(Index)`, `Clear()` | manage the list in code |
| `ListFillRange` | the list's source range (e.g. `"Settings!A2:A20"`); read straight from the cells, not through Python, and updated automatically when the range changes |
| `LinkedCell` | two-way bound cell: the selection is written to the cell (firing its Change and per-cell OnChange), and changes of the cell update the selection |
| `Style` | `fmStyleDropDownCombo` (editable) or `fmStyleDropDownList` (select only) |
| `ListRows` | number of rows in the drop-down |

Events: `<name>_Change()` (selection or input changed), `<name>_Click()` (picked from the list), `<name>_DropButtonClick()` (before the list opens; a good place to refresh the list).

```python
r = Me.Range("E2:F2")
ole = Me.OLEObjects.Add(ClassType="Forms.ComboBox.1", Left=r.Left, Top=r.Top, Width=r.Width, Height=r.Height)
ole.Name = "cbAccount"
ole.ListFillRange = "Settings!A2:A20"
ole.LinkedCell = "Settings!C1"
ole.Placement = ycFloatWindow                 # [YC] doesn't move when scrolling
```

### 6.4a Check boxes, option and toggle buttons, text boxes, labels, list boxes, spin buttons, scroll bars, images

Developer -> Insert lists every control; `OLEObjects.Add("Forms.CheckBox.1", ...)` (also `Forms.OptionButton.1`, `Forms.ToggleButton.1`,
`Forms.TextBox.1`, `Forms.Label.1`, `Forms.ListBox.1`, `Forms.SpinButton.1`, `Forms.ScrollBar.1`, `Forms.Image.1`) adds them from Python.
With a `LinkedCell` the value goes into the cell (TRUE / FALSE for check boxes, option and toggle buttons, the number for spin buttons and
scroll bars, the text otherwise), and a change of the cell updates the control.

| Control | Properties | Events |
|---|---|---|
| CheckBox, ToggleButton | `Value` (True / False), `Caption`, `LinkedCell`, `WordWrap` | `_Click`, `_Change` |
| OptionButton | `Value`, `Caption`, `GroupName` (one choice per group; empty = the sheet), `LinkedCell` | `_Click`, `_Change` |
| TextBox | `Value` / `Text`, `LinkedCell`, `MultiLine`, `PasswordChar`, `WordWrap` | `_Change` (when typing ends), `_GotFocus`, `_LostFocus` |
| Label | `Caption`, `WordWrap`, `BackColor`, `ForeColor` | `_Click` |
| ListBox | `List`, `ListFillRange`, `ListIndex`, `Value`, `LinkedCell`, `MultiSelect` (`fmMultiSelectSingle`, `fmMultiSelectMulti`, `fmMultiSelectExtended`), `Selected(i)` | `_Click`, `_Change` |
| SpinButton | `Value`, `Min`, `Max`, `SmallChange`, `Orientation`, `LinkedCell` | `_SpinUp`, `_SpinDown`, `_Change` |
| ScrollBar | `Value`, `Min`, `Max`, `SmallChange`, `LargeChange`, `Orientation`, `LinkedCell` | `_Scroll`, `_Change` |
| Image | `Picture` (`LoadPicture(path)`), `PictureSizeMode` (`fmPictureSizeModeClip`, `Stretch`, `Zoom`) | `_Click` |

```python
ws = Worksheets("Order")
ws.OLEObjects.Add("Forms.OptionButton.1", Left=10, Top=10, Name="optBuy", Caption="Buy", GroupName="side", Value=True)
ws.OLEObjects.Add("Forms.OptionButton.1", Left=10, Top=30, Name="optSell", Caption="Sell", GroupName="side")
ws.OLEObjects.Add("Forms.SpinButton.1", Left=120, Top=10, Name="spnLots", Min=1, Max=20, LinkedCell="$C$2")

def spnLots_Change():                 # in sheets/<CodeName>.py
    Range("C3").Value = Me.spnLots.Value * 1000
```

### 6.5 Design Mode (pointers for AI)

- Add controls with `yc_add_control` (`kind`=button / combobox / checkbox / optionbutton / togglebutton / textbox / label / listbox / spinbutton / scrollbar / image; `cover_range` makes it exactly cover some cells); move, resize, change properties or delete with `yc_modify_control`.
- After adding, write the event function (`<name>_Click` or `<name>_Change`) in the sheet module, then `yc_reload_python`.
  - Confirm with `yc_python_status` that no control has a missing event function.
  - Finally test with `yc_trigger_control` (buttons, check boxes, option and toggle buttons `event=click`; combo boxes, list boxes, text boxes, spin buttons and scroll bars `event=change` with a `value`).
- Renaming doesn't rename functions: after renaming, update the event functions in the `.py` file.
- A person double-clicking a control in Design Mode (or its menu's View Code, or Developer > View Code with it selected) gets its default event's function appended to its sheet module when it isn't there (`def CommandButton1_Click():` / `pass`; `_Change` for combo boxes, text boxes, spin buttons and scroll bars), and VS Code opens at it. If you work in the same file, re-read it first so you don't overwrite what the user or the UI appended.
- If a control may send orders or change positions, get the user's consent before testing, or make sure it is connected to a simulated environment.

### 6.6 Not supported yet

- Frames, multi-page and tab strip controls (use several UserForms, or a custom panel).
- Form controls (`Shapes.AddFormControl`, the `Buttons` collection) are supported: see object-model.md (Shapes); a form
  button's `OnAction` names the macro it runs.

## 7. Per-cell OnChange [YC]

When only changes of certain cells matter, use per-cell OnChange instead of checking `Intersect` in `Worksheet_Change`: it is more precise and its cost only grows with the actual changes.

```python
# main.py
def OnPrice(Target):                                   # Target: the cells that actually changed (a multi-area Range for several cells)
    for c in Target:
        Debug.Print(c.Address, c.Value)

def Workbook_Open():
    Range("Quotes!B2:B100").OnChange = OnPrice              # or the string "main.OnPrice"
    Range("Quotes!H2:H100").OnChange = "Module1.OnTime"     # one range can have several handlers

# sheets/Sheet1.py
@Me.OnChange("C2:C100", Sources=ycUser | ycPython | ycLive)
def OnPriceChange(Target):
    ...

# any module: the sources too, e.g. the user's edits only (the code's own writes don't call it)
Range("Trade!B14:F25").SetOnChange(OnZoneEdit, Sources=ycUser)

@Application.OnChange("Trade!G5:G11", Sources=ycUser)
def OnParamEdit(Target):
    ...
```

| Item | Rule |
|---|---|
| trigger sources `Sources` | `ycUser` (user edits, paste, fill, clear, undo), `ycPython` (object model writes), `ycCalc` (formula recalculation results), `ycLive` (live market data); default `ycUser \| ycPython`, the same as `Worksheet_Change` |
| merging | with `ycCalc` or `ycLive` it fires at most once per publish cycle; `Target` is the union of the cycle's changes |
| order | per-cell OnChange (in registration order) -> `Worksheet_Change` -> `Workbook_SheetChange` |
| `EnableEvents = False` | blocks it too |
| removing | `Range(...).OnChange = None` (or `SetOnChange(None)`) |
| writes from a running macro | the events of cells a host macro writes are queued and run after the macro returns; the macro never waits for them |
| saving | registrations aren't saved: create them in `Workbook_Open`; they are rebuilt automatically after reloads |
| latency | <= 5 ms from the user pressing Enter to the handler starting (when the host is idle) |

## 8. Events of panels, charts and windows [YC]

- Custom panel controls name their handler with `OnAction` (`"module.function"` or a callable); with a `LinkedCell`, value changes are written to the cell as well.
- Users panning and zooming a chart (with `Chart.Interactive = True`) fire no events; read `Axes(xlCategory).MinimumScale` / `MaximumScale` when needed.
- Moving and docking floating windows and containers fire no events; the layout is saved with the workbook (`Application.Layout`).
