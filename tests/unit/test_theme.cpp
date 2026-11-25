// SPDX-License-Identifier: Apache-2.0
// document themes, theme colors and named cell styles
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "io/xlsx.h"
#include "io/ycg.h"
#include "model/theme.h"

#include <cstdlib>

namespace model = yc::model;
namespace io = yc::io;
using yc::core::Rect;

namespace {

bool Near(uint32_t a, uint32_t b) {
  for (int sh : {0, 8, 16}) {
    if (std::abs(static_cast<int>((a >> sh) & 0xFF) - static_cast<int>((b >> sh) & 0xFF)) > 2) return false;
  }
  return true;
}

const model::Style& StyleAt(yctest::Doc& t, uint32_t r, uint32_t c) { return t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(r, c)); }

}  // namespace

TEST_CASE("theme: tints and theme colors resolve like Excel's palette") {
  // "Blue, Accent 1, Lighter 80%" and "Darker 50%" of the Office theme
  CHECK(Near(model::ApplyTint(0x4472C4, 0.8), 0xDAE3F3));
  CHECK(Near(model::ApplyTint(0x4472C4, -0.5), 0x203864));
  CHECK(model::ApplyTint(0x123456, 0) == 0x123456);
  const uint32_t c = model::MakeThemeColor(model::kAccent2, -0.25);
  CHECK(model::IsThemeColor(c));
  CHECK(model::ThemeColorIndex(c) == model::kAccent2);
  CHECK(model::ThemeColorTint(c) == -0.25);
  const model::Theme office = model::DefaultTheme();
  CHECK(model::ResolveColor(model::MakeThemeColor(model::kAccent1), office) == 0xFF4472C4u);
  CHECK(model::ResolveColor(0xFF112233u, office) == 0xFF112233u);  // fixed colors stay
  CHECK(model::ResolveColor(0, office) == 0);                      // automatic stays
}

TEST_CASE("theme: changing it keeps theme colors linked and sets the body font") {
  yctest::Doc t;
  t.Set("A1", "x");
  t.d.ModifyStyle(t.sheet(), Rect::Cell(0, 0), [](model::Style& s) { s.fill_color = model::MakeThemeColor(model::kAccent1); });
  model::Theme th = model::BuiltinThemes()[1];  // Office 2007 - 2010
  t.d.SetTheme(th);
  CHECK(model::ResolveColor(StyleAt(t, 0, 0).fill_color, *t.d.wb().theme) == 0xFF4F81BDu);
  CHECK(t.d.wb().styles->default_font == "Calibri");
  th.minor_font = "Arial";
  t.d.SetTheme(th);
  CHECK(t.d.wb().styles->default_font == "Arial");
  // saved and loaded
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  CHECK(*res.wb.theme == th);
}

TEST_CASE("cell styles: applying, modifying and deleting named styles") {
  yctest::Doc t;
  const uint32_t sh = t.sheet();
  t.Set("A1", "good");
  REQUIRE(t.d.ApplyCellStyle(sh, Rect::Cell(0, 0), "good"));  // names are case-insensitive
  CHECK(t.d.wb().styles->Str(StyleAt(t, 0, 0).cell_style) == "Good");
  CHECK(StyleAt(t, 0, 0).fill_color == model::Rgb(0xC6, 0xEF, 0xCE));
  // a style that only includes the font and fill keeps the number format
  t.d.ModifyStyle(sh, Rect::Cell(1, 0), [&](model::Style& s) { s.numfmt = t.d.wb_mut().styles.mut().InternString("0.00"); });
  t.d.ApplyCellStyle(sh, Rect::Cell(1, 0), "Bad");
  CHECK(t.d.wb().styles->NumberFormat(StyleAt(t, 1, 0)) == "0.00");
  CHECK(StyleAt(t, 1, 0).font_color == model::Rgb(0x9C, 0x00, 0x06));
  // number styles
  t.d.ApplyCellStyle(sh, Rect::Cell(2, 0), "Percent");
  CHECK(t.d.wb().styles->NumberFormat(StyleAt(t, 2, 0)) == "0%");
  CHECK_FALSE(t.d.ApplyCellStyle(sh, Rect::Cell(2, 0), "No such style"));

  // a custom style; its cells follow when it changes
  model::NamedStyle mine;
  mine.name = "Mine";
  mine.style.bold = true;
  mine.style.font_size = 14;
  t.d.SaveCellStyle(mine);
  t.d.ApplyCellStyle(sh, Rect{4, 0, 6, 0}, "Mine");
  CHECK(StyleAt(t, 5, 0).bold);
  mine.style.italic = true;
  mine.style.bold = false;
  t.d.SaveCellStyle(mine);
  CHECK(StyleAt(t, 6, 0).italic);
  CHECK_FALSE(StyleAt(t, 6, 0).bold);
  CHECK_FALSE(StyleAt(t, 0, 0).italic);  // other styles untouched
  // a changed built-in style is kept and its cells follow; deleting it brings back the original
  auto good = *t.d.CellStyleDef("Good");
  good.style.bold = true;
  t.d.SaveCellStyle(good);
  CHECK(StyleAt(t, 0, 0).bold);
  REQUIRE(t.d.DeleteCellStyle("Good"));
  CHECK_FALSE(StyleAt(t, 0, 0).bold);
  CHECK(StyleAt(t, 0, 0).fill_color == model::Rgb(0xC6, 0xEF, 0xCE));
  CHECK_FALSE(t.d.DeleteCellStyle("Normal"));

  // saved and loaded
  auto res = io::LoadYcgFromBytes(io::SaveYcgToBytes(t.d.wb(), {}));
  REQUIRE(res.ok);
  REQUIRE(res.wb.cell_styles->size() == 1);
  CHECK((*res.wb.cell_styles)[0].name == "Mine");
  CHECK((*res.wb.cell_styles)[0].style.italic);
  const auto& ls = *res.wb.sheets[0];
  CHECK(res.wb.styles->Str(res.wb.styles->Get(ls.GetStyle(5, 0)).cell_style) == "Mine");

  // deleting a custom style: its cells get the Normal formats
  REQUIRE(t.d.DeleteCellStyle("Mine"));
  CHECK_FALSE(StyleAt(t, 5, 0).italic);
  CHECK(StyleAt(t, 5, 0).cell_style == 0);
  CHECK(t.d.wb().cell_styles->empty());
}

