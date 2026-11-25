// SPDX-License-Identifier: Apache-2.0
// Round 8 (measured against Excel 16): PivotTables (errors in the source, items without case, blank cells, GETPIVOTDATA
// of an empty intersection, Show Values As on cells without a value, group labels), conditional formats (the
// population standard deviation, text rules on the value, duplicates of numbers as text, blanks of spaces, color scale
// ends, an error operand, Add appending) and data validation (the value, not its text; TRUE above every number;
// Ignore blank on a custom formula)
#include <catch2/catch_test_macros.hpp>

#include "core/i18n.h"
#include "engine/pivot_engine.h"
#include "engine/rule_eval.h"
#include "test_helpers.h"

#include <cmath>
#include <string>
#include <vector>

using yc::core::Rect;
using yc::core::Value;
namespace engine = yc::engine;
namespace model = yc::model;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}

void Fill(yctest::Doc& t, const std::vector<std::vector<const char*>>& rows) {
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t c = 0; c < rows[r].size(); ++c) {
      if (rows[r][c] && *rows[r][c]) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
    }
  }
}

// Sym, Side, Qty in A1:C7 (Excel's probe): AAPL and aapl, B with a blank Qty, C with #N/A, the text 10 and the number 10
void FillSyms(yctest::Doc& t) {
  Fill(t, {{"Sym", "Side", "Qty"}, {"AAPL", "Buy", "1"}, {"aapl", "Buy", "2"}, {"B", "Sell", ""}, {"C", "Sell", "=NA()"},
           {"'10", "Buy", "4"}, {"10", "Buy", "8"}});
}

model::PivotTable Pivot(yctest::Doc& t, const Rect& src) {
  model::PivotTable p;
  p.src_sheet = t.sheet();
  p.src = src;
  p.r0 = 0;
  p.c0 = 5;  // F1
  return p;
}

model::PivotAxisField Axis(int f) {
  model::PivotAxisField a;
  a.field = f;
  return a;
}

model::PivotDataField Data(int f, model::PivotFunc fn = model::PivotFunc::Sum) {
  model::PivotDataField d;
  d.field = f;
  d.func = fn;
  return d;
}

engine::PivotReport Report(yctest::Doc& t, const model::PivotTable& p, engine::PivotCache* cache) {
  std::string err;
  REQUIRE(engine::ReadPivotSource(t.d.wb(), p, cache, &err));
  return engine::ComputePivot(p, *cache);
}

bool IsErr(const Value& v, yc::core::Err e) { return v.IsError() && v.error() == e; }

void SetFormat(yctest::Doc& t, const char* a1, const char* code) {
  const auto p = yctest::Doc::P(a1);
  t.d.ModifyStyle(t.sheet(), Rect::Cell(p.row, p.col), [&](model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString(code); });
}

model::CfRule Rule(model::CfType type, const char* range) {
  model::CfRule r;
  r.type = type;
  r.fmt.has_fill = true;
  r.fmt.fill_color = 0xFFFF0000;
  r.ranges = {R(range)};
  return r;
}

bool Filled(yctest::Doc& t, const char* a1) {
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v;
  const auto p = yctest::Doc::P(a1);
  return ev.Eval(p.row, p.col, &v) && v.has_fill;
}

uint32_t FillOf(yctest::Doc& t, const char* a1) {
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v;
  const auto p = yctest::Doc::P(a1);
  REQUIRE(ev.Eval(p.row, p.col, &v));
  return v.fill & 0xFFFFFF;
}

bool Valid(yctest::Doc& t, const char* a1, const std::string& text, const model::DvRule& rule) {
  const auto p = yctest::Doc::P(a1);
  std::string msg;
  return engine::ValidateInput(t.d.wb(), t.sheet(), p.row, p.col, text, rule, &msg);
}

model::DvRule Dv(model::DvType type, const char* range, model::CmpOp op, const char* f1, const char* f2 = "") {
  model::DvRule r;
  r.type = type;
  r.op = op;
  r.formula1 = f1;
  r.formula2 = f2;
  r.ranges = {R(range)};
  return r;
}

}  // namespace

// ---- PivotTables ----

