// SPDX-License-Identifier: Apache-2.0
// dialogs.cpp - dialogs and context menus (PLAN §9, §11)
#include "app_state.h"
#include "settings.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "icons/yc_icons.h"
#include "formula/func_help.h"
#include "formula/functions.h"
#include "formula/numfmt.h"
#include "frame/frame_context.h"
#include "grid/grid.h"
#include "ribbon/ribbon.h"
#include "dock/dock_layout.h"
#include "theme/theme.h"
#include "yc_ui_items.h"
#include "trust.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

void Exec(const char* id) { ui::Commands().Execute(id); }

bool MenuCmd(const char* id, const char* label_override = nullptr) {
  ui::Command* c = ui::Commands().Find(id);
  std::string label = label_override ? label_override : (c ? c->label : id);
  for (char& ch : label) {
    if (ch == '\n') ch = ' ';
  }
  const bool enabled = !c || c->enabled;
  if (ImGui::MenuItem(label.c_str(), c && !c->shortcut.empty() ? c->shortcut.c_str() : nullptr, false, enabled)) {
    Exec(id);
    return true;
  }
  return false;
}

uint32_t g_rename_sheet = 0;
char g_rename_buf[128];
bool g_rename_open = false;    // the Rename Sheet box (when the tab isn't on the screen)
bool g_rename_inplace = false;  // the name is edited on the tab (Excel)
bool g_rename_focus = false;
int g_rename_unseen = 0;        // frames the tab bar didn't draw the box (a scrolled-away or hidden tab: the box opens instead)
uint32_t g_tab_menu_sheet = 0;
bool g_open_cell_menu = false, g_open_col_menu = false, g_open_row_menu = false, g_open_tab_menu = false;

