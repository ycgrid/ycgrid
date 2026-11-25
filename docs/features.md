# YCGrid features: a user guide by ribbon tab

What YCGrid can do and where to find it, arranged the way Excel's ribbon is. Every button, menu item and shortcut is a
command with an id (shown in `code`). Python runs any of them with `yc.ui.command(id)`
(`Application.CommandBars.ExecuteMso(id)`), and [ui-coverage.md](../skill/ycgrid-python/references/ui-coverage.md)
lists the Python members with Excel's names that do the same. Items marked [YC] are YCGrid's own; everything else
works like Excel.

Can't find a command? Press **Alt+Q** (Tell Me, `CommandSearch`) and type its name, or press **Alt** for KeyTips.

## The window

- **Title bar**: the Quick Access Toolbar (Save, Undo, Redo and Screenshot by default; right-click any ribbon command
  and choose Add to Quick Access Toolbar; `CustomizeQat`), the workbook name, and the ribbon display options
  (`RibbonAlwaysShow`, `RibbonTabsOnly`, `RibbonAutoHide`; Ctrl+F1 collapses the ribbon).
- **Ribbon**: the tabs below; contextual tabs that appear while a table, PivotTable, chart, sparkline, shape, picture,
  slicer, timeline or header / footer is selected; and the tabs a workbook adds with its own `customUI.xml` (RibbonX,
  see [events-and-controls.md](../skill/ycgrid-python/references/events-and-controls.md)). A group's corner button
  opens its dialog (Font, Alignment, Number, Page Setup...). [YC] **About**, after the tabs, shows the version and
  where the running `ycgrid.exe` is, with Copy Version and Location (`AboutCopyInfo`) and Open File Location
  (`OpenProgramLocation`).
- **Name Box and formula bar**: go to a cell, range or name; define a name by typing it. The formula bar expands to
  three lines with its chevron or Ctrl+Shift+U (`ExpandFormulaBar`), and its bottom edge drags down to 20 lines; `fx`
  opens Insert Function.
- **Grid**: 1,048,576 rows by 16,384 columns; rows grow on demand (`ExpandRows100M` grows a sheet to 100 million rows).
  Headings, gridlines, frozen and split panes, outline buttons, AutoFilter buttons, the fill handle, error checking
  triangles and their options button, note and comment markers, and hyperlinks (one click follows them; see
  `editing.follow_links_on_click` in [settings.md](settings.md)).
- **Working with cells**: the mini toolbar above the right-click menu; the Paste Options and Insert Options buttons;
  Quick Analysis at a selection's corner (Ctrl+Q: formatting, charts, totals, tables, sparklines).
- **Sheet tabs**: click to switch, double-click to rename, drag to reorder, Ctrl+click to group sheets, the **+**
  button (`NewSheetAfter`), and the right-click menu (Insert, Delete, Rename, Move or Copy, View Code, Protect Sheet,
  Tab Color, Screenshot of Sheet / Workbook, Hide, Unhide, Select All Sheets). [YC] Drag a tab off the bar to float
  the sheet in a window of its own.
- **Status bar**: the mode (Ready, Enter, Edit, Point, Extend Selection, Add to Selection, End Mode), Calculate in
  manual mode, a red chip with the number of new errors (it opens the Error Log), the selection's Average, Count,
  Numerical Count, Min, Max and Sum, Caps Lock / Num Lock / Scroll Lock, the view buttons and the zoom slider
  (10% to 400%; click the percentage for Zoom). Right-click it to choose what it shows.
- **Panes** (dockable; `yc.ui.pane(name)` opens them from Python): Clipboard, Watch Window, Comments, PivotTable
  Fields, Format (shapes, pictures, chart elements), Selection, Immediate, Error Log, and [YC] Heartbeats, Change Log,
  Alerts, Latency Monitor and the Performance panel. A docked window floats when its tab is dragged out or
  double-clicked.

## File (Backstage)

