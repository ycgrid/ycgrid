# ADR 0006: Co-authoring through a shared folder

- Status: superseded by [ADR 0012](0012-pieces-not-ready-made-screens.md) (co-authoring was removed)
- Related: PLAN §4 (engine thread), §15 (the .ycg format), `io/coauthor.h`, `engine/coauthor_parts.h`

## Context

In Excel, several people edit one workbook at the same time when it is stored in OneDrive or SharePoint: Microsoft's
service merges the changes and shows who is where. YCGrid has no server. It can still give people the same
experience wherever their copies of YCGrid see the same folder: one computer, a network share, or a folder that a
sync service (OneDrive, Dropbox, a NAS) keeps the same.

## Decision

**Each session appends its changes to its own log in a folder next to the workbook; the others read the logs.**

- The folder is `~$<file name>.coauthor` (hidden), next to the `.ycg`. Each session writes `<id>.log` (append-only,
  so two sessions never write the same file) and `<id>.who` (name, color, sheet, selection; rewritten every second).
  Review > Co-author shares a saved workbook; opening a workbook whose folder exists joins the others.
- A record is one of:
  - **cells**: the cells a session changed, read after the change (value or formula text, own style, array formula
    range). Cell changes come from the engine's change list, so every way of editing (typing, pasting, Python, fill,
    sort...) is covered without each command knowing about co-authoring.
  - **struct**: inserted / deleted rows, columns and cells; added, deleted, renamed, moved and copied sheets. The
    document records them as they happen (`Document::struct_log`); undo and redo record their inverses.
  - **parts**: everything else of a sheet (row heights, merges, charts, tables, conditional formats, validation,
    page setup, PivotTables...) and of the workbook (names, theme, queries, the Data Model...). A part is a
    copy-on-write member, so a change is seen as a new object, and the whole part is sent inside a `.ycg` image of
    the workbook without its cells.
- Conflicts: the last writer of a cell or a part wins, by Lamport clock and then session id, so every window ends
  with the same workbook. Struct operations made at the same time are transformed against each other (an insert
  above moves the other's rows down; deleting the same rows twice deletes them once), and cells written meanwhile
  move with them.
- Undo: a change from a co-author clears this session's undo history. YCGrid's undo restores whole snapshots, which
  would also undo the others' work.
- Saving writes into the file (the `COAU` chunk) how many records of each log it contains. Whoever opens the file
  while others edit, or after someone left changes unsaved, replays only the rest. The last session to leave a
  saved workbook deletes the folder.

## Consequences

- No server or account is needed, and the logs are plain files that sync services carry like any other.
- Latency is the folder's: a fraction of a second locally or on a LAN; a sync service adds its own delay.
- Parts are replaced as a whole: two people changing the same part at the same moment (for example both adding a
  chart to the same sheet) keep only the later change. Cells and structure merge at a finer level.
- With three or more sessions inserting and deleting rows at the very same time, the transformation is the simple
  pairwise kind and can, rarely, place a row differently in one window; the next edit of those cells makes the
  windows agree again.