TEST_CASE("Round8 pivots: an error in the source shows in its sums; AAPL and aapl are one item, the text 10 and 10 two") {
  English en;
  yctest::Doc t;
  FillSyms(t);
  auto p = Pivot(t, R("A1:C7"));
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  engine::PivotCache cache;
  const auto rep = Report(t, p, &cache);
  // items: the number 10, the text 10 (numbers first), AAPL as first spelled, B, C
  REQUIRE(cache.items[0] == std::vector<std::string>{"10", "10", "AAPL", "B", "C"});
  CHECK(cache.item_values[0][0].IsNumber());
  CHECK(cache.item_values[0][1].IsString());
  REQUIRE(rep.rows == 8);
  REQUIRE(rep.cols == 4);
  CHECK(rep.at(2, 1).v.AsDouble() == 8);  // the number 10, Buy
  CHECK(rep.at(3, 1).v.AsDouble() == 4);  // the text 10, Buy
  CHECK(rep.at(4, 1).v.AsDouble() == 3);  // AAPL + aapl
  CHECK(rep.at(4, 3).v.AsDouble() == 3);
  // B: only a blank Qty: empty cells, not 0 (Sell and the row's total)
  CHECK(rep.at(5, 2).v.IsEmpty());
  CHECK(rep.at(5, 3).v.IsEmpty());
  // C / Sell holds #N/A, and so do the Sell total and the grand total
  CHECK(IsErr(rep.at(6, 2).v, yc::core::Err::NA));
  CHECK(IsErr(rep.at(6, 3).v, yc::core::Err::NA));
  CHECK(rep.at(7, 1).v.AsDouble() == 15);
  CHECK(IsErr(rep.at(7, 2).v, yc::core::Err::NA));
  CHECK(IsErr(rep.at(7, 3).v, yc::core::Err::NA));
  // Count counts the error and leaves the blank-only cell empty
  p.data = {Data(2, model::PivotFunc::Count)};
  const auto cnt = Report(t, p, &cache);
  CHECK(cnt.at(5, 2).v.IsEmpty());
  CHECK(cnt.at(6, 2).v.AsDouble() == 1);
  CHECK(cnt.at(7, 3).v.AsDouble() == 5);
  // a hidden item named in another case still hides the item
  p.data = {Data(2)};
  p.rows[0].hidden = {"aapl"};
  const auto hid = Report(t, p, &cache);
  CHECK(hid.rows == 7);
}

TEST_CASE("Round8 pivots: GETPIVOTDATA of items on the report without data there is 0") {
  English en;
  yctest::Doc t;
  FillSyms(t);
  auto p = Pivot(t, R("A1:C7"));
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  REQUIRE(t.d.AddPivot(t.sheet(), p) != 0);
  CHECK(t.Eval("GETPIVOTDATA(\"Sum of Qty\",$F$1,\"Sym\",\"B\",\"Side\",\"Buy\")") == "0");  // B has no Buy
  CHECK(t.Eval("GETPIVOTDATA(\"Sum of Qty\",$F$1,\"Sym\",\"B\")") == "0");                  // blanks only
  CHECK(t.Eval("GETPIVOTDATA(\"Sum of Qty\",$F$1,\"Sym\",\"aapl\")") == "3");
  CHECK(t.Eval("GETPIVOTDATA(\"Sum of Qty\",$F$1,\"Sym\",\"ZZZ\")") == "#REF!");  // not an item
  CHECK(t.Eval("GETPIVOTDATA(\"Sum of Qty\",$F$1,\"Sym\",\"C\")") == "#N/A");
}

TEST_CASE("Round8 pivots: Show Values As counts a cell without a value as 0") {
  English en;
  yctest::Doc t;
  // B has no Buy, C's Buy and D's Sell are blank
  Fill(t, {{"Sym", "Side", "Qty"}, {"A", "Buy", "10"}, {"A", "Sell", "5"}, {"B", "Sell", "5"}, {"C", "Buy", ""}, {"C", "Sell", "2"},
           {"D", "Buy", "4"}, {"D", "Sell", ""}});
  auto p = Pivot(t, R("A1:C8"));
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  p.data = {Data(2)};
  engine::PivotCache cache;
  // rows 2..5: A, B, C, D; columns 1 Buy, 2 Sell, 3 Grand Total
  p.data[0].show_as = model::PivotShowAs::PercentOfColumnTotal;
  auto rep = Report(t, p, &cache);
  CHECK(rep.at(3, 1).v.IsNumber());
  CHECK(rep.at(3, 1).v.AsDouble() == 0);  // B / Buy: no records
  CHECK(rep.at(4, 1).v.AsDouble() == 0);  // C / Buy: a blank
  CHECK(std::fabs(rep.at(2, 1).v.AsDouble() - 10.0 / 14.0) < 1e-12);
  p.data[0].show_as = model::PivotShowAs::RunningTotal;
  p.data[0].base_field = 0;
  rep = Report(t, p, &cache);
  CHECK(rep.at(3, 1).v.AsDouble() == 10);  // carried down past B
  CHECK(rep.at(4, 1).v.AsDouble() == 10);
  CHECK(rep.at(5, 1).v.AsDouble() == 14);
  p.data[0].show_as = model::PivotShowAs::DifferenceFrom;
  p.data[0].base_item = "A";
  rep = Report(t, p, &cache);
  CHECK(rep.at(3, 1).v.AsDouble() == -10);
  CHECK(rep.at(5, 2).v.AsDouble() == -5);
  CHECK(rep.at(2, 1).v.IsEmpty());  // the base item itself
  p.data[0].show_as = model::PivotShowAs::PercentDifferenceFrom;
  p.data[0].base_field = 1;
  p.data[0].base_item = "Buy";
  rep = Report(t, p, &cache);
  CHECK(rep.at(2, 2).v.AsDouble() == -0.5);
  CHECK(rep.at(3, 2).v.IsEmpty());                                // B: no Buy to compare with
  CHECK(rep.at(4, 2).v.IsEmpty());                                // C: its Buy is blank
  CHECK(IsErr(rep.at(5, 2).v, yc::core::Err::Null));              // D: Sell is blank, Buy is 4
}

