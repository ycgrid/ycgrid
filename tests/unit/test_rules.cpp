// SPDX-License-Identifier: Apache-2.0
// Conditional formatting, data validation, AutoFilter
#include <catch2/catch_test_macros.hpp>

#include "engine/rule_eval.h"
#include "io/ycg.h"
#include "test_helpers.h"

using yc::core::Rect;
namespace engine = yc::engine;
namespace model = yc::model;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
}  // namespace

TEST_CASE("篩選條件比對（萬用字元、數值、空白）") {
  using engine::MatchCriterion;
  using yc::core::Value;
  CHECK(MatchCriterion(">100", Value::Number(150), "150"));
  CHECK_FALSE(MatchCriterion(">100", Value::Number(50), "50"));
  CHECK(MatchCriterion("<>0", Value::Number(1), "1"));
  CHECK(MatchCriterion("=台*", Value::String("台積電"), "台積電"));
  CHECK_FALSE(MatchCriterion("=台*", Value::String("鴻海"), "鴻海"));
  CHECK(MatchCriterion("=?x", Value::String("ax"), "ax"));
  CHECK(MatchCriterion("=", Value(), ""));
  CHECK(MatchCriterion("<>", Value::String("a"), "a"));
  CHECK(MatchCriterion("=abc", Value::String("ABC"), "ABC"));  // case-insensitive
  CHECK(MatchCriterion("=~*", Value::String("*"), "*"));
}

TEST_CASE("規則範圍：扣除區域、插入刪除列跟著移動") {
  yctest::Doc t;
  model::CfRule r;
  r.type = model::CfType::CellValue;
  r.op = model::CmpOp::Greater;
  r.formula1 = "10";
  r.ranges = {R("B2:B10")};
  t.d.AddCondFormat(t.sheet(), r);
  t.d.ClearCondFormats(t.sheet(), R("B5:B6"));
  const auto& v = *t.d.S(t.sheet())->cond_formats;
  REQUIRE(v.size() == 1);
  REQUIRE(v[0].ranges.size() == 2);
  CHECK(v[0].ranges[0] == R("B2:B4"));
  CHECK(v[0].ranges[1] == R("B7:B10"));
  t.d.InsertRows(t.sheet(), 0, 2);
  CHECK((*t.d.S(t.sheet())->cond_formats)[0].ranges[0] == R("B4:B6"));
  t.d.ClearCondFormats(t.sheet(), R("A1:Z100"));
  CHECK(t.d.S(t.sheet())->cond_formats->empty());
}

