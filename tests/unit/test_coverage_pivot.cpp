// SPDX-License-Identifier: Apache-2.0
// Coverage of the PivotTable engine (engine/pivot_engine.cpp) where the other pivot tests don't reach: every summary
// function, every label and value filter type, grouping by hours, minutes, seconds, quarters and runs of days, number
// groups with an end, Show Values As kinds, calculated items' grammar, the source's errors and the style gallery.
// The reports are computed directly (ReadPivotSource + ComputePivot) and read by their labels.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/bind.h"
#include "engine/pivot_engine.h"

#include <cmath>
#include <set>

using yc::core::Rect;
using yc::core::Value;
using yc::engine::PivotReport;
using yc::model::PivotFilter;
using yc::model::PivotFunc;
using yc::model::PivotShowAs;
using yc::model::PivotTable;
using Catch::Matchers::WithinAbs;

namespace {

struct English {
  yc::i18n::Lang old = yc::i18n::Current();
  English() { yc::i18n::SetCurrent(yc::i18n::Lang::En); }
  ~English() { yc::i18n::SetCurrent(old); }
};

// Region, Product, Sales, Qty in A1:D7. Sums by Region: East 35 (10, 20, 5), North 7, West 70 (30, 40);
// by Region x Product: East A 15, B 20; North B 7; West A 30, B 40; Product totals A 45, B 67; grand total 112
void Fill(yctest::Doc& t) {
  const char* rows[][4] = {{"Region", "Product", "Sales", "Qty"}, {"East", "A", "10", "1"}, {"East", "B", "20", "2"}, {"West", "A", "30", "3"},
                           {"West", "B", "40", "4"},          {"East", "A", "5", "1"},  {"North", "B", "7", "1"}};
  for (int r = 0; r < 7; ++r) {
    for (int c = 0; c < 4; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
}

PivotTable Base(yctest::Doc& t, Rect src = Rect{0, 0, 6, 3}) {
  PivotTable p;
  p.src_sheet = t.sheet();
  p.src = src;
  p.c0 = 8;
  return p;
}

yc::model::PivotAxisField Axis(int f) {
  yc::model::PivotAxisField a;
  a.field = f;
  return a;
}

yc::model::PivotDataField Data(int f, PivotFunc fn = PivotFunc::Sum, PivotShowAs as = PivotShowAs::Normal, int base = -1) {
  yc::model::PivotDataField d;
  d.field = f;
  d.func = fn;
  d.show_as = as;
  d.base_field = base;
  return d;
}

PivotReport Report(yctest::Doc& t, const PivotTable& p, yc::engine::PivotCache* cache) {
  std::string err;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, cache, &err));
  return yc::engine::ComputePivot(p, *cache);
}
PivotReport Report(yctest::Doc& t, const PivotTable& p) {
  yc::engine::PivotCache cache;
  return Report(t, p, &cache);
}

std::string Text(const Value& v) {
  if (v.IsString()) return std::string(v.str());
  if (v.IsBool()) return v.boolean() ? "TRUE" : "FALSE";
  return yc::engine::BindKeyText(v);
}

// the item rows' labels, in order (compact form: the first column)
std::vector<std::string> Labels(const PivotReport& rep) {
  std::vector<std::string> out;
  for (uint32_t r = 0; r < rep.rows; ++r) {
    if (r < rep.row_kind.size() && rep.row_kind[r] == 0) out.push_back(Text(rep.at(r, 0).v));
  }
  return out;
}

// the report row whose label is `label` (the first one after the row labelled `after`, when given)
uint32_t RowOf(const PivotReport& rep, const std::string& label, const std::string& after = "") {
  bool seen = after.empty();
  for (uint32_t r = 0; r < rep.rows; ++r) {
    const std::string t = Text(rep.at(r, 0).v);
    if (!seen) {
      seen = t == after;
      continue;
    }
    if (t == label) return r;
  }
  FAIL_CHECK("no row labelled " << label);
  return 0;
}

// the report column whose header reads `label`
uint32_t ColOf(const PivotReport& rep, const std::string& label) {
  const uint32_t first_body = rep.page_rows + rep.header_rows;
  for (uint32_t r = 0; r < first_body && r < rep.rows; ++r) {
    for (uint32_t c = 0; c < rep.cols; ++c) {
      if (Text(rep.at(r, c).v) == label) return c;
    }
  }
  FAIL_CHECK("no column labelled " << label);
  return 0;
}

// the value of data field d in the row labelled `label` (values in columns, no column fields)
Value DataAt(const PivotReport& rep, const std::string& label, int d) {
  const uint32_t r = RowOf(rep, label);
  for (uint32_t c = 0; c < rep.cols; ++c) {
    if (c < rep.col_data.size() && rep.col_data[c] == d) return rep.at(r, c).v;
  }
  FAIL_CHECK("no column for data field " << d);
  return Value();
}

bool IsErr(const Value& v, yc::core::Err e) { return v.IsError() && v.error() == e; }

}  // namespace

