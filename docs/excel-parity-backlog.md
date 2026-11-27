# Excel parity backlog

Where YCGrid still works differently from Excel, and what was decided to stay different.

YCGrid is an Excel-like front end for traders and researchers, driven by Python. It follows Excel's habits and
interface where people work every day (keys, KeyTips, menus, ribbon, dialogs), not every Excel feature. **Before
listing differences from Excel, read "Kept on purpose" and "Not planned" below**: those were decided, so don't propose
them again. A new "won't do" decision goes into "Not planned" with its reason, so the list gets shorter over time.

## Open

Add a difference you find here, under its area (Home, Insert, Formulas, Data, Review, View, Page Layout, File,
charts, PivotTables), with the steps that show it. Whoever fixes it adds the test that proves it (most live in
`python/tests/test_excel_parity.py` and `python/tests/test_parity_*.py`) and deletes the line. Nothing is open
at the moment (measured against Excel 16 on 2026-11-08), except:

- Home > Find & Select > Go To Special (and `SpecialCells`): the same cells as Excel (480 queries measured), but
  split into areas another way: Excel's `Areas` (and `Address`) list the areas by their last row and may join
  cells down a column where YCGrid joins them along a row (`B3:B4,B5:C6` where YCGrid gives `B3:B5,C5:C6`), so a
  macro's `For Each a In r.Areas` meets the same cells in another grouping.
- Python: `Range("E1:E2,G1:G2").Select()` makes G1 the active cell; Excel's is E1, the first area's first cell, and
  typing with Enter then goes E1, E2, G1, G2 (YCGrid: G1, G2, E1, E2). The selection keeps its active area last, so the
  fix is a selection that knows which area is active (measured 2026-11-11).
