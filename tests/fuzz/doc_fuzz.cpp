// SPDX-License-Identifier: Apache-2.0
// The Document under random operations, in-process (thousands of steps a second, where the UI's chaos takes six
// minutes for five hundred): a stream of bytes picks each operation and its cells, and after each one
//   1. Undo gives back exactly what was there and Redo exactly what the operation made (io::ContentDump);
//      an operation that changed the workbook left an Undo step;
//   2. every few steps a .ycg save and open gives back the same content, worked out again by a full recalculation
//      (so the incremental results agree with a fresh calculation);
//   3. nothing throws.
// A failing stream is written to a file and replays with --replay; the same entry point (LLVMFuzzerTestOneInput)
// takes libFuzzer's input when the target is built with /fsanitize=fuzzer (YC_LIBFUZZER).
//
//   yc_doc_fuzz [--seconds N] [--seed S] [--cases N] [--out DIR] [--replay FILE [--minimize]] [--corpus DIR] [--verbose] [--every-step]
//               [--wide]
// --corpus replays every stream of a folder first (tests/fuzz/corpus: the ones that failed once, each fixed).
// A failing stream found by seed is cut down (--minimize) to the fewest bytes that still fail the same way.
#include "core/geometry.h"
#include "engine/document.h"
#include "formula/datetime.h"
#include "io/content_dump.h"
#include "io/xlsx.h"
#include "io/ycg.h"
#include "model/rules.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <tuple>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace engine = yc::engine;
namespace model = yc::model;
namespace io = yc::io;
using yc::core::Rect;
using yc::core::Value;

namespace {

bool g_verbose = false;
bool g_every_step = false;  // --every-step: the save / open check after every step, not every sixth (to find the step)
// --wide: one range in 32 is whole columns, whole rows or the whole sheet, as a person selects with a header click or
// Ctrl+A (the operations over whole lines froze one by one: borders, Merge, Fill Right, Remove Duplicates). Streams are
// read the same way only with the same flag: tests/fuzz/corpus/wide holds the ones found so
bool g_wide = false;
// --xlsx: the save / open check also writes an Excel copy and opens it, comparing every cell's formula and value
// (what an .xlsx keeps of the cells; it is checked while the workbook holds nothing the copy leaves out)
bool g_xlsx = false;
// --more: twelve more operations (moving and hiding sheets, Paste Special, borders, fonts, hidden lines, Replace,
// Remove Duplicates, Fill Right, Clear, Subtotal, outlines, notes); tests/fuzz/corpus/more holds the streams found so
bool g_more = false;
// --gaps: the --more operations and seven of the workbook's calculation settings and their inputs: the 1904 date
// system, precision as displayed, a full calculation every time, a sheet's calculation turned off and on, dates and
// numbers in formats that round, and a calculation interrupted part way (CalculationInterruptKey), then finished
bool g_gaps = false;
// a step that never ends (an endless loop) never returns to be checked: a watchdog thread writes the case's stream
// and ends the process when a step has run for more than 120 s (a --wide round ran for its whole 50 minutes with
// nothing kept, round 2340)
std::atomic<int64_t> g_step_since{0};  // steady-clock ms when the running step began, 0 between steps
std::atomic<int64_t> g_case_since{0};  // ... when the running case began: this tool's own checks may stall too
const std::vector<uint8_t>* g_case_bytes = nullptr;
std::atomic<uint64_t> g_case_seed{0};
std::string g_out_dir = ".";
// --keep-case: each case's stream is written to doc_fuzz_current_<the run's --seed>.bin before it runs, for a case that
// ends the process
// (an allocation past a job's memory limit fails fast, and nothing else is kept)
bool g_keep_case = false;
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void StartWatchdog() {
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const int64_t since = g_step_since.load(), case_since = g_case_since.load();
      const bool step_stuck = since && NowMs() - since >= 120'000;
      const bool case_stuck = case_since && NowMs() - case_since >= 600'000;  // the checks after the steps too
      if (!step_stuck && !case_stuck) continue;
      const uint64_t seed = g_case_seed.load();
      const std::string path = g_out_dir + "/doc_fuzz_" + std::to_string(seed) + ".bin";
      if (g_case_bytes) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(g_case_bytes->data()), static_cast<std::streamsize>(g_case_bytes->size()));
      }
      std::printf("seed %llu FAILED: %s\n  replay: yc_doc_fuzz%s%s%s%s --verbose --replay %s\n", static_cast<unsigned long long>(seed),
                  step_stuck ? "a step has run for more than 120 s (it never ended)" : "the case has run for more than 10 minutes (a check after its steps never ended)", g_wide ? " --wide" : "", g_xlsx ? " --xlsx" : "", g_more ? " --more" : "", g_gaps ? " --gaps" : "", path.c_str());
      std::fflush(stdout);
      std::_Exit(1);
    }
  }).detach();
}
// --step-limit S: the CPU seconds a step may take (lower to cut a slow step's stream down). With --wide 120: a million
// formulas that each read a whole column (a calculated column of =COUNT(A:A) in a table as tall as the sheet) is a
// million times a column's cells, in Excel too; the watchdog still ends a step that never does
double g_step_limit = 0;
uint64_t g_xlsx_checked = 0, g_xlsx_skipped = 0;

// the CPU time this process has used, all its threads (the engine's parallel calculation too)
double CpuSeconds() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  auto sec = [](const FILETIME& f) { return static_cast<double>((uint64_t{f.dwHighDateTime} << 32) | f.dwLowDateTime) / 1e7; };
  return sec(k) + sec(u);
#else
  return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

// a reference to a deleted sheet (#REF!A1) is written #REF!, which Excel opens (xlsx.cpp AddPrefixes)
std::string CutRefTails(std::string text) {
  for (size_t at = text.find("#REF!"); at != std::string::npos; at = text.find("#REF!", at + 5)) {
    size_t e = at + 5;
    while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '$' || text[e] == ':')) ++e;
    text.erase(at + 5, e - at - 5);
  }
  return text;
}

