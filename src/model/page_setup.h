// SPDX-License-Identifier: Apache-2.0
// page_setup.h - Page Layout -> Page Setup of a sheet (what printing and PDF export use)
#pragma once

#include "core/geometry.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace yc::model {

struct PageSetup {
  bool landscape = false;
  int paper = 9;  // Excel's xlPaper* / Windows DMPAPER_* code: 1 Letter, 5 Legal, 8 A3, 9 A4, 11 A5...
  // margins in points (Excel's "Normal": 0.7" left / right, 0.75" top / bottom, 0.3" header / footer)
  double left = 50.4, right = 50.4, top = 54, bottom = 54, header = 21.6, footer = 21.6;
  int zoom = 100;          // percent (10..400); 0 = fit to fit_wide x fit_tall pages
  int fit_wide = 1, fit_tall = 1;  // 0 = automatic (as many as needed) in that direction
  bool has_area = false;   // print area (Print_Area)
  core::Rect area{};
  bool has_title_rows = false, has_title_cols = false;  // print titles (rows / columns repeated on every page)
  uint32_t title_r0 = 0, title_r1 = 0, title_c0 = 0, title_c1 = 0;
  bool gridlines = false, headings = false, center_h = false, center_v = false, black_and_white = false;
  bool over_then_down = false;  // page order (default: down, then over)
  int first_page_number = 0;    // 0 = automatic (1)
  std::string header_left, header_center, header_right, footer_left, footer_center, footer_right;  // with &P, &N, &D, &T, &F, &A codes
  // Header/Footer: Different first page / Different odd and even pages (their own six texts: left, center, right header,
  // then footer), Scale with document and Align with page margins (stored negated: the old default stays)
  bool different_first = false, different_odd_even = false, no_scale_with_doc = false, no_align_margins = false;
  std::string first_hf[6], even_hf[6];
  std::vector<uint32_t> row_breaks, col_breaks;  // manual page breaks: a new page starts at this row / column
  // Sheet tab: Comments (0 none, 1 at end of sheet, 2 as displayed on sheet), Cell errors as (0 displayed, 1 <blank>,
  // 2 "--", 3 #N/A), Draft quality (no gridlines, pictures, shapes or charts)
  int print_comments = 0, print_errors = 0;
  bool draft = false;
  // Set Print Area / Add to Print Area over several ranges: the second, third... range (each prints on its own pages)
  std::vector<core::Rect> more_areas;
  // Header/Footer > Insert Picture (&G in a section): 0-5 = left / center / right header, then footer; the picture's
  // bytes and its printed size in points (0 = its own size at 96 dpi)
  struct HfPicture {
    std::string image;
    double w = 0, h = 0;
    bool operator==(const HfPicture&) const = default;
  };
  HfPicture hf_pictures[6];
  // any of the trailing settings above differs from the default (the .ycg page setup array then gets element 14)
  bool HasExtras() const {
    bool pics = false;
    for (const auto& p : hf_pictures) pics = pics || !p.image.empty();
    return print_comments || print_errors || draft || !more_areas.empty() || pics;
  }

  bool operator==(const PageSetup&) const = default;
};

// the header / footer text a page prints (which: 0-2 header left / center / right, 3-5 footer): page_index counts the
// sheet's pages from 0, page_number is the number printed on it (odd / even pages)
inline const std::string& HeaderFooterText(const PageSetup& ps, int which, int page_index, int page_number) {
  if (ps.different_first && page_index == 0) return ps.first_hf[which];
  if (ps.different_odd_even && page_number % 2 == 0) return ps.even_hf[which];
  const std::string* t[] = {&ps.header_left, &ps.header_center, &ps.header_right, &ps.footer_left, &ps.footer_center, &ps.footer_right};
  return *t[which];
}

