// SPDX-License-Identifier: Apache-2.0
// Engine pieces the UI leans on that had no unit test: Ctrl+arrow jumps and the status bar's sums, formatting whole
// rows and columns, unmerging, hiding and AutoFit of columns, CSV export, msgpack extension values, conditional
// formats that read names, other sheets, tables and hidden rows, the error-checking texts, and the ToolPak's charts.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.h"

#include "core/i18n.h"
#include "engine/error_check.h"
#include "engine/navigate.h"
#include "engine/rule_eval.h"
#include "engine/snapshot.h"
#include "io/csv.h"
#include "io/msgpack.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using yc::core::Rect;
namespace engine = yc::engine;
namespace model = yc::model;
namespace io = yc::io;

namespace {
Rect R(const char* a1) {
  Rect r;
  yc::core::ParseRectA1(a1, &r);
  return r;
}
const model::Style& StyleAt(yctest::Doc& t, uint32_t r, uint32_t c) { return t.d.wb().styles->Get(t.d.S(t.sheet())->GetStyle(r, c)); }
}  // namespace

TEST_CASE("Ctrl+arrow jumps, the last cell and the status bar's numbers") {
  yctest::Doc t;
  // column A: a block A2:A4, then a lone A10; row 1: B1:D1
  for (const char* a : {"A2", "A3", "A4", "A10", "B1", "C1", "D1"}) t.Set(a, "1");
  t.Set("A3", "text");
  const auto& s = *t.d.S(t.sheet());
  CHECK(engine::CtrlJump(s, 0, 0, true, 1) == 1);      // empty A1 -> the next value, A2
  CHECK(engine::CtrlJump(s, 1, 0, true, 1) == 3);      // inside the block -> its end, A4
  CHECK(engine::CtrlJump(s, 3, 0, true, 1) == 9);      // the block's end -> the next value, A10
  CHECK(engine::CtrlJump(s, 9, 0, true, 1) == s.row_limit - 1);  // nothing below -> the last row
  CHECK(engine::CtrlJump(s, 9, 0, true, -1) == 3);     // up from A10 -> A4
  CHECK(engine::CtrlJump(s, 1, 0, true, -1) == 0);     // up from A2 (nothing above) -> A1
  CHECK(engine::CtrlJump(s, 0, 0, true, -1) == 0);     // already at the edge
  CHECK(engine::CtrlJump(s, 0, 1, false, 1) == 3);     // B1 -> D1 along the row
  CHECK(engine::CtrlJump(s, 0, 3, false, 1) == s.col_limit - 1);
  CHECK(engine::CtrlJump(s, 500000, 0, true, -1) == 9);  // far below, across empty tiles, back to A10
  const auto last = engine::LastCell(s);
  CHECK(last.row == 9);
  CHECK(last.col == 3);
  CHECK(engine::LastCell(*t.d.S(t.d.AddSheet("Empty"))).row == 0);
  t.Set("E5", "2.5");
  t.Set("E6", "-1");
  const auto st = engine::ComputeStats(*t.d.S(t.sheet()), R("A1:E1048576"));
  CHECK(st.count == 9);    // every cell with a value, the text too
  CHECK(st.numbers == 8);
  CHECK(st.sum == 7.5);
  CHECK(st.min == -1);
  CHECK(st.max == 2.5);
  const auto none = engine::ComputeStats(*t.d.S(t.sheet()), R("Z1:Z9"));
  CHECK(none.count == 0);
  CHECK(none.min == 0);
  const auto text = engine::ComputeStats(*t.d.S(t.sheet()), R("A3:A3"));
  CHECK(text.count == 1);
  CHECK(text.numbers == 0);
  CHECK(text.max == 0);
}

