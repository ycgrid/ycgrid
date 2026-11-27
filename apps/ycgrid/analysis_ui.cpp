// SPDX-License-Identifier: Apache-2.0
// analysis_ui.cpp - Data > Data Analysis: Excel's Analysis ToolPak dialogs (engine/analysis.cpp)
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "grid/grid.h"
#include "icons/yc_icons.h"
#include "io/xlsx.h"

#include "imgui.h"
#include "yc_ui_items.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace yc::app {

using engine::Document;
using engine::analysis::Tool;
using ui::S;

namespace {

struct AnalysisUi {
  int sel = 5;  // Descriptive Statistics
  Tool tool = Tool::Descriptive;
  char in[256] = "", in2[256] = "", bins[256] = "", out[128] = "", sheet_name[64] = "";
  int out_kind = 1;  // 0 output range, 1 new worksheet ply, 2 new workbook
  bool chart_output = false, residual_plots = false, line_fit_plots = false;
  bool by_rows = false, labels = false;
  float alpha = 0.05f;
  // tool options
  bool summary = true, conf_on = true, k1_on = false, k2_on = false;
  float conf = 95;
  int k1 = 1, k2 = 1;
  int rows_per_sample = 2;
  float damping = 0.3f;
  int interval = 3;
  bool std_errors = false;
  float diff = 0, var1 = 0, var2 = 0;
  bool inverse = false, pareto = false, cumulative = false;
  int variables = 1, points = 10, distribution = 0;
  float p[5] = {0, 1, 1, 1, 1};
  int seed = 0;
  bool zero_constant = false, confidence_on = false, residuals = false, std_residuals = false, normal_prob = false;
  float confidence = 95;
  int sample_method = 0, period = 2, count = 10;
};

AnalysisUi& A() {
  static AnalysisUi a;
  return a;
}

bool BeginDlg(AppState& s, Dialog d, const char* title) {
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return false;
  }
  if (!open || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    s.dialog = Dialog::None;
    return false;
  }
  return true;
}

void EndDlg(AppState& s, bool close) {
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// the tools in Excel's order (alphabetical, as in its list)
const Tool kOrder[] = {Tool::Anova1,        Tool::Anova2,     Tool::Anova3,        Tool::Correlation, Tool::Covariance,
                       Tool::Descriptive,   Tool::ExpSmoothing, Tool::FTest,       Tool::Fourier,     Tool::Histogram,
                       Tool::MovingAverage, Tool::Random,     Tool::RankPercentile, Tool::Regression,  Tool::Sampling,
                       Tool::TTestPaired,   Tool::TTestEqual, Tool::TTestUnequal,  Tool::ZTest};

const char* ToolLabel(Tool t) {
  switch (t) {
    case Tool::Anova1: return TR("Anova: Single Factor");
    case Tool::Anova2: return TR("Anova: Two-Factor With Replication");
    case Tool::Anova3: return TR("Anova: Two-Factor Without Replication");
    case Tool::Correlation: return TR("Correlation");
    case Tool::Covariance: return TR("Covariance");
    case Tool::Descriptive: return TR("Descriptive Statistics");
    case Tool::ExpSmoothing: return TR("Exponential Smoothing");
    case Tool::FTest: return TR("F-Test Two-Sample for Variances");
    case Tool::Fourier: return TR("Fourier Analysis");
    case Tool::Histogram: return TR("Histogram");
    case Tool::MovingAverage: return TR("Moving Average");
    case Tool::Random: return TR("Random Number Generation");
    case Tool::RankPercentile: return TR("Rank and Percentile");
    case Tool::Regression: return TR("Regression");
    case Tool::Sampling: return TR("Sampling");
    case Tool::TTestPaired: return TR("t-Test: Paired Two Sample for Means");
    case Tool::TTestEqual: return TR("t-Test: Two-Sample Assuming Equal Variances");
    case Tool::TTestUnequal: return TR("t-Test: Two-Sample Assuming Unequal Variances");
    case Tool::ZTest: return TR("z-Test: Two Sample for Means");
    default: return "";
  }
}

bool TwoSample(Tool t) { return t == Tool::FTest || t == Tool::TTestPaired || t == Tool::TTestEqual || t == Tool::TTestUnequal || t == Tool::ZTest; }
bool HasGrouped(Tool t) {
  return t == Tool::Anova1 || t == Tool::Correlation || t == Tool::Covariance || t == Tool::Descriptive || t == Tool::RankPercentile;
}
bool HasAlpha(Tool t) { return t == Tool::Anova1 || t == Tool::Anova2 || t == Tool::Anova3 || TwoSample(t); }

// a range box: typed, or picked on the sheet with its button (the dialog shrinks while the user selects)
void Field(AppState& s, const char* label, char* buf, size_t n) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(S(190));
  RangeBox(s, label, buf, n, S(250), 0, label);
}

