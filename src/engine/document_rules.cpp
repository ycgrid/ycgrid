// SPDX-License-Identifier: Apache-2.0
// document_rules.cpp - model operations for conditional formatting, data validation and AutoFilter (engine thread)
#include "engine/document.h"
#include "engine/rule_eval.h"

#include "formula/collate.h"
#include "formula/datetime.h"
#include "formula/fn_util.h"
#include "formula/functions.h"
#include "formula/numfmt.h"

#include <algorithm>
#include <tuple>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace yc::engine {

using model::Sheet;

std::vector<Rect> SubtractRect(const Rect& a, const Rect& b) {
  if (!a.Intersects(b)) return {a};
  std::vector<Rect> out;
  const Rect i{std::max(a.r0, b.r0), std::max(a.c0, b.c0), std::min(a.r1, b.r1), std::min(a.c1, b.c1)};
  if (a.r0 < i.r0) out.push_back(Rect{a.r0, a.c0, i.r0 - 1, a.c1});               // up
  if (i.r1 < a.r1) out.push_back(Rect{i.r1 + 1, a.c0, a.r1, a.c1});               // down
  if (a.c0 < i.c0) out.push_back(Rect{i.r0, a.c0, i.r1, i.c0 - 1});               // left
  if (i.c1 < a.c1) out.push_back(Rect{i.r0, i.c1 + 1, i.r1, a.c1});               // right
  return out;
}

namespace {

// Excel wildcards: * any text, ? one character, ~ escape: the formulas' matcher (a pattern's * meets a * in the text as
// a wildcard; ? is one UTF-8 character)
bool Glob(std::string_view p, std::string_view s) { return formula::fn::WildcardMatch(p, s); }

}  // namespace

// a custom AutoFilter criterion as Excel 16 applies it (measured on 20 kinds of cells and 29 criteria):
// - equals compares what the cell shows, whatever it holds: =5 shows 5, 05 and the text 5; =1,000 only a number
//   shown 1,000, not one shown 1000; =05 shows nothing. A pattern (* ?) looks at texts only, and never at an empty
//   cell (an empty text is one); without * or ? a ~ is itself (~~ isn't a tilde)
// - does not equal hides a cell of the criterion's own kind and value: <>5 hides 5 and 05 but not the text 5, <>1000
//   hides 1000 shown 1,000 too, <>TRUE hides TRUE but not the text TRUE; with a pattern it hides texts only
// - greater / less than compare numbers with a number (2024/1/1 and 50% are numbers) and texts with a text, in
//   Excel's text order; every other cell is hidden (>=5 hides the text 5, TRUE and errors; <a hides numbers)
bool MatchCriterion(const std::string& crit, const core::Value& v, const std::string& text) {
  std::string_view c = crit;
  std::string op = "=";
  for (const char* o : {">=", "<=", "<>", ">", "<", "="}) {
    const size_t n = std::strlen(o);
    if (c.substr(0, n) == o) {
      op = o;
      c.remove_prefix(n);
      break;
    }
  }
  const std::string operand(c);
  const bool blank = v.IsEmpty() || (v.IsString() && v.str().empty());
  if (operand.empty()) return op == "=" ? blank : (op == "<>" ? !blank : false);
  const bool pattern = operand.find_first_of("*?") != std::string::npos;
  const std::string a = formula::FoldCase(text), b = formula::FoldCase(operand);
  if (op == "=" || op == "<>") {
    bool same;
    if (pattern) {
      same = v.IsString() && Glob(b, a);
    } else if (op == "=") {
      same = a == b;
    } else {
      const core::Value k = formula::ParseInput(operand).value;
      if (k.IsNumeric()) same = v.IsNumeric() && v.AsDouble() == k.AsDouble();
      else if (k.IsBool()) same = v.IsBool() && v.boolean() == k.boolean();
      else if (k.IsError()) same = v.IsError() && v.error() == k.error();
      else same = v.IsString() && formula::FoldCase(v.str()) == b;
    }
    return op == "=" ? same : !same;
  }
  if (v.IsEmpty()) return false;
  const core::Value k = formula::ParseInput(operand).value;
  int order;
  if (k.IsNumeric()) {
    if (!v.IsNumeric()) return false;
    const double x = v.AsDouble(), y = k.AsDouble();
    order = x < y ? -1 : x > y ? 1 : 0;
  } else {
    if (!v.IsString()) return false;
    order = formula::CollateLowered(a, b);  // Excel's text order (word sort)
  }
  if (op == ">") return order > 0;
  if (op == "<") return order < 0;
  if (op == ">=") return order >= 0;
  return order <= 0;
}

// ---- Notes and hyperlinks ----
// Read-only mode refuses each change of notes, comments, rules, validation and filters below (RefuseReadOnly);
// protection keeps its own checks where Excel has them