| Page | What it does |
|---|---|
| Home | a blank workbook and the recent workbooks |
| New | a blank workbook (Ctrl+N, `FileNew`) |
| Open | Browse (Ctrl+O, Ctrl+F12, `FileOpen`) and the recent workbooks: pin, remove and search them; opens `.ycg`, `.xlsx` / `.xlsm` and `.csv` |
| Info | Protect Workbook (Encrypt with Password: AES-256, see [ycg-format.md](ycg-format.md); Protect Workbook Structure), Manage Workbook (Recover Unsaved Workbooks), the workbook's properties, and About (versions, sheet and style counts) |
| Save, Save As | Ctrl+S / Shift+F12 (`FileSave`), F12 / Alt+F2 (`FileSaveAs`): `.ycg`, a copy as `.xlsx`, or CSV |
| Print | printer, copies, what to print (active sheets, entire workbook, selection, ignore print area), pages, collation, two-sided printing, Page Setup, and a preview of the pages (Ctrl+P, `FilePrint`; `PrintPreview`, `PrintNow`) |
| Export | Create PDF; Change File Type (YCGrid workbook, Excel workbook, CSV, PDF); the current sheet as CSV (`ExportCsv`) |
| Close | Ctrl+W / Ctrl+F4 (`FileClose`) |
| Options | Excel's categories: General (Quick Analysis, the font, size and number of sheets of a new workbook, language), Formulas (calculation, iteration, R1C1, error checking rules), Data, Proofing, Save (AutoRecover), Advanced (editing, cut / copy / paste, display options for the workbook and the sheet, live data, projects folder, VS Code), Trust Center; see [settings.md](settings.md) |

## Home

| Group | Commands |
|---|---|
| Clipboard | Paste with Excel's paste pictures (Paste, Formulas, Formulas & Number Formatting, Keep Source Formatting, No Borders, Keep Source Column Widths, Transpose, Values, Values & Number Formatting, Values & Source Formatting, Formatting, Paste Link, Picture, Linked Picture) and Paste Special (Ctrl+Alt+V); Cut, Copy, Copy as Picture, Format Painter (double-click keeps it on); the Clipboard pane |
| Font | font and size boxes, Increase / Decrease Font Size, Bold, Italic, Underline / Double Underline, Borders (presets, Draw Border, Draw Border Grid, Erase Border, Line Color, Line Style, More Borders), Fill Color, Font Color (theme and standard colors, More Colors); the Font tab of Format Cells; Strikethrough is Ctrl+5 |
| Alignment | top / middle / bottom, left / center / right, decrease / increase indent, Orientation (angle, vertical text, rotate up / down), Wrap Text, Merge & Center, Merge Across, Merge Cells, Unmerge Cells |
| Number | the format box (General, Number, Currency, Accounting, Short / Long Date, Time, Percentage, Fraction, Scientific, Text, [YC] Timestamp (ns)), Accounting Number Format ($, £, €, ¥, NT$, CHF), Percent Style, Comma Style, Increase / Decrease Decimal, More Number Formats |
| Styles | Conditional Formatting (Highlight Cells Rules, Top/Bottom Rules, Data Bars, Color Scales, Icon Sets, New Rule, Clear Rules, Manage Rules), Format as Table (a gallery of every table style with live preview, New Table Style, a default style; [YC] a Trading style), Cell Styles (the built-in gallery, New Cell Style, Merge Styles; right-click a style to modify it) |
| Cells | Insert (cells, rows, columns, sheet), Delete, Format (Cell Size, Visibility, Organize Sheets and Protection, as in Excel: row height, column width, AutoFit, default width, Hide & Unhide, Rename Sheet, Move or Copy, Tab Color, Protect Sheet, Lock Cell, Format Cells) |
| Editing | AutoSum (Sum, Average, Count, Max, Min; Alt+=), Fill (Down, Right, Up, Left, Across Worksheets, Series, Justify, Flash Fill), Clear (All, Formats, Contents, Comments and Notes, Hyperlinks), Sort & Filter (A to Z, Z to A, Custom Sort, Filter, Clear, Reapply), Find & Select (Find, Replace, Go To, Go To Special, Formulas, Notes, Conditional Formatting, Constants, Data Validation, Select Objects, Selection Pane) |
| Share | [YC] Screenshot (Ctrl+Shift+S): the selection, the table or data region, the whole sheet, the workbook, the visible area, the YCGrid window or any open chart window, with or without headings, at 2x; Save Last Screenshot |

Format Cells (Ctrl+1) has Number (with custom codes, [YC] `[epoch]` and nanosecond timestamps), Alignment (with the
orientation dial), Font, Border, Fill (colors, patterns, gradients) and Protection.

## Insert

