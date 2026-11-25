// SPDX-License-Identifier: Apache-2.0
#include "titlebar/titlebar.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

namespace yc::ui::titlebar {
namespace {

State g_state;

constexpr float kHeight = 34.0f;
constexpr float kButtonW = 46.0f;

bool PtIn(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

RECT ToRect(const ImRect& r, const ImVec2& origin) {
  RECT out;
  out.left = static_cast<LONG>(r.Min.x - origin.x);
  out.top = static_cast<LONG>(r.Min.y - origin.y);
  out.right = static_cast<LONG>(r.Max.x - origin.x);
  out.bottom = static_cast<LONG>(r.Max.y - origin.y);
  return out;
}

int FrameThickness(HWND hwnd) {
  const UINT dpi = ::GetDpiForWindow(hwnd);
  return ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) + ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

void DrawGlyph(ImDrawList* dl, const char* glyph, ImVec2 center, float base, ImU32 col) {
  ImFont* font = F().icons;
  const float px = FontPx(base);
  const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, glyph);
  dl->AddText(font, px, ImVec2(IM_ROUND(center.x - sz.x * 0.5f), IM_ROUND(center.y - sz.y * 0.5f)), col, glyph);
}

}  // namespace

State& St() { return g_state; }

float Height() { return S(kHeight); }

void Install(HWND hwnd) {
  const MARGINS margins = {0, 0, 1, 0};  // leave 1 px so DWM keeps drawing the shadow
  ::DwmExtendFrameIntoClientArea(hwnd, &margins);
  ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT* result) {
  switch (msg) {
    case WM_NCCALCSIZE: {
      if (!wparam) return false;
      if (::IsZoomed(hwnd)) {
        // when maximized the window overflows the screen by one frame thickness; pull it back in
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
        const int f = FrameThickness(hwnd);
        p->rgrc[0].left += f;
        p->rgrc[0].top += f;
        p->rgrc[0].right -= f;
        p->rgrc[0].bottom -= f;
      }
      *result = 0;
      return true;
    }
    case WM_NCACTIVATE:
      // lparam = -1: don't redraw the system frame
      *result = ::DefWindowProcW(hwnd, msg, wparam, -1);
      return true;
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd, &p);
      RECT rc;
      ::GetClientRect(hwnd, &rc);
      if (!::IsZoomed(hwnd)) {
        const int f = FrameThickness(hwnd);
        const bool left = p.x < f, right = p.x >= rc.right - f, top = p.y < f, bottom = p.y >= rc.bottom - f;
        if (top && left) { *result = HTTOPLEFT; return true; }
        if (top && right) { *result = HTTOPRIGHT; return true; }
        if (bottom && left) { *result = HTBOTTOMLEFT; return true; }
        if (bottom && right) { *result = HTBOTTOMRIGHT; return true; }
        if (left) { *result = HTLEFT; return true; }
        if (right) { *result = HTRIGHT; return true; }
        if (top) { *result = HTTOP; return true; }
        if (bottom) { *result = HTBOTTOM; return true; }
      }
      if (PtIn(g_state.max_button, p)) { *result = HTMAXBUTTON; return true; }
      for (const RECT& ir : g_state.interactive) {
        if (PtIn(ir, p)) { *result = HTCLIENT; return true; }
      }
      if (PtIn(g_state.caption, p)) { *result = HTCAPTION; return true; }
      *result = HTCLIENT;
      return true;
    }
    case WM_NCMOUSEMOVE: {
      const bool hover = wparam == HTMAXBUTTON;
      if (hover != g_state.max_hover) {
        g_state.max_hover = hover;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      // leave tracking (TME_NONCLIENT) is handled by ImGui's Win32 backend; not repeated here.
      return false;
    }
    case WM_NCMOUSELEAVE:
    case WM_MOUSELEAVE:
      g_state.max_hover = false;
      g_state.max_pressed = false;
      return false;
    case WM_NCLBUTTONDOWN:
      if (wparam == HTMAXBUTTON) {
        g_state.max_pressed = true;
        *result = 0;
        return true;
      }
      return false;
    case WM_NCLBUTTONUP:
      if (wparam == HTMAXBUTTON) {
        if (g_state.max_pressed) ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        g_state.max_pressed = false;
        *result = 0;
        return true;
      }
      return false;
    default:
      return false;
  }
}

