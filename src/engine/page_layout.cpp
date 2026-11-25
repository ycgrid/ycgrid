// SPDX-License-Identifier: Apache-2.0
// page_layout.cpp - splitting a sheet into printed pages
#include "engine/page_layout.h"

#include "model/sheet.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace yc::engine {

namespace {

constexpr double kPt = 0.75;  // sheet pixels (96 dpi) -> points

// cuts lo..hi into pages: avail(start) is the space for a page starting at start; breaks force a new page
template <class SizeFn, class AvailFn>
std::vector<std::pair<uint32_t, uint32_t>> Cut(uint32_t lo, uint32_t hi, SizeFn size, AvailFn avail, const std::vector<uint32_t>* breaks) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  uint32_t start = lo;
  double acc = 0;
  for (uint32_t i = lo; i <= hi; ++i) {
    const double s = size(i);
    const bool forced = breaks && i > start && std::find(breaks->begin(), breaks->end(), i) != breaks->end();
    if (i > start && (forced || (s > 0 && acc + s > avail(start) + 1e-6))) {
      out.emplace_back(start, i - 1);
      start = i;
      acc = 0;
    }
    acc += s;
  }
  out.emplace_back(start, hi);
  return out;
}

}  // namespace

double RowPt(const model::Sheet& sh, uint32_t r) { return sh.rows->Size(r) * kPt; }
double ColPt(const model::Sheet& sh, uint32_t c) { return sh.cols->Size(c) * kPt; }

Rect PrintableRange(const model::Sheet& sh, bool* empty) {
  model::Sheet tmp = sh;  // copying a sheet copies pointers only
  tmp.ComputeUsedRange();
  bool any = tmp.has_used;
  Rect r = tmp.used;
  auto add_box = [&](double left, double top, double w, double h, bool visible) {
    if (!visible || w <= 0 || h <= 0) return;
    const int64_t c1 = sh.cols->IndexAt(static_cast<int64_t>((left + w) / kPt - 1)), r1 = sh.rows->IndexAt(static_cast<int64_t>((top + h) / kPt - 1));
    const int64_t c0 = sh.cols->IndexAt(static_cast<int64_t>(left / kPt)), r0 = sh.rows->IndexAt(static_cast<int64_t>(top / kPt));
    const Rect b{static_cast<uint32_t>(std::max<int64_t>(0, r0)), static_cast<uint32_t>(std::max<int64_t>(0, c0)), static_cast<uint32_t>(std::max<int64_t>(0, r1)),
                 static_cast<uint32_t>(std::max<int64_t>(0, c1))};
    r = any ? r.Union(b) : b;
    any = true;
  };
  for (const auto& s : *sh.shapes) add_box(s.left, s.top, s.width, s.height, s.visible);
  for (const auto& c : *sh.charts) {
    if (!c.floating) add_box(c.left, c.top, c.width, c.height, true);
  }
  for (const auto& g : *sh.sparklines) {
    for (const auto& it : g.items) {
      const Rect b = Rect::Cell(it.r, it.c);
      r = any ? r.Union(b) : b;
      any = true;
    }
  }
  *empty = !any;
  return any ? r : Rect{};
}