// centered modal window
bool BeginDialog(AppState& s, const char* title, Dialog d, ImVec2 size) {
  if (s.dialog == d && s.dialog_open_request) {
    ImGui::OpenPopup(title);
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(title, &open, ImGuiWindowFlags_NoSavedSettings)) {
    // the popup went away without the dialog closing it (another popup took its place): the dialog is closed,
    // or YCGrid would wait for a dialog nobody can see, and Escape couldn't reach it
    if (!RangePicking() && s.dialog == d && !s.dialog_open_request && !ImGui::IsPopupOpen(title)) s.dialog = Dialog::None;
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

void EndDialog(AppState& s, bool close) {
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

bool OkCancel(bool* ok) {
  ImGui::Separator();
  const float w = S(88);
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w * 2 - S(24));
  *ok = ImGui::Button(TR("OK"), ImVec2(w, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  ImGui::SameLine();
  const bool cancel = ImGui::Button(TR("Cancel"), ImVec2(w, 0));
  return *ok || cancel;
}

// ---- Format Cells ----
struct FormatDlg {
  model::Style base, edit;
  bool chars = false;  // editing a cell with selected characters: the Font tab changes only them
  std::string numfmt, font_name;
  std::string base_numfmt, base_font;
  std::string opened_code;  // the code the Number tab showed when the dialog opened: no change there keeps base_numfmt
  bool merge = false, merge0 = false;  // Alignment > Merge cells (the active cell in a merged area when it opened)
  core::Value sample;
  int category = 0;
  int decimals = 2;
  bool thousands = false;
  int negative = 0;       // Number / Currency: Excel's four negative number formats
  int symbol = 1;         // Currency / Accounting: kSymbols
  std::string type_code;  // Date / Time / Fraction / Special: the type picked in the list
  bool type_picked = false;  // a type was clicked: OK applies it even when the list showed it chosen already
  char custom[256] = "";
  char font_filter[64] = "";
  model::LineStyle line = model::LineStyle::Thin;
  ImVec4 line_color{0, 0, 0, 1};
  // Border tab: top, bottom, left, right, inside horizontal, inside vertical, diagonal down, diagonal up.
  // edges: 0 keep, 1 set to side[e] (the line style and color chosen when that edge or preset was clicked), 2 clear;
  // init / init_mixed: what the selection has now (mixed: the cells differ, drawn gray as in Excel)
  int edges[8] = {};
  model::BorderSide side[8] = {};
  model::BorderSide init[8] = {};
  bool init_mixed[8] = {};
} g_fmt;
// a code Excel refuses was typed: the message replaces the dialog, which comes back as it was once the message closes
bool g_fmt_return = false, g_fmt_keep = false;

// Format Cells for a format to look for or to write (Find & Replace's Format...): the dialog starts from spec and
// hands back the fields chosen (title: "Find Format" / "Replace Format")
struct FormatFor {
  bool on = false;
  const char* title = "";
  engine::FormatSpec spec;
  std::function<void(const engine::FormatSpec&)> done;
  std::function<void()> pick;  // Choose Format From Cell...
  bool find = true;            // Find Format's Choose Format From Cell... and Clear buttons
  unsigned tabs = 0x3F;        // the tabs shown (bit per tab: Number, Alignment, Font, Border, Fill, Protection)
  bool inner = false;          // the Border tab offers inside lines (a table style's element)
  model::BorderSide inner_h, inner_v;  // in: the inside lines; out: those chosen
} g_fmt_for;

const char* kCategories[] = {TR("General"), TR("Number"), TR("Currency"), TR("Accounting"), TR("Date"),   TR("Time"),   TR("Percentage"),
                             TR("Fraction"), TR("Scientific"), TR("Text"), TR("Special"), TR("Custom"), TR("Timestamp (ns) [YC]")};
enum Cat { kGeneral = 0, kNumber, kCurrency, kAccounting, kDate, kTime, kPercent, kFraction, kScientific, kText, kSpecial, kCustom, kTimestamp };

// Currency / Accounting symbols (the UTF-8 bytes of yen, euro and pound)
// (then won, rupee, the other dollars, Swiss franc and krona)
const char* kSymbols[] = {TR("None"), "$", "NT$", "\xC2\xA5", "\xE2\x82\xAC", "\xC2\xA3", "\xE2\x82\xA9", "\xE2\x82\xB9", "HK$", "S$", "A$", "C$", "R$", "CHF", "kr"};
std::string SymbolCode(int sym) {
  if (sym <= 0 || sym >= static_cast<int>(std::size(kSymbols))) return "";
  if (sym == 1) return "$";
  return std::string("\"") + kSymbols[sym] + "\"";
}
int SymbolOf(const std::string& code) {
  for (int k = static_cast<int>(std::size(kSymbols)) - 1; k >= 2; --k) {
    if (code.find(kSymbols[k]) != std::string::npos) return k;
  }
  return code.find('$') != std::string::npos ? 1 : 0;
}
int DefaultSymbol() { return i18n::Current() == i18n::Lang::ZhTW ? 2 : 1; }  // as Excel in Taiwan

// Excel's four negative number formats: -1234.10, 1234.10 in red, (1234.10), (1234.10) in red
std::string WithNegative(const std::string& base, int n) {
  switch (n) {
    case 1: return base + ";[Red]" + base;
    case 2: return base + "_);(" + base + ")";
    case 3: return base + "_);[Red](" + base + ")";
    default: return base;
  }
}
// the positive part and the negative style of a Number / Currency code; false when it is neither
bool SplitNegative(const std::string& code, std::string* base, int* neg) {
  const std::string b = code.substr(0, std::min(code.find(';'), code.find("_)")));
  for (int n = 0; n < 4; ++n) {
    if (WithNegative(b, n) == code) {
      *base = b;
      *neg = n;
      return true;
    }
  }
  return false;
}

struct TypeItem {
  const char* code;
  const char* label;  // nullptr: the sample shows the type (dates, times)
};
std::vector<TypeItem> DateTypes() {
  std::vector<TypeItem> v = {{"yyyy/m/d", nullptr},       {"yyyy-mm-dd", nullptr},        {"m/d/yyyy", nullptr},           {"d/m/yyyy", nullptr},
                             {"m/d", nullptr},            {"m/d/yy", nullptr},            {"mm/dd/yy", nullptr},           {"d-mmm", nullptr},
                             {"d-mmm-yy", nullptr},       {"dd-mmm-yy", nullptr},         {"mmm-yy", nullptr},             {"mmmm-yy", nullptr},
                             {"mmmm d, yyyy", nullptr},   {"dddd, mmmm d, yyyy", nullptr}, {"yyyy/m/d h:mm", nullptr},     {"yyyy-mm-dd hh:mm:ss", nullptr},
                             {"m/d/yy h:mm AM/PM", nullptr}};
  // the Chinese date types Excel offers in Taiwan (the year / month / day characters as UTF-8 bytes)
  if (i18n::Current() == i18n::Lang::ZhTW) {
    v.insert(v.begin() + 2, {"yyyy\"\xE5\xB9\xB4\"m\"\xE6\x9C\x88\"d\"\xE6\x97\xA5\"", nullptr});
    v.insert(v.begin() + 3, {"m\"\xE6\x9C\x88\"d\"\xE6\x97\xA5\"", nullptr});
  }
  return v;
}
std::vector<TypeItem> TimeTypes() {
  return {{"h:mm", nullptr},     {"hh:mm:ss", nullptr}, {"h:mm AM/PM", nullptr},   {"h:mm:ss AM/PM", nullptr}, {"mm:ss", nullptr},
          {"mm:ss.0", nullptr},  {"[h]:mm:ss", nullptr}, {"hh:mm:ss.000", nullptr}, {"yyyy/m/d h:mm", nullptr}};
}
std::vector<TypeItem> FractionTypes() {
  return {{"# ?/?", TR("Up to one digit (1/4)")},     {"# ?\?/?\?", TR("Up to two digits (21/25)")}, {"# ?\?\?/?\?\?", TR("Up to three digits (312/943)")},
          {"# ?/2", TR("As halves (1/2)")},           {"# ?/4", TR("As quarters (2/4)")},        {"# ?/8", TR("As eighths (4/8)")},
          {"# ?\?/16", TR("As sixteenths (8/16)")},    {"# ?/10", TR("As tenths (3/10)")},        {"# ?\?/100", TR("As hundredths (30/100)")}};
}
std::vector<TypeItem> SpecialTypes() {
  return {{"00000", TR("Zip Code")}, {"00000-0000", TR("Zip Code + 4")}, {"[<=9999999]###-####;(###) ###-####", TR("Phone Number")},
          {"000-00-0000", TR("Social Security Number")}};
}
const std::vector<TypeItem>* TypesOf(int category) {
  static const auto date = DateTypes(), time = TimeTypes(), frac = FractionTypes(), special = SpecialTypes();
  switch (category) {
    case kDate: return &date;
    case kTime: return &time;
    case kFraction: return &frac;
    case kSpecial: return &special;
    default: return nullptr;
  }
}
bool InTypes(int category, const std::string& code) {
  if (const auto* v = TypesOf(category)) {
    for (const auto& it : *v) {
      if (code == it.code) return true;
    }
  }
  return false;
}

std::string BuildCode(const FormatDlg& f) {
  const std::string dec = f.decimals > 0 ? "." + std::string(static_cast<size_t>(f.decimals), '0') : "";
  switch (f.category) {
    case kGeneral: return "General";
    case kNumber: return WithNegative((f.thousands ? "#,##0" : "0") + dec, f.negative);
    case kCurrency: return WithNegative(SymbolCode(f.symbol) + "#,##0" + dec, f.negative);
    case kAccounting: {
      const std::string cur = SymbolCode(f.symbol);
      return "_(" + cur + "* #,##0" + dec + "_);_(" + cur + "* (#,##0" + dec + ");_(" + cur + "* \"-\"" + (f.decimals > 0 ? std::string(static_cast<size_t>(f.decimals), '?') : "") + "_);_(@_)";
    }
    case kDate:
    case kTime:
    case kFraction:
    case kSpecial: {
      if (!f.type_code.empty() && InTypes(f.category, f.type_code)) return f.type_code;
      return (*TypesOf(f.category))[0].code;
    }
    case kPercent: return "0" + dec + "%";
    case kScientific: return "0" + dec + "E+00";
    case kText: return "@";
    case kTimestamp: return "[epoch][local]yyyy-mm-dd hh:mm:ss" + (f.decimals > 0 ? "." + std::string(static_cast<size_t>(std::min(f.decimals, 9)), '0') : "");
    default: return f.custom;
  }
}

// the category a code looks like (GuessCategory checks the dialog can build it again)
int GuessKind(const std::string& code) {
  if (code.empty() || code == "General") return kGeneral;
  if (code.find("[epoch]") != std::string::npos) return kTimestamp;
  if (code == "@") return kText;
  for (int c : {kFraction, kSpecial, kDate, kTime}) {
    if (InTypes(c, code)) return c;
  }
  if (code.find('%') != std::string::npos) return kPercent;
  if (code.find("E+") != std::string::npos) return kScientific;
  if (code.rfind("_(", 0) == 0 && code.find("* #,##0") != std::string::npos) return kAccounting;
  std::string base;
  int neg = 0;
  if (SplitNegative(code, &base, &neg)) {
    if (base.find_first_not_of("0#,.") == std::string::npos) return kNumber;
    const size_t num = base.find("#,##0");
    if (num != std::string::npos && SymbolOf(base) > 0 && base.find_first_not_of("0.", num + 5) == std::string::npos) return kCurrency;
  }
  if (code.find('?') != std::string::npos && code.find('/') != std::string::npos) return kFraction;
  if (formula::IsDateFormat(code)) return (code.find('y') != std::string::npos || code.find('d') != std::string::npos) ? kDate : kTime;
  return kCustom;
}

// the Number tab's settings from a code read as `category` (decimals, separator, negatives, symbol, type)
void ReadFields(FormatDlg& f, const std::string& code, int category) {
  f.category = category;
  std::snprintf(f.custom, sizeof(f.custom), "%s", code.c_str());
  const size_t dot = code.find('.');
  if (dot != std::string::npos) {
    size_t k = dot + 1;
    while (k < code.size() && code[k] == '0') ++k;
    f.decimals = static_cast<int>(k - dot - 1);
  } else if (f.category != kGeneral) {
    f.decimals = 0;
  }
  f.thousands = code.find(',') != std::string::npos;
  std::string base;
  int neg = 0;
  if ((f.category == kNumber || f.category == kCurrency) && SplitNegative(code, &base, &neg)) f.negative = neg;
  f.symbol = (f.category == kCurrency || f.category == kAccounting) ? SymbolOf(code) : DefaultSymbol();
  if (TypesOf(f.category)) f.type_code = code;
}

// the category the Number tab opens on (Excel 16, measured): a code its settings can't build again opens on
// Custom, a date not in the Type list (dd/mm/yyyy) as a scaled number (#,##0.0,,), or the dialog would show
// another code and OK would drop the difference. A currency symbol with a locale ([$NT$-404]) keeps its category.
int GuessCategory(const std::string& code) {
  const int c = GuessKind(code);
  if (c == kGeneral || c == kText || c == kCustom || c == kTimestamp) return c;
  if ((c == kCurrency || c == kAccounting) && code.find("[$") != std::string::npos) return c;
  FormatDlg probe;
  ReadFields(probe, code, c);
  return BuildCode(probe) == code ? c : kCustom;
}

void ReadCode(FormatDlg& f, const std::string& code) { ReadFields(f, code, GuessCategory(code)); }

// the Border tab starts with what the selection has: the outline from the rim cells, inside lines from the cells
// inside, diagonals from every cell; an edge that differs between the cells is "mixed"
void InitBorders(FormatDlg& f, const model::Sheet& sh, const model::StyleTable& tbl, const Rect& sel) {
  auto edge = [&](uint32_t r, uint32_t c, int side) -> model::BorderSide {
    const model::BorderSide own = tbl.Get(sh.GetStyle(r, c)).border[side];
    if (own.style != model::LineStyle::None) return own;
    // the neighbour's opposite edge draws the same line
    if (side == model::kTop && r > 0) return tbl.Get(sh.GetStyle(r - 1, c)).border[model::kBottom];
    if (side == model::kBottom) return tbl.Get(sh.GetStyle(r + 1, c)).border[model::kTop];
    if (side == model::kLeft && c > 0) return tbl.Get(sh.GetStyle(r, c - 1)).border[model::kRight];
    if (side == model::kRight) return tbl.Get(sh.GetStyle(r, c + 1)).border[model::kLeft];
    return own;
  };
  for (int e = 0; e < 8; ++e) {
    f.init[e] = {};
    f.init_mixed[e] = false;
  }
  if (sel.Count() > 20000) {
    for (bool& m : f.init_mixed) m = true;
    return;
  }
  bool first[8] = {true, true, true, true, true, true, true, true};
  auto add = [&](int e, const model::BorderSide& b) {
    if (first[e]) {
      f.init[e] = b;
      first[e] = false;
    } else if (!(f.init[e] == b)) {
      f.init_mixed[e] = true;
    }
  };
  for (uint32_t c = sel.c0; c <= sel.c1; ++c) {
    add(0, edge(sel.r0, c, model::kTop));
    add(1, edge(sel.r1, c, model::kBottom));
  }
  for (uint32_t r = sel.r0; r <= sel.r1; ++r) {
    add(2, edge(r, sel.c0, model::kLeft));
    add(3, edge(r, sel.c1, model::kRight));
  }
  for (uint32_t r = sel.r0; r <= sel.r1; ++r) {
    for (uint32_t c = sel.c0; c <= sel.c1; ++c) {
      if (r < sel.r1) add(4, edge(r, c, model::kBottom));
      if (c < sel.c1) add(5, edge(r, c, model::kRight));
      const model::Style& st = tbl.Get(sh.GetStyle(r, c));
      add(6, st.border[model::kDiagDown]);
      add(7, st.border[model::kDiagUp]);
    }
  }
}

void InitFormatDialog(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  const auto& sel = s.FocusView().sel;
  const auto& tbl = *s.snap->wb.styles;
  g_fmt = FormatDlg{};
  if (g_fmt_for.on) {
    // Find Format / Replace Format: the format chosen so far (nothing = No Format Set)
    const engine::FormatSpec& sp = g_fmt_for.spec;
    g_fmt.base = model::Style{};
    g_fmt.edit = sp.style;
    g_fmt.edit.numfmt = g_fmt.edit.font_name = 0;
    g_fmt.base_numfmt = "General";
    g_fmt.numfmt = (sp.mask & engine::kFmNumber) && !sp.numfmt.empty() ? sp.numfmt : std::string("General");
    g_fmt.font_name = g_fmt.base_font = (sp.mask & engine::kFmFontName) && !sp.font.empty() ? sp.font : tbl.default_font;
    ReadCode(g_fmt, g_fmt.numfmt);
    g_fmt.opened_code = BuildCode(g_fmt);
    const int map[8] = {model::kTop, model::kBottom, model::kLeft, model::kRight, -1, -1, model::kDiagDown, model::kDiagUp};
    for (int e = 0; e < 8; ++e) g_fmt.init[e] = map[e] >= 0 ? sp.style.border[map[e]] : model::BorderSide{};
    g_fmt.init[4] = g_fmt_for.inner_h;
    g_fmt.init[5] = g_fmt_for.inner_v;
    if (!(g_fmt_for.tabs & 1u)) s.format_tab = 2;  // no Number tab: open on Font
    return;
  }
  g_fmt.base = g_fmt.edit = tbl.Get(sh->GetStyle(sel.act_r, sel.act_c));
  if (const model::Style* target = StyleFormatTarget()) {
    // Style > Format...: the style's formats
    g_fmt.base = g_fmt.edit = *target;
    g_fmt.numfmt = g_fmt.base_numfmt = tbl.NumberFormat(*target);
    // a number style (Comma, Currency...) keeps its code apart until it is applied: show that code
    if (!target->numfmt && !StyleFormatCode().empty()) g_fmt.numfmt = g_fmt.base_numfmt = StyleFormatCode();
    g_fmt.font_name = g_fmt.base_font = tbl.FontName(*target);
    ReadCode(g_fmt, g_fmt.numfmt);
    g_fmt.opened_code = BuildCode(g_fmt);
    const int map[8] = {model::kTop, model::kBottom, model::kLeft, model::kRight, -1, -1, model::kDiagDown, model::kDiagUp};
    for (int e = 0; e < 8; ++e) g_fmt.init[e] = map[e] >= 0 ? target->border[map[e]] : model::BorderSide{};
    return;
  }
  if (EditHasTextSelection(s)) {
    // the font of the first selected character
    const auto& e = s.edit;
    const model::RichText rt = model::RemapRuns(s.edit_rich, e.text);
    const size_t a = model::CharCount(std::string_view(e.text).substr(0, static_cast<size_t>(std::max(0, std::min(e.sel_start, e.sel_end)))));
    const model::RunFont f = model::ResolveRunFont(tbl, tbl.Get(sh->GetStyle(e.r, e.c)), model::RunAt(rt, a));
    g_fmt.chars = true;
    g_fmt.edit.bold = f.bold;
    g_fmt.edit.italic = f.italic;
    g_fmt.edit.strike = f.strike;
    g_fmt.edit.underline = f.underline;
    g_fmt.edit.font_color = f.color;
    g_fmt.edit.font_size = f.size;
    g_fmt.edit.script = f.script;
    g_fmt.base = g_fmt.edit;
    g_fmt.font_name = g_fmt.base_font = f.name;
    s.format_tab = 2;
    return;
  }
  g_fmt.numfmt = g_fmt.base_numfmt = tbl.NumberFormat(g_fmt.base);
  g_fmt.font_name = g_fmt.base_font = tbl.FontName(g_fmt.base);
  g_fmt.sample = sh->GetValue(sel.act_r, sel.act_c);
  ReadCode(g_fmt, g_fmt.numfmt);
  g_fmt.opened_code = BuildCode(g_fmt);
  if (s.format_category >= 0) {
    // opened on a category (More Accounting Formats): OK applies it
    g_fmt.category = s.format_category;
    if (g_fmt.category == kAccounting) g_fmt.decimals = 2;
    s.format_category = -1;
  }
  for (const Rect& mr : *sh->merges) g_fmt.merge = g_fmt.merge || mr.Contains(sel.act_r, sel.act_c);
  g_fmt.merge0 = g_fmt.merge;
  InitBorders(g_fmt, *sh, tbl, SelRect(s));
}

ImVec4 ArgbToVec(uint32_t c) {
  if (!c) return ImVec4(0, 0, 0, 0);
  c = model::ResolveColor(c);
  return ImVec4(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f);
}
uint32_t VecToArgb(const ImVec4& v) {
  if (v.w <= 0.0f) return 0;
  const uint32_t r = static_cast<uint32_t>(v.x * 255.0f + 0.5f), g = static_cast<uint32_t>(v.y * 255.0f + 0.5f), b = static_cast<uint32_t>(v.z * 255.0f + 0.5f);
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint32_t TintArgb(uint32_t argb, float tint) {
  auto ch = [&](int shift) {
    const float v = static_cast<float>((argb >> shift) & 0xFF);
    const float r = tint >= 0 ? v + (255 - v) * tint : v * (1 + tint);
    return static_cast<uint32_t>(std::clamp(r, 0.0f, 255.0f) + 0.5f) << shift;
  };
  return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// a color swatch that opens Excel's palette (ui::ribbon::PaletteSwatches: the workbook's theme colors with their
// shades, kept as theme colors so they follow Page Layout > Themes; the standard colors) and More Colors (a picker);
// allow_auto adds Automatic / No Color first
bool ColorEdit(const char* label, uint32_t* argb, bool allow_auto, const char* auto_label) {
  bool changed = false;
  ImGui::PushID(label);
  const ImVec4 v = *argb ? ArgbToVec(*argb) : ImVec4(1, 1, 1, 0);
  if (ImGui::ColorButton("##swatch", v, ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(S(44), S(20)))) ImGui::OpenPopup("##palette");
  if (label[0] && !(label[0] == '#' && label[1] == '#')) {
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
  }
  if (*argb == 0 && allow_auto) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", auto_label);
  }
  if (ImGui::BeginPopup("##palette")) {
    uint32_t ref = 0;
    if (ui::ribbon::PaletteSwatches(&ref, allow_auto ? auto_label : nullptr)) {
      *argb = ref;
      changed = true;
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::TreeNode(TR("More Colors..."))) {
      ImVec4 vv = *argb ? ArgbToVec(*argb) : ImVec4(1, 1, 1, 1);
      ImGui::SetNextItemWidth(S(200));
      if (ImGui::ColorPicker3("##more", &vv.x, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex)) {
        vv.w = 1;
        *argb = VecToArgb(vv);
        changed = true;
      }
      ImGui::TreePop();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

// how a line style looks (the Border tab's samples and preview)
void DrawLineSample(ImDrawList* dl, ImVec2 a, ImVec2 b, model::LineStyle st, ImU32 col) {
  using L = model::LineStyle;
  const bool hz = a.y == b.y;
  const float len = hz ? b.x - a.x : b.y - a.y;
  const float w = st == L::Medium || st == L::MediumDashed || st == L::MediumDashDot || st == L::MediumDashDotDot || st == L::SlantDashDot ? S(2) : st == L::Thick ? S(3) : S(1);
  auto seg = [&](float s0, float s1, float off) {
    const ImVec2 p0 = hz ? ImVec2(a.x + s0, a.y + off) : ImVec2(a.x + off, a.y + s0);
    const ImVec2 p1 = hz ? ImVec2(a.x + s1, a.y + off) : ImVec2(a.x + off, a.y + s1);
    dl->AddLine(p0, p1, col, w);
  };
  if (st == L::Double) {
    seg(0, len, -S(1.5f));
    seg(0, len, S(1.5f));
    return;
  }
  std::vector<float> pat;  // dash, gap, dash, gap...
  switch (st) {
    case L::Dashed:
    case L::MediumDashed: pat = {S(6), S(3)}; break;
    case L::Dotted: pat = {S(1), S(2)}; break;
    case L::Hair: pat = {S(1), S(1)}; break;
    case L::DashDot:
    case L::MediumDashDot:
    case L::SlantDashDot: pat = {S(7), S(2), S(2), S(2)}; break;
    case L::DashDotDot:
    case L::MediumDashDotDot: pat = {S(7), S(2), S(2), S(2), S(2), S(2)}; break;
    default: break;
  }
  if (pat.empty()) {
    seg(0, len, 0);
    return;
  }
  float x = 0;
  for (size_t k = 0; x < len; k = (k + 1) % pat.size()) {
    const float e = std::min(len, x + pat[k]);
    if (k % 2 == 0) seg(x, e, 0);
    x = e;
  }
}

// the line style and color chosen in the Border tab (automatic black is stored as 0)
model::BorderSide CurrentSide(const FormatDlg& f) {
  const uint32_t c = VecToArgb(f.line_color);
  return model::BorderSide{f.line, c == 0xFF000000u ? 0u : c};
}
// what an edge of the Border tab's preview shows: set, cleared, or what the selection has
model::BorderSide ShownSide(const FormatDlg& f, int e, bool* mixed = nullptr) {
  if (mixed) *mixed = f.edges[e] == 0 && f.init_mixed[e];
  if (f.edges[e] == 1) return f.side[e];
  if (f.edges[e] == 2) return {};
  return f.init_mixed[e] ? model::BorderSide{} : f.init[e];
}
// Excel's click on an edge: the current line goes on it, unless it already has exactly that line (then it comes off)
void ClickEdge(FormatDlg& f, int e) {
  const model::BorderSide cur = CurrentSide(f);
  bool mixed = false;
  const model::BorderSide now = ShownSide(f, e, &mixed);
  if (cur.style == model::LineStyle::None || (!mixed && now == cur)) {
    f.edges[e] = 2;
  } else {
    f.edges[e] = 1;
    f.side[e] = cur;
  }
}
void SetEdge(FormatDlg& f, int e) {
  const model::BorderSide cur = CurrentSide(f);
  if (cur.style == model::LineStyle::None) {
    f.edges[e] = 2;
    return;
  }
  f.edges[e] = 1;
  f.side[e] = cur;
}

// the Border tab, as Excel's: Presets (None, Outline, Inside), the line style and color, and the preview whose edges
// take the current line when clicked (inside lines when several cells are selected); each edge keeps its own line
void DrawBorderTab(AppState& s, FormatDlg& f) {
  const Rect sel = StyleFormatTarget() || (g_fmt_for.on && !g_fmt_for.inner) ? Rect{0, 0, 0, 0} : g_fmt_for.on ? Rect{0, 0, 1, 1} : SelRect(s);
  const bool multi = !sel.IsCell();
  const bool tall = sel.r1 > sel.r0, wide = sel.c1 > sel.c0;
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Line"));
  ImGui::TextDisabled("%s", TR("Style:"));
  using L = model::LineStyle;
  static const L kStyles[] = {L::None, L::Hair, L::Dotted, L::DashDotDot, L::DashDot, L::Dashed, L::Thin, L::MediumDashDotDot, L::SlantDashDot, L::MediumDashDot, L::MediumDashed, L::Medium, L::Thick, L::Double};
  static const char* kStyleNames[] = {"None", "Hair", "Dotted", "Dash Dot Dot", "Dash Dot", "Dashed", "Thin", "Medium Dash Dot Dot", "Slanted Dash Dot", "Medium Dash Dot", "Medium Dashed", "Medium", "Thick", "Double"};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (size_t k = 0; k < std::size(kStyles); ++k) {
    ImGui::PushID(static_cast<int>(k));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##ls", f.line == kStyles[k], 0, ImVec2(S(110), S(16)))) f.line = kStyles[k];
    // tests find the samples by name ("Line style: Thick")
    ui::NoteUiItem((std::string("Line style: ") + kStyleNames[k]).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    if (kStyles[k] == L::None) dl->AddText(ImVec2(p.x + S(4), p.y), ImGui::GetColorU32(ImGuiCol_Text), TR("None"));
    else DrawLineSample(dl, ImVec2(p.x + S(6), p.y + S(8)), ImVec2(p.x + S(104), p.y + S(8)), kStyles[k], IM_COL32_BLACK);
    ImGui::PopID();
  }
  ImGui::Spacing();
  ImGui::TextDisabled("%s", TR("Color:"));
  uint32_t lc = VecToArgb(f.line_color);
  if (ColorEdit("##linecolor", &lc, true, TR("Automatic"))) f.line_color = lc ? ArgbToVec(lc) : ImVec4(0, 0, 0, 1);
  ImGui::EndGroup();
  ImGui::SameLine(0, S(24));
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Presets"));
  if (ImGui::Button(TR("None"), ImVec2(S(80), 0))) {
    for (int e = 0; e < 8; ++e) f.edges[e] = 2;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Outline"), ImVec2(S(80), 0))) {
    for (int e = 0; e < 4; ++e) SetEdge(f, e);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!multi);
  if (ImGui::Button(TR("Inside"), ImVec2(S(80), 0))) {
    if (tall) SetEdge(f, 4);
    if (wide) SetEdge(f, 5);
  }
  ImGui::EndDisabled();
  ImGui::Spacing();
  ImGui::TextUnformatted(TR("Border"));
  // the preview: two by two cells when several are selected; click near an edge to put the current line on it
  const ImVec2 p0 = ImGui::GetCursorScreenPos() + ImVec2(S(20), S(10));
  const float bw = S(220), bh = S(150);
  const ImRect box(p0, p0 + ImVec2(bw, bh));
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##preview", ImVec2(bw, bh));
  const bool preview_clicked = ImGui::IsItemClicked();
  dl->AddRectFilled(box.Min, box.Max, IM_COL32_WHITE);
  // corner marks of the cells
  const ImU32 mark = IM_COL32(0x90, 0x90, 0x90, 255);
  const float mx = box.GetCenter().x, my = box.GetCenter().y, m = S(6);
  for (ImVec2 c : {box.Min, ImVec2(box.Max.x, box.Min.y), box.Max, ImVec2(box.Min.x, box.Max.y)}) {
    dl->AddLine(ImVec2(c.x - m, c.y), ImVec2(c.x + m, c.y), mark);
    dl->AddLine(ImVec2(c.x, c.y - m), ImVec2(c.x, c.y + m), mark);
  }
  auto text = [&](float x, float y) { dl->AddText(ImVec2(x - S(12), y - ImGui::GetFontSize() * 0.5f), IM_COL32(0x60, 0x60, 0x60, 255), TR("Text")); };
  if (multi) {
    text(box.Min.x + bw * 0.25f, box.Min.y + bh * 0.25f);
    text(box.Min.x + bw * 0.75f, box.Min.y + bh * 0.25f);
    text(box.Min.x + bw * 0.25f, box.Min.y + bh * 0.75f);
    text(box.Min.x + bw * 0.75f, box.Min.y + bh * 0.75f);
  } else {
    text(mx, my);
  }
  // every edge in its own line style and color; edges that differ between the cells in gray
  auto draw = [&](int e, ImVec2 a, ImVec2 b) {
    bool mixed = false;
    const model::BorderSide bs = ShownSide(f, e, &mixed);
    if (mixed) {
      dl->AddLine(a, b, IM_COL32(0xA0, 0xA0, 0xA0, 255), S(2));
      return;
    }
    if (bs.style == L::None) return;
    const ImU32 col = bs.color ? ImGui::ColorConvertFloat4ToU32(ArgbToVec(bs.color)) : IM_COL32_BLACK;
    if (a.x == b.x || a.y == b.y) {
      DrawLineSample(dl, a, b, bs.style, col);
      return;
    }
    const bool medium = bs.style == L::Medium || bs.style == L::MediumDashed || bs.style == L::MediumDashDot || bs.style == L::MediumDashDotDot || bs.style == L::SlantDashDot;
    if (bs.style == L::Double) {
      dl->AddLine(a + ImVec2(S(2), 0), b + ImVec2(S(2), 0), col, S(1));
      dl->AddLine(a - ImVec2(S(2), 0), b - ImVec2(S(2), 0), col, S(1));
    } else {
      dl->AddLine(a, b, col, bs.style == L::Thick ? S(3) : medium ? S(2) : S(1));
    }
  };
  draw(0, box.Min, ImVec2(box.Max.x, box.Min.y));
  draw(1, ImVec2(box.Min.x, box.Max.y), box.Max);
  draw(2, box.Min, ImVec2(box.Min.x, box.Max.y));
  draw(3, ImVec2(box.Max.x, box.Min.y), box.Max);
  if (multi && tall) draw(4, ImVec2(box.Min.x, my), ImVec2(box.Max.x, my));
  if (multi && wide) draw(5, ImVec2(mx, box.Min.y), ImVec2(mx, box.Max.y));
  draw(6, box.Min, box.Max);
  draw(7, ImVec2(box.Min.x, box.Max.y), ImVec2(box.Max.x, box.Min.y));
  if (preview_clicked) {
    // the edge nearest the click (within a few pixels)
    const ImVec2 q = ImGui::GetIO().MousePos;
    const float tol = S(10);
    int e = -1;
    if (std::fabs(q.y - box.Min.y) < tol) e = 0;
    else if (std::fabs(q.y - box.Max.y) < tol) e = 1;
    else if (std::fabs(q.x - box.Min.x) < tol) e = 2;
    else if (std::fabs(q.x - box.Max.x) < tol) e = 3;
    else if (multi && tall && std::fabs(q.y - my) < tol) e = 4;
    else if (multi && wide && std::fabs(q.x - mx) < tol) e = 5;
    if (e >= 0) ClickEdge(f, e);
  }
  // the buttons around the preview, as in Excel: checked while the edge has a line; a click works as on the preview
  ImGui::SetCursorScreenPos(ImVec2(box.Min.x - S(20), box.Max.y + S(12)));
  const char* names[] = {TR("Top"), TR("Bottom"), TR("Left"), TR("Right"), TR("Inside Horizontal"), TR("Inside Vertical"), TR("Diagonal Down"), TR("Diagonal Up")};
  for (int e = 0; e < 8; ++e) {
    if ((e == 4 && !(multi && tall)) || (e == 5 && !(multi && wide))) continue;
    if (e % 4 && ImGui::GetContentRegionAvail().x > S(90)) ImGui::SameLine();
    bool mixed = false;
    bool v = ShownSide(f, e, &mixed).style != L::None;
    if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::Checkbox(names[e], &v)) ClickEdge(f, e);
    if (mixed) ImGui::PopItemFlag();
  }
  ImGui::EndGroup();
  ImGui::TextDisabled("%s", TR("Click a line style and a color, then click the preview's edges or the presets."));
}

}  // namespace

void OpenFormatCellsFor(AppState& s, const char* title, const engine::FormatSpec& start, unsigned tabs, model::BorderSide inner_h, model::BorderSide inner_v,
                        std::function<void(const engine::FormatSpec&, model::BorderSide, model::BorderSide)> done) {
  g_fmt_for = FormatFor{};
  g_fmt_for.on = true;
  g_fmt_for.title = title;
  g_fmt_for.spec = start;
  g_fmt_for.find = false;
  g_fmt_for.tabs = tabs;
  g_fmt_for.inner = true;
  g_fmt_for.inner_h = inner_h;
  g_fmt_for.inner_v = inner_v;
  g_fmt_for.done = [done](const engine::FormatSpec& out) {
    if (done) done(out, g_fmt_for.inner_h, g_fmt_for.inner_v);
  };
  s.OpenDialog(Dialog::FormatCells);
}

namespace {

void DrawFormatCells(AppState& s) {
  if (g_fmt_return && s.dialog == Dialog::None) {
    g_fmt_return = false;
    g_fmt_keep = true;
    s.OpenDialog(Dialog::FormatCells);
  } else if (g_fmt_return && s.dialog != Dialog::Message) {
    g_fmt_return = false;  // something else opened meanwhile
  }
  if (s.dialog == Dialog::FormatCells && s.dialog_open_request && g_fmt_keep) {
    g_fmt_keep = false;
  } else if (s.dialog == Dialog::FormatCells && s.dialog_open_request) {
    InitFormatDialog(s);
  }
  if (!BeginDialog(s, g_fmt_for.on ? g_fmt_for.title : TR("Format Cells"), Dialog::FormatCells, ImVec2(S(640), S(520)))) {
    if (StyleFormatTarget() && s.dialog != Dialog::FormatCells) StyleFormatDone(s, false);  // closed with Esc / x
    if (g_fmt_for.on && s.dialog != Dialog::FormatCells) g_fmt_for.on = false;
    return;
  }
  FormatDlg& f = g_fmt;
  bool apply = false, close = false;
  if (BeginDlgTabBar("##fmt_tabs")) {
    const char* tabs[] = {TR("Number"), TR("Alignment"), TR("Font"), TR("Border"), TR("Fill"), TR("Protection")};
    for (int i = 0; i < 6; ++i) {
      if (f.chars && i != 2) continue;  // selected characters: only their font
      if (g_fmt_for.on && !(g_fmt_for.tabs & (1u << i))) continue;  // a table style's element: Font, Border and Fill
      ImGuiTabItemFlags fl = (s.format_tab == i && ImGui::IsWindowAppearing()) ? ImGuiTabItemFlags_SetSelected : 0;
      if (!ImGui::BeginTabItem(tabs[i], nullptr, fl)) continue;
      if (!f.chars && !ImGui::IsWindowAppearing()) s.format_tab = i;  // Excel reopens Format Cells on the last tab used
      ImGui::BeginChild("##page", ImVec2(0, S(400)));
      switch (i) {
        case 0: {
          // the arrows and a letter choose the category, as in Excel
          if (DlgListKeys(&f.category, static_cast<int>(std::size(kCategories)), [](int k) { return kCategories[k]; }) && f.category == kTimestamp)
            f.decimals = 9;
          ImGui::BeginChild("##cats", ImVec2(S(170), 0), ImGuiChildFlags_Borders);
          ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
          for (int k = 0; k < static_cast<int>(std::size(kCategories)); ++k) {
            if (ImGui::Selectable(kCategories[k], f.category == k)) {
              f.category = k;
              if (k == kTimestamp) f.decimals = 9;
            }
          }
          ImGui::PopItemFlag();
          ImGui::EndChild();
          ImGui::SameLine();
          ImGui::BeginGroup();
          ImGui::TextUnformatted(TR("Sample"));
          const std::string code = BuildCode(f);
          const auto shown = formula::FormatValue(f.sample, code);
          ImGui::BeginChild("##sample", ImVec2(0, S(36)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
          if (shown.color) ImGui::TextColored(ArgbToVec(shown.color), "%s", shown.text.c_str());
          else ImGui::TextUnformatted(shown.text.c_str());
          ImGui::EndChild();
          // Excel's spin box for the decimal places
          auto decimals = [&](const char* label, int max) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(S(170));
            ImGui::SetNextItemWidth(S(110));
            if (ImGui::InputInt("##decimals", &f.decimals, 1, 1)) f.decimals = std::clamp(f.decimals, 0, max);
          };
          auto symbol = [&] {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("Symbol:"));
            ImGui::SameLine(S(170));
            ImGui::SetNextItemWidth(S(110));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ui::NoteUiItem("##symbol", ImRect(at, at + ImVec2(S(110), ImGui::GetFrameHeight())));
            if (ui::BeginCombo("##symbol", kSymbols[std::clamp(f.symbol, 0, static_cast<int>(std::size(kSymbols)) - 1)])) {
              for (int k = 0; k < static_cast<int>(std::size(kSymbols)); ++k) {
                if (ImGui::Selectable(kSymbols[k], f.symbol == k)) f.symbol = k;
              }
              ImGui::EndCombo();
            }
          };
          // a list of choices shown as they look (the negative numbers in red, dates as the cell's date)
          auto pick_list = [&](const char* title, int count, const std::function<std::string(int)>& code_of, const std::function<std::string(int)>& label_of,
                               const std::function<bool(int)>& selected, const std::function<void(int)>& choose) {
            ImGui::TextUnformatted(title);
            ImGui::BeginChild(title, ImVec2(0, std::max(S(120), ImGui::GetContentRegionAvail().y - S(8))), ImGuiChildFlags_Borders);
            for (int k = 0; k < count; ++k) {
              const std::string c = code_of(k);
              const std::string label = label_of(k);
              uint32_t color = 0;
              std::string text = label;
              if (text.empty()) {
                const auto r = formula::FormatValue(f.sample.IsNumeric() ? f.sample : core::Value::Number(40981.5631), c);
                text = r.text;
              } else if (label[0] == '\x01') {  // a negative number sample: shown in its color
                const auto r = formula::FormatValue(core::Value::Number(-1234.1), c);
                text = r.text;
                color = r.color;
              }
              ImGui::PushID(k);
              if (color) ImGui::PushStyleColor(ImGuiCol_Text, ArgbToVec(color));
              if (ImGui::Selectable((text + "##" + c).c_str(), selected(k))) choose(k);
              if (color) ImGui::PopStyleColor();
              if (ImGui::IsItemHovered() && label.empty()) ImGui::SetTooltip("%s", c.c_str());
              ImGui::PopID();
            }
            ImGui::EndChild();
          };
          auto negatives = [&](const std::string& base) {
            pick_list(TR("Negative numbers:"), 4, [&](int k) { return WithNegative(base, k); }, [](int) { return std::string("\x01"); },
                      [&](int k) { return f.negative == k; }, [&](int k) { f.negative = k; });
          };
          const std::string dec = f.decimals > 0 ? "." + std::string(static_cast<size_t>(f.decimals), '0') : "";
          switch (f.category) {
            case kNumber:
              decimals(TR("Decimal places:"), 30);
              ImGui::Checkbox(TR("Use 1000 Separator (,)"), &f.thousands);
              negatives((f.thousands ? "#,##0" : "0") + dec);
              break;
            case kCurrency:
              decimals(TR("Decimal places:"), 30);
              symbol();
              negatives(SymbolCode(f.symbol) + "#,##0" + dec);
              break;
            case kAccounting:
              decimals(TR("Decimal places:"), 30);
              symbol();
              break;
            case kPercent:
            case kScientific: decimals(TR("Decimal places:"), 30); break;
            case kTimestamp: decimals(TR("Fractional seconds digits:"), 9); break;
            case kDate:
            case kTime:
            case kFraction:
            case kSpecial: {
              const auto& types = *TypesOf(f.category);
              const std::string cur = BuildCode(f);
              pick_list(TR("Type:"), static_cast<int>(types.size()), [&](int k) { return std::string(types[static_cast<size_t>(k)].code); },
                        [&](int k) { return std::string(types[static_cast<size_t>(k)].label ? types[static_cast<size_t>(k)].label : ""); },
                        [&](int k) { return cur == types[static_cast<size_t>(k)].code; }, [&](int k) {
                          f.type_code = types[static_cast<size_t>(k)].code;
                          f.type_picked = true;
                        });
              break;
            }
            case kCustom: {
              ImGui::TextUnformatted(TR("Type:"));
              ImGui::SetNextItemWidth(-1);
              ImGui::InputText("##custom", f.custom, sizeof(f.custom));
              // Excel's list: the common codes, then every code the workbook uses
              static const char* kPresets[] = {"General",    "0",          "0.00",          "#,##0",     "#,##0.00",  "#,##0_);(#,##0)", "#,##0.00_);[Red](#,##0.00)",
                                               "0%",         "0.00%",      "0.00E+00",      "##0.0E+0",  "# ?/?",     "# ?\?/?\?",       "yyyy/m/d",
                                               "yyyy-mm-dd", "d-mmm-yy",   "mmm-yy",        "h:mm",      "hh:mm:ss",  "[h]:mm:ss",       "mm:ss.0",
                                               "yyyy/m/d h:mm", "@",       "[epoch][utc]yyyy-mm-dd hh:mm:ss.000000000", "[epoch][tz=+08:00]hh:mm:ss.000000"};
              std::vector<std::string> list(std::begin(kPresets), std::end(kPresets));
              const size_t presets = list.size();
              static std::vector<std::string> deleted;  // codes deleted with Delete (the style table keeps old entries)
              if (s.snap) {
                const auto& tbl = *s.snap->wb.styles;
                for (size_t k = 0; k < tbl.size() && list.size() < 400; ++k) {
                  const std::string& c = tbl.NumberFormat(tbl.Get(static_cast<uint32_t>(k)));
                  if (std::find(list.begin(), list.end(), c) == list.end() && std::find(deleted.begin(), deleted.end(), c) == deleted.end()) list.push_back(c);
                }
              }
              // Excel's Delete: a code of the workbook's own (not a built-in one) goes; its cells become General
              {
                const auto at = std::find(list.begin(), list.end(), std::string(f.custom));
                const bool own = at != list.end() && static_cast<size_t>(at - list.begin()) >= presets;
                ImGui::BeginDisabled(!own || StyleFormatTarget() || g_fmt_for.on);
                if (ImGui::Button(TR("Delete")) && own && s.engine) {
                  const std::string gone = f.custom;
                  deleted.push_back(gone);
                  s.engine->PostUser(TR("Delete Number Format"), [gone](Document& d) {
                    const uint32_t id = d.wb_mut().styles.mut().InternString(gone);
                    std::vector<std::pair<uint32_t, Rect>> used;
                    for (const auto& sh : d.wb().sheets) {
                      if (sh->has_used) used.emplace_back(sh->id, sh->used);
                    }
                    for (const auto& [sid, area] : used) {
                      d.ModifyStyle(sid, area, [id](model::Style& x) {
                        if (x.numfmt == id) x.numfmt = 0;
                      });
                    }
                  });
                  std::snprintf(f.custom, sizeof(f.custom), "%s", "General");
                }
                ImGui::EndDisabled();
              }
              ImGui::BeginChild("##presets", ImVec2(0, S(200)), ImGuiChildFlags_Borders);
              for (size_t k = 0; k < list.size(); ++k) {
                // the code drawn as text: a label would be cut at its "##" (#,##0)
                const std::string& c = list[k];
                ImGui::PushID(static_cast<int>(k));
                if (ImGui::Selectable("##code", c == f.custom)) std::snprintf(f.custom, sizeof(f.custom), "%s", c.c_str());
                ui::NoteUiText(c.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));  // ui.items finds it by its text
                ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x);
                ImGui::TextUnformatted(c.c_str());
                ImGui::PopID();
              }
              ImGui::EndChild();
              break;
            }
            default: break;
          }
          if (f.category == kTimestamp) ImGui::TextWrapped(TR("[YC] The value is Unix epoch seconds (Decimal keeps 10 integer + 9 fractional digits), shown in the local time zone down to nanoseconds."));
          // Excel's line about each category
          {
            const char* about = nullptr;
            switch (f.category) {
              case kGeneral: about = TR("General format cells have no specific number format."); break;
              case kNumber: about = TR("Number is used for general display of numbers. Currency and Accounting offer specialized formatting for monetary value."); break;
              case kCurrency: about = TR("Currency formats are used for general monetary values. Use Accounting formats to align decimal points in a column."); break;
              case kAccounting: about = TR("Accounting formats line up the currency symbols and decimal points in a column."); break;
              case kDate: about = TR("Date formats display date and time serial numbers as date values."); break;
              case kTime: about = TR("Time formats display date and time serial numbers as time values."); break;
              case kPercent: about = TR("Percentage formats multiply the cell value by 100 and display the result with a percent symbol."); break;
              case kText: about = TR("Text format cells are treated as text even when a number is in the cell. The cell is displayed exactly as entered."); break;
              case kSpecial: about = TR("Special formats are useful for tracking list and database values."); break;
              case kCustom: about = TR("Type the number format code, using one of the existing codes as a starting point."); break;
              default: break;
            }
            if (about) {
              ImGui::Spacing();
              ImGui::TextWrapped("%s", about);
            }
          }
          ImGui::EndGroup();
          break;
        }
        case 1: {
          // Excel's page: Text alignment, Text control and Right-to-left on the left; Orientation (the vertical Text
          // box, the dial and the Degrees box) on the right
          using H = model::HAlign;
          const float lw = S(96);
          ImGui::BeginGroup();
          ImGui::SeparatorText(TR("Text alignment"));
          const char* h[] = {TR("General"), TR("Left (Indent)"), TR("Center"), TR("Right (Indent)"), TR("Fill"), TR("Justify"), TR("Center Across Selection"), TR("Distributed (Indent)")};
          int hi = static_cast<int>(f.edit.halign);
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Horizontal:"));
          ImGui::SameLine(lw);
          ImGui::SetNextItemWidth(S(190));
          {
            const ImVec2 at = ImGui::GetCursorScreenPos();  // combos aren't recorded: tests find it by its id
            ui::NoteUiItem("##halign", ImRect(at, at + ImVec2(S(190), ImGui::GetFrameHeight())));
          }
          if (ui::Combo("##halign", &hi, h, IM_ARRAYSIZE(h))) {
            f.edit.halign = static_cast<H>(hi);
            if (f.edit.halign != H::General && f.edit.halign != H::Left && f.edit.halign != H::Right && f.edit.halign != H::Distributed) f.edit.indent = 0;
            if (f.edit.halign != H::Distributed) f.edit.justify_last_line = false;
          }
          // Indent: for General, Left, Right and Distributed; an indent on General text makes it Left (Indent)
          {
            const bool can_indent = f.edit.halign == H::General || f.edit.halign == H::Left || f.edit.halign == H::Right || f.edit.halign == H::Distributed;
            int ind = f.edit.indent;
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_indent);
            ImGui::TextUnformatted(TR("Indent:"));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(S(80));
            if (ImGui::InputInt("##indent", &ind)) {
              f.edit.indent = static_cast<uint8_t>(std::clamp(ind, 0, 15));
              if (f.edit.indent && f.edit.halign == H::General) f.edit.halign = H::Left;
            }
            ImGui::EndDisabled();
          }
          // Excel's order: Top, Center, Bottom, Justify, Distributed
          const char* v[] = {TR("Top##ctx_align"), TR("Center"), TR("Bottom##ctx_align"), TR("Justify"), TR("Distributed")};
          static const model::VAlign kVOrder[] = {model::VAlign::Top, model::VAlign::Center, model::VAlign::Bottom, model::VAlign::Justify, model::VAlign::Distributed};
          int vi = 0;
          for (int k = 0; k < 5; ++k) {
            if (kVOrder[k] == f.edit.valign) vi = k;
          }
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Vertical:"));
          ImGui::SameLine(lw);
          ImGui::SetNextItemWidth(S(190));
          {
            const ImVec2 at = ImGui::GetCursorScreenPos();  // combos aren't recorded: tests find it by its id
            ui::NoteUiItem("##valign", ImRect(at, at + ImVec2(S(190), ImGui::GetFrameHeight())));
          }
          if (ui::Combo("##valign", &vi, v, IM_ARRAYSIZE(v))) f.edit.valign = kVOrder[vi];
          // Justify distributed: only for Distributed (Indent)
          ImGui::BeginDisabled(f.edit.halign != H::Distributed);
          ImGui::Checkbox(TR("Justify distributed"), &f.edit.justify_last_line);
          ImGui::EndDisabled();
          ImGui::SeparatorText(TR("Text control"));
          if (ImGui::Checkbox(TR("Wrap text"), &f.edit.wrap) && f.edit.wrap) f.edit.shrink = false;
          ImGui::BeginDisabled(f.edit.wrap);  // Excel: Shrink to fit can't go with Wrap text
          ImGui::Checkbox(TR("Shrink to fit"), &f.edit.shrink);
          ImGui::EndDisabled();
          ImGui::Checkbox(TR("Merge cells"), &f.merge);
          ImGui::SeparatorText(TR("Right-to-left"));
          {
            const char* dirs[] = {TR("Context"), TR("Left-to-Right"), TR("Right-to-Left")};
            const int dir = std::clamp<int>(f.edit.reading_order, 0, 2);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("Text direction:"));
            ImGui::SameLine(lw + S(20));
            ImGui::SetNextItemWidth(S(170));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ui::NoteUiItem("##text_direction", ImRect(at, at + ImVec2(S(170), ImGui::GetFrameHeight())));
            if (ui::BeginCombo("##text_direction", dirs[dir])) {
              for (int k = 0; k < 3; ++k) {
                if (ImGui::Selectable(dirs[k], dir == k)) f.edit.reading_order = static_cast<uint8_t>(k);
              }
              ImGui::EndCombo();
            }
          }
          ImGui::EndGroup();
          ImGui::SameLine(0, S(24));
          ImGui::BeginGroup();
          ImGui::TextUnformatted(TR("Orientation"));
          {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float bh = S(150), tw = S(30), dw = S(120);
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const bool vertical = f.edit.rotation == 255;
            const ImU32 frame = IM_COL32(0xAB, 0xAB, 0xAB, 255), ink = ImGui::GetColorU32(ImGuiCol_Text);
            constexpr float kPi = 3.14159265f;
            // the vertical Text box: a click stacks the letters (Vertical Text); a second click turns it off
            const ImRect tr(o, ImVec2(o.x + tw, o.y + bh));
            ImGui::SetCursorScreenPos(tr.Min);
            if (ImGui::InvisibleButton("##orient_vertical", tr.GetSize())) f.edit.rotation = vertical ? 0 : 255;
            ui::NoteUiItem("##orient_vertical", tr);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Vertical Text"));
            dl->AddRectFilled(tr.Min, tr.Max, vertical ? IM_COL32(0x20, 0x20, 0x20, 255) : IM_COL32_WHITE);
            dl->AddRect(tr.Min, tr.Max, frame);
            {
              const char* word = TR("Text");
              float y = tr.Min.y + S(12);
              for (const char* p = word; *p;) {
                const char* q = p + 1;
                while (*q && (static_cast<unsigned char>(*q) & 0xC0) == 0x80) ++q;  // one UTF-8 character
                const ImVec2 sz = ImGui::CalcTextSize(p, q);
                dl->AddText(ImVec2(tr.GetCenter().x - sz.x * 0.5f, y), vertical ? IM_COL32_WHITE : IM_COL32(0x20, 0x20, 0x20, 255), p, q);
                y += sz.y;
                p = q;
              }
            }
            // the dial: a half circle with its center at the left, -90 at the bottom and 90 at the top, a point every
            // 15 degrees; a click or a drag points the text
            const ImRect dr(ImVec2(tr.Max.x + S(8), o.y), ImVec2(tr.Max.x + S(8) + dw, o.y + bh));
            ImGui::SetCursorScreenPos(dr.Min);
            ImGui::InvisibleButton("##orient_dial", dr.GetSize());
            ui::NoteUiItem("##orient_dial", dr);
            const ImVec2 c(dr.Min.x + S(6), dr.GetCenter().y);
            const float rad = std::min(dr.GetWidth() - S(14), bh * 0.5f - S(8));
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
              const ImVec2 m = ImGui::GetIO().MousePos;
              const float deg = std::atan2(c.y - m.y, std::max(0.0f, m.x - c.x)) * 180.0f / kPi;
              f.edit.rotation = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(deg)), -90, 90));
            }
            dl->AddRectFilled(dr.Min, dr.Max, IM_COL32_WHITE);
            dl->AddRect(dr.Min, dr.Max, frame);
            dl->PathArcTo(c, rad, -kPi * 0.5f, kPi * 0.5f, 48);
            dl->PathStroke(IM_COL32(0xC8, 0xC8, 0xC8, 255), S(1), 0);
            const int angle = vertical ? 0 : f.edit.rotation;
            for (int k = -90; k <= 90; k += 15) {
              const float t = static_cast<float>(k) * kPi / 180.0f;
              const ImVec2 p(c.x + std::cos(t) * rad, c.y - std::sin(t) * rad);
              dl->AddCircleFilled(p, k % 45 == 0 ? S(2.5f) : S(1.5f), IM_COL32(0x40, 0x40, 0x40, 255));
            }
            {
              const float t = static_cast<float>(angle) * kPi / 180.0f;
              const ImVec2 dir(std::cos(t), -std::sin(t));
              // "Text" along the pointer (its vertices turned about the center), then a line to the red point
              const char* word = TR("Text");
              const ImVec2 sz = ImGui::CalcTextSize(word);
              const int v0 = dl->VtxBuffer.Size;
              dl->AddText(ImVec2(c.x + S(8), c.y - sz.y * 0.5f), vertical ? IM_COL32(0xA0, 0xA0, 0xA0, 255) : IM_COL32(0x20, 0x20, 0x20, 255), word);
              const float cs = std::cos(-t), sn = std::sin(-t);
              for (int vx = v0; vx < dl->VtxBuffer.Size; ++vx) {
                ImVec2& q = dl->VtxBuffer[vx].pos;
                const float x = q.x - c.x, y = q.y - c.y;
                q = ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs);
              }
              const float from = S(12) + sz.x;
              if (!vertical) {
                dl->AddLine(ImVec2(c.x + dir.x * from, c.y + dir.y * from), ImVec2(c.x + dir.x * rad, c.y + dir.y * rad), IM_COL32(0x40, 0x40, 0x40, 255), S(1));
                const ImVec2 p(c.x + dir.x * rad, c.y + dir.y * rad);
                const float d = S(4.5f);
                dl->AddQuadFilled(ImVec2(p.x, p.y - d), ImVec2(p.x + d, p.y), ImVec2(p.x, p.y + d), ImVec2(p.x - d, p.y), IM_COL32(0xE0, 0x20, 0x20, 255));
              }
            }
            (void)ink;
            ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + bh + S(6)));
            // Degrees: typed or stepped; a number ends Vertical Text
            int deg = vertical ? 0 : f.edit.rotation;
            ImGui::SetNextItemWidth(S(96));
            if (ImGui::InputInt("##degrees", &deg, 1, 15)) f.edit.rotation = static_cast<int16_t>(std::clamp(deg, -90, 90));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("Degrees"));
          }
          ImGui::EndGroup();
          break;
        }
        case 2: {
          ImGui::BeginGroup();
          ImGui::TextUnformatted(TR("Font: "));
          ImGui::SetNextItemWidth(S(260));
          if (ImGui::InputTextWithHint("##ff", f.font_name.c_str(), f.font_filter, sizeof(f.font_filter))) {
            for (const std::string& fam : ui::SystemFontFamilies()) {
              if (engine::Lower(fam) == engine::Lower(f.font_filter)) f.font_name = fam;
            }
          }
          ImGui::BeginChild("##fonts", ImVec2(S(260), S(200)), ImGuiChildFlags_Borders);
          for (const std::string& fam : ui::SystemFontFamilies()) {
            if (f.font_filter[0] && engine::Lower(fam).find(engine::Lower(f.font_filter)) == std::string::npos) continue;
            if (ImGui::Selectable(fam.c_str(), fam == f.font_name)) {
              f.font_name = fam;
              f.font_filter[0] = 0;
            }
            if (fam == f.font_name && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
          }
          ImGui::EndChild();
          ImGui::EndGroup();
          ImGui::SameLine();
          ImGui::BeginGroup();
          const char* styles[] = {TR("Regular"), TR("Italic"), TR("Bold"), TR("Bold Italic")};
          int st = (f.edit.bold ? 2 : 0) + (f.edit.italic ? 1 : 0);
          ImGui::SetNextItemWidth(S(140));
          {
            const ImVec2 at = ImGui::GetCursorScreenPos();  // combos aren't recorded: tests find it by its id
            ui::NoteUiItem("##font_style", ImRect(at, at + ImVec2(S(140), ImGui::GetFrameHeight())));
          }
          if (ui::Combo(TR("Font style"), &st, styles, 4)) {
            f.edit.bold = st >= 2;
            f.edit.italic = st % 2 == 1;
          }
          // Size: a box to type in over Excel's list of sizes
          float size = f.edit.font_size > 0 ? f.edit.font_size : s.snap->wb.styles->default_font_size;
          {
            static char size_text[16] = "";
            static float shown = -1.0f;
            if (shown != size || ImGui::IsWindowAppearing()) {
              shown = size;
              if (std::fabs(size - std::round(size)) < 0.01f) std::snprintf(size_text, sizeof(size_text), "%d", static_cast<int>(std::round(size)));
              else std::snprintf(size_text, sizeof(size_text), "%.1f", size);
            }
            ImGui::TextUnformatted(TR("Size:"));
            ImGui::SetNextItemWidth(S(140));
            if (ImGui::InputText("##fsize", size_text, sizeof(size_text), ImGuiInputTextFlags_CharsDecimal)) {
              const float v = static_cast<float>(std::atof(size_text));
              if (v >= 1.0f && v <= 409.0f) f.edit.font_size = shown = v;
            }
            static const int kSizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
            ImGui::BeginChild("##fsizes", ImVec2(S(140), S(90)), ImGuiChildFlags_Borders);
            for (int k : kSizes) {
              const std::string label = std::to_string(k);
              const bool on = std::fabs(size - static_cast<float>(k)) < 0.01f;
              if (ImGui::Selectable(label.c_str(), on)) f.edit.font_size = static_cast<float>(k);
              if (on && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            }
            ImGui::EndChild();
          }
          const char* ul[] = {TR("None"), TR("Single"), TR("Double"), TR("Single Accounting"), TR("Double Accounting")};
          int u = static_cast<int>(f.edit.underline);
          ImGui::SetNextItemWidth(S(140));
          {
            const ImVec2 at = ImGui::GetCursorScreenPos();  // combos aren't recorded: tests find it by its id
            ui::NoteUiItem("##underline", ImRect(at, at + ImVec2(S(140), ImGui::GetFrameHeight())));
          }
          if (ui::Combo(TR("Underline"), &u, ul, 5)) f.edit.underline = static_cast<model::Underline>(u);
          ColorEdit(TR("Color"), &f.edit.font_color, true, TR("Automatic"));
          ImGui::Checkbox(TR("Strikethrough"), &f.edit.strike);
          bool sup = f.edit.script == 1, sub = f.edit.script == 2;
          if (ImGui::Checkbox(TR("Superscript"), &sup)) f.edit.script = sup ? 1 : 0;
          ImGui::SameLine();
          if (ImGui::Checkbox(TR("Subscript"), &sub)) f.edit.script = sub ? 2 : 0;
          ImGui::EndGroup();
          ImGui::Separator();
          // the preview in the chosen font, style, size, color, underline and strikethrough, as Excel's Preview box
          ImGui::TextUnformatted(TR("Preview: "));
          {
            ImFont* pf = ui::CellFont(f.font_name, f.edit.bold, f.edit.italic);
            const float px = std::min(48.0f, (f.edit.font_size > 0 ? f.edit.font_size : s.snap->wb.styles->default_font_size) * 96.0f / 72.0f) * ui::Dpi();
            const char* sample = TR("AaBbCcYyZz 123");
            const ImVec2 ts = pf->CalcTextSizeA(px, FLT_MAX, 0, sample);
            const ImVec2 box(std::max(S(380), ts.x + S(20)), ts.y + S(16));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, p + box, IM_COL32_WHITE);
            dl->AddRect(p, p + box, IM_COL32(0xA0, 0xA0, 0xA0, 255));
            const ImVec4 cv = f.edit.font_color ? ArgbToVec(f.edit.font_color) : ImVec4(0, 0, 0, 1);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(cv);
            const ImVec2 tp(p.x + (box.x - ts.x) * 0.5f, p.y + (box.y - ts.y) * 0.5f);
            dl->AddText(pf, px, tp, col, sample);
            if (f.edit.underline != model::Underline::None) {
              const float uy = tp.y + ts.y - std::max(1.0f, px * 0.08f);
              dl->AddLine(ImVec2(tp.x, uy), ImVec2(tp.x + ts.x, uy), col);
              if (f.edit.underline == model::Underline::Double || f.edit.underline == model::Underline::DoubleAccounting)
                dl->AddLine(ImVec2(tp.x, uy + S(2)), ImVec2(tp.x + ts.x, uy + S(2)), col);
            }
            if (f.edit.strike) dl->AddLine(ImVec2(tp.x, tp.y + ts.y * 0.55f), ImVec2(tp.x + ts.x, tp.y + ts.y * 0.55f), col);
            ImGui::Dummy(box);
          }
          break;
        }
        case 3:
          DrawBorderTab(s, f);
          break;
        case 4: {
          ImGui::BeginGroup();
          ImGui::TextUnformatted(TR("Background Color"));
          // Excel shows the palette on the page: No Color, the theme colors with their shades, the standard colors
          {
            uint32_t ref = 0;
            ImGui::PushID("##fillpal");
            if (ui::ribbon::PaletteSwatches(&ref, TR("No Color"))) {
              f.edit.fill_color = ref;
              if (!ref) f.edit.gradient = model::FillGradient::None;
            }
            ImGui::PopID();
          }
          ImGui::TextDisabled("%s", TR("More Colors:"));
          ImGui::SameLine();
          ColorEdit("##fill", &f.edit.fill_color, true, TR("No Color"));
          ImGui::EndGroup();
          ImGui::SameLine(0, S(24));
          ImGui::BeginGroup();
          ImGui::TextUnformatted(TR("Pattern Color"));
          ColorEdit("##pcolor", &f.edit.pattern_color, true, TR("Automatic"));
          // Excel's Pattern Style: a 6 x 3 grid of the patterns as they look, each named in its tooltip
          ImGui::TextUnformatted(TR("Pattern Style"));
          {
            const char* kPatterns[] = {TR("Solid"),        TR("Gray 75%"),       TR("Gray 50%"),       TR("Gray 25%"),         TR("Gray 12.5%"),
                                       TR("Gray 6.25%"),   TR("Horizontal Stripe"), TR("Vertical Stripe"), TR("Reverse Diagonal Stripe"),
                                       TR("Diagonal Stripe"), TR("Diagonal Crosshatch"), TR("Thick Diagonal Crosshatch"), TR("Thin Horizontal Stripe"),
                                       TR("Thin Vertical Stripe"), TR("Thin Reverse Diagonal Stripe"), TR("Thin Diagonal Stripe"),
                                       TR("Thin Horizontal Crosshatch"), TR("Thin Diagonal Crosshatch")};
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const float sw = S(26), gap = S(4);
            const uint32_t ink = f.edit.pattern_color ? f.edit.pattern_color : 0xFF000000u;
            for (int k = 0; k < static_cast<int>(std::size(kPatterns)); ++k) {
              if (k % 6) ImGui::SameLine(0, gap);
              ImGui::PushID(k);
              const ImVec2 q = ImGui::GetCursorScreenPos();
              const ImRect tile(q, q + ImVec2(sw, sw));
              if (ImGui::InvisibleButton("##pat", tile.GetSize())) {
                f.edit.pattern = static_cast<model::FillPattern>(k);
                f.edit.gradient = model::FillGradient::None;
                // a pattern on no background: Excel puts it on white
                if (k && !(f.edit.fill_color >> 24)) f.edit.fill_color = 0xFFFFFFFF;
              }
              const bool hovered = ImGui::IsItemHovered();
              if (hovered) ImGui::SetTooltip("%s", kPatterns[k]);
              ui::NoteUiItem((std::string("Pattern: ") + kPatterns[k]).c_str(), tile);
              model::Style look;
              look.fill_color = k ? 0xFFFFFFFFu : ink;
              look.pattern = static_cast<model::FillPattern>(k);
              look.pattern_color = ink;
              render::DrawCellFill(pdl, tile, look, S(1), IM_COL32_WHITE);
              const bool chosen = f.edit.gradient == model::FillGradient::None && static_cast<int>(f.edit.pattern) == k && (k || (f.edit.fill_color >> 24));
              pdl->AddRect(tile.Min, tile.Max, chosen ? IM_COL32(0x21, 0x73, 0x46, 255) : hovered ? IM_COL32(242, 148, 0, 255) : IM_COL32(0xA0, 0xA0, 0xA0, 255), 0.0f,
                           chosen || hovered ? S(2) : 1.0f);
              ImGui::PopID();
            }
          }
          ImGui::Spacing();
          // Fill Effects: the shading styles drawn as they look (two colors), then the second color
          ImGui::TextUnformatted(TR("Fill Effects"));
          {
            struct Shade {
              const char* name;
              model::FillGradient g;
              int16_t angle;
            };
            const Shade kShades[] = {{TR("None"), model::FillGradient::None, 0},          {TR("Horizontal"), model::FillGradient::Linear, 90},
                                     {TR("Vertical"), model::FillGradient::Linear, 0},     {TR("Diagonal up"), model::FillGradient::Linear, 135},
                                     {TR("Diagonal down"), model::FillGradient::Linear, 45}, {TR("From corner"), model::FillGradient::FromCorner, 0},
                                     {TR("From center"), model::FillGradient::FromCenter, 0}};
            ImDrawList* gdl = ImGui::GetWindowDrawList();
            const float sw = S(34), gap = S(4);
            const uint32_t c1 = (f.edit.fill_color >> 24) ? f.edit.fill_color : 0xFFFFFFFFu;
            const uint32_t c2 = (f.edit.gradient_color2 >> 24) ? f.edit.gradient_color2 : 0xFF5B9BD5u;
            for (int k = 0; k < static_cast<int>(std::size(kShades)); ++k) {
              if (k) ImGui::SameLine(0, gap);
              ImGui::PushID(100 + k);
              const ImVec2 q = ImGui::GetCursorScreenPos();
              const ImRect tile(q, q + ImVec2(sw, S(24)));
              if (ImGui::InvisibleButton("##shade", tile.GetSize())) {
                f.edit.gradient = kShades[k].g;
                f.edit.gradient_angle = kShades[k].angle;
                if (kShades[k].g != model::FillGradient::None) {
                  f.edit.pattern = model::FillPattern::Solid;
                  f.edit.fill_color = c1;
                  f.edit.gradient_color2 = c2;
                }
              }
              const bool hovered = ImGui::IsItemHovered();
              if (hovered) ImGui::SetTooltip("%s", kShades[k].name);
              ui::NoteUiItem((std::string("Shading: ") + kShades[k].name).c_str(), tile);
              model::Style look;
              look.fill_color = c1;
              look.gradient = kShades[k].g;
              look.gradient_angle = kShades[k].angle;
              look.gradient_color2 = c2;
              if (kShades[k].g == model::FillGradient::None) gdl->AddRectFilled(tile.Min, tile.Max, IM_COL32_WHITE);
              else render::DrawCellFill(gdl, tile, look, S(1), IM_COL32_WHITE);
              if (kShades[k].g == model::FillGradient::None) gdl->AddLine(tile.Min, tile.Max, IM_COL32(0xC0, 0x40, 0x40, 255));
              const bool chosen = f.edit.gradient == kShades[k].g && (kShades[k].g != model::FillGradient::Linear || f.edit.gradient_angle == kShades[k].angle);
              if (chosen && kShades[k].g == model::FillGradient::FromCorner) look.gradient_angle = f.edit.gradient_angle;
              gdl->AddRect(tile.Min, tile.Max, chosen ? IM_COL32(0x21, 0x73, 0x46, 255) : hovered ? IM_COL32(242, 148, 0, 255) : IM_COL32(0xA0, 0xA0, 0xA0, 255), 0.0f,
                           chosen || hovered ? S(2) : 1.0f);
              ImGui::PopID();
            }
            if (f.edit.gradient == model::FillGradient::FromCorner) {
              // Excel's Variants: the corner the first color starts in
              ImGui::TextUnformatted(TR("Variants"));
              const char* kCorners[] = {TR("Upper left"), TR("Upper right"), TR("Lower left"), TR("Lower right")};
              for (int k = 0; k < 4; ++k) {
                if (k) ImGui::SameLine(0, gap);
                ImGui::PushID(200 + k);
                const ImVec2 q = ImGui::GetCursorScreenPos();
                const ImRect tile(q, q + ImVec2(sw, S(24)));
                if (ImGui::InvisibleButton("##variant", tile.GetSize())) f.edit.gradient_angle = static_cast<int16_t>(k);
                const bool hovered = ImGui::IsItemHovered();
                if (hovered) ImGui::SetTooltip("%s", kCorners[k]);
                ui::NoteUiItem((std::string("Variant: ") + kCorners[k]).c_str(), tile);
                model::Style look;
                look.fill_color = c1;
                look.gradient = model::FillGradient::FromCorner;
                look.gradient_angle = static_cast<int16_t>(k);
                look.gradient_color2 = c2;
                render::DrawCellFill(gdl, tile, look, S(1), IM_COL32_WHITE);
                const bool chosen = f.edit.gradient_angle == k;
                gdl->AddRect(tile.Min, tile.Max, chosen ? IM_COL32(0x21, 0x73, 0x46, 255) : hovered ? IM_COL32(242, 148, 0, 255) : IM_COL32(0xA0, 0xA0, 0xA0, 255), 0.0f,
                             chosen || hovered ? S(2) : 1.0f);
                ImGui::PopID();
              }
            }
            if (f.edit.gradient != model::FillGradient::None) {
              ImGui::TextUnformatted(TR("Color 2"));
              ImGui::SameLine();
              ColorEdit("##gcolor2", &f.edit.gradient_color2, false, nullptr);
            }
          }
          ImGui::Spacing();
          ImGui::TextUnformatted(TR("Sample"));
          const ImVec2 p0 = ImGui::GetCursorScreenPos();
          const ImRect sample(p0, ImVec2(p0.x + S(240), p0.y + S(48)));
          render::DrawCellFill(ImGui::GetWindowDrawList(), sample, f.edit, S(1), IM_COL32_WHITE);
          ImGui::GetWindowDrawList()->AddRect(sample.Min, sample.Max, IM_COL32(0xA0, 0xA0, 0xA0, 255));
          ImGui::Dummy(sample.GetSize());
          ImGui::EndGroup();
          break;
        }
        default: {
          ImGui::Checkbox(TR("Locked"), &f.edit.locked);
          ImGui::Checkbox(TR("Hidden"), &f.edit.hidden);
          ImGui::TextWrapped("%s", TR("Locking cells or hiding formulas has no effect until you protect the worksheet (Review tab, Protect group, Protect Sheet button)."));
          ui::NoteUiItem(TR("Locking cells or hiding formulas has no effect until you protect the worksheet (Review tab, Protect group, Protect Sheet button)."),
                         ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
          break;
        }
      }
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  bool ok = false;
  close = OkCancel(&ok);
  apply = ok;
  if (apply && (!g_fmt_for.on || (g_fmt_for.tabs & 1u))) {
    // a code Excel refuses ("0;@;0", "[Blue", "0_"): its message, and the dialog stays with the code to fix (Range.NumberFormat refuses them: measured)
    const std::string code = BuildCode(f);
    if (code != f.base_numfmt && !formula::ValidFormatCode(code)) {
      apply = close = false;
      s.ShowError("YCGrid", TR("YCGrid cannot use the number format you typed.\n\nTry using one of the built-in number formats."));
      if (s.dialog == Dialog::Message) g_fmt_return = true;
    }
  }
  if (g_fmt_for.on && g_fmt_for.find) {
    // Excel's Find Format: Choose Format From Cell... and Clear at the lower left
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button(TR("Choose Format From Cell..."))) {
      close = true;
      apply = false;
      g_fmt_for.on = false;
      EndDialog(s, close);
      if (g_fmt_for.pick) g_fmt_for.pick();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Clear"))) {
      g_fmt_for.spec = engine::FormatSpec{};
      InitFormatDialog(s);
    }
  }
  if (apply && f.font_filter[0]) {
    // a name typed in the Font box: that font, else the first one the box lists (as Excel's box completes it)
    std::string pick;
    for (const std::string& fam : ui::SystemFontFamilies()) {
      if (engine::Lower(fam) == engine::Lower(f.font_filter)) {
        pick = fam;
        break;
      }
      if (pick.empty() && engine::Lower(fam).find(engine::Lower(f.font_filter)) != std::string::npos) pick = fam;
    }
    f.font_name = pick.empty() ? std::string(f.font_filter) : pick;
  }
  if ((apply || close) && g_fmt_for.on) {
    // Find Format / Replace Format: the fields chosen (and those chosen before)
    if (apply) {
      engine::FormatSpec out = g_fmt_for.spec;
      out.style = f.edit;
      out.style.numfmt = out.style.font_name = 0;
      uint32_t m = out.mask | engine::DiffMask(model::Style{}, out.style);
      const std::string code = BuildCode(f);
      if (code != f.base_numfmt && (code != f.opened_code || f.type_picked)) {
        m |= engine::kFmNumber;
        out.numfmt = code;
      }
      if (f.font_name != f.base_font) {
        m |= engine::kFmFontName;
        out.font = f.font_name;
      }
      const int map[8] = {model::kTop, model::kBottom, model::kLeft, model::kRight, -1, -1, model::kDiagDown, model::kDiagUp};
      for (int k = 0; k < 8; ++k) {
        if (map[k] < 0 || !f.edges[k]) continue;
        out.style.border[map[k]] = f.edges[k] == 1 ? f.side[k] : model::BorderSide{};
        m |= engine::kFmBorderLeft << map[k];
      }
      out.mask = m;
      if (f.edges[4]) g_fmt_for.inner_h = f.edges[4] == 1 ? f.side[4] : model::BorderSide{};
      if (f.edges[5]) g_fmt_for.inner_v = f.edges[5] == 1 ? f.side[5] : model::BorderSide{};
      g_fmt_for.spec = out;
      if (g_fmt_for.done) g_fmt_for.done(out);
    }
    g_fmt_for.on = false;
    EndDialog(s, true);
    return;
  }
  if (apply && StyleFormatTarget()) {
    // Style > Format...: every field goes into the style
    model::Style e = f.edit;
    const std::string code = BuildCode(f), font = f.font_name;
    s.engine->Call([&](Document& d) {
      auto& tbl = d.wb_mut().styles.mut();
      e.numfmt = code == "General" ? 0u : tbl.InternString(code);
      e.font_name = font == tbl.default_font ? 0u : tbl.InternString(font);
    });
    // the Border tab: a style is one cell, so only the outline and the diagonals apply
    const int map[8] = {model::kTop, model::kBottom, model::kLeft, model::kRight, -1, -1, model::kDiagDown, model::kDiagUp};
    for (int k = 0; k < 8; ++k) {
      if (map[k] < 0 || !f.edges[k]) continue;
      e.border[map[k]] = f.edges[k] == 1 ? f.side[k] : model::BorderSide{};
    }
    *StyleFormatTarget() = e;
    apply = false;
    close = true;
  }
  if (close && StyleFormatTarget()) {
    EndDialog(s, close);
    StyleFormatDone(s, true);
    return;
  }
  if (apply && f.chars) {
    const model::Style b = f.base, e = f.edit;
    const bool font_changed = f.font_name != f.base_font;
    const std::string font = f.font_name;
    EditSelectionFormat(s, [&](model::RichRun& r, const model::RunFont&) {
      if (font_changed) r.font = font;
      if (e.bold != b.bold) r.bold = e.bold ? 1 : 0;
      if (e.italic != b.italic) r.italic = e.italic ? 1 : 0;
      if (e.strike != b.strike) r.strike = e.strike ? 1 : 0;
      if (e.underline != b.underline) r.underline = static_cast<int8_t>(e.underline);
      if (e.font_color != b.font_color) r.color = e.font_color;
      if (e.font_size != b.font_size) r.size = e.font_size;
      if (e.script != b.script) r.script = static_cast<int8_t>(e.script);
    });
    apply = false;
  }
  // Alignment > Merge cells: run after the dialog has closed (merging cells that hold data asks first)
  const char* merge_cmd = apply && s.engine && f.merge != f.merge0 ? (f.merge ? "MergeCells" : "UnmergeCells") : nullptr;
  if (apply && s.engine) {
    // only apply fields that changed (with a multi-cell selection the other cells keep their own settings)
    const model::Style b = f.base, e = f.edit;
    const std::string code = BuildCode(f);
    // the Number tab untouched leaves the cells' own codes (a date as mmm-yy isn't rewritten as yyyy/m/d)
    const bool numfmt_changed = code != f.base_numfmt && (code != f.opened_code || f.type_picked);
    const bool font_changed = f.font_name != f.base_font;
    const std::string font = f.font_name;
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    int edges[8];
    model::BorderSide sides[8];
    for (int k = 0; k < 8; ++k) {
      edges[k] = f.edges[k];
      sides[k] = f.side[k];
    }
    // rows that size themselves fit the new look (Wrap Text, a bigger font), in the same undo step, as Excel's do
    const auto fit = FitRowsForStyle(s, sheet, areas, [b, e](model::Style& x) {
      if (e.bold != b.bold) x.bold = e.bold;
      if (e.italic != b.italic) x.italic = e.italic;
      if (e.font_size != b.font_size) x.font_size = e.font_size;
      if (e.wrap != b.wrap) x.wrap = e.wrap;
      if (e.shrink != b.shrink) x.shrink = e.shrink;
    }, font_changed ? &font : nullptr);  // a new font name is measured too
    s.engine->PostUser(TR("Format Cells"), [=](Document& d) {
      auto& tbl = d.wb_mut().styles.mut();
      const uint32_t numfmt_id = numfmt_changed ? (code == "General" ? 0u : tbl.InternString(code)) : 0u;
      const uint32_t font_id = font_changed ? tbl.InternString(font) : 0u;
      for (const Rect& a : areas) {
        d.ModifyStyle(sheet, a, [&](model::Style& x) {
          if (numfmt_changed) x.numfmt = numfmt_id;
          if (font_changed) x.font_name = font_id;
          if (e.bold != b.bold) x.bold = e.bold;
          if (e.italic != b.italic) x.italic = e.italic;
          if (e.font_size != b.font_size) x.font_size = e.font_size;
          if (e.underline != b.underline) x.underline = e.underline;
          if (e.strike != b.strike) x.strike = e.strike;
          if (e.script != b.script) x.script = e.script;
          if (e.font_color != b.font_color) x.font_color = e.font_color;
          if (e.fill_color != b.fill_color) x.fill_color = e.fill_color;
          if (e.pattern != b.pattern) x.pattern = e.pattern;
          if (e.pattern_color != b.pattern_color) x.pattern_color = e.pattern_color;
          if (e.gradient != b.gradient) x.gradient = e.gradient;
          if (e.gradient_angle != b.gradient_angle) x.gradient_angle = e.gradient_angle;
          if (e.gradient_color2 != b.gradient_color2) x.gradient_color2 = e.gradient_color2;
          if (e.halign != b.halign) x.halign = e.halign;
          if (e.valign != b.valign) x.valign = e.valign;
          if (e.indent != b.indent) x.indent = e.indent;
          if (e.wrap != b.wrap) x.wrap = e.wrap;
          if (e.shrink != b.shrink) x.shrink = e.shrink;
          if (e.rotation != b.rotation) x.rotation = e.rotation;
          if (e.justify_last_line != b.justify_last_line) x.justify_last_line = e.justify_last_line;
          if (e.reading_order != b.reading_order) x.reading_order = e.reading_order;
          if (e.locked != b.locked) x.locked = e.locked;
          if (e.hidden != b.hidden) x.hidden = e.hidden;
        });
        // the Border tab: inside lines on every cell, then the outline on the area's rim; each edge with its own line
        auto put = [&](const Rect& r, int edge, int e) {
          if (!edges[e]) return;
          const model::BorderSide side = edges[e] == 1 ? sides[e] : model::BorderSide{};
          d.ModifyStyle(sheet, r, [&](model::Style& x) { x.border[edge] = side; });
        };
        if (a.r1 > a.r0) {  // inside horizontal: between the rows only
          put(Rect{a.r0 + 1, a.c0, a.r1, a.c1}, model::kTop, 4);
          put(Rect{a.r0, a.c0, a.r1 - 1, a.c1}, model::kBottom, 4);
        }
        if (a.c1 > a.c0) {  // inside vertical: between the columns only
          put(Rect{a.r0, a.c0 + 1, a.r1, a.c1}, model::kLeft, 5);
          put(Rect{a.r0, a.c0, a.r1, a.c1 - 1}, model::kRight, 5);
        }
        put(a, model::kDiagDown, 6);
        put(a, model::kDiagUp, 7);
        put(Rect{a.r0, a.c0, a.r0, a.c1}, model::kTop, 0);
        put(Rect{a.r1, a.c0, a.r1, a.c1}, model::kBottom, 1);
        put(Rect{a.r0, a.c0, a.r1, a.c0}, model::kLeft, 2);
        put(Rect{a.r0, a.c1, a.r1, a.c1}, model::kRight, 3);
        // a cleared outline edge also comes off the neighbour that draws the same line
        auto clear_outside = [&](const Rect& r, int edge, int e) {
          if (edges[e] == 2) d.ModifyStyle(sheet, r, [&](model::Style& x) { x.border[edge] = {}; });
        };
        if (a.r0 > 0) clear_outside(Rect{a.r0 - 1, a.c0, a.r0 - 1, a.c1}, model::kBottom, 0);
        clear_outside(Rect{a.r1 + 1, a.c0, a.r1 + 1, a.c1}, model::kTop, 1);
        if (a.c0 > 0) clear_outside(Rect{a.r0, a.c0 - 1, a.r1, a.c0 - 1}, model::kRight, 2);
        clear_outside(Rect{a.r0, a.c1 + 1, a.r1, a.c1 + 1}, model::kLeft, 3);
        // a single cell row / column: no inside line was asked for, its outline edges came from the rim passes
      }
      for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
    });
  }
  EndDialog(s, close);
  if (merge_cmd) ui::Commands().Execute(merge_cmd);
}