// the cells as an Excel copy keeps them: input text and value, and the merged areas
std::string CellsDump(const model::Workbook& wb) {
  std::string out;
  for (const auto& sp : wb.sheets) {
    const model::Sheet& s = *sp;
    out += "sheet " + s.name + "\n";
    std::vector<std::pair<uint32_t, uint32_t>> cells;
    s.values.ForEach([&](uint32_t rt, uint32_t ct, const model::ValueTile& t) {
      for (uint32_t i = 0; i < model::kTileCells; ++i) {
        if (!t.v[i].IsEmpty()) cells.push_back({rt * model::kTileRows + i % model::kTileRows, ct * model::kTileCols + i / model::kTileRows});
      }
    });
    s.meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
      if (t.formula_count == 0) return;
      for (uint32_t i = 0; i < model::kTileCells; ++i) {
        if (t.formula[i]) cells.push_back({rt * model::kTileRows + i % model::kTileRows, ct * model::kTileCols + i / model::kTileRows});
      }
    });
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    for (const auto& [r, c] : cells) {
      const Value v = s.GetValue(r, c);
      char buf[64];
      std::string val;
      if (v.IsEmpty()) val = "-";
      else if (v.IsNumeric()) { std::snprintf(buf, sizeof(buf), "%.15g", v.AsDouble()); val = buf; }
      else if (v.IsBool()) val = v.boolean() ? "TRUE" : "FALSE";
      else if (v.IsError()) val = yc::core::ErrorText(v.error());
      else val = "\"" + std::string(v.str()) + "\"";
      std::string input = CutRefTails(engine::InputTextOf(wb, s.id, r, c));
      // a newer error as a constant (#SPILL! pasted as a value) is written #VALUE!: the file holds the classic ones
      // only (xlsx.cpp; Excel keeps the others as rich values, which an Excel copy doesn't carry)
      if (v.IsError() && !s.GetFormula(r, c) && v.error() > yc::core::Err::NA) {
        val = "#VALUE!";
        input = "#VALUE!";
      }
      // what the copy keeps of the format a cell shows (its own, its row's or its column's)
      const model::Style& st = engine::StyleOf(wb, s, r, c);
      char fmt[160];
      std::snprintf(fmt, sizeof(fmt), "%s%s%s%s h%d v%d fill %08x color %08x border %d%d%d%d",
                    st.bold ? "b" : "", st.italic ? "i" : "", st.strike ? "s" : "", st.wrap ? "w" : "",
                    static_cast<int>(st.halign), static_cast<int>(st.valign), st.fill_color, st.font_color,
                    static_cast<int>(st.border[model::kLeft].style), static_cast<int>(st.border[model::kTop].style),
                    static_cast<int>(st.border[model::kRight].style), static_cast<int>(st.border[model::kBottom].style));
      out += "  " + yc::core::CellA1(r, c) + "\t" + input + "\t" + val + "\t[" + wb.styles->NumberFormat(st) + "] " + fmt + "\n";
    }
    std::vector<Rect> merges(s.merges->begin(), s.merges->end());
    std::sort(merges.begin(), merges.end(), [](const Rect& a, const Rect& b) { return std::tie(a.r0, a.c0) < std::tie(b.r0, b.c0); });
    for (const auto& m : merges) out += "  merged " + yc::core::CellA1(m.r0, m.c0) + ":" + yc::core::CellA1(m.r1, m.c1) + "\n";
    // the sizes and hidden lines (the copy keeps widths and heights)
    for (const auto* dim : {&*s.rows, &*s.cols}) {
      const bool cols = dim == &*s.cols;
      for (const auto& e : dim->Entries()) {
        if (e.size == dim->default_size()) continue;
        out += std::string("  ") + (cols ? "col " + yc::core::ColumnName(static_cast<uint32_t>(e.index)) : "row " + std::to_string(e.index + 1)) +
               " size " + std::to_string(e.size) + "\n";
      }
    }
    if (!s.visible) out += "  hidden\n";
  }
  // the defined names, each with the sheet it belongs to
  std::vector<std::string> names;
  for (const auto& n : *wb.names) {
    const int32_t si = n.sheet_scope >= 0 ? wb.IndexOfId(static_cast<uint32_t>(n.sheet_scope)) : -1;
    names.push_back("name " + (si >= 0 ? wb.sheets[static_cast<size_t>(si)]->name + "!" : std::string()) + n.name + " " + CutRefTails(n.refers_to) + "\n");
  }
  std::sort(names.begin(), names.end());
  for (const auto& n : names) out += n;
  return out;
}

// the bytes as a source of choices; past the end every choice is 0
struct Bytes {
  const uint8_t* p;
  size_t n, at = 0;
  bool Done() const { return at >= n; }
  uint32_t U8() { return at < n ? p[at++] : 0; }
  uint32_t Pick(uint32_t k) { return k ? U8() % k : 0; }
};

constexpr uint32_t kRows = 12, kCols = 6;  // the grid the operations work in: A1:F12

Rect PickRect(Bytes& b) {
  const uint32_t r0 = b.Pick(kRows), c0 = b.Pick(kCols);
  const uint32_t r1 = r0 + b.Pick(3), c1 = c0 + b.Pick(2);
  if (g_wide && b.Pick(32) == 0) {  // rare: every check after a step then reads millions of cells
    // to the sheet's last row, as a header click selects (the hard row limit grew the sheet to two billion rows
    // through an array entered there, which no person or script reaches: the bridge clamps to the sheet)
    constexpr uint32_t kLastRow = 1048575;
    // with --more also from a cell to the sheet's last cell, as Shift+click there or Ctrl+Shift+End selects (Wrap Text
    // over D5:XFD1048576, then rows inserted: basics seq327428)
    switch (b.Pick(g_more ? 4 : 3)) {
      case 0: return Rect{0, c0, kLastRow, c1};  // whole columns
      case 1: return Rect{r0, 0, r1, yc::core::kMaxCols - 1};  // whole rows
      case 3: return Rect{r0, c0, kLastRow, yc::core::kMaxCols - 1};  // to the last cell
      default: return Rect{0, 0, kLastRow, yc::core::kMaxCols - 1};  // the whole sheet
    }
  }
  return Rect{r0, c0, r1, c1};
}

const char* const kFormulas[] = {
    "=A1+B2", "=SUM(A1:C5)", "=B3*2", "=IF(A1>2,C1,D2)", "=COUNT(A:A)", "=$A$1&B1", "=SUM(Data!A1:A3)", "=SEQUENCE(3)",
    "=INDEX(A1:C5,2,2)", "=OFFSET(A1,1,1)", "=Rate*2", "=A1:A3*2", "=SUMIF(A1:A10,\">2\")", "=VLOOKUP(A1,A1:C9,2,FALSE)",
    "=C1#", "=TRANSPOSE(A1:B2)", "=Data!B2+1", "=IFERROR(1/A2,\"x\")", "=ROW()", "=SUM(Table1[h])",
};
// dates, times and date functions: what the 1904 date system and precision as displayed change
const char* const kDates[] = {
    "=DATE(2024,1,31)", "=A1+1", "=WEEKDAY(B1)", "=YEAR(A1)", "=EDATE(A1,1)", "=DATEVALUE(\"2024-03-01\")",
    "=TEXT(A1,\"yyyy-mm-dd\")", "=DAY(0)", "=EOMONTH(A1,0)", "=NETWORKDAYS(A1,A1+10)", "2024-01-05", "1/2/2026", "12:30",
    "=DATE(1903,12,31)", "=TIME(25,0,0)", "=A1*1.005", "=ROUND(B2,1)", "1.23456", "-0.005", "12345.678", "0.125", "1E-7",
    "=1/3", "=B1/7",
};
const char* const kRoundingFormats[] = {"0%", "#,##0.0,", "0.0E+00", "# ?/?", "0", "$#,##0.00", "0.000", "General"};
const char* const kTexts[] = {"apple", "Pear", "x", "TRUE", "12%", "2024-01-05", "$5", "=", "a b", "00123"};

