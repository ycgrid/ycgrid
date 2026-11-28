// SPDX-License-Identifier: Apache-2.0
// Problems tests/fuzz/doc_fuzz.cpp found, each cut down to the steps that still show it (yc_doc_fuzz --replay FILE
// --minimize) and kept here so they stay fixed.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/content_dump.h"
#include "io/ycg.h"

#include <string>

namespace model = yc::model;
using yc::core::Rect;

namespace {

std::string Dump(const yctest::Doc& t) { return yc::io::ContentDump(t.d.wb()); }

// the members of each sheet that differ between two versions (the dump keeps a checksum of the sheet's chunk only)
std::string SheetDiff(const model::Workbook& a, const model::Workbook& b) {
  std::string out;
  for (size_t i = 0; i < a.sheets.size() && i < b.sheets.size(); ++i) {
    const model::Sheet& x = *a.sheets[i];
    const model::Sheet& y = *b.sheets[i];
    auto note = [&](bool same, const char* what) {
      if (!same) out += x.name + "." + what + " ";
    };
    if (!(*x.merges == *y.merges)) {
      out += x.name + ".merges (";
      for (const Rect& m : *x.merges) out += yc::core::RectA1(m) + " ";
      out += "vs ";
      for (const Rect& m : *y.merges) out += yc::core::RectA1(m) + " ";
      out += ") ";
    }
    note(*x.row_styles == *y.row_styles, "row_styles");
    note(*x.col_styles == *y.col_styles, "col_styles");
    note(*x.cond_formats == *y.cond_formats, "cond_formats");
    note(*x.validations == *y.validations, "validations");
    note(*x.autofilter == *y.autofilter, "autofilter");
    note(*x.spills == *y.spills, "spills");
    note(*x.tables == *y.tables, "tables");
    note(*x.notes == *y.notes, "notes");
    note(*x.links == *y.links, "links");
    note(*x.rich == *y.rich, "rich");
    note(*x.ignored_errors == *y.ignored_errors, "ignored_errors");
    note(*x.protection == *y.protection, "protection");
    note(*x.page_setup == *y.page_setup, "page_setup");
    note(x.has_used == y.has_used && x.used == y.used, "used");
    note(x.row_limit == y.row_limit, "row_limit");
  }
  return out;
}

// one user step, checked as doc_fuzz checks it: Undo gives back what was there, Redo what the step made
template <class Fn>
void CheckStep(yctest::Doc& t, Fn&& step) {
  const std::string before = Dump(t);
  const model::Workbook wb_before = t.d.wb();
  t.d.Begin("step");
  step();
  t.d.Commit();
  t.d.Recalc();
  const std::string after = Dump(t);
  const model::Workbook wb_after = t.d.wb();
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  {
    INFO("after Undo, these differ: " << SheetDiff(wb_before, t.d.wb()));
    CHECK(Dump(t) == before);
  }
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  INFO("after Redo, these differ: " << SheetDiff(wb_after, t.d.wb()));
  CHECK(Dump(t) == after);
}

}  // namespace

TEST_CASE("doc_fuzz seed 23: an array that spills over the cells it reads is a circular reference, the first time as after Redo") {
  // Rate is relative (no $): read from D3 it is E3:F3, where =Rate*2 spills
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  model::NameDef rate;
  rate.name = "Rate";
  rate.refers_to = "=Sheet1!$B$2";
  REQUIRE(t.d.DefineName(rate));
  t.d.ClearUndo();
  SECTION("each step undone and redone, as the fuzzer does") {
    CheckStep(t, [&] { t.d.InsertRows(data, 10, 2); });
    CheckStep(t, [&] { REQUIRE(t.d.SetArrayFormula(sh, Rect{10, 1, 11, 1}, "A1:B3*2")); });
    CheckStep(t, [&] {
      model::NameDef n = rate;
      n.refers_to = "=Sheet1!B1:C1";
      REQUIRE(t.d.DefineName(n));
    });
    CheckStep(t, [&] { t.d.SetInput(sh, 2, 3, "=Rate*2"); });
  }
  SECTION("without the array") {
    CheckStep(t, [&] {
      model::NameDef n = rate;
      n.refers_to = "=Sheet1!B1:C1";
      REQUIRE(t.d.DefineName(n));
    });
    CheckStep(t, [&] { t.d.SetInput(sh, 2, 3, "=Rate*2"); });
  }
}

TEST_CASE("doc_fuzz seed 102: Undo of a clear next to cells shifted up through merged areas") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  const uint32_t text = t.d.wb_mut().styles.mut().InternString("@");
  t.d.ClearUndo();
  CheckStep(t, [&] { t.d.Merge(data, Rect{3, 2, 5, 3}); });           // C4:D6
  CheckStep(t, [&] { t.d.DeleteRows(data, 10, 1); });
  CheckStep(t, [&] { t.d.Paste(sh, 11, 4, t.d.Copy(sh, Rect{3, 3, 5, 3})); });  // D4:D6 to E12
  CheckStep(t, [&] { t.d.SetInput(sh, 6, 2, "Pear"); });
  CheckStep(t, [&] { t.d.ModifyStyle(data, Rect{0, 1, 2, 1}, [text](model::Style& s) { s.numfmt = text; }); });
  CheckStep(t, [&] { t.d.Merge(data, Rect{2, 4, 3, 4}); });           // E3:E4
  CheckStep(t, [&] { t.d.SetInput(sh, 11, 2, "TRUE"); });
  CheckStep(t, [&] { t.d.SetInput(data, 1, 0, "Pear"); });
  CheckStep(t, [&] { t.d.DeleteCells(data, Rect{5, 2, 7, 3}, true); });  // C6:D8, up
  CheckStep(t, [&] { t.d.ClearContents(data, Rect::Cell(4, 3)); });  // D5
}

TEST_CASE("doc_fuzz seed 112: an array formula over several cells of a table is refused, as in Excel") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B2", "h1");
  t.Set("C2", "h2");
  REQUIRE(t.d.AddTable(sh, Rect{1, 1, 2, 2}, true, "TableStyleMedium2") != 0);  // B2:C3
  std::string err;
  CHECK_FALSE(t.d.SetArrayFormula(sh, Rect{1, 2, 2, 2}, "{1;2}", &err));  // C2:C3, through the header
  CHECK(err == "Multi-cell array formulas aren't allowed in tables.");
  CHECK(t.d.S(sh)->spills->empty());
  CHECK(t.V("C2").str() == "h2");
  CHECK(t.d.SetArrayFormula(sh, Rect{5, 5, 6, 5}, "{1;2}", &err));  // F6:F7, outside it
  CHECK(t.d.SetArrayFormula(sh, Rect{2, 2, 2, 2}, "1+1", &err));    // one cell of the table: an ordinary formula there
}

TEST_CASE("Excel's random cases (rand_*): a sort moves formulas as a copy, but references to other sheets stay and ranges are cut at the edge") {
  // Excel 16, measured (probes of 2026-11-15; 40 of the 150 random sequences differed on a sort before)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t other = t.d.AddSheet("Other");
  for (const char* a : {"A1", "A2", "A3", "A4"}) t.Set(a, "1", other);
  t.Set("A1", "k");
  t.Set("A2", "3");  // goes to row 4
  t.Set("A3", "1");  // to row 2
  t.Set("A4", "2");  // to row 3
  t.Set("C2", "=A3");
  t.Set("E2", "=Other!A2");
  t.Set("F2", "=$A$2");
  t.Set("G2", "=A$2");
  t.Set("H4", "=A1");          // two rows up: past the top
  t.Set("D4", "=SUM(A1:A3)");  // one row up: A0:A2, cut to A1:A2
  t.Set("I4", "=SUM(A2:A3)");  // one row up: A1:A2
  // the app's frame holds a snapshot that shares the formula table: a formula the sort makes is added to a copy of it
  // (rand_147 lost =Other!A1+B3 to its value that way, only in a running YCGrid)
  const model::Workbook snapshot = t.d.wb();
  t.d.Sort(sh, Rect{0, 0, 3, 8}, {yc::engine::SortKey{0}}, true);
  CHECK(t.F("C4") == "=A5");
  CHECK(t.F("E4") == "=Other!A2");
  CHECK(t.F("F4") == "=$A$2");
  CHECK(t.F("G4") == "=A$2");
  CHECK(t.F("H3") == "=#REF!");
  CHECK(t.F("D3") == "=SUM(A1:A2)");
  CHECK(t.F("I3") == "=SUM(A1:A2)");
}

TEST_CASE("Excel's random cases (rand_103): arithmetic reads its operands left to right, each as a number") {
  yctest::Doc t;
  t.Set("A1", "=\"Qty\"*#REF!");
  CHECK(t.T("A1") == "#VALUE!");
  t.Set("A2", "=\"5\"*#N/A");  // text that reads as a number: the right side's error
  CHECK(t.T("A2") == "#N/A");
  t.Set("A3", "=#N/A*\"Qty\"");
  CHECK(t.T("A3") == "#N/A");
  t.Set("A4", "=\"Qty\"&#N/A");  // joining text reads no number
  CHECK(t.T("A4") == "#N/A");
  t.Set("A5", "=\"Qty\"=#N/A");
  CHECK(t.T("A5") == "#N/A");
}

TEST_CASE("A formula reading a relative name is calculated again when the cells the name reaches from it change") {
  yctest::Doc t;
  model::NameDef n;
  n.name = "Right1";
  n.refers_to = "=Sheet1!B1";  // relative: one column right of the cell that reads it
  REQUIRE(t.d.DefineName(n));
  REQUIRE(t.Set("D3", "=Right1"));
  CHECK(t.N("D3") == 0);
  REQUIRE(t.Set("E3", "5"));
  CHECK(t.N("D3") == 5);
  REQUIRE(t.Set("B1", "7"));  // what the name says from A1, not what D3 reads
  CHECK(t.N("D3") == 5);
}

TEST_CASE("doc_fuzz seed 247: a #SPILL! anchor brought back by Undo / Redo spills once the cell in its way goes") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  auto step = [&](const char* a1, const char* text) {  // one user step, with its Undo step
    t.d.Begin("type");
    REQUIRE(t.Set(a1, text));
    t.d.Commit();
    t.d.Recalc();
  };
  step("B6", "=TRANSPOSE(A1:B2)");
  step("C7", "$5");
  REQUIRE(t.V("B6").IsError());
  SECTION("Undo and Redo of the typing in the way") {
    REQUIRE(t.d.Undo());
    t.d.Recalc();
    CHECK(t.V("C7").IsNumber());  // the spill is back
    REQUIRE(t.d.Redo());
    t.d.Recalc();
    REQUIRE(t.V("B6").IsError());
  }
  SECTION("Redo of a structural step") {
    t.d.Begin("insert");
    t.d.InsertRows(sh, 20, 1);
    t.d.Commit();
    t.d.Recalc();
    REQUIRE(t.d.Undo());
    REQUIRE(t.d.Redo());
    t.d.Recalc();
    REQUIRE(t.V("B6").IsError());
  }
  SECTION("Undo of a step taken after it") {
    step("A20", "1");
    REQUIRE(t.d.Undo());
    t.d.Recalc();
    REQUIRE(t.V("B6").IsError());
  }
  // the cell in the way goes with its column: B6 spills over B6:C7 (A1:B2 is empty: zeros)
  t.d.DeleteCols(sh, 2, 1);
  t.d.Recalc();
  CHECK(t.V("B6").IsNumber());
  CHECK(t.V("C7").IsNumber());
  t.d.Recalc();
  CHECK(t.V("B6").IsNumber());
}