// ---- Find and Replace ----
// Excel's dialog is modeless: the sheet can be clicked and typed in while it stays open. One window, two tabs.
char g_find[256] = "", g_repl[256] = "";
bool g_match_case = false, g_whole = false, g_by_rows = true, g_scope_wb = false;
int g_look_in = 0;      // 0 formulas (Excel's default), 1 values, 2 notes
int g_find_open = 0;    // 0 closed, 1 the Find tab, 2 the Replace tab
bool g_find_tab_pick = false, g_find_focus = false, g_find_options = false;
struct FindHit {
  uint32_t sheet, r, c;
  std::string sheet_name, text;
  std::string value, formula, name;  // Find All's Value, Formula and Name columns (as Excel's list)
};
std::vector<FindHit> g_hits;
int g_hit_sel = -1;  // the row of the list last clicked, highlighted as in Excel
// Format...: the format to find and to replace with; Choose Format From Cell arms a pick (1 find, 2 replace) taken
// from the next cell clicked on the sheet
engine::FormatSpec g_find_fmt, g_repl_fmt;
int g_fmt_pick = 0;
const engine::FormatSpec* FindFmt() { return g_find_fmt.Any() ? &g_find_fmt : nullptr; }
const engine::FormatSpec* ReplFmt() { return g_repl_fmt.Any() ? &g_repl_fmt : nullptr; }

