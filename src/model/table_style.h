// SPDX-License-Identifier: Apache-2.0
// table_style.h - table styles (Home > Format as Table, Table Design > Table Styles; Excel's TableStyles): Excel's 60
// built-in styles (Light 1-21, Medium 1-28, Dark 1-11) in theme colors, YCGrid's Trading look, and the workbook's own
// styles (New Table Style...), kept in the workbook store under kTableStylesKey.
#pragma once

#include "model/style.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace yc::model {

struct Table;

// one element of a table style (Excel's TableStyleElement): its fill, font and lines. Unset parts (color 0, bold -1,
// line None) leave what the elements under it set.
struct TableStyleElement {
  uint32_t fill = 0, font_color = 0;
  int8_t bold = -1, italic = -1;
  BorderSide top, bottom, left, right, inner_h, inner_v;
  bool Empty() const;
  friend bool operator==(const TableStyleElement& a, const TableStyleElement& b);
};

// Excel's order of the elements (XlTableStyleElementType where one exists)
enum TableElement : int {
  kTeWholeTable = 0, kTeFirstColumnStripe, kTeSecondColumnStripe, kTeFirstRowStripe, kTeSecondRowStripe, kTeLastColumn, kTeFirstColumn,
  kTeHeaderRow, kTeTotalRow, kTeCount
};

struct TableStyleDef {
  std::string name;  // TableStyleMedium2..., or the name given in New Table Style
  bool builtin = false;
  int group = 0;     // the gallery's section: 0 Custom, 1 Light, 2 Medium, 3 Dark
  TableStyleElement el[kTeCount];
  int row_stripe = 1, col_stripe = 1;  // stripe sizes (rows / columns per stripe)
};

inline constexpr const char* kTableStylesKey = "format.table_styles";
// New Table Style's "Set as default table style for this document": the style Ctrl+T and Format as Table start with
inline constexpr const char* kDefaultTableStyleKey = "format.default_table_style";
std::string DefaultTableStyle(const std::map<std::string, std::string>& store);  // TableStyleMedium2 when none is set
std::string EncodeStoreString(const std::string& s);                            // a msgpack str (a store value)

// every built-in style (Light 1-21, Medium 1-28, Dark 1-11, then TableStyleTrading)
const std::vector<TableStyleDef>& BuiltinTableStyles();
const TableStyleDef* BuiltinTableStyle(std::string_view name);
// the workbook's own styles from the store value (msgpack bytes); "" = none
std::vector<TableStyleDef> DecodeTableStyles(std::string_view stored);
std::string EncodeTableStyles(const std::vector<TableStyleDef>& defs);
// the style named: the workbook's own (store), else a built-in one; nullptr = TableStyleNone or unknown
const TableStyleDef* FindTableStyle(const std::map<std::string, std::string>& store, std::string_view name);
// Excel's tooltip of a built-in style ("Blue, Table Style Medium 2"); a custom style's name
std::string TableStyleLabel(const TableStyleDef& d);

// the look of the table's style on cell (r, c): fill, font color, bold and lines, under the cell's own formats
void ApplyTableStyle(const Table& t, const TableStyleDef& d, uint32_t r, uint32_t c, Style* s);

}  // namespace yc::model
