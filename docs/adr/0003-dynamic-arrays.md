# ADR 0003: dynamic arrays: spill ranges per sheet, array lifting by argument masks

- Status: accepted (M14)
- Related: PLAN §6.4 (legacy semantics), §6.7 (arrays and dynamic arrays), §7.3 (recalculation)

## Context

PLAN §6.7 reserved per-cell spill flags in `MetaTile.flags` for dynamic arrays. A formula result with several cells
was reduced to its upper-left value (or implicit intersection), and functions that expect one value used implicit
intersection when given a range (`=LEN(A1:A3)` gave one length).

## Decision

**Spill ranges are a per-sheet map, not cell flags.** `Sheet::spills` maps each anchor (`row << 16 | col`) to its
range and whether it is a legacy Ctrl+Shift+Enter array formula. Member cells hold plain values, so readers, saving
and copying need no change. The engine keeps an index (anchor → range, tile → anchors, pending anchors) rebuilt from
the map whenever the sheet list or the version changes (undo).

- `EvaluateCell` returns the whole result when it has several cells (an array, or a multi-cell range).
  `Document::StoreResult` writes the members and updates the range. A non-empty cell, another formula or a merged area
  in the way gives `#SPILL!`; the anchor remembers where it wanted to go, and a change there evaluates it again.
- Reading a member of a pending anchor waits for the anchor, like reading a pending cell (`NeedCell` in the serial
  pass, *blocked* in a parallel wave). When a range grows, shrinks or changes, the dependents of its cells are marked
  pending and `Recalc` runs another pass (bounded at 64).
- A write into members (UI or Python) drops the range and clears the other members; the anchor is evaluated again and
  reports `#SPILL!` over the new content. Removing an anchor's formula clears its members. Rows and columns moving
  drop the dynamic ranges of that sheet; RebuildAll spills them again.
- Parallel waves never commit a spill: anchors and new array results go to the serial pass.
- `A1#` is a flag on the reference (`RefSpec::spill`) and depends on the anchor cell only; `@` is a unary operator.
- Saved as SHET key 22 (`[anchor row, anchor col, rows, cols, cse]`); older versions skip it and see plain values.

**Array lifting by argument masks.** `FuncInfo::lift` marks the arguments that expect one value. `CallFunction`
evaluates those first; when one has several cells, the function runs once per element (with broadcasting like the
binary operators) and returns an array. Otherwise it runs once, reusing the evaluated arguments through an argument
frame, so nothing is evaluated twice. IF-like functions mark only their condition and set `lift_rest`, so their
unused branches stay unevaluated unless lifting happens (no false circular references).

## Consequences

- Every formula is a dynamic-array formula, as in current Excel: `=A1:A10` spills instead of intersecting; `@`
  keeps the old behavior.
- The map is scanned linearly for UI lookups (`Sheet::SpillAt`); thousands of spill ranges per sheet would need a
  spatial index there too.
- New functions that take single values must be added to the lifting table in `functions.cpp`.
