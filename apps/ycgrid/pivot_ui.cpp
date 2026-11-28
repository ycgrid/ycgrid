// SPDX-License-Identifier: Apache-2.0
// pivot_ui.cpp - Insert -> PivotTable, the PivotTable Fields pane, PivotTable Analyze / Design, the report's filter
// and +/- buttons, slicers, timelines and PivotCharts' field buttons
#include "app_state.h"
#include "core/i18n.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "engine/pivot_engine.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_set>

namespace yc::app {

using engine::Document;
using model::PivotTable;
using ui::S;

namespace {
// an empty list of slicers (a sheet that is gone): a reference, so a loop over it never copies
const std::vector<model::Slicer>& NoSlicers() {
  static const std::vector<model::Slicer> none;
  return none;
}

enum Area { kNone = 0, kFilters = 1, kColumns = 2, kRows = 3, kValues = 4 };

struct DragField {
  int field = -1;
  int from = kNone;  // area it is dragged from
  int index = -1;    // position in that area
};

// a PivotTable's source and report, read once per snapshot (slicers, menus and dialogs read them every frame)
struct Cached {
  uint64_t version = ~0ull;
  bool ok = false;
  std::string err;
  std::shared_ptr<const engine::PivotCache> cache;
  std::shared_ptr<const engine::PivotReport> rep;
  std::map<int, std::vector<char>> with_data;  // slicers: the items of a field that still have data under the other filters
  std::map<int, std::pair<double, double>> dates;  // timelines: a field's first and last day
};

struct PivotUi {
  uint32_t sheet = 0, id = 0;  // the PivotTable of the active cell (or of the selected PivotChart; 0 = none)
  bool chart_mode = false;     // a PivotChart is selected: the pane names the areas as for a chart
  bool field_list = true;      // the pane opens for the active PivotTable (Field List button)
  // Insert PivotTable / Change Data Source / Move dialogs
  char source[256] = "", dest[128] = "", name[128] = "";
  int dest_new = 1;
  bool with_chart = false;  // Insert > PivotChart: a PivotChart comes with the new PivotTable
  int move_new = 0;         // Move PivotTable: 1 New Worksheet
  // Value Field Settings
  int vf_index = -1, vf_tab = 0;
  char vf_caption[128] = "", vf_format[64] = "";
  int vf_func = 0, vf_show = 0, vf_base_field = -1;
  std::string vf_base_item;
  // Field Settings
  int fs_area = kRows, fs_pos = 0, fs_sub = 0;
  char fs_caption[128] = "";
  uint32_t fs_funcs = 0;
  bool fs_repeat = false, fs_blank = false, fs_all = false;
  bool fs_new = false, fs_compact = false, fs_top = false, fs_break = false;
  int fs_form = 1;  // 1 outline, 2 tabular
  bool vf_nf_open = false;  // Value Field Settings > Number Format
  // PivotTable Options
  PivotTable opt;
  char opt_name[128] = "", opt_err[64] = "", opt_null[64] = "";
  // More Sort Options
  int so_area = kRows, so_pos = 0, so_mode = 1, so_by = -1;
  // Label / Value / Top 10 filter
  int pf_area = kRows, pf_pos = 0, pf_kind = 0, pf_type = 15, pf_data = 0, pf_top = 0, pf_unit = 0;
  char pf_v1[128] = "", pf_v2[128] = "";
  // Show Detail (double-click an innermost item)
  int sd_area = kRows, sd_pos = 0, sd_pick = -1;
  // Number Format (a data field)
  int nf_index = -1;
  char nf_format[64] = "";
  // Group dialog
  int grp_field = -1, grp_by = 6;
  bool grp_dates = false, grp_auto_start = true, grp_auto_end = true;
  uint32_t grp_periods = model::kPeriodMonths;
  int grp_days = 1;
  double grp_start = 0, grp_end = 0, grp_interval = 10;
  char grp_start_text[32] = "", grp_end_text[32] = "";
  // Insert Slicer dialog: which fields
  std::vector<char> slicer_pick;
  // slicer drag (move / resize)
  uint32_t sl_drag = 0;
  int sl_mode = 0;  // 1 move, 2 resize
  ImVec2 sl_mouse0;
  double sl_l0 = 0, sl_t0 = 0, sl_w0 = 0, sl_h0 = 0;
  std::set<uint32_t> sl_multi;  // slicers with Multi-Select on (a click adds or removes an item)
  // the slicer selected by a click (Slicer tab, Delete, Escape) and the Slicer Settings dialog's copy of it
  uint32_t sl_sel = 0, sl_sel_sheet = 0;
  // Insert Slicer: the new slicer is selected when it appears (ids above sl_select_after on that sheet)
  bool sl_select_pending = false;
  uint32_t sl_select_after = 0, sl_select_sheet = 0;
  uint32_t ss_sheet = 0, ss_id = 0;
  model::Slicer ss;
  model::Slicer clip;  // a slicer copied with Ctrl+C (Paste pastes it while nothing was copied since)
  bool has_clip = false;
  uint32_t clip_seq = 0;
  char ss_caption[128] = "";
  // Calculated Field / Calculated Item dialogs
  char calc_name[64] = "", calc_formula[256] = "=";
  int item_field = -1;
  // Insert Slicer / Insert Timeline
  bool pick_timeline = false;
  std::string pick_table;  // Insert Slicer for a table (its columns are the fields)
  uint32_t pick_table_sheet = 0;
  // Report Connections (a slicer) / Filter Connections (a PivotTable): the check boxes
  uint32_t rc_sheet = 0, rc_id = 0;
  std::vector<std::string> rc_names;  // PivotTables (Report Connections) or "sheet id:slicer id" (Filter Connections)
  std::vector<char> rc_on;
  // Recommended PivotTables: the source and the picked layout
  int rec_pick = 0;
  std::string rec_source;
  // the field list's gear menu: 0 stacked, 1 side by side, 2 fields only, 3 areas only (2 by 2), 4 areas only (1 by 4)
  int pane_layout = 0;
  // Analyze > Active Field: the box's text
  char active_field[128] = "";
  uint64_t active_field_version = ~0ull;
  int active_field_key = -1;
  // Analyze > PivotTable Name: the box's text
  char name_box[256] = "";
  uint64_t name_box_version = ~0ull;
  // timelines: horizontal scroll and the period drag
  std::map<uint32_t, float> tl_scroll;
  uint32_t tl_drag = 0;
  int tl_a = -1, tl_b = -1;
  // filter drop-down on the report (or on a PivotChart's field button)
  int filter_area = kRows, filter_field_pos = 0;
  std::vector<std::string> filter_items;  // identity texts
  std::vector<std::string> filter_shown;  // as shown
  std::vector<char> filter_on;
  bool filter_multi = false;  // the Filters area: Select Multiple Items
  char filter_search[64] = "";
  ImVec2 filter_pos;
  // the pane: Search, Defer Layout Update (changes wait in `pending` until Update)
  char search[64] = "";
  bool defer = false;
  std::optional<PivotTable> pending;
  std::map<uint32_t, Cached> cached;
  // Insert PivotTable runs on the engine thread (a large source took the screen for a second): its result
  struct Made {
    std::atomic<bool> done{false};
    uint32_t id = 0, sheet = 0, r0 = 0, c0 = 0;
    bool message = false;  // err is about the source (a message), not a failure
    bool chart = false;    // Insert > PivotChart
    std::string err;
  };
  std::shared_ptr<Made> made;
};

PivotUi& U() {
  static PivotUi u;
  return u;
}

// the selected slicer (a click on it), when it is still on its sheet
const model::Slicer* SelectedSlicer(const AppState& s) {
  const PivotUi& u = U();
  const model::Sheet* sh = u.sl_sel && s.snap ? s.snap->Sheet(u.sl_sel_sheet) : nullptr;
  if (!sh) return nullptr;
  for (const auto& sl : *sh->slicers) {
    if (sl.id == u.sl_sel) return &sl;
  }
  return nullptr;
}

void OpenSlicerSettings(AppState& s) {
  const model::Slicer* sl = SelectedSlicer(s);
  if (!sl) return;
  PivotUi& u = U();
  u.ss = *sl;
  u.ss_sheet = u.sl_sel_sheet;
  u.ss_id = sl->id;
  std::snprintf(u.ss_caption, sizeof u.ss_caption, "%s", (sl->caption.empty() ? sl->name : sl->caption).c_str());
  s.OpenDialog(Dialog::SlicerSettings);
}

// a copied slicer is what Paste pastes until something else is copied (in YCGrid or another program)
bool SlicerClipPending() { return U().has_clip && ClipboardSequence() == U().clip_seq; }

void CopySlicer(AppState& s, const model::Slicer& sl, uint32_t sheet, bool cut) {
  PivotUi& u = U();
  u.clip = sl;
  u.has_clip = true;
  u.clip_seq = ClaimClipboard(s);  // the cells' copy ends, as for any copied object
  if (cut) {
    const uint32_t id = sl.id;
    if (u.sl_sel == id) u.sl_sel = 0;
    s.engine->PostUser(TR("Cut"), [sheet, id](Document& d) { d.DeleteSlicer(sheet, id); });
  } else {
    s.SetStatus(TR("Slicer copied"));
  }
}

// a copy of a slicer at (left, top) points on a sheet; it is selected when it appears
void PasteSlicerAt(AppState& s, uint32_t sheet, model::Slicer c, double left, double top) {
  PivotUi& u = U();
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return;
  c.id = 0;
  c.left = left;
  c.top = top;
  u.sl_select_pending = true;
  u.sl_select_sheet = sheet;
  u.sl_select_after = 0;
  for (const auto& x : *sh->slicers) u.sl_select_after = std::max(u.sl_select_after, x.id);
  s.engine->PostUser(TR("Paste"), [sheet, c](Document& d) { d.AddSlicer(sheet, c); });
}

void EditSelectedSlicer(AppState& s, const char* label, std::function<void(model::Slicer&)> fn) {
  const model::Slicer* sl = SelectedSlicer(s);
  if (!sl) return;
  const uint32_t sheet = U().sl_sel_sheet, id = sl->id;
  s.engine->PostUser(label, [sheet, id, fn = std::move(fn)](Document& d) { d.UpdateSlicer(sheet, id, fn); });
}

// the Slicer tab's boxes: Caption, Columns, Height and Width (inches, as Excel shows them); Enter or leaving the box sets it
void DrawSlicerBox(AppState& s, int which, const ImRect& r) {
  const model::Slicer* sl = SelectedSlicer(s);
  if (!sl) return;
  static char buf[4][128];
  static uint64_t shown_version[4] = {};
  static uint32_t shown_id[4] = {};
  static const char* const kIds[] = {"##SlicerCaption", "##SlicerColumns", "##SlicerHeight", "##SlicerWidth"};
  static const char* const kNotes[] = {"Slicer Caption", "Slicer Columns", "Slicer Height", "Slicer Width"};
  ImGui::PushID(which);
  const char* label = which == 0 ? TR("Slicer Caption:") : which == 1 ? TR("Columns:") : which == 2 ? TR("Height:") : TR("Width:");
  const float fy = r.Min.y + std::max(0.0f, (r.GetHeight() - ImGui::GetFrameHeight()) * 0.5f);
  ImGui::GetWindowDrawList()->AddText(ImVec2(r.Min.x + S(4), fy + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), label);
  ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(4) + ImGui::CalcTextSize(label).x + S(8), fy));
  const ImGuiID id = ImGui::GetID(kIds[which]);
  if (ImGui::GetActiveID() != id && (shown_version[which] != s.snap->version || shown_id[which] != sl->id)) {
    switch (which) {
      case 0: std::snprintf(buf[0], sizeof buf[0], "%s", (sl->caption.empty() ? sl->name : sl->caption).c_str()); break;
      case 1: std::snprintf(buf[1], sizeof buf[1], "%d", std::max(1, sl->columns)); break;
      case 2: std::snprintf(buf[2], sizeof buf[2], "%.2f\"", sl->height / 72.0); break;
      default: std::snprintf(buf[3], sizeof buf[3], "%.2f\"", sl->width / 72.0); break;
    }
    shown_version[which] = s.snap->version;
    shown_id[which] = sl->id;
  }
  ImGui::SetNextItemWidth(std::max(S(40), r.Max.x - ImGui::GetCursorScreenPos().x - S(2)));
  bool enter = false;
  if (which == 1) {
    int cols = std::max(1, std::atoi(buf[1]));
    ImGui::SetNextItemWidth(std::max(S(60), r.Max.x - ImGui::GetCursorScreenPos().x - S(2)));
    if (ImGui::InputInt(kIds[1], &cols, 1, 1)) {
      cols = std::clamp(cols, 1, 20);
      std::snprintf(buf[1], sizeof buf[1], "%d", cols);
      EditSelectedSlicer(s, TR("Slicer Columns"), [cols](model::Slicer& x) { x.columns = cols; });
    }
  } else {
    enter = ImGui::InputText(kIds[which], buf[which], sizeof buf[which], ImGuiInputTextFlags_EnterReturnsTrue);
  }
  ui::NoteUiItem(kNotes[which], ImGui::GetCurrentContext()->LastItemData.Rect);
  if (which != 1 && (enter || ImGui::IsItemDeactivatedAfterEdit())) {
    if (which == 0) {
      const std::string cap = buf[0];
      EditSelectedSlicer(s, TR("Slicer Caption"), [cap](model::Slicer& x) { x.caption = cap; });
    } else {
      const double inches = std::strtod(buf[which], nullptr);
      if (inches > 0.2 && inches < 100) {
        const double pt = inches * 72.0;
        EditSelectedSlicer(s, which == 2 ? TR("Slicer Height") : TR("Slicer Width"), [which, pt](model::Slicer& x) { (which == 2 ? x.height : x.width) = pt; });
      }
    }
    shown_version[which] = 0;
  }
  ImGui::PopID();
}

const PivotTable* ActivePivot(AppState& s) {
  PivotUi& u = U();
  if (!u.id || !s.snap) return nullptr;
  const model::Sheet* sh = s.snap->Sheet(u.sheet);
  if (!sh) return nullptr;
  for (const auto& p : *sh->pivots) {
    if (p.id == u.id) return &p;
  }
  return nullptr;
}

// the source and report of a PivotTable as this snapshot has it (read again only when the workbook changed)
const Cached& CacheFor(AppState& s, const PivotTable& p) {
  Cached& c = U().cached[p.id];
  const uint64_t v = s.snap ? s.snap->version : 0;
  if (c.version == v && c.cache) return c;
  c = Cached{};
  c.version = v;
  auto cache = std::make_shared<engine::PivotCache>();
  c.ok = engine::ReadPivotSource(s.snap->wb, p, cache.get(), &c.err);
  if (c.ok) c.rep = std::make_shared<engine::PivotReport>(engine::ComputePivot(p, *cache));
  else c.rep = std::make_shared<engine::PivotReport>();
  c.cache = std::move(cache);
  return c;
}

// a change to the report that may cover data: Excel asks "There's already data in ... Do you want to replace it?"
// (op runs on the engine thread with the check on; Yes runs it again allowing it)
void PivotDocOp(AppState& s, const std::string& label, std::function<bool(Document&, std::string*)> op) {
  std::string err;
  bool hit = false;
  s.engine->Call([&](Document& d) {
    d.Begin(label.c_str());
    d.pivot_overwrite = 1;
    op(d, &err);
    hit = d.pivot_overwrite_hit;
    d.pivot_overwrite = 0;
    d.pivot_overwrite_hit = false;
    d.Commit();
    return 0;
  });
  if (hit) {
    AskYesNo(s, "YCGrid", err, [&s, label, op]() {
      std::string e2;
      s.engine->Call([&](Document& d) {
        d.Begin(label.c_str());
        op(d, &e2);
        d.Commit();
        return 0;
      });
      if (!e2.empty()) s.ShowError("YCGrid", e2);
    });
    return;
  }
  if (!err.empty()) s.ShowError("YCGrid", err);
}

void EditPivotOf(AppState& s, uint32_t sheet, uint32_t id, const char* label, std::function<void(PivotTable&)> fn) {
  if (!id) return;
  PivotDocOp(s, label, [sheet, id, fn](Document& d, std::string* err) { return d.UpdatePivot(sheet, id, fn, err); });
}

void EditPivot(AppState& s, const char* label, std::function<void(PivotTable&)> fn) { EditPivotOf(s, U().sheet, U().id, label, std::move(fn)); }

// the pane's changes: at once, or kept until Update while Defer Layout Update is on
void PaneEdit(AppState& s, const PivotTable& p, const char* label, std::function<void(PivotTable&)> fn) {
  PivotUi& u = U();
  if (u.defer) {
    if (!u.pending || u.pending->id != p.id) u.pending = p;
    fn(*u.pending);
    return;
  }
  EditPivot(s, label, std::move(fn));
}

std::string RangeText(const AppState& s, uint32_t sheet, const Rect& r) {
  const model::Sheet* sh = s.snap->Sheet(sheet);
  std::string name = sh ? sh->name : std::string();
  bool quote = false;
  for (char ch : name) quote = quote || !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
  if (quote) name = "'" + name + "'";
  return name + "!" + core::CellA1(r.r0, r.c0, true, true) + ":" + core::CellA1(r.r1, r.c1, true, true);
}

// Excel's rule for a checked field: 0 Rows (text, dates), 1 Values as Sum, 2 Values as Count (numbers with blanks / text)
int DefaultArea(AppState& s, const PivotTable& p, int field) {
  const Cached& c = CacheFor(s, p);
  if (!c.ok) return 0;
  return engine::PivotFieldDefaultArea(*c.cache, field);
}

std::vector<model::PivotAxisField>* AxisOf(PivotTable& p, int area) {
  return area == kFilters ? &p.pages : area == kColumns ? &p.cols : area == kRows ? &p.rows : nullptr;
}
const std::vector<model::PivotAxisField>* AxisOf(const PivotTable& p, int area) {
  return area == kFilters ? &p.pages : area == kColumns ? &p.cols : area == kRows ? &p.rows : nullptr;
}

// moves (or adds) a field to an area at a position (-1 = the end); func: Values' function for a new data field
void ApplyMove(PivotTable& p, DragField drag, int to, int at, model::PivotFunc func) {
  model::PivotAxisField moved;
  moved.field = drag.field;
  model::PivotDataField moved_data;
  moved_data.field = drag.field;
  moved_data.func = func;
  if (drag.from == kValues && drag.index >= 0 && static_cast<size_t>(drag.index) < p.data.size()) {
    moved_data = p.data[static_cast<size_t>(drag.index)];
    p.data.erase(p.data.begin() + drag.index);
  } else if (auto* src = AxisOf(p, drag.from); src && drag.index >= 0 && static_cast<size_t>(drag.index) < src->size()) {
    moved = (*src)[static_cast<size_t>(drag.index)];
    src->erase(src->begin() + drag.index);
    if (drag.from == to && at > drag.index) --at;
  }
  if (to == kValues) {
    if (drag.from == kValues && at > drag.index) --at;
    const size_t pos = at < 0 ? p.data.size() : std::min(p.data.size(), static_cast<size_t>(at));
    p.data.insert(p.data.begin() + static_cast<std::ptrdiff_t>(pos), moved_data);
    return;
  }
  auto* dst = AxisOf(p, to);
  if (!dst) return;
  // a field is on one axis only: Excel moves it
  for (auto* v : {&p.rows, &p.cols, &p.pages}) {
    v->erase(std::remove_if(v->begin(), v->end(), [&](const model::PivotAxisField& f) { return f.field == drag.field; }), v->end());
  }
  moved.page_item = to == kFilters ? moved.page_item : std::string();
  const size_t pos = at < 0 ? dst->size() : std::min(dst->size(), static_cast<size_t>(at));
  dst->insert(dst->begin() + static_cast<std::ptrdiff_t>(pos), moved);
}

model::PivotFunc FuncFor(int area) { return area == 2 ? model::PivotFunc::Count : model::PivotFunc::Sum; }

void RemoveField(PivotTable& p, int area, int index) {
  if (area == kValues) {
    if (index >= 0 && static_cast<size_t>(index) < p.data.size()) p.data.erase(p.data.begin() + index);
    return;
  }
  if (auto* v = AxisOf(p, area); v && index >= 0 && static_cast<size_t>(index) < v->size()) v->erase(v->begin() + index);
}

bool FieldUsed(const PivotTable& p, int f) {
  for (const auto* v : {&p.rows, &p.cols, &p.pages}) {
    for (const auto& a : *v) {
      if (a.field == f) return true;
    }
  }
  for (const auto& d : p.data) {
    if (d.field == f) return true;
  }
  return false;
}

// a field carries a filter (hidden items, a page item, a label or value filter)
bool Filtered(const model::PivotAxisField& af) {
  return !af.hidden.empty() || !af.page_item.empty() || af.label_filter.type != 0 || af.value_filter.type != 0;
}

const model::PivotAxisField* AxisFieldOf(const PivotTable& p, int field, int* area = nullptr, int* pos = nullptr) {
  for (int a : {kRows, kColumns, kFilters}) {
    const auto& v = *AxisOf(p, a);
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].field != field) continue;
      if (area) *area = a;
      if (pos) *pos = static_cast<int>(i);
      return &v[i];
    }
  }
  return nullptr;
}

std::string FieldName(const PivotTable& p, int field) {
  if (const auto* af = AxisFieldOf(p, field); af && !af->caption.empty()) return af->caption;
  return field >= 0 && static_cast<size_t>(field) < p.fields.size() ? p.fields[static_cast<size_t>(field)] : std::string("?");
}

const char* FuncLabel(int i) {
  static const char* kNames[] = {TR("Sum"), TR("Count"), TR("Average"), TR("Max"), TR("Min"), TR("Product"), TR("Count Numbers"),
                                 TR("StdDev"), TR("StdDevp"), TR("Var"), TR("Varp"), TR("Distinct Count")};
  return kNames[std::clamp(i, 0, model::kPivotFuncs - 1)];
}

const char* ShowAsLabel(int i) {
  static const char* kNames[] = {TR("No Calculation"), TR("% of Grand Total"), TR("% of Column Total"), TR("% of Row Total"), TR("Running Total In"),
                                 TR("% Of"), TR("% of Parent Row Total"), TR("% of Parent Column Total"), TR("% of Parent Total"),
                                 TR("Difference From"), TR("% Difference From"), TR("% Running Total In"), TR("Rank Smallest to Largest"),
                                 TR("Rank Largest to Smallest"), TR("Index")};
  return kNames[std::clamp(i, 0, model::kPivotShowAs - 1)];
}

bool BeginDlg(AppState& s, Dialog d, const char* title, float width) {
  if (s.dialog != d) return false;
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(width), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return false;
  }
  if (!open || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !U().vf_nf_open)) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
    ImGui::EndPopup();
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

// ---- what a report cell is: its field (row / column / filter), its item, or its data field ----
struct CellInfo {
  bool in = false;
  uint32_t r = 0, c = 0;  // relative to the report
  engine::PivotCell::Kind kind = engine::PivotCell::Empty;
  int area = kNone, pos = -1;  // the axis field the cell belongs to
  int field = -1, item = -1;   // its source field and item (labels)
  int data = -1;               // the data field (values, captions)
  bool caption = false;        // a data field's caption ("Sum of Sales")
};

CellInfo InfoAt(AppState& s, const PivotTable& p, uint32_t ar, uint32_t ac) {
  CellInfo ci;
  const Cached& cc = CacheFor(s, p);
  if (!cc.ok || ar < p.r0 || ac < p.c0) return ci;
  const engine::PivotReport& rep = *cc.rep;
  const uint32_t r = ar - p.r0, c = ac - p.c0;
  if (r >= rep.rows || c >= rep.cols) return ci;
  ci.in = true;
  ci.r = r;
  ci.c = c;
  const engine::PivotCell& cell = rep.at(r, c);
  ci.kind = cell.kind;
  auto set_axis = [&](int area, int field) {
    int a = kNone, pos = -1;
    if (AxisFieldOf(p, field, &a, &pos) && a == area) {
      ci.area = a;
      ci.pos = pos;
      ci.field = field;
    }
  };
  const uint32_t body0 = rep.page_rows + rep.header_rows;
  if (cell.kind == engine::PivotCell::PageName || cell.kind == engine::PivotCell::PageValue) {
    if (r < p.pages.size()) set_axis(kFilters, p.pages[r].field);
    return ci;
  }
  if (cell.kind == engine::PivotCell::RowLabel && r < rep.row_filter.size()) {
    const auto& rf = rep.row_filter[r];
    if (rep.row_kind[r] == 4) {
      ci.data = rep.row_data[r];
      ci.caption = true;
      return ci;
    }
    // the field whose labels this column holds (compact: the row's own item; outline / tabular: the column's field)
    const int lv = engine::PivotLabelLevel(rep, r, c);
    const size_t lvl = lv >= 0 ? static_cast<size_t>(lv) : 0;
    if (lvl < rf.size()) {
      set_axis(kRows, rf[lvl].first);
      ci.item = rf[lvl].second;
    }
    return ci;
  }
  if (cell.kind == engine::PivotCell::ColLabel && c < rep.col_filter.size()) {
    const auto& cf = rep.col_filter[c];
    const uint32_t lvl = r - rep.page_rows - 1;
    if (lvl < cf.size()) {
      set_axis(kColumns, cf[lvl].first);
      ci.item = cf[lvl].second;
    } else if (rep.col_data[c] >= 0) {
      ci.data = rep.col_data[c];
      ci.caption = true;
    }
    return ci;
  }
  if (cell.kind == engine::PivotCell::Subtotal && r < rep.row_filter.size() && !rep.row_filter[r].empty()) {
    set_axis(kRows, rep.row_filter[r].back().first);
    ci.item = rep.row_filter[r].back().second;
  }
  if (cell.kind == engine::PivotCell::Header) {
    const uint32_t last_header = rep.page_rows + rep.header_rows - 1;
    if (r == last_header && c < rep.label_cols && !p.rows.empty()) {
      const int lv = engine::PivotColumnField(rep, c);
      set_axis(kRows, p.rows[lv >= 0 ? std::min<size_t>(static_cast<size_t>(lv), p.rows.size() - 1) : 0].field);  // "Row Labels" / a row field's name
    } else if (r == rep.page_rows && c == rep.label_cols && !p.cols.empty()) {
      set_axis(kColumns, p.cols[0].field);  // "Column Labels" / the column field's name
    } else if (c < rep.col_data.size() && rep.col_data[c] >= 0) {
      ci.data = rep.col_data[c];
      ci.caption = true;
    } else if (r == rep.page_rows && c == 0 && p.data.size() == 1) {
      ci.data = 0;  // "Sum of Sales" over the row labels
      ci.caption = true;
    }
    return ci;
  }
  if (cell.kind == engine::PivotCell::Data || cell.kind == engine::PivotCell::Grand || cell.kind == engine::PivotCell::Subtotal ||
      (cell.kind == engine::PivotCell::Empty && r >= body0 && c >= rep.label_cols)) {
    if (c < rep.col_data.size() && rep.col_data[c] >= 0) ci.data = rep.col_data[c];
    else if (r < rep.row_data.size() && rep.row_data[r] >= 0) ci.data = rep.row_data[r];
    else if (p.data.size() == 1) ci.data = 0;
  }
  return ci;
}

CellInfo ActiveInfo(AppState& s, const PivotTable& p) {
  const auto& sel = s.FocusView().sel;
  return InfoAt(s, p, sel.act_r, sel.act_c);
}

// the items of a field as the report shows them (identity, shown text)
std::vector<std::pair<std::string, std::string>> ItemsOf(AppState& s, const PivotTable& p, int field) {
  std::vector<std::pair<std::string, std::string>> out;
  const Cached& c = CacheFor(s, p);
  if (!c.ok || field < 0 || static_cast<size_t>(field) >= c.cache->items.size()) return out;
  const auto* af = AxisFieldOf(p, field);
  for (size_t i = 0; i < c.cache->items[static_cast<size_t>(field)].size(); ++i) {
    std::string shown = c.cache->display[static_cast<size_t>(field)][i];
    if (af) {
      for (const auto& [from, to] : af->item_captions) {
        if (from == c.cache->items[static_cast<size_t>(field)][i]) shown = to;
      }
    }
    out.emplace_back(c.cache->items[static_cast<size_t>(field)][i], shown);
  }
  return out;
}

// the items of a field in the order the report shows them
std::vector<std::string> ShownOrder(AppState& s, const PivotTable& p, int area, int field) {
  std::vector<std::string> out;
  const Cached& c = CacheFor(s, p);
  if (!c.ok) return out;
  const auto& filters = area == kColumns ? c.rep->col_filter : c.rep->row_filter;
  std::unordered_set<int> seen;  // each item once: a find over the list so far was report rows times items
  for (const auto& f : filters) {
    for (const auto& [fld, item] : f) {
      if (fld != field || !seen.insert(item).second) continue;
      out.push_back(c.cache->items[static_cast<size_t>(fld)][static_cast<size_t>(item)]);
    }
  }
  return out;
}

Rect Overlap(const Rect& a, const Rect& b) {
  return Rect{std::max(a.r0, b.r0), std::max(a.c0, b.c0), std::min(a.r1, b.r1), std::min(a.c1, b.c1)};
}

// the items of a field whose labels are selected (Keep Only / Hide Selected Items, Group Selection)
std::vector<std::string> SelectedItems(AppState& s, const PivotTable& p, int field) {
  std::vector<std::string> out;
  const Cached& c = CacheFor(s, p);
  if (!c.ok) return out;
  std::unordered_set<int> seen;
  for (const Rect& a : SelAreas(s)) {
    if (!a.Intersects(p.output)) continue;
    const Rect in = Overlap(a, p.output);
    if (in.Count() > 100000) continue;
    for (uint32_t r = in.r0; r <= in.r1; ++r) {
      for (uint32_t cc = in.c0; cc <= in.c1; ++cc) {
        const CellInfo ci = InfoAt(s, p, r, cc);
        if (ci.field != field || ci.item < 0 || !seen.insert(ci.item).second) continue;
        out.push_back(c.cache->items[static_cast<size_t>(field)][static_cast<size_t>(ci.item)]);
      }
    }
  }
  return out;
}

void OpenValueField(AppState& s, const PivotTable& p, int index, int tab = 0) {
  PivotUi& u = U();
  if (index < 0 || static_cast<size_t>(index) >= p.data.size()) return;
  const auto& df = p.data[static_cast<size_t>(index)];
  const Cached& c = CacheFor(s, p);
  u.vf_index = index;
  u.vf_tab = tab;
  std::snprintf(u.vf_caption, sizeof(u.vf_caption), "%s", c.ok ? engine::PivotDataCaption(p, *c.cache, static_cast<size_t>(index)).c_str() : df.caption.c_str());
  std::snprintf(u.vf_format, sizeof(u.vf_format), "%s", df.number_format.c_str());
  u.vf_func = static_cast<int>(df.func);
  u.vf_show = static_cast<int>(df.show_as);
  u.vf_base_field = df.base_field;
  u.vf_base_item = df.base_item;
  s.OpenDialog(Dialog::PivotValueField);
}

void OpenFieldSettings(AppState& s, const PivotTable& p, int area, int pos) {
  PivotUi& u = U();
  const auto* axis = AxisOf(p, area);
  if (!axis || pos < 0 || static_cast<size_t>(pos) >= axis->size()) return;
  const auto& af = (*axis)[static_cast<size_t>(pos)];
  u.fs_area = area;
  u.fs_pos = pos;
  std::snprintf(u.fs_caption, sizeof(u.fs_caption), "%s", FieldName(p, af.field).c_str());
  u.fs_sub = !af.subtotal ? 1 : af.subtotal_funcs ? 2 : 0;
  u.fs_funcs = af.subtotal_funcs ? af.subtotal_funcs : 1u;
  u.fs_repeat = af.repeat_labels;
  u.fs_blank = af.blank_line;
  u.fs_all = af.show_all_items;
  u.fs_new = af.include_new;
  const int form = af.form >= 0 ? af.form : p.layout;
  u.fs_form = form == 2 ? 2 : 1;
  u.fs_compact = form == 0;
  u.fs_top = af.sub_top >= 0 ? af.sub_top == 1 : p.subtotals == 1;
  u.fs_break = af.page_break;
  s.OpenDialog(Dialog::PivotFieldSettings);
}