void FloatField(const char* label, float* v, const char* fmt = "%g") {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(S(190));
  ImGui::SetNextItemWidth(S(100));
  ImGui::InputFloat((std::string("##") + label).c_str(), v, 0, 0, fmt);
}

void IntField(const char* label, int* v) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(S(190));
  ImGui::SetNextItemWidth(S(100));
  ImGui::InputInt((std::string("##") + label).c_str(), v, 0);
}

bool ParseOne(AppState& s, const char* text, uint32_t* sheet, Rect* r) {
  std::vector<std::pair<uint32_t, Rect>> refs;
  const uint32_t focus = s.focus_sheet;
  const std::string t = text;
  if (t.empty()) return false;
  const bool ok = s.engine->Call([&](Document& d) { return d.ParseRefList(t, focus, &refs); });
  if (!ok || refs.size() != 1) return false;
  *sheet = refs[0].first;
  *r = refs[0].second;
  return true;
}

std::string SelText(AppState& s) {
  const Rect r = SelRect(s);
  return r.IsCell() ? core::CellA1(r.r0, r.c0, true, true) : core::CellA1(r.r0, r.c0, true, true) + ":" + core::CellA1(r.r1, r.c1, true, true);
}

std::string g_overwrite;  // the output range a tool would write over (Excel asks first)