TEST_CASE("Round8 pivots: group labels follow the data (fractions, Ending at, blanks below Starting at)") {
  English en;
  yctest::Doc t;
  Fill(t, {{"Date", "Qty", "Px"}, {"46021", "10", "1.5"}, {"46027", "", "2.25"}, {"46063", "3", "5.5"}, {"46100", "12", "1"}});
  auto p = Pivot(t, R("A1:C5"));
  p.data = {Data(1, model::PivotFunc::Count)};
  engine::PivotCache cache;
  std::string err;
  // numbers that aren't whole, By 1 from the smallest: "1-2", not "1-1"
  model::PivotGroup px;
  px.field = 2;
  px.by = model::PivotGroupBy::Numbers;
  px.interval = 1;
  px.auto_start = px.auto_end = true;
  p.groups = {px};
  REQUIRE(engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(cache.items[2] == std::vector<std::string>{"1-2", "2-3", "5-6"});
  // whole numbers from 0 By 5, with a blank: "<0 or (blank)" first
  model::PivotGroup q;
  q.field = 1;
  q.by = model::PivotGroupBy::Numbers;
  q.interval = 5;
  q.start = 0;
  q.end = 14;
  p.groups = {q};
  REQUIRE(engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(cache.items[1] == std::vector<std::string>{"<0 or (blank)", "0-4", "10-14"});
  // days By 7: the last group ends at Ending at
  model::PivotGroup d;
  d.field = 0;
  d.by = model::PivotGroupBy::Days;
  d.periods = model::kPeriodDays;
  d.days = 7;
  d.start = 46021;
  d.end = 46101;
  p.groups = {d};
  REQUIRE(engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  REQUIRE(!cache.items[0].empty());
  CHECK(cache.items[0].back() == "2026-03-17 - 2026-03-20");
  CHECK(cache.items[0].front() == "2025-12-30 - 2026-01-05");
}

// ---- conditional formatting ----

TEST_CASE("Round8 rules: FormatConditions.Add's rule goes last (a negative priority), the UI's on top") {
  yctest::Doc t;
  t.d.AddCondFormat(t.sheet(), Rule(model::CfType::Blanks, "A1"));
  auto last = Rule(model::CfType::Errors, "A2");
  last.priority = -1;
  const uint32_t id = t.d.AddCondFormat(t.sheet(), last);
  t.d.AddCondFormat(t.sheet(), Rule(model::CfType::NoBlanks, "A3"));
  int prio = -100;
  for (const auto& r : *t.d.wb().sheets[0]->cond_formats) {
    if (r.id == id) prio = r.priority;
  }
  CHECK(prio == 2);  // after the first rule and the one added on top later
}

TEST_CASE("Round8 rules: Above n standard deviations uses the population's (STDEV.P)") {
  yctest::Doc t;
  const char* vals[] = {"0", "0", "0", "10", "7.2", "7.4", "7.6", "0", "0", "0"};
  for (int i = 0; i < 10; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), vals[i]);
  auto r = Rule(model::CfType::AboveAverage, "A1:A10");
  r.std_dev = 1;
  t.d.AddCondFormat(t.sheet(), r);
  CHECK(Filled(t, "A6"));  // 7.4 > 3.22 + 4.008 (the sample's 4.225 left it out)
  CHECK_FALSE(Filled(t, "A5"));
  CHECK(Filled(t, "A4"));
}

TEST_CASE("Round8 rules: text rules read the value, not the formatted text; errors match none") {
  yctest::Doc t;
  t.Set("A1", "1234");
  SetFormat(t, "A1", "#,##0");
  t.Set("A2", "a,b");
  auto r = Rule(model::CfType::TextContains, "A1:A2");
  r.text = ",";
  t.d.AddCondFormat(t.sheet(), r);
  CHECK_FALSE(Filled(t, "A1"));  // 1,234 as shown, 1234 as a value
  CHECK(Filled(t, "A2"));
  t.Set("B1", "=1/0");
  t.Set("B2", "#DIV/0!x");
  t.Set("B3", "46027");
  SetFormat(t, "B3", "yyyy-mm-dd");
  auto b = Rule(model::CfType::TextContains, "B1:B2");
  b.text = "#";
  b.text_op = 2;  // begins with
  t.d.AddCondFormat(t.sheet(), b);
  CHECK_FALSE(Filled(t, "B1"));
  CHECK(Filled(t, "B2"));
  auto d = Rule(model::CfType::TextContains, "B3");
  d.text = "2026";
  t.d.AddCondFormat(t.sheet(), d);
  CHECK_FALSE(Filled(t, "B3"));  // the date's value is 46027
}

TEST_CASE("Round8 rules: duplicates take text that reads as a number for the number; blanks take spaces") {
  yctest::Doc t;
  t.Set("A1", "'01");
  t.Set("A2", "1");
  t.Set("A3", "'1");
  t.Set("A4", "' 1");
  t.Set("A5", "x");
  t.Set("A6", "'0.3");
  t.Set("A7", "0.3");
  t.d.AddCondFormat(t.sheet(), Rule(model::CfType::Unique, "A1:A7"));
  for (const char* a : {"A1", "A2", "A3", "A4", "A6", "A7"}) CHECK(Filled(t, a));
  CHECK_FALSE(Filled(t, "A5"));
  t.Set("B1", "=\"  \"");
  t.Set("B2", "x");
  t.d.AddCondFormat(t.sheet(), Rule(model::CfType::Blanks, "B1:B3"));
  CHECK(Filled(t, "B1"));
  CHECK_FALSE(Filled(t, "B2"));
  CHECK(Filled(t, "B3"));
  t.d.AddCondFormat(t.sheet(), Rule(model::CfType::NoBlanks, "C1:C2"));
  t.Set("C1", "=\"   \"");
  t.Set("C2", "y");
  CHECK_FALSE(Filled(t, "C1"));
  CHECK(Filled(t, "C2"));
}

TEST_CASE("Round8 rules: color scales color their ends first, and a minimum above the maximum still grades") {
  auto scale = [](const char* range, int points) {
    model::CfRule r;
    r.type = model::CfType::ColorScale;
    r.points = points;
    r.colors[0] = 0xFFFF0000;
    r.colors[1] = 0xFF00FF00;
    r.colors[2] = 0xFF0000FF;
    r.ranges = {R(range)};
    return r;
  };
  {
    yctest::Doc t;
    for (const char* a : {"A1", "A2", "A3"}) t.Set(a, "5");
    t.d.AddCondFormat(t.sheet(), scale("A1:A3", 3));
    CHECK(FillOf(t, "A1") == 0x0000FF);  // all equal: the maximum's color
  }
  {
    yctest::Doc t;
    t.Set("A1", "7");
    t.d.AddCondFormat(t.sheet(), scale("A1", 3));
    CHECK(FillOf(t, "A1") == 0x0000FF);  // one value
  }
  {
    yctest::Doc t;  // the midpoint (50th percentile) equals the lowest: the 1s get the minimum's color
    const char* v[] = {"1", "1", "1", "5"};
    for (int i = 0; i < 4; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), v[i]);
    t.d.AddCondFormat(t.sheet(), scale("A1:A4", 3));
    CHECK(FillOf(t, "A1") == 0xFF0000);
    CHECK(FillOf(t, "A4") == 0x0000FF);
  }
  {
    yctest::Doc t;  // the midpoint equals the highest: the 5s get the maximum's color
    const char* v[] = {"1", "5", "5", "5"};
    for (int i = 0; i < 4; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), v[i]);
    t.d.AddCondFormat(t.sheet(), scale("A1:A4", 3));
    CHECK(FillOf(t, "A1") == 0xFF0000);
    CHECK(FillOf(t, "A2") == 0x0000FF);
  }
  {
    yctest::Doc t;  // 2 colors, Number 8 to Number 2: 1 the minimum's color, 5 halfway, 10 the maximum's
    t.Set("A1", "1");
    t.Set("A2", "5");
    t.Set("A3", "10");
    auto r = scale("A1:A3", 2);
    r.point_type[0] = 0;
    r.point_value[0] = 8;
    r.point_type[2] = 0;
    r.point_value[2] = 2;
    t.d.AddCondFormat(t.sheet(), r);
    CHECK(FillOf(t, "A1") == 0xFF0000);
    const uint32_t mid = FillOf(t, "A2");
    CHECK(((mid >> 16) & 0xFF) >= 0x7E);
    CHECK(((mid >> 16) & 0xFF) <= 0x81);
    CHECK((mid & 0xFF) >= 0x7E);
    CHECK((mid & 0xFF) <= 0x81);
    CHECK(FillOf(t, "A3") == 0x0000FF);
  }
}

