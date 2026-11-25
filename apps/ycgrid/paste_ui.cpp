// SPDX-License-Identifier: Apache-2.0
// paste_ui.cpp - Excel's ways to paste as pictures: Home > Paste's drop-down, the Paste Options button and the cell
// menu's Paste Options / Paste Special share them (Paste, Formulas, Formulas & Number Formatting, Keep Source
// Formatting, No Borders, Keep Source Column Widths, Transpose; Values...; Formatting, Paste Link, Picture, Linked Picture)
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

// the ways, in Excel's order and groups; id is the Paste Options number (AppState::LastPaste::option)
struct Way {
  int id;
  const char* cmd;
  const char* label;
  int group;  // 0 Paste, 1 Paste Values, 2 Other Paste Options
};
const Way* Ways(size_t* n) {
  static const Way k[] = {
      {0, "Paste", TR("Paste"), 0},
      {1, "PasteFormulas", TR("Formulas"), 0},
      {13, "PasteFormulasNumberFormats", TR("Formulas & Number Formatting"), 0},
      {9, "PasteKeepSource", TR("Keep Source Formatting"), 0},
      {3, "PasteNoBorders", TR("No Borders"), 0},
      {2, "PasteKeepWidths", TR("Keep Source Column Widths"), 0},
      {4, "PasteTranspose", TR("Transpose"), 0},
      {5, "PasteValues", TR("Values"), 1},
      {12, "PasteValuesNumberFormats", TR("Values & Number Formatting"), 1},
      {6, "PasteValuesSourceFormats", TR("Values & Source Formatting"), 1},
      {7, "PasteFormats", TR("Formatting"), 2},
      {8, "PasteLink", TR("Paste Link"), 2},
      {10, "PasteAsPicture", TR("Picture"), 2},
      {11, "PasteLinkedPicture", TR("Linked Picture"), 2},
  };
  *n = std::size(k);
  return k;
}

const Way* WayOf(int id) {
  size_t n = 0;
  const Way* w = Ways(&n);
  for (size_t i = 0; i < n; ++i) {
    if (w[i].id == id) return &w[i];
  }
  return nullptr;
}

const Way* WayOfCmd(const std::string& cmd) {
  size_t n = 0;
  const Way* w = Ways(&n);
  for (size_t i = 0; i < n; ++i) {
    if (cmd == w[i].cmd) return &w[i];
  }
  return nullptr;
}

// a small mark in the corner of the clipboard: text ("fx", "123") or an icon
void Badge(ImDrawList* dl, ImVec2 min, float size, const char* text, const char* glyph, ImU32 col) {
  const float b = size * 0.56f;
  const ImVec2 p0(min.x + size - b, min.y + size - b), p1(min.x + size, min.y + size);
  dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255), S(1));
  dl->AddRect(p0, p1, (col & 0x00FFFFFF) | 0x90000000, S(1));
  if (glyph) {
    ImFont* f = ui::F().icons;
    const float px = b * 0.9f;
    const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0, glyph);
    dl->AddText(f, px, ImVec2(p0.x + (b - ts.x) * 0.5f, p0.y + (b - ts.y) * 0.5f), col, glyph);
  }
  if (text) {
    ImFont* f = ImGui::GetFont();
    float px = b * 0.62f;
    ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0, text);
    if (ts.x > b - S(1)) {
      px *= (b - S(1)) / ts.x;
      ts = f->CalcTextSizeA(px, FLT_MAX, 0, text);
    }
    dl->AddText(f, px, ImVec2(p0.x + (b - ts.x) * 0.5f, p0.y + (b - ts.y) * 0.5f), IM_COL32(0x21, 0x73, 0x46, 255), text);
  }
}

}  // namespace

