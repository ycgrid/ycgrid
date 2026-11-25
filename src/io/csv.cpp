// SPDX-License-Identifier: Apache-2.0
#include "io/csv.h"

#include "core/i18n.h"
#include "core/utf8.h"

#include "core/decimal.h"
#include "engine/document.h"
#include "engine/tile_iter.h"
#include "formula/numfmt.h"
#include "formula/parser.h"
#include "io/ycg.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace yc::io {

namespace {

bool ValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n;
    if (c < 0x80) n = 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    else return false;
    if (i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

std::string WideToUtf8(const wchar_t* w, int n) {
  const int m = ::WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(m), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), m, nullptr, nullptr);
  return s;
}

}  // namespace

std::string DecodeText(std::string_view b) {
  if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF && static_cast<unsigned char>(b[1]) == 0xBB && static_cast<unsigned char>(b[2]) == 0xBF) {
    return std::string(b.substr(3));
  }
  if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xFE) {
    return WideToUtf8(reinterpret_cast<const wchar_t*>(b.data() + 2), static_cast<int>((b.size() - 2) / 2));
  }
  if (b.size() >= 2 && static_cast<unsigned char>(b[0]) == 0xFE && static_cast<unsigned char>(b[1]) == 0xFF) {
    std::wstring w((b.size() - 2) / 2, L'\0');
    for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<wchar_t>((static_cast<unsigned char>(b[2 + i * 2]) << 8) | static_cast<unsigned char>(b[3 + i * 2]));
    return WideToUtf8(w.data(), static_cast<int>(w.size()));
  }
  if (ValidUtf8(b)) return std::string(b);
  // the system's ANSI code page (Big5 / CP950 on Traditional Chinese Windows), as Excel reads such files: not CP_ACP,
  // which is UTF-8 for YCGrid (its manifest's activeCodePage), so a broker's Big5 download showed replacement marks
  DWORD cp = 0;
  if (::GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&cp),
                        sizeof(cp) / sizeof(wchar_t)) == 0 ||
      cp == 0 || cp == CP_UTF8) {
    cp = 1252;
  }
  const int n = ::MultiByteToWideChar(cp, 0, b.data(), static_cast<int>(b.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(cp, 0, b.data(), static_cast<int>(b.size()), w.data(), n);
  return WideToUtf8(w.data(), n > 0 ? n : 0);
}

CsvTable ParseCsv(std::string_view s, char delim) {
  CsvTable t;
  // "sep=;" on the first line names the separator (Excel reads it and doesn't show the line)
  if (s.size() >= 5 && (s.substr(0, 4) == "sep=" || s.substr(0, 4) == "SEP=")) {
    size_t e = 4;
    while (e < s.size() && s[e] != '\r' && s[e] != '\n') ++e;
    if (e == 5) {
      if (!delim) delim = s[4];
      if (e < s.size()) e += s[e] == '\r' && e + 1 < s.size() && s[e + 1] == '\n' ? 2 : 1;  // the line's end
      s.remove_prefix(e);
    }
  }
  if (!delim) {
    // decided by the first line: the most frequent separator (outside quotes)
    size_t counts[3] = {0, 0, 0};
    const char cands[3] = {',', '\t', ';'};
    bool q = false;
    for (char ch : s) {
      if (ch == '"') q = !q;
      if (!q && (ch == '\n' || ch == '\r')) break;
      for (int k = 0; k < 3; ++k) counts[k] += (!q && ch == cands[k]) ? 1 : 0;
    }
    delim = ',';
    size_t best = 0;
    for (int k = 0; k < 3; ++k) {
      if (counts[k] > best) {
        best = counts[k];
        delim = cands[k];
      }
    }
  }
  t.delimiter = delim;
  std::vector<std::string> row;
  std::string cell;
  bool quoted = false, was_quoted = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char ch = s[i];
    if (quoted) {
      if (ch == '"') {
        if (i + 1 < s.size() && s[i + 1] == '"') {
          cell += '"';
          ++i;
        } else {
          quoted = false;
        }
      } else {
        cell += ch;
      }
      continue;
    }
    if (ch == '"' && cell.empty() && !was_quoted) {
      quoted = was_quoted = true;
    } else if (ch == delim) {
      row.push_back(std::move(cell));
      cell.clear();
      was_quoted = false;
    } else if (ch == '\r' || ch == '\n') {
      if (ch == '\r' && i + 1 < s.size() && s[i + 1] == '\n') ++i;
      row.push_back(std::move(cell));
      cell.clear();
      was_quoted = false;
      t.rows.push_back(std::move(row));
      row.clear();
    } else {
      cell += ch;
    }
  }
  if (!cell.empty() || !row.empty() || was_quoted) {
    row.push_back(std::move(cell));
    t.rows.push_back(std::move(row));
  }
  return t;
}

