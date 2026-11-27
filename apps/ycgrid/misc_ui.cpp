// SPDX-License-Identifier: Apache-2.0
// misc_ui.cpp - remaining ribbon commands: notes, hyperlinks, cell styles, Format as Table, Go To Special, custom sort, series,
// Copy as Picture, Clipboard task pane
#include "app_state.h"
#include "settings.h"
#include "core/i18n.h"
#include "core/utf8.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/pivot_engine.h"
#include "engine/rule_eval.h"
#include "engine/custom_lists.h"
#include "formula/datetime.h"
#include "formula/numfmt.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"
#include "model/theme.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <mutex>

#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace yc::app {

using engine::Document;
using model::Style;
using ui::S;

namespace {

uint64_t Key(uint32_t r, uint32_t c) { return (uint64_t{r} << 16) | c; }

struct MiscUi {
  // notes
  uint32_t note_sheet = 0, note_r = 0, note_c = 0;
  char note_text[2048] = {};
  bool note_inplace = false, note_focus = false;  // the note's yellow box beside its cell, being typed in
  bool note_caret_end = false;  // the box, once it has the keyboard, puts its caret after the note's text
  bool note_active = false;                        // the box had the keyboard last frame
  std::string note_before;                         // its text when the editing started
  bool note_new = false;                           // the cell had no note: leaving the box makes one, empty or not
  // the note box being sized (a handle: 1..8 from the top-left clockwise) or moved (9) with the mouse
  int note_drag = 0;
  ImVec2 note_m0;
  model::CellNote::Look note_look0, note_live;
  bool note_live_on = false;
  // Format Comment
  model::CellNote::Look fc_look;
  // hyperlinks
  char link_addr[1024] = {}, link_sub[256] = {}, link_text[512] = {}, link_tip[256] = {};
  int link_kind = 0;  // 0 web page or file, 1 place in this document, 2 e-mail address
  bool link_existing = false;     // the cell has a link already: Edit Hyperlink, Remove Link
  bool link_text_locked = false;  // the cell holds a number or a formula: its text isn't touched (Excel greys the box)
  std::string link_text0;         // the text shown when the dialog opened
  int link_place = 0;             // place in this document: 0.. sheets, then defined names
  char link_cell[64] = {};        // ... and the cell on the sheet
  char link_mail[256] = {}, link_subject[256] = {};
  // Go To Special
  int gs_kind = 0;
  bool gs_num = true, gs_text = true, gs_logical = true, gs_err = true;
  bool gs_all_levels = false;  // precedents / dependents: all levels
  bool gs_same = false;        // conditional formats / data validation: only the active cell's rules (Excel's Same)
  // custom sort
  struct Level {
    int col = 0;
    bool desc = false;     // Cell Values: Z to A; a color: On Bottom
    int sort_on = 0;       // Sort On: 0 Cell Values, 1 Cell Color, 2 Font Color, 3 Conditional Formatting Icon
    uint32_t color = 0;    // the color put on top / bottom (as stored in the cells; 0 = No Cell Color / Automatic)
    int list = -1;         // Order: a custom list (index into engine::AllFillLists)
    int icon_set = 0, icon = -1;  // Sort On icon: the icon put on top / bottom
  };
  bool sort_case = false, sort_ltr = false;  // Sort Options: Case sensitive; Orientation (Sort left to right)
  bool sort_case_edit = false, sort_ltr_edit = false;  // the Sort Options dialog's own choices until OK
  std::vector<Level> levels;
  int sort_sel = 0;        // the level chosen (Delete Level, Copy Level, Move Up / Down)
  bool sort_header = true;
  Rect sort_rect{};
  uint32_t sort_table = 0;  // a table's data body: its header row (above) names the columns
  // the selection before the dialog showed the cells it sorts: Cancel, Escape and the close box put it back (Excel 16,
  // measured); OK keeps the sorted cells selected
  bool sort_restore = false;
  uint32_t sort_restore_sheet = 0;
  render::Selection sort_saved_sel;
  // series
  int fs_dir = 1;   // 0 rows, 1 columns
  int fs_type = 0;  // 0 linear, 1 growth, 2 date, 3 AutoFill
  bool fs_trend = false;
  int fs_unit = 0;  // date: day, weekday, month, year
  char fs_step[64] = "1", fs_stop[64] = "";
  // threaded comments: the thread window (one cell) and the Comments pane (the whole sheet)
  bool thread_open = false, thread_focus = false;
  uint32_t thread_sheet = 0, thread_r = 0, thread_c = 0;
  // where the thread's cell is on screen (its top-right corner and left edge, set by the grid's overlay): the card opens
  // beside it, as in Excel; placed_key is the cell it was last placed for
  ImVec2 thread_anchor;
  float thread_cell_left = 0;
  bool thread_anchor_ok = false;
  uint64_t thread_placed_key = ~0ull;
  // a left press on a link cell: released soon and in place, it follows the link (Excel: click once to follow, click
  // and hold to select)
  bool link_press = false;
  uint64_t link_press_key = 0;
  double link_press_time = 0;
  ImVec2 link_press_pos;
  char thread_text[4096] = {};
  int thread_edit = -2;  // post being edited: -1 the first post, 0.. a reply, -2 none (the box posts a reply)
  bool show_comments = false;
  // Clipboard task pane
  bool show_clipboard = false;
  // tooltip delay
  uint64_t hover_key = ~0ull;
  double hover_since = 0;
};

MiscUi& M() {
  static MiscUi m;
  return m;
}

std::string WindowsUser() {
  wchar_t buf[256];
  DWORD n = 256;
  if (::GetUserNameW(buf, &n) && n > 1) return ToUtf8(std::wstring(buf, n - 1));
  return "YCGrid";
}

// sheet, r, c: the link's cell (Worksheet_FollowHyperlink, after the link is followed, as in Excel)
void FollowLink(AppState& s, const model::Hyperlink& l, uint32_t sheet, uint32_t r, uint32_t c) {
  if (!l.address.empty()) {
    // a hidden instance (tests, agents) opens nothing on the user's desktop; the status bar still says what it would open
    OpenOnDesktop(FromUtf8(l.address), L"", s.hwnd);
    s.SetStatus(TR("Open: ") + l.address);
  }
  if (!l.sub_address.empty()) Goto(s, l.sub_address);
  FireFollowHyperlink(s, sheet, r, c);
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
  if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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

}  // namespace

void OpenCustomSort(AppState& s, const DataRange& t) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  MiscUi& m = M();
  m.sort_rect = t.rect;
  if (sh->has_used && m.sort_rect.r1 >= sh->row_limit - 1) m.sort_rect.r1 = std::max(m.sort_rect.r0, sh->used.r1);
  m.sort_header = t.header;
  m.sort_table = t.table;
  // the list's recorded sort (the AutoFilter's), or one level on the active cell's column, as Excel opens it
  m.levels.clear();
  const model::AutoFilter& af = *sh->autofilter;
  if (af.on && af.range.c0 == m.sort_rect.c0 && af.range.c1 == m.sort_rect.c1 && af.range.r0 + (t.header ? 0 : 1) == m.sort_rect.r0) {
    for (const auto& l : af.sort_levels) {
      if (l.col < m.sort_rect.Cols()) m.levels.push_back(MiscUi::Level{static_cast<int>(l.col), l.desc});
    }
  }
  if (m.levels.empty()) {
    const uint32_t ac = s.FocusView().sel.act_c;
    MiscUi::Level l;
    l.col = ac >= m.sort_rect.c0 && ac <= m.sort_rect.c1 ? static_cast<int>(ac - m.sort_rect.c0) : 0;
    m.levels.push_back(l);
  }
  m.sort_sel = 0;
  m.sort_case = false;
  m.sort_ltr = false;
  m.sort_restore = true;
  m.sort_restore_sheet = s.focus_sheet;
  m.sort_saved_sel = s.FocusView().sel;
  s.OpenDialog(Dialog::CustomSort);
}

// ---- Threaded comments ----

namespace {

std::string PostDate(double t) {
  if (!(t > 0)) return {};
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm lt{};
  localtime_s(&lt, &tt);
  char b[64];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
  return b;
}

// the posts of a thread; with buttons, *edit / *del get the clicked post (-1 = the first post)
void DrawThreadPosts(const model::CellNote& n, bool buttons, int* edit, int* del) {
  auto post = [&](const std::string& author, const std::string& text, double date, int idx) {
    ImGui::PushID(idx);
    ImGui::PushFont(ui::F().ui_bold, 0.0f);
    ImGui::TextUnformatted(author.empty() ? TR("Unknown") : author.c_str());
    ImGui::PopFont();
    const std::string d = PostDate(date);
    if (!d.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", d.c_str());
    }
    ImGui::TextWrapped("%s", text.c_str());
    if (buttons) {
      if (ImGui::SmallButton(TR("Edit")) && edit) *edit = idx;
      if (idx >= 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Delete")) && del) *del = idx;
      }
    }
    ImGui::PopID();
  };
  if (n.resolved) ImGui::TextColored(ImVec4(0.35f, 0.55f, 0.35f, 1), "%s", TR("Resolved"));
  post(n.author, n.text, n.date, -1);
  for (size_t i = 0; i < n.replies.size(); ++i) {
    ImGui::Separator();
    ImGui::Indent(S(12));
    post(n.replies[i].author, n.replies[i].text, n.replies[i].date, static_cast<int>(i));
    ImGui::Unindent(S(12));
  }
}

const model::CellNote* ThreadOf(const AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh) return nullptr;
  auto it = sh->notes->find(Key(r, c));
  return it != sh->notes->end() && it->second.threaded ? &it->second : nullptr;
}

void SelectCell(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  if (s.focus_sheet != sheet) return;
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  auto& view = s.FocusView();
  view.sel.areas = {Rect::Cell(r, c)};
  view.sel.anc_r = view.sel.act_r = r;
  view.sel.anc_c = view.sel.act_c = c;
  view.sel.mode = render::SelectMode::Cells;
  render::EnsureVisible(*sh, view, r, c);
}

void OpenThread(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  MiscUi& m = M();
  m.thread_open = true;
  m.thread_focus = true;
  m.thread_sheet = sheet;
  m.thread_r = r;
  m.thread_c = c;
  m.thread_text[0] = 0;
  m.thread_edit = -2;
  m.thread_anchor_ok = false;
  m.thread_placed_key = ~0ull;
  (void)s;
}

// the next / previous thread on the sheet after the active cell (row by row, like Excel)
// Review > Notes > Previous Note / Next Note: the notes of the sheet in reading order (rows, then columns), wrapping
void StepNote(AppState& s, int dir) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  std::vector<uint64_t> keys;
  for (const auto& [k, n] : *sh->notes) {
    if (!n.threaded) keys.push_back(k);
  }
  if (keys.empty()) {
    s.SetStatus(TR("There are no notes on this sheet."));
    return;
  }
  const auto& v = s.FocusView();
  const uint64_t cur = Key(v.sel.act_r, v.sel.act_c);
  uint64_t next = dir > 0 ? keys.front() : keys.back();
  if (dir > 0) {
    for (uint64_t k : keys) {
      if (k > cur) {
        next = k;
        break;
      }
    }
  } else {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
      if (*it < cur) {
        next = *it;
        break;
      }
    }
  }
  SelectCell(s, sh->id, static_cast<uint32_t>(next >> 16), static_cast<uint32_t>(next & 0xFFFF));
}

void StepThread(AppState& s, int dir) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  std::vector<uint64_t> keys;
  for (const auto& [k, n] : *sh->notes) {
    if (n.threaded) keys.push_back(k);
  }
  if (keys.empty()) {
    s.SetStatus(TR("There are no comments on this sheet."));
    return;
  }
  const auto& v = s.FocusView();
  const uint64_t cur = Key(v.sel.act_r, v.sel.act_c);
  uint64_t next = dir > 0 ? keys.front() : keys.back();
  if (dir > 0) {
    for (uint64_t k : keys) {
      if (k > cur) {
        next = k;
        break;
      }
    }
  } else {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
      if (*it < cur) {
        next = *it;
        break;
      }
    }
  }
  const uint32_t r = static_cast<uint32_t>(next >> 16), c = static_cast<uint32_t>(next & 0xFFFF);
  SelectCell(s, sh->id, r, c);
  OpenThread(s, sh->id, r, c);
}