TEST_CASE("doc_fuzz seed 10030116: the parts of a Ctrl+Shift+Enter array paste as arrays, as Excel's (cse_ cases)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  for (const char* a : {"A1", "B1", "A2", "B2", "A3", "B3"}) REQUIRE(t.Set(a, std::to_string(a[0] - 'A' + 1 + 2 * (a[1] - '1'))));
  REQUIRE(t.Set("C1", "7"));
  REQUIRE(t.Set("D1", "8"));
  REQUIRE(t.Set("C2", "9"));
  REQUIRE(t.Set("D2", "10"));
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A8:B10"), "A1:B3*2"));
  t.d.Recalc();
  auto array_at = [&](const char* a1) {
    uint32_t ar = 0, ac = 0;
    const auto p = yctest::Doc::P(a1);
    const model::SpillRange* sp = t.d.S(sh)->SpillAt(p.row, p.col, &ar, &ac);
    return sp && sp->cse ? yc::core::RectA1(sp->rect) : std::string();
  };
  auto copy = [&](const char* from, const char* to) {
    const auto p = yctest::Doc::P(to);
    t.d.Paste(sh, p.row, p.col, t.d.Copy(sh, yctest::Doc::Range(from)));
    t.d.Recalc();
  };
  SECTION("part of it: an array of the part's size, its formula moved from the array's first cell") {
    copy("A6:B8", "C7");  // A8:B8 lands in C9:D9
    CHECK(array_at("C9") == "C9:D9");
    CHECK(t.F("C9") == "=C2:D4*2");
    CHECK(t.N("C9") == 18);
    CHECK(t.N("D9") == 20);
    CHECK(t.V("C10").IsEmpty());
  }
  SECTION("one cell of it") {
    copy("B9", "D8");
    CHECK(array_at("D8") == "D8");
    CHECK(t.F("D8") == "=D1:E3*2");
    CHECK(t.N("D8") == 16);
  }
  SECTION("all of it") {
    copy("A8:B10", "D8");
    CHECK(array_at("E10") == "D8:E10");
    CHECK(t.F("D8") == "=D1:E3*2");
  }
  SECTION("the fill handle from its last row, and all of it dragged down") {
    t.d.AutoFill(sh, yctest::Doc::Range("A10:B10"), yctest::Doc::Range("A10:B12"));
    t.d.Recalc();
    CHECK(array_at("A11") == "A11:B11");
    CHECK(t.F("A12") == "=A5:B7*2");
    t.d.AutoFill(sh, yctest::Doc::Range("A8:B10"), yctest::Doc::Range("A8:B13"));
    t.d.Recalc();
    CHECK(array_at("B13") == "A11:B13");
    CHECK(t.F("A11") == "=A4:B6*2");
  }
  SECTION("the fill handle past the array's end: the part that fits, calculated (doc_fuzz seed 10040032)") {
    yctest::Doc u;
    const uint32_t us = u.sheet();
    REQUIRE(u.Set("A3", "164"));
    REQUIRE(u.d.SetArrayFormula(us, yctest::Doc::Range("A5:A6"), "A1:B3*2"));
    u.d.Recalc();
    const uint32_t dup = u.d.CopySheet(us);  // on a copy of the sheet, as the fuzzer found it
    u.d.Recalc();
    for (const uint32_t at : {us, dup}) {
      u.d.Begin("fill");
      u.d.AutoFill(at, yctest::Doc::Range("A5:A6"), yctest::Doc::Range("A5:A7"));
      u.d.Commit();
      u.d.Recalc();
      CHECK(u.F("A7", at) == "=A3:B5*2");
      CHECK(u.N("A7", at) == 328);
    }
  }
  SECTION("a fill down from its last row: an array a row") {
    t.d.FillDown(sh, yctest::Doc::Range("A10:B12"));
    t.d.Recalc();
    CHECK(array_at("B11") == "A11:B11");
    CHECK(t.F("A11") == "=A4:B6*2");
    CHECK(array_at("A12") == "A12:B12");
    CHECK(t.F("A12") == "=A5:B7*2");
  }
}

TEST_CASE("doc_fuzz seed 10090004: a table over any of a multi-cell array is refused, as in Excel; a one-cell array is taken in") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("C6:D7"), "A1:B3*2"));
  t.d.Recalc();
  std::string err;
  CHECK(t.d.AddTable(sh, yctest::Doc::Range("D7:E9"), true, "", &err) == 0);  // part of it
  CHECK(err == "Multi-cell array formulas aren't allowed in tables.");
  CHECK(t.d.AddTable(sh, yctest::Doc::Range("C6:D9"), true, "", &err) == 0);  // all of it
  CHECK(t.d.AddTable(sh, yctest::Doc::Range("D3:E6"), true, "", &err) == 0);  // its first row as the table's data
  CHECK(t.d.AddTable(sh, yctest::Doc::Range("F6:G9"), true, "", &err) != 0);  // beside it
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A12"), "A1:B3*2"));
  CHECK(t.d.AddTable(sh, yctest::Doc::Range("A11:B14"), true, "", &err) != 0);  // a one-cell array in its body
}

TEST_CASE("doc_fuzz seed 10040032: a range over an array of a copied sheet is read after Undo and Redo") {
  // Redo of a step put back a version and emptied the pending set, but left the copy's array in pending_spills_: every
  // range over its cells waited for a cell nobody calculated, and SUM over them stayed 0
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  t.d.ClearUndo();
  t.d.SetInput(data, 2, 0, "164");
  CheckStep(t, [&] { t.d.SetArrayFormula(data, yctest::Doc::Range("A5:A6"), "A1:B3*2"); });
  uint32_t dup = 0;
  CheckStep(t, [&] { dup = t.d.CopySheet(data); });
  REQUIRE(t.Set("B1", "=SUM('Data (2)'!A3:B5)"));
  CHECK(t.N("B1") == 164);
  // and the fill handle over that array, each step undone and redone as the fuzzer found it
  CheckStep(t, [&] { t.d.AutoFill(dup, yctest::Doc::Range("A5:A6"), yctest::Doc::Range("A5:A7")); });
  CHECK(t.F("A7", dup) == "=A3:B5*2");
  CHECK(t.N("A7", dup) == 328);
}

TEST_CASE("lessons checked against Excel 16: the 15-digit correction and a formula's format (measured 2026-11-15)") {
  yctest::Doc t;
  // the last addition or subtraction that cancels is 0; parentheses, 1*(...) and a +0 after it keep the residue
  CHECK(t.Eval("0.1+0.2-0.3") == "0");
  CHECK(t.Eval("0.3-0.2-0.1") == "0");
  CHECK(t.Eval("(0.1+0.2)-0.3") == "0");
  CHECK(t.Eval("SUM(0.1,0.2,-0.3)") == "0");
  CHECK(t.N("Z1000") == 0);
  t.Eval("(0.1+0.2-0.3)");
  CHECK(t.N("Z1000") != 0);
  t.Eval("1*(0.1+0.2-0.3)");
  CHECK(t.N("Z1000") != 0);
  t.Eval("0.1+0.2-0.3+0");
  CHECK(t.N("Z1000") != 0);
  // a formula in a General cell takes the format of what it reads, the first that has one
  const uint32_t pct = t.d.wb_mut().styles.mut().InternString("0.00%");
  t.d.ModifyStyle(t.sheet(), yctest::Doc::Range("A1:A2"), [pct](model::Style& st) { st.numfmt = pct; });
  REQUIRE(t.Set("A1", "0.5"));
  REQUIRE(t.Set("A2", "0.5"));
  REQUIRE(t.Set("B2", "3"));
  REQUIRE(t.Set("B1", "=A1+1"));
  REQUIRE(t.Set("C2", "=B2+A2"));
  auto fmt = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    return std::string(t.d.wb().styles->NumberFormat(t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(p.row, p.col))));
  };
  CHECK(fmt("B1") == "0.00%");
  CHECK(fmt("C2") == "0.00%");
}

TEST_CASE("doc_fuzz seed 10110000: merging cells over part or all of an array, as Excel 16 does (measured)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  auto array_at = [&](const char* a1) {
    uint32_t ar = 0, ac = 0;
    const auto p = yctest::Doc::P(a1);
    const model::SpillRange* sp = t.d.S(sh)->SpillAt(p.row, p.col, &ar, &ac);
    return sp && sp->cse ? yc::core::RectA1(sp->rect) : std::string();
  };
  auto merged = [&](const char* a1) {
    const auto p = yctest::Doc::P(a1);
    for (const auto& m : *t.d.S(sh)->merges) {
      if (m.Contains(p.row, p.col)) return yc::core::RectA1(m);
    }
    return std::string();
  };
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A11:B12"), "A1:B3*2"));
  t.d.Recalc();
  SECTION("over part of it: refused") {
    t.d.Merge(sh, yctest::Doc::Range("B10:C12"));
    CHECK(merged("B11").empty());
    CHECK(array_at("B12") == "A11:B12");
  }
  SECTION("over exactly it: refused") {
    t.d.Merge(sh, yctest::Doc::Range("A11:B12"));
    CHECK(merged("A11").empty());
    CHECK(array_at("B12") == "A11:B12");
  }
  SECTION("over all of it and more: merged, the array gone") {
    t.d.Merge(sh, yctest::Doc::Range("A10:C13"));
    t.d.Recalc();
    CHECK(merged("A11") == "A10:C13");
    CHECK(array_at("A11").empty());
    CHECK(t.V("A10").IsEmpty());  // the array's value isn't the one kept
    CHECK(t.F("A10").empty());
  }
  SECTION("with the array in its first row: refused") {
    yctest::Doc u;
    REQUIRE(u.d.SetArrayFormula(u.sheet(), yctest::Doc::Range("B11:C12"), "A1:B3*2"));
    u.d.Merge(u.sheet(), yctest::Doc::Range("A11:C12"));
    CHECK(u.d.S(u.sheet())->merges->empty());
    u.d.Merge(u.sheet(), yctest::Doc::Range("B11:C13"));
    CHECK(u.d.S(u.sheet())->merges->empty());
  }
  SECTION("a one-cell array: merged, the array kept") {
    REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("E11"), "A1:B3*2"));
    t.d.Merge(sh, yctest::Doc::Range("E11:F11"));
    t.d.Recalc();
    CHECK(merged("E11") == "E11:F11");
    CHECK(array_at("E11") == "E11");
  }
}

TEST_CASE("doc_fuzz seed 10130034: an array entered over a dynamic array's spill takes its cells; the dynamic array shows #SPILL!") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("B6", "=SEQUENCE(3)"));
  REQUIRE(t.N("B8") == 3);
  CheckStep(t, [&] { REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A7:B8"), "A1:B3*2")); });
  CHECK(t.V("B6").IsError());
  CHECK(t.N("B7") == 0);  // the array's own value
  CheckStep(t, [&] { t.d.InsertCells(sh, yctest::Doc::Range("A6"), false); });  // row 6 shifts right, the array stays
  CHECK(t.N("C8") == 3);  // moved off the array, it spills again
  CHECK(t.V("B7").IsNumber());
  CHECK(t.V("B8").IsNumber());
  const model::Workbook copy = t.d.wb();
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(copy, yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  yc::engine::Document fresh(std::move(lr.wb));
  CHECK(yc::io::ContentDump(fresh.wb()) == Dump(t));
}

TEST_CASE("doc_fuzz seed 10250095: inserting or deleting cells that would move part of a table is refused, as Excel 16 (measured)") {
  struct Case {
    const char* cells;
    bool insert, vertical, refused;
    const char* table_after;
  };
  // table C7:D9; Excel 16's answers, measured through ListObjects
  const Case cases[] = {
      {"C6", false, true, true, "C7:D9"},     {"C6:D6", false, true, false, "C6:D8"}, {"B6:E6", false, true, false, "C6:D8"},
      {"C10:D10", true, true, false, "C7:D9"}, {"C10", true, true, false, "C7:D9"},  {"C8", true, true, true, "C7:D9"},
      {"C8:D8", true, true, false, "C7:D10"}, {"C6", true, true, true, "C7:D9"},     {"B8", true, false, true, "C7:D9"},
      {"B7:B9", true, false, false, "D7:E9"}, {"B8", false, false, true, "C7:D9"},  {"E8", true, false, false, "C7:D9"},
      {"E8", false, false, false, "C7:D9"},
  };
  for (const Case& k : cases) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("C7:D9"), true, "") != 0);
    const uint32_t before = t.d.protect_blocks();
    if (k.insert) t.d.InsertCells(sh, yctest::Doc::Range(k.cells), k.vertical);
    else t.d.DeleteCells(sh, yctest::Doc::Range(k.cells), k.vertical);
    INFO((k.insert ? "insert " : "delete ") << k.cells << (k.vertical ? " vertically" : " sideways"));
    CHECK((t.d.protect_blocks() > before) == k.refused);
    CHECK(yc::core::RectA1(t.d.S(sh)->tables->front().range) == k.table_after);
  }
}

