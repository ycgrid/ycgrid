// SPDX-License-Identifier: Apache-2.0
// objects_ui.cpp - objects floating over the sheet and custom panels (PLAN §9, §10.3, §12.9)
//
// - Controls (CommandButton, ComboBox): in run mode clicks send events to Python; in Design Mode they can be selected, moved, resized and deleted.
// - Charts (ImPlot): can be selected, moved and resized in any mode; ImPlot pan/zoom is off by default (only with Interactive).
// - Object position and size are stored in points (pt) in the model; sheet pixels = pt x 4/3 (at 100% zoom).
// - Dragging only updates a preview; releasing sends one undoable engine command.
#include "app_state.h"
#include "trust.h"
#include "engine/chart_layout.h"
#include "engine/navigate.h"
#include "engine/shape_geometry.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"
#include "core/i18n.h"

#include "plot_theme.h"
#include "python_bridge.h"
#include "screenshot.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "dock/dock_layout.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "theme/theme.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "implot.h"
#include "ribbon/ribbon.h"
#include "implot_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace yc::app {

using engine::Document;
using model::ChartObj;
using model::ChartType;
using model::Control;
using model::ControlKind;
using model::Placement;
using model::ShapeKind;
using model::ShapeObj;
using ui::S;

namespace {

constexpr double kPx = 4.0 / 3.0;  // points -> sheet pixels (100%)
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();


// Excel RGB value (0xBBGGRR) -> ImU32
ImU32 ExcelColor(uint32_t c) { return (c & 0x00FFFFFFu) | 0xFF000000u; }

struct ObjSel {
  uint32_t sheet = 0;
  int kind = 0;  // 0 none, 1 control, 2 chart, 3 shape (picture, shape, text box)
  uint32_t id = 0;
  bool operator==(const ObjSel& o) const { return sheet == o.sheet && kind == o.kind && id == o.id; }
  explicit operator bool() const { return kind != 0; }
};

struct Geom {
  double l = 0, t = 0, w = 0, h = 0;  // points
};

using SeriesBuf = engine::ChartSeriesValues;

struct ChartCache {
  uint64_t version = 0;
  size_t def_hash = 0;
  double built_at = -1.0;
  engine::ChartValues v;
  std::vector<double> bars;     // column / bar series: [bar series x points]
  std::vector<size_t> bar_idx;  // which series are columns / bars
  // the drawn scene of radar, waterfall, histogram, treemap... (rebuilt with the data or the size)
  engine::ChartScene scene;
  bool scene_ok = false;
  float scene_w = 0, scene_h = 0, scene_fs = 0;
  // where the elements were drawn last (screen): clicking selects the element under the mouse
  struct Hit {
    ImRect frame, title, legend, plot, plot_frame, cat_axis, val_axis, val_axis2, cat_title, val_title, val2_title;
    std::vector<std::vector<ImVec2>> lines;  // per series: line or marker points
    std::vector<std::vector<int>> line_idx;  // the point each entry of lines is
    std::vector<std::vector<ImRect>> boxes;  // per series: columns, bars (one per point)
    std::vector<std::vector<ImRect>> labels;  // per series: its data labels
    std::vector<std::vector<int>> label_idx;  // the point each entry of labels is
    std::vector<std::vector<std::string>> label_texts;  // ... and its text as drawn
    std::vector<ImRect> legend_rects;         // each legend entry
    std::vector<int> legend_series;           // its series (-1: a pie's category or a trendline)
    std::vector<int> legend_cats;             // a category's entry: its category
    std::vector<std::vector<ImVec2>> trend;   // per series: its trendline
    std::vector<std::vector<ImRect>> errs;    // per series: its error bars
    std::vector<float> grid;                  // the value axis' major gridlines (screen y; x for bars)
    std::vector<float> minor_grid;            // ... its minor gridlines
    bool grid_vertical = false;
    // pie and doughnut wedges (radians clockwise from 12 o'clock)
    struct Slice {
      int series = 0, point = 0;
      ImVec2 c;
      float r0 = 0, r1 = 0, a0 = 0, a1 = 0;
    };
    std::vector<Slice> slices;
    // the value axes' bounds and major unit as drawn (Format Axis shows them while they are automatic): 0 the value
    // axis, 1 the secondary one, 2 a scatter chart's X axis
    double axis_lo[3] = {0, 0, 0}, axis_hi[3] = {1, 1, 1}, axis_step[3] = {0.2, 0.2, 0.2};
    ImVec2 pie_c;
    float pie_r = 0, pie_r0 = 0;
    bool scene = false;
    std::vector<ImRect> text_boxes;  // scene charts: each primitive's text box (screen), empty for other primitives
    std::vector<ImRect> up_bars, down_bars;   // stock charts: the candles (Up Bars, Down Bars)
    std::vector<ImVec2> hilo;                 // ... the high-low lines, two ends each
    std::vector<std::vector<int>> trend_idx;  // per series: which trendline each point of trend is on (0 the first)
    std::vector<std::vector<ImRect>> xerrs;   // per series: its X error bars (scatter charts)
  } hit;
};

struct UiState {
  ObjSel sel;
  model::ChartElem elem = model::ChartElem::ChartArea;  // the selected chart's selected element
  int elem_series = 0;
  int elem_point = -1;  // a data point (ChartElem::Point)
  // dragging the selected title, legend or plot area inside its chart
  bool elem_drag = false;
  ObjSel elem_drag_obj;
  model::ChartElem elem_drag_e = model::ChartElem::None;
  ImVec2 elem_drag_m0;
  ImRect elem_drag_r0, elem_drag_frame;
  int elem_drag_handle = -1;  // the legend's or the plot area's handle being dragged (resizing it), -1 moving
  int text_elem = 0;  // the chart text typed in place: 0 the title, 1 the category axis', 2 the value axis', 3 the secondary axis' title, 4 one data label
  int text_series = 0, text_point = -1;  // ... that label's series and point
  bool label_click_again = false;  // the press was on the one label already selected: released in place, it is typed into
  ObjSel elem_of;  // the chart elem belongs to (another selection means its chart area)
  // dragging
  int drag = 0;  // 0 none, 1 move, 2..9 handles (top-left, top, top-right, right, bottom-right, bottom, bottom-left, left), 10 drawing a new control
  ObjSel drag_obj;
  ImVec2 drag_mouse0;
  double drag_sc = 1.0;
  Geom g0, preview;
  bool preview_on = false;
  uint64_t preview_until = 0;  // stop the preview once the snapshot version exceeds this (the command has been applied)
  ObjSel preview_obj;
  int insert_kind = -1;  // Design Mode: control created by the next drag (a ControlKind)
  bool insert_form = false;  // ... as a form control
  // Assign Macro
  ObjSel assign;
  bool assign_open = false;
  char assign_buf[256] = {};
  bool show_props = false;
  // combo box
  ObjSel combo;
  char combo_buf[512] = {};
  bool combo_focus = false;
  ImVec2 combo_pos;
  std::map<uint32_t, ChartCache> charts;
  std::set<std::string> panels_docked;
  std::set<std::string> panels_known;  // the panels of the workbook last frame (a deleted one gives up its dock place)
  std::map<std::string, std::string> panel_text;  // text being edited in a panel text box
  // a panel slider's live value: kept while dragging and until the engine publishes the new value
  struct SliderLive {
    float v = 0;
    std::string sent;  // value posted on release ("" while still dragging)
    double sent_at = 0;
  };
  std::map<std::string, SliderLive> panel_slider;
  ObjSel ctx;  // target of the context menu
  // pictures: textures by shape id (rebuilt when the image changes)
  struct Tex {
    size_t hash = 0;
    ImTextureID tex{};
  };
  std::map<uint32_t, Tex> textures;
  // controls: text box being edited, list box scroll positions, scroll bar thumb drag
  ObjSel tb_obj;
  char tb_buf[8192] = {};
  bool tb_focus = false;
  std::map<uint32_t, int> list_top;
  ObjSel scroll_drag;
  float scroll_m0 = 0;
  double scroll_v0 = 0;
  // editing a shape's text (or a chart's title: text_obj is the chart)
  ObjSel text_obj;
  char text_buf[8192] = {};
  bool text_focus = false;
  ImVec2 text_pos, text_size;
  std::string text_keep;  // what the box held before this frame's Escape (ImGui's box puts the old text back on Escape)
  // more selected objects (Shift / Ctrl+click, a group): moved, nudged, deleted, copied and aligned with sel
  std::vector<ObjSel> more;
  bool more_moving = false;  // the move preview applies to them too
  // rotating the selected shape with its rotation handle
  bool rotating = false;
  // a line's end (drag 11 / 12) or the rotation being dragged: the shape is drawn with these
  bool preview_shape_on = false, preview_flip_h = false, preview_flip_v = false;
  double preview_rot = 0;
  ImVec2 rot_center;
  // where the selected shape was drawn last (typing on it starts editing its text there)
  ImRect sel_screen;
  float sel_scale = 1.0f;
  bool text_cursor_end = false;  // the text editor opens with the caret at the end
  float text_scale = 1.0f;       // screen pixels per sheet pixel where the text is edited
  bool drag_copy = false;        // Ctrl was down when the move started: the drop makes a copy
  // where the selected object's handles were drawn last frame (they take the mouse before the objects under them)
  std::vector<ImVec2> handle_pts;
  uint32_t handle_sheet = 0;
};

UiState& U() {
  static UiState u;
  return u;
}

bool OverHandle(uint32_t sheet, ImVec2 m) {
  const UiState& u = U();
  if (u.handle_sheet != sheet || !u.sel) return false;
  for (const ImVec2& p : u.handle_pts) {
    if (std::fabs(p.x - m.x) <= S(6) && std::fabs(p.y - m.y) <= S(6)) return true;
  }
  return false;
}

bool InMore(const ObjSel& o) { return std::find(U().more.begin(), U().more.end(), o) != U().more.end(); }
bool IsSelected(const ObjSel& o) { return U().sel == o || InMore(o); }

// the object clipboard: copies of the objects (the whole model object, charts included), their sheet, and the
// clipboard sequence number when they were put there (a later copy of cells or text replaces them)
struct ObjClip {
  uint32_t sheet = 0;
  std::vector<ShapeObj> shapes;
  std::vector<ChartObj> charts;
  std::vector<Control> controls;
  uint32_t seq = 0;
  int pastes = 0;
  bool valid = false;
  std::string png;  // a copied chart's picture (Paste as Picture, Paste Special > Picture (PNG)), taken after the copy
};
// the selection's handles are left out while a copied chart's picture is taken
int g_marks_hidden_until = -1;
ObjClip& Clip() {
  static ObjClip c;
  return c;
}

void StartShapeText(ObjSel me, const ShapeObj& sp, const ImRect& r, float scale, const std::string& typed);
void OpenFormatControl(AppState& s, ObjSel o);
}  // namespace
void StartCaptionEdit(ObjSel o, const std::string& caption);
namespace {
bool SelectOnClick(const model::Sheet& sh, ObjSel me);
void AddGroupOf(const model::Sheet& sh, ObjSel o);
void MoveMore(AppState& s, double dx, double dy);
void DuplicateSelection(AppState& s, double dx, double dy, const char* label);

// names of the chart types in ChartType order
const char* const* ChartTypeNames() {
  static const char* k[] = {TR("Line"),           TR("Line with Markers"), TR("Clustered Column"), TR("Stacked Column"), TR("Clustered Bar"),
                            TR("XY Scatter"),     TR("Area"),              TR("Pie"),              TR("Doughnut"),       TR("Stock (Open-High-Low-Close)"),
                            TR("Radar"),          TR("Radar with Markers"), TR("Filled Radar"),    TR("Waterfall"),      TR("Histogram"),
                            TR("Pareto"),         TR("Box and Whisker"),   TR("Treemap"),          TR("Sunburst"),       TR("Funnel"),
                            TR("3-D Column"),     TR("3-D Clustered Column"), TR("3-D Stacked Column"), TR("3-D Clustered Bar"), TR("3-D Line"),
                            TR("3-D Area"),       TR("3-D Pie"),           TR("3-D Surface"),      TR("Wireframe 3-D Surface"), TR("Contour"),
                            TR("Filled Map"),     TR("100% Stacked Column"), TR("Stacked Bar"),   TR("100% Stacked Bar"), TR("Stacked Line"),
                            TR("100% Stacked Line"), TR("Stacked Line with Markers"), TR("100% Stacked Line with Markers"), TR("Stacked Area"),
                            TR("100% Stacked Area"), TR("Scatter with Straight Lines and Markers"), TR("Scatter with Straight Lines"),
                            TR("Scatter with Smooth Lines and Markers"), TR("Scatter with Smooth Lines"), TR("Bubble"), TR("Stock (High-Low-Close)"),
                            TR("Pie of Pie"), TR("Bar of Pie"), TR("Stock (Volume-High-Low-Close)"), TR("Stock (Volume-Open-High-Low-Close)")};
  static_assert(std::size(k) == model::kChartTypeCount);
  return k;
}

// popups with fixed IDs (opening and drawing can happen under different ID stacks)
const ImGuiID kComboPopup = ImHashStr("##yc_combo_popup");
const ImGuiID kObjCtxPopup = ImHashStr("##yc_objctx_popup");
constexpr ImGuiWindowFlags kPopupFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings;

std::string Lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

}  // namespace

// ---- Control values ----

// the form option buttons that work together with c: those in the same group box (the smallest one around their center),
// or all the ones outside group boxes; in creation order
std::vector<const Control*> FormOptionGroup(const model::Sheet& sh, const Control& c) {
  auto box_of = [&](const Control& o) -> const Control* {
    const double cx = o.left + o.width / 2, cy = o.top + o.height / 2;
    const Control* best = nullptr;
    for (const auto& g : *sh.controls) {
      if (g.kind != ControlKind::GroupBox || cx < g.left || cx > g.left + g.width || cy < g.top || cy > g.top + g.height) continue;
      if (!best || g.width * g.height < best->width * best->height) best = &g;
    }
    return best;
  };
  const Control* mine = box_of(c);
  std::vector<const Control*> out;
  for (const auto& o : *sh.controls) {
    if (o.form && o.kind == ControlKind::OptionButton && box_of(o) == mine) out.push_back(&o);
  }
  std::sort(out.begin(), out.end(), [](const Control* a, const Control* b) { return a->id < b->id; });
  return out;
}

void ApplyControlValue(Document& d, uint32_t sheet, uint32_t id, const std::string& value, int list_index) {
  const model::Sheet* sh = d.S(sheet);
  if (!sh) return;
  const Control* c = nullptr;
  for (const auto& x : *sh->controls) {
    if (x.id == id) c = &x;
  }
  if (!c) return;
  const ControlKind kind = c->kind;
  const bool boolish = kind == ControlKind::CheckBox || kind == ControlKind::OptionButton || kind == ControlKind::ToggleButton;
  std::string v = value;
  bool on = false;
  if (boolish) {
    std::string l;
    for (char ch : value) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    on = l == "true" || l == "1" || l == "-1";
    v = on ? "True" : "False";
  }
  const std::string linked = c->linked_cell, group = c->group_name;
  if (c->form && (kind == ControlKind::OptionButton || kind == ControlKind::ComboBox || kind == ControlKind::ListBox)) {
    // form controls: the linked cell gets the 1-based number of the choice
    auto write_num = [&](const std::string& cell, int n) {
      uint32_t sid = 0;
      Rect r;
      if (!cell.empty() && ResolveRangeText(d.wb(), sheet, cell, &sid, &r)) d.SetInput(sid, r.r0, r.c0, n > 0 ? std::to_string(n) : std::string());
    };
    if (kind == ControlKind::OptionButton) {
      if (!on) return;
      const auto grp = FormOptionGroup(*sh, *c);
      std::string cell;
      int n = 0;
      std::vector<uint32_t> ids;
      for (size_t i = 0; i < grp.size(); ++i) {
        ids.push_back(grp[i]->id);
        if (grp[i]->id == id) n = static_cast<int>(i) + 1;
        if (cell.empty()) cell = grp[i]->linked_cell;  // the group shares one linked cell
      }
      for (uint32_t oid : ids) d.UpdateControl(sheet, oid, [&](Control& x) { x.value = x.id == id ? "True" : "False"; });
      write_num(cell, n);
      return;
    }
    d.UpdateControl(sheet, id, [&](Control& x) {
      x.value = v;
      if (list_index >= 0) x.list_index = list_index;
    });
    write_num(linked, list_index >= 0 ? list_index + 1 : 0);
    return;
  }
  d.UpdateControl(sheet, id, [&](Control& x) {
    x.value = v;
    if (list_index >= -1 && kind != ControlKind::ListBox) x.list_index = list_index;
    if (kind == ControlKind::ListBox && list_index >= 0) x.list_index = list_index;
  });
  auto write = [&](const std::string& cell, const std::string& text) {
    uint32_t sid = 0;
    Rect r;
    if (!cell.empty() && ResolveRangeText(d.wb(), sheet, cell, &sid, &r)) d.SetInput(sid, r.r0, r.c0, text);
  };
  write(linked, boolish ? (on ? "TRUE" : "FALSE") : v);
  if (kind == ControlKind::OptionButton && on) {
    std::vector<std::pair<uint32_t, std::string>> others;
    for (const auto& x : *d.S(sheet)->controls) {
      if (x.id != id && x.kind == ControlKind::OptionButton && x.group_name == group && model::ControlIsOn(x)) others.emplace_back(x.id, x.linked_cell);
    }
    for (const auto& [oid, cell] : others) {
      d.UpdateControl(sheet, oid, [](Control& x) { x.value = "False"; });
      write(cell, "FALSE");
    }
  }
}

// ---- Range parsing ("Sheet!A1:B2", defined names) ----

bool ResolveRangeText(const model::Workbook& wb, uint32_t default_sheet, std::string_view text, uint32_t* sheet, Rect* rect, int depth) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '=')) text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  if (text.empty() || depth > 4) return false;
  uint32_t sid = default_sheet;
  const size_t bang = text.rfind('!');
  if (bang != std::string_view::npos) {
    std::string name(text.substr(0, bang));
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
      std::string un;
      for (size_t i = 1; i + 1 < name.size(); ++i) {
        un.push_back(name[i]);
        if (name[i] == '\'' && i + 2 < name.size() && name[i + 1] == '\'') ++i;
      }
      name = un;
    }
    const int32_t idx = wb.FindSheet(name);
    if (idx < 0) return false;
    sid = wb.sheets[static_cast<size_t>(idx)]->id;
    text = text.substr(bang + 1);
  }
  Rect r;
  if (!core::ParseRectA1(text, &r)) {
    if (bang != std::string_view::npos) return false;
    const std::string key = Lower(std::string(text));
    for (const auto& n : *wb.names) {
      if (Lower(n.name) == key && (n.sheet_scope < 0 || static_cast<uint32_t>(n.sheet_scope) == default_sheet)) {
        return ResolveRangeText(wb, default_sheet, n.refers_to, sheet, rect, depth + 1);
      }
    }
    return false;
  }
  const model::Sheet* s = wb.SheetById(sid);
  if (!s) return false;
  // entire column / row: limit to the used range
  if (r.r1 >= s->row_limit - 1 || r.r1 >= core::kMaxRows - 1) r.r1 = s->has_used ? std::max(r.r0, std::min(r.r1, s->used.r1)) : r.r0;
  if (r.c1 >= core::kMaxCols - 1) r.c1 = s->has_used ? std::max(r.c0, std::min(r.c1, s->used.c1)) : r.c0;
  *sheet = sid;
  *rect = r;
  return true;
}

namespace {

std::string CellDisplay(const engine::Snapshot& snap, const model::Sheet& s, uint32_t r, uint32_t c) {
  const core::Value v = s.GetValue(r, c);
  if (v.IsEmpty()) return {};
  const model::Style& st = snap.wb.styles->Get(s.GetStyle(r, c));
  return render::FormatCellText(snap, st, v, -1, nullptr, nullptr, nullptr);
}

// values of a range flattened in order (one column downwards, one row to the right, otherwise row-major); visible_only:
// hidden (and filtered) rows and columns are left out, as a chart leaves them out by default
template <class F>
void ForEachInRange(const model::Sheet& s, const Rect& r, size_t limit, F&& f, bool visible_only = false) {
  size_t n = 0;
  auto row_on = [&](uint32_t row) { return !visible_only || !s.rows->IsHidden(row); };
  auto col_on = [&](uint32_t col) { return !visible_only || !s.cols->IsHidden(col); };
  if (r.c0 == r.c1 || r.r0 != r.r1) {
    for (uint32_t row = r.r0; row <= r.r1 && n < limit; ++row) {
      if (!row_on(row)) continue;
      for (uint32_t col = r.c0; col <= r.c1 && n < limit; ++col) {
        if (!col_on(col)) continue;
        f(row, col);
        ++n;
      }
    }
  } else {
    for (uint32_t col = r.c0; col <= r.c1 && n < limit; ++col) {
      if (!col_on(col) || !row_on(r.r0)) continue;
      f(r.r0, col);
      ++n;
    }
  }
}

std::vector<std::string> ControlItems(const AppState& s, uint32_t sheet, const Control& c) {
  if (c.list_fill_range.empty()) return c.items;
  std::vector<std::string> out;
  uint32_t sid = 0;
  Rect r;
  if (!ResolveRangeText(s.snap->wb, sheet, c.list_fill_range, &sid, &r)) return out;
  const model::Sheet* sh = s.snap->Sheet(sid);
  if (!sh) return out;
  ForEachInRange(*sh, r, 10000, [&](uint32_t row, uint32_t col) { out.push_back(CellDisplay(*s.snap, *sh, row, col)); });
  return out;
}

std::string LinkedText(const AppState& s, uint32_t sheet, const std::string& linked, bool* ok) {
  uint32_t sid = 0;
  Rect r;
  *ok = false;
  if (linked.empty() || !ResolveRangeText(s.snap->wb, sheet, linked, &sid, &r)) return {};
  const model::Sheet* sh = s.snap->Sheet(sid);
  if (!sh) return {};
  *ok = true;
  return CellDisplay(*s.snap, *sh, r.r0, r.c0);
}

// write the linked cell on the engine thread. A control linked to a parameter cell (yc.safety.parameter) is the user
// choosing a value on purpose: it changes the parameter when the value passes its checks (type, range), like typing
// (the change log records it); a refused value leaves the cell as it was
void WriteLinked(Document& d, uint32_t sheet, const std::string& linked, const std::string& value) {
  uint32_t sid = 0;
  Rect r;
  if (linked.empty() || !ResolveRangeText(d.wb(), sheet, linked, &sid, &r)) return;
  const model::ParameterDef* pd = nullptr;
  for (const auto& x : *d.wb().parameters) {
    if (x.sheet == sid && x.row == r.r0 && x.col == r.c0) pd = &x;
  }
  if (pd) {
    std::string why;
    if (!Document::CheckParameterValue(*pd, formula::ParseInput(value).value, &why)) return;
  }
  const bool saved = d.param_commit;
  d.param_commit = d.param_commit || pd != nullptr;
  d.SetInput(sid, r.r0, r.c0, value);
  d.param_commit = saved;
}

bool HostReady(const AppState& s) { return s.python && s.python->HostReady(); }

// ---- Chart data ----

// a smoothed line through the points (Excel's Smoothed line): Catmull-Rom segments, 8 steps between two points; a
// point without a value breaks the line
void SmoothCurve(const double* x, const double* y, int n, std::vector<double>* ox, std::vector<double>* oy) {
  ox->clear();
  oy->clear();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (int i = 0; i + 1 < n; ++i) {
    if (!std::isfinite(y[i]) || !std::isfinite(y[i + 1])) {
      ox->push_back(x[i]);
      oy->push_back(nan);
      continue;
    }
    const int i0 = i > 0 && std::isfinite(y[i - 1]) ? i - 1 : i, i3 = i + 2 < n && std::isfinite(y[i + 2]) ? i + 2 : i + 1;
    for (int k = 0; k < 8; ++k) {
      const double t = k / 8.0, t2 = t * t, t3 = t2 * t;
      auto cr = [&](const double* p) {
        return 0.5 * ((2 * p[i]) + (-p[i0] + p[i + 1]) * t + (2 * p[i0] - 5 * p[i] + 4 * p[i + 1] - p[i3]) * t2 + (-p[i0] + 3 * p[i] - 3 * p[i + 1] + p[i3]) * t3);
      };
      ox->push_back(cr(x));
      oy->push_back(cr(y));
    }
  }
  ox->push_back(x[n - 1]);
  oy->push_back(y[n - 1]);
}

size_t ChartDefHash(const ChartObj& ch) {
  std::hash<std::string> h;
  std::hash<double> hd;
  size_t v = static_cast<size_t>(ch.type) * 1315423911u;
  v = v * 31 + hd(ch.bin_width) + static_cast<size_t>(ch.bin_count) * 7 + (ch.has_title ? 11 : 0) + h(ch.title) + (ch.has_legend ? 5 : 0);
  // element formatting and the 3-D view (scene charts are rebuilt when they change)
  auto font = [&](const model::ChartFont& f) { return hd(f.size) * 3 + static_cast<size_t>(f.color) * 5 + (f.bold ? 7 : 0); };
  auto axis = [&](const model::ChartAxisFormat& a) {
    size_t x = (a.visible ? 11u : 0u) + (a.has_title ? 13u : 0u) + h(a.title) * 3 + h(a.number_format) * 5 + (a.gridlines ? 17u : 0u);
    x = x * 31 + (a.min_auto ? 1u : 0u) + (a.max_auto ? 2u : 0u) + hd(a.min) * 3 + hd(a.max) * 5 + hd(a.major) * 7 + hd(a.minor) * 11 + (a.log ? 13u : 0u) +
        hd(a.log_base) * 17 + (a.reverse ? 19u : 0u) + static_cast<size_t>(a.crosses + 3) * 23 + hd(a.crosses_at) * 29 + hd(a.display_unit) * 31 +
        static_cast<size_t>(a.major_tick + 3) * 37 + static_cast<size_t>(a.label_pos + 3) * 41 + (a.minor_gridlines ? 43u : 0u) + (a.between ? 47u : 0u) +
        static_cast<size_t>(a.label_interval) * 53 + static_cast<size_t>(a.mark_interval) * 59 + (a.unit_label ? 61u : 0u);
    return x;
  };
  v = v * 31 + font(ch.title_font) + font(ch.legend_font) * 3 + font(ch.axis_font) * 5 + static_cast<size_t>(ch.legend_pos) * 19 + static_cast<size_t>(ch.plot_fill) * 23;
  v = v * 31 + axis(ch.cat_axis) + axis(ch.val_axis) * 7 + axis(ch.val_axis2) * 3 + (ch.view3d ? 29 : 0) + hd(ch.rotation) + hd(ch.elevation) * 3 + hd(ch.perspective) * 5 +
      hd(ch.depth_percent) * 7;
  v = v * 31 + static_cast<size_t>(ch.color_scheme) * 37 + (ch.plot_visible_only ? 1 : 0) + static_cast<size_t>(ch.display_blanks) * 3 + (ch.na_as_blank ? 5 : 0) + (ch.title_overlay ? 7 : 0) + (ch.connectors ? 11 : 0) +
      (ch.bins_overflow ? 13 : 0) + (ch.bins_underflow ? 17 : 0) + hd(ch.overflow_at) * 19 + hd(ch.underflow_at) * 23 + (ch.bins_by_category ? 29 : 0);
  for (const auto& se : ch.series) {
    v = v * 31 + h(se.name) + 7 * h(se.values) + 13 * h(se.x_values) + static_cast<size_t>(se.color) + static_cast<size_t>(se.axis_group) * 3;
    v = v * 31 + static_cast<size_t>(se.type + 1) * 17 + (se.data_labels ? 19 : 0) + static_cast<size_t>(se.trend) * 23 + static_cast<size_t>(se.trend_order) * 29 +
        static_cast<size_t>(se.trend_period) * 37 + hd(se.trend_forward) + hd(se.trend_backward) * 3 + (se.trend_eq ? 41 : 0) + (se.trend_r2 ? 43 : 0) +
        static_cast<size_t>(se.err) * 47 + hd(se.err_amount) * 5 + h(se.err_plus) * 3 + h(se.err_minus) * 7 + static_cast<size_t>(se.err_include) * 53;
    v = v * 31 + h(se.label_range) * 3 + h(se.bubble_sizes) * 5 + static_cast<size_t>(se.label_show) * 7;
    for (int t : se.totals) v = v * 7 + static_cast<size_t>(t);
    for (const auto& p : se.points) v = v * 11 + static_cast<size_t>(p.index) * 3 + static_cast<size_t>(p.fill) * 5 + static_cast<size_t>(p.label + 2) + h(p.label_text) * 7;
    v = v * 3 + (se.filtered ? 1 : 0);
    v ^= reinterpret_cast<size_t>(se.data.get());
  }
  for (int k : ch.hidden_cats) v = v * 13 + static_cast<size_t>(k) + 1;
  v = v * 31 + static_cast<size_t>(ch.gap_width + 1000) * 3 + static_cast<size_t>(ch.overlap + 1000) * 5 + static_cast<size_t>(ch.first_slice_angle + 1000) * 7 +
      static_cast<size_t>(ch.hole_size) * 11;
  for (const auto& se : ch.series) v = v * 7 + (se.smooth ? 1 : 0) + static_cast<size_t>(se.box_flags) * 3;
  // the chart types' own options: pie of pie, stock elements, floor and walls, the date axis; the series' trendlines,
  // X error bars, treemap / box and whisker / map options
  v = v * 31 + static_cast<size_t>(ch.split_type) * 3 + hd(ch.split_value) * 5 + static_cast<size_t>(ch.second_size) * 7 + static_cast<size_t>(ch.pie_gap) * 11 +
      (ch.up_down_bars ? 13 : 0) + (ch.hi_lo_lines ? 17 : 0) + static_cast<size_t>(ch.up_fill) * 19 + static_cast<size_t>(ch.down_fill) * 23 +
      static_cast<size_t>(ch.hilo_line) * 29 + static_cast<size_t>(ch.updown_gap) * 31 + static_cast<size_t>(ch.floor_fill) * 37 + static_cast<size_t>(ch.wall_fill) * 41 +
      static_cast<size_t>(ch.cat_type + 1) * 43 + static_cast<size_t>(ch.base_unit + 2) * 47 + static_cast<size_t>(ch.major_unit_scale + 2) * 53;
  for (int k : ch.second_points) v = v * 5 + static_cast<size_t>(k) + 1;
  for (const auto& se : ch.series) {
    v = v * 31 + static_cast<size_t>(se.parent_labels) * 3 + (se.inclusive_median ? 5 : 0) + static_cast<size_t>(se.geo_projection) * 7 + static_cast<size_t>(se.geo_area) * 11 +
        static_cast<size_t>(se.geo_labels) * 13 + static_cast<size_t>(se.geo_colors) * 17 + static_cast<size_t>(se.geo_min) * 19 + static_cast<size_t>(se.geo_mid) * 23 +
        static_cast<size_t>(se.geo_max) * 29 + (se.err_cap ? 31 : 0) + static_cast<size_t>(se.xerr) * 37 + hd(se.xerr_amount) + h(se.xerr_plus) * 3 + h(se.xerr_minus) * 5 +
        static_cast<size_t>(se.xerr_include) * 41 + h(se.trend_name) * 7 + (se.trend_icept_on ? 43 : 0) + hd(se.trend_icept) * 11 + se.more_trends.size() * 47;
    for (const auto& t : se.more_trends) v = v * 13 + static_cast<size_t>(t.kind) + static_cast<size_t>(t.order) * 3 + static_cast<size_t>(t.period) * 5 + hd(t.forward) + h(t.name);
  }
  return v;
}

}  // namespace

// a title as the chart shows it: "=Sheet1!$B$1" is that cell's text (Excel's linked title); placeholder: what an empty
// title shows ("Chart Title", "Axis Title")
std::string ResolveChartText(const engine::Snapshot& snap, uint32_t sheet, const std::string& text, const char* placeholder) {
  if (text.empty()) return placeholder ? placeholder : std::string();
  if (text[0] != '=') return text;
  uint32_t sid;
  Rect r;
  if (ResolveRangeText(snap.wb, sheet, text, &sid, &r)) {
    if (const model::Sheet* sh = snap.Sheet(sid)) return CellDisplay(snap, *sh, r.r0, r.c0);
  }
  return text;
}

// series values from the cells (or Python data) of a chart; shared by the screen and the printed page
void ResolveChartValues(const engine::Snapshot& snap, uint32_t sheet, const ChartObj& ch, engine::ChartValues* out) {
  const auto& wb = snap.wb;
  out->series.clear();
  out->cats.clear();
  out->paths.clear();
  out->cat_vals.clear();
  out->cat_dates = false;
  out->cat_fmt.clear();
  out->points = 0;
  // the titles as drawn (linked to a cell, or the placeholder of a title that has no text yet)
  out->title = ch.has_title ? ResolveChartText(snap, sheet, ch.title, TR("Chart Title")) : std::string();
  out->cat_title = ch.cat_axis.has_title ? ResolveChartText(snap, sheet, ch.cat_axis.title, TR("Axis Title")) : std::string();
  out->val_title = ch.val_axis.has_title ? ResolveChartText(snap, sheet, ch.val_axis.title, TR("Axis Title")) : std::string();
  out->val2_title = ch.val_axis2.has_title ? ResolveChartText(snap, sheet, ch.val_axis2.title, TR("Axis Title")) : std::string();
  out->point_labels.clear();
  for (size_t i = 0; i < ch.series.size(); ++i) {
    for (const auto& p : ch.series[i].points) {
      if (!p.label_text.empty() && p.index >= 0) out->point_labels[(static_cast<uint64_t>(i) << 32) | static_cast<uint32_t>(p.index)] = ResolveChartText(snap, sheet, p.label_text, "");
    }
  }
  const bool vis = ch.plot_visible_only;  // Show data in hidden rows and columns: off
  bool cats_done = false;
  // the numbers of a range (bubble sizes): text and empty cells as nothing
  auto numbers = [&](const std::string& text, std::vector<double>* v) {
    uint32_t nsid;
    std::vector<Rect> nareas;
    if (text.empty() || !ResolveAreasText(wb, sheet, text, &nsid, &nareas)) return;
    if (const model::Sheet* sh = snap.Sheet(nsid)) {
      for (const Rect& ar : nareas) {
        ForEachInRange(*sh, ar, 5000000, [&](uint32_t row, uint32_t col) {
          const core::Value x = sh->GetValue(row, col);
          v->push_back(x.IsNumeric() ? x.AsDouble() : kNaN);
        }, vis);
      }
    }
  };
  for (const auto& se : ch.series) {
    SeriesBuf b;
    b.color = se.color;
    b.axis = se.axis_group;
    // name: may be a cell reference (=Sheet!B1)
    b.name = se.name;
    if (!se.name.empty() && se.name[0] == '=') {
      uint32_t sid;
      Rect r;
      if (ResolveRangeText(wb, sheet, se.name, &sid, &r)) {
        if (const model::Sheet* sh = snap.Sheet(sid)) b.name = CellDisplay(snap, *sh, r.r0, r.c0);
      }
    }
    if (b.name.empty()) b.name = TR("Series") + std::to_string(out->series.size() + 1);
    if (se.python_data && se.data) {
      b.ext = se.data;
      if (!se.data->cats.empty() && !cats_done) {  // a PivotChart: the report's row labels
        out->cats = se.data->cats;
        if (se.data->cat_paths.size() == se.data->cats.size()) out->paths = se.data->cat_paths;  // by level (several row fields)
        cats_done = true;
      }
      if (se.data->x.empty()) {
        b.x.resize(se.data->y.size());
        for (size_t i = 0; i < b.x.size(); ++i) b.x[i] = static_cast<double>(i + 1);
      } else {
        b.numeric_x = engine::IsScatterType(ch.type);
      }
    } else {
      uint32_t sid;
      Rect r;
      std::vector<Rect> areas;  // one range, or several ("Sheet1!$B$2:$B$5,Sheet1!$B$9:$B$12")
      if (!se.values.empty() && se.values[0] == '{') {
        b.y = engine::ParseNumberList(se.values);  // an array constant (Edit Series: ={1,2,3})
      } else if (ResolveAreasText(wb, sheet, se.values, &sid, &areas)) {
        if (const model::Sheet* sh = snap.Sheet(sid)) {
          b.fmt = wb.styles->NumberFormat(wb.styles->Get(sh->GetStyle(areas[0].r0, areas[0].c0)));
          const bool map = ch.type == ChartType::FilledMap;
          for (const Rect& ar : areas) {
            ForEachInRange(*sh, ar, 5000000, [&](uint32_t row, uint32_t col) {
              const core::Value v = sh->GetValue(row, col);
              // Excel: text is plotted as zero, an empty cell as nothing (Hidden and Empty Cells), #N/A as an empty cell
              // or joined over, other errors as zero
              double y = kNaN;
              if (v.IsNumeric()) y = v.AsDouble();
              else if (v.IsBool()) y = v.boolean() ? 1.0 : 0.0;
              else if (v.IsString()) y = map ? kNaN : 0.0;
              else if (v.IsError()) {
                if (v.error() != core::Err::NA) y = 0.0;
                else if (!ch.na_as_blank) b.joined.push_back(static_cast<int>(b.y.size()));
              }
              b.y.push_back(y);
              if (map) b.texts.push_back(v.IsNumeric() ? std::string() : CellDisplay(snap, *sh, row, col));  // category maps
            }, vis);
          }
        }
      }
      b.x.resize(b.y.size());
      bool have_x = false;
      if (!se.x_values.empty() && ResolveAreasText(wb, sheet, se.x_values, &sid, &areas)) {
        r = areas[0];
        if (const model::Sheet* sh = snap.Sheet(sid)) {
          if (areas.size() == 1 && r.c1 > r.c0 && r.r1 > r.r0) {
            // several category columns: hierarchical labels (treemap, sunburst); a blank parent repeats the one above
            if (!cats_done) {
              std::vector<std::string> prev;
              // the categories run down the rows (levels across the columns), or along a row when the series are in
              // rows (levels down the rows)
              const bool across = r.Cols() == b.y.size() && r.Rows() != b.y.size();
              const uint32_t n0 = across ? r.c0 : r.r0, n1 = across ? r.c1 : r.r1, l0 = across ? r.r0 : r.c0, l1 = across ? r.r1 : r.c1;
              for (uint32_t row = n0; row <= n1 && out->paths.size() < b.y.size(); ++row) {
                if (vis && (across ? sh->cols->IsHidden(row) : sh->rows->IsHidden(row))) continue;
                std::vector<std::string> path;
                for (uint32_t col = l0; col <= l1; ++col) path.push_back(across ? CellDisplay(snap, *sh, col, row) : CellDisplay(snap, *sh, row, col));
                size_t last = path.size();
                while (last > 0 && path[last - 1].empty()) --last;
                path.resize(last);
                for (size_t k = 0; k < path.size(); ++k) {
                  if (path[k].empty() && k < prev.size()) path[k] = prev[k];
                }
                prev = path;
                out->cats.push_back(path.empty() ? std::string() : path.back());
                out->paths.push_back(std::move(path));
              }
              cats_done = true;
            }
          } else {
            std::vector<double> xs;
            std::vector<std::string> labels;
            bool text = false;
            const std::string first_fmt = wb.styles->NumberFormat(wb.styles->Get(sh->GetStyle(areas[0].r0, areas[0].c0)));
            for (const Rect& ar : areas) {
              if (xs.size() >= b.y.size()) break;
              ForEachInRange(*sh, ar, b.y.size() - xs.size(), [&](uint32_t row, uint32_t col) {
                const core::Value v = sh->GetValue(row, col);
                if (!v.IsNumeric()) text = true;
                xs.push_back(v.IsNumeric() ? v.AsDouble() : kNaN);
                labels.push_back(CellDisplay(snap, *sh, row, col));
              }, vis);
            }
            const bool numeric_x = !text && (engine::IsScatterType(ch.type) || engine::IsScatterType(engine::EffectiveSeriesType(ch, out->series.size())));
            if (numeric_x) {
              for (size_t i = 0; i < b.x.size(); ++i) b.x[i] = i < xs.size() ? xs[i] : kNaN;
              have_x = true;
              b.numeric_x = true;
            } else if (!cats_done) {
              out->cats = std::move(labels);
              out->cat_vals = xs;  // (a date axis places the points by these)
              out->cat_fmt = first_fmt;
              out->cat_dates = !text && !xs.empty() && formula::IsDateFormat(first_fmt);
              cats_done = true;
            }
          }
        }
      }
      if (!have_x) {
        for (size_t i = 0; i < b.x.size(); ++i) b.x[i] = static_cast<double>(i + 1);
      }
    }
    if (ch.type == ChartType::Bubble) numbers(se.bubble_sizes, &b.sizes);
    if ((se.label_show & 32) && !se.label_range.empty()) {
      // data labels' Value From Cells: the cells' text, one per point
      uint32_t lsid;
      std::vector<Rect> lareas;
      if (ResolveAreasText(wb, sheet, se.label_range, &lsid, &lareas)) {
        if (const model::Sheet* sh = snap.Sheet(lsid)) {
          for (const Rect& ar : lareas) ForEachInRange(*sh, ar, 100000, [&](uint32_t row, uint32_t col) { b.label_texts.push_back(CellDisplay(snap, *sh, row, col)); }, vis);
        }
      }
    }
    if (se.err == model::ErrorBarKind::Custom) {
      // custom error amounts: a range or an array constant
      auto amounts = [&](const std::string& text, std::vector<double>* out_v) {
        *out_v = engine::ParseNumberList(text);
        uint32_t esid;
        Rect er;
        if (out_v->empty() && !text.empty() && ResolveRangeText(wb, sheet, text, &esid, &er)) {
          if (const model::Sheet* sh = snap.Sheet(esid)) {
            ForEachInRange(*sh, er, 5000000, [&](uint32_t row, uint32_t col) {
              const core::Value v = sh->GetValue(row, col);
              out_v->push_back(v.IsNumeric() ? v.AsDouble() : 0.0);
            }, vis);
          }
        }
      };
      amounts(se.err_plus, &b.err_plus);
      amounts(se.err_minus, &b.err_minus);
    }
    if (se.xerr == model::ErrorBarKind::Custom) {  // a scatter chart's X error bars: the same kinds of amounts
      for (int k = 0; k < 2; ++k) {
        const std::string& text = k == 0 ? se.xerr_plus : se.xerr_minus;
        std::vector<double>* out_v = k == 0 ? &b.xerr_plus : &b.xerr_minus;
        *out_v = engine::ParseNumberList(text);
        uint32_t esid;
        Rect er;
        if (out_v->empty() && !text.empty() && ResolveRangeText(wb, sheet, text, &esid, &er)) {
          if (const model::Sheet* sh = snap.Sheet(esid)) {
            ForEachInRange(*sh, er, 5000000, [&](uint32_t row, uint32_t col) {
              const core::Value v = sh->GetValue(row, col);
              out_v->push_back(v.IsNumeric() ? v.AsDouble() : 0.0);
            }, vis);
          }
        }
      }
    }
    out->points += static_cast<size_t>(std::max(0, b.N()));
    out->series.push_back(std::move(b));
  }
  // Chart Filters: a series left out keeps its place (its color, its formatting) but shows nothing; hidden categories
  // leave every series and the axis
  bool any_filter = !ch.hidden_cats.empty();
  for (size_t i = 0; i < out->series.size() && i < ch.series.size(); ++i) {
    if (!ch.series[i].filtered) continue;
    auto& b = out->series[i];
    b.ext.reset();
    b.x.clear();
    b.y.clear();
    b.texts.clear();
    b.err_plus.clear();
    b.err_minus.clear();
    b.sizes.clear();
    b.label_texts.clear();
    b.joined.clear();
    any_filter = true;
  }
  if (!ch.hidden_cats.empty()) {
    auto hidden = [&](size_t k) { return std::binary_search(ch.hidden_cats.begin(), ch.hidden_cats.end(), static_cast<int>(k)); };
    auto compact = [&](auto& vec) {
      size_t w = 0;
      for (size_t k = 0; k < vec.size(); ++k) {
        if (!hidden(k)) vec[w++] = std::move(vec[k]);
      }
      vec.resize(w);
    };
    for (auto& b : out->series) {
      if (b.ext) continue;  // Python data has no categories to filter
      compact(b.y);
      compact(b.texts);
      compact(b.err_plus);
      compact(b.err_minus);
      compact(b.sizes);
      compact(b.label_texts);
      b.joined.clear();
      if (b.numeric_x) {
        compact(b.x);
      } else {
        b.x.resize(b.y.size());
        for (size_t k = 0; k < b.x.size(); ++k) b.x[k] = static_cast<double>(k + 1);
      }
    }
    compact(out->cats);
    compact(out->paths);
    compact(out->cat_vals);
  }
  if (any_filter) {
    out->points = 0;
    for (const auto& b : out->series) out->points += static_cast<size_t>(std::max(0, b.N()));
  }
}

namespace {

void BuildCache(const AppState& s, uint32_t sheet, const ChartObj& ch, ChartCache& cc) {
  ResolveChartValues(*s.snap, sheet, ch, &cc.v);
  // matrix for the column / bar series: [bar series x points]
  cc.bars.clear();
  cc.bar_idx.clear();
  size_t groups = 0;
  for (size_t i = 0; i < cc.v.series.size(); ++i) {
    if (!engine::IsBarType(engine::EffectiveSeriesType(ch, i))) continue;
    cc.bar_idx.push_back(i);
    groups = std::max(groups, static_cast<size_t>(std::max(0, cc.v.series[i].N())));
  }
  if (!cc.bar_idx.empty()) {
    groups = std::min<size_t>(groups, 5000);
    cc.bars.assign(cc.bar_idx.size() * groups, 0.0);
    for (size_t k = 0; k < cc.bar_idx.size(); ++k) {
      const SeriesBuf& b = cc.v.series[cc.bar_idx[k]];
      const double* y = b.Y();
      const size_t n = std::min(groups, static_cast<size_t>(std::max(0, b.N())));
      for (size_t j = 0; j < n; ++j) cc.bars[k * groups + j] = std::isfinite(y[j]) ? y[j] : 0.0;
    }
  }
  cc.version = s.snap->version;
  cc.def_hash = ChartDefHash(ch);
  cc.built_at = ImGui::GetTime();
  cc.scene_ok = false;
}

ChartCache& CacheFor(const AppState& s, uint32_t sheet, const ChartObj& ch) {
  ChartCache& cc = U().charts[ch.id];
  const size_t h = ChartDefHash(ch);
  const bool stale = cc.version != s.snap->version;
  // throttle for large data (rebuild at most every 0.25 s); rebuild immediately when the definition changes
  if (h != cc.def_hash || cc.built_at < 0 || (stale && (cc.v.points < 200000 || ImGui::GetTime() - cc.built_at > 0.25))) BuildCache(s, sheet, ch, cc);
  return cc;
}

// ImPlot's colormap for the current theme (pie slices): one per set of theme colors
const char* EnsureColormap() {
  static std::string name;
  const model::Theme& th = model::CurrentTheme();
  char buf[64];
  std::snprintf(buf, sizeof buf, "Yc%06X%06X%06X", th.colors[model::kAccent1], th.colors[model::kAccent2], th.colors[model::kAccent6]);
  name = buf;
  if (ImPlot::GetColormapIndex(name.c_str()) == -1) {
    ImU32 cols[12];
    for (size_t i = 0; i < 12; ++i) cols[i] = engine::ChartPalette(i);
    ImPlot::AddColormap(name.c_str(), cols, 12, true);
  }
  return name.c_str();
}

// the chart kinds ImPlot does not draw: a scene from engine/chart_layout drawn with the window's draw list
void DrawSceneChart(ChartCache& cc, const ChartObj& ch, const ImVec2& pos, const ImVec2& size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  ImFont* bold = ui::F().ui_bold ? ui::F().ui_bold : font;
  const float fs = ImGui::GetFontSize();
  if (!cc.scene_ok || cc.scene_w != size.x || cc.scene_h != size.y || cc.scene_fs != fs) {
    auto measure = [&](const std::string& t, float sc) { return font->CalcTextSizeA(fs * sc, FLT_MAX, 0.0f, t.c_str()).x; };
    engine::BuildChartScene(ch, cc.v, size.x, size.y, fs, measure, &cc.scene);
    cc.scene_ok = true;
    cc.scene_w = size.x;
    cc.scene_h = size.y;
    cc.scene_fs = fs;
  }
  const ImVec2 br(pos.x + size.x, pos.y + size.y);
  dl->PushClipRect(pos, br, true);
  auto P = [&](float x, float y) { return ImVec2(pos.x + x, pos.y + y); };
  static std::vector<ImVec2> pts;
  cc.hit.text_boxes.assign(cc.scene.prims.size(), ImRect());
  for (size_t pi = 0; pi < cc.scene.prims.size(); ++pi) {
    const auto& p = cc.scene.prims[pi];
    switch (p.kind) {
      case engine::ChartPrim::Rect:
        if (p.fill) dl->AddRectFilled(P(p.pts[0], p.pts[1]), P(p.pts[2], p.pts[3]), p.fill);
        if (p.stroke) dl->AddRect(P(p.pts[0], p.pts[1]), P(p.pts[2], p.pts[3]), p.stroke, 0.0f, p.width);
        break;
      case engine::ChartPrim::Poly:
      case engine::ChartPrim::Line:
        pts.clear();
        for (size_t k = 0; k + 1 < p.pts.size(); k += 2) pts.push_back(P(p.pts[k], p.pts[k + 1]));
        if (pts.size() < 2) break;
        if (p.kind == engine::ChartPrim::Poly) {
          if (p.fill && pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), p.fill);
          if (p.stroke) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), p.stroke, p.width, ImDrawFlags_Closed);
        } else {
          dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), p.stroke, p.width);
        }
        break;
      case engine::ChartPrim::Circle:
        if (p.fill) dl->AddCircleFilled(P(p.pts[0], p.pts[1]), p.pts[2], p.fill, 12);
        if (p.stroke) dl->AddCircle(P(p.pts[0], p.pts[1]), p.pts[2], p.stroke, 12, 1.2f);
        break;
      case engine::ChartPrim::Text: {
        ImFont* f = p.bold ? bold : font;
        const float sz = fs * p.scale;
        const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, p.text.c_str());
        ImVec2 at = P(p.pts[0], p.pts[1]);
        at.x -= p.halign == 1 ? ts.x / 2 : (p.halign == 2 ? ts.x : 0);
        at.y -= p.valign == 1 ? ts.y / 2 : (p.valign == 2 ? ts.y : 0);
        if (p.rotate) {
          // drawn level, then its vertices turned around the anchor
          const ImVec2 c = P(p.pts[0], p.pts[1]);
          const int v0 = dl->VtxBuffer.Size;
          dl->AddText(f, sz, ImFloor(at), p.stroke, p.text.c_str());
          const float a = -static_cast<float>(p.rotate) * 3.14159265f / 180.0f, cs = std::cos(a), sn = std::sin(a);
          for (int k = v0; k < dl->VtxBuffer.Size; ++k) {
            ImVec2& q = dl->VtxBuffer[k].pos;
            const ImVec2 d = q - c;
            q = ImVec2(c.x + d.x * cs - d.y * sn, c.y + d.x * sn + d.y * cs);
          }
          const float hw = std::fabs(ts.x * cs) + std::fabs(ts.y * sn), hh = std::fabs(ts.x * sn) + std::fabs(ts.y * cs);
          cc.hit.text_boxes[pi] = ImRect(c - ImVec2(hw, hh) * 0.5f, c + ImVec2(hw, hh) * 0.5f);
          break;
        }
        dl->AddText(f, sz, ImFloor(at), p.stroke, p.text.c_str());
        cc.hit.text_boxes[pi] = ImRect(at, at + ts);
        break;
      }
    }
  }
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(pos);
  ImGui::Dummy(size);
}

const char* TrendName(model::TrendKind k) {
  switch (k) {
    case model::TrendKind::Linear: return TR("Linear");
    case model::TrendKind::Exponential: return TR("Exponential");
    case model::TrendKind::Logarithmic: return TR("Logarithmic");
    case model::TrendKind::Polynomial: return TR("Polynomial");
    case model::TrendKind::Power: return TR("Power");
    case model::TrendKind::MovingAverage: return TR("Moving Average");
    default: return "";
  }
}

// a series' trendlines: the first (kept in the series' trend fields), then the others
std::vector<model::ChartTrendline> TrendlinesOf(const model::ChartSeries& d) {
  std::vector<model::ChartTrendline> v;
  if (d.trend == model::TrendKind::None) return v;
  model::ChartTrendline t;
  t.kind = d.trend;
  t.order = d.trend_order;
  t.period = d.trend_period;
  t.forward = d.trend_forward;
  t.backward = d.trend_backward;
  t.eq = d.trend_eq;
  t.r2 = d.trend_r2;
  t.name = d.trend_name;
  t.intercept_on = d.trend_icept_on;
  t.intercept = d.trend_icept;
  v.push_back(std::move(t));
  v.insert(v.end(), d.more_trends.begin(), d.more_trends.end());
  return v;
}

// the trendline's name in the legend: its own, or Excel's automatic one ("Linear (Sales)", "2 per. Mov. Avg. (Sales)")
std::string TrendLegendName(const model::ChartTrendline& t, const std::string& series_name) {
  if (!t.name.empty()) return t.name;
  const char* k = TR("Linear");
  switch (t.kind) {
    case model::TrendKind::Exponential: k = TR("Expon."); break;
    case model::TrendKind::Logarithmic: k = TR("Log."); break;
    case model::TrendKind::Polynomial: k = TR("Poly."); break;
    case model::TrendKind::Power: k = TR("Power"); break;
    case model::TrendKind::MovingAverage: return std::to_string(t.period) + " " + TR("per. Mov. Avg.") + " (" + series_name + ")";
    default: break;
  }
  return std::string(k) + " (" + series_name + ")";
}

}  // namespace

// draw the chart body (shared by the sheet, floating windows and panels)
namespace {

constexpr float kPtPx = 4.0f / 3.0f;  // points -> unscaled pixels
ImU32 ArgbCol(uint32_t c) { return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) ? (c >> 24) & 0xFF : 255); }
ImU32 ChartTextCol(const model::ChartFont& f) { return f.color ? ArgbCol(f.color) : IM_COL32(0x59, 0x59, 0x59, 255); }
// a chart text's font size for PushFont (Excel's defaults: title 14 pt, the rest 9 pt)
float ChartFontSize(const model::ChartFont& f, float default_pt, float font_scale) { return (f.size > 0 ? f.size : default_pt) * kPtPx * font_scale; }
// a chart text's face: its own font name, bold and italic (the chart's UI font while it has none)
ImFont* ChartFontOf(const model::ChartFont& f) {
  if (!f.name.empty() || f.italic) return ui::CellFont(f.name.empty() ? std::string("Segoe UI") : f.name, f.bold, f.italic);
  return f.bold && ui::F().ui_bold ? ui::F().ui_bold : ui::F().ui;
}
// an underlined chart text: the line under it
void ChartUnderline(ImDrawList* dl, const model::ChartFont& f, ImVec2 tp, ImVec2 ts, ImU32 col) {
  if (f.underline && ts.x > 0) dl->AddLine(ImVec2(tp.x, tp.y + ts.y - 1), ImVec2(tp.x + ts.x, tp.y + ts.y - 1), col, std::max(1.0f, ts.y / 16));
}
// an axis' text: its own font (Format Axis > Text Options) or the chart's; its title's, as Excel's 10 pt default
const model::ChartFont& AxisFontOf(const ChartObj& ch, const model::ChartAxisFormat& a) { return a.own_font ? a.font : ch.axis_font; }
const model::ChartFont& AxisTitleFontOf(const ChartObj& ch, const model::ChartAxisFormat& a) { return a.title_font == model::ChartFont{} ? ch.axis_font : a.title_font; }

struct LegendEntry {
  std::string text;
  ImU32 col = 0;
  int kind = 0;  // 0 box, 1 line, 2 line with a marker
  int series = -1;
  bool own = true;  // a series' own entry (false: a pie's category, a trendline)
  const model::ChartFont* font = nullptr;  // its own font (Format Legend Entry), else the legend's
  int cat = -1;                            // a category's entry: its category
};

int ImPlotMarkerOf(int m) {
  switch (m) {
    case 1: return ImPlotMarker_Circle;
    case 2: return ImPlotMarker_Square;
    case 3: return ImPlotMarker_Diamond;
    case 4: return ImPlotMarker_Up;
    case 5: return ImPlotMarker_Cross;
    case 6: return ImPlotMarker_Plus;
    default: return ImPlotMarker_None;
  }
}

// Excel-like automatic value axis bounds: round steps (1, 2, 5 x 10^n) with a little room above the data; starts at zero
// unless the values are far from it. *step: the major unit
void NiceAxis(double lo, double hi, double* a, double* b, double* step_out = nullptr) {
  if (!(hi > lo)) hi = lo + 1;
  const double span = hi - lo;
  const double raw = span / 10;
  const double mag = std::pow(10.0, std::floor(std::log10(raw)));
  const double f = raw / mag;
  const double step = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * mag;
  *b = hi <= 0 && lo < 0 ? 0 : std::ceil((hi + span * 0.05) / step) * step;
  *a = lo >= 0 && lo < hi * 5 / 6 ? 0 : std::floor((lo - span * 0.05) / step) * step;
  if (hi <= 0 && lo < 0) *a = std::floor((lo - span * 0.05) / step) * step;
  if (step_out) *step_out = step;
}

int FormatTick(double value, char* buff, int size, void* data) {
  const std::string t = engine::FormatChartNumber(value, *static_cast<const std::string*>(data));
  return std::snprintf(buff, static_cast<size_t>(size), "%s", t.c_str());
}

// an Excel BGR color with a transparency (%)
ImU32 WithTransparency(ImU32 c, int transparency) {
  const int a = 255 * (100 - std::clamp(transparency, 0, 100)) / 100;
  return (c & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24);
}

// a polyline with Excel's dash types (1 round dot, 2 square dot, 3 dash, 4 dash dot, 5 long dash); NaN points break it
void DashedPolyline(ImDrawList* dl, const std::vector<ImVec2>& pts, ImU32 col, float w, int dash) {
  static const float kPat[6][4] = {{1, 0, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {4, 3, 0, 0}, {4, 3, 1, 3}, {8, 3, 0, 0}};
  const int d = std::clamp(dash, 0, 5);
  const int np = kPat[d][2] > 0 ? 4 : 2;
  float pat[4];
  for (int k = 0; k < 4; ++k) pat[k] = kPat[d][k] * std::max(1.0f, w);
  int seg = 0;
  float left = pat[0];
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    ImVec2 a = pts[i];
    const ImVec2 b = pts[i + 1];
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
    float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    if (len <= 0) continue;
    const ImVec2 u((b.x - a.x) / len, (b.y - a.y) / len);
    while (len > 0) {
      const float t = std::min(left, len);
      const ImVec2 e(a.x + u.x * t, a.y + u.y * t);
      if (seg % 2 == 0) dl->AddLine(a, e, col, w);
      a = e;
      len -= t;
      left -= t;
      if (left <= 0) {
        seg = (seg + 1) % np;
        left = pat[seg];
      }
    }
  }
}

// ---- Format Data Series > Fill & Line: gradient, picture, pattern fills; compound lines, caps, joins, arrows ----

// a color mixed toward white (t 0..1), keeping its alpha
ImU32 Lighter(ImU32 c, float t) {
  auto ch = [&](int sh) {
    const float v = static_cast<float>((c >> sh) & 0xFF);
    return static_cast<ImU32>(v + (255.0f - v) * t) << sh;
  };
  return ch(0) | ch(8) | ch(16) | (c & 0xFF000000u);
}

// the screen points' bounding box
ImRect PolyBounds(const std::vector<ImVec2>& poly) {
  ImRect bb(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
  for (const ImVec2& p : poly) bb.Add(p);
  return bb;
}

bool InPolygon(const std::vector<ImVec2>& poly, ImVec2 m) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const ImVec2 a = poly[i], b = poly[j];
    if ((a.y > m.y) != (b.y > m.y) && m.x < (b.x - a.x) * (m.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

// the parts of the line p + t*u inside the polygon, drawn (hatching clipped to any polygon)
void ClippedLine(ImDrawList* dl, const std::vector<ImVec2>& poly, ImVec2 p, ImVec2 u, ImU32 col, float w) {
  std::vector<float> ts;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const ImVec2 a = poly[j], e = poly[i] - poly[j];
    const float den = u.x * e.y - u.y * e.x;
    if (std::fabs(den) < 1e-9f) continue;
    const ImVec2 d = a - p;
    const float t = (d.x * e.y - d.y * e.x) / den, s = (d.x * u.y - d.y * u.x) / den;
    if (s >= 0 && s < 1) ts.push_back(t);
  }
  std::sort(ts.begin(), ts.end());
  for (size_t k = 0; k + 1 < ts.size(); k += 2) dl->AddLine(p + u * ts[k], p + u * ts[k + 1], col, w);
}

// Excel's patterns (Pattern fill): dotted percentages, then lines and grids in the foreground color
void HatchPolygon(ImDrawList* dl, const std::vector<ImVec2>& poly, int pattern, ImU32 fore) {
  if (poly.size() < 3) return;
  const ImRect bb = PolyBounds(poly);
  const float sc = S(1);
  const int p = std::clamp(pattern, 0, model::kChartPatternCount - 1);
  if (p <= 3) {  // 5%, 10%, 25%, 50%: dots
    static const float kGap[4] = {8, 6, 4, 2};
    const float g = kGap[p] * sc;
    int row = 0;
    for (float y = bb.Min.y + g * 0.5f; y < bb.Max.y; y += g, ++row) {
      for (float x = bb.Min.x + ((row & 1) ? g : g * 0.5f); x < bb.Max.x; x += g * 2) {
        if (InPolygon(poly, ImVec2(x, y))) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sc, y + sc), fore);
      }
    }
    return;
  }
  // 4 light horizontal, 5 light vertical, 6 light downward diagonal, 7 light upward diagonal, 8 dark horizontal,
  // 9 dark vertical, 10 small grid, 11 diagonal cross
  const bool dark = p == 8 || p == 9;
  const float gap = (dark ? 3.0f : 5.0f) * sc, w = (dark ? 1.5f : 1.0f) * sc;
  auto family = [&](ImVec2 u) {
    const ImVec2 n(-u.y, u.x);
    const float span = bb.GetWidth() + bb.GetHeight();
    const ImVec2 c = bb.GetCenter();
    for (float o = -span; o <= span; o += gap) ClippedLine(dl, poly, c + n * o - u * span, u, fore, w);
  };
  const float r = 0.70710678f;
  if (p == 4 || p == 8 || p == 10) family(ImVec2(1, 0));
  if (p == 5 || p == 9 || p == 10) family(ImVec2(0, 1));
  if (p == 6 || p == 11) family(ImVec2(r, r));
  if (p == 7 || p == 11) family(ImVec2(r, -r));
}

ImTextureID PictureFillTexture(const std::string& bytes, int* w, int* h) {
  struct T {
    ImTextureID tex = 0;
    int w = 0, h = 0;
  };
  static std::unordered_map<size_t, T> cache;
  if (bytes.empty()) return 0;
  const size_t key = std::hash<std::string>{}(bytes);
  auto it = cache.find(key);
  if (it == cache.end()) {
    T t;
    t.tex = TextureFromImage(bytes, &t.w, &t.h);
    it = cache.emplace(key, t).first;
  }
  if (w) *w = it->second.w;
  if (h) *h = it->second.h;
  return it->second.tex;
}

// the part of a polygon inside a rectangle (Sutherland-Hodgman, one side after another)
std::vector<ImVec2> ClipToRect(const std::vector<ImVec2>& poly, const ImRect& r) {
  std::vector<ImVec2> out = poly, in;
  for (int side = 0; side < 4 && !out.empty(); ++side) {
    in.swap(out);
    out.clear();
    auto inside = [&](ImVec2 p) {
      switch (side) {
        case 0: return p.x >= r.Min.x;
        case 1: return p.x <= r.Max.x;
        case 2: return p.y >= r.Min.y;
        default: return p.y <= r.Max.y;
      }
    };
    auto cross = [&](ImVec2 a, ImVec2 b) {
      const float t = side == 0 ? (r.Min.x - a.x) / (b.x - a.x) : side == 1 ? (r.Max.x - a.x) / (b.x - a.x) : side == 2 ? (r.Min.y - a.y) / (b.y - a.y) : (r.Max.y - a.y) / (b.y - a.y);
      return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    };
    for (size_t i = 0; i < in.size(); ++i) {
      const ImVec2 a = in[i], b = in[(i + 1) % in.size()];
      const bool ia = inside(a), ib = inside(b);
      if (ia) out.push_back(a);
      if (ia != ib) out.push_back(cross(a, b));
    }
  }
  return out;
}

// Tile picture as texture over any shape: the picture at its own size times the scale, placed from the alignment
// point plus the offsets, every other tile mirrored when asked (the sampler clamps: each tile is drawn on its own)
void TilePolygon(ImDrawList* dl, const std::vector<ImVec2>& poly, ImTextureID tex, int tw, int th, const model::ChartSeries& d, ImU32 tint) {
  const ImRect bb = PolyBounds(poly);
  const float w = std::max(2.0f, static_cast<float>(tw) * S(1) * std::clamp(d.tile_sx, 1.0f, 1000.0f) / 100.0f);
  const float h = std::max(2.0f, static_cast<float>(th) * S(1) * std::clamp(d.tile_sy, 1.0f, 1000.0f) / 100.0f);
  const int ax = std::clamp(d.tile_align, 0, 8) % 3, ay = std::clamp(d.tile_align, 0, 8) / 3;
  float x0 = ax == 0 ? bb.Min.x : ax == 1 ? bb.GetCenter().x - w * 0.5f : bb.Max.x - w;
  float y0 = ay == 0 ? bb.Min.y : ay == 1 ? bb.GetCenter().y - h * 0.5f : bb.Max.y - h;
  x0 += d.tile_ox * kPtPx * S(1);
  y0 += d.tile_oy * kPtPx * S(1);
  const int i0 = static_cast<int>(std::floor((bb.Min.x - x0) / w)), i1 = static_cast<int>(std::ceil((bb.Max.x - x0) / w));
  const int j0 = static_cast<int>(std::floor((bb.Min.y - y0) / h)), j1 = static_cast<int>(std::ceil((bb.Max.y - y0) / h));
  if (static_cast<int64_t>(i1 - i0) * (j1 - j0) > 4000) return;  // a picture scaled down to dust
  dl->PushTexture(ImTextureRef(tex));
  const ImDrawFlags flags0 = dl->Flags;
  dl->Flags &= ~ImDrawFlags_AAFill;  // tiles meet without seams (an anti-aliased edge shows between them)
  for (int j = j0; j < j1; ++j) {
    for (int i = i0; i < i1; ++i) {
      const ImRect t(ImVec2(x0 + static_cast<float>(i) * w, y0 + static_cast<float>(j) * h), ImVec2(x0 + static_cast<float>(i + 1) * w, y0 + static_cast<float>(j + 1) * h));
      const std::vector<ImVec2> piece = ClipToRect(poly, t);
      if (piece.size() < 3) continue;
      const bool fx = (d.tile_mirror & 1) && (i & 1), fy = (d.tile_mirror & 2) && (j & 1);
      const int v0 = dl->VtxBuffer.Size;
      dl->AddConcavePolyFilled(piece.data(), static_cast<int>(piece.size()), tint);
      ImGui::ShadeVertsLinearUV(dl, v0, dl->VtxBuffer.Size, t.Min, t.Max, ImVec2(fx ? 1.0f : 0.0f, fy ? 1.0f : 0.0f), ImVec2(fx ? 0.0f : 1.0f, fy ? 0.0f : 1.0f), true);
    }
  }
  dl->Flags = flags0;
  dl->PopTexture();
}

// a column, bar, slice or area filled as the series says: solid (in col), gradient, picture or texture, pattern
void SeriesFillPoly(ImDrawList* dl, const std::vector<ImVec2>& poly, const model::ChartSeries& d, ImU32 col, bool rect) {
  if (poly.size() < 3 || d.no_fill) return;
  const ImRect bb = PolyBounds(poly);
  const ImU32 c = WithTransparency(col, d.transparency);
  auto fill = [&](ImU32 fc) {
    if (rect) dl->AddRectFilled(bb.Min, bb.Max, fc);
    else dl->AddConcavePolyFilled(poly.data(), static_cast<int>(poly.size()), fc);
  };
  switch (d.fill_type) {
    case 1: {  // gradient: from the color to the second stop along the angle
      const ImU32 c2 = d.grad_color2 ? WithTransparency(ExcelColor(d.grad_color2), d.transparency) : Lighter(c, 0.65f);
      const int v0 = dl->VtxBuffer.Size;
      fill(IM_COL32(255, 255, 255, (c >> 24) & 0xFF));
      const float a = static_cast<float>(d.grad_angle) * 3.14159265f / 180.0f;
      const ImVec2 u(std::cos(a), std::sin(a));
      const float half = std::fabs(u.x) * bb.GetWidth() * 0.5f + std::fabs(u.y) * bb.GetHeight() * 0.5f;
      const ImVec2 ctr = bb.GetCenter();
      ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ctr - u * half, ctr + u * half, c, c2);
      break;
    }
    case 2: {  // picture or texture: stretched, or tiled at its own size
      int tw = 0, th = 0;
      const ImTextureID tex = PictureFillTexture(d.picture, &tw, &th);
      if (!tex) {
        fill(c);
        break;
      }
      const ImU32 tint = IM_COL32(255, 255, 255, (c >> 24) & 0xFF);
      if (d.picture_tile && tw > 0 && th > 0) {
        TilePolygon(dl, poly, tex, tw, th, d, tint);  // Tile picture as texture: any shape (columns, slices, areas)
      } else if (rect) {
        dl->AddImage(ImTextureRef(tex), bb.Min, bb.Max, ImVec2(0, 0), ImVec2(1, 1), tint);
      } else {
        dl->PushTexture(ImTextureRef(tex));
        const int v0 = dl->VtxBuffer.Size;
        dl->AddConcavePolyFilled(poly.data(), static_cast<int>(poly.size()), tint);
        ImGui::ShadeVertsLinearUV(dl, v0, dl->VtxBuffer.Size, bb.Min, bb.Max, ImVec2(0, 0), ImVec2(1, 1), true);
        dl->PopTexture();
      }
      break;
    }
    case 3:  // pattern: the foreground color's lines or dots on the background color
      fill(d.pattern_back ? WithTransparency(ExcelColor(d.pattern_back), d.transparency) : WithTransparency(IM_COL32_WHITE, d.transparency));
      HatchPolygon(dl, poly, d.pattern, c);
      break;
    default: fill(c); break;
  }
}

std::vector<ImVec2> RectPoly(const ImRect& r) { return {r.Min, ImVec2(r.Max.x, r.Min.y), r.Max, ImVec2(r.Min.x, r.Max.y)}; }

// a line's ends drawn as Excel's arrow types (1 arrow, 2 open arrow, 3 stealth, 4 diamond, 5 oval) at tip, coming from
// the direction of `from`
void LineEndShape(ImDrawList* dl, ImVec2 from, ImVec2 tip, int kind, ImU32 col, float w) {
  ImVec2 u = tip - from;
  const float len = std::sqrt(u.x * u.x + u.y * u.y);
  if (len <= 0 || kind <= 0) return;
  u = u / len;
  const ImVec2 n(-u.y, u.x);
  const float L = std::max(6.0f, w * 3.5f), H = std::max(3.0f, w * 1.8f);
  const ImVec2 base = tip - u * L;
  switch (kind) {
    case 1: dl->AddTriangleFilled(tip, base + n * H, base - n * H, col); break;
    case 2:
      dl->AddLine(tip, base + n * H, col, std::max(1.0f, w));
      dl->AddLine(tip, base - n * H, col, std::max(1.0f, w));
      break;
    case 3: {
      const ImVec2 q[4] = {tip, base + n * H, tip - u * (L * 0.6f), base - n * H};
      dl->AddConcavePolyFilled(q, 4, col);
      break;
    }
    case 4: {
      const ImVec2 m = tip - u * (L * 0.5f);
      const ImVec2 q[4] = {tip, m + n * H, tip - u * L, m - n * H};
      dl->AddConvexPolyFilled(q, 4, col);
      break;
    }
    default: dl->AddCircleFilled(tip - u * (L * 0.35f), L * 0.35f, col); break;
  }
}

// Vary colors by point: the one series shown, when it is columns or bars and asks for it (-1: none)
int VaryByPoint(const ChartObj& ch) {
  int only = -1, shown = 0;
  for (size_t i = 0; i < ch.series.size(); ++i) {
    if (ch.series[i].filtered) continue;
    ++shown;
    only = static_cast<int>(i);
  }
  if (shown != 1 || !ch.series[static_cast<size_t>(only)].vary_points) return -1;
  return engine::IsBarType(engine::EffectiveSeriesType(ch, static_cast<size_t>(only))) ? only : -1;
}

bool StyledLine(const model::ChartSeries& d) { return d.dash != 0 || d.compound != 0 || d.cap != 0 || d.join != 0 || d.begin_arrow != 0 || d.end_arrow != 0; }

// one unbroken polyline with the series' dash type, compound type, cap and join
void StyledRun(ImDrawList* dl, const std::vector<ImVec2>& pts, ImU32 col, float w, const model::ChartSeries& d) {
  if (pts.size() < 2) return;
  // compound lines: parallel strokes across the width (Excel's Double, Thick Thin, Thin Thick, Triple)
  struct Stroke {
    float off, w;
  };
  std::vector<Stroke> strokes;
  switch (d.compound) {
    case 1: strokes = {{-w / 3, w / 3}, {w / 3, w / 3}}; break;
    case 2: strokes = {{-w * 0.2f, w * 0.6f}, {w * 0.4f, w * 0.2f}}; break;
    case 3: strokes = {{-w * 0.4f, w * 0.2f}, {w * 0.2f, w * 0.6f}}; break;
    case 4: strokes = {{-w * 0.4f, w * 0.2f}, {0, w * 0.2f}, {w * 0.4f, w * 0.2f}}; break;
    default: strokes = {{0, w}}; break;
  }
  if (d.compound != 0) w = std::max(w, 3.0f);
  std::vector<ImVec2> moved(pts.size());
  for (const Stroke& st : strokes) {
    for (size_t k = 0; k < pts.size(); ++k) {
      // the normal at each point: the average of its segments'
      ImVec2 t(0, 0);
      if (k > 0) t += pts[k] - pts[k - 1];
      if (k + 1 < pts.size()) t += pts[k + 1] - pts[k];
      const float l = std::sqrt(t.x * t.x + t.y * t.y);
      moved[k] = l > 0 ? pts[k] + ImVec2(-t.y / l, t.x / l) * st.off : pts[k];
    }
    const float sw = std::max(1.0f, st.w);
    if (d.dash != 0) {
      DashedPolyline(dl, moved, col, sw, d.dash);
    } else if (d.join == 2) {  // miter
      dl->AddPolyline(moved.data(), static_cast<int>(moved.size()), col, sw, ImDrawFlags_None);
    } else {
      for (size_t k = 0; k + 1 < moved.size(); ++k) dl->AddLine(moved[k], moved[k + 1], col, sw);
      for (size_t k = 1; k + 1 < moved.size(); ++k) {
        if (d.join == 1) {  // bevel: the outer gap closed by a triangle
          const ImVec2 a = moved[k] - moved[k - 1], b = moved[k + 1] - moved[k];
          const float la = std::sqrt(a.x * a.x + a.y * a.y), lb = std::sqrt(b.x * b.x + b.y * b.y);
          if (la <= 0 || lb <= 0) continue;
          const ImVec2 na(-a.y / la, a.x / la), nb(-b.y / lb, b.x / lb);
          dl->AddTriangleFilled(moved[k], moved[k] + na * (sw * 0.5f), moved[k] + nb * (sw * 0.5f), col);
          dl->AddTriangleFilled(moved[k], moved[k] - na * (sw * 0.5f), moved[k] - nb * (sw * 0.5f), col);
        } else {  // round
          dl->AddCircleFilled(moved[k], sw * 0.5f, col);
        }
      }
    }
  }
  // caps: round or square ends (flat: none)
  if (d.cap != 0 && d.dash == 0) {
    for (int e = 0; e < 2; ++e) {
      const ImVec2 tip = e ? pts.back() : pts.front(), prev = e ? pts[pts.size() - 2] : pts[1];
      ImVec2 u = tip - prev;
      const float l = std::sqrt(u.x * u.x + u.y * u.y);
      if (l <= 0) continue;
      u = u / l;
      if (d.cap == 1) {
        dl->AddCircleFilled(tip, w * 0.5f, col);
      } else {
        const ImVec2 n(-u.y, u.x);
        const ImVec2 q[4] = {tip + n * (w * 0.5f), tip + n * (w * 0.5f) + u * (w * 0.5f), tip - n * (w * 0.5f) + u * (w * 0.5f), tip - n * (w * 0.5f)};
        dl->AddConvexPolyFilled(q, 4, col);
      }
    }
  }
}

// a series' line in screen points (NaN breaks it): dash, compound, cap and join on each run, arrows at its two ends
void DrawSeriesLine(ImDrawList* dl, const std::vector<ImVec2>& pts, ImU32 col, float w, const model::ChartSeries& d) {
  std::vector<ImVec2> run;
  std::vector<ImVec2> all;
  for (const ImVec2& p : pts) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      StyledRun(dl, run, col, w, d);
      run.clear();
      continue;
    }
    run.push_back(p);
    all.push_back(p);
  }
  StyledRun(dl, run, col, w, d);
  if (all.size() >= 2) {
    LineEndShape(dl, all[1], all[0], d.begin_arrow, col, w);
    LineEndShape(dl, all[all.size() - 2], all.back(), d.end_arrow, col, w);
  }
}

// text turned a quarter left (reading upwards) around its center
void AddTextUp(ImDrawList* dl, ImFont* f, float sz, ImVec2 center, ImU32 col, const char* text) {
  const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, text);
  const int v0 = dl->VtxBuffer.Size;
  dl->AddText(f, sz, ImFloor(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f)), col, text);
  for (int k = v0; k < dl->VtxBuffer.Size; ++k) {
    ImVec2& q = dl->VtxBuffer[k].pos;
    const ImVec2 d = q - center;
    q = ImVec2(center.x + d.y, center.y - d.x);
  }
}

const model::ChartPointFormat* PointFmt(const model::ChartSeries& d, int k) {
  for (const auto& p : d.points) {
    if (p.index == k) return &p;
  }
  return nullptr;
}

bool PointLabelOn(const model::ChartSeries& d, int k) {
  const model::ChartPointFormat* p = PointFmt(d, k);
  return p && p->label >= 0 ? p->label == 1 : d.data_labels;
}

// a data label dragged away from its place: the offset in screen pixels (the series' labels dragged together, then
// this one label dragged on its own)
ImVec2 LabelOffset(const model::ChartSeries& d, int pt, const ImRect& frame) {
  float dx = 0, dy = 0;
  model::DataLabelOffset(d, pt, &dx, &dy);  // (the printed page uses the same)
  return ImVec2(dx * frame.GetWidth(), dy * frame.GetHeight());
}

// one data label's own options (Format Data Label) or its series'
const model::ChartFont& LabelFontOf(const model::ChartSeries& d, int pt) {
  const model::ChartPointFormat* pf = pt >= 0 ? PointFmt(d, pt) : nullptr;
  return pf && pf->label_font_set ? pf->label_font : d.label_font;
}
int LabelShowOf(const model::ChartSeries& d, int pt) {
  const model::ChartPointFormat* pf = pt >= 0 ? PointFmt(d, pt) : nullptr;
  return pf && pf->label_show >= 0 ? pf->label_show : d.label_show;
}
int LabelPosOf(const model::ChartSeries& d, int pt) {
  const model::ChartPointFormat* pf = pt >= 0 ? PointFmt(d, pt) : nullptr;
  return pf && pf->label_pos >= 0 ? pf->label_pos : d.label_pos;
}
// a point's own format record (added when it has none yet)
model::ChartPointFormat& PointFormatRef(model::ChartSeries& se, int point) {
  auto it = std::find_if(se.points.begin(), se.points.end(), [&](const model::ChartPointFormat& q) { return q.index == point; });
  if (it != se.points.end()) return *it;
  model::ChartPointFormat q;
  q.index = point;
  se.points.push_back(q);
  return se.points.back();
}

// the series whose categories are the legend's entries (pies, doughnuts, one series colored by point); -1: none
int CategoryEntrySeries(const ChartObj& ch) {
  if (ch.type == ChartType::Pie || ch.type == ChartType::Doughnut || ch.type == ChartType::Pie3D) {
    for (size_t i = 0; i < ch.series.size(); ++i) {
      if (!ch.series[i].filtered) return static_cast<int>(i);
    }
    return -1;
  }
  return VaryByPoint(ch);
}

// a label typed or linked to a cell shows that text instead of its contents
const std::string& LabelOwnText(const engine::ChartValues& v, size_t series, int pt, const std::string& contents) {
  if (pt < 0 || v.point_labels.empty()) return contents;
  const auto it = v.point_labels.find((static_cast<uint64_t>(series) << 32) | static_cast<uint32_t>(pt));
  return it != v.point_labels.end() ? it->second : contents;
}

ImRect LabelRectAt(const model::ChartSeries& d, int pt, ImVec2 tp, ImVec2 ts, const ImRect& frame) {
  const ImVec2 p = ImFloor(tp + LabelOffset(d, pt, frame));
  return ImRect(p, p + ts);
}

// a data label in the current font: its legend key, its text (underlined if asked), and a leader line back to its
// point once it was dragged away (Show Leader Lines)
void DrawLabelAt(ImDrawList* dl, const model::ChartSeries& d, int pt, const std::string& t, ImVec2 tp, ImVec2 ts, float key, ImU32 col, ImU32 key_col, const ImRect& anchor, const ImRect& frame) {
  const ImVec2 off = LabelOffset(d, pt, frame);
  const ImVec2 p = ImFloor(tp + off);
  if (d.leader_lines && std::fabs(off.x) + std::fabs(off.y) > ts.y) {
    const ImVec2 a = ImClamp(ImRect(p, p + ts).GetCenter(), anchor.Min, anchor.Max);
    const ImVec2 b = ImClamp(a, p, p + ts);
    dl->AddLine(a, b, IM_COL32(0xA6, 0xA6, 0xA6, 255), 1.0f);
  }
  {  // Fill & Line: the label's box (its own, else its series')
    const model::ChartPointFormat* pf = pt >= 0 ? PointFmt(d, pt) : nullptr;
    const uint32_t fill = pf && pf->label_fill ? pf->label_fill : d.label_fill, line = pf && pf->label_line ? pf->label_line : d.label_line;
    const ImVec2 m(S(2), S(1));
    if (fill) dl->AddRectFilled(p - m, p + ts + m, ArgbCol(fill));
    if (line) dl->AddRect(p - m, p + ts + m, ArgbCol(line));
  }
  if (key > 0) {
    const float k = key - S(3);
    dl->AddRectFilled(ImVec2(p.x, p.y + (ts.y - k) * 0.5f), ImVec2(p.x + k, p.y + (ts.y + k) * 0.5f), key_col);
  }
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + key, p.y), col, t.c_str());
  ChartUnderline(dl, LabelFontOf(d, pt), ImVec2(p.x + key, p.y), ImVec2(ts.x - key, ts.y), col);
}

// a data label's text: Value From Cells, Series Name, Category Name, Value, Percentage (Excel's order)
std::string LabelText(const model::ChartSeries& d, const SeriesBuf& b, const engine::ChartValues& v, int k, double value, double pct) {
  std::string out;
  auto add = [&](const std::string& t) {
    if (!out.empty()) out += d.label_sep.empty() ? std::string(", ") : d.label_sep;
    out += t;
  };
  const int show = LabelShowOf(d, k);
  if ((show & 32) && k >= 0 && static_cast<size_t>(k) < b.label_texts.size()) add(b.label_texts[static_cast<size_t>(k)]);
  if (show & 1) add(b.name);
  if (show & 2) add(k >= 0 && static_cast<size_t>(k) < v.cats.size() ? v.cats[static_cast<size_t>(k)] : std::to_string(k + 1));
  if ((show & 4) && std::isfinite(value)) add(engine::FormatChartNumber(value, d.label_format.empty() ? b.fmt : d.label_format));
  if ((show & 8) && pct >= 0) add(engine::FormatChartNumber(pct, "0%"));
  return out;
}

// pies and doughnuts, wedge by wedge: each slice's own color, explosion and label (Format Data Point); a doughnut has a
// ring per series, the first innermost
void DrawPies(ImDrawList* dl, ChartCache& cc, const ChartObj& ch, ImVec2 pos, ImVec2 size, const std::function<ImU32(size_t, int)>& slice_col) {
  auto& hit = cc.hit;
  const auto& series = cc.v.series;
  const bool dough = ch.type == ChartType::Doughnut;
  std::vector<size_t> rings;
  for (size_t i = 0; i < series.size(); ++i) {
    if (i < ch.series.size() && ch.series[i].filtered) continue;
    if (series[i].N() <= 0) continue;
    rings.push_back(i);
    if (!dough) break;
  }
  if (rings.empty()) return;
  float maxe = 0;
  for (size_t i : rings) {
    if (i >= ch.series.size()) continue;
    maxe = std::max(maxe, static_cast<float>(ch.series[i].explosion));
    for (const auto& p : ch.series[i].points) maxe = std::max(maxe, static_cast<float>(p.explosion));
  }
  maxe = std::clamp(maxe, 0.0f, 400.0f);
  const ImVec2 c = pos + size * 0.5f;
  const float R = std::min(size.x, size.y) * 0.45f / (1.0f + maxe / 100.0f);
  const float hole = dough ? std::clamp(ch.hole_size, 10, 90) / 100.0f : 0.0f;
  hit.pie_c = c;
  hit.pie_r = R * (1.0f + maxe / 100.0f);
  hit.pie_r0 = R * hole;
  constexpr float kTau = 6.28318530718f;
  const float a_start = static_cast<float>(std::clamp(ch.first_slice_angle, 0, 360)) * kTau / 360.0f;
  auto at = [](ImVec2 o, float r, float a) { return ImVec2(o.x + r * std::sin(a), o.y - r * std::cos(a)); };
  std::vector<ImVec2> poly;
  for (size_t ri = 0; ri < rings.size(); ++ri) {
    const size_t i = rings[ri];
    const SeriesBuf& b = series[i];
    static const model::ChartSeries kNone;
    const model::ChartSeries& d = i < ch.series.size() ? ch.series[i] : kNone;
    const int n = std::min(b.N(), 255);
    double total = 0;
    for (int k = 0; k < n; ++k) {
      if (std::isfinite(b.Y()[k])) total += std::fabs(b.Y()[k]);  // a negative value takes its size, as in Excel
    }
    if (!(total > 0)) continue;
    const float r_in = dough ? R * (hole + (1 - hole) * static_cast<float>(ri) / static_cast<float>(rings.size())) : 0.0f;
    const float r_out = dough ? R * (hole + (1 - hole) * static_cast<float>(ri + 1) / static_cast<float>(rings.size())) : R;
    float a = a_start;
    for (int k = 0; k < n; ++k) {
      const double y = b.Y()[k];
      const double val = std::isfinite(y) ? std::fabs(y) : 0.0;
      if (!(val > 0)) continue;
      const float sweep = static_cast<float>(val / total) * kTau;
      const model::ChartPointFormat* pf = PointFmt(d, k);
      const int e = pf && pf->explosion >= 0 ? pf->explosion : d.explosion;
      const float mid = a + sweep * 0.5f;
      const ImVec2 o = (ri + 1 == rings.size()) ? at(c, R * static_cast<float>(std::clamp(e, 0, 400)) / 100.0f, mid) : c;
      const int segs = std::max(2, static_cast<int>(sweep / kTau * 120));
      poly.clear();
      if (r_in > 0) {
        for (int q = 0; q <= segs; ++q) poly.push_back(at(o, r_out, a + sweep * static_cast<float>(q) / static_cast<float>(segs)));
        for (int q = segs; q >= 0; --q) poly.push_back(at(o, r_in, a + sweep * static_cast<float>(q) / static_cast<float>(segs)));
      } else {
        poly.push_back(o);
        for (int q = 0; q <= segs; ++q) poly.push_back(at(o, r_out, a + sweep * static_cast<float>(q) / static_cast<float>(segs)));
      }
      if (!d.no_fill) {
        if (pf && pf->fill) dl->AddConcavePolyFilled(poly.data(), static_cast<int>(poly.size()), WithTransparency(slice_col(i, k), d.transparency));
        else SeriesFillPoly(dl, poly, d, slice_col(i, k), false);  // solid, gradient, picture, pattern
      }
      if (!d.no_line) {
        const ImU32 border = pf && pf->line ? ExcelColor(pf->line) : d.line_color ? ExcelColor(d.line_color) : IM_COL32_WHITE;
        const float bw = d.line_weight > 0 ? d.line_weight * kPtPx : 1.5f;
        if (d.dash != 0) {
          std::vector<ImVec2> closed = poly;
          closed.push_back(poly[0]);
          DashedPolyline(dl, closed, border, bw, d.dash);
        } else {
          dl->AddPolyline(poly.data(), static_cast<int>(poly.size()), border, bw, ImDrawFlags_Closed);
        }
      }
      hit.slices.push_back({static_cast<int>(i), k, o, r_in, r_out, a, a + sweep});
      if (PointLabelOn(d, k)) {
        const std::string contents = LabelText(d, b, cc.v, k, y, val / total);
        const std::string& t = LabelOwnText(cc.v, i, k, contents);
        if (!t.empty()) {
          const model::ChartFont& lf = LabelFontOf(d, k);
          const int lpos = LabelPosOf(d, k);
          ImGui::PushFont(ChartFontOf(lf), ImGui::GetFontSize() * (lf.size > 0 ? lf.size / 9.0f : 1.0f));
          const float key = (LabelShowOf(d, k) & 16) ? ImGui::GetFontSize() * 0.6f + S(3) : 0.0f;
          const ImVec2 ts = ImGui::CalcTextSize(t.c_str()) + ImVec2(key, 0);
          ImVec2 lc;
          switch (lpos) {
            case 0: lc = at(o, dough ? (r_in + r_out) * 0.5f : r_out * 0.5f, mid); break;                        // center
            case 1: lc = at(o, r_out - std::max(ts.x, ts.y) * 0.6f, mid); break;                              // inside end
            case 3: lc = at(o, r_out + S(4) + std::max(ts.x, ts.y) * 0.5f, mid); break;                         // outside end
            default: lc = at(o, dough ? (r_in + r_out) * 0.5f : r_out * 0.65f, mid); break;                       // best fit
          }
          const ImVec2 tp = ImFloor(lc - ts * 0.5f);
          const bool outside = lpos == 3;
          ImU32 tcol = outside ? IM_COL32(0x40, 0x40, 0x40, 255) : (lpos < 0 && !dough ? IM_COL32_WHITE : IM_COL32(0x40, 0x40, 0x40, 255));
          if (lf.color) tcol = ArgbCol(lf.color);
          const ImVec2 edge = at(o, r_out, mid);
          DrawLabelAt(dl, d, k, t, tp, ts, key, tcol, slice_col(i, k), ImRect(edge, edge), hit.frame);
          if (i < hit.labels.size()) {
            hit.labels[i].push_back(LabelRectAt(d, k, tp, ts, hit.frame));
            hit.label_idx[i].push_back(k);
            if (i < hit.label_texts.size()) hit.label_texts[i].push_back(t);
            ui::NoteUiItem(t.c_str(), hit.labels[i].back());
          }
          ImGui::PopFont();
        }
      }
      a += sweep;
    }
  }
}

}  // namespace

bool VolumeStock(ChartType t) { return t == ChartType::StockVHLC || t == ChartType::StockVOHLC; }

using engine::DatePlan;
using engine::DateUnit;
using engine::PlanDateAxis;
using engine::UnitSerial;

// the value axis a series is drawn on: a volume stock chart's volume on the primary axis, its prices on the secondary
int StockAxisOf(ChartType t, size_t i, int axis) { return VolumeStock(t) ? (i == 0 ? 0 : 1) : axis; }

// Excel's four stock charts: volume columns (the prices then on the secondary axis), up / down bars between the open
// and the close, high-low lines, the close (and the open) as ticks without up / down bars; each one a chart element
void DrawStockBody(ImDrawList* pdl, ChartCache& cc, const ChartObj& ch, const DatePlan& dp) {
  auto& hit = cc.hit;
  const auto& series = cc.v.series;
  const size_t ns = series.size();
  const ChartType ct = ch.type;
  const bool vol = VolumeStock(ct);
  const bool ohlc = ct == ChartType::StockOHLC || ct == ChartType::StockVOHLC;
  const size_t off = vol ? 1 : 0;
  if (ns < off + (ohlc ? 4 : 3)) return;
  const SeriesBuf* o = ohlc ? &series[off] : nullptr;
  const SeriesBuf& hi = series[off + (ohlc ? 1 : 0)];
  const SeriesBuf& lo = series[off + (ohlc ? 2 : 1)];
  const SeriesBuf& cl = series[off + (ohlc ? 3 : 2)];
  int cnt = std::min({hi.N(), lo.N(), cl.N()});
  if (o) cnt = std::min(cnt, o->N());
  // each day's place: one per row, or its date on a date axis (Excel's Automatic: weekends as gaps)
  auto xat = [&](int i) { return dp.on ? (static_cast<size_t>(i) < dp.pos.size() ? dp.pos[static_cast<size_t>(i)] : kNaN) : static_cast<double>(i + 1); };
  std::vector<double> xs(static_cast<size_t>(std::max(0, cnt)));
  for (int i = 0; i < cnt; ++i) xs[static_cast<size_t>(i)] = xat(i);
  ImPlotSpec inv;
  inv.LineColor = ImVec4(0, 0, 0, 0);
  if (vol) {  // the volume: columns from zero on the primary axis
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
    const SeriesBuf& v = series[0];
    const ImU32 vc = v.color ? ExcelColor(v.color) : static_cast<ImU32>(engine::ChartPalette(0));
    const double vw = 0.5 / (1.0 + std::clamp(ch.gap_width, 0, 500) / 100.0);
    const bool skip = !ch.series.empty() && ch.series[0].filtered;
    for (int i = 0; i < v.N() && i < 20000 && !skip; ++i) {
      const double y = v.Y()[i];
      if (!std::isfinite(y)) continue;
      const double vx = xat(i);
      if (!std::isfinite(vx)) continue;
      const ImVec2 a = ImPlot::PlotToPixels(vx - vw, y), b = ImPlot::PlotToPixels(vx + vw, 0.0);
      const ImRect r(ImMin(a, b), ImMax(a, b));
      pdl->AddRectFilled(r.Min, r.Max, vc);
      if (!hit.boxes.empty()) hit.boxes[0].push_back(r);
    }
    if (v.N() > 0 && !dp.on) ImPlot::PlotLine("##vol", v.X(), v.Y(), v.N(), inv);
  }
  ImPlot::SetAxes(ImAxis_X1, vol ? ImAxis_Y2 : ImAxis_Y1);
  // red up, green down: the Taiwanese convention of yc.plot's candles (one convention in the whole app), unless the Up
  // Bars / Down Bars have a fill of their own
  const ImU32 up = ch.up_fill ? ArgbCol(ch.up_fill) : CandleUpColor(PlotTheme::Light);
  const ImU32 down = ch.down_fill ? ArgbCol(ch.down_fill) : CandleDownColor(PlotTheme::Light);
  const bool bars = ohlc && ch.up_down_bars;
  const double half = 0.5 / (1.0 + std::clamp(ch.updown_gap, 0, 500) / 100.0);  // Up / Down Bars' Gap Width
  const ImU32 tick = IM_COL32(0x40, 0x40, 0x40, 255);
  for (int i = 0; i < cnt; ++i) {
    const double x = xat(i);
    if (!std::isfinite(x)) continue;
    const double vh = hi.Y()[i], vl = lo.Y()[i], vc = cl.Y()[i], vo = o ? o->Y()[i] : vc;
    if (!std::isfinite(vh) || !std::isfinite(vl) || !std::isfinite(vc) || !std::isfinite(vo)) continue;
    const bool rising = vc >= vo;
    if (ch.hi_lo_lines) {
      const ImU32 lc = ch.hilo_line ? ArgbCol(ch.hilo_line) : bars ? (rising ? up : down) : tick;
      const ImVec2 a = ImPlot::PlotToPixels(x, vh), b = ImPlot::PlotToPixels(x, vl);
      pdl->AddLine(a, b, lc, bars ? 1.0f : 1.5f);
      hit.hilo.push_back(a);
      hit.hilo.push_back(b);
    }
    if (bars) {
      ImVec2 a = ImPlot::PlotToPixels(x - half, std::max(vo, vc)), bb = ImPlot::PlotToPixels(x + half, std::min(vo, vc));
      if (bb.y - a.y < 1.0f) bb.y = a.y + 1.0f;
      pdl->AddRectFilled(a, bb, rising ? up : down);
      (rising ? hit.up_bars : hit.down_bars).emplace_back(a, bb);
    } else {
      const ImVec2 c = ImPlot::PlotToPixels(x, vc);
      pdl->AddLine(c, ImVec2(c.x + S(5), c.y), tick, 1.5f);
      if (o) {
        const ImVec2 q = ImPlot::PlotToPixels(x, vo);
        pdl->AddLine(ImVec2(q.x - S(5), q.y), q, tick, 1.5f);
      }
    }
  }
  if (cnt > 0) {
    ImPlot::PlotLine("##hi", xs.data(), hi.Y(), cnt, inv);
    ImPlot::PlotLine("##lo", xs.data(), lo.Y(), cnt, inv);
  }
  ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
}

// the display unit's name, as Format Axis lists them (a custom unit: "x 250", as Excel 16 labels it, measured)
std::string ChartUnitLabel(double unit) {
  struct U {
    double v;
    const char* name;
  };
  static const U kU[] = {{100, "Hundreds"}, {1000, "Thousands"}, {10000, "10000"}, {100000, "100000"}, {1e6, "Millions"},
                        {1e7, "10000000"}, {1e8, "100000000"}, {1e9, "Billions"}, {1e12, "Trillions"}};
  for (const U& u : kU) {
    if (unit == u.v) return TR(u.name);
  }
  return "x " + engine::FormatChartNumber(unit, "General");
}

void DrawChartBody(AppState& s, uint32_t sheet, const ChartObj& ch, const ImVec2& pos0, const ImVec2& size0, bool allow_input, float font_scale) {
  if (size0.x < 20 || size0.y < 20) return;
  const engine::PaletteScope palette(ch.color_scheme);  // Change Colors
  EnsureColormap();
  ChartCache& cc = CacheFor(s, sheet, ch);
  auto& hit = cc.hit;
  hit = {};
  hit.frame = ImRect(pos0, pos0 + size0);
  ImDrawList* cdl = ImGui::GetWindowDrawList();
  cdl->AddRectFilled(pos0, pos0 + size0, ch.chart_fill ? ArgbCol(ch.chart_fill) : IM_COL32_WHITE);
  if (engine::IsSceneChart(ch.type)) {
    hit.scene = true;
    ImGui::PushFont(ui::F().ui, ui::F().ui_size * font_scale);
    DrawSceneChart(cc, ch, pos0, size0);
    ImGui::PopFont();
    return;
  }
  const auto& series = cc.v.series;
  const size_t ns = series.size();
  hit.lines.assign(ns, {});
  hit.line_idx.assign(ns, {});
  hit.boxes.assign(ns, {});
  hit.labels.assign(ns, {});
  hit.label_idx.assign(ns, {});
  hit.label_texts.assign(ns, {});
  hit.trend.assign(ns, {});
  hit.trend_idx.assign(ns, {});
  hit.errs.assign(ns, {});
  hit.xerrs.assign(ns, {});
  // the title and the legend are laid out here; ImPlot draws the plot area and the axes inside what is left
  ImVec2 pos = pos0, size = size0;
  const float pad = S(6) * font_scale;
  auto frac_pos = [&](const ChartObj::ManualRect& m) { return ImVec2(pos0.x + m.x * size0.x, pos0.y + m.y * size0.y); };
  const std::string& title = cc.v.title;
  if (ch.has_title && !title.empty()) {
    ImGui::PushFont(ChartFontOf(ch.title_font), ChartFontSize(ch.title_font, 14, font_scale));
    const ImVec2 ts = ImGui::CalcTextSize(title.c_str(), nullptr, false, size0.x - pad * 2);
    ImVec2 tp(pos0.x + std::max(pad, (size0.x - ts.x) * 0.5f), pos0.y + pad);
    if (ch.title_rect.on) tp = ImClamp(frac_pos(ch.title_rect), pos0, ImMax(pos0, pos0 + size0 - ts));
    if (ch.title_fill) cdl->AddRectFilled(tp - ImVec2(S(3), S(1)), tp + ts + ImVec2(S(3), S(1)), ArgbCol(ch.title_fill));
    if (ch.title_line) cdl->AddRect(tp - ImVec2(S(3), S(1)), tp + ts + ImVec2(S(3), S(1)), ArgbCol(ch.title_line));
    cdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), tp, ChartTextCol(ch.title_font), title.c_str(), nullptr, size0.x - pad * 2);
    ChartUnderline(cdl, ch.title_font, tp, ts, ChartTextCol(ch.title_font));
    ImGui::PopFont();
    hit.title = ImRect(tp, tp + ts);
    if (!ch.title_overlay && !ch.title_rect.on) {
      pos.y += ts.y + pad;
      size.y -= ts.y + pad;
    }
  }
  const ChartType ct = ch.type;
  const bool pie = ct == ChartType::Pie || ct == ChartType::Doughnut;
  const bool horizontal = engine::IsHorizontalBar(ct);
  auto color_of = [&](size_t i, const SeriesBuf& b) { return b.color ? ExcelColor(b.color) : static_cast<ImU32>(engine::ChartPalette(i)); };
  // a pie slice's color: its own fill, else one per slice (Vary colors by slice) or the series'
  auto slice_col = [&](size_t i, int k) -> ImU32 {
    if (i < ch.series.size()) {
      const model::ChartSeries& d = ch.series[i];
      if (const model::ChartPointFormat* pf = PointFmt(d, k); pf && pf->fill) return ExcelColor(pf->fill);
      if (!d.vary_colors) return d.color ? ExcelColor(d.color) : static_cast<ImU32>(engine::ChartPalette(i));
    }
    return static_cast<ImU32>(engine::ChartPalette(static_cast<size_t>(k)));
  };
  // legend entries: series (pie and doughnut: categories) and trendlines
  std::vector<LegendEntry> legend;
  if (ch.has_legend) {
    if (pie) {
      size_t first = 0;
      while (first < ns && first < ch.series.size() && ch.series[first].filtered) ++first;
      const int n = first < ns ? std::min(series[first].N(), 64) : 0;
      for (int i = 0; i < n; ++i) {
        if (first < ch.series.size()) {
          if (const model::ChartPointFormat* pf = PointFmt(ch.series[first], i); pf && pf->legend_hidden) continue;  // its entry deleted
        }
        legend.push_back({static_cast<size_t>(i) < cc.v.cats.size() ? cc.v.cats[static_cast<size_t>(i)] : std::to_string(i + 1), slice_col(first, i), 0, static_cast<int>(first), false,
                          nullptr, i});
      }
    } else if (VaryByPoint(ch) >= 0) {  // one series colored by point: its categories (Excel)
      const size_t vi = static_cast<size_t>(VaryByPoint(ch));
      const int n = vi < ns ? std::min(series[vi].N(), 64) : 0;
      for (int k = 0; k < n; ++k) {
        legend.push_back({static_cast<size_t>(k) < cc.v.cats.size() ? cc.v.cats[static_cast<size_t>(k)] : std::to_string(k + 1), static_cast<ImU32>(engine::ChartPalette(static_cast<size_t>(k))), 0, static_cast<int>(vi), false});
      }
    } else if (!engine::IsStockType(ct)) {  // (no legend for the price series)
      for (size_t i = 0; i < ns; ++i) {
        if (i < ch.series.size() && (ch.series[i].filtered || ch.series[i].legend_hidden)) continue;  // Chart Filters, a deleted entry
        const auto& b = series[i];
        const ChartType t = engine::EffectiveSeriesType(ch, i);
        const bool marker = engine::HasMarkers(t) || (i < ch.series.size() && ch.series[i].marker > 0);
        const int kind = engine::IsBarType(t) || engine::IsAreaType(t) || t == ChartType::Bubble ? 0 : marker ? 2 : 1;
        legend.push_back({b.name, color_of(i, b), kind, static_cast<int>(i)});
      }
      const size_t nl = legend.size();
      for (size_t k = 0; k < nl; ++k) {
        const size_t i = static_cast<size_t>(legend[k].series);
        if (i < ch.series.size()) {
          for (const auto& t : TrendlinesOf(ch.series[i])) legend.push_back({TrendLegendName(t, series[i].name), legend[k].col, 1, static_cast<int>(i), false});
        }
      }
    }
  }
  // entries with fonts of their own: a series' entry, or a category's (pies, vary colors by point)
  const bool cat_entries = pie || VaryByPoint(ch) >= 0;
  for (size_t k = 0; k < legend.size(); ++k) {
    LegendEntry& e = legend[k];
    if (e.series < 0 || static_cast<size_t>(e.series) >= ch.series.size()) continue;
    const model::ChartSeries& d = ch.series[static_cast<size_t>(e.series)];
    if (e.own) {
      if (d.legend_entry_font_set) e.font = &d.legend_entry_font;
    } else if (cat_entries) {
      const int cat = e.cat >= 0 ? e.cat : static_cast<int>(k);
      if (const model::ChartPointFormat* pf = PointFmt(d, cat); pf && pf->legend_font_set) e.font = &pf->legend_font;
    }
  }
  ImFont* lfont = ChartFontOf(ch.legend_font);
  const float lsize = ChartFontSize(ch.legend_font, 9, font_scale);
  std::vector<ImRect> lrects;  // entry rectangles
  if (!legend.empty()) {
    ImVec2 lsz(0, 0);
    ImGui::PushFont(lfont, lsize);
    const float fh = ImGui::GetFontSize(), key = fh * 0.75f, gap = S(4) * font_scale, sp = S(10) * font_scale;
    const bool sized = ch.legend_rect.on && ch.legend_rect.w > 0 && ch.legend_rect.h > 0;  // resized by hand
    const bool vertical = !sized && (ch.legend_pos == 1 || ch.legend_pos == 3 || ch.legend_pos == 4);
    const float max_w = sized ? ch.legend_rect.w * size0.x : vertical ? size.x * 0.4f : size.x - pad * 2;
    float x = 0, y = 0;
    for (const auto& e : legend) {
      const model::ChartFont& ef = e.font ? *e.font : ch.legend_font;
      const float efs = e.font ? ChartFontSize(ef, 9, font_scale) : fh;
      const float eh = std::max(fh, efs);
      const float tw = e.font ? ChartFontOf(ef)->CalcTextSizeA(efs, FLT_MAX, 0.0f, e.text.c_str()).x : ImGui::CalcTextSize(e.text.c_str()).x;
      const float w = std::min(max_w, key + gap + tw + S(2));
      if (!vertical && x > 0 && x + w > max_w) {
        x = 0;
        y += fh + S(2) * font_scale;
      }
      lrects.emplace_back(ImVec2(x, y), ImVec2(x + w, y + eh));
      lsz.x = std::max(lsz.x, x + w);
      lsz.y = std::max(lsz.y, y + eh);
      if (vertical) y += eh + S(3) * font_scale;
      else x += w + sp;
    }
    ImGui::PopFont();
    // room for the legend (unless it overlays the plot or was placed by hand), then its corner
    const bool room = !ch.legend_overlay && !ch.legend_rect.on;
    ImVec2 p2 = pos, s2 = size;
    ImVec2 lp;
    switch (ch.legend_pos) {
      case 1:  // right
        s2.x -= lsz.x + pad * 2;
        lp = ImVec2(p2.x + s2.x + pad, p2.y + (s2.y - lsz.y) * 0.5f);
        break;
      case 2:  // top
        lp = ImVec2(p2.x + (s2.x - lsz.x) * 0.5f, p2.y + S(2) * font_scale);
        p2.y += lsz.y + pad;
        s2.y -= lsz.y + pad;
        break;
      case 3:  // left
        lp = ImVec2(p2.x + pad, p2.y + (s2.y - lsz.y) * 0.5f);
        p2.x += lsz.x + pad * 2;
        s2.x -= lsz.x + pad * 2;
        break;
      case 4:  // top right corner
        s2.x -= lsz.x + pad * 2;
        lp = ImVec2(p2.x + s2.x + pad, p2.y + pad);
        break;
      default:  // bottom
        s2.y -= lsz.y + pad;
        lp = ImVec2(p2.x + (s2.x - lsz.x) * 0.5f, p2.y + s2.y + pad * 0.5f);
        break;
    }
    if (sized) lsz = ImVec2(ch.legend_rect.w * size0.x, std::max(lsz.y, ch.legend_rect.h * size0.y));
    if (ch.legend_rect.on) lp = ImClamp(frac_pos(ch.legend_rect), pos0, ImMax(pos0, pos0 + size0 - lsz));
    if (room) pos = p2, size = s2;
    hit.legend = ImRect(lp, lp + lsz);
    for (auto& r : lrects) r.Translate(lp);
  }
  // the axis titles, drawn here so they can be clicked and typed in place: the category axis' under the plot (bars:
  // at the left, turned), the value axis' at the left turned, the secondary axis' at the right
  bool y2 = false;
  for (size_t i = 0; i < ns; ++i) y2 = y2 || (series[i].axis == 1 && !(i < ch.series.size() && ch.series[i].filtered));
  y2 = y2 && !pie && !horizontal;
  if (VolumeStock(ct) && ns >= 4) y2 = true;  // a volume stock chart: the prices on the secondary axis
  struct AxisTitle {
    const std::string* text = nullptr;
    int side = 0;  // 0 bottom, 1 left (turned), 2 right (turned)
    ImRect* out = nullptr;
    const model::ChartFont* font = nullptr;
  };
  std::vector<AxisTitle> atitles;
  if (!pie) {
    if (ch.cat_axis.has_title && !cc.v.cat_title.empty()) atitles.push_back({&cc.v.cat_title, horizontal ? 1 : 0, &hit.cat_title, &AxisTitleFontOf(ch, ch.cat_axis)});
    if (ch.val_axis.has_title && !cc.v.val_title.empty()) atitles.push_back({&cc.v.val_title, horizontal ? 0 : 1, &hit.val_title, &AxisTitleFontOf(ch, ch.val_axis)});
    if (y2 && ch.val_axis2.has_title && !cc.v.val2_title.empty()) atitles.push_back({&cc.v.val2_title, 2, &hit.val2_title, &AxisTitleFontOf(ch, ch.val_axis2)});
  }
  for (const AxisTitle& t : atitles) {
    ImGui::PushFont(ChartFontOf(*t.font), ChartFontSize(*t.font, 10, font_scale));
    ImFont* f = ImGui::GetFont();
    const float fsz = ImGui::GetFontSize();
    const ImU32 col = ChartTextCol(*t.font);
    const float th = fsz * 1.35f;
    const ChartObj::ManualRect& man = t.out == &hit.cat_title ? ch.cat_title_rect : t.out == &hit.val_title ? ch.val_title_rect : ch.val2_title_rect;
    if (man.on) {  // dragged to a place of its own: drawn there, the plot area keeps its room
      const ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0.0f, t.text->c_str());
      const ImVec2 bs = t.side == 0 ? ts : ImVec2(ts.y, ts.x);
      const ImVec2 tl = ImClamp(frac_pos(man), pos0, ImMax(pos0, pos0 + size0 - bs));
      if (t.side == 0) {
        cdl->AddText(f, fsz, ImFloor(tl), col, t.text->c_str());
        ChartUnderline(cdl, *t.font, ImFloor(tl), ts, col);
      } else {
        AddTextUp(cdl, f, fsz, tl + bs * 0.5f, col, t.text->c_str());
      }
      *t.out = ImRect(tl, tl + bs);
    } else {
      const ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0.0f, t.text->c_str());
      if (t.side == 0) {
        const ImVec2 tp(pos.x + (size.x - ts.x) * 0.5f, pos.y + size.y - th + (th - ts.y) * 0.5f);
        cdl->AddText(f, fsz, ImFloor(tp), col, t.text->c_str());
        ChartUnderline(cdl, *t.font, ImFloor(tp), ts, col);
        *t.out = ImRect(tp, tp + ts);
        size.y -= th;
      } else {
        const float cx = t.side == 1 ? pos.x + th * 0.5f : pos.x + size.x - th * 0.5f;
        const ImVec2 c(cx, pos.y + size.y * 0.5f);
        AddTextUp(cdl, f, fsz, c, col, t.text->c_str());
        *t.out = ImRect(c - ImVec2(ts.y, ts.x) * 0.5f, c + ImVec2(ts.y, ts.x) * 0.5f);
        if (t.side == 1) pos.x += th;
        size.x -= th;
      }
    }
    ImGui::PopFont();
  }
  if (ch.plot_rect.on) {  // the plot area placed by hand
    pos = ImClamp(frac_pos(ch.plot_rect), pos0, pos0 + size0 - ImVec2(20, 20));
    size = ImMin(ImVec2(ch.plot_rect.w * size0.x, ch.plot_rect.h * size0.y), pos0 + size0 - pos);
  }
  auto draw_legend = [&]() {
    if (legend.empty()) return;
    if (ch.legend_fill) cdl->AddRectFilled(hit.legend.Min - ImVec2(S(3), S(2)), hit.legend.Max + ImVec2(S(3), S(2)), ArgbCol(ch.legend_fill));
    if (ch.legend_line) cdl->AddRect(hit.legend.Min - ImVec2(S(3), S(2)), hit.legend.Max + ImVec2(S(3), S(2)), ArgbCol(ch.legend_line));
    ImGui::PushFont(lfont, lsize);
    const float fh = ImGui::GetFontSize(), key = fh * 0.75f, gap = S(4) * font_scale;
    const ImU32 tc = ChartTextCol(ch.legend_font);
    for (size_t k = 0; k < legend.size() && k < lrects.size(); ++k) {
      const LegendEntry& e = legend[k];
      const ImRect& r = lrects[k];
      const float cy = r.GetCenter().y;
      if (e.kind == 0) {
        cdl->AddRectFilled(ImVec2(r.Min.x, cy - key * 0.5f), ImVec2(r.Min.x + key, cy + key * 0.5f), e.col);
      } else {
        cdl->AddLine(ImVec2(r.Min.x, cy), ImVec2(r.Min.x + key, cy), e.col, std::max(1.5f, S(2)));
        if (e.kind == 2) cdl->AddCircleFilled(ImVec2(r.Min.x + key * 0.5f, cy), key * 0.25f, e.col);
      }
      cdl->PushClipRect(r.Min, r.Max, true);
      const model::ChartFont& ef = e.font ? *e.font : ch.legend_font;
      ImFont* efont = e.font ? ChartFontOf(ef) : ImGui::GetFont();
      const float efs = e.font ? ChartFontSize(ef, 9, font_scale) : ImGui::GetFontSize();
      const ImU32 etc = e.font ? ChartTextCol(ef) : tc;
      const ImVec2 ets = efont->CalcTextSizeA(efs, FLT_MAX, 0.0f, e.text.c_str());
      const ImVec2 etp(r.Min.x + key + gap, r.Min.y + (r.GetHeight() - ets.y) * 0.5f);
      cdl->AddText(efont, efs, etp, etc, e.text.c_str());
      ChartUnderline(cdl, ef, etp, ets, etc);
      cdl->PopClipRect();
      hit.legend_rects.push_back(r);
      hit.legend_series.push_back(e.own ? e.series : -1);
      hit.legend_cats.push_back(e.cat >= 0 ? e.cat : static_cast<int>(k));
      ui::NoteUiItem(e.text.c_str(), r);  // the UI item recorder sees each entry, as a person does
    }
    ImGui::PopFont();
  };
  if (size.x < 20 || size.y < 20) {
    draw_legend();
    return;
  }
  if (pie) {
    hit.plot = hit.plot_frame = ImRect(pos, pos + size);
    if (ch.plot_fill) cdl->AddRectFilled(pos, pos + size, ArgbCol(ch.plot_fill));
    if (ch.plot_line) cdl->AddRect(pos, pos + size, ArgbCol(ch.plot_line));
    ImGui::PushFont(ChartFontOf(ch.axis_font), ChartFontSize(ch.axis_font, 9, font_scale));
    cdl->PushClipRect(pos0, pos0 + size0, true);
    DrawPies(cdl, cc, ch, pos, size, slice_col);
    cdl->PopClipRect();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);
    draw_legend();
    return;
  }
  const bool interactive = allow_input && ch.interactive;
  ImPlot::PushColormap(EnsureColormap());
  ImPlot::PushStyleColor(ImPlotCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImPlot::PushStyleColor(ImPlotCol_PlotBg, ch.plot_fill ? ArgbCol(ch.plot_fill) : IM_COL32(0, 0, 0, 0));
  ImPlot::PushStyleColor(ImPlotCol_PlotBorder, ch.plot_line ? ArgbCol(ch.plot_line) : IM_COL32(0, 0, 0, 0));
  ImPlot::PushStyleColor(ImPlotCol_LegendBg, IM_COL32(255, 255, 255, 0));
  ImPlot::PushStyleColor(ImPlotCol_LegendBorder, IM_COL32(0, 0, 0, 0));
  ImPlot::PushStyleColor(ImPlotCol_LegendText, IM_COL32(0x59, 0x59, 0x59, 255));
  ImPlot::PushStyleColor(ImPlotCol_TitleText, IM_COL32(0x59, 0x59, 0x59, 255));
  // the tick labels: ImPlot draws the value axis' in its font; another axis in a font of its own is drawn here
  const model::ChartFont& vfont = AxisFontOf(ch, ch.val_axis);
  ImPlot::PushStyleColor(ImPlotCol_AxisText, ChartTextCol(vfont));
  ImPlot::PushStyleColor(ImPlotCol_AxisGrid, IM_COL32(0xD9, 0xD9, 0xD9, 255));
  ImPlot::PushStyleColor(ImPlotCol_AxisTick, IM_COL32(0xBF, 0xBF, 0xBF, 255));
  ImPlot::PushStyleColor(ImPlotCol_InlayText, IM_COL32(0x40, 0x40, 0x40, 255));
  ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(S(10), S(8)));
  {  // Label distance from axis (Excel's 100: ImPlot's own padding)
    const float cat_off = std::clamp(ch.cat_axis.label_offset, 0, 1000) / 100.0f, val_off = std::clamp(ch.val_axis.label_offset, 0, 1000) / 100.0f;
    const bool hz = engine::IsHorizontalBar(ch.type);
    ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(5.0f * (hz ? cat_off : val_off), 5.0f * (hz ? val_off : cat_off)));
  }
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x40, 0x40, 0x40, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
  ImGui::PushFont(ChartFontOf(vfont), ChartFontSize(vfont, 9, font_scale));
  ImGui::SetCursorScreenPos(pos);

  ImPlotFlags flags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend;
  if (!interactive) flags |= ImPlotFlags_NoInputs;
  const std::string plot_id = "##ycchart" + std::to_string(ch.id);
  if (ImPlot::BeginPlot(plot_id.c_str(), size, flags)) {
    // ---- what each series is drawn as ----
    std::vector<ChartType> types(ns);
    for (size_t i = 0; i < ns; ++i) types[i] = engine::EffectiveSeriesType(ch, i);
    const size_t nb = cc.bar_idx.size();
    const int groups = nb ? static_cast<int>(cc.bars.size() / nb) : 0;
    const bool barlike = horizontal || nb > 0;
    const bool bar_stacked = nb && engine::IsStackedType(types[cc.bar_idx[0]]);
    const bool bar_pct = nb && engine::IsPercentType(types[cc.bar_idx[0]]);
    const bool scatter = engine::IsScatterType(ct) && nb == 0;
    const bool stock = engine::IsStockType(ct);
    const double bar_base = ch.val_axis.crosses == 1 ? ch.val_axis.crosses_at : 0.0;
    auto filtered = [&](size_t i) { return i < ch.series.size() && ch.series[i].filtered; };
    // the bars' extents [series x category] (stacked: on top of each other; 100%: shares of the category's total)
    std::vector<double> blo(cc.bars.size(), 0.0), bhi(cc.bars.size(), 0.0);
    for (int j = 0; j < groups; ++j) {
      double total = 0, up = 0, down = 0;
      for (size_t k = 0; k < nb; ++k) total += std::fabs(cc.bars[k * static_cast<size_t>(groups) + static_cast<size_t>(j)]);
      for (size_t k = 0; k < nb; ++k) {
        const size_t at = k * static_cast<size_t>(groups) + static_cast<size_t>(j);
        double v = cc.bars[at];
        if (bar_pct) v = total > 0 ? v / total : 0;
        if (bar_stacked) {
          if (v >= 0) blo[at] = up, bhi[at] = up + v, up += v;
          else bhi[at] = down, blo[at] = down + v, down += v;
        } else {
          blo[at] = std::min(bar_base, v);
          bhi[at] = std::max(bar_base, v);
        }
      }
    }
    // lines and areas as drawn: empty cells as zero (or gaps), stacked series on top of the ones before them
    std::vector<std::vector<double>> dy(ns), dlo(ns);
    {
      std::vector<double> cum_line, cum_area, tot_line, tot_area;
      for (size_t i = 0; i < ns; ++i) {
        if (engine::IsBarType(types[i]) || filtered(i)) continue;
        const int n = series[i].N();
        if (!engine::IsStackedType(types[i]) || !engine::IsPercentType(types[i])) continue;
        auto& tot = engine::IsAreaType(types[i]) ? tot_area : tot_line;
        if (tot.size() < static_cast<size_t>(n)) tot.resize(static_cast<size_t>(n), 0.0);
        for (int k = 0; k < n; ++k) {
          if (std::isfinite(series[i].Y()[k])) tot[static_cast<size_t>(k)] += std::fabs(series[i].Y()[k]);
        }
      }
      for (size_t i = 0; i < ns; ++i) {
        if (engine::IsBarType(types[i])) continue;
        const SeriesBuf& b = series[i];
        const int n = b.N();
        auto& v = dy[i];
        v.assign(b.Y(), b.Y() + std::max(0, n));
        const bool area = engine::IsAreaType(types[i]);
        if (area || ch.display_blanks == 2 || engine::IsStackedType(types[i])) {
          for (size_t k = 0; k < v.size(); ++k) {
            if (!std::isfinite(v[k]) && !std::binary_search(b.joined.begin(), b.joined.end(), static_cast<int>(k))) v[k] = 0;
          }
        }
        if (engine::IsStackedType(types[i])) {
          auto& cum = area ? cum_area : cum_line;
          const auto& tot = area ? tot_area : tot_line;
          if (cum.size() < v.size()) cum.resize(v.size(), 0.0);
          dlo[i].assign(v.size(), 0.0);
          for (size_t k = 0; k < v.size(); ++k) {
            double x = std::isfinite(v[k]) ? v[k] : 0.0;
            if (engine::IsPercentType(types[i])) x = k < tot.size() && tot[k] > 0 ? x / tot[k] : 0.0;
            dlo[i][k] = cum[k];
            cum[k] += x;
            v[k] = cum[k];
          }
        }
      }
    }
    // ---- the value axes' ranges ----
    auto extent = [&](int group, double* lo, double* hi) {
      *lo = 1e300;
      *hi = -1e300;
      for (size_t k = 0; k < nb; ++k) {
        const size_t i = cc.bar_idx[k];
        if (series[i].axis != group || filtered(i)) continue;
        for (int j = 0; j < groups; ++j) {
          const size_t at = k * static_cast<size_t>(groups) + static_cast<size_t>(j);
          *lo = std::min(*lo, blo[at]);
          *hi = std::max(*hi, bhi[at]);
        }
      }
      for (size_t i = 0; i < ns; ++i) {
        if (engine::IsBarType(types[i]) || filtered(i) || (StockAxisOf(ct, i, series[i].axis) != group && (!stock || VolumeStock(ct)))) continue;
        for (double y : dy[i]) {
          if (std::isfinite(y)) *lo = std::min(*lo, y), *hi = std::max(*hi, y);
        }
        for (double y : dlo[i]) {
          if (std::isfinite(y)) *lo = std::min(*lo, y), *hi = std::max(*hi, y);
        }
      }
      return *hi >= *lo;
    };
    bool any_area = false, any_pct = bar_pct;
    for (size_t i = 0; i < ns; ++i) {
      any_area = any_area || engine::IsAreaType(types[i]);
      any_pct = any_pct || engine::IsPercentType(types[i]);
    }
    struct AxisPlan {
      double lo = 0, hi = 1, step = 1;
      std::vector<double> ticks;
      std::vector<std::string> labels;
    };
    // round bounds like Excel's, each end automatic or fixed; units, log scale and display units
    auto plan_axis = [&](const model::ChartAxisFormat& a, double dlo0, double dhi0, bool zero, bool pct, const std::string& data_fmt) {
      AxisPlan p;
      double dlo1 = dlo0, dhi1 = dhi0;
      if (!(dhi1 >= dlo1)) dlo1 = 0, dhi1 = 1;
      if (zero) dlo1 = std::min(dlo1, 0.0), dhi1 = std::max(dhi1, 0.0);
      if (a.log) {
        const double base = a.log_base > 1 ? a.log_base : 10;
        const double lb = std::log(base);
        double lo = dlo1 > 0 ? dlo1 : (dhi1 > 0 ? dhi1 / 1000 : 1);
        double hi = dhi1 > lo ? dhi1 : lo * base;
        double k0 = std::floor(std::log(lo) / lb + 1e-9), k1 = std::ceil(std::log(hi) / lb - 1e-9);
        if (k1 <= k0) k1 = k0 + 1;
        p.lo = a.min_auto ? std::pow(base, k0) : std::max(a.min, 1e-300);
        p.hi = a.max_auto ? std::pow(base, k1) : a.max;
        if (!(p.hi > p.lo)) p.hi = p.lo * base;
        for (double k = std::ceil(std::log(p.lo) / lb - 1e-9); k <= std::floor(std::log(p.hi) / lb + 1e-9) && p.ticks.size() < 200; k += 1) p.ticks.push_back(std::pow(base, k));
        p.step = base;
      } else {
        double a0 = 0, b0 = 1, step = 1;
        if (pct) {
          a0 = dlo1 < -1e-12 ? -1 : 0;
          b0 = 1;
          step = 0.1;
        } else {
          NiceAxis(dlo1, dhi1, &a0, &b0, &step);
        }
        p.lo = a.min_auto ? a0 : a.min;
        p.hi = a.max_auto ? b0 : a.max;
        if (a.major > 0 && (p.hi - p.lo) / a.major <= 200) step = a.major;
        if (!(p.hi > p.lo)) p.hi = p.lo + step;
        p.step = step;
        for (double t = std::ceil(p.lo / step - 1e-9) * step; t <= p.hi + step * 1e-6 && p.ticks.size() < 200; t += step) p.ticks.push_back(std::fabs(t) < step * 1e-9 ? 0.0 : t);
      }
      const std::string fmt = !a.number_format.empty() ? a.number_format : pct ? std::string("0%") : data_fmt;
      const double du = a.display_unit > 0 ? a.display_unit : 1;
      for (double t : p.ticks) p.labels.push_back(engine::FormatChartNumber(t / du, fmt));
      return p;
    };
    auto apply_axis = [&](ImAxis ax, const AxisPlan& p, const model::ChartAxisFormat& a) {
      if (a.log) ImPlot::SetupAxisScale(ax, ImPlotScale_Log10);
      ImPlot::SetupAxisLimits(ax, p.lo, p.hi, ImPlotCond_Always);
      std::vector<const char*> lp;
      for (const auto& l : p.labels) lp.push_back(l.c_str());
      if (!p.ticks.empty()) ImPlot::SetupAxisTicks(ax, p.ticks.data(), static_cast<int>(p.ticks.size()), lp.data());
    };
    const ImPlotAxisFlags fit = interactive ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit;
    auto axis_flags = [&](const model::ChartAxisFormat& a, bool fixed) {
      ImPlotAxisFlags f = (fixed ? ImPlotAxisFlags_None : fit) | ImPlotAxisFlags_NoHighlight;
      if (!a.gridlines) f |= ImPlotAxisFlags_NoGridLines;
      if (!a.visible || a.label_pos == 3) f |= ImPlotAxisFlags_NoTickLabels;
      if (!a.visible || a.major_tick == 0) f |= ImPlotAxisFlags_NoTickMarks;
      if (a.reverse) f |= ImPlotAxisFlags_Invert;
      if (a.label_pos == 1) f |= ImPlotAxisFlags_Opposite;
      return f;
    };
    const std::string data_fmt = ns ? series[0].fmt : std::string();
    // the primary value axis
    AxisPlan vplan, v2plan, xplan;
    bool vplan_ok = false, v2plan_ok = false, xplan_ok = false;
    if (!interactive) {
      double lo = 0, hi = 0;
      if (extent(0, &lo, &hi) || nb) {
        vplan = plan_axis(ch.val_axis, lo, hi, nb > 0 || any_area || VolumeStock(ct), any_pct, data_fmt);
        vplan_ok = true;
      }
      if (y2 && extent(1, &lo, &hi)) {
        std::string f2;
        for (size_t i = 0; i < ns; ++i) {
          if (series[i].axis == 1) {
            f2 = series[i].fmt;
            break;
          }
        }
        bool zero2 = false;
        for (size_t i = 0; i < ns; ++i) zero2 = zero2 || (series[i].axis == 1 && (engine::IsBarType(types[i]) || engine::IsAreaType(types[i])));
        v2plan = plan_axis(ch.val_axis2, lo, hi, zero2, false, f2);
        v2plan_ok = true;
      }
      // a scatter chart's X axis is a value axis too
      if (scatter) {
        double xlo = 1e300, xhi = -1e300;
        for (size_t i = 0; i < ns; ++i) {
          if (filtered(i)) continue;
          for (int k = 0; k < series[i].N(); ++k) {
            const double x = series[i].X()[k];
            if (std::isfinite(x)) xlo = std::min(xlo, x), xhi = std::max(xhi, x);
          }
        }
        if (xhi >= xlo) {
          std::string xf;
          xplan = plan_axis(ch.cat_axis, xlo, xhi, false, false, xf);
          xplan_ok = true;
        }
      }
    }
    if (vplan_ok) hit.axis_lo[0] = vplan.lo, hit.axis_hi[0] = vplan.hi, hit.axis_step[0] = vplan.step;
    if (v2plan_ok) hit.axis_lo[1] = v2plan.lo, hit.axis_hi[1] = v2plan.hi, hit.axis_step[1] = v2plan.step;
    if (xplan_ok) hit.axis_lo[2] = xplan.lo, hit.axis_hi[2] = xplan.hi, hit.axis_step[2] = xplan.step;
    // columns and lines on categories: each category takes one slot (a gap before the first and after the last)
    int slots = 0;
    const double slot0 = barlike ? -0.5 : 0.5;  // columns sit at 0..n-1, lines on categories at 1..n
    const bool on_cats = !scatter && !interactive && (barlike || !cc.v.cats.empty() || any_area || engine::IsLineType(ct) || stock);
    if (on_cats) {
      for (size_t i = 0; i < ns; ++i) slots = std::max(slots, series[i].N());
    }
    // a date axis: the categories at their dates, the days (months, years) between them kept
    const DatePlan dplan = on_cats && !horizontal ? PlanDateAxis(ch, cc.v, static_cast<size_t>(slots)) : DatePlan{};
    if (dplan.on) slots = static_cast<int>(dplan.u1 - dplan.u0 + 1);
    double cat_lo = slot0, cat_hi = slot0 + slots;
    if (slots > 0 && (!ch.cat_axis.between || (any_area && !barlike))) {
      cat_lo += 0.5;  // on tick marks: the first and last categories at the edges (areas fill the plot like this)
      cat_hi -= 0.5;
      if (!(cat_hi > cat_lo)) cat_lo -= 0.5, cat_hi += 0.5;
    }
    const ImAxis val_ax = horizontal ? ImAxis_X1 : ImAxis_Y1, cat_ax = horizontal ? ImAxis_Y1 : ImAxis_X1;
    std::vector<std::vector<std::string>> cat_levels;  // Multi-level Category Labels (filled with the tick labels)
    ImPlotAxisFlags vflags = axis_flags(ch.val_axis, vplan_ok), cflags = axis_flags(ch.cat_axis, slots > 0 || xplan_ok);
    if (ch.val_axis.crosses == 2) cflags |= ImPlotAxisFlags_Opposite;  // the horizontal axis crosses at the maximum value
    if (ch.cat_axis.crosses == 2) vflags |= ImPlotAxisFlags_Opposite;  // the vertical axis crosses at the maximum category
    if (horizontal) ImPlot::SetupAxes(nullptr, nullptr, vflags, cflags);
    else ImPlot::SetupAxes(nullptr, nullptr, cflags, vflags);
    if (vplan_ok) {
      apply_axis(val_ax, vplan, ch.val_axis);
    } else {
      if (!ch.val_axis.number_format.empty()) ImPlot::SetupAxisFormat(val_ax, FormatTick, const_cast<std::string*>(&ch.val_axis.number_format));
      if (!ch.val_axis.min_auto && !ch.val_axis.max_auto) ImPlot::SetupAxisLimits(val_ax, ch.val_axis.min, ch.val_axis.max, ImPlotCond_Always);
    }
    if (xplan_ok) {
      apply_axis(ImAxis_X1, xplan, ch.cat_axis);
    } else if (scatter && !ch.cat_axis.min_auto && !ch.cat_axis.max_auto) {
      ImPlot::SetupAxisLimits(ImAxis_X1, ch.cat_axis.min, ch.cat_axis.max, ImPlotCond_Always);
    }
    if (slots > 0) ImPlot::SetupAxisLimits(cat_ax, cat_lo, cat_hi, ImPlotCond_Always);
    if (!ch.cat_axis.number_format.empty() && cc.v.cats.empty() && !barlike && !xplan_ok)
      ImPlot::SetupAxisFormat(cat_ax, FormatTick, const_cast<std::string*>(&ch.cat_axis.number_format));
    if (y2) {
      ImPlot::SetupAxis(ImAxis_Y2, nullptr, axis_flags(ch.val_axis2, v2plan_ok) | ImPlotAxisFlags_Opposite);
      if (v2plan_ok) apply_axis(ImAxis_Y2, v2plan, ch.val_axis2);
      else if (!ch.val_axis2.number_format.empty()) ImPlot::SetupAxisFormat(ImAxis_Y2, FormatTick, const_cast<std::string*>(&ch.val_axis2.number_format));
    }
    // category labels
    size_t n = 0;
    for (const auto& b : series) n = std::max(n, static_cast<size_t>(std::max(0, b.N())));
    if (dplan.on) {
      // a date axis' labels: every major unit (Format Axis > Units > Major, in days, months or years) from the first date
      const int tu = std::max(dplan.unit, ch.major_unit_scale);
      const std::string dfmt = !ch.cat_axis.number_format.empty() ? ch.cat_axis.number_format : !cc.v.cat_fmt.empty() ? cc.v.cat_fmt : std::string("yyyy/m/d");
      const int64_t t0 = DateUnit(UnitSerial(dplan.u0, dplan.unit), tu), t1 = DateUnit(UnitSerial(dplan.u1, dplan.unit), tu);
      int64_t step = ch.cat_axis.major >= 1 ? static_cast<int64_t>(ch.cat_axis.major) : 0;
      if (step <= 0) {
        const float lw = ImGui::CalcTextSize(engine::FormatChartNumber(UnitSerial(dplan.u1, dplan.unit), dfmt).c_str()).x + ImGui::GetFontSize();
        const ImPlotPlot* cur = ImPlot::GetCurrentPlot();
        const float axis_len = cur ? cur->FrameRect.GetWidth() * 0.85f : 0.0f;
        const int64_t room = axis_len > 0 && lw > 0 ? std::clamp<int64_t>(static_cast<int64_t>(axis_len / lw), 1, 12) : 12;
        step = std::max<int64_t>(1, (t1 - t0 + 1 + room - 1) / room);
      }
      std::vector<double> ticks;
      std::vector<std::string> tmp;
      std::vector<const char*> labels;
      for (int64_t t = t0; t <= t1 && ticks.size() < 400; t += step) {
        const double serial = UnitSerial(t, tu);
        const double at = static_cast<double>(DateUnit(serial, dplan.unit) - dplan.u0) + (barlike ? 0.0 : 1.0);
        if (at < cat_lo - 1e-9 || at > cat_hi + 1e-9) continue;
        ticks.push_back(at);
        tmp.push_back(engine::FormatChartNumber(serial, dfmt));
      }
      for (const auto& t : tmp) labels.push_back(t.c_str());
      if (!ticks.empty()) ImPlot::SetupAxisTicks(cat_ax, ticks.data(), static_cast<int>(ticks.size()), labels.data());
    } else if (!scatter && (!cc.v.cats.empty() || barlike)) {
      std::vector<double> ticks;
      std::vector<std::string> tmp;
      std::vector<const char*> labels;
      const size_t cnt = std::min(n, cc.v.cats.empty() ? n : cc.v.cats.size());
      size_t step = static_cast<size_t>(std::max(0, ch.cat_axis.label_interval));
      cat_levels = engine::ChartCategoryLevels(ch, cc.v, cnt);
      if (!cat_levels.empty() && step == 0) step = 1;  // levels: every inner label, the outer ones grouped below
      if (step == 0) {
        // as many labels as fit along the axis without touching (dates are wide), at most 12
        float widest = 0;
        for (size_t i = 0; i < cnt && i < cc.v.cats.size(); i += std::max<size_t>(1, cnt / 200)) widest = std::max(widest, ImGui::CalcTextSize(cc.v.cats[i].c_str()).x);
        const ImPlotPlot* cur = ImPlot::GetCurrentPlot();
        const float axis_len = cur ? (horizontal ? cur->FrameRect.GetHeight() : cur->FrameRect.GetWidth() * 0.85f) : 0.0f;
        const float per = horizontal ? ImGui::GetTextLineHeight() * 1.3f : widest + ImGui::GetFontSize();
        const size_t room = axis_len > 0 && per > 0 ? std::clamp<size_t>(static_cast<size_t>(axis_len / per), 1, 12) : 12;
        step = std::max<size_t>(1, n / room + ((n % room) ? 1 : 0));
      }
      // levels: the inner label, with room for the outer rows below it (beside it on a vertical axis)
      size_t nlev = 0;
      float outer_w = 0;
      for (const auto& p : cat_levels) nlev = std::max(nlev, p.size());
      if (!cat_levels.empty() && horizontal) {
        std::vector<float> lw(nlev, 0.0f);
        for (const auto& p : cat_levels) {
          for (size_t k = 0; k + 1 < p.size(); ++k) lw[k] = std::max(lw[k], ImGui::CalcTextSize(p[k].c_str()).x);
        }
        for (size_t k = 0; k + 1 < nlev; ++k) outer_w += lw[k] + ImGui::GetFontSize();
      }
      const float space_w = std::max(1.0f, ImGui::CalcTextSize(" ").x);
      // Axis.TickMarkSpacing: a tick mark every `mark` categories, the labels every `step` (a mark without a label
      // has an empty one)
      const size_t mark = ch.cat_axis.mark_interval > 0 ? static_cast<size_t>(ch.cat_axis.mark_interval) : step;
      for (size_t i = 0; i < cnt; i += (mark == step ? step : 1)) {
        if (mark != step && i % step != 0 && i % mark != 0) continue;
        ticks.push_back(barlike ? static_cast<double>(i) : static_cast<double>(i + 1));
        if (mark != step && i % step != 0) {
          tmp.emplace_back();
          continue;
        }
        if (!cat_levels.empty()) {
          const std::string inner = cat_levels[i].empty() ? std::string() : cat_levels[i].back();
          if (horizontal) tmp.push_back(std::string(static_cast<size_t>(std::ceil(outer_w / space_w)), ' ') + inner);
          else tmp.push_back(inner + std::string(nlev - 1, '\n'));
        } else {
          tmp.push_back(engine::ChartCategoryLabel(cc.v, i));
        }
      }
      for (const auto& t : tmp) labels.push_back(t.c_str());
      if (!ticks.empty()) ImPlot::SetupAxisTicks(cat_ax, ticks.data(), static_cast<int>(ticks.size()), labels.data());
    }
    // an axis in a font of its own (or underlined): ImPlot lays its labels out, they are drawn below in that font
    struct OwnLabels {
      ImAxis ax;
      const model::ChartAxisFormat* a;
    };
    std::vector<OwnLabels> own_labels;
    if (ImPlotPlot* sp0 = ImPlot::GetCurrentPlot()) {
      auto own = [&](ImAxis ax, const model::ChartAxisFormat& a) {
        const model::ChartFont& f = AxisFontOf(ch, a);
        if (f.underline || !(f.name == vfont.name && f.size == vfont.size && f.bold == vfont.bold && f.italic == vfont.italic)) {
          sp0->Axes[ax].ColorTxt = 0;
          own_labels.push_back({ax, &a});
        } else {
          sp0->Axes[ax].ColorTxt = ChartTextCol(f);
        }
        // Fill & Line: the axis' tick marks in its line color (No line: none)
        if (a.line_color) sp0->Axes[ax].ColorTick = ArgbCol(a.line_color);
        if (a.no_line) sp0->Axes[ax].ColorTick = 0;
      };
      own(cat_ax, ch.cat_axis);
      own(val_ax, ch.val_axis);
      if (y2) own(ImAxis_Y2, ch.val_axis2);
    }
    ImPlot::SetupFinish();
    ImPlotPlot* pl = ImPlot::GetCurrentPlot();
    if (pl) {
      // the value axis' display unit label ("Thousands", "x 250": Axis.HasDisplayUnitLabel) above its labels
      auto unit_label = [&](ImAxis axn, const model::ChartAxisFormat& a) {
        if (!a.visible || !a.unit_label || !(a.display_unit > 0) || a.display_unit == 1) return;
        const ImPlotAxis& ax = pl->Axes[axn];
        if (!ax.Enabled) return;
        const std::string text = ChartUnitLabel(a.display_unit);
        const model::ChartFont& f = AxisFontOf(ch, a);
        ImGui::PushFont(ChartFontOf(f), ChartFontSize(f, 9, font_scale));
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const float x = ax.IsOpposite() ? pl->PlotRect.Max.x - ts.x : pl->PlotRect.Min.x - ts.x * 0.5f;
        cdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImFloor(ImVec2(std::max(x, pl->FrameRect.Min.x + 2), pl->PlotRect.Min.y - ts.y - 2)),
                     ChartTextCol(f), text.c_str());
        ImGui::PopFont();
      };
      if (!horizontal) {
        unit_label(val_ax, ch.val_axis);
        if (y2) unit_label(ImAxis_Y2, ch.val_axis2);
      }
      for (const OwnLabels& o : own_labels) {
        const ImPlotAxis& ax = pl->Axes[o.ax];
        if (!ax.Enabled || !ax.HasTickLabels()) continue;
        const model::ChartFont& f = AxisFontOf(ch, *o.a);
        ImGui::PushFont(ChartFontOf(f), ChartFontSize(f, 9, font_scale));
        const ImU32 col = ChartTextCol(f);
        const ImVec2 lpad = ImPlot::GetStyle().LabelPadding;
        const bool xaxis = o.ax == ImAxis_X1 || o.ax == ImAxis_X2 || o.ax == ImAxis_X3;
        for (int j = 0; j < ax.Ticker.TickCount(); ++j) {
          const ImPlotTick& tk = ax.Ticker.Ticks[j];
          if (!tk.ShowLabel) continue;
          const char* txt = ax.Ticker.GetText(j);
          const ImVec2 ts = ImGui::CalcTextSize(txt);
          ImVec2 tp;
          if (xaxis) {
            if (tk.PixelPos < pl->PlotRect.Min.x - 1 || tk.PixelPos > pl->PlotRect.Max.x + 1) continue;
            tp = ImVec2(tk.PixelPos - ts.x * 0.5f, ax.IsOpposite() ? ax.Datum1 - lpad.y - ts.y : ax.Datum1 + lpad.y);
          } else {
            if (tk.PixelPos < pl->PlotRect.Min.y - 1 || tk.PixelPos > pl->PlotRect.Max.y + 1) continue;
            tp = ImVec2(ax.IsOpposite() ? ax.Datum1 + lpad.x : ax.Datum1 - lpad.x - ts.x, tk.PixelPos - ts.y * 0.5f);
          }
          tp = ImFloor(tp);
          cdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), tp, col, txt);
          ChartUnderline(cdl, f, tp, ts, col);
        }
        ImGui::PopFont();
      }
      // Multi-level Category Labels: the outer levels' labels in rows below the inner ones (columns left of them on a
      // vertical axis), each centered on its group, with lines between the groups
      if (!cat_levels.empty() && ch.cat_axis.visible && ch.cat_axis.label_pos != 3) {
        const ImPlotAxis& lax = pl->Axes[cat_ax];
        size_t nlev = 0;
        for (const auto& p : cat_levels) nlev = std::max(nlev, p.size());
        const ImVec2 lpad = ImPlot::GetStyle().LabelPadding;
        const float lh = ImGui::GetTextLineHeight(), em = ImGui::GetFontSize();
        const ImU32 col = ChartTextCol(AxisFontOf(ch, ch.cat_axis)), sep = IM_COL32(0xBF, 0xBF, 0xBF, 255);
        auto pix = [&](double i) { return static_cast<float>(lax.PlotToPixels(barlike ? i : i + 1.0)); };
        std::vector<float> lw(nlev, 0.0f);
        float inner_w = 0;
        for (const auto& p : cat_levels) {
          for (size_t k = 0; k + 1 < p.size(); ++k) lw[k] = std::max(lw[k], ImGui::CalcTextSize(p[k].c_str()).x);
          if (!p.empty()) inner_w = std::max(inner_w, ImGui::CalcTextSize(p.back().c_str()).x);
        }
        for (size_t k = 0; k + 1 < nlev; ++k) {
          // the band of level k: row (nlev - 1 - k) below the axis, or a column left of the inner labels
          float b0 = 0, b1 = 0;
          if (!horizontal) {
            b0 = lax.Datum1 + lpad.y + lh * static_cast<float>(nlev - 1 - k);
            b1 = b0 + lh;
          } else {
            float right = lax.Datum1 - lpad.x - inner_w - em * 0.5f;
            for (size_t q = nlev - 1; q-- > k + 1;) right -= lw[q] + em;
            b1 = right;
            b0 = right - lw[k];
          }
          size_t i = 0;
          while (i < cat_levels.size()) {
            size_t j = i + 1;
            auto same = [&](size_t a, size_t b) {
              if (cat_levels[a].size() <= k + 1 || cat_levels[b].size() <= k + 1) return false;
              return std::equal(cat_levels[a].begin(), cat_levels[a].begin() + static_cast<std::ptrdiff_t>(k + 1), cat_levels[b].begin());
            };
            while (j < cat_levels.size() && same(i, j)) ++j;
            const float g0 = pix(static_cast<double>(i) - 0.5), g1 = pix(static_cast<double>(j) - 0.5);
            const float glo = std::min(g0, g1), ghi = std::max(g0, g1);
            const std::string t = cat_levels[i].size() > k + 1 ? cat_levels[i][k] : std::string();
            const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            // tools find each outer label as a person sees it (level, text)
            if (!t.empty()) ui::NoteUiItem(("Category Level " + std::to_string(k + 1) + ": " + t).c_str(), horizontal ? ImRect(ImVec2(b1 - ts.x, (glo + ghi - ts.y) * 0.5f), ImVec2(b1, (glo + ghi + ts.y) * 0.5f)) : ImRect(ImVec2((glo + ghi - ts.x) * 0.5f, b0), ImVec2((glo + ghi + ts.x) * 0.5f, b1)));
            if (!horizontal) {
              cdl->PushClipRect(ImVec2(glo, b0), ImVec2(ghi, b1 + 1), true);
              cdl->AddText(ImFloor(ImVec2((glo + ghi - ts.x) * 0.5f, b0)), col, t.c_str());
              cdl->PopClipRect();
              // the lines between the groups: down from the axis through this level's row
              for (float gx : {g0, g1}) cdl->AddLine(ImVec2(gx, lax.Datum1), ImVec2(gx, b1), sep, 1.0f);
            } else {
              cdl->AddText(ImFloor(ImVec2(b1 - ts.x, (glo + ghi - ts.y) * 0.5f)), col, t.c_str());
              for (float gy : {g0, g1}) cdl->AddLine(ImVec2(b0 - em * 0.25f, gy), ImVec2(lax.Datum1, gy), sep, 1.0f);
            }
            i = j;
          }
        }
      }
      // Fill & Line: the axis' own line along the plot area
      auto axis_line = [&](ImAxis id, const model::ChartAxisFormat& a) {
        if (!a.line_color || a.no_line || !a.visible) return;
        const ImPlotAxis& ax = pl->Axes[id];
        if (!ax.Enabled) return;
        const float w = a.line_weight > 0 ? a.line_weight * kPtPx * font_scale : 1.0f;
        const bool xaxis = id == ImAxis_X1;
        const float d = ax.Datum1;
        if (xaxis) cdl->AddLine(ImVec2(pl->PlotRect.Min.x, d), ImVec2(pl->PlotRect.Max.x, d), ArgbCol(a.line_color), w);
        else cdl->AddLine(ImVec2(d, pl->PlotRect.Min.y), ImVec2(d, pl->PlotRect.Max.y), ArgbCol(a.line_color), w);
      };
      axis_line(cat_ax, ch.cat_axis);
      axis_line(val_ax, ch.val_axis);
      if (y2) axis_line(ImAxis_Y2, ch.val_axis2);
      hit.plot = pl->PlotRect;
      hit.plot_frame = pl->FrameRect;
      hit.cat_axis = pl->Axes[cat_ax].HoverRect;
      hit.val_axis = pl->Axes[val_ax].HoverRect;
      if (y2) hit.val_axis2 = pl->Axes[ImAxis_Y2].HoverRect;
    }
    ImDrawList* pdl = ImPlot::GetPlotDrawList();
    // gridlines: where the major ones are (to click them); minor ones drawn here, lighter
    if (vplan_ok && pl) {
      ImPlot::PushPlotClipRect();
      ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
      if (ch.val_axis.gridlines) {
        for (double t : vplan.ticks) {
          const ImVec2 q = horizontal ? ImPlot::PlotToPixels(t, cat_lo) : ImPlot::PlotToPixels(cat_lo, t);
          hit.grid.push_back(horizontal ? q.x : q.y);
        }
        hit.grid_vertical = horizontal;
      }
      if (ch.val_axis.minor_gridlines && !ch.val_axis.log) {
        const double mstep = ch.val_axis.minor > 0 ? ch.val_axis.minor : vplan.step / 5;
        int cnt = 0;
        for (double t = std::ceil(vplan.lo / mstep - 1e-9) * mstep; t <= vplan.hi && cnt < 1000 && mstep > 0; t += mstep, ++cnt) {
          if (horizontal) {
            const float x = ImPlot::PlotToPixels(t, 0).x;
            pdl->AddLine(ImVec2(x, hit.plot.Min.y), ImVec2(x, hit.plot.Max.y), IM_COL32(0xEE, 0xEE, 0xEE, 255));
            hit.minor_grid.push_back(x);
          } else {
            const float y = ImPlot::PlotToPixels(0, t).y;
            pdl->AddLine(ImVec2(hit.plot.Min.x, y), ImVec2(hit.plot.Max.x, y), IM_COL32(0xEE, 0xEE, 0xEE, 255));
            hit.minor_grid.push_back(y);
          }
        }
      }
      if (ch.val_axis.crosses == 1) {  // the horizontal axis crosses at a value: its line there
        const ImVec2 q = horizontal ? ImPlot::PlotToPixels(bar_base, 0) : ImPlot::PlotToPixels(0, bar_base);
        if (horizontal) pdl->AddLine(ImVec2(q.x, hit.plot.Min.y), ImVec2(q.x, hit.plot.Max.y), IM_COL32(0xA6, 0xA6, 0xA6, 255));
        else pdl->AddLine(ImVec2(hit.plot.Min.x, q.y), ImVec2(hit.plot.Max.x, q.y), IM_COL32(0xA6, 0xA6, 0xA6, 255));
      }
      ImPlot::PopPlotClipRect();
    }
    // a series' drawn points, for clicking it and its points (at most ~400 per series)
    auto note_points = [&](size_t i, const double* xs, const double* ys, int cnt) {
      if (i >= hit.lines.size() || cnt <= 0) return;
      const int st = std::max(1, cnt / 400);
      for (int k = 0; k < cnt; k += st) {
        if (!std::isfinite(xs[k]) || !std::isfinite(ys[k])) continue;
        hit.lines[i].push_back(horizontal ? ImPlot::PlotToPixels(ys[k], xs[k]) : ImPlot::PlotToPixels(xs[k], ys[k]));
        hit.line_idx[i].push_back(k);
      }
    };
    auto line_col = [&](size_t i, const SeriesBuf& b) {
      return i < ch.series.size() && ch.series[i].line_color ? ExcelColor(ch.series[i].line_color) : color_of(i, b);
    };
    auto spec_for = [&](size_t i, const SeriesBuf& b) {
      ImPlotSpec sp;
      sp.LineColor = ImGui::ColorConvertU32ToFloat4(line_col(i, b));
      sp.FillColor = ImGui::ColorConvertU32ToFloat4(color_of(i, b));
      sp.LineWeight = 2.0f;
      if (i < ch.series.size() && ch.series[i].line_weight > 0) sp.LineWeight = ch.series[i].line_weight * kPtPx * font_scale * ui::Dpi();
      return sp;
    };
    // a series' own marker (Series.MarkerStyle / MarkerSize / marker colors); -1 keeps the chart type's
    auto apply_marker = [&](size_t i, ImPlotSpec& sp) {
      if (i >= ch.series.size()) return;
      const model::ChartSeries& d = ch.series[i];
      if (d.marker == 0) sp.Marker = ImPlotMarker_None;
      else if (d.marker > 0) sp.Marker = ImPlotMarkerOf(d.marker);
      if (d.marker != 0 && d.marker_size > 0) sp.MarkerSize = d.marker_size * 0.5f * kPtPx * font_scale * ui::Dpi();
      if (d.marker > 0) {
        sp.MarkerFillColor = sp.FillColor;
        if (sp.MarkerSize <= 0) sp.MarkerSize = 3.5f;
      }
      if (d.marker_fill) sp.MarkerFillColor = ImGui::ColorConvertU32ToFloat4(ExcelColor(d.marker_fill));
      if (d.marker_line) sp.MarkerLineColor = ImGui::ColorConvertU32ToFloat4(ExcelColor(d.marker_line));
    };
    // x / y in plot coordinates (a horizontal bar chart swaps them)
    auto px = [&](double x, double y) { return horizontal ? ImPlot::PlotToPixels(y, x) : ImPlot::PlotToPixels(x, y); };
    auto plot_line = [&](const char* label, const double* xs, const double* ys, int cnt, const ImPlotSpec& sp) {
      if (horizontal) ImPlot::PlotLine(label, ys, xs, cnt, sp);
      else ImPlot::PlotLine(label, xs, ys, cnt, sp);
    };
    // a data label beside a point or in a column (Format Data Labels > Label Position)
    auto put_label = [&](size_t i, const std::string& contents, ImRect box, bool bar, bool negative, int pos_default, int pt) {
      const std::string& t = LabelOwnText(cc.v, i, pt, contents);
      if (t.empty()) return;
      static const model::ChartSeries kNoSeries;
      const model::ChartSeries& ds = i < ch.series.size() ? ch.series[i] : kNoSeries;
      const model::ChartFont& lf = LabelFontOf(ds, pt);
      ImGui::PushFont(ChartFontOf(lf), ChartFontSize(lf, 9, font_scale));
      const float key = (LabelShowOf(ds, pt) & 16) ? ImGui::GetFontSize() * 0.6f + S(3) : 0.0f;  // Legend key
      const ImVec2 ts = ImGui::CalcTextSize(t.c_str()) + ImVec2(key, 0);
      const int lp = LabelPosOf(ds, pt) >= 0 ? LabelPosOf(ds, pt) : pos_default;
      const ImVec2 c = box.GetCenter();
      ImVec2 tp;
      ImU32 col = IM_COL32(0x40, 0x40, 0x40, 255);
      const float g = S(3);
      if (bar) {
        // along the bar: its end is where the value is (the top of a positive column)
        const bool up = !negative;
        if (horizontal) {
          const float end = up ? box.Max.x : box.Min.x, base = up ? box.Min.x : box.Max.x, dir = up ? 1.0f : -1.0f;
          float x = c.x - ts.x * 0.5f;
          if (lp == 1) x = end - dir * g - (up ? ts.x : 0);
          else if (lp == 2) x = base + dir * g - (up ? 0 : ts.x);
          else if (lp == 3) x = end + dir * g - (up ? 0 : ts.x);
          tp = ImVec2(x, c.y - ts.y * 0.5f);
        } else {
          const float end = up ? box.Min.y : box.Max.y, base = up ? box.Max.y : box.Min.y, dir = up ? -1.0f : 1.0f;
          float y = c.y - ts.y * 0.5f;
          if (lp == 1) y = end - dir * g - (up ? 0 : ts.y);
          else if (lp == 2) y = base + dir * g - (up ? ts.y : 0);
          else if (lp == 3) y = end + dir * g - (up ? ts.y : 0);
          tp = ImVec2(c.x - ts.x * 0.5f, y);
        }
        if (lp == 0 || lp == 1 || lp == 2) col = IM_COL32_WHITE;
      } else {
        switch (lp) {
          case 0: tp = c - ts * 0.5f; break;                                   // center
          case 4: tp = ImVec2(c.x - ts.x - g * 2, c.y - ts.y * 0.5f); break;     // left
          case 6: tp = ImVec2(c.x - ts.x * 0.5f, c.y - ts.y - g * 2); break;     // above
          case 7: tp = ImVec2(c.x - ts.x * 0.5f, c.y + g * 2); break;            // below
          default: tp = ImVec2(c.x + g * 2, c.y - ts.y * 0.5f); break;           // right
        }
      }
      if (lf.color) col = ArgbCol(lf.color);
      pdl->PushClipRect(hit.frame.Min, hit.frame.Max, false);  // a label dragged out of the plot area stays in the chart
      DrawLabelAt(pdl, ds, pt, t, tp, ts, key, col, color_of(i, series[i]), box, hit.frame);
      pdl->PopClipRect();
      if (i < hit.labels.size()) {
        hit.labels[i].emplace_back(LabelRectAt(ds, pt, tp, ts, hit.frame));
        hit.label_idx[i].push_back(pt);
        if (i < hit.label_texts.size()) hit.label_texts[i].push_back(t);
        ui::NoteUiItem(t.c_str(), hit.labels[i].back());  // the UI item recorder sees each label where it is drawn
      }
      ImGui::PopFont();
    };
    ImPlot::PushPlotClipRect();
    // ---- columns and bars (behind the lines): drawn rectangle by rectangle (each point's own fill, borders) ----
    const double gap = std::clamp(ch.gap_width, 0, 500) / 100.0, ov = bar_stacked ? 1.0 : std::clamp(ch.overlap, -100, 100) / 100.0;
    const double nbars = nb ? static_cast<double>(nb) : 1.0;
    const double span = bar_stacked ? 1.0 : nbars - (nbars - 1) * ov;
    const double bar_w = 1.0 / (span + gap), group_w = bar_w * span;
    std::vector<std::vector<double>> xpos(ns);
    // a category's place: its index, or on a date axis its date
    auto cx = [&](int j) { return dplan.on ? (static_cast<size_t>(j) < dplan.pos.size() ? dplan.pos[static_cast<size_t>(j)] - 1 : kNaN) : static_cast<double>(j); };
    auto xs_of = [&](size_t i) -> const double* {
      const SeriesBuf& b = series[i];
      const auto it = std::find(cc.bar_idx.begin(), cc.bar_idx.end(), i);
      if (it != cc.bar_idx.end()) {
        const double k = static_cast<double>(it - cc.bar_idx.begin());
        auto& v = xpos[i];
        if (v.empty()) {
          v.resize(static_cast<size_t>(groups));
          for (int j = 0; j < groups; ++j) v[static_cast<size_t>(j)] = bar_stacked ? cx(j) : cx(j) - group_w / 2 + bar_w / 2 + k * bar_w * (1 - ov);
        }
        return v.data();
      }
      if (barlike && !b.numeric_x) {
        auto& v = xpos[i];
        if (v.empty()) {
          const int cnt = b.N();
          v.resize(static_cast<size_t>(std::max(0, cnt)));
          for (int j = 0; j < cnt; ++j) v[static_cast<size_t>(j)] = dplan.on ? cx(j) : b.X()[j] - 1;
        }
        return v.data();
      }
      if (dplan.on && !b.numeric_x) {
        auto& v = xpos[i];
        if (v.empty()) {
          const int cnt = b.N();
          v.resize(static_cast<size_t>(std::max(0, cnt)));
          for (int j = 0; j < cnt; ++j) v[static_cast<size_t>(j)] = cx(j) + 1;
        }
        return v.data();
      }
      return b.X();
    };
    pdl = ImPlot::GetPlotDrawList();
    for (size_t k = 0; k < nb && groups > 0; ++k) {
      const size_t i = cc.bar_idx[k];
      if (filtered(i)) continue;
      static const model::ChartSeries kNone;
      const model::ChartSeries& d = i < ch.series.size() ? ch.series[i] : kNone;
      ImPlot::SetAxes(ImAxis_X1, series[i].axis == 1 && y2 ? ImAxis_Y2 : ImAxis_Y1);
      const double* bx = xs_of(i);
      const double hw = (bar_stacked ? group_w : bar_w) / 2;
      const ImU32 base_col = color_of(i, series[i]);
      const int vary_i = VaryByPoint(ch);
      for (int j = 0; j < groups && j < 5000; ++j) {
        const size_t at = k * static_cast<size_t>(groups) + static_cast<size_t>(j);
        const double v = cc.bars[at];
        const ImVec2 a = px(bx[j] - hw, bhi[at]), b2 = px(bx[j] + hw, blo[at]);
        const ImRect r(ImMin(a, b2), ImMax(a, b2));
        const model::ChartPointFormat* pf = PointFmt(d, j);
        const bool vary = vary_i == static_cast<int>(i);
        ImU32 fill = pf && pf->fill ? ExcelColor(pf->fill) : vary ? static_cast<ImU32>(engine::ChartPalette(static_cast<size_t>(j))) : base_col;
        const bool inverted = d.invert_negative && v < 0 && !(pf && pf->fill);
        if (inverted) fill = d.invert_color ? ExcelColor(d.invert_color) : IM_COL32_WHITE;
        if (!d.no_fill) {
          if ((pf && pf->fill) || inverted || vary) pdl->AddRectFilled(r.Min, r.Max, WithTransparency(fill, d.transparency));
          else SeriesFillPoly(pdl, RectPoly(r), d, fill, true);  // gradient, picture, pattern
        }
        const uint32_t border = pf && pf->line ? pf->line : d.line_color;
        const float bw = d.line_weight > 0 ? d.line_weight * kPtPx : 1.0f;
        if (border && !d.no_line && d.dash != 0) {
          std::vector<ImVec2> rp = RectPoly(r);
          rp.push_back(rp[0]);
          DashedPolyline(pdl, rp, ExcelColor(border), bw, d.dash);
        } else if (border && !d.no_line) {
          pdl->AddRect(r.Min, r.Max, ExcelColor(border), 0.0f, bw);
        }
        else if (d.invert_negative && v < 0 && !d.invert_color) pdl->AddRect(r.Min, r.Max, base_col);
        hit.boxes[i].push_back(r);
        if (PointLabelOn(d, j) && j < 2000 && (v != 0 || !bar_stacked)) {
          const double pct = bar_pct ? bhi[at] - blo[at] : -1.0;
          put_label(i, LabelText(d, series[i], cc.v, j, v, pct), r, true, v < 0, bar_stacked ? 0 : 3, j);
        }
      }
      // (interactive charts fit the plot to what is drawn)
      if (interactive) {
        ImPlotSpec inv;
        inv.LineColor = ImVec4(0, 0, 0, 0);
        std::vector<double> lo2(blo.begin() + static_cast<std::ptrdiff_t>(k * static_cast<size_t>(groups)), blo.begin() + static_cast<std::ptrdiff_t>((k + 1) * static_cast<size_t>(groups)));
        std::vector<double> hi2(bhi.begin() + static_cast<std::ptrdiff_t>(k * static_cast<size_t>(groups)), bhi.begin() + static_cast<std::ptrdiff_t>((k + 1) * static_cast<size_t>(groups)));
        plot_line(("##fit_lo" + std::to_string(i)).c_str(), bx, lo2.data(), groups, inv);
        plot_line(("##fit_hi" + std::to_string(i)).c_str(), bx, hi2.data(), groups, inv);
      }
    }
    // ---- areas (stacked ones on top of each other; Excel fills them solid, later series in front) ----
    for (size_t i = 0; i < ns; ++i) {
      if (!engine::IsAreaType(types[i]) || filtered(i)) continue;
      const SeriesBuf& b = series[i];
      const int cnt = static_cast<int>(dy[i].size());
      if (cnt <= 0) continue;
      static const model::ChartSeries kNone;
      const model::ChartSeries& d = i < ch.series.size() ? ch.series[i] : kNone;
      ImPlot::SetAxes(ImAxis_X1, b.axis == 1 && y2 ? ImAxis_Y2 : ImAxis_Y1);
      ImPlotSpec sp = spec_for(i, b);
      const double* X = xs_of(i);
      sp.FillAlpha = d.no_fill || d.fill_type != 0 ? 0.0f : 1.0f - std::clamp(d.transparency, 0, 100) / 100.0f;
      if (dlo[i].empty()) ImPlot::PlotShaded(b.name.c_str(), X, dy[i].data(), cnt, 0.0, sp);
      else ImPlot::PlotShaded(b.name.c_str(), X, dlo[i].data(), dy[i].data(), cnt, sp);
      if (!d.no_fill && d.fill_type != 0) {  // gradient, picture or pattern: the area's outline filled here
        std::vector<ImVec2> poly;
        for (int k2 = 0; k2 < cnt; ++k2) poly.push_back(ImPlot::PlotToPixels(X[k2], std::isfinite(dy[i][static_cast<size_t>(k2)]) ? dy[i][static_cast<size_t>(k2)] : 0.0));
        for (int k2 = cnt; k2-- > 0;) poly.push_back(ImPlot::PlotToPixels(X[k2], dlo[i].empty() ? 0.0 : dlo[i][static_cast<size_t>(k2)]));
        SeriesFillPoly(pdl, poly, d, color_of(i, b), false);
      }
      if (d.line_color && !d.no_line) {  // a border only when one is chosen (Excel's areas have none)
        ImPlotSpec ln = sp;
        ImPlot::PlotLine(b.name.c_str(), X, dy[i].data(), cnt, ln);
      }
      note_points(i, X, dy[i].data(), cnt);
      for (int k2 = 0; k2 < cnt && k2 < 500; ++k2) {
        if (!PointLabelOn(d, k2) || !std::isfinite(dy[i][static_cast<size_t>(k2)])) continue;
        const ImVec2 q = ImPlot::PlotToPixels(X[k2], dy[i][static_cast<size_t>(k2)]);
        put_label(i, LabelText(d, b, cc.v, k2, b.Y()[k2], -1), ImRect(q, q), false, false, 6, k2);
      }
    }
    // ---- lines, scatter and bubbles ----
    for (size_t i = 0; i < ns; ++i) {
      const ChartType t = types[i];
      if (engine::IsBarType(t) || engine::IsAreaType(t) || filtered(i) || stock) continue;
      const SeriesBuf& b = series[i];
      const int cnt = static_cast<int>(dy[i].size());
      if (cnt <= 0) continue;
      static const model::ChartSeries kNone;
      const model::ChartSeries& d = i < ch.series.size() ? ch.series[i] : kNone;
      ImPlot::SetAxes(ImAxis_X1, b.axis == 1 && y2 ? ImAxis_Y2 : ImAxis_Y1);
      ImPlotSpec sp = spec_for(i, b);
      const double* X = xs_of(i);
      const double* Y = dy[i].data();
      note_points(i, X, Y, cnt);
      if (t == ChartType::Bubble) {
        double smax = 0;
        for (double v : b.sizes) {
          if (std::isfinite(v)) smax = std::max(smax, std::fabs(v));
        }
        const float rmax = std::min(hit.plot.GetWidth(), hit.plot.GetHeight()) * 0.125f;
        const ImU32 fill = WithTransparency(color_of(i, b), d.transparency ? d.transparency : 15);
        for (int k2 = 0; k2 < cnt; ++k2) {
          if (!std::isfinite(X[k2]) || !std::isfinite(Y[k2])) continue;
          const double sz = static_cast<size_t>(k2) < b.sizes.size() && std::isfinite(b.sizes[static_cast<size_t>(k2)]) ? std::fabs(b.sizes[static_cast<size_t>(k2)]) : 0.0;
          const float r = smax > 0 ? static_cast<float>(std::sqrt(sz / smax)) * rmax : S(4);
          const ImVec2 q = ImPlot::PlotToPixels(X[k2], Y[k2]);
          if (!d.no_fill) pdl->AddCircleFilled(q, std::max(1.0f, r), fill, 32);
          if (!d.no_line) pdl->AddCircle(q, std::max(1.0f, r), d.line_color ? ExcelColor(d.line_color) : color_of(i, b), 32, 1.0f);
          hit.boxes[i].emplace_back(q - ImVec2(r, r), q + ImVec2(r, r));
          if (PointLabelOn(d, k2)) put_label(i, LabelText(d, b, cc.v, k2, b.Y()[k2], -1), ImRect(q, q), false, false, 0, k2);
        }
        continue;
      }
      // markers: the chart type's (line with markers, scatter), then the series' own
      const bool scatter_t = engine::IsScatterType(t);
      if (engine::HasMarkers(t) && cnt <= 2000) {
        sp.Marker = ImPlotMarker_Circle;
        sp.MarkerSize = scatter_t ? 3.0f : 3.5f;
        sp.MarkerFillColor = sp.FillColor;
      }
      if (cnt <= 2000) apply_marker(i, sp);
      const bool line = !scatter_t || t == ChartType::ScatterLines || t == ChartType::ScatterLinesNoMarkers || t == ChartType::ScatterSmooth ||
                        t == ChartType::ScatterSmoothNoMarkers || sp.Marker == ImPlotMarker_None || d.smooth;
      const bool smooth = d.smooth || t == ChartType::ScatterSmooth || t == ChartType::ScatterSmoothNoMarkers;
      // empty cells: gaps (Excel's default), zero, or joined; #N/A joined when it isn't shown as an empty cell
      std::vector<double> jx, jy;
      const double* LX = X;
      const double* LY = Y;
      int ln = cnt;
      if (!b.joined.empty()) {
        for (int k2 = 0; k2 < cnt; ++k2) {
          if (std::binary_search(b.joined.begin(), b.joined.end(), k2)) continue;
          jx.push_back(X[k2]);
          jy.push_back(Y[k2]);
        }
        LX = jx.data();
        LY = jy.data();
        ln = static_cast<int>(jx.size());
      }
      const ImPlotLineFlags lflags = ch.display_blanks == 3 ? ImPlotLineFlags_SkipNaN : ImPlotLineFlags_None;
      if (line && !d.no_line) {
        static std::vector<double> sx, sy;
        const double* PX = LX;
        const double* PY = LY;
        int pn = ln;
        if (smooth && ln >= 3 && ln <= 20000) {
          SmoothCurve(LX, LY, ln, &sx, &sy);
          PX = sx.data();
          PY = sy.data();
          pn = static_cast<int>(sx.size());
        }
        ImPlotSpec lsp = sp;
        lsp.Marker = ImPlotMarker_None;
        lsp.Flags = lflags;
        if (StyledLine(d)) {  // dash, compound type, cap, join, arrows
          std::vector<ImVec2> pts;
          for (int k2 = 0; k2 < pn; ++k2) {
            if (!std::isfinite(PY[k2]) && lflags) continue;
            pts.push_back(std::isfinite(PY[k2]) ? px(PX[k2], PY[k2]) : ImVec2(NAN, NAN));
          }
          DrawSeriesLine(pdl, pts, line_col(i, b), lsp.LineWeight, d);
          ImPlotSpec fitsp;
          fitsp.LineColor = ImVec4(0, 0, 0, 0);
          plot_line(b.name.c_str(), PX, PY, pn, fitsp);
        } else {
          plot_line(b.name.c_str(), PX, PY, pn, lsp);
        }
      }
      if (sp.Marker != ImPlotMarker_None) {
        if (horizontal) ImPlot::PlotScatter(b.name.c_str(), Y, X, cnt, sp);
        else ImPlot::PlotScatter(b.name.c_str(), X, Y, cnt, sp);
      } else if (!line || d.no_line) {
        ImPlotSpec fitsp;
        fitsp.LineColor = ImVec4(0, 0, 0, 0);
        plot_line(b.name.c_str(), X, Y, cnt, fitsp);
      }
      // points with their own color
      for (const auto& pf : d.points) {
        if (pf.index < 0 || pf.index >= cnt || !pf.fill || !std::isfinite(Y[pf.index])) continue;
        const ImVec2 q = px(X[pf.index], Y[pf.index]);
        const float r = std::max(S(3.5f), sp.MarkerSize);
        pdl->AddCircleFilled(q, r, ExcelColor(pf.fill));
        if (pf.line) pdl->AddCircle(q, r, ExcelColor(pf.line));
      }
      for (int k2 = 0; k2 < cnt && k2 < 500; ++k2) {
        if (!PointLabelOn(d, k2) || !std::isfinite(Y[k2])) continue;
        const ImVec2 q = px(X[k2], Y[k2]);
        put_label(i, LabelText(d, b, cc.v, k2, b.Y()[k2], engine::IsPercentType(t) && !dlo[i].empty() ? Y[k2] - dlo[i][static_cast<size_t>(k2)] : -1), ImRect(q, q), false, false, 5, k2);
      }
    }
    // ---- stock charts: volume, up / down bars, high-low lines ----
    if (stock) DrawStockBody(pdl, cc, ch, dplan);
    ImPlot::PopPlotClipRect();
    // ---- trendlines (each series may have several) and error bars (Y; a scatter chart's X too) ----
    for (size_t i = 0; i < ns && i < ch.series.size(); ++i) {
      const model::ChartSeries& d = ch.series[i];
      if (filtered(i) || stock || (d.trend == model::TrendKind::None && d.err == model::ErrorBarKind::None && d.xerr == model::ErrorBarKind::None)) continue;
      const SeriesBuf& b = series[i];
      const bool bar = std::find(cc.bar_idx.begin(), cc.bar_idx.end(), i) != cc.bar_idx.end();
      const int cnt = bar ? std::min(groups, b.N()) : b.N();
      if (cnt <= 0) continue;
      const double* X = xs_of(i);
      const double* Y = b.Y();
      ImPlot::SetAxes(ImAxis_X1, b.axis == 1 && y2 ? ImAxis_Y2 : ImAxis_Y1);
      const ImU32 col = color_of(i, b);
      const std::vector<model::ChartTrendline> trends = TrendlinesOf(d);
      for (size_t ti = 0; ti < trends.size(); ++ti) {
        const model::ChartTrendline& t = trends[ti];
        ImPlotSpec sp;
        sp.LineColor = ImVec4(0, 0, 0, 0);  // (drawn dotted below; the line keeps an interactive plot fitted)
        sp.LineWeight = 1.5f;
        sp.Flags = ImPlotLineFlags_SkipNaN;
        const std::string label = TrendLegendName(t, b.name) + "##trend" + std::to_string(i) + "_" + std::to_string(ti);
        std::vector<double> tx, ty;
        if (t.kind == model::TrendKind::MovingAverage) {
          ty = engine::MovingAverage(Y, cnt, t.period);
          tx.assign(X, X + cnt);
        } else {
          const engine::TrendFit f = engine::FitTrend(b.X(), Y, cnt, t.kind, t.order, t.intercept_on ? &t.intercept : nullptr);
          if (f.ok) {
            double lo = 1e300, hi = -1e300;
            for (int k = 0; k < cnt; ++k) {
              if (std::isfinite(b.X()[k]) && std::isfinite(Y[k])) lo = std::min(lo, b.X()[k]), hi = std::max(hi, b.X()[k]);
            }
            lo -= std::max(0.0, t.backward);
            hi += std::max(0.0, t.forward);
            if ((t.kind == model::TrendKind::Logarithmic || t.kind == model::TrendKind::Power) && lo <= 0) lo = std::max(hi * 1e-3, 1e-9);
            const double shift = X[0] - b.X()[0];  // the series' drawing offset (columns, combo lines)
            for (int k = 0; k <= 120 && hi >= lo; ++k) {
              const double x = lo + (hi - lo) * k / 120;
              tx.push_back(x + shift);
              ty.push_back(f.Eval(x));
            }
            if ((t.eq || t.r2) && !tx.empty()) {
              std::string txt = t.eq ? f.Equation() : std::string();
              if (t.r2) {
                char r2[64];
                std::snprintf(r2, sizeof r2, "R\xC2\xB2 = %.4f", f.r2);
                txt += (txt.empty() ? "" : "\n") + std::string(r2);
              }
              const double ax = horizontal ? ty.back() : tx.back(), ay = horizontal ? tx.back() : ty.back();
              ImPlot::Annotation(ax, ay, ImVec4(1, 1, 1, 0.85f), ImVec2(-8, -8 - 30.0f * static_cast<float>(ti)), true, "%s", txt.c_str());
            }
          }
        }
        if (!tx.empty()) {
          plot_line(label.c_str(), tx.data(), ty.data(), static_cast<int>(tx.size()), sp);
          // Excel's trendline: a dotted line in the series' color
          std::vector<ImVec2> pts;
          for (size_t k = 0; k < tx.size(); ++k) pts.push_back(std::isfinite(ty[k]) ? px(tx[k], ty[k]) : ImVec2(NAN, NAN));
          DashedPolyline(pdl, pts, col, 1.5f, 2);
          for (size_t k = 0; k < tx.size(); k += 4) {
            if (!std::isfinite(ty[k])) continue;
            hit.trend[i].push_back(px(tx[k], ty[k]));
            if (i < hit.trend_idx.size()) hit.trend_idx[i].push_back(static_cast<int>(ti));
          }
        }
      }
      for (int dir = 0; dir < 2; ++dir) {  // 0: the Y error bars, 1: a scatter chart's X error bars
        if (dir == 0 && d.err == model::ErrorBarKind::None) continue;
        if (dir == 1 && (d.xerr == model::ErrorBarKind::None || !scatter)) continue;
        bool at_mean = false;
        double mean = 0;
        std::vector<double> neg, plus;
        if (dir == 0) engine::ErrorExtents(d, b, cnt, &neg, &plus, &at_mean, &mean);
        else engine::ErrorExtentsX(d, b, cnt, &neg, &plus, &at_mean, &mean);
        std::vector<double> ys(Y, Y + cnt), xs(X, X + cnt);
        if (at_mean) std::fill(dir == 0 ? ys.begin() : xs.begin(), dir == 0 ? ys.end() : xs.end(), mean);
        ImPlotSpec sp;
        sp.LineColor = ImVec4(0.25f, 0.25f, 0.25f, 1);
        sp.LineWeight = 1.0f;
        sp.Size = d.err_cap ? 5.0f : 0.0f;  // End Style: Cap / No Cap
        const bool across = horizontal != (dir == 1);  // the bars run horizontally
        if (across) sp.Flags = ImPlotErrorBarsFlags_Horizontal;
        const std::string eid = (dir ? "##xerr" : "##err") + std::to_string(i);
        if (horizontal) ImPlot::PlotErrorBars(eid.c_str(), ys.data(), xs.data(), neg.data(), plus.data(), cnt, sp);
        else ImPlot::PlotErrorBars(eid.c_str(), xs.data(), ys.data(), neg.data(), plus.data(), cnt, sp);
        auto& out = dir == 0 ? hit.errs : hit.xerrs;
        if (out.size() < ns) out.resize(ns);
        for (int k = 0; k < cnt && k < 2000; ++k) {
          const size_t u = static_cast<size_t>(k);
          if (!std::isfinite(ys[u]) || !std::isfinite(xs[u])) continue;
          const double m0 = u < neg.size() ? neg[u] : 0, m1 = u < plus.size() ? plus[u] : 0;
          const ImVec2 a = dir == 0 ? px(xs[u], ys[u] - m0) : px(xs[u] - m0, ys[u]), c = dir == 0 ? px(xs[u], ys[u] + m1) : px(xs[u] + m1, ys[u]);
          ImRect r(ImMin(a, c), ImMax(a, c));
          r.Expand(S(3));
          out[i].push_back(r);
        }
      }
    }
    ImPlot::EndPlot();
  }
  ImGui::PopFont();
  draw_legend();
  ImGui::PopStyleColor(2);
  ImPlot::PopStyleVar(2);
  ImPlot::PopStyleColor(11);
  ImPlot::PopColormap();
}

namespace {

float SegDist(ImVec2 p, ImVec2 a, ImVec2 b) {
  const ImVec2 ab = b - a, ap = p - a;
  const float l2 = ab.x * ab.x + ab.y * ab.y;
  const float t = l2 > 0 ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
  const ImVec2 q = a + ab * t;
  return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
}

bool InPoly(const std::vector<float>& p, ImVec2 m, ImVec2 o) {
  bool in = false;
  const size_t n = p.size() / 2;
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const float xi = o.x + p[2 * i], yi = o.y + p[2 * i + 1], xj = o.x + p[2 * j], yj = o.y + p[2 * j + 1];
    if ((yi > m.y) != (yj > m.y) && m.x < (xj - xi) * (m.y - yi) / (yj - yi) + xi) in = !in;
  }
  return in;
}

// a scene chart's primitive under the mouse: the topmost one wins
model::ChartElem HitScene(const ChartCache& cc, ImVec2 m, int* series, int* point) {
  using E = model::ChartElem;
  const ImVec2 o = cc.hit.frame.Min;
  const auto& prims = cc.scene.prims;
  for (size_t k = prims.size(); k-- > 0;) {
    const auto& p = prims[k];
    bool hit = false;
    switch (p.kind) {
      case engine::ChartPrim::Text: hit = k < cc.hit.text_boxes.size() && cc.hit.text_boxes[k].Contains(m); break;
      case engine::ChartPrim::Rect: hit = (p.fill || p.stroke) && ImRect(o + ImVec2(p.pts[0], p.pts[1]), o + ImVec2(p.pts[2], p.pts[3])).Contains(m); break;
      case engine::ChartPrim::Circle: {
        const float dx = m.x - (o.x + p.pts[0]), dy = m.y - (o.y + p.pts[1]);
        hit = std::sqrt(dx * dx + dy * dy) <= p.pts[2] + 2;
        break;
      }
      case engine::ChartPrim::Poly:
        if (p.fill && p.pts.size() >= 6) {
          hit = InPoly(p.pts, m, o);
          break;
        }
        [[fallthrough]];
      case engine::ChartPrim::Line:
        for (size_t i = 0; i + 3 < p.pts.size() && !hit; i += 2)
          hit = SegDist(m, o + ImVec2(p.pts[i], p.pts[i + 1]), o + ImVec2(p.pts[i + 2], p.pts[i + 3])) < S(4);
        break;
    }
    if (!hit) continue;
    *series = std::max<int>(0, p.series);
    if (point) *point = p.point;
    return static_cast<E>(std::clamp<int>(p.elem, 1, model::kChartElemLast));
  }
  return E::ChartArea;
}

// the chart element under the mouse (a series: *series is its index; *point the data point under the mouse, or -1)
model::ChartElem HitChartElem(const ChartCache& cc, ImVec2 m, int* series, int* point = nullptr) {
  using E = model::ChartElem;
  const ChartCache::Hit& h = cc.hit;
  int pt_dummy = -1;
  int& pt = point ? *point : pt_dummy;
  pt = -1;
  auto in = [&](const ImRect& r) { return r.GetWidth() > 0 && r.GetHeight() > 0 && r.Contains(m); };
  if (h.scene) return h.frame.Contains(m) ? HitScene(cc, m, series, &pt) : E::ChartArea;
  if (in(h.title)) return E::Title;
  if (in(h.legend)) {  // *point: the legend entry under the mouse (*series: its series, -1 a category)
    for (size_t k = 0; k < h.legend_rects.size() && k < h.legend_series.size(); ++k) {
      if (h.legend_rects[k].Contains(m)) {
        *series = h.legend_series[k];
        // a series' entry: its place in the legend; a category's (a pie's): its category
        pt = h.legend_series[k] < 0 && k < h.legend_cats.size() ? h.legend_cats[k] : static_cast<int>(k);
        break;
      }
    }
    return E::Legend;
  }
  if (in(h.cat_title)) return E::CatAxisTitle;
  if (in(h.val_title)) return E::ValAxisTitle;
  if (in(h.val2_title)) return E::ValAxis2Title;
  // data labels are on top of their points
  for (size_t i = 0; i < h.labels.size(); ++i) {
    for (size_t k = 0; k < h.labels[i].size(); ++k) {
      if (h.labels[i][k].Contains(m)) {
        *series = static_cast<int>(i);
        pt = i < h.label_idx.size() && k < h.label_idx[i].size() ? h.label_idx[i][k] : -1;  // the label's point
        return E::DataLabels;
      }
    }
  }
  for (const auto& sl : h.slices) {
    const float dx = m.x - sl.c.x, dy = m.y - sl.c.y, d = std::sqrt(dx * dx + dy * dy);
    if (d > sl.r1 || d < sl.r0) continue;
    float a = std::atan2(dx, -dy);  // clockwise from 12 o'clock
    constexpr float kTau = 6.28318530718f;
    while (a < sl.a0) a += kTau;
    while (a >= sl.a0 + kTau) a -= kTau;
    if (a <= sl.a1) {
      *series = sl.series;
      pt = sl.point;
      return E::Series;
    }
  }
  if (h.pie_r > 0) return in(h.plot) ? E::PlotArea : E::ChartArea;
  if (in(h.plot)) {
    // a stock chart's up / down bars and high-low lines
    for (const auto& r : h.up_bars) {
      if (r.Contains(m)) return E::UpBars;
    }
    for (const auto& r : h.down_bars) {
      if (r.Contains(m)) return E::DownBars;
    }
    for (size_t k = 0; k + 1 < h.hilo.size(); k += 2) {
      if (SegDist(m, h.hilo[k], h.hilo[k + 1]) < S(3)) return E::HiLoLines;
    }
    for (size_t i = 0; i < h.xerrs.size(); ++i) {
      for (const auto& r : h.xerrs[i]) {
        if (r.Contains(m) && std::fabs(m.y - r.GetCenter().y) < S(3)) {
          *series = static_cast<int>(i);
          pt = 1;  // the X error bars
          return E::ErrorBars;
        }
      }
    }
    for (size_t i = 0; i < h.trend.size(); ++i) {
      const auto& ti = i < h.trend_idx.size() ? h.trend_idx[i] : std::vector<int>();
      for (size_t k = 0; k + 1 < h.trend[i].size(); ++k) {
        if (k + 1 < ti.size() && ti[k] != ti[k + 1]) continue;  // (the next trendline starts there)
        if (SegDist(m, h.trend[i][k], h.trend[i][k + 1]) < S(4)) {
          *series = static_cast<int>(i);
          pt = k < ti.size() ? ti[k] : 0;  // which of the series' trendlines
          return E::Trendline;
        }
      }
    }
    for (size_t i = 0; i < h.errs.size(); ++i) {
      for (const auto& r : h.errs[i]) {
        if (r.Contains(m) && (std::fabs(m.x - r.GetCenter().x) < S(3) || std::fabs(m.y - r.Min.y - S(3)) < S(3) || std::fabs(m.y - r.Max.y + S(3)) < S(3))) {
          *series = static_cast<int>(i);
          return E::ErrorBars;
        }
      }
    }
    for (size_t i = 0; i < h.boxes.size(); ++i) {
      for (size_t k = 0; k < h.boxes[i].size(); ++k) {
        if (h.boxes[i][k].Contains(m)) {
          *series = static_cast<int>(i);
          pt = static_cast<int>(k);
          return E::Series;
        }
      }
    }
    float best = S(5);
    int bi = -1, bk = -1;
    for (size_t i = 0; i < h.lines.size(); ++i) {
      const auto& pts = h.lines[i];
      for (size_t k = 0; k < pts.size(); ++k) {
        const float d = k + 1 < pts.size() ? SegDist(m, pts[k], pts[k + 1]) : SegDist(m, pts[k], pts[k]);
        if (d < best) {
          best = d;
          bi = static_cast<int>(i);
          // the nearer end of the segment is the point
          const float d0 = std::hypot(m.x - pts[k].x, m.y - pts[k].y);
          const float d1 = k + 1 < pts.size() ? std::hypot(m.x - pts[k + 1].x, m.y - pts[k + 1].y) : 1e9f;
          const size_t kk = d1 < d0 ? k + 1 : k;
          bk = kk < h.line_idx[i].size() ? h.line_idx[i][kk] : -1;
        }
      }
    }
    if (bi >= 0) {
      *series = bi;
      pt = bk;
      return E::Series;
    }
    for (float g : h.grid) {
      if (std::fabs((h.grid_vertical ? m.x : m.y) - g) < S(3)) return E::MajorGridlines;
    }
    for (float g : h.minor_grid) {
      if (std::fabs((h.grid_vertical ? m.x : m.y) - g) < S(2)) return E::MinorGridlines;
    }
    return E::PlotArea;
  }
  if (in(h.cat_axis)) return E::CategoryAxis;
  if (in(h.val_axis)) return E::ValueAxis;
  if (in(h.val_axis2)) return E::ValueAxis2;
  return E::ChartArea;
}

// the handle of an element's selection box under the mouse (0 top-left, 1 top, 2 top-right, 3 right, 4 bottom-right,
// 5 bottom, 6 bottom-left, 7 left), -1 for none
int ElemHandleAt(ImRect r, ImVec2 m) {
  if (r.GetWidth() <= 0 || r.GetHeight() <= 0) return -1;
  r.Expand(S(2));
  const ImVec2 pts[8] = {r.Min, ImVec2(r.GetCenter().x, r.Min.y), ImVec2(r.Max.x, r.Min.y), ImVec2(r.Max.x, r.GetCenter().y), r.Max,
                         ImVec2(r.GetCenter().x, r.Max.y), ImVec2(r.Min.x, r.Max.y), ImVec2(r.Min.x, r.GetCenter().y)};
  for (int k = 0; k < 8; ++k) {
    if (std::fabs(m.x - pts[k].x) <= S(5) && std::fabs(m.y - pts[k].y) <= S(5)) return k;
  }
  return -1;
}

// Excel's selection marks: a box with round handles, or squares on a series' points
void DrawChartSelection(ImDrawList* dl, const ChartCache& cc, model::ChartElem e, int series, int point = -1) {
  if (ImGui::GetFrameCount() <= g_marks_hidden_until) return;  // a copied chart's picture is being taken
  using E = model::ChartElem;
  const ChartCache::Hit& h = cc.hit;
  const ImU32 line = IM_COL32(0x60, 0x60, 0x60, 255), fill = IM_COL32_WHITE;
  auto box = [&](ImRect r) {
    if (r.GetWidth() <= 0 || r.GetHeight() <= 0) return;
    r.Expand(S(2));
    ui::NoteUiItem(TR("Selected Chart Element"), r);  // its box and handles, as a person sees them (tests drag them)
    dl->AddRect(r.Min, r.Max, line);
    const ImVec2 pts[8] = {r.Min, ImVec2(r.GetCenter().x, r.Min.y), ImVec2(r.Max.x, r.Min.y), ImVec2(r.Max.x, r.GetCenter().y), r.Max,
                           ImVec2(r.GetCenter().x, r.Max.y), ImVec2(r.Min.x, r.Max.y), ImVec2(r.Min.x, r.GetCenter().y)};
    for (const ImVec2& p : pts) {
      dl->AddCircleFilled(p, S(3), fill);
      dl->AddCircle(p, S(3), line);
    }
  };
  auto mark = [&](ImVec2 p) {
    const float a = S(3);
    dl->AddRectFilled(p - ImVec2(a, a), p + ImVec2(a, a), fill);
    dl->AddRect(p - ImVec2(a, a), p + ImVec2(a, a), line);
  };
  if (h.scene) {
    // scene charts: the box around the element's primitives, or marks on a series' shapes
    if (e == E::ChartArea || e == E::None) return;
    const ImVec2 o = h.frame.Min;
    ImRect bb(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    std::vector<ImVec2> centers;
    const E pe = e == E::Point ? E::Series : e;
    for (size_t k = 0; k < cc.scene.prims.size(); ++k) {
      const auto& p = cc.scene.prims[k];
      if (p.elem != static_cast<int>(pe) || (pe == E::Series && p.series != series)) continue;
      if (e == E::Point && p.point != point) continue;
      ImRect r(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
      if (p.kind == engine::ChartPrim::Text) {
        if (k < h.text_boxes.size()) r = h.text_boxes[k];
      } else if (p.kind == engine::ChartPrim::Circle) {
        r = ImRect(o + ImVec2(p.pts[0] - p.pts[2], p.pts[1] - p.pts[2]), o + ImVec2(p.pts[0] + p.pts[2], p.pts[1] + p.pts[2]));
      } else {
        for (size_t i = 0; i + 1 < p.pts.size(); i += 2) r.Add(o + ImVec2(p.pts[i], p.pts[i + 1]));
      }
      if (r.Min.x > r.Max.x) continue;
      bb.Add(r);
      if (p.kind != engine::ChartPrim::Text) centers.push_back(r.GetCenter());
    }
    if (bb.Min.x > bb.Max.x) return;
    if (e == E::Point) {
      box(bb);
      return;
    }
    if (e != E::Series) {
      box(bb);
      return;
    }
    const size_t step = std::max<size_t>(1, centers.size() / 60);
    for (size_t k = 0; k < centers.size(); k += step) mark(centers[k]);
    return;
  }
  auto slice_outline = [&](const ChartCache::Hit::Slice& sl, float th) {
    const int segs = std::max(4, static_cast<int>((sl.a1 - sl.a0) / 6.2832f * 96));
    std::vector<ImVec2> pts;
    auto at = [&](float r, float a) { return ImVec2(sl.c.x + r * std::sin(a), sl.c.y - r * std::cos(a)); };
    for (int q = 0; q <= segs; ++q) pts.push_back(at(sl.r1, sl.a0 + (sl.a1 - sl.a0) * static_cast<float>(q) / static_cast<float>(segs)));
    if (sl.r0 > 0) {
      for (int q = segs; q >= 0; --q) pts.push_back(at(sl.r0, sl.a0 + (sl.a1 - sl.a0) * static_cast<float>(q) / static_cast<float>(segs)));
    } else {
      pts.push_back(sl.c);
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), line, th, ImDrawFlags_Closed);
  };
  switch (e) {
    case E::Title: box(h.title); break;
    case E::Legend: box(h.legend); break;
    case E::PlotArea: box(h.plot); break;
    case E::CategoryAxis: box(h.cat_axis); break;
    case E::ValueAxis: box(h.val_axis); break;
    case E::ValueAxis2: box(h.val_axis2); break;
    case E::CatAxisTitle: box(h.cat_title); break;
    case E::ValAxisTitle: box(h.val_title); break;
    case E::ValAxis2Title: box(h.val2_title); break;
    case E::MajorGridlines:
      for (float g : h.grid) {
        mark(h.grid_vertical ? ImVec2(g, h.plot.Min.y) : ImVec2(h.plot.Min.x, g));
        mark(h.grid_vertical ? ImVec2(g, h.plot.Max.y) : ImVec2(h.plot.Max.x, g));
      }
      break;
    case E::DataLabels:
      if (series >= 0 && static_cast<size_t>(series) < h.labels.size()) {
        for (const auto& r : h.labels[static_cast<size_t>(series)]) dl->AddRect(r.Min - ImVec2(1, 1), r.Max + ImVec2(1, 1), line);
      }
      break;
    case E::DataLabel:  // one label: a box with handles
      if (series >= 0 && static_cast<size_t>(series) < h.labels.size()) {
        const size_t i = static_cast<size_t>(series);
        for (size_t k = 0; k < h.labels[i].size() && k < h.label_idx[i].size(); ++k) {
          if (h.label_idx[i][k] == point) box(h.labels[i][k]);
        }
      }
      break;
    case E::LegendEntry:
      box(h.legend);
      for (size_t k = 0; k < h.legend_rects.size() && k < h.legend_series.size(); ++k) {
        const bool me = series >= 0 ? h.legend_series[k] == series : (h.legend_series[k] < 0 && k < h.legend_cats.size() && h.legend_cats[k] == point);
        if (!me) continue;
        const ImRect& r = h.legend_rects[k];
        dl->AddRect(r.Min - ImVec2(1, 1), r.Max + ImVec2(1, 1), line);
        break;
      }
      break;
    case E::MinorGridlines:
      for (float g : h.minor_grid) {
        mark(h.grid_vertical ? ImVec2(g, h.plot.Min.y) : ImVec2(h.plot.Min.x, g));
        mark(h.grid_vertical ? ImVec2(g, h.plot.Max.y) : ImVec2(h.plot.Max.x, g));
      }
      break;
    case E::Trendline:
      if (series >= 0 && static_cast<size_t>(series) < h.trend.size()) {
        // the ends of the selected trendline (point: which of the series' trendlines)
        const auto& t = h.trend[static_cast<size_t>(series)];
        const auto& ti = static_cast<size_t>(series) < h.trend_idx.size() ? h.trend_idx[static_cast<size_t>(series)] : std::vector<int>();
        const int want = std::max(0, point);
        int first = -1, last = -1;
        for (size_t k = 0; k < t.size(); ++k) {
          if (k < ti.size() ? ti[k] == want : want == 0) {
            if (first < 0) first = static_cast<int>(k);
            last = static_cast<int>(k);
          }
        }
        if (first >= 0) mark(t[static_cast<size_t>(first)]), mark(t[static_cast<size_t>(last)]);
      }
      break;
    case E::UpBars:
    case E::DownBars: {
      const auto& bars = e == E::UpBars ? h.up_bars : h.down_bars;
      const size_t step = std::max<size_t>(1, bars.size() / 40);
      for (size_t k = 0; k < bars.size(); k += step) mark(bars[k].GetCenter());
      break;
    }
    case E::HiLoLines: {
      const size_t step = std::max<size_t>(1, h.hilo.size() / 80) * 2;
      for (size_t k = 0; k + 1 < h.hilo.size(); k += step) mark(h.hilo[k]), mark(h.hilo[k + 1]);
      break;
    }
    case E::ErrorBars:
      if (point == 1 && series >= 0 && static_cast<size_t>(series) < h.xerrs.size()) {  // a scatter chart's X error bars
        const auto& t = h.xerrs[static_cast<size_t>(series)];
        const size_t step = std::max<size_t>(1, t.size() / 40);
        for (size_t k = 0; k < t.size(); k += step) mark(ImVec2(t[k].Min.x + S(3), t[k].GetCenter().y));
      } else if (series >= 0 && static_cast<size_t>(series) < h.errs.size()) {
        const auto& t = h.errs[static_cast<size_t>(series)];
        const size_t step = std::max<size_t>(1, t.size() / 40);
        for (size_t k = 0; k < t.size(); k += step) mark(ImVec2(t[k].GetCenter().x, t[k].Min.y + S(3)));
      }
      break;
    case E::Point: {
      for (const auto& sl : h.slices) {
        if (sl.series == series && sl.point == point) slice_outline(sl, 2.0f);
      }
      if (series >= 0 && static_cast<size_t>(series) < h.boxes.size() && point >= 0 && static_cast<size_t>(point) < h.boxes[static_cast<size_t>(series)].size()) {
        box(h.boxes[static_cast<size_t>(series)][static_cast<size_t>(point)]);
      } else if (series >= 0 && static_cast<size_t>(series) < h.lines.size()) {
        const auto& idx = h.line_idx[static_cast<size_t>(series)];
        for (size_t k = 0; k < idx.size(); ++k) {
          if (idx[k] == point) {
            const ImVec2 p = h.lines[static_cast<size_t>(series)][k];
            box(ImRect(p - ImVec2(S(4), S(4)), p + ImVec2(S(4), S(4))));
          }
        }
      }
      break;
    }
    case E::Series: {
      bool any = false;
      for (const auto& sl : h.slices) {
        if (sl.series == series) {
          const ImVec2 p(sl.c.x + sl.r1 * 0.75f * std::sin((sl.a0 + sl.a1) * 0.5f), sl.c.y - sl.r1 * 0.75f * std::cos((sl.a0 + sl.a1) * 0.5f));
          mark(p);
          any = true;
        }
      }
      if (any || series < 0) break;
      const size_t i = static_cast<size_t>(series);
      if (i < h.boxes.size()) {
        const auto& b = h.boxes[i];
        const size_t step = std::max<size_t>(1, b.size() / 60);
        for (size_t k = 0; k < b.size(); k += step) mark(b[k].GetCenter());
      }
      if (i < h.lines.size()) {
        const auto& l = h.lines[i];
        const size_t step = std::max<size_t>(1, l.size() / 40);
        for (size_t k = 0; k < l.size(); k += step) mark(l[k]);
      }
      break;
    }
    default: break;
  }
}

const char* LegendPosName(int p) {
  switch (p) {
    case 1: return TR("Right");
    case 2: return TR("Top");
    case 3: return TR("Left");
    case 4: return TR("Top Right");
    default: return TR("Bottom");
  }
}

// "Chart Title", "Series \"Sales\"" (the Format tab's Chart Elements list, the Format pane's title)
std::string ChartElemName(const ChartObj& ch, model::ChartElem e, int series, int point = -1);

// File > Options > Advanced > Chart: the element under the mouse (Show chart element names on hover) and a point's
// value (Show data point values on hover), shown as a tip while the mouse rests on a chart
void ChartTip(AppState& s, const ChartObj& ch, const ChartCache& cc, ImVec2 m) {
  const Settings& c = app::Config();
  if (!c.show_chart_tip_names && !c.show_chart_tip_values) return;
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
  int es = -1, ep = -1;
  model::ChartElem e = HitChartElem(cc, m, &es, &ep);
  using E = model::ChartElem;
  const bool point = (e == E::Series || e == E::Point || e == E::DataLabels) && es >= 0 && ep >= 0 && static_cast<size_t>(es) < cc.v.series.size();
  std::string tip;
  if (c.show_chart_tip_names) {
    if (point) {
      const std::string cat = static_cast<size_t>(ep) < cc.v.cats.size() && !cc.v.cats[static_cast<size_t>(ep)].empty() ? cc.v.cats[static_cast<size_t>(ep)] : std::to_string(ep + 1);
      tip = ChartElemName(ch, E::Series, es) + " " + TR("Point") + " \"" + cat + "\"";
    } else if (e != E::ChartArea || ImRect(cc.hit.frame).Contains(m)) {
      tip = ChartElemName(ch, e, es, ep);
    }
  }
  if (c.show_chart_tip_values && point) {
    const engine::ChartSeriesValues& sv = cc.v.series[static_cast<size_t>(es)];
    if (static_cast<size_t>(ep) < sv.y.size() && std::isfinite(sv.y[static_cast<size_t>(ep)])) {
      const std::string v = formula::FormatValue(core::Value::Number(sv.y[static_cast<size_t>(ep)]), sv.fmt.empty() ? std::string_view("General") : std::string_view(sv.fmt)).text;
      tip += (tip.empty() ? "" : "\n") + std::string(TR("Value: ")) + v;
    }
  }
  if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
  s.chart_tip = tip;
}

std::string ChartElemName(const ChartObj& ch, model::ChartElem e, int series, int point) {
  using E = model::ChartElem;
  const bool bar = engine::IsHorizontalBar(ch.type);
  auto series_name = [&]() {
    std::string n = series >= 0 && static_cast<size_t>(series) < ch.series.size() ? ch.series[static_cast<size_t>(series)].name : std::string();
    if (!n.empty() && n[0] == '=') {  // a cell reference: the name as the chart shows it
      const auto it = U().charts.find(ch.id);
      n = it != U().charts.end() && static_cast<size_t>(series) < it->second.v.series.size() ? it->second.v.series[static_cast<size_t>(series)].name : std::string();
    }
    if (n.empty()) n = TR("Series") + std::to_string(series + 1);
    return std::string(TR("Series")) + " \"" + n + "\"";
  };
  switch (e) {
    case E::PlotArea: return TR("Plot Area");
    case E::Title: return TR("Chart Title");
    case E::Legend: return TR("Legend");
    case E::CategoryAxis:
      if (engine::IsScatterType(ch.type)) return TR("Horizontal (Value) Axis");  // a scatter chart's X axis holds values
      return bar ? TR("Vertical (Category) Axis") : TR("Horizontal (Category) Axis");
    case E::ValueAxis: return bar ? TR("Horizontal (Value) Axis") : TR("Vertical (Value) Axis");
    case E::ValueAxis2: return TR("Secondary Vertical (Value) Axis");
    case E::CatAxisTitle: return bar ? TR("Vertical (Category) Axis Title") : TR("Horizontal (Category) Axis Title");
    case E::ValAxisTitle: return bar ? TR("Horizontal (Value) Axis Title") : TR("Vertical (Value) Axis Title");
    case E::ValAxis2Title: return TR("Secondary Vertical (Value) Axis Title");
    case E::MajorGridlines: return bar ? TR("Horizontal (Value) Axis Major Gridlines") : TR("Vertical (Value) Axis Major Gridlines");
    case E::MinorGridlines: return bar ? TR("Horizontal (Value) Axis Minor Gridlines") : TR("Vertical (Value) Axis Minor Gridlines");
    case E::LegendEntry: return TR("Legend Entry");
    case E::DataLabel: return series_name() + " " + TR("Point") + " \"" + std::to_string(point + 1) + "\" " + TR("Data Label");
    case E::Series: return series_name();
    case E::Point: return series_name() + " " + TR("Point") + " \"" + std::to_string(point + 1) + "\"";
    case E::DataLabels: return series_name() + " " + TR("Data Labels");
    case E::Trendline: return series_name() + " " + TR("Trendline");
    case E::ErrorBars: return series_name() + " " + (point == 1 ? TR("X Error Bars") : TR("Error Bars"));
    case E::UpBars: return TR("Up Bars");
    case E::DownBars: return TR("Down Bars");
    case E::HiLoLines: return TR("High-Low Lines");
    case E::Floor: return TR("Floor");
    case E::Walls: return TR("Walls");
    default: return TR("Chart Area");
  }
}

// the Format pane's title for an element ("Format Data Point", "Format Data Series", "Format Axis"...): Excel's words
std::string FormatPaneName(const ChartObj& ch, model::ChartElem e, int series, int point) {
  using E = model::ChartElem;
  switch (e) {
    case E::Series: return TR("Format Data Series");
    case E::Point: return TR("Format Data Point");
    case E::DataLabels: return TR("Format Data Labels");
    case E::Trendline: return TR("Format Trendline");
    case E::ErrorBars: return TR("Format Error Bars");
    case E::MajorGridlines: return TR("Format Major Gridlines");
    case E::MinorGridlines: return TR("Format Minor Gridlines");
    case E::LegendEntry: return TR("Format Legend Entry");
    case E::DataLabel: return TR("Format Data Label");
    case E::CatAxisTitle:
    case E::ValAxisTitle:
    case E::ValAxis2Title: return TR("Format Axis Title");
    default: return std::string(TR("Format ")) + ChartElemName(ch, e, series, point);
  }
}

// the element being formatted for chart o (another selection: its chart area)
model::ChartElem ElemOf(ObjSel o, int* series, int* point = nullptr) {
  const UiState& u = U();
  *series = u.elem_of == o ? u.elem_series : 0;
  if (point) *point = u.elem_of == o ? u.elem_point : -1;
  return u.elem_of == o ? u.elem : model::ChartElem::ChartArea;
}

void SelectElem(ObjSel o, model::ChartElem e, int series, int point = -1) {
  UiState& u = U();
  u.sel = o;
  u.elem_of = o;
  u.elem = e;
  u.elem_series = series;
  u.elem_point = point;
}

// ---- Object geometry ----

ImRect ScreenRect(const render::OverlayGeom& g, Placement pl, const Geom& m) {
  ImVec2 a;
  if (pl == Placement::FloatWindow) {
    a = ImVec2(g.cells.Min.x + static_cast<float>(m.l * kPx * g.sc), g.cells.Min.y + static_cast<float>(m.t * kPx * g.sc));
  } else {
    a = g.to_screen(m.l * kPx, m.t * kPx);
  }
  a = ImFloor(a);
  return ImRect(a, ImFloor(ImVec2(a.x + static_cast<float>(m.w * kPx * g.sc), a.y + static_cast<float>(m.h * kPx * g.sc))));
}

Geom GeomOf(const Control& c) { return Geom{c.left, c.top, c.width, c.height}; }
Geom GeomOf(const ChartObj& c) { return Geom{c.left, c.top, c.width, c.height}; }
Geom GeomOf(const ShapeObj& c) { return Geom{c.left, c.top, c.width, c.height}; }

const ShapeObj* FindShape(const model::Sheet& sh, uint32_t id) {
  for (const auto& x : *sh.shapes) {
    if (x.id == id) return &x;
  }
  return nullptr;
}

ImU32 Argb(uint32_t c) { return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) ? (c >> 24) & 0xFF : 255); }

ImTextureID TextureOf(const ShapeObj& sh) {
  UiState& u = U();
  const size_t h = std::hash<std::string>{}(sh.image);
  auto& t = u.textures[sh.id];
  if (t.hash != h || !t.tex) {
    if (t.tex) ReleaseTexture(t.tex);
    t.tex = TextureFromImage(sh.image);
    t.hash = h;
  }
  return t.tex;
}

bool HasText(const ShapeObj& sh) {
  return sh.kind != ShapeKind::Picture && sh.kind != ShapeKind::Line && sh.kind != ShapeKind::Arrow && (sh.path.empty() || sh.path_closed);
}

/// rotate the point around c by deg degrees clockwise (screen y points down)
ImVec2 RotPt(ImVec2 p, ImVec2 c, double deg) {
  if (deg == 0) return p;
  const double a = deg * 3.14159265358979323846 / 180.0, cs = std::cos(a), sn = std::sin(a);
  const double dx = p.x - c.x, dy = p.y - c.y;
  return ImVec2(static_cast<float>(c.x + dx * cs - dy * sn), static_cast<float>(c.y + dx * sn + dy * cs));
}

// the screen box around r turned by deg
ImRect RotBounds(const ImRect& r, double deg) {
  if (deg == 0) return r;
  const ImVec2 c = r.GetCenter();
  ImRect b(RotPt(r.Min, c, deg), RotPt(r.Min, c, deg));
  for (const ImVec2& p : {ImVec2(r.Max.x, r.Min.y), r.Max, ImVec2(r.Min.x, r.Max.y)}) b.Add(RotPt(p, c, deg));
  return b;
}

bool IsLine(const ShapeObj& sh) { return sh.kind == ShapeKind::Line || sh.kind == ShapeKind::Arrow; }

// a line's two ends on screen (begin, end)
void LineEnds(const ShapeObj& sh, const ImRect& r, ImVec2* a, ImVec2* b) {
  *a = ImVec2(sh.flip_h ? r.Max.x : r.Min.x, sh.flip_v ? r.Max.y : r.Min.y);
  *b = ImVec2(sh.flip_h ? r.Min.x : r.Max.x, sh.flip_v ? r.Min.y : r.Max.y);
}

// the outline of a shape in r (not rotated, not flipped): a closed polygon (lines: the two ends)
std::vector<ImVec2> ShapeOutline(ImDrawList* dl, const ShapeObj& sh, const ImRect& r) {
  std::vector<ImVec2> pts;
  const ImVec2 c = r.GetCenter();
  switch (sh.kind) {
    case ShapeKind::Oval: {
      const int n = 48;
      for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / n;
        pts.push_back(ImVec2(c.x + std::cos(a) * r.GetWidth() * 0.5f, c.y + std::sin(a) * r.GetHeight() * 0.5f));
      }
      break;
    }
    case ShapeKind::RoundedRectangle: {
      const float rad = std::min(r.GetWidth(), r.GetHeight()) * 0.16f;
      dl->PathRect(r.Min, r.Max, rad);
      pts.assign(dl->_Path.Data, dl->_Path.Data + dl->_Path.Size);
      dl->PathClear();
      break;
    }
    case ShapeKind::Triangle: pts = {ImVec2(c.x, r.Min.y), r.Max, ImVec2(r.Min.x, r.Max.y)}; break;
    case ShapeKind::RightArrow: {
      const float w = r.GetWidth(), h = r.GetHeight(), head = std::min(w * 0.45f, h);
      pts = {ImVec2(r.Min.x, r.Min.y + h * 0.25f), ImVec2(r.Max.x - head, r.Min.y + h * 0.25f), ImVec2(r.Max.x - head, r.Min.y), ImVec2(r.Max.x, c.y),
             ImVec2(r.Max.x - head, r.Max.y),      ImVec2(r.Max.x - head, r.Max.y - h * 0.25f), ImVec2(r.Min.x, r.Max.y - h * 0.25f)};
      break;
    }
    case ShapeKind::Line:
    case ShapeKind::Arrow: {
      ImVec2 a, b;
      LineEnds(sh, r, &a, &b);
      pts = {a, b};
      break;
    }
    default: pts = {r.Min, ImVec2(r.Max.x, r.Min.y), r.Max, ImVec2(r.Min.x, r.Max.y)}; break;
  }
  return pts;
}

// a polyline drawn with the shape's dash style (0 solid, 1 dash, 2 round dot, 3 square dot, 4 dash dot, 5 long dash)
void DashedPolyline(ImDrawList* dl, const std::vector<ImVec2>& pts, bool closed, ImU32 col, float lw, int dash) {
  if (pts.size() < 2) return;
  if (dash <= 0) {
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, lw, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
    return;
  }
  // pattern lengths in line widths: on, off, on, off...
  static const std::vector<float> kPat[] = {{}, {4, 3}, {1, 1}, {1, 1}, {4, 3, 1, 3}, {8, 3}};
  const std::vector<float>& pat = kPat[std::clamp(dash, 1, 5)];
  const float unit = std::max(1.0f, lw);
  size_t k = 0;
  float left = pat[0] * unit;
  bool on = true;
  const size_t n = pts.size(), segs = closed ? n : n - 1;
  for (size_t i = 0; i < segs; ++i) {
    ImVec2 a = pts[i];
    const ImVec2 b = pts[(i + 1) % n];
    float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    if (len <= 0) continue;
    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
    while (len > 0) {
      const float step = std::min(left, len);
      const ImVec2 e(a.x + d.x * step, a.y + d.y * step);
      if (on) dl->AddLine(a, e, col, lw);
      a = e;
      len -= step;
      left -= step;
      if (left <= 0.001f) {
        k = (k + 1) % pat.size();
        left = pat[k] * unit;
        on = !on;
      }
    }
  }
}

void ArrowHead(ImDrawList* dl, ImVec2 from, ImVec2 tip, float lw, ImU32 col) {
  const float len = std::sqrt((tip.x - from.x) * (tip.x - from.x) + (tip.y - from.y) * (tip.y - from.y));
  if (len <= 1) return;
  const ImVec2 u2((tip.x - from.x) / len, (tip.y - from.y) / len), n(-u2.y, u2.x);
  const float hl = std::max(6.0f, lw * 4), hw = hl * 0.5f;
  dl->AddTriangleFilled(tip, ImVec2(tip.x - u2.x * hl + n.x * hw, tip.y - u2.y * hl + n.y * hw), ImVec2(tip.x - u2.x * hl - n.x * hw, tip.y - u2.y * hl - n.y * hw), col);
}

// the font a shape's text is drawn with: the cell font of its face, bold and italic (as the printed page)
ImFont* ShapeFont(const ShapeObj& sh) { return ui::CellFont(sh.font_name, sh.bold, sh.italic); }

// a fill color made darker (shade < 0) or lighter (shade > 0), as Excel shades a cube's or a can's faces
ImU32 Shade(ImU32 c, int shade) {
  if (!shade) return c;
  const float k = shade < 0 ? 1.0f + 0.2f * static_cast<float>(shade) : 0.0f;
  auto ch = [&](int sh) {
    const float v = static_cast<float>((c >> sh) & 0xFF);
    return static_cast<ImU32>(shade < 0 ? v * k : v + (255 - v) * 0.3f) << sh;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

// a polygon for ImGui's filler: no repeated points, not closed on itself
std::vector<ImVec2> CleanPoly(const std::vector<engine::ShapePt>& v, ImVec2 o) {
  std::vector<ImVec2> out;
  for (const auto& q : v) {
    const ImVec2 p(o.x + static_cast<float>(q.x), o.y + static_cast<float>(q.y));
    if (out.empty() || std::fabs(out.back().x - p.x) + std::fabs(out.back().y - p.y) > 0.05f) out.push_back(p);
  }
  while (out.size() > 2 && std::fabs(out.back().x - out.front().x) + std::fabs(out.back().y - out.front().y) <= 0.05f) out.pop_back();
  return out;
}

// a preset shape's or a freeform's drawing in r (screen; not flipped, not turned): false when sh has none
bool ShapeGeomIn(const ShapeObj& sh, const ImRect& r, engine::ShapeGeom* geo) {
  if (sh.auto_type == 0 && sh.path.empty()) return false;
  if (r.GetWidth() < 0.5f && r.GetHeight() < 0.5f) return false;
  return engine::ShapeObjGeometry(sh, r.GetWidth(), r.GetHeight(), geo);
}

// a connector's path on screen (straight: its two ends)
std::vector<ImVec2> ConnectorScreen(const ShapeObj& sh, ImVec2 a, ImVec2 b) {
  std::vector<ImVec2> v;
  for (const auto& q : engine::ConnectorPath(sh.connector, {a.x, a.y}, {b.x, b.y})) v.push_back(ImVec2(static_cast<float>(q.x), static_cast<float>(q.y)));
  return v;
}

// a picture, shape or text box in r (screen); scale = screen pixels per sheet pixel
void DrawShape(ImDrawList* dl, const ShapeObj& sh, const ImRect& r, float scale) {
  const int v0 = dl->VtxBuffer.Size;
  ImU32 fill = sh.fill_visible ? Argb(sh.fill_color) : 0;
  if (fill && sh.fill_transparency > 0) {
    const float a = static_cast<float>((fill >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(1.0f - sh.fill_transparency, 0.0f, 1.0f);
    fill = (fill & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
  }
  const ImU32 line = sh.line_visible ? Argb(sh.line_color) : 0;
  const float lw = std::max(1.0f, sh.line_weight * static_cast<float>(kPx) * scale);
  const ImVec2 c = r.GetCenter();
  const ImRect clip = RotBounds(r, sh.rotation);
  if (sh.kind == ShapeKind::Picture) {
    const ImTextureID tex = TextureOf(sh);
    if (tex) {
      const double ow = sh.orig_w > 0 ? sh.orig_w : sh.width, oh = sh.orig_h > 0 ? sh.orig_h : sh.height;
      const ImVec2 uv0(static_cast<float>(std::clamp(sh.crop_l / ow, 0.0, 0.99)), static_cast<float>(std::clamp(sh.crop_t / oh, 0.0, 0.99)));
      const ImVec2 uv1(static_cast<float>(std::clamp(1.0 - sh.crop_r / ow, 0.01, 1.0)), static_cast<float>(std::clamp(1.0 - sh.crop_b / oh, 0.01, 1.0)));
      dl->AddImage(ImTextureRef(tex), r.Min, r.Max, ImVec2(sh.flip_h ? uv1.x : uv0.x, sh.flip_v ? uv1.y : uv0.y),
                   ImVec2(sh.flip_h ? uv0.x : uv1.x, sh.flip_v ? uv0.y : uv1.y));
    } else {
      dl->AddRectFilled(r.Min, r.Max, IM_COL32(0xF2, 0xF2, 0xF2, 255));
      dl->AddLine(r.Min, r.Max, IM_COL32(0xA0, 0xA0, 0xA0, 255));
      dl->AddLine(ImVec2(r.Min.x, r.Max.y), ImVec2(r.Max.x, r.Min.y), IM_COL32(0xA0, 0xA0, 0xA0, 255));
    }
    if (sh.line_visible && sh.line_color != 0xFF2F528F) DashedPolyline(dl, ShapeOutline(dl, sh, r), true, line, lw, sh.line_dash);
  } else if (IsLine(sh)) {
    ImVec2 a, b;
    LineEnds(sh, r, &a, &b);
    const ImU32 col = line ? line : IM_COL32(0x44, 0x72, 0xC4, 255);
    // an elbow or curved connector follows its path; the heads point along its last pieces
    const std::vector<ImVec2> path = sh.connector >= 2 ? ConnectorScreen(sh, a, b) : std::vector<ImVec2>{a, b};
    DashedPolyline(dl, path, false, col, lw, sh.line_dash);
    if (sh.kind == ShapeKind::Arrow || sh.end_arrow) ArrowHead(dl, path[path.size() - 2], path.back(), lw, col);
    if (sh.begin_arrow) ArrowHead(dl, path[1], path.front(), lw, col);
  } else if (engine::ShapeGeom geo; ShapeGeomIn(sh, r, &geo)) {
    for (const auto& f : geo.fills) {
      if (!fill) break;
      const std::vector<ImVec2> pts = CleanPoly(f.pts, r.Min);
      if (pts.size() >= 3) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), Shade(fill, f.shade));
    }
    for (const auto& st : geo.strokes) {
      if (!line) break;
      std::vector<ImVec2> pts;
      for (const auto& q : st.pts) pts.push_back(ImVec2(r.Min.x + static_cast<float>(q.x), r.Min.y + static_cast<float>(q.y)));
      DashedPolyline(dl, pts, st.closed, line, lw, sh.line_dash);
    }
    // an open freeform takes arrowheads as a line does
    if (!sh.path.empty() && !sh.path_closed && line && !geo.strokes.empty() && geo.strokes[0].pts.size() >= 2) {
      const auto& v = geo.strokes[0].pts;
      auto at = [&](size_t i) { return ImVec2(r.Min.x + static_cast<float>(v[i].x), r.Min.y + static_cast<float>(v[i].y)); };
      if (sh.end_arrow) ArrowHead(dl, at(v.size() - 2), at(v.size() - 1), lw, line);
      if (sh.begin_arrow) ArrowHead(dl, at(1), at(0), lw, line);
    }
    if (sh.flip_h || sh.flip_v) {
      for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        if (sh.flip_h) p.x = c.x * 2 - p.x;
        if (sh.flip_v) p.y = c.y * 2 - p.y;
      }
    }
  } else {
    const std::vector<ImVec2> pts = ShapeOutline(dl, sh, r);
    if (fill) {
      if (sh.kind == ShapeKind::RightArrow) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), fill);
      else dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), fill);
    }
    if (line) DashedPolyline(dl, pts, true, line, lw, sh.line_dash);
    // Flip Horizontal / Vertical mirror the outline (not the text)
    if (sh.flip_h || sh.flip_v) {
      for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& p = dl->VtxBuffer[i].pos;
        if (sh.flip_h) p.x = c.x * 2 - p.x;
        if (sh.flip_v) p.y = c.y * 2 - p.y;
      }
    }
  }
  if (!sh.text.empty() && HasText(sh)) {
    ImFont* font = ShapeFont(sh);
    const float k = static_cast<float>(kPx) * scale;
    const float px = std::max(6.0f, sh.font_size * k);
    const float pl = static_cast<float>(sh.margin_l) * k, pr = static_cast<float>(sh.margin_r) * k, pt = static_cast<float>(sh.margin_t) * k,
                pb = static_cast<float>(sh.margin_b) * k;
    const float wrap = sh.wrap ? std::max(1.0f, r.GetWidth() - pl - pr) : 0.0f;
    const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, wrap, sh.text.c_str());
    const float y = sh.valign == 0 ? r.Min.y + pt : sh.valign == 2 ? r.Max.y - pb - ts.y : (r.Min.y + pt + r.Max.y - pb) * 0.5f - ts.y * 0.5f;
    dl->PushClipRect(clip.Min, clip.Max, true);
    // each line on its own, placed by the alignment (ImGui's own wrap puts every line at the left)
    const char* p = sh.text.c_str();
    const char* end = p + sh.text.size();
    float ly = y;
    const float lh = font->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
    while (p < end) {
      const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
      const char* le = nl ? nl : end;
      const char* e = wrap > 0 ? font->CalcWordWrapPosition(px, p, le, wrap) : le;
      if (e <= p) e = std::min(le, p + 1);
      const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, p, e).x;
      const float lx = sh.halign == 0 ? r.Min.x + pl : sh.halign == 2 ? r.Max.x - pr - w : (r.Min.x + pl + r.Max.x - pr) * 0.5f - w * 0.5f;
      const ImVec2 tp(std::floor(lx), std::floor(ly));
      // WordArt: a soft shadow below right, then the letters' outline around them
      if (sh.text_shadow) dl->AddText(font, px, tp + ImVec2(std::max(1.0f, px * 0.04f), std::max(1.0f, px * 0.04f)), IM_COL32(0, 0, 0, 90), p, e);
      if (sh.text_outline) {
        const float o = std::max(1.0f, px * 0.02f);
        for (const ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(-o, -o), ImVec2(o, o), ImVec2(-o, o), ImVec2(o, -o)})
          dl->AddText(font, px, tp + d, Argb(sh.text_outline), p, e);
      }
      dl->AddText(font, px, tp, Argb(sh.font_color), p, e);
      ly += lh;
      p = e;
      if (p < end && *p == '\n') {
        ++p;
      } else {
        while (p < le && *p == ' ') ++p;
      }
    }
    dl->PopClipRect();
  }
  if (sh.rotation != 0) {
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) dl->VtxBuffer[i].pos = RotPt(dl->VtxBuffer[i].pos, c, sh.rotation);
  }
}

// Resize shape to fit text: the height its text needs (at 100%, in points)
void FitShapeToText(ShapeObj& sh) {
  if (!sh.autofit || !HasText(sh)) return;
  ImFont* font = ShapeFont(sh);
  const float k = static_cast<float>(kPx);
  const float wrap = sh.wrap ? std::max(1.0f, static_cast<float>((sh.width - sh.margin_l - sh.margin_r) * kPx)) : 0.0f;
  const float th = sh.text.empty() ? sh.font_size * k : font->CalcTextSizeA(sh.font_size * k, FLT_MAX, wrap, sh.text.c_str()).y;
  sh.height = std::max(6.0, th / kPx + sh.margin_t + sh.margin_b);
}

// whether the mouse is on the shape itself, not just inside its box: lines within 4 px, ovals inside the ellipse,
// triangles and arrows inside their outline
bool HitShape(const ShapeObj& sh, const ImRect& r, ImVec2 m) {
  const ImVec2 c = r.GetCenter();
  const ImVec2 q = RotPt(m, c, -sh.rotation);
  if (IsLine(sh)) {
    ImVec2 a, b;
    LineEnds(sh, r, &a, &b);
    if (sh.connector >= 2) {
      const std::vector<ImVec2> v = ConnectorScreen(sh, a, b);
      for (size_t i = 0; i + 1 < v.size(); ++i) {
        if (SegDist(q, v[i], v[i + 1]) <= S(4)) return true;
      }
      return false;
    }
    return SegDist(q, a, b) <= S(4);
  }
  if (engine::ShapeGeom geo; ShapeGeomIn(sh, r, &geo)) {
    // a preset shape or a freeform: on its filled area (when it has a fill) or near its lines (callout leaders, arcs)
    ImVec2 l(q.x - r.Min.x, q.y - r.Min.y);
    if (sh.flip_h) l.x = r.GetWidth() - l.x;
    if (sh.flip_v) l.y = r.GetHeight() - l.y;
    if (engine::InShapeGeometry(geo, {l.x, l.y})) return true;
    for (const auto& st : geo.strokes) {
      const size_t n = st.pts.size();
      for (size_t i = 0; i + 1 < n + (st.closed ? 1 : 0); ++i) {
        const auto& a = st.pts[i];
        const auto& b = st.pts[(i + 1) % n];
        if (SegDist(l, ImVec2(static_cast<float>(a.x), static_cast<float>(a.y)), ImVec2(static_cast<float>(b.x), static_cast<float>(b.y))) <= S(4)) return true;
      }
    }
    return false;
  }
  if (!r.Contains(q)) return false;
  if (sh.kind == ShapeKind::Oval) {
    const float rx = std::max(1.0f, r.GetWidth() * 0.5f), ry = std::max(1.0f, r.GetHeight() * 0.5f);
    const float dx = (q.x - c.x) / rx, dy = (q.y - c.y) / ry;
    return dx * dx + dy * dy <= 1.05f;
  }
  if (sh.kind == ShapeKind::Triangle || sh.kind == ShapeKind::RightArrow) {
    std::vector<ImVec2> pts = ShapeOutline(ImGui::GetWindowDrawList(), sh, r);
    std::vector<float> f;
    for (ImVec2 p : pts) {
      if (sh.flip_h) p.x = c.x * 2 - p.x;
      if (sh.flip_v) p.y = c.y * 2 - p.y;
      f.push_back(p.x);
      f.push_back(p.y);
    }
    return InPoly(f, q, ImVec2(0, 0));
  }
  return true;
}

void CommitGeom(AppState& s, ObjSel o, Geom g, const char* label) {
  UiState& u = U();
  // read-only mode: the object goes back (a refused move left its preview where the mouse let go)
  if (RefuseReadOnlyUi(s)) {
    u.preview_on = false;
    u.preview_shape_on = false;
    return;
  }
  u.preview = g;
  u.preview_shape_on = false;
  u.preview_on = true;
  u.preview_obj = o;
  u.preview_until = s.engine->version();
  s.engine->PostUser(label, [o, g](Document& d) {
    if (o.kind == 1) {
      d.UpdateControl(o.sheet, o.id, [&](Control& c) {
        c.left = g.l;
        c.top = g.t;
        c.width = g.w;
        c.height = g.h;
      });
    } else if (o.kind == 3) {
      d.UpdateShape(o.sheet, o.id, [&](ShapeObj& c) {
        c.left = g.l;
        c.top = g.t;
        c.width = g.w;
        c.height = g.h;
      });
    } else {
      d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) {
        c.left = g.l;
        c.top = g.t;
        c.width = g.w;
        c.height = g.h;
      });
    }
  });
}

// Alt: snap to the cell grid
double SnapX(const model::Sheet& sh, double pt) {
  const int64_t px = static_cast<int64_t>(std::llround(pt * kPx));
  const int64_t i = sh.cols->IndexAt(px);
  const int64_t a = sh.cols->Offset(i), b = sh.cols->Offset(i + 1);
  return static_cast<double>((px - a < b - px) ? a : b) / kPx;
}
double SnapY(const model::Sheet& sh, double pt) {
  const int64_t px = static_cast<int64_t>(std::llround(pt * kPx));
  const int64_t i = sh.rows->IndexAt(px);
  const int64_t a = sh.rows->Offset(i), b = sh.rows->Offset(i + 1);
  return static_cast<double>((px - a < b - px) ? a : b) / kPx;
}

/// while dragging: compute the new geometry from the mouse delta. Like Excel: Alt snaps to the cell grid; Shift keeps a
// move horizontal or vertical and a corner drag proportional (always so for lock_aspect, e.g. pictures); Ctrl sizes
// around the center
// a chart whose Format Chart Area has Lock aspect ratio on: its corner handles keep its shape
bool ChartLocksAspect(const model::Sheet& sh, ObjSel o) {
  if (o.kind != 2) return false;
  for (const auto& c : *sh.charts) {
    if (c.id == o.id) return c.lock_aspect;
  }
  return false;
}

Geom DragGeom(const model::Sheet& sh, int mode, const Geom& g0, ImVec2 delta_px, double sc, bool lock_aspect = false) {
  double dx = delta_px.x / sc / kPx, dy = delta_px.y / sc / kPx;
  Geom g = g0;
  const ImGuiIO& io = ImGui::GetIO();
  const bool snap = io.KeyAlt;
  if (mode == 1) {
    if (io.KeyShift) (std::fabs(dx) >= std::fabs(dy) ? dy : dx) = 0;
    g.l = std::max(0.0, g0.l + dx);
    g.t = std::max(0.0, g0.t + dy);
    if (snap) {
      g.l = SnapX(sh, g.l);
      g.t = SnapY(sh, g.t);
    }
    return g;
  }
  // handles: 2 top-left, 3 top, 4 top-right, 5 right, 6 bottom-right, 7 bottom, 8 bottom-left, 9 left
  const int h = mode - 2;
  const bool left = h == 0 || h == 6 || h == 7, right = h == 2 || h == 3 || h == 4;
  const bool top = h == 0 || h == 1 || h == 2, bottom = h == 4 || h == 5 || h == 6;
  const bool corner = (left || right) && (top || bottom);
  const bool center = io.KeyCtrl;
  double l = g0.l, t = g0.t, r = g0.l + g0.w, b = g0.t + g0.h;
  if (left) l += dx;
  if (right) r += dx;
  if (top) t += dy;
  if (bottom) b += dy;
  if (center) {  // the opposite side moves the other way
    if (left) r -= dx;
    if (right) l -= dx;
    if (top) b -= dy;
    if (bottom) t -= dy;
  }
  if (snap) {
    if (left) l = SnapX(sh, l);
    if (right) r = SnapX(sh, r);
    if (top) t = SnapY(sh, t);
    if (bottom) b = SnapY(sh, b);
  }
  if (r - l < 6) (left ? l : r) = left ? r - 6 : l + 6;
  if (b - t < 6) (top ? t : b) = top ? b - 6 : t + 6;
  if (corner && (lock_aspect || io.KeyShift) && g0.w > 0 && g0.h > 0) {
    // the larger relative change wins; the other side follows it
    const double ratio = g0.h / g0.w;
    const double sw = (r - l) / g0.w, shh = (b - t) / g0.h;
    const double k = std::max(sw, shh);
    const double nw = g0.w * k, nh = nw * ratio;
    if (center) {
      const double cx = g0.l + g0.w / 2, cy = g0.t + g0.h / 2;
      l = cx - nw / 2, r = cx + nw / 2, t = cy - nh / 2, b = cy + nh / 2;
    } else {
      if (left) l = r - nw;
      else r = l + nw;
      if (top) t = b - nh;
      else b = t + nh;
    }
  }
  g.l = std::max(0.0, l);
  g.t = std::max(0.0, t);
  g.w = r - g.l;
  g.h = b - g.t;
  return g;
}

void DeleteSelected(AppState& s) {
  UiState& u = U();
  const ObjSel o = u.sel;
  if (!o) return;
  if (o.kind == 2) {
    int es = 0, ep = -1;
    const model::ChartElem e = ElemOf(o, &es, &ep);
    using E = model::ChartElem;
    if (e != E::ChartArea && e != E::None) {
      // Excel: Delete removes the selected element; on the plot area it only clears its formatting; on a point it
      // removes the point's series; the chart itself goes with the chart area
      s.engine->PostUser(e == E::PlotArea ? TR("Clear Formats") : TR("Delete"), [o, e, es, ep](Document& d) {
        d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) {
          model::ChartSeries* se = es >= 0 && static_cast<size_t>(es) < c.series.size() ? &c.series[static_cast<size_t>(es)] : nullptr;
          switch (e) {
            case E::Title: c.has_title = false; break;
            case E::Legend: c.has_legend = false; break;
            case E::CategoryAxis: c.cat_axis.visible = false; break;
            case E::ValueAxis: c.val_axis.visible = false; break;
            case E::ValueAxis2: c.val_axis2.visible = false; break;
            case E::CatAxisTitle: c.cat_axis.has_title = false; break;
            case E::ValAxisTitle: c.val_axis.has_title = false; break;
            case E::ValAxis2Title: c.val_axis2.has_title = false; break;
            case E::MajorGridlines: c.val_axis.gridlines = false; break;
            case E::MinorGridlines: c.val_axis.minor_gridlines = false; break;
            case E::LegendEntry:  // a series' entry (or a pie's category's) leaves the legend (LegendEntry.Delete)
              if (se) {
                se->legend_hidden = true;
              } else if (const int cs = CategoryEntrySeries(c); cs >= 0 && ep >= 0) {
                PointFormatRef(c.series[static_cast<size_t>(cs)], ep).legend_hidden = true;
              }
              break;
            case E::DataLabel:  // just this label (Point.HasDataLabel = False)
              if (se && ep >= 0) {
                auto it = std::find_if(se->points.begin(), se->points.end(), [&](const model::ChartPointFormat& q) { return q.index == ep; });
                if (it == se->points.end()) {
                  model::ChartPointFormat q;
                  q.index = ep;
                  se->points.push_back(q);
                  it = se->points.end() - 1;
                }
                it->label = 0;
              }
              break;
            case E::PlotArea:
              c.plot_fill = c.plot_line = 0;
              c.plot_rect = {};
              break;
            case E::DataLabels:
              if (se) {
                se->data_labels = false;
                for (auto& p : se->points) p.label = -1;
              }
              break;
            case E::Trendline:
              if (se) se->trend = model::TrendKind::None;
              break;
            case E::ErrorBars:
              if (se) se->err = model::ErrorBarKind::None;
              break;
            case E::Series:
            case E::Point:
              if (se) c.series.erase(c.series.begin() + es);
              break;
            default: break;
          }
        });
      });
      SelectElem(o, e == E::PlotArea ? E::PlotArea : e == E::LegendEntry ? E::Legend : e == E::DataLabel ? E::DataLabels : E::ChartArea, es);
      return;
    }
    const model::Sheet* sh = s.snap->Sheet(o.sheet);
    if (sh && sh->chart_sheet) {  // like Excel: a chart sheet keeps its chart, Delete clears it
      s.engine->PostUser(TR("Clear"), [o](Document& d) {
        d.UpdateChart(o.sheet, o.id, [](ChartObj& c) {
          c.series.clear();
          c.source.clear();
          c.pivot.clear();
          c.has_title = false;
        });
      });
      return;
    }
  }
  std::vector<ObjSel> all{o};
  for (const ObjSel& m : u.more) {
    if (m.sheet == o.sheet) all.push_back(m);
  }
  s.engine->PostUser(TR("Delete Object"), [all](Document& d) {
    for (const ObjSel& x : all) {
      if (x.kind == 1) d.DeleteControl(x.sheet, x.id);
      else if (x.kind == 3) d.DeleteShape(x.sheet, x.id);
      else d.DeleteChart(x.sheet, x.id);
    }
  });
  u.sel = {};
  u.more.clear();
}

// Bring to Front / Send to Back (step = 0), Bring Forward / Send Backward (step = 1: past the next object above / below)
void BringTo(AppState& s, ObjSel o, bool front, int step = 0) {
  const char* label = step ? (front ? TR("Bring Forward") : TR("Send Backward")) : (front ? TR("Bring to Front") : TR("Send to Back"));
  s.engine->PostUser(label, [o, front, step](Document& d) {
    const model::Sheet* sh = d.wb().SheetById(o.sheet);
    if (!sh) return;
    // every object's z, in order
    struct Z {
      int kind;
      uint32_t id;
      int z;
    };
    std::vector<Z> zs;
    for (const auto& x : *sh->controls) zs.push_back({1, x.id, x.z});
    for (const auto& x : *sh->charts) zs.push_back({2, x.id, x.z});
    for (const auto& x : *sh->shapes) zs.push_back({3, x.id, x.z});
    std::stable_sort(zs.begin(), zs.end(), [](const Z& a, const Z& b) { return a.z < b.z; });
    auto me = std::find_if(zs.begin(), zs.end(), [&](const Z& x) { return x.kind == o.kind && x.id == o.id; });
    if (me == zs.end()) return;
    const Z mine = *me;
    size_t at = static_cast<size_t>(me - zs.begin());
    zs.erase(me);
    if (step == 0) at = front ? zs.size() : 0;
    else if (front) at = std::min(zs.size(), at + 1);
    else at = at > 0 ? at - 1 : 0;
    zs.insert(zs.begin() + static_cast<std::ptrdiff_t>(at), mine);
    // renumber: 0, 1, 2... (only the ones that change are written)
    for (size_t i = 0; i < zs.size(); ++i) {
      const int z = static_cast<int>(i);
      if (zs[i].z == z) continue;
      if (zs[i].kind == 1) d.UpdateControl(o.sheet, zs[i].id, [z](Control& c) { c.z = z; });
      else if (zs[i].kind == 3) d.UpdateShape(o.sheet, zs[i].id, [z](ShapeObj& c) { c.z = z; });
      else d.UpdateChart(o.sheet, zs[i].id, [z](ChartObj& c) { c.z = z; });
    }
  });
}

void FireControl(AppState& s, uint32_t sheet, const Control& c, const char* ev);
void DrawChartFlyouts(AppState& s, ObjSel o, const ChartObj& ch, const ImRect& r, const ImRect& cells);
const ChartObj* ChartElemPreview(uint32_t sheet, uint32_t id);
bool ElementsCartesian(ChartType t);

void ComboCommit(AppState& s, uint32_t sheet, const Control& c, const std::string& value, int index) {
  const uint32_t id = c.id;
  if (c.form) {
    s.engine->PostUser(TR("Change ") + c.name, [sheet, id, value, index](Document& d) { ApplyControlValue(d, sheet, id, value, index); });
    FireControl(s, sheet, c, "Change");
    return;
  }
  const std::string linked = c.linked_cell, name = c.name;
  s.engine->PostUser(TR("Change ") + name, [sheet, id, linked, value, index](Document& d) {
    d.UpdateControl(sheet, id, [&](Control& x) {
      x.value = value;
      x.list_index = index;
    });
    WriteLinked(d, sheet, linked, value);
  });
  if (HostReady(s)) s.python->ControlEvent(sheet, name, "Change", "");
}

void DrawHandles(ImDrawList* dl, const ImRect& r, ImU32 col, double rot = 0) {
  if (ImGui::GetFrameCount() <= g_marks_hidden_until) return;  // a copied chart's picture is being taken
  const float hs = S(3.5f);
  ImVec2 pts[8] = {r.Min, ImVec2((r.Min.x + r.Max.x) * 0.5f, r.Min.y), ImVec2(r.Max.x, r.Min.y), ImVec2(r.Max.x, (r.Min.y + r.Max.y) * 0.5f),
                   r.Max, ImVec2((r.Min.x + r.Max.x) * 0.5f, r.Max.y), ImVec2(r.Min.x, r.Max.y), ImVec2(r.Min.x, (r.Min.y + r.Max.y) * 0.5f)};
  if (rot != 0) {
    for (ImVec2& p : pts) p = RotPt(p, r.GetCenter(), rot);
    const ImVec2 box[4] = {pts[0], pts[2], pts[4], pts[6]};
    dl->AddPolyline(box, 4, col, 1.0f, ImDrawFlags_Closed);
  } else {
    dl->AddRect(r.Min, r.Max, col);
  }
  for (const ImVec2& p : pts) {
    dl->AddRectFilled(p - ImVec2(hs, hs), p + ImVec2(hs, hs), IM_COL32_WHITE);
    dl->AddRect(p - ImVec2(hs, hs), p + ImVec2(hs, hs), col);
  }
}

ImVec2 HandlePos(const ImRect& r, int h) {
  switch (h) {
    case 0: return r.Min;
    case 1: return ImVec2((r.Min.x + r.Max.x) * 0.5f, r.Min.y);
    case 2: return ImVec2(r.Max.x, r.Min.y);
    case 3: return ImVec2(r.Max.x, (r.Min.y + r.Max.y) * 0.5f);
    case 4: return r.Max;
    case 5: return ImVec2((r.Min.x + r.Max.x) * 0.5f, r.Max.y);
    case 6: return ImVec2(r.Min.x, r.Max.y);
    default: return ImVec2(r.Min.x, (r.Min.y + r.Max.y) * 0.5f);
  }
}

ImGuiMouseCursor HandleCursor(int h) {
  switch (h) {
    case 0: case 4: return ImGuiMouseCursor_ResizeNWSE;
    case 2: case 6: return ImGuiMouseCursor_ResizeNESW;
    case 1: case 5: return ImGuiMouseCursor_ResizeNS;
    default: return ImGuiMouseCursor_ResizeEW;
  }
}

void DrawControlFace(const Control& c, const ImRect& r, const std::string& text, bool hovered, bool held, bool host_ready) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool enabled = c.enabled && host_ready;
  const ImU32 face = c.back_color ? ExcelColor(c.back_color) : (held ? IM_COL32(0xCC, 0xE4, 0xF7, 255) : hovered ? IM_COL32(0xE5, 0xF1, 0xFB, 255) : IM_COL32(0xF0, 0xF0, 0xF0, 255));
  const ImU32 border = hovered && enabled ? IM_COL32(0x00, 0x78, 0xD7, 255) : IM_COL32(0xAD, 0xAD, 0xAD, 255);
  const ImU32 fg = !enabled ? IM_COL32(0xA0, 0xA0, 0xA0, 255) : (c.fore_color ? ExcelColor(c.fore_color) : IM_COL32(0, 0, 0, 255));
  // ImGui already scales fonts by DPI: only apply the zoom here
  const float zoom = r.GetHeight() / std::max(1.0f, static_cast<float>(c.height * kPx) * ui::Dpi());
  const float fs = (c.font_size > 0 ? c.font_size * static_cast<float>(kPx) : ui::F().ui_size) * zoom;
  ImGui::PushFont(c.bold ? ui::F().ui_bold : ui::F().ui, std::max(6.0f, fs));
  if (c.kind == ControlKind::Button) {
    dl->AddRectFilled(r.Min, r.Max, face);
    dl->AddRect(r.Min, r.Max, border);
    // Format Control > Alignment and Margins (a form button): the caption placed in the box less its margins
    const float k = static_cast<float>(kPx) * zoom * ui::Dpi();
    const ImRect in = c.form && !c.auto_margins ? ImRect(r.Min + ImVec2(static_cast<float>(c.margin_l) * k, static_cast<float>(c.margin_t) * k),
                                                        r.Max - ImVec2(static_cast<float>(c.margin_r) * k, static_cast<float>(c.margin_b) * k))
                                                 : ImRect(r.Min + ImVec2(S(3), S(2)), r.Max - ImVec2(S(3), S(2)));
    const int ha = c.form ? c.text_halign : 1, va = c.form ? c.text_valign : 1, orient = c.form ? c.text_orientation : 0;
    dl->PushClipRect(r.Min, r.Max, true);
    if (orient == 1) {
      // stacked: a character per line
      std::vector<std::string> chars;
      for (const char* p = text.c_str(); *p;) {
        unsigned int ch = 0;
        const int n = ImTextCharFromUtf8(&ch, p, nullptr);
        chars.emplace_back(p, static_cast<size_t>(std::max(1, n)));
        p += std::max(1, n);
      }
      const float lh = ImGui::GetTextLineHeight();
      const float th = lh * static_cast<float>(chars.size());
      float y = va == 0 ? in.Min.y : va == 2 ? in.Max.y - th : in.GetCenter().y - th * 0.5f;
      for (const auto& ch : chars) {
        const float w = ImGui::CalcTextSize(ch.c_str()).x;
        const float x = ha == 0 ? in.Min.x : ha == 2 ? in.Max.x - w : in.GetCenter().x - w * 0.5f;
        dl->AddText(ImVec2(std::floor(x), std::floor(y)), fg, ch.c_str());
        y += lh;
      }
    } else {
      const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
      const bool turned = orient == 2 || orient == 3;
      const float tw = turned ? ts.y : ts.x, th = turned ? ts.x : ts.y;
      const float x = ha == 0 ? in.Min.x : ha == 2 ? in.Max.x - tw : in.GetCenter().x - tw * 0.5f;
      const float y = va == 0 ? in.Min.y : va == 2 ? in.Max.y - th : in.GetCenter().y - th * 0.5f;
      const int v0 = dl->VtxBuffer.Size;
      // drawn level, then turned a quarter about the box it takes (upward: counterclockwise)
      const ImVec2 at = turned ? ImVec2(x + tw * 0.5f - ts.x * 0.5f, y + th * 0.5f - ts.y * 0.5f) : ImVec2(x, y);
      dl->AddText(ImVec2(std::floor(at.x), std::floor(at.y)), fg, text.c_str());
      if (turned) {
        const ImVec2 cc(x + tw * 0.5f, y + th * 0.5f);
        for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
          ImVec2& p = dl->VtxBuffer[i].pos;
          const ImVec2 d = p - cc;
          p = orient == 2 ? ImVec2(cc.x + d.y, cc.y - d.x) : ImVec2(cc.x - d.y, cc.y + d.x);
        }
      }
    }
    dl->PopClipRect();
  } else {
    const float aw = std::min(r.GetHeight(), S(17));
    dl->AddRectFilled(r.Min, r.Max, c.back_color ? ExcelColor(c.back_color) : IM_COL32_WHITE);
    dl->AddRect(r.Min, r.Max, border);
    if (c.form && c.shading_3d) {  // 3-D shading: the drop-down's box sunken
      dl->AddLine(r.Min, ImVec2(r.Max.x, r.Min.y), IM_COL32(0x80, 0x80, 0x80, 255));
      dl->AddLine(r.Min, ImVec2(r.Min.x, r.Max.y), IM_COL32(0x80, 0x80, 0x80, 255));
      dl->AddLine(ImVec2(r.Min.x + 1, r.Min.y + 1), ImVec2(r.Max.x - 1, r.Min.y + 1), IM_COL32(0x40, 0x40, 0x40, 255));
      dl->AddLine(ImVec2(r.Min.x + 1, r.Min.y + 1), ImVec2(r.Min.x + 1, r.Max.y - 1), IM_COL32(0x40, 0x40, 0x40, 255));
    }
    const ImRect ar(ImVec2(r.Max.x - aw, r.Min.y + 1), ImVec2(r.Max.x - 1, r.Max.y - 1));
    dl->AddRectFilled(ar.Min, ar.Max, held ? IM_COL32(0xCC, 0xE4, 0xF7, 255) : hovered ? IM_COL32(0xE5, 0xF1, 0xFB, 255) : IM_COL32(0xF0, 0xF0, 0xF0, 255));
    const ImVec2 cc((ar.Min.x + ar.Max.x) * 0.5f, (ar.Min.y + ar.Max.y) * 0.5f);
    const float a = S(3.5f);
    dl->AddTriangleFilled(ImVec2(cc.x - a, cc.y - a * 0.5f), ImVec2(cc.x + a, cc.y - a * 0.5f), ImVec2(cc.x, cc.y + a * 0.6f), fg);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    dl->PushClipRect(r.Min, ImVec2(ar.Min.x - 2, r.Max.y), true);
    dl->AddText(ImVec2(r.Min.x + S(3), std::floor((r.Min.y + r.Max.y - ts.y) * 0.5f)), fg, text.c_str());
    dl->PopClipRect();
  }
  ImGui::PopFont();
}

// ---- Check boxes, option and toggle buttons, text boxes, labels, list boxes, spin buttons, scroll bars, images ----

// the value shown: the linked cell when there is one, otherwise the control's own value
std::string ControlState(const AppState& s, uint32_t sheet, const Control& c) {
  bool ok = false;
  if (c.form && c.kind == ControlKind::OptionButton) {
    // the group's linked cell holds the number of the chosen option
    const model::Sheet* sh = s.snap->Sheet(sheet);
    if (!sh) return c.value;
    const auto grp = FormOptionGroup(*sh, c);
    std::string cell;
    int me = 0;
    for (size_t i = 0; i < grp.size(); ++i) {
      if (cell.empty()) cell = grp[i]->linked_cell;
      if (grp[i]->id == c.id) me = static_cast<int>(i) + 1;
    }
    const std::string lt = LinkedText(s, sheet, cell, &ok);
    if (!ok) return c.value;
    return std::atoi(lt.c_str()) == me ? "True" : "False";
  }
  const std::string lt = LinkedText(s, sheet, c.linked_cell, &ok);
  if (c.form && (c.kind == ControlKind::ComboBox || c.kind == ControlKind::ListBox) && ok) {
    // the linked cell holds the 1-based item number
    const int n = std::atoi(lt.c_str());
    const auto items = ControlItems(s, sheet, c);
    return n >= 1 && static_cast<size_t>(n) <= items.size() ? items[static_cast<size_t>(n) - 1] : std::string();
  }
  return ok && !lt.empty() ? lt : c.value;
}

bool StateOn(const std::string& v) {
  std::string l = Lower(v);
  return l == "true" || l == "1" || l == "-1";
}

int StateNum(const std::string& v, int def) {
  std::string t;
  for (char ch : v) {
    if (ch != ',') t.push_back(ch);
  }
  char* end = nullptr;
  const double d = std::strtod(t.c_str(), &end);
  return end && end != t.c_str() && std::isfinite(d) ? static_cast<int>(std::lround(d)) : def;
}

void FireControl(AppState& s, uint32_t sheet, const Control& c, const char* ev) {
  if (c.form && !c.on_action.empty() && trust::MacroBlocked(s, c.on_action)) return;  // the code is disabled: Excel's message
  if (!HostReady(s)) return;
  // form controls run their OnAction macro (once per action) instead of sheet-module events
  if (c.form) {
    if (!c.on_action.empty()) s.python->ControlEvent(sheet, c.name, "OnAction", c.on_action);
    return;
  }
  s.python->ControlEvent(sheet, c.name, ev, "");
}

// a new value from the user: the engine updates the control and its linked cell, then the events fire
void SetControlValueUi(AppState& s, uint32_t sheet, const Control& c, const std::string& value, int index, std::initializer_list<const char*> events) {
  const uint32_t id = c.id;
  s.engine->PostUser(TR("Change ") + c.name, [sheet, id, value, index](Document& d) { ApplyControlValue(d, sheet, id, value, index); });
  for (const char* ev : events) {
    FireControl(s, sheet, c, ev);
    if (c.form) break;
  }
}

ImTextureID ControlTexture(const Control& c) {
  UiState& u = U();
  const size_t h = std::hash<std::string>{}(c.picture);
  auto& t = u.textures[c.id];
  if (t.hash != h || !t.tex) {
    if (t.tex) ReleaseTexture(t.tex);
    t.tex = c.picture.empty() ? ImTextureID{} : TextureFromImage(c.picture);
    t.hash = h;
  }
  return t.tex;
}

bool Vertical(const Control& c, const ImRect& r) { return c.orientation == 0 || (c.orientation < 0 && r.GetHeight() >= r.GetWidth()); }

void Arrow(ImDrawList* dl, ImVec2 c, float a, int dir, ImU32 col) {
  // dir: 0 up, 1 down, 2 left, 3 right
  if (dir == 0) dl->AddTriangleFilled(ImVec2(c.x - a, c.y + a * 0.5f), ImVec2(c.x + a, c.y + a * 0.5f), ImVec2(c.x, c.y - a * 0.6f), col);
  if (dir == 1) dl->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.5f), ImVec2(c.x + a, c.y - a * 0.5f), ImVec2(c.x, c.y + a * 0.6f), col);
  if (dir == 2) dl->AddTriangleFilled(ImVec2(c.x + a * 0.5f, c.y - a), ImVec2(c.x + a * 0.5f, c.y + a), ImVec2(c.x - a * 0.6f, c.y), col);
  if (dir == 3) dl->AddTriangleFilled(ImVec2(c.x - a * 0.5f, c.y - a), ImVec2(c.x - a * 0.5f, c.y + a), ImVec2(c.x + a * 0.6f, c.y), col);
}

// draws one of the newer control kinds; interactive = run mode (clicks change the value and fire events)
void DrawControlKind(AppState& s, uint32_t sheet, const Control& c, const ImRect& r, bool interactive) {
  using K = ControlKind;
  UiState& u = U();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const bool enabled = c.enabled;
  const float zoom = r.GetHeight() / std::max(1.0f, static_cast<float>(c.height * kPx) * ui::Dpi());
  const float fs = std::max(6.0f, (c.font_size > 0 ? c.font_size * static_cast<float>(kPx) : ui::F().ui_size) * zoom);
  ImGui::PushFont(c.bold ? ui::F().ui_bold : ui::F().ui, fs);
  const ImU32 fg = !enabled ? IM_COL32(0xA0, 0xA0, 0xA0, 255) : (c.fore_color ? ExcelColor(c.fore_color) : IM_COL32(0, 0, 0, 255));
  const ImU32 border = IM_COL32(0x7A, 0x7A, 0x7A, 255), accent = IM_COL32(0x00, 0x78, 0xD7, 255);
  const ImU32 face = IM_COL32(0xF0, 0xF0, 0xF0, 255), face_hot = IM_COL32(0xE5, 0xF1, 0xFB, 255), face_down = IM_COL32(0xCC, 0xE4, 0xF7, 255);
  const std::string state = ControlState(s, sheet, c);
  const ObjSel me{sheet, 1, c.id};
  auto button = [&](const char* id, const ImRect& b, bool repeat = false) {
    ImGui::SetCursorScreenPos(b.Min);
    if (repeat) ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const bool pressed = ImGui::InvisibleButton(id, ImMax(b.GetSize(), ImVec2(1, 1)));
    if (repeat) ImGui::PopItemFlag();
    return pressed;
  };
  auto caption = [&](const ImRect& b, bool center) {
    dl->PushClipRect(b.Min, b.Max, true);
    const float wrap = c.word_wrap ? b.GetWidth() : 0.0f;
    const ImVec2 ts = ImGui::CalcTextSize(c.caption.c_str(), nullptr, false, wrap > 0 ? wrap : -1.0f);
    const ImVec2 pos(center ? std::floor((b.Min.x + b.Max.x - ts.x) * 0.5f) : b.Min.x, c.kind == K::Label ? b.Min.y : std::floor((b.Min.y + b.Max.y - ts.y) * 0.5f));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, fg, c.caption.c_str(), nullptr, wrap);
    dl->PopClipRect();
  };
  if (c.back_color && c.kind != K::ToggleButton) dl->AddRectFilled(r.Min, r.Max, ExcelColor(c.back_color));
  // Format Control > 3-D shading: a sunken bevel (dark above and left, light below and right)
  auto sunken = [&](const ImRect& b) {
    const ImU32 dark = IM_COL32(0x80, 0x80, 0x80, 255), darker = IM_COL32(0x40, 0x40, 0x40, 255), light = IM_COL32_WHITE, mid = IM_COL32(0xD4, 0xD0, 0xC8, 255);
    dl->AddLine(b.Min, ImVec2(b.Max.x, b.Min.y), dark, 1.0f);
    dl->AddLine(b.Min, ImVec2(b.Min.x, b.Max.y), dark, 1.0f);
    dl->AddLine(ImVec2(b.Min.x + 1, b.Min.y + 1), ImVec2(b.Max.x - 1, b.Min.y + 1), darker, 1.0f);
    dl->AddLine(ImVec2(b.Min.x + 1, b.Min.y + 1), ImVec2(b.Min.x + 1, b.Max.y - 1), darker, 1.0f);
    dl->AddLine(ImVec2(b.Min.x, b.Max.y), b.Max, light, 1.0f);
    dl->AddLine(ImVec2(b.Max.x, b.Min.y), b.Max, light, 1.0f);
    dl->AddLine(ImVec2(b.Min.x + 1, b.Max.y - 1), ImVec2(b.Max.x - 1, b.Max.y - 1), mid, 1.0f);
    dl->AddLine(ImVec2(b.Max.x - 1, b.Min.y + 1), ImVec2(b.Max.x - 1, b.Max.y - 1), mid, 1.0f);
  };
  const bool shaded = c.form && c.shading_3d;
  struct ShadeLater {
    std::function<void()> fn;
    ~ShadeLater() {
      if (fn) fn();
    }
  } shade_later;
  if (shaded && (c.kind == K::ListBox || c.kind == K::ScrollBar || c.kind == K::SpinButton)) shade_later.fn = [&, r] { sunken(r); };
  switch (c.kind) {
    case K::GroupBox: {
      // a frame with its caption on the top line
      const float ty = r.Min.y + fs * 0.5f;
      const ImVec2 ts = ImGui::CalcTextSize(c.caption.c_str());
      const ImU32 line = IM_COL32(0xA0, 0xA0, 0xA0, 255);
      const float tx0 = r.Min.x + S(8), tx1 = tx0 + (c.caption.empty() ? 0 : ts.x + S(6));
      dl->AddLine(ImVec2(r.Min.x, ty), ImVec2(tx0, ty), line);
      dl->AddLine(ImVec2(tx1, ty), ImVec2(r.Max.x, ty), line);
      dl->AddLine(ImVec2(r.Min.x, ty), ImVec2(r.Min.x, r.Max.y), line);
      dl->AddLine(ImVec2(r.Max.x, ty), ImVec2(r.Max.x, r.Max.y), line);
      dl->AddLine(ImVec2(r.Min.x, r.Max.y), ImVec2(r.Max.x, r.Max.y), line);
      if (shaded) {
        // 3-D shading: an etched frame (a light line inside the dark one)
        const ImU32 light = IM_COL32_WHITE;
        dl->AddLine(ImVec2(r.Min.x + 1, ty + 1), ImVec2(tx0, ty + 1), light);
        dl->AddLine(ImVec2(tx1, ty + 1), ImVec2(r.Max.x - 1, ty + 1), light);
        dl->AddLine(ImVec2(r.Min.x + 1, ty + 1), ImVec2(r.Min.x + 1, r.Max.y - 1), light);
        dl->AddLine(ImVec2(r.Max.x + 1, ty), ImVec2(r.Max.x + 1, r.Max.y + 1), light);
        dl->AddLine(ImVec2(r.Min.x, r.Max.y + 1), ImVec2(r.Max.x + 1, r.Max.y + 1), light);
      }
      if (!c.caption.empty()) dl->AddText(ImVec2(tx0 + S(3), r.Min.y), fg, c.caption.c_str());
      break;
    }
    case K::CheckBox:
    case K::OptionButton: {
      bool hot = false, down = false;
      if (interactive) {
        const bool pressed = button("##ctl", r);
        hot = ImGui::IsItemHovered();
        down = ImGui::IsItemActive();
        if (hot && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (pressed && enabled) {
          const bool on = c.kind == K::OptionButton ? true : !StateOn(state);
          if (c.kind == K::CheckBox || !StateOn(state)) SetControlValueUi(s, sheet, c, on ? "True" : "False", -1, {"Click", "Change"});
          else FireControl(s, sheet, c, "Click");
        }
      }
      const float b = std::floor(std::min(r.GetHeight() - 2, std::max(fs * 0.95f, S(13) * zoom)));
      const ImRect box(ImVec2(r.Min.x + S(2) * zoom, std::floor((r.Min.y + r.Max.y - b) * 0.5f)), ImVec2(r.Min.x + S(2) * zoom + b, std::floor((r.Min.y + r.Max.y + b) * 0.5f)));
      const ImU32 bg = down ? face_down : hot ? face_hot : IM_COL32_WHITE;
      const bool on = StateOn(state);
      if (c.kind == K::CheckBox && shaded) {
        // 3-D shading: Excel's classic sunken box with a black check mark
        dl->AddRectFilled(box.Min, box.Max, IM_COL32_WHITE);
        sunken(box);
        if (on) {
          const float k = b * 0.22f, t = std::max(1.5f, b * 0.12f);
          const ImVec2 p0(box.Min.x + k, box.GetCenter().y), p1(box.Min.x + b * 0.42f, box.Max.y - k), p2(box.Max.x - k, box.Min.y + k);
          dl->AddLine(p0, p1, fg, t);
          dl->AddLine(p1, p2, fg, t);
        }
      } else if (c.kind == K::OptionButton && shaded) {
        dl->AddCircleFilled(box.GetCenter(), b * 0.5f, IM_COL32_WHITE);
        dl->PathArcTo(box.GetCenter(), b * 0.5f, 3.14159f * 0.75f, 3.14159f * 1.75f);
        dl->PathStroke(IM_COL32(0x80, 0x80, 0x80, 255), 1.5f, ImDrawFlags_None);
        dl->PathArcTo(box.GetCenter(), b * 0.5f, -3.14159f * 0.25f, 3.14159f * 0.75f);
        dl->PathStroke(IM_COL32(0xD4, 0xD0, 0xC8, 255), 1.5f, ImDrawFlags_None);
        if (on) dl->AddCircleFilled(box.GetCenter(), b * 0.22f, fg);
      } else if (c.kind == K::CheckBox) {
        dl->AddRectFilled(box.Min, box.Max, on && enabled ? accent : bg, S(2));
        dl->AddRect(box.Min, box.Max, on && enabled ? accent : (hot ? accent : border), S(2));
        if (on) {
          const float k = b * 0.22f, t = std::max(1.5f, b * 0.12f);
          const ImVec2 p0(box.Min.x + k, box.GetCenter().y), p1(box.Min.x + b * 0.42f, box.Max.y - k), p2(box.Max.x - k, box.Min.y + k);
          dl->AddLine(p0, p1, enabled ? IM_COL32_WHITE : fg, t);
          dl->AddLine(p1, p2, enabled ? IM_COL32_WHITE : fg, t);
        }
      } else if (!shaded) {
        dl->AddCircleFilled(box.GetCenter(), b * 0.5f, bg);
        dl->AddCircle(box.GetCenter(), b * 0.5f, on && enabled ? accent : (hot ? accent : border), 0, std::max(1.0f, S(1)));
        if (on) dl->AddCircleFilled(box.GetCenter(), b * 0.25f, enabled ? accent : fg);
      }
      caption(ImRect(ImVec2(box.Max.x + S(4) * zoom, r.Min.y), r.Max), false);
      break;
    }
    case K::ToggleButton: {
      bool hot = false, down = false;
      if (interactive) {
        const bool pressed = button("##ctl", r);
        hot = ImGui::IsItemHovered();
        down = ImGui::IsItemActive();
        if (hot && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (pressed && enabled) SetControlValueUi(s, sheet, c, StateOn(state) ? "False" : "True", -1, {"Click", "Change"});
      }
      const bool on = StateOn(state);
      const ImU32 f = c.back_color ? ExcelColor(c.back_color) : (on || down ? face_down : hot ? face_hot : face);
      dl->AddRectFilled(r.Min, r.Max, f);
      dl->AddRect(r.Min, r.Max, on ? accent : (hot ? accent : IM_COL32(0xAD, 0xAD, 0xAD, 255)), 0.0f, on ? 2.0f : 1.0f);
      caption(r, true);
      break;
    }
    case K::Label: {
      if (interactive) {
        if (button("##ctl", r) && enabled) FireControl(s, sheet, c, "Click");
      }
      caption(ImRect(r.Min + ImVec2(S(1), S(1)), r.Max), false);
      break;
    }
    case K::Image: {
      if (interactive) {
        if (button("##ctl", r) && enabled) FireControl(s, sheet, c, "Click");
      }
      if (const ImTextureID tex = ControlTexture(c)) {
        int iw = 0, ih = 0;
        ImageSize(c.picture, &iw, &ih);
        ImRect dst = r;
        if (c.picture_size_mode == 3 && iw > 0 && ih > 0) {  // zoom: keep the aspect ratio
          const float k = std::min(r.GetWidth() / static_cast<float>(iw), r.GetHeight() / static_cast<float>(ih));
          const ImVec2 sz(static_cast<float>(iw) * k, static_cast<float>(ih) * k), c0 = r.GetCenter();
          dst = ImRect(c0 - sz * 0.5f, c0 + sz * 0.5f);
        } else if (c.picture_size_mode == 0 && iw > 0 && ih > 0) {  // clip: its own size from the top-left
          const float k = zoom * ui::Dpi();  // picture pixels are sheet pixels at 100%
          dst = ImRect(r.Min, r.Min + ImVec2(static_cast<float>(iw) * k, static_cast<float>(ih) * k));
        }
        dl->PushClipRect(r.Min, r.Max, true);
        dl->AddImage(tex, dst.Min, dst.Max);
        dl->PopClipRect();
      } else {
        dl->AddRectFilled(r.Min, r.Max, IM_COL32(0xF4, 0xF4, 0xF4, 255));
        dl->AddLine(r.Min, r.Max, IM_COL32(0xC0, 0xC0, 0xC0, 255));
        dl->AddLine(ImVec2(r.Min.x, r.Max.y), ImVec2(r.Max.x, r.Min.y), IM_COL32(0xC0, 0xC0, 0xC0, 255));
      }
      dl->AddRect(r.Min, r.Max, border);
      break;
    }
    case K::TextBox: {
      const bool editing = interactive && u.tb_obj == me;
      if (!editing) {
        dl->AddRectFilled(r.Min, r.Max, c.back_color ? ExcelColor(c.back_color) : IM_COL32_WHITE);
        dl->AddRect(r.Min, r.Max, border);
        std::string shown = state;
        if (!c.password_char.empty()) {
          std::string masked;
          for (size_t i = 0; i < shown.size(); ++i) {
            if ((static_cast<unsigned char>(shown[i]) & 0xC0) != 0x80) masked += c.password_char.substr(0, 1);
          }
          shown = masked;
        }
        const ImRect tr(r.Min + ImVec2(S(3) * zoom, S(2) * zoom), r.Max - ImVec2(S(3) * zoom, S(1)));
        dl->PushClipRect(tr.Min, tr.Max, true);
        const float wrap = c.multi_line || c.word_wrap ? tr.GetWidth() : 0.0f;
        const float ty = c.multi_line ? tr.Min.y : std::floor((tr.Min.y + tr.Max.y - ImGui::GetFontSize()) * 0.5f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tr.Min.x, ty), fg, shown.c_str(), nullptr, wrap);
        dl->PopClipRect();
        if (interactive) {
          if (button("##ctl", r)) {
            // ignore: editing starts on activation below
          }
          if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
          if (ImGui::IsItemActivated() && enabled) {
            u.tb_obj = me;
            std::snprintf(u.tb_buf, sizeof(u.tb_buf), "%s", state.c_str());
            u.tb_focus = true;
            FireControl(s, sheet, c, "GotFocus");
          }
        }
      } else {
        ImGui::SetCursorScreenPos(r.Min);
        if (u.tb_focus) {
          ImGui::SetKeyboardFocusHere();
          u.tb_focus = false;
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
        ImGui::PushStyleColor(ImGuiCol_Text, fg);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(3) * zoom, std::max(0.0f, (r.GetHeight() - ImGui::GetFontSize()) * 0.5f)));
        ImGuiInputTextFlags fl = 0;
        if (!c.password_char.empty()) fl |= ImGuiInputTextFlags_Password;
        if (c.multi_line) {
          ImGui::InputTextMultiline("##tb", u.tb_buf, sizeof(u.tb_buf), r.GetSize(), fl);
        } else {
          ImGui::SetNextItemWidth(r.GetWidth());
          ImGui::InputText("##tb", u.tb_buf, sizeof(u.tb_buf), fl | ImGuiInputTextFlags_EnterReturnsTrue);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        dl->AddRect(r.Min, r.Max, accent);
        if (ImGui::IsItemDeactivated()) {
          const std::string text = u.tb_buf;
          u.tb_obj = {};
          if (text != state) SetControlValueUi(s, sheet, c, text, -1, {"Change", "LostFocus"});
          else FireControl(s, sheet, c, "LostFocus");
        }
      }
      break;
    }
    case K::ListBox: {
      dl->AddRectFilled(r.Min, r.Max, c.back_color ? ExcelColor(c.back_color) : IM_COL32_WHITE);
      const std::vector<std::string> items = ControlItems(s, sheet, c);
      const float row_h = std::floor(ImGui::GetFontSize() + S(3) * zoom);
      const int visible = std::max(1, static_cast<int>((r.GetHeight() - 2) / row_h));
      const int n = static_cast<int>(items.size());
      int& top = u.list_top[c.id];
      top = std::clamp(top, 0, std::max(0, n - visible));
      const bool bar = n > visible;
      const float bar_w = bar ? std::min(S(12) * zoom, r.GetWidth() * 0.3f) : 0.0f;
      const ImRect body(r.Min + ImVec2(1, 1), ImVec2(r.Max.x - 1 - bar_w, r.Max.y - 1));
      const bool multi = c.multi_select != 0;
      auto is_sel = [&](int i) {
        if (multi) return std::find(c.selected.begin(), c.selected.end(), i) != c.selected.end();
        return items[static_cast<size_t>(i)] == state;
      };
      if (interactive) {
        button("##ctl", body);
        const bool hot = ImGui::IsItemHovered();
        if (hot && io.MouseWheel != 0) top = std::clamp(top - static_cast<int>(io.MouseWheel) * 3, 0, std::max(0, n - visible));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && enabled) {
          const int i = top + static_cast<int>((io.MousePos.y - body.Min.y) / row_h);
          if (i >= 0 && i < n) {
            if (!multi || (c.multi_select == 2 && !io.KeyCtrl)) {
              const uint32_t id = c.id;
              if (multi) {
                s.engine->PostUser(TR("Change ") + c.name, [sheet, id, i](Document& d) { d.UpdateControl(sheet, id, [i](Control& x) { x.selected = {i}; x.list_index = i; }); });
              }
              SetControlValueUi(s, sheet, c, items[static_cast<size_t>(i)], i, {"Click", "Change"});
            } else {
              std::vector<int> sel = c.selected;
              const auto it = std::find(sel.begin(), sel.end(), i);
              if (it == sel.end()) sel.push_back(i);
              else sel.erase(it);
              std::sort(sel.begin(), sel.end());
              const uint32_t id = c.id;
              s.engine->PostUser(TR("Change ") + c.name, [sheet, id, sel, i](Document& d) {
                d.UpdateControl(sheet, id, [&](Control& x) {
                  x.selected = sel;
                  x.list_index = i;
                });
              });
              FireControl(s, sheet, c, "Change");
            }
          }
        }
      }
      dl->PushClipRect(body.Min, body.Max, true);
      for (int i = top; i < n && i < top + visible + 1; ++i) {
        const float y = body.Min.y + static_cast<float>(i - top) * row_h;
        const bool sel = is_sel(i);
        if (sel) dl->AddRectFilled(ImVec2(body.Min.x, y), ImVec2(body.Max.x, y + row_h), enabled ? accent : IM_COL32(0xC8, 0xC8, 0xC8, 255));
        dl->AddText(ImVec2(body.Min.x + S(3) * zoom, y + std::floor((row_h - ImGui::GetFontSize()) * 0.5f)), sel && enabled ? IM_COL32_WHITE : fg, items[static_cast<size_t>(i)].c_str());
      }
      dl->PopClipRect();
      if (bar) {
        const ImRect tr(ImVec2(r.Max.x - 1 - bar_w, r.Min.y + 1), ImVec2(r.Max.x - 1, r.Max.y - 1));
        dl->AddRectFilled(tr.Min, tr.Max, IM_COL32(0xF0, 0xF0, 0xF0, 255));
        const float th = std::max(S(8), tr.GetHeight() * static_cast<float>(visible) / static_cast<float>(n));
        const float ty = tr.Min.y + (tr.GetHeight() - th) * static_cast<float>(top) / static_cast<float>(std::max(1, n - visible));
        dl->AddRectFilled(ImVec2(tr.Min.x + 2, ty), ImVec2(tr.Max.x - 2, ty + th), IM_COL32(0xC0, 0xC0, 0xC0, 255), S(2));
      }
      dl->AddRect(r.Min, r.Max, border);
      break;
    }
    case K::SpinButton:
    case K::ScrollBar: {
      const bool vert = Vertical(c, r);
      const int lo = std::min(c.min, c.max), hi = std::max(c.min, c.max);
      const int val = std::clamp(StateNum(state, c.min), lo, hi);
      auto step = [&](int delta, const char* ev) {
        // Min may be larger than Max (the value then grows the other way)
        const int dir = c.min <= c.max ? 1 : -1;
        const int nv = std::clamp(val + delta * dir, lo, hi);
        if (nv == val) return;
        SetControlValueUi(s, sheet, c, std::to_string(nv), -1, {ev, "Change"});
      };
      dl->AddRectFilled(r.Min, r.Max, IM_COL32(0xF0, 0xF0, 0xF0, 255));
      const float a = std::max(S(2), std::min(r.GetWidth(), r.GetHeight()) * 0.18f);
      if (c.kind == K::SpinButton) {
        const ImRect up = vert ? ImRect(r.Min, ImVec2(r.Max.x, r.GetCenter().y)) : ImRect(ImVec2(r.GetCenter().x, r.Min.y), r.Max);
        const ImRect dn = vert ? ImRect(ImVec2(r.Min.x, r.GetCenter().y), r.Max) : ImRect(r.Min, ImVec2(r.GetCenter().x, r.Max.y));
        for (int k = 0; k < 2; ++k) {
          const ImRect& b = k == 0 ? up : dn;
          bool hot = false, down = false;
          if (interactive) {
            const bool pressed = button(k == 0 ? "##up" : "##dn", b, true);
            hot = ImGui::IsItemHovered();
            down = ImGui::IsItemActive();
            if (pressed && enabled) step(k == 0 ? c.small_change : -c.small_change, k == 0 ? "SpinUp" : "SpinDown");
          }
          dl->AddRectFilled(b.Min, b.Max, down ? face_down : hot ? face_hot : face);
          dl->AddRect(b.Min, b.Max, IM_COL32(0xAD, 0xAD, 0xAD, 255));
          Arrow(dl, b.GetCenter(), a, vert ? (k == 0 ? 0 : 1) : (k == 0 ? 3 : 2), fg);
        }
        break;
      }
      // scroll bar: arrows at both ends, a track and a thumb
      const float t = vert ? std::min(r.GetWidth(), r.GetHeight() / 3) : std::min(r.GetHeight(), r.GetWidth() / 3);
      const ImRect a0 = vert ? ImRect(r.Min, ImVec2(r.Max.x, r.Min.y + t)) : ImRect(r.Min, ImVec2(r.Min.x + t, r.Max.y));
      const ImRect a1 = vert ? ImRect(ImVec2(r.Min.x, r.Max.y - t), r.Max) : ImRect(ImVec2(r.Max.x - t, r.Min.y), r.Max);
      const ImRect track = vert ? ImRect(ImVec2(r.Min.x, a0.Max.y), ImVec2(r.Max.x, a1.Min.y)) : ImRect(ImVec2(a0.Max.x, r.Min.y), ImVec2(a1.Min.x, r.Max.y));
      const float len = vert ? track.GetHeight() : track.GetWidth();
      const float range = static_cast<float>(hi - lo);
      const float thumb = std::clamp(len * static_cast<float>(std::max(1, c.large_change)) / (range + static_cast<float>(std::max(1, c.large_change))), std::min(len, S(8)), len);
      const float frac = range > 0 ? static_cast<float>(c.min <= c.max ? val - lo : hi - val) / range : 0.0f;
      const float tpos = (vert ? track.Min.y : track.Min.x) + (len - thumb) * frac;
      const ImRect th = vert ? ImRect(ImVec2(track.Min.x + 1, tpos), ImVec2(track.Max.x - 1, tpos + thumb)) : ImRect(ImVec2(tpos, track.Min.y + 1), ImVec2(tpos + thumb, track.Max.y - 1));
      for (int k = 0; k < 2; ++k) {
        const ImRect& b = k == 0 ? a0 : a1;
        bool hot = false, down = false;
        if (interactive) {
          const bool pressed = button(k == 0 ? "##a0" : "##a1", b, true);
          hot = ImGui::IsItemHovered();
          down = ImGui::IsItemActive();
          if (pressed && enabled) step(k == 0 ? -c.small_change : c.small_change, "Scroll");
        }
        dl->AddRectFilled(b.Min, b.Max, down ? face_down : hot ? face_hot : face);
        Arrow(dl, b.GetCenter(), a, vert ? (k == 0 ? 0 : 1) : (k == 0 ? 2 : 3), fg);
      }
      if (interactive && len > thumb) {
        // the track (page up / down) and the thumb (drag)
        const bool on_thumb = th.Contains(io.MousePos);
        ImGui::SetCursorScreenPos(track.Min);
        ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
        const bool pressed = ImGui::InvisibleButton("##track", ImMax(track.GetSize(), ImVec2(1, 1)));
        ImGui::PopItemFlag();
        if (ImGui::IsItemActivated() && on_thumb) {
          u.scroll_drag = me;
          u.scroll_m0 = vert ? io.MousePos.y : io.MousePos.x;
          u.scroll_v0 = val;
        }
        if (u.scroll_drag == me) {
          if (ImGui::IsItemActive()) {
            const float d = (vert ? io.MousePos.y : io.MousePos.x) - u.scroll_m0;
            const int dir = c.min <= c.max ? 1 : -1;
            const int nv = std::clamp(static_cast<int>(std::lround(u.scroll_v0 + dir * d / std::max(1.0f, len - thumb) * range)), lo, hi);
            if (nv != val) SetControlValueUi(s, sheet, c, std::to_string(nv), -1, {"Scroll"});
          } else {
            u.scroll_drag = {};
            FireControl(s, sheet, c, "Change");
          }
        } else if (pressed && enabled && !on_thumb) {
          const float m = vert ? io.MousePos.y : io.MousePos.x;
          step(m < (vert ? th.Min.y : th.Min.x) ? -c.large_change : c.large_change, "Scroll");
        }
      }
      dl->AddRectFilled(th.Min, th.Max, u.scroll_drag == me ? IM_COL32(0x90, 0x90, 0x90, 255) : IM_COL32(0xC0, 0xC0, 0xC0, 255), S(2));
      dl->AddRect(r.Min, r.Max, IM_COL32(0xAD, 0xAD, 0xAD, 255));
      break;
    }
    default: break;
  }
  ImGui::PopFont();
}

void OpenAssignMacro(ObjSel o, const std::string& current) {
  UiState& u = U();
  u.assign = o;
  u.assign_open = true;
  std::snprintf(u.assign_buf, sizeof u.assign_buf, "%s", current.c_str());
}

// Assign Macro: the OnAction of a form control (a function name; modules\*.py and main.py list their macros)
void DrawAssignMacro(AppState& s) {
  UiState& u = U();
  if (!u.assign_open) return;
  ImGui::SetNextWindowSize(ImVec2(S(380), S(340)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin(TR("Assign Macro###yc_assign_macro"), &u.assign_open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextUnformatted(TR("Macro name:"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##macro", u.assign_buf, sizeof u.assign_buf);
    const auto macros = s.python ? s.python->Macros() : std::vector<std::string>{};
    ImGui::BeginChild("##macros", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - S(6)), ImGuiChildFlags_Borders);
    if (macros.empty()) ImGui::TextWrapped("%s", TR("No macros. Functions without parameters in modules\\*.py or main.py show up here."));
    for (const auto& m : macros) {
      if (ImGui::Selectable(m.c_str(), m == u.assign_buf, ImGuiSelectableFlags_AllowDoubleClick)) std::snprintf(u.assign_buf, sizeof u.assign_buf, "%s", m.c_str());
    }
    ImGui::EndChild();
    bool done = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const ObjSel o = u.assign;
      const std::string name = u.assign_buf;
      s.engine->PostUser(TR("Assign Macro"), [o, name](Document& d) {
        if (o.kind == 3) d.UpdateShape(o.sheet, o.id, [&](ShapeObj& x) { x.on_action = name; });
        else d.UpdateControl(o.sheet, o.id, [&](Control& x) { x.on_action = name; });
      });
      done = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
    if (done) u.assign_open = false;
  }
  ImGui::End();
}

void OpenObjectContext(ObjSel o) {
  U().ctx = o;
  ImGui::OpenPopupEx(kObjCtxPopup);
}

void DrawObjectContext(AppState& s) {
  UiState& u = U();
  if (!ImGui::BeginPopupEx(kObjCtxPopup, kPopupFlags)) return;
  const ObjSel o = u.ctx;
  const model::Sheet* sh = s.snap->Sheet(o.sheet);
  const ChartObj* chart = nullptr;
  if (sh && o.kind == 2) {
    for (const auto& c : *sh->charts) {
      if (c.id == o.id) chart = &c;
    }
  }
  // like Excel, the menu starts with Cut, Copy and Paste (of the object)
  if (ImGui::MenuItem(TR("Cut"), "Ctrl+X")) ui::Commands().Execute("ObjectCut");
  if (ImGui::MenuItem(TR("Copy"), "Ctrl+C")) ui::Commands().Execute("ObjectCopy");
  if (ImGui::MenuItem(TR("Paste"), "Ctrl+V", false, Clip().valid)) ui::Commands().Execute("ObjectPaste");
  ImGui::Separator();
  if (ImGui::MenuItem(TR("Delete"), "Delete")) {
    if (!IsSelected(o)) u.sel = o, u.more.clear();
    DeleteSelected(s);
  }
  const Control* fctl = nullptr;
  if (sh && o.kind == 1) {
    for (const auto& c : *sh->controls) {
      if (c.id == o.id && c.form) fctl = &c;
    }
  }
  if (fctl && ImGui::MenuItem(TR("Assign Macro..."))) OpenAssignMacro(o, fctl->on_action);
  const ShapeObj* cshape = sh && o.kind == 3 ? FindShape(*sh, o.id) : nullptr;
  if (cshape && ImGui::MenuItem(TR("Assign Macro..."))) OpenAssignMacro(o, cshape->on_action);
  if (chart) {
    // the menu of the right-clicked element: a series' items act on that series only (Excel)
    int es = 0, ep = -1;
    const model::ChartElem e = ElemOf(o, &es, &ep);
    using E = model::ChartElem;
    const bool on_series = (e == E::Series || e == E::Point || e == E::DataLabels || e == E::Trendline || e == E::ErrorBars) && es >= 0 &&
                           static_cast<size_t>(es) < chart->series.size();
    if (ImGui::MenuItem(TR("Reset to Match Style"))) {
      u.sel = o;
      ui::Commands().Execute("ChartResetStyle");
    }
    if (ImGui::MenuItem(on_series ? TR("Change Series Chart Type...") : TR("Change Chart Type..."))) {
      u.sel = o;
      OpenChartTypeDialog(s, true, on_series ? es : -1);
    }
    if (ImGui::MenuItem(TR("Select Data..."))) {
      u.sel = o;
      OpenSelectData(s, o.sheet, o.id);
    }
    if (!on_series && ImGui::MenuItem(TR("Move Chart..."))) {
      u.sel = o;
      ui::Commands().Execute("MoveChart");
    }
    if (on_series) {
      const model::ChartSeries& se = chart->series[static_cast<size_t>(es)];
      const bool point = e == E::Point && ep >= 0;
      const model::ChartPointFormat* pf = point ? PointFmt(se, ep) : nullptr;
      const bool labels = point ? (pf && pf->label >= 0 ? pf->label == 1 : se.data_labels) : se.data_labels;
      if (ImGui::MenuItem(point ? TR("Add Data Label") : TR("Add Data Labels"), nullptr, labels)) {
        const bool v = !labels;
        s.engine->PostUser(TR("Data Labels"), [o, es, ep, point, v](Document& d) {
          d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) {
            if (static_cast<size_t>(es) >= c.series.size()) return;
            auto& x = c.series[static_cast<size_t>(es)];
            if (!point) {
              x.data_labels = v;
              return;
            }
            auto it = std::find_if(x.points.begin(), x.points.end(), [&](const model::ChartPointFormat& p) { return p.index == ep; });
            if (it == x.points.end()) {
              x.points.push_back({});
              it = x.points.end() - 1;
              it->index = ep;
            }
            it->label = v ? 1 : 0;
          });
        });
      }
      if (ElementsCartesian(chart->type) && ImGui::MenuItem(TR("Add Trendline..."))) {
        // a linear trendline on this series, and its Format pane (Excel)
        s.engine->PostUser(TR("Trendline"), [o, es](Document& d) {
          d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) {
            if (static_cast<size_t>(es) < c.series.size() && c.series[static_cast<size_t>(es)].trend == model::TrendKind::None)
              c.series[static_cast<size_t>(es)].trend = model::TrendKind::Linear;
          });
        });
        SelectElem(o, E::Trendline, es);
        u.show_props = true;
      }
      if (point && chart->type == ChartType::Waterfall) {
        const bool tot = std::find(se.totals.begin(), se.totals.end(), ep) != se.totals.end();
        if (ImGui::MenuItem(TR("Set as Total"), nullptr, tot)) {
          s.engine->PostUser(TR("Set as Total"), [o, es, ep, tot](Document& d) {
            d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) {
              if (static_cast<size_t>(es) >= c.series.size()) return;
              auto& t = c.series[static_cast<size_t>(es)].totals;
              t.erase(std::remove(t.begin(), t.end(), ep), t.end());
              if (!tot) t.push_back(ep);
              std::sort(t.begin(), t.end());
            });
          });
        }
      }
    }
    if (ImGui::MenuItem((FormatPaneName(*chart, e, es, ep) + "...").c_str())) {
      u.sel = o;
      u.show_props = true;
    }
  } else if (fctl) {
    if (ImGui::MenuItem(TR("Format Control..."))) u.sel = o, OpenFormatControl(s, o);
  } else if (ImGui::MenuItem(cshape ? (cshape->kind == ShapeKind::Picture ? TR("Format Picture...") : TR("Format Shape...")) : TR("Properties..."))) {
    u.sel = o;
    u.show_props = true;
  }
  if (o.kind == 1 && !fctl && ImGui::MenuItem(TR("Format Control..."))) u.sel = o, OpenFormatControl(s, o);
  if (o.kind == 1 && !fctl && ImGui::MenuItem(TR("View Code"))) {
    u.sel = o;
    ViewSelectedControlCode(s);
  }
  // a form control's text is typed in place (Edit Text), as in Excel: buttons, check boxes, option buttons, group boxes
  if (fctl && (fctl->kind == ControlKind::Button || fctl->kind == ControlKind::CheckBox || fctl->kind == ControlKind::OptionButton ||
               fctl->kind == ControlKind::GroupBox || fctl->kind == ControlKind::Label) &&
      ImGui::MenuItem(TR("Edit Text"))) {
    StartCaptionEdit(o, fctl->caption);
  }
  ImGui::Separator();
  if (ImGui::BeginMenu(TR("Bring to Front"))) {
    if (ImGui::MenuItem(TR("Bring to Front"))) BringTo(s, o, true);
    if (ImGui::MenuItem(TR("Bring Forward"))) BringTo(s, o, true, 1);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(TR("Send to Back"))) {
    if (ImGui::MenuItem(TR("Send to Back"))) BringTo(s, o, false);
    if (ImGui::MenuItem(TR("Send Backward"))) BringTo(s, o, false, 1);
    ImGui::EndMenu();
  }
  if (cshape || !u.more.empty()) {
    if (ImGui::BeginMenu(TR("Group"))) {
      if (ImGui::MenuItem(TR("Group"), nullptr, false, !u.more.empty())) ui::Commands().Execute("GroupObjects");
      if (ImGui::MenuItem(TR("Ungroup"), nullptr, false, cshape && cshape->group)) ui::Commands().Execute("UngroupObjects");
      ImGui::EndMenu();
    }
  }
  if (cshape && HasText(*cshape) && ImGui::MenuItem(TR("Edit Text"))) StartShapeText(o, *cshape, u.sel_screen, u.sel_scale, "");
  if (chart) {
    ImGui::Separator();
    if (ImGui::MenuItem(TR("Interactive (pan / zoom)"), nullptr, chart->interactive)) {
      const bool v = !chart->interactive;
      s.engine->PostUser(TR("Chart interactive mode"), [o, v](Document& d) { d.UpdateChart(o.sheet, o.id, [v](ChartObj& c) { c.interactive = v; }); });
    }
    if (ImGui::MenuItem(chart->floating ? TR("Move Back to Sheet") : TR("Move to Own Window"))) {
      const bool v = !chart->floating;
      s.engine->PostUser(v ? TR("Chart moved to its own window") : TR("Chart moved back to the sheet"), [o, v](Document& d) { d.UpdateChart(o.sheet, o.id, [v](ChartObj& c) { c.floating = v; }); });
    }
  }
  ImGui::EndPopup();
}

// ---- Several selected objects, groups, copies ----

// the other members of o's group (shapes with the same group id) join the selection
void AddGroupOf(const model::Sheet& sh, ObjSel o) {
  UiState& u = U();
  if (o.kind != 3) return;
  const ShapeObj* x = FindShape(sh, o.id);
  if (!x || !x->group) return;
  for (const auto& y : *sh.shapes) {
    const ObjSel m{o.sheet, 3, y.id};
    if (y.group == x->group && !IsSelected(m)) u.more.push_back(m);
  }
}

// a click on an object: Shift or Ctrl+click adds it to the selection or takes it out; a plain click selects it (and
// its group), or keeps the selection when it is part of it (to move them together). Returns whether a move may start.
bool SelectOnClick(const model::Sheet& sh, ObjSel me) {
  UiState& u = U();
  const ImGuiIO& io = ImGui::GetIO();
  if ((io.KeyShift || io.KeyCtrl) && u.sel && u.sel.sheet == me.sheet) {
    if (IsSelected(me)) {
      if (io.KeyCtrl) return true;  // Ctrl+drag of the selection copies it
      if (u.sel == me) {
        if (u.more.empty()) return false;
        u.sel = u.more.front();
        u.more.erase(u.more.begin());
      } else {
        u.more.erase(std::remove(u.more.begin(), u.more.end(), me), u.more.end());
      }
      return false;
    }
    u.more.push_back(u.sel);
    u.sel = me;
    AddGroupOf(sh, me);
    return true;
  }
  if (IsSelected(me)) {
    if (u.sel != me) {
      u.more.erase(std::remove(u.more.begin(), u.more.end(), me), u.more.end());
      u.more.push_back(u.sel);
      u.sel = me;
    }
    return true;
  }
  u.sel = me;
  u.more.clear();
  AddGroupOf(sh, me);
  return true;
}

// the selected objects (the primary first) on their sheet
std::vector<ObjSel> SelectedList() {
  const UiState& u = U();
  std::vector<ObjSel> v;
  if (!u.sel) return v;
  v.push_back(u.sel);
  for (const ObjSel& m : u.more) {
    if (m.sheet == u.sel.sheet) v.push_back(m);
  }
  return v;
}

// moves the other selected objects by (dx, dy) points (the primary was moved by CommitGeom)
void MoveMore(AppState& s, double dx, double dy) {
  std::vector<ObjSel> v = U().more;
  s.engine->PostUser(TR("Move Object"), [v, dx, dy](Document& d) {
    for (const ObjSel& o : v) {
      auto mv = [&](auto& x) {
        x.left = std::max(0.0, x.left + dx);
        x.top = std::max(0.0, x.top + dy);
      };
      if (o.kind == 1) d.UpdateControl(o.sheet, o.id, mv);
      else if (o.kind == 3) d.UpdateShape(o.sheet, o.id, mv);
      else d.UpdateChart(o.sheet, o.id, mv);
    }
  });
}

// copies of the selected objects (each found in the snapshot)
ObjClip CopyOfSelection(const AppState& s) {
  ObjClip c;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(U().sel.sheet) : nullptr;
  if (!sh) return c;
  c.sheet = sh->id;
  for (const ObjSel& o : SelectedList()) {
    if (o.kind == 3) {
      if (const ShapeObj* x = FindShape(*sh, o.id)) c.shapes.push_back(*x);
    } else if (o.kind == 2) {
      for (const auto& x : *sh->charts) {
        if (x.id == o.id) c.charts.push_back(x);
      }
    } else {
      for (const auto& x : *sh->controls) {
        if (x.id == o.id) c.controls.push_back(x);
      }
    }
  }
  c.valid = !c.shapes.empty() || !c.charts.empty() || !c.controls.empty();
  return c;
}

// adds the clipboard's objects to sheet, moved by (dx, dy) points, or with their top-left corner at (at_l, at_t) when
// at_l >= 0; selects the new ones. Groups get new ids, so a pasted group is a group of its own.
void PasteObjClip(AppState& s, const ObjClip& c, uint32_t sheet, double dx, double dy, const char* label, double at_l = -1, double at_t = -1) {
  if (!c.valid) return;
  if (at_l >= 0) {
    double l = std::numeric_limits<double>::max(), t = std::numeric_limits<double>::max();
    for (const auto& x : c.shapes) l = std::min(l, x.left), t = std::min(t, x.top);
    for (const auto& x : c.charts) l = std::min(l, x.left), t = std::min(t, x.top);
    for (const auto& x : c.controls) l = std::min(l, x.left), t = std::min(t, x.top);
    dx = at_l - l;
    dy = at_t - t;
  }
  struct Added {
    std::vector<ObjSel> v;
  };
  const Added added = s.engine->Call([c, sheet, dx, dy, label](Document& d) {
    Added a;
    d.Begin(label);
    std::map<uint32_t, std::vector<uint32_t>> groups;  // a copied group: the new members (they get the first one's id)
    for (ShapeObj x : c.shapes) {
      x.name.clear();
      x.left = std::max(0.0, x.left + dx);
      x.top = std::max(0.0, x.top + dy);
      const uint32_t old_group = x.group;
      x.group = 0;
      const uint32_t id = d.AddShape(sheet, x);
      if (!id) continue;
      a.v.push_back(ObjSel{sheet, 3, id});
      if (old_group) groups[old_group].push_back(id);
    }
    for (const auto& [old_group, ids] : groups) {
      for (const uint32_t id : ids) d.UpdateShape(sheet, id, [g = ids.front()](ShapeObj& y) { y.group = g; });
    }
    for (ChartObj x : c.charts) {
      x.name.clear();
      x.floating = false;
      x.left = std::max(0.0, x.left + dx);
      x.top = std::max(0.0, x.top + dy);
      if (const uint32_t id = d.AddChart(sheet, x)) a.v.push_back(ObjSel{sheet, 2, id});
    }
    for (Control x : c.controls) {
      const bool caption_is_name = x.caption == x.name;
      x.name.clear();
      if (caption_is_name) x.caption.clear();
      x.left = std::max(0.0, x.left + dx);
      x.top = std::max(0.0, x.top + dy);
      if (const uint32_t id = d.AddControl(sheet, x)) a.v.push_back(ObjSel{sheet, 1, id});
    }
    d.Commit();
    return a;
  });
  UiState& u = U();
  if (added.v.empty()) return;
  u.sel = added.v.front();
  u.more.assign(added.v.begin() + 1, added.v.end());
}

// Ctrl+D / Ctrl+drag: copies of the selection, moved by (dx, dy)
void DuplicateSelection(AppState& s, double dx, double dy, const char* label) {
  const ObjClip c = CopyOfSelection(s);
  PasteObjClip(s, c, c.sheet, dx, dy, label);
}

// starts typing into a shape: its text (plus what was just typed) in a box over it, the caret at the end
void StartShapeText(ObjSel me, const ShapeObj& sp, const ImRect& r, float scale, const std::string& typed) {
  UiState& u = U();
  u.sel = me;
  u.text_obj = me;
  std::snprintf(u.text_buf, sizeof(u.text_buf), "%s%s", sp.text.c_str(), typed.c_str());
  u.text_focus = true;
  u.text_cursor_end = true;
  u.text_pos = r.Min;
  u.text_size = r.GetSize();
  u.text_scale = scale;
  u.drag = 0;
}

}  // namespace

// ---- Objects on the sheet (overlay callback of the grid) ----

// Edit Text of a form control: its caption typed in a box over it; Enter, Escape or a click away keeps it
struct CaptionEdit {
  ObjSel obj;
  char buf[512] = {};
  bool focus = false;
};
CaptionEdit& CE() {
  static CaptionEdit c;
  return c;
}
void StartCaptionEdit(ObjSel o, const std::string& caption) {
  CE().obj = o;
  std::snprintf(CE().buf, sizeof(CE().buf), "%s", caption.c_str());
  CE().focus = true;
}

// a preset shape's yellow handles (Shape.Adjustments): the one being dragged and the adjustments it makes
struct AdjDrag {
  bool on = false, pending = false;
  ObjSel obj;
  int handle = 0;
  std::vector<double> adj;
  std::vector<ImVec2> pts;  // where the handles are on screen (drawn over the objects)
};
AdjDrag& AD() {
  static AdjDrag a;
  return a;
}

// a new WordArt's placeholder text: the first typing replaces it (Excel selects it)
uint32_t g_placeholder_shape = 0;
void MarkPlaceholderText(uint32_t shape_id) { g_placeholder_shape = shape_id; }

// ---- Insert > Shapes: the shape picked is drawn on the sheet, as in Excel ----
namespace {
struct ShapeDraw {
  bool armed = false;
  int code = 0;          // the gallery entry: an msoAutoShapeType, 0 a text box, engine::kLine... for the Lines group
  uint32_t sheet = 0;
  bool down = false;     // the mouse is pressed (a box or line being dragged, a scribble)
  ImVec2 m0;             // where the drag started (screen)
  std::vector<ImVec2> pts;  // Curve / Freeform / Scribble: the points so far (sheet pixels)
  double last_click = -1;
};
ShapeDraw& SD() {
  static ShapeDraw d;
  return d;
}
bool FreeformCode(int code) { return code == engine::kCurve || code == engine::kFreeform || code == engine::kScribble; }
bool LineCode(int code) { return code < 0 && code >= engine::kCurvedDoubleArrow; }

// the new shape of a gallery entry, before its place and size
ShapeObj NewGalleryShape(int code) {
  ShapeObj sh;
  switch (code) {
    // the kinds older versions know, with their AutoShapeType (drawn with its adjustments here)
    case 1: sh.kind = ShapeKind::Rectangle, sh.auto_type = 1; break;
    case 5: sh.kind = ShapeKind::RoundedRectangle, sh.auto_type = 5; break;
    case 9: sh.kind = ShapeKind::Oval, sh.auto_type = 9; break;
    case 7: sh.kind = ShapeKind::Triangle, sh.auto_type = 7; break;
    case 33: sh.kind = ShapeKind::RightArrow, sh.auto_type = 33; break;
    case 0:
      sh.kind = ShapeKind::TextBox;
      sh.fill_color = 0xFFFFFFFF;
      sh.line_color = 0xFF000000;
      sh.font_color = 0xFF000000;
      sh.halign = 0;
      sh.valign = 0;
      break;
    default:
      if (code < 0) {
        const bool arrow = code == engine::kLineArrow || code == engine::kElbowArrow || code == engine::kCurvedArrow;
        const bool both = code == engine::kLineDoubleArrow || code == engine::kElbowDoubleArrow || code == engine::kCurvedDoubleArrow;
        sh.kind = arrow ? ShapeKind::Arrow : ShapeKind::Line;
        sh.end_arrow = arrow || both ? 1 : 0;
        sh.begin_arrow = both ? 1 : 0;
        sh.connector = code <= engine::kElbow && code >= engine::kElbowDoubleArrow ? 2 : code <= engine::kCurved && code >= engine::kCurvedDoubleArrow ? 3 : 0;
        sh.line_color = 0xFF4472C4;
        sh.line_weight = 1.5f;
        if (FreeformCode(code)) {
          sh.kind = ShapeKind::Rectangle;
          sh.auto_type = engine::kFreeformType;
          sh.path_smooth = code == engine::kCurve;
          sh.line_weight = 0.75f;
        }
      } else {
        sh.kind = ShapeKind::Rectangle;
        sh.auto_type = code;
      }
      break;
  }
  return sh;
}
}  // namespace

void ArmShapeDrawing(AppState& s, int code) {
  ShapeDraw& d = SD();
  d = ShapeDraw{};
  d.armed = true;
  d.code = code;
  d.sheet = s.focus_sheet;
  U().sel = {};
  U().more.clear();
  // the keyboard goes back to the sheet (Escape stops drawing)
  ImGui::SetWindowFocus(s.Win(s.focus_sheet).floating ? s.SheetWindowName(s.focus_sheet).c_str() : kGridHostWindow);
  s.SetStatus(FreeformCode(code) ? TR("Click to add points, double-click to finish (Esc ends)") : TR("Drag on the sheet to draw the shape; a click inserts it at its default size"));
}

bool ShapeDrawingArmed() { return SD().armed; }

void DrawGalleryPreview(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g);

void DrawSheetObjects(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  UiState& u = U();
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool host_ready = HostReady(s);
  if (u.preview_on && s.snap->version > u.preview_until && u.drag == 0) u.preview_on = false;

  struct Item {
    int kind;
    size_t index;
    int z;
  };
  std::vector<Item> items;
  for (size_t i = 0; i < sh->controls->size(); ++i) items.push_back({1, i, (*sh->controls)[i].z});
  for (size_t i = 0; i < sh->charts->size(); ++i) {
    if (!(*sh->charts)[i].floating) items.push_back({2, i, (*sh->charts)[i].z});
  }
  for (size_t i = 0; i < sh->shapes->size(); ++i) {
    if ((*sh->shapes)[i].visible) items.push_back({3, i, (*sh->shapes)[i].z});
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.z < b.z; });
  // the selected chart's data outlined on the cells (its corner handles take the mouse before the objects)
  DrawChartDataHighlights(s, sheet_id, g);
  // ---- the selected preset shape's yellow handles: their buttons go before the objects (the one submitted first gets the
  // mouse), placed where the shape was drawn last frame; dragging one changes an adjustment (Shape.Adjustments) ----
  {
    AdjDrag& ad = AD();
    ad.pts.clear();
    const ShapeObj* as = u.sel.kind == 3 && u.sel.sheet == sheet_id && u.more.empty() ? FindShape(*sh, u.sel.id) : nullptr;
    if (as && as->auto_type > 0 && as->path.empty() && u.drag == 0 && !u.rotating && u.sel_screen.GetWidth() > 2 && u.sel_screen.GetHeight() > 2) {
      if (ad.pending && (as->adj == ad.adj || !(ad.obj == u.sel))) ad.pending = false;
      const ImRect r = u.sel_screen;
      const std::vector<double>& cur = (ad.on || ad.pending) && ad.obj == u.sel ? ad.adj : as->adj;
      const auto hp = engine::AutoShapeHandles(as->auto_type, r.GetWidth(), r.GetHeight(), cur);
      const ImVec2 c = r.GetCenter();
      auto to_screen = [&](engine::ShapePt q) {
        const float x = static_cast<float>(as->flip_h ? r.GetWidth() - q.x : q.x), y = static_cast<float>(as->flip_v ? r.GetHeight() - q.y : q.y);
        return RotPt(ImVec2(r.Min.x + x, r.Min.y + y), c, as->rotation);
      };
      const bool locked = sh->protection->on && !sh->protection->objects && as->locked;
      for (size_t k = 0; k < hp.size(); ++k) {
        const ImVec2 p = to_screen(hp[k]);
        ad.pts.push_back(p);
        ImGui::SetCursorScreenPos(p - ImVec2(S(5), S(5)));
        ImGui::PushID(1300 + static_cast<int>(k));
        ImGui::InvisibleButton("##adjust", ImVec2(S(10), S(10)));
        ui::NoteUiItem((std::string("adjust handle ") + std::to_string(k + 1)).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        if (ImGui::IsItemHovered() || (ad.on && ad.handle == static_cast<int>(k))) ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        if (ImGui::IsItemActivated() && !locked) {
          ad.on = true;
          ad.obj = u.sel;
          ad.handle = static_cast<int>(k);
          ad.adj = as->adj;
        }
        ImGui::PopID();
      }
      if (ad.on && ad.obj == u.sel) {
        // the mouse in the shape's own box (turned back, flipped back)
        const ImVec2 q = RotPt(io.MousePos, c, -as->rotation);
        double x = q.x - r.Min.x, y = q.y - r.Min.y;
        if (as->flip_h) x = r.GetWidth() - x;
        if (as->flip_v) y = r.GetHeight() - y;
        engine::AutoShapeDragHandle(as->auto_type, static_cast<size_t>(ad.handle), r.GetWidth(), r.GetHeight(), {x, y}, &ad.adj);
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          ad.on = false;
          ad.pending = true;
          const ObjSel o = ad.obj;
          const std::vector<double> v = ad.adj;
          s.engine->PostUser(TR("Adjust Shape"), [o, v](Document& d) { d.UpdateShape(o.sheet, o.id, [&](ShapeObj& x) { x.adj = v; }); });
        }
      }
    } else if (!ad.on) {
      ad.pending = false;
    }
  }
  // ---- Insert > Shapes (before the objects, so a drag over one draws instead of moving it): drawing the shape picked (drag; a click: its default size; Shift: square / 45 degrees;
  // Ctrl: from the center; Alt: to the cell grid). Curve / Freeform: clicks add points, a double-click ends. ----
  if (ShapeDraw& sd = SD(); sd.armed && sd.sheet == sheet_id && s.focus_sheet == sheet_id) {
    const bool ff = FreeformCode(sd.code);
    ImGui::SetCursorScreenPos(g.cells.Min);
    ImGui::InvisibleButton("##yc_draw_shape", g.cells.GetSize());
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    auto finish_freeform = [&](bool closed) {
      ShapeObj n = NewGalleryShape(sd.code);
      std::vector<ImVec2> v = sd.pts;
      if (v.size() >= 2) {
        ImVec2 lo = v[0], hi = v[0];
        for (const ImVec2& q : v) lo = ImMin(lo, q), hi = ImMax(hi, q);
        const float w = std::max(1.0f, hi.x - lo.x), h = std::max(1.0f, hi.y - lo.y);
        for (const ImVec2& q : v) n.path.push_back((q.x - lo.x) / w), n.path.push_back((q.y - lo.y) / h);
        n.path_closed = closed;
        n.fill_visible = closed;
        if (!closed) n.line_color = 0xFF4472C4;
        n.left = lo.x / kPx;
        n.top = lo.y / kPx;
        n.width = w / kPx;
        n.height = h / kPx;
        const uint32_t id = s.engine->Call([sheet_id, n](Document& d) {
          d.Begin(TR("Insert Shape"));
          const uint32_t v2 = d.AddShape(sheet_id, n);
          d.Commit();
          return v2;
        });
        if (id) u.sel = ObjSel{sheet_id, 3, id};
      }
      sd = ShapeDraw{};
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ff && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
      if (ff && sd.pts.size() >= 2) finish_freeform(false);
      else sd = ShapeDraw{};
    } else if (ff) {
      const ImVec2 at = g.to_sheet(io.MousePos);
      if (ImGui::IsItemActivated()) {
        const bool dbl = io.MouseClickedLastCount[0] >= 2;
        sd.down = true;
        if (dbl && sd.pts.size() >= 2) {
          // a double-click ends it; back on the first point closes it
          const ImVec2 a = sd.pts.front(), b = sd.pts.back();
          finish_freeform(sd.code != engine::kScribble && std::fabs(a.x - b.x) + std::fabs(a.y - b.y) < 10);
        } else if (!sd.pts.empty() && sd.code != engine::kScribble && sd.pts.size() >= 3 &&
                   std::fabs(at.x - sd.pts.front().x) + std::fabs(at.y - sd.pts.front().y) < 8) {
          finish_freeform(true);  // a click on the first point closes the shape
        } else {
          sd.pts.push_back(at);
        }
      } else if (sd.down && ImGui::IsMouseDown(ImGuiMouseButton_Left) && sd.code != engine::kCurve) {
        // dragging draws freehand (Freeform: Shape and Scribble)
        if (!sd.pts.empty() && std::fabs(at.x - sd.pts.back().x) + std::fabs(at.y - sd.pts.back().y) >= 3) sd.pts.push_back(at);
      } else if (sd.down && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        sd.down = false;
        if (sd.code == engine::kScribble) finish_freeform(false);
      }
      if (sd.armed && !sd.pts.empty()) {
        std::vector<ImVec2> v;
        for (const ImVec2& q : sd.pts) v.push_back(g.to_screen(q.x, q.y));
        if (!sd.down || sd.code == engine::kCurve) v.push_back(io.MousePos);
        dl->AddPolyline(v.data(), static_cast<int>(v.size()), IM_COL32(0x44, 0x72, 0xC4, 255), S(1.5f), ImDrawFlags_None);
      }
    } else {
      if (ImGui::IsItemActivated()) {
        sd.down = true;
        sd.m0 = io.MousePos;
      }
      if (sd.down) {
        ImVec2 a = sd.m0, b = io.MousePos;
        const bool line = LineCode(sd.code);
        if (io.KeyShift) {
          const float dx = b.x - a.x, dy = b.y - a.y;
          if (line && std::fabs(dx) > 2 * std::fabs(dy)) b.y = a.y;
          else if (line && std::fabs(dy) > 2 * std::fabs(dx)) b.x = a.x;
          else {
            const float m = std::max(std::fabs(dx), std::fabs(dy));
            b = ImVec2(a.x + (dx < 0 ? -m : m), a.y + (dy < 0 ? -m : m));
          }
        }
        if (io.KeyCtrl && !line) a = ImVec2(a.x * 2 - b.x, a.y * 2 - b.y);  // from the center
        ShapeObj n = NewGalleryShape(sd.code);
        const ImRect box(ImMin(a, b), ImMax(a, b));
        n.flip_h = line && b.x < a.x;
        n.flip_v = line && b.y < a.y;
        if (box.GetWidth() + box.GetHeight() >= 3) DrawShapeFx(dl, n, box, static_cast<float>(g.sc));  // with its Effects
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          ImVec2 sa = g.to_sheet(box.Min), sb = g.to_sheet(box.Max);
          if (box.GetWidth() + box.GetHeight() < 4) {
            // a click: Excel's default size (one inch; a line one inch across; a text box a line of text)
            sa = g.to_sheet(sd.m0);
            sb = ImVec2(sa.x + static_cast<float>(72 * kPx), sa.y + static_cast<float>((line ? 0 : sd.code == 0 ? 24 : 72) * kPx));
            n.flip_h = n.flip_v = false;
          }
          n.left = sa.x / kPx;
          n.top = sa.y / kPx;
          n.width = (sb.x - sa.x) / kPx;
          n.height = (sb.y - sa.y) / kPx;
          if (io.KeyAlt) {
            n.left = SnapX(*sh, n.left);
            n.top = SnapY(*sh, n.top);
          }
          const uint32_t id = s.engine->Call([sheet_id, n](Document& d) {
            d.Begin(TR("Insert Shape"));
            const uint32_t v2 = d.AddShape(sheet_id, n);
            d.Commit();
            return v2;
          });
          const int code = sd.code;
          sd = ShapeDraw{};
          if (id) {
            u.sel = ObjSel{sheet_id, 3, id};
            // a new text box takes the typing at once (Excel puts the caret in it)
            if (code == 0) {
              ShapeObj tb = n;
              tb.id = id;
              StartShapeText(u.sel, tb, ScreenRect(g, Placement::MoveAndSize, GeomOf(tb)), static_cast<float>(g.sc), "");
            }
          }
        }
      }
    }
  }

  ImRect sel_rect;
  bool sel_visible = false;
  Placement sel_pl = Placement::MoveAndSize;
  for (const Item& it : items) {
    const bool is_ctl = it.kind == 1;
    const Control* c = is_ctl ? &(*sh->controls)[it.index] : nullptr;
    const ChartObj* ch = it.kind == 2 ? &(*sh->charts)[it.index] : nullptr;
    const ShapeObj* shp = it.kind == 3 ? &(*sh->shapes)[it.index] : nullptr;
    const ObjSel me{sheet_id, it.kind, is_ctl ? c->id : ch ? ch->id : shp->id};
    if (is_ctl && !c->visible && !s.design_mode) continue;
    const Placement pl = is_ctl ? c->placement : ch ? ch->placement : shp->placement;
    Geom gm = is_ctl ? GeomOf(*c) : ch ? GeomOf(*ch) : GeomOf(*shp);
    if ((u.drag >= 1 && u.drag <= 12 && u.drag_obj == me) || (u.preview_on && u.preview_obj == me)) gm = u.preview;
    if (u.more_moving && (u.drag == 1 || u.preview_on) && InMore(me)) {  // moved with the primary selection
      gm.l = std::max(0.0, gm.l + u.preview.l - u.g0.l);
      gm.t = std::max(0.0, gm.t + u.preview.t - u.g0.t);
    }
    // a protected sheet (without "Edit objects") keeps its locked objects in place (Excel); form controls still work
    const bool obj_locked = sh->protection->on && !sh->protection->objects && (is_ctl ? c->locked : ch ? ch->locked : shp->locked);
    // a chart sheet's chart fills the window
    const bool sheet_chart = ch && sh->chart_sheet;
    ImRect r = ScreenRect(g, pl, gm);
    if (sheet_chart) {
      r = g.cells;
      r.Expand(-S(12));
    }
    if (!r.Overlaps(g.cells)) continue;
    ImGui::PushID(static_cast<int>(me.id) * 4 + it.kind);
    const bool design = s.design_mode || !is_ctl;  // charts can be selected and moved in any mode

    ShapeObj shown_shape;
    if (shp && (AD().on || AD().pending) && AD().obj == me) {
      // its yellow handle being dragged: drawn with the new adjustments
      shown_shape = *shp;
      shown_shape.adj = AD().adj;
      shp = &shown_shape;
    } else if (shp && u.drag_obj == me && (u.drag == 11 || u.drag == 12 || u.rotating || (u.preview_on && u.preview_obj == me && u.preview_shape_on))) {
      // a line's end or the rotation being dragged: drawn as it will be
      shown_shape = *shp;
      shown_shape.flip_h = u.preview_flip_h;
      shown_shape.flip_v = u.preview_flip_v;
      shown_shape.rotation = u.preview_rot;
      shp = &shown_shape;
    }
    if (shp) {
      DrawShapeFx(dl, *shp, r, static_cast<float>(g.sc));  // with its Effects (shape_effects.cpp)
      if (u.sel == me) {
        u.sel_screen = r;
        u.sel_scale = static_cast<float>(g.sc);
      }
      // a shape with a macro (OnAction) runs it when clicked, outside Design Mode (right-click still selects it)
      if (!shp->on_action.empty() && !s.design_mode) {
        ImGui::SetCursorScreenPos(r.Min);
        ImGui::InvisibleButton("##shape_macro", ImMax(r.GetSize(), ImVec2(1, 1)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          if (trust::MacroBlocked(s, shp->on_action)) {
            // the workbook's code is disabled (Trust Center): Excel's "Cannot run the macro" message
          } else if (host_ready) s.python->RunMacro(shp->on_action);
          else s.SetStatus(TR("Python is not ready yet: ") + shp->on_action);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          u.sel = me;
          OpenObjectContext(me);
        }
        if (u.sel == me) {
          sel_rect = r;
          sel_visible = true;
          sel_pl = pl;
        }
        ImGui::PopID();
        continue;
      }
    }
    if (ch) {
      // in interactive mode ImPlot handles the mouse itself: keep only a draggable border around the frame
      const ChartObj* shown = ChartDataPreview(sheet_id, ch->id);  // Select Data's changes before OK
      if (!shown) shown = ChartElemPreview(sheet_id, ch->id);    // a + flyout choice under the mouse
      NoteChartRect(sheet_id, ch->id, r);
      DrawChartBody(s, sheet_id, shown ? *shown : *ch, r.Min, r.GetSize(), true, static_cast<float>(g.sc) / ui::Dpi());
      if ((shown ? shown : ch)->series.empty() && ch->pivot.empty() && !SelectDataOpen()) {
        if (DrawEmptyChartPrompt(s, sheet_id, ch->id, r)) u.sel = me;
      }
      if (!ch->chart_line_none) dl->AddRect(r.Min, r.Max, ch->chart_line ? ArgbCol(ch->chart_line) : IM_COL32(0xD9, 0xD9, 0xD9, 255));
      if (u.sel == me && !ch->interactive) {
        int es = 0, ep = -1;
        const model::ChartElem e = ElemOf(me, &es, &ep);
        DrawChartSelection(dl, u.charts[ch->id], e, es, ep);
      }
    }

    if (c && static_cast<int>(c->kind) >= static_cast<int>(ControlKind::CheckBox) && !s.design_mode) {
      DrawControlKind(s, sheet_id, *c, r, true);
      if (c->form && r.Contains(io.MousePos) && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        u.sel = me;
        OpenObjectContext(me);
      }
      ImGui::PopID();
      continue;
    }
    bool hovered = false, held = false;
    if (ch && ch->interactive) {
      // the border (5 px wide) is the drag handle
      const float bw = S(5);
      const ImRect bars[4] = {ImRect(r.Min, ImVec2(r.Max.x, r.Min.y + bw)), ImRect(ImVec2(r.Min.x, r.Max.y - bw), r.Max), ImRect(r.Min, ImVec2(r.Min.x + bw, r.Max.y)),
                              ImRect(ImVec2(r.Max.x - bw, r.Min.y), r.Max)};
      for (int k = 0; k < 4; ++k) {
        ImGui::SetCursorScreenPos(bars[k].Min);
        ImGui::PushID(k);
        ImGui::InvisibleButton("##edge", ImMax(bars[k].GetSize(), ImVec2(1, 1)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemActivated()) {
          u.sel = me;
          u.drag = 1;
          u.drag_obj = me;
          u.drag_mouse0 = io.MousePos;
          u.drag_sc = g.sc;
          u.g0 = gm;
          u.preview = gm;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) OpenObjectContext(me);
        ImGui::PopID();
      }
    } else if ((shp && !HitShape(*shp, r, io.MousePos) && !(u.drag && u.drag_obj == me)) || (u.drag == 0 && !u.rotating && OverHandle(sheet_id, io.MousePos))) {
      // only the shape itself takes the mouse (a line, the outline of an oval): cells around it stay clickable; the
      // selected object's handles (drawn after the objects) get the mouse before any object under them. The object is
      // still listed for the UI item recorder (a person sees it), without taking the mouse.
      const ImRect hr = shp ? RotBounds(r, shp->rotation) : r;
      const ImGuiID oid = ImGui::GetID("##obj");
      if (ImGui::ItemAdd(hr, oid) && GImGui->TestEngineHookItems) ImGuiTestEngineHook_ItemInfo(GImGui, oid, "##obj", 0);
    } else {
      const ImRect hr = shp ? RotBounds(r, shp->rotation) : r;
      ImGui::SetCursorScreenPos(hr.Min);
      ImGui::InvisibleButton("##obj", ImMax(hr.GetSize(), ImVec2(1, 1)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      hovered = ImGui::IsItemHovered();
      held = ImGui::IsItemActive();
      if (hovered && ch && !held && u.drag == 0) ChartTip(s, *ch, u.charts[ch->id], io.MousePos);
      if (design && obj_locked) {
        // a locked object of a protected sheet can't be selected, moved or formatted (Excel)
        if (ImGui::IsItemActivated() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) ShowProtectedMessage(s);
      } else if (design) {
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          const bool was_selected = u.sel == me;
          bool drag = SelectOnClick(*sh, me);
          if (ch && u.sel == me) {  // the element under the mouse is selected with the chart (like Excel)
            int es = 0, ep = -1, ces = 0, cep = -1;
            const model::ChartElem e = HitChartElem(u.charts[ch->id], io.MousePos, &es, &ep);
            const model::ChartElem cur = ElemOf(me, &ces, &cep);
            using E = model::ChartElem;
            // the second press of a double-click isn't a second click: it formats what the first selected (Excel: a
            // double-click on a bar opens Format Data Series, on the legend Format Legend); it chose the point and the
            // pane said Format Data Point
            const bool again = was_selected && io.MouseClickedCount[ImGuiMouseButton_Left] % 2 == 1;  // a third press is a click again (Windows alternates them)
            const bool label_again = again && e == E::DataLabels && ep >= 0 && (cur == E::DataLabels || cur == E::DataLabel) && ces == es;
            const bool entry_again = again && e == E::Legend && ep >= 0 && (cur == E::Legend || cur == E::LegendEntry);
            if (label_again || entry_again) {
              // a second click on the selected data labels or legend: the one label or entry under the mouse; a drag
              // then moves that label away from its point (or the whole legend), as in Excel
              const ChartCache::Hit& hh = u.charts[ch->id].hit;
              u.label_click_again = label_again && cur == E::DataLabel && cep == ep;
              SelectElem(me, label_again ? E::DataLabel : E::LegendEntry, es, ep);
              ImRect r0 = hh.legend;
              if (label_again && static_cast<size_t>(es) < hh.labels.size()) {
                for (size_t k = 0; k < hh.labels[static_cast<size_t>(es)].size() && k < hh.label_idx[static_cast<size_t>(es)].size(); ++k) {
                  if (hh.label_idx[static_cast<size_t>(es)][k] == ep) r0 = hh.labels[static_cast<size_t>(es)][k];
                }
              }
              if (!ch->interactive) {
                u.elem_drag = true;
                u.elem_drag_obj = me;
                u.elem_drag_e = label_again ? E::DataLabel : E::Legend;
                u.elem_drag_m0 = io.MousePos;
                u.elem_drag_r0 = r0;
                u.elem_drag_frame = hh.frame;
              }
              drag = false;
            } else if (again && e == E::Series && ep >= 0 && (cur == E::Series || cur == E::Point) && ces == es) {
              SelectElem(me, E::Point, es, ep);  // a second click on the selected series: the point under the mouse
            } else if (was_selected && !ch->interactive && (cur == E::Legend || cur == E::PlotArea) &&
                       ElemHandleAt(cur == E::Legend ? u.charts[ch->id].hit.legend : u.charts[ch->id].hit.plot, io.MousePos) >= 0) {
              // a handle of the selected legend or plot area: resized by it (manual layout)
              const ChartCache::Hit& hh = u.charts[ch->id].hit;
              u.elem_drag = true;
              u.elem_drag_obj = me;
              u.elem_drag_e = cur;
              u.elem_drag_m0 = io.MousePos;
              u.elem_drag_r0 = cur == E::Legend ? hh.legend : hh.plot_frame;
              u.elem_drag_frame = hh.frame;
              u.elem_drag_handle = ElemHandleAt(cur == E::Legend ? hh.legend : hh.plot, io.MousePos);
              drag = false;
            } else if (was_selected && cur == e &&
                       (e == E::Title || e == E::Legend || e == E::PlotArea || e == E::CatAxisTitle || e == E::ValAxisTitle || e == E::ValAxis2Title) &&
                       !ch->interactive) {
              // the selected title, legend, plot area or axis title is dragged inside the chart (manual layout)
              const ChartCache::Hit& hh = u.charts[ch->id].hit;
              u.elem_drag = true;
              u.elem_drag_obj = me;
              u.elem_drag_e = e;
              u.elem_drag_m0 = io.MousePos;
              u.elem_drag_r0 = e == E::Title ? hh.title : e == E::Legend ? hh.legend : e == E::CatAxisTitle ? hh.cat_title : e == E::ValAxisTitle ? hh.val_title
                               : e == E::ValAxis2Title ? hh.val2_title : hh.plot_frame;
              u.elem_drag_frame = hh.frame;
              u.elem_drag_handle = -1;
              drag = false;
            } else {
              SelectElem(me, e, es, ep);
            }
          }
          if (!sheet_chart && drag) {
            u.drag = 1;
            u.drag_obj = me;
            u.drag_mouse0 = io.MousePos;
            u.drag_sc = g.sc;
            u.g0 = gm;
            u.preview = gm;
            u.more_moving = !u.more.empty();
            u.preview_shape_on = false;
            u.drag_copy = io.KeyCtrl;  // Ctrl+drag drops a copy (Excel)
          }
          if (s.edit.active) CancelEdit(s);
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          u.sel = me;
          if (c && !c->form && s.design_mode) {
            // Excel's double-click on an ActiveX control in Design Mode: its default event's procedure
            u.drag = 0;
            ViewSelectedControlCode(s);
          } else if (shp && HasText(*shp)) {
            // edit the text in place
            StartShapeText(me, *shp, r, static_cast<float>(g.sc), "");
          } else if (int hs = 0, he = ch ? static_cast<int>(HitChartElem(u.charts[ch->id], io.MousePos, &hs)) : 0;
                     ch && (he == static_cast<int>(model::ChartElem::Title) || he == static_cast<int>(model::ChartElem::CatAxisTitle) ||
                            he == static_cast<int>(model::ChartElem::ValAxisTitle) || he == static_cast<int>(model::ChartElem::ValAxis2Title))) {
            // the chart title or an axis title: typed in place, as in Excel
            using E = model::ChartElem;
            const ChartCache& cc0 = u.charts[ch->id];
            const E te = static_cast<E>(he);
            ImRect tr = te == E::Title ? cc0.hit.title : te == E::CatAxisTitle ? cc0.hit.cat_title : te == E::ValAxisTitle ? cc0.hit.val_title : cc0.hit.val2_title;
            if (tr.GetHeight() <= 0) tr = ImRect(r.Min + ImVec2(0, S(6)), r.Min + ImVec2(r.GetWidth(), S(26)));
            const std::string& now = te == E::Title ? cc0.v.title : te == E::CatAxisTitle ? cc0.v.cat_title : te == E::ValAxisTitle ? cc0.v.val_title : cc0.v.val2_title;
            u.text_obj = me;
            u.text_elem = te == E::Title ? 0 : te == E::CatAxisTitle ? 1 : te == E::ValAxisTitle ? 2 : 3;
            SelectElem(me, te, 0);
            std::snprintf(u.text_buf, sizeof(u.text_buf), "%s", now.c_str());
            u.text_focus = true;
            const float w = std::max(S(160), std::min(r.GetWidth() - S(16), tr.GetWidth() + S(80)));
            u.text_pos = ImVec2(std::clamp(tr.GetCenter().x - w * 0.5f, r.Min.x + S(4), std::max(r.Min.x + S(4), r.Max.x - w - S(4))), tr.GetCenter().y - ImGui::GetFrameHeight() * 0.5f);
            u.text_size = ImVec2(w, ImGui::GetFrameHeight());
            u.drag = 0;
          } else {
            u.show_props = true;
          }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          if (!IsSelected(me)) {
            u.sel = me;
            u.more.clear();
            AddGroupOf(*sh, me);
          }
          if (ch) {
            int es = 0, ep = -1, ces = 0, cep = -1;
            const model::ChartElem e = HitChartElem(u.charts[ch->id], io.MousePos, &es, &ep);
            const model::ChartElem cur = ElemOf(me, &ces, &cep);
            // a right-click on the selected point keeps it (its menu has Set as Total, Add Data Label)
            if (!(cur == model::ChartElem::Point && e == model::ChartElem::Series && ces == es && cep == ep)) SelectElem(me, e, es);
          }
          OpenObjectContext(me);
        }
      } else if (c) {
        // run mode; form controls stay selectable with a right-click (Assign Macro, Format Control)
        if (c->form && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          u.sel = me;
          OpenObjectContext(me);
        }
        if (hovered && c->enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (c->kind == ControlKind::Button) {
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            // Click fires on release (like Windows buttons): detected with IsItemDeactivated
          }
          if (ImGui::IsItemDeactivated() && r.Contains(io.MousePos) && c->enabled && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (host_ready) FireControl(s, sheet_id, *c, "Click");
            else s.SetStatus(TR("Python is not ready yet: ") + c->name + TR(" Click event didn't run"));
          }
        } else if (ImGui::IsItemActivated() && c->enabled) {
          u.combo = me;
          bool ok = false;
          const std::string lt = LinkedText(s, sheet_id, c->linked_cell, &ok);
          std::snprintf(u.combo_buf, sizeof(u.combo_buf), "%s", ok ? lt.c_str() : c->value.c_str());
          u.combo_focus = true;
          ImGui::OpenPopupEx(kComboPopup);
          u.combo_pos = ImVec2(r.Min.x, r.Max.y);
          if (host_ready && !c->form) s.python->ControlEvent(sheet_id, c->name, "DropButtonClick", "");
        }
      }
    }

    if (c) {
      std::string text = c->caption;
      if (c->kind == ControlKind::ComboBox) {
        bool ok = false;
        const std::string lt = LinkedText(s, sheet_id, c->linked_cell, &ok);
        text = c->form ? ControlState(s, sheet_id, *c) : (ok ? lt : c->value);
      }
      if (static_cast<int>(c->kind) >= static_cast<int>(ControlKind::CheckBox)) DrawControlKind(s, sheet_id, *c, r, false);
      else DrawControlFace(*c, r, text, hovered && !s.design_mode, held && !s.design_mode, host_ready || s.design_mode);
      if (s.design_mode && !c->visible) dl->AddRectFilled(r.Min, r.Max, IM_COL32(255, 255, 255, 120));
    }
    if (u.sel == me && !sheet_chart) {
      sel_rect = r;
      sel_visible = true;
      sel_pl = pl;
    }
    ImGui::PopID();
  }

  // ---- Combo box list ----
  if (u.combo.sheet == sheet_id && u.combo.kind == 1) {
    const Control* c = nullptr;
    for (const auto& x : *sh->controls) {
      if (x.id == u.combo.id) c = &x;
    }
    if (c) {
      const float w = std::max(S(120), static_cast<float>(c->width * kPx * g.sc));
      ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(std::max(w, S(400)), S(22) * static_cast<float>(std::max(1, c->list_rows)) + S(40)));
      ImGui::SetNextWindowPos(u.combo_pos);
      if (ImGui::BeginPopupEx(kComboPopup, kPopupFlags & ~ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::vector<std::string> list = ControlItems(s, sheet_id, *c);
        if (c->style == 0) {
          if (u.combo_focus) {
            ImGui::SetKeyboardFocusHere();
            u.combo_focus = false;
          }
          ImGui::SetNextItemWidth(-1);
          if (ImGui::InputText("##combo_text", u.combo_buf, sizeof(u.combo_buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            int idx = -1;
            for (size_t i = 0; i < list.size(); ++i) {
              if (list[i] == u.combo_buf) idx = static_cast<int>(i);
            }
            ComboCommit(s, sheet_id, *c, u.combo_buf, idx);
            ImGui::CloseCurrentPopup();
          }
        }
        bool ok = false;
        const std::string cur = LinkedText(s, sheet_id, c->linked_cell, &ok);
        const std::string& current = ok ? cur : c->value;
        for (size_t i = 0; i < list.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          // editable style: filter by the typed text (MatchEntry)
          if (c->style == 0 && u.combo_buf[0] && Lower(list[i]).find(Lower(u.combo_buf)) == std::string::npos && list[i] != current) {
            ImGui::PopID();
            continue;
          }
          if (ImGui::Selectable(list[i].empty() ? " " : list[i].c_str(), list[i] == current)) {
            ComboCommit(s, sheet_id, *c, list[i], static_cast<int>(i));
          }
          ImGui::PopID();
        }
        if (list.empty()) ImGui::TextDisabled(TR("(no items)"));
        ImGui::EndPopup();
      } else {
        u.combo = {};
      }
    }
  }

  // ---- Selected object: 8 handles (a line: one at each end; a shape: the rotation handle above) ----
  // the other selected objects (Shift+click, a group) are outlined with their handles
  for (const ObjSel& m : u.more) {
    if (m.sheet != sheet_id) continue;
    Geom mg;
    Placement mp = Placement::MoveAndSize;
    double rot = 0;
    bool ok = false;
    if (m.kind == 3) {
      if (const ShapeObj* x = FindShape(*sh, m.id)) mg = GeomOf(*x), mp = x->placement, rot = x->rotation, ok = x->visible;
    } else if (m.kind == 2) {
      for (const auto& x : *sh->charts) {
        if (x.id == m.id && !x.floating) mg = GeomOf(x), mp = x.placement, ok = true;
      }
    } else {
      for (const auto& x : *sh->controls) {
        if (x.id == m.id) mg = GeomOf(x), mp = x.placement, ok = true;
      }
    }
    if (!ok) continue;
    if (u.more_moving && (u.drag == 1 || u.preview_on)) {
      mg.l = std::max(0.0, mg.l + u.preview.l - u.g0.l);
      mg.t = std::max(0.0, mg.t + u.preview.t - u.g0.t);
    }
    DrawHandles(dl, ScreenRect(g, mp, mg), IM_COL32(0x80, 0x80, 0x80, 255), rot);
  }
  if (u.handle_sheet == sheet_id) u.handle_pts.clear();
  if (sel_visible && u.sel.sheet == sheet_id) {
    u.handle_sheet = sheet_id;
    u.handle_pts.clear();
    const ShapeObj* sel_shape = u.sel.kind == 3 ? FindShape(*sh, u.sel.id) : nullptr;
    const bool line = sel_shape && IsLine(*sel_shape);
    double rot = sel_shape ? sel_shape->rotation : 0;
    if (sel_shape && (u.rotating || (u.preview_on && u.preview_obj == u.sel && u.preview_shape_on))) rot = u.preview_rot;
    if (line) {
      // a line: its two ends
      ShapeObj ls = *sel_shape;
      if (u.drag == 11 || u.drag == 12 || (u.preview_on && u.preview_obj == u.sel && u.preview_shape_on)) ls.flip_h = u.preview_flip_h, ls.flip_v = u.preview_flip_v;
      ImVec2 ends[2];
      LineEnds(ls, sel_rect, &ends[0], &ends[1]);
      for (int k = 0; k < 2; ++k) {
        const float hs = S(4.5f);
        dl->AddCircleFilled(ends[k], hs, IM_COL32_WHITE);
        dl->AddCircle(ends[k], hs, IM_COL32(0x80, 0x80, 0x80, 255));
        u.handle_pts.push_back(ends[k]);
        if (u.drag != 0 && u.drag != 11 + k) continue;
        ImGui::SetCursorScreenPos(ends[k] - ImVec2(S(5), S(5)));
        ImGui::PushID(1100 + k);
        ImGui::InvisibleButton("##line_end", ImVec2(S(10), S(10)));
        if (ImGui::IsItemHovered() || u.drag == 11 + k) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemActivated() && !(sh->protection->on && !sh->protection->objects && sel_shape->locked)) {
          u.drag = 11 + k;
          u.drag_obj = u.sel;
          u.drag_mouse0 = io.MousePos;
          u.drag_sc = g.sc;
          u.g0 = (u.preview_on && u.preview_obj == u.sel) ? u.preview : GeomOf(*sel_shape);
          u.preview = u.g0;
          u.preview_flip_h = ls.flip_h;
          u.preview_flip_v = ls.flip_v;
          u.preview_rot = sel_shape->rotation;
          u.more_moving = false;
        }
        ImGui::PopID();
      }
    } else {
      DrawHandles(dl, sel_rect, IM_COL32(0x80, 0x80, 0x80, 255), rot);
    }
    if (u.sel.kind == 2 && u.drag == 0) {  // a chart: Excel's Chart Elements and Chart Filters buttons beside it
      for (const auto& x : *sh->charts) {
        if (x.id == u.sel.id && !x.interactive) DrawChartFlyouts(s, u.sel, x, sel_rect, g.cells);
      }
    }
    // the rotation handle: a circle above the top edge
    if (sel_shape && !line && (u.drag == 0 || u.rotating)) {
      const ImVec2 c = sel_rect.GetCenter();
      const ImVec2 top((sel_rect.Min.x + sel_rect.Max.x) * 0.5f, sel_rect.Min.y);
      const ImVec2 hp = RotPt(top - ImVec2(0, S(20)), c, rot);
      dl->AddLine(RotPt(top, c, rot), hp, IM_COL32(0x80, 0x80, 0x80, 255));
      dl->AddCircleFilled(hp, S(5), IM_COL32_WHITE);
      dl->AddCircle(hp, S(5), IM_COL32(0x80, 0x80, 0x80, 255), 0, 1.5f);
      u.handle_pts.push_back(hp);
      ImGui::SetCursorScreenPos(hp - ImVec2(S(6), S(6)));
      ImGui::PushID(1200);
      ImGui::InvisibleButton("##rotate", ImVec2(S(12), S(12)));
      if (ImGui::IsItemHovered() || u.rotating) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsItemActivated() && !(sh->protection->on && !sh->protection->objects && sel_shape->locked)) {
        u.rotating = true;
        u.drag_obj = u.sel;
        u.rot_center = c;
        u.preview_rot = sel_shape->rotation;
        u.preview_flip_h = sel_shape->flip_h;
        u.preview_flip_v = sel_shape->flip_v;
      }
      ImGui::PopID();
    }
    // the yellow adjustment handles (their buttons were submitted before the objects)
    for (const ImVec2& p : AD().pts) {
      dl->AddCircleFilled(p, S(4.5f), IM_COL32(0xFF, 0xD9, 0x00, 255));
      dl->AddCircle(p, S(4.5f), IM_COL32(0x60, 0x60, 0x60, 255));
      u.handle_pts.push_back(p);
    }
    if (!line && (u.drag == 0 || (u.drag >= 2 && u.drag <= 9)) && !u.rotating) {
      for (int h = 0; h < 8; ++h) {
        const ImVec2 p = RotPt(HandlePos(sel_rect, h), sel_rect.GetCenter(), rot);
        u.handle_pts.push_back(p);
        const float hs = S(5);
        ImGui::SetCursorScreenPos(p - ImVec2(hs, hs));
        ImGui::PushID(1000 + h);
        ImGui::InvisibleButton("##handle", ImVec2(hs * 2, hs * 2));
        if (ImGui::IsItemHovered() || (u.drag == h + 2)) ImGui::SetMouseCursor(HandleCursor(h));
        if (ImGui::IsItemActivated()) {
          const Control* c = nullptr;
          const ChartObj* ch = nullptr;
          const ShapeObj* sp = sel_shape;
          if (u.sel.kind == 1) {
            for (const auto& x : *sh->controls) {
              if (x.id == u.sel.id) c = &x;
            }
          } else if (u.sel.kind == 2) {
            for (const auto& x : *sh->charts) {
              if (x.id == u.sel.id) ch = &x;
            }
          }
          const bool locked = sh->protection->on && !sh->protection->objects && (c ? c->locked : ch ? ch->locked : sp && sp->locked);
          if (locked) {
            ShowProtectedMessage(s);
          } else if (c || ch || sp) {
            u.drag = h + 2;
            u.drag_obj = u.sel;
            u.drag_mouse0 = io.MousePos;
            u.drag_sc = g.sc;
            u.g0 = (u.preview_on && u.preview_obj == u.sel) ? u.preview : (c ? GeomOf(*c) : ch ? GeomOf(*ch) : GeomOf(*sp));
            u.preview = u.g0;
            u.more_moving = false;
            u.preview_shape_on = false;
          }
        }
        ImGui::PopID();
      }
    }
  }
  (void)sel_pl;

  // ---- Rotating ----
  if (u.rotating && u.drag_obj.sheet == sheet_id) {
    const ImVec2 d = io.MousePos - u.rot_center;
    double deg = std::atan2(d.y, d.x) * 180.0 / 3.14159265358979323846 + 90.0;
    if (io.KeyShift) deg = std::round(deg / 15.0) * 15.0;
    deg = std::fmod(deg + 720.0, 360.0);
    if (std::fabs(deg - 360.0) < 1e-6) deg = 0;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      u.preview_rot = deg;
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else {
      const ObjSel o = u.drag_obj;
      const double v = u.preview_rot;
      u.rotating = false;
      u.preview_on = true;
      u.preview_obj = o;
      u.preview_shape_on = true;
      if (const ShapeObj* x = FindShape(*sh, o.id)) u.preview = GeomOf(*x);
      u.preview_until = s.engine->version();
      s.engine->PostUser(TR("Rotate"), [o, v](Document& d) { d.UpdateShape(o.sheet, o.id, [v](ShapeObj& x) { x.rotation = v; }); });
    }
  }

  // ---- Dragging ----
  if (u.drag >= 1 && u.drag <= 12 && u.drag_obj.sheet == sheet_id) {
    ImVec2 delta = io.MousePos - u.drag_mouse0;
    const ShapeObj* dshape = u.drag_obj.kind == 3 ? FindShape(*sh, u.drag_obj.id) : nullptr;
    // a turned shape is sized along its own sides
    if (dshape && dshape->rotation != 0 && u.drag >= 2 && u.drag <= 9) delta = RotPt(delta, ImVec2(0, 0), -dshape->rotation);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (u.drag == 11 || u.drag == 12) {
        // a line's end follows the mouse; the other end stays: the box and the diagonal come from the two points
        const Geom& g0 = u.g0;
        const bool fh0 = dshape && dshape->flip_h, fv0 = dshape && dshape->flip_v;
        const double bx = fh0 ? g0.l + g0.w : g0.l, by = fv0 ? g0.t + g0.h : g0.t;
        const double ex = fh0 ? g0.l : g0.l + g0.w, ey = fv0 ? g0.t : g0.t + g0.h;
        const double mx = delta.x / u.drag_sc / kPx, my = delta.y / u.drag_sc / kPx;
        double ax = bx, ay = by, cx = ex, cy = ey;
        if (u.drag == 11) ax = std::max(0.0, bx + mx), ay = std::max(0.0, by + my);
        else cx = std::max(0.0, ex + mx), cy = std::max(0.0, ey + my);
        if (io.KeyShift) {  // Shift: horizontal, vertical or 45 degrees
          double& fx = u.drag == 11 ? ax : cx;
          double& fy = u.drag == 11 ? ay : cy;
          const double ox = u.drag == 11 ? cx : ax, oy = u.drag == 11 ? cy : ay;
          const double ddx = fx - ox, ddy = fy - oy;
          if (std::fabs(ddx) > 2 * std::fabs(ddy)) fy = oy;
          else if (std::fabs(ddy) > 2 * std::fabs(ddx)) fx = ox;
          else {
            const double m = std::max(std::fabs(ddx), std::fabs(ddy));
            fx = ox + (ddx < 0 ? -m : m);
            fy = oy + (ddy < 0 ? -m : m);
          }
        }
        u.preview = Geom{std::min(ax, cx), std::min(ay, cy), std::fabs(cx - ax), std::fabs(cy - ay)};
        u.preview_flip_h = cx < ax;
        u.preview_flip_v = cy < ay;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      } else {
        if (std::fabs(delta.x) + std::fabs(delta.y) >= 2.0f || u.drag >= 2)
          u.preview = DragGeom(*sh, u.drag, u.g0, delta, u.drag_sc, (dshape && dshape->lock_aspect) || ChartLocksAspect(*sh, u.drag_obj));
        ImGui::SetMouseCursor(u.drag == 1 ? ImGuiMouseCursor_ResizeAll : HandleCursor(u.drag - 2));
      }
    } else {
      const Geom gnew = u.preview;
      const bool changed = std::fabs(gnew.l - u.g0.l) > 0.01 || std::fabs(gnew.t - u.g0.t) > 0.01 || std::fabs(gnew.w - u.g0.w) > 0.01 || std::fabs(gnew.h - u.g0.h) > 0.01;
      if (u.drag == 11 || u.drag == 12) {
        if (changed) {
          const ObjSel o = u.drag_obj;
          const bool fh = u.preview_flip_h, fv = u.preview_flip_v;
          u.preview_on = true;
          u.preview_obj = o;
          u.preview_until = s.engine->version();
          u.preview_shape_on = true;
          u.preview_rot = dshape ? dshape->rotation : 0;
          s.engine->PostUser(TR("Resize Object"), [o, gnew, fh, fv](Document& d) {
            d.UpdateShape(o.sheet, o.id, [&](ShapeObj& x) {
              x.left = gnew.l;
              x.top = gnew.t;
              x.width = gnew.w;
              x.height = gnew.h;
              x.flip_h = fh;
              x.flip_v = fv;
            });
          });
        }
      } else if (changed && u.drag == 1 && u.drag_copy) {
        // Ctrl+drag: the selection is copied to where it was dropped
        DuplicateSelection(s, gnew.l - u.g0.l, gnew.t - u.g0.t, TR("Copy Object"));
        u.more_moving = false;
      } else if (changed) {
        CommitGeom(s, u.drag_obj, gnew, u.drag == 1 ? TR("Move Object") : TR("Resize Object"));
        if (u.drag == 1 && !u.more.empty()) MoveMore(s, gnew.l - u.g0.l, gnew.t - u.g0.t);
      } else {
        u.more_moving = false;
      }
      u.drag = 0;
      u.drag_copy = false;
    }
  }
  if (!u.preview_on && u.drag == 0) u.more_moving = false;

  // ---- Design Mode: drawing a new control ----
  if (s.design_mode && u.insert_kind >= 0 && s.focus_sheet == sheet_id) {
    ImGui::SetCursorScreenPos(g.cells.Min);
    ImGui::InvisibleButton("##yc_insert", g.cells.GetSize());
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsItemActivated()) {
      u.drag = 10;
      u.drag_mouse0 = io.MousePos;
    }
    if (u.drag == 10) {
      const ImVec2 a = ImMin(u.drag_mouse0, io.MousePos), b = ImMax(u.drag_mouse0, io.MousePos);
      dl->AddRect(a, b, IM_COL32(0x40, 0x40, 0x40, 255));
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 sa = g.to_sheet(a), sb = g.to_sheet(b);
        Control c;
        c.kind = static_cast<ControlKind>(std::clamp(u.insert_kind, 0, model::kControlKinds - 1));
        c.form = u.insert_form;
        c.left = sa.x / kPx;
        c.top = sa.y / kPx;
        c.width = (sb.x - sa.x) / kPx;
        c.height = (sb.y - sa.y) / kPx;
        if (c.width < 8 || c.height < 6) {
          // a click: the kind's default size (points)
          static const double kSize[][2] = {{72, 24}, {96, 18}, {108, 18}, {108, 18}, {72, 24}, {96, 18}, {72, 18}, {96, 72}, {13, 26}, {13, 72}, {72, 72}, {144, 96}};
          c.width = kSize[static_cast<int>(c.kind)][0];
          c.height = kSize[static_cast<int>(c.kind)][1];
        }
        if (c.kind == ControlKind::Label) c.word_wrap = true;
        if (io.KeyAlt) {
          c.left = SnapX(*sh, c.left);
          c.top = SnapY(*sh, c.top);
        }
        const uint32_t id = s.engine->Call([sheet_id, c](Document& d) {
          d.Begin(TR("Insert Control"));
          const uint32_t v = d.AddControl(sheet_id, c);
          d.Commit();
          return v;
        });
        u.sel = ObjSel{sheet_id, 1, id};
        u.insert_kind = -1;
        u.drag = 0;
        if (c.form) {
          // like Excel: a new form button asks for its macro; form controls work outside Design Mode
          s.design_mode = false;
          if (auto* dm = ui::Commands().Find("DesignMode")) dm->pressed = false;
          if (c.kind == ControlKind::Button) OpenAssignMacro(u.sel, "");
          s.SetStatus(TR("Form control inserted: right-click it to assign a macro or to format it"));
        } else {
          s.SetStatus(TR("Control inserted: double-click it to write its events in VS Code; right-click for its properties"));
        }
        u.insert_form = false;
      }
    }
  }
  // ---- Edit Text of a form control ----
  if (CaptionEdit& ce = CE(); ce.obj && ce.obj.sheet == sheet_id) {
    const Control* cc = nullptr;
    for (const auto& x : *sh->controls) {
      if (x.id == ce.obj.id) cc = &x;
    }
    if (!cc) {
      ce.obj = {};
    } else {
      const ImRect r = ScreenRect(g, cc->placement, GeomOf(*cc));
      ImGui::SetCursorScreenPos(ImVec2(r.Min.x, cc->kind == ControlKind::GroupBox ? r.Min.y - ImGui::GetFrameHeight() * 0.5f : r.GetCenter().y - ImGui::GetFrameHeight() * 0.5f));
      const bool opening = ce.focus;
      if (ce.focus) ImGui::SetKeyboardFocusHere(), ce.focus = false;
      ImGui::SetNextItemWidth(std::max(S(80), r.GetWidth()));
      const bool enter = ImGui::InputText("##yc_caption_edit", ce.buf, sizeof(ce.buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      const bool esc = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);
      const bool away = !opening && (ImGui::IsItemDeactivated() || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()));
      if (enter || esc || away) {
        const ObjSel co = ce.obj;
        const std::string cap = ce.buf;
        ce.obj = {};
        if (cap != cc->caption) s.engine->PostUser(TR("Edit Text"), [co, cap](Document& d) { d.UpdateControl(co.sheet, co.id, [&](Control& x) { x.caption = cap; }); });
      }
    }
  }
  // ---- Editing a chart's title in place: Enter keeps it, Escape leaves it as it was ----
  if (u.text_obj && u.text_obj.sheet == sheet_id && u.text_obj.kind == 2) {
    ImGui::SetCursorScreenPos(u.text_pos);
    const bool opening = u.text_focus;  // the double-click that opened it isn't a click away
    if (u.text_focus) {
      ImGui::SetKeyboardFocusHere();
      u.text_focus = false;
    }
    static std::string initial;  // what the title read when the editing began (left as it is: a linked title stays linked)
    if (opening) initial = u.text_buf;
    ImGui::SetNextItemWidth(u.text_size.x);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 245));
    const bool enter = ImGui::InputText("##chart_title_edit", u.text_buf, sizeof(u.text_buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleColor();
    const bool esc = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool away = !opening && (ImGui::IsItemDeactivated() || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()));
    if (esc) {
      u.text_obj = {};
    } else if (enter || away) {
      const ObjSel o = u.text_obj;
      const std::string text = u.text_buf;
      const int which = u.text_elem;
      if (which == 4 && text != initial) {  // one data label: its own text from now on
        const int ls = u.text_series, lpnt = u.text_point;
        s.engine->PostUser(TR("Data Label"), [o, text, ls, lpnt](Document& d) {
          d.UpdateChart(o.sheet, o.id, [&](ChartObj& x) {
            if (ls < 0 || static_cast<size_t>(ls) >= x.series.size() || lpnt < 0) return;
            model::ChartPointFormat& pf = PointFormatRef(x.series[static_cast<size_t>(ls)], lpnt);
            pf.label_text = text;
            pf.label = 1;
          });
        });
      } else if (which != 4 && text != initial) {
        s.engine->PostUser(which == 0 ? TR("Chart Title") : TR("Axis Title"), [o, text, which](Document& d) {
          d.UpdateChart(o.sheet, o.id, [&](ChartObj& x) {
            std::string& t = which == 0 ? x.title : which == 1 ? x.cat_axis.title : which == 2 ? x.val_axis.title : x.val_axis2.title;
            bool& on = which == 0 ? x.has_title : which == 1 ? x.cat_axis.has_title : which == 2 ? x.val_axis.has_title : x.val_axis2.has_title;
            t = text;
            on = !text.empty();
          });
        });
      }
      u.text_obj = {};
      u.text_elem = 0;
    }
  }
  // ---- Dragging the selected title, legend or plot area inside its chart (kept as its manual layout) ----
  if (u.elem_drag && u.elem_drag_obj.sheet == sheet_id) {
    const ImVec2 d = io.MousePos - u.elem_drag_m0;
    ImRect nr = u.elem_drag_r0;
    const ImRect& fr = u.elem_drag_frame;
    if (const int hd = u.elem_drag_handle; hd >= 0) {
      // resizing by a handle: its sides follow the mouse, inside the chart, never smaller than a few pixels
      const float mn = S(16);
      if (hd == 0 || hd == 6 || hd == 7) nr.Min.x = std::clamp(nr.Min.x + d.x, fr.Min.x, nr.Max.x - mn);
      if (hd == 2 || hd == 3 || hd == 4) nr.Max.x = std::clamp(nr.Max.x + d.x, nr.Min.x + mn, fr.Max.x);
      if (hd == 0 || hd == 1 || hd == 2) nr.Min.y = std::clamp(nr.Min.y + d.y, fr.Min.y, nr.Max.y - mn);
      if (hd == 4 || hd == 5 || hd == 6) nr.Max.y = std::clamp(nr.Max.y + d.y, nr.Min.y + mn, fr.Max.y);
    } else {
      nr.Translate(d);
    }
    const ImVec2 sz = nr.GetSize();
    nr.Min = ImClamp(nr.Min, fr.Min, ImMax(fr.Min, fr.Max - sz));
    nr.Max = nr.Min + sz;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      dl->AddRect(nr.Min, nr.Max, IM_COL32(0x40, 0x40, 0x40, 255), 0.0f, 1.0f);
      ImGui::SetMouseCursor(u.elem_drag_handle >= 0 ? HandleCursor(u.elem_drag_handle) : ImGuiMouseCursor_ResizeAll);
    } else {
      u.elem_drag = false;
      u.elem_drag_handle = -1;
      if (u.label_click_again && u.elem_drag_e == model::ChartElem::DataLabel && std::fabs(d.x) + std::fabs(d.y) < 3) {
        // a click into the selected label: its text typed in place (Excel), Enter keeps it
        u.label_click_again = false;
        const auto cit = U().charts.find(u.elem_drag_obj.id);
        std::string now;
        if (cit != U().charts.end()) {
          const ChartCache::Hit& hh = cit->second.hit;
          const size_t i = static_cast<size_t>(std::max(0, u.elem_series));
          for (size_t k = 0; i < hh.label_idx.size() && k < hh.label_idx[i].size() && k < hh.label_texts[i].size(); ++k) {
            if (hh.label_idx[i][k] == u.elem_point) now = hh.label_texts[i][k];
          }
        }
        u.text_obj = u.elem_drag_obj;
        u.text_elem = 4;
        u.text_series = u.elem_series;
        u.text_point = u.elem_point;
        std::snprintf(u.text_buf, sizeof(u.text_buf), "%s", now.c_str());
        u.text_focus = true;
        const float w = std::max(S(120), u.elem_drag_r0.GetWidth() + S(40));
        u.text_pos = ImVec2(u.elem_drag_r0.GetCenter().x - w * 0.5f, u.elem_drag_r0.GetCenter().y - ImGui::GetFrameHeight() * 0.5f);
        u.text_size = ImVec2(w, ImGui::GetFrameHeight());
      }
      u.label_click_again = false;
      if (std::fabs(d.x) + std::fabs(d.y) >= 3 && fr.GetWidth() > 1 && fr.GetHeight() > 1) {
        ChartObj::ManualRect m;
        m.on = true;
        m.x = (nr.Min.x - fr.Min.x) / fr.GetWidth();
        m.y = (nr.Min.y - fr.Min.y) / fr.GetHeight();
        m.w = sz.x / fr.GetWidth();
        m.h = sz.y / fr.GetHeight();
        const ObjSel o = u.elem_drag_obj;
        const model::ChartElem e = u.elem_drag_e;
        // one data label: its offset from its place (fractions of the chart)
        const int les = u.elem_series, lep = u.elem_point;
        const ImVec2 frac((nr.Min.x - u.elem_drag_r0.Min.x) / fr.GetWidth(), (nr.Min.y - u.elem_drag_r0.Min.y) / fr.GetHeight());
        s.engine->PostUser(TR("Move Chart Element"), [o, e, m, les, lep, frac](Document& doc) {
          doc.UpdateChart(o.sheet, o.id, [&](ChartObj& x) {
            if (e == model::ChartElem::DataLabel) {
              if (les < 0 || static_cast<size_t>(les) >= x.series.size() || lep < 0) return;
              auto& pts = x.series[static_cast<size_t>(les)].points;
              auto it = std::find_if(pts.begin(), pts.end(), [&](const model::ChartPointFormat& q) { return q.index == lep; });
              if (it == pts.end()) {
                model::ChartPointFormat q;
                q.index = lep;
                pts.push_back(q);
                it = pts.end() - 1;
              }
              it->label_dx += frac.x;
              it->label_dy += frac.y;
              return;
            }
            if (e == model::ChartElem::Title) x.title_rect = m;
            else if (e == model::ChartElem::Legend) x.legend_rect = m;
            else if (e == model::ChartElem::CatAxisTitle) x.cat_title_rect = m;
            else if (e == model::ChartElem::ValAxisTitle) x.val_title_rect = m;
            else if (e == model::ChartElem::ValAxis2Title) x.val2_title_rect = m;
            else x.plot_rect = m;
          });
        });
      }
    }
  }
  // ---- Editing a shape's text: in place, in the shape's font; Escape and a click away keep what was typed (Excel) ----
  if (u.text_obj && u.text_obj.sheet == sheet_id && u.text_obj.kind != 2) {
    const ShapeObj* ts = FindShape(*sh, u.text_obj.id);
    const bool opening = u.text_focus;  // the double-click that opened it isn't a click away
    if (u.text_focus) {
      ImGui::SetKeyboardFocusHere();
      u.text_focus = false;
    }
    float pad_x = S(5), pad_y = S(3);
    ImFont* font = ui::F().ui;
    float font_px = ImGui::GetFontSize();
    if (ts) {
      const float k = static_cast<float>(kPx) * u.text_scale;
      pad_x = static_cast<float>(ts->margin_l) * k;
      pad_y = static_cast<float>(ts->margin_t) * k;
      font = ShapeFont(*ts);
      font_px = std::max(6.0f, ts->font_size * k);
    }
    ImGui::SetCursorScreenPos(u.text_pos);
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ts ? Argb(ts->font_color) : IM_COL32_BLACK);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushFont(font, font_px / std::max(0.01f, st.FontScaleDpi * st.FontScaleMain));
    const ImGuiID tid = ImGui::GetID("##shape_text");
    const bool esc = ImGui::GetActiveID() == tid && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const std::string typed = esc ? std::string(u.text_buf) : std::string();
    auto to_end = [](ImGuiInputTextCallbackData* data) {
      bool* flag = static_cast<bool*>(data->UserData);
      if (*flag) {
        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        *flag = false;
      }
      return 0;
    };
    ImGui::InputTextMultiline("##shape_text", u.text_buf, sizeof(u.text_buf), ImMax(u.text_size, ImVec2(S(40), S(20))), ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_WordWrap,
                              to_end, &u.text_cursor_end);
    if (esc) std::snprintf(u.text_buf, sizeof(u.text_buf), "%s", typed.c_str());
    ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    const bool active = ImGui::IsItemActive();
    const bool done = esc || (!opening && (ImGui::IsItemDeactivated() || (!active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered())));
    if (done) {
      const ObjSel o = u.text_obj;
      const std::string text = u.text_buf;
      if (!ts || text != ts->text) {
        double fit_h = -1;
        if (ts && ts->autofit) {
          ShapeObj t = *ts;
          t.text = text;
          FitShapeToText(t);
          fit_h = t.height;
        }
        s.engine->PostUser(TR("Edit Text"), [o, text, fit_h](Document& d) {
          d.UpdateShape(o.sheet, o.id, [&](ShapeObj& x) {
            x.text = text;
            if (fit_h > 0) x.height = fit_h;
          });
        });
      }
      u.text_obj = {};
      if (esc) u.sel = o;  // Escape leaves the shape selected (Excel)
    }
  }
  DrawGalleryPreview(s, sheet_id, g);
  DrawObjectContext(s);
}
bool ObjectsOwnKeyboard(AppState& s, uint32_t sheet_id) {
  const UiState& u = U();
  // typing into a shape or a text box control: the grid must not take the keys
  if ((u.text_obj && u.text_obj.sheet == sheet_id) || (u.tb_obj && u.tb_obj.sheet == sheet_id)) return true;
  if (SlicerSelected(sheet_id) && !s.edit.active) return true;  // Delete removes the slicer, not the cells
  return u.sel && u.sel.sheet == sheet_id && !s.edit.active && !u.text_obj;
}

namespace {

// whether a selected object is locked on its protected sheet (it can't be moved or formatted); shows Excel's message
bool SelectionProtected(AppState& s) {
  const UiState& u = U();
  const model::Sheet* sh = s.snap ? s.snap->Sheet(u.sel.sheet) : nullptr;
  if (!sh || !sh->protection->on || sh->protection->objects) return false;
  for (const ObjSel& o : SelectedList()) {
    bool locked = o.kind == 2;
    if (o.kind == 3) {
      if (const ShapeObj* x = FindShape(*sh, o.id)) locked = x->locked;
    } else if (o.kind == 2) {  // Format Chart Area > Properties > Locked
      for (const auto& c : *sh->charts) {
        if (c.id == o.id) locked = c.locked;
      }
    } else if (o.kind == 1) {
      for (const auto& c : *sh->controls) {
        if (c.id == o.id) locked = c.locked;
      }
    }
    if (locked) {
      ShowProtectedMessage(s);
      return true;
    }
  }
  return false;
}

}  // namespace

// the next chart element with the keyboard: the elements present in Excel's order (Up / Down), or the points of the
// selected series (Left / Right)
void StepChartElement(ObjSel o, const ChartObj& ch, int dx, int dy) {
  using E = model::ChartElem;
  int es = 0, ep = -1;
  const E cur = ElemOf(o, &es, &ep);
  const auto it = U().charts.find(ch.id);
  if (dx != 0 && (cur == E::Series || cur == E::Point) && it != U().charts.end() && static_cast<size_t>(es) < it->second.v.series.size()) {
    const int n = it->second.v.series[static_cast<size_t>(es)].N();
    if (n > 0) {
      const int p = cur == E::Series ? (dx > 0 ? 0 : n - 1) : (ep + dx + n) % n;
      SelectElem(o, E::Point, es, p);
      return;
    }
  }
  std::vector<std::pair<E, int>> list = {{E::ChartArea, 0}, {E::PlotArea, 0}};
  if (ch.has_title) list.push_back({E::Title, 0});
  const bool scene = engine::IsSceneChart(ch.type);
  const bool axes = !scene && ch.type != ChartType::Pie && ch.type != ChartType::Doughnut;
  if (axes) {
    if (ch.cat_axis.visible) list.push_back({E::CategoryAxis, 0});
    if (ch.val_axis.visible) list.push_back({E::ValueAxis, 0});
    bool y2 = false;
    for (const auto& se : ch.series) y2 = y2 || se.axis_group == 1;
    if (y2 && ch.val_axis2.visible) list.push_back({E::ValueAxis2, 0});
    if (ch.cat_axis.has_title) list.push_back({E::CatAxisTitle, 0});
    if (ch.val_axis.has_title) list.push_back({E::ValAxisTitle, 0});
    if (ch.val_axis.gridlines) list.push_back({E::MajorGridlines, 0});
  }
  if (ch.has_legend) list.push_back({E::Legend, 0});
  for (size_t i = 0; i < ch.series.size(); ++i) {
    if (ch.series[i].filtered) continue;
    list.push_back({E::Series, static_cast<int>(i)});
    if (!scene && ch.series[i].data_labels) list.push_back({E::DataLabels, static_cast<int>(i)});
    if (!scene && ch.series[i].trend != model::TrendKind::None) list.push_back({E::Trendline, static_cast<int>(i)});
  }
  const E base = cur == E::Point ? E::Series : cur;
  int at = 0;
  for (size_t k = 0; k < list.size(); ++k) {
    if (list[k].first == base && (base != E::Series && base != E::DataLabels && base != E::Trendline ? true : list[k].second == es)) at = static_cast<int>(k);
  }
  const int step = (dy != 0 ? dy : dx) > 0 ? 1 : -1;
  const int nl = static_cast<int>(list.size());
  at = (at + step + nl) % nl;
  SelectElem(o, list[static_cast<size_t>(at)].first, list[static_cast<size_t>(at)].second);
}

void HandleObjectKeys(AppState& s) {
  UiState& u = U();
  ImGuiIO& io = ImGui::GetIO();
  if (!u.sel || io.WantTextInput) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    // Escape during a move or resize puts the object back where it was (Excel); the mouse's release then does nothing
    if ((u.drag >= 1 && u.drag <= 9) || u.elem_drag) {
      u.drag = 0;
      u.elem_drag = false;
      u.preview_on = false;
      return;
    }
    // this Escape closed a menu or flyout (the chart's + and filter buttons): the object stays selected (Excel); the
    // next Escape lets go of it
    if (EscClosedPopup()) return;
    u.sel = {};
    u.more.clear();
    u.insert_kind = -1;
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
    if (!SelectionProtected(s)) DeleteSelected(s);
    return;
  }
  const model::Sheet* sh = s.snap->Sheet(u.sel.sheet);
  if (!sh) return;
  // typing (or F2) on a selected shape or text box starts editing its text, the typed characters added at its end
  if (u.sel.kind == 3 && u.more.empty() && !io.KeyCtrl && !io.KeyAlt) {
    const ShapeObj* sp = FindShape(*sh, u.sel.id);
    if (sp && HasText(*sp) && (io.InputQueueCharacters.Size > 0 || ImGui::IsKeyPressed(ImGuiKey_F2, false))) {
      if (SelectionProtected(s)) return;
      std::string typed;
      for (const ImWchar ch : io.InputQueueCharacters) {
        if (ch < 32) continue;
        char buf[5] = {};
        ImTextCharToUtf8(buf, ch);
        typed += buf;
      }
      if (g_placeholder_shape == sp->id && !typed.empty()) {
        ShapeObj blank = *sp;
        blank.text.clear();
        g_placeholder_shape = 0;
        StartShapeText(u.sel, blank, u.sel_screen, u.sel_scale, typed);
        return;
      }
      StartShapeText(u.sel, *sp, u.sel_screen, u.sel_scale, typed);
      return;
    }
  }
  double dx = 0, dy = 0;
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy = 1;
  if (dx == 0 && dy == 0) return;
  // a chart: Up / Down step through its elements and Left / Right through the selected series' points, as in Excel
  // (Ctrl or Shift + arrow still move or size it)
  if (u.sel.kind == 2 && u.more.empty() && !io.KeyCtrl && !io.KeyShift) {
    for (const auto& x : *sh->charts) {
      if (x.id == u.sel.id && !x.interactive) {
        StepChartElement(u.sel, x, static_cast<int>(dx), static_cast<int>(dy));
        return;
      }
    }
  }
  if (SelectionProtected(s)) return;
  // like Excel: an arrow moves the object a few pixels (Ctrl: one pixel); Shift+arrow sizes it
  const double step = io.KeyCtrl ? 0.75 : 3.0;
  const bool size = io.KeyShift;
  const std::vector<ObjSel> all = SelectedList();
  if (!size && u.preview_on) u.preview_on = false;
  s.engine->PostUser(size ? TR("Resize Object") : TR("Move Object"), [all, dx, dy, step, size](Document& d) {
    for (const ObjSel& o : all) {
      auto mv = [&](auto& x) {
        if (size) {
          x.width = std::max(6.0, x.width + dx * step);
          x.height = std::max(6.0, x.height + dy * step);
        } else {
          x.left = std::max(0.0, x.left + dx * step);
          x.top = std::max(0.0, x.top + dy * step);
        }
      };
      if (o.kind == 1) d.UpdateControl(o.sheet, o.id, mv);
      else if (o.kind == 3) d.UpdateShape(o.sheet, o.id, mv);
      else d.UpdateChart(o.sheet, o.id, mv);
    }
  });
}

void ClearObjectSelection() {
  ClearSlicerSelection();
  if (U().drag == 0) {
    U().sel = {};
    U().more.clear();
  }
}

bool HasSelectedChart() { return U().sel.kind == 2; }

// the event Excel's View Code opens for a control (its default event); each one is fired by the control's drawing above
static const char* DefaultControlEvent(ControlKind k) {
  switch (k) {
    case ControlKind::ComboBox:
    case ControlKind::TextBox:
    case ControlKind::SpinButton:
    case ControlKind::ScrollBar:
      return "Change";
    default:
      return "Click";
  }
}

bool ViewSelectedControlCode(AppState& s) {
  const ObjSel o = U().sel;
  if (o.kind != 1 || o.sheet != s.focus_sheet || !s.snap) return false;
  const model::Sheet* sh = s.snap->Sheet(o.sheet);
  if (!sh) return false;
  for (const auto& c : *sh->controls) {
    // form controls run a macro (Assign Macro), not sheet-module events
    if (c.id == o.id) {
      if (c.form) return false;
      ViewControlCode(s, o.sheet, c.name, DefaultControlEvent(c.kind));
      return true;
    }
  }
  return false;
}

// ---- Series from a range (Excel's inference: text in the first row = series names, text in the first column = categories) ----

namespace {

// the cells at positions idx[from..] along one column (vertical) or one row, as A1 areas joined by commas:
// "Sheet!$B$2:$B$9", or "Sheet!$B$2:$B$4,Sheet!$B$7:$B$9" when the positions skip
std::string LineA1(const std::string& sref, bool vertical, uint32_t fixed, const std::vector<uint32_t>& idx, size_t from) {
  std::string out;
  for (size_t i = from; i < idx.size();) {
    size_t j = i;
    while (j + 1 < idx.size() && idx[j + 1] == idx[j] + 1) ++j;
    const std::string a = vertical ? core::CellA1(idx[i], fixed, true, true) : core::CellA1(fixed, idx[i], true, true);
    const std::string b = vertical ? core::CellA1(idx[j], fixed, true, true) : core::CellA1(fixed, idx[j], true, true);
    if (!out.empty()) out += ",";
    out += sref + a + (i == j ? "" : ":" + b);
    i = j + 1;
  }
  return out;
}

}  // namespace

std::string AreasAddress(const model::Workbook& wb, uint32_t sheet, const std::vector<Rect>& areas) {
  const model::Sheet* sh = wb.SheetById(sheet);
  if (!sh) return {};
  const std::string sref = core::QuoteSheetName(sh->name) + "!";
  std::string out;
  for (const Rect& r : areas) {
    if (!out.empty()) out += ",";
    out += sref + core::CellA1(r.r0, r.c0, true, true);
    if (!r.IsCell()) out += ":" + core::CellA1(r.r1, r.c1, true, true);
  }
  return out;
}

bool ResolveAreasText(const model::Workbook& wb, uint32_t default_sheet, std::string_view text, uint32_t* sheet, std::vector<Rect>* areas) {
  areas->clear();
  while (!text.empty() && (text.front() == ' ' || text.front() == '=')) text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')') text = text.substr(1, text.size() - 2);  // =(A1:A3,C1:C3)
  // split at the commas outside quoted sheet names
  bool quoted = false;
  size_t from = 0;
  uint32_t sid0 = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i < text.size() && text[i] == '\'') quoted = !quoted;
    if (i < text.size() && (quoted || text[i] != ',')) continue;
    uint32_t sid = 0;
    Rect r;
    if (!ResolveRangeText(wb, areas->empty() ? default_sheet : sid0, text.substr(from, i - from), &sid, &r)) return false;
    if (!areas->empty() && sid != sid0) return false;  // every area on one sheet
    sid0 = sid;
    areas->push_back(r);
    from = i + 1;
  }
  if (areas->empty()) return false;
  *sheet = sid0;
  return true;
}

bool PlotByRows(const std::vector<Rect>& areas) {
  // Excel: series in rows when the block is wider than it is tall
  if (areas.empty()) return false;
  bool same_rows = true;
  for (const Rect& r : areas) same_rows = same_rows && r.r0 == areas[0].r0 && r.r1 == areas[0].r1;
  uint32_t rows = 0, cols = 0;
  if (same_rows) {
    rows = areas[0].r1 - areas[0].r0 + 1;
    for (const Rect& r : areas) cols += r.c1 - r.c0 + 1;
  } else {
    cols = areas[0].c1 - areas[0].c0 + 1;
    for (const Rect& r : areas) rows += r.r1 - r.r0 + 1;
  }
  return cols > rows;
}

bool BuildSeriesFromAreas(const model::Workbook& wb, const model::Sheet& sh, std::vector<Rect> areas, ChartType type, ChartObj* ch, bool by_rows) {
  for (Rect& r : areas) {
    if (sh.has_used) {
      r.r1 = std::min(r.r1, sh.used.r1);
      r.c1 = std::min(r.c1, sh.used.c1);
    }
  }
  areas.erase(std::remove_if(areas.begin(), areas.end(), [](const Rect& r) { return r.r1 < r.r0 || r.c1 < r.c0; }), areas.end());
  if (areas.empty()) return false;
  // areas side by side (the same rows: Ctrl+selected X and Y columns) or one above the other (the same columns) form
  // one block; others: the box around them
  bool same_rows = true, same_cols = true;
  for (const Rect& r : areas) {
    same_rows = same_rows && r.r0 == areas[0].r0 && r.r1 == areas[0].r1;
    same_cols = same_cols && r.c0 == areas[0].c0 && r.c1 == areas[0].c1;
  }
  std::vector<uint32_t> rows, cols;
  // the rows and columns the areas take, each once: a find per index made a chart over whole columns, with a value in
  // the sheet's last row, take a million squared steps (25 s; scale_sweep)
  auto span = [](std::vector<uint32_t>& v, uint32_t a, uint32_t b) {
    for (uint32_t k = a; k <= b; ++k) v.push_back(k);
  };
  if (same_rows) {
    span(rows, areas[0].r0, areas[0].r1);
    for (const Rect& r : areas) span(cols, r.c0, r.c1);
  } else if (same_cols) {
    span(cols, areas[0].c0, areas[0].c1);
    for (const Rect& r : areas) span(rows, r.r0, r.r1);
  } else {
    Rect b = areas[0];
    for (const Rect& r : areas) b = Rect{std::min(b.r0, r.r0), std::min(b.c0, r.c0), std::max(b.r1, r.r1), std::max(b.c1, r.c1)};
    span(rows, b.r0, b.r1);
    span(cols, b.c0, b.c1);
  }
  for (auto* v : {&rows, &cols}) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  }
  const std::string sheet_ref = core::QuoteSheetName(sh.name) + "!";
  auto is_text = [&](uint32_t row, uint32_t col) {
    const core::Value v = sh.GetValue(row, col);
    return v.IsString() || v.IsEmpty();
  };
  // i runs along a series (its points), j across the series; when series are in rows, rows and columns swap
  const std::vector<uint32_t>& along = by_rows ? cols : rows;
  const std::vector<uint32_t>& across = by_rows ? rows : cols;
  auto row_of = [&](uint32_t i, uint32_t j) { return by_rows ? across[j] : along[i]; };
  auto col_of = [&](uint32_t i, uint32_t j) { return by_rows ? along[i] : across[j]; };
  auto at_text = [&](uint32_t i, uint32_t j) { return is_text(row_of(i, j), col_of(i, j)); };
  // dates label the categories too (a Date | Open | High | Low | Close block), as in Excel
  auto at_date = [&](uint32_t i, uint32_t j) {
    const uint32_t row = row_of(i, j), col = col_of(i, j);
    return sh.GetValue(row, col).IsNumber() && formula::IsDateFormat(wb.styles->NumberFormat(wb.styles->Get(sh.GetStyle(row, col))));
  };
  const uint32_t R = static_cast<uint32_t>(along.size()), C = static_cast<uint32_t>(across.size());
  // more than 8 million points (the whole sheet with a value in its last cell: 255 series of a million points) is an
  // empty chart, as a selection with nothing to chart, and Select Data picks its range: drawn, such a chart took
  // every frame for seconds, and Excel 16 didn't answer for minutes (measured)
  constexpr uint64_t kMaxChartPoints = 8000000;
  if (uint64_t{R} * std::min<uint64_t>(C, 255) > kMaxChartPoints) return false;
  bool header = R > 1;
  for (uint32_t j = (C > 1 ? 1 : 0); j < C && header; ++j) header = at_text(0, j);
  bool cats = C > 1;
  for (uint32_t i = header ? 1 : 0; i < R && cats; ++i) cats = at_text(i, 0) || at_date(i, 0) || engine::IsScatterType(type);
  if (engine::IsScatterType(type) && C > 1) cats = true;
  // a blank top-left cell: the first row names the series and the first column labels the categories, also when they
  // hold numbers (years across the top), as in Excel
  if (R > 1 && C > 1 && sh.GetValue(row_of(0, 0), col_of(0, 0)).IsEmpty()) header = cats = true;
  // a stock chart has four price series (open, high, low, close): a fifth column in front labels the points, also
  // when its dates aren't formatted as dates (Excel refused such a block; it is a Date | O | H | L | C table)
  if (type == ChartType::StockOHLC && C == 5) cats = true;
  if ((type == ChartType::StockVHLC && C == 5) || (type == ChartType::StockVOHLC && C == 6)) cats = true;  // Date | Volume | (Open) | High...
  const uint32_t d0 = header ? 1 : 0;
  if (d0 >= R) return false;
  ch->type = type;
  ch->series.clear();
  ch->source = AreasAddress(wb, sh.id, areas);
  ch->source_by_rows = by_rows;
  // series j's points: the cells along it from the first data position
  auto line = [&](uint32_t j) { return LineA1(sheet_ref, !by_rows, across[j], along, d0); };
  auto cell = [&](uint32_t i, uint32_t j) { return sheet_ref + core::CellA1(row_of(i, j), col_of(i, j), true, true); };
  if ((type == ChartType::Treemap || type == ChartType::Sunburst || type == ChartType::FilledMap) && !by_rows) {
    // the leading text columns are the category levels, the next column holds the values
    uint32_t k = 0;
    while (k + 1 < C) {
      bool all = true;
      for (uint32_t i = d0; i < R && all; ++i) all = at_text(i, k);
      if (!all) break;
      ++k;
    }
    const bool contiguous = k >= 1 && cols[k - 1] - cols[0] == k - 1 && rows.back() - rows[d0] == R - 1 - d0;
    if (k >= 1 && contiguous) {
      model::ChartSeries se;
      se.values = line(k);
      se.x_values = sheet_ref + core::CellA1(rows[d0], cols[0], true, true) + ":" + core::CellA1(rows.back(), cols[k - 1], true, true);
      if (header) se.name = "=" + cell(0, k);
      ch->series.push_back(std::move(se));
      ch->has_legend = type == ChartType::Treemap || type == ChartType::FilledMap;
      // a new chart has a title: the series' name, linked to its cell (or "Chart Title")
      ch->has_title = true;
      ch->title = header ? "=" + cell(0, k) : std::string();
      return true;
    }
  }
  // at most 255 series, Excel's limit for a chart (over the whole sheet: 16,383 columns of a million points; Excel 16
  // didn't answer for minutes there, measured)
  constexpr size_t kMaxSeries = 255;
  for (uint32_t j = cats ? 1 : 0; j < C && ch->series.size() < kMaxSeries; ++j) {
    model::ChartSeries se;
    se.values = line(j);
    if (cats) se.x_values = line(0);
    if (header) se.name = "=" + cell(0, j);
    // a bubble chart: X in the first column, then each series' Y values and its bubble sizes
    if (type == ChartType::Bubble && j + 1 < C) se.bubble_sizes = line(++j);
    ch->series.push_back(std::move(se));
    if (type == ChartType::Pie || type == ChartType::Pie3D || type == ChartType::PieOfPie || type == ChartType::BarOfPie) break;  // (a doughnut has a ring per series)
  }
  if (ch->series.empty()) return false;
  ch->has_legend = ch->series.size() > 1 || type == ChartType::Pie || type == ChartType::Doughnut || type == ChartType::Waterfall || type == ChartType::Treemap ||
                   type == ChartType::FilledMap || type == ChartType::PieOfPie || type == ChartType::BarOfPie;
  if (type == ChartType::Histogram || type == ChartType::Pareto || type == ChartType::Funnel || type == ChartType::Sunburst) ch->has_legend = type == ChartType::Pareto;
  // Excel shows the values on waterfall and funnel charts
  if (type == ChartType::Waterfall || type == ChartType::Funnel) {
    for (auto& se : ch->series) se.data_labels = true;
  }
  // a new chart has a title: one series' name linked to its cell, otherwise "Chart Title" (an empty title)
  ch->has_title = true;
  ch->title = ch->series.size() == 1 && header ? "=" + cell(0, cats ? 1 : 0) : std::string();
  return true;
}

bool BuildSeriesFromRange(const model::Workbook& wb, const model::Sheet& sh, Rect r, ChartType type, ChartObj* ch, bool by_rows) {
  return BuildSeriesFromAreas(wb, sh, {r}, type, ch, by_rows);
}

void KeepSeriesFormat(const std::vector<model::ChartSeries>& old, std::vector<model::ChartSeries>* fresh) {
  for (size_t i = 0; i < old.size() && i < fresh->size(); ++i) {
    model::ChartSeries f = old[i];
    model::ChartSeries& n = (*fresh)[i];
    f.name = n.name;
    f.values = n.values;
    f.x_values = n.x_values;
    f.python_data = false;
    f.data = nullptr;
    f.topic.clear();
    n = std::move(f);
  }
}

// only one cell selected: expand to the current region (the block bounded by blank rows and columns, Excel's Ctrl+*)
Rect ExpandToRegion(const model::Sheet& sh, Rect r) {
  if (!r.IsCell()) return r;
  return engine::CurrentRegion(sh, r.r0, r.c0);
}

namespace {
bool Hierarchical(ChartType t) { return t == ChartType::Treemap || t == ChartType::Sunburst || t == ChartType::FilledMap; }
bool LabelsByDefault(ChartType t) { return t == ChartType::Waterfall || t == ChartType::Funnel; }
}  // namespace

void ChangeChartType(const model::Workbook& wb, ChartObj& c, ChartType t) {
  const ChartType old = c.type;
  c.type = t;
  if (old == t) return;
  // another kind of chart has its own Chart Styles set: Style 1 of it (Excel's Chart.ChartStyle follows the type)
  if (engine::ChartStyleNumber(old) != engine::ChartStyleNumber(t) || engine::ChartStyleCount(old) != engine::ChartStyleCount(t)) c.style = 0;
  // treemaps and sunbursts read the leading text columns as category levels: lay the source out again
  if ((Hierarchical(old) != Hierarchical(t) || (old == ChartType::Bubble) != (t == ChartType::Bubble)) && !c.source.empty() && c.pivot.empty()) {
    uint32_t sid = 0;
    std::vector<Rect> areas;
    if (ResolveAreasText(wb, 0, c.source, &sid, &areas)) {
      if (const model::Sheet* sh = wb.SheetById(sid)) {
        ChartObj tmp = c;
        if (BuildSeriesFromAreas(wb, *sh, areas, t, &tmp, c.source_by_rows)) {
          KeepSeriesFormat(c.series, &tmp.series);
          c.series = tmp.series;
          c.has_legend = tmp.has_legend;
          if (!c.has_title || c.title.empty()) {
            c.has_title = tmp.has_title;
            c.title = tmp.title;
          }
        }
      }
    }
  }
  if (LabelsByDefault(t) && !LabelsByDefault(old)) {
    for (auto& se : c.series) se.data_labels = true;
  }
}

bool BuildChartFromSelection(AppState& s, ChartType type, ChartObj* ch, Rect* anchor) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return false;
  // one cell: its current region; several areas (Ctrl+selected X and Y columns): all of them, as in Excel
  std::vector<Rect> areas = SelAreas(s);
  if (areas.size() <= 1) areas = {ExpandToRegion(*sh, SelRect(s))};
  Rect r = areas.back();
  for (const Rect& a : areas) r = Rect{std::min(r.r0, a.r0), std::min(r.c0, a.c0), std::max(r.r1, a.r1), std::max(r.c1, a.c1)};
  *ch = ChartObj{};
  // blank cells hold nothing to chart: Excel inserts an empty chart, and the user selects its data
  uint64_t filled = 0;
  for (const Rect& a : areas) filled += engine::ComputeStats(*sh, a).count;
  // a stock table (Date | Open | High | Low | Close) keeps its prices in columns, also when it has few rows
  const bool by_rows = engine::IsStockType(type) ? false : PlotByRows(areas);  // (a stock table keeps its prices in columns)
  if (filled == 0 || !BuildSeriesFromAreas(s.snap->wb, *sh, areas, type, ch, by_rows)) {
    // nothing to chart there: an empty chart, filled with Select Data (like Excel)
    *ch = ChartObj{};
    ch->type = type;
    ch->has_legend = false;
    *anchor = SelRect(s);
    return false;
  }
  *anchor = r;
  return true;
}

// where a new chart goes: in the middle of what the window shows, as Excel places it (beside the data when the window
// size isn't known yet)
void PlaceNewChart(AppState& s, const model::Sheet* sh, ChartObj& ch, Rect r) {
  double x = static_cast<double>(sh->cols->Offset(static_cast<int64_t>(r.c1) + 2));
  double y = static_cast<double>(sh->rows->Offset(r.r0));
  if (const render::GridView& v = s.FocusView(); v.px_scale > 0 && v.cells_rect.GetWidth() > 0) {
    const double vx = static_cast<double>(sh->cols->Offset(v.h.first)), vy = static_cast<double>(sh->rows->Offset(v.v.first));
    const double vw = v.cells_rect.GetWidth() / v.px_scale, vh = v.cells_rect.GetHeight() / v.px_scale;
    const double w = ch.width * kPx, h = ch.height * kPx;
    x = vx + std::max(0.0, (vw - w) / 2);
    y = vy + std::max(0.0, (vh - h) / 2);
  }
  ch.left = x / kPx;
  ch.top = y / kPx;
}

void InsertBuiltChart(AppState& s, ChartObj ch, Rect r) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  const bool empty = ch.series.empty();
  PlaceNewChart(s, sh, ch, r);
  const uint32_t sheet = sh->id;
  const uint32_t id = s.engine->Call([sheet, ch](Document& d) {
    d.Begin(TR("Insert Chart"));
    const uint32_t v = d.AddChart(sheet, ch);
    d.Commit();
    return v;
  });
  U().sel = ObjSel{sheet, 2, id};
  if (empty) s.SetStatus(TR("Empty chart inserted: click Select Data in it, then drag over the data on the sheet"));
  else s.SetStatus(TR("Chart inserted: its data is outlined on the sheet; drag the outline's corners to change it, or right-click > Select Data"));
}

// the Insert tab's chart commands and the chart each inserts (a gallery entry under the mouse is previewed)
std::map<std::string, ChartType>& GalleryChartTypes() {
  static std::map<std::string, ChartType> m;
  return m;
}

// Excel's live preview: while the mouse rests on an entry of an Insert > Charts gallery, that chart is shown over the
// sheet where it would go, built from the selection; nothing is inserted until the entry is clicked
namespace {
void SetChartElemPreview(ObjSel o, ChartObj ch);
}  // namespace

void DrawGalleryPreview(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  static ChartObj pv;
  static std::string pv_key;
  static bool pv_ok = false;
  const std::string& tile = ui::ribbon::St().hovered_tile;
  // Chart Design's galleries (Chart Styles, Change Colors, Quick Layout): the entry under the mouse shown on the
  // selected chart until the mouse leaves it
  if (U().sel.kind == 2 && U().sel.sheet == sheet_id && s.snap) {
    auto num = [&](const char* prefix) {
      const size_t n = std::strlen(prefix);
      return tile.size() > n && tile.compare(0, n, prefix) == 0 && std::isdigit(static_cast<unsigned char>(tile[n])) ? std::atoi(tile.c_str() + n) : -1;
    };
    const int st = num("ChartStyle"), co = num("ChartColors"), la = num("ChartQuickLayout");
    if (st > 0 || co >= 0 || la > 0) {
      if (const model::Sheet* sh = s.snap->Sheet(sheet_id)) {
        for (const auto& c : *sh->charts) {
          if (c.id != U().sel.id) continue;
          ChartObj copy = c;
          if (st > 0) engine::ApplyChartStyle(copy, st);
          if (co >= 0) copy.color_scheme = std::clamp(co, 0, engine::kChartColorSchemes - 1);
          if (la > 0) engine::ApplyChartQuickLayout(copy, la);
          SetChartElemPreview(U().sel, std::move(copy));
        }
      }
      return;
    }
  }
  const auto it = tile.empty() ? GalleryChartTypes().end() : GalleryChartTypes().find(tile);
  if (it == GalleryChartTypes().end() || sheet_id != s.focus_sheet || s.edit.active || !s.snap) return;
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  const Rect sel = SelRect(s);
  const std::string key = tile + "|" + std::to_string(s.snap->version) + "|" + std::to_string(sel.r0) + "," + std::to_string(sel.c0) + "," +
                          std::to_string(sel.r1) + "," + std::to_string(sel.c1);
  if (key != pv_key) {
    pv_key = key;
    Rect anchor{};
    pv = ChartObj{};
    pv_ok = BuildChartFromSelection(s, it->second, &pv, &anchor);
    if (pv_ok) {
      pv.id = 0xFFFFFF00u;  // its own drawing cache
      pv.name = "##preview";
      PlaceNewChart(s, sh, pv, anchor);
    }
  }
  if (!pv_ok) return;
  const ImRect r = ScreenRect(g, pv.placement, GeomOf(pv));
  if (!r.Overlaps(g.cells)) return;
  DrawChartBody(s, sheet_id, pv, r.Min, r.GetSize(), false, static_cast<float>(g.sc) / ui::Dpi());
  ImGui::GetWindowDrawList()->AddRect(r.Min, r.Max, IM_COL32(0xD9, 0xD9, 0xD9, 255));
  ui::NoteUiItem(TR("Chart Preview"), r);  // tests see it as a person does
}

void InsertChartFromSelection(AppState& s, ChartType type, bool combo) {
  ChartObj ch;
  Rect r;
  BuildChartFromSelection(s, type, &ch, &r);
  // Clustered Column - Line: the last series becomes a line
  if (combo && ch.series.size() >= 2) ch.series.back().type = static_cast<int>(ChartType::Line);
  InsertBuiltChart(s, std::move(ch), r);
}

const char* ChartTypeName(ChartType t) { return ChartTypeNames()[std::clamp(static_cast<int>(t), 0, model::kChartTypeCount - 1)]; }

void SelectInsertedShape(AppState& s, uint32_t sheet, uint32_t id) {
  if (!id) return;
  U().sel = ObjSel{sheet, 3, id};
  // the keyboard goes back to the sheet (the ribbon button had it), so Delete and the arrows act on the new shape
  ImGui::SetWindowFocus(s.Win(sheet).floating ? s.SheetWindowName(sheet).c_str() : kGridHostWindow);
}

// Go To Special > Objects (and Find & Select > Select Objects' Ctrl+A): every object on the sheet that is shown, the
// first one primary; returns how many
int SelectAllObjects(AppState& s, uint32_t sheet) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return 0;
  std::vector<ObjSel> v;
  for (const auto& x : *sh->shapes) {
    if (x.visible) v.push_back(ObjSel{sheet, 3, x.id});
  }
  for (const auto& x : *sh->charts) v.push_back(ObjSel{sheet, 2, x.id});
  for (const auto& x : *sh->controls) {
    if (x.visible && x.placement != Placement::FloatWindow) v.push_back(ObjSel{sheet, 1, x.id});
  }
  UiState& u = U();
  u.more.clear();
  if (v.empty()) return 0;
  u.sel = v.front();
  u.more.assign(v.begin() + 1, v.end());
  ImGui::SetWindowFocus(s.Win(sheet).floating ? s.SheetWindowName(sheet).c_str() : kGridHostWindow);
  return static_cast<int>(v.size());
}

// ---- Properties window ----

namespace {

// a cell / range property (LinkedCell, ListFillRange): typed, or picked on the sheet with the box's button. Its text
// lives between frames (a pick arrives over several frames); the property changes once the pick or the typing is done
bool EditRefProp(AppState& s, const char* label, const std::string& key, std::string& v, int flags = 0) {
  static std::map<std::string, std::string> bufs, loaded;
  std::string& b = bufs[key];
  std::string& from = loaded[key];
  if (from != v && !RangePicking()) b = from = v;
  ImGui::TextUnformatted(label);
  RangeBox(s, key.c_str(), &b, 0, flags, label);
  if (b != v && !RangePicking() && !ImGui::IsAnyItemActive()) {
    v = from = b;
    return true;
  }
  return false;
}

// a cell / range property in a grid cell (no label above it)
bool EditRefPropCell(AppState& s, const std::string& key, std::string& v, bool cell = true) {
  static std::map<std::string, std::string> bufs, loaded;
  std::string& b = bufs[key];
  std::string& from = loaded[key];
  if (from != v && !RangePicking()) b = from = v;
  RangeBox(s, key.c_str(), &b, 0, cell ? RangeBox_Cell : 0, key.c_str());
  if (b != v && !RangePicking() && !ImGui::IsAnyItemActive()) {
    v = from = b;
    return true;
  }
  return false;
}

bool EditText(const char* label, std::string& v, bool multiline = false) {
  char buf[4096];
  std::snprintf(buf, sizeof(buf), "%s", v.c_str());
  ImGui::SetNextItemWidth(-1);
  bool done = false;
  ImGui::TextUnformatted(label);
  ImGui::PushID(label);
  if (multiline) {
    ImGui::InputTextMultiline("##v", buf, sizeof(buf), ImVec2(-1, S(70)));
  } else {
    ImGui::InputText("##v", buf, sizeof(buf));
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    v = buf;
    done = true;
  }
  ImGui::PopID();
  return done;
}

// a number with step buttons (Excel's spin boxes); fmt shows the unit ("%.2f pt")
bool EditNum(const char* label, double& v, double step = 0, const char* fmt = "%.2f") {
  ImGui::SetNextItemWidth(S(150));
  const double before = v;
  ImGui::InputDouble(label, &v, step, step * 10, fmt);
  // a step button: at once; typing: when the box is left
  return (v != before && !ImGui::GetIO().WantTextInput) || ImGui::IsItemDeactivatedAfterEdit();
}

bool SpinInt(const char* label, int& v, int lo, int hi) {
  ImGui::SetNextItemWidth(S(150));
  const int before = v;
  ImGui::InputInt(label, &v, 1, 10);
  const bool done = (v != before && !ImGui::GetIO().WantTextInput) || ImGui::IsItemDeactivatedAfterEdit();
  v = std::clamp(v, lo, hi);
  return done;
}

bool SpinFloat(const char* label, float& v, float step, float lo, float hi, const char* fmt) {
  ImGui::SetNextItemWidth(S(150));
  const float before = v;
  ImGui::InputFloat(label, &v, step, step * 4, fmt);
  const bool done = (v != before && !ImGui::GetIO().WantTextInput) || ImGui::IsItemDeactivatedAfterEdit();
  v = std::clamp(v, lo, hi);
  return done;
}

// a series' chart type in a combo chart (Format Data Series)
bool EditSeriesFormat(AppState& s, const ChartObj& m, model::ChartSeries& se, const engine::ChartValues* vals, int index) {
  (void)s;
  (void)vals;
  (void)index;
  bool changed = false;
  using T = ChartType;
  const bool combo = m.type == T::Line || m.type == T::LineMarkers || m.type == T::ColumnClustered || m.type == T::ColumnStacked || m.type == T::Area || m.type == T::Scatter;
  if (combo) {
    static const int kTypes[] = {-1, 0, 1, 2, 3, 6, 5};
    const char* const* names = ChartTypeNames();
    const char* items[7] = {TR("Same as the chart")};
    for (int k = 1; k < 7; ++k) items[k] = names[kTypes[k]];
    int cur = 0;
    for (int k = 0; k < 7; ++k) {
      if (kTypes[k] == se.type) cur = k;
    }
    ImGui::SetNextItemWidth(S(180));
    if (ImGui::Combo(TR("Series Chart Type"), &cur, items, 7)) {
      se.type = kTypes[cur];
      changed = true;
    }
  }
  return changed;
}

// Format Trendline: the type with its order or period, the forecast, the equation and R-squared
// Format Trendline: the type (order, period), its name, the forecast, Set Intercept and what is shown on the chart; idx:
// which of the series' trendlines (0 the first)
bool EditTrendline(model::ChartSeries& se, int idx) {
  std::vector<model::ChartTrendline> all = TrendlinesOf(se);
  if (all.empty()) return false;
  idx = std::clamp(idx, 0, static_cast<int>(all.size()) - 1);
  model::ChartTrendline t = all[static_cast<size_t>(idx)];
  bool changed = false;
  ImGui::SeparatorText(TR("Trendline Options"));
  const char* trends[] = {TR("None"), TR("Exponential"), TR("Linear"), TR("Logarithmic"), TR("Polynomial"), TR("Power"), TR("Moving Average")};
  // Excel's order: Exponential, Linear, Logarithmic, Polynomial, Power, Moving Average
  static const model::TrendKind kOrder[] = {model::TrendKind::Exponential, model::TrendKind::Linear, model::TrendKind::Logarithmic, model::TrendKind::Polynomial,
                                            model::TrendKind::Power, model::TrendKind::MovingAverage};
  for (int k = 0; k < 6; ++k) {
    const model::TrendKind kind = kOrder[k];
    if (ImGui::RadioButton(trends[k + 1], t.kind == kind)) t.kind = kind, changed = true;
    if (kind == model::TrendKind::Polynomial && t.kind == kind) {
      ImGui::SameLine();
      changed |= SpinInt(TR("Order"), t.order, 2, 6);
    }
    if (kind == model::TrendKind::MovingAverage && t.kind == kind) {
      ImGui::SameLine();
      changed |= SpinInt(TR("Period"), t.period, 2, 255);
    }
  }
  ImGui::SeparatorText(TR("Trendline Name"));
  if (ImGui::RadioButton(TR("Automatic##trendname"), t.name.empty()) && !t.name.empty()) t.name.clear(), changed = true;
  bool custom = !t.name.empty();
  if (ImGui::RadioButton(TR("Custom##trendname"), custom) && !custom) {
    t.name = TrendLegendName(t, se.name.empty() || se.name[0] == '=' ? std::string(TR("Series")) : se.name);
    changed = true;
  }
  if (!t.name.empty()) changed |= EditText("##trend_name", t.name);
  if (t.kind != model::TrendKind::MovingAverage) {
    ImGui::SeparatorText(TR("Forecast"));
    changed |= EditNum(TR("Forward (periods)"), t.forward, 1, "%.1f");
    changed |= EditNum(TR("Backward (periods)"), t.backward, 1, "%.1f");
    t.forward = std::max(0.0, t.forward);
    t.backward = std::max(0.0, t.backward);
    // Set Intercept: linear, polynomial and exponential trendlines (Excel greys it out for the others)
    const bool icept_ok = t.kind == model::TrendKind::Linear || t.kind == model::TrendKind::Polynomial || t.kind == model::TrendKind::Exponential;
    ImGui::BeginDisabled(!icept_ok);
    changed |= ImGui::Checkbox(TR("Set Intercept"), &t.intercept_on);
    if (t.intercept_on) {
      ImGui::SameLine();
      changed |= EditNum("##icept", t.intercept, 1, "%.6g");
    }
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox(TR("Display Equation on chart"), &t.eq);
    changed |= ImGui::Checkbox(TR("Display R-squared value on chart"), &t.r2);
  }
  if (!changed) return false;
  if (idx == 0) {
    se.trend = t.kind;
    se.trend_order = t.order;
    se.trend_period = t.period;
    se.trend_forward = t.forward;
    se.trend_backward = t.backward;
    se.trend_eq = t.eq;
    se.trend_r2 = t.r2;
    se.trend_name = t.name;
    se.trend_icept_on = t.intercept_on;
    se.trend_icept = t.intercept;
  } else if (static_cast<size_t>(idx - 1) < se.more_trends.size()) {
    se.more_trends[static_cast<size_t>(idx - 1)] = t;
  }
  return true;
}

bool EditErrorBarsOf(AppState& s, model::ChartSeries& se, int index, const char* key);

// Format Error Bars: the direction, the end style and the amount (custom amounts picked on the sheet); x: a scatter
// chart's horizontal (X) error bars
bool EditErrorBars(AppState& s, model::ChartSeries& se, int index, bool x = false) {
  if (!x) return EditErrorBarsOf(s, se, index, "chart.err");
  model::ChartSeries t;  // the X error bars' fields through the same pane
  t.err = se.xerr;
  t.err_amount = se.xerr_amount;
  t.err_plus = se.xerr_plus;
  t.err_minus = se.xerr_minus;
  t.err_include = se.xerr_include;
  t.err_cap = se.err_cap;
  if (!EditErrorBarsOf(s, t, index, "chart.xerr")) return false;
  se.xerr = t.err;
  se.xerr_amount = t.err_amount;
  se.xerr_plus = t.err_plus;
  se.xerr_minus = t.err_minus;
  se.xerr_include = t.err_include;
  se.err_cap = t.err_cap;
  return true;
}

bool EditErrorBarsOf(AppState& s, model::ChartSeries& se, int index, const char* key) {
  bool changed = false;
  ImGui::SeparatorText(TR("Direction"));
  const char* dirs[] = {TR("Both"), TR("Plus"), TR("Minus")};
  const int dir = std::clamp(se.err_include, 1, 3) - 1;
  for (int k = 0; k < 3; ++k) {
    if (ImGui::RadioButton(dirs[k], dir == k)) {
      se.err_include = k + 1;
      changed = true;
    }
  }
  ImGui::SeparatorText(TR("End Style"));
  if (ImGui::RadioButton(TR("No Cap"), !se.err_cap)) se.err_cap = false, changed = true;
  if (ImGui::RadioButton(TR("Cap"), se.err_cap)) se.err_cap = true, changed = true;
  ImGui::SeparatorText(TR("Error Amount"));
  const char* errs[] = {TR("None"), TR("Fixed value"), TR("Percentage"), TR("Standard deviation(s)"), TR("Standard error"), TR("Custom")};
  const int e = static_cast<int>(se.err);
  for (int k = 1; k < 6; ++k) {
    if (ImGui::RadioButton(errs[k], e == k)) {
      se.err = static_cast<model::ErrorBarKind>(k);
      changed = true;
    }
    if (k == e) {
      if (k == 1) ImGui::SameLine(), changed |= EditNum("##errfix", se.err_amount, 1, "%.2f");
      if (k == 2) ImGui::SameLine(), changed |= EditNum("##errpct", se.err_amount, 1, "%.1f%%");
      if (k == 3) ImGui::SameLine(), changed |= EditNum("##errsd", se.err_amount, 0.5, "%.1f");
    }
  }
  if (se.err == model::ErrorBarKind::Custom) {
    // ranges picked on the sheet, like Excel's Custom Error Bars dialog
    // a range shows as =Sheet1!$E$2:$E$9; an array {1,2,3} or one number as typed
    auto shown = [](const std::string& v) { return v.empty() || v[0] == '{' || std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '.' ? v : "=" + v; };
    auto stored = [](std::string v) {
      while (!v.empty() && (v[0] == '=' || v[0] == ' ')) v.erase(0, 1);
      return v;
    };
    ImGui::TextUnformatted(TR("Positive Error Value"));
    std::string plus = shown(se.err_plus);
    if (RefEdit(s, (std::string(key) + "plus" + std::to_string(index)).c_str(), &plus, ImGui::GetContentRegionAvail().x)) {
      se.err_plus = stored(plus);
      changed = true;
    }
    ImGui::TextUnformatted(TR("Negative Error Value"));
    std::string minus = shown(se.err_minus);
    if (RefEdit(s, (std::string(key) + "minus" + std::to_string(index)).c_str(), &minus, ImGui::GetContentRegionAvail().x)) {
      se.err_minus = stored(minus);
      changed = true;
    }
    ImGui::TextDisabled("%s", TR("Select the cells with the amounts, or type {1,2,3}"));
  }
  return changed;
}

uint32_t SwapRB(uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

// a color from a palette like Excel's (Automatic, the theme's accents, standard colors); true when one was picked
bool PickColor(const char* label, uint32_t* argb, bool allow_auto = true) {
  bool changed = false;
  ImGui::PushID(label);
  const ImVec4 cv = *argb ? ImGui::ColorConvertU32ToFloat4(ArgbCol(*argb)) : ImVec4(1, 1, 1, 0);
  if (ImGui::ColorButton("##swatch", cv, ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(S(36), S(18)))) ImGui::OpenPopup("##palette");
  ImGui::SameLine();
  ImGui::TextUnformatted(label);
  if (*argb == 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", TR("Automatic"));
  }
  if (ImGui::BeginPopup("##palette")) {
    // Excel's palette: Automatic, the theme's ten colors with their lighter and darker shades, the standard colors,
    // the colors used last, and More Colors (any color)
    static std::vector<uint32_t> recent;
    auto pick = [&](uint32_t c) {
      *argb = c;
      changed = true;
      if (c) {
        recent.erase(std::remove(recent.begin(), recent.end(), c), recent.end());
        recent.insert(recent.begin(), c);
        if (recent.size() > 10) recent.resize(10);
      }
      ImGui::CloseCurrentPopup();
    };
    if (allow_auto && ImGui::Selectable(TR("Automatic"), *argb == 0)) pick(0);
    ImGui::TextUnformatted(TR("Theme Colors"));
    static const int kCols[10] = {model::kLight1, model::kDark1, model::kLight2, model::kDark2, model::kAccent1, model::kAccent2, model::kAccent3, model::kAccent4, model::kAccent5, model::kAccent6};
    static const double kTint[6] = {0.0, 0.8, 0.6, 0.4, -0.25, -0.5};
    const model::Theme& th = model::CurrentTheme();
    auto swatch = [&](uint32_t c, int id) {
      ImGui::PushID(id);
      if (ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(ArgbCol(c)), 0, ImVec2(S(16), S(16)))) pick(c);
      ImGui::PopID();
    };
    for (int row = 0; row < 6; ++row) {
      for (int i = 0; i < 10; ++i) {
        if (i) ImGui::SameLine(0, S(3));
        swatch(0xFF000000u | (model::ApplyTint(th.colors[kCols[i]], kTint[row]) & 0xFFFFFF), row * 10 + i);
      }
      if (row == 0) ImGui::Dummy(ImVec2(1, S(2)));
    }
    ImGui::TextUnformatted(TR("Standard Colors"));
    static const uint32_t kStd[10] = {0xFFC00000u, 0xFFFF0000u, 0xFFFFC000u, 0xFFFFFF00u, 0xFF92D050u, 0xFF00B050u, 0xFF00B0F0u, 0xFF0070C0u, 0xFF002060u, 0xFF7030A0u};
    for (int i = 0; i < 10; ++i) {
      if (i) ImGui::SameLine(0, S(3));
      swatch(kStd[i], 100 + i);
    }
    if (!recent.empty()) {
      ImGui::TextUnformatted(TR("Recent Colors"));
      for (size_t i = 0; i < recent.size(); ++i) {
        if (i) ImGui::SameLine(0, S(3));
        swatch(recent[i], 200 + static_cast<int>(i));
      }
    }
    ImGui::Separator();
    if (ImGui::TreeNode(TR("More Colors..."))) {
      // any color: by its red, green and blue values (Excel's Custom tab) or on the color field
      static ImVec4 custom(1, 0, 0, 1);
      if (ImGui::IsWindowAppearing() && *argb) custom = ImGui::ColorConvertU32ToFloat4(ArgbCol(*argb));
      ImGui::SetNextItemWidth(S(220));
      ImGui::ColorPicker4("##picker", &custom.x, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB);
      if (ImGui::Button(TR("OK"))) {
        const ImU32 c = ImGui::ColorConvertFloat4ToU32(custom);  // ABGR
        pick(0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF));
      }
      ImGui::TreePop();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// Excel's font size list
bool PickFontSize(const char* label, float* size, float default_pt) {
  static const float kSizes[] = {6, 7, 8, 9, 10, 10.5f, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 48, 54, 60, 72};
  const float cur = *size > 0 ? *size : default_pt;
  char now[16];
  std::snprintf(now, sizeof now, "%g", cur);
  bool changed = false;
  ImGui::SetNextItemWidth(S(80));
  if (ImGui::BeginCombo(label, now)) {
    for (float f : kSizes) {
      char b[16];
      std::snprintf(b, sizeof b, "%g", f);
      if (ImGui::Selectable(b, f == cur)) {
        *size = f;
        changed = true;
      }
      if (f == cur) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// the font of a chart text (Text Options / Font): name, size, bold, italic, underline, color
bool EditChartFont(model::ChartFont& f, float default_pt) {
  bool changed = false;
  {
    const std::string now = f.name.empty() ? std::string(TR("(Automatic)")) : f.name;
    ImGui::SetNextItemWidth(S(180));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ui::NoteUiItem(TR("Font"), ImRect(at, at + ImVec2(S(180), ImGui::GetFrameHeight())));  // combos report no label (AGENTS.md)
    if (ImGui::BeginCombo(TR("Font"), now.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::Selectable(TR("(Automatic)"), f.name.empty())) f.name.clear(), changed = true;
      for (const std::string& fam : ui::SystemFontFamilies()) {
        if (ImGui::Selectable(fam.c_str(), fam == f.name)) f.name = fam, changed = true;
        if (fam == f.name && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
      }
      ImGui::EndCombo();
    }
  }
  changed |= PickFontSize(TR("Size"), &f.size, default_pt);
  changed |= ImGui::Checkbox(TR("Bold"), &f.bold);
  ImGui::SameLine();
  changed |= ImGui::Checkbox(TR("Italic"), &f.italic);
  ImGui::SameLine();
  changed |= ImGui::Checkbox(TR("Underline"), &f.underline);
  changed |= PickColor(TR("Font Color"), &f.color);
  return changed;
}

// Format Axis > Number: a category and its decimal places, like Excel (Custom shows the format code)
bool EditNumberFormat(std::string& code) {
  struct Cat {
    const char* base;  // with 2 decimals; "" = General, nullptr = Custom
    bool decimals;
  };
  static const Cat kCats[] = {{"", false}, {"#,##0.00", true}, {"$#,##0.00", true}, {"0.00%", true}, {"0.00E+00", true}, {"yyyy/m/d", false}, {"h:mm", false}, {nullptr, false}};
  constexpr int kN = static_cast<int>(sizeof(kCats) / sizeof(kCats[0]));
  const char* names[kN] = {TR("General"), TR("Number"), TR("Currency"), TR("Percentage"), TR("Scientific"), TR("Date"), TR("Time"), TR("Custom")};
  // which category the code is, and its decimals
  int cat = kN - 1, dec = 2;
  auto with_dec = [](const char* base, int d) {
    std::string s = base;
    const size_t p = s.find(".00");
    if (p == std::string::npos) return s;
    s.erase(p, 3);
    if (d > 0) s.insert(p, "." + std::string(static_cast<size_t>(d), '0'));
    return s;
  };
  for (int k = 0; k < kN - 1; ++k) {
    if (!kCats[k].decimals) {
      if (code == kCats[k].base) cat = k;
      continue;
    }
    for (int d = 0; d <= 10; ++d) {
      if (code == with_dec(kCats[k].base, d)) cat = k, dec = d;
    }
  }
  bool changed = false;
  ImGui::SetNextItemWidth(S(160));
  if (ImGui::Combo(TR("Category"), &cat, names, kN)) {
    if (kCats[cat].base) code = with_dec(kCats[cat].base, kCats[cat].decimals ? 2 : 0);
    changed = true;
  }
  if (kCats[cat].base && kCats[cat].decimals) {
    if (SpinInt(TR("Decimal places"), dec, 0, 10)) {
      code = with_dec(kCats[cat].base, dec);
      changed = true;
    }
  }
  if (!kCats[cat].base) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "%s", code.c_str());
    ImGui::SetNextItemWidth(S(160));
    ImGui::InputText(TR("Format Code"), buf, sizeof buf);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      code = buf;
      changed = true;
    }
    ImGui::TextDisabled("%s", TR("For example: 0.00   0%   yyyy-mm-dd   $0.00"));
  }
  if (!code.empty()) {
    const std::string sample = engine::DisplayTextOf(core::Value::Number(1234.5678), code);
    ImGui::TextDisabled("%s %s", TR("Sample:"), sample.c_str());
  }
  return changed;
}

// Format Axis > Axis Options, grouped as Excel groups them: Bounds (each Auto or fixed, with Reset), Units, where the
// other axis crosses, Display units, Logarithmic scale, reverse order; a category axis: reverse order, crossing,
// position and label interval; then Tick Marks, Labels and Number. lo / hi / step: what Auto gives now
bool EditAxisFormat(model::ChartAxisFormat& a, bool value_axis, double lo, double hi, double step) {
  bool changed = ImGui::Checkbox(TR("Show axis"), &a.visible);
  changed |= ImGui::Checkbox(TR("Major Gridlines"), &a.gridlines);
  changed |= ImGui::Checkbox(TR("Minor Gridlines"), &a.minor_gridlines);
  changed |= ImGui::Checkbox(TR("Axis Title"), &a.has_title);
  ImGui::SeparatorText(TR("Axis Options"));
  auto num_auto = [&](const char* label, bool* autob, double* v, double now, bool zero_is_auto) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(S(90));
    const bool is_auto = autob ? *autob : (zero_is_auto && *v == 0);
    double x = is_auto ? now : *v;
    ImGui::SetNextItemWidth(S(110));
    ImGui::InputDouble("##v", &x, 0, 0, "%.6g");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      *v = x;
      if (autob) *autob = false;
      changed = true;
    }
    ImGui::SameLine();
    if (is_auto) {
      ImGui::TextDisabled("%s", TR("Auto"));
    } else if (ImGui::SmallButton(TR("Reset"))) {  // back to automatic
      if (autob) *autob = true;
      else *v = 0;
      changed = true;
    }
    ImGui::PopID();
  };
  if (value_axis) {
    ImGui::TextUnformatted(TR("Bounds"));
    num_auto(TR("Minimum"), &a.min_auto, &a.min, lo, false);
    num_auto(TR("Maximum"), &a.max_auto, &a.max, hi, false);
    ImGui::TextUnformatted(TR("Units"));
    num_auto(TR("Major"), nullptr, &a.major, step, true);
    num_auto(TR("Minor"), nullptr, &a.minor, step / 5, true);
    a.major = std::max(0.0, a.major);
    a.minor = std::max(0.0, a.minor);
    ImGui::TextUnformatted(TR("Horizontal axis crosses:"));
    if (ImGui::RadioButton(TR("Automatic##crosses"), a.crosses == 0)) a.crosses = 0, changed = true;
    if (ImGui::RadioButton(TR("Axis value"), a.crosses == 1)) a.crosses = 1, changed = true;
    if (a.crosses == 1) {
      ImGui::SameLine();
      changed |= EditNum("##crosses_at", a.crosses_at, step > 0 ? step : 1, "%.6g");
    }
    if (ImGui::RadioButton(TR("Maximum axis value"), a.crosses == 2)) a.crosses = 2, changed = true;
    // Display units: the labels divided (Excel's list)
    static const double kUnits[] = {1, 100, 1000, 10000, 100000, 1e6, 1e7, 1e8, 1e9, 1e12};
    const char* unit_names[] = {TR("None"), TR("Hundreds"), TR("Thousands"), "10000", "100000", TR("Millions"), "10000000", "100000000", TR("Billions"), TR("Trillions")};
    int ui = 0;
    for (int k = 0; k < 10; ++k) {
      if (a.display_unit == kUnits[k]) ui = k;
    }
    ImGui::SetNextItemWidth(S(140));
    if (ImGui::Combo(TR("Display units"), &ui, unit_names, 10)) a.display_unit = kUnits[ui], changed = true;
    changed |= ImGui::Checkbox(TR("Logarithmic scale"), &a.log);
    if (a.log) {
      ImGui::SameLine();
      if (EditNum(TR("Base"), a.log_base, 1, "%.4g")) a.log_base = std::clamp(a.log_base, 2.0, 1000.0), changed = true;
    }
    changed |= ImGui::Checkbox(TR("Values in reverse order"), &a.reverse);
  } else {
    ImGui::TextUnformatted(TR("Vertical axis crosses:"));
    if (ImGui::RadioButton(TR("Automatic##vcrosses"), a.crosses != 2)) a.crosses = 0, changed = true;
    if (ImGui::RadioButton(TR("At maximum category"), a.crosses == 2)) a.crosses = 2, changed = true;
    ImGui::TextUnformatted(TR("Axis position:"));
    if (ImGui::RadioButton(TR("On tick marks"), !a.between)) a.between = false, changed = true;
    if (ImGui::RadioButton(TR("Between tick marks"), a.between)) a.between = true, changed = true;
    changed |= ImGui::Checkbox(TR("Categories in reverse order"), &a.reverse);
    ImGui::TextUnformatted(TR("Interval between labels:"));
    if (ImGui::RadioButton(TR("Automatic##interval"), a.label_interval == 0)) a.label_interval = 0, changed = true;
    if (ImGui::RadioButton(TR("Specify interval unit"), a.label_interval > 0) && a.label_interval == 0) a.label_interval = 1, changed = true;
    if (a.label_interval > 0) {
      ImGui::SameLine();
      changed |= SpinInt("##interval", a.label_interval, 1, 10000);
    }
  }
  ImGui::SeparatorText(TR("Tick Marks"));
  const char* ticks[] = {TR("None"), TR("Inside"), TR("Outside"), TR("Cross")};
  int mt = a.major_tick < 0 ? 2 : std::clamp(a.major_tick, 0, 3);
  ImGui::SetNextItemWidth(S(140));
  if (ImGui::Combo(TR("Major type"), &mt, ticks, 4)) a.major_tick = mt, changed = true;
  ImGui::SetNextItemWidth(S(140));
  int mn = std::clamp(a.minor_tick, 0, 3);
  if (ImGui::Combo(TR("Minor type"), &mn, ticks, 4)) a.minor_tick = mn, changed = true;
  ImGui::SeparatorText(TR("Labels"));
  const char* lpos[] = {TR("Next to Axis"), TR("High"), TR("Low"), TR("None")};
  int lp = std::clamp(a.label_pos, 0, 3);
  ImGui::SetNextItemWidth(S(140));
  if (ImGui::Combo(TR("Label Position"), &lp, lpos, 4)) a.label_pos = lp, changed = true;
  if (!value_axis) changed |= SpinInt(TR("Distance from axis"), a.label_offset, 0, 1000);
  if (!value_axis) changed |= ImGui::Checkbox(TR("Multi-level Category Labels"), &a.multi_level);
  ImGui::SeparatorText(TR("Number"));
  // Linked to source: the labels take the cells' number format
  bool linked = a.number_format.empty();
  if (ImGui::Checkbox(TR("Linked to source##axis"), &linked)) {
    a.number_format = linked ? std::string() : std::string("General");
    changed = true;
  }
  if (!linked) {
    if (EditNumberFormat(a.number_format)) {
      if (a.number_format.empty()) a.number_format = "General";
      changed = true;
    }
  }
  // Fill & Line: the axis line (Automatic, No line, Solid line with its color and width)
  ImGui::SeparatorText(TR("Line##axis"));
  const int lk = a.no_line ? 1 : a.line_color ? 2 : 0;
  if (ImGui::RadioButton(TR("No line##axis"), lk == 1)) a.no_line = true, changed = true;
  ImGui::SameLine();
  if (ImGui::RadioButton(TR("Solid line##axis"), lk == 2) && lk != 2) a.no_line = false, a.line_color = 0xFFBFBFBFu, changed = true;
  ImGui::SameLine();
  if (ImGui::RadioButton(TR("Automatic##axisline"), lk == 0)) a.no_line = false, a.line_color = 0, changed = true;
  if (lk == 2) {
    changed |= PickColor(TR("Color##axisline"), &a.line_color, false);
    float w = a.line_weight > 0 ? a.line_weight : 0.75f;
    if (SpinFloat(TR("Width (pt)##axis"), w, 0.25f, 0.25f, 20.0f, "%.2f")) a.line_weight = w, changed = true;
  }
  return changed;
}

// Excel's Format Control for a form control: its Control tab in Excel's words, then Size and positioning
bool DrawFormatControl(AppState& s, Control& m) {
  using K = ControlKind;
  bool changed = false;
  const std::string id = std::to_string(m.id);
  auto num = [&](const char* label, int& v, int lo, int hi) {
    ImGui::SetNextItemWidth(S(140));
    const int before = v;
    ImGui::InputInt(label, &v, 1, 10);
    v = std::clamp(v, lo, hi);
    return v != before && (!ImGui::GetIO().WantTextInput || ImGui::IsItemDeactivatedAfterEdit());
  };
  ImGui::SeparatorText(TR("Control"));
  if (m.kind == K::CheckBox || m.kind == K::OptionButton) {
    ImGui::TextUnformatted(TR("Value"));
    const bool on = model::ControlIsOn(m);
    const bool mixed = m.value == "Mixed" || m.value == "#N/A";
    if (ImGui::RadioButton(TR("Unchecked"), !on && !mixed)) m.value = "False", changed = true;
    if (ImGui::RadioButton(TR("Checked"), on)) m.value = "True", changed = true;
    if (m.kind == K::CheckBox && ImGui::RadioButton(TR("Mixed"), mixed)) m.value = "Mixed", changed = true;
    changed |= EditRefProp(s, TR("Cell link:"), "fc.link." + id, m.linked_cell, RangeBox_Cell);
  } else if (m.kind == K::ComboBox || m.kind == K::ListBox) {
    changed |= EditRefProp(s, TR("Input range:"), "fc.fill." + id, m.list_fill_range);
    changed |= EditRefProp(s, TR("Cell link:"), "fc.link." + id, m.linked_cell, RangeBox_Cell);
    if (m.kind == K::ComboBox) {
      changed |= num(TR("Drop down lines"), m.list_rows, 1, 100);
    } else {
      ImGui::TextUnformatted(TR("Selection type"));
      if (ImGui::RadioButton(TR("Single"), m.multi_select == 0)) m.multi_select = 0, changed = true;
      if (ImGui::RadioButton(TR("Multi"), m.multi_select == 1)) m.multi_select = 1, changed = true;
      if (ImGui::RadioButton(TR("Extend"), m.multi_select == 2)) m.multi_select = 2, changed = true;
    }
  } else if (m.kind == K::SpinButton || m.kind == K::ScrollBar) {
    int v = static_cast<int>(std::lround(std::strtod(m.value.c_str(), nullptr)));
    if (num(TR("Current value"), v, m.min, m.max)) m.value = std::to_string(v), changed = true;
    changed |= num(TR("Minimum value"), m.min, -30000, 30000);
    changed |= num(TR("Maximum value"), m.max, -30000, 30000);
    changed |= num(TR("Incremental change"), m.small_change, 1, 30000);
    if (m.kind == K::ScrollBar) changed |= num(TR("Page change"), m.large_change, 1, 30000);
    changed |= EditRefProp(s, TR("Cell link:"), "fc.link." + id, m.linked_cell, RangeBox_Cell);
  } else {
    changed |= EditText(TR("Text"), m.caption);
  }
  if (m.kind == K::Button || !m.on_action.empty()) {
    ImGui::TextDisabled("%s %s", TR("Macro:"), m.on_action.empty() ? TR("(none)") : m.on_action.c_str());
  }
  // 3-D shading, on the controls Excel offers it for
  if (m.form && (m.kind == K::CheckBox || m.kind == K::OptionButton || m.kind == K::ListBox || m.kind == K::ComboBox || m.kind == K::GroupBox ||
                 m.kind == K::ScrollBar || m.kind == K::SpinButton)) {
    ImGui::Separator();
    changed |= ImGui::Checkbox(TR("3-D shading"), &m.shading_3d);
  }
  return changed;
}


// an Excel RGB value (0xBBGGRR) <-> the ARGB the palettes use
uint32_t CtlBgrToArgb(uint32_t c) { return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
uint32_t CtlArgbToBgr(uint32_t c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

// a value of an enumerated property as Excel's Properties window shows it ("1 - xlMoveAndSize")
struct EnumItem {
  int value;
  const char* name;
};
std::string EnumText(int v, const EnumItem* items, int n) {
  for (int i = 0; i < n; ++i) {
    if (items[i].value == v) return std::to_string(v) + " - " + items[i].name;
  }
  return std::to_string(v);
}

// one row of the Properties window: the name in the left column, its editor in the right one
struct PropRow {
  const char* name;
  const char* category;  // Appearance, Behavior, Data, Font, Misc, Picture, Position, Scrolling
  std::function<bool()> edit;
};

// the editors of the grid's right column
bool PropBool(const char* id, bool& v) {
  int i = v ? 0 : 1;
  const char* kTF[] = {"True", "False"};
  ImGui::SetNextItemWidth(-1);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ui::NoteUiItem((std::string(id + 2) + "=" + kTF[i]).c_str(), ImRect(at, at + ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())));
  if (ImGui::Combo(id, &i, kTF, 2)) {
    v = i == 0;
    return true;
  }
  return false;
}
bool PropEnum(const char* id, int& v, const EnumItem* items, int n) {
  bool changed = false;
  ImGui::SetNextItemWidth(-1);
  // the recorder sees the value as a person reads it ("1 - xlMoveAndSize")
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ui::NoteUiItem(EnumText(v, items, n).c_str(), ImRect(at, at + ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight())));
  if (ImGui::BeginCombo(id, EnumText(v, items, n).c_str())) {
    for (int i = 0; i < n; ++i) {
      const std::string t = std::to_string(items[i].value) + " - " + items[i].name;
      if (ImGui::Selectable(t.c_str(), items[i].value == v)) v = items[i].value, changed = true;
    }
    ImGui::EndCombo();
  }
  return changed;
}
bool PropText(const char* id, std::string& v, bool multiline = false) {
  char buf[4096];
  std::snprintf(buf, sizeof(buf), "%s", v.c_str());
  ImGui::SetNextItemWidth(-1);
  if (multiline) ImGui::InputTextMultiline(id, buf, sizeof(buf), ImVec2(-1, S(60)));
  else ImGui::InputText(id, buf, sizeof(buf));
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    v = buf;
    return true;
  }
  return false;
}
bool PropNum(const char* id, double& v) {
  ImGui::SetNextItemWidth(-1);
  const double before = v;
  ImGui::InputDouble(id, &v, 0, 0, "%.6g");
  return ImGui::IsItemDeactivatedAfterEdit() && v != before;
}
bool PropInt(const char* id, int& v) {
  ImGui::SetNextItemWidth(-1);
  const int before = v;
  ImGui::InputInt(id, &v, 0, 0);
  return ImGui::IsItemDeactivatedAfterEdit() && v != before;
}
// BackColor / ForeColor: "&H00BBGGRR&" (a system color while it is the default) and Excel's palette
bool PropColor(const char* id, uint32_t& bgr, bool back) {
  bool changed = false;
  char text[24];
  if (bgr == 0) std::snprintf(text, sizeof(text), "%s", back ? "&H8000000F&" : "&H80000012&");
  else std::snprintf(text, sizeof(text), "&H00%06X&", bgr & 0xFFFFFF);
  ImGui::PushID(id);
  const ImVec4 cv = ImGui::ColorConvertU32ToFloat4(bgr ? ExcelColor(bgr) : back ? IM_COL32(0xF0, 0xF0, 0xF0, 255) : IM_COL32(0, 0, 0, 255));
  if (ImGui::ColorButton("##sw", cv, 0, ImVec2(S(18), ImGui::GetFrameHeight()))) ImGui::OpenPopup("##pal");
  ImGui::SameLine(0, S(4));
  ImGui::TextUnformatted(text);
  if (ImGui::BeginPopup("##pal")) {
    uint32_t ref = 0;
    if (ImGui::Selectable(back ? TR("Button Face (system)") : TR("Button Text (system)"), bgr == 0)) bgr = 0, changed = true, ImGui::CloseCurrentPopup();
    if (ui::ribbon::PaletteSwatches(&ref, nullptr)) {
      uint32_t c = CtlArgbToBgr(model::ResolveColor(ref) | 0xFF000000u);
      if (c == 0) c = 0x010101;  // black (0 is the system default)
      bgr = c;
      changed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}
// Font: its name, and "..." for Excel's Font dialog (font, style, size)
bool PropFont(const char* id, Control& m) {
  bool changed = false;
  ImGui::PushID(id);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(m.font_name.empty() ? "Calibri" : m.font_name.c_str());
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - S(26));
  if (ImGui::SmallButton("...")) ImGui::OpenPopup(TR("Font###yc_ctl_font"));
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (BeginModal(TR("Font###yc_ctl_font"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    static std::string name;
    static int style = 0;
    static float size = 11;
    if (ImGui::IsWindowAppearing()) {
      name = m.font_name.empty() ? "Calibri" : m.font_name;
      style = (m.bold ? 2 : 0) + (m.italic ? 1 : 0);
      size = m.font_size > 0 ? m.font_size : 11;
    }
    ImGui::TextUnformatted(TR("Font:"));
    ImGui::SetNextItemWidth(S(220));
    if (ImGui::BeginCombo("##fname", name.c_str())) {
      for (const std::string& f : ui::SystemFontFamilies()) {
        if (ImGui::Selectable(f.c_str(), f == name)) name = f;
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted(TR("Font style:"));
    const char* kStyles[] = {TR("Regular"), TR("Italic"), TR("Bold"), TR("Bold Italic")};
    ImGui::SetNextItemWidth(S(220));
    ImGui::Combo("##fstyle", &style, kStyles, 4);
    ImGui::TextUnformatted(TR("Size:"));
    ImGui::SetNextItemWidth(S(220));
    ImGui::InputFloat("##fsize", &size, 1, 2, "%.0f");
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      m.font_name = name == "Calibri" ? std::string() : name;
      m.bold = style >= 2;
      m.italic = style == 1 || style == 3;
      m.font_size = std::clamp(size, 1.0f, 409.0f);
      changed = true;
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// Excel's Properties window of an ActiveX control: (Name) first, the properties in alphabetical order or by
// category, each value as Excel writes it (True / False, "1 - xlMoveAndSize", "&H8000000F&")
bool DrawActiveXGrid(AppState& s, Control& m) {
  using K = ControlKind;
  static int tab = 0;  // 0 Alphabetic, 1 Categorized
  bool changed = false;
  static const EnumItem kPlacement[] = {{1, "xlMoveAndSize"}, {2, "xlMove"}, {3, "xlFreeFloating"}, {100, "ycFloatWindow"}};
  static const EnumItem kStyle[] = {{0, "fmStyleDropDownCombo"}, {2, "fmStyleDropDownList"}};
  static const EnumItem kMulti[] = {{0, "fmMultiSelectSingle"}, {1, "fmMultiSelectMulti"}, {2, "fmMultiSelectExtended"}};
  static const EnumItem kOrient[] = {{-1, "fmOrientationAuto"}, {0, "fmOrientationVertical"}, {1, "fmOrientationHorizontal"}};
  static const EnumItem kPicMode[] = {{0, "fmPictureSizeModeClip"}, {1, "fmPictureSizeModeStretch"}, {3, "fmPictureSizeModeZoom"}};
  const bool caption = m.kind == K::Button || m.kind == K::CheckBox || m.kind == K::OptionButton || m.kind == K::ToggleButton || m.kind == K::Label;
  const bool valued = m.kind != K::Button && m.kind != K::Label && m.kind != K::Image;
  const bool linked = valued;
  std::vector<PropRow> rows;
  rows.push_back({"(Name)", "Misc", [&] { return PropText("##name", m.name); }});
  if (m.kind != K::Image) {
    rows.push_back({"BackColor", "Appearance", [&] { return PropColor("BackColor", m.back_color, true); }});
    rows.push_back({"ForeColor", "Appearance", [&] { return PropColor("ForeColor", m.fore_color, false); }});
  }
  if (caption) rows.push_back({"Caption", "Appearance", [&] { return PropText("##caption", m.caption); }});
  rows.push_back({"Enabled", "Behavior", [&] { return PropBool("##enabled", m.enabled); }});
  if (m.kind != K::Image && m.kind != K::SpinButton && m.kind != K::ScrollBar) rows.push_back({"Font", "Font", [&] { return PropFont("Font", m); }});
  rows.push_back({"Height", "Position", [&] { return PropNum("##height", m.height); }});
  rows.push_back({"Left", "Position", [&] { return PropNum("##left", m.left); }});
  if (linked) rows.push_back({"LinkedCell", "Data", [&] { return EditRefPropCell(s, "ctl.link." + std::to_string(m.id), m.linked_cell); }});
  if (m.kind == K::ComboBox || m.kind == K::ListBox) {
    rows.push_back({"ListFillRange", "Data", [&] { return EditRefPropCell(s, "ctl.fill." + std::to_string(m.id), m.list_fill_range, false); }});
  }
  if (m.kind == K::ComboBox) rows.push_back({"ListRows", "Behavior", [&] { return PropInt("##listrows", m.list_rows); }});
  rows.push_back({"Locked", "Behavior", [&] { return PropBool("##locked", m.locked); }});
  if (m.kind == K::SpinButton || m.kind == K::ScrollBar) {
    rows.push_back({"Max", "Scrolling", [&] { return PropInt("##max", m.max); }});
    rows.push_back({"Min", "Scrolling", [&] { return PropInt("##min", m.min); }});
    rows.push_back({"SmallChange", "Scrolling", [&] { return PropInt("##small", m.small_change); }});
    if (m.kind == K::ScrollBar) rows.push_back({"LargeChange", "Scrolling", [&] { return PropInt("##large", m.large_change); }});
    rows.push_back({"Orientation", "Appearance", [&] { return PropEnum("##orient", m.orientation, kOrient, 3); }});
  }
  if (m.kind == K::ListBox) rows.push_back({"MultiSelect", "Behavior", [&] { return PropEnum("##multi", m.multi_select, kMulti, 3); }});
  if (m.kind == K::TextBox) {
    rows.push_back({"MultiLine", "Behavior", [&] { return PropBool("##multiline", m.multi_line); }});
    rows.push_back({"PasswordChar", "Behavior", [&] { return PropText("##pwchar", m.password_char); }});
  }
  if (m.kind == K::OptionButton) rows.push_back({"GroupName", "Misc", [&] { return PropText("##group", m.group_name); }});
  if (m.kind == K::Image) {
    rows.push_back({"Picture", "Picture", [&] {
                      ImGui::AlignTextToFramePadding();
                      ImGui::TextUnformatted(m.picture.empty() ? "(None)" : "(Bitmap)");
                      ImGui::SameLine();
                      if (ImGui::SmallButton("...##pic")) {
                        const std::wstring path = PickImageFile(s.hwnd);
                        if (!path.empty()) {
                          std::ifstream f(path, std::ios::binary);
                          m.picture.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                          return true;
                        }
                      }
                      return false;
                    }});
    rows.push_back({"PictureSizeMode", "Picture", [&] { return PropEnum("##picmode", m.picture_size_mode, kPicMode, 3); }});
  }
  rows.push_back({"Placement", "Misc", [&] {
                    int pl = static_cast<int>(m.placement);
                    if (!PropEnum("##placement", pl, kPlacement, 4)) return false;
                    m.placement = static_cast<Placement>(pl);
                    return true;
                  }});
  rows.push_back({"PrintObject", "Misc", [&] { return PropBool("##print", m.print_object); }});
  if (m.kind == K::ComboBox) {
    rows.push_back({"Style", "Behavior", [&] {
                      int st = m.style == 1 ? 2 : 0;
                      if (!PropEnum("##style", st, kStyle, 2)) return false;
                      m.style = st == 2 ? 1 : 0;
                      return true;
                    }});
  }
  rows.push_back({"Top", "Position", [&] { return PropNum("##top", m.top); }});
  if (valued) {
    rows.push_back({"Value", "Data", [&] {
                      if (m.kind == K::CheckBox || m.kind == K::OptionButton || m.kind == K::ToggleButton) {
                        bool on = model::ControlIsOn(m);
                        if (!PropBool("##value", on)) return false;
                        m.value = on ? "True" : "False";
                        return true;
                      }
                      return PropText("##value", m.value, m.kind == K::TextBox && m.multi_line);
                    }});
  }
  rows.push_back({"Visible", "Behavior", [&] { return PropBool("##visible", m.visible); }});
  rows.push_back({"Width", "Position", [&] { return PropNum("##width", m.width); }});
  if (caption || m.kind == K::TextBox) rows.push_back({"WordWrap", "Behavior", [&] { return PropBool("##wrap", m.word_wrap); }});
  // the combo at the top: the control's name and type
  ImGui::SetNextItemWidth(-1);
  const std::string head = m.name + " " + model::ControlTypeName(m.kind);
  ImGui::BeginDisabled();
  ImGui::InputText("##objname", const_cast<char*>(head.c_str()), head.size() + 1, ImGuiInputTextFlags_ReadOnly);
  ImGui::EndDisabled();
  if (BeginDlgTabBar("##proptabs")) {
    if (ImGui::BeginTabItem(TR("Alphabetic"))) tab = 0, ImGui::EndTabItem();
    if (ImGui::BeginTabItem(TR("Categorized"))) tab = 1, ImGui::EndTabItem();
    ImGui::EndTabBar();
  }
  if (tab == 0) {
    std::stable_sort(rows.begin() + 1, rows.end(), [](const PropRow& a, const PropRow& b) { return std::strcmp(a.name, b.name) < 0; });
  } else {
    std::stable_sort(rows.begin(), rows.end(), [](const PropRow& a, const PropRow& b) {
      const int c = std::strcmp(a.category, b.category);
      return c != 0 ? c < 0 : std::strcmp(a.name, b.name) < 0;
    });
  }
  if (ImGui::BeginTable("##props", 2, ImGuiTableFlags_BordersInner | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, S(110));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    const char* cat = nullptr;
    for (const PropRow& r : rows) {
      if (tab == 1 && (!cat || std::strcmp(cat, r.category) != 0)) {
        cat = r.category;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", r.category);
        ImGui::TableNextColumn();
      }
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(r.name);
      ImGui::TableNextColumn();
      ImGui::PushID(r.name);
      if (r.edit()) changed = true;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  // YCGrid keeps a list typed in (Excel fills it from code with AddItem): one item per line
  if (m.kind == K::ComboBox || m.kind == K::ListBox) {
    std::string items;
    for (const auto& it : m.items) items += it + "\n";
    if (EditText(TR("List (one item per line)"), items, true)) {
      m.items.clear();
      size_t p = 0;
      while (p < items.size()) {
        const size_t e = items.find('\n', p);
        const std::string line = items.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!line.empty()) m.items.push_back(line);
        if (e == std::string::npos) break;
        p = e + 1;
      }
      changed = true;
    }
  }
  return changed;
}

namespace {
// Format Control: Excel's dialog, its tabs by the control (Font, Colors and Lines, Size, Protection, Properties,
// Alt Text, Control), applied by OK
struct FcDlg {
  ObjSel obj;
  Control work;
};
FcDlg& FC() {
  static FcDlg f;
  return f;
}
}  // namespace

void OpenFormatControl(AppState& s, ObjSel o) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(o.sheet) : nullptr;
  if (!sh || o.kind != 1) return;
  for (const auto& c : *sh->controls) {
    if (c.id != o.id) continue;
    FC().obj = o;
    FC().work = c;
    s.OpenDialog(Dialog::FormatControl);
  }
}

void DrawFormatControlDialog(AppState& s) {
  if (s.dialog != Dialog::FormatControl) return;
  const char* title = TR("Format Control###yc_dlg_fc");
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(440), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
    return;
  }
  bool close = !open || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
  using K = ControlKind;
  Control& m = FC().work;
  const bool form = m.form;
  if (BeginDlgTabBar("##fctabs")) {
    // Font: the form button's text
    if (form && m.kind == K::Button && ImGui::BeginTabItem(TR("Font"))) {
      PropFont("fcfont", m);
      ImGui::SameLine();
      ImGui::TextUnformatted(TR("Font..."));
      ImGui::Text("%s %s, %.0f", m.font_name.empty() ? "Calibri" : m.font_name.c_str(), m.bold && m.italic ? TR("Bold Italic") : m.bold ? TR("Bold") : m.italic ? TR("Italic") : TR("Regular"),
                  m.font_size > 0 ? m.font_size : 11.0f);
      uint32_t argb = m.fore_color ? CtlBgrToArgb(m.fore_color) : 0;
      if (PickColor(TR("Color:"), &argb)) m.fore_color = argb ? (CtlArgbToBgr(argb) ? CtlArgbToBgr(argb) : 0x010101) : 0;
      ImGui::EndTabItem();
    }
    if (form && (m.kind == K::CheckBox || m.kind == K::OptionButton) && ImGui::BeginTabItem(TR("Colors and Lines"))) {
      ImGui::SeparatorText(TR("Fill"));
      uint32_t argb = m.back_color ? CtlBgrToArgb(m.back_color) : 0;
      if (PickColor(TR("Color:##fill"), &argb)) m.back_color = argb ? (CtlArgbToBgr(argb) ? CtlArgbToBgr(argb) : 0x010101) : 0;
      ImGui::EndTabItem();
    }
    // a form button's Alignment and Margins tabs, as Excel's
    if (form && m.kind == K::Button && ImGui::BeginTabItem(TR("Alignment"))) {
      ImGui::SeparatorText(TR("Text alignment"));
      const char* kH[] = {TR("Left"), TR("Center"), TR("Right")};
      const char* kV[] = {TR("Top"), TR("Center"), TR("Bottom")};
      ImGui::SetNextItemWidth(S(140));
      ImGui::Combo(TR("Horizontal:"), &m.text_halign, kH, 3);
      ImGui::SetNextItemWidth(S(140));
      ImGui::Combo(TR("Vertical:"), &m.text_valign, kV, 3);
      ImGui::SeparatorText(TR("Orientation"));
      ImGui::RadioButton(TR("Horizontal##orient"), &m.text_orientation, 0);
      ImGui::SameLine();
      ImGui::RadioButton(TR("Vertical (stacked)"), &m.text_orientation, 1);
      ImGui::RadioButton(TR("Upward"), &m.text_orientation, 2);
      ImGui::SameLine();
      ImGui::RadioButton(TR("Downward"), &m.text_orientation, 3);
      ImGui::Separator();
      ImGui::Checkbox(TR("Automatic size"), &m.auto_size);
      ImGui::EndTabItem();
    }
    if (form && m.kind == K::Button && ImGui::BeginTabItem(TR("Margins"))) {
      ImGui::Checkbox(TR("Automatic##margins"), &m.auto_margins);
      ImGui::BeginDisabled(m.auto_margins);
      // in inches, as Excel's dialog
      double l = m.margin_l / 72, r = m.margin_r / 72, t = m.margin_t / 72, b = m.margin_b / 72;
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputDouble(TR("Left:"), &l, 0.01, 0.1, "%.2f\"")) m.margin_l = std::max(0.0, l * 72);
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputDouble(TR("Right:"), &r, 0.01, 0.1, "%.2f\"")) m.margin_r = std::max(0.0, r * 72);
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputDouble(TR("Top:"), &t, 0.01, 0.1, "%.2f\"")) m.margin_t = std::max(0.0, t * 72);
      ImGui::SetNextItemWidth(S(120));
      if (ImGui::InputDouble(TR("Bottom:"), &b, 0.01, 0.1, "%.2f\"")) m.margin_b = std::max(0.0, b * 72);
      ImGui::EndDisabled();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Size"))) {
      ImGui::SeparatorText(TR("Size and rotate"));
      EditNum(TR("Height:"), m.height, 1, "%.2f pt");
      EditNum(TR("Width:"), m.width, 1, "%.2f pt");
      m.height = std::max(1.0, m.height), m.width = std::max(1.0, m.width);
      ImGui::BeginDisabled();
      double rot = 0;
      EditNum(TR("Rotation:"), rot, 0, "%.0f\xC2\xB0");
      ImGui::EndDisabled();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Protection"))) {
      ImGui::Checkbox(TR("Locked"), &m.locked);
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(380));
      ImGui::TextDisabled("%s", TR("Locking objects has no effect unless the sheet is protected. To help protect the sheet, choose Protect Sheet on the Review tab."));
      ImGui::PopTextWrapPos();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Properties"))) {
      ImGui::SeparatorText(TR("Object positioning"));
      if (ImGui::RadioButton(TR("Move and size with cells"), m.placement == Placement::MoveAndSize)) m.placement = Placement::MoveAndSize;
      if (ImGui::RadioButton(TR("Move but don't size with cells"), m.placement == Placement::Move)) m.placement = Placement::Move;
      if (ImGui::RadioButton(TR("Don't move or size with cells"), m.placement == Placement::FreeFloating)) m.placement = Placement::FreeFloating;
      ImGui::Separator();
      ImGui::Checkbox(TR("Print object"), &m.print_object);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Alt Text"))) {
      ImGui::TextUnformatted(TR("Alternative text:"));
      char buf[1024];
      std::snprintf(buf, sizeof(buf), "%s", m.alt_text.c_str());
      if (ImGui::InputTextMultiline("##alt", buf, sizeof(buf), ImVec2(S(400), S(110)))) m.alt_text = buf;
      ImGui::EndTabItem();
    }
    const bool has_control = form && m.kind != K::Button && m.kind != K::Label;
    if (has_control && ImGui::BeginTabItem(TR("Control"))) {
      DrawFormatControl(s, m);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::Separator();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    const ObjSel o = FC().obj;
    if (m.form && m.kind == K::Button && m.auto_size) {
      // Automatic size: the button fits its caption (and its margins)
      ImFont* f = m.bold ? ui::F().ui_bold : ui::F().ui;
      const float px = (m.font_size > 0 ? m.font_size : 11.0f);
      const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, m.caption.c_str());
      const double ml = m.auto_margins ? 7.2 : m.margin_l, mr = m.auto_margins ? 7.2 : m.margin_r, mt = m.auto_margins ? 3.6 : m.margin_t,
                   mb = m.auto_margins ? 3.6 : m.margin_b;
      const bool turned = m.text_orientation != 0;
      m.width = (turned ? ts.y : ts.x) + ml + mr;
      m.height = (turned ? ts.x : ts.y) + mt + mb;
    }
    const Control w = m;
    s.engine->PostUser(TR("Format Control"), [o, w](Document& d) { d.UpdateControl(o.sheet, o.id, [&](Control& x) { x = w; }); });
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// Format Shape / Format Picture: Fill, Line, Text and Size, chosen from palettes and lists as in Excel
bool DrawFormatShape(ShapeObj& m) {
  bool changed = false;
  const bool line_only = m.kind == ShapeKind::Line || m.kind == ShapeKind::Arrow;
  if (m.kind != ShapeKind::Picture && !line_only) {
    ImGui::SeparatorText(TR("Fill"));
    if (ImGui::RadioButton(TR("No fill"), !m.fill_visible)) m.fill_visible = false, changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Solid fill"), m.fill_visible)) m.fill_visible = true, changed = true;
    if (m.fill_visible) {
      changed |= PickColor(TR("Color"), &m.fill_color, false);
      int tr = static_cast<int>(std::lround(m.fill_transparency * 100));
      ImGui::SetNextItemWidth(S(140));
      if (ImGui::SliderInt(TR("Transparency"), &tr, 0, 100, "%d%%")) m.fill_transparency = std::clamp(tr, 0, 100) / 100.0f, changed = true;
    }
  }
  ImGui::SeparatorText(TR("Line"));
  if (ImGui::RadioButton(TR("No line"), !m.line_visible)) m.line_visible = false, changed = true;
  ImGui::SameLine();
  if (ImGui::RadioButton(TR("Solid line"), m.line_visible)) m.line_visible = true, changed = true;
  if (m.line_visible) {
    changed |= PickColor(TR("Color##line"), &m.line_color, false);
    changed |= SpinFloat(TR("Width (pt)"), m.line_weight, 0.25f, 0.25f, 20.0f, "%.2f");
    const char* dashes[] = {TR("Solid"), TR("Dash"), TR("Round Dot"), TR("Square Dot"), TR("Dash Dot"), TR("Long Dash")};
    ImGui::SetNextItemWidth(S(140));
    changed |= ImGui::Combo(TR("Dash type"), &m.line_dash, dashes, 6);
    if (line_only) {
      const char* heads[] = {TR("None##arrow"), TR("Arrow##head")};
      ImGui::SetNextItemWidth(S(140));
      changed |= ImGui::Combo(TR("Begin Arrow type"), &m.begin_arrow, heads, 2);
      int end = m.kind == ShapeKind::Arrow ? 1 : m.end_arrow;
      ImGui::SetNextItemWidth(S(140));
      if (ImGui::Combo(TR("End Arrow type"), &end, heads, 2)) {
        m.end_arrow = end;
        if (m.kind == ShapeKind::Arrow && end == 0) m.kind = ShapeKind::Line;
        changed = true;
      }
    }
  }
  if (HasText(m)) {
    ImGui::SeparatorText(TR("Text"));
    changed |= EditText(TR("Text"), m.text, true);
    changed |= PickFontSize(TR("Size"), &m.font_size, 11);
    ImGui::SameLine();
    changed |= ImGui::Checkbox(TR("Bold"), &m.bold);
    ImGui::SameLine();
    changed |= ImGui::Checkbox(TR("Italic"), &m.italic);
    changed |= PickColor(TR("Font Color"), &m.font_color, false);
    ImGui::TextUnformatted(TR("Horizontal"));
    const char* hs[] = {TR("Left"), TR("Center"), TR("Right")};
    for (int k = 0; k < 3; ++k) {
      if (k) ImGui::SameLine();
      if (ImGui::RadioButton((std::string(hs[k]) + "##h").c_str(), m.halign == k)) m.halign = k, changed = true;
    }
    ImGui::TextUnformatted(TR("Vertical"));
    const char* vs[] = {TR("Top##ctx_align"), TR("Middle"), TR("Bottom##ctx_align")};
    for (int k = 0; k < 3; ++k) {
      if (k) ImGui::SameLine();
      if (ImGui::RadioButton((std::string(vs[k]) + "##v").c_str(), m.valign == k)) m.valign = k, changed = true;
    }
    // Text Box: margins, wrap, autofit (Format Shape > Text Options > Text Box)
    changed |= EditNum(TR("Left margin (pt)"), m.margin_l, 0.5, "%.1f");
    changed |= EditNum(TR("Right margin (pt)"), m.margin_r, 0.5, "%.1f");
    changed |= EditNum(TR("Top margin (pt)"), m.margin_t, 0.5, "%.1f");
    changed |= EditNum(TR("Bottom margin (pt)"), m.margin_b, 0.5, "%.1f");
    for (double* v : {&m.margin_l, &m.margin_r, &m.margin_t, &m.margin_b}) *v = std::clamp(*v, 0.0, 200.0);
    changed |= ImGui::Checkbox(TR("Wrap text in shape"), &m.wrap);
    changed |= ImGui::Checkbox(TR("Resize shape to fit text"), &m.autofit);
  }
  changed |= DrawShapeEffectsSection(m);  // Effects (shape_effects.cpp)
  ImGui::SeparatorText(TR("Size"));
  const double h0 = m.height, w0 = m.width;
  if (EditNum(TR("Height (pt)"), m.height, 1, "%.1f")) {
    m.height = std::max(1.0, m.height);
    if (m.lock_aspect && h0 > 0) m.width = w0 * m.height / h0;
    changed = true;
  }
  if (EditNum(TR("Width (pt)"), m.width, 1, "%.1f")) {
    m.width = std::max(1.0, m.width);
    if (m.lock_aspect && w0 > 0) m.height = h0 * m.width / w0;
    changed = true;
  }
  if (EditNum(TR("Rotation"), m.rotation, 1, "%.0f\xC2\xB0")) m.rotation = std::fmod(std::fmod(m.rotation, 360.0) + 360.0, 360.0), changed = true;
  if (m.orig_w > 0 && m.orig_h > 0) {
    double sh = m.height / m.orig_h * 100, sw = m.width / m.orig_w * 100;
    if (EditNum(TR("Scale Height (%)"), sh, 1, "%.0f")) {
      m.height = std::max(1.0, m.orig_h * sh / 100);
      if (m.lock_aspect) m.width = std::max(1.0, m.orig_w * sh / 100);
      changed = true;
    }
    if (EditNum(TR("Scale Width (%)"), sw, 1, "%.0f")) {
      m.width = std::max(1.0, m.orig_w * sw / 100);
      if (m.lock_aspect) m.height = std::max(1.0, m.orig_h * sw / 100);
      changed = true;
    }
  }
  changed |= ImGui::Checkbox(TR("Lock aspect ratio"), &m.lock_aspect);
  if (m.kind == ShapeKind::Picture) {
    ImGui::SeparatorText(TR("Crop"));
    changed |= EditNum(TR("Crop left (pt)"), m.crop_l, 1, "%.1f");
    changed |= EditNum(TR("Crop right (pt)"), m.crop_r, 1, "%.1f");
    changed |= EditNum(TR("Crop top (pt)"), m.crop_t, 1, "%.1f");
    changed |= EditNum(TR("Crop bottom (pt)"), m.crop_b, 1, "%.1f");
    for (double* v : {&m.crop_l, &m.crop_r, &m.crop_t, &m.crop_b}) *v = std::max(0.0, *v);
  }
  ImGui::SeparatorText(TR("Properties"));
  if (ImGui::RadioButton(TR("Move and size with cells"), m.placement == Placement::MoveAndSize)) m.placement = Placement::MoveAndSize, changed = true;
  if (ImGui::RadioButton(TR("Move but don't size with cells"), m.placement == Placement::Move)) m.placement = Placement::Move, changed = true;
  if (ImGui::RadioButton(TR("Don't move or size with cells"), m.placement == Placement::FreeFloating)) m.placement = Placement::FreeFloating, changed = true;
  changed |= ImGui::Checkbox(TR("Print object"), &m.print_object);
  changed |= ImGui::Checkbox(TR("Locked"), &m.locked);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", TR("(when the sheet is protected)"));
  ImGui::SeparatorText(TR("Alt Text"));
  changed |= EditText(TR("Alt Text"), m.alt_text, true);
  ImGui::TextDisabled("%s %s", TR("Macro:"), m.on_action.empty() ? TR("(none)") : m.on_action.c_str());
  return changed;
}

bool CartesianChart(ChartType t) {
  using T = ChartType;
  return t != T::Pie && t != T::Doughnut && t != T::Pie3D && t != T::FilledMap && t != T::Treemap && t != T::Sunburst && t != T::Radar && t != T::RadarMarkers && t != T::RadarFilled;
}

ChartType SeriesTypeOf(const ChartObj& m, const model::ChartSeries& se) {
  return se.type >= 0 && se.type < model::kChartTypeCount ? static_cast<ChartType>(se.type) : m.type;
}

// Excel's Series Options, first in Format Data Series: what this kind of chart lets you change, as sliders and boxes
bool EditSeriesOptions(ChartObj& m, model::ChartSeries& se) {
  using CT = ChartType;
  const CT st = SeriesTypeOf(m, se);
  const bool bars = engine::IsBarType(st);
  const bool lines = engine::IsLineType(st) || engine::IsScatterType(st);
  const bool pie = st == CT::Pie || st == CT::Doughnut;
  const bool box = st == CT::BoxWhisker;
  const bool waterfall = st == CT::Waterfall;
  if (!bars && !lines && !pie && !box && !waterfall) return false;
  bool changed = false;
  ImGui::SeparatorText(TR("Series Options"));
  auto slider = [&](const char* label, int* v, int lo, int hi, const char* fmt) {
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::SliderInt(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) changed = true;
  };
  if (bars) {
    if (!engine::IsStackedType(st)) slider(TR("Series Overlap"), &m.overlap, -100, 100, "%d%%");
    slider(TR("Gap Width"), &m.gap_width, 0, 500, "%d%%");
  }
  if (waterfall) {
    changed |= ImGui::Checkbox(TR("Show connector lines"), &m.connectors);
    slider(TR("Gap Width"), &m.gap_width, 0, 500, "%d%%");
  }
  if (lines && st != CT::Bubble) changed |= ImGui::Checkbox(TR("Smoothed line"), &se.smooth);
  if (pie) {
    slider(TR("Angle of first slice"), &m.first_slice_angle, 0, 360, "%d\xC2\xB0");
    slider(TR("Pie Explosion"), &se.explosion, 0, 400, "%d%%");
    if (st == CT::Doughnut) slider(TR("Doughnut Hole Size"), &m.hole_size, 10, 90, "%d%%");
  }
  if (box) {
    auto flag = [&](const char* label, int bit) {
      bool on = (se.box_flags & bit) != 0;
      if (ImGui::Checkbox(label, &on)) {
        se.box_flags = on ? (se.box_flags | bit) : (se.box_flags & ~bit);
        changed = true;
      }
    };
    flag(TR("Show inner points"), 1);
    flag(TR("Show outlier points"), 2);
    flag(TR("Show mean markers"), 4);
  }
  return changed;
}

// Excel BGR <-> the palette's ARGB (0 stays automatic)
uint32_t BgrToArgb(uint32_t c) { return c ? 0xFF000000u | SwapRB(c & 0xFFFFFF) : 0; }
uint32_t ArgbToBgr(uint32_t c) { return c ? SwapRB(c & 0xFFFFFF) : 0; }

bool PickBgr(const char* label, uint32_t* bgr, bool allow_auto = true);

// a combo box the UI item recorder finds by its label (combos report none, AGENTS.md)
bool NotedCombo(const char* label, const char* id, int* v, const char* const items[], int n, float w) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ui::NoteUiItem(label, ImRect(at, at + ImVec2(w, ImGui::GetFrameHeight())));
  ImGui::SetNextItemWidth(w);
  return ImGui::Combo(id, v, items, n);
}

// Excel's 3-D Rotation presets (Format Chart Area > Effects > 3-D Rotation): X / Y rotation and perspective together
bool Pick3DPreset(ChartObj& m) {
  struct Preset {
    const char* name;
    double rot, elev, persp;
  };
  static const Preset kPresets[] = {
      {TR("Isometric Left Down"), 45, 35, 0},     {TR("Isometric Right Up"), 315, 35, 0},  {TR("Off Axis 1 Left"), 64, 18, 0},
      {TR("Off Axis 1 Right"), 296, 18, 0},       {TR("Off Axis 2 Left"), 34, 10, 0},       {TR("Off Axis 2 Right"), 326, 10, 0},
      {TR("Perspective Front"), 0, 0, 30},         {TR("Perspective Left"), 20, 0, 30},      {TR("Perspective Right"), 340, 0, 30},
      {TR("Perspective Above"), 0, 30, 30},        {TR("Perspective Below"), 0, -15, 30},    {TR("Perspective Relaxed"), 0, 45, 15},
      {TR("Perspective Heroic Extreme Left"), 34, 8, 80}, {TR("Perspective Heroic Extreme Right"), 326, 8, 80},
  };
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ui::NoteUiItem(TR("Presets"), ImRect(at, at + ImVec2(S(200), ImGui::GetFrameHeight())));
  ImGui::SetNextItemWidth(S(200));
  bool changed = false;
  if (ImGui::BeginCombo("##rot_presets", TR("Presets"))) {
    for (const Preset& p : kPresets) {
      const bool cur = m.view3d && m.rotation == p.rot && m.elevation == p.elev && m.perspective == p.persp;
      if (ImGui::Selectable(p.name, cur)) {
        m.view3d = true;
        m.rotation = p.rot;
        m.elevation = p.elev;
        m.perspective = p.persp;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Format Data Series > Series Options of the chart types with options of their own: pie of pie and bar of pie (Split
// Series By, Values in second plot, Second Plot Size, Gap Width), box and whisker (mean line, quartile calculation),
// treemap (Label Options), filled map (projection, area, labels, Series Color), histogram, Pareto and funnel (Gap Width)
bool EditChartTypeOptions(ChartObj& m, model::ChartSeries& se, const engine::ChartValues* vals) {
  using CT = ChartType;
  bool changed = false;
  auto slider = [&](const char* label, int* v, int lo, int hi, const char* fmt) {
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::SliderInt(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) changed = true;
  };
  auto radio = [&](const char* label, int* v, int val) {
    if (ImGui::RadioButton(label, *v == val) && *v != val) *v = val, changed = true;
  };
  switch (m.type) {
    case CT::PieOfPie:
    case CT::BarOfPie: {
      ImGui::SeparatorText(TR("Series Options"));
      ImGui::TextUnformatted(TR("Split Series By"));
      static const char* kSplit[] = {TR("Position"), TR("Value"), TR("Percentage value"), TR("Custom")};
      int st = std::clamp(m.split_type, 1, 4) - 1;
      if (NotedCombo(TR("Split Series By"), "##split_by", &st, kSplit, 4, S(170))) {
        m.split_type = st + 1;
        m.split_value = 0;
        changed = true;
      }
      if (m.split_type == 1) {
        int nv = m.split_value > 0 ? static_cast<int>(m.split_value) : 3;
        if (SpinInt(TR("Values in second plot"), nv, 1, 254)) m.split_value = nv, changed = true;
      } else if (m.split_type == 2 || m.split_type == 3) {
        double v = m.split_value > 0 ? m.split_value : (m.split_type == 3 ? 10.0 : 0.0);
        if (EditNum(TR("Values less than"), v, 1, m.split_type == 3 ? "%.0f%%" : "%.6g")) m.split_value = std::max(0.0, v), changed = true;
      } else {
        // custom: each point goes to the first or the second plot (Excel: Point Belongs To)
        const int n = vals && !vals->series.empty() ? std::min(vals->series[0].N(), 255) : 0;
        for (int k = 0; k < n; ++k) {
          ImGui::PushID(k);
          bool second = std::binary_search(m.second_points.begin(), m.second_points.end(), k);
          const std::string nm = static_cast<size_t>(k) < vals->cats.size() ? vals->cats[static_cast<size_t>(k)] : std::to_string(k + 1);
          if (ImGui::Checkbox((nm + "  " + TR("(second plot)")).c_str(), &second)) {
            if (second) m.second_points.push_back(k);
            else m.second_points.erase(std::remove(m.second_points.begin(), m.second_points.end(), k), m.second_points.end());
            std::sort(m.second_points.begin(), m.second_points.end());
            changed = true;
          }
          ImGui::PopID();
        }
      }
      slider(TR("Gap Width##ofpie"), &m.pie_gap, 0, 500, "%d%%");
      slider(TR("Second Plot Size"), &m.second_size, 5, 200, "%d%%");
      break;
    }
    case CT::BoxWhisker: {
      ImGui::SeparatorText(TR("Series Options##box"));
      bool line = (se.box_flags & 8) != 0;
      if (ImGui::Checkbox(TR("Show mean line"), &line)) se.box_flags = line ? (se.box_flags | 8) : (se.box_flags & ~8), changed = true;
      ImGui::TextUnformatted(TR("Quartile Calculation"));
      if (ImGui::RadioButton(TR("Inclusive median"), se.inclusive_median)) se.inclusive_median = true, changed = true;
      if (ImGui::RadioButton(TR("Exclusive median"), !se.inclusive_median)) se.inclusive_median = false, changed = true;
      break;
    }
    case CT::Treemap:
      ImGui::SeparatorText(TR("Label Options"));
      radio(TR("None##treemap_labels"), &se.parent_labels, 0);
      radio(TR("Overlapping"), &se.parent_labels, 2);
      radio(TR("Banner"), &se.parent_labels, 1);
      break;
    case CT::FilledMap: {
      ImGui::SeparatorText(TR("Series Options##map"));
      static const char* kProj[] = {TR("Automatic"), TR("Mercator"), TR("Miller"), TR("Albers"), TR("Robinson")};
      ImGui::TextUnformatted(TR("Map projection"));
      changed |= NotedCombo(TR("Map projection"), "##map_proj", &se.geo_projection, kProj, 5, S(170));
      static const char* kArea[] = {TR("Automatic"), TR("Only regions with data"), TR("Country/region"), TR("Countries/regions"), TR("World")};
      static const int kAreaVal[] = {0, 1, 5, 6, 7};
      int ai = 0;
      for (int k = 0; k < 5; ++k) {
        if (kAreaVal[k] == se.geo_area) ai = k;
      }
      ImGui::TextUnformatted(TR("Map area"));
      if (NotedCombo(TR("Map area"), "##map_area", &ai, kArea, 5, S(170))) se.geo_area = kAreaVal[ai], changed = true;
      static const char* kLabels[] = {TR("None"), TR("Show only best fit"), TR("Show all")};
      ImGui::TextUnformatted(TR("Map labels"));
      changed |= NotedCombo(TR("Map labels"), "##map_labels", &se.geo_labels, kLabels, 3, S(170));
      ImGui::SeparatorText(TR("Series Color"));
      static const char* kColors[] = {TR("Sequential (2-color)"), TR("Diverging (3-color)")};
      changed |= NotedCombo(TR("Series Color"), "##map_colors", &se.geo_colors, kColors, 2, S(170));
      changed |= PickBgr(TR("Minimum"), &se.geo_min);
      if (se.geo_colors == 1) changed |= PickBgr(TR("Midpoint"), &se.geo_mid);
      changed |= PickBgr(TR("Maximum"), &se.geo_max);
      break;
    }
    case CT::Histogram:
    case CT::Pareto:
    case CT::Funnel:
      ImGui::SeparatorText(TR("Series Options##gap"));
      slider(TR("Gap Width"), &m.gap_width, 0, 500, "%d%%");
      break;
    default: break;
  }
  return changed;
}

// Format Up Bars / Down Bars / High-Low Lines (stock charts), Format Floor / Walls (3-D charts)
bool EditChartElemFill(ChartObj& m, model::ChartElem el) {
  using E = model::ChartElem;
  bool changed = false;
  switch (el) {
    case E::UpBars:
    case E::DownBars:
      ImGui::SeparatorText(TR("Fill"));
      changed |= PickColor(TR("Fill Color"), el == E::UpBars ? &m.up_fill : &m.down_fill);
      ImGui::SeparatorText(TR("Series Options##updown"));
      ImGui::SetNextItemWidth(S(170));
      changed |= ImGui::SliderInt(TR("Gap Width##updown"), &m.updown_gap, 0, 500, "%d%%", ImGuiSliderFlags_AlwaysClamp);
      break;
    case E::HiLoLines:
      ImGui::SeparatorText(TR("Line"));
      changed |= PickColor(TR("Color"), &m.hilo_line);
      break;
    case E::Floor:
    case E::Walls:
      ImGui::SeparatorText(TR("Fill"));
      changed |= PickColor(TR("Fill Color"), el == E::Floor ? &m.floor_fill : &m.wall_fill);
      break;
    default: break;
  }
  return changed;
}

// Format Axis > Axis Type of a category axis (Automatic, Text axis, Date axis) and a date axis' bounds and units
bool EditDateAxis(ChartObj& m, const engine::ChartValues* vals) {
  bool changed = false;
  ImGui::SeparatorText(TR("Axis Type"));
  if (ImGui::RadioButton(TR("Automatically select based on data"), m.cat_type == 0) && m.cat_type != 0) m.cat_type = 0, changed = true;
  if (ImGui::RadioButton(TR("Text axis"), m.cat_type == 1) && m.cat_type != 1) m.cat_type = 1, changed = true;
  if (ImGui::RadioButton(TR("Date axis"), m.cat_type == 2) && m.cat_type != 2) m.cat_type = 2, changed = true;
  const DatePlan dp = vals ? PlanDateAxis(m, *vals, vals->cat_vals.size()) : DatePlan{};
  if (!dp.on) return changed;
  // the bounds as dates (each automatic or fixed) and the base / major units
  const std::string dfmt = !m.cat_axis.number_format.empty() ? m.cat_axis.number_format : !vals->cat_fmt.empty() ? vals->cat_fmt : std::string("yyyy/m/d");
  auto bound = [&](const char* label, bool* is_auto, double* v, double shown, const char* id) {
    ImGui::PushID(id);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(S(90));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s", engine::FormatChartNumber(*is_auto ? shown : *v, dfmt).c_str());
    ImGui::SetNextItemWidth(S(110));
    if (ImGui::InputText("##date", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
      double serial = 0;
      if (formula::ParseDateTime(buf, &serial)) *v = serial, *is_auto = false, changed = true;
    }
    ImGui::SameLine();
    if (*is_auto) ImGui::TextDisabled("%s", TR("Auto"));
    else if (ImGui::SmallButton(TR("Reset"))) *is_auto = true, changed = true;
    ImGui::PopID();
  };
  ImGui::SeparatorText(TR("Bounds"));
  bound(TR("Minimum"), &m.cat_axis.min_auto, &m.cat_axis.min, UnitSerial(dp.u0, dp.unit), "dmin");
  bound(TR("Maximum"), &m.cat_axis.max_auto, &m.cat_axis.max, UnitSerial(dp.u1, dp.unit), "dmax");
  ImGui::SeparatorText(TR("Units"));
  static const char* kUnits[] = {TR("Days"), TR("Months"), TR("Years")};
  ImGui::TextUnformatted(TR("Base"));
  ImGui::SameLine(S(90));
  int bu = dp.unit;
  if (NotedCombo(TR("Base"), "##base_unit", &bu, kUnits, 3, S(110))) m.base_unit = bu, changed = true;
  ImGui::SameLine();
  if (m.base_unit < 0) ImGui::TextDisabled("%s", TR("Auto"));
  else if (ImGui::SmallButton(TR("Reset##base"))) m.base_unit = -1, changed = true;
  ImGui::TextUnformatted(TR("Major"));
  ImGui::SameLine(S(90));
  int major = m.cat_axis.major >= 1 ? static_cast<int>(m.cat_axis.major) : 1;
  ImGui::SetNextItemWidth(S(50));
  if (ImGui::InputInt("##major_n", &major, 0)) m.cat_axis.major = std::max(1, major), changed = true;
  ImGui::SameLine();
  int mu = m.major_unit_scale >= 0 ? m.major_unit_scale : dp.unit;
  if (NotedCombo(TR("Major unit"), "##major_unit", &mu, kUnits, 3, S(80))) m.major_unit_scale = mu, changed = true;
  return changed;
}

bool PickBgr(const char* label, uint32_t* bgr, bool allow_auto) {
  uint32_t argb = BgrToArgb(*bgr);
  if (!PickColor(label, &argb, allow_auto)) return false;
  *bgr = ArgbToBgr(argb);
  return true;
}

// Excel's dash types (Format Data Series > Line > Dash type)
bool PickDash(const char* label, int* dash) {
  const char* names[] = {TR("Solid"), TR("Round Dot"), TR("Square Dot"), TR("Dash"), TR("Dash Dot"), TR("Long Dash")};
  int d = std::clamp(*dash, 0, 5);
  ImGui::SetNextItemWidth(S(150));
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ui::NoteUiItem(label, ImRect(at, at + ImVec2(S(150), ImGui::GetFrameHeight())));  // combos report no label (AGENTS.md)
  if (!ImGui::Combo(label, &d, names, 6)) return false;
  *dash = d;
  return true;
}

// a series' data and formatting (Format Data Series); its ranges are chosen with Select Data, as in Excel
bool EditSeriesBlock(AppState& s, ObjSel o, const ChartObj& m, model::ChartSeries& se, const engine::ChartValues* vals, int index) {
  bool changed = false;
  if (se.python_data) {
    ImGui::Text(TR("%s (Python data, %zu points)"), se.name.c_str(), se.data ? se.data->y.size() : size_t{0});
  } else {
    ImGui::TextDisabled("%s", se.values.empty() ? TR("(no values yet)") : se.values.c_str());
    if (ImGui::Button(TR("Select Data..."))) OpenSelectData(s, o.sheet, o.id);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Choose the series' name, values and categories on the sheet"));
  }
  using CT = ChartType;
  const CT st = SeriesTypeOf(m, se);
  if (CartesianChart(m.type) && !engine::IsSceneChart(m.type) && !engine::IsHorizontalBar(m.type)) {
    ImGui::SeparatorText(TR("Plot Series On"));
    if (ImGui::RadioButton(TR("Primary Axis"), se.axis_group == 0)) {
      se.axis_group = 0;
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Secondary Axis"), se.axis_group == 1)) {
      se.axis_group = 1;
      changed = true;
    }
  }
  // what the series is drawn as decides what can be set, as in Excel: a line's color, width, dash and markers; a
  // column's, an area's or a slice's fill and border; tiles, map regions and 3-D shapes only their color
  const bool lines = engine::IsLineType(st) || (engine::IsScatterType(st) && st != CT::Bubble) || st == CT::Radar || st == CT::RadarMarkers || engine::IsStockType(st);
  const bool filled = engine::IsBarType(st) || engine::IsAreaType(st) || st == CT::Pie || st == CT::Doughnut || st == CT::Bubble;
  const bool markers_on = lines && !engine::IsStockType(st) && st != CT::Radar;
  if (filled) {
    // Fill: No fill, Solid fill, Gradient fill, Picture or texture fill, Pattern fill, Automatic (Excel's list)
    ImGui::SeparatorText(TR("Fill"));
    const int fk = se.no_fill ? 0 : se.fill_type == 1 ? 2 : se.fill_type == 2 ? 3 : se.fill_type == 3 ? 4 : se.color ? 1 : 5;
    const char* fills[] = {TR("No fill"), TR("Solid fill"), TR("Gradient fill"), TR("Picture or texture fill"), TR("Pattern fill"), TR("Automatic##fill")};
    for (int k = 0; k < 6; ++k) {
      if (ImGui::RadioButton(fills[k], fk == k) && fk != k) {
        se.no_fill = k == 0;
        se.fill_type = k == 2 ? 1 : k == 3 ? 2 : k == 4 ? 3 : 0;
        if (k == 1 && !se.color) se.color = (static_cast<uint32_t>(engine::ChartPalette(static_cast<size_t>(index))) & 0xFFFFFFu);
        if (k == 5) se.color = 0;
        changed = true;
      }
      if (k % 2 == 0 && k < 5) ImGui::SameLine(S(150));
    }
    if (!se.no_fill) {
      if (st == CT::Pie || st == CT::Doughnut) changed |= ImGui::Checkbox(TR("Vary colors by slice"), &se.vary_colors);
      else if (engine::IsBarType(st)) changed |= ImGui::Checkbox(TR("Vary colors by point"), &se.vary_points);
      if (se.fill_type == 2) {
        if (ImGui::Button(TR("Insert..."))) {
          const std::wstring path = PickImageFile(s.hwnd);
          if (!path.empty()) {
            std::ifstream f(path, std::ios::binary);
            se.picture.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            changed = true;
          }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", se.picture.empty() ? TR("(no picture)") : TR("(picture)"));
        changed |= ImGui::Checkbox(TR("Tile picture as texture"), &se.picture_tile);
        if (se.picture_tile) {
          auto num = [&](const char* label, float* v, float step, float lo, float hi, const char* fmt) {
            if (SpinFloat(label, *v, step, lo, hi, fmt)) changed = true;
          };
          num(TR("Offset X (pt)"), &se.tile_ox, 1, -2000, 2000, "%.1f");
          num(TR("Offset Y (pt)"), &se.tile_oy, 1, -2000, 2000, "%.1f");
          num(TR("Scale X (%)"), &se.tile_sx, 10, 1, 1000, "%.0f");
          num(TR("Scale Y (%)"), &se.tile_sy, 10, 1, 1000, "%.0f");
          const char* aligns[] = {TR("Top left"), TR("Top"), TR("Top right"), TR("Left"), TR("Center"), TR("Right"), TR("Bottom left"), TR("Bottom"), TR("Bottom right")};
          const char* mirrors[] = {TR("None"), TR("Horizontal"), TR("Vertical"), TR("Both")};
          auto combo = [&](const char* label, int* v, const char* const* items, int n) {
            int k = std::clamp(*v, 0, n - 1);
            ImGui::SetNextItemWidth(S(150));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ui::NoteUiItem(label, ImRect(at, at + ImVec2(S(150), ImGui::GetFrameHeight())));  // combos report no label (AGENTS.md)
            if (ImGui::Combo(label, &k, items, n)) *v = k, changed = true;
          };
          combo(TR("Alignment"), &se.tile_align, aligns, 9);
          combo(TR("Mirror type"), &se.tile_mirror, mirrors, 4);
        }
      } else if (se.fill_type == 3) {
        const char* pats[] = {TR("5%"), TR("10%"), TR("25%"), TR("50%"), TR("Light horizontal"), TR("Light vertical"), TR("Light downward diagonal"),
                              TR("Light upward diagonal"), TR("Dark horizontal"), TR("Dark vertical"), TR("Small grid"), TR("Diagonal cross")};
        int pk = std::clamp(se.pattern, 0, model::kChartPatternCount - 1);
        ImGui::SetNextItemWidth(S(180));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ui::NoteUiItem(TR("Pattern"), ImRect(at, at + ImVec2(S(180), ImGui::GetFrameHeight())));  // combos report no label (AGENTS.md)
        if (ImGui::Combo(TR("Pattern"), &pk, pats, model::kChartPatternCount)) se.pattern = pk, changed = true;
        changed |= PickBgr(TR("Foreground"), &se.color);
        changed |= PickBgr(TR("Background"), &se.pattern_back);
      } else {
        changed |= PickBgr(TR("Color"), &se.color);
        if (se.fill_type == 1) {
          changed |= PickBgr(TR("Second color"), &se.grad_color2);
          ImGui::SetNextItemWidth(S(170));
          if (ImGui::SliderInt(TR("Angle"), &se.grad_angle, 0, 359, "%d\xC2\xB0", ImGuiSliderFlags_AlwaysClamp)) changed = true;
        }
      }
      ImGui::SetNextItemWidth(S(170));
      if (ImGui::SliderInt(TR("Transparency"), &se.transparency, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) changed = true;
      if (engine::IsBarType(st)) {
        changed |= ImGui::Checkbox(TR("Invert if negative"), &se.invert_negative);
        if (se.invert_negative) changed |= PickBgr(TR("Invert color"), &se.invert_color);
      }
    }
    ImGui::SeparatorText(TR("Border"));
    const bool auto_border = st == CT::Pie || st == CT::Doughnut || st == CT::Bubble;  // these have one by default
    const int bk = se.no_line ? 0 : se.line_color ? 1 : 2;
    if (ImGui::RadioButton(TR("No line"), bk == 0 || (bk == 2 && !auto_border)) && bk != 0) se.no_line = true, changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Solid line"), bk == 1) && bk != 1) {
      se.no_line = false;
      se.line_color = 0x404040;
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Automatic##border"), bk == 2 && auto_border) && !(bk == 2 && auto_border)) se.no_line = false, se.line_color = 0, changed = true;
    if (bk == 1 || (bk == 2 && auto_border)) {
      changed |= PickBgr(TR("Border color"), &se.line_color);
      float w = se.line_weight > 0 ? se.line_weight : 0.75f;
      if (SpinFloat(TR("Border width (pt)"), w, 0.25f, 0.25f, 20.0f, "%.2f")) se.line_weight = w, changed = true;
      changed |= PickDash(TR("Dash type"), &se.dash);
    }
  }
  if (lines) {
    // Line: No line, Solid line, Automatic; color, width, compound type, dash type, cap and join types, arrows
    ImGui::SeparatorText(TR("Line"));
    const int lk = se.no_line ? 0 : se.color || se.line_color ? 1 : 2;
    if (ImGui::RadioButton(TR("No line"), lk == 0) && lk != 0) se.no_line = true, changed = true;
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Solid line"), lk == 1) && lk != 1) {
      se.no_line = false;
      if (!se.color) se.color = (static_cast<uint32_t>(engine::ChartPalette(static_cast<size_t>(index))) & 0xFFFFFFu);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Automatic##line"), lk == 2) && lk != 2) se.no_line = false, se.color = 0, se.line_color = 0, changed = true;
    changed |= PickBgr(TR("Color"), &se.color);
    if (!se.no_line) {
      float w = se.line_weight > 0 ? se.line_weight : 1.5f;
      if (SpinFloat(TR("Width (pt)"), w, 0.25f, 0.25f, 20.0f, "%.2f")) {
        se.line_weight = w;
        changed = true;
      }
      auto combo = [&](const char* label, int* v, const char* const* items, int n) {
        int k = std::clamp(*v, 0, n - 1);
        ImGui::SetNextItemWidth(S(150));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ui::NoteUiItem(label, ImRect(at, at + ImVec2(S(150), ImGui::GetFrameHeight())));  // combos report no label (AGENTS.md)
        if (ImGui::Combo(label, &k, items, n)) *v = k, changed = true;
      };
      const char* compounds[] = {TR("Single"), TR("Double"), TR("Thick Thin"), TR("Thin Thick"), TR("Triple")};
      combo(TR("Compound type"), &se.compound, compounds, 5);
      changed |= PickDash(TR("Dash type"), &se.dash);
      const char* caps[] = {TR("Flat"), TR("Round"), TR("Square")};
      combo(TR("Cap type"), &se.cap, caps, 3);
      const char* joins[] = {TR("Round"), TR("Bevel"), TR("Miter")};
      combo(TR("Join type"), &se.join, joins, 3);
      const char* arrows[] = {TR("No Arrow"), TR("Arrow"), TR("Open Arrow"), TR("Stealth Arrow"), TR("Diamond Arrow"), TR("Oval Arrow")};
      combo(TR("Begin Arrow type"), &se.begin_arrow, arrows, 6);
      combo(TR("End Arrow type"), &se.end_arrow, arrows, 6);
    }
  }
  if (markers_on) {
    ImGui::SeparatorText(TR("Marker"));
    const char* markers[] = {TR("Automatic"), TR("None"), TR("Circle"), TR("Square"), TR("Diamond"), TR("Triangle"), TR("X"), TR("Plus")};
    int mk = std::clamp(se.marker + 1, 0, 7);
    ImGui::SetNextItemWidth(S(150));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ui::NoteUiItem(TR("Marker"), ImRect(at, at + ImVec2(S(150), ImGui::GetFrameHeight())));  // combos report no label (AGENTS.md)
    if (ImGui::Combo(TR("Marker"), &mk, markers, 8)) {
      se.marker = mk - 1;
      changed = true;
    }
    if (se.marker > 0) {
      float ms = se.marker_size > 0 ? se.marker_size : 5;
      if (SpinFloat(TR("Marker size"), ms, 1, 2, 72, "%.0f")) {
        se.marker_size = ms;
        changed = true;
      }
    }
    if (se.marker != 0) {
      changed |= PickBgr(TR("Marker fill"), &se.marker_fill);
      changed |= PickBgr(TR("Marker border"), &se.marker_line);
    }
  }
  if (!filled && !lines) {
    ImGui::SeparatorText(TR("Fill & Line"));
    changed |= PickBgr(TR("Color"), &se.color);
  }
  changed |= EditSeriesFormat(s, m, se, vals, index);
  return changed;
}

// Format Data Point: one point's fill and border, a pie slice pulled out, a waterfall total, its own data label
bool EditPointFormat(const ChartObj& m, model::ChartSeries& se, int point) {
  bool changed = false;
  auto it = std::find_if(se.points.begin(), se.points.end(), [&](const model::ChartPointFormat& p) { return p.index == point; });
  model::ChartPointFormat pf;
  pf.index = point;
  if (it != se.points.end()) pf = *it;
  const ChartType st = SeriesTypeOf(m, se);
  ImGui::TextDisabled(TR("Point %d"), point + 1);
  if (st == ChartType::Pie || st == ChartType::Doughnut) {
    ImGui::SeparatorText(TR("Series Options"));
    int e = pf.explosion >= 0 ? pf.explosion : se.explosion;
    ImGui::SetNextItemWidth(S(170));
    if (ImGui::SliderInt(TR("Point Explosion"), &e, 0, 400, "%d%%", ImGuiSliderFlags_AlwaysClamp)) pf.explosion = e, changed = true;
  }
  if (st == ChartType::Waterfall) {
    ImGui::SeparatorText(TR("Series Options"));
    bool tot = std::find(se.totals.begin(), se.totals.end(), point) != se.totals.end();
    if (ImGui::Checkbox(TR("Set as total"), &tot)) {
      se.totals.erase(std::remove(se.totals.begin(), se.totals.end(), point), se.totals.end());
      if (tot) se.totals.push_back(point);
      std::sort(se.totals.begin(), se.totals.end());
      changed = true;
    }
  }
  ImGui::SeparatorText(TR("Fill & Line"));
  changed |= PickBgr(TR("Fill"), &pf.fill);
  changed |= PickBgr(TR("Border"), &pf.line);
  bool lab = pf.label >= 0 ? pf.label == 1 : se.data_labels;
  if (ImGui::Checkbox(TR("Data Label"), &lab)) pf.label = lab ? 1 : 0, changed = true;
  if (changed) {
    if (it != se.points.end()) *it = pf;
    else se.points.push_back(pf);
  }
  return changed;
}

// Format Data Labels: Label Contains (Value From Cells picked on the sheet), Separator, Label Position, Number
bool EditDataLabels(AppState& s, const ChartObj& m, model::ChartSeries& se, int index) {
  bool changed = false;
  const ChartType st = SeriesTypeOf(m, se);
  const bool pie = st == ChartType::Pie || st == ChartType::Doughnut;
  ImGui::SeparatorText(TR("Label Contains"));
  auto bit = [&](const char* label, int b) {
    bool on = (se.label_show & b) != 0;
    if (ImGui::Checkbox(label, &on)) {
      se.label_show = on ? (se.label_show | b) : (se.label_show & ~b);
      changed = true;
    }
    return on;
  };
  if (bit(TR("Value From Cells"), 32)) {
    std::string r = se.label_range.empty() ? std::string() : "=" + se.label_range;
    ImGui::TextUnformatted(TR("Data Label Range:"));
    if (RefEdit(s, ("chart.labelrange" + std::to_string(index)).c_str(), &r, ImGui::GetContentRegionAvail().x)) {
      while (!r.empty() && (r[0] == '=' || r[0] == ' ')) r.erase(0, 1);
      se.label_range = r;
      changed = true;
    }
  }
  bit(TR("Series Name"), 1);
  bit(TR("Category Name"), 2);
  bit(TR("Value"), 4);
  if (pie) bit(TR("Percentage"), 8);
  changed |= ImGui::Checkbox(TR("Show Leader Lines"), &se.leader_lines);
  bit(TR("Legend key"), 16);
  static const char* kSeps[] = {", ", "; ", ". ", "\n", " "};
  const char* sep_names[] = {", " , "; ", ". ", TR("(New Line)"), TR("(Space)")};
  int si = 0;
  for (int k = 0; k < 5; ++k) {
    if (se.label_sep == kSeps[k]) si = k;
  }
  ImGui::SetNextItemWidth(S(120));
  if (ImGui::Combo(TR("Separator"), &si, sep_names, 5)) se.label_sep = kSeps[si], changed = true;
  // Label Position: the places this kind of chart offers
  std::vector<std::pair<int, const char*>> places;
  if (engine::IsBarType(st)) {
    places = {{0, TR("Center")}, {1, TR("Inside End")}, {2, TR("Inside Base")}};
    if (!engine::IsStackedType(st)) places.push_back({3, TR("Outside End")});
  } else if (pie) {
    places = {{0, TR("Center")}, {1, TR("Inside End")}, {3, TR("Outside End")}, {8, TR("Best Fit")}};
  } else if (engine::IsLineType(st) || engine::IsScatterType(st)) {
    places = {{0, TR("Center")}, {4, TR("Left")}, {5, TR("Right")}, {6, TR("Above")}, {7, TR("Below")}};
  }
  if (!places.empty()) {
    ImGui::SeparatorText(TR("Label Position"));
    const int def = engine::IsBarType(st) ? (engine::IsStackedType(st) ? 0 : 3) : pie ? 8 : 5;
    const int cur = se.label_pos >= 0 ? se.label_pos : def;
    for (const auto& [v, name] : places) {
      if (ImGui::RadioButton(name, cur == v)) se.label_pos = v, changed = true;
    }
  }
  ImGui::SeparatorText(TR("Number"));
  bool linked = se.label_format.empty();
  if (ImGui::Checkbox(TR("Linked to source"), &linked)) {
    se.label_format = linked ? std::string() : std::string("#,##0.00");
    changed = true;
  }
  if (!linked) changed |= EditNumberFormat(se.label_format);
  ImGui::SeparatorText(TR("Fill & Line"));  // the labels' box
  changed |= PickColor(TR("Fill##label_box"), &se.label_fill);
  changed |= PickColor(TR("Border##label_box"), &se.label_line);
  ImGui::SeparatorText(TR("Text Options"));
  changed |= EditChartFont(se.label_font, 9);
  if (changed) se.data_labels = true;
  return changed;
}

// ---- Chart Elements (+) and Chart Filters buttons beside the selected chart (Excel's flyouts) ----

const ImGuiID kChartElemPopup = ImHashStr("##yc_chart_elements");
const ImGuiID kChartFilterPopup = ImHashStr("##yc_chart_filters");
const ImGuiID kChartStylePopup = ImHashStr("##yc_chart_styles");

void PostChartEdit(AppState& s, ObjSel o, const char* label, std::function<void(ChartObj&)> fn) {
  s.engine->PostUser(label, [o, fn = std::move(fn)](Document& d) { d.UpdateChart(o.sheet, o.id, fn); });
}

bool ElementsCartesian(ChartType t) { return CartesianChart(t) && !engine::IsSceneChart(t) && !engine::IsStockType(t); }

// the statistic charts draw axes and gridlines too (Excel's + lists them)
bool ElementsAxes(ChartType t) {
  using T = ChartType;
  return ElementsCartesian(t) || engine::IsStockType(t) || t == T::Histogram || t == T::Pareto || t == T::BoxWhisker || t == T::Waterfall || t == T::Funnel;
}

// one square button with an icon; true when pressed
bool FlyoutButton(const char* id, const char* icon, ImVec2 p, float sz, const char* tip, bool open) {
  ImGui::SetCursorScreenPos(p);
  ImGui::PushID(id);
  const bool press = ImGui::InvisibleButton("##fly", ImVec2(sz, sz));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, p + ImVec2(sz, sz), open ? IM_COL32(0xD5, 0xE6, 0xF7, 255) : hov ? IM_COL32(0xEB, 0xF3, 0xFC, 255) : IM_COL32_WHITE);
  dl->AddRect(p, p + ImVec2(sz, sz), IM_COL32(0xA6, 0xA6, 0xA6, 255));
  ImFont* f = ui::F().icons;
  const float fs = sz * 0.62f;
  const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, icon);
  dl->AddText(f, fs, p + ImVec2((sz - ts.x) * 0.5f, (sz - ts.y) * 0.5f), IM_COL32(0x40, 0x40, 0x40, 255), icon);
  if (hov) ImGui::SetTooltip("%s", tip);
  ImGui::PopID();
  return press;
}

// the + flyout: each element a check box, with its choices under its arrow and More Options... opening its Format pane
// Live preview of a + flyout choice under the mouse: the chart drawn with it (for this frame and the next)
struct ElemPreview {
  ObjSel o;
  ChartObj ch;
  int frame = -10;
};
ElemPreview& EP() {
  static ElemPreview p;
  return p;
}
void SetChartElemPreview(ObjSel o, ChartObj ch) {
  EP().o = o;
  EP().ch = std::move(ch);
  EP().frame = ImGui::GetFrameCount();
}
const ChartObj* ChartElemPreview(uint32_t sheet, uint32_t id) {
  const ElemPreview& p = EP();
  if (p.frame < ImGui::GetFrameCount() - 1 || p.o.sheet != sheet || p.o.id != id || p.o.kind != 2) return nullptr;
  return &p.ch;
}

void DrawChartElementsMenu(AppState& s, ObjSel o, const ChartObj& ch) {
  if (!ImGui::BeginPopupEx(kChartElemPopup, kPopupFlags)) return;
  using E = model::ChartElem;
  ImGui::TextDisabled("%s", TR("Chart Elements"));
  ImGui::Separator();
  const bool cart = ElementsCartesian(ch.type);
  const bool axes = ElementsAxes(ch.type);
  bool y2 = false;
  for (const auto& se : ch.series) y2 = y2 || se.axis_group == 1;
  auto more = [&](E e, int series = 0) {
    if (ImGui::MenuItem(TR("More Options..."))) {
      SelectElem(o, e, series);
      U().show_props = true;
      ImGui::CloseCurrentPopup();
    }
  };
  // a row: the check box, then its arrow (a submenu with Excel's choices)
  auto row = [&](const char* label, bool* v) {
    const bool c = ImGui::Checkbox(label, v);
    ImGui::SameLine(S(170));
    return c;
  };
  // a choice: clicked, it changes the chart; hovered, the chart shows it until the mouse leaves (Excel's live preview)
  auto act = [&](bool clicked, const char* label, std::function<void(ChartObj&)> fn, std::function<void(ChartObj&)> hover) {
    if (clicked) {
      PostChartEdit(s, o, label, fn);
    } else if (ImGui::IsItemHovered()) {
      ChartObj copy = ch;
      (hover ? hover : fn)(copy);
      SetChartElemPreview(o, std::move(copy));
    }
  };
  bool v = false;
  if (axes) {
    v = ch.cat_axis.visible && ch.val_axis.visible;
    if (const bool k_ = row(TR("Axes"), &v); true) act(k_, TR("Axes"), [v](ChartObj& c) { c.cat_axis.visible = c.val_axis.visible = v; }, nullptr);
    if (ImGui::BeginMenu("##axes_more")) {
      bool a = ch.cat_axis.visible;
      if (const bool k_ = ImGui::MenuItem(TR("Primary Horizontal"), nullptr, &a); true) act(k_, TR("Axes"), [a](ChartObj& c) { c.cat_axis.visible = a; }, [a = !a](ChartObj& c) { c.cat_axis.visible = a; });
      bool b = ch.val_axis.visible;
      if (const bool k_ = ImGui::MenuItem(TR("Primary Vertical"), nullptr, &b); true) act(k_, TR("Axes"), [b](ChartObj& c) { c.val_axis.visible = b; }, [b = !b](ChartObj& c) { c.val_axis.visible = b; });
      if (y2) {
        bool c2 = ch.val_axis2.visible;
        if (const bool k_ = ImGui::MenuItem(TR("Secondary Vertical"), nullptr, &c2); true) act(k_, TR("Axes"), [c2](ChartObj& c) { c.val_axis2.visible = c2; }, [c2 = !c2](ChartObj& c) { c.val_axis2.visible = c2; });
      }
      more(E::ValueAxis);
      ImGui::EndMenu();
    }
    v = ch.cat_axis.has_title && ch.val_axis.has_title;
    if (const bool k_ = row(TR("Axis Titles"), &v); true) act(k_, TR("Axis Titles"), [v](ChartObj& c) { c.cat_axis.has_title = c.val_axis.has_title = v; }, nullptr);
    if (ImGui::BeginMenu("##titles_more")) {
      bool a = ch.cat_axis.has_title;
      if (const bool k_ = ImGui::MenuItem(TR("Primary Horizontal"), nullptr, &a); true) act(k_, TR("Axis Titles"), [a](ChartObj& c) { c.cat_axis.has_title = a; }, [a = !a](ChartObj& c) { c.cat_axis.has_title = a; });
      bool b = ch.val_axis.has_title;
      if (const bool k_ = ImGui::MenuItem(TR("Primary Vertical"), nullptr, &b); true) act(k_, TR("Axis Titles"), [b](ChartObj& c) { c.val_axis.has_title = b; }, [b = !b](ChartObj& c) { c.val_axis.has_title = b; });
      if (y2) {
        bool c2 = ch.val_axis2.has_title;
        if (const bool k_ = ImGui::MenuItem(TR("Secondary Vertical"), nullptr, &c2); true) act(k_, TR("Axis Titles"), [c2](ChartObj& c) { c.val_axis2.has_title = c2; }, [c2 = !c2](ChartObj& c) { c.val_axis2.has_title = c2; });
      }
      more(E::ValAxisTitle);
      ImGui::EndMenu();
    }
  }
  v = ch.has_title;
  if (const bool k_ = row(TR("Chart Title"), &v); true) act(k_, TR("Chart Title"), [v](ChartObj& c) { c.has_title = v; }, nullptr);
  if (ImGui::BeginMenu("##title_more")) {
    if (const bool k_ = ImGui::MenuItem(TR("Above Chart"), nullptr, ch.has_title && !ch.title_overlay); true) act(k_, TR("Chart Title"), [](ChartObj& c) { c.has_title = true, c.title_overlay = false; }, nullptr);
    if (const bool k_ = ImGui::MenuItem(TR("Centered Overlay"), nullptr, ch.has_title && ch.title_overlay); true) act(k_, TR("Chart Title"), [](ChartObj& c) { c.has_title = true, c.title_overlay = true; }, nullptr);
    more(E::Title);
    ImGui::EndMenu();
  }
  bool labels = !ch.series.empty();
  for (const auto& se : ch.series) labels = labels && se.data_labels;
  v = labels;
  if (row(TR("Data Labels"), &v)) {
    PostChartEdit(s, o, TR("Data Labels"), [v](ChartObj& c) {
      for (auto& se : c.series) se.data_labels = v;
    });
  }
  if (ImGui::BeginMenu("##labels_more")) {
    // the positions this kind of chart offers (Excel's list)
    std::vector<std::pair<int, const char*>> places;
    const ChartType t = ch.type;
    if (engine::IsBarType(t)) {
      places = {{0, TR("Center")}, {1, TR("Inside End")}, {2, TR("Inside Base")}};
      if (!engine::IsStackedType(t)) places.push_back({3, TR("Outside End")});
    } else if (t == ChartType::Pie || t == ChartType::Doughnut) {
      places = {{0, TR("Center")}, {1, TR("Inside End")}, {3, TR("Outside End")}, {8, TR("Best Fit")}};
    } else if (engine::IsLineType(t) || engine::IsScatterType(t)) {
      places = {{0, TR("Center")}, {4, TR("Left")}, {5, TR("Right")}, {6, TR("Above")}, {7, TR("Below")}};
    }
    for (const auto& [p, name] : places) {
      const int pp = p;
      const bool k_ = ImGui::MenuItem(name);
      act(k_, TR("Data Labels"), [pp](ChartObj& c) {
        for (auto& se : c.series) se.data_labels = true, se.label_pos = pp;
      }, nullptr);
    }
    more(E::DataLabels, 0);
    ImGui::EndMenu();
  }
  if (cart) {
    bool err = !ch.series.empty();
    for (const auto& se : ch.series) err = err && se.err != model::ErrorBarKind::None;
    v = err;
    if (row(TR("Error Bars"), &v)) {
      PostChartEdit(s, o, TR("Error Bars"), [v](ChartObj& c) {
        for (auto& se : c.series) se.err = v ? model::ErrorBarKind::StError : model::ErrorBarKind::None;
      });
    }
    if (ImGui::BeginMenu("##err_more")) {
      const std::pair<model::ErrorBarKind, const char*> kinds[] = {{model::ErrorBarKind::StError, TR("Standard Error")}, {model::ErrorBarKind::Percent, TR("Percentage")},
                                                                   {model::ErrorBarKind::StDev, TR("Standard Deviation")}};
      for (const auto& [k, name] : kinds) {
        const model::ErrorBarKind kk = k;
        const bool k_ = ImGui::MenuItem(name);
        act(k_, TR("Error Bars"), [kk](ChartObj& c) {
          for (auto& se : c.series) {
            se.err = kk;
            se.err_amount = kk == model::ErrorBarKind::Percent ? 5 : 1;
          }
        }, nullptr);
      }
      more(E::ErrorBars, 0);
      ImGui::EndMenu();
    }
  }
  if (axes) {
    v = ch.val_axis.gridlines;
    if (const bool k_ = row(TR("Gridlines"), &v); true) act(k_, TR("Gridlines"), [v](ChartObj& c) { c.val_axis.gridlines = v; }, nullptr);
    if (ImGui::BeginMenu("##grid_more")) {
      bool a = ch.val_axis.gridlines, b = ch.cat_axis.gridlines, c1 = ch.val_axis.minor_gridlines, d1 = ch.cat_axis.minor_gridlines;
      if (const bool k_ = ImGui::MenuItem(TR("Primary Major Horizontal"), nullptr, &a); true) act(k_, TR("Gridlines"), [a](ChartObj& c) { c.val_axis.gridlines = a; }, [a = !a](ChartObj& c) { c.val_axis.gridlines = a; });
      if (const bool k_ = ImGui::MenuItem(TR("Primary Major Vertical"), nullptr, &b); true) act(k_, TR("Gridlines"), [b](ChartObj& c) { c.cat_axis.gridlines = b; }, [b = !b](ChartObj& c) { c.cat_axis.gridlines = b; });
      if (const bool k_ = ImGui::MenuItem(TR("Primary Minor Horizontal"), nullptr, &c1); true) act(k_, TR("Gridlines"), [c1](ChartObj& c) { c.val_axis.minor_gridlines = c1; }, [c1 = !c1](ChartObj& c) { c.val_axis.minor_gridlines = c1; });
      if (const bool k_ = ImGui::MenuItem(TR("Primary Minor Vertical"), nullptr, &d1); true) act(k_, TR("Gridlines"), [d1](ChartObj& c) { c.cat_axis.minor_gridlines = d1; }, [d1 = !d1](ChartObj& c) { c.cat_axis.minor_gridlines = d1; });
      more(E::MajorGridlines);
      ImGui::EndMenu();
    }
  }
  v = ch.has_legend;
  if (const bool k_ = row(TR("Legend"), &v); true) act(k_, TR("Legend"), [v](ChartObj& c) { c.has_legend = v; }, nullptr);
  if (ImGui::BeginMenu("##legend_more")) {
    static const int kOrder[] = {1, 2, 3, 0};  // Right, Top, Left, Bottom (Excel's list)
    for (int p : kOrder) {
      if (const bool k_ = ImGui::MenuItem(LegendPosName(p), nullptr, ch.has_legend && ch.legend_pos == p); true) act(k_, TR("Legend"), [p](ChartObj& c) { c.legend_pos = p, c.has_legend = true, c.legend_rect = {}; }, nullptr);
    }
    more(E::Legend);
    ImGui::EndMenu();
  }
  if (cart) {
    bool trend = !ch.series.empty();
    for (const auto& se : ch.series) trend = trend && se.trend != model::TrendKind::None;
    v = trend;
    if (row(TR("Trendline"), &v)) {
      PostChartEdit(s, o, TR("Trendline"), [v](ChartObj& c) {
        for (auto& se : c.series) se.trend = v ? model::TrendKind::Linear : model::TrendKind::None;
      });
    }
    if (ImGui::BeginMenu("##trend_more")) {
      const std::pair<model::TrendKind, const char*> kinds[] = {{model::TrendKind::Linear, TR("Linear")}, {model::TrendKind::Exponential, TR("Exponential")},
                                                                {model::TrendKind::Linear, TR("Linear Forecast")}, {model::TrendKind::MovingAverage, TR("Two Period Moving Average")}};
      for (size_t k = 0; k < 4; ++k) {
        const model::TrendKind kk = kinds[k].first;
        const bool forecast = k == 2;
        const bool k_ = ImGui::MenuItem(kinds[k].second);
        act(k_, TR("Trendline"), [kk, forecast](ChartObj& c) {
          for (auto& se : c.series) {
            se.trend = kk;
            se.trend_period = 2;
            se.trend_forward = forecast ? 2 : 0;
          }
        }, nullptr);
      }
      more(E::Trendline, 0);
      ImGui::EndMenu();
    }
  }
  if (engine::IsStockType(ch.type)) {  // a stock chart's own elements: Up/Down Bars (with an open) and High-Low Lines
    if (ch.type == ChartType::StockOHLC || ch.type == ChartType::StockVOHLC) {
      v = ch.up_down_bars;
      if (const bool k_ = row(TR("Up/Down Bars"), &v); true) act(k_, TR("Up/Down Bars"), [v](ChartObj& c) { c.up_down_bars = v; }, nullptr);
      if (ImGui::BeginMenu("##updown_more")) {
        more(E::UpBars);
        ImGui::EndMenu();
      }
    }
    v = ch.hi_lo_lines;
    if (const bool k_ = row(TR("High-Low Lines"), &v); true) act(k_, TR("High-Low Lines"), [v](ChartObj& c) { c.hi_lo_lines = v; }, nullptr);
    if (ImGui::BeginMenu("##hilo_more")) {
      more(E::HiLoLines);
      ImGui::EndMenu();
    }
  }
  ImGui::Separator();
  if (ImGui::Selectable(TR("More Options..."))) {
    SelectElem(o, model::ChartElem::ChartArea, 0);
    U().show_props = true;
  }
  ImGui::EndPopup();
}
void DrawChartFiltersMenu(AppState& s, ObjSel o, const ChartObj& ch) {
  if (!ImGui::BeginPopupEx(kChartFilterPopup, kPopupFlags)) return;
  // every series and category, filtered or not
  ChartObj all = ch;
  all.hidden_cats.clear();
  for (auto& se : all.series) se.filtered = false;
  engine::ChartValues v;
  ResolveChartValues(*s.snap, o.sheet, all, &v);
  ImGui::TextDisabled("%s", TR("Values"));
  ImGui::Separator();
  ImGui::TextUnformatted(TR("SERIES"));
  for (size_t i = 0; i < ch.series.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    bool on = !ch.series[i].filtered;
    const std::string name = i < v.series.size() ? v.series[i].name : TR("Series") + std::to_string(i + 1);
    if (ImGui::Checkbox(name.c_str(), &on)) {
      PostChartEdit(s, o, TR("Chart Filters"), [i, on](ChartObj& c) {
        if (i < c.series.size()) c.series[i].filtered = !on;
      });
    }
    ImGui::PopID();
  }
  const bool cats = !v.cats.empty() && !engine::IsScatterType(ch.type);
  if (cats) {
    ImGui::Spacing();
    ImGui::TextUnformatted(TR("CATEGORIES"));
    bool all_on = ch.hidden_cats.empty();
    if (CheckboxMixed(TR("(Select All)"), &all_on, !all_on && ch.hidden_cats.size() < v.cats.size())) {
      const int n = static_cast<int>(v.cats.size());
      PostChartEdit(s, o, TR("Chart Filters"), [all_on, n](ChartObj& c) {
        c.hidden_cats.clear();
        if (!all_on) {
          for (int k = 0; k < n; ++k) c.hidden_cats.push_back(k);
        }
      });
    }
    const size_t n = std::min<size_t>(v.cats.size(), 300);
    ImGui::BeginChild("##cats", ImVec2(S(220), std::min(S(260), static_cast<float>(n) * ImGui::GetFrameHeightWithSpacing() + S(4))));
    for (size_t k = 0; k < n; ++k) {
      ImGui::PushID(static_cast<int>(1000 + k));
      bool on = !std::binary_search(ch.hidden_cats.begin(), ch.hidden_cats.end(), static_cast<int>(k));
      if (ImGui::Checkbox(v.cats[k].empty() ? " " : v.cats[k].c_str(), &on)) {
        const int kk = static_cast<int>(k);
        PostChartEdit(s, o, TR("Chart Filters"), [kk, on](ChartObj& c) {
          c.hidden_cats.erase(std::remove(c.hidden_cats.begin(), c.hidden_cats.end(), kk), c.hidden_cats.end());
          if (!on) c.hidden_cats.push_back(kk);
          std::sort(c.hidden_cats.begin(), c.hidden_cats.end());
        });
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
  }
  ImGui::Separator();
  if (ImGui::Selectable(TR("Select Data..."))) OpenSelectData(s, o.sheet, o.id);
  ImGui::EndPopup();
}

// the buttons outside the chart's top right corner (inside the window: at the left side when there is no room)
}  // namespace

void DrawChartPresetPicture(ImDrawList* dl, ImVec2 a, ImVec2 b, int kind, int n, ChartType type);
bool DrawChartTypePictogram(ImDrawList* dl, ImVec2 a, float sz, ChartType t, int combo);

namespace {

// the Chart Styles flyout (the brush beside a selected chart): Style, the chart type's set of looks, and Color, the
// colorful and monochromatic palettes; each choice under the mouse is previewed on the chart, a click applies it
bool& StylesFlyoutOpening() {  // the flyout opens on its Style tab, as Excel's does each time
  static bool v = false;
  return v;
}

void DrawChartStylesMenu(AppState& s, ObjSel o, const ChartObj& ch) {
  if (!ImGui::BeginPopupEx(kChartStylePopup, kPopupFlags)) return;
  if (BeginDlgTabBar("##chart_style_tabs")) {
    const bool opening = StylesFlyoutOpening();
    StylesFlyoutOpening() = false;
    if (ImGui::BeginTabItem(TR("Style"), nullptr, opening ? ImGuiTabItemFlags_SetSelected : 0)) {
      const int count = engine::ChartStyleCount(ch.type);
      if (opening) ImGui::SetNextWindowScroll(ImVec2(0, 0));  // from Style 1 each time it opens
      ImGui::BeginChild("##chart_style_list", ImVec2(S(250), std::min(S(430), static_cast<float>(count) * S(62))));
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (int k = 1; k <= count; ++k) {
        ImGui::PushID(k);
        char label[48];
        std::snprintf(label, sizeof label, TR("Style %d"), k);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(label, std::max(1, ch.style) == k, 0, ImVec2(S(230), S(58)))) {
          PostChartEdit(s, o, TR("Chart Style"), [k](ChartObj& c) { engine::ApplyChartStyle(c, k); });
          ImGui::CloseCurrentPopup();
        } else if (ImGui::IsItemHovered()) {
          ChartObj copy = ch;
          engine::ApplyChartStyle(copy, k);
          SetChartElemPreview(o, std::move(copy));
          ImGui::SetTooltip("%s", engine::ChartStyleTip(ch.type, k).c_str());
        }
        DrawChartPresetPicture(dl, ImVec2(p.x + S(70), p.y + S(3)), ImVec2(p.x + S(70) + S(78), p.y + S(55)), 2, k, ch.type);
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Color"))) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      for (int k = 0; k < engine::kChartColorSchemes; ++k) {
        if (k == 0 || k == 4) ImGui::SeparatorText(k == 0 ? TR("Colorful") : TR("Monochromatic"));
        ImGui::PushID(100 + k);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(engine::ChartColorSchemeName(k), ch.color_scheme == k, 0, ImVec2(S(250), S(22)))) {
          PostChartEdit(s, o, TR("Change Colors"), [k](ChartObj& c) { c.color_scheme = k; });
          ImGui::CloseCurrentPopup();
        } else if (ImGui::IsItemHovered()) {
          ChartObj copy = ch;
          copy.color_scheme = k;
          SetChartElemPreview(o, std::move(copy));
        }
        const engine::PaletteScope pal(k);  // the palette's six colors at the right
        for (int i = 0; i < 6; ++i) {
          const ImVec2 q(p.x + S(160) + static_cast<float>(i) * S(14), p.y + S(5));
          dl->AddRectFilled(q, q + ImVec2(S(12), S(12)), engine::ChartPalette(static_cast<size_t>(i)));
        }
        ImGui::PopID();
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndPopup();
}

void DrawChartFlyouts(AppState& s, ObjSel o, const ChartObj& ch, const ImRect& r, const ImRect& cells) {
  const float sz = std::round(S(24)), gap = std::round(S(5));
  float x = r.Max.x + gap;
  if (x + sz > cells.Max.x) x = r.Min.x - gap - sz;
  if (x < cells.Min.x) x = r.Max.x - sz - gap;
  const float y0 = std::max(r.Min.y, cells.Min.y);
  const bool el_open = ImGui::IsPopupOpen(kChartElemPopup, ImGuiPopupFlags_None);
  const bool fi_open = ImGui::IsPopupOpen(kChartFilterPopup, ImGuiPopupFlags_None);
  const bool st_open = ImGui::IsPopupOpen(kChartStylePopup, ImGuiPopupFlags_None);
  // (each menu is drawn right after its button: a closed popup's Begin clears the next window's position)
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 1.0f);  // the menus in the UI's font, not the sheet's zoom
  if (FlyoutButton("elements", ui::icon::Add, ImVec2(x, y0), sz, TR("Chart Elements: add, remove or change the title, legend, axes, labels..."), el_open)) {
    ImGui::SetNextWindowPos(ImVec2(x < r.Min.x ? x - S(200) : x + sz + gap, y0));
    ImGui::OpenPopupEx(kChartElemPopup);
  }
  DrawChartElementsMenu(s, o, ch);
  // Chart Styles (Excel's brush), between + and the filter
  if (FlyoutButton("styles", ui::icon::FormatPainter, ImVec2(x, y0 + sz + gap), sz, TR("Chart Styles: set a style and color scheme for your chart"), st_open)) {
    ImGui::SetNextWindowPos(ImVec2(x < r.Min.x ? x - S(270) : x + sz + gap, y0 + sz + gap));
    ImGui::OpenPopupEx(kChartStylePopup);
    StylesFlyoutOpening() = true;
  }
  DrawChartStylesMenu(s, o, ch);
  if (FlyoutButton("filters", ui::icon::Filter, ImVec2(x, y0 + 2 * (sz + gap)), sz, TR("Chart Filters: which series and categories the chart shows"), fi_open)) {
    ImGui::SetNextWindowPos(ImVec2(x < r.Min.x ? x - S(240) : x + sz + gap, y0 + 2 * (sz + gap)));
    ImGui::OpenPopupEx(kChartFilterPopup);
  }
  DrawChartFiltersMenu(s, o, ch);
  ImGui::PopFont();
}

}  // namespace

bool NumberFormatPicker(std::string& code) { return EditNumberFormat(code); }

// the chart's elements as Excel's Chart Elements box lists them: every element present, each series by its name with
// its labels, trendline and error bars
std::vector<std::pair<model::ChartElem, int>> ChartElemList(const ChartObj& m) {
  using E = model::ChartElem;
  const bool axes = CartesianChart(m.type);
  bool y2 = false;
  for (const auto& se : m.series) y2 = y2 || se.axis_group == 1;
  std::vector<std::pair<E, int>> items = {{E::ChartArea, 0}, {E::PlotArea, 0}};
  if (m.has_title) items.push_back({E::Title, 0});
  if (m.has_legend) items.push_back({E::Legend, 0});
  if (axes) {
    items.push_back({E::CategoryAxis, 0});
    items.push_back({E::ValueAxis, 0});
    if (y2) items.push_back({E::ValueAxis2, 0});
    if (m.cat_axis.has_title) items.push_back({E::CatAxisTitle, 0});
    if (m.val_axis.has_title) items.push_back({E::ValAxisTitle, 0});
    if (y2 && m.val_axis2.has_title) items.push_back({E::ValAxis2Title, 0});
    if (m.val_axis.gridlines) items.push_back({E::MajorGridlines, 0});
    if (m.val_axis.minor_gridlines) items.push_back({E::MinorGridlines, 0});
  }
  for (size_t i = 0; i < m.series.size(); ++i) {
    items.push_back({E::Series, static_cast<int>(i)});
    if (m.series[i].data_labels) items.push_back({E::DataLabels, static_cast<int>(i)});
    if (m.series[i].trend != model::TrendKind::None) items.push_back({E::Trendline, static_cast<int>(i)});
    if (m.series[i].err != model::ErrorBarKind::None) items.push_back({E::ErrorBars, static_cast<int>(i)});
  }
  return items;
}

// the Chart Elements box (Format pane, Format > Current Selection): the selected element by name; choosing one selects it
void ChartElemCombo(ObjSel o, const ChartObj& m, model::ChartElem el, int es, int ep, const char* id, float width) {
  using E = model::ChartElem;
  ImGui::SetNextItemWidth(width);
  if (ImGui::BeginCombo(id, ChartElemName(m, el, es, ep).c_str(), ImGuiComboFlags_HeightLarge)) {
    const auto items = ChartElemList(m);
    for (size_t k = 0; k < items.size(); ++k) {
      ImGui::PushID(static_cast<int>(k));
      const auto [e, i] = items[k];
      const bool per_series = e == E::Series || e == E::DataLabels || e == E::Trendline || e == E::ErrorBars;
      if (ImGui::Selectable(ChartElemName(m, e, i).c_str(), e == el && (!per_series || i == es))) SelectElem(o, e, i);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
}

void DrawPropertiesWindow(AppState& s) {
  DrawFormatControlDialog(s);
  UiState& u = U();
  if (!u.show_props) return;
  const model::Sheet* sh = u.sel ? s.snap->Sheet(u.sel.sheet) : nullptr;
  const Control* c = nullptr;
  const ChartObj* ch = nullptr;
  const ShapeObj* shape = sh && u.sel.kind == 3 ? FindShape(*sh, u.sel.id) : nullptr;
  if (sh) {
    for (const auto& x : *sh->controls) {
      if (u.sel.kind == 1 && x.id == u.sel.id) c = &x;
    }
    for (const auto& x : *sh->charts) {
      if (u.sel.kind == 2 && x.id == u.sel.id) ch = &x;
    }
  }
  const ObjSel o = u.sel;
  // a form control has Excel's Format Control dialog, not a pane
  if (c && c->form) {
    u.show_props = false;
    OpenFormatControl(s, o);
    return;
  }
  // charts: the Format pane of the selected element (Format Chart Area, Format Legend...)
  std::string wtitle = TR("Properties");
  if (shape) wtitle = shape->kind == ShapeKind::Picture ? TR("Format Picture") : TR("Format Shape");
  if (ch) {
    int es0 = 0, ep0 = -1;
    const model::ChartElem e0 = ElemOf(o, &es0, &ep0);
    wtitle = FormatPaneName(*ch, e0, es0, ep0);
  }
  wtitle += "###yc_props";
  ui::NoteWindowTitle(wtitle);  // ui.windows() shows the title of now (Format Shape after Format Chart Area)
  ImGui::SetNextWindowSize(ImVec2(S(300), S(460)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Right);
  DockAsTaskPane(wtitle.c_str(), false, S(300));  // Excel's Format pane is on the right of the sheets
  if (!ImGui::Begin(wtitle.c_str(), &u.show_props)) {
    ImGui::End();
    return;
  }
  if (shape) {
    ShapeObj m = *shape;
    ImGui::TextDisabled("%s", m.name.c_str());
    // a locked shape of a protected sheet can't be formatted (Excel greys the pane out)
    const bool prot = sh->protection->on && !sh->protection->objects && shape->locked;
    if (prot) ImGui::BeginDisabled();
    if (DrawFormatShape(m) && !prot) {
      FitShapeToText(m);
      s.engine->PostUser(TR("Format Shape"), [o, m](Document& d) { d.UpdateShape(o.sheet, o.id, [&](ShapeObj& x) { x = m; }); });
    }
    if (prot) ImGui::EndDisabled();
  } else if (c) {
    Control m = *c;
    bool changed = false;
    using K = ControlKind;
    ImGui::TextDisabled(TR("%s (%s)"), m.name.c_str(), m.form ? model::FormControlTypeName(m.kind) : model::ControlTypeName(m.kind));
    changed |= DrawActiveXGrid(s, m);
    if (changed) {
      s.engine->PostUser(TR("Change Properties"), [o, m](Document& d) { d.UpdateControl(o.sheet, o.id, [&](Control& x) { x = m; }); });
    }
    ImGui::Separator();
    const bool click = m.kind == K::Button || m.kind == K::Label || m.kind == K::Image || m.kind == K::CheckBox || m.kind == K::OptionButton || m.kind == K::ToggleButton;
    ImGui::TextWrapped(TR("Event: in sheets\\%s.py, define %s_%s()."), sh->code_name.c_str(), m.name.c_str(), click ? "Click" : "Change");
  } else if (ch) {
    ChartObj m = *ch;
    bool changed = false;
    using E = model::ChartElem;
    int es = 0, ep = -1;
    E el = ElemOf(o, &es, &ep);
    const bool series_elem = el == E::Series || el == E::Point || el == E::DataLabels || el == E::Trendline || el == E::ErrorBars || el == E::DataLabel;
    if (series_elem && (es < 0 || static_cast<size_t>(es) >= m.series.size())) el = E::ChartArea;
    const bool axes = CartesianChart(m.type);
    if (!axes && (el == E::CategoryAxis || el == E::ValueAxis || el == E::ValueAxis2 || el == E::MajorGridlines || el == E::MinorGridlines)) el = E::ChartArea;
    // Chart Elements: what the pane formats (also chosen by clicking the chart)
    ChartElemCombo(o, m, el, es, ep, "##chart_elem", -1);
    ImGui::Separator();
    int remove = -1;
    const ChartCache& ccache = CacheFor(s, o.sheet, *ch);
    const engine::ChartValues* vals = &ccache.v;
    const ChartCache::Hit& hh = ccache.hit;
    // histogram bins (Excel: Format Axis > Bins on the horizontal axis)
    auto bins = [&]() {
      if (m.type != ChartType::Histogram && m.type != ChartType::Pareto) return;
      ImGui::SeparatorText(TR("Bins"));
      const int mode = m.bins_by_category ? 3 : m.bin_width > 0 ? 1 : m.bin_count > 0 ? 2 : 0;
      if (ImGui::RadioButton(TR("By Category"), mode == 3)) {
        m.bins_by_category = true;
        changed = true;
      }
      if (ImGui::RadioButton(TR("Automatic##bins"), mode == 0)) {
        m.bin_width = 0;
        m.bin_count = 0;
        m.bins_by_category = false;
        changed = true;
      }
      if (ImGui::RadioButton(TR("Bin width"), mode == 1) && mode != 1) {
        m.bin_count = 0;
        m.bin_width = 1;
        m.bins_by_category = false;
        changed = true;
      }
      if (mode == 1) {
        ImGui::SameLine();
        if (EditNum("##binw", m.bin_width, 1, "%.4g")) {
          m.bin_width = std::max(1e-9, m.bin_width);
          changed = true;
        }
      }
      if (ImGui::RadioButton(TR("Number of bins"), mode == 2) && mode != 2) {
        m.bin_width = 0;
        m.bin_count = 10;
        m.bins_by_category = false;
        changed = true;
      }
      if (mode == 2) {
        ImGui::SameLine();
        changed |= SpinInt("##binn", m.bin_count, 1, 1000);
      }
      // everything above / at or below a value in one bin of its own
      changed |= ImGui::Checkbox(TR("Overflow bin"), &m.bins_overflow);
      if (m.bins_overflow) {
        ImGui::SameLine();
        changed |= EditNum("##overflow", m.overflow_at, 1, "%.6g");
      }
      changed |= ImGui::Checkbox(TR("Underflow bin"), &m.bins_underflow);
      if (m.bins_underflow) {
        ImGui::SameLine();
        changed |= EditNum("##underflow", m.underflow_at, 1, "%.6g");
      }
    };
    // a title's text: typed, or linked to a cell picked on the sheet (Excel's =Sheet1!$A$1 in the formula bar)
    auto title_text = [&](const char* key, std::string& t) {
      const bool linked = !t.empty() && t[0] == '=';
      ImGui::SeparatorText(TR("Text"));
      if (!linked) {
        changed |= EditText(TR("Title text"), t);
      } else {
        ImGui::TextDisabled("%s", TR("The title shows the linked cell's text."));
      }
      ImGui::TextUnformatted(TR("Link to cell:"));
      std::string link = linked ? t : std::string();
      if (RefEdit(s, key, &link, ImGui::GetContentRegionAvail().x)) {
        while (!link.empty() && link[0] == ' ') link.erase(0, 1);
        if (!link.empty() && link[0] != '=') link = "=" + link;
        t = link.size() > 1 ? link : std::string();
        changed = true;
      }
    };
    auto fill_border = [&](uint32_t& fill, uint32_t& line) {
      ImGui::SeparatorText(TR("Fill & Border"));
      changed |= PickColor(TR("Fill"), &fill);
      changed |= PickColor(TR("Border"), &line);
    };
    switch (el) {
      case E::Title:
        changed |= ImGui::Checkbox(TR("Show##chart_title"), &m.has_title);
        title_text("chart.titlelink", m.title);
        ImGui::SeparatorText(TR("Title Options"));
        if (ImGui::RadioButton(TR("Above Chart"), !m.title_overlay)) m.title_overlay = false, changed = true;
        if (ImGui::RadioButton(TR("Centered Overlay"), m.title_overlay)) m.title_overlay = true, changed = true;
        if (m.title_rect.on && ImGui::Button(TR("Reset Position##title"))) m.title_rect = {}, changed = true;
        fill_border(m.title_fill, m.title_line);
        ImGui::SeparatorText(TR("Font"));
        changed |= EditChartFont(m.title_font, 14);
        break;
      case E::CatAxisTitle:
      case E::ValAxisTitle:
      case E::ValAxis2Title: {
        model::ChartAxisFormat& a = el == E::CatAxisTitle ? m.cat_axis : el == E::ValAxisTitle ? m.val_axis : m.val_axis2;
        changed |= ImGui::Checkbox(TR("Show##axis_title"), &a.has_title);
        title_text(el == E::CatAxisTitle ? "chart.cattitlelink" : el == E::ValAxisTitle ? "chart.valtitlelink" : "chart.val2titlelink", a.title);
        ImGui::SeparatorText(TR("Font"));
        model::ChartFont tf = AxisTitleFontOf(m, a);
        if (EditChartFont(tf, 10)) a.title_font = tf, changed = true;
        break;
      }
      case E::Legend: {
        changed |= ImGui::Checkbox(TR("Show##chart_legend"), &m.has_legend);
        ImGui::SeparatorText(TR("Legend Position"));
        static const int kOrder[] = {2, 3, 1, 0, 4};  // Top, Left, Right, Bottom, Top Right (Excel's list)
        for (int p : kOrder) {
          if (ImGui::RadioButton(LegendPosName(p), m.legend_pos == p)) {
            m.legend_pos = p;
            m.has_legend = true;
            m.legend_rect = {};
            changed = true;
          }
        }
        bool room = !m.legend_overlay;
        if (ImGui::Checkbox(TR("Show the legend without overlapping the chart"), &room)) m.legend_overlay = !room, changed = true;
        fill_border(m.legend_fill, m.legend_line);
        ImGui::SeparatorText(TR("Font"));
        changed |= EditChartFont(m.legend_font, 9);
        break;
      }
      case E::CategoryAxis:
      case E::ValueAxis:
      case E::ValueAxis2: {
        const bool scatter_x = el == E::CategoryAxis && engine::IsScatterType(m.type);
        const bool value_axis = el != E::CategoryAxis || scatter_x;
        model::ChartAxisFormat& a = el == E::CategoryAxis ? m.cat_axis : el == E::ValueAxis ? m.val_axis : m.val_axis2;
        const int k = el == E::ValueAxis ? 0 : el == E::ValueAxis2 ? 1 : 2;
        changed |= EditAxisFormat(a, value_axis, hh.axis_lo[k], hh.axis_hi[k], hh.axis_step[k]);
        if (el == E::CategoryAxis && !value_axis && !engine::IsSceneChart(m.type)) changed |= EditDateAxis(m, vals);
        if (el == E::CategoryAxis) bins();
        ImGui::SeparatorText(TR("Text Options"));
        model::ChartFont af = AxisFontOf(m, a);  // this axis' own font (Excel: each axis has its own)
        if (EditChartFont(af, 9)) a.font = af, a.own_font = true, changed = true;
        break;
      }
      case E::MajorGridlines:
      case E::MinorGridlines:
        changed |= ImGui::Checkbox(TR("Major Gridlines"), &m.val_axis.gridlines);
        changed |= ImGui::Checkbox(TR("Minor Gridlines"), &m.val_axis.minor_gridlines);
        break;
      case E::LegendEntry: {
        // Excel's Format Legend Entry: the entry's own font; Delete takes the entry out of the legend
        ImGui::TextDisabled("%s", TR("Press Delete to remove this entry from the legend."));
        ImGui::SeparatorText(TR("Text Options"));
        const int cs = es >= 0 ? -1 : CategoryEntrySeries(m);
        if (es >= 0 && static_cast<size_t>(es) < m.series.size()) {
          model::ChartSeries& se = m.series[static_cast<size_t>(es)];
          model::ChartFont f = se.legend_entry_font_set ? se.legend_entry_font : m.legend_font;
          if (EditChartFont(f, 9)) se.legend_entry_font = f, se.legend_entry_font_set = true, changed = true;
        } else if (cs >= 0 && ep >= 0) {
          model::ChartPointFormat& pf = PointFormatRef(m.series[static_cast<size_t>(cs)], ep);
          model::ChartFont f = pf.legend_font_set ? pf.legend_font : m.legend_font;
          if (EditChartFont(f, 9)) pf.legend_font = f, pf.legend_font_set = true, changed = true;
        } else {
          changed |= EditChartFont(m.legend_font, 9);
        }
        break;
      }
      case E::DataLabel: {
        // one label (Format Data Label): shown or not, its own text (typed, or a cell's), its own Label Options and
        // font over the series', put back in its place after a drag
        model::ChartSeries& se = m.series[static_cast<size_t>(es)];
        if (ep < 0) break;
        model::ChartPointFormat& pf = PointFormatRef(se, ep);
        bool on = pf.label >= 0 ? pf.label == 1 : se.data_labels;
        if (ImGui::Checkbox(TR("Data Label"), &on)) pf.label = on ? 1 : 0, changed = true;
        ImGui::SeparatorText(TR("Label Text"));
        const bool linked = !pf.label_text.empty() && pf.label_text[0] == '=';
        if (!linked) {
          std::string t = pf.label_text;
          if (EditText(TR("Text"), t)) pf.label_text = t, pf.label = 1, changed = true;
        }
        ImGui::TextUnformatted(TR("Link to cell:"));
        std::string link = linked ? pf.label_text : std::string();
        if (RefEdit(s, ("chart.labellink" + std::to_string(es)).c_str(), &link, ImGui::GetContentRegionAvail().x)) {
          while (!link.empty() && link[0] == ' ') link.erase(0, 1);
          if (!link.empty() && link[0] != '=') link = "=" + link;
          pf.label_text = link.size() > 1 ? link : std::string();
          pf.label = 1;
          changed = true;
        }
        if (!pf.label_text.empty() && ImGui::Button(TR("Reset Label Text"))) pf.label_text.clear(), changed = true;
        if ((pf.label_dx != 0 || pf.label_dy != 0) && ImGui::Button(TR("Reset Label Position"))) {
          pf.label_dx = pf.label_dy = 0;
          changed = true;
        }
        // its own options: edited on a copy of the series that has this label's, written back to the point
        model::ChartSeries own = se;
        own.label_show = pf.label_show >= 0 ? pf.label_show : se.label_show;
        own.label_pos = pf.label_pos >= 0 ? pf.label_pos : se.label_pos;
        own.label_font = pf.label_font_set ? pf.label_font : se.label_font;
        own.label_fill = pf.label_fill ? pf.label_fill : se.label_fill;
        own.label_line = pf.label_line ? pf.label_line : se.label_line;
        if (EditDataLabels(s, m, own, es)) {
          if (own.label_fill != (pf.label_fill ? pf.label_fill : se.label_fill)) pf.label_fill = own.label_fill;
          if (own.label_line != (pf.label_line ? pf.label_line : se.label_line)) pf.label_line = own.label_line;
          if (own.label_show != (pf.label_show >= 0 ? pf.label_show : se.label_show)) pf.label_show = own.label_show;
          if (own.label_pos != (pf.label_pos >= 0 ? pf.label_pos : se.label_pos)) pf.label_pos = own.label_pos;
          if (!(own.label_font == (pf.label_font_set ? pf.label_font : se.label_font))) pf.label_font = own.label_font, pf.label_font_set = true;
          // (separator, number format, cells and leader lines are the series')
          se.label_sep = own.label_sep;
          se.label_format = own.label_format;
          se.label_range = own.label_range;
          se.leader_lines = own.leader_lines;
          pf.label = 1;
          changed = true;
        }
        break;
      }
      case E::PlotArea:
        changed |= PickColor(TR("Fill"), &m.plot_fill);
        changed |= PickColor(TR("Border"), &m.plot_line);
        if (m.plot_rect.on && ImGui::Button(TR("Reset Position##plot"))) m.plot_rect = {}, changed = true;
        break;
      case E::Series:
        changed |= EditSeriesOptions(m, m.series[static_cast<size_t>(es)]);
        changed |= EditChartTypeOptions(m, m.series[static_cast<size_t>(es)], vals);
        changed |= EditSeriesBlock(s, o, m, m.series[static_cast<size_t>(es)], vals, es);
        ImGui::Separator();
        if (ImGui::Button(TR("Remove Series"))) remove = es;
        break;
      case E::Point:
        changed |= EditPointFormat(m, m.series[static_cast<size_t>(es)], std::max(0, ep));
        break;
      case E::DataLabels:
        changed |= EditDataLabels(s, m, m.series[static_cast<size_t>(es)], es);
        break;
      case E::Trendline:
        changed |= EditTrendline(m.series[static_cast<size_t>(es)], std::max(0, ep));
        break;
      case E::ErrorBars:
        changed |= EditErrorBars(s, m.series[static_cast<size_t>(es)], es, ep == 1 && engine::IsScatterType(m.type));
        break;
      case E::UpBars:
      case E::DownBars:
      case E::HiLoLines:
      case E::Floor:
      case E::Walls:
        changed |= EditChartElemFill(m, el);
        break;
      default: {
        ImGui::SeparatorText(TR("Chart"));
        changed |= EditText(TR("Name"), m.name);
        const char* const* kNames = ChartTypeNames();
        int t = static_cast<int>(m.type);
        ImGui::TextUnformatted(TR("Chart Type"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##type", &t, kNames, model::kChartTypeCount)) {
          const ChartType nt = static_cast<ChartType>(t);
          s.engine->PostUser(TR("Change Chart Type"), [o, nt](Document& d) { d.UpdateChart(o.sheet, o.id, [&d, nt](ChartObj& x) { ChangeChartType(d.wb(), x, nt); }); });
        }
        changed |= ImGui::Checkbox(TR("Chart Title"), &m.has_title);
        changed |= ImGui::Checkbox(TR("Legend"), &m.has_legend);
        changed |= ImGui::Checkbox(TR("Interactive (pan / zoom)"), &m.interactive);
        changed |= ImGui::Checkbox(TR("Own Window"), &m.floating);
        // the series: click one to format it; their ranges are chosen with Select Data
        ImGui::SeparatorText(TR("Series"));
        for (size_t i = 0; i < m.series.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          const std::string nm = i < vals->series.size() ? vals->series[i].name : TR("Series") + std::to_string(i + 1);
          if (ImGui::Selectable(nm.c_str(), false)) SelectElem(o, E::Series, static_cast<int>(i));
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", TR("Format this series"));
          ImGui::PopID();
        }
        if (m.series.empty()) ImGui::TextDisabled("%s", TR("(no data yet)"));
        if (ImGui::Button(TR("Select Data..."))) OpenSelectData(s, o.sheet, o.id);
        bins();
        ImGui::SeparatorText(TR("Fill & Border"));
        changed |= PickColor(TR("Fill"), &m.chart_fill);
        changed |= ImGui::Checkbox(TR("No border"), &m.chart_line_none);
        if (!m.chart_line_none) changed |= PickColor(TR("Border"), &m.chart_line);
        if (model::Is3DChart(m.type) && m.type != ChartType::SurfaceContour) {
          ImGui::SeparatorText(TR("3-D Rotation"));
          if (!m.view3d) {  // the type's defaults, shown as the starting values
            m.rotation = m.type == ChartType::Pie3D ? 0 : 20;
            m.elevation = m.type == ChartType::Pie3D ? 30 : 15;
            m.perspective = m.type == ChartType::Pie3D ? 0 : 15;
            m.depth_percent = 100;
          }
          auto num = [&](const char* label, double* v, double step, double lo, double hi) {
            if (EditNum(label, *v, step, "%.0f")) {
              *v = std::clamp(*v, lo, hi);
              m.view3d = true;
              changed = true;
            }
          };
          changed |= Pick3DPreset(m);
          num(TR("X Rotation"), &m.rotation, 10, -360, 360);
          num(TR("Y Rotation"), &m.elevation, 5, -90, 90);
          num(TR("Perspective"), &m.perspective, 5, 0, 100);
          num(TR("Depth (percent of base)"), &m.depth_percent, 10, 20, 2000);
        }
        ImGui::SeparatorText(TR("Size"));
        const double h0 = m.height, w0 = m.width;
        if (EditNum(TR("Height (pt)"), m.height, 6, "%.1f")) {
          m.height = std::max(24.0, m.height);
          if (m.lock_aspect && h0 > 0) m.width = std::max(36.0, w0 * m.height / h0);
          changed = true;
        }
        if (EditNum(TR("Width (pt)"), m.width, 6, "%.1f")) {
          m.width = std::max(36.0, m.width);
          if (m.lock_aspect && w0 > 0) m.height = std::max(24.0, h0 * m.width / w0);
          changed = true;
        }
        changed |= ImGui::Checkbox(TR("Lock aspect ratio"), &m.lock_aspect);
        ImGui::SeparatorText(TR("Properties"));
        if (ImGui::RadioButton(TR("Move and size with cells"), m.placement == Placement::MoveAndSize)) m.placement = Placement::MoveAndSize, changed = true;
        if (ImGui::RadioButton(TR("Move but don't size with cells"), m.placement == Placement::Move)) m.placement = Placement::Move, changed = true;
        if (ImGui::RadioButton(TR("Don't move or size with cells"), m.placement == Placement::FreeFloating)) m.placement = Placement::FreeFloating, changed = true;
        changed |= ImGui::Checkbox(TR("Print object"), &m.print_object);
        changed |= ImGui::Checkbox(TR("Locked"), &m.locked);
        break;
      }
    }
    if (remove >= 0 && static_cast<size_t>(remove) < m.series.size()) {
      m.series.erase(m.series.begin() + remove);
      changed = true;
      SelectElem(o, E::ChartArea, 0);
    }
    if (changed) s.engine->PostUser(TR("Change Chart"), [o, m](Document& d) { d.UpdateChart(o.sheet, o.id, [&](ChartObj& x) { x = m; }); });
  } else {
    ImGui::TextWrapped(TR("Select a control or chart to show its properties. In Design Mode, click a control to select it."));
  }
  ImGui::End();
}

// ---- Charts and custom panels in their own windows ----

namespace {

int DockDir(const std::string& d) {
  if (d == "left") return 0;
  if (d == "right") return 1;
  if (d == "top") return 2;
  if (d == "bottom") return 3;
  if (d == "tab") return 4;
  return -1;
}

void PanelEvent(AppState& s, const model::Panel& p, const model::PanelControl& pc, const char* event) {
  if (HostReady(s)) s.python->ControlEvent(0, p.name + "." + pc.name, event, pc.on_action, true);
}

void PanelValue(AppState& s, const model::Panel& p, const model::PanelControl& pc, const std::string& value) {
  const std::string pn = p.name, cn = pc.name, linked = pc.linked_cell;
  const uint32_t sheet = s.focus_sheet;
  // a control linked to a parameter cell (yc.safety.parameter) changes it like typing does: operator mode, type and
  // range, confirmation, the change log; a refused value leaves the cell (and so the control) as it was
  bool param = false;
  {
    uint32_t sid = 0;
    Rect r;
    if (!linked.empty() && s.snap && ResolveRangeText(s.snap->wb, sheet, linked, &sid, &r)) {
      bool cancel = false;
      if (!SafetyBeforeCommit(s, sid, r.r0, r.c0, value, &param, &cancel)) return;
    }
  }
  s.engine->Post([pn, cn, linked, value, sheet, param](Document& d) {
    d.UpdatePanel(pn, [&](model::Panel& x) {
      for (auto& c : x.controls) {
        if (c.name == cn) c.value = value;
      }
    });
    if (!linked.empty()) {
      d.Begin(TR("Change ") + cn);
      d.param_commit = param;
      try {
        WriteLinked(d, sheet, linked, value);
      } catch (...) {
        d.param_commit = false;
        d.Commit();
        throw;
      }
      d.param_commit = false;
      d.Commit();
    }
  });
}

const ChartObj* FindChartByName(const AppState& s, const std::string& name, uint32_t* sheet) {
  for (const auto& sh : s.snap->wb.sheets) {
    for (const auto& c : *sh->charts) {
      if (c.name == name) {
        *sheet = sh->id;
        return &c;
      }
    }
  }
  return nullptr;
}

// Excel's RGB number (r + g * 256 + b * 65536) as an ImGui color
ImU32 XlColor(int64_t rgb) {
  return IM_COL32(static_cast<int>(rgb & 0xFF), static_cast<int>((rgb >> 8) & 0xFF), static_cast<int>((rgb >> 16) & 0xFF), 255);
}

// a control's colors and font while it draws
// Panel.BackColor / ForeColor: the window and its controls in those colors (frames, buttons and popups follow the
// background's lightness). Returns how many colors were pushed.
int PushPanelColors(const model::Panel& p) {
  if (p.back_color < 0 && p.fore_color < 0) return 0;
  int n = 0;
  auto push = [&n](ImGuiCol c, ImVec4 v) {
    ImGui::PushStyleColor(c, v);
    ++n;
  };
  auto mix = [](ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f); };
  if (p.back_color >= 0) {
    const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(XlColor(p.back_color));
    const bool dark = 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z < 0.5f;
    const ImVec4 toward = dark ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
    push(ImGuiCol_WindowBg, bg);
    push(ImGuiCol_ChildBg, bg);
    push(ImGuiCol_PopupBg, mix(bg, toward, 0.06f));
    push(ImGuiCol_FrameBg, mix(bg, toward, 0.10f));
    push(ImGuiCol_FrameBgHovered, mix(bg, toward, 0.16f));
    push(ImGuiCol_FrameBgActive, mix(bg, toward, 0.22f));
    push(ImGuiCol_Button, mix(bg, toward, 0.12f));
    push(ImGuiCol_ButtonHovered, mix(bg, toward, 0.20f));
    push(ImGuiCol_ButtonActive, mix(bg, toward, 0.28f));
    push(ImGuiCol_Header, mix(bg, toward, 0.14f));
    push(ImGuiCol_HeaderHovered, mix(bg, toward, 0.22f));
    push(ImGuiCol_HeaderActive, mix(bg, toward, 0.30f));
    push(ImGuiCol_Separator, mix(bg, toward, 0.18f));
    push(ImGuiCol_Border, mix(bg, toward, 0.18f));
    // its tab when docked follows the panel's colors (a dark panel's light text on a light tab was unreadable)
    push(ImGuiCol_Tab, mix(bg, toward, 0.08f));
    push(ImGuiCol_TabHovered, mix(bg, toward, 0.22f));
    push(ImGuiCol_TabSelected, mix(bg, toward, 0.14f));
    push(ImGuiCol_TabDimmed, mix(bg, toward, 0.04f));
    push(ImGuiCol_TabDimmedSelected, mix(bg, toward, 0.10f));
    push(ImGuiCol_TitleBg, mix(bg, toward, 0.04f));
    push(ImGuiCol_TitleBgActive, mix(bg, toward, 0.10f));
    push(ImGuiCol_CheckMark, dark ? ImVec4(0.35f, 0.75f, 1.0f, 1.0f) : ImVec4(0.1f, 0.4f, 0.8f, 1.0f));
    push(ImGuiCol_SliderGrab, dark ? ImVec4(0.35f, 0.75f, 1.0f, 1.0f) : ImVec4(0.1f, 0.4f, 0.8f, 1.0f));
    if (p.fore_color < 0) {
      push(ImGuiCol_Text, dark ? ImVec4(0.86f, 0.86f, 0.86f, 1.0f) : ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
      push(ImGuiCol_TextDisabled, dark ? ImVec4(0.45f, 0.45f, 0.45f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
    }
  }
  if (p.fore_color >= 0) push(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(XlColor(p.fore_color)));
  return n;
}

struct ControlStyle {
  int colors = 0;
  bool font = false;
  ControlStyle(const model::PanelControl& pc) {
    if (pc.fore_color >= 0) ImGui::PushStyleColor(ImGuiCol_Text, XlColor(pc.fore_color)), ++colors;
    if (pc.back_color >= 0 && pc.type == "button") {
      const ImU32 c = XlColor(pc.back_color);
      ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
      const ImVec4 hov(std::min(1.0f, v.x * 1.15f + 0.05f), std::min(1.0f, v.y * 1.15f + 0.05f), std::min(1.0f, v.z * 1.15f + 0.05f), 1.0f);
      ImGui::PushStyleColor(ImGuiCol_Button, c);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(v.x * 0.85f, v.y * 0.85f, v.z * 0.85f, 1.0f));
      colors += 3;
    }
    if (pc.font_size > 0 || pc.bold) {
      ImGui::PushFont(pc.bold && ui::F().ui_bold ? ui::F().ui_bold : ui::F().ui, pc.font_size > 0 ? S(static_cast<float>(pc.font_size) * 4.0f / 3.0f) : 0.0f);
      font = true;
    }
  }
  ~ControlStyle() {
    if (font) ImGui::PopFont();
    if (colors) ImGui::PopStyleColor(colors);
  }
};

// a label's linked value with its number format: the text and the format's color (0 = none)
std::string FormattedLinked(const AppState& s, const model::PanelControl& pc, uint32_t* color) {
  uint32_t sid = 0;
  Rect r;
  *color = 0;
  if (pc.linked_cell.empty() || !ResolveRangeText(s.snap->wb, s.focus_sheet, pc.linked_cell, &sid, &r)) return {};
  const model::Sheet* sh = s.snap->Sheet(sid);
  if (!sh) return {};
  const formula::FormatResult fr = formula::FormatValue(sh->GetValue(r.r0, r.c0), pc.number_format);
  *color = fr.color;
  return fr.text;
}

}  // namespace

io::MV PanelInput(AppState& s, const io::MV& p, std::string* err) {
  if (!s.snap) return *err = "No workbook", io::MV();
  const std::string pn = p.GetStr("panel"), cn = p.GetStr("control"), action = p.GetStr("action", "click");
  for (const auto& pnl : *s.snap->wb.panels) {
    if (pnl.name != pn) continue;
    for (const auto& pc : pnl.controls) {
      if (pc.name != cn) continue;
      if (action == "click") {
        if (!HostReady(s)) return *err = "The Python host isn't running", io::MV();
        PanelEvent(s, pnl, pc, "Click");
      } else if (action == "set_value") {
        const io::MV* v = p.Get("value");
        std::string text = !v ? std::string() : v->t == io::MV::T::Str ? v->s : v->t == io::MV::T::Bool ? (v->b ? "TRUE" : "FALSE") : formula::FormatGeneral(v->Num(), 15);
        if (pc.type == "spin" || pc.type == "slider") {  // what the user can reach: between Min and Max, on the steps
          const int lo = static_cast<int>(std::min(pc.min, pc.max)), hi = static_cast<int>(std::max(pc.min, pc.max));
          const int step = std::max(1, static_cast<int>(std::lround(pc.small_change)));
          const int x = static_cast<int>(std::lround(std::atof(text.c_str())));
          text = std::to_string(std::clamp(lo + (x - lo + step / 2) / step * step, lo, hi));
        }
        PanelValue(s, pnl, pc, text);
        PanelEvent(s, pnl, pc, "Change");
      } else {
        return *err = "Unknown panel input: " + action, io::MV();
      }
      return io::MV::Bool(true);
    }
    return *err = "Panel control not found: " + pn + "." + cn, io::MV();
  }
  return *err = "No panel " + pn, io::MV();
}

namespace {

void DrawPanel(AppState& s, const model::Panel& p) {
  UiState& u = U();
  const bool ready = HostReady(s);
  const float avail = ImGui::GetContentRegionAvail().x;
  for (const auto& pc : p.controls) {
    if (!pc.visible) continue;
    ImGui::PushID(pc.name.c_str());
    ImGui::BeginDisabled(!pc.enabled);
    bool linked_ok = false;
    const std::string linked = LinkedText(s, s.focus_sheet, pc.linked_cell, &linked_ok);
    const std::string& value = (linked_ok && !linked.empty()) ? linked : pc.value;
    const std::string key = p.name + "." + pc.name;
    ControlStyle style(pc);
    if (pc.type == "label") {
      uint32_t fmt_color = 0;
      const std::string shown = !pc.number_format.empty() && linked_ok ? FormattedLinked(s, pc, &fmt_color) : linked;
      const std::string text = pc.caption + (linked_ok ? (pc.caption.empty() ? shown : "  " + shown) : std::string());
      if (pc.back_color >= 0) {  // a colored strip behind the label
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, avail);
        ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + ts.y + S(4)), XlColor(pc.back_color), S(2));
        ImGui::SetCursorScreenPos(ImVec2(p0.x + S(4), p0.y + S(2)));
      }
      if (fmt_color) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(ImVec4(((fmt_color >> 16) & 0xFF) / 255.0f, ((fmt_color >> 8) & 0xFF) / 255.0f,
                                                                                          (fmt_color & 0xFF) / 255.0f, 1.0f)));
      ImGui::TextWrapped("%s", text.c_str());
      if (fmt_color) ImGui::PopStyleColor();
      if (pc.back_color >= 0) ImGui::Dummy(ImVec2(0, S(2)));
    } else if (pc.type == "button") {
      ImGui::BeginDisabled(!ready);
      const float h = pc.height > 0 ? S(static_cast<float>(pc.height) * 4.0f / 3.0f) : 0.0f;  // points
      if (ImGui::Button(pc.caption.empty() ? pc.name.c_str() : pc.caption.c_str(), ImVec2(avail, h))) PanelEvent(s, p, pc, "Click");
      ImGui::EndDisabled();
    } else if (pc.type == "textbox") {
      if (!pc.caption.empty()) ImGui::TextUnformatted(pc.caption.c_str());
      auto it = u.panel_text.find(key);
      char buf[1024];
      std::snprintf(buf, sizeof(buf), "%s", it != u.panel_text.end() ? it->second.c_str() : value.c_str());
      ImGui::SetNextItemWidth(-1);
      ImGui::InputText("##t", buf, sizeof(buf));
      if (ImGui::IsItemActive()) u.panel_text[key] = buf;
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        PanelValue(s, p, pc, buf);
        PanelEvent(s, p, pc, "Change");
      }
      if (!ImGui::IsItemActive()) u.panel_text.erase(key);
    } else if (pc.type == "combobox") {
      if (!pc.caption.empty()) ImGui::TextUnformatted(pc.caption.c_str());
      Control tmp;
      tmp.items = pc.items;
      tmp.list_fill_range = pc.list_fill_range;
      const std::vector<std::string> list = ControlItems(s, s.focus_sheet, tmp);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##c", value.c_str())) {
        for (size_t i = 0; i < list.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          if (ImGui::Selectable(list[i].empty() ? " " : list[i].c_str(), list[i] == value)) {
            PanelValue(s, p, pc, list[i]);
            PanelEvent(s, p, pc, "Change");
          }
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
    } else if (pc.type == "checkbox") {
      bool v = value == "1" || Lower(value) == "true";
      if (ImGui::Checkbox(pc.caption.empty() ? pc.name.c_str() : pc.caption.c_str(), &v)) {
        PanelValue(s, p, pc, v ? "TRUE" : "FALSE");
        PanelEvent(s, p, pc, "Change");
      }
    } else if (pc.type == "listbox" || pc.type == "option") {
      // a list to pick one item from, or option buttons (one per item)
      if (!pc.caption.empty()) ImGui::TextUnformatted(pc.caption.c_str());
      Control tmp;
      tmp.items = pc.items;
      tmp.list_fill_range = pc.list_fill_range;
      const std::vector<std::string> list = ControlItems(s, s.focus_sheet, tmp);
      if (pc.type == "listbox") {
        const float rows = static_cast<float>(std::clamp<size_t>(list.size(), 3, 8));
        if (ImGui::BeginListBox("##l", ImVec2(-1, rows * ImGui::GetTextLineHeightWithSpacing() + S(4)))) {
          for (size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(list[i].empty() ? " " : list[i].c_str(), list[i] == value)) {
              PanelValue(s, p, pc, list[i]);
              PanelEvent(s, p, pc, "Change");
            }
            ImGui::PopID();
          }
          ImGui::EndListBox();
        }
      } else {
        for (size_t i = 0; i < list.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          if (ImGui::RadioButton(list[i].empty() ? " " : list[i].c_str(), list[i] == value) && list[i] != value) {
            PanelValue(s, p, pc, list[i]);
            PanelEvent(s, p, pc, "Change");
          }
          ImGui::PopID();
        }
      }
    } else if (pc.type == "spin") {
      // a whole number between Min and Max with - / + buttons
      if (!pc.caption.empty()) ImGui::TextUnformatted(pc.caption.c_str());
      const int lo = static_cast<int>(std::min(pc.min, pc.max)), hi = static_cast<int>(std::max(pc.min, pc.max));
      int v = std::clamp(static_cast<int>(std::lround(std::atof(value.c_str()))), lo, hi);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputInt("##n", &v, std::max(1, static_cast<int>(std::lround(pc.small_change))), 10 * std::max(1, static_cast<int>(std::lround(pc.small_change))))) {
        v = std::clamp(v, lo, hi);
        PanelValue(s, p, pc, std::to_string(v));
        PanelEvent(s, p, pc, "Change");
      }
    } else if (pc.type == "slider") {
      if (!pc.caption.empty()) ImGui::TextUnformatted(pc.caption.c_str());
      // The stored value only changes once the engine publishes, so keep the dragged value locally.
      // Otherwise every frame resets the knob and the release frame posts the old value back.
      auto it = u.panel_slider.find(key);
      if (it != u.panel_slider.end() && !it->second.sent.empty() && (it->second.sent == value || ImGui::GetTime() - it->second.sent_at > 1.0)) {
        u.panel_slider.erase(it);
        it = u.panel_slider.end();
      }
      float v = it != u.panel_slider.end() ? it->second.v : static_cast<float>(std::atof(value.c_str()));
      const float lo = static_cast<float>(std::min(pc.min, pc.max)), hi = static_cast<float>(std::max(pc.min, pc.max));
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("##s", &v, lo, hi > lo ? hi : lo + 1.0f, "%.2f");
      if (ImGui::IsItemActive()) u.panel_slider[key] = UiState::SliderLive{v, "", 0};
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.6g", v);
        u.panel_slider[key] = UiState::SliderLive{v, b, ImGui::GetTime()};
        PanelValue(s, p, pc, b);
        PanelEvent(s, p, pc, "Change");
      } else if (!ImGui::IsItemActive() && it != u.panel_slider.end() && it->second.sent.empty()) {
        u.panel_slider.erase(key);
      }
    } else if (pc.type == "separator") {
      if (pc.caption.empty()) ImGui::Separator();
      else ImGui::SeparatorText(pc.caption.c_str());
    } else if (pc.type == "chart") {
      uint32_t sid = 0;
      if (const ChartObj* ch = FindChartByName(s, pc.chart, &sid)) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(avail, std::max(S(120), static_cast<float>(pc.max > pc.min && pc.max > 100 ? pc.max : 200) * ImGui::GetStyle().FontScaleDpi));
        DrawChartBody(s, sid, *ch, pos, size, true);
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(1, 1));
      } else {
        ImGui::TextDisabled(TR("(chart \"%s\" not found)"), pc.chart.c_str());
      }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
}

}  // namespace

void DrawObjectPasteSpecial(AppState& s);

void DrawObjectWindows(AppState& s) {
  DrawObjectPasteSpecial(s);  // Paste Special while a chart or shape is copied
  DrawAssignMacro(s);
  if (!s.snap) return;
  UpdateChartContext(s);
  UpdateShapeContext(s);
  UiState& u = U();
  // charts in their own windows
  for (const auto& sh : s.snap->wb.sheets) {
    for (const auto& ch : *sh->charts) {
      if (!ch.floating) continue;
      const std::string name = ch.name + "###yc_chartobj_" + std::to_string(ch.id);
      if (!ui::dock::ApplyNextWindowRect(name.c_str())) ImGui::SetNextWindowSize(ImVec2(static_cast<float>(ch.width * kPx) * ui::Dpi(), static_cast<float>(ch.height * kPx) * ui::Dpi()), ImGuiCond_FirstUseEver);
      bool open = true;
      const bool locked = s.WindowsProtected();
      const int lock_flags = LockWindow(locked);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      const bool vis = ImGui::Begin(name.c_str(), locked ? nullptr : &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | lock_flags);
      ImGui::PopStyleVar();
      if (vis) {
        const ImVec2 p = ImGui::GetCursorScreenPos(), a = ImGui::GetContentRegionAvail();
        const ChartObj* shown = ChartDataPreview(sh->id, ch.id);
        NoteChartRect(sh->id, ch.id, ImRect(p, p + a));
        DrawChartBody(s, sh->id, shown ? *shown : ch, p, a, true);
        if (ch.series.empty() && ch.pivot.empty() && !SelectDataOpen()) DrawEmptyChartPrompt(s, sh->id, ch.id, ImRect(p, p + a));
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) OpenObjectContext(ObjSel{sh->id, 2, ch.id});
        DrawObjectContext(s);
      }
      ImGui::End();
      if (!open) {
        const ObjSel o{sh->id, 2, ch.id};
        s.engine->PostUser(TR("Chart moved back to the sheet"), [o](Document& d) { d.UpdateChart(o.sheet, o.id, [](ChartObj& c) { c.floating = false; }); });
      }
    }
  }
  // custom panels
  {
    // a deleted panel takes its window out of the dock, so its empty place closes up
    std::set<std::string> now;
    for (const auto& p : *s.snap->wb.panels) now.insert(p.name);
    for (const auto& n : u.panels_known) {
      if (now.count(n)) continue;
      ui::dock::UndockWindow(("###yc_panel_" + n).c_str());
      u.panels_docked.erase(n);
    }
    u.panels_known = std::move(now);
  }
  for (const auto& p : *s.snap->wb.panels) {
    if (!p.visible) continue;
    if (p.form) {
      DrawUserForm(s, p);
      continue;
    }
    const std::string name = (p.caption.empty() ? p.name : p.caption) + "###yc_panel_" + p.name;
    const int dir = DockDir(p.dock);
    if (!u.panels_docked.count(p.name)) {
      if (dir >= 0) {
        // don't split again when the layout restore already placed it (a dock node in the window or the settings)
        ImGuiWindow* pw = ImGui::FindWindowByName(name.c_str());
        ImGuiWindowSettings* ps = ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()));
        const bool placed = (pw && (pw->DockNode || pw->DockId)) || (ps && ps->DockId);
        if (!placed) {
          const float total = ImGui::GetMainViewport()->Size.x;
          const float ratio = (dir <= 1) ? static_cast<float>(p.width) * ui::Dpi() / std::max(1.0f, total) : static_cast<float>(p.height) * ui::Dpi() / std::max(1.0f, ImGui::GetMainViewport()->Size.y);
          ui::dock::DockBesideWindow(name.c_str(), kGridHostWindow, dir, ratio);
        }
      } else if (!ui::dock::ApplyNextWindowRect(name.c_str())) {
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(p.width) * ui::Dpi(), static_cast<float>(p.height) * ui::Dpi()), ImGuiCond_FirstUseEver);
      }
      u.panels_docked.insert(p.name);
    } else {
      ui::dock::ApplyNextWindowRect(name.c_str());
    }
    bool open = true;
    const int pushed = PushPanelColors(p);
    const bool locked = s.WindowLocked("panel:" + p.name);
    const int lock_flags = LockWindow(locked);
    if (ImGui::Begin(name.c_str(), locked ? nullptr : &open, lock_flags)) DrawPanel(s, p);
    ImGui::End();
    if (pushed) ImGui::PopStyleColor(pushed);
    if (!open) {
      const std::string pn = p.name;
      s.engine->Post([pn](Document& d) { d.UpdatePanel(pn, [](model::Panel& x) { x.visible = false; }); });
    }
  }
  DrawPropertiesWindow(s);
  DrawSelectDataWindow(s);
  // Escape ends drawing a form control with nothing selected too (Excel: the next drag selects cells; it drew one)
  if (!u.sel && u.insert_kind >= 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !EscClosedPopup()) {
    u.insert_kind = -1;
    s.SetStatus("");
  }
  // when an object is selected: arrow keys and Delete go to the object (grid keyboard handling is off)
  if (u.sel && s.FocusSheet() && u.sel.sheet == s.focus_sheet && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    ImGuiWindow* nav = GImGui->NavWindow;
    const bool grid_focus = nav && (std::strstr(nav->Name, "###yc_sheet_") || std::strstr(nav->Name, "###yc_gridhost"));
    if (grid_focus) HandleObjectKeys(s);
  }
}

void ForgetPanelDocking() { U().panels_docked.clear(); }

namespace {

// a UserForm: controls at their own positions (points), in a dialog that blocks the window when it is modal
void DrawFormBody(AppState& s, const model::Panel& p) {
  UiState& u = U();
  const float k = ui::Dpi() * (96.0f / 72.0f);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const bool ready = HostReady(s);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float y_flow = 0;
  for (const auto& pc : p.controls) {
    if (!pc.visible) continue;
    ImGui::PushID(pc.name.c_str());
    ImGui::BeginDisabled(!pc.enabled);
    const float w = pc.width > 0 ? static_cast<float>(pc.width) * k : S(120), h = pc.height > 0 ? static_cast<float>(pc.height) * k : 0.0f;
    const ImVec2 at = pc.left >= 0 ? ImVec2(origin.x + static_cast<float>(pc.left) * k, origin.y + static_cast<float>(pc.top) * k) : ImVec2(origin.x, origin.y + y_flow);
    ImGui::SetCursorScreenPos(at);
    const std::string key = p.name + "." + pc.name;
    const std::string& value = pc.value;
    const std::string caption = pc.caption.empty() && pc.type != "label" && pc.type != "frame" ? pc.name : pc.caption;
    if (pc.type == "label") {
      ImGui::PushTextWrapPos(at.x - ImGui::GetWindowPos().x + w);
      ImGui::TextWrapped("%s", caption.c_str());
      ImGui::PopTextWrapPos();
    } else if (pc.type == "frame") {
      const float ty = at.y + ImGui::GetTextLineHeight() * 0.5f;
      const ImU32 line = ImGui::GetColorU32(ImGuiCol_Border);
      dl->AddRect(ImVec2(at.x, ty), ImVec2(at.x + w, at.y + std::max(h, S(20))), line);
      if (!caption.empty()) {
        const ImVec2 ts = ImGui::CalcTextSize(caption.c_str());
        dl->AddRectFilled(ImVec2(at.x + S(6), at.y), ImVec2(at.x + S(10) + ts.x, at.y + ts.y), ImGui::GetColorU32(ImGuiCol_WindowBg));
        dl->AddText(ImVec2(at.x + S(8), at.y), ImGui::GetColorU32(ImGuiCol_Text), caption.c_str());
      }
      ImGui::Dummy(ImVec2(1, 1));
    } else if (pc.type == "button") {
      ImGui::BeginDisabled(!ready);
      if (ImGui::Button(caption.c_str(), ImVec2(w, h))) PanelEvent(s, p, pc, "Click");
      ImGui::EndDisabled();
    } else if (pc.type == "textbox") {
      auto it = u.panel_text.find(key);
      char buf[4096];
      std::snprintf(buf, sizeof(buf), "%s", it != u.panel_text.end() ? it->second.c_str() : value.c_str());
      if (h > ImGui::GetFrameHeight() * 1.6f) ImGui::InputTextMultiline("##t", buf, sizeof(buf), ImVec2(w, h));
      else {
        ImGui::SetNextItemWidth(w);
        ImGui::InputText("##t", buf, sizeof(buf));
      }
      if (ImGui::IsItemActive()) u.panel_text[key] = buf;
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        PanelValue(s, p, pc, buf);
        PanelEvent(s, p, pc, "Change");
      }
      if (!ImGui::IsItemActive()) u.panel_text.erase(key);
    } else if (pc.type == "combobox" || pc.type == "listbox") {
      Control tmp;
      tmp.items = pc.items;
      tmp.list_fill_range = pc.list_fill_range;
      const std::vector<std::string> list = ControlItems(s, s.focus_sheet, tmp);
      auto pick = [&](size_t i) {
        PanelValue(s, p, pc, list[i]);
        PanelEvent(s, p, pc, "Change");
        PanelEvent(s, p, pc, "Click");
      };
      if (pc.type == "combobox") {
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##c", value.c_str())) {
          for (size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(list[i].empty() ? " " : list[i].c_str(), list[i] == value)) pick(i);
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
      } else if (ImGui::BeginListBox("##l", ImVec2(w, h > 0 ? h : S(80)))) {
        for (size_t i = 0; i < list.size(); ++i) {
          ImGui::PushID(static_cast<int>(i));
          if (ImGui::Selectable(list[i].empty() ? " " : list[i].c_str(), list[i] == value)) pick(i);
          ImGui::PopID();
        }
        ImGui::EndListBox();
      }
    } else if (pc.type == "checkbox" || pc.type == "togglebutton") {
      bool v = value == "1" || Lower(value) == "true";
      const bool changed = pc.type == "checkbox" ? ImGui::Checkbox(caption.c_str(), &v) : ImGui::Selectable(caption.c_str(), &v, 0, ImVec2(w, h));
      if (changed) {
        PanelValue(s, p, pc, v ? "True" : "False");
        PanelEvent(s, p, pc, "Change");
        PanelEvent(s, p, pc, "Click");
      }
    } else if (pc.type == "optionbutton") {
      // one option button; the others of its group (GroupName, or all the form's ungrouped ones) turn off
      const bool on = value == "1" || Lower(value) == "true";
      if (ImGui::RadioButton(caption.c_str(), on) && !on) {
        const std::string pn = p.name, cn = pc.name, grp = pc.group;
        s.engine->Post([pn, cn, grp](Document& d) {
          d.UpdatePanel(pn, [&](model::Panel& x) {
            for (auto& c : x.controls) {
              if (c.type == "optionbutton" && c.group == grp) c.value = c.name == cn ? "True" : "False";
            }
          });
        });
        PanelEvent(s, p, pc, "Change");
        PanelEvent(s, p, pc, "Click");
      }
    } else if (pc.type == "spin" || pc.type == "slider") {
      const int lo = static_cast<int>(std::min(pc.min, pc.max)), hi = static_cast<int>(std::max(pc.min, pc.max));
      int v = std::clamp(static_cast<int>(std::lround(std::atof(value.c_str()))), lo, hi);
      ImGui::SetNextItemWidth(w);
      const bool changed = pc.type == "spin" ? ImGui::InputInt("##n", &v, std::max(1, static_cast<int>(std::lround(pc.small_change))), 10 * std::max(1, static_cast<int>(std::lround(pc.small_change)))) : ImGui::SliderInt("##s", &v, lo, hi > lo ? hi : lo + 1);
      if (changed) {
        const int step = std::max(1, static_cast<int>(std::lround(pc.small_change)));
        v = std::clamp(lo + (v - lo + step / 2) / step * step, lo, hi);  // on the steps (a slider dragged between them)
        PanelValue(s, p, pc, std::to_string(v));
        PanelEvent(s, p, pc, "Change");
      }
    }
    if (pc.left < 0) y_flow += std::max(ImGui::GetItemRectSize().y, h) + ImGui::GetStyle().ItemSpacing.y;
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  // the content size: the furthest control
  float maxx = 0, maxy = y_flow;
  for (const auto& pc : p.controls) {
    if (pc.left >= 0) maxx = std::max(maxx, static_cast<float>(pc.left + pc.width) * k), maxy = std::max(maxy, static_cast<float>(pc.top + pc.height) * k);
  }
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(maxx, maxy));
}

}  // namespace

void DrawUserForm(AppState& s, const model::Panel& p) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const std::string win = (p.caption.empty() ? p.name : p.caption) + "###yc_form_" + p.name;
  const std::string block = "##yc_form_block_" + p.name;
  if (p.modal) {
    // a modal form: a dim layer over the window takes every click; clicking it brings the form back to the front
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 50));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(block.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::InvisibleButton("##block", vp->Size);
    const bool clicked = ImGui::IsItemClicked();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (clicked) ImGui::SetNextWindowFocus();
  }
  const float k = ui::Dpi() * (96.0f / 72.0f);
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(p.width) * k, static_cast<float>(p.height) * k), ImGuiCond_Appearing);
  bool open = true;
  if (ImGui::Begin(win.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    if (p.modal) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    DrawFormBody(s, p);
  }
  ImGui::End();
  if (!open) {
    // the close box: Python decides (UserForm_QueryClose) whether the form really goes away
    if (HostReady(s)) s.python->ControlEvent(0, p.name + ".", "QueryClose", "", true);
    else {
      const std::string pn = p.name;
      s.engine->Post([pn](Document& d) { d.DeletePanel(pn); });
    }
  }
}

// ---- Commands ----

void RegisterObjectCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* ic, const char* desc, bool toggle = false) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.tip_desc = desc;
    c.toggle = toggle;
    return c;
  };
  R.Add(make("DesignMode", TR("Design\nMode"), ui::icon::DesignMode, TR("Turn Design Mode on or off: in Design Mode you can select, move and resize controls, and controls don't fire events."), true)).execute = [&s](ui::Command& c) {
    s.design_mode = c.pressed;
    if (!s.design_mode) {
      U().insert_kind = -1;
      if (U().sel.kind == 1) U().sel = {};
    }
    s.SetStatus(s.design_mode ? TR("Design Mode: click a control to select it, drag to move, drag the handles to resize, double-click for its code") : TR("Exited Design Mode"));
  };
  auto insert_ctl = [&s](int kind) {
    return [&s, kind](ui::Command&) {
      s.design_mode = true;
      if (auto* d = ui::Commands().Find("DesignMode")) d->pressed = true;
      U().insert_kind = kind;
      s.SetStatus(TR("Drag on the sheet to draw the control (click = default size; hold Alt to snap to the grid)"));
    };
  };
  R.Add(make("InsertButton", TR("Button"), ui::icon::Button, TR("Design Mode: draw a button (CommandButton) on the sheet."))).execute = insert_ctl(0);
  R.Add(make("InsertComboBox", TR("Combo Box"), ui::icon::ComboBox, TR("Design Mode: draw a combo box (ComboBox) on the sheet."))).execute = insert_ctl(1);
  R.Add(make("InsertCheckBox", TR("Check Box"), ui::icon::CheckBox, TR("Design Mode: draw a check box (CheckBox): True / False."))).execute = insert_ctl(2);
  R.Add(make("InsertOptionButton", TR("Option Button"), ui::icon::OptionButton, TR("Design Mode: draw an option button (OptionButton): one choice per GroupName."))).execute = insert_ctl(3);
  R.Add(make("InsertToggleButton", TR("Toggle Button"), ui::icon::ToggleButton, TR("Design Mode: draw a toggle button (ToggleButton)."))).execute = insert_ctl(4);
  R.Add(make("InsertTextBoxControl", TR("Text Box##ctl"), ui::icon::TextBoxCtl, TR("Design Mode: draw a text box (TextBox) for typed input."))).execute = insert_ctl(5);
  R.Add(make("InsertLabel", TR("Label"), ui::icon::LabelCtl, TR("Design Mode: draw a label (Label)."))).execute = insert_ctl(6);
  R.Add(make("InsertListBox", TR("List Box"), ui::icon::ListBox, TR("Design Mode: draw a list box (ListBox): single or multiple selection."))).execute = insert_ctl(7);
  R.Add(make("InsertSpinButton", TR("Spin Button"), ui::icon::SpinButton, TR("Design Mode: draw a spin button (SpinButton) between Min and Max."))).execute = insert_ctl(8);
  R.Add(make("InsertScrollBar", TR("Scroll Bar"), ui::icon::ScrollBar, TR("Design Mode: draw a scroll bar (ScrollBar) between Min and Max."))).execute = insert_ctl(9);
  R.Add(make("InsertImageControl", TR("Image"), ui::icon::ImageCtl, TR("Design Mode: draw an image control (Image); set its picture in Properties."))).execute = insert_ctl(10);
  R.Add(make("InsertControlsMenu", TR("Insert##ctl"), ui::icon::ControlsGroup, TR("Insert a form control or an ActiveX control."))).execute = [](ui::Command&) {};
  auto insert_form = [&s](int kind) {
    return [&s, kind](ui::Command&) {
      s.design_mode = true;
      if (auto* d = ui::Commands().Find("DesignMode")) d->pressed = true;
      U().insert_kind = kind;
      U().insert_form = true;
      s.SetStatus(TR("Drag on the sheet to draw the form control (click = default size)"));
    };
  };
  R.Add(make("InsertFormButton", TR("Button (Form Control)"), ui::icon::Button, TR("Draw a form button that runs a macro."))).execute = insert_form(0);
  R.Add(make("InsertFormComboBox", TR("Combo Box (Form Control)"), ui::icon::ComboBox, TR("Draw a drop-down list; the linked cell gets the number of the chosen item."))).execute =
      insert_form(1);
  R.Add(make("InsertFormCheckBox", TR("Check Box (Form Control)"), ui::icon::CheckBox, TR("Draw a check box; the linked cell gets TRUE or FALSE."))).execute = insert_form(2);
  R.Add(make("InsertFormSpinButton", TR("Spin Button (Form Control)"), ui::icon::SpinButton, TR("Draw a spin button that steps a number in the linked cell."))).execute =
      insert_form(8);
  R.Add(make("InsertFormListBox", TR("List Box (Form Control)"), ui::icon::ListBox, TR("Draw a list box; the linked cell gets the number of the chosen item."))).execute =
      insert_form(7);
  R.Add(make("InsertFormOptionButton", TR("Option Button (Form Control)"), ui::icon::OptionButton,
             TR("Draw an option button; the options in one group box share a linked cell that gets the number of the chosen one."))).execute = insert_form(3);
  R.Add(make("InsertFormGroupBox", TR("Group Box (Form Control)"), ui::icon::GroupBox, TR("Draw a frame that groups option buttons."))).execute = insert_form(11);
  R.Add(make("InsertFormLabel", TR("Label (Form Control)"), ui::icon::LabelCtl, TR("Draw a label."))).execute = insert_form(6);
  R.Add(make("InsertFormScrollBar", TR("Scroll Bar (Form Control)"), ui::icon::ScrollBar, TR("Draw a scroll bar that sets a number in the linked cell."))).execute = insert_form(9);
  R.Add(make("ControlProperties", TR("Properties"), ui::icon::Properties, TR("Show the properties of the selected control or chart."))).execute = [](ui::Command&) { U().show_props = true; };
  auto chart = [&s](ChartType t) {
    return [&s, t](ui::Command&) { InsertChartFromSelection(s, t); };
  };
  R.Add(make("InsertChartColumn", TR("Clustered Column"), ui::icon::ChartColumn, TR("Insert a clustered column chart from the selection."))).execute = chart(ChartType::ColumnClustered);
  R.Add(make("InsertChartLine", TR("Line"), ui::icon::ChartLine, TR("Insert a line chart from the selection."))).execute = chart(ChartType::Line);
  R.Add(make("InsertChartPie", TR("Pie"), ui::icon::ChartPie, TR("Insert a pie chart from the selection."))).execute = chart(ChartType::Pie);
  R.Add(make("InsertChartScatter", TR("Scatter"), ui::icon::ChartScatter, TR("Insert an XY scatter chart from the selection."))).execute = chart(ChartType::Scatter);
  R.Add(make("InsertChartBar", TR("Clustered Bar"), ui::icon::ChartColumn, TR("Insert a clustered bar chart from the selection."))).execute = chart(ChartType::BarClustered);
  R.Add(make("InsertChartArea", TR("Area"), ui::icon::ChartLine, TR("Insert an area chart from the selection."))).execute = chart(ChartType::Area);
  R.Add(make("InsertChartStock", TR("Stock"), ui::icon::ChartColumn, TR("Insert a stock chart from the selection (open, high, low, close)."))).execute = chart(ChartType::StockOHLC);
  R.Add(make("InsertChartRadar", TR("Radar"), ui::icon::ChartRadar, TR("Insert a radar chart from the selection."))).execute = chart(ChartType::Radar);
  R.Add(make("InsertChartWaterfall", TR("Waterfall"), ui::icon::ChartWaterfall, TR("Insert a waterfall chart: how increases and decreases add up to a total."))).execute =
      chart(ChartType::Waterfall);
  R.Add(make("InsertChartFunnel", TR("Funnel"), ui::icon::ChartFunnel, TR("Insert a funnel chart: values across the stages of a process."))).execute = chart(ChartType::Funnel);
  R.Add(make("InsertChartHistogram", TR("Histogram"), ui::icon::ChartHistogram, TR("Insert a histogram: how often the values fall into each bin."))).execute =
      chart(ChartType::Histogram);
  R.Add(make("InsertChartPareto", TR("Pareto"), ui::icon::ChartHistogram, TR("Insert a Pareto chart: sorted columns with the cumulative percentage."))).execute =
      chart(ChartType::Pareto);
  R.Add(make("InsertChartBoxWhisker", TR("Box and Whisker"), ui::icon::ChartBoxWhisker, TR("Insert a box and whisker chart: quartiles, median, mean and outliers."))).execute =
      chart(ChartType::BoxWhisker);
  R.Add(make("InsertChartTreemap", TR("Treemap"), ui::icon::ChartTreemap, TR("Insert a treemap: nested rectangles sized by value."))).execute = chart(ChartType::Treemap);
  R.Add(make("InsertChartSunburst", TR("Sunburst"), ui::icon::ChartSunburst, TR("Insert a sunburst chart: the levels of a hierarchy as rings."))).execute =
      chart(ChartType::Sunburst);
  R.Add(make("InsertChartCombo", TR("Clustered Column - Line"), ui::icon::ChartCombo, TR("Insert a combo chart: clustered columns with the last series as a line."))).execute = [&s](ui::Command&) {
    InsertChartFromSelection(s, ChartType::ColumnClustered, true);
  };
  // Insert > Combo: Excel's drop-down of three combos and Create Custom Combo Chart
  R.Add(make("InsertChartComboMenu", TR("Combo"), ui::icon::ChartCombo, TR("Insert a combo chart: mix chart types to show different kinds of information."))).execute =
      [](ui::Command&) {};
  R.Add(make("InsertChartComboSecondary", TR("Clustered Column - Line on Secondary Axis"), ui::icon::ChartCombo,
             TR("Insert a combo chart: clustered columns, the last series as a line on the secondary axis."))).execute = [&s](ui::Command&) {
    ChartObj ch;
    Rect r;
    BuildChartFromSelection(s, ChartType::ColumnClustered, &ch, &r);
    if (ch.series.size() >= 2) {
      ch.series.back().type = static_cast<int>(ChartType::Line);
      ch.series.back().axis_group = 1;
    }
    InsertBuiltChart(s, std::move(ch), r);
  };
  R.Add(make("InsertChartComboArea", TR("Stacked Area - Clustered Column"), ui::icon::ChartCombo,
             TR("Insert a combo chart: stacked areas, the last series as clustered columns."))).execute = [&s](ui::Command&) {
    ChartObj ch;
    Rect r;
    BuildChartFromSelection(s, ChartType::AreaStacked, &ch, &r);
    if (ch.series.size() >= 2) ch.series.back().type = static_cast<int>(ChartType::ColumnClustered);
    InsertBuiltChart(s, std::move(ch), r);
  };
  R.Add(make("InsertChartComboCustom", TR("Create Custom Combo Chart..."), ui::icon::ChartCombo, TR("Choose the chart type and axis of each series."))).execute =
      [&s](ui::Command&) { OpenChartTypeDialog(s, false, -1, -2); };
  R.Add(make("InsertChart3DColumn", TR("3-D Column"), ui::icon::ChartColumn, TR("Insert a 3-D column chart: categories across, each series in its own row."))).execute =
      chart(ChartType::Column3D);
  R.Add(make("InsertChart3DClusteredColumn", TR("3-D Clustered Column"), ui::icon::ChartColumn, TR("Insert a 3-D clustered column chart."))).execute =
      chart(ChartType::ColumnClustered3D);
  R.Add(make("InsertChart3DStackedColumn", TR("3-D Stacked Column"), ui::icon::ChartColumn, TR("Insert a 3-D stacked column chart."))).execute =
      chart(ChartType::ColumnStacked3D);
  R.Add(make("InsertChart3DBar", TR("3-D Clustered Bar"), ui::icon::ChartColumn, TR("Insert a 3-D clustered bar chart."))).execute = chart(ChartType::BarClustered3D);
  R.Add(make("InsertChart3DLine", TR("3-D Line"), ui::icon::ChartLine, TR("Insert a 3-D line chart: a ribbon per series."))).execute = chart(ChartType::Line3D);
  R.Add(make("InsertChart3DArea", TR("3-D Area"), ui::icon::ChartLine, TR("Insert a 3-D area chart."))).execute = chart(ChartType::Area3D);
  R.Add(make("InsertChart3DPie", TR("3-D Pie"), ui::icon::ChartPie, TR("Insert a 3-D pie chart."))).execute = chart(ChartType::Pie3D);
  R.Add(make("InsertChartSurface", TR("3-D Surface"), ui::icon::ChartRadar, TR("Insert a 3-D surface chart: values over two directions, colored by value bands."))).execute =
      chart(ChartType::Surface);
  R.Add(make("InsertChartWireframe", TR("Wireframe 3-D Surface"), ui::icon::ChartRadar, TR("Insert a wireframe 3-D surface chart."))).execute =
      chart(ChartType::SurfaceWireframe);
  R.Add(make("InsertChartContour", TR("Contour"), ui::icon::ChartRadar, TR("Insert a contour chart: a surface seen from the top."))).execute =
      chart(ChartType::SurfaceContour);
  R.Add(make("InsertChartMap", TR("Maps"), ui::icon::Map, TR("Insert a filled map: countries, states, provinces or counties colored by their values (or by category)."))).execute =
      chart(ChartType::FilledMap);
  R.Add(make("InsertChart3DMenu", TR("3-D"), ui::icon::ChartColumn, TR("Insert a 3-D column, bar, line, area or pie chart."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartSurfaceMenu", TR("Surface"), ui::icon::ChartRadar, TR("Insert a surface or contour chart."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartHierarchyMenu", TR("Hierarchy"), ui::icon::ChartTreemap, TR("Insert a hierarchy chart: treemap or sunburst."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartStatisticMenu", TR("Statistic"), ui::icon::ChartHistogram, TR("Insert a statistic chart: histogram, Pareto or box and whisker."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartWaterfallMenu", TR("Waterfall##ctx_menu"), ui::icon::ChartWaterfall, TR("Insert a waterfall, funnel, stock or radar chart."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartDialog", TR("All Charts"), nullptr, TR("Choose a chart type."))).execute = [&s](ui::Command&) { OpenChartTypeDialog(s, false); };
  // the Insert tab's chart galleries, as Excel's (Insert Column or Bar Chart, Insert Line or Area Chart...): every variant,
  // and More ... Charts opening Insert Chart on that kind
  struct Variant {
    const char* id;
    const char* label;
    const char* icon;
    ChartType type;
  };
  static const Variant kVariants[] = {
      {"InsertChartColumnStacked", TR("Stacked Column"), ui::icon::ChartColumn, ChartType::ColumnStacked},
      {"InsertChartColumnStacked100", TR("100% Stacked Column"), ui::icon::ChartColumn, ChartType::ColumnStacked100},
      {"InsertChartBarStacked", TR("Stacked Bar"), ui::icon::ChartColumn, ChartType::BarStacked},
      {"InsertChartBarStacked100", TR("100% Stacked Bar"), ui::icon::ChartColumn, ChartType::BarStacked100},
      {"InsertChartLineMarkers", TR("Line with Markers"), ui::icon::ChartLine, ChartType::LineMarkers},
      {"InsertChartLineStacked", TR("Stacked Line"), ui::icon::ChartLine, ChartType::LineStacked},
      {"InsertChartLineStacked100", TR("100% Stacked Line"), ui::icon::ChartLine, ChartType::LineStacked100},
      {"InsertChartLineMarkersStacked", TR("Stacked Line with Markers"), ui::icon::ChartLine, ChartType::LineMarkersStacked},
      {"InsertChartLineMarkersStacked100", TR("100% Stacked Line with Markers"), ui::icon::ChartLine, ChartType::LineMarkersStacked100},
      {"InsertChartAreaStacked", TR("Stacked Area"), ui::icon::ChartLine, ChartType::AreaStacked},
      {"InsertChartAreaStacked100", TR("100% Stacked Area"), ui::icon::ChartLine, ChartType::AreaStacked100},
      {"InsertChartDoughnut", TR("Doughnut"), ui::icon::ChartPie, ChartType::Doughnut},
      {"InsertChartScatterSmooth", TR("Scatter with Smooth Lines and Markers"), ui::icon::ChartScatter, ChartType::ScatterSmooth},
      {"InsertChartScatterSmoothNoMarkers", TR("Scatter with Smooth Lines"), ui::icon::ChartScatter, ChartType::ScatterSmoothNoMarkers},
      {"InsertChartScatterLines", TR("Scatter with Straight Lines and Markers"), ui::icon::ChartScatter, ChartType::ScatterLines},
      {"InsertChartScatterLinesNoMarkers", TR("Scatter with Straight Lines"), ui::icon::ChartScatter, ChartType::ScatterLinesNoMarkers},
      {"InsertChartBubble", TR("Bubble"), ui::icon::ChartScatter, ChartType::Bubble},
      {"InsertChartStockHLC", TR("High-Low-Close"), ui::icon::ChartColumn, ChartType::StockHLC},
      {"InsertChartStockVHLC", TR("Volume-High-Low-Close"), ui::icon::ChartColumn, ChartType::StockVHLC},
      {"InsertChartStockVOHLC", TR("Volume-Open-High-Low-Close"), ui::icon::ChartColumn, ChartType::StockVOHLC},
      {"InsertChartPieOfPie", TR("Pie of Pie"), ui::icon::ChartPie, ChartType::PieOfPie},
      {"InsertChartBarOfPie", TR("Bar of Pie"), ui::icon::ChartPie, ChartType::BarOfPie},
      {"InsertChartRadarMarkers", TR("Radar with Markers"), ui::icon::ChartRadar, ChartType::RadarMarkers},
      {"InsertChartRadarFilled", TR("Filled Radar"), ui::icon::ChartRadar, ChartType::RadarFilled},
  };
  for (const Variant& v : kVariants) {
    R.Add(make(v.id, v.label, v.icon, TR("Insert this chart from the selection."))).execute = chart(v.type);
    GalleryChartTypes()[v.id] = v.type;
  }
  for (const auto& [id, t] : std::initializer_list<std::pair<const char*, ChartType>>{
           {"InsertChartColumn", ChartType::ColumnClustered}, {"InsertChartLine", ChartType::Line}, {"InsertChartPie", ChartType::Pie},
           {"InsertChartScatter", ChartType::Scatter}, {"InsertChartBar", ChartType::BarClustered}, {"InsertChartArea", ChartType::Area},
           {"InsertChartStock", ChartType::StockOHLC}, {"InsertChartRadar", ChartType::Radar}, {"InsertChartWaterfall", ChartType::Waterfall},
           {"InsertChartFunnel", ChartType::Funnel}, {"InsertChartHistogram", ChartType::Histogram}, {"InsertChartPareto", ChartType::Pareto},
           {"InsertChartBoxWhisker", ChartType::BoxWhisker}, {"InsertChartTreemap", ChartType::Treemap}, {"InsertChartSunburst", ChartType::Sunburst},
           {"InsertChart3DColumn", ChartType::Column3D}, {"InsertChart3DClusteredColumn", ChartType::ColumnClustered3D},
           {"InsertChart3DStackedColumn", ChartType::ColumnStacked3D}, {"InsertChart3DBar", ChartType::BarClustered3D}, {"InsertChart3DLine", ChartType::Line3D},
           {"InsertChart3DArea", ChartType::Area3D}, {"InsertChart3DPie", ChartType::Pie3D}, {"InsertChartSurface", ChartType::Surface},
           {"InsertChartWireframe", ChartType::SurfaceWireframe}, {"InsertChartContour", ChartType::SurfaceContour}, {"InsertChartMap", ChartType::FilledMap}}) {
    GalleryChartTypes()[id] = t;
  }
  R.Add(make("InsertChartColumnMenu", TR("Column or\nBar"), ui::icon::ChartColumn, TR("Insert a column or bar chart: compare values across categories."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartLineMenu", TR("Line or\nArea"), ui::icon::ChartLine, TR("Insert a line or area chart: show trends over time or categories."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartPieMenu", TR("Pie or\nDoughnut"), ui::icon::ChartPie, TR("Insert a pie or doughnut chart: show the parts of a whole."))).execute = [](ui::Command&) {};
  R.Add(make("InsertChartScatterMenu", TR("Scatter or\nBubble"), ui::icon::ChartScatter, TR("Insert a scatter (X, Y) or bubble chart: show how values relate."))).execute = [](ui::Command&) {};
  auto more = [&s](ChartType t) { return [&s, t](ui::Command&) { OpenChartTypeDialog(s, false, -1, static_cast<int>(t)); }; };
  R.Add(make("InsertChartMoreColumn", TR("More Column Charts..."), nullptr, TR("Insert Chart, on its column charts."))).execute = more(ChartType::ColumnClustered);
  R.Add(make("InsertChartMoreLine", TR("More Line Charts..."), nullptr, TR("Insert Chart, on its line charts."))).execute = more(ChartType::Line);
  R.Add(make("InsertChartMorePie", TR("More Pie Charts..."), nullptr, TR("Insert Chart, on its pie charts."))).execute = more(ChartType::Pie);
  R.Add(make("InsertChartMoreScatter", TR("More Scatter Charts..."), nullptr, TR("Insert Chart, on its scatter charts."))).execute = more(ChartType::Scatter);
  R.Add(make("RecommendedCharts", TR("Recommended\nCharts"), ui::icon::RecommendedCharts, TR("Charts that suit the selected data, each previewed: pick one."))).execute = [&s](ui::Command&) {
    OpenChartTypeDialog(s, false, -1, -1, true);
  };
  R.Add(make("DeleteObject", TR("Delete Object"), nullptr, TR("Delete the selected control or chart."))).execute = [&s](ui::Command&) { DeleteSelected(s); };
  RegisterChartCommands(s);
  RegisterShapeCommands(s);
}

// ---- Chart Design / Format tabs, chart sheets, Move Chart ----

namespace {

// the selected chart, or the active chart sheet's chart
const ChartObj* SelectedChart(const AppState& s, ObjSel* o) {
  if (!s.snap) return nullptr;
  const UiState& u = U();
  if (u.sel.kind == 2 && u.sel.sheet == s.focus_sheet) {  // a chart left selected on another sheet isn't the one worked on
    if (const model::Sheet* sh = s.snap->Sheet(u.sel.sheet)) {
      for (const auto& c : *sh->charts) {
        if (c.id == u.sel.id) {
          *o = u.sel;
          return &c;
        }
      }
    }
  }
  const model::Sheet* sh = s.FocusSheet();
  if (sh && sh->chart_sheet && !sh->charts->empty()) {
    *o = ObjSel{sh->id, 2, (*sh->charts)[0].id};
    return &(*sh->charts)[0];
  }
  return nullptr;
}

void EditSelectedChart(AppState& s, const char* label, std::function<void(ChartObj&)> fn) {
  ObjSel o;
  if (!SelectedChart(s, &o)) return;
  s.engine->PostUser(label, [o, fn = std::move(fn)](Document& d) { d.UpdateChart(o.sheet, o.id, fn); });
}

std::string NextChartSheetName(const model::Workbook& wb) {
  for (int n = 1;; ++n) {
    const std::string name = TR("Chart") + std::to_string(n);
    if (wb.FindSheet(name) < 0) return name;
  }
}

ObjSel g_move;  // the chart Move Chart moves
int g_move_choice = 0, g_move_target = 0;
char g_move_name[128] = {};

}  // namespace

// every frame: the Chart Design and Format tabs and their check marks
// the check marks of Add Chart Element's set-one-way commands
struct ChartElemState {
  std::string id;
  std::function<bool(const ChartObj&)> on;
};
std::vector<ChartElemState>& ChartElemStates() {
  static std::vector<ChartElemState> v;
  return v;
}
constexpr int kSelSeries = 8;  // Format > Chart Elements lists the first series by name

ChartType& StyleChartType();

void UpdateChartContext(AppState& s) {
  ObjSel o;
  const ChartObj* ch = SelectedChart(s, &o);
  ui::ribbon::Context().chart = ch != nullptr;
  StyleChartType() = ch ? ch->type : ChartType::ColumnClustered;  // the Chart Styles galleries show this type's set
  if (!ch) return;
  auto press = [](const char* id, bool v) {
    if (auto* c = ui::Commands().Find(id)) c->pressed = v;
  };
  press("ChartElemTitle", ch->has_title);
  press("ChartElemLegendNone", !ch->has_legend);
  for (int i = 0; i < 5; ++i) {
    static const char* kIds[] = {"ChartElemLegendBottom", "ChartElemLegendRight", "ChartElemLegendTop", "ChartElemLegendLeft", "ChartElemLegendCorner"};
    press(kIds[i], ch->has_legend && ch->legend_pos == i);
  }
  press("ChartElemAxisCat", ch->cat_axis.visible);
  press("ChartElemAxisVal", ch->val_axis.visible);
  press("ChartElemAxisTitleCat", ch->cat_axis.has_title);
  press("ChartElemAxisTitleVal", ch->val_axis.has_title);
  press("ChartElemGridCat", ch->cat_axis.gridlines);
  press("ChartElemGridVal", ch->val_axis.gridlines);
  bool labels = !ch->series.empty();
  for (const auto& se : ch->series) labels = labels && se.data_labels;
  press("ChartElemDataLabels", labels);
  for (const auto& st : ChartElemStates()) press(st.id.c_str(), st.on(*ch));
  // Format > Chart Elements: each series by its name
  for (int i = 0; i < kSelSeries; ++i) {
    if (auto* c = ui::Commands().Find("ChartSelSeries" + std::to_string(i + 1))) {
      c->enabled = static_cast<size_t>(i) < ch->series.size();
      c->label = c->enabled ? ChartElemName(*ch, model::ChartElem::Series, i) : TR("Series") + std::to_string(i + 1);
    }
  }
  for (int i = 0; i < model::kChartTypeCount; ++i) press(("ChartType" + std::to_string(i)).c_str(), static_cast<int>(ch->type) == i);
}

// the chart type the Chart Styles galleries show their set for: the selected chart's (columns while none is)
ChartType& StyleChartType() {
  static ChartType t = ChartType::ColumnClustered;
  return t;
}

// a small picture of a chart with Quick Layout n (kind 1) or Chart Style n (kind 2) applied: Excel's galleries show
// each choice as the chart it makes, a style in the selected chart's own type (columns, bars, lines, pies...)
// Insert > Charts' drop-downs: a small picture of each kind of chart, as Excel's galleries show (combo: 1 column - line,
// 2 column - line on the secondary axis, 3 stacked area - column). False: no picture for this type (its icon is used).
bool DrawChartTypePictogram(ImDrawList* dl, ImVec2 a, float sz, ChartType t, int combo) {
  using T = ChartType;
  const ImU32 c0 = engine::ChartPalette(0), c1 = engine::ChartPalette(1), c2 = engine::ChartPalette(2), axis = IM_COL32(0x90, 0x90, 0x90, 255);
  const float u = sz / 16.0f;
  auto P = [&](float x, float y) { return ImVec2(a.x + x * u, a.y + y * u); };
  auto box = [&](float x0, float y0, float x1, float y1, ImU32 c) { dl->AddRectFilled(P(x0, y0), P(x1, y1), c); };
  auto shade = [](ImU32 c, float k) {
    const auto ch = [&](int sh) { return static_cast<ImU32>(std::clamp(static_cast<float>((c >> sh) & 0xFF) * k, 0.0f, 255.0f)) << sh; };
    return ch(0) | ch(8) | ch(16) | (c & 0xFF000000u);
  };
  // a bar with a 3-D top and side
  auto box3 = [&](float x0, float y0, float x1, float y1, ImU32 c) {
    const float d = 1.2f;
    const ImVec2 top[4] = {P(x0, y0), P(x0 + d, y0 - d), P(x1 + d, y0 - d), P(x1, y0)};
    const ImVec2 side[4] = {P(x1, y0), P(x1 + d, y0 - d), P(x1 + d, y1 - d), P(x1, y1)};
    dl->AddConvexPolyFilled(top, 4, shade(c, 1.25f));
    dl->AddConvexPolyFilled(side, 4, shade(c, 0.75f));
    box(x0, y0, x1, y1, c);
  };
  auto bar = [&](bool d3, float x0, float y0, float x1, float y1, ImU32 c) {
    if (d3) box3(x0, y0, x1, y1, c);
    else box(x0, y0, x1, y1, c);
  };
  auto line = [&](const float* xy, int n, ImU32 c, bool markers, bool smooth) {
    std::vector<ImVec2> pts;
    for (int i = 0; i < n; ++i) pts.push_back(P(xy[2 * i], xy[2 * i + 1]));
    if (smooth && n >= 3) {
      dl->PathClear();
      dl->PathLineTo(pts[0]);
      for (int i = 1; i < n; ++i) {
        const ImVec2 m((pts[static_cast<size_t>(i) - 1].x + pts[static_cast<size_t>(i)].x) * 0.5f, pts[static_cast<size_t>(i) - 1].y);
        const ImVec2 m2(m.x, pts[static_cast<size_t>(i)].y);
        dl->PathBezierCubicCurveTo(m, m2, pts[static_cast<size_t>(i)]);
      }
      dl->PathStroke(c, std::max(1.0f, 1.2f * u), 0);
    } else if (n >= 2) {
      dl->AddPolyline(pts.data(), n, c, std::max(1.0f, 1.2f * u), 0);
    }
    if (markers) {
      for (const ImVec2& p : pts) dl->AddCircleFilled(p, std::max(1.5f, 1.1f * u), c);
    }
  };
  auto area = [&](const float* xy, int n, float base, ImU32 c) {
    for (int i = 0; i + 1 < n; ++i) {
      const ImVec2 q[4] = {P(xy[2 * i], base), P(xy[2 * i], xy[2 * i + 1]), P(xy[2 * i + 2], xy[2 * i + 3]), P(xy[2 * i + 2], base)};
      dl->AddConvexPolyFilled(q, 4, c);
    }
  };
  auto axes = [&](bool horizontal) {
    if (horizontal) dl->AddLine(P(1.5f, 1), P(1.5f, 15), axis);
    else dl->AddLine(P(1, 14.5f), P(15, 14.5f), axis);
  };
  auto pie = [&](ImVec2 o, float r, float squash, bool hole) {
    const float shares[3] = {0.45f, 0.3f, 0.25f};
    const ImU32 cols[3] = {c0, c1, c2};
    float a0 = -IM_PI / 2;
    for (int k = 0; k < 3; ++k) {
      const float a1 = a0 + shares[k] * 2 * IM_PI;
      std::vector<ImVec2> pts = {o};
      for (int q = 0; q <= 12; ++q) {
        const float an = a0 + (a1 - a0) * static_cast<float>(q) / 12.0f;
        pts.push_back(ImVec2(o.x + std::cos(an) * r, o.y + std::sin(an) * r * squash));
      }
      dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), cols[k]);
      a0 = a1;
    }
    if (hole) dl->AddCircleFilled(o, r * 0.5f, IM_COL32_WHITE);
  };
  if (combo) {
    axes(false);
    static const float kL[] = {2, 9, 6, 5, 10, 7, 14, 3};
    if (combo == 3) {
      static const float kA[] = {1.5f, 9, 6, 7, 10, 8, 14.5f, 6};
      area(kA, 4, 14.5f, c0);
      box(4, 9, 6, 14.5f, c1), box(10, 6, 12, 14.5f, c1);
      return true;
    }
    box(2.5f, 7, 4.5f, 14.5f, c0), box(6.5f, 10, 8.5f, 14.5f, c0), box(10.5f, 5, 12.5f, 14.5f, c0);
    line(kL, 4, c1, true, false);
    if (combo == 2) dl->AddLine(P(15, 1), P(15, 14.5f), axis);
    return true;
  }
  const bool h = engine::IsHorizontalBar(t) || t == T::BarClustered3D;
  switch (t) {
    case T::ColumnClustered:
    case T::ColumnClustered3D:
    case T::BarClustered:
    case T::BarClustered3D: {
      axes(h);
      const float v[4][2] = {{2, 4}, {4, 6}, {9, 11}, {11, 13}};
      const float hh[4] = {7, 4.5f, 10.5f, 6.5f};
      for (int i = 0; i < 4; ++i) {
        const ImU32 c = i % 2 ? c1 : c0;
        const bool d3 = t == T::ColumnClustered3D || t == T::BarClustered3D;
        if (h) bar(d3, 1.5f, v[i][0], 1.5f + hh[i], v[i][1], c);
        else bar(d3, v[i][0], 14.5f - hh[i], v[i][1], 14.5f, c);
      }
      return true;
    }
    case T::ColumnStacked:
    case T::ColumnStacked100:
    case T::ColumnStacked3D:
    case T::Column3D:
    case T::BarStacked:
    case T::BarStacked100: {
      axes(h);
      const bool full = t == T::ColumnStacked100 || t == T::BarStacked100;
      const float lo[3] = {5, 7, 4}, hi[3] = {3, 4, 5};
      for (int i = 0; i < 3; ++i) {
        const float x0 = 2.5f + i * 4.5f, x1 = x0 + 3;
        const float a1 = full ? 13.0f * lo[i] / (lo[i] + hi[i]) : lo[i], a2 = full ? 13.0f : lo[i] + hi[i];
        const bool d3 = t == T::ColumnStacked3D || t == T::Column3D;
        if (t == T::Column3D) {
          box3(x0, 14.5f - lo[i] - 2, x1, 14.5f, i % 2 ? c1 : c0);
          continue;
        }
        if (h) {
          box(1.5f, x0, 1.5f + a1, x1, c0), box(1.5f + a1, x0, 1.5f + a2, x1, c1);
        } else {
          bar(d3, x0, 14.5f - a1, x1, 14.5f, c0);
          bar(d3, x0, 14.5f - a2, x1, 14.5f - a1, c1);
        }
      }
      return true;
    }
    case T::Line:
    case T::LineMarkers:
    case T::LineStacked:
    case T::LineMarkersStacked:
    case T::LineStacked100:
    case T::LineMarkersStacked100:
    case T::Line3D: {
      axes(false);
      const bool m = t == T::LineMarkers || t == T::LineMarkersStacked || t == T::LineMarkersStacked100;
      const bool full = t == T::LineStacked100 || t == T::LineMarkersStacked100;
      const bool stacked = t == T::LineStacked || t == T::LineMarkersStacked || full;
      const float a0[] = {2, 11, 6, 8, 10, 10, 14, 7};
      const float b0[] = {2, 7, 6, 4, 10, 6, 14, 2};
      const float b1[] = {2, 9, 6, 11, 10, 8, 14, 12};
      const float f[] = {2, 3, 6, 3, 10, 3, 14, 3};
      if (t == T::Line3D) {
        static const float r1[] = {2, 11, 6, 8, 10, 10, 14, 7};
        for (int i = 0; i < 3; ++i) {
          const ImVec2 q[4] = {P(r1[2 * i], r1[2 * i + 1]), P(r1[2 * i] + 1.5f, r1[2 * i + 1] - 1.5f), P(r1[2 * i + 2] + 1.5f, r1[2 * i + 3] - 1.5f),
                               P(r1[2 * i + 2], r1[2 * i + 3])};
          dl->AddConvexPolyFilled(q, 4, c0);
        }
        return true;
      }
      line(a0, 4, c0, m, false);
      line(full ? f : stacked ? b0 : b1, 4, c1, m, false);
      return true;
    }
    case T::Area:
    case T::AreaStacked:
    case T::AreaStacked100:
    case T::Area3D: {
      axes(false);
      const float back[] = {1.5f, 6, 6, 8, 10, 4, 14.5f, 7};
      const float front[] = {1.5f, 11, 6, 9, 10, 12, 14.5f, 10};
      const float full[] = {1.5f, 2, 6, 2, 10, 2, 14.5f, 2};
      if (t == T::Area) {
        area(back, 4, 14.5f, c1);
        area(front, 4, 14.5f, c0);
      } else {
        area(t == T::AreaStacked100 ? full : back, 4, 14.5f, c1);
        area(front, 4, 14.5f, c0);
        if (t == T::Area3D) dl->AddLine(P(1.5f, 6), P(3, 4.5f), axis);
      }
      return true;
    }
    case T::Pie: pie(P(8, 8), 6.5f * u, 1.0f, false); return true;
    case T::Pie3D: pie(P(8, 8.5f), 7.0f * u, 0.6f, false); return true;
    case T::Doughnut: pie(P(8, 8), 6.5f * u, 1.0f, true); return true;
    case T::PieOfPie:
    case T::BarOfPie: {
      pie(P(5.5f, 8), 4.8f * u, 1.0f, false);
      dl->AddLine(P(8, 5), P(11.5f, 4), axis), dl->AddLine(P(8, 11), P(11.5f, 12), axis);
      if (t == T::PieOfPie) dl->AddCircleFilled(P(13.2f, 8), 2.6f * u, c2);
      else box(12, 3, 15, 13, c2), box(12, 3, 15, 7, c1);
      return true;
    }
    case T::Scatter:
    case T::ScatterSmooth:
    case T::ScatterSmoothNoMarkers:
    case T::ScatterLines:
    case T::ScatterLinesNoMarkers: {
      axes(false);
      dl->AddLine(P(1.5f, 1), P(1.5f, 14.5f), axis);
      const float s0[] = {3, 12, 6, 8, 9, 10, 13, 4};
      const float s1[] = {3, 9, 7, 11, 10, 6, 14, 7};
      const bool lines = t != T::Scatter, markers = t == T::Scatter || t == T::ScatterSmooth || t == T::ScatterLines;
      const bool smooth = t == T::ScatterSmooth || t == T::ScatterSmoothNoMarkers;
      if (lines) {
        line(s0, 4, c0, markers, smooth);
        line(s1, 4, c1, markers, smooth);
      } else {
        for (int i = 0; i < 4; ++i) dl->AddCircleFilled(P(s0[2 * i], s0[2 * i + 1]), 1.2f * u, c0), dl->AddCircleFilled(P(s1[2 * i], s1[2 * i + 1]), 1.2f * u, c1);
      }
      return true;
    }
    case T::Bubble: {
      axes(false);
      dl->AddLine(P(1.5f, 1), P(1.5f, 14.5f), axis);
      dl->AddCircleFilled(P(5, 10), 2.5f * u, (c0 & 0x00FFFFFF) | 0xC0000000);
      dl->AddCircleFilled(P(10, 6), 3.5f * u, (c1 & 0x00FFFFFF) | 0xC0000000);
      dl->AddCircleFilled(P(12.5f, 11.5f), 1.6f * u, (c0 & 0x00FFFFFF) | 0xC0000000);
      return true;
    }
    case T::StockHLC:
    case T::StockOHLC:
    case T::StockVHLC:
    case T::StockVOHLC: {
      axes(false);
      const bool vol = t == T::StockVHLC || t == T::StockVOHLC, candles = t == T::StockOHLC || t == T::StockVOHLC;
      if (vol) box(3, 11, 5, 14.5f, c2), box(7.5f, 12, 9.5f, 14.5f, c2), box(12, 10.5f, 14, 14.5f, c2);
      const float x[3] = {4, 8.5f, 13}, hi[3] = {2, 4, 3}, lo[3] = {9, 10, 8}, op[3] = {4, 8, 4}, cl[3] = {7, 5, 6};
      for (int i = 0; i < 3; ++i) {
        dl->AddLine(P(x[i], hi[i]), P(x[i], lo[i]), IM_COL32(0x40, 0x40, 0x40, 255), std::max(1.0f, u));
        if (candles) box(x[i] - 1.2f, std::min(op[i], cl[i]), x[i] + 1.2f, std::max(op[i], cl[i]), op[i] > cl[i] ? IM_COL32_WHITE : IM_COL32(0x40, 0x40, 0x40, 255));
        else dl->AddLine(P(x[i], cl[i]), P(x[i] + 1.5f, cl[i]), IM_COL32(0x40, 0x40, 0x40, 255), std::max(1.0f, u));
      }
      return true;
    }
    case T::Radar:
    case T::RadarMarkers:
    case T::RadarFilled: {
      ImVec2 web[6], s0[6];
      const float r0[6] = {6, 4, 5.5f, 3.5f, 5, 4.5f};
      for (int i = 0; i < 6; ++i) {
        const float an = -IM_PI / 2 + static_cast<float>(i) * IM_PI / 3;
        web[i] = P(8 + std::cos(an) * 6.5f, 8 + std::sin(an) * 6.5f);
        s0[i] = P(8 + std::cos(an) * r0[i], 8 + std::sin(an) * r0[i]);
      }
      dl->AddPolyline(web, 6, axis, 1.0f, ImDrawFlags_Closed);
      if (t == T::RadarFilled) dl->AddConvexPolyFilled(s0, 6, (c0 & 0x00FFFFFF) | 0xC0000000);
      else dl->AddPolyline(s0, 6, c0, std::max(1.0f, 1.2f * u), ImDrawFlags_Closed);
      if (t == T::RadarMarkers) {
        for (const ImVec2& p : s0) dl->AddCircleFilled(p, std::max(1.5f, 1.1f * u), c0);
      }
      return true;
    }
    default: return false;
  }
}

void DrawChartPresetPicture(ImDrawList* dl, ImVec2 a, ImVec2 b, int kind, int n, ChartType type = ChartType::ColumnClustered) {
  ChartObj c;
  c.type = kind == 2 ? type : ChartType::ColumnClustered;
  c.series.resize(2);
  c.has_title = true;
  c.val_axis.gridlines = true;
  if (kind == 1) engine::ApplyChartQuickLayout(c, n);
  else engine::ApplyChartStyle(c, n);
  const float w = b.x - a.x, h = b.y - a.y, u = std::max(1.0f, h / 40.0f);
  dl->AddRectFilled(a, b, c.chart_fill ? ArgbCol(c.chart_fill) : IM_COL32_WHITE);
  dl->AddRect(a, b, IM_COL32(0xBF, 0xBF, 0xBF, 255));
  const ImU32 ink = c.title_font.color ? ArgbCol(c.title_font.color) : IM_COL32(0x7F, 0x7F, 0x7F, 255);
  float L = a.x + 4 * u, T = a.y + 3 * u, R = b.x - 4 * u, B = b.y - 3 * u;
  if (c.has_title) {
    dl->AddRectFilled(ImVec2(a.x + w * 0.32f, T), ImVec2(a.x + w * 0.68f, T + 2.5f * u), ink);
    T += 5 * u;
  }
  // the legend: two keys
  auto keys = [&](float x, float y, bool vertical) {
    for (int i = 0; i < 2; ++i) {
      const float kx = vertical ? x : x + i * 9 * u, ky = vertical ? y + i * 4 * u : y;
      dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + 2 * u, ky + 2 * u), engine::ChartPalette(i));
      dl->AddRectFilled(ImVec2(kx + 3 * u, ky + 0.5f * u), ImVec2(kx + 7 * u, ky + 1.5f * u), ink);
    }
  };
  if (c.has_legend) {
    switch (c.legend_pos) {
      case 0: keys(a.x + w / 2 - 8 * u, B - 2 * u, false), B -= 4 * u; break;
      case 1: keys(R - 7 * u, (T + B) / 2 - 3 * u, true), R -= 9 * u; break;
      case 2: keys(a.x + w / 2 - 8 * u, T, false), T += 4 * u; break;
      case 3: keys(L, (T + B) / 2 - 3 * u, true), L += 9 * u; break;
      default: keys(R - 7 * u, T, true), R -= 9 * u; break;
    }
  }
  if (c.val_axis.has_title) dl->AddRectFilled(ImVec2(L, (T + B) / 2 - 4 * u), ImVec2(L + 1.5f * u, (T + B) / 2 + 4 * u), ink), L += 3 * u;
  if (c.cat_axis.has_title) dl->AddRectFilled(ImVec2((L + R) / 2 - 5 * u, B - 1.5f * u), ImVec2((L + R) / 2 + 5 * u, B), ink), B -= 3 * u;
  if (c.val_axis.visible) {
    for (int i = 0; i < 3; ++i) dl->AddRectFilled(ImVec2(L, T + 1 * u + i * (B - T - 2 * u) / 2), ImVec2(L + 2.5f * u, T + 1.8f * u + i * (B - T - 2 * u) / 2), ink);
    L += 4 * u;
  }
  if (c.plot_fill) dl->AddRectFilled(ImVec2(L, T), ImVec2(R, B), ArgbCol(c.plot_fill));
  const ImU32 grid = c.chart_fill == 0xFF404040 ? IM_COL32(0x70, 0x70, 0x70, 255) : IM_COL32(0xD9, 0xD9, 0xD9, 255);
  if (c.val_axis.gridlines) {
    for (int i = 0; i < 3; ++i) dl->AddLine(ImVec2(L, T + 1.4f * u + i * (B - T - 2 * u) / 2), ImVec2(R, T + 1.4f * u + i * (B - T - 2 * u) / 2), grid);
  }
  if (c.val_axis.minor_gridlines) {
    for (int i = 0; i < 2; ++i) dl->AddLine(ImVec2(L, T + 1.4f * u + (i + 0.5f) * (B - T - 2 * u) / 2), ImVec2(R, T + 1.4f * u + (i + 0.5f) * (B - T - 2 * u) / 2), grid);
  }
  dl->AddLine(ImVec2(L, B), ImVec2(R, B), grid);
  const ImU32 pal[2] = {engine::ChartPalette(0), engine::ChartPalette(1)};
  auto filled = [&](int i, ImU32 col) {
    const int alpha = 255 - 255 * std::clamp(c.series[static_cast<size_t>(i)].transparency, 0, 100) / 100;
    return (col & 0x00FFFFFF) | (static_cast<ImU32>(alpha) << 24);
  };
  const auto& s0 = c.series[0];
  // a series' fill as the style paints it: solid, a gradient (lighter at the end), a pattern (stripes), or none
  auto paint = [&](int i, ImVec2 p0, ImVec2 p1) {
    const auto& se = c.series[static_cast<size_t>(i)];
    const ImU32 col = filled(i, pal[i]);
    if (!se.no_fill) {
      if (se.fill_type == 1) {
        dl->AddRectFilledMultiColor(p0, p1, col, col, (col & 0x00FFFFFF) | 0x50000000, (col & 0x00FFFFFF) | 0x50000000);
      } else if (se.fill_type == 3) {
        dl->AddRectFilled(p0, p1, IM_COL32_WHITE);
        dl->PushClipRect(p0, p1, true);
        for (float x = p0.x - (p1.y - p0.y); x < p1.x; x += 3 * u) dl->AddLine(ImVec2(x, p1.y), ImVec2(x + (p1.y - p0.y), p0.y), col, std::max(1.0f, u * 0.8f));
        dl->PopClipRect();
      } else {
        dl->AddRectFilled(p0, p1, col);
      }
    }
    if (se.line_color) dl->AddRect(p0, p1, se.no_fill ? pal[i] : IM_COL32(0x40, 0x40, 0x40, 255), 0.0f, se.no_fill ? 1.5f * u : 1.0f);
  };
  const bool lines = engine::IsLineType(c.type) || c.type == ChartType::Line3D || c.type == ChartType::Radar || c.type == ChartType::RadarMarkers;
  const bool pies = c.type == ChartType::Pie || c.type == ChartType::Pie3D || c.type == ChartType::PieOfPie || c.type == ChartType::BarOfPie ||
                    c.type == ChartType::Doughnut;
  const bool areas = engine::IsAreaType(c.type) || c.type == ChartType::Area3D || c.type == ChartType::RadarFilled;
  const bool dots = engine::IsScatterType(c.type);
  const bool hbars = engine::IsHorizontalBar(c.type) || c.type == ChartType::BarClustered3D;
  static const float kY[2][4] = {{0.35f, 0.6f, 0.45f, 0.85f}, {0.2f, 0.35f, 0.55f, 0.5f}};
  if (pies) {
    const ImVec2 o((L + R) / 2, (T + B) / 2);
    const float rad = std::min(R - L, B - T) * 0.45f;
    static const float kShare[4] = {0.4f, 0.25f, 0.2f, 0.15f};
    float a0 = -IM_PI / 2;
    for (int k = 0; k < 4; ++k) {
      const float a1 = a0 + kShare[k] * 2 * IM_PI, mid = (a0 + a1) / 2;
      const ImVec2 oc = s0.explosion > 0 ? o + ImVec2(std::cos(mid), std::sin(mid)) * rad * 0.12f : o;
      std::vector<ImVec2> pts = {oc};
      for (int q = 0; q <= 16; ++q) pts.push_back(oc + ImVec2(std::cos(a0 + (a1 - a0) * q / 16), std::sin(a0 + (a1 - a0) * q / 16)) * rad);
      const ImU32 col = (engine::ChartPalette(static_cast<size_t>(k)) & 0x00FFFFFF) | (static_cast<ImU32>(255 - 255 * std::clamp(s0.transparency, 0, 100) / 100) << 24);
      dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), s0.fill_type == 1 ? (col & 0x00FFFFFF) | 0xB0000000 : col);
      if (s0.line_color) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32_WHITE, std::max(1.0f, s0.line_weight > 0 ? u * 1.5f : u * 0.7f), ImDrawFlags_Closed);
      if (s0.data_labels) dl->AddRectFilled(oc + ImVec2(std::cos(mid), std::sin(mid)) * rad * 0.6f - ImVec2(u, 0.6f * u), oc + ImVec2(std::cos(mid), std::sin(mid)) * rad * 0.6f + ImVec2(u, 0.6f * u), IM_COL32_WHITE);
      a0 = a1;
    }
    if (c.type == ChartType::Doughnut) dl->AddCircleFilled(o, rad * 0.5f, c.chart_fill ? ArgbCol(c.chart_fill) : IM_COL32_WHITE);
    return;
  }
  if (lines || areas || dots) {
    for (int i = areas ? 1 : 0; areas ? i >= 0 : i < 2; areas ? --i : ++i) {
      ImVec2 pts[4];
      for (int k = 0; k < 4; ++k) pts[k] = ImVec2(L + (R - L) * (k + 0.5f) / 4, B - (B - T - 3 * u) * kY[i][k]);
      const auto& se = c.series[static_cast<size_t>(i)];
      if (areas) {
        std::vector<ImVec2> poly(pts, pts + 4);
        poly.insert(poly.begin(), ImVec2(pts[0].x, B));
        poly.push_back(ImVec2(pts[3].x, B));
        const ImU32 col = filled(i, pal[i]);
        for (int k = 0; k < 3; ++k) {
          const ImVec2 q[4] = {ImVec2(pts[k].x, B), pts[k], pts[k + 1], ImVec2(pts[k + 1].x, B)};
          dl->AddConvexPolyFilled(q, 4, se.fill_type == 1 ? (col & 0x00FFFFFF) | 0xA0000000 : col);
        }
        if (se.fill_type == 3) {
          for (int k = 0; k < 4; ++k) dl->AddLine(ImVec2(pts[k].x, B), pts[k], IM_COL32_WHITE, u * 0.6f);
        }
        if (se.line_color) dl->AddPolyline(pts, 4, IM_COL32_WHITE, u, 0);
      } else if (lines) {
        const float lw = (se.line_weight > 0 ? se.line_weight : 2.0f) * 0.6f * u;
        if (se.dash) {
          for (int k = 0; k < 3; ++k) {
            for (float f = 0; f < 1; f += 0.34f) dl->AddLine(pts[k] + (pts[k + 1] - pts[k]) * f, pts[k] + (pts[k + 1] - pts[k]) * std::min(1.0f, f + 0.2f), pal[i], lw);
          }
        } else {
          dl->AddPolyline(pts, 4, pal[i], lw, 0);
        }
      }
      const bool mark = dots || se.marker > 0;
      for (int k = 0; k < 4 && mark; ++k) {
        const float r = std::max(1.2f * u, (se.marker_size > 0 ? se.marker_size : 5.0f) * 0.25f * u);
        if (se.marker == 2) dl->AddRectFilled(pts[k] - ImVec2(r, r), pts[k] + ImVec2(r, r), pal[i]);
        else if (se.marker == 3) dl->AddQuadFilled(pts[k] - ImVec2(0, r), pts[k] + ImVec2(r, 0), pts[k] + ImVec2(0, r), pts[k] - ImVec2(r, 0), pal[i]);
        else if (se.marker == 4) dl->AddTriangleFilled(pts[k] - ImVec2(0, r), pts[k] + ImVec2(r, r), pts[k] + ImVec2(-r, r), pal[i]);
        else dl->AddCircleFilled(pts[k], r, pal[i]);
      }
      for (int k = 0; k < 4 && se.data_labels; ++k) dl->AddRectFilled(pts[k] + ImVec2(-1.2f * u, -3 * u), pts[k] + ImVec2(1.2f * u, -1.6f * u), ink);
    }
    return;
  }
  if (hbars) {  // bars: two series across three categories, from the left
    const float slot = (B - T) / 3, bar = slot / (2 + std::clamp(c.gap_width, 0, 500) / 100.0f);
    for (int k = 0; k < 3; ++k) {
      for (int i = 0; i < 2; ++i) {
        const float y0 = T + slot * k + (slot - 2 * bar) / 2 + i * bar, right = L + (R - L - 3 * u) * kY[i][k + 1];
        paint(i, ImVec2(L, y0), ImVec2(right, y0 + bar));
        if (c.series[static_cast<size_t>(i)].data_labels) dl->AddRectFilled(ImVec2(right - 3 * u, y0 + bar / 2 - 0.6f * u), ImVec2(right - 1 * u, y0 + bar / 2 + 0.6f * u), IM_COL32_WHITE);
      }
    }
    return;
  }
  // three categories of two columns; the gap width sets how wide they are
  static const float kH[3][2] = {{0.55f, 0.35f}, {0.75f, 0.5f}, {0.95f, 0.65f}};
  const float slot = (R - L) / 3, col = slot / (2 + std::clamp(c.gap_width, 0, 500) / 100.0f);
  const int alpha = 255 - 255 * std::clamp(c.series[0].transparency, 0, 100) / 100;
  for (int k = 0; k < 3; ++k) {
    for (int i = 0; i < 2; ++i) {
      const float x0 = L + slot * k + (slot - 2 * col) / 2 + i * col, top = B - (B - T - 3 * u) * kH[k][i];
      (void)alpha;
      paint(i, ImVec2(x0, top), ImVec2(x0 + col, B));
      if (c.series[i].data_labels) {
        const float ly = c.series[i].label_pos == 1 ? top + 1 * u : top - 2.5f * u;
        dl->AddRectFilled(ImVec2(x0 + col / 2 - 1.2f * u, ly), ImVec2(x0 + col / 2 + 1.2f * u, ly + 1.4f * u), c.series[i].label_pos == 1 ? IM_COL32_WHITE : ink);
      }
    }
  }
}

void RegisterChartCommands(AppState& s) {
  auto& R = ui::Commands();
  auto add = [&R](const char* id, const char* label, const char* ic, const char* desc, bool toggle = false) -> ui::Command& {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.tip_desc = desc;
    c.toggle = toggle;
    return R.Add(std::move(c));
  };
  using E = model::ChartElem;
  add("ChartAddElementMenu", TR("Add Chart\nElement"), ui::icon::ChartColumn, TR("Add, remove or position chart elements: title, legend, axes, gridlines, data labels.")).execute = [](ui::Command&) {};
  add("ChartElemTitle", TR("Chart Title"), nullptr, TR("Show or hide the chart title."), true).execute = [&s](ui::Command&) {
    EditSelectedChart(s, TR("Chart Title"), [](ChartObj& c) { c.has_title = !c.has_title; });
  };
  // a chart element set one way: Chart Title > None / Above Chart / Centered Overlay, Data Labels > Center..., Error
  // Bars, Gridlines, Trendline (each a command of Add Chart Element's submenus, as in Excel)
  auto elem = [&](const char* id, const char* label, const char* desc, std::function<void(ChartObj&)> fn, std::function<bool(const ChartObj&)> on) {
    add(id, label, nullptr, desc, true).execute = [&s, fn, label](ui::Command&) { EditSelectedChart(s, label, fn); };
    ChartElemStates().push_back({id, std::move(on)});
  };
  elem("ChartElemTitleNone", TR("Chart Title: None"), TR("Hide the chart title."), [](ChartObj& c) { c.has_title = false; }, [](const ChartObj& c) { return !c.has_title; });
  elem("ChartElemTitleAbove", TR("Chart Title: Above Chart"), TR("The title above the plot area."), [](ChartObj& c) { c.has_title = true, c.title_overlay = false, c.title_rect = {}; },
       [](const ChartObj& c) { return c.has_title && !c.title_overlay; });
  elem("ChartElemTitleOverlay", TR("Chart Title: Centered Overlay"), TR("The title over the plot area, which keeps its size."),
       [](ChartObj& c) { c.has_title = true, c.title_overlay = true, c.title_rect = {}; }, [](const ChartObj& c) { return c.has_title && c.title_overlay; });
  auto labels = [&](const char* id, const char* label, int pos) {
    elem(id, label, TR("Show the data labels there (or hide them)."),
         [pos](ChartObj& c) {
           for (auto& se : c.series) se.data_labels = pos >= -1, se.label_pos = pos >= 0 ? pos : se.label_pos;
         },
         [pos](const ChartObj& c) {
           bool all = !c.series.empty();
           for (const auto& se : c.series) all = all && (pos == -2 ? !se.data_labels : se.data_labels && se.label_pos == pos);
           return all;
         });
  };
  labels("ChartElemLabelsNone", TR("Data Labels: None"), -2);
  labels("ChartElemLabelsCenter", TR("Data Labels: Center"), 0);
  labels("ChartElemLabelsInsideEnd", TR("Data Labels: Inside End"), 1);
  labels("ChartElemLabelsInsideBase", TR("Data Labels: Inside Base"), 2);
  labels("ChartElemLabelsOutsideEnd", TR("Data Labels: Outside End"), 3);
  auto errs = [&](const char* id, const char* label, model::ErrorBarKind k, double amount) {
    elem(id, label, TR("Error bars on every series."),
         [k, amount](ChartObj& c) {
           for (auto& se : c.series) se.err = k, se.err_amount = amount;
         },
         [k](const ChartObj& c) {
           bool all = !c.series.empty();
           for (const auto& se : c.series) all = all && se.err == k;
           return all;
         });
  };
  errs("ChartElemErrNone", TR("Error Bars: None"), model::ErrorBarKind::None, 1);
  errs("ChartElemErrStdErr", TR("Error Bars: Standard Error"), model::ErrorBarKind::StError, 1);
  errs("ChartElemErrPercent", TR("Error Bars: Percentage"), model::ErrorBarKind::Percent, 5);
  errs("ChartElemErrStdDev", TR("Error Bars: Standard Deviation"), model::ErrorBarKind::StDev, 1);
  elem("ChartElemGridValMinor", TR("Gridlines: Primary Minor Horizontal"), TR("Show or hide the value axis' minor gridlines."),
       [](ChartObj& c) { c.val_axis.minor_gridlines = !c.val_axis.minor_gridlines; }, [](const ChartObj& c) { return c.val_axis.minor_gridlines; });
  elem("ChartElemGridCatMinor", TR("Gridlines: Primary Minor Vertical"), TR("Show or hide the category axis' minor gridlines."),
       [](ChartObj& c) { c.cat_axis.minor_gridlines = !c.cat_axis.minor_gridlines; }, [](const ChartObj& c) { return c.cat_axis.minor_gridlines; });
  elem("ChartElemAxisVal2", TR("Axis: Secondary Vertical"), TR("Show or hide the secondary vertical axis."), [](ChartObj& c) { c.val_axis2.visible = !c.val_axis2.visible; },
       [](const ChartObj& c) { return c.val_axis2.visible; });
  elem("ChartElemAxisTitleVal2", TR("Axis Title: Secondary Vertical"), TR("Show or hide the secondary vertical axis' title."),
       [](ChartObj& c) { c.val_axis2.has_title = !c.val_axis2.has_title; }, [](const ChartObj& c) { return c.val_axis2.has_title; });
  auto trend = [&](const char* id, const char* label, model::TrendKind k, double forward) {
    elem(id, label, TR("A trendline on every series."),
         [k, forward](ChartObj& c) {
           for (auto& se : c.series) se.trend = k, se.trend_forward = forward, se.trend_period = 2;
         },
         [k](const ChartObj& c) {
           bool all = !c.series.empty();
           for (const auto& se : c.series) all = all && se.trend == k;
           return all;
         });
  };
  trend("ChartElemTrendNone", TR("Trendline: None"), model::TrendKind::None, 0);
  trend("ChartElemTrendLinear", TR("Trendline: Linear"), model::TrendKind::Linear, 0);
  trend("ChartElemTrendExp", TR("Trendline: Exponential"), model::TrendKind::Exponential, 0);
  trend("ChartElemTrendForecast", TR("Trendline: Linear Forecast"), model::TrendKind::Linear, 2);
  trend("ChartElemTrendMovingAvg", TR("Trendline: Moving Average"), model::TrendKind::MovingAverage, 0);
  // a stock chart's Lines (High-Low Lines) and Up/Down Bars
  elem("ChartElemLinesNone", TR("Lines: None"), TR("Hide the stock chart's high-low lines."), [](ChartObj& c) { c.hi_lo_lines = false; },
       [](const ChartObj& c) { return engine::IsStockType(c.type) && !c.hi_lo_lines; });
  elem("ChartElemHiLoLines", TR("Lines: High-Low Lines"), TR("Show the stock chart's high-low lines."), [](ChartObj& c) { c.hi_lo_lines = true; },
       [](const ChartObj& c) { return engine::IsStockType(c.type) && c.hi_lo_lines; });
  elem("ChartElemUpDownNone", TR("Up/Down Bars: None"), TR("Hide the stock chart's up / down bars."), [](ChartObj& c) { c.up_down_bars = false; },
       [](const ChartObj& c) { return engine::IsStockType(c.type) && !c.up_down_bars; });
  elem("ChartElemUpDownBars", TR("Up/Down Bars: Up/Down Bars"), TR("Show bars from the open to the close of each day."), [](ChartObj& c) { c.up_down_bars = true; },
       [](const ChartObj& c) { return engine::IsStockType(c.type) && c.up_down_bars; });
  // Change Colors: Excel's colorful and monochromatic palettes
  add("ChartChangeColorsMenu", TR("Change\nColors"), ui::icon::ChartColumn, TR("Choose the chart's colors: a colorful palette or shades of one color.")).execute = [](ui::Command&) {};
  for (int k = 0; k < engine::kChartColorSchemes; ++k) {
    static std::vector<std::string> ids;
    ids.push_back("ChartColors" + std::to_string(k));
    elem(ids.back().c_str(), engine::ChartColorSchemeName(k), TR("Color the chart's series with this palette."), [k](ChartObj& c) { c.color_scheme = k; },
         [k](const ChartObj& c) { return c.color_scheme == k; });
  }
  // Quick Layout and Chart Styles: Excel's galleries, each choice shown as the chart it makes
  add("ChartQuickLayoutMenu", TR("Quick\nLayout"), ui::icon::ChartColumn, TR("Change the overall layout of the chart: which elements show and where.")).execute =
      [](ui::Command&) {};
  add("ChartStylesMenu", TR("Chart\nStyles"), ui::icon::ChartColumn, TR("Change the chart's look: fills, gridlines, labels and background together.")).execute =
      [](ui::Command&) {};
  for (int k = 1; k <= engine::kChartQuickLayouts; ++k) {
    static std::vector<std::string> ids, names;  // kept: the commands point at them
    ids.reserve(engine::kChartQuickLayouts), names.reserve(engine::kChartQuickLayouts);
    char buf[64];
    std::snprintf(buf, sizeof buf, TR("Layout %d"), k);
    ids.push_back("ChartQuickLayout" + std::to_string(k));
    names.push_back(buf);
    add(ids.back().c_str(), names.back().c_str(), nullptr, engine::ChartQuickLayoutTip(k)).execute = [&s, k](ui::Command&) {
      EditSelectedChart(s, TR("Quick Layout"), [k](ChartObj& c) { engine::ApplyChartQuickLayout(c, k); });
    };
  }
  for (int k = 1; k <= engine::kChartStyles; ++k) {
    static std::vector<std::string> ids, names;  // kept: the commands point at them
    ids.reserve(engine::kChartStyles), names.reserve(engine::kChartStyles);
    char buf[64];
    std::snprintf(buf, sizeof buf, TR("Style %d"), k);
    ids.push_back("ChartStyle" + std::to_string(k));
    names.push_back(buf);
    elem(ids.back().c_str(), names.back().c_str(), engine::ChartStyleTip(k), [k](ChartObj& c) { engine::ApplyChartStyle(c, k); },
         [k](const ChartObj& c) { return std::max(1, c.style) == k; });
  }
  {
    auto& st = ui::ribbon::St();
    auto preset = [](const std::string& cmd, int* kind) {
      if (cmd.rfind("ChartQuickLayout", 0) == 0 && cmd.size() > 16 && std::isdigit(static_cast<unsigned char>(cmd[16]))) return *kind = 1, std::atoi(cmd.c_str() + 16);
      if (cmd.rfind("ChartStyle", 0) == 0 && cmd.size() > 10 && std::isdigit(static_cast<unsigned char>(cmd[10]))) return *kind = 2, std::atoi(cmd.c_str() + 10);
      return 0;
    };
    st.menu_picture = [prev = st.menu_picture, preset](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
      // Insert > Charts' galleries: a picture of each kind of chart
      const int combo = cmd == "InsertChartCombo" ? 1 : cmd == "InsertChartComboSecondary" ? 2 : cmd == "InsertChartComboArea" ? 3 : 0;
      const auto gt = GalleryChartTypes().find(cmd);
      if (combo || gt != GalleryChartTypes().end()) {
        if (!dl) return h;
        if (DrawChartTypePictogram(dl, ImVec2(at.x + S(2), at.y + S(2)), h - S(4), combo ? ChartType::ColumnClustered : gt->second, combo)) return h;
        return prev ? prev(cmd, dl, at, h) : 0.0f;
      }
      int kind = 0;
      const int n = preset(cmd, &kind);
      if (n <= 0) return prev ? prev(cmd, dl, at, h) : 0.0f;
      const float w = (h - S(8)) * 1.5f;
      if (dl) DrawChartPresetPicture(dl, ImVec2(at.x, at.y + S(4)), ImVec2(at.x + w, at.y + h - S(4)), kind, n, StyleChartType());
      return w;
    };
    // the chart galleries' tiles are larger, as Excel's
    st.menu_tile_size = [prev = st.menu_tile_size](const std::string& first) -> float {
      if (first.rfind("InsertChart", 0) == 0) return 40.0f;
      return prev ? prev(first) : 0.0f;
    };
    // the Chart Styles gallery lists the selected chart type's set (Excel: 10-15 styles per kind of chart)
    st.menu_entry_shown = [prev = st.menu_entry_shown, preset](const std::string& cmd) -> bool {
      int kind = 0;
      const int n = preset(cmd, &kind);
      if (n > 0 && kind == 2) return n <= engine::ChartStyleCount(StyleChartType());
      return prev ? prev(cmd) : true;
    };
    st.menu_entry_height = [prev = st.menu_entry_height, preset](const std::string& cmd) -> float {
      int kind = 0;
      if (preset(cmd, &kind) > 0) return S(46);  // eleven layouts still fit a 768-pixel screen
      return prev ? prev(cmd) : 0.0f;
    };
  }
  auto legend = [&](const char* id, const char* label, int pos) {
    add(id, label, nullptr, TR("Show the legend there (or hide it)."), true).execute = [&s, pos](ui::Command&) {
      EditSelectedChart(s, TR("Legend"), [pos](ChartObj& c) {
        c.has_legend = pos >= 0;
        if (pos >= 0) c.legend_pos = pos;
      });
    };
  };
  legend("ChartElemLegendNone", TR("Legend: None"), -1);
  legend("ChartElemLegendRight", TR("Legend: Right"), 1);
  legend("ChartElemLegendTop", TR("Legend: Top"), 2);
  legend("ChartElemLegendLeft", TR("Legend: Left"), 3);
  legend("ChartElemLegendBottom", TR("Legend: Bottom"), 0);
  legend("ChartElemLegendCorner", TR("Legend: Top Right"), 4);
  auto axis = [&](const char* id, const char* label, bool value, int what) {
    add(id, label, nullptr, "", true).execute = [&s, value, what](ui::Command&) {
      EditSelectedChart(s, TR("Axes"), [value, what](ChartObj& c) {
        auto& a = value ? c.val_axis : c.cat_axis;
        if (what == 0) a.visible = !a.visible;
        if (what == 1) {
          a.has_title = !a.has_title;
          if (a.has_title && a.title.empty()) a.title = TR("Axis Title");
        }
        if (what == 2) a.gridlines = !a.gridlines;
      });
    };
  };
  axis("ChartElemAxisCat", TR("Axis: Primary Horizontal"), false, 0);
  axis("ChartElemAxisVal", TR("Axis: Primary Vertical"), true, 0);
  axis("ChartElemAxisTitleCat", TR("Axis Title: Primary Horizontal"), false, 1);
  axis("ChartElemAxisTitleVal", TR("Axis Title: Primary Vertical"), true, 1);
  axis("ChartElemGridCat", TR("Gridlines: Primary Major Vertical"), false, 2);
  axis("ChartElemGridVal", TR("Gridlines: Primary Major Horizontal"), true, 2);
  add("ChartElemDataLabels", TR("Data Labels"), nullptr, TR("Show or hide every point's value."), true).execute = [&s](ui::Command&) {
    EditSelectedChart(s, TR("Data Labels"), [](ChartObj& c) {
      bool all = !c.series.empty();
      for (const auto& se : c.series) all = all && se.data_labels;
      for (auto& se : c.series) se.data_labels = !all;
    });
  };
  add("ChartSwitchRowCol", TR("Switch Row/\nColumn"), ui::icon::Rows, TR("Swap the data: series in rows become series in columns.")).execute = [&s](ui::Command&) {
    ObjSel o;
    const ChartObj* ch = SelectedChart(s, &o);
    if (!ch) return;
    uint32_t sid = 0;
    std::vector<Rect> areas;
    const model::Sheet* src = nullptr;
    if (ch->source.empty() || !ResolveAreasText(s.snap->wb, o.sheet, ch->source, &sid, &areas) || !(src = s.snap->Sheet(sid))) {
      s.SetStatus(TR("This chart's data range isn't known: set it with Select Data or Python's SetSourceData"));
      return;
    }
    ChartObj tmp = *ch;
    if (!BuildSeriesFromAreas(s.snap->wb, *src, areas, ch->type, &tmp, !ch->source_by_rows)) return;
    KeepSeriesFormat(ch->series, &tmp.series);
    s.engine->PostUser(TR("Switch Row/Column"), [o, tmp](Document& d) {
      d.UpdateChart(o.sheet, o.id, [&](ChartObj& x) {
        x.series = tmp.series;
        x.source = tmp.source;
        x.source_by_rows = tmp.source_by_rows;
      });
    });
  };
  add("ChartSelectData", TR("Select\nData"), ui::icon::SelectData, TR("Change the data range in the chart: select the cells on the sheet.")).execute = [&s](ui::Command&) {
    ObjSel o;
    if (!SelectedChart(s, &o)) return;
    OpenSelectData(s, o.sheet, o.id);
  };
  add("ChartChangeTypeMenu", TR("Change\nChart Type"), ui::icon::ChartCombo, TR("Change to a different type of chart: every type with a preview, and Combo for each series.")).execute = [&s](ui::Command&) {
    OpenChartTypeDialog(s, true);
  };
  for (int i = 0; i < model::kChartTypeCount; ++i) {
    static std::vector<std::string> ids;
    ids.push_back("ChartType" + std::to_string(i));
    add(ids.back().c_str(), ChartTypeNames()[i], nullptr, "", true).execute = [&s, i](ui::Command&) {
      ObjSel o;
      if (!SelectedChart(s, &o)) return;
      s.engine->PostUser(TR("Change Chart Type"), [o, i](Document& d) {
        d.UpdateChart(o.sheet, o.id, [&d, i](ChartObj& c) { ChangeChartType(d.wb(), c, static_cast<ChartType>(i)); });
      });
    };
  }
  add("MoveChart", TR("Move\nChart"), ui::icon::Chart, TR("Move this chart to another sheet or to its own chart sheet.")).execute = [&s](ui::Command&) {
    ObjSel o;
    if (!SelectedChart(s, &o)) return;
    g_move = o;
    s.OpenDialog(Dialog::MoveChart);
  };
  add("InsertChartSheet", TR("Chart Sheet"), ui::icon::Chart, TR("Chart the selected data on a new chart sheet.")).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh || sh->chart_sheet) return;
    ChartObj ch;
    std::vector<Rect> areas = SelAreas(s);
    if (areas.size() <= 1) areas = {ExpandToRegion(*sh, SelRect(s))};
    if (!BuildSeriesFromAreas(s.snap->wb, *sh, areas, ChartType::ColumnClustered, &ch, PlotByRows(areas))) {
      s.SetStatus(TR("The selection has no data that can be charted"));
      return;
    }
    const uint32_t before = sh->id;
    const uint32_t id = s.engine->Call([&](Document& d) {
      d.Begin(TR("Insert Chart Sheet"));
      const uint32_t v = d.AddChartSheet({}, d.wb().IndexOfId(before), ch);  // like Excel: before the active sheet
      d.Commit();
      return v;
    });
    if (!id) return;
    s.engine->Sync();
    s.snap = s.engine->Current();
    ActivateSheet(s, id);
  };
  add("InsertChartObject", TR("Insert Chart"), ui::icon::ChartColumn, TR("Chart the selected data next to it (Alt+F1).")).execute = [&s](ui::Command&) {
    InsertChartFromSelection(s, ChartType::ColumnClustered);
  };
  // Format tab
  add("ChartElementsMenu", TR("Chart\nElements"), ui::icon::Properties, TR("Select a chart element to format it.")).execute = [](ui::Command&) {};
  auto sel = [&](const char* id, const char* label, E e) {
    add(id, label, nullptr, "").execute = [&s, e](ui::Command&) {
      ObjSel o;
      const ChartObj* ch = SelectedChart(s, &o);
      if (!ch) return;
      int series = 0;
      if (e == E::Series) {
        if (ch->series.empty()) return;
        int cur = 0;
        const E was = ElemOf(o, &cur);
        series = was == E::Series ? (cur + 1) % static_cast<int>(ch->series.size()) : 0;  // again: the next series
      }
      SelectElem(o, e, series);
    };
  };
  sel("ChartSelChartArea", TR("Chart Area"), E::ChartArea);
  sel("ChartSelPlotArea", TR("Plot Area"), E::PlotArea);
  sel("ChartSelTitle", TR("Chart Title"), E::Title);
  sel("ChartSelLegend", TR("Legend"), E::Legend);
  sel("ChartSelCatAxis", TR("Category Axis"), E::CategoryAxis);
  sel("ChartSelValAxis", TR("Value Axis"), E::ValueAxis);
  sel("ChartSelSeries", TR("Series (next)"), E::Series);
  sel("ChartSelValAxis2", TR("Secondary Vertical (Value) Axis"), E::ValueAxis2);
  sel("ChartSelGridlines", TR("Vertical (Value) Axis Major Gridlines"), E::MajorGridlines);
  for (int i = 0; i < kSelSeries; ++i) {
    static std::vector<std::string> ids;
    ids.push_back("ChartSelSeries" + std::to_string(i + 1));
    add(ids.back().c_str(), (TR("Series") + std::to_string(i + 1)).c_str(), nullptr, "").execute = [&s, i](ui::Command&) {
      ObjSel o;
      const ChartObj* ch = SelectedChart(s, &o);
      if (ch && static_cast<size_t>(i) < ch->series.size()) SelectElem(o, model::ChartElem::Series, i);
    };
  }
  add("ChartFormatSelection", TR("Format Selection"), ui::icon::Properties, TR("Open the Format pane for the selected chart element.")).execute = [&s](ui::Command&) {
    ObjSel o;
    if (!SelectedChart(s, &o)) return;
    if (!(U().sel == o)) SelectElem(o, E::ChartArea, 0);
    U().show_props = true;
  };
  add("ChartResetStyle", TR("Reset to Match Style"), ui::icon::Clear, TR("Clear the selected element's own formatting.")).execute = [&s](ui::Command&) {
    ObjSel o;
    if (!SelectedChart(s, &o)) return;
    int es = 0;
    const E e = ElemOf(o, &es);
    EditSelectedChart(s, TR("Reset to Match Style"), [e, es](ChartObj& c) {
      const ChartObj def;
      switch (e) {
        case E::Title: c.title_font = def.title_font; break;
        case E::Legend: c.legend_font = def.legend_font; break;
        case E::CategoryAxis:
        case E::ValueAxis: c.axis_font = def.axis_font; break;
        case E::PlotArea: c.plot_fill = 0; break;
        case E::Series:
          if (es >= 0 && static_cast<size_t>(es) < c.series.size()) {
            auto& se = c.series[static_cast<size_t>(es)];
            se.color = 0;
            se.line_weight = 0;
            se.marker = -1;
            se.marker_size = 0;
          }
          break;
        default:
          c.chart_fill = c.chart_line = c.plot_fill = 0;
          c.chart_line_none = false;
          c.title_font = def.title_font;
          c.legend_font = def.legend_font;
          c.axis_font = def.axis_font;
          break;
      }
    });
  };
}

bool SelectedChartObj(AppState& s, uint32_t* sheet, uint32_t* id) {
  ObjSel o;
  if (!SelectedChart(s, &o)) return false;
  *sheet = o.sheet;
  *id = o.id;
  return true;
}

int SelectedChartPoint() { return U().sel.kind == 2 && U().elem_of == U().sel ? U().elem_point : -1; }

bool SelectedChartElem(uint32_t* sheet, uint32_t* id, int* elem, int* series) {
  const UiState& u = U();
  if (u.sel.kind != 2) return false;
  *sheet = u.sel.sheet;
  *id = u.sel.id;
  const model::ChartElem e = ElemOf(u.sel, series);
  *elem = static_cast<int>(e);
  return true;
}

void SelectChartElement(AppState& s, uint32_t sheet, uint32_t chart_id, int elem, int series, bool format, int point) {
  if (s.focus_sheet != sheet) ActivateSheet(s, sheet);
  const model::ChartElem e = static_cast<model::ChartElem>(std::clamp(elem, 1, model::kChartElemLast));
  // (a category's legend entry has no series of its own: -1)
  SelectElem(ObjSel{sheet, 2, chart_id}, e, std::max(e == model::ChartElem::LegendEntry ? -1 : 0, series), point);
  if (format) U().show_props = true;
}

void DrawMoveChartDialog(AppState& s) {
  if (s.dialog != Dialog::MoveChart) return;
  const char* title = TR("Move Chart");
  const model::Sheet* from = s.snap ? s.snap->Sheet(g_move.sheet) : nullptr;
  std::vector<const model::Sheet*> targets;
  if (s.snap) {
    for (const auto& sh : s.snap->wb.sheets) {
      if (!sh->chart_sheet && sh->visible) targets.push_back(&sh.get());
    }
  }
  if (s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
    g_move_choice = from && from->chart_sheet ? 0 : 1;
    std::snprintf(g_move_name, sizeof g_move_name, "%s", from && from->chart_sheet ? from->name.c_str() : NextChartSheetName(s.snap->wb).c_str());
    g_move_target = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
      if (from && targets[i]->id == from->id) g_move_target = static_cast<int>(i);
    }
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (!RangePicking() && !open) s.dialog = Dialog::None;
    return;
  }
  ImGui::TextUnformatted(TR("Choose where you want the chart to be placed:"));
  ImGui::Spacing();
  ImGui::RadioButton(TR("New sheet:"), &g_move_choice, 0);
  ImGui::SameLine(S(130));
  ImGui::SetNextItemWidth(S(240));
  if (ImGui::InputText("##newsheet", g_move_name, sizeof g_move_name)) g_move_choice = 0;
  ImGui::RadioButton(TR("Object in:"), &g_move_choice, 1);
  ImGui::SameLine(S(130));
  ImGui::SetNextItemWidth(S(240));
  g_move_target = std::clamp(g_move_target, 0, std::max(0, static_cast<int>(targets.size()) - 1));
  if (ImGui::BeginCombo("##target", targets.empty() ? "" : targets[static_cast<size_t>(g_move_target)]->name.c_str())) {
    for (size_t i = 0; i < targets.size(); ++i) {
      if (ImGui::Selectable(targets[i]->name.c_str(), static_cast<int>(i) == g_move_target)) {
        g_move_target = static_cast<int>(i);
        g_move_choice = 1;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Separator();
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (ImGui::Button(TR("OK"), ImVec2(S(90), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    const ObjSel o = g_move;
    const uint32_t to = g_move_choice == 1 && !targets.empty() ? targets[static_cast<size_t>(g_move_target)]->id : 0;
    const std::string name = g_move_name;
    if (to == o.sheet) {  // the sheet it is on already: nothing to do
      ImGui::CloseCurrentPopup();
      s.dialog = Dialog::None;
      ImGui::EndPopup();
      return;
    }
    // on a worksheet: near the top left of what its window shows
    double left = 36, top = 18;
    if (to) {
      const model::Sheet* t = s.snap->Sheet(to);
      const auto& v = s.Win(to).view;
      if (t) {
        left += static_cast<double>(t->cols->Offset(v.h.first)) / kPx;
        top += static_cast<double>(t->rows->Offset(v.v.first)) / kPx;
      }
    }
    uint32_t nid = 0;
    std::string err;
    const uint32_t dest = s.engine->Call([&](Document& d) {
      d.Begin(TR("Move Chart"));
      const uint32_t r = d.MoveChart(o.sheet, o.id, to, name, left, top, &nid);
      d.Commit();
      if (!r) err = to ? TR("The chart couldn't be moved there.") : TR("That sheet name can't be used.");
      return r;
    });
    if (dest) {
      s.engine->Sync();
      s.snap = s.engine->Current();
      ActivateSheet(s, dest);
      SelectElem(ObjSel{dest, 2, nid}, model::ChartElem::ChartArea, 0);
      close = true;
    } else {
      s.ShowError("YCGrid", err);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}


// ---- Shape Format / Picture Format, the object clipboard and keys, Align / Group / Rotate, Selection Pane ----

namespace {

uint32_t ArgbOf(ImU32 c) {
  return (c & 0xFF000000u) | ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}
ImU32 ImOf(uint32_t argb) { return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255); }

// the selected shapes (the primary first); a locked one on a protected sheet stops the command with Excel's message
std::vector<ObjSel> SelectedShapes(AppState& s, bool check_protection = true) {
  std::vector<ObjSel> v;
  for (const ObjSel& o : SelectedList()) {
    if (o.kind == 3) v.push_back(o);
  }
  if (check_protection && !v.empty() && SelectionProtected(s)) v.clear();
  return v;
}

void EditShapes(AppState& s, const char* label, std::function<void(ShapeObj&)> fn) {
  const std::vector<ObjSel> v = SelectedShapes(s);
  if (v.empty()) return;
  s.engine->PostUser(label, [v, fn](Document& d) {
    for (const ObjSel& o : v) d.UpdateShape(o.sheet, o.id, fn);
  });
}

// moves / sizes each selected object with fn(geometry) (any kind)
void EditGeoms(AppState& s, const char* label, std::function<void(ObjSel, Geom&)> fn) {
  const std::vector<ObjSel> v = SelectedList();
  if (v.empty() || SelectionProtected(s)) return;
  const model::Sheet* sh = s.snap->Sheet(v[0].sheet);
  if (!sh) return;
  struct G {
    ObjSel o;
    Geom g;
  };
  std::vector<G> gs;
  for (const ObjSel& o : v) {
    Geom g;
    bool ok = false;
    if (o.kind == 3) {
      if (const ShapeObj* x = FindShape(*sh, o.id)) g = GeomOf(*x), ok = true;
    } else if (o.kind == 2) {
      for (const auto& x : *sh->charts) {
        if (x.id == o.id) g = GeomOf(x), ok = true;
      }
    } else {
      for (const auto& x : *sh->controls) {
        if (x.id == o.id) g = GeomOf(x), ok = true;
      }
    }
    if (!ok) continue;
    fn(o, g);
    gs.push_back({o, g});
  }
  s.engine->PostUser(label, [gs](Document& d) {
    for (const G& x : gs) {
      auto set = [&](auto& y) {
        y.left = std::max(0.0, x.g.l);
        y.top = std::max(0.0, x.g.t);
        y.width = std::max(1.0, x.g.w);
        y.height = std::max(1.0, x.g.h);
      };
      if (x.o.kind == 1) d.UpdateControl(x.o.sheet, x.o.id, set);
      else if (x.o.kind == 3) d.UpdateShape(x.o.sheet, x.o.id, set);
      else d.UpdateChart(x.o.sheet, x.o.id, set);
    }
  });
}

// Arrange > Align: 0 left, 1 center, 2 right, 3 top, 4 middle, 5 bottom, 6 distribute horizontally, 7 vertically
void AlignSelection(AppState& s, int how) {
  const std::vector<ObjSel> v = SelectedList();
  if (v.size() < 2) {
    s.SetStatus(TR("Select two or more objects to align them (Shift+click)."));
    return;
  }
  // the bounds of the selection, and for distributing the order of the objects
  const model::Sheet* sh = s.snap->Sheet(v[0].sheet);
  if (!sh) return;
  std::map<uint64_t, Geom> geoms;
  double l = 1e18, t = 1e18, r = -1e18, b = -1e18;
  for (const ObjSel& o : v) {
    Geom g;
    if (o.kind == 3) {
      if (const ShapeObj* x = FindShape(*sh, o.id)) g = GeomOf(*x);
    } else if (o.kind == 2) {
      for (const auto& x : *sh->charts) {
        if (x.id == o.id) g = GeomOf(x);
      }
    } else {
      for (const auto& x : *sh->controls) {
        if (x.id == o.id) g = GeomOf(x);
      }
    }
    geoms[(static_cast<uint64_t>(o.kind) << 32) | o.id] = g;
    l = std::min(l, g.l), t = std::min(t, g.t), r = std::max(r, g.l + g.w), b = std::max(b, g.t + g.h);
  }
  // distributing: the objects in order, equal gaps between them
  std::map<uint64_t, double> place;
  if (how >= 6) {
    std::vector<std::pair<double, uint64_t>> order;
    double total = 0;
    for (const auto& [k, g] : geoms) order.push_back({how == 6 ? g.l : g.t, k}), total += how == 6 ? g.w : g.h;
    std::sort(order.begin(), order.end());
    const double span = how == 6 ? r - l : b - t;
    const double gap = order.size() > 1 ? (span - total) / static_cast<double>(order.size() - 1) : 0;
    double at = how == 6 ? l : t;
    for (const auto& [pos, k] : order) {
      place[k] = at;
      at += (how == 6 ? geoms[k].w : geoms[k].h) + gap;
    }
  }
  static const char* kLabels[] = {TR("Align Left"), TR("Align Center"), TR("Align Right"), TR("Align Top"), TR("Align Middle"), TR("Align Bottom"),
                                  TR("Distribute Horizontally"), TR("Distribute Vertically")};
  EditGeoms(s, kLabels[std::clamp(how, 0, 7)], [&](ObjSel o, Geom& g) {
    switch (how) {
      case 0: g.l = l; break;
      case 1: g.l = (l + r) / 2 - g.w / 2; break;
      case 2: g.l = r - g.w; break;
      case 3: g.t = t; break;
      case 4: g.t = (t + b) / 2 - g.h / 2; break;
      case 5: g.t = b - g.h; break;
      case 6: g.l = place[(static_cast<uint64_t>(o.kind) << 32) | o.id]; break;
      default: g.t = place[(static_cast<uint64_t>(o.kind) << 32) | o.id]; break;
    }
  });
}

// the object clipboard is what Paste pastes while nothing was copied after it
bool ObjClipPending() { return Clip().valid && ClipboardSequence() == Clip().seq; }

void CopyObjects(AppState& s, bool cut) {
  if (!U().sel) return;
  if (cut && SelectionProtected(s)) return;
  ObjClip c = CopyOfSelection(s);
  if (!c.valid) return;
  // the cells' copy is replaced (Paste pastes the objects now), and another program's copy replaces these
  c.seq = ClaimClipboard(s);
  c.pastes = 0;
  Clip() = std::move(c);
  if (!cut && Clip().charts.size() == 1 && Clip().shapes.empty() && Clip().controls.empty()) {
    // the copied chart's picture, for Paste as Picture (taken once it is drawn without its selection marks)
    AppState::ShotRequest q;
    q.target = "chart";
    q.address = Clip().charts[0].name;
    q.sheet = Clip().sheet;
    q.headers = false;
    q.max_width = 0;
    q.wait_frames = 3;
    const uint32_t seq = Clip().seq;
    q.on_pixels = [seq](std::vector<uint8_t>&& px, int w, int h) {
      if (Clip().seq != seq || w <= 0 || h <= 0) return;
      Image img;
      img.w = w;
      img.h = h;
      img.rgba = std::move(px);
      std::string png;
      if (EncodePng(img, &png)) Clip().png = std::move(png);
    };
    g_marks_hidden_until = ImGui::GetFrameCount() + 4;
    s.shots.push_back(std::move(q));
  }
  if (cut) DeleteSelected(s);
}


void PasteObjects(AppState& s) {
  ObjClip& c = Clip();
  if (!c.valid || !s.FocusSheet()) return;
  const model::Sheet* sh = s.FocusSheet();
  const UiState& u = U();
  if (u.sel && u.sel.sheet == sh->id) {
    // an object is selected: the copy goes a little down and right of the copied one (each paste a step further)
    ++c.pastes;
    const double off = 7.5 * c.pastes;
    PasteObjClip(s, c, sh->id, off, off, TR("Paste"));
  } else {
    // like Excel: at the active cell
    const auto& v = s.FocusView();
    const double l = static_cast<double>(sh->cols->Offset(v.sel.act_c)) / kPx, t = static_cast<double>(sh->rows->Offset(v.sel.act_r)) / kPx;
    PasteObjClip(s, c, sh->id, 0, 0, TR("Paste"), l, t);
  }
}

// the selected shape (for the Shape Format tab and the Size boxes)
const ShapeObj* PrimaryShape(const AppState& s) {
  const UiState& u = U();
  if (u.sel.kind != 3 || !s.snap) return nullptr;
  const model::Sheet* sh = s.snap->Sheet(u.sel.sheet);
  return sh ? FindShape(*sh, u.sel.id) : nullptr;
}

// Size group: Height / Width in inches, typed (Enter) or stepped
void DrawSizeBox(AppState& s, int which, const ImRect& r) {
  const UiState& u = U();
  Geom g;
  bool ok = false;
  const model::Sheet* sh = u.sel ? s.snap->Sheet(u.sel.sheet) : nullptr;
  if (sh && u.sel.kind == 3) {
    if (const ShapeObj* x = FindShape(*sh, u.sel.id)) g = GeomOf(*x), ok = true;
  } else if (sh && u.sel.kind == 2) {
    for (const auto& x : *sh->charts) {
      if (x.id == u.sel.id) g = GeomOf(x), ok = true;
    }
  }
  if (!ok) return;
  static char buf[2][32];
  static uint64_t shown_version[2] = {0, 0};
  static uint32_t shown_id[2] = {0, 0};
  const double pt = which == 0 ? g.h : g.w;
  ImGui::PushID(which);
  // laid out from the item's box (SameLine's offsets count from the window, not from r)
  const float fy = r.Min.y + std::max(0.0f, (r.GetHeight() - ImGui::GetFrameHeight()) * 0.5f);
  ImGui::GetWindowDrawList()->AddText(ImVec2(r.Min.x + S(4), fy + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text),
                                      which == 0 ? TR("Height:") : TR("Width:"));
  ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(50), fy));
  const ImGuiID id = ImGui::GetID(which == 0 ? "##ShapeHeight" : "##ShapeWidth");
  if (ImGui::GetActiveID() != id && (shown_version[which] != s.snap->version || shown_id[which] != u.sel.id)) {
    std::snprintf(buf[which], sizeof(buf[which]), "%.2f\"", pt / 72.0);
    shown_version[which] = s.snap->version;
    shown_id[which] = u.sel.id;
  }
  ImGui::SetNextItemWidth(r.Max.x - ImGui::GetCursorScreenPos().x - S(2));
  const bool enter = ImGui::InputText(which == 0 ? "##ShapeHeight" : "##ShapeWidth", buf[which], sizeof(buf[which]), ImGuiInputTextFlags_EnterReturnsTrue);
  ui::NoteUiItem(which == 0 ? "Shape Height" : "Shape Width", ImGui::GetCurrentContext()->LastItemData.Rect);
  if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
    const double inches = std::strtod(buf[which], nullptr);
    if (inches > 0 && inches < 200) {
      const double v = inches * 72.0;
      // a shape with Lock aspect ratio keeps its proportions
      const ShapeObj* sp = u.sel.kind == 3 ? FindShape(*sh, u.sel.id) : nullptr;
      const bool keep = sp && sp->lock_aspect && g.w > 0 && g.h > 0;
      EditGeoms(s, which == 0 ? TR("Shape Height") : TR("Shape Width"), [&](ObjSel o, Geom& x) {
        if (!(o == u.sel)) return;
        if (which == 0) {
          if (keep) x.w = x.w * v / x.h;
          x.h = v;
        } else {
          if (keep) x.h = x.h * v / x.w;
          x.w = v;
        }
      });
    }
    shown_version[which] = 0;
  }
  ImGui::PopID();
}

// ---- Selection Pane: every object on the sheet, the top one first; click to select, the eye shows / hides ----
bool g_selection_pane = false;

void DrawSelectionPane(AppState& s) {
  if (!g_selection_pane) return;
  const model::Sheet* sh = s.FocusSheet();
  ImGui::SetNextWindowSize(ImVec2(S(260), S(360)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Right);
  DockAsTaskPane(TR("Selection###yc_selpane"), false, S(260));  // beside the sheets, as Excel's
  if (!ImGui::Begin(TR("Selection###yc_selpane"), &g_selection_pane)) {
    ImGui::End();
    return;
  }
  if (!sh) {
    ImGui::End();
    return;
  }
  struct Row {
    ObjSel o;
    std::string name;
    int z;
    bool visible;
    bool can_hide;
  };
  std::vector<Row> rows;
  for (const auto& x : *sh->shapes) rows.push_back({ObjSel{sh->id, 3, x.id}, x.name, x.z, x.visible, true});
  for (const auto& x : *sh->charts) rows.push_back({ObjSel{sh->id, 2, x.id}, x.name, x.z, true, false});
  for (const auto& x : *sh->controls) rows.push_back({ObjSel{sh->id, 1, x.id}, x.name, x.z, x.visible, true});
  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.z > b.z; });
  if (ImGui::SmallButton(TR("Show All"))) {
    const uint32_t sid = sh->id;
    s.engine->PostUser(TR("Show All"), [sid](Document& d) {
      const model::Sheet* x = d.wb().SheetById(sid);
      if (!x) return;
      for (const auto& y : *x->shapes) {
        if (!y.visible) d.UpdateShape(sid, y.id, [](ShapeObj& z) { z.visible = true; });
      }
      for (const auto& y : *x->controls) {
        if (!y.visible) d.UpdateControl(sid, y.id, [](Control& z) { z.visible = true; });
      }
    });
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(TR("Hide All"))) {
    const uint32_t sid = sh->id;
    s.engine->PostUser(TR("Hide All"), [sid](Document& d) {
      const model::Sheet* x = d.wb().SheetById(sid);
      if (!x) return;
      for (const auto& y : *x->shapes) d.UpdateShape(sid, y.id, [](ShapeObj& z) { z.visible = false; });
      for (const auto& y : *x->controls) d.UpdateControl(sid, y.id, [](Control& z) { z.visible = false; });
    });
  }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##up", ImGuiDir_Up) && U().sel) ui::Commands().Execute("BringForward");
  ImGui::SameLine();
  if (ImGui::ArrowButton("##down", ImGuiDir_Down) && U().sel) ui::Commands().Execute("SendBackward");
  ImGui::Separator();
  for (const Row& row : rows) {
    ImGui::PushID(static_cast<int>(row.o.id) * 4 + row.o.kind);
    if (row.can_hide) {
      if (ImGui::SmallButton(row.visible ? TR("Hide") : TR("Show"))) {
        const ObjSel o = row.o;
        const bool v = !row.visible;
        s.engine->PostUser(v ? TR("Show") : TR("Hide"), [o, v](Document& d) {
          if (o.kind == 3) d.UpdateShape(o.sheet, o.id, [v](ShapeObj& x) { x.visible = v; });
          else d.UpdateControl(o.sheet, o.id, [v](Control& x) { x.visible = v; });
        });
      }
      ImGui::SameLine();
    }
    if (ImGui::Selectable(row.name.c_str(), IsSelected(row.o))) {
      UiState& u = U();
      if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
        if (!IsSelected(row.o)) {
          if (u.sel) u.more.push_back(u.sel);
          u.sel = row.o;
        }
      } else {
        u.sel = row.o;
        u.more.clear();
      }
    }
    ImGui::PopID();
  }
  ImGui::End();
}

}  // namespace

// Paste > Picture: the copied chart, or the copied cells, as a picture at the active cell (Excel's Paste Options >
// Picture and Paste Special > Picture (PNG))
void PasteAsPicture(AppState& s) {
  std::string png;
  double w = 0, h = 0;
  if (ObjClipPending() && !Clip().png.empty()) {
    png = Clip().png;
    if (!Clip().charts.empty()) w = Clip().charts[0].width, h = Clip().charts[0].height;
  } else if (CopyModeOn(s) && s.FocusSheet()) {  // the copied cells: Paste Options > Picture
    const auto& sel = s.FocusView().sel;
    PasteCellsPicture(s, s.focus_sheet, Rect::Cell(sel.act_r, sel.act_c), s.marquee_sheet, s.marquee_rect, false);
    return;
  }
  if (png.empty() || !s.FocusSheet() || s.FocusSheet()->chart_sheet) {
    s.SetStatus(TR("Copy a chart or cells first, then paste them as a picture."));
    return;
  }
  ShapeObj sh;
  sh.kind = ShapeKind::Picture;
  const auto& v = s.FocusView();
  const model::Sheet* fs = s.FocusSheet();
  sh.left = static_cast<double>(fs->cols->Offset(v.sel.act_c)) / kPx;
  sh.top = static_cast<double>(fs->rows->Offset(v.sel.act_r)) / kPx;
  if (w <= 0 || h <= 0) {
    int pw = 0, ph = 0;
    ImageSize(png, &pw, &ph);
    w = pw * 0.75, h = ph * 0.75;
  }
  sh.width = std::max(1.0, w);
  sh.height = std::max(1.0, h);
  sh.orig_w = sh.width;
  sh.orig_h = sh.height;
  sh.lock_aspect = true;
  sh.image = std::move(png);
  const uint32_t sheet = fs->id;
  const uint32_t id = s.engine->Call([&](Document& d) {
    d.Begin(TR("Paste as Picture"));
    const uint32_t nid = d.AddShape(sheet, sh);
    d.Commit();
    return nid;
  });
  SelectInsertedShape(s, sheet, id);
}

bool CanPasteAsPicture(const AppState& s) { return (ObjClipPending() && !Clip().png.empty()) || CopyModeOn(s); }

// Paste Special while objects are copied: paste them as they are, or (a chart) as a picture, as Excel's Paste Special
// lists them
bool g_obj_paste_special = false;
void DrawObjectPasteSpecial(AppState& s) {
  if (!g_obj_paste_special) return;
  static int as = 0;
  const bool picture_ok = !Clip().png.empty();
  if (!picture_ok) as = 0;
  ImGui::SetNextWindowSize(ImVec2(S(320), 0), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true, ok = false;
  if (ImGui::Begin(TR("Paste Special###yc_obj_paste_special"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    ImGui::TextUnformatted(TR("As:"));
    const char* what = !Clip().charts.empty() ? TR("YCGrid Chart Object") : TR("YCGrid Drawing Object");
    if (ImGui::RadioButton(what, as == 0)) as = 0;
    ImGui::BeginDisabled(!picture_ok);
    if (ImGui::RadioButton(TR("Picture (PNG)"), as == 1)) as = 1;
    ImGui::EndDisabled();
    ImGui::Separator();
    ok = ImGui::Button(TR("OK"), ImVec2(S(90), 0)) || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter));
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(90), 0)) || (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape))) open = false;
  }
  ImGui::End();
  if (ok) {
    g_obj_paste_special = false;
    if (as == 1) PasteAsPicture(s);
    else PasteObjects(s);
  } else if (!open) {
    g_obj_paste_special = false;
  }
}

bool ChartTopLeftCell(const AppState& s, uint32_t sheet, const std::string& name, uint32_t* row, uint32_t* col) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return false;
  for (const auto& c : *sh->charts) {
    if (c.name != name) continue;
    *row = static_cast<uint32_t>(std::max<int64_t>(0, sh->rows->IndexAt(static_cast<int64_t>(c.top * kPx))));
    *col = static_cast<uint32_t>(std::max<int64_t>(0, sh->cols->IndexAt(static_cast<int64_t>(c.left * kPx))));
    return true;
  }
  return false;
}

bool ChartScreenRect(const AppState& s, uint32_t sheet, const std::string& name, ImRect* rect) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  if (!sh) return false;
  for (const auto& c : *sh->charts) {
    if (c.name != name) continue;
    const auto it = U().charts.find(c.id);
    if (it == U().charts.end() || it->second.hit.frame.GetWidth() < 2) return false;
    *rect = it->second.hit.frame;
    return true;
  }
  return false;
}

bool ObjectClipboardPending() { return ObjClipPending(); }

// the ribbon's contextual Shape Format / Picture Format tabs follow the selection
void UpdateShapeContext(AppState& s) {
  auto& cx = ui::ribbon::Context();
  const ShapeObj* sp = PrimaryShape(s);
  cx.shape = sp && sp->kind != ShapeKind::Picture;
  cx.picture = sp && sp->kind == ShapeKind::Picture;
  if (!cx.size_box) cx.size_box = [&s](int which, const ImRect& r) { DrawSizeBox(s, which, r); };
  if (!cx.chart_elements_box) {
    cx.chart_elements_box = [&s](const ImRect& r) {  // Format > Current Selection: the Chart Elements box
      ObjSel o;
      const ChartObj* ch = SelectedChart(s, &o);
      ImGui::SetCursorScreenPos(ImVec2(r.Min.x, r.Min.y + std::max(0.0f, (r.GetHeight() - ImGui::GetFrameHeight()) * 0.5f)));
      ui::NoteUiItem(TR("Chart Elements"), r);  // combos report no label (AGENTS.md)
      if (!ch) {
        ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(r.GetWidth());
        if (ImGui::BeginCombo("##ribbon_chart_elem", "")) ImGui::EndCombo();
        ImGui::EndDisabled();
        return;
      }
      int es = 0, ep = -1;
      const model::ChartElem el = ElemOf(o, &es, &ep);
      ChartElemCombo(o, *ch, el, es, ep, "##ribbon_chart_elem", r.GetWidth());
    };
  }
  DrawSelectionPane(s);
}

bool ObjectSelected(const AppState& s) {
  const UiState& u = U();
  return u.sel && u.sel.sheet == s.focus_sheet && !u.text_obj && !s.edit.active;
}

void SelectObject(AppState& s, uint32_t sheet, int kind, uint32_t id, bool replace) {
  UiState& u = U();
  const ObjSel o{sheet, kind, id};
  if (!replace && u.sel && u.sel.sheet == sheet) {
    if (!IsSelected(o)) u.more.push_back(o);
  } else {
    u.sel = o;
    u.more.clear();
    if (const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr) AddGroupOf(*sh, o);
  }
  if (s.edit.active) CancelEdit(s);
}

std::vector<std::pair<int, uint32_t>> SelectedObjectIds() {
  std::vector<std::pair<int, uint32_t>> v;
  for (const ObjSel& o : SelectedList()) v.push_back({o.kind, o.id});
  return v;
}

// keys while an object is selected (Excel): they act on the object, never on the hidden cells. Returns true when an
// object is selected, so the cell shortcuts are skipped.
bool ObjectShortcuts(AppState& s) {
  if (!ObjectSelected(s)) return SlicerSelected(s.focus_sheet) && !s.edit.active;  // a slicer: the cell keys are skipped too
  const std::pair<ImGuiKeyChord, const char*> keys[] = {
      {ImGuiMod_Ctrl | ImGuiKey_C, "ObjectCopy"},           {ImGuiMod_Ctrl | ImGuiKey_X, "ObjectCut"},
      {ImGuiMod_Ctrl | ImGuiKey_V, "ObjectPaste"},          {ImGuiMod_Ctrl | ImGuiKey_D, "ObjectDuplicate"},
      {ImGuiMod_Ctrl | ImGuiKey_1, "FormatShapePane"},      {ImGuiMod_Ctrl | ImGuiKey_B, "Bold"},
      {ImGuiMod_Ctrl | ImGuiKey_I, "Italic"},               {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Period, "FontSizeIncrease"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Comma, "FontSizeDecrease"},
  };
  for (const auto& [chord, cmd] : keys) {
    if (ImGui::IsKeyChordPressed(chord)) ui::Commands().Execute(cmd);
  }
  return true;
}

// the text of the selected chart element formatted (Home > Font on a chart): fn(font, the element's font before, its
// default size)
using ChartFontFn = std::function<void(model::ChartFont&, const model::ChartFont&, float)>;
void ApplyChartElementFont(ChartObj& c, model::ChartElem e, int es, int ep, const ChartFontFn& fn) {
  using E = model::ChartElem;
  auto axis = [&](model::ChartAxisFormat& a, const model::ChartFont& first) {
    model::ChartFont f = AxisFontOf(c, a);
    fn(f, first, 9);
    a.font = f;
    a.own_font = true;
  };
  auto axis_title = [&](model::ChartAxisFormat& a, const model::ChartFont& first) {
    model::ChartFont f = AxisTitleFontOf(c, a);
    fn(f, first, 10);
    a.title_font = f;
  };
  model::ChartSeries* se = es >= 0 && static_cast<size_t>(es) < c.series.size() ? &c.series[static_cast<size_t>(es)] : nullptr;
  switch (e) {
    case E::Title: {
      const model::ChartFont first = c.title_font;
      fn(c.title_font, first, 14);
      break;
    }
    case E::LegendEntry: {  // that entry's own font
      const int cs = es >= 0 ? -1 : CategoryEntrySeries(c);
      if (se) {
        model::ChartFont f = se->legend_entry_font_set ? se->legend_entry_font : c.legend_font;
        const model::ChartFont first = f;
        fn(f, first, 9);
        se->legend_entry_font = f;
        se->legend_entry_font_set = true;
      } else if (cs >= 0 && ep >= 0) {
        model::ChartPointFormat& pf = PointFormatRef(c.series[static_cast<size_t>(cs)], ep);
        model::ChartFont f = pf.legend_font_set ? pf.legend_font : c.legend_font;
        const model::ChartFont first = f;
        fn(f, first, 9);
        pf.legend_font = f;
        pf.legend_font_set = true;
      }
      break;
    }
    case E::Legend: {
      const model::ChartFont first = c.legend_font;
      fn(c.legend_font, first, 9);
      break;
    }
    case E::DataLabel:  // that one label's own font
      if (se && ep >= 0) {
        model::ChartPointFormat& pf = PointFormatRef(*se, ep);
        model::ChartFont f = pf.label_font_set ? pf.label_font : se->label_font;
        const model::ChartFont first = f;
        fn(f, first, 9);
        pf.label_font = f;
        pf.label_font_set = true;
      }
      break;
    case E::CategoryAxis: axis(c.cat_axis, AxisFontOf(c, c.cat_axis)); break;
    case E::ValueAxis: axis(c.val_axis, AxisFontOf(c, c.val_axis)); break;
    case E::ValueAxis2: axis(c.val_axis2, AxisFontOf(c, c.val_axis2)); break;
    case E::CatAxisTitle: axis_title(c.cat_axis, AxisTitleFontOf(c, c.cat_axis)); break;
    case E::ValAxisTitle: axis_title(c.val_axis, AxisTitleFontOf(c, c.val_axis)); break;
    case E::ValAxis2Title: axis_title(c.val_axis2, AxisTitleFontOf(c, c.val_axis2)); break;
    case E::DataLabels:
    case E::Series:
    case E::Point:
      if (se) {
        const model::ChartFont first = se->label_font;
        fn(se->label_font, first, 9);
      }
      break;
    default: {  // the chart area (or an element without text): every text of the chart, as Excel
      const model::ChartFont first = c.title_font;
      fn(c.title_font, first, 14);
      fn(c.legend_font, first, 9);
      fn(c.axis_font, first, 9);
      for (model::ChartAxisFormat* a : {&c.cat_axis, &c.val_axis, &c.val_axis2}) {
        if (a->own_font) fn(a->font, first, 9);
        if (!(a->title_font == model::ChartFont{})) fn(a->title_font, first, 10);
      }
      for (auto& x : c.series) fn(x.label_font, first, 9);
      break;
    }
  }
}

void EditChartElementFont(AppState& s, ObjSel o, const char* label, ChartFontFn fn) {
  int es = 0, ep = -1;
  const model::ChartElem e = ElemOf(o, &es, &ep);
  s.engine->PostUser(label, [o, e, es, ep, fn](Document& d) { d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) { ApplyChartElementFont(c, e, es, ep, fn); }); });
}

// cells copied and a chart selected: Paste adds them to the chart as new series (Excel); false when that isn't the case
bool PasteCellsIntoChart(AppState& s) {
  ObjSel o;
  const ChartObj* ch = U().sel.kind == 2 && !ObjClipPending() && CopyModeOn(s) ? SelectedChart(s, &o) : nullptr;
  const model::Sheet* src = s.snap ? s.snap->Sheet(s.clip->src_sheet) : nullptr;
  if (!ch || !src || !ch->pivot.empty()) return false;
  if (SelectionProtected(s)) return true;
  ChartObj tmp;
  tmp.type = ch->type;
  const std::vector<Rect> areas{s.clip->src};
  if (!BuildSeriesFromAreas(s.snap->wb, *src, areas, ch->type, &tmp, PlotByRows(areas)) || tmp.series.empty()) return true;
  const std::vector<model::ChartSeries> add = tmp.series;
  s.engine->PostUser(TR("Paste"), [o, add](Document& d) {
    d.UpdateChart(o.sheet, o.id, [&](ChartObj& c) {
      for (const auto& se : add) c.series.push_back(se);
      c.source.clear();  // the series no longer come from one range
    });
  });
  return true;
}

void RegisterShapeCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* ic, const char* desc) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("ObjectCopy", TR("Copy Object"), ui::icon::Copy, TR("Copy the selected objects."))).execute = [&s](ui::Command&) { CopyObjects(s, false); };
  R.Add(make("ObjectCut", TR("Cut Object"), ui::icon::Cut, TR("Cut the selected objects."))).execute = [&s](ui::Command&) { CopyObjects(s, true); };
  R.Add(make("ObjectPaste", TR("Paste Object"), ui::icon::Paste, TR("Paste the copied objects."))).execute = [&s](ui::Command&) {
    if (!PasteCellsIntoChart(s)) PasteObjects(s);  // copied cells go into a selected chart as new series
  };
  R.Add(make("ObjectDuplicate", TR("Duplicate"), ui::icon::Copy, TR("Duplicate the selected objects (Ctrl+D)."))).execute = [&s](ui::Command&) {
    if (U().sel && !SelectionProtected(s)) DuplicateSelection(s, 7.5, 7.5, TR("Duplicate"));
  };
  R.Add(make("FormatShapePane", TR("Format Shape..."), ui::icon::Properties, TR("Open the Format pane of the selected object."))).execute = [&s](ui::Command&) {
    if (U().sel && !SelectionProtected(s)) U().show_props = true;
  };
  // Home's commands act on the selected objects (Copy, Cut, Paste of the copied objects, the font of a shape's text)
  auto wrap = [&R, &s](const char* id, std::function<bool()> on_object) {
    ui::Command* c = R.Find(id);
    if (!c) return;
    auto orig = c->execute;
    c->execute = [on_object, orig](ui::Command& self) {
      if (on_object()) return;
      if (orig) orig(self);
    };
  };
  wrap("Copy", [&s] { return ObjectSelected(s) && (CopyObjects(s, false), true); });
  wrap("Cut", [&s] { return ObjectSelected(s) && (CopyObjects(s, true), true); });
  wrap("Paste", [&s] { return ObjClipPending() && (PasteObjects(s), true); });
  // cells copied and a chart selected: Paste adds them to the chart as new series (Excel)
  wrap("Paste", [&s] { return PasteCellsIntoChart(s); });
  wrap("PasteSpecial", [] {
    if (!ObjClipPending()) return false;
    g_obj_paste_special = true;  // objects copied: Excel's Paste Special lists them and a picture
    return true;
  });
  R.Add(make("PasteAsPicture", TR("Picture"), ui::icon::Picture, TR("Paste the copied chart or cells as a picture."))).execute = [&s](ui::Command&) { PasteAsPicture(s); };
  wrap("FormatCellsDialog", [&s] {
    if (!ObjectSelected(s)) return false;
    ui::Commands().Execute("FormatShapePane");
    return true;
  });
  auto font_wrap = [&](const char* id, const char* label, std::function<void(ShapeObj&, const ShapeObj&)> fn) {
    wrap(id, [&s, label, fn] {
      if (!ObjectSelected(s)) return false;
      const ShapeObj* p = PrimaryShape(s);
      if (!p) return true;  // a chart or control: nothing to do here (Excel formats the selected chart element)
      const ShapeObj first = *p;
      EditShapes(s, label, [fn, first](ShapeObj& x) { fn(x, first); });
      return true;
    });
  };
  font_wrap("Bold", TR("Bold"), [](ShapeObj& x, const ShapeObj& f) { x.bold = !f.bold; });
  font_wrap("Italic", TR("Italic"), [](ShapeObj& x, const ShapeObj& f) { x.italic = !f.italic; });
  static const float kSizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
  font_wrap("FontSizeIncrease", TR("Increase Font Size"), [](ShapeObj& x, const ShapeObj& f) {
    float v = f.font_size + 10;
    for (const float k : kSizes) {
      if (k > f.font_size) {
        v = k;
        break;
      }
    }
    x.font_size = std::min(409.0f, v);
  });
  font_wrap("FontSizeDecrease", TR("Decrease Font Size"), [](ShapeObj& x, const ShapeObj& f) {
    float v = std::max(1.0f, f.font_size - 10);
    for (const float k : kSizes) {
      if (k < f.font_size) v = k;
    }
    x.font_size = std::max(1.0f, v);
  });
  font_wrap("FontSize", TR("Font Size"), [](ShapeObj& x, const ShapeObj&) {
    const float pt = std::strtof(ui::ribbon::St().font_size.c_str(), nullptr);
    if (pt >= 1 && pt <= 409) x.font_size = pt;
  });
  font_wrap("FontName", TR("Font"), [](ShapeObj& x, const ShapeObj&) { x.font_name = ui::ribbon::St().font_name; });
  font_wrap("FontColor", TR("Font Color"), [](ShapeObj& x, const ShapeObj&) { x.font_color = ArgbOf(ui::ribbon::St().font_color) | 0xFF000000u; });
  font_wrap("FillColor", TR("Shape Fill"), [](ShapeObj& x, const ShapeObj&) {
    const ImU32 c = ui::ribbon::St().fill_color;
    x.fill_visible = (c >> 24) != 0;
    if (x.fill_visible) x.fill_color = ArgbOf(c) | 0xFF000000u;
  });
  // ... and on the selected chart element's text, as Excel: the title, an axis, its title, the legend, data labels;
  // the chart area: all of the chart's text
  auto chart_wrap = [&](const char* id, const char* label, ChartFontFn fn) {
    wrap(id, [&s, label, fn] {
      ObjSel o;
      if (U().sel.kind != 2 || !SelectedChart(s, &o)) return false;
      if (!SelectionProtected(s)) EditChartElementFont(s, o, label, fn);
      return true;
    });
  };
  chart_wrap("Bold", TR("Bold"), [](model::ChartFont& x, const model::ChartFont& f, float) { x.bold = !f.bold; });
  chart_wrap("Italic", TR("Italic"), [](model::ChartFont& x, const model::ChartFont& f, float) { x.italic = !f.italic; });
  chart_wrap("Underline", TR("Underline"), [](model::ChartFont& x, const model::ChartFont& f, float) { x.underline = !f.underline; });
  chart_wrap("FontSizeIncrease", TR("Increase Font Size"), [](model::ChartFont& x, const model::ChartFont&, float def) {
    const float now = x.size > 0 ? x.size : def;
    float v = now + 10;
    for (const float k : kSizes) {
      if (k > now) {
        v = k;
        break;
      }
    }
    x.size = std::min(409.0f, v);
  });
  chart_wrap("FontSizeDecrease", TR("Decrease Font Size"), [](model::ChartFont& x, const model::ChartFont&, float def) {
    const float now = x.size > 0 ? x.size : def;
    float v = std::max(1.0f, now - 10);
    for (const float k : kSizes) {
      if (k < now) v = k;
    }
    x.size = std::max(1.0f, v);
  });
  chart_wrap("FontSize", TR("Font Size"), [](model::ChartFont& x, const model::ChartFont&, float) {
    const float pt = std::strtof(ui::ribbon::St().font_size.c_str(), nullptr);
    if (pt >= 1 && pt <= 409) x.size = pt;
  });
  chart_wrap("FontName", TR("Font"), [](model::ChartFont& x, const model::ChartFont&, float) { x.name = ui::ribbon::St().font_name; });
  chart_wrap("FontColor", TR("Font Color"), [](model::ChartFont& x, const model::ChartFont&, float) { x.color = ArgbOf(ui::ribbon::St().font_color) | 0xFF000000u; });
  font_wrap("AlignLeft", TR("Align Text Left"), [](ShapeObj& x, const ShapeObj&) { x.halign = 0; });
  font_wrap("AlignCenter", TR("Center"), [](ShapeObj& x, const ShapeObj&) { x.halign = 1; });
  font_wrap("AlignRight", TR("Align Text Right"), [](ShapeObj& x, const ShapeObj&) { x.halign = 2; });
  font_wrap("AlignTop", TR("Top Align"), [](ShapeObj& x, const ShapeObj&) { x.valign = 0; });
  font_wrap("AlignMiddle", TR("Middle Align"), [](ShapeObj& x, const ShapeObj&) { x.valign = 1; });
  font_wrap("AlignBottom", TR("Bottom Align"), [](ShapeObj& x, const ShapeObj&) { x.valign = 2; });

  // ---- Shape Format tab ----
  R.Add(make("ShapeFill", TR("Shape Fill"), ui::icon::FillColor, TR("Fill the selected shape with a solid color."))).execute = [&s](ui::Command&) {
    const uint32_t c = ArgbOf(ui::ribbon::Context().shape_fill) | 0xFF000000u;
    EditShapes(s, TR("Shape Fill"), [c](ShapeObj& x) {
      x.fill_visible = true;
      x.fill_color = c;
    });
  };
  R.Add(make("ShapeNoFill", TR("No Fill"), ui::icon::FillColor, TR("Remove the fill of the selected shape."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("No Fill"), [](ShapeObj& x) { x.fill_visible = false; });
  };
  R.Add(make("ShapeOutline", TR("Shape Outline"), ui::icon::BorderOutside, TR("Color the outline of the selected shape."))).execute = [&s](ui::Command&) {
    const uint32_t c = ArgbOf(ui::ribbon::Context().shape_line) | 0xFF000000u;
    EditShapes(s, TR("Shape Outline"), [c](ShapeObj& x) {
      x.line_visible = true;
      x.line_color = c;
    });
  };
  R.Add(make("PictureBorder", TR("Picture Border"), ui::icon::BorderOutside, TR("Draw a border of this color around the picture."))).execute = [&s](ui::Command&) {
    const uint32_t c = ArgbOf(ui::ribbon::Context().picture_line) | 0xFF000000u;
    EditShapes(s, TR("Picture Border"), [c](ShapeObj& x) {
      x.line_visible = true;
      x.line_color = c;
    });
  };
  R.Add(make("ShapeNoOutline", TR("No Outline"), ui::icon::BorderNone, TR("Remove the outline of the selected shape."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("No Outline"), [](ShapeObj& x) { x.line_visible = false; });
  };
  R.Add(make("ShapeWeightMenu", TR("Weight"), ui::icon::BorderThick, TR("The width of the outline."))).execute = [](ui::Command&) {};
  static const float kWeights[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.25f, 3.0f, 4.5f};
  for (int i = 0; i < 8; ++i) {
    char id[32], label[32];
    std::snprintf(id, sizeof(id), "ShapeWeight%d", i);
    std::snprintf(label, sizeof(label), "%g pt", kWeights[i]);
    const float w = kWeights[i];
    R.Add(make(id, "", nullptr, TR("The width of the outline."))).execute = [&s, w](ui::Command&) {
      EditShapes(s, TR("Shape Outline Weight"), [w](ShapeObj& x) {
        x.line_visible = true;
        x.line_weight = w;
      });
    };
    R.Find(id)->label = label;
  }
  R.Add(make("ShapeDashesMenu", TR("Dashes"), ui::icon::BorderBottom, TR("The dash style of the outline."))).execute = [](ui::Command&) {};
  static const char* kDashes[] = {TR("Solid"), TR("Dash"), TR("Round Dot"), TR("Square Dot"), TR("Dash Dot"), TR("Long Dash")};
  for (int i = 0; i < 6; ++i) {
    const std::string id = "ShapeDash" + std::to_string(i);
    R.Add(make(id.c_str(), kDashes[i], nullptr, TR("The dash style of the outline."))).execute = [&s, i](ui::Command&) {
      EditShapes(s, TR("Shape Outline Dashes"), [i](ShapeObj& x) { x.line_dash = i; });
    };
  }
  R.Add(make("ShapeArrowsMenu", TR("Arrows"), ui::icon::CaretRight, TR("Arrowheads at the ends of a line."))).execute = [](ui::Command&) {};
  static const char* kArrows[] = {TR("No Arrows"), TR("Arrow at End"), TR("Arrow at Beginning"), TR("Arrows at Both Ends")};
  for (int i = 0; i < 4; ++i) {
    const std::string id = "ShapeArrow" + std::to_string(i);
    R.Add(make(id.c_str(), kArrows[i], nullptr, TR("Arrowheads at the ends of a line."))).execute = [&s, i](ui::Command&) {
      EditShapes(s, TR("Arrows"), [i](ShapeObj& x) {
        x.end_arrow = (i == 1 || i == 3) ? 1 : 0;
        x.begin_arrow = (i == 2 || i == 3) ? 1 : 0;
        if (x.kind == ShapeKind::Arrow && x.end_arrow == 0) x.kind = ShapeKind::Line;  // the arrow kind always has its end arrow
      });
    };
  }
  R.Add(make("ShapeTextAlignMenu", TR("Align Text"), ui::icon::AlignCenter, TR("Where the text sits in the shape."))).execute = [](ui::Command&) {};
  struct TA {
    const char* id;
    const char* label;
    int h, v;
  };
  static const TA kTa[] = {{"ShapeTextLeft", TR("Align Left"), 0, -1},   {"ShapeTextCenter", TR("Center"), 1, -1}, {"ShapeTextRight", TR("Align Right"), 2, -1},
                           {"ShapeTextTop", TR("Top"), -1, 0},            {"ShapeTextMiddle", TR("Middle"), -1, 1},  {"ShapeTextBottom", TR("Bottom"), -1, 2}};
  for (const TA& t : kTa) {
    const int h = t.h, v = t.v;
    R.Add(make(t.id, t.label, nullptr, TR("Where the text sits in the shape."))).execute = [&s, h, v](ui::Command&) {
      EditShapes(s, TR("Align Text"), [h, v](ShapeObj& x) {
        if (h >= 0) x.halign = h;
        if (v >= 0) x.valign = v;
      });
    };
  }
  // ---- Arrange ----
  auto z = [&s](bool front, int step) {
    return [&s, front, step](ui::Command&) {
      if (U().sel && !SelectionProtected(s)) BringTo(s, U().sel, front, step);
    };
  };
  R.Add(make("BringForward", TR("Bring Forward"), ui::icon::ChevronUp, TR("Bring the selected object forward one level."))).execute = z(true, 1);
  R.Add(make("BringToFront", TR("Bring to Front"), ui::icon::ChevronUp, TR("Bring the selected object in front of all the others."))).execute = z(true, 0);
  R.Add(make("SendBackward", TR("Send Backward"), ui::icon::ChevronDown, TR("Send the selected object back one level."))).execute = z(false, 1);
  R.Add(make("SendToBack", TR("Send to Back"), ui::icon::ChevronDown, TR("Send the selected object behind all the others."))).execute = z(false, 0);
  R.Add(make("SelectionPane", TR("Selection Pane"), ui::icon::Panel, TR("See every object on the sheet: select them, show or hide them, change their order."))).execute =
      [](ui::Command&) { g_selection_pane = !g_selection_pane; };
  R.Add(make("ObjAlignMenu", TR("Align"), ui::icon::AlignLeft, TR("Line the selected objects up, or space them evenly."))).execute = [](ui::Command&) {};
  struct AL {
    const char* id;
    const char* label;
  };
  static const AL kAl[] = {{"ObjAlignLeft", TR("Align Left")},       {"ObjAlignCenter", TR("Align Center")},       {"ObjAlignRight", TR("Align Right")},
                           {"ObjAlignTop", TR("Align Top")},         {"ObjAlignMiddle", TR("Align Middle")},       {"ObjAlignBottom", TR("Align Bottom")},
                           {"ObjDistributeH", TR("Distribute Horizontally")}, {"ObjDistributeV", TR("Distribute Vertically")}};
  for (int i = 0; i < 8; ++i) {
    R.Add(make(kAl[i].id, kAl[i].label, nullptr, TR("Line the selected objects up, or space them evenly."))).execute = [&s, i](ui::Command&) { AlignSelection(s, i); };
  }
  R.Add(make("ObjGroupMenu", TR("Group"), ui::icon::Group, TR("Join objects so they move and size as one."))).execute = [](ui::Command&) {};
  R.Add(make("GroupObjects", TR("Group"), ui::icon::Group, TR("Join the selected shapes so they move and size as one."))).execute = [&s](ui::Command&) {
    const std::vector<ObjSel> v = SelectedShapes(s);
    if (v.size() < 2) {
      s.SetStatus(TR("Select two or more shapes to group them (Shift+click)."));
      return;
    }
    const uint32_t g = v.front().id;
    s.engine->PostUser(TR("Group"), [v, g](Document& d) {
      for (const ObjSel& o : v) d.UpdateShape(o.sheet, o.id, [g](ShapeObj& x) { x.group = g; });
    });
  };
  R.Add(make("UngroupObjects", TR("Ungroup"), ui::icon::Ungroup, TR("Break the group into its shapes."))).execute = [&s](ui::Command&) {
    const std::vector<ObjSel> v = SelectedShapes(s);
    if (v.empty()) return;
    s.engine->PostUser(TR("Ungroup"), [v](Document& d) {
      for (const ObjSel& o : v) d.UpdateShape(o.sheet, o.id, [](ShapeObj& x) { x.group = 0; });
    });
  };
  R.Add(make("ObjRotateMenu", TR("Rotate"), ui::icon::Rotate, TR("Turn or flip the selected shape."))).execute = [](ui::Command&) {};
  R.Add(make("RotateRight90", TR("Rotate Right 90\xC2\xB0"), nullptr, TR("Turn the shape a quarter to the right."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("Rotate"), [](ShapeObj& x) { x.rotation = std::fmod(x.rotation + 90.0, 360.0); });
  };
  R.Add(make("RotateLeft90", TR("Rotate Left 90\xC2\xB0"), nullptr, TR("Turn the shape a quarter to the left."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("Rotate"), [](ShapeObj& x) { x.rotation = std::fmod(x.rotation + 270.0, 360.0); });
  };
  R.Add(make("FlipVertical", TR("Flip Vertical"), nullptr, TR("Turn the shape upside down."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("Flip"), [](ShapeObj& x) { x.flip_v = !x.flip_v; });
  };
  R.Add(make("FlipHorizontal", TR("Flip Horizontal"), nullptr, TR("Mirror the shape left to right."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("Flip"), [](ShapeObj& x) { x.flip_h = !x.flip_h; });
  };
  // ---- Picture Format ----
  R.Add(make("ResetPicture", TR("Reset Picture"), ui::icon::ResetPicture, TR("Back to the picture's own size: no crop, no turn, no border."))).execute = [&s](ui::Command&) {
    EditShapes(s, TR("Reset Picture"), [](ShapeObj& x) {
      if (x.kind != ShapeKind::Picture) return;
      x.crop_l = x.crop_t = x.crop_r = x.crop_b = 0;
      x.rotation = 0;
      x.flip_h = x.flip_v = false;
      if (x.orig_w > 0 && x.orig_h > 0) x.width = x.orig_w, x.height = x.orig_h;
    });
  };
  R.Add(make("ChangePicture", TR("Change Picture"), ui::icon::ChangePicture, TR("Put another picture in this one's place, at its size."))).execute = [&s](ui::Command&) {
    const ShapeObj* p = PrimaryShape(s);
    if (!p || p->kind != ShapeKind::Picture || SelectedShapes(s).empty()) return;
    std::string bytes;
    if (!PickPictureFile(s, &bytes)) return;
    const ObjSel o = U().sel;
    s.engine->PostUser(TR("Change Picture"), [o, bytes](Document& d) { d.UpdateShape(o.sheet, o.id, [&](ShapeObj& x) { x.image = bytes; }); });
  };
  R.Add(make("CropPicture", TR("Crop"), ui::icon::Crop, TR("Cut the edges off the picture (Format Picture > Crop)."))).execute = [&s](ui::Command&) {
    if (PrimaryShape(s) && !SelectionProtected(s)) U().show_props = true;
  };
}

// the shape drawn on screen (shape_effects.cpp draws its Effects around it)
void DrawShapeOnScreen(ImDrawList* dl, const ShapeObj& sh, const ImRect& r, float scale) { DrawShape(dl, sh, r, scale); }

}  // namespace yc::app
