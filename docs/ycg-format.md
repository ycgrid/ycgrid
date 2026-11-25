# .ycg file format (version 1.0)

This document is the normative spec of the `.ycg` format. Implementation: `src/io/ycg.cpp` (`SaveYcgToBytes` /
`LoadYcgFromBytes`). Compatibility rules: PLAN §15, hard rule 21. Every key the current writer emits is listed below;
when you add one, add it here in the same change.

## Compatibility rules

1. **Every newer YCGrid must open every file saved by an older version.** `tests/compat/ycg/<version>/` is the corpus; the unit tests open, compare, save, reopen and compare again on every build.
2. Metadata chunks are integer-keyed msgpack maps: **fields are only added, never changed; numbers are never reused and never change meaning**; deleted fields keep their numbers. Readers skip unknown fields and use defaults for missing ones.
3. Positional records (msgpack arrays) only grow at the end: an older reader reads the elements it knows and skips the rest. When a record has no room left, its last element is an integer-keyed map (shapes, PivotTable fields) and new fields go there.
4. Binary chunks (`CELL`) have no field structure: any format change must bump the chunk version and add an "old -> new" upgrade function to `ycg.cpp` plus a corpus file.
5. When a chunk is unknown, or newer than the reader:
   - `critical`: the workbook opens read-only and says a newer YCGrid is needed.
   - `preserve_if_unknown`: kept as is and written back on save.
   - neither: skipped with a warning.

## Header (48 bytes, little-endian)

| Offset | Type | Contents |
|---|---|---|
| 0 | char[4] | `YCG1` |
| 4 | u16 | format_major (1) |
| 6 | u16 | format_minor (0) |
| 8 | u16 | min_reader_major: lowest format major version needed for a complete read (1) |
| 10 | u16 | min_reader_minor (0) |
| 12 | u64 | required_features (feature bits the reader must know; currently 0) |
| 20 | u64 | optional_features (0) |
| 28 | u32 | number of chunks |
| 32 | u64 | directory offset |
| 40 | u32 | reserved |
| 44 | u32 | crc32c of the first 44 bytes |

## Directory

40 bytes per chunk: `u32 fourcc`, `u16 schema version`, `u16 flags` (1 = critical, 2 = preserve_if_unknown), `u64 offset`, `u64 stored size`, `u64 raw size`, `u8 compression` (0 none, 1 zstd), `u8[3]` reserved, `u32 crc32c` (of the stored contents). The directory is followed by its own crc32c. Saving writes a temporary file first, then replaces the original.

## Encrypted workbooks

File > Info > Encrypt with Password (Python: `Workbook.Password`, `SaveAs(..., Password=)`) wraps the whole file above in an
envelope, so nothing of the workbook (not even its sheet names) is readable without the password:

| Offset | Type | Contents |
|---|---|---|
| 0 | 4 bytes | magic `YCGE` |
| 4 | u16 | envelope version (1) |
| 6 | u16 | key derivation: 1 = PBKDF2-HMAC-SHA256 |
| 8 | u32 | iterations (200,000 when written) |
| 12 | 16 bytes | salt (random per save) |
| 28 | 12 bytes | nonce (random per save) |
| 40 | u64 | length of the plain `.ycg` bytes |
| 48 | n bytes | AES-256-GCM ciphertext of the plain `.ycg` |
| 48 + n | 16 bytes | GCM tag |

The UTF-8 password and the salt give the 256-bit key; the 48-byte header is the additional authenticated data, so a wrong
password and a damaged file both fail the tag check. The password is never stored in the file; YCGrid keeps it in memory
for the next save. Versions without this envelope report the file as not a YCGrid workbook.

## Chunks (version 1)

All chunks are at schema version 1. They are written in this order: `META`, `WKBK`, `STYL`, `SHET` (one per sheet),
`EXTL`, `FMLA`, `CELL` (one per sheet), `NAME`, `KVST`, `PROP`, `PANL`, `CONT`, `THEM`, `CSTY`, `BIND`, `SAFE`, `NBOK`,
`LAYT`, then the unknown chunks kept from the file that was opened. `EXTL`, `THEM`, `CSTY`, `BIND`, `SAFE` and `NBOK`
are written only when they have content.

Conventions: rows and columns are 0-based unless stated otherwise; a rectangle is `[r0, c0, r1, c1]` (inclusive);
colors are ARGB `u32` (`0xAARRGGBB`); a color with `0xFE` in the top byte refers to the document theme (theme color
index in bits 16-23, tint x 10000 as an int16 in bits 0-15); sizes and positions of objects are in points; a "hash" is
Excel's 16-bit password verifier (the password itself is never stored).