TEST_CASE("doc_fuzz seed 10260083: cells deleted inside a table across part of it delete its whole rows or columns (Excel 16, measured)") {
  auto setup = [](yctest::Doc& t) {
    const uint32_t sh = t.sheet();
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("C7:D10"), true, "") != 0);
    for (const auto& [a, v] : std::vector<std::pair<const char*, const char*>>{
             {"C8", "a8"}, {"D8", "b8"}, {"C9", "a9"}, {"D9", "b9"}, {"C10", "a10"}, {"D10", "b10"}, {"C11", "below"}}) {
      REQUIRE(t.Set(a, v));
    }
  };
  SECTION("C9 up: the table's row 9") {
    yctest::Doc t;
    setup(t);
    t.d.DeleteCells(t.sheet(), yctest::Doc::Range("C9"), true);
    CHECK(t.T("C9") == "a10");
    CHECK(t.T("D9") == "b10");
    CHECK(t.T("C10") == "below");
  }
  SECTION("D10 up: the table's last row") {
    yctest::Doc t;
    setup(t);
    t.d.DeleteCells(t.sheet(), yctest::Doc::Range("D10"), true);
    CHECK(t.T("C9") == "a9");
    CHECK(t.T("C10") == "below");
    CHECK(t.V("D10").IsEmpty());
  }
  SECTION("D9 left: the table's column D") {
    yctest::Doc t;
    setup(t);
    t.d.DeleteCells(t.sheet(), yctest::Doc::Range("D9"), false);
    CHECK(t.T("C9") == "a9");
    CHECK(t.V("D8").IsEmpty());
    CHECK(t.V("D9").IsEmpty());
    CHECK(t.V("D10").IsEmpty());
  }
  SECTION("a formula never lands in its header row") {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    REQUIRE(t.Set("C7", "=SUM(A1:C5)"));
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("A7:A10"), true, "") != 0);
    t.d.DeleteCells(sh, yctest::Doc::Range("A7:B8"), false);
    for (const auto& tb : *t.d.S(sh)->tables) {
      for (uint32_t c = tb.range.c0; c <= tb.range.c1; ++c) CHECK(t.d.S(sh)->GetFormula(tb.range.r0, c) == 0);
    }
  }
}

TEST_CASE("doc_fuzz seed 10240003: a copied sheet's #SPILL! spills once the cell in its way goes; its arrays are indexed") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("C8", "=A1:A3*2"));
  REQUIRE(t.Set("C10", "x"));
  REQUIRE(t.V("C8").IsError());
  const uint32_t dup = t.d.CopySheet(sh);
  t.d.Recalc();
  REQUIRE(t.V("C8", dup).IsError());
  t.d.DeleteRows(dup, 9, 1);  // row 10, with the x
  t.d.Recalc();
  CHECK(t.N("C8", dup) == 0);
  CHECK(t.N("C10", dup) == 0);
  // the copy's spill is an array of its own: an array entered over it takes the cells
  REQUIRE(t.d.SetArrayFormula(dup, yctest::Doc::Range("B9:C9"), "A1:B1"));
  t.d.Recalc();
  CHECK(t.V("C8", dup).IsError());
}

TEST_CASE("lessons checked against Excel 16: a defined name that is a union (measured 2026-11-15)") {
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  REQUIRE(t.Set("A1", "1", data));
  REQUIRE(t.Set("A3", "2", data));
  model::NameDef u;
  u.name = "U";
  u.refers_to = "=Data!$A$1,Data!$A$3";
  REQUIRE(t.d.DefineName(u));
  CHECK(t.Eval("SUM(U)") == "3");
  CHECK(t.Eval("AREAS(U)") == "2");
  CHECK(t.Eval("COUNT(U)") == "2");
  CHECK(t.Eval("U") == "#VALUE!");
  CHECK(t.Eval("ROWS(U)") == "#REF!");
  CHECK(t.Eval("INDEX(U,1,1,2)") == "2");
  CHECK(t.Eval("SUM(U,5)") == "8");
}

TEST_CASE("a circular reference without iteration: what each cell shows, as Excel 16 (measured 2026-11-15)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "5"));
  REQUIRE(t.Set("B1", "=$A$1*2"));
  REQUIRE(t.Set("A1", "=A1+1"));  // typed: closes a cycle, shows 0; what reads it keeps its value
  CHECK(t.N("A1") == 0);
  CHECK(t.N("B1") == 10);
  REQUIRE(t.Set("C1", "=$A$1*3"));  // typed, reads the cycle: 0
  CHECK(t.N("C1") == 0);
  t.d.Paste(sh, 0, 3, t.d.Copy(sh, yctest::Doc::Range("B1")));  // copied: keeps the value it was copied with
  t.d.Recalc();
  CHECK(t.F("D1") == "=$A$1*2");
  CHECK(t.N("D1") == 10);
  t.d.FillRight(sh, yctest::Doc::Range("B1:E1"));
  t.d.Recalc();
  CHECK(t.N("C1") == 10);
  CHECK(t.N("E1") == 10);
  REQUIRE(t.Set("B2", "7"));
  REQUIRE(t.Set("B3", "=$A$1+B2"));
  CHECK(t.N("B3") == 0);
  REQUIRE(t.Set("B2", "8"));
  CHECK(t.N("B3") == 0);
  // a range over its own cell
  REQUIRE(t.Set("G5", "x"));
  REQUIRE(t.Set("G6", "y"));
  REQUIRE(t.Set("G7", "=COUNTA(G4:G9)"));
  CHECK(t.N("G7") == 0);
  REQUIRE(t.Set("H5", "1"));
  REQUIRE(t.Set("H7", "2"));
  REQUIRE(t.Set("H6", "=SUM(H5:H7)"));
  CHECK(t.N("H6") == 0);
  REQUIRE(t.Set("J5", "3"));
  REQUIRE(t.Set("J6", "=ROWS(J5:J7)+J5"));  // reads no value of its own cell: no cycle
  CHECK(t.N("J6") == 6);
}

TEST_CASE("unit tests' formula claims checked against Excel 16: ADDRESS, the match modes, references only (measured 2026-11-15)") {
  yctest::Doc t;
  SECTION("ADDRESS quotes a sheet name as a reference would, after a [book] prefix") {
    CHECK(t.Eval("ADDRESS(2,3,1,FALSE,\"[Book1]Sheet1\")") == "[Book1]Sheet1!R2C3");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"[Book1]Sheet1\")") == "[Book1]Sheet1!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"[My Book]Sheet1\")") == "'[My Book]Sheet1'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"[Book1]My Sheet\")") == "'[Book1]My Sheet'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"[Book1]\")") == "[Book1]!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"abc[1]\")") == "'abc[1]'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"Sheet-1\")") == "'Sheet-1'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"1abc\")") == "'1abc'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"R1C1\")") == "'R1C1'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"Sheet'1\")") == "'Sheet''1'!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"a.b\")") == "a.b!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"_x\")") == "_x!$C$2");
    CHECK(t.Eval("ADDRESS(2,3,1,TRUE,\"a(b)\")") == "'a(b)'!$C$2");
  }
  SECTION("XMATCH and XLOOKUP take the integer part of a mode (INT), MATCH its sign") {
    CHECK(t.Eval("XMATCH(2,{1,2,3},0,-1.5)") == "2");
    CHECK(t.Eval("XMATCH(2,{1,2,3},0,1.9)") == "2");
    CHECK(t.Eval("XMATCH(2,{1,2,3},0.5)") == "2");
    CHECK(t.Eval("XMATCH(2.5,{1,2,3},-1.5)") == "#VALUE!");
    CHECK(t.Eval("XMATCH(2.5,{1,2,3},1.9)") == "3");
    CHECK(t.Eval("XMATCH(2,{1,2,3},0,2.5)") == "2");
    CHECK(t.Eval("XMATCH(2,{1,2,3},0,-2.5)") == "#VALUE!");
    CHECK(t.Eval("XMATCH(2,{1,2,3},0,0.5)") == "#VALUE!");
    CHECK(t.Eval("XMATCH(2,{1,2,3},0,-0.5)") == "2");
    CHECK(t.Eval("XLOOKUP(2,{1,2,3},{4,5,6},\"x\",0.9)") == "5");
    CHECK(t.Eval("XLOOKUP(2,{1,2,3},{4,5,6},\"x\",0,-1.5)") == "5");
    CHECK(t.Eval("XLOOKUP(2.5,{1,2,3},{4,5,6},\"x\",-1.2)") == "#VALUE!");
    CHECK(t.Eval("MATCH(2,{1,2,3},0.5)") == "2");
    CHECK(t.Eval("MATCH(2.5,{1,2,3},0.5)") == "2");
    CHECK(t.Eval("MATCH(2.5,{3,2,1},-0.5)") == "1");
    // the binary searches step as Excel's (measured): the leftmost equal value probed, the rightmost when descending
    const std::pair<const char*, const char*> binary[] = {
        {"XMATCH(2,{1,2,2,2,3},0,2)", "2"}, {"XMATCH(2,{1,2,2,2,2,2,3},0,2)", "2"}, {"XMATCH(2,{1,1,1,2,2,2,2,3,3},0,2)", "4"},
        {"XMATCH(2,{3,2,2,2,1},0,-2)", "4"}, {"XMATCH(2,{1,2,3},0,-2)", "2"}, {"XMATCH(1,{1,2,3},0,-2)", "#N/A"},
        {"XMATCH(3,{1,2,3},0,-2)", "#N/A"}, {"XMATCH(2,{3,1,2},0,2)", "3"}, {"XMATCH(1,{3,1,2},0,2)", "2"},
        {"XMATCH(2.5,{1,2,3},-1,2)", "2"}, {"XMATCH(2.5,{1,2,2,3},-1,2)", "3"}, {"XMATCH(2.5,{1,2,2,3},1,2)", "4"},
        {"XMATCH(2,{1,2,2,3},-1,2)", "2"}, {"XMATCH(2,{1,2,2,3},1,2)", "2"}, {"XMATCH(0,{1,2,3},-1,2)", "#N/A"},
        {"XMATCH(4,{1,2,3},1,2)", "#N/A"}, {"XMATCH(2.5,{3,2,1},-1,-2)", "2"}, {"XMATCH(2.5,{3,2,1},1,-2)", "1"},
        {"XMATCH(2,{3,2,2,1},-1,-2)", "3"}, {"XMATCH(7,{1,2,3,4,5,6,7,8},0,2)", "7"}, {"XMATCH(8,{1,2,3,4,5,6,7,8},0,2)", "8"},
        {"XMATCH(2,{2,2,2,2,2,2,2,3},0,2)", "1"}, {"XMATCH(\"b\",{\"a\",\"b\",\"b\",\"c\"},0,2)", "2"},
        {"XLOOKUP(2,{1,2,2,2,3},{10,20,30,40,50},,0,2)", "20"}, {"XLOOKUP(2,{1,2,3},{4,5,6},\"x\",0,-2)", "5"},
    };
    for (const auto& [f, want] : binary) {
      INFO(f);
      CHECK(t.Eval(f) == want);
    }
  }
  SECTION("a constant where only a reference goes is refused as it is typed, as is LET without names") {
    for (const char* f : {"=PHONETIC(\"abc\")", "=ROW(\"A1\")", "=COLUMN(5)", "=OFFSET(\"A1\",1,1)", "=AREAS(\"A1\")",
                          "=SUMIF(\"a\",\"a\")", "=COUNTIF({1,2},1)", "=LET(1,2,3)", "=LET(x,1)"}) {
      INFO(f);
      CHECK_FALSE(t.Set("B1", f));
    }
    REQUIRE(t.Set("B1", "=LET(x,1,y)"));
    CHECK(t.T("B1") == "#NAME?");
    REQUIRE(t.Set("B1", "=LET(x_1,1,x_1)"));
    CHECK(t.N("B1") == 1);
    REQUIRE(t.Set("B1", "=ROW(INDIRECT(\"A3\"))"));  // a reference made by a function is fine
    CHECK(t.N("B1") == 3);
  }
}

TEST_CASE("doc_fuzz seeds 10280369, 10290940, 10270301: whatever lands in a table's header row is its text") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  REQUIRE(t.d.AddTable(data, yctest::Doc::Range("B10:C13"), true, "") != 0);
  t.d.ClearUndo();
  SECTION("cut onto it from another sheet (10280369)") {
    REQUIRE(t.Set("B4", "00123"));
    CheckStep(t, [&] { t.d.MoveCells(sh, yctest::Doc::Range("B4:C6"), data, 9, 2); });
    CHECK(t.V("C10", data).IsString());
    CHECK(t.T("C10", data) == "123");
  }
  SECTION("filled down into it (10290940)") {
    REQUIRE(t.Set("B9", "92", data));
    REQUIRE(t.Set("C9", "=1+1", data));
    CheckStep(t, [&] { t.d.FillDown(data, yctest::Doc::Range("B9:C10")); });
    CHECK(t.V("B10", data).IsString());
    CHECK(t.d.S(data)->GetFormula(9, 2) == 0);
  }
  SECTION("blanks filled down into it (10290940)") {
    CheckStep(t, [&] { t.d.FillDown(data, yctest::Doc::Range("B9:C10")); });
    CHECK(t.T("B10", data) == "Column1");
    CHECK(t.T("C10", data) == "Column2");
  }
  SECTION("a formula cut onto it (10270301)") {
    REQUIRE(t.Set("A1", "=1+1"));
    CheckStep(t, [&] { t.d.MoveCells(sh, yctest::Doc::Range("A1"), data, 9, 1); });
    CHECK(t.d.S(data)->GetFormula(9, 1) == 0);
    CHECK(t.V("B10", data).IsString());
  }
}

