# Porting Excel macros to YCGrid Python

YCGrid's Python API has the same names and values as Excel. Most code can be translated line by line; the differences are Python syntax and a few run-time semantics. This file is the mapping table and checklist for translating.

## 1. Porting procedure

1. **Extract**: get the source code of every module and the list of controls (buttons are listed in full) with `yc_extract_macros(path)`. No macro is executed.
   - Sheet contents: open the `.xlsx` / `.xlsm` in YCGrid (an old `.xls`: save it as `.xlsx` in Excel first) (values, formulas, formatting and names come along; a message lists, with counts, what didn't: charts, PivotTables, conditional formatting, validation, comments, hyperlinks, controls, macros...; a `Workbooks.Open` from Python puts the same list on the status bar and in the Errors window) and save it as `.ycg`; sheet names and CodeNames must match the original. Save As to `.xlsx` lists what the copy won't hold before writing it (Excel's Compatibility Checker).
2. **Inventory**: list the following and have the user confirm the scope:
   - event procedures, macros (Public Subs without parameters), UDFs (Public Functions), buttons and other controls.
   - external dependencies: `Declare`, `CreateObject`, file I/O, DDE, RTD (COM servers), UserForms.
3. **Map the files**:

   | Macro module | YCGrid Python |
   |---|---|
   | ThisWorkbook | `main.py` (the project entry point) |
   | sheet module (CodeName = Sheet1) | `sheets/Sheet1.py` (file name = CodeName, not the tab name) |
   | standard module Module1 | `modules/Module1.py` (keep the original name for easy comparison) |
   | class module clsOrder | `modules/clsOrder.py`, or merged into the module that uses it, implemented as a Python class |
   | UserForm | a Python class deriving from `UserForm` (object-model.md §14B): add the controls in `UserForm_Initialize`, keep the event names (`btnOK_Click`) |

4. **Translate**: module by module according to §2-§4.
   - Keep the original procedure and variable names so the user can compare during review.
   - Keep the original comments, with `'` changed to `#`.
5. **Rebuild the controls**: ActiveX-style controls (`OLEObjects.Add`) and form controls (`Shapes.AddFormControl`) of the same names.
   - ActiveX buttons: create a button with the same name, caption and position with `yc_add_control` (`kind=button`); put the original `CommandButton1_Click` into `sheets/<CodeName>.py` under the same name.
   - ActiveX combo boxes: create them with `yc_add_control` (`kind=combobox`) with the original `ListFillRange`, `LinkedCell` and `Style`; put `ComboBox1_Change` into the sheet module under the same name.
   - Form buttons (macro assigned with OnAction): create a button, give it a valid Python name (e.g. `btnRefresh`) and write `def btnRefresh_Click(): Module1.Refresh()` in the sheet module.
   - When the original macro uses `Application.Caller` to tell buttons apart: give every button its own `_Click` and pass the button name to a shared function.
   - ActiveX check boxes, option and toggle buttons, text boxes, labels, list boxes, spin buttons, scroll bars and images: rebuild them with the same names (`yc_add_control` or `OLEObjects.Add("Forms.CheckBox.1", ...)`); their events keep the same names (`<name>_Click`, `<name>_Change`, `<name>_SpinUp`...).
   - Form controls of the same kinds: rebuild them as the ActiveX controls above and move the OnAction macro into the control's event.
6. **Load and test**:
   - `yc_reload_python`, then `yc_python_status`: no load errors and no controls with a missing event function.
   - Test every macro, control and event one by one.
   - Compare the output with the original Excel results when available.
7. **Report**:
   - what was ported, and the behavior differences.
   - what wasn't tested.
   - what the user must decide: UserForms, Win32 API, DDE, external COM objects, macros that place orders.

## 2. Syntax mapping

| Excel | Python | Notes |
|---|---|---|
| `Sub Foo()` ... `End Sub` | `def Foo():` | public functions without parameters are macros |
| `Private Sub Foo()` | `def _Foo():` | names starting with an underscore are not listed as macros |
| `Function F(x As Double) As Double` ... `F = x * 2` | `@udf` + `def F(x: float) -> float: return x * 2` | only add `@udf` to functions meant for cells |
| `Dim x As Long`, `Dim s As String` | omit it, or `x: int = 0` | |
| `Const N As Long = 10` | `N = 10` | |
| `Set r = Range("A1")` | `r = Range("A1")` | |
| `Set ws = ActiveSheet` (also `ActiveCell`, `Selection`, `ActiveWindow`) | `ws = Application.ActiveSheet` | **the bare `ActiveSheet` is the current sheet on every access**: `ws = ActiveSheet` follows whatever becomes active later (after `Worksheets.Add`, `Activate`), while Excel's `Set` keeps that sheet |
| `Range("A1") = 5` | `Range("A1").Value = 5` | **always spell out default properties** |
| `x = Range("A1")` | `x = Range("A1").Value` | as above |
| `If a Then … ElseIf b Then … Else … End If` | `if a: … elif b: … else: …` | |
| `Select Case x` / `Case 1, 2` / `Case 3 To 5` / `Case Is > 9` / `Case Else` | `if x in (1, 2): ... elif 3 <= x <= 5: ... elif x > 9: ... else: ...` | or `match` |
| `For i = 1 To n` ... `Next` | `for i in range(1, n + 1):` | the upper bound is inclusive, hence +1 |
| `For i = n To 1 Step -1` | `for i in range(n, 0, -1):` | |
| `For Each c In rng` | `for c in rng:` | |
| `Do While cond` ... `Loop`, `Do Until cond` ... `Loop` | `while cond:`, `while not cond:` | |
| `Exit Sub`, `Exit Function` | `return` | |
| `Exit For`, `Exit Do` | `break` | |
| `GoTo label` | restructure into loops or functions | Python has no GoTo |
| `With rng.Font` / `.Bold = True` / `.Size = 12` / `End With` | `with rng.Font as f:` / `f.Bold = True` / `f.Size = 12` | settings inside `with` are sent together |
| `If r Is Nothing Then` | `if r is None:` | |
| `Not Intersect(Target, Range("A1:A9")) Is Nothing` | `Intersect(Target, Range("A1:A9")) is not None` | |
| `s = "a" & x` | `s = "a" + CStr(x)` or `f"a{x}"` | `&` converts numbers to strings; in Python you convert yourself |
| `a <> b`, `a = b` (comparison) | `a != b`, `a == b` | |
| `a ^ 2` | `a ** 2` | |
| `a \ b`, `a Mod b` | `a // b`, `Mod(a, b)` | for negative numbers Python's `%` and Excel's `Mod` differ in sign |
| `And`, `Or`, `Not` | `and`, `or`, `not` | Excel doesn't short-circuit, Python does; watch calls with side effects |
| `"A" & i` to build addresses | `f"A{i}"` | `Range("A" & i)` -> `Range(f"A{i}")` |
| ` _` (line continuation) | break lines inside parentheses | |
| `'` comments | `#` | |
| `#2026/10/11#` | `DateSerial(2026, 10, 11)` | |
| `Static n As Long` | a module-level variable or a function attribute | |
| `Public gCount As Long` (module level) | a module-level variable; other modules `import Module1` and write `Module1.gCount` | never `from Module1 import gCount` (that copies the value) |
| `Optional x As Long = 5` | `x: int = 5` | |
| `ParamArray args()` | `*args` | |
| `ByRef` parameters (modified by the callee) | return a tuple, or pass a mutable object | events' `Cancel` uses `Cancel.Value = True` |
| `Type … End Type` | `@dataclass` | |
| `Enum … End Enum` | `class X(IntEnum)` | |
| class modules, `Class_Initialize`, `Property Get` / `Let` | `class`, `__init__`, `@property` | |
| `Debug.Print a; b` | `Debug.Print(a, b)` | |
| `MsgBox "x", vbYesNo + vbQuestion, "Title"` | `MsgBox("x", vbYesNo + vbQuestion, "Title")` | same return values as Excel |
| `On Error Resume Next` (tolerating one line) | wrap that line in `try` / `except YcError` | only wrap the line that may fail |
| `On Error GoTo Handler` … `Handler: MsgBox Err.Description` | `try:` … `except YcError as e: MsgBox(e.Description)` | |
| `Err.Number`, `Err.Description` | `e.Number`, `e.Description` | |
| `Err.Raise vbObjectError + 1, , "message"` | `raise YcError("message", Number=vbObjectError + 1)` | or an ordinary Python exception |

## 3. Run-time and object model mapping

| Excel | Python | Notes |
|---|---|---|
| `Cells(i, 1)`, `Cells(i, "B")` | same | 1-based; columns may be letters |
| `Cells(Rows.Count, "A").End(xlUp).Row` | same | find the last row |
| `arr = Range("A1:C10").Value`, `arr(i, j)` | same (`VArray`) | 1-based; `arr[i-1][j-1]` works too |
| `UBound(arr, 1)` | same | the number of rows of a VArray |
| `ReDim arr(1 To n)`, `ReDim Preserve` | `arr = [None] * n`, `append` | Python lists are 0-based |
| `Split(s, ",")(0)` | `Split(s, ",")[0]` or `s.split(",")[0]` | both are 0-based |
| `Mid(s, 2, 3)`, `InStr(s, "x")`, `Left(s, 2)` | same (porting helpers) | 1-based; or use slices `s[1:4]`, `s.find("x") + 1`, `s[:2]` |
| `s Like "TX*"` | `Like(s, "TX*")` | |
| `CreateObject("Scripting.Dictionary")` | `{}` | `d.Exists(k)` -> `k in d`; `d.Keys` -> `list(d)` |
| `New Collection` | `[]` | Collections are 1-based, lists 0-based |
| `Application.WorksheetFunction.VLookup(...)` | same | raises on errors; use `Application.VLookup` to get error values |
| `Range("A1:B2").Copy Destination:=Range("D1")` | `Range("A1:B2").Copy(Destination=Range("D1"))` | named parameters keep their names |
| `Selection.PasteSpecial Paste:=xlPasteValues` | `Selection.PasteSpecial(Paste=xlPasteValues)` | |
| `Application.CutCopyMode = False` | same | |
| `Worksheets("x").Activate` followed by `Range(...)` | use `Worksheets("x").Range(...)` | avoid Select / Activate: faster and more robust |
| loops of `ActiveCell.Offset(1, 0).Select` | address cells directly with `Cells(r, c)` | see §4 |
| `Application.Wait Now + TimeValue("0:00:01")` | `Application.Wait(Now() + TimeValue("0:00:01"))` | blocks the macro thread; use `OnTime` inside events |
| busy-wait loops with `DoEvents` | use `Application.OnTime` or a background thread | |
| `Now + 1` (one day later) | `Now() + timedelta(days=1)` or `DateAdd("d", 1, Now())` | |
| `Open f For Input As #1` / `Line Input #1, s` | `with open(f, encoding="cp950") as fh:` | text files written by Excel on Traditional Chinese Windows are usually CP950 (Big5) |
| `Dir`, `Kill`, `MkDir`, `FileCopy` | `pathlib`, `os`, `shutil` | |
| `ThisWorkbook.Path & "\data.csv"` | `Path(ThisWorkbook.Path) / "data.csv"` | relative to the workbook's folder, so the whole folder can be moved; never hard-code absolute paths like `C:\...` |
| `Shell "cmd /c ..."` | `subprocess.run([...])` | |
| `Declare PtrSafe Function ...` (Win32 API) | `ctypes` | check each one by hand |
| `=RTD("prog", "", topic...)` formulas | write `=STREAM("prog", topic...)` (the server argument goes) and provide an `@stream` named `prog`, or switch to `Range.BindTopics`; YCGrid doesn't talk to COM RTD servers | |
| DDE (`=DDE(...)`, `DDEInitiate`) | not supported; use a Python data feed process (`[[python.feeds]]`) + `Application.Topics` | |
| `Application.Volatile` | same, or `@udf(volatile=True)` | |
| `Private WithEvents App As Application` (class module) | `Workbook_SheetChange` and the other `Workbook_Sheet*` events in `main.py` (they fire after the sheet module's own handler); per-cell OnChange when only certain cells matter | one YCGrid instance has only one workbook |

## 4. Performance rewrites

Everything that runs in Excel runs in YCGrid, but with the process boundary these patterns are worth rewriting:

**Cell-by-cell reads and writes -> one array read and write**

```python
# slow: one access per cell (a literal translation from Excel)
for i in range(2, last + 1):
    Cells(i, 3).Value = Cells(i, 1).Value * Cells(i, 2).Value

# fast: read once, write once
a = Range(f"A2:B{last}").Value
Range(f"C2:C{last}").Value = [[CDbl(r[0]) * CDbl(r[1])] for r in a]

# faster: numpy
ab = Range(f"A2:B{last}").ToNumpy(Empty=0.0)     # blanks as 0, like Excel; the default would be NaN
Range(f"C2:C{last}").Value = (ab[:, 0] * ab[:, 1]).reshape(-1, 1)

# or: write formulas so the engine computes in parallel and updates automatically
Range(f"C2:C{last}").Formula = "=A2*B2"
```

**Other common rewrites**

- Loops that interleave reads and writes: every read of an affected formula cell waits for recalculation. Use arrays, or set `Application.Calculation = xlCalculationManual` first and switch back to automatic with `Application.Calculate()` at the end.
- Chains of `Select` / `Activate`: reference the objects directly.
- `WorksheetFunction.VLookup` inside loops: read the lookup range into a dict first and look up in Python.
- Formatting cell by cell inside loops: compute the range to format first and set it at once; or use conditional formatting.
- Writing live quotes with `Range.Value` in a loop: use `Application.Topics.Push` and `Range.BindTopics`.

## 5. Example: a complete port

**Original Excel code**

```excel
' Sheet1 (CodeName) sheet module
Private Sub Worksheet_Change(ByVal Target As Range)
    If Intersect(Target, Me.Range("C2:C200")) Is Nothing Then Exit Sub
    Application.EnableEvents = False
    Dim c As Range
    For Each c In Intersect(Target, Me.Range("C2:C200"))
        If IsNumeric(c.Value) And c.Value <> "" Then
            c.Offset(0, 1).Value = Now
            If c.Value > c.Offset(0, -1).Value Then
                c.Interior.Color = RGB(255, 199, 206)
            Else
                c.Interior.ColorIndex = xlNone
            End If
        End If
    Next c
    Application.EnableEvents = True
End Sub

Private Sub CommandButton1_Click()
    Dim lastRow As Long
    lastRow = Me.Cells(Me.Rows.Count, "A").End(xlUp).Row
    Me.Range("B2:B" & lastRow).ClearContents
    MsgBox "Cleared " & (lastRow - 1) & " rows", vbInformation
End Sub

' Module1 standard module
Public Function MidPrice(bid As Double, ask As Double) As Double
    MidPrice = (bid + ask) / 2
End Function

Sub ExportPositions()
    Dim arr As Variant, i As Long, total As Double
    arr = Worksheets("Positions").Range("A2:D500").Value
    For i = 1 To UBound(arr, 1)
        If arr(i, 1) <> "" Then total = total + arr(i, 3) * arr(i, 4)
    Next i
    Worksheets("Positions").Range("F1").Value = total
End Sub
```

**YCGrid Python**

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
            # Excel's Empty equals "", Python's None doesn't: use IsEmpty
            if IsNumeric(c.Value) and not IsEmpty(c.Value):
                c.Offset(0, 1).Value = Now()
                # Excel compares a blank left side as 0; comparing None fails in Python: use CDbl
                if CDbl(c.Value) > CDbl(c.Offset(0, -1).Value):
                    c.Interior.Color = RGB(255, 199, 206)
                else:
                    c.Interior.ColorIndex = xlNone
    finally:
        Application.EnableEvents = True   # the Excel original had no error handling; try/finally guarantees it is restored

def CommandButton1_Click():
    lastRow = Cells(Rows.Count, "A").End(xlUp).Row
    Range(f"B2:B{lastRow}").ClearContents()
    MsgBox(f"Cleared {lastRow - 1} rows", vbInformation)
```

```python
# modules/Module1.py
from yc.excel import *

@udf
def MidPrice(bid: float, ask: float) -> float:
    return (bid + ask) / 2

def ExportPositions():
    arr = Worksheets("Positions").Range("A2:D500").Value
    total = 0.0
    for i in range(1, UBound(arr, 1) + 1):
        if not IsEmpty(arr(i, 1)) and arr(i, 1) != "":
            total += CDbl(arr(i, 3)) * CDbl(arr(i, 4))
    Worksheets("Positions").Range("F1").Value = total
```

**Rebuild the button**: `yc_add_control(sheet="Sheet1", kind="button", name="CommandButton1", caption="Clear", cover_range=<original position>)`.

**Test**:

- `yc_ui_input`: type a number into C5, check the time in D5 and the fill color of C5.
- `yc_trigger_control(sheet="Sheet1", name="CommandButton1", event="click")`.
- `yc_evaluate_formula("=MidPrice(100,102)")`, expect 101.
- `yc_run_macro("Module1.ExportPositions")`, then read `Positions!F1`.

## 6. Pitfall list (check every item after translating)

1. **Default properties**: always add `.Value` when reading or setting `Range(...)` and `Cells(...)`.
1b. **`Set ws = ActiveSheet` keeps the sheet; `ws = ActiveSheet` doesn't**: write `ws = Application.ActiveSheet`
   (or `Worksheets("Name")`). With the bare proxy, a `Worksheets.Add` later makes `ws` the new, empty sheet.
2. **Empty**:
   - Excel's Empty equals both 0 and `""`; Python's `None` equals neither.
   - Check for blanks with `IsEmpty()`.
   - Use `CDbl()` before numeric operations or comparisons (`CDbl(None)` is 0), `CStr()` for strings (`CStr(None)` is `""`).
3. **1-based vs 0-based**:
   - 1-based: the call syntax of Range / Cells / collections / `VArray`, the string functions `Mid` / `InStr`.
   - 0-based: Python lists, slices, `Split` results, `VArray[...]` indexing.
4. **Booleans**: Excel's True is -1 and `CInt(True)` is -1; in Python it is 1. Rewrite idioms like `x = -(a > b)`.
5. **Operators**:
   - `&` concatenates strings (Python's `+` adds numbers).
   - `\` is integer division; `Mod` treats negative numbers differently from Python; `^` is exponentiation.
6. **Short-circuiting**: both sides of Excel's `And` / `Or` run; Python short-circuits. Rewrite conditions that rely on side effects.
7. **Variable names**: avoid Python keywords and built-ins such as `str`, `list`, `type`, `id`, `sum`, `max`, `min`, `input`, `range`, `format`. Excel's `Dim sum` becomes `total`.
8. **Dates**:
   - `Now()` and `Date()` return `datetime`, `TimeValue()` returns `timedelta`.
   - Add or subtract days with `timedelta` or `DateAdd`.
   - For cells with a date format `.Value` returns `datetime` and `.Value2` the serial number.
9. **Range variables and inserted rows**: in Excel a Range variable moves when rows are inserted; in YCGrid v1 it doesn't. Get the Range again after structural changes.
10. **Events don't nest**: events fired by writes inside a handler run after the current handler finishes. Still use `EnableEvents = False` to prevent infinite loops.
11. **Always restore EnableEvents**: use `try` / `finally`. Excel originals often forget to restore it on errors.
12. **Writes are asynchronous**: most writes are sent without waiting, so an error YCGrid reports for one (a protected cell, a value it refuses) may only be raised at the next synchronization point (a read, `DoEvents()`, `Application.Sync()`), with the message pointing to the original file and line. Formula syntax errors are raised on the line that writes the formula (`FormulaError`).
13. **Blocking**: `Application.Wait`, `time.sleep` and `DoEvents` busy loops delay later events. Use `OnTime` or a background thread.
14. **MsgBox doesn't freeze the UI**: users can still work with other cells while a MsgBox is open. If the logic relies on the "freeze", check the state first.
15. **Text file encoding**: files read and written by Excel are usually CP950 on Traditional Chinese Windows, Python defaults to UTF-8; give the `encoding` explicitly.
16. **Object comparison**: `rng1 Is rng2` becomes `rng1 == rng2`, comparing sheet and address; never use `is`.
17. **UDF limits**: as in Excel, UDFs can't change sheets. If an original Excel UDF secretly changed cells, turn it into a macro or event.

## 7. Unsupported items and items needing a human decision

| Item | Recommendation |
|---|---|
| UserForm | a Python class deriving from `UserForm` (object-model.md §14B); keep the control and event names |
| `OnAction` of form buttons, `Application.Caller` | form buttons keep `OnAction` (`Shapes.AddFormControl`); to tell buttons apart pass the name to a shared function |
| check boxes, option buttons, spin buttons and other controls | supported both as form controls (`Shapes.AddFormControl`, `ControlFormat`) and as ActiveX-style controls (`Forms.CheckBox.1`...); frames and multi-page controls aren't |
| custom ribbon (`customUI/customUI14.xml` inside the xlsm) | copy the XML into the project folder as `customUI.xml`; each callback becomes a Python function in `modules/` that returns its value instead of setting `returnedVal` (`Sub GetLabel(control, ByRef returnedVal)` -> `def GetLabel(control): return ...`) |
| Win32 API (`Declare`) | rewrite with `ctypes`; check each one |
| external COM objects (`CreateObject("...")`) | prefer a Python package as a replacement; use `pywin32` when really needed |
| DDE | use a Python data feed process (`[[python.feeds]]`) + `Application.Topics` |
| printing, charts, pivot tables | supported: `PageSetup`, `PrintOut`, `ExportAsFixedFormat` (object-model.md §16E), `ChartObjects` (§16A), `PivotCaches` / `PivotTables` with grouping, calculated fields, PivotCharts, slicers, calculated items and timelines (§13B) |
| cross-workbook references, opening other workbooks | supported: `=[Book2.ycg]Sheet1!A1` links and `Workbooks.Open` (`.ycg`, `.xlsx` / `.xlsm`, `.csv`; each workbook in its own window; an old `.xls` must be saved as `.xlsx` in Excel first) |
| `Application`-level events (WithEvents) | the `Workbook_Sheet*` events in `main.py` (`Workbook_SheetChange`, `Workbook_SheetSelectionChange`, `Workbook_SheetActivate`... all fire: events-and-controls.md §2), or per-cell OnChange when only certain cells matter |
