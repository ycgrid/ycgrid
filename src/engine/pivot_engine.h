// SPDX-License-Identifier: Apache-2.0
// pivot_engine.h - PivotTable reports: reading the source, grouping, aggregating and laying out the report
#pragma once

#include "core/geometry.h"
#include "core/value.h"
#include "model/pivot.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace yc::model {
class Workbook;
}

namespace yc::engine {

// the pivot cache: field names (the source's header row) and the records below it
struct PivotCache {
  // the source's fields, the fields groups made ("Years", "Region2"), then the calculated fields
  std::vector<std::string> names;
  size_t source_fields = 0;          // fields that can go on Rows / Columns / Filters (the source's and the groups')
  size_t columns = 0;                // the source's own columns (the first fields)
  size_t source_records = 0;         // records read from the source (copies for calculated items follow them)
  std::vector<std::string> numfmts;  // number format of each field's first value
  std::vector<std::vector<core::Value>> records;  // a record holds the source's columns, then the grouped fields' values
  std::vector<std::vector<int>> item_of;        // [field][record] -> item index
  std::vector<std::vector<std::string>> items;  // [field] -> sorted item texts (the item's identity: General format)
  std::vector<std::vector<std::string>> display;      // [field][item] the item as shown (the source's number format)
  std::vector<std::vector<core::Value>> item_values;  // [field][item] what a label cell holds (numbers and dates stay numbers)
  std::vector<int> made_by;  // [field] the group (index into PivotTable::groups) that made the field, -1 for a source column
  // [field] calculated items whose formula isn't a sum of items (=East*West, =MAX(East,West)): (item, formula); they have no
  // records and are worked out in each report cell from the values of the items they name
  std::vector<std::vector<std::pair<int, std::string>>> formula_items;
  // a PivotTable on the Data Model: each field's name in the model ("[Sales].[Region]", "[Measures].[Total]"; the
  // measures follow the columns, from measure_from on) and the value of a data field where the (field, item) pairs
  // hold, worked out by the model's measures
  std::vector<std::string> cube_names;
  size_t measure_from = static_cast<size_t>(-1);
  std::function<core::Value(const model::PivotTable&, const PivotCache&, size_t data, const std::vector<std::pair<int, int>>& pairs)> measure;
  bool IsMeasure(int f) const { return f >= 0 && static_cast<size_t>(f) >= measure_from && static_cast<size_t>(f) < names.size(); }
};

// fields follow their names: when the source's columns were inserted, deleted or reordered (or the groups made new
// fields), every reference to a field (axes, data, groups, calculated items, filters) moves to the field of the same
// name; fields that are gone are dropped. Returns old index -> new index (-1 = gone); empty when nothing changed
std::vector<int> RemapPivotFields(model::PivotTable* pt, const PivotCache& cache);

bool ReadPivotSource(const model::Workbook& wb, const model::PivotTable& pt, PivotCache* out, std::string* err);
std::string PivotItemText(const core::Value& v);  // an item's identity text; "(blank)" for an empty cell
// an item as shown (the source's number format) from its identity text
std::string PivotItemDisplay(const PivotCache& cache, int field, const std::string& identity);
// the caption of a field (Field Settings' Custom Name, else the source's heading)
std::string PivotFieldCaption(const model::PivotTable& pt, const PivotCache& cache, int field);
// where Excel puts a field checked in the field list: 0 Rows (text, dates), 1 Values as Sum (numbers), 2 Values as
// Count (numbers with blanks or text)
int PivotFieldDefaultArea(const PivotCache& cache, int field);
// a calculated field's formula ("=Sales/Qty", 'Unit Price' * 2): false with the reason when it can't be used
bool CheckPivotFormula(const std::vector<std::string>& source_names, const std::string& formula, std::string* err);
// a calculated item's formula: a sum of the field's items, each optionally multiplied or divided by a number
// (=East+West, ='North East'*1.1, =A-B/2); terms = (item, factor)
bool ParseCalcItem(const std::vector<std::string>& items, const std::string& formula, std::vector<std::pair<std::string, double>>* terms, std::string* err);
std::string PivotDataCaption(const model::PivotTable& pt, const PivotCache& cache, size_t data_index);  // "Sum of Sales"
// Show Values As kinds whose results are fractions (shown as percentages)
bool PivotShowAsPercent(model::PivotShowAs k);
bool PivotShowAsNeedsBase(model::PivotShowAs k);      // a base field
bool PivotShowAsNeedsBaseItem(model::PivotShowAs k);  // and a base item

// one cell of the report (relative to the report's top-left)
struct PivotCell {
  enum Kind : uint8_t { Empty, Data, Header, RowLabel, ColLabel, Subtotal, Grand, PageName, PageValue };
  Kind kind = Empty;
  core::Value v;
  std::string numfmt;
  bool bold = false;
  uint8_t indent = 0;
  bool header_row = false;  // in the column header rows (styled as the header band)
  bool grand_row = false;   // in the grand total row
};

struct PivotReport {
  uint32_t rows = 0, cols = 0;
  std::vector<PivotCell> cells;  // row-major
  uint32_t page_rows = 0;        // filter area at the top (fields + a blank row)
  uint32_t header_rows = 0;      // column header band
  uint32_t label_cols = 0;       // row label columns
  // the items each report row / column stands for: (field, item index) pairs (drill-down: Show Details)
  std::vector<std::vector<std::pair<int, int>>> row_filter, col_filter;
  // what each report row / column is: 0 an item, 1 a subtotal, 2 the grand total, 3 a group row, 4 a data caption row
  // (values on rows), 5 a blank line, 6 a collapsed item (its totals on one row / column); -1 for the filter / header
  // rows and label columns
  std::vector<int> row_kind, col_kind;
  std::vector<int> col_data;  // the data field of each value column (-1 = the row decides)
  std::vector<int> row_data;  // the data field of each row (values on rows; -1 = the column decides)
  // each row's and column's identity: kinds, field names and item texts (formats the user gave follow it)
  std::vector<std::string> row_ids, col_ids;
  // the +/- buttons: an outer item's label cell, its field and item, collapsed or not
  struct Toggle {
    uint32_t r = 0, c = 0;
    bool rows = true;
    int field = -1, item = -1;
    bool collapsed = false;
  };
  std::vector<Toggle> toggles;
  // the label column of each row field (by level): fields in compact form share their column with the next field
  std::vector<uint32_t> row_field_col;
  // report rows where a new page starts (Field Settings > Insert page break after each item)
  std::vector<uint32_t> breaks;
  // the records the report is made of when a value filter (Top 10, Value Filters) left some out: GETPIVOTDATA reads
  // the totals the report shows (Excel 16, measured: a Top 2's grand total is the two items')
  bool value_filtered = false;
  std::vector<size_t> kept_records;
  PivotCell& at(uint32_t r, uint32_t c) { return cells[static_cast<size_t>(r) * cols + c]; }
  const PivotCell& at(uint32_t r, uint32_t c) const { return cells[static_cast<size_t>(r) * cols + c]; }
};

PivotReport ComputePivot(const model::PivotTable& pt, const PivotCache& cache);

// the source records behind a report cell: the report's filters plus (field, item index) pairs (only records read from
// the source: never the copies made for calculated items)
std::vector<size_t> PivotDetailRecords(const model::PivotTable& pt, const PivotCache& cache, const std::vector<std::pair<int, int>>& filter);

// the records that pass the report's filters (slicers: which items still have data)
std::vector<size_t> PivotFilteredRecords(const model::PivotTable& pt, const PivotCache& cache);

// GETPIVOTDATA: the data field's value for the given field / item pairs (an empty list = the grand total)
bool PivotLookup(const model::PivotTable& pt, const PivotCache& cache, const std::string& data_field,
                 const std::vector<std::pair<std::string, std::string>>& pairs, core::Value* out);

// is an item left out by its field's manual filter (hidden, or new since the filter was set and not included)
bool PivotItemHidden(const model::PivotAxisField& af, const std::string& item);
// a row label cell: the row field level (index into row_filter[r]) whose label the column holds; -1 for none
int PivotLabelLevel(const PivotReport& rep, uint32_t r, uint32_t c);
// the first row field (level) whose labels go in label column c; -1 for none
int PivotColumnField(const PivotReport& rep, uint32_t c);

// is an item collapsed (expand / collapse) on its axis field
bool PivotItemCollapsed(const model::PivotAxisField& af, const std::string& item);
void SetPivotItemCollapsed(model::PivotAxisField& af, const std::string& item, bool collapsed);

// PivotTable styles (Design > PivotTable Styles): the look RefreshPivot paints
struct PivotStyleLook {
  bool none = false;
  uint32_t header_fill = 0, header_font = 0, header_line = 0;  // 0 = none / automatic
  uint32_t grand_fill = 0, grand_line = 0;
  uint32_t subtotal_fill = 0;
  uint32_t band_fill = 0;   // banded rows / columns
  uint32_t body_line = 0;   // lines between rows
  bool header_bold = true, grand_bold = true;
};
PivotStyleLook PivotStyleFor(const std::string& name);
std::vector<std::string> PivotStyleNames();  // the gallery: Light 1-28, Medium 1-28, Dark 1-28

}  // namespace yc::engine
