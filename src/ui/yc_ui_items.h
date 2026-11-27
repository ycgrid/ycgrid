// SPDX-License-Identifier: Apache-2.0
// yc_ui_items.h - [YC] tests: every labelled control drawn in the last frame (buttons, check boxes, radio buttons, text
// boxes, menu items, tree nodes...) with its window and position, through Dear ImGui's test engine hooks
// (IMGUI_ENABLE_TEST_ENGINE in yc_imconfig.h). Off unless a test turns it on (ui.items over YCP), so it costs nothing.
#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <string>
#include <utility>
#include <vector>

namespace yc::ui {

struct UiItem {
  std::string label;   // as written, "##id" parts removed ("Replace All", "OK")
  std::string id;      // the whole label with its "##id" part (a swatch "##c3" has only this)
  std::string window;  // the window it is in (its title, "##id" parts removed)
  ImRect rect;         // window pixels
  bool disabled = false;
  int checked = -1;  // a check box or a checkable menu item: 0 empty, 1 ticked, 2 mixed; -1 not one
};

void RecordUiItems(bool on);
bool RecordingUiItems();
// drawn in the last frame; unlabelled ones too (color swatches, custom drawn buttons) when all is set
std::vector<UiItem> UiItems(bool all = false);
// a control drawn by hand (a status bar chip) that a test can find by its label like the others
void NoteUiItem(const char* label, const ImRect& rect);
// a cell's text drawn by hand, recorded as it is ("C##1" keeps its ## part, which labels lose)
void NoteUiText(const char* text, const ImRect& rect);
// a window's title as drawn last frame: ImGui keeps the name a "###id" window was created with, the title bar shows
// the current one (Format Chart Area, then Format Shape). The name itself when nothing was recorded.
std::string LiveWindowTitle(const char* name);
// a window whose title changes (the Format pane) gives it here every frame: ImGui keeps the first one as its name
void NoteWindowTitle(const std::string& title);
// the label of an item ImGui added without reporting one (a combo box: BeginCombo reports none), by its id
void NameUiItem(ImGuiID id, const char* label);

// ImGui's combo boxes, listed in ui.items by their label ("##type" by its id): ImGui::BeginCombo tells the test hooks
// no label, and the Data Validation dialog's Allow box, like most others, was a nameless control tests couldn't find.
// Every combo box in the app goes through these (check_lessons.py's combo-name)
template <typename... A>
bool Combo(const char* label, A&&... args) {
  const ImGuiID id = ImGui::GetID(label);
  const bool changed = ImGui::Combo(label, std::forward<A>(args)...);  // lesson-ok: combo-name the wrapper itself
  NameUiItem(id, label);
  return changed;
}
inline bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0) {
  const ImGuiID id = ImGui::GetID(label);
  const bool open = ImGui::BeginCombo(label, preview, flags);  // lesson-ok: combo-name the wrapper itself
  NameUiItem(id, label);
  return open;
}

}  // namespace yc::ui
