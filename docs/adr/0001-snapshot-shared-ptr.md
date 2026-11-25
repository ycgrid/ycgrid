# ADR 0001: copy-on-write with shared_ptr for snapshots and undo

- Status: accepted (M1)
- Related: PLAN §4.5 (snapshot publishing), §5.3 (TileDir), §7.4 (recalculation), §9.6 (undo)

## Context

The PLAN originally planned epoch-based reclamation for snapshots: the engine writes a new version, the UI and Python read old versions, and a version is reclaimed once every reader has left its epoch. That performs well but is expensive to implement and debug (custom allocators, reader registration, deferred reclamation queues).

## Decision

Every data structure that readers may share is wrapped in `std::shared_ptr` and written copy-on-write:

- `core::Cow<T>`: holds a `shared_ptr<T>`; `mut()` copies first when `use_count() > 1`, then writes.
- `model::TileDir<T>`: the three levels Chunk -> Page -> Tile are all `shared_ptr`; writing a cell only copies the three nodes on its path (about 82 KB), every other tile stays shared.
- `model::Workbook`: every sheet is a `Cow<Sheet>`; the formula table, style table, names and KV are all `Cow`.
- **Publishing a snapshot** = copying the `Workbook` object (pointers only) into `std::atomic<std::shared_ptr<const Snapshot>>`.
- **Undo** = saving a copy of the whole `Workbook` when a transaction starts. The cost is only the tiles that get rewritten afterwards; the memory of 100 undo steps grows with the amount of changed data, not with the workbook size.
- On undo the formula and style tables keep their current versions (both only grow and are supersets of the old ones); KV and document properties are not part of undo (as in PLAN §15.8).

## Correctness

- The engine is the only writer. Other threads only release snapshots, so the `use_count` the engine sees can only go down: modifying in place at 1 is safe; a larger value costs at most one extra copy.
- Readers hold a `shared_ptr<const Snapshot>`; the whole snapshot is never reclaimed or modified before the reader releases it.
- Members computed lazily (the prefix tables of `DimIndex`) are `Finalize()`d before publishing, so `mutable` members are never written while threads read the snapshot.
- Heap strings of `Value` are atomically reference counted and immutable, so they can be shared across snapshots.

## Costs and follow-ups

- The first write to a tile after each publish copies 64 KB (ValueTile). In the market data scenario (scenario A) only a limited number of tiles is written per publish cycle; measurements will decide whether ValueTile should be split further.
- The atomic increments/decrements of `shared_ptr` cost something when many tiles are copied; if profiling shows a bottleneck, switch to intrusive reference counting.
- This decision only affects `core/cow.h`, `model/tile_dir.h` and `engine/`; switching to epoch reclamation later doesn't change the interfaces (read-only access to `Snapshot` and `Workbook`).
