// SPDX-License-Identifier: Apache-2.0
// screenshot.h - Home > Screenshot (screenshot_ui.cpp): pictures of cells, sheets, the workbook and windows to share
#pragma once

#include "io/msgvalue.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace yc::app {

struct AppState;

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // top-down RGBA
};

bool EncodePng(const Image& img, std::string* bytes);
bool SavePngFile(const std::wstring& path, const Image& img);
bool CopyImageToClipboard(HWND owner, const Image& img);                    // CF_DIB and "PNG"
bool CopyFilesToClipboard(HWND owner, const std::vector<std::wstring>& paths);  // CF_HDROP: paste as files

enum class ShotWhat { Selection, Table, Sheet, Workbook, Visible, Window, FloatingWindow };
// takes the picture (windows: after the next frame), copies it and shows the preview
void TakeScreenshot(AppState& s, ShotWhat what, const std::string& window = {});

void RegisterScreenshotCommands(AppState& s);
void DrawScreenshot(AppState& s);  // every frame: the preview and the window list of the menu
// image.export (Range / Worksheet / Workbook.ExportImage) and image.windows (yc.ui.windows)
io::MV ScreenshotRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err);

}  // namespace yc::app