TEST_CASE("Pivot coverage: every summary function, with one record, with text and with errors") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2, PivotFunc::Max),    Data(2, PivotFunc::Min),      Data(2, PivotFunc::Product), Data(2, PivotFunc::Var),
            Data(2, PivotFunc::StdDev), Data(2, PivotFunc::VarP),     Data(2, PivotFunc::StdDevP), Data(1, PivotFunc::CountNums),
            Data(1, PivotFunc::DistinctCount), Data(1, PivotFunc::Average), Data(1, PivotFunc::Max), Data(2, PivotFunc::Average)};
  const auto rep = Report(t, p);
  CHECK(Labels(rep) == std::vector<std::string>{"East", "North", "West"});
  CHECK(DataAt(rep, "East", 0).AsDouble() == 20);
  CHECK(DataAt(rep, "East", 1).AsDouble() == 5);
  CHECK(DataAt(rep, "East", 2).AsDouble() == 1000);
  // East 10, 20, 5: the mean is 35/3, the squares about it sum to 350/3
  CHECK_THAT(DataAt(rep, "East", 3).AsDouble(), WithinAbs(175.0 / 3, 1e-9));
  CHECK_THAT(DataAt(rep, "East", 4).AsDouble(), WithinAbs(std::sqrt(175.0 / 3), 1e-9));
  CHECK_THAT(DataAt(rep, "East", 5).AsDouble(), WithinAbs(350.0 / 9, 1e-9));
  CHECK_THAT(DataAt(rep, "East", 6).AsDouble(), WithinAbs(std::sqrt(350.0 / 9), 1e-9));
  CHECK_THAT(DataAt(rep, "West", 3).AsDouble(), WithinAbs(50, 1e-9));
  // one record: the sample variance and deviation divide by zero, the population ones are 0
  CHECK(IsErr(DataAt(rep, "North", 3), yc::core::Err::Div0));
  CHECK(IsErr(DataAt(rep, "North", 4), yc::core::Err::Div0));
  CHECK(DataAt(rep, "North", 5).AsDouble() == 0);
  CHECK(DataAt(rep, "North", 6).AsDouble() == 0);
  // a text field: no numbers to count, average or take the largest of; distinct products per region
  CHECK(DataAt(rep, "East", 7).AsDouble() == 0);
  CHECK(DataAt(rep, "East", 8).AsDouble() == 2);
  CHECK(DataAt(rep, "North", 8).AsDouble() == 1);
  CHECK(IsErr(DataAt(rep, "East", 9), yc::core::Err::Div0));
  CHECK(DataAt(rep, "East", 10).AsDouble() == 0);
  CHECK(DataAt(rep, "Grand Total", 8).AsDouble() == 2);
  CHECK_THAT(DataAt(rep, "Grand Total", 11).AsDouble(), WithinAbs(112.0 / 6, 1e-9));

  // an error in the values: every function but the counts shows the first error
  t.Set("C3", "=1/0");
  t.d.Recalc();
  p.data = {Data(2, PivotFunc::Sum), Data(2, PivotFunc::Count), Data(2, PivotFunc::CountNums), Data(2, PivotFunc::Max)};
  const auto bad = Report(t, p);
  CHECK(IsErr(DataAt(bad, "East", 0), yc::core::Err::Div0));
  CHECK(DataAt(bad, "East", 1).AsDouble() == 3);
  CHECK(DataAt(bad, "East", 2).AsDouble() == 2);
  CHECK(IsErr(DataAt(bad, "East", 3), yc::core::Err::Div0));
  CHECK(DataAt(bad, "West", 3).AsDouble() == 40);
}

