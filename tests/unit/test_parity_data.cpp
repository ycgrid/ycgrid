// SPDX-License-Identifier: Apache-2.0
// Data tab parity with Excel: sort, AutoFilter, validation, Remove Duplicates, outline,
// Advanced Filter, Data Analysis
#include <catch2/catch_test_macros.hpp>

#include "engine/rule_eval.h"
#include "io/ycg.h"
#include "test_helpers.h"

using yc::core::Rect;
using yc::core::Value;
namespace engine = yc::engine;
namespace model = yc::model;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
std::string S(yctest::Doc& t, const char* a1) { return t.V(a1).IsString() ? std::string(t.V(a1).str()) : t.T(a1); }
}  // namespace

TEST_CASE("Data parity H1: sorting a filtered list leaves the hidden rows where they are") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* names[] = {"e", "d", "c", "b", "a", "f"};
  const char* tags[] = {"x", "y", "x", "y", "x", "y"};
  t.Set("A1", "Name");
  t.Set("B1", "Tag");
  for (int i = 0; i < 6; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), names[i]);
    t.Set(("B" + std::to_string(i + 2)).c_str(), tags[i]);
  }
  REQUIRE(t.d.SetAutoFilter(sh, R("A1:B7")));
  model::FilterColumn fc;
  fc.field = 1;
  fc.kind = 1;
  fc.values = {"x"};
  fc.blanks = false;
  t.d.SetFilterColumn(sh, fc);
  REQUIRE(t.d.S(sh)->rows->IsHidden(2));
  t.d.Sort(sh, R("A1:B7"), {engine::SortKey{0, false}}, true);
  // visible rows 2, 4, 6: a, c, e (all x); hidden rows 3, 5, 7 keep d, b, f (y)
  CHECK(S(t, "A2") == "a");
  CHECK(S(t, "A4") == "c");
  CHECK(S(t, "A6") == "e");
  CHECK(S(t, "B2") == "x");
  CHECK(S(t, "B6") == "x");
  CHECK(S(t, "A3") == "d");
  CHECK(S(t, "A5") == "b");
  CHECK(S(t, "A7") == "f");
  CHECK(S(t, "B3") == "y");
  // everything sorted when asked (visible_only = false)
  t.d.ShowAllData(sh);
  t.d.Sort(sh, R("A1:B7"), {engine::SortKey{0, false}}, true, false);
  CHECK(S(t, "A2") == "a");
  CHECK(S(t, "A7") == "f");
}

TEST_CASE("Data parity H2: Remove Duplicates in a table deletes table rows; the totals row follows") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "Name");
  t.Set("B1", "City");
  t.Set("A2", "d");
  t.Set("B2", "x");
  t.Set("A3", "d");
  t.Set("B3", "x");
  t.Set("A4", "c");
  t.Set("B4", "x");
  t.Set("A5", "a");
  t.Set("B5", "y");
  const uint32_t id = t.d.AddTable(sh, R("A1:B5"), true, "");
  REQUIRE(id);
  REQUIRE(t.d.UpdateTable(sh, id, [](model::Table& x) { x.totals = true; }));
  REQUIRE(t.d.TableById(sh, id)->range == R("A1:B6"));
  const Rect body = t.d.TableById(sh, id)->Data();
  CHECK(t.d.RemoveDuplicates(sh, body, {}, false) == 1);
  const model::Table* tb = t.d.TableById(sh, id);
  REQUIRE(tb);
  CHECK(tb->range == R("A1:B5"));
  CHECK(tb->totals);
  CHECK(S(t, "A2") == "d");
  CHECK(S(t, "A3") == "c");
  CHECK(S(t, "A4") == "a");
  CHECK(t.d.S(sh)->GetFormula(4, 1) != 0);  // the totals row's SUBTOTAL moved up to row 5
}