bool NoteMatches(std::string text, std::string what) {
  if (!g_match_case) {
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (auto& ch : what) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return g_whole ? text == what : text.find(what) != std::string::npos;
}

// find all within one sheet
void FindAllIn(Document& d, uint32_t sheet, const Rect& w, const std::string& what, std::vector<FindHit>* out) {
  const model::Sheet* sh = d.S(sheet);
  if (!sh || out->size() >= 5000) return;
  if (g_look_in == 2) {
    // notes: in the search order (by rows or by columns)
    std::vector<FindHit> found;
    for (const auto& [k, n] : *sh->notes) {
      const uint32_t r = static_cast<uint32_t>(k >> 16), c = static_cast<uint32_t>(k & 0xFFFF);
      if (w.Contains(r, c) && NoteMatches(n.text, what)) found.push_back({sheet, r, c, sh->name, n.text});
    }
    if (!g_by_rows) std::stable_sort(found.begin(), found.end(), [](const FindHit& a, const FindHit& b) { return a.c != b.c ? a.c < b.c : a.r < b.r; });
    for (auto& h : found) {
      if (out->size() >= 5000) break;
      out->push_back(std::move(h));
    }
    return;
  }
  const auto& tbl = *d.wb().styles;
  const bool in_formulas = g_look_in == 0;
  // the defined names on this sheet, for the Name column (a name covering the cell)
  std::vector<std::pair<std::string, Rect>> names;
  for (const auto& n : *d.wb().names) {
    if (n.hidden || names.size() >= 500 || (n.sheet_scope >= 0 && static_cast<uint32_t>(n.sheet_scope) != sheet)) continue;
    uint32_t ns = 0;
    Rect nr;
    if (d.ResolveNameRef(n.name, sheet, &ns, &nr) && ns == sheet) names.emplace_back(n.name, nr);
  }
  for (const core::CellPos& p : d.FindAll(sheet, w, what, g_by_rows, g_match_case, g_whole, in_formulas, 5000 - out->size(), FindFmt())) {
    const std::string value = engine::DisplayTextOf(sh->GetValue(p.row, p.col), tbl.NumberFormat(tbl.Get(sh->GetStyle(p.row, p.col))));
    FindHit h{sheet, p.row, p.col, sh->name, in_formulas ? d.GetInputText(sheet, p.row, p.col) : value};
    h.value = value;
    if (sh->GetFormula(p.row, p.col)) h.formula = d.GetInputText(sheet, p.row, p.col);
    for (const auto& [nm, nr] : names) {
      if (nr.Contains(p.row, p.col)) {
        h.name = nm;
        break;
      }
    }
    out->push_back(std::move(h));
  }
}

void GoToHit(AppState& s, const FindHit& h) {
  if (h.sheet != s.focus_sheet) ActivateSheet(s, h.sheet);
  auto& v = s.Win(h.sheet).view;
  render::SelectCell(v, h.r, h.c);
  if (const model::Sheet* sh = s.snap->Sheet(h.sheet)) render::EnsureVisible(*sh, v, h.r, h.c);
}

void OpenFindWindow(AppState& s, bool replace) {
  g_find_open = replace ? 2 : 1;
  g_find_tab_pick = true;
  g_find_focus = true;
  if (replace && g_look_in == 2) g_look_in = 0;  // Replace looks in formulas only (as Excel)
  // the active cell's text when nothing was searched yet
  if (!g_find[0] && s.snap) {
    if (const model::Sheet* sh = s.FocusSheet()) {
      const auto& sel = s.FocusView().sel;
      const auto& v = sh->GetValue(sel.act_r, sel.act_c);
      if (v.IsString()) std::snprintf(g_find, sizeof(g_find), "%.*s", static_cast<int>(std::min<size_t>(v.str().size(), sizeof(g_find) - 1)), v.str().data());
    }
  }
}


void DrawFindReplace(AppState& s) {
  if (!g_find_open) return;
  ImGui::SetNextWindowSize(ImVec2(S(600), 0), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  const bool shown = ImGui::Begin(TR("Find and Replace###yc_find"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
  if (!shown) {
    ImGui::End();
    if (!open) g_find_open = 0;
    return;
  }
  if (g_find_focus) ImGui::SetWindowFocus();
  // the dialog has the keyboard: Alt + a letter is its own (Replace All...), not the ribbon's KeyTips behind it
  NoteDialogKeyboard();
  bool close = !open || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape));
  bool replace = g_find_open == 2;
  if (BeginDlgTabBar("##find_tabs")) {
    if (ImGui::BeginTabItem(TR("Find"), nullptr, g_find_tab_pick && !replace ? ImGuiTabItemFlags_SetSelected : 0)) {
      replace = false;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Replace"), nullptr, g_find_tab_pick && replace ? ImGuiTabItemFlags_SetSelected : 0)) {
      replace = true;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (!g_find_tab_pick && (replace ? 2 : 1) != g_find_open) {
    g_find_open = replace ? 2 : 1;  // the user clicked the other tab
    g_find_focus = true;
  }
  // a tab asked for (Ctrl+H) is selected a frame later, and not at all on the frame the window reappears: ask until it shows
  if ((replace ? 2 : 1) == g_find_open) g_find_tab_pick = false;
  replace = g_find_open == 2;
  if (replace && g_look_in == 2) g_look_in = 0;
  const float label_w = S(100);
  // Options >>: the format's preview and Format... beside each box (Excel)
  const float fmt_w = g_find_options ? S(110) + S(100) + S(30) : 0.0f;
  const float box_w = ImGui::GetContentRegionAvail().x - label_w - fmt_w;
  // a format chosen with Format...: "Preview*" drawn in it, or "No Format Set"
  auto format_row = [&](const char* id, engine::FormatSpec& spec, int which) {
    if (!g_find_options) return;
    ImGui::SameLine();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImRect box(p, ImVec2(p.x + S(110), p.y + ImGui::GetFrameHeight()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* text = spec.Any() ? TR("Preview*") : TR("No Format Set");
    model::Style look = spec.style;
    dl->AddRectFilled(box.Min, box.Max, IM_COL32_WHITE);
    if (spec.Any() && (spec.mask & engine::kFmFill) && look.HasFill()) render::DrawCellFill(dl, box, look, S(1), IM_COL32_WHITE);
    ImU32 ink = IM_COL32(0x40, 0x40, 0x40, 255);
    if (spec.Any() && (look.font_color >> 24)) {
      const uint32_t c = model::ResolveColor(look.font_color);
      ink = IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    }
    ImFont* font = ui::CellFont(spec.Any() && !spec.font.empty() ? spec.font : std::string("Calibri"), spec.Any() && look.bold, spec.Any() && look.italic);
    const float px = ImGui::GetFontSize();
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 tp(box.GetCenter().x - sz.x * 0.5f, box.GetCenter().y - sz.y * 0.5f);
    dl->AddText(font, px, tp, ink, text);
    if (spec.Any() && look.underline != model::Underline::None) dl->AddLine(ImVec2(tp.x, tp.y + sz.y - 1), ImVec2(tp.x + sz.x, tp.y + sz.y - 1), ink);
    if (spec.Any() && look.strike) dl->AddLine(ImVec2(tp.x, tp.y + sz.y * 0.55f), ImVec2(tp.x + sz.x, tp.y + sz.y * 0.55f), ink);
    dl->AddRect(box.Min, box.Max, IM_COL32(0xA0, 0xA0, 0xA0, 255));
    ui::NoteUiItem(id, box);
    ImGui::Dummy(box.GetSize());
    // Format... (a split button: the Find Format dialog; its arrow: Format..., Choose Format From Cell..., Clear)
    ImGui::SameLine();
    ImGui::PushID(id);
    auto open_dialog = [&s, &spec, which] {
      g_fmt_for = FormatFor{};
      g_fmt_for.on = true;
      g_fmt_for.title = which == 1 ? TR("Find Format") : TR("Replace Format");
      g_fmt_for.spec = spec;
      g_fmt_for.done = [which](const engine::FormatSpec& out) { (which == 1 ? g_find_fmt : g_repl_fmt) = out; };
      g_fmt_for.pick = [which] { g_fmt_pick = which; };
      s.OpenDialog(Dialog::FormatCells);
    };
    if (ImGui::Button(TR("Format..."), ImVec2(S(80), 0))) open_dialog();
    ImGui::SameLine(0, 0);
    if (ImGui::ArrowButton("##fmt_menu", ImGuiDir_Down)) ImGui::OpenPopup("##fmt_menu_popup");
    if (ImGui::BeginPopup("##fmt_menu_popup")) {
      if (ImGui::MenuItem(TR("Format..."))) open_dialog();
      if (ImGui::MenuItem(TR("Choose Format From Cell..."))) g_fmt_pick = which;
      if (ImGui::MenuItem(which == 1 ? TR("Clear Find Format") : TR("Clear Replace Format"))) spec = engine::FormatSpec{};
      ImGui::EndPopup();
    }
    ImGui::PopID();
  };
  // Choose Format From Cell: the next cell clicked on the sheet gives its whole format
  if (g_fmt_pick) {
    ImGui::TextColored(ImVec4(0.13f, 0.45f, 0.27f, 1.0f), "%s", TR("Click a cell to use its format."));
    if (ImGui::IsMouseReleased(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && s.snap) {
      if (const model::Sheet* psh = s.FocusSheet()) {
        const auto& sel = s.FocusView().sel;
        const auto& tbl = *s.snap->wb.styles;
        engine::FormatSpec got = engine::SpecOfCell(tbl.Get(psh->GetStyle(sel.act_r, sel.act_c)), tbl);
        got.style.numfmt = got.style.font_name = 0;
        got.style.cell_style = 0;
        (g_fmt_pick == 1 ? g_find_fmt : g_repl_fmt) = got;
      }
      g_fmt_pick = 0;
      g_find_focus = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_fmt_pick = 0;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Find what:"));
  ImGui::SameLine(label_w);
  ImGui::SetNextItemWidth(box_w);
  if (g_find_focus) {
    ImGui::SetKeyboardFocusHere();
    g_find_focus = false;
  }
  const bool enter = ImGui::InputText("##find_what", g_find, sizeof(g_find), ImGuiInputTextFlags_EnterReturnsTrue);
  const bool back = ImGui::GetIO().KeyShift;  // Shift+Enter / Shift+Find Next: the previous match (Excel)
  if (enter) ImGui::SetKeyboardFocusHere(-1);  // the box keeps the keyboard: Enter again finds the next one, as in Excel
  format_row("##find_format", g_find_fmt, 1);
  if (replace) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Replace with:"));
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(box_w);
    ImGui::InputText("##replace_with", g_repl, sizeof(g_repl));
    format_row("##replace_format", g_repl_fmt, 2);
  }
  const bool can_find = g_find[0] || g_find_fmt.Any();  // a format alone finds too
  if (g_find_options) {
    ImGui::Spacing();
    const float cw = S(130);
    auto combo = [&](const char* label, const char* id, int* v, const std::vector<const char*>& items) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::SameLine(label_w);
      ImGui::SetNextItemWidth(cw);
      // combo boxes report no label to the UI item recorder: note it, so tests click it as a person does
      const ImVec2 at = ImGui::GetCursorScreenPos();
      ui::NoteUiItem(id, ImRect(at, at + ImVec2(cw, ImGui::GetFrameHeight())));
      if (ui::BeginCombo(id, items[static_cast<size_t>(std::clamp(*v, 0, static_cast<int>(items.size()) - 1))])) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
          if (ImGui::Selectable(items[static_cast<size_t>(i)], *v == i)) *v = i;
        }
        ImGui::EndCombo();
      }
    };
    // rows as in Excel: Within + Match case, Search + Match entire cell contents, Look in
    int within = g_scope_wb ? 1 : 0, search = g_by_rows ? 0 : 1;
    combo(TR("Within:"), "##find_within", &within, {TR("Sheet"), TR("Workbook")});
    ImGui::SameLine(label_w + cw + S(30));
    ImGui::Checkbox(TR("Match case"), &g_match_case);
    if (DlgKey(ImGuiKey_C)) g_match_case = !g_match_case;  // Excel's letters: Match (c)ase, entire cell c(o)ntents
    combo(TR("Search:"), "##find_search", &search, {TR("By Rows"), TR("By Columns")});
    ImGui::SameLine(label_w + cw + S(30));
    ImGui::Checkbox(TR("Match entire cell contents"), &g_whole);
    if (DlgKey(ImGuiKey_O)) g_whole = !g_whole;
    if (replace) combo(TR("Look in:"), "##find_look", &g_look_in, {TR("Formulas")});
    else combo(TR("Look in:"), "##find_look", &g_look_in, {TR("Formulas"), TR("Values"), TR("Notes")});
    g_scope_wb = within == 1;
    g_by_rows = search == 0;
  }
  ImGui::Spacing();
  {
    const char* opt = g_find_options ? TR("Options <<") : TR("Options >>");
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize(opt).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().WindowPadding.x);
    if (ImGui::Button(opt) || DlgKey(ImGuiKey_T)) g_find_options = !g_find_options;  // Op(t)ions
  }
  ImGui::Separator();
  const model::Sheet* sh = s.FocusSheet();
  auto within = [&](const model::Sheet* x) {
    Rect r = SelRect(s);
    if (g_scope_wb || r.IsCell() || x != sh) r = Rect{0, 0, x->row_limit - 1, x->col_limit - 1};
    return r;
  };
  std::vector<uint32_t> order;  // sheet order starting from the active sheet
  if (s.snap) {
    const auto& v = s.snap->wb.sheets;
    const int32_t i0 = std::max(0, s.snap->wb.IndexOfId(s.focus_sheet));
    for (size_t k = 0; k < v.size(); ++k) {
      const auto& x = v[(static_cast<size_t>(i0) + k) % v.size()];
      if (x->visible && (g_scope_wb || x->id == s.focus_sheet)) order.push_back(x->id);
    }
    if (order.empty() && sh) order.push_back(sh->id);
  }
  auto collect = [&](const std::string& what) {
    std::vector<FindHit> hits;
    std::vector<std::pair<uint32_t, Rect>> targets;
    for (uint32_t id : order) targets.push_back({id, within(s.snap->Sheet(id))});
    s.engine->Call([&](Document& d) {
      for (const auto& [id, w] : targets) FindAllIn(d, id, w, what, &hits);
    });
    return hits;
  };
  auto not_found = [&] { s.ShowError("YCGrid", TR("We couldn't find what you were looking for.")); };
  auto find_next = [&](bool backward) {
    if (!s.engine || !can_find || !sh) return;
    const auto& sel = s.FocusView().sel;
    const std::string what = g_find;
    FindHit hit{};
    bool found = false;
    if (backward || g_look_in == 2) {
      // the previous match (or a note): every match in the search order, then the one before / after the active cell
      const auto hits = collect(what);
      if (hits.empty()) return not_found();
      auto before = [&](const FindHit& h) {
        if (h.sheet != s.focus_sheet) return false;
        return g_by_rows ? (h.r < sel.act_r || (h.r == sel.act_r && h.c < sel.act_c)) : (h.c < sel.act_c || (h.c == sel.act_c && h.r < sel.act_r));
      };
      auto after = [&](const FindHit& h) { return h.sheet != s.focus_sheet || (!before(h) && !(h.r == sel.act_r && h.c == sel.act_c)); };
      if (backward) {
        hit = hits.back();
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
          if (it->sheet == s.focus_sheet && before(*it)) {
            hit = *it;
            break;
          }
        }
      } else {
        hit = hits.front();
        for (const auto& h : hits) {
          if (h.sheet == s.focus_sheet && after(h)) {
            hit = h;
            break;
          }
        }
      }
      found = true;
    } else {
      // search the active sheet first (after the active cell); when wrapping, the workbook scope moves on to the next sheet
      for (size_t k = 0; k < order.size() && !found; ++k) {
        const uint32_t id = order[k];
        const model::Sheet* x = s.snap->Sheet(id);
        const Rect w = within(x);
        const uint32_t sr = k == 0 ? sel.act_r : w.r1, sc = k == 0 ? sel.act_c : w.c1;
        uint32_t r = 0, c = 0;
        const bool ok = s.engine->Call([&](Document& d) { return d.Find(id, w, what, sr, sc, g_by_rows, g_match_case, g_whole, g_look_in == 0, &r, &c, FindFmt()); });
        if (!ok) continue;
        const bool wrapped = k == 0 && order.size() > 1 && (g_by_rows ? (r < sr || (r == sr && c <= sc)) : (c < sc || (c == sc && r <= sr)));
        if (wrapped) continue;
        hit = {id, r, c, x->name, ""};
        found = true;
      }
      if (!found && order.size() > 1) {
        // wrapped all the way around: back to the first hit on the active sheet
        const uint32_t id = order[0];
        const Rect w = within(s.snap->Sheet(id));
        uint32_t r = 0, c = 0;
        found = s.engine->Call([&](Document& d) { return d.Find(id, w, what, w.r1, w.c1, g_by_rows, g_match_case, g_whole, g_look_in == 0, &r, &c, FindFmt()); });
        hit = {id, r, c, "", ""};
      }
    }
    if (!found) return not_found();
    if (hit.sheet == s.focus_sheet && !SelRect(s).IsCell() && !g_scope_wb) {
      auto& v = s.FocusView();
      v.sel.act_r = hit.r;
      v.sel.act_c = hit.c;
      render::EnsureVisible(*sh, v, hit.r, hit.c);
    } else {
      GoToHit(s, hit);
    }
  };
  // the buttons at the lower right, in Excel's order
  {
    std::vector<const char*> labels;
    if (replace) labels = {TR("Replace All"), TR("Replace")};
    labels.push_back(TR("Find All"));
    labels.push_back(TR("Find Next"));
    labels.push_back(TR("Close"));
    float w = 0;
    for (const char* l : labels) w += std::max(S(84), ImGui::CalcTextSize(l).x + S(16)) + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x + ImGui::GetStyle().ItemSpacing.x));
  }
  auto button = [](const char* l) { return ImGui::Button(l, ImVec2(std::max(S(84), ImGui::CalcTextSize(l).x + S(16)), 0)); };
  if (replace) {
    // read-only mode: the status bar says why, never "We couldn't find anything to replace" (the engine refused)
    // Excel's letters: Replace (A)ll, (R)eplace, F(i)nd All, (F)ind Next (Alt with them from a text box)
    if ((button(TR("Replace All")) || DlgKey(ImGuiKey_A)) && s.engine && can_find && !RefuseReadOnlyUi(s)) {
      const std::string what = g_find, with = g_repl;
      const bool mc = g_match_case, wh = g_whole;
      std::vector<std::pair<uint32_t, Rect>> targets;
      for (uint32_t id : order) targets.push_back({id, within(s.snap->Sheet(id))});
      const uint32_t n = s.engine->Call([&](Document& d) {
        d.Begin(TR("Replace"));
        uint32_t k = 0;
        for (const auto& [id, w] : targets) k += d.Replace(id, w, what, with, mc, wh, FindFmt(), ReplFmt());
        d.Commit();
        return k;
      });
      if (n == 0) s.ShowMessage("YCGrid", TR("We couldn't find anything to replace. Click Options for more ways to search."));
      else s.ShowMessage("YCGrid", TR("All done. We made ") + std::to_string(n) + TR(" replacements."));
    }
    ImGui::SameLine();
    if ((button(TR("Replace")) || DlgKey(ImGuiKey_R)) && s.engine && can_find && sh && !RefuseReadOnlyUi(s)) {
      // Excel: the active cell is replaced when it matches, then the next match is selected
      const auto& sel = s.FocusView().sel;
      const uint32_t sheet = s.focus_sheet, r = sel.act_r, c = sel.act_c;
      const std::string what = g_find, with = g_repl;
      const bool mc = g_match_case, wh = g_whole;
      s.engine->Call([&](Document& d) {
        d.Begin(TR("Replace"));
        d.Replace(sheet, Rect::Cell(r, c), what, with, mc, wh, FindFmt(), ReplFmt());
        d.Commit();
      });
      find_next(false);
    }
    ImGui::SameLine();
  }
  if ((button(TR("Find All")) || DlgKey(ImGuiKey_I)) && s.engine && can_find) {
    g_hits = collect(g_find);
    g_hit_sel = -1;
    s.SetStatus(g_hits.empty() ? std::string(TR("We couldn't find what you were looking for")) : TR("Find All: found ") + std::to_string(g_hits.size()) + TR(" cells"));
    if (g_hits.empty()) not_found();
  }
  ImGui::SameLine();
  if (button(TR("Find Next")) || enter || DlgKey(ImGuiKey_F)) find_next(back);
  ImGui::SameLine();
  if (button(TR("Close"))) close = true;
  if (!g_hits.empty()) {
    ImGui::Text(TR("Found %zu cells%s"), g_hits.size(), g_hits.size() >= 5000 ? TR("(showing the first 5000 only)") : "");
    // Excel's columns: Sheet, Name, Cell, Value, Formula (notes: the note); a click on a header sorts
    const bool notes = g_look_in == 2;
    // the list scrolls inside the room left under it in the window (it pushed the dialog past the window's bottom)
    const ImGuiViewport* fvp = ImGui::GetMainViewport();
    const float room = fvp->WorkPos.y + fvp->WorkSize.y - ImGui::GetCursorScreenPos().y - S(60);
    const float list_h = std::clamp(room, S(80), S(200));
    if (ImGui::BeginTable("##hits", notes ? 3 : 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable,
                          ImVec2(0, list_h))) {
      ImGui::TableSetupColumn(TR("Sheet"), ImGuiTableColumnFlags_WidthFixed, S(90));
      if (!notes) ImGui::TableSetupColumn(TR("Name"), ImGuiTableColumnFlags_WidthFixed, S(80));
      ImGui::TableSetupColumn(TR("Cell"), ImGuiTableColumnFlags_WidthFixed, S(60));
      if (notes) {
        ImGui::TableSetupColumn(TR("Note"));
      } else {
        ImGui::TableSetupColumn(TR("Value"));
        ImGui::TableSetupColumn(TR("Formula"));
      }
      ImGui::TableHeadersRow();
      if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsDirty && ss->SpecsCount > 0) {
        const int col = ss->Specs[0].ColumnIndex;
        const bool desc = ss->Specs[0].SortDirection == ImGuiSortDirection_Descending;
        auto key = [&](const FindHit& h) -> std::string {
          char cell[24];
          std::snprintf(cell, sizeof(cell), "%010u%06u", h.r, h.c);
          if (col == 0) return h.sheet_name;
          if (notes) return col == 1 ? std::string(cell) : h.text;
          return col == 1 ? h.name : col == 2 ? std::string(cell) : col == 3 ? h.value : h.formula;
        };
        std::stable_sort(g_hits.begin(), g_hits.end(), [&](const FindHit& a, const FindHit& b) { return desc ? key(b) < key(a) : key(a) < key(b); });
        g_hit_sel = -1;
        ss->SpecsDirty = false;
      }
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(g_hits.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const FindHit& h = g_hits[static_cast<size_t>(i)];
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::PushID(i);
          if (ImGui::Selectable(h.sheet_name.c_str(), i == g_hit_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            g_hit_sel = i;
            GoToHit(s, h);
          }
          ImGui::PopID();
          if (!notes) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.name.c_str());
          }
          ImGui::TableNextColumn();
          const std::string addr = core::CellA1(h.r, h.c, true, true);
          ImGui::TextUnformatted(addr.c_str());
          // tests find a row by its sheet and cell, as a person reads it (the sheet's name alone is also a tab's)
          ui::NoteUiText((h.sheet_name + "!" + addr).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(notes ? h.text.c_str() : h.value.c_str());
          if (!notes) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.formula.c_str());
          }
        }
      }
      ImGui::EndTable();
    }
    // Ctrl+A in the list selects every found cell on the active sheet (Excel)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A) && !ImGui::GetIO().WantTextInput) {
      auto& v = s.FocusView();
      std::vector<Rect> areas;
      for (const FindHit& h : g_hits) {
        if (h.sheet == s.focus_sheet) areas.push_back(Rect::Cell(h.r, h.c));
      }
      if (!areas.empty()) {
        render::SelectCell(v, areas[0].r0, areas[0].c0);
        v.sel.areas = areas;
      }
    }
  }
  ImGui::End();
  if (close) {
    g_find_open = 0;
    g_hits.clear();
    g_hit_sel = -1;
  }
}

