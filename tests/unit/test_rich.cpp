// SPDX-License-Identifier: Apache-2.0
// fonts inside a cell's text (Characters().Font), superscript / subscript, rotated text styles
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/xlsx.h"
#include "io/ycg.h"
#include "model/rich_text.h"

namespace model = yc::model;
namespace io = yc::io;

namespace {

model::RichText Plain(const std::string& s) { return model::RichText{s, {}}; }
bool BoldAt(const model::RichText& rt, size_t ch) {
  const model::RichRun* r = model::RunAt(rt, ch);
  return r && r->bold == 1;
}

}  // namespace

TEST_CASE("rich text: applying a font to characters splits and merges runs") {
  model::RichText rt = Plain("Hello world");
  model::ApplyToChars(&rt, 6, 5, [](model::RichRun& r) { r.bold = 1; });
  REQUIRE(rt.runs.size() == 2);
  CHECK(rt.runs[0].start == 0);
  CHECK(rt.runs[1].start == 6);
  CHECK_FALSE(BoldAt(rt, 5));
  CHECK(BoldAt(rt, 6));
  // the neighbors become equal again: one run
  model::ApplyToChars(&rt, 0, 6, [](model::RichRun& r) { r.bold = 1; });
  CHECK(rt.runs.size() == 1);
  CHECK(BoldAt(rt, 0));
  // characters, not bytes
  model::RichText cjk = Plain("\xE4\xB8\xAD\xE6\x96\x87 ok");  // two CJK characters, space, "ok"
  model::ApplyToChars(&cjk, 1, 1, [](model::RichRun& r) { r.script = 1; });
  CHECK(model::RunAt(cjk, 1)->script == 1);
  CHECK(model::RunAt(cjk, 2)->script != 1);
  CHECK(model::ByteOfChar(cjk.text, 2) == 6);
}

TEST_CASE("rich text: replacing and editing keep the fonts in place") {
  model::RichText rt = Plain("Total: 100 units");
  model::ApplyToChars(&rt, 7, 3, [](model::RichRun& r) { r.italic = 1; });
  const model::RichText rep = model::ReplaceChars(rt, 7, 3, "2500");
  CHECK(rep.text == "Total: 2500 units");
  CHECK(model::RunAt(rep, 7)->italic == 1);
  CHECK(model::RunAt(rep, 10)->italic == 1);
  CHECK(model::RunAt(rep, 11)->italic != 1);
  // typed text continues the font before it
  model::RichText b = Plain("ab");
  model::ApplyToChars(&b, 1, 1, [](model::RichRun& r) { r.bold = 1; });
  const model::RichText ed = model::RemapRuns(b, "abXY");
  CHECK(BoldAt(ed, 3));
  CHECK_FALSE(BoldAt(ed, 0));
  const model::RichText front = model::RemapRuns(b, "Zab");
  CHECK_FALSE(BoldAt(front, 1));
  CHECK(BoldAt(front, 2));
}

TEST_CASE("rich text: layout of runs, line breaks and wrapping") {
  model::RichText rt = Plain("aa bb\ncc");
  model::ApplyToChars(&rt, 3, 2, [](model::RichRun& r) { r.size = 20; });
  auto measure = [&](const std::string& t, size_t run) {
    const float h = rt.runs.empty() || rt.runs[run].size == 0 ? 10.0f : 20.0f;
    return std::pair<float, float>(static_cast<float>(t.size()) * 5.0f, h);
  };
  const model::RichLayout l = model::LayoutRich(rt, false, 0, 0, 10, measure);
  REQUIRE(l.pieces.size() == 3);
  CHECK(l.height == 30);       // a 20-high line and a 10-high line
  CHECK(l.pieces[2].y == 20);  // "cc" on the second line
  CHECK(l.pieces[0].y == 10);  // "aa " sits on the bottom of the taller line
  const model::RichLayout w = model::LayoutRich(model::RichText{"aaaa bbbb", {}}, true, 30, 0, 10,
                                                [](const std::string& t, size_t) { return std::pair<float, float>(static_cast<float>(t.size()) * 5.0f, 10.0f); });
  CHECK(w.height == 20);  // wrapped onto two lines
}