TEST_CASE("Pivot coverage: label filters of every type, on text and on numbers") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  auto labels = [&](int type, const char* v1, const char* v2 = "") {
    p.rows[0].label_filter = PivotFilter{type, 0, v1, v2};
    return Labels(Report(t, p));
  };
  using L = std::vector<std::string>;
  CHECK(labels(15, "?ast") == L{"East"});  // equals, with wildcards
  CHECK(labels(15, "*TH") == L{"North"});  // case doesn't matter
  CHECK(labels(15, "n*h") == L{"North"});
  CHECK(labels(15, "east?") == L{});
  CHECK(labels(16, "*st") == L{"North"});  // doesn't equal
  CHECK(labels(17, "e") == L{"East"});     // begins with
  CHECK(labels(18, "e") == L{"North", "West"});
  CHECK(labels(19, "st") == L{"East", "West"});  // ends with
  CHECK(labels(20, "st") == L{"North"});
  CHECK(labels(21, "OR") == L{"North"});  // contains
  CHECK(labels(22, "or") == L{"East", "West"});
  CHECK(labels(23, "north") == L{"West"});  // greater than, as text
  CHECK(labels(24, "north") == L{"North", "West"});
  CHECK(labels(25, "north") == L{"East"});
  CHECK(labels(26, "north") == L{"East", "North"});
  CHECK(labels(27, "f", "w") == L{"North"});  // between: "west" is past "w"
  CHECK(labels(28, "f", "w") == L{"East", "West"});
  CHECK(labels(99, "x") == L{"East", "North", "West"});  // a type YCGrid doesn't know filters nothing
  // a field of numbers is compared as numbers (10 after 9), its items as shown
  p.rows = {Axis(3)};
  CHECK(labels(23, "2") == L{"3", "4"});
  CHECK(labels(27, "2", "3") == L{"2", "3"});
  CHECK(labels(25, "2") == L{"1"});
  CHECK(labels(23, "x") == L{});  // a number against text: compared as text ("1" < "x")
  CHECK(labels(15, "~?") == L{});  // ~ makes the next character literal
}

TEST_CASE("Pivot coverage: value filters of every type and the three Top 10 kinds") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  auto labels = [&](int type, const char* v1, const char* v2 = "") {
    p.rows[0].value_filter = PivotFilter{type, 0, v1, v2};
    return Labels(Report(t, p));
  };
  using L = std::vector<std::string>;  // East 35, North 7, West 70; the total is 112
  CHECK(labels(1, "1") == L{"West"});  // top 1 item
  CHECK(labels(2, "1") == L{"North"});  // bottom 1
  CHECK(labels(3, "50") == L{"West"});  // top 50% of the total: West alone reaches it
  CHECK(labels(4, "10") == L{"East", "North"});  // bottom 10%: North (7) is under 11.2, so East is taken too
  CHECK(labels(5, "80") == L{"East", "West"});  // top items summing to 80
  CHECK(labels(6, "5") == L{"North"});  // bottom sum 5: the first item always
  CHECK(labels(7, "35") == L{"East"});
  CHECK(labels(8, "35") == L{"North", "West"});
  CHECK(labels(10, "35") == L{"East", "West"});
  CHECK(labels(11, "35") == L{"North"});
  CHECK(labels(12, "35") == L{"East", "North"});
  CHECK(labels(13, "40", "5") == L{"East", "North"});  // between, the bounds in either order
  CHECK(labels(14, "5", "40") == L{"West"});
  CHECK(labels(42, "0") == L{"East", "North", "West"});
}

