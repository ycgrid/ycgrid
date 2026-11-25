// SPDX-License-Identifier: Apache-2.0
// dock_tabs.cpp - taking a docked window out again must be easy, for every window: each docked window shows its tab
// (title and close button); dragging the tab out, double-clicking it, its right-click menu or the tab bar's menu (the
// arrow) floats it. Windows the workbook locks (Workbook.Protect Windows, Panel / Figure / Container .Locked) stay
// where they are, and say so in the menu.
#include "app_state.h"
#include "core/i18n.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>

namespace yc::app {

namespace {

ImGuiID g_place = 0;       // floated last frame: give it a place over the sheet
int g_place_frames = 0;    // frames left to find it undocked
ImGuiID g_menu_window = 0; // the window whose tab was right-clicked
bool g_menu_open = false;

// (a docked window carries ImGuiWindowFlags_ChildWindow; the windows the workbook locks, and the sheets' own window,
// have NoUndocking on their node through their window class)
bool CanFloat(const ImGuiWindow* w) {
  return w && w->DockNode && !(w->DockNode->MergedFlags & ImGuiDockNodeFlags_NoUndocking);
}

const char* TabTitle(const ImGuiWindow* w) {
  const char* end = ImGui::FindRenderedTextEnd(w->Name);
  static std::string s;
  s.assign(w->Name, static_cast<size_t>(end - w->Name));
  return s.c_str();
}

void FloatMenuItems(ImGuiWindow* w) {
  const bool can = CanFloat(w);
  if (ImGui::MenuItem(TR("Float (undock)"), TR("double-click the tab"), false, can)) FloatDockedWindow(w);
  if (!can && w && w->DockNode) ImGui::TextDisabled("%s", TR("This window is locked by the workbook."));
  if (w && w->DockNode && w->HasCloseButton && ImGui::MenuItem(TR("Close"))) w->DockNode->WantCloseTabId = w->TabId;
}

// the tab bar's menu (the arrow at its left): the windows of the node, then Float / Close for the one shown. Without
// ImGui's "Hide tab bar": a hidden tab bar leaves nothing to take the window out by.
void WindowMenu(ImGuiContext*, ImGuiDockNode* node, ImGuiTabBar* tab_bar) {
  if (tab_bar->Tabs.Size > 1) {
    for (int i = 0; i < tab_bar->Tabs.Size; ++i) {
      ImGuiTabItem* tab = &tab_bar->Tabs[i];
      if (tab->Flags & ImGuiTabItemFlags_Button) continue;
      if (ImGui::Selectable(ImGui::TabBarGetTabName(tab_bar, tab), tab->ID == tab_bar->SelectedTabId)) ImGui::TabBarQueueFocus(tab_bar, tab);
    }
    ImGui::Separator();
  }
  FloatMenuItems(node->VisibleWindow);
}

}  // namespace

void FloatDockedWindow(ImGuiWindow* w) {
  if (!CanFloat(w)) return;
  ImGui::DockContextQueueUndockWindow(GImGui, w);
  g_place = w->ID;
  g_place_frames = 4;
}

bool DockTabMenuOpen() { return g_menu_open; }

void InitDockTabs() { GImGui->DockNodeWindowMenuHandler = &WindowMenu; }

void DrawDockTabs(AppState& s) {
  ImGuiContext& g = *GImGui;
  // a floated window keeps its docked size (a whole strip of the screen): a smaller window over the sheet instead
  if (g_place) {
    ImGuiWindow* w = ImGui::FindWindowByID(g_place);
    if (!w || --g_place_frames <= 0) {
      g_place = 0;
    } else if (!w->DockNode) {
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      const ImVec2 size(std::min(w->Size.x, vp->WorkSize.x * 0.6f), std::min(w->Size.y, vp->WorkSize.y * 0.6f));
      ImGui::SetWindowSize(w, size);
      ImGui::SetWindowPos(w, ImVec2(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f, vp->WorkPos.y + (vp->WorkSize.y - size.y) * 0.4f));
      ImGui::FocusWindow(w);
      g_place = 0;
    }
  }
  // tabs: a right-click opens the menu, a double-click floats the window
  const bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right), dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  for (ImGuiWindow* w : g.Windows) {
    ImGuiDockNode* n = w->DockNode;
    if (!n || !w->DockIsActive || !n->HostWindow || n->IsHiddenTabBar() || n->IsNoTabBar()) continue;
    if (!(right || dbl) || g.HoveredWindow != n->HostWindow || !w->DC.DockTabItemRect.Contains(g.IO.MousePos)) continue;
    if (dbl) {
      FloatDockedWindow(w);
    } else {
      g_menu_window = w->ID;
      ImGui::OpenPopup("##yc_docktab");
    }
    break;
  }
  // a tab bar someone hid (an older layout, ImGui's own menu) comes back: it is how a window is taken out again
  for (ImGuiWindow* w : g.Windows) {
    if (w->DockNode && w->DockNode->IsHiddenTabBar() && !w->DockNode->IsNoTabBar() && (w->DockNode->LocalFlags & ImGuiDockNodeFlags_HiddenTabBar))
      w->DockNode->WantHiddenTabBarToggle = true;
  }
  g_menu_open = ImGui::IsPopupOpen("##yc_docktab");
  if (ImGui::BeginPopup("##yc_docktab")) {
    ImGuiWindow* w = ImGui::FindWindowByID(g_menu_window);
    if (w) {
      ImGui::TextDisabled("%s", TabTitle(w));
      ImGui::Separator();
      FloatMenuItems(w);
    } else {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  (void)s;
}

}  // namespace yc::app
