// SPDX-License-Identifier: Apache-2.0
// functions.h - built-in function registry (PLAN §6.5)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yc::formula {

class Evaluator;
struct Node;
struct EV;

using FuncImpl = EV (*)(Evaluator& ev, const Node& call);

struct FuncInfo {
  const char* name;
  int min_args;
  int max_args;  // -1 = unlimited
  FuncImpl impl;
  bool is_volatile = false;
  bool dynamic_ref = false;
  // array lifting (like Excel): bit i = argument i expects a single value; given several cells there, the function is
  // evaluated once per element and returns an array (bit 31 also covers arguments 32 and later). lift_rest: once that
  // happens, every other argument is lifted too (IF, IFERROR, CHOOSE...: their value arguments stay lazy otherwise)
  uint32_t lift = 0;
  bool lift_rest = false;
  // an array of references (RefArray) given to the function: 1 = it runs once per reference (N, SUBTOTAL, COUNTIF),
  // 2 = it reads the first cell of each (SUM, MAX), 3 = each is a value that isn't a number (SUMPRODUCT: 0),
  // 4 = the call is #N/A unless another argument holds an error (AND, OR, XOR), 5 = #NUM! unless another argument
  // holds an error or the sizes differ (SUMX2MY2 and its two siblings); anything else sees the array's #VALUE! values
  // (all measured in Excel 16)
  uint8_t ref_mode = 0;
  // the functions that came from the Analysis ToolPak refuse TRUE / FALSE where a number goes: #VALUE!
  // (QUOTIENT(TRUE,1), EFFECT(0.1,A10) with TRUE in A10: Excel 16, measured); Evaluator::ArgNumber applies it
  bool no_logical = false;
};

// the value of IMAGE(source, [alt_text], [sizing], [height], [width]): text with a marker that the grid draws as the
// picture (other formulas see the alt text through ImageAltText)
struct ImageSpec {
  std::string url, alt;
  int sizing = 0;  // 0 fit (keep the aspect ratio), 1 fill the cell, 2 original size, 3 height x width
  double height = 0, width = 0;
};
std::string MakeImageValue(const ImageSpec& spec);
// text lowercased the way every comparison that ignores case does it (formulas, Find, filters, rules): ASCII,
// accented Latin, Greek, Cyrillic and full-width letters; the UTF-8 length stays the same
std::string FoldCase(std::string_view s);
bool ParseImageValue(std::string_view text, ImageSpec* out);

// a CUBE function's member / set / KPI value: text with a marker (ESC "CUBE" US kind US caption US members). Cells and
// other formulas see its caption
bool ParseCubeValue(std::string_view text, std::string* caption);

int32_t FindFunction(std::string_view upper_name);  // strips the _xlfn. prefix; -1 when not found
const FuncInfo& GetFunction(int32_t id);
const std::vector<FuncInfo>& AllFunctions();

// for autocomplete and API lookup
std::vector<std::string> FunctionNames();

}  // namespace yc::formula
