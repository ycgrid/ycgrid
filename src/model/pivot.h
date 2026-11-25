// SPDX-License-Identifier: Apache-2.0
// pivot.h - PivotTables (Insert -> PivotTable): the definition saved with the sheet
#pragma once

#include "core/geometry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yc::model {

enum class PivotFunc : uint8_t { Sum = 0, Count, Average, Max, Min, Product, CountNums, StdDev, StdDevP, Var, VarP, DistinctCount };
inline constexpr int kPivotFuncs = 12;

// "Show Values As" (Excel's 15 kinds; the first five are the ones older files know)
enum class PivotShowAs : uint8_t {
  Normal = 0, PercentOfGrandTotal, PercentOfColumnTotal, PercentOfRowTotal, RunningTotal,  // RunningTotal: "Running Total In" (base field)
  PercentOf, PercentOfParentRow, PercentOfParentColumn, PercentOfParent, DifferenceFrom, PercentDifferenceFrom,
  PercentRunningTotal, RankAscending, RankDescending, Index
};
inline constexpr int kPivotShowAs = 15;

// a label or value filter on a row / column field (Excel's PivotFilter; type = XlPivotFilterType, 0 = none)
struct PivotFilter {
  int type = 0;
  int data = 0;        // value filters: the data field (index into PivotTable::data)
  std::string v1, v2;  // the values (numbers as text); Top 10: v1 = the count / percent / sum
  bool operator==(const PivotFilter&) const = default;
};

// a field on the Rows / Columns / Filters area: its source column and its item filter
struct PivotAxisField {
  int field = 0;                       // source column (0-based within the source range)
  std::vector<std::string> hidden;     // items unchecked in the filter (shown text; "(blank)" for empty)
  bool subtotal = true;                // subtotals for this field (outer fields only)
  bool descending = false;             // item order
  std::string page_item;               // Filters area: the selected item ("" = (All))
  // expand / collapse (outer fields): the collapsed items; with collapse_all every item is collapsed and `collapsed`
  // lists the ones expanded again
  std::vector<std::string> collapsed;
  bool collapse_all = false;
  // sort by values: a data field (-1 = by labels) read in the column (row) of these items of the other axis (empty = the total)
  int sort_by = -1;
  std::vector<std::string> sort_col;
  bool multi_page = false;  // Filters area: Select Multiple Items (the check boxes write `hidden`)
  PivotFilter label_filter, value_filter;
  // Field Settings
  std::string caption;          // "" = the source field's name
  uint32_t subtotal_funcs = 0;  // custom subtotals: a bit per PivotFunc (0 = automatic, the data field's own function)
  bool repeat_labels = false;   // outline / tabular: the item on every row of its group
  bool blank_line = false;      // an empty row after each item
  bool show_all_items = false;  // items with no data too
  std::vector<std::string> manual_order;  // items moved by hand come first, in this order (the rest follow, sorted)
  std::vector<std::pair<std::string, std::string>> item_captions;  // items renamed in the report: (item, its new name)
  // Field Settings > Subtotals & Filters > Include new items in manual filter: with a manual filter (hidden items), items
  // that weren't there when it was set (`known`) are hidden too unless this is on (Excel's default: off)
  bool include_new = false;
  std::vector<std::string> known;  // the field's items when its manual filter was last set (filled by the refresh)
  // Field Settings > Layout & Print: -1 = the report's layout; 0 outline form with the next field's labels in the same
  // column (compact), 1 outline form, 2 tabular form
  int form = -1;
  int sub_top = -1;         // Display subtotals at the top of each group: -1 = the report's setting, 0 bottom, 1 top
  bool page_break = false;  // Insert page break after each item
  bool operator==(const PivotAxisField&) const = default;
};

struct PivotDataField {
  int field = 0;
  PivotFunc func = PivotFunc::Sum;
  PivotShowAs show_as = PivotShowAs::Normal;
  std::string caption;      // "" = "Sum of <field>"
  std::string number_format;  // "" = the source's / General
  int base_field = -1;      // Show Values As: the base field (-1 = the innermost row field)
  std::string base_item;    // ... and item: "(previous)", "(next)" or an item
  bool operator==(const PivotDataField&) const = default;
};

// Group: dates by period, numbers by ranges (the items of a source field become the groups), chosen items (Group Selection)
enum class PivotGroupBy : uint8_t { None = 0, Years, Quarters, Months, YearMonths, Days, Numbers, Items };
// Excel's Periods: seconds, minutes, hours, days, months, quarters, years
enum PivotPeriod : uint32_t { kPeriodSeconds = 1, kPeriodMinutes = 2, kPeriodHours = 4, kPeriodDays = 8, kPeriodMonths = 16, kPeriodQuarters = 32, kPeriodYears = 64 };

struct PivotGroup {
  int field = 0;
  PivotGroupBy by = PivotGroupBy::None;
  double start = 0, end = 0, interval = 10;  // Numbers: "start-(start+interval-1)" ranges; values outside go to "<start" / ">end"
  // dates: several periods (0 = `by` alone): the field shows the finest one, the others become fields of their own
  // ("Years", "Quarters"...) placed before it; Days: `days` per group
  uint32_t periods = 0;
  int days = 1;
  bool auto_start = false, auto_end = false;  // Starting at / Ending at: the smallest / largest value
  // Items (Group Selection): a new field (`name`, "Region2") whose items are these groups of the field's items
  std::string name;
  std::vector<std::pair<std::string, std::vector<std::string>>> named;
  bool placed = false;  // the fields it made were put on the report next to the field (once)
  bool operator==(const PivotGroup&) const = default;
};