TEST_CASE("rich text: documents keep fonts through copy, row inserts, clearing and saving") {
  yctest::Doc t;
  t.Set("A1", "H2O");
  const uint32_t sh = t.sheet();
  model::RichText rt = Plain("H2O");
  model::ApplyToChars(&rt, 1, 1, [](model::RichRun& r) { r.script = 2; });
  t.d.SetRichText(sh, 0, 0, rt);
  REQUIRE(t.d.RichTextAt(sh, 0, 0));
  // a different value makes the fonts stale
  t.Set("B1", "x");
  t.d.SetRichText(sh, 0, 1, model::RichText{"x", {model::RichRun{}}});
  CHECK_FALSE(t.d.RichTextAt(sh, 0, 1));  // nothing changed: no entry
  // copy / paste
  t.d.Paste(sh, 4, 0, t.d.Copy(sh, yc::core::Rect::Cell(0, 0)));
  REQUIRE(t.d.RichTextAt(sh, 4, 0));
  CHECK(model::RunAt(*t.d.RichTextAt(sh, 4, 0), 1)->script == 2);
  // rows inserted above move it
  t.d.InsertRows(sh, 0, 2);
  CHECK(t.d.RichTextAt(sh, 2, 0));
  CHECK_FALSE(t.d.RichTextAt(sh, 0, 0));
  // saving and loading
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& rich = *res.wb.sheets[0]->rich;
  REQUIRE(rich.count((uint64_t{2} << 16) | 0));
  CHECK(rich.at((uint64_t{2} << 16) | 0) == *t.d.RichTextAt(sh, 2, 0));
  // Clear Contents removes the fonts with the text
  t.d.ClearContents(sh, yc::core::Rect::Cell(2, 0));
  t.Set("A3", "H2O");
  CHECK_FALSE(t.d.RichTextAt(sh, 2, 0));
}

TEST_CASE("rich text: superscript styles and runs survive .xlsx") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "E=mc2");
  model::RichText rt = Plain("E=mc2");
  model::ApplyToChars(&rt, 4, 1, [](model::RichRun& r) { r.script = 1; });
  model::ApplyToChars(&rt, 0, 1, [](model::RichRun& r) {
    r.bold = 1;
    r.color = model::Rgb(200, 0, 0);
  });
  t.d.SetRichText(sh, 0, 0, rt);
  t.Set("A2", "x");
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(1, 0), [](model::Style& s) {
    s.script = 2;
    s.rotation = 45;
  });
  std::string err;
  model::Workbook wb;
  REQUIRE(io::ImportXlsxBytes(io::ExportXlsxBytes(t.d.wb()), &wb, &err, nullptr));
  const auto& s0 = *wb.sheets[0];
  auto it = s0.rich->find(0);
  REQUIRE(it != s0.rich->end());
  CHECK(it->second.text == "E=mc2");
  CHECK(model::RunAt(it->second, 4)->script == 1);
  CHECK(model::RunAt(it->second, 0)->bold == 1);
  CHECK(model::RunAt(it->second, 0)->color == model::Rgb(200, 0, 0));
  CHECK(model::RunAt(it->second, 2)->bold == 0);
  const model::Style& st = wb.styles->Get(s0.GetStyle(1, 0));
  CHECK(st.script == 2);
  CHECK(st.rotation == 45);
  // and .ycg
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(res.wb.styles->Get(res.wb.sheets[0]->GetStyle(1, 0)).script == 2);
}