void OpenSort(AppState& s, const PivotTable& p, int area, int pos) {
  PivotUi& u = U();
  const auto* axis = AxisOf(p, area);
  if (!axis || pos < 0 || static_cast<size_t>(pos) >= axis->size()) return;
  const auto& af = (*axis)[static_cast<size_t>(pos)];
  u.so_area = area;
  u.so_pos = pos;
  u.so_mode = !af.manual_order.empty() ? 0 : af.descending ? 2 : 1;
  u.so_by = af.sort_by;
  s.OpenDialog(Dialog::PivotSort);
}

// kind: 0 Label Filters, 1 Value Filters, 2 Top 10
void OpenFilterDlg(AppState& s, const PivotTable& p, int area, int pos, int kind, int type) {
  PivotUi& u = U();
  const auto* axis = AxisOf(p, area);
  if (!axis || pos < 0 || static_cast<size_t>(pos) >= axis->size()) return;
  const auto& af = (*axis)[static_cast<size_t>(pos)];
  u.pf_area = area;
  u.pf_pos = pos;
  u.pf_kind = kind;
  u.pf_type = type;
  const model::PivotFilter& f = kind == 0 ? af.label_filter : af.value_filter;
  const bool same = f.type == type || (kind == 2 && f.type >= 1 && f.type <= 6);
  std::snprintf(u.pf_v1, sizeof(u.pf_v1), "%s", same ? f.v1.c_str() : (kind == 2 ? "10" : ""));
  std::snprintf(u.pf_v2, sizeof(u.pf_v2), "%s", same ? f.v2.c_str() : "");
  u.pf_data = same ? f.data : 0;
  u.pf_top = same && kind == 2 ? (f.type % 2 == 1 ? 0 : 1) : 0;
  u.pf_unit = same && kind == 2 ? (f.type - 1) / 2 : 0;
  s.OpenDialog(Dialog::PivotFilterDlg);
}

void OpenOptions(AppState& s, const PivotTable& p) {
  PivotUi& u = U();
  u.opt = p;
  std::snprintf(u.opt_name, sizeof(u.opt_name), "%s", p.name.c_str());
  std::snprintf(u.opt_err, sizeof(u.opt_err), "%s", p.error_string.c_str());
  std::snprintf(u.opt_null, sizeof(u.opt_null), "%s", p.null_string.c_str());
  s.OpenDialog(Dialog::PivotOptions);
}

std::string DateText(double serial) {
  int y = 0, m = 0, d = 0;
  if (!formula::SerialToYmd(serial, &y, &m, &d)) return {};
  char b[32];
  std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
  return b;
}

// Group...: dates by periods (when the field holds dates), numbers by ranges; text needs a selection of items
void OpenGroupDialog(AppState& s, const PivotTable& p, int field) {
  PivotUi& u = U();
  if (p.from_model) {
    s.ShowMessage("YCGrid", TR("This PivotTable reads the Data Model, which YCGrid no longer has"));
    return;
  }
  const Cached& c = CacheFor(s, p);
  double lo = 0, hi = 0;
  bool any = false, dates = false, text = false;
  if (c.ok && field >= 0 && static_cast<size_t>(field) < c.cache->columns) {
    for (size_t k = 0; k < c.cache->source_records; ++k) {
      const auto& v = c.cache->records[k][static_cast<size_t>(field)];
      if (!v.IsNumeric()) {
        text = text || !v.IsEmpty();
        continue;
      }
      const double x = v.AsDouble();
      lo = any ? std::min(lo, x) : x;
      hi = any ? std::max(hi, x) : x;
      any = true;
    }
    dates = formula::IsDateFormat(c.cache->numfmts[static_cast<size_t>(field)]);
  }
  if (!any || (text && !dates)) {
    s.ShowMessage("YCGrid", TR("Cannot group that selection."));
    return;
  }
  u.grp_field = field;
  u.grp_dates = dates;
  u.grp_by = dates ? 3 : 6;
  u.grp_periods = model::kPeriodMonths;
  u.grp_days = 1;
  u.grp_auto_start = u.grp_auto_end = true;
  u.grp_start = dates ? std::floor(lo) : lo;
  u.grp_end = dates ? std::floor(hi) : hi;
  u.grp_interval = 10;
  for (const auto& g : p.groups) {
    if (g.field != field || g.by == model::PivotGroupBy::Items) continue;
    u.grp_by = static_cast<int>(g.by);
    if (g.periods) u.grp_periods = g.periods;
    else if (g.by == model::PivotGroupBy::Years) u.grp_periods = model::kPeriodYears;
    else if (g.by == model::PivotGroupBy::Quarters) u.grp_periods = model::kPeriodQuarters;
    else if (g.by == model::PivotGroupBy::YearMonths) u.grp_periods = model::kPeriodYears | model::kPeriodMonths;
    else if (g.by == model::PivotGroupBy::Days) u.grp_periods = model::kPeriodDays;
    u.grp_days = std::max(1, g.days);
    u.grp_auto_start = g.auto_start || (g.start == 0 && dates);
    u.grp_auto_end = g.auto_end || (g.end == 0 && dates);
    if (!u.grp_auto_start) u.grp_start = g.start;
    if (!u.grp_auto_end) u.grp_end = g.end;
    u.grp_interval = g.interval;
  }
  std::snprintf(u.grp_start_text, sizeof(u.grp_start_text), "%s", DateText(u.grp_start).c_str());
  std::snprintf(u.grp_end_text, sizeof(u.grp_end_text), "%s", DateText(u.grp_end).c_str());
  s.OpenDialog(Dialog::PivotGroup);
}

// Ungroup: a group of items (the label's group, or the whole field), or the field's periods / ranges
void Ungroup(AppState& s, const PivotTable& p, int field, const std::string& item) {
  const Cached& c = CacheFor(s, p);
  if (!c.ok) return;
  int gi = field >= 0 && static_cast<size_t>(field) < c.cache->made_by.size() ? c.cache->made_by[static_cast<size_t>(field)] : -1;
  if (gi < 0) {
    for (size_t i = 0; i < p.groups.size(); ++i) {
      if (p.groups[i].field == field) gi = static_cast<int>(i);
    }
  }
  if (gi < 0) return;
  const bool items = p.groups[static_cast<size_t>(gi)].by == model::PivotGroupBy::Items;
  const bool on_made = field >= 0 && static_cast<size_t>(field) < c.cache->made_by.size() && c.cache->made_by[static_cast<size_t>(field)] == gi;
  EditPivot(s, TR("Ungroup"), [gi, items, item, on_made](PivotTable& x) {
    if (static_cast<size_t>(gi) >= x.groups.size()) return;
    auto& g = x.groups[static_cast<size_t>(gi)];
    if (items && on_made && !item.empty()) {
      g.named.erase(std::remove_if(g.named.begin(), g.named.end(), [&](const auto& e) { return e.first == item; }), g.named.end());
      if (!g.named.empty()) return;
    }
    x.groups.erase(x.groups.begin() + gi);
  });
}

// two PivotTables read the same data (a slicer can be connected to both: Report Connections)
bool SameSource(const PivotTable& a, const PivotTable& b) {
  if (!a.src_table.empty() || !b.src_table.empty()) {
    std::string x = a.src_table, y = b.src_table;
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(y.begin(), y.end(), y.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return x == y;
  }
  return a.src_sheet == b.src_sheet && a.src == b.src && !a.from_model && !b.from_model;
}

const PivotTable* PivotNamed(const AppState& s, const std::string& name, uint32_t* sheet = nullptr) {
  for (const auto& sc : s.snap->wb.sheets) {
    for (const auto& p : *sc->pivots) {
      if (p.name == name) {
        if (sheet) *sheet = sc->id;
        return &p;
      }
    }
  }
  return nullptr;
}

// the name of a slicer's field (in its first PivotTable)
std::string SlicerFieldName(const AppState& s, const model::Slicer& sl) {
  const PivotTable* p = PivotNamed(s, sl.pivot);
  return p && sl.field >= 0 && static_cast<size_t>(sl.field) < p->fields.size() ? p->fields[static_cast<size_t>(sl.field)] : std::string();
}

bool SlicerConnectedTo(const model::Slicer& sl, const std::string& pivot) {
  return sl.pivot == pivot || std::find(sl.more_pivots.begin(), sl.more_pivots.end(), pivot) != sl.more_pivots.end();
}

bool HasFieldNamed(const PivotTable& p, const std::string& name) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  for (const auto& f : p.fields) {
    std::string x = f;
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (x == n) return true;
  }
  return false;
}

// Slicer / Timeline tab > Report Connections: the PivotTables of the same source that have the slicer's field
void OpenReportConnections(AppState& s) {
  const model::Slicer* sl = SelectedSlicer(s);
  if (!sl || !sl->table.empty()) return;
  PivotUi& u = U();
  const PivotTable* first = PivotNamed(s, sl->pivot);
  if (!first) return;
  const std::string fname = SlicerFieldName(s, *sl);
  u.rc_sheet = u.sl_sel_sheet;
  u.rc_id = sl->id;
  u.rc_names.clear();
  u.rc_on.clear();
  for (const auto& sc : s.snap->wb.sheets) {
    for (const auto& p : *sc->pivots) {
      if (!SameSource(*first, p) || !HasFieldNamed(p, fname)) continue;
      u.rc_names.push_back(p.name);
      u.rc_on.push_back(SlicerConnectedTo(*sl, p.name) ? 1 : 0);
    }
  }
  s.OpenDialog(Dialog::PivotReportConnections);
}

// Excel's automatic grouping of a date field put on Rows or Columns: Years, Quarters and Months (Months alone when the
// dates are in one year); nothing when it is off (File > Options > Data), grouped already, or not a column of dates
std::optional<model::PivotGroup> AutoDateGroup(AppState& s, const PivotTable& p, int field) {
  if (!Config().pivot_auto_group_dates || p.from_model) return std::nullopt;
  const Cached& c = CacheFor(s, p);
  if (!c.ok || field < 0 || static_cast<size_t>(field) >= c.cache->columns) return std::nullopt;
  if (!formula::IsDateFormat(c.cache->numfmts[static_cast<size_t>(field)])) return std::nullopt;
  for (const auto& g : p.groups) {
    if (g.field == field) return std::nullopt;
  }
  int y0 = 0, y1 = 0;
  bool any = false;
  for (size_t k = 0; k < c.cache->source_records; ++k) {
    const core::Value& v = c.cache->records[k][static_cast<size_t>(field)];
    if (!v.IsNumeric()) continue;
    int y = 0, m = 0, d = 0;
    if (!formula::SerialToYmd(v.AsDouble(), &y, &m, &d)) continue;
    y0 = any ? std::min(y0, y) : y;
    y1 = any ? std::max(y1, y) : y;
    any = true;
  }
  if (!any) return std::nullopt;
  model::PivotGroup g;
  g.field = field;
  g.auto_start = g.auto_end = true;
  g.periods = y1 > y0 ? (model::kPeriodYears | model::kPeriodQuarters | model::kPeriodMonths) : model::kPeriodMonths;
  g.by = y1 > y0 ? model::PivotGroupBy::YearMonths : model::PivotGroupBy::Months;
  return g;
}

// a field added to the report: to Rows / Columns a date field comes grouped (Excel)
void AddFieldTo(AppState& s, const PivotTable& live, const PivotTable& p, int field, int to, int at, model::PivotFunc func, const char* label) {
  const std::optional<model::PivotGroup> g = (to == kRows || to == kColumns) && !FieldUsed(p, field) ? AutoDateGroup(s, live, field) : std::nullopt;
  PaneEdit(s, p, label, [field, to, at, func, g](PivotTable& x) {
    ApplyMove(x, DragField{field, kNone, -1}, to, at, func);
    if (g && std::none_of(x.groups.begin(), x.groups.end(), [&](const model::PivotGroup& o) { return o.field == field; })) x.groups.push_back(*g);
  });
}

// the Recommended PivotTables of a source: a text or date field on Rows with a number summed (or the rows counted)
struct Recommendation {
  std::string title;
  int row = -1, value = -1;
  model::PivotFunc func = model::PivotFunc::Sum;
};

std::vector<Recommendation> Recommend(const engine::PivotCache& c) {
  std::vector<int> labels, numbers;
  for (size_t f = 0; f < c.columns; ++f) {
    const int area = engine::PivotFieldDefaultArea(c, static_cast<int>(f));
    // a label field: few items compared to the records (not an id); a value field: numbers
    if (area == 0) {
      if (c.items[f].size() <= std::max<size_t>(2, c.source_records * 9 / 10) || formula::IsDateFormat(c.numfmts[f])) labels.push_back(static_cast<int>(f));
    } else {
      numbers.push_back(static_cast<int>(f));
    }
  }
  std::vector<Recommendation> out;
  for (int v : numbers) {
    for (int l : labels) {
      Recommendation r;
      r.row = l;
      r.value = v;
      r.title = std::string(TR("Sum of ")) + c.names[static_cast<size_t>(v)] + TR(" by ") + c.names[static_cast<size_t>(l)];
      out.push_back(r);
      if (out.size() >= 10) return out;
    }
  }
  for (int l : labels) {
    if (out.size() >= 10) break;
    Recommendation r;
    r.row = l;
    r.value = l;
    r.func = model::PivotFunc::Count;
    r.title = std::string(TR("Count of ")) + c.names[static_cast<size_t>(l)] + TR(" by ") + c.names[static_cast<size_t>(l)];
    out.push_back(r);
  }
  return out;
}

// one area of the pane: its fields, drag sources and drop targets
void DrawArea(AppState& s, const PivotTable& p, int area, const char* title, float width, float height) {
  ImGui::BeginGroup();
  ImGui::TextUnformatted(title);
  ImGui::PushID(area);
  ImGui::BeginChild("##area", ImVec2(width, height), ImGuiChildFlags_Borders);
  struct Entry {
    int field;
    std::string label;
  };
  std::vector<Entry> entries;
  if (area == kValues) {
    engine::PivotCache cache;
    cache.names = p.fields;
    for (size_t i = 0; i < p.data.size(); ++i) entries.push_back(Entry{p.data[i].field, engine::PivotDataCaption(p, cache, i)});
  } else {
    const auto& v = area == kFilters ? p.pages : area == kColumns ? p.cols : p.rows;
    for (const auto& a : v) entries.push_back(Entry{a.field, FieldName(p, a.field)});
  }
  const std::string values = std::string("\xCE\xA3 ") + TR("Values");
  if (area == kColumns && p.data.size() > 1 && !p.values_on_rows) entries.push_back(Entry{-2, values});
  if (area == kRows && p.data.size() > 1 && p.values_on_rows) entries.push_back(Entry{-2, values});
  auto accept = [&](int at) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("YC_PIVOT_FIELD")) {
      DragField drag = *static_cast<const DragField*>(pl->Data);
      if (drag.field == -2) {
        // the "Values" placeholder: which axis holds the data fields
        const bool rows = area == kRows;
        if (area == kRows || area == kColumns) PaneEdit(s, p, TR("Move Field"), [rows](PivotTable& x) { x.values_on_rows = rows; });
      } else {
        const model::PivotFunc func = area == kValues && drag.from != kValues ? FuncFor(DefaultArea(s, p, drag.field)) : model::PivotFunc::Sum;
        const PivotTable* live = ActivePivot(s);
        if (drag.from == kNone && live) AddFieldTo(s, *live, p, drag.field, area, at, func, TR("Move Field"));
        else PaneEdit(s, p, TR("Move Field"), [drag, area, at, func](PivotTable& x) { ApplyMove(x, drag, area, at, func); });
      }
    }
    ImGui::EndDragDropTarget();
  };
  int axis_index = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    ImGui::PushID(static_cast<int>(i));
    const int index = e.field == -2 ? -1 : axis_index++;
    // a click opens the field's menu; a drag (past the threshold) moves the field instead
    const bool clicked = ImGui::Selectable(e.label.c_str(), false, 0, ImVec2(0, 0)) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 16.0f;
    if (ImGui::BeginDragDropSource()) {
      DragField drag{e.field, area, index};
      ImGui::SetDragDropPayload("YC_PIVOT_FIELD", &drag, sizeof(drag));
      ImGui::TextUnformatted(e.label.c_str());
      ImGui::EndDragDropSource();
    }
    accept(index);
    if (e.field != -2 && clicked) ImGui::OpenPopup("##fieldmenu");
    if (ImGui::BeginPopup("##fieldmenu")) {
      const int n = static_cast<int>(area == kValues ? p.data.size() : AxisOf(p, area)->size());
      DragField me{e.field, area, index};
      if (ImGui::MenuItem(TR("Move Up"), nullptr, false, index > 0)) PaneEdit(s, p, TR("Move Field"), [me, area, index](PivotTable& x) { ApplyMove(x, me, area, index - 1, model::PivotFunc::Sum); });
      if (ImGui::MenuItem(TR("Move Down"), nullptr, false, index + 1 < n)) PaneEdit(s, p, TR("Move Field"), [me, area, index](PivotTable& x) { ApplyMove(x, me, area, index + 2, model::PivotFunc::Sum); });
      if (ImGui::MenuItem(TR("Move to Beginning"), nullptr, false, index > 0)) PaneEdit(s, p, TR("Move Field"), [me, area](PivotTable& x) { ApplyMove(x, me, area, 0, model::PivotFunc::Sum); });
      if (ImGui::MenuItem(TR("Move to End"), nullptr, false, index + 1 < n)) PaneEdit(s, p, TR("Move Field"), [me, area](PivotTable& x) { ApplyMove(x, me, area, -1, model::PivotFunc::Sum); });
      ImGui::Separator();
      const int targets[] = {kFilters, kRows, kColumns, kValues};
      const char* names[] = {TR("Move to Report Filter"), TR("Move to Row Labels"), TR("Move to Column Labels"), TR("Move to Values")};
      for (int k = 0; k < 4; ++k) {
        if (targets[k] == area) continue;
        if (ImGui::MenuItem(names[k])) {
          const int to = targets[k];
          const model::PivotFunc func = to == kValues ? FuncFor(DefaultArea(s, p, e.field)) : model::PivotFunc::Sum;
          PaneEdit(s, p, TR("Move Field"), [me, to, func](PivotTable& x) { ApplyMove(x, me, to, -1, func); });
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Remove Field"))) PaneEdit(s, p, TR("Remove Field"), [area, index](PivotTable& x) { RemoveField(x, area, index); });
      if (area == kValues && ImGui::MenuItem(TR("Value Field Settings..."))) OpenValueField(s, p, index);
      if (area != kValues && ImGui::MenuItem(TR("Field Settings..."))) OpenFieldSettings(s, p, area, index);
      if (area != kValues && area != kFilters) {
        const auto& axis = *AxisOf(p, area);
        const bool desc = axis[static_cast<size_t>(index)].descending;
        if (ImGui::MenuItem(TR("Sort A to Z"), nullptr, !desc)) EditPivot(s, TR("Sort"), [area, index](PivotTable& x) {
          auto& f = (*AxisOf(x, area))[static_cast<size_t>(index)];
          f.descending = false;
          f.sort_by = -1;
          f.manual_order.clear();
        });
        if (ImGui::MenuItem(TR("Sort Z to A"), nullptr, desc)) EditPivot(s, TR("Sort"), [area, index](PivotTable& x) {
          auto& f = (*AxisOf(x, area))[static_cast<size_t>(index)];
          f.descending = true;
          f.sort_by = -1;
          f.manual_order.clear();
        });
        const bool sub = axis[static_cast<size_t>(index)].subtotal;
        if (ImGui::MenuItem(TR("Subtotal"), nullptr, sub)) EditPivot(s, TR("Subtotal"), [area, index, sub](PivotTable& x) { (*AxisOf(x, area))[static_cast<size_t>(index)].subtotal = !sub; });
      }
      if (area != kValues) {
        ImGui::Separator();
        const int field = e.field;
        bool grouped = false;
        for (const auto& g : p.groups) grouped = grouped || g.field == field;
        const Cached& c = CacheFor(s, p);
        grouped = grouped || (c.ok && static_cast<size_t>(field) < c.cache->made_by.size() && c.cache->made_by[static_cast<size_t>(field)] >= 0);
        if (ImGui::MenuItem(TR("Group..."))) OpenGroupDialog(s, p, field);
        if (ImGui::MenuItem(TR("Ungroup"), nullptr, false, grouped)) Ungroup(s, p, field, {});
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  // the empty part of the box: drop at the end
  const ImVec2 rest = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##drop", ImVec2(std::max(1.0f, rest.x), std::max(S(8), rest.y)));
  accept(-1);
  ImGui::EndChild();
  // the box's frame accepts drops too
  accept(-1);
  ImGui::PopID();
  ImGui::EndGroup();
}

// ---- Group Selection: chosen items of a field become a group of a new field ("Region2") ----
bool GroupItems(Document& d, uint32_t sheet, uint32_t id, int field, const std::vector<std::string>& items, std::string* err) {
  const PivotTable* p = d.PivotById(sheet, id);
  if (!p) return false;
  engine::PivotCache cache;
  if (!engine::ReadPivotSource(d.wb(), *p, &cache, err)) return false;
  if (items.empty() || field < 0 || static_cast<size_t>(field) >= cache.columns) {
    *err = TR("Cannot group that selection.");
    return false;
  }
  return d.UpdatePivot(sheet, id, [&](PivotTable& x) {
    model::PivotGroup* g = nullptr;
    for (auto& e : x.groups) {
      if (e.field == field && e.by == model::PivotGroupBy::Items) g = &e;
    }
    if (!g) {
      // a field grouped by periods or ranges keeps that grouping out (Excel asks to ungroup first)
      x.groups.erase(std::remove_if(x.groups.begin(), x.groups.end(), [&](const model::PivotGroup& o) { return o.field == field; }), x.groups.end());
      model::PivotGroup ng;
      ng.field = field;
      ng.by = model::PivotGroupBy::Items;
      x.groups.push_back(ng);
      g = &x.groups.back();
    }
    for (auto& [name, members] : g->named) {
      members.erase(std::remove_if(members.begin(), members.end(), [&](const std::string& m) { return std::find(items.begin(), items.end(), m) != items.end(); }),
                    members.end());
    }
    g->named.erase(std::remove_if(g->named.begin(), g->named.end(), [](const auto& e) { return e.second.empty(); }), g->named.end());
    std::string name;
    for (int k = 1;; ++k) {
      name = TR("Group") + std::to_string(k);
      if (std::none_of(g->named.begin(), g->named.end(), [&](const auto& e) { return e.first == name; })) break;
    }
    g->named.emplace_back(name, items);
  }, err);
}

}  // namespace

bool PivotGroupCells(Document& d, uint32_t sheet, const Rect& sel, std::string* err) {
  const model::Sheet* sh = d.S(sheet);
  if (!sh) return false;
  for (const auto& p : *sh->pivots) {
    if (!p.has_output || !p.output.Intersects(sel)) continue;
    engine::PivotCache cache;
    if (!engine::ReadPivotSource(d.wb(), p, &cache, err)) return false;
    const engine::PivotReport rep = engine::ComputePivot(p, cache);
    int field = -1;
    std::vector<std::string> items;
    std::unordered_set<int> seen;  // each item once, whatever the selection's size
    const Rect in = Overlap(sel, p.output);
    for (uint32_t r = in.r0; r <= in.r1; ++r) {
      for (uint32_t c = in.c0; c <= in.c1; ++c) {
        const uint32_t rr = r - p.output.r0, cc = c - p.output.c0;
        if (rr >= rep.rows || cc >= rep.cols) continue;
        const auto& cell = rep.at(rr, cc);
        std::pair<int, int> fi{-1, -1};
        if (cell.kind == engine::PivotCell::RowLabel && !rep.row_filter[rr].empty() && rep.row_kind[rr] != 4) {
          const auto& rf = rep.row_filter[rr];
          const int lv = engine::PivotLabelLevel(rep, rr, cc);
          fi = lv >= 0 && static_cast<size_t>(lv) < rf.size() ? rf[static_cast<size_t>(lv)] : rf.back();
        } else if (cell.kind == engine::PivotCell::ColLabel) {
          const uint32_t lvl = rr - rep.page_rows - 1;
          if (lvl < rep.col_filter[cc].size()) fi = rep.col_filter[cc][lvl];
        }
        if (fi.first < 0) continue;
        if (field >= 0 && fi.first != field) {
          *err = TR("Cannot group that selection.");
          return false;
        }
        field = fi.first;
        if (seen.insert(fi.second).second) items.push_back(cache.items[static_cast<size_t>(field)][static_cast<size_t>(fi.second)]);
      }
    }
    return GroupItems(d, sheet, p.id, field, items, err);
  }
  *err = TR("Cannot group that selection.");
  return false;
}

// PivotTable Analyze > Active Field: the active cell's field (its caption; typing a new name renames it, as in Excel)
// PivotTable Analyze > PivotTable: "PivotTable Name:" over a box that renames the PivotTable (Excel)
void DrawPivotNameBox(AppState& s, int which, const ImRect& r) {
  PivotUi& u = U();
  const PivotTable* p = ActivePivot(s);
  if (which == 0) {
    ImGui::GetWindowDrawList()->AddText(ImVec2(r.Min.x + S(4), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), ImGui::GetColorU32(ImGuiCol_Text),
                                        TR("PivotTable Name:"));
    return;
  }
  const std::string name = p ? p->name : std::string();
  const ImGuiID id = ImGui::GetID("##PivotNameBox");
  const uint64_t ver = s.snap ? s.snap->version : 0;
  if (ImGui::GetActiveID() != id && (u.name_box_version != ver || name != u.name_box)) {
    std::snprintf(u.name_box, sizeof u.name_box, "%s", name.c_str());
    u.name_box_version = ver;
  }
  ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(4), r.Min.y + std::max(0.0f, (r.GetHeight() - ImGui::GetFrameHeight()) * 0.5f)));
  ImGui::SetNextItemWidth(r.GetWidth() - S(8));
  ImGui::BeginDisabled(!p);
  const bool enter = ImGui::InputText("##PivotNameBox", u.name_box, sizeof u.name_box, ImGuiInputTextFlags_EnterReturnsTrue);
  ui::NoteUiItem("PivotTable Name", ImGui::GetCurrentContext()->LastItemData.Rect);
  ImGui::EndDisabled();
  if (!(enter || ImGui::IsItemDeactivatedAfterEdit()) || !p) return;
  const std::string text = u.name_box;
  u.name_box_version = ~0ull;
  if (text.empty() || text == name) return;
  EditPivot(s, TR("PivotTable Name"), [text](PivotTable& x) { x.name = text; });
}

void DrawActiveFieldBox(AppState& s, const ImRect& r) {
  PivotUi& u = U();
  const PivotTable* p = ActivePivot(s);
  const CellInfo ci = p ? ActiveInfo(s, *p) : CellInfo{};
  std::string name;
  int key = -1;
  if (p && ci.area != kNone && ci.field >= 0) {
    name = FieldName(*p, ci.field);
    key = ci.area * 4096 + ci.pos;
  } else if (p && ci.data >= 0 && static_cast<size_t>(ci.data) < p->data.size()) {
    const Cached& c = CacheFor(s, *p);
    if (c.ok) name = engine::PivotDataCaption(*p, *c.cache, static_cast<size_t>(ci.data));
    key = 100000 + ci.data;
  }
  const char* label = TR("Active Field:");
  const float fy = r.Min.y + std::max(0.0f, (r.GetHeight() - ImGui::GetFrameHeight()) * 0.5f);
  ImGui::GetWindowDrawList()->AddText(ImVec2(r.Min.x + S(4), fy + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), label);
  ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(4) + ImGui::CalcTextSize(label).x + S(6), fy));
  const ImGuiID id = ImGui::GetID("##PivotActiveField");
  const uint64_t ver = s.snap ? s.snap->version : 0;
  if (ImGui::GetActiveID() != id && (u.active_field_version != ver || u.active_field_key != key || name != u.active_field)) {
    std::snprintf(u.active_field, sizeof u.active_field, "%s", name.c_str());
    u.active_field_version = ver;
    u.active_field_key = key;
  }
  ImGui::SetNextItemWidth(std::max(S(60), r.Max.x - ImGui::GetCursorScreenPos().x - S(2)));
  ImGui::BeginDisabled(key < 0);
  const bool enter = ImGui::InputText("##PivotActiveField", u.active_field, sizeof u.active_field, ImGuiInputTextFlags_EnterReturnsTrue);
  ui::NoteUiItem("Active Field", ImGui::GetCurrentContext()->LastItemData.Rect);
  ImGui::EndDisabled();
  if (!(enter || ImGui::IsItemDeactivatedAfterEdit()) || key < 0 || !p) return;
  const std::string text = u.active_field;
  u.active_field_version = ~0ull;
  if (text.empty() || text == name) return;
  const Cached& c = CacheFor(s, *p);
  bool taken = false;
  for (size_t i = 0; c.ok && i < p->data.size(); ++i) taken = taken || (static_cast<int>(i) != ci.data && engine::PivotDataCaption(*p, *c.cache, i) == text);
  for (size_t f = 0; f < p->fields.size(); ++f) taken = taken || (static_cast<int>(f) != ci.field && p->fields[f] == text);
  if (taken) {
    s.ShowMessage("YCGrid", TR("PivotTable field name already exists."));
    return;
  }
  if (ci.area != kNone) {
    const int area = ci.area, pos = ci.pos;
    const std::string src = p->fields[static_cast<size_t>(ci.field)];
    EditPivot(s, TR("Rename"), [area, pos, text, src](PivotTable& x) {
      auto* v = AxisOf(x, area);
      if (v && static_cast<size_t>(pos) < v->size()) (*v)[static_cast<size_t>(pos)].caption = text == src ? std::string() : text;
    });
  } else {
    const int d = ci.data;
    EditPivot(s, TR("Rename"), [d, text](PivotTable& x) {
      if (static_cast<size_t>(d) < x.data.size()) x.data[static_cast<size_t>(d)].caption = text;
    });
  }
}

void UpdatePivotContext(AppState& s) {
  PivotUi& u = U();
  auto& cx = ui::ribbon::Context();
  // a dialog or a drop-down opened from a report's (or a PivotChart's) button keeps working on that PivotTable while
  // the active cell is elsewhere
  const uint32_t keep_sheet = u.sheet, keep_id = u.id;
  const bool keep = s.dialog != Dialog::None || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  u.id = 0;
  u.chart_mode = false;
  if (s.snap) {
    const model::Sheet* sh = s.FocusSheet();
    const auto& sel = s.FocusView().sel;
    if (sh) {
      for (const auto& p : *sh->pivots) {
        if (engine::PivotArea(p).Contains(sel.act_r, sel.act_c)) {
          u.sheet = s.focus_sheet;
          u.id = p.id;
        }
      }
    }
    // a selected PivotChart: its PivotTable (the pane shows Legend (Series) / Axis (Categories))
    uint32_t csheet = 0, cid = 0;
    if (SelectedChartObj(s, &csheet, &cid)) {
      if (const model::Sheet* cs = s.snap->Sheet(csheet)) {
        for (const auto& ch : *cs->charts) {
          if (ch.id != cid || ch.pivot.empty()) continue;
          for (const auto& sc : s.snap->wb.sheets) {
            for (const auto& p : *sc->pivots) {
              if (p.name == ch.pivot) {
                u.sheet = sc->id;
                u.id = p.id;
                u.chart_mode = true;
              }
            }
          }
        }
      }
    }
  }
  cx.pivot = u.id != 0;
  if (!u.id && keep && keep_id) {
    u.sheet = keep_sheet;
    u.id = keep_id;
  }
  if (u.pending && (!u.id || u.pending->id != u.id)) u.pending.reset();
  if (const PivotTable* p = ActivePivot(s)) {
    cx.pv_row_headers = p->style_row_headers;
    cx.pv_col_headers = p->style_col_headers;
    cx.pv_banded_rows = p->style_banded_rows;
    cx.pv_banded_cols = p->style_banded_cols;
    cx.pv_buttons = p->show_buttons;
    cx.pv_headers = p->field_captions;
    cx.pv_chart_buttons = p->chart_buttons;
    cx.pv_enable_selection = p->enable_selection;
    if (auto* c = ui::Commands().Find("PivotEnableSelection")) c->pressed = p->enable_selection;
  }
  if (!cx.pivot_field_box) cx.pivot_field_box = [&s](const ImRect& r) { DrawActiveFieldBox(s, r); };
  if (!cx.pivot_name_box) cx.pivot_name_box = [&s](int which, const ImRect& r) { DrawPivotNameBox(s, which, r); };
}

void RegisterPivotCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon = nullptr, const char* desc = "", const char* shortcut = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    c.shortcut = shortcut;
    return c;
  };
  auto open_create = [&s](bool chart) {
    PivotUi& u = U();
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    Rect r = SelRect(s);
    std::string src;
    // a table: its name; a single cell: the current region
    const auto& sel = s.FocusView().sel;
    if (const model::Table* t = model::TableAt(s.snap->wb, s.focus_sheet, sel.act_r, sel.act_c)) {
      src = t->name;
    } else {
      if (r.IsCell()) r = ExpandToRegion(*sh, r);
      src = RangeText(s, s.focus_sheet, r);
    }
    std::snprintf(u.source, sizeof(u.source), "%s", src.c_str());
    u.dest_new = 1;
    u.with_chart = chart;
    std::snprintf(u.dest, sizeof(u.dest), "%s", "");
    s.OpenDialog(Dialog::CreatePivot);
  };
  R.Add(make("InsertPivotTable", TR("PivotTable"), ui::icon::PivotTable, TR("Summarize data with a PivotTable."))).execute = [open_create](ui::Command&) { open_create(false); };
  R.Add(make("InsertPivotChart", TR("PivotChart"), ui::icon::PivotChart, TR("Summarize data with a PivotTable and its chart."))).execute = [open_create](ui::Command&) { open_create(true); };
  R.Add(make("PivotRefresh", TR("Refresh"), ui::icon::Refresh, TR("Get the latest data from the source of this PivotTable."), "Alt+F5")).execute = [&s](ui::Command&) {
    const uint32_t sheet = U().sheet, id = U().id;
    if (!id) return;
    PivotDocOp(s, TR("Refresh"), [sheet, id](Document& d, std::string* err) { return d.RefreshPivot(sheet, id, err); });
  };
  R.Add(make("RefreshAll", TR("Refresh\nAll"), ui::icon::RefreshAll, TR("Refresh every PivotTable in the workbook."), "Ctrl+Alt+F5")).execute = [&s](ui::Command&) {
    s.engine->PostUser(TR("Refresh All"), [](Document& d) { d.RefreshAllPivots(); });
    s.SetStatus(TR("Refreshed all PivotTables"));
  };
  R.Add(make("PivotChangeSource", TR("Change Data\nSource"), ui::icon::Data, TR("Change the range of data this PivotTable summarizes."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    std::snprintf(U().source, sizeof(U().source), "%s", p->src_table.empty() ? RangeText(s, p->src_sheet, p->src).c_str() : p->src_table.c_str());
    s.OpenDialog(Dialog::PivotSource);
  };
  R.Add(make("PivotMove", TR("Move\nPivotTable"), ui::icon::Move, TR("Move this PivotTable to another place."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    std::snprintf(U().dest, sizeof(U().dest), "%s", core::CellA1(p->r0, p->c0, true, true).c_str());
    U().move_new = 0;
    s.OpenDialog(Dialog::PivotMove);
  };
  R.Add(make("PivotRename", TR("PivotTable\nName"), ui::icon::NameManager, TR("Rename this PivotTable."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    std::snprintf(U().name, sizeof(U().name), "%s", p->name.c_str());
    s.OpenDialog(Dialog::PivotRename);
  };
  R.Add(make("PivotOptions", TR("Options"), ui::icon::Options, TR("PivotTable Options: layout, totals, display, data."))).execute = [&s](ui::Command&) {
    if (const PivotTable* p = ActivePivot(s)) OpenOptions(s, *p);
  };
  R.Add(make("PivotChart", TR("PivotChart"), ui::icon::PivotChart, TR("A chart of this PivotTable that follows its fields and filters."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    // the Insert Chart dialog (every chart type, previews of this PivotTable's data); OK puts the chart beside the report
    const model::Sheet* sh = s.snap->Sheet(U().sheet);
    const Rect a = engine::PivotArea(*p);
    model::ChartObj ch;
    ch.type = model::ChartType::ColumnClustered;
    ch.pivot = p->name;
    ch.left = sh ? static_cast<double>(sh->cols->Offset(static_cast<int64_t>(a.c1) + 2)) * 0.75 : 0;
    ch.top = sh ? static_cast<double>(sh->rows->Offset(a.r0)) * 0.75 : 0;
    engine::PivotChartSeries(s.snap->wb, *p, &ch);
    OpenPivotChartDialog(s, U().sheet, ch);
  };
  R.Add(make("InsertSlicer", TR("Insert\nSlicer"), ui::icon::Filter, TR("Filter a PivotTable or a table with buttons for the items of a field."))).execute = [&s](ui::Command&) {
    PivotUi& u = U();
    u.pick_timeline = false;
    u.pick_table.clear();
    if (const PivotTable* p = ActivePivot(s)) {
      u.slicer_pick.assign(p->fields.size(), 0);
      s.OpenDialog(Dialog::InsertSlicer);
      return;
    }
    // a table: its columns (Insert > Slicer, Table Design > Insert Slicer)
    const auto& sel = s.FocusView().sel;
    if (const model::Table* t = s.snap ? model::TableAt(s.snap->wb, s.focus_sheet, sel.act_r, sel.act_c) : nullptr) {
      u.pick_table = t->name;
      u.pick_table_sheet = s.focus_sheet;
      u.slicer_pick.assign(t->range.Cols(), 0);
      s.OpenDialog(Dialog::InsertSlicer);
      return;
    }
    s.ShowMessage("YCGrid", TR("Select a cell in a PivotTable or a table to insert a slicer."));
  };
  R.Add(make("SlicerSettings", TR("Slicer\nSettings"), ui::icon::Options, TR("The slicer's caption, header, item sorting and items with no data."))).execute =
      [&s](ui::Command&) { OpenSlicerSettings(s); };
  R.Add(make("InsertTimeline", TR("Insert\nTimeline"), ui::icon::Timeline, TR("Filter the PivotTable by a date range: drag across years, quarters, months or days."))).execute =
      [&s](ui::Command&) {
        const PivotTable* p = ActivePivot(s);
        if (!p) {
          s.ShowMessage("YCGrid", TR("Timelines filter PivotTables: select a cell in a PivotTable to insert a timeline."));
          return;
        }
        U().slicer_pick.assign(p->fields.size(), 0);
        U().pick_timeline = true;
        U().pick_table.clear();
        s.OpenDialog(Dialog::InsertSlicer);
      };
  R.Add(make("PivotCalcItem", TR("Calculated\nItem"), ui::icon::Formula, TR("Add an item made of a field's other items (=East+West)."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    if (p->from_model) {
      s.ShowMessage("YCGrid", TR("This PivotTable reads the Data Model, which YCGrid no longer has"));
      return;
    }
    U().calc_name[0] = 0;
    std::snprintf(U().calc_formula, sizeof(U().calc_formula), "%s", "=");
    // the first row / column field
    U().item_field = !p->rows.empty() ? p->rows[0].field : (!p->cols.empty() ? p->cols[0].field : 0);
    s.OpenDialog(Dialog::PivotCalcItem);
  };
  R.Add(make("PivotCalcField", TR("Calculated\nField"), ui::icon::Formula, TR("Add a field computed from other fields (=Sales/Qty)."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    if (p->from_model) {
      s.ShowMessage("YCGrid", TR("This PivotTable reads the Data Model, which YCGrid no longer has"));
      return;
    }
    U().calc_name[0] = 0;
    std::snprintf(U().calc_formula, sizeof(U().calc_formula), "%s", "=");
    s.OpenDialog(Dialog::PivotCalcField);
  };
  // ---- Report Connections / Filter Connections, Enable Selection, Active Field, Recommended PivotTables ----
  R.Add(make("SlicerReportConnections", TR("Report\nConnections"), ui::icon::Connections, TR("Choose the PivotTables this slicer filters."))).execute = [&s](ui::Command&) {
    OpenReportConnections(s);
  };
  R.Add(make("PivotFilterConnections", TR("Filter\nConnections"), ui::icon::Connections, TR("Choose the slicers and timelines that filter this PivotTable."))).execute =
      [&s](ui::Command&) {
        const PivotTable* p = ActivePivot(s);
        if (!p) return;
        PivotUi& u = U();
        u.rc_names.clear();
        u.rc_on.clear();
        for (const auto& sc : s.snap->wb.sheets) {
          for (const auto& sl : *sc->slicers) {
            if (!sl.table.empty()) continue;
            const PivotTable* first = PivotNamed(s, sl.pivot);
            if (!first || !SameSource(*first, *p) || !HasFieldNamed(*p, SlicerFieldName(s, sl))) continue;
            u.rc_names.push_back(std::to_string(sc->id) + ":" + std::to_string(sl.id));
            u.rc_on.push_back(SlicerConnectedTo(sl, p->name) ? 1 : 0);
          }
        }
        if (u.rc_names.empty()) {
          s.ShowMessage("YCGrid", TR("There are no slicers or timelines that can filter this PivotTable. Insert one first."));
          return;
        }
        s.OpenDialog(Dialog::PivotFilterConnections);
      };
  R.Add(make("PivotEnableSelection", TR("Enable Selection"), nullptr, TR("Click the edge of an item's label to select the item's labels and values."))).execute =
      [&s](ui::Command&) {
        const PivotTable* p = ActivePivot(s);
        if (!p) return;
        const bool v = !p->enable_selection;
        EditPivot(s, TR("Enable Selection"), [v](PivotTable& x) { x.enable_selection = v; });
      };
  if (auto* c = R.Find("PivotEnableSelection")) c->toggle = true;
  R.Add(make("PivotActiveField", TR("Active Field:"), nullptr, TR("The name of the active field: type a new name to rename it."))).execute = [](ui::Command&) {};
  R.Add(make("PivotCalcMenu", TR("Fields, Items,\n& Sets"), ui::icon::Formula, TR("Calculated fields and items."))).execute = [](ui::Command&) {};
  R.Add(make("RecommendedPivotTables", TR("Recommended\nPivotTables"), ui::icon::DataTable, TR("Pick from PivotTables that summarize your data."))).execute =
      [&s](ui::Command&) {
        PivotUi& u = U();
        const model::Sheet* sh = s.FocusSheet();
        if (!sh) return;
        std::string src;
        if (const PivotTable* p = ActivePivot(s)) {
          src = p->src_table.empty() ? RangeText(s, p->src_sheet, p->src) : p->src_table;
        } else {
          const auto& sel = s.FocusView().sel;
          if (const model::Table* t = model::TableAt(s.snap->wb, s.focus_sheet, sel.act_r, sel.act_c)) {
            src = t->name;
          } else {
            Rect r = SelRect(s);
            if (r.IsCell()) r = ExpandToRegion(*sh, r);
            src = RangeText(s, s.focus_sheet, r);
          }
        }
        u.rec_source = src;
        u.rec_pick = 0;
        s.OpenDialog(Dialog::RecommendedPivots);
      };
  // ---- Timeline tab > Show ----
  struct TlShow {
    const char* id;
    const char* label;
    int bit;  // 0 = the header (Slicer Settings' Display header)
  };
  static const TlShow kTl[] = {{"TimelineShowHeader", TR("Header"), 0},
                               {"TimelineShowSelectionLabel", TR("Selection Label"), 1},
                               {"TimelineShowScrollbar", TR("Scrollbar"), 2},
                               {"TimelineShowTimeLevel", TR("Time Level"), 4}};
  for (const TlShow& t : kTl) {
    const int bit = t.bit;
    R.Add(make(t.id, t.label)).execute = [&s, bit](ui::Command&) {
      EditSelectedSlicer(s, TR("Timeline"), [bit](model::Slicer& x) {
        if (bit == 0) x.show_header = !x.show_header;
        else x.tl_hide ^= bit;
      });
    };
    if (auto* c = R.Find(t.id)) c->toggle = true;
  }
  // ---- Paste (Home > Paste, Ctrl+V): a copied slicer is pasted at the active cell, as Excel pastes a copied object ----
  if (ui::Command* paste = R.Find("Paste")) {
    auto orig = paste->execute;
    paste->execute = [&s, orig](ui::Command& self) {
      if (SlicerClipPending() && s.FocusSheet() && !s.edit.active) {
        const model::Sheet* sh = s.FocusSheet();
        const auto& sel = s.FocusView().sel;
        PasteSlicerAt(s, s.focus_sheet, U().clip, static_cast<double>(sh->cols->Offset(sel.act_c)) * 0.75, static_cast<double>(sh->rows->Offset(sel.act_r)) * 0.75);
        return;
      }
      if (orig) orig(self);
    };
  }
  // ---- Actions: Clear, Select ----
  R.Add(make("PivotClearMenu", TR("Clear"), ui::icon::Clear, TR("Remove the fields or the filters of this PivotTable."))).execute = [](ui::Command&) {};
  R.Add(make("PivotClear", TR("Clear All"), ui::icon::Clear, TR("Remove every field from this PivotTable."))).execute = [&s](ui::Command&) {
    EditPivot(s, TR("Clear All"), [](PivotTable& x) {
      x.rows.clear();
      x.cols.clear();
      x.pages.clear();
      x.data.clear();
    });
  };
  R.Add(make("PivotClearFilters", TR("Clear Filters"), ui::icon::Filter, TR("Show every item again: clear the filters of every field, slicer and timeline."))).execute =
      [&s](ui::Command&) {
        EditPivot(s, TR("Clear Filters"), [](PivotTable& x) {
          for (auto* axis : {&x.rows, &x.cols, &x.pages}) {
            for (auto& a : *axis) {
              a.hidden.clear();
              a.page_item.clear();
              a.label_filter = {};
              a.value_filter = {};
            }
          }
          x.slicer_filters.clear();
          x.date_filters.clear();
        });
      };
  R.Add(make("PivotSelectMenu", TR("Select"), ui::icon::SelectCells, TR("Select the labels, the values or the whole PivotTable."))).execute = [](ui::Command&) {};
  struct SelOpt {
    const char* id;
    const char* label;
    int what;  // 0 labels and values, 1 values, 2 labels, 3 entire
  };
  static const SelOpt kSel[] = {{"PivotSelectLabelsValues", TR("Labels and Values"), 0}, {"PivotSelectValues", TR("Values"), 1},
                                {"PivotSelectLabels", TR("Labels"), 2}, {"PivotSelectEntire", TR("Entire PivotTable"), 3}};
  for (const SelOpt& o : kSel) {
    const int what = o.what;
    R.Add(make(o.id, o.label)).execute = [&s, what](ui::Command&) {
      const PivotTable* p = ActivePivot(s);
      if (!p || !p->has_output) return;
      const Cached& c = CacheFor(s, *p);
      if (!c.ok) return;
      const auto& rep = *c.rep;
      const Rect out = p->output;
      Rect r = out;
      const uint32_t top = out.r0 + rep.page_rows;
      if (what == 0) r = Rect{top, out.c0, out.r1, out.c1};
      if (what == 1) r = Rect{top + rep.header_rows, out.c0 + rep.label_cols, out.r1, out.c1};
      if (what == 2) r = Rect{top, out.c0, out.r1, out.c0 + std::max<uint32_t>(rep.label_cols, 1) - 1};
      if (r.r0 > r.r1 || r.c0 > r.c1) return;
      render::SelectRange(s.FocusView(), r);
    };
  }
  R.Add(make("PivotDelete", TR("Delete\nPivotTable"), ui::icon::Delete, TR("Delete this PivotTable and its report."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = U().sheet, id = U().id;
    if (!id) return;
    s.engine->PostUser(TR("Delete PivotTable"), [sheet, id](Document& d) { d.DeletePivot(sheet, id, true); });
  };
  // ---- Active Field ----
  R.Add(make("PivotFieldSettings", TR("Field\nSettings"), ui::icon::TableDesign, TR("Subtotals, layout and filters of the active field."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    const CellInfo ci = ActiveInfo(s, *p);
    if (ci.data >= 0 && ci.area == kNone) {
      OpenValueField(s, *p, ci.data);
    } else if (ci.area != kNone) {
      OpenFieldSettings(s, *p, ci.area, ci.pos);
    } else {
      s.ShowMessage("YCGrid", TR("Select a cell of the PivotTable's field first."));
    }
  };
  // Expand / Collapse Field: the active field's items (the innermost field: the field above it)
  auto expand_field = [&s](bool collapse) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    const CellInfo ci = ActiveInfo(s, *p);
    if (ci.area != kRows && ci.area != kColumns) return;
    const auto& axis = *AxisOf(*p, ci.area);
    int pos = ci.pos;
    if (pos + 1 >= static_cast<int>(axis.size())) pos = static_cast<int>(axis.size()) - 2;  // the innermost: drill up
    if (pos < 0) return;
    const int area = ci.area;
    EditPivot(s, collapse ? TR("Collapse Field") : TR("Expand Field"), [area, pos, collapse](PivotTable& x) {
      auto& v = *AxisOf(x, area);
      if (static_cast<size_t>(pos) >= v.size()) return;
      v[static_cast<size_t>(pos)].collapse_all = collapse;
      v[static_cast<size_t>(pos)].collapsed.clear();
      if (!collapse) {
        for (auto& f : v) f.collapse_all = false, f.collapsed.clear();
      }
    });
  };
  R.Add(make("PivotExpandField", TR("Expand Field"), ui::icon::Add, TR("Show the details of every item of the active field."))).execute = [expand_field](ui::Command&) { expand_field(false); };
  R.Add(make("PivotCollapseField", TR("Collapse Field"), ui::icon::HideDetail, TR("Hide the details of every item of the active field."))).execute = [expand_field](ui::Command&) { expand_field(true); };
  // ---- Group ----
  R.Add(make("PivotGroupSelection", TR("Group\nSelection"), ui::icon::Group, TR("Group the selected items into a new field."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    const CellInfo ci = ActiveInfo(s, *p);
    if (ci.area == kNone || ci.item < 0) {
      s.ShowMessage("YCGrid", TR("Cannot group that selection."));
      return;
    }
    // dates and numbers: by periods / ranges (Grouping); text: the selected items
    const Cached& c = CacheFor(s, *p);
    bool numeric = false;
    if (c.ok && static_cast<size_t>(ci.field) < c.cache->columns) {
      for (size_t k = 0; k < c.cache->source_records && !numeric; ++k) numeric = c.cache->records[k][static_cast<size_t>(ci.field)].IsNumeric();
    }
    const std::vector<std::string> items = SelectedItems(s, *p, ci.field);
    if (numeric && items.size() <= 1) {
      OpenGroupDialog(s, *p, ci.field);
      return;
    }
    const uint32_t sheet = U().sheet, id = U().id;
    const int field = ci.field;
    PivotDocOp(s, TR("Group"), [sheet, id, field, items](Document& d, std::string* err) { return GroupItems(d, sheet, id, field, items, err); });
  };
  R.Add(make("PivotUngroup", TR("Ungroup"), ui::icon::Ungroup, TR("Ungroup the active field's items."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    const CellInfo ci = ActiveInfo(s, *p);
    if (ci.area == kNone) return;
    const Cached& c = CacheFor(s, *p);
    std::string item;
    if (c.ok && ci.item >= 0) item = c.cache->items[static_cast<size_t>(ci.field)][static_cast<size_t>(ci.item)];
    Ungroup(s, *p, ci.field, item);
  };
  R.Add(make("PivotGroupField", TR("Group\nField"), ui::icon::GroupDates, TR("Group the active field's dates or numbers."))).execute = [&s](ui::Command&) {
    const PivotTable* p = ActivePivot(s);
    if (!p) return;
    const CellInfo ci = ActiveInfo(s, *p);
    if (ci.area == kNone) {
      s.ShowMessage("YCGrid", TR("Cannot group that selection."));
      return;
    }
    OpenGroupDialog(s, *p, ci.field);
  };
  // ---- Show ----
  R.Add(make("PivotFieldList", TR("Field\nList"), ui::icon::Panel, TR("Show or hide the PivotTable Fields pane."))).execute = [](ui::Command& c) {
    U().field_list = !U().field_list;
    c.pressed = U().field_list;
  };
  RegisterPane("pivot_fields", "PivotFieldList", []() -> bool& { return U().field_list; });
  if (auto* c = R.Find("PivotFieldList")) {
    c->toggle = true;
    c->pressed = true;
  }
  // a PivotTable option the button turns on and off
  auto add = [&s, &R, make](const char* id, const char* label, const char* icon, const char* desc, bool PivotTable::*member) {
    R.Add(make(id, label, icon, desc)).execute = [&s, member, label](ui::Command&) {
      const PivotTable* p = ActivePivot(s);
      if (!p) return;
      const bool v = !(p->*member);
      EditPivot(s, label, [member, v](PivotTable& x) { x.*member = v; });
    };
    if (auto* c = R.Find(id)) c->toggle = true;
  };
  add("PivotToggleButtons", TR("+/- Buttons"), ui::icon::Add, TR("Show or hide the buttons that expand and collapse items."), &PivotTable::show_buttons);
  add("PivotToggleFieldHeaders", TR("Field\nHeaders"), ui::icon::Filter, TR("Show or hide the field captions and filter buttons."), &PivotTable::field_captions);
  add("PivotChartButtons", TR("Field\nButtons"), ui::icon::ChartColumn, TR("Show or hide the field buttons on this PivotTable's charts."), &PivotTable::chart_buttons);
  // ---- Design ----
  R.Add(make("PivotSubtotalsMenu", TR("Subtotals"), ui::icon::AutoSum, TR("Show or hide subtotals."))).execute = [](ui::Command&) {};
  struct Opt {
    const char* id;
    const char* label;
    int v;
  };
  static const Opt kSub[] = {{"PivotSubtotalsNone", TR("Do Not Show Subtotals"), 0}, {"PivotSubtotalsBottom", TR("Show All Subtotals at Bottom of Group"), 2},
                             {"PivotSubtotalsTop", TR("Show All Subtotals at Top of Group"), 1}};
  for (const Opt& o : kSub) {
    const int v = o.v;
    R.Add(make(o.id, o.label)).execute = [&s, v](ui::Command&) {
      EditPivot(s, TR("Subtotals"), [v](PivotTable& x) {
        x.subtotals = v;
        for (auto& f : x.rows) f.sub_top = -1;
      });
    };
  }
  R.Add(make("PivotGrandMenu", TR("Grand\nTotals"), ui::icon::Calculator, TR("Show or hide grand totals."))).execute = [](ui::Command&) {};
  static const Opt kGrand[] = {{"PivotGrandOff", TR("Off for Rows and Columns"), 0}, {"PivotGrandOn", TR("On for Rows and Columns"), 3},
                               {"PivotGrandRows", TR("On for Rows Only"), 1}, {"PivotGrandCols", TR("On for Columns Only"), 2}};
  for (const Opt& o : kGrand) {
    const int v = o.v;
    // "rows" = the grand total column at the right of every row (Excel's ColumnGrand / RowGrand naming)
    R.Add(make(o.id, o.label)).execute = [&s, v](ui::Command&) {
      EditPivot(s, TR("Grand Totals"), [v](PivotTable& x) {
        x.col_grand = (v & 1) != 0;
        x.row_grand = (v & 2) != 0;
      });
    };
  }
  R.Add(make("PivotLayoutMenu", TR("Report\nLayout"), ui::icon::PivotLayout, TR("Compact, outline or tabular form."))).execute = [](ui::Command&) {};
  static const Opt kLayout[] = {{"PivotLayoutCompact", TR("Show in Compact Form"), 0}, {"PivotLayoutOutline", TR("Show in Outline Form"), 1},
                                {"PivotLayoutTabular", TR("Show in Tabular Form"), 2}, {"PivotLayoutRepeat", TR("Repeat All Item Labels"), 3},
                                {"PivotLayoutNoRepeat", TR("Do Not Repeat Item Labels"), 4}};
  for (const Opt& o : kLayout) {
    const int v = o.v;
    R.Add(make(o.id, o.label)).execute = [&s, v](ui::Command&) {
      EditPivot(s, TR("Report Layout"), [v](PivotTable& x) {
        if (v >= 3) {
          for (auto& f : x.rows) f.repeat_labels = v == 3;
          return;
        }
        x.layout = v;
        for (auto& f : x.rows) f.form = -1;
        if (v == 2 && x.subtotals == 1) x.subtotals = 2;  // tabular form has no group rows
      });
    };
  }
  R.Add(make("PivotBlankRowsMenu", TR("Blank\nRows"), ui::icon::Rows, TR("An empty row after each item."))).execute = [](ui::Command&) {};
  static const Opt kBlank[] = {{"PivotBlankRowsInsert", TR("Insert Blank Line after Each Item"), 1}, {"PivotBlankRowsRemove", TR("Remove Blank Line after Each Item"), 0}};
  for (const Opt& o : kBlank) {
    const bool on = o.v != 0;
    R.Add(make(o.id, o.label)).execute = [&s, on](ui::Command&) {
      EditPivot(s, TR("Blank Rows"), [on](PivotTable& x) {
        for (size_t i = 0; i < x.rows.size(); ++i) x.rows[i].blank_line = on && i + 1 < x.rows.size();
      });
    };
  }
  // PivotTable Styles (Design > PivotTable Styles); every Light / Medium / Dark style works from Python (TableStyle2)
  R.Add(make("PivotStylesMenu", TR("PivotTable\nStyles"), ui::icon::FormatAsTable, TR("Choose a look for the PivotTable."))).execute = [](ui::Command&) {};
  // every style of Excel's gallery (Light 1-28, Medium 1-28, Dark 1-28) and None, each a tile picturing its look
  std::vector<std::string> styles = engine::PivotStyleNames();
  styles.push_back("PivotStyleNone");
  std::vector<ui::ribbon::GalleryTile> tiles;
  for (const std::string& st : styles) {
    std::string label = st.substr(10);
    const char* section = TR("Light");
    const std::pair<const char*, const char*> kFam[] = {{"Light", TR("Light")}, {"Medium", TR("Medium")}, {"Dark", TR("Dark")}};
    for (const auto& [fam, shown] : kFam) {
      if (label.rfind(fam, 0) == 0 && label.size() > std::strlen(fam)) label = std::string(shown) + " " + label.substr(std::strlen(fam)), section = shown;
    }
    if (label == "None") label = TR("None");
    const std::string name = st;
    R.Add(make(st.c_str(), label.c_str())).execute = [&s, name](ui::Command&) { EditPivot(s, TR("PivotTable Style"), [name](PivotTable& x) { x.style = name; }); };
    if (st == "PivotStyleNone") continue;  // in the footer, as Excel's Clear
    ui::ribbon::GalleryTile g;
    g.cmd = st;
    g.section = section;
    const engine::PivotStyleLook look = engine::PivotStyleFor(st);
    // a small report: header row, rows with bands and lines, the grand total
    g.picture = [look](ImDrawList* dl, const ImRect& r) {
      auto col = [](uint32_t argb) { return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255); };
      const ImRect in(ImVec2(r.Min.x + S(3), r.Min.y + S(3)), ImVec2(r.Max.x - S(3), r.Max.y - S(3)));
      dl->AddRectFilled(in.Min, in.Max, IM_COL32_WHITE);
      const int rows = 6;
      const float rh = in.GetHeight() / rows;
      for (int i = 0; i < rows; ++i) {
        const ImVec2 a(in.Min.x, in.Min.y + rh * i), b(in.Max.x, in.Min.y + rh * (i + 1));
        const bool head = i == 0, grand = i == rows - 1;
        const uint32_t fill = head ? look.header_fill : grand ? look.grand_fill : (i % 2 == 0 ? look.band_fill : 0);
        if (fill) dl->AddRectFilled(a, b, col(fill));
        const uint32_t ink = head && look.header_font ? look.header_font : 0xFF404040;
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * 0.15f, (a.y + b.y) * 0.5f), ImVec2(a.x + (b.x - a.x) * 0.45f, (a.y + b.y) * 0.5f), col(ink),
                    (head && look.header_bold) || (grand && look.grand_bold) ? S(2) : 1.0f);
        if (look.body_line && !head && !grand) dl->AddLine(ImVec2(a.x, b.y), b, col(look.body_line));
        if (grand && look.grand_line) dl->AddLine(a, ImVec2(b.x, a.y), col(look.grand_line), S(1.5f));
        if (head && look.header_line) dl->AddLine(ImVec2(a.x, b.y), b, col(look.header_line));
      }
      dl->AddRect(in.Min, in.Max, IM_COL32(0xD0, 0xD0, 0xD0, 255));
    };
    tiles.push_back(std::move(g));
  }
  ui::ribbon::SetGallery("PivotStyles", std::move(tiles));
  ui::ribbon::SetGalleryLayout("PivotStyles", 7, 61, 46);
  ui::ribbon::SetGalleryFooter("PivotStyles", {"PivotStyleNone"});
  add("PivotStyleRowHeaders", TR("Row Headers"), nullptr, TR("Format the first column of the PivotTable."), &PivotTable::style_row_headers);
  add("PivotStyleColHeaders", TR("Column Headers"), nullptr, TR("Format the header row of the PivotTable."), &PivotTable::style_col_headers);
  add("PivotStyleBandedRows", TR("Banded Rows"), nullptr, TR("Every other row in a different color."), &PivotTable::style_banded_rows);
  add("PivotStyleBandedCols", TR("Banded Columns"), nullptr, TR("Every other column in a different color."), &PivotTable::style_banded_cols);
}

bool PivotDoubleClick(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || sh->pivots->empty()) return false;
  const auto& sel = s.FocusView().sel;
  const uint32_t sheet = s.focus_sheet, r = sel.act_r, c = sel.act_c;
  const PivotTable* p = nullptr;
  for (const auto& x : *sh->pivots) {
    if (x.has_output && x.output.Contains(r, c)) p = &x;
  }
  if (!p) return false;
  U().sheet = sheet;
  U().id = p->id;
  const Cached& cc = CacheFor(s, *p);
  if (!cc.ok) return true;
  const CellInfo ci = InfoAt(s, *p, r, c);
  // an outer item's label: expand / collapse it
  for (const auto& t : cc.rep->toggles) {
    if (t.r != ci.r || t.c != ci.c) continue;
    const std::string item = cc.cache->items[static_cast<size_t>(t.field)][static_cast<size_t>(t.item)];
    const int field = t.field;
    const bool collapse = !t.collapsed;
    EditPivot(s, collapse ? TR("Collapse") : TR("Expand"), [field, item, collapse](PivotTable& x) {
      for (auto* axis : {&x.rows, &x.cols}) {
        for (auto& a : *axis) {
          if (a.field == field) engine::SetPivotItemCollapsed(a, item, collapse);
        }
      }
    });
    return true;
  }
  if (ci.caption && ci.data >= 0) {
    OpenValueField(s, *p, ci.data);
    return true;
  }
  if (ci.kind == engine::PivotCell::Header || ci.kind == engine::PivotCell::PageName) {
    if (ci.area != kNone) OpenFieldSettings(s, *p, ci.area, ci.pos);
    return true;
  }
  // the innermost item's label: Show Detail (a field to add below it)
  if ((ci.kind == engine::PivotCell::RowLabel || ci.kind == engine::PivotCell::ColLabel) && ci.area != kNone && ci.item >= 0) {
    PivotUi& u = U();
    u.sd_area = ci.area;
    u.sd_pos = ci.pos;
    u.sd_pick = -1;
    s.OpenDialog(Dialog::PivotShowDetail);
    return true;
  }
  if (ci.kind != engine::PivotCell::Data) return true;
  std::string err;
  const uint32_t ns = s.engine->Call([&](Document& d) {
    d.Begin(TR("Show Details"));
    const uint32_t v = d.PivotShowDetails(sheet, r, c, &err);
    d.Commit();
    return v;
  });
  if (ns) {
    s.engine->Sync();
    s.snap = s.engine->Current();
    ActivateSheet(s, ns);
  } else if (!err.empty()) {
    s.ShowError("YCGrid", err);
  }
  return true;
}

void DrawPivotFieldList(AppState& s) {
  PivotUi& u = U();
  const PivotTable* live = ActivePivot(s);
  if (!live || !u.field_list) return;
  // Defer Layout Update: the pane shows the changes waiting for Update
  const PivotTable& view = u.defer && u.pending && u.pending->id == live->id ? *u.pending : *live;
  const PivotTable* p = &view;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  // inside the window: a pane taller than the screen hid its Rows and Values areas below its edge
  const float top = std::min(S(200), vp->WorkSize.y * 0.25f);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(330), vp->WorkPos.y + top), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(S(320), std::max(S(300), std::min(S(600), vp->WorkSize.y - top - S(8)))), ImGuiCond_FirstUseEver);
  bool open = true;
  // it appears when a click enters a report: without taking the focus (that would close the right-click menu just opened)
  const char* title = u.chart_mode ? TR("PivotChart Fields###yc_pivot_fields") : TR("PivotTable Fields###yc_pivot_fields");
  if (!ImGui::Begin(title, &open, ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    if (!open) u.field_list = false;
    return;
  }
  if (!open) u.field_list = false;
  ImGui::TextDisabled("%s", p->name.c_str());
  const float line_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  ImGui::TextUnformatted(TR("Choose fields to add to report:"));
  {
    // Tools (the gear): the pane's layout and the order of the field list
    const float gw = ImGui::GetFrameHeight();
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), line_right - gw));
    ImGui::PushFont(ui::F().icons, ImGui::GetFontSize() * 0.95f);  // the glyph is in the icon font (it showed as a box)
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    const bool tools = ImGui::Button(ui::icon::Options, ImVec2(gw, gw));
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (tools) ImGui::OpenPopup("##pane_tools");
    ui::NoteUiItem("Field List Tools", ImGui::GetCurrentContext()->LastItemData.Rect);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Tools"));
    if (ImGui::BeginPopup("##pane_tools")) {
      static const char* kLayouts[] = {TR("Fields Section and Areas Section Stacked"), TR("Fields Section and Areas Section Side-By-Side"), TR("Fields Section Only"),
                                       TR("Areas Section Only (2 by 2)"), TR("Areas Section Only (1 by 4)")};
      for (int k = 0; k < 5; ++k) {
        if (ImGui::MenuItem(kLayouts[k], nullptr, u.pane_layout == k)) u.pane_layout = k;
      }
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Sort A to Z"), nullptr, live->field_list_order == 1)) EditPivot(s, TR("Field List"), [](PivotTable& x) { x.field_list_order = 1; });
      if (ImGui::MenuItem(TR("Sort in Data Source Order"), nullptr, live->field_list_order == 0)) EditPivot(s, TR("Field List"), [](PivotTable& x) { x.field_list_order = 0; });
      ImGui::EndPopup();
    }
  }
  const float w = ImGui::GetContentRegionAvail().x;
  const bool show_fields = u.pane_layout <= 2, show_areas = u.pane_layout != 2, side = u.pane_layout == 1;
  const float fields_w = side ? (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f : w;
  if (side) ImGui::BeginGroup();
  if (show_fields) {
  ImGui::SetNextItemWidth(fields_w);
  ImGui::InputTextWithHint("##search", TR("Search"), u.search, sizeof(u.search));
  std::string q = u.search;
  std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  const float list_h = u.pane_layout == 0 ? S(160) : std::max(S(160), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2 - S(8));
  ImGui::BeginChild("##fields", ImVec2(fields_w, list_h), ImGuiChildFlags_Borders);
  std::vector<size_t> order(p->fields.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  if (p->field_list_order == 1) {
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p->fields[a] < p->fields[b]; });
  }
  for (size_t k = 0; k < order.size(); ++k) {
    const size_t f = order[k];
    if (!q.empty()) {
      std::string n = p->fields[f];
      std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (n.find(q) == std::string::npos) continue;
    }
    ImGui::PushID(static_cast<int>(f));
    bool used = FieldUsed(*p, static_cast<int>(f));
    const std::string label = p->fields[f];
    const int field = static_cast<int>(f);
    if (ImGui::Checkbox(label.c_str(), &used)) {
      if (used) {
        // numbers go to Values (Count when the field has blanks or text), text and dates to Rows (Excel's rule)
        const int where = DefaultArea(s, *live, field);
        AddFieldTo(s, *live, *p, field, where == 0 ? kRows : kValues, -1, FuncFor(where), TR("Add Field"));
      } else {
        PaneEdit(s, *p, TR("Remove Field"), [field](PivotTable& x) {
          for (auto* v : {&x.rows, &x.cols, &x.pages}) {
            v->erase(std::remove_if(v->begin(), v->end(), [&](const model::PivotAxisField& a) { return a.field == field; }), v->end());
          }
          x.data.erase(std::remove_if(x.data.begin(), x.data.end(), [&](const model::PivotDataField& d) { return d.field == field; }), x.data.end());
        });
      }
    }
    if (ImGui::BeginDragDropSource()) {
      DragField drag{field, kNone, -1};
      ImGui::SetDragDropPayload("YC_PIVOT_FIELD", &drag, sizeof(drag));
      ImGui::TextUnformatted(p->fields[f].c_str());
      ImGui::EndDragDropSource();
    }
    // right-click: Add to Report Filter / Row Labels / Column Labels / Values
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##addto");
    if (ImGui::BeginPopup("##addto")) {
      const int targets[] = {kFilters, kRows, kColumns, kValues};
      const char* names[] = {TR("Add to Report Filter"), TR("Add to Row Labels"), TR("Add to Column Labels"), TR("Add to Values")};
      for (int t = 0; t < 4; ++t) {
        if (!ImGui::MenuItem(names[t])) continue;
        const int to = targets[t];
        const model::PivotFunc func = to == kValues ? FuncFor(DefaultArea(s, *live, field) == 2 ? 2 : 1) : model::PivotFunc::Sum;
        AddFieldTo(s, *live, *p, field, to, -1, func, TR("Add Field"));
      }
      ImGui::EndPopup();
    }
    // a filtered field shows the funnel
    if (const auto* af = AxisFieldOf(*p, field); af && Filtered(*af)) {
      ImGui::SameLine();
      ImGui::TextUnformatted(ui::icon::Filter);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("This field is filtered"));
    }
    ImGui::PopID();
  }
  // a field dragged out of an area onto the list leaves the report
  const ImVec2 rest = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##out", ImVec2(std::max(1.0f, rest.x), std::max(S(4), rest.y)));
  ImGui::EndChild();
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("YC_PIVOT_FIELD")) {
      const DragField drag = *static_cast<const DragField*>(pl->Data);
      if (drag.from != kNone && drag.field != -2) {
        const int area = drag.from, index = drag.index;
        PaneEdit(s, *p, TR("Remove Field"), [area, index](PivotTable& x) { RemoveField(x, area, index); });
      }
    }
    ImGui::EndDragDropTarget();
  }
  }  // the fields section
  if (side) {
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
  }
  if (show_areas) {
    ImGui::TextUnformatted(TR("Drag fields between areas below:"));
    const float aw = side ? fields_w : w;
    const float half = (aw - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const float avail = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2 - ImGui::GetFrameHeightWithSpacing() - S(12);
    if (u.pane_layout == 4 || side) {
      // one column of four areas
      const float h = std::max(S(60), (avail - ImGui::GetTextLineHeightWithSpacing() * 2) * 0.25f);
      DrawArea(s, *p, kFilters, TR("Filters"), aw, h);
      DrawArea(s, *p, kColumns, u.chart_mode ? TR("Legend (Series)") : TR("Columns"), aw, h);
      DrawArea(s, *p, kRows, u.chart_mode ? TR("Axis (Categories)") : TR("Rows"), aw, h);
      DrawArea(s, *p, kValues, TR("Values"), aw, h);
    } else {
      const float h = std::max(S(80), avail * 0.5f);
      DrawArea(s, *p, kFilters, TR("Filters"), half, h);
      ImGui::SameLine();
      DrawArea(s, *p, kColumns, u.chart_mode ? TR("Legend (Series)") : TR("Columns"), half, h);
      DrawArea(s, *p, kRows, u.chart_mode ? TR("Axis (Categories)") : TR("Rows"), half, h);
      ImGui::SameLine();
      DrawArea(s, *p, kValues, TR("Values"), half, h);
    }
  }
  if (side) ImGui::EndGroup();
  // Defer Layout Update: changes wait for Update
  bool defer = u.defer;
  if (ImGui::Checkbox(TR("Defer Layout Update"), &defer)) {
    u.defer = defer;
    if (!defer && u.pending) {
      const PivotTable next = *u.pending;
      u.pending.reset();
      EditPivot(s, TR("Update"), [next](PivotTable& x) {
        x.rows = next.rows;
        x.cols = next.cols;
        x.pages = next.pages;
        x.data = next.data;
        x.values_on_rows = next.values_on_rows;
      });
    }
  }
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(70) + ImGui::GetCursorPosX());
  ImGui::BeginDisabled(!u.defer || !u.pending);
  if (ImGui::Button(TR("Update"), ImVec2(S(70), 0)) && u.pending) {
    const PivotTable next = *u.pending;
    u.pending.reset();
    EditPivot(s, TR("Update"), [next](PivotTable& x) {
      x.rows = next.rows;
      x.cols = next.cols;
      x.pages = next.pages;
      x.data = next.data;
      x.values_on_rows = next.values_on_rows;
    });
  }
  ImGui::EndDisabled();
  ImGui::End();
}

namespace {

// a filter on one field of an axis, from a menu: items hidden (by their identity)
void SetHidden(AppState& s, int area, int pos, std::vector<std::string> hidden) {
  EditPivot(s, TR("Filter"), [area, pos, hidden](PivotTable& x) {
    auto* v = AxisOf(x, area);
    if (v && static_cast<size_t>(pos) < v->size()) (*v)[static_cast<size_t>(pos)].hidden = hidden;
  });
}

// the item moved to a place among the field's items (Move > Move "East" Up...): manual order
void MoveItem(AppState& s, const PivotTable& p, int area, int pos, const std::string& item, int where) {
  std::vector<std::string> order = ShownOrder(s, p, area, (*AxisOf(p, area))[static_cast<size_t>(pos)].field);
  const auto it = std::find(order.begin(), order.end(), item);
  if (it == order.end()) return;
  size_t i = static_cast<size_t>(it - order.begin());
  order.erase(it);
  size_t to = where == 0 ? 0 : where == 1 ? (i > 0 ? i - 1 : 0) : where == 2 ? std::min(order.size(), i + 1) : order.size();
  order.insert(order.begin() + static_cast<std::ptrdiff_t>(to), item);
  EditPivot(s, TR("Move"), [area, pos, order](PivotTable& x) {
    auto* v = AxisOf(x, area);
    if (!v || static_cast<size_t>(pos) >= v->size()) return;
    auto& f = (*v)[static_cast<size_t>(pos)];
    f.manual_order = order;
    f.descending = false;
    f.sort_by = -1;
  });
}

void ShowDetailsAt(AppState& s, uint32_t ar, uint32_t ac) {
  std::string e2;
  const uint32_t sheet = U().sheet;
  const uint32_t made = s.engine->Call([&](Document& d) {
    d.Begin(TR("Show Details"));
    const uint32_t v = d.PivotShowDetails(sheet, ar, ac, &e2);
    d.Commit();
    return v;
  });
  if (made) {
    s.engine->Sync();
    s.snap = s.engine->Current();
    ActivateSheet(s, made);
  } else if (!e2.empty()) {
    s.ShowError("YCGrid", e2);
  }
}

void LabelFilterMenu(AppState& s, const PivotTable& p, int area, int pos) {
  struct T {
    const char* label;
    int type;
  };
  static const T kLabel[] = {{TR("Equals..."), 15}, {TR("Does Not Equal..."), 16}, {TR("Begins With..."), 17}, {TR("Does Not Begin With..."), 18},
                             {TR("Ends With..."), 19}, {TR("Does Not End With..."), 20}, {TR("Contains..."), 21}, {TR("Does Not Contain..."), 22},
                             {TR("Greater Than..."), 23}, {TR("Greater Than Or Equal To..."), 24}, {TR("Less Than..."), 25},
                             {TR("Less Than Or Equal To..."), 26}, {TR("Between..."), 27}, {TR("Not Between..."), 28}};
  static const T kValue[] = {{TR("Equals..."), 7}, {TR("Does Not Equal..."), 8}, {TR("Greater Than..."), 9}, {TR("Greater Than Or Equal To..."), 10},
                             {TR("Less Than..."), 11}, {TR("Less Than Or Equal To..."), 12}, {TR("Between..."), 13}, {TR("Not Between..."), 14}};
  const auto& af = (*AxisOf(p, area))[static_cast<size_t>(pos)];
  if (ImGui::BeginMenu(TR("Label Filters"))) {
    if (ImGui::MenuItem(TR("Clear Filter"), nullptr, false, af.label_filter.type != 0))
      EditPivot(s, TR("Filter"), [area, pos](PivotTable& x) { (*AxisOf(x, area))[static_cast<size_t>(pos)].label_filter = {}; });
    ImGui::Separator();
    for (const T& t : kLabel) {
      if (ImGui::MenuItem(t.label, nullptr, af.label_filter.type == t.type)) OpenFilterDlg(s, p, area, pos, 0, t.type);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(TR("Value Filters"), !p.data.empty())) {
    if (ImGui::MenuItem(TR("Clear Filter"), nullptr, false, af.value_filter.type != 0))
      EditPivot(s, TR("Filter"), [area, pos](PivotTable& x) { (*AxisOf(x, area))[static_cast<size_t>(pos)].value_filter = {}; });
    ImGui::Separator();
    for (const T& t : kValue) {
      if (ImGui::MenuItem(t.label, nullptr, af.value_filter.type == t.type)) OpenFilterDlg(s, p, area, pos, 1, t.type);
    }
    if (ImGui::MenuItem(TR("Top 10..."), nullptr, af.value_filter.type >= 1 && af.value_filter.type <= 6)) OpenFilterDlg(s, p, area, pos, 2, 1);
    ImGui::EndMenu();
  }
}

void ClearFilterOf(AppState& s, int area, int pos) {
  EditPivot(s, TR("Clear Filter"), [area, pos](PivotTable& x) {
    auto* v = AxisOf(x, area);
    if (!v || static_cast<size_t>(pos) >= v->size()) return;
    auto& f = (*v)[static_cast<size_t>(pos)];
    f.hidden.clear();
    f.page_item.clear();
    f.label_filter = {};
    f.value_filter = {};
    const int field = f.field;
    x.slicer_filters.erase(std::remove_if(x.slicer_filters.begin(), x.slicer_filters.end(), [&](const model::PivotAxisField& sf) { return sf.field == field; }),
                           x.slicer_filters.end());
  });
}

}  // namespace

bool DrawPivotCellMenu(AppState& s) {
  // Show Details moves to a new snapshot in the middle of the menu: what the menu reads stays alive until it is drawn
  const std::shared_ptr<const engine::Snapshot> snap_hold = s.snap;
  const PivotTable* p = ActivePivot(s);
  if (!p) return false;
  const Cached& cc = CacheFor(s, *p);
  if (!cc.ok) return false;
  const std::shared_ptr<const engine::PivotCache> cache_hold = cc.cache;
  const std::shared_ptr<const engine::PivotReport> rep_hold = cc.rep;
  const CellInfo ci = ActiveInfo(s, *p);
  if (!ci.in) return false;
  const auto& sel = s.FocusView().sel;
  const engine::PivotReport& rep = *rep_hold;
  const engine::PivotCache& cache = *cache_hold;
  const int area = ci.area, index = ci.pos, data = ci.data;
  const bool label = area != kNone;
  const bool value_cell = data >= 0 && !ci.caption && !label;
  std::string fname = label ? FieldName(*p, ci.field) : std::string();
  std::string iname = label && ci.item >= 0 ? cache.display[static_cast<size_t>(ci.field)][static_cast<size_t>(ci.item)] : std::string();
  const std::string item_id = label && ci.item >= 0 ? cache.items[static_cast<size_t>(ci.field)][static_cast<size_t>(ci.item)] : std::string();
  if (value_cell && ImGui::MenuItem(TR("Number Format..."))) {
    PivotUi& u = U();
    u.nf_index = data;
    std::snprintf(u.nf_format, sizeof(u.nf_format), "%s", p->data[static_cast<size_t>(data)].number_format.c_str());
    s.OpenDialog(Dialog::PivotNumberFormat);
  }
  if (ImGui::MenuItem(TR("Refresh"), "Alt+F5")) ui::Commands().Execute("PivotRefresh");
  // Sort: a label sorts its field A to Z; a value sorts the field of its row (or column) by this value column (row)
  if ((label && area != kFilters) || value_cell) {
    if (ImGui::BeginMenu(TR("Sort"))) {
      if (label) {
        const bool desc = (*AxisOf(*p, area))[static_cast<size_t>(index)].descending;
        if (ImGui::MenuItem(TR("Sort A to Z"), nullptr, !desc)) EditPivot(s, TR("Sort"), [area, index](PivotTable& x) {
          auto& f = (*AxisOf(x, area))[static_cast<size_t>(index)];
          f.descending = false;
          f.sort_by = -1;
          f.manual_order.clear();
        });
        if (ImGui::MenuItem(TR("Sort Z to A"), nullptr, desc)) EditPivot(s, TR("Sort"), [area, index](PivotTable& x) {
          auto& f = (*AxisOf(x, area))[static_cast<size_t>(index)];
          f.descending = true;
          f.sort_by = -1;
          f.manual_order.clear();
        });
        if (ImGui::MenuItem(TR("More Sort Options..."))) OpenSort(s, *p, area, index);
      } else {
        // the row's innermost field, read in this cell's column (the grand total row: the column field, read in it)
        int sarea = kNone, spos = -1;
        std::vector<std::string> other;
        const auto& rf = rep.row_filter[ci.r];
        const auto& cf = rep.col_filter[ci.c];
        if (!rf.empty()) {
          AxisFieldOf(*p, rf.back().first, &sarea, &spos);
          for (const auto& [f, it] : cf) other.push_back(cache.items[static_cast<size_t>(f)][static_cast<size_t>(it)]);
        } else if (!cf.empty()) {
          AxisFieldOf(*p, cf.back().first, &sarea, &spos);
        } else if (!p->rows.empty()) {
          sarea = kRows, spos = static_cast<int>(p->rows.size()) - 1;
        }
        for (int k = 0; k < 2 && sarea != kNone; ++k) {
          const bool desc = k == 1;
          if (ImGui::MenuItem(desc ? TR("Sort Largest to Smallest") : TR("Sort Smallest to Largest"))) {
            EditPivot(s, TR("Sort"), [sarea, spos, data, other, desc](PivotTable& x) {
              auto* v = AxisOf(x, sarea);
              if (!v || static_cast<size_t>(spos) >= v->size()) return;
              auto& f = (*v)[static_cast<size_t>(spos)];
              f.sort_by = data;
              f.sort_col = other;
              f.descending = desc;
              f.manual_order.clear();
            });
          }
        }
        if (sarea != kNone && ImGui::MenuItem(TR("More Sort Options..."))) OpenSort(s, *p, sarea, spos);
      }
      ImGui::EndMenu();
    }
  }
  if (label && area != kFilters) {
    // Filter: Keep Only / Hide Selected Items, the condition filters, Clear Filter From
    if (ImGui::BeginMenu(TR("Filter"))) {
      const std::vector<std::string> picked = SelectedItems(s, *p, ci.field);
      if (ImGui::MenuItem(TR("Keep Only Selected Items"), nullptr, false, !picked.empty())) {
        std::vector<std::string> hidden;
        for (const auto& [id, shown] : ItemsOf(s, *p, ci.field)) {
          if (std::find(picked.begin(), picked.end(), id) == picked.end()) hidden.push_back(id);
        }
        SetHidden(s, area, index, hidden);
      }
      if (ImGui::MenuItem(TR("Hide Selected Items"), nullptr, false, !picked.empty())) {
        std::vector<std::string> hidden = (*AxisOf(*p, area))[static_cast<size_t>(index)].hidden;
        for (const auto& it : picked) {
          if (std::find(hidden.begin(), hidden.end(), it) == hidden.end()) hidden.push_back(it);  // lesson-ok: find-push the items picked in a menu
        }
        if (hidden.size() >= ItemsOf(s, *p, ci.field).size()) s.ShowMessage("YCGrid", TR("At least one item must be visible."));
        else SetHidden(s, area, index, hidden);
      }
      if (ImGui::MenuItem(TR("Top 10..."), nullptr, false, !p->data.empty())) OpenFilterDlg(s, *p, area, index, 2, 1);
      LabelFilterMenu(s, *p, area, index);
      ImGui::Separator();
      if (ImGui::MenuItem((std::string(TR("Clear Filter From \"")) + fname + "\"").c_str(), nullptr, false, Filtered((*AxisOf(*p, area))[static_cast<size_t>(index)])))
        ClearFilterOf(s, area, index);
      ImGui::EndMenu();
    }
    const auto& axis = *AxisOf(*p, area);
    const bool sub = axis[static_cast<size_t>(index)].subtotal;
    if (ImGui::MenuItem((std::string(TR("Subtotal \"")) + fname + "\"").c_str(), nullptr, sub))
      EditPivot(s, TR("Subtotal"), [area, index, sub](PivotTable& x) { (*AxisOf(x, area))[static_cast<size_t>(index)].subtotal = !sub; });
    // Expand / Collapse
    if (ImGui::BeginMenu(TR("Expand/Collapse"))) {
      const int field = ci.field;
      const bool outer = index + 1 < static_cast<int>(axis.size());
      const bool collapsed = outer && !item_id.empty() && engine::PivotItemCollapsed(axis[static_cast<size_t>(index)], item_id);
      auto set_item = [&s, area, index, item_id](bool c) {
        EditPivot(s, c ? TR("Collapse") : TR("Expand"), [area, index, item_id, c](PivotTable& x) {
          auto* v = AxisOf(x, area);
          if (v && static_cast<size_t>(index) < v->size()) engine::SetPivotItemCollapsed((*v)[static_cast<size_t>(index)], item_id, c);
        });
      };
      auto set_field = [&s, area](int pos, bool c) {
        EditPivot(s, c ? TR("Collapse Entire Field") : TR("Expand Entire Field"), [area, pos, c](PivotTable& x) {
          auto* v = AxisOf(x, area);
          if (!v) return;
          for (size_t i = 0; i < v->size(); ++i) {
            // "Collapse to": every field from pos on is collapsed; "Expand to": every field before pos is expanded
            if (c ? static_cast<int>(i) >= pos : static_cast<int>(i) < pos) {
              (*v)[i].collapse_all = c;
              (*v)[i].collapsed.clear();
            }
          }
        });
      };
      if (ImGui::MenuItem(TR("Expand"), nullptr, false, outer && collapsed)) set_item(false);
      if (ImGui::MenuItem(TR("Collapse"), nullptr, false, outer && !collapsed)) set_item(true);
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Expand Entire Field"), nullptr, false, outer)) set_field(index + 1, false);
      if (ImGui::MenuItem(TR("Collapse Entire Field"), nullptr, false, outer)) set_field(index, true);
      ImGui::Separator();
      for (size_t i = 0; i < axis.size(); ++i) {
        const std::string n = FieldName(*p, axis[i].field);
        if (static_cast<int>(i) < index && ImGui::MenuItem((std::string(TR("Collapse to \"")) + n + "\"").c_str())) set_field(static_cast<int>(i), true);
        if (static_cast<int>(i) > index && ImGui::MenuItem((std::string(TR("Expand to \"")) + n + "\"").c_str())) set_field(static_cast<int>(i), false);
      }
      (void)field;
      ImGui::EndMenu();
    }
  }
  if (label) {
    ImGui::Separator();
    const int field = ci.field;
    bool grouped = false;
    for (const auto& g : p->groups) grouped = grouped || g.field == field;
    grouped = grouped || (static_cast<size_t>(field) < cache.made_by.size() && cache.made_by[static_cast<size_t>(field)] >= 0);
    if (ImGui::MenuItem(TR("Group..."))) {
      // several text items selected: a group of them; otherwise the periods / ranges of dates and numbers
      const std::vector<std::string> picked = SelectedItems(s, *p, field);
      bool numeric = false;
      if (static_cast<size_t>(field) < cache.columns) {
        for (size_t k = 0; k < cache.source_records && !numeric; ++k) numeric = cache.records[k][static_cast<size_t>(field)].IsNumeric();
      }
      if (!numeric && picked.size() >= 2) ui::Commands().Execute("PivotGroupSelection");
      else OpenGroupDialog(s, *p, field);
    }
    if (ImGui::MenuItem(TR("Ungroup"), nullptr, false, grouped)) Ungroup(s, *p, field, item_id);
    // Move: the item among its siblings, the field among the axis' fields
    if (ImGui::BeginMenu(TR("Move"))) {
      if (!item_id.empty() && area != kFilters) {
        const std::string q = "\"" + iname + "\"";
        static const char* kWhere[] = {TR("Move %s to Beginning"), TR("Move %s Up"), TR("Move %s Down"), TR("Move %s to End")};
        for (int w = 0; w < 4; ++w) {
          char b[256];
          std::snprintf(b, sizeof(b), kWhere[w], q.c_str());
          if (ImGui::MenuItem(b)) MoveItem(s, *p, area, index, item_id, w);
        }
        ImGui::Separator();
      }
      const int n = static_cast<int>(AxisOf(*p, area)->size());
      const std::string q = "\"" + fname + "\"";
      DragField me{field, area, index};
      struct W {
        const char* fmt;
        int at;
        bool ok;
      };
      const W kField[] = {{TR("Move %s to Beginning"), 0, index > 0}, {TR("Move %s Up"), index - 1, index > 0}, {TR("Move %s Down"), index + 2, index + 1 < n},
                          {TR("Move %s to End"), -1, index + 1 < n}};
      for (const W& w : kField) {
        char b[256];
        std::snprintf(b, sizeof(b), w.fmt, q.c_str());
        const int at = w.at;
        if (ImGui::MenuItem(b, nullptr, false, w.ok)) EditPivot(s, TR("Move Field"), [me, area, at](PivotTable& x) { ApplyMove(x, me, area, at, model::PivotFunc::Sum); });
      }
      char b[256];
      const int to = area == kRows ? kColumns : kRows;
      std::snprintf(b, sizeof(b), area == kRows ? TR("Move %s to Columns") : TR("Move %s to Rows"), q.c_str());
      if (ImGui::MenuItem(b)) EditPivot(s, TR("Move Field"), [me, to](PivotTable& x) { ApplyMove(x, me, to, -1, model::PivotFunc::Sum); });
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem((std::string(TR("Remove \"")) + fname + "\"").c_str())) EditPivot(s, TR("Remove Field"), [area, index](PivotTable& x) { RemoveField(x, area, index); });
    if (ImGui::MenuItem(TR("Field Settings..."))) OpenFieldSettings(s, *p, area, index);
  }
  if (data >= 0 && static_cast<size_t>(data) < p->data.size()) {
    const auto& df = p->data[static_cast<size_t>(data)];
    const std::string cap = engine::PivotDataCaption(*p, cache, static_cast<size_t>(data));
    if (!label) {
      ImGui::Separator();
      if (ImGui::MenuItem((std::string(TR("Remove \"")) + cap + "\"").c_str())) EditPivot(s, TR("Remove Field"), [data](PivotTable& x) { RemoveField(x, kValues, data); });
    }
    if (ImGui::BeginMenu(TR("Summarize Values By"))) {
      for (int f : {0, 1, 2, 3, 4, 5}) {
        if (ImGui::MenuItem(FuncLabel(f), nullptr, static_cast<int>(df.func) == f)) {
          EditPivot(s, TR("Summarize Values By"), [data, f](PivotTable& x) {
            auto& d = x.data[static_cast<size_t>(data)];
            d.func = static_cast<model::PivotFunc>(f);
            d.caption.clear();  // "Sum of Sales" becomes "Average of Sales"
          });
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem(TR("More Options..."))) OpenValueField(s, *p, data, 0);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(TR("Show Values As"))) {
      for (int k = 0; k < model::kPivotShowAs; ++k) {
        const bool base = engine::PivotShowAsNeedsBase(static_cast<model::PivotShowAs>(k));
        const std::string lbl = std::string(ShowAsLabel(k)) + (base ? "..." : "");
        if (!ImGui::MenuItem(lbl.c_str(), nullptr, static_cast<int>(df.show_as) == k)) continue;
        if (base) {
          // the base field (and item) are chosen in a dialog, as in Excel
          OpenValueField(s, *p, data, 1);
          U().vf_show = k;
          if (U().vf_base_field < 0) U().vf_base_field = !p->rows.empty() ? p->rows.back().field : (!p->cols.empty() ? p->cols.back().field : -1);
          if (engine::PivotShowAsNeedsBaseItem(static_cast<model::PivotShowAs>(k)) && U().vf_base_item.empty()) U().vf_base_item = "(previous)";
        } else {
          EditPivot(s, TR("Show Values As"), [data, k](PivotTable& x) { x.data[static_cast<size_t>(data)].show_as = static_cast<model::PivotShowAs>(k); });
        }
      }
      ImGui::Separator();
      if (ImGui::MenuItem(TR("More Options..."))) OpenValueField(s, *p, data, 1);
      ImGui::EndMenu();
    }
    if (value_cell && ci.kind == engine::PivotCell::Data && ImGui::MenuItem(TR("Show Details"))) ShowDetailsAt(s, sel.act_r, sel.act_c);
    if (ImGui::MenuItem(TR("Value Field Settings..."))) OpenValueField(s, *p, data);
  }
  ImGui::Separator();
  if (ImGui::MenuItem(TR("PivotTable Options..."))) OpenOptions(s, *p);
  if (ImGui::MenuItem(U().field_list ? TR("Hide Field List") : TR("Show Field List"))) ui::Commands().Execute("PivotFieldList");
  return true;
}

void DrawPivotDialogs(AppState& s) {
  PivotUi& u = U();
  if (u.made && u.made->done) {
    const std::shared_ptr<PivotUi::Made> m = std::move(u.made);
    if (m->message) {
      s.ShowMessage("YCGrid", m->err);
    } else if (!m->id) {
      s.ShowError("YCGrid", m->err.empty() ? TR("The PivotTable couldn't be created.") : m->err);
    } else {
      s.engine->Sync();
      s.snap = s.engine->Current();
      if (m->sheet != s.focus_sheet && s.snap->Sheet(m->sheet)) ActivateSheet(s, m->sheet);
      render::SelectCell(s.FocusView(), m->r0, m->c0, false);
      u.field_list = true;
      if (m->chart) {
        // Insert > PivotChart: the chart of the new (empty) PivotTable, next to it
        const model::Sheet* sh = s.snap->Sheet(m->sheet);
        const PivotTable* p = nullptr;
        if (sh) {
          for (const auto& x : *sh->pivots) {
            if (x.id == m->id) p = &x;
          }
        }
        if (p) {
          model::ChartObj ch;
          ch.type = model::ChartType::ColumnClustered;
          ch.pivot = p->name;
          const Rect a = engine::PivotArea(*p);
          ch.left = static_cast<double>(sh->cols->Offset(static_cast<int64_t>(a.c1) + 2)) * 0.75;
          ch.top = static_cast<double>(sh->rows->Offset(a.r0)) * 0.75;
          const uint32_t sheet = m->sheet;
          s.engine->Call([&](Document& d) {
            d.Begin(TR("Insert PivotChart"));
            d.AddChart(sheet, ch);
            d.UpdatePivotCharts();
            d.Commit();
            return 0;
          });
        }
      }
    }
  } else if (u.made) {
    ui::RequestRedrawIn(1.0 / 30);  // until the engine is done
  }
  if (BeginDlg(s, Dialog::CreatePivot, u.with_chart ? TR("Create PivotChart###yc_dlg_pivot") : TR("Create PivotTable###yc_dlg_pivot"), 440)) {
    ImGui::TextUnformatted(TR("Choose the data that you want to analyze"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Table/Range:"));
    ImGui::SameLine(S(110));
    RangeBox(s, "pivot.source", u.source, sizeof(u.source), S(300), RangeBox_Sheet, TR("Table/Range:"));
    ImGui::Separator();
    ImGui::TextUnformatted(u.with_chart ? TR("Choose where you want the PivotChart to be placed") : TR("Choose where you want the PivotTable report to be placed"));
    ui::RadioButton(TR("New Worksheet"), &u.dest_new, 1);
    ui::RadioButton(TR("Existing Worksheet"), &u.dest_new, 0);
    ImGui::BeginDisabled(u.dest_new == 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Location:"));
    ImGui::SameLine(S(110));
    {
      const std::string before = u.dest;
      RangeBox(s, "pivot.dest", u.dest, sizeof(u.dest), S(300), RangeBox_Sheet | RangeBox_Cell, TR("Location:"));
      if (before != u.dest) u.dest_new = 0;
    }
    ImGui::EndDisabled();
    // Excel's "Choose whether you want to analyze multiple tables": YCGrid has no Data Model (ADR 0012), so the box
    // is there, off and disabled, and says why
    ImGui::Separator();
    ImGui::TextUnformatted(TR("Choose whether you want to analyze multiple tables"));
    {
      bool data_model = false;
      ImGui::BeginDisabled();
      ImGui::Checkbox(TR("Add this data to the Data Model"), &data_model);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", TR("YCGrid has no Data Model: relate tables with formulas (XLOOKUP) or in Python (pandas)."));
      }
    }
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      model::PivotTable p;
      std::string src = u.source;
      uint32_t sid = 0;
      Rect r;
      bool ok = true;
      if (model::FindTable(s.snap->wb, src, &sid)) {
        p.src_table = src;
        p.src_sheet = sid;
      } else if (ResolveRangeText(s.snap->wb, s.focus_sheet, src, &sid, &r)) {
        p.src_sheet = sid;
        p.src = r;
      } else {
        ok = false;
        s.ShowError("YCGrid", TR("The reference isn't valid."));
      }
      uint32_t dest_sheet = s.focus_sheet;
      Rect dr{2, 0, 2, 0};  // a new sheet: A3, like Excel
      if (ok && !u.dest_new) {
        ok = ResolveRangeText(s.snap->wb, s.focus_sheet, u.dest, &dest_sheet, &dr);
        if (!ok) s.ShowError("YCGrid", TR("The destination reference isn't valid."));
      }
      if (ok && !u.made) {
        p.r0 = dr.r0;
        p.c0 = dr.c0;
        const bool new_sheet = u.dest_new == 1;
        const uint32_t focus = s.focus_sheet;
        auto made = std::make_shared<PivotUi::Made>();
        made->r0 = p.r0;
        made->c0 = p.c0;
        made->chart = u.with_chart;
        u.made = made;
        const std::string label = TR("Insert PivotTable");
        // the source is read (checked, then cached by the PivotTable) on the engine thread: the screen stays live
        s.engine->Post([made, p, new_sheet, focus, dest_sheet, label](Document& d) {
          // check the source before anything is created (a bad source mustn't leave an empty new sheet)
          engine::PivotCache probe;
          std::string why;
          if (!engine::ReadPivotSource(d.wb(), p, &probe, &why)) {
            made->err = why;
            made->message = true;
            made->done = true;
            return;
          }
          std::string err;
          d.Begin(label);
          uint32_t target = dest_sheet;
          if (new_sheet) {
            const int32_t idx = d.wb().IndexOfId(focus);
            target = d.AddSheet({}, idx);
          }
          made->sheet = target;
          made->id = d.AddPivot(target, p, &err);
          d.Commit();
          made->err = err;
          made->done = true;
        });
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotSource, TR("Change PivotTable Data Source###yc_dlg_pivotsrc"), 420)) {
    ImGui::TextUnformatted(TR("Choose the data that you want to analyze"));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Table/Range:"));
    ImGui::SameLine(S(110));
    RangeBox(s, "pivot.newsource", u.source, sizeof(u.source), S(300), RangeBox_Sheet, TR("Table/Range:"));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const std::string src = u.source;
      uint32_t sid = 0;
      Rect r;
      if (model::FindTable(s.snap->wb, src, &sid)) {
        EditPivot(s, TR("Change Data Source"), [src, sid](PivotTable& x) {
          x.src_table = src;
          x.src_sheet = sid;
        });
        close = true;
      } else if (ResolveRangeText(s.snap->wb, s.focus_sheet, src, &sid, &r)) {
        EditPivot(s, TR("Change Data Source"), [sid, r](PivotTable& x) {
          x.src_table.clear();
          x.src_sheet = sid;
          x.src = r;
        });
        close = true;
      } else {
        s.ShowError("YCGrid", TR("The reference isn't valid."));
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotMove, TR("Move PivotTable###yc_dlg_pivotmove"), 380)) {
    ImGui::TextUnformatted(TR("Choose where you want the PivotTable to be placed"));
    ui::RadioButton(TR("New Worksheet"), &u.move_new, 1);
    ui::RadioButton(TR("Existing Worksheet"), &u.move_new, 0);
    ImGui::BeginDisabled(u.move_new == 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Location:"));
    ImGui::SameLine(S(90));
    {
      const std::string before = u.dest;
      RangeBox(s, "pivot.move", u.dest, sizeof(u.dest), S(260), RangeBox_Sheet | RangeBox_Cell, TR("Location:"));
      if (before != u.dest) u.move_new = 0;
    }
    ImGui::EndDisabled();
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      uint32_t sid = 0;
      Rect r{2, 0, 2, 0};
      const uint32_t from = U().sheet, id = U().id;
      const bool new_sheet = u.move_new == 1;
      if (!new_sheet && (!ResolveRangeText(s.snap->wb, from, u.dest, &sid, &r) || !s.snap->Sheet(sid) || s.snap->Sheet(sid)->chart_sheet)) {
        s.ShowError("YCGrid", TR("The destination reference isn't valid."));
      } else if (!new_sheet && sid == from) {
        EditPivot(s, TR("Move PivotTable"), [r](PivotTable& x) {
          x.r0 = r.r0;
          x.c0 = r.c0;
        });
        close = true;
      } else {
        // another sheet (or a new one, before this one): the report leaves its sheet and is made again there
        std::string err;
        uint32_t target = sid;
        const uint32_t nid = s.engine->Call([&](Document& d) {
          const PivotTable* old = d.PivotById(from, id);
          if (!old) return uint32_t{0};
          PivotTable copy = *old;
          copy.r0 = r.r0;
          copy.c0 = r.c0;
          copy.written.clear();
          d.Begin(TR("Move PivotTable"));
          if (new_sheet) target = d.AddSheet({}, d.wb().IndexOfId(from));
          d.DeletePivot(from, id, true, false);
          const uint32_t v = d.AddPivot(target, copy, &err);
          d.Commit();
          return v;
        });
        if (nid) {
          U().sheet = target;
          U().id = nid;
          s.engine->Sync();
          s.snap = s.engine->Current();
          ActivateSheet(s, target);
          close = true;
        } else {
          s.ShowError("YCGrid", err.empty() ? TR("The PivotTable couldn't be moved there.") : err);
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotRename, TR("PivotTable Name###yc_dlg_pivotname"), 320)) {
    ImGui::SetNextItemWidth(S(220));
    ImGui::InputText("##name", u.name, sizeof(u.name));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const std::string name = u.name;
      if (!name.empty()) EditPivot(s, TR("PivotTable Name"), [name](PivotTable& x) { x.name = name; });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::SlicerSettings, TR("Slicer Settings###yc_dlg_slicer_settings"), 420)) {
    bool close = false;
    model::Slicer& x = u.ss;
    ImGui::Text(TR("Source Name: %s"), x.pivot.c_str());
    {
      std::string fn = "Slicer_" + x.name;  // the slicer cache's name, as Python's SlicerCache.Name gives it
      for (char& ch : fn) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && !(static_cast<unsigned char>(ch) & 0x80)) ch = '_';
      }
      ImGui::Text(TR("Name to use in formulas: %s"), fn.c_str());
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Caption:"));
    ImGui::SameLine(S(90));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##ss_caption", u.ss_caption, sizeof u.ss_caption);
    ImGui::Checkbox(TR("Display header"), &x.show_header);
    if (!x.timeline) {
      ImGui::SeparatorText(TR("Item Sorting and Filtering"));
      int sort = x.descending ? 1 : 0;
      ui::RadioButton(TR("Ascending (A to Z)"), &sort, 0);
      ui::RadioButton(TR("Descending (Z to A)"), &sort, 1);
      x.descending = sort == 1;
      ImGui::Checkbox(TR("Hide items with no data"), &x.hide_no_data);
      ImGui::BeginDisabled(x.hide_no_data);
      ImGui::Checkbox(TR("Visually indicate items with no data"), &x.indicate_no_data);
      ImGui::BeginDisabled(!x.indicate_no_data);
      ImGui::Checkbox(TR("Show items with no data last"), &x.no_data_last);
      ImGui::EndDisabled();
      ImGui::EndDisabled();
    }
    ImGui::Separator();
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      model::Slicer v = x;
      v.caption = u.ss_caption;
      const uint32_t sheet = u.ss_sheet, id = u.ss_id;
      s.engine->PostUser(TR("Slicer Settings"), [sheet, id, v](Document& d) {
        d.UpdateSlicer(sheet, id, [&v](model::Slicer& y) {
          y.caption = v.caption;
          y.show_header = v.show_header;
          y.descending = v.descending;
          y.hide_no_data = v.hide_no_data;
          y.indicate_no_data = v.indicate_no_data;
          y.no_data_last = v.no_data_last;
        });
      });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::InsertSlicer, u.pick_timeline ? TR("Insert Timelines###yc_dlg_slicer") : TR("Insert Slicers###yc_dlg_slicer"), 300)) {
    const PivotTable* p = ActivePivot(s);
    // a table's slicer: its column headers
    const model::Table* tbl = nullptr;
    if (!u.pick_table.empty()) {
      uint32_t ts = 0;
      tbl = model::FindTable(s.snap->wb, u.pick_table, &ts);
      p = nullptr;
    }
    bool close = p == nullptr && tbl == nullptr;
    if (tbl) {
      const model::Sheet* tsh = s.snap->Sheet(u.pick_table_sheet);
      u.slicer_pick.resize(tbl->range.Cols(), 0);
      ImGui::BeginChild("##slcols", ImVec2(S(260), S(220)), ImGuiChildFlags_Borders);
      for (uint32_t c = 0; c < tbl->range.Cols(); ++c) {
        const std::string name = c < tbl->columns.size() ? tbl->columns[c].name
                                 : tsh ? engine::DisplayTextOf(tsh->GetValue(tbl->range.r0, tbl->range.c0 + c), "General") : std::string();
        bool on = u.slicer_pick[c] != 0;
        ImGui::PushID(static_cast<int>(c));
        if (ImGui::Checkbox(name.c_str(), &on)) u.slicer_pick[c] = on ? 1 : 0;
        ImGui::PopID();
      }
      ImGui::EndChild();
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        const model::Sheet* sh = s.FocusSheet();
        const uint32_t sheet = s.focus_sheet;
        double left = sh ? static_cast<double>(sh->cols->Offset(static_cast<int64_t>(tbl->range.c1) + 2)) * 0.75 : 0;
        const double top = sh ? static_cast<double>(sh->rows->Offset(tbl->range.r0)) * 0.75 : 0;
        std::vector<model::Slicer> add;
        for (uint32_t c = 0; c < u.slicer_pick.size(); ++c) {
          if (!u.slicer_pick[c]) continue;
          model::Slicer sl;
          sl.table = tbl->name;
          sl.field = static_cast<int>(c);
          sl.caption = c < tbl->columns.size() ? tbl->columns[c].name : std::string("Slicer");
          sl.left = left;
          sl.top = top + 18.0 * static_cast<double>(add.size());
          left += 18;
          add.push_back(sl);
        }
        if (!add.empty()) {
          u.sl_select_pending = true;
          u.sl_select_sheet = sheet;
          u.sl_select_after = 0;
          if (sh) {
            for (const auto& x : *sh->slicers) u.sl_select_after = std::max(u.sl_select_after, x.id);
          }
          s.engine->PostUser(TR("Insert Slicer"), [sheet, add](Document& d) {
            for (const auto& sl : add) d.AddSlicer(sheet, sl);
          });
        }
        close = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    }
    if (p) {
      const Cached& c = CacheFor(s, *p);
      size_t nsrc = c.ok ? c.cache->source_fields : p->fields.size();
      u.slicer_pick.resize(p->fields.size(), 0);
      ImGui::BeginChild("##slfields", ImVec2(S(260), S(220)), ImGuiChildFlags_Borders);
      int shown = 0;
      for (size_t f = 0; f < nsrc && f < p->fields.size(); ++f) {
        // timelines: only the fields of dates (numbers formatted as dates, as Excel lists them)
        if (u.pick_timeline) {
          if (!c.ok || f >= c.cache->columns) continue;
          bool dates = false;
          for (size_t k = 0; k < c.cache->source_records && !dates; ++k) dates = c.cache->records[k][f].IsNumeric();
          if (!dates || f >= c.cache->numfmts.size() || !formula::IsDateFormat(c.cache->numfmts[f])) continue;
        }
        ++shown;
        bool on = u.slicer_pick[f] != 0;
        if (ImGui::Checkbox(p->fields[f].c_str(), &on)) u.slicer_pick[f] = on ? 1 : 0;
      }
      if (shown == 0) ImGui::TextWrapped("%s", u.pick_timeline ? TR("This PivotTable has no fields of dates to filter with a timeline.") : TR("(no fields)"));
      ImGui::EndChild();
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        const model::Sheet* sh = s.FocusSheet();
        const Rect a = engine::PivotArea(*p);
        double left = sh ? static_cast<double>(sh->cols->Offset(static_cast<int64_t>(a.c1) + 1)) * 0.75 + 12 : 0;
        const double top = sh ? static_cast<double>(sh->rows->Offset(a.r0)) * 0.75 : 0;
        const uint32_t sheet = s.focus_sheet;
        const std::string pivot = p->name;
        std::vector<model::Slicer> add;
        for (size_t f = 0; f < nsrc && f < u.slicer_pick.size(); ++f) {
          if (!u.slicer_pick[f]) continue;
          model::Slicer sl;
          sl.pivot = pivot;
          sl.field = static_cast<int>(f);
          sl.caption = p->fields[f];
          sl.left = left;
          sl.top = top + 18.0 * static_cast<double>(add.size());
          if (u.pick_timeline) {
            sl.timeline = true;
            sl.width = 330;
            sl.height = 110;
          }
          left += 18;
          add.push_back(sl);
        }
        if (!add.empty()) {
          u.sl_select_pending = true;
          u.sl_select_sheet = sheet;
          u.sl_select_after = 0;
          if (sh) {
            for (const auto& x : *sh->slicers) u.sl_select_after = std::max(u.sl_select_after, x.id);
          }
          s.engine->PostUser(TR("Insert Slicer"), [sheet, add](Document& d) {
            for (const auto& sl : add) d.AddSlicer(sheet, sl);
          });
        }
        close = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotReportConnections, TR("Report Connections###yc_dlg_pivotrc"), 420)) {
    bool close = false;
    ImGui::TextUnformatted(TR("Select PivotTables to connect to this filter"));
    ImGui::BeginChild("##rclist", ImVec2(S(380), S(180)), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < u.rc_names.size(); ++i) {
      bool on = u.rc_on[i] != 0;
      uint32_t ps = 0;
      PivotNamed(s, u.rc_names[i], &ps);
      const model::Sheet* psh = s.snap->Sheet(ps);
      const std::string label = u.rc_names[i] + "   (" + (psh ? psh->name : std::string()) + ")";
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Checkbox(label.c_str(), &on)) u.rc_on[i] = on ? 1 : 0;
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      std::vector<std::string> pick;
      for (size_t i = 0; i < u.rc_names.size(); ++i) {
        if (u.rc_on[i]) pick.push_back(u.rc_names[i]);
      }
      if (pick.empty()) {
        s.ShowMessage("YCGrid", TR("A slicer must be connected to at least one PivotTable."));
      } else {
        const uint32_t sheet = u.rc_sheet, id = u.rc_id;
        PivotDocOp(s, TR("Report Connections"), [sheet, id, pick](Document& d, std::string* err) { return d.ConnectSlicer(sheet, id, pick, err); });
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotFilterConnections, TR("Filter Connections###yc_dlg_pivotfc"), 420)) {
    bool close = false;
    const PivotTable* p = ActivePivot(s);
    ImGui::TextUnformatted(TR("Select slicers and timelines to connect to this PivotTable"));
    ImGui::BeginChild("##fclist", ImVec2(S(380), S(180)), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < u.rc_names.size(); ++i) {
      const uint32_t sid = static_cast<uint32_t>(std::strtoul(u.rc_names[i].c_str(), nullptr, 10));
      const uint32_t id = static_cast<uint32_t>(std::strtoul(u.rc_names[i].c_str() + u.rc_names[i].find(':') + 1, nullptr, 10));
      const model::Sheet* ssh = s.snap->Sheet(sid);
      std::string label = "?";
      for (const auto& sl : ssh ? *ssh->slicers : NoSlicers()) {
        if (sl.id == id) label = (sl.caption.empty() ? sl.name : sl.caption) + "   (" + ssh->name + (sl.timeline ? std::string(", ") + TR("Timeline") : std::string()) + ")";
      }
      bool on = u.rc_on[i] != 0;
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Checkbox(label.c_str(), &on)) u.rc_on[i] = on ? 1 : 0;
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) && p) {
      // each slicer whose box changed joins or leaves this PivotTable
      struct Change {
        uint32_t sheet, id;
        std::vector<std::string> pivots;
      };
      std::vector<Change> changes;
      for (size_t i = 0; i < u.rc_names.size(); ++i) {
        const uint32_t sid = static_cast<uint32_t>(std::strtoul(u.rc_names[i].c_str(), nullptr, 10));
        const uint32_t id = static_cast<uint32_t>(std::strtoul(u.rc_names[i].c_str() + u.rc_names[i].find(':') + 1, nullptr, 10));
        const model::Sheet* ssh = s.snap->Sheet(sid);
        for (const auto& sl : ssh ? *ssh->slicers : NoSlicers()) {
          if (sl.id != id) continue;
          const bool was = SlicerConnectedTo(sl, p->name), now = u.rc_on[i] != 0;
          if (was == now) continue;
          std::vector<std::string> list = {sl.pivot};
          list.insert(list.end(), sl.more_pivots.begin(), sl.more_pivots.end());
          if (now) list.push_back(p->name);
          else list.erase(std::remove(list.begin(), list.end(), p->name), list.end());
          if (!list.empty()) changes.push_back(Change{sid, id, list});
        }
      }
      if (!changes.empty()) {
        PivotDocOp(s, TR("Filter Connections"), [changes](Document& d, std::string* err) {
          bool ok = true;
          for (const auto& c : changes) ok = d.ConnectSlicer(c.sheet, c.id, c.pivots, err) && ok;
          return ok;
        });
      }
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::RecommendedPivots, TR("Recommended PivotTables###yc_dlg_pivotrec"), 640)) {
    bool close = false;
    // the source, read once per snapshot: the recommendations and a preview of each
    static std::string read_src;
    static uint64_t read_ver = ~0ull;
    static std::shared_ptr<engine::PivotCache> cache;
    static model::PivotTable base;
    static std::vector<Recommendation> recs;
    static std::string why;
    if (read_src != u.rec_source || read_ver != s.snap->version) {
      read_src = u.rec_source;
      read_ver = s.snap->version;
      base = model::PivotTable{};
      cache = std::make_shared<engine::PivotCache>();
      recs.clear();
      why.clear();
      uint32_t sid = 0;
      Rect r;
      if (model::FindTable(s.snap->wb, read_src, &sid)) {
        base.src_table = read_src;
        base.src_sheet = sid;
      } else if (ResolveRangeText(s.snap->wb, s.focus_sheet, read_src, &sid, &r)) {
        base.src_sheet = sid;
        base.src = r;
      }
      if (engine::ReadPivotSource(s.snap->wb, base, cache.get(), &why)) recs = Recommend(*cache);
    }
    if (recs.empty()) {
      ImGui::TextWrapped("%s", why.empty() ? TR("YCGrid found no PivotTables to recommend for this data.") : why.c_str());
    } else {
      u.rec_pick = std::clamp(u.rec_pick, 0, static_cast<int>(recs.size()) - 1);
      ImGui::BeginChild("##reclist", ImVec2(S(250), S(300)), ImGuiChildFlags_Borders);
      for (int i = 0; i < static_cast<int>(recs.size()); ++i) {
        if (ImGui::Selectable(recs[static_cast<size_t>(i)].title.c_str(), u.rec_pick == i, ImGuiSelectableFlags_AllowDoubleClick)) {
          u.rec_pick = i;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) close = true;
        }
      }
      ImGui::EndChild();
      ImGui::SameLine();
      // the preview: the report's first rows
      ImGui::BeginChild("##recprev", ImVec2(S(350), S(300)), ImGuiChildFlags_Borders);
      const Recommendation& rc = recs[static_cast<size_t>(u.rec_pick)];
      model::PivotTable probe = base;
      probe.fields = cache->names;
      model::PivotAxisField af;
      af.field = rc.row;
      probe.rows.push_back(af);
      model::PivotDataField df;
      df.field = rc.value;
      df.func = rc.func;
      probe.data.push_back(df);
      const engine::PivotReport rep = engine::ComputePivot(probe, *cache);
      if (ImGui::BeginTable("##recgrid", static_cast<int>(std::max<uint32_t>(1, std::min<uint32_t>(rep.cols, 6))), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (uint32_t r = 0; r < rep.rows && r < 16; ++r) {
          ImGui::TableNextRow();
          for (uint32_t c = 0; c < rep.cols && c < 6; ++c) {
            ImGui::TableSetColumnIndex(static_cast<int>(c));
            const engine::PivotCell& cell = rep.at(r, c);
            const std::string t = engine::DisplayTextOf(cell.v, cell.numfmt.empty() ? "General" : cell.numfmt);
            if (cell.bold) ImGui::TextColored(ImVec4(0.1f, 0.1f, 0.1f, 1), "%s", t.c_str());
            else ImGui::TextUnformatted(t.c_str());
          }
        }
        ImGui::EndTable();
      }
      ImGui::EndChild();
    }
    ImGui::TextDisabled("%s %s", TR("Source:"), u.rec_source.c_str());
    if ((DlgOkButton(TR("OK"), ImVec2(S(80), 0)) || close) && !recs.empty() && !u.made) {
      const Recommendation rc = recs[static_cast<size_t>(u.rec_pick)];
      model::PivotTable p = base;
      p.r0 = 2;  // A3 of a new sheet, like Excel
      p.c0 = 0;
      model::PivotAxisField af;
      af.field = rc.row;
      p.rows.push_back(af);
      model::PivotDataField df;
      df.field = rc.value;
      df.func = rc.func;
      p.data.push_back(df);
      auto made = std::make_shared<PivotUi::Made>();
      made->r0 = p.r0;
      made->c0 = p.c0;
      u.made = made;
      const uint32_t focus = s.focus_sheet;
      const std::string label = TR("Insert PivotTable");
      s.engine->Post([made, p, focus, label](Document& d) {
        std::string err;
        d.Begin(label);
        const uint32_t target = d.AddSheet({}, d.wb().IndexOfId(focus));
        made->sheet = target;
        made->id = d.AddPivot(target, p, &err);
        d.Commit();
        made->err = err;
        made->done = true;
      });
      close = true;
    } else if (close && recs.empty()) {
      close = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotGroup, TR("Grouping###yc_dlg_pivotgroup"), 380)) {
    ImGui::SeparatorText(TR("Auto"));
    ImGui::Checkbox(TR("Starting at:"), &u.grp_auto_start);
    ImGui::SameLine(S(140));
    ImGui::SetNextItemWidth(S(160));
    ImGui::BeginDisabled(u.grp_auto_start);
    if (u.grp_dates) ImGui::InputText("##start", u.grp_start_text, sizeof(u.grp_start_text));
    else ImGui::InputDouble("##start", &u.grp_start, 0, 0, "%g");
    ImGui::EndDisabled();
    ImGui::Checkbox(TR("Ending at:"), &u.grp_auto_end);
    ImGui::SameLine(S(140));
    ImGui::SetNextItemWidth(S(160));
    ImGui::BeginDisabled(u.grp_auto_end);
    if (u.grp_dates) ImGui::InputText("##end", u.grp_end_text, sizeof(u.grp_end_text));
    else ImGui::InputDouble("##end", &u.grp_end, 0, 0, "%g");
    ImGui::EndDisabled();
    ImGui::SeparatorText(TR("By"));
    if (u.grp_dates) {
      // several periods: the field shows the finest, the others become fields of their own
      struct P {
        const char* label;
        uint32_t bit;
      };
      static const P kPeriods[] = {{TR("Seconds"), model::kPeriodSeconds}, {TR("Minutes"), model::kPeriodMinutes}, {TR("Hours"), model::kPeriodHours},
                                   {TR("Days"), model::kPeriodDays},       {TR("Months"), model::kPeriodMonths},   {TR("Quarters"), model::kPeriodQuarters},
                                   {TR("Years"), model::kPeriodYears}};
      if (ImGui::BeginListBox("##periods", ImVec2(S(200), S(150)))) {
        for (const P& pr : kPeriods) {
          if (ImGui::Selectable(pr.label, (u.grp_periods & pr.bit) != 0)) {
            u.grp_periods ^= pr.bit;
            if (u.grp_periods == 0) u.grp_periods = pr.bit;
          }
        }
        ImGui::EndListBox();
      }
      ImGui::BeginDisabled(u.grp_periods != model::kPeriodDays);
      ImGui::SetNextItemWidth(S(100));
      ImGui::InputInt(TR("Number of days"), &u.grp_days);
      u.grp_days = std::clamp(u.grp_days, 1, 32767);
      ImGui::EndDisabled();
    } else {
      ImGui::SetNextItemWidth(S(160));
      ImGui::InputDouble("##by", &u.grp_interval, 0, 0, "%g");
    }
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      model::PivotGroup g;
      g.field = u.grp_field;
      g.auto_start = u.grp_auto_start;
      g.auto_end = u.grp_auto_end;
      bool ok = true;
      if (u.grp_dates) {
        g.periods = u.grp_periods;
        g.days = u.grp_days;
        // older YCGrid versions read one period: the nearest
        const uint32_t pr = u.grp_periods;
        g.by = (pr & model::kPeriodYears) && (pr & model::kPeriodMonths) ? model::PivotGroupBy::YearMonths
               : (pr & model::kPeriodDays)                               ? model::PivotGroupBy::Days
               : (pr & model::kPeriodMonths)                             ? model::PivotGroupBy::Months
               : (pr & model::kPeriodQuarters)                           ? model::PivotGroupBy::Quarters
               : (pr & model::kPeriodYears)                              ? model::PivotGroupBy::Years
                                                                         : model::PivotGroupBy::Days;
        auto parse_date = [&](const char* t, double* out) {
          const core::Value v = s.engine->Call([&](Document& d) {
            std::string e;
            return d.EvaluateFormula(s.focus_sheet, std::string("DATEVALUE(\"") + t + "\")", &e);
          });
          if (!v.IsNumeric()) return false;
          *out = std::floor(v.AsDouble());
          return true;
        };
        if (!g.auto_start) ok = ok && parse_date(u.grp_start_text, &g.start);
        if (!g.auto_end) ok = ok && parse_date(u.grp_end_text, &g.end);
        if (!ok) s.ShowMessage("YCGrid", TR("Enter dates for Starting at and Ending at."));
      } else {
        g.by = model::PivotGroupBy::Numbers;
        g.start = u.grp_start;
        g.end = u.grp_end;
        g.interval = u.grp_interval;
        if (!(u.grp_interval > 0)) {
          ok = false;
          s.ShowMessage("YCGrid", TR("The group size must be greater than zero."));
        } else if (!g.auto_end && !g.auto_start && g.end < g.start) {
          ok = false;
          s.ShowMessage("YCGrid", TR("The ending number must be greater than the starting number."));
        }
      }
      if (ok) {
        EditPivot(s, TR("Group"), [g](PivotTable& x) {
          x.groups.erase(std::remove_if(x.groups.begin(), x.groups.end(), [&](const model::PivotGroup& o) { return o.field == g.field; }), x.groups.end());
          x.groups.push_back(g);
        });
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotCalcItem, TR("Insert Calculated Item###yc_dlg_pivotitem"), 460)) {
    const PivotTable* p = ActivePivot(s);
    bool close = p == nullptr;
    if (p) {
      const Cached& c = CacheFor(s, *p);
      const bool have = c.ok;
      const int nsrc = have ? static_cast<int>(c.cache->columns) : 0;
      u.item_field = std::clamp(u.item_field, 0, std::max(0, nsrc - 1));
      ImGui::SetNextItemWidth(S(300));
      if (ui::BeginCombo(TR("Field"), u.item_field < nsrc ? c.cache->names[static_cast<size_t>(u.item_field)].c_str() : "")) {
        for (int f = 0; f < nsrc; ++f) {
          if (ImGui::Selectable(c.cache->names[static_cast<size_t>(f)].c_str(), f == u.item_field)) u.item_field = f;
        }
        ImGui::EndCombo();
      }
      // the field's own items (calculated ones excluded)
      std::vector<std::string> items;
      if (have && u.item_field < nsrc) {
        for (const auto& it : c.cache->items[static_cast<size_t>(u.item_field)]) {
          const bool calc = std::any_of(p->calc_items.begin(), p->calc_items.end(), [&](const model::PivotCalcItem& ci) { return ci.field == u.item_field && ci.name == it; });
          if (!calc) items.push_back(it);
        }
      }
      ImGui::SetNextItemWidth(S(300));
      if (ui::BeginCombo(TR("Name"), u.calc_name)) {
        for (const auto& ci : p->calc_items) {
          if (ci.field == u.item_field && ImGui::Selectable(ci.name.c_str())) {
            std::snprintf(u.calc_name, sizeof(u.calc_name), "%s", ci.name.c_str());
            std::snprintf(u.calc_formula, sizeof(u.calc_formula), "%s", ci.formula.c_str());
          }
        }
        ImGui::EndCombo();
      }
      ImGui::SetNextItemWidth(S(300));
      ImGui::InputText(TR("Name##ctx_new"), u.calc_name, sizeof(u.calc_name));
      ImGui::SetNextItemWidth(S(300));
      ImGui::InputText(TR("Formula"), u.calc_formula, sizeof(u.calc_formula));
      ImGui::TextUnformatted(TR("Items (click to insert):"));
      ImGui::BeginChild("##calcitems", ImVec2(S(300), S(120)), ImGuiChildFlags_Borders);
      for (const auto& n : items) {
        if (ImGui::Selectable(n.c_str())) {
          const bool quote = n.find_first_of(" -+*/^()'") != std::string::npos;
          std::string f = u.calc_formula;
          f += quote ? "'" + n + "'" : n;
          std::snprintf(u.calc_formula, sizeof(u.calc_formula), "%s", f.c_str());
        }
      }
      ImGui::EndChild();
      const std::string name = u.calc_name, formula = u.calc_formula;
      const int field = u.item_field;
      const bool exists = std::any_of(p->calc_items.begin(), p->calc_items.end(), [&](const model::PivotCalcItem& ci) { return ci.field == field && ci.name == name; });
      // OK adds (or changes) the item and closes, as in Excel; with nothing typed it just closes
      const bool unchanged = name.empty() || (exists && std::any_of(p->calc_items.begin(), p->calc_items.end(), [&](const model::PivotCalcItem& ci) {
                                                 return ci.field == field && ci.name == name && ci.formula == formula;
                                               }));
      const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
      if (ok_pressed && unchanged) close = true;
      ImGui::SameLine();
      if (ImGui::Button(exists ? TR("Modify") : TR("Add"), ImVec2(S(80), 0)) || (ok_pressed && !unchanged)) {
        std::vector<std::pair<std::string, double>> terms;
        std::string why;
        const bool grouped = std::any_of(p->groups.begin(), p->groups.end(), [&](const model::PivotGroup& g) { return g.field == field && g.by != model::PivotGroupBy::None; });
        if (name.empty() || (!exists && std::find(items.begin(), items.end(), name) != items.end())) {
          s.ShowMessage("YCGrid", TR("Enter a name that is not already an item of the field."));
        } else if (grouped) {
          s.ShowError("YCGrid", TR("Calculated items can't be added to a grouped field."));
        } else if (!engine::ParseCalcItem(items, formula, &terms, &why)) {
          s.ShowMessage("YCGrid", why);
        } else {
          if (ok_pressed) close = true;
          EditPivot(s, TR("Calculated Item"), [name, formula, field, exists](PivotTable& x) {
            for (auto& ci : x.calc_items) {
              if (exists && ci.field == field && ci.name == name) ci.formula = formula;
            }
            if (!exists) x.calc_items.push_back(model::PivotCalcItem{field, name, formula});
          });
        }
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!exists);
      if (ImGui::Button(TR("Delete"), ImVec2(S(80), 0))) {
        EditPivot(s, TR("Calculated Item"), [name, field](PivotTable& x) {
          x.calc_items.erase(std::remove_if(x.calc_items.begin(), x.calc_items.end(), [&](const model::PivotCalcItem& ci) { return ci.field == field && ci.name == name; }),
                             x.calc_items.end());
        });
        u.calc_name[0] = 0;
        std::snprintf(u.calc_formula, sizeof(u.calc_formula), "%s", "=");
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Close"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotCalcField, TR("Insert Calculated Field###yc_dlg_pivotcalc"), 460)) {
    const PivotTable* p = ActivePivot(s);
    bool close = p == nullptr;
    if (p) {
      ImGui::SetNextItemWidth(S(300));
      if (ui::BeginCombo(TR("Name"), u.calc_name)) {
        for (const auto& c : p->calc_fields) {
          if (ImGui::Selectable(c.name.c_str())) {
            std::snprintf(u.calc_name, sizeof(u.calc_name), "%s", c.name.c_str());
            std::snprintf(u.calc_formula, sizeof(u.calc_formula), "%s", c.formula.c_str());
          }
        }
        ImGui::EndCombo();
      }
      ImGui::SetNextItemWidth(S(300));
      ImGui::InputText(TR("Name##ctx_new"), u.calc_name, sizeof(u.calc_name));
      static int caret = -1;
      ImGui::SetNextItemWidth(S(300));
      ImGui::InputText(TR("Formula"), u.calc_formula, sizeof(u.calc_formula), ImGuiInputTextFlags_CallbackAlways,
                       [](ImGuiInputTextCallbackData* d) {
                         caret = d->CursorPos;
                         return 0;
                       });
      ImGui::TextUnformatted(TR("Fields (double-click or Insert Field):"));
      const Cached& cc = CacheFor(s, *p);
      std::vector<std::string> src;
      if (cc.ok) src.assign(cc.cache->names.begin(), cc.cache->names.begin() + static_cast<std::ptrdiff_t>(cc.cache->columns));
      const size_t n_after = cc.ok ? cc.cache->source_fields : src.size();  // calculated fields come after these
      static int field_sel = -1;
      bool insert = false;
      ImGui::BeginChild("##calcfields", ImVec2(S(300), S(120)), ImGuiChildFlags_Borders);
      for (size_t k = 0; k < src.size(); ++k) {
        if (ImGui::Selectable(src[k].c_str(), field_sel == static_cast<int>(k), ImGuiSelectableFlags_AllowDoubleClick)) {
          field_sel = static_cast<int>(k);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) insert = true;
        }
      }
      ImGui::EndChild();
      ImGui::BeginDisabled(field_sel < 0 || field_sel >= static_cast<int>(src.size()));
      if (ImGui::Button(TR("Insert Field"))) insert = true;
      ImGui::EndDisabled();
      if (insert && field_sel >= 0 && field_sel < static_cast<int>(src.size())) {
        const std::string& n = src[static_cast<size_t>(field_sel)];
        const bool quote = n.find_first_of(" -+*/^()'") != std::string::npos;
        std::string f = u.calc_formula;
        const size_t at = caret >= 0 && static_cast<size_t>(caret) <= f.size() ? static_cast<size_t>(caret) : f.size();
        const std::string ins = quote ? "'" + n + "'" : n;
        f.insert(at, ins);
        caret = static_cast<int>(at + ins.size());
        std::snprintf(u.calc_formula, sizeof(u.calc_formula), "%s", f.c_str());
      }
      const std::string name = u.calc_name, formula = u.calc_formula;
      const bool exists = std::any_of(p->calc_fields.begin(), p->calc_fields.end(), [&](const model::PivotCalcField& c) { return c.name == name; });
      // OK adds (or changes) the field and closes, as in Excel; with nothing typed it just closes
      const bool unchanged = name.empty() || (exists && std::any_of(p->calc_fields.begin(), p->calc_fields.end(), [&](const model::PivotCalcField& c) {
                                                 return c.name == name && c.formula == formula;
                                               }));
      const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
      if (ok_pressed && unchanged) close = true;
      ImGui::SameLine();
      if (ImGui::Button(exists ? TR("Modify") : TR("Add"), ImVec2(S(80), 0)) || (ok_pressed && !unchanged)) {
        if (ok_pressed) close = true;
        std::string why;
        bool clash = false;
        for (const auto& n : p->fields) clash = clash || (n == name && !exists);
        if (name.empty() || clash) {
          s.ShowMessage("YCGrid", TR("Enter a name that is not already a field of the source."));
          close = false;
        } else if (!engine::CheckPivotFormula(src, formula, &why)) {
          s.ShowMessage("YCGrid", why);
          close = false;
        } else {
          EditPivot(s, TR("Calculated Field"), [name, formula, exists](PivotTable& x) {
            if (exists) {
              for (auto& c : x.calc_fields) {
                if (c.name == name) c.formula = formula;
              }
              return;
            }
            x.calc_fields.push_back({name, formula});
            // a new calculated field goes to Values (Excel)
            model::PivotDataField df;
            df.field = static_cast<int>(x.fields.size());  // the field index after the current ones (source + earlier calculated fields)
            x.data.push_back(df);
          });
        }
      }
      ImGui::SameLine();
      ImGui::BeginDisabled(!exists);
      if (ImGui::Button(TR("Delete"), ImVec2(S(80), 0))) {
        EditPivot(s, TR("Calculated Field"), [name, n_after](PivotTable& x) {
          for (size_t i = 0; i < x.calc_fields.size(); ++i) {
            if (x.calc_fields[i].name != name) continue;
            const int f = static_cast<int>(n_after + i);
            x.data.erase(std::remove_if(x.data.begin(), x.data.end(), [&](const model::PivotDataField& d) { return d.field == f; }), x.data.end());
            for (auto& d : x.data) {
              if (d.field > f) --d.field;
            }
            x.calc_fields.erase(x.calc_fields.begin() + static_cast<std::ptrdiff_t>(i));
            break;
          }
        });
        u.calc_name[0] = 0;
        std::snprintf(u.calc_formula, sizeof(u.calc_formula), "%s", "=");
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Close"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotValueField, TR("Value Field Settings###yc_dlg_pivotvf"), 420)) {
    const PivotTable* p = ActivePivot(s);
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputText(TR("Custom Name"), u.vf_caption, sizeof(u.vf_caption));
    if (BeginDlgTabBar("##vftabs")) {
      if (ImGui::BeginTabItem(TR("Summarize Values By"), nullptr, u.vf_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
        ImGui::TextUnformatted(TR("Summarize value field by"));
        if (ImGui::BeginListBox("##func", ImVec2(S(260), S(150)))) {
          for (int i = 0; i < model::kPivotFuncs; ++i) {
            if (ImGui::Selectable(FuncLabel(i), u.vf_func == i)) u.vf_func = i;
          }
          ImGui::EndListBox();
        }
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Show Values As"), nullptr, u.vf_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
        ImGui::SetNextItemWidth(S(260));
        if (ui::BeginCombo("##show", ShowAsLabel(u.vf_show))) {
          for (int k = 0; k < model::kPivotShowAs; ++k) {
            if (ImGui::Selectable(ShowAsLabel(k), u.vf_show == k)) u.vf_show = k;
          }
          ImGui::EndCombo();
        }
        const auto kind = static_cast<model::PivotShowAs>(u.vf_show);
        const bool base = engine::PivotShowAsNeedsBase(kind), base_item = engine::PivotShowAsNeedsBaseItem(kind);
        const float half = S(128);
        ImGui::BeginDisabled(!base);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(TR("Base field:"));
        if (p && ImGui::BeginListBox("##basefield", ImVec2(half, S(130)))) {
          for (size_t f = 0; f < p->fields.size(); ++f) {
            if (!AxisFieldOf(*p, static_cast<int>(f))) continue;
            if (ImGui::Selectable(p->fields[f].c_str(), u.vf_base_field == static_cast<int>(f))) {
              u.vf_base_field = static_cast<int>(f);
              u.vf_base_item = "(previous)";
            }
          }
          ImGui::EndListBox();
        }
        ImGui::EndGroup();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!base_item);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(TR("Base item:"));
        if (p && ImGui::BeginListBox("##baseitem", ImVec2(half, S(130)))) {
          if (ImGui::Selectable(TR("(previous)"), u.vf_base_item == "(previous)")) u.vf_base_item = "(previous)";
          if (ImGui::Selectable(TR("(next)"), u.vf_base_item == "(next)")) u.vf_base_item = "(next)";
          for (const auto& [id, shown] : ItemsOf(s, *p, u.vf_base_field)) {
            ImGui::PushID(id.c_str());
            if (ImGui::Selectable(shown.c_str(), u.vf_base_item == id)) u.vf_base_item = id;
            ImGui::PopID();
          }
          ImGui::EndListBox();
        }
        ImGui::EndGroup();
        ImGui::EndDisabled();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    u.vf_tab = -1;  // chosen once, then the user's clicks pick the tab
    // Number Format...: Format Cells' Number tab over this dialog (a category and its decimals), as in Excel
    bool close = false;
    if (ImGui::Button(TR("Number Format"))) {
      std::snprintf(u.nf_format, sizeof(u.nf_format), "%s", u.vf_format);
      u.vf_nf_open = true;
      ImGui::OpenPopup(TR("Format Cells###yc_dlg_pivotvfnf"));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", u.vf_format[0] ? u.vf_format : "General");
    ImGui::SetNextWindowSize(ImVec2(S(420), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(TR("Format Cells###yc_dlg_pivotvfnf"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
      std::string code = u.nf_format;
      if (NumberFormatPicker(code)) std::snprintf(u.nf_format, sizeof(u.nf_format), "%s", code.c_str());
      bool done = false;
      if (DlgOkButton(TR("OK##vfnf"), ImVec2(S(80), 0))) {
        const std::string picked = u.nf_format;
        std::snprintf(u.vf_format, sizeof(u.vf_format), "%s", picked == "General" ? "" : picked.c_str());
        done = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel##vfnf"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
      if (done) {
        u.vf_nf_open = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    } else {
      u.vf_nf_open = false;
    }
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) && p) {
      const int index = u.vf_index, func = u.vf_func, show = u.vf_show, base_field = u.vf_base_field;
      const std::string caption = u.vf_caption, fmt = u.vf_format, base_item = u.vf_base_item;
      // a custom name may not repeat another field's name (Excel)
      bool clash = false;
      const Cached& c = CacheFor(s, *p);
      for (size_t i = 0; c.ok && i < p->data.size(); ++i) {
        if (static_cast<int>(i) != index && engine::PivotDataCaption(*p, *c.cache, i) == caption) clash = true;
      }
      for (const auto& n : p->fields) clash = clash || n == caption;
      if (clash) {
        s.ShowMessage("YCGrid", TR("PivotTable field name already exists."));
      } else {
        EditPivot(s, TR("Value Field Settings"), [index, func, show, caption, fmt, base_field, base_item](PivotTable& x) {
          if (index < 0 || static_cast<size_t>(index) >= x.data.size()) return;
          auto& df = x.data[static_cast<size_t>(index)];
          const model::PivotFunc old = df.func;
          df.func = static_cast<model::PivotFunc>(func);
          df.show_as = static_cast<model::PivotShowAs>(show);
          df.base_field = base_field;
          df.base_item = base_item;
          df.number_format = fmt;
          // the automatic caption follows the function; a typed one stays
          engine::PivotCache c2;
          c2.names = x.fields;
          model::PivotTable probe;
          probe.data = x.data;
          probe.data[static_cast<size_t>(index)].caption.clear();
          probe.data[static_cast<size_t>(index)].func = old;
          const std::string auto_old = engine::PivotDataCaption(probe, c2, static_cast<size_t>(index));
          df.caption = caption == auto_old ? std::string() : caption;
        });
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotFieldSettings, TR("Field Settings###yc_dlg_pivotfs"), 400)) {
    const PivotTable* p = ActivePivot(s);
    bool close = p == nullptr;
    if (p) {
      ImGui::TextDisabled("%s %s", TR("Source Name:"), [&]() {
        const auto* axis = AxisOf(*p, u.fs_area);
        return axis && static_cast<size_t>(u.fs_pos) < axis->size() ? p->fields[static_cast<size_t>((*axis)[static_cast<size_t>(u.fs_pos)].field)].c_str() : "";
      }());
      ImGui::SetNextItemWidth(S(240));
      ImGui::InputText(TR("Custom Name"), u.fs_caption, sizeof(u.fs_caption));
      if (BeginDlgTabBar("##fstabs")) {
        if (ImGui::BeginTabItem(TR("Subtotals & Filters"))) {
          ui::RadioButton(TR("Automatic"), &u.fs_sub, 0);
          ui::RadioButton(TR("None"), &u.fs_sub, 1);
          ui::RadioButton(TR("Custom"), &u.fs_sub, 2);
          ImGui::BeginDisabled(u.fs_sub != 2);
          if (ImGui::BeginListBox("##funcs", ImVec2(S(220), S(150)))) {
            for (int i = 0; i < model::kPivotFuncs - 1; ++i) {  // Excel's eleven (no Distinct Count)
              if (ImGui::Selectable(FuncLabel(i), (u.fs_funcs >> i) & 1)) {
                u.fs_funcs ^= 1u << i;
                if (u.fs_funcs == 0) u.fs_funcs = 1u << i;
              }
            }
            ImGui::EndListBox();
          }
          ImGui::EndDisabled();
          ImGui::SeparatorText(TR("Filter"));
          ImGui::Checkbox(TR("Include new items in manual filter"), &u.fs_new);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(TR("Layout & Print"))) {
          ImGui::SeparatorText(TR("Layout"));
          ui::RadioButton(TR("Show item labels in outline form"), &u.fs_form, 1);
          ImGui::Indent();
          ImGui::BeginDisabled(u.fs_form != 1);
          ImGui::Checkbox(TR("Display labels from the next field in the same column (compact form)"), &u.fs_compact);
          ImGui::Checkbox(TR("Display subtotals at the top of each group"), &u.fs_top);
          ImGui::EndDisabled();
          ImGui::Unindent();
          ui::RadioButton(TR("Show item labels in tabular form"), &u.fs_form, 2);
          ImGui::Checkbox(TR("Repeat item labels"), &u.fs_repeat);
          ImGui::Checkbox(TR("Insert blank line after each item label"), &u.fs_blank);
          ImGui::Checkbox(TR("Show items with no data"), &u.fs_all);
          ImGui::SeparatorText(TR("Print"));
          ImGui::Checkbox(TR("Insert page break after each item"), &u.fs_break);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        const int area = u.fs_area, pos = u.fs_pos, sub = u.fs_sub;
        const uint32_t funcs = u.fs_funcs;
        const bool rep = u.fs_repeat, blank = u.fs_blank, all = u.fs_all, inc_new = u.fs_new, brk = u.fs_break;
        const int form = u.fs_form == 2 ? 2 : u.fs_compact ? 0 : 1, top = u.fs_top ? 1 : 0;
        std::string cap = u.fs_caption;
        const auto* axis = AxisOf(*p, area);
        const int field = axis && static_cast<size_t>(pos) < axis->size() ? (*axis)[static_cast<size_t>(pos)].field : -1;
        const std::string src = field >= 0 ? p->fields[static_cast<size_t>(field)] : std::string();
        bool clash = false;
        for (size_t f = 0; f < p->fields.size(); ++f) clash = clash || (static_cast<int>(f) != field && p->fields[f] == cap);
        if (clash) {
          s.ShowMessage("YCGrid", TR("PivotTable field name already exists."));
        } else {
          if (cap == src) cap.clear();
          EditPivot(s, TR("Field Settings"), [area, pos, sub, funcs, rep, blank, all, cap, inc_new, brk, form, top](PivotTable& x) {
            auto* v = AxisOf(x, area);
            if (!v || static_cast<size_t>(pos) >= v->size()) return;
            auto& f = (*v)[static_cast<size_t>(pos)];
            f.caption = cap;
            f.subtotal = sub != 1;
            f.subtotal_funcs = sub == 2 ? funcs : 0;
            f.repeat_labels = rep;
            f.blank_line = blank;
            f.show_all_items = all;
            f.include_new = inc_new;
            f.page_break = brk;
            // the field's own form and subtotal place only where they differ from the report's (Report Layout keeps working)
            const int report_form = x.layout;
            f.form = area == kRows && form != report_form ? form : -1;
            f.sub_top = area == kRows && top != (x.subtotals == 1 ? 1 : 0) ? top : -1;
          });
          close = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotOptions, TR("PivotTable Options###yc_dlg_pivotopt"), 460)) {
    PivotTable& o = u.opt;
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputText(TR("PivotTable Name:"), u.opt_name, sizeof(u.opt_name));
    if (BeginDlgTabBar("##opttabs")) {
      if (ImGui::BeginTabItem(TR("Layout & Format"))) {
        ImGui::Checkbox(TR("Merge and center cells with labels"), &o.merge_labels);
        ImGui::SetNextItemWidth(S(80));
        ImGui::InputInt(TR("When in compact form indent row labels (characters)"), &o.indent);
        o.indent = std::clamp(o.indent, 0, 15);
        ImGui::Checkbox(TR("For error values show:"), &o.show_error);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputText("##err", u.opt_err, sizeof(u.opt_err));
        ImGui::Checkbox(TR("For empty cells show:"), &o.show_null);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(120));
        ImGui::InputText("##null", u.opt_null, sizeof(u.opt_null));
        ImGui::Checkbox(TR("Autofit column widths on update"), &o.autofit);
        ImGui::Checkbox(TR("Preserve cell formatting on update"), &o.preserve_formatting);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Totals & Filters"))) {
        ImGui::Checkbox(TR("Show grand totals for rows"), &o.col_grand);
        ImGui::Checkbox(TR("Show grand totals for columns"), &o.row_grand);
        ImGui::Checkbox(TR("Use Custom Lists when sorting"), &o.custom_lists);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Display"))) {
        ImGui::Checkbox(TR("Show expand/collapse buttons"), &o.show_buttons);
        ImGui::Checkbox(TR("Display field captions and filter drop downs"), &o.field_captions);
        ImGui::TextUnformatted(TR("Field List"));
        ui::RadioButton(TR("Sort A to Z"), &o.field_list_order, 1);
        ui::RadioButton(TR("Sort in data source order"), &o.field_list_order, 0);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Data"))) {
        ImGui::Checkbox(TR("Refresh data when opening the file"), &o.refresh_on_open);
        ImGui::Checkbox(TR("Enable show details"), &o.enable_drilldown);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      PivotTable next = o;
      next.name = u.opt_name;
      next.error_string = u.opt_err;
      next.null_string = u.opt_null;
      EditPivot(s, TR("PivotTable Options"), [next](PivotTable& x) {
        if (!next.name.empty()) x.name = next.name;
        x.merge_labels = next.merge_labels;
        x.indent = next.indent;
        x.show_error = next.show_error;
        x.error_string = next.error_string;
        x.show_null = next.show_null;
        x.null_string = next.null_string;
        x.autofit = next.autofit;
        x.preserve_formatting = next.preserve_formatting;
        x.row_grand = next.row_grand;
        x.col_grand = next.col_grand;
        x.custom_lists = next.custom_lists;
        x.show_buttons = next.show_buttons;
        x.field_captions = next.field_captions;
        x.field_list_order = next.field_list_order;
        x.refresh_on_open = next.refresh_on_open;
        x.enable_drilldown = next.enable_drilldown;
      });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotSort, TR("Sort###yc_dlg_pivotsort"), 380)) {
    const PivotTable* p = ActivePivot(s);
    bool close = p == nullptr;
    if (p) {
      const auto* axis = AxisOf(*p, u.so_area);
      const int field = axis && static_cast<size_t>(u.so_pos) < axis->size() ? (*axis)[static_cast<size_t>(u.so_pos)].field : -1;
      ImGui::TextDisabled("%s", FieldName(*p, field).c_str());
      ImGui::SeparatorText(TR("Sort options"));
      ui::RadioButton(TR("Manual (you can drag items to rearrange them)"), &u.so_mode, 0);
      ui::RadioButton(TR("Ascending (A to Z) by:"), &u.so_mode, 1);
      ui::RadioButton(TR("Descending (Z to A) by:"), &u.so_mode, 2);
      ImGui::BeginDisabled(u.so_mode == 0);
      ImGui::SetNextItemWidth(S(260));
      const Cached& c = CacheFor(s, *p);
      std::string cur = u.so_by < 0 ? FieldName(*p, field) : (c.ok && static_cast<size_t>(u.so_by) < p->data.size() ? engine::PivotDataCaption(*p, *c.cache, static_cast<size_t>(u.so_by)) : "");
      if (ui::BeginCombo("##by", cur.c_str())) {
        if (ImGui::Selectable(FieldName(*p, field).c_str(), u.so_by < 0)) u.so_by = -1;
        for (size_t d = 0; c.ok && d < p->data.size(); ++d) {
          ImGui::PushID(static_cast<int>(d));
          if (ImGui::Selectable(engine::PivotDataCaption(*p, *c.cache, d).c_str(), u.so_by == static_cast<int>(d))) u.so_by = static_cast<int>(d);
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        const int area = u.so_area, pos = u.so_pos, mode = u.so_mode, by = u.so_by;
        const std::vector<std::string> shown = ShownOrder(s, *p, area, field);
        EditPivot(s, TR("Sort"), [area, pos, mode, by, shown](PivotTable& x) {
          auto* v = AxisOf(x, area);
          if (!v || static_cast<size_t>(pos) >= v->size()) return;
          auto& f = (*v)[static_cast<size_t>(pos)];
          if (mode == 0) {
            f.manual_order = shown;  // Manual: the order as it is now, moved by hand from here
            f.sort_by = -1;
            f.descending = false;
            return;
          }
          f.manual_order.clear();
          f.descending = mode == 2;
          f.sort_by = by;
          if (by < 0) f.sort_col.clear();
        });
        close = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotFilterDlg, u.pf_kind == 0 ? TR("Label Filter###yc_dlg_pivotflt") : u.pf_kind == 1 ? TR("Value Filter###yc_dlg_pivotflt") : TR("Top 10 Filter###yc_dlg_pivotflt"), 460)) {
    const PivotTable* p = ActivePivot(s);
    bool close = p == nullptr;
    if (p) {
      const auto* axis = AxisOf(*p, u.pf_area);
      const int field = axis && static_cast<size_t>(u.pf_pos) < axis->size() ? (*axis)[static_cast<size_t>(u.pf_pos)].field : -1;
      ImGui::Text("%s (%s)", u.pf_kind == 2 ? TR("Show") : TR("Show items for which"), FieldName(*p, field).c_str());
      const Cached& c = CacheFor(s, *p);
      auto data_combo = [&]() {
        ImGui::SetNextItemWidth(S(160));
        const std::string cur = c.ok && static_cast<size_t>(u.pf_data) < p->data.size() ? engine::PivotDataCaption(*p, *c.cache, static_cast<size_t>(u.pf_data)) : "";
        if (ui::BeginCombo("##data", cur.c_str())) {
          for (size_t d = 0; c.ok && d < p->data.size(); ++d) {
            ImGui::PushID(static_cast<int>(d));
            if (ImGui::Selectable(engine::PivotDataCaption(*p, *c.cache, d).c_str(), u.pf_data == static_cast<int>(d))) u.pf_data = static_cast<int>(d);
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
      };
      if (u.pf_kind == 2) {
        static const char* kTop[] = {TR("Top"), TR("Bottom")};
        static const char* kUnit[] = {TR("Items"), TR("Percent"), TR("Sum")};
        ImGui::SetNextItemWidth(S(90));
        ui::Combo("##top", &u.pf_top, kTop, 2);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(70));
        ImGui::InputText("##n", u.pf_v1, sizeof(u.pf_v1), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(90));
        ui::Combo("##unit", &u.pf_unit, kUnit, 3);
        ImGui::SameLine();
        ImGui::TextUnformatted(TR("by"));
        ImGui::SameLine();
        data_combo();
      } else {
        if (u.pf_kind == 1) {
          data_combo();
          ImGui::SameLine();
        }
        static const int kLabelTypes[] = {15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
        static const int kValueTypes[] = {7, 8, 9, 10, 11, 12, 13, 14};
        static const char* kNames[] = {"", "", "", "", "", "", "", TR("equals"), TR("does not equal"), TR("is greater than"), TR("is greater than or equal to"),
                                       TR("is less than"), TR("is less than or equal to"), TR("is between"), TR("is not between"), TR("equals"), TR("does not equal"),
                                       TR("begins with"), TR("does not begin with"), TR("ends with"), TR("does not end with"), TR("contains"), TR("does not contain"),
                                       TR("is greater than"), TR("is greater than or equal to"), TR("is less than"), TR("is less than or equal to"), TR("is between"),
                                       TR("is not between")};
        ImGui::SetNextItemWidth(S(180));
        if (ui::BeginCombo("##type", kNames[std::clamp(u.pf_type, 0, 28)])) {
          if (u.pf_kind == 0) {
            for (int t : kLabelTypes) {
              if (ImGui::Selectable(kNames[t], u.pf_type == t)) u.pf_type = t;
            }
          } else {
            for (int t : kValueTypes) {
              if (ImGui::Selectable(kNames[t], u.pf_type == t)) u.pf_type = t;
            }
          }
          ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(S(140));
        ImGui::InputText("##v1", u.pf_v1, sizeof(u.pf_v1));
        if (u.pf_type == 13 || u.pf_type == 14 || u.pf_type == 27 || u.pf_type == 28) {
          ImGui::SameLine();
          ImGui::TextUnformatted(TR("and"));
          ImGui::SameLine();
          ImGui::SetNextItemWidth(S(140));
          ImGui::InputText("##v2", u.pf_v2, sizeof(u.pf_v2));
        }
        if (u.pf_kind == 0) ImGui::TextDisabled("%s", TR("Use ? to represent any single character. Use * to represent any series of characters."));
      }
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        model::PivotFilter f;
        f.type = u.pf_kind == 2 ? 1 + u.pf_unit * 2 + u.pf_top : u.pf_type;
        f.data = u.pf_kind == 0 ? 0 : u.pf_data;
        f.v1 = u.pf_v1;
        f.v2 = u.pf_v2;
        const bool label_kind = u.pf_kind == 0;
        const int area = u.pf_area, pos = u.pf_pos;
        // one condition filter per field (Excel): a label filter replaces a value filter and the other way round
        EditPivot(s, TR("Filter"), [area, pos, f, label_kind](PivotTable& x) {
          auto* v = AxisOf(x, area);
          if (!v || static_cast<size_t>(pos) >= v->size()) return;
          auto& af = (*v)[static_cast<size_t>(pos)];
          if (label_kind) {
            af.label_filter = f;
            af.value_filter = {};
          } else {
            af.value_filter = f;
            af.label_filter = {};
          }
        });
        close = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotShowDetail, TR("Show Detail###yc_dlg_pivotsd"), 320)) {
    const PivotTable* p = ActivePivot(s);
    bool close = p == nullptr;
    if (p) {
      ImGui::TextUnformatted(TR("Choose the field containing the detail you want to show:"));
      ImGui::BeginChild("##sdfields", ImVec2(S(280), S(200)), ImGuiChildFlags_Borders);
      const Cached& c = CacheFor(s, *p);
      const size_t n = c.ok ? c.cache->source_fields : 0;
      for (size_t f = 0; f < n && f < p->fields.size(); ++f) {
        if (AxisFieldOf(*p, static_cast<int>(f))) continue;
        if (ImGui::Selectable(p->fields[f].c_str(), u.sd_pick == static_cast<int>(f), ImGuiSelectableFlags_AllowDoubleClick)) {
          u.sd_pick = static_cast<int>(f);
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) close = true;
        }
      }
      ImGui::EndChild();
      const bool ok = DlgOkButton(TR("OK"), ImVec2(S(80), 0)) || close;
      close = false;
      if (ok && u.sd_pick >= 0) {
        const int area = u.sd_area, pos = u.sd_pos, field = u.sd_pick;
        EditPivot(s, TR("Show Detail"), [area, pos, field](PivotTable& x) {
          ApplyMove(x, DragField{field, kNone, -1}, area, pos + 1, model::PivotFunc::Sum);
        });
        close = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    }
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::PivotNumberFormat, TR("Number Format###yc_dlg_pivotnf"), 420)) {
    std::string code = u.nf_format;
    if (NumberFormatPicker(code)) std::snprintf(u.nf_format, sizeof(u.nf_format), "%s", code.c_str());
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const int index = u.nf_index;
      const std::string fmt = u.nf_format;
      EditPivot(s, TR("Number Format"), [index, fmt](PivotTable& x) {
        if (index >= 0 && static_cast<size_t>(index) < x.data.size()) x.data[static_cast<size_t>(index)].number_format = fmt == "General" ? std::string() : fmt;
      });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
}

// ---- timelines ----

struct Period {
  double from, to;  // serial days, inclusive
  std::string label, group;
};

std::vector<Period> TimelinePeriods(double dmin, double dmax, int level) {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  std::vector<Period> out;
  int y0 = 0, m0 = 0, d0 = 0, y1 = 0, m1 = 0, d1 = 0;
  if (dmin > dmax || !formula::SerialToYmd(dmin, &y0, &m0, &d0) || !formula::SerialToYmd(dmax, &y1, &m1, &d1)) return out;
  if (level == 0) {
    for (int y = y0; y <= y1; ++y) out.push_back({formula::DateSerial(y, 1, 1), formula::DateSerial(y, 12, 31), std::to_string(y), ""});
  } else if (level == 1) {
    for (int q = (y0 * 4 + (m0 - 1) / 3); q <= y1 * 4 + (m1 - 1) / 3; ++q) {
      const int y = q / 4, qq = q % 4;
      out.push_back({formula::DateSerial(y, qq * 3 + 1, 1), formula::DateSerial(y, qq * 3 + 4, 0), "Q" + std::to_string(qq + 1), std::to_string(y)});
    }
  } else if (level == 2) {
    for (int k = y0 * 12 + m0 - 1; k <= y1 * 12 + m1 - 1; ++k) {
      const int y = k / 12, m = k % 12 + 1;
      out.push_back({formula::DateSerial(y, m, 1), formula::DateSerial(y, m + 1, 0), kMonths[m - 1], std::to_string(y)});
    }
  } else {
    for (double d = dmin; d <= dmax && out.size() < 20000; d += 1) {
      int y = 0, m = 0, dd = 0;
      formula::SerialToYmd(d, &y, &m, &dd);
      out.push_back({d, d, std::to_string(dd), std::string(kMonths[m - 1]) + " " + std::to_string(y)});
    }
  }
  return out;
}

void DrawTimelineBody(AppState& s, const model::Slicer& sl, const PivotTable* pt, const ImRect& body, double dmin, double dmax, double from, double to) {
  PivotUi& u = U();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const char* const kLevels[] = {TR("YEARS"), TR("QUARTERS"), TR("MONTHS"), TR("DAYS")};
  const int level = std::clamp(sl.level, 0, 3);
  const std::vector<Period> per = TimelinePeriods(dmin, dmax, level);
  const float lh = ImGui::GetFontSize();
  // the selection text and the level menu (Timeline tab > Show: Selection Label, Time Level)
  const bool show_label = !(sl.tl_hide & 1), show_scroll = !(sl.tl_hide & 2), show_level = !(sl.tl_hide & 4);
  const std::string sel = from <= to ? DateText(from) + " - " + DateText(to) : std::string(TR("All Periods"));
  if (show_label) dl->AddText(ImVec2(body.Min.x, body.Min.y), IM_COL32(0x40, 0x40, 0x40, 255), sel.c_str());
  const std::string lv = std::string(kLevels[level]) + " \xE2\x96\xBE";
  const ImVec2 lts = ImGui::CalcTextSize(lv.c_str());
  const ImRect lr(ImVec2(body.Max.x - lts.x - S(4), body.Min.y), ImVec2(body.Max.x, body.Min.y + lh + S(2)));
  if (show_level) {
    ImGui::SetCursorScreenPos(lr.Min);
    if (ImGui::InvisibleButton("##level", ImMax(lr.GetSize(), ImVec2(1, 1)))) ImGui::OpenPopup("##tllevel");
    ui::NoteUiItem("Timeline Level", lr);
    dl->AddText(lr.Min, ImGui::IsItemHovered() ? IM_COL32(0x1F, 0x4E, 0x79, 255) : IM_COL32(0x26, 0x26, 0x26, 255), lv.c_str());
  }
  if (ImGui::BeginPopup("##tllevel")) {
    for (int k = 0; k < 4; ++k) {
      if (ImGui::MenuItem(kLevels[k], nullptr, k == level)) {
        const uint32_t sheet = s.focus_sheet, id = sl.id;
        s.engine->PostUser(TR("Timeline"), [sheet, id, k](Document& d) { d.UpdateSlicer(sheet, id, [k](model::Slicer& x) { x.level = k; }); });
      }
    }
    ImGui::EndPopup();
  }
  if (per.empty() || !pt) {
    dl->AddText(ImVec2(body.Min.x, body.Min.y + lh * 2), IM_COL32(0x90, 0x90, 0x90, 255), TR("No dates in this field"));
    return;
  }
  // the strip: group labels (years / months), then one cell per period; the scroll bar under it
  const float sbh = show_scroll ? S(8) : 0.0f;
  const float top0 = show_label || show_level ? lh * 1.3f : 0.0f;
  const float cw = level == 3 ? S(26) : S(42), ch = std::max(S(16), body.Max.y - body.Min.y - top0 - lh * 1.3f - sbh - S(2));
  const float sy = body.Min.y + top0 + lh * 1.1f, gy = body.Min.y + top0;
  const float total = cw * static_cast<float>(per.size());
  float& sc = u.tl_scroll[sl.id];
  const bool over = ImRect(ImVec2(body.Min.x, gy), body.Max).Contains(io.MousePos);
  if (over && io.MouseWheel != 0) sc -= io.MouseWheel * cw * 3;
  const float max_sc = std::max(0.0f, total - body.GetWidth());
  sc = std::clamp(sc, 0.0f, max_sc);
  if (show_scroll && max_sc > 0) {
    // the scroll bar: drag its thumb (or click the track) to see other periods
    const ImRect track(ImVec2(body.Min.x, body.Max.y - sbh), body.Max);
    const float tw = std::max(S(20), track.GetWidth() * body.GetWidth() / total);
    const float tx = track.Min.x + (track.GetWidth() - tw) * (sc / max_sc);
    ImGui::SetCursorScreenPos(track.Min);
    ImGui::InvisibleButton("##tlscroll", ImMax(track.GetSize(), ImVec2(1, 1)));
    ui::NoteUiItem("Timeline Scrollbar", track);
    if (ImGui::IsItemActive()) {
      const float rel = (io.MousePos.x - track.Min.x - tw / 2) / std::max(1.0f, track.GetWidth() - tw);
      sc = std::clamp(rel, 0.0f, 1.0f) * max_sc;
    }
    dl->AddRectFilled(track.Min, track.Max, IM_COL32(0xF0, 0xF0, 0xF0, 255), S(3));
    dl->AddRectFilled(ImVec2(tx, track.Min.y + 1), ImVec2(tx + tw, track.Max.y - 1), ImGui::IsItemHovered() || ImGui::IsItemActive() ? IM_COL32(0x90, 0x90, 0x90, 255) : IM_COL32(0xB8, 0xB8, 0xB8, 255), S(3));
  }
  const float x0 = body.Min.x - sc;
  // the selection's edge handles (submitted before the periods: small handles first); a drag moves that edge
  if (from <= to && u.tl_drag != sl.id) {
    int sa = -1, sb = -1;
    for (size_t i = 0; i < per.size(); ++i) {
      if (per[i].to >= from && per[i].from <= to) {
        if (sa < 0) sa = static_cast<int>(i);
        sb = static_cast<int>(i);
      }
    }
    if (sa >= 0) {
      const float hw = S(5);
      for (int edge = 0; edge < 2; ++edge) {
        const float ex = x0 + cw * static_cast<float>(edge == 0 ? sa : sb + 1);
        if (ex < body.Min.x - hw || ex > body.Max.x + hw) continue;
        const ImRect hr(ImVec2(ex - hw, sy), ImVec2(ex + hw, sy + ch));
        ImGui::SetCursorScreenPos(hr.Min);
        ImGui::PushID(edge);
        ImGui::InvisibleButton("##tlhandle", ImMax(hr.GetSize(), ImVec2(1, 1)));
        ui::NoteUiItem(edge == 0 ? "Timeline Start Handle" : "Timeline End Handle", hr);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
          u.tl_drag = sl.id;
          u.tl_a = edge == 0 ? sb : sa;  // the other edge stays
        }
        ImGui::PopID();
        dl->AddRectFilled(ImVec2(ex - S(2), sy + ch * 0.2f), ImVec2(ex + S(2), sy + ch * 0.8f), IM_COL32(0x2F, 0x6E, 0xA8, 255), S(2));
      }
    }
  }
  dl->PushClipRect(ImVec2(body.Min.x, gy), ImVec2(body.Max.x, body.Max.y - sbh), true);
  // during a drag the selection follows the mouse
  int a = -1, b = -1;
  if (u.tl_drag == sl.id && u.tl_a >= 0) {
    const int hov = std::clamp(static_cast<int>((io.MousePos.x - x0) / cw), 0, static_cast<int>(per.size()) - 1);
    a = std::min(u.tl_a, hov), b = std::max(u.tl_a, hov);
  }
  std::string last_group;
  for (size_t i = 0; i < per.size(); ++i) {
    const float px = x0 + cw * static_cast<float>(i);
    if (px + cw < body.Min.x || px > body.Max.x) {
      last_group = per[i].group;
      continue;
    }
    if (per[i].group != last_group || (i > 0 && px < body.Min.x + 1)) {
      dl->AddText(ImVec2(std::max(px, body.Min.x) + S(2), gy), IM_COL32(0x59, 0x59, 0x59, 255), per[i].group.c_str());
      last_group = per[i].group;
    }
    const bool on = a >= 0 ? (static_cast<int>(i) >= a && static_cast<int>(i) <= b) : (from <= to && per[i].to >= from && per[i].from <= to);
    const ImRect cr(ImVec2(px + 1, sy), ImVec2(px + cw - 1, sy + ch));
    dl->AddRectFilled(cr.Min, cr.Max, on ? IM_COL32(0x5B, 0x9B, 0xD5, 255) : IM_COL32(0xE4, 0xE4, 0xE4, 255));
    const ImVec2 ts = ImGui::CalcTextSize(per[i].label.c_str());
    dl->AddText(ImVec2(px + (cw - ts.x) / 2, sy + ch + S(1) - (ch > lh * 2 ? ch / 2 + ts.y / 2 : 0)), on ? IM_COL32_WHITE : IM_COL32(0x40, 0x40, 0x40, 255),
                per[i].label.c_str());
    ImGui::SetCursorScreenPos(cr.Min);
    ImGui::PushID(static_cast<int>(i));
    ImGui::InvisibleButton("##per", ImMax(cr.GetSize(), ImVec2(1, 1)));
    if (ImGui::IsItemActivated()) {
      u.tl_drag = sl.id;
      // Shift+click extends the current selection
      int start = static_cast<int>(i);
      if (io.KeyShift && from <= to) {
        for (size_t k = 0; k < per.size(); ++k) {
          if (per[k].to >= from && per[k].from <= to) {
            start = per[k].from <= per[i].from ? static_cast<int>(k) : start;
            break;
          }
        }
      }
      u.tl_a = start;
    }
    ImGui::PopID();
  }
  dl->PopClipRect();
  if (u.tl_drag == sl.id && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (a >= 0) {
      const uint32_t sheet = s.focus_sheet, id = sl.id;
      const double f = per[static_cast<size_t>(a)].from, t = per[static_cast<size_t>(b)].to;
      PivotDocOp(s, TR("Timeline"), [sheet, id, f, t](Document& d, std::string* err) { return d.SetSlicerDates(sheet, id, f, t, err); });
    }
    u.tl_drag = 0;
    u.tl_a = -1;
  }
}

namespace {

// the filter drop-down of a field (the report's buttons and a PivotChart's field buttons)
void DrawFilterPopupBody(AppState& s, const PivotTable& p, int area) {
  PivotUi& u = U();
  const auto& axis = *AxisOf(p, area);
  if (axis.empty()) return;
  if (area != kFilters && axis.size() > 1) {
    // which field of the axis
    std::vector<std::string> names;
    for (const auto& f : axis) names.push_back(FieldName(p, f.field));
    std::vector<const char*> ptrs;
    for (const auto& n : names) ptrs.push_back(n.c_str());
    ImGui::SetNextItemWidth(S(200));
    if (ui::Combo(TR("Select field:"), &u.filter_field_pos, ptrs.data(), static_cast<int>(ptrs.size()))) u.filter_items.clear();
  }
  const int pos = std::clamp(u.filter_field_pos, 0, static_cast<int>(axis.size()) - 1);
  const auto& af = axis[static_cast<size_t>(pos)];
  if (u.filter_items.empty()) {
    for (const auto& [id, shown] : ItemsOf(s, p, af.field)) {
      u.filter_items.push_back(id);
      u.filter_shown.push_back(shown);
    }
    u.filter_on.assign(u.filter_items.size(), 1);
    for (size_t i = 0; i < u.filter_items.size(); ++i) {
      if (engine::PivotItemHidden(af, u.filter_items[i])) u.filter_on[i] = 0;
    }
    u.filter_multi = af.multi_page || (area == kFilters && !af.hidden.empty());
    u.filter_search[0] = 0;
  }
  const std::string fname = FieldName(p, af.field);
  if (area != kFilters) {
    auto sort = [&](bool desc) {
      EditPivot(s, TR("Sort"), [area, pos, desc](PivotTable& x) {
        auto& f = (*AxisOf(x, area))[static_cast<size_t>(pos)];
        f.descending = desc;
        f.sort_by = -1;
        f.manual_order.clear();
      });
    };
    if (ImGui::MenuItem(TR("Sort A to Z"))) sort(false);
    if (ImGui::MenuItem(TR("Sort Z to A"))) sort(true);
    if (ImGui::MenuItem(TR("More Sort Options..."))) OpenSort(s, p, area, pos);
    ImGui::Separator();
    if (ImGui::MenuItem((std::string(TR("Clear Filter From \"")) + fname + "\"").c_str(), nullptr, false, Filtered(af))) {
      ClearFilterOf(s, area, pos);
      ImGui::CloseCurrentPopup();
    }
    LabelFilterMenu(s, p, area, pos);
    ImGui::Separator();
  }
  ImGui::SetNextItemWidth(S(220));
  ImGui::InputTextWithHint("##fsearch", TR("Search"), u.filter_search, sizeof(u.filter_search));
  std::string q = u.filter_search;
  std::transform(q.begin(), q.end(), q.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  auto matches = [&](size_t i) {
    if (q.empty()) return true;
    std::string t = u.filter_shown[i];
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return t.find(q) != std::string::npos;
  };
  if (area == kFilters && !u.filter_multi) {
    // a report filter: one item, or (All)
    if (ImGui::Selectable(TR("(All)"), af.page_item.empty() && af.hidden.empty())) {
      EditPivot(s, TR("Filter"), [pos](PivotTable& x) {
        x.pages[static_cast<size_t>(pos)].page_item.clear();
        x.pages[static_cast<size_t>(pos)].hidden.clear();
      });
      ImGui::CloseCurrentPopup();
    }
    ImGui::BeginChild("##items", ImVec2(S(220), S(200)), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < u.filter_items.size(); ++i) {
      if (!matches(i)) continue;
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(u.filter_shown[i].c_str(), af.page_item == u.filter_items[i])) {
        const std::string it = u.filter_items[i];
        EditPivot(s, TR("Filter"), [pos, it](PivotTable& x) {
          auto& f = x.pages[static_cast<size_t>(pos)];
          f.page_item = it;
          f.hidden.clear();
          f.multi_page = false;
        });
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Checkbox(TR("Select Multiple Items"), &u.filter_multi);
    return;
  }
  bool all = true, any = false;
  for (size_t i = 0; i < u.filter_items.size(); ++i) all = all && (!matches(i) || u.filter_on[i]), any = any || (matches(i) && u.filter_on[i]);
  if (CheckboxMixed(q.empty() ? TR("(Select All)") : TR("(Select All Search Results)"), &all, any && !all)) {
    for (size_t i = 0; i < u.filter_items.size(); ++i) {
      if (matches(i)) u.filter_on[i] = static_cast<char>(all ? 1 : 0);
    }
  }
  ImGui::BeginChild("##items", ImVec2(S(220), S(200)), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < u.filter_items.size(); ++i) {
    if (!matches(i)) continue;
    bool on = u.filter_on[i] != 0;
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Checkbox(u.filter_shown[i].c_str(), &on)) u.filter_on[i] = on ? 1 : 0;
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (area == kFilters) ImGui::Checkbox(TR("Select Multiple Items"), &u.filter_multi);
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    // a search keeps only the matching items (Excel); the others are hidden
    std::vector<std::string> hidden;
    for (size_t i = 0; i < u.filter_items.size(); ++i) {
      if (!u.filter_on[i] || !matches(i)) hidden.push_back(u.filter_items[i]);
    }
    if (hidden.size() == u.filter_items.size()) {
      s.ShowMessage("YCGrid", TR("At least one item must be visible."));
    } else {
      const bool page = area == kFilters;
      EditPivot(s, TR("Filter"), [area, pos, hidden, page](PivotTable& x) {
        auto& f = (*AxisOf(x, area))[static_cast<size_t>(pos)];
        f.hidden = hidden;
        if (page) {
          f.page_item.clear();
          f.multi_page = true;
        }
      });
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) ImGui::CloseCurrentPopup();
}

// a field's filter button: the drop-down arrow, a funnel when filtered, a sort arrow when sorted
void DrawFieldButton(ImDrawList* dl, const ImRect& br, bool hov, const model::PivotAxisField* af) {
  dl->AddRectFilled(br.Min, br.Max, hov ? IM_COL32(0xE5, 0xF1, 0xFB, 255) : IM_COL32(0xFF, 0xFF, 0xFF, 255));
  dl->AddRect(br.Min, br.Max, IM_COL32(0xA0, 0xA0, 0xA0, 255));
  const ImVec2 c = br.GetCenter();
  const float a = br.GetHeight() * 0.22f;
  const ImU32 ink = IM_COL32(0x40, 0x40, 0x40, 255);
  const bool filtered = af && Filtered(*af);
  const bool sorted = af && (af->descending || af->sort_by >= 0 || !af->manual_order.empty());
  if (filtered) {
    // a funnel, with the small arrow at its side
    dl->AddTriangleFilled(ImVec2(c.x - a * 1.5f, c.y - a), ImVec2(c.x + a * 0.5f, c.y - a), ImVec2(c.x - a * 0.5f, c.y + a * 0.1f), ink);
    dl->AddRectFilled(ImVec2(c.x - a * 0.7f, c.y), ImVec2(c.x - a * 0.3f, c.y + a), ink);
    dl->AddTriangleFilled(ImVec2(c.x + a * 0.5f, c.y + a * 0.2f), ImVec2(c.x + a * 1.5f, c.y + a * 0.2f), ImVec2(c.x + a, c.y + a * 0.8f), ink);
  } else if (sorted) {
    dl->AddTriangleFilled(ImVec2(c.x - a * 1.4f, c.y - a * 0.5f), ImVec2(c.x - a * 0.2f, c.y - a * 0.5f), ImVec2(c.x - a * 0.8f, c.y + a * 0.6f), ink);
    dl->AddLine(ImVec2(c.x + a * 0.8f, c.y - a), ImVec2(c.x + a * 0.8f, c.y + a), ink, 1.0f);
    dl->AddTriangleFilled(ImVec2(c.x + a * 0.3f, c.y + a * 0.4f), ImVec2(c.x + a * 1.3f, c.y + a * 0.4f), ImVec2(c.x + a * 0.8f, c.y + a), ink);
  } else {
    dl->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.5f), ImVec2(c.x + a, c.y - a * 0.5f), ImVec2(c.x, c.y + a * 0.6f), ink);
  }
}

// a PivotChart's field buttons: the data fields, the filters, the axis and legend fields (filter drop-downs)
void DrawPivotChartButtons(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  PivotUi& u = U();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr double kPx = 4.0 / 3.0;
  for (const auto& ch : *sh->charts) {
    if (ch.pivot.empty()) continue;
    uint32_t psheet = 0;
    const PivotTable* p = nullptr;
    for (const auto& sc : s.snap->wb.sheets) {
      for (const auto& x : *sc->pivots) {
        if (x.name == ch.pivot) p = &x, psheet = sc->id;
      }
    }
    if (!p || !p->chart_buttons) continue;
    const ImRect r(g.to_screen(ch.left * kPx, ch.top * kPx), g.to_screen((ch.left + ch.width) * kPx, (ch.top + ch.height) * kPx));
    if (!r.Overlaps(g.cells)) continue;
    const Cached& c = CacheFor(s, *p);
    if (!c.ok) continue;
    struct B {
      std::string label;
      int area, pos;
    };
    std::vector<B> top, bottom, right;
    for (size_t d = 0; d < p->data.size(); ++d) top.push_back(B{engine::PivotDataCaption(*p, *c.cache, d), kValues, static_cast<int>(d)});
    for (size_t i = 0; i < p->pages.size(); ++i) top.push_back(B{FieldName(*p, p->pages[i].field), kFilters, static_cast<int>(i)});
    for (size_t i = 0; i < p->rows.size(); ++i) bottom.push_back(B{FieldName(*p, p->rows[i].field), kRows, static_cast<int>(i)});
    for (size_t i = 0; i < p->cols.size(); ++i) right.push_back(B{FieldName(*p, p->cols[i].field), kColumns, static_cast<int>(i)});
    const float bh = std::floor(ImGui::GetFontSize() + S(6)), pad = S(4);
    ImGui::PushID(static_cast<int>(ch.id) + 0x40000);
    auto draw = [&](const B& b, ImVec2 at, int k) {
      const bool arrow = b.area != kValues;
      const float w = ImGui::CalcTextSize(b.label.c_str()).x + S(10) + (arrow ? bh : 0);
      const ImRect br(at, ImVec2(at.x + w, at.y + bh));
      ImGui::SetCursorScreenPos(br.Min);
      ImGui::PushID(k);
      const bool hit = ImGui::InvisibleButton("##cfb", ImMax(br.GetSize(), ImVec2(1, 1)));
      const bool hov = ImGui::IsItemHovered();
      dl->AddRectFilled(br.Min, br.Max, hov ? IM_COL32(0xE5, 0xF1, 0xFB, 255) : IM_COL32(0xF2, 0xF2, 0xF2, 255), S(2));
      dl->AddRect(br.Min, br.Max, IM_COL32(0xA6, 0xA6, 0xA6, 255), S(2));
      dl->AddText(ImVec2(br.Min.x + S(5), br.Min.y + S(3)), IM_COL32(0x26, 0x26, 0x26, 255), b.label.c_str());
      const model::PivotAxisField* af = b.area == kValues ? nullptr : &(*AxisOf(*p, b.area))[static_cast<size_t>(b.pos)];
      if (arrow) DrawFieldButton(dl, ImRect(ImVec2(br.Max.x - bh + S(2), br.Min.y + S(2)), ImVec2(br.Max.x - S(2), br.Max.y - S(2))), hov, af);
      if (hit) {
        u.sheet = psheet;
        u.id = p->id;
        if (b.area == kValues) {
          OpenValueField(s, *p, b.pos);
        } else {
          u.filter_area = b.area;
          u.filter_field_pos = b.pos;
          u.filter_items.clear();
          u.filter_shown.clear();
          u.filter_pos = ImVec2(br.Min.x, br.Max.y + 2);
          ImGui::OpenPopup("##chart_filter");
        }
      }
      ImGui::SetNextWindowPos(u.filter_pos, ImGuiCond_Appearing);
      if (ImGui::BeginPopup("##chart_filter")) {
        DrawFilterPopupBody(s, *p, b.area);
        ImGui::EndPopup();
      }
      ImGui::PopID();
      return w;
    };
    int k = 0;
    float x = r.Min.x + pad;
    for (const B& b : top) x += draw(b, ImVec2(x, r.Min.y + pad), k++) + pad;
    x = r.Min.x + pad;
    for (const B& b : bottom) x += draw(b, ImVec2(x, r.Max.y - pad - bh), k++) + pad;
    float y = r.Min.y + pad + bh + pad;
    for (const B& b : right) {
      const float w = ImGui::CalcTextSize(b.label.c_str()).x + S(10) + bh;
      draw(b, ImVec2(r.Max.x - pad - w, y), k++);
      y += bh + pad;
    }
    ImGui::PopID();
  }
}

}  // namespace

// slicers: a header (caption, Multi-Select, Clear Filter), item buttons; drag the header to move, the corner to resize
void DrawSlicers(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (sheet_id == s.focus_sheet) {
    // the Slicer tab follows the selected slicer; Delete removes it, Escape lets it go (Excel)
    PivotUi& su = U();
    auto& cx = ui::ribbon::Context();
    if (!cx.slicer_box) cx.slicer_box = [&s](int which, const ImRect& r) { DrawSlicerBox(s, which, r); };
    if (su.sl_select_pending && su.sl_select_sheet == sheet_id) {
      // a slicer just inserted is selected, as in Excel (its Slicer tab shows)
      for (const auto& x : *sh->slicers) {
        if (x.id > su.sl_select_after) {
          ClearObjectSelection();
          su.sl_sel = x.id, su.sl_sel_sheet = sheet_id, su.sl_select_pending = false;
        }
      }
    }
    if (su.sl_sel && (!SelectedSlicer(s) || ObjectSelected(s))) su.sl_sel = 0;  // gone, or a chart or shape was clicked since
    const model::Slicer* ssl = su.sl_sel != 0 && su.sl_sel_sheet == sheet_id ? SelectedSlicer(s) : nullptr;
    cx.slicer = ssl && !ssl->timeline;
    cx.timeline = ssl && ssl->timeline;
    if (ssl) {
      cx.tl_header = ssl->show_header;
      cx.tl_label = !(ssl->tl_hide & 1);
      cx.tl_scroll = !(ssl->tl_hide & 2);
      cx.tl_level = !(ssl->tl_hide & 4);
    }
    if ((cx.slicer || cx.timeline) && !s.edit.active && !ImGui::IsAnyItemActive() && s.dialog == Dialog::None && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        ui::ribbon::St().keytip_level == 0) {
      if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        const uint32_t id = su.sl_sel;
        su.sl_sel = 0;
        s.engine->PostUser(TR("Remove Slicer"), [sheet_id, id](Document& d) { d.DeleteSlicer(sheet_id, id); });
      } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        su.sl_sel = 0;
      } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)) {
        // Ctrl+C / Ctrl+X: the slicer is what Paste pastes (on any sheet, at the active cell)
        CopySlicer(s, *ssl, sheet_id, ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X));
      } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        // the selected slicer keeps the keys: the copy goes beside it
        if (SlicerClipPending()) PasteSlicerAt(s, sheet_id, su.clip, ssl->left + 12, ssl->top + 12);
        else ui::Commands().Execute("Paste");
      } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
        PasteSlicerAt(s, sheet_id, *ssl, ssl->left + 12, ssl->top + 12);
      }
    }
  }
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  DrawPivotChartButtons(s, sheet_id, g);
  if (sh->slicers->empty()) {
    dl->PopClipRect();
    return;
  }
  PivotUi& u = U();
  ImGuiIO& io = ImGui::GetIO();
  constexpr double kPx = 4.0 / 3.0;
  std::vector<const model::Slicer*> order;
  for (const auto& sl : *sh->slicers) order.push_back(&sl);
  std::stable_sort(order.begin(), order.end(), [](const model::Slicer* a, const model::Slicer* b) { return a->z < b->z; });
  for (const model::Slicer* slp : order) {
    model::Slicer sl = *slp;
    if (u.sl_drag == sl.id) {
      const ImVec2 dm = (io.MousePos - u.sl_mouse0) / static_cast<float>(g.sc * kPx);
      if (u.sl_mode == 1) {
        sl.left = std::max(0.0, u.sl_l0 + dm.x);
        sl.top = std::max(0.0, u.sl_t0 + dm.y);
      } else {
        sl.width = std::max(60.0, u.sl_w0 + dm.x);
        sl.height = std::max(50.0, u.sl_h0 + dm.y);
      }
    }
    const ImVec2 a = g.to_screen(sl.left * kPx, sl.top * kPx);
    const ImVec2 b = g.to_screen((sl.left + sl.width) * kPx, (sl.top + sl.height) * kPx);
    const ImRect r(a, b);
    if (!r.Overlaps(g.cells)) continue;
    // its PivotTable and the field's items (read once per snapshot)
    const PivotTable* pt = nullptr;
    for (const auto& sc : s.snap->wb.sheets) {
      for (const auto& p : *sc->pivots) {
        if (p.name == sl.pivot) pt = &p;
      }
    }
    std::vector<std::string> items, shown, hidden;
    std::vector<char> has_data;
    if (!sl.table.empty()) pt = nullptr;
    const Cached* cc = pt ? &CacheFor(s, *pt) : nullptr;
    if (!sl.table.empty()) {
      // a table's slicer: the column's items, its filter, the items the other columns' filters leave
      engine::TableSlicerState(s.snap->wb, sl, nullptr, &items, &hidden, &has_data);
      shown = items;
    } else if (cc && cc->ok && sl.field >= 0 && static_cast<size_t>(sl.field) < cc->cache->source_fields) {
      items = cc->cache->items[static_cast<size_t>(sl.field)];
      shown = cc->cache->display[static_cast<size_t>(sl.field)];
      // the field on the report: its own filter is the slicer's (one filter); otherwise the slicer's
      if (const auto* af = AxisFieldOf(*pt, sl.field)) {
        for (const auto& it : items) {
          if (engine::PivotItemHidden(*af, it)) hidden.push_back(it);
        }
      } else {
        for (const auto& f : pt->slicer_filters) {
          if (f.field == sl.field) hidden = f.hidden;
        }
      }
      // items with no data under the other filters are dimmed and listed last
      Cached& mc = const_cast<Cached&>(*cc);
      auto it = mc.with_data.find(sl.field);
      if (it == mc.with_data.end()) {
        PivotTable probe = *pt;
        for (auto* axis : {&probe.rows, &probe.cols, &probe.pages, &probe.slicer_filters}) {
          for (auto& f : *axis) {
            if (f.field == sl.field) f.hidden.clear(), f.page_item.clear(), f.label_filter = {};
          }
        }
        std::vector<char> on(items.size(), 0);
        for (size_t rec : engine::PivotFilteredRecords(probe, *cc->cache)) on[static_cast<size_t>(cc->cache->item_of[static_cast<size_t>(sl.field)][rec])] = 1;
        it = mc.with_data.emplace(sl.field, std::move(on)).first;
      }
      has_data = it->second;
    }
    // a timeline: its date range (from > to = all periods)
    double tl_from = 1, tl_to = 0, dmin = 1e300, dmax = -1e300;
    if (sl.timeline && pt) {
      for (const auto& f : pt->date_filters) {
        if (f.field == sl.field) tl_from = f.from, tl_to = f.to;
      }
      if (cc && cc->ok && sl.field >= 0 && static_cast<size_t>(sl.field) < cc->cache->columns) {
        Cached& mc = const_cast<Cached&>(*cc);
        auto it = mc.dates.find(sl.field);
        if (it == mc.dates.end()) {
          double lo = 1e300, hi = -1e300;
          for (const auto& rec : cc->cache->records) {
            const core::Value& v = rec[static_cast<size_t>(sl.field)];
            if (v.IsNumeric() && v.AsDouble() >= 1) lo = std::min(lo, std::floor(v.AsDouble())), hi = std::max(hi, std::floor(v.AsDouble()));
          }
          it = mc.dates.emplace(sl.field, std::make_pair(lo, hi)).first;
        }
        dmin = it->second.first;
        dmax = it->second.second;
      }
      if (tl_from <= tl_to) hidden = {"*"};  // for the Clear Filter button's look
    }
    ImGui::PushID(static_cast<int>(sl.id));
    bool pressed = false;  // a press anywhere on it selects the slicer (its Slicer tab)
    // Slicer Settings > Display header off: a thin strip stays to drag it by
    const float pad = S(6), hh = sl.show_header ? std::floor(ImGui::GetFontSize() + S(10)) : S(6);
    dl->AddRectFilled(r.Min, r.Max, IM_COL32_WHITE, S(3));
    dl->AddRect(r.Min, r.Max, IM_COL32(0xB4, 0xB4, 0xB4, 255), S(3));
    // header: caption, Multi-Select, Clear Filter
    const ImRect head(r.Min, ImVec2(r.Max.x, r.Min.y + hh));
    const float cb = hh - S(8);
    const ImRect clear(ImVec2(head.Max.x - pad - cb, head.Min.y + S(4)), ImVec2(head.Max.x - pad, head.Min.y + S(4) + cb));
    const ImRect multi_r = !sl.show_header ? ImRect(head.Max, head.Max)
                           : sl.timeline    ? ImRect(clear.Min, clear.Min)
                                            : ImRect(ImVec2(clear.Min.x - S(4) - cb, clear.Min.y), ImVec2(clear.Min.x - S(4), clear.Max.y));
    const bool multi = u.sl_multi.count(sl.id) != 0;
    if (sl.show_header) {
    dl->PushClipRect(head.Min, ImVec2(multi_r.Min.x - 2, head.Max.y), true);
    dl->AddText(ImVec2(head.Min.x + pad, head.Min.y + S(5)), IM_COL32(0x26, 0x26, 0x26, 255), (sl.caption.empty() ? sl.name : sl.caption).c_str());
    dl->PopClipRect();
    if (!sl.timeline) {
      ImGui::SetCursorScreenPos(multi_r.Min);
      if (ImGui::InvisibleButton("##multi", ImMax(multi_r.GetSize(), ImVec2(1, 1)))) {
        if (multi) u.sl_multi.erase(sl.id);
        else u.sl_multi.insert(sl.id);
      }
      const bool mh = ImGui::IsItemHovered();
      if (mh) ImGui::SetTooltip("%s", TR("Multi-Select"));
      if (multi || mh) dl->AddRectFilled(multi_r.Min, multi_r.Max, multi ? IM_COL32(0xBD, 0xD7, 0xEE, 255) : IM_COL32(0xEE, 0xEE, 0xEE, 255), S(2));
      // a check list glyph
      const ImU32 ink = IM_COL32(0x40, 0x40, 0x40, 255);
      for (int k = 0; k < 3; ++k) {
        const float y = multi_r.Min.y + cb * (0.25f + 0.25f * static_cast<float>(k));
        dl->AddLine(ImVec2(multi_r.Min.x + cb * 0.2f, y), ImVec2(multi_r.Min.x + cb * 0.35f, y), ink, S(1.2f));
        dl->AddLine(ImVec2(multi_r.Min.x + cb * 0.45f, y), ImVec2(multi_r.Max.x - cb * 0.15f, y), ink, S(1.2f));
      }
    }
    ImGui::SetCursorScreenPos(clear.Min);
    const bool clear_hit = ImGui::InvisibleButton("##clear", ImMax(clear.GetSize(), ImVec2(1, 1)));
    const bool clear_hov = ImGui::IsItemHovered();
    if (clear_hov) ImGui::SetTooltip("%s", TR("Clear Filter"));
    {
      const ImU32 col = hidden.empty() ? IM_COL32(0xC0, 0xC0, 0xC0, 255) : (clear_hov ? IM_COL32(0xC0, 0x30, 0x30, 255) : IM_COL32(0x40, 0x40, 0x40, 255));
      const ImVec2 c = clear.GetCenter();
      const float k = cb * 0.3f;
      dl->AddTriangleFilled(ImVec2(c.x - k * 1.4f, c.y - k), ImVec2(c.x + k * 0.6f, c.y - k), ImVec2(c.x - k * 0.4f, c.y + k * 0.2f), col);
      dl->AddLine(ImVec2(c.x + k * 0.2f, c.y + k * 0.1f), ImVec2(c.x + k * 1.2f, c.y + k * 1.1f), col, S(1.5f));
      dl->AddLine(ImVec2(c.x + k * 1.2f, c.y + k * 0.1f), ImVec2(c.x + k * 0.2f, c.y + k * 1.1f), col, S(1.5f));
    }
    if (clear_hit && !hidden.empty() && (pt || !sl.table.empty())) {
      const uint32_t id = sl.id;
      if (sl.timeline) PivotDocOp(s, TR("Clear Filter"), [sheet_id, id](Document& d, std::string* err) { return d.SetSlicerDates(sheet_id, id, 1, 0, err); });
      else PivotDocOp(s, TR("Clear Filter"), [sheet_id, id](Document& d, std::string* err) { return d.SetSlicerSelection(sheet_id, id, {}, err); });
    }
    }  // the header
    // header drag: move
    ImGui::SetCursorScreenPos(head.Min);
    ImGui::InvisibleButton("##head", ImMax(ImVec2(multi_r.Min.x - head.Min.x - 2, head.GetHeight()), ImVec2(1, 1)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    pressed = pressed || ImGui::IsItemActivated();
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      u.sl_drag = sl.id;
      u.sl_mode = 1;
      u.sl_mouse0 = io.MousePos;
      u.sl_l0 = slp->left, u.sl_t0 = slp->top, u.sl_w0 = slp->width, u.sl_h0 = slp->height;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##slmenu");
    ImGui::PushFont(ui::F().ui, ui::F().ui_size);  // a menu in the UI's font, not the zoomed cells'
    if (ImGui::BeginPopup("##slmenu")) {
      const uint32_t id = sl.id;
      u.sl_sel = sl.id, u.sl_sel_sheet = sheet_id;
      if (ImGui::MenuItem(TR("Cut"), "Ctrl+X")) CopySlicer(s, *slp, sheet_id, true);
      if (ImGui::MenuItem(TR("Copy"), "Ctrl+C")) CopySlicer(s, *slp, sheet_id, false);
      ImGui::Separator();
      if (ImGui::MenuItem(TR("Refresh"))) {
        if (pt) {
          uint32_t ps = 0;
          for (const auto& sc : s.snap->wb.sheets) {
            for (const auto& x : *sc->pivots) {
              if (x.id == pt->id && x.name == pt->name) ps = sc->id;
            }
          }
          const uint32_t pid = pt->id;
          PivotDocOp(s, TR("Refresh"), [ps, pid](Document& d, std::string* err) { return d.RefreshPivot(ps, pid, err); });
        }
      }
      if (!sl.timeline) {
        if (ImGui::MenuItem(TR("Sort A to Z"), nullptr, !sl.descending))
          s.engine->PostUser(TR("Sort"), [sheet_id, id](Document& d) { d.UpdateSlicer(sheet_id, id, [](model::Slicer& x) { x.descending = false; }); });
        if (ImGui::MenuItem(TR("Sort Z to A"), nullptr, sl.descending))
          s.engine->PostUser(TR("Sort"), [sheet_id, id](Document& d) { d.UpdateSlicer(sheet_id, id, [](model::Slicer& x) { x.descending = true; }); });
      }
      const std::string cap = sl.caption.empty() ? sl.name : sl.caption;
      if (ImGui::MenuItem((std::string(TR("Clear Filter From \"")) + cap + "\"").c_str(), "Alt+C", false, !hidden.empty())) {
        if (sl.timeline) PivotDocOp(s, TR("Clear Filter"), [sheet_id, id](Document& d, std::string* err) { return d.SetSlicerDates(sheet_id, id, 1, 0, err); });
        else PivotDocOp(s, TR("Clear Filter"), [sheet_id, id](Document& d, std::string* err) { return d.SetSlicerSelection(sheet_id, id, {}, err); });
      }
      ImGui::Separator();
      for (int cols = 1; cols <= 3; ++cols) {
        char label[32];
        std::snprintf(label, sizeof(label), TR("%d Columns"), cols);
        if (ImGui::MenuItem(label, nullptr, sl.columns == cols)) {
          s.engine->PostUser(TR("Slicer"), [sheet_id, id, cols](Document& d) { d.UpdateSlicer(sheet_id, id, [cols](model::Slicer& x) { x.columns = cols; }); });
        }
      }
      if (!sl.timeline && ImGui::MenuItem(TR("Multi-Select"), "Alt+S", multi)) {
        if (multi) u.sl_multi.erase(sl.id);
        else u.sl_multi.insert(sl.id);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(sl.timeline ? TR("Timeline Settings...") : TR("Slicer Settings..."))) {
        u.sl_sel = sl.id, u.sl_sel_sheet = sheet_id;
        OpenSlicerSettings(s);
      }
      if (sl.table.empty() && ImGui::MenuItem(TR("Report Connections..."))) {
        u.sl_sel = sl.id, u.sl_sel_sheet = sheet_id;
        OpenReportConnections(s);
      }
      if (ImGui::MenuItem(sl.timeline ? TR("Remove Timeline") : TR("Remove Slicer"))) {
        s.engine->PostUser(TR("Remove Slicer"), [sheet_id, id](Document& d) { d.DeleteSlicer(sheet_id, id); });
      }
      ImGui::EndPopup();
    }
    ImGui::PopFont();
    const ImRect body(ImVec2(r.Min.x + pad, head.Max.y + S(2)), ImVec2(r.Max.x - pad, r.Max.y - pad));
    if (sl.timeline) {
      DrawTimelineBody(s, sl, pt, body, dmin, dmax, tl_from, tl_to);
    } else {
      // items: with data first, then the dimmed ones
      std::vector<size_t> idx(items.size());
      for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
      // Slicer Settings: descending order; items with no data hidden, or dimmed (and listed last), or shown as the others
      if (sl.descending) std::reverse(idx.begin(), idx.end());
      const bool known = has_data.size() == items.size();
      const bool dims = known && sl.indicate_no_data && !sl.hide_no_data;
      if (known && sl.hide_no_data) idx.erase(std::remove_if(idx.begin(), idx.end(), [&](size_t i) { return has_data[i] == 0; }), idx.end());
      else if (dims && sl.no_data_last) std::stable_partition(idx.begin(), idx.end(), [&](size_t i) { return has_data[i] != 0; });
      const int ncol = std::max(1, sl.columns);
      const float gap = S(4), bh = std::floor(ImGui::GetFontSize() + S(8));
      const float bw = (body.GetWidth() - gap * static_cast<float>(ncol - 1)) / static_cast<float>(ncol);
      dl->PushClipRect(body.Min, body.Max, true);
      for (size_t k = 0; k < idx.size(); ++k) {
        const size_t i = idx[k];
        const int col = static_cast<int>(k % static_cast<size_t>(ncol)), row = static_cast<int>(k / static_cast<size_t>(ncol));
        const ImVec2 p0(body.Min.x + static_cast<float>(col) * (bw + gap), body.Min.y + static_cast<float>(row) * (bh + gap));
        const ImRect br(p0, ImVec2(p0.x + bw, p0.y + bh));
        if (br.Min.y > body.Max.y) break;
        const bool selected = std::find(hidden.begin(), hidden.end(), items[i]) == hidden.end();
        const bool data = !dims || has_data[i];
        ImGui::SetCursorScreenPos(br.Min);
        ImGui::PushID(static_cast<int>(i));
        const bool hit = ImGui::InvisibleButton("##it", ImMax(br.GetSize(), ImVec2(1, 1)));
        const bool hov = ImGui::IsItemHovered();
        pressed = pressed || ImGui::IsItemActivated();
        ImGui::PopID();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##slmenu");
        const ImU32 fill = selected ? (data ? (hov ? IM_COL32(0xBD, 0xD7, 0xEE, 255) : IM_COL32(0xDD, 0xEB, 0xF7, 255)) : IM_COL32(0xEE, 0xF2, 0xF7, 255))
                                    : (hov ? IM_COL32(0xF2, 0xF2, 0xF2, 255) : IM_COL32_WHITE);
        dl->AddRectFilled(br.Min, br.Max, fill, S(2));
        dl->AddRect(br.Min, br.Max, selected && data ? IM_COL32(0x5B, 0x9B, 0xD5, 255) : IM_COL32(0xD0, 0xD0, 0xD0, 255), S(2));
        const ImU32 ink = !data ? IM_COL32(0xB8, 0xB8, 0xB8, 255) : selected ? IM_COL32(0x1F, 0x1F, 0x1F, 255) : IM_COL32(0x90, 0x90, 0x90, 255);
        dl->AddText(ImVec2(br.Min.x + S(6), br.Min.y + S(4)), ink, (i < shown.size() ? shown[i] : items[i]).c_str());
        if (hit && (pt || !sl.table.empty())) {
          // a click selects only that item; Ctrl+click (or Multi-Select) adds or removes it
          std::vector<std::string> nh;
          if (io.KeyCtrl || multi) {
            nh = hidden;
            const auto it = std::find(nh.begin(), nh.end(), items[i]);
            if (it == nh.end()) {
              if (nh.size() + 1 < items.size()) nh.push_back(items[i]);
            } else {
              nh.erase(it);
            }
          } else {
            for (const auto& other : items) {
              if (other != items[i]) nh.push_back(other);
            }
          }
          const uint32_t id = sl.id;
          PivotDocOp(s, TR("Slicer"), [sheet_id, id, nh](Document& d, std::string* err) { return d.SetSlicerSelection(sheet_id, id, nh, err); });
        }
      }
      dl->PopClipRect();
    }
    // resize corner
    const float gs = S(10);
    const ImRect grip(ImVec2(r.Max.x - gs, r.Max.y - gs), r.Max);
    ImGui::SetCursorScreenPos(grip.Min);
    ImGui::InvisibleButton("##grip", grip.GetSize());
    pressed = pressed || ImGui::IsItemActivated();
    if (ImGui::IsItemHovered() || (u.sl_drag == sl.id && u.sl_mode == 2)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    if (ImGui::IsItemActivated()) {
      u.sl_drag = sl.id;
      u.sl_mode = 2;
      u.sl_mouse0 = io.MousePos;
      u.sl_l0 = slp->left, u.sl_t0 = slp->top, u.sl_w0 = slp->width, u.sl_h0 = slp->height;
    }
    dl->AddTriangleFilled(ImVec2(r.Max.x - gs * 0.6f, r.Max.y - 2), ImVec2(r.Max.x - 2, r.Max.y - gs * 0.6f), ImVec2(r.Max.x - 2, r.Max.y - 2), IM_COL32(0xA0, 0xA0, 0xA0, 255));
    // the rest of the slicer (submitted last: its buttons keep the mouse): a press selects it, a right-click opens its menu
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::InvisibleButton("##slbg", ImMax(r.GetSize(), ImVec2(1, 1)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    pressed = pressed || ImGui::IsItemActivated();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##slmenu");
    // selected (a press on it): a frame with handles, as Excel draws a selected slicer
    if (pressed && !(u.sl_sel == sl.id && u.sl_sel_sheet == sheet_id)) {
      ClearObjectSelection();  // a chart or shape selected before lets go
      u.sl_sel = sl.id;
      u.sl_sel_sheet = sheet_id;
    }
    if (u.sl_sel == sl.id && u.sl_sel_sheet == sheet_id) {
      const ImU32 sel_col = IM_COL32(0x21, 0x73, 0x46, 255);
      dl->AddRect(ImVec2(r.Min.x - 2, r.Min.y - 2), ImVec2(r.Max.x + 2, r.Max.y + 2), sel_col, 0.0f, S(1.5f));
      const float hs = S(3.5f);
      for (const ImVec2& c : {r.Min, ImVec2(r.Max.x, r.Min.y), ImVec2(r.Min.x, r.Max.y), r.Max, ImVec2((r.Min.x + r.Max.x) / 2, r.Min.y),
                              ImVec2((r.Min.x + r.Max.x) / 2, r.Max.y), ImVec2(r.Min.x, (r.Min.y + r.Max.y) / 2), ImVec2(r.Max.x, (r.Min.y + r.Max.y) / 2)}) {
        dl->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), IM_COL32_WHITE);
        dl->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), sel_col);
      }
    }
    // end of a drag: save the geometry
    if (u.sl_drag == sl.id && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const uint32_t id = sl.id;
      const double l = sl.left, t = sl.top, w = sl.width, h = sl.height;
      u.sl_drag = 0;
      if (l != slp->left || t != slp->top || w != slp->width || h != slp->height) {
        s.engine->PostUser(TR("Move Slicer"), [sheet_id, id, l, t, w, h](Document& d) {
          d.UpdateSlicer(sheet_id, id, [&](model::Slicer& x) {
            x.left = l;
            x.top = t;
            x.width = w;
            x.height = h;
          });
        });
      }
    }
    ImGui::PopID();
  }
  dl->PopClipRect();
}

bool SlicerClipboardPending() { return SlicerClipPending(); }

bool CopySlicerNamed(AppState& s, uint32_t sheet, const std::string& name, bool cut) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return false;
  for (const auto& sl : *sh->slicers) {
    if (sl.name == name) {
      CopySlicer(s, sl, sheet, cut);
      return true;
    }
  }
  return false;
}

bool SlicerSelected(uint32_t sheet_id) { return U().sl_sel != 0 && U().sl_sel_sheet == sheet_id; }
void ClearSlicerSelection() { U().sl_sel = 0; }

// the placeholder of an empty PivotTable, the filter buttons and the +/- buttons of a report
void DrawPivotOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh || sh->pivots->empty()) return;
  PivotUi& u = U();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  auto cell_rect = [&](uint32_t r, uint32_t c) {
    const ImVec2 a = g.to_screen(static_cast<double>(sh->cols->Offset(c)), static_cast<double>(sh->rows->Offset(r)));
    const ImVec2 b = g.to_screen(static_cast<double>(sh->cols->Offset(static_cast<int64_t>(c) + 1)), static_cast<double>(sh->rows->Offset(static_cast<int64_t>(r) + 1)));
    return ImRect(a, b);
  };
  for (const auto& p : *sh->pivots) {
    if (!p.has_output) {
      const Rect a = engine::PivotArea(p);
      const ImRect r(cell_rect(a.r0, a.c0).Min, cell_rect(a.r1, a.c1).Max);
      dl->AddRectFilled(r.Min, r.Max, IM_COL32(0xF2, 0xF2, 0xF2, 255));
      dl->AddRect(r.Min, r.Max, IM_COL32(0xA6, 0xA6, 0xA6, 255));
      const std::string text = p.name + "\n\n" + TR("To build a report, choose fields from the PivotTable Field List");
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(r.Min.x + S(10), r.Min.y + S(30)), IM_COL32(0x40, 0x40, 0x40, 255), text.c_str(), nullptr,
                  r.GetWidth() - S(20));
      continue;
    }
    // the report (read once per snapshot)
    const Cached& cc = CacheFor(s, p);
    if (!cc.ok) continue;
    const engine::PivotReport& rep = *cc.rep;
    // +/- buttons left of the outer items' labels (submitted before the filter buttons: small handles first)
    if (p.show_buttons) {
      const float unit = static_cast<float>(11.0 * g.sc);
      for (size_t k = 0; k < rep.toggles.size(); ++k) {
        const auto& t = rep.toggles[k];
        const ImRect cr = cell_rect(p.r0 + t.r, p.c0 + t.c);
        if (!cr.Overlaps(g.cells)) continue;
        const uint8_t ind = rep.at(t.r, t.c).indent;
        const float sz = std::floor(std::min(cr.GetHeight() - 6, unit * 0.85f));
        if (sz < 5) continue;
        const float x = cr.Min.x + S(2) + std::max(0.0f, static_cast<float>(ind) - 1.0f) * unit;
        const ImRect br(ImVec2(x, cr.GetCenter().y - sz / 2), ImVec2(x + sz, cr.GetCenter().y + sz / 2));
        ImGui::SetCursorScreenPos(br.Min);
        ImGui::PushID(static_cast<int>(p.id * 4096 + static_cast<uint32_t>(k)));
        const bool hit = ImGui::InvisibleButton("##pm", ImMax(br.GetSize(), ImVec2(1, 1)));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddRectFilled(br.Min, br.Max, hov ? IM_COL32(0xE5, 0xF1, 0xFB, 255) : IM_COL32_WHITE);
        dl->AddRect(br.Min, br.Max, IM_COL32(0x80, 0x80, 0x80, 255));
        const ImVec2 c = br.GetCenter();
        const float a = sz * 0.3f;
        const ImU32 ink = IM_COL32(0x30, 0x30, 0x30, 255);
        dl->AddLine(ImVec2(c.x - a, c.y), ImVec2(c.x + a + 1, c.y), ink, 1.0f);
        if (t.collapsed) dl->AddLine(ImVec2(c.x, c.y - a), ImVec2(c.x, c.y + a + 1), ink, 1.0f);
        if (hit) {
          const std::string item = cc.cache->items[static_cast<size_t>(t.field)][static_cast<size_t>(t.item)];
          const int field = t.field;
          const bool collapse = !t.collapsed;
          EditPivotOf(s, sheet_id, p.id, collapse ? TR("Collapse") : TR("Expand"), [field, item, collapse](PivotTable& x) {
            for (auto* axis : {&x.rows, &x.cols}) {
              for (auto& af : *axis) {
                if (af.field == field) engine::SetPivotItemCollapsed(af, item, collapse);
              }
            }
          });
        }
      }
    }
    if (p.enable_selection && g.cells.Contains(ImGui::GetIO().MousePos) && g.to_sheet) {
      // the mouse at the left edge of a row label (the top edge of a column label): a click selects the item's rows
      // (columns) of the report, as Excel's structured selection does
      const ImVec2 mp = ImGui::GetIO().MousePos;
      const ImVec2 sp = g.to_sheet(mp);
      const int64_t ar = sh->rows->IndexAt(static_cast<int64_t>(sp.y)), ac = sh->cols->IndexAt(static_cast<int64_t>(sp.x));
      // the report's own cells only: the output takes in the filter area above it too, where ar - r0 wrapped round and
      // read past the report (a debug build stopped on it, chaos seed 5806), and a report from another snapshot may
      // be smaller than the output
      if (ar >= 0 && ac >= 0 && p.output.Contains(static_cast<uint32_t>(ar), static_cast<uint32_t>(ac)) && ar >= p.r0 && ac >= p.c0 &&
          static_cast<uint32_t>(ar) - p.r0 < rep.rows && static_cast<uint32_t>(ac) - p.c0 < rep.cols) {
        const uint32_t rr = static_cast<uint32_t>(ar) - p.r0, rc = static_cast<uint32_t>(ac) - p.c0;
        const engine::PivotCell& cell = rep.at(rr, rc);
        const ImRect cr = cell_rect(static_cast<uint32_t>(ar), static_cast<uint32_t>(ac));
        const float edge = S(5);
        const bool row_lbl = cell.kind == engine::PivotCell::RowLabel && rep.row_kind[rr] != 4 && mp.x < cr.Min.x + edge;
        const bool col_lbl = cell.kind == engine::PivotCell::ColLabel && mp.y < cr.Min.y + edge;
        if (row_lbl || col_lbl) {
          const ImRect strip = row_lbl ? ImRect(cr.Min, ImVec2(cr.Min.x + edge, cr.Max.y)) : ImRect(cr.Min, ImVec2(cr.Max.x, cr.Min.y + edge));
          ImGui::SetCursorScreenPos(strip.Min);
          ImGui::PushID(static_cast<int>(p.id) + 0x60000);
          const bool hit = ImGui::InvisibleButton("##pvsel", ImMax(strip.GetSize(), ImVec2(1, 1)));
          ImGui::PopID();
          // a small black arrow at the pointer, as Excel shows it there
          const ImU32 ink = IM_COL32(0, 0, 0, 255);
          if (row_lbl) dl->AddTriangleFilled(ImVec2(mp.x - S(9), mp.y - S(4)), ImVec2(mp.x - S(1), mp.y), ImVec2(mp.x - S(9), mp.y + S(4)), ink);
          else dl->AddTriangleFilled(ImVec2(mp.x - S(4), mp.y - S(9)), ImVec2(mp.x + S(4), mp.y - S(9)), ImVec2(mp.x, mp.y - S(1)), ink);
          if (hit) {
            Rect sel;
            if (row_lbl) {
              const int lv = engine::PivotLabelLevel(rep, rr, rc);
              const auto& rf = rep.row_filter[rr];
              const size_t n = lv >= 0 ? static_cast<size_t>(lv) + 1 : rf.size();
              uint32_t last = rr;
              for (uint32_t q = rr + 1; q < rep.rows; ++q) {
                const auto& f = rep.row_filter[q];
                if (rep.row_kind[q] == 2 || f.size() < n || !std::equal(rf.begin(), rf.begin() + static_cast<std::ptrdiff_t>(n), f.begin())) break;
                last = q;
              }
              sel = Rect{p.r0 + rr, p.output.c0, p.r0 + last, p.output.c1};
            } else {
              const auto& cf = rep.col_filter[rc];
              const size_t n = std::min<size_t>(cf.size(), rr - rep.page_rows);
              uint32_t last = rc;
              for (uint32_t q = rc + 1; q < rep.cols; ++q) {
                const auto& f = rep.col_filter[q];
                if (rep.col_kind[q] == 2 || f.size() < n || !std::equal(cf.begin(), cf.begin() + static_cast<std::ptrdiff_t>(n), f.begin())) break;
                last = q;
              }
              sel = Rect{p.r0 + rr, p.c0 + rc, p.output.r1, p.c0 + last};
            }
            if (sheet_id == s.focus_sheet) render::SelectRange(s.FocusView(), sel);
          }
        }
      }
    }
    if (!p.field_captions) continue;  // Display field captions and filter drop downs: off
    struct Btn {
      uint32_t r, c;
      int area, pos;
    };
    std::vector<Btn> btns;
    for (size_t i = 0; i < p.pages.size(); ++i) btns.push_back(Btn{p.r0 + static_cast<uint32_t>(i), p.c0 + 1, kFilters, static_cast<int>(i)});
    if (!p.rows.empty() && rep.label_cols > 0) {
      // a button on each label column's header (compact: one for the fields sharing the column)
      const uint32_t hr = p.r0 + rep.page_rows + rep.header_rows - 1;
      for (uint32_t c = 0; c < rep.label_cols; ++c) {
        const int lv = engine::PivotColumnField(rep, c);
        if (lv >= 0 && static_cast<size_t>(lv) < p.rows.size()) btns.push_back(Btn{hr, p.c0 + c, kRows, lv});
      }
    }
    if (!p.cols.empty()) btns.push_back(Btn{p.r0 + rep.page_rows, p.c0 + rep.label_cols, kColumns, 0});
    for (const Btn& b : btns) {
      const ImRect cr = cell_rect(b.r, b.c);
      const float sz = std::floor(std::min(cr.GetHeight() - 2, S(16)));
      const ImRect br(ImVec2(cr.Max.x - sz - 2, cr.Max.y - sz - 1), ImVec2(cr.Max.x - 2, cr.Max.y - 1));
      ImGui::SetCursorScreenPos(br.Min);
      ImGui::PushID(static_cast<int>(p.id * 16 + static_cast<uint32_t>(b.area) * 4 + static_cast<uint32_t>(b.pos)));
      ImGui::InvisibleButton("##pf", ImMax(br.GetSize(), ImVec2(1, 1)));
      const bool hov = ImGui::IsItemHovered();
      const auto& axis = *AxisOf(p, b.area);
      // a compact column's one button: filtered when any of its fields is
      const model::PivotAxisField* af = static_cast<size_t>(b.pos) < axis.size() ? &axis[static_cast<size_t>(b.pos)] : nullptr;
      if (b.area == kRows && static_cast<size_t>(b.pos) < rep.row_field_col.size()) {
        for (size_t i = 0; i < axis.size() && i < rep.row_field_col.size(); ++i) {
          if (rep.row_field_col[i] == rep.row_field_col[static_cast<size_t>(b.pos)] && Filtered(axis[i])) af = &axis[i];
        }
      }
      if (hov) {
        std::string tip = FieldName(p, axis[static_cast<size_t>(b.pos)].field);
        if (af && Filtered(*af)) tip += std::string("\n") + TR("Filtered");
        if (af && (af->descending || af->sort_by >= 0)) tip += std::string("\n") + (af->descending ? TR("Sorted Z to A") : TR("Sorted A to Z"));
        ImGui::SetTooltip("%s", tip.c_str());
      }
      if (ImGui::IsItemClicked()) {
        u.sheet = sheet_id;
        u.id = p.id;
        u.filter_area = b.area;
        u.filter_field_pos = b.pos;
        u.filter_items.clear();
        u.filter_shown.clear();
        u.filter_pos = ImVec2(br.Min.x, br.Max.y + 2);
        ImGui::OpenPopup("##pivot_filter");
      }
      DrawFieldButton(dl, br, hov, af);
      // the filter list
      ImGui::SetNextWindowPos(u.filter_pos, ImGuiCond_Appearing);
      ImGui::PushFont(ui::F().ui, ui::F().ui_size);  // a menu in the UI's font, not the zoomed cells'
      if (ImGui::BeginPopup("##pivot_filter")) {
        DrawFilterPopupBody(s, p, b.area);
        ImGui::EndPopup();
      }
      ImGui::PopFont();
      ImGui::PopID();
    }
  }
  dl->PopClipRect();
}

bool PivotCommitText(AppState& s, uint32_t sheet, uint32_t r, uint32_t c, const std::string& text) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh || sh->pivots->empty()) return false;
  const PivotTable* p = nullptr;
  for (const auto& x : *sh->pivots) {
    if (x.has_output && x.output.Contains(r, c)) p = &x;
  }
  if (!p || text.empty() || text[0] == '=') return false;
  const Cached& cc = CacheFor(s, *p);
  if (!cc.ok) return false;
  const CellInfo ci = InfoAt(s, *p, r, c);
  const uint32_t id = p->id;
  auto taken = [&](const std::string& name, int except_data, int except_field) {
    for (size_t i = 0; i < p->data.size(); ++i) {
      if (static_cast<int>(i) != except_data && engine::PivotDataCaption(*p, *cc.cache, i) == name) return true;
    }
    for (size_t f = 0; f < p->fields.size(); ++f) {
      if (static_cast<int>(f) != except_field && p->fields[f] == name) return true;
    }
    return false;
  };
  // a data field's caption: its Custom Name
  if (ci.caption && ci.data >= 0) {
    if (taken(text, ci.data, p->data[static_cast<size_t>(ci.data)].field)) {
      s.ShowMessage("YCGrid", TR("PivotTable field name already exists."));
      return true;
    }
    const int d = ci.data;
    EditPivotOf(s, sheet, id, TR("Rename"), [d, text](PivotTable& x) {
      if (static_cast<size_t>(d) < x.data.size()) x.data[static_cast<size_t>(d)].caption = text;
    });
    return true;
  }
  // a field's header (outline / tabular, the filters): its caption
  if ((ci.kind == engine::PivotCell::Header || ci.kind == engine::PivotCell::PageName) && ci.area != kNone) {
    const auto& cell = cc.rep->at(ci.r, ci.c);
    if (cell.v.IsString() && (std::string(cell.v.str()) == TR("Row Labels") || std::string(cell.v.str()) == TR("Column Labels"))) return false;
    if (taken(text, -1, ci.field)) {
      s.ShowMessage("YCGrid", TR("PivotTable field name already exists."));
      return true;
    }
    const int area = ci.area, pos = ci.pos;
    const std::string src = p->fields[static_cast<size_t>(ci.field)];
    EditPivotOf(s, sheet, id, TR("Rename"), [area, pos, text, src](PivotTable& x) {
      auto* v = AxisOf(x, area);
      if (v && static_cast<size_t>(pos) < v->size()) (*v)[static_cast<size_t>(pos)].caption = text == src ? std::string() : text;
    });
    return true;
  }
  // an item's label: typing another item's name moves that item here; otherwise the item is renamed
  if ((ci.kind == engine::PivotCell::RowLabel || ci.kind == engine::PivotCell::ColLabel) && ci.area != kNone && ci.item >= 0) {
    const std::string me = cc.cache->items[static_cast<size_t>(ci.field)][static_cast<size_t>(ci.item)];
    const auto items = ItemsOf(s, *p, ci.field);
    std::string other;
    for (const auto& [idt, shown] : items) {
      if (idt != me && (shown == text || idt == text)) other = idt;
    }
    const int area = ci.area, pos = ci.pos;
    if (!other.empty()) {
      std::vector<std::string> order = ShownOrder(s, *p, area, ci.field);
      order.erase(std::remove(order.begin(), order.end(), other), order.end());
      const auto at = std::find(order.begin(), order.end(), me);
      order.insert(at, other);
      EditPivotOf(s, sheet, id, TR("Move"), [area, pos, order](PivotTable& x) {
        auto* v = AxisOf(x, area);
        if (!v || static_cast<size_t>(pos) >= v->size()) return;
        auto& f = (*v)[static_cast<size_t>(pos)];
        f.manual_order = order;
        f.sort_by = -1;
        f.descending = false;
      });
      return true;
    }
    EditPivotOf(s, sheet, id, TR("Rename"), [area, pos, me, text](PivotTable& x) {
      auto* v = AxisOf(x, area);
      if (!v || static_cast<size_t>(pos) >= v->size()) return;
      auto& caps = (*v)[static_cast<size_t>(pos)].item_captions;
      caps.erase(std::remove_if(caps.begin(), caps.end(), [&](const auto& e) { return e.first == me; }), caps.end());
      if (text != me) caps.emplace_back(me, text);
    });
    return true;
  }
  return false;
}


bool PivotMoveCells(AppState& s, uint32_t sheet, const Rect& src, uint32_t r, uint32_t c) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return false;
  const PivotTable* p = nullptr;
  for (const auto& x : *sh->pivots) {
    if (x.has_output && x.output.Intersects(src)) p = &x;
  }
  if (!p || !src.IsCell()) return false;
  const CellInfo a = InfoAt(s, *p, src.r0, src.c0), b = InfoAt(s, *p, r, c);
  if (a.area == kNone || a.area == kFilters || a.item < 0 || b.area != a.area || b.field != a.field || b.item < 0 || b.item == a.item) return false;
  const Cached& cc = CacheFor(s, *p);
  if (!cc.ok) return false;
  // the dragged item takes the place of the item it was dropped on (Excel: the field's sort becomes Manual)
  const std::string me = cc.cache->items[static_cast<size_t>(a.field)][static_cast<size_t>(a.item)];
  const std::string there = cc.cache->items[static_cast<size_t>(b.field)][static_cast<size_t>(b.item)];
  std::vector<std::string> order = ShownOrder(s, *p, a.area, a.field);
  const auto from = std::find(order.begin(), order.end(), me), to = std::find(order.begin(), order.end(), there);
  if (from == order.end() || to == order.end()) return false;
  const bool down = from < to;
  order.erase(from);
  auto at = std::find(order.begin(), order.end(), there);
  if (down) ++at;
  order.insert(at, me);
  const int area = a.area, pos = a.pos;
  U().sheet = sheet;
  U().id = p->id;
  EditPivotOf(s, sheet, p->id, TR("Move"), [area, pos, order](PivotTable& x) {
    auto* v = AxisOf(x, area);
    if (!v || static_cast<size_t>(pos) >= v->size()) return;
    auto& f = (*v)[static_cast<size_t>(pos)];
    f.manual_order = order;
    f.sort_by = -1;
    f.descending = false;
  });
  return true;
}

bool PivotOutline(AppState& s, bool group) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || sh->pivots->empty()) return false;
  const auto& sel = s.FocusView().sel;
  for (const auto& p : *sh->pivots) {
    if (!p.has_output || !p.output.Contains(sel.act_r, sel.act_c)) continue;
    U().sheet = s.focus_sheet;
    U().id = p.id;
    ui::Commands().Execute(group ? "PivotGroupSelection" : "PivotUngroup");
    return true;
  }
  return false;
}

}  // namespace yc::app