TEST_CASE("theme: .xlsx theme colors and fonts are read") {
  const std::string theme =
      "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Facet\"><a:themeElements>"
      "<a:clrScheme name=\"Green\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
      "<a:dk2><a:srgbClr val=\"2C3C43\"/></a:dk2><a:lt2><a:srgbClr val=\"EBEBEB\"/></a:lt2><a:accent1><a:srgbClr val=\"90C226\"/></a:accent1>"
      "<a:accent2><a:srgbClr val=\"54A021\"/></a:accent2><a:accent3><a:srgbClr val=\"E6B91E\"/></a:accent3><a:accent4><a:srgbClr val=\"E76618\"/></a:accent4>"
      "<a:accent5><a:srgbClr val=\"C42F1A\"/></a:accent5><a:accent6><a:srgbClr val=\"918655\"/></a:accent6><a:hlink><a:srgbClr val=\"99CA3C\"/></a:hlink>"
      "<a:folHlink><a:srgbClr val=\"B9D181\"/></a:folHlink></a:clrScheme>"
      "<a:fontScheme name=\"Facet\"><a:majorFont><a:latin typeface=\"Trebuchet MS\"/></a:majorFont><a:minorFont><a:latin typeface=\"Trebuchet MS\"/></a:minorFont></a:fontScheme>"
      "</a:themeElements></a:theme>";
  const std::string styles =
      "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Trebuchet MS\"/></font></fonts>"
      "<fills count=\"3\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill>"
      "<fill><patternFill patternType=\"solid\"><fgColor theme=\"4\" tint=\"0.5\"/></patternFill></fill></fills>"
      "<borders count=\"1\"><border/></borders><cellXfs count=\"2\"><xf fontId=\"0\" fillId=\"0\" borderId=\"0\"/><xf fontId=\"0\" fillId=\"2\" borderId=\"0\" applyFill=\"1\"/></cellXfs></styleSheet>";
  const std::string sheet =
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row r=\"1\"><c r=\"A1\" s=\"1\"><v>1</v></c></row></sheetData></worksheet>";
  const std::string wbx =
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
      "<sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
  const std::string rels =
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"theme/theme1.xml\"/>"
      "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/></Relationships>";
  const std::string zip = io::WriteZip({{"xl/workbook.xml", wbx}, {"xl/_rels/workbook.xml.rels", rels}, {"xl/worksheets/sheet1.xml", sheet},
                                       {"xl/theme/theme1.xml", theme}, {"xl/styles.xml", styles}});
  model::Workbook wb;
  std::string err;
  REQUIRE(io::ImportXlsxBytes(zip, &wb, &err, nullptr));
  CHECK(wb.theme->name == "Facet");
  CHECK(wb.theme->colors[model::kAccent1] == 0x90C226);
  CHECK(wb.theme->minor_font == "Trebuchet MS");
  const model::Style& st = wb.styles->Get(wb.sheets[0]->GetStyle(0, 0));
  REQUIRE(model::IsThemeColor(st.fill_color));
  CHECK(model::ThemeColorIndex(st.fill_color) == model::kAccent1);
  CHECK(model::ThemeColorTint(st.fill_color) == 0.5);
}
