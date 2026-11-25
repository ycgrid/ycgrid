# ADR 0005: Get & Transform with a list of steps instead of the M language

- Status: superseded by [ADR 0012](0012-pieces-not-ready-made-screens.md) (Get & Transform was removed)
- Related: PLAN §1.2 (Power Query was P3), §15 (the .ycg format), ADR 0004 (.xlsx import)

## Context

Excel's Get & Transform (Power Query) imports data from files, web pages and the workbook itself, shapes it with
recorded steps and loads the result into a table that Refresh fills again. Underneath, every query is a program in
the M language, run by the Power Query engine. Reimplementing M (a lazy functional language with hundreds of library
functions and connectors) is far beyond the scope of YCGrid, while the part people use every day is small: pick a
source, remove / rename / retype columns, filter, sort, remove duplicates, load, refresh.

## Decision

**A query is a source plus a list of named steps, run by YCGrid's own code (`io/query.cpp`).**

- Sources: a CSV / text file, a sheet of an `.xlsx` / `.xlsm` workbook (through the ADR 0004 importer), a table of a
  web page (downloaded with the app's WinHTTP client; `<table>` elements are read without a full HTML parser), and a
  table, defined name or range of this workbook.
- Steps (`model::QueryStep`: an op and string arguments): promote / demote headers, remove / keep columns, rename,
  change type (text, number, date, true/false), filter rows (`=`, `<>`, `>`, `>=`, `<`, `<=`, contains, begins,
  ends, blank, not blank...), sort, remove duplicates, remove blank rows, replace values, keep / remove top rows,
  add an index column, fill down.
- The UI follows Excel's: Data -> Get Data (From Text/CSV, From Workbook, From Web, From Table/Range) opens the Power
  Query Editor (preview, toolbar, Applied Steps; selecting a step previews the data at that step), Close & Load or
  Connection Only, the Queries & Connections pane, and Refresh All (queries, then PivotTables).
- A loaded query owns a table (ListObject) on its own sheet or at a chosen cell; Refresh rewrites the cells and
  resizes the table. A failed refresh keeps the old data and records the error on the query.
- Saved in a new `QRYS` chunk (preserve if unknown), so older versions keep the queries untouched.
- Python: `Workbook.Queries` (`Add`, `Item`, iteration) and `WorkbookQuery` (`Steps`, `AddStep`, `Refresh`,
  `Preview`, `Delete`, `ListObject`...), marked [YC] because Excel's `Queries.Add` takes M code.

## Consequences

- No M: queries can't be copied to or from Excel, and the Advanced Editor doesn't exist. Merge / append queries,
  pivot / unpivot, group by, custom columns, databases, OData, folders and authenticated web sources are not
  available; they can be added as new step ops or source kinds without changing the file format.
- Web pages that build their tables with JavaScript return no tables (no browser engine).
- Refresh runs on the UI thread; large files or slow web pages pause the window until they finish.

## Addendum: the M language

Queries can now also be written in M (`model::Query::m`, QRYS key 15). `io/m_lang.cpp` is an interpreter for the
language and the common part of its library:

- Language: `let ... in` (steps in any order, evaluated when needed), `if`, `each` and `_`, `(x, y) => ...`,
  `try ... otherwise`, `error`, records (fields may refer to each other), lists and ranges (`{1..10}`), field access
  (`[Column]`, `r[Field]?`), item access (`list{0}`, `table{[Name="x"]}`), operators (arithmetic, `&`, comparisons,
  `and`, `or`, `not`, `??`, `is`), `type` literals, `#date` / `#datetime` / `#duration` / `#table`, string escapes
  (`#(tab)`, `#(cr,lf)`, `#(00A0)`), comments, and other queries of the workbook by name.
- Sources: `File.Contents`, `Csv.Document`, `Excel.Workbook`, `Excel.CurrentWorkbook`, `Web.Contents`, `Web.Page`,
  `Json.Document`, `Text.FromBinary`, `Lines.FromBinary`, `Folder.Files`, `Folder.Contents`, `OData.Feed` (JSON),
  databases through ODBC (`Odbc.Query`, `Odbc.DataSource`, `Sql.Database` with Windows sign-in, `Sql.Databases`; a
  navigation table's `Data` is read only when used), and [YC] `YCGrid.Range("Sheet1!A1:D20")` for a plain range.
  Like YCGrid's own sources, `Csv.Document` and `Web.Page` turn number-like texts into numbers.
- About 230 library functions: `Table.*` (select / add / remove / rename / reorder / transform columns, select rows,
  sort, distinct, group, nested joins and joins, expand, combine, pivot, unpivot, transpose, split and combine
  columns, fill, replace, index, first / last / skip / range...), `List.*`, `Text.*`, `Number.*`, `Date.*`,
  `DateTime.*`, `Duration.*`, `Record.*`, `Logical.*`, `Value.*`, `Splitter.*`, `Combiner.*`, `Replacer.*`,
  `Comparer.*`, and the enumerations (`Order`, `JoinKind`, `MissingField`, `QuoteStyle`...).
- The Power Query Editor's Advanced Editor shows the query in M (a query built with steps as generated M) and turns it
  into an M query when edited; the Applied Steps list then shows the `let` steps, and the toolbar's buttons append M
  steps. Python: `WorkbookQuery.Formula` and `Queries.Add(Name, Formula, Description)`, as in Excel.

Not covered: query folding (everything runs locally; database queries run as written), sources tied to Microsoft's
cloud services (SharePoint, Dataverse, Azure; left out by design), custom connectors, `Table.Profile`, and the rest of the library's long tail; an unknown function reports its name.
