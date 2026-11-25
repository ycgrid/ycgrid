// SPDX-License-Identifier: Apache-2.0
// Members of Excel's object model YCGrid didn't have (docs/excel-object-model.md), at the engine: a sheet's
// calculation turned off, a calculation interrupted, the 1904 date system, precision as displayed, a full calculation
// every time, the calculation threads, =STREAM, the @udf functions' help, and what the file keeps of them. The
// expected values are Excel 16's, measured (the probes are in the commit that added them).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/parallel.h"
#include "engine/engine.h"
#include "formula/datetime.h"
#include "formula/func_help.h"
#include "formula/numfmt.h"
#include "io/ycg.h"
#include "test_helpers.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

using yc::core::Rect;
using Catch::Matchers::WithinAbs;

namespace {

void Format(yctest::Doc& t, const char* a1, const char* code) {
  const uint32_t nf = t.d.wb_mut().styles.mut().InternString(code);
  t.d.ModifyStyle(t.sheet(), yctest::Doc::Range(a1), [nf](yc::model::Style& s) { s.numfmt = nf; });
}

}  // namespace

TEST_CASE("Gaps: a sheet whose calculation is off calculates nothing until it is on again (Worksheet.EnableCalculation)") {
  yctest::Doc t;
  t.Set("B1", "5");
  t.Set("C1", "=B1*2");
  REQUIRE(t.N("C1") == 10);
  t.d.SetSheetCalculation(t.sheet(), false);
  CHECK_FALSE(t.d.SheetCalculation(t.sheet()));
  t.Set("B1", "7");
  CHECK(t.N("C1") == 10);  // not calculated (Excel 16: 10)
  t.Set("D1", "=1+1");
  CHECK(t.V("D1").IsNumber());
  CHECK(t.N("D1") == 0);  // a formula entered there shows 0
  t.d.CalculateNow();
  t.d.RecalcAll();
  t.d.RecalcSheet(t.sheet());
  CHECK(t.N("C1") == 10);  // F9, CalculateFull, Worksheet.Calculate: nothing
  t.d.SetSheetCalculation(t.sheet(), true);
  CHECK(t.N("C1") == 14);  // on again: the whole sheet is calculated
  CHECK(t.N("D1") == 2);
}

TEST_CASE("Gaps: an interrupted calculation keeps what it didn't reach pending and the next one goes on (CalculationInterruptKey)") {
  yctest::Doc t;
  std::atomic<bool> stop{false};
  t.d.interrupt = &stop;
  t.d.calc_threads = 1;
  const uint32_t sh = t.sheet();
  t.d.SetInput(sh, 0, 0, "1");
  for (uint32_t r = 1; r < 3000; ++r) t.d.SetInput(sh, r, 0, "=A" + std::to_string(r) + "+1");
  t.d.Recalc();
  REQUIRE(t.N("A3000") == 3000);
  t.d.SetInput(sh, 0, 0, "2");
  stop = true;  // the key pressed while it calculates
  t.d.Recalc();
  CHECK(t.d.CalcInterrupted());
  CHECK(t.d.has_pending());
  CHECK_FALSE(stop.load());  // the request is used up by the calculation it stopped
  t.d.Recalc();  // the next calculation finishes the work
  CHECK_FALSE(t.d.CalcInterrupted());
  CHECK_FALSE(t.d.has_pending());
  CHECK(t.N("A3000") == 3001);
}