TEST_CASE("Data parity H4: validation formulas see the value being entered") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "5");
  model::DvRule rule;
  rule.type = model::DvType::Custom;
  rule.formula1 = "=ISNUMBER(A1)";
  rule.ranges = {R("A1")};
  std::string msg;
  CHECK_FALSE(engine::ValidateInput(t.d.wb(), sh, 0, 0, "abc", rule, &msg));
  t.Set("A1", "abc");
  CHECK(engine::ValidateInput(t.d.wb(), sh, 0, 0, "7", rule, &msg));
  // no duplicates in a column: COUNTIF sees the new value in its range
  t.Set("B1", "x");
  t.Set("B2", "y");
  model::DvRule uniq;
  uniq.type = model::DvType::Custom;
  uniq.formula1 = "=COUNTIF($B$1:$B$20,B1)=1";
  uniq.ranges = {R("B1:B20")};
  CHECK_FALSE(engine::ValidateInput(t.d.wb(), sh, 2, 1, "x", uniq, &msg));
  CHECK(engine::ValidateInput(t.d.wb(), sh, 2, 1, "z", uniq, &msg));
  CHECK(engine::ValidateInput(t.d.wb(), sh, 0, 1, "x", uniq, &msg));  // B1 itself: replacing x by x is no duplicate
  // a limit that refers to the cell itself
  model::DvRule lim;
  lim.type = model::DvType::Decimal;
  lim.op = model::CmpOp::Greater;
  lim.formula1 = "=C1-1";
  lim.ranges = {R("C1")};
  t.Set("C1", "100");
  CHECK(engine::ValidateInput(t.d.wb(), sh, 0, 2, "5", lim, &msg));  // 5 > 5-1
}

TEST_CASE("Data parity M6: text length counts characters, not bytes") {
  yctest::Doc t;
  model::DvRule rule;
  rule.type = model::DvType::TextLength;
  rule.op = model::CmpOp::LessEqual;
  rule.formula1 = "4";
  rule.ranges = {R("B1")};
  std::string msg;
  CHECK(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 1, "\xE5\x8F\xB0\xE7\xA9\x8D\xE9\x9B\xBB", rule, &msg));  // three CJK characters
  CHECK_FALSE(engine::ValidateInput(t.d.wb(), t.sheet(), 0, 1, "abcde", rule, &msg));
}

TEST_CASE("Data parity H6: filter criteria read dates, thousands and percents like a cell entry") {
  using engine::MatchCriterion;
  yctest::Doc t;
  t.Set("A1", "2023/10/1");
  t.Set("A2", "2024/1/20");
  t.Set("A3", "2024/1/10");
  const Value d1 = t.V("A1"), d2 = t.V("A2"), d3 = t.V("A3");
  REQUIRE(d2.IsNumeric());
  CHECK_FALSE(MatchCriterion(">1/15/2024", d1, "10/1/2023"));
  CHECK(MatchCriterion(">1/15/2024", d2, "1/20/2024"));
  CHECK_FALSE(MatchCriterion(">1/15/2024", d3, "1/10/2024"));
  CHECK(MatchCriterion(">=2024/1/10", d3, "2024/1/10"));
  CHECK(MatchCriterion(">1,000", Value::Number(1500), "1,500"));
  CHECK_FALSE(MatchCriterion(">1,000", Value::Number(500), "500"));
  CHECK(MatchCriterion("<50%", Value::Number(0.25), "25%"));
  // text keeps its text comparison
  CHECK(MatchCriterion("=a*", Value::String("apple"), "apple"));
}

TEST_CASE("Data parity M1 / M5: other columns' criteria and rows typed below the list") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "Fruit");
  t.Set("B1", "Color");
  t.Set("A2", "Apple");
  t.Set("B2", "Red");
  t.Set("A3", "Pear");
  t.Set("B3", "Green");
  t.Set("A4", "Apple");
  t.Set("B4", "Yellow");
  REQUIRE(t.d.SetAutoFilter(sh, R("A1:B4")));
  model::FilterColumn fc;
  fc.field = 0;
  fc.kind = 1;
  fc.values = {"Apple"};
  fc.blanks = false;
  t.d.SetFilterColumn(sh, fc);
  const model::Sheet& s = *t.d.S(sh);
  const engine::FilterTester test(t.d.wb(), s, *s.autofilter, s.autofilter->range);
  CHECK(test.Passes(1, 1));        // Red: Apple
  CHECK_FALSE(test.Passes(2, 1));  // Green: a pear, hidden by the Fruit column
  CHECK(test.Passes(2, 0));        // ...but listed in the Fruit column's own menu
  // a row typed below the list belongs to it
  t.Set("A5", "Pear");
  t.Set("B5", "Brown");
  CHECK(engine::AutoFilterExtent(*t.d.S(sh), t.d.S(sh)->autofilter->range) == R("A1:B5"));
  t.d.ApplyAutoFilter(sh);
  CHECK(t.d.S(sh)->autofilter->range == R("A1:B5"));
  CHECK(t.d.S(sh)->rows->IsHidden(4));
}