PrintLayout LayoutPages(const model::Sheet& sh, const model::PageSetup& ps) {
  if (ps.has_area && !ps.more_areas.empty()) {
    // several print areas (Excel: each range prints on its own pages, in the order they were selected), all at one scale
    model::PageSetup one = ps;
    one.more_areas.clear();
    std::vector<Rect> all{ps.area};
    all.insert(all.end(), ps.more_areas.begin(), ps.more_areas.end());
    std::vector<PrintLayout> parts;
    double scale = 4;
    for (const Rect& a : all) {
      one.area = a;
      parts.push_back(LayoutPages(sh, one));
      scale = std::min(scale, parts.back().scale);
    }
    if (ps.zoom <= 0) {  // fit to pages: the smallest scale of the areas for all of them
      one.zoom = std::max(10, static_cast<int>(std::floor(scale * 100 + 1e-9)));
      for (size_t i = 0; i < all.size(); ++i) {
        one.area = all[i];
        parts[i] = LayoutPages(sh, one);
      }
    }
    PrintLayout L = parts[0];
    L.areas = all;
    for (size_t i = 1; i < parts.size(); ++i) {
      for (PrintLayout::Page p : parts[i].pages) {
        p.area = static_cast<uint32_t>(i);
        L.pages.push_back(p);
      }
      L.empty = L.empty && parts[i].empty;
    }
    return L;
  }
  PrintLayout L;
  model::PaperSize(ps.paper, &L.paper_w, &L.paper_h);
  if (ps.landscape) std::swap(L.paper_w, L.paper_h);
  L.box_x = ps.left;
  L.box_y = ps.top;
  L.box_w = std::max(36.0, L.paper_w - ps.left - ps.right);
  L.box_h = std::max(36.0, L.paper_h - ps.top - ps.bottom);
  if (ps.has_area) {
    L.area = ps.area;
    L.area.r1 = std::min(L.area.r1, sh.row_limit - 1);
    L.area.c1 = std::min(L.area.c1, sh.col_limit - 1);
    L.empty = false;
  } else {
    L.area = PrintableRange(sh, &L.empty);
  }
  L.areas = {L.area};
  if (L.empty) return L;
  const Rect a = L.area;
  const bool trows = ps.has_title_rows && ps.title_r0 <= ps.title_r1, tcols = ps.has_title_cols && ps.title_c0 <= ps.title_c1;
  double title_h = 0, title_w = 0;
  if (trows) {
    for (uint32_t r = ps.title_r0; r <= ps.title_r1; ++r) title_h += RowPt(sh, r);
  }
  if (tcols) {
    for (uint32_t c = ps.title_c0; c <= ps.title_c1; ++c) title_w += ColPt(sh, c);
  }
  // headings: a column of row numbers and a row of column letters
  const double head_h = ps.headings ? 15.0 : 0.0;
  const double head_w = ps.headings ? std::max(3.0, std::floor(std::log10(static_cast<double>(a.r1) + 1)) + 1) * 6.0 + 8.0 : 0.0;

  auto layout = [&](double s, bool use_breaks) {
    auto avail_w = [&](uint32_t start) { return L.box_w - head_w * s - (tcols && start > ps.title_c1 ? title_w * s : 0.0); };
    auto avail_h = [&](uint32_t start) { return L.box_h - head_h * s - (trows && start > ps.title_r1 ? title_h * s : 0.0); };
    L.col_segs = Cut(a.c0, a.c1, [&](uint32_t c) { return ColPt(sh, c) * s; }, avail_w, use_breaks ? &ps.col_breaks : nullptr);
    L.row_segs = Cut(a.r0, a.r1, [&](uint32_t r) { return RowPt(sh, r) * s; }, avail_h, use_breaks ? &ps.row_breaks : nullptr);
  };
  if (ps.zoom > 0) {
    L.scale = std::clamp(ps.zoom, 10, 400) / 100.0;
    layout(L.scale, true);
  } else {
    // fit to W x H pages: the largest scale (at most 100%) that fits, found by bisection
    auto fits = [&](double s) {
      layout(s, false);
      return (ps.fit_wide <= 0 || static_cast<int>(L.col_segs.size()) <= ps.fit_wide) && (ps.fit_tall <= 0 || static_cast<int>(L.row_segs.size()) <= ps.fit_tall);
    };
    double s = 1.0;
    if (!fits(1.0)) {
      double lo = 0.1, hi = 1.0;
      for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2;
        if (fits(mid)) lo = mid;
        else hi = mid;
      }
      s = std::floor(lo * 100) / 100;  // whole percents, like Excel
      if (s < 0.1) s = 0.1;
    }
    L.scale = s;
    layout(s, false);
  }
  L.heading_w = head_w * L.scale;
  L.heading_h = head_h * L.scale;
  L.title_w = title_w * L.scale;
  L.title_h = title_h * L.scale;
  auto add = [&](const std::pair<uint32_t, uint32_t>& rs, const std::pair<uint32_t, uint32_t>& cs) {
    PrintLayout::Page p;
    p.r0 = rs.first;
    p.r1 = rs.second;
    p.c0 = cs.first;
    p.c1 = cs.second;
    p.title_rows = trows && p.r0 > ps.title_r1;
    p.title_cols = tcols && p.c0 > ps.title_c1;
    L.pages.push_back(p);
  };
  // the pages are the page rows times the page columns: a value at XFD1048576 makes them tens of millions (gigabytes,
  // and the Page Layout view laid them out again at every edit: 18 s, basics seq277302). The first kMaxLayoutPages are
  // made; the view then lists them in one column, and printing stops there
  const size_t total = L.row_segs.size() * L.col_segs.size();
  L.pages.reserve(std::min(total, kMaxLayoutPages));
  if (ps.over_then_down) {
    for (size_t i = 0; i < L.row_segs.size() && L.pages.size() < kMaxLayoutPages; ++i)
      for (size_t j = 0; j < L.col_segs.size() && L.pages.size() < kMaxLayoutPages; ++j) add(L.row_segs[i], L.col_segs[j]);
  } else {
    for (size_t j = 0; j < L.col_segs.size() && L.pages.size() < kMaxLayoutPages; ++j)
      for (size_t i = 0; i < L.row_segs.size() && L.pages.size() < kMaxLayoutPages; ++i) add(L.row_segs[i], L.col_segs[j]);
  }
  L.truncated = total > kMaxLayoutPages;
  return L;
}