void Document::SetNote(uint32_t sheet, uint32_t r, uint32_t c, const std::string& author, const std::string& text) {
  // a note is one of the sheet's objects: protection refuses it unless editing objects is allowed (Excel 16, measured
  // 2026-11-20: AddComment raises on a protected sheet, locked cell or not)
  if (RefuseReadOnly(sheet) || Refuse(sheet, nullptr, Allow::Objects)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  const uint64_t key = (uint64_t{r} << 16) | c;
  if (text.empty()) {
    s->notes.mut().erase(key);
  } else {
    auto it = s->notes->find(key);
    const bool shown = it != s->notes->end() && !it->second.threaded && it->second.visible;  // an edit keeps a shown note shown
    model::CellNote n{author, text};
    n.visible = shown;
    if (it != s->notes->end() && !it->second.threaded) n.look = it->second.look;  // and its box
    s->notes.mut()[key] = std::move(n);
  }
  wb_.dirty = true;
}

bool Document::SetNoteLook(uint32_t sheet, uint32_t r, uint32_t c, const model::CellNote::Look& look) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  const uint64_t key = (uint64_t{r} << 16) | c;
  auto it = s->notes->find(key);
  if (it == s->notes->end() || it->second.threaded) return false;
  NoteTouched();
  model::CellNote::Look lk = look;
  lk.w = std::max(0.0, lk.w), lk.h = std::max(0.0, lk.h);
  s->notes.mut()[key].look = lk;
  wb_.dirty = true;
  return true;
}

int Document::SetNotesVisible(uint32_t sheet, const Rect& area, bool visible) {
  if (RefuseReadOnly(sheet)) return 0;
  Sheet* s = M(sheet);
  if (!s || s->notes->empty()) return 0;
  int n = 0;
  for (const auto& [k, v] : *s->notes) {
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    if (!v.threaded && v.visible != visible && area.Contains(r, c)) ++n;
  }
  if (n == 0) return 0;
  NoteTouched();
  for (auto& [k, v] : s->notes.mut()) {
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    if (!v.threaded && area.Contains(r, c)) v.visible = visible;
  }
  wb_.dirty = true;
  return n;
}

void Document::SetRichText(uint32_t sheet, uint32_t r, uint32_t c, model::RichText rt) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  const uint64_t key = (uint64_t{r} << 16) | c;
  if (!model::Normalize(&rt)) s->rich.mut().erase(key);
  else s->rich.mut()[key] = std::move(rt);
  NoteRegion(sheet, Rect::Cell(r, c));
  changes_.push_back(CellChange{sheet, Rect::Cell(r, c), kSrcUser});
  wb_.dirty = true;
}

const model::RichText* Document::RichTextAt(uint32_t sheet, uint32_t r, uint32_t c) const {
  const Sheet* s = S(sheet);
  if (!s || s->rich->empty()) return nullptr;
  auto it = s->rich->find((uint64_t{r} << 16) | c);
  if (it == s->rich->end()) return nullptr;
  const Value& v = s->GetValue(r, c);
  return v.IsString() && v.str() == it->second.text ? &it->second : nullptr;
}

void Document::EraseRich(uint32_t sheet, const Rect& rect) {
  const Sheet* cs = S(sheet);
  if (!cs || cs->rich->empty()) return;
  bool any = false;
  for (const auto& [k, v] : *cs->rich) any = any || rect.Contains(static_cast<uint32_t>(k >> 16), static_cast<uint32_t>(k & 0xFFFF));
  if (!any) return;
  NoteTouched();
  auto& m = M(sheet)->rich.mut();
  for (auto it = m.begin(); it != m.end();) {
    if (rect.Contains(static_cast<uint32_t>(it->first >> 16), static_cast<uint32_t>(it->first & 0xFFFF))) it = m.erase(it);
    else ++it;
  }
}

namespace {
template <class M>
bool EraseInRect(M& m, const Rect& a) {
  bool any = false;
  for (auto it = m.begin(); it != m.end();) {
    const uint32_t r = static_cast<uint32_t>(it->first >> 16), c = static_cast<uint32_t>(it->first & 0xFFFF);
    if (a.Contains(r, c)) {
      it = m.erase(it);
      any = true;
    } else {
      ++it;
    }
  }
  return any;
}
}  // namespace

void Document::ClearNotes(uint32_t sheet, const Rect& area) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || s->notes->empty()) return;
  NoteTouched();
  if (EraseInRect(s->notes.mut(), area)) wb_.dirty = true;
}

void Document::SetComment(uint32_t sheet, uint32_t r, uint32_t c, const std::string& author, const std::string& text, double date) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  const uint64_t key = (uint64_t{r} << 16) | c;
  if (text.empty()) {
    s->notes.mut().erase(key);
  } else {
    model::CellNote n;
    n.author = author;
    n.text = text;
    n.threaded = true;
    n.date = date;
    s->notes.mut()[key] = std::move(n);
  }
  wb_.dirty = true;
}

namespace {
model::CellNote* ThreadAt(model::Sheet* s, uint32_t r, uint32_t c) {
  if (!s) return nullptr;
  const uint64_t key = (uint64_t{r} << 16) | c;
  if (s->notes->find(key) == s->notes->end()) return nullptr;
  model::CellNote& n = s->notes.mut()[key];
  return n.threaded ? &n : nullptr;
}
}  // namespace

bool Document::AddCommentReply(uint32_t sheet, uint32_t r, uint32_t c, const std::string& author, const std::string& text, double date) {
  if (RefuseReadOnly(sheet)) return false;
  if (text.empty()) return false;
  NoteTouched();
  model::CellNote* n = ThreadAt(M(sheet), r, c);
  if (!n) return false;
  n->replies.push_back(model::NoteReply{author, text, date});
  n->resolved = false;  // a new reply reopens the thread, as in Excel
  wb_.dirty = true;
  return true;
}

