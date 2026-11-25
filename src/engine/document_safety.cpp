// SPDX-License-Identifier: Apache-2.0
// document_safety.cpp - [YC] parameter cells and the change log (model/safety.h)
#include "core/i18n.h"
#include "engine/document.h"
#include "formula/numfmt.h"

#include <chrono>
#include <cmath>

namespace yc::engine {

using model::AuditEntry;
using model::ParameterDef;

namespace {

double NowUnix() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string Text(const Value& v) { return v.IsEmpty() ? std::string() : DisplayTextOf(v, "General"); }

uint64_t CellKey(uint32_t sheet, uint32_t r, uint32_t c) { return (uint64_t{sheet} << 48) | (uint64_t{r} << 16) | c; }

}  // namespace

const ParameterDef* Document::ParameterAt(uint32_t sheet, uint32_t r, uint32_t c) const {
  for (const auto& p : *wb_.parameters) {
    if (p.sheet == sheet && p.row == r && p.col == c) return &p;
  }
  return nullptr;
}

bool Document::SetParameter(ParameterDef def, std::string* error) {
  if (RefuseReadOnly(def.sheet)) return false;  // read-only mode: parameters, alerts and safety settings stay
  if (!S(def.sheet)) {
    if (error) *error = TR("The sheet doesn't exist");
    return false;
  }
  if (def.has_min && def.has_max && def.min > def.max) {
    if (error) *error = TR("The minimum is larger than the maximum");
    return false;
  }
  auto& v = wb_.parameters.mut();
  bool replaced = false;
  for (auto& p : v) {
    if (p.sheet == def.sheet && p.row == def.row && p.col == def.col) {
      p = def;
      replaced = true;
    }
  }
  if (!replaced) v.push_back(def);
  param_seen_[CellKey(def.sheet, def.row, def.col)] = Text(GetValue(def.sheet, def.row, def.col));
  wb_.dirty = true;
  return true;
}

bool Document::RemoveParameter(uint32_t sheet, uint32_t r, uint32_t c) {
  if (RefuseReadOnly(sheet)) return false;
  auto& v = wb_.parameters.mut();
  const size_t n = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const ParameterDef& p) { return p.sheet == sheet && p.row == r && p.col == c; }), v.end());
  param_seen_.erase(CellKey(sheet, r, c));
  if (v.size() == n) return false;
  wb_.dirty = true;
  return true;
}

bool Document::CheckParameterValue(const ParameterDef& def, const Value& v, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  const std::string label = def.name.empty() ? std::string(TR("This parameter")) : def.name;
  switch (def.type) {
    case 1:
    case 2: {
      if (!v.IsNumeric()) return fail(label + TR(" needs a number."));
      const double x = v.AsDouble();
      if (def.type == 2 && x != std::floor(x)) return fail(label + TR(" needs a whole number."));
      char b[64];
      if (def.has_min && x < def.min) {
        std::snprintf(b, sizeof b, "%g", def.min);
        return fail(label + TR(" can't be less than ") + b + ".");
      }
      if (def.has_max && x > def.max) {
        std::snprintf(b, sizeof b, "%g", def.max);
        return fail(label + TR(" can't be more than ") + b + ".");
      }
      return true;
    }
    case 3:
      if (v.IsNumeric() || v.IsBool()) return fail(label + TR(" needs text."));
      return true;
    case 4:
      if (!v.IsBool()) return fail(label + TR(" needs TRUE or FALSE."));
      return true;
    case 5: {
      const std::string t = Text(v);
      for (const auto& c : def.choices) {
        if (Lower(c) == Lower(t)) return true;
      }
      std::string list;
      for (const auto& c : def.choices) list += (list.empty() ? "" : ", ") + c;
      return fail(label + TR(" must be one of: ") + list);
    }
    default: return true;
  }
}

void Document::AddAudit(AuditEntry e) {
  if (e.time == 0) e.time = NowUnix();
  auto& log = wb_.audit_log.mut();
  log.push_back(std::move(e));
  if (log.size() > model::kAuditLimit) log.erase(log.begin(), log.begin() + static_cast<std::ptrdiff_t>(log.size() - model::kAuditLimit));
  wb_.dirty = true;
}

void Document::SetSafetySettings(const model::SafetySettings& s) {
  if (RefuseReadOnly()) return;
  wb_.safety.mut() = s;
  wb_.dirty = true;
}

void Document::SetAlert(model::AlertRule rule) {
  if (RefuseReadOnly(rule.sheet)) return;
  auto& v = wb_.alerts.mut();
  for (auto& r : v) {
    if (Lower(r.name) == Lower(rule.name)) {
      r = std::move(rule);
      wb_.dirty = true;
      return;
    }
  }
  v.push_back(std::move(rule));
  wb_.dirty = true;
}

bool Document::RemoveAlert(const std::string& name) {
  if (RefuseReadOnly()) return false;
  auto& v = wb_.alerts.mut();
  const size_t n = v.size();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const model::AlertRule& r) { return Lower(r.name) == Lower(name); }), v.end());
  wb_.dirty = wb_.dirty || v.size() != n;
  return v.size() != n;
}

// parameter cells of a loaded workbook: their current values are the "before" of the first change
void Document::SeedParameterLog() {
  for (const auto& p : *wb_.parameters) param_seen_[CellKey(p.sheet, p.row, p.col)] = Text(GetValue(p.sheet, p.row, p.col));
}

// every change of a parameter cell's value goes to the change log, whoever made it
void Document::AuditParameters(const std::vector<CellChange>& changes) {
  for (const auto& p : *wb_.parameters) {
    const CellChange* hit = nullptr;
    for (const auto& ch : changes) {
      if (ch.sheet == p.sheet && ch.rect.Contains(p.row, p.col)) hit = &ch;
    }
    if (!hit) continue;
    const std::string now = Text(GetValue(p.sheet, p.row, p.col));
    const uint64_t k = CellKey(p.sheet, p.row, p.col);
    auto it = param_seen_.find(k);
    if (it == param_seen_.end()) {
      param_seen_[k] = now;
      continue;
    }
    if (it->second == now) continue;
    AuditEntry e;
    e.kind = "parameter";
    e.target = p.name.empty() ? core::CellA1(p.row, p.col) : p.name;
    if (const model::Sheet* s = S(p.sheet)) e.target += " (" + s->name + "!" + core::CellA1(p.row, p.col) + ")";
    e.before = it->second;
    e.after = now;
    const uint8_t src = hit->source;
    e.source = PolicyFor(src).name;
    e.user = e.source == "user" ? audit_user : e.source;
    it->second = now;
    AddAudit(std::move(e));
  }
}

}  // namespace yc::engine
