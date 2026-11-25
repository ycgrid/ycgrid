// SPDX-License-Identifier: Apache-2.0
// Round 3 (formulas): the engine side of the Formulas tab working as in Excel:
// Calculate Sheet, Automatic except for data tables, the error checking rules, Evaluate Formula's Step In, the
// shifts the Watch Window follows, tracing to other sheets
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/error_check.h"

using yc::core::CellPos;
using yc::core::Rect;
using namespace yc::engine;

namespace {
CellPos P(const char* a1) { return yctest::Doc::P(a1); }
}  // namespace

TEST_CASE("Calculate Sheet calculates only that sheet (manual calculation)") {
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Sheet2");
  t.Set("A1", "1");
  t.Set("B1", "=A1");
  t.Set("B1", "=Sheet1!A1", s2);
  t.Set("C1", "=B1*10", s2);
  REQUIRE(t.N("B1") == 1);
  REQUIRE(t.N("B1", s2) == 1);
  t.d.auto_calc = false;
  t.d.SetInput(s1, 0, 0, "2");
  t.d.CalcEntered();
  CHECK(t.N("B1") == 1);  // waiting for F9
  t.d.RecalcSheet(s1);
  CHECK(t.N("B1") == 2);
  CHECK(t.N("B1", s2) == 1);  // the other sheet keeps waiting
  CHECK(t.d.has_pending());
  t.d.RecalcSheet(s2);
  CHECK(t.N("B1", s2) == 2);
  CHECK(t.N("C1", s2) == 20);
}

TEST_CASE("Automatic except for data tables leaves the data tables to F9") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B1", "0.1");
  t.Set("C3", "1");
  t.Set("C4", "2");
  t.Set("D2", "=B1*10+E1");
  const CellPos col_in{0, 1};
  std::string err;
  REQUIRE(t.d.AddDataTable(sh, Rect{1, 2, 3, 3}, nullptr, &col_in, &err));
  CHECK(t.N("D3") == 10);
  t.d.auto_data_tables = false;
  t.Set("E1", "5");  // the table's formula reads E1
  CHECK(t.N("D2") == 6);   // the cells are calculated
  CHECK(t.N("D3") == 10);  // the table is not
  t.d.CalculateNow();      // F9
  CHECK(t.N("D3") == 15);
  CHECK(t.N("D4") == 25);
  t.d.auto_data_tables = true;
  t.Set("E1", "0");
  CHECK(t.N("D3") == 10);  // Automatic again: the table follows
}

TEST_CASE("error checking rules: text dates, omitted cells, unlocked formulas, empty cells, table columns") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  ErrorCheckOptions o;
  o.empty_cell_reference = true;
  // a text date with a two-digit year
  t.d.SetValue(sh, 0, 0, yc::core::Value::String("12/31/99"));
  t.d.SetValue(sh, 1, 0, yc::core::Value::String("12/31/1999"));
  const auto& wb = t.d.wb();
  CHECK(ErrorCheckRule(wb, *t.d.S(sh), 0, 0, o) == kErrTextDate);
  CHECK(ErrorCheckRule(wb, *t.d.S(sh), 1, 0, o) == kErrNone);
  // a formula whose range stops next to more numbers
  t.Set("C1", "1");
  t.Set("C2", "2");
  t.Set("C3", "3");
  t.Set("C4", "4");
  t.Set("C6", "=SUM(C1:C3)");
  CHECK(ErrorCheckRule(wb, *t.d.S(sh), 5, 2, o) == kErrOmittedCells);
  CHECK(OmittedCellsFormulaText(t.d, sh, 5, 2) == "=SUM(C1:C4)");
  t.Set("C7", "=SUM(C1:C4)");  // takes them all: no error
  CHECK(ErrorCheckRule(wb, *t.d.S(sh), 6, 2, o) == kErrNone);
  // a formula in an unlocked cell
  t.Set("E1", "=1+1");
  t.d.ModifyStyle(sh, Rect::Cell(0, 4), [](yc::model::Style& st) { st.locked = false; });
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 4, o) == kErrUnlockedFormula);
  o.unlocked_formula = false;
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 4, o) == kErrNone);
  // a formula that reads an empty cell (off by default, as in Excel)
  t.Set("G1", "=H1*2");
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 6, o) == kErrEmptyCellReference);
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 6, ErrorCheckOptions{}) == kErrNone);
  // a calculated column of a table with one cell unlike the others
  t.Set("J1", "Qty");
  t.Set("K1", "Twice");
  for (int r = 2; r <= 5; ++r) {
    t.Set(("J" + std::to_string(r)).c_str(), std::to_string(r));
    t.Set(("K" + std::to_string(r)).c_str(), "=J" + std::to_string(r) + "*2");
  }
  std::string err;
  t.d.AddTable(sh, Rect{0, 9, 4, 10}, true, "TableStyleMedium2", &err);
  t.Set("K3", "99");
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 2, 10, ErrorCheckOptions{}) == kErrInconsistentListFormula);
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 3, 10, ErrorCheckOptions{}) == kErrNone);
}

