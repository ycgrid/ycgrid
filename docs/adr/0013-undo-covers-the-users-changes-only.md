# ADR 0013: Undo covers the user's changes only; Python's writes don't clear it

- Status: accepted
- Related: `src/engine/document.cpp` (`Undo`, `Redo`, `RestoreSelective`, `ForeignWrite`), `src/engine/document.h`
  (`UndoEntry`), `apps/ycgrid/python_bridge*.cpp`, `tests/unit/test_undo.cpp`, `python/tests/test_live.py` (`LockTest`),
  `skill/ycgrid-python/references/object-model.md` (`Application.Undo`)

## Context

Excel clears the undo list after a macro changes the workbook. YCGrid did the same for every Python request, and its
undo put back a whole earlier version of the workbook. In a live app (quotes, a desk) Python writes several times a
second, so the user's undo list was always empty: a trader who dragged the strike column of an option chain by mistake
couldn't undo it. Keeping the list would not have helped either: undoing by putting back a whole version would also
have put back old quotes.

## Decision

1. **Undo covers the user's changes only.** Python's writes, live tables and topics are not recorded and don't clear
   the user's list. Python inserting or deleting rows or columns, sorting, adding, deleting, renaming or moving sheets,
   or creating or removing a bound table still clears it: the recorded positions no longer match.
2. **Undo puts back what the change touched.** An undo entry records the cells (contents or formats only) and the rows
   and columns (size, visibility, entire-row / column format) the user's change touched; undo copies those back from
   the earlier version and recalculates what depends on them. Everything else, live values included, stays.
3. **The old way stays where it is needed.** A change that touched something without cells to name (a theme, names,
   rules, objects, tables, pivots...), a spilled array, or more than 4,096 regions is still undone with the whole
   version. Such an entry is dropped, with every older one, when Python writes (it would undo Python's writes too).
4. A cell the user changed and Python later wrote gets the user's previous value back on undo: undo reverses the
   user's change.
5. `Application.UndoGroup`, planned to make Python's writes inside it one undo step, is not built (P2 in
   `skill/ycgrid-python/references/object-model.md`): Python's writes are never undoable.

## Consequences

- Ctrl+Z works in live apps; formats, column widths, moved cells and typed values are the common mistakes and are
  always undone without touching live data.
- The engine checks at each commit whether the change touched state outside its cells (pointers of the sheet and
  workbook parts compared); a part copied on write without a real change makes the entry undo the old way, which is
  always correct.
- Python code that relied on Excel's rule (an undo list emptied by a macro) sees the user's list survive its writes.
