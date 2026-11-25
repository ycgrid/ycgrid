// SPDX-License-Identifier: Apache-2.0
// geometry.h - coordinates, rectangles, A1 addresses (PLAN §5.1)
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yc::core {

inline constexpr uint32_t kDefaultRows = 1048576;
inline constexpr uint32_t kMaxRows = 2147483647u;   // hard max_rows limit (within int32)
inline constexpr uint32_t kMaxCols = 16384;         // XFD
inline constexpr uint32_t kRowEnd = 0xFFFFFFFFu;    // open-ended rectangle: up to the current limit (entire-column references)
inline constexpr uint32_t kColEnd = 0xFFFFFFFFu;

struct CellPos {
  uint32_t row = 0, col = 0;
  friend bool operator==(const CellPos& a, const CellPos& b) { return a.row == b.row && a.col == b.col; }
};

struct Rect {
  uint32_t r0 = 0, c0 = 0, r1 = 0, c1 = 0;  // closed interval
  static Rect Cell(uint32_t r, uint32_t c) { return Rect{r, c, r, c}; }
  bool Contains(uint32_t r, uint32_t c) const { return r >= r0 && r <= r1 && c >= c0 && c <= c1; }
  bool Contains(const Rect& o) const { return o.r0 >= r0 && o.r1 <= r1 && o.c0 >= c0 && o.c1 <= c1; }
  bool Intersects(const Rect& o) const { return !(o.r1 < r0 || o.r0 > r1 || o.c1 < c0 || o.c0 > c1); }
  bool IsCell() const { return r0 == r1 && c0 == c1; }
  uint64_t Rows() const { return uint64_t{r1} - r0 + 1; }
  uint64_t Cols() const { return uint64_t{c1} - c0 + 1; }
  uint64_t Count() const { return Rows() * Cols(); }
  Rect Union(const Rect& o) const { return Rect{std::min(r0, o.r0), std::min(c0, o.c0), std::max(r1, o.r1), std::max(c1, o.c1)}; }
  Rect Clamp(uint32_t row_limit, uint32_t col_limit) const {
    return Rect{r0, c0, std::min(r1, row_limit - 1), std::min(c1, col_limit - 1)};
  }
  friend bool operator==(const Rect& a, const Rect& b) { return a.r0 == b.r0 && a.c0 == b.c0 && a.r1 == b.r1 && a.c1 == b.c1; }
};

inline uint64_t CellKey(uint32_t sheet, uint32_t row, uint32_t col) {
  return (uint64_t{sheet} << 48) | (uint64_t{row} << 16) | uint64_t{col & 0xFFFF};
}

// areas as one Excel range: none repeated or inside another, neighbours that make a rectangle joined (a change
// event's Target: Ctrl+Enter on F1:F2 is "$F$1:$F$2", not "$F$1,$F$1:$F$2")
void TidyAreas(std::vector<Rect>* areas);

std::string ColumnName(uint32_t col);                 // 0 -> "A"
bool ParseColumnName(std::string_view s, uint32_t* col);
std::string CellA1(uint32_t row, uint32_t col, bool abs_row = false, bool abs_col = false);
std::string RectA1(const Rect& r);                    // "A1", "A1:B2", "A:A", "1:3"
bool ParseCellA1(std::string_view s, CellPos* out);   // accepts $
bool ParseRectA1(std::string_view s, Rect* out);      // "A1", "A1:B2", "A:C", "2:5"

// "Sheet1!A1:B2", "'My Sheet'!A1" -> sheet name (may be empty) and address
bool SplitSheetRef(std::string_view s, std::string* sheet, std::string_view* addr);
// adds single quotes when needed; in R1C1 formulas a name that reads as an A1 cell (T3) stays bare (Excel, measured)
std::string QuoteSheetName(std::string_view name, bool r1c1 = false);

}  // namespace yc::core