struct Fuzz {
  engine::Document d;
  std::vector<std::string> log;  // the operations so far, as text
  std::string failure;
  uint32_t numfmt[3] = {0, 0, 0};
  std::vector<uint32_t> rounding;  // --gaps: kRoundingFormats
  bool no_step_ = false;  // the step changed a setting that isn't the workbook's content and leaves no Undo step
  bool interrupted_ = false;  // the step's calculation was interrupted part way and finished afterwards
  bool cyclic_before_ = false;  // the workbook had a circular reference before this step

  Fuzz() {
    d.AddSheet("Data");
    numfmt[0] = d.wb_mut().styles.mut().InternString("0.00");
    numfmt[1] = d.wb_mut().styles.mut().InternString("yyyy-mm-dd");
    numfmt[2] = d.wb_mut().styles.mut().InternString("@");
    model::NameDef n;
    n.name = "Rate";
    n.refers_to = "=Sheet1!$B$2";
    d.DefineName(n);
    if (g_gaps) {
      // General is format 0, as every way a person or Python gives it stores it
      for (const char* f : kRoundingFormats) rounding.push_back(std::strcmp(f, "General") == 0 ? 0 : d.wb_mut().styles.mut().InternString(f));
    }
    yc::formula::SetDate1904(false);  // the case before may have left the 1904 system on
    d.ClearUndo();
  }

  uint32_t Sheet(Bytes& b) const {
    const auto& v = d.wb().sheets;
    uint32_t id = v[b.Pick(static_cast<uint32_t>(v.size()))]->id;
    for (int i = 0; i < 4 && d.S(id) && d.S(id)->chart_sheet; ++i) id = v[b.Pick(static_cast<uint32_t>(v.size()))]->id;
    return id;
  }