void DrawThreadWindow(AppState& s) {
  MiscUi& m = M();
  if (!m.thread_open) return;
  const model::CellNote* n = ThreadOf(s, m.thread_sheet, m.thread_r, m.thread_c);
  const model::Sheet* sh = s.snap->Sheet(m.thread_sheet);
  if (!sh) {
    m.thread_open = false;
    return;
  }
  // a cell with a note: the note window handles it
  auto ni = sh->notes->find(Key(m.thread_r, m.thread_c));
  const bool has_note = ni != sh->notes->end() && !ni->second.threaded;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  // like Excel: the card opens beside its cell (right of its top-right corner; left of the cell when that would leave
  // the window), placed again when the thread's cell changes
  const uint64_t key = (uint64_t{m.thread_sheet} << 40) | (uint64_t{m.thread_r} << 16) | m.thread_c;
  if (m.thread_placed_key != key) {
    if (!m.thread_anchor_ok) return;  // the grid gives the cell's place this frame
    ImVec2 p(m.thread_anchor.x + S(14), m.thread_anchor.y);
    if (p.x + S(340) > vp->WorkPos.x + vp->WorkSize.x) p.x = std::max(vp->WorkPos.x, m.thread_cell_left - S(14) - S(340));
    if (p.y + S(220) > vp->WorkPos.y + vp->WorkSize.y) p.y = std::max(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - S(220));
    ImGui::SetNextWindowPos(p);
    m.thread_placed_key = key;
  }
  ImGui::SetNextWindowSize(ImVec2(S(340), 0), ImGuiCond_Appearing);
  const std::string title = std::string(TR("Comment")) + " - " + sh->name + "!" + core::CellA1(m.thread_r, m.thread_c) + "###yc_comment_thread";
  if (m.thread_focus) ImGui::SetNextWindowFocus();
  KeepOpenedWidth(title.c_str());
  if (ImGui::Begin(title.c_str(), &m.thread_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    NoteOpenedWidth();
    const uint32_t sheet = m.thread_sheet, r = m.thread_r, c = m.thread_c;
    int edit = -3, del = -3;
    if (has_note) {
      ImGui::TextWrapped("%s", TR("This cell has a note. Convert it to a comment to reply to it."));
      if (ImGui::Button(TR("Convert to Comment"))) {
        const double now = static_cast<double>(std::time(nullptr));
        s.engine->PostUser(TR("Convert to Comments"), [sheet, r, c, now](Document& d) { d.ConvertNotesToComments(sheet, Rect::Cell(r, c), now); });
      }
    } else {
      if (n) {
        ImGui::BeginChild("##posts", ImVec2(S(320), std::min(S(320), ImGui::GetTextLineHeightWithSpacing() * (4.0f + 3.0f * static_cast<float>(n->replies.size())))),
                          ImGuiChildFlags_Borders);
        DrawThreadPosts(*n, true, &edit, &del);
        ImGui::EndChild();
      }
      if (edit >= -1 && n) {
        m.thread_edit = edit;
        const std::string& t = edit < 0 ? n->text : n->replies[static_cast<size_t>(edit)].text;
        std::snprintf(m.thread_text, sizeof m.thread_text, "%s", t.c_str());
        m.thread_focus = true;
      }
      if (del >= 0) s.engine->PostUser(TR("Delete Reply"), [sheet, r, c, del](Document& d) { d.DeleteCommentReply(sheet, r, c, del); });
      const char* hint = !n ? TR("Start a conversation") : (m.thread_edit >= -1 ? TR("Edit the post") : TR("Reply"));
      if (m.thread_focus) {
        ImGui::SetKeyboardFocusHere();
        m.thread_focus = false;
      }
      ImGui::InputTextMultiline("##thread_text", m.thread_text, sizeof m.thread_text, ImVec2(S(320), S(60)));
      if (!m.thread_text[0]) {
        const ImVec2 mn = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + S(6), mn.y + S(4)), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
      }
      const bool post = ImGui::Button(TR("Post"));
      const bool ctrl_enter = ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl;
      if ((post || ctrl_enter) && m.thread_text[0]) {
        const std::string text = m.thread_text, author = WindowsUser();
        const double now = static_cast<double>(std::time(nullptr));
        const int ed = m.thread_edit;
        if (!n) s.engine->PostUser(TR("New Comment"), [sheet, r, c, author, text, now](Document& d) { d.SetComment(sheet, r, c, author, text, now); });
        else if (ed >= -1) s.engine->PostUser(TR("Edit Comment"), [sheet, r, c, ed, text](Document& d) { d.EditCommentPost(sheet, r, c, ed, text); });
        else s.engine->PostUser(TR("Reply"), [sheet, r, c, author, text, now](Document& d) { d.AddCommentReply(sheet, r, c, author, text, now); });
        m.thread_text[0] = 0;
        m.thread_edit = -2;
      }
      if (n) {
        ImGui::SameLine();
        const bool resolved = n->resolved;
        if (ImGui::Button(resolved ? TR("Reopen") : TR("Resolve Thread"))) {
          s.engine->PostUser(resolved ? TR("Reopen") : TR("Resolve Thread"), [sheet, r, c, resolved](Document& d) { d.ResolveComment(sheet, r, c, !resolved); });
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Delete Thread"))) {
          s.engine->PostUser(TR("Delete Comment"), [sheet, r, c](Document& d) { d.SetComment(sheet, r, c, "", "", 0); });
          m.thread_open = false;
        }
      }
      ImGui::TextDisabled("%s", TR("Ctrl+Enter posts"));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) m.thread_open = false;
  }
  ImGui::End();
}

// all comment threads of the active sheet
void DrawCommentsPane(AppState& s) {
  MiscUi& m = M();
  if (!m.show_comments) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - S(340), vp->WorkPos.y + S(240)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(S(320), S(460)), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(TR("Comments###yc_comments_pane"), &m.show_comments)) {
    const model::Sheet* sh = s.FocusSheet();
    if (ImGui::Button(TR("New"))) ui::Commands().Execute("NewComment");
    ImGui::Separator();
    bool any = false;
    if (sh) {
      for (const auto& [k, n] : *sh->notes) {
        if (!n.threaded) continue;
        any = true;
        const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
        ImGui::PushID(static_cast<int>(k & 0x7FFFFFFF));
        const std::string head = core::CellA1(r, c);
        if (ImGui::Selectable(head.c_str(), m.thread_open && m.thread_sheet == sh->id && m.thread_r == r && m.thread_c == c)) {
          SelectCell(s, sh->id, r, c);
          OpenThread(s, sh->id, r, c);
        }
        ImGui::Indent(S(8));
        DrawThreadPosts(n, false, nullptr, nullptr);
        ImGui::Unindent(S(8));
        ImGui::Separator();
        ImGui::PopID();
      }
    }
    if (!any) ImGui::TextWrapped("%s", TR("No comments on this sheet. Select a cell and choose New Comment (Ctrl+Alt+M) to start a conversation."));
  }
  ImGui::End();
}

}  // namespace

// ---- Overlay: note and hyperlink tooltips, Ctrl+click follows a link ----

// the sheet leaves the keyboard to the note only while its box has it (a box that never got the focus takes nothing)
bool NoteEditing() { return M().note_inplace && (M().note_active || M().note_focus); }

constexpr double kNotePx = 4.0 / 3.0;  // points -> sheet pixels

// a note box on screen: Excel's place beside its cell (moved by dx / dy) and its size (the default 200 x 120)
ImRect NoteRect(const model::Sheet& sh, uint32_t r, uint32_t c, const model::CellNote::Look& lk, const render::OverlayGeom& g, ImVec2* corner_out = nullptr) {
  const ImVec2 corner = g.to_screen(static_cast<double>(sh.cols->Offset(static_cast<int64_t>(c) + 1)), static_cast<double>(sh.rows->Offset(r)));
  if (corner_out) *corner_out = corner;
  const float k = static_cast<float>(kNotePx * g.sc);
  const ImVec2 p(corner.x + S(14) + static_cast<float>(lk.dx) * k, corner.y - S(8) + static_cast<float>(lk.dy) * k);
  const ImVec2 sz(lk.w > 0 ? static_cast<float>(lk.w) * k : S(200), lk.h > 0 ? static_cast<float>(lk.h) * k : S(120));
  return ImRect(p, p + sz);
}

ImU32 NoteCol(uint32_t argb, ImU32 def) { return argb ? IM_COL32((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, 255) : def; }

// the box's frame: Excel's shadow, fill and line, and the line to its cell
void DrawNoteFrame(ImDrawList* dl, const ImRect& box, ImVec2 corner, const model::CellNote::Look& lk) {
  dl->AddLine(corner, ImVec2(box.Min.x, std::clamp(corner.y, box.Min.y, box.Max.y) + S(8) * (corner.y < box.Min.y ? 0 : 1)), IM_COL32(0x40, 0x40, 0x40, 255));
  dl->AddRectFilled(box.Min + ImVec2(S(3), S(3)), box.Max + ImVec2(S(3), S(3)), IM_COL32(0, 0, 0, 40));
  dl->AddRectFilled(box.Min, box.Max, NoteCol(lk.fill_color, IM_COL32(0xFF, 0xFF, 0xE1, 255)));
  dl->AddRect(box.Min, box.Max, NoteCol(lk.line_color, IM_COL32(0x60, 0x60, 0x60, 255)));
}

// the note text in its font, color and alignment
void DrawNoteText(ImDrawList* dl, const ImRect& area, const std::string& text, const model::CellNote::Look& lk, float zoom) {
  ImFont* f = lk.font_name.empty() && !lk.bold && !lk.italic ? ImGui::GetFont() : ui::CellFont(lk.font_name.empty() ? "Tahoma" : lk.font_name, lk.bold, lk.italic);
  const float px = lk.font_size > 0 ? lk.font_size * static_cast<float>(kNotePx) * zoom : ImGui::GetFontSize();
  const float wrap = std::max(1.0f, area.GetWidth());
  const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, wrap, text.c_str());
  const float y = lk.valign == 1 ? area.GetCenter().y - ts.y * 0.5f : lk.valign == 2 ? area.Max.y - ts.y : area.Min.y;
  // each line placed by the alignment
  const char* p = text.c_str();
  const char* end = p + text.size();
  float ly = y;
  const float lh = f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
  while (p < end) {
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    const char* le = nl ? nl : end;
    const char* e = f->CalcWordWrapPosition(px, p, le, wrap);
    if (e <= p) e = std::min(le, p + 1);
    const float w = f->CalcTextSizeA(px, FLT_MAX, 0.0f, p, e).x;
    const float lx = lk.halign == 1 ? area.GetCenter().x - w * 0.5f : lk.halign == 2 ? area.Max.x - w : area.Min.x;
    dl->AddText(f, px, ImVec2(std::floor(lx), std::floor(ly)), NoteCol(lk.font_color, IM_COL32_BLACK), p, e);
    ly += lh;
    p = e;
    if (p < end && *p == '\n') ++p;
    else
      while (p < le && *p == ' ') ++p;
  }
}

