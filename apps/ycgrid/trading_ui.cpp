// SPDX-License-Identifier: Apache-2.0
// trading_ui.cpp - [YC] trading screens (ROADMAP phase 3): change flashes of bound columns, click actions on cell ranges
// (click-to-trade), Application.OnKey hotkeys, the cell context Python reads in handlers, and alert rules
#include "app_state.h"
#include "core/i18n.h"
#include "python_bridge.h"

#include "commands/command_registry.h"
#include "engine/bind.h"
#include "fonts/fonts.h"
#include "formula/numfmt.h"
#include "frame/frame_context.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>

#include <shellapi.h>

namespace yc::app {

using io::MV;
using ui::S;

namespace {

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

ImU32 ArgbToImU32(uint32_t argb, int alpha) {
  return IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, std::clamp(alpha, 0, 255));
}

struct Toast {
  std::string title, text;
  double until = 0;
  uint32_t color = 0xFFFFC107;
};

struct AlertState {
  std::set<uint64_t> active;              // cells meeting the condition
  std::map<uint64_t, double> last_value;  // for "the value changes"
  double last_fire = -1e9;
};

struct TradingUi {
  std::vector<Toast> toasts;
  std::map<std::string, AlertState> alerts;  // by lowercase rule name
  uint64_t alerts_version = 0;
};

TradingUi& U() {
  static TradingUi u;
  return u;
}

// Windows notifications for alerts: a notification-area icon that is added with the first alert and removed at exit
struct TrayIcon {
  NOTIFYICONDATAW nid{};
  bool added = false;
  ~TrayIcon() {
    if (added) ::Shell_NotifyIconW(NIM_DELETE, &nid);
  }
};

void Notify(HWND hwnd, const std::string& title, const std::string& text) {
  static TrayIcon tray;
  if (!tray.added) {
    tray.nid.cbSize = sizeof(tray.nid);
    tray.nid.hWnd = hwnd;
    tray.nid.uID = 0x5943;
    tray.nid.uFlags = NIF_ICON | NIF_TIP;
    tray.nid.hIcon = reinterpret_cast<HICON>(::GetClassLongPtrW(hwnd, GCLP_HICONSM));
    if (!tray.nid.hIcon) tray.nid.hIcon = ::LoadIconW(nullptr, IDI_WARNING);
    wcsncpy_s(tray.nid.szTip, L"YCGrid", _TRUNCATE);
    tray.added = ::Shell_NotifyIconW(NIM_ADD, &tray.nid) != FALSE;
    if (!tray.added) return;
  }
  tray.nid.uFlags = NIF_INFO;
  tray.nid.dwInfoFlags = NIIF_WARNING;
  wcsncpy_s(tray.nid.szInfoTitle, FromUtf8(title).c_str(), _TRUNCATE);
  wcsncpy_s(tray.nid.szInfo, FromUtf8(text).c_str(), _TRUNCATE);
  if (!::Shell_NotifyIconW(NIM_MODIFY, &tray.nid)) {
    // the icon is gone (Explorer restarted): add it again with the message
    tray.nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    tray.added = ::Shell_NotifyIconW(NIM_ADD, &tray.nid) != FALSE;
  }
}

MV ValueToMV(const core::Value& v) {
  if (v.IsEmpty()) return MV();
  if (v.IsNumeric()) return MV::Double(v.AsDouble());
  if (v.IsBool()) return MV::Bool(v.boolean());
  if (v.IsString()) return MV::Str(std::string(v.str()));
  return MV::Str(engine::DisplayTextOf(v, "General"));
}

uint64_t CellKey(uint32_t sheet, uint32_t r, uint32_t c) { return (uint64_t{sheet} << 48) | (uint64_t{r} << 16) | c; }

bool Meets(const model::AlertRule& a, const core::Value& v, AlertState& st, uint64_t key) {
  if (a.op == 9) {
    if (!v.IsNumeric()) return false;
    const double x = v.AsDouble();
    auto it = st.last_value.find(key);
    const bool changed = it != st.last_value.end() && it->second != x;
    st.last_value[key] = x;
    return changed;
  }
  if (!v.IsNumeric()) return false;
  const double x = v.AsDouble();
  switch (a.op) {
    case 1: return x > a.a;
    case 2: return x < a.a;
    case 3: return x >= a.a;
    case 4: return x <= a.a;
    case 5: return x == a.a;
    case 6: return x != a.a;
    case 7: return x >= std::min(a.a, a.b) && x <= std::max(a.a, a.b);
    case 8: return x < std::min(a.a, a.b) || x > std::max(a.a, a.b);
    default: return false;
  }
}

const char* OpText(int op) {
  static const char* k[] = {"", ">", "<", ">=", "<=", "=", "<>", "between", "outside", "changed"};
  return op >= 1 && op <= 9 ? k[op] : "?";
}

}  // namespace