TEST_CASE("Formatting whole rows and columns, unmerging, hiding and AutoFit of columns") {
  yctest::Doc t;
  t.Set("B2", "x");
  t.d.ModifyRowStyle(t.sheet(), 1, 2, [](model::Style& s) { s.bold = true; });
  CHECK(StyleAt(t, 1, 1).bold);       // B2, a cell with a value
  CHECK(StyleAt(t, 2, 500).bold);     // an empty cell far out in row 3
  CHECK_FALSE(StyleAt(t, 3, 1).bold);
  t.d.Begin("Italic");
  t.d.ModifyColStyle(t.sheet(), 4, 4, [](model::Style& s) { s.italic = true; });
  t.d.Commit();
  CHECK(StyleAt(t, 0, 4).italic);
  CHECK(StyleAt(t, 900000, 4).italic);
  CHECK(StyleAt(t, 1, 4).italic);
  CHECK(StyleAt(t, 1, 4).bold);  // where the bold rows cross the italic column, both
  CHECK_FALSE(StyleAt(t, 0, 5).italic);
  REQUIRE(t.d.Undo());  // one step undoes the column format
  CHECK_FALSE(StyleAt(t, 0, 4).italic);
  // a merged area goes back to cells; only its own merge, not one beside it
  t.Set("A10", "kept");
  t.d.Merge(t.sheet(), R("A10:C11"), false);
  t.d.Merge(t.sheet(), R("E10:F10"), false);
  REQUIRE(t.d.S(t.sheet())->merges->size() == 2);
  t.d.Unmerge(t.sheet(), R("B10:B10"));  // any cell of the area unmerges it
  REQUIRE(t.d.S(t.sheet())->merges->size() == 1);
  CHECK((*t.d.S(t.sheet())->merges)[0] == R("E10:F10"));
  CHECK(t.T("A10") == "kept");
  // columns hidden and shown again keep their width
  const int32_t w = t.d.S(t.sheet())->cols->Size(2);
  t.d.HideCols(t.sheet(), 2, 3, true);
  CHECK(t.d.S(t.sheet())->cols->IsHidden(2));
  CHECK(t.d.S(t.sheet())->cols->IsHidden(3));
  t.d.HideCols(t.sheet(), 2, 3, false);
  CHECK(t.d.S(t.sheet())->cols->Size(2) == w);
  // AutoFit measures each cell's shown text with the measure it's given
  t.Set("H1", "a");
  t.Set("H2", "a much longer text");
  t.Set("I1", "1");
  t.d.AutoFitCols(t.sheet(), 7, 9, [](const std::string& s, const model::Style&) { return static_cast<int32_t>(s.size() * 7); });
  const int32_t h = t.d.S(t.sheet())->cols->Size(7);
  CHECK(h >= 18 * 7);
  CHECK(t.d.S(t.sheet())->cols->Size(8) < h);
  CHECK(t.d.S(t.sheet())->cols->Size(9) == t.d.S(t.sheet())->cols->default_size());  // J is empty: unchanged
}

TEST_CASE("CSV export writes the shown text, quotes what needs it, and reads back") {
  yctest::Doc t;
  t.Set("A1", "Name");
  t.Set("B1", "Px");
  t.Set("A2", "a,b");
  t.Set("B2", "1234.5");
  t.Set("A3", "say \"hi\"");
  t.Set("B3", "=B2*2");
  t.Set("A4", "\xE5\x8F\xB0\xE7\xA9\x8D\xE9\x9B\xBB");
  const uint32_t nf = t.d.wb_mut().styles.mut().InternString("#,##0.00");
  t.d.ModifyStyle(t.sheet(), R("B2:B3"), [nf](model::Style& s) { s.numfmt = nf; });
  engine::Snapshot snap;
  snap.wb = t.d.wb();
  const std::wstring path = (std::filesystem::temp_directory_path() / L"yc_export_test.csv").wstring();
  std::string err;
  REQUIRE(io::ExportCsv(snap, t.sheet(), path, &err));
  std::string text;
  {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }
  CHECK(text.rfind("\xEF\xBB\xBF", 0) == 0);  // UTF-8 with a BOM: Excel opens it without garbling
  CHECK(text.find("\"a,b\"") != std::string::npos);
  CHECK(text.find("\"say \"\"hi\"\"\"") != std::string::npos);
  CHECK(text.find("\"1,234.50\"") != std::string::npos);  // the shown text, quoted for its comma
  CHECK(text.find("\"2,469.00\"") != std::string::npos);  // a formula's value
  model::Workbook back;
  REQUIRE(io::ImportCsv(path, &back, &err));
  CHECK(back.sheets[0]->GetValue(1, 0).str() == "a,b");
  CHECK(back.sheets[0]->GetValue(2, 0).str() == "say \"hi\"");
  CHECK(back.sheets[0]->GetValue(3, 0).str() == "\xE5\x8F\xB0\xE7\xA9\x8D\xE9\x9B\xBB");
  std::string semi_err;
  REQUIRE(io::ExportCsv(snap, t.sheet(), path, &semi_err, ';'));
  {
    std::ifstream g(std::filesystem::path(path), std::ios::binary);
    std::stringstream gs;
    gs << g.rdbuf();
    CHECK(gs.str().find("Name;Px") != std::string::npos);
  }
  std::filesystem::remove(path);
  CHECK_FALSE(io::ExportCsv(snap, 999999, path, &err));  // no such sheet
}

