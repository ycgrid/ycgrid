// SPDX-License-Identifier: Apache-2.0
// Coverage of defined names (engine/document_names.cpp) beyond test_names.cpp: Apply Names on the ends of a range
// (Excel 16, measured: data13_apply_names_*), Refers To that doesn't parse, a rename that rewrites the formulas using
// the name but not quoted text, a sheet's own name or a function, and how a kept union is shown.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/document.h"

using yc::core::Rect;
using yc::model::NameDef;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

NameDef Def(const char* name, const char* refers_to, int32_t scope = -1) {
  NameDef d;
  d.name = name;
  d.refers_to = refers_to;
  d.sheet_scope = scope;
  return d;
}

}  // namespace

TEST_CASE("Names coverage: Apply Names puts a name on either end of a range") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (const char* c : {"A1", "B2", "C3"}) t.Set(c, "1");
  REQUIRE(t.d.AddName("Rate", "=Sheet1!$B$2"));
  REQUIRE(t.d.AddName("Top", "=Sheet1!$A$1"));
  t.Set("E1", "=SUM(B2:C3)");
  t.Set("E2", "=SUM($B$2:C3)");
  t.Set("E3", "=SUM(A1:B2)");
  t.Set("E4", "=SUM($A$1:$B$2)");
  t.Set("E5", "=SUM(C3:C3)");
  // Ignore Relative/Absolute off: only the absolute ends take the name
  CHECK(t.d.ApplyNames(sh, Rect{0, 4, 4, 4}, {"Rate", "Top"}, false) == 2);
  CHECK(t.F("E1") == "=SUM(B2:C3)");
  CHECK(t.F("E2") == "=SUM(Rate:C3)");
  CHECK(t.F("E3") == "=SUM(A1:B2)");
  CHECK(t.F("E4") == "=SUM(Top:Rate)");
  // on (the default): relative ends too
  CHECK(t.d.ApplyNames(sh, Rect{0, 4, 4, 4}, {"Rate", "Top"}, true) == 2);
  CHECK(t.F("E1") == "=SUM(Rate:C3)");
  CHECK(t.F("E3") == "=SUM(Top:Rate)");
  CHECK(t.F("E5") == "=SUM(C3:C3)");
  CHECK(t.N("E4") == 2);
  CHECK(t.N("E1") == 2);
}

TEST_CASE("Names coverage: Refers To errors, replacing a name, renaming one in its formulas") {
  English en;
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::string err;
  CHECK_FALSE(t.d.AddName("Bad", "=SUM(", -1, &err));
  CHECK(err.rfind("The formula in Refers To has an error: ", 0) == 0);
  REQUIRE(t.d.AddName("Rate", "=0.1"));
  REQUIRE(t.d.AddName("rate", "=0.2"));  // the same name in another case replaces it
  size_t count = 0;
  for (const auto& n : *t.d.wb().names) count += n.name == "Rate" || n.name == "rate";
  CHECK(count == 1);
  REQUIRE(t.d.AddName("Other", "=1"));
  CHECK_FALSE(t.d.UpdateName("Nope", -1, Def("Nope", "=1"), &err));
  CHECK(err == "Name not found: \"Nope\".");
  CHECK_FALSE(t.d.UpdateName("Rate", -1, Def("Rate", "=(("), &err));
  CHECK(err.rfind("The formula in Refers To has an error: ", 0) == 0);
  CHECK_FALSE(t.d.UpdateName("Rate", -1, Def("OTHER", "=1"), &err));
  CHECK(err == "There's already a name \"OTHER\".");
  // a rename rewrites the formulas that use it, not text, a function's name or a sheet's prefix
  t.Set("A1", "=Rate*10");
  t.Set("A2", "=\"Rate\"&RATE(1,-1,2)");
  t.d.AddSheet("Rate's");
  t.Set("A3", "='Rate''s'!A1&Rate");
  REQUIRE(t.d.UpdateName("Rate", -1, Def("Pct", "=0.5"), &err));
  CHECK(t.F("A1") == "=Pct*10");
  CHECK(t.N("A1") == 5);
  CHECK(t.F("A2") == "=\"Rate\"&RATE(1,-1,2)");
  CHECK(t.F("A3") == "='Rate''s'!A1&Pct");
  // a sheet with a sheet-level name of the same name keeps its formulas
  const uint32_t s2 = t.d.AddSheet("Two");
  REQUIRE(t.d.AddName("Pct", "=0.9", static_cast<int32_t>(s2)));
  t.Set("A1", "=Pct", s2);
  REQUIRE(t.d.UpdateName("Pct", -1, Def("Share", "=0.5"), &err));
  CHECK(t.F("A1") == "=Share*10");
  CHECK(t.F("A1", s2) == "=Pct");
  CHECK(t.N("A1", s2) == 0.9);
  // a sheet that doesn't exist before the name
  yc::core::Rect r;
  uint32_t got = 0;
  CHECK_FALSE(t.d.ResolveNameRef("Nowhere!Pct", sh, &got, &r, &err));
  CHECK(err == "Sheet not found: \"Nowhere\".");
}

TEST_CASE("Names coverage: a kept union is shown as Excel writes it, anything else as kept") {
  yctest::Doc t;
  CHECK(yc::engine::ShownNameUnion("=((Sheet1!$A$1,Sheet1!$A$3))", &t.d) == "=Sheet1!$A$1,Sheet1!$A$3");
  CHECK(yc::engine::ShownNameUnion("((Sheet1!$A$1,Sheet1!$B$2:$C$3))", &t.d) == "Sheet1!$A$1,Sheet1!$B$2:$C$3");
  CHECK(yc::engine::ShownNameUnion("=Sheet1!$A$1", &t.d) == "=Sheet1!$A$1");
  CHECK(yc::engine::ShownNameUnion("=((1,2))", &t.d) == "=((1,2))");  // not references
  CHECK(yc::engine::ShownNameUnion("=((A1+1))", &t.d) == "=((A1+1))");
  CHECK(yc::engine::ShownNameUnion("=((A1,A2)", &t.d) == "=((A1,A2)");
  CHECK(yc::engine::ShownNameUnion("", &t.d).empty());
}