// a format the user gave report cells, kept across refreshes (Preserve cell formatting on update): where (the row's
// and the column's items) and the cell's formats
struct PivotFormat {
  std::string row, col;
  uint32_t style = 0;  // StyleTable id (the file keeps the formats themselves)
  bool operator==(const PivotFormat&) const = default;
};

// a calculated field: a formula over other fields' names (=Sales/Qty), evaluated on their sums
struct PivotCalcField {
  std::string name, formula;
  bool operator==(const PivotCalcField&) const = default;
};

// a calculated item: a new item of a field made of its other items (=East+West, =North*1.1, =A-B)
struct PivotCalcItem {
  int field = 0;
  std::string name, formula;
  bool operator==(const PivotCalcItem&) const = default;
};

// a timeline's selection: the records whose date (a field's value) falls in [from, to] (whole days, inclusive)
struct PivotDateFilter {
  int field = 0;
  double from = 0, to = 0;
  bool operator==(const PivotDateFilter&) const = default;
};

struct PivotTable {
  uint32_t id = 0;
  std::string name;  // PivotTable1
  // source: a range (with headers in its first row) or a table name
  uint32_t src_sheet = 0;
  core::Rect src{};
  std::string src_table;
  // the source is the workbook's Data Model: the fields are the columns of its tables, then its measures; values are
  // measures evaluated for each cell (subtotals and totals too), so they need not add up
  bool from_model = false;
  // top-left cell of the report on its sheet
  uint32_t r0 = 0, c0 = 0;
  std::vector<PivotAxisField> rows, cols, pages;
  std::vector<PivotDataField> data;
  std::vector<PivotGroup> groups;
  std::vector<PivotCalcField> calc_fields;  // field indexes after the source's fields (fields.size() + i)
  std::vector<PivotAxisField> slicer_filters;  // fields filtered by slicers (their hidden items), on top of the layout's filters
  std::vector<PivotCalcItem> calc_items;
  std::vector<PivotDateFilter> date_filters;  // timelines
  bool values_on_rows = false;  // several data fields: "Σ Values" on rows instead of columns
  bool row_grand = true, col_grand = true;
  int layout = 0;  // 0 compact, 1 outline, 2 tabular (Report Layout)
  int subtotals = 1;  // 0 none, 1 at the top of the group (compact / outline), 2 at the bottom
  std::string style = "PivotStyleLight16";
  // PivotTable Options
  bool merge_labels = false;
  int indent = 1;  // compact form: characters per level
  bool show_error = false, show_null = false;
  std::string error_string, null_string;  // shown for error values / empty value cells when show_* is on
  bool autofit = true;              // Autofit column widths on update
  bool preserve_formatting = true;  // Preserve cell formatting on update
  bool show_buttons = true;         // +/- buttons
  bool field_captions = true;       // Display field captions and filter drop downs
  bool refresh_on_open = false;
  bool enable_drilldown = true;
  int field_list_order = 0;         // the field list: 0 data source order, 1 A to Z
  bool custom_lists = true;         // Use Custom Lists when sorting (month and day names in calendar order)
  bool chart_buttons = true;        // its PivotCharts show their field buttons
  std::string grand_name;           // PivotTable.GrandTotalName: the grand totals' label ("" = "Grand Total")
  // the last refresh: when (a date serial, 0 = never), and by whom (PivotTable.RefreshDate, RefreshName)
  double refreshed_at = 0;
  std::string refreshed_by;
  // Design > PivotTable Style Options
  bool style_row_headers = true, style_col_headers = true, style_banded_rows = false, style_banded_cols = false;
  std::vector<PivotFormat> formats;  // formats kept across refreshes
  // values without a number format of their own (Value Field Settings > Number Format) are General, as in Excel; files
  // saved before keep the source's format (false)
  bool general_values = true;
  bool enable_selection = true;  // PivotTable Analyze > Select > Enable Selection
  std::vector<uint32_t> breaks;  // the page breaks the report put in its sheet's page setup (Insert page break after each item)
  // not saved: each output cell's style id as the last refresh left it, and the identity of each output row / column
  // then (a cell whose style differs at the next refresh was formatted by the user)
  std::vector<uint32_t> written;
  std::vector<std::string> written_rows, written_cols;
  // not saved: how many of `fields` were the source's (and the groups') at the last refresh; the rest are calculated
  // fields, which references name by their position after these (-1 = unknown: fields minus calc_fields)
  int64_t fields_src = -1;
  // last refresh: the cells the report covers (cleared before the next refresh) and the field names then
  core::Rect output{};
  bool has_output = false;
  std::vector<std::string> fields;
  bool operator==(const PivotTable&) const = default;
};

const char* PivotFuncName(PivotFunc f);  // "Sum", "Count"...

}  // namespace yc::model