TEST_CASE("Gaps: the 1904 date system shifts every date by 1462 days and keeps the cells' numbers (Workbook.Date1904)") {
  yctest::Doc t;
  t.Set("A1", "43831");
  Format(t, "A1", "yyyy-mm-dd");
  CHECK(t.T("A1") == "2020-01-01");
  t.d.SetDate1904(true);
  CHECK(t.d.wb().date1904);
  CHECK(t.N("A1") == 43831);         // the number stays
  CHECK(t.T("A1") == "2024-01-02");  // and shows a date four years and a day later (measured)
  CHECK(t.Eval("DATE(2020,1,1)") == "42369");
  CHECK(t.Eval("YEAR(43831)") == "2024");
  CHECK(t.Eval("WEEKDAY(0)") == "6");  // 1904-01-01 was a Friday
  CHECK(t.Eval("EDATE(0,1)") == "31");
  CHECK(t.Eval("DATEVALUE(\"2020-01-01\")") == "42369");
  CHECK(t.Eval("DATE(1903,12,31)") == "#NUM!");
  t.Set("B1", "2020/1/1");
  CHECK(t.N("B1") == 42369);  // typed, in the workbook's system
  t.Set("A2", "0");
  Format(t, "A2", "yyyy-mm-dd");
  CHECK(t.T("A2") == "1904-01-01");
  // saved with the workbook
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  const yc::model::Workbook& back = lr.wb;
  CHECK(back.date1904);
  t.d.SetDate1904(false);
  CHECK(t.T("A1") == "2020-01-01");
}

TEST_CASE("Gaps: precision as displayed rounds numbers to what their format shows, for good (Workbook.PrecisionAsDisplayed)") {
  yctest::Doc t;
  t.Set("A1", "1.23456");
  Format(t, "A1", "0.00");
  t.Set("A2", "2.71828");
  t.Set("A3", "=A1*3");
  Format(t, "A3", "0.0");
  t.Set("A5", "1.23456");
  Format(t, "A5", "0%");
  t.Set("A6", "45000.75");
  Format(t, "A6", "yyyy-mm-dd");
  t.Set("A7", "1234567.891");
  Format(t, "A7", "#,##0,\"K\"");
  t.Set("A8", "0.333333333");
  Format(t, "A8", "# ?/?");
  t.d.SetPrecisionAsDisplayed(true);
  t.d.Recalc();
  // Excel 16, measured
  CHECK(t.N("A1") == 1.23);
  CHECK(t.N("A2") == 2.71828);  // General keeps its digits
  CHECK_THAT(t.N("A3"), WithinAbs(3.7, 1e-12));
  CHECK(t.N("A5") == 1.23);  // 123%: two decimals more
  CHECK(t.N("A6") == 45000.75);  // a date keeps its time
  CHECK(t.N("A7") == 1235000);  // a trailing comma: thousands
  CHECK(t.N("A8") == 0.333333333);  // a fraction keeps its digits
  t.Set("A1", "9.87654");
  CHECK(t.N("A1") == 9.88);  // what is typed after
  CHECK_THAT(t.N("A3"), WithinAbs(29.6, 1e-12));  // and what is calculated
  t.Set("B1", "1.23456");
  CHECK(t.N("B1") == 1.23456);
  t.d.Begin("format");
  Format(t, "B1", "0.0");
  t.d.Commit();
  CHECK(t.N("B1") == 1.2);  // a format given after rounds too
  t.d.SetPrecisionAsDisplayed(false);
  CHECK(t.N("A1") == 9.88);  // turned off, the numbers stay rounded
  t.Set("C1", "1.23456");
  Format(t, "C1", "0.00");
  CHECK(t.N("C1") == 1.23456);
}

TEST_CASE("Gaps: RoundAsShown follows the format's decimals, percent, scaling and exponent") {
  double v = 0;
  CHECK(yc::formula::RoundAsShown(1.23456, "0.00", &v));
  CHECK(v == 1.23);
  CHECK(yc::formula::RoundAsShown(-1.25678, "0.00;(0.00)", &v));
  CHECK(v == -1.26);
  CHECK(yc::formula::RoundAsShown(1234567.891, "#,##0,,\"M\"", &v));
  CHECK(v == 1000000);
  CHECK(yc::formula::RoundAsShown(123456, "0.00E+00", &v));
  CHECK_THAT(v, WithinAbs(123000, 1e-6));
  CHECK_FALSE(yc::formula::RoundAsShown(1.23456, "General", &v));
  CHECK_FALSE(yc::formula::RoundAsShown(45000.75, "m/d/yyyy h:mm", &v));
  CHECK_FALSE(yc::formula::RoundAsShown(0.3333, "# ?/?", &v));
  CHECK_FALSE(yc::formula::RoundAsShown(5, "[>3]0.0;0", &v));
  // what the cell shows, half away from zero on 15 digits: -75.195 * 100 is -7519.4999... in binary (gaps_fuzz 61311)
  CHECK(yc::formula::RoundAsShown(-75.195, "#,##0.00", &v));
  CHECK(v == -75.2);
  CHECK(yc::formula::RoundAsShown(201.015, "0.00", &v));
  CHECK(v == 201.02);
  CHECK(yc::formula::RoundAsShown(0.12345, "0.0%", &v));
  CHECK(v == 0.123);
  CHECK(yc::formula::RoundAsShown(1234567, "#,##0.0,", &v));
  CHECK(v == 1234600);
}

