// SPDX-License-Identifier: Apache-2.0
// safety_ui.cpp - [YC] safety for live trading (ROADMAP phase 2): operator mode and read-only mode, parameter cells,
// heartbeats of feeds and scripts, stale data, the kill switch and the change log (model/safety.h)
//
// Nothing here may make a stopped feed look alive: when a heartbeat is late, the Python host stops answering or a
// bound table stops updating, the whole window is tinted and a banner names the source until it recovers.
#include "app_state.h"
#include "core/i18n.h"
#include "python_bridge.h"

#include "commands/command_registry.h"
#include "engine/bind.h"
#include "formula/numfmt.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct SafetyUi {
  // windows
  bool show_log = false, show_beats = false, show_param = false;
  // Parameter window (for the active cell)
  uint32_t p_sheet = 0, p_r = 0, p_c = 0;
  char p_name[64] = "", p_choices[256] = "";
  int p_type = 1;
  bool p_has_min = false, p_has_max = false, p_confirm = true;
  double p_min = 0, p_max = 0, p_step = 0;
  // operator PIN
  bool pin_request = false;
  char pin[32] = "";
  std::string pin_error;
  // change log window
  char filter[64] = "";
  // alarms: active ones (logged when they start and when they clear)
  std::set<std::string> active;
  double last_ping = 0;
};

SafetyUi& U() {
  static SafetyUi u;
  return u;
}

std::string WindowsUser() {
  wchar_t b[256];
  DWORD n = 256;
  return ::GetUserNameW(b, &n) ? ToUtf8(b) : std::string("user");
}

void Audit(AppState& s, const std::string& kind, const std::string& target, const std::string& before, const std::string& after,
           const std::string& source = "user") {
  model::AuditEntry e;
  e.kind = kind;
  e.target = target;
  e.before = before;
  e.after = after;
  e.source = source;
  e.user = source == "user" ? WindowsUser() : source;
  s.engine->Post([e = std::move(e)](Document& d) mutable { d.AddAudit(std::move(e)); });
}

std::string Seconds(double x) {
  char b[32];
  std::snprintf(b, sizeof b, x < 10 ? "%.1f s" : "%.0f s", x);
  return b;
}

std::string TimeText(double unix_s) {
  const std::time_t t = static_cast<std::time_t>(unix_s);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[48];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>((unix_s - std::floor(unix_s)) * 1000));
  return b;
}

const model::ParameterDef* ParamAt(const AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  if (!s.snap) return nullptr;
  for (const auto& p : *s.snap->wb.parameters) {
    if (p.sheet == sheet && p.row == r && p.col == c) return &p;
  }
  return nullptr;
}

std::string ParamLabel(const AppState& s, const model::ParameterDef& p) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(p.sheet) : nullptr;
  const std::string addr = (sh ? sh->name + "!" : std::string()) + core::CellA1(p.row, p.col);
  return p.name.empty() ? addr : p.name + " (" + addr + ")";
}

}  // namespace

// ---- alarms ----

std::vector<std::string> SafetyAlarms(AppState& s) {
  std::vector<std::string> out;
  const double now = Now();
  for (const auto& hb : s.heartbeats) {
    const double age = now - hb.last;
    if (age > hb.timeout) out.push_back(hb.name + TR(" stopped: no heartbeat for ") + Seconds(age) + (hb.detail.empty() ? "" : " (" + hb.detail + ")"));
  }
  // the Python host: crashed, or not answering (a macro that never returns, a hung event loop)
  if (s.python && s.python->HostCrashed()) out.push_back(TR("The Python host stopped unexpectedly (Developer > Reload starts it again)"));
  if (s.python && s.python->HostRunning() && s.python->HostReady() && s.snap) {
    const double timeout = std::max(0.5, s.snap->wb.safety->host_timeout_s);
    const double pong = s.python->LastPong();
    if (pong > 0 && now - pong > timeout) out.push_back(TR("The Python host is not responding for ") + Seconds(now - pong));
  }
  // bound tables that stopped updating
  if (s.snap) {
    const int64_t now_ns = engine::PerfCounterNs();
    for (const auto& b : *s.snap->wb.bound_tables) {
      if (b.stale_after <= 0) continue;
      auto it = s.snap->bind_times.find(engine::Lower(b.name));
      if (it == s.snap->bind_times.end() || !it->second) continue;
      int64_t newest = 0;
      for (const auto& t : it->second->t) newest = std::max(newest, t.load(std::memory_order_relaxed));
      if (newest == 0) continue;
      const double age = static_cast<double>(now_ns - newest) / 1e9;
      if (age > b.stale_after) out.push_back(TR("No updates for table ") + b.name + TR(" for ") + Seconds(age));
    }
  }
  for (const auto& [name, text] : s.alarms) out.push_back(name + ": " + text);
  return out;
}