TEST_CASE("Data parity M3: the list's sort is kept, saved and sorted again by Reapply") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "k");
  t.Set("B1", "v");
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), std::string(1, static_cast<char>('a' + i)));
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
  }
  REQUIRE(t.d.SetAutoFilter(sh, R("A1:B5")));
  t.d.Sort(sh, R("A2:B5"), {engine::SortKey{1, true}}, false);
  t.d.SetFilterSort(sh, {model::SortLevel{1, true}});
  CHECK(t.N("B2") == 4);
  t.Set("B4", "10");
  t.d.ApplyAutoFilter(sh, true);
  CHECK(t.N("B2") == 10);
  // saved: key 6 holds every level, key 5 the first one for older versions
  const std::string bytes = yc::io::SaveYcgToBytes(t.d.wb(), {});
  auto res = yc::io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  const auto& af = *res.wb.sheets[0]->autofilter;
  REQUIRE(af.sort_levels.size() == 1);
  CHECK(af.sort_levels[0].col == 1);
  CHECK(af.sort_levels[0].desc);
}

TEST_CASE("Data parity M10: outline settings are saved; Auto Outline groups what SUM adds up") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "3");
  t.Set("A4", "=SUM(A1:A3)");
  t.Set("C5", "4");
  t.Set("D5", "5");
  t.Set("E5", "=SUBTOTAL(9,C5:D5)");
  CHECK(t.d.AutoOutline(sh, R("A1:E5")) == 2);
  CHECK(t.d.S(sh)->rows->Level(0) == 1);
  CHECK(t.d.S(sh)->rows->Level(2) == 1);
  CHECK(t.d.S(sh)->rows->Level(3) == 0);
  CHECK(t.d.S(sh)->cols->Level(2) == 1);
  CHECK(t.d.S(sh)->cols->Level(3) == 1);
  CHECK(t.d.S(sh)->cols->Level(4) == 0);
  t.d.SetOutlineSettings(sh, false, false, true);
  const std::string bytes = yc::io::SaveYcgToBytes(t.d.wb(), {});
  auto res = yc::io::LoadYcgFromBytes(bytes);
  REQUIRE(res.ok);
  CHECK_FALSE(res.wb.sheets[0]->outline_below);
  CHECK_FALSE(res.wb.sheets[0]->outline_right);
  CHECK(res.wb.sheets[0]->outline_styles);
  // a file without the key: the defaults
  yctest::Doc u;
  auto res2 = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(u.d.wb(), {}));
  REQUIRE(res2.ok);
  CHECK(res2.wb.sheets[0]->outline_below);
  CHECK(res2.wb.sheets[0]->outline_right);
}

TEST_CASE("Data parity L10: Remove Duplicates judges the value shown") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1,000");  // shown 1,000
  t.Set("A2", "1000");   // shown 1000: unique
  t.Set("A3", "abc");
  t.Set("A4", "ABC");    // the same text (case-insensitive)
  CHECK(t.d.RemoveDuplicates(sh, R("A1:A4"), {}, false) == 1);
  CHECK(t.N("A1") == 1000);
  CHECK(t.N("A2") == 1000);
  CHECK(S(t, "A3") == "abc");
  CHECK(t.V("A4").IsEmpty());
}

TEST_CASE("Data parity L9: Advanced Filter's copy keeps the cells' formats") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "Date");
  t.Set("A2", "2024/1/15");
  t.Set("A3", "2024/2/20");
  std::string err;
  REQUIRE(t.d.AdvancedFilter(sh, R("A1:A3"), 0, Rect{}, true, sh, R("G1"), false, &err));
  CHECK(t.T("G2") == t.T("A2"));
  CHECK(t.T("G3") == t.T("A3"));
}