TEST_CASE("Gaps: a number RoundAsShown rounds shows as the number did, and rounds to itself (every format, random values)") {
  // the class of the -75.195 finding: precision as displayed keeps exactly what the cell shows
  const char* codes[] = {"0", "0.0", "0.00", "0.000", "#,##0.00", "0%", "0.0%", "0.00%", "#,##0.0,", "#,##0,,", "0.00E+00",
                         "0.0E+0", "##0.0E+0", "$#,##0.00", "0.00;(0.00)", "0.000_);[Red](0.000)"};
  uint64_t h = 0x9E3779B97F4A7C15ull;
  auto next = [&] { return h = h * 6364136223846793005ull + 1442695040888963407ull, h >> 11; };
  for (int i = 0; i < 20000; ++i) {
    const double mag = std::pow(10.0, static_cast<double>(next() % 13) - 6.0);
    double x = static_cast<double>(next() % 2000001) / 1000.0 * mag;
    if (next() % 3 == 0) x = std::round(x * 1000.0) / 1000.0 + 0.0005;  // a half at the third decimal
    if (next() % 2) x = -x;
    const char* code = codes[next() % (sizeof(codes) / sizeof(*codes))];
    double r = 0;
    if (!yc::formula::RoundAsShown(x, code, &r)) continue;
    const std::string shown = yc::formula::FormatValue(yc::core::Value::Number(x), code).text;
    INFO(x << " in " << code << " shows " << shown << ", rounded to " << r);
    // a negative shown as zero ((0.00) by its own section) is 0 once rounded, and shows as 0 does
    if (r != 0) CHECK(yc::formula::FormatValue(yc::core::Value::Number(r), code).text == shown);
    double again = 0;
    REQUIRE(yc::formula::RoundAsShown(r, code, &again));
    CHECK(again == r);
  }
}

TEST_CASE("Gaps: a full calculation every time calculates every formula (Workbook.ForceFullCalculation)") {
  yctest::Doc t;
  t.d.calc_threads = 1;
  for (int i = 1; i <= 20; ++i) t.Set(("A" + std::to_string(i)).c_str(), "=ROW()");
  t.Set("B1", "1");
  CHECK(t.d.last_stats().evaluated == 0);  // nothing reads B1
  t.d.SetForceFullCalculation(true);
  t.Set("B1", "2");
  CHECK(t.d.last_stats().evaluated >= 20);
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  const yc::model::Workbook& back = lr.wb;
  CHECK(back.force_full_calc);
}

TEST_CASE("Gaps: the calculation threads follow MultiThreadedCalculation") {
  const size_t all = yc::core::PoolSize();
  yc::core::SetWorkerLimit(1);
  CHECK(yc::core::WorkerCount() == 1);
  yc::core::SetWorkerLimit(0);
  CHECK(yc::core::WorkerCount() == all);
  // a ParallelFor under a limit still does every chunk
  yc::core::SetWorkerLimit(2);
  std::atomic<int> sum{0};
  yc::core::ParallelFor(1000, 10, [&](size_t b, size_t e) { sum += static_cast<int>(e - b); });
  CHECK(sum == 1000);
  yc::core::SetWorkerLimit(0);
}

TEST_CASE("Gaps: =STREAM is the stream function and =RTD is no longer one") {
  yctest::Doc t;
  CHECK(t.Eval("RTD(\"X\",\"\",\"a\")") == "#NAME?");
  CHECK(t.Eval("STREAM(\"X\",\"a\")") != "#NAME?");
}