bool Document::EditCommentPost(uint32_t sheet, uint32_t r, uint32_t c, int reply, const std::string& text) {
  if (RefuseReadOnly(sheet)) return false;
  if (text.empty()) return false;
  NoteTouched();
  model::CellNote* n = ThreadAt(M(sheet), r, c);
  if (!n || reply >= static_cast<int>(n->replies.size())) return false;
  (reply < 0 ? n->text : n->replies[static_cast<size_t>(reply)].text) = text;
  wb_.dirty = true;
  return true;
}

bool Document::DeleteCommentReply(uint32_t sheet, uint32_t r, uint32_t c, int reply) {
  if (RefuseReadOnly(sheet)) return false;
  NoteTouched();
  model::CellNote* n = ThreadAt(M(sheet), r, c);
  if (!n || reply < 0 || reply >= static_cast<int>(n->replies.size())) return false;
  n->replies.erase(n->replies.begin() + reply);
  wb_.dirty = true;
  return true;
}

bool Document::ResolveComment(uint32_t sheet, uint32_t r, uint32_t c, bool resolved) {
  if (RefuseReadOnly(sheet)) return false;
  NoteTouched();
  model::CellNote* n = ThreadAt(M(sheet), r, c);
  if (!n) return false;
  n->resolved = resolved;
  wb_.dirty = true;
  return true;
}

int Document::ConvertNotesToComments(uint32_t sheet, const Rect& area, double date) {
  if (RefuseReadOnly(sheet)) return 0;
  Sheet* s = M(sheet);
  if (!s || s->notes->empty()) return 0;
  NoteTouched();
  int n = 0;
  for (auto& [k, v] : s->notes.mut()) {
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    if (v.threaded || !area.Contains(r, c)) continue;
    v.threaded = true;
    v.date = date;
    ++n;
  }
  if (n) wb_.dirty = true;
  return n;
}

void Document::SetHyperlink(uint32_t sheet, uint32_t r, uint32_t c, const model::Hyperlink& link) {
  if (Refuse(sheet, nullptr, Allow::InsertHyperlinks)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  const uint64_t key = (uint64_t{r} << 16) | c;
  if (link.address.empty() && link.sub_address.empty()) s->links.mut().erase(key);
  else s->links.mut()[key] = link;
  wb_.dirty = true;
}

void Document::ClearHyperlinks(uint32_t sheet, const Rect& area) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || s->links->empty()) return;
  NoteTouched();
  if (EraseInRect(s->links.mut(), area)) wb_.dirty = true;
}

// ---- Conditional formatting ----

uint32_t Document::AddCondFormat(uint32_t sheet, model::CfRule rule) {
  if (RefuseReadOnly(sheet)) return 0;
  Sheet* s = M(sheet);
  if (!s) return 0;
  NoteTouched();
  if (!rule.fmt.numfmt.empty()) wb_.styles.mut().InternString(rule.fmt.numfmt);  // drawn by its id
  s = M(sheet);
  rule.id = wb_.next_object_id++;
  auto& v = s->cond_formats.mut();
  if (rule.priority < 0) {
    // a negative priority asks for the last place: Excel's FormatConditions.Add appends (Priority = Count)
    int last = -1;
    for (const auto& r : v) last = std::max(last, r.priority);
    rule.priority = last + 1;
    const uint32_t id = rule.id;
    v.push_back(std::move(rule));
    wb_.dirty = true;
    return id;
  }
  // new rules first (like Excel's UI: a new rule goes on top)
  for (auto& r : v) ++r.priority;
  rule.priority = 0;
  v.insert(v.begin(), std::move(rule));
  wb_.dirty = true;
  return v.front().id;
}

bool Document::UpdateCondFormat(uint32_t sheet, uint32_t id, const std::function<void(model::CfRule&)>& fn) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  for (auto& r : s->cond_formats.mut()) {
    if (r.id != id) continue;
    NoteTouched();
    fn(r);
    if (!r.fmt.numfmt.empty()) wb_.styles.mut().InternString(r.fmt.numfmt);  // drawn by its id
    wb_.dirty = true;
    return true;
  }
  return false;
}

bool Document::DeleteCondFormat(uint32_t sheet, uint32_t id) {
  if (RefuseReadOnly(sheet)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  auto& v = s->cond_formats.mut();
  const size_t n = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::CfRule& r) { return r.id == id; }), v.end());
  if (v.size() == n) return false;
  NoteTouched();
  wb_.dirty = true;
  return true;
}

void Document::SetCondFormats(uint32_t sheet, std::vector<model::CfRule> rules) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  rules.erase(std::remove_if(rules.begin(), rules.end(), [](const model::CfRule& r) { return r.ranges.empty(); }), rules.end());
  for (size_t i = 0; i < rules.size(); ++i) {
    if (!rules[i].id) rules[i].id = wb_.next_object_id++;
    rules[i].priority = static_cast<int>(i);
    if (!rules[i].fmt.numfmt.empty()) wb_.styles.mut().InternString(rules[i].fmt.numfmt);  // drawn by its id
  }
  s->cond_formats.mut() = std::move(rules);
  for (const auto& r : *s->cond_formats) {
    for (const Rect& x : r.ranges) changes_.push_back(CellChange{sheet, x, kSrcUser});
  }
  wb_.dirty = true;
}