// ---- the cell context: what a click, a hotkey or a context menu handler needs to know ----

io::MV CellContextMV(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  MV o = MV::Map();
  if (!s.snap) return o;
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh) return o;
  o.Set("sheet", MV::Int(sheet));
  o.Set("sheet_name", MV::Str(sh->name));
  o.Set("row", MV::Int(r));
  o.Set("col", MV::Int(c));
  o.Set("address", MV::Str(core::CellA1(r, c, true, true)));
  const core::Value& v = sh->GetValue(r, c);
  o.Set("value", ValueToMV(v));
  o.Set("text", MV::Str(engine::DisplayTextOf(v, s.snap->wb.styles->NumberFormat(s.snap->wb.styles->Get(sh->GetStyle(r, c))))));
  // in a table: its name, the column and the whole row by column name (e.g. the strike of a row of an option chain)
  if (const model::Table* t = model::TableAt(s.snap->wb, sheet, r, c)) {
    o.Set("table", MV::Str(t->name));
    if (c - t->range.c0 < t->columns.size()) o.Set("column", MV::Str(t->columns[c - t->range.c0].name));
    const core::Rect d = t->Data();
    if (t->HasData() && r >= d.r0 && r <= d.r1) {
      o.Set("table_row", MV::Int(r - d.r0));
      MV rec = MV::Map();
      for (uint32_t i = 0; i < t->columns.size(); ++i) rec.Set(t->columns[i].name, ValueToMV(sh->GetValue(r, t->range.c0 + i)));
      o.Set("record", std::move(rec));
    }
  }
  return o;
}

// ---- column renderers of bound tables: bars, two-sided bars, heat, progress, trends (drawn every frame) ----
namespace {
// the recent values of each row of a trend column (the UI samples the cell when it changes)
struct Trend {
  std::vector<std::deque<float>> rows;
  std::vector<double> last;
};
std::map<std::string, Trend>& Trends() {
  static std::map<std::string, Trend> m;
  return m;
}
}  // namespace

