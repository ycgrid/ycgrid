// SPDX-License-Identifier: Apache-2.0
// page_layout.h - splitting a sheet into printed pages (Page Setup, print area, print titles, page breaks)
#pragma once

#include "core/geometry.h"
#include "model/page_setup.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace yc::model {
class Sheet;
}

namespace yc::engine {

using core::Rect;

struct PrintLayout {
  double paper_w = 595, paper_h = 842;  // points, after orientation
  // the box inside the margins
  double box_x = 0, box_y = 0, box_w = 0, box_h = 0;
  double scale = 1;  // cells (and headings) are drawn at this scale
  bool empty = true;  // nothing to print
  Rect area{};
  double heading_w = 0, heading_h = 0;  // row / column headings (scaled points; 0 when not printed)
  double title_w = 0, title_h = 0;      // print title columns / rows (scaled points)
  std::vector<std::pair<uint32_t, uint32_t>> col_segs, row_segs;  // the columns / rows of each page column / page row
  struct Page {
    uint32_t r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    bool title_rows = false, title_cols = false;  // repeat the print titles on this page
    uint32_t area = 0;  // which print area (0 the first; Set Print Area over several ranges prints each on its own pages)
    int notes_from = -1, notes_to = -1;  // a page of the sheet's notes (Comments: At end of sheet): notes [from, to)
  };
  std::vector<Page> pages;
  std::vector<Rect> areas;  // every print area (area is the first)
  bool truncated = false;   // more pages than kMaxLayoutPages: the first ones only
};
// the most pages a layout makes (a value at XFD1048576 lays the used range out over tens of millions)
inline constexpr size_t kMaxLayoutPages = 200000;

// row height / column width in (unscaled) points
double RowPt(const model::Sheet& sh, uint32_t r);
double ColPt(const model::Sheet& sh, uint32_t c);

// the range printed when there is no print area: used cells, plus the cells under shapes and charts
Rect PrintableRange(const model::Sheet& sh, bool* empty);

PrintLayout LayoutPages(const model::Sheet& sh, const model::PageSetup& ps);

// header / footer text with its codes replaced: &P page, &N pages, &D date, &T time, &F file, &A sheet, &Z path, && "&";
// font codes (&"Arial,Bold", &12, &B, &I, &U, &S...) are dropped
std::string ExpandHeaderFooter(const std::string& code, int page, int pages, const std::string& file, const std::string& path,
                               const std::string& sheet, const std::string& date, const std::string& time);

// a header / footer section split into runs of one font (Format Text puts &"Font,Bold Italic", &12, &B, &I, &U, &S,
// &Krrggbb in the code); a run with picture set stands for the section's picture (&G)
struct HfRun {
  std::string text;  // with the fields filled in; "\n" starts a new line
  std::string font;  // "" = the default font
  double size = 0;   // points; 0 = the default (11)
  bool bold = false, italic = false, underline = false, strike = false;
  uint32_t color = 0;  // 0xFFRRGGBB once set, 0 = black
  bool picture = false;
};
std::vector<HfRun> HeaderFooterRuns(const std::string& code, int page, int pages, const std::string& file, const std::string& path,
                                    const std::string& sheet, const std::string& date, const std::string& time);

}  // namespace yc::engine