| Group | Commands |
|---|---|
| Tables | PivotTable, Recommended PivotTables, Table (Ctrl+T / Ctrl+L) |
| Illustrations | Pictures, Shapes (Excel's Shapes gallery, drawn by dragging, with adjustment handles), Icons |
| Charts | Recommended Charts; galleries with a picture of each chart, grouped as Excel's: Column or Bar, Line or Area, Pie or Doughnut, X Y (Scatter) or Bubble, Hierarchy (treemap, sunburst), Statistic (histogram, Pareto, box and whisker), Waterfall, Funnel, Stock, Surface or Radar, Combo; Maps (filled maps, offline); PivotChart. The corner button opens Insert Chart. F11 puts a chart on its own chart sheet, Alt+F1 on the sheet |
| Sparklines | Line, Column, Win/Loss |
| Filters | Slicer, Timeline |
| Links | Link (Ctrl+K) |
| Comments | Comment (threaded, Ctrl+Alt+M) |
| Text | Text Box, Header & Footer, WordArt |
| Symbols | Symbol |
| Controls | [YC] Button, Combo Box (ActiveX-style controls on the sheet) |

## Page Layout

| Group | Commands |
|---|---|
| Themes | Themes, Colors, Fonts, Effects (the built-in Office themes, color sets, font pairs and effects, with previews) |
| Page Setup | Margins (Normal, Wide, Narrow, Custom), Orientation, Size, Print Area (set, clear), Breaks (insert, remove, reset), Background, Print Titles; the Page Setup dialog (Page, Margins, Header/Footer with first and odd / even pages, Sheet) |
| Scale to Fit | Width, Height (Automatic, 1 to 9 pages, More Pages), Scale |
| Sheet Options | Gridlines and Headings: View and Print |
| Arrange | Bring Forward, Send Backward, Selection Pane, Align, Group, Rotate |
| Print | [YC] Print, Export PDF |

## Formulas

| Group | Commands |
|---|---|
| Function Library | Insert Function (Shift+F3, with Function Arguments), AutoSum, Recently Used, Financial, Logical, Text, Date & Time, Lookup & Reference, Math & Trig, More Functions (Statistical, Engineering, Information, Compatibility, Database, Web); one command per function (`InsertFn_<NAME>`) |
| Defined Names | Name Manager (Ctrl+F3), Define Name, Apply Names, Use in Formula, Create from Selection (Ctrl+Shift+F3); Paste Name (F3) |
| Formula Auditing | Trace Precedents, Trace Dependents, Remove Arrows, Show Formulas (Ctrl+\`), Error Checking (the Error Checking window, Trace Error, Circular References), Evaluate Formula (Step In / Step Out), Watch Window (Add Watch); Ctrl+[ / Ctrl+] select precedents / dependents |
| Calculation | Calculation Options (Automatic, Automatic Except for Data Tables, Manual), Calculate Now (F9), Calculate Sheet (Shift+F9); Ctrl+Alt+F9 calculates everything |

While a formula is typed: Formula AutoComplete (functions, names, table columns), the argument hint, F4 to cycle the
`$` signs, F3 to paste a name, pointing with the mouse or the arrow keys (Ctrl+click adds a reference),
Ctrl+Shift+Enter for an array formula, and Ctrl+A after a function name for Function Arguments. Background error
checking marks cells with a green triangle; its button offers Excel's fixes and the Error Checking Options.

## Data

| Group | Commands |
|---|---|
| Get & Transform Data | Get Data, From Text/CSV (with a preview) |
| Queries & Connections | Refresh All (Ctrl+Alt+F5), Edit Links (links to other workbooks) |
| Sort & Filter | Sort A to Z / Z to A, Sort (levels; by values, cell color, font color or conditional formatting icon; custom lists; options: case sensitive, left to right), Filter (Ctrl+Shift+L), Clear, Reapply (Ctrl+Alt+L), Advanced |
| Data Tools | Text to Columns, Flash Fill (Ctrl+E), Remove Duplicates, Data Validation (lists with a drop-down, numbers, dates, text length, custom formulas, input and error messages; Circle Invalid Data), Consolidate |
| Forecast | What-If Analysis (Scenario Manager, Goal Seek, Data Table), Forecast Sheet |
| Analysis | Data Analysis (the Analysis ToolPak: descriptive statistics, histograms, regression, correlation, covariance, t-tests, z-test, F-test, ANOVA, moving average, exponential smoothing, sampling, random numbers, rank and percentile, Fourier analysis), Solver (options, Load / Save Model, the Answer, Sensitivity and Limits reports) |
| Outline | Group (Shift+Alt+Right), Auto Outline, Ungroup (Shift+Alt+Left), Clear Outline, Subtotal, Show Detail, Hide Detail; the outline settings |

The AutoFilter drop-down sorts; filters by values (dates grouped by year, month and day), colors and conditions
(text, number, date, top 10, above average); and searches.

## Review

| Group | Commands |
|---|---|
| Proofing | Spelling (F7, with Options and AutoCorrect) |
| Comments | New Comment, Delete, Previous / Next Comment, Show Comments (the Comments pane) |
| Notes | Notes (New / Edit Note, Previous / Next Note, Show / Hide Note, Show All Notes, Convert to Comments) |
| Protect | Protect Sheet (with the 15 allowed actions), Protect Workbook (structure, windows), Allow Edit Ranges (with Windows user permissions) |

## View

| Group | Commands |
|---|---|
| Workbook Views | Normal, Page Break Preview (drag the page breaks), Page Layout (rulers, click to add headers and footers), Custom Views |
| Show | Ruler, Gridlines, Formula Bar, Headings, Focus Cell (tints the active cell's row and column) |
| Zoom | Zoom, 100%, Zoom to Selection |
| Window | New Window, Arrange All, Freeze Panes (panes, top row, first column, [YC] columns in the middle), Split, Hide, Unhide, View Side by Side, Synchronous Scrolling, Reset Window Position, Switch Windows (also Ctrl+F6 / Ctrl+Tab); [YC] New Container, Save Layout, Load Layout, Reset Layout |
| Macros | Macros (Alt+F8) |
| Python | [YC] Immediate, Error Log |

## Developer

| Group | Commands |
|---|---|
| Code | VS Code (Alt+F11: saves the workbook and sets up the project with `AGENTS.md` / `CLAUDE.md`, the skill and the MCP server), Debug in VS Code, Macros (Alt+F8), Reload (the Python code), Immediate, Error Log, Macro Security (the Trust Center) |
| Controls | Insert: form controls (button, combo box, check box, spin button, list box, option button, group box, label, scroll bar) and ActiveX-style controls (button, combo box, check box, option button, toggle button, text box, label, list box, spin button, scroll bar, image); Design Mode; Properties; View Code |

## Trading [YC]

| Group | Commands |
|---|---|
| Safety | Operator Mode (on: parameter cells and guarded buttons can be changed; off: the screen is read-only, optionally behind a PIN and an idle timeout), Kill Switch (Ctrl+Alt+K), Heartbeats (the data feeds, scripts and Python host that report they are alive) |
| Parameters | Parameter Cell (a cell with limits, choices, a maximum step and confirmation), Change Log |
| Recording | Record (the data reaching the bound tables), Record Everything (the whole screen: a time machine), Replay (at any speed), Back to Live |
| Monitor | Latency Monitor (tick-to-screen latency, frame time, throughput), Alert Rules (sound, popup, flash, Windows notification) |
| Examples | complete research and trading screens, built in the open workbook from simulated data |

See [safety.md](../skill/ycgrid-python/references/safety.md) and
[data-binding.md](../skill/ycgrid-python/references/data-binding.md).

## Contextual tabs

| Tab | Shown while | Groups |
|---|---|---|
| Table Design | a cell of a table is selected | Properties (Table Name, Resize Table), Tools (PivotTable, Remove Duplicates, Convert to Range, Insert Slicer), Table Style Options (header row, total row, banded rows / columns, first / last column, filter button), Table Styles |
| PivotTable Analyze | a cell of a PivotTable is selected | PivotTable (name, Options), Active Field (Field Settings, expand, collapse), Group (selection, field, ungroup), Filter (Insert Slicer, Insert Timeline, Filter Connections), Data (Refresh (Alt+F5), Refresh All, Change Data Source), Actions (Clear, Select, Move PivotTable, Delete), Calculations (Calculated Field, Calculated Item), Tools (PivotChart, Recommended PivotTables), Show (Field List, +/- Buttons, Field Headers) |
| Design (PivotTable) | a cell of a PivotTable is selected | Layout (Subtotals, Grand Totals, Report Layout: compact, outline, tabular, repeat labels; Blank Rows), PivotTable Style Options, PivotTable Styles (a gallery of every style) |
| Chart Design | a chart is selected | Chart Layouts (Add Chart Element, Quick Layout), Chart Styles (Change Colors, styles), Data (Switch Row/Column, Select Data), Type (Change Chart Type), Location (Move Chart) |
| Format (chart) | a chart is selected | the Chart Elements list, Format Selection (the element's Format pane), Reset to Match Style |
| Sparkline | a sparkline cell is selected | Sparkline (Edit Data), Type, Show (high, low, negative, first, last points, markers), Style, Group (axis, Group, Ungroup, Clear) |
| Shape Format | a shape or text box is selected | Insert Shapes, Shape Styles (fill, outline, weight, dashes, arrows, quick styles, effects), [YC] Text (alignment), Arrange (forward / backward, Selection Pane, align and distribute, group, rotate and flip), Size |
| Picture Format | a picture is selected | Adjust (Reset Picture, Change Picture), Picture Styles (Picture Border), Crop |
| Header & Footer | the header or footer of a page is being edited in Page Layout view | Header & Footer, Header & Footer Elements (page number, number of pages, date, time, file path, file name, sheet name, picture), Navigation, Options |
| Slicer, Timeline | a slicer or a timeline is selected | Slicer / Timeline (caption, Settings, Report Connections), Buttons or Show, Size |

Charts are edited in place, as in Excel: click an element (title, legend, axis, series, point) to select it,
double-click it for its Format pane, drag the chart or its handles, and use the Chart Elements (+), Chart Styles
(brush) and Chart Filters buttons beside it.

## Right-click menus

- **Cells**: the mini toolbar above the menu; Cut, Copy, Paste Options (the paste pictures) and Paste Special;
  Screenshot of Selection; Insert / Delete (with copied or cut cells); Clear Contents; Filter (by the selected cell's
  value, color, font color or icon); Sort (including Put Selected Cell Color / Font Color / Icon On Top, Custom Sort);
  comments and notes; Format Cells; Pick From Drop-down List; Define Name; Link / Edit / Open / Remove Hyperlink.
- **Row and column headings**: Insert, Delete, Clear Contents, Format Cells, Row Height / Column Width, Hide, Unhide.
- **Sheet tabs, status bar, docked windows, charts, shapes, pictures and controls**: their own menus. Python adds
  items with `Application.CommandBars("Cell").Controls.Add(...)`.

## Keyboard

Excel's shortcuts work, with KeyTips on Alt. The main ones:

| Keys | Action |
|---|---|
| Arrows, Ctrl+arrows, Home, Ctrl+Home, Ctrl+End, Page Up / Down, Alt+Page Up / Down | move; with Shift: extend the selection |
| Ctrl+Space, Shift+Space, Ctrl+A (Ctrl+Shift+Space) | select columns, rows, the data region then the sheet |
| F8, Shift+F8, End | Extend Selection, Add to Selection, End mode |
| Enter, Tab (Shift: back), Ctrl+Enter, Alt+Enter | commit and move, fill the selection, line break in a cell |
| F2, Delete, Backspace, Esc | edit the cell, clear contents, clear and edit, cancel |
| Ctrl+; / Ctrl+Shift+; | today's date / the current time |
| Ctrl+' / Ctrl+Shift+' | copy the formula / value from the cell above |
| Ctrl+D, Ctrl+R, Ctrl+E | fill down, fill right, Flash Fill |
| Ctrl+Z, Ctrl+Y, F4 | undo, redo or repeat, repeat (F4 in a formula cycles the `$` signs) |
| Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Shift+V, Ctrl+Alt+V | copy, cut, paste, paste values, Paste Special |
| Ctrl+B, Ctrl+I, Ctrl+U, Ctrl+5, Ctrl+1, Ctrl+Shift+F / P | bold, italic, underline, strikethrough, Format Cells, its Font tab |
| Ctrl+Shift+~ ! @ # $ % ^ | General, number, time, date, currency, percent, scientific formats |
| Ctrl+Shift+&, Ctrl+Shift+_ | outline border, no border |
| Ctrl+Shift+> / < | increase / decrease the font size |
| Ctrl+9, Ctrl+0, Ctrl+Shift+9 / 0 | hide rows / columns, unhide them |
| Ctrl++ (Ctrl+Shift+=), Ctrl+- | insert cells, delete cells |
| Ctrl+F, Ctrl+H, Shift+F5, Shift+F4, Ctrl+G / F5 | Find, Replace, Find again, Find Next, Go To |
| Alt+;, Ctrl+Shift+O, Ctrl+/ | select visible cells, cells with notes, the current array |
| Alt+Down | open the cell's list (data validation, a table's total cell) or the filter drop-down on a header |
| Ctrl+Q, Ctrl+T, Ctrl+Shift+T, Alt+' | Quick Analysis, create a table, a table's total row, Style |
| Shift+F10, the Menu key | the context menu of the active cell |
| Ctrl+Page Up / Down, Ctrl+Shift+Page Up / Down | previous / next sheet; group sheets |
| Shift+F11 or Alt+Shift+F1, F11, Alt+F1 | new sheet, chart sheet, chart on the sheet |
| Ctrl+S or Shift+F12, F12 or Alt+F2, Ctrl+O or Ctrl+F12, Ctrl+N, Ctrl+W, Ctrl+P or Ctrl+Shift+F12 | save, Save As, open, new, close, print |
| Ctrl+F6, Ctrl+Tab (Shift: back) | next workbook window |
| Alt+Q, F7, Alt+F8, Alt+F11, Ctrl+Shift+U, Ctrl+F1 | Tell Me, spelling, macros, VS Code, expand the formula bar, collapse the ribbon |
| Ctrl+Shift+S, Ctrl+Alt+K | [YC] screenshot, kill switch |

`Application.OnKey` overrides any of them from Python.

## Python, MCP and agents

Anything the UI does, Python does with Excel's names; the skill is the full reference:

- [SKILL.md](../skill/ycgrid-python/SKILL.md): writing Python for YCGrid (projects, events, macros, `@udf`, `@stream`).
- [object-model.md](../skill/ycgrid-python/references/object-model.md): every object and member.
- [ui-coverage.md](../skill/ycgrid-python/references/ui-coverage.md): every command and its Python entry points.
- [mcp-tools.md](../skill/ycgrid-python/references/mcp-tools.md): the `yc_*` MCP tools for AI agents.
- [data-binding.md](../skill/ycgrid-python/references/data-binding.md), [plot.md](../skill/ycgrid-python/references/plot.md),
  [screens.md](../skill/ycgrid-python/references/screens.md), [safety.md](../skill/ycgrid-python/references/safety.md):
  [YC] live tables, `yc.plot` figures, the Data View (`yc.data.view`, tens of millions of rows from `.npy` files),
  trading screens and safety.
- [testing.md](../skill/ycgrid-python/references/testing.md): driving a hidden YCGrid the way a user does.

The `yc` command line (`python -m yc`, or `ycgrid` after `pip install`): `open`, `run <script>`, `examples [name]`,
`init <folder>`, `devenv`, `pack` (`--with-wheels`) and `python <version>`.

## ycgrid.exe command line

| Option | Meaning |
|---|---|
| `<file>` | open a workbook (`.ycg`, `.xlsx`, `.xlsm`, `.csv`); a running YCGrid in the same folder opens it instead, and a project folder with its own `ycgrid.exe` hands the file to that one |
| `--new-window` | a process and window of its own, even when YCGrid is running |
| `--hidden` | no window (tests, CI and agents drive it over YCP; screenshots still work) |
| `--parent-pid <pid>` | end when that process ends (`yc.launch` uses it) |
| `--lang en` / `zh-TW` | the UI language for this run |
| `--install`, `--uninstall` (`--silent`, `--no-desktop`) | set or remove the `.ycg` file association and the desktop and Start menu shortcuts for the current user (`install.cmd`, `install.cmd /nodesktop`, `uninstall.cmd`) |
| `--no-handoff` | don't hand the file to the project folder's own `ycgrid.exe` |

For tests and benchmarks: `--frames N` (draw N frames and exit), `--report r.json` (frame times and counters),
`--screenshot s.png` (the last frame), `--width` / `--height`, `--warp` (software rendering), `--keys "<script>"`
(simulated input), `--type "<text>"` (typed into A1), `--save-as <path>` (save before exiting), `--demo` and
`--big-data` (fill test data), `--expand-rows`, `--top-row N`, `--ribbon-tab N`, `--backstage`, `--collapsed`,
`--float-sheet N`, `--new-container`, `--dock-into-container`, `--save-layout`, `--load-layout`.

Settings, `ycgrid.toml` and environment variables: [settings.md](settings.md). The file format:
[ycg-format.md](ycg-format.md).