void SafetySetAlarm(AppState& s, const std::string& name, const std::string& text) {
  if (text.empty()) {
    s.alarms.erase(name);
  } else {
    s.alarms[name] = text;
  }
}

void SafetyBeat(AppState& s, const std::string& name, double timeout, const std::string& detail) {
  for (auto& hb : s.heartbeats) {
    if (hb.name == name) {
      hb.last = Now();
      hb.timeout = timeout > 0 ? timeout : hb.timeout;
      hb.detail = detail;
      return;
    }
  }
  s.heartbeats.push_back(AppState::Heartbeat{name, detail, timeout > 0 ? timeout : 2.0, Now()});
}

void SafetyRemoveBeat(AppState& s, const std::string& name) {
  s.heartbeats.erase(std::remove_if(s.heartbeats.begin(), s.heartbeats.end(), [&](const AppState::Heartbeat& h) { return h.name == name; }), s.heartbeats.end());
}

// ---- operator mode, kill switch ----

bool SetOperatorMode(AppState& s, bool on, const std::string& pin, std::string* error) {
  if (on == s.operator_mode) return true;
  if (on && s.snap && s.snap->wb.safety->has_pin && model::PinHash(pin) != s.snap->wb.safety->pin_hash) {
    if (error) *error = TR("The PIN is not correct.");
    return false;
  }
  s.operator_mode = on;
  s.engine->Post([ro = !on](Document& d) { d.ui_read_only = ro; });
  s.last_input = Now();
  Audit(s, "mode", TR("Operator mode"), on ? "off" : "on", on ? "on" : "off");
  s.SetStatus(on ? TR("Operator mode: parameters and guarded buttons can be changed") : TR("Read-only mode: parameters and guarded buttons are locked"));
  return true;
}

bool RunKillSwitch(AppState& s, bool ask, const std::string& source, std::string* error) {
  const std::string macro = s.snap ? s.snap->wb.safety->kill_macro : std::string();
  if (macro.empty()) {
    if (error) *error = TR("No kill switch is set up: Python sets it with yc.safety.on_kill(\"module.function\").");
    return false;
  }
  if (ask) {
    const std::string msg = TR("Run the kill switch now?\n\nIt runs ") + macro + TR(" (cancel orders, flatten positions...).");
    if (AppMessageBox(s, msg, "YCGrid", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1, IDYES) != IDYES) return false;
  }
  if (!s.python || !s.python->HostReady()) {
    Audit(s, "kill", TR("Kill switch"), "", TR("failed: the Python host isn't running"), source);
    if (error) *error = TR("The Python host isn't running: the kill switch couldn't run.");
    return false;
  }
  s.python->RunMacro(macro);
  Audit(s, "kill", TR("Kill switch"), "", macro, source);
  s.SetStatus(TR("Kill switch: ") + macro, 8.0);
  return true;
}

void SafetyOnOpen(AppState& s, bool start_read_only) {
  s.operator_mode = !start_read_only;
  s.engine->Post([start_read_only](Document& d) { d.ui_read_only = start_read_only; });
  s.heartbeats.clear();
  U().active.clear();
  s.engine->Post([user = WindowsUser()](Document& d) { d.audit_user = user; });
}

// ---- editing parameter cells ----