bool RunTool(AppState& s, bool overwrite_ok = false) {
  AnalysisUi& a = A();
  Document::AnalysisRequest req;
  auto& p = req.p;
  p.tool = a.tool;
  auto need = [&](const char* text, const char* what, uint32_t* sheet, Rect* r) {
    if (ParseOne(s, text, sheet, r)) return true;
    s.ShowError(TR("Data Analysis"), std::string(what) + " " + TR("isn't a valid reference."));
    return false;
  };
  if (a.tool != Tool::Random) {
    const char* what = a.tool == Tool::Regression ? TR("Input Y Range") : TwoSample(a.tool) ? TR("Variable 1 Range") : TR("Input Range");
    if (!need(a.in, what, &req.in_sheet, &req.in)) return false;
    req.has_in = true;
  } else {
    req.in_sheet = s.focus_sheet;
  }
  if (TwoSample(a.tool) || a.tool == Tool::Regression) {
    if (!need(a.in2, a.tool == Tool::Regression ? TR("Input X Range") : TR("Variable 2 Range"), &req.in2_sheet, &req.in2)) return false;
    req.has_in2 = true;
  }
  if ((a.tool == Tool::Histogram || (a.tool == Tool::Random && a.distribution == 6)) && a.bins[0]) {
    if (!need(a.bins, a.tool == Tool::Histogram ? TR("Bin Range") : TR("Value and Probability Input Range"), &req.bins_sheet, &req.bins)) return false;
    req.has_bins = true;
  }
  if (a.out_kind == 0) {
    Rect o{};
    if (!need(a.out, TR("Output Range"), &req.out_sheet, &o)) return false;
    req.out_r = o.r0;
    req.out_c = o.c0;
  } else {
    req.new_sheet = true;
    req.sheet_name = a.out_kind == 1 ? a.sheet_name : "";
  }
  req.chart_output = a.chart_output;
  req.residual_plots = a.residual_plots;
  req.line_fit_plots = a.line_fit_plots;
  req.normal_plots = a.normal_prob;
  p.by_rows = a.by_rows;
  p.labels = a.labels;
  p.alpha = a.alpha;
  p.summary = a.summary;
  p.conf = a.conf_on ? a.conf : 0;
  p.kth_largest = a.k1_on ? a.k1 : 0;
  p.kth_smallest = a.k2_on ? a.k2 : 0;
  p.rows_per_sample = a.rows_per_sample;
  p.damping = a.damping;
  p.interval = a.interval;
  p.std_errors = a.std_errors;
  p.hyp_diff = a.diff;
  p.var1 = a.var1;
  p.var2 = a.var2;
  p.inverse = a.inverse;
  p.pareto = a.pareto;
  p.cumulative = a.cumulative;
  p.variables = a.variables;
  p.points = a.points;
  p.distribution = a.distribution + 1;
  p.p1 = a.p[0];
  p.p2 = a.p[1];
  p.p3 = a.p[2];
  p.p4 = a.p[3];
  p.p5 = a.p[4];
  p.seed = a.seed > 0 ? static_cast<uint64_t>(a.seed) : 0;
  p.zero_constant = a.zero_constant;
  p.confidence = a.confidence_on ? a.confidence : 0;
  p.residuals = a.residuals;
  p.std_residuals = a.std_residuals;
  p.normal_probability = a.normal_prob;
  p.periodic = a.sample_method == 0;
  p.period = a.period;
  p.count = a.count;
  std::string err;
  uint32_t osheet = 0;
  Rect orect{};
  if (!req.new_sheet && !overwrite_ok) {
    // where the results would go: "Output range will overwrite existing data" when any of those cells holds something
    req.dry_run = true;
    bool filled = false;
    const bool fits = s.engine->Call([&](Document& d) {
      if (!d.RunAnalysis(req, &osheet, &orect, &err)) return false;
      const model::Sheet* os = d.S(osheet);
      for (uint32_t r = orect.r0; os && r <= orect.r1 && !filled; ++r) {
        for (uint32_t c = orect.c0; c <= orect.c1 && !filled; ++c) filled = !os->IsEmptyCell(r, c);
      }
      return true;
    });
    req.dry_run = false;
    if (!fits) {
      if (!err.empty()) s.ShowError(TR("Data Analysis"), err);  // a refusal says why itself: never a blank message
      return err.empty();
    }
    if (filled) {
      g_overwrite = core::CellA1(orect.r0, orect.c0, true, true) + ":" + core::CellA1(orect.r1, orect.c1, true, true);
      return true;  // the dialog closes; the question follows
    }
  }
  if (a.out_kind == 2) {
    // New Workbook: the results on a sheet of their own, as values with their formats, in a new window
    model::Workbook book;
    const bool ok = s.engine->Call([&](Document& d) {
      const size_t before = d.UndoDepth();
      if (!d.RunAnalysis(req, &osheet, &orect, &err)) return false;
      book = d.wb();
      if (d.UndoDepth() > before) d.Undo();  // this workbook stays as it was
      return true;
    });
    if (!ok) {
      if (!err.empty()) s.ShowError(TR("Data Analysis"), err);
      return err.empty();
    }
    Document nd(std::move(book));
    std::vector<uint32_t> others;
    for (const auto& sc : nd.wb().sheets) {
      if (sc->id != osheet) others.push_back(sc->id);
    }
    for (uint32_t r = orect.r0; r <= orect.r1; ++r) {
      for (uint32_t c = orect.c0; c <= orect.c1; ++c) {
        if (nd.GetFormulaA1(osheet, r, c).empty()) continue;
        const core::Value v = nd.GetValue(osheet, r, c);
        nd.SetValue(osheet, r, c, v);
      }
    }
    for (uint32_t id : others) nd.DeleteSheet(id);
    nd.RenameSheet(osheet, "Sheet1");
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    const std::filesystem::path dir = std::filesystem::path(tmp) / L"ycgrid_newbook";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring path = (dir / (L"Book" + std::to_wstring(::GetTickCount64() % 100000) + L".xlsx")).wstring();
    // an xlsx file opens as a new, untitled workbook (File > Save asks where to keep it)
    if (!io::ExportXlsx(nd.wb(), path, &err) || !OpenInNewWindow(path)) {
      s.ShowError(TR("Data Analysis"), err.empty() ? TR("The new workbook couldn't be opened.") : err);
      return false;
    }
    return true;
  }
  const bool ok = s.engine->Call([&](Document& d) { return d.RunAnalysis(req, &osheet, &orect, &err); });
  if (!ok) {
    if (!err.empty()) s.ShowError(TR("Data Analysis"), err);
    return err.empty();
  }
  if (osheet != s.focus_sheet) {
    // a new worksheet ply: the snapshot must have it before the results can be shown (Excel shows them)
    s.engine->Sync();
    s.snap = s.engine->Current();
    ActivateSheet(s, osheet);
  }
  render::SelectRange(s.FocusView(), orect);
  return true;
}