TEST_CASE("the Error Checking dialog walks the errors of the sheet in reading order") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B2", "=1/0");
  t.Set("A3", "'5");
  t.Set("D1", "=NA()");
  ErrorCheckOptions o;
  uint32_t r = 0, c = 0;
  REQUIRE(NextErrorCell(t.d.wb(), *t.d.S(sh), 0, 0, true, o, &r, &c));
  CHECK((r == 0 && c == 3));  // D1
  REQUIRE(NextErrorCell(t.d.wb(), *t.d.S(sh), r, c, true, o, &r, &c));
  CHECK((r == 1 && c == 1));  // B2
  REQUIRE(NextErrorCell(t.d.wb(), *t.d.S(sh), r, c, true, o, &r, &c));
  CHECK((r == 2 && c == 0));  // A3
  CHECK_FALSE(NextErrorCell(t.d.wb(), *t.d.S(sh), r, c, true, o, &r, &c));
  REQUIRE(NextErrorCell(t.d.wb(), *t.d.S(sh), 2, 0, false, o, &r, &c));  // Previous
  CHECK((r == 1 && c == 1));
  const auto lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);  // the titles are UI text
  CHECK(std::string(ErrorRuleTitle(kErrEvaluateToError, t.V("B2"))) == "Divide by Zero Error");
  CHECK(std::string(ErrorRuleTitle(kErrEvaluateToError, t.V("D1"))) == "Value Not Available Error");
  yc::i18n::SetCurrent(lang);
}

TEST_CASE("Evaluate Formula knows the cell a reference steps into") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("Data");
  t.Set("A1", "4", s2);
  t.Set("B1", "=Data!A1*2");
  t.Set("C1", "=B1+1");
  const auto steps = t.d.EvaluateSteps(t.sheet(), 0, 2);
  REQUIRE(steps.size() >= 2);
  CHECK(steps[0].in_sheet == static_cast<int32_t>(t.sheet()));  // B1 underlined first
  CHECK(steps[0].in_r == 0);
  CHECK(steps[0].in_c == 1);
  const auto inner = t.d.EvaluateSteps(t.sheet(), 0, 1);
  REQUIRE(!inner.empty());
  CHECK(inner[0].in_sheet == static_cast<int32_t>(s2));  // Data!A1
  // a constant: one step, its value
  const auto constant = t.d.EvaluateSteps(s2, 0, 0, true);
  REQUIRE(constant.size() == 1);
  CHECK(constant[0].text == "4");
  CHECK(t.d.EvaluateSteps(s2, 0, 0).empty());
}

TEST_CASE("inserted and deleted rows are logged for what follows cells by position") {
  yctest::Doc t;
  const uint64_t seq0 = t.d.shift_seq;
  t.d.InsertRows(t.sheet(), 0, 2);
  REQUIRE(t.d.shift_seq == seq0 + 1);
  const StructShift& x = t.d.shift_log.back();
  CHECK(x.rows);
  CHECK(x.at == 0);
  CHECK(x.delta == 2);
}

TEST_CASE("precedents and dependents on other sheets are found for the sheet icon arrow") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("Other");
  t.Set("A1", "1", s2);
  t.Set("B1", "=Other!A1+Other!B2:B3+A5");
  const auto prec = t.d.OtherSheetPrecedents(t.sheet(), 0, 1);
  REQUIRE(prec.size() == 2);
  CHECK(prec[0].first == static_cast<int32_t>(s2));
  CHECK(prec[0].second == Rect::Cell(0, 0));
  CHECK(prec[1].second == (Rect{1, 1, 2, 1}));
  const auto dep = t.d.OtherSheetDependents(s2, 0, 0);
  REQUIRE(dep.size() == 1);
  CHECK(dep[0].first == static_cast<int32_t>(t.sheet()));
  CHECK(dep[0].second == Rect::Cell(0, 1));
  CHECK(t.d.OtherSheetPrecedents(t.sheet(), 0, 0).empty());
}