bool SafetyBeforeCommit(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text, bool* param, bool* cancel) {
  *param = false;
  *cancel = false;
  const model::ParameterDef* p = ParamAt(s, sheet, r, c);
  if (!p) return true;
  const std::string label = ParamLabel(s, *p);
  if (!s.operator_mode) {
    s.ShowMessage("YCGrid", TR("Parameters can be changed in operator mode only (Trading > Operator Mode).\n\n") + label);
    *cancel = true;
    return false;
  }
  const formula::InputParse in = formula::ParseInput(text);
  std::string why;
  if (!Document::CheckParameterValue(*p, in.value, &why)) {
    // like Excel's data validation: Retry goes back to the entry, Cancel drops it (also the automatic answer)
    if (AppMessageBox(s, label + "\n\n" + why, "YCGrid", MB_RETRYCANCEL | MB_ICONERROR, IDCANCEL) != IDRETRY) *cancel = true;
    return false;
  }
  const model::Sheet* sh = s.snap->Sheet(sheet);
  const core::Value& old = sh ? sh->GetValue(r, c) : core::Value();
  const std::string before = old.IsEmpty() ? std::string(TR("(empty)")) : engine::DisplayTextOf(old, "General");
  const std::string after = in.value.IsEmpty() ? std::string(TR("(empty)")) : engine::DisplayTextOf(in.value, "General");
  if (before == after) {
    *param = true;
    return true;
  }
  bool big = false;
  std::string step_note;
  if (p->max_step > 0 && old.IsNumeric() && in.value.IsNumeric()) {
    const double d = std::fabs(in.value.AsDouble() - old.AsDouble());
    if (d > p->max_step) {
      big = true;
      char b[160];
      std::snprintf(b, sizeof b, TR("\n\nWarning: the change (%g) is larger than the allowed step (%g)."), d, p->max_step);
      step_note = b;
    }
  }
  if (p->confirm || big) {
    const std::string msg = TR("Change the parameter ") + label + "\n\n" + TR("from: ") + before + "\n" + TR("to: ") + after + "?" + step_note;
    const UINT flags = MB_YESNO | (big ? MB_ICONWARNING | MB_DEFBUTTON2 : MB_ICONQUESTION);
    if (AppMessageBox(s, msg, "YCGrid", flags, big ? IDNO : IDYES) != IDYES) {
      *cancel = true;
      return false;
    }
  }
  *param = true;
  return true;
}

// ---- drawing ----

void DrawSafetyOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (!s.snap) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  // parameter cells: an orange corner (a lock when read-only)
  for (const auto& p : *s.snap->wb.parameters) {
    if (p.sheet != sheet_id) continue;
    const ImVec2 a = g.to_screen(static_cast<double>(sh->cols->Offset(p.col)), static_cast<double>(sh->rows->Offset(p.row)));
    const float w = static_cast<float>(sh->cols->Size(p.col) * g.sc), h = static_cast<float>(sh->rows->Size(p.row) * g.sc);
    if (w <= 0 || h <= 0 || a.x > g.cells.Max.x || a.y > g.cells.Max.y || a.x + w < g.cells.Min.x || a.y + h < g.cells.Min.y) continue;
    const ImU32 col = s.operator_mode ? IM_COL32(0xF0, 0x8C, 0x00, 255) : IM_COL32(0x80, 0x80, 0x80, 255);
    const float k = std::min(S(7), h * 0.45f);
    dl->AddTriangleFilled(a, ImVec2(a.x + k, a.y), ImVec2(a.x, a.y + k), col);
    dl->AddRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(a.x + w - 0.5f, a.y + h - 0.5f), (col & 0x00FFFFFF) | 0x90000000);
  }
  // bound tables: stale rows grayed, row ages at the right
  if (!s.snap->wb.bound_tables->empty()) {
    const int64_t now_ns = engine::PerfCounterNs();
    const ImVec2 top = g.to_sheet(g.cells.Min), bottom = g.to_sheet(g.cells.Max);
    const int64_t first_row = std::max<int64_t>(0, sh->rows->IndexAt(static_cast<int64_t>(top.y)));
    const int64_t last_row = sh->rows->IndexAt(static_cast<int64_t>(bottom.y));
    for (const auto& b : *s.snap->wb.bound_tables) {
      if (b.sheet != sheet_id || (b.stale_after <= 0 && !b.show_age)) continue;
      const model::Table* t = model::FindTable(s.snap->wb, b.name);
      if (!t || !t->HasData()) continue;
      auto it = s.snap->bind_times.find(engine::Lower(b.name));
      if (it == s.snap->bind_times.end() || !it->second) continue;
      const auto& times = it->second->t;
      const core::Rect d = t->Data();
      const int64_t r0 = std::max<int64_t>(d.r0, first_row), r1 = std::min<int64_t>(d.r1, last_row);
      const float x0 = g.to_screen(static_cast<double>(sh->cols->Offset(d.c0)), 0).x;
      const float x1 = g.to_screen(static_cast<double>(sh->cols->Offset(d.c1) + sh->cols->Size(d.c1)), 0).x;
      for (int64_t r = r0; r <= r1; ++r) {
        const size_t i = static_cast<size_t>(r - d.r0);
        const int64_t tt = i < times.size() ? times[i].load(std::memory_order_relaxed) : 0;
        const double age = tt ? static_cast<double>(now_ns - tt) / 1e9 : -1;
        const float y0 = g.to_screen(0, static_cast<double>(sh->rows->Offset(r))).y;
        const float h = static_cast<float>(sh->rows->Size(r) * g.sc);
        if (h <= 0) continue;
        if (b.stale_after > 0 && (age < 0 || age > b.stale_after)) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + h), IM_COL32(0x70, 0x70, 0x70, 0x60));
        if (b.show_age && age >= 0) {
          const std::string txt = Seconds(age);
          const ImU32 tc = b.stale_after > 0 && age > b.stale_after ? IM_COL32(0xC6, 0x28, 0x28, 255) : IM_COL32(0x60, 0x60, 0x60, 255);
          dl->AddText(ImVec2(x1 + S(4), y0 + (h - ImGui::GetFontSize()) * 0.5f), tc, txt.c_str());
        }
      }
    }
  }
  dl->PopClipRect();
}