TEST_CASE("條件式格式求值") {
  yctest::Doc t;
  for (int i = 1; i <= 10; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  t.Set("B1", "x");
  t.Set("B2", "x");
  t.Set("B3", "y");
  t.Set("C1", "5");
  model::CfRule gt;
  gt.type = model::CfType::CellValue;
  gt.op = model::CmpOp::Greater;
  gt.formula1 = "$C$1";
  gt.fmt.has_fill = true;
  gt.fmt.fill_color = 0xFFFF0000;
  gt.ranges = {R("A1:A10")};
  t.d.AddCondFormat(t.sheet(), gt);
  model::CfRule top;
  top.type = model::CfType::Top10;
  top.rank = 2;
  top.fmt.bold = 1;
  top.ranges = {R("A1:A10")};
  t.d.AddCondFormat(t.sheet(), top);
  model::CfRule dup;
  dup.type = model::CfType::Unique;
  dup.fmt.has_font_color = true;
  dup.fmt.font_color = 0xFF0000FF;
  dup.ranges = {R("B1:B3")};
  t.d.AddCondFormat(t.sheet(), dup);
  model::CfRule expr;
  expr.type = model::CfType::Expression;
  expr.formula1 = "MOD(A1,2)=0";  // relative to A1: A4 uses MOD(A4,2)
  expr.fmt.italic = 1;
  expr.ranges = {R("A1:A10")};
  t.d.AddCondFormat(t.sheet(), expr);
  model::CfRule bar;
  bar.type = model::CfType::DataBar;
  bar.ranges = {R("A1:A10")};
  t.d.AddCondFormat(t.sheet(), bar);
  t.d.Recalc();
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v;
  CHECK(ev.Eval(5, 0, &(v = {})));  // A6 = 6 > 5
  CHECK(v.has_fill);
  CHECK(v.italic == 1);  // even
  CHECK(v.bold == -1);
  CHECK(v.bar > 0.5f);
  v = {};
  ev.Eval(9, 0, &v);  // A10 = 10: top 2
  CHECK(v.bold == 1);
  v = {};
  ev.Eval(2, 0, &v);  // A3 = 3: not above 5, odd
  CHECK_FALSE(v.has_fill);
  CHECK(v.italic == -1);
  v = {};
  CHECK(ev.Eval(0, 1, &v));  // B1 is a duplicate
  CHECK(v.has_font);
  v = {};
  CHECK_FALSE(ev.Eval(2, 1, &v));  // B3 is unique
}

TEST_CASE("資料驗證") {
  yctest::Doc t;
  t.Set("E1", "甲");
  t.Set("E2", "乙");
  model::DvRule list;
  list.type = model::DvType::List;
  list.formula1 = "=$E$1:$E$2";
  t.d.SetValidation(t.sheet(), R("A1:A5"), list);
  model::DvRule whole;
  whole.type = model::DvType::WholeNumber;
  whole.op = model::CmpOp::Between;
  whole.formula1 = "1";
  whole.formula2 = "100";
  t.d.SetValidation(t.sheet(), R("B1:B5"), whole);
  const auto& wb = t.d.wb();
  const auto& sh = *t.d.S(t.sheet());
  const model::DvRule* r = engine::FindValidation(sh, 2, 0);
  REQUIRE(r);
  CHECK(engine::ValidationList(wb, t.sheet(), *r, 2, 0) == std::vector<std::string>{"甲", "乙"});
  std::string msg;
  CHECK(engine::ValidateInput(wb, t.sheet(), 2, 0, "乙", *r, &msg));
  CHECK_FALSE(engine::ValidateInput(wb, t.sheet(), 2, 0, "丙", *r, &msg));
  const model::DvRule* w = engine::FindValidation(sh, 0, 1);
  REQUIRE(w);
  CHECK(engine::ValidateInput(wb, t.sheet(), 0, 1, "50", *w, &msg));
  CHECK_FALSE(engine::ValidateInput(wb, t.sheet(), 0, 1, "500", *w, &msg));
  CHECK_FALSE(engine::ValidateInput(wb, t.sheet(), 0, 1, "1.5", *w, &msg));
  CHECK(engine::ValidateInput(wb, t.sheet(), 0, 1, "", *w, &msg));  // blanks ignored
  // overwrite: a new rule replaces the old rules in its range
  model::DvRule any;
  any.type = model::DvType::Custom;
  any.formula1 = "TRUE";
  t.d.SetValidation(t.sheet(), R("A2:B2"), any);
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 1, 0)->type == model::DvType::Custom);
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 0, 0)->type == model::DvType::List);
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 2, 1)->type == model::DvType::WholeNumber);
}

TEST_CASE("Clear All removes data validation, like Excel's Range.Clear") {
  yctest::Doc t;
  model::DvRule list;
  list.type = model::DvType::List;
  list.formula1 = "Support,Resistance";
  t.d.SetValidation(t.sheet(), R("B14:B25"), list);
  t.d.ClearAll(t.sheet(), R("A17:K30"));  // a new layout over part of the old table
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 13, 1) != nullptr);  // B14..B16 keep theirs
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 15, 1) != nullptr);
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 16, 1) == nullptr);  // B17 is free again
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 24, 1) == nullptr);
  t.d.ClearContents(t.sheet(), R("B14:B16"));  // Clear Contents leaves the rule
  CHECK(engine::FindValidation(*t.d.S(t.sheet()), 13, 1) != nullptr);
}