TEST_CASE("doc_fuzz seed 10312766: a table grows by typing beside it only over what a table can hold") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("C8:D10"), "A1:B3*2"));
  const uint32_t id = t.d.AddTable(sh, yctest::Doc::Range("B10:B12"), true, "");
  REQUIRE(id != 0);
  REQUIRE(t.Set("C12", "5"));  // beside it: would take in C10, part of the array
  CHECK(yc::core::RectA1(t.d.S(sh)->tables->front().range) == "B10:B12");
  REQUIRE(t.Set("B13", "6"));  // below it: grows
  CHECK(yc::core::RectA1(t.d.S(sh)->tables->front().range) == "B10:B13");
  std::string err;
  CHECK_FALSE(t.d.ResizeTable(sh, id, yctest::Doc::Range("B10:C13"), &err));
  CHECK(err == "Multi-cell array formulas aren't allowed in tables.");
}

TEST_CASE("doc_fuzz seed 10301020: a #SPILL! anchor spills once whatever was in its way is gone, a merged area too") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.Merge(sh, yctest::Doc::Range("C8:C10")));
  REQUIRE(t.Set("C7", "=SEQUENCE(3)"));
  REQUIRE(t.V("C7").IsError());
  CheckStep(t, [&] { t.d.Unmerge(sh, yctest::Doc::Range("C8:C10")); });  // no value changed there
  CHECK(t.N("C9") == 3);
  REQUIRE(t.d.Merge(sh, yctest::Doc::Range("C8:C10")));
  t.d.Recalc();
  CHECK(t.V("C7").IsError());
  // a fill over part of the merged area removes it, as a paste does
  CheckStep(t, [&] { t.d.AutoFill(sh, yctest::Doc::Range("B7:B9"), yctest::Doc::Range("B7:C9")); });
  t.d.Recalc();
  const model::Workbook copy = t.d.wb();
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(copy, yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  yc::engine::Document fresh(std::move(lr.wb));
  CHECK(yc::io::ContentDump(fresh.wb()) == Dump(t));
}

TEST_CASE("doc_fuzz seed 10320462: tables and dynamic arrays, as Excel 16 (measured): no spill in or into a table") {
  SECTION("a table over part of a spill takes its cells empty; the array shows #SPILL!") {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    REQUIRE(t.Set("C6", "=SEQUENCE(3)"));
    CheckStep(t, [&] { REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("C7:D9"), true, "") != 0); });
    CHECK(t.T("C7") == "Column1");
    CHECK(t.T("D7") == "Column2");
    CHECK(t.V("C6").IsError());
    CHECK(t.V("C8").IsEmpty());
  }
  SECTION("a dynamic array typed in a table, or spilling into one, is #SPILL!") {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("C7:D9"), true, "") != 0);
    REQUIRE(t.Set("D8", "=SEQUENCE(2)"));
    CHECK(t.V("D8").IsError());
    REQUIRE(t.Set("C3", "=SEQUENCE(6)"));
    CHECK(t.V("C3").IsError());
    REQUIRE(t.Set("F3", "=SEQUENCE(2)"));  // beside it: spills
    CHECK(t.N("F4") == 2);
  }
}

TEST_CASE("calc_fuzz seed 3022: a binary search over a whole column, as Excel 16 (measured): data in other columns changes nothing") {
  yctest::Doc t;
  const int vals[] = {5, 60, 7, 45, 3, 90, 12, 44, 2, 80, 41, 9, 70, 30, 55, 1, 99, 20, 43, 8};
  for (int i = 0; i < 20; ++i) REQUIRE(t.Set(("G" + std::to_string(i + 1)).c_str(), std::to_string(vals[i])));
  REQUIRE(t.Set("A1", "=MATCH(43,G:G,1)"));
  REQUIRE(t.Set("A2", "=MATCH(43,G1:G20,1)"));
  REQUIRE(t.Set("A3", "=MATCH(43,G1:G1000,1)"));
  REQUIRE(t.Set("A4", "=XMATCH(43,G:G,-1,2)"));
  CHECK(t.N("A1") == 20);
  CHECK(t.N("A2") == 7);
  CHECK(t.N("A3") == 12);
  CHECK(t.N("A4") == 20);
  for (const char* far : {"C25", "C1000", "C5000"}) {
    REQUIRE(t.Set(far, "1"));
    t.d.RecalcAll();
    INFO(far);
    CHECK(t.N("A1") == 20);
    CHECK(t.N("A3") == 12);
    CHECK(t.N("A4") == 20);
  }
}

TEST_CASE("doc_fuzz seed 10420333: a formula naming a deleted sheet is the formula its text reads back as") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  const uint32_t dup = t.d.CopySheet(sh);
  t.d.Recalc();
  REQUIRE(t.Set("C5", "=Data!B2+1"));
  REQUIRE(t.Set("D5", "=Data!C2+1"));
  t.d.DeleteCols(data, 1, 1);  // C5 reads Data!#REF!
  t.d.Recalc();
  t.d.DeleteSheet(data);
  t.d.Recalc();
  REQUIRE(t.Set("B10", "=Data!B2+1", dup));  // typed after: #REF!+1, as C5 shows
  REQUIRE(t.Set("B11", "=#REF!B2+1", dup));
  CHECK(t.F("C5") == "=#REF!+1");
  CHECK(t.d.S(sh)->GetFormula(4, 2) == t.d.S(dup)->GetFormula(9, 1));  // one formula, as after an open
  const model::Workbook copy = t.d.wb();
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(copy, yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  yc::engine::Document fresh(std::move(lr.wb));
  CHECK(yc::io::ContentDump(fresh.wb()) == Dump(t));
}

TEST_CASE("doc_fuzz seed 10431002: a formula reading through a name sees the cells a spill fills in the same calculation") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  model::NameDef rate;
  rate.name = "Rate";
  rate.refers_to = "=Sheet1!E4:F5";  // relative: from B8 it is F11:G12
  REQUIRE(t.d.DefineName(rate));
  REQUIRE(t.Set("B8", "=Rate*2"));
  CHECK(t.N("B8") == 0);
  CheckStep(t, [&] { t.d.SetInput(sh, 9, 5, "=SEQUENCE(3)"); });  // F10:F12
  CHECK(t.N("B8") == 4);
  CHECK(t.N("B9") == 6);
  REQUIRE(t.Set("F10", ""));  // the spill goes: B8 reads empty cells again
  CHECK(t.N("B8") == 0);
}

TEST_CASE("doc_fuzz seed 10460222: a range on a deleted sheet reads back as it is written") {
  yctest::Doc t;
  const uint32_t data = t.d.AddSheet("Data");
  REQUIRE(t.Set("F8", "=SUM(Data!A1:A3)"));
  REQUIRE(t.Set("F9", "=Data!A1:B2"));
  t.d.DeleteSheet(data);
  t.d.Recalc();
  CHECK(t.F("F8") == "=SUM(#REF!A1:A3)");
  CHECK(t.F("F9") == "=#REF!A1:B2");
  // copied up and left, off the sheet: #REF! alone there, and the original keeps its text after a save and open
  t.d.Paste(t.sheet(), 0, 0, t.d.Copy(t.sheet(), yctest::Doc::Range("F8")));
  t.d.Recalc();
  CHECK(t.F("A1") == "=SUM(#REF!)");
  CHECK(t.F("F8") == "=SUM(#REF!A1:A3)");
  const model::Workbook copy = t.d.wb();
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(copy, yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  yc::engine::Document fresh(std::move(lr.wb));
  CHECK(yc::io::ContentDump(fresh.wb()) == Dump(t));
}

TEST_CASE("doc_fuzz seed 88001420: a reference to a deleted sheet moves with its formula as one to another sheet does") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  REQUIRE(t.Set("B3", "=OFFSET(Data!A1,1,1)"));
  REQUIRE(t.Set("C5", "=SUM(Data!B2:B4)"));
  t.d.DeleteSheet(data);
  t.d.Recalc();
  CHECK(t.F("B3") == "=OFFSET(#REF!A1,1,1)");
  t.d.DeleteRows(sh, 0, 1);  // the formulas move up a row; what they name doesn't
  t.d.Recalc();
  CHECK(t.F("B2") == "=OFFSET(#REF!A1,1,1)");
  CHECK(t.F("C4") == "=SUM(#REF!B2:B4)");
  t.d.InsertCols(sh, 0, 1);
  t.d.Recalc();
  CHECK(t.F("C2") == "=OFFSET(#REF!A1,1,1)");
  t.d.Sort(sh, yctest::Doc::Range("C1:C4"), {yc::engine::SortKey{2}}, false);
  t.d.Recalc();
  const model::Workbook copy = t.d.wb();
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(copy, yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  yc::engine::Document fresh(std::move(lr.wb));
  CHECK(yc::io::ContentDump(fresh.wb()) == Dump(t));
}

TEST_CASE("doc_fuzz seed 10503625: an array spilling into cells its own inputs read is a circular reference, found in every order") {
  // Excel 16, measured: =Rate*2 (Rate relative, E8:F8 from A2) with =OFFSET(A1,1,1) in E8 reading B2 reports the
  // circular reference (and spills 0, 0 over A2:B2, where YCGrid shows the typed formula's 0 alone: excel-parity-backlog)
  for (const char* f : {"=Rate*2", "=E8:F8*2"}) {
    INFO(f);
    yctest::Doc t;
    const uint32_t data = t.d.AddSheet("Data");
    model::NameDef rate;
    rate.name = "Rate";
    rate.refers_to = "=Data!E7:F7";
    REQUIRE(t.d.DefineName(rate));
    REQUIRE(t.Set("E8", "=OFFSET(A1,1,1)", data));
    REQUIRE(t.Set("A2", f, data));
    CHECK_FALSE(t.d.circular_cells.empty());
    CHECK(t.N("A2", data) == 0);
  }
}

TEST_CASE("doc_fuzz seed 99007204: an array entered over several cells isn't cut into a table") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("D3:E4"), true, "") != 0);
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("B3:B4"), "A1:A2*2"));
  t.d.Recalc();
  const uint32_t before = t.d.protect_blocks();
  CHECK_FALSE(t.d.MoveCells(sh, yctest::Doc::Range("B3:B5"), sh, 1, 3));  // B3:B4 would be D2:D3, over the header
  CHECK(t.d.protect_blocks() > before);
  CHECK(t.T("D3") == "Column1");
  CHECK(t.d.MoveCells(sh, yctest::Doc::Range("B3:B5"), sh, 6, 0));  // elsewhere: moves
  t.d.Recalc();
  CHECK(t.F("A7") == "=A1:A2*2");  // a cut keeps naming the cells it named
}

TEST_CASE("doc_fuzz seed 10573473: Undo of a fill over a #SPILL! anchor gives the cell in its way back") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("C10", "$5"));
  REQUIRE(t.Set("C8", "=SEQUENCE(3)"));
  REQUIRE(t.V("C8").IsError());
  // the fill empties C10 (C7's blank): C8 spills; Undo puts the 5 back and C8 is #SPILL! again
  CheckStep(t, [&] { t.d.AutoFill(sh, yctest::Doc::Range("C6:D8"), yctest::Doc::Range("C6:D11")); });
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.N("C10") == 5);
  CHECK(t.V("C8").IsError());
}

TEST_CASE("doc_fuzz seed 10553941: two arrays reading each other's spill are a circular reference, in every order") {
  for (int order = 0; order < 2; ++order) {
    INFO("order " << order);
    yctest::Doc t;
    // A4 reads G5:H6 (G4's spill) and spills over B4; G4 reads B3:C4 (B4 is A4's spill)
    auto a4 = [&] { REQUIRE(t.Set("A4", "=G4:H5*2")); };
    auto g4 = [&] { REQUIRE(t.Set("G4", "=TRANSPOSE(B3:C4)")); };
    if (order == 0) a4(), g4();
    else g4(), a4();
    CHECK_FALSE(t.d.circular_cells.empty());
    const auto circular = t.d.circular_cells.size();
    t.d.RecalcAll();
    CHECK(t.d.circular_cells.size() == circular);
  }
}

TEST_CASE("doc_fuzz seed 10592817: a formula typed into a spill after a fill through it blocks the spill") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("B12", "=TRANSPOSE(A1:B2)"));
  REQUIRE(t.V("C13").IsNumber());
  CheckStep(t, [&] { t.d.AutoFill(sh, yctest::Doc::Range("C9:C10"), yctest::Doc::Range("C9:C12")); });

  CheckStep(t, [&] { t.d.SetInput(sh, 11, 2, "=A1+B2"); });
  CHECK(t.V("B12").IsError());
}

