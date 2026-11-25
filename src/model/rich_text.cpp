// SPDX-License-Identifier: Apache-2.0
// rich_text.cpp - runs of fonts inside a cell's text
#include "model/rich_text.h"

#include <algorithm>

namespace yc::model {

size_t CharCount(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

size_t ByteOfChar(std::string_view s, size_t ch) {
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
    if (n == ch) return i;
    ++n;
  }
  return s.size();
}

RunFont ResolveRunFont(const StyleTable& t, const Style& cell, const RichRun* run) {
  RunFont f;
  f.name = t.FontName(cell);
  f.size = t.FontSize(cell);
  f.color = cell.font_color;
  f.bold = cell.bold;
  f.italic = cell.italic;
  f.strike = cell.strike;
  f.underline = cell.underline;
  f.script = cell.script;
  if (!run) return f;
  if (!run->font.empty()) f.name = run->font;
  if (run->size > 0) f.size = run->size;
  if (run->color) f.color = run->color;
  if (run->bold >= 0) f.bold = run->bold != 0;
  if (run->italic >= 0) f.italic = run->italic != 0;
  if (run->strike >= 0) f.strike = run->strike != 0;
  if (run->underline >= 0) f.underline = static_cast<Underline>(run->underline);
  if (run->script >= 0) f.script = static_cast<uint8_t>(run->script);
  return f;
}

const RichRun* RunAt(const RichText& rt, size_t ch) {
  const RichRun* hit = nullptr;
  for (const auto& r : rt.runs) {
    if (r.start > ch) break;
    hit = &r;
  }
  return hit;
}

namespace {

// a run boundary at ch (the new run copies the font in effect there)
size_t SplitAt(RichText* rt, size_t ch) {
  auto& v = rt->runs;
  if (v.empty() || v.front().start != 0) v.insert(v.begin(), RichRun{});
  size_t i = 0;
  while (i + 1 < v.size() && v[i + 1].start <= ch) ++i;
  if (v[i].start == ch) return i;
  RichRun r = v[i];
  r.start = static_cast<uint32_t>(ch);
  v.insert(v.begin() + static_cast<std::ptrdiff_t>(i) + 1, r);
  return i + 1;
}

}  // namespace

bool Normalize(RichText* rt) {
  auto& v = rt->runs;
  const size_t n = CharCount(rt->text);
  v.erase(std::remove_if(v.begin(), v.end(), [&](const RichRun& r) { return r.start >= n && n > 0; }), v.end());
  std::stable_sort(v.begin(), v.end(), [](const RichRun& a, const RichRun& b) { return a.start < b.start; });
  std::vector<RichRun> out;
  for (const auto& r : v) {
    if (!out.empty() && out.back().start == r.start) out.back() = r;  // the later one wins
    else if (!out.empty() && out.back().SameFont(r)) continue;
    else out.push_back(r);
  }
  if (!out.empty()) out.front().start = 0;
  v = std::move(out);
  return std::any_of(v.begin(), v.end(), [](const RichRun& r) { return !r.Inherits(); });
}

void ApplyToChars(RichText* rt, size_t start, size_t len, const std::function<void(RichRun&)>& fn) {
  const size_t n = CharCount(rt->text);
  start = std::min(start, n);
  const size_t end = std::min(n, start + len);
  if (end <= start) return;
  const size_t a = SplitAt(rt, start);
  const size_t b = end < n ? SplitAt(rt, end) : rt->runs.size();
  for (size_t i = a; i < b; ++i) fn(rt->runs[i]);
  Normalize(rt);
}

namespace {

// code points as the byte sequences that encode them (enough to compare)
std::vector<std::string_view> Chars(std::string_view s) {
  std::vector<std::string_view> v;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i + 1;
    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) ++j;
    v.push_back(s.substr(i, j - i));
    i = j;
  }
  return v;
}

}  // namespace

RichText RemapRuns(const RichText& old, const std::string& new_text) {
  RichText out;
  out.text = new_text;
  if (old.runs.empty()) return out;
  const auto a = Chars(old.text), b = Chars(new_text);
  size_t prefix = 0;
  while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
  size_t suffix = 0;
  while (suffix < a.size() - prefix && suffix < b.size() - prefix && a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) ++suffix;
  const size_t old_mid_end = a.size() - suffix, new_mid_end = b.size() - suffix;
  for (const auto& r : old.runs) {
    RichRun x = r;
    if (r.start < prefix || r.start == 0) {
      out.runs.push_back(x);
    } else if (r.start >= old_mid_end) {
      x.start = static_cast<uint32_t>(r.start - old_mid_end + new_mid_end);
      out.runs.push_back(x);
    }
    // runs starting inside the replaced middle disappear; typed text continues the run before it
  }
  // the run in effect where the unchanged end starts must still begin there
  if (suffix > 0 && old_mid_end > prefix) {
    if (const RichRun* at = RunAt(old, old_mid_end)) {
      RichRun x = *at;
      x.start = static_cast<uint32_t>(new_mid_end);
      out.runs.push_back(x);
    }
  }
  Normalize(&out);
  return out;
}

