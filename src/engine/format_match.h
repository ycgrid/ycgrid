// SPDX-License-Identifier: Apache-2.0
// format_match.h - find and replace by format (Excel's Find Format / Replace Format; Application.FindFormat /
// ReplaceFormat): the fields named in a mask are compared with a cell's format, or written into it
#pragma once

#include "model/style.h"

#include <cstdint>
#include <string>

namespace yc::engine {

enum FormatField : uint32_t {
  kFmNumber = 1u << 0,
  kFmHAlign = 1u << 1,
  kFmVAlign = 1u << 2,
  kFmIndent = 1u << 3,
  kFmWrap = 1u << 4,
  kFmShrink = 1u << 5,
  kFmRotation = 1u << 6,
  kFmFontName = 1u << 7,
  kFmFontSize = 1u << 8,
  kFmBold = 1u << 9,
  kFmItalic = 1u << 10,
  kFmUnderline = 1u << 11,
  kFmStrike = 1u << 12,
  kFmScript = 1u << 13,
  kFmFontColor = 1u << 14,
  kFmBorderLeft = 1u << 15,  // kFmBorderLeft << edge for each model::BorderEdge
  kFmBorderTop = 1u << 16,
  kFmBorderRight = 1u << 17,
  kFmBorderBottom = 1u << 18,
  kFmBorderDiagDown = 1u << 19,
  kFmBorderDiagUp = 1u << 20,
  kFmFill = 1u << 21,  // fill color, pattern and gradient together
  kFmLocked = 1u << 22,
  kFmHidden = 1u << 23,
  kFmAll = (1u << 24) - 1,
};

// a format to find or to replace with: style holds the values of the fields in mask; the font name and the number
// format are kept as text (they don't depend on a workbook's string table)
struct FormatSpec {
  model::Style style;
  uint32_t mask = 0;
  std::string font, numfmt;
  bool Any() const { return mask != 0; }
};

// the fields of b that differ from a (the Format Cells dialog's choices over its starting point)
uint32_t DiffMask(const model::Style& a, const model::Style& b);
// a cell's whole format as a spec (Choose Format From Cell): every field
FormatSpec SpecOfCell(const model::Style& cell, const model::StyleTable& t);
// the cell's format has every field of the spec
bool FormatMatches(const model::Style& cell, const model::StyleTable& t, const FormatSpec& f);
// write the spec's fields into a cell's format (font_id / numfmt_id: the spec's texts interned by the caller)
void ApplyFormatSpec(model::Style& cell, const FormatSpec& f, uint32_t font_id, uint32_t numfmt_id);

}  // namespace yc::engine
