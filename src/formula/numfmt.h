// SPDX-License-Identifier: Apache-2.0
// numfmt.h - Excel number format engine (PLAN §5.5, §5.2a)
//
// Supports: four sections (positive;negative;zero;text), conditions [>=100], colors [Red] / Chinese color names / [Color10],
// 0 # ? . , % E+, thousands separators and scaling, fractions, text @, literals "..." \x _x *x,
// dates and times y m d h s AM/PM (and the Chinese AM/PM marker) [h] [m] [s] .000, ROC year e,
// [YC] [epoch] (the value is Unix epoch seconds), [utc] / [local] / [tz=+08:00], up to 9 fractional second digits (exact with Decimal).
#pragma once

#include "core/value.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace yc::formula {

struct FormatResult {
  std::string text;
  uint32_t color = 0;     // 0xAARRGGBB; 0 = keep the color
  bool numeric = false;   // numeric (right-aligned in General)
  bool hashes = false;    // column too narrow (####)
  // a date or time outside the calendar (negative, after 9999), or a number too big for a fraction without a whole
  // part ("?/?" of 2^31 - 1): Excel fills the cell with # and TEXT is #VALUE!
  bool bad_date = false;
  // a code Excel refuses ("0;@;0", "@0", five sections, measured): TEXT is #VALUE!; the text is the value in General
  bool bad_code = false;
  // *x in the code: x repeats at byte fill_at of text until the cell is full (an accounting format's $ goes to the
  // left edge); TEXT and the other text uses leave it out, as Excel does
  int fill_at = -1;
  std::string fill;
};

class NumberFormat;
// can be cached after compiling (the same format code is reused)
std::shared_ptr<const NumberFormat> CompileFormat(std::string_view code);
// false for a code Excel refuses: Format Cells says so and keeps the dialog open, Range.NumberFormat raises, TEXT
// is #VALUE! (measured in Excel 16)
bool ValidFormatCode(std::string_view code);

// max_chars: General adjusts the shown digits to the column width (in characters); < 0 means unlimited
FormatResult FormatValue(const core::Value& v, std::string_view code, double max_chars = -1);
// a whole number in Chinese numerals, as [DBNum1] / NUMBERSTRING(n, 1) (style 1, with units) and [DBNum2] /
// NUMBERSTRING(n, 2) (style 2, the financial numerals); 12 is one ten two and a zero inside is \u25cb, as Excel
std::string ChineseNumerals(uint64_t n, int style);
FormatResult FormatValue(const core::Value& v, const NumberFormat& fmt, double max_chars = -1);
// Workbook.PrecisionAsDisplayed: the number a cell shows under `code`, rounded to the digits the code shows (its
// decimals, two more per %, three fewer per trailing comma, an exponent's significant digits); false when the code
// shows the number as it is (General, a date or time, a fraction, text, a conditional section), as Excel 16 keeps them
bool RoundAsShown(double v, std::string_view code, double* out);

// General, fitted to max_chars characters (11 at most in a cell, as Excel)
std::string FormatGeneral(double v, int max_chars = 11);
// a number as text, as & and the text functions convert it (Excel): 15 significant digits, written out while that
// takes at most 20 characters (1E+19, 0.0000000000000001), scientific beyond (1E+20, 1.23456789012345E-12)
std::string NumberToText(double v);
// a number as the cell editor and the formula bar show it to edit (Excel's Range.Formula of a constant, measured):
// 15 significant digits written out while that takes at most 21 characters (123456789012, 1E+20 as twenty-one
// digits, 0.00000025), E notation beyond (1E+21, 1.23456789012346E-15); F2 then Enter changes nothing
std::string EditNumberText(double v);
// how a typed entry's format replaces a cell's own (Excel, measured: 5% typed into a m/d/yyyy cell makes it 0%):
// only a built-in percent, scientific, fraction, currency, accounting, date or time format gives way, to an entry
// of another of those kinds; custom and plain number formats stay
bool TypedFormatReplaces(std::string_view cell_code, std::string_view typed_code);
// "a/b" typed into a cell with a number format (not General, a date, a time or text) is a fraction, as in Excel:
// 1/2 in a 0.00 cell is 0.5, 3/32 in a # ??/32 cell 0.09375; *out gets the value
bool FractionForNumberFormat(std::string_view text, std::string_view cell_code, double* out);

bool IsDateFormat(std::string_view code);
// a format whose first section shows a percentage (an unquoted %: 0%, 0.00%, [Red]0%, %0; not 0"%" or 0\%): a number
// typed into such a cell is a percentage, Excel's automatic percent entry (Excel 16, measured: 7 typed is 7%)
bool IsPercentFormat(std::string_view code);
// Excel's automatic percent entry for text typed into a cell of format cell_code: a number without a % gets one
// (5 -> 5%, .5 -> .5%, (5) -> (5)%), anything else is returned as it is (5%, =5, abc, a date)
std::string PercentEntry(std::string_view text, std::string_view cell_code);
// a format whose first section shows a date (a day, month or year), not only a time
bool FormatShowsDate(std::string_view code);
// CELL("format"): G, F2, ,0, C2, P0, S2, D1..D9 (measured against Excel 16), with - when negative numbers have a
// color (*color) and () when positive ones show parentheses (*parens)
std::string CellFormatCode(std::string_view code, bool* color, bool* parens);

// text typed by the user -> value and suggested format (50% -> 0.5, "%"; 2026/10/11 -> date serial, "yyyy/m/d")
struct InputParse {
  core::Value value;
  std::string format;  // empty = keep the format
  bool typed_only = false;  // a form Excel takes only typed (the month and year 1/2026): a formula reads it as text
};
InputParse ParseInput(std::string_view text, bool excel_15_digit_input = false);
// ParseInput's numbers alone (no dates or times): true when text is a typed number, currency amount, percentage or
// mixed fraction, as Excel reads them; *out gets the value and the format
bool ParseNumberInput(std::string_view text, InputParse* out, bool excel_15_digit_input = false);

}  // namespace yc::formula
