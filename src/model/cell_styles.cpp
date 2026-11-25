// SPDX-License-Identifier: Apache-2.0
// cell_styles.cpp - Excel's built-in cell styles and applying a named style to a cell
#include "model/style.h"
#include "model/theme.h"

namespace yc::model {

void ApplyNamedStyle(Style& c, const NamedStyle& ns, uint32_t name_id) {
  const Style& s = ns.style;
  if (ns.includes & kIncNumber) c.numfmt = s.numfmt;
  if (ns.includes & kIncAlignment) {
    c.halign = s.halign;
    c.valign = s.valign;
    c.indent = s.indent;
    c.wrap = s.wrap;
    c.shrink = s.shrink;
    c.rotation = s.rotation;
  }
  if (ns.includes & kIncFont) {
    c.font_name = s.font_name;
    c.font_size = s.font_size;
    c.font_color = s.font_color;
    c.bold = s.bold;
    c.italic = s.italic;
    c.strike = s.strike;
    c.underline = s.underline;
    c.script = s.script;
  }
  if (ns.includes & kIncBorder) {
    for (int e = 0; e < kEdgeCount; ++e) c.border[e] = s.border[e];
  }
  if (ns.includes & kIncFill) {
    c.fill_color = s.fill_color;
    c.pattern = s.pattern;
    c.pattern_color = s.pattern_color;
    c.gradient = s.gradient;
    c.gradient_angle = s.gradient_angle;
    c.gradient_color2 = s.gradient_color2;
  }
  if (ns.includes & kIncProtection) {
    c.locked = s.locked;
    c.hidden = s.hidden;
  }
  c.cell_style = name_id;
}

const std::vector<NamedStyle>& BuiltinCellStyles() {
  static const std::vector<NamedStyle> v = [] {
    std::vector<NamedStyle> out;
    auto add = [&](const char* name, const char* section, uint8_t inc, auto fn) {
      NamedStyle ns;
      ns.name = name;
      ns.section = section;
      ns.includes = inc;
      ns.builtin = true;
      fn(ns.style);
      out.push_back(std::move(ns));
    };
    auto box = [](Style& s, LineStyle ls, uint32_t c) {
      for (int e = 0; e < 4; ++e) s.border[e] = BorderSide{ls, c};
    };
    const uint32_t dk2 = MakeThemeColor(kDark2), lt1 = MakeThemeColor(kLight1), dk1 = MakeThemeColor(kDark1), acc1 = MakeThemeColor(kAccent1);
    const char* gbn = "Good, Bad and Neutral";
    const char* dm = "Data and Model";
    const char* th = "Titles and Headings";
    const char* tcs = "Themed Cell Styles";
    const char* nf = "Number Format";
    add("Normal", gbn, kIncAll, [](Style&) {});
    add("Bad", gbn, kIncFont | kIncFill, [](Style& s) { s.fill_color = Rgb(0xFF, 0xC7, 0xCE); s.font_color = Rgb(0x9C, 0x00, 0x06); });
    add("Good", gbn, kIncFont | kIncFill, [](Style& s) { s.fill_color = Rgb(0xC6, 0xEF, 0xCE); s.font_color = Rgb(0x00, 0x61, 0x00); });
    add("Neutral", gbn, kIncFont | kIncFill, [](Style& s) { s.fill_color = Rgb(0xFF, 0xEB, 0x9C); s.font_color = Rgb(0x9C, 0x57, 0x00); });
    add("Calculation", dm, kIncFont | kIncBorder | kIncFill, [&](Style& s) {
      s.fill_color = Rgb(0xF2, 0xF2, 0xF2);
      s.font_color = Rgb(0xFA, 0x7D, 0x00);
      s.bold = true;
      box(s, LineStyle::Thin, Rgb(0x7F, 0x7F, 0x7F));
    });
    add("Check Cell", dm, kIncFont | kIncBorder | kIncFill, [&](Style& s) {
      s.fill_color = Rgb(0xA5, 0xA5, 0xA5);
      s.font_color = lt1;
      s.bold = true;
      box(s, LineStyle::Double, Rgb(0x3F, 0x3F, 0x3F));
    });
    add("Explanatory Text", dm, kIncFont, [](Style& s) { s.font_color = Rgb(0x7F, 0x7F, 0x7F); s.italic = true; });
    add("Hyperlink", dm, kIncFont, [](Style& s) { s.font_color = MakeThemeColor(kHyperlink); s.underline = Underline::Single; });
    add("Input", dm, kIncFont | kIncBorder | kIncFill, [&](Style& s) {
      s.fill_color = Rgb(0xFF, 0xCC, 0x99);
      s.font_color = Rgb(0x3F, 0x3F, 0x76);
      box(s, LineStyle::Thin, Rgb(0x7F, 0x7F, 0x7F));
    });
    add("Linked Cell", dm, kIncFont | kIncBorder, [](Style& s) {
      s.font_color = Rgb(0xFA, 0x7D, 0x00);
      s.border[kBottom] = BorderSide{LineStyle::Double, Rgb(0xFF, 0x80, 0x01)};
    });
    add("Note", dm, kIncBorder | kIncFill, [&](Style& s) {
      s.fill_color = Rgb(0xFF, 0xFF, 0xCC);
      box(s, LineStyle::Thin, Rgb(0xB2, 0xB2, 0xB2));
    });
    add("Output", dm, kIncFont | kIncBorder | kIncFill, [&](Style& s) {
      s.fill_color = Rgb(0xF2, 0xF2, 0xF2);
      s.font_color = Rgb(0x3F, 0x3F, 0x3F);
      s.bold = true;
      box(s, LineStyle::Thin, Rgb(0x3F, 0x3F, 0x3F));
    });
    add("Warning Text", dm, kIncFont, [](Style& s) { s.font_color = Rgb(0xFF, 0x00, 0x00); });
    add("Heading 1", th, kIncFont | kIncBorder, [&](Style& s) {
      s.font_size = 15;
      s.bold = true;
      s.font_color = dk2;
      s.border[kBottom] = BorderSide{LineStyle::Thick, acc1};
    });
    add("Heading 2", th, kIncFont | kIncBorder, [&](Style& s) {
      s.font_size = 13;
      s.bold = true;
      s.font_color = dk2;
      s.border[kBottom] = BorderSide{LineStyle::Thick, MakeThemeColor(kAccent1, 0.5)};
    });
    add("Heading 3", th, kIncFont | kIncBorder, [&](Style& s) {
      s.bold = true;
      s.font_color = dk2;
      s.border[kBottom] = BorderSide{LineStyle::Medium, MakeThemeColor(kAccent1, 0.4)};
    });
    add("Heading 4", th, kIncFont, [&](Style& s) {
      s.bold = true;
      s.font_color = dk2;
    });
    add("Title", th, kIncFont, [&](Style& s) {
      s.font_size = 18;
      s.font_color = dk2;
    });
    add("Total", th, kIncFont | kIncBorder, [&](Style& s) {
      s.bold = true;
      s.border[kTop] = BorderSide{LineStyle::Thin, acc1};
      s.border[kBottom] = BorderSide{LineStyle::Double, acc1};
    });
    // themed: 20% / 40% / 60% tints and the accent itself, for each accent
    static const char* const kNames[4][6] = {
        {"20% - Accent1", "20% - Accent2", "20% - Accent3", "20% - Accent4", "20% - Accent5", "20% - Accent6"},
        {"40% - Accent1", "40% - Accent2", "40% - Accent3", "40% - Accent4", "40% - Accent5", "40% - Accent6"},
        {"60% - Accent1", "60% - Accent2", "60% - Accent3", "60% - Accent4", "60% - Accent5", "60% - Accent6"},
        {"Accent1", "Accent2", "Accent3", "Accent4", "Accent5", "Accent6"}};
    const double tints[4] = {0.8, 0.6, 0.4, 0.0};
    for (int k = 0; k < 4; ++k) {
      for (int a = 0; a < 6; ++a) {
        add(kNames[k][a], tcs, kIncFont | kIncFill, [&](Style& s) {
          s.fill_color = MakeThemeColor(kAccent1 + a, tints[k]);
          s.font_color = k >= 2 ? lt1 : dk1;
        });
      }
    }
    // the codes as Excel writes them (Styles("Comma").NumberFormat, measured against Excel 16)
    const std::pair<const char*, const char*> numbers[] = {
        {"Comma", "_(* #,##0.00_);_(* (#,##0.00);_(* \"-\"??_);_(@_)"},
        {"Comma [0]", "_(* #,##0_);_(* (#,##0);_(* \"-\"_);_(@_)"},
        {"Currency", "_($* #,##0.00_);_($* (#,##0.00);_($* \"-\"??_);_(@_)"},
        {"Currency [0]", "_($* #,##0_);_($* (#,##0);_($* \"-\"_);_(@_)"},
        {"Percent", "0%"},
    };
    for (const auto& [name, code] : numbers) {
      add(name, nf, kIncNumber, [](Style&) {});
      out.back().number_format = code;
    }
    return out;
  }();
  return v;
}

}  // namespace yc::model