TEST_CASE("Data parity M9: a Data Analysis dry run says where the results go and writes nothing") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (int i = 1; i <= 10; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  t.Set("E1", "keep");
  engine::Document::AnalysisRequest req;
  req.p.tool = engine::analysis::Tool::Descriptive;
  req.p.summary = true;
  req.in_sheet = sh;
  req.in = R("A1:A10");
  req.has_in = true;
  req.out_sheet = sh;
  req.out_r = 0;
  req.out_c = 4;
  req.dry_run = true;
  uint32_t os = 0;
  Rect orect{};
  std::string err;
  REQUIRE(t.d.RunAnalysis(req, &os, &orect, &err));
  CHECK(os == sh);
  CHECK(orect.r0 == 0);
  CHECK(orect.c0 == 4);
  CHECK(orect.r1 > 0);
  CHECK(S(t, "E1") == "keep");
}

TEST_CASE("Data parity M4: every table has its own AutoFilter") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* a[] = {"k", "x", "y", "x", "y"};
  const char* b[] = {"n", "1", "2", "3", "4"};
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    t.Set(("B" + std::to_string(i + 1)).c_str(), b[i]);
    t.Set(("D" + std::to_string(i + 1)).c_str(), i ? std::to_string(10 * i) : std::string("v"));
    t.Set(("E" + std::to_string(i + 1)).c_str(), i ? std::string(1, static_cast<char>('a' + i)) : std::string("w"));
  }
  const uint32_t t1 = t.d.AddTable(sh, R("A1:B5"), true, "");
  const uint32_t t2 = t.d.AddTable(sh, R("D1:E5"), true, "");
  REQUIRE(t1);
  REQUIRE(t2);
  const model::Sheet* s = t.d.S(sh);
  CHECK(engine::TableOwnsSheetFilter(*s, *t.d.TableById(sh, t1)));
  CHECK(engine::TableHasOwnFilter(*s, *t.d.TableById(sh, t2)));  // the second table has buttons of its own
  // the first table on A = x (hides rows 3 and 5), the second on D > 25 (hides rows 2 and 3)
  model::FilterColumn f1;
  f1.field = 0;
  f1.kind = 1;
  f1.values = {"x"};
  f1.blanks = false;
  t.d.SetFilterColumn(sh, f1);
  model::FilterColumn f2;
  f2.field = 0;
  f2.kind = 2;
  f2.crit1 = ">25";
  t.d.SetTableFilterColumn(sh, t2, f2);
  s = t.d.S(sh);
  CHECK_FALSE(s->rows->IsHidden(3));  // row 4: x and 30
  CHECK(s->rows->IsHidden(1));        // row 2: x but 10
  CHECK(s->rows->IsHidden(2));        // row 3: y and 20
  CHECK(s->rows->IsHidden(4));        // row 5: y
  // clearing the sheet's filter keeps the table's
  t.d.ShowAllData(sh);
  s = t.d.S(sh);
  CHECK(s->rows->IsHidden(1));
  CHECK_FALSE(s->rows->IsHidden(4));
  // saved with the table (its record's 7th element)
  auto res = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const model::Table* lt = nullptr;
  for (const auto& x : *res.wb.sheets[0]->tables) {
    if (x.id == t2) lt = &x;
  }
  REQUIRE(lt);
  REQUIRE(lt->filter.columns.size() == 1);
  CHECK(lt->filter.columns[0].crit1 == ">25");
  CHECK(lt->filter.hidden.count(1));
  // clearing the table's filter shows its rows
  t.d.ClearTableFilter(sh, t2);
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(1));
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(2));
}

TEST_CASE("Data parity: a case-sensitive sort weighs case before hyphens and apostrophes (measured in Excel 16)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* in[] = {"AB", "a'b", "Ab", "a-b", "ab-", "cant", "can't", "B", "b"};
  for (int i = 0; i < 9; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), in[i]);
  t.d.Sort(sh, R("A1:A9"), {engine::SortKey{0, false}}, false, true, true);
  // Excel 16, Case sensitive (measured): ab-, a'b and a-b (lowercase) before Ab before AB; cant before can't
  const char* want[] = {"ab-", "a'b", "a-b", "Ab", "AB", "b", "B", "cant", "can't"};
  for (int i = 0; i < 9; ++i) {
    INFO(i);
    CHECK(S(t, ("A" + std::to_string(i + 1)).c_str()) == want[i]);
  }
  t.d.Sort(sh, R("A1:A9"), {engine::SortKey{0, true}}, false, true, true);
  for (int i = 0; i < 9; ++i) {
    INFO(i);
    CHECK(S(t, ("A" + std::to_string(9 - i)).c_str()) == want[i]);
  }
}