| FourCC | Flags | Contents |
|---|---|---|
| `META` | preserve | {1: program name (`YCGrid`), 2: program version, 3: save time (epoch ms)} |
| `WKBK` | critical | {1: [sheet id order], 2: active sheet index, 3: next_sheet_id, 4: next_object_id, 5: workbook protection [structure, windows, password hash], 6: formula dialect (1: `DECIMAL` with two arguments is Excel's base conversion; absent: older files, where it reads as `DECIMAL.SCALE`), 7: calculation options [iterative (bool), maximum iterations, maximum change, mode (0 automatic, 1 manual, 2 automatic except for data tables; only when set)] (only when the workbook has calculation settings of its own)} |
| `STYL` | critical | {1: default font, 2: default size, 3: [styles]}; a style is a [style map](#style-map). Only the styles in use are written, renumbered; index 0 is the default style |
| `SHET` | critical | one per sheet: an integer-keyed map, see [SHET keys](#shet-keys) |
| `EXTL` | preserve | links to other workbooks, written before `FMLA`: [[path, [sheet names], updated (epoch seconds), last error, [per sheet: bin(varint count, then per cell varint row, varint column, value)]]]; a formula refers to link n's sheet k with the pseudo sheet id 0xC000 + n x 64 + k, so `FMLA` text such as `='[Book2.ycg]Sheet1'!A1` finds the cached values |
| `FMLA` | critical | {1: [[sheet id, row, column, A1 text]]}: one record per distinct formula, its A1 text written relative to the first cell that uses it (parse it at that origin; relative references follow). Indexes into this list are 1-based |
| `CELL` | critical | one per sheet. varint sheet id, varint cell count, then per cell in (row, column) order: varint row delta (from the previous cell's row, starting at 0), varint column, u8 flags (1 value, 2 formula, 4 style), value (flag 1), varint formula index (flag 2), varint style index (flag 4) |
| `NAME` | critical | [{1: name, 2: refers-to text, 3: scope (-1 workbook or sheet id), 4: comment, 5: hidden, 6: relative references are relative to A1 (always true; older files without it read them as absolute)}] |
| `KVST` | critical | {key: msgpack value} (`ThisWorkbook.Store`); YCGrid's own keys: `view.custom_views` (View > Custom Views: [{name, sheet, areas, active, scroll, zoom, page_view, gridlines, headings, print, hidden, sheets: [{id, rows / cols hidden spans, ps}]}]), `view.docprops` (File > Info's built-in properties: {Title, Subject, Author, Manager, Company, Category, Keywords, Comments, Status, Created, LastSaved, LastModifiedBy}), `view.theme_effects` (Page Layout > Themes > Effects: the scheme's name), `format.table_styles` (Home > Format as Table > New Table Style: a string with one line per style, `name|row stripe|column stripe` then 9 elements `|fill;font color;bold;italic;top;bottom;left;right;inside h;inside v`, colors in hex ARGB, each line `style.color`), `format.default_table_style` (New Table Style's "Set as default table style for this document": the style's name, a msgpack str; none = TableStyleMedium2) |
| `PROP` | critical | {name: {1: value (bin, value encoding), 2: LinkToContent, 3: link source}} (CustomDocumentProperties) |
| `PANL` | preserve | custom panels (UserForms are shown by Python at run time and are not saved): [{1 name, 2 caption, 3 dock, 4 width, 5 height, 6 visible, 7 controls, 8 back color, 9 fore color}]; a control is {1 type, 2 name, 3 caption, 4 linked cell, 5 OnAction, 6 list fill range, 7 chart, 8 [items], 9 value, 10 min, 11 max, 12 enabled, 13 back color, 14 fore color, 15 font size, 16 bold, 17 number format, 18 height, 19 small change (the spin's step)} |
| `CONT` | preserve | containers [{1 name, 2 caption, 3 left, 4 top, 5 width, 6 height, 7 floating, 8 created by Python}] |
| `QRYS` | preserve | Get & Transform queries (removed, ADR 0012): kept and written back as an unknown chunk |
| `THEM` | preserve | document theme {1 name, 2 [12 colors: dk1, lt1, dk2, lt2, accent1-6, hlink, folHlink], 3 color set name, 4 heading font, 5 body font, 6 font set name}; absent = Office |
| `CSTY` | preserve | custom and changed built-in cell styles [[name, style map, built-in, includes (1 number, 2 alignment, 4 font, 8 border, 16 fill, 32 protection)]] |
| `COAU` | preserve | co-authoring (removed, ADR 0012): kept and written back as an unknown chunk |
| `DMOD` | preserve | the Data Model (removed, ADR 0012): kept and written back as an unknown chunk; PivotTables on it open but don't refresh |
| `BIND` | preserve | [YC] bound tables (ADR 0007) [{1 name, 2 sheet id, 3 key column, 4 columns, 5 stale after (s), 6 show age, 7 recorded (written only when false)}]; a column is [name, number format, flash, up color, down color, flash ms, max hz] + [render, render color, render negative color, render max, trend length] when the column draws a bar or trend line |
| `SAFE` | preserve | [YC] safety {1: parameters [[sheet, row, column, name, type, has min, min, has max, max, [choices], confirm, max step]], 2: change log [[time, user, kind, target, before, after, source]], 3: settings {1 start read-only, 2 has PIN, 3 PIN hash, 4 operator idle (s), 5 kill macro, 6 host timeout (s)}, 4: alert rules [[name, sheet, r0, c0, r1, c1, op, a, b, flags (1 sound, 2 popup, 4 flash window, 8 enabled, 16 Windows notification), color, cooldown (s)]]} |
| `NBOK` | preserve | [YC] the former Notebook window's cells {1: [code of each cell]} ([ADR 0010](adr/0010-notebook-cells-in-the-workbook.md)); kept and written back, exported to `.ipynb` since [ADR 0011](adr/0011-the-notebook-is-removed.md); outputs are not saved |
| `LAYT` | preserve | layout JSON (schema `ycgrid.layout`: `version`, `app`, `main` window, `panes`, `dock` tree, `containers`, `next_container_id`, `locked`, and per sheet its scroll and selection in `sheets`) |

### Value encoding (shared by `CELL` and msgpack bin; type codes never change)

| Code | Type | Contents |
|---|---|---|
| 0 | blank | — |
| 1 | number | f64 |
| 2 / 3 | FALSE / TRUE | — |
| 4 | error | u8 (1 #NULL!, 2 #DIV/0!, 3 #VALUE!, 4 #REF!, 5 #NAME?, 6 #NUM!, 7 #N/A, 8 #GETTING_DATA, 9 #SPILL!, 10 #CALC!, 11 #PYTHON!, 12 #CONNECT!, 13 #BLOCKED!) |
| 5 | text | varint length + UTF-8 |
| 6 | Decimal | u8 scale + i128 mantissa (little-endian, 16 bytes) |

### Style map

Only non-default fields are written: 1 font name, 2 size, 3 font color, 4 bold, 5 italic, 6 strikethrough, 7 underline,
8 fill color, 9 borders [[line style, color] x 6 edges], 10 horizontal alignment, 11 vertical alignment, 12 indent,
13 wrap text, 14 shrink to fit, 15 orientation (degrees), 16 number format code, 17 locked (written when false),
18 hidden, 19 fill pattern, 20 pattern color, 21 gradient (0 none, 1 linear, 2 from the center, 3 from a corner), 22 gradient
angle (from a corner: 0 upper left, 1 upper right, 2 lower left, 3 lower right), 23 second gradient color, 24 superscript (1)
/ subscript (2), 25 named cell style, 26 justify distributed (true), 27 text direction (1 left-to-right, 2 right-to-left).

### SHET keys

| Key | Contents |
|---|---|
| 1 | sheet id |
| 2 | name |
| 3 | CodeName |
| 4 | row limit |
| 5 | column limit |
| 6 | visible |
| 7 | tab color |
| 8 | row heights [default, [[index, size, size before hiding]], [[index, outline level]]] (older files have no third element) |
| 9 | column widths (as 8) |
| 10 | merged cells [rect] |
| 11 | whole-row styles {row: style index} |
| 12 | whole-column styles {column: style index} |
| 13 | notes and threaded comments [[row, column, author, text]]; a threaded comment adds [true, date, resolved, [[author, text, date]] replies] (older versions read its first post as a note); a note with its own box adds [false, 0, false, [], look] where look is {1 width, 2 height, 3 x offset, 4 y offset (points; width 0 = the default box beside its cell), 5 font, 6 size, 7 bold, 8 italic, 9 font color, 10 fill color, 11 line color (ARGB, 0 = Excel's), 12 horizontal, 13 vertical alignment} (handles and Format Comment; older versions skip element 8) |
| 14 | sheet key/value store {key: msgpack value} |
| 15 | controls (ActiveX-style and form controls) [[control map](#controls)] |
| 16 | charts [[chart map](#charts)] |
| 17 | view [top row, left column, active row, active column, freeze rows, freeze columns] |
| 18 | conditional formats [[rule map](#conditional-formatting-rules)] |
| 19 | data validation [[rule map](#data-validation-rules)] |
| 20 | the sheet's AutoFilter ([AutoFilter map](#autofilter)) |
| 21 | hyperlinks [[row, column, address, sub-address, screen tip]] |
| 22 | spill ranges of dynamic arrays and array formulas [[anchor row, anchor column, rows, columns, array (Ctrl+Shift+Enter) formula]] |
| 23 | protection [on, password hash, [15 allow flags: select locked, select unlocked, format cells, format columns, format rows, insert columns, insert rows, insert hyperlinks, delete columns, delete rows, sort, AutoFilter, PivotTables, objects, scenarios (older files have 14; older versions read 14)], [[title, r0, c0, r1, c1, password hash, [[account, allow]] (Permissions; older files have 6 elements)]] (Allow Edit Ranges)] |
| 24 | tables [[id, name, rect, style, flag bits (1 header row, 2 total row, 4 banded rows, 8 banded columns, 16 first column, 32 last column, 64 filter button), [[column name, totals function, totals label]], the table's own AutoFilter map (element 7; older versions skip it)]] |
| 25 | shapes, pictures and text boxes [[shape record](#shapes)] |
| 26 | sparkline groups [[id, type, [[row, column, data sheet id, r0, c0, r1, c1]], [7 colors: series, negative, markers, high, low, first, last], flag bits (1 high, 2 low, 4 first, 8 last, 16 negative, 32 markers, 64 axis), line weight, {1 empty cells as (1 gaps, 2 zero, 3 connected), 2 show hidden data, 3 minimum scale, 4 maximum scale (1 same for all, 2 each its own, 3 custom), 5 custom minimum, 6 custom maximum, 7 right to left, 8 date range, 9 axis color}]] (element 6 is new; older files have 6 elements) |
| 27 | page setup [landscape, paper, [left, right, top, bottom, header, footer margins (pt)], zoom (0 = fit), fit wide, fit tall, [has print area, r0, c0, r1, c1], [has title rows, r0, r1], [has title columns, c0, c1], flag bits (1 gridlines, 2 headings, 4 center horizontally, 8 center vertically, 16 black and white, 32 over then down, 64 different first page, 128 different odd and even pages, 256 don't scale with document, 512 don't align with page margins), first page number, [left / center / right header, left / center / right footer] followed, when any is set, by the first page's six and the even pages' six (18 texts), [row breaks], [column breaks]], then (element 14, only when one is set) a map {1 Comments: 0 none / 1 at end of sheet / 2 as displayed, 2 Cell errors as: 0 displayed / 1 blank / 2 "--" / 3 #N/A, 3 Draft quality (bool), 4 [[r0, c0, r1, c1]] the second, third... print area, 5 [[section 0-5, picture bytes (bin), width, height (pt; 0 = its own size)]] header / footer pictures (&G)} |
| 28 | PivotTables [[PivotTable record](#pivottables)] |
| 29 | slicers and timelines [[id, name, caption, PivotTable name, field, [left, top, width, height], columns, z, timeline, timeline level, Slicer Settings bits (1 no header, 2 descending, 4 hide items with no data, 8 don't indicate items with no data, 16 items with no data not last; 0 = Excel's defaults; older files have no element 11), [more PivotTables it filters (Report Connections)], table name (a table's slicer: field is its column; "" = a PivotTable's), timeline Show options hidden (bits: 1 selection label, 2 scroll bar, 4 time level) (elements 12-14; older files have up to 11)]] |
| 30 | What-If data tables [[rect, has row input, row input row, row input column, has column input, column input row, column input column]] |
| 31 | scenarios [[name, comment, [[row, column]] changing cells, [values], locked, hidden]] |
| 32 | Solver model [defined, objective, goal, target value, changing cells, [[cell reference, relation, right-hand side]], non-negative, method, max iterations, precision, max time] |
| 33 | fonts inside cell texts [[row, column, text, [[start character, font, size, color, bold, italic, strikethrough, underline, script]]]] (-1 / 0 / "" = the cell's own; they apply only while the cell holds that text) |
| 34 | chart sheet (the sheet is one chart: its first chart fills the window) |
| 35 | outline settings: bit 0 summary rows above detail, bit 1 summary columns left of detail, bit 2 automatic styles |
| 80 | Solver Options {1 use automatic scaling, 2 show iteration results, 3 ignore integer constraints, 4 integer optimality (%), 5 max subproblems (0 = no limit), 6 max feasible solutions (0 = no limit), 7 convergence, 8 derivatives (1 forward, 2 central), 9 use multistart, 10 require bounds on variables, 11 population size, 12 random seed, 13 mutation rate, 14 maximum time without improvement (s)}; older versions skip it and keep the defaults |
| 81 | Consolidate: [function (0 Sum ... 10 Varp, the dialog's order), top row, left column, create links to source data, [references]] |
| 47 | Ignore Error (error checking): [rect] of cells whose error-checking rule is ignored |
| 68 | notes shown all the time (Show/Hide Note): [rect] of one cell each; written after 13 so the notes exist when it is read |
| 100 | Page Layout > Background: the picture's bytes (bin), tiled behind the cells on screen (only when set) |
| 70 | linked pictures (Paste Options > Linked Picture): [[picture shape id, source sheet id, [rect]]]; the picture is drawn again when those cells change |
| 71 | the sheet's tab is selected (grouped sheets, Excel's [Group]); the sheets saved selected are grouped again when the file opens |
| 72 | very hidden (`xlSheetVeryHidden`) |

Keys not listed are not written by this version; readers skip them.

### Controls

{1 id, 2 kind (0 button, 1 combo box, 2 check box, 3 option button, 4 toggle button, 5 text box, 6 label, 7 list box,
8 spin button, 9 scroll bar, 10 image, 11 group box), 3 name, 4 caption, 5 left, 6 top, 7 width, 8 height,
9 placement (1 move and size, 2 move, 3 free floating, 100 floating window), 10 enabled, 11 visible, 12 back color,
13 fore color, 14 font name, 15 font size, 16 bold, 17 [items], 18 ListFillRange, 19 LinkedCell, 20 style, 21 value,
22 list index, 23 z, 24 created by Python, 25 group name, 26 min, 27 max, 28 small change, 29 large change,
30 orientation, 31 multi-select, 32 [selected indexes], 33 multi-line, 34 password character, 35 picture (bin),
36 picture size mode, 37 italic, 38 locked, 39 word wrap, 40 list rows, 41 form control (true) or ActiveX-style (false),
42 OnAction macro, 43 print object, 44 alt text, 45 a form button's alignment [horizontal, vertical (0 left / top, 1 center,
2 right / bottom), orientation (0 horizontal, 1 stacked, 2 upward, 3 downward), automatic size], 46 its margins [automatic,
left, top, right, bottom (points)], 47 3-D shading}

### Charts

A chart is an integer-keyed map:

| Key | Contents |
|---|---|
| 1 | id |
| 2 | name |
| 3 | chart type (`model::ChartType`) |
| 4-7 | left, top, width, height |
| 8 | placement (as for controls) |
| 9 | title text |
| 10 | has title |
| 11 | has legend |
| 12 | interactive (pan and zoom) |
| 13-15 | the value and category axes' bounds as versions before Format Axis kept them: 13 value axis automatic, 14 category axis automatic, 15 [value min, value max, category min, category max]; still written, read only when the axes carry no bounds |
| 16 | series [[series map](#chart-series)] |
| 17 | z order |
| 18 | created by Python |
| 19 | floating window |
| 20 | reserved (1) |
| 21 | the PivotTable of a PivotChart |
| 22 | histogram bin width |
| 23 | histogram bin count |
| 24 | source range (Select Data) |
| 25 | series in rows |
| 26 | element formatting {1 title font, 2 legend font, 3 axis font, 4 legend position, 5 chart area fill, 6 chart area border, 7 no border, 8 plot area fill, 9 category axis, 10 value axis, 11 secondary value axis, 12 plot area border, 13 legend fill, 14 legend border, 15 title fill, 16 title border, 17 title manual layout, 18 legend manual layout, 19 plot area manual layout, 20 legend overlaps the chart} |
| 27 | 3-D view [set, rotation, elevation, perspective, depth percent] |
| 28 | Chart Filters' hidden categories [0-based points] |
| 29 | gap width |
| 30 | overlap |
| 31 | angle of the first slice |
| 32 | doughnut hole size |
| 33 | plot visible cells only |
| 34 | show empty cells as (1 gaps, 2 zero, 3 connected) |
| 35 | show #N/A as an empty cell |
| 36 | title overlays the chart |
| 37 | chart style |
| 38 | color scheme (Change Colors: 0 the theme's colors, 1-3 colorful palettes, 4-9 monochromatic) |
| 39 | histogram bins [overflow bin, overflow at, underflow bin, underflow at, by category] |
| 40 | waterfall connector lines |
| 41 | lock aspect ratio |
| 50 | pie of pie / bar of pie {1 split type (1 position, 2 value, 3 percent value, 4 custom), 2 split value, 3 second plot size (%), 4 gap width (%), 5 custom points in the second plot [0-based]} |
| 51 | stock chart elements {1 up / down bars, 2 high-low lines, 3 up bars fill, 4 down bars fill, 5 high-low line color (ARGB, 0 automatic), 6 up / down bars gap width} |
| 52 | 3-D floor and walls {1 floor fill, 2 walls fill} (ARGB, 0 automatic) |
| 53 | date axis {1 category type (0 automatic, 1 text, 2 date), 2 base unit (-1 automatic, 0 days, 1 months, 2 years), 3 major unit scale (-1 the base unit)} |
| 54 | the axis titles' manual layout {1 category axis title, 2 value axis title, 3 secondary value axis title} (a manual layout as below) |
| 55 | Format Chart Area > Properties {1 print object, 2 locked} |

A font is [size, color, bold, name, italic, underline] (older files: the first three); a manual layout is [on, x, y,
width, height] (fractions of the chart); an axis is [visible, has title, title, number format, major gridlines, min
automatic, max automatic, min, max, major unit, minor unit, logarithmic, log base, values in reverse order, crosses
(automatic, at value, at maximum), crosses at, display unit, major tick mark, minor tick mark, label position, minor
gridlines, axis position between categories, label interval, has its own font, its font, its title's font, line
color (ARGB, 0 automatic), no line, line weight, label distance from axis (100 by default), Multi-level Category Labels
(element 30; absent = on)] (files before Format Axis have only the first five elements; files from before 0.2.0 have the first 23:
the axis then uses the chart's axis font).

### Chart series

| Key | Contents |
|---|---|
| 1 | name (text or reference) |
| 2 | values (reference or array) |
| 3 | X values / categories |
| 4 | axis group (1 primary, 2 secondary) |
| 5 | fill / line color |
| 6 | live topic (streaming series) |
| 7 | capacity of a streaming series |
| 8 | data supplied by Python (`Series.SetData` / `AppendData`) |
| 9, 10 | Python data X and Y (bin, little-endian f64), written when 8 is set |
| 11 | chart type of this series (combo charts) |
| 12 | data labels on |
| 13-19 | trendline: kind, polynomial order, moving average period, forecast forward, forecast backward, show equation, show R² |
| 20, 21 | error bars: kind, amount |
| 22 | waterfall totals [point indexes] |
| 23 | line weight |
| 24 | marker |
| 25 | marker size |
| 26, 27 | custom error amounts, plus and minus (a range, an array constant or a number) |
| 28 | error bar directions (1 both, 2 plus, 3 minus) |
| 29 | left out by Chart Filters |
| 30 | smoothed line |
| 31 | box and whisker options (bits) |
| 32 | points with their own format [[index, fill, border, explosion, data label, label dx, label dy, label text, label contents, label position, label font set, label font, legend entry font set, legend entry font, label fill, label border, legend entry deleted]] (dx / dy: the point's data label dragged away, fractions of the chart's size, added to the series' own offset; the label's text: "" its contents, "=Sheet1!$C$2" a cell's; contents / position -1: the series'; the legend entry font: a pie's category entry; older files: the first five or seven) |
| 33 | pie explosion |
| 34 | vary colors by point |
| 35-39 | data labels: position, contents (bits), "value from cells" range, number format, separator |
| 40 | bubble sizes |
| 41 | line color |
| 42 | no fill |
| 43 | no line |
| 44 | dash type |
| 45 | transparency |
| 46 | invert if negative |
| 47 | invert color |
| 48 | marker [fill, border] colors |
| 49 | fill [type (0 solid, 1 gradient, 2 picture or texture, 3 pattern), gradient second color, gradient angle, pattern, pattern background, tile the picture, tile offset x, offset y (points), scale x, scale y (%), alignment (0 top left .. 8 bottom right), mirror type (0 none, 1 horizontal, 2 vertical, 3 both)] |
| 50 | picture fill: the image file's bytes (bin) |
| 51 | line [compound type, cap, join, begin arrow, end arrow] |
| 52 | data labels' font (a font as above) |
| 53 | [show leader lines, vary colors by point (columns and bars), legend entry deleted] |
| 54 | its legend entry's own font [set, font] |
| 55 | its data labels' box [fill, border] (ARGB, 0 = none) |
| 56 | chart type options {1 treemap label options (0 none, 1 banner, 2 overlapping), 2 box and whisker inclusive median, 3 map projection, 4 map area (Series.GeoMappingLevel), 5 map labels, 6 map colors (0 sequential, 1 diverging), 7-9 map minimum / midpoint / maximum colors (Excel BGR, 0 automatic), 10 error bars end cap, 11-12 data labels' offset (fractions of the chart's size)} |
| 57 | trendlines {1 the first trendline's name ("" automatic), 2 set intercept, 3 intercept, 4 more trendlines [[kind, order, period, forward, backward, equation, R², name, set intercept, intercept]]} |
| 58 | X error bars [kind, amount, plus, minus, directions] (scatter charts) |

### Conditional formatting rules

{1 id, 2 [ranges], 3 type, 4 operator, 5 formula 1, 6 formula 2, 7 text, 8 has fill, 9 fill color, 10 has font color,
11 font color, 12 bold, 13 italic, 14 strikethrough (each -1 unchanged / 0 / 1), 15 rank, 16 bottom, 17 percent, 18 below
average, 19 duplicate (false = unique), 20 color scale points (2 / 3), 21 [3 colors], 22 data bar color, 23 priority,
24 stop if true, 25 icon style, 26 icon thresholds [[type, value, formula, greater or equal]], 27 reverse icon order,
28 show icon only, 29 text operator, 30 date period, 31 equal to average, 32 standard deviations, 33 color scale points
[[type, value, formula] x 3], 34 outline color of the format (ARGB, alpha 0 = none: Red Border), 35 data bar look [gradient
fill, show the value, border color (0 none), direction (0 context, 1 left-to-right, 2 right-to-left)], 36 data bar shortest and
longest bar [[type, value, formula] x 2] (type -1 automatic), 37 the format's [number format code ("" none), underline (-1
none), pattern (-1 none), pattern color], 38 the format's lines [[line style, color] x left, top, right, bottom]}. Keys 35-38
are written only when set. The formulas are relative to the first cell of the first range.

### Data validation rules

{1 [ranges], 2 type, 3 operator, 4 formula 1, 5 formula 2, 6 ignore blank, 7 in-cell drop-down, 8 show input message,
9 show error alert, 10 alert style, 11 input title, 12 input message, 13 error title, 14 error message}

### AutoFilter

{1 on, 2 rect, 3 filter columns, 4 [hidden rows], 5 first sort level [column (-1 none), descending] (older versions),
6 every sort level [[column, descending]]}; a filter column is {1 field, 2 kind, 3 [checked values], 4 blanks,
5 criterion 1, 6 criterion 2, 7 OR, 8 top, 9 percent, 10 count, 11 reserved (0), 12 color, 13 dynamic filter, 14 icon set
(kind 7, Filter by Cell Icon: an XlIconSet, 0 = No Cell Icon; older files have 13 keys), 15 icon (0 = the lowest values' icon)}.

### Shapes

A shape is a 13-element array: [id, kind (0 picture, 1 rectangle, 2 rounded rectangle, 3 oval, 4 triangle, 5 line,
6 arrow, 7 right arrow, 8 text box), name, [left, top, width, height], placement, [fill on, line on, fill color, line
color, line weight, flip horizontal, flip vertical], [text, font, size, color, bold, italic, horizontal alignment,
vertical alignment], image bytes (bin), OnAction macro, visible, z, extra, reserved (nil)]. Files from before the
extra map have 12 elements. The extra map is {1 dash, 2 begin arrow, 3 end arrow, 4 fill transparency, 5-8 text
margins left / top / right / bottom, 9 wrap text, 10 resize shape to fit text, 11 alt text, 12-15 crop left / top /
right / bottom, 16 locked, 17 lock aspect ratio, 18 rotation (degrees), 19 group id, 20 print object, 21-22 original
width / height (pictures), 23 AutoShapeType (a preset shape of the Shapes gallery, msoAutoShapeType; kind stays a
rectangle or a line for older versions), 24 connector (2 elbow, 3 curved), 25 freeform points [x0, y0, x1, y1...] as
fractions of the box, 26 freeform flags (1 closed, 2 smooth), 27 WordArt letters' outline color, 28 WordArt shadow,
29 adjustments [values] (Shape.Adjustments; missing ones take Excel's defaults), 30 Format Shape > Effects (only when
set) {1 Shape Effects preset, 2 theme effect style (0 subtle, 1 moderate, 2 intense, -1 the shape's own), 3 shadow (0 none,
1 outer, 2 inner, 3 perspective), 4 shadow color, 5 transparency, 6 size (%), 7 blur, 8 angle, 9 distance, 10-13
reflection size (%) / transparency / blur / distance, 14-16 glow size / color / transparency, 17 soft edges, 18-19 top /
bottom bevel type, 20-23 top bevel width / height, bottom bevel width / height, 24-27 3-D rotation x / y / z /
perspective, 28 3-D rotation preset}}.

### PivotTables

A PivotTable is a 22-element array (older files have fewer; missing elements take their defaults):

| Element | Contents |
|---|---|
| 0 | id |
| 1 | name |
| 2 | source sheet id |
| 3 | source rect |
| 4 | source table name |
| 5 | [row, column] of the top-left cell |
| 6, 7, 8 | row, column and filter (page) fields [[axis field](#pivottable-fields)] |
| 9 | data fields [[data field](#pivottable-fields)] |
| 10 | flag bits (1 values on rows, 2 grand total row, 4 grand total column, 8 built on the Data Model) |
| 11 | layout (0 compact, 1 outline, 2 tabular) |
| 12 | subtotals (0 none, 1 top, 2 bottom) |
| 13 | style |
| 14 | [has output, r0, c0, r1, c1] |
| 15 | [field names] (the source's headers when it was last refreshed) |
| 16 | groups [[field, by (1 years, 2 quarters, 3 months, 4 year-months, 5 days, 6 numbers; 0 for a group of items), start, end, interval, extra map]] |
| 17 | calculated fields [[name, formula]] |
| 18 | slicer filters [[field, [hidden items]]] |
| 19 | calculated items [[field, name, formula]] |
| 20 | timeline date filters [[field, from, to]] |
| 21 | options map |

#### PivotTable fields

An axis field is [field, [hidden items], subtotal, descending, page item, extra map]; its extra map is {1 [collapsed
items], 2 collapse all, 3 sort by (data field), 4 [sort column items], 5 select multiple filter items, 6 label filter,
7 value filter, 8 caption, 9 subtotal functions (bits), 10 repeat item labels, 11 blank line after each item, 12 show
items with no data, 13 [manual item order], 14 renamed items [item, name, item, name...], 15 include new items in
manual filter, 16 [the items when the manual filter was set], 17 layout form (0 compact, 1 outline, 2 tabular; absent =
the report's), 18 subtotals at the top (0 / 1; absent = the report's), 19 insert page break after each item}; a filter is
[type, data field, value 1, value 2].

A data field is [field, function, Show Values As (the first five kinds; 0 otherwise), caption, number format, extra
map]; its extra map is {1 Show Values As (all 15 kinds), 2 base field, 3 base item}.

A group's extra map is {1 periods (bits), 2 number of days, 3 automatic start, 4 automatic end, 5 name, 6 groups of
items [[name, [items]]], 7 placed in its own field, 8 the kind (`model::PivotGroupBy`, also 7 items)}.

The options map (element 21) has only non-default entries: 1 merge and center labels, 2 indent for compact form
(default 1), 3 show error values as, 4 error text, 5 show empty cells as, 6 empty cell text, 7 autofit column widths
(written when false), 8 preserve cell formatting (when false), 9 show expand / collapse buttons (when false),
10 display field captions and filter drop-downs (when false), 11 refresh on open, 12 enable Show Details (when false),
13 field list order, 14 use custom lists when sorting (when false), 15 PivotChart field buttons (when false), 16 row
headers styled (when false), 17 column headers styled (when false), 18 banded rows, 19 banded columns, 20 formats kept
across refreshes [[row item, column item, style map]], 22 values without a number format of their own are General (written
when true; older files, without it, show the source's format), 23 Enable Selection (when false), 24 [rows of the page
breaks the report put in its sheet's page setup].
