// SPDX-License-Identifier: Apache-2.0
// Excel parity: cell formatting - engine, model and .ycg parts
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "engine/rule_eval.h"
#include "formula/datetime.h"
#include "io/ycg.h"
#include "test_helpers.h"

#include <cmath>

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
model::CfRule Greater(const char* range, const char* value) {
  model::CfRule r;
  r.type = model::CfType::CellValue;
  r.op = model::CmpOp::Greater;
  r.formula1 = value;
  r.fmt.has_fill = true;
  r.fmt.fill_color = 0xFFFFC7CE;
  r.ranges = {R(range)};
  return r;
}
bool Filled(yctest::Doc& t, const char* a1) {
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v;
  const auto p = yctest::Doc::P(a1);
  return ev.Eval(p.row, p.col, &v) && v.has_fill;
}
}  // namespace

TEST_CASE("Merge keeps a lone value that isn't top-left (item 6)", "[parity_formatting]") {
  yctest::Doc t;
  t.Set("B1", "Total");
  t.d.Begin("Merge");
  REQUIRE(t.d.Merge(t.sheet(), R("A1:C1"), false, true));
  t.d.Commit();
  REQUIRE(t.V("A1").IsString());
  CHECK(t.V("A1").str() == "Total");
  CHECK(t.V("B1").IsEmpty());
  // two values: only the upper-left one stays (the UI asks first)
  t.Set("A2", "1");
  t.Set("B2", "2");
  t.d.Merge(t.sheet(), R("A2:B2"), false, false);
  CHECK(t.N("A2") == 1);
  CHECK(t.V("B2").IsEmpty());
}

TEST_CASE("Clear Formats takes the conditional formatting; Clear All also notes and links (item 10)", "[parity_formatting]") {
  yctest::Doc t;
  t.d.AddCondFormat(t.sheet(), Greater("B2:B5", "5"));
  t.d.ClearFormats(t.sheet(), R("B2:B5"));
  CHECK(t.d.wb().sheets[0]->cond_formats->empty());

  t.d.AddCondFormat(t.sheet(), Greater("B2:B5", "5"));
  t.d.SetNote(t.sheet(), 1, 1, "me", "a note");
  model::Hyperlink l;
  l.address = "https://example.com";
  t.d.SetHyperlink(t.sheet(), 2, 1, l);
  // the engine's own Clear All (cut, refresh) keeps them
  t.d.ClearAll(t.sheet(), R("B2:B5"));
  CHECK(t.d.wb().sheets[0]->cond_formats->size() == 1);
  CHECK(t.d.wb().sheets[0]->notes->size() == 1);
  // Home > Clear > Clear All, Range.Clear: everything
  t.d.ClearAll(t.sheet(), R("B2:B5"), engine::kSrcUser, true);
  CHECK(t.d.wb().sheets[0]->cond_formats->empty());
  CHECK(t.d.wb().sheets[0]->notes->empty());
  CHECK(t.d.wb().sheets[0]->links->empty());
}

TEST_CASE("Paste and Format Painter bring the conditional formatting (item 11)", "[parity_formatting]") {
  yctest::Doc t;
  t.Set("A1", "9");
  t.d.AddCondFormat(t.sheet(), Greater("A1:A3", "5"));
  const engine::ClipData clip = t.d.Copy(t.sheet(), R("A1"));
  REQUIRE(clip.rules.size() == 1);
  // painted cell by cell (Format Painter tiles its paste): the same rule as the source's, so that rule grows to cover
  // the painted cells (Excel merges identical rules instead of piling up copies)
  for (uint32_t r = 0; r < 3; ++r) t.d.Paste(t.sheet(), r, 2, clip, engine::PasteMode::Formats);
  {
    const auto& rules = *t.d.wb().sheets[0]->cond_formats;
    REQUIRE(rules.size() == 1);
    CHECK(rules[0].ranges.size() == 2);  // A1:A3 and C1:C3 (the painted cells join one range as they come)
    CHECK(rules[0].formula1 == "5");
  }
  t.Set("C2", "7");
  CHECK(Filled(t, "C2"));
  t.Set("C3", "1");
  CHECK_FALSE(Filled(t, "C3"));
  // a rule that differs from what is at the destination is added as its own rule
  yctest::Doc g;
  g.d.AddCondFormat(g.sheet(), Greater("A1", "5"));
  g.d.AddCondFormat(g.sheet(), Greater("C1:C3", "50"));
  g.d.Paste(g.sheet(), 0, 2, g.d.Copy(g.sheet(), R("A1")), engine::PasteMode::Formats);
  CHECK(g.d.wb().sheets[0]->cond_formats->size() == 2);  // C1 now follows "> 5" (grown), C2:C3 keep "> 50"
  g.Set("C1", "7");
  g.Set("C2", "7");
  CHECK(Filled(g, "C1"));
  CHECK_FALSE(Filled(g, "C2"));
  // a formula rule: its relative references move with the cells (D5 compares with E5)
  yctest::Doc u;
  model::CfRule e;
  e.type = model::CfType::Expression;
  e.formula1 = "A1>B1";
  e.fmt.has_fill = true;
  e.fmt.fill_color = 0xFFFF0000;
  e.ranges = {R("A1:A2")};
  u.d.AddCondFormat(u.sheet(), e);
  u.d.Paste(u.sheet(), 4, 3, u.d.Copy(u.sheet(), R("A1:A2")), engine::PasteMode::All);  // to D5
  u.Set("D5", "2");
  u.Set("E5", "1");
  u.Set("D6", "1");
  u.Set("E6", "2");
  CHECK(Filled(u, "D5"));
  CHECK_FALSE(Filled(u, "D6"));
  // pasted into another sheet's rules it is rebased: a rule made for D5:D6 alone reads D5>E5
  yctest::Doc x;
  x.d.AddCondFormat(x.sheet(), e);
  model::CfRule other = e;
  other.formula1 = "A1<B1";  // something else at the destination keeps it from merging
  other.ranges = {R("H1")};
  x.d.AddCondFormat(x.sheet(), other);
  x.d.ClearCondFormats(x.sheet(), R("A1:A2"));
  engine::ClipData xc = u.d.Copy(u.sheet(), R("A1:A2"));
  x.d.Paste(x.sheet(), 4, 3, xc, engine::PasteMode::All);
  bool found = false;
  for (const auto& r : *x.d.wb().sheets[0]->cond_formats) {
    if (r.ranges.front() == R("D5:D6")) found = r.formula1 == "D5>E5";
  }
  CHECK(found);
  // values and number formats don't bring rules
  yctest::Doc w;
  w.d.AddCondFormat(w.sheet(), Greater("A1", "5"));
  w.d.Paste(w.sheet(), 0, 5, w.d.Copy(w.sheet(), R("A1")), engine::PasteMode::Values);
  CHECK(w.d.wb().sheets[0]->cond_formats->size() == 1);
}