TEST_CASE("Data parity: deleting every data row of a table keeps one, emptied (measured in Excel 16)") {
  // A: outside, B:C a table (header row 3, data rows 4:5), D formulas outside, rows 6:8 other data
  auto fill = [](yctest::Doc& t) {
    const char* rows[][4] = {{"x1", "top", "", "y1"}, {"x2", "", "", "y2"}, {"x3", "Qty", "Px", "y3"}, {"x4", "1", "10", "=B4*2"},
                             {"x5", "2", "20", "=B5*2"}, {"x6", "o6", "p6", "y6"}, {"x7", "o7", "p7", "y7"}, {"x8", "o8", "p8", "y8"}};
    for (int r = 0; r < 8; ++r) {
      for (int c = 0; c < 4; ++c) {
        if (*rows[r][c]) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
      }
    }
    std::string err;
    REQUIRE(t.d.AddTable(t.sheet(), R("B3:C5"), true, "TableStyleMedium2", &err));
  };
  auto row = [](yctest::Doc& t, int r) {
    std::string out;
    for (int c = 0; c < 4; ++c) out += (c ? "|" : "") + t.F((std::string(1, static_cast<char>('A' + c)) + std::to_string(r)).c_str());
    return out;
  };
  {  // Rows("4:5").Delete: row 4 stays, its table cells emptied; the formula beside it keeps its cell
    yctest::Doc t;
    fill(t);
    t.d.DeleteRows(t.sheet(), 3, 2);
    CHECK(row(t, 4) == "x4|||=B4*2");
    CHECK(row(t, 5) == "x6|o6|p6|y6");
    REQUIRE(t.d.S(t.sheet())->tables->size() == 1);
    CHECK(t.d.S(t.sheet())->tables->front().range == R("B3:C4"));
  }
  {  // Rows("4:7").Delete: row 4 stays, rows 5:7 go
    yctest::Doc t;
    fill(t);
    t.d.DeleteRows(t.sheet(), 3, 4);
    CHECK(row(t, 4) == "x4|||=B4*2");
    CHECK(row(t, 5) == "x8|o8|p8|y8");
    CHECK(t.d.S(t.sheet())->tables->front().range == R("B3:C4"));
  }
  {  // Range("B4:C5").Delete xlShiftUp: B4:C4 emptied, B5:C5 deleted and the cells below move up
    yctest::Doc t;
    fill(t);
    t.d.DeleteCells(t.sheet(), R("B4:C5"), true);
    CHECK(row(t, 4) == "x4|||=B4*2");
    CHECK(row(t, 5) == "x5|o6|p6|=#REF!*2");
    CHECK(t.d.S(t.sheet())->tables->front().range == R("B3:C4"));
  }
  {  // the whole table, header too: it goes
    yctest::Doc t;
    fill(t);
    t.d.DeleteRows(t.sheet(), 2, 3);
    CHECK(t.d.S(t.sheet())->tables->empty());
    CHECK(row(t, 3) == "x6|o6|p6|y6");
  }
}

TEST_CASE("Data parity: Remove Duplicates takes TRUE and the text true as one, 1 and the text 1 as two (measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* in[] = {"TRUE", "'TRUE", "FALSE", "'false", "1", "'1", "#N/A", "'#N/A", "x"};
  for (int i = 0; i < 9; ++i) t.Set(("A" + std::to_string(i + 1)).c_str(), in[i]);
  CHECK(t.d.RemoveDuplicates(sh, R("A1:A9"), {0}, false) == 2);
  // Excel 16: TRUE, FALSE, 1, 1 (text), #N/A, #N/A (text), x
  CHECK(t.V("A1").IsBool());
  CHECK(t.V("A2").IsBool());
  CHECK(t.V("A3").IsNumber());
  CHECK(t.V("A4").IsString());
  CHECK(t.V("A5").IsError());
  CHECK(t.V("A6").IsString());
  CHECK(S(t, "A7") == "x");
  CHECK(t.V("A8").IsEmpty());
}