template <class CellRect>
void DrawColumnRenderers(AppState& s, uint32_t sheet_id, const model::Sheet& sh, const render::OverlayGeom& g, int64_t first_row, int64_t last_row,
                         const CellRect& cell_rect) {
  (void)g;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const auto& b : *s.snap->wb.bound_tables) {
    if (b.sheet != sheet_id) continue;
    const model::Table* t = nullptr;
    for (const auto& c : b.columns) {
      if (!c.render) continue;
      if (!t) t = model::FindTable(s.snap->wb, b.name);
      if (!t || !t->HasData()) break;
      const int ci = t->ColumnIndex(c.name);
      if (ci < 0) continue;
      const core::Rect d = t->Data();
      const uint32_t col = t->range.c0 + static_cast<uint32_t>(ci);
      auto num = [&](int64_t r, double* v) {
        const core::Value& x = sh.GetValue(static_cast<uint32_t>(r), col);
        if (!x.IsNumeric()) return false;
        *v = x.AsDouble();
        return std::isfinite(*v);
      };
      if (c.render == 5) {  // trend: sample every row (also off screen), draw the visible ones
        Trend& tr = Trends()[engine::Lower(b.name) + "|" + engine::Lower(c.name)];
        const size_t n = static_cast<size_t>(d.r1 - d.r0 + 1);
        if (tr.rows.size() != n) tr.rows.resize(n), tr.last.resize(n, std::numeric_limits<double>::quiet_NaN());
        for (size_t i = 0; i < n && i < 100000; ++i) {
          double v;
          if (!num(static_cast<int64_t>(d.r0 + i), &v) || v == tr.last[i]) continue;
          tr.last[i] = v;
          tr.rows[i].push_back(static_cast<float>(v));
          while (tr.rows[i].size() > static_cast<size_t>(std::max(2, c.trend_len))) tr.rows[i].pop_front();
        }
        const ImU32 line = c.render_color ? ArgbToImU32(c.render_color, 255) : IM_COL32(0x2F, 0x6F, 0xED, 255);
        for (int64_t r = std::max<int64_t>(d.r0, first_row); r <= std::min<int64_t>(d.r1, last_row); ++r) {
          const auto& h = tr.rows[static_cast<size_t>(r - d.r0)];
          if (h.size() < 2) continue;
          const auto [lo_it, hi_it] = std::minmax_element(h.begin(), h.end());
          const float lo = *lo_it, span = std::max(1e-12f, *hi_it - lo);
          const ImRect rc = cell_rect(static_cast<uint32_t>(r), col);
          const float pad = std::min(3.0f, rc.GetHeight() * 0.15f);
          ImVec2 prev;
          for (size_t k = 0; k < h.size(); ++k) {
            const ImVec2 p(rc.Min.x + pad + (rc.GetWidth() - 2 * pad) * static_cast<float>(k) / static_cast<float>(h.size() - 1),
                           rc.Max.y - pad - (rc.GetHeight() - 2 * pad) * (h[k] - lo) / span);
            if (k) dl->AddLine(prev, p, line, 1.3f);
            prev = p;
          }
          // the last value rose (up color) or fell (down color)
          const bool up = h.back() >= h[h.size() - 2];
          dl->AddCircleFilled(prev, 2.0f, up ? IM_COL32(0xE5, 0x38, 0x3B, 255) : IM_COL32(0x16, 0xA3, 0x4A, 255));
        }
        continue;
      }
      // the column's scale: its largest absolute value, smallest / largest for heat (all rows, at most 100,000)
      double lo = std::numeric_limits<double>::infinity(), hi = -lo, amax = 0;
      for (int64_t r = d.r0; r <= d.r1 && r - d.r0 < 100000; ++r) {
        double v;
        if (!num(r, &v)) continue;
        lo = std::min(lo, v), hi = std::max(hi, v), amax = std::max(amax, std::abs(v));
      }
      if (!std::isfinite(lo)) continue;
      const double scale = c.render_max > 0 ? c.render_max : (amax > 0 ? amax : 1.0);
      const ImU32 pos_col = c.render_color ? ArgbToImU32(c.render_color, 0x70) : IM_COL32(0xE5, 0x38, 0x3B, 0x70);
      const ImU32 neg_col = c.render_neg ? ArgbToImU32(c.render_neg, 0x70) : IM_COL32(0x16, 0xA3, 0x4A, 0x70);
      for (int64_t r = std::max<int64_t>(d.r0, first_row); r <= std::min<int64_t>(d.r1, last_row); ++r) {
        double v;
        if (!num(r, &v)) continue;
        const ImRect rc = cell_rect(static_cast<uint32_t>(r), col);
        const float m = std::min(2.0f, rc.GetHeight() * 0.12f);
        const float w = rc.GetWidth() - 2 * m;
        if (c.render == 1) {  // bar from the left
          const float len = w * static_cast<float>(std::clamp(std::abs(v) / scale, 0.0, 1.0));
          dl->AddRectFilled(ImVec2(rc.Min.x + m, rc.Min.y + m), ImVec2(rc.Min.x + m + len, rc.Max.y - m), v >= 0 ? pos_col : neg_col);
        } else if (c.render == 2) {  // two-sided: zero in the middle
          const float mid = rc.Min.x + m + w * 0.5f, len = w * 0.5f * static_cast<float>(std::clamp(std::abs(v) / scale, 0.0, 1.0));
          dl->AddRectFilled(ImVec2(v >= 0 ? mid : mid - len, rc.Min.y + m), ImVec2(v >= 0 ? mid + len : mid, rc.Max.y - m), v >= 0 ? pos_col : neg_col);
          dl->AddLine(ImVec2(mid, rc.Min.y + m), ImVec2(mid, rc.Max.y - m), IM_COL32(0x80, 0x80, 0x80, 0x90));
        } else if (c.render == 3) {  // heat: low = negative color, high = positive color
          const float k = hi > lo ? static_cast<float>((v - lo) / (hi - lo)) : 0.5f;
          const ImVec4 a = ImGui::ColorConvertU32ToFloat4(neg_col), z = ImGui::ColorConvertU32ToFloat4(pos_col);
          const ImVec4 mix(a.x + (z.x - a.x) * k, a.y + (z.y - a.y) * k, a.z + (z.z - a.z) * k, 0.25f + 0.35f * std::abs(k - 0.5f) * 2);
          dl->AddRectFilled(rc.Min, rc.Max, ImGui::ColorConvertFloat4ToU32(mix));
        } else if (c.render == 4) {  // progress 0..1 (or of render_max)
          const double frac = std::clamp(c.render_max > 0 ? v / c.render_max : v, 0.0, 1.0);
          dl->AddRectFilled(ImVec2(rc.Min.x + m, rc.Max.y - m - 3), ImVec2(rc.Min.x + m + w * static_cast<float>(frac), rc.Max.y - m), pos_col | 0xFF000000);
        }
      }
    }
  }
}