TEST_CASE("Standard Width changes the columns not sized by hand, undone in one step (item 12)", "[parity_formatting]") {
  yctest::Doc t;
  t.d.Begin("Width");
  t.d.SetColWidth(t.sheet(), 1, 1, 100);
  t.d.Commit();
  t.d.Begin("Standard Width");
  t.d.SetStandardWidth(t.sheet(), 89);  // 12 characters
  t.d.Commit();
  CHECK(t.d.wb().sheets[0]->cols->Size(0) == 89);
  CHECK(t.d.wb().sheets[0]->cols->Size(1) == 100);
  t.d.Undo();
  CHECK(t.d.wb().sheets[0]->cols->Size(0) == 64);
}

TEST_CASE("Conditional formatting: text operators, dates occurring, average variants, scale points (items 13, 15, 16)", "[parity_formatting]") {
  yctest::Doc t;
  t.Set("A1", "ABC");
  t.Set("A2", "XAB");
  t.Set("A3", "AB");
  model::CfRule r;
  r.type = model::CfType::TextContains;
  r.text = "ab";
  r.text_op = 2;  // begins with
  r.fmt.has_fill = true;
  r.fmt.fill_color = 0xFFFFFF00;
  r.ranges = {R("A1:A3")};
  t.d.AddCondFormat(t.sheet(), r);
  CHECK(Filled(t, "A1"));
  CHECK_FALSE(Filled(t, "A2"));
  CHECK(Filled(t, "A3"));

  // A Date Occurring: yesterday
  yctest::Doc d;
  const double today = std::floor(yc::formula::NowSerial());
  d.Set("A1", std::to_string(static_cast<int64_t>(today - 1)));
  d.Set("A2", std::to_string(static_cast<int64_t>(today)));
  model::CfRule p;
  p.type = model::CfType::TimePeriod;
  p.time_period = 1;
  p.fmt.has_fill = true;
  p.fmt.fill_color = 0xFFFFC7CE;
  p.ranges = {R("A1:A2")};
  d.d.AddCondFormat(d.sheet(), p);
  CHECK(Filled(d, "A1"));
  CHECK_FALSE(Filled(d, "A2"));

  // equal or above average; one standard deviation above
  yctest::Doc a;
  for (int i = 1; i <= 5; ++i) a.Set(("A" + std::to_string(i)).c_str(), std::to_string(i * 10));
  model::CfRule av;
  av.type = model::CfType::AboveAverage;
  av.equal_average = true;
  av.fmt.has_fill = true;
  av.fmt.fill_color = 0xFF00FF00;
  av.ranges = {R("A1:A5")};
  a.d.AddCondFormat(a.sheet(), av);
  CHECK(Filled(a, "A3"));  // 30 = the average
  CHECK_FALSE(Filled(a, "A2"));
  a.d.ClearCondFormats(a.sheet(), R("A1:A5"));
  av.equal_average = false;
  av.std_dev = 1;  // average 30 + sd 15.8 = 45.8
  a.d.AddCondFormat(a.sheet(), av);
  CHECK(Filled(a, "A5"));
  CHECK_FALSE(Filled(a, "A4"));

  // a 3-color scale whose midpoint is the number 0: 0 gets the midpoint color exactly
  yctest::Doc s;
  s.Set("A1", "-10");
  s.Set("A2", "0");
  s.Set("A3", "100");
  model::CfRule sc;
  sc.type = model::CfType::ColorScale;
  sc.points = 3;
  sc.colors[0] = 0xFFFF0000;
  sc.colors[1] = 0xFFFFFFFF;
  sc.colors[2] = 0xFF00FF00;
  sc.point_type[1] = 0;
  sc.point_value[1] = 0;
  sc.ranges = {R("A1:A3")};
  s.d.AddCondFormat(s.sheet(), sc);
  engine::CondFormatEval ev(s.d.wb(), s.sheet());
  engine::CfVisual v;
  REQUIRE(ev.Eval(1, 0, &v));
  CHECK((v.fill & 0xFFFFFF) == 0xFFFFFF);
}

