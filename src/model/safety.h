// SPDX-License-Identifier: Apache-2.0
// safety.h - [YC] safety for live trading: parameter cells, the change log and the workbook's safety settings
//
// Parameter cells hold the values a strategy or a model reads (spreads, limits, sizes). The user changes them only by
// typing (with the type and range checked, and a confirmation when asked for), and only in operator mode; Python
// changes them freely. Every change of a parameter cell's value, whoever made it, goes to the change log, as do mode
// changes, kill-switch presses and whatever Python logs (orders, actions). All of it is saved with the workbook
// (SAFE chunk).
#pragma once

#include "core/geometry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yc::model {

struct ParameterDef {
  uint32_t sheet = 0;
  uint32_t row = 0, col = 0;
  std::string name;       // shown in confirmations and the log ("" = the address)
  int type = 0;           // 0 any, 1 number, 2 whole number, 3 text, 4 TRUE / FALSE, 5 one of `choices`
  bool has_min = false, has_max = false;
  double min = 0, max = 0;
  std::vector<std::string> choices;
  bool confirm = true;    // ask before a change is applied
  double max_step = 0;    // a change larger than this asks again with a warning (0 = no limit)
  bool operator==(const ParameterDef&) const = default;
};

struct AuditEntry {
  double time = 0;        // Unix seconds (UTC)
  std::string user;       // Windows user name, or the process / feed that sent it
  std::string kind;       // "parameter", "mode", "kill", "order", "action", "alert"...
  std::string target;     // the parameter's name (or address), the order id...
  std::string before, after;
  std::string source;     // "user", "python", "live", "remote"
  bool operator==(const AuditEntry&) const = default;
};

struct SafetySettings {
  bool start_read_only = false;  // the workbook opens in read-only mode (operator mode must be switched on)
  bool has_pin = false;
  uint32_t pin_hash = 0;         // operator mode asks for this PIN (FNV-1a of the PIN)
  int operator_idle_s = 0;       // operator mode turns off after this many seconds without input (0 = never)
  std::string kill_macro;        // what the kill switch runs (a macro of the Python host)
  double host_timeout_s = 3;     // the Python host is "not responding" after this long without answering
  bool operator==(const SafetySettings&) const = default;
};

// an alert rule: when a cell of the range meets the condition, YCGrid highlights it and (once per cooldown) beeps,
// shows a popup, flashes the taskbar button, logs it and tells Python (yc.alerts.on_alert)
struct AlertRule {
  std::string name;
  uint32_t sheet = 0;
  core::Rect range{};
  int op = 1;  // 1 >, 2 <, 3 >=, 4 <=, 5 =, 6 <>, 7 between a and b, 8 outside a..b, 9 the value changes
  double a = 0, b = 0;
  bool sound = true, popup = true, flash_window = false, enabled = true;
  bool notify = false;  // a Windows notification (seen when YCGrid is minimized or behind other windows)
  uint32_t color = 0xFFFFC107;  // highlight of the cells that meet the condition (ARGB; 0 = none)
  double cooldown_s = 5;
  bool operator==(const AlertRule&) const = default;
};

inline uint32_t PinHash(const std::string& pin) {
  uint32_t h = 2166136261u;
  for (unsigned char ch : pin) h = (h ^ ch) * 16777619u;
  return h;
}

inline constexpr size_t kAuditLimit = 100000;  // oldest entries are dropped beyond this

}  // namespace yc::model