void Document::ClearCondFormats(uint32_t sheet, const Rect& area) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || s->cond_formats->empty()) return;
  NoteTouched();
  auto& v = s->cond_formats.mut();
  for (auto& r : v) {
    std::vector<Rect> next;
    for (const Rect& x : r.ranges) {
      for (const Rect& y : SubtractRect(x, area)) next.push_back(y);
    }
    r.ranges = std::move(next);
  }
  v.erase(std::remove_if(v.begin(), v.end(), [](const model::CfRule& r) { return r.ranges.empty(); }), v.end());
  wb_.dirty = true;
}

// ---- Data validation ----

void Document::SetValidation(uint32_t sheet, const Rect& area, const model::DvRule& rule) {
  // a protected sheet takes no validation rule, locked cells or not (Excel 16, measured 2026-11-20: Validation.Add raises)
  if (RefuseReadOnly(sheet) || Refuse(sheet, nullptr, Allow::Never)) return;
  ClearValidation(sheet, area);
  Sheet* s = M(sheet);
  if (!s) return;
  NoteTouched();
  model::DvRule r = rule;
  r.ranges = {area};
  s->validations.mut().push_back(std::move(r));
  wb_.dirty = true;
}

// Fill Down / Right / Up / Left and the fill handle carry data validation as a copy does, and no notes (Excel 16,
// measured 2026-11-19: FillRight, FillDown and AutoFill of a cell with a list rule and a note give the list and no
// note; a source without a rule takes the destination's away; two source cells, one with a rule, repeat that way; a
// relative list =G1:G2 filled from H1 to H3 is =G3:G4 there). Each filled cell takes the rule of the source cell it
// repeats: the rule grows by the cells it reaches (no new rules, so relative formulas keep their meaning from the
// rule's first cell), and the lanes that take it join. src: the source lines; target: the cells filled, beyond src
// along the fill (reverse: up or left); n source cells a lane.
void Document::FillValidations(uint32_t sheet, const Rect& src, const Rect& target, bool vertical, bool reverse) {
  if (target.r0 > target.r1 || target.c0 > target.c1) return;
  const Sheet* s0 = S(sheet);
  if (!s0 || s0->validations->empty()) return;
  ClearValidation(sheet, target);
  Sheet* s = M(sheet);
  if (!s || s->validations->empty()) return;
  const uint32_t n = vertical ? static_cast<uint32_t>(src.Rows()) : static_cast<uint32_t>(src.Cols());
  const uint32_t lanes = vertical ? static_cast<uint32_t>(src.Cols()) : static_cast<uint32_t>(src.Rows());
  const auto& rules = *s->validations;
  auto rule_at = [&](uint32_t r, uint32_t c) -> int {
    for (size_t i = 0; i < rules.size(); ++i) {
      for (const Rect& x : rules[i].ranges) {
        if (x.Contains(r, c)) return static_cast<int>(i);
      }
    }
    return -1;
  };
  // the span of the target a lane covers, along the fill
  const uint32_t t0 = vertical ? target.r0 : target.c0, t1 = vertical ? target.r1 : target.c1;
  std::vector<std::vector<Rect>> grown(rules.size());
  uint64_t cells = 0;
  for (uint32_t l = 0; l < lanes; ++l) {
    std::vector<int> ri(n);
    bool one = true;
    for (uint32_t k = 0; k < n; ++k) {
      ri[k] = vertical ? rule_at(src.r0 + k, src.c0 + l) : rule_at(src.r0 + l, src.c0 + k);
      one = one && ri[k] == ri[0];
    }
    auto lane_rect = [&](uint32_t a, uint32_t b) {
      return vertical ? Rect{a, src.c0 + l, b, src.c0 + l} : Rect{src.r0 + l, a, src.r0 + l, b};
    };
    if (one) {  // the whole lane takes one rule (one source cell, or a source all under the same rule)
      if (ri[0] >= 0) grown[static_cast<size_t>(ri[0])].push_back(lane_rect(t0, t1));
      continue;
    }
    // a pattern: each cell the rule of the source cell it repeats, counted from the source outward
    for (uint32_t p = t0; p <= t1 && cells < 1'000'000; ++p) {
      const uint32_t t = reverse ? t1 - p : p - t0;
      const uint32_t k = reverse ? (n - 1) - (t % n) : t % n;
      if (ri[k] < 0) continue;
      auto& v = grown[static_cast<size_t>(ri[k])];
      const Rect cell = lane_rect(p, p);
      Rect* last = v.empty() ? nullptr : &v.back();
      if (last && (vertical ? (last->c0 == cell.c0 && uint64_t{last->r1} + 1 == cell.r0) : (last->r0 == cell.r0 && uint64_t{last->c1} + 1 == cell.c0))) {
        if (vertical) last->r1 = cell.r1;
        else last->c1 = cell.c1;
      } else {
        v.push_back(cell);
        ++cells;
      }
    }
  }
  bool any = false;
  for (const auto& v : grown) any = any || !v.empty();
  if (!any) return;
  NoteTouched();
  auto& mv = s->validations.mut();
  for (size_t i = 0; i < grown.size(); ++i) {
    auto& v = grown[i];
    // lanes side by side with the same span join (Fill Right of a column of a million rows: one range)
    std::sort(v.begin(), v.end(), [&](const Rect& a, const Rect& b) {
      return vertical ? std::tie(a.r0, a.r1, a.c0) < std::tie(b.r0, b.r1, b.c0) : std::tie(a.c0, a.c1, a.r0) < std::tie(b.c0, b.c1, b.r0);
    });
    std::vector<Rect> joined;
    for (const Rect& x : v) {
      Rect* last = joined.empty() ? nullptr : &joined.back();
      if (last && vertical && last->r0 == x.r0 && last->r1 == x.r1 && uint64_t{last->c1} + 1 == x.c0) last->c1 = x.c1;
      else if (last && !vertical && last->c0 == x.c0 && last->c1 == x.c1 && uint64_t{last->r1} + 1 == x.r0) last->r1 = x.r1;
      else joined.push_back(x);
    }
    mv[i].ranges.insert(mv[i].ranges.end(), joined.begin(), joined.end());
  }
  wb_.dirty = true;
}

