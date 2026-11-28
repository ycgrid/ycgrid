# YCGrid Python object model reference (Excel compatible)

> Written by hand and checked against `python/yc` (the classes are the truth). When unsure whether a member exists, look
> it up with `yc_api_lookup` (it reports whether the member exists in the installed `yc` package); `E_UNSUPPORTED` at
> run time means it isn't implemented yet. Members marked (P2) are planned and **not there yet**: don't use them.
> `Application.<name>` that is neither a member nor a worksheet function raises `AttributeError`, and so does setting a
> member `Application` doesn't have (a typo or a planned member fails on its line, as in Excel).

**Markers**

- no marker: available in v1.
- [YC]: YCGrid extension, not in Excel.
- (P2) / (P3): comes in a later version.
- read-only: can only be read, not set.

**Naming rules**

- Names, parameter names and constant values are the same as Excel's, always PascalCase: `Range("A1").Value`, `Cells(2, 3)`, `Borders(xlEdgeBottom)`.
- Collections and coordinates are 1-based, with call syntax: `Worksheets(1)`, `rng.Areas(2)`.
- Arguments can be passed by position or by name, with Excel's names: `rng.Copy(Destination=Range("D1"))`.
- Excel methods without arguments need parentheses in Python: `rng.ClearContents()`, `rng.Select()`.
- The parameterized property `Address` works both ways: `rng.Address` (without parentheses) or `rng.Address(False, False)`.
- Only the Python-specific constructs (`@udf`, `@stream`, `yc.connect`, the [YC] modules) use lowercase names and snake_case parameters; so do the porting helpers' parameters (§3) and `Series.SetData` / `AppendData` (`x`, `y`), which are usually passed by position.

---

## 1. Imports

```python
from yc.excel import *
```

The import includes: every global object and function (§2, §3), every xl / vb / mso constant (§17), `udf`, `stream`, `SheetModule` and the exception classes.

Sheet CodeNames (e.g. `Sheet1`) don't need importing: the host injects them as globals before running a module (§6).

**Standalone scripts** (data feed processes, Jupyter, tests) are not loaded by the host and must connect first:

```python
import yc
from yc.excel import *
yc.connect()          # yc.connect(target=None, timeout=5.0): target is a pid or the program folder; when omitted: $YCGRID_PID, else the ycgrid of this folder (or the only instance)
Worksheets("Quotes").Range("A1").Value = "Connected"
```

- After connecting, global objects such as `Application` and `Worksheets` work on that connection (one per process).
- Standalone scripts have the controller role: they can read and write the workbook, push market data and `Application.Run`; they can't define events, UDFs or streams and have no CodeName globals.
- Standalone scripts have no macro thread; writes are sent automatically about every 1 ms, without waiting for a session to end.

**Starting YCGrid from a script, headless checks** [YC] (tests, CI, agents):

```python
import yc
from yc import ui
proc = yc.launch("book.ycg", hidden=True)   # starts ycgrid.exe (yc.find_exe(): $YCGRID_EXE or the YCGrid folder) and connects
st = ui.state()                              # sheets, active sheet / cell / selection and value, open dialog, status,
                                             # operator mode, alarms, bound tables, figures, replay, Python host
ui.screenshot("check.png", target="window")  # also "grid", or target="range", address="A1:H20"; works when hidden
proc.terminate()
```

`ycgrid.exe --hidden` runs without a window and keeps no AutoRecover copies; every window stays inside the main window
(so screenshots show figures and dialogs too).

- `yc.launch(file=None, hidden=False, exe=None, lang=None, timeout=30.0, size=None, automation_security=1)`: `file` is optional (a new
  workbook); `lang="en"` or `"zh-TW"` sets the UI language (default: the user's), which also names new sheets
  ("Sheet1" / "工作表1", so look sheets up by index or `ActiveSheet` in tests); `size=(width, height)` in pixels.
  Returns a `subprocess.Popen`: call `terminate()` at the end. A hidden instance also ends when the process that
  launched it ends (`--parent-pid`), so a script that stops early never leaves an instance nobody can see or close.
- Every request waits at most `yc._conn.REQUEST_TIMEOUT` seconds for YCGrid's reply (600; env `YCGRID_REQUEST_TIMEOUT`,
  the old name `YC_REQUEST_TIMEOUT` still works; 120 for a connection to a hidden instance, made by
  `yc.launch(hidden=True)` or `yc.connect`, where nobody can answer a dialog) and then raises
  `YcError` with `Code == "E_TIMEOUT"`; `conn.request(method, params, timeout=...)` sets it per call (`<= 0`: wait
  for ever) and `conn.default_timeout` per connection. A script therefore never hangs on a reply that doesn't come.
- `yc.find_exe()`: `$YCGRID_EXE`, else the `ycgrid.exe` installed with the package (`yc/_bin`, pip) or the YCGrid folder
  this `python\yc` belongs to (`<YCGrid>\python\yc` -> `<YCGrid>\ycgrid.exe`).
