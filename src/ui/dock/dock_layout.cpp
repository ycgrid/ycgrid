// SPDX-License-Identifier: Apache-2.0
#include "dock/dock_layout.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <utility>

namespace yc::ui::dock {
namespace {

using nlohmann::json;

struct PendingRect {
  ImVec2 pos, size;
  bool collapsed = false;
};

json g_pending;  // docking tree waiting to be applied
bool g_has_pending = false;
std::map<std::string, PendingRect> g_rects;
std::vector<std::pair<ImGuiID, std::string>> g_select_fixups;  // leaf node -> tab window to select

bool HasWindows(const json& j) {
  if (j.contains("split")) {
    const json& ch = j["children"];
    return ch.is_array() && ch.size() == 2 && (HasWindows(ch[0]) || HasWindows(ch[1]));
  }
  return j.value("central", false) || (j.contains("windows") && !j["windows"].empty());
}

json NodeToJson(const ImGuiDockNode* n) {
  json j;
  if (n->IsSplitNode()) {
    // one side has no (shown) window: use the other side directly, no empty pane
    json ja = NodeToJson(n->ChildNodes[0]), jb = NodeToJson(n->ChildNodes[1]);
    if (!HasWindows(ja)) return jb;
    if (!HasWindows(jb)) return ja;
    const int axis = n->SplitAxis;
    const float a = n->ChildNodes[0]->SizeRef[axis];
    const float b = n->ChildNodes[1]->SizeRef[axis];
    j["split"] = axis == ImGuiAxis_X ? "x" : "y";
    j["ratio"] = (a + b) > 0.0f ? a / (a + b) : 0.5f;
    j["children"] = json::array({std::move(ja), std::move(jb)});
    return j;
  }
  json windows = json::array();
  std::string selected;
  if (n->TabBar) {
    for (const ImGuiTabItem& tab : n->TabBar->Tabs) {
      if (tab.Window && tab.Window->WasActive) windows.push_back(tab.Window->Name);
    }
  } else {
    for (const ImGuiWindow* w : n->Windows) {
      if (w->WasActive) windows.push_back(w->Name);
    }
  }
  for (const ImGuiWindow* w : n->Windows) {
    if (w->TabId == n->SelectedTabId) selected = w->Name;
  }
  j["windows"] = windows;
  if (!selected.empty()) j["selected"] = selected;
  if (n->IsCentralNode()) j["central"] = true;
  return j;
}

void BuildNode(ImGuiID id, const json& j) {
  if (j.contains("split") && j["children"].is_array() && j["children"].size() == 2) {
    if (!HasWindows(j["children"][0])) return BuildNode(id, j["children"][1]);
    if (!HasWindows(j["children"][1])) return BuildNode(id, j["children"][0]);
  }
  if (j.contains("split")) {
    const bool x = j.value("split", "x") == "x";
    const float ratio = std::clamp(j.value("ratio", 0.5f), 0.05f, 0.95f);
    ImGuiID a = 0, b = 0;
    ImGui::DockBuilderSplitNode(id, x ? ImGuiDir_Left : ImGuiDir_Up, ratio, &a, &b);
    const json& ch = j["children"];
    if (ch.is_array() && ch.size() == 2) {
      BuildNode(a, ch[0]);
      BuildNode(b, ch[1]);
    }
    return;
  }
  if (j.contains("windows")) {
    for (const auto& w : j["windows"]) {
      if (w.is_string()) ImGui::DockBuilderDockWindow(w.get<std::string>().c_str(), id);
    }
  }
  if (j.contains("selected") && j["selected"].is_string()) g_select_fixups.emplace_back(id, j["selected"].get<std::string>());
}

// is the window rectangle inside the work area of any monitor? If not, move it to the primary monitor (e.g. a second monitor was unplugged).
void ValidateRect(ImVec2& pos, ImVec2& size, float saved_dpi) {
  const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
  if (pio.Monitors.Size == 0) return;
  const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
  const ImGuiPlatformMonitor* hit = nullptr;
  for (const ImGuiPlatformMonitor& m : pio.Monitors) {
    if (center.x >= m.WorkPos.x && center.y >= m.WorkPos.y && center.x < m.WorkPos.x + m.WorkSize.x &&
        center.y < m.WorkPos.y + m.WorkSize.y) {
      hit = &m;
      break;
    }
  }
  if (!hit) {
    hit = &pio.Monitors[0];
    pos = ImVec2(hit->WorkPos.x + 40.0f, hit->WorkPos.y + 40.0f);
  }
  if (saved_dpi > 0.0f && hit->DpiScale > 0.0f && std::abs(hit->DpiScale - saved_dpi) > 0.01f) {
    const float k = hit->DpiScale / saved_dpi;
    size = ImVec2(size.x * k, size.y * k);
  }
  size.x = std::min(size.x, hit->WorkSize.x);
  size.y = std::min(size.y, hit->WorkSize.y);
}

}  // namespace

ImGuiID MainDockspaceId() { return ImHashStr("yc_main_dockspace"); }

ImGuiID ContainerDockspaceId(int container_id) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "yc_container_ds_%d", container_id);
  return ImHashStr(buf);
}