  // one operation; returns false for Undo / Redo (they aren't checked as steps of their own)
  bool Step(Bytes& b) {
    const uint32_t op = b.Pick(g_gaps ? 47 : g_more ? 40 : 28);
    const uint32_t sh = Sheet(b);
    std::ostringstream what;
    what << "op " << op << " sheet " << sh;
    auto note = [&](const std::string& s) {
      what << " " << s;
      if (g_verbose) {  // before the operation runs: a run stopped inside it (out of memory) names it
        std::printf("  > %s\n", what.str().c_str());
        std::fflush(stdout);
      }
    };
    auto A1 = [](const Rect& r) { return yc::core::RectA1(r); };
    switch (op) {
      case 0: {
        const uint32_t r = b.Pick(kRows), c = b.Pick(kCols), v = b.U8();
        note("set " + yc::core::CellA1(r, c) + " = " + std::to_string(v));
        d.SetInput(sh, r, c, std::to_string(static_cast<int>(v) - 40));
        break;
      }
      case 1: {
        const uint32_t r = b.Pick(kRows), c = b.Pick(kCols);
        const char* t = kTexts[b.Pick(sizeof(kTexts) / sizeof(*kTexts))];
        note("type " + yc::core::CellA1(r, c) + " " + t);
        d.SetInput(sh, r, c, t);
        break;
      }
      case 2: case 3: {
        const uint32_t r = b.Pick(kRows), c = b.Pick(kCols);
        const char* f = kFormulas[b.Pick(sizeof(kFormulas) / sizeof(*kFormulas))];
        note("formula " + yc::core::CellA1(r, c) + " " + f);
        d.SetInput(sh, r, c, f);
        break;
      }
      case 4: {
        const Rect r = PickRect(b);
        note("clear " + A1(r));
        d.ClearContents(sh, r);
        break;
      }
      case 5: {
        const uint32_t at = b.Pick(kRows), n = 1 + b.Pick(2);
        note("insert rows " + std::to_string(at) + "+" + std::to_string(n));
        d.InsertRows(sh, at, n);
        break;
      }
      case 6: {
        const uint32_t at = b.Pick(kRows), n = 1 + b.Pick(2);
        note("delete rows " + std::to_string(at) + "+" + std::to_string(n));
        d.DeleteRows(sh, at, n);
        break;
      }
      case 7: {
        const uint32_t at = b.Pick(kCols);
        note("insert col " + std::to_string(at));
        d.InsertCols(sh, at, 1);
        break;
      }
      case 8: {
        const uint32_t at = b.Pick(kCols);
        note("delete col " + std::to_string(at));
        d.DeleteCols(sh, at, 1);
        break;
      }
      case 9: {
        const Rect r = PickRect(b);
        const bool down = b.Pick(2);
        note("insert cells " + A1(r) + (down ? " down" : " right"));
        d.InsertCells(sh, r, down);
        break;
      }
      case 10: {
        const Rect r = PickRect(b);
        const bool up = b.Pick(2);
        note("delete cells " + A1(r) + (up ? " up" : " left"));
        d.DeleteCells(sh, r, up);
        break;
      }
      case 11: {
        const Rect r = PickRect(b);
        const uint32_t to = Sheet(b), dr = b.Pick(kRows), dc = b.Pick(kCols);
        note("cut " + A1(r) + " to sheet " + std::to_string(to) + " " + yc::core::CellA1(dr, dc));
        d.MoveCells(sh, r, to, dr, dc);
        break;
      }
      case 12: {
        const Rect r = PickRect(b);
        const uint32_t dr = b.Pick(kRows), dc = b.Pick(kCols);
        note("copy " + A1(r) + " to " + yc::core::CellA1(dr, dc));
        const engine::ClipData clip = d.Copy(sh, r);
        d.Paste(sh, dr, dc, clip);
        break;
      }
      case 13: {
        const Rect r = PickRect(b);
        if (b.Pick(2)) {
          note("merge " + A1(r));
          d.Merge(sh, r);
        } else {
          note("unmerge " + A1(r));
          d.Unmerge(sh, r);
        }
        break;
      }
      case 14: {
        const Rect r = PickRect(b);
        const uint32_t nf = numfmt[b.Pick(3)];
        note("format " + A1(r) + " " + std::to_string(nf));
        d.ModifyStyle(sh, r, [nf](model::Style& s) { s.numfmt = nf; });
        break;
      }
      case 15: {
        const Rect r = PickRect(b);
        note("fill down " + A1(r));
        d.FillDown(sh, r);
        break;
      }
      case 16: {
        Rect r = PickRect(b);
        r.r1 += 2;
        note("sort " + A1(r));
        d.Sort(sh, r, {engine::SortKey{r.c0}}, b.Pick(2) != 0);
        break;
      }
      case 17: {
        model::CfRule rule;
        const Rect r = PickRect(b);
        rule.ranges.push_back(r);
        rule.type = model::CfType::Expression;
        rule.formula1 = b.Pick(2) ? "$A1>2" : "SUM($B$1:$B$4)>3";
        note("rule " + A1(r) + " " + rule.formula1);
        d.AddCondFormat(sh, rule);
        break;
      }
      case 18: {
        model::DvRule rule;
        const Rect r = PickRect(b);
        rule.type = model::DvType::List;
        rule.formula1 = b.Pick(2) ? "=$A$1:$A$4" : "Buy,Sell";
        rule.ranges.push_back(r);
        note("validation " + A1(r) + " " + rule.formula1);
        d.SetValidation(sh, r, rule);
        break;
      }
      case 19: {
        model::NameDef n;
        n.name = b.Pick(2) ? "Rate" : "Band";
        const Rect r = PickRect(b);
        n.refers_to = "=" + std::string(b.Pick(2) ? "Sheet1" : "Data") + "!" + yc::core::RectA1(r);
        note("name " + n.name + " " + n.refers_to);
        d.DefineName(n);
        break;
      }
      case 20: {
        const char* names[] = {"Data", "Prices", "Q 1"};
        const char* to = names[b.Pick(3)];
        note(std::string("rename to ") + to);
        d.RenameSheet(sh, to);
        break;
      }
      case 21: {
        if (d.wb().sheets.size() < 5) {
          note("copy sheet");
          d.CopySheet(sh);
        }
        break;
      }
      case 22: {
        if (d.wb().sheets.size() > 2) {
          note("delete sheet");
          d.DeleteSheet(sh);
        }
        break;
      }
      case 23: {
        Rect r = PickRect(b);
        r.r1 += 1;
        note("table " + A1(r));
        d.AddTable(sh, r, true, "TableStyleMedium2");
        break;
      }
      case 24: {
        Rect r = PickRect(b);
        r.r1 += 1;
        note("autofilter " + A1(r));
        d.SetAutoFilter(sh, r);
        break;
      }
      case 25: {
        Rect r = PickRect(b);
        note("array " + A1(r));
        d.SetArrayFormula(sh, r, "A1:B3*2");
        break;
      }
      case 26: {
        const Rect r = PickRect(b);
        model::ChartObj c;
        c.series.emplace_back();
        c.series[0].values = d.S(sh)->name.find(' ') == std::string::npos ? d.S(sh)->name + "!" + A1(r) : "'" + d.S(sh)->name + "'!" + A1(r);
        note("chart " + c.series[0].values);
        d.AddChart(sh, c);
        break;
      }
      case 27: {
        const Rect src = PickRect(b);
        Rect dest = src;
        dest.r1 += 1 + b.Pick(3);
        note("autofill " + A1(src) + " to " + A1(dest));
        d.AutoFill(sh, src, dest);
        break;
      }
      // --more: what people do every day besides (the streams found with these replay only with --more)
      case 28: {
        const int32_t to = static_cast<int32_t>(b.Pick(static_cast<uint32_t>(d.wb().sheets.size())));
        note("move sheet to " + std::to_string(to));
        d.MoveSheet(sh, to);
        break;
      }
      case 29: {
        const Rect r = PickRect(b);
        const uint32_t to = Sheet(b), dr = b.Pick(kRows), dc = b.Pick(kCols);
        static const engine::PasteMode kModes[] = {engine::PasteMode::All, engine::PasteMode::Values, engine::PasteMode::Formulas,
                                                   engine::PasteMode::Formats, engine::PasteMode::ValuesAndFormats,
                                                   engine::PasteMode::NoBorders, engine::PasteMode::FormulasAndFormats};
        const uint32_t mode = b.Pick(7), transpose = b.Pick(2), skip = b.Pick(2), pop = b.Pick(5);
        note("paste special " + A1(r) + " to sheet " + std::to_string(to) + " " + yc::core::CellA1(dr, dc) + " mode " +
             std::to_string(mode) + (transpose ? " transpose" : "") + (skip ? " skip blanks" : "") + " op " + std::to_string(pop));
        const engine::ClipData clip = d.Copy(sh, r);
        d.Paste(to, dr, dc, clip, kModes[mode], transpose != 0, skip != 0, static_cast<engine::PasteOp>(pop));
        break;
      }
      case 30: {
        const Rect r = PickRect(b);
        const uint32_t preset = b.Pick(18);
        note("borders " + A1(r) + " " + std::to_string(preset));
        d.SetBorders(sh, r, static_cast<engine::BorderPreset>(preset), b.Pick(2) ? model::LineStyle::Thin : model::LineStyle::Double);
        break;
      }
      case 31: {
        const Rect r = PickRect(b);
        const uint32_t k = b.Pick(4);
        note("style " + A1(r) + " " + std::to_string(k));
        d.ModifyStyle(sh, r, [k](model::Style& s) {
          if (k == 0) s.bold = !s.bold;
          else if (k == 1) s.fill_color = 0xFFFFFF00u;
          else if (k == 2) s.wrap = !s.wrap;
          else s.halign = model::HAlign::Center;
        });
        break;
      }
      case 32: {
        const uint32_t a = b.Pick(kRows), n = 1 + b.Pick(3), k = b.Pick(4);
        note((k == 0 ? "hide rows " : k == 1 ? "unhide rows " : k == 2 ? "hide cols " : "row height ") + std::to_string(a) + "+" + std::to_string(n));
        if (k < 2) d.HideRows(sh, a, a + n - 1, k == 0);
        else if (k == 2) d.HideCols(sh, a % kCols, a % kCols + n - 1, b.Pick(2) == 0);
        else d.SetRowHeight(sh, a, a + n - 1, 10 + static_cast<int32_t>(b.Pick(40)));
        break;
      }
      case 33: {
        const Rect r = PickRect(b);
        static const char* const kWhat[] = {"a", "1", "A1", "p", "e"};
        static const char* const kWith[] = {"b", "2", "=", "", "B2"};
        const uint32_t w = b.Pick(5), x = b.Pick(5), cs = b.Pick(2), whole = b.Pick(2);
        note("replace " + A1(r) + " " + kWhat[w] + " -> " + kWith[x] + (cs ? " case" : "") + (whole ? " whole" : ""));
        d.Replace(sh, r, kWhat[w], kWith[x], cs != 0, whole != 0);
        break;
      }
      case 34: {
        const Rect r = PickRect(b);
        const bool header = b.Pick(2) != 0;
        note("remove duplicates " + A1(r) + (header ? " header" : ""));
        d.RemoveDuplicates(sh, r, {r.c0}, header);
        break;
      }
      case 35: {
        const Rect r = PickRect(b);
        note("fill right " + A1(r));
        d.FillRight(sh, r);
        break;
      }
      case 36: {
        const Rect r = PickRect(b);
        const bool all = b.Pick(2) != 0;
        note((all ? "clear all " : "clear formats ") + A1(r));
        if (all) d.ClearAll(sh, r);
        else d.ClearFormats(sh, r);
        break;
      }
      case 37: {
        Rect r = PickRect(b);
        r.r1 += 2;
        const bool replace = b.Pick(2) != 0;
        note("subtotal " + A1(r) + (replace ? " replace" : ""));
        std::string err;
        d.Subtotal(sh, r, r.c0, 9, {r.c1}, replace, false, true, &err);
        break;
      }
      case 38: {
        const bool rows = b.Pick(2) != 0, group = b.Pick(3) != 0;
        const uint32_t a = b.Pick(kRows), n = b.Pick(3);
        note(std::string(group ? "group " : "ungroup ") + (rows ? "rows " : "cols ") + std::to_string(a) + "+" + std::to_string(n));
        d.GroupOutline(sh, rows, rows ? a : a % kCols, (rows ? a : a % kCols) + n, group);
        break;
      }
      case 39: {
        if (b.Pick(2)) {
          const uint32_t r = b.Pick(kRows), c = b.Pick(kCols);
          note("note " + yc::core::CellA1(r, c));
          d.SetNote(sh, r, c, "fuzz", b.Pick(3) ? "a note" : "");
        } else {
          const bool vis = b.Pick(2) != 0;
          note(vis ? "show sheet" : "hide sheet");
          d.SetSheetVisible(sh, vis);
        }
        break;
      }
      case 40: {
        note(d.wb().date1904 ? "1900 date system" : "1904 date system");
        d.SetDate1904(!d.wb().date1904);
        break;
      }
      case 41: {
        note(d.wb().precision_as_displayed ? "full precision" : "precision as displayed");
        d.SetPrecisionAsDisplayed(!d.wb().precision_as_displayed);
        break;
      }
      case 42: {
        note(d.wb().force_full_calc ? "no full calculation" : "full calculation every time");
        d.SetForceFullCalculation(!d.wb().force_full_calc);
        break;
      }
      case 43: {
        const uint32_t r = b.Pick(kRows), c = b.Pick(kCols);
        const char* t = kDates[b.Pick(sizeof(kDates) / sizeof(*kDates))];
        note("date " + yc::core::CellA1(r, c) + " " + t);
        d.SetInput(sh, r, c, t);
        break;
      }
      case 44: {
        const Rect r = PickRect(b);
        const uint32_t k = b.Pick(static_cast<uint32_t>(rounding.size()));
        note("format " + A1(r) + " " + kRoundingFormats[k]);
        const uint32_t nf = rounding[k];
        d.ModifyStyle(sh, r, [nf](model::Style& s) { s.numfmt = nf; });
        break;
      }
      case 45: {
        // Worksheet.EnableCalculation: a setting of the session, not saved and not undone (Excel 16)
        const bool on = !d.SheetCalculation(sh);
        note(on ? "calculation on" : "calculation off");
        d.SetSheetCalculation(sh, on);
        no_step_ = true;
        break;
      }
      case 46: {
        // a key stops the calculation an edit starts after a few cells; the next calculation goes on
        const uint32_t r = b.Pick(kRows), c = b.Pick(kCols), after = b.Pick(24);
        const char* f = kFormulas[b.Pick(sizeof(kFormulas) / sizeof(*kFormulas))];
        note("interrupted after " + std::to_string(after) + ": " + yc::core::CellA1(r, c) + " " + f);
        d.interrupt_after = after;
        d.SetInput(sh, r, c, f);
        interrupted_ = true;
        break;
      }
    }
    log.push_back(what.str());
    return true;
  }