TEST_CASE("Pivot coverage: dates grouped by quarters, days, runs of days, hours, minutes and seconds") {
  English en;
  yctest::Doc t;
  t.Set("A1", "When");
  t.Set("B1", "Amount");
  const char* when[] = {"2024-01-05 09:30:15", "2024-01-05 13:05:45", "2024-02-20 00:00:00", "2024-05-11 23:59:59", "n/a"};
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), when[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
  }
  auto p = Base(t, Rect{0, 0, 5, 1});
  p.rows = {Axis(0)};
  p.data = {Data(1)};
  auto grouped = [&](uint32_t period, int days = 1, double start = 0, double end = 0) {
    yc::model::PivotGroup g;
    g.field = 0;
    g.by = yc::model::PivotGroupBy::Days;
    g.periods = period;
    g.days = days;
    g.start = start;
    g.end = end;
    p.groups = {g};
    return Labels(Report(t, p));
  };
  using L = std::vector<std::string>;
  CHECK(grouped(yc::model::kPeriodQuarters) == L{"Qtr1", "Qtr2", "n/a"});  // text after the groups
  CHECK(grouped(yc::model::kPeriodDays) == L{"5-Jan", "20-Feb", "11-May", "n/a"});
  CHECK(grouped(yc::model::kPeriodHours) == L{"12 AM", "9 AM", "1 PM", "11 PM", "n/a"});
  CHECK(grouped(yc::model::kPeriodMinutes) == L{":00", ":05", ":30", ":59", "n/a"});
  CHECK(grouped(yc::model::kPeriodSeconds) == L{":00", ":15", ":45", ":59", "n/a"});
  // runs of 30 days from the first date; Starting at and Ending at put the others in "<" and ">" groups
  const auto runs = grouped(yc::model::kPeriodDays, 30);
  REQUIRE(runs.size() == 4);
  CHECK(runs[0].find(" - ") != std::string::npos);
  const double feb1 = 45323, apr30 = 45412;  // 2024-02-01, 2024-04-30
  const auto bounded = grouped(yc::model::kPeriodMonths, 1, feb1, apr30);
  REQUIRE(bounded.size() == 4);
  CHECK(bounded[0].rfind("<", 0) == 0);
  CHECK(bounded[1] == "Feb");
  CHECK(bounded[2].rfind(">", 0) == 0);
  CHECK(bounded[3] == "n/a");
  // files from before several periods: one period by `by` alone
  auto by = [&](yc::model::PivotGroupBy b) {
    yc::model::PivotGroup g;
    g.field = 0;
    g.by = b;
    p.groups = {g};
    return Labels(Report(t, p));
  };
  CHECK(by(yc::model::PivotGroupBy::Months) == L{"Jan", "Feb", "May", "n/a"});
  CHECK(by(yc::model::PivotGroupBy::Quarters) == L{"Qtr1", "Qtr2", "n/a"});
  CHECK(by(yc::model::PivotGroupBy::YearMonths) == L{"2024-01", "2024-02", "2024-05", "n/a"});
  CHECK(by(yc::model::PivotGroupBy::Days) == L{"2024-01-05", "2024-02-20", "2024-05-11", "n/a"});
}

TEST_CASE("Pivot coverage: number groups with an end, text among the numbers, and booleans as items") {
  English en;
  yctest::Doc t;
  Fill(t);
  t.Set("C8", "n/a");
  t.Set("A8", "South");
  t.Set("B8", "A");
  t.Set("D8", "1");
  auto p = Base(t, Rect{0, 0, 7, 3});
  p.rows = {Axis(2)};
  p.data = {Data(3, PivotFunc::Count)};
  yc::model::PivotGroup g;
  g.field = 2;
  g.by = yc::model::PivotGroupBy::Numbers;
  g.start = 10;
  g.end = 30;
  g.interval = 10;
  p.groups = {g};
  CHECK(Labels(Report(t, p)) == std::vector<std::string>{"<10", "10-19", "20-29", "30-39", ">30", "n/a"});
  // TRUE and FALSE: after numbers and text, FALSE first
  yctest::Doc u;
  u.Set("A1", "Flag");
  u.Set("B1", "N");
  const char* flags[] = {"TRUE", "FALSE", "x", "TRUE", "3"};
  for (int i = 0; i < 5; ++i) {
    u.Set(("A" + std::to_string(i + 2)).c_str(), flags[i]);
    u.Set(("B" + std::to_string(i + 2)).c_str(), "1");
  }
  auto q = Base(u, Rect{0, 0, 5, 1});
  q.rows = {Axis(0)};
  q.data = {Data(1)};
  CHECK(Labels(Report(u, q)) == std::vector<std::string>{"3", "x", "FALSE", "TRUE"});
}