// ---- Go To ----
char g_goto[256] = "";
std::string g_goto_back;  // where the active cell was before the last Go To: the Reference box offers it (F5, Enter goes back)
std::vector<std::string> g_goto_recent;  // the places gone to before (Excel lists them under the names)
std::vector<std::string> g_goto_trace;   // a tracer arrow to another sheet was double-clicked: only its references
void DrawGoTo(AppState& s) {
  if (!BeginDialog(s, TR("Go To"), Dialog::GoTo, ImVec2(S(380), 0))) {
    if (s.dialog != Dialog::GoTo) g_goto_trace.clear();
    return;
  }
  // Go to: the defined names, then the places gone to before; a click fills Reference, a double-click goes
  ImGui::TextUnformatted(TR("Go to:"));
  std::vector<std::string> items;
  if (!g_goto_trace.empty()) {
    items = g_goto_trace;
  } else {
    for (const auto& n : *s.snap->wb.names) {
      if (!n.hidden) items.push_back(n.name);
    }
    for (const auto& r : g_goto_recent) {
      if (std::find(items.begin(), items.end(), r) == items.end()) items.push_back(r); // lesson-ok: find-push the defined names and recent Go To entries
    }
  }
  bool go = false;
  if (ImGui::BeginChild("##gotolist", ImVec2(-1, S(170)), ImGuiChildFlags_Borders)) {
    for (size_t i = 0; i < items.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(items[i].c_str(), items[i] == g_goto, ImGuiSelectableFlags_AllowDoubleClick)) {
        std::snprintf(g_goto, sizeof g_goto, "%s", items[i].c_str());
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = true;
      }
      ImGui::PopID();
    }
    if (items.empty()) ImGui::TextDisabled("%s", TR("(no names or earlier places yet)"));
  }
  ImGui::EndChild();
  ImGui::TextUnformatted(TR("Reference:"));
  if (ImGui::IsWindowAppearing()) {
    // Excel: the box holds where the last Go To started, selected, so F5 then Enter goes back and typing replaces it
    // (a trace arrow's Go To has put its own place there already)
    if (g_goto_trace.empty()) std::snprintf(g_goto, sizeof g_goto, "%s", g_goto_back.c_str());
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(-1);
  const bool enter = ImGui::InputTextWithHint("##goto", TR("A1, B2:D10, Sheet2!C3 or a name"), g_goto, sizeof(g_goto),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  if (ImGui::Button(TR("Special...")) || DlgKey(ImGuiKey_S)) {  // Alt+S from the reference box (Excel)
    EndDialog(s, true);
    s.OpenDialog(Dialog::GoToSpecial);
    return;
  }
  bool ok = false;
  bool close = OkCancel(&ok);
  if ((ok || enter || go) && g_goto[0]) {
    if (const model::Sheet* here = s.FocusSheet()) {
      // the whole selection, every area (Excel 16, measured: Application.PreviousSelections(1) after Goto from
      // B2:B8 is $B$2:$B$8, from A3:A4,A6,A8 all three areas), so F5, Enter selects again what was selected
      const bool other_sheet = std::strchr(g_goto, '!') != nullptr;
      const std::string prefix = other_sheet ? core::QuoteSheetName(here->name) + "!" : std::string();
      std::string back;
      for (const Rect& a : s.FocusView().sel.areas) {
        const bool whole_cols = a.r0 == 0 && a.r1 + 1 >= here->row_limit;
        const bool whole_rows = a.c0 == 0 && a.c1 + 1 >= here->col_limit;
        std::string t;
        if (whole_cols && !whole_rows) t = "$" + core::ColumnName(a.c0) + ":$" + core::ColumnName(a.c1);
        else if (whole_rows && !whole_cols) t = "$" + std::to_string(uint64_t{a.r0} + 1) + ":$" + std::to_string(uint64_t{a.r1} + 1);
        else t = core::CellA1(a.r0, a.c0, true, true) + (a.IsCell() ? "" : ":" + core::CellA1(a.r1, a.c1, true, true));
        back += (back.empty() ? "" : ",") + prefix + t;
      }
      // a selection too long for the box goes back to its active cell
      const auto& sel = s.FocusView().sel;
      g_goto_back = back.size() < sizeof g_goto ? back : prefix + core::CellA1(sel.act_r, sel.act_c, true, true);
    }
    Goto(s, g_goto);
    const std::string r = g_goto;
    g_goto_recent.erase(std::remove(g_goto_recent.begin(), g_goto_recent.end(), r), g_goto_recent.end());
    g_goto_recent.insert(g_goto_recent.begin(), r);
    if (g_goto_recent.size() > 8) g_goto_recent.resize(8);
    close = true;
  }
  if (close || enter || go) g_goto_trace.clear();
  EndDialog(s, close || enter || go);
}

// ---- Row height, column width ----
char g_dim_buf[32] = "";
Dialog g_dim_return = Dialog::None;  // the size dialog to bring back once the message about its value closes
bool g_dim_keep = false;             // ...with the text that was refused, to fix
int g_dim_focus = 0;                 // frames left of giving the box the keyboard again, its text selected

// the size box: focused when its dialog appears and again after a refused value (the text selected, so typing
// replaces it, as in Excel)
void SizeBox(char* buf, size_t cap) {
  if (g_dim_focus == 2) {
    ImGui::ClearActiveID();  // let go of the box, so that taking it again selects its text
    --g_dim_focus;
  } else if (ImGui::IsWindowAppearing() || g_dim_focus == 1) {
    ImGui::SetKeyboardFocusHere();
    g_dim_focus = 0;
  }
  ImGui::SetNextItemWidth(S(100));
  ImGui::InputText("##size", buf, cap, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
}

// a size typed in a dialog, checked as Excel does: not a number, or outside [0, max], is refused with its message and
// the dialog stays (in a window, the message replaces it and it comes back with the text)
bool RefuseSize(AppState& s, Dialog d, const char* buf, double max, const char* range_msg, double* value) {
  char* end = nullptr;
  *value = std::strtod(buf, &end);
  while (end && *end == ' ') ++end;
  const char* msg = (!buf[0] || end == buf || *end) ? TR("The number you typed is not valid.") : (*value < 0 || *value > max) ? range_msg : nullptr;
  if (!msg) return false;
  s.ShowError("YCGrid", msg);
  g_dim_focus = 2;
  if (s.dialog == Dialog::Message) {
    g_dim_return = d;
    g_dim_keep = true;
  }
  return true;
}

void DrawDim(AppState& s, bool rows) {
  const char* title = rows ? TR("Row Height") : TR("Column Width");
  const Dialog me = rows ? Dialog::RowHeight : Dialog::ColumnWidth;
  if (g_dim_return == me && s.dialog == Dialog::None) {  // the message closed: the dialog comes back
    g_dim_return = Dialog::None;
    s.OpenDialog(me);
  }
  if (s.dialog == me && s.dialog_open_request && g_dim_keep) {
    g_dim_keep = false;
  } else if (s.dialog == me && s.dialog_open_request) {
    // Excel units: row height in points, column width in characters of the default font (ColPxToChars). The box is
    // empty when the selected rows / columns don't all have the same size, and OK with it still empty changes nothing.
    g_dim_buf[0] = 0;
    if (const model::Sheet* sh = s.FocusSheet()) {
      const auto& sel = s.FocusView().sel;
      int32_t size = rows ? sh->rows->Size(sel.act_r) : sh->cols->Size(sel.act_c);
      bool same = true;
      for (const Rect& a : SelAreas(s)) {
        const uint32_t lo = rows ? a.r0 : a.c0, hi = rows ? a.r1 : a.c1;
        for (uint32_t k = lo; k <= hi && k - lo < 20000 && same; ++k) same = (rows ? sh->rows->Size(k) : sh->cols->Size(k)) == size;
      }
      if (same) std::snprintf(g_dim_buf, sizeof(g_dim_buf), "%.2f", rows ? size * 0.75 : render::ColPxToChars(size));
    }
  }
  if (!BeginDialog(s, title, rows ? Dialog::RowHeight : Dialog::ColumnWidth, ImVec2(S(240), 0))) return;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(rows ? TR("Row height:") : TR("Column width:"));
  ImGui::SameLine();
  SizeBox(g_dim_buf, sizeof(g_dim_buf));
  bool ok = false;
  bool close = OkCancel(&ok);
  double value = 0;
  if (ok && !g_dim_buf[0]) {
    ok = false;  // the box left empty (sizes that differ): OK changes nothing
  } else if (ok && RefuseSize(s, me, g_dim_buf, rows ? 409.0 : 255.0, rows ? TR("Row height must be between 0 and 409.") : TR("Column width must be between 0 and 255."), &value)) {
    ok = false;
    if (s.dialog != me) {  // the message took the dialog's place: this popup goes, the dialog comes back after it
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    close = false;  // without a window (hidden) the dialog stays with the text
  }
  if (ok && s.engine) {
    const auto areas = SelAreas(s);
    const uint32_t sheet = s.focus_sheet;
    const int32_t px = rows ? static_cast<int32_t>(std::round(value / 0.75)) : render::CharsToColPx(value);
    s.engine->PostUser(title, [=](Document& d) {
      for (const Rect& a : areas) {
        if (rows) d.SetRowHeight(sheet, a.r0, a.r1, px);
        else d.SetColWidth(sheet, a.c0, a.c1, px);
      }
    });
  }
  EndDialog(s, close);
}

// ---- Home > Format > Default Width ----
char g_std_buf[32] = "";
void DrawStandardWidth(AppState& s) {
  if (g_dim_return == Dialog::StandardWidth && s.dialog == Dialog::None) {
    g_dim_return = Dialog::None;
    s.OpenDialog(Dialog::StandardWidth);
  }
  if (s.dialog == Dialog::StandardWidth && s.dialog_open_request && g_dim_keep) {
    g_dim_keep = false;
  } else if (s.dialog == Dialog::StandardWidth && s.dialog_open_request) {
    const model::Sheet* sh = s.FocusSheet();
    std::snprintf(g_std_buf, sizeof(g_std_buf), "%.2f", sh ? render::ColPxToChars(sh->cols->default_size()) : 8.43);
  }
  if (!BeginDialog(s, TR("Standard Width"), Dialog::StandardWidth, ImVec2(S(280), 0))) return;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Standard column width:"));
  ImGui::SameLine();
  SizeBox(g_std_buf, sizeof(g_std_buf));
  bool ok = false;
  bool close = OkCancel(&ok);
  double value = 0;
  if (ok && RefuseSize(s, Dialog::StandardWidth, g_std_buf, 255.0, TR("Column width must be between 0 and 255."), &value)) {
    ok = false;
    if (s.dialog != Dialog::StandardWidth) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    close = false;
  }
  if (ok && s.engine && value > 0) {
    const uint32_t sheet = s.focus_sheet;
    const int32_t px = render::CharsToColPx(value);
    s.engine->PostUser(TR("Standard Width"), [=](Document& d) { d.SetStandardWidth(sheet, px); });
  }
  EndDialog(s, close);
}

// ---- Insert / delete cells ----
int g_ins_choice = 1;
void DrawInsDel(AppState& s, bool insert) {
  const char* title = insert ? TR("Insert") : TR("Delete");
  if (!BeginDialog(s, title, insert ? Dialog::InsertCells : Dialog::DeleteCells, ImVec2(S(240), 0))) return;
  // cells copied or cut: Excel's Insert Paste (the copied cells go in, the others shift right or down)
  const bool paste_in = insert && CopyModeOn(s);
  if (ImGui::IsWindowAppearing()) {
    // Excel's choice: a taller-than-wide selection shifts across, others shift down (Insert) / up (Delete)
    const Rect a = paste_in ? s.marquee_rect : SelRect(s);
    g_ins_choice = (a.r1 - a.r0) > (a.c1 - a.c0) ? 4 : 3;
  }
  if (paste_in) ImGui::TextUnformatted(TR("Insert Paste"));
  // Excel's letters: Shift cells r(I)ght / (L)eft, (D)own / (U)p, Entire (R)ow, Entire (C)olumn (Ctrl+-, R, Enter
  // deletes the row, as people type it)
  if (DlgKey(insert ? ImGuiKey_I : ImGuiKey_L)) g_ins_choice = 4;
  if (DlgKey(insert ? ImGuiKey_D : ImGuiKey_U)) g_ins_choice = 3;
  if (!paste_in && DlgKey(ImGuiKey_R)) g_ins_choice = 1;
  if (!paste_in && DlgKey(ImGuiKey_C)) g_ins_choice = 2;
  ImGui::RadioButton(insert ? TR("Shift cells right") : TR("Shift cells left"), &g_ins_choice, 4);
  ImGui::RadioButton(insert ? TR("Shift cells down") : TR("Shift cells up"), &g_ins_choice, 3);
  if (paste_in && g_ins_choice != 3 && g_ins_choice != 4) g_ins_choice = 3;
  if (!paste_in) {
    ImGui::RadioButton(TR("Entire row"), &g_ins_choice, 1);
    ImGui::RadioButton(TR("Entire column"), &g_ins_choice, 2);
  }
  bool ok = false;
  const bool close = OkCancel(&ok);
  if (ok) InsertOrDelete(s, insert, g_ins_choice);
  EndDialog(s, close);
}

// ---- Unhide sheet, Move or Copy ----
int g_unhide_sel = 0;
void DrawUnhideSheet(AppState& s) {
  if (!BeginDialog(s, TR("Unhide"), Dialog::UnhideSheet, ImVec2(S(320), 0))) return;
  if (ImGui::IsWindowAppearing()) g_unhide_sel = 0;
  std::vector<const model::Sheet*> hidden;
  for (const auto& sh : s.snap->wb.sheets) {
    if (!sh->visible && !sh->very_hidden) hidden.push_back(&sh.get());  // a very hidden sheet isn't offered (Excel)
  }
  ImGui::TextUnformatted(TR("Unhide sheet:"));
  bool go = false;
  if (ImGui::BeginChild("##hidden", ImVec2(-1, S(170)), ImGuiChildFlags_Borders)) {
    for (size_t i = 0; i < hidden.size(); ++i) {
      if (ImGui::Selectable(hidden[i]->name.c_str(), g_unhide_sel == static_cast<int>(i), ImGuiSelectableFlags_AllowDoubleClick)) {
        g_unhide_sel = static_cast<int>(i);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = true;
      }
    }
  }
  ImGui::EndChild();
  bool ok = false;
  bool close = OkCancel(&ok);
  if ((ok || go) && g_unhide_sel >= 0 && g_unhide_sel < static_cast<int>(hidden.size())) {
    const uint32_t id = hidden[static_cast<size_t>(g_unhide_sel)]->id;
    s.engine->PostUser(TR("Unhide Sheet"), [id](Document& d) { d.SetSheetVisible(id, true); });
    s.engine->Sync();
    s.snap = s.engine->Current();
    ActivateSheet(s, id);
    close = true;
  }
  if (hidden.empty()) close = true;
  EndDialog(s, close);
}

int g_move_before = 0;
bool g_move_copy = false;
void DrawMoveCopySheet(AppState& s) {
  if (!BeginDialog(s, TR("Move or Copy"), Dialog::MoveCopySheet, ImVec2(S(340), 0))) return;
  const auto& sheets = s.snap->wb.sheets;
  const uint32_t id = s.focus_sheet;
  const int cur = s.snap->wb.IndexOfId(id);
  if (ImGui::IsWindowAppearing()) {
    g_move_copy = false;
    g_move_before = std::min(cur + 1, static_cast<int>(sheets.size()));  // the sheet after it (Excel's start)
  }
  const model::Sheet* me = s.snap->Sheet(id);
  ImGui::Text(TR("Move selected sheets: %s"), me ? me->name.c_str() : "");
  ImGui::TextUnformatted(TR("Before sheet:"));
  bool go = false;
  if (ImGui::BeginChild("##before", ImVec2(-1, S(180)), ImGuiChildFlags_Borders)) {
    for (size_t i = 0; i <= sheets.size(); ++i) {
      if (i < sheets.size() && !sheets[i]->visible) continue;
      const std::string label = i < sheets.size() ? sheets[i]->name : std::string(TR("(move to end)"));
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(label.c_str(), g_move_before == static_cast<int>(i), ImGuiSelectableFlags_AllowDoubleClick)) {
        g_move_before = static_cast<int>(i);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) go = true;
      }
      if (ImGui::IsWindowAppearing() && g_move_before == static_cast<int>(i)) ImGui::SetScrollHereY();  // the choice in view
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  if (DlgKey(ImGuiKey_C)) g_move_copy = !g_move_copy;  // Excel 16's letter (UI Automation's AccessKey)
  ImGui::Checkbox(TR("Create a copy"), &g_move_copy);
  bool ok = false;
  bool close = OkCancel(&ok);
  if (ok || go) {
    const int before = g_move_before;
    const bool copy = g_move_copy;
    const uint32_t made = s.engine->Call([&](Document& d) {
      d.Begin(copy ? TR("Copy Sheet") : TR("Move Sheet"));
      uint32_t v = id;
      if (copy) {
        v = d.CopySheet(id, before);
      } else {
        // the index after it leaves its own place
        const int to = before > cur ? before - 1 : before;
        if (to != cur) d.MoveSheet(id, to);
      }
      d.Commit();
      return v;
    });
    s.engine->Sync();
    s.snap = s.engine->Current();
    if (made) ActivateSheet(s, made);
    close = true;
  }
  EndDialog(s, close);
}

// ---- Paste Special ----
int g_ps_mode = 0, g_ps_op = 0;
bool g_ps_transpose = false, g_ps_skip = false;
// Paste Special with text from another program on the Clipboard (no copied cells): Excel's "As" list instead of the
// choices for cells
void DrawPasteSpecialText(AppState& s) {
  static int as = 0;
  if (!BeginDialog(s, TR("Paste Special###yc_ps_text"), Dialog::PasteSpecial, ImVec2(S(420), 0))) return;
  if (ImGui::IsWindowAppearing()) as = 0;
  ImGui::TextUnformatted(TR("Source: text from another program"));
  ImGui::Spacing();
  ImGui::RadioButton(TR("Paste"), true);
  ImGui::Spacing();
  ImGui::TextUnformatted(TR("As:"));
  const char* kinds[] = {TR("Unicode Text"), TR("Text")};
  if (ImGui::BeginListBox("##ps_as", ImVec2(S(240), ImGui::GetTextLineHeightWithSpacing() * 3))) {
    for (int i = 0; i < 2; ++i) {
      if (ImGui::Selectable(kinds[i], as == i)) as = i;
    }
    ImGui::EndListBox();
  }
  ImGui::TextDisabled("%s", TR("Inserts the contents of the Clipboard as text, one cell per tab-separated field."));
  bool ok = false;
  const bool close = OkCancel(&ok);
  if (ok) PasteClipboard(s);  // the text goes into the cells as typed, one field per cell (both kinds)
  EndDialog(s, close || ok);
}

void DrawPasteSpecial(AppState& s) {
  // text from another program: its own list (Excel); decided as the dialog opens
  static bool as_text = false;
  if (s.dialog == Dialog::PasteSpecial && s.dialog_open_request) as_text = !CopyModeOn(s) && ClipboardHasText(s.hwnd);
  if (as_text) {
    DrawPasteSpecialText(s);
    return;
  }
  if (!BeginDialog(s, TR("Paste Special"), Dialog::PasteSpecial, ImVec2(S(520), 0))) return;
  if (ImGui::IsWindowAppearing()) g_ps_mode = g_ps_op = 0, g_ps_skip = g_ps_transpose = false;  // opens on All and None, like Excel
  // Excel's choices in its two columns, each with its letter (the dialog has no text box, so a plain letter or
  // Alt+letter chooses)
  struct Choice {
    const char* label;
    ImGuiKey key;
    engine::PasteMode mode;
  };
  const Choice kChoices[] = {
      {TR("All"), ImGuiKey_A, engine::PasteMode::All},
      {TR("Formulas"), ImGuiKey_F, engine::PasteMode::Formulas},
      {TR("Values"), ImGuiKey_V, engine::PasteMode::Values},
      {TR("Formats"), ImGuiKey_T, engine::PasteMode::Formats},
      {TR("Comments and Notes"), ImGuiKey_C, engine::PasteMode::Comments},
      {TR("Validation"), ImGuiKey_N, engine::PasteMode::Validation},
      {TR("All using Source theme"), ImGuiKey_H, engine::PasteMode::AllSourceTheme},
      {TR("All except borders"), ImGuiKey_X, engine::PasteMode::NoBorders},
      {TR("Column widths"), ImGuiKey_W, engine::PasteMode::ColumnWidths},
      {TR("Formulas and number formats"), ImGuiKey_R, engine::PasteMode::FormulasAndFormats},
      {TR("Values and number formats"), ImGuiKey_U, engine::PasteMode::ValuesAndFormats},
      {TR("All merging conditional formats"), ImGuiKey_G, engine::PasteMode::AllMergingCf},
  };
  constexpr int kCount = static_cast<int>(sizeof(kChoices) / sizeof(kChoices[0]));
  const bool ctrl = ImGui::GetIO().KeyCtrl;
  for (int i = 0; i < kCount; ++i) {
    if (ImGui::IsKeyPressed(kChoices[i].key, false) && !ctrl) g_ps_mode = i;
  }
  // Operation: None, Add, Subtract | Multiply, Divide
  static const ImGuiKey kOpKeys[] = {ImGuiKey_O, ImGuiKey_D, ImGuiKey_S, ImGuiKey_M, ImGuiKey_I};
  for (int i = 0; i < 5; ++i) {
    if (ImGui::IsKeyPressed(kOpKeys[i], false) && !ctrl) g_ps_op = i;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_B, false) && !ctrl) g_ps_skip = !g_ps_skip;
  if (ImGui::IsKeyPressed(ImGuiKey_E, false) && !ctrl) g_ps_transpose = !g_ps_transpose;
  g_ps_mode = std::clamp(g_ps_mode, 0, kCount - 1);
  const engine::PasteMode mode = kChoices[g_ps_mode].mode;
  // formats, widths, notes and validation have no numbers to combine
  const bool no_op = mode == engine::PasteMode::Formats || mode == engine::PasteMode::ColumnWidths || mode == engine::PasteMode::Comments ||
                     mode == engine::PasteMode::Validation;
  ImGui::SeparatorText(TR("Paste"));
  if (ImGui::BeginTable("##ps", 2)) {
    for (int row = 0; row < kCount / 2; ++row) {
      for (int col = 0; col < 2; ++col) {
        ImGui::TableNextColumn();
        const int i = col * (kCount / 2) + row;
        ImGui::RadioButton(kChoices[i].label, &g_ps_mode, i);
      }
    }
    ImGui::EndTable();
  }
  ImGui::SeparatorText(TR("Operation"));
  const char* ops[] = {TR("None"), TR("Add"), TR("Subtract"), TR("Multiply"), TR("Divide")};
  ImGui::BeginDisabled(no_op);
  if (ImGui::BeginTable("##ps_op", 2)) {
    static const int kOrder[] = {0, 3, 1, 4, 2};  // Excel: None | Multiply, Add | Divide, Subtract
    for (int k : kOrder) {
      ImGui::TableNextColumn();
      ImGui::PushID(100 + k);
      ImGui::RadioButton(ops[k], &g_ps_op, k);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::Checkbox(TR("Skip blanks"), &g_ps_skip);
  ImGui::SameLine(S(260));
  ImGui::Checkbox(TR("Transpose"), &g_ps_transpose);
  ImGui::Spacing();
  // Paste Link: formulas that refer to the copied cells (Excel's button at the lower left)
  const bool can_link = s.has_clip && !s.clip->cut;
  ImGui::BeginDisabled(!can_link);
  bool link = ImGui::Button(TR("Paste Link"));
  ImGui::EndDisabled();
  if (ImGui::IsKeyPressed(ImGuiKey_L, false) && !ctrl && can_link) link = true;
  ImGui::SameLine();
  bool ok = false;
  bool close = OkCancel(&ok);
  if (link) {
    PasteLink(s);
    close = true;
  }
  if (ok) {
    const auto op = no_op ? engine::PasteOp::None : static_cast<engine::PasteOp>(g_ps_op);
    PasteClipboard(s, mode, g_ps_transpose, g_ps_skip, op);
  }
  EndDialog(s, close);
}

// ---- Tab color ----
// Excel's palette: No Color, the theme colors with their shades (kept as theme colors), the standard colors, More Colors
void DrawTabColor(AppState& s) {
  if (!BeginDialog(s, TR("Tab Color"), Dialog::TabColor, ImVec2(S(260), 0))) return;
  bool close = false;
  uint32_t ref = 0;
  const uint32_t id = s.focus_sheet;
  if (ui::ribbon::PaletteSwatches(&ref, TR("No Color")) && s.engine) {
    s.engine->PostUser(TR("Tab Color"), [id, ref](Document& d) { d.SetTabColor(id, ref); });
    close = true;
  }
  if (ImGui::TreeNode(TR("More Colors..."))) {
    static ImVec4 more(1, 0, 0, 1);
    ImGui::SetNextItemWidth(S(200));
    ImGui::ColorPicker3("##more", &more.x, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_DisplayHex);
    if (ImGui::Button(TR("OK")) && s.engine) {
      const uint32_t c = VecToArgb(ImVec4(more.x, more.y, more.z, 1));
      s.engine->PostUser(TR("Tab Color"), [id, c](Document& d) { d.SetTabColor(id, c); });
      close = true;
    }
    ImGui::TreePop();
  }
  EndDialog(s, close);
}

// ---- Zoom ----
void DrawZoom(AppState& s) {
  if (!BeginDialog(s, TR("Zoom"), Dialog::ZoomDialog, ImVec2(S(240), 0))) return;
  static int pct = 100;
  static bool fit = false;  // Excel's "Fit selection": the zoom that shows the selected cells
  if (ImGui::IsWindowAppearing()) pct = static_cast<int>(s.FocusView().zoom * 100 + 0.5f), fit = false;
  // Excel 16's letters (UI Automation's AccessKey of its Zoom dialog): 0 200%, 1 100%, 7 75%, 5 50%, 2 25%, F Fit
  // selection, C the Custom box
  static const std::pair<int, ImGuiKey> kZooms[] = {{200, ImGuiKey_0}, {100, ImGuiKey_1}, {75, ImGuiKey_7}, {50, ImGuiKey_5}, {25, ImGuiKey_2}};
  for (const auto& [z, key] : kZooms) {
    char label[16];
    std::snprintf(label, sizeof(label), "%d%%", z);
    if (ImGui::RadioButton(label, !fit && pct == z) || DlgKey(key)) pct = z, fit = false;
  }
  if (ImGui::RadioButton(TR("Fit selection"), fit) || DlgKey(ImGuiKey_F)) fit = true;
  if (DlgKey(ImGuiKey_C)) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(S(100));
  if (ImGui::InputInt(TR("Custom %"), &pct)) fit = false;
  pct = std::clamp(pct, 10, 400);
  bool ok = false;
  const bool close = OkCancel(&ok);
  if (ok && fit) ui::Commands().Execute("ZoomToSelection");
  else if (ok) s.FocusView().zoom = pct / 100.0f;
  EndDialog(s, close);
}

// ---- Insert Function ----
// insert the chosen function: at the caret while editing, otherwise start editing with =FN(); caret goes inside the parentheses
// ---- Function Arguments (Excel's: a box per argument, its value beside it, the formula's result below) ----
struct FnArgsDlg {
  std::string fn;
  std::vector<std::string> names;  // the arguments' names ("[number2]" without the brackets)
  std::vector<bool> optional;
  int group = 0;                   // the arguments repeated after "..." (number2 -> number3...): how many repeat
  std::vector<std::string> vals;
  bool replace_cell = false;       // the cell's whole formula is this call (fx on =SUM(...)): OK writes the cell
  bool focus = false;
  // the call is a part of a formula (Excel: fx with the caret inside SUM of =ROUND(SUM(A1:A3),0), or not editing,
  // the first call of =SUM(A1:A3)*2): OK puts the call back over [span_begin, span_end) of the text being edited,
  // or of cell_formula (written to the cell)
  int span_begin = -1, span_end = -1;
  std::string cell_formula;
  int focus_arg = 0;  // the box the keyboard is in (its argument's description is shown)
};
FnArgsDlg g_fa;
// Insert Function's Most Recently Used: kept in the settings across sessions, as Excel does (Excel's list to start)
std::vector<std::string>& g_fn_recent = Config().recent_functions;
// fx on a cell that wasn't being edited: the cell shows "=" while Insert Function is open, then the call being built
// (Excel); OK writes it, Cancel takes the "=" away
bool g_fx_edit = false;

void NoteRecentFunction(const std::string& fn) {
  g_fn_recent.erase(std::remove(g_fn_recent.begin(), g_fn_recent.end(), fn), g_fn_recent.end());
  g_fn_recent.insert(g_fn_recent.begin(), fn);
  if (g_fn_recent.size() > 10) g_fn_recent.resize(10);
  SaveSettings();
  UpdateFunctionLibraryMenu();
}

// "number1, [number2], ..." -> names, optional flags, how many repeat
void SplitArgs(const char* args, FnArgsDlg* d) {
  d->names.clear();
  d->optional.clear();
  d->group = 0;
  std::string a = args ? args : "";
  std::vector<std::string> parts;
  std::string cur;
  int depth = 0;
  for (char ch : a) {
    if (ch == '[') ++depth;
    if (ch == ']') --depth;
    if (ch == ',' && depth == 0) {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) parts.push_back(cur);
  bool variadic = false;
  for (std::string p : parts) {
    while (!p.empty() && p.front() == ' ') p.erase(0, 1);
    while (!p.empty() && p.back() == ' ') p.pop_back();
    if (p == "..." || p.empty()) {
      variadic = variadic || p == "...";
      continue;
    }
    const bool opt = p.front() == '[';
    p.erase(std::remove(p.begin(), p.end(), '['), p.end());
    p.erase(std::remove(p.begin(), p.end(), ']'), p.end());
    // "[criteria_range2, criteria2]" in one bracket: two names
    size_t from = 0;
    while (from < p.size()) {
      size_t c = p.find(',', from);
      std::string n = p.substr(from, c == std::string::npos ? std::string::npos : c - from);
      while (!n.empty() && n.front() == ' ') n.erase(0, 1);
      if (!n.empty()) {
        d->names.push_back(n);
        d->optional.push_back(opt);
      }
      if (c == std::string::npos) break;
      from = c + 1;
    }
  }
  if (variadic) {
    // the names ending in 2 repeat (number2 -> number3; criteria_range2, criteria2 -> ...3)
    for (size_t k = d->names.size(); k > 0 && !d->names[k - 1].empty() && d->names[k - 1].back() == '2'; --k) ++d->group;
    if (d->group == 0) d->group = 1;
  }
}

// a name with its number one higher (number2 -> number3)
std::string NextName(const std::string& n) {
  size_t k = n.size();
  while (k > 0 && std::isdigit(static_cast<unsigned char>(n[k - 1]))) --k;
  if (k == n.size()) return n;
  return n.substr(0, k) + std::to_string(std::atoi(n.c_str() + k) + 1);
}

// the arguments of a call written as text: commas outside quotes, parentheses and braces
std::vector<std::string> SplitCallArgs(std::string_view body) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool q = false;
  for (char ch : body) {
    if (ch == '"') q = !q;
    if (!q && (ch == '(' || ch == '{')) ++depth;
    if (!q && (ch == ')' || ch == '}')) --depth;
    if (!q && depth == 0 && ch == ',') {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += ch;
  }
  if (!cur.empty() || !out.empty()) out.push_back(cur);
  for (auto& a : out) {
    while (!a.empty() && a.front() == ' ') a.erase(0, 1);
    while (!a.empty() && a.back() == ' ') a.pop_back();
  }
  return out;
}

std::string CallText(const FnArgsDlg& d) {
  size_t n = d.vals.size();
  while (n > 0 && d.vals[n - 1].empty() && (n - 1 >= d.optional.size() || d.optional[n - 1])) --n;  // empty optional ones at the end
  std::string t = d.fn + "(";
  for (size_t i = 0; i < n; ++i) t += (i ? "," : "") + d.vals[i];
  return t + ")";
}

// a box's value, shown beside it (worked out on the engine thread; kept until the workbook changes)
std::string ArgValue(AppState& s, const std::string& expr) {
  if (expr.empty()) return {};
  static std::map<std::string, std::pair<uint64_t, std::string>> cache;
  auto it = cache.find(expr);
  if (it != cache.end() && it->second.first == s.snap->version) return it->second.second;
  const uint32_t sheet = s.focus_sheet;
  std::string shown;
  uint32_t sid = 0;
  std::vector<Rect> areas;
  if (ResolveAreasText(s.snap->wb, sheet, expr, &sid, &areas) && s.snap->Sheet(sid)) {
    // a range: its first values, as Excel shows them ({1;2;3;...})
    const model::Sheet* sh = s.snap->Sheet(sid);
    const auto& tbl = *s.snap->wb.styles;
    const Rect& r = areas[0];
    if (areas.size() == 1 && r.IsCell()) {
      shown = engine::DisplayTextOf(sh->GetValue(r.r0, r.c0), tbl.NumberFormat(tbl.Get(sh->GetStyle(r.r0, r.c0))));
    } else {
      int k = 0;
      shown = "{";
      for (uint32_t row = r.r0; row <= r.r1 && k < 8; ++row) {
        for (uint32_t col = r.c0; col <= r.c1 && k < 8; ++col, ++k) {
          const core::Value v = sh->GetValue(row, col);
          const std::string tx = engine::DisplayTextOf(v, "General");
          shown += (col > r.c0 ? "," : row > r.r0 ? ";" : "") + (v.IsString() ? "\"" + tx + "\"" : tx);
        }
      }
      shown += r.Count() > 8 || areas.size() > 1 ? ";...}" : "}";
    }
  } else {
    shown = s.engine->Call([&](Document& d) {
      std::string err;
      const core::Value v = d.EvaluateFormula(sheet, expr, &err);
      if (!err.empty()) return std::string(TR("Invalid"));
      return v.IsString() ? "\"" + engine::DisplayTextOf(v, "General") + "\"" : engine::DisplayTextOf(v, "General");
    });
  }
  if (cache.size() > 400) cache.clear();
  cache[expr] = {s.snap->version, shown};
  return shown;
}

}  // namespace

const std::vector<std::string>& RecentFunctions() { return g_fn_recent; }

void OpenGoToWith(AppState& s, std::vector<std::string> refs) {
  g_goto_trace = std::move(refs);
  std::snprintf(g_goto, sizeof g_goto, "%s", g_goto_trace.empty() ? "" : g_goto_trace.front().c_str());
  s.OpenDialog(Dialog::GoTo);
}

void BeginFunctionEdit(AppState& s) {
  if (s.edit.active) return;
  StartEdit(s, true, "=");
  g_fx_edit = s.edit.active;
}

// Formulas > Recently Used: the functions used last (the ribbon's menus are rebuilt when it is customized)
void UpdateFunctionLibraryMenu() {
  std::vector<std::string> menu;
  // nothing used yet: Excel's starting list
  static const std::vector<std::string> kStart = {"SUM", "AVERAGE", "IF", "HYPERLINK", "COUNT", "MAX", "SIN", "SUMIF", "PMT", "STDEV"};
  for (const std::string& fn : g_fn_recent.empty() ? kStart : g_fn_recent) {
    if (formula::FindFuncHelp(fn)) menu.push_back("InsertFn_" + fn);
  }
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "FunctionsRecentlyUsed" && it.menu != menu) it.menu = menu;
        }
      }
    }
  }
}

