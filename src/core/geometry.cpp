// SPDX-License-Identifier: Apache-2.0
#include "core/geometry.h"

#include <cctype>
#include <tuple>

namespace yc::core {

std::string ColumnName(uint32_t col) {
  std::string s;
  uint64_t c = uint64_t{col} + 1;
  while (c > 0) {
    const uint64_t rem = (c - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    c = (c - 1) / 26;
  }
  return s;
}

bool ParseColumnName(std::string_view s, uint32_t* col) {
  if (s.empty() || s.size() > 4) return false;
  uint64_t v = 0;
  for (char ch : s) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (c < 'A' || c > 'Z') return false;
    v = v * 26 + static_cast<uint64_t>(c - 'A' + 1);
  }
  if (v == 0 || v > kMaxCols) return false;  // dynamic column growth (P2) needs its own naming rules
  *col = static_cast<uint32_t>(v - 1);
  return true;
}

std::string CellA1(uint32_t row, uint32_t col, bool abs_row, bool abs_col) {
  std::string s;
  if (abs_col) s += '$';
  s += ColumnName(col);
  if (abs_row) s += '$';
  s += std::to_string(uint64_t{row} + 1);
  return s;
}

std::string RectA1(const Rect& r) {
  const bool whole_cols = r.r0 == 0 && r.r1 == kRowEnd;
  const bool whole_rows = r.c0 == 0 && r.c1 == kColEnd;
  if (whole_cols && !whole_rows) return ColumnName(r.c0) + ":" + ColumnName(r.c1);
  if (whole_rows && !whole_cols) return std::to_string(uint64_t{r.r0} + 1) + ":" + std::to_string(uint64_t{r.r1} + 1);
  if (r.IsCell()) return CellA1(r.r0, r.c0);
  return CellA1(r.r0, r.c0) + ":" + CellA1(r.r1, r.c1);
}

bool ParseCellA1(std::string_view s, CellPos* out) {
  size_t i = 0;
  if (i < s.size() && s[i] == '$') ++i;
  const size_t cs = i;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
  if (i == cs) return false;
  uint32_t col;
  if (!ParseColumnName(s.substr(cs, i - cs), &col)) return false;
  if (i < s.size() && s[i] == '$') ++i;
  const size_t rs = i;
  uint64_t row = 0;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    row = row * 10 + static_cast<uint64_t>(s[i] - '0');
    if (row > kMaxRows) return false;
    ++i;
  }
  if (i == rs || i != s.size() || row == 0) return false;
  out->row = static_cast<uint32_t>(row - 1);
  out->col = col;
  return true;
}

namespace {
bool ParseRowNum(std::string_view s, uint32_t* row) {
  if (!s.empty() && s[0] == '$') s.remove_prefix(1);
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > kMaxRows) return false;
  }
  if (v == 0) return false;
  *row = static_cast<uint32_t>(v - 1);
  return true;
}
bool ParseColOnly(std::string_view s, uint32_t* col) {
  if (!s.empty() && s[0] == '$') s.remove_prefix(1);
  return ParseColumnName(s, col);
}
}  // namespace

bool ParseRectA1(std::string_view s, Rect* out) {
  const size_t colon = s.find(':');
  if (colon == std::string_view::npos) {
    CellPos p;
    if (!ParseCellA1(s, &p)) return false;
    *out = Rect::Cell(p.row, p.col);
    return true;
  }
  const std::string_view a = s.substr(0, colon), b = s.substr(colon + 1);
  CellPos pa, pb;
  if (ParseCellA1(a, &pa) && ParseCellA1(b, &pb)) {
    *out = Rect{std::min(pa.row, pb.row), std::min(pa.col, pb.col), std::max(pa.row, pb.row), std::max(pa.col, pb.col)};
    return true;
  }
  uint32_t x, y;
  if (ParseColOnly(a, &x) && ParseColOnly(b, &y)) {
    *out = Rect{0, std::min(x, y), kRowEnd, std::max(x, y)};
    return true;
  }
  if (ParseRowNum(a, &x) && ParseRowNum(b, &y)) {
    *out = Rect{std::min(x, y), 0, std::max(x, y), kColEnd};
    return true;
  }
  return false;
}