TEST_CASE("Pivot coverage: Show Values As over rows and columns") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  auto at = [&](PivotShowAs as, const std::string& row, const std::string& col, int base = -1) {
    p.data = {Data(2, PivotFunc::Sum, as, base)};
    const auto rep = Report(t, p);
    return rep.at(RowOf(rep, row), ColOf(rep, col)).v;
  };
  CHECK_THAT(at(PivotShowAs::PercentOfRowTotal, "East", "A").AsDouble(), WithinAbs(15.0 / 35, 1e-12));
  CHECK_THAT(at(PivotShowAs::Index, "East", "A").AsDouble(), WithinAbs(15.0 * 112 / (35 * 45), 1e-12));
  CHECK_THAT(at(PivotShowAs::PercentOfParentRow, "East", "A").AsDouble(), WithinAbs(15.0 / 45, 1e-12));
  CHECK(at(PivotShowAs::PercentOfParentRow, "Grand Total", "A").AsDouble() == 1);
  CHECK_THAT(at(PivotShowAs::PercentOfParentColumn, "East", "A").AsDouble(), WithinAbs(15.0 / 35, 1e-12));
  CHECK(at(PivotShowAs::PercentOfParentColumn, "East", "Grand Total").AsDouble() == 1);
  // running totals down the regions: North has no A, which counts as 0
  CHECK(at(PivotShowAs::RunningTotal, "North", "A", 0).AsDouble() == 15);
  CHECK(at(PivotShowAs::RunningTotal, "West", "A", 0).AsDouble() == 45);
  CHECK_THAT(at(PivotShowAs::PercentRunningTotal, "East", "A", 0).AsDouble(), WithinAbs(15.0 / 45, 1e-12));
  CHECK(at(PivotShowAs::PercentRunningTotal, "West", "A", 0).AsDouble() == 1);
  // ranks among the regions: a region without a value has no rank
  CHECK(at(PivotShowAs::RankDescending, "East", "A", 0).AsDouble() == 2);
  CHECK(at(PivotShowAs::RankAscending, "East", "A", 0).AsDouble() == 1);
  CHECK(at(PivotShowAs::RankAscending, "North", "A", 0).IsEmpty());
  // a base field on the columns
  CHECK(at(PivotShowAs::RunningTotal, "East", "B", 1).AsDouble() == 35);
  // a base field that isn't on the report: #N/A
  CHECK(IsErr(at(PivotShowAs::RunningTotal, "East", "A", 3), yc::core::Err::NA));
  // % of Parent Total: a product's share of its region
  p.rows = {Axis(0), Axis(1)};
  p.cols = {};
  p.data = {Data(2, PivotFunc::Sum, PivotShowAs::PercentOfParent, 0)};
  const auto rep = Report(t, p);
  const uint32_t ea = RowOf(rep, "A", "East");
  CHECK_THAT(rep.at(ea, 1).v.AsDouble(), WithinAbs(15.0 / 35, 1e-12));
  // a running total over the columns alone (no row fields): the innermost column field
  p.rows = {};
  p.cols = {Axis(1)};
  p.data = {Data(2, PivotFunc::Sum, PivotShowAs::RunningTotal)};
  const auto cols_only = Report(t, p);
  CHECK(cols_only.at(cols_only.rows - 1, ColOf(cols_only, "B")).v.AsDouble() == 112);
  // a total of 0: the share of a running total divides by zero
  t.Set("C2", "0");
  t.Set("C6", "0");
  t.Set("C4", "0");
  p.rows = {Axis(0)};
  p.cols = {Axis(1)};
  CHECK(IsErr(at(PivotShowAs::PercentRunningTotal, "East", "A", 0), yc::core::Err::Div0));
}