bool ImportCsv(const std::wstring& path, model::Workbook* wb, std::string* error) {
  std::string bytes;
  if (!ReadFileBytes(path, &bytes)) {
    if (error) *error = TR("Couldn't read the file.");
    return false;
  }
  const CsvTable t = ParseCsv(DecodeText(bytes));
  std::wstring stem = std::filesystem::path(path).stem().wstring();
  std::string name = WideToUtf8(stem.data(), static_cast<int>(stem.size()));
  if (name.empty() || name.find_first_of("[]:*?/\\") != std::string::npos) name = TR("Sheet1");
  name = std::string(core::Utf8First(name, 31));  // Excel's limit, counted in characters (never half a character)
  engine::Document doc(model::Workbook::NewDefault(TR("Sheet")));
  const uint32_t sheet = doc.wb().sheets[0]->id;
  doc.RenameSheet(sheet, name);
  size_t width = 0;
  for (const auto& row : t.rows) width = std::max(width, std::min<size_t>(row.size(), core::kMaxCols));
  // each field read once, as typing reads it (a number, a date, a percentage, with the format it implies), and written
  // a block of rows at a time; the formats go on as runs down each column (a million rows took over four seconds a
  // field at a time, reading each one twice)
  struct Run {
    uint32_t r0 = 0;
    std::string code;
  };
  std::vector<Run> runs(width);
  auto end_run = [&](uint32_t c, uint32_t r_end) {
    Run& run = runs[c];
    if (!run.code.empty() && r_end > run.r0) {
      const uint32_t id = doc.wb_mut().styles.mut().InternString(run.code);
      doc.ModifyStyle(sheet, core::Rect{run.r0, c, r_end - 1, c}, [id](model::Style& x) { x.numfmt = id; });
    }
    run.code.clear();
  };
  constexpr size_t kBlockRows = 65536;
  std::vector<core::Value> block;
  for (size_t r0 = 0; r0 < t.rows.size() && width > 0; r0 += kBlockRows) {
    const size_t r1 = std::min(t.rows.size(), r0 + kBlockRows);
    block.assign((r1 - r0) * width, core::Value());
    for (size_t r = r0; r < r1; ++r) {
      for (size_t c = 0; c < width; ++c) {
        const std::string empty;
        const std::string& f = c < t.rows[r].size() ? t.rows[r][c] : empty;
        std::string code;
        core::Value v;
        if (!f.empty()) {
          // CSV fields are never formulas (prevents CSV injection): what typing would make a formula is kept as text
          // (=, and + or - before anything but an amount: +2+3 and -A1 became live formulas), and as Excel opens a CSV
          // (measured): an apostrophe is part of the text ('5 stays '5), and a month with a year in numbers (12/2024),
          // which only typing reads as a date, stays text
          if (f[0] == '=' || f[0] == '\'' || formula::LooksLikeFormula(f)) {
            v = core::Value::String(f);
          } else {
            formula::InputParse p = formula::ParseInput(f, doc.excel_15_digit_input);
            if (p.typed_only) {
              v = core::Value::String(f);
            } else {
              v = std::move(p.value);
              code = std::move(p.format);
            }
          }
        }
        block[(r - r0) * width + c] = std::move(v);
        if (code != runs[c].code) {
          end_run(static_cast<uint32_t>(c), static_cast<uint32_t>(r));
          runs[c].r0 = static_cast<uint32_t>(r);
          runs[c].code = std::move(code);
        }
      }
    }
    doc.SetValues(sheet, core::Rect{static_cast<uint32_t>(r0), 0, static_cast<uint32_t>(r1 - 1), static_cast<uint32_t>(width - 1)}, block,
                  engine::kSrcPython);
  }
  for (size_t c = 0; c < width; ++c) end_run(static_cast<uint32_t>(c), static_cast<uint32_t>(t.rows.size()));
  doc.Recalc();
  doc.ClearUndo();
  *wb = doc.wb();
  wb->dirty = false;
  return true;
}

bool ExportCsv(const engine::Snapshot& snap, uint32_t sheet_id, const std::wstring& path, std::string* error, char delim) {
  const model::Sheet* s = snap.Sheet(sheet_id);
  if (!s) {
    if (error) *error = TR("Sheet not found.");
    return false;
  }
  std::string out = "\xEF\xBB\xBF";
  if (s->has_used) {
    for (uint32_t r = 0; r <= s->used.r1; ++r) {
      std::string line;
      for (uint32_t c = 0; c <= s->used.c1; ++c) {
        if (c) line += delim;
        const core::Value& v = s->GetValue(r, c);
        if (v.IsEmpty()) continue;
        std::string text;
        if (v.IsDecimal()) {
          text = core::DecToString(core::DecFromValue(v));
        } else {
          const model::Style& st = snap.wb.styles->Get(s->GetStyle(r, c));
          text = formula::FormatValue(v, snap.wb.styles->NumberFormat(st)).text;
        }
        if (text.find_first_of(std::string("\"\r\n") + delim) != std::string::npos) {
          std::string q = "\"";
          for (char ch : text) {
            if (ch == '"') q += '"';
            q += ch;
          }
          text = q + "\"";
        }
        line += text;
      }
      out += line;
      out += "\r\n";
    }
  }
  return WriteFileAtomic(path, out, error);
}

}  // namespace yc::io