TEST_CASE("AutoFilter custom criteria as Excel 16 applies them (measured: 20 kinds of cells, 29 criteria)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* cells[] = {"5", "'5", "05", "15", "1,000", "abc", "TRUE", "=NA()", "", "a5", "2024/1/5", "=\"\"", "5.5", "-5", "1000", "\"5\"", "x", "'TRUE", "50%", "0.5"};
  t.Set("A1", "Head");
  for (int i = 0; i < 20; ++i) {
    if (*cells[i]) t.Set(("A" + std::to_string(i + 2)).c_str(), cells[i]);
  }
  // each criterion and the rows Excel left shown (1) and hid (0), A2 first
  const std::pair<const char*, const char*> cases[] = {
      {"=5", "11100000000000000000"},
      {"<>5", "01011111111111111111"},
      {"=05", "00000000000000000000"},
      {"5", "11100000000000000000"},
      {"=1,000", "00001000000000000000"},
      {"=1000", "00000000000000100000"},
      {"<>1000", "11110111111111011111"},
      {">=5", "10111000001010100000"},
      {"<5", "00000000000001000011"},
      {">a", "00000100010000001100"},
      {"<a", "01000000000100010000"},
      {"=TRUE", "00000010000000000100"},
      {"<>TRUE", "11111101111111111111"},
      {"=#N/A", "00000001000000000000"},
      {"<>abc", "11111011111111111111"},
      {"=abc", "00000100000000000000"},
      {"*", "01000100010100011100"},
      {"<>*", "10111011101011100011"},
      {"=*5*", "01000000010000010000"},
      {"<>*5*", "10111111101111101111"},
      {"=0.5", "00000000000000000001"},
      {"=50%", "00000000000000000010"},
      {">=2024/1/1", "00000000001000000000"},
      {"=2024/1/5", "00000000001000000000"},
      {"=1/5/2024", "00000000000000000000"},
      {"=5*", "01000000000000000000"},
      {"?", "01000000000000001000"},
      {"=", "00000000100100000000"},
      {"<>", "11111111011011111111"},
  };
  REQUIRE(t.d.SetAutoFilter(sh, R("A1:A21")));
  for (const auto& k : cases) {
    INFO(k.first);
    model::FilterColumn fc;
    fc.field = 0;
    fc.kind = 2;
    fc.crit1 = k.first;
    t.d.SetFilterColumn(sh, fc);
    std::string shown;
    for (uint32_t r = 1; r <= 20; ++r) shown += t.d.S(sh)->rows->IsHidden(r) ? '0' : '1';
    CHECK(shown == k.second);
  }
}

TEST_CASE("An AutoFilter set outside a table whose buttons use the sheet's keeps the table's criteria, as in Excel") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* a[] = {"k", "x", "y", "x"};
  for (int i = 0; i < 4; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), a[i]);
    t.Set(("D" + std::to_string(i + 6)).c_str(), i ? std::to_string(i) : std::string("v"));
  }
  const uint32_t t1 = t.d.AddTable(sh, R("A1:A4"), true, "");
  REQUIRE(t1);
  REQUIRE(engine::TableOwnsSheetFilter(*t.d.S(sh), *t.d.TableById(sh, t1)));
  model::FilterColumn f1;
  f1.field = 0;
  f1.kind = 1;
  f1.values = {"x"};
  t.d.SetFilterColumn(sh, f1);
  REQUIRE(t.d.S(sh)->rows->IsHidden(2));  // row 3: y
  // Ctrl+Shift+L on D6:D9: the list gets the sheet's AutoFilter, the table keeps its buttons and its filter
  REQUIRE(t.d.SetAutoFilter(sh, R("D6:D9")));
  const model::Sheet* s = t.d.S(sh);
  CHECK(s->autofilter->on);
  CHECK(s->autofilter->range == R("D6:D9"));
  const model::Table* tb = t.d.TableById(sh, t1);
  REQUIRE(tb);
  CHECK(engine::TableHasOwnFilter(*s, *tb));
  REQUIRE(tb->filter.columns.size() == 1);
  CHECK(s->rows->IsHidden(2));
  // the table's filter still clears by itself
  t.d.ClearTableFilter(sh, t1);
  CHECK_FALSE(t.d.S(sh)->rows->IsHidden(2));
  CHECK(t.d.S(sh)->autofilter->on);
}

