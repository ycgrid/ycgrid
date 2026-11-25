// SPDX-License-Identifier: Apache-2.0
// bind.h - [YC] bound tables: a table (ListObject) fed by Python by key (ws.bind(df, key="strike"))
//
// The definition is saved with the workbook (BIND chunk); the data are ordinary cells of the table, so formulas,
// charts, PivotTables and filters use them like any table. Python sends column frames through shared memory
// (engine/bind.h); the engine finds each key's row and writes only the cells that changed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yc::model {

// how a column shows its values, declared once in Python and applied to every row (also rows added later)
struct BoundColumn {
  std::string name;
  std::string number_format;  // "" = keep the cells' format
  // change flash: 0 off, 1 up / down colors (rises in up_color, falls in down_color), 2 any change in up_color
  int flash = 0;
  uint32_t up_color = 0xFFE5383B, down_color = 0xFF16A34A;  // ARGB: red up, green down (Taiwanese convention)
  int flash_ms = 600;    // fade time
  int max_hz = 0;        // per-cell refresh cap on screen (0 = every publish)
  // [YC] drawn over the cells every frame: 0 none, 1 bar (value / scale), 2 two-sided bar (negative to the left, in
  // render_neg), 3 heat (color from the column's smallest to largest value), 4 progress (0..1), 5 trend (the row's last
  // trend_len values as a line)
  int render = 0;
  uint32_t render_color = 0, render_neg = 0;  // ARGB; 0 = defaults
  double render_max = 0;  // bar scale (0 = the column's largest absolute value)
  int trend_len = 60;
  bool operator==(const BoundColumn&) const = default;
};

struct BoundTable {
  std::string name;   // also the table's (ListObject's) name
  uint32_t sheet = 0;
  std::string key;    // the key column's name
  std::vector<BoundColumn> columns;  // declared columns (formats, flash); the table may have more
  double stale_after = 0;  // seconds: rows not updated for longer turn gray (0 = never)
  bool show_age = false;   // [YC] the age of each row is drawn at the right of the table
  bool record = true;      // market data: recorded and replayed (Trading > Record); false for the app's own tables
  bool operator==(const BoundTable&) const = default;
};

}  // namespace yc::model
