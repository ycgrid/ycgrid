// SPDX-License-Identifier: Apache-2.0
// A text picture of what a workbook holds, for tests: Undo must give back exactly what was there, Redo exactly what
// the action made, and read-only mode or a protected sheet must leave it as it was. Two dumps are equal when the
// workbook holds the same things; where they differ, the lines name what changed.
#pragma once

#include <string>

#include "model/workbook.h"

namespace yc::io {

// One line per cell that holds something (a value, a formula or a format of its own): its address, its text as typed
// (InputTextOf), its value and its number format; then each sheet's rows and columns that differ from the default
// (size, hidden), its merged areas and whole-row / whole-column formats; then one checksum per .ycg chunk for
// everything else the file keeps (rules, validation, tables, names, notes, objects, PivotTables, protection).
// Left out: what belongs to the window rather than the workbook (selection, scroll, zoom, frozen panes, the active
// sheet, grouped tabs), the used range (it only grows, as Excel's), counters for the next ids, and the file's header.
std::string ContentDump(const model::Workbook& wb);

}  // namespace yc::io