void ToolOptions(AppState& s) {
  AnalysisUi& a = A();
  const Tool t = a.tool;
  ImGui::SeparatorText(TR("Input"));
  if (t == Tool::Regression) {
    Field(s, TR("Input Y Range:"), a.in, sizeof(a.in));
    Field(s, TR("Input X Range:"), a.in2, sizeof(a.in2));
  } else if (TwoSample(t)) {
    Field(s, TR("Variable 1 Range:"), a.in, sizeof(a.in));
    Field(s, TR("Variable 2 Range:"), a.in2, sizeof(a.in2));
  } else if (t != Tool::Random) {
    Field(s, TR("Input Range:"), a.in, sizeof(a.in));
  }
  if (HasGrouped(t)) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Grouped By:"));
    ImGui::SameLine(S(190));
    int g = a.by_rows ? 1 : 0;
    ui::RadioButton(TR("Columns"), &g, 0);
    ImGui::SameLine();
    ui::RadioButton(TR("Rows"), &g, 1);
    a.by_rows = g == 1;
  }
  if (t == Tool::Histogram) Field(s, TR("Bin Range:"), a.bins, sizeof(a.bins));
  if (t == Tool::Anova2) IntField(TR("Rows per sample:"), &a.rows_per_sample);
  if (t == Tool::Random) {
    IntField(TR("Number of Variables:"), &a.variables);
    if (a.distribution != 5) IntField(TR("Number of Random Numbers:"), &a.points);
    const char* dists[] = {TR("Uniform"), TR("Normal"), TR("Bernoulli"), TR("Binomial"), TR("Poisson"), TR("Patterned"), TR("Discrete")};
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Distribution:"));
    ImGui::SameLine(S(190));
    ImGui::SetNextItemWidth(S(160));
    ui::Combo("##dist", &a.distribution, dists, 7);
    switch (a.distribution) {
      case 0:
        FloatField(TR("Between:"), &a.p[0]);
        FloatField(TR("and:"), &a.p[1]);
        break;
      case 1:
        FloatField(TR("Mean ="), &a.p[0]);
        FloatField(TR("Standard deviation ="), &a.p[1]);
        break;
      case 2: FloatField(TR("p Value ="), &a.p[0]); break;
      case 3:
        FloatField(TR("p Value ="), &a.p[0]);
        FloatField(TR("Number of trials ="), &a.p[1], "%.0f");
        break;
      case 4: FloatField(TR("Lambda ="), &a.p[0]); break;
      case 5:
        FloatField(TR("From:"), &a.p[0]);
        FloatField(TR("To:"), &a.p[1]);
        FloatField(TR("in steps of:"), &a.p[2]);
        FloatField(TR("repeating each number:"), &a.p[3], "%.0f");
        FloatField(TR("repeating the sequence:"), &a.p[4], "%.0f");
        break;
      case 6: Field(s, TR("Value and Probability Input Range:"), a.bins, sizeof(a.bins)); break;
      default: break;
    }
    IntField(TR("Random Seed:"), &a.seed);
  }
  if (t != Tool::Random && t != Tool::Anova2) ImGui::Checkbox(TR("Labels"), &a.labels);
  if (HasAlpha(t)) FloatField(TR("Alpha:"), &a.alpha, "%.3g");
  if (t == Tool::TTestPaired || t == Tool::TTestEqual || t == Tool::TTestUnequal || t == Tool::ZTest)
    FloatField(TR("Hypothesized Mean Difference:"), &a.diff);
  if (t == Tool::ZTest) {
    FloatField(TR("Variable 1 Variance (known):"), &a.var1);
    FloatField(TR("Variable 2 Variance (known):"), &a.var2);
  }
  if (t == Tool::ExpSmoothing) FloatField(TR("Damping factor:"), &a.damping);
  if (t == Tool::MovingAverage) IntField(TR("Interval:"), &a.interval);
  if (t == Tool::Regression) {
    ImGui::Checkbox(TR("Constant is Zero"), &a.zero_constant);
    ImGui::Checkbox(TR("Confidence Level"), &a.confidence_on);
    if (a.confidence_on) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(70));
      ImGui::InputFloat("%##regconf", &a.confidence, 0, 0, "%g");
    }
  }
  ImGui::SeparatorText(TR("Output options"));
  ui::RadioButton(TR("Output Range:"), &a.out_kind, 0);
  ImGui::SameLine(S(190));
  {
    const std::string before = a.out;
    RangeBox(s, "analysis.out", a.out, sizeof(a.out), S(250), RangeBox_Cell, TR("Output Range:"));
    if (before != a.out) a.out_kind = 0;
  }
  ui::RadioButton(TR("New Worksheet Ply:"), &a.out_kind, 1);
  ImGui::SameLine(S(190));
  ImGui::SetNextItemWidth(S(220));
  if (ImGui::InputText("##ply", a.sheet_name, sizeof(a.sheet_name))) a.out_kind = 1;
  ui::RadioButton(TR("New Workbook"), &a.out_kind, 2);
  switch (t) {
    case Tool::Descriptive:
      ImGui::Checkbox(TR("Summary statistics"), &a.summary);
      ImGui::Checkbox(TR("Confidence Level for Mean:"), &a.conf_on);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(70));
      ImGui::InputFloat("%##conf", &a.conf, 0, 0, "%g");
      ImGui::Checkbox(TR("Kth Largest:"), &a.k1_on);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(70));
      ImGui::InputInt("##k1", &a.k1, 0);
      ImGui::Checkbox(TR("Kth Smallest:"), &a.k2_on);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(70));
      ImGui::InputInt("##k2", &a.k2, 0);
      break;
    case Tool::ExpSmoothing:
    case Tool::MovingAverage: ImGui::Checkbox(TR("Standard Errors"), &a.std_errors); break;
    case Tool::Fourier: ImGui::Checkbox(TR("Inverse"), &a.inverse); break;
    case Tool::Histogram:
      ImGui::Checkbox(TR("Pareto (sorted histogram)"), &a.pareto);
      ImGui::Checkbox(TR("Cumulative Percentage"), &a.cumulative);
      ImGui::Checkbox(TR("Chart Output"), &a.chart_output);
      break;
    case Tool::Regression:
      // Excel's layout: Residuals / Residual Plots, Standardized Residuals / Line Fit Plots
      ImGui::SeparatorText(TR("Residuals"));
      ImGui::Checkbox(TR("Residuals"), &a.residuals);
      ImGui::SameLine(S(200));
      ImGui::Checkbox(TR("Residual Plots"), &a.residual_plots);
      ImGui::Checkbox(TR("Standardized Residuals"), &a.std_residuals);
      ImGui::SameLine(S(200));
      ImGui::Checkbox(TR("Line Fit Plots"), &a.line_fit_plots);
      ImGui::SeparatorText(TR("Normal Probability"));
      ImGui::Checkbox(TR("Normal Probability Plots"), &a.normal_prob);
      break;
    case Tool::Sampling:
      ImGui::SeparatorText(TR("Sampling Method"));
      ui::RadioButton(TR("Periodic"), &a.sample_method, 0);
      ImGui::SameLine(S(190));
      if (a.sample_method == 0) IntField(TR("Period:"), &a.period);
      ui::RadioButton(TR("Random"), &a.sample_method, 1);
      if (a.sample_method == 1) {
        ImGui::SameLine(S(190));
        IntField(TR("Number of Samples:"), &a.count);
      }
      break;
    default: break;
  }
  (void)s;
}

}  // namespace