RichText ReplaceChars(const RichText& rt, size_t start, size_t len, const std::string& ins) {
  const size_t n = CharCount(rt.text);
  start = std::min(start, n);
  len = std::min(len, n - start);
  if (len == 0 && ins.empty()) return rt;
  RichText out;
  const size_t a = ByteOfChar(rt.text, start), b = ByteOfChar(rt.text, start + len);
  out.text = rt.text.substr(0, a) + ins + rt.text.substr(b);
  if (rt.runs.empty()) return out;
  const size_t ins_n = CharCount(ins);
  // the font the new text takes
  const RichRun* first = RunAt(rt, len > 0 ? start : (start > 0 ? start - 1 : 0));
  for (const auto& r : rt.runs) {
    if (r.start < start) {
      out.runs.push_back(r);
    } else if (r.start >= start + len && (r.start > start || len > 0)) {
      RichRun x = r;
      x.start = static_cast<uint32_t>(r.start - len + ins_n);
      out.runs.push_back(x);
    }
  }
  if (first && ins_n > 0) {
    RichRun x = *first;
    x.start = static_cast<uint32_t>(start);
    out.runs.push_back(x);
    // what follows keeps its own font
    if (start + len < n) {
      if (const RichRun* after = RunAt(rt, start + len)) {
        RichRun y = *after;
        y.start = static_cast<uint32_t>(start + ins_n);
        out.runs.push_back(y);
      }
    }
  }
  Normalize(&out);
  return out;
}

RichLayout LayoutRich(const RichText& rt, bool wrap, float wrap_width, int halign, float empty_h,
                      const std::function<std::pair<float, float>(const std::string&, size_t)>& measure) {
  // tokens: a run's text cut at line breaks and (when wrapping) after spaces
  struct Tok {
    size_t run;
    std::string text;
    bool newline;
  };
  std::vector<Tok> toks;
  const size_t nruns = std::max<size_t>(1, rt.runs.size());
  for (size_t i = 0; i < nruns; ++i) {
    const size_t a = rt.runs.empty() ? 0 : ByteOfChar(rt.text, rt.runs[i].start);
    const size_t b = i + 1 < rt.runs.size() ? ByteOfChar(rt.text, rt.runs[i + 1].start) : rt.text.size();
    std::string cur;
    for (size_t k = a; k < b; ++k) {
      const char ch = rt.text[k];
      if (ch == '\n') {
        if (!cur.empty()) toks.push_back({i, std::move(cur), false});
        cur.clear();
        toks.push_back({i, {}, true});
        continue;
      }
      if (ch == '\r') continue;
      cur += ch;
      if (wrap && ch == ' ') {
        toks.push_back({i, std::move(cur), false});
        cur.clear();
      }
    }
    if (!cur.empty()) toks.push_back({i, std::move(cur), false});
  }
  RichLayout out;
  struct Line {
    size_t first = 0, last = 0;  // pieces [first, last)
    float w = 0, h = 0;
  };
  std::vector<Line> lines(1);
  float x = 0;
  for (auto& t : toks) {
    if (t.newline) {
      lines.back().last = out.pieces.size();
      lines.push_back(Line{out.pieces.size(), out.pieces.size()});
      x = 0;
      continue;
    }
    auto [w, h] = measure(t.text, t.run);
    if (wrap && x > 0 && x + w > wrap_width) {
      // trailing spaces don't count at the end of a line
      if (t.text.find_first_not_of(' ') == std::string::npos) continue;
      lines.back().last = out.pieces.size();
      lines.push_back(Line{out.pieces.size(), out.pieces.size()});
      x = 0;
    }
    RichPiece p;
    p.run = t.run;
    p.text = std::move(t.text);
    p.x = x;
    p.w = w;
    p.h = h;
    out.pieces.push_back(std::move(p));
    x += w;
    lines.back().w = x;
    lines.back().h = std::max(lines.back().h, h);
  }
  lines.back().last = out.pieces.size();
  float maxw = 0;
  for (const auto& l : lines) maxw = std::max(maxw, l.w);
  const float box = wrap ? std::max(wrap_width, 0.0f) : maxw;
  float y = 0;
  for (auto& l : lines) {
    if (l.h <= 0) l.h = empty_h;
    const float off = halign == 1 ? (box - l.w) * 0.5f : halign == 2 ? box - l.w : 0.0f;
    for (size_t i = l.first; i < l.last; ++i) {
      out.pieces[i].x += off;
      out.pieces[i].y = y + l.h - out.pieces[i].h;
    }
    y += l.h;
  }
  out.width = box;
  out.height = y;
  return out;
}

}  // namespace yc::model