TEST_CASE("doc_fuzz seed 580: Undo puts back a formula a cycle keeps with the value it showed") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A11", "=VLOOKUP(A1,A1:C9,2,FALSE)"));
  REQUIRE(t.T("A11") == "#N/A");
  // cells deleted up move it into the range it reads: a cycle, which keeps the value it showed
  t.d.DeleteCells(sh, yctest::Doc::Range("A7:A9"), true);
  t.d.Recalc();
  REQUIRE(t.F("A8") == "=VLOOKUP(A1,A1:C9,2,FALSE)");
  REQUIRE(t.T("A8") == "#N/A");
  // a fill clears it, and Undo puts it back as it was, not as a typed formula's 0
  CheckStep(t, [&] { t.d.FillDown(sh, yctest::Doc::Range("A7:A9")); });
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.T("A8") == "#N/A");
}

TEST_CASE("doc_fuzz seed 10660583: deleting an array's anchor calculates what read its spilled values") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  SECTION("alone") {
    REQUIRE(t.Set("B2", "=SEQUENCE(3)"));
    REQUIRE(t.Set("F7", "=B3*2"));
    REQUIRE(t.T("F7") == "4");
    t.d.DeleteRows(sh, 0, 2);
    t.d.Recalc();
    CHECK(t.F("F5") == "=B1*2");
    CHECK(t.T("F5") == "0");
  }
  SECTION("as the fuzzer did") {
    const uint32_t two = t.d.AddSheet("Q 1");
    REQUIRE(t.Set("B2", "=SEQUENCE(3)"));
    REQUIRE(t.Set("F7", "=B3*2"));
    t.d.AutoFill(sh, yctest::Doc::Range("B9:B11"), yctest::Doc::Range("B9:B14"));
    t.d.Recalc();
    t.d.SetInput(two, 7, 3, "=OFFSET(A1,1,1)");
    t.d.Recalc();
    REQUIRE(t.T("F7") == "4");
    t.d.DeleteRows(sh, 0, 2);
    t.d.Recalc();
    CHECK(t.T("F5") == "0");
  }
}

TEST_CASE("doc_fuzz seed 10882828: Undo and Redo keep the circular references their version had") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("E9", "=VLOOKUP(A1,A1:C9,2,FALSE)"));
  REQUIRE(t.T("E9") == "#N/A");
  REQUIRE(t.Set("B5", "=COUNT(A:A)"));
  // moved into column A: a cycle, which E9 reads and keeps its value; Redo puts back a version taken settled
  CheckStep(t, [&] { t.d.DeleteCells(sh, yctest::Doc::Range("A4:A6"), false); });
  REQUIRE(t.F("A5") == "=COUNT(A:A)");
  CHECK(!t.d.circular_cells.empty());
  CHECK(t.T("E9") == "#N/A");
  CheckStep(t, [&] { REQUIRE(t.d.Merge(sh, yctest::Doc::Range("E9:F9"))); });
  CHECK(t.T("E9") == "#N/A");
}

TEST_CASE("doc_fuzz seed 11051768: a formula reading a table header follows the text the header is made") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "a"));
  REQUIRE(t.Set("B1", "b"));
  REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("A1:B3"), true, "TableStyleMedium2") != 0);
  REQUIRE(t.Set("E1", "=B1&\"x\""));
  REQUIRE(t.T("E1") == "bx");
  // a formula typed into a header becomes its value as text before it is calculated: "0" (Excel)
  CheckStep(t, [&] { t.d.SetInput(sh, 0, 1, "=1+1"); });
  CHECK(t.V("B1").IsString());
  CHECK(t.T("E1") == t.T("B1") + "x");
}

TEST_CASE("doc_fuzz seed 11051768: the fuzzer's steps") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  auto step = [&](auto&& fn) {
    t.d.Begin("step");
    fn();
    t.d.Commit();
    t.d.Recalc();
  };
  step([&] { t.d.SetArrayFormula(sh, yctest::Doc::Range("E3"), "A1:B3*2"); });
  step([&] { t.d.ClearContents(sh, yctest::Doc::Range("D6:E7")); });
  step([&] { t.d.InsertCols(sh, 0, 1); });
  step([&] { t.d.Paste(sh, 7, 0, t.d.Copy(sh, yctest::Doc::Range("B2:C3"))); });
  step([&] { t.d.AddTable(sh, yctest::Doc::Range("A1:A3"), true, "TableStyleMedium2"); });
  step([&] { t.d.SetInput(sh, 11, 4, "12%"); });
  step([&] { t.d.MoveCells(sh, yctest::Doc::Range("E8:E10"), sh, 6, 5); });
  step([&] { t.d.SetInput(sh, 0, 1, "=OFFSET(A1,1,1)"); });
  step([&] { t.d.InsertRows(sh, 2, 2); });
  step([&] { t.d.Paste(sh, 0, 3, t.d.Copy(sh, yctest::Doc::Range("B5:C5"))); });
  step([&] { t.d.SetInput(sh, 0, 0, "=A1+B2"); });
  INFO("B1 " << t.T("B1") << " F5 " << t.F("F5"));
  const std::string shown = t.T("F5");
  t.d.RebuildAll();
  t.d.Recalc();
  CHECK(t.T("F5") == shown);
}

TEST_CASE("doc_fuzz seed 11652641: a fill refused over part of an array changes nothing, its merged cells either") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.Merge(sh, yctest::Doc::Range("D11:E11")));
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("D12:E14"), "A1:B3*2"));
  t.d.Recalc();
  const std::string before = Dump(t);
  const size_t merges = t.d.S(sh)->merges->size();
  t.d.Begin("fill");
  t.d.AutoFill(sh, yctest::Doc::Range("D11:E13"), yctest::Doc::Range("D11:E14"));
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.d.S(sh)->merges->size() == merges);
  CHECK(Dump(t) == before);
}

TEST_CASE("doc_fuzz seed 11682258: a reference to a deleted sheet on a copied sheet stays one formula with its text") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  auto step = [&](auto&& fn) {
    t.d.Begin("step");
    fn();
    t.d.Commit();
    t.d.Recalc();
  };
  auto chart = [&](uint32_t on, const std::string& values) {
    model::ChartObj c;
    c.series.emplace_back();
    c.series[0].values = values;
    t.d.AddChart(on, c);
  };
  step([&] { t.d.SetInput(sh, 7, 1, "=SUM(Data!A1:A3)"); });
  step([&] { t.d.AddTable(data, yctest::Doc::Range("B6:C8"), true, "TableStyleMedium2"); });
  step([&] { t.d.ClearContents(sh, yctest::Doc::Range("A4:B4")); });
  step([&] { t.d.InsertCols(sh, 0, 1); });
  step([&] { chart(data, "Data!C6:D8"); });
  step([&] { t.d.InsertCols(data, 2, 1); });
  step([&] { t.d.SetInput(sh, 3, 0, "=TRANSPOSE(A1:B2)"); });
  step([&] { t.d.SetInput(sh, 7, 1, "=COUNT(A:A)"); });
  uint32_t copy = 0;
  step([&] { copy = t.d.CopySheet(sh); });
  step([&] { t.d.SetInput(copy, 2, 1, "-23"); });
  step([&] { t.d.SetInput(sh, 3, 1, "21"); });
  step([&] { REQUIRE(t.d.DeleteSheet(data)); });
  step([&] { t.d.MoveCells(copy, yctest::Doc::Range("A10:B12"), copy, 10, 1); });
  step([&] { t.d.InsertRows(sh, 1, 2); });
  step([&] { t.d.Merge(sh, yctest::Doc::Range("C9:D10")); });
  step([&] {
    model::CfRule rule;
    rule.ranges.push_back(yctest::Doc::Range("B1"));
    rule.type = model::CfType::Expression;
    rule.formula1 = "SUM($B$1:$B$4)>3";
    t.d.AddCondFormat(copy, rule);
  });
  step([&] { chart(copy, "'Sheet1 (2)'!D5:E5"); });
  step([&] { t.d.InsertRows(copy, 6, 1); });
  // the two cells' =SUM(#REF!A1:A3) are one formula, as their text reads back
  CHECK(t.d.S(sh)->GetFormula(8, 2) == t.d.S(copy)->GetFormula(8, 2));
  const model::Workbook wb = t.d.wb();
  yc::io::LoadResult lr = yc::io::LoadYcgFromBytes(yc::io::SaveYcgToBytes(wb, yc::io::SaveOptions{}));
  REQUIRE(lr.ok);
  yc::engine::Document fresh(std::move(lr.wb));
  CHECK(yc::io::ContentDump(fresh.wb()) == Dump(t));
}

TEST_CASE("a sort that leaves every line in place is a step on the undo list, as Excel's (measured)") {
  // measured 2026-11-16: after sorting 1, 2, 3 ascending, Excel's Undo is enabled; Ctrl+Z took back the edit before
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "1"));
  REQUIRE(t.Set("A2", "2"));
  REQUIRE(t.Set("A3", "3"));
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("Sort");
  t.d.Sort(sh, yctest::Doc::Range("A1:A3"), {yc::engine::SortKey{0, false}}, false);
  t.d.Commit();
  CHECK(t.d.UndoDepth() == depth + 1);
  REQUIRE(t.d.Undo());
  CHECK(t.T("A3") == "3");  // the sort's step undone, the typing before it kept
}

namespace {

// A1:E6 a list with the sheet's AutoFilter hiding row 3 (MSFT), a total in row 7 and a column G beside it
uint32_t FilteredList(yctest::Doc& t) {
  const uint32_t sh = t.sheet();
  const char* names[] = {"AAPL", "MSFT", "TSMC", "NVDA", "AMZN"};
  const char* heads[] = {"Name", "Qty", "Price", "Total", "When"};
  for (int c = 0; c < 5; ++c) REQUIRE(t.Set((std::string(1, static_cast<char>('A' + c)) + "1").c_str(), heads[c]));
  for (int i = 0; i < 5; ++i) {
    const std::string r = std::to_string(i + 2);
    REQUIRE(t.Set(("A" + r).c_str(), names[i]));
    REQUIRE(t.Set(("B" + r).c_str(), std::to_string(10 * (i + 1))));
    REQUIRE(t.Set(("C" + r).c_str(), "1.5"));
    REQUIRE(t.Set(("D" + r).c_str(), "=B" + r + "*C" + r));
    REQUIRE(t.Set(("E" + r).c_str(), std::to_string(45000 + i)));
    REQUIRE(t.Set(("G" + r).c_str(), r));
  }
  REQUIRE(t.Set("A7", "Sum"));
  REQUIRE(t.Set("B7", "=SUBTOTAL(9,B2:B6)"));
  REQUIRE(t.d.SetAutoFilter(sh, yctest::Doc::Range("A1:E6")));
  model::FilterColumn fc;
  fc.field = 0;
  fc.kind = 1;
  fc.values = {"aapl", "tsmc", "nvda", "amzn"};
  fc.blanks = false;
  t.d.SetFilterColumn(sh, fc);
  REQUIRE(t.d.S(sh)->rows->IsHidden(2));
  return sh;
}

}  // namespace

TEST_CASE("a table over part of a filtered list is refused before anything changes, as Excel's (measured)") {
  // basics seq2708: Ctrl+T over B2:E7 without headers inserted no header row (refused), went on, and made D2's formula
  // the text 15 of a header. Excel 16 (ListObjects.Add, measured 2026-11-16) refuses B2:E7, A2:E7, A3:E4 and A1:D6
  // with or without headers: "This will change a filtered range on your worksheet..."
  for (const char* range : {"B2:E7", "A2:E7", "A3:E4", "A1:D6"}) {
    for (const bool headers : {false, true}) {
      yctest::Doc t;
      const uint32_t sh = FilteredList(t);
      const std::string before = Dump(t);
      const size_t depth = t.d.UndoDepth();
      std::string err;
      t.d.Begin("Create Table");
      CHECK(t.d.AddTable(sh, yctest::Doc::Range(range), headers, "", &err) == 0);
      t.d.Commit();
      INFO(range << (headers ? " with headers" : " without headers"));
      CHECK(err.find("change a filtered range") != std::string::npos);
      CHECK(Dump(t) == before);
      CHECK(t.d.UndoDepth() == depth);
      CHECK(t.F("D2") == "=B2*C2");
    }
  }
}

