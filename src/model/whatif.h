// SPDX-License-Identifier: Apache-2.0
// whatif.h - What-If Analysis kept with a sheet: data tables, scenarios and the Solver model
#pragma once

#include "core/geometry.h"
#include "core/value.h"

#include <string>
#include <vector>

namespace yc::model {

// Data -> What-If Analysis -> Data Table: {=TABLE(row_input, col_input)} over rect. The top row and / or left column
// hold the input values and the formulas; the body is computed by putting each input value into the input cell(s)
struct DataTable {
  core::Rect rect{};
  bool has_row = false, has_col = false;  // row input cell / column input cell given
  uint32_t row_r = 0, row_c = 0, col_r = 0, col_c = 0;
  // the computed cells (rect without its top row and left column)
  core::Rect Body() const { return core::Rect{rect.r0 + 1, rect.c0 + 1, rect.r1, rect.c1}; }
  bool operator==(const DataTable&) const = default;
};

// Data > Consolidate: the dialog's settings and its All references list, kept with the destination sheet (.ycg SHET 81)
struct ConsolidateSettings {
  int func = 0;  // 0 Sum, 1 Count, 2 Average, 3 Max, 4 Min, 5 Product, 6 Count Numbers, 7 StdDev, 8 StdDevp, 9 Var, 10 Varp
  bool top_row = false, left_col = false, links = false;
  std::vector<std::string> refs;
  bool operator==(const ConsolidateSettings&) const = default;
};

// Scenario Manager: a named set of values for the changing cells
struct Scenario {
  std::string name, comment;
  std::vector<core::CellPos> cells;
  std::vector<core::Value> values;
  bool locked = true, hidden = false;
  bool operator==(const Scenario&) const = default;
};

// Solver (Data -> Solver): the model of the sheet, like the hidden solver_* names Excel keeps
struct SolverConstraint {
  std::string ref;  // cells, e.g. $B$2:$B$5
  int rel = 1;      // 1 <=, 2 =, 3 >=, 4 int, 5 bin, 6 dif (AllDifferent)
  std::string rhs;  // a number, a reference or a formula ("=B10*2")
  bool operator==(const SolverConstraint&) const = default;
};
struct SolverModel {
  bool defined = false;
  std::string objective;  // the objective cell, e.g. $D$10 (empty: only the constraints)
  int goal = 1;           // 1 max, 2 min, 3 value of
  double value = 0;
  std::string changing;   // the variable cells, e.g. $B$2:$B$5,$C$7
  std::vector<SolverConstraint> constraints;
  bool non_negative = true;
  int method = 1;          // 1 GRG Nonlinear, 2 Simplex LP, 3 Evolutionary
  int max_iterations = 1000;
  double precision = 1e-6;
  double max_time = 100;   // seconds
  // Options (Excel's Solver Options dialog; .ycg SHET 80). All Methods: precision above is Constraint Precision,
  // max_time / max_iterations are the Solving Limits
  bool auto_scaling = true, show_iterations = false, ignore_integer = false;
  double integer_optimality = 1;  // % (the gap within which an integer solution is accepted)
  int max_subproblems = 0, max_feasible = 0;  // 0 = no limit (Excel's empty box)
  // GRG Nonlinear: Convergence, Derivatives (1 forward, 2 central), Multistart; Evolutionary shares Convergence,
  // Population Size, Random Seed and Require Bounds on Variables
  double convergence = 0.0001;
  int derivatives = 1;
  bool multistart = false, require_bounds = true;
  int population = 100, random_seed = 0;  // 0 = Solver picks the seed
  double mutation_rate = 0.075, max_time_no_improve = 30;
  bool operator==(const SolverModel&) const = default;
};

// the texts of a Solver model that refer to cells (Excel keeps them as the sheet's solver_* names, which follow
// inserts, deletes, renames and moves): code that rewrites references goes through this list
template <class Fn>
void ForEachSolverRefText(SolverModel& m, Fn&& fn) {
  if (!m.defined) return;
  if (!m.objective.empty()) fn(m.objective);
  if (!m.changing.empty()) {  // areas joined by commas (a formula reads a union only in parentheses): one at a time
    std::string out;
    size_t at = 0;
    while (at <= m.changing.size()) {
      size_t comma = m.changing.find(',', at);
      if (comma == std::string::npos) comma = m.changing.size();
      std::string part = m.changing.substr(at, comma - at);
      if (!part.empty()) fn(part);
      out += (at ? "," : "") + part;
      at = comma + 1;
    }
    m.changing = std::move(out);
  }
  for (auto& c : m.constraints) {
    if (!c.ref.empty()) fn(c.ref);
    if (!c.rhs.empty()) fn(c.rhs);
  }
}

}  // namespace yc::model