void Draw(HWND hwnd, const ImRect& r, const char* const* qat_cmds, int qat_count) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetMainViewport()->Pos;  // client origin (screen coordinates)
  const Theme& t = T();
  dl->AddRectFilled(r.Min, r.Max, t.title_bg);
  g_state.caption = ToRect(r, origin);
  g_state.interactive.clear();
  auto add_interactive = [&](const ImRect& ir) { g_state.interactive.push_back(ToRect(ir, origin)); };

  // app icon: our own drawing (grid + rising trend line), same design as resources/ycgrid.ico (tools/make_icon.py)
  float x = r.Min.x + S(10);
  {
    const ImVec2 c(x + S(9), r.GetCenter().y);
    const float h = S(8);
    dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), t.accent, S(3));
    for (int i = 1; i < 3; ++i) {
      const float o = -h + (2 * h) * static_cast<float>(i) / 3.0f;
      dl->AddRectFilled(ImVec2(c.x - h + S(3), c.y + o), ImVec2(c.x + h - S(3), c.y + o + 1), IM_COL32(255, 255, 255, 150));
      dl->AddRectFilled(ImVec2(c.x + o, c.y - h + S(3)), ImVec2(c.x + o + 1, c.y + h - S(3)), IM_COL32(255, 255, 255, 150));
    }
    const ImVec2 trend[] = {ImVec2(c.x - h * 0.70f, c.y + h * 0.45f), ImVec2(c.x - h * 0.20f, c.y - h * 0.05f),
                            ImVec2(c.x + h * 0.15f, c.y + h * 0.25f), ImVec2(c.x + h * 0.70f, c.y - h * 0.55f)};
    dl->AddPolyline(trend, 4, IM_COL32(0xF5, 0xB3, 0x42, 255), std::max(1.5f, S(1.8f)));
    x += S(28);
  }

  // Quick Access Toolbar
  ImGui::PushID("##qat");
  for (int i = 0; i < qat_count; ++i) {
    Command* c = Commands().Find(qat_cmds[i]);
    const ImRect br(ImVec2(x, r.Min.y + S(4)), ImVec2(x + S(30), r.Max.y - S(4)));
    ImGui::SetCursorScreenPos(br.Min);
    ImGui::PushID(i);
    const bool clicked = ImGui::InvisibleButton("##q", br.GetSize());
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (hovered && c && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s%s%s%s", c->label.c_str(), c->shortcut.empty() ? "" : " (", c->shortcut.c_str(),
                        c->shortcut.empty() ? "" : ")");
    }
    if (hovered) RequestRedrawIn(ImGui::GetStyle().HoverDelayNormal + 0.05);
    ImGui::PopID();
    if (held || hovered) dl->AddRectFilled(br.Min, br.Max, held ? t.title_btn_pressed : t.title_btn_hover, S(4));
    if (c && c->icon) DrawGlyph(dl, c->icon, br.GetCenter(), 16.0f, t.title_text);
    if (clicked && c) Commands().Execute(c->id);
    // KeyTips 1, 2, 3... (09, 08... after the ninth, like Excel)
    if (c) {
      const std::string key = i < 9 ? std::to_string(i + 1) : "0" + std::to_string(18 - i);
      const std::string id = c->id;
      ribbon::AddKeyTip(key, ImVec2(br.GetCenter().x, br.Max.y), [id] { Commands().Execute(id); });
    }
    add_interactive(br);
    x += S(32);
    // Undo / Redo: a drop-down arrow with the list of actions
    const bool redo = c && c->id == "Redo";
    if (c && (c->id == "Undo" || redo) && g_state.history && g_state.history_run) {
      const ImRect ar(ImVec2(x - S(2), r.Min.y + S(4)), ImVec2(x + S(12), r.Max.y - S(4)));
      ImGui::SetCursorScreenPos(ar.Min);
      ImGui::PushID(i);
      const bool open = ImGui::InvisibleButton(redo ? "##redo_hist" : "##undo_hist", ar.GetSize());
      const bool arrow_hover = ImGui::IsItemHovered();
      if (arrow_hover) dl->AddRectFilled(ar.Min, ar.Max, t.title_btn_hover, S(4));
      const ImVec2 ac = ar.GetCenter();
      dl->AddTriangleFilled(ImVec2(ac.x - S(3), ac.y - S(1)), ImVec2(ac.x + S(3), ac.y - S(1)), ImVec2(ac.x, ac.y + S(2)), t.title_text);
      static int hover_k = -1;
      if (open) {
        ImGui::OpenPopup("##histpop");
        hover_k = -1;
      }
      ImGui::SetNextWindowPos(ImVec2(br.Min.x, r.Max.y));
      if (ImGui::BeginPopup("##histpop")) {
        const auto labels = g_state.history(redo);
        if (labels.empty()) ImGui::TextDisabled("%s", redo ? TR("Can't Redo") : TR("Can't Undo"));
        ImGui::BeginChild("##hist_list", ImVec2(S(220), std::min(S(320), S(26) * static_cast<float>(std::max<size_t>(1, labels.size())) + S(6))));
        int now = -1;
        for (int k = 0; k < static_cast<int>(labels.size()); ++k) {
          ImGui::PushID(k);
          // the actions above the one pointed at go with it (Excel highlights them all)
          if (ImGui::Selectable(labels[static_cast<size_t>(k)].c_str(), k <= hover_k)) {
            g_state.history_run(redo, k + 1);
            ImGui::CloseCurrentPopup();
          }
          if (ImGui::IsItemHovered()) now = k;
          ImGui::PopID();
        }
        ImGui::EndChild();
        if (now >= 0) hover_k = now;
        const int n = std::max(1, hover_k + 1);
        char foot[64];
        std::snprintf(foot, sizeof(foot), redo ? TR("Redo %d Actions") : TR("Undo %d Actions"), n);
        ImGui::Separator();
        ImGui::TextDisabled("%s", hover_k < 0 ? TR("Cancel") : foot);
        ImGui::EndPopup();
      }
      ImGui::PopID();
      add_interactive(ar);
      x += S(12);
    }
  }
  // Customize Quick Access Toolbar
  if (g_state.qat_menu) {
    const ImRect br(ImVec2(x, r.Min.y + S(4)), ImVec2(x + S(20), r.Max.y - S(4)));
    ImGui::SetCursorScreenPos(br.Min);
    const bool clicked = ImGui::InvisibleButton("##qatmenu", br.GetSize());
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      dl->AddRectFilled(br.Min, br.Max, t.title_btn_hover, S(4));
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", TR("Customize Quick Access Toolbar"));
    }
    const ImVec2 c = br.GetCenter();
    dl->AddTriangleFilled(ImVec2(c.x - S(4), c.y - S(1)), ImVec2(c.x + S(4), c.y - S(1)), ImVec2(c.x, c.y + S(3)), t.title_text);
    dl->AddLine(ImVec2(c.x - S(4), c.y - S(4)), ImVec2(c.x + S(4), c.y - S(4)), t.title_text);
    if (clicked) ImGui::OpenPopup("##qatpopup");
    if (ImGui::BeginPopup("##qatpopup")) {
      g_state.qat_menu();
      ImGui::EndPopup();
    }
    add_interactive(br);
    x += S(24);
  }
  ImGui::PopID();

  // window buttons on the right
  const float bw = S(kButtonW);
  const ImRect close_r(ImVec2(r.Max.x - bw, r.Min.y), r.Max);
  const ImRect max_r(ImVec2(close_r.Min.x - bw, r.Min.y), ImVec2(close_r.Min.x, r.Max.y));
  const ImRect min_r(ImVec2(max_r.Min.x - bw, r.Min.y), ImVec2(max_r.Min.x, r.Max.y));
  g_state.max_button = ToRect(max_r, origin);

  // search box: finds commands by name (Alt+Q)
  {
    const float sw = std::min(S(360), std::max(0.0f, (min_r.Min.x - x) * 0.45f));
    if (sw > S(120)) {
      const ImRect sr(ImVec2(r.GetCenter().x - sw * 0.5f + S(60), r.Min.y + S(5)),
                      ImVec2(r.GetCenter().x + sw * 0.5f + S(60), r.Max.y - S(5)));
      dl->AddRectFilled(sr.Min, sr.Max, IM_COL32_WHITE, S(4));
      dl->AddRect(sr.Min, sr.Max, IM_COL32(0xD0, 0xD0, 0xD0, 255), S(4));
      DrawGlyph(dl, icon::Search, ImVec2(sr.Min.x + S(16), sr.GetCenter().y), 16.0f, IM_COL32(0x60, 0x60, 0x60, 255));
      ImGui::PushFont(F().ui, F().ui_size);
      dl->AddText(ImVec2(sr.Min.x + S(30), sr.GetCenter().y - ImGui::GetFontSize() * 0.5f), IM_COL32(0x70, 0x70, 0x70, 255),
                  TR("Search (Alt+Q)"));
      ImGui::PopFont();
      // the box opens the command search (drawn by the app)
      ImGui::SetCursorScreenPos(sr.Min);
      if (ImGui::InvisibleButton("##tb_search", sr.GetSize())) g_state.search_open_request = true;
      g_state.search_box = sr;
      add_interactive(sr);
    }
  }

  // title text (centered; pushed right when it overlaps the QAT)
  {
    ImGui::PushFont(F().ui, F().ui_size);
    const float tw = ImGui::CalcTextSize(g_state.title.c_str()).x;
    float tx = std::max(x + S(12), r.Min.x + S(220));
    if (tx + tw > min_r.Min.x - S(8)) tx = std::max(x + S(12), min_r.Min.x - S(8) - tw);
    dl->PushClipRect(ImVec2(x, r.Min.y), ImVec2(min_r.Min.x, r.Max.y), true);
    dl->AddText(ImVec2(IM_ROUND(tx), IM_ROUND(r.GetCenter().y - ImGui::GetFontSize() * 0.5f)), t.title_text,
                g_state.title.c_str());
    dl->PopClipRect();
    ImGui::PopFont();
  }

  // minimize (client area, handled by ImGui)
  ImGui::SetCursorScreenPos(min_r.Min);
  if (ImGui::InvisibleButton("##tb_min", min_r.GetSize())) ::ShowWindow(hwnd, SW_MINIMIZE);
  if (ImGui::IsItemActive()) dl->AddRectFilled(min_r.Min, min_r.Max, t.title_btn_pressed);
  else if (ImGui::IsItemHovered()) dl->AddRectFilled(min_r.Min, min_r.Max, t.title_btn_hover);
  DrawGlyph(dl, icon::Minimize, min_r.GetCenter(), 14.0f, t.title_text);
  add_interactive(min_r);

  // maximize / restore (HTMAXBUTTON, state updated by NC messages)
  if (g_state.max_pressed) dl->AddRectFilled(max_r.Min, max_r.Max, t.title_btn_pressed);
  else if (g_state.max_hover) dl->AddRectFilled(max_r.Min, max_r.Max, t.title_btn_hover);
  DrawGlyph(dl, ::IsZoomed(hwnd) ? icon::Restore : icon::Maximize, max_r.GetCenter(), 14.0f, t.title_text);

  // close
  ImGui::SetCursorScreenPos(close_r.Min);
  const bool close_clicked = ImGui::InvisibleButton("##tb_close", close_r.GetSize());
  const bool close_hover = ImGui::IsItemHovered(), close_held = ImGui::IsItemActive();
  if (close_held) dl->AddRectFilled(close_r.Min, close_r.Max, t.close_pressed);
  else if (close_hover) dl->AddRectFilled(close_r.Min, close_r.Max, t.close_hover);
  DrawGlyph(dl, icon::Close, close_r.GetCenter(), 14.0f, (close_hover || close_held) ? IM_COL32_WHITE : t.title_text);
  if (close_clicked) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
  add_interactive(close_r);
}

}  // namespace yc::ui::titlebar