TEST_CASE("a table over all of a filtered list takes its criteria only when it is the list, as Excel's (measured)") {
  // the list is A1:E7: applying the filter takes in the rows under it (AutoFilterExtent), as Excel's AutoFilter.Range
  // reads (measured: A1:E7 for a filter set on A1:E6)
  {  // exactly the list: the table's buttons are the sheet's, the hidden row stays hidden
    yctest::Doc t;
    const uint32_t sh = FilteredList(t);
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("A1:E7"), true, "") != 0);
    CHECK(t.d.S(sh)->rows->IsHidden(2));
    CHECK(yc::engine::TableOwnsSheetFilter(*t.d.S(sh), t.d.S(sh)->tables->front()));
  }
  for (const char* range : {"A1:E8", "A1:F7", "A1:F9", "A1:E6"}) {  // more or fewer rows: the criteria go, the row shows
    yctest::Doc t;
    const uint32_t sh = FilteredList(t);
    INFO(range);
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range(range), true, "") != 0);
    CHECK_FALSE(t.d.S(sh)->rows->IsHidden(2));
    CHECK(t.d.S(sh)->autofilter->hidden.empty());
    CHECK(t.d.S(sh)->autofilter->columns.empty());
  }
  {  // the list without headers: a header row is inserted above it, the criteria go
    yctest::Doc t;
    const uint32_t sh = FilteredList(t);
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("A1:E6"), false, "") != 0);
    CHECK(t.T("A1") == "Column1");
    CHECK(t.T("A2") == "Name");
    CHECK(t.F("D3") == "=B3*C3");
    CHECK(t.T("G2") == "2");  // column G stays
    for (uint32_t r = 0; r < 9; ++r) CHECK_FALSE(t.d.S(sh)->rows->IsHidden(r));
  }
  {  // apart from the list: the sheet's AutoFilter stays as it was
    yctest::Doc t;
    const uint32_t sh = FilteredList(t);
    REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("I1:J3"), true, "") != 0);
    CHECK(t.d.S(sh)->rows->IsHidden(2));
    CHECK(t.d.S(sh)->autofilter->range == yctest::Doc::Range("A1:E7"));
  }
  {  // apart, without headers, its header row inserted across the hidden row: refused (Excel raises), nothing changes
    yctest::Doc t;
    const uint32_t sh = FilteredList(t);
    const std::string before = Dump(t);
    t.d.Begin("Create Table");
    CHECK(t.d.AddTable(sh, yctest::Doc::Range("G2:H3"), false, "") == 0);
    t.d.Commit();
    CHECK(Dump(t) == before);
  }
}

TEST_CASE("a table's header or totals row whose insertion is refused writes nothing over the cells in the way") {
  yctest::Doc t;
  const uint32_t sh = FilteredList(t);
  REQUIRE(t.Set("I2", "a"));
  REQUIRE(t.Set("J2", "b"));
  REQUIRE(t.Set("I3", "1"));
  REQUIRE(t.Set("J3", "2"));
  REQUIRE(t.Set("I4", "in the way"));
  const uint32_t id = t.d.AddTable(sh, yctest::Doc::Range("I2:J3"), true, "");
  REQUIRE(id != 0);
  // the header row, hidden and shown again with I2 taken, needs an insertion at I3 across the hidden row 3: refused,
  // and I2 keeps its text (it was written over with the column's name)
  REQUIRE(t.d.UpdateTable(sh, id, [](model::Table& x) { x.header = false; }));
  REQUIRE(t.Set("I2", "taken"));
  const std::string before = Dump(t);
  CHECK_FALSE(t.d.UpdateTable(sh, id, [](model::Table& x) { x.header = true; }));
  CHECK(t.T("I2") == "taken");
  CHECK(Dump(t) == before);
  // the totals row under a table whose next row is taken, the insertion across the hidden row refused
  yctest::Doc u;
  const uint32_t sh2 = FilteredList(u);
  REQUIRE(u.Set("I1", "a"));
  REQUIRE(u.Set("J1", "b"));
  REQUIRE(u.Set("I2", "1"));
  REQUIRE(u.Set("I3", "in the way"));
  const uint32_t id2 = u.d.AddTable(sh2, yctest::Doc::Range("I1:J2"), true, "");
  REQUIRE(id2 != 0);
  const std::string before2 = Dump(u);
  CHECK_FALSE(u.d.UpdateTable(sh2, id2, [](model::Table& x) { x.totals = true; }));
  CHECK(u.T("I3") == "in the way");
  CHECK(Dump(u) == before2);
}

TEST_CASE("an AutoFilter set on whole rows or columns goes as far as the data, as Excel's (measured)") {
  // Excel 16, measured 2026-11-16: over data in A1:H7, Range("3:4").AutoFilter reads A3:H7 and Range("B:C") B1:C7.
  // Whole rows kept their width to XFD, and a table over the list was refused (basics seq19)
  for (const bool rows : {true, false}) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    for (int r = 1; r <= 7; ++r) {
      for (char c = 'A'; c <= 'H'; ++c) REQUIRE(t.Set((std::string(1, c) + std::to_string(r)).c_str(), r == 1 ? std::string("h") + c : std::to_string(r)));
    }
    const model::Sheet& s0 = *t.d.S(sh);
    const Rect given = rows ? Rect{2, 0, 3, s0.col_limit - 1} : Rect{0, 1, s0.row_limit - 1, 2};
    REQUIRE(t.d.SetAutoFilter(sh, given));
    INFO((rows ? "rows 3:4" : "columns B:C"));
    CHECK(t.d.S(sh)->autofilter->range == yctest::Doc::Range(rows ? "A3:H7" : "B1:C7"));
    // a table over part of the list is refused, one over all its header row and columns takes it over (measured)
    std::string err;
    CHECK(t.d.AddTable(sh, yctest::Doc::Range(rows ? "A3:E4" : "B1:B7"), true, "", &err) == 0);
    CHECK(err.find("change a filtered range") != std::string::npos);
    CHECK(t.d.AddTable(sh, yctest::Doc::Range(rows ? "A3:H4" : "B1:C7"), true, "", &err) != 0);
  }
}

TEST_CASE("a paste whose copy area would run past the sheet's edge is refused, as Excel's (measured)") {
  // Excel 16, measured 2026-11-16 (Range.Copy Destination): B5:XFD6 to J2, A1:B2 to XFD1 and a whole column to row 2
  // raise "We can't paste here because the Copy area and paste area aren't the same size"; B5:XFD6 to A2 fits.
  // YCGrid pasted the used part of the copy and selected cells past XFD (basics seq 268656)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("B5", "1"));
  REQUIRE(t.Set("C6", "2"));
  const model::Sheet& s = *t.d.S(sh);
  const yc::engine::ClipData wide = t.d.Copy(sh, Rect{4, 1, 5, s.col_limit - 1});
  CHECK(wide.cols == 2);  // only the used part is copied...
  CHECK(wide.area.Cols() == 16383);  // ...but the copy area is what a paste must fit
  CHECK(yc::engine::PasteRunsOffSheet(wide, 1, 9));
  CHECK_FALSE(yc::engine::PasteRunsOffSheet(wide, 1, 0));
  CHECK(yc::engine::PasteRunsOffSheet(t.d.Copy(sh, yctest::Doc::Range("A1:B2")), 0, 16383));
  CHECK_FALSE(yc::engine::PasteRunsOffSheet(t.d.Copy(sh, yctest::Doc::Range("A1:B2")), 0, 16382));
  const yc::engine::ClipData column = t.d.Copy(sh, Rect{0, 1, s.row_limit - 1, 1});
  CHECK_FALSE(yc::engine::PasteRunsOffSheet(column, 0, 3));
  CHECK(yc::engine::PasteRunsOffSheet(column, 1, 3));
  // transposed, the copy area's rows go across
  CHECK(yc::engine::PasteRunsOffSheet(t.d.Copy(sh, yctest::Doc::Range("A1:A3")), 0, 16382, true));
}

TEST_CASE("a refusal's kind stays until the next refusal, whatever is allowed in between") {
  // basics seq270330: Delete Sheet Rows over two areas refused the first (a table's header row), the next area's
  // allowed check cleared the kind, and the UI showed "The cell or chart you're trying to change is on a protected
  // sheet" on a sheet without protection
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("D5", "a"));
  REQUIRE(t.Set("E5", "b"));
  REQUIRE(t.Set("D6", "1"));
  REQUIRE(t.d.AddTable(sh, yctest::Doc::Range("D5:E6"), true, "") != 0);
  const uint32_t before = t.d.protect_blocks();
  CHECK(t.d.RefuseRowsCols(sh, true, false, 4, 1));  // row 5, the table's header row: refused
  CHECK(t.d.protect_blocks() == before + 1);
  CHECK(t.d.block_table_shift());
  CHECK_FALSE(t.d.RefuseRowsCols(sh, true, false, 1, 1));  // row 2: allowed
  REQUIRE(t.Set("A1", "x"));  // a write (Refuse asked, allowed)
  CHECK(t.d.protect_blocks() == before + 1);
  CHECK(t.d.block_table_shift());  // still what the last refusal was
  // the next refusal says its own kind only
  t.d.SetArrayFormula(sh, yctest::Doc::Range("J1:J3"), "1");
  t.d.SetInput(sh, 1, 9, "5");  // part of the array: refused
  CHECK(t.d.protect_blocks() == before + 2);
  CHECK(t.d.block_array());
  CHECK_FALSE(t.d.block_table_shift());
}

TEST_CASE("the anchor of an array entered with Ctrl+Shift+Enter is part of it: a write there changes nothing") {
  // Excel 16 (measured): Range("A4").Formula = "=1" over A4:B4's array {=D1*2} leaves A4 and B4 as they were. Written
  // as "a range written cell by cell", the anchor took a plain formula while the array's record and B4's value stayed;
  // a formula reading B4 was never calculated again, and a save and open showed other values (doc_fuzz seed 17241693)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("D1", "3"));
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A4:B4"), "D1*2"));
  t.d.Recalc();
  const uint32_t before = t.d.protect_blocks();
  t.d.SetFormula(sh, 3, 0, "1");
  t.d.SetInput(sh, 3, 0, "5");
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == before + 2);
  CHECK(t.d.block_array());
  CHECK(t.F("A4") == "=D1*2");
  CHECK(t.N("A4") == 6);
  CHECK(t.N("B4") == 6);
  // the whole array written over is fine
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A4:B4"), "D1*3"));
  t.d.Recalc();
  CHECK(t.N("B4") == 9);
}

TEST_CASE("the merged cells a fill repeats are an Undo step even when no value changes") {
  // doc_fuzz seed 17480302: C6:C8 holding only the merged C7:C8, filled down to C11, repeated the merge as C10:C11;
  // every pasted cell was already empty, nothing noted the merges, and the workbook changed without an Undo step
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.Merge(sh, yctest::Doc::Range("C7:C8")));
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("Fill");
  t.d.AutoFill(sh, yctest::Doc::Range("C6:C8"), yctest::Doc::Range("C6:C11"), 0);
  t.d.Commit();
  REQUIRE(t.d.S(sh)->MergeAt(9, 2) != nullptr);  // C10:C11
  CHECK(t.d.UndoDepth() == depth + 1);
  REQUIRE(t.d.Undo());
  CHECK(t.d.S(sh)->MergeAt(9, 2) == nullptr);
  CHECK(t.d.S(sh)->MergeAt(6, 2) != nullptr);  // C7:C8 stays
}

TEST_CASE("a table cut onto another sheet's table, then cleared: Undo and Redo give back each version") {
  // doc_fuzz seed 20411091 (cut down): the cut table was kept as a table over the other one, the two fought over B9's
  // name, and Redo of Clear gave back the other. Excel 16 (measured): the cut table goes into the other as cells, one
  // table is left, and what named the cut table is #REF!
  yctest::Doc t;
  const uint32_t s1 = t.sheet();
  const uint32_t s2 = t.d.AddSheet("Data");
  auto step = [&](auto fn) {
    t.d.Begin("step");
    fn();
    t.d.Commit();
    t.d.Recalc();
  };
  step([&] { t.d.Paste(s1, 9, 0, t.d.Copy(s1, Rect{1, 4, 1, 5})); });  // copy E2:F2 to A10
  step([&] { t.d.AddTable(s1, Rect{5, 0, 6, 0}, true, "TableStyleMedium2"); });  // A6:A7
  step([&] { t.d.SetInput(s2, 9, 4, "=OFFSET(A1,1,1)"); });
  step([&] { t.d.Merge(s1, Rect{5, 1, 7, 2}, false, false, false); });  // B6:C8
  step([&] { t.d.SetInput(s2, 7, 0, "apple"); });
  step([&] { t.d.AddTable(s2, Rect{8, 0, 11, 1}, true, "TableStyleMedium2"); });  // A9:B12
  step([&] { t.d.SetInput(s1, 1, 7, "=ROWS(Table1)"); });  // H2 names the table that will be cut
  step([&] { t.d.MoveCells(s1, Rect{5, 0, 6, 0}, s2, 8, 1); });  // cut A6:A7 to Data!B9
  auto tables = [&](uint32_t sh) {
    std::string out;
    for (const auto& x : *t.d.S(sh)->tables) {
      out += x.name + " " + yc::core::RectA1(x.range) + " [";
      for (const auto& c : x.columns) out += c.name + ",";
      out += "] ";
    }
    return out;
  };
  INFO("after the cut: " << tables(s2));
  REQUIRE(t.d.S(s2)->tables->size() == 1);
  CHECK(yc::core::RectA1(t.d.S(s2)->tables->front().range) == "A9:B12");
  CHECK(t.d.S(s1)->tables->empty());
  CHECK(t.F("H2") == "=ROWS(#REF!)");
  const std::string before = Dump(t);
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("clear");
  t.d.ClearContents(s2, Rect::Cell(8, 1));  // B9
  t.d.Commit();
  t.d.Recalc();
  const std::string after = Dump(t);
  const std::string after_tables = tables(s2);
  INFO("after the clear: " << after_tables);
  if (t.d.UndoDepth() > depth) {
    REQUIRE(t.d.Undo());
    t.d.Recalc();
    CHECK(Dump(t) == before);
    REQUIRE(t.d.Redo());
    t.d.Recalc();
    INFO("after Redo: " << tables(s2));
    CHECK(tables(s2) == after_tables);
    CHECK(Dump(t) == after);
  } else {
    CHECK(after == before);
  }
}

