// SPDX-License-Identifier: Apache-2.0
// format_match.cpp - find and replace by format
#include "engine/format_match.h"

#include "model/theme.h"

namespace yc::engine {

namespace {

bool SameFill(const model::Style& a, const model::Style& b) {
  auto col = [](uint32_t c) { return (c >> 24) ? model::ResolveColor(c) : 0u; };
  if (a.pattern != b.pattern || a.gradient != b.gradient || col(a.fill_color) != col(b.fill_color)) return false;
  if (a.pattern != model::FillPattern::Solid && col(a.pattern_color) != col(b.pattern_color)) return false;
  if (a.gradient != model::FillGradient::None && (a.gradient_angle != b.gradient_angle || col(a.gradient_color2) != col(b.gradient_color2))) return false;
  return true;
}

bool SameColor(uint32_t a, uint32_t b) {
  // automatic is its own value; theme colors compare by the color they show
  if (!(a >> 24) || !(b >> 24)) return (a >> 24) == (b >> 24);
  return model::ResolveColor(a) == model::ResolveColor(b);
}

bool SameSide(const model::BorderSide& a, const model::BorderSide& b) {
  if (a.style != b.style) return false;
  return a.style == model::LineStyle::None || SameColor(a.color, b.color);
}

}  // namespace

uint32_t DiffMask(const model::Style& a, const model::Style& b) {
  uint32_t m = 0;
  if (a.numfmt != b.numfmt) m |= kFmNumber;
  if (a.halign != b.halign) m |= kFmHAlign;
  if (a.valign != b.valign) m |= kFmVAlign;
  if (a.indent != b.indent) m |= kFmIndent;
  if (a.wrap != b.wrap) m |= kFmWrap;
  if (a.shrink != b.shrink) m |= kFmShrink;
  if (a.rotation != b.rotation) m |= kFmRotation;
  if (a.font_name != b.font_name) m |= kFmFontName;
  if (a.font_size != b.font_size) m |= kFmFontSize;
  if (a.bold != b.bold) m |= kFmBold;
  if (a.italic != b.italic) m |= kFmItalic;
  if (a.underline != b.underline) m |= kFmUnderline;
  if (a.strike != b.strike) m |= kFmStrike;
  if (a.script != b.script) m |= kFmScript;
  if (a.font_color != b.font_color) m |= kFmFontColor;
  for (int e = 0; e < model::kEdgeCount; ++e) {
    if (!(a.border[e] == b.border[e])) m |= kFmBorderLeft << e;
  }
  if (a.fill_color != b.fill_color || a.pattern != b.pattern || a.pattern_color != b.pattern_color || a.gradient != b.gradient ||
      a.gradient_angle != b.gradient_angle || a.gradient_color2 != b.gradient_color2) {
    m |= kFmFill;
  }
  if (a.locked != b.locked) m |= kFmLocked;
  if (a.hidden != b.hidden) m |= kFmHidden;
  return m;
}

FormatSpec SpecOfCell(const model::Style& cell, const model::StyleTable& t) {
  FormatSpec f;
  f.style = cell;
  f.mask = kFmAll;
  f.font = t.FontName(cell);
  f.numfmt = t.NumberFormat(cell);
  return f;
}

bool FormatMatches(const model::Style& c, const model::StyleTable& t, const FormatSpec& f) {
  const uint32_t m = f.mask;
  const model::Style& w = f.style;
  if ((m & kFmNumber) && t.NumberFormat(c) != (f.numfmt.empty() ? std::string("General") : f.numfmt)) return false;
  if ((m & kFmHAlign) && c.halign != w.halign) return false;
  if ((m & kFmVAlign) && c.valign != w.valign) return false;
  if ((m & kFmIndent) && c.indent != w.indent) return false;
  if ((m & kFmWrap) && c.wrap != w.wrap) return false;
  if ((m & kFmShrink) && c.shrink != w.shrink) return false;
  if ((m & kFmRotation) && c.rotation != w.rotation) return false;
  if ((m & kFmFontName) && t.FontName(c) != (f.font.empty() ? t.default_font : f.font)) return false;
  if ((m & kFmFontSize) && t.FontSize(c) != (w.font_size > 0 ? w.font_size : t.default_font_size)) return false;
  if ((m & kFmBold) && c.bold != w.bold) return false;
  if ((m & kFmItalic) && c.italic != w.italic) return false;
  if ((m & kFmUnderline) && c.underline != w.underline) return false;
  if ((m & kFmStrike) && c.strike != w.strike) return false;
  if ((m & kFmScript) && c.script != w.script) return false;
  if ((m & kFmFontColor) && !SameColor(c.font_color, w.font_color)) return false;
  for (int e = 0; e < model::kEdgeCount; ++e) {
    if ((m & (kFmBorderLeft << e)) && !SameSide(c.border[e], w.border[e])) return false;
  }
  if ((m & kFmFill) && !SameFill(c, w)) return false;
  if ((m & kFmLocked) && c.locked != w.locked) return false;
  if ((m & kFmHidden) && c.hidden != w.hidden) return false;
  return true;
}

void ApplyFormatSpec(model::Style& c, const FormatSpec& f, uint32_t font_id, uint32_t numfmt_id) {
  const uint32_t m = f.mask;
  const model::Style& w = f.style;
  if (m & kFmNumber) c.numfmt = numfmt_id;
  if (m & kFmHAlign) c.halign = w.halign;
  if (m & kFmVAlign) c.valign = w.valign;
  if (m & kFmIndent) c.indent = w.indent;
  if (m & kFmWrap) c.wrap = w.wrap;
  if (m & kFmShrink) c.shrink = w.shrink;
  if (m & kFmRotation) c.rotation = w.rotation;
  if (m & kFmFontName) c.font_name = font_id;
  if (m & kFmFontSize) c.font_size = w.font_size;
  if (m & kFmBold) c.bold = w.bold;
  if (m & kFmItalic) c.italic = w.italic;
  if (m & kFmUnderline) c.underline = w.underline;
  if (m & kFmStrike) c.strike = w.strike;
  if (m & kFmScript) c.script = w.script;
  if (m & kFmFontColor) c.font_color = w.font_color;
  for (int e = 0; e < model::kEdgeCount; ++e) {
    if (m & (kFmBorderLeft << e)) c.border[e] = w.border[e];
  }
  if (m & kFmFill) {
    c.fill_color = w.fill_color;
    c.pattern = w.pattern;
    c.pattern_color = w.pattern_color;
    c.gradient = w.gradient;
    c.gradient_angle = w.gradient_angle;
    c.gradient_color2 = w.gradient_color2;
  }
  if (m & kFmLocked) c.locked = w.locked;
  if (m & kFmHidden) c.hidden = w.hidden;
}

}  // namespace yc::engine
