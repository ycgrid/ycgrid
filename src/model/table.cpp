// SPDX-License-Identifier: Apache-2.0
#include "model/table.h"

#include "model/workbook.h"

#include <algorithm>

namespace yc::model {

namespace {

bool IEq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 32);
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 32);
    if (x != y) return false;
  }
  return true;
}

}  // namespace

int Table::ColumnIndex(std::string_view n) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (IEq(columns[i].name, n)) return static_cast<int>(i);
  }
  return -1;
}

const Table* TableAt(const Workbook& wb, uint32_t sheet, uint32_t r, uint32_t c) {
  const Sheet* s = wb.SheetById(sheet);
  if (!s) return nullptr;
  for (const Table& t : *s->tables) {
    if (t.range.Contains(r, c)) return &t;
  }
  return nullptr;
}

const Table* FindTable(const Workbook& wb, std::string_view name, uint32_t* sheet) {
  for (const auto& sc : wb.sheets) {
    for (const Table& t : *sc->tables) {
      if (IEq(t.name, name)) {
        if (sheet) *sheet = sc->id;
        return &t;
      }
    }
  }
  return nullptr;
}

int ResolveTableRef(const Workbook& wb, uint32_t sheet, std::string_view table, uint8_t items, std::string_view col1, std::string_view col2,
                    uint32_t origin_r, uint32_t origin_c, uint32_t* out_sheet, core::Rect* out) {
  constexpr int kRef = 4, kValue = 3;
  uint32_t ts = sheet;
  const Table* t = table.empty() ? TableAt(wb, sheet, origin_r, origin_c) : FindTable(wb, table, &ts);
  if (!t) return kRef;
  core::Rect r = t->range;
  // columns
  if (!col1.empty()) {
    const int a = t->ColumnIndex(col1);
    const int b = col2.empty() ? a : t->ColumnIndex(col2);
    if (a < 0 || b < 0) return kRef;
    r.c0 = t->range.c0 + static_cast<uint32_t>(std::min(a, b));
    r.c1 = t->range.c0 + static_cast<uint32_t>(std::max(a, b));
  }
  // rows
  const core::Rect data = t->Data();
  const uint32_t header_row = t->range.r0, totals_row = t->range.r1;
  if (items & kItemThisRow) {
    if (ts != sheet || origin_r < data.r0 || origin_r > data.r1) return kValue;
    r.r0 = r.r1 = origin_r;
  } else if (items & kItemAll) {
    // the whole range
  } else {
    const bool h = (items & kItemHeaders) != 0, tot = (items & kItemTotals) != 0;
    const bool d = (items & kItemData) != 0 || items == 0;
    if (h && !t->header) return kRef;
    if (tot && !t->totals) return kRef;
    int64_t lo = -1, hi = -1;
    auto add = [&](int64_t a, int64_t b) {
      if (a > b) return;
      lo = lo < 0 ? a : std::min(lo, a);
      hi = std::max(hi, b);
    };
    if (h) add(header_row, header_row);
    if (d) add(data.r0, data.r1);
    if (tot) add(totals_row, totals_row);
    if (lo < 0) {
      // a table without data rows: its data body is the row after the header (like Excel's empty data row)
      if (d && t->header) lo = hi = header_row + 1;
      else return kRef;
    }
    r.r0 = static_cast<uint32_t>(lo);
    r.r1 = static_cast<uint32_t>(hi);
  }
  *out_sheet = ts;
  *out = r;
  return 0;
}

// (the main colors of a built-in style: see table_style.h for the whole look)
TableStyleColors ColorsOfTableStyle(std::string_view style) {
  auto rgb = [](uint32_t v) { return 0xFF000000u | v; };
  TableStyleColors c;
  c.header_text = rgb(0xFFFFFF);
  struct P {
    const char* id;
    uint32_t header, band, border;
  };
  static const P kStyles[] = {
      {"TableStyleMedium2", 0x4472C4, 0xD9E1F2, 0x8EA9DB}, {"TableStyleMedium3", 0xED7D31, 0xFCE4D6, 0xF4B084},
      {"TableStyleMedium4", 0xA5A5A5, 0xEDEDED, 0xC9C9C9}, {"TableStyleMedium5", 0xFFC000, 0xFFF2CC, 0xFFD966},
      {"TableStyleMedium6", 0x5B9BD5, 0xDDEBF7, 0x9BC2E6}, {"TableStyleMedium7", 0x70AD47, 0xE2EFDA, 0xA9D08E},
      {"TableStyleMedium1", 0x000000, 0xD9D9D9, 0x808080}, {"TableStyleLight1", 0xFFFFFF, 0xD9D9D9, 0x000000},
      {"TableStyleLight9", 0x4472C4, 0xFFFFFF, 0x4472C4}, {"TableStyleDark1", 0x000000, 0x737373, 0x404040},
  };
  // [YC] the black trading look (Taiwanese trading terminals): black rows, light text, a dark header
  if (style == "TableStyleTrading") {
    c.header = rgb(0x262626);
    c.header_text = rgb(0xC8C8C8);
    c.band = rgb(0x0B0B0B);
    c.border = rgb(0x333333);
    c.body = rgb(0x000000);
    c.body_text = rgb(0xE6E6E6);
    return c;
  }
  for (const P& p : kStyles) {
    if (style == p.id) {
      c.header = rgb(p.header);
      c.band = rgb(p.band);
      c.border = rgb(p.border);
      if (p.header == 0xFFFFFF) c.header_text = rgb(0x000000);
      return c;
    }
  }
  if (style.empty() || style == "TableStyleNone") return TableStyleColors{0, 0, 0, 0, true};
  c.header = rgb(0x4472C4);
  c.band = rgb(0xD9E1F2);
  c.border = rgb(0x8EA9DB);
  return c;
}

const std::vector<std::pair<std::string, std::string>>& TableStyleNames() {
  static const std::vector<std::pair<std::string, std::string>> v = {
      {"TableStyleMedium2", "Blue, Table Style Medium 2"},   {"TableStyleMedium3", "Orange, Table Style Medium 3"},
      {"TableStyleMedium4", "Gray, Table Style Medium 4"},   {"TableStyleMedium5", "Gold, Table Style Medium 5"},
      {"TableStyleMedium6", "Light Blue, Table Style Medium 6"}, {"TableStyleMedium7", "Green, Table Style Medium 7"},
      {"TableStyleMedium1", "Black, Table Style Medium 1"},  {"TableStyleLight1", "Light, Table Style Light 1"},
      {"TableStyleLight9", "Blue, Table Style Light 9"},     {"TableStyleDark1", "Dark, Table Style Dark 1"},
      {"TableStyleTrading", "Trading (black)"},             {"TableStyleNone", "None"},
  };
  return v;
}

}  // namespace yc::model