  static std::string Diff(const std::string& a, const std::string& b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const size_t from = a.rfind('\n', i) == std::string::npos ? 0 : a.rfind('\n', i) + 1;
    auto line = [&](const std::string& s) {
      const size_t e = s.find('\n', from);
      return from < s.size() ? s.substr(from, e == std::string::npos ? std::string::npos : e - from) : std::string("(end)");
    };
    // the sheet the line is on: the last "sheet ..." line above it
    std::string sheet;
    for (size_t at = a.rfind("\nsheet ", from); at != std::string::npos;) {
      sheet = a.substr(at + 1, a.find('\n', at + 1) - at - 1);
      break;
    }
    if (sheet.empty() && a.compare(0, 6, "sheet ") == 0) sheet = a.substr(0, a.find('\n'));
    return "(" + sheet + ")\n    expected: " + line(a) + "\n    got:      " + line(b);
  }

  bool Fail(const std::string& why) {
    failure = why;
    return false;
  }

  // runs the whole stream; false with `failure` set on the first broken rule
  bool Run(Bytes& b) {
    size_t step = 0;
    while (!b.Done() && step < 400) {
      ++step;
      const std::string before = io::ContentDump(d.wb());
      const size_t depth = d.UndoDepth();
      d.Begin("fuzz");
      const double op_start = CpuSeconds();
      g_step_since = NowMs();
      try {
        Step(b);
      } catch (const std::exception& e) {
        g_step_since = 0;
        return Fail(std::string("threw: ") + e.what());
      }
      d.Commit();
      d.Recalc();
      for (int again = 0; again < 1000 && d.CalcInterrupted(); ++again) d.Recalc();
      d.interrupt_after = -1;
      g_step_since = 0;
      // a step a person waits on (the operation and its calculation, not this tool's checks after it): past 10 s of
      // CPU it is a freeze of the kind basics and scale_sweep find (a wide case ran for minutes, its steps seconds
      // each). CPU time, not the clock: rounds beside it made a step of 2 s take 14
      {
        const double took = CpuSeconds() - op_start;
        if (g_verbose && took > 2) std::printf("  (the step took %.1f s of CPU)\n", took);
        if (took > g_step_limit) {
          // a workbook of hundreds of thousands of formulas (a calculated column down a table as tall as the sheet,
          // each =COUNT(A:A)) is that many times a column's cells to calculate, in Excel too: the work, not a freeze
          // (seed 25000015, 211 s of CPU in 36 s). The watchdog still ends a step that never does
          uint64_t formulas = 0;
          for (const auto& sc : d.wb().sheets) sc->meta.ForEach([&](uint32_t, uint32_t, const model::MetaTile& t) { formulas += t.formula_count; });
          if (formulas < 200'000) return Fail("the step took " + std::to_string(static_cast<int>(took)) + " s of CPU");
          if (g_verbose) std::printf("  (%llu formulas: their work, not a freeze)\n", static_cast<unsigned long long>(formulas));
        }
      }
      const std::string after = io::ContentDump(d.wb());
      if (g_verbose) {
        std::printf("  %s\n", log.back().c_str());
        // the circular references the step leaves: a value check is skipped while there are any, so a cycle lost on
        // the way is a step whose checks ran over cells a cycle still kept uncalculated
        if (!d.circular_cells.empty()) {
          std::string cyc;
          for (const uint64_t k : d.circular_cells) cyc += " " + std::to_string(engine::KeySheet(k)) + ":" + yc::core::CellA1(engine::KeyRow(k), engine::KeyCol(k));
          std::printf("    circular:%s\n", cyc.c_str());
        }
        std::fflush(stdout);  // the step a stuck run is in, when it is stopped
      }
      // the checks after a step hold several dumps of the workbook and, every sixth step, a saved and reopened copy:
      // once whole-column steps have made it millions of cells (a table on the whole sheet and its calculated column)
      // that took 4 GB, where YCGrid itself held under one. The case ends there and the next one starts
      if (g_wide && after.size() > (64u << 20)) return true;
      // so is a workbook of hundreds of thousands of formulas: each step's Undo, Redo and reopen rebuild them all
      // (a case of a few million ran past the watchdog's 10 minutes, each step a dozen seconds; seed 27800104)
      if (g_wide) {
        uint64_t formulas = 0;
        for (const auto& sc : d.wb().sheets) sc->meta.ForEach([&](uint32_t, uint32_t, const model::MetaTile& t) { formulas += t.formula_count; });
        if (formulas > 300'000) return true;
      }
#ifdef _WIN32
      // the document doesn't grow without bound: two --wide rounds held 80 GB each, the machine ran out of memory and a
      // YCGrid beside them died of an uncaught bad_alloc. Past 8 GB the step is the finding, and its stream is kept
      {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) && pmc.PrivateUsage > (8ull << 30)) {
          return Fail("the process holds " + std::to_string(pmc.PrivateUsage >> 20) + " MB after the step");
        }
      }
#endif
      // two tables never share a cell, whatever made them (a table cut onto another one stayed a table over it, and
      // the two fought over a header's name: seed 20411091)
      for (const auto& sc : d.wb().sheets) {
        const auto& ts = *sc->tables;
        for (size_t i = 0; i < ts.size(); ++i) {
          for (size_t j = i + 1; j < ts.size(); ++j) {
            if (ts[i].range.Intersects(ts[j].range)) return Fail("two tables overlap on " + sc->name + ": " + ts[i].name + " and " + ts[j].name);
          }
        }
        // merged areas don't overlap, a table holds no merged area and no part of an array over several cells, and two
        // such arrays never share a cell (what TableCantHold, Merge and SetArrayFormula each refuse)
        const auto& ms = *sc->merges;
        {
          // sorted by their first row, each compared with those that start before it ends: a fill down a whole column
          // makes a million merged areas, and every pair was 5 x 10^11 checks (this tool's own step that never ended)
          std::vector<Rect> sorted(ms.begin(), ms.end());
          std::sort(sorted.begin(), sorted.end(), [](const Rect& a, const Rect& b) { return std::tie(a.r0, a.c0) < std::tie(b.r0, b.c0); });
          for (size_t i = 0; i < sorted.size(); ++i) {
            for (size_t j = i + 1; j < sorted.size() && sorted[j].r0 <= sorted[i].r1; ++j) {
              if (sorted[i].Intersects(sorted[j])) return Fail("two merged areas overlap on " + sc->name + ": " + yc::core::RectA1(sorted[i]) + " and " + yc::core::RectA1(sorted[j]));
            }
          }
        }
        for (size_t i = 0; i < ms.size(); ++i) {
          for (const auto& t : ts) {
            if (t.range.Intersects(ms[i])) return Fail("table " + t.name + " holds merged cells " + yc::core::RectA1(ms[i]) + " on " + sc->name);
          }
        }
        std::vector<Rect> arrays;
        for (const auto& [k, sp] : *sc->spills) {
          if (sp.cse && !sp.rect.IsCell()) arrays.push_back(sp.rect);
          // a spill's record is its anchor's: the anchor holds the formula that spills
          if (!sc->GetFormula(sp.rect.r0, sp.rect.c0)) return Fail("a spill record without its formula at " + yc::core::RectA1(sp.rect) + " on " + sc->name);
        }
        // what the sheet keeps lies on the sheet, and a table names every column it has
        for (const auto& t : ts) {
          if (t.range.r1 >= sc->row_limit || t.range.c1 >= sc->col_limit) return Fail("table " + t.name + " runs past the sheet: " + yc::core::RectA1(t.range));
          if (t.columns.size() != t.range.Cols()) return Fail("table " + t.name + " has " + std::to_string(t.columns.size()) + " columns named for " + std::to_string(t.range.Cols()));
        }
        for (const Rect& m : ms) {
          if (m.r1 >= sc->row_limit || m.c1 >= sc->col_limit) return Fail("merged area past the sheet: " + yc::core::RectA1(m) + " on " + sc->name);
        }
        for (size_t i = 0; i < arrays.size(); ++i) {
          for (const auto& t : ts) {
            if (t.range.Intersects(arrays[i])) return Fail("table " + t.name + " holds array " + yc::core::RectA1(arrays[i]) + " on " + sc->name);
          }
          for (size_t j = i + 1; j < arrays.size(); ++j) {
            if (arrays[i].Intersects(arrays[j])) return Fail("two arrays overlap on " + sc->name + ": " + yc::core::RectA1(arrays[i]) + " and " + yc::core::RectA1(arrays[j]));
          }
        }
      }
      // a circular reference without iteration isn't calculated: its cells keep the values they show (Excel), so what
      // they show depends on the order things happened in, and Undo, Redo or a reopen may show other values for the
      // same formulas. Values are compared while the workbook has none; Undo / Redo must still work.
      const bool cyclic = !d.circular_cells.empty() || cyclic_before_;
      cyclic_before_ = !d.circular_cells.empty();
      // a sheet whose calculation is off keeps values a reopen calculates again (YCGrid calculates every formula of a
      // file it opens): its values are compared while every sheet calculates
      bool calc_off = false;
      for (const auto& sc : d.wb().sheets) calc_off = calc_off || !d.SheetCalculation(sc->id);
      const bool setting_only = no_step_;
      const bool force_save_check = interrupted_;
      no_step_ = interrupted_ = false;
      if (d.UndoDepth() > depth) {
        if (!d.Undo()) return Fail("Undo refused the step just made");
        d.Recalc();
        const std::string undone = io::ContentDump(d.wb());
        if (!cyclic && undone != before) {
          if (g_verbose) std::printf("--- before the step:\n%s--- after Undo:\n%s---\n", before.c_str(), undone.c_str());
          return Fail("Undo didn't give back what was there:\n    " + Diff(before, undone));
        }
        if (!d.Redo()) return Fail("Redo refused the step just undone");
        d.Recalc();
        const std::string redone = io::ContentDump(d.wb());
        if (!cyclic && redone != after) {
          if (g_verbose) std::printf("--- after the step:\n%s--- after Redo:\n%s---\n", after.c_str(), redone.c_str());
          return Fail("Redo didn't give back what the step made:\n    " + Diff(after, redone));
        }
      } else if (!cyclic && !setting_only && after != before) {
        return Fail("the workbook changed without an Undo step:\n    " + Diff(before, after));
      }
      if ((g_every_step || force_save_check || step % 6 == 0) && !cyclic && !calc_off) {
        const std::string bytes = io::SaveYcgToBytes(d.wb(), io::SaveOptions{});
        io::LoadResult lr = io::LoadYcgFromBytes(bytes);
        if (!lr.ok) return Fail("the saved workbook doesn't open: " + lr.error);
        engine::Document fresh(std::move(lr.wb));  // a full recalculation of what was saved
        const std::string reopened = io::ContentDump(fresh.wb());
        if (reopened != after) {
          if (g_verbose) {
            std::printf("--- before the save:\n%s--- after the open:\n%s---\n", after.c_str(), reopened.c_str());
            // the cells as text, to see which formula or value differs (the dump above holds hashes)
            std::printf("--- cells before the save:\n%s--- cells after the open:\n%s---\n", CellsDump(d.wb()).c_str(), CellsDump(fresh.wb()).c_str());
            // each formula cell's formula id and key: two cells of one text with two keys are two formulas, which a
            // save and an open makes one (the FMLA chunk differs while every cell reads the same)
            for (const auto& sp : d.wb().sheets) {
              sp->meta.ForEach([&](uint32_t rt, uint32_t ct, const model::MetaTile& t) {
                for (uint32_t i = 0; i < model::kTileCells; ++i) {
                  if (!t.formula[i]) continue;
                  const uint32_t r = rt * model::kTileRows + i % model::kTileRows, c = ct * model::kTileCols + i / model::kTileRows;
                  const auto* f = d.Formula(t.formula[i]);
                  std::printf("FORMULA %s r%u c%u id %u key %s text %s\n", sp->name.c_str(), r, c, t.formula[i], f ? f->key.c_str() : "?",
                              d.GetInputText(sp->id, r, c).c_str());
                }
              });
            }          }
          return Fail("saved and opened (and calculated again), the workbook differs:\n    " + Diff(after, reopened));
        }
        // what a copy leaves out that the cells depend on (structured references, a report's cells, links); rules,
        // notes, charts and the like don't change a cell's formula or value
        const io::XlsxSkipped sk = g_xlsx ? io::XlsxExportSkipped(d.wb()) : io::XlsxSkipped{};
        const bool xlsx_now = g_xlsx && sk.tables == 0 && sk.pivot_tables == 0 && sk.external_links == 0 && sk.queries == 0;
        if (g_xlsx) ++(xlsx_now ? g_xlsx_checked : g_xlsx_skipped);
        if (xlsx_now) {
          const std::string xbytes = io::ExportXlsxBytes(d.wb());
          model::Workbook xwb;
          std::string xerr;
          std::vector<std::string> warn;
          if (!io::ImportXlsxBytes(xbytes, &xwb, &xerr, &warn)) return Fail("the Excel copy doesn't open: " + xerr);
          engine::Document xd(std::move(xwb));
          const std::string had = CellsDump(d.wb()), got = CellsDump(xd.wb());
          if (had != got) {
            if (g_verbose) {
              std::printf("--- before the Excel copy:\n%s--- the copy opened:\n%s---\n", had.c_str(), got.c_str());
              std::ofstream("doc_fuzz_copy.xlsx", std::ios::binary).write(xbytes.data(), static_cast<std::streamsize>(xbytes.size()));
              std::printf("(the copy: doc_fuzz_copy.xlsx)\n");
            }
            return Fail("saved as an Excel copy and opened, the cells differ:\n    " + Diff(had, got));
          }
          // a damaged copy opens as an error or as a workbook, never as a crash, a hang or all the memory: a part
          // cut short, a character changed, a huge number or a piece repeated (the changes are drawn from the
          // workbook's content, not from the stream, so a stream reads the same with or without --xlsx)
          std::vector<io::ZipEntry> parts;
          std::string zerr;
          if (io::ReadZip(xbytes, &parts, &zerr) && !parts.empty()) {
            uint64_t h = std::hash<std::string>{}(had) | 1;
            auto next = [&] { return h = h * 6364136223846793005ull + 1442695040888963407ull, h >> 33; };
            for (int k = 0; k < 3; ++k) {
              std::vector<io::ZipEntry> bad = parts;
              io::ZipEntry& e = bad[next() % bad.size()];
              if (e.data.empty()) continue;
              const size_t at = next() % e.data.size();
              const uint64_t how = next() % 4;
              static const char kChars[] = "<>\"=/&;0123456789-:.AZaz ";
              if (how == 0) e.data.resize(at);
              else if (how == 1) e.data[at] = kChars[next() % (sizeof(kChars) - 1)];
              else if (how == 2) e.data.insert(at, "99999999999");
              else e.data.insert(at, e.data.substr(at, std::min<size_t>(e.data.size() - at, 1 + next() % 200)));
              const auto t0 = std::chrono::steady_clock::now();
              try {
                model::Workbook mwb;
                std::string merr;
                std::vector<std::string> mwarn;
                if (io::ImportXlsxBytes(io::WriteZip(bad), &mwb, &merr, &mwarn)) engine::Document md(std::move(mwb));
              } catch (const std::bad_alloc&) {
                return Fail("a damaged Excel copy (" + e.name + ", change " + std::to_string(how) + " at " + std::to_string(at) +
                            ") ran out of memory");
              } catch (const std::exception&) {
                // an error message for the person (OpenWorkbook catches it)
              }
              if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
                return Fail("a damaged Excel copy (" + e.name + ", change " + std::to_string(how) + " at " + std::to_string(at) +
                            ") took more than 5 s to open");
              }
            }
          }
        }
      }
    }
    return true;
  }
};