TEST_CASE("Pivot coverage: labels and levels, field areas and the kinds that need a base") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0), Axis(1)};
  p.data = {Data(2)};
  yc::engine::PivotCache cache;
  const auto rep = Report(t, p, &cache);
  CHECK(yc::engine::PivotColumnField(rep, 0) == 0);  // compact form: both fields in the first column
  CHECK(yc::engine::PivotColumnField(rep, 5) == -1);
  CHECK(yc::engine::PivotLabelLevel(rep, RowOf(rep, "A", "East"), 0) == 1);
  CHECK(yc::engine::PivotLabelLevel(rep, RowOf(rep, "East"), 0) == 0);  // the outer field: its row stands for it alone
  CHECK(yc::engine::PivotLabelLevel(rep, 0, 0) == -1);  // the header
  CHECK(yc::engine::PivotLabelLevel(rep, 9999, 0) == -1);
  CHECK(yc::engine::PivotFieldDefaultArea(cache, -1) == 0);
  CHECK(yc::engine::PivotFieldDefaultArea(cache, 0) == 0);  // text
  CHECK(yc::engine::PivotFieldDefaultArea(cache, 2) == 1);  // numbers
  CHECK(yc::engine::PivotFieldCaption(p, cache, 99) == "?");
  CHECK(yc::engine::PivotItemDisplay(cache, 99, "x") == "x");
  CHECK(yc::engine::PivotItemDisplay(cache, 0, "nowhere") == "nowhere");
  std::set<int> base, item;
  for (int k = 0; k < yc::model::kPivotShowAs; ++k) {
    if (yc::engine::PivotShowAsNeedsBase(static_cast<PivotShowAs>(k))) base.insert(k);
    if (yc::engine::PivotShowAsNeedsBaseItem(static_cast<PivotShowAs>(k))) item.insert(k);
  }
  using S = PivotShowAs;
  CHECK(item == std::set<int>{int(S::PercentOf), int(S::DifferenceFrom), int(S::PercentDifferenceFrom)});
  for (S k : {S::RunningTotal, S::PercentOf, S::PercentOfParent, S::DifferenceFrom, S::PercentDifferenceFrom, S::PercentRunningTotal,
              S::RankAscending, S::RankDescending}) {
    CHECK(base.count(int(k)) == 1);
  }
  for (S k : {S::Normal, S::PercentOfGrandTotal, S::PercentOfColumnTotal, S::PercentOfRowTotal, S::Index}) CHECK(base.count(int(k)) == 0);
}

TEST_CASE("Pivot coverage: calculated items' formulas, and calculated fields' empty formula") {
  const std::vector<std::string> items = {"East", "West", "North East"};
  std::vector<std::pair<std::string, double>> terms;
  std::string err;
  REQUIRE(yc::engine::ParseCalcItem(items, " = 2 * east + West/2 - 'North East'*3", &terms, &err));
  REQUIRE(terms.size() == 3);
  CHECK(terms[0] == std::pair<std::string, double>{"East", 2});
  CHECK(terms[1] == std::pair<std::string, double>{"West", 0.5});
  CHECK(terms[2] == std::pair<std::string, double>{"North East", -3});
  REQUIRE(yc::engine::ParseCalcItem(items, "-East", &terms, &err));
  CHECK(terms[0].second == -1);
  auto refused = [&](const char* f) {
    err.clear();
    CHECK_FALSE(yc::engine::ParseCalcItem(items, f, &terms, &err));
    return err;
  };
  CHECK(refused("=East*West").find("by numbers only") != std::string::npos);
  CHECK(refused("=East/0").find("by numbers only") != std::string::npos);
  CHECK(refused("=Nope").find("Not an item of this field: Nope") != std::string::npos);
  CHECK(refused("=3+East").find("Not an item of this field: 3") != std::string::npos);  // a number alone isn't an item
  CHECK(refused("=East West").find("sums and differences") != std::string::npos);
  CHECK(refused("=").find("Expected an item name") != std::string::npos);
  CHECK(refused("='North East").find("Expected an item name") != std::string::npos);  // an unclosed quote
  CHECK_FALSE(yc::engine::CheckPivotFormula({"Sales"}, "  ", &err));
  CHECK(err.find("Enter a formula") != std::string::npos);
  CHECK_FALSE(yc::engine::CheckPivotFormula({"Sales"}, "=", &err));
}