TEST_CASE("自動篩選：依值、自訂條件、前 N 項、清除") {
  yctest::Doc t;
  t.Set("A1", "名稱");
  t.Set("B1", "數量");
  const char* names[] = {"台積電", "鴻海", "聯發科", "台達電", "中華電"};
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), names[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string((i + 1) * 10));
  }
  const uint32_t s = t.sheet();
  REQUIRE(t.d.SetAutoFilter(s, R("A1:B6")));
  model::FilterColumn byval;
  byval.field = 0;
  byval.kind = 1;
  byval.values = {"鴻海", "台達電"};
  byval.blanks = false;
  t.d.SetFilterColumn(s, byval);
  auto hidden = [&](uint32_t row) { return t.d.S(s)->rows->IsHidden(row); };
  CHECK(hidden(1));
  CHECK_FALSE(hidden(2));
  CHECK(hidden(3));
  CHECK_FALSE(hidden(4));
  CHECK_FALSE(hidden(0));  // header row
  model::FilterColumn crit;
  crit.field = 1;
  crit.kind = 2;
  crit.crit1 = ">=30";
  t.d.SetFilterColumn(s, crit);  // both columns' criteria hold: one row (40)
  CHECK(hidden(2));
  CHECK_FALSE(hidden(4));
  model::FilterColumn clear0;
  clear0.field = 0;
  t.d.SetFilterColumn(s, clear0);  // only >=30 is left: three rows
  CHECK(hidden(1));
  CHECK_FALSE(hidden(3));
  CHECK_FALSE(hidden(5));
  model::FilterColumn top;
  top.field = 1;
  top.kind = 3;
  top.count = 2;
  t.d.SetFilterColumn(s, top);
  CHECK(hidden(3));
  CHECK_FALSE(hidden(4));
  CHECK_FALSE(hidden(5));
  t.d.ShowAllData(s);
  for (uint32_t r = 0; r < 6; ++r) CHECK_FALSE(hidden(r));
  t.d.ClearAutoFilter(s);
  CHECK_FALSE(t.d.S(s)->autofilter->on);
}

TEST_CASE(".ycg 保存規則") {
  yctest::Doc t;
  const uint32_t s = t.sheet();
  model::CfRule c;
  c.type = model::CfType::ColorScale;
  c.points = 2;
  c.ranges = {R("A1:A9"), R("C1:C9")};
  t.d.AddCondFormat(s, c);
  model::DvRule d;
  d.type = model::DvType::List;
  d.formula1 = "買,賣";
  d.input_message = "選方向";
  t.d.SetValidation(s, R("D1:D9"), d);
  t.Set("A1", "h");
  t.Set("A2", "1");
  t.Set("A3", "2");
  t.d.SetAutoFilter(s, R("A1:A3"));
  model::FilterColumn f;
  f.kind = 2;
  f.crit1 = ">1";
  t.d.SetFilterColumn(s, f);
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& sh = *res.wb.sheets[0];
  REQUIRE(sh.cond_formats->size() == 1);
  CHECK((*sh.cond_formats)[0].ranges.size() == 2);
  CHECK((*sh.cond_formats)[0].points == 2);
  REQUIRE(sh.validations->size() == 1);
  CHECK((*sh.validations)[0].formula1 == "買,賣");
  CHECK((*sh.validations)[0].input_message == "選方向");
  CHECK(sh.autofilter->on);
  CHECK(sh.autofilter->columns.size() == 1);
  CHECK(sh.autofilter->hidden.count(1) == 1);
  CHECK(sh.rows->IsHidden(1));
}

TEST_CASE("全部尋找") {
  yctest::Doc t;
  t.Set("B3", "商品 1");
  t.Set("B4", "商品 2");
  t.Set("B12", "商品 10");
  t.Set("D1", "其他");
  auto hits = t.d.FindAll(t.sheet(), R("A1:Z100"), "商品 1", true, false, false, false);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].row == 2);
  CHECK(hits[1].row == 11);
  CHECK(t.d.FindAll(t.sheet(), R("A1:Z100"), "商品 1", true, false, true, false).size() == 1);
  CHECK(t.d.FindAll(t.sheet(), R("A1:Z100"), "商品*", true, false, true, false).size() == 3);
  CHECK(t.d.FindAll(t.sheet(), R("A1:Z100"), "不存在", true, false, false, false).empty());
}