TEST_CASE("Round8 rules: Cell Value equal to an error matches nothing, not the blank cells") {
  yctest::Doc t;
  t.Set("A2", "1");
  t.Set("A3", "=1/0");
  auto r = Rule(model::CfType::CellValue, "A1:A3");
  r.op = model::CmpOp::Equal;
  r.formula1 = "1/0";
  t.d.AddCondFormat(t.sheet(), r);
  CHECK_FALSE(Filled(t, "A1"));
  CHECK_FALSE(Filled(t, "A2"));
  CHECK_FALSE(Filled(t, "A3"));
}

// ---- data validation ----

TEST_CASE("Round8 validation: a list checks the value (text, a formula's result, TRUE), not the text typed") {
  yctest::Doc t;
  const auto abc = Dv(model::DvType::List, "E1:E9", model::CmpOp::Between, "a,b,c");
  CHECK(Valid(t, "E1", "=\"b\"", abc));
  CHECK(Valid(t, "E1", "'b", abc));
  CHECK_FALSE(Valid(t, "E1", "'d", abc));
  const auto nums = Dv(model::DvType::List, "E1:E9", model::CmpOp::Between, "1,2,3");
  CHECK(Valid(t, "E1", "'1", nums));  // the text 1 is in a typed list
  CHECK(Valid(t, "E1", "1", nums));
  const auto tf = Dv(model::DvType::List, "E1:E9", model::CmpOp::Between, "TRUE,FALSE");
  CHECK(Valid(t, "E1", "'TRUE", tf));
  CHECK(Valid(t, "E1", "=TRUE", tf));
  // a list of cells: the text 3 isn't the number 3, x matches X
  t.Set("Z2", "x");
  t.Set("Z3", "y");
  t.Set("Z4", "3");
  const auto cells = Dv(model::DvType::List, "E1:E9", model::CmpOp::Between, "=$Z$2:$Z$4");
  CHECK_FALSE(Valid(t, "E1", "'3", cells));
  CHECK(Valid(t, "E1", "3", cells));
  CHECK(Valid(t, "E1", "X", cells));
  CHECK_FALSE(Valid(t, "E1", "=1/0", cells));
}

