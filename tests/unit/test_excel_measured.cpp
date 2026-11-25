// SPDX-License-Identifier: Apache-2.0
// Number formats against Excel itself: every (code, value) TEXT(value, code) case measured in Excel 16 where YCGrid
// agreed (excel_measured_formats.inc, generated from the measurement). A change to the format engine that fixes one
// case and breaks another fails here: the measured table, not the cases someone picked, decides. Left out on purpose
// (see docs/excel-parity-backlog.md): codes whose names a Chinese Excel spells in Chinese inside TEXT (General,
// [Red], [Blue], [Color10]), and the few measured differences listed under Open.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include <string>

namespace {

using V = yc::core::Value;

struct Measured {
  const char* code;
  V value;
  const char* excel;
};

const Measured kMeasured[] = {
#include "excel_measured_formats.inc"
};

std::string Quote(const char* code) {
  std::string q;
  for (const char* p = code; *p; ++p) {
    q += *p;
    if (*p == '"') q += '"';
  }
  return q;
}

}  // namespace

TEST_CASE("Number formats: every case measured in Excel 16 reads as Excel's (TEXT)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(sizeof(kMeasured) / sizeof(kMeasured[0]) > 2000);
  size_t failed = 0;
  for (const Measured& m : kMeasured) {
    t.d.SetValue(sh, 0, 0, m.value);
    const std::string got = t.Eval("TEXT(A1,\"" + Quote(m.code) + "\")");
    if (got != m.excel) {
      ++failed;
      if (failed <= 25) {
        INFO("code " << m.code << "  value " << yc::engine::DisplayTextOf(m.value, "General"));
        CHECK(got == std::string(m.excel));
      }
    }
  }
  CHECK(failed == 0);
}