// Formulas > Function Library: a menu of each category's functions (built by the ribbon from the help table); an
// entry opens Function Arguments for its function, as in Excel (OK puts the call in the cell, or at the caret)
void RegisterFunctionLibrary(AppState& s) {
  auto& R = ui::Commands();
  auto head = [&](const char* id, const char* label, const char* ic, const char* tip) {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = ic;
    c.tip_desc = tip;
    R.Add(std::move(c)).execute = [](ui::Command&) {};
  };
  head("FunctionsRecentlyUsed", TR("Recently\nUsed"), ui::icon::History, TR("Browse and select from a list of recently used functions."));
  head("FunctionsFinancial", TR("Financial"), ui::icon::Currency, TR("Browse and select from a list of financial functions."));
  head("FunctionsLogical", TR("Logical"), ui::icon::Check, TR("Browse and select from a list of logical functions."));
  head("FunctionsText", TR("Text"), ui::icon::TextBox, TR("Browse and select from a list of text functions."));
  head("FunctionsDateTime", TR("Date &\nTime"), ui::icon::Timeline, TR("Browse and select from a list of date and time functions."));
  head("FunctionsLookup", TR("Lookup &\nReference"), ui::icon::Search, TR("Browse and select from a list of lookup and reference functions."));
  head("FunctionsMath", TR("Math &\nTrig"), ui::icon::MathTrig, TR("Browse and select from a list of math and trigonometry functions."));
  head("FunctionsMore", TR("More\nFunctions"), ui::icon::MoreFunctions, TR("Browse and select from lists of statistical, engineering, information and other functions."));
  head("FunctionsStatistical", TR("Statistical"), nullptr, "");
  head("FunctionsEngineering", TR("Engineering"), nullptr, "");
  head("FunctionsInformation", TR("Information"), nullptr, "");
  head("FunctionsCompatibility", TR("Compatibility"), nullptr, "");
  head("FunctionsDatabase", TR("Database"), nullptr, "");
  head("FunctionsWeb", TR("Web"), nullptr, "");
  for (const auto& h : formula::AllFuncHelp()) {
    ui::Command c;
    c.id = std::string("InsertFn_") + h.name;
    c.label = h.name;
    c.tip_title = std::string(h.name) + "(" + h.args + ")";
    c.tip_desc = h.desc;
    const std::string fn = h.name;
    R.Add(std::move(c)).execute = [&s, fn](ui::Command&) {
      BeginFunctionEdit(s);
      OpenFunctionArgs(s, fn, {}, false);
    };
  }
}

void OpenFunctionArgs(AppState& s, const std::string& fn, const std::vector<std::string>& initial, bool replace_cell) {
  const formula::FuncHelp* h = formula::FindFuncHelp(fn);
  g_fa = FnArgsDlg{};
  g_fa.fn = fn;
  g_fa.replace_cell = replace_cell;
  SplitArgs(h ? h->args : "", &g_fa);
  // the boxes: every named argument, more for the values given, and a free one after them when it repeats
  while (g_fa.names.size() < initial.size() && g_fa.group > 0) {
    const size_t n = g_fa.names.size();
    for (int k = g_fa.group; k > 0; --k) {
      g_fa.names.push_back(NextName(g_fa.names[n - static_cast<size_t>(k)]));
      g_fa.optional.push_back(true);
    }
  }
  g_fa.vals = initial;
  g_fa.vals.resize(std::max(g_fa.names.size(), initial.size()));
  g_fa.focus = true;
  NoteRecentFunction(fn);
  s.OpenDialog(Dialog::FunctionArgs);
}

namespace {

bool IsFnChar(unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '.' || ch >= 0x80; }

// the end of the call whose "(" is at open: past its ")" (or the end of an unfinished formula)
size_t CallEnd(const std::string& t, size_t open, bool* closed) {
  int depth = 0;
  bool q = false;
  for (size_t i = open; i < t.size(); ++i) {
    if (t[i] == '"') q = !q;
    if (q) continue;
    if (t[i] == '(') ++depth;
    if (t[i] == ')' && --depth == 0) {
      *closed = true;
      return i + 1;
    }
  }
  *closed = false;
  return t.size();
}

// a known function's call in a formula's text: the innermost one around pos (or a function name just typed before
// it), or with pos < 0 the first one; its name, where it is ([b, e)) and its arguments as written
bool FindCall(const std::string& t, int pos, std::string* name, int* b, int* e, std::vector<std::string>* args) {
  struct Frame {
    size_t name_b, open;
  };
  std::vector<Frame> stack;
  bool q = false, sq = false;
  const size_t end = pos < 0 ? t.size() : std::min(t.size(), static_cast<size_t>(pos));
  auto known = [&](size_t nb, size_t ne, std::string* out) {
    std::string n = t.substr(nb, ne - nb);
    for (char& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (n.empty() || !formula::FindFuncHelp(n)) return false;
    *out = n;
    return true;
  };
  auto take = [&](size_t nb, size_t open) {
    bool closed = false;
    const size_t ce = CallEnd(t, open, &closed);
    *b = static_cast<int>(nb);
    *e = static_cast<int>(ce);
    *args = SplitCallArgs(std::string_view(t).substr(open + 1, (closed ? ce - 1 : ce) - open - 1));
    return true;
  };
  for (size_t i = 1; i < end; ++i) {
    const char ch = t[i];
    if (q) {
      q = ch != '"';
      continue;
    }
    if (sq) {
      sq = ch != '\'';
      continue;
    }
    if (ch == '"') q = true;
    else if (ch == '\'') sq = true;
    else if (ch == '(') {
      size_t j = i;
      while (j > 1 && IsFnChar(static_cast<unsigned char>(t[j - 1]))) --j;
      std::string n;
      if (pos < 0 && known(j, i, &n)) {
        *name = n;
        return take(j, i);
      }
      stack.push_back({j, i});
    } else if (ch == ')' && !stack.empty()) {
      stack.pop_back();
    }
  }
  if (pos < 0) return false;
  // a function's name just typed before the caret (=SUM, or =SUM( )
  size_t j = end;
  const bool paren = end < t.size() && t[end] == '(';
  while (j > 1 && IsFnChar(static_cast<unsigned char>(t[j - 1]))) --j;
  if (j < end && known(j, end, name)) {
    if (paren) return take(j, end);
    *b = static_cast<int>(j);
    *e = static_cast<int>(end);
    args->clear();
    return true;
  }
  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    if (known(it->name_b, it->open, name)) return take(it->name_b, it->open);
  }
  return false;
}

}  // namespace

bool OpenFunctionArgsForCell(AppState& s) {
  if (!s.snap) return false;
  if (s.edit.active) {
    // the call around the caret (Excel: fx, Shift+F3 or Ctrl+A with the caret in SUM of =ROUND(SUM(A1:A3)
    std::string name;
    int b = 0, e = 0;
    std::vector<std::string> args;
    if (!formula::LooksLikeFormula(s.edit.text) || !FindCall(s.edit.text, s.edit.caret, &name, &b, &e, &args)) return false;
    OpenFunctionArgs(s, name, args, false);
    g_fa.span_begin = b;
    g_fa.span_end = e;
    return true;
  }
  const auto& v = s.FocusView().sel;
  const std::string f = engine::FormulaTextOf(s.snap->wb, s.focus_sheet, v.act_r, v.act_c, false);
  // the formula's first call (Excel: =SUM(A1:A3)*2 opens SUM); a formula that is that one call is written whole
  if (f.size() < 4 || f[0] != '=') return false;
  std::string name;
  int b = 0, e = 0;
  std::vector<std::string> args;
  if (!FindCall(f, -1, &name, &b, &e, &args)) return false;
  const bool whole = b == 1 && e == static_cast<int>(f.size());
  OpenFunctionArgs(s, name, args, whole);
  if (!whole) {
    g_fa.span_begin = b;
    g_fa.span_end = e;
    g_fa.cell_formula = f;
  }
  return true;
}