TEST_CASE("Round8 validation: TRUE is above every number; text length reads 15 digits") {
  yctest::Doc t;
  CHECK(Valid(t, "A1", "=TRUE", Dv(model::DvType::WholeNumber, "A1", model::CmpOp::Greater, "3")));
  CHECK(Valid(t, "A1", "=TRUE", Dv(model::DvType::WholeNumber, "A1", model::CmpOp::NotBetween, "1", "10")));
  CHECK(Valid(t, "A1", "=TRUE", Dv(model::DvType::Decimal, "A1", model::CmpOp::NotEqual, "5")));
  CHECK_FALSE(Valid(t, "A1", "=TRUE", Dv(model::DvType::WholeNumber, "A1", model::CmpOp::Between, "1", "10")));
  CHECK_FALSE(Valid(t, "A1", "=TRUE", Dv(model::DvType::WholeNumber, "A1", model::CmpOp::Less, "5")));
  CHECK(Valid(t, "A1", "=0.30000000000000004", Dv(model::DvType::TextLength, "A1", model::CmpOp::LessEqual, "3")));
}

TEST_CASE("Round8 validation: Ignore blank lets a custom formula that refers to an empty cell pass") {
  yctest::Doc t;
  const char* v[] = {"1", "3", "2", "5", "4"};
  for (int i = 0; i < 5; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), v[i]);
  auto rule = Dv(model::DvType::Custom, "A1:A5", model::CmpOp::Between, "=A1<A2");
  CHECK(Valid(t, "A1", "1", rule));
  CHECK_FALSE(Valid(t, "A2", "3", rule));
  CHECK(Valid(t, "A5", "4", rule));  // A6 is empty
  rule.ignore_blank = false;
  CHECK_FALSE(Valid(t, "A5", "4", rule));
}