void DrawPasteGlyph(ImDrawList* dl, int id, ImVec2 min, float size, bool enabled) {
  const ImU32 col = enabled ? IM_COL32(0x40, 0x40, 0x40, 255) : IM_COL32(0xA0, 0xA0, 0xA0, 255);
  ImFont* f = ui::F().icons;
  const bool picture = id == 10 || id == 11;
  const char* base = picture ? ui::icon::ImageCtl : ui::icon::PasteSmall;
  const float px = size * 0.86f;
  dl->AddText(f, px, ImVec2(min.x, min.y), col, base);
  switch (id) {
    case 1: Badge(dl, min, size, "fx", nullptr, col); break;
    case 13: Badge(dl, min, size, "%fx", nullptr, col); break;
    case 9: Badge(dl, min, size, nullptr, ui::icon::FormatPainter, col); break;
    case 3: Badge(dl, min, size, nullptr, ui::icon::BorderNone, col); break;
    case 2: Badge(dl, min, size, "\xe2\x86\x94", nullptr, col); break;  // <->
    case 4: Badge(dl, min, size, nullptr, ui::icon::Rows, col); break;
    case 5: Badge(dl, min, size, "123", nullptr, col); break;
    case 12: Badge(dl, min, size, "%123", nullptr, col); break;
    case 6: Badge(dl, min, size, nullptr, ui::icon::FillColor, col); break;
    case 7: Badge(dl, min, size, "%", nullptr, col); break;
    case 8: Badge(dl, min, size, nullptr, ui::icon::Link, col); break;
    case 11: Badge(dl, min, size, nullptr, ui::icon::Link, col); break;
    default: break;
  }
}

// the copied cells pasted the way `id` says, in one undo step (the Paste Options button redoes a paste this way)
void ApplyPasteWay(Document& d, uint32_t sheet, const Rect& dest, const engine::ClipData& clip, int id) {
  using M = engine::PasteMode;
  if (id == 6) {  // Values & Source Formatting: the values with every format of the copied cells
    d.Paste(sheet, dest.r0, dest.c0, clip, M::Formats);
    d.Paste(sheet, dest.r0, dest.c0, clip, M::Values);
    return;
  }
  if (id == 8) {  // Paste Link: formulas that point at the copied cells (one cell absolutely, as Excel)
    const model::Sheet* src = d.S(clip.src_sheet);
    const std::string prefix = clip.src_sheet == sheet || !src ? std::string() : core::QuoteSheetName(src->name) + "!";
    const bool one = clip.rows == 1 && clip.cols == 1;
    for (uint32_t i = 0; i < clip.rows; ++i) {
      for (uint32_t j = 0; j < clip.cols; ++j) d.SetFormula(sheet, dest.r0 + i, dest.c0 + j, prefix + core::CellA1(clip.src.r0 + i, clip.src.c0 + j, one, one));
    }
    return;
  }
  const M mode = id == 1 ? M::Formulas : id == 3 ? M::NoBorders : id == 5 ? M::Values : id == 12 ? M::ValuesAndFormats : id == 13 ? M::FormulasAndFormats
                 : id == 7 ? M::Formats : M::All;
  d.Paste(sheet, dest.r0, dest.c0, clip, mode, id == 4, false);
  if (id == 2) {  // the copied columns' widths too
    for (uint32_t c = 0; c < clip.cols && c < clip.col_widths.size(); ++c) d.SetColWidth(sheet, dest.c0 + c, dest.c0 + c, clip.col_widths[c]);
  }
}

// Home > Paste's entries and the cell menu's icons: paste now the way `id` says
void PasteWayNow(AppState& s, int id) {
  if (!s.engine || !s.snap) return;
  if (PasteAfterCopy(s, [id](AppState& st) { PasteWayNow(st, id); })) return;
  const bool copied = CopyModeOn(s) && !s.clip->cut;
  const auto areas = SelAreas(s);
  if (areas.empty()) return;
  const Rect dst = areas.back();
  const uint32_t sheet = s.focus_sheet;
  const std::shared_ptr<const engine::ClipData> clip = s.clip;
  const uint32_t rows = id == 4 ? clip->cols : clip->rows, cols = id == 4 ? clip->rows : clip->cols;
  const Rect dest = copied && rows && cols ? Rect{dst.r0, dst.c0, dst.r0 + rows - 1, dst.c0 + cols - 1} : dst;
  switch (id) {
    case 8: PasteLink(s); break;
    case 10: ui::Commands().Execute("PasteAsPicture"); break;
    case 11:
      if (copied) PasteCellsPicture(s, sheet, dest, clip->src_sheet, clip->src, true);
      break;
    case 2:
    case 6:
      if (!copied) {
        PasteClipboard(s, id == 6 ? engine::PasteMode::Values : engine::PasteMode::All);
        break;
      }
      s.engine->PostUser(TR("Paste"), [sheet, dest, clip, id](Document& d) { ApplyPasteWay(d, sheet, dest, *clip, id); });
      render::SelectRange(s.FocusView(), dest);
      break;
    default: {
      using M = engine::PasteMode;
      const M mode = id == 1 ? M::Formulas : id == 3 ? M::NoBorders : id == 5 ? M::Values : id == 12 ? M::ValuesAndFormats : id == 13 ? M::FormulasAndFormats
                     : id == 7 ? M::Formats : M::All;
      PasteClipboard(s, mode, id == 4);
      break;
    }
  }
  // the Paste Options button follows, with this way chosen (Excel), when one block of the copied cells was pasted
  if (copied && id != 10 && id != 11 && rows && cols && dst.Rows() <= rows && dst.Cols() <= cols) {
    s.last_paste = AppState::LastPaste{};
    s.last_paste.active = true;
    s.last_paste.sheet = sheet;
    s.last_paste.dest = Rect{dst.r0, dst.c0, dst.r0 + clip->rows - 1, dst.c0 + clip->cols - 1};  // as copied (Transpose turns it)
    s.last_paste.clip = clip;
    s.last_paste.option = id;
  }
}