json CaptureDockTrees(const std::vector<std::string>& known_windows, const std::vector<int>& container_ids) {
  ImGuiContext& g = *GImGui;
  json roots = json::array();
  for (int i = 0; i < g.DockContext.Nodes.Data.Size; ++i) {
    const ImGuiDockNode* n = static_cast<const ImGuiDockNode*>(g.DockContext.Nodes.Data[i].val_p);
    if (!n || n->ParentNode) continue;
    json r;
    if (n->ID == MainDockspaceId()) {
      r["kind"] = "main";
    } else {
      int container = -1;
      for (int cid : container_ids) {
        if (n->ID == ContainerDockspaceId(cid)) container = cid;
      }
      if (container >= 0) {
        r["kind"] = "container";
        r["container_id"] = container;
      } else if (n->IsFloatingNode() && !n->IsEmpty()) {
        r["kind"] = "floating";
        r["x"] = n->Pos.x;
        r["y"] = n->Pos.y;
      } else {
        continue;
      }
    }
    r["w"] = n->Size.x;
    r["h"] = n->Size.y;
    r["node"] = NodeToJson(n);
    roots.push_back(std::move(r));
  }
  json windows = json::array();
  for (const std::string& name : known_windows) {
    const ImGuiWindow* w = ImGui::FindWindowByName(name.c_str());
    if (!w) continue;
    json jw;
    jw["name"] = name;
    jw["x"] = w->Pos.x;
    jw["y"] = w->Pos.y;
    jw["w"] = w->SizeFull.x;
    jw["h"] = w->SizeFull.y;
    jw["collapsed"] = w->Collapsed;
    jw["docked"] = w->DockNode != nullptr;
    jw["dpi"] = w->Viewport ? w->Viewport->DpiScale : 1.0f;
    windows.push_back(std::move(jw));
  }
  return json{{"roots", roots}, {"windows", windows}};
}

void QueueRestore(const json& dock_trees) {
  g_pending = dock_trees;
  g_has_pending = true;
}

