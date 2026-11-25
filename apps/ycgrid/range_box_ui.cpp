// SPDX-License-Identifier: Apache-2.0
// range_box_ui.cpp - range boxes in dialogs, the way Excel's RefEdit works
//
// A dialog's cell or range box can be typed in, or its button shrinks the dialog away: a small bar with the box stays
// on the screen while the user selects cells on the sheet (the box follows every drag), and Enter, Escape or the
// button brings the dialog back with the address in the box. Works for modal dialogs too: their popups are put aside
// while picking and opened again as they were, so a dialog's own state (its other fields) is untouched.
#include "app_state.h"
#include "core/i18n.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace yc::app {

using ui::S;

namespace {

struct Pick {
  bool active = false;   // the dialog is shrunk to the bar
  bool start = false;    // the button was pressed this frame: put the dialog aside at the end of the frame
  ImGuiID key = 0;
  char* buf = nullptr;   // the box's text (a dialog's own buffer, or a std::string)
  size_t size = 0;
  std::string* str = nullptr;
  int flags = 0;
  std::string label;     // the box's label and its dialog's title, shown on the bar
  std::string base;      // formula boxes: the text before the reference being picked
  std::string tail;      // and the text after it (the caret was inside the text, as Excel inserts there)
  uint32_t start_sheet = 0;
  uint32_t seen_sheet = 0;
  std::vector<Rect> seen;
  ImVector<ImGuiPopupData> popups;  // the dialog (and the dialogs under it) put aside
  bool focus = false;
  // the sheet's selection before picking: put back afterwards, as Excel does (a dialog that reads the selection at OK,
  // Data Validation, applied its rule to the cells picked for its list instead)
  uint32_t saved_sheet = 0, saved_win = 0;
  render::Selection saved_sel;
};

Pick& P() {
  static Pick p;
  return p;
}

std::string Text(const Pick& p) { return p.str ? *p.str : std::string(p.buf ? p.buf : ""); }

void SetText(Pick& p, const std::string& t) {
  if (p.str) *p.str = t;
  else if (p.buf && p.size) std::snprintf(p.buf, p.size, "%s", t.c_str());
}

bool EndsWithOperator(const std::string& t) {
  if (t.empty()) return false;
  const char c = t.back();
  return std::strchr("=+-*/^&(,;:<> ", c) != nullptr;
}

// where each formula box's caret was when it last had the keyboard (the range button takes the keyboard away)
std::map<ImGuiID, int>& Carets() {
  static std::map<ImGuiID, int> m;
  return m;
}
int NoteCaret(ImGuiInputTextCallbackData* d) {
  Carets()[*static_cast<const ImGuiID*>(d->UserData)] = d->CursorPos;
  return 0;
}

bool RefChar(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '$' || ch == ':' || ch == '!' || ch == '_' || ch == '.' || ch == '\''; }

// a formula box: the picked reference goes in at the caret (Excel): after an operator it is inserted there, and a
// reference the caret touches is replaced; *base and *tail get the text before and after it
void SplitAtCaret(const std::string& t, int caret, std::string* base, std::string* tail) {
  const size_t at = static_cast<size_t>(std::clamp(caret, 0, static_cast<int>(t.size())));
  size_t b = at, e = at;
  while (b > 0 && RefChar(t[b - 1])) --b;
  while (e < t.size() && RefChar(t[e])) ++e;
  const bool function = e < t.size() && t[e] == '(';  // SUM( : a function's name, not a reference
  if ((b < at || e > at) && !function && (b == 0 || EndsWithOperator(t.substr(0, b)))) {
    *base = t.substr(0, b);
    *tail = t.substr(e);
  } else if (at == 0 || EndsWithOperator(t.substr(0, at))) {
    *base = t.substr(0, at);
    *tail = t.substr(at);
  } else {
    base->clear();
    tail->clear();
  }
}

// the address of the selection as the box wants it
std::string Address(AppState& s, const Pick& p, const std::vector<Rect>& areas) {
  const model::Sheet* sh = s.snap->Sheet(s.focus_sheet);
  if (!sh) return {};
  const bool sheet = (p.flags & RangeBox_Sheet) || s.focus_sheet != p.start_sheet;
  const std::string sref = sheet ? core::QuoteSheetName(sh->name) + "!" : std::string();
  const bool abs = !(p.flags & RangeBox_Relative);
  std::string out;
  for (const Rect& r : areas) {
    Rect a = r;
    if (p.flags & RangeBox_Cell) a = Rect::Cell(r.r0, r.c0);
    if (!out.empty()) out += ",";
    // whole rows or columns read as Excel writes them ($1:$3, $A:$C)
    const bool rows = a.c0 == 0 && a.c1 >= core::kMaxCols - 1, cols = a.r0 == 0 && a.r1 >= core::kMaxRows - 1;
    if (rows && !cols) {
      out += sref + (abs ? "$" : "") + std::to_string(a.r0 + 1) + ":" + (abs ? "$" : "") + std::to_string(a.r1 + 1);
    } else if (cols && !rows) {
      const std::string c0 = core::CellA1(0, a.c0), c1 = core::CellA1(0, a.c1);
      out += sref + (abs ? "$" : "") + c0.substr(0, c0.size() - 1) + ":" + (abs ? "$" : "") + c1.substr(0, c1.size() - 1);
    } else {
      out += sref + core::CellA1(a.r0, a.c0, abs, abs);
      if (!a.IsCell()) out += ":" + core::CellA1(a.r1, a.c1, abs, abs);
    }
    if (p.flags & RangeBox_Cell) break;
  }
  return out;
}

void Begin(AppState& s, ImGuiID key, char* buf, size_t size, std::string* str, int flags, const char* label) {
  Pick& p = P();
  p = Pick{};
  p.start = true;
  p.key = key;
  p.buf = buf;
  p.size = size;
  p.str = str;
  p.flags = flags;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  std::string title = w && w->RootWindow ? w->RootWindow->Name : "";  // the dialog's title, not a child's "Title/child"
  if (const size_t h = title.find("##"); h != std::string::npos) title.resize(h);
  p.label = title.empty() ? std::string(label ? label : "") : title + (label && *label ? " - " + std::string(label) : std::string());
  const std::string t = Text(p);
  if (flags & RangeBox_Formula) {
    const auto it = Carets().find(key);
    SplitAtCaret(t, it != Carets().end() ? it->second : static_cast<int>(t.size()), &p.base, &p.tail);
  }
  p.start_sheet = s.focus_sheet;
  p.seen = SelAreas(s);
  p.seen_sheet = s.focus_sheet;
  p.saved_sheet = s.focus_sheet;
  p.saved_win = s.focus_win;
  p.saved_sel = s.FocusView().sel;
}

bool Draw(AppState& s, const char* key, char* buf, size_t size, std::string* str, float width, int flags, const char* label) {
  ImGui::PushID(key);
  const ImGuiID id = ImHashStr(key);
  bool changed = false;
  const float bw = ImGui::GetFrameHeight() * 1.3f, sp = S(4);
  ImGui::SetNextItemWidth(std::max(S(60), (width > 0 ? width : ImGui::GetContentRegionAvail().x) - bw - sp));
  // a formula box remembers its caret: a picked reference goes in there (Excel)
  const ImGuiInputTextFlags tf = (flags & RangeBox_Formula) ? ImGuiInputTextFlags_CallbackAlways : 0;
  ImGuiID key_id = id;
  if (str) {
    char tmp[2048];
    std::snprintf(tmp, sizeof tmp, "%s", str->c_str());
    if (ImGui::InputText("##rb", tmp, sizeof tmp, tf, tf ? NoteCaret : nullptr, &key_id)) {
      *str = tmp;
      changed = true;
    }
  } else if (ImGui::InputText("##rb", buf, size, tf, tf ? NoteCaret : nullptr, &key_id)) {
    changed = true;
  }
  ImGui::SameLine(0, sp);
  const float fh = ImGui::GetFrameHeight();
  ImGui::PushFont(ui::F().icons, ImGui::GetFontSize() * 0.95f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
  ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);  // Tab goes from box to box, as in Excel
  const bool press = ImGui::Button(ui::icon::RangePick, ImVec2(bw, fh));
  ImGui::PopItemFlag();
  ImGui::PopStyleVar();
  ImGui::PopFont();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Select the cells on the sheet"));
  if (press) Begin(s, id, buf, size, str, flags, label);
  ImGui::PopID();
  return changed;
}

}  // namespace

