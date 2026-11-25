// SPDX-License-Identifier: Apache-2.0
// dataview_ui.cpp - [YC] Data View: a read-only window over tens of millions of rows of columnar data (yc.data.view).
// The columns come from .npy files straight into C++ (no cells, no Python objects); rows scroll virtually, sorting and
// filtering run on all cores over the columns, and a block of rows can go to a sheet or the Clipboard.
#include "app_state.h"
#include "core/i18n.h"
#include "npy.h"
#include "python_bridge.h"

#include "engine/engine.h"
#include "frame/frame_context.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <execution>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace yc::app {

using io::MV;
using ui::S;

namespace {

struct DvColumn {
  std::string name;
  NpyColumn data;
  char filter[96] = {};
};

struct DataView {
  int64_t id = 0;
  std::string title;
  std::vector<DvColumn> cols;
  size_t rows = 0;
  std::vector<uint32_t> order;  // the rows shown, in their shown order (filtered and sorted)
  int sort_col = -1;
  bool sort_desc = false;
  bool open = true;
  bool dirty = true;  // filters changed: rebuild the order
  int stats_col = 0;
  uint64_t order_version = 0;  // bumped whenever the shown rows change
  struct {                     // the footer's statistics, computed once per column and shown rows, not every frame
    int col = -1;
    uint64_t version = ~0ull;
    size_t n = 0;
    double mean = 0, lo = 0, hi = 0;
  } stats;
  std::string note;
};

std::map<int64_t, DataView>& Views() {
  static std::map<int64_t, DataView> v;
  return v;
}
int64_t g_next_view = 1;

// 20000000 -> "20,000,000"
std::string Grouped(size_t n) {
  std::string d = std::to_string(n), out;
  for (size_t i = 0; i < d.size(); ++i) {
    if (i > 0 && (d.size() - i) % 3 == 0) out += ',';
    out += d[i];
  }
  return out;
}

std::string Number(const DvColumn& c, double v) {
  if (!std::isfinite(v)) return {};
  char b[64];
  if (c.data.date) {
    const std::time_t t = static_cast<std::time_t>(std::floor(v));
    std::tm tm{};
    localtime_s(&tm, &t);
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    std::snprintf(b, sizeof(b), "%.10g", v);
  }
  return b;
}

std::string Cell(const DvColumn& c, size_t r) {
  if (c.data.text) return r < c.data.str.size() ? c.data.str[r] : std::string();
  return Number(c, r < c.data.num.size() ? c.data.num[r] : std::nan(""));
}

// a filter: ">5", ">=5", "<5", "<=5", "=5", "!=5", "a..b" (numbers, dates as Unix seconds), text otherwise (contains,
// case-insensitive); "" = no filter
struct Filter {
  int op = 0;  // 0 none, 1 >, 2 >=, 3 <, 4 <=, 5 =, 6 !=, 7 range, 8 contains
  double a = 0, b = 0;
  std::string text;
};

Filter ParseFilter(const char* s, bool text_col) {
  Filter f;
  std::string t(s);
  while (!t.empty() && t.front() == ' ') t.erase(t.begin());
  while (!t.empty() && t.back() == ' ') t.pop_back();
  if (t.empty()) return f;
  auto num = [](const std::string& x, double* v) {
    char* e = nullptr;
    *v = std::strtod(x.c_str(), &e);
    return e && *e == 0 && !x.empty();
  };
  if (!text_col) {
    static const std::pair<const char*, int> ops[] = {{">=", 2}, {"<=", 4}, {"!=", 6}, {">", 1}, {"<", 3}, {"=", 5}};
    for (const auto& [p, o] : ops) {
      if (t.rfind(p, 0) == 0 && num(t.substr(std::strlen(p)), &f.a)) {
        f.op = o;
        return f;
      }
    }
    if (const size_t k = t.find(".."); k != std::string::npos && num(t.substr(0, k), &f.a) && num(t.substr(k + 2), &f.b)) {
      f.op = 7;
      return f;
    }
    if (num(t, &f.a)) {
      f.op = 5;
      return f;
    }
  }
  f.op = 8;
  f.text = engine::Lower(t);
  return f;
}

bool Pass(const DvColumn& c, const Filter& f, size_t r) {
  if (f.op == 0) return true;
  if (f.op == 8) {
    const std::string s = c.data.text ? c.data.str[r] : Cell(c, r);
    return engine::Lower(s).find(f.text) != std::string::npos;
  }
  if (c.data.text) return false;
  const double v = c.data.num[r];
  if (!std::isfinite(v)) return false;
  switch (f.op) {
    case 1: return v > f.a;
    case 2: return v >= f.a;
    case 3: return v < f.a;
    case 4: return v <= f.a;
    case 5: return v == f.a;
    case 6: return v != f.a;
    case 7: return v >= std::min(f.a, f.b) && v <= std::max(f.a, f.b);
    default: return true;
  }
}

void Rebuild(DataView& v) {
  std::vector<Filter> fs;
  bool any = false;
  for (const auto& c : v.cols) {
    fs.push_back(ParseFilter(c.filter, c.data.text));
    any = any || fs.back().op != 0;
  }
  v.order.resize(v.rows);
  std::iota(v.order.begin(), v.order.end(), 0u);
  if (any) {
    // each row checked against every filter, on all cores
    std::vector<uint8_t> keep(v.rows, 1);
    std::vector<size_t> idx(v.rows);
    std::iota(idx.begin(), idx.end(), size_t{0});
    std::for_each(std::execution::par, idx.begin(), idx.end(), [&](size_t r) {
      for (size_t c = 0; c < v.cols.size(); ++c) {
        if (!Pass(v.cols[c], fs[c], r)) {
          keep[r] = 0;
          return;
        }
      }
    });
    size_t w = 0;
    for (size_t r = 0; r < v.rows; ++r) {
      if (keep[r]) v.order[w++] = static_cast<uint32_t>(r);
    }
    v.order.resize(w);
  }
  if (v.sort_col >= 0 && static_cast<size_t>(v.sort_col) < v.cols.size()) {
    const auto& c = v.cols[static_cast<size_t>(v.sort_col)];
    const bool desc = v.sort_desc;
    if (c.data.text) {
      std::stable_sort(std::execution::par, v.order.begin(), v.order.end(),
                       [&](uint32_t a, uint32_t b) { return desc ? c.data.str[b] < c.data.str[a] : c.data.str[a] < c.data.str[b]; });
    } else {
      // missing values last either way
      std::stable_sort(std::execution::par, v.order.begin(), v.order.end(), [&](uint32_t a, uint32_t b) {
        const double x = c.data.num[a], y = c.data.num[b];
        if (!std::isfinite(x)) return false;
        if (!std::isfinite(y)) return true;
        return desc ? y < x : x < y;
      });
    }
  }
  v.dirty = false;
  ++v.order_version;
}

// the shown rows (at most 1,048,576) on a new sheet
void ToSheet(AppState& s, const DataView& v) {
  const size_t n = std::min<size_t>(v.order.size(), 1048575);
  const uint32_t ncols = static_cast<uint32_t>(v.cols.size());
  std::vector<core::Value> vals;
  vals.reserve((n + 1) * ncols);
  for (const auto& c : v.cols) vals.push_back(core::Value::String(c.name));
  for (size_t i = 0; i < n; ++i) {
    const size_t r = v.order[i];
    for (const auto& c : v.cols) {
      if (c.data.text) vals.push_back(core::Value::String(c.data.str[r]));
      else if (!std::isfinite(c.data.num[r])) vals.push_back(core::Value());
      else vals.push_back(core::Value::Number(c.data.date ? c.data.num[r] / 86400.0 + 25569.0 : c.data.num[r]));
    }
  }
  std::vector<uint32_t> date_cols;
  for (uint32_t k = 0; k < ncols; ++k) {
    if (v.cols[k].data.date) date_cols.push_back(k);
  }
  const uint32_t id = s.engine->Call([&](engine::Document& d) {
    d.Begin(TR("Data View to Sheet"));
    const uint32_t sid = d.AddSheet();
    d.SetValues(sid, Rect{0, 0, static_cast<uint32_t>(n), ncols - 1}, vals, engine::kSrcUser);
    const uint32_t fmt = d.wb_mut().styles.mut().InternString("yyyy-mm-dd hh:mm:ss");
    for (uint32_t k : date_cols) d.ModifyStyle(sid, Rect{1, k, static_cast<uint32_t>(n), k}, [fmt](model::Style& st) { st.numfmt = fmt; });
    d.Commit();
    return sid;
  });
  s.engine->Sync();
  s.snap = s.engine->Current();
  ActivateSheet(s, id);
  s.SetStatus(std::to_string(n) + TR(" rows written to a new sheet") + (n < v.order.size() ? TR(" (the sheet's limit)") : std::string()), 6.0);
}

}  // namespace