void Document::ClearValidation(uint32_t sheet, const Rect& area) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || s->validations->empty()) return;
  NoteTouched();
  auto& v = s->validations.mut();
  for (auto& r : v) {
    std::vector<Rect> next;
    for (const Rect& x : r.ranges) {
      for (const Rect& y : SubtractRect(x, area)) next.push_back(y);
    }
    r.ranges = std::move(next);
  }
  v.erase(std::remove_if(v.begin(), v.end(), [](const model::DvRule& r) { return r.ranges.empty(); }), v.end());
  wb_.dirty = true;
}

// ---- AutoFilter ----

bool Document::SetAutoFilter(uint32_t sheet, const Rect& range) {
  if (Refuse(sheet, nullptr, Allow::AutoFilter)) return false;
  Sheet* s = M(sheet);
  if (!s) return false;
  // a table's buttons that use the sheet's AutoFilter keep their criteria and hidden rows as the table's own, and the
  // sheet's AutoFilter is free for the new list (Excel: one AutoFilter a sheet, besides every table's buttons)
  uint32_t owner = 0;
  for (const auto& t : *s->tables) {
    const bool same = range.r0 == t.range.r0 && range.c0 == t.range.c0 && range.c1 == t.range.c1;
    if (t.header && t.filter_button && !same && TableOwnsSheetFilter(*s, t)) owner = t.id;
  }
  if (owner) {
    NoteTouched();
    const model::AutoFilter was = *s->autofilter;
    for (auto& t : s->tables.mut()) {
      if (t.id != owner) continue;
      t.filter = was;
      t.filter.on = true;
      t.filter.adv_range = {};
    }
    s->autofilter.mut() = model::AutoFilter{};
  } else if (s->autofilter->on) {
    ClearAutoFilter(sheet);
  }
  NoteTouched();
  auto& af = s->autofilter.mut();
  af = model::AutoFilter{};
  af.on = true;
  // as far as the data, as Excel's: Range("3:4").AutoFilter reads A3:H7 over data in A1:H7 (Excel 16, measured
  // 2026-11-16), not the whole rows, whose buttons went to XFD and refused a table over the list (basics seq19);
  // a table's buttons are its own range
  const bool for_table = std::any_of(s->tables->begin(), s->tables->end(), [&](const model::Table& t) {
    return range.r0 == t.range.r0 && range.c0 == t.range.c0 && range.c1 == t.range.c1;
  });
  af.range = for_table ? range : AutoFilterExtent(*s, range);
  wb_.dirty = true;
  return true;
}

void Document::ClearAutoFilter(uint32_t sheet) {
  if (Refuse(sheet, nullptr, Allow::AutoFilter)) return;
  Sheet* s = M(sheet);
  if (!s || !s->autofilter->on) return;
  ShowAllData(sheet);
  NoteTouched();
  s->autofilter.mut() = model::AutoFilter{};
  wb_.dirty = true;
}

void Document::SetFilterColumn(uint32_t sheet, const model::FilterColumn& col) {
  if (Refuse(sheet, nullptr, Allow::AutoFilter)) return;
  Sheet* s = M(sheet);
  if (!s || !s->autofilter->on) return;
  NoteTouched();
  auto& cols = s->autofilter.mut().columns;
  cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const model::FilterColumn& c) { return c.field == col.field; }), cols.end());
  if (col.kind != 0) cols.push_back(col);
  ApplyAutoFilter(sheet);
}

void Document::ShowAllData(uint32_t sheet) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || (s->autofilter->hidden.empty() && s->autofilter->columns.empty())) return;  // Clear also drops criteria that hide nothing
  NoteTouched();
  auto& af = s->autofilter.mut();
  auto& d = s->rows.mut();
  uint32_t lo = UINT32_MAX, hi = 0;
  for (const auto& [r, sz] : af.hidden) {
    d.Hide(r, false);
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  af.hidden.clear();
  af.columns.clear();
  if (lo <= hi) MarkChanged(sheet, Rect{lo, 0, hi, core::kMaxCols - 1}, kSrcCalc | kSrcNoEvents);
  wb_.dirty = true;
  // rows a table's own filter hides stay hidden
  bool tables = false;
  for (const auto& t : *s->tables) tables = tables || !t.filter.columns.empty();
  if (tables) ApplyAutoFilter(sheet);
}

Rect AutoFilterExtent(const model::Sheet& s, const Rect& range) {
  Rect r = range;
  if (!s.has_used) return r;
  if (r.r1 > s.used.r1) r.r1 = std::max(r.r0, s.used.r1);  // a range given as whole columns: up to the data
  if (r.c1 > s.used.c1) r.c1 = std::max(r.c0, s.used.c1);  // whole rows: up to the data's last column
  auto has_data = [&](uint32_t row) {
    for (uint32_t c = r.c0; c <= r.c1; ++c) {
      if (!s.GetValue(row, c).IsEmpty()) return true;
    }
    return false;
  };
  while (r.r1 < s.used.r1 && r.r1 + 1 < s.row_limit && has_data(r.r1 + 1)) ++r.r1;
  return r;
}