// ---- drawing: change flashes, alert highlights; click actions ----

void DrawTradingOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (!s.snap) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  const ImVec2 top = g.to_sheet(g.cells.Min), bottom = g.to_sheet(g.cells.Max);
  const int64_t first_row = std::max<int64_t>(0, sh->rows->IndexAt(static_cast<int64_t>(top.y)));
  const int64_t last_row = sh->rows->IndexAt(static_cast<int64_t>(bottom.y));
  auto cell_rect = [&](uint32_t r, uint32_t c) {
    const ImVec2 a = g.to_screen(static_cast<double>(sh->cols->Offset(c)), static_cast<double>(sh->rows->Offset(r)));
    return ImRect(a, ImVec2(a.x + static_cast<float>(sh->cols->Size(c) * g.sc), a.y + static_cast<float>(sh->rows->Size(r) * g.sc)));
  };
  // change flashes of bound columns: up / down colors fading out
  bool flashing = false;
  const int64_t now_ns = engine::PerfCounterNs();
  for (const auto& b : *s.snap->wb.bound_tables) {
    if (b.sheet != sheet_id) continue;
    auto it = s.snap->bind_times.find(engine::Lower(b.name));
    if (it == s.snap->bind_times.end() || !it->second || it->second->cols.empty()) continue;
    const model::Table* t = model::FindTable(s.snap->wb, b.name);
    if (!t || !t->HasData()) continue;
    const core::Rect d = t->Data();
    const int64_t r0 = std::max<int64_t>(d.r0, first_row), r1 = std::min<int64_t>(d.r1, last_row);
    for (const auto& col : it->second->cols) {
      if (!col->flash || col->col >= t->columns.size()) continue;
      const int64_t window = static_cast<int64_t>(std::max(50, col->flash_ms)) * 1000000;
      for (int64_t r = r0; r <= r1; ++r) {
        const size_t i = static_cast<size_t>(r - d.r0);
        if (i >= col->t.size()) break;
        const int64_t tt = col->t[i].load(std::memory_order_relaxed);
        if (!tt || now_ns - tt >= window) continue;
        const int8_t dir = col->dir[i].load(std::memory_order_relaxed);
        const uint32_t argb = col->flash == 2 || dir >= 0 ? col->up : col->down;
        if (col->flash == 1 && dir == 0) continue;
        const float k = 1.0f - static_cast<float>(now_ns - tt) / static_cast<float>(window);
        const ImRect rc = cell_rect(static_cast<uint32_t>(r), t->range.c0 + col->col);
        dl->AddRectFilled(rc.Min, rc.Max, ArgbToImU32(argb, static_cast<int>(110 * k * k)));
        flashing = true;
      }
    }
  }
  if (flashing) ui::RequestRedrawIn(1.0 / 60);
  DrawColumnRenderers(s, sheet_id, *sh, g, first_row, last_row, cell_rect);
  // alert highlights
  for (const auto& a : *s.snap->wb.alerts) {
    if (a.sheet != sheet_id || !a.enabled || !a.color) continue;
    auto st = U().alerts.find(engine::Lower(a.name));
    if (st == U().alerts.end()) continue;
    for (uint64_t k : st->second.active) {
      const uint32_t r = static_cast<uint32_t>((k >> 16) & 0xFFFFFFFF), c = static_cast<uint32_t>(k & 0xFFFF);
      if (static_cast<int64_t>(r) < first_row || static_cast<int64_t>(r) > last_row) continue;
      const ImRect rc = cell_rect(r, c);
      dl->AddRectFilled(rc.Min, rc.Max, ArgbToImU32(a.color, 90));
      dl->AddRect(rc.Min, rc.Max, ArgbToImU32(a.color, 255), 0.0f, S(2));
    }
  }
  dl->PopClipRect();
  // click actions (click-to-trade): a mouse click on a registered range tells Python; arrow keys never do. A click is
  // a press and a release on the same cell, as a button's: a drag that starts on a size (selecting cells) sends nothing.
  // The press is noted per frame for every pane this overlay draws (frozen panes), the release may land in another.
  static struct {
    int button = -1;
    uint32_t sheet = 0;
    int64_t r = -1, c = -1;
    int frame = -1;
  } press;
  if (press.button >= 0 && press.frame != ImGui::GetFrameCount() && !ImGui::IsMouseDown(press.button) &&
      !ImGui::IsMouseReleased(press.button))
    press.button = -1;  // released outside the cells
  if (!s.click_ranges.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && g.cells.Contains(ImGui::GetMousePos())) {
    const ImVec2 p = g.to_sheet(ImGui::GetMousePos());
    const int64_t r = sh->rows->IndexAt(static_cast<int64_t>(p.y)), c = sh->cols->IndexAt(static_cast<int64_t>(p.x));
    const int pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? 0 : ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 1 : -1;
    if (pressed >= 0) {
      press = {pressed, sheet_id, r, c, ImGui::GetFrameCount()};
      for (const auto& cr : s.click_ranges)  // the cell's own menu stays shut on a right click there
        if (pressed == 1 && cr.right && cr.sheet == sheet_id && r >= 0 && c >= 0 && cr.rect.Contains(static_cast<uint32_t>(r), static_cast<uint32_t>(c)))
          s.suppress_cell_menu = true;
    }
    int button = -1;
    if (press.button >= 0 && ImGui::IsMouseReleased(press.button)) {
      if (press.sheet == sheet_id && press.r == r && press.c == c) button = press.button;
      press.button = -1;
    }
    const bool dbl = button == 0 && ImGui::GetIO().MouseClickedLastCount[0] >= 2;
    if (button >= 0) {
      for (const auto& cr : s.click_ranges) {
        if (cr.sheet != sheet_id || r < 0 || c < 0 || !cr.rect.Contains(static_cast<uint32_t>(r), static_cast<uint32_t>(c))) continue;
        if ((button == 0 && !cr.left) || (button == 1 && !cr.right)) continue;
        if (s.python && s.python->HostReady()) {
          MV pm = CellContextMV(s, sheet_id, static_cast<uint32_t>(r), static_cast<uint32_t>(c));
          pm.Set("id", MV::Int(cr.id));
          pm.Set("button", MV::Str(button == 0 ? "left" : "right"));
          pm.Set("double", MV::Bool(dbl));
          pm.Set("ctrl", MV::Bool(ImGui::GetIO().KeyCtrl));
          pm.Set("shift", MV::Bool(ImGui::GetIO().KeyShift));
          pm.Set("alt", MV::Bool(ImGui::GetIO().KeyAlt));
          s.python->NotifyHost("click", pm);
        }
        break;
      }
    }
  }
}

