// SPDX-License-Identifier: Apache-2.0
// Formulas against Excel itself: every formula measured in Excel 16 where YCGrid gives Excel's result
// (excel_measured_formulas.inc, generated from the measurements: each set is a sheet of inputs with its formulas in
// column AD, all entered at once as they were measured). A change to the evaluator or to a function that fixes one
// formula and breaks another fails here: the measured table decides, not the cases someone picked. Numbers are
// compared to 12 digits (the last places depend on the runtime library), but a result Excel gives as 0 must be 0.
// What still differs from Excel is entered and not compared (YC_SKIP); docs/excel-parity-backlog.md lists it.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using V = yc::core::Value;

struct GridCell {
  int set;  // -1: the grid every set starts from
  const char* cell;
  const char* typed;
};

struct Case {
  int set;
  int row;  // the formula sits in AD<row>
  const char* formula;
  char kind;  // n number, s text, b logical, e error, - not compared
  const char* excel;
};

const GridCell kGrid[] = {
#define YC_GRID(set, cell, typed) {set, cell, typed},
#define YC_CASE(set, row, formula, kind, excel)
#define YC_SKIP(set, row, formula)
#include "excel_measured_formulas.inc"
#undef YC_GRID
#undef YC_CASE
#undef YC_SKIP
};

const Case kCases[] = {
#define YC_GRID(set, cell, typed)
#define YC_CASE(set, row, formula, kind, excel) {set, row, formula, kind, excel},
#define YC_SKIP(set, row, formula) {set, row, formula, '-', ""},
#include "excel_measured_formulas.inc"
#undef YC_GRID
#undef YC_CASE
#undef YC_SKIP
};

constexpr uint32_t kColAD = 29;

bool Agrees(const V& v, const Case& c) {
  const std::string_view excel = c.excel;
  switch (c.kind) {
    case 'n': {
      if (!v.IsNumeric()) return false;
      const double want = std::strtod(c.excel, nullptr);
      const double got = v.AsDouble();
      return got == want || std::fabs(got - want) <= 1e-12 * std::max({std::fabs(got), std::fabs(want), 1e-300});
    }
    case 's':
      return (v.IsString() && v.str() == excel) || (v.IsEmpty() && excel.empty());
    case 'b':
      return v.IsBool() && v.boolean() == (excel == "1");
    case 'e':
      return v.IsError() && excel == yc::core::ErrorText(v.error());
    default:
      return false;
  }
}

std::string Shown(const V& v) {
  const char* kind = v.IsEmpty() ? "empty" : v.IsNumeric() ? "number" : v.IsString() ? "text" : v.IsBool() ? "logical" : v.IsError() ? "error" : "other";
  return std::string(kind) + " " + yc::engine::DisplayTextOf(v, "General");
}

}  // namespace

TEST_CASE("Formulas: every formula measured in Excel 16 where YCGrid agreed gives Excel's result") {
  int sets = 0;
  for (const Case& c : kCases) sets = std::max(sets, c.set + 1);
  size_t compared = 0;
  size_t failed = 0;
  for (int set = 0; set < sets; ++set) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    const uint32_t sh2 = t.d.AddSheet("Sheet2");
    for (const GridCell& g : kGrid) {
      if (g.set != -1 && g.set != set) continue;
      std::string_view cell = g.cell;
      uint32_t on = sh;
      if (cell.rfind("Sheet2!", 0) == 0) {
        on = sh2;
        cell.remove_prefix(7);
      }
      yc::core::CellPos p;
      REQUIRE(yc::core::ParseCellA1(cell, &p));
      t.d.SetInput(on, p.row, p.col, g.typed);
    }
    for (const Case& c : kCases)
      if (c.set == set) t.d.SetInput(sh, static_cast<uint32_t>(c.row - 1), kColAD, c.formula);
    t.d.RecalcAll();
    for (const Case& c : kCases) {
      if (c.set != set || c.kind == '-') continue;
      ++compared;
      const V& v = t.d.GetValue(sh, static_cast<uint32_t>(c.row - 1), kColAD);
      if (Agrees(v, c)) continue;
      if (++failed <= 40) {
        INFO("set " << set << " AD" << c.row << "  " << c.formula << "  Excel: " << c.excel << "  YCGrid: " << Shown(v));
        CHECK(false);
      }
    }
  }
  REQUIRE(compared > 8000);
  CHECK(failed == 0);
}