- Data > tables: cells deleted inside a table across part of its width delete its whole rows (columns), as in Excel,
  and the cells end the same; but Excel's table then also takes in the row (column) of cells that moved up next to
  it (C9 deleted up in C7:D10 with `below` in C11 leaves the table C7:D10 holding `below`; YCGrid's is C7:D9).
  Measured 2026-11-15.
- Formulas: an array formula typed so that it closes a circular reference shows 0 in its own cell; Excel spills zeros
  over its range (=E8:F8*2 in A2 with =OFFSET(A1,1,1) in E8, which reads B2: Excel shows 0 in A2 and B2). Both
  report the circular reference. Measured 2026-11-15.
- Formulas: a name read through a sheet (`Data!Rate`, or `Data!A` a Replace made of `Data!A1`) whose sheet is deleted
  is the workbook's name in Excel 16 (`=SUM(Book1!A:A3)`, #NAME? while no such name exists; measured 2026-11-21);
  YCGrid makes it #REF! (`=SUM(#REF!:A3)`).
- Data > Data Validation without an error message of its own: YCGrid's Stop alert names the reason ("You must enter a
  whole number.", "The value is outside the allowed range."); Excel 16 refuses the same entries (measured 2026-11-17)
  with a text of its own, not yet read (a hidden Excel's alert is a NUIDialog without readable text: measuring it
  needs a visible Excel).
- Home > Paste > Paste Special with an operation (Add, Subtract, Multiply, Divide) or Skip Blanks over any cell of an array formula
  entered with Ctrl+Shift+Enter: YCGrid refuses it with "You can't change part of an array." (it can't combine
  the array cell by cell). What Excel 16 does there is not measured yet: the probe needs Windows' clipboard
  (doc_fuzz --more seeds 80142 and 22650345, 2026-11-19).

## Kept on purpose (YCGrid extras)

These are not differences to fix; they stay:

- `Range.Text` gives a number's formatted text whatever its column's width: Excel's gives what the cell shows
  there (`1E+09`, `####` in a narrow column, measured 2026-11-27). What fits depends on the window's fonts, zoom and
  screen scale, and a script reads `Text` for the formatted value; the sheet itself shows Excel's text.

- A CSV's fields are never formulas: `=1+1`, `+1+1`, `-1+1`, `--5` and `@SUM(A1)` open as text, where Excel 16
  calculates them (measured 2026-11-19 over 719 fields, Workbooks.Open): a CSV from outside must not run a
  formula (CSV injection). A number of more than 15 significant digits opens as an exact Decimal (PLAN 5.2a),
  where Excel keeps 15. The rest of that measurement matched, except what the region decides (dates read
  year/month/day in Taiwan, `2024-1` a month) and the two lines under Not planned.
- A circular reference closed by a script (without iterative calculation): what reads the cycle keeps the value it
  showed when the cycle closed, in both applications, but Excel calculates after every write of a script and YCGrid
  calculates a script's writes together (a read in between calculates first). So a reader that was waiting to be
  calculated when the cycle closed can show its older value in YCGrid (`rand_058`: with a read between
  the steps YCGrid shows Excel's values, measured 2026-11-15), and a formula written just before the write that
  closes the cycle, with no read between them, is calculated with it and shows 0 as one entered after the cycle
  does, where Excel keeps the value it had (`rand3_029`, `rand3_112`, measured 2026-11-23). Calculating between
  every write would slow every script and live feed for values Excel itself flags as a circular reference.

- A sheet laid out over more than 200,000 printed pages (a value at XFD1048576 and one near A1: tens of millions of
  pages) shows and prints its first 200,000 (`kMaxLayoutPages`), in one column in Page Layout view; Excel lays the
  rest out too. Making every page took gigabytes and 18 s at each edit in Page Layout view.

- A data table (What-If Analysis > Data Table, `Range.Table`) of more than 20,000,000 cells is refused ("The data
  table is too large"), as a fill and an array formula are: its results are calculated and kept a cell at a time.

- Format Painter started from more than 4,000,000 cells of the used range is refused ("The selection is too large
  to copy the formats of"), where Excel takes any selection. The painter holds a copy of the cells it took the
  formats of (about 160 bytes a cell), and the whole sheet with a value in its last cell took gigabytes before
  painting them took the rest of the memory.

- A copy (Ctrl+C, Ctrl+X) of more than 20,000,000 cells of the used range is refused ("The selection is too large to
  copy"), where Excel copies any selection. Copied cells are held one by one; whole rows and columns are cut to the
  used range first, so only a selection over data spread to both ends of the sheet (a value at XFD1048576 and one
  near A1) meets the limit, and holding billions of empty cells would take the memory of a trader's day.

- A chart inserted over a selection of more than 8,000,000 points (series times points: the whole sheet with a value
  in its last cell is 255 series of a million points) is an empty chart that Select Data fills, where Excel 16 makes
  the chart and didn't answer for minutes (measured). Three whole columns of a million rows still chart in full.

- A fill (Ctrl+D, Ctrl+R, Fill Down / Right) that would write more than 20,000,000 cells holding something (Fill Right
  then Fill Down over the whole sheet: 17 billion values) is refused with the reason, as a copy that large is; a fill of
  formats alone goes to the lines' own styles and has no limit.
- Cells inserted that don't span whole rows (columns), next to a line whose cells have formats of their own, when
  the formats they would take come to more than 20,000,000 cells (B2 to the last cell inserted down under Outside
  Borders on the whole sheet: 17 billion) are refused with the reason; whole rows and columns take them as a line
  style and have no limit.

- Advanced Filter over a list of more than 20,000,000 cells (the whole sheet with a value in its last cell) is refused
  ("The list range is too large"), as a copy that large is; the list is read whole.

- Python's read of more than 4,000,000 cells at once (`Range.Value`, `Formula`, `Text`, `NumberFormat`, `HasFormula` of
  the whole sheet) raises "The range is too large (more than 4 million cells); read it in parts", where Excel's object
  model builds the whole array (17 billion cells, more memory than the machine has). The used range, or a column at a
  time, reads in one call.

- A range typed down 1,048,576 rows (`=SUM(B1:C1048576)`) stays as typed; Excel writes it back as whole columns
  (`=SUM(B:C)`, measured). A YCGrid sheet may grow past Excel's 1,048,576 rows, where the two mean different cells.
  (A range across the whole width does become whole rows, as in Excel: `A1:XFD2` is `1:2`.)
- Home > Share (screenshots of cells, sheets and windows) and Ctrl+Shift+S.
- Page Layout > Print group (Print, Export to PDF).
- View > Freeze Center Columns, the Python group, containers and layouts; the Trading and Developer > Code groups.
- Shape Format > Text (text alignment of a shape).
- The sheet tab menu's Insert adds a sheet at once (no Insert dialog: YCGrid has no sheet templates).
- Opening and saving go straight to the file dialog (Excel's "Don't show the Backstage when opening or saving").
- `Range.Formula` is `Formula2` (dynamic arrays): `B3.Formula = "=A1:A5"` spills, where Excel's legacy `Formula`
  writes `=@A1:A5`. Scripts written for YCGrid get one meaning; `Formula2` is there for Excel's own scripts.
- A dynamic array spilling past row 1,048,576 grows the sheet (rows grow on demand); Excel shows #SPILL!.
- A whole row or column in a dynamic array formula is cut to the used area: `=2:2` in E1 spills row 2's filled cells,
  where Excel's 16,384 columns don't fit from column E and show #SPILL! (measured 2026-11-11). A whole column is a
  million cells; formulas that take one (`=SUM(B:B)`, `=B:B*2` beside the data) stay fast.
- Inserting rows when the last row holds data grows the sheet past 1,048,576 rows (rows grow on demand, PLAN
  5.1); Excel refuses ("To prevent possible loss of data..."). Inserting columns that would push data past XFD is
  refused, as in Excel.
- Before* events (`Workbook_BeforeSave`, `Worksheet_BeforeDoubleClick`...) act a few milliseconds after the click: the
  window never waits for Python, so a handler that hangs can't freeze a trading screen. The outcome is Excel's: the
  action waits for the handler's answer, and Cancel stops it.
- Results where Excel gives up and YCGrid gives the number (measured 2026-11-10): `MOD` once the quotient passes
  about 1.1259E+12 (`MOD(1E+13,7)` is 3, `MOD(2^41-1,1)` and `MOD(1E+12,0.5)` are 0), SIN / COS / TAN / COT / SEC /
  CSC of 134,217,728 (2^27) and beyond, SINH / COSH from 709.79, ASINH / ACOSH from 1.4E+154 and MULTINOMIAL past
  170! (`MULTINOMIAL(100,71)`, `MULTINOMIAL(171)`), all #NUM! in Excel; and AGGREGATE's functions 1 to 13 over an
  array or a calculation instead of a reference (`AGGREGATE(9,6,A1:A7/(A1:A7>3))`, `AGGREGATE(9,6,{1,2,3})`,
  `AGGREGATE(9,6,5)`, `AGGREGATE(9,6,A1:A3,{1,2})`), #VALUE! in Excel.
- A function passed by its name where a LAMBDA goes (`BYROW(A1:B3,SUM)`, `MAP(A1:A3,ISBLANK)`,
  `REDUCE(0,A1:A3,SUM)`), as Excel 365 takes it; the Excel measured here (16.0 build 20326) answers #NAME?.
- The newer error values written in a formula (`=#SPILL!`, `=#CALC!`, `=#CONNECT!`, `=#BLOCKED!`, `=#PYTHON!`),
  which Excel refuses to enter (of these it takes `=#GETTING_DATA` only).
- A change a protected sheet or a protected workbook structure refuses opens no message box (decided 2026-11-25):
  Excel's whole message goes to the Error Log as an error, and a red line beside the status bar's Error Log entry
  names the cells for six seconds (a click opens the log). A box over the sheet at every touch of a locked cell
  stopped the work of people at a screen built to be used, not edited. A cell in a range that asks for a password
  still opens Unlock Range, since that is a question to answer, and Python still gets its error.
- Ctrl+Backspace in the cell editor deletes the word before the caret, as in every other editor; Excel's does nothing
  there (measured 2026-11-11). Outside the editor it shows the active cell, as in Excel.

## Regional settings, not differences (measured 2026-11-07)

YCGrid follows US Excel (en-US format codes, US date order). An Excel on a Windows set to another region shows some
things its own way; comparing with it, these are the region, not YCGrid bugs. Don't list them again (measured on
Excel 16 with Taiwan's regional settings and the Traditional Chinese UI):

- A time typed with the region's AM / PM word after it (`9:30 上午`) is a time in Taiwan's Excel, which takes Windows'
  own designators there; YCGrid takes US Excel's `AM` / `PM` / `a` / `p` (measured 2026-11-27). The word before the
  time (`上午 9:30`, as Taiwan's Excel shows times) is text in both.
- `Application.International` reads what YCGrid reads and writes, US Excel's separators and order (country 1, `.`
  and `,`, month first, currency `$`), where Excel reads the machine's (Taiwan's: 886, year first, General in the UI's language;
  measured 2026-11-22).
- Built-in format ids 14, 20-22, 37-44 and the CJK ids show the region's form while `NumberFormat` reports US
  Excel's code: `m/d/yyyy` (14) shows `2024/1/7`, `h:mm` (20) shows `09:00`, `$#,##0_);($#,##0)` shows `-$1`.
  YCGrid shows what the code says: `1/7/2024`, `9:00`, `($1)`.
- Typed dates follow the region's order: `1/7/24` is 2001/7/24 (year/month/day) in Taiwan, January 7 2024 in
  YCGrid. `2024/1/7` is the same date in both.
- The code a typed date gets is the region's short date: `NumberFormat` reports `m/d/yyyy` in Excel (built-in id 14,
  shown 2024/1/7 in Taiwan); YCGrid writes `yyyy/m/d` for a typed 2024/1/7 (it shows the same), and so for Ctrl+;
  (today's date). Ctrl+Shift+3 applies the region's short date in Taiwan's Excel (id 14) and US Excel's `d-mmm-yy` in
  YCGrid; Ctrl+Shift+1 applies `#,##0` there and US Excel's `#,##0.00` in YCGrid (measured 2026-11-11). The time a cell is
  edited as (`18:00:00` against the region's long time) comes from the region too.
- Format codes are written in the UI's language in a localized Excel (General and the colors have Chinese names): its
  `TEXT(x, "General")` and `TEXT(1, "0;[Red]@")` are #VALUE!, though the same codes are fine as a cell's
  `NumberFormat`. YCGrid takes the English codes everywhere (`TEXT(x, "General")` works).
- `Range.NumberFormatLocal`, `FormulaLocal` and the list separator follow the region and the UI language; YCGrid's
  are the en-US forms.
- Negative currency in the region's default (`-$5`, `-NT$5`) comes from the region; a code says how YCGrid shows it.
- `DOLLAR(-1234.567, 1)` is `-$1,234.6` in Taiwan (the region's negative currency), `($1,234.6)` in YCGrid.
- Text the UI language names: error values in `Range.Text` (`#SPILL!` in Chinese), the default names of tables,
  their columns and totals rows, sheets and workbooks (`Table1`, `Column1`, `Total`, `Sheet1`, `[Book1]` in English),
  Subtotal's labels, PivotTable labels (`(blank)`, `(All)`, "X Total", `<0 or (blank)`), grouped months and years
  (`Jan`, `2025` against the Chinese month and year words) and error messages.
- In a Chinese Excel: TRIM also removes the ideographic space U+3000; Qtr1 / Quarter 1 aren't recognized as quarters
  by AutoFill (they count on to Qtr5); CHAR(160) is half of a double-byte character (code page 950).
- Dates read from text follow the region: `DATEVALUE("99/1/2")` is 1999/1/2 and `"2024/3"` is March 1, 2024 in
  Taiwan; the US long date `"Thursday, March 5, 2026"` is #VALUE! there and a date in YCGrid. Day groups of a
  PivotTable, Text to Columns' dates and a validation's date or time read back use the region's short forms.
  Typed month names follow it too (measured 2026-11-09): Taiwan's Excel takes `Jan5`, `5Mar` and `Mar2024` as dates
  and keeps `Mon Jan 5`, `Jan. 5` and `Mar 5 2024` as text; YCGrid reads them the US way.
- AutoCorrect's list is the UI language's: Taiwan's Excel keeps a lone i typed into a cell, YCGrid makes it I as US
  Excel's list does (measured 2026-11-11).
- Currency marks other than the region's: Taiwan's Excel reads `€5`, `5 €` and `US$5` as numbers; YCGrid takes `$`
  and `NT$` (`US$5` is the cell US5, as in a US Excel).
- The letter `r` of a format code shows the year in Taiwan's Excel, as `e` does (measured 2026-11-10):
  `TEXT(A8,"0;-0;zero")` of an empty A8 is `z19001900o` there and `TEXT(1234.567,TRUE)` is `T1903U1903`; YCGrid
  reads `e` only and shows `z1900ro` and `TRU1903`. A date given as text is read the region's way too:
  `COUNT("1/1/2024")` is 0 and `="1/1/2020"+0` is #VALUE! there, 1 and 43831 in YCGrid; and `CELL("address")` of
  another sheet's cell names the workbook in the UI's language.

Not regional, decided: Python's `Range.Text` returns the formatted text without fitting it to the column (Excel's
`Text` shows `####` or fewer General digits in a narrow column). Scripts read `Text` for the formatted value; the
grid itself fits and shows `####` as Excel does. Find in values matches the same unfitted text (Excel's Find doesn't
find 12345678901 in a column too narrow to show more than `1.23E+10`).

## Not planned (decided 2026-11-03)

Each with its reason. Don't list these as missing again. Members of Excel's object model (and so the File > Options
settings) are classified one by one in `schema/excel_parity.toml`; [excel-object-model.md](excel-object-model.md)
lists what is YCGrid's, what is not planned and the open gaps by priority.

- Amounts with the euro sign (U+20AC before 5) and digits of other scripts (Arabic-Indic U+0661 U+0662) typed or in a CSV: Excel 16
  reads them as numbers (measured 2026-11-19), YCGrid as text. The data traders and researchers here type and
  load is in dollars and ASCII digits.
- A full-color icon set: YCGrid keeps one-color Fluent glyphs. Redrawing every icon changes nothing in how YCGrid is
  used; commands that shared one glyph got their own instead.
- Record Macro and Use Relative References: macros are Python written in VS Code, where recorded code is of little use.
- SmartArt, 3D Models, Equation, Signature Line, Object (OLE embedding), Office Add-ins, 3D Maps, Excel's Screenshot of
  other windows: rarely used by traders and researchers, and large to build.
- Cell checkboxes (Excel 2024's Insert > Checkbox): the form and ActiveX check boxes cover it.
- Thesaurus, Workbook Statistics, Check Accessibility, Check Performance, Translate, Smart Lookup, Copilot, Sheet View,
  Share, Data Types (Stocks, Geography), AutoSave to the cloud: cloud services, or rarely used.
- The chart Data Table element: rarely used.
- WordArt Styles on Shape / Chart Format, Edit Points, Merge Shapes, the Alt Text button (Alt Text stays in the Format
  pane).
- Picture editing: Remove Background, Corrections, Color, Artistic Effects, Transparency, Compress Pictures, Picture
  Styles / Effects / Layout.
- Slicer and Timeline style galleries.
- Table Design > External Table Data; PivotTable Solve Order, List Formulas, OLAP Drill Down / Drill Up.
- Developer > Add-ins, XML, Run Dialog; Customize the Ribbon from the UI (ribbons are customized from Python, RibbonX).
- Templates on File > New, the Save As places page, the Open page's Folders tab, Inspect Document.
- Chart Design > Add Chart Element > Lines > Drop Lines and the chart Format tab's Shape Styles group: rarely used; the
  Format pane (Ctrl+1) formats every chart element.
- Slicer and Timeline tabs' Arrange group and the slicer buttons' Height / Width: slicers aren't part of the object
  selection the Arrange commands act on; their Size and Columns boxes cover the common case.
- Find & Select > Comments (threaded comments): Find & Select > Notes and Review > Comments cover it.
- File > Options as a separate dialog showing one category at a time: the Options page has Excel's categories on the
  left and a click goes to that category; the options and OK / Cancel work as Excel's.
- Options > General: "Show Mini Toolbar on selection" (Excel's applies to text selected inside a cell; the right-click
  mini toolbar shows either way, as in Excel) and "Enable Live Preview".
- Options > Advanced: "Automatically insert a decimal point" and "Use system separators" (rarely used; YCGrid uses
  `.` and `,`, as Taiwan's settings do), and turning "Enable automatic percent entry" off: it is always on, as in a new
  Excel (7 typed into a percent cell is 7%).
- Spaces and line breaks typed inside a formula (`= 1 + 2`, `=SUM( B2:B4 )`, Alt+Enter after an operator) and a
  unary plus before a number (`=+1+2`): Excel keeps the spacing and drops that plus, YCGrid writes the formula from its
  parsed form (`=1+2`, `=+1+2`). The value is the same; keeping the typed spacing needs the text kept beside the
  parse. (A typed `+1+2` without the `=` is `=1+2` in both.)
- F1 Help; Ctrl+F9 / Ctrl+F10 (minimize / maximize the window: Windows' own keys and buttons do it).
- Goal Seek's Step / Pause: the search ends before its status dialog is drawn, so they are greyed, as Excel shows them
  after a search has finished.
- Excel's "Disable all macros except digitally signed macros" and Trusted Publishers: YCGrid's code is plain `.py` files, which
  carry no signature to check. Companies deploy Trusted Locations and the macro setting in settings.json, as Excel's
  are deployed through policy.
- The last units in the last place of RATE and PMT (measured 2026-11-08): RATE follows Excel's secant search bit
  for bit in 1,925 of 1,953 measured cases; in the rest the search ends inside the cash flows' rounding, a few
  units in the last place away (`RATE(64,0,-100,250)` is 0.014420022409793502 in Excel, 0.014420022409793608 in
  YCGrid). There Excel's own answer jumps between a rate and #NUM! from one amount to the next:
  `RATE(120,-6.1E9,50*6.1E9,0,1)` and 67 more such loans are #NUM! in Excel and in YCGrid, while
  `RATE(12,-5396000000,62700000000,0,1)` and `RATE(300,-2698000000,375000000000,0,1)` are rates in Excel (on
  points YCGrid's search never reaches) and #NUM! in YCGrid; a looser end test would turn those 68 into rates. PMT is
  within 4 units in the last place of Excel's in all 80 measured cases. Closer needs Excel's exact order of
  operations, which measuring from outside doesn't give.
- The last digits and the limits of other functions (measured 2026-11-10: 11,565 formulas against Excel, of which
  the 8,554 that agree are kept as cases in `tests/unit/excel_measured_formulas.inc`). These are written down, not
  chased: the difference shows far behind the decimal point, and only Excel's own order of operations gives it.
  - XIRR agrees to about 8 digits: `XIRR({-1,2},{45000,45100})` is 11.553345584869385 in Excel and
    11.553345566348014 in YCGrid.
  - SIN / COS / TAN next to a multiple of pi / 2, and of arguments above about 1E+8, differ from the 11th to the
    13th digit: `SIN(PI())` is 1.22514845490862E-16 in Excel and 1.2246467991473532E-16 in YCGrid, `TAN(PI()/2)`
    16324552277619072 and 1.633123935319537E+16, `COS(123456789)` 0.14025968153586152 and 0.14025968153390964.
  - `ASINH(1E-10)` is 1.000000082690371E-10 in Excel and 1E-10 in YCGrid; `ACOSH(1.0000001)` is
    0.00044721359189477274 and 0.00044721359190373475; `BESSELJ("1",1)` 0.44005058567713012 and
    0.44005058574493344.
  - A result under 2.2250738585072E-308 is 0 in Excel (`=1E-307/10`, `=10^-308`, `=2^-1023`, `SECH(710)`;
    `=1E-308*0.1=0` is TRUE, `SIGN(1E-200*1E-110)` 0 and `NORM.S.INV(2E-308)` #NUM!); YCGrid keeps the number.
- Excel's quirks on malformed or undocumented input (measured 2026-11-09 by random probes): formulas and calls
  written as documented give Excel's result; these give another, and nobody relies on them:
  - WEEKNUM's return types 22 and above (undocumented): Excel takes them (`WEEKNUM(59,22)` to `WEEKNUM(59,30)` are
    10, as type 2); YCGrid gives #NUM!, as for every type Excel's help doesn't list.
  - TEXTAFTER / TEXTBEFORE of a one-character text with an empty delimiter: Excel's `TEXTAFTER("b","")` and
    `TEXTAFTER(3,"")` are empty and `TEXTBEFORE("b","")` is `b`, while for longer texts (`TEXTAFTER("apple","")`
    is `apple`) the empty delimiter matches at the start, as YCGrid does for every length.
  - SORTBY's argument checks: Excel refuses to enter a call whose last `by_array` has no `sort_order`
    (`SORTBY(A1:A8,B1:B8,1,E1:E8)`) and takes a range of numbers as one `sort_order`
    (`SORTBY(A1:A8,B1:B8,E1:E8)` spills); YCGrid takes the unpaired array and gives #VALUE! for the range.
  - `Range.Find("~")` (a lone tilde, an escape with nothing after it): Excel's returns the `After` cell whatever it
    holds; YCGrid's finds nothing (`None`), as Replace finds nothing for it in both.
  - `Union` area order in two corners (307 of 309 random unions are Excel's address): an area inside a whole-row
    area keeps that area where it is (`Union(3:5,F1:H5,C5:E5,B5:I7)` is `3:5,F1:H5,B5:I7` in Excel,
    `F1:H5,3:5,B5:I7` in YCGrid), and an area absorbed earlier still joins a later one
    (`Union(I4:I8,H3:I9,A6:H9,I2:I5)` ends in `I2:I8` in Excel, `I2:I5` in YCGrid). The cells are the same.
  - An argument left empty in a function that came from the Analysis ToolPak is #N/A in Excel (measured 2026-11-10,
    as the rest of this list: `GCD(,4)`, `EDATE(45000,)`, `WORKDAY(,1)`, `NETWORKDAYS(45000,)`, `YEARFRAC(,45000)`,
    `WEEKNUM(,)`, `QUOTIENT(5,)`, `MROUND(5,)`, `RANDBETWEEN(,)`, `ERF(,)`, `DELTA(,)`, `GESTEP(,)`,
    `SERIESSUM(,0,1,1)`, `BESSELJ(,1)`, `EFFECT(0.1,)`, `DOLLARDE(1.1,)`, `FVSCHEDULE(1,)`,
    `CUMIPMT(0.1,10,1000,1,2,)`, `CONVERT(,"m","ft")`, `COMPLEX(,1)`, `DEC2BIN(,)`, `IMSUB("1",)`,
    `PRICE(,46000,0.05,0.06,100,2)`, `TBILLPRICE(45000,,0.05)`, also `DELTA(IF(TRUE,,))`); YCGrid reads it as 0, as
    the built-in functions do, and answers what the function gives for 0.
  - Lists of those functions that hold logicals, text or empty cells follow no one rule in Excel (YCGrid's answer
    in parentheses): `GCD(TRUE,2)` is #VALUE! (1), `LCM` over 4, an empty cell and 6 is 0 (12), `GCD` / `LCM` /
    `MULTINOMIAL` of one empty cell #VALUE! (0, 1, 1), `MULTINOMIAL({1,"2"})` 3.0000000000000004 (1),
    `FVSCHEDULE(100,TRUE)` #VALUE! (200), `SERIESSUM(2,0,1,"3")` 3 and over an empty cell 0 (#VALUE!),
    `XNPV(0.1,"1",45000)` 1 and XNPV / XIRR over logicals or empty cells #NUM! (#VALUE!), `IMSUM(TRUE,"1")` #VALUE!
    (#NUM!), `IMPRODUCT("2",A8)` of an empty A8 "0" ("2"), `CONVERT(1,TRUE,"ft")` #VALUE! (#N/A) and
    `Z.TEST(TRUE,4)` #DIV/0! (#N/A).
  - An empty argument or one empty cell where Excel wants an array: `SUMPRODUCT(,)`, `SUMPRODUCT(2,)`,
    `SUMPRODUCT(A8)` of an empty A8, `SUMX2MY2(,)`, `CORREL(,)`, `SLOPE(,)`, `RSQ(,)`, `FREQUENCY(,)`,
    `ARRAYTOTEXT(,)`, `AGGREGATE(9,6,A1:A3,)`, `FORECAST(7,A1:A3,)` and `RANK(0,IF(TRUE,,1))` are #VALUE! in Excel;
    YCGrid reads a 0 there.
  - Unary plus on an empty cell or on an array of references: `PRODUCT(+A8,5)` of an empty A8 is 0 in Excel and 5
    in YCGrid (`COUNT(+A8)` is 1 and `MIN(+A8,5)` 0 in both), and `SUM(+OFFSET(A1,{0,1},0))`,
    `SUM(+INDIRECT({"A1","A2"}))` and `INDEX(+OFFSET(A1,{0,1},0),2)` read each reference as its first cell in
    Excel and are #VALUE! in YCGrid.
  - DECIMAL's text with a prefix or suffix of its base: `DECIMAL("0xff",16)`, `DECIMAL("ffh",16)` and
    `DECIMAL("xff",16)` are 255, `DECIMAL("101b",2)` 5 and `DECIMAL("x",16)` 0 in Excel, #NUM! in YCGrid;
    `DECIMAL(REPT("1",256),2)` is #VALUE! in Excel and #NUM! in YCGrid; and `DECIMAL("1",)` is #NUM! in Excel,
    where YCGrid's one-argument DECIMAL (an exact decimal) answers.
  - LAMBDA corners: `=LAMBDA(x,x)(A1:A3)` entered in a cell fills one cell in Excel and spills three in YCGrid;
    MAP over a whole column gives 1,048,576 rows in Excel (and its SUM #N/A) where YCGrid maps the used rows;
    `LAMBDA(a,[b],ISOMITTED(b))(1,IF(TRUE,,))` is TRUE in Excel, FALSE in YCGrid (a blank handed on isn't an
    omitted argument); Excel refuses a parameter name with spaces or a period (`[ y ]`, `[x.1]`) and a constant
    that overflows (`=1E+308*10`), which YCGrid takes.
  - Windows' linguistic equality of text: in Excel the German sharp s equals `ss`, the ligatures of `ae`, `oe`,
    `ij`, `dz` and `fi` and the thorn (`th`) equal their letters, the two Greek small sigmas are equal, a
    full-width `A` equals `A`, a letter followed by a combining accent equals the accented letter
    (`"e"&UNICHAR(769)=UNICHAR(233)`) and a zero-width character is ignored (`"a"&UNICHAR(8204)&"b"="ab"`); `=`,
    MATCH, XMATCH, XLOOKUP, SEARCH, UNIQUE and IF follow it. YCGrid folds case only: following Excel needs
    Windows' sort keys in every comparison, lookup index and filter, for texts a trading sheet doesn't hold.
- .xlsx fidelity beyond the basic import and export (decided 2026-10-25: YCGrid's own .ycg keeps everything, and
  measured on 2026-11-08 it loses nothing; since 2026-11-09 what a file holds of these is listed with counts when it
  is opened and before a copy is saved, ADR 0004, so nothing is dropped silently): tables (ListObjects), page setup and print titles, sheet protection, the
  workbook's Normal style and column / row styles, tab colors, very hidden sheets, outline levels, AutoFilter state,
  zoom and gridlines, the 1904 date system, Excel's column width rounding, normalizing escaped format codes
  (`yyyy\-mm\-dd`), `_xHHHH_` escapes, legacy formulas without dynamic-array metadata read with implicit
  intersection, and .xlsm. Files YCGrid writes do open in Excel and compute the same (LET / LAMBDA, spill references,
  the newer error values, references to deleted sheets, array arithmetic).
- Excluded by ADR 0012: Get & Transform (Power Query), the Data Model and the CUBE functions, co-authoring, reading
  `.xls`. So From Text/CSV's Transform Data, Import Data's PivotTable Report / PivotChart / Only Create Connection and
  Create PivotTable's "Add this data to the Data Model" are greyed (Load and Load To... > Table work as in Excel).