bool SplitSheetRef(std::string_view s, std::string* sheet, std::string_view* addr) {
  sheet->clear();
  const size_t bang = s.rfind('!');
  if (bang == std::string_view::npos) {
    *addr = s;
    return true;
  }
  std::string_view name = s.substr(0, bang);
  *addr = s.substr(bang + 1);
  if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
    name = name.substr(1, name.size() - 2);
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
      out += name[i];
      if (name[i] == '\'' && i + 1 < name.size() && name[i + 1] == '\'') ++i;
    }
    *sheet = out;
  } else {
    *sheet = std::string(name);
  }
  return !sheet->empty();
}

std::string QuoteSheetName(std::string_view name, bool r1c1_text) {
  // a name that starts with a digit or a period (.5), or reads as a logical value, needs quotes: written bare, .5!A1
  // reads as a number and TRUE!A1 as a logical value (Excel quotes them)
  bool needs = name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) || name[0] == '.';
  if (!needs && (name.size() == 4 || name.size() == 5)) {
    std::string up(name);
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    needs = up == "TRUE" || up == "FALSE";
  }
  for (unsigned char c : name) {
    if (c < 0x80 && !std::isalnum(c) && c != '_' && c != '.') needs = true;
  }
  if (!needs && !r1c1_text) {
    // text that looks like a cell address (e.g. A1, XFD1) needs quotes too (in A1 formulas only)
    CellPos p;
    if (ParseCellA1(name, &p)) needs = true;
  }
  if (!needs) {
    // and text that reads as an R1C1 reference (R, C, RC, R1C1, R12, C3): in R1C1 mode R1C1!RC would lex as a name
    size_t i = 0;
    bool r1c1 = true;
    for (char part : {'R', 'C'}) {
      if (i < name.size() && (name[i] == part || name[i] == static_cast<char>(part + 32))) {
        ++i;
        while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
      }
    }
    r1c1 = i > 0 && i == name.size();
    // and a name that starts with one followed by more text (R1C1x, C3PO): Excel quotes them
    if (!r1c1 && i > 1 && std::isdigit(static_cast<unsigned char>(name[i - 1]))) r1c1 = true;
    if (r1c1) needs = true;
  }
  if (!needs) return std::string(name);
  std::string out = "'";
  for (char c : name) {
    out += c;
    if (c == '\'') out += '\'';
  }
  out += '\'';
  return out;
}

// a change event's Target as Excel gives it: no area repeated or inside another, and neighbours that make a rectangle
// joined (Ctrl+Enter on F1:F2 gave "$F$1,$F$1:$F$2", a sort "$J$1,$J$2,$J$3")
void TidyAreas(std::vector<Rect>* areas) {
  auto& a = *areas;
  for (int pass = 0; pass < 3 && a.size() > 1; ++pass) {
    const size_t before = a.size();
    std::vector<Rect> kept;  // inside another: left out (the first of two equal ones stays)
    for (size_t i = 0; i < a.size(); ++i) {
      bool inside = false;
      for (size_t j = 0; j < a.size() && !inside; ++j) inside = j != i && a[j].Contains(a[i]) && !(a[i] == a[j] && j > i);
      if (!inside) kept.push_back(a[i]);
    }
    // the same columns, rows that touch: one area; then the same rows, columns that touch
    std::sort(kept.begin(), kept.end(), [](const Rect& x, const Rect& y) { return std::tie(x.c0, x.c1, x.r0) < std::tie(y.c0, y.c1, y.r0); });
    std::vector<Rect> down;
    for (const Rect& r : kept) {
      if (!down.empty() && down.back().c0 == r.c0 && down.back().c1 == r.c1 && r.r0 <= down.back().r1 + 1) down.back().r1 = std::max(down.back().r1, r.r1);
      else down.push_back(r);
    }
    std::sort(down.begin(), down.end(), [](const Rect& x, const Rect& y) { return std::tie(x.r0, x.r1, x.c0) < std::tie(y.r0, y.r1, y.c0); });
    a.clear();
    for (const Rect& r : down) {
      if (!a.empty() && a.back().r0 == r.r0 && a.back().r1 == r.r1 && r.c0 <= a.back().c1 + 1) a.back().c1 = std::max(a.back().c1, r.c1);
      else a.push_back(r);
    }
    if (a.size() == before) break;
  }
}

}  // namespace yc::core