TEST_CASE("Pivot coverage: the source's errors, a table with a totals row and whole columns") {
  English en;
  yctest::Doc t;
  Fill(t);
  yc::engine::PivotCache cache;
  std::string err;
  auto p = Base(t);
  p.src_table = "NoSuchTable";
  CHECK_FALSE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(err.find("source table doesn't exist: NoSuchTable") != std::string::npos);
  p = Base(t);
  p.src_sheet = 999;
  CHECK_FALSE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(err.find("source sheet doesn't exist") != std::string::npos);
  p = Base(t, Rect{0, 0, 0, 3});
  CHECK_FALSE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(err.find("at least one row of data") != std::string::npos);
  p = Base(t, Rect{0, 0, 6, 600});
  CHECK_FALSE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(err.find("too wide") != std::string::npos);
  p = Base(t, Rect{0, 0, 6, 4});  // E1 has no heading
  CHECK_FALSE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(err.find("needs a heading") != std::string::npos);
  // whole columns A:D: the records end with the used range
  p = Base(t, Rect{0, 0, t.d.wb().sheets[0]->row_limit - 1, 3});
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(cache.source_records == 6);
  // a table's totals row is not a record
  const uint32_t id = t.d.AddTable(t.sheet(), Rect{0, 0, 6, 3}, true, "TableStyleMedium2");
  REQUIRE(id != 0);
  REQUIRE(t.d.UpdateTable(t.sheet(), id, [](yc::model::Table& tb) { tb.totals = true; }));
  REQUIRE(t.d.SetTableTotal(t.sheet(), id, 2, 109, ""));
  p = Base(t);
  p.src_table = t.d.TableById(t.sheet(), id)->name;
  REQUIRE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(cache.source_records == 6);
  // a PivotTable on the Data Model is kept but not read
  p.from_model = true;
  CHECK_FALSE(yc::engine::ReadPivotSource(t.d.wb(), p, &cache, &err));
  CHECK(err.find("Data Model") != std::string::npos);
}

TEST_CASE("Pivot coverage: every style in the gallery has a look, other names have none") {
  const auto names = yc::engine::PivotStyleNames();
  REQUIRE(names.size() == 84);
  std::set<uint32_t> headers;
  for (const auto& n : names) {
    const auto look = yc::engine::PivotStyleFor(n);
    CHECK_FALSE(look.none);
    CHECK((look.header_fill | look.header_line | look.band_fill | look.grand_fill | look.grand_line) != 0);
    headers.insert(look.header_fill ^ (look.header_line << 1) ^ (look.band_fill << 2));
  }
  CHECK(headers.size() > 40);  // the tiers and accents differ
  CHECK(yc::engine::PivotStyleFor("pivotstylemedium9").header_fill == yc::engine::PivotStyleFor("PivotStyleMedium9").header_fill);
  for (const char* n : {"None", "PivotStyleLight0", "PivotStyleLight29", "PivotStyleDarkX", "PivotStyleMedium1a", "TableStyleLight1", ""}) {
    CHECK(yc::engine::PivotStyleFor(n).none);
  }
}

TEST_CASE("Pivot coverage: GETPIVOTDATA under a value filter reads what the report shows (Excel 16, measured; gaps_fuzz seed 20003)") {
  English en;
  yctest::Doc t;
  Fill(t);
  auto p = Base(t);
  p.rows = {Axis(0)};
  p.data = {Data(2)};
  p.rows[0].value_filter = PivotFilter{1, 0, "2", ""};  // Top 2: West 70 and East 35, North 7 left out
  yc::engine::PivotCache cache;
  const PivotReport rep = Report(t, p, &cache);
  CHECK(Labels(rep) == std::vector<std::string>{"East", "West"});
  Value v;
  REQUIRE(yc::engine::PivotLookup(p, cache, "Sum of Sales", {}, &v));
  CHECK(v.AsDouble() == 105);  // the grand total of the two shown (it summed all three: 112)
  REQUIRE(yc::engine::PivotLookup(p, cache, "Sales", {{"Region", "West"}}, &v));
  CHECK(v.AsDouble() == 70);
  CHECK_FALSE(yc::engine::PivotLookup(p, cache, "Sales", {{"Region", "North"}}, &v));  // not on the report: #REF!
  // Show Details still lists every record: the value filter doesn't apply there (Excel 16, measured)
  CHECK(yc::engine::PivotDetailRecords(p, cache, {}).size() == 6);
}