FilterTester::FilterTester(const model::Workbook& wb, const model::Sheet& s, const model::AutoFilter& af, const Rect& range)
    : wb_(wb), s_(s), af_(af), rg_(range) {
  if (s.has_used) rg_.r1 = std::min(rg_.r1, std::max(s.used.r1, rg_.r0));
  today_ = std::floor(formula::NowSerial());
  formula::SerialToYmd(today_, &ty_, &tm_, &td_);
  // top N: compute the threshold first (and the average for above / below average)
  thresholds_.assign(af.columns.size(), 0.0);
  values_.resize(af.columns.size());
  for (size_t i = 0; i < af.columns.size(); ++i) {
    if (af.columns[i].kind != 1) continue;
    for (const std::string& v : af.columns[i].values) values_[i].insert(Lower(v));
  }
  for (size_t i = 0; i < af.columns.size(); ++i) {
    const auto& fc = af.columns[i];
    if (fc.kind != 6 || (fc.dynamic != 33 && fc.dynamic != 34)) continue;
    double sum = 0;
    size_t n = 0;
    for (uint32_t r = rg_.r0 + 1; r <= rg_.r1; ++r) {
      const core::Value& v = s.GetValue(r, rg_.c0 + fc.field);
      if (v.IsNumeric()) sum += v.AsDouble(), ++n;
    }
    thresholds_[i] = n ? sum / static_cast<double>(n) : 0.0;
  }
  // Filter by Cell Icon: the icons conditional formatting shows in those columns
  icons_.assign(af.columns.size(), {});
  bool any_icon = false;
  for (const auto& fc : af.columns) any_icon = any_icon || fc.kind == 7;
  if (any_icon && rg_.r1 > rg_.r0) {
    CondFormatEval ev(wb, s.id);
    for (size_t i = 0; i < af.columns.size(); ++i) {
      if (af.columns[i].kind != 7) continue;
      auto& col = icons_[i];
      col.assign(rg_.r1 - rg_.r0, -1);
      for (uint32_t r = rg_.r0 + 1; r <= rg_.r1; ++r) {
        CfVisual v;
        if (!ev.Empty() && ev.Eval(r, rg_.c0 + af.columns[i].field, &v) && v.icon >= 0) col[r - rg_.r0 - 1] = (int{v.icon_style} << 8) | v.icon;
      }
    }
  }
  for (size_t i = 0; i < af.columns.size(); ++i) {
    const auto& fc = af.columns[i];
    if (fc.kind != 3) continue;
    std::vector<double> nums;
    for (uint32_t r = rg_.r0 + 1; r <= rg_.r1; ++r) {
      const core::Value& v = s.GetValue(r, rg_.c0 + fc.field);
      if (v.IsNumeric()) nums.push_back(v.AsDouble());
    }
    if (nums.empty()) continue;
    size_t n = fc.percent ? static_cast<size_t>(std::ceil(nums.size() * std::clamp(fc.count, 0, 100) / 100.0)) : static_cast<size_t>(std::max(0, fc.count));
    n = std::clamp<size_t>(n, 1, nums.size());
    std::sort(nums.begin(), nums.end());
    thresholds_[i] = fc.top ? nums[nums.size() - n] : nums[n - 1];
  }
}