TEST_CASE("msgpack extension values: every size round-trips and is skipped") {
  for (size_t n : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, size_t{3}, size_t{200}, size_t{70000}}) {
    io::MsgWriter w;
    const std::string data(n, 'x');
    w.Array(3);
    w.Ext(5, data);
    w.Int(-7);
    w.Ext(-1, "");
    io::MsgReader r(w.buf());
    uint32_t len = 0;
    REQUIRE(r.Array(&len));
    REQUIRE(len == 3);
    CHECK(r.Peek() == io::MsgType::Ext);
    int8_t type = 0;
    std::string_view got;
    REQUIRE(r.Ext(&type, &got));
    CHECK(type == 5);
    CHECK(got == data);
    int64_t v = 0;
    REQUIRE(r.Int(&v));
    CHECK(v == -7);
    REQUIRE(r.Ext(&type, &got));
    CHECK(type == -1);
    CHECK(got.empty());
    io::MsgReader skip(w.buf());
    CHECK(skip.Skip());  // the whole array, extensions inside
    CHECK(skip.AtEnd());
  }
  const std::string cut = std::string("\xc7\x05\x01", 3) + "ab";  // ext 8 claiming 5 bytes, 2 present
  io::MsgReader bad(cut);
  int8_t type = 0;
  std::string_view got;
  CHECK_FALSE(bad.Ext(&type, &got));
  io::MsgReader bad2(cut);
  CHECK_FALSE(bad2.Skip());
}

