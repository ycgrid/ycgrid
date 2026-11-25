# ADR 0004: opening Excel workbooks and saving a copy as .xlsx

- Status: accepted
- Related: PLAN §1.2 and §15.3 (Excel file formats were out of scope), §15 (the .ycg format)

## Context

The PLAN kept Excel's file formats out of scope: data was to move between Excel and YCGrid through CSV and the
clipboard. In practice the first thing people do with a spreadsheet program is open the workbooks they already have,
and they need to hand results back to colleagues who use Excel. CSV loses formulas, formatting and every sheet but one.

## Decision

**YCGrid reads `.xlsx` / `.xlsm` and writes `.xlsx`, as an exchange format next to `.ycg`, not instead of it.**

- Opening an Excel workbook creates a new, untitled YCGrid workbook (like opening a CSV): Save asks for a `.ycg`, so
  the YCGrid-only parts (Python project, charts, controls, panels, layouts) never silently disappear. Save As with
  the "Excel Workbook" type (or a `.xlsx` path, or `SaveAs(FileFormat=xlOpenXMLWorkbook)` from Python) writes a copy
  and leaves the working file as it was.
- Carried over both ways: sheets (names, hidden), values, formulas (shared and array formulas when reading; Excel's
  `_xlfn.` / `_xlws.` prefixes added when writing and dropped when reading), number formats, fonts, fills (solid,
  patterns, two-stop gradients), borders, alignment, protection flags, merged cells, column widths, row heights,
  hidden rows / columns, defined names and the print area. Cached values are written so Excel shows them before
  recalculating (`fullCalcOnLoad` is set).
- Not carried over (documented in `io/xlsx.h`): charts, pictures, shapes, controls, PivotTables, slicers, conditional
  formatting, data validation, notes / comments, hyperlinks, sparklines, freeze panes and the Python project.
  Dynamic-array formulas are written as ordinary formulas (Excel may show `@` in front of them).
- Implementation: `io/xlsx.cpp` with zlib (a new vcpkg dependency at the pinned baseline) for the ZIP container and
  pugixml for the XML. The importer builds the workbook through `engine::Document` like the CSV importer, so formulas
  are parsed and registered the normal way.

## Consequences

- `.ycg` stays the only full-fidelity format and keeps its backward-compatibility rules; xlsx support can grow
  (conditional formatting and charts are the next candidates) without touching `.ycg`.
- Tests: `tests/unit/test_xlsx.cpp` (ZIP round trip, a styled workbook written and read back, shared formulas as
  Excel writes them). The output was also checked with openpyxl in both directions.

## Addendum: Excel 97-2003 workbooks (.xls) - removed (ADR 0012)

The `.xls` reader below was removed in phase 6: save an old `.xls` as `.xlsx` in Excel first.

`.xls` files (BIFF8 inside an OLE compound file) open the same way, read-only like `.xlsx`: `io/xls.cpp` reads the
compound file and the Workbook stream itself (no new dependency). Values, shared strings, formulas (decoded from
Excel's parsed tokens into formula text, shared and array formulas included; add-in functions by name), number
formats, fonts, fills, borders, alignment, merged cells, column widths, row heights, hidden sheets / rows / columns,
defined names and the print area are carried over; 1904-date workbooks are moved to the 1900 system. Workbooks are
saved as `.ycg` or `.xlsx` (Excel reads both), so YCGrid doesn't write `.xls`. Password-protected files open with their
password (XOR obfuscation, RC4 and RC4 CryptoAPI, MS-OFFCRYPTO; Excel's default password is tried first, so
write-protected workbooks open without asking); the BIFF5 / BIFF7 formats of Excel 95 and older are refused with a
message that says how to convert them in Excel.

## Addendum: what isn't carried over is said every time (2026-11-09)

The fidelity stays as decided (the parity backlog's "Not planned" lists what isn't read or written), but nothing is
dropped silently. `io::XlsxSkipped` counts, kind by kind, what an Excel workbook holds that YCGrid doesn't carry
over: charts, pictures, shapes, form controls, PivotTables, slicers and timelines, tables, conditional formatting
rules, data validation rules, comments and notes, hyperlinks, sparkline groups, frozen panes, protected sheets, links
to other workbooks, data connections and queries, and Excel macros.

- Opening an Excel workbook (File > Open, a recent file, a file dropped on the window) ends with a message box that
  lists them with their counts and says the values, formulas and formatting came along and that Save keeps a
  `.ycg`. A Python or MCP open stays quiet, as Excel's alerts do for scripts: the status bar and the Errors window
  carry the same list.
- Save As to an Excel workbook lists what this workbook holds that the copy won't, before anything is written, as
  Excel's Compatibility Checker does before a save to a lesser format: OK writes the copy, Cancel keeps everything as
  it was. The status bar then repeats what was left out. Python's `SaveAs` / `SaveCopyAs` write at once and the
  status bar says what was left out.
- Tests: `tests/unit/test_xlsx.cpp` counts a hand-made workbook with one of everything (and checks that a copy
  YCGrid wrote has nothing to report); `scripts/feature_sections.py` (`excel_exchange`) opens and saves through the
  dialogs in a hidden instance and reads the messages (`ui.state()["dialog_text"]`).