void RegisterAnalysisCommands(AppState& s) {
  ui::Command c;
  c.id = "DataAnalysis";
  c.label = TR("Data\nAnalysis");
  c.icon = ui::icon::DataAnalysis;
  c.tip_desc = TR("Analyze data with statistical and engineering tools: descriptive statistics, regression, t-tests, ANOVA, histograms and more.");
  ui::Commands().Add(c).execute = [&s](ui::Command&) { s.OpenDialog(Dialog::DataAnalysis); };
}

void DrawAnalysisDialogs(AppState& s) {
  AnalysisUi& a = A();
  if (BeginDlg(s, Dialog::DataAnalysis, TR("Data Analysis###yc_data_analysis"))) {
    bool close = false, next = false;
    ImGui::TextUnformatted(TR("Analysis Tools"));
    const bool moved = DlgListKeys(&a.sel, static_cast<int>(std::size(kOrder)), [](int i) { return ToolLabel(kOrder[i]); });
    if (ImGui::BeginListBox("##tools", ImVec2(S(330), ImGui::GetTextLineHeightWithSpacing() * 12))) {
      ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);  // the arrows move the choice (DlgListKeys), not a second cursor
      for (int i = 0; i < static_cast<int>(std::size(kOrder)); ++i) {
        if (moved && a.sel == i) ImGui::SetScrollHereY();
        if (ImGui::Selectable(ToolLabel(kOrder[i]), a.sel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
          a.sel = i;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) next = true;
        }
      }
      ImGui::PopItemFlag();
      ImGui::EndListBox();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) next = true;
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    ImGui::EndGroup();
    if (next) {
      a.tool = kOrder[a.sel];
      // the selection is the input, like Excel's dialogs
      std::snprintf(a.in, sizeof(a.in), "%s", SelText(s).c_str());
      a.in2[0] = a.bins[0] = 0;
      close = true;
    }
    EndDlg(s, close);
    if (next) s.OpenDialog(Dialog::AnalysisTool);
  }
  const std::string title = std::string(ToolLabel(a.tool)) + "###yc_analysis_tool";
  if (BeginDlg(s, Dialog::AnalysisTool, title.c_str())) {
    bool close = false;
    ImGui::BeginGroup();
    ToolOptions(s);
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    g_overwrite.clear();
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) && RunTool(s)) close = true;
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    ImGui::EndGroup();
    EndDlg(s, close);
    if (!g_overwrite.empty()) {
      AskYesNo(s, ToolLabel(a.tool), TR("Output range will overwrite existing data. Press OK to overwrite data in range:") + std::string("\n") + g_overwrite,
               [&s]() { RunTool(s, true); });
      g_overwrite.clear();
    }
  }
}

}  // namespace yc::app
