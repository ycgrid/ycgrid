// SPDX-License-Identifier: Apache-2.0
// alerts_ui.cpp - [YC] Trading > Alert Rules: the alert rules of the workbook (yc.alerts) in a window: add one for the
// selected cells, change the condition and what happens, turn rules on and off, delete them. The rules are evaluated by
// trading_ui.cpp; they are saved with the workbook (SAFE chunk).
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "engine/engine.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "yc_ui_items.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace yc::app {

using ui::S;

namespace {

struct AlertsUi {
  bool open = false;
  char name[64] = "";
  int op = 0;  // index into kOps
  double a = 0, b = 0;
  bool sound = true, popup = true, flash = false, notify = false;
  float cooldown = 5;
  std::string error;
};

AlertsUi& A() {
  static AlertsUi u;
  return u;
}

// rule op codes 1..9 (model::AlertRule::op)
const char* const kOps[] = {">", "<", ">=", "<=", "=", "<>", "between", "outside", "changed"};

std::string RangeText(const AppState& s, const model::AlertRule& r) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(r.sheet) : nullptr;
  std::string a = core::CellA1(r.range.r0, r.range.c0);
  if (!r.range.IsCell()) a += ":" + core::CellA1(r.range.r1, r.range.c1);
  return (sh ? sh->name + "!" : std::string()) + a;
}

void Put(AppState& s, model::AlertRule r) {
  s.engine->Post([r = std::move(r)](engine::Document& d) mutable { d.SetAlert(std::move(r)); });
}

}  // namespace

void RegisterAlertCommands(AppState& s) {
  ui::Command c;
  c.id = "AlertRules";
  c.label = TR("Alert\nRules");
  c.icon = ui::icon::CondFormat;
  c.toggle = true;
  c.tip_desc = TR("Rules that watch cells (a price above a level, a value that changes...) and highlight, beep, pop up, "
                  "flash the taskbar or show a Windows notification; saved with the workbook.");
  ui::Commands().Add(c).execute = [](ui::Command&) { A().open = !A().open; };
  RegisterPane("alerts", "AlertRules", []() -> bool& { return A().open; });
  (void)s;
}

void DrawAlertRules(AppState& s) {
  AlertsUi& u = A();
  if (ui::Command* c = ui::Commands().Find("AlertRules")) c->pressed = u.open;
  if (!u.open || !s.snap) return;
  ImGui::SetNextWindowSize(ImVec2(S(860), S(420)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Center);
  bool open = u.open;
  if (ImGui::Begin(TR("Alert Rules###yc_alert_rules"), &open)) {
    // ---- a new rule for the selected cells ----
    const auto& view = s.FocusView();
    const core::Rect sel = view.sel.Active();
    model::AlertRule probe;
    probe.sheet = view.sheet;
    probe.range = sel;
    ImGui::TextUnformatted(TR("New rule for the selection:"));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", RangeText(s, probe).c_str());
    ImGui::SetNextItemWidth(S(160));
    ImGui::InputTextWithHint("##name", TR("Name"), u.name, sizeof u.name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(110));
    ui::Combo("##op", &u.op, kOps, IM_ARRAYSIZE(kOps));
    if (u.op != 8) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(110));
      ImGui::InputDouble("##a", &u.a, 0, 0, "%g");
    }
    if (u.op == 6 || u.op == 7) {
      ImGui::SameLine();
      ImGui::TextUnformatted(TR("and"));
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(110));
      ImGui::InputDouble("##b", &u.b, 0, 0, "%g");
    }
    ImGui::Checkbox(TR("Sound"), &u.sound);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Popup"), &u.popup);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Flash taskbar"), &u.flash);
    ImGui::SameLine();
    ImGui::Checkbox(TR("Windows notification"), &u.notify);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(90));
    ImGui::InputFloat(TR("Cooldown (s)"), &u.cooldown, 0, 0, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button(TR("Add Rule"))) {
      std::string name = u.name;
      if (name.empty()) name = std::string(kOps[u.op]) + " " + RangeText(s, probe);
      bool taken = false;
      for (const auto& r : *s.snap->wb.alerts) taken = taken || engine::Lower(r.name) == engine::Lower(name);
      if (taken) {
        u.error = TR("A rule with this name exists: choose another name.");
      } else {
        model::AlertRule r = probe;
        r.name = name;
        r.op = u.op + 1;
        r.a = u.a;
        r.b = u.b;
        r.sound = u.sound;
        r.popup = u.popup;
        r.flash_window = u.flash;
        r.notify = u.notify;
        r.cooldown_s = std::max(0.0f, u.cooldown);
        Put(s, std::move(r));
        u.name[0] = 0;
        u.error.clear();
      }
    }
    if (!u.error.empty()) ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", u.error.c_str());
    ImGui::Separator();
    // ---- the rules ----
    const auto& rules = *s.snap->wb.alerts;
    if (rules.empty()) {
      ImGui::TextDisabled("%s", TR("No alert rules yet. Select cells and add one, or use yc.alerts.add() from Python."));
    } else if (ImGui::BeginTable("##rules", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                                 ImVec2(0, ImGui::GetContentRegionAvail().y))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn(TR("On"), ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn(TR("Name"));
      ImGui::TableSetupColumn(TR("Cells"));
      ImGui::TableSetupColumn(TR("Condition"));
      ImGui::TableSetupColumn(TR("Sound"), ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn(TR("Popup"), ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn(TR("Flash"), ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn(TR("Notify"), ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < rules.size(); ++i) {
        const model::AlertRule& r = rules[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        model::AlertRule edit = r;
        bool changed = false;
        ImGui::TableSetColumnIndex(0);
        changed |= ImGui::Checkbox("##on", &edit.enabled);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(r.name.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(RangeText(s, r).c_str());
        ImGui::TableSetColumnIndex(3);
        char cond[96];
        const char* op = r.op >= 1 && r.op <= 9 ? kOps[r.op - 1] : "?";
        if (r.op == 7 || r.op == 8) std::snprintf(cond, sizeof cond, "%s %g .. %g", op, r.a, r.b);
        else if (r.op == 9) std::snprintf(cond, sizeof cond, "%s", op);
        else std::snprintf(cond, sizeof cond, "%s %g", op, r.a);
        ImGui::TextUnformatted(cond);
        ImGui::TableSetColumnIndex(4);
        changed |= ImGui::Checkbox("##snd", &edit.sound);
        ImGui::TableSetColumnIndex(5);
        changed |= ImGui::Checkbox("##pop", &edit.popup);
        ImGui::TableSetColumnIndex(6);
        changed |= ImGui::Checkbox("##fl", &edit.flash_window);
        ImGui::TableSetColumnIndex(7);
        changed |= ImGui::Checkbox("##nt", &edit.notify);
        ImGui::TableSetColumnIndex(8);
        if (ImGui::SmallButton(TR("Delete"))) {
          const std::string name = r.name;
          s.engine->Post([name](engine::Document& d) { d.RemoveAlert(name); });
        } else if (changed) {
          Put(s, std::move(edit));
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
  u.open = open;
}

}  // namespace yc::app