TEST_CASE("a merge over any of a table's cells is refused, as in Excel") {
  // doc_fuzz seed 31000041 (its invariants): rows inserted above a table moved it under a later merge, which cut it.
  // Excel 16 refuses Range.Merge over a table's edge and inside it (measured)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("C10", "h1"));
  REQUIRE(t.Set("D10", "h2"));
  REQUIRE(t.d.AddTable(sh, Rect{9, 2, 11, 3}, true, "TableStyleMedium2") != 0);  // C10:D12
  const uint32_t refused = t.d.protect_blocks();
  CHECK_FALSE(t.d.Merge(sh, Rect{8, 3, 10, 3}, false, false, false));  // D9:D11, across its edge
  CHECK_FALSE(t.d.Merge(sh, Rect{10, 2, 10, 3}, false, false, false));  // C11:D11, inside
  CHECK(t.d.protect_blocks() == refused + 2);
  CHECK(t.d.block_table_merge());
  CHECK(t.d.S(sh)->merges->empty());
  CHECK(t.d.Merge(sh, Rect{1, 5, 2, 6}, false, false, false));  // F2:G3, beside it
}

TEST_CASE("merged cells cut or copied onto a table's cells come unmerged, as in Excel") {
  // doc_fuzz seed 32000257 (its invariants): a cut of D7:E9 merged onto a table made a merged area inside it. Excel 16
  // moves or pastes the cells there unmerged (measured), and the table keeps its range
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("C4", "h1"));
  REQUIRE(t.Set("D4", "h2"));
  REQUIRE(t.d.AddTable(sh, Rect{3, 2, 4, 3}, true, "TableStyleMedium2") != 0);  // C4:D5
  REQUIRE(t.Set("G7", "m"));
  REQUIRE(t.d.Merge(sh, Rect{6, 6, 8, 7}, false, false, false));  // G7:H9
  REQUIRE(t.d.MoveCells(sh, Rect{6, 6, 8, 7}, sh, 4, 2));         // cut to C5
  CHECK(t.d.S(sh)->merges->empty());
  CHECK(t.T("C5") == "m");
  CHECK(yc::core::RectA1(t.d.S(sh)->tables->front().range) == "C4:D5");
  REQUIRE(t.Set("K7", "n"));
  REQUIRE(t.d.Merge(sh, Rect{6, 10, 7, 11}, false, false, false));  // K7:L8
  t.d.Paste(sh, 3, 2, t.d.Copy(sh, Rect{6, 10, 7, 11}));            // copy onto C4
  CHECK(t.d.S(sh)->merges->size() == 1);  // K7:L8 itself
}

TEST_CASE("merged cells cut onto part of another merged area undo that one, and a copy there is refused") {
  // doc_fuzz seed 33003351 (its invariants): the cut left two merged areas overlapping. Excel 16 (measured): the cut
  // goes through and undoes the merged area it lands on part of; a copy onto part of one is refused
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A2", "a"));
  REQUIRE(t.d.Merge(sh, Rect{1, 0, 2, 1}, false, false, false));  // A2:B3
  REQUIRE(t.Set("F3", "f"));
  REQUIRE(t.d.Merge(sh, Rect{2, 5, 4, 6}, false, false, false));  // F3:G5
  const auto before = Dump(t);
  t.d.Begin("cut");
  REQUIRE(t.d.MoveCells(sh, Rect{1, 0, 2, 1}, sh, 2, 4));  // to E3
  t.d.Commit();
  REQUIRE(t.d.S(sh)->merges->size() == 1);
  CHECK(yc::core::RectA1(t.d.S(sh)->merges->front()) == "E3:F4");
  CHECK(t.T("E3") == "a");
  REQUIRE(t.d.Undo());
  CHECK(Dump(t) == before);
}

TEST_CASE("rows inserted into a table on whole columns grow the sheet with it; columns past XFD are refused") {
  // doc_fuzz --wide seeds 20680024, 20840077: a row inserted into a table on A:B, or above a merged area reaching the
  // last row, pushed it past the sheet's end without growing the sheet. Rows grow on demand (PLAN 5.1, kept on purpose
  // where Excel refuses, measured): the sheet grows as for cells. Columns stop at XFD: refused, as in Excel (measured)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.Set("A1", "h1"));
  REQUIRE(t.Set("B1", "h2"));
  const uint32_t last = t.d.S(sh)->row_limit - 1;
  REQUIRE(t.d.AddTable(sh, Rect{0, 0, last, 1}, true, "TableStyleMedium2") != 0);  // A:B
  REQUIRE(t.d.Merge(sh, Rect{last - 1, 3, last, 4}, false, false, false));         // D1048575:E1048576
  t.d.InsertRows(sh, 11, 1);
  const auto* s = t.d.S(sh);
  CHECK(s->tables->front().range.r1 == last + 1);
  CHECK(s->merges->front().r1 == last + 1);
  CHECK(s->row_limit > last + 1);
  yctest::Doc u;
  const uint32_t sh2 = u.sheet();
  REQUIRE(u.Set("XFC1", "h1"));
  REQUIRE(u.d.AddTable(sh2, Rect{0, 16382, 4, 16383}, true, "TableStyleMedium2") != 0);  // XFC1:XFD5
  const uint32_t refused = u.d.protect_blocks();
  u.d.InsertCols(sh2, 3, 1);
  CHECK(u.d.protect_blocks() == refused + 1);
  CHECK(u.d.block_table_shift());
}

TEST_CASE("a name over two whole rows spilled from column B doesn't fit the sheet: #SPILL!, the same after Undo and Redo") {
  // doc_fuzz --wide seed 20920008: =Rate*2 in B10 with Rate = A3:XFD4 (2 x 16,384) spilled part of itself, and Redo
  // spilled another part. It runs past XFD from column B: #SPILL! with nothing spilled (Excel 16, measured; from
  // column A it spills)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.AddName("Rate", "=Sheet1!$A$3:$XFD$4"));
  REQUIRE(t.Set("A3", "1"));
  const std::string before = Dump(t);
  t.d.Begin("type");
  REQUIRE(t.d.SetInput(sh, 9, 1, "=Rate*2"));
  t.d.Commit();
  t.d.Recalc();
  const std::string after = Dump(t);
  CHECK(t.T("B10") == "#SPILL!");
  CHECK(t.T("B11").empty());
  CHECK(t.T("C10").empty());
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(Dump(t) == before);
  REQUIRE(t.d.Redo());
  t.d.Recalc();
  CHECK(Dump(t) == after);
  REQUIRE(t.Set("A20", "=Rate*2"));  // from column A it fits
  CHECK(t.T("A20") == "2");
  CHECK(t.T("XFD21") == "0");
}

TEST_CASE("cells or columns inserted that would push a merged area past XFD are refused") {
  // doc_fuzz --wide seed 21200010: cells inserted at B:B with a shift right stretched A7:XFD7 merged to A7:XFE7. Excel 16
  // refuses an insertion that would push merged cells off the sheet (measured)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.Merge(sh, Rect{6, 0, 6, yc::core::kMaxCols - 1}, false, false, false));  // A7:XFD7
  const uint32_t refused = t.d.protect_blocks();
  t.d.InsertCells(sh, Rect{0, 1, t.d.S(sh)->row_limit - 1, 1}, false);  // B:B, shift right
  CHECK(t.d.protect_blocks() == refused + 1);
  CHECK(t.d.block_off_sheet());
  t.d.InsertCols(sh, 3, 1);
  CHECK(t.d.protect_blocks() == refused + 2);
  REQUIRE(t.d.S(sh)->merges->size() == 1);
  CHECK(t.d.S(sh)->merges->front().c1 == yc::core::kMaxCols - 1);
  t.d.InsertCells(sh, Rect{0, 1, 3, 1}, false);  // B1:B4: rows the merged area isn't in
  CHECK(t.d.protect_blocks() == refused + 2);
}

TEST_CASE("Paste Special with an operation over an array formula is refused, and leaves the array whole") {
  // doc_fuzz --more seed 80142: Values and formats, transposed, Divide, from empty cells onto D4:F5, over all of the
  // array E4:F5: its anchor kept the array's formula while its other cells took #DIV/0! as constants, and Redo showed
  // the array's values again
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  REQUIRE(t.d.SetArrayFormula(sh, Rect{3, 4, 4, 5}, "A1:B3*2"));  // E4:F5
  t.d.Recalc();
  const yc::engine::ClipData clip = t.d.Copy(sh, Rect{10, 3, 12, 4});  // D11:E13, empty
  const std::string before = Dump(t);
  const uint32_t refused = t.d.protect_blocks();
  t.d.Begin("paste");
  t.d.Paste(sh, 3, 3, clip, yc::engine::PasteMode::ValuesAndFormats, true, false, yc::engine::PasteOp::Divide);
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == refused + 1);
  CHECK(t.d.block_array());
  CHECK(Dump(t) == before);
  // beside the array the operation works, with Undo and Redo
  t.Set("H4", "6");
  t.Set("J1", "3");
  const yc::engine::ClipData three = t.d.Copy(sh, Rect{0, 9, 0, 9});
  CheckStep(t, [&] { t.d.Paste(sh, 3, 7, three, yc::engine::PasteMode::Values, false, false, yc::engine::PasteOp::Multiply); });
  CHECK(t.N("H4") == 18);
  // so is Skip Blanks over all of an array (doc_fuzz --more seed 22650345: one cell of E4:F5 written, the array kept
  // over the others, and Redo showed the array's value there)
  t.Set("L1", "8");
  const yc::engine::ClipData blanks = t.d.Copy(sh, Rect{0, 10, 1, 11});  // K1:L2: one value among blanks
  const std::string before2 = Dump(t);
  t.d.Begin("paste");
  t.d.Paste(sh, 3, 4, blanks, yc::engine::PasteMode::Formulas, false, true);
  t.d.Commit();
  t.d.Recalc();
  CHECK(t.d.protect_blocks() == refused + 2);
  CHECK(t.d.block_array());
  CHECK(Dump(t) == before2);
}

TEST_CASE("a name before a sheet's reference is written so it reads back the same (Ab:Sheet1!A3)") {
  // doc_fuzz --more seed 24071853: Replace made =A1:A3*2 =Ab:A3*2 (the name Ab to A3), and cut to another sheet it was
  // written =Ab:Sheet1!A3*2, which reads back as the sheets Ab to Sheet1 (#REF!) after a save and an open
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t two = t.d.AddSheet("Two");
  REQUIRE(t.Set("B11", "=A1:A3*2"));
  t.d.Replace(sh, Rect{10, 1, 10, 1}, "1", "b", false, false);
  REQUIRE(t.d.GetFormulaA1(sh, 10, 1) == "=Ab:A3*2");
  REQUIRE(t.d.MoveCells(sh, Rect{10, 1, 10, 1}, two, 7, 2));  // to Two!C8
  const std::string text = t.d.GetFormulaA1(two, 7, 2);
  CHECK(text == "=(Ab):Sheet1!A3*2");
  // the text reads back as the same formula
  REQUIRE(t.d.SetFormula(two, 8, 2, text.substr(1)));
  CHECK(t.d.GetFormulaA1(two, 8, 2) == text);
  t.d.Recalc();
  CHECK(t.d.GetValue(two, 8, 2) == t.d.GetValue(two, 7, 2));
  // what the same reference is without the name stays as it was
  REQUIRE(t.d.SetFormula(two, 9, 2, "SUM(Sheet1!A1:A3)"));
  CHECK(t.d.GetFormulaA1(two, 9, 2) == "=SUM(Sheet1!A1:A3)");
}

