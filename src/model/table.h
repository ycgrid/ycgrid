// SPDX-License-Identifier: Apache-2.0
// table.h - tables (Excel ListObjects): a named range with a header row, an optional totals row and a style
//
// The table's look (header, banded rows / columns, first / last column) is drawn from its style at render time, so it
// follows the table when rows are inserted or it is resized; the cells' own formats override it.
#pragma once

#include "core/geometry.h"
#include "model/rules.h"
#include "model/table_style.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yc::model {

class Workbook;

struct TableColumn {
  std::string name;
  int totals_func = 0;  // totals row: 0 none / label, otherwise the SUBTOTAL function number (101 average ... 109 sum)
  std::string totals_label;
  bool operator==(const TableColumn&) const = default;
};

struct Table {
  uint32_t id = 0;
  std::string name;
  core::Rect range{};  // header row, data, totals row
  bool header = true, totals = false;
  bool banded_rows = true, banded_cols = false, first_col = false, last_col = false, filter_button = true;
  std::string style = "TableStyleMedium2";
  std::vector<TableColumn> columns;
  // the table's own AutoFilter (criteria, hidden rows, sort) when the sheet's AutoFilter is on another range: Excel
  // gives every table its buttons. Its range is the table's (header and data rows); .range and .on aren't used
  AutoFilter filter;

  // the data rows (empty when r0 > r1)
  core::Rect Data() const {
    core::Rect d = range;
    if (header) d.r0 += 1;
    if (totals && d.r1 > 0) d.r1 -= 1;
    return d;
  }
  bool HasData() const { return Data().r0 <= Data().r1; }
  int ColumnIndex(std::string_view name) const;  // case-insensitive; -1 when missing
  bool operator==(const Table&) const = default;
};

// structured reference items (bits of StructRef::op)
enum TableItem : uint8_t { kItemAll = 1, kItemData = 2, kItemHeaders = 4, kItemTotals = 8, kItemThisRow = 16 };

// a structured reference -> its range. table "" means the table that contains (origin_r, origin_c) on sheet.
// Returns 0 or an error code (core::Err as int: 4 #REF!, 3 #VALUE!)
int ResolveTableRef(const Workbook& wb, uint32_t sheet, std::string_view table, uint8_t items, std::string_view col1, std::string_view col2,
                    uint32_t origin_r, uint32_t origin_c, uint32_t* out_sheet, core::Rect* out);

// the table containing (r,c) on the sheet, or nullptr
const Table* TableAt(const Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c);
// table by name (case-insensitive) in any sheet
const Table* FindTable(const Workbook& wb, std::string_view name, uint32_t* sheet = nullptr);

// colors of a table style name (TableStyleMedium2 ... TableStyleLight / Dark); ARGB
struct TableStyleColors {
  uint32_t header = 0, header_text = 0, band = 0, border = 0;
  bool header_bold = true;
  uint32_t body = 0, body_text = 0;  // fill and font of the data rows (0 = the cells' own; dark styles set them)
};
TableStyleColors ColorsOfTableStyle(std::string_view style);
const std::vector<std::pair<std::string, std::string>>& TableStyleNames();  // id, English label

}  // namespace yc::model
