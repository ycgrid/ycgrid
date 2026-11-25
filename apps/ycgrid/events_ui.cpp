// SPDX-License-Identifier: Apache-2.0
// events_ui.cpp - Excel's workbook and sheet events for the Python project (sheets/<CodeName>.py and main.py)
//
// What the UI does decides when an event fires, as in Excel:
//   - TrackSheetEvents, at the end of every frame, compares the focused sheet and its selection with the last frame's:
//     a new sheet in front fires Worksheet_Deactivate / Worksheet_Activate, a new selection Worksheet_SelectionChange
//     (once per change, whatever made it: a click, keys, the Name Box, Go To, Python's Select; a drag reports when the
//     button is released)
//   - Before* events ask the host and act on its answer a few frames later; the window never waits for Python, and a
//     handler that doesn't answer in time counts as not cancelled (PythonBridge::AskEvent)
//   - the engine reports calculated and added sheets with every snapshot (Worksheet_Calculate, Workbook_NewSheet:
//     PythonBridge::OnSheetEvents)
// The host runs the sheet module's handler, then main.py's Workbook_Sheet* one (host.py _on_event).
#include "app_state.h"
#include "python_bridge.h"

#include "imgui.h"

namespace yc::app {

using io::MV;

namespace {

struct Tracker {
  bool init = false;
  uint32_t sheet = 0, win = 0;
  std::vector<Rect> areas;  // the selection last reported (or seen when nobody listened)
  bool close_answered = false;  // Workbook_BeforeClose said go on: the next WM_CLOSE closes
};

Tracker& T() {
  static Tracker t;
  return t;
}

bool Wants(AppState& s, const char* name, uint32_t sheet = 0) { return s.python && s.python->WantsEvent(name, sheet); }

}  // namespace

void TrackSheetEvents(AppState& s) {
  if (!s.python || !s.snap) return;
  s.python->TickAsks();
  Tracker& t = T();
  const uint32_t sheet = s.focus_sheet, win = s.focus_win;
  const std::vector<Rect>& areas = s.FocusView().sel.areas;
  const bool ready = s.python->HostReady();
  if (!t.init || !ready) {  // nobody listens yet: what is on screen now is where the events start from
    t = Tracker{true, sheet, win, areas, t.close_answered};
    return;
  }
  if (sheet != t.sheet) {
    // switching sheets: the old one's Deactivate, then the new one's Activate (no SelectionChange, as in Excel)
    if (t.sheet && s.snap->Sheet(t.sheet) && Wants(s, "Worksheet_Deactivate", t.sheet)) s.python->SendEvent("Worksheet_Deactivate", t.sheet);
    if (s.snap->Sheet(sheet) && Wants(s, "Worksheet_Activate", sheet)) s.python->SendEvent("Worksheet_Activate", sheet);
    t.sheet = sheet, t.win = win, t.areas = areas;
    return;
  }
  if (win != t.win) {  // another window on the same sheet keeps its own selection
    t.win = win, t.areas = areas;
    return;
  }
  // a drag is still choosing cells, and a formula being typed points at cells without selecting them
  if (s.edit.active || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
  if (areas == t.areas) return;
  t.areas = areas;
  // (Enter / Tab moving the active cell inside a selection change no area: no event, as in Excel)
  if (Wants(s, "Worksheet_SelectionChange", sheet)) s.python->SendEvent("Worksheet_SelectionChange", sheet, &areas);
}

void AskThenAct(AppState& s, const std::string& event, uint32_t sheet, const std::vector<Rect>* target, MV extra, std::function<void(AppState&)> act) {
  auto done = [act](AppState& st, bool cancel) {
    if (!cancel) act(st);
  };
  if (!s.python || !s.python->AskEvent(event, sheet, target, std::move(extra), done)) act(s);
}

void SaveWithEvents(AppState& s, bool save_as) {
  MV extra = MV::Map();
  extra.Set("save_as_ui", MV::Bool(save_as || s.file_path.empty()));  // Excel's SaveAsUI: the Save As dialog will show
  AskThenAct(s, "Workbook_BeforeSave", 0, nullptr, std::move(extra), [save_as](AppState& st) { SaveWorkbook(st, save_as, /*background=*/true); });
}

void FireAfterSave(AppState& s, bool success) {
  if (!Wants(s, "Workbook_AfterSave")) return;
  MV extra = MV::Map();
  extra.Set("success", MV::Bool(success));
  s.python->SendEvent("Workbook_AfterSave", 0, nullptr, &extra);
}

void FireFollowHyperlink(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  if (!Wants(s, "Worksheet_FollowHyperlink", sheet)) return;
  MV extra = MV::Map();
  extra.Set("row", MV::Int(r));
  extra.Set("col", MV::Int(c));
  s.python->SendEvent("Worksheet_FollowHyperlink", sheet, nullptr, &extra);
}

bool AskBeforeClose(AppState& s) {
  Tracker& t = T();
  if (t.close_answered) {  // the handlers ran and didn't cancel: this WM_CLOSE goes on
    t.close_answered = false;
    return false;
  }
  if (!s.python) return false;
  return s.python->AskEvent("Workbook_BeforeClose", 0, nullptr, MV::Map(), [](AppState& st, bool cancel) {
    if (cancel) return;
    T().close_answered = true;
    if (st.python) st.python->SetBeforeCloseDone(true);
    ::PostMessageW(st.hwnd, WM_CLOSE, 0, 0);
  });
}

void BeforeCloseUndone(AppState& s) {
  if (s.python) s.python->SetBeforeCloseDone(false);
}

}  // namespace yc::app
