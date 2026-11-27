# Changelog

What changed in each release of YCGrid. The full notes, with install steps, are on the
[Releases](https://github.com/ycgrid/ycgrid/releases) page. Versions follow [Semantic Versioning](https://semver.org/);
while the version starts with 0, a minor version (0.x) may change the Python API, and the release notes say how.
`.ycg` workbooks stay readable by every later version (see [Compatibility](README.md#compatibility)).

## Unreleased

### Added
- `RiskLimits(duplicate_window=seconds)`: an order the same as the last one sent, again within that time, is refused,
  so a double-click on an order button sends one order. The `trading_desk` example uses it.
- `plot.figures()` lists each plot's title and items (their kind and label), so a test checks what a figure shows,
  and its view, its place on the screen, its drag lines and the date a linked row marks.
- Excel 2003's menu keys after Alt (Alt, E, S, V for Paste Special, Values; Alt, O, C, A to AutoFit; Alt, D, F, F...)
  run their commands, with Excel's hint while they are typed.
- A dialog's range box takes a cell clicked or a range dragged on the sheet while it has the keyboard, as Excel's
  RefEdit (Greater Than's value, a validation list, Goal Seek); its button still works too.
- Excel's letters in the menus a person opens from the keyboard: Alt+Down, E, a few letters, Enter filters a list
  (S and O sort, C clears); Shift+F10 (or the Menu key) then V pastes values, D deletes, I inserts; Ctrl+V, Ctrl, V
  turns a paste into values (each Paste Options picture takes its letter, named in its tip: Values (V)).
- Inserting a table, a chart or another object opens its tab (Table Design, Chart Design...), and the tab before it
  comes back when the selection leaves the object, as in Excel.
- Insert Table Rows Above, Insert Table Columns to the Left, Delete Table Rows and Delete Table Columns: in a table
  the cell menu's Insert and Delete offer them (Shift+F10, I, A adds a row above), and Ctrl+- and Ctrl+Shift+= act on
  the table's rows (its columns for a selection taller than wide) without a dialog, moving only the table's cells, as
  in Excel.
- Use table names in formulas (File > Options > Formulas, `Application.GenerateTableRefs`, on by default): pointing at
  a table's cells while typing a formula writes structured references, as in Excel: `=[@Px]*[@Qty]` in the table,
  `Table1[Px]` for a column's data, `Table1[#All]`, `Table1[[#Headers],[Qty]]`; part of a column or another row stays
  cells.

### Fixed
- `market_watch` example: rows clicked quickly while the quotes streamed drew the intraday chart twice at once, and
  each click could add another copy of every line (every price tag shown twice or more).
- `trading_desk` example: a round trip's realized profit stayed 0 (a fill that closed the position read the average
  after the fill); a quantity typed wrong sent one lot instead of nothing; a double-click on BUY MKT sent two orders.
- Click actions (`yc.interact.on_click`, click-to-trade) run on a press and a release on the same cell, as a button:
  a drag that began on a ladder's size, to select cells, sent an order.
- The click-to-trade ladder (`dom_ladder`, `trading_desk`, `txo_market_maker`) shows the total of every working
  order at a price, not the last one's size; a double-click on `txo_market_maker`'s Start no longer starts and stops it.
- `demo`: pausing and resuming live updates within a second added a second update chain each time, and a sorted
  quote table measured every change against another stock's open.
- `order_flow`: the working order's line stays in view and says when the order filled; `time_machine` shows the
  ladder it replays; `quick_plot`, `strategy_lab`, `pairs_trading` and the other screens dock their charts beside the
  sheet instead of floating them over its cells, so the tables and trade lists they show can be read and clicked.
- A daily chart marks the clicked bar with its date, not "00:00"; a heatmap's long column labels no longer run
  together.
- Escape closes Insert Symbol and Mark Parameter's windows, as every dialog.
- The mouse wheel scrolls the sheet while a drag selects cells; Ctrl+wheel zooms 10% for every notch, also in Page
  Layout view, where it zoomed twice.
- Alt held with a tab's letter (Alt+W pressed together) opens that tab's KeyTips; Alt in a dialog no longer lights
  the ribbon's KeyTips behind it; KeyTips work in the Traditional Chinese UI (Alt, H, L, H, G reached nothing).
- 12.5% typed into a percent cell by a Chinese IME (the whole text at once) is 12.5%, not the text "12.5%%".
- `yc.launch(path)` raises when the file can't be opened, instead of handing back a blank workbook.
- F5, Enter after a Go To selects again everything that was selected (all its areas), not only its active cell.
- Shift+F2 on a cell with a note goes on typing after the note's text, not in front of it.
- Function Arguments' OK enters the formula when it was opened while typing (Ctrl+A after =SUM), as Excel's does.
- Esc in Goal Seek Status and Solver Results puts the original values back, as their Cancel; on Solver's and the
  Scenario Manager's other pages it goes back a page instead of closing the dialog.
- In the Traditional Chinese UI, sheets, tables, their headers and PivotTables added later are named in Chinese like
  the first sheet (Sheet2, Table1 and Column1 were English).
- A double-click on a chart's column formats its series (Format Data Series), as Excel's, not the single point.
- Figures docked one after another no longer split a pane down to a few pixels (a splitter dragged there stopped a
  debug build): a pane too small to split takes the figure as a tab.
- The Clipboard, Format and Selection panes open beside the sheets instead of floating over their cells.
- The Sort dialog selects the cells it sorts, without the header row while My data has headers is on.
- A formula typed with a full-width = (a Chinese IME in full-width mode) is entered as a formula, as in Excel.
- Typing beside a table grows it only over a line that holds nothing else, as Excel does: a total under the table
  was taken in and became a circular reference. A refused formula no longer grows the table.
- A click on a status bar statistic (Sum, Average...) copies its value, as in Excel 365.
- A hidden YCGrid whose TEMP folder is missing no longer ends while starting.
- An array formula entered with Ctrl+Shift+Enter that reads its own cells no longer leaves what reads it uncalculated
  (`{=A1:B3*2}` beside `=VLOOKUP(A1,A1:C9,2,FALSE)` kept 0 where Excel shows #N/A).
- Tab in a table goes from its last column to the next row's first, and from its last cell adds a row, as in Excel;
  Tab ending an entry on a protected sheet goes to the next unlocked cell, as Tab on the sheet does.
- Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgDn and Ctrl+PgUp move between a dialog's tabs (Format Cells, Find and Replace,
  Page Setup...).
- Holding Ctrl and then dragging a sheet tab copies the sheet, as Excel's help describes.
- Menus over the cells (the filter menu, Paste Options, Auto Fill Options, error checking, Quick Analysis, a
  PivotTable field's filter, a slicer's menu) keep the UI's size at any zoom.
- (Select All) in a filter list and Chart Filters shows a mixed box while some values are ticked.
- Flash Fill takes a word at either end, as Excel does: "2330" out of "Taiwan Semi (2330)" gives "2454" for
  "MediaTek (2454)".
- Find All highlights the row clicked.
- A validation's input message floats over the circles and objects on the cells.
- Renaming a sheet to a name in use says Excel's message ("That name is already taken. Try a different one.").
- Excel's letters in the dialogs people drive from the keyboard: Insert and Delete (Ctrl+-, R, Enter deletes the
  row), Text to Columns, Go To's Special button (Alt+S) and Go To Special (F5, Alt+S, K, Enter selects the blanks; K
  did nothing, and the = Up Ctrl+Enter after it wrote a formula over the data), Find and Replace (Alt+A replaces all).
  Alt and a letter in a dialog that leaves the sheet usable stay with the dialog instead of the ribbon behind it.
- Ribbon drop-downs open under their button: opened from the KeyTips (Alt, H, N) they showed wherever the mouse was.
  The arrows and Enter pick an entry in them, and that Enter no longer moves the active cell; Alt, H, H and Alt, H,
  F, C open the color palette instead of applying the last color.
- Greater Than, Less Than, Equal To and Between start with Excel's values (halfway between the lowest and the highest
  number; a quarter and three quarters of the way, rounded), and Text That Contains with the active cell's text.
- Cancelling the Sort dialog puts the selection back, as in Excel; OK keeps the sorted cells selected.
- Home > Fill > Series from one cell with a Stop value fills down (or across) to it, as in Excel.
- Ctrl+PgDn and Ctrl+PgUp no longer page the sheet they leave a screen down; Alt+Enter, Ctrl+Enter and Alt+Home
  outside an entry leave the active cell where it is, as in Excel.
- Customize Status Bar names Minimum and Maximum in full, as Excel's menu.
- Alt+Down, Down, Enter on a cell takes the first entry of its pick list: the Down of Alt+Down moved it one further.
- Ctrl+\ and Ctrl+Shift+| (Row and Column Differences) make the first cell found active, and find none with
  one cell selected, as in Excel; Ctrl+/ selects an array entered with Ctrl+Shift+Enter from its first cell and says
  "No cells were found." on a dynamic array's spill, as in Excel.
- End, Enter goes to the last column of the used area in the row (with Shift, selects up to it), and Escape
  leaves End mode on, as in Excel.
- In a table, Ctrl+Space selects the column's data, then the column with its header and totals, then the sheet's
  column; Shift+Space the table's row before the sheet's; Ctrl+A (and Ctrl+Shift+Space) the data, then the whole
  table, then the sheet, as in Excel.
- Ctrl+Enter over a column beside a table, its header cell included, types the data cells as entered: they were
  all the header's text.
- A formula in a table keeps its table's name in references other than one column's (`Table1[[#Headers],[Qty]]`,
  `Table1[[Qty]:[Px]]`), as Excel writes them.
- A value typed, entered with Ctrl+Enter or filled right beside a table grows it in an Undo step of its own: the first
  Ctrl+Z takes back the growth and keeps the value, as in Excel (Table AutoExpansion).

## 0.10.3 (2026-11-25)

### Changed
- A change a protected sheet refuses (typing into a locked cell, Delete, a paste, a locked shape) no longer opens a
  message box over the sheet: the message goes to the Error Log as an error, and a red line beside the status bar's
  Error Log entry names the cells for a few seconds (click it to open the log). So does a change the workbook's
  protected structure refuses.

## 0.10.2 (2026-11-25)

### Changed
- The animation at the top of the README shows the `market_watch` example live (quotes and the intraday chart of
  the symbol picked), in the look of the other screens, in English and Traditional Chinese; it no longer shows
  record and replay. `tools/make_readme_gif.py` records it.

## 0.10.1 (2026-11-24)

### Added
- Four example screens in the look of Taiwanese trading terminals, each a project in `examples`: `market_watch`
  (multi-symbol quotes colored against the previous close, change arrows, limit rows filled, category tabs and the
  selected symbol's intraday chart), `option_chain` (the T-style TXO chain with expiry tabs, the smile of every expiry
  and the open interest by strike), `technical_chart` (daily candles with moving averages and Bollinger bands, volume,
  MACD and KD in linked plots) and `trading_desk` (a watch list, order buttons, a click-to-trade ladder, the position
  and the fills with live candles, on one sheet). The README shows them, and every picture is taken by
  `tools/make_readme_shots.py` in English and Traditional Chinese.

### Fixed
- Ctrl+F1 collapses and pins the ribbon, as in Excel: two handlers of the key each switched it in the same frame, so
  it never changed.

## 0.10.0 (2026-11-23)

### Changed
- `=RTD(progid, server, topic...)` is now `=STREAM(name, topic...)`: it reaches the Python `@stream` of that name, as
  `=RTD` did, without the server argument it ignored, and no name suggests a COM RTD server (YCGrid never talked to
  one). A workbook with `=RTD(` formulas shows #NAME? there: replace `RTD("name","",` with `STREAM("name",`.
- A worksheet function written in Python that reads `Application.Caller` or `Application.ThisCell` is called for each
  cell, as Excel calls it; other functions are still called once for every cell with the same arguments.