// ---- alerts: evaluated when a new snapshot arrives ----

void DrawTradingFrame(AppState& s) {
  TradingUi& u = U();
  const double now = Now();
  if (s.snap && s.snap->version != u.alerts_version && !s.snap->wb.alerts->empty()) {
    u.alerts_version = s.snap->version;
    for (const auto& a : *s.snap->wb.alerts) {
      AlertState& st = u.alerts[engine::Lower(a.name)];
      const model::Sheet* sh = s.snap->Sheet(a.sheet);
      if (!sh || !a.enabled) {
        st.active.clear();
        continue;
      }
      std::set<uint64_t> now_active;
      std::vector<uint64_t> fresh;
      size_t n = 0;
      for (uint32_t r = a.range.r0; r <= a.range.r1 && n < 50000; ++r) {
        for (uint32_t c = a.range.c0; c <= a.range.c1 && n < 50000; ++c, ++n) {
          const uint64_t k = CellKey(a.sheet, r, c);
          if (!Meets(a, sh->GetValue(r, c), st, k)) continue;
          now_active.insert(k);
          if (!st.active.count(k)) fresh.push_back(k);
        }
      }
      st.active = a.op == 9 ? std::set<uint64_t>(fresh.begin(), fresh.end()) : std::move(now_active);
      if (fresh.empty() || now - st.last_fire < a.cooldown_s) continue;
      st.last_fire = now;
      const uint64_t k = fresh.front();
      const uint32_t r = static_cast<uint32_t>((k >> 16) & 0xFFFFFFFF), c = static_cast<uint32_t>(k & 0xFFFF);
      const core::Value& v = sh->GetValue(r, c);
      char cond[96];
      if (a.op == 7 || a.op == 8) std::snprintf(cond, sizeof cond, "%s %g..%g", OpText(a.op), a.a, a.b);
      else if (a.op == 9) std::snprintf(cond, sizeof cond, "%s", OpText(a.op));
      else std::snprintf(cond, sizeof cond, "%s %g", OpText(a.op), a.a);
      std::string text = sh->name + "!" + core::CellA1(r, c) + " = " + engine::DisplayTextOf(v, "General") + "  (" + cond + ")";
      if (fresh.size() > 1) text += TR(" and ") + std::to_string(fresh.size() - 1) + TR(" more");
      if (a.sound) ::MessageBeep(MB_ICONEXCLAMATION);
      if (a.popup) u.toasts.push_back(Toast{a.name, text, now + 8, a.color ? a.color : 0xFFFFC107});
      if (a.flash_window) {
        FLASHWINFO fi{sizeof(fi), s.hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 5, 0};
        ::FlashWindowEx(&fi);
      }
      if (a.notify && !s.headless) Notify(s.hwnd, a.name, text);
      if (s.python) s.python->TapeMark("alert", a.name + ": " + text);  // the time machine's timeline
      model::AuditEntry e;
      e.kind = "alert";
      e.target = a.name;
      e.after = text;
      e.source = "ycgrid";
      e.user = "ycgrid";
      s.engine->Post([e = std::move(e)](engine::Document& d) mutable { d.AddAudit(std::move(e)); });
      if (s.python && s.python->HostReady()) {
        MV pm = CellContextMV(s, a.sheet, r, c);
        pm.Set("name", MV::Str(a.name));
        pm.Set("message", MV::Str(text));
        pm.Set("count", MV::Int(static_cast<int64_t>(fresh.size())));
        s.python->NotifyHost("alert", pm);
      }
    }
  }
  // popups: top right, newest first, gone after 8 s (click to dismiss)
  u.toasts.erase(std::remove_if(u.toasts.begin(), u.toasts.end(), [&](const Toast& t) { return t.until < now; }), u.toasts.end());
  if (!u.toasts.empty()) {
    ui::RequestRedrawIn(0.5);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->Pos.y + S(110);
    for (size_t i = u.toasts.size(); i-- > 0;) {
      Toast& t = u.toasts[i];
      ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - S(16), y), ImGuiCond_Always, ImVec2(1, 0));
      ImGui::SetNextWindowBgAlpha(0.97f);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ArgbToImU32(t.color, 255));
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0x20, 0x20, 0x20, 255));
      const std::string id = "##yc_toast" + std::to_string(i);
      ImGui::Begin(id.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
      ImGui::PushFont(ui::F().ui_bold, ui::F().ui_size + 1);
      ImGui::TextUnformatted((std::string("\xe2\x9a\xa0 ") + t.title).c_str());
      ImGui::PopFont();
      ImGui::TextUnformatted(t.text.c_str());
      if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) t.until = 0;
      y += ImGui::GetWindowHeight() + S(8);
      ImGui::End();
      ImGui::PopStyleColor(2);
    }
  }
}