bool RunOne(const uint8_t* data, size_t size, std::string* why, std::vector<std::string>* log) {
  Fuzz f;
  Bytes b{data, size};
  const bool ok = f.Run(b);
  if (!ok) {
    *why = f.failure;
    *log = f.log;
  }
  return ok;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string why;
  std::vector<std::string> log;
  if (!RunOne(data, size, &why, &log)) {
    std::fprintf(stderr, "%s\n", why.c_str());
    std::abort();
  }
  return 0;
}

#ifndef YC_LIBFUZZER
int main(int argc, char** argv) {
  double seconds = 30;
  uint64_t seed = 1, cases = 0;
  std::string out = ".", replay, corpus;
  bool minimize = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--seconds") seconds = std::atof(next().c_str());
    else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--cases") cases = std::strtoull(next().c_str(), nullptr, 10);
    else if (a == "--out") out = next();
    else if (a == "--replay") replay = next();
    else if (a == "--verbose") g_verbose = true;
    else if (a == "--every-step") g_every_step = true;
    else if (a == "--wide") g_wide = true;
    else if (a == "--xlsx") g_xlsx = true;
    else if (a == "--more") g_more = true;
    else if (a == "--gaps") g_gaps = true;
    else if (a == "--keep-case") g_keep_case = true;
    else if (a == "--step-limit") g_step_limit = std::atof(next().c_str());
    else if (a == "--minimize") minimize = true;
    else if (a == "--corpus") corpus = next();
    else {
      std::fprintf(stderr, "usage: yc_doc_fuzz [--seconds N] [--seed S] [--cases N] [--out DIR] [--replay FILE [--minimize]] [--corpus DIR] [--verbose] [--every-step] [--wide] [--xlsx] [--more] [--gaps]\n");
      return 2;
    }
  }
  auto report = [&](const std::string& why, const std::vector<std::string>& log) {
    std::printf("FAILED: %s\n  after %zu operations:\n", why.c_str(), log.size());
    for (const auto& l : log) std::printf("    %s\n", l.c_str());
  };
  if (g_step_limit <= 0) g_step_limit = g_wide ? 120 : 10;
  if (!corpus.empty()) {
    // every stream that once failed (tests/fuzz/corpus): each must pass now, so a fixed problem stays fixed
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator(corpus)) {
      if (entry.path().extension() != ".bin") continue;
      std::ifstream f(entry.path(), std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      std::string why;
      std::vector<std::string> log;
      ++n;
      if (!RunOne(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &why, &log)) {
        std::printf("%s ", entry.path().string().c_str());
        report(why, log);
        return 1;
      }
    }
    std::printf("doc_fuzz corpus %s: %zu streams, no problem\n", corpus.c_str(), n);
    if (!cases && seconds <= 0) return 0;
  }
  if (!replay.empty()) {
    std::ifstream f(replay, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string why;
    std::vector<std::string> log;
    if (!RunOne(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &why, &log)) {
      if (minimize) {
        // drop chunks of bytes, halving the chunk, while the stream still fails with the same first line
        const std::string kind = why.substr(0, why.find('\n'));
        std::vector<uint8_t> best(bytes.begin(), bytes.end());
        for (size_t chunk = best.size() / 2; chunk >= 1; chunk /= 2) {
          for (size_t at = 0; at + chunk <= best.size();) {
            std::vector<uint8_t> trial(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(at));
            trial.insert(trial.end(), best.begin() + static_cast<std::ptrdiff_t>(at + chunk), best.end());
            std::string w;
            std::vector<std::string> l;
            if (!trial.empty() && !RunOne(trial.data(), trial.size(), &w, &l) && w.substr(0, w.find('\n')) == kind) best = std::move(trial);
            else at += chunk;
          }
          if (chunk == 1) break;
        }
        RunOne(best.data(), best.size(), &why, &log);
        const std::string path = replay + ".min";
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(best.data()), static_cast<std::streamsize>(best.size()));
        std::printf("minimized to %zu bytes: %s\n", best.size(), path.c_str());
      }
      report(why, log);
      return 1;
    }
    std::printf("replay %s: no problem\n", replay.c_str());
    return 0;
  }
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t n = 0;
  g_out_dir = out;
  StartWatchdog();
  for (uint64_t s = seed;; ++s) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (cases ? n >= cases : elapsed >= seconds) break;
    std::mt19937_64 rng(s);
    std::vector<uint8_t> bytes(64 + rng() % 448);
    for (auto& x : bytes) x = static_cast<uint8_t>(rng());
    g_case_bytes = &bytes;
    g_case_seed = s;
    g_case_since = NowMs();
    if (g_keep_case) {
      std::ofstream(g_out_dir + "/doc_fuzz_current_" + std::to_string(seed) + ".bin", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (g_verbose) {  // a case that ends the process (out of memory past a job's limit) is found by its seed
      std::printf("case seed %llu\n", static_cast<unsigned long long>(s));
      std::fflush(stdout);
    }
    std::string why;
    std::vector<std::string> log;
    ++n;
    if (!RunOne(bytes.data(), bytes.size(), &why, &log)) {
      const std::string path = out + "/doc_fuzz_" + std::to_string(s) + ".bin";
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      std::printf("seed %llu ", static_cast<unsigned long long>(s));
      report(why, log);
      std::printf("  replay: yc_doc_fuzz%s%s%s%s --replay %s\n", g_wide ? " --wide" : "", g_xlsx ? " --xlsx" : "", g_more ? " --more" : "", g_gaps ? " --gaps" : "", path.c_str());
      return 1;
    }
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("doc_fuzz: %llu cases from seed %llu in %.1f s, no problem\n", static_cast<unsigned long long>(n),
              static_cast<unsigned long long>(seed), elapsed);
  if (g_xlsx) std::printf("  Excel copies compared: %llu, left out (the workbook held what a copy doesn't keep): %llu\n",
                          static_cast<unsigned long long>(g_xlsx_checked), static_cast<unsigned long long>(g_xlsx_skipped));
  return 0;
}
#endif