// dataview.open {title, columns: [{name, path, column}]} / dataview.close {id} / dataview.filter {id, column, filter} /
// dataview.sort {id, column, descending} / dataview.state {id}
io::MV DataViewRequest(AppState& /*s*/, const std::string& m, const io::MV& p, std::string* err) {
  auto& views = Views();
  if (m == "dataview.open") {
    DataView v;
    v.id = g_next_view++;
    v.title = p.GetStr("title", "Data");
    if (const MV* cols = p.Get("columns")) {
      for (const MV& c : cols->a) {
        DvColumn col;
        col.name = c.GetStr("name");
        std::string why;
        if (!ReadNpy(FromUtf8(c.GetStr("path")), &col.data, &why, static_cast<int>(c.GetInt("column", -1)))) {
          *err = col.name + ": " + why;
          return MV();
        }
        const size_t n = col.data.text ? col.data.str.size() : col.data.num.size();
        if (!v.cols.empty() && n != v.rows) {
          *err = col.name + " has " + std::to_string(n) + " rows, the others " + std::to_string(v.rows);
          return MV();
        }
        v.rows = n;
        v.cols.push_back(std::move(col));
      }
    }
    if (v.cols.empty()) {
      *err = "No columns";
      return MV();
    }
    if (v.rows > 0xFFFFFFFFull) {
      *err = "At most 4,294,967,295 rows";
      return MV();
    }
    const int64_t id = v.id;
    views[id] = std::move(v);
    ui::RequestRedrawIn(0.0);
    return MV::Int(id);
  }
  auto it = views.find(p.GetInt("id", 0));
  if (it == views.end()) {
    *err = "No data view " + std::to_string(p.GetInt("id", 0)) + " (it was closed)";
    return MV();
  }
  DataView& v = it->second;
  auto col_index = [&](const std::string& name) -> int {
    for (size_t c = 0; c < v.cols.size(); ++c) {
      if (engine::Lower(v.cols[c].name) == engine::Lower(name)) return static_cast<int>(c);
    }
    return -1;
  };
  ui::RequestRedrawIn(0.0);
  if (m == "dataview.close") {
    views.erase(it);
    return MV::Bool(true);
  }
  if (m == "dataview.filter") {
    const int c = col_index(p.GetStr("column"));
    if (c < 0) {
      *err = "No column " + p.GetStr("column");
      return MV();
    }
    std::snprintf(v.cols[static_cast<size_t>(c)].filter, sizeof(v.cols[0].filter), "%s", p.GetStr("filter").c_str());
    Rebuild(v);
    return MV::Int(static_cast<int64_t>(v.order.size()));
  }
  if (m == "dataview.sort") {
    v.sort_col = p.GetStr("column").empty() ? -1 : col_index(p.GetStr("column"));
    v.sort_desc = p.GetBool("descending", false);
    Rebuild(v);
    return MV::Int(static_cast<int64_t>(v.order.size()));
  }
  if (m == "dataview.state") {
    if (v.dirty) Rebuild(v);
    MV x = MV::Map();
    x.Set("rows", MV::Int(static_cast<int64_t>(v.rows)));
    x.Set("shown", MV::Int(static_cast<int64_t>(v.order.size())));
    MV first = MV::Array();  // the first shown row numbers (0-based in the data), for checks
    for (size_t i = 0; i < v.order.size() && i < 20; ++i) first.Push(MV::Int(v.order[i]));
    x.Set("first", std::move(first));
    return x;
  }
  *err = "Unknown data view request: " + m;
  return MV();
}

