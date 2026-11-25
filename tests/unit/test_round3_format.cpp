// SPDX-License-Identifier: Apache-2.0
// Round 3, the Home tab's formatting: engine and model parts of the fixes
#include <catch2/catch_test_macros.hpp>

#include "engine/format_match.h"
#include "engine/rule_eval.h"
#include "io/ycg.h"
#include "model/table_style.h"
#include "model/theme.h"
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
const model::Style& StyleOf(yctest::Doc& t, const char* a1) {
  const auto p = yctest::Doc::P(a1);
  return t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(p.row, p.col));
}
}  // namespace

TEST_CASE("Round 3 format: Justify distributed, Text direction and From corner survive a save (style keys 26, 27)") {
  yctest::Doc t;
  t.Set("A1", "x");
  t.d.ModifyStyle(t.sheet(), R("A1"), [](model::Style& x) {
    x.halign = model::HAlign::Distributed;
    x.justify_last_line = true;
    x.reading_order = 2;
    x.gradient = model::FillGradient::FromCorner;
    x.gradient_angle = 3;
    x.fill_color = 0xFFFF0000u;
  });
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& sh = *res.wb.sheets[0];
  const model::Style& st = res.wb.styles->Get(sh.GetStyle(0, 0));
  CHECK(st.justify_last_line);
  CHECK(st.reading_order == 2);
  CHECK(st.gradient == model::FillGradient::FromCorner);
  CHECK(st.gradient_angle == 3);
}

TEST_CASE("Round 3 format: Find and Replace by format (Find Format / Replace Format)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "apple");
  t.Set("A2", "apple");
  t.Set("A3", "pear");
  t.d.ModifyStyle(sh, R("A2:A3"), [](model::Style& x) { x.bold = true; });
  engine::FormatSpec bold;
  bold.style.bold = true;
  bold.mask = engine::kFmBold;
  uint32_t r = 0, c = 0;
  // the text and the format: only A2
  REQUIRE(t.d.Find(sh, R("A1:A10"), "apple", 9, 0, true, false, false, true, &r, &c, &bold));
  CHECK(r == 1);
  // the format alone: A2 and A3
  CHECK(t.d.FindAll(sh, R("A1:A10"), "", true, false, false, true, 100, &bold).size() == 2);
  // Replace: bold cells get a red fill, their text stays
  engine::FormatSpec red;
  red.style.fill_color = 0xFFFF0000u;
  red.mask = engine::kFmFill;
  CHECK(t.d.Replace(sh, R("A1:A10"), "", "", false, false, &bold, &red) == 2);
  CHECK(StyleOf(t, "A2").fill_color == 0xFFFF0000u);
  CHECK(StyleOf(t, "A3").fill_color == 0xFFFF0000u);
  CHECK(StyleOf(t, "A1").fill_color == 0u);
  CHECK(std::string(t.V("A3").str()) == "pear");
  // a number format is compared by its code
  engine::FormatSpec pct;
  pct.numfmt = "0%";
  pct.mask = engine::kFmNumber;
  CHECK(engine::FormatMatches(StyleOf(t, "A1"), *t.d.wb().styles, pct) == false);
  CHECK(engine::DiffMask(model::Style{}, StyleOf(t, "A2")) == (engine::kFmBold | engine::kFmFill));
}