// the note being typed: Excel's yellow box beside the cell, with the author on top and a line to the cell
void DrawNoteEditor(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  MiscUi& m = M();
  if (!m.note_inplace || m.note_sheet != sheet_id) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  // read-only mode came on while the note was typed (the button, the idle timeout, Python): the box closes and what
  // was typed is dropped, as an entry in a cell is (left open it took every key and saved them on the next click)
  if (!sh || !s.operator_mode) {
    m.note_inplace = false;
    m.note_drag = 0;
    m.note_live_on = false;
    return;
  }
  const auto it = sh->notes->find(Key(m.note_r, m.note_c));
  const model::CellNote::Look look = m.note_live_on ? m.note_live : it != sh->notes->end() ? it->second.look : model::CellNote::Look{};
  ImVec2 corner;
  const ImRect box = NoteRect(*sh, m.note_r, m.note_c, look, g, &corner);
  const ImVec2 p = box.Min, sz = box.GetSize();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DrawNoteFrame(dl, box, corner, look);
  // Excel's handles: the eight around the box size it, its border moves it (submitted before the text box, so they get
  // the mouse first); a right-click on the border offers Format Comment
  {
    ImGuiIO& io = ImGui::GetIO();
    const float hs = S(7);
    const ImVec2 hp[8] = {box.Min, ImVec2(box.GetCenter().x, box.Min.y), ImVec2(box.Max.x, box.Min.y), ImVec2(box.Max.x, box.GetCenter().y),
                          box.Max, ImVec2(box.GetCenter().x, box.Max.y), ImVec2(box.Min.x, box.Max.y), ImVec2(box.Min.x, box.GetCenter().y)};
    static const ImGuiMouseCursor kCur[8] = {ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW,
                                             ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW};
    const model::CellNote::Look stored = it != sh->notes->end() ? it->second.look : model::CellNote::Look{};
    auto start = [&](int which) {
      m.note_drag = which;
      m.note_m0 = io.MousePos;
      m.note_look0 = stored;
      const float k = static_cast<float>(kNotePx * g.sc);
      if (m.note_look0.w <= 0) m.note_look0.w = sz.x / k;
      if (m.note_look0.h <= 0) m.note_look0.h = sz.y / k;
    };
    for (int i = 0; i < 8; ++i) {
      ImGui::SetCursorScreenPos(hp[i] - ImVec2(hs * 0.5f, hs * 0.5f));
      ImGui::PushID(i);
      ImGui::InvisibleButton("##note_handle", ImVec2(hs, hs));
      if (ImGui::IsItemHovered() || m.note_drag == i + 1) ImGui::SetMouseCursor(kCur[i]);
      if (ImGui::IsItemActivated()) start(i + 1);
      ui::NoteUiItem((std::string("note handle ") + std::to_string(i + 1)).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
      ImGui::PopID();
      dl->AddRectFilled(hp[i] - ImVec2(S(2.5f), S(2.5f)), hp[i] + ImVec2(S(2.5f), S(2.5f)), IM_COL32(255, 255, 255, 255));
      dl->AddRect(hp[i] - ImVec2(S(2.5f), S(2.5f)), hp[i] + ImVec2(S(2.5f), S(2.5f)), IM_COL32(0x40, 0x40, 0x40, 255));
    }
    // the border: four strips just outside the box
    const float bw = S(4);
    const ImRect strips[4] = {ImRect(box.Min - ImVec2(bw, bw), ImVec2(box.Max.x + bw, box.Min.y)), ImRect(ImVec2(box.Min.x - bw, box.Max.y), box.Max + ImVec2(bw, bw)),
                              ImRect(box.Min - ImVec2(bw, 0), ImVec2(box.Min.x, box.Max.y)), ImRect(ImVec2(box.Max.x, box.Min.y), ImVec2(box.Max.x + bw, box.Max.y))};
    for (int i = 0; i < 4; ++i) {
      ImGui::SetCursorScreenPos(strips[i].Min);
      ImGui::PushID(10 + i);
      ImGui::InvisibleButton("##note_border", ImMax(strips[i].GetSize(), ImVec2(1, 1)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      if (ImGui::IsItemHovered() || m.note_drag == 9) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) start(9);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##note_menu");
      ui::NoteUiItem(i == 0 ? "note border" : "note border 2", ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
      ImGui::PopID();
    }
    for (int i = 10; i < 14; ++i) {
      ImGui::PushID(i);
      if (ImGui::BeginPopup("##note_menu")) {
        if (ImGui::MenuItem(TR("Format Comment..."))) ui::Commands().Execute("FormatComment");
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    if (m.note_drag) {
      const float k = static_cast<float>(kNotePx * g.sc);
      const double dx = (io.MousePos.x - m.note_m0.x) / k, dy = (io.MousePos.y - m.note_m0.y) / k;
      model::CellNote::Look lk = m.note_look0;
      const int h = m.note_drag;
      if (h == 9) {
        lk.dx += dx, lk.dy += dy;
      } else {
        if (h == 1 || h == 7 || h == 8) lk.dx += dx, lk.w -= dx;
        if (h == 3 || h == 4 || h == 5) lk.w += dx;
        if (h == 1 || h == 2 || h == 3) lk.dy += dy, lk.h -= dy;
        if (h == 5 || h == 6 || h == 7) lk.h += dy;
        lk.w = std::max(12.0, lk.w), lk.h = std::max(12.0, lk.h);
      }
      m.note_live = lk;
      m.note_live_on = true;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m.note_drag = 0;
        const uint32_t sheet = m.note_sheet, r = m.note_r, c = m.note_c;
        s.engine->PostUser(h == 9 ? TR("Move Note") : TR("Resize Note"), [sheet, r, c, lk](Document& d) { d.SetNoteLook(sheet, r, c, lk); });
        m.note_focus = true;  // the typing goes on
      }
    } else if (m.note_live_on && it != sh->notes->end() && it->second.look == m.note_live) {
      m.note_live_on = false;
    }
  }
  const std::string author = it != sh->notes->end() && !it->second.author.empty() ? it->second.author : WindowsUser();
  ImGui::PushFont(ui::F().ui_bold, 0.0f);
  dl->AddText(p + ImVec2(S(5), S(3)), IM_COL32_BLACK, (author + ":").c_str());
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(p + ImVec2(S(1), S(3) + ImGui::GetTextLineHeight()));
  if (m.note_focus) {
    ImGui::SetKeyboardFocusHere();
    m.note_focus = false;
  }
  ImGui::PushStyleColor(ImGuiCol_FrameBg, NoteCol(look.fill_color, IM_COL32(0xFF, 0xFF, 0xE1, 255)));
  ImGui::PushStyleColor(ImGuiCol_Text, NoteCol(look.font_color, IM_COL32_BLACK));
  // Escape ends the editing but keeps what was typed (Excel); ImGui's box would put the old text back
  const bool esc = ImGui::GetActiveID() == ImGui::GetID("##note_inplace") && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  const std::string typed = esc ? std::string(m.note_text) : std::string();
  // Excel: Shift+F2 on a note goes on typing after its text; ImGui's box takes the keyboard with its caret at 0, and
  // what was typed went before the note
  auto to_end = [](ImGuiInputTextCallbackData* data) {
    bool* flag = static_cast<bool*>(data->UserData);
    if (*flag) {
      data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
      *flag = false;
    }
    return 0;
  };
  ImGui::InputTextMultiline("##note_inplace", m.note_text, sizeof(m.note_text), ImVec2(sz.x - S(2), sz.y - ImGui::GetTextLineHeight() - S(6)),
                            ImGuiInputTextFlags_CallbackAlways, to_end, &m.note_caret_end);
  if (esc) std::snprintf(m.note_text, sizeof(m.note_text), "%s", typed.c_str());
  ImGui::PopStyleColor(2);
  const bool active = ImGui::IsItemActive();
  m.note_active = active;
  // a click elsewhere or Escape ends the editing and keeps the note (Excel); also when the box lost the keyboard
  const bool on_frame = m.note_drag != 0 || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || s.dialog == Dialog::FormatComment;
  const bool left = !on_frame && (ImGui::IsItemDeactivated() || (!active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered() &&
                                                                  !ImGui::IsAnyItemHovered()) ||
                                  (!active && ImGui::IsKeyPressed(ImGuiKey_Escape, false)));
  if (left) {
    const uint32_t sheet = m.note_sheet, r = m.note_r, c = m.note_c;
    const std::string text = m.note_text;
    // the note stays when its box is left empty, a new one too (Excel 16: Shift+F2, Escape leaves a note, and one
    // emptied by Backspace stays; Delete Note takes it away)
    if (text != m.note_before || m.note_new) {
      s.engine->PostUser(TR("Notes"), [sheet, r, c, text, author](Document& d) { d.SetNote(sheet, r, c, author, text, true); });
    }
    m.note_new = false;
    m.note_inplace = false;
  }
}

// notes shown all the time (Show/Hide Note, Show All Notes): Excel's yellow box beside the cell with a line to it; a
// click on the box edits the note
void DrawShownNotes(AppState& s, const model::Sheet& sh, const render::OverlayGeom& g) {
  MiscUi& m = M();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float lh = ImGui::GetTextLineHeight();
  for (const auto& [k, n] : *sh.notes) {
    if (n.threaded || !n.visible) continue;
    const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
    if (m.note_inplace && m.note_sheet == sh.id && m.note_r == r && m.note_c == c) continue;  // being typed: the editor shows it
    ImVec2 corner;
    ImRect box = NoteRect(sh, r, c, n.look, g, &corner);
    if (n.look.h <= 0) {
      // the default box grows with its text (a sized one keeps its size)
      const ImVec2 ts = ImGui::CalcTextSize(n.text.c_str(), nullptr, false, box.GetWidth() - S(10));
      box.Max.y = box.Min.y + std::max(S(60), ts.y + (n.author.empty() ? 0.0f : lh) + S(10));
    }
    const ImVec2 p = box.Min;
    if (!box.Overlaps(g.cells) && !g.cells.Contains(corner)) continue;
    dl->PushClipRect(g.cells.Min, g.cells.Max, true);
    DrawNoteFrame(dl, box, corner, n.look);
    float ty = p.y + S(3);
    if (!n.author.empty()) {
      ImGui::PushFont(ui::F().ui_bold, 0.0f);
      dl->AddText(ImVec2(p.x + S(5), ty), NoteCol(n.look.font_color, IM_COL32_BLACK), (n.author + ":").c_str());
      ImGui::PopFont();
      ty += lh;
    }
    dl->PushClipRect(box.Min, box.Max, true);
    DrawNoteText(dl, ImRect(ImVec2(p.x + S(5), ty), ImVec2(box.Max.x - S(5), box.Max.y - S(3))), n.text, n.look, static_cast<float>(g.sc));
    dl->PopClipRect();
    dl->PopClipRect();
    const ImRect hit(ImMax(box.Min, g.cells.Min), ImMin(box.Max, g.cells.Max));
    if (hit.GetWidth() < 1 || hit.GetHeight() < 1) continue;
    ImGui::SetCursorScreenPos(hit.Min);
    ImGui::PushID(static_cast<int>(k & 0x7FFFFFFF));
    if (ImGui::InvisibleButton("##shown_note", hit.GetSize())) {
      SelectCell(s, sh.id, r, c);
      ui::Commands().Execute("InsertComment");
    }
    ImGui::PopID();
  }
}

namespace {
// Format Stale Values: the waiting cells in view, as the engine last answered (it is asked at most every 0.2 s, and
// the frame never waits for it)
struct StaleView {
  std::mutex mu;
  uint32_t sheet = 0;
  std::vector<uint64_t> keys;
  bool waiting = false;
  double asked_at = -1;
};
StaleView& Stale() {
  static StaleView v;
  return v;
}
}  // namespace

// File > Options > Formulas > Format stale values (Application.FormatStaleValues): in manual calculation the cells
// waiting for F9 are struck through, so nobody reads an old value as a new one
void DrawStaleOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  if (!s.snap || !s.snap->manual_calc || !s.snap->calculating || !Config().format_stale_values || !s.engine) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  const render::GridView& v = s.ViewFor(sheet_id);
  const uint32_t r0 = static_cast<uint32_t>(v.freeze_rows ? std::min<int64_t>(v.frozen_top, v.v.first) : v.v.first);
  const uint32_t c0 = static_cast<uint32_t>(v.freeze_cols ? std::min<int64_t>(v.frozen_left, v.h.first) : v.h.first);
  const Rect view{r0, c0, static_cast<uint32_t>(std::max<int64_t>(r0, v.last_bottom_row)), static_cast<uint32_t>(std::max<int64_t>(c0, v.last_right_col))};
  StaleView& st = Stale();
  const double now = ImGui::GetTime();
  {
    std::lock_guard<std::mutex> lk(st.mu);
    if (!st.waiting && now - st.asked_at > 0.2) {
      st.waiting = true;
      st.asked_at = now;
      s.engine->Post([sheet_id, view](Document& d) {
        std::vector<uint64_t> keys;
        d.PendingIn(sheet_id, view, 20000, &keys);
        StaleView& sv = Stale();
        std::lock_guard<std::mutex> lk2(sv.mu);
        sv.keys = std::move(keys);
        sv.sheet = sheet_id;
        sv.waiting = false;
      });
    }
    ui::RequestRedrawIn(0.25);  // the answer is drawn when it comes
    if (st.sheet != sheet_id) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(g.cells.Min, g.cells.Max, true);
    const ImU32 col = IM_COL32(0x40, 0x40, 0x40, 255);
    for (const uint64_t k : st.keys) {
      const int64_t r = static_cast<int64_t>(engine::KeyRow(k)), c = static_cast<int64_t>(engine::KeyCol(k));
      const ImVec2 a = g.to_screen(static_cast<double>(sh->cols->Offset(c)), static_cast<double>(sh->rows->Offset(r)));
      const ImVec2 b = g.to_screen(static_cast<double>(sh->cols->Offset(c + 1)), static_cast<double>(sh->rows->Offset(r + 1)));
      const float y = std::floor((a.y + b.y) * 0.5f) + 0.5f;
      dl->AddLine(ImVec2(a.x + S(2), y), ImVec2(b.x - S(2), y), col, 1.0f);
    }
    dl->PopClipRect();
  }
}

std::string TableRefFor(AppState& s, uint32_t sheet, const Rect& rr) {
  if (!Config().generate_table_refs || !s.snap || !s.edit.active) return {};
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh || sh->tables->empty() || s.edit.sheet != sheet) return {};  // (another sheet's: not measured, cells as before)
  const model::Table* t = nullptr;
  for (const model::Table& x : *sh->tables) {
    if (x.range.Contains(rr.r0, rr.c0) && x.range.Contains(rr.r1, rr.c1)) t = &x;
  }
  if (!t) return {};
  const Rect d = t->Data();
  const uint32_t hr = t->range.r0, tr = t->range.r1;
  // the rows: the table's header, data and totals rows, or the formula's own row of its data ([@...])
  bool at = false;
  std::vector<const char*> parts;
  if (rr.r0 == t->range.r0 && rr.r1 == t->range.r1 && (t->header || t->totals)) {
    parts.push_back("#All");
  } else if (d.r0 <= d.r1 && rr.r0 == d.r0 && rr.r1 == d.r1) {
    // the data rows: no item
  } else if (t->header && rr.r0 == hr && rr.r1 == hr) {
    parts.push_back("#Headers");
  } else if (t->totals && rr.r0 == tr && rr.r1 == tr) {
    parts.push_back("#Totals");
  } else if (t->header && t->totals && d.r0 <= d.r1 && rr.r0 == hr && rr.r1 == d.r1) {
    parts = {"#Headers", "#Data"};
  } else if (t->header && t->totals && d.r0 <= d.r1 && rr.r0 == d.r0 && rr.r1 == tr) {
    parts = {"#Data", "#Totals"};
  } else if (rr.r0 == rr.r1 && rr.r0 >= d.r0 && rr.r0 <= d.r1 && s.edit.r == rr.r0) {
    at = true;  // the formula's own row
  } else {
    return {};  // part of a column, another row: the cells (Excel 16, measured)
  }
  auto name_of = [&](uint32_t c) {
    const uint32_t j = c - t->range.c0;
    if (j < t->columns.size() && !t->columns[j].name.empty()) return t->columns[j].name;
    return std::string();
  };
  auto esc = [](const std::string& n) {
    std::string o;
    for (char ch : n) {
      if (ch == '[' || ch == ']' || ch == '#' || ch == '\'') o += '\'';
      o += ch;
    }
    return "[" + o + "]";
  };
  const bool all_cols = rr.c0 == t->range.c0 && rr.c1 == t->range.c1;
  std::string cols;
  if (!all_cols) {
    const std::string a = name_of(rr.c0), b = name_of(rr.c1);
    if (a.empty() || b.empty()) return {};
    cols = rr.c0 == rr.c1 ? esc(a) : esc(a) + ":" + esc(b);
  }
  // Excel's forms: Table1[Px], Table1[[Qty]:[Px]], Table1, Table1[[#All],[Px]], Table1[#All], Table1[[#Headers],[Qty]],
  // Table1[@Px], Table1[@[Qty]:[Px]], Table1[@] (measured by keys posted to its sheet window; inside the table the
  // formula reads back as [@Px], [Px])
  std::string body;
  if (at) {
    body = cols.empty() ? "@" : "@" + cols;
  } else if (parts.empty()) {
    if (cols.empty()) return t->name;
    body = cols;
  } else if (cols.empty() && parts.size() == 1) {
    body = parts[0];
  } else {
    for (size_t i = 0; i < parts.size(); ++i) body += std::string(i ? "," : "") + "[" + parts[i] + "]";
    if (!cols.empty()) body += "," + cols;
  }
  return t->name + "[" + body + "]";
}

std::string GetPivotDataFor(AppState& s, uint32_t sheet, uint32_t r, uint32_t c) {
  if (!Config().generate_getpivotdata || !s.snap) return {};
  const model::Sheet* sh = s.snap->Sheet(sheet);
  if (!sh) return {};
  for (const model::PivotTable& pt : *sh->pivots) {
    if (!pt.has_output || !pt.output.Contains(r, c) || pt.data.empty()) continue;
    // the report as the snapshot holds it, worked out once while the pointer moves over it (a drag asks every frame,
    // and a report of a large source takes a while to make)
    struct Kept {
      uint64_t version = ~0ull;
      uint32_t sheet = 0, id = 0;
      engine::PivotCache cache;
      engine::PivotReport rep;
      bool ok = false;
    };
    static Kept kept;
    if (kept.version != s.snap->version || kept.sheet != sheet || kept.id != pt.id) {
      kept = Kept{};
      kept.version = s.snap->version, kept.sheet = sheet, kept.id = pt.id;
      std::string why;
      kept.ok = engine::ReadPivotSource(s.snap->wb, pt, &kept.cache, &why);
      if (kept.ok) kept.rep = engine::ComputePivot(pt, kept.cache);
    }
    if (!kept.ok) return {};
    const engine::PivotCache& cache = kept.cache;
    const engine::PivotReport& rep = kept.rep;
    const uint32_t rr = r - pt.output.r0, cc = c - pt.output.c0;
    if (rr >= rep.rows || cc >= rep.cols) return {};
    const engine::PivotCell& cell = rep.at(rr, cc);
    using K = engine::PivotCell;
    if (cell.kind != K::Data && cell.kind != K::Subtotal && cell.kind != K::Grand) return {};
    int df = cc < rep.col_data.size() ? rep.col_data[cc] : -1;
    if (df < 0 && rr < rep.row_data.size()) df = rep.row_data[rr];
    if (df < 0) df = 0;
    if (static_cast<size_t>(df) >= pt.data.size()) return {};
    auto quote = [](const std::string& t) {
      std::string q = "\"";
      for (char ch : t) q += ch == '"' ? std::string("\"\"") : std::string(1, ch);
      return q + "\"";
    };
    std::string f = "GETPIVOTDATA(" + quote(engine::PivotDataCaption(pt, cache, static_cast<size_t>(df))) + "," +
                    core::CellA1(pt.output.r0 + rep.page_rows, pt.output.c0, true, true);  // the report's corner, under its filters
    auto pairs = [&](const std::vector<std::pair<int, int>>& v) {
      for (const auto& [fi, it] : v) {
        if (fi < 0 || static_cast<size_t>(fi) >= cache.names.size() || it < 0 || static_cast<size_t>(it) >= cache.display[static_cast<size_t>(fi)].size()) continue;
        f += "," + quote(cache.names[static_cast<size_t>(fi)]) + "," + quote(cache.display[static_cast<size_t>(fi)][static_cast<size_t>(it)]);
      }
    };
    if (rr < rep.row_filter.size()) pairs(rep.row_filter[rr]);
    if (cc < rep.col_filter.size()) pairs(rep.col_filter[cc]);
    return f + ")";
  }
  return {};
}

void DrawCellTips(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  DrawNoteEditor(s, sheet_id, g);
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (sh && !sh->notes->empty()) DrawShownNotes(s, *sh, g);
  MiscUi& m = M();
  if (sh && m.thread_open && m.thread_sheet == sheet_id) {
    // the comment card's cell on screen (DrawThreadWindow opens the card beside it)
    const double y = static_cast<double>(sh->rows->Offset(m.thread_r));
    m.thread_anchor = g.to_screen(static_cast<double>(sh->cols->Offset(static_cast<int64_t>(m.thread_c) + 1)), y);
    m.thread_cell_left = g.to_screen(static_cast<double>(sh->cols->Offset(m.thread_c)), y).x;
    m.thread_anchor_ok = true;
  }
  if (!sh || (sh->notes->empty() && sh->links->empty())) return;
  ImGuiIO& io = ImGui::GetIO();
  const bool on_sheet = g.cells.Contains(io.MousePos) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  const ImVec2 sp = g.to_sheet(io.MousePos);
  const uint32_t r = static_cast<uint32_t>(sh->rows->IndexAt(static_cast<int64_t>(sp.y)));
  const uint32_t c = static_cast<uint32_t>(sh->cols->IndexAt(static_cast<int64_t>(sp.x)));
  uint32_t ar = r, ac = c;
  if (const Rect* mg = sh->MergeAt(r, c)) ar = mg->r0, ac = mg->c0;
  const uint64_t key = Key(ar, ac) | (uint64_t{sheet_id} << 48);
  auto ni = sh->notes->find(Key(ar, ac));
  auto li = sh->links->find(Key(ar, ac));
  // a note shown all the time needs no hover box
  const bool has_note = ni != sh->notes->end() && !(ni->second.visible && !ni->second.threaded), has_link = li != sh->links->end();
  // a link: one click follows it (released within half a second, the mouse not moved); click and hold selects the cell
  // (Excel). Ctrl+click always follows; the setting follow_links_on_click = false keeps Ctrl+click only.
  const bool one_click = Config().follow_links_on_click;
  if (m.link_press && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    const ImVec2 d = io.MousePos - m.link_press_pos;
    const bool quick = ImGui::GetTime() - m.link_press_time < 0.5 && std::fabs(d.x) + std::fabs(d.y) < S(4);
    if (quick && on_sheet && has_link && key == m.link_press_key) FollowLink(s, li->second, sheet_id, ar, ac);
    m.link_press = false;
  }
  if (on_sheet && has_link && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (io.KeyCtrl) {
      FollowLink(s, li->second, sheet_id, ar, ac);
    } else if (one_click && !io.KeyShift && !s.edit.active) {
      m.link_press = true;
      m.link_press_key = key;
      m.link_press_time = ImGui::GetTime();
      m.link_press_pos = io.MousePos;
    }
  }
  if (!on_sheet || ImGui::IsAnyMouseDown() && !io.KeyCtrl) {
    m.hover_key = ~0ull;
    return;
  }
  if (!has_note && !has_link) {
    m.hover_key = ~0ull;
    return;
  }
  if (has_link) ImGui::SetMouseCursor(io.KeyCtrl || one_click ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
  if (m.hover_key != key) {
    m.hover_key = key;
    m.hover_since = ImGui::GetTime();
  }
  if (ImGui::GetTime() - m.hover_since < 0.35) {
    ui::RequestRedrawIn(0.4);
    return;
  }
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(S(280));
  if (has_link) {
    const auto& l = li->second;
    ImGui::TextUnformatted(l.tip.empty() ? (l.address.empty() ? ("#" + l.sub_address).c_str() : l.address.c_str()) : l.tip.c_str());
    ImGui::TextDisabled("%s", Config().follow_links_on_click ? TR("Click once to follow. Click and hold to select this cell.") : TR("Ctrl+click to follow link"));
  }
  if (has_note && ni->second.threaded) {
    if (has_link) ImGui::Separator();
    DrawThreadPosts(ni->second, false, nullptr, nullptr);
  } else if (has_note) {
    if (has_link) ImGui::Separator();
    if (!ni->second.author.empty()) {
      ImGui::PushFont(ui::F().ui_bold, 0.0f);
      ImGui::TextUnformatted((ni->second.author + TR(": ")).c_str());
      ImGui::PopFont();
    }
    ImGui::TextUnformatted(ni->second.text.c_str());
  }
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

// ---- Clipboard task pane ----

void DrawMiscWindows(AppState& s) {
  MiscUi& m = M();
  DrawThreadWindow(s);
  DrawCommentsPane(s);
  if (!m.show_clipboard) return;
  // the Office Clipboard collects while its pane is shown, the copy already on the clipboard when it opens included:
  // only then is a copy's text written out (a million copied rows took over a second at every Ctrl+C)
  if (s.has_clip && ClipboardSequence() == s.clip_seq && s.clip_history_seq != s.clip_seq) {
    s.clip_history_seq = s.clip_seq;
    std::string t = ClipboardTextUtf8(s.hwnd);
    if (!t.empty() && (s.clip_history.empty() || s.clip_history.back() != t)) {
      s.clip_history.push_back(std::move(t));
      if (s.clip_history.size() > 24) s.clip_history.erase(s.clip_history.begin());
    }
  }
  ImGui::SetNextWindowSize(ImVec2(S(280), S(420)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Left);
  DockAsTaskPane(TR("Clipboard###yc_clipboard"), true, S(280));  // Excel's pane is on the left of the sheets
  if (ImGui::Begin(TR("Clipboard###yc_clipboard"), &m.show_clipboard)) {
    if (ImGui::Button(TR("Clear All"))) s.clip_history.clear();
    ImGui::Separator();
    if (s.clip_history.empty()) ImGui::TextWrapped(TR("Items appear here after you copy cells (up to 24). Click an item to paste it into the active cell."));
    for (size_t i = s.clip_history.size(); i > 0; --i) {
      const std::string& t = s.clip_history[i - 1];
      // the copied text on a line: a copied cell's text ends in a line break (CR LF), which showed as nothing but
      // was in the label, and a ## in it (#,##0) cut the rest off as an ImGui id
      std::string preview(core::Utf8Prefix(t, 120));
      for (char& ch : preview) {
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
      }
      while (!preview.empty() && preview.back() == ' ') preview.pop_back();
      ImGui::PushID(static_cast<int>(i));
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const bool picked = ImGui::Selectable("##clipitem", false, 0, ImVec2(0, ImGui::GetTextLineHeight() * 2));
      const ImRect item(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), preview.c_str());
      ui::NoteUiText(preview.c_str(), item);
      if (picked) {
        // put it back on the system clipboard, then paste (like the Office clipboard in Excel)
        SetClipboardTextUtf8(s.hwnd, t);
        s.has_clip = false;
        PasteClipboard(s);
      }
      ImGui::PopID();
      ImGui::Separator();
    }
  }
  ImGui::End();
}

// ---- Commands ----

void RegisterMiscCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* ic = nullptr, const char* shortcut = "", const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  // notes
  R.Add(make("InsertComment", TR("Notes"), ui::icon::NewNote, "Shift+F2", TR("Add or edit a note for the active cell."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh || RefuseReadOnlyUi(s)) return;  // read-only mode: no note box opens to type in
    MiscUi& m = M();
    const auto& v = s.FocusView();
    m.note_sheet = sh->id;
    m.note_r = v.sel.act_r;
    m.note_c = v.sel.act_c;
    auto it = sh->notes->find(Key(m.note_r, m.note_c));
    std::snprintf(m.note_text, sizeof(m.note_text), "%s", it != sh->notes->end() ? it->second.text.c_str() : "");
    // Excel: the note opens beside its cell for typing (no dialog); a click elsewhere keeps it
    m.note_before = m.note_text;
    m.note_new = it == sh->notes->end();
    m.note_inplace = true;
    m.note_focus = true;
    m.note_caret_end = true;
  };
  // Format Comment: the note's font, alignment, colors and lines, size (right-click its border, as in Excel)
  R.Add(make("FormatComment", TR("Format Comment..."), ui::icon::Comment, "", TR("Format the note: its font, alignment, fill and line, and size."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    MiscUi& m = M();
    uint32_t r = s.FocusView().sel.act_r, c = s.FocusView().sel.act_c;
    if (m.note_inplace && m.note_sheet == sh->id) r = m.note_r, c = m.note_c;
    auto it = sh->notes->find(Key(r, c));
    if (it == sh->notes->end() || it->second.threaded) return;
    m.note_sheet = sh->id, m.note_r = r, m.note_c = c;
    m.fc_look = it->second.look;
    if (m.fc_look.w <= 0) m.fc_look.w = 150;
    if (m.fc_look.h <= 0) m.fc_look.h = 90;
    s.OpenDialog(Dialog::FormatComment);
  };
  R.Add(make("NewComment", TR("New Comment"), ui::icon::CommentNew, "Ctrl+Alt+M", TR("Start a conversation about the active cell; others can reply."))).execute =
      [&s](ui::Command&) {
        const model::Sheet* sh = s.FocusSheet();
        if (!sh) return;
        const auto& v = s.FocusView();
        OpenThread(s, sh->id, v.sel.act_r, v.sel.act_c);
      };
  R.Add(make("DeleteCommentThread", TR("Delete##ctx_comment"), ui::icon::CommentDelete, "", TR("Delete the comment threads in the selection."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    std::vector<std::pair<uint32_t, uint32_t>> cells;
    for (const auto& [k, n] : *sh->notes) {
      const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
      for (const Rect& a : areas) {
        if (n.threaded && a.Contains(r, c)) cells.emplace_back(r, c);
      }
    }
    s.engine->PostUser(TR("Delete Comment"), [sheet, cells](Document& d) {
      for (const auto& [r, c] : cells) d.SetComment(sheet, r, c, "", "", 0);
    });
  };
  R.Add(make("PreviousComment", TR("Previous Comment"), ui::icon::CommentPrevious, "", TR("Go to the previous comment on this sheet."))).execute = [&s](ui::Command&) { StepThread(s, -1); };
  R.Add(make("NextComment", TR("Next Comment"), ui::icon::CommentNext, "", TR("Go to the next comment on this sheet."))).execute = [&s](ui::Command&) { StepThread(s, 1); };
  R.Add(make("ShowComments", TR("Show Comments"), ui::icon::CommentsPane, "", TR("Show all the comment threads of this sheet in a pane."))).execute = [](ui::Command&) {
    M().show_comments = !M().show_comments;
  };
  R.Add(make("PreviousNote", TR("Previous Note"), ui::icon::CommentPrevious, "", TR("Go to the previous note on this sheet."))).execute = [&s](ui::Command&) { StepNote(s, -1); };
  R.Add(make("NextNote", TR("Next Note"), ui::icon::CommentNext, "", TR("Go to the next note on this sheet."))).execute = [&s](ui::Command&) { StepNote(s, 1); };
  R.Add(make("ShowHideNote", TR("Show/Hide Note"), ui::icon::Note, "", TR("Keep the selected cell's note on the sheet, or show it only when the mouse is over its cell."))).execute =
      [&s](ui::Command&) {
        const model::Sheet* sh = s.FocusSheet();
        if (!sh) return;
        const auto& v = s.FocusView();
        auto it = sh->notes->find(Key(v.sel.act_r, v.sel.act_c));
        if (it == sh->notes->end() || it->second.threaded) {
          s.SetStatus(TR("The active cell has no note."));
          return;
        }
        const bool show = !it->second.visible;
        const uint32_t sheet = sh->id;
        const auto areas = SelAreas(s);
        const Rect act = Rect::Cell(v.sel.act_r, v.sel.act_c);
        s.engine->PostUser(TR("Show/Hide Note"), [sheet, areas, act, show](Document& d) {
          d.SetNotesVisible(sheet, act, show);
          for (const Rect& a : areas) d.SetNotesVisible(sheet, a, show);
        });
      };
  // Review > Notes: Excel's one drop-down of the note commands
  R.Add(make("NotesMenu", TR("Notes"), ui::icon::NoteEdit, "", TR("Add a note, go to the previous or next one, show or hide them, or turn them into comments."))).execute =
      [](ui::Command&) {};
  R.Add(make("ShowAllNotes", TR("Show All Notes"), ui::icon::CommentsPane, "", TR("Show every note in the workbook on its sheet, or hide them all again."))).execute =
      [&s](ui::Command&) {
        // a toggle over the whole workbook (Excel): shown when any note is hidden, hidden when all are shown
        bool all = true, any = false;
        for (const auto& sc : s.snap->wb.sheets) {
          for (const auto& [k, n] : *sc->notes) {
            if (n.threaded) continue;
            any = true;
            all = all && n.visible;
          }
        }
        if (!any) {
          s.SetStatus(TR("There are no notes in this workbook."));
          return;
        }
        std::vector<uint32_t> sheets;
        for (const auto& sc : s.snap->wb.sheets) sheets.push_back(sc->id);
        const bool show = !all;
        s.engine->PostUser(TR("Show All Notes"), [sheets, show](Document& d) {
          for (uint32_t id : sheets) d.SetNotesVisible(id, Rect{0, 0, core::kMaxRows - 1, core::kMaxCols - 1}, show);
        });
      };
  R.Add(make("ConvertToComments", TR("Convert to Comments"), ui::icon::CommentConvert, "", TR("Turn the notes in the selection into comment threads."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    const double now = static_cast<double>(std::time(nullptr));
    s.engine->PostUser(TR("Convert to Comments"), [sheet, areas, now](Document& d) {
      for (const Rect& a : areas) d.ConvertNotesToComments(sheet, a, now);
    });
  };
  R.Add(make("DeleteComment", TR("Delete Note"))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    s.engine->PostUser(TR("Delete Note"), [sheet, areas](Document& d) {
      for (const Rect& a : areas) d.ClearNotes(sheet, a);
    });
  };
  R.Add(make("ClearComments", TR("Clear Comments and Notes"))).execute = [](ui::Command&) { ui::Commands().Execute("DeleteComment"); };
  // hyperlinks
  R.Add(make("InsertHyperlink", TR("Link"), ui::icon::Link, "Ctrl+K", TR("Create a link to a web page, a file or a place in this workbook."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    MiscUi& m = M();
    const auto& v = s.FocusView();
    auto it = sh->links->find(Key(v.sel.act_r, v.sel.act_c));
    const model::Hyperlink l = it != sh->links->end() ? it->second : model::Hyperlink{};
    std::snprintf(m.link_addr, sizeof(m.link_addr), "%s", l.address.c_str());
    std::snprintf(m.link_sub, sizeof(m.link_sub), "%s", l.sub_address.c_str());
    std::snprintf(m.link_tip, sizeof(m.link_tip), "%s", l.tip.c_str());
    m.link_kind = l.address.empty() && !l.sub_address.empty() ? 1 : 0;
    m.link_existing = it != sh->links->end();
    m.link_mail[0] = m.link_subject[0] = 0;
    if (l.address.rfind("mailto:", 0) == 0) {  // an e-mail link: its address and subject
      m.link_kind = 2;
      std::string a = l.address.substr(7), subj;
      if (const size_t q = a.find("?subject="); q != std::string::npos) subj = a.substr(q + 9), a.resize(q);
      std::snprintf(m.link_mail, sizeof(m.link_mail), "%s", a.c_str());
      std::snprintf(m.link_subject, sizeof(m.link_subject), "%s", subj.c_str());
    }
    // a place in this document: its sheet (or name) and cell
    m.link_place = 0;
    m.link_cell[0] = 0;
    {
      std::string sub = l.sub_address, sheet;
      if (const size_t bang = sub.rfind('!'); bang != std::string::npos) sheet = sub.substr(0, bang), sub = sub.substr(bang + 1);
      if (sheet.size() >= 2 && sheet.front() == '\'') sheet = sheet.substr(1, sheet.size() - 2);
      int idx = 0;
      for (const auto& x : s.snap->wb.sheets) {
        if (x->name == (sheet.empty() ? sh->name : sheet)) m.link_place = idx;
        ++idx;
      }
      const auto& names = *s.snap->wb.names;
      for (size_t k = 0; k < names.size(); ++k) {
        if (sheet.empty() && names[k].name == sub) m.link_place = static_cast<int>(s.snap->wb.sheets.size() + k), sub.clear();
      }
      std::snprintf(m.link_cell, sizeof(m.link_cell), "%s", sub.empty() && m.link_place < static_cast<int>(s.snap->wb.sheets.size()) ? "A1" : sub.c_str());
    }
    const auto& tbl = *s.snap->wb.styles;
    const core::Value cv = sh->GetValue(v.sel.act_r, v.sel.act_c);
    // a number or a formula keeps its value: the text box is greyed, as in Excel
    m.link_text_locked = cv.IsNumeric() || cv.IsBool() || sh->GetFormula(v.sel.act_r, v.sel.act_c);
    const std::string text = engine::DisplayTextOf(cv, tbl.NumberFormat(tbl.Get(sh->GetStyle(v.sel.act_r, v.sel.act_c))));
    std::snprintf(m.link_text, sizeof(m.link_text), "%s", text.c_str());
    m.link_text0 = text;
    s.OpenDialog(Dialog::HyperlinkEdit);
  };
  R.Add(make("ClearHyperlinks", TR("Clear Hyperlinks"))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    s.engine->PostUser(TR("Clear Hyperlinks"), [sheet, areas](Document& d) {
      for (const Rect& a : areas) d.ClearHyperlinks(sheet, a);
    });
  };
  // Excel's Remove Hyperlinks: the links and their formatting (the cells that had a link go back to Normal)
  R.Add(make("RemoveHyperlinks", TR("Remove Hyperlinks"))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    s.engine->PostUser(TR("Remove Hyperlinks"), [sheet, areas](Document& d) {
      for (const Rect& a : areas) {
        std::vector<Rect> cells;
        if (const model::Sheet* sh = d.S(sheet)) {
          for (const auto& [k, l] : *sh->links) {
            const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
            if (a.Contains(r, c)) cells.push_back(Rect::Cell(r, c));
          }
        }
        d.ClearHyperlinks(sheet, a);
        for (const Rect& c : cells) d.ClearFormats(sheet, c, false);
      }
    });
  };
  R.Add(make("FollowHyperlink", TR("Open Hyperlink"))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    const auto& v = s.FocusView();
    auto it = sh->links->find(Key(v.sel.act_r, v.sel.act_c));
    if (it != sh->links->end()) FollowLink(s, it->second, s.focus_sheet, v.sel.act_r, v.sel.act_c);
  };
  // Go To Special, custom sort, series
  R.Add(make("GoToSpecial", TR("Go To Special..."), nullptr, "", TR("Select blanks, constants, formulas, notes, visible cells and more."))).execute = [&s](ui::Command&) { s.OpenDialog(Dialog::GoToSpecial); };
  R.Add(make("CustomSort", TR("Sort"), ui::icon::SortFilter, "", TR("Sort by several columns, each A to Z or Z to A."))).execute = [&s](ui::Command&) {
    if (!s.FocusSheet()) return;
    RunDataAction(s, 2, FindDataRange(s));
  };
  R.Add(make("FillSeries", TR("Series..."), nullptr, "", TR("Fill the selection with a linear, growth or date series."))).execute = [&s](ui::Command&) {
    const Rect r = SelRect(s);
    M().fs_dir = r.Rows() >= r.Cols() ? 1 : 0;
    M().fs_trend = false;
    // a date in the first cell: Excel offers a Date series (by days)
    if (const model::Sheet* sh = s.FocusSheet(); sh && s.snap) {
      const auto& tbl = *s.snap->wb.styles;
      const bool date = sh->GetValue(r.r0, r.c0).IsNumber() && formula::IsDateFormat(tbl.NumberFormat(tbl.Get(sh->GetStyle(r.r0, r.c0))));
      if (date) M().fs_type = 2;
      else if (M().fs_type == 2) M().fs_type = 0;
    }
    s.OpenDialog(Dialog::FillSeries);
  };
  // Copy as Picture
  R.Add(make("CopyAsPicture", TR("Copy as Picture..."), ui::icon::Copy, "", TR("Copy the current look of the selection as a picture (to paste into other programs)."))).execute = [&s](ui::Command&) {
    AppState::ShotRequest q;
    q.target = "range";
    q.address = SelectionAddress(s);
    q.sheet = s.focus_sheet;
    q.headers = false;
    q.max_width = 0;
    q.to_clipboard = true;
    q.done = [&s](bool ok, int w, int h, const std::string&) { s.SetStatus(ok ? TR("Copied as a picture (") + std::to_string(w) + "×" + std::to_string(h) + TR(")") : TR("Couldn't copy as a picture")); };
    s.shots.push_back(q);
  };
  R.Add(make("ClipboardPane", TR("Clipboard"), nullptr, "", TR("Show the Clipboard task pane (last 24 copies)."))).execute = [](ui::Command&) { M().show_clipboard = !M().show_clipboard; };
  RegisterPane("clipboard", "ClipboardPane", []() -> bool& { return M().show_clipboard; });
  RegisterPane("comments", "ShowComments", []() -> bool& { return M().show_comments; });
}

// ---- Dialogs ----

void DrawMiscDialogs(AppState& s) {
  MiscUi& m = M();
  // the Sort dialog closed without OK (its Options page is part of it)
  if (m.sort_restore && s.dialog != Dialog::CustomSort && s.dialog != Dialog::SortOptions) {
    m.sort_restore = false;
    if (s.focus_sheet == m.sort_restore_sheet) s.FocusView().sel = m.sort_saved_sel;
  }
  // notes
  if (BeginDlg(s, Dialog::NoteEdit, TR("Notes###yc_dlg_note"), 380)) {
    ImGui::Text(TR("Note for %s"), core::CellA1(m.note_r, m.note_c).c_str());
    ImGui::InputTextMultiline("##note", m.note_text, sizeof(m.note_text), ImVec2(S(360), S(120)));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const uint32_t sheet = m.note_sheet, r = m.note_r, c = m.note_c;
      const std::string text = m.note_text, author = WindowsUser();
      s.engine->PostUser(TR("Notes"), [sheet, r, c, text, author](Document& d) { d.SetNote(sheet, r, c, author, text); });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Delete"), ImVec2(S(80), 0))) {
      const uint32_t sheet = m.note_sheet, r = m.note_r, c = m.note_c;
      s.engine->PostUser(TR("Delete Note"), [sheet, r, c](Document& d) { d.SetNote(sheet, r, c, "", ""); });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // hyperlinks
  if (BeginDlg(s, Dialog::HyperlinkEdit, m.link_existing ? TR("Edit Hyperlink###yc_dlg_link") : TR("Insert Hyperlink###yc_dlg_link"), 520)) {
    // Link to: as Excel's buttons on the left
    ImGui::TextUnformatted(TR("Link to:"));
    ui::RadioButton(TR("Existing File or Web Page"), &m.link_kind, 0);
    ImGui::SameLine();
    ui::RadioButton(TR("Place in This Document"), &m.link_kind, 1);
    ImGui::SameLine();
    ui::RadioButton(TR("E-mail Address"), &m.link_kind, 2);
    ImGui::Separator();
    ImGui::TextUnformatted(TR("Text to display:"));
    ImGui::SetNextItemWidth(-1);
    if (m.link_text_locked) {
      // a number or a formula stays as it is (Excel: <<Selection in Document>>)
      char locked[64];
      std::snprintf(locked, sizeof locked, "%s", TR("<<Selection in Document>>"));
      ImGui::BeginDisabled();
      ImGui::InputText("##text", locked, sizeof locked, ImGuiInputTextFlags_ReadOnly);
      ImGui::EndDisabled();
    } else {
      ImGui::InputText("##text", m.link_text, sizeof(m.link_text));
    }
    const auto& sheets = s.snap->wb.sheets;
    const auto& names = *s.snap->wb.names;
    if (m.link_kind == 0) {
      ImGui::TextUnformatted(TR("Address:"));
      // as in Excel the keyboard starts in the address: Ctrl+K, type the address, Enter
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - S(110));
      ImGui::InputTextWithHint("##addr", "https://", m.link_addr, sizeof(m.link_addr));
      ImGui::SameLine();
      if (ImGui::Button(TR("Browse..."), ImVec2(S(100), 0))) {
        const std::wstring f = PickAnyFile(s.hwnd, TR("Link to File"));
        if (!f.empty()) {
          const int n = ::WideCharToMultiByte(CP_UTF8, 0, f.c_str(), -1, nullptr, 0, nullptr, nullptr);
          std::string u8(static_cast<size_t>(std::max(0, n - 1)), '\0');
          ::WideCharToMultiByte(CP_UTF8, 0, f.c_str(), -1, u8.data(), n, nullptr, nullptr);
          std::snprintf(m.link_addr, sizeof(m.link_addr), "%s", u8.c_str());
        }
      }
      // Excel's Look in lists: the workbook's own folder (Current Folder) and Recent Files; a click puts the file in Address
      static int look_in = 0;
      ui::RadioButton(TR("Current Folder"), &look_in, 0);
      ImGui::SameLine();
      ui::RadioButton(TR("Recent Files"), &look_in, 1);
      ImGui::BeginChild("##recent_links", ImVec2(-1, S(110)), ImGuiChildFlags_Borders);
      std::vector<std::wstring> files;
      if (look_in == 1) {
        files = RecentFiles();
      } else if (!s.file_path.empty()) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(s.file_path).parent_path();
        for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator() && files.size() < 200; it.increment(ec)) {
          if (it->is_regular_file(ec)) files.push_back(it->path().wstring());
        }
        std::sort(files.begin(), files.end());
      }
      for (size_t k = 0; k < files.size(); ++k) {
        const std::string u8 = ToUtf8(files[k]);
        const std::string shown = look_in == 0 ? ToUtf8(std::filesystem::path(files[k]).filename().wstring()) : u8;
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::Selectable(shown.c_str(), u8 == m.link_addr)) std::snprintf(m.link_addr, sizeof(m.link_addr), "%s", u8.c_str());
        ImGui::PopID();
      }
      if (files.empty()) ImGui::TextDisabled("%s", look_in == 1 ? TR("(no recent files)") : TR("(the workbook isn't saved yet)"));
      ImGui::EndChild();
    } else if (m.link_kind == 1) {
      // Or select a place in this document: the sheets, then the defined names
      ImGui::TextUnformatted(TR("Type the cell reference:"));
      const bool is_sheet = m.link_place < static_cast<int>(sheets.size());
      ImGui::BeginDisabled(!is_sheet);
      RangeBox(s, "link.cell", m.link_cell, sizeof(m.link_cell), 0, RangeBox_Relative, TR("Type the cell reference:"));
      ImGui::EndDisabled();
      ImGui::TextUnformatted(TR("Or select a place in this document:"));
      if (ImGui::BeginChild("##places", ImVec2(0, S(150)), ImGuiChildFlags_Borders)) {
        ImGui::TextDisabled("%s", TR("Cell Reference"));
        for (size_t k = 0; k < sheets.size(); ++k) {
          if (sheets[k]->chart_sheet) continue;
          ImGui::PushID(static_cast<int>(k));
          if (ImGui::Selectable(("   " + sheets[k]->name).c_str(), m.link_place == static_cast<int>(k))) m.link_place = static_cast<int>(k);
          ImGui::PopID();
        }
        if (!names.empty()) ImGui::TextDisabled("%s", TR("Defined Names"));
        for (size_t k = 0; k < names.size(); ++k) {
          if (names[k].hidden) continue;
          ImGui::PushID(static_cast<int>(1000 + k));
          const int idx = static_cast<int>(sheets.size() + k);
          if (ImGui::Selectable(("   " + names[k].name).c_str(), m.link_place == idx)) m.link_place = idx;
          ImGui::PopID();
        }
      }
      ImGui::EndChild();
    } else {
      ImGui::TextUnformatted(TR("E-mail address:"));
      ImGui::SetNextItemWidth(-1);
      ImGui::InputTextWithHint("##mail", "name@example.com", m.link_mail, sizeof(m.link_mail));
      ImGui::TextUnformatted(TR("Subject:"));
      ImGui::SetNextItemWidth(-1);
      ImGui::InputText("##subject", m.link_subject, sizeof(m.link_subject));
    }
    ImGui::TextUnformatted(TR("ScreenTip:"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##tip", m.link_tip, sizeof(m.link_tip));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      model::Hyperlink l;
      if (m.link_kind == 0) {
        l.address = m.link_addr;
      } else if (m.link_kind == 2) {
        l.address = std::string("mailto:") + m.link_mail + (m.link_subject[0] ? std::string("?subject=") + m.link_subject : std::string());
      } else if (m.link_place < static_cast<int>(sheets.size())) {
        std::string cell = m.link_cell;
        cell.erase(std::remove(cell.begin(), cell.end(), '$'), cell.end());
        if (cell.empty()) cell = "A1";
        l.sub_address = core::QuoteSheetName(sheets[static_cast<size_t>(m.link_place)]->name) + "!" + cell;
      } else if (static_cast<size_t>(m.link_place) - sheets.size() < names.size()) {
        l.sub_address = names[static_cast<size_t>(m.link_place) - sheets.size()].name;
      }
      l.tip = m.link_tip;
      // the text is written only into a cell that holds text (or nothing) and only when it changed
      std::string text = m.link_text_locked ? std::string() : std::string(m.link_text);
      const bool write_text = !m.link_text_locked && (text != m.link_text0 || m.link_text0.empty());
      if (write_text && text.empty()) text = m.link_kind == 2 ? std::string(m.link_mail) : !l.address.empty() ? l.address : l.sub_address;
      const uint32_t sheet = s.focus_sheet;
      // every cell of the selection gets the link, as in Excel (the text goes into the active cell)
      const Rect area = SelRect(s);
      const uint32_t r = s.FocusView().sel.act_r, c = s.FocusView().sel.act_c;
      const bool many = area.Count() <= 10000;
      s.engine->PostUser(m.link_existing ? TR("Edit Hyperlink") : TR("Insert Hyperlink"), [sheet, r, c, l, text, write_text, area, many](Document& d) {
        if (write_text && !text.empty()) d.SetValue(sheet, r, c, core::Value::String(text));
        const Rect cells = many ? area : Rect::Cell(r, c);
        d.ModifyStyle(sheet, cells, [](Style& x) {
          x.font_color = model::Rgb(0x05, 0x63, 0xC1);
          x.underline = model::Underline::Single;
        });
        for (uint32_t rr = cells.r0; rr <= cells.r1; ++rr) {
          for (uint32_t cc = cells.c0; cc <= cells.c1; ++cc) d.SetHyperlink(sheet, rr, cc, l);
        }
      });
      close = true;
    }
    ImGui::SameLine();
    // Excel's Remove Link takes the link and its blue underlined look
    if (m.link_existing && ImGui::Button(TR("Remove Link"), ImVec2(S(110), 0))) ui::Commands().Execute("RemoveHyperlinks"), close = true;
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Format Comment (a note): Font, Alignment, Colors and Lines, Size
  if (BeginDlg(s, Dialog::FormatComment, TR("Format Comment###yc_dlg_fcomment"), 420)) {
    auto& lk = m.fc_look;
    bool close = false;
    auto color = [](const char* label, uint32_t* argb) {
      ImGui::PushID(label);
      const ImVec4 cv = ImGui::ColorConvertU32ToFloat4(*argb ? NoteCol(*argb, 0) : IM_COL32(0xFF, 0xFF, 0xFF, 0));
      if (ImGui::ColorButton("##sw", cv, 0, ImVec2(S(40), S(18)))) ImGui::OpenPopup("##pal");
      ImGui::SameLine();
      ImGui::TextUnformatted(label);
      if (ImGui::BeginPopup("##pal")) {
        uint32_t ref = 0;
        if (ImGui::Selectable(TR("Automatic"), *argb == 0)) *argb = 0, ImGui::CloseCurrentPopup();
        if (ui::ribbon::PaletteSwatches(&ref, nullptr)) *argb = 0xFF000000u | model::ResolveColor(ref), ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopID();
    };
    if (BeginDlgTabBar("##fctabs")) {
      if (ImGui::BeginTabItem(TR("Font"))) {
        ImGui::TextUnformatted(TR("Font:"));
        ImGui::SetNextItemWidth(S(240));
        if (ui::BeginCombo("##font", lk.font_name.empty() ? "Tahoma" : lk.font_name.c_str())) {
          for (const std::string& f : ui::SystemFontFamilies()) {
            if (ImGui::Selectable(f.c_str(), f == lk.font_name)) lk.font_name = f;
          }
          ImGui::EndCombo();
        }
        const char* kStyles[] = {TR("Regular"), TR("Italic"), TR("Bold"), TR("Bold Italic")};
        int st = (lk.bold ? 2 : 0) + (lk.italic ? 1 : 0);
        ImGui::TextUnformatted(TR("Font style:"));
        ImGui::SetNextItemWidth(S(240));
        if (ui::Combo("##style", &st, kStyles, 4)) lk.bold = st >= 2, lk.italic = st == 1 || st == 3;
        float size = lk.font_size > 0 ? lk.font_size : 9;
        ImGui::TextUnformatted(TR("Size:"));
        ImGui::SetNextItemWidth(S(240));
        if (ImGui::InputFloat("##size", &size, 1, 2, "%.0f")) lk.font_size = std::clamp(size, 1.0f, 409.0f);
        color(TR("Color:"), &lk.font_color);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Alignment"))) {
        const char* kH[] = {TR("Left"), TR("Center"), TR("Right")};
        const char* kV[] = {TR("Top"), TR("Center"), TR("Bottom")};
        ImGui::TextUnformatted(TR("Horizontal:"));
        ImGui::SetNextItemWidth(S(200));
        ui::Combo("##h", &lk.halign, kH, 3);
        ImGui::TextUnformatted(TR("Vertical:"));
        ImGui::SetNextItemWidth(S(200));
        ui::Combo("##v", &lk.valign, kV, 3);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Colors and Lines"))) {
        ImGui::SeparatorText(TR("Fill"));
        color(TR("Color:##fill"), &lk.fill_color);
        ImGui::SeparatorText(TR("Line"));
        color(TR("Color:##line"), &lk.line_color);
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem(TR("Size"))) {
        ImGui::SetNextItemWidth(S(150));
        ImGui::InputDouble(TR("Height:"), &lk.h, 1, 10, "%.2f pt");
        ImGui::SetNextItemWidth(S(150));
        ImGui::InputDouble(TR("Width:"), &lk.w, 1, 10, "%.2f pt");
        lk.w = std::max(12.0, lk.w), lk.h = std::max(12.0, lk.h);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const uint32_t sheet = m.note_sheet, r = m.note_r, c = m.note_c;
      const model::CellNote::Look l2 = lk;
      s.engine->PostUser(TR("Format Comment"), [sheet, r, c, l2](Document& d) { d.SetNoteLook(sheet, r, c, l2); });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // Go To Special
  if (BeginDlg(s, Dialog::GoToSpecial, TR("Go To Special###yc_dlg_gs"), 380)) {
    static const char* kKinds[] = {TR("Notes"), TR("Constants"), TR("Formulas"), TR("Blanks"), TR("Current region"), TR("Last cell"), TR("Visible cells only"), TR("Conditional formats"), TR("Data validation"), TR("Precedents"), TR("Dependents"), TR("Current array"), TR("Row differences"), TR("Column differences"), TR("Objects")};
    // in Excel's order, two columns
    static const int kOrder[] = {0, 1, 2, 3, 4, 11, 14, -1, 12, 13, 9, 10, 5, 6, 7, 8};
    // Excel's letters, by kKinds: (N)otes, C(o)nstants, (F)ormulas, Blan(k)s, Current (r)egion, La(s)t cell, Visible
    // cells onl(y), Condi(t)ional formats, Data (v)alidation, (P)recedents, (D)ependents, Current (a)rray, Ro(w)
    // differences, Colu(m)n differences, O(b)jects (F5, Alt+S, K, Enter selects the blanks, as people type it)
    static const ImGuiKey kKeys[] = {ImGuiKey_N, ImGuiKey_O, ImGuiKey_F, ImGuiKey_K, ImGuiKey_R, ImGuiKey_S, ImGuiKey_Y, ImGuiKey_T,
                                     ImGuiKey_V, ImGuiKey_P, ImGuiKey_D, ImGuiKey_A, ImGuiKey_W, ImGuiKey_M, ImGuiKey_B};
    for (int i = 0; i < static_cast<int>(std::size(kKeys)); ++i) {
      if (DlgKey(kKeys[i])) m.gs_kind = i;
    }
    if ((m.gs_kind == 9 || m.gs_kind == 10) && DlgKey(ImGuiKey_I)) m.gs_all_levels = false;  // D(i)rect only
    if ((m.gs_kind == 9 || m.gs_kind == 10) && DlgKey(ImGuiKey_L)) m.gs_all_levels = true;   // All (l)evels
    if (m.gs_kind == 1 || m.gs_kind == 2) {  // N(u)mbers, Te(x)t, Lo(g)icals, (E)rrors
      if (DlgKey(ImGuiKey_U)) m.gs_num = !m.gs_num;
      if (DlgKey(ImGuiKey_X)) m.gs_text = !m.gs_text;
      if (DlgKey(ImGuiKey_G)) m.gs_logical = !m.gs_logical;
      if (DlgKey(ImGuiKey_E)) m.gs_err = !m.gs_err;
    }
    ImGui::BeginGroup();
    for (const int i : kOrder) {
      if (i < 0) {
        ImGui::EndGroup();
        ImGui::SameLine(0, S(24));
        ImGui::BeginGroup();
        continue;
      }
      ui::RadioButton(kKinds[i], &m.gs_kind, i);
      if ((i == 9 || i == 10) && m.gs_kind == i) {
        ImGui::Indent();
        int lv = m.gs_all_levels ? 1 : 0;
        ui::RadioButton(TR("Direct only"), &lv, 0);
        ImGui::SameLine();
        ui::RadioButton(TR("All levels"), &lv, 1);
        m.gs_all_levels = lv == 1;
        ImGui::Unindent();
      }
      if ((i == 7 || i == 8) && m.gs_kind == i) {
        ImGui::Indent();
        int same = m.gs_same ? 1 : 0;
        ui::RadioButton(TR("All##gs_rules"), &same, 0);
        ImGui::SameLine();
        ui::RadioButton(TR("Same##gs_rules"), &same, 1);
        m.gs_same = same == 1;
        ImGui::Unindent();
      }
      if (i == 1 || i == 2) {
        if (m.gs_kind == i) {
          ImGui::Indent();
          ImGui::Checkbox(TR("Numbers"), &m.gs_num);
          ImGui::SameLine();
          ImGui::Checkbox(TR("Text"), &m.gs_text);
          ImGui::SameLine();
          ImGui::Checkbox(TR("Logicals"), &m.gs_logical);
          ImGui::SameLine();
          ImGui::Checkbox(TR("Errors"), &m.gs_err);
          ImGui::Unindent();
        }
      }
    }
    ImGui::EndGroup();
    bool close = false;
    const bool gs_ok = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
    if (gs_ok && m.gs_kind == 14) {
      // Objects: every object on the sheet is selected (Excel's "No objects were found." when there is none)
      if (SelectAllObjects(s, s.focus_sheet) == 0) s.ShowMessage("YCGrid", TR("No objects were found."));
      close = true;
    } else if (gs_ok && s.FocusSheet()) {
      const model::Sheet& sh = *s.FocusSheet();
      auto& view = s.FocusView();
      Rect within = SelRect(s);
      // notes, conditional formats and validation can sit on empty cells, outside the used range: the whole sheet
      if (within.IsCell() && (m.gs_kind == 0 || m.gs_kind == 7 || m.gs_kind == 8)) {
        within = Rect{0, 0, sh.row_limit - 1, sh.col_limit - 1};
      } else {
        if (within.IsCell() && sh.has_used) within = Rect{0, 0, sh.used.r1, sh.used.c1};
        if (sh.has_used) {
          within.r1 = std::min(within.r1, sh.used.r1);
          within.c1 = std::min(within.c1, sh.used.c1);
        }
      }
      std::vector<Rect> areas;
      static const engine::Special kMap[] = {engine::Special::Comments, engine::Special::Constants, engine::Special::Formulas, engine::Special::Blanks,
                                             engine::Special::Comments, engine::Special::LastCell, engine::Special::Visible,
                                             engine::Special::AllFormatConditions, engine::Special::AllValidation,
                                             engine::Special::Blanks, engine::Special::Blanks};
      const int values = (m.gs_num ? 1 : 0) | (m.gs_text ? 2 : 0) | (m.gs_logical ? 4 : 0) | (m.gs_err ? 16 : 0);
      if (m.gs_kind == 9 || m.gs_kind == 10) {
        const uint32_t sid = s.focus_sheet;
        const Rect src = SelRect(s);
        const bool prec = m.gs_kind == 9, direct = !m.gs_all_levels;
        areas = s.engine->Call([&](engine::Document& d) { return prec ? d.Precedents(sid, src, direct) : d.Dependents(sid, src, direct); });
        for (Rect& a : areas) a.r1 = std::min(a.r1, sh.row_limit - 1);
      } else if (m.gs_kind == 4) areas.push_back(engine::CurrentRegion(sh, view.sel.act_r, view.sel.act_c));
      else if (m.gs_kind == 12 || m.gs_kind == 13) {
        // Row / Column differences: each row (column) against its cell in the active cell's column (row); one cell
        // selected finds none (Excel 16, measured by Ctrl+\)
        const Rect sel = SelRect(s);
        if (!sel.IsCell()) areas = engine::DifferenceCells(sh, sel, view.sel.act_r, view.sel.act_c, m.gs_kind == 12);
      }
      else if (m.gs_kind == 11) {
        // Current array: the array formula entered with Ctrl+Shift+Enter the active cell belongs to; a dynamic array's
        // spill isn't one (Excel 16 finds no cells there, measured by Ctrl+/)
        uint32_t ar = 0, ac = 0;
        const model::SpillRange* sp = sh.spills->empty() ? nullptr : sh.SpillAt(view.sel.act_r, view.sel.act_c, &ar, &ac);
        if (sp && sp->cse) areas.push_back(sp->rect);
      } else if ((m.gs_kind == 7 || m.gs_kind == 8) && m.gs_same) {
        // Same: the cells that share a rule with the active cell
        const uint32_t ar = view.sel.act_r, ac = view.sel.act_c;
        auto take = [&](const std::vector<Rect>& ranges) {
          bool has = false;
          for (const Rect& q : ranges) has = has || q.Contains(ar, ac);
          if (has) areas.insert(areas.end(), ranges.begin(), ranges.end());
        };
        if (m.gs_kind == 7) {
          for (const auto& rule : *sh.cond_formats) take(rule.ranges);
        } else {
          for (const auto& rule : *sh.validations) take(rule.ranges);
        }
        std::sort(areas.begin(), areas.end(), [](const Rect& x, const Rect& y) { return x.r0 != y.r0 ? x.r0 < y.r0 : x.c0 < y.c0; });
        areas.erase(std::unique(areas.begin(), areas.end()), areas.end());
      } else areas = engine::SpecialCells(sh, within, kMap[m.gs_kind], values);
      if (areas.empty()) {
        s.ShowMessage("YCGrid", TR("No cells were found."));
      } else {
        if (areas.size() > 10000) areas.resize(10000);
        // the active cell is the first area's corner; the active area is the last one, so the first goes last
        std::rotate(areas.begin(), areas.begin() + 1, areas.end());
        view.sel.areas = areas;
        view.sel.anc_r = view.sel.act_r = areas.back().r0;
        view.sel.anc_c = view.sel.act_c = areas.back().c0;
        view.sel.mode = render::SelectMode::Cells;
        render::EnsureVisible(sh, view, view.sel.act_r, view.sel.act_c);
      }
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // custom sort: Excel's Sort dialog (levels of Column / Sort On / Order; Add, Delete, Copy, Move Up / Down)
  if (BeginDlg(s, Dialog::CustomSort, TR("Sort###yc_dlg_sort"), 700)) {
    const model::Sheet* sh = s.FocusSheet();
    const Rect r = m.sort_rect;
    const auto& tbl = *s.snap->wb.styles;
    m.sort_sel = std::clamp(m.sort_sel, 0, std::max(0, static_cast<int>(m.levels.size()) - 1));
    // the tool bar, with Excel 16's letters (its dialog's AccessKey through UI Automation): Add Level A, Delete Level D,
    // Copy Level C, Options O, My data has headers H
    if ((ImGui::Button(TR("Add Level")) || DlgKey(ImGuiKey_A)) && m.levels.size() < 64) {
      m.levels.insert(m.levels.begin() + m.sort_sel + 1, MiscUi::Level{});
      ++m.sort_sel;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m.levels.size() <= 1);
    if ((ImGui::Button(TR("Delete Level")) || (m.levels.size() > 1 && DlgKey(ImGuiKey_D)))) m.levels.erase(m.levels.begin() + m.sort_sel);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m.levels.empty() || m.levels.size() >= 64);
    if (ImGui::Button(TR("Copy Level")) || (!m.levels.empty() && m.levels.size() < 64 && DlgKey(ImGuiKey_C))) {
      const MiscUi::Level copy = m.levels[static_cast<size_t>(m.sort_sel)];
      m.levels.insert(m.levels.begin() + m.sort_sel + 1, copy);
      ++m.sort_sel;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m.sort_sel <= 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
      std::swap(m.levels[static_cast<size_t>(m.sort_sel)], m.levels[static_cast<size_t>(m.sort_sel - 1)]);
      --m.sort_sel;
    }
    ImGui::SetItemTooltip("%s", TR("Move Up"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m.sort_sel + 1 >= static_cast<int>(m.levels.size()));
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
      std::swap(m.levels[static_cast<size_t>(m.sort_sel)], m.levels[static_cast<size_t>(m.sort_sel + 1)]);
      ++m.sort_sel;
    }
    ImGui::SetItemTooltip("%s", TR("Move Down"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool open_options = false;
    if (ImGui::Button(TR("Options...")) || DlgKey(ImGuiKey_O)) {
      m.sort_case_edit = m.sort_case;
      m.sort_ltr_edit = m.sort_ltr;
      open_options = true;  // the Sort Options dialog; the Sort dialog comes back with its levels
    }
    ImGui::SameLine(S(470));
    bool header_box = (m.sort_header || m.sort_table) && !m.sort_ltr;
    ImGui::BeginDisabled(m.sort_table != 0 || m.sort_ltr);  // a table's header row never moves; left to right has none
    if (!m.sort_table && !m.sort_ltr && DlgKey(ImGuiKey_H)) m.sort_header = !m.sort_header;
    if (ImGui::Checkbox(TR("My data has headers"), &header_box) && !m.sort_table) m.sort_header = header_box;
    ImGui::EndDisabled();
    ImGui::TextDisabled(TR("Range: %s:%s"), core::CellA1(r.r0, r.c0).c_str(), core::CellA1(r.r1, r.c1).c_str());
    // the columns by their headings, and what each holds (text, numbers or dates) and its colors
    const bool has_header = (m.sort_header || m.sort_table) && sh && !m.sort_ltr;
    const uint32_t hr = m.sort_table && r.r0 > 0 ? r.r0 - 1 : r.r0;
    const uint32_t data_r0 = m.sort_header && !m.sort_table && !m.sort_ltr ? r.r0 + 1 : r.r0;
    // Excel selects the cells it will sort behind the dialog (without the header row while My data has headers is on),
    // the active cell where it was: from one cell the sheet kept showing that cell alone
    if (s.FocusSheet() && data_r0 <= r.r1) {
      auto& sel = s.FocusView().sel;
      const Rect want{data_r0, r.c0, r.r1, r.c1};
      if (sel.areas.size() != 1 || !(sel.areas[0] == want)) {
        sel.areas = {want};
        if (!want.Contains(sel.act_r, sel.act_c)) sel.act_r = want.r0, sel.act_c = want.c0;
        sel.anc_r = want.r0, sel.anc_c = want.c0;
        sel.mode = render::SelectMode::Cells;
      }
    }
    std::vector<std::string> names;
    if (m.sort_ltr) {
      for (uint32_t row = r.r0; row <= r.r1 && row - r.r0 < 1000; ++row) names.push_back(TR("Row ") + std::to_string(row + 1));
    }
    for (uint32_t c = r.c0; c <= r.c1 && !m.sort_ltr; ++c) {
      std::string n = TR("Column ") + core::ColumnName(c);
      if (has_header) {
        const std::string h = engine::DisplayTextOf(sh->GetValue(hr, c), tbl.NumberFormat(tbl.Get(sh->GetStyle(hr, c))));
        if (!h.empty()) n = h;
      }
      names.push_back(n);
    }
    // Sort On Conditional Formatting Icon: the icons the column shows, first seen first
    auto column_icons = [&](uint32_t c) {
      std::vector<std::pair<int, int>> out;
      if (!sh || !s.snap) return out;
      engine::CondFormatEval cf(s.snap->wb, s.focus_sheet);
      if (cf.Empty()) return out;
      for (uint32_t row = data_r0; row <= r.r1 && row - data_r0 < 5000 && out.size() < 32; ++row) {
        engine::CfVisual vis;
        if (!cf.Eval(row, c, &vis) || vis.icon < 0) continue;
        const std::pair<int, int> k{vis.icon_style, vis.icon};
        if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k); // lesson-ok: find-push at most 32 icons
      }
      return out;
    };
    auto column_kind = [&](uint32_t c) {  // 0 text, 1 numbers, 2 dates: the first value under the heading
      for (uint32_t row = data_r0; sh && row <= r.r1 && row - data_r0 < 2000; ++row) {
        const auto& v = sh->GetValue(row, c);
        if (v.IsEmpty()) continue;
        if (v.IsNumber()) return formula::IsDateFormat(tbl.NumberFormat(tbl.Get(sh->GetStyle(row, c)))) ? 2 : 1;
        return 0;
      }
      return 0;
    };
    auto column_colors = [&](uint32_t c, int sort_on) {  // the fill / font colors in the column, first seen first
      std::vector<uint32_t> out;
      for (uint32_t row = data_r0; sh && row <= r.r1 && row - data_r0 < 5000 && out.size() < 32; ++row) {
        const model::Style& st = tbl.Get(sh->GetStyle(row, c));
        uint32_t col = sort_on == 1 ? st.fill_color : st.font_color;
        if (!(col >> 24)) col = 0;
        if (std::find(out.begin(), out.end(), col) == out.end()) out.push_back(col); // lesson-ok: find-push at most 32 colors
      }
      return out;
    };
    auto swatch = [&](uint32_t col, int sort_on, float w) {  // a color's box, as Excel's Order list shows it
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = ImGui::GetTextLineHeight();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (col) {
        dl->AddRectFilled(p + ImVec2(0, 1), p + ImVec2(w, h - 1), model::ResolveColor(col) | 0xFF000000u);
      } else {
        dl->AddRect(p + ImVec2(0, 1), p + ImVec2(w, h - 1), IM_COL32(0x80, 0x80, 0x80, 255));
        const char* t = sort_on == 1 ? TR("No Cell Color") : TR("Automatic");
        dl->AddText(p + ImVec2(S(4), 0), IM_COL32(0x40, 0x40, 0x40, 255), t);
      }
      ImGui::Dummy(ImVec2(w, h));
    };
    const auto lists = engine::AllFillLists();
    auto list_text = [&](int i) {
      std::string t;
      if (i < 0 || i >= static_cast<int>(lists.size())) return t;
      for (const auto& x : lists[static_cast<size_t>(i)]) t += (t.empty() ? "" : ", ") + x;
      if (t.size() <= 60) return t;
      return std::string(core::Utf8Prefix(t, 57)) + "...";  // not inside a character (Chinese lists)
    };
    // the levels: Column | Sort On | Order
    const float c_col = S(160), c_on = S(170), c_ord = S(250);
    if (ImGui::BeginTable("##levels", 4, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(S(670), S(200)))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("##lbl", ImGuiTableColumnFlags_WidthFixed, S(60));
      ImGui::TableSetupColumn(m.sort_ltr ? TR("Row") : TR("Column"), ImGuiTableColumnFlags_WidthFixed, c_col);
      ImGui::TableSetupColumn(TR("Sort On"), ImGuiTableColumnFlags_WidthFixed, c_on);
      ImGui::TableSetupColumn(TR("Order"), ImGuiTableColumnFlags_WidthFixed, c_ord);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < m.levels.size(); ++i) {
        MiscUi::Level& l = m.levels[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // the row's label chooses the level (Delete / Copy / Move act on it)
        if (ImGui::Selectable(i == 0 ? TR("Sort by") : TR("Then by"), m.sort_sel == static_cast<int>(i), ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                              ImVec2(0, ImGui::GetFrameHeight())))
          m.sort_sel = static_cast<int>(i);
        ImGui::TableNextColumn();
        l.col = std::clamp(l.col, 0, std::max(0, static_cast<int>(names.size()) - 1));
        const uint32_t c = m.sort_ltr ? r.c0 : r.c0 + static_cast<uint32_t>(l.col);
        ImGui::SetNextItemWidth(c_col);
        ui::NoteUiItem((std::string("##col") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(c_col, ImGui::GetFrameHeight())));
        if (ui::BeginCombo("##col", names.empty() ? "" : names[static_cast<size_t>(l.col)].c_str())) {
          for (size_t k = 0; k < names.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            // the header's text as it is ("C##1" too): an id-only item and the text beside it
            if (ImGui::Selectable("##colname", l.col == static_cast<int>(k))) {
              l.col = static_cast<int>(k);
              l.color = 0;
              m.sort_sel = static_cast<int>(i);
            }
            ui::NoteUiText(names[k].c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));  // ui.items finds it by its text
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(names[k].c_str());
            ImGui::PopID();
          }
          ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        static const char* kOn[] = {TR("Cell Values"), TR("Cell Color"), TR("Font Color"), TR("Conditional Formatting Icon")};
        ImGui::SetNextItemWidth(c_on);
        ui::NoteUiItem((std::string("##on") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(c_on, ImGui::GetFrameHeight())));
        if (ui::Combo("##on", &l.sort_on, kOn, m.sort_ltr ? 3 : 4)) {
          l.desc = false;
          l.list = -1;
          const auto cols = l.sort_on == 3 ? std::vector<uint32_t>{} : column_colors(c, l.sort_on);
          l.color = 0;
          for (uint32_t x : cols) {
            if (x) {
              l.color = x;  // the first color in the column, as Excel picks it
              break;
            }
          }
          if (l.sort_on == 3) {  // the first icon the column shows
            const auto icons = column_icons(c);
            l.icon_set = icons.empty() ? 0 : icons[0].first;
            l.icon = icons.empty() ? -1 : icons[0].second;
          }
        }
        ImGui::TableNextColumn();
        if (l.sort_on == 0) {
          // the words follow what the column holds; Custom List... orders by one of the lists (File > Options)
          const int kind = column_kind(c);
          static const char* kAsc[] = {TR("A to Z"), TR("Smallest to Largest"), TR("Oldest to Newest")};
          static const char* kDesc[] = {TR("Z to A"), TR("Largest to Smallest"), TR("Newest to Oldest")};
          const std::string shown = l.list >= 0 ? list_text(l.list) + (l.desc ? TR(" (reversed)") : "") : (l.desc ? kDesc[kind] : kAsc[kind]);
          ImGui::SetNextItemWidth(c_ord);
          ui::NoteUiItem((std::string("##ord") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(c_ord, ImGui::GetFrameHeight())));
          if (ui::BeginCombo("##ord", shown.c_str())) {
            if (ImGui::Selectable(kAsc[kind], l.list < 0 && !l.desc)) l.list = -1, l.desc = false;
            if (ImGui::Selectable(kDesc[kind], l.list < 0 && l.desc)) l.list = -1, l.desc = true;
            ImGui::SeparatorText(TR("Custom List"));
            for (int k = 0; k < static_cast<int>(lists.size()); ++k) {
              ImGui::PushID(k);
              if (ImGui::Selectable(list_text(k).c_str(), l.list == k && !l.desc)) l.list = k, l.desc = false;
              ImGui::PopID();
            }
            ImGui::EndCombo();
          }
        } else if (l.sort_on == 3) {
          // Sort by icon: the icons the column shows, then On Top / On Bottom
          const auto icons = column_icons(c);
          const float cw = S(130);
          ImGui::SetNextItemWidth(cw);
          ui::NoteUiItem((std::string("##icon") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(cw, ImGui::GetFrameHeight())));
          const ImVec2 at = ImGui::GetCursorScreenPos();
          const float ih = ImGui::GetTextLineHeight();
          if (ui::BeginCombo("##icon", " ", ImGuiComboFlags_HeightLarge)) {
            for (size_t k = 0; k < icons.size(); ++k) {
              ImGui::PushID(static_cast<int>(k));
              const ImVec2 p = ImGui::GetCursorScreenPos();
              if (ImGui::Selectable("##ic", l.icon_set == icons[k].first && l.icon == icons[k].second, 0, ImVec2(S(120), 0))) {
                l.icon_set = icons[k].first;
                l.icon = icons[k].second;
              }
              render::DrawRuleIcon(ImGui::GetWindowDrawList(), ImRect(p, p + ImVec2(ih, ih)), icons[k].first, icons[k].second);
              ImGui::PopID();
            }
            if (icons.empty()) ImGui::TextDisabled("%s", TR("No icons in this column"));
            ImGui::EndCombo();
          }
          if (l.icon >= 0) {
            const ImVec2 p = at + ImGui::GetStyle().FramePadding;
            render::DrawRuleIcon(ImGui::GetWindowDrawList(), ImRect(p, p + ImVec2(ih, ih)), l.icon_set, l.icon);
          }
          ImGui::SameLine();
          int pos = l.desc ? 1 : 0;
          static const char* kPos[] = {TR("On Top"), TR("On Bottom")};
          ImGui::SetNextItemWidth(c_ord - cw - ImGui::GetStyle().ItemSpacing.x);
          ui::NoteUiItem((std::string("##ipos") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(c_ord - cw, ImGui::GetFrameHeight())));
          if (ui::Combo("##ipos", &pos, kPos, 2)) l.desc = pos == 1;
        } else {
          // Sort by color: the colors in the column (as swatches), then On Top / On Bottom
          const auto cols = column_colors(c, l.sort_on);
          const float cw = S(130);
          ImGui::SetNextItemWidth(cw);
          ui::NoteUiItem((std::string("##color") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(cw, ImGui::GetFrameHeight())));
          const ImVec2 at = ImGui::GetCursorScreenPos();
          if (ui::BeginCombo("##color", " ", ImGuiComboFlags_HeightLarge)) {
            for (size_t k = 0; k < cols.size(); ++k) {
              ImGui::PushID(static_cast<int>(k));
              const ImVec2 p = ImGui::GetCursorScreenPos();
              if (ImGui::Selectable("##sw", l.color == cols[k], 0, ImVec2(S(120), 0))) l.color = cols[k];
              ImGui::SetCursorScreenPos(p);
              swatch(cols[k], l.sort_on, S(120));
              ImGui::PopID();
            }
            ImGui::EndCombo();
          }
          {  // the chosen color drawn in the closed box
            const ImVec2 save = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(at + ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
            swatch(l.color, l.sort_on, cw - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x * 2);
            ImGui::SetCursorScreenPos(save);
          }
          ImGui::SameLine();
          int pos = l.desc ? 1 : 0;
          static const char* kPos[] = {TR("On Top"), TR("On Bottom")};
          ImGui::SetNextItemWidth(c_ord - cw - ImGui::GetStyle().ItemSpacing.x);
          ui::NoteUiItem((std::string("##pos") + std::to_string(i)).c_str(), ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(c_ord - cw, ImGui::GetFrameHeight())));
          if (ui::Combo("##pos", &pos, kPos, 2)) l.desc = pos == 1;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::Separator();
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      std::vector<engine::SortKey> keys;
      std::vector<model::SortLevel> levels;
      bool values_only = true;
      for (const auto& l : m.levels) {
        engine::SortKey k{(m.sort_ltr ? r.r0 : r.c0) + static_cast<uint32_t>(l.col), l.desc};
        k.sort_on = l.sort_on;
        k.color = l.color;
        k.icon_set = l.icon_set;
        k.icon = l.icon;
        k.custom_list = l.sort_on == 0 ? l.list : -1;
        values_only = values_only && l.sort_on == 0 && l.list < 0;
        keys.push_back(k);
        levels.push_back(model::SortLevel{static_cast<uint32_t>(l.col), l.desc});
      }
      const uint32_t sheet = s.focus_sheet;
      const bool header = m.sort_header && !m.sort_table && !m.sort_ltr;
      const bool match_case = m.sort_case, ltr = m.sort_ltr;
      values_only = values_only && !ltr && !match_case;
      // sorting the AutoFilter's list records the levels (the buttons show them, Reapply and this dialog reuse them)
      bool in_filter = false;
      if (sh) {
        const model::AutoFilter& af = *sh->autofilter;
        in_filter = values_only && af.on && af.range.c0 == r.c0 && af.range.c1 == r.c1 && af.range.r0 + (header ? 0 : 1) == r.r0;
      }
      s.engine->PostUser(TR("Sort"), [sheet, r, keys, header, in_filter, levels, match_case, ltr](Document& d) {
        d.Sort(sheet, r, keys, header, true, match_case, ltr);
        if (in_filter) d.SetFilterSort(sheet, levels);
      });
      m.sort_restore = false;
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close || open_options);
    if (open_options) s.OpenDialog(Dialog::SortOptions);
  }
  // Sort Options: Case sensitive; Orientation (top to bottom, left to right); back to the Sort dialog
  if (BeginDlg(s, Dialog::SortOptions, TR("Sort Options###yc_dlg_sortopts"), 260)) {
    bool close = false, done = false;
    ImGui::Checkbox(TR("Case sensitive"), &m.sort_case_edit);
    ImGui::SeparatorText(TR("Orientation"));
    int o = m.sort_ltr_edit ? 1 : 0;
    if (ui::RadioButton(TR("Sort top to bottom"), &o, 0) || ui::RadioButton(TR("Sort left to right"), &o, 1)) m.sort_ltr_edit = o == 1;
    ImGui::Separator();
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      if (m.sort_ltr_edit != m.sort_ltr) {
        // the levels named columns, now rows (or back): start again from the first one
        m.levels.assign(1, MiscUi::Level{});
        m.sort_sel = 0;
      }
      m.sort_case = m.sort_case_edit;
      m.sort_ltr = m.sort_ltr_edit;
      close = done = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = done = true;
    EndDlg(s, close);
    if (done) s.OpenDialog(Dialog::CustomSort);
  }
  // series: Excel's Series dialog (Series in, Type, Date unit, Trend, Step value, Stop value)
  if (BeginDlg(s, Dialog::FillSeries, TR("Series###yc_dlg_series"), 420)) {
    ImGui::BeginGroup();
    ImGui::TextUnformatted(TR("Series in"));
    ui::RadioButton(TR("Rows"), &m.fs_dir, 0);
    ui::RadioButton(TR("Columns"), &m.fs_dir, 1);
    ImGui::EndGroup();
    ImGui::SameLine(S(130));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(TR("Type"));
    ui::RadioButton(TR("Linear"), &m.fs_type, 0);
    ui::RadioButton(TR("Growth"), &m.fs_type, 1);
    ui::RadioButton(TR("Date"), &m.fs_type, 2);
    ui::RadioButton(TR("AutoFill"), &m.fs_type, 3);
    ImGui::EndGroup();
    ImGui::SameLine(S(260));
    ImGui::BeginGroup();
    ImGui::BeginDisabled(m.fs_type != 2);  // the units only mean something for dates, as in Excel
    ImGui::TextUnformatted(TR("Date unit"));
    ui::RadioButton(TR("Day"), &m.fs_unit, 0);
    ui::RadioButton(TR("Weekday"), &m.fs_unit, 1);
    ui::RadioButton(TR("Month"), &m.fs_unit, 2);
    ui::RadioButton(TR("Year"), &m.fs_unit, 3);
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::BeginDisabled(m.fs_type >= 2);
    ImGui::Checkbox(TR("Trend"), &m.fs_trend);
    ImGui::EndDisabled();
    // Step and Stop take numbers or dates (2026/12/31), as Excel reads them; Trend and AutoFill don't use them
    ImGui::BeginDisabled(m.fs_type == 3 || (m.fs_trend && m.fs_type < 2));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Step value:"));
    ImGui::SameLine(S(90));
    ImGui::SetNextItemWidth(S(100));
    ImGui::InputText("##step", m.fs_step, sizeof(m.fs_step));
    ImGui::SameLine(S(210));
    ImGui::TextUnformatted(TR("Stop value:"));
    ImGui::SameLine(S(300));
    ImGui::SetNextItemWidth(S(100));
    ImGui::InputText("##stop", m.fs_stop, sizeof(m.fs_stop));
    ImGui::EndDisabled();
    auto read = [](const char* text, double* out) {
      std::string t = text;
      while (!t.empty() && t.back() == ' ') t.pop_back();
      while (!t.empty() && t.front() == ' ') t.erase(t.begin());
      if (t.empty()) return false;
      char* end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (end && *end == 0) {
        *out = v;
        return true;
      }
      return formula::ParseDateTime(t, out);
    };
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      double step = 1, stop = 0;
      const bool step_ok = m.fs_type == 3 || m.fs_trend || read(m.fs_step, &step);
      const bool has_stop = m.fs_type != 3 && !m.fs_trend && m.fs_stop[0] != 0;
      if (!step_ok || (has_stop && !read(m.fs_stop, &stop))) {
        s.ShowError("YCGrid", TR("Enter a number or a date."));
      } else {
        const int dir = m.fs_dir, type = m.fs_type, unit = m.fs_unit;
        const bool trend = m.fs_trend && type < 2;
        const uint32_t sheet = s.focus_sheet;
        const auto areas = SelAreas(s);
        s.engine->PostUser(TR("Series"), [=](Document& d) {
          for (const Rect& a : areas) {
            if (type == 3) {
              // AutoFill: the first row (column) of the selection continued over the rest, as a drag of the fill handle
              const Rect src = dir == 1 ? Rect{a.r0, a.c0, a.r0, a.c1} : Rect{a.r0, a.c0, a.r1, a.c0};
              if (!(src == a)) d.AutoFill(sheet, src, a, 0);
            } else if (!trend || !d.FillTrend(sheet, a, dir == 1, type == 1)) {
              d.FillSeries(sheet, a, dir == 1, type, unit, step, has_stop, stop);
            }
          }
        });
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
}

}  // namespace yc::app