float DrawSafetyStatus(AppState& s, ImDrawList* dl, const ImRect& bar, float left, float ty) {
  if (!s.snap) return left;
  const bool has_safety = !s.snap->wb.parameters->empty() || !s.snap->wb.safety->kill_macro.empty() || s.snap->wb.safety->start_read_only ||
                          !s.heartbeats.empty() || !s.operator_mode;
  if (!has_safety) return left;
  auto chip = [&](const char* text, ImU32 bg, ImU32 fg, const char* tip) -> bool {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImRect r(ImVec2(left, bar.Min.y + S(3)), ImVec2(left + ts.x + S(14), bar.Max.y - S(3)));
    dl->AddRectFilled(r.Min, r.Max, bg, S(3));
    dl->AddText(ImVec2(r.Min.x + S(7), ty), fg, text);
    left = r.Max.x + S(8);
    const bool hov = ImGui::IsMouseHoveringRect(r.Min, r.Max);
    if (hov) ImGui::SetTooltip("%s", tip);
    return hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  };
  if (chip(s.operator_mode ? TR("OPERATOR") : TR("READ-ONLY"), s.operator_mode ? IM_COL32(0x2E, 0x7D, 0x32, 255) : IM_COL32(0x60, 0x60, 0x60, 255),
           IM_COL32_WHITE, TR("Operator mode: click to switch (Trading > Operator Mode)"))) {
    ui::Commands().Execute("OperatorMode");
  }
  if (!s.snap->wb.safety->kill_macro.empty() &&
      chip(TR("KILL SWITCH"), IM_COL32(0xC6, 0x28, 0x28, 255), IM_COL32_WHITE, TR("Run the kill switch (Ctrl+Alt+K)"))) {
    ui::Commands().Execute("KillSwitch");
  }
  return left;
}

