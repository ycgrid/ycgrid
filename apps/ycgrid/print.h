// SPDX-License-Identifier: Apache-2.0
// print.h - printing, PDF export and Print Preview (print_render.cpp draws the pages with GDI, print_ui.cpp is the UI)
#pragma once

#include "engine/page_layout.h"
#include "engine/snapshot.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace yc::app {

// one sheet of a print job: its page setup (with the print area of this job) and its pages
struct PrintSheetJob {
  uint32_t sheet = 0;
  model::PageSetup ps;
  engine::PrintLayout layout;
  uint32_t gridline_rgb = 0xA6A6A6;  // printed gridlines (a screenshot uses the screen's lighter gray)
  int chart = -1;  // Print Selected Chart: only this chart of the sheet (index in its charts), filling the page
};

// what header / footer codes show
struct PrintInfo {
  std::string file, path, date, time;
  int page_number = 1, page_count = 1;
};

struct PrintRequest {
  std::wstring printer;
  std::wstring output;  // print to this file (Microsoft Print to PDF writes the PDF here)
  int from = 1, to = 0;  // pages of the job (to = 0: up to the last)
  int copies = 1;
  bool collate = true;
  int duplex = 0;  // 0 Print One Sided, 1 Print on Both Sides (flip on long edge), 2 (flip on short edge)
  std::vector<BYTE> devmode;  // Printer Properties: the DEVMODE the driver's dialog returned (empty = the printer's defaults)
};

inline constexpr wchar_t kPdfPrinter[] = L"Microsoft Print to PDF";

// selection: print only that range of each sheet; ignore_area: print the used range even when a print area is set
std::vector<PrintSheetJob> BuildPrintJobs(const engine::Snapshot& snap, const std::vector<uint32_t>& sheets, const core::Rect* selection = nullptr,
                                          bool ignore_area = false);
// File > Print > Print Selected Chart: one page with the chart (an empty list when it isn't there)
std::vector<PrintSheetJob> BuildChartJob(const engine::Snapshot& snap, uint32_t sheet, int chart);
// Page Setup > Sheet > Comments: At end of sheet: the pages listing the notes, after the job's pages
void AddNotesPages(const model::Sheet& sh, PrintSheetJob* job);
PrintInfo MakePrintInfo(const std::wstring& file_path);
// coordinates in twips of the paper; offset = the printer's unprintable margin (device pixels)
void SetPageMapping(HDC dc, double device_px_per_inch_x, double device_px_per_inch_y, int offset_x_px, int offset_y_px);
void PaintPage(HDC dc, double device_px_per_inch, const engine::Snapshot& snap, const PrintSheetJob& job, int page, const PrintInfo& info);
// where the cells of a page are on the paper (points): rows / columns in printed order, print titles included
struct PageCellMap {
  struct Slot {
    uint32_t index;
    double pos, size;
  };
  std::vector<Slot> rows, cols;
  bool CellAt(double x, double y, uint32_t* r, uint32_t* c) const;
  bool CellRect(uint32_t r, uint32_t c, double* x0, double* y0, double* x1, double* y1) const;
};
bool PageCellMapOf(const engine::Snapshot& snap, const PrintSheetJob& job, int page, PageCellMap* out);
bool RenderPagePreview(const engine::Snapshot& snap, const PrintSheetJob& job, int page, const PrintInfo& info, int width_px, std::vector<uint8_t>* rgba, int* w,
                       int* h);
std::vector<std::wstring> PrinterNames(std::wstring* default_printer);

// ---- Screenshot: cells drawn as an image (no paper, no selection), any size ----
struct ImagePart {
  uint32_t sheet = 0;
  core::Rect area{};      // the cells (a chart sheet ignores it)
  std::string caption;   // a title bar above the part ("" = none)
};
struct ImageOptions {
  double px_per_pt = 96.0 / 72.0;  // resolution (screen at 100%: 96 / 72)
  bool headings = false;           // row numbers and column letters
  bool gridlines = true;
  int max_side = 16000;            // pixels; a larger picture is drawn at a lower resolution
  double max_pixels = 60e6;
};
// the parts one under another; rgba is RGBA, top-down. scaled_down: the picture was too large for px_per_pt
bool RenderImage(const engine::Snapshot& snap, const std::vector<ImagePart>& parts, const ImageOptions& o, std::vector<uint8_t>* rgba, int* w, int* h,
                 bool* scaled_down = nullptr);
bool PrintJobs(const engine::Snapshot& snap, const std::vector<PrintSheetJob>& jobs, const PrintRequest& rq, const PrintInfo& info, std::string* err);

}  // namespace yc::app