std::string ExpandHeaderFooter(const std::string& code, int page, int pages, const std::string& file, const std::string& path,
                               const std::string& sheet, const std::string& date, const std::string& time) {
  std::string out;
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (ch != '&' || i + 1 >= code.size()) {
      out += ch;
      continue;
    }
    const char k = code[++i];
    switch (std::toupper(static_cast<unsigned char>(k))) {
      case '&': out += '&'; break;
      case 'P': {
        // &P+n / &P-n
        int v = page;
        if (i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-')) {
          const bool neg = code[i + 1] == '-';
          size_t j = i + 2;
          int n = 0;
          while (j < code.size() && std::isdigit(static_cast<unsigned char>(code[j]))) n = n * 10 + (code[j++] - '0');
          if (j > i + 2) {
            v += neg ? -n : n;
            i = j - 1;
          }
        }
        out += std::to_string(v);
        break;
      }
      case 'N': out += std::to_string(pages); break;
      case 'D': out += date; break;
      case 'T': out += time; break;
      case 'F': out += file; break;
      case 'Z': out += path; break;
      case 'A': out += sheet; break;
      case '"': {  // &"font,style"
        const size_t e = code.find('"', i + 1);
        i = e == std::string::npos ? code.size() : e;
        break;
      }
      case 'K': i = std::min(code.size() - 1, i + 6); break;  // &Krrggbb color
      default:
        if (std::isdigit(static_cast<unsigned char>(k))) {
          while (i + 1 < code.size() && std::isdigit(static_cast<unsigned char>(code[i + 1]))) ++i;  // &12 font size
        }
        break;  // &B, &I, &U, &S, &E, &X, &Y, &L, &C, &R, &G
    }
  }
  return out;
}

std::vector<HfRun> HeaderFooterRuns(const std::string& code, int page, int pages, const std::string& file, const std::string& path,
                                    const std::string& sheet, const std::string& date, const std::string& time) {
  std::vector<HfRun> out;
  HfRun cur;
  auto flush = [&] {
    if (!cur.text.empty()) out.push_back(cur);
    cur.text.clear();
  };
  for (size_t i = 0; i < code.size(); ++i) {
    const char ch = code[i];
    if (ch != '&' || i + 1 >= code.size()) {
      cur.text += ch;
      continue;
    }
    const char k = code[++i];
    switch (std::toupper(static_cast<unsigned char>(k))) {
      case '&': cur.text += '&'; break;
      case 'P': {
        int v = page;
        if (i + 1 < code.size() && (code[i + 1] == '+' || code[i + 1] == '-')) {
          const bool neg = code[i + 1] == '-';
          size_t j = i + 2;
          int n = 0;
          while (j < code.size() && std::isdigit(static_cast<unsigned char>(code[j]))) n = n * 10 + (code[j++] - '0');
          if (j > i + 2) {
            v += neg ? -n : n;
            i = j - 1;
          }
        }
        cur.text += std::to_string(v);
        break;
      }
      case 'N': cur.text += std::to_string(pages); break;
      case 'D': cur.text += date; break;
      case 'T': cur.text += time; break;
      case 'F': cur.text += file; break;
      case 'Z': cur.text += path; break;
      case 'A': cur.text += sheet; break;
      case 'G': {
        flush();
        HfRun pic = cur;
        pic.picture = true;
        out.push_back(pic);
        break;
      }
      case '"': {  // &"Font,Style": "-" keeps the font; the style is Regular / Bold / Italic / Bold Italic
        const size_t e = code.find('"', i + 1);
        const std::string spec = code.substr(i + 1, (e == std::string::npos ? code.size() : e) - i - 1);
        i = e == std::string::npos ? code.size() : e;
        flush();
        const size_t comma = spec.find(',');
        const std::string name = spec.substr(0, comma);
        if (!name.empty() && name != "-") cur.font = name;
        if (comma != std::string::npos) {
          std::string style = spec.substr(comma + 1);
          for (char& c : style) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          cur.bold = style.find("bold") != std::string::npos;
          cur.italic = style.find("italic") != std::string::npos;
        }
        break;
      }
      case 'K': {  // &Krrggbb
        flush();
        if (i + 6 < code.size()) {
          const std::string hex = code.substr(i + 1, 6);
          char* end = nullptr;
          const unsigned long v = std::strtoul(hex.c_str(), &end, 16);
          if (end == hex.c_str() + 6) cur.color = 0xFF000000u | static_cast<uint32_t>(v);
        }
        i = std::min(code.size() - 1, i + 6);
        break;
      }
      case 'B': flush(); cur.bold = !cur.bold; break;
      case 'I': flush(); cur.italic = !cur.italic; break;
      case 'U':
      case 'E': flush(); cur.underline = !cur.underline; break;
      case 'S': flush(); cur.strike = !cur.strike; break;
      default:
        if (std::isdigit(static_cast<unsigned char>(k))) {
          flush();
          int n = k - '0';
          while (i + 1 < code.size() && std::isdigit(static_cast<unsigned char>(code[i + 1]))) n = n * 10 + (code[++i] - '0');
          cur.size = std::clamp(n, 1, 409);
        }
        break;  // &X, &Y, &L, &C, &R
    }
  }
  flush();
  return out;
}

}  // namespace yc::engine