TEST_CASE("Conditional formats read names, other sheets, tables, hidden rows and whole columns") {
  yctest::Doc t;
  const uint32_t s2 = t.d.AddSheet("Limits");
  t.d.SetInput(s2, 0, 0, "15");
  std::string err;
  REQUIRE(t.d.DefineName(model::NameDef{"Limit", "=Limits!$A$1", -1}, &err));
  for (int i = 1; i <= 4; ++i) t.Set(("A" + std::to_string(i)).c_str(), std::to_string(i * 10));
  t.Set("C1", "Sym");
  t.Set("C2", "20");
  t.Set("C3", "40");
  REQUIRE(t.d.AddTable(t.sheet(), R("C1:C3"), true, "TableStyleMedium2", &err));
  const std::string table = (*t.d.S(t.sheet())->tables)[0].name;
  auto rule = [&](const std::string& f, int italic) {
    model::CfRule r;
    r.type = model::CfType::Expression;
    r.formula1 = f;
    r.ranges = {R("A1:A4")};
    if (italic) r.fmt.italic = 1;
    else r.fmt.bold = 1;
    t.d.AddCondFormat(t.sheet(), r);
  };
  rule("A1>Limit", 1);                                  // a name pointing at another sheet
  rule("COUNTIF(" + table + "[Sym],A1)>0", 0);           // a table column
  t.d.Recalc();
  engine::CondFormatEval ev(t.d.wb(), t.sheet());
  engine::CfVisual v;
  ev.Eval(0, 0, &(v = {}));  // 10: under 15, not in the table
  CHECK(v.italic == -1);
  CHECK(v.bold == -1);
  ev.Eval(1, 0, &(v = {}));  // 20: over 15, in the table
  CHECK(v.italic == 1);
  CHECK(v.bold == 1);
  ev.Eval(2, 0, &(v = {}));  // 30: over 15, not in the table
  CHECK(v.italic == 1);
  CHECK(v.bold == -1);
  // a rule over another sheet's whole column and SUBTOTAL, which reads hidden rows
  yctest::Doc u;
  for (int i = 1; i <= 3; ++i) u.Set(("A" + std::to_string(i)).c_str(), std::to_string(i));
  const uint32_t other = u.d.AddSheet("Other");
  u.d.SetInput(other, 0, 0, "5");
  model::CfRule big;
  big.type = model::CfType::Expression;
  big.formula1 = "SUM(Other!A:A)>4";
  big.fmt.bold = 1;
  big.ranges = {R("B1:B1")};
  u.d.AddCondFormat(u.sheet(), big);
  model::CfRule vis;
  vis.type = model::CfType::Expression;
  vis.formula1 = "SUBTOTAL(109,$A$1:$A$3)=6";
  vis.fmt.italic = 1;
  vis.ranges = {R("B1:B1")};
  u.d.AddCondFormat(u.sheet(), vis);
  u.d.Recalc();
  {
    engine::CondFormatEval e2(u.d.wb(), u.sheet());
    e2.Eval(0, 1, &(v = {}));
    CHECK(v.bold == 1);
    CHECK(v.italic == 1);
  }
  u.d.HideRows(u.sheet(), 1, 1, true);
  {
    engine::CondFormatEval e3(u.d.wb(), u.sheet());
    e3.Eval(0, 1, &(v = {}));
    CHECK(v.italic == -1);  // the hidden 2 is left out: 4
  }
  // a validation list from a name
  model::DvRule list;
  list.type = model::DvType::List;
  list.formula1 = "=Choices";
  t.Set("E1", "buy");
  t.Set("E2", "sell");
  REQUIRE(t.d.DefineName(model::NameDef{"Choices", "=$E$1:$E$2", -1}, &err));
  t.d.SetValidation(t.sheet(), R("F1:F3"), list);
  const model::DvRule* found = engine::FindValidation(*t.d.S(t.sheet()), 0, 5);
  REQUIRE(found);
  CHECK(engine::ValidationList(t.d.wb(), t.sheet(), *found, 0, 5) == std::vector<std::string>{"buy", "sell"});
}

TEST_CASE("Error checking: every rule has a title and help, every format kind a name") {
  using namespace engine;
  const auto lang = yc::i18n::Current();
  yc::i18n::SetCurrent(yc::i18n::Lang::En);
  ErrorCheckOptions o;
  for (int rule = kErrEvaluateToError; rule < kErrRuleCount; ++rule) {
    const char* title = ErrorRuleTitle(rule, yc::core::Value::Error(yc::core::Err::Div0));
    const char* help = ErrorRuleHelp(rule, yc::core::Value::Error(yc::core::Err::Div0));
    INFO("rule " << rule);
    REQUIRE(title);
    REQUIRE(help);
    CHECK(std::string(title).size() > 3);
    CHECK(std::string(help).size() > 3);
  }
  // the error-value rule names the error, as Excel does
  CHECK(std::string(ErrorRuleTitle(kErrEvaluateToError, yc::core::Value::Error(yc::core::Err::Div0))).find("Divide") != std::string::npos);
  CHECK(std::string(ErrorRuleTitle(kErrEvaluateToError, yc::core::Value::Error(yc::core::Err::NA))) !=
        std::string(ErrorRuleTitle(kErrEvaluateToError, yc::core::Value::Error(yc::core::Err::Div0))));
  CHECK(std::string(ErrorRuleTitle(kErrNumberAsText, yc::core::Value())).find("Text") != std::string::npos);
  for (int k = kFmtGeneral; k <= kFmtText; ++k) {
    INFO("kind " << k);
    REQUIRE(FormatKindName(k));
    CHECK(std::string(FormatKindName(k)).size() > 2);
  }
  CHECK(std::string(FormatKindName(kFmtCurrency)) == "Currency");
  CHECK(FormatKindOf("$#,##0.00") == kFmtCurrency);
  CHECK(FormatKindOf("0.00%") == kFmtPercent);
  CHECK(FormatKindOf("yyyy-mm-dd") == kFmtDate);
  CHECK(FormatKindOf("@") == kFmtText);
  // On() follows each switch of File > Options > Formulas
  for (int rule = kErrEvaluateToError; rule < kErrRuleCount; ++rule) CHECK(o.On(rule) == (rule != kErrEmptyCellReference));
  o.evaluate_to_error = false;
  CHECK_FALSE(o.On(kErrEvaluateToError));
  CHECK_FALSE(o.On(99));
  yc::i18n::SetCurrent(lang);
}