void ApplyPendingRestore() {
  // selected tab: a window only has a TabId after its first Begin, so this is applied once the window exists
  for (auto it = g_select_fixups.begin(); it != g_select_fixups.end();) {
    ImGuiWindow* w = ImGui::FindWindowByName(it->second.c_str());
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(it->first);
    if (w && node && w->DockNode == node && w->TabId != 0) {
      node->SelectedTabId = w->TabId;
      it = g_select_fixups.erase(it);
    } else {
      ++it;
    }
  }
  if (!g_has_pending) return;
  g_has_pending = false;
  g_select_fixups.clear();
  const json& p = g_pending;

  // 1) windows: undock everything first, remember the positions of undocked windows
  if (p.contains("windows")) {
    for (const auto& jw : p["windows"]) {
      const std::string name = jw.value("name", "");
      if (name.empty()) continue;
      UndockWindow(name.c_str());
      if (!jw.value("docked", false)) {
        PendingRect r;
        r.pos = ImVec2(jw.value("x", 100.0f), jw.value("y", 100.0f));
        r.size = ImVec2(jw.value("w", 640.0f), jw.value("h", 420.0f));
        r.collapsed = jw.value("collapsed", false);
        ValidateRect(r.pos, r.size, jw.value("dpi", 0.0f));
        g_rects[name] = r;
      }
    }
  }
  // 2) docking tree
  if (p.contains("roots")) {
    for (const auto& r : p["roots"]) {
      const std::string kind = r.value("kind", "");
      ImVec2 size(r.value("w", 800.0f), r.value("h", 600.0f));
      ImGuiID id = 0;
      if (kind == "main" || kind == "container") {
        id = kind == "main" ? MainDockspaceId() : ContainerDockspaceId(r.value("container_id", 0));
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, size);
      } else if (kind == "floating") {
        ImVec2 pos(r.value("x", 100.0f), r.value("y", 100.0f));
        ValidateRect(pos, size, 0.0f);
        id = ImGui::DockBuilderAddNode(0, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodePos(id, pos);
        ImGui::DockBuilderSetNodeSize(id, size);
      } else {
        continue;
      }
      if (r.contains("node")) BuildNode(id, r["node"]);
      ImGui::DockBuilderFinish(id);
    }
  }
}