- `yc.ui.ping()`: True when the UI thread answers; it doesn't wait for the engine's queue like `state()` does (which
  shows this connection's writes), so a slow answer is a frozen screen, not a busy engine.
- `yc.ui.state()` keys: `pid`, `visible`, `file`, `title` (the title bar: `Book1 [Group] - YCGrid` while sheets are grouped), `dialog` (0 = none), `status`, `status_stats` (the selection statistics on the status bar: `Average: 6.5    Count: 3    Sum: 13`), `circular_status` (the status bar's "Circular References: B2", `""` when none), `copy_mode` (copied cells outlined, Excel's CutCopyMode), `interactive`, `operator_mode`,
  `imgui_asserts` (ImGui checks that failed; a debug build stops on the first), `table_headings` (the table whose column names the column headings show, its header row scrolled out of view; "" for the letters), `titlebar_clickable` (the title bar areas Windows lets a real mouse click, in `ui.items` coordinates: a button outside them only drags the window), `alarms` (list of text), `dirty`, `undo` / `redo` (`depth`: steps on the list, `label`: the next one; `undo.made`: steps ever made, which grows when the list is at its limit of 100 and the depth stays), `sheets` (list of `id`, `name`, `visible`, `used` (A1 range or ""), `tables`,
  `charts`), `active_sheet`, `active_cell`, `selection`, `active_value`, `active_text`, `edit` (only while a cell is being typed in: `cell`, `text`, `caret`,
  `formula_bar`, `mode` "enter" / "edit"), `bound_tables` (list of
  `name`, `rows`, `record`, `stale_after`), `figures` (list of `id`, `key`, `title`, `owner`, `alive`, `docked`, `follow_paused`;
  `dock_root`, `menu` (its context menu's items) and `sync_cursor` when set; `plots`:
  list of `title`, `synced_x`, `handles` (`id`, `kind`, `label`, `x`, `y`, a band's `y1`, `closable`), `items` (list of `kind`, `label`, `points`) and `view` (`[x0, x1, y0, y1]` drawn last
  frame)), `ribbon` (`collapsed`, `peek`, `tab`, `backstage` (File open), `popup_open` (any menu or drop-down open), `tabs` with the pixel centers of their labels, `arrow`), `replay` (`mode`
  "live" / "replay" / "paused" / "ended",
  `recording`), `python` (`running`, `ready`).

---

## 2. Global objects and functions

| Name | Description |
|---|---|
| `Application` | the application object (§4); every object has it too (`ws.Application`, `rng.Application`), as in Excel |
| `ThisWorkbook`, `ActiveWorkbook` | the current workbook (§5); one YCGrid instance has one workbook, so both are the same |
| `ActiveSheet`, `ActiveCell`, `Selection`, `ActiveWindow` | the current value on every access (keep one with `ws = Application.ActiveSheet`: `ws = ActiveSheet` follows later activations). `Selection` may be a Range, or the button selected in Design Mode |
| `Workbooks`, `Worksheets`, `Sheets`, `Names` | collections |
| `Range(Cell1, Cell2=None)` | a range on the active sheet; in sheet modules the template makes it refer to that sheet (§6) |
| `Cells(RowIndex, ColumnIndex)`, `Cells` | cells; `ColumnIndex` may be a number or a column name (`Cells(5, "B")`). `Cells` without arguments is every cell |
| `Rows`, `Rows(Index)`, `Columns`, `Columns(Index)` | entire rows and columns: `Rows(5)`, `Columns("A:C")`, `Rows.Count` |
| `Intersect(*Ranges)` | intersection; `None` when there is none (Excel's Nothing) |
| `Union(*Ranges)` | union (a multi-area Range), tidied as Excel's: an area inside another adds nothing (`Union(A1:A3, A2)` is `$A$1:$A$3`), areas that make one rectangle together join, also when they overlap (`Union(A1, A2)` is `$A$1:$A$2`, `Union(A1:B2, B1:C2)` `$A$1:$C$2`), others stay as they are (`Union(A1:B2, B2:C3)` counts B2 twice); a repeated area, or the area holding the new one, moves to the end (`Union(A1, C3, A1)` is `$C$3,$A$1`, `Union(C4:J7, A3:F9, D4:G4)` `$A$3:$F$9,$C$4:$J$7`) |
| `Evaluate(Name)` | evaluate a formula or resolve a name, like `Application.Evaluate`; an array result is a `VArray` |
| `WorksheetFunction` | worksheet functions (§13) |
| `MsgBox(Prompt, Buttons=vbOKOnly, Title=None)` | message box, returns `vbOK`, `vbYes`, etc. |
| `InputBox(Prompt, Title=None, Default="")` | input box, returns a string; `""` when cancelled |
| `RGB(Red, Green, Blue)` | returns a BGR integer color, like Excel |
| `Debug.Print(*Items)` | output to the Immediate window console |
| `DoEvents()` | flushes the write buffer and lets the UI update (a synchronization point) |
| `CVErr(Number)`, `IsError(v)` | create / test error values, e.g. `CVErr(xlErrNA)` |
| `IsEmpty(v)`, `IsNumeric(v)`, `IsDate(v)`, `IsArray(v)` | type tests with Excel semantics (`IsNumeric(None)` is True) |
| `Now()`, `Date()`, `Time()`, `Timer()` | `Now()` returns a `datetime`; `Date()` returns today at 00:00 as a `datetime`; `Time()` returns a `timedelta` since midnight; `Timer()` returns seconds since midnight. So `Now() + TimeValue("00:00:05")` and `Date() + TimeValue("13:30")` can be added directly |
| `Empty`, `Nothing` | both equal `None`, for literal ports |
| `vbCrLf`, `vbLf`, `vbCr`, `vbTab`, `vbNullString` | string constants |
| `vbObjectError` | -2147221504, the base of a program's own error numbers (`YcError("...", Number=vbObjectError + 1)`) |

---

## 3. Macro porting helpers

These functions make literal ports behave like Excel. New code can use native Python instead. Pass their arguments by
position: the names below are Excel's, while the parameters are lowercase Python names (`Mid(s, start, length=None)`,
`Split(expression, delimiter=" ", limit=-1)`, `Join(items, delimiter=" ")`, `UBound(arr, dimension=1)`,
`Replace(expression, find, replace, start=1, count=-1)`, `InStrRev(s1, s2, start=-1)`, `StrComp(a, b, compare=0)`,
`Weekday(d, firstdayofweek=1)`, `Format(expression, fmt="")`), so `Mid(String="abc", Start=2)` raises `TypeError`.

| Category | Functions | Differences from Python |
|---|---|---|
| strings | `Left`, `Right`, `Mid(String, Start, Length=None)`, `Len`, `InStr([Start,] String1, String2)`, `InStrRev`, `Replace`, `Trim`, `LTrim`, `RTrim`, `UCase`, `LCase`, `StrComp`, `StrReverse`, `Space`, `String(Number, Character)`, `Chr`, `ChrW`, `Asc`, `AscW` | positions are 1-based; `InStr` returns 0 when not found |
| splitting | `Split(Expression, Delimiter=" ")`, `Join(List, Delimiter=" ")` | `Split` returns a 0-based list, like Excel |
| matching | `Like(String, Pattern)` | Excel's `Like` operator (`*`, `?`, `#`, `[A-Z]`) |
| conversion | `CStr`, `CDbl`, `CLng`, `CInt`, `CBool`, `CDate`, `CSng`, `Val` | `CDbl(None)` is 0; `CInt` and `CLng` use banker's rounding; unconvertible values raise `TypeMismatchError` (13) |
| math | `Int`, `Fix`, `Round`, `Abs`, `Sgn`, `Sqr`, `Log`, `Exp`, `Rnd`, `Mod(a, b)` | the sign of `Mod` follows the dividend (Excel's rule, unlike Python's `%`); `Round` uses banker's rounding |
| dates | `DateSerial`, `TimeSerial`, `DateValue`, `TimeValue`, `DateAdd(Interval, Number, Date)`, `DateDiff(Interval, Date1, Date2)`, `Year`, `Month`, `Day`, `Hour`, `Minute`, `Second`, `Weekday`, `Format(Expression, Format)` | `DateSerial` and `DateValue` return `datetime`; `TimeSerial` and `TimeValue` return `timedelta`; `Format` uses YCGrid's number format engine (like `TEXT()`) |
| arrays | `Array(*Items)`, `UBound(Arr, Dimension=1)`, `LBound(Arr, Dimension=1)` | `Array()` is 0-based; a `VArray` read from a Range is 1-based |
| other | `IIf(Expression, TruePart, FalsePart)`, `Environ(Name)`, `Beep()` | `IIf` evaluates both branches first (like Excel) |

---

## 3A. Values, types and errors

| Cell contents | `.Value` | `.Value2` |
|---|---|---|
| number | `float` | `float` |
| number with a date format | `datetime.datetime` | serial `float` |
| text | `str` | `str` |
| boolean | `bool` | `bool` |
| blank | `None` (= Empty) | `None` |
| error | `XlError` (test with `IsError`) | same |
| multi-cell range | `VArray` (see below) | `VArray` |

**VArray**: the 2D array read from a Range, like Excel's Variant array.

- `arr(i, j)`: 1-based call syntax, like Excel.
- `arr[i][j]`: 0-based indexing, like Python.
- `UBound(arr, 1)` is the number of rows, `UBound(arr, 2)` the number of columns; `arr.Rows` and `arr.Columns` work too.
- `for row in arr:` iterates row by row; every row is a 0-based list.
- `arr.ToNumpy(dtype="float64", Empty=float("nan"))` [YC] converts to a numpy array.
- Single-row or single-column ranges still read as 2D (like Excel).

**Writing**

- A scalar fills the whole range.
- Lists of lists, `VArray` and 2D numpy arrays are written by shape; 1D sequences are written as one row (like Excel).
  As in Excel, a one-row array is repeated down a taller range and a one-column array across a wider one
  (`Range("A1:A3").Value = [1, 2, 3]` writes 1, 1, 1: write `[[1], [2], [3]]` for a column); cells past a larger
  array get `#N/A`.
  Any other iterable (a generator, `range`, a pandas column) is read like a list; a mix of rows and scalars
  (`[[1, 2], 3]`) raises `ApplicationDefinedError` instead of writing text.
- Addresses and moves outside the sheet raise `ApplicationDefinedError`, as in Excel: `Range("A0")`, `Rows(0)`,
  `Columns(0)`, `Offset(-1, 0)` from row 1, `Resize(0)`, and `Offset`, `Resize` or `Cells` past the last row
  (1,048,576, or the sheet's `RowLimit`) or column; none of them quietly means row or column 1.
- Writing `None` clears the contents.
- `datetime` writes a serial; a cell in General format gets a date format automatically.

**Colors**

- Colors are BGR integers, like Excel: `RGB(255, 0, 0)` equals 255 (red).
- [YC] Setters also accept `"#RRGGBB"` strings; reading always returns an integer.
- `ColorIndex` uses Excel's 56-color palette; `xlNone` and `xlColorIndexNone` mean no color.

**MsoTriState**: properties such as `Shape.Visible` return `msoTrue` (-1) or `msoFalse` (0), like Excel. Setters accept `True` / `False` too.

**Range comparison** [YC]: `rng1 == rng2` compares sheet and address. `is` compares object identity, and every Range you get is a new object, so never compare two Ranges with `is`.

**Exceptions**

| Class | Excel error number | When |
|---|---|---|
| `YcError` | — | base class of every YCGrid error. Attributes: `Number`, `Description`, `Source`, `Code` (YCP error code) |
| `SubscriptOutOfRangeError` | 9 | not found in a collection, e.g. `Worksheets("Missing")`; also a subclass of `KeyError` and `IndexError` |
| `TypeMismatchError` | 13 | type mismatch, e.g. `CDbl("abc")`; also a subclass of `TypeError` |
| `ApplicationDefinedError` | 1004 | application-defined error, e.g. `SpecialCells` found no cells, protected, invalid argument |
| `FormulaError` | 1004 | formula syntax error; the `Position` attribute is the character position |
| `ProtectedError` | 1004 | a change refused by sheet or workbook protection (`E_PROTECTED`; only the request whose own change was refused gets it, not one running while the user is refused), or a wrong password (`E_PASSWORD`); a subclass of `ApplicationDefinedError` |
| `UnsupportedError` | 438 | member not implemented yet (`E_UNSUPPORTED`); the message includes the planned milestone |
| `YcConnectionError` | — | the connection to ycgrid.exe was lost |

---

## 4. Application

`Application.Sum(...)`, `Application.VLookup(...)`: any worksheet function, its errors returned as `XlError` (use
`WorksheetFunction.X` to have them raised). Any other unknown name raises `AttributeError`, read or set.

`ByRef(Value=False)` [YC]: the `Cancel` argument of the Before* events (`Cancel.Value = True` cancels; `bool(Cancel)`
reads it), see events-and-controls.md.

**Properties**

| Member | Description |
|---|---|
| `ActiveCell`, `ActiveSheet`, `ActiveWindow`, `ActiveWorkbook`, `Selection`, `ThisWorkbook` | read-only |
| `Workbooks`, `Worksheets`, `Sheets`, `Names`, `WorksheetFunction` | read-only |
| `Caller`, `ThisCell` | read-only. In a UDF, the calling cell (Range); `ThisCell` is None anywhere else. A function that reads either is called for each cell (others once for every cell of the same arguments) |
| `Calculation` | `xlCalculationAutomatic`, `xlCalculationSemiautomatic` (Automatic except for data tables: the data tables wait for F9), `xlCalculationManual` (File -> Options and the ribbon show it, as Excel's do; set from code it isn't saved as the user's default). Saved with the workbook, and opening a workbook sets it, as in Excel: a file saved in Manual opens in Manual |
| `Iteration`, `MaxIterations`, `MaxChange` | iterative calculation of circular references (defaults False, 100, 0.001); saved with the workbook, and opening a workbook sets them, as in Excel |
| `Watches` | the Watch Window (Book, Sheet, Name, Cell, Value, Formula; its Add Watch... dialog): `Add(Source)`, `Count`, `Item(i)` / `Watches(i)` (`.Source`, `.Delete()`), `Delete()` (all); a watch follows its cell when rows / columns are inserted or deleted, as Excel's |
| `ReferenceStyle` | `xlA1` or `xlR1C1`: how the UI shows and takes formulas (saved for this computer); `Range.Formula` stays A1-style either way |
| `AutoCorrect` | `AddReplacement(What, Replacement)`, `DeleteReplacement(What)`, `ReplacementList(Index=None)`: words replaced when they are typed into a cell (Excel's built-in list, plus what Spelling > AutoCorrect adds; saved for this computer); `ReplaceText` (False turns the replacements off) |
| `AutomationSecurity` | how a workbook opened by a script (`Workbooks.Open`, `yc.launch(file)`, the MCP tools) treats its Python code, as Excel's: `msoAutomationSecurityLow` (1, the default: it runs), `msoAutomationSecurityByUI` (2: the Trust Center decides, as for a workbook a person opens; untrusted code stays disabled and the Message Bar offers Enable Content, also in a hidden YCGrid) or `msoAutomationSecurityForceDisable` (3: it never runs, without a message). Not saved; `yc.launch(..., automation_security=)` sets it for a new YCGrid. Another value raises |
| `ProtectedViewWindows` | the Protected View windows: a workbook a person opened from the Internet (the Mark of the Web), a browser cache or an Outlook attachment folder is read-only (writes raise `ProtectedError`) with its Python code off. `Count`, `Item(i)` -> `ProtectedViewWindow` (`Caption`, `SourceName`, `SourcePath`, `Workbook`, `Edit()` = Enable Editing, returns the Workbook, `Close()`). `Workbooks.Open` from a script never opens in Protected View, as in Excel |
| [YC] `TrustCenter` | File -> Options -> Trust Center Settings (Excel keeps these in the registry, with no object model). `MacroSettings` (`ycMacroDisableWithNotification` (default), `ycMacroDisableWithoutNotification`, `ycMacroEnableAll`), `MessageBar`, `TrustedLocations` (list of `{Path, Subfolders, Description, DateModified}`), `AddTrustedLocation(Path, Subfolders=False, Description="")` (the folder must exist; a network path needs `AllowNetworkLocations = True`), `RemoveTrustedLocation(Path)`, `AllowNetworkLocations`, `DisableTrustedLocations`, `TrustedDocuments` (the project folders trusted with Enable Content), `AllowNetworkDocuments`, `DisableTrustedDocuments`, `ClearTrustedDocuments()`, `ProtectedViewInternet`, `ProtectedViewUnsafeLocations`, `ProtectedViewOutlook`. Saved for this computer; a hidden YCGrid starts from the defaults and keeps changes in memory. `yc.ui.state()["trust"]` gives the open workbook's state: `code` (`none` / `enabled` / `disabled`), `bar`, `reason` (`trusted_location`, `trusted_document`, `session` (enabled while it stays open), `created` (made by YCGrid in this session), `enable_all`, `automation`, `automation_force_disable`, `disabled`, `disabled_without_notification`) |
| `CellDragAndDrop` | False: dragging the selection's border no longer moves cells and the fill handle is gone (File -> Options; saved for this computer: an app that turns it off sets the user's value back in `Workbook_BeforeClose`). A protected sheet never offers them |
| [YC] `TruncateTo15Digits` | File -> Options -> Calculation: True turns the digits of a typed number past the 15th into 0, as Excel does; False (the default) keeps it exactly as a Decimal. Saved for this computer |
| `CalculationState` | read-only: `xlDone` (0) when nothing waits, `xlPending` (2) when formulas wait to be calculated (manual calculation, or `Range.Dirty`); a recalculation finishes inside the request that started it, so never `xlCalculating` |
| `CutCopyMode` | `xlCopy` / `xlCut` while copied cells are outlined (Paste or Enter pastes them), else `False`; set to `False` to end it, as Esc does (Esc, typing in a cell, an insert or delete end it too, and Paste then has nothing of those cells to paste) |
| `ShowQuickAnalysis`, `AlertBeforeOverwriting` | the Quick Analysis button on a selection; the question before dropped cells replace data (File -> Options; saved for this computer) |
| `StandardFont`, `StandardFontSize`, `SheetsInNewWorkbook` | what a new workbook starts with (File -> Options -> General) |
| `MoveAfterReturn`, `MoveAfterReturnDirection` | Enter moves the selection after an entry (Shift+Enter: the other way), `xlDown` / `xlToRight` / `xlUp` / `xlToLeft` (File -> Options; saved for this computer) |
| `EditDirectlyInCell` | False: F2 and a double-click edit in the formula bar (File -> Options) |
| `EnableAutoComplete` | False: typing the start of a text already in the column no longer finishes it (File -> Options) |
| `Cursor` | `xlDefault`, `xlWait`, `xlNorthwestArrow`, `xlIBeam`: the mouse pointer over YCGrid while a macro works; it stays until set back or Python restarts, as in Excel |
| `DisplayAlerts` | whether confirmation prompts are shown (e.g. when deleting sheets) |
| `EnableEvents` | whether events fire; recorded with every write at the time of the write |
| `Iteration`, `MaxIterations`, `MaxChange` | iterative calculation settings |
| `Name`, `Version`, `Path` | read-only; `Name` is "YCGrid"; `Path` is the program folder of ycgrid.exe (the whole folder in portable distributions); base file paths in code on it |
| `ScreenUpdating` | when False, changes of this session are not shown step by step |
| `UserName` | the author name of new threaded comments (read/write; defaults to the Windows user name) |
| `AddCustomList(ListArray, ByRow=False)`, `CustomListCount`, `GetCustomListContents(ListNum)`, `GetCustomListNum(ListArray)`, `DeleteCustomList(ListNum)` | custom lists for AutoFill and Sort (saved in settings.json for every workbook; the built-in lists come first and can't be deleted) |
| `AutoRecover` | `Enabled`, `Time` (minutes), `Path` |
| `CommandBars` | `ExecuteMso(idMso)`, `GetEnabledMso`, `GetPressedMso`, `GetLabelMso`; `CommandBars("Cell")` (also `"Row"`, `"Column"`, `"Ply"` for the sheet tabs) -> CommandBar: `Controls.Add(Type=msoControlButton, Id=None, Parameter=None, Before=None, Temporary=True)` (`msoControlPopup` = a submenu with its own `Controls`), `Controls.Count`, `Controls(index or caption)`, `FindControl(Type, Id, Tag)`, `Reset()`; a control has `Caption`, `OnAction` (a macro name like `"Module1.run"`), `Tag`, `Parameter`, `BeginGroup`, `Enabled`, `Visible`, `Delete()`. Added commands appear at the bottom of that context menu for the session. `CommandBars.ActionControl` is the command whose macro is running |
| `Dialogs(xlDialog...)` | `.Show()` opens a built-in dialog and waits until it closes: `xlDialogOpen`, `xlDialogSaveAs`, `xlDialogPageSetup`, `xlDialogPrint`, `xlDialogPrintPreview`, `xlDialogFormatNumber`, `xlDialogAlignment`, `xlDialogFormatFont`, `xlDialogFontProperties`, `xlDialogBorder`, `xlDialogPatterns`, `xlDialogCellProtection`, `xlDialogStyle`, `xlDialogSort`, `xlDialogRowHeight`, `xlDialogColumnWidth`, `xlDialogInsert`, `xlDialogEditDelete`, `xlDialogPasteSpecial`, `xlDialogDefineName`, `xlDialogCreateNames`, `xlDialogPasteNames`, `xlDialogNameManager`, `xlDialogFormulaGoto`, `xlDialogFormulaFind`, `xlDialogFormulaReplace`, `xlDialogFunctionWizard`, `xlDialogDataValidation`, `xlDialogConditionalFormatting`, `xlDialogInsertHyperlink`, `xlDialogGoalSeek`, `xlDialogConsolidate`, `xlDialogZoom`, `xlDialogProtectDocument`, `xlDialogNew` (arguments are ignored; returns True) |
| `CheckSpelling(Word, CustomDictionary=None, IgnoreUppercase=False)` | True when the word or text is spelled correctly (the Windows spell checker, US English); [YC] `GetSpellingSuggestions(Word)` lists suggestions. In the UI: Review -> Spelling (F7); words added there go to YCGrid's own `custom.dic` next to `settings.json` |
| `Undo()`, [YC] `Redo()` | the user's last action (Ctrl+Z / Ctrl+Y). [YC] Undo covers the user's changes only: Python, live tables and topics write without being undone and without clearing the user's list (Excel clears it after a macro), so Ctrl+Z keeps working in a live app and puts back just the cells, formats and rows / columns of that change. Python inserting / deleting rows or columns, sorting or adding / removing sheets still clears it (positions moved). Like Excel, an undo or redo activates the sheet of the change and selects its cells |
| `Repeat()` | runs the user's last formatting, insert / delete, fill or clear command again on the selection (F4) |
| `DisplayFormulaBar` | show or hide the formula bar |
| `FormulaBarHeight` | the formula bar's height in lines (1 = collapsed; the chevron or Ctrl+Shift+U expands it to 3; saved for this computer) |
| `DisplayFormulaAutoComplete`, `DisplayFunctionToolTips` | the list of functions and names shown while a formula is typed, and the argument hint (File -> Options -> Formulas; saved) |
| `ErrorCheckingOptions` | background error checking (the green triangles), File -> Options -> Formulas: `BackgroundChecking`, `EvaluateToError`, `TextDate`, `NumberAsText`, `InconsistentFormula`, `OmittedCells`, `UnlockedFormulaCells`, `EmptyCellReferences` (off by default, as in Excel), `ListDataValidation`, `InconsistentTableFormula`, `MisleadingNumberFormats` (read / write, saved for this computer); `IndicatorColorIndex` reads `xlColorIndexAutomatic` |
| [YC] `RibbonDisplay` | `ycRibbonAlwaysShow` (0), `ycRibbonTabsOnly` (1), `ycRibbonAutoHide` (2): Ribbon Display Options |
| `GetOpenFilename(FileFilter=None, FilterIndex=1, Title=None, ButtonText=None, MultiSelect=False)` | the Open dialog without opening: a path, a list of paths with MultiSelect, or `False` when cancelled; FileFilter as in Excel (`"CSV (*.csv),*.csv,All Files (*.*),*.*"`) |
| `GetSaveAsFilename(InitialFilename=None, FileFilter=None, FilterIndex=1, Title=None)` | the Save As dialog without saving: a path or `False` |
| `StatusBar` | status bar text; set to `False` to restore it |
| `Interactive` | `False` blocks the user's keyboard and mouse input (dialogs from Python still work); back to `True` when the Python host restarts |
| `Caption` | the title bar, `"<window caption> - YCGrid"`; setting it replaces the application's part (`"Desk"` gives `"Book1 - Desk"`), None puts YCGrid back (Excel 16's rule, measured) |
| `WindowState`, `Left`, `Top`, `Width`, `Height` | the YCGrid window: `xlMaximized` / `xlMinimized` / `xlNormal`, its place and size in points (a hidden instance records the state and never shows its window) |
| `UsableWidth`, `UsableHeight` | the workspace under the ribbon and formula bar, in points |
| `DisplayFullScreen` | the window maximized without the ribbon, the formula bar and the status bar (`DisplayFormulaBar` / `DisplayStatusBar` read False meanwhile; False brings them back) |
| `DisplayStatusBar` | the status bar |
| `Ready` | False while a cell is edited or a dialog is open |
| `EnableCancelKey` | what Ctrl+Break does to the code running: `xlInterrupt` (stops it; the Error Log says so), `xlErrorHandler` (raises `CodeInterruptedError`, number 18, where the code was), `xlDisabled`; back to `xlInterrupt` when the code returns, as Excel at idle. Esc keeps its UI meaning |
| `CalculationInterruptKey`, `CheckAbort(KeepAbort=None)` | `xlAnyKey` (default) / `xlEscKey` / `xlNoKey`: the key that stops a calculation running for a while (the UI never waits for one; what it didn't reach stays pending and the next calculation or F9 goes on); `CheckAbort` stops it from code, KeepAbort is calculated all the same |
| `MultiThreadedCalculation` | `Enabled`, `ThreadMode` (`xlThreadModeAutomatic` / `Manual`), `ThreadCount` (File > Options > Formulas; saved for this computer) |
| `CalculateBeforeSave`, `FormatStaleValues` | File > Options > Formulas: a manual-calculation workbook is calculated when saved; its waiting cells show struck through |
| `EnableLargeOperationAlert`, `LargeOperationCellThousandCount` | the question before a person's paste or Fill Down / Right over more than that many thousand cells (33554, Excel's default; scripts are never asked; off in a hidden instance) |
| `ShowChartTipNames`, `ShowChartTipValues` | the tip over a chart: the element's name, a point's value |
| `GenerateGetPivotData` | pointing at a PivotTable value while typing a formula writes `GETPIVOTDATA(...)` |
| `ExtendList` | a value typed under a list takes the rows' format and formulas (Excel's Extend data range formats and formulas; on by default; typing only) |
| `AutoFormatAsYouTypeReplaceHyperlinks` | a web or e-mail address typed into a cell becomes a link with the Hyperlink style, in an Undo step of its own (on by default; typing only) |
| `GenerateTableRefs` | pointing at a table's column, row or header while typing a formula writes `Table1[Px]`, `Table1[@Qty]` (1, `xlGenerateTableRefsStruct`; 0 for plain cells) |
| `International(Index)` | the separators and orders YCGrid reads and writes (US Excel's: `"."`, `","`, month first), by Excel's `xlApplicationInternational` indexes 1 to 45 |
| `FileDialog(Type)` | `msoFileDialogOpen`, `SaveAs`, `FilePicker`, `FolderPicker`: `Title`, `InitialFileName`, `AllowMultiSelect`, `Filters.Add`, `ButtonName`; `Show()` returns -1 when chosen (`SelectedItems`), 0 when cancelled; `Execute()` opens or saves the choice |
| `MacroOptions(Macro, Description, Category, ArgumentDescriptions, HasShortcutKey, ShortcutKey)` | an @udf's description, category and argument descriptions in Insert Function and the argument hints (without it: the signature, the docstring's first line, User Defined); a shortcut runs the macro as `OnKey` |

**Methods**

| Member | Description |
|---|---|
| `Calculate()`, `CalculateFull()`, `CalculateFullRebuild()` | like F9, Ctrl+Alt+F9, Ctrl+Alt+Shift+F9 |
| `Evaluate(Name)` | evaluate a formula string, or resolve names and addresses |
| `Goto(Reference, Scroll=False)` | go to a range |
| `InputBox(Prompt, Title, Default, Left, Top, HelpFile, HelpContextID, Type)` | `Type=8` lets the user pick a range and returns a Range; returns `False` when cancelled |
| `Intersect(*Args)`, `Union(*Args)` | same as the global functions |
| `OnKey(Key, Procedure=None)` | run a macro on a shortcut; `Procedure=""` disables the key, `None` restores the default |
| `OnTime(EarliestTime, Procedure, LatestTime=None, Schedule=True)` | schedule a macro; `Schedule=False` cancels (time and name must match) |
| `Quit()` | close YCGrid (fires BeforeClose) |
| `Run(Macro, *Args)` | run a macro or public function and return its return value |
| `Volatile(Volatile=True)` | called inside a UDF, marks it volatile |
| `Wait(Time)` | wait until the given time; blocks the macro thread, avoid it in event handlers |
| `Application.<worksheet function>(...)` | e.g. `Application.VLookup(...)`: **returns** the error value on errors; the `WorksheetFunction` version raises |

**[YC] extensions**

| Member | Description |
|---|---|
| `PublishInterval` | [YC] minimum ms between screen updates caused by Python / live data (0-5000, 0 = every change; default from File -> Options); this session only. Typing and clicking always update at once |
| `DesignMode` | Design Mode switch |
| `Topics` | the real-time market data topic hub (§16) |
| `Sync(Wait="ui")` | a synchronization point: `"ui"` returns once the UI thread has handled what this connection sent before (view state: scrolling, selection, zoom); `"engine"` once the engine has applied the writes (what `DoEvents()` does) |

Planned, not available yet (P2): `CalcPolicy`, `StrictErrors` (formula syntax errors already raise on the line that
writes them), `UndoGroup(Name)` (Python's writes aren't undoable; see `Undo()`), `RunAsync(coro)` (start your own
asyncio loop on a thread and stop it in `Workbook_HostShutdown`), `HostInfo` (`yc_python_status` shows the host).

---

## 5. Workbooks, Workbook

`Workbooks.OpenText(Filename, Origin, StartRow, DataType, TextQualifier, ConsecutiveDelimiter, Tab, Semicolon, Comma, Space, Other, OtherChar, FieldInfo, DecimalSeparator, ThousandsSeparator, TrailingMinusNumbers)`: a text file as a new workbook, split as Text to Columns does with the same arguments (`FieldInfo=[[1, xlTextFormat]]` keeps 0050 as text). Origin is a code page (65001, 950, `xlWindows`); by default a byte order mark or UTF-8, else the system's. The options apply whatever the extension; the sheet is named after the file.

`Workbook.Date1904` (the 1904 date system: the cells keep their numbers and show dates 1462 days later; Python's datetimes are the same either way), `PrecisionAsDisplayed` (every number rounded for good to what its format shows, and every number written or calculated after; General, dates and fractions keep theirs), `ForceFullCalculation`: saved with the workbook, as in Excel.

| Member | Description |
|---|---|
| `Workbooks.Count`, `Workbooks(Index or name)`, `Workbooks.Item(Index)`, `for wb in Workbooks` | the open workbooks: this one first, then the other YCGrid windows (each YCGrid window holds one workbook, like Excel since 2013) |
| `Workbooks.Open(Filename, ..., Password=None)` | opens a `.ycg`, `.xlsx` / `.xlsm` or `.csv` (Excel and CSV files open as a new, untitled workbook): an empty Book1 is replaced, otherwise it opens in a new YCGrid window and the returned Workbook works with it; an open file returns its Workbook; `Password` opens an encrypted `.ycg` (password-protected Excel files and old `.xls` files don't open) |
| `Workbooks.Add()` | a new blank workbook (in a new YCGrid window when this one is in use) |
| `RefreshAll()` | refresh every PivotTable of the workbook |
| `Charts` | the workbook's chart sheets (§16A), as the global `Charts` and `Application.Charts` |
| another window's Workbook | `Name`, `FullName`, `Path`, `Saved`, `Worksheets` / `Sheets` (Worksheet, Range: values, formulas, formats, Cells, Offset, iteration...), `ActiveSheet`, `Save()`, `SaveAs()`, `Close(SaveChanges)`, `Activate()`. Objects remember their workbook: `wb.Worksheets(1).Range("A1:C9").Copy(ThisWorkbook.Worksheets(1).Range("E1"))` copies values and formulas across (relative references follow; formats stay). Tables, charts, PivotTables and other objects of another window aren't reachable this way |
| `Name`, `FullName`, `Path` | read-only |
| `Saved` | whether there are no unsaved changes; can be set to True |
| `ReadOnly` | read-only |
| `DisplayDrawingObjects` | `xlDisplayShapes`, or `xlHide` to hide every shape, picture, chart and control (Ctrl+6) |
| `CodeName` | read-only, always "ThisWorkbook" |
| `Worksheets`, `Sheets`, `Names`, `ActiveSheet` | read-only (the windows are the global `Windows`; `Workbook.Windows` is P2) |
| `CoAuthoring`, `MultiUserEditing`, `UserStatus`, `ExclusiveAccess()` | answered for one local user (a workbook is never shared): `False`, `False`, `((Application.UserName, now, 1),)`; `ExclusiveAccess()` does nothing |
| a member it doesn't have | setting one raises `AttributeError` (`ActiveWorkbook.PrecisionAsDisplayed = True`), as `Application` does: a misspelled or planned member is never kept silently |
| `Save()` | save |
| `SaveAs(Filename, FileFormat=None, Password=None)` | save as; the format follows the extension (`.ycg`; `.csv` and `.xlsx` save a copy and keep the working file) or `FileFormat` (`xlOpenXMLWorkbook`, `xlCSV`); `.xlsx` keeps values, formulas and formatting (not charts, controls, PivotTables or the Python project); `Password` encrypts a `.ycg` (`""` removes the encryption); `WriteResPassword` is not supported |
| `Password`, `HasPassword` | write `Password` to encrypt the workbook from its next save (`""` removes it; reading gives `"********"`); `HasPassword`: whether it is encrypted |
| `SaveCopyAs(Filename)` | save a copy (`.ycg`; an `.xlsx` or `.csv` name writes a copy in that format, as Save As does) |
| `LinkSources(Type=xlExcelLinks)` | the sources of formulas such as `=[Book2.ycg]Sheet1!A1` or `='D:\data\[Prices.xlsx]Rates'!B2` (a tuple of paths, or None). Values are read from `.ycg`, `.xlsx` and `.csv` sources when the workbook opens and whenever a source is saved; the last values are kept in the workbook |
| `UpdateLink(Name=None)` | read the values again from one source (path or file name), a list of them, or all |
| `ChangeLink(Name, NewName)` | point the formulas that use one source at another file |
| `BreakLink(Name)` | the formulas that use the source become their values |
| `LinkInfo(Name)` | [YC] dict: `path`, `updated` (datetime or None), `error` ("" = fine) |
| `Close(SaveChanges=None)` | close the workbook (`True` saves first, `False` discards changes; omitted with unsaved changes: YCGrid asks "Save changes?" after the call returns, and a hidden instance (`--hidden`) refuses with an error instead). This window's workbook is replaced by a new blank one and YCGrid stays open, like Excel after its last workbook closes; another window's workbook closes that window; `Application.Quit()` closes YCGrid |
| `Protect(Password=None, Structure=True, Windows=False)`, `Unprotect(Password=None)`, `ProtectStructure`, `ProtectWindows` | workbook protection: `Structure`: sheets can't be added, deleted, renamed, moved or hidden; `Windows`: the user can't move, undock, dock into, resize or close the workbook's windows (panels, figures, containers, floating sheets), drag the splitters next to them, tear a sheet tab out or Reset Layout. YCGrid's own windows (Error Log, Immediate...) stay free: one the user docked always comes out again. Every docked window shows its tab; the user floats an unlocked one by dragging the tab off, double-clicking it or its right-click menu > Float. Python still docks, floats, sizes and resets them (`Application.Layout.Reset()`). One window only: [YC] `Panel.Locked`, `Container.Locked`, `Figure.Locked` |
| `IconSets(Index)` | an icon set for `FormatCondition.IconSet` |
| `Activate()` | bring the workbook's window to the front |

---

## 6. Sheets, Worksheets, Worksheet

**Collection**: `Count`, `Worksheets(Index)` (name or 1-based index), `Item(Index)`, `for ws in Worksheets`, `Add(Before=None, After=None, Count=1, Type=None, Name=None)` (neither Before nor After: before the active sheet, as in Excel; `After=Worksheets(Worksheets.Count)` adds at the end; [YC] `Name` names the new sheet; `Type=xlChart`: a chart sheet), `Add2(Before=None, After=None, Count=1, NewLayout=None)`, [YC] `Exists(Name)` / `"Data" in Worksheets` (case-insensitive), `Select(Replace=True)`. Copying and deleting are the sheet's own `Copy(Before, After)` (the copy brings the sheet-level names) and `Delete()` (below).

Grouped sheets (Excel's `[Group]`, Ctrl / Shift + click on tabs, Ctrl+Shift+PgDn / PgUp, the tab menu's Select All
Sheets): what is typed, cleared, formatted, filled, pasted, inserted or deleted, and column widths / row heights set on
the active sheet go to the same cells of every grouped sheet. `Sheets(["Sheet1", "Sheet2"])` (Excel's `Array(...)`) is
those sheets: `Select(Replace=True)` groups them, `Count`, `Item`, iteration, `FillAcrossSheets(Range,
Type=xlFillWithAll)` (Home > Fill > Across Worksheets: `xlFillWithAll`, `xlFillWithContents`, `xlFillWithFormats`).
`Worksheets.Select()` groups every sheet, `Worksheets.FillAcrossSheets(Range, Type)` fills them all.
`ActiveWindow.SelectedSheets` is the group (the active sheet alone when none). The group is saved with the workbook
(each sheet's tab-selected state) and comes back, `[Group]` in the title, when the file opens.

- `Worksheets` has the worksheets only; `Sheets` has every sheet, chart sheets too (as `ChartSheet` objects); `Charts` has the chart sheets (see 16A).

**CodeName globals**

- Every sheet's CodeName (e.g. `Sheet1`) is a global object in all modules, like Excel.
- `Sheet1.Range("A1")`, `Sheet1.CommandButton1` and `Sheet1.MyFunc()` all work; the last one calls a function in `sheets/Sheet1.py`.
- Never `import Sheet1`: it shadows that object.

**Sheet module template**

```python
from yc.excel import *
Me = SheetModule(__name__)
Range, Cells, Rows, Columns = Me.Range, Me.Cells, Me.Rows, Me.Columns
```

**Properties**

| Member | Description |
|---|---|
| `Name` | sheet name (the text on the tab) |
| `ListObjects` | the sheet's tables (§13A) |
| `Outline` | `ShowLevels(RowLevels=0, ColumnLevels=0)`: show outline levels up to the given ones; `SummaryRow` (`xlSummaryBelow` default, `xlSummaryAbove`: the +/- button sits on the row above the group), `SummaryColumn` (`xlSummaryOnRight` default, `xlSummaryOnLeft`), `AutomaticStyles` (Data > Outline settings) |
| `ClearArrows()` | remove the tracer arrows |
| `CodeName` | read-only (YCGrid gives `Sheet<n>` to a new sheet; changing it is P2, also in `yc_manage_sheets`) |
| `Index` | read-only, 1-based |
| `Visible` | `xlSheetVisible`, `xlSheetHidden`, `xlSheetVeryHidden` (not offered by Home > Format > Unhide Sheet; only code shows it again); hiding the last visible sheet raises, and so does `Select` of a hidden sheet, while `Activate` of one does nothing, as in Excel. Hiding the active sheet makes the next visible sheet active at once (the one before it when none follows), as deleting it does |
| `DisplayPageBreaks` | File > Options > Advanced > "Show page breaks" for the sheet: the dashed lines where pages break in Normal view. Print, Print Preview, a page setup change and `PageSetup` / `HPageBreaks` from Python turn them on, as in Excel; `False` takes them away. Not saved: a workbook opened or made anew starts without them |
| `EnableCalculation` | False: no formula of the sheet is calculated, whatever asks; a formula entered shows 0; True calculates the whole sheet (not saved, Excel 16 measured) |
| `EnableFormatConditionsCalculation` | False: the conditional formats keep what they show |
| `EnableAutoFilter`, `EnableOutlining`, `EnablePivotTable` | with `Protect(UserInterfaceOnly=True)`, a person can still use the AutoFilter arrows, the outline and the PivotTables (not saved) |
| `ConsolidationFunction`, `ConsolidationOptions`, `ConsolidationSources` | the last Data > Consolidate onto the sheet (`xlSum`, `[TopRow, LeftColumn, CreateLinks]`, R1C1 text; None before one) |
| `SaveAs(Filename, FileFormat)` | a text format (`xlCSV`, `xlCSVUTF8`, a `.csv` name) writes this sheet; any other the whole workbook |
| `Parent`, `Next`, `Previous` | read-only |
| `Cells`, `Rows`, `Columns`, `UsedRange` | read-only |
| `Shapes`, `OLEObjects`, `ChartObjects` | controls (§14), charts (§16A) |
| [YC] `RowLimit`, `Store`, `Float()`, `Dock()` | row count (§16F), sheet KV (§16C), floating windows (§16B) |
| `Names`, `Hyperlinks`, `Comments`, `CommentsThreaded` | sheet-level collections (`Comments` holds the notes) |
| `AutoFilter`, `AutoFilterMode`, `Sort` | §12; `AutoFilterMode = False` removes the AutoFilter |
| `Protect(Password=None, DrawingObjects=True, Contents=True, Scenarios=True, UserInterfaceOnly=False, AllowFormattingCells=False, AllowFormattingColumns=False, AllowFormattingRows=False, AllowInsertingColumns=False, AllowInsertingRows=False, AllowInsertingHyperlinks=False, AllowDeletingColumns=False, AllowDeletingRows=False, AllowSorting=False, AllowFiltering=False, AllowUsingPivotTables=False)`, `Unprotect(Password=None)` | sheet protection: locked cells (`Range.Locked`) can't be changed, refused changes raise `ProtectedError`; `UserInterfaceOnly=True` still lets Python change the sheet, its bound tables included (not saved: protect again in `Workbook_Open`). The user's refused change says which cells in its message and the Error Log (`Sheet!B4`). A protected sheet offers no drags it would refuse (moving cells, the fill handle, column / row borders unless allowed) |
| `ProtectContents`, `ProtectDrawingObjects`, `ProtectScenarios`, `ProtectionMode` | read-only (`ProtectScenarios`: protected without Protect Sheet's "Edit scenarios"; then scenarios can't be added, changed or deleted) |
| `EnableSelection` | `xlNoRestrictions` (0), `xlUnlockedCells` (1: clicks and arrow keys skip locked cells), `xlNoSelection` (-4142); applies while the sheet is protected (Review > Protect Sheet's "Select locked / unlocked cells"). On a protected sheet Tab / Shift+Tab go to the next / previous unlocked cell |
| `Protection` | `AllowFormattingCells` and the other `Allow...` flags (read-only), `AllowEditRanges` (`Add(Title, Range, Password=None)`, `Count`, `Item(i or title)` with `Title`, `Range`, `ChangePassword(p)`, `Delete()`, `Users` (Permissions...: `Count`, `Item(i or name)` with `Name`, `AllowEdit`, `Delete()`; `Add(Name, AllowEdit)`, `DeleteAll()`; a Windows account `"DOMAIN\user"` allowed edits the range without its password)) |
| `Tab.Color` | tab color (`xlNone`: none); `Tab.ThemeColor` is P2 |
| `StandardWidth` | default column width |
| `ProtectContents` | read-only |
| [YC] `bind(data, key=None, name=None, at="A1", formats=None, flash=None, stale_after=0, show_age=False, columns=None, replace=True, record=True, render=None)` | a live table updated by key through shared memory; returns a `BoundTable` (`update`, `append`, `delete`, `replace`, `sync`, `set_format`, `set_flash`, `set_render`, `to_frame`, `record`, `age`, `unbind`); see data-binding.md |
| `ScrollArea` | the range the user can select and scroll to (`"$A$1:$H$40"`; `""` = the whole sheet); not saved, like Excel: set it in `Workbook_Open` |
| `CircularReference` | a cell of a circular reference on this sheet (the one the status bar names first), or `None` |
| `<control name>` | get a control, e.g. `Me.CommandButton1.Caption = "Stop"`, `Me.ComboBox1.Value` |

**Methods**

| Member | Description |
|---|---|
| `Range(Cell1, Cell2=None)` | a range |
| `Activate()`, `Select(Replace=True)` | activate; `Select(False)` adds the sheet to the grouped sheets (Ctrl+click on its tab) instead, `Select()` shows it alone (ends a group) |
| `Pictures.Paste(Link=False)` | Paste Options > Picture: a picture of the copied cells (after `Range.Copy`) at the active cell; `Link=True` (Linked Picture) keeps it showing those cells as they change, saved with the workbook |
| `Delete()` | delete; asks the user when `DisplayAlerts=True`. Deleting the active sheet makes the next visible sheet active within the call (the one before it when none follows), as in Excel |
| `Copy(Before=None, After=None)`, `Move(Before=None, After=None)` | copy (the copy, named like `Data (2)`, becomes the active sheet, as in Excel; a hidden sheet's copy is hidden too and the active sheet stays; a very hidden sheet can't be copied: `ApplicationDefinedError`), move |
| `Calculate()` | calculate this sheet only (Shift+F9, Calculate Sheet): with manual calculation the other sheets keep waiting |
| `CircleInvalid()`, `ClearCircles()` | Data Validation > Circle Invalid Data / Clear Validation Circles: red circles around the cells whose values break their rule (up to 255); a corrected cell loses its circle. `CircleInvalid` returns how many [YC] |
| `Evaluate(Name)` | evaluate in the context of this sheet |
| `Paste(Destination=None)` | paste |
| `ShowAllData()` | clear the filter criteria |

Planned, not available yet (P2): `StandardHeight`, `EnableCalculation`, `CustomProperties`, [YC] `LiveWatchRange`
(`Worksheet_LiveChange` gets every changed cell of the sheet; intersect `Target` with the range you watch).

---

## 7. Range

### 7.1 Getting ranges

Address text (`Range("...")`, `ws.Range("...")`) supports: `"A1"`, `"A1:B2"`, `"A:C"`, `"2:5"`, `"Sheet1!A1"`,
unions `"A1,C3:D4"` (multiple areas), `"C1#"` (the spill range of the dynamic array anchored at C1, the cell itself when its result is one value; `ApplicationDefinedError` on a cell without a formula), intersections `"A1:C5 B2:D3"` (a space = the intersection operator), and defined names:
workbook or sheet level (sheet level first), `"Sheet1!Price"`, dynamic names (`=OFFSET(...)`, `=INDEX(...)`, the range is recomputed on every access).

| Member | Description |
|---|---|
| `Range(Cell1, Cell2=None)` | a sub-range relative to this range: `rng.Range("A1")` is the upper-left cell of `rng` |
| `Cells(RowIndex, ColumnIndex)`, `Item(RowIndex, ColumnIndex)`, `rng(RowIndex, ColumnIndex)` | a cell relative to this range (1-based; `Cells(5)` counts row by row). `Cells` without arguments is the range's cells, as in Excel: `Cells.Count` (the number of cells, also for `rng.Rows.Cells`), `Cells.Item(3)`, `for c in rng.Cells`, and every Range member (`rng.Cells.Font.Bold = True`) |
| `Offset(RowOffset=0, ColumnOffset=0)` | offset (raises when it leaves the sheet) |
| `Resize(RowSize=None, ColumnSize=None)` | resize |
| `End(Direction)` | like Ctrl+arrow; `xlUp`, `xlDown`, `xlToLeft`, `xlToRight` |
| `CurrentRegion` | the contiguous data region (like Ctrl+Shift+*) |
| `EntireRow`, `EntireColumn` | entire rows, entire columns |
| `Rows`, `Columns` | the range by rows / columns: `rng.Rows.Count`, `rng.Columns(2)`, `for r in rng.Rows`. Other members act on the range itself, as in Excel: `Range("B2:C3").Rows.Address` is `$B$2:$C$3`, `.Rows.Delete()` deletes B2:C3 (its cells shift as `Delete()` without `Shift` shifts them), `.Columns.Insert()` inserts cells, `.Rows.Value2` reads B2:C3; `Hidden`, `Group()`, `Ungroup()`, `OutlineLevel` and `ShowDetail` take the whole rows (columns): `Range("B2:C3").Columns.Hidden = True` hides columns B:C |
| `Areas` | the areas of a multi-area range: `rng.Areas.Count`, `rng.Areas(1)` |
| `MergeArea` | the merged range it belongs to |
| `SpecialCells(Type, Value=None)` | `xlCellTypeBlanks`, `xlCellTypeConstants`, `xlCellTypeFormulas`, `xlCellTypeLastCell`, `xlCellTypeVisible`, `xlCellTypeComments`, `xlCellTypeAllFormatConditions`, `xlCellTypeAllValidation`, `xlCellTypeSameFormatConditions` / `xlCellTypeSameValidation` (the cells sharing a rule with the single cell, or with the active cell when it is in the range: Go To Special > Same); `Value` may be a sum of `xlNumbers`, `xlTextValues`, `xlLogical`, `xlErrors`. A single cell searches the used range of the whole sheet (notes, rules and validation: the whole sheet, as they sit on empty cells too; visible cells: that cell, or the whole sheet's visible cells once rows or columns are hidden, as Excel); raises `ApplicationDefinedError` (1004) when nothing is found, like Excel |
| `RowDifferences(Comparison)` / `ColumnDifferences(Comparison)` | Go To Special > Row / Column differences: in each row, the cells that differ from that row's cell in `Comparison`'s column (each column against its cell in `Comparison`'s row); formulas compare as formulas (relative: `=A1` in B1 and `=A2` in B2 are the same), other cells by value; a single cell searches its current region; `ApplicationDefinedError` when none differ |
| `Find(What, After=None, LookIn=None, LookAt=None, SearchOrder=None, SearchDirection=xlNext, MatchCase=False, SearchFormat=False)` | starts after `After` (by default the range's upper-left cell, searched last, as in Excel) and wraps around; `None` when not found. `LookIn`, `LookAt` and `SearchOrder` left out take the last `Find`'s, as Excel saves them (at first `xlValues`, `xlPart`, `xlByRows`); `MatchCase` isn't saved. `What=""` finds the next empty cell (with `xlFormulas`: no formula or constant; `xlValues`: shows nothing, so `=""` counts). `SearchFormat=True`: only cells with `Application.FindFormat` (`What=""` finds any cell with it) |
| `FindNext(After=None)`, `FindPrevious(After=None)` | continue forwards / backwards with the settings of the last `Find`; compare addresses yourself to stop when it wraps to the first hit (the usual Excel idiom) |
| `Next`, `Previous` | the cell right (left) of the range's first cell, as Tab (Shift+Tab) goes |
| `Worksheet`, `Parent` | the sheet it is on |
| `Precedents`, `DirectPrecedents`, `Dependents`, `DirectDependents` | precedents and dependents (same sheet; names in formulas are resolved too); raise 1004 when there are none |
| `Name` | get: the `Name` object referring to this range (raises 1004 when there is none); set: `rng.Name = "Price"` defines a workbook-level name, `"Sheet1!Price"` a sheet-level name |
| `CreateNames(Top=None, Left=None, Bottom=None, Right=None)` | create names from the text in the top row, left column etc. (like Create from Selection); detected automatically when nothing is given; returns the number created |
| `ListNames()` | paste a two-column "name | refers to" list at the upper-left cell |

### 7.2 Position and size

| Member | Description |
|---|---|
| `Address(RowAbsolute=True, ColumnAbsolute=True, ReferenceStyle=xlA1, External=False, RelativeTo=None)` | `"$A$1"` without parentheses; `ReferenceStyle=xlR1C1` gives `"R2C3"`, with `RowAbsolute=False, ColumnAbsolute=False, RelativeTo=ws.Range("A1")` it gives `"R[1]C[2]"`; multiple areas are comma-separated |
| `AddressLocal(...)` | same as `Address` |
| `Row`, `Column` | read-only, row and column number of the upper-left cell |
| `Count`, `CountLarge` | read-only, number of cells |
| `Left`, `Top`, `Width`, `Height` | read-only, in points |
| `ColumnWidth` | column width (character units, 0 to 255; columns of different widths read `None`) |
| `RowHeight` | row height (points, 0 to 409; rows of different heights read `None`) |
| `Hidden` | for `EntireRow` / `EntireColumn`: hidden (`None` when some are and some aren't; writing `None` changes nothing) |

### 7.3 Values and formulas

| Member | Description |
|---|---|
| `Value`, `Value2` | values (see §3A) |
| `Formula`, `FormulaLocal` | A1-style formulas; a constant reads as the text of the formula bar, always text as in Excel (`5` is `"5"`, `1E+20` is `"100000000000000000000"`, a date its serial number, text typed as `'10` is `"10"`; `Value2` stays what the cell holds). One formula given to a multi-cell range is relative to the upper-left cell and its relative references are adjusted per cell (like Excel); given to several areas it is relative to the first area's upper-left cell (`Range("A1,C1").Formula = "=B1"` puts `=D1` in C1; also for `Value`); a 2D array sets cells individually |
| `FormulaR1C1`, `FormulaR1C1Local` | R1C1 style (`=R[-1]C*RC[-1]`), readable and writable; given to a multi-cell range every cell gets the same R1C1 formula. Strings, sheet names, functions and names are unaffected |
| `Formula2`, `Formula2R1C1` | the same as `Formula` / `FormulaR1C1`: every formula is a dynamic-array formula (an array result spills; write `@` for implicit intersection) |
| `HasSpill` | read-only; the cell is part of a dynamic array's spill range |
| `SpillingToRange` | read-only; the whole spill range of an anchor cell (`None` for other cells) |
| `SpillParent` | read-only; the anchor cell of the spill range the cell belongs to (`None` otherwise) |
| `FormulaArray` | array formula (Ctrl+Shift+Enter) of the range: writing `"=..."` puts one formula over the whole range, its result fitted to the range; writing text without `=` enters it as a constant in every cell; reading gives the array's formula, a single cell's `Formula` (a constant as text), or `None` (Excel's Null) for several cells that aren't one array formula |
| `HasFormula` | read-only; may be `None` (mixed) for several cells |
| `HasArray` | read-only; the cell is part of an array formula |
| `PrefixCharacter` | read-only; `'` when the first cell's text was typed after an apostrophe, else `""` |
| `CurrentArray` | read-only; the whole range of the array formula the cell belongs to (`None` otherwise) |
| `Text` | read-only, the text shown on screen (a range: the text its cells all show, `None` when they differ) |
| `NumberFormat`, `NumberFormatLocal` | number format code (a range: its one format, `None` when the cells differ; writing `None` changes nothing) |

### 7.4 Formatting

| Member | Description |
|---|---|
| `Font`, `Interior`, `Borders` | §8 |
| `HorizontalAlignment` | `xlGeneral`, `xlLeft`, `xlCenter`, `xlRight`, `xlFill`, `xlJustify`, `xlCenterAcrossSelection`, `xlDistributed` |
| `VerticalAlignment` | `xlTop`, `xlCenter`, `xlBottom`, `xlJustify`, `xlDistributed` |
| `WrapText`, `ShrinkToFit`, `IndentLevel`, `Orientation` | wrap text, shrink to fit, indent, text angle (`Orientation`: -90..90 degrees counterclockwise, `xlHorizontal`, `xlUpward`, `xlDownward`, `xlVertical` for stacked letters) |
| `ReadingOrder` | Format Cells > Alignment > Text direction: `xlContext`, `xlLTR`, `xlRTL` (Right-to-Left mirrors General alignment) |
| `JustifyLastLine` | [YC] Alignment > Justify distributed (with `HorizontalAlignment = xlDistributed`; the file formats' justifyLastLine) |
| `Characters(Start=1, Length=None)` | part of the (top-left) cell's text: `Text` (settable: replaces those characters), `Caption`, `Count`, `Insert(String)`, `Delete()`, `Font` (`Name`, `Size`, `Bold`, `Italic`, `Underline`, `Strikethrough`, `Color`, `Superscript`, `Subscript`, `FontStyle`; reads `None` when the characters differ). The fonts follow the text when it is copied, moved or saved (also to `.xlsx`); a new value drops them |
| `MergeCells` | whether its cells are merged (`None` when only some are); can be set: `True` merges as Excel's property does (the first value goes to the upper-left cell, which keeps its own formats; the other cells go back to Normal, unlike `Merge()`) |
| `Locked`, `FormulaHidden` | protection related |
| `BorderAround(LineStyle=xlContinuous, Weight=xlThin, ColorIndex=None, Color=None)` | outline |
| `Style` | the cell's named style (a `Style`); set it with a name (`"Good"`, `"Heading 1"`, a custom style) or a `Style` |

### 7.5 Editing

`RemoveDuplicates(Columns=None, Header=xlNo)`: remove duplicate rows (only comparing the 1-based `Columns` given, a list is allowed; all columns by default; text is compared case-insensitively; `xlGuess` guesses the header as Excel does, see `ListObjects.Add`); the unique rows move up within the range with their formats; returns the number of rows removed.

`TextToColumns(Destination=None, DataType=xlDelimited, TextQualifier=xlTextQualifierDoubleQuote, ConsecutiveDelimiter=False, Tab=False, Semicolon=False, Comma=False, Space=False, Other=False, OtherChar=None, FieldInfo=None)`: split the text of the first column into columns (`xlFixedWidth`: `FieldInfo` gives `[start position, format]` pairs, positions in characters starting at 0; delimited: `[column, format]` with `xlGeneralFormat`, `xlTextFormat`, a date order `xlMDYFormat`, `xlDMYFormat`, `xlYMDFormat`, `xlMYDFormat`, `xlDYMFormat`, `xlYDMFormat` (a field that isn't such a date stays text) or `xlSkipColumn`); numbers and dates are recognized in General columns. `DecimalSeparator` / `ThousandsSeparator` (one character each, `"."` and `","` by default) and `TrailingMinusNumbers` (default `True`: `"123-"` is -123) are the wizard's Advanced settings for General columns.

| Member | Description |
|---|---|
| `ClearContents()`, `ClearFormats()`, `Clear()`, `ClearComments()` (`ClearNotes()` is the same), `ClearHyperlinks()` | clear; `ClearContents()` or `Clear()` over a whole table (header row too) deletes the table, as in Excel (what named it is #REF!) |
| `Delete(Shift=None)` | `xlShiftUp`, `xlShiftToLeft`; without `Shift`, Excel's rule: a range with more rows than columns shifts left (`Range("A1:A2").Delete()`), others up; whole rows and columns are deleted. Every area is deleted (the lowest first: `Range("A2,A4").EntireRow.Delete()` deletes rows 2 and 4), each shifting by its own shape; overlapping areas, or whole rows / columns with other cells, raise `ApplicationDefinedError` as in Excel |
| `Insert(Shift=None, CopyOrigin=None)` | `xlShiftDown`, `xlShiftToRight`; without `Shift` a range with more rows than columns shifts right, others down; every area gets its cells (`Range("B:B,D:D").Insert()` puts a new column before B and before D); the new cells take the formats of the row above / column left (`xlFormatFromLeftOrAbove`, default) or below / right (`xlFormatFromRightOrBelow`), as Excel's. While cells are copied or cut (`CutCopyMode`) it is Insert Copied Cells / Insert Cut Cells: room is made and the cells go there (a cut leaves no gap behind), as in Excel |
| `Justify()` | Home > Fill > Justify: the text of the first column is spread over lines as wide as the range's columns, one line per row |
| `Copy(Destination=None)`, `Cut(Destination=None)` | to the clipboard when no Destination is given. Several areas that share their columns (or rows) are copied closed up, in sheet order, as Excel's: `Range("A1,A3").Copy(Range("C1"))` fills C1:C2, with their formats, formulas pasted as their values; areas that don't line up, and `Cut` of several areas, raise `ApplicationDefinedError` ("This action won't work on multiple selections"). To the clipboard, several areas copy the first only |
| `PasteSpecial(Paste=xlPasteAll, Operation=xlNone, SkipBlanks=False, Transpose=False)` | `xlPasteValues`, `xlPasteFormats`, `xlPasteFormulas`, `xlPasteComments` (comments and notes), `xlPasteValidation`, `xlPasteAllUsingSourceTheme`, `xlPasteAllExceptBorders`, `xlPasteColumnWidths`, `xlPasteFormulasAndNumberFormats`, `xlPasteValuesAndNumberFormats`, `xlPasteAllMergingConditionalFormats` (the destination's rules stay); `xlPasteAll` brings the notes and the data validation too; `Operation=xlPasteSpecialOperationAdd` / `Subtract` / `Multiply` / `Divide` combines the copied numbers with the cells' (a formula becomes `=(formula)+n`, text stays); `Transpose=True` turns references to the copied cells with them, absolute ones too (`=$A$1` copied from A1 to A4 reads `=$A$4`), as Excel does |
| `Replace(What, Replacement, LookAt=None, SearchOrder=None, MatchCase=False, MatchByte=None, SearchFormat=False, ReplaceFormat=False)` | returns `True`, as Excel's, whether or not anything was replaced. `SearchFormat`: only cells with `Application.FindFormat`; `ReplaceFormat`: `Application.ReplaceFormat` is written into each replaced cell (`What=""`: every cell with the find format, contents kept) |
| `Merge(Across=False)`, `UnMerge()` | merge, unmerge; as Excel's, the merged cell keeps the first value in reading order (moved to the upper-left cell) and every cell takes the formats of the cell it came from |
| `AutoFit()` | for `Columns` / `Rows`: autofit |
| `AutoFill(Destination, Type=xlFillDefault)` | AutoFill; `Type`: `xlFillCopy`, `xlFillSeries`, `xlFillFormats`, `xlFillValues`, `xlFillDays`, `xlFillWeekdays`, `xlFillMonths`, `xlFillYears`, `xlLinearTrend`, `xlGrowthTrend` (the Auto Fill Options button after dragging the fill handle offers the same) |
| `FillDown()`, `FillUp()`, `FillLeft()`, `FillRight()` | fill |
| `Sort(Key1=None, Order1=xlAscending, Key2=None, Type=None, Order2=xlAscending, Key3=None, Order3=xlAscending, Header=xlNo, OrderCustom=None, MatchCase=False, Orientation=xlTopToBottom)` | sort (Excel's argument order: `Type` comes after `Key2`; `MatchCase` and `Orientation` by name only); `Header=xlGuess`: Excel's guess of a header row (as `ListObjects.Add`'s); one cell sorts its current region; `OrderCustom`: 1 + the number of the custom list that orders `Key1`, as in Excel (1 is the normal order) |
| `AutoFilter(Field=None, Criteria1=None, Operator=xlAnd, Criteria2=None, VisibleDropDown=True)` | AutoFilter; toggles without arguments |
| `RemoveDuplicates(Columns, Header=xlNo)` | remove duplicates (see above) |
| `Group()`, `Ungroup()` | outline: group entire rows (`Rows("3:5")`) or entire columns; a range of cells groups its rows; outline levels go up to 8 (`Group` of rows all at level 8 raises `ApplicationDefinedError`, `Ungroup` of rows in no group too, as in Excel); rows an `Ungroup` takes out of a collapsed group stay hidden. On a PivotTable's labels, `Group()` groups the selected items (Group Selection) and `Group(Start, End, By, Periods)` groups the label's field, as `PivotField.Group` |
| `ShowDetail` | read / write: expand (`True`) or collapse the group this summary row / column sums up: the group above it (left of it), or below it (right of it) with `Outline.SummaryRow = xlSummaryAbove` (`SummaryColumn = xlSummaryOnLeft`); a row inside a group collapses that group. Reads `True` on any other row; writing it on a row in no group raises, as in Excel |
| `OutlineLevel` | read / write: outline level of the first row (1 = not grouped, up to 8; other levels raise) |
| `ClearOutline()` | remove the groups of the range's rows and columns; one cell clears the whole sheet's outline (Data > Ungroup > Clear Outline); collapsed rows and columns are shown again, where `Ungroup` and `OutlineLevel = 1` keep them hidden (Excel 16, measured) |
| `ListObject` | the table the range's first cell is in, or `None` |
| `AutoOutline()` | Data > Group > Auto Outline: groups the rows (columns) that `SUM` / `SUBTOTAL` formulas right below (right of) them add up; a single cell works on the sheet's data; returns the number of groups |
| `ShowPrecedents(Remove=False)`, `ShowDependents(Remove=False)` | tracer arrows of formula auditing (each call one level further); cells on other sheets get Excel's dashed arrow to a sheet icon (a double-click on it opens Go To with those references) |
| `ApplyNames(Names=None, IgnoreRelativeAbsolute=True, UseRowColumnNames=True, OmitColumn=True, OmitRow=True, Order=xlRowThenColumn, AppendLast=False)` | Formulas -> Define Name -> Apply Names: references in the range's formulas that are exactly one of the names (a name or a list; None: all) become the name; with `UseRowColumnNames` a cell where a row-range name crosses a column-range name becomes `RowName ColumnName` (`Order=xlColumnThenRow`: the other way round; `OmitColumn` / `OmitRow` leave out the name of the formula's own column / row, written `@Name`); a single cell applies them to the whole sheet. Returns the number of formulas changed |
| `ShowErrors()` | Formulas -> Error Checking -> Trace Error: arrows back from the cell's error value to the cell where it starts (red), and from that cell's precedents (blue) |
| `Errors(index)` | background error checking of the first cell: `Errors(xlNumberAsText).Value` is True when the cell breaks that rule (`xlEvaluateToError` 1, `xlTextDate` 2, `xlNumberAsText` 3, `xlInconsistentFormula` 4, `xlOmittedCells` 5, `xlUnlockedFormulaCells` 6, `xlEmptyCellReferences` 7, `xlListDataValidation` 8, `xlInconsistentListFormula` 9, `xlMisleadingFormat` 10), whatever the options; `.Ignore = True` is the error button's Ignore Error for the range (saved with the workbook, undoable) |
| `Calculate()` | calculate the formulas of this range only (Excel's Range.Calculate): cells outside are read as they show, and with manual calculation everything else keeps waiting |
| `Dirty()` | mark the range's formulas to be calculated again: with automatic calculation at once, with manual calculation at the next `Calculate` (`CalculationState` reads `xlPending` meanwhile) |
| `Select()`, `Activate()`, `Show()` | select, activate, scroll into view |

### 7.6 Related objects and iteration

`Range.DisplayFormat` (read-only): the first cell as the sheet shows it, conditional formats applied: `Font`, `Interior`, `NumberFormat`, `HorizontalAlignment`, `VerticalAlignment`, `WrapText`, `IndentLevel`... `Range.PivotCell` (`PivotCellType`, `PivotTable`, `PivotField`, `PivotItem`, `RowItems`, `ColumnItems`, `DataField`), `Range.PivotField`, `Range.PivotItem`: what a cell of a report is (Excel 16's types, measured).

| Member | Description |
|---|---|
| `Comment`, `AddComment(Text)` | notes (§10); `AddComment` on a cell with a note raises, as in Excel (change it with `Comment.Text`) |
| `CommentThreaded`, `AddCommentThreaded(Text)` | threaded comments (§10); [YC] `ConvertNotesToComments()` turns the notes in the range into threads |
| `Hyperlinks` | hyperlinks (§10) |
| `Validation` | data validation (§9) |
| `FormatConditions` | conditional formatting (§9) |
| `Name` | the Name object when this range is a defined name |
| `for c in rng:` | iterate cell by cell, in the order of Excel's For Each (rows first, then columns) |

### 7.7 [YC] extensions

| Member | Description |
|---|---|
| `ToNumpy(dtype="float64", Empty=nan)` | read as a 2D numpy array; non-numeric cells are replaced by `Empty` |
| `Value = ndarray` | write a numpy array directly (the fastest way to write) |
| `BindTopics(TopicIds)` | bind every cell to a market data topic (ids of the same shape, nested lists or numpy): each `Topics.Push` writes the cell directly, with zero formula cost; runtime state, so bind again in `Workbook_Open` |
| `UnbindTopics()` | remove the bindings (the cells keep their last values) |
| `TopicIds` | the bound topic ids (rows of ids; None where a cell isn't bound) |
| `IsStale` | (P2) read-only, whether any cell is waiting for a new UDF or stream value |
| `NoteText(Text=None, Start=None, Length=None)` | Excel's old note accessor: without `Text` the top-left cell's note text (`""` when none), with `Text` it sets the note (`Start` / `Length` are ignored) |

---

## 8. Font, Interior, Borders, Border

On several cells whose formats differ, a property reads `None` (Excel's Null): `Font.Bold`, `Font.Color`,
`Interior.Color`, `HorizontalAlignment`, `WrapText`, `Locked` and the like; writing `None` back changes nothing.

| Object | Members |
|---|---|
| `Font` | `Name`, `Size`, `Bold`, `Italic`, `Underline` (`xlUnderlineStyleNone`, `xlUnderlineStyleSingle`, `xlUnderlineStyleDouble`, `xlUnderlineStyleSingleAccounting`, `xlUnderlineStyleDoubleAccounting`), `Strikethrough`, `Superscript`, `Subscript`, `Color`, `ColorIndex`, `ThemeColor` (`xlThemeColorDark1` ... `xlThemeColorFollowedHyperlink`; `None` for a fixed color; the color follows the theme), `TintAndShade` (-1 darker .. 1 lighter), `FontStyle` |
| `Interior` | `Color`, `ColorIndex` (`xlNone` is no fill), `Pattern` (`xlSolid`, `xlNone`, the 17 patterns `xlPatternGray75` ... `xlPatternCrissCross`, `xlPatternLinearGradient`, `xlPatternRectangularGradient`), `PatternColor`, `PatternColorIndex` (the color of the pattern lines; `Color` is the background), `Gradient` (`Degree`: 0, 45, 90, 135...; `ColorStops.Add(0).Color` / `ColorStops.Add(1).Color` or `ColorStops(1)` / `ColorStops(2)`: two colors; a rectangular gradient's `RectangleLeft` / `RectangleTop` / `RectangleRight` / `RectangleBottom`: 0.5 from the center, 0 or 1 from a corner, Fill Effects > From corner), `ThemeColor`, `TintAndShade` |
| `Borders` | `Borders(Index)`, `Item(Index)`, `LineStyle`, `Weight`, `Color` (set every edge at once), `for b in rng.Borders`, `Count` (6); `ColorIndex` on the collection is P2 (use `Borders(i).ColorIndex`) |
| `Border` | `LineStyle`, `Weight`, `Color`, `ColorIndex`, `ThemeColor`, `TintAndShade` |
| `Range.Style` | the cell's named style (a `Style`); set it with a name (`"Good"`, `"Heading 1"`, `"20% - Accent1"`, `"Percent"`, a custom style) or a `Style` |
| `Workbook.Styles` | `Count`, `Item(name or index)`, iteration, `Add(Name, BasedOn=None)` (`BasedOn`: a Range whose first cell's formats the style takes; default Normal), `Merge(Workbook)` (Merge Styles: the styles of another workbook, a `.ycg` path or an object with `FullName`; same names are replaced; a workbook open in another YCGrid gives its current styles, unsaved changes included) |
| `Application.FindFormat`, `Application.ReplaceFormat` | Find and Replace's Format...: a `CellFormat` with `Font` (`Name`, `Size`, `Bold`, `Italic`, `Underline`, `Strikethrough`, `Color`, `Superscript`, `Subscript`), `Interior` (`Color`, `ColorIndex`, `Pattern`, `PatternColor`), `Borders(xlEdgeLeft...)` (`LineStyle`, `Weight`, `Color`), `NumberFormat`, `HorizontalAlignment`, `VerticalAlignment`, `WrapText`, `ShrinkToFit`, `IndentLevel`, `Orientation`, `Locked`, `FormulaHidden`, `Clear()`; only the properties set are compared or written |
| `Style` | `Name`, `NameLocal`, `BuiltIn`, `Font`, `Interior`, `Borders`, `NumberFormat`, `HorizontalAlignment`, `VerticalAlignment`, `WrapText`, `IndentLevel`, `Locked`, `FormulaHidden`, `IncludeNumber`, `IncludeAlignment`, `IncludeFont`, `IncludeBorder`, `IncludePatterns`, `IncludeProtection`, `Delete()` (a custom style's cells get the Normal formats; a changed built-in style goes back to its original). Changing a style changes every cell that has it |
| `Workbook.Theme` | `ThemeColorScheme.Colors(msoThemeDark1 ... msoThemeFollowedHyperlink).RGB` (settable), `ThemeColorScheme.Name`, `ThemeFontScheme.MajorFont(msoThemeLatin).Name` / `MinorFont` (settable; the minor font is the default cell font), [YC] `Name`, `BuiltinThemes` |
| `Workbook.ApplyTheme(Filename)` | [YC] a built-in theme by name (`"Office"`, `"Office 2007 - 2010"`, `"Office 2023"`, `"Facet"`, `"Ion"`, `"Retrospect"`, `"Slice"`, `"Wisp"`, `"Integral"`, `"Gray"`); a path's file name is used as the name |

- `Borders` indexes: `xlEdgeLeft`, `xlEdgeTop`, `xlEdgeBottom`, `xlEdgeRight`, `xlInsideVertical`, `xlInsideHorizontal`, `xlDiagonalDown`, `xlDiagonalUp`. Two cells side by side share the line between them, as in Excel: B2's `xlEdgeBottom` is B3's `xlEdgeTop`, read from either cell, and `xlLineStyleNone` set on either clears it.
- `LineStyle`: `xlContinuous`, `xlDash`, `xlDashDot`, `xlDashDotDot`, `xlDot`, `xlDouble`, `xlSlantDashDot`, `xlLineStyleNone`.
- `Weight`: `xlHairline`, `xlThin`, `xlMedium`, `xlThick`.
- Settings inside a `with rng.Font as f:` block are sent together.

---

## 9. Validation, FormatConditions, FormatCondition

| Object | Members |
|---|---|
| `Validation` | `Add(Type, AlertStyle=xlValidAlertStop, Operator=xlBetween, Formula1=None, Formula2=None)`, `Modify(...)`, `Delete()`, `Type`, `Operator`, `Formula1`, `Formula2`, `AlertStyle`, `IgnoreBlank`, `InCellDropdown`, `InputTitle`, `InputMessage`, `ErrorTitle`, `ErrorMessage`, `ShowInput`, `ShowError`, `Value` (read-only: whether the current value passes validation) |
| `FormatConditions` | `Add(Type, Operator=None, Formula1=None, Formula2=None)`, `AddTop10()`, `AddAboveAverage()`, `AddUniqueValues()`, `AddColorScale(ColorScaleType)`, `AddDatabar()`, `AddIconSetCondition()`, `Delete()`, `Count`, `Item(Index)` |
| `FormatCondition` | `Font`, `Interior`, `Borders` (the outline the rule draws around a cell, Excel's Red Border: `Borders.Color`, `Borders.LineStyle = xlNone` removes it; `Borders(xlTop)` is the same outline), `StopIfTrue`, `Priority`, `SetFirstPriority()`, `SetLastPriority()`, `Formula1`, `Formula2`, `Type`, `Operator`, `AppliesTo`, `ModifyAppliesToRange(Range)`, `Delete()`; icon sets: `IconSet` (`xl3Arrows`... or `Workbook.IconSets(n)`), `ReverseOrder`, `ShowIconOnly`, `IconCriteria(i)` (`Type`: `xlConditionValuePercent`, `xlConditionValueNumber`, `xlConditionValuePercentile`, `xlConditionValueFormula`; `Value`; `Operator`: `xlGreaterEqual` / `xlGreater`; `IconCriteria(1)` is the lowest icon) |

- As in Excel: `FormatConditions.Add` and the other `Add...` methods put the new rule last (the lowest priority; the UI's
  New Rule puts it first); `Validation.Add` raises `ApplicationDefinedError` (1004) over cells that already have a rule
  (`Delete()` first) and for an incomplete rule (a list without `Formula1`, Between without `Formula2`, a number rule
  whose `Formula1` isn't a number); reading a property of a multi-cell range whose cells don't share one rule raises
  1004; `Validation.Value` checks each cell's stored value (`="b"` is valid in the list a,b,c). A Text, Date Occurring,
  Blanks or Errors rule's `Formula1` is the formula Excel generates for it (`=LEN(TRIM(A1))=0`).
- Validation `Type`: `xlValidateInputOnly`, `xlValidateWholeNumber`, `xlValidateDecimal`, `xlValidateList`, `xlValidateDate`, `xlValidateTime`, `xlValidateTextLength`, `xlValidateCustom`.
- `AlertStyle`: `xlValidAlertStop`, `xlValidAlertWarning`, `xlValidAlertInformation`.
- `Operator`: `xlBetween`, `xlNotBetween`, `xlEqual`, `xlNotEqual`, `xlGreater`, `xlLess`, `xlGreaterEqual`, `xlLessEqual`.
- FormatConditions `Type`: `xlCellValue`, `xlExpression`, `xlTextString` (`String=`, `TextOperator=`: `xlContains`, `xlDoesNotContain`, `xlBeginsWith`, `xlEndsWith`; `FormatCondition.TextOperator`, `.Text`), `xlTimePeriod` (A Date Occurring: `DateOperator=` `xlToday`, `xlYesterday`, `xlTomorrow`, `xlLast7Days`, `xlThisWeek`, `xlLastWeek`, `xlNextWeek`, `xlThisMonth`, `xlLastMonth`, `xlNextMonth`; `FormatCondition.DateOperator`), `xlBlanksCondition`, `xlNoBlanksCondition`, `xlErrorsCondition`, `xlNoErrorsCondition`; plus `AddTop10()` (`Rank`, `Percent`, `TopBottom`), `AddAboveAverage()` (`AboveBelow`: `xlAboveAverage`, `xlBelowAverage`, `xlEqualAboveAverage`, `xlEqualBelowAverage`, `xlAboveStdDev`, `xlBelowStdDev`; `NumStdDev`), `AddUniqueValues()` (`DupeUnique`), `AddColorScale(2|3)` (`ColorScaleCriteria(i).FormatColor.Color`, `.Type`: `xlConditionValueLowestValue`, `xlConditionValueHighestValue`, `xlConditionValueNumber`, `xlConditionValuePercent`, `xlConditionValuePercentile`, `xlConditionValueFormula`; `.Value`), `AddDatabar()` (`BarColor.Color`, `BarFillType` `xlDataBarFillGradient` / `xlDataBarFillSolid`, `BarBorder.Type` / `.Color`, `Direction` `xlContext` / `xlLTR` / `xlRTL`, `ShowValue` (False: Show Bar Only), `MinPoint` / `MaxPoint` `.Type`, `.Value`, `.Modify(newtype, newvalue)`). The rule's format (Format...): `Interior` (`Color`, `Pattern`, `PatternColor`), `Font` (`Color`, `Bold`, `Italic`, `Strikethrough`, `Underline`), `NumberFormat` (`Range.Text` shows it), `Borders(xlLeft / xlTop / xlRight / xlBottom)` (`LineStyle`, `Weight`, `Color`) and `Borders.Color` (an outline in one color).
- **Difference from Excel**: formulas in FormatConditions and Validation use the upper-left cell of the applied range as the base of relative references (Excel uses the active cell). `"=B2>100"` applied to `B2:B100` is `B3>100` for B3.
- AutoFilter: `Range.AutoFilter(Field, Criteria1, Operator, Criteria2)` (no arguments = toggle; with `Operator=xlFilterValues`, `Criteria1` is a list of values; with `xlTop10Items` etc. `Criteria1` is the number of items; `xlFilterCellColor` / `xlFilterFontColor` with an RGB color, `xlFilterNoFill`, `xlFilterAutomaticFontColor`; `xlFilterIcon` with `ActiveWorkbook.IconSets(xl3Arrows).Item(n)` (Filter by Cell Icon: the rows whose conditional formatting shows that icon, 1 = the lowest values' icon), `xlFilterNoIcon` (the cells without one); `AutoFilter.Filters[k]` (the filtered columns' settings) then has `icon_set` and `icon` (0-based); `xlFilterDynamic` with `xlFilterToday`, `xlFilterThisMonth`, `xlFilterAllDatesInPeriodQuarter1`, `xlFilterAboveAverage`, `xlFilterBelowAverage`...; otherwise criteria such as `">100"`, `"=T*"`, with `xlAnd` / `xlOr` and `Criteria2`), `Worksheet.AutoFilterMode` (set False to turn it off), `Worksheet.FilterMode`, `Worksheet.ShowAllData()`, `Worksheet.AutoFilter.Range`, `.ApplyFilter()`, `.Filters(i)` (column i of the list, as in Excel: `On`, `Criteria1`, `Criteria2`, `Operator`, `Count`; `Filters.Count` is the list's columns). A filter by values ignores case, as Excel's list: checking `AAPL` shows `aapl` too.

---

## 10. Comment, Hyperlinks, Names

| Object | Members |
|---|---|
| `Comment` | a note: `Text(Text=None, Start=None, Overwrite=False)` (with `Start`, the text goes in at that character, 1-based: inserted, or over what is there with `Overwrite=True`), `Visible` (True: shown on the sheet all the time, Review > Show/Hide Note; saved with the workbook), `Author`, `Next()`, `Previous()`, `Delete()`, `Shape` (its box, sized by its handles and Format Comment: `Width`, `Height`, `Left`, `Top` in points, `Fill.ForeColor.RGB`, `Line.ForeColor.RGB`, `TextFrame.Characters().Font` `Name` / `Size` / `Bold` / `Italic` / `Color`, `TextFrame.HorizontalAlignment` / `VerticalAlignment`) |
| `CommentThreaded` | a threaded comment (a cell has either a note or a thread): `Text(Text=None, Start=None, Overwrite=False)`, `Author.Name`, `Date`, `Replies` (`Count`, `Replies(Index)`, `for r in Replies`; each reply is a `CommentThreaded`), `AddReply(Text)`, `Resolved` (read/write), `Parent`, `Delete()` (the first post deletes the thread, a reply deletes itself). New posts are signed with `Application.UserName` |
| `CommentThreaded` (more) | `Next()` / `Previous()`: the next / previous thread on the sheet (Review > Next / Previous Comment), `None` at the end |
| `Hyperlinks` | `Add(Anchor, Address, SubAddress=None, ScreenTip=None, TextToDisplay=None)`, `Delete()`, `Count`, `Item(Index)` |
| `Hyperlink` | `Address`, `SubAddress`, `TextToDisplay`, `ScreenTip`, `Range`, `Follow()`, `Delete()` |
| `Names` | `ThisWorkbook.Names` / `Application.Names` / the global `Names`: every name; `ws.Names`: the sheet-level names of that sheet (`ws.Names.Add` creates sheet-level names). `Add(Name, RefersTo=None, Visible=True, ..., RefersToR1C1=None, Comment=None)`: replaces a name with the same name and scope; writing `Name` as `"Sheet1!Price"` also makes it sheet level; `RefersTo` may be a `Range`, `"=Sheet1!$A$1:$A$10"`, a constant `"=0.05"` or a formula `"=OFFSET(...)"` (text without `=` is a string constant); relative references are relative to the active cell, as in Excel (`=Sheet1!A1` with B1 active: the cell left of the one using the name). A formula refers to a sheet-level name of another sheet as `=Sheet1!Price`. `Names(Index)`: a 1-based index (sorted by name), a name, `"Sheet1!Name"`, or `Names(RefersTo="=...")`; `Count`, `len()`, `in`, `for n in Names` |
| `Name` | `Name` (get: sheet-level names carry the `"Sheet1!"` prefix; set: rename, formulas that use it are updated), `RefersTo` (read/write; relative references shown from the active cell), `RefersToR1C1` (read/write; `RC[-1]` is the cell to the left), `RefersToLocal` / `RefersToR1C1Local` (the same as `RefersTo` / `RefersToR1C1`), `RefersToRange` (dynamic names work too; raises 1004 when it isn't a range), `Value` (= `RefersTo`, like Excel), `Visible` (read/write; hidden names don't appear in the Name Box or Use in Formula), `Comment` (read/write), `Index`, `Parent` (workbook or sheet), `WorkbookParameter` (always `False`), `MacroType` (always `xlNone`), `Delete()` |

Name rules as in Excel: start with a letter, a CJK character, an underscore or a backslash; only letters, digits, CJK characters, underscores and periods; can't be the same as a cell reference (`A1`, `R1C1`); case-insensitive; sheet-level names take priority over workbook-level names with the same name.
In the UI, threaded comments are on Review -> Comments (New Comment Ctrl+Alt+M, Delete, Previous / Next, Show Comments pane) and the cell's right-click menu; a purple marker shows a thread, gray once resolved. Notes stay on Review -> Notes (Shift+F2) with Convert to Comments.

In the UI: typing a new name in the Name Box and pressing Enter defines it for the selection, and its drop-down jumps to a name; Formulas -> Defined Names (Name Manager Ctrl+F3, Define Name, Use in Formula F3, Create from Selection Ctrl+Shift+F3).

---

## 11. Window, Windows

`Window.Caption` (the workbook's part of the title bar; None: the file's name), `WindowState`, `Left`, `Top`, `Width`, `Height` (the YCGrid window's), `UsableWidth`, `UsableHeight` (the cells' area), `Close(SaveChanges, Filename)` (closes the workbook), `ScrollIntoView(Left, Top, Width, Height, Start)` (points), `Panes` / `ActivePane`: one pane, or two or four of a split or frozen window, each with `VisibleRange`, `ScrollRow`, `ScrollColumn`, `SmallScroll`, `LargeScroll`, `ScrollIntoView`, `Activate()`. In four panes of a split window 1 is the top left, 2 the bottom left, 3 the top right, 4 the bottom right, and the ScrollRow / ScrollColumn of 2 and 3 read each other's; four frozen panes go across then down (2 is the top right), each pane's ScrollRow / ScrollColumn its own, and a frozen pane asked to scroll (Panes(1).ScrollColumn = 17) scrolls the panes beside it while it stays: Excel 16 does both (measured).

View state (scrolling, selection, zoom) belongs to the UI thread: writes go through the UI command ring and apply on the next frame, reads come from the UiSnapshot, so the UI never waits. Call `Application.Sync(Wait="ui")` [YC] when you need to be sure it was applied.

| Member | Description |
|---|---|
| `FreezePanes` | freeze panes: select the first scrollable cell first, then set it to True; or set `SplitRow` / `SplitColumn`, then set it to True |
| `SplitRow`, `SplitColumn` | split position (rows / columns in the top / left panes) |
| [YC] `CenterColumns` | T layout for an option chain (calls \| strike \| puts): `ActiveWindow.CenterColumns = "K"` keeps column K in the middle of the window; the columns on its left are right-aligned against it and one horizontal scroll moves both sides outward (scrolling right reveals the outer call and put columns together). Rows frozen with `SplitRow` / `FreezePanes` stay on top; `"K:L"` or a Range pins several columns; `None` (or `FreezePanes = False`) ends it; not column A. Saved with the workbook's layout. UI: View > Freeze Panes > Freeze Columns in the Middle (select the strike cell under the header rows first) |
| `Split` | View -> Split: True splits the window at the active cell (or turns frozen panes into split ones); each pane scrolls on its own |
| `ScrollRow`, `ScrollColumn` | row and column number of the upper-left cell |
| `Zoom` | zoom (10-400; other values raise, as in Excel); `True` zooms to the selection |
| `View` | `xlNormalView`, `xlPageBreakPreview` (gray outside the print area; drag the blue lines to move page breaks or resize the print area), `xlPageLayoutView` (the sheet as printed pages with headers and footers; click a cell to select it and type to edit it through the formula bar). View -> Workbook Views in the UI |
| `DisplayZeros`, `GridlineColor` | the active sheet's zeros and gridline color (RGB; 0 Automatic), saved with the workbook |
| `DisplayWorkbookTabs`, `DisplayHorizontalScrollBar`, `DisplayVerticalScrollBar` | the workbook's sheet tabs and scroll bars, saved with it |
| `DisplayGridlines`, `DisplayHeadings`, `DisplayFormulas` | display options; gridlines and headings belong to the active sheet, like Excel (each sheet keeps its own, saved with the workbook) |
| `DisplayOutline` | the outline's bars and level buttons (Ctrl+8) |
| `SelectedSheets` | the grouped sheets (`Count`, `Item`, iteration); the active sheet alone when none are grouped |
| `VisibleRange` | read-only, the currently visible range |
| `ActiveCell`, `ActiveSheet`, `Selection`, `RangeSelection` | read-only |
| `SmallScroll(Down=0, Up=0, ToRight=0, ToLeft=0)`, `LargeScroll(...)` | scroll |
| `Activate()` | activate (give focus to that window) |
| `NewWindow()` | View > New Window: a second window on this workbook |
| `ActivateNext()` / `ActivatePrevious()` | the next / previous window of View > Switch Windows (Ctrl+F6 / Ctrl+Shift+F6, Ctrl+Tab) |
| [YC] `Floating` | whether it is a floating sheet window (read/write: setting True is the same as pulling the sheet out) |
| `Windows(Index)`, `Windows.Count`, `for w in Windows` | collection: one entry each for the sheet window in the main window, floating sheet windows and sheet windows docked in containers. View -> New Window opens more windows on the workbook (Book1:2...), each with its own sheet, selection and scrolling; View -> Split divides a window into panes that scroll on their own (`Window.Split`) |

Planned, not available yet (P2): `WindowState`, `Caption`, `Left`, `Top`, `Width`, `Height`;
`ScrollIntoView(Left, Top, Width, Height, Start=True)` raises `UnsupportedError` (use `Range.Show()`).
A floating sheet window is placed with `Worksheet.Float(Left, Top, Width, Height)`.

---

## 12. Sort, SortFields, AutoFilter

```python
ws = Worksheets("Positions")
ws.Sort.SortFields.Clear()
ws.Sort.SortFields.Add(Key=ws.Range("C2:C500"), SortOn=xlSortOnValues, Order=xlDescending)
ws.Sort.SetRange(ws.Range("A1:F500"))
ws.Sort.Header = xlYes
ws.Sort.Apply()
```

| Object | Members |
|---|---|
| `Sort` | `SortFields`, `SetRange(Rng)`, `Header` (`xlYes`, `xlNo`, `xlGuess`), `MatchCase` (Sort Options > Case sensitive: lowercase before uppercase), `Orientation` (`xlTopToBottom`, `xlLeftToRight`: the columns move, ordered by the keys' rows), `Apply()`; hidden rows (a filtered list) don't move, as in Excel: the visible rows are sorted among themselves (also `Range.Sort`) |
| `SortFields` | `Add(Key, SortOn=xlSortOnValues, Order=xlAscending, CustomOrder=None, DataOption=xlSortNormal)` (`CustomOrder`: a custom list number or `"High,Medium,Low"`; values not in it go last), `Add2(...)` (same as Add, matching what newer Excel macro recorders write), `Clear()`, `Count`, `Item(i)`; a field's `SetIcon(Icon)` with `SortOn=xlSortOnIcon` puts the rows showing that conditional formatting icon on top (`xlDescending`: on bottom); `Icon` is `ActiveWorkbook.IconSets(xl3Arrows).Item(n)` (1 = the lowest values' icon) |
| `AutoFilter` (of a sheet) | `Range`, `Filters(Index)` (`On`, `Criteria1`, `Criteria2`, `Operator`), `ShowAllData()`, `ApplyFilter()`, `Sort` (`SortFields.Add(Key, Order=xlDescending)`, `SortFields(1).Order` / `.Key`, `SortFields.Clear()`, `Apply()`: sorts the list's visible rows and records the sort; the filter button shows it, Data > Reapply sorts again, it is saved with the workbook) |

- `Operator` of `Range.AutoFilter`: `xlAnd`, `xlOr`, `xlTop10Items`, `xlBottom10Items`, `xlTop10Percent`, `xlBottom10Percent`, `xlFilterValues` (`Criteria1` is a list), `xlFilterCellColor` / `xlFilterFontColor` (`Criteria1` is an RGB color), `xlFilterNoFill`, `xlFilterAutomaticFontColor`, `xlFilterDynamic`.

---

## 13. WorksheetFunction

- Every function of the formula engine can be called; dots in names become underscores: `WorksheetFunction.Sum(Range("A:A"))`, `WorksheetFunction.VLookup(x, Range("A:C"), 3, False)`, `WorksheetFunction.Norm_S_Dist(z, True)`.
- Arguments may be Ranges, scalars or arrays.
- Errors raise `ApplicationDefinedError` (1004), like Excel.
- To avoid exceptions use `Application.VLookup(...)`: errors return an `XlError`, test with `IsError()`.
- Many calls in a loop are slow (one round trip each). Read an array once and compute in Python, or write formulas and let the engine compute.

---

## 13A. ListObjects (tables)

`ListColumn.Total` (the column's cell in the total row; None without one), `ListObject.Slicers` (the slicers filtering the table).

Tables (Insert -> Table, Ctrl+T) have a header row, an optional totals row and a style drawn from the table (banded rows follow inserted rows). Formulas use structured references: `Table1[Qty]`, `Table1[[#Headers],[Qty]]`, `Table1[#All]`, `Table1[#Totals]`, `Table1[[Qty]:[Price]]`, `[@Qty]` (this row, inside the table), `Table1` alone (the data body). Renaming the table or a header updates the formulas. Typing (or writing from Python) just below or right of a table grows it, and a formula entered in an empty table column fills the whole column.

| Member | Description |
|---|---|
| `Worksheet.ListObjects` | `Add(SourceType=xlSrcRange, Source=None, LinkSource=None, XlListObjectHasHeaders=xlGuess, Destination=None, TableStyleName=None)` (`TableStyleName=None`: the workbook's `DefaultTableStyle`, `TableStyleMedium2` unless changed; without headers a header row is inserted above; `xlGuess` guesses as Excel and the Create Table dialog do: no blank in the first two rows, and text over a number in some column or a first row formatted apart), `Count`, `Item(index or name)` / `ListObjects(...)`, iteration |
| `ListObject` | `Name` / `DisplayName` (read / write), `Range`, `HeaderRowRange`, `DataBodyRange`, `TotalsRowRange`, `ListColumns`, `ListRows`, `ShowTotals`, `ShowHeaders`, `TableStyle` (name such as `"TableStyleMedium2"`, `"TableStyleLight9"`, or [YC] `"TableStyleTrading"`: black, for quote boards), `ShowTableStyleRowStripes`, `ShowTableStyleColumnStripes`, `ShowTableStyleFirstColumn`, `ShowTableStyleLastColumn`, `ShowAutoFilterDropDown` / `ShowAutoFilter` (the Filter Button), `AutoFilter` (the table's filter: the sheet's when it is on the table, otherwise the table's own buttons, as every table has in Excel; `Filters`, `ShowAllData()`, `ApplyFilter()`, `Sort`; `Range.AutoFilter(Field, Criteria1)` inside such a table filters it), `Sort` (as a recorded macro writes it: `SortFields.Clear()` / `Add(Key, SortOn, Order, CustomOrder, DataOption)`, `Header` (`xlYes`; `xlNo` raises for a table with its header row), `MatchCase`, `Orientation`, `Rng` (the table), `Apply()`: sorts the data rows, never the header or totals row; the fields stay after `Apply`), `Resize(Range)`, `Unlist()` (convert to range), `Delete()` |
| `ListColumns` / `ListColumn` | `Count`, `Item(index or name)`, `Add(Position=None)`; `Name`, `Index`, `Range`, `DataBodyRange`, `TotalsCalculation` (`xlTotalsCalculationSum`, `Average`, `Count`, `CountNums`, `Min`, `Max`, `StdDev`, `Var`, `None`), `Delete()` |
| `ListRows` / `ListRow` | `Count`, `Item(index)`, `Add(Position=None)` (calculated columns continue; `Position` from 1 to `Count + 1`, others raise `SubscriptOutOfRangeError`); `Index`, `Range`, `Delete()` |
| `Workbook.TableStyles` | Excel's 60 built-in styles (`TableStyleLight1`-`21`, `TableStyleMedium1`-`28`, `TableStyleDark1`-`11`; [YC] `TableStyleTrading`) then the workbook's own: `Count`, `Item(name or index)`, iteration, `Add(TableStyleName)` (New Table Style: an empty style), `DefaultTableStyle` (as `Workbook.DefaultTableStyle`) |
| `Workbook.DefaultTableStyle` | the style Ctrl+T, Format as Table and `ListObjects.Add` (no `TableStyleName`) start with: a `TableStyle`; set it with a name or a `TableStyle` (New Table Style's "Set as default table style for this document"); saved with the workbook |
| `TableStyle` | `Name`, `NameLocal`, `BuiltIn`, `ShowAsAvailableTableStyle`, `Duplicate(NewTableStyleName=None)`, `Delete()` (the workbook's own only), `TableStyleElements(xlWholeTable / xlHeaderRow / xlTotalRow / xlFirstColumn / xlLastColumn / xlRowStripe1 / xlRowStripe2 / xlColumnStripe1 / xlColumnStripe2)`: `Interior.Color`, `Font.Color` / `Bold` / `Italic`, `Borders(xlEdgeTop / xlEdgeBottom / xlEdgeLeft / xlEdgeRight / xlInsideHorizontal / xlInsideVertical)` (`LineStyle`, `Weight`, `Color`), `StripeSize`, `HasFormat`, `Clear()`. Built-in styles are read-only (Duplicate them first), as in Excel |

## 13A2. What-If Analysis, Solver and data tools

| Member | Description |
|---|---|
| `Range.GoalSeek(Goal, ChangingCell)` | change `ChangingCell` until this formula cell returns `Goal`; True when found |
| `Range.Table(RowInput=None, ColumnInput=None)` | make the range a data table (`{=TABLE(row, col)}`): the top row / left column hold the input values and formulas; the body follows recalculation and can't be edited (clear the whole body to remove it); `Range.Formula` of a body cell is `=TABLE(,D1)` |
| `Worksheet.Scenarios` (also `Scenarios()`, `Scenarios(i)`) | `Add(Name, ChangingCells, Values=None, Comment=None, Locked=True, Hidden=False)` -> Scenario, `Count`, `Item(index or name)`, `CreateSummary(ReportType=xlStandardSummary, ResultCells)` -> the new "Scenario Summary" sheet (`xlPivotTable`: a "Scenario PivotTable" sheet with a PivotTable of the result cells per scenario), `Merge(Source)` (Scenario Manager > Merge: copy the scenarios of another sheet; a name in use gets a number) |
| `Scenario` | `Name`, `Comment`, `ChangingCells`, `Values(Index=None)`, `Locked` / `Hidden` (read / write: Protection > Prevent changes / Hide), `Show()`, `Delete()`, `ChangeScenario(ChangingCells, Values=None)` |
| `Range.Consolidate(Sources, Function=xlSum, TopRow=False, LeftColumn=False, CreateLinks=False)` | combine `Sources` (`"Sheet1!R1C1:R5C3"`, A1 text or Range objects) at this cell, by position or by the labels of the top row / left column; `Function`: `xlSum`, `xlCount`, `xlAverage`, `xlMax`, `xlMin`, `xlProduct`, `xlCountNums`, `xlStDev`, `xlStDevP`, `xlVar`, `xlVarP`; `CreateLinks`: under each label a collapsed group of rows linking to each source's cells (with the workbook's name in an extra column) and the summary formula over them (sources on other sheets only). The Consolidate dialog keeps its settings and All references list with the sheet |
| `Range.Subtotal(GroupBy, Function, TotalList, Replace=True, PageBreaks=False, SummaryBelowData=xlSummaryBelow)` | Data > Subtotal: a `=SUBTOTAL(n, ...)` row after each run of the same value in field `GroupBy` (1 = the list's first column) for the fields in `TotalList`, a Grand row, outline levels (1 grand, 2 groups, 3 detail); one cell: its current region; not in a table (Excel's rule). `Range.RemoveSubtotal()` takes the subtotal rows, the outline and the page breaks away |
| `Worksheet.QueryTables` | `Add(Connection="TEXT;path", Destination)` -> QueryTable: `TextFileParseType` (`xlDelimited` / `xlFixedWidth`), `TextFileCommaDelimiter`, `TextFileTabDelimiter`, `TextFileSemicolonDelimiter`, `TextFileSpaceDelimiter`, `TextFileOtherDelimiter`, `TextFileConsecutiveDelimiter`, `TextFileTextQualifier`, `TextFileColumnDataTypes` (`xlGeneralFormat`, `xlTextFormat`, `xlSkipColumn` per column), `TextFilePlatform` (code page), `TextFileStartRow`, `TextFileFixedColumnWidths`, `Refresh()`, `ResultRange`. Data > From Text/CSV (the preview window, Load / Load To...) loads a file as a table |
| `Application.FlashFill` | File > Options > Advanced > Automatically Flash Fill: grey suggestions down the column while typing next to data; Enter fills them |
| `Range.AdvancedFilter(Action, CriteriaRange=None, CopyToRange=None, Unique=False)` | `xlFilterInPlace` hides the other rows (`Worksheet.ShowAllData()` shows them), `xlFilterCopy` copies the matching rows (the labels in the first row of `CopyToRange` pick the columns); criteria like the database functions (AND across a row, OR between rows, text = "begins with") |
| `Range.FlashFill()` | fill the column from the examples typed above (Ctrl+E): tokens of the neighboring columns, case changes, initials and constant text |
| Analysis ToolPak | `Application.Run("ATPVBAEN.XLAM!<procedure>", ...)` or `yc.analysis.<procedure>(...)` with Excel's arguments; the output range is a top-left cell, or a text / None for a new sheet; returns the output Range. Procedures: `Anova1(inprng, outrng, grouped="C", labels, alpha)`, `Anova2(inprng, outrng, sample_rows, alpha)`, `Anova3(inprng, outrng, labels, alpha)`, `Mcorrel` / `Mcovar(inprng, outrng, grouped, labels)`, `Descr(inprng, outrng, grouped, labels, summary, ds_conf, k1, k2)`, `Expon(inprng, outrng, damp, stderrs, chart, labels)`, `Ftestv(inprng1, inprng2, outrng, labels, alpha)`, `Fourier(inprng, outrng, inverse, labels)`, `Histogram(inprng, outrng, binrng, pareto, chartc, chart, labels)`, `Moveavg(inprng, outrng, interval, stderrs, chart, labels)`, `Random(outrng, variables, points, distribution, seed, *params)`, `RankPerc(inprng, outrng, grouped, labels)`, `Regress(inpyrng, inpxrng, constant, labels, confid, soutrng, residuals, sresiduals, rplots, lplots, routrng, nplots, poutrng)`, `Sample(inprng, outrng, method "P"/"R", rate, labels)`, `Ttestm` (paired) / `Ttest1` (equal variances) / `Ttest2` (unequal) `(inprng1, inprng2, outrng, labels, alpha, difference)`, `Ztestm(..., difference, var1, var2)`. Moving Average and Exponential Smoothing write formulas, the others values. In the Data Analysis dialogs, Histogram's Chart Output and Regression's Residual Plots, Line Fit Plots and Normal Probability Plots add charts beside the output, and New Workbook opens the results in a new window |
| Solver functions | `SolverReset()`, `SolverOk(SetCell, MaxMinVal=1 (2 min, 3 value of), ValueOf, ByChange, Engine=1 Simplex LP / 2 GRG Nonlinear / 3 Evolutionary)`, `SolverAdd(CellRef, Relation (1 <=, 2 =, 3 >=, 4 int, 5 bin, 6 dif), FormulaText)`, `SolverChange`, `SolverDelete`, `SolverOptions(MaxTime, Iterations, Precision, AssumeLinear, StepThru, Estimates, Derivatives, SearchOption, IntTolerance, Scaling, Convergence, AssumeNonNeg, PopulationSize, RandomSeed, MutationRate, Multistart, RequireBounds, MaxSubproblems, MaxIntegerSols, MaxTimeNoImp, SolveWithout)` (the Options dialog's three tabs; saved with the model), `SolverSave(SaveArea)` / `SolverLoad(LoadArea)` (Load/Save Model: the model as cells), `SolverSolve(UserFinish)` -> Excel's result code (0 found, 1 converged, 3 limit, 4 unbounded, 5 infeasible, 7 not linear, 13 model error, 14 integer solution), `SolverFinish(KeepFinal=1 / 2 restore, ReportArray=[1 Answer, 2 Sensitivity, 3 Limits])` (report sheets before the model's sheet), `SolverGet(TypeNum)`; the model is kept with the active sheet (saved in the file) |

## 13B. PivotTables

`PivotFields("Sum of Sales")` finds a data field by its caption too, as recorded macros write it. The report's areas, as Excel 16 gives them (measured; one value under a filter is one column wide, its RowRange an error): `RowRange` (the row labels from their caption to the grand total), `ColumnRange` (the column caption and items; an error without column fields), `DataLabelRange` (the values' caption cell), `PageRange` / `PageRangeCells` (the filters), `DataPivotField` (the Values field), `VisibleFields`, `HiddenFields`, `Slicers`, `GrandTotalName` ("Grand Total" until set), `RefreshDate`, `RefreshName`, `GetData("'Sum of Sales' East 2024")` (as GETPIVOTDATA: under a Top N or a value filter the totals the report shows), `ShowPages(PageField)` (a sheet with a copy of the report for each filter item). `PivotField`: `AutoShow(Type, Range, Count, Field)` (top or bottom N by a data field) with `AutoShowType`, `AutoShowRange`, `AutoShowCount`, `AutoShowField`, `AddPageItem`, `VisibleItems`, `HiddenItems`, `DataType`, `AutoGroup()`, `Value`, `CurrentPageName` (None: OLAP fields only). `PivotItem.DataRange`, `LabelRange`. `PivotCache.CreatePivotChart`, `MissingItemsLimit` (xlMissingItemsNone: items the source lost are not kept).

Insert -> PivotTable (or the Tables group), the PivotTable Fields pane (tick fields or drag them between Filters, Columns, Rows and Values),
the PivotTable Analyze and Design tabs, filter buttons and +/- buttons on the report. The report is written into the cells (values in
its PivotTable style, PivotStyleLight16 by default; formats you give report cells stay with their items across refreshes); refresh it
after the source changes (Alt+F5, Ctrl+Alt+F5 for all). Fields follow their names: inserting, deleting or moving source columns keeps
every field where it was. `GETPIVOTDATA(data_field, pivot_table, [field1, item1], ...)` works in formulas (items as shown or as General).
A report never covers a table; one that would grow over data raises `YcError` ("There's already data in ...") while
`Application.DisplayAlerts` is True, as Excel would ask, and replaces the data when it is False.

| Member | Description |
|---|---|
| `Workbook.PivotCaches` (also `PivotCaches()`) | `Create(SourceType=xlDatabase, SourceData)` (a Range, an address in A1 or R1C1 (`"Data!R1C1:R100C4"`, as recorded macros write it) or a table name) -> PivotCache; `Refresh()` refreshes every PivotTable |
| `PivotCache` | `CreatePivotTable(TableDestination, TableName=None)` (a Range or an address, `"Sheet2!R3C1"` too), `Refresh()`, `SourceData` |
| `Worksheet.PivotTables(Index=None)` | the collection (`Count`, `Item`, iteration, `Add(PivotCache, TableDestination, TableName)`) or one PivotTable; `Range.PivotTable` is the one covering a cell |
| `PivotTable` | `Name`, `PivotFields(Index)`, `RowFields`, `ColumnFields`, `PageFields`, `DataFields`, `AddDataField(Field, Caption, Function)`, `AddFields(RowFields, ColumnFields, PageFields)`, `RefreshTable()`, `PivotCache()`, `SourceData` (read as Excel reads it, R1C1: `"Data!R1C1:R100C4"`; set in A1 or R1C1), `TableRange1`, `TableRange2`, `DataBodyRange`, `Location`, `RowGrand`, `ColumnGrand`, `RowAxisLayout(xlCompactRow / xlOutlineRow / xlTabularRow)`, `SubtotalLocation(xlAtTop / xlAtBottom)`, `RepeatAllLabels(xlRepeatLabels / xlDoNotRepeatLabels)`, `TableStyle2` (`"PivotStyleLight1"`...`"PivotStyleDark28"`, `"PivotStyleNone"`), `ShowTableStyleRowHeaders`, `ShowTableStyleColumnHeaders`, `ShowTableStyleRowStripes`, `ShowTableStyleColumnStripes`, `ClearTable()`, `ClearAllFilters()` (every field, slicer and timeline), `PivotSelect(Name="", Mode=xlDataAndLabel / xlLabelOnly / xlDataOnly)` ([YC] the whole report's labels / values; returns the Range), `ManualUpdate` (True: changes wait until it is set back to False, like Defer Layout Update), `GetPivotData(DataField, field1, item1, ...)` ([YC] returns the value), `Delete()` [YC] |
| PivotTable Options | `NullString` / `DisplayNullString` (For empty cells show), `ErrorString` / `DisplayErrorString`, `HasAutoFormat` (Autofit column widths on update), `PreserveFormatting`, `ShowDrillIndicators` (+/- buttons), `DisplayFieldCaptions`, `MergeLabels`, `CompactRowIndent`, `EnableDrilldown` (Show Details), `SortUsingCustomLists` (month and day names in calendar order), `FieldListSortAscending` (Display > Field List: Sort A to Z; False: data source order), `PivotCache().RefreshOnFileOpen`, [YC] `ShowAllFieldButtons` (the field buttons of its PivotCharts), [YC] `EnableSelection` (Analyze > Select > Enable Selection: the edge of a label selects the item's rows). Values are General until `DataFields(i).NumberFormat` is set (files saved before keep the source's format) |
| `PivotField` | `Name` / `Caption` (a custom name; "PivotTable field name already exists." when taken), `SourceName`, `Orientation` (`xlRowField`, `xlColumnField`, `xlPageField`, `xlDataField`, `xlHidden`), `Position`, `PivotItems(Index)`, `CurrentPage` (Filters: an item or `"(All)"`), `EnableMultiplePageItems` (Filters: several items, the cell reads "(Multiple Items)"), `ClearAllFilters()`, `ClearLabelFilters()`, `ClearValueFilters()`, `ClearManualFilter()`, `PivotFilters` (`Add2(Type, DataField, Value1, Value2)` with `xlCaptionEquals`...`xlCaptionIsNotBetween`, `xlValueEquals`...`xlValueIsNotBetween`, `xlTopCount` / `xlTopPercent` / `xlTopSum` and Bottom; `Count`, `Item` (`FilterType`, `Value1`, `Value2`), `Delete()`), `AutoSort(Order, Field)` (Field = the field's name: by labels; a data field's caption: by its values; `xlManual`), `AutoSortOrder`, `AutoSortField`, `Subtotals` (Excel's 12 booleans: Automatic, then custom functions), `RepeatLabels`, `LayoutBlankLine`, `ShowAllItems`, `ShowDetail` / `DrilledDown` (expand / collapse every item), Field Settings: `IncludeNewItemsInFilter` (False, Excel's default: items that arrive after a manual filter was set stay out), `LayoutForm` (`xlTabular` / `xlOutline`), `LayoutCompactRow` (the next field's labels in this column), `LayoutSubtotalLocation` (`xlAtTop` / `xlAtBottom`), `LayoutPageBreak` (a page break after each item in the sheet's page setup), `DataRange` (the cells of the field's items on the report; a data field's values without grand totals), `LabelRange` (the cell with the field's caption) |
| data field (from `DataFields` / `AddDataField`) | `Function` (`xlSum`, `xlCount`, `xlAverage`, `xlMax`, `xlMin`, `xlProduct`, `xlCountNums`, `xlStDev`, `xlStDevP`, `xlVar`, `xlVarP`), `Caption`, `NumberFormat` (by default a value keeps its source's format), `Calculation` (all 15: `xlNoAdditionalCalculation`, `xlPercentOfTotal`, `xlPercentOfColumn`, `xlPercentOfRow`, `xlPercentOf`, `xlPercentOfParentRow`, `xlPercentOfParentColumn`, `xlPercentOfParent`, `xlDifferenceFrom`, `xlPercentDifferenceFrom`, `xlRunningTotal`, `xlPercentRunningTotal`, `xlRankAscending`, `xlRankDecending`, `xlIndex`), `BaseField`, `BaseItem` (an item, `"(previous)"` / `"(next)"`). The same field twice reads "Sum of Sales" and "Sum of Sales2" |
| `PivotItem` | `Name` (as shown: a date in its source's format, a renamed item's name), `SourceName` [YC] (its identity: General format), `Caption` (rename), `Value`, `Visible` (filters the item out of the report), `ShowDetail` (False = collapsed), `Position` (move it; the field's sort becomes manual) |
| grouping | `PivotField.Group(Start=True, End=True, By=None, Periods=None)`: dates with Excel's `Periods` array (seconds ... years; several periods: the field shows the finest, the others become fields of their own, "Years", "Quarters"..., placed before it; days with `By` = number of days) or [YC] `By=ycGroupYears / ycGroupQuarters / ycGroupMonths / ycGroupYearMonths / ycGroupDays`; numbers with `By` = the range size (`"0-9"`, `"10-19"`...); `Ungroup()`. Chosen items: `Range.Group()` on the report's labels (Group Selection) makes a field "Region2" with "Group1" holding them |
| calculated fields | `PivotTable.CalculatedFields().Add(Name, Formula)` (`"=Amount/Qty"`; field names with spaces in `'quotes'`; evaluated on the fields' sums, like Excel), `Count`, `Item`; `PivotField.Formula`, `IsCalculated`, `Delete()`; put one on Values with `Orientation = xlDataField` |
| calculated items | `PivotField.CalculatedItems().Add(Name, Formula)` -> PivotItem: an item made of the field's other items (`"=East+West"`, `"=North*1.1"`, `"='North East'-West/2"`: sums and differences of items, each multiplied or divided by a number); it comes after the field's items and the subtotals / grand totals include it (as in Excel); not on grouped fields. `PivotItem.Formula`, `IsCalculated`, `Delete()`; `CalculatedItems().Count`, `Item` |
| Show Details | `Range.ShowDetail = True` on a value of the report (or double-click it): its source records on a new sheet, as a table |
| PivotCharts | `Chart.SetSourceData(pt.TableRange1)` (or PivotTable Analyze -> PivotChart): the chart follows the report's fields, filters and refreshes; `Chart.PivotLayout.PivotTable`, `Chart.ShowAllFieldButtons`. PivotTable Analyze -> PivotChart opens the Insert Chart dialog (every type, previews of the report's data) |
| slicers | `Workbook.SlicerCaches.Add2(Source, SourceField, Name=None, SlicerCacheType=None)` (`Source`: a PivotTable or a ListObject; `Add` is the same) -> SlicerCache (`SourceName`, `SlicerItems(name).Selected`, `VisibleSlicerItemsList`, `ClearManualFilter()`, `Slicers`, `Delete()`, Slicer Settings: `SortItems` (`xlSlicerSortAscending` / `Descending`), `CrossFilterType` (`xlSlicerCrossFilterShowItemsWithDataAtTop`, `...ShowItemsWithNoData`, `xlSlicerNoCrossFilter`), `ShowAllItems` (False: hide items with no data), Report Connections: `PivotTables` (`AddPivotTable(pt)`, `RemovePivotTable(pt)`, `Count`, `Item`: PivotTables of the same source, filtered by the field of the same name)); a table's slicers: `SlicerCaches.Add2(ListObject, "Column")` (`ListObject`, `SlicerItems`, `ClearManualFilter()`: the table's AutoFilter on that column; Insert -> Slicer or Table Design -> Insert Slicer with a cell of the table selected); `SlicerCache.Slicers.Add(SlicerDestination, Level=None, Name=None, Caption=None, Top=None, Left=None, Width=None, Height=None)` (Excel's order: `Level`, used by timelines, comes second; pass the others by name) -> Slicer (`Name`, `Caption`, `Top`, `Left`, `Width`, `Height`, `NumberOfColumns`, `DisplayHeader`, `Copy()` / `Cut()` (then `Worksheet.Paste()` or Ctrl+V pastes it at the active cell), `Delete()`). In the UI a click selects a slicer (the Slicer tab: Caption, Slicer Settings, Columns, Height, Width; Delete removes it) |
| timelines | `Workbook.SlicerCaches.Add2(PivotTable, "Date", None, xlTimeline)` -> SlicerCache (`SlicerCacheType`, `TimelineState.SetFilterDateRange(StartDate, EndDate)`, `TimelineState.StartDate` / `EndDate`, `TimelineState.FilterType` ([YC] 0 none, 1 a date range), `ClearDateFilter()`); `.Slicers.Add(ws, Level, Name, Caption, Top, Left, Width, Height)` -> a timeline (`TimelineViewState.Level`: `xlTimelineLevelYears`, `Quarters`, `Months`, `Days`; `TimelineViewState.ShowHeader`, `ShowSelectionLabel`, `ShowHorizontalScrollbar`, `ShowTimeLevel`). In the UI: PivotTable Analyze -> Insert Timeline; drag across periods, Shift+click extends, drag the selection's edge handles, the scroll bar; a selected timeline shows the Timeline tab (Caption, Settings, Report Connections, Show) |

```python
pc = ActiveWorkbook.PivotCaches.Create(xlDatabase, "Trades!A1:F500")
pt = pc.CreatePivotTable("Summary!A3", "TradePivot")
pt.PivotFields("Symbol").Orientation = xlRowField
pt.PivotFields("Side").Orientation = xlColumnField
pt.AddDataField(pt.PivotFields("Amount"), "Total Amount", xlSum)
pt.PivotFields("Symbol").PivotItems("2412").Visible = False
pt.RefreshTable()                            # after the source changed
```

- The report's values can't be typed over (like Excel: "Cannot change this part of a PivotTable report"); typing over a data field's
  caption or a field's header renames it, over an item's label renames the item (or, with another item's name, moves that item there).
- A slicer and the field's own filter drop-down are one filter when the field is on the report; deleting the slicer keeps it.
- External OLAP servers and data models are not a source.

## 14. Shapes, OLEObjects, CommandButton, ComboBox (controls)

Controls can float anywhere on a sheet. Event handling and Design Mode are in `events-and-controls.md`.

| Object | Members |
|---|---|
| `Shapes` | `Count`, `Shapes(Index)`, `Item(Index)`, `for s in Shapes`, `AddChart2(Style=-1, XlChartType=xlColumnClustered, Left=None, Top=None, Width=None, Height=None, NewLayout=True)` (§16A; `AddChart` is the same), `AddPicture(Filename, LinkToFile=False, SaveWithDocument=True, Left=0, Top=0, Width=-1, Height=-1)` (§14A; `AddPicture2` is the same), `AddShape(Type, Left, Top, Width, Height)` (every preset of Excel's Shapes gallery by its msoAutoShapeType: rectangles, basic shapes, block arrows, equation shapes, flowchart, stars and banners, callouts), `AddTextbox(Orientation, Left, Top, Width, Height)`, `AddLine(BeginX, BeginY, EndX, EndY)`, `AddCurve(SafeArrayOfPoints)` (Curve), `AddPolyline(SafeArrayOfPoints)` (Freeform: Shape; closed when it ends on its first point), `AddTextEffect(PresetTextEffect, Text, FontName, FontSize, FontBold, FontItalic, Left, Top)` (WordArt: `msoTextEffect1` = 0 ... 15 styles) |
| `Shapes.AddFormControl(Type, Left, Top, Width, Height)` | a form control: `xlButtonControl`, `xlCheckBox`, `xlDropDown`, `xlListBox`, `xlOptionButton`, `xlGroupBox`, `xlLabel`, `xlScrollBar`, `xlSpinner` (`xlEditBox` is not supported); returns a `FormControl` |
| `Buttons()`, `CheckBoxes()`, `DropDowns()`, `ListBoxes()`, `OptionButtons()`, `Spinners()`, `ScrollBars()`, `GroupBoxes()`, `Labels()` (Worksheet) | the form controls of one kind: `Add(Left, Top, Width, Height)`, `Count`, `Item(Index)`, `for c in ...`, `Delete()` |
| `FormControl` | `HorizontalAlignment` / `VerticalAlignment` (`xlLeft`, `xlCenter`, `xlRight` / `xlTop`, `xlCenter`, `xlBottom`), `Orientation` (`xlHorizontal`, `xlVertical`, `xlUpward`, `xlDownward`), `AutoSize`, `TextFrame.AutoMargins` / `MarginLeft` / `MarginTop` / `MarginRight` / `MarginBottom` (a button's Format Control > Alignment and Margins), `Display3DShading` (check boxes, option buttons, lists, group boxes, scroll bars, spinners), `Name`, `Left`, `Top`, `Width`, `Height`, `Visible`, `Enabled`, `Caption` / `Text` / `TextFrame.Characters().Text`, `OnAction` (macro name or function; runs on click / change instead of sheet-module events), `FormControlType`, `Type` (`msoFormControl`), `ControlFormat` (the control itself): `LinkedCell`, `ListFillRange`, `Value` (check boxes and option buttons `xlOn` / `xlOff`; lists: the item number; spinners and scroll bars: the number), `ListIndex` (1-based), `List`, `ListCount`, `AddItem(Text, Index)`, `RemoveItem(Index, Count)`, `RemoveAllItems()`, `Min`, `Max`, `SmallChange`, `LargeChange`, `DropDownLines`, `MultiSelect`, `Delete()`. Like Excel, combo / list boxes and option buttons put the 1-based number of the choice in the linked cell, and the option buttons inside one group box share a linked cell. `OLEObjects` lists only the ActiveX-style controls |
| `Shape` (a control or chart in `Shapes`) | the `OLEObject` / `ChartObject` members (below, §16A), plus `Type` (`msoOLEControlObject`, `msoChart`) and `HasChart`; `Chart` for a chart. Pictures, AutoShapes, lines and text boxes are §14A. `TopLeftCell`, `BottomRightCell` and `OLEFormat` are P2: use `Left` / `Top` and the object itself |
| `Shapes.AddConnector(Type, BeginX, BeginY, EndX, EndY)` | a connector between the points: `msoConnectorStraight`, `msoConnectorElbow`, `msoConnectorCurve`; `Shape.ConnectorFormat.Type` reads / changes it, `Shape.Connector` is `msoTrue` for lines and connectors, `Shape.AutoShapeType` is the preset (`msoShapeNotPrimitive` 138 for a freeform); `Shape.Adjustments` (`Count`, `Item(n)`, `Adjustments[n] = v`: the yellow handles' values as Excel's fractions) |
| `OLEObjects` | `Add(ClassType="Forms.CommandButton.1", Link=False, DisplayAsIcon=False, IconFileName=None, IconIndex=None, IconLabel=None, Left=0, Top=0, Width=None, Height=None, Name=None, Caption=None, Placement=None, **properties)` (Excel's argument order: pass `Left`, `Top`, `Width`, `Height` by name), `Count`, `OLEObjects(Index)`, `for o in OLEObjects`, `Delete()` |
| `OLEObject` | `Name`, `Object` (CommandButton or ComboBox), `progID`, `Left`, `Top`, `Width`, `Height`, `Visible`, `Enabled`, `Locked`, `PrintObject` (Format Control > Properties > Print object; controls are printed), `AlternativeText` (Format Control > Alt Text), `Placement`, `LinkedCell`, `ListFillRange`, `Parent`, `BringToFront()`, `SendToBack()`, `ZOrderPosition`, `Delete()`, `Select()` (`TopLeftCell`, `BottomRightCell` and `ZOrder()` are P2) |
| `CommandButton` | `Caption`, `Enabled`, `Visible`, `BackColor`, `ForeColor`, `Font` (`Name`, `Size`, `Bold`, `Italic`, `Color`), `WordWrap` |
| `ComboBox` | `Value`, `Text`, `ListIndex` (-1 = nothing selected), `ListCount`, `List` (read/write a list, or `List(i)`), `AddItem(Item, Index=None)`, `RemoveItem(Index)`, `Clear()`, `ListFillRange` (e.g. `"Settings!A2:A20"`, read straight from the cells), `LinkedCell` (two-way), `Style` (`fmStyleDropDownCombo`, `fmStyleDropDownList`), `ListRows`, `Enabled`, `Locked`, `Font`, `BackColor`, `ForeColor`; `MatchEntry` is P2 (setting it raises `AttributeError`) |

- `OLEObjects.Add` accepts `Forms.CommandButton.1`, `Forms.ComboBox.1`, `Forms.CheckBox.1`, `Forms.OptionButton.1`, `Forms.ToggleButton.1`, `Forms.TextBox.1`, `Forms.Label.1`, `Forms.ListBox.1`, `Forms.SpinButton.1`, `Forms.ScrollBar.1` and `Forms.Image.1` (their properties and events are in `events-and-controls.md` §6.4a); other ClassTypes raise `UnsupportedError`.
- The newer controls' properties: `Value`, `GroupName`, `Min`, `Max`, `SmallChange`, `LargeChange`, `Orientation`, `MultiSelect`, `Selected(i)`, `MultiLine`, `PasswordChar`, `WordWrap`, `Picture` (`LoadPicture(path)`), `PictureSizeMode`.
- Placing by cells: `r = Me.Range("B2:C3")`, then `Me.OLEObjects.Add(ClassType="Forms.ComboBox.1", Left=r.Left, Top=r.Top, Width=r.Width, Height=r.Height)`.
- Fixed to the window (doesn't move when scrolling): `Me.OLEObjects("ComboBox1").Placement = ycFloatWindow` [YC]; `Left` / `Top` then become positions relative to the window (or the frozen-pane quadrant).
- Getting a control: `Me.ComboBox1` (sheet modules), `Sheet1.ComboBox1` (any module), `Me.OLEObjects("ComboBox1").Object`.

## 14A. Pictures, shapes, text boxes and sparklines

`Shape.TopLeftCell`, `BottomRightCell` (the cells under its corners), `CopyPicture()`.

Insert -> Illustrations (Pictures, Shapes), Insert -> Text Box and Insert -> Sparklines. Pictures are stored inside the workbook. Shapes and text boxes take text (double-click one to type); a shape with `OnAction` runs that macro when clicked outside Design Mode. Colors are Excel RGB values (`R + G*256 + B*65536`, e.g. `RGB(255, 0, 0)`).

| Object | Members |
|---|---|
| `Shape` (picture, AutoShape, line, text box) | `Name`, `ID`, `Type` (`msoPicture`, `msoAutoShape`, `msoLine`, `msoTextBox`), `AutoShapeType` (`msoShapeRectangle`, `msoShapeRoundedRectangle`, `msoShapeOval`, `msoShapeIsoscelesTriangle`, `msoShapeRightArrow`; read / write), `Left`, `Top`, `Width`, `Height`, `IncrementLeft(n)`, `IncrementTop(n)`, `Placement`, `Visible`, `OnAction` (a macro name or function), `Fill` (`ForeColor.RGB`, `Visible`, `Solid()`), `Line` (`ForeColor.RGB`, `Weight`, `Visible`), `TextFrame` / `TextFrame2` (`TextRange.Text`, `Characters().Text`, `TextRange.Font` with `Name`, `Size`, `Bold`, `Italic`, `Color`, `Fill.ForeColor.RGB`; `HorizontalAlignment` `xlLeft` / `xlCenter` / `xlRight`, `VerticalAlignment` `xlTop` / `xlCenter` / `xlBottom` (`VerticalAnchor` is the same), `HasText`, `MarginLeft` / `MarginTop` / `MarginRight` / `MarginBottom` (points), `WordWrap`, `AutoSize` (`msoAutoSizeShapeToFitText`: the height follows the text when it is edited)), `ZOrder(msoBringToFront / msoSendToBack / msoBringForward / msoSendBackward)` (forward / backward: past the next object), `ZOrderPosition`, `Delete()` |
| `Shape` (format, protection) | `Line.DashStyle` (`msoLineSolid`, `msoLineSquareDot`, `msoLineRoundDot`, `msoLineDash`, `msoLineDashDot`, `msoLineLongDash`), `Line.BeginArrowheadStyle` / `EndArrowheadStyle` (`msoArrowheadNone`, `msoArrowheadTriangle`), `Fill.Transparency` (0-1), `Rotation` (degrees clockwise), `IncrementRotation(n)`, `Flip(msoFlipHorizontal / msoFlipVertical)`, `HorizontalFlip`, `VerticalFlip`, `LockAspectRatio` (pictures `msoTrue` by default: corner drags keep the proportions), `ScaleHeight(Factor, RelativeToOriginalSize, Scale)`, `ScaleWidth(...)`, `AlternativeText`, `PictureFormat.CropLeft` / `CropTop` / `CropRight` / `CropBottom` (points), `Locked` (a protected sheet, unless `Protect(DrawingObjects=False)`, keeps a locked shape from being moved, sized or formatted), `PrintObject` (Format Shape > Properties > Print object; Excel's `DrawingObject.PrintObject`: False leaves it out of printouts and PDFs), [YC] `GroupId`, `Ungroup()` |
| `Shape` (Format Shape > Effects) | `Shadow` (`Visible`, `Style` `msoShadowStyleOuterShadow` / `msoShadowStyleInnerShadow`, `ForeColor.RGB`, `Transparency` 0-1, `Size` %, `Blur`, `OffsetX`, `OffsetY` pt), `Glow` (`Radius` pt, `Color.RGB`, `Transparency`), `SoftEdge` (`Radius`, `Type` `msoSoftEdgeType1`-`6`), `Reflection` (`Type` `msoReflectionType1`-`9`, `Transparency`, `Size` %, `Blur`, `Offset`), `ThreeD` (`BevelTopType` / `BevelBottomType` `msoBevelCircle`..., `BevelTopInset` / `BevelTopDepth`, `BevelBottomInset` / `BevelBottomDepth`, `RotationX` / `RotationY` / `RotationZ`, `FieldOfView`, `SetPresetCamera(n)`), `ShapeStyle` (`msoShapeStylePreset1`-`42`: Theme Styles; the Subtle / Moderate / Intense Effect rows take the theme's effects) |
| `Workbook.Theme.ThemeEffectScheme` | Page Layout > Themes > Effects: `Load(name)` ("Office", "Subtle Solids", "Banded Edges", "Smokey Glass", "Glow Edge", "Grunge Texture", "Frosted Glass", "Top Shadow", "Inset", "Milk Glass", "Riblet", "Reflection", "Extreme Shadow", "Glossy", "Office 2007 - 2010", or an .eftx file named so), `Name`, [YC] `BuiltinSchemes` |
| `Shape` (selection, clipboard) | `Select(Replace=True)` (as a click; `Replace=False` adds it, as Shift+click), `Copy()`, `Cut()`, `Duplicate()` (returns the copy, 7.5 pt down and right); `Worksheet.Paste()` pastes copied shapes (on any sheet) |
| `Shapes.Range(["A", "B"])` -> `ShapeRange` | `Count`, `Item(i)`, iteration, `Select()`, `Group()` (the shapes move and size together; a click on one selects the group), `Ungroup()`, `Align(msoAlignLefts / msoAlignCenters / msoAlignRights / msoAlignTops / msoAlignMiddles / msoAlignBottoms, RelativeTo)`, `Distribute(msoDistributeHorizontally / msoDistributeVertically, RelativeTo)`; `Shapes.SelectAll()` |
| `Range.SparklineGroups` | `Add(Type, SourceData)` (`xlSparkLine`, `xlSparkColumn`, `xlSparkColumnStacked100` = win / loss; one sparkline per data row when the range is a column, per data column when it is a row), `Count`, `Item(index)`, iteration, `Clear()` (the sparklines in this range), `ClearGroups()` (whole groups), `Group(Location)` (into the group at Location, which keeps its look), `Ungroup()` |
| `SparklineGroup` | `Type`, `Location`, `SourceData`, `Count`, `SeriesColor.Color`, `LineWeight`, `Points` (`Highpoint`, `Lowpoint`, `Firstpoint`, `Lastpoint`, `Negative`, `Markers`, each with `Visible` and `Color.Color`), `Axes.Horizontal.Axis.Visible` / `.Color.Color`, `Axes.Horizontal.RightToLeftPlotOrder`, `Axes.Vertical.MinScaleType` / `MaxScaleType` (`xlSparkScaleGroup` 1, `xlSparkScaleSingle` 2, `xlSparkScaleCustom` 3), `Axes.Vertical.CustomMinScaleValue` / `CustomMaxScaleValue`, `DisplayBlanksAs` (`xlNotPlotted`, `xlZero`, `xlInterpolated`), `DisplayHidden`, `DateRange`, `ModifyDateRange(DateRange)`, `Modify(Location, SourceData)`, `ModifyLocation(Location)`, `ModifySourceData(SourceData)`, `Delete()` |

```python
ws = Application.ActiveSheet   # this sheet, even if another one becomes active
p = ws.Shapes.AddPicture(r"C:\data\logo.png", False, True, 10, 10)    # its own size
b = ws.Shapes.AddShape(msoShapeRoundedRectangle, 10, 80, 120, 40)
b.TextFrame2.TextRange.Text = "Refresh"
b.OnAction = "refresh_quotes"
g = ws.Range("H2:H20").SparklineGroups.Add(xlSparkLine, "B2:G20")
g.Points.Highpoint.Visible = True
```

---

## 14B. UserForms

A UserForm is a Python class: add MSForms controls in `UserForm_Initialize` (or `__init__`) and write the events as
methods named like Excel's. YCGrid draws it as a dialog (it isn't saved with the workbook).

```python
from yc.excel import *

class OrderForm(UserForm):
    def UserForm_Initialize(self):
        self.Caption = "New order"
        self.Width, self.Height = 260, 170
        t = self.Controls.Add("Forms.TextBox.1", "txtQty")
        t.Left, t.Top, t.Width = 80, 10, 150
        self.Controls.Add("Forms.CommandButton.1", "btnOK").Caption = "OK"

    def btnOK_Click(self):
        Range("A1").Value = self.txtQty.Value   # Me.txtQty
        Unload(self)

def ShowOrder():          # a macro
    OrderForm().Show()    # modal: returns after Hide / Unload, handling the form's events meanwhile
```

| Member | Description |
|---|---|
| `UserForm(Caption=None, Width=240, Height=180)` | base class; `Caption`, `Width`, `Height` (points), `Visible` |
| `Show(Modal=vbModal)` | `vbModal` waits until the form is hidden or unloaded (events keep running); `vbModeless` returns at once |
| `Hide()`, `Unload(form)` / `form.Unload()` | hide (the values stay readable) / close for good (`UserForm_Terminate`) |
| `Controls.Add(bstrProgID, Name=None, Visible=True)` | `Forms.CommandButton.1`, `Forms.Label.1`, `Forms.TextBox.1`, `Forms.ComboBox.1`, `Forms.ListBox.1`, `Forms.CheckBox.1`, `Forms.OptionButton.1`, `Forms.ToggleButton.1`, `Forms.SpinButton.1`, `Forms.ScrollBar.1`, `Forms.Frame.1`; controls without `Left` / `Top` are stacked down the form |
| `Controls(Index)`, `Controls.Count`, `for c in Controls`, `Controls.Remove(Index)`, `form.<Name>` | the controls (0-based index, like MSForms) |
| control | `Name`, `Caption`, `Left`, `Top`, `Width`, `Height`, `Enabled`, `Visible`, `Value` (text; `True` / `False` for check, option and toggle buttons; a number for spin buttons and scroll bars), `Text`, `List`, `ListCount`, `ListIndex` (0-based, -1 = none), `AddItem(Item, Index)`, `RemoveItem(Index)`, `Clear()`, `Min`, `Max`, `GroupName` (option buttons), `SetFocus()` |
| events | `<Control>_Click`, `<Control>_Change`, `UserForm_Initialize`, `UserForm_Activate`, `UserForm_QueryClose(CloseMode)` (the close box; return True to keep the form open), `UserForm_Terminate` |

- Events run in the workbook's Python host (macros, buttons, ribbon callbacks). A script started outside the workbook
  can show a form and read its values after the user closes it, but receives no events.

## 15. Control events

Event functions go in the module of the control's sheet (`sheets/<CodeName>.py`) and have no parameters: buttons `<name>_Click()`; combo boxes `<name>_Change()`, `<name>_Click()`, `<name>_DropButtonClick()`. Details in `events-and-controls.md` §6.

---

## 16. Python-specific constructs

**`@udf`**: makes a function usable in cell formulas, like a Public Function in an Excel standard module.

- Usage: `@udf` or `@udf(name=None, volatile=False, vectorize=False, thread_safe=False, async_=False, accept_errors=False, nan_as=None)`.
- `vectorize=True`: every parameter receives a numpy array of length N and it returns an array of length N; called once per batch of cells.
- Parameters annotated as `Range` receive a read-only snapshot Range; parameters annotated as `float`, `str` etc. receive scalars.
- UDFs must not modify the workbook. Inside a UDF `Application.Caller` is the calling cell.

**`@stream(name)`**: a stream provider; cells write `=STREAM("NAME", args...)`.

- The class implements `subscribe(self, topic)` and `unsubscribe(self, topic)`.
- Cells reach it with `=STREAM("NAME", topic1, ...)`; `topic.Args` is the tuple of the topics. `topic.Push(value)`
  updates the cells. There is no `=RTD`: YCGrid doesn't talk to COM RTD servers, a sheet from Excel writes
  `=STREAM("prog", topic...)` for `=RTD("prog", "", topic...)`.
- `topic.Id` is the topic's key. After Python is reloaded the formulas subscribe again with the new host.

**`Application.Topics`** [YC]: the real-time market data hub.

| Member | Description |
|---|---|
| `Register(Names)` | register names, returns a list of ids (one id for a single name); already registered names return their existing ids |
| `Push(Ids, Values, TimeNs=None)` | push the latest values; single values or arrays; any thread; never blocks. [YC] `TimeNs`: the source's time (ns since the epoch; one number or one per value), given to subscribers and live plots |
| `Subscribe(Names, OnTick, Conflate=False, MaxBatch=1000, Capacity=1 << 20)` | [YC] host only: every update of these topics, in order, to `OnTick(batch)` on the subscription's own worker thread (a slow callback delays only itself). `batch` is a list of `Tick` (`topic` id, `value`, `time_ns`, `seq`; `.name`), unpacking as `for topic_id, value, time_ns, seq in batch`. `Conflate=True`: the latest per topic about every 20 ms. Replays feed it too. Returns a `TopicSubscription`: `Id`, `Names`, `Delivered`, `Dropped` (lost when a queue of `Capacity` was full: a gap in `seq`), `Errors`, `LastError`, `Active`, `Unsubscribe()` |
| `Subscriptions` | [YC] this host's `TopicSubscription` objects |
| `SetError(Id, ErrorText)` | e.g. `"#CONNECT!"` |
| `Lookup(Name)` | name -> id; None when it doesn't exist |
| `Count` | number of registered topics |
| `Stats` | dict: `UpdatesPerSecond` (since the previous call), `TotalUpdates`, `Topics`, `BoundCells`, `Subscriptions` (per subscription: `Id`, `Topics`, `Conflate`, `Queued`, `Delivered`, `Dropped`, `Capacity`) |

Cells show a topic through `Range.BindTopics` or `=QUOTE("name")`. Heavy market data should go in a separate data feed process (`feeds\*.py`, registered under `[[python.feeds]]` in `ycgrid.toml`; YCGrid starts it with the host, restarts it when it exits and passes `YCGRID_PID` so `yc.connect()` finds this YCGrid) that does `Register`, `BindTopics` and `Push` (over 1 million cell updates per second, see the README). The host gets the same id with `Lookup`.

**Research and trading modules** [YC] (snake_case, outside the Excel object model):

| Module | Members | Reference |
|---|---|---|
| `yc.data` | `bind`, `table`, `tables`, `BoundTable.sync(df, delete_missing=True)` (send only the difference against the previous snapshot), `read(name)` (any table as a DataFrame through shared memory), `read_range(rng, header)`, `write_frame(sheet, df, at, header, index)` (also `Range.to_frame()` and `Worksheet.write_frame()`), `BoundTable`, `number_format`, `encode_frame`, `decode_frame` | data-binding.md |
| `yc.replay` | `record(path=None, tables=None)`, `stop_recording()`, `play(path, speed=1, start=0, loop=False)`, `pause()`, `resume()`, `seek(seconds)`, `set_speed(speed)`, `set_loop(loop)`, `live()`, `status()`, `is_replaying()`, `wait(timeout)` | data-binding.md |
| `yc.plot` | `figure(title, rows, cols, size, link_x, pos, theme, sync_cursor, key, reuse, persist, height_ratios, width_ratios)` -> `Figure` (`fig[i]` / `axes(i)`, `link_table(table, column)`, `cursor(x)` (the group's synced crosshair), `on(event, cb, right=False)`, `context_menu(items)`, `input(action, ...)`, `fit()`, `close()`); `figures()`, `close(key=)`; `Axes` (`set`, `line`, `step`, `scatter`, `bar`, `hist`, `fill_between`, `candles`, `heatmap`, `surface`, `hline`, `vline`, `text`, `signals` (buy / sell marks on candles), `clear`); `Item` (`set_data`, `append`, `style`, `remove`); `line`, `scatter`, `hist`, `bar`, `candles`, `heatmap`, `close_all` | plot.md |
| `yc.trading` | `Order` (`type`, `stop`), `Fill` (`fee`), `Gateway` (`supports`, `submit`, `cancel`, `modify`, `last_price`, `positions`, `account`, `open_orders`, `connected`), `PaperGateway` (`spread_ticks`, `slippage_ticks`, `fill_ratio`, `queue`, `set_speed`, `pause`, `resume`, `quote(symbol, bid, ask, last=None)` for options and other contracts), `RiskLimits`, `Rejected`, `OrderManager` (`buy`, `sell`, `submit(type=, stop=)`, `modify`, `cancel`, `cancel_all`, `flatten`, `order`, `position`, `avg_cost`, `orders`, `fills`, `on_order`, `on_fill`), `check_gateway`; `yc.gateways.template` (a broker gateway skeleton) | screens.md |
| `yc.bars` | `BarBuilder(seconds, keep, fill_gaps)` (`add`, `all`, `frame`, `draw`), `Bar` | screens.md |
| `yc.analysis` | the Analysis ToolPak's procedures with Excel's arguments (`Descr`, `Regress`, `Ttest2`...) | §13A2 |
| `yc.notebook` | the code cells workbooks kept from the former Notebook window (the notebook was removed, ADR 0011): `cells()`, `export_ipynb(path)` (a plain `.ipynb`) | |
| `yc.safety`, `yc.interact`, `yc.alerts`, `yc.sim` | parameters, heartbeats, kill switch; click actions and hotkeys; alert rules; the market simulator | safety.md, screens.md |

**Every UI command from Python** [YC]: [ui-coverage.md](ui-coverage.md) lists each ribbon button, menu item and
shortcut with the members that do the same (generated from `schema/commands.yaml`, checked by
`scripts/check_commands.py`). New members for that coverage: `Range.DataSeries(Rowcol, Type, Date, Step, Stop, Trend)`
(Fill > Series: `xlDataSeriesLinear`, `xlGrowth`, `xlChronological` with `xlDay` / `xlWeekday` / `xlMonth` / `xlYear`,
`xlAutoFill`; `Trend=True` with Linear / Growth writes the best-fit line or curve through the values at the start of each
row / column over the whole range, the starting values too), `Range.InsertIndent(InsertAmount)`, `Range.ClearOutline()`, `Range.CopyPicture()` (the range's look to
the Clipboard as a picture), `Range.CheckSpelling()` (the Spelling dialog over the range), `Worksheet.Paste(Destination, Link=False)` (`Link=True`: Paste Link, formulas referring to the copied cells),
`Workbook.FollowHyperlink(Address, SubAddress)`, `PivotTable.ChangePivotCache(cache)`, `Window.NewWindow()`,
`Windows.Arrange()`. `yc.ui`: `pane(name, visible=None)` shows, hides or reads a built-in pane (`"immediate"`,
`"watch"`, `"errors"`, `"clipboard"`, `"comments"`, `"pivot_fields"`, `"latency"`, `"alerts"`, `"heartbeats"`,
`"change_log"`, `"chart_window"`, `"perf"`, `"formula_bar"`), `panes()` (each with whether it shows),
`qat(commands=None)` (the Quick Access Toolbar's command ids, read / set), `commands()` (every command: id, label,
shortcut, toggle, pressed, enabled, `ribbon`: shown as a button on a built-in ribbon tab, `icon`: that button has an
icon) and `command(id)` (run one, like `CommandBars.ExecuteMso`; False when the command doesn't exist or is
disabled); `design_mode()` reads Design Mode (`Application.DesignMode` reads and sets it).

**Screenshots to share** [YC]: Home > Share > Screenshot (Ctrl+Shift+S, also on the Quick Access Toolbar and the cell
and sheet tab menus) copies a picture of the selection (one cell: its table or data region); the arrow takes a table,
the whole sheet, the whole workbook (every visible sheet under its name), the visible area, the YCGrid window or one
of its floating windows (plot figures, the Latency Monitor, panes). Cells are drawn by the print renderer, so a range
larger than the screen comes out whole and without selection marks; options: row and column headings, 2x resolution.
A preview offers Save As (PNG), Copy as File (a PNG file for chats and mail that take files) and opens the picture in
the viewer. From Python: `Range.ExportImage(Filename=None, Headings=False, Gridlines=None, Scale=1.0)`,
`Worksheet.ExportImage(...)`, `Workbook.ExportImage(...)` (a PNG file, or the Clipboard when Filename is None; returns
`width`, `height`, `path`, `note`, `scaled_down`; at most 3,000 rows x 200 columns per sheet, and very large pictures
are drawn at a lower resolution); `yc.ui.windows()` lists the windows (label, name, `rect` = [left, top, right,
bottom] in the window pixels `ui.input` takes, `docked`; a docked one also `tab`, the rect of its own tab, and
`tab_selected`, and `floating` when its group floats over the sheets; the hidden tabs of a group are listed with `visible`
False), `yc.ui.cell_rect("B2:C4")` where a range of the active sheet is
in those pixels (scrolled into view first; for drags a person makes: a selection's border, the fill handle, a column's
border in the header row) and
`yc.ui.screenshot(path=None, target="window:<label>")` takes one of them (or `"window"`, `"grid"`, `"range"`) as a PNG
or to the Clipboard. For tests that click what a person clicks: `yc.ui.ribbon_items()` (the ribbon buttons, open
menu and submenu entries, gallery tiles and Backstage entries on the screen: `cmd`, `where`, `rect`),
`yc.ui.items(window="")` (the controls drawn in the last frame: `label`, `id`, `window`, `rect`, `disabled`; with a
window filter its unlabelled controls too) and `yc.ui.answer_file_dialogs(*paths)` (what the next file dialogs of a
hidden YCGrid choose; `""` cancels); see testing.md.

**Error log** [YC] (View > Error Log): every error YCGrid shows or receives - message boxes, Python's errors and
tracebacks, the Python host or VS Code failing - newest first, with a red count on the status bar until it is opened;
also appended to a daily file (`errors-YYYY-MM-DD.log`, settings `errors.log_to_file` / `errors.log_folder`).
`yc.ui.errors(errors_only=False, after=0)` returns its entries (dicts: `id`, `time`, `source`, `title`, `text`,
`error`; `error` is False for an ordinary message box), `yc.ui.clear_errors()` empties the window,
`yc.ui.resolve_errors(ids, note="")` turns errors that no longer apply into messages with the note added (they
leave the red count; YCGrid does it itself for `No module named ...` once the module is installed),
`yc.ui.errors_folder()` is the files' folder, `ui.pane("errors")` shows it. In any dialog, Ctrl+C outside a text box
copies all its text; message boxes have selectable text and a Copy button. When the window stops responding for 10
seconds, or YCGrid crashes, a dump with every thread's stack (`ycgrid-hang-*.dmp` / `ycgrid-crash-*.dmp`) is written to
the same folder (hidden instances: `%TEMP%\ycgrid-dumps`) and the hang is listed here: send it with what was running.

**Immediate window** [YC]: completes Python as you type (variables, the Excel object model, members after a dot,
builtins); Tab or Enter inserts, Up / Down choose, Esc closes; without the list Up / Down walk the history. Only
side-effect-free expressions are evaluated to find members (names, attributes, literal subscripts, and calls of
capitalized Excel accessors such as `Range("A1")` with literal arguments).

**VS Code on the live YCGrid** [YC]: Developer > Debug in VS Code (`yc.debug.start(port=5678, folder=None)`, run in the host:
returns `{port, launch_json, folder}`; another port when 5678 is taken; `yc.debug.listen(port)` only starts the debug
server, `yc.debug.write_launch_json(folder, port)` only writes the configuration; debugpy is installed when missing)
listens for VS Code's debugger and writes an "Attach to YCGrid" configuration: breakpoints in strategies, handlers and
UDFs while the window keeps drawing. Saving a project file swaps its functions and classes in the running host (live
references and existing objects get the new code; the file's other top-level code doesn't run again; new or deleted
files restart the host; `editor.restart_on_save` restarts instead).

**`=PY(code, range1, ...)`** [YC]: Python in a cell. The code runs in the host's shared namespace (what a VS Code cell
defined is there); `x` is the first range (a table with a header row becomes a DataFrame, numbers a numpy array), `_1`,
`_2`... each range, `args` all of them; several statements are separated by `;`. The result spills: a DataFrame with its
header (row labels as a first column), a Series as a column, arrays, dicts as two columns, a scalar in one cell. It
recalculates when its ranges change; UDFs that return arrays spill too.

```text
=PY("x.describe().round(2)", A1:C251)
=PY("import numpy as np; r = x.pct_change().dropna(); np.polyfit(r.TXF, r.Stock, 1)[0]", A1:C251)
```

**Data View** [YC] (`yc.data`): `save_columns(data, folder=None)` writes a DataFrame / dict / Parquet, Feather or CSV
path as one `.npy` per column and returns `{column: path}`; `view(data, title=None)` opens a window over all the rows
(tens of millions), read by C++ from those files: virtual scrolling, header click to sort, a filter row (`>100`, `<=5`,
`=7`, `!=0`, `a..b`, text contains), column statistics, To Sheet (the shown rows, at most 1,048,575). The returned
`DataView` has `filter(column, expr)`, `sort(column, descending)`, `state()` (`rows`, `shown`, `first`) and `close()`.
`Axes.plot_files(kind, **columns)` draws an item whose columns C++ reads from such files.

**Feeds** [YC]: `yc.feeds.Feed(table=None, key=None, interval=0.1, heartbeat=3.0, reconnect=(0.5, 1, 2, 5, 10),
on_rows=None)` is the base of data feeds: subclasses implement `run_source()` (blocking; return or raise to reconnect,
return when `self.stopping`) and call `emit(row)` (a dict; with a key the newest row per key wins until the next
publish). `start()` runs it on its own threads, `stop(timeout=2.0)` from any thread, `flush()` publishes what is
pending now (returns the rows), `status()` is `{name, connected, rows_in, rows_out, reconnects, last_error}`; a
heartbeat `"feed <name>"` (the class attribute `name`) beats in `yc.safety` while it is connected. `CsvTailFeed(path, table=None, key=None, convert=None,
from_start=False, poll=0.05)` follows a CSV another program appends to; `WebSocketFeed(url, table=None, key=None,
parse=None, subscribe=(), headers=None)` reads JSON messages (needs the `websockets` package). `yc.trading.check_gateway(gw, symbol, far_price)` runs the conformance checks a
gateway should pass (paper accounts). Screens and research maths are built from the pieces: [screens.md](screens.md)
and the examples (ADR 0012).

**Time machine** [YC] (`yc.replay`): `record(path=None, everything=True)` (Trading > Record > Record Everything) records
every bound table, the app's own included, the topics, the `yc.plot` figures and markers; `mark(text, kind="note")`
adds a marker (the OrderManager marks its orders and fills, alert rules their alerts); the replay bar shows the
markers (click one to go there); `pin()` remembers the tables at the current moment and `compare(limit=500)` lists what
changed since ([{table, key, column, then, now, change}]; the replay bar's Pin This Moment / Compare).
Plain `record()` (Trading > Record) takes the market-data tables (`bind(record=True)`, the default); when no table
is marked as market data it records the whole screen as `everything=True` does, and when there is nothing live to
record (no bound table, `=QUOTE()` cell or figure) it raises instead of writing an empty file. A recording that got
no data at all is removed when it stops.

**`SheetModule(name)`**: gets this sheet (`Me`) in a sheet module.

---

## 16A. Charts: ChartObjects, Chart, Series [partly YC]

`Chart.Shapes`: shapes drawn on the chart (`AddLine`, `AddShape`, `AddTextbox`; points from the chart's corner, and their `Left` / `Top` read from it too): they move with it, go when it is deleted and aren't among `Worksheet.Shapes` (Excel 16, measured). `Chart.DisplayValueNotAvailableAsBlank` (#N/A drawn as an empty cell; True in a new chart), `Chart.ExportAsFixedFormat(xlTypePDF, Filename)` (the chart in a one-page PDF), `ChartObject.TopLeftCell`, `BottomRightCell`, `CopyPicture()`. `Axis.AxisGroup`, `DisplayUnitCustom` (DisplayUnit then reads `xlCustom`), `HasDisplayUnitLabel`, `DisplayUnitLabel.Text` ("Thousands", "x 250"), `TickMarkSpacing` (a tick mark every n categories), `CategoryNames`.

```python
ch = Me.Shapes.AddChart2(-1, xlLine, Left=Me.Range("H2").Left, Top=Me.Range("H2").Top, Width=480, Height=260).Chart
ch.SetSourceData(Me.Range("A1:C500"))
ch.HasTitle = True
ch.ChartTitle.Text = "TXF"
s = ch.SeriesCollection.NewSeries()
s.Name = "Last"
s.SetData([], Capacity=100_000)                                         # [YC] keep the newest 100,000 points
s.AppendData([23015.0])                                                 # [YC] streaming: call it as ticks arrive
ch.Interactive = True                                                   # [YC] pan and zoom
```

| Object | Members |
|---|---|
| `ChartObjects` | `Add(Left, Top, Width, Height)`, `Count`, `ChartObjects(Index)`, `for c in ChartObjects` |
| `ChartObject` | `Name`, `Chart`, `Left`, `Top`, `Width`, `Height`, `Placement`, `LockAspectRatio`, `Activate()`, `Select()`, `BringToFront()`, `SendToBack()`, `Delete()`, `Copy()` / `Cut()` (Ctrl+C / Ctrl+X on the chart; `Worksheet.Paste()` pastes it, on any sheet), `Duplicate()` (Ctrl+D: returns the new ChartObject); `PrintObject`, `Locked` (Format Chart Area > Properties); [YC] `Floating` (True: the chart in its own dockable window, which can go into a container; False puts it back on the sheet). `Visible` is P2. A new chart goes in the middle of the window, as in Excel |
| `Chart` | `ChartType` (`XlChartType`), `SetSourceData(Source, PlotBy=xlColumns)` (`Source` may have several areas: `"A1:A20,C1:C20"` makes X from A and Y from C, as Ctrl+selected columns do), `SeriesCollection`, `Axes(Type, AxisGroup=xlPrimary)`, `HasTitle`, `ChartTitle`, `HasLegend`, `Legend`, `ApplyDataLabels(ShowValue=True)`, `ChartGroups(1)` (`GapWidth`, `Overlap`, `FirstSliceAngle`, `DoughnutHoleSize`, histogram bins); [YC] `Interactive`. `Parent`, [YC] `MaxPoints` and `Chart.AppendData` are P2 (append with `Series.AppendData`) |
| `ChartGroup` | pie of pie / bar of pie: `SplitType` (`xlSplitByPosition`, `xlSplitByValue`, `xlSplitByPercentValue`, `xlSplitByCustomSplit`), `SplitValue`, `SecondPlotSize`, `GapWidth` (between the plots; [YC] `PieGapWidth` is the same gap); stock charts: `HasUpDownBars`, `HasHiLoLines`, `UpBars` / `DownBars` (`Format.Fill.ForeColor.RGB`, `Interior.Color`, `Select()`, `Delete()`), `HiLoLines` (`Format.Line.ForeColor.RGB`, `Border.Color`, `Select()`, `Delete()`); [YC] `ShowConnectorLines` (waterfall: Format Data Series > Show connector lines; True by default); histogram / Pareto bins: `BinsType` (`xlBinsTypeAutomatic`, `xlBinsTypeBinSize`, `xlBinsTypeBinCount`), `BinWidthValue`, `BinsCountValue`; Chart Filters: `FullCategoryCollection()` (every category: `Count`, `(Index or name).Name`, `.IsFiltered` read / write), `CategoryCollection()` (the categories shown) |
| `SeriesCollection` | `NewSeries()`, `Count`, `SeriesCollection(Index)`, `for s in SeriesCollection` |
| `Series` | `Name`, `Values`, `XValues` (read: the values as a tuple, as Excel gives them; write: a range or its address), `Formula` (`=SERIES(...)`: the ranges), `ChartType` (combination charts: line, line with markers, clustered / stacked column, area and XY scatter series mix in one chart), `AxisGroup` (`xlPrimary`, `xlSecondary`), `Format.Line` (`ForeColor.RGB`, `Weight`), `MarkerStyle`, `HasDataLabels`, `ApplyDataLabels(ShowValue=True)`, `DataLabels().Delete()`, `Trendlines()`, `ErrorBar(Direction=xlY, Include=xlErrorBarIncludeBoth / PlusValues / MinusValues, Type, Amount, MinusValues)` (`xlErrorBarTypeCustom`: `Amount` / `MinusValues` are ranges, lists or numbers), `HasErrorBars`, `ErrorBars` (`Type`, `Amount`, `MinusValues`, `Include`, `EndStyle` (`xlCap`, `xlNoCap`), `Delete()`), `ErrorBar(Direction=xlX, ...)` on a scatter chart: X error bars ([YC] `XErrorBars` reads them), `Smooth` (line and scatter: a smoothed line), `Points(Index).IsTotal` (waterfall totals), `IsFiltered` (Chart Filters: left out of the chart, keeping its place and formatting), `Delete()`; [YC] `ShowInnerPoints`, `ShowOutlierPoints`, `ShowMeanMarkers`, `ShowMeanLine`, `QuartileCalculationInclusiveMedian` (box and whisker), `ParentDataLabelOption` (treemap: `xlParentDataLabelOptionsNone` / `Banner` / `Overlapping`), `GeoProjectionType` (`xlGeoProjectionTypeAutomatic`, `Mercator`, `Miller`, `Albers`, `Robinson`), `GeoMappingLevel` (`xlGeoMappingLevelAutomatic`, `DataOnly`, `CountryRegion`, `CountryRegionList`, `World`), `RegionLabelOption` (`xlRegionLabelOptionsNone`, `BestFitOnly`, `ShowAll`), `SeriesColorGradientStyle` (`xlSeriesColorGradientStyleSequential`, `Diverging`) (filled map), `SetData(x=None, y=None, Capacity=None)` (lists or numpy arrays; one argument is `y`, and `x` defaults to 1..N), `AppendData(x=None, y=None)` (streaming: points, or one point, are added and the oldest dropped beyond `Capacity`; one argument is `y`). `BindTopic(Topic, Capacity=100000)` is P2 (it raises `UnsupportedError`: call `AppendData` from the code that receives the ticks, or use a `yc.plot` figure's `line_live(topic)`); `Unbind()` is P2 |
| `Trendlines` | `Add(Type=xlLinear, Order, Period, Forward, Backward, Intercept, DisplayEquation, DisplayRSquared, Name)` (a series may have several: `Add` adds one more), `Count`, `Trendlines(i)` |
| `Trendline` | `Type` (`xlLinear`, `xlExponential`, `xlLogarithmic`, `xlPolynomial`, `xlPower`, `xlMovingAvg`), `Order` (2-6), `Period`, `Forward`, `Backward`, `Intercept` / `InterceptIsAuto` (Set Intercept: linear, polynomial, exponential), `Name` / `NameIsAuto` (the legend's "Linear (Sales)" or a custom name), `DisplayEquation`, `DisplayRSquared`, `Index`, `Select()`, `Delete()` |
| `Axis` | `Axes(xlCategory)` / `Axes(xlValue)`: `MinimumScale`, `MaximumScale`, `MinimumScaleIsAuto`, `MaximumScaleIsAuto`, `HasTitle`, `AxisTitle` (`Text`, `Font`), `HasMajorGridlines`, `TickLabels` (`NumberFormat`, `Font`; time axes can use the [YC] `[epoch]` format), `Delete()` (hides it), `Select()`; [YC] `Visible` |
| `ChartTitle` | `Text`, `Caption`, `Font` (`Size`, `Bold`, `Color`), `Select()`, `Delete()` |
| `Legend` | `Position` (`xlLegendPositionBottom`, `Right`, `Top`, `Left`, `Corner`), `Font`, `Select()`, `Delete()` |
| `ChartArea` | `Format.Fill.ForeColor.RGB`, `Format.Line` (`ForeColor.RGB`, `Visible`), `Interior.Color`, `Border` (`Color`, `LineStyle`), `ClearFormats()`, `Select()` |
| `PlotArea` | `Format.Fill.ForeColor.RGB`, `Interior.Color`, `ClearFormats()`, `Select()` |
| `Chart` (more) | `ChartArea`, `PlotArea`, `HasAxis(xlCategory / xlValue)`, `Location(Where, Name)` (`xlLocationAsNewSheet` moves it to a new chart sheet called Name; `xlLocationAsObject` onto the worksheet Name), `Select()` |
| `Chart` (design) | `PlotBy` (`xlRows` / `xlColumns`, read / write: Switch Row/Column), `SetElement(msoElement...)` (Add Chart Element: titles, legend positions, data labels, axes, axis titles, gridlines), `ClearToMatchStyle()` (Reset to Match Style for the whole chart), `ApplyLayout(Layout, ChartType=None)` (Quick Layout 1-11), `ChartStyle` (Chart Styles: each kind of chart has its own set, numbered as Excel's: columns 201-214, bars 216-228, lines 227-241, scatter 240-254, pies 251-262, doughnuts 251-260, areas 276-286, the others 201-208; setting one replaces the series' and elements' fills, borders, lines, markers and text colors, as Excel's does; another chart type starts at its Style 1) |
| `Series` (more) | `MarkerStyle` (`xlMarkerStyleAutomatic`, `None`, `Circle`, `Square`, `Diamond`, `Triangle`, `X`, `Plus`), `MarkerSize`, `Format.Line.Weight`, `Format.Line.ForeColor.RGB`, `Select()` |
| `Charts` | the chart sheets (also `Workbook.Charts`, `Application.Charts`): `Charts.Add(Before=None, After=None, Count=1, Type=None, Name=None)` (charts the selected data on a new chart sheet, like F11), `Count`, `Charts(Index)`, iteration; also `Sheets.Add(Type=xlChart)` |
| `ChartSheet` | a chart sheet: every `Chart` member, plus `Name`, `Index`, `Type` (`xlChart`), `Visible`, `Activate()`, `Select()`, `Delete()`, `Move(Before, After)`, `Copy(Before, After)`; `Application.ActiveChart` is the active chart sheet |
| `Chart` (data) | `PlotVisibleOnly` (True, Excel's default: hidden and filtered rows / columns are left out), `DisplayBlanksAs` (`xlNotPlotted` gaps, the default; `xlZero`; `xlInterpolated` joins the line), [YC] `DisplayNAAsBlank` (Hidden and Empty Cells > Show #N/A as an empty cell; True by default), `ChartColor` (Change Colors: 10-13 colorful, 14-19 monochromatic palettes), `HasAxis(xlValue, xlSecondary)`, `Axes(xlValue, xlSecondary)` (the secondary vertical axis of the series on `AxisGroup` 2: its own automatic bounds, format and title), `Floor`, `Walls` (`BackWall`, `SideWall`: 3-D charts, `Format.Fill.ForeColor.RGB`, `Interior.Color`, `Select()`); `Axes(xlCategory).CategoryType` (`xlAutomaticScale`: dates make a date axis, stock charts too, so days without data show as gaps; `xlCategoryScale`: a text axis, one point per row; `xlTimeScale`), `BaseUnit` / `BaseUnitIsAuto` (`xlDays`, `xlMonths`, `xlYears`), `MajorUnitScale`, `MinimumScale` / `MaximumScale` as dates on a date axis; `SetElement` also takes `msoElementChartTitleCenteredOverlay`, `msoElementDataLabelCenter` ... `BestFit`, `msoElementErrorBar...`, `msoElementTrendline...`, the minor gridlines |
| `Axis` (options) | each bound on its own: `MinimumScaleIsAuto` / `MaximumScaleIsAuto` (setting `MinimumScale` or `MaximumScale` fixes only that end), `MajorUnit`, `MinorUnit` (`...IsAuto`), `ScaleType` (`xlScaleLinear`, `xlScaleLogarithmic`), `LogBase`, `ReversePlotOrder`, `Crosses` (`xlAxisCrossesAutomatic`, `xlAxisCrossesCustom` with `CrossesAt`, `xlAxisCrossesMaximum`), `DisplayUnit` (`xlThousands`, `xlMillions`...), `MajorTickMark` / `MinorTickMark` (`xlTickMarkNone`, `Inside`, `Outside`, `Cross`), `TickLabelPosition` (`xlTickLabelPositionNextToAxis`, `High`, `Low`, `None`), `HasMinorGridlines`, `MajorGridlines.Select()`, `AxisBetweenCategories`, `TickLabelSpacing`; `AxisTitle.Formula` / `ChartTitle.Formula` (`"=Sheet1!$B$1"`: the title follows the cell; `Text` reads what the chart shows) |
| `ChartTitle` (more) | a new chart has a title: one series' name linked to its cell, else "Chart Title"; `IncludeInLayout` (False: Centered Overlay), `Format.Fill`, `Format.Line`; `Legend.IncludeInLayout` (False: the legend overlays the plot area), `Legend.Format.Fill` / `Line`, `PlotArea.Format.Line`; places set by hand (points from the chart area's corner; 0 while placed automatically; setting one places the element by hand): `ChartTitle.Left` / `Top`, `Legend.Left` / `Top` / `Width` / `Height`, `PlotArea.InsideLeft` / `InsideTop` / `InsideWidth` / `InsideHeight`, `Axis.AxisTitle.Left` / `Top` |
| `Series` (format) | `Format.Fill` (`ForeColor.RGB`, `Transparency` 0-1, `Visible`), `Format.Line` (a line's color or a column's / slice's border: `ForeColor.RGB`, `Weight`, `DashStyle` `msoLineSolid` / `RoundDot` / `SquareDot` / `Dash` / `DashDot` / `LongDash`, `Visible`), `InvertIfNegative`, `InvertColor`, `MarkerBackgroundColor`, `MarkerForegroundColor`, `Explosion` (pies), `BubbleSizes` (`xlBubble`), `ApplyDataLabels(Type, ..., ShowSeriesName, ShowCategoryName, ShowValue, ShowPercentage, Separator)` |
| `DataLabels` | `Series.DataLabels` (or `DataLabels()`): `ShowValue`, `ShowSeriesName`, `ShowCategoryName`, `ShowPercentage`, `ShowRange` (Value From Cells: `Format.TextFrame2.TextRange.InsertChartField(msoChartFieldRange, "=Sheet1!$C$2:$C$5")`), `Separator`, `Position` (`xlLabelPositionCenter`, `InsideEnd`, `InsideBase`, `OutsideEnd`, `Left`, `Right`, `Above`, `Below`, `BestFit`), `NumberFormat`, `NumberFormatLinked`, `Select()`, `Delete()`; `Trendline.Select()`, `ErrorBars.Select()` |
| `Point` | `Series.Points(i)`: `Format.Fill.ForeColor.RGB`, `Format.Line.ForeColor.RGB`, `Explosion` (a pie slice pulled out), `HasDataLabel`, `IsTotal` (waterfall), `MarkerBackgroundColor`, `Select()` (Format Data Point); `ChartGroups(1).VaryByCategories` (a color per slice), `BinsOverflowEnabled` / `BinsOverflowValue`, `BinsUnderflowEnabled` / `BinsUnderflowValue`, `BinsType = xlBinsTypeCategorical` (Pareto) |
| Chart text (fonts) | every chart `Font` (`ChartTitle.Font`, `Legend.Font`, `AxisTitle.Font`, `TickLabels.Font`, `DataLabels().Font`, `LegendEntry.Font`): `Name`, `Size`, `Bold`, `Italic`, `Underline` (`xlUnderlineStyleSingle` / `None`), `FontStyle`, `Color`; each axis has its own `TickLabels.Font` (Format Axis > Text Options). Home > Font (Bold, Italic, Underline, font, size, color, Increase / Decrease Font Size, Ctrl+B / I) acts on the selected chart element; on the chart area, on all of its text |
| `Axis` (format) | `Format.Line` (`ForeColor.RGB`, `Visible`, `Weight`: the axis line), `TickLabels.NumberFormatLinked` (Linked to source), `TickLabels.Offset` (Label distance from axis, 0-1000), `TickLabels.MultiLevel` (Multi-level Category Labels: several category columns, or a PivotChart's row fields, drawn as rows of grouped labels with lines between the groups; on by default), `MinorGridlines.Select()` |
| `Series` (fills) | `Format.Fill.Type` (`msoFillSolid`, `msoFillGradient`, `msoFillPicture`, `msoFillPatterned`), `TwoColorGradient(Style, Variant)` with `ForeColor` / `BackColor`, `GradientAngle`, `Patterned(Pattern)` (`msoPattern5Percent` ... `msoPatternDiagonalCross`) with `ForeColor` / `BackColor`, `Pattern`, `UserPicture(PictureFile)` (stretched), `UserTextured(TextureFile)` (tiled), `Solid()`; `Format.Line.Style` (compound: `msoLineSingle`, `msoLineThinThin`, `msoLineThinThick`, `msoLineThickThin`, `msoLineThickBetweenThin`), `BeginArrowheadStyle` / `EndArrowheadStyle` (`msoArrowheadNone` ... `Oval`); `ChartGroups(1).VaryByCategories` on one series of columns or bars: a color per point (its legend lists the categories); `HasLeaderLines` |
| `DataLabels` (more) | `ShowLegendKey`, `ShowLeaderLines` (a line back to a label dragged away), `Font`, `DataLabels(i)` / `Count`: one label, `DataLabel` (`Select()` = Format Data Label, `Delete()`, `Font`, [YC] `Offset` (dx, dy): how far it was dragged, fractions of the chart). A second click on the selected labels selects one; dragging it moves it |
| `LegendEntry` | `Legend.LegendEntries(i)` / `.Count` (a pie's or a vary-colors chart's entries are its categories): `Select()` (Format Legend Entry; also a second click on the selected legend), `Delete()` (a series' entry leaves the legend, the series stays), `Font` (the entry's own font: Name, Size, Bold, Italic, Underline, Color; Home > Font on a selected entry sets it) |
| `DataLabel` | `Series.DataLabels(i)` / `Point.DataLabel`: `Text` / `Caption` (its own text; reads what it shows), `Formula` (`"=Sheet1!$E$1"`: it shows that cell, also typed with = in the formula bar), `Font` (its own), `ShowValue`, `ShowSeriesName`, `ShowCategoryName`, `ShowPercentage`, `ShowLegendKey`, `Position` (its own over the series'), `Select()`, `Delete()`, [YC] `Offset`. A click into the selected label types its text in place |
| `FillFormat` (texture) | `TextureTile`, `TextureOffsetX` / `TextureOffsetY` (points), `TextureHorizontalScale` / `TextureVerticalScale` (1 = the picture's size), `TextureAlignment` (`msoTextureTopLeft` 0 ... `msoTextureBottomRight` 8), [YC] `TextureMirrorType` (0 none, 1 horizontal, 2 vertical, 3 both); tiles columns, slices and areas alike |
| `Chart` (pictures) | `CopyPicture()` (the chart as it looks, to the Clipboard), `Export(Filename)` (a PNG); `ChartObject.Copy()` then `Worksheet.Pictures.Paste()` (Home > Paste > Picture, Paste Special > Picture (PNG)): the chart as a picture; cells copied and pasted with the chart selected become new series |

- Clicking a chart selects the element under the mouse (title, legend, axis, series, plot area, chart area) like Excel; `Select()` on an element does the same from Python. The Format pane (double-click, or Format > Format Selection) formats the selected element, Delete removes it, and Chart Design > Add Chart Element turns elements on and off.

- More chart types: 100% stacked column, stacked and 100% stacked bar, stacked / 100% stacked line (with or without markers) and area, scatter with straight or smooth lines (with or without markers), bubble (X, Y and sizes), high-low-close stock (`xlColumnStacked100`, `xlBarStacked`, `xlBarStacked100`, `xlLineStacked`, `xlLineStacked100`, `xlLineMarkersStacked`, `xlLineMarkersStacked100`, `xlAreaStacked`, `xlAreaStacked100`, `xlXYScatterLines`, `xlXYScatterLinesNoMarkers`, `xlXYScatterSmooth`, `xlXYScatterSmoothNoMarkers`, `xlBubble`, `xlStockHLC`); a doughnut has a ring per series. Insert > Charts shows each kind's variants in a gallery, Recommended Charts suggests some from the data, and Insert Chart previews the data by rows and by columns.
- Chart types: line, column, bar, XY scatter, area, pie, doughnut, stock, radar (plain, with markers, filled), waterfall, histogram, Pareto, box and whisker, treemap, sunburst and funnel; 3-D column (`xl3DColumn`, `xl3DColumnClustered`, `xl3DColumnStacked`), 3-D bar (`xl3DBarClustered`), 3-D line, 3-D area, 3-D pie, surface (`xlSurface`, `xlSurfaceWireframe`) and contour (`xlSurfaceTopView`); filled maps (`xlRegionMap`): the categories name countries, or states / provinces / counties (in English, the local language, Chinese, Japanese, Korean, German, French or Spanish, or ISO codes; a second category column such as the country decides names that several countries use); numbers color the regions on a two-color scale, text colors them by category. The shapes come with YCGrid (Natural Earth), so maps work offline. Treemaps and sunbursts read the leading text columns of the source as category levels (a blank cell repeats the parent above). Error bars are Y error bars (both directions, or plus / minus only); standard deviation bars sit at the series mean, as in Excel.
- 3-D view: `Chart.Rotation` (around the vertical axis), `Elevation`, `Perspective`, `DepthPercent`, `RightAngleAxes`; also on the Format pane of the chart area (3-D Rotation). `ChartArea.ClearContents()` / `Clear()` empty a chart (Delete on a chart sheet does the same).
- Charts are drawn with ImPlot and look like Excel's default style (to be calibrated). Series with more than 100,000 points are downsampled automatically, so zooming stays smooth.
- Don't write large or high-frequency data into cells just to chart it: use `SetData` / `AppendData`, or a `yc.plot`
  figure (`line_live(topic)` draws a topic without Python in the loop).

---

## 16A2. RibbonX: customUI.xml

Put `customUI.xml` (or `customUI14.xml`) in the project folder, in Excel's customUI format; YCGrid reloads it whenever the file changes.
Callbacks are Python functions named in the XML (`onAction="Ribbon.OnStart"` = `modules/Ribbon.py`, or just `"OnStart"` to search every module).

- Elements: `tab` (`id` for a new tab, or `idMso` = `TabHome`, `TabInsert`, `TabPageLayoutExcel`, `TabFormulas`, `TabData`, `TabReview`, `TabView`, `TabDeveloper` to add groups to a built-in tab), `group`, `button`, `toggleButton`, `checkBox`, `splitButton` (a `button` and a `menu`), `menu`, `menuSeparator`, `dropDown`, `comboBox`, `editBox`, `gallery` (with `item`s), `labelControl`, `separator`, `box` / `buttonGroup` (their children join the group), `dialogBoxLauncher`.
- Attributes: `id`, `idMso` (a built-in control such as `Copy`), `label`, `size="large"`, `imageMso` (mapped to YCGrid's icons; unknown names show a generic icon), `image` (a picture file in the project folder: `name`, `name.png`, `images\name.png` or `customUI\images\name.png`; PNG, JPEG, BMP, GIF, ICO), `screentip`, `supertip`, `enabled`, `visible`, `tag`, `sizeString`.
- Callbacks (values are returned, not passed back by reference):

| Callback | Python signature |
|---|---|
| `customUI onLoad` | `def OnLoad(ribbon)`: keep the `IRibbonUI` |
| `onAction` (button, gallery item launcher) | `def OnStart(control)`; toggleButton / checkBox: `(control, pressed)`; dropDown / gallery: `(control, selected_id, selected_index)` |
| `onChange` (editBox, comboBox) | `def OnText(control, text)` |
| `getLabel`, `getText` | `(control) -> str` |
| `getEnabled`, `getVisible`, `getPressed` | `(control) -> bool` |
| `getSelectedItemIndex`, `getItemCount` | `(control) -> int`; `getItemLabel(control, index) -> str` |
| `getImage` | `(control) -> str`: a picture file name or path (like `image`) |

- `IRibbonUI`: `Invalidate()` (ask every get* callback again), `InvalidateControl(id)`, `InvalidateControlMso(id)` (does nothing: built-in controls follow YCGrid's own state), `ActivateTab(id)`, `ActivateTabMso("TabHome")`. `IRibbonControl`: `Id`, `Tag`.
- get* results are cached until invalidated; YCGrid never waits for Python: until the answer arrives the control shows the static attribute or the last value.

```xml
<customUI xmlns="http://schemas.microsoft.com/office/2009/07/customui" onLoad="Ribbon.OnLoad">
  <ribbon><tabs><tab id="tabTrade" label="Trading"><group id="grpRun" label="Run">
    <button id="btnStart" label="Start" size="large" imageMso="MacroPlay" onAction="Ribbon.OnStart"/>
    <toggleButton id="tglLive" getLabel="Ribbon.LiveLabel" onAction="Ribbon.OnLive"/>
  </group></tab></tabs></ribbon>
</customUI>
```

```python
# modules/Ribbon.py
ribbon = None
live = False

def OnLoad(r):
    global ribbon
    ribbon = r

def OnStart(control):
    Range("A1").Value = "started"

def OnLive(control, pressed):
    global live
    live = pressed
    ribbon.InvalidateControl("tglLive")

def LiveLabel(control):
    return "Live: ON" if live else "Live: OFF"
```

## 16B. Custom panels, containers, floating windows and layouts [YC]

```python
p = Application.Panels.Add("Risk", Caption="Risk Control", Dock=ycDockRight, Width=320)
p.Controls.Add(ycLabel, "lblPnl", Caption="P&L", LinkedCell="Quotes!B2")
p.Controls.Add(ycButton, "btnFlat", Caption="Flatten All", OnAction="Module1.FlatAll")
p.Controls.Add(ycComboBox, "cbAcct", ListFillRange="Settings!A2:A20", LinkedCell="Settings!C1")

c = Application.Containers.Add("Monitor", Floating=True, Left=100, Top=100, Width=900, Height=600)
Worksheets("Quotes").Float()                       # pull out into a floating window
c.Dock(Worksheets("Quotes"), Position=ycDockTab)
c.Dock(Application.Panels("Risk"), Position=ycDockRight, Ratio=0.3)
Application.Layout.Save("Trading")
```

| Object | Members |
|---|---|
| `Application.Panels` | `Add(Name, Caption=None, Dock=ycDockRight, Width=320, Height=240, Visible=True)`, `Panels(Name)` / `Item(Name)`, `Exists(Name)`, `Count`, `for p in Panels` |
| `Panel` | `Name`, `Caption`, `Visible`, `Show()`, `Hide()`, `Width`, `Controls`, `Rebuild(KeepValues=True)`, `Delete()` (to float one: `yc.ui.Float(panel, Left, Top, Width, Height)`; `Panel.Float` is P2); [YC] `BackColor`, `ForeColor` (Excel RGB numbers, None = the theme's; frames, buttons and popups follow the background's lightness: a dark ticket beside a dark chart); [YC] `Locked` (the user can't move, undock, resize or close it; its controls still work; saved with the workbook's layout). A deleted panel's dock place closes up |
| `Panel.Controls` | `Add(Type, Name, **properties)`: Type is `ycLabel`, `ycButton`, `ycTextBox`, `ycComboBox`, `ycCheckBox`, `ycSlider`, `ycChart`, `ycSeparator`, `ycGroup` (accepted, not drawn yet: P2), `ycListBox`, `ycOptionGroup` (option buttons, one per `List` item), `ycSpin` (a whole number between `Min` and `Max`, stepping by `SmallChange`); `Controls(Name)`, `Remove(Name)`. Every property in the next row can be given to `Add` (`Add(ycSpin, "spWidth", Min=100, Max=500, SmallChange=100, LinkedCell="Desk!O5")`, `Add(ycLabel, "lblPnl", LinkedCell=..., NumberFormat=..., FontSize=16)`); an unknown one raises TypeError |
| panel controls | `Name`, `Type` (read-only), `Caption`, `Value` (`Text` is the same), `LinkedCell` (two-way binding, displayed without going through Python), `OnAction` (`"module.function"` or a callable), `Enabled`, `Visible`, `List` (ComboBox, ListBox, OptionGroup); `ListFillRange` (ComboBox), `Min` / `Max` (Slider, Spin) and `Chart` (ycChart) are given to `Controls.Add` only (reading or changing them on the control is P2); [YC] styling `BackColor`, `ForeColor` (Excel RGB numbers, None = the theme's), `FontSize` (points), `Bold`, `NumberFormat` (a label's linked value; `"[Red]#,##0;[Green]-#,##0"` colors it by sign), `Height` (buttons, points), `SmallChange` (a spin's / slider's step: a strike width of 100); `Click()` clicks a button like the user (MCP: `yc_panel_input`; `set_value` on a spin or slider lands on what the user can reach: between `Min` and `Max`, on the steps) |
| `Application.Containers` | `Add(Name, Caption=None, Left=None, Top=None, Width=640, Height=420, Floating=True)`, `Containers(Name)` / `Item(Name)`, `Count` |
| `Container` | `Name`, `Dock(Item, Position=ycDockTab, Ratio=0.3)` (Item: a sheet, panel, chart, container or [YC] `yc.plot` figure), `Delete()` (the container goes; the windows docked in it float), [YC] `Locked` (no moving, resizing or closing it, undocking what is in it or moving its splitters). `Caption`, `Left`, `Top`, `Width`, `Height`, `Floating`, `Items` and `Close()` are P2 |
| `Worksheet` | [YC] `Float(Left=None, Top=None, Width=None, Height=None)`, `Dock()` (back into the main window) |
| `yc.ui.Float(item, Left=None, Top=None, Width=None, Height=None)` | [YC] float a sheet, panel, chart (`ChartObject`) or container in its own window |
| `Application.Layout` | `Save(Name)`, `Restore(Name)`, `Names`, `Delete(Name)`, `Reset()` (View > Reset Layout); layouts are stored in the workbook |

- Panels, controls, containers and layouts are saved in the workbook and restored on open before Python starts. When the code that builds the UI changes, record a UI version in the workbook KV (§16C) and rebuild only when the version differs.
- Constants: `ycDockLeft`, `ycDockRight`, `ycDockTop`, `ycDockBottom`, `ycDockTab`, `ycDockFloat`.

---

## 16C. Workbook KV and CustomDocumentProperties

```python
UI_VERSION = 3
def Workbook_Open():
    if ThisWorkbook.Store.get("ui.version") != UI_VERSION:
        build_ui(keep_values=True)
        ThisWorkbook.Store["ui.version"] = UI_VERSION
```

| Object | Members |
|---|---|
| [YC] `ThisWorkbook.Store`, `Worksheet.Store` | `Store[key]`, `Store[key] = value`, `get(key, default=None)`, `keys()`, `items()`, `del Store[key]`, `in`, `Batch()` (merges a with block into one round trip) |
| `Workbook.CustomDocumentProperties` | `Add(Name, LinkToContent=False, Type=msoPropertyTypeString, Value=None)`, `CustomDocumentProperties(Name).Value`, `.Delete()`, `Count` |
| `Worksheet.CustomProperties` | (P2) use `Worksheet.Store` |

- `Store` values: None, bool, int, float, str, bytes, datetime, `decimal.Decimal`, list, dict (nested), numpy arrays; 16 MB per value, 256 MB in total.
- Writes mark the workbook as unsaved but don't go on the undo stack; they are saved with the `.ycg`.

---

## 16D. Per-cell OnChange [YC]

| Syntax | Description |
|---|---|
| `Range(addr).OnChange = "module.function"` or a callable | register; handler signature `def f(Target):` |
| `Range(addr).OnChange = None` | remove every handler of that range |
| `@Me.OnChange("B2:B100", Sources=ycUser \| ycPython)` | decorator form in sheet modules |
| `Application.OnChange(Address, Handler=None, Sources=ycUser \| ycPython)` | any module (main.py too): `Address` with a sheet name (`"Trade!B14:F25"`) or of the active sheet; also a decorator: `@Application.OnChange("Trade!B2:B9", Sources=ycUser)` |
| `Range(addr).SetOnChange(handler, Sources=ycUser \| ycPython)` | `Range.OnChange = handler` with a choice of sources; `handler=None` removes it |
| `Sources` | a combination of `ycUser`, `ycPython`, `ycCalc`, `ycLive`; default `ycUser \| ycPython` (like `Worksheet_Change`) |

- Order: per-cell OnChange -> `Worksheet_Change` -> `Workbook_SheetChange`; `Application.EnableEvents = False` blocks it.
- Registrations aren't saved: create them in `Workbook_Open`; they are rebuilt after reloads.

---

## 16E. Page setup, printing and PDF

Page Layout tab, File -> Print (Ctrl+P, with the preview) and File -> Export -> Create PDF. PDF files are written through Windows' "Microsoft Print to PDF"
printer, so the PDF looks exactly like the printout. After a page setup change or a preview, the sheet shows dashed page-break lines.

| Member | Description |
|---|---|
| `Worksheet.PageSetup` | `Orientation` (`xlPortrait`, `xlLandscape`), `PaperSize` (`xlPaperA4`, `xlPaperLetter`, `xlPaperLegal`, `xlPaperA3`, `xlPaperA5`, `xlPaperB4`, `xlPaperB5`, `xlPaperTabloid`, `xlPaperExecutive`), `LeftMargin`, `RightMargin`, `TopMargin`, `BottomMargin`, `HeaderMargin`, `FooterMargin` (points; `Application.InchesToPoints`, `CentimetersToPoints`), `Zoom` (10-400, or `False` to fit), `FitToPagesWide`, `FitToPagesTall` (`False` = automatic), `PrintArea` (`"$A$1:$F$40"`, `""` clears), `PrintTitleRows` (`"$1:$2"`), `PrintTitleColumns` (`"$A:$A"`), `PrintGridlines`, `PrintHeadings`, `CenterHorizontally`, `CenterVertically`, `BlackAndWhite`, `Order` (`xlDownThenOver`, `xlOverThenDown`), `FirstPageNumber` (`xlAutomatic`), `LeftHeader`, `CenterHeader`, `RightHeader`, `LeftFooter`, `CenterFooter`, `RightFooter` (codes `&P`, `&N`, `&D`, `&T`, `&F`, `&A`, `&Z`, `&G` picture, `&&`), `DifferentFirstPageHeaderFooter`, `OddAndEvenPagesHeaderFooter`, `FirstPage` / `EvenPage` (`.LeftHeader.Text` ... `.RightFooter.Text`: that page's own texts), `ScaleWithDocHeaderFooter`, `AlignMarginsHeaderFooter`, `Pages.Count`, `PrintComments` (`xlPrintNoComments`, `xlPrintSheetEnd`, `xlPrintInPlace`), `PrintErrors` (`xlPrintErrorsDisplayed`, `xlPrintErrorsBlank`, `xlPrintErrorsDash`, `xlPrintErrorsNA`), `Draft`, `PrintArea` over several ranges (`"$A$1:$B$2,$D$5:$E$6"`: each prints on its own pages), `LeftHeaderPicture` ... `RightFooterPicture` (`.Filename` loads the picture shown where the section has `&G`, `.Width`, `.Height` in points); font codes `&"Arial,Bold"`, `&14`, `&B`, `&I`, `&U`, `&S`, `&Krrggbb` are printed |
| `Worksheet.SetBackgroundPicture(Filename)` | Page Layout > Background: a picture tiled behind the cells on screen (`""` deletes it; saved with the workbook) |
| `Workbook.BuiltinDocumentProperties(Name)` | `.Value` of `"Title"`, `"Subject"`, `"Author"`, `"Manager"`, `"Company"`, `"Category"`, `"Keywords"`, `"Comments"`, `"Content status"`; `"Creation Date"`, `"Last Save Time"`, `"Last Author"` are set when the workbook is saved (File > Info) |
| `Workbook.CustomViews` | View > Custom Views: `Add(ViewName, PrintSettings=True, RowColSettings=True)`, `CustomViews(name).Show()`, `.Delete()`, `.PrintSettings`, `.RowColSettings`, `Count` |
| `Windows` | `Arrange(ArrangeStyle=xlArrangeStyleTiled, ActiveWorkbook=False)` (`xlArrangeStyleHorizontal`, `xlArrangeStyleVertical`, `xlArrangeStyleCascade`), `CompareSideBySideWith(WindowName)` (another YCGrid's workbook by its file name: Synchronous Scrolling then scrolls both YCGrids, both ways; without a name, the workbook's second window), `BreakSideBySide()`, `SyncScrollingSideBySide`, `ResetPositionsSideBySide()` |
| `Window.DisplayRuler`, `Window.Visible` | View > Ruler (Page Layout view); View > Hide / Unhide of the workbook's window |
| `Worksheet.HPageBreaks` / `VPageBreaks` | `Add(Before=Range)`, `Count`, `Item(i)`, iteration; each `HPageBreak` / `VPageBreak` has `Location`, `Type` (`xlPageBreakManual`, `xlPageBreakAutomatic`), `Delete()` |
| `Worksheet.ResetAllPageBreaks()` | remove the manual page breaks |
| `PrintOut(From=None, To=None, Copies=1, Preview=False, ActivePrinter=None, PrintToFile=False, Collate=True, PrToFileName=None, IgnorePrintAreas=False)` | `Worksheet`, `Workbook` (every visible sheet) and `Range` (only that range; its `PrintOut` has no `IgnorePrintAreas`); `Preview=True` opens File -> Print |
| `PrintPreview()` | `Worksheet`, `Workbook`: open File -> Print |
| `ExportAsFixedFormat(Type=xlTypePDF, Filename, Quality, IncludeDocProperties, IgnorePrintAreas, From, To, OpenAfterPublish)` | `Worksheet`, `Workbook` (one PDF of every visible sheet), `Range`; returns the PDF's full path; `xlTypeXPS` is not supported |
| `Application.ActivePrinter` | the printer `PrintOut` uses (the Windows default until set); `Application.Printers` [YC] lists the installed printers |

```python
ws = Application.ActiveSheet   # this sheet, even if another one becomes active
ps = ws.PageSetup
ps.Orientation = xlLandscape
ps.Zoom = False
ps.FitToPagesWide, ps.FitToPagesTall = 1, False   # all columns on one page
ps.PrintTitleRows = "$1:$1"
ps.CenterFooter = "Page &P of &N"
ws.ExportAsFixedFormat(xlTypePDF, r"C:\reports\positions.pdf")
```

## 16F. Exact decimals, nanosecond timestamps and sheet size [YC]

| Member | Description |
|---|---|
| `Range.Value` | Decimal cells read as `decimal.Decimal`; writing a `decimal.Decimal` stores it exactly (up to 28 decimal places) |
| `Range.ValueNs` | read and write epoch timestamps as int nanoseconds (e.g. `1729658400123456789`) |
| `Range.ToNumpy()` | columns with scale = 9 return `datetime64[ns]`; writing a `datetime64[ns]` array stores Decimals and applies a nanosecond time format |
| `NumberFormat` | `"[epoch][tz=+08:00]yyyy-mm-dd hh:mm:ss.000000000"`, `"0.000000000"` |
| `NumberFormat` `[mul=N]` | multiplies the value before formatting: `[mul=10000]0.0" bp"` shows 0.00125 as `12.5 bp` |
| worksheet functions | `DECIMAL(text)`, `NOW_NS()`, `EPOCH_TO_DATE(ts)`, `DATE_TO_EPOCH(d)`, `TS_PART(ts, unit)` |
| `Worksheet.RowLimit` | current number of rows (default 1,048,576); can be raised; grows automatically when writes, pastes or imports go past the end |
| `Rows.Count` | returns the current `RowLimit`, so `Cells(Rows.Count, 1).End(xlUp)` works as usual |
| `Topics.Register(Names, Type=None)` | (P2) `Type=ycDecimal9` for topics of the nanosecond timestamp type: the argument is accepted and ignored for now |

---

## 17. Constants

Constant names and values are generated from the Excel type library and are exactly the same as Excel's; all are imported by `from yc.excel import *`. Common families:

| Family | Examples |
|---|---|
| directions | `xlUp`, `xlDown`, `xlToLeft`, `xlToRight` |
| shifts | `xlShiftDown`, `xlShiftToRight`, `xlShiftUp`, `xlShiftToLeft` |
| alignment | `xlGeneral`, `xlLeft`, `xlCenter`, `xlRight`, `xlTop`, `xlBottom`, `xlJustify`, `xlDistributed`, `xlCenterAcrossSelection` |
| borders | `xlEdgeLeft`, `xlEdgeTop`, `xlEdgeBottom`, `xlEdgeRight`, `xlInsideVertical`, `xlInsideHorizontal`, `xlDiagonalDown`, `xlDiagonalUp`, `xlContinuous`, `xlDash`, `xlDot`, `xlDouble`, `xlLineStyleNone`, `xlHairline`, `xlThin`, `xlMedium`, `xlThick` |
| fills and colors | `xlNone`, `xlSolid`, `xlAutomatic`, `xlColorIndexNone`, `xlThemeColorAccent1`...`xlThemeColorAccent6` |
| paste | `xlPasteAll`, `xlPasteValues`, `xlPasteFormats`, `xlPasteFormulas`, `xlPasteColumnWidths`, `xlPasteValuesAndNumberFormats` |
| calculation | `xlCalculationAutomatic`, `xlCalculationManual`, `xlCalculationSemiautomatic` |
| find | `xlValues`, `xlFormulas`, `xlComments`, `xlPart`, `xlWhole`, `xlByRows`, `xlByColumns`, `xlNext`, `xlPrevious` |
| sort and filter | `xlAscending`, `xlDescending`, `xlYes`, `xlNo`, `xlGuess`, `xlSortOnValues`, `xlTopToBottom`, `xlAnd`, `xlOr`, `xlFilterValues` |
| special cells | `xlCellTypeBlanks`, `xlCellTypeConstants`, `xlCellTypeFormulas`, `xlCellTypeLastCell`, `xlCellTypeVisible`, `xlNumbers`, `xlTextValues`, `xlLogical`, `xlErrors` |
| error values | `xlErrDiv0`, `xlErrNA`, `xlErrName`, `xlErrNull`, `xlErrNum`, `xlErrRef`, `xlErrValue`, `xlErrGettingData`, `xlErrSpill`, `xlErrConnect`, `xlErrBlocked`, `xlErrCalc` (an error's `.Number` is Excel's: `#SPILL!` 2045, `#CALC!` 2050) |
| sheet visibility | `xlSheetVisible`, `xlSheetHidden`, `xlSheetVeryHidden` |
| controls | `xlMoveAndSize`, `xlMove`, `xlFreeFloating`, [YC] `ycFloatWindow`, `msoOLEControlObject`, `msoTrue`, `msoFalse`, `fmStyleDropDownCombo`, `fmStyleDropDownList`, `fmMatchEntryComplete` |
| charts | `xlLine`, `xlLineMarkers`, `xlColumnClustered`, `xlColumnStacked`, `xlBarClustered`, `xlXYScatter`, `xlArea`, `xlPie`, `xlDoughnut`, `xlStockOHLC`, `xlStockHLC`, `xlStockVHLC`, `xlStockVOHLC`, `xlPieOfPie`, `xlBarOfPie`, `xlRadar`, `xlRadarMarkers`, `xlRadarFilled`, `xlWaterfall`, `xlHistogram`, `xlPareto`, `xlBoxwhisker`, `xlTreemap`, `xlSunburst`, `xlFunnel`, `xlLinear`, `xlExponential`, `xlLogarithmic`, `xlPolynomial`, `xlPower`, `xlMovingAvg`, `xlErrorBarTypeFixedValue`, `xlErrorBarTypePercent`, `xlErrorBarTypeStDev`, `xlErrorBarTypeStError`, `xlBinsTypeBinSize`, `xlBinsTypeBinCount`, `xlPrimary`, `xlSecondary`, `xlValue`, `xlCategory`, `xlLegendPositionBottom` |
| shapes (Office) | `msoConnectorStraight`, `msoAlignLefts`, `msoDistributeHorizontally`, `msoFlipHorizontal`, `msoFillSolid`, `msoLineThinThin`, `msoArrowheadTriangle`, `msoPattern5Percent`, `msoTextureTopLeft`, `msoShadowStyleOuterShadow`, `msoSoftEdgeType1`, `msoReflectionType1`, `msoBevelCircle`, `msoShapeStylePreset1`, `msoTextEffect1`, `msoAutoSizeShapeToFitText`, `msoChartFieldRange`, `msoShapeNotPrimitive` |
| protection, sparklines, cursor | `xlNoRestrictions`, `xlUnlockedCells`, `xlNoSelection`, `xlSparkScaleGroup`, `xlSparkScaleSingle`, `xlSparkScaleCustom`, `xlDefault`, `xlWait`, `xlNorthwestArrow`, `xlIBeam` |
| [YC] UI | `ycDockLeft`, `ycDockRight`, `ycDockTop`, `ycDockBottom`, `ycDockTab`, `ycDockFloat`, `ycLabel`, `ycButton`, `ycTextBox`, `ycComboBox`, `ycCheckBox`, `ycSlider`, `ycChart`, `ycSeparator`, `ycGroup`, `ycListBox`, `ycOptionGroup`, `ycSpin`, `ycUser`, `ycPython`, `ycCalc`, `ycLive`, `ycDecimal9` |
| MsgBox | `vbOKOnly`, `vbOKCancel`, `vbYesNo`, `vbYesNoCancel`, `vbCritical`, `vbQuestion`, `vbExclamation`, `vbInformation`, `vbDefaultButton2`, `vbOK`, `vbCancel`, `vbYes`, `vbNo` |
| colors | `vbBlack`, `vbRed`, `vbGreen`, `vbYellow`, `vbBlue`, `vbMagenta`, `vbCyan`, `vbWhite` |
| reference style | `xlA1`, `xlR1C1` |