- The examples and samples are one folder, `examples`: every research and trading screen that Trading > Examples
  built is now a project of its own (`examples\<name>\<name>.ycg` with its `main.py`), and opening its workbook
  builds the screen. The demo and the TXO market-making screen moved there too (`examples\demo`,
  `examples\txo_market_maker`); `open_demo.cmd` opens `examples\demo\demo.ycg`. `examples\README.md` lists them.
- Removed: the Trading > Examples gallery, the `yc.examples` package, `python -m yc examples` and the MCP tool
  `yc_examples`. Build an example by opening its workbook.
- The default Trusted Location for YCGrid's own projects is `%YCGRID_DIR%\examples` (it was `demo` and `samples`).
  A Trust Center list saved before keeps its entries; add `examples` there to open the examples without the prompt.
- The install, uninstall and file association messages of `ycgrid.exe --install` / `--uninstall` are English on
  every Windows, as `install.cmd` and `uninstall.cmd` are; Explorer names the file type "YCGrid Workbook".

### Fixed
- A cut and paste over part of a range a formula reads changes that range as Excel does in every case measured
  (13,617 cuts: an edge moved along it or diagonally, a corner moved, cells from outside it landing inside it, the
  cells written over leaving all of it): `=SUM(C2:C11)` after C1:C2 is cut to C4 reads C5:C11, `=SUM(C2:C5)` after
  C1:C4 is cut to C2 reads C3:C5 where it was #REF!.
- An array entered with Ctrl+Shift+Enter that reads its own cells is calculated when none of its values reads itself
  (`{=A2:A9+1}` over A1:A8 counts down from 9), as in Excel; one whose values do read themselves is still a circular
  reference. An array entered after a circular reference shows 0 only in the values that read it, and the others are
  calculated.
- A 1904 workbook opened in a window that held a 1900 one was calculated in the 1900 system (=DATE(2024,1,1) read as
  2028-01-02) until something calculated it again.
- Precision as displayed: turned off, it calculates every formula again at full precision (they kept their rounded
  values); the cells an array spills are rounded to their own formats too; a number half way between two shown
  values is rounded as the cell shows it (-75.195 in 0.00 is -75.20, it was -75.19).
- A calculation interrupted part way (CalculationInterruptKey) and then finished finds the circular references an
  uninterrupted one finds, and doesn't calculate what reads them.
- A sheet whose calculation is off: Undo gives its formulas their values back (they showed 0), and a copy of it is
  calculated, as in Excel.
- GETPIVOTDATA and PivotTable.GetData under a value filter (Top 10, Value Filters) give the total the report shows,
  not every item's; `PivotFields("Sum of Sales")` finds a data field by its caption, as recorded macros use it; a
  report of one value under a filter is one column wide (TableRange1, DataBodyRange; RowRange raises); an empty value
  cell is `xlPivotCellValue` and the column Grand Total heading `xlPivotCellGrandTotal`.
- Four frozen panes are numbered as Excel numbers them (2 is the top right), and a frozen pane asked to scroll scrolls
  the panes beside it.
- A chart's shapes (`Chart.Shapes`) are placed from the chart's corner when read too, and aren't among the sheet's
  `Shapes`.
- `Workbooks.Add` and `Workbooks.OpenText` work with the window they started: a YCGrid started meanwhile (another
  script, a person) was taken for it and got the text file's contents.
- A hidden instance's frames no longer wait for the window under the user's mouse: on a desktop slow to answer
  (the screen asleep) every frame of a script's or a test's YCGrid took half a second.
- `Application.Caller` in a worksheet function written in Python was always None: the calling cell was never sent.
- The missing-packages bar shows "Installing Python packages: ..." while they install; an install that fails brings
  it back with the installer's reason, Try Again and Skip, and the Error Log keeps the reason. It used to vanish on
  Install, and a failed install said only "Installation failed (code 1)" while the workbook's code stayed unloaded.
- A package a handler needs only when it runs (`Workbook_Open` reaching numpy through `yc.trading` or `yc.sim`, an
  event, a macro) is offered on the package bar when it's missing, once a session; the example showed the error and
  no way to install it.
- A project with shared code (`[python] paths`) is offered only the packages of the shared modules it imports: every
  example was offered numpy and pandas because other modules of `examples\lib` need them.
- The restart after installing packages in a project with data feeds no longer logs "The Python host exited (code0)".
- A double-clicked example opens in the YCGrid it ships with (`<YCGrid>\examples\<name>\<name>.ycg` goes to
  `<YCGrid>\ycgrid.exe`), as a workbook beside `ycgrid.exe` already did: an older YCGrid registered for `.ycg` files
  took the examples' shared code for missing packages and offered to install them.
- `install.cmd` over an older install leaves nothing of it in YCGrid's own folders (`python`, `skill`, `mcp`,
  `licenses`), and removes its old `demo` and `samples` folders, now in `examples`.