bool FilterTester::Passes(uint32_t r, int except_field) const {
  const auto& tbl = *wb_.styles;
  auto text_of = [&](uint32_t c) { return DisplayTextOf(s_.GetValue(r, c), tbl.NumberFormat(tbl.Get(s_.GetStyle(r, c)))); };
  // dynamic date ranges [lo, hi)
  const double today = today_;
  const int ty = ty_, tm = tm_;
  auto dyn_range = [&](int code, double* lo2, double* hi2) {
    const int wd = formula::Weekday(today);  // 1 = Sunday
    const double week0 = today - (wd - 1);
    const int q = (tm - 1) / 3;
    switch (code) {
      case 1: *lo2 = today, *hi2 = today + 1; return true;
      case 2: *lo2 = today - 1, *hi2 = today; return true;
      case 3: *lo2 = today + 1, *hi2 = today + 2; return true;
      case 4: *lo2 = week0, *hi2 = week0 + 7; return true;
      case 5: *lo2 = week0 - 7, *hi2 = week0; return true;
      case 6: *lo2 = week0 + 7, *hi2 = week0 + 14; return true;
      case 7: *lo2 = formula::DateSerial(ty, tm, 1), *hi2 = formula::DateSerial(ty, tm + 1, 1); return true;
      case 8: *lo2 = formula::DateSerial(ty, tm - 1, 1), *hi2 = formula::DateSerial(ty, tm, 1); return true;
      case 9: *lo2 = formula::DateSerial(ty, tm + 1, 1), *hi2 = formula::DateSerial(ty, tm + 2, 1); return true;
      case 10: *lo2 = formula::DateSerial(ty, q * 3 + 1, 1), *hi2 = formula::DateSerial(ty, q * 3 + 4, 1); return true;
      case 11: *lo2 = formula::DateSerial(ty, q * 3 - 2, 1), *hi2 = formula::DateSerial(ty, q * 3 + 1, 1); return true;
      case 12: *lo2 = formula::DateSerial(ty, q * 3 + 4, 1), *hi2 = formula::DateSerial(ty, q * 3 + 7, 1); return true;
      case 13: *lo2 = formula::DateSerial(ty, 1, 1), *hi2 = formula::DateSerial(ty + 1, 1, 1); return true;
      case 14: *lo2 = formula::DateSerial(ty - 1, 1, 1), *hi2 = formula::DateSerial(ty, 1, 1); return true;
      case 15: *lo2 = formula::DateSerial(ty + 1, 1, 1), *hi2 = formula::DateSerial(ty + 2, 1, 1); return true;
      case 16: *lo2 = formula::DateSerial(ty, 1, 1), *hi2 = today + 1; return true;
      default: return false;
    }
  };
  bool show = true;
  for (size_t i = 0; i < af_.columns.size() && show; ++i) {
    const auto& fc = af_.columns[i];
    if (static_cast<int>(fc.field) == except_field) continue;
    const uint32_t c = rg_.c0 + fc.field;
    const core::Value& v = s_.GetValue(r, c);
    if (fc.kind == 1) {
      const std::string t = text_of(c);
      if (t.empty()) show = fc.blanks;
      else show = values_[i].count(Lower(t)) != 0;
    } else if (fc.kind == 2) {
      const std::string t = text_of(c);
      const bool a = fc.crit1.empty() || MatchCriterion(fc.crit1, v, t);
      if (fc.crit2.empty()) show = a;
      else {
        const bool b = MatchCriterion(fc.crit2, v, t);
        show = fc.or_op ? (a || b) : (a && b);
      }
    } else if (fc.kind == 3) {
      show = v.IsNumeric() && (fc.top ? v.AsDouble() >= thresholds_[i] : v.AsDouble() <= thresholds_[i]);
    } else if (fc.kind == 4 || fc.kind == 5) {
      const model::Style& st = tbl.Get(s_.GetStyle(r, c));
      const uint32_t have = fc.kind == 4 ? st.fill_color : st.font_color;
      const bool none = (have >> 24) == 0, want_none = (fc.color >> 24) == 0;
      show = none ? want_none : (!want_none && (have & 0xFFFFFF) == (fc.color & 0xFFFFFF));
    } else if (fc.kind == 6) {
      if (fc.dynamic == 33 || fc.dynamic == 34) {
        show = v.IsNumeric() && (fc.dynamic == 33 ? v.AsDouble() > thresholds_[i] : v.AsDouble() < thresholds_[i]);
      } else if (fc.dynamic >= 17 && fc.dynamic <= 32) {
        int y = 0, m = 0, dd = 0;
        show = v.IsNumeric() && formula::SerialToYmd(v.AsDouble(), &y, &m, &dd) &&
               (fc.dynamic <= 20 ? (m - 1) / 3 == fc.dynamic - 17 : m == fc.dynamic - 20);
      } else {
        double lo2 = 0, hi2 = 0;
        show = v.IsNumeric() && dyn_range(fc.dynamic, &lo2, &hi2) && v.AsDouble() >= lo2 && v.AsDouble() < hi2;
      }
    } else if (fc.kind == 7) {
      // Filter by Cell Icon: the icon conditional formatting shows in the cell (none: No Cell Icon)
      const auto& col = icons_[i];
      const int got = r > rg_.r0 && r - rg_.r0 - 1 < col.size() ? col[r - rg_.r0 - 1] : -1;
      show = fc.icon_set == 0 ? got < 0 : got == ((fc.icon_set << 8) | fc.icon);
    }
  }
  return show;
}

void Document::SetFilterSort(uint32_t sheet, const std::vector<model::SortLevel>& levels) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s || !s->autofilter->on || s->autofilter->sort_levels == levels) return;
  NoteTouched();
  s->autofilter.mut().sort_levels = levels;
  wb_.dirty = true;
}

void Document::ApplyAutoFilter(uint32_t sheet, bool resort) {
  if (RefuseReadOnly(sheet)) return;
  ApplyAutoFilter(sheet);
  const Sheet* s = S(sheet);
  if (!resort || !s || !s->autofilter->on || s->autofilter->sort_levels.empty()) return;
  // Reapply: the list is sorted again by its recorded levels (the visible rows among themselves)
  const model::AutoFilter& af = *s->autofilter;
  std::vector<SortKey> keys;
  for (const auto& l : af.sort_levels) {
    if (af.range.c0 + l.col <= af.range.c1) keys.push_back(SortKey{af.range.c0 + l.col, l.desc});
  }
  const Rect range = af.range;
  if (!keys.empty()) Sort(sheet, range, keys, true);
}

bool TableOwnsSheetFilter(const model::Sheet& s, const model::Table& t) {
  const model::AutoFilter& af = *s.autofilter;
  return af.on && af.range.r0 == t.range.r0 && af.range.c0 == t.range.c0 && af.range.c1 == t.range.c1;
}

bool TableHasOwnFilter(const model::Sheet& s, const model::Table& t) { return t.filter_button && t.header && !TableOwnsSheetFilter(s, t); }

Rect TableFilterRange(const model::Table& t) {
  Rect r = t.range;
  if (t.totals && r.r1 > r.r0) --r.r1;
  return r;
}

bool FilterHidRow(const model::Sheet& s, uint32_t r) {
  if (s.autofilter->hidden.count(r)) return true;
  for (const auto& t : *s.tables) {
    if (t.filter.hidden.count(r)) return true;
  }
  return false;
}