bool ApplyNextWindowRect(const char* window_name) {
  auto it = g_rects.find(window_name);
  if (it == g_rects.end()) return false;
  ImGui::SetNextWindowPos(it->second.pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(it->second.size, ImGuiCond_Always);
  ImGui::SetNextWindowCollapsed(it->second.collapsed, ImGuiCond_Always);
  g_rects.erase(it);
  return true;
}

json CaptureMainWindow(HWND hwnd) {
  WINDOWPLACEMENT wp{sizeof(wp)};
  ::GetWindowPlacement(hwnd, &wp);
  const RECT& r = wp.rcNormalPosition;
  return json{{"x", r.left},
              {"y", r.top},
              {"w", r.right - r.left},
              {"h", r.bottom - r.top},
              {"maximized", wp.showCmd == SW_SHOWMAXIMIZED || ::IsZoomed(hwnd)},
              {"dpi", ::GetDpiForWindow(hwnd)}};
}

void RestoreMainWindow(HWND hwnd, const json& j) {
  if (!j.is_object()) return;
  WINDOWPLACEMENT wp{sizeof(wp)};
  ::GetWindowPlacement(hwnd, &wp);
  RECT r{j.value("x", 100), j.value("y", 100), 0, 0};
  r.right = r.left + std::max(400, j.value("w", 1280));
  r.bottom = r.top + std::max(300, j.value("h", 800));
  // the original monitor no longer exists: move to the primary monitor
  if (!::MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
    MONITORINFO mi{sizeof(mi)};
    ::GetMonitorInfoW(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const LONG w = std::min<LONG>(r.right - r.left, mi.rcWork.right - mi.rcWork.left);
    const LONG h = std::min<LONG>(r.bottom - r.top, mi.rcWork.bottom - mi.rcWork.top);
    r = {mi.rcWork.left + 40, mi.rcWork.top + 40, mi.rcWork.left + 40 + w, mi.rcWork.top + 40 + h};
  }
  wp.rcNormalPosition = r;
  wp.showCmd = j.value("maximized", false) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
  ::SetWindowPlacement(hwnd, &wp);
}

namespace {
// what DockNodeUpdateForRootNode (private to imgui.cpp) sets up for a root node
void FindTreeInfo(ImGuiDockNode* node, ImGuiDockNode** central, ImGuiDockNode** first, int* count) {
  node->HasCentralNodeChild = false;
  if (node->Windows.Size > 0) {
    if (!*first) *first = node;
    ++*count;
  }
  if (node->IsCentralNode() && !*central) *central = node;
  for (ImGuiDockNode* c : node->ChildNodes) {
    if (c) FindTreeInfo(c, central, first, count);
  }
}
}  // namespace

void RefreshDockRoots() {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g) return;
  ImGuiStorage& nodes = g->DockContext.Nodes;
  for (int n = 0; n < nodes.Data.Size; n++) {
    ImGuiDockNode* root = static_cast<ImGuiDockNode*>(nodes.Data[n].val_p);
    if (!root || root->ParentNode) continue;
    ImGuiDockNode *central = nullptr, *first = nullptr;
    int count = 0;
    FindTreeInfo(root, &central, &first, &count);
    root->CentralNode = central;
    root->OnlyNodeWithWindows = count == 1 ? first : nullptr;
    root->CountNodeWithWindows = count;
    for (ImGuiDockNode* m = central; m; m = m->ParentNode) m->HasCentralNodeChild = true;
  }
}

void UndockWindow(const char* window_name) { DockWindow(window_name, 0); }

ImGuiID SplitOrTab(ImGuiDockNode* leaf, ImGuiDir dir, float ratio) {
  const float r = std::clamp(ratio, 0.05f, 0.95f);
  const float along = (dir == ImGuiDir_Left || dir == ImGuiDir_Right) ? leaf->Size.x : leaf->Size.y;
  const ImGuiStyle& st = ImGui::GetStyle();
  const float min_part = std::max({st.WindowMinSize.x * 2.0f, st.WindowMinSize.y * 2.0f, ImGui::GetFontSize() * 6.0f});
  if (along > 0.0f && (along * r < min_part || along * (1.0f - r) < min_part)) return leaf->ID;
  ImGuiID rest = 0;
  return ImGui::DockBuilderSplitNode(leaf->ID, dir, r, nullptr, &rest);
}

void DockWindow(const char* window_name, ImGuiID node_id) {
  ImGui::DockBuilderDockWindow(window_name, node_id);
  RefreshDockRoots();
}

bool DockBesideWindow(const char* window_name, const char* anchor_window, int dir, float ratio) {
  // A window docked already is taken out first: its node, left empty, merges into its sibling at once - which can
  // be the anchor's node, freeing it - so the anchor's node is looked up only afterwards.
  if (ImGuiWindow* w = ImGui::FindWindowByName(window_name); w && (w->DockNode || w->DockId)) UndockWindow(window_name);
  ImGuiWindow* anchor = ImGui::FindWindowByName(anchor_window);
  if (!anchor || !anchor->DockNode) return false;
  ImGuiDockNode* leaf = anchor->DockNode;
  ImGuiDockNode* root = leaf;
  while (root->ParentNode) root = root->ParentNode;
  const ImGuiID root_id = root->ID;  // splitting the leaf keeps the root
  ImGuiID target = leaf->ID;
  if (dir >= 0 && dir < 4) {
    static constexpr ImGuiDir kDirs[] = {ImGuiDir_Left, ImGuiDir_Right, ImGuiDir_Up, ImGuiDir_Down};
    target = SplitOrTab(leaf, kDirs[dir], std::min(ratio, 0.9f));
  }
  DockWindow(window_name, target);
  ImGui::DockBuilderFinish(root_id);
  return true;
}

void BuildDefaultLayout(const char* grid_window, const char* chart_window, ImVec2 size) {
  const ImGuiID id = MainDockspaceId();
  ImGui::DockBuilderRemoveNode(id);
  ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(id, size);
  // the grid fills the main dock space; other windows such as charts are docked by the user (or Python) when they open
  ImGui::DockBuilderDockWindow(grid_window, id);
  (void)chart_window;
  ImGui::DockBuilderFinish(id);
}

}  // namespace yc::ui::dock