TEST_CASE("Gaps: the @udf functions' help reaches Insert Function and the argument hints (Application.MacroOptions)") {
  yc::formula::SetUserFuncHelp({{"pricebs", "spot, [vol]", "Black-Scholes price.", "", {{"spot", "the underlying's price"}}}});
  const yc::formula::FuncHelp* h = yc::formula::FindFuncHelp("PRICEBS");
  REQUIRE(h != nullptr);
  CHECK(std::string(h->args) == "spot, [vol]");
  CHECK(std::string(h->desc) == "Black-Scholes price.");
  CHECK(std::string(h->category) == "User Defined");
  CHECK(std::string(yc::formula::FuncArgHelp("PRICEBS", "spot")) == "the underlying's price");
  bool listed = false;
  for (const char* c : yc::formula::FuncCategories()) listed = listed || std::strcmp(c, "User Defined") == 0;
  CHECK(listed);
  yc::formula::SetUserFuncHelp({});
  CHECK(yc::formula::FindFuncHelp("PRICEBS") == nullptr);
  CHECK(h->name != nullptr);  // what was handed out stays valid
}

TEST_CASE("Gaps: what the file keeps of the new members (a report's grand total name, a chart axis' marks, a chart's shapes)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  yc::model::ChartObj c;
  c.cat_axis.mark_interval = 3;
  c.val_axis.unit_label = false;
  const uint32_t cid = t.d.AddChart(sh, c);
  yc::model::ShapeObj sp;
  sp.on_chart = cid;
  t.d.AddShape(sh, sp);
  {
    yc::model::PivotTable pt;
    pt.name = "PT1";
    pt.grand_name = "All";
    pt.refreshed_at = 45000.5;
    pt.refreshed_by = "trader";
    t.d.wb_mut().MutSheetById(sh)->pivots.mut().push_back(pt);
  }
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(t.d.wb(), yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  const yc::model::Workbook& back = lr.wb;
  const yc::model::Sheet& s = *back.sheets[0];
  REQUIRE(s.charts->size() == 1);
  CHECK((*s.charts)[0].cat_axis.mark_interval == 3);
  CHECK_FALSE((*s.charts)[0].val_axis.unit_label);
  REQUIRE(s.shapes->size() == 1);
  CHECK((*s.shapes)[0].on_chart == cid);
  REQUIRE(s.pivots->size() == 1);
  CHECK((*s.pivots)[0].grand_name == "All");
  CHECK((*s.pivots)[0].refreshed_at == 45000.5);
  CHECK((*s.pivots)[0].refreshed_by == "trader");
}

TEST_CASE("Gaps: a chart's shapes move with it and go when it is deleted (Chart.Shapes)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  yc::model::ChartObj c;
  c.left = 100;
  c.top = 50;
  const uint32_t cid = t.d.AddChart(sh, c);
  yc::model::ShapeObj on, off;
  on.on_chart = cid;
  on.left = 120;
  on.top = 60;
  off.left = 10;
  t.d.AddShape(sh, on);
  t.d.AddShape(sh, off);
  REQUIRE(t.d.UpdateChart(sh, cid, [](yc::model::ChartObj& x) { x.left += 30, x.top += 20; }));
  const auto& shapes = *t.d.S(sh)->shapes;
  REQUIRE(shapes.size() == 2);
  for (const auto& s : shapes) {
    if (s.on_chart == cid) {
      CHECK(s.left == 150);
      CHECK(s.top == 80);
    } else {
      CHECK(s.left == 10);
    }
  }
  REQUIRE(t.d.DeleteChart(sh, cid));
  REQUIRE(t.d.S(sh)->shapes->size() == 1);
  CHECK((*t.d.S(sh)->shapes)[0].on_chart == 0);
}

TEST_CASE("Gaps: the last consolidation onto a sheet is kept (Worksheet.ConsolidationFunction / Options / Sources)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "1");
  t.Set("A2", "2");
  std::string err;
  REQUIRE(t.d.Consolidate(sh, 5, 5, {{sh, Rect{0, 0, 1, 0}}}, 2, true, false, &err));
  const auto it = t.d.last_consolidation.find(sh);
  REQUIRE(it != t.d.last_consolidation.end());
  CHECK(it->second.func == 2);
  CHECK(it->second.top);
  CHECK_FALSE(it->second.left);
  REQUIRE(it->second.sources.size() == 1);
}

TEST_CASE("Gaps: in automatic calculation an interrupted calculation goes on by itself after what was queued") {
  yc::engine::Engine eng(yc::model::Workbook::NewDefault("Sheet"));
  eng.Start();
  eng.SetPublishInterval(0);
  const uint32_t sheet = eng.Current()->wb.sheets[0]->id;
  eng.PostUser("Typing", [sheet](yc::engine::Document& d) {
    d.calc_threads = 1;
    d.SetInput(sheet, 0, 0, "1");
    for (uint32_t r = 1; r < 3000; ++r) d.SetInput(sheet, r, 0, "=A" + std::to_string(r) + "+1");
  });
  eng.Sync();
  REQUIRE(eng.Current()->wb.sheets[0]->GetValue(2999, 0).AsDouble() == 3000);
  // a key interrupts the calculation the next edit starts (the request made while the engine works)
  eng.PostUser("Typing", [sheet](yc::engine::Document& d) {
    d.SetInput(sheet, 0, 0, "2");
    d.interrupt->store(true);
  });
  bool done = false;
  for (int i = 0; i < 200 && !done; ++i) {
    eng.Sync();
    done = eng.Current()->wb.sheets[0]->GetValue(2999, 0).AsDouble() == 3001;
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(done);  // no edit came after it, and the cells weren't left behind
  eng.Stop();
}

TEST_CASE("Gaps: a 1904 workbook opened in a 1900 session is calculated in its own date system (Engine::Replace)") {
  yc::formula::SetDate1904(false);
  yc::engine::Engine eng(yc::model::Workbook::NewDefault("Sheet"));
  eng.Start();
  // the 1904 workbook as a file gives it: its formulas calculated on open
  yc::model::Workbook wb = [] {
    yctest::Doc t;
    t.d.SetDate1904(true);
    t.Set("A1", "=DATE(2024,1,1)");
    yc::model::Workbook w = t.d.wb();
    yc::formula::SetDate1904(false);  // the session's own system, back
    return w;
  }();
  REQUIRE(wb.date1904);
  const std::string bytes = yc::io::SaveYcgToBytes(wb, yc::io::SaveOptions{});
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(bytes);
  REQUIRE(lr.ok);
  eng.Replace(std::move(lr.wb));
  eng.Sync();
  CHECK(eng.Current()->wb.sheets[0]->GetValue(0, 0).AsDouble() == 43830);  // 45292 in the 1900 system
  eng.Stop();
  yc::formula::SetDate1904(false);
}

TEST_CASE("Gaps: a copy of a sheet whose calculation is off calculates (Excel 16, measured; doc_fuzz --gaps 700139)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "5");
  t.Set("B1", "=A1*2");
  t.d.SetSheetCalculation(sh, false);
  t.Set("C1", "=A1+1");
  t.Set("D1", "=SUM(#REF!)");
  CHECK(t.N("C1") == 0);
  CHECK(t.N("D1") == 0);
  const uint32_t cp = t.d.CopySheet(sh);
  REQUIRE(cp);
  t.d.Recalc();
  CHECK(t.d.SheetCalculation(cp));
  CHECK(t.N("B1", cp) == 10);
  CHECK(t.N("C1", cp) == 6);
  CHECK(t.T("D1", cp) == "#REF!");
  CHECK(t.N("C1") == 0);  // the source keeps what it shows
}

TEST_CASE("Gaps: precision as displayed, off again, calculates the formulas at full precision and keeps the constants (Excel 16, measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t two = t.d.wb_mut().styles.mut().InternString("0.00");
  const uint32_t sci = t.d.wb_mut().styles.mut().InternString("0.00E+00");
  t.Set("A1", "=1/3");
  t.Set("B1", "0.333333");
  t.Set("C1", "=DATE(2024,1,31)");
  t.Set("D1", "=A1*3");
  t.Set("E1", "=B1*3");
  t.d.ModifyStyle(sh, Rect{0, 0, 0, 1}, [two](yc::model::Style& s) { s.numfmt = two; });
  t.d.ModifyStyle(sh, Rect{0, 2, 0, 2}, [sci](yc::model::Style& s) { s.numfmt = sci; });
  t.d.SetPrecisionAsDisplayed(true);
  t.d.Recalc();
  CHECK(t.N("A1") == 0.33);
  CHECK(t.N("C1") == 45300);
  CHECK_THAT(t.N("D1"), Catch::Matchers::WithinAbs(0.99, 1e-12));
  t.d.SetPrecisionAsDisplayed(false);
  t.d.Recalc();
  CHECK_THAT(t.N("A1"), Catch::Matchers::WithinAbs(1.0 / 3, 1e-15));
  CHECK(t.N("B1") == 0.33);  // a constant keeps what it was rounded to
  CHECK(t.N("C1") == 45322);
  CHECK(t.N("D1") == 1);
  CHECK_THAT(t.N("E1"), Catch::Matchers::WithinAbs(0.99, 1e-12));
}

TEST_CASE("Gaps: precision as displayed rounds every cell an array spills to its own format (Excel 16, measured; doc_fuzz --gaps 714889)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t two = t.d.wb_mut().styles.mut().InternString("0.00");
  const uint32_t thousands = t.d.wb_mut().styles.mut().InternString("#,##0.0,");
  t.d.ModifyStyle(sh, Rect{0, 0, 2, 0}, [two](yc::model::Style& s) { s.numfmt = two; });
  t.d.ModifyStyle(sh, Rect{0, 1, 2, 1}, [thousands](yc::model::Style& s) { s.numfmt = thousands; });
  t.d.SetPrecisionAsDisplayed(true);
  t.Set("A1", "=SEQUENCE(3)/3");
  t.Set("B1", "=SEQUENCE(3)");
  t.Set("D1", "=SUM(A1:A3)");
  t.Set("E1", "=SUM(B1:B3)");
  t.d.Recalc();
  CHECK(t.N("A1") == 0.33);
  CHECK(t.N("A2") == 0.67);
  CHECK(t.N("A3") == 1);
  CHECK(t.N("B2") == 0);
  CHECK(t.N("D1") == 2);
  CHECK(t.N("E1") == 0);
}

TEST_CASE("Gaps: an interrupted calculation, finished, finds the circular references one never interrupted does (doc_fuzz --gaps 763382)") {
  // A2's VLOOKUP reads its own cell (a cycle) and B8's reads A2 (it keeps its value, #N/A, as what reads a cycle does)
  std::string want;
  for (int64_t after : {-1, 0, 1, 2, 3, 4, 5, 8, 20}) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    t.Set("E3", "=SUM(A1:A3)");
    t.Set("B8", "=VLOOKUP(A1,A1:C9,2,FALSE)");
    t.d.Recalc();
    t.d.Begin("x");
    t.d.interrupt_after = after;
    t.d.SetInput(sh, 1, 0, "=VLOOKUP(A1,A1:C9,2,FALSE)");
    t.d.Commit();
    t.d.Recalc();
    for (int rounds = 0; t.d.CalcInterrupted() && rounds < 100; ++rounds) t.d.Recalc();
    t.d.interrupt_after = -1;
    std::string got;
    for (uint64_t k : t.d.circular_cells) got += yc::core::CellA1(yc::engine::KeyRow(k), yc::engine::KeyCol(k)) + " ";
    got += "A2=" + t.T("A2") + " B8=" + t.T("B8") + " E3=" + t.T("E3");
    if (after < 0) want = got;
    INFO("interrupted after " << after);
    CHECK(got == want);
  }
  CHECK(want == "A2 A2=0 B8=#N/A E3=0");
}
