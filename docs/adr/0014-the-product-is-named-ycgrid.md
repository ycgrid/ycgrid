# ADR 0014: The product is named YCGrid, and every name follows from it

- Status: accepted
- Related: [docs/YCGRID_PLAN.md](../YCGRID_PLAN.md) ("Main changes in 3.0", hard rule 21), [AGENTS.md](../../AGENTS.md)
  ("Naming"), [docs/ycg-format.md](../ycg-format.md)

## Context

A spreadsheet that works like Excel invites names that sound like Excel. A name close to a trademark is a risk once the
project is open source, and a product whose names are invented case by case ends up with files, packages and
environment variables that don't look related.

## Decision

1. **The product is YCGrid.** "YC" are the author's initials; "Grid" says what it is. The name is always written as one
   word: never "YC Grid", and "YC" alone is never the brand (it is easily read as Y Combinator).
2. **Every name follows from it**, in five forms (AGENTS.md, "Naming"): `YCGrid` in text people read, `ycgrid` in file,
   folder, package and pipe names, `YCGRID_` in environment variables, the two letters `yc` / `Yc` / `YC` as the prefix
   of code identifiers, and `.ycg` / `.yctape` / YCP for the workbook file, recordings and the control protocol.
3. **No name contains `cel`, `xcel` or `Excel`.** Describing compatibility ("works like Excel", Excel's object model
   names) is fine; naming the product, a file type, a package or a logo after Excel is not.
4. Internal binary tags use the same prefix: the workbook header `YCG1`, the encrypted envelope `YCGE`, the tape header
   `YCTAPE`, the column frame `YCCF`, the live-table channel `YCXCHAN1`, the map data `YCGEO1` and the open-file message
   `YCOP`.

## Consequences

- Hard rule 21 (every newer version opens every older file) counts from `.ycg` 1.0.
- A new file type, package, environment variable or protocol picks its name from the five forms instead of inventing
  one.