TEST_CASE("analysis: the Histogram's and Regression's charts") {
  using yc::engine::analysis::Tool;
  yctest::Doc t;
  t.Set("A1", "Y");
  t.Set("B1", "X");
  for (int i = 0; i < 8; ++i) {
    t.Set(("A" + std::to_string(i + 2)).c_str(), std::to_string(3 + 2 * i + (i % 3)));
    t.Set(("B" + std::to_string(i + 2)).c_str(), std::to_string(i + 1));
  }
  auto run = [&](engine::Document::AnalysisRequest req, const char* out) {
    req.in_sheet = req.in2_sheet = req.bins_sheet = req.out_sheet = t.sheet();
    const auto o = yctest::Doc::P(out);
    req.out_r = o.row;
    req.out_c = o.col;
    uint32_t sh = 0;
    Rect rect{};
    std::string err;
    return t.d.RunAnalysis(req, &sh, &rect, &err);
  };
  engine::Document::AnalysisRequest h;
  h.p.tool = Tool::Histogram;
  h.in = R("A2:A9");
  h.has_in = true;
  h.p.cumulative = true;
  h.chart_output = true;
  REQUIRE(run(h, "D1"));
  const auto& charts = *t.d.S(t.sheet())->charts;
  REQUIRE(charts.size() == 1);
  CHECK(charts[0].title == "Histogram");
  CHECK(charts[0].series.size() == 2);  // the frequencies and the cumulative %
  engine::Document::AnalysisRequest reg;
  reg.p.tool = Tool::Regression;
  reg.in = R("A1:A9");
  reg.in2 = R("B1:B9");
  reg.has_in = reg.has_in2 = true;
  reg.p.labels = true;
  reg.p.residuals = true;
  reg.p.normal_probability = true;
  reg.residual_plots = reg.line_fit_plots = reg.normal_plots = true;
  REQUIRE(run(reg, "H1"));
  const auto& all = *t.d.S(t.sheet())->charts;
  CHECK(all.size() == 4);  // + a residual plot and a line fit plot for X, and the normal probability plot
  bool residual = false, fit = false, normal = false;
  for (const auto& c : all) {
    residual = residual || c.title.find("Residual") != std::string::npos;
    fit = fit || c.title.find("Line Fit") != std::string::npos;
    normal = normal || c.title.find("Normal Probability") != std::string::npos;
    CHECK_FALSE(c.series.empty());
  }
  CHECK(residual);
  CHECK(fit);
  CHECK(normal);
  REQUIRE(t.d.Undo());  // the regression and its charts are one step
  CHECK(t.d.S(t.sheet())->charts->size() == 1);
}

TEST_CASE("The status bar's statistics leave out hidden rows and columns, as Excel's do") {
  yctest::Doc t;
  t.Set("A1", "1");
  t.Set("A2", "2");
  t.Set("A3", "4");
  t.Set("B1", "100");
  t.d.HideRows(t.sheet(), 1, 1, true);
  const Rect r{0, 0, 2, 1};
  const auto all = engine::ComputeStats(*t.d.S(t.sheet()), r);
  CHECK(all.sum == 107);
  const auto shown = engine::ComputeStats(*t.d.S(t.sheet()), r, true);
  CHECK(shown.sum == 105);  // A2 is hidden (it counted)
  CHECK(shown.count == 3);
  CHECK(shown.numbers == 3);
}