TEST_CASE(".ycg keeps the new rule fields (text operator, period, averages, scale points)", "[parity_formatting]") {
  yctest::Doc t;
  model::CfRule r;
  r.type = model::CfType::TextContains;
  r.text = "x";
  r.text_op = 3;
  r.ranges = {R("A1:A3")};
  t.d.AddCondFormat(t.sheet(), r);
  model::CfRule p;
  p.type = model::CfType::TimePeriod;
  p.time_period = 8;
  p.ranges = {R("B1:B3")};
  t.d.AddCondFormat(t.sheet(), p);
  model::CfRule a;
  a.type = model::CfType::AboveAverage;
  a.below = true;
  a.equal_average = true;
  a.std_dev = 2;
  a.ranges = {R("C1:C3")};
  t.d.AddCondFormat(t.sheet(), a);
  model::CfRule c;
  c.type = model::CfType::ColorScale;
  c.point_type[0] = 0;
  c.point_value[0] = -5;
  c.point_type[1] = 4;
  c.point_formula[1] = "$Z$1";
  c.point_type[2] = 3;
  c.point_value[2] = 90;
  c.ranges = {R("D1:D3")};
  t.d.AddCondFormat(t.sheet(), c);
  const std::string bytes = io::SaveYcgToBytes(t.d.wb(), {});
  auto res = io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  const auto& got = *res.wb.sheets[0]->cond_formats;
  const auto& want = *t.d.wb().sheets[0]->cond_formats;
  REQUIRE(got.size() == want.size());
  for (size_t i = 0; i < got.size(); ++i) CHECK(got[i] == want[i]);
}

TEST_CASE("SetCondFormats replaces the rules in order, one undo step (the Rules Manager's OK)", "[parity_formatting]") {
  yctest::Doc t;
  t.d.AddCondFormat(t.sheet(), Greater("A1:A3", "1"));
  t.d.AddCondFormat(t.sheet(), Greater("B1:B3", "2"));
  std::vector<model::CfRule> v = *t.d.wb().sheets[0]->cond_formats;
  v.erase(v.begin());
  model::CfRule n = Greater("C1", "3");
  n.id = 0;
  v.push_back(n);
  t.d.Begin("Rules Manager");
  t.d.SetCondFormats(t.sheet(), v);
  t.d.Commit();
  const auto& now = *t.d.wb().sheets[0]->cond_formats;
  REQUIRE(now.size() == 2);
  CHECK(now[1].id != 0);
  CHECK(now[0].priority == 0);
  CHECK(now[1].priority == 1);
  t.d.Undo();
  CHECK(t.d.wb().sheets[0]->cond_formats->size() == 2);
}

TEST_CASE("Conditional formatting: Text that Contains takes SEARCH's wildcards", "[parity_formatting]") {
  yctest::Doc t;
  const char* texts[] = {"abc", "a*c", "xaZc", "ac", "bca", "123"};
  for (int i = 0; i < 6; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), texts[i]);
  model::CfRule r;
  r.type = model::CfType::TextContains;
  r.text = "a*c";
  r.text_op = 0;  // contains
  r.fmt.has_fill = true;
  r.fmt.fill_color = 0xFFFFFF00;
  r.ranges = {R("A1:A6")};
  t.d.AddCondFormat(t.sheet(), r);
  CHECK(Filled(t, "A1"));
  CHECK(Filled(t, "A2"));
  CHECK(Filled(t, "A3"));
  CHECK(Filled(t, "A4"));
  CHECK_FALSE(Filled(t, "A5"));
  CHECK_FALSE(Filled(t, "A6"));
  // ~ makes the next character plain: "a~*c" finds only the text a*c
  yctest::Doc u;
  for (int i = 0; i < 6; ++i) u.Set(("A" + std::to_string(i + 1)).c_str(), texts[i]);
  r.text = "a~*c";
  u.d.AddCondFormat(u.sheet(), r);
  CHECK_FALSE(Filled(u, "A1"));
  CHECK(Filled(u, "A2"));
  // does not contain: the cells the wildcard doesn't find
  yctest::Doc n;
  for (int i = 0; i < 6; ++i) n.Set(("A" + std::to_string(i + 1)).c_str(), texts[i]);
  r.text = "a?c";
  r.text_op = 1;
  n.d.AddCondFormat(n.sheet(), r);
  CHECK_FALSE(Filled(n, "A1"));  // abc
  CHECK(Filled(n, "A4"));        // ac
}