TEST_CASE("Round 3 format: Paste Special Comments and Notes, Validation, All merging conditional formats") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "5");
  t.d.SetNote(sh, 0, 0, "me", "a note");
  model::DvRule dv;
  dv.type = model::DvType::WholeNumber;
  dv.op = model::CmpOp::Greater;
  dv.formula1 = "0";
  t.d.SetValidation(sh, R("A1"), dv);
  engine::ClipData clip = t.d.Copy(sh, R("A1"));
  clip.carry_notes = true;
  // Comments and Notes: the note only
  t.d.Paste(sh, 0, 1, clip, engine::PasteMode::Comments);
  CHECK(t.V("B1").IsEmpty());
  CHECK(t.d.S(sh)->notes->count((uint64_t{0} << 16) | 1) == 1);
  // Validation: the rule only
  t.d.Paste(sh, 0, 2, clip, engine::PasteMode::Validation);
  CHECK(t.V("C1").IsEmpty());
  CHECK(engine::FindValidation(*t.d.S(sh), 0, 2) != nullptr);
  CHECK(t.d.S(sh)->notes->count((uint64_t{0} << 16) | 2) == 0);
  // All (the clipboard's copy): value, note and validation
  t.d.Paste(sh, 0, 3, clip, engine::PasteMode::All);
  CHECK(t.N("D1") == 5);
  CHECK(t.d.S(sh)->notes->count((uint64_t{0} << 16) | 3) == 1);
  CHECK(engine::FindValidation(*t.d.S(sh), 0, 3) != nullptr);
  // All merging conditional formats: the destination's rule stays next to the copied one
  model::CfRule mine;
  mine.ranges = {R("F1")};
  mine.type = model::CfType::Blanks;
  t.d.AddCondFormat(sh, mine);
  model::CfRule src;
  src.ranges = {R("A1")};
  src.type = model::CfType::Errors;
  t.d.AddCondFormat(sh, src);
  clip = t.d.Copy(sh, R("A1"));
  t.d.Paste(sh, 0, 5, clip, engine::PasteMode::AllMergingCf);
  int on_f1 = 0;
  for (const auto& rule : *t.d.S(sh)->cond_formats) {
    for (const Rect& x : rule.ranges) on_f1 += x.Contains(0, 5) ? 1 : 0;
  }
  CHECK(on_f1 == 2);
  // plain All replaces it
  t.d.Paste(sh, 0, 6, clip, engine::PasteMode::All);
  t.d.Paste(sh, 0, 5, t.d.Copy(sh, R("G1")), engine::PasteMode::All);
  on_f1 = 0;
  for (const auto& rule : *t.d.S(sh)->cond_formats) {
    for (const Rect& x : rule.ranges) on_f1 += x.Contains(0, 5) ? 1 : 0;
  }
  CHECK(on_f1 == 1);
}

TEST_CASE("Round 3 format: Excel's 60 built-in table styles and a workbook's own (New Table Style)") {
  int light = 0, medium = 0, dark = 0;
  for (const auto& d : model::BuiltinTableStyles()) {
    light += d.group == 1;
    medium += d.group == 2;
    dark += d.group == 3;
  }
  CHECK(light == 21);
  CHECK(medium == 28);
  CHECK(dark == 11);
  CHECK(model::TableStyleLabel(*model::BuiltinTableStyle("TableStyleMedium2")) == "Blue, Table Style Medium 2");
  // Medium 2: a blue header with white bold text, light blue stripes
  model::Table t;
  t.range = R("A1:C5");
  const model::TableStyleDef& m2 = *model::BuiltinTableStyle("TableStyleMedium2");
  model::Style h, band, plain;
  model::ApplyTableStyle(t, m2, 0, 0, &h);
  model::ApplyTableStyle(t, m2, 1, 0, &band);
  model::ApplyTableStyle(t, m2, 2, 0, &plain);
  CHECK(model::ResolveColor(h.fill_color) == 0xFF4472C4u);
  CHECK(h.bold);
  CHECK(model::ResolveColor(band.fill_color) != 0u);
  CHECK(plain.fill_color == 0u);
  CHECK(band.border[model::kBottom].style == model::LineStyle::Thin);  // lines between the rows
  // a style of the workbook's own, kept in the store; it wins over a built-in name lookup
  model::TableStyleDef mine;
  mine.name = "My Style";
  mine.el[model::kTeHeaderRow].fill = 0xFF00FF00u;
  mine.el[model::kTeWholeTable].top = model::BorderSide{model::LineStyle::Thick, 0xFF000000u};
  std::map<std::string, std::string> store;
  store[model::kTableStylesKey] = model::EncodeTableStyles({mine});
  const auto back = model::DecodeTableStyles(store[model::kTableStylesKey]);
  REQUIRE(back.size() == 1);
  CHECK(back[0].name == "My Style");
  CHECK(back[0].el[model::kTeHeaderRow] == mine.el[model::kTeHeaderRow]);
  const model::TableStyleDef* found = model::FindTableStyle(store, "My Style");
  REQUIRE(found);
  model::Style x;
  model::ApplyTableStyle(t, *found, 0, 1, &x);
  CHECK(x.fill_color == 0xFF00FF00u);
  CHECK(x.border[model::kTop].style == model::LineStyle::Thick);
  CHECK(model::FindTableStyle(store, "TableStyleNone") == nullptr);
  // the cell's own fill wins
  model::Style own;
  own.fill_color = 0xFF123456u;
  model::ApplyTableStyle(t, m2, 0, 0, &own);
  CHECK(own.fill_color == 0xFF123456u);
}