namespace {

// "Help on this function" (Insert Function, Function Arguments): the function's help shown in the dialog, its syntax
// and what each argument expects (Excel opens its help page for the function)
void FunctionHelpLink(const std::string& fn, bool* shown) {
  if (ImGui::TextLink(TR("Help on this function"))) *shown = !*shown;
  const formula::FuncHelp* h = *shown ? formula::FindFuncHelp(fn) : nullptr;
  if (!h) return;
  ImGui::BeginChild("##fnhelp", ImVec2(0, S(120)), ImGuiChildFlags_Borders);
  ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
  if (ui::F().ui_bold) ImGui::PushFont(ui::F().ui_bold, 0.0f);
  ImGui::Text("%s(%s)", h->name, h->args);
  if (ui::F().ui_bold) ImGui::PopFont();
  ImGui::TextWrapped("%s", h->desc);
  // each argument and what it expects
  std::string args = h->args;
  size_t p = 0;
  while (p < args.size()) {
    size_t q = args.find(',', p);
    if (q == std::string::npos) q = args.size();
    std::string a = args.substr(p, q - p);
    p = q + 1;
    while (!a.empty() && (a.front() == ' ' || a.front() == '[')) a.erase(0, 1);
    while (!a.empty() && (a.back() == ' ' || a.back() == ']')) a.pop_back();
    if (a.empty() || a == "...") continue;
    const char* what = formula::FuncArgHelp(h->name, a);
    if (*what) ImGui::TextWrapped("%s: %s", a.c_str(), what);
  }
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
}

void DrawFunctionArgs(AppState& s) {
  // fx's "=" goes when its dialogs close without OK (Escape, the close box), as in Excel
  if (g_fx_edit && s.dialog != Dialog::FunctionArgs && s.dialog != Dialog::InsertFunction) {
    g_fx_edit = false;
    if (s.edit.active) CancelEdit(s);
  }
  if (!BeginDialog(s, TR("Function Arguments"), Dialog::FunctionArgs, ImVec2(S(560), 0))) return;
  FnArgsDlg& d = g_fa;
  const formula::FuncHelp* h = formula::FindFuncHelp(d.fn);
  ImGui::TextUnformatted(d.fn.c_str());
  ImGui::Separator();
  // one row per argument: its name (bold when required), the box (cells picked with its button), its value
  const float label_w = S(140), box_w = S(250);
  if (ImGui::BeginChild("##args", ImVec2(0, std::min(S(300), (ImGui::GetFrameHeightWithSpacing() + S(4)) * static_cast<float>(d.names.size()) + S(8))), ImGuiChildFlags_None)) {
    for (size_t i = 0; i < d.names.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::AlignTextToFramePadding();
      if (!d.optional[i] && ui::F().ui_bold) ImGui::PushFont(ui::F().ui_bold, 0.0f);
      ImGui::TextUnformatted(d.names[i].c_str());
      if (!d.optional[i] && ui::F().ui_bold) ImGui::PopFont();
      ImGui::SameLine(label_w);
      if (d.focus && i == 0) {
        ImGui::SetKeyboardFocusHere();
        d.focus = false;
      }
      ImGui::BeginGroup();
      RangeBox(s, ("fnarg." + std::to_string(i)).c_str(), &d.vals[i], box_w, RangeBox_Formula | RangeBox_Relative, d.names[i].c_str());
      ImGui::EndGroup();
      if (ImGui::IsItemActive()) d.focus_arg = static_cast<int>(i);  // the box with the keyboard: its description shows below
      ImGui::SameLine();
      const std::string v = ArgValue(s, d.vals[i]);
      ImGui::TextDisabled("= %s", v.empty() ? (d.optional[i] ? TR("(optional)") : "") : v.c_str());
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  // a repeating argument: a new box once the last one is filled
  if (d.group > 0 && !d.vals.empty() && !d.vals.back().empty() && d.names.size() < 255) {
    const size_t n = d.names.size();
    for (int k = d.group; k > 0; --k) {
      d.names.push_back(NextName(d.names[n - static_cast<size_t>(k)]));
      d.optional.push_back(true);
      d.vals.emplace_back();
    }
  }
  ImGui::Spacing();
  if (h) {
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("%s", h->desc);
    // Excel: under it, the argument whose box has the keyboard, in bold, with what it expects
    if (d.focus_arg >= 0 && d.focus_arg < static_cast<int>(d.names.size())) {
      std::string an = d.names[static_cast<size_t>(d.focus_arg)];
      if (!an.empty()) an[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(an[0])));
      const char* what = formula::FuncArgHelp(d.fn, d.names[static_cast<size_t>(d.focus_arg)]);
      ImGui::Spacing();
      if (ui::F().ui_bold) ImGui::PushFont(ui::F().ui_bold, 0.0f);
      ImGui::TextUnformatted((an + ":").c_str());
      if (ui::F().ui_bold) ImGui::PopFont();
      ui::NoteUiText((an + ": " + what).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
      if (*what) {
        ImGui::SameLine();
        ImGui::TextWrapped("%s", what);
      }
    }
    ImGui::PopTextWrapPos();
  }
  const std::string call = CallText(d);
  ImGui::Text("%s %s", TR("Formula result ="), ArgValue(s, call).c_str());
  static bool help = false;
  if (ImGui::IsWindowAppearing()) help = false;
  FunctionHelpLink(d.fn, &help);
  bool ok = false;
  const bool close = OkCancel(&ok);
  // started by fx on a cell: the cell and the formula bar show the call being built; OK writes it, Cancel drops it
  if (g_fx_edit && !s.edit.active) g_fx_edit = false;
  if (g_fx_edit && !close && s.edit.text != "=" + call) render::SetEditText(s.edit, "=" + call, static_cast<int>(call.size()) + 1);
  if (g_fx_edit && close) {
    g_fx_edit = false;
    if (ok) {
      s.edit.text = "=" + call;
      CommitEdit(s, false, 0);
    } else {
      CancelEdit(s);
    }
    EndDialog(s, close);
    return;
  }
  if (ok && d.span_begin >= 0 && !d.cell_formula.empty()) {
    // the first call of the cell's formula: the formula with the call changed goes into the cell
    std::string f = d.cell_formula;
    if (d.span_end <= static_cast<int>(f.size())) f.replace(static_cast<size_t>(d.span_begin), static_cast<size_t>(d.span_end - d.span_begin), call);
    StartEdit(s, true, f);
    CommitEdit(s, false, 0);
  } else if (ok && d.span_begin >= 0 && s.edit.active && d.span_end <= static_cast<int>(s.edit.text.size())) {
    // the call around the caret: it is written back over itself, and OK enters the formula, as Excel's does (Ctrl+A
    // after =SUM, then OK: the cell has =SUM(A1:A2); left in the editor, the entry waited for an Enter nobody
    // expected to press). An outer call still missing arguments gets Enter's message, as in Excel
    std::string t = s.edit.text;
    t.replace(static_cast<size_t>(d.span_begin), static_cast<size_t>(d.span_end - d.span_begin), call);
    render::SetEditText(s.edit, t, d.span_begin + static_cast<int>(call.size()));
    s.edit.ref_begin = s.edit.ref_end = -1;
    CommitEdit(s, false, 0);
  } else if (ok) {
    if (s.edit.active) {
      // typing a formula: the call goes in at the caret, and OK enters the formula (as above)
      std::string t = s.edit.text;
      int caret = std::clamp(s.edit.caret, 0, static_cast<int>(t.size()));
      if (t.empty() || t[0] != '=') {
        // text being typed becomes a formula: its "=" goes in front, never at the caret in the middle of the text
        t.insert(t.begin(), '=');
        ++caret;
      }
      t.insert(static_cast<size_t>(caret), call);
      render::SetEditText(s.edit, t, caret + static_cast<int>(call.size()));
      CommitEdit(s, false, 0);
    } else {
      // the cell gets the formula, as Excel's OK does
      StartEdit(s, true, "=" + call);
      CommitEdit(s, false, 0);
    }
  }
  EndDialog(s, close);
}

void InsertFunctionCall(AppState& s, const std::string& fn) {
  if (s.edit.active) {
    std::string t = s.edit.text;
    int caret = std::clamp(s.edit.caret, 0, static_cast<int>(t.size()));
    const std::string ins = fn + "()";
    if (t.empty() || t[0] != '=') {
      // text being typed becomes a formula: the "=" goes in front of it, not at the caret (Excel)
      t.insert(t.begin(), '=');
      ++caret;
    }
    t.insert(static_cast<size_t>(caret), ins);
    render::SetEditText(s.edit, t, caret + static_cast<int>(ins.size()) - 1);
    s.edit.focus_request = true;
    return;
  }
  StartEdit(s, true, "=" + fn + "()");
  s.edit.cursor_to_end = false;
  s.edit.pending_caret = static_cast<int>(s.edit.text.size()) - 1;
}

void DrawInsertFunction(AppState& s) {
  if (!BeginDialog(s, TR("Insert Function"), Dialog::InsertFunction, ImVec2(S(520), 0))) return;
  static char filter[64] = "";
  static std::string chosen;
  static int category = 0;  // -1 Most Recently Used, 0 all, 1.. the categories
  const auto cats = formula::FuncCategories();
  if (ImGui::IsWindowAppearing()) {
    filter[0] = 0;
    category = g_fn_recent.empty() ? 0 : -1;  // Excel opens on Most Recently Used
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::TextUnformatted(TR("Search for a function: "));
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##ff", TR("Type a function name or a brief description, e.g. \"sum\""), filter, sizeof(filter));
  ImGui::TextUnformatted(TR("Select a category: "));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(200));
  category = std::clamp(category, -1, static_cast<int>(cats.size()));
  if (ui::BeginCombo("##cat", category == -1 ? TR("Most Recently Used") : category == 0 ? TR("All") : cats[static_cast<size_t>(category - 1)])) {
    if (ImGui::Selectable(TR("Most Recently Used"), category == -1)) category = -1;
    if (ImGui::Selectable(TR("All"), category == 0)) category = 0;
    for (size_t i = 0; i < cats.size(); ++i) {
      if (ImGui::Selectable(cats[i], category == static_cast<int>(i) + 1)) category = static_cast<int>(i) + 1;
    }
    ImGui::EndCombo();
  }
  // list: the help table + other registered functions (without help)
  std::vector<const formula::FuncHelp*> list;
  const std::string f = engine::Lower(filter);
  // the built-in functions, then the Python ones (@udf, under User Defined as Excel lists its add-ins' functions)
  static std::vector<formula::FuncHelp> all;
  all = formula::AllFuncHelp();
  for (const formula::FuncHelp& h : formula::UserFuncHelp()) all.push_back(h);
  for (const auto& h : all) {
    if (category > 0 && std::strcmp(h.category, cats[static_cast<size_t>(category - 1)]) != 0) continue;
    if (category == -1 && f.empty() && std::find(g_fn_recent.begin(), g_fn_recent.end(), h.name) == g_fn_recent.end()) continue;
    // the name or the description, in any case ("returns" finds "Returns ...")
    if (!f.empty() && engine::Lower(h.name).find(f) == std::string::npos && engine::Lower(h.desc).find(f) == std::string::npos) continue;
    list.push_back(&h);
  }
  if (category == -1 && f.empty()) {
    // Most Recently Used: the last one used first, as Excel lists them
    auto rank = [](const formula::FuncHelp* h) { return std::find(g_fn_recent.begin(), g_fn_recent.end(), h->name) - g_fn_recent.begin(); };
    std::sort(list.begin(), list.end(), [&](auto* a, auto* b) { return rank(a) < rank(b); });
  } else if (!f.empty()) {
    // a search lists the best matches first, as Excel's recommends them: the name, then a name it starts, then one it
    // is in, then the descriptions, the functions people use most first and the shortest description next. In the
    // Chinese UI a search matches the descriptions only, and alphabetical order chose AVEDEV for the Chinese word for
    // average and DSUM for the one for adding up
    static const char* const kCommon[] = {"SUM", "AVERAGE", "COUNT", "MAX", "MIN", "IF", "VLOOKUP", "XLOOKUP", "INDEX", "MATCH", "ROUND",
                                          "SUMIF", "SUMIFS", "COUNTIF", "COUNTIFS", "AVERAGEIF", "IFERROR", "AND", "OR", "DATE", "TODAY", "NOW",
                                          "YEAR", "MONTH", "DAY", "LEFT", "RIGHT", "MID", "LEN", "TEXT", "CONCAT", "TRIM", "ABS", "STDEV.S", "PMT"};
    auto tier = [&](const formula::FuncHelp* h) {
      const std::string n = engine::Lower(h->name);
      if (n == f) return 0;
      if (n.rfind(f, 0) == 0) return 1;
      if (n.find(f) != std::string::npos) return 2;
      for (const char* c : kCommon) {
        if (std::strcmp(c, h->name) == 0) return 3;
      }
      return 4;
    };
    std::sort(list.begin(), list.end(), [&](auto* a, auto* b) {
      const int ta = tier(a), tb = tier(b);
      if (ta != tb) return ta < tb;
      if (ta == 3) return std::find_if(std::begin(kCommon), std::end(kCommon), [&](const char* c) { return std::strcmp(c, a->name) == 0; }) <
                          std::find_if(std::begin(kCommon), std::end(kCommon), [&](const char* c) { return std::strcmp(c, b->name) == 0; });
      if (ta == 4 && std::strlen(a->desc) != std::strlen(b->desc)) return std::strlen(a->desc) < std::strlen(b->desc);
      return std::strcmp(a->name, b->name) < 0;
    });
  } else {
    std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return std::strcmp(a->name, b->name) < 0; });
  }
  // as in Excel, a search selects its best match (the function of that name, else the first listed), so Enter inserts it
  if (!f.empty() && std::none_of(list.begin(), list.end(), [](auto* h) { return chosen == h->name; })) {
    chosen.clear();
    for (const auto* h : list) {
      if (engine::Lower(h->name) == f) chosen = h->name;
    }
    if (chosen.empty() && !list.empty()) chosen = list.front()->name;
  }
  ImGui::TextUnformatted(TR("Select a function: "));
  bool accept = false;
  ImGui::BeginChild("##list", ImVec2(0, S(220)), ImGuiChildFlags_Borders);
  for (const auto* h : list) {
    if (ImGui::Selectable(h->name, chosen == h->name, ImGuiSelectableFlags_AllowDoubleClick)) {
      chosen = h->name;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) accept = true;
    }
  }
  if (list.empty()) ImGui::TextDisabled(TR("No matching functions"));
  ImGui::EndChild();
  if (const formula::FuncHelp* h = chosen.empty() ? nullptr : formula::FindFuncHelp(chosen)) {
    ImGui::Text("%s(%s)", h->name, h->args);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("%s", h->desc);
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled(TR("Category: %s"), h->category);
  }
  static bool help = false;
  if (ImGui::IsWindowAppearing()) help = false;
  if (!chosen.empty()) FunctionHelpLink(chosen, &help);
  bool ok = false;
  const bool close = OkCancel(&ok) || accept;
  if (close && !((ok || accept) && !chosen.empty()) && g_fx_edit) {  // Cancel: the "=" fx put in the cell goes
    g_fx_edit = false;
    CancelEdit(s);
  }
  if ((ok || accept) && !chosen.empty()) {
    // as in Excel: the function's arguments next, one box each
    EndDialog(s, true);
    OpenFunctionArgs(s, chosen, {}, false);
    return;
  }
  EndDialog(s, close);
}

struct AskState {
  std::string title, text;
  std::function<void()> yes;
  std::string ok_label, alt_label;  // AskChoice's buttons ("" = OK / Cancel only)
  std::function<void()> alt;
} g_ask;

void DrawAsk(AppState& s) {
  if (!BeginDialog(s, (g_ask.title + "###yc_ask").c_str(), Dialog::Ask, ImVec2(S(420), 0))) return;
  ImGui::PushTextWrapPos(S(400));
  ImGui::TextWrapped("%s", g_ask.text.c_str());
  ImGui::PopTextWrapPos();
  bool ok = false, alt = false, close = false;
  if (g_ask.ok_label.empty()) {
    close = OkCancel(&ok);
  } else {
    ImGui::Separator();
    const float pad = S(20), gap = ImGui::GetStyle().ItemSpacing.x;
    auto width = [&](const char* label) { return std::max(S(88), ImGui::CalcTextSize(label, nullptr, true).x + pad); };
    const char* cancel = TR("Cancel");
    const float wo = width(g_ask.ok_label.c_str()), wa = g_ask.alt_label.empty() ? 0.0f : width(g_ask.alt_label.c_str()), wc = width(cancel);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - wo - (wa > 0 ? wa + gap : 0.0f) - wc - gap - S(16));
    ok = ImGui::Button(g_ask.ok_label.c_str(), ImVec2(wo, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (wa > 0) {
      ImGui::SameLine();
      alt = ImGui::Button(g_ask.alt_label.c_str(), ImVec2(wa, 0));
    }
    ImGui::SameLine();
    close = ImGui::Button(cancel, ImVec2(wc, 0)) || ok || alt;
  }
  if ((ok && g_ask.yes) || (alt && g_ask.alt)) {
    auto fn = std::move(ok ? g_ask.yes : g_ask.alt);
    g_ask.yes = g_ask.alt = nullptr;
    EndDialog(s, true);
    fn();
    return;
  }
  EndDialog(s, close);
}

void DrawMessage(AppState& s) {
  if (!BeginDialog(s, (s.message_title + "###yc_msg").c_str(), Dialog::Message, ImVec2(S(460), 0))) return;
  // read-only text: select any part, or Copy for all of it
  const float w = S(440);
  const float h = std::min(S(320), ImGui::CalcTextSize(s.message_text.c_str(), nullptr, false, w - S(16)).y + S(16));
  ImGui::InputTextMultiline("##msg", s.message_text.data(), s.message_text.size() + 1, ImVec2(w, h),
                            ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
  ImGui::Separator();
  const bool close = ImGui::Button(TR("OK"), ImVec2(S(88), 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter);
  ImGui::SameLine();
  if (ImGui::Button(TR("Copy"), ImVec2(S(88), 0))) {
    ImGui::SetClipboardText((s.message_title + "\n" + s.message_text).c_str());
    s.SetStatus(TR("Copied the message"));
  }
  EndDialog(s, close);
}

}  // namespace

namespace {
bool g_modal_copy = false;  // this frame's dialog text goes to the clipboard
}  // namespace

// the same for a window that isn't a dialog (Spelling, a comment thread): KeepOpenedWidth before ImGui::Begin,
// NoteOpenedWidth right after it returned true
namespace {
std::map<ImGuiID, float>& OpenedWidths() {
  static std::map<ImGuiID, float> m;
  return m;
}
}  // namespace

void KeepOpenedWidth(const char* name) {
  ImGuiWindow* w = ImGui::FindWindowByName(name);
  auto& m = OpenedWidths();
  if (!w || !w->WasActive) {
    if (w) m.erase(w->ID);
    return;
  }
  const auto it = m.find(w->ID);
  if (it != m.end()) ImGui::SetNextWindowSizeConstraints(ImVec2(it->second, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
}

void NoteOpenedWidth() {
  ImGuiWindow* w = ImGui::GetCurrentWindowRead();
  if (w->Appearing || w->AutoFitFramesX > 0) {
    float& v = OpenedWidths()[w->ID];
    v = std::max(v, w->SizeFull.x);
  }
}

bool BeginModal(const char* title, bool* open, int flags) {
  // a dialog that fits its contents (AlwaysAutoResize) and holds items as wide as the dialog (wrapped text, boxes of
  // width -1) measured itself a little narrower each frame and shrank before the user's eyes (the Password dialog):
  // it never gets narrower than it was when it opened, as Excel's dialogs keep their size
  static std::map<ImGuiID, float> opened_width;
  const ImGuiID id = ImGui::GetCurrentWindowRead()->GetID(title);
  const bool auto_size = (flags & ImGuiWindowFlags_AlwaysAutoResize) != 0;
  // a dialog given its width (SetNextWindowSize(w, 0)) keeps exactly that width, as Excel's dialogs have one, and only
  // its height follows the contents: a fitted width took the longest sentence's (wrapped text has no width of its own)
  // and stretched every full-width box with it (the Password dialog opened twice too wide)
  // Contents that can't wrap (a table, a box of a fixed width) still widen it: the width is at least what they needed
  // last frame. Wrapped text and boxes as wide as the dialog never need more than they are given, so they don't.
  ImGuiNextWindowData& next = ImGui::GetCurrentContext()->NextWindowData;
  if (auto_size && (next.HasFlags & ImGuiNextWindowDataFlags_HasSize) && next.SizeVal.x > 0.0f && next.SizeVal.y == 0.0f &&
      !(next.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint)) {
    float w = next.SizeVal.x;
    if (const ImGuiWindow* last = ImGui::FindWindowByName(title); last && last->WasActive) {
      w = std::max(w, last->ContentSizeIdeal.x + last->WindowPadding.x * 2.0f + (last->ScrollbarY ? last->ScrollbarSizes.x : 0.0f));
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));
  }
  if (auto_size && ImGui::IsPopupOpen(id, ImGuiPopupFlags_None)) {
    const auto it = opened_width.find(id);
    if (it != opened_width.end() && !(ImGui::GetCurrentContext()->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint)) {
      ImGui::SetNextWindowSizeConstraints(ImVec2(it->second, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    }
  }
  if (!ImGui::BeginPopupModal(title, open, flags)) {
    opened_width.erase(id);
    return false;
  }
  // a title that changes (Step 2 of 3) keeps its first one as ImGui's name: ui.items / ui.windows report this one
  ui::NoteWindowTitle(title);
  if (auto_size) {
    // the width it opened at (the first frames lay it out: the widest of them)
    float& w = opened_width[id];
    if (ImGui::GetCurrentWindowRead()->Appearing || ImGui::GetCurrentWindowRead()->AutoFitFramesX > 0) w = std::max(w, ImGui::GetWindowWidth());
  }
  // as in Excel, typing starts in the first field (Goal Seek's Set cell, Create Table's range...): without it the first
  // Tab only entered the dialog and every typed value landed one field early
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  // Ctrl+C in a dialog, not in one of its text boxes: all its text (labels, values, buttons) to the clipboard
  if (!g_modal_copy && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !ImGui::IsAnyItemActive() &&
      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    ImGui::LogToClipboard();
    g_modal_copy = true;
  }
  return true;
}

void DockAsTaskPane(const char* window_title, bool left, float width) {
  if (ImGui::FindWindowByName(window_title)) return;  // shown before in this session: where the user left it
  const ImGuiWindow* grid = ImGui::FindWindowByName("###yc_gridhost");
  if (!grid || !grid->DockNode) return;  // the sheets aren't docked (a container): it floats
  const float total = grid->DockNode->Size.x;
  if (total <= width * 2.0f) return;  // too narrow to share
  ui::dock::DockBesideWindow(window_title, "###yc_gridhost", left ? 0 : 1, width / total);
}

void PlaceNewWindow(WindowPlace where) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 pos = vp->GetCenter(), pivot(0.5f, 0.5f);
  switch (where) {
    case WindowPlace::Center: break;
    case WindowPlace::Bottom: pos = ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - S(40)), pivot = ImVec2(0.5f, 1.0f); break;
    case WindowPlace::Left: pos = ImVec2(vp->Pos.x + S(24), vp->Pos.y + vp->Size.y * 0.22f), pivot = ImVec2(0.0f, 0.0f); break;
    case WindowPlace::Right: pos = ImVec2(vp->Pos.x + vp->Size.x - S(24), vp->Pos.y + vp->Size.y * 0.22f), pivot = ImVec2(1.0f, 0.0f); break;
  }
  ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver, pivot);
}

void NoteDialogKeyboard() {
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ui::ribbon::St().dialog_keys_frame = ImGui::GetFrameCount();
}

bool DlgKey(ImGuiKey key) {
  const ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return false;
  if (io.WantTextInput && !io.KeyAlt) return false;  // a text box takes the letter itself; Alt+letter still chooses
  return ImGui::IsKeyPressed(key, false);
}

bool CheckboxMixed(const char* label, bool* all, bool mixed) {
  if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
  const bool clicked = ImGui::Checkbox(label, all);
  if (mixed) ImGui::PopItemFlag();
  return clicked;
}

bool BeginDlgTabBar(const char* id) {
  if (!ImGui::BeginTabBar(id)) return false;  // lesson-ok: dlg-tabs the one place that calls it
  ImGuiContext& g = *GImGui;
  ImGuiTabBar* bar = g.CurrentTabBar;
  // the dialog's own tabs only (a tab bar inside a tab, as Header / Footer's pages, keeps out), while it has the keyboard
  if (!bar || g.CurrentTabBarStack.Size != 1 || bar->Tabs.Size < 2 || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return true;
  int dir = 0;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Tab) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageDown)) dir = 1;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageUp)) dir = -1;
  if (!dir) return true;
  // the tabs in the order they show (a tab left out this frame, as Format Cells' for a table style, isn't one)
  std::vector<const ImGuiTabItem*> shown;
  for (const ImGuiTabItem& t : bar->Tabs) {
    if (t.LastFrameVisible >= g.FrameCount - 1) shown.push_back(&t);
  }
  std::sort(shown.begin(), shown.end(), [](const ImGuiTabItem* a, const ImGuiTabItem* b) { return a->Offset < b->Offset; });
  if (shown.size() < 2) return true;
  size_t at = 0;
  for (size_t i = 0; i < shown.size(); ++i) {
    if (shown[i]->ID == bar->SelectedTabId) at = i;
  }
  const int n = static_cast<int>(shown.size());
  bar->NextSelectedTabId = shown[static_cast<size_t>((static_cast<int>(at) + dir + n) % n)]->ID;
  return true;
}

bool DlgListKeys(int* sel, int count, const std::function<const char*(int)>& label) {
  if (count <= 0 || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return false;
  const int before = *sel;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) *sel = std::min(*sel + 1, count - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) *sel = std::max(*sel - 1, 0);
  if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) *sel = 0;
  if (ImGui::IsKeyPressed(ImGuiKey_End, false)) *sel = count - 1;
  for (ImGuiKey k = ImGuiKey_A; k <= ImGuiKey_Z; k = static_cast<ImGuiKey>(k + 1)) {
    if (!ImGui::IsKeyPressed(k, false) || ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyAlt) continue;
    const char want = static_cast<char>('a' + (k - ImGuiKey_A));
    for (int i = 1; i <= count; ++i) {  // the next item with that letter, after the current one
      const int j = (*sel + i) % count;
      const char* l = label(j);
      if (l && std::tolower(static_cast<unsigned char>(l[0])) == want) {
        *sel = j;
        break;
      }
    }
  }
  return *sel != before;
}