TEST_CASE("rich text: a font set on the cell is set for every character, as in Excel") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "abc");
  model::RichText rt = Plain("abc");
  model::ApplyToChars(&rt, 1, 1, [](model::RichRun& r) {
    r.bold = 1;
    r.color = model::Rgb(200, 0, 0);
  });
  t.d.SetRichText(sh, 0, 0, rt);
  // Bold on the cell (Ctrl+B, Range.Font.Bold = True), then off: no character stays bold; the color stays
  t.d.Begin("bold");
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](model::Style& s) { s.bold = true; });
  t.d.Commit();
  CHECK(model::RunAt(*t.d.RichTextAt(sh, 0, 0), 1)->bold == -1);
  REQUIRE(t.d.Undo());  // Undo gives the character its bold back
  CHECK(model::RunAt(*t.d.RichTextAt(sh, 0, 0), 1)->bold == 1);
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](model::Style& s) { s.bold = true; });
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](model::Style& s) { s.bold = false; });
  const model::RichText* now = t.d.RichTextAt(sh, 0, 0);
  REQUIRE(now);
  CHECK(model::RunAt(*now, 1)->bold == -1);
  CHECK(model::RunAt(*now, 1)->color == model::Rgb(200, 0, 0));
  // the cell already bold and one character not: Bold on the cell makes them all bold
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](model::Style& s) { s.bold = true; });
  model::RichText r2 = *t.d.RichTextAt(sh, 0, 0);
  model::ApplyToChars(&r2, 0, 1, [](model::RichRun& r) { r.bold = 0; });
  t.d.SetRichText(sh, 0, 0, r2);
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](model::Style& s) { s.bold = true; });
  CHECK(model::RunAt(*t.d.RichTextAt(sh, 0, 0), 0)->bold == -1);
  // a toggle seen per cell, and the color too: nothing of its own is left
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 0), [](model::Style& s) { s.font_color = model::Rgb(0, 0, 200); });
  CHECK_FALSE(t.d.RichTextAt(sh, 0, 0));
  // a whole column's format reaches the characters too
  t.Set("B1", "xy");
  model::RichText r3 = Plain("xy");
  model::ApplyToChars(&r3, 0, 1, [](model::RichRun& r) { r.italic = 1; });
  t.d.SetRichText(sh, 0, 1, r3);
  t.d.ModifyColStyle(sh, 1, 1, [](model::Style& s) { s.italic = false; });
  CHECK_FALSE(t.d.RichTextAt(sh, 0, 1));
  // a format that isn't a font leaves the characters alone
  t.Set("C1", "pq");
  model::RichText r4 = Plain("pq");
  model::ApplyToChars(&r4, 1, 1, [](model::RichRun& r) { r.bold = 1; });
  t.d.SetRichText(sh, 0, 2, r4);
  t.d.ModifyStyle(sh, yc::core::Rect::Cell(0, 2), [](model::Style& s) { s.fill_color = model::Rgb(255, 255, 0); });
  REQUIRE(t.d.RichTextAt(sh, 0, 2));
  CHECK(model::RunAt(*t.d.RichTextAt(sh, 0, 2), 1)->bold == 1);
}

TEST_CASE("rich text: a sort moves the fonts inside a text with its row") {
  // each sorted line is pasted, and the paste erased the fonts under it: H2O sorted from A3 to A1 lost its subscript
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "z");
  t.Set("A2", "y");
  t.Set("A3", "H2O");
  model::RichText rt = Plain("H2O");
  model::ApplyToChars(&rt, 1, 1, [](model::RichRun& r) { r.script = 2; });
  t.d.SetRichText(sh, 2, 0, rt);
  t.d.Sort(sh, yc::core::Rect{0, 0, 2, 0}, {yc::engine::SortKey{0}}, false);
  REQUIRE(t.V("A1").str() == "H2O");
  REQUIRE(t.d.RichTextAt(sh, 0, 0));
  CHECK(model::RunAt(*t.d.RichTextAt(sh, 0, 0), 1)->script == 2);
  CHECK_FALSE(t.d.RichTextAt(sh, 2, 0));
}
