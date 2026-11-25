// SPDX-License-Identifier: Apache-2.0
// rich_text.h - several fonts inside one cell's text (Excel's Characters(Start, Length).Font, superscript / subscript)
#pragma once

#include "model/style.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace yc::model {

// the font of the characters from `start` up to the next run's start; fields left at "inherit" take the cell's font
struct RichRun {
  uint32_t start = 0;     // first character (Unicode code points, 0-based)
  std::string font;       // "" = the cell's font
  float size = 0;         // 0 = the cell's size
  uint32_t color = 0;     // 0 = the cell's color
  int8_t bold = -1, italic = -1, strike = -1;  // -1 = the cell's
  int8_t underline = -1;  // model::Underline; -1 = the cell's
  int8_t script = -1;     // 0 none, 1 superscript, 2 subscript; -1 = the cell's

  bool SameFont(const RichRun& o) const {
    return font == o.font && size == o.size && color == o.color && bold == o.bold && italic == o.italic && strike == o.strike &&
           underline == o.underline && script == o.script;
  }
  bool Inherits() const { return font.empty() && size == 0 && color == 0 && bold < 0 && italic < 0 && strike < 0 && underline < 0 && script < 0; }
  bool operator==(const RichRun& o) const { return start == o.start && SameFont(o); }
};

// the runs of a cell; they only apply while the cell still holds exactly `text` (a new value makes them stale)
struct RichText {
  std::string text;
  std::vector<RichRun> runs;  // sorted by start; the first starts at 0
  bool operator==(const RichText&) const = default;
};

size_t CharCount(std::string_view utf8);
size_t ByteOfChar(std::string_view utf8, size_t ch);  // byte offset of the ch-th character (size() past the end)

// the font of a run: the cell's font with the run's changes (run may be nullptr)
struct RunFont {
  std::string name;
  float size = 11;  // points
  uint32_t color = 0;  // 0 = automatic
  bool bold = false, italic = false, strike = false;
  Underline underline = Underline::None;
  uint8_t script = 0;  // 1 superscript, 2 subscript
};
RunFont ResolveRunFont(const StyleTable& t, const Style& cell, const RichRun* run);
// the run in effect at a character
const RichRun* RunAt(const RichText& rt, size_t ch);

// change the font of characters [start, start + len): splits runs at both ends, calls fn on each run inside, merges equal neighbors
void ApplyToChars(RichText* rt, size_t start, size_t len, const std::function<void(RichRun&)>& fn);
// keep the formatting when the text is edited: the common start and end keep their runs, typed text takes the font before it
RichText RemapRuns(const RichText& old, const std::string& new_text);
// replace characters [start, start + len) with ins: the new text takes the font of the first replaced character
// (or of the character before, when inserting)
RichText ReplaceChars(const RichText& rt, size_t start, size_t len, const std::string& ins);
// drop runs that change nothing; true when formatting is left
bool Normalize(RichText* rt);

// line layout of a rich text (shared by the grid and printing): pieces of one run on one line
struct RichPiece {
  size_t run = 0;    // index into RichText::runs (0 when there are none)
  std::string text;
  float x = 0, y = 0, w = 0, h = 0;  // relative to the text block's top-left; pieces sit on the line's bottom
};
struct RichLayout {
  std::vector<RichPiece> pieces;
  float width = 0, height = 0;
};
// measure(text, run) -> {width, height} in the renderer's units; empty_h = the height of an empty line;
// halign 0 left, 1 center, 2 right (within wrap_width when wrapping, else within the widest line)
RichLayout LayoutRich(const RichText& rt, bool wrap, float wrap_width, int halign, float empty_h,
                      const std::function<std::pair<float, float>(const std::string&, size_t)>& measure);

}  // namespace yc::model
