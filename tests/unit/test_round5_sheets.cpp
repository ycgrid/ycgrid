// SPDX-License-Identifier: Apache-2.0
// Round 5 (sheets and rules): formulas of conditional formats and data validation follow inserted rows, a list
// validation matches by value, a copied sheet's formulas name the copy, and very hidden sheets
#include <catch2/catch_test_macros.hpp>

#include "engine/rule_eval.h"
#include "io/ycg.h"
#include "test_helpers.h"

using yc::core::Rect;
namespace engine = yc::engine;
namespace model = yc::model;
namespace io = yc::io;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
}  // namespace

TEST_CASE("Round5: data validation and conditional format formulas follow inserted and deleted rows") {
  yctest::Doc t;
  for (int i = 1; i <= 10; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  model::DvRule dv;
  dv.type = model::DvType::WholeNumber;
  dv.op = model::CmpOp::Between;
  dv.formula1 = "1";
  dv.formula2 = "=$A$10";
  dv.ranges = {R("C3:C6")};
  t.d.SetValidation(t.sheet(), R("C3:C6"), dv);
  model::CfRule cf;
  cf.type = model::CfType::Expression;
  cf.formula1 = "A3>$A$1";  // relative to C3... the rule's first cell, A3 here
  cf.ranges = {R("A3:A6")};
  cf.fmt.has_fill = true;
  cf.fmt.fill_color = 0xFFFF0000;
  t.d.AddCondFormat(t.sheet(), cf);
  model::DvRule list;
  list.type = model::DvType::List;
  list.formula1 = "Buy,Sell";
  list.ranges = {R("D3")};
  t.d.SetValidation(t.sheet(), R("D3"), list);

  t.d.InsertRows(t.sheet(), 3, 2);  // two rows above row 4
  const auto& vs = *t.d.wb().sheets[0]->validations;
  bool found = false;
  for (const auto& v : vs) {
    if (v.type != model::DvType::WholeNumber) continue;
    found = true;
    CHECK(v.formula2 == "=$A$12");
    CHECK(v.formula1 == "1");
  }
  CHECK(found);
  for (const auto& v : vs) {
    if (v.type == model::DvType::List) CHECK(v.formula1 == "Buy,Sell");  // a list of values is no formula
  }
  const auto& cfs = *t.d.wb().sheets[0]->cond_formats;
  REQUIRE(cfs.size() == 1);
  CHECK(cfs[0].ranges.front() == R("A3:A8"));
  CHECK(cfs[0].formula1 == "A3>$A$1");  // the first cell didn't move, the relative reference neither

  t.d.InsertRows(t.sheet(), 0, 1);  // a row above everything: the rule's first cell moves, its formula with it
  CHECK(t.d.wb().sheets[0]->cond_formats->front().formula1 == "A4>$A$2");
  t.d.DeleteRows(t.sheet(), 12, 1);  // A13 (was A10) deleted: the validation's limit is #REF!
  for (const auto& v : *t.d.wb().sheets[0]->validations) {
    if (v.type == model::DvType::WholeNumber) CHECK(v.formula2.find("#REF!") != std::string::npos);
  }
}

TEST_CASE("Round5: a list validation matches a number by its value") {
  yctest::Doc t;
  t.Set("A1", "$1.00");  // 1 shown as $1.00
  t.Set("A2", "2.5");
  REQUIRE(t.T("A1").rfind("$1.00", 0) == 0);
  model::DvRule list;
  list.type = model::DvType::List;
  list.formula1 = "=$A$1:$A$2";
  list.ranges = {R("C1")};
  std::string why;
  CHECK(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 2, "1", list, &why));  // the list shows $1.00
  CHECK(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 2, "1.00", list, &why));
  CHECK(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 2, "2.5", list, &why));
  CHECK_FALSE(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 2, "3", list, &why));
  model::DvRule consts;
  consts.type = model::DvType::List;
  consts.formula1 = "10,20";
  consts.ranges = {R("C1")};
  CHECK(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 2, "10.0", consts, &why));
  CHECK_FALSE(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 2, "15", consts, &why));
}

TEST_CASE("Round5: a copied sheet's formulas that name the source name the copy") {
  yctest::Doc t;
  t.Set("A1", "5");
  t.Set("B1", "=Sheet1!A1+1");
  t.Set("C1", "=A1*2");
  const uint32_t nid = t.d.CopySheet(t.sheet(), 1);
  REQUIRE(nid != 0);
  const auto* ns = t.d.S(nid);
  REQUIRE(ns);
  CHECK(t.F("B1", nid) == "='" + ns->name + "'!A1+1");
  CHECK(t.F("C1", nid) == "=A1*2");
  CHECK(t.F("B1") == "=Sheet1!A1+1");  // the source keeps its own
  t.Set("A1", "7", nid);
  CHECK(t.N("B1", nid) == 8);
  CHECK(t.N("B1") == 6);
}

TEST_CASE("Round5: the last visible sheet can't be hidden, and very hidden sheets are kept in .ycg") {
  yctest::Doc t;
  const uint32_t one = t.sheet();
  const uint32_t two = t.d.AddSheet("Two");
  CHECK(t.d.SetSheetVisible(two, false, true));
  CHECK_FALSE(t.d.SetSheetVisible(one, false));  // the only visible sheet left
  CHECK(t.d.S(one)->visible);
  CHECK(t.d.S(two)->very_hidden);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK_FALSE(res.wb.sheets[1]->visible);
  CHECK(res.wb.sheets[1]->very_hidden);
  CHECK(t.d.SetSheetVisible(two, true));
  CHECK_FALSE(t.d.S(two)->very_hidden);
}

TEST_CASE("Round5: Find / Replace, filters and Remove Duplicates ignore case beyond ASCII") {
  yctest::Doc t;
  t.Set("A1", "Café");
  t.Set("A2", "CAFÉ");
  t.Set("A3", "ＡＢ");
  CHECK(engine::Lower("CAFÉ ＡＢ ΣЖ") == "café ａｂ σж");
  CHECK(t.d.Replace(t.sheet(), R("A1:A3"), "café", "X", false, true) == 2);  // both spellings (only one was replaced)
  CHECK(t.d.Replace(t.sheet(), R("A1:A3"), "ａｂ", "Y", false, true) == 1);
  CHECK(t.T("A3") == "Y");
  t.Set("B1", "É");
  t.Set("B2", "é");
  t.Set("B3", "x");
  CHECK(t.d.RemoveDuplicates(t.sheet(), R("B1:B3"), {1}, false) == 1);  // column B
  CHECK(t.T("B2") == "x");
  // a filter by values: one entry for both spellings
  yctest::Doc f;
  f.Set("A1", "Name");
  f.Set("A2", "Café");
  f.Set("A3", "CAFÉ");
  f.Set("A4", "Tea");
  REQUIRE(f.d.SetAutoFilter(f.sheet(), R("A1:A4")));
  model::FilterColumn fc;
  fc.kind = 1;
  fc.values = {"café"};
  fc.blanks = false;
  f.d.SetFilterColumn(f.sheet(), fc);
  const auto& rows = *f.d.wb().sheets[0]->rows;
  CHECK_FALSE(rows.IsHidden(1));
  CHECK_FALSE(rows.IsHidden(2));
  CHECK(rows.IsHidden(3));
}