TEST_CASE("Round 3 format: Convert to Range keeps the style's whole look") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "h1");
  t.Set("B1", "h2");
  t.Set("A2", "1");
  t.Set("A3", "2");
  std::string err;
  const uint32_t id = t.d.AddTable(sh, R("A1:B3"), true, "TableStyleLight9", &err);
  REQUIRE(id);
  REQUIRE(t.d.DeleteTable(sh, id, true));
  CHECK(StyleOf(t, "A1").bold);
  CHECK(StyleOf(t, "A1").fill_color != 0u);                                 // Light 9: a filled header
  CHECK(StyleOf(t, "A1").border[model::kTop].style == model::LineStyle::Thin);  // its outline
}

TEST_CASE("Round 3 format: a rule's number format, underline, pattern, lines and data bar options (rule keys 35-38)") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "0.5");
  model::CfRule r;
  r.ranges = {R("A1")};
  r.type = model::CfType::Expression;
  r.formula1 = "TRUE";
  r.fmt.numfmt = "0.0%";
  r.fmt.underline = 1;
  r.fmt.pattern = 3;
  r.fmt.pattern_color = 0xFF00FF00u;
  r.fmt.side_style[3] = static_cast<uint8_t>(model::LineStyle::Thick);
  r.fmt.side_color[3] = 0xFF0000FFu;
  t.d.AddCondFormat(sh, r);
  // the code is in the style table: the renderer finds it by id
  CHECK(t.d.wb().styles->FindString("0.0%") != 0u);
  engine::CondFormatEval ev(t.d.wb(), sh);
  engine::CfVisual v;
  REQUIRE(ev.Eval(0, 0, &v));
  CHECK(v.numfmt == "0.0%");
  CHECK(v.underline == 1);
  CHECK(v.pattern == 3);
  CHECK(v.side_style[3] == static_cast<uint8_t>(model::LineStyle::Thick));
  model::CfRule bar;
  bar.ranges = {R("B1:B4")};
  bar.type = model::CfType::DataBar;
  bar.bar_gradient = false;
  bar.bar_show_value = false;
  bar.bar_border = 0xFF112233u;
  bar.bar_direction = 2;
  bar.bar_min_type = 0;
  bar.bar_min_value = 10;
  bar.bar_max_type = 0;
  bar.bar_max_value = 20;
  for (int i = 0; i < 4; ++i) t.Set(("B" + std::to_string(i + 1)).c_str(), std::to_string(10 + i * 5));
  t.d.AddCondFormat(sh, bar);
  engine::CondFormatEval ev2(t.d.wb(), sh);
  engine::CfVisual b2;
  REQUIRE(ev2.Eval(1, 1, &b2));  // 15: half way from 10 to 20
  CHECK(b2.bar > 0.45f);
  CHECK(b2.bar < 0.55f);
  CHECK(!b2.bar_gradient);
  CHECK(b2.bar_only);
  CHECK(b2.bar_rtl);
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  const auto& rules = *res.wb.sheets[0]->cond_formats;
  REQUIRE(rules.size() == 2);
  for (const auto& x : rules) {
    for (const auto& y : *t.d.S(sh)->cond_formats) {
      if (x.id == y.id) CHECK(x == y);
    }
  }
  CHECK(res.wb.styles->FindString("0.0%") != 0u);
}
