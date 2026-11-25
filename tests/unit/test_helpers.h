// SPDX-License-Identifier: Apache-2.0
// For tests: work on a Document with A1 addresses
#pragma once

#include "core/geometry.h"
#include "engine/document.h"
#include "formula/numfmt.h"

#include <string>

namespace yctest {

struct Doc {
  yc::engine::Document d;
  uint32_t sheet() const { return d.wb().sheets[0]->id; }
  static yc::core::CellPos P(const char* a1) {
    yc::core::CellPos p;
    yc::core::ParseCellA1(a1, &p);
    return p;
  }
  static yc::core::Rect Range(const char* a1) {
    yc::core::Rect r;
    yc::core::ParseRectA1(a1, &r);
    return r;
  }
  bool Set(const char* a1, const std::string& text, uint32_t sh = 0) {
    const auto p = P(a1);
    const bool ok = d.SetInput(sh ? sh : sheet(), p.row, p.col, text);
    d.Recalc();
    return ok;
  }
  const yc::core::Value& V(const char* a1, uint32_t sh = 0) {
    const auto p = P(a1);
    return d.GetValue(sh ? sh : sheet(), p.row, p.col);
  }
  double N(const char* a1, uint32_t sh = 0) { return V(a1, sh).AsDouble(); }
  // display text (with the number format applied)
  std::string T(const char* a1, uint32_t sh = 0) {
    const auto p = P(a1);
    const uint32_t s = sh ? sh : sheet();
    const auto* st = d.S(s);
    const auto& style = d.wb().styles->Get(st->GetStyle(p.row, p.col));
    return yc::engine::DisplayTextOf(d.GetValue(s, p.row, p.col), d.wb().styles->NumberFormat(style));
  }
  std::string F(const char* a1, uint32_t sh = 0) {
    const auto p = P(a1);
    return d.GetInputText(sh ? sh : sheet(), p.row, p.col);
  }
  // evaluates one formula (put in Z1000)
  std::string Eval(const std::string& formula) {
    Set("Z1000", "=" + formula);
    return yc::engine::DisplayTextOf(V("Z1000"), "General");
  }
};

}  // namespace yctest