### Added
- Excel's object model, member by member: `schema/excel_parity.toml` classifies every member of 53 objects Excel 16's
  type library lists (YCGrid's, not planned with the reason, or a gap with its priority), `scripts/check_excel_parity.py`
  checks it, and `docs/excel-object-model.md` lists the gaps.
- `scripts/gaps_fuzz.py`: random rounds over those members, each checked by its result (in check_all and in
  `random_rounds.py`), and `yc_doc_fuzz --gaps` for the calculation settings (`tests/fuzz/corpus/gaps`).
- The members of that list traders, model training and statistics use, each as Excel 16 does it (measured):
  - the window: `Application.Caption`, `WindowState`, `Left` / `Top` / `Width` / `Height`, `UsableWidth` /
    `UsableHeight`, `DisplayFullScreen`, `DisplayStatusBar`, `Cursor`, `Ready`; `Window.Caption`, `WindowState`, its
    place and size, `Close`, `Panes`, `ActivePane` and the `Pane` object (`VisibleRange`, `ScrollRow`,
    `ScrollColumn`, scrolling), `ScrollIntoView`;
  - long runs: Ctrl+Break interrupts the running macro as `Application.EnableCancelKey` says (`xlInterrupt`,
    `xlErrorHandler` with `CodeInterruptedError`, `xlDisabled`); `CalculationInterruptKey` and `CheckAbort` stop a
    long calculation (what it didn't reach stays pending); `MultiThreadedCalculation`; `CalculateBeforeSave`,
    `FormatStaleValues` (waiting cells struck through in manual calculation); `EnableLargeOperationAlert` and
    `LargeOperationCellThousandCount` (the question before a paste or Fill Down / Right over many cells);
  - `Worksheet.EnableCalculation`, `EnableFormatConditionsCalculation`, `EnableAutoFilter`, `EnableOutlining`,
    `EnablePivotTable`, `ConsolidationFunction` / `Options` / `Sources`, `SaveAs` (a sheet as CSV);
    `Workbook.Date1904`, `PrecisionAsDisplayed`, `ForceFullCalculation` (saved with the workbook; File > Options >
    Advanced > When calculating this workbook); `Workbooks.OpenText` (codes such as 0050 kept as text);
    `Application.International`, `FileDialog` (a folder picker too), `ThisCell`, `MacroOptions` (an @udf's
    description, category and argument help in Insert Function, which now lists the @udf functions);
  - PivotTables: `RowRange`, `ColumnRange`, `DataLabelRange`, `PageRange`, `DataPivotField`, `VisibleFields`,
    `HiddenFields`, `Slicers`, `GrandTotalName`, `RefreshDate`, `RefreshName`, `GetData`, `ShowPages`;
    `PivotField.AutoShow` and its `AutoShow*`, `AddPageItem`, `VisibleItems`, `HiddenItems`, `DataType`, `AutoGroup`;
    `PivotItem.DataRange`, `LabelRange`; `Range.PivotCell`, `PivotField`, `PivotItem`; `PivotCache.CreatePivotChart`;
    `Application.GenerateGetPivotData` (pointing at a value writes GETPIVOTDATA);
  - `Range.DisplayFormat` (the format conditional formats give), `ListColumn.Total`, `ListObject.Slicers`,
    `SlicerCache.VisibleSlicerItems`;
  - charts: `Chart.Shapes` (lines and marks that move with the chart), `DisplayValueNotAvailableAsBlank`,
    `ExportAsFixedFormat` (PDF), `ChartObject.TopLeftCell` / `BottomRightCell` / `CopyPicture`, `Shape.TopLeftCell`
    / `BottomRightCell` / `CopyPicture`, `Axis.AxisGroup`, `DisplayUnitCustom`, `HasDisplayUnitLabel` and the unit's
    label on the chart, `DisplayUnitLabel`, `TickMarkSpacing`, `CategoryNames`; the tip over a chart
    (`Application.ShowChartTipNames`, `ShowChartTipValues`).
  File > Options has the new settings (Formulas: before saving, stale values, threads, GetPivotData; Advanced:
  the large operation alert, the chart tips, the workbook's 1904 date system and precision as displayed).
- File > Options > Advanced > "Show page breaks" and `Worksheet.DisplayPageBreaks`, as in Excel: the dashed page
  break lines that Print, Print Preview and Page Setup turn on in Normal view can be turned off again. They are no
  longer kept when another workbook is opened in the same window.
- `build_release.cmd` in the source folder: double-click it to build, run the unit tests and lay out
  `dist_release`, which it opens; `install.cmd` there installs the new version.
- `[python] paths` in `ycgrid.toml`: folders of shared code a project imports as if they were in its `lib` (the
  examples share `examples\lib` with `paths = ["../lib"]`). Their modules are the project's own, never offered as
  missing packages; `python -m yc pack` copies them into the packed project's `lib`, and the VS Code settings name
  them for Pylance.

## 0.9.0 (2026-11-21)

### Changed
- Ranges of several areas fill, refuse and read as Excel's do from Python: a method Excel refuses over several areas
  raises instead of acting on the first one, and `Range.Find` searches every area in Excel's order. `Find`, `Replace`
  and `SpecialCells` of one cell act on the whole sheet; `Range.Next` and `Previous` give the cell beside the first one.
- Filtered lists take writes, fills, inserts, deletes and Find as Excel's do: cells inserted while a filter filters,
  or inside a list with filter buttons, are refused with Excel's message, and Find skips the rows a filter hides.
- Protected sheets refuse and allow what Excel's do: notes follow "Edit objects", data validation and conditional
  formats are refused, a Replace with a locked match changes nothing, and a fill with a locked cell is refused whole.
- Tables, as measured in Excel: inserting inside a table inserts table rows or columns; inserted rows get the
  calculated columns; a fill below a table grows it, making room when the rows under it hold something; the cells a
  table grows over take its formats, and only calculated columns continue into them; a table growing down takes in
  the ranges that end at its last row (`=SUM(B2:C3)` under B1:C3 becomes `=SUM(B2:C4)`). Sort and Remove Duplicates
  over cells that meet a table they aren't inside are refused, and sorting part of a table sorts its whole body.
- Remove Duplicates leaves out a last row of totals (SUM, AVERAGE, COUNT, COUNTA, MAX, MIN, SUBTOTAL, AGGREGATE).
- Merged cells: fills, Clear, Cut, Find and Replace treat them as Excel's do; a table holds no merged cells; a cut
  onto part of a merged area undoes it; inserting cells that would push a merged area past XFD is refused.
- Arrays entered with Ctrl+Shift+Enter: formulas and sorts over them behave as Excel's, an array's first cell can't
  be changed alone, a one-cell array sorts as a cell, and Paste Special with an operation or Skip Blanks over one is
  refused instead of breaking it.
- AutoSum over whole columns writes its totals as far as the data, over the whole sheet edits the active cell, and
  stops past the data of a formatted column, as Excel does.
- Cells as wide as the sheet inserted to the right (deleted to the left) are whole rows, and as tall as the sheet
  shifted down (up) whole columns; a reference an insertion pushes past XFD is `#REF!`.
- A range operator with an error at an end gives that error: `=SUM(Foo:A3)` over an undefined name is `#NAME?`.
- A paste whose copy area runs past the sheet's edge is refused; a command over several areas refuses them all with
  the refusal's own message; `Rows`, `Columns` and `Range.Delete` end cut / copy mode; sorting lines already in order
  is an Undo step.

### Added
- `Range.PrefixCharacter`; `Range.FormulaArray` over whole columns enters the array as Excel does.

### Fixed
- Operations over whole columns, whole rows and the whole sheet that froze or ran out of memory now answer in a
  moment: fills, formats, borders, merges, Remove Duplicates, Advanced Filter, Auto Outline, grouping and Show Levels,
  charts and PivotTables over a million rows, Page Layout view, F7, Format Painter, Flash Fill's preview and Row and
  Column Differences.
- Calculation: an edit at the top of a chain of a million formulas, a million formulas reading a whole column and a
  million formulas reading their own cells no longer stall; what reads a cell of a circular array keeps its value.
- Keys that ask the engine (Ctrl+C, the Name Box, F9 in the editor, Format Painter) no longer hold the screen while it
  works.
- `.xlsx`: a dynamic array keeps the range it held, a one-cell array stays an array, an automatic font color stays
  automatic, a sheet-level name keeps hiding the workbook's name, and a damaged file opens as an error.
- Fill Down, Fill Right and the fill handle bring data validation, as in Excel.
- Formulas naming a deleted sheet, or a name through it, are calculated again and stay one formula after a save.
- A table made from header formulas a script just wrote takes the text they show.
- A click on a container's free part keeps the keyboard, so typing no longer writes the cells behind it.
- A hidden YCGrid opens nothing on the user's desktop.

## 0.8.0 (2026-11-14)

### Changed
- Python acts on the rows a filter shows, as Excel 16's object model does (measured): one value or formula written over
  a filtered list, `ClearContents`, `Clear`, formats, `Replace`, `FillDown`, `FillRight` and `Rows.Delete` leave the
  rows the filter hides alone (a formula written over the list is moved into each run of shown rows). Inserting cells
  that would shift a filtered list is refused with Excel's message. A script that cleared a filtered column wiped the
  hidden rows too. Rows hidden by hand still get everything, as in Excel.
- `Window.SplitRow` and `SplitColumn` split a window that isn't frozen (they froze it), and a frozen window stays
  frozen at the new row or column.
- `Range.FormulaR1C1` written over a whole range writes it the legacy way, with `@` where Excel adds it, and a range
  across the sheet's whole width is written as rows (`1:2`).
- `Worksheet.Paste` without a `Destination` pastes into the selection: one copied cell fills all of it.
- `Range.Sort`'s `OrderCustom` is one more than the custom list's number, as in Excel (1 is the normal order).
- Python's `ClearContents` over part of a merged cell raises Excel's "We can't do that to a merged cell.", and a fill
  through part of one does nothing.

### Added
- A workbook keeps its calculation mode and its iterative calculation settings, and opening it sets them, as Excel
  keeps them in the file: a model built on a circular reference opens without the warning on another machine.
- Hidden outline symbols (Ctrl+8) and Show Formulas (Ctrl+`) are saved with the workbook.

### Fixed
- Tables: a whole table cut and pasted goes with its cells (it stayed behind with its header cleared and formulas
  renamed to `Column1`...); clearing a whole table deletes it; a table, a table column or a copied sheet's table
  deleted any way leaves #REF! where formulas named it; a copied sheet works on its own tables and cells; Ctrl+Shift+L
  beside a table filters the list there.
- Filters: Delete, Ctrl+D and Python's writes keep the rows a second table's filter hides; one copied cell pasted over
  a filtered list fills only the rows it shows; the rows a filter hid follow inserted and deleted rows.
- Merged cells, as measured in Excel: cells shifted across part of a merge unmerge it, a merge over part of one grows
  to take it, and AutoFill repeats merges.
- References that follow their cells: data tables and their input cells, scenarios' changing cells, Solver models,
  data labels' Value From Cells, bubble sizes, sparkline date axes, controls' linked cells and lists, and chart series'
  names linked to a cell move with inserted and deleted rows and columns, renamed sheets and moved cells. A defined
  name with relative and absolute parts follows inserted and deleted rows and columns as Excel's does.
- GETPIVOTDATA reads the PivotTable as it was last refreshed, as Excel does: an edit of the source changed the formula
  while the report beside it didn't. PivotTable sources in R1C1 work as recorded macros write them, and rows or
  columns inserted through a report are refused.
- STOCKHISTORY shows #VALUE! for an answer without trading days; it could read past the answer and crash YCGrid.
- Database functions (DSUM, DMAX...) give #VALUE! for a criteria range of labels alone, as Excel.
- Data tools: Consolidate leaves a label's cells without numbers blank; Advanced Filter works out computed criteria for
  each record; Subtotal places its totals as Excel above the detail and beside an earlier subtotal; Apply Names names
  the end of a range a name covers; a refused Forecast Sheet leaves no empty sheet behind.
- Fonts set on a cell reach every character of its text, and a partly bold cell reads `Font.Bold` as Null; a border
  between two cells reads from both and clears from either; rows fit a font or Wrap Text set from Python.
- `NoteText` reads and writes from Start for Length; Create from Selection names labels as Excel; Dependents includes
  whole array formulas.
- Unfreezing panes and removing a split show the rows and columns that were frozen again.
- The Block Arc's and Circular Arrow's handles no longer jump when clicked on a shape wider or taller than round.

## 0.7.0 (2026-11-12)

### Added
- In Design Mode, double-clicking a control opens its event procedure in VS Code, as Excel opens the code window:
  `sheets\<CodeName>.py` with the caret in `CommandButton1_Click()` (each control's default event, `Change` for
  combo boxes, text boxes, spin buttons and scroll bars), added empty when it isn't there yet. The control's
  right-click menu has View Code, and Developer > View Code with a control selected does the same. Properties stay
  on the right-click menu and the ribbon.
- `install.cmd` puts YCGrid on the desktop as well as in the Start menu (`install.cmd /nodesktop` leaves it out), and
  `uninstall.cmd` removes it again (only a shortcut that starts YCGrid). When YCGrid offers to open `.ycg` files with a
  moved YCGrid, a desktop shortcut already there follows it; one deleted stays deleted.

## 0.6.0 (2026-11-11)

### Changed
- Python: a save that fails raises `YcError` with its reason, as Excel's object model raises error 1004. `Save`,
  `SaveAs`, `SaveCopyAs` and `Close(SaveChanges=True)` returned quietly, so a script never knew its file wasn't
  written (a name Windows refuses, a read-only file, a file another program holds open, a folder that isn't there);
  a script that saves where it may fail catches `yc.YcError` now. Only `Workbook_BeforeSave`'s Cancel stays quiet,
  and `SaveCopyAs` into a folder that doesn't exist yet makes it, as `SaveAs` does.

### Added
- Alt+F11 finds VS Code in more places (system and user installs, PATH, Scoop, what Windows lists for it, VS Code
  Insiders). When it still isn't found, it asks for VS Code's program (Browse...) and keeps the answer in the settings
  (File > Options > Advanced > VS Code), or opens VS Code's download page.

### Fixed
- In the Chinese UI, the status line after Alt+F11 made a project was cut text that Python couldn't read: every
  `yc.ui.state()` failed until the line changed. Every translation is now checked to be valid text.
- Formulas give Excel's results in many more corners. 11,565 formulas were measured in Excel 16; the 8,554 where
  YCGrid gives Excel's result are now a test that every later change must keep, and what still differs (digits far
  behind the decimal point, limits where Excel gives up) is listed in `docs/excel-parity-backlog.md`. What changed:
  - Numbers are compared at 15 significant digits, as Excel compares them (`=`, `<`, `>`, COUNTIF and its family),
    and a sum that cancels to the 15th digit is 0 in SUM, AVERAGE, SUBTOTAL and AGGREGATE as well as at the end of
    a formula (`=SUM(0.1,0.2,-0.3)` is 0).
  - `@` of a reference is still a reference: `=ROW(@A1:A10)` is the formula's own row, `=COUNT(@A2)` of an empty
    cell is 0, and ISREF, CELL, OFFSET and COUNTBLANK see the cell (they saw a value).
  - LAMBDA takes optional parameters (`LAMBDA(x,[y],IF(ISOMITTED(y),x,x+y))`) and refuses a name used twice. MAP,
    BYROW, BYCOL, REDUCE and SCAN hand a reference's cells to the LAMBDA as references
    (`MAP(A1:A3,LAMBDA(c,ROW(c)))`) and keep an empty result empty; REDUCE and SCAN without a start value start
    from the first element.
  - TAKE and DROP of a reference give that part of it, still a reference, and take a count of any size; COUNTA
    counts every element of an array, an empty one too.
  - An argument left empty, or a blank given directly: `AVERAGE(,4)` is 2, `COUNT(,,)` 3, `MIN(,5)` 0,
    `MATCH(6,C1:C10,)` an exact match, `LOG(100,)` #NUM!, `CEILING(2.5,)` 0, `LEFT("abc",)` empty,
    `NORM.S.DIST(1,)` the density and `WEEKDAY(45000,)` #NUM!. IF's empty branch hands on a blank
    (`=IF(TRUE,,1)&"x"` is `x`), and VLOOKUP, HLOOKUP, MATCH and LOOKUP look for a blank as 0.
  - Logicals and text: the functions that came from the Analysis ToolPak (QUOTIENT, MROUND, CONVERT, EFFECT, XNPV,
    the bond functions and the others) refuse TRUE / FALSE where a number goes; `AND("a",TRUE)` is TRUE and
    `AND("a","b")` #VALUE!; a unary plus changes nothing (`=+TRUE` is TRUE); MAXA, MINA, VARA, STDEVA, AVERAGEA and
    MODE read logicals and text as Excel's do.
  - Results: MROUND, RANDBETWEEN with fractional bounds, ACOT and ACOTH, NORM.S.INV in the far tail, NUMBERVALUE,
    TEXT of a blank, DOLLAR, DB / DDB / VDB, Z.TEST, TRIMMEAN, and SUBTOTAL / AGGREGATE over error values.
- `CHOOSEROWS` / `CHOOSECOLS` with an index too large for a whole number (`CHOOSEROWS(A1:A3,1E+300)`) read memory
  outside the array; such an index is #VALUE! now, as in Excel.
- Python: `XlError.Number` of #GETTING_DATA, #SPILL!, #CALC!, #CONNECT! and #BLOCKED! is Excel's (2043, 2045, 2050,
  2046, 2047), with `xlErrGettingData`, `xlErrSpill`, `xlErrCalc`, `xlErrConnect` and `xlErrBlocked` in
  `yc.constants`.
- Typing and the keys around it work as in Excel; the same keys were posted to a hidden Excel 16 and to YCGrid, 1,200
  cases compared:
  - AutoSum (Alt+=) over a range never writes over a cell: the totals go to the first row below (column beside)
    that is free in every column, a total already there is redone and anything else is kept. The header row and the
    label column are left out of the sums, a last row or column without numbers gets the totals (in it when empty,
    beyond it when it holds a label, with the grand total in the corner), the selection grows to take in the totals
    as Excel's does, empty cells get the first cell's total moved along, and a block of empty cells or of labels
    opens the editor. On one cell the proposed range is selected, so typing replaces it, and the arrows move it from
    its first cell (Down after AutoSum in B6 pointed at B6, a circular reference).
  - F4 and Ctrl+Y redo after an Undo and otherwise repeat the last command; typing is never repeated and doesn't end
    what they repeat (F4 after an F2 and Enter typed the cell's old value over the next one), and Delete is repeated.
  - Ctrl+Shift+4 shows negative amounts in red, as Excel's Currency format.
  - A number typed into a cell with a percent format is a percentage, Excel's automatic percent entry: 7 is 7% (it
    was 700%), .5 is 0.5%, and the % shows as you type. Python's `Range.Value` still writes the value as given.
  - An entry that starts with + or - is a formula unless it is an amount: `+1+2` is `=1+2` and `-2^2` is `=-2^2`
    (they were text).
  - Ctrl+Shift+arrows from a range jump along the active cell's row or column, not the far corner's, and
    Ctrl+Shift+Home / Ctrl+Shift+End grow the selection instead of starting it again from the active cell.
  - In Enter mode, Ctrl+arrows, Shift+arrows, Home, End, Ctrl+Home, Ctrl+End, Page Up and Page Down end the entry and
    then move on the sheet (they moved the caret, or one cell); while pointing in a formula, Home, Shift+Home,
    Ctrl+End and End, then an arrow, point as Excel's do, and a plain arrow moves on from the reference's first cell.
  - Shift+Tab no longer makes Enter go back to the column where it was pressed.
  - The cell editor takes the keyboard on the frame it opens: keys sent right after F2 or the first letter (Shift+Home,
    Ctrl+Left, Delete, the Enter that takes an AutoComplete suggestion) were lost or met a caret at the start, more
    often on a busy screen.
  - Ctrl+Left / Ctrl+Right move by Excel's words (`=B2+C2` was one word: Ctrl+Left went to its start) and select with
    Shift; Ctrl+Delete deletes to the end of the line; Up on the first line goes to its start and Down on the last
    line to its end (Shift selects to there).
  - Ctrl+Z in the editor takes back the last run of typing at once, and an entry typed over a cell back to the cell's
    content; Ctrl+Y gives it back (it took back one letter, and nothing typed before the editor had the keyboard).
  - F4 changes the cell reference the caret is at, one end of a range on its own (`=SUM(B2:B4)` and F4 is
    `=SUM(B2:$B$4)`), every one selected (they stay selected), or all of one just pointed at; nothing when the caret
    is at no reference (`=B2+5` became `=$B$2+5`).
  - AutoComplete leaves out a list's header row (typing i under Item completed Item), keeps the letters typed as they
    are until Enter takes the entry as the column writes it, counts an entry as long as the text typed (apple beside
    Apple and Apple pie completes nothing), and takes formulas' text and TRUE / FALSE as entries; Alt+Enter over a
    suggestion replaces it.
  - Ctrl+Shift+8 makes the region's first cell active, Ctrl+[ selects side-by-side cells as one area, and a fraction
    typed into a scientific cell shows as a fraction.
- Deleting rows of a filtered list (Ctrl+- or Delete Sheet Rows over rows it shows) kept deleting the rows it hides
  between them; only the rows shown go now, as in Excel. Python's `Rows(...).Delete` still deletes every row, as
  Excel's object model does.
- Data tools measured against Excel 16 through its object model (50 more cases in the measured replay: Remove
  Duplicates, AutoFilter, Replace, Text to Columns, Subtotal, Sort): Text to Columns empties the rest of its block in
  a row with fewer fields (an old value stayed beside the new ones), and Subtotal's grand total sums the rows of detail
  without the last group's total (same value, Excel's formula).
- Big lists stay smooth (a million rows timed step by step with a trader's keys):
  - Ctrl+C of a large range doesn't freeze the window. A copy of more than 250,000 cells writes the text other
    programs paste when one of them asks for it, as Excel does (it stays out of Windows' clipboard history, which
    keeps 4 MB at most an item), and the Clipboard pane collects the copies made while it is shown, as Excel's Office
    Clipboard does (every Ctrl+C wrote out its text for the pane: 1.5 s for a million rows).
  - A paste no longer copies the copied cells again (each paste of six million cells copied them once more).
  - Typing right after undoing a large paste doesn't wait while the undone cells are freed (0.8 s for six million).
  - A color scale, data bar or other rule over 100,000 cells or more gathers its numbers twice a second while the
    data changes, not at every frame (scrolling such a list stuttered), and the status bar's sum of 200,000 numbers or
    more follows a changing list once a second.
  - Python's `Workbook.SaveCopyAs` to `.csv` or `.xlsx` writes the file without stopping the window (1.9 s for a
    million rows).
  - A sheet whose rows Subtotal or Group outlined no longer slows every frame (each frame went through every grouped
    row for the outline's depth: a million after Subtotal), and sorting by a text column takes a quarter less time.
- A save that fails says why: the file is read-only, the folder doesn't exist, the path is longer than Windows takes,
  or Windows doesn't allow the file name (every cause said the folder might be read-only, or that another program
  might hold the file), and a copy left half-written by a full disk is deleted.
- Opening a CSV: a field that typing would turn into a formula stays text, as `=` already did (`+2+3`, `-A1` and a
  phone number such as `+886-2-1234-5678` became formulas), and a million rows open in less than half the time.

## 0.5.0 (2026-11-10)

### Added
- Excel workbooks say what didn't come along. Opening an `.xlsx` / `.xlsm` lists, with counts, what the file held
  that YCGrid doesn't carry over (charts, pictures, shapes, controls, PivotTables, slicers, tables, conditional
  formatting and data validation rules, comments, hyperlinks, sparklines, frozen panes, protection, links to other
  workbooks, data connections, macros), and Save As to an Excel workbook lists what this workbook holds that the copy
  won't before writing it, as Excel's Compatibility Checker does (OK writes, Cancel keeps everything). Python's
  opens and saves stay quiet and put the same list on the status bar and in the Errors window; tests read it from
  `ui.state()["dialog_text"]`.
- The Error Log has a place on the status bar, next to Ready: an "Error Log" entry that is always there and opens
  the window with a click; a small round badge after it counts the errors not seen yet (red) or, when there are
  none, the new messages (the accent color), and the tooltip says which (the red chip used to appear only while
  errors were unseen, and messages never showed). Right-click the status bar to hide it (`status_bar.error_log`);
  `ui.state()["error_log"]` gives the counts.
- Codex reads the YCGrid skill. New projects get a copy in `.agents/skills/ycgrid-python`, the only project folder
  Codex reads skills from, beside the copies for Claude Code (`.claude/skills`) and GitHub Copilot (`.github/skills`);
  **Developer → VS Code** adds it to projects made before, and every copy follows the YCGrid that opens the project.

### Fixed
- Cell memory kept after a burst of work (a big paste, a sheet copy) goes back to Windows after ten idle seconds
  from the blocks that stayed idle, never the one the next edit reuses (the trim could hand back the block just
  freed and allocate again at once, also when a block of another size was the idle one).
- Python: `Union` orders its areas as Excel's when a new area lies inside one already there (it moves to the end:
  `Union(C4:J7, A3:F9, D4:G4)` is `$A$3:$F$9,$C$4:$J$7`; 1,200 unions, intersections, offsets and resizes measured),
  and `Range("C:D").Offset(4, 0)` is an error as in Excel (it gave whole columns again).
- Fill Down, Right, Up and Left of whole columns or the whole sheet (Ctrl+D with columns or everything selected,
  Python's `Range("C:C").FillDown()`) take a moment: a whole column took minutes, and the whole sheet froze YCGrid
  or ran past the sheet's last row and out of memory.
- Python: `SpecialCells(xlCellTypeVisible)` of one cell is the whole sheet's visible cells once rows or columns are
  hidden, as in Excel (`Range("A1")` with rows 2:3 hidden is `$1:$1,$4:$1048576`; it gave `$A$1`). Find and Go To
  Special measured against Excel (500 searches, 480 queries): the same cells otherwise.
- Lookup and text functions as Excel (1,500 formulas measured): XMATCH and XLOOKUP's next smaller / larger finds
  the nearest value of another type when the key's has none (numbers, then text, TRUE / FALSE, empty cells), and an
  empty cell as the key finds an empty cell; TEXTBEFORE / TEXTAFTER take the instance down (-0.5 is -1) and refuse
  one given past the text's length; TEXTSPLIT of a range splits each cell (its first part), refuses an empty
  delimiter and tries the column delimiters before the row ones (`TEXTSPLIT("a-b-c","-","-")` is one row);
  WRAPROWS / WRAPCOLS longer than the vector give it unpadded; SUBSTITUTE's instance refuses TRUE / FALSE.
- Date functions as Excel (1,400 formulas measured): EDATE, EOMONTH, WORKDAY, NETWORKDAYS, WEEKNUM and YEARFRAC
  refuse TRUE / FALSE (#VALUE!), as do the holidays, and a holiday before 1900 or past 9999 is #NUM!; WORKDAY of
  less than one day is the start, whatever the weekend; a weekend of TRUE is 1; YEARFRAC reads its basis first and
  NETWORKDAYS.INTL its weekend first, DATEDIF both dates first; DATEDIF "YD" counts as Excel does
  (2020-02-28 to 2021-03-31 is 32, not 31, and 2024-02-28 to 2027-02-27 is 365).
- Python: `Range("E1#")` of a dynamic array whose result is one value (`=DROP({1;2;3},2)`) is that cell, as in
  Excel (it raised "Invalid address or name" while `=E1#` worked); `Workbook.SaveCopyAs` with an `.xlsx` or `.csv`
  name writes a copy in that format, as Save As does (it wrote a `.ycg` under that name).
- `python/tests/test_release_common.py`: the everyday work checked by its results against Excel's (300 common
  formulas, 30 dynamic arrays, what typing enters, the editor, the common charts, a P&L sheet made with the keys,
  a save / reopen round trip).
- Typing reads what Excel reads (703 inputs measured in Excel; 127 differed): `TRUE ` and ` TRUE` with a space,
  `#CALC!` and `#SPILL!` stay text; a date or time after a space is text; AM / PM needs a space before it (`1:30PM`
  is text); `9:30:`, `9 : 30` and `9:30.` are 9:30; minutes or seconds past 59 (`9:60`, `23:120`) give the number in
  General and `24:60`, `9:60 AM` text; `012:30` is text; the time may come before the date (`9:30 1/5`); spaces
  beside a date's separators don't count (`1 / 2`); a mixed fraction takes a sign, parentheses, a currency mark, a
  thousands separator and a percent (`-$1 1/2`, `1 1/2%` is 1.50%), up to five-digit numerators and denominators;
  `1,2345` and `1234,567` are numbers and `0,123` text; `1e308` and numbers below about 1E-309 are text, those down
  to it 0. `="1,2345"+0`, VALUE, DATEVALUE and TIMEVALUE read text the same way.
- A month and year typed in numbers (`12/2024`, `2/30` for February 1930) is a date only when typed: a formula that
  reads such text (`="12/2024"+0`, VALUE, DATEVALUE, a criterion) gives #VALUE! or compares it as text, as Excel.
- Sort with Case sensitive orders as Excel: the letters first, then lowercase before uppercase, then hyphens and
  apostrophes (`ab-`, `a'b`, `a-b`, `Ab`, `AB`); `a'b` went after `AB`.
- COUNTIF / SUMIF criteria: `<=TRUE`, `>=FALSE` and the other comparisons order TRUE and FALSE, and errors by their
  number (`<=#N/A` counts #DIV/0! too); without `*` or `?` a `~` is itself (`~~` matches `~~`, not `~`), as Excel.
- Deleting every data row of a table (whole rows, or its cells with a shift up) keeps its first data row, emptied,
  as Excel does: the table took in the row below it.
- AutoFill of seeds of several kinds goes on run by run, as Excel (measured on 300 random fills): a formula among
  the seeds no longer stops the others (`Item0`, `=NA()` -> `Item1`); numbers in General join any number format but
  numbers, currency and percentages in their own formats, dates and times each go on by themselves (`1`, `9:30` ->
  `2`, `10:30`; `50%`, `1,000` -> `1.5`, `1001`), a time alone among texts by an hour, a date shown as a month
  (`Dec-24`) by a month; texts of one series in a run go on by their step (`Item10`, `Item2` -> `Item18` above); a
  time counted back past midnight goes round (`1:00`, `0:00` -> `23:00`, no negative time). Fill Days, Weekdays,
  Months, Years and the trends repeat texts but the day names (Days, Weekdays: `Fri` -> `Mon`) and month names
  (Months) they step, and a date among texts moves by their unit.
- Find and Replace look at a text's characters without its leading apostrophe and keep it text (`'123` with `?`
  replaced by `~` is `~~~`, not four), and a lone `~` finds nothing (`~~` finds a tilde), as Excel.
- Remove Duplicates takes TRUE and the text `true` as the same value, as Excel (1 and the text `1` stay two).
- Text to Columns and opening a CSV keep an apostrophe at the start of a field as part of its text (`'5`), and a CSV
  keeps a month and year in numbers (`12/2024`) as text, as Excel.
- Formulas: a space between a range and whole rows is the intersection (`=SUM(B6:F8 2:8)`, `=SUM(2:3 C:D)`; they
  were refused as a missing comma), `$9:2` and `2:$9` are row ranges, and a range typed the other way round is
  kept from its upper-left corner (`=SUM(E7:B7)` is `=SUM(B7:E7)`, `$C5:F$3` is `$C$3:F5`, `G:C` is `C:G`), as Excel
  writes them. Inserting and deleting rows, columns and cells then changes formulas as Excel (150 random cases).
- Functions at their edges as Excel (1,600 random formulas measured): DATE and TIME take each part down to a whole
  number (`DATE(2024,1,-0.4)` is 2023-12-30, `TIME(1,0,-0.4)` 0:59:59), DATE's day stops at 32767 and months back
  before 1900 are #NUM!; ROUNDUP of a tiny number goes up (`ROUNDUP(1E-20,1)` is 0.1, not 0) and ROUNDUP / DOWN
  with hundreds of digits keep the number; FACT(1E+20), GCD / LCM past 2^53 and DAYS / NETWORKDAYS past 9999-12-31
  are #NUM! (FACT gave 1, GCD a negative number); MID past 2^31 and REPLACE past 32767 characters are #VALUE!;
  RANK and RANK.AVG give an error in their numbers, SUMPRODUCT gives one before a size mismatch, and VLOOKUP /
  HLOOKUP of a value not found are #N/A before a column past the table's.
- Data Validation as Excel (400 random rules measured): a bound typed without `=` is a constant read as typed, so a
  date rule between 2024/1/1 and 2024/6/30 and a time rule between 9:00 and 12:00 work (2024/1/1 was 2024 / 1 / 1
  and 9:00 a row range: every date and time rule was wrong); with Ignore blank off a blank is checked as 0 (decimal
  < 5 passes, > 5 fails); an error is never a valid text length; a list item with spaces around it isn't a number.
- AutoFilter custom criteria as Excel (measured on 20 kinds of cells and 29 criteria): equals compares what a cell
  shows (`=1,000` isn't the number shown 1000, `=05` shows nothing); does not equal hides only the criterion's own
  kind (`<>5` keeps the text 5); greater / less than compare numbers with numbers and texts with texts and hide the
  rest (`>=5` hid no text, `<a` showed numbers); `*` never matches an empty cell, and `~~` without a wildcard is
  itself.
- A copied formula whose range corners pass each other is written from the upper-left corner, each part keeping its
  `$`, as Excel (`=SUM($E3:F$5)` copied seven rows down is `=SUM($E$5:F10)`; 120 random copies and cuts measured).
- Python: `Formula` of a cell a dynamic array spilled into is `""`, as Excel (it read the spilled value).
- Python: `Range("A1048577")` (past the sheet's last row) is an error, as in Excel, and the Address of rows 1 to the
  last is whole columns (`Range("A1:A1048576").Address` is `$A:$A`).
- Hiding, showing and sizing rows or columns to the end of the sheet stops at its last row or column: Python's
  `Rows.Hidden = False` took seconds, and a range ending at the sheet's end marker never finished.
- Python: `Rows.Hidden = False`, `Columns.Hidden = False` and `Cells.EntireRow.Hidden = False` show every row
  (column) again, as macros write it; they did nothing (or showed the columns instead of the rows).

## 0.4.0 (2026-11-08)

### New
- **About**, after the ribbon tabs: the version and where the running `ycgrid.exe` is, with Copy Version and Location
  and Open File Location.

- Python: `Range("C1#")` is the spill range anchored at C1, `Application.CalculationState`, `Range.Dirty`,
  `PivotField.DataRange` / `LabelRange`, and `Range.Group` on a PivotTable's labels. A Workbook member that doesn't
  exist raises instead of being kept silently.
- `scripts\basics.py` tries every basic operation in every state a person reaches (editing, pointing, read-only mode
  switched on mid-edit, filters, protection, merged cells, grouped sheets...) and checks that no unexpected message
  appears, Undo and Redo give back exactly what was there, and read-only mode changes nothing;
  `python/tests/test_excel_basics.py` compares 492 basic operations with Excel's measured results; `--sequences`
  runs random chains of operations and checks that Undo and Redo take them all back; both run in English and in the
  Traditional Chinese UI; `--commands` tries every command of the Home tab in the basic states, and `--tab others`
  those of every other tab.

### Changed
- `yc.plot`'s dark trading theme is named `"dark"` (or `"trading"`); its old name is no longer accepted, and a
  misspelled theme raises an error instead of drawing the light one. The examples' helper is `dark_table()`.

### Fixed
- Unhiding every row of whole columns (Ctrl+Shift+9) or setting them back to the standard height no longer freezes
  YCGrid; copying a cell a filter hides copies it (it said the selection was too large); an arrow key typed right
  after a comma or an operator in a formula points at a cell instead of entering the formula.
- Dragging the fill handle of whole rows no longer crashes YCGrid; Fill Down / Right through a dynamic array's spill
  leaves the spill as it is, as Excel does (it showed #SPILL!, and so did Redo of such a fill); Undo of a formula whose
  spill grew the sheet takes the added rows back.
- AutoSum (Alt+=) proposes what Excel proposes (measured from Excel's formula bar): past blank cells to the numbers
  above, blanks included (`=SUM(B2:B8)` in B9), only the totals when there are some (`=SUM(B7,B4)`), then to the
  left; it proposed `=SUM()` under a blank row or a total.
- Undo of a bigger font size takes back the rows it made taller (on empty cells they stayed tall).
- With every column or every row hidden, the keys reach the sheet again, as in Excel (measured): typing, F2, Delete,
  Escape and Undo act on the active cell, Enter and Tab leave it where it is, Down and Up still move with every
  column hidden and no arrow moves with every row hidden; the headers, scroll bars and sheet tabs stay shown (no key
  worked and the window below the ribbon was empty).
- Merge keeps a border on each side of the merged cell only where the whole side had the same line, and clears the
  edges inside, as Excel does (measured); AutoFill by years and months of dates near month ends and Feb 29 as Excel's.
- `Range.MergeCells = True` as Excel's: the upper-left cell keeps its own formats and the other cells go back to
  Normal (`Merge()` gives them all the value cell's formats).
- Basic operations as Excel's, measured: deleting cells trims a range whose edge they held, and cutting cells moves a
  range's edge with them (formulas, names, charts and rules); Merge keeps the first value and gives the merged cells
  its format; AutoFill of seeds with blanks, of one time (by the hour), of two dates by months or years and of month
  ends; `1/0` typed is January 2000; a name that is a union works in SUM, COUNT, AREAS and INDEX and shows as typed;
  a sheet named like a cell (T3) is quoted in A1 formulas only; names like R2D2 are refused as Excel refuses them.
- Formulas as Excel's: OFFSET and INDIRECT over arrays, LET with a name twice is refused, approximate MATCH and
  VLOOKUP over mixed types, a date or time with a leading space is #VALUE!, CELL("address") of another sheet, fraction
  formats (`# ?/??` of pi is 3 1/7), a deleted spill anchor's `#REF!#`, YIELD over one coupon period.
- RATE of a large loan (`RATE(360,-1E7,1E9)`) gives Excel's rate instead of #NUM!, and RATE ends on Excel's last bit
  in 1,925 of 1,953 measured cases; FV and PV are Excel's to the last bit (whole periods by repeated squaring), and
  PMT is within a few units in the last place of Excel's (it was up to 300 units away). IPMT and PPMT keep their
  digits: PPMT where payment and interest nearly cancel (`PPMT(0.08,10,360,8.172E3,0,1)`) was wrong from the fourth
  digit, and IPMT over few periods at a small rate from the fourteenth (measured: within 3 and 23 units in the last
  place of Excel's now).
- SUMX2MY2, SUMX2PY2 and SUMXMY2 without any pair of numbers (blank or text cells) are #DIV/0! (they were 0), and
  of an array of references (`OFFSET(A1,{0,1},0)`) #NUM! (it was #VALUE!), as Excel's (measured).
- Arrays of references as Excel's (measured): `SUMPRODUCT(OFFSET(A1,{0,1},0))` is 0 and AND / OR / XOR of one are
  #N/A (both were #VALUE!), INDEX(..., 0) of one keeps its references, and VAR.S, VARA, STDEV.P, MINA, DEVSQ, NPV and
  the like read each reference's first cell as SUM does; `SUMPRODUCT(SUBTOTAL(9,OFFSET(...)))` sums the visible rows.
- TEXT refuses the number format codes Excel refuses (#VALUE!): an `@` outside the last section (`0;@;0`), a digit
  or date part beside the `@` or in the fourth section (`@0`, `0;0;0;0`), five sections. The last section with an
  `@` is the text section also in two- or three-section codes (`0;\n@` shows a as na; zero goes to the first section
  of `0.0;0.0;@`), and a number only the text section or no condition would take is #VALUE! (`[>0]0;@` of -1).
- With a second window (View > New Window, View Side by Side) the Name Box, Go To and Python's `Select` /
  `Selection` act on the window the keys go to: the Name Box selected in the first window while the typing went
  into the second one's active cell.
- Undo of Page Layout > Themes, Fonts and Effects gives back all of it (the default font and the effects stayed);
  Effects is one Undo step and refused in read-only mode. View > Hide during an entry writes the entry first (its
  editor stayed open in the hidden window).
- A number format code Excel refuses is refused (measured): `Range.NumberFormat = "[Blue"` raises and changes
  nothing, and Format Cells says "cannot use the number format you typed" and stays open with the code to fix
  (an `@` outside the last section, five sections, a quote or bracket left open, `0_`, `[Color57]`, two conditions
  in one section); YCGrid took every code (`[Blue` showed #######).
- Calculation as Excel's: a circular reference keeps its cells' values (the formula that closes it shows 0) and
  what reads it isn't calculated; with iteration the cycle goes on at every recalculation, MaxIterations rounds
  of each cell once, to Excel's numbers, and turning iteration off keeps the values; Worksheet.Calculate no longer
  leaves cells of other sheets stale until a full calculation; Range.Calculate calculates the range only; part of
  an array formula (Ctrl+Shift+Enter) can't be changed from Python or by inserting rows through it either.
- F2 then Enter keeps a long number (123456789012 became 123457000000); a typed entry's format replaces a built-in
  format of another kind; a/b typed into a number format is a fraction; Ctrl+Enter gives every cell the entry's
  format; COUNTIF and the other criteria compare at 15 digits and the lookups exactly; INT(4.35*100) is 435.
- Increase / Decrease Decimal on scientific, fraction, time and General formats; Format Cells opens codes it can't
  rebuild on Custom (picking a date type applied nothing); IndentLevel makes General and centered cells left-aligned;
  a border's Weight makes a dotted or double line continuous; Interior.Pattern = xlSolid on an unfilled cell is
  white. 2,621 number format cases measured in Excel are checked on every build.
- Files: .xlsx files YCGrid writes open in Excel and compute the same: LET and LAMBDA parameters, spill references
  (`A1#`, read back too), #SPILL! and #CALC! values, references to a deleted sheet, and formulas with array
  arithmetic (`=SUM(C1:C3*C1:C3)` became 1 in Excel); the built-in formats 37-40 read as Excel's. CSV files in the
  system's code page (Big5 downloads from brokers) read correctly, and a `sep=;` first line names the separator.
  Save As .xlsm is refused (the file had no macros and Excel wouldn't open it). Ctrl+PgDn / Ctrl+PgUp while typing a
  formula point at the next / previous sheet; hiding or deleting the active sheet goes to the next one on its right.
- Dialogs: picking cells with a range box keeps the sheet's selection (Data Validation applied its rule to the
  cells picked for its list); Escape on an open drop-down in a dialog closes the drop-down only; Insert Function's
  range buttons take a dragged range and Enter brings the dialog back; Custom AutoFilter starts in the value box; Go
  To offers where the last Go To started, selected (F5, Enter goes back); sorting a lone empty cell says so instead
  of opening Sort on it; the PivotTable Fields pane fits the window and its Tools button shows its icon.
- Everyday keys and clicks: clicking another sheet's tab while typing enters the text (a formula keeps pointing at
  the other sheet, and Enter or Escape goes back to it); before, the entry stayed open where no key reached it.
  Delete, formatting and Ctrl+D / Ctrl+R end copy mode (a later Enter pasted over the cells); cut cells paste once;
  the Escape that closes a menu or dialog keeps the copied cells outlined; a column pasted on a row repeats across it;
  a paste that cuts a merged cell is refused; Escape ends drawing a form control; F4 after inserting or deleting whole
  rows repeats that, without the Insert dialog.
- Read-only mode refuses every change the UI could still make (sheets, protection, notes, comments, rules,
  validation, names, objects, outlines, filters, tables, PivotTables, Undo), quietly with its status message:
  commands that would change the workbook don't open their dialogs, and no dialog reports "nothing found" or a blank
  message for a refused change.
- Tables as in Excel: renaming a table renames `=SUM(Table1)` too; INDIRECT reads structured references; ListColumns
  .Add(1) adds the first column; a formula typed over a calculated column replaces it; ShowHeaders off starts the
  table at its data; a text last column is counted in the totals row; headers become text and repeated ones skip the
  names other headers hold; references to a deleted column or table are #REF!; Resize moves the totals row; the
  last data row can be deleted; escaped brackets and leading spaces in column names; `Data!Table1[Qty]`.
- Python: `ListObject.Sort` and `Range.ListObject`; ActiveSheet is the next sheet at once after the active one is
  deleted or hidden; copying a hidden sheet works and a very hidden one is refused; Activate of a hidden sheet does
  nothing (Excel 16); ShowDetail follows the summary row's place; ClearOutline and Ungroup keep collapsed rows
  hidden; outline levels stop at 8.
- Switching to read-only mode (Trading > Operator Mode) while a cell is being edited drops the entry, as Escape does;
  before, every later click went into it as a reference and Enter answered "There's a problem with this formula".
  Read-only mode also offers no drag and drop or fill handle (no "There's already data here" for a move it refuses).
- Escape puts away the Paste Options and Auto Fill Options buttons, as in Excel: left there, they took the next click
  on the cell below and to the right. In a hidden instance Ctrl+F6 no longer switches to the user's YCGrid windows.
- Formulas written but not calculated yet keep calculating after a row or column is inserted or deleted (a script
  that wrote formulas and deleted a row read blanks from them).
- Checked against Excel itself (several thousand formulas and object-model sequences): wildcards match texts that
  hold `*` themselves, and a trailing `~` is dropped; criteria with an operator alone (`"<>"`, `"="`, `">"`) tell
  empty cells from empty texts; SUM, AVERAGE, SUBTOTAL and AGGREGATE cancel to 0 as Excel's do (`0.1+0.2-0.3`);
  texts compare and sort in Excel's order (Windows' word sort: `co-op` after `coop`, punctuation before digits) in
  formulas, SORT, MATCH, COUNTIF, Sort and AutoFilter; SORT and SORTBY put blank cells last; XLOOKUP and XMATCH
  search binary for search mode 2 / -2 and refuse invalid modes and shapes; MATCH reads its type's sign and stops at
  the first equal value in descending data; INDIRECT reads relative R1C1, whole rows and columns and trailing spaces;
  INDEX, ROWS and COLUMNS refuse the shapes Excel refuses; ADDRESS checks its numbers; sheet names starting with a
  period or a digit, or reading as TRUE / FALSE or an R1C1 reference (`R2D2`), are quoted (a formula on a sheet
  named `.5` no longer turned into #N/A after saving); a deleted cell on another sheet is written `Data!#REF!`;
  PROPER, SUBSTITUTE with an instance, TEXTJOIN with a list of delimiters, FIXED, T, N, TEXTBEFORE, TEXTAFTER,
  TEXTSPLIT, NUMBERVALUE, UNICHAR and the 32,767-character limit of text; LAMBDA's argument count; date and time
  parts rounded to the second; date functions on numbers stored as text; ACCRINT, PRICE, YIELD and DURATION for
  every basis; far tails of the distributions and their inverses; MOD, FLOOR.PRECISE, odd roots of negative
  numbers, LINEST with collinear columns, PMT near a zero rate, CONVERT's decimal prefixes on bits and bytes, SMALL,
  LARGE and QUARTILE with fractions, loose times (`0:90`, `1:30.5`), EDATE and EOMONTH limits, ISEVEN / ISODD of
  logical values, and many argument rules.
- Range operations as in Excel: visible cells are not limited to the used range; AutoFill continues month ends by
  months, counts on the last number inside a text (`x1y`, `Day1 PM`), one number among texts, a growth trend along
  the fitted curve and a single `mmm-yy` date by months; Replace takes the shortest wildcard match; Text to Columns
  splits numbers as they show (20240115 into 2024, 1, 15); a write to a merged range fills its first cell only;
  sorting merged cells of different sizes and inserting columns that would push data past XFD are refused with
  Excel's messages; an AutoFilter wildcard matches text only.
- Python: `Range.Rows` / `.Columns` of part of a sheet act on that part; Delete, Insert and Copy act on every area;
  Insert / Delete without Shift follow the range's shape; Find remembers SearchOrder and LookIn and finds empty
  cells; Union joins overlapping areas; `Range.Cells` is a collection; one formula for several areas is relative to
  the first; `Formula` of a constant is its text; `FormatConditions.Add` appends the rule last; `Validation.Value`
  checks stored values and `Validation.Add` refuses what Excel refuses.
- PivotTables: an error in the source shows in the Sum and its totals; items group text without case and keep text
  `"10"` apart from the number 10; cells with only blank records stay empty; GETPIVOTDATA of an empty intersection is
  0; Show Values As counts empty cells as 0; numeric and day group labels follow Excel. Conditional formatting: Above
  / Below Std Dev uses the population standard deviation; text rules test the value, not what it shows; duplicates
  read numbers stored as text as numbers; Blanks takes texts of spaces; color scales at their edges.
- A formula entered in a General cell takes the number format Excel gives it (checked against Excel itself): a
  reference's (`=A1`, `=A1+1`, `=-A1`), the first formatted argument's of `SUM`, `AVERAGE`, `MIN`, `MAX`, `MEDIAN`,
  `ROUND`, `ROUNDUP`, `ROUNDDOWN`, `INT`, `TRUNC` and `MOD`, currency for `PMT`, `PV`, `FV`, `NPV`, `IPMT`, `PPMT`,
  `SLN`, `DDB`, `SYD` and `DB`, `0%` for `RATE`, `IRR` and `MIRR`; `*`, `/`, `&` and two dates subtracted stay
  General. Only the date functions did this before.
- Number formats show what Excel shows (each code checked against Excel itself): fixed denominators (`# ??/100`
  showed 0.5 as `1   1`, now `50/100`) and whole numbers in fraction formats (`1    `, `0    `); the whole part
  without placeholders (`.00` dropped it: 1234.57 showed `.57`); `#.##0` and `0.#0` close up (`.50`, `1.0`); zero
  in scientific formats (`000.0E+0`); a negative number whose digits all show as 0 has no minus (`0`, not `-0`); a
  conditional section drops the minus only when it shows negative numbers alone (`[<0]0;0`, `[>0]0;0`; not
  `[>=1000]#,##0;0`); TRUE and FALSE use the text section (`"Text: "@`, `;;;`); elapsed time alone takes negative
  and very large numbers (`[h]` of -1.5 is -36); and the `*` fill repeats across the cell, so an accounting format's
  `$` sits at the left edge.
- Chinese formats and functions follow Excel's Taiwan edition: `[DBNum1]` and `NUMBERSTRING` read 12 as "one ten
  two" and write a zero inside as a circle, as Excel does, with units up to a trillion (1E+15 was written with an
  exponent); digit placeholders write every digit (`[DBNum1]0`); dates in `[DBNum1]` / `[DBNum2]` / `[DBNum3]` are
  written in Chinese numerals or full-width digits; `aaa` gets the short day names of Taiwan, `[$-804]`, `[$-411]`
  and `[$-412]`; `[$-404]mmm` names the month in Chinese; `e` and `ggg` give the Republic's first year and the years
  before it (they gave -11 for 1900), and `[$-411]` gives Japan's eras.
- `CELL("format")`, `"color"`, `"parentheses"`, `"prefix"`, `"protect"` and `"width"` work (they gave #VALUE!).
- Numbers turned into text (`&`, `CONCAT`, `TEXTJOIN`) keep 15 digits: `=123456789012&""` is `123456789012` (it
  was `1.23457E+11`), `1E-10` `0.0000000001`; a number in a formula keeps 15 significant digits, as in Excel.
- General shows 11 characters besides the minus (`-833.3333333`), and the built-in Comma and Currency styles have
  Excel's codes.
- Typed input as Excel's Taiwan edition reads it: full-width digits and marks are numbers, a date written with the
  Chinese year, month and day marks is a date and an hour-and-minute time with the Chinese marks a time, a time with
  the Chinese AM / PM markers stays text, fractions of a second get `mm:ss.0`, `100:00` is 100 hours,
  `2024/1/7 25:00` stays General and `1/7` shows as month and day.
- Typed numbers get the formats Excel gives them (checked against Excel itself): a decimal point typed gives two
  decimals (`12.5%` is `0.00%`, `1,000.5` `#,##0.00`, `$1,000.5` `$#,##0.00`; they got one), a typed dollar amount
  shows negatives red, in parentheses (`$#,##0_);[Red]($#,##0)`), an `NT$` amount keeps `NT$` (it showed `$`) and
  an exponent wins over the other marks (`1,000e3` is `0.00E+00`). The signs, parentheses and spaces Excel takes
  around a number are taken (`$-5`, `$(5)`, `$ 5`, `NT$-5`, `nt$5`, `( 5 )`, `%12.5`, `12.5 %`, `(12.5)%` were
  text), `-NT$5` is an amount rather than the formula `=-NT$5`, and what Excel keeps as text stays text (`(-5)`
  was 5, `$5%` 5%, `13:00 pm` 1:00 PM).
- Text to Columns formats a Text column's cells as Text (`@`), as Excel does, so what is typed there later stays
  text too.
- A formula whose last step is an addition or a subtraction that cancels out shows 0, as Excel's does (Microsoft's
  KB 78113): `=A1-B1` of `0.1+0.2` and `0.3` is 0 (it showed 5.55E-17). As in Excel, a product or parentheses as
  the last step keep the residue (`=1*(0.5-0.4-0.1)`, `=(0.5-0.4-0.1)`).
- F4 toggles whole columns and rows while a formula is typed: `=SUMIF(A:A,...)` becomes `$A:$A`, `1:3` `$1:$3`
  (it did nothing).
- The fill handle dragged across moves a formula's table columns along, as in Excel: `=SUM(Sales[Jan])` filled right
  gives `Sales[Feb]`, `Sales[Mar]` (each kept `[Jan]`), `[@Qty]` gives `[@Px]`, and past the table's last column
  `#REF!`; copy and paste and Fill Right (Ctrl+R) keep the names, as Excel's do. The totals row writes its own
  columns as Excel does: `=SUBTOTAL(109,[Price])`.
- A cell formatted as Text keeps what is typed as text, as in Excel: `00123` keeps its zeros, `30` stays text and
  `=A1` shows as typed (all became numbers or formulas), from the keyboard and from Python's `Value` / `Formula`.
- `Range.PasteSpecial` and `Worksheet.Paste(Destination)` paste over the whole destination as Excel does: one copied
  cell divides, multiplies or fills every selected cell, and a copy that fits a whole number of times repeats (only
  the first cell was pasted).
- Cut and paste and drag and drop move what reads the cells too, as in Excel: defined names, chart series,
  conditional formats and data validation follow the moved cells (names and charts stayed on the emptied cells,
  the formats' rules stayed behind and the validation was dropped), and a rule partly on the moved cells is split.
  Formulas reading the cells written over show `#REF!`, as Excel's do (they read the pasted values).
- A reference to a deleted sheet keeps its cell part, as Excel writes it: `=Sheet2!A1+1` becomes `=#REF!A1+1` (it
  became `=#REF!+1`), and such a formula can be typed, saved and opened again. Deleting the sheet at one end of a
  3-D reference ends it at the next sheet inside (`Sheet1:Sheet3` becomes `Sheet1:Sheet2`), as Excel's help says;
  it became `#REF!`.
- AutoFill continues a text series by the step its cells show, as in Excel: `Jan`, `Feb` dragged down gives `Mar`,
  `Apr`, `May` (it gave `Feb`, `Mar`, `Mar`, `Apr`), `Jan`, `Apr` gives `Jul`, `Oct`, `Jan`, and `Item 1`, `Item 3`
  gives `Item 5`, `Item 7`; quarters and ordinals likewise. Three or more numbers follow their best-fit line
  (`1`, `3`, `4` gives 5.67, 7.17, 8.67; it gave 5.5, 7, 8.5).
- Typed times take `a` / `p` after a space and the hour alone, as Excel's help says to type them: `9:00 p` is
  9:00 PM and `3 PM` 3:00 PM (both stayed text), in any case; `TIMEVALUE` reads them too. `1e` (an exponent without
  digits) stays text (it became the number 1).
- AutoComplete completes from the first character typed, as in Excel: one Chinese character (as an IME commits it)
  finishes a name from the column, and `a` finishes `Apple` (the first character never completed, so a one-character
  Chinese start never did); case is ignored for accented and full-width letters too.
- Find and Replace (without Match case), filters by value, Remove Duplicates and conditional formats ignore the case
  of accented and full-width letters as formulas now do: Replace of `café` changes `CAFÉ` too, and the filter list
  shows one entry for both (only ASCII letters were folded).
- Wildcards match whole Chinese characters: `COUNTIF` with a family name and `?` counts the two-character names, and
  `SEARCH("?" & a character, ...)` finds the character before it (`?` matched one byte of a character, so these found
  nothing or the wrong place). Text comparisons that ignore
  case fold accented, Greek, Cyrillic and full-width letters too (`MATCH("CAFÉ",...)` finds `Café`), and `UPPER` /
  `LOWER` change them.
- `NUMBERSTRING(n,1)` / `(n,2)` read numbers as the `[DBNum1]` / `[DBNum2]` formats do: 10 reads "ten" (it read
  "one ten") and a zero inside a number is the character for zero, as Excel writes 85073.
- Chinese number formats work as in Excel: `[DBNum1]` writes 1234 in Chinese numerals and `[DBNum2]` in the financial
  ones (the cell showed nothing at all), `[DBNum3]` full-width digits; `aaa` / `aaaa` give the day of the week in
  Chinese (they showed the letters), and `[$-zh-TW]` / `[$-404]` `mmmm` the Chinese month name. A format of
  modifiers only (`[Red]`, `[$-404]`) shows the value as General (it showed nothing).
- `Precedents`, `DirectPrecedents`, `Dependents` and `DirectDependents` return the cells as one set, as Excel's: `=B2+C2`
  traces `$B$2:$C$2` (it was `$B$2,$C$2`), and an area inside another is left out.
- Without iterative calculation a circular reference shows 0, as in Excel: `=C1+1` in C1 showed 1, and grew by one at
  every full calculation.
- `Comment.Text(Text, Start, Overwrite)` inserts at character `Start`, or writes over the text there with
  `Overwrite=True` (it replaced the whole note); `CommentThreaded.Text` too.
- `Union` joins areas that touch into one block, as Excel's: cells added one by one in a loop make `$A$1:$A$10`, not
  ten areas.
- Ctrl+arrow keys, Ctrl+Shift+arrows and `Range.End` pass over hidden rows and columns, as in Excel: Ctrl+Down in a
  filtered list stops at its last shown row (it stopped on hidden cells, and the active cell disappeared).
- Enter and Tab in a selection pass over hidden rows and columns and the inside of merged cells.
- A selection takes in the merged cells it touches, whole, however it is made: Shift+arrows or a drag from A1 into a
  merged B2:C3 select A1:C3 (`Selection` read A1:B2), and Shift+Space / Ctrl+Space on a merged cell select all its
  rows / columns.
- A data validation's or conditional format's formula follows inserted and deleted rows and columns, as cell
  formulas do: a Whole Number limit `=$A$10` becomes `=$A$12` when two rows go in above row 10 (it kept pointing at
  row 10), and a rule's relative references move with its cells.
- A list validation accepts a number by its value: 1 is in a list showing `$1.00`, 10.0 in the list `10,20` (they
  were refused as "not in the list").
- A copied sheet's formulas that name the source sheet (`=Src!A1` on Src) name the copy, as in Excel (they kept
  reading the source).
- `Worksheet.Visible = xlSheetVeryHidden` is kept (it became Hidden): the sheet isn't offered by Unhide Sheet, and
  the setting is saved in `.ycg`. Hiding the last visible sheet raises an error instead of doing nothing, and
  `Activate` / `Select` of a hidden sheet raise as in Excel.
- `yc.launch(path)` returns once the workbook is read: it returned while a large file was still loading, so another
  YCGrid's `CompareSideBySideWith("Book.ycg")` or `Workbooks("Book.ycg")` didn't find it yet.
- A filter by values ignores case, as Excel's list does: `AAPL` and `aapl` are one entry, and checking it shows
  both (only the exact text was shown). `AutoFilter.Filters(i)` returns column i's Filter (`On`, `Criteria1`,
  `Criteria2`, `Operator`) as recorded macros read it (it raised `TypeError`).
- A Text that Contains conditional format takes Excel's wildcards: `a*c` finds `abc` and `ac` (it looked for the
  characters `a*c`); `~*` finds a real `*`.
- A negative number that rounds to zero keeps its sign, as in Excel: -0.4 in the format `0` shows `-0`, and
  `TEXT(-0.001,"0.00")` is `-0.00`. A negative date or time, or one after 9999, fills the cell with `#` (it showed
  `#NUM!`), and `TEXT` of it is `#VALUE!`.
- Typed times keep Excel's look: `10:30 pm` shows `10:30 PM` (format `h:mm AM/PM`, it showed `22:30`), and `25:00`
  shows `25:00:00` (format `[h]:mm:ss`, it showed `1:00`).
- A range's `NumberFormat` and `Text` read one value, as in Excel: the format or text its cells share, `None` when
  they differ (they returned a list per cell, so `rng.NumberFormat == "0.00"` was never true); writing
  `NumberFormat = None` back changes nothing (it set the format `None`). `Font.Superscript` and `Subscript` of cells
  that differ read `None` (they read `False`), `Font.FontStyle` reads `None` (it raised `KeyError`), and
  `Interior.PatternColor` reads `None` (it read 0, and writing that back cleared every cell's pattern color).
- `Worksheet_Change` gets the Target Excel gives: Ctrl+Enter on F1:F2 is `$F$1:$F$2` (it was `$F$1,$F$1:$F$2`, Count 3),
  a sort `$J$1:$J$3` (it was `$J$1,$J$2,$J$3`), and Ctrl+D / Ctrl+R the whole range filled (the first row was left
  out).
- `PivotCaches.Create(...)`, `ActiveSheet.PivotTables.Count`, `Scenarios.Add(...)`, `Series.Trendlines.Add()` and
  `Series.DataLabels.ShowValue` work as recorded Excel macros write them, without parentheses (they raised
  `AttributeError`); `PivotCaches()`, `PivotTables("PT1")` and the like still work.
- Shift+Enter and Shift+Tab move the active cell up and left, as in Excel (on one cell they extended the selection
  instead).
- Alt+Down on a cell without a list or filter button opens Pick From Drop-down List, the column's entries, as in Excel
  (the shortcut was only written on the menu).
- The status bar's Average, Count and Sum leave out hidden rows and columns, as Excel's do.
- `Selection.Address` of whole columns reads `$C:$C`, and of the whole sheet (`Cells`, Ctrl+A twice) `$1:$1048576`, as
  in Excel (they read `$C$1:$C$1048576` and `$A:$XFD`).
- `Application.Evaluate` and `WorksheetFunction` return a whole array result (`Evaluate("{1,2,3}*2")`,
  `WorksheetFunction.Transpose(...)`) instead of its first value.
- Writing a one-row array to a taller range repeats it down, and a one-column array across a wider range, as in
  Excel (`Range("A1:A3").Value = [1, 2, 3]` wrote 1, #N/A, #N/A).
- `Series.Values` and `XValues` read the values, as in Excel (they returned the range's text, which
  `Series.Formula` has); a data label without text of its own reads its value.
- `ActiveWindow.Zoom` outside 10-400 raises an error instead of being clamped; `Zoom = True` zooms to the
  selection. A new table column with no header is `Column1` (the first free `ColumnN`), as in Excel.
- `Offset`, `Resize` and `Cells` past the sheet's last row or column raise error 1004, as in Excel (below row
  1,048,576 they made a range off the sheet).
- Copying a sheet copies its sheet-level names, pointing at the copy, as in Excel (formulas on the copy that used
  them were `#NAME?`). Deleting a sheet deletes its sheet-level names, and the names, chart series and rules that
  referred to it become `#REF!`: a sheet added later under the same name no longer takes the deleted one's place.
- A workbook that can't be read no longer leaves a window that looks like it: started with a broken file, YCGrid
  shows the error over an untitled Book1 that Save doesn't write over the file with, and `Workbooks.Open` raises the
  error instead of returning that blank workbook.
- `Worksheets.Add()` without `Before` or `After` adds the sheet before the active one, as in Excel (it went to the
  end).
- `Range.Copy(Destination)` brings the notes and data validation, as Ctrl+C / Ctrl+V does; `AddComment` on a cell
  that has a note raises Excel's error instead of replacing it.
- `Validation.Value` sees a rule and a value Python has just written (it answered from before them);
  `MergeCells` of a range only partly merged reads `None` (Excel's Null); `Range("B2:D4")(2, 2)` is the cell
  C3, as in Excel.
- A hidden YCGrid (`yc.launch(hidden=True)`, tests, agents) no longer opens VS Code windows: Developer > VS Code,
  View Code and Debug in VS Code make the project and say what they would open (`ui.state()["vscode_request"]`).
- Dialogs open at Excel's sizes: a dialog keeps its width and only its height follows the contents, so long sentences
  wrap instead of stretching it (Encrypt Document, Protect Sheet, Protect Structure and Windows, Unprotect, Confirm
  Password); a dialog whose table needs more room widens to show it all (Sort, Edit Links, Move Chart, Resize Table,
  Page Setup); Data Validation keeps one size, with its buttons at the bottom. Go To, Insert, Delete, Unhide, Move or
  Copy, Zoom, Insert Function and Paste Special no longer leave empty space under their buttons; Row Height, Column
  Width and Standard Width read "Column width: [8.43]" as in Excel; Protect Sheet's list scrolls instead of making the
  dialog taller than the window.
- Insert Icons: the icons are drawn on a transparent background again, in the dialog and on the sheet (they were dark
  gray squares).
- The wheel no longer carries the license of a font YCGrid doesn't include (left in the build folder by an older
  build).
- Texts YCGrid shortens are cut between characters, never inside one: a CSV with a long Chinese file name gave its
  sheet a broken name that made every Python and MCP request fail to decode; the undo list's "Typing ..." label, the
  Clipboard pane's previews and Import Data's sheet name had the same cut. Sheet names are limited to 31 characters
  (93 letters were accepted).
- Row Height, Column Width and Standard Width refuse a value outside Excel's range with Excel's message and keep the
  text to fix (a height of -5 silently hid the rows; 500 became 409).
- Typing `1e309` keeps the text, as Excel does (it became `#NUM!`).
- A `.xlsx` whose ZIP directory lies about sizes or offsets is reported as damaged instead of allocating what it
  claims; a crafted `.xlsx` or `.csv` that the importer can't read is an error message, not the end of YCGrid.
- File > Open > Recent: an entry whose file is gone offers to remove it from the list (Excel), instead of "Can't open
  the file" with the entry staying.
- The examples' background threads end quietly when YCGrid closes (they printed a traceback per loop).
- A write that protection refuses always raises `ProtectedError` in Python: the engine answered the request before
  it had counted the refusal, so under load some refused writes came back as done.
- `AGGREGATE` ignores error values with options 2, 3, 6 and 7 (they had no effect), counts nested SUBTOTAL /
  AGGREGATE cells with options 4-7, and has its array form: 14 LARGE, 15 SMALL, 16-19 percentiles and quartiles.
- `DOLLAR` rounds to tens and hundreds with negative decimals and shows negative amounts in parentheses
  (`DOLLAR(-1234.567,-2)` is `($1,200)`, as Excel's examples); `LOG(x,1)` is `#DIV/0!`; the `am/pm` and `a/p`
  markers of a number format keep the code's case.
- Python properties take back what they read: `Range.FormulaArray` of several plain cells reads `None` (it read
  the first cell, and writing that back turned the whole range into one array formula) and text without `=` is a
  constant; a conditional format's `Font.Bold`, `Italic`, `Underline` = `None` leave the cell's own (`Underline =
  None` underlined), its fill, border and data bar border colors take `None`; `ThemeColor = None` and
  `Shadow.Style = msoShadowStyleMixed` change nothing; `ChartTitle.Formula` takes plain text as the title; and
  `Range.ValueNs` takes back text and error values.
- F4 / Ctrl+Y repeat an entry that was handed to a busy engine.
- Numbers written by Python below or right of a table grow the table, as text did (`Range.Value = 5` under a table
  left it as it was); a block written there grows it once to take the whole block, and its calculated columns fill
  the new rows.
- Dates typed with a month's name are dates, as in Excel: `15-Jan-2024`, `Jan 15, 2024`, `Monday, January 15, 2024`,
  `15-Jan` (this year) and `Jan-2024` (its first day), in the formats `d-mmm-yy`, `d-mmm` and `mmm-yy`, and the date
  functions read them (`DATEVALUE("22-MAY-2011")` was `#VALUE!`).
- A defined name with relative references works as Excel's relative names: defined with B1 active, `=Sheet1!A1` is
  the cell left of the one that uses it (it was always A1). The Name Manager, the Name Box, `RefersTo` and
  `RefersToR1C1` show it from the active cell; workbooks from Excel with such names now give Excel's results, and
  names in workbooks saved by earlier versions keep working as the absolute references they were.
- `=Sheet1!Price` refers to the name `Price` defined for Sheet1 (it was refused as an error).
- `Range.Formula = "=TODAY()"` from Python shows a date, as typing it does (every cell of a range written at once
  too); `DATEVALUE`, `TIMEVALUE`, `EDATE`, `EOMONTH` and `WORKDAY` no longer get a date format of their own, as in
  Excel, where they show serial numbers.
- `Range.Value` of a date cell holding a number no date shows (negative, past 9999-12-31) reads the number (it
  raised `OverflowError`).
- `ColumnWidth`, `RowHeight` and `Hidden` of rows or columns that differ read `None` (Excel's Null) and writing
  `None` back changes nothing (`Hidden = None` showed every row); a width past 255 or a height past 409 points
  raises an error instead of being taken. A sheet can't be named `History`, as in Excel.
- Renaming a sheet rewrites the defined names, chart series, conditional formats and validation lists that refer to
  it (a name kept `=Old!$A$1:$A$3` and stopped referring to anything).
- `Range.Find` without `After` starts after the range's upper-left cell and searches it last, as in Excel (it found
  the first cell first). `Range.Sort(Header=xlGuess)` guesses the header (it sorted the header into the data), the
  Sort object's `Header = xlGuess` no longer raises, and `Range.Sort` on one cell sorts its current region.
- `Worksheet.Copy(Before / After)` makes the copy the active sheet, as Excel does.
- Go To Special (`SpecialCells`), Sort, Remove Duplicates, Subtotal and Row / Column Differences from Python see the
  values of formulas written just before (`=1+1` written and then looked for wasn't a formula yet).
- On several cells whose formats differ, `Font`, `Interior` and alignment properties read `None` (Excel's Null) and
  writing `None` back changes nothing (they read the first cell's, and writing that back changed every cell).
- `TEXTBEFORE` / `TEXTAFTER` take `match_end` and a list of delimiters (`{",",";"}`), and an instance past the
  text's length is `#VALUE!`; `NUMBERVALUE("9%%")` is 0.0009, and a group separator after the decimal one is
  `#VALUE!`.
- `yc.data.view` and `plot_files` read 1- and 2-byte numbers, half floats and bytes (`int8`, `int16`, `uint8`,
  `uint16`, `float16`, `S`): a DataFrame with such a column failed, and `save_columns` turned bytes into floats and
  failed. A `.npy` file that is cut short, holds Python objects or gives sizes it doesn't hold is an error message
  (one of them closed YCGrid).

### Project
- Each release file (the zip and the wheel) comes with a `.sha256` file, since YCGrid is not code-signed yet; the
  README says how to check a download and what to do when SmartScreen warns.
- The wheel's metadata names the license (Apache-2.0) and every license file it carries, links
  to the source, issues and changelog, and its description's links and pictures work outside the repository.
- [docs/releasing.md](docs/releasing.md): every step of a release and what to set up when the
  repository goes public. The build workflow keeps vcpkg's libraries between runs, runs on version tags and leaves
  the zip and the wheel as its artifact.
- Every source file carries the license line `SPDX-License-Identifier: Apache-2.0`, and the boundary check fails on
  a new file without it. A test checks that the five files naming the version agree.
- Dependabot keeps the workflows' actions up to date; CODEOWNERS asks the maintainers to review pull requests.
- Tests of the MCP tools against a hidden YCGrid (`tests.test_mcp_live`: every tool, by what it did) and of the pure
  Python parts (`tests.test_pure`: the paper market and exchange, the project environment scan, the column frame
  codec, the command line); the Python package's measured line coverage goes from 60% to 65%, `yc.mcp.tools` from
  20% to 90%.
- `scripts\coverage.cmd`: the C++ line coverage of the Catch2 unit tests (OpenCppCoverage), per directory and per
  file. New unit tests of everyday worksheet functions, Ctrl+arrow jumps, whole-row and column formats, CSV export,
  conditional formats over names and tables, the ToolPak's charts, objects, names and table rows and columns take
  the engine, formulas, file formats, model and core from 85% to 89%; `tests.test_object_sweep` reads every
  property of the object model and writes each read-write one back (the Python package from 65% to 70%).

## 0.3.1 (2026-11-03)

### Fixed
- Dialogs that fit their contents (Password, Encrypt with Password, AutoCorrect, the screenshot tool, Spelling and
  others) no longer shrink a little every frame while they are open; they keep the width they opened at.
- `ui.state()["shrinking_windows"]` names any window that still narrows by itself, and the UI sweep fails on it.

### Project
- Contributing guide, security policy, code of conduct, issue and pull request templates, this changelog.
- The documentation was brought up to date with the ribbon, File and Options of 0.3.0 (features.md, settings.md,
  the README), and the old audit notes were folded into `docs/excel-parity-backlog.md`.
- `scripts/check_boundary.py --history` also scans everything the published branches ever committed.
- A light check (translations, command coverage, boundary, Python tests that need no YCGrid) runs on every push and
  pull request.

## 0.3.0 (2026-11-03)

Excel users' habits: the keys, the menus, the ribbon, File and Options.

- KeyTips with Excel's letters on every tab and inside drop-downs and submenus (Alt, H, O, I autofits a column).
- Excel's missing shortcuts: F12 / Alt+F2 (Save As), Shift+F12, Ctrl+F12, Ctrl+Shift+F12, Alt+Shift+F1,
  Ctrl+Shift+T, Alt+', Ctrl+F6 / Ctrl+Tab between windows.
- The mini toolbar above the right-click menu; Excel's paste pictures in Home > Paste, the Paste Options button and
  the cell menu; Quick Analysis (Ctrl+Q); View > Focus Cell; a customizable status bar.
- Ribbon group names and order as Excel's; chart galleries with pictures; a PivotTable Styles gallery; an icon for
  every command.
- File opens on Home; pin, remove and search recent workbooks; Options has Excel's categories and new settings
  (Quick Analysis, a new workbook's font and sheets, zeros, gridline color, sheet tabs, scroll bars, paste and
  insert option buttons, the overwrite alert), reachable from Python with Excel's names.
- Mouse cursors over cells, the fill handle and the headings have Excel's size.

## 0.2.1 (2026-11-02)

### Fixed
- The title bar's X closes YCGrid again, and the minimize button and the search box react to the mouse again.

## 0.2.0 (2026-11-02)

- Cells and keyboard: grouped sheets, right-drag menus, Shift-drag to insert, Repeat (F4 / Ctrl+Y), filter by the
  selected cell, Pick From Drop-down List, more of Excel's Ctrl shortcuts.
- Home: the Format Cells orientation dial, Draw Border tools, Format as Table gallery with live preview and custom
  styles, every Paste Special choice.
- Formulas: background error checking with Excel's rules, Evaluate Formula, trace arrows across sheets, Apply Names,
  Paste Names (F3), Watch Window.
- Data: Subtotal, sort options, date grouping in filter lists, Forecast Sheet, From Text/CSV, Flash Fill as you type,
  Solver options and reports.
- Charts: every chart type of Excel's Insert Chart dialog, Recommended Charts, Chart Styles and Chart Elements
  buttons, date axes, multi-level labels, linked titles and labels, richer fills.
- PivotTables: Field Settings, grouping, the whole Analyze tab, PivotCharts, slicers for tables, Report Connections.
- Insert and Review: Excel's Shapes gallery with adjustment handles and effects, Symbol, Icons, WordArt, sparklines,
  spelling options and AutoCorrect, Allow Edit Ranges.
- Page Layout view, Page Break Preview, window arrangement, Custom Views, themes and printing options.
- Security: Python in a workbook's folder runs only after Enable Content, with a Trust Center; downloaded files open
  in Protected View.
- Python: every UI command has a member with Excel's name; workbook and sheet events fire as in Excel.

## 0.1.4 (2026-11-01)

### Fixed
- Saving runs in the background, so live data keeps updating while a large workbook saves.
- Alt+Enter typed quickly, AutoComplete, `Range.Borders` on several cells, the font and size boxes, times without a
  date, form control names, focus after inserting a shape, several dialogs' first field, New Window and Arrange All,
  Load Layout, long menus, hidden instances ending with their script.

## 0.1.3 (2026-10-31)

### Fixed
- Trading > Record records the whole screen when no table is marked as market data, instead of an empty file.

## 0.1.2 (2026-10-31)

### Changed
- The Missing Python packages bar installs every package the code needs at once and reruns it; resolved
  "No module named ..." errors stop counting. New `yc.ui.resolve_errors`.

## 0.1.1 (2026-10-31)

### Fixed
- Trading > Examples did nothing when a package it needs wasn't installed yet; ten ribbon buttons had no icon.

## 0.1.0 (2026-10-31)

The first public release.