// a header / footer text as Excel shows it while it is typed on the page (&[Page], &[Pages], &[Date], &[Time],
// &[Path], &[File], &[Tab]) and back to the stored codes (&P, &N, &D, &T, &Z, &F, &A); && and other codes stay
inline std::string HeaderCodesToDisplay(const std::string& t) {
  std::string out;
  for (size_t i = 0; i < t.size(); ++i) {
    if (t[i] != '&' || i + 1 >= t.size()) {
      out += t[i];
      continue;
    }
    const char c = t[i + 1];
    const char* name = c == 'P' ? "Page" : c == 'N' ? "Pages" : c == 'D' ? "Date" : c == 'T' ? "Time" : c == 'Z' ? "Path" : c == 'F' ? "File" : c == 'A' ? "Tab"
                       : c == 'G'                                                                                                  ? "Picture"
                                                                                                                                   : nullptr;
    if (name) {
      out += std::string("&[") + name + "]";
    } else {
      out += '&';
      out += c;
    }
    ++i;
  }
  return out;
}
inline std::string HeaderCodesFromDisplay(const std::string& t) {
  static const char* const kNames[][2] = {{"&[Pages]", "&N"}, {"&[Page]", "&P"}, {"&[Date]", "&D"}, {"&[Time]", "&T"},
                                          {"&[Path]", "&Z"},  {"&[File]", "&F"}, {"&[Tab]", "&A"},  {"&[Picture]", "&G"}};
  std::string out;
  for (size_t i = 0; i < t.size();) {
    bool hit = false;
    if (t[i] == '&' && i + 1 < t.size() && t[i + 1] == '&') {
      out += "&&";
      i += 2;
      continue;
    }
    for (const auto& n : kNames) {
      const size_t len = std::char_traits<char>::length(n[0]);
      if (t.compare(i, len, n[0]) == 0) {
        out += n[1];
        i += len;
        hit = true;
        break;
      }
    }
    if (!hit) out += t[i++];
  }
  return out;
}

// sizes of papers a printer offers that the table below doesn't know (Page Layout > Size lists the printer's papers):
// code -> width, height in points (portrait); set by the app from the printer driver
inline std::map<int, std::pair<double, double>>& PrinterPaperSizes(std::mutex** lock = nullptr) {
  static std::map<int, std::pair<double, double>> m;
  static std::mutex mu;
  if (lock) *lock = &mu;
  return m;
}

// paper size in points (portrait); unknown codes give A4
inline void PaperSize(int paper, double* w, double* h) {
  switch (paper) {
    case 1: *w = 612, *h = 792; break;    // Letter
    case 2: *w = 612, *h = 792; break;    // Letter Small
    case 3: *w = 792, *h = 1224; break;   // Tabloid
    case 4: *w = 1224, *h = 792; break;   // Ledger
    case 5: *w = 612, *h = 1008; break;   // Legal
    case 6: *w = 396, *h = 612; break;    // Statement
    case 7: *w = 522, *h = 756; break;    // Executive
    case 8: *w = 842, *h = 1191; break;   // A3
    case 10: *w = 595, *h = 842; break;   // A4 Small
    case 11: *w = 420, *h = 595; break;   // A5
    case 12: *w = 729, *h = 1032; break;  // B4 (JIS)
    case 13: *w = 516, *h = 729; break;   // B5 (JIS)
    case 14: *w = 612, *h = 936; break;   // Folio
    case 15: *w = 610, *h = 780; break;   // Quarto
    case 16: *w = 720, *h = 1008; break;  // 10x14 in
    case 17: *w = 792, *h = 1224; break;  // 11x17 in
    case 18: *w = 612, *h = 792; break;   // Note
    case 20: *w = 297, *h = 684; break;   // Envelope #10
    case 27: *w = 312, *h = 624; break;   // Envelope DL
    case 28: *w = 459, *h = 649; break;   // Envelope C5
    case 34: *w = 499, *h = 709; break;   // Envelope B5
    case 37: *w = 279, *h = 540; break;   // Envelope Monarch
    case 43: *w = 283, *h = 420; break;   // Japanese Postcard
    case 70: *w = 298, *h = 420; break;   // A6
    case 88: *w = 363, *h = 516; break;   // B6 (JIS)
    default: {
      std::mutex* mu = nullptr;
      auto& m = PrinterPaperSizes(&mu);
      std::lock_guard<std::mutex> g(*mu);
      if (auto it = m.find(paper); it != m.end() && it->second.first > 0 && it->second.second > 0) {
        *w = it->second.first, *h = it->second.second;
      } else {
        *w = 595, *h = 842;  // A4
      }
      break;
    }
  }
}

}  // namespace yc::model