TEST_CASE("One copied cell pasted over a filtered list fills the rows it shows; a block fills every row (Excel 16, measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const char* k[] = {"k", "a", "b", "a", "b"};
  for (int i = 0; i < 5; ++i) {
    t.Set(("A" + std::to_string(i + 1)).c_str(), k[i]);
    t.Set(("B" + std::to_string(i + 1)).c_str(), i ? std::to_string(i) : std::string("v"));
  }
  t.Set("D1", "z");
  t.Set("D2", "q");
  REQUIRE(t.d.SetAutoFilter(sh, R("A1:B5")));
  model::FilterColumn f;
  f.field = 0;
  f.kind = 1;
  f.values = {"a"};
  t.d.SetFilterColumn(sh, f);
  REQUIRE(t.d.S(sh)->rows->IsHidden(2));
  auto col_b = [&] { return t.T("B2") + t.T("B3") + t.T("B4") + t.T("B5"); };
  // one cell over B2:B5 (Ctrl+V, Worksheet.Paste, Copy Destination): B2 and B4 only
  t.d.PasteRepeated(sh, 1, 1, 4, 1, t.d.Copy(sh, R("D1")));
  CHECK(col_b() == "z2z4");
  // two cells over B2:B5: every row, hidden or not
  for (int i = 2; i <= 5; ++i) t.Set(("B" + std::to_string(i)).c_str(), std::to_string(i - 1));
  t.d.PasteRepeated(sh, 1, 1, 2, 1, t.d.Copy(sh, R("D1:D2")));
  CHECK(col_b() == "zqzq");
  // without a filter one cell fills every row
  t.d.ClearAutoFilter(sh);
  t.d.PasteRepeated(sh, 1, 1, 4, 1, t.d.Copy(sh, R("D2")));
  CHECK(col_b() == "qqqq");
}

TEST_CASE("A table's own filter: clearing and Fill Down leave the rows it hides, as the sheet's AutoFilter does") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  // the first table (J1:K3) owns the sheet's AutoFilter buttons; the second (A1:C6) filters with its own
  t.Set("J1", "p");
  t.Set("K1", "q");
  t.Set("J2", "1");
  t.Set("K2", "2");
  REQUIRE(t.d.AddTable(sh, Rect{0, 9, 1, 10}, true, "TableStyleMedium2"));
  const char* rows[][3] = {{"k", "v", "n"}, {"a", "1", "x"}, {"b", "2", "y"}, {"a", "3", "z"}, {"b", "4", "w"}};
  for (int r = 0; r < 5; ++r) {
    for (int c = 0; c < 3; ++c) t.Set((std::string(1, static_cast<char>('A' + c)) + std::to_string(r + 1)).c_str(), rows[r][c]);
  }
  const uint32_t id = t.d.AddTable(sh, Rect{0, 0, 4, 2}, true, "TableStyleMedium2");
  REQUIRE(id);
  yc::model::FilterColumn f;
  f.field = 0;
  f.kind = 1;
  f.values = {"a"};
  f.blanks = false;
  t.d.SetTableFilterColumn(sh, id, f);
  REQUIRE(t.d.S(sh)->rows->IsHidden(2));
  const auto runs = yc::engine::FilterShownRuns(*t.d.S(sh), {Rect{1, 2, 4, 2}});  // C2:C5 shows C2 and C4
  REQUIRE(runs.size() == 2);
  CHECK(runs[0] == Rect{1, 2, 1, 2});
  CHECK(runs[1] == Rect{3, 2, 3, 2});
  t.Set("C2", "top");
  CHECK(t.d.FillDownShown(sh, Rect{1, 2, 4, 2}));
  CHECK(t.T("C3") == "y");  // hidden: kept
  CHECK(t.T("C4") == "top");
  CHECK(t.T("C5") == "w");
  // the range's first row hidden: it is still the source (Excel 16, measured 2026-11-28 through excel_diff)
  t.Set("C3", "hidden first");
  CHECK(t.d.FillDownShown(sh, Rect{2, 2, 4, 2}));
  CHECK(t.T("C4") == "hidden first");
}