void DrawDataViews(AppState& s) {
  auto& views = Views();
  for (auto it = views.begin(); it != views.end();) {
    DataView& v = it->second;
    if (!v.open) {
      it = views.erase(it);
      continue;
    }
    if (v.dirty) Rebuild(v);
    ImGui::SetNextWindowSize(ImVec2(S(900), S(560)), ImGuiCond_FirstUseEver);
    PlaceNewWindow(WindowPlace::Center);
    const std::string title = v.title + "  (" + Grouped(v.rows) + TR(" rows") + ")###yc_view" + std::to_string(v.id);
    if (ImGui::Begin(title.c_str(), &v.open, ImGuiWindowFlags_NoSavedSettings)) {
      // the footer: shown rows, statistics of one column, actions
      const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
      const ImGuiTableFlags tf = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_Hideable;
      const int ncols = static_cast<int>(v.cols.size()) + 1;
      if (ImGui::BeginTable("##data", ncols, tf, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(1, 2);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, S(70));
        for (const auto& c : v.cols) ImGui::TableSetupColumn(c.name.c_str(), c.data.text ? ImGuiTableColumnFlags_None : ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsDirty) {
          v.sort_col = ss->SpecsCount ? ss->Specs[0].ColumnIndex - 1 : -1;
          v.sort_desc = ss->SpecsCount && ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
          ss->SpecsDirty = false;
          Rebuild(v);
        }
        // the filter row: ">100", "a..b", text
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", TR("filter"));
        for (size_t c = 0; c < v.cols.size(); ++c) {
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-1);
          ImGui::PushID(static_cast<int>(c));
          if (ImGui::InputTextWithHint("##f", v.cols[c].data.text ? TR("contains") : ">x  <x  a..b", v.cols[c].filter, sizeof(v.cols[c].filter))) v.dirty = true;
          ImGui::PopID();
        }
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(std::min<size_t>(v.order.size(), 0x7FFFFFFF)));
        while (clip.Step()) {
          for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const size_t r = v.order[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", r + 1);
            for (const auto& c : v.cols) {
              ImGui::TableNextColumn();
              const std::string t = Cell(c, r);
              if (!c.data.text) {  // numbers to the right
                const float w = ImGui::CalcTextSize(t.c_str()).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
              }
              ImGui::TextUnformatted(t.c_str());
            }
          }
        }
        ImGui::EndTable();
      }
      ImGui::Text("%s %s / %s", TR("Rows shown:"), Grouped(v.order.size()).c_str(), Grouped(v.rows).c_str());
      ImGui::SameLine();
      ImGui::SetNextItemWidth(S(160));
      std::vector<const char*> names;
      for (const auto& c : v.cols) names.push_back(c.name.c_str());
      ImGui::Combo("##stat", &v.stats_col, names.data(), static_cast<int>(names.size()));
      ImGui::SameLine();
      if (ImGui::Button(TR("To Sheet"))) ToSheet(s, v);
      ImGui::SetItemTooltip("%s", TR("Write the shown rows (at most 1,048,575) to a new sheet."));
      if (v.stats_col >= 0 && static_cast<size_t>(v.stats_col) < v.cols.size() && !v.cols[static_cast<size_t>(v.stats_col)].data.text) {
        const auto& c = v.cols[static_cast<size_t>(v.stats_col)];
        auto& st = v.stats;
        if (st.col != v.stats_col || st.version != v.order_version) {  // millions of rows: once, not every frame
          double sum = 0, lo = INFINITY, hi = -INFINITY;
          size_t n = 0;
          for (uint32_t r : v.order) {
            const double x = c.data.num[r];
            if (!std::isfinite(x)) continue;
            sum += x, lo = std::min(lo, x), hi = std::max(hi, x), ++n;
          }
          st.col = v.stats_col, st.version = v.order_version, st.n = n;
          st.mean = n ? sum / static_cast<double>(n) : 0, st.lo = lo, st.hi = hi;
        }
        ImGui::SameLine();
        if (st.n)
          ImGui::TextDisabled("n %s   %s %s   %s %s   %s %s", Grouped(st.n).c_str(), TR("mean"), Number(c, st.mean).c_str(), TR("min"),
                              Number(c, st.lo).c_str(), TR("max"), Number(c, st.hi).c_str());
      }
    }
    ImGui::End();
    ++it;
  }
}

}  // namespace yc::app
