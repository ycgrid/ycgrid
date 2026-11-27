// SPDX-License-Identifier: Apache-2.0
// yc_ui_items.cpp - Dear ImGui's test engine hooks, recording the controls drawn (yc_ui_items.h)
#include "yc_ui_items.h"

#include <cstdarg>
#include <cstring>
#include <unordered_map>

namespace yc::ui {
namespace {

struct Entry {
  ImRect rect;
  std::string window, label, raw;
  int frame = 0;
  bool disabled = false;
  bool clipped = false;  // out of its window's view this frame: not listed
  bool fallback = false;  // in ImGui's implicit Debug window, shown only when something is drawn into it
  bool mixed = false;     // drawn with ImGuiItemFlags_MixedValue ((Select All) over some ticked items)
  int checked = -1;       // UiItem::checked
};
std::unordered_map<ImGuiID, Entry> g_items;
bool g_on = false;

std::string Visible(const char* s) {
  if (!s) return {};
  std::string v(s);
  const size_t hash = v.find("##");
  if (hash != std::string::npos) v.resize(hash);
  return v;
}

std::string WindowName(const ImGuiWindow* w);

}  // namespace

void RecordUiItems(bool on) {
  g_on = on;
  if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = on;
  if (!on) g_items.clear();
}

bool RecordingUiItems() { return g_on; }

void NoteUiItem(const char* label, const ImRect& rect) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g_on || !g || !label) return;
  if (g->CurrentWindow && !rect.Overlaps(g->CurrentWindow->ClipRect)) {  // scrolled out of view: not on the screen
    g_items.erase(ImHashStr(label));
    return;
  }
  auto& e = g_items[ImHashStr(label)];
  e.rect = rect;
  e.window = WindowName(g->CurrentWindow);
  e.label = Visible(label);
  e.raw = label;
  e.frame = g->FrameCount;
  e.disabled = false;
  e.clipped = false;
  e.checked = -1;
}

void NoteUiText(const char* text, const ImRect& rect) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g_on || !g || !text) return;
  if (g->CurrentWindow && !rect.Overlaps(g->CurrentWindow->ClipRect)) {
    g_items.erase(ImHashStr(text) ^ 0x5A17u);
    return;
  }
  auto& e = g_items[ImHashStr(text) ^ 0x5A17u];
  e.rect = rect;
  e.window = WindowName(g->CurrentWindow);
  e.label = text;
  e.raw = text;
  e.frame = g->FrameCount;
  e.disabled = false;
  e.clipped = false;
  e.checked = -1;
}

namespace {
std::unordered_map<std::string, std::pair<std::string, int>> g_titles;  // "###id" -> the title given, the frame

// the window an item is in, by the title it shows now: ImGui keeps the first title of a "Title###id" window as its
// name, so a pane whose title changes (Format Data Series, then Format Axis) would report the old one
std::string WindowName(const ImGuiWindow* w) {
  if (!w) return {};
  if (const char* id = std::strstr(w->Name, "###")) {
    if (auto it = g_titles.find(id); it != g_titles.end()) return Visible(it->second.first.c_str());
  }
  return Visible(w->Name);
}
}  // namespace

void NoteWindowTitle(const std::string& title) {
  const size_t k = title.find("###");
  if (k == std::string::npos) return;
  ImGuiContext* g = ImGui::GetCurrentContext();
  g_titles[title.substr(k)] = {title, g ? g->FrameCount : 0};
}

std::string LiveWindowTitle(const char* name) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  const char* id = name ? std::strstr(name, "###") : nullptr;
  if (!g || !id) return name ? name : "";
  if (auto it = g_titles.find(id); it != g_titles.end() && g->FrameCount - it->second.second <= 2) return it->second.first;
  // the title bar's item reports the title given to Begin this frame
  for (const auto& [k, e] : g_items) {
    if (g->FrameCount - e.frame > 2 || e.raw.size() < std::strlen(id)) continue;
    if (e.raw.compare(e.raw.size() - std::strlen(id), std::string::npos, id) == 0 && e.raw.find("###") == e.raw.size() - std::strlen(id)) return e.raw;
  }
  return name;
}

void NameUiItem(ImGuiID id, const char* label) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g_on || !g || !label) return;
  auto it = g_items.find(id);
  if (it == g_items.end() || it->second.frame != g->FrameCount) return;  // not added this frame (clipped by the window)
  it->second.label = Visible(label);
  it->second.raw = label;
}

namespace {
bool FallbackShown() {
  for (const ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
    if (w->IsFallbackWindow) return w->WriteAccessed;  // something drew into it this frame (Begin clears it)
  }
  return false;
}
}  // namespace

std::vector<UiItem> UiItems(bool all) {
  std::vector<UiItem> out;
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (!g) return out;
  const int now = g->FrameCount;
  for (auto it = g_items.begin(); it != g_items.end();) {
    if (now - it->second.frame > 2) {
      it = g_items.erase(it);
      continue;
    }
    // the implicit Debug window's title bar is added at every frame and hidden at its end when nothing was drawn
    // into it: listed, tests saw a Debug window nobody could see
    if (it->second.clipped || (it->second.fallback && !FallbackShown())) {
      ++it;
      continue;
    }
    if (all || !it->second.raw.empty()) out.push_back(UiItem{it->second.label, it->second.raw, it->second.window, it->second.rect, it->second.disabled, it->second.checked});
    ++it;
  }
  return out;
}

}  // namespace yc::ui

// ---- the hooks Dear ImGui calls while g.TestEngineHookItems is set ----
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item_data) {
  if (!yc::ui::g_on || id == 0) return;
  auto& e = yc::ui::g_items[id];
  // a label given this frame stays: tab items report their label before they are added
  if (e.frame != ctx->FrameCount) {
    e.label.clear();
    e.raw.clear();
    e.checked = -1;
  }
  e.rect = bb;
  e.window = yc::ui::WindowName(ctx->CurrentWindow);
  e.frame = ctx->FrameCount;
  e.disabled = item_data && (item_data->ItemFlags & ImGuiItemFlags_Disabled);
  e.mixed = item_data && (item_data->ItemFlags & ImGuiItemFlags_MixedValue);
  e.fallback = ctx->CurrentWindow && ctx->CurrentWindow->IsFallbackWindow;
  // ImGui calls this before its own clipping test, and a clipped check box still reports its label (ItemInfo): a
  // control scrolled or clipped out of its window's view is not one a person can click, so it isn't listed
  e.clipped = ctx->CurrentWindow && !bb.Overlaps(ctx->CurrentWindow->ClipRect);
  // one cut by its window's edge is reported by its visible part: a click at its middle lands on it, as a person's does
  if (ctx->CurrentWindow && !e.clipped) e.rect.ClipWithFull(ctx->CurrentWindow->ClipRect);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
  if (!yc::ui::g_on) return;
  // usually after ItemAdd; tab items report before it (ItemAdd keeps a label given in the same frame)
  auto& e = yc::ui::g_items[id];
  if (e.frame != ctx->FrameCount) {
    e.frame = ctx->FrameCount;
    e.window = yc::ui::WindowName(ctx->CurrentWindow);
    e.clipped = false;  // ItemAdd comes next (tab items)
  }
  e.label = yc::ui::Visible(label);
  e.raw = label ? label : "";
  if (flags & ImGuiItemStatusFlags_Checkable) e.checked = e.mixed ? 2 : (flags & ImGuiItemStatusFlags_Checked) ? 1 : 0;
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
  auto it = yc::ui::g_items.find(id);
  return it == yc::ui::g_items.end() ? nullptr : it->second.label.c_str();
}
