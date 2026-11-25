// SPDX-License-Identifier: Apache-2.0
// Every text on a sheet that names cells, in one place. What moves, renames, breaks or copies references walks these
// lists, never the kinds one by one: five places kept lists of their own and each missed a different kind (a copied
// sheet's Solver model named the source, an icon set's formula thresholds stayed put when rows were inserted, a color
// scale's formulas kept a deleted sheet's name, a scatter chart's X error bars never followed their cells).
// schema/model_fields.yaml says for every field of the model whether it names cells and what keeps it right;
// scripts/check_model_fields.py fails on a field it doesn't list, so a new kind of text is added here with it.
#pragma once

#include "core/cow.h"
#include "core/geometry.h"
#include "model/sheet.h"
#include "model/workbook.h"

#include <string>
#include <utility>
#include <vector>

namespace yc::model {

// the texts of a conditional format that name cells (without '='), relative to the rule's first cell
template <class Fn>
void ForEachCfRefText(CfRule& r, Fn&& fn) {
  fn(r.formula1);
  fn(r.formula2);
  for (auto& f : r.point_formula) fn(f);
  fn(r.bar_min_formula);
  fn(r.bar_max_formula);
  for (auto& th : r.icon_thresholds) fn(th.formula);
}

// the texts of a data validation rule that name cells, relative to the rule's first cell: fn(text, constants_ok), where
// constants_ok says a text without '=' is a list of values (Buy,Sell), not a formula
template <class Fn>
void ForEachDvRefText(DvRule& r, Fn&& fn) {
  fn(r.formula1, r.type == DvType::List);
  fn(r.formula2, false);
}

namespace detail {
// applies `each` to a copy of the list and stores the copy only when a text changed, so a sheet shared with a
// snapshot isn't copied for nothing
template <class T, class Each, class Fn>
bool EditList(core::Cow<Sheet>& sc, core::Cow<std::vector<T>> Sheet::*member, Each&& each, Fn&& fn) {
  const std::vector<T>& list = *((*sc).*member);
  if (list.empty()) return false;
  std::vector<T> copy = list;
  bool any = false;
  for (auto& x : copy) {
    each(x, [&](std::string& t) {
      if (t.empty()) return;
      const std::string before = t;
      fn(t);
      any = any || t != before;
    });
  }
  if (any) (sc.mut().*member).mut() = std::move(copy);
  return any;
}
}  // namespace detail

// Every object text on a sheet that names cells by fixed references, read at A1 with the sheet as context: the
// charts' ranges and linked texts, form controls' linked cells and lists, sparkline groups' date axes and the Solver
// model. fn(std::string&) may rewrite a text; returns whether any changed.
template <class Fn>
bool EditObjectRefTexts(core::Cow<Sheet>& sc, Fn&& fn) {
  bool any = false;
  any = detail::EditList(sc, &Sheet::charts, [](ChartObj& c, auto&& f) { ForEachChartRefText(c, f); }, fn) || any;
  any = detail::EditList(sc, &Sheet::controls, [](Control& c, auto&& f) { ForEachControlRefText(c, f); }, fn) || any;
  any = detail::EditList(sc, &Sheet::sparklines, [](SparklineGroup& g, auto&& f) { ForEachSparklineRefText(g, f); }, fn) || any;
  if (sc->solver->defined) {
    SolverModel m = *sc->solver;
    bool changed = false;
    ForEachSolverRefText(m, [&](std::string& t) {
      if (t.empty()) return;
      const std::string before = t;
      fn(t);
      changed = changed || t != before;
    });
    if (changed) sc.mut().solver.mut() = std::move(m);
    any = any || changed;
  }
  return any;
}

// Every rule text on a sheet (conditional formats, data validation): fn(std::string& text, core::CellPos first_cell,
// bool constants_ok), where first_cell is the cell its relative references count from. Returns whether any changed.
// Code that splits or moves the rules' ranges as well (MoveCells, ShiftSheet) walks a rule's texts with
// ForEachCfRefText / ForEachDvRefText.
template <class Fn>
bool EditRuleRefTexts(core::Cow<Sheet>& sc, Fn&& fn) {
  auto first = [](const std::vector<core::Rect>& ranges) {
    return ranges.empty() ? core::CellPos{0, 0} : core::CellPos{ranges.front().r0, ranges.front().c0};
  };
  bool any = false;
  if (!sc->cond_formats->empty()) {
    std::vector<CfRule> v = *sc->cond_formats;
    bool changed = false;
    for (auto& r : v) {
      const core::CellPos at = first(r.ranges);
      ForEachCfRefText(r, [&](std::string& t) {
        if (t.empty()) return;
        const std::string before = t;
        fn(t, at, false);
        changed = changed || t != before;
      });
    }
    if (changed) sc.mut().cond_formats.mut() = std::move(v);
    any = any || changed;
  }
  if (!sc->validations->empty()) {
    std::vector<DvRule> v = *sc->validations;
    bool changed = false;
    for (auto& r : v) {
      const core::CellPos at = first(r.ranges);
      ForEachDvRefText(r, [&](std::string& t, bool constants_ok) {
        if (t.empty()) return;
        const std::string before = t;
        fn(t, at, constants_ok);
        changed = changed || t != before;
      });
    }
    if (changed) sc.mut().validations.mut() = std::move(v);
    any = any || changed;
  }
  return any;
}

// Every text of the workbook's custom panels that names cells: their controls' linked cells and lists when they name
// a sheet ("Data!$B$3"); one without a sheet reads the active sheet, whichever it is, and stays as written.
// fn(std::string&) may rewrite a text; returns whether any changed. (Defined names, each n.refers_to, are walked by
// each operation itself: a name's relative references follow rules of their own.)
template <class Fn>
bool EditWorkbookRefTexts(Workbook& wb, Fn&& fn) {
  if (wb.panels->empty()) return false;
  std::vector<Panel> copy = *wb.panels;
  bool any = false;
  for (auto& p : copy) {
    for (auto& c : p.controls) {
      for (std::string* t : {&c.linked_cell, &c.list_fill_range}) {
        if (t->find('!') == std::string::npos) continue;
        const std::string before = *t;
        fn(*t);
        any = any || *t != before;
      }
    }
  }
  if (any) wb.panels.mut() = std::move(copy);
  return any;
}

}  // namespace yc::model