// a row of paste pictures (the cell menu's Paste Options: Paste, Values, Formulas, Transpose, Formatting, Paste Link);
// each named by its tooltip and for the UI item recorder. Returns the way clicked, or -1.
int PasteIconRow(const int* ids, size_t n, int current) {
  int chosen = -1;
  const float tile = S(30);
  for (size_t i = 0; i < n; ++i) {
    const Way* w = WayOf(ids[i]);
    if (!w) continue;
    if (i) ImGui::SameLine(0, S(2));
    ImGui::PushID(w->id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##way", ImVec2(tile, tile));
    const bool hovered = ImGui::IsItemHovered();
    const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ui::NoteUiItem(w->label, r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || w->id == current) dl->AddRectFilled(r.Min, r.Max, hovered ? IM_COL32(0xD5, 0xE8, 0xD4, 255) : IM_COL32(0xE6, 0xE6, 0xE6, 255), S(2));
    if (w->id == current) dl->AddRect(r.Min, r.Max, IM_COL32(0x21, 0x73, 0x46, 255), S(2));
    DrawPasteGlyph(dl, w->id, ImVec2(p.x + S(4), p.y + S(4)), tile - S(8), true);
    if (hovered) ImGui::SetTooltip("%s", w->label);
    if (clicked) chosen = w->id;
    ImGui::PopID();
  }
  return chosen;
}

// Excel's three rows: Paste, Paste Values, Other Paste Options (the Paste Options button, the cell menu's Paste Special)
int PasteGallery(int current) {
  size_t n = 0;
  const Way* w = Ways(&n);
  int chosen = -1;
  static const char* kHeads[] = {TR("Paste"), TR("Paste Values"), TR("Other Paste Options")};
  for (int g = 0; g < 3; ++g) {
    ImGui::TextDisabled("%s", kHeads[g]);
    std::vector<int> ids;
    for (size_t i = 0; i < n; ++i) {
      if (w[i].group == g) ids.push_back(w[i].id);
    }
    const int c = PasteIconRow(ids.data(), ids.size(), current);
    if (c >= 0) chosen = c;
  }
  return chosen;
}

void RegisterPasteCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ui::icon::PasteSmall;
    return c;
  };
  // the ways without a command yet (Paste, Formulas, Transpose, Values, Formatting, Picture already have one)
  for (int id : {13, 9, 3, 2, 12, 6, 8, 11}) {
    const Way* w = WayOf(id);
    R.Add(make(w->cmd, w->label)).execute = [&s, id](ui::Command&) { PasteWayNow(s, id); };
  }
  // the drop-down shows each way as Excel's picture
  auto& st = ui::ribbon::St();
  st.menu_picture = [prev = st.menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
    const Way* w = WayOfCmd(cmd);
    if (!w) return prev ? prev(cmd, dl, at, h) : 0.0f;
    if (dl) DrawPasteGlyph(dl, w->id, ImVec2(at.x + S(1), at.y + S(1)), h - S(2), true);
    return h;
  };
}

}  // namespace yc::app
