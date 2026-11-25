// SPDX-License-Identifier: Apache-2.0
// error_check.h - Excel's background error checking: the rule a cell breaks (the green triangle in its corner and the
// error options button), as File > Options > Formulas > Error checking rules turns them on
#pragma once

#include "core/geometry.h"
#include "formula/numfmt.h"
#include "model/sheet.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace yc::model {
class Workbook;
}

namespace yc::engine {

// Excel's XlErrorChecks numbers (Range.Errors(index), ErrorCheckingOptions)
enum ErrorRule : int {
  kErrNone = 0,
  kErrEvaluateToError = 1,       // a formula that gives an error value
  kErrTextDate = 2,              // text with a two-digit year ('12/31/99)
  kErrNumberAsText = 3,          // a number stored as text (or typed after an apostrophe)
  kErrInconsistentFormula = 4,   // a formula unlike its neighbors on both sides, which agree with each other
  kErrOmittedCells = 5,          // a formula whose range stops next to more numbers of its region
  kErrUnlockedFormula = 6,       // a formula in an unlocked cell
  kErrEmptyCellReference = 7,    // a formula that refers to an empty cell
  kErrListDataValidation = 8,    // a value in a table that breaks the cell's data validation rule
  kErrInconsistentListFormula = 9,  // a cell of a table's calculated column unlike the column's formula
  kErrMisleadingFormat = 10,     // a formula shown in another kind of number format than the cells it reads
  kErrRuleCount = 11,
};

// File > Options > Formulas > Error checking (Application.ErrorCheckingOptions), Excel's defaults
struct ErrorCheckOptions {
  bool background = true;  // BackgroundChecking
  bool evaluate_to_error = true, text_date = true, number_as_text = true, inconsistent_formula = true, omitted_cells = true,
       unlocked_formula = true, empty_cell_reference = false, list_data_validation = true, inconsistent_list_formula = true,
       misleading_format = true;
  bool On(int rule) const;
};

// the cells whose rule the user ignored (Ignore Error)
bool ErrorIgnored(const model::Sheet& s, uint32_t r, uint32_t c);

// the rule cell (r, c) breaks, kErrNone when none (or the user ignored it); cheap enough for every visible cell
int ErrorCheckRule(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, const ErrorCheckOptions& o, bool check_ignored = true);

// the rule's name as the error options button shows it ("Number Stored as Text"; an error value names its error:
// "Divide by Zero Error"), translated
const char* ErrorRuleTitle(int rule, const core::Value& v);
// the explanation Help on this Error and the Error Checking dialog show
const char* ErrorRuleHelp(int rule, const core::Value& v);

// Omitted cells: the formula's range grown over the numbers next to it (Update Formula to Include Cells); false when
// nothing is omitted
bool OmittedCellsFix(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, core::Rect* old_range, core::Rect* new_range);

// the kind of a number format code, as Format Cells' categories (Misleading number formats compares them)
enum FormatKind : int { kFmtGeneral = 0, kFmtNumber, kFmtCurrency, kFmtAccounting, kFmtDate, kFmtTime, kFmtPercent, kFmtFraction, kFmtScientific, kFmtText };
int FormatKindOf(std::string_view code);
const char* FormatKindName(int kind);  // "Currency", "Date"... (translated)

// Misleading number formats: the number format of the cells the formula reads (Update Format to ...); false when the
// cell doesn't break the rule
bool MisleadingFormat(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, uint32_t* numfmt, int* kind);

class Document;
// Update Formula to Include Cells: the formula at (r, c) with its range grown over the omitted cells ("" when none)
std::string OmittedCellsFormulaText(const Document& d, uint32_t sheet, uint32_t r, uint32_t c);

// the cells of the sheet that break a rule, in reading order (rows, then columns), starting after (r, c) (the Error
// Checking dialog's Next; backward for Previous); false when there is none
bool NextErrorCell(const model::Workbook& wb, const model::Sheet& s, uint32_t r, uint32_t c, bool forward, const ErrorCheckOptions& o, uint32_t* out_r,
                   uint32_t* out_c);

}  // namespace yc::engine