TEST_CASE("Apply Names puts the names in place of the references they stand for") {
  yctest::Doc t;
  REQUIRE(t.d.AddName("Price", "Sheet1!$A$1"));
  REQUIRE(t.d.AddName("Qtys", "Sheet1!$B$1:$B$3"));
  t.Set("A1", "2");
  t.Set("C1", "=$A$1*SUM($B$1:$B$3)");
  t.Set("C2", "=A1+1");
  CHECK(t.d.ApplyNames(t.sheet(), Rect{0, 2, 1, 2}, {"Price", "Qtys"}, false) == 1);
  CHECK(t.F("C1") == "=Price*SUM(Qtys)");
  CHECK(t.F("C2") == "=A1+1");  // relative: only with Ignore Relative/Absolute
  CHECK(t.d.ApplyNames(t.sheet(), Rect{0, 2, 1, 2}, {"Price"}, true) == 1);
  CHECK(t.F("C2") == "=Price+1");
  CHECK(t.N("C2") == 3);
}

TEST_CASE("Misleading number formats: a formula shown unlike the cells it reads") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "$10");
  t.Set("A2", "$20");  // currency, as typed
  t.Set("B1", "=A1+A2");
  // the formula takes A1's currency format as it is entered (Excel); set back to General, it misleads
  CHECK(t.d.wb().styles->Get(t.d.S(sh)->GetStyle(0, 1)).numfmt != 0);
  t.d.ModifyStyle(sh, Rect::Cell(0, 1), [](yc::model::Style& st) { st.numfmt = 0; });
  ErrorCheckOptions o;
  // the result in General: a plain number for money is misleading (Excel's rule)
  uint32_t fmt = 0;
  int kind = 0;
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 1, o) == kErrMisleadingFormat);
  REQUIRE(MisleadingFormat(t.d.wb(), *t.d.S(sh), 0, 1, &fmt, &kind));
  CHECK(kind == kFmtCurrency);
  // Update Format to Currency: no error
  t.d.ModifyStyle(sh, Rect::Cell(0, 1), [fmt](yc::model::Style& st) { st.numfmt = fmt; });
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 1, o) == kErrNone);
  // a function that gives another kind of number (YEAR of a date) is left alone
  t.Set("C1", "2024-05-01");
  t.Set("D1", "=YEAR(C1)");
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 0, 3, o) == kErrNone);
  t.Set("D2", "=C1+1");  // takes C1's date format as it is entered; set back to General, a date shown as a number
  t.d.ModifyStyle(sh, Rect::Cell(1, 3), [](yc::model::Style& st) { st.numfmt = 0; });
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 1, 3, o) == kErrMisleadingFormat);
  o.misleading_format = false;
  CHECK(ErrorCheckRule(t.d.wb(), *t.d.S(sh), 1, 3, o) == kErrNone);
  CHECK(FormatKindOf("0.00%") == kFmtPercent);
  CHECK(FormatKindOf("_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)") == kFmtAccounting);
  CHECK(FormatKindOf("[$-409]h:mm AM/PM") == kFmtTime);
  CHECK(FormatKindOf("yyyy-mm-dd") == kFmtDate);
  CHECK(FormatKindOf("#,##0.00") == kFmtNumber);
}

TEST_CASE("Apply Names with row and column names writes their intersection") {
  yctest::Doc t;
  REQUIRE(t.d.AddName("Jan", "Sheet1!$B$1:$B$3"));    // a column
  REQUIRE(t.d.AddName("Sales", "Sheet1!$A$2:$C$2"));  // a row
  t.Set("B2", "5");
  t.Set("E5", "=B2*2");
  Document::ApplyNamesOptions o;
  CHECK(t.d.ApplyNames(t.sheet(), Rect::Cell(4, 4), {"Jan", "Sales"}, o) == 1);
  CHECK(t.F("E5") == "=Sales Jan*2");
  CHECK(t.N("E5") == 10);
  t.Set("E6", "=B2+1");
  o.row_first = false;
  CHECK(t.d.ApplyNames(t.sheet(), Rect::Cell(5, 4), {"Jan", "Sales"}, o) == 1);
  CHECK(t.F("E6") == "=Jan Sales+1");
  // in the same column as B2: the column name is left out (Omit column name if same column)
  t.Set("B7", "=B2*3");
  CHECK(t.d.ApplyNames(t.sheet(), Rect::Cell(6, 1), {"Jan", "Sales"}, o) == 1);
  CHECK(t.F("B7") == "=@Sales*3");
  CHECK(t.N("B7") == 15);
  // off: nothing to replace
  t.Set("E8", "=B2");
  o.use_row_column = false;
  CHECK(t.d.ApplyNames(t.sheet(), Rect::Cell(7, 4), {"Jan", "Sales"}, o) == 0);
}