TEST_CASE("a whole column in an array formula is as long as its column's data, whatever else the sheet holds") {
  // doc_fuzz --more seed 26213328: =B:B*2 in D5 was cut to the sheet's used range, which its own spill widened; calculated
  // again (Redo) it spilled further
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B1", "1");
  t.Set("B2", "2");
  t.Set("B3", "3");
  CheckStep(t, [&] { REQUIRE(t.Set("D5", "=B:B*2")); });
  t.d.Recalc();
  CHECK(t.N("D7") == 6);
  CHECK(t.T("D8").empty());
  // the sheet's used range grows elsewhere: the spill stays three cells
  t.Set("Z100", "1");
  t.d.RecalcAll();
  t.d.Recalc();
  CHECK(t.T("D8").empty());
  uint32_t ar = 0, ac = 0;
  const yc::model::SpillRange* sp = t.d.S(sh)->SpillAt(4, 3, &ar, &ac);
  REQUIRE(sp);
  CHECK(sp->rect == Rect{4, 3, 6, 3});
}

TEST_CASE("doc_fuzz seed 33240081: a reference an insertion pushes past XFD is #REF!, and cells as wide as the sheet are rows") {
  // a reference pushed past the last column was kept as an offset past XFD: it read #REF! as text, and only a save and
  // an open made it the #REF! that text is (two formulas of one text until then). Excel 16 (measured, edge_insert_
  // cases): =XFD1+1 is =#REF!+1 after column C goes in, =SUM(B1:XFD1) stays; A11:XFD12 inserted to the right (deleted
  // to the left) are rows 11:12
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("J10", "=XFD1+1");
  t.Set("J9", "=SUM(B1:XFD1)");
  t.d.InsertCols(sh, 2, 1);
  CHECK(t.F("K10") == "=#REF!+1");
  CHECK(t.F("K9") == "=SUM(B1:XFD1)");
  const auto* f = t.d.Formula(t.d.S(sh)->GetFormula(9, 10));
  REQUIRE(f);
  CHECK(f->key.find("#REF!") != std::string::npos);  // one formula with what =#REF!+1 typed is
  yctest::Doc u;
  const uint32_t us = u.sheet();
  u.Set("J10", "=B13*2");
  u.Set("J9", "=B11");
  REQUIRE(u.d.InsertCells(us, Rect{10, 0, 11, yc::core::kMaxCols - 1}, false));  // A11:XFD12 right: rows 11:12 go in
  CHECK(u.F("J10") == "=B15*2");
  CHECK(u.F("J9") == "=B13");
  u.d.DeleteCells(us, Rect{10, 0, 11, yc::core::kMaxCols - 1}, false);  // and back, as rows
  CHECK(u.F("J10") == "=B13*2");
  CHECK(u.F("J9") == "=B11");
}

TEST_CASE("doc_fuzz --xlsx seed 34112022: a name read through a deleted sheet is what its text reads back as") {
  // Replace made Data!A:A3 of Data!A1:A3 (Data!A is a name of the sheet, never defined: #NAME?, Excel 16 measured);
  // Data deleted, its text read #REF!:A3 while it kept #NAME? until a save and an open gave #REF!
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  const uint32_t data = t.d.AddSheet("Data");
  t.d.RenameSheet(sh, "Sheet1");
  t.Set("A3", "3");
  t.Set("B2", "=SUM(Data!A:A3)");
  t.d.Recalc();
  CHECK(t.T("B2") == "#NAME?");
  REQUIRE(t.d.DeleteSheet(data));
  t.d.Recalc();
  CHECK(t.F("B2") == "=SUM(#REF!:A3)");
  CHECK(t.T("B2") == "#REF!");
}

TEST_CASE("doc_fuzz --xlsx --more seed 35490977: the parentheses the writer puts around a name before a sheet's reference aren't kept") {
  // =(A):Data!E13*2 is written so (A:Data!E13 would read as a range of sheets); read back, the parentheses made it
  // another formula than the one written, one text two formulas until a save and an open
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.d.AddSheet("Data");
  for (const char* text : {"=(A):Data!E13*2", "=(A1):Data!B2", "=SUM((Foo):Data!A1:A3)"}) {
    t.Set("B1", text);
    CHECK(t.F("B1") == text);
    const auto* f = t.d.Formula(t.d.S(sh)->GetFormula(0, 1));
    REQUIRE(f);
    INFO(f->key);
    // no Paren node around the name or cell: a name's key ends with ', a Paren's with ): "(N'A'):..." / "(R[..]C[..]):..."
    CHECK(f->key.find("')") == std::string::npos);
    CHECK(f->key.rfind("(R", 0) == std::string::npos);
  }
  t.Set("B2", "=(A1):B2");  // parentheses the writer wouldn't put back are the formula's
  CHECK(t.F("B2") == "=(A1):B2");
}

TEST_CASE("doc_fuzz --gaps seed 700022: Undo on a sheet whose calculation is off gives the formulas their values back") {
  // a formula entered there shows 0 (Excel 16); one Undo puts back, moved by inserted cells, keeps the value it had
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "5");
  t.Set("B1", "=A1*2");
  REQUIRE(t.N("B1") == 10);
  t.d.SetSheetCalculation(sh, false);
  t.Set("C1", "=A1+1");
  CHECK(t.N("C1") == 0);  // entered: 0 until the sheet calculates
  const size_t depth = t.d.UndoDepth();
  t.d.Begin("insert");
  REQUIRE(t.d.InsertCells(sh, Rect{0, 0, 0, 0}, false));  // shift right
  t.d.Commit();
  t.d.Recalc();
  REQUIRE(t.d.UndoDepth() > depth);
  CHECK(t.N("C1") == 10);  // =B1*2 moved right keeps its value
  REQUIRE(t.d.Undo());
  t.d.Recalc();
  CHECK(t.F("B1") == "=A1*2");
  CHECK(t.N("B1") == 10);  // put back with its value, not a typed formula's 0
  t.d.SetSheetCalculation(sh, true);
  CHECK(t.N("C1") == 6);
}

TEST_CASE("doc_fuzz --gaps seed 26: an array reading its own cells and another circular reference is still circular") {
  // its values were settled one by one, and the read of A1, a cell another cycle blocks, ended the process (NeedCell
  // isn't a std::exception); the array waits for that cycle, as before
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("B2", "10");
  t.Set("B3", "20");
  t.Set("A1", "=A1+1");
  REQUIRE_FALSE(t.d.circular_cells.empty());
  std::string err;
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("C2:C3"), "B2:C3*2+A1", &err));
  t.d.Recalc();
  CHECK_FALSE(t.d.circular_cells.empty());
  CHECK(t.N("C2") == 0);
  t.Set("A2", "=SUM(C2:C3)");  // what reads it waits too
  CHECK(t.N("A2") == 0);
}

TEST_CASE("doc_fuzz --xlsx seed 24000181430: an array value that reads a formula entered later is calculated") {
  // C2:D2 {=A1:B3*2} is A1*2, B1*2; B1's VLOOKUP reads A1:C9, which holds C2, but C2's value doesn't read B1, so no
  // value waits for itself: D2 follows B1 (#N/A). After the step's Redo it kept 0 until a full calculation (Undo of
  // the next step) gave #N/A, so Undo didn't give back what was there
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::string err;
  t.d.ClearUndo();
  CheckStep(t, [&] { REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("B3:B5"), "A1:B3*2", &err)); });
  CheckStep(t, [&] { REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("C2:D2"), "A1:B3*2", &err)); });
  CheckStep(t, [&] { REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("E6:F8"), "A1:B3*2", &err)); });
  CheckStep(t, [&] { t.d.SetInput(sh, 0, 1, "=VLOOKUP(A1,A1:C9,2,FALSE)"); });
  CHECK(t.T("B1") == "#N/A");
  CHECK(t.N("C2") == 0);
  CHECK(t.T("D2") == "#N/A");
  CHECK(t.T("F6") == "#N/A");
  CheckStep(t, [&] { t.d.ModifyStyle(sh, yctest::Doc::Range("A1"), [](model::Style& x) { x.bold = true; }); });
}


TEST_CASE("doc_fuzz --xlsx seed 24002581569: a formula filled beside an array follows a cell the next array writes") {
  // B3 {=A1:B3*2} (one cell: A1*2), A4 177, C1 =SUMIF(A1:A10,">2") filled to C4 (=SUMIF(A4:A13,">2")), then
  // A2:A4 {=A1:B3*2} wrote 0 over A4: C4 kept 177 (only a full calculation gave 0)
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  std::string err;
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("B3"), "A1:B3*2", &err));
  t.Set("A4", "177");
  t.Set("C1", "=SUMIF(A1:A10,\">2\")");
  t.d.AutoFill(sh, yctest::Doc::Range("C1:D3"), yctest::Doc::Range("C1:D6"));
  t.d.Recalc();
  CHECK(t.N("C4") == 177);
  REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("A2:A4"), "A1:B3*2", &err));
  t.d.Recalc();
  CHECK(t.N("A4") == 0);
  CHECK(t.N("C4") == 0);
}


TEST_CASE("doc_fuzz --xlsx seed 24004181633: a formula under a table doesn't make a totals row over another table") {
  // a formula in the row under a table makes that row its totals row (Excel 16, measured), but the row was the next
  // table's header: the first table took it and the two overlapped
  yctest::Doc t;
  t.Set("A1", "Qty");
  t.Set("A2", "5");
  t.Set("A3", "Px");
  t.Set("A4", "7");
  std::string err;
  const uint32_t first = t.d.AddTable(t.sheet(), Rect{0, 0, 1, 0}, true, "", &err);
  const uint32_t second = t.d.AddTable(t.sheet(), Rect{2, 0, 3, 0}, true, "", &err);
  REQUIRE(first);
  REQUIRE(second);
  t.Set("A3", "=1+1");
  t.d.Recalc();
  CHECK(t.d.TableById(t.sheet(), first)->range == Rect{0, 0, 1, 0});
  CHECK_FALSE(t.d.TableById(t.sheet(), first)->totals);
  CHECK(t.d.TableById(t.sheet(), second)->range == Rect{2, 0, 3, 0});
}


TEST_CASE("doc_fuzz --xlsx seed 2400534: an array reads a lookup that never reads the cells it spills over") {
  // B1 {=A1:B2*2} reads its own cell but its value doesn't (A1*2): settled, no cycle. B4 =TRANSPOSE(A1:B2) reads B1
  // and B2 =VLOOKUP(#REF!,A1:C7,2,FALSE), whose table holds B4's spill, but VLOOKUP stops at its error before it reads
  // the table: no cycle, B4 spills (Excel 16, measured 2026-11-28). Each pass met B1 unsettled, set B4 aside, and the
  // next pass marked B1 again through B2: B4 showed 0 unspilled until Undo of the next step calculated it again.
  // (A sort made the two formulas; header or not, they land one row apart.)
  for (const bool header : {true, false}) {
    yctest::Doc t;
    const uint32_t sh = t.sheet();
    std::string err;
    CheckStep(t, [&] { REQUIRE(t.d.SetArrayFormula(sh, yctest::Doc::Range("B2"), "A1:B3*2", &err)); });
    CheckStep(t, [&] { t.d.SetInput(sh, 3, 1, "=VLOOKUP(A1,A1:C9,2,FALSE)"); });
    CheckStep(t, [&] { t.d.Sort(sh, yctest::Doc::Range("B1:B4"), {yc::engine::SortKey{1}}, header); });
    CheckStep(t, [&] { t.d.SetInput(sh, 3, 1, "=TRANSPOSE(A1:B2)"); });
    std::string cyc;
    for (const uint64_t k : t.d.circular_cells) cyc += yc::core::CellA1(yc::engine::KeyRow(k), yc::engine::KeyCol(k)) + " ";
    INFO("circular: " << cyc << "\n" << Dump(t));
    CHECK(t.d.circular_cells.empty());
    const model::SpillRange* sp = t.d.S(sh)->SpillOf(3, 1);
    REQUIRE(sp);
    CHECK(yc::core::RectA1(sp->rect) == "B4:C5");
    CHECK(t.T("C5") == (header ? "0" : "#REF!"));
  }
  // =SUM(A1:C7) there reads the spill: a cycle, and B4 shows 0 without spilling (measured)
  yctest::Doc u;
  u.Set("A1", "1");
  u.Set("B2", "=SUM(A1:C7)");
  u.Set("B4", "=TRANSPOSE(A1:B2)");
  u.d.Recalc();
  CHECK_FALSE(u.d.circular_cells.empty());
  CHECK(u.N("B4") == 0);
  CHECK(u.T("C4").empty());
}