bool RangeBox(AppState& s, const char* key, char* buf, size_t size, float width, int flags, const char* label) {
  return Draw(s, key, buf, size, nullptr, width, flags, label);
}

bool RangeBox(AppState& s, const char* key, std::string* text, float width, int flags, const char* label) {
  return Draw(s, key, nullptr, 0, text, width, flags, label);
}

bool RangePicking() { return P().active || P().start; }

void DrawRangePick(AppState& s) {
  Pick& p = P();
  ImGuiContext& g = *GImGui;
  if (p.start) {
    // the end of the frame, outside every popup: put the dialogs aside
    p.start = false;
    p.active = true;
    p.focus = true;
    p.popups = g.OpenPopupStack;
    if (g.OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
  }
  if (!p.active) return;
  // Enter or Escape anywhere (the sheet has the keyboard while the user selects) brings the dialog back; checked
  // before the selection is read, so a key that also reached the sheet can't change the box
  bool done = !ImGui::GetIO().WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                                                 ImGui::IsKeyPressed(ImGuiKey_Escape));
  // a new selection on the sheet goes into the box
  if (s.snap && !done) {
    const std::vector<Rect> cur = SelAreas(s);
    if (s.focus_sheet != p.seen_sheet || cur != p.seen) {
      p.seen_sheet = s.focus_sheet;
      p.seen = cur;
      const model::Sheet* sh = s.snap->Sheet(s.focus_sheet);
      if (sh && !sh->chart_sheet) {
        const std::string a = Address(s, p, cur);
        SetText(p, ((p.flags & RangeBox_Equals) && p.base.empty() ? "=" : "") + p.base + a + p.tail);
      }
    }
  }
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 1.0f);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + S(150)), ImGuiCond_Appearing, ImVec2(0.5f, 0));
  ImGui::SetNextWindowSize(ImVec2(S(460), 0), ImGuiCond_Always);
  const std::string title = (p.label.empty() ? std::string(TR("Select the cells")) : p.label) + "###yc_range_pick";
  bool open = true;
  if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
    char tmp[2048];
    std::snprintf(tmp, sizeof tmp, "%s", Text(p).c_str());
    const float bw = ImGui::GetFrameHeight() * 1.3f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - S(4));
    if (p.focus) {
      ImGui::SetWindowFocus();
      p.focus = false;
    }
    if (ImGui::InputText("##pick", tmp, sizeof tmp, ImGuiInputTextFlags_EnterReturnsTrue)) {
      SetText(p, tmp);
      done = true;
    } else if (ImGui::IsItemEdited()) {
      SetText(p, tmp);
    }
    ImGui::SameLine(0, S(4));
    const float fh = ImGui::GetFrameHeight();
    ImGui::PushFont(ui::F().icons, ImGui::GetFontSize() * 0.95f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    if (ImGui::Button(ui::icon::RangePick, ImVec2(bw, fh))) done = true;
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Back to the dialog"));
    ImGui::TextDisabled("%s", TR("Select the cells on the sheet, then press Enter."));
  }
  ImGui::End();
  ImGui::PopFont();
  if (!open) done = true;
  if (done) {
    p.active = false;
    // the sheet and its selection as they were before picking (Excel)
    if (s.snap && s.snap->Sheet(p.saved_sheet)) {
      if (s.focus_sheet != p.saved_sheet) s.active_sheet = s.focus_sheet = p.saved_sheet;
      s.focus_win = p.saved_win;
      s.FocusView().sel = p.saved_sel;
    }
    // the dialogs come back as they were, in their order
    if (g.OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
    for (ImGuiPopupData d : p.popups) {
      d.OpenFrameCount = g.FrameCount;
      g.OpenPopupStack.push_back(d);
    }
    p.popups.clear();
  }
}

}  // namespace yc::app