void DrawSafetyFrame(AppState& s) {
  SafetyUi& u = U();
  const double now = Now();
  ImGuiIO& io = ImGui::GetIO();
  // input activity: the idle timeout of operator mode
  bool input = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.InputQueueCharacters.Size > 0 || ImGui::IsAnyMouseDown();
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !input; ++k) input = ImGui::IsKeyDown(static_cast<ImGuiKey>(k));
  if (input || s.last_input == 0) s.last_input = now;
  if (s.snap && s.operator_mode && s.snap->wb.safety->operator_idle_s > 0 && now - s.last_input > s.snap->wb.safety->operator_idle_s &&
      (s.snap->wb.safety->start_read_only || s.snap->wb.safety->has_pin)) {
    s.operator_mode = false;
    Audit(s, "mode", TR("Operator mode"), "on", TR("off (idle)"));
  }
  if (ui::Command* c = ui::Commands().Find("OperatorMode")) c->pressed = s.operator_mode;
  // the engine refuses UI edits in read-only mode, whatever turned operator mode off (button, idle timeout, Python)
  static int synced = -1;
  if (synced != static_cast<int>(s.operator_mode) && s.engine) {
    synced = static_cast<int>(s.operator_mode);
    s.engine->Post([ro = !s.operator_mode](Document& d) { d.ui_read_only = ro; });
    // read-only mode drops an entry in progress, as Escape does: left open, every later click pointed at cells
    // inside it and every key typed into it, and Enter only answered "There's a problem with this formula"
    if (!s.operator_mode && s.edit.active) CancelEdit(s);
  }
  // host liveness: ping every second while it runs
  if (s.python && s.python->HostReady() && now - u.last_ping > 1.0) {
    u.last_ping = now;
    s.python->PingHost();
  }
  // alarms: log when they start and clear; tint the window and show the banner while any is active
  const std::vector<std::string> alarms = SafetyAlarms(s);
  std::set<std::string> keys;
  for (const auto& a : alarms) {
    const std::string k = a.substr(0, a.find(':') != std::string::npos ? a.find(':') : a.rfind(" for "));
    keys.insert(k);
    if (!u.active.count(k)) Audit(s, "alert", k, "", a, "ycgrid");
  }
  for (const auto& k : u.active) {
    if (!keys.count(k)) Audit(s, "alert", k, TR("alarm"), TR("cleared"), "ycgrid");
  }
  u.active = std::move(keys);
  const bool watch = !s.heartbeats.empty() || (s.python && s.python->HostReady()) ||
                     (s.snap && std::any_of(s.snap->wb.bound_tables->begin(), s.snap->wb.bound_tables->end(),
                                            [](const model::BoundTable& b) { return b.stale_after > 0 || b.show_age; }));
  if (watch) ui::RequestRedrawIn(0.25);
  if (!alarms.empty()) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(vp));
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 6.0f);
    fg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0xD0, 0x00, 0x00, static_cast<int>(18 + 14 * pulse)));
    fg->AddRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0xD0, 0x00, 0x00, 230), 0.0f, S(6));
    // the banner: what stopped, and since when
    std::string text;
    for (const auto& a : alarms) text += (text.empty() ? "" : "\n") + std::string("\xe2\x9a\xa0 ") + a;
    ImGui::PushFont(ui::F().ui_bold, ui::F().ui_size + 2);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImVec2 size(ts.x + S(32), ts.y + S(20));
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, vp->Pos.y + S(64));
    fg->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0xC6, 0x28, 0x28, 245), S(4));
    fg->AddText(ImVec2(pos.x + S(16), pos.y + S(10)), IM_COL32_WHITE, text.c_str());
    ImGui::PopFont();
  }
  // ---- windows ----
  if (u.pin_request) {
    ImGui::OpenPopup(TR("Operator Mode###yc_pin"));
    u.pin_request = false;
  }
  if (BeginModal(TR("Operator Mode###yc_pin"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(TR("Enter the PIN to switch on operator mode:"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##pin", u.pin, sizeof u.pin, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    if (!u.pin_error.empty()) ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", u.pin_error.c_str());
    if (ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || enter) {
      std::string err;
      if (SetOperatorMode(s, true, u.pin, &err)) {
        u.pin[0] = 0;
        u.pin_error.clear();
        ImGui::CloseCurrentPopup();
      } else {
        u.pin_error = err;
        Audit(s, "mode", TR("Operator mode"), "off", TR("wrong PIN"));
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) {
      u.pin[0] = 0;
      u.pin_error.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (u.show_param) {
    ImGui::SetNextWindowSize(ImVec2(S(380), 0), ImGuiCond_Appearing);
    if (ImGui::Begin(TR("Parameter Cell###yc_param"), &u.show_param, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
      const model::Sheet* sh = s.snap ? s.snap->Sheet(u.p_sheet) : nullptr;
      ImGui::Text("%s", ((sh ? sh->name + "!" : std::string()) + core::CellA1(u.p_r, u.p_c)).c_str());
      ImGui::SetNextItemWidth(S(220));
      ImGui::InputText(TR("Name"), u.p_name, sizeof u.p_name);
      const char* types[] = {TR("Any value"), TR("Number"), TR("Whole number"), TR("Text"), TR("TRUE / FALSE"), TR("One of a list")};
      ImGui::SetNextItemWidth(S(220));
      ImGui::Combo(TR("Type"), &u.p_type, types, 6);
      if (u.p_type == 1 || u.p_type == 2) {
        ImGui::Checkbox(TR("Minimum"), &u.p_has_min);
        ImGui::SameLine(S(120));
        ImGui::BeginDisabled(!u.p_has_min);
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputDouble("##min", &u.p_min);
        ImGui::EndDisabled();
        ImGui::Checkbox(TR("Maximum"), &u.p_has_max);
        ImGui::SameLine(S(120));
        ImGui::BeginDisabled(!u.p_has_max);
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputDouble("##max", &u.p_max);
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputDouble(TR("Largest change without a warning (0 = any)"), &u.p_step);
      }
      if (u.p_type == 5) {
        ImGui::SetNextItemWidth(S(260));
        ImGui::InputText(TR("Choices (comma-separated)"), u.p_choices, sizeof u.p_choices);
      }
      ImGui::Checkbox(TR("Ask before every change"), &u.p_confirm);
      ImGui::Separator();
      const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
      if (ok_pressed && RefuseReadOnlyUi(s)) {
        u.show_param = false;  // read-only mode came on meanwhile: the status bar says why (not a blank message box)
      } else if (ok_pressed) {
        model::ParameterDef p;
        p.sheet = u.p_sheet;
        p.row = u.p_r;
        p.col = u.p_c;
        p.name = u.p_name;
        p.type = u.p_type;
        p.has_min = u.p_has_min;
        p.has_max = u.p_has_max;
        p.min = u.p_min;
        p.max = u.p_max;
        p.confirm = u.p_confirm;
        p.max_step = u.p_step;
        std::string list = u.p_choices;
        size_t a = 0;
        while (a <= list.size() && u.p_type == 5) {
          const size_t b = list.find(',', a);
          std::string item = list.substr(a, b == std::string::npos ? std::string::npos : b - a);
          while (!item.empty() && item.front() == ' ') item.erase(0, 1);
          while (!item.empty() && item.back() == ' ') item.pop_back();
          if (!item.empty()) p.choices.push_back(item);
          if (b == std::string::npos) break;
          a = b + 1;
        }
        std::string err;
        const bool ok = s.engine->Call([&](Document& d) { return d.SetParameter(p, &err); });
        if (ok) u.show_param = false;
        else if (!err.empty()) s.ShowError("YCGrid", err);
      }
      ImGui::SameLine();
      if (ParamAt(s, u.p_sheet, u.p_r, u.p_c) && ImGui::Button(TR("Remove"), ImVec2(S(80), 0))) {
        const uint32_t sh_id = u.p_sheet, r = u.p_r, c = u.p_c;
        if (!RefuseReadOnlyUi(s)) s.engine->Post([sh_id, r, c](Document& d) { d.RemoveParameter(sh_id, r, c); });
        u.show_param = false;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) u.show_param = false;
    }
    ImGui::End();
  }
  if (u.show_beats) {
    ImGui::SetNextWindowSize(ImVec2(S(460), S(260)), ImGuiCond_FirstUseEver);
    PlaceNewWindow(WindowPlace::Center);
    if (ImGui::Begin(TR("Heartbeats###yc_beats"), &u.show_beats)) {
      if (ImGui::BeginTable("##beats", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(TR("Source"));
        ImGui::TableSetupColumn(TR("Last"));
        ImGui::TableSetupColumn(TR("Timeout"));
        ImGui::TableSetupColumn(TR("Status"));
        ImGui::TableHeadersRow();
        auto row = [&](const std::string& name, double age, double timeout, const std::string& detail) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(age < 0 ? "-" : (Seconds(age) + TR(" ago")).c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(Seconds(timeout).c_str());
          ImGui::TableNextColumn();
          const bool ok = age >= 0 && age <= timeout;
          ImGui::TextColored(ok ? ImVec4(0.1f, 0.5f, 0.1f, 1) : ImVec4(0.8f, 0.1f, 0.1f, 1), "%s%s", ok ? TR("OK") : TR("STOPPED"),
                             detail.empty() ? "" : ("  " + detail).c_str());
        };
        if (s.python && s.python->HostRunning()) {
          const double pong = s.python->LastPong();
          row(TR("Python host"), pong > 0 ? now - pong : -1, s.snap ? s.snap->wb.safety->host_timeout_s : 3, "");
        }
        for (const auto& hb : s.heartbeats) row(hb.name, now - hb.last, hb.timeout, hb.detail);
        ImGui::EndTable();
      }
    }
    ImGui::End();
  }
  if (u.show_log) {
    ImGui::SetNextWindowSize(ImVec2(S(860), S(420)), ImGuiCond_FirstUseEver);
    PlaceNewWindow(WindowPlace::Center);
    if (ImGui::Begin(TR("Change Log###yc_changelog"), &u.show_log)) {
      ImGui::SetNextItemWidth(S(220));
      ImGui::InputTextWithHint("##filter", TR("Filter"), u.filter, sizeof u.filter);
      ImGui::SameLine();
      if (ImGui::Button(TR("Export CSV"))) {
        const std::filesystem::path dir = s.file_path.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(s.file_path).parent_path();
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char name[64];
        std::snprintf(name, sizeof name, "ChangeLog_%04d%02d%02d_%02d%02d%02d.csv", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        const std::filesystem::path out = dir / name;
        std::ofstream f(out, std::ios::binary);
        f << "\xEF\xBB\xBFtime,user,kind,target,before,after,source\r\n";
        auto q = [](const std::string& v) {
          std::string o = "\"";
          for (char ch : v) o += ch == '"' ? std::string("\"\"") : std::string(1, ch);
          return o + "\"";
        };
        for (const auto& e : *s.snap->wb.audit_log)
          f << q(TimeText(e.time)) << ',' << q(e.user) << ',' << q(e.kind) << ',' << q(e.target) << ',' << q(e.before) << ',' << q(e.after) << ',' << q(e.source) << "\r\n";
        s.SetStatus(TR("Change log exported: ") + ToUtf8(out.wstring()), 8.0);
      }
      ImGui::SameLine();
      ImGui::TextDisabled(TR("%zu entries"), s.snap ? s.snap->wb.audit_log->size() : size_t{0});
      if (s.snap && ImGui::BeginTable("##log", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                                       ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : {TR("Time"), TR("User"), TR("Kind"), TR("Target"), TR("Before"), TR("After"), TR("Source")}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        const auto& log = *s.snap->wb.audit_log;
        const std::string flt = engine::Lower(u.filter);
        std::vector<size_t> rows;
        for (size_t i = log.size(); i-- > 0;) {
          const auto& e = log[i];
          if (!flt.empty() && engine::Lower(e.kind + " " + e.target + " " + e.before + " " + e.after + " " + e.user).find(flt) == std::string::npos) continue;
          rows.push_back(i);
        }
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        while (clip.Step()) {
          for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
            const auto& e = log[rows[static_cast<size_t>(k)]];
            ImGui::TableNextRow();
            const std::string* cells[] = {nullptr, &e.user, &e.kind, &e.target, &e.before, &e.after, &e.source};
            for (const std::string* v : cells) {
              ImGui::TableNextColumn();
              ImGui::TextUnformatted(v ? v->c_str() : TimeText(e.time).c_str());
            }
          }
        }
        ImGui::EndTable();
      }
    }
    ImGui::End();
  }
}

// the commands that change the workbook: in read-only mode they say so on the status bar and open no dialog, as
// Excel greys them on a protected sheet (the engine refuses the change itself too; this keeps the dialogs shut).
// Commands that only show, select, copy, print, save, calculate or run Python stay. A menu's own id (...Menu) only opens
// the menu; its entries are judged one by one.
bool ReadOnlyRefusesCommand(const std::string& id) {
  auto starts = [&](const char* p) { return id.rfind(p, 0) == 0; };
  if (id.size() > 4 && id.compare(id.size() - 4, 4, "Menu") == 0) return false;
  static const char* const kKeep[] = {"ObjectCopy", "ClearValidationCircles", "PivotFieldList", "UngroupSheets", "ChartSelChartArea",
                                      "ChartSelPlotArea", "ChartSelTitle", "ChartSelLegend", "ChartSelCatAxis", "ChartSelValAxis",
                                      "ChartSelSeries", "ChartSelValAxis2", "ChartSelGridlines"};
  for (const char* k : kKeep) {
    if (id == k) return false;
  }
  if (starts("PivotSelect")) return false;
  static const char* const kPrefixes[] = {
      "Border", "Font", "Align", "Angle", "Rotate", "Flip", "Indent", "Merge", "Unmerge", "FormatCells", "FormatAsTable", "FormatComment",
      "FormatShape", "Accounting", "MoreAccounting", "Decimal", "Cf", "CellStyle", "NewCellStyle", "Insert", "Delete", "Clear", "Auto",
      "Fill", "FlashFill", "Sort", "CustomSort", "Paste", "Object", "Shape", "Picture", "Bring", "Send", "ObjAlign", "ObjDistribute",
      "Spark", "QaSpark", "Pivot", "Slicer", "Timeline", "Recommended", "Chart", "Theme", "Margins", "Orient", "Paper", "Scale", "Hf",
      "PageSetup", "Tbl", "TableStyle", "NewTableStyle", "Functions", "UseName", "UseInFormula", "FilterBy", "Group", "Ungroup", "Solver"};
  for (const char* p : kPrefixes) {
    if (starts(p)) return true;
  }
  static const char* const kIds[] = {
      "Undo", "Redo", "Repeat", "Cut", "FormatPainter", "WrapText", "VerticalText", "NumberFormat", "CurrencyFormat", "PercentStyle",
      "CommaStyle", "GeneralFormat", "NumberStyle", "TimeFormat", "DateFormat", "ScientificFormat", "ConditionalFormatting", "NewSheetAfter",
      "RowHeight", "ColumnWidth", "DefaultWidth", "LockCell", "HideRows", "HideColumns", "UnhideRows", "UnhideColumns", "HideSheet",
      "UnhideSheet", "RenameSheet", "TabColor", "CopySheet", "ExpandRows100M", "RemoveHyperlinks", "Replace", "CopyFormulaAbove",
      "CopyValueAbove", "QuickAnalysis", "NewComment", "ShowHideNote", "ShowAllNotes", "ConvertToComments", "ConvertToRange", "RenameTable",
      "ResizeTable", "RefreshAll", "MoveChart", "SetPrintArea", "RemovePageBreak", "ResetPageBreaks", "PrintTitles", "PrintGridlines",
      "PrintHeadings", "AddToPrintArea", "SheetBackground", "DefineName", "NameManager", "CreateNamesFromSelection", "ApplyNames",
      "ToggleFilter", "ReapplyFilter", "AdvancedFilter", "TextToColumns", "RemoveDuplicates", "DataValidation", "Consolidate", "Subtotal",
      "ForecastSheet", "ImportText", "GoalSeek", "ScenarioManager", "DataTableDialog", "OutlineSettings", "ShowDetail", "HideDetail",
      "DataAnalysis", "Spelling", "ProtectSheet", "ProtectWorkbook", "AllowEditRanges", "CustomViews", "ControlProperties", "MarkParameter",
      "PickFromList", "ResetPicture", "ChangePicture", "CropPicture"};
  for (const char* k : kIds) {
    if (id == k) return true;
  }
  return false;
}

void RegisterSafetyCommands(AppState& s) {
  auto& R = ui::Commands();
  // read-only mode: a command that would change the workbook is refused before it runs (no dialog opens)
  R.guard = [&s](const ui::Command& c) { return s.operator_mode || !ReadOnlyRefusesCommand(c.id) || !RefuseReadOnlyUi(s); };
  auto make = [](const char* id, const char* label, const char* icon, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  ui::Command om = make("OperatorMode", TR("Operator\nMode"), ui::icon::ProtectSheet,
                        TR("Switch between operator mode (parameters and guarded buttons can be changed) and read-only mode."));
  om.toggle = true;
  R.Add(om).execute = [&s](ui::Command&) {
    if (s.operator_mode) {
      SetOperatorMode(s, false, "", nullptr);
    } else if (s.snap && s.snap->wb.safety->has_pin) {
      U().pin_request = true;
    } else {
      SetOperatorMode(s, true, "", nullptr);
    }
  };
  ui::Command ks = make("KillSwitch", TR("Kill\nSwitch"), ui::icon::Dismiss, TR("Run the emergency handler (cancel orders, flatten positions) set up by Python."));
  ks.shortcut = "Ctrl+Alt+K";
  R.Add(ks).execute = [&s](ui::Command&) {
    std::string err;
    if (!RunKillSwitch(s, true, "user", &err) && !err.empty()) s.ShowError("YCGrid", err);
  };
  R.Add(make("MarkParameter", TR("Parameter\nCell"), ui::icon::Validation,
             TR("Make the active cell a parameter: typed and range-checked, changed only in operator mode, every change logged.")))
      .execute = [&s](ui::Command&) {
    SafetyUi& u = U();
    const auto& sel = s.FocusView().sel;
    u.p_sheet = s.focus_sheet;
    u.p_r = sel.act_r;
    u.p_c = sel.act_c;
    if (const model::ParameterDef* p = ParamAt(s, u.p_sheet, u.p_r, u.p_c)) {
      std::snprintf(u.p_name, sizeof u.p_name, "%s", p->name.c_str());
      u.p_type = p->type;
      u.p_has_min = p->has_min;
      u.p_has_max = p->has_max;
      u.p_min = p->min;
      u.p_max = p->max;
      u.p_confirm = p->confirm;
      u.p_step = p->max_step;
      std::string list;
      for (const auto& c : p->choices) list += (list.empty() ? "" : ", ") + c;
      std::snprintf(u.p_choices, sizeof u.p_choices, "%s", list.c_str());
    } else {
      u.p_name[0] = 0;
      u.p_choices[0] = 0;
      u.p_type = 1;
      u.p_has_min = u.p_has_max = false;
      u.p_confirm = true;
      u.p_step = 0;
    }
    u.show_param = true;
  };
  R.Add(make("ChangeLog", TR("Change\nLog"), ui::icon::CommentsPane, TR("Every parameter change, mode change, kill switch and logged action, with time and user.")))
      .execute = [](ui::Command&) { U().show_log = !U().show_log; };
  R.Add(make("Heartbeats", TR("Heartbeats"), ui::icon::Eye, TR("The data feeds, scripts and the Python host that report they are alive, and when they last did.")))
      .execute = [](ui::Command&) { U().show_beats = !U().show_beats; };
  RegisterPane("change_log", "ChangeLog", []() -> bool& { return U().show_log; });
  RegisterPane("heartbeats", "Heartbeats", []() -> bool& { return U().show_beats; });
}

}  // namespace yc::app