bool DlgOkButton(const char* label, const ImVec2& size) {
  if (ImGui::Button(label, size)) return true;
  if (!ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) return false;
  if (ImGui::IsWindowAppearing() || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return false;
  const ImGuiInputTextState* st = ImGui::GetActiveID() ? ImGui::GetInputTextState(ImGui::GetActiveID()) : nullptr;
  return !(st && (st->Flags & ImGuiInputTextFlags_Multiline));
}

void FinishModalCopy(AppState& s) {
  if (!g_modal_copy) return;
  ImGui::LogFinish();
  g_modal_copy = false;
  s.SetStatus(TR("Copied the dialog's text"));
}

void BeginRenameSheet(AppState& s, uint32_t sheet_id) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh || RefuseReadOnlyUi(s)) return;  // read-only mode: no name box opens on the tab (a double-click, Rename)
  g_rename_sheet = sheet_id;
  std::snprintf(g_rename_buf, sizeof(g_rename_buf), "%s", sh->name.c_str());
  g_rename_inplace = true;
  g_rename_focus = true;
  g_rename_unseen = 0;
}

bool SheetRenaming(uint32_t* sheet, char** buf, size_t* cap, bool* focus) {
  if (!g_rename_inplace) return false;
  *sheet = g_rename_sheet;
  *buf = g_rename_buf;
  *cap = sizeof(g_rename_buf);
  // the keyboard goes to the box once no message is in front of it (an invalid name's message)
  *focus = g_rename_focus && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
  if (*focus) g_rename_focus = false;
  return true;
}

void SheetRenameResult(AppState& s, bool drawn, bool commit, bool cancel) {
  if (!g_rename_inplace) return;
  if (!s.operator_mode) {  // read-only mode came on while the name was typed: the box closes, the name stays
    g_rename_inplace = false;
    return;
  }
  if (!drawn && ++g_rename_unseen > 2) {
    // the tab isn't on the screen: the small box instead
    g_rename_inplace = false;
    g_rename_open = true;
    return;
  }
  if (drawn) g_rename_unseen = 0;
  if (cancel) {
    g_rename_inplace = false;
    return;
  }
  if (!commit || !s.engine) return;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(g_rename_sheet) : nullptr;
  const std::string name = g_rename_buf;
  if (!sh || name == sh->name) {
    g_rename_inplace = false;
    return;
  }
  std::string err;
  const uint32_t id = g_rename_sheet;
  const bool done = s.engine->Call([&](Document& d) {
    d.Begin(TR("Rename Sheet"));
    const bool r = d.RenameSheet(id, name, &err);
    d.Commit();
    return r;
  });
  if (done) {
    g_rename_inplace = false;
  } else {
    // Excel: the reason, then the name stays in the box to be fixed
    s.ShowError("YCGrid", err.empty() ? std::string(TR("That sheet name isn't valid.")) : err);
    g_rename_focus = true;
  }
}

void OpenCellContextMenu() { g_open_cell_menu = true; }

bool PaletteColor(const char* label, uint32_t* argb, bool allow_auto, const char* auto_label) {
  // conditional formats blend scale and bar colors as RGB: a theme color is stored as the color it is now
  if (!ColorEdit(label, argb, allow_auto, auto_label)) return false;
  if (model::IsThemeColor(*argb)) *argb = 0xFF000000u | model::ResolveColor(*argb);
  return true;
}

void AskYesNo(AppState& s, std::string title, std::string text, std::function<void()> yes) {
  g_ask.title = std::move(title);
  g_ask.text = std::move(text);
  g_ask.yes = std::move(yes);
  g_ask.ok_label.clear();
  g_ask.alt_label.clear();
  g_ask.alt = nullptr;
  s.OpenDialog(Dialog::Ask);
}

void AskChoice(AppState& s, std::string title, std::string text, std::string ok, std::function<void()> yes,
               std::string other, std::function<void()> alt) {
  AskYesNo(s, std::move(title), std::move(text), std::move(yes));
  g_ask.ok_label = std::move(ok);
  g_ask.alt_label = std::move(other);
  g_ask.alt = std::move(alt);
}

std::string DialogText(const AppState& s) {
  if (s.dialog == Dialog::Message) return s.message_title + "\n" + s.message_text;
  if (s.dialog == Dialog::Ask) return g_ask.title + "\n" + g_ask.text;
  return {};
}
namespace {
bool g_cell_menu_at = false;  // opened from the keyboard: placed at the active cell instead of the mouse
ImVec2 g_cell_menu_pos;
bool g_menu_open = false;  // a cell, header or sheet tab menu is open (the keyboard moves through it)
bool g_menu_mini = false;  // the open cell / header menu has Excel's mini toolbar (opened by the mouse, not Shift+F10)

// Excel's mini toolbar over the cell, row and column menus a right-click opens: the Home tab's controls (fonts,
// sizes, number formats, Format Painter; bold, italic, center, borders, colors, decimals, merge). Returns its box.
ImRect MiniToolbar() {
  struct E {
    const char* cmd;
    float w;
  };
  static const E kRow1[] = {{"FontName", 112}, {"FontSize", 44}, {"FontSizeIncrease", 24}, {"FontSizeDecrease", 24},
                            {"AccountingFormat", 36}, {"PercentStyle", 24}, {"CommaStyle", 24}, {"FormatPainter", 24}};
  static const E kRow2[] = {{"Bold", 24}, {"Italic", 24}, {"AlignCenter", 24}, {"Borders", 36}, {"FillColor", 36},
                            {"FontColor", 36}, {"DecimalIncrease", 24}, {"DecimalDecrease", 24}, {"MergeCenter", 36}};
  const ImVec2 o = ImGui::GetCursorScreenPos();
  float right = o.x;
  auto row = [&](const E* e, size_t n, float y) {
    float x = o.x;
    for (size_t i = 0; i < n; ++i) {
      ui::ribbon::DrawItemElsewhere(e[i].cmd, ImRect(ImVec2(x, y), ImVec2(x + S(e[i].w), y + S(24))));
      x += S(e[i].w) + S(2);
    }
    right = std::max(right, x);
  };
  row(kRow1, std::size(kRow1), o.y);
  row(kRow2, std::size(kRow2), o.y + S(27));
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + S(51)));
  ImGui::Dummy(ImVec2(right - o.x, S(1)));
  return ImRect(o, ImVec2(right, o.y + S(51)));
}

// a menu that shows the mini toolbar: the window has no background of its own; the toolbar and the menu are drawn
// as two boxes, the toolbar above (Excel). Begin before the menu's items, End after them.
struct MiniMenu {
  bool on = false;
  ImDrawList* dl = nullptr;
  ImRect bar;
  float menu_y0 = 0;
  int commands = 0;
  void Begin(bool show) {
    on = show;
    if (!on) return;
    dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    commands = ui::Commands().execute_count();
    bar = MiniToolbar();
    // a control of the toolbar used: the menu goes away (Excel)
    if (ui::Commands().execute_count() != commands) ImGui::CloseCurrentPopup();
    ImGui::Dummy(ImVec2(1, S(10)));
    menu_y0 = ImGui::GetCursorScreenPos().y;
  }
  void End() {
    if (!on) return;
    const float pad = S(5);
    const ImVec2 wp = ImGui::GetWindowPos();
    const float wr = wp.x + ImGui::GetWindowWidth();
    const float menu_y1 = ImGui::GetCursorScreenPos().y;
    dl->ChannelsSetCurrent(0);
    auto box = [&](ImVec2 a, ImVec2 b) {
      dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_PopupBg), S(2));
      dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), S(2));
    };
    box(ImVec2(wp.x, bar.Min.y - pad), ImVec2(std::min(wr, bar.Max.x + pad), bar.Max.y + pad));
    box(ImVec2(wp.x, menu_y0 - pad), ImVec2(wr, menu_y1 + pad));
    dl->ChannelsMerge();
  }
};
}  // namespace
void OpenCellContextMenuAt(ImVec2 pos, int kind) {
  (kind == 1 ? g_open_row_menu : kind == 2 ? g_open_col_menu : g_open_cell_menu) = true;
  g_cell_menu_at = true;
  g_cell_menu_pos = pos;
}
bool ContextMenuOpen() { return g_menu_open || DockTabMenuOpen(); }
void OpenColHeaderContextMenu() { g_open_col_menu = true; }
void OpenRowHeaderContextMenu() { g_open_row_menu = true; }
void OpenSheetTabContextMenu(uint32_t sheet_id) {
  g_tab_menu_sheet = sheet_id;
  g_open_tab_menu = true;
}

void OpenFind(AppState& s, bool replace) { OpenFindWindow(s, replace); }

// Shift+F4: the next match of the last search, without the dialog (Excel); nothing searched yet: the Find dialog
void FindNextKey(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!g_find[0] || g_look_in == 2) {
    OpenFindWindow(s, false);
    return;
  }
  if (!s.engine || !sh) return;
  const auto& sel = s.FocusView().sel;
  const Rect w{0, 0, sh->row_limit - 1, sh->col_limit - 1};
  const std::string what = g_find;
  uint32_t r = 0, c = 0;
  const uint32_t id = s.focus_sheet;
  const bool ok = s.engine->Call([&](Document& d) { return d.Find(id, w, what, sel.act_r, sel.act_c, g_by_rows, g_match_case, g_whole, g_look_in == 0, &r, &c); });
  if (!ok) {
    s.ShowError("YCGrid", TR("We couldn't find what you were looking for."));
    return;
  }
  GoToHit(s, FindHit{id, r, c, sh->name, ""});
}
bool FindOpen() { return g_find_open != 0; }

void DrawDialogs(AppState& s) {
  DrawInsertChartDialog(s);
  DrawRuleDialogs(s);
  DrawMiscDialogs(s);
  DrawDataDialogs(s);
  DrawWhatIfDialogs(s);
  DrawAnalysisDialogs(s);
  DrawProtectDialogs(s);
  DrawTableDialogs(s);
  DrawIllustrationDialogs(s);
  DrawPageSetupDialog(s);
  DrawPivotDialogs(s);
  DrawStyleDialogs(s);
  DrawCustomListsDialog(s);
  trust::DrawDialogs(s);
  DrawLinksDialog(s);
  DrawMoveChartDialog(s);
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
  DrawFormatCells(s);
  DrawFindReplace(s);
  DrawGoTo(s);
  DrawDim(s, true);
  DrawDim(s, false);
  DrawStandardWidth(s);
  DrawInsDel(s, true);
  DrawInsDel(s, false);
  DrawPasteSpecial(s);
  DrawUnhideSheet(s);
  DrawAsk(s);
  DrawFunctionArgs(s);
  DrawMoveCopySheet(s);
  DrawTabColor(s);
  DrawZoom(s);
  DrawInsertFunction(s);
  DrawNameDialogs(s);
  DrawMessage(s);

  // rename sheet (small floating input box)
  if (g_rename_open) {
    ImGui::OpenPopup(TR("Rename Sheet"));
    g_rename_open = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (BeginModal(TR("Rename Sheet"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(S(240));
    const bool enter = ImGui::InputText("##rename", g_rename_buf, sizeof(g_rename_buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool ok = false;
    const bool close = OkCancel(&ok) || ImGui::IsKeyPressed(ImGuiKey_Escape) || !s.operator_mode;
    if ((ok || enter) && s.engine && !RefuseReadOnlyUi(s)) {
      std::string err;
      const std::string name = g_rename_buf;
      const uint32_t id = g_rename_sheet;
      const bool done = s.engine->Call([&](Document& d) {
        d.Begin(TR("Rename Sheet"));
        const bool r = d.RenameSheet(id, name, &err);
        d.Commit();
        return r;
      });
      if (!done) s.SetStatus(err, 6.0);
    }
    if (close || enter) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  // every dialog is a popup: one that is "open" with no popup on the screen for a few frames went away without
  // closing itself (another popup took its place). Nobody could see or close it, and YCGrid would wait for it.
  static int stray_frames = 0;
  DrawRangePick(s);  // a dialog shrunk to a range box while the user selects cells
  // (a dialog put aside for picking cells isn't stray)
  const bool stray = s.dialog != Dialog::None && !s.dialog_open_request && !RangePicking() &&
                     !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  stray_frames = stray ? stray_frames + 1 : 0;
  if (stray_frames >= 3) {
    s.dialog = Dialog::None;
    stray_frames = 0;
  }
}

void DrawContextMenus(AppState& s) {
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(7)));
  if (g_open_cell_menu) ImGui::OpenPopup("##cellmenu");
  static int kbd_frames = 0;  // opened from the keyboard: show where the keyboard is (the first item) for its first frames
  // the menu Shift+F10 opened (the cell's, the row's or the column's), placed at the active cell before its Begin
  static const char* kbd_menu = nullptr;
  if (g_cell_menu_at) kbd_menu = g_open_row_menu ? "##rowmenu" : g_open_col_menu ? "##colmenu" : "##cellmenu";
  auto kbd_place = [&](const char* id) {
    if (!kbd_menu || std::strcmp(kbd_menu, id) != 0 || !ImGui::IsPopupOpen(id)) return;
    ImGui::SetNextWindowPos(g_cell_menu_pos);
    kbd_frames = 2;
    kbd_menu = nullptr;
  };
  // the mini toolbar comes with a menu the mouse opened (Excel: not with Shift+F10 or the Menu key)
  if (g_open_cell_menu || g_open_col_menu || g_open_row_menu) g_menu_mini = !g_cell_menu_at;
  g_cell_menu_at = false;
  if (g_open_col_menu) ImGui::OpenPopup("##colmenu");
  if (g_open_row_menu) ImGui::OpenPopup("##rowmenu");
  if (g_open_tab_menu) ImGui::OpenPopup("##tabmenu");
  g_open_cell_menu = g_open_col_menu = g_open_row_menu = g_open_tab_menu = false;
  auto clip_items = [&]() {
    MenuCmd("Cut");
    MenuCmd("Copy");
    // Excel's Paste Options: a row of pictures (Paste, Values, Formulas, Transpose, Formatting, Paste Link), then
    // Paste Special with every way and the dialog
    ImGui::TextDisabled("%s", TR("Paste Options:"));
    static const int kRow[] = {0, 5, 1, 4, 7, 8};
    ImGui::Indent(S(6));
    int way = PasteIconRow(kRow, std::size(kRow), -1);
    ImGui::Unindent(S(6));
    if (ImGui::BeginMenu(TR("Paste Special"))) {
      const int w = PasteGallery(-1);
      if (w >= 0) way = w;
      ImGui::Separator();
      MenuCmd("PasteSpecial");
      ImGui::EndMenu();
    }
    if (way >= 0) {
      ImGui::CloseCurrentPopup();
      PasteWayNow(s, way);
    }
    MenuCmd("ScreenshotSelection", TR("Screenshot of Selection"));
    ImGui::Separator();
  };
  if (!ImGui::IsPopupOpen("##cellmenu") && !ImGui::IsPopupOpen("##rowmenu") && !ImGui::IsPopupOpen("##colmenu") && !kbd_menu)
    kbd_frames = 0;  // closed in its first frame: the next menu starts plain
  // with the mini toolbar: no window background (the toolbar and the menu draw their own boxes), the menu at the mouse
  // and the toolbar above it
  auto mini_style = [&](const char* id) {
    const bool mini = g_menu_mini && ImGui::IsPopupOpen(id);
    if (mini) {
      ImGui::SetNextWindowPos(ImGui::GetIO().MousePos - ImVec2(S(6), S(72)), ImGuiCond_Appearing);
      ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0, 0, 0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    }
    return mini;
  };
  auto mini_pop = [](bool mini) {
    if (!mini) return;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  };
  // Excel's letters in the menus (the underlined ones; Shift+F10 or the Menu key, then V pastes values): only the
  // letters no other entry of the menu shares, which run their entry at once (a shared one only moves the highlight
  // in Excel's menus). Only while the menu itself has the keyboard, not an open submenu
  auto letters = [&](std::initializer_list<std::pair<ImGuiKey, const char*>> cmds, std::initializer_list<std::pair<ImGuiKey, int>> pastes) {
    const ImGuiIO& kio = ImGui::GetIO();
    if (!ImGui::IsWindowFocused() || ImGui::IsAnyItemActive() || kio.KeyCtrl || kio.KeyAlt || kio.KeyShift) return;
    for (const auto& [key, id] : cmds) {
      const ui::Command* c = ui::Commands().Find(id);
      if (ImGui::IsKeyPressed(key, false) && (!c || c->enabled)) {
        ImGui::CloseCurrentPopup();
        Exec(id);
        return;
      }
    }
    for (const auto& [key, way] : pastes) {  // the Paste Options pictures: Paste (P), Values (V), Formatting (R)
      if (ImGui::IsKeyPressed(key, false) && CopyModeOn(s)) {
        ImGui::CloseCurrentPopup();
        PasteWayNow(s, way);
        return;
      }
    }
  };
  kbd_place("##cellmenu");
  const bool cell_mini = mini_style("##cellmenu");
  const bool cell_open = ImGui::BeginPopup("##cellmenu");
  mini_pop(cell_mini);
  if (cell_open) {
    MiniMenu mm;
    mm.Begin(cell_mini);
    // in a PivotTable report: Excel's PivotTable menu (the report's cells aren't inserted, merged or sorted as cells)
    const bool pivot_menu = DrawPivotCellMenu(s);
    if (pivot_menu) {
      ImGui::Separator();
      MenuCmd("Copy");
      MenuCmd("FormatCellsDialog", TR("Format Cells..."));
    } else {
    if (kbd_frames > 0 && --kbd_frames == 0) ImGui::SetNavCursorVisible(true);
    letters({{ImGuiKey_T, "Cut"}, {ImGuiKey_C, "Copy"}, {ImGuiKey_I, "InsertCells"}, {ImGuiKey_D, "DeleteCells"}, {ImGuiKey_F, "FormatCellsDialog"},
             {ImGuiKey_K, "PickFromList"}, {ImGuiKey_A, "NameManager"}},
            {{ImGuiKey_P, 0}, {ImGuiKey_V, 5}, {ImGuiKey_R, 7}});
    clip_items();
    // a #SPILL! formula: what is in its way can be selected (Excel's error button)
    if (s.snap) {
      const auto& sel = s.FocusView().sel;
      for (const auto& [k, want] : s.snap->spill_blocked) {
        if (engine::KeySheet(k) != s.focus_sheet || engine::KeyRow(k) != sel.act_r || engine::KeyCol(k) != sel.act_c) continue;
        MenuCmd("SelectObstructingCells");
        ImGui::Separator();
        break;
      }
    }
    MenuCmd("InsertCells", !CopyModeOn(s) ? TR("Insert...") : s.clip->cut ? TR("Insert Cut Cells...") : TR("Insert Copied Cells..."));
    MenuCmd("DeleteCells", TR("Delete..."));
    MenuCmd("ClearContents");
    ImGui::Separator();
    DrawCellMenuFilter(s);  // Filter > by Selected Cell's Value / Color / Font Color (Excel)
    if (ImGui::BeginMenu(TR("Sort"))) {
      MenuCmd("SortAscending");
      MenuCmd("SortDescending");
      MenuCmd("SortCellColorTop");
      MenuCmd("SortFontColorTop");
      MenuCmd("SortIconTop");
      ImGui::Separator();
      MenuCmd("CustomSort", TR("Custom Sort..."));
      ImGui::EndMenu();
    }
    ImGui::Separator();
    {
      // like Excel, the items follow what the active cell holds: a comment, a note, a link
      const model::Sheet* sh = s.FocusSheet();
      const auto& sel = s.FocusView().sel;
      const uint64_t key = (uint64_t{sel.act_r} << 16) | sel.act_c;
      const auto ni = sh ? sh->notes->find(key) : decltype(sh->notes->end()){};
      const bool note = sh && ni != sh->notes->end() && !ni->second.threaded, thread = sh && ni != sh->notes->end() && ni->second.threaded;
      const bool link = sh && sh->links->count(key) > 0;
      if (thread) {
        MenuCmd("NewComment", TR("Reply to Comment"));
        MenuCmd("DeleteCommentThread", TR("Delete Comment"));
      } else if (note) {
        MenuCmd("InsertComment", TR("Edit Note"));
        MenuCmd("DeleteComment", TR("Delete Note"));
        MenuCmd("ShowHideNote");
      } else {
        MenuCmd("NewComment");
        MenuCmd("InsertComment", TR("New Note"));
      }
      ImGui::Separator();
      if (link) {
        MenuCmd("InsertHyperlink", TR("Edit Hyperlink..."));
        MenuCmd("FollowHyperlink", TR("Open Hyperlink"));
        MenuCmd("RemoveHyperlinks", TR("Remove Hyperlink"));
      } else {
        MenuCmd("InsertHyperlink", TR("Link"));
      }
    }
    ImGui::Separator();
    MenuCmd("FormatCellsDialog", TR("Format Cells..."));
    MenuCmd("PickFromList", TR("Pick From Drop-down List..."));
    MenuCmd("NameManager", TR("Define Name..."));
    DrawCustomMenuItems(s, "Cell");
    }  // not a PivotTable report
    mm.End();
    ImGui::EndPopup();
  }
  // the row and column menus' letters are Excel 16's (UI Automation's AccessKey of its menus): Cut T, Copy C, Paste P,
  // Paste Special S, Insert I, Delete D, Clear Contents N, Format Cells F, Row Height R / Column Width W, Hide H,
  // Unhide U
  kbd_place("##colmenu");
  const bool col_mini = mini_style("##colmenu");
  const bool col_open = ImGui::BeginPopup("##colmenu");
  mini_pop(col_mini);
  if (col_open) {
    MiniMenu mm;
    mm.Begin(col_mini);
    if (kbd_frames > 0 && --kbd_frames == 0) ImGui::SetNavCursorVisible(true);
    letters({{ImGuiKey_T, "Cut"}, {ImGuiKey_C, "Copy"}, {ImGuiKey_S, "PasteSpecial"}, {ImGuiKey_I, "InsertColumns"}, {ImGuiKey_D, "DeleteColumns"},
             {ImGuiKey_N, "ClearContents"}, {ImGuiKey_F, "FormatCellsDialog"}, {ImGuiKey_W, "ColumnWidth"}, {ImGuiKey_H, "HideColumns"},
             {ImGuiKey_U, "UnhideColumns"}},
            {{ImGuiKey_P, 0}, {ImGuiKey_V, 5}, {ImGuiKey_R, 7}});
    clip_items();
    MenuCmd("InsertColumns", !CopyModeOn(s) ? TR("Insert") : s.clip->cut ? TR("Insert Cut Cells") : TR("Insert Copied Cells"));
    MenuCmd("DeleteColumns", TR("Delete"));
    MenuCmd("ClearContents");
    ImGui::Separator();
    MenuCmd("FormatCellsDialog", TR("Format Cells..."));
    MenuCmd("ColumnWidth");
    MenuCmd("HideColumns", TR("Hide"));
    MenuCmd("UnhideColumns", TR("Unhide"));
    DrawCustomMenuItems(s, "Column");
    mm.End();
    ImGui::EndPopup();
  }
  kbd_place("##rowmenu");
  const bool row_mini = mini_style("##rowmenu");
  const bool row_open = ImGui::BeginPopup("##rowmenu");
  mini_pop(row_mini);
  if (row_open) {
    MiniMenu mm;
    mm.Begin(row_mini);
    if (kbd_frames > 0 && --kbd_frames == 0) ImGui::SetNavCursorVisible(true);
    letters({{ImGuiKey_T, "Cut"}, {ImGuiKey_C, "Copy"}, {ImGuiKey_S, "PasteSpecial"}, {ImGuiKey_I, "InsertRows"}, {ImGuiKey_D, "DeleteRows"},
             {ImGuiKey_N, "ClearContents"}, {ImGuiKey_F, "FormatCellsDialog"}, {ImGuiKey_R, "RowHeight"}, {ImGuiKey_H, "HideRows"},
             {ImGuiKey_U, "UnhideRows"}},
            {{ImGuiKey_P, 0}, {ImGuiKey_V, 5}});  // R is Row Height's here, not the Formatting picture's
    clip_items();
    MenuCmd("InsertRows", !CopyModeOn(s) ? TR("Insert") : s.clip->cut ? TR("Insert Cut Cells") : TR("Insert Copied Cells"));
    MenuCmd("DeleteRows", TR("Delete"));
    MenuCmd("ClearContents");
    ImGui::Separator();
    MenuCmd("FormatCellsDialog", TR("Format Cells..."));
    MenuCmd("RowHeight");
    MenuCmd("HideRows", TR("Hide"));
    MenuCmd("UnhideRows", TR("Unhide"));
    DrawCustomMenuItems(s, "Row");
    mm.End();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##tabmenu")) {
    s.focus_sheet = g_tab_menu_sheet;
    MenuCmd("InsertSheet", TR("Insert"));
    MenuCmd("DeleteSheet", TR("Delete"));
    if (ImGui::MenuItem(TR("Rename"))) BeginRenameSheet(s, g_tab_menu_sheet);
    MenuCmd("CopySheet", TR("Move or Copy..."));
    MenuCmd("ViewCode");
    MenuCmd("ProtectSheet", TR("Protect Sheet..."));
    // Tab Color: Excel's palette in a submenu (No Color, theme and standard colors, More Colors...)
    if (ImGui::BeginMenu(TR("Tab Color"))) {
      uint32_t ref = 0;
      if (ui::ribbon::PaletteSwatches(&ref, TR("No Color")) && s.engine) {
        const uint32_t sheet = g_tab_menu_sheet;
        s.engine->PostUser(TR("Tab Color"), [sheet, ref](engine::Document& d) { d.SetTabColor(sheet, ref); });
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::Selectable(TR("More Colors..."))) {
        ImGui::CloseCurrentPopup();
        Exec("TabColor");
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    MenuCmd("ScreenshotSheet", TR("Screenshot of Sheet"));
    MenuCmd("ScreenshotWorkbook", TR("Screenshot of Workbook"));
    ImGui::Separator();
    MenuCmd("HideSheet", TR("Hide"));
    MenuCmd("UnhideSheet", TR("Unhide..."));
    ImGui::Separator();
    MenuCmd("SelectAllSheets");
    if (s.group_sheets.size() > 1) MenuCmd("UngroupSheets");  // shown while sheets are grouped (Excel)
    DrawCustomMenuItems(s, "Ply");
    ImGui::EndPopup();
  }
  g_menu_open = ImGui::IsPopupOpen("##cellmenu") || ImGui::IsPopupOpen("##colmenu") || ImGui::IsPopupOpen("##rowmenu") || ImGui::IsPopupOpen("##tabmenu");
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
}

}  // namespace yc::app
