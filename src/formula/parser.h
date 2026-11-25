// SPDX-License-Identifier: Apache-2.0
// parser.h - formula lexing, parsing and text restoration (PLAN §6.1-§6.3, §6.8)
#pragma once

#include "core/geometry.h"
#include "formula/ast.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace yc::formula {

// provided by the caller: sheet name <-> id
class SheetResolver {
 public:
  virtual ~SheetResolver() = default;
  virtual int32_t FindSheet(std::string_view name) const = 0;  // -1 when not found
  virtual std::string SheetName(int32_t id) const = 0;          // "" when deleted
  // a file saved before DECIMAL(text, radix) became Excel's: DECIMAL with two arguments then reads as DECIMAL.SCALE
  virtual bool LegacyDecimal() const { return false; }
};

struct ParseResult {
  bool ok = false;
  std::string error;
  size_t error_pos = 0;
  Node root;
};

// text without the leading '='. origin is the formula's cell. r1c1: references are written R1C1-style (R[-1]C, RC2,
// R3, C[1]:C[2]); the A1 cell names (A1, B2) are then plain names
// keep_order: a range's ends stay as written (a defined name shown as Excel keeps it: $C2:B$5 after a column went in
// at B, measured); otherwise they are put in order per dimension, as a typed formula is (=SUM($C1:B2) is B1:$C2)
ParseResult Parse(std::string_view text, core::CellPos origin, const SheetResolver* sheets, bool r1c1 = false, bool keep_order = false);

// dedup key of shared formulas (R1C1 semantics, including sheet ids)
std::string SerializeKey(const Node& root);

// back to A1 text (without '=')
// keep_order: a range's ends are written as they are (a defined name, as Excel keeps it); otherwise from the range's
// upper-left corner, as Excel writes a cell's formula
std::string ToA1(const Node& root, core::CellPos origin, const SheetResolver* sheets, bool keep_order = false);
// back to R1C1 text (without '='): relative references as offsets in brackets, absolute ones as numbers
std::string ToR1C1(const Node& root, core::CellPos origin, const SheetResolver* sheets);
// a range's address in R1C1 style relative to origin (point mode); absolute when origin is null
std::string RangeR1C1(const core::Rect& r, const core::CellPos* origin);

// formula flags: volatile, dynamic references, UDF
uint32_t ComputeFlags(const Node& root);

// resolved references (for the dependency graph): sheet = -1 means the formula's own sheet
struct ResolvedRef {
  int32_t sheet = -1;
  core::Rect rect;
};
void CollectRefs(const Node& root, core::CellPos origin, std::vector<ResolvedRef>* out);
// the references of a formula, unresolved (once per formula; ResolveRef places one at a cell)
void CollectRefSpecs(const Node& root, std::vector<RefSpec>* out);
// a reference as seen from origin, as CollectRefs gives it; false when it falls off the sheet
bool ResolveRef(const RefSpec& r, core::CellPos origin, ResolvedRef* out);
void CollectNames(const Node& root, std::vector<std::string>* out);

// visit every reference node (for structural changes)
void ForEachRef(Node& root, const std::function<void(RefSpec&)>& fn);

// ---- relative references in defined names (Excel's relative names) ----
// A name's text keeps its relative references relative to A1, as Excel's files do, and they wrap around the sheet's
// edges: "=Sheet1!XFD1" defined at A1 is the cell left of the one that uses it. Excel shows and takes the text relative
// to the active cell.
bool HasRelativeRef(const Node& root);
// makes the relative references of a tree parsed at A1 (or at another cell) reach, from `to`, the cells the same
// offsets reach after wrapping around the sheet's edges; then evaluate or write the tree at `to`
void RelativeTo(Node& root, core::CellPos to);
// a name's text typed at `from` as it reads at `to` (the same offsets); text without relative references, or that
// doesn't parse, comes back as it is
std::string MoveRelativeText(std::string_view text, core::CellPos from, core::CellPos to, const SheetResolver* sheets,
                             bool r1c1 = false, bool keep_order = false);
// the text with every relative reference made absolute where it points from A1 (names in files saved before names
// kept relative references, which then read as absolute ones)
std::string AbsoluteRefText(std::string_view text, const SheetResolver* sheets);

// the references in a formula's text, in order, with where each one is ([begin, end) bytes): every word outside strings
// (sheet names, brackets and ranges included) that isn't a function name and is one reference; group numbers the
// different references (=A1+B1+A1: 0, 1, 0), as the colors of the boxes drawn while a formula is typed
struct RefSpan {
  int begin = 0, end = 0;
  ResolvedRef ref;
  int group = 0;
};
std::vector<RefSpan> FindRefSpans(std::string_view text, core::CellPos origin, const SheetResolver* sheets, bool r1c1 = false);

// whether it looks like a numeric constant (to decide whether an input is a formula)
bool LooksLikeFormula(std::string_view text);
// a name A1 formulas refuse as the start of an R1C1 reference (R2D2, C3PO, R1C1, RC1x; Rx and R0x are names)
bool IsR1C1LikeName(std::string_view s);

// F4 while typing a formula: cycle the A1 reference at the caret (or every reference in the selection [sel_begin, sel_end))
// through A1 -> $A$1 -> A$1 -> $A1 -> A1; a range's two ends change together. Returns false when there is no reference there;
// otherwise rewrites text and sets *out_begin / *out_end to the changed span (the caret goes to its end).
bool CycleAbsoluteRef(std::string& text, int sel_begin, int sel_end, int* out_begin, int* out_end);

}  // namespace yc::formula
