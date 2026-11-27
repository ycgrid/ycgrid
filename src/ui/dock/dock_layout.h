// SPDX-License-Identifier: Apache-2.0
// dock_layout.h - saving and restoring the layout (PLAN §10.5)
//
// No imgui.ini: ImGui's ini is an internal format that changes between versions, which breaks backward compatibility (rule 21).
// The layout is saved with our own versioned JSON schema (written into the .ycg LAYOUT chunk in the final version),
// and the docking tree is rebuilt with DockBuilder when loading. DockBuilder is ImGui internal API used only in this file,
// so upgrading ImGui only needs changes here.
#pragma once

#include "imgui.h"
struct ImGuiDockNode;  // imgui_internal.h

#include <windows.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace yc::ui::dock {

inline constexpr int kLayoutSchemaVersion = 1;

// fixed IDs of the main dock space and container dock spaces (stable across runs so they can be saved and restored)
ImGuiID MainDockspaceId();
ImGuiID ContainerDockspaceId(int container_id);

// serialize every root dock node (main dock space, containers, floating dock groups).
// known_windows: names of the windows we manage (with ### IDs), to record the position and size of undocked windows.
nlohmann::json CaptureDockTrees(const std::vector<std::string>& known_windows,
                                const std::vector<int>& container_ids);

// applied at the start of the next frame (after NewFrame, before any Begin).
void QueueRestore(const nlohmann::json& dock_trees);
void ApplyPendingRestore();

// pending window position (undocked floating windows); call before Begin. Returns true when position and size were set;
// the caller must not call SetNextWindowPos / Size in this frame (that would overwrite the restored values).
bool ApplyNextWindowRect(const char* window_name);

// main window position (WINDOWPLACEMENT), with a fallback when the monitor no longer exists.
nlohmann::json CaptureMainWindow(HWND hwnd);
void RestoreMainWindow(HWND hwnd, const nlohmann::json& j);

// take a window out of its dock node / put it into one in the middle of a frame. Use these instead of
// ImGui::DockBuilderDockWindow: ImGui deletes the node a window leaves at once (its sibling merges into the parent),
// but the root node's CentralNode / OnlyNodeWithWindows keep pointing at the deleted node until a later frame, and a
// split in the same frame then reads freed memory (a crash docking a figure that was docked already).
void UndockWindow(const char* window_name);
void DockWindow(const char* window_name, ImGuiID node_id);
// splits `leaf` toward dir (the new part is ratio of it) and returns the new part's node; a leaf too small for both
// parts to hold a window returns itself, so what is docked goes in as a tab. Docked one after another, each split
// took its ratio of the part before until a pane was under a pixel, and dragging a splitter there failed ImGui's
// own check (a debug build stopped: chaos seed 7609)
ImGuiID SplitOrTab(::ImGuiDockNode* leaf, ImGuiDir dir, float ratio);
void RefreshDockRoots();  // recomputes those pointers of every root node

// dock a window to one side of the dock node of anchor_window (a custom panel shown for the first time).
// dir: 0 left, 1 right, 2 top, 3 bottom, 4 same tab group. Returns false when the anchor is not docked yet.
bool DockBesideWindow(const char* window_name, const char* anchor_window, int dir, float ratio);

// default layout: grid on top, chart below.
void BuildDefaultLayout(const char* grid_window, const char* chart_window, ImVec2 size);

}  // namespace yc::ui::dock