void Document::ApplyAutoFilter(uint32_t sheet) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  const bool sheet_on = s->autofilter->on;
  bool tables = false;
  for (const auto& t : *s->tables) tables = tables || !t.filter.columns.empty() || !t.filter.hidden.empty();
  if (!sheet_on && !tables) return;
  NoteTouched();
  auto& d = s->rows.mut();
  uint32_t lo = UINT32_MAX, hi = 0;
  auto note = [&](uint32_t r) { lo = std::min(lo, r), hi = std::max(hi, r); };
  // show what the filters hid, then filter again: the sheet's AutoFilter, then each table's own (Excel: one per table)
  if (sheet_on) {
    auto& af = s->autofilter.mut();
    af.range = AutoFilterExtent(*s, af.range);  // rows typed below the list belong to it
    for (const auto& [r, sz] : af.hidden) d.Hide(r, false), note(r);
    af.hidden.clear();
  }
  if (tables) {
    for (auto& t : s->tables.mut()) {
      for (const auto& [r, sz] : t.filter.hidden) d.Hide(r, false), note(r);
      t.filter.hidden.clear();
    }
  }
  if (sheet_on) {
    auto& af = s->autofilter.mut();
    Rect rg = af.range;
    if (s->has_used) rg.r1 = std::min(rg.r1, std::max(s->used.r1, rg.r0));
    if (rg.r1 > rg.r0) {
      const FilterTester test(wb_, *s, af, rg);
      for (uint32_t r = rg.r0 + 1; r <= rg.r1; ++r) {
        if (!test.Passes(r) && !s->rows->IsHidden(r)) {
          d.Hide(r, true);
          af.hidden[r] = 0;
          note(r);
        }
      }
    }
  }
  if (tables) {
    for (auto& t : s->tables.mut()) {
      if (t.filter.columns.empty() || !TableHasOwnFilter(*s, t)) continue;
      Rect rg = TableFilterRange(t);
      if (rg.r1 <= rg.r0) continue;
      const FilterTester test(wb_, *s, t.filter, rg);
      for (uint32_t r = rg.r0 + 1; r <= rg.r1; ++r) {
        if (test.Passes(r)) continue;
        const bool by_sheet = s->autofilter->hidden.count(r) != 0;
        if (!s->rows->IsHidden(r)) d.Hide(r, true), note(r);
        else if (!by_sheet) continue;  // hidden by hand: not the filter's
        t.filter.hidden[r] = 0;
      }
    }
  }
  if (lo <= hi) MarkChanged(sheet, Rect{lo, 0, hi, core::kMaxCols - 1}, kSrcCalc | kSrcNoEvents);
  wb_.dirty = true;
}

void Document::SetTableFilterColumn(uint32_t sheet, uint32_t id, const model::FilterColumn& col) {
  if (Refuse(sheet, nullptr, Allow::AutoFilter)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  for (auto& t : s->tables.mut()) {
    if (t.id != id) continue;
    NoteTouched();
    auto& cols = t.filter.columns;
    cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const model::FilterColumn& c) { return c.field == col.field; }), cols.end());
    if (col.kind != 0) cols.push_back(col);
    t.filter.on = true;
    ApplyAutoFilter(sheet);
    return;
  }
}

void Document::ClearTableFilter(uint32_t sheet, uint32_t id) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  for (auto& t : s->tables.mut()) {
    if (t.id != id || (t.filter.columns.empty() && t.filter.hidden.empty() && t.filter.sort_levels.empty())) continue;
    NoteTouched();
    t.filter.columns.clear();
    t.filter.sort_levels.clear();
    auto& d = s->rows.mut();
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const auto& [r, sz] : t.filter.hidden) {
      if (!s->autofilter->hidden.count(r)) d.Hide(r, false);
      lo = std::min(lo, r), hi = std::max(hi, r);
    }
    t.filter.hidden.clear();
    if (lo <= hi) MarkChanged(sheet, Rect{lo, 0, hi, core::kMaxCols - 1}, kSrcCalc | kSrcNoEvents);
    wb_.dirty = true;
    return;
  }
}

void Document::SetTableFilterSort(uint32_t sheet, uint32_t id, const std::vector<model::SortLevel>& levels) {
  if (RefuseReadOnly(sheet)) return;
  Sheet* s = M(sheet);
  if (!s) return;
  for (auto& t : s->tables.mut()) {
    if (t.id != id || t.filter.sort_levels == levels) continue;
    NoteTouched();
    t.filter.sort_levels = levels;
    wb_.dirty = true;
    return;
  }
}

void Document::SetTableFilterButton(uint32_t sheet, uint32_t id, bool on) {
  if (RefuseReadOnly(sheet)) return;
  const Sheet* s = S(sheet);
  const model::Table* t = TableById(sheet, id);
  if (!s || !t) return;
  const Rect range = t->range;
  const bool owns = TableOwnsSheetFilter(*s, *t);
  UpdateTable(sheet, id, [&](model::Table& x) { x.filter_button = on; });
  if (on) {
    // the sheet's AutoFilter when it is free (as Excel's first table); otherwise the table's own buttons
    if (!s->autofilter->on) SetAutoFilter(sheet, range);
  } else {
    if (owns) ClearAutoFilter(sheet);
    ClearTableFilter(sheet, id);
  }
}

}  // namespace yc::engine
