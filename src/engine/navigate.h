// SPDX-License-Identifier: Apache-2.0
// navigate.h - read-only queries on a snapshot: Ctrl+arrow, Ctrl+End, selection statistics (UI thread)
#pragma once

#include "core/geometry.h"
#include "model/sheet.h"
#include "model/style.h"

#include <cstdint>
#include <vector>

namespace yc::engine {

// Excel's Ctrl+arrow (End mode):
//   current and next cell both have values -> jump to the last cell of the block; otherwise -> the next cell with a value; none -> the sheet edge
// vertical: along rows; dir: +1 / -1
uint32_t CtrlJump(const model::Sheet& s, uint32_t r, uint32_t c, bool vertical, int dir);

// current region (Ctrl+Shift+*): grow from (r,c) until surrounded by blank rows and columns
core::Rect CurrentRegion(const model::Sheet& s, uint32_t r, uint32_t c);

// Ctrl+End: bottom-right corner of the used range
core::CellPos LastCell(const model::Sheet& s);

// selection statistics (average, count, sum in the status bar); only visits tiles with values
struct RangeStats {
  uint64_t count = 0;    // non-blank cells (count)
  uint64_t numbers = 0;  // numeric cells (numerical count)
  double sum = 0, min = 0, max = 0;
};
// visible_only: leave out hidden and filtered-out rows and hidden columns, as Excel's status bar does
RangeStats ComputeStats(const model::Sheet& s, const core::Rect& r, bool visible_only = false);

// a list of cells -> rectangles (contiguous cells in a row are merged, then identical runs on consecutive rows)
std::vector<core::Rect> CellsToRects(std::vector<core::CellPos> cells);

// Go To Special (Range.SpecialCells, the Go To Special dialog). values as XlSpecialCellsValue: 1 numbers, 2 text, 4 logicals, 16 errors
enum class Special { Comments, Constants, Formulas, Blanks, LastCell, Visible, AllFormatConditions, AllValidation };
std::vector<core::Rect> SpecialCells(const model::Sheet& s, core::Rect within, Special kind, int values = 1 | 2 | 4 | 16);
// Go To Special > Row differences (rows) / Column differences: in each row of `within`, the cells whose contents differ
// from that row's cell in column cmp_c (each column against its cell in row cmp_r); formulas compare as formulas
// (relative, so =A1 in B1 and =A2 in B2 are the same), other cells by their values (Range.RowDifferences)
std::vector<core::Rect> DifferenceCells(const model::Sheet& s, core::Rect within, uint32_t cmp_r, uint32_t cmp_c, bool rows);

// for AutoFit column width / row height: the UI provides the width of a string in a style; this only walks the cells

}  // namespace yc::engine
