// SPDX-License-Identifier: Apache-2.0
// ast.h - formula AST (PLAN §6.3)
//
// References are always stored as offsets from the origin or as absolute coordinates (R1C1 semantics), so filled-down formulas
// with different text but the same AST can share one FormulaDef.
#pragma once

#include "core/value.h"
#include "model/formula_table.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yc::formula {

struct RefCoord {
  int64_t v = 0;     // 0-based index when abs; otherwise the offset from the origin
  bool abs = false;
  int64_t Resolve(int64_t origin) const { return abs ? v : origin + v; }
  friend bool operator==(const RefCoord& a, const RefCoord& b) { return a.v == b.v && a.abs == b.abs; }
};

struct RefSpec {
  int32_t sheet = -1;          // -1 = the formula's own sheet; otherwise a sheet id
  int32_t sheet2 = -1;         // 3D reference Sheet1:Sheet3!A1: the last sheet's id (-1 = not 3D, -2 = missing sheet)
  RefCoord r0, c0, r1, c1;
  bool whole_cols = false;     // A:C
  bool whole_rows = false;     // 2:5
  bool is_range = false;       // single cell = false
  bool bad = false;            // #REF! (deleted reference)
  bool lost_sheet = false;     // with bad: #REF!A1, a reference whose sheet was deleted keeps its cell part (Excel)
  bool spill = false;          // A1#: the whole spill range of the anchor A1

  // it still names cells: a live reference, or one whose sheet was deleted (#REF!A1), which moves with its formula as a
  // reference to another sheet does. Every walk that moves, rebases or sorts references asks this, not !bad: those
  // that skipped #REF!A1 left it naming other cells than its text after the formula moved (doc_fuzz seed 88001420).
  bool HasCells() const { return !bad || lost_sheet; }
  // the sheet it names, for code that compares it with a sheet: the formula's own (own), a sheet id, or -3 for a
  // deleted sheet, which is no sheet of the workbook
  int32_t TargetSheet(int32_t own) const { return lost_sheet ? -3 : sheet < 0 ? own : sheet; }
};

enum class NK : uint8_t {
  Num,
  Str,
  Bool,
  Err,
  Ref,
  Name,      // defined name
  Func,      // function call
  Unary,     // op: '-' '+' '@' (implicit intersection)
  Binary,    // op: + - * / ^ & = <>(N) <(<) >(>) <=(L) >=(G) :(range) ,(union)
  Percent,   // postfix %
  Array,     // array constant {1,2;3,4}
  Missing,   // omitted argument =IF(A1,,1)
  Paren,     // parentheses (kept so the text can be restored)
  Call,      // calling a LAMBDA value: kids[0](kids[1], ...), e.g. LAMBDA(x,x+1)(5)
  StructRef, // table reference: str = table ("" = the table the formula is in), op = model::TableItem bits,
             // kids[0] / kids[1] = first / last column name (NK::Str, "" = all columns): Table1[[#Headers],[A]:[B]]
};

struct Node {
  NK kind = NK::Num;
  char op = 0;
  double num = 0;
  bool b = false;
  core::Err err = core::Err::None;
  std::string str;         // Str, Name, Func (uppercase)
  RefSpec ref;
  int32_t func_id = -1;    // built-in function id; -1 = unknown (possibly a Python UDF)
  uint32_t rows = 0, cols = 0;  // Array
  std::vector<Node> kids;
};

// compiled formula definition (shared)
struct CompiledFormula : model::FormulaDef {
  Node root;
};

}  // namespace yc::formula