// ---- Application.OnKey ----

// Excel's key text: ^ Ctrl, + Shift, % Alt, then a key: a letter, a digit, or {F5}, {ENTER}, {ESC}, {DEL}, {UP}...
bool ParseOnKey(const std::string& text, int* out) {
  ImGuiKeyChord mods = 0;
  size_t i = 0;
  for (; i < text.size() && (text[i] == '^' || text[i] == '+' || text[i] == '%'); ++i) {
    mods |= text[i] == '^' ? ImGuiMod_Ctrl : text[i] == '+' ? ImGuiMod_Shift : ImGuiMod_Alt;
  }
  std::string k = text.substr(i);
  if (k.size() >= 2 && k.front() == '{' && k.back() == '}') k = k.substr(1, k.size() - 2);
  std::string up;
  for (char ch : k) up += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  ImGuiKey key = ImGuiKey_None;
  if (up.size() == 1 && up[0] >= 'A' && up[0] <= 'Z') key = static_cast<ImGuiKey>(ImGuiKey_A + (up[0] - 'A'));
  else if (up.size() == 1 && up[0] >= '0' && up[0] <= '9') key = static_cast<ImGuiKey>(ImGuiKey_0 + (up[0] - '0'));
  else if (up.size() >= 2 && up[0] == 'F' && std::isdigit(static_cast<unsigned char>(up[1]))) {
    const int n = std::atoi(up.c_str() + 1);
    if (n >= 1 && n <= 24) key = static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
  } else {
    static const std::map<std::string, ImGuiKey> kNames = {
        {"ENTER", ImGuiKey_Enter}, {"~", ImGuiKey_Enter}, {"RETURN", ImGuiKey_Enter}, {"ESC", ImGuiKey_Escape}, {"ESCAPE", ImGuiKey_Escape},
        {"DEL", ImGuiKey_Delete}, {"DELETE", ImGuiKey_Delete}, {"BS", ImGuiKey_Backspace}, {"BACKSPACE", ImGuiKey_Backspace},
        {"TAB", ImGuiKey_Tab}, {"UP", ImGuiKey_UpArrow}, {"DOWN", ImGuiKey_DownArrow}, {"LEFT", ImGuiKey_LeftArrow}, {"RIGHT", ImGuiKey_RightArrow},
        {"HOME", ImGuiKey_Home}, {"END", ImGuiKey_End}, {"PGUP", ImGuiKey_PageUp}, {"PGDN", ImGuiKey_PageDown}, {"INSERT", ImGuiKey_Insert},
        {"INS", ImGuiKey_Insert}, {" ", ImGuiKey_Space}, {"SPACE", ImGuiKey_Space}};
    if (auto it = kNames.find(up); it != kNames.end()) key = it->second;
  }
  if (key == ImGuiKey_None) return false;
  *out = mods | key;
  return true;
}

// called by Shortcuts() before the built-in shortcuts; true = the key was taken (run, or disabled with "")
bool HandleOnKeys(AppState& s) {
  for (const auto& ok : s.on_keys) {
    if (!ImGui::IsKeyChordPressed(ok.chord)) continue;
    if (!ok.procedure.empty() && s.python && s.python->HostReady()) {
      const auto& sel = s.FocusView().sel;
      MV pm = CellContextMV(s, s.focus_sheet, sel.act_r, sel.act_c);
      pm.Set("key", MV::Str(ok.key));
      pm.Set("procedure", MV::Str(ok.procedure));
      s.python->NotifyHost("onkey", pm);
    }
    return true;
  }
  return false;
}

}  // namespace yc::app