TEST_CASE("數列：等差、等比、日期（月底、工作日）、終止值") {
  yctest::Doc t;
  const uint32_t s = t.sheet();
  t.Set("A1", "1");
  t.d.FillSeries(s, R("A1:A5"), true, 0, 0, 2, false, 0);
  CHECK(t.N("A5") == 9);
  t.Set("B1", "3");
  t.d.FillSeries(s, R("B1:F1"), false, 1, 0, 2, true, 30);
  CHECK(t.N("D1") == 12);
  CHECK(t.N("E1") == 24);
  CHECK(t.V("F1").IsEmpty());  // 48 > 30
  t.Set("C3", "2026/1/31");
  t.d.FillSeries(s, R("C3:C5"), true, 2, 2, 1, false, 0);
  CHECK(t.T("C4") == "2026/2/28");
  CHECK(t.T("C5") == "2026/3/31");
  t.Set("D3", "2026/10/23");  // a Friday
  t.d.FillSeries(s, R("D3:D4"), true, 2, 1, 1, false, 0);
  CHECK(t.T("D4") == "2026/10/26");  // the next Monday
}

TEST_CASE("Data bars with negative values: an axis at 0, negative bars to the left") {
  yctest::Doc t;
  t.Set("A1", "-50");
  t.Set("A2", "0");
  t.Set("A3", "150");
  model::CfRule bar;
  bar.type = model::CfType::DataBar;
  bar.ranges = {R("A1:A3")};
  t.d.AddCondFormat(t.sheet(), bar);
  t.d.Recalc();
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v;
  REQUIRE(ev.Eval(0, 0, &(v = {})));
  CHECK(v.bar_negative);
  CHECK(v.bar_axis == 0.25f);
  CHECK(v.bar == 0.25f);
  REQUIRE(ev.Eval(2, 0, &(v = {})));
  CHECK_FALSE(v.bar_negative);
  CHECK(v.bar == 0.75f);
  // only positive values: the bars start at the left edge
  t.Set("A1", "50");
  engine::CondFormatEval ev2(t.d.wb(), t.sheet());
  REQUIRE(ev2.Eval(0, 0, &(v = {})));
  CHECK(v.bar_axis == 0.0f);
  CHECK_FALSE(v.bar_negative);
}

TEST_CASE("a typed validation list matches with case, a list of cells without (Excel 16, measured)") {
  yctest::Doc t;
  REQUIRE(t.Set("D1", "AAPL"));
  REQUIRE(t.Set("D2", "MSFT"));
  std::string msg;
  auto ok = [&](const char* list, const char* typed) {
    model::DvRule rule;
    rule.type = model::DvType::List;
    rule.formula1 = list;
    rule.ranges.push_back(R("A1"));
    return engine::ValidateInput(t.d.wb(), t.sheet(), 0, 0, typed, rule, &msg);
  };
  // typed: with case; spaces around the entry don't count, around an item they do
  CHECK(ok("AAPL,MSFT", "AAPL"));
  CHECK_FALSE(ok("AAPL,MSFT", "aapl"));
  CHECK_FALSE(ok("AAPL,MSFT", "Aapl"));
  CHECK(ok("AAPL,MSFT", " AAPL"));
  CHECK(ok("AAPL,MSFT", " MSFT "));
  CHECK_FALSE(ok(" AAPL , MSFT ", "AAPL"));
  CHECK_FALSE(ok(" AAPL , MSFT ", " MSFT "));
  CHECK(ok("a b,Ab", "a b"));
  CHECK_FALSE(ok("a b,Ab", "a  b"));
  CHECK_FALSE(ok("a b,Ab", "AB"));
  // cells: without case, as the cells show
  CHECK(ok("=$D$1:$D$2", "aapl"));
  CHECK(ok("=$D$1:$D$2", "Msft"));
  CHECK_FALSE(ok("=$D$1:$D$2", "MSFT "));
}
