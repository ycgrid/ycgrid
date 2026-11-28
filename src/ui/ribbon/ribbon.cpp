// SPDX-License-Identifier: Apache-2.0
#include "ribbon/ribbon.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "fonts/cell_fonts.h"
#include "formula/func_help.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "model/objects.h"
#include "model/theme.h"
#include "yc_ui_items.h"
#include "theme/theme.h"

#include "implot.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace yc::ui::ribbon {
namespace {

// base sizes (px at 100% DPI, to be calibrated)
constexpr float kTabRowH = 30.0f;
constexpr float kBodyH = 94.0f;
constexpr float kContentTop = 3.0f;
constexpr float kRowH = 22.0f;
constexpr float kLabelH = 20.0f;
constexpr float kGroupPadX = 4.0f;
constexpr float kColGap = 2.0f;
constexpr float kLargeTopH = 38.0f;   // height of the upper (icon) half of a large split button
constexpr float kArrowW = 13.0f;
// gallery (Cell Styles)
constexpr float kTileW = 80.0f;
constexpr float kTileH = 32.0f;
constexpr float kTileGap = 3.0f;
constexpr float kGalleryScrollW = 16.0f;
constexpr int kPopupCols = 5;

State g_state;
std::map<std::string, std::vector<GalleryTile>> g_galleries;
std::map<std::string, std::vector<std::string>> g_gallery_footers;
std::map<std::string, int> g_gallery_row;  // first visible row of each in-ribbon gallery
std::string g_open_gallery;  // a gallery to open on the next frame (OpenGallery)
struct GalleryLayout {
  int cols;
  float w, h;
};
std::map<std::string, GalleryLayout> g_gallery_layouts;
std::vector<Tab> g_tabs;
bool g_executed = false;
struct SeenItem {
  std::string cmd;
  ImRect rect;
  const char* where = "ribbon";
  int frame = 0;
};
// VisibleItems; by command and kind, so a drop-down and the entry of the same name in its menu are both there
std::map<std::string, SeenItem> g_seen;
const char* g_seen_where = nullptr;  // drawn elsewhere (DrawItemElsewhere): every item is listed as there
void Seen(const std::string& cmd, const ImRect& r, const char* where) {
  if (cmd.empty()) return;
  if (g_seen_where) where = g_seen_where;
  // what a person sees: an entry scrolled out of its menu isn't listed, one cut by the menu's edge is listed by its
  // visible part (a click at the middle of the whole entry landed outside the menu, on another entry)
  ImRect v = r;
  if (const ImGuiWindow* w = ImGui::GetCurrentContext() ? ImGui::GetCurrentContext()->CurrentWindow : nullptr) {
    if (!r.Overlaps(w->ClipRect)) return;
    v.ClipWithFull(w->ClipRect);
  }
  g_seen[cmd + "|" + where] = SeenItem{cmd, v, where, ImGui::GetFrameCount()};
}
bool g_peek_opened = false;  // the temporary ribbon's popup was opened (it opens once; a tab switch opens it again)   // whether a command ran this frame (closes the temporary expansion of a collapsed ribbon)

Command* Cmd(const std::string& id) { return Commands().Find(id); }

// the label a person sees: an ImGui id after "##" (Group##spark, which keeps two commands' labels apart) is never
// drawn, as ImGui's own widgets hide it (the Sparkline tab showed "Group##spark")
std::string LabelOf(const std::string& id) {
  Command* c = Cmd(id);
  std::string label = c ? c->label : id;
  if (const size_t at = label.find("##"); at != std::string::npos) label.resize(at);
  return label;
}

const char* IconOf(const Item& it, bool large) {
  if (large && it.icon_large) return it.icon_large;
  if (it.icon) return it.icon;
  Command* c = Cmd(it.cmd);
  return c ? c->icon : nullptr;
}

bool IsPressed(const Item& it) {
  Command* c = Cmd(it.cmd);
  return c && c->toggle && c->pressed;
}

bool IsEnabled(const std::string& id) {
  Command* c = Cmd(id);
  return !c || c->enabled;
}

void Exec(const std::string& id) {
  Commands().Execute(id);
  g_executed = true;
}

// split buttons that remember their last menu entry (Borders, Underline): split command -> the entry
std::map<std::string, std::string> g_split_last;
std::string g_menu_ran;  // the menu entry run while drawing the current menu popup

// the face of a split that remembers: the entry chosen last (dialog openers such as More Borders aren't kept)
Item SplitFace(const Item& it) {
  if (!it.remember_last) return it;
  auto f = g_split_last.find(it.cmd);
  if (f == g_split_last.end()) return it;
  Item face = it;
  face.cmd = f->second;
  face.icon = face.icon_large = nullptr;
  return face;
}
void NoteSplitChoice(const Item& it) {
  if (!it.remember_last || g_menu_ran.empty()) return;
  // modes (Draw Border, Erase Border, Line Style) aren't kept either
  const bool mode = g_menu_ran.rfind("BorderDraw", 0) == 0 || g_menu_ran == "BorderErase" || g_menu_ran.rfind("BorderLine", 0) == 0;
  if (std::find(it.menu.begin(), it.menu.end(), g_menu_ran) != it.menu.end() && g_menu_ran.find("More") != 0 && !mode) g_split_last[it.cmd] = g_menu_ran;
}

// width of single-line text or text split into two lines by '\n' (current font)
float LabelWidth(const std::string& s) {
  float w = 0.0f;
  size_t start = 0;
  while (true) {
    const size_t nl = s.find('\n', start);
    const std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    w = std::max(w, ImGui::CalcTextSize(line.c_str()).x);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return w;
}

std::string FirstLine(const std::string& s) {
  const size_t nl = s.find('\n');
  return nl == std::string::npos ? s : s.substr(0, nl);
}

// joining a two-line label into one line: CJK characters join directly, otherwise a space is inserted
// a label as a person sees it on one line: an ImGui id after "##" is never drawn (ImGui's widgets hide it; the ribbon
// draws its labels itself, and the Sparkline tab showed "Group##spark", its Axis menu "...Sparkline##min")
std::string OneLine(const std::string& s_in) {
  const std::string_view s = std::string_view(s_in).substr(0, s_in.find("##"));
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\n') {
      out.push_back(s[i]);
      continue;
    }
    const bool prev_wide = i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0x80);
    const bool next_wide = i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0x80);
    if (!(prev_wide && next_wide)) out.push_back(' ');
  }
  return out;
}

void DrawIcon(ImDrawList* dl, const char* glyph, ImVec2 center, float base, ImU32 col) {
  if (!glyph) return;
  ImFont* font = F().icons;
  const float px = FontPx(base);
  const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, glyph);
  dl->AddText(font, px, ImVec2(IM_ROUND(center.x - sz.x * 0.5f), IM_ROUND(center.y - sz.y * 0.5f)), col, glyph);
}

ImU32 TextCol(bool enabled);

// the item's picture (RibbonX image) when it has one, otherwise its icon glyph
void DrawItemIcon(ImDrawList* dl, const Item& it, bool large, ImVec2 center, float base, ImU32 col) {
  if (it.image) {
    if (const ImTextureID tex = it.image()) {
      const float half = S(large ? 16.0f : 8.0f);
      dl->AddImage(tex, ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), ImVec2(0, 0), ImVec2(1, 1),
                   col == TextCol(false) ? IM_COL32(255, 255, 255, 110) : IM_COL32_WHITE);
      return;
    }
  }
  DrawIcon(dl, IconOf(it, large), center, base, col);
}

void DrawCaret(ImDrawList* dl, ImVec2 center, ImU32 col) {
  // small triangle (independent of the font)
  const float w = S(3.5f), h = S(2.0f);
  dl->AddTriangleFilled(ImVec2(center.x - w, center.y - h), ImVec2(center.x + w, center.y - h),
                        ImVec2(center.x, center.y + h), col);
}

ImU32 TextCol(bool enabled) { return enabled ? T().btn_text : T().btn_text_disabled; }

// ---- ScreenTip --------------------------------------------------------------
void ScreenTip(const std::string& id) {
  if (ImGui::IsItemHovered()) RequestRedrawIn(ImGui::GetStyle().HoverDelayNormal + 0.05);
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) return;
  Command* c = Cmd(id);
  if (!c) return;
  ImGui::BeginTooltip();
  ImGui::PushFont(F().ui_bold, F().ui_size);
  std::string title = c->tip_title.empty() ? OneLine(c->label) : c->tip_title;
  if (!c->shortcut.empty()) title += " (" + c->shortcut + ")";
  ImGui::TextUnformatted(title.c_str());
  ImGui::PopFont();
  if (!c->tip_desc.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + S(260));
    ImGui::TextUnformatted(c->tip_desc.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::EndTooltip();
}

// ---- Interactive blocks ----------------------------------------------------------------
struct Hit {
  bool hovered = false;
  bool held = false;
  bool clicked = false;
};

Hit HitRegion(const char* id, ImVec2 pos, ImVec2 size, bool enabled = true) {
  ImGui::SetCursorScreenPos(pos);
  ImGui::BeginDisabled(!enabled);
  Hit h;
  h.clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
  h.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && enabled;
  h.held = ImGui::IsItemActive();
  ImGui::EndDisabled();
  return h;
}

void FillState(ImDrawList* dl, ImRect r, const Hit& h, bool checked) {
  const float rounding = S(3.0f);
  if (checked) {
    dl->AddRectFilled(r.Min, r.Max, h.hovered ? T().btn_pressed : T().btn_checked, rounding);
    dl->AddRect(r.Min, r.Max, T().btn_checked_border, rounding);
  } else if (h.held) {
    dl->AddRectFilled(r.Min, r.Max, T().btn_pressed, rounding);
  } else if (h.hovered) {
    dl->AddRectFilled(r.Min, r.Max, T().btn_hover, rounding);
  }
}


// ---- KeyTips (Alt / F10): letters over the tabs, then over the selected tab's commands ----
struct KeyTip {
  std::string key, cmd;
  int tab = -1;             // a tab (level 1)
  std::string popup;        // opens this drop-down of the item instead of running the command
  bool* check = nullptr;    // a check box: toggles it
  std::function<void()> action;  // other targets (Quick Access Toolbar)
};
std::vector<KeyTip> g_kt, g_kt_prev;  // registered while drawing this frame / the last frame
std::string g_kt_typed;
std::string g_kt_open;  // the command whose drop-down a KeyTip opens on the next draw
std::map<std::string, std::string> g_kt_keys;  // command -> key on the selected tab
int g_kt_keys_tab = -1;

// Excel's KeyTips for the commands on the tabs (Excel for Microsoft 365); the others get two letters from their label.
// Keys are by command id: an id must be the one the ribbon item uses (ConditionalFormatting, not a menu's name).
const std::map<std::string, std::string>& FixedKeyTips() {
  static const std::map<std::string, std::string> m = {
      // Home
      {"Paste", "V"}, {"Cut", "X"}, {"Copy", "C"}, {"FormatPainter", "FP"}, {"FontName", "FF"}, {"FontSize", "FS"},
      {"FontSizeIncrease", "FG"}, {"FontSizeDecrease", "FK"}, {"Bold", "1"}, {"Italic", "2"}, {"Underline", "3"}, {"Borders", "B"},
      {"FillColor", "H"}, {"FontColor", "FC"}, {"AlignTop", "AT"}, {"AlignMiddle", "AM"}, {"AlignBottom", "AB"},
      {"TextOrientationMenu", "FQ"}, {"WrapText", "W"}, {"AlignLeft", "AL"}, {"AlignCenter", "AC"}, {"AlignRight", "AR"},
      {"IndentDecrease", "5"}, {"IndentIncrease", "6"}, {"MergeCenter", "M"}, {"NumberFormat", "N"}, {"AccountingFormat", "AN"},
      {"PercentStyle", "P"}, {"CommaStyle", "K"}, {"DecimalIncrease", "0"}, {"DecimalDecrease", "9"}, {"ConditionalFormatting", "L"},
      {"FormatAsTable", "T"}, {"CellStyles", "J"}, {"InsertCells", "I"}, {"DeleteCells", "D"}, {"FormatCellsMenu", "O"},
      {"AutoSum", "U"}, {"FillMenu", "FI"}, {"ClearMenu", "E"}, {"SortFilter", "S"}, {"FindSelect", "FD"},
      {"FormatCellsFontDialog", "FN"}, {"FormatCellsAlignmentDialog", "FA"}, {"FormatCellsNumberDialog", "FM"}, {"ClipboardPane", "FO"},
      // Insert
      {"InsertPivotTable", "V"}, {"RecommendedPivotTables", "SP"}, {"InsertTable", "T"}, {"InsertPicture", "P"}, {"InsertShapesMenu", "SH"},
      {"RecommendedCharts", "R"}, {"InsertChartColumnMenu", "C"}, {"InsertChartLineMenu", "N"},
      {"InsertChartPieMenu", "Q"}, {"InsertChartScatterMenu", "D"}, 
      
      {"InsertSparkLine", "SN"}, {"InsertSparkColumn", "SO"}, {"InsertSparkWinLoss", "SW"},
      {"InsertSlicer", "SF"}, {"InsertHyperlink", "I"}, {"InsertTextBox", "X"},
      {"InsertHeaderFooter", "H"}, {"InsertWordArtMenu", "W"}, {"InsertSymbol", "U"},
      // Page Layout
      {"ThemesMenu", "TH"}, {"ThemeColorsMenu", "TC"}, {"ThemeFontsMenu", "TF"}, {"ThemeEffectsMenu", "TE"}, {"MarginsMenu", "M"},
      {"OrientationMenu", "O"}, {"PaperSizeMenu", "SZ"}, {"PrintAreaMenu", "R"}, {"BreaksMenu", "B"}, {"SheetBackground", "G"},
      {"PrintTitles", "I"}, {"ScaleWidthMenu", "W"}, {"ScaleHeightMenu", "H"}, {"ScaleAdjust", "SC"}, {"PageSetupDialog", "SP"},
      
      // Formulas
      {"InsertFunction", "F"}, {"FunctionsRecentlyUsed", "R"}, {"FunctionsFinancial", "I"}, {"FunctionsLogical", "L"},
      {"FunctionsText", "T"}, {"FunctionsDateTime", "E"}, {"FunctionsLookup", "O"}, {"FunctionsMath", "G"}, {"FunctionsMore", "Q"},
      {"NameManager", "N"}, {"DefineName", "M"}, {"UseInFormula", "S"}, {"CreateNamesFromSelection", "C"}, {"TracePrecedents", "P"},
      {"TraceDependents", "D"}, {"RemoveArrows", "A"}, {"ShowFormulas", "H"}, {"ErrorChecking", "K"}, {"EvaluateFormula", "V"},
      {"WatchWindow", "W"}, {"CalculationOptions", "X"}, {"Calculate", "B"}, {"CalculateSheet", "J"},
      // Data
      {"GetDataMenu", "PN"}, {"ImportText", "FT"}, {"RefreshAll", "RA"}, {"EditLinks", "K"}, {"SortAscending", "SA"},
      {"SortDescending", "SD"}, {"CustomSort", "SS"}, {"ToggleFilter", "T"}, {"ClearFilter", "C"}, {"ReapplyFilter", "Y"},
      {"AdvancedFilter", "Q"}, {"TextToColumns", "E"}, {"FlashFill", "FF"}, {"RemoveDuplicates", "M"}, {"DataValidation", "V"},
      {"Consolidate", "N"}, {"WhatIfMenu", "W"}, {"ForecastSheet", "FC"}, {"GroupOutline", "G"}, {"UngroupOutline", "U"},
      {"Subtotal", "B"}, {"ShowDetail", "J"}, {"HideDetail", "H"}, 
      // Review
      {"Spelling", "S"}, {"ProtectSheet", "PS"}, {"ProtectWorkbook", "PW"}, {"AllowEditRanges", "U"}, {"DeleteCommentThread", "D"},
      {"PreviousComment", "V"}, {"NextComment", "N"}, {"ShowComments", "K"}, 
      {"ShowAllNotes", "TA"}, {"ShowHideNote", "TS"}, {"PreviousNote", "TP"}, {"NextNote", "TN"},
      // View
      {"ViewNormal", "L"}, {"ViewPageBreakPreview", "I"}, {"ViewPageLayout", "P"}, {"CustomViews", "C"}, {"ToggleRuler", "R"},
      {"ToggleGridlines", "VG"}, {"ToggleFormulaBar", "VF"}, {"ToggleHeadings", "VH"}, {"ZoomDialog", "Q"}, {"Zoom100", "J"},
      {"ZoomToSelection", "G"}, {"NewWindow", "N"}, {"ArrangeAll", "A"}, {"FreezePanes", "F"}, {"SplitWindow", "S"}, {"HideWindow", "H"},
      {"UnhideWindow", "U"}, {"ViewSideBySide", "B"}, {"SwitchWindowsMenu", "W"},
      {"ViewMacros", "M"},
      // Developer
      {"OpenVSCode", "V"}, {"Macros", "PM"}, {"MacroSecurity", "AS"}, {"InsertControlsMenu", "I"}, {"DesignMode", "DM"},
      
      // Chart Design
      {"ChartAddElementMenu", "A"}, {"ChartQuickLayoutMenu", "L"}, {"ChartChangeColorsMenu", "H"}, {"ChartStylesMenu", "S"},
      {"ChartSwitchRowCol", "W"}, {"ChartSelectData", "E"}, {"ChartChangeTypeMenu", "C"}, {"MoveChart", "V"},
  };
  return m;
}

// Excel's keys for the entries of the drop-downs a KeyTip opens (the others get their label's first free letter);
// a ">Submenu" entry has one too (Alt, H, O, U opens Hide & Unhide)
const std::map<std::string, std::string>& MenuKeyTips() {
  static const std::map<std::string, std::string> m = {
      // View > Freeze Panes; Page Layout
      {"FreezePanes", "F"}, {"FreezeTopRow", "R"}, {"FreezeFirstColumn", "C"}, {"SetPrintArea", "S"}, {"ClearPrintArea", "C"},
      {"AddToPrintArea", "A"}, {"InsertPageBreak", "I"}, {"RemovePageBreak", "R"}, {"ResetPageBreaks", "A"}, {"MarginsNormal", "N"},
      {"MarginsWide", "W"}, {"MarginsNarrow", "R"}, {"MarginsCustom", "A"}, {"OrientPortrait", "P"}, {"OrientLandscape", "L"},
      // Home > Paste
      {"Paste", "P"}, {"PasteFormulas", "F"}, {"PasteFormulasNumberFormats", "O"}, {"PasteKeepSource", "K"}, {"PasteNoBorders", "B"},
      {"PasteKeepWidths", "W"}, {"PasteTranspose", "T"}, {"PasteValues", "V"}, {"PasteValuesNumberFormats", "A"},
      {"PasteValuesSourceFormats", "E"}, {"PasteFormats", "R"}, {"PasteLink", "N"}, {"PasteAsPicture", "U"}, {"PasteLinkedPicture", "I"},
      {"PasteSpecial", "S"},
      // Home > Copy, Underline
      {"Copy", "C"}, {"CopyAsPicture", "P"}, {"UnderlineDouble", "D"},
      // Home > Borders
      {"BorderBottom", "O"}, {"BorderTop", "P"}, {"BorderLeft", "L"}, {"BorderRight", "R"}, {"BorderNone", "N"}, {"BorderAll", "A"},
      {"BorderOutside", "S"}, {"BorderThickOutside", "T"}, {"BorderDoubleBottom", "B"}, {"BorderThickBottom", "H"}, {"BorderTopBottom", "D"},
      {"BorderTopThickBottom", "C"}, {"BorderTopDoubleBottom", "U"}, {"BorderDraw", "W"}, {"BorderDrawGrid", "G"}, {"BorderErase", "E"},
      {"BorderLineColor", "I"}, {"BorderLineStyle", "Y"}, {"MoreBorders", "M"},
      // Home > Orientation, Merge & Center
      {"AngleCounterclockwise", "O"}, {"AngleClockwise", "L"}, {"VerticalText", "V"}, {"RotateTextUp", "U"}, {"RotateTextDown", "D"},
      {"FormatCellsAlignmentDialog", "M"}, {"MergeCenter", "C"}, {"MergeAcross", "A"}, {"MergeCells", "M"}, {"UnmergeCells", "U"},
      // Home > Conditional Formatting
      {"CfHighlightRules", "H"}, {"CfTopBottomRules", "T"}, {"CfDataBars", "D"}, {"CfColorScales", "S"}, {"CfIconSets", "I"},
      {"CfNewRule", "N"}, {"CfClearRules", "C"}, {"CfManageRules", "R"}, {"CfClearSelection", "S"}, {"CfClearSheet", "E"},
      {"CfClearTable", "T"}, {"CfClearPivot", "P"},
      // Home > Insert, Delete
      {"InsertCells", "I"}, {"InsertRows", "R"}, {"InsertColumns", "C"}, {"InsertSheet", "S"},
      {"DeleteCells", "D"}, {"DeleteRows", "R"}, {"DeleteColumns", "C"}, {"DeleteSheet", "S"},
      // Home > Format (and Hide & Unhide)
      {"RowHeight", "H"}, {"AutoFitRows", "A"}, {"ColumnWidth", "W"}, {"AutoFitColumns", "I"}, {"DefaultWidth", "D"},
      {"HideUnhideMenu", "U"}, {"HideRows", "R"}, {"HideColumns", "C"}, {"HideSheet", "S"}, {"UnhideRows", "O"}, {"UnhideColumns", "L"},
      {"UnhideSheet", "H"}, {"RenameSheet", "R"}, {"CopySheet", "M"}, {"TabColor", "T"}, {"ProtectSheet", "P"}, {"LockCell", "L"},
      {"FormatCellsDialog", "E"},
      // Home > AutoSum, Fill, Clear, Sort & Filter, Find & Select
      {"AutoSum", "S"}, {"AutoAverage", "A"}, {"AutoCount", "C"}, {"AutoMax", "M"}, {"AutoMin", "I"}, {"AutoSumMoreFunctions", "F"},
      {"FillDown", "D"}, {"FillRight", "R"}, {"FillUp", "U"}, {"FillLeft", "L"}, {"FillAcrossWorksheets", "A"}, {"FillSeries", "S"},
      {"FillJustify", "J"}, {"FlashFill", "F"},
      {"ClearAll", "A"}, {"ClearFormats", "F"}, {"ClearContents", "C"}, {"ClearComments", "M"}, {"ClearHyperlinks", "L"},
      {"RemoveHyperlinks", "R"},
      {"SortAscending", "S"}, {"SortDescending", "O"}, {"CustomSort", "U"}, {"ToggleFilter", "F"}, {"ClearFilter", "C"}, {"ReapplyFilter", "Y"},
      {"Find", "F"}, {"Replace", "R"}, {"GoTo", "G"}, {"GoToSpecial", "S"}, {"SelectFormulas", "U"}, {"SelectNotes", "M"},
      {"SelectConditionalFormats", "C"}, {"SelectConstants", "N"}, {"SelectDataValidation", "V"}, {"SelectObjects", "O"},
      {"SelectionPane", "P"},
      // Data
      {"DataValidation", "V"}, {"CircleInvalidData", "I"}, {"ClearValidationCircles", "R"}, {"ScenarioManager", "S"}, {"GoalSeek", "G"},
      {"DataTableDialog", "T"}, {"GroupOutline", "G"}, {"AutoOutline", "A"}, {"UngroupOutline", "U"}, {"ClearOutline", "C"},
      // Formulas
      {"DefineName", "D"}, {"ApplyNames", "A"}, {"PasteName", "P"}, {"RemovePrecedentArrows", "P"}, {"RemoveDependentArrows", "D"},
      {"RemoveArrows", "A"}, {"ErrorChecking", "K"}, {"TraceError", "E"}, {"CircularReferences", "C"}, {"CalcAutomatic", "A"},
      {"CalcSemiautomatic", "E"}, {"CalcManual", "M"},
  };
  return m;
}

// KeyTips in a drop-down a KeyTip opened (level 3): registered by its entries while drawing, typed in KeyTipInput
bool g_kt_menu = false;
std::vector<std::pair<std::string, std::string>> g_kt_menu_keys, g_kt_menu_prev;  // (key, command; ">Id" a submenu)
std::string g_kt_menu_typed;
std::string g_kt_sub_active;    // the submenu a KeyTip opened: only its entries take letters now
std::string g_kt_sub_open;      // ... opened on its next draw
std::string g_kt_sub_drawing;   // the submenu whose entries are being drawn ("" the drop-down itself)
std::set<std::string> g_kt_reserved;  // Excel's letters of the entries of the list being drawn: never given to another entry

std::string KeyTipFor(const std::string& cmd) {
  if (g_kt_keys_tab != g_state.selected_tab) {
    // assign once per tab: Excel's keys first, then two letters for the rest (never a prefix of another key)
    g_kt_keys.clear();
    g_kt_keys_tab = g_state.selected_tab;
    std::vector<std::string> order;
    if (g_state.selected_tab >= 0 && g_state.selected_tab < static_cast<int>(g_tabs.size())) {
      for (const Group& g : g_tabs[static_cast<size_t>(g_state.selected_tab)].groups) {
        for (const Block& b : g.blocks) {
          for (const Item& it : b.items) order.push_back(it.cmd);
          for (const auto& row : b.rows) {
            for (const Item& it : row) order.push_back(it.cmd);
          }
        }
        if (!g.launcher.empty()) order.push_back(g.launcher);
      }
    }
    std::set<std::string> used;
    auto conflicts = [&](const std::string& k) {
      for (const auto& u : used) {
        if (u == k || (u.size() < k.size() && k.compare(0, u.size(), u) == 0) || (k.size() < u.size() && u.compare(0, k.size(), k) == 0)) return true;
      }
      return false;
    };
    std::vector<std::string> rest;
    for (const auto& c : order) {
      auto f = FixedKeyTips().find(c);
      if (f != FixedKeyTips().end() && !conflicts(f->second) && !g_kt_keys.count(c)) {
        g_kt_keys[c] = f->second;
        used.insert(f->second);
      } else if (!g_kt_keys.count(c)) {
        rest.push_back(c);
      }
    }
    for (const auto& c : rest) {
      if (g_kt_keys.count(c)) continue;
      std::string letters;
      // the English label's letters, in every language (a Chinese label has none: its KeyTips were AA, AB...)
      for (char ch : OneLine(i18n::SourceOf(LabelOf(c).c_str()))) {
        if (std::isalnum(static_cast<unsigned char>(ch))) letters += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
      if (letters.empty()) letters = "Y";
      std::string key;
      for (size_t i = 1; i < letters.size() && key.empty(); ++i) {
        const std::string k = std::string(1, letters[0]) + letters[i];
        if (!conflicts(k)) key = k;
      }
      for (char a = 'A'; a <= 'Z' && key.empty(); ++a) {
        for (char b = 'A'; b <= 'Z' && key.empty(); ++b) {
          const std::string k = std::string(1, a) + b;
          if (!conflicts(k)) key = k;
        }
      }
      g_kt_keys[c] = key;
      used.insert(key);
    }
  }
  auto it = g_kt_keys.find(cmd);
  return it == g_kt_keys.end() ? std::string() : it->second;
}

void DrawKeyTipBadge(const std::string& key, ImVec2 at) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImGui::PushFont(F().ui, F().ui_small);
  const ImVec2 ts = ImGui::CalcTextSize(key.c_str());
  const ImRect b(ImVec2(at.x - ts.x * 0.5f - S(4), at.y - ts.y * 0.5f - S(2)), ImVec2(at.x + ts.x * 0.5f + S(4), at.y + ts.y * 0.5f + S(2)));
  const bool live = g_kt_typed.empty() || key.compare(0, g_kt_typed.size(), g_kt_typed) == 0;
  dl->AddRectFilled(b.Min, b.Max, live ? IM_COL32(0x40, 0x40, 0x40, 240) : IM_COL32(0x40, 0x40, 0x40, 90), S(2));
  dl->AddText(ImVec2(b.Min.x + S(4), b.Min.y + S(2)), IM_COL32_WHITE, key.c_str());
  ImGui::PopFont();
}

// after an item is drawn (inside its ID scope): its KeyTip, a drop-down a KeyTip asked for, the right-click menu
// a drop-down opens under its button, left-aligned with it, as Excel's do: set before the popup's Begin. Without it
// ImGui opens a popup where the mouse is, and one opened from the KeyTips (Alt, H, N) showed in the screen's corner
void PopupBelow(const ImRect& r) { ImGui::SetNextWindowPos(ImVec2(r.Min.x, r.Max.y + S(2)), ImGuiCond_Appearing); }
// called inside every drop-down the ribbon shows
void NoteMenuShown() { g_state.menu_frame = ImGui::GetFrameCount(); }

void ItemExtras(const Item& it, const ImRect& r, const char* popup, bool* check = nullptr) {
  // a drop-down ("##menu", "##gallery"), a combo box ("##fonts", "##sizes", "##formats") or a plain button
  Seen(it.cmd, r, !popup ? "ribbon" : (std::strcmp(popup, "##menu") == 0 || std::strcmp(popup, "##gallery") == 0) ? "dropdown" : "combo");
  if (!popup) popup = "";
  // a color button's KeyTip opens its palette, as Excel's (Alt, H, H: Fill Color); a click on its face applies the color
  if (!*popup && it.kind == ItemKind::ColorSplit && it.color) popup = "##palette";
  if (g_kt_open == it.cmd && *popup) {
    ImGui::OpenPopup(popup);
    g_kt_open.clear();
  }
  if (g_state.keytip_level == 2) {
    const std::string key = KeyTipFor(it.cmd);
    if (!key.empty()) {
      KeyTip k;
      k.key = key;
      k.cmd = it.cmd;
      k.popup = popup;
      k.check = check;
      g_kt.push_back(std::move(k));
      DrawKeyTipBadge(key, ImVec2(r.Max.x - S(10) > r.Min.x + S(12) && r.GetWidth() > S(60) ? r.Min.x + S(14) : r.GetCenter().x, r.Max.y - S(2)));
    }
  }
  if (g_state.item_menu && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && r.Contains(ImGui::GetIO().MousePos) &&
      ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
    ImGui::OpenPopup("##itemctx");
  }
  if (ImGui::BeginPopup("##itemctx")) {
    if (g_state.item_menu) g_state.item_menu(it.cmd);
    ImGui::EndPopup();
  }
}

// Excel 2003's menu keys, which Excel still takes after Alt when no tab has the letter: Alt, E, S, V is Paste Special
// and its Values, Alt, O, C, A AutoFits columns. People who learned them type them without looking.
const std::map<std::string, std::string>& LegacyKeys() {
  static const std::map<std::string, std::string> k = {
      {"ES", "PasteSpecial"},  {"ED", "DeleteCells"},     {"EL", "DeleteSheet"},     {"EM", "CopySheet"},
      {"EIS", "FillSeries"},   {"EF", "Find"},            {"EE", "Replace"},         {"EG", "GoTo"},
      {"EAA", "ClearAll"},     {"EAF", "ClearFormats"},   {"EAC", "ClearContents"},  {"IR", "InsertRows"},
      {"IC", "InsertColumns"}, {"IW", "InsertSheet"},     {"IE", "InsertCells"},     {"IF", "InsertFunction"},
      {"IND", "DefineName"},   {"OE", "FormatCellsDialog"}, {"OCA", "AutoFitColumns"}, {"OCW", "ColumnWidth"},
      {"OCH", "HideColumns"},  {"OCU", "UnhideColumns"},  {"ORA", "AutoFitRows"},    {"ORE", "RowHeight"},
      {"ORH", "HideRows"},     {"ORU", "UnhideRows"},     {"OHR", "RenameSheet"},    {"DS", "CustomSort"},
      {"DFF", "ToggleFilter"}, {"DE", "TextToColumns"},   {"DL", "DataValidation"},  {"DB", "Subtotal"},
      {"DP", "InsertPivotTable"}, {"DGG", "GroupOutline"}, {"DGU", "UngroupOutline"}, {"VZ", "ZoomDialog"}};
  return k;
}
// 0: no such sequence, 1: the start of one, 2: a whole one (cmd set)
int LegacyMatch(const std::string& typed, std::string* cmd) {
  int m = 0;
  for (const auto& [key, c] : LegacyKeys()) {
    if (key == typed) {
      *cmd = c;
      return 2;
    }
    if (key.compare(0, typed.size(), typed) == 0) m = 1;
  }
  return m;
}

// typed KeyTip letters: run the match (previous frame's KeyTips); returns true while KeyTips stay on
void KeyTipInput() {
  ImGuiIO& io = ImGui::GetIO();
  // a drop-down opened by a KeyTip: its entries' letters run them (Alt, W, F, F = Freeze Panes)
  g_kt_menu_prev = std::move(g_kt_menu_keys);
  g_kt_menu_keys.clear();
  if (g_kt_menu) {
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && g_kt_open.empty()) g_kt_menu = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) g_kt_menu = false;
    for (int i = 0; g_kt_menu && i < io.InputQueueCharacters.Size; ++i) {
      const ImWchar c = io.InputQueueCharacters[i];
      if (c < 128 && std::isalnum(static_cast<int>(c))) g_kt_menu_typed += static_cast<char>(std::toupper(static_cast<int>(c)));
    }
    if (g_kt_menu) io.InputQueueCharacters.resize(0);
    bool partial = false;
    for (const auto& [key, cmd] : g_kt_menu_prev) {
      if (!g_kt_menu || g_kt_menu_typed.empty()) break;
      if (key == g_kt_menu_typed) {
        g_kt_menu_typed.clear();
        if (!cmd.empty() && cmd[0] == '>') {
          // a submenu (Alt, H, O, U: Hide & Unhide): it opens and its entries take the next letters
          g_kt_sub_active = g_kt_sub_open = cmd.substr(1);
          partial = true;
          break;
        }
        g_kt_menu = false;
        g_kt_sub_active.clear();
        ImGui::ClosePopupsOverWindow(nullptr, false);
        Exec(cmd);
        return;
      }
      if (key.compare(0, g_kt_menu_typed.size(), g_kt_menu_typed) == 0) partial = true;
    }
    if (!partial) g_kt_menu_typed.clear();
    if (g_kt_menu) return;
  }
  g_kt_sub_active.clear();
  g_kt_sub_open.clear();
  static bool alt_tap = false;
  const bool alt_now = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);
  bool other = false;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    const ImGuiKey key = static_cast<ImGuiKey>(k);
    if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_ReservedForModAlt || key == ImGuiKey_ReservedForModCtrl ||
        key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModSuper || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift)
      continue;
    if (ImGui::IsKeyPressed(key, false)) other = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_LeftAlt, false) || ImGui::IsKeyPressed(ImGuiKey_RightAlt, false)) alt_tap = true;
  if (other || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) alt_tap = alt_tap && !other && !ImGui::IsMouseClicked(0) && !ImGui::IsMouseClicked(1);
  // Alt + a letter (no Ctrl) goes straight to that tab's KeyTips, like Excel's Alt+H, or starts Excel 2003's menu keys
  // (Alt+E). The tabs register their KeyTips only while KeyTips show, so the letter is matched on the next frame, once
  // they do: matched at once against none, a held Alt+W did nothing and the F, F that followed were typed into a cell
  static bool defer = false;
  if (alt_now && !io.KeyCtrl && g_state.keytip_level == 0 && !io.WantTextInput) {
    for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
      if (k == ImGuiKey_Q || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) continue;
      g_state.keytip_level = 1;
      g_kt_typed = std::string(1, static_cast<char>('A' + (k - ImGuiKey_A)));
      io.InputQueueCharacters.resize(0);
      defer = true;
    }
  }
  if (!alt_now && alt_tap && (ImGui::IsKeyReleased(ImGuiKey_LeftAlt) || ImGui::IsKeyReleased(ImGuiKey_RightAlt))) {
    alt_tap = false;
    g_state.keytip_level = g_state.keytip_level ? 0 : 1;
    g_kt_typed.clear();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_F10, false) && !io.KeyShift) {
    g_state.keytip_level = g_state.keytip_level ? 0 : 1;
    g_kt_typed.clear();
  }
  if (!g_state.keytip_level) {
    g_state.legacy_keys.clear();
    return;
  }
  if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || g_state.backstage) {
    g_state.keytip_level = 0;
    g_state.legacy_keys.clear();
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (!g_state.legacy_keys.empty()) g_state.keytip_level = 0;  // Excel: Esc cancels the 2003 keys
    else if (!g_kt_typed.empty()) g_kt_typed.clear();
    else g_state.keytip_level = g_state.keytip_level == 2 ? 1 : 0;
    g_kt_typed.clear();
    g_state.legacy_keys.clear();
    io.InputQueueCharacters.resize(0);
    return;
  }
  if (defer) {  // a held Alt's letter: the tabs' KeyTips are registered by the time it is matched
    defer = false;
    io.InputQueueCharacters.resize(0);
    return;
  }
  // the letters typed (the grid doesn't get them while KeyTips are on)
  for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
    const ImWchar c = io.InputQueueCharacters[i];
    if (c < 128 && std::isalnum(static_cast<int>(c))) g_kt_typed += static_cast<char>(std::toupper(static_cast<int>(c)));
  }
  io.InputQueueCharacters.resize(0);
  if (g_kt_typed.empty()) return;
  bool partial = false;
  for (const auto& t : g_kt_prev) {
    if (t.key == g_kt_typed) {
      g_kt_typed.clear();
      if (t.tab >= 0) {
        if (g_tabs[static_cast<size_t>(t.tab)].backstage) {
          g_state.backstage = true;
          g_state.backstage_page = 6;  // File opens on Home, as Excel's does
          g_state.keytip_level = 0;
        } else {
          g_state.selected_tab = t.tab;
          g_state.keytip_level = 2;
          if (g_state.collapsed) g_state.peek = true;
        }
      } else {
        g_state.keytip_level = 0;
        if (t.action) {
          t.action();
        } else if (!t.popup.empty()) {
          g_kt_open = t.cmd;
          g_kt_menu = true;  // its entries show KeyTips too
          g_kt_menu_typed.clear();
        } else {
          if (t.check) *t.check = !*t.check;
          Exec(t.cmd);
        }
      }
      return;
    }
    if (t.key.compare(0, g_kt_typed.size(), g_kt_typed) == 0) partial = true;
  }
  if (!partial && g_state.keytip_level == 1) {  // no tab: Excel 2003's menu keys
    std::string cmd;
    const int m = LegacyMatch(g_kt_typed, &cmd);
    if (m == 2) {
      g_kt_typed.clear();
      g_state.legacy_keys.clear();
      g_state.keytip_level = 0;
      Exec(cmd);
      return;
    }
    if (m == 1) {
      g_state.legacy_keys = g_kt_typed;
      return;
    }
    if (!g_state.legacy_keys.empty()) {  // a letter no sequence has: Excel leaves the keys, as Esc
      g_state.legacy_keys.clear();
      g_kt_typed.clear();
      g_state.keytip_level = 0;
      return;
    }
  }
  if (!partial) g_kt_typed.clear();  // no such KeyTip: start over
}

// ---- Drop-down menu contents --------------------------------------------------------------
// an entry's KeyTip in a drop-down a KeyTip opened: Excel's key, or the first letter of its label that no other entry
// of the list has (taken or reserved as Excel's). While a KeyTip opened a submenu, only its entries get letters.
void MenuKeyTip(const std::string& target, const std::string& label, ImVec2 at) {
  if (!g_kt_menu) return;
  if (g_kt_sub_drawing != g_kt_sub_active) return;
  const std::string id = !target.empty() && target[0] == '>' ? target.substr(1) : target;
  auto taken = [](const std::string& k) {
    for (const auto& e : g_kt_menu_keys) {
      if (e.first == k) return true;
    }
    return false;
  };
  std::string key;
  const auto f = MenuKeyTips().find(id);
  if (f != MenuKeyTips().end() && !taken(f->second)) key = f->second;
  // the first free letter of the English label, in every language: a Chinese label has none, and Alt, H, L, H, G
  // reached nothing in the Chinese UI
  for (char ch : std::string(i18n::SourceOf(label.c_str()))) {
    if (!key.empty()) break;
    const std::string k(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    if (std::isalnum(static_cast<unsigned char>(ch)) && !taken(k) && !g_kt_reserved.count(k)) key = k;
  }
  if (key.empty()) return;
  g_kt_menu_keys.push_back({key, target});
  DrawKeyTipBadge(key, at);
}

bool MenuEntry(const std::string& id) {
  Command* c = Cmd(id);
  const std::string label = c ? OneLine(c->label) : id;
  const char* icon = c ? c->icon : nullptr;
  const float h = std::max(S(26.0f), g_state.menu_entry_height ? g_state.menu_entry_height(id) : 0.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushID(id.c_str());
  const bool enabled = !c || c->enabled;
  ImGui::BeginDisabled(!enabled);
  // a picture instead of the icon (the app's menu_picture): measured first, so the entry is wide enough
  float pic_w = 0.0f;
  if (g_state.menu_picture) pic_w = g_state.menu_picture(id, nullptr, p, h);  // dl nullptr: only its width
  const float label_x = pic_w > 0 ? S(8) + pic_w + S(10) : S(32);
  const bool clicked = ImGui::Selectable("##entry", c && c->toggle && c->pressed, 0,
                                         ImVec2(std::max(S(200.0f), ImGui::CalcTextSize(label.c_str()).x + label_x + S(28)), h));
  if (enabled && ImGui::IsItemHovered()) g_state.hovered_tile = id;  // a drop-down gallery's entry: the app previews it
  ImGui::EndDisabled();
  Seen(id, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), "menu");
  ImGui::PopID();
  if (pic_w > 0) g_state.menu_picture(id, dl, ImVec2(p.x + S(8), p.y), h);
  else DrawIcon(dl, icon, ImVec2(p.x + S(14), p.y + h * 0.5f), F().icon_small, TextCol(enabled));
  const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
  dl->AddText(ImVec2(p.x + label_x, ty), TextCol(enabled), label.c_str());
  if (c && !c->shortcut.empty()) {
    const float sw = ImGui::CalcTextSize(c->shortcut.c_str()).x;
    dl->AddText(ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - sw - S(16), ty), T().group_label,
                c->shortcut.c_str());
  }
  if (clicked) {
    Exec(id);
    g_menu_ran = id;
  }
  if (enabled) MenuKeyTip(id, label, ImVec2(p.x + S(14), p.y + h * 0.5f));
  return clicked;
}

// a gallery section of a drop-down (Excel's Shapes, Icons): its heading, then its commands as square tiles in rows,
// each drawn by the app's menu_picture, its label in the tooltip
void MenuTiles(const std::string& heading, const std::vector<std::string>& ids) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 hp = ImGui::GetCursorScreenPos();
  const float row_w = S(12 * 26.0f);
  if (!heading.empty()) {
    dl->AddRectFilled(hp, ImVec2(hp.x + row_w + S(8), hp.y + ImGui::GetFontSize() + S(6)), T().header_bg);
    dl->AddText(ImVec2(hp.x + S(6), hp.y + S(3)), TextCol(true), heading.c_str());
    ImGui::Dummy(ImVec2(row_w + S(8), ImGui::GetFontSize() + S(8)));
  }
  const float custom = !heading.empty() && !ids.empty() && g_state.menu_tile_size ? g_state.menu_tile_size(ids.front()) : 0.0f;
  const float tile = heading.empty() ? S(52) : custom > 0 ? S(custom) : S(26);
  const int per_row = heading.empty() ? 5 : std::max(1, static_cast<int>(row_w / tile));
  const ImVec2 start(hp.x + S(4), ImGui::GetCursorScreenPos().y);
  int col = 0;
  for (const std::string& id : ids) {
    Command* c = Cmd(id);
    const ImVec2 p(start.x + static_cast<float>(col) * tile, ImGui::GetCursorScreenPos().y);
    ImGui::SetCursorScreenPos(p);
    ImGui::PushID(id.c_str());
    const bool clicked = ImGui::Selectable("##tile", false, 0, ImVec2(tile - S(2), tile - S(2)));
    const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    Seen(id, r, "menu");
    ImGui::PopID();
    if (g_state.menu_picture) g_state.menu_picture(id, dl, ImVec2(r.Min.x + S(2), r.Min.y + S(2)), r.GetHeight() - S(4));
    else DrawIcon(dl, c ? c->icon : nullptr, r.GetCenter(), F().icon_small, TextCol(true));
    if (ImGui::IsItemHovered() && c) ImGui::SetTooltip("%s", OneLine(c->label).c_str());
    if (ImGui::IsItemHovered()) g_state.hovered_tile = id;  // the app previews it (a chart gallery's tile on the sheet)
    MenuKeyTip(id, c ? OneLine(c->label) : id, ImVec2(r.Min.x + S(8), r.Max.y - S(6)));
    if (clicked) {
      Exec(id);
      g_menu_ran = id;
      ImGui::CloseCurrentPopup();
    }
    if (++col == per_row) {
      col = 0;
    } else {
      ImGui::SameLine(0, 0);
    }
  }
  if (col) ImGui::NewLine();
  ImGui::Dummy(ImVec2(0, S(2)));
}

// a menu's entries: command ids, "-" a separator, submenus as ">Id" ... "<" (Id a command for its label, or the
// label), and gallery sections as "=Heading" followed by the commands shown as tiles (up to the next "=", "-" or "<")
void MenuPopupBody(const std::vector<std::string>& menu) {
  // Excel's letters of this list's entries are kept for them: an entry drawn before one of them doesn't take its letter
  const std::set<std::string> outer_reserved = std::move(g_kt_reserved);
  g_kt_reserved.clear();
  if (g_kt_menu) {
    for (size_t i = 0, depth = 0; i < menu.size(); ++i) {
      const std::string& e = menu[i];
      if (e == "<") {
        depth = depth ? depth - 1 : 0;
        continue;
      }
      const bool sub = !e.empty() && e[0] == '>';
      if (depth == 0) {
        const auto f = MenuKeyTips().find(sub ? e.substr(1) : e);
        if (f != MenuKeyTips().end()) g_kt_reserved.insert(f->second);
      }
      if (sub) ++depth;
    }
  }
  struct Restore {
    std::set<std::string> v;
    ~Restore() { g_kt_reserved = std::move(v); }
  } restore{outer_reserved};
  for (size_t i = 0; i < menu.size(); ++i) {
    const std::string& id = menu[i];
    if (id == "-") {
      ImGui::Separator();
      continue;
    }
    if (!id.empty() && id[0] == '~') {
      // a section title, as Excel's Format menu has (Cell Size, Visibility, Organize Sheets, Protection)
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 hp = ImGui::GetCursorScreenPos();
      const float w = std::max(S(200.0f), ImGui::GetContentRegionAvail().x);
      dl->AddRectFilled(hp, ImVec2(hp.x + w, hp.y + ImGui::GetFontSize() + S(6)), T().header_bg);
      ImGui::PushFont(F().ui_bold, F().ui_size);
      dl->AddText(ImVec2(hp.x + S(6), hp.y + S(3)), TextCol(true), id.c_str() + 1);
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() + S(6)));
      continue;
    }
    if (!id.empty() && id[0] == '#') {
      // Excel's palette in a drop-down (Sparkline Color, Marker Color > High Point): a pick runs the command
      ImGui::PushID(id.c_str());
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(2), S(2)));
      uint32_t ref = 0;
      if (PaletteSwatches(&ref, nullptr)) {
        g_state.menu_color = ref;
        Exec(id.substr(1));
        g_menu_ran = id.substr(1);
        ImGui::CloseCurrentPopup();
      }
      ImGui::PopStyleVar();
      ImGui::PopID();
      continue;
    }
    if (!id.empty() && id[0] == '=') {
      std::vector<std::string> tiles;
      size_t end = i + 1;
      for (; end < menu.size() && !menu[end].empty() && menu[end][0] != '=' && menu[end] != "-" && menu[end][0] != '>' && menu[end] != "<"; ++end) tiles.push_back(menu[end]);
      ImGui::PushID(static_cast<int>(i));
      MenuTiles(id.substr(1), tiles);
      ImGui::PopID();
      i = end - 1;
      continue;
    }
    if (!id.empty() && id[0] == '>') {
      size_t end = i + 1;
      for (int depth = 1; end < menu.size(); ++end) {
        if (!menu[end].empty() && menu[end][0] == '>') ++depth;
        else if (menu[end] == "<" && --depth == 0) break;
      }
      const std::string sub = id.substr(1);
      Command* c = Cmd(sub);
      const std::string label = c ? OneLine(c->label) : sub;
      ImGui::PushID(sub.c_str());
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(32), S(5)));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(5)));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(p.x + S(32), p.y + S(5)));  // the text lines up with the entries' labels
      if (end - i > 25) ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetMainViewport()->Size.y * 0.7f));
      // a KeyTip typed for this submenu opens it (Excel: Alt, H, O, U)
      if (g_kt_sub_open == sub) {
        ImGui::OpenPopup(label.c_str());
        g_kt_sub_open.clear();
      }
      const bool open = ImGui::BeginMenu(label.c_str());
      Seen(sub, ImRect(ImVec2(p.x, p.y), ImVec2(p.x + std::max(S(200.0f), ImGui::GetItemRectSize().x + S(40)), ImGui::GetItemRectMax().y + S(5))), "submenu");
      ImGui::PopStyleVar(2);
      MenuKeyTip(">" + sub, label, ImVec2(p.x + S(14), p.y + S(13)));
      if (open) {
        const std::string outer = g_kt_sub_drawing;
        g_kt_sub_drawing = sub;
        if (!(g_state.submenu_body && g_state.submenu_body(sub))) {
          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
          MenuPopupBody(std::vector<std::string>(menu.begin() + static_cast<std::ptrdiff_t>(i) + 1, menu.begin() + static_cast<std::ptrdiff_t>(end)));
          ImGui::PopStyleVar();
        }
        g_kt_sub_drawing = outer;
        ImGui::EndMenu();
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetCursorScreenPos().y, p.y + S(26.0f))));
      // a moved cursor needs an item after it (ImGui's check): the last entry has none (Add Chart Element > Trendline)
      if (end + 1 >= menu.size()) ImGui::Dummy(ImVec2(0.0f, 0.0f));
      ImGui::PopID();
      i = end;
      continue;
    }
    if (g_state.menu_entry_shown && !g_state.menu_entry_shown(id)) continue;  // (an entry the app leaves out now)
    if (MenuEntry(id)) ImGui::CloseCurrentPopup();
  }
}

void MenuPopup(const char* popup_id, const std::vector<std::string>& menu) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  // a long menu (Formulas > Math & Trig) scrolls instead of running off the screen; a submenu's entries don't count
  size_t shown = 0;
  for (size_t i = 0, depth = 0; i < menu.size(); ++i) {
    if (!menu[i].empty() && menu[i][0] == '>') shown += depth++ == 0 ? 1 : 0;
    else if (menu[i] == "<") depth = depth ? depth - 1 : 0;
    else shown += depth == 0 ? 1 : 0;
  }
  if (shown > 24) ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetMainViewport()->Size.y * 0.7f));
  if (ImGui::BeginPopup(popup_id)) {
    NoteMenuShown();
    MenuPopupBody(menu);
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
}

// ---- Color picker (Excel's palette) ---------------------------------------
// the shades of each theme column, as Excel's palette: Background 1 darker, Text 1 lighter, Background 2 darker
// by its own steps, the others lighter 80 / 60 / 40 % and darker 25 / 50 %
double PaletteTint(int col, int row) {
  static const double kLight1[5] = {-0.05, -0.15, -0.25, -0.35, -0.5};
  static const double kDark1[5] = {0.5, 0.35, 0.25, 0.15, 0.05};
  static const double kLight2[5] = {-0.1, -0.25, -0.5, -0.75, -0.9};
  static const double kOther[5] = {0.8, 0.6, 0.4, -0.25, -0.5};
  if (row <= 0) return 0.0;
  const int k = std::min(row, 5) - 1;
  return col == 0 ? kLight1[k] : col == 1 ? kDark1[k] : col == 2 ? kLight2[k] : kOther[k];
}

std::string PaletteName(int col, int row) {
  const char* kCols[10] = {TR("Background 1"), TR("Text 1"), TR("Background 2"), TR("Text 2"), TR("Accent 1"), TR("Accent 2"), TR("Accent 3"), TR("Accent 4"), TR("Accent 5"), TR("Accent 6")};
  std::string name = kCols[col];
  const double t = PaletteTint(col, row);
  if (t != 0.0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), t > 0 ? TR(", Lighter %d%%") : TR(", Darker %d%%"), static_cast<int>(std::lround(std::fabs(t) * 100)));
    name += buf;
  }
  return name;
}

}  // namespace

bool PaletteSwatches(uint32_t* ref, const char* none_label) {
  // the palette's columns: Light 1, Dark 1, Light 2, Dark 2, Accent 1-6 of the workbook's theme
  static const int kCols[10] = {model::kLight1, model::kDark1, model::kLight2, model::kDark2, model::kAccent1,
                                model::kAccent2, model::kAccent3, model::kAccent4, model::kAccent5, model::kAccent6};
  static const uint32_t kStandard[10] = {0xFFC00000, 0xFFFF0000, 0xFFFFC000, 0xFFFFFF00, 0xFF92D050, 0xFF00B050, 0xFF00B0F0, 0xFF0070C0, 0xFF002060, 0xFF7030A0};
  const char* kStandardNames[10] = {TR("Dark Red"), TR("Red"), TR("Orange"), TR("Yellow"), TR("Light Green"), TR("Green"), TR("Light Blue"), TR("Blue"), TR("Dark Blue"), TR("Purple")};
  bool picked = false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cell = S(16.0f), gap = S(4.0f);
  if (none_label) {
    // No Fill / Automatic: a swatch-less entry above the grid, as Excel's
    if (ImGui::Selectable(none_label, false, 0, ImVec2(cell * 10 + gap * 9, 0))) {
      *ref = 0;
      picked = true;
    }
    ImGui::Dummy(ImVec2(1, gap));
  }
  auto swatch = [&](uint32_t value, uint32_t rgb, int id, const std::string& name) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##sw", ImVec2(cell, cell));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    NoteUiItem(name.c_str(), ImRect(p, ImVec2(p.x + cell, p.y + cell)));
    if (hovered) ImGui::SetTooltip("%s", name.c_str());
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255));
    dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), hovered ? IM_COL32(242, 148, 0, 255) : IM_COL32(200, 200, 200, 255),
                0.0f, hovered ? S(2) : 1.0f);  // 1.92.8+: (rounding, thickness, flags)
    if (clicked) {
      *ref = value;
      picked = true;
    }
  };
  ImGui::TextUnformatted(TR("Theme Colors"));
  const model::Theme& theme = model::CurrentTheme();
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 10; ++i) {
      const uint32_t r = model::MakeThemeColor(kCols[i], PaletteTint(i, row));
      if (i > 0) ImGui::SameLine(0, gap);
      swatch(r, model::ResolveColor(r, theme), row * 10 + i, PaletteName(i, row));
    }
    if (row == 0) ImGui::Dummy(ImVec2(1, gap));
  }
  ImGui::Dummy(ImVec2(1, gap));
  ImGui::TextUnformatted(TR("Standard Colors"));
  for (int i = 0; i < 10; ++i) {
    if (i > 0) ImGui::SameLine(0, gap);
    swatch(kStandard[i], kStandard[i], 100 + i, kStandardNames[i]);
  }
  ImGui::Dummy(ImVec2(1, gap));
  return picked;
}

namespace {

void ColorPalettePopup(const char* popup_id, ImU32* color, uint32_t* theme_ref, const std::string& cmd) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(8), S(8)));
  if (ImGui::BeginPopup(popup_id)) {
    NoteMenuShown();
    // Fill Color starts with No Fill, Font Color with Automatic (that is how a fill or a color is taken off)
    const char* none = cmd == "FillColor" ? TR("No Fill") : cmd == "FontColor" ? TR("Automatic") : nullptr;
    uint32_t ref = 0;
    if (PaletteSwatches(&ref, none)) {
      if (ref == 0) {
        *color = 0;
        if (theme_ref) *theme_ref = 0;
      } else {
        const uint32_t rgb = model::ResolveColor(ref);
        *color = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
        if (theme_ref) *theme_ref = model::IsThemeColor(ref) ? ref : 0;
      }
      Exec(cmd);
      ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    static ImVec4 custom(1, 0, 0, 1);
    if (ImGui::TreeNode(TR("More Colors..."))) {
      ImGui::SetNextItemWidth(S(200));
      if (ImGui::ColorPicker4("##picker", &custom.x, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoSidePreview)) {
        *color = ImGui::ColorConvertFloat4ToU32(custom);
        if (theme_ref) *theme_ref = 0;
      }
      if (ImGui::Button(TR("OK"))) {
        Exec(cmd);
        ImGui::CloseCurrentPopup();
      }
      ImGui::TreePop();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

// ---- Gallery (Cell Styles): preview tiles ---------------------------------------------
const std::vector<GalleryTile>& GalleryOf(const std::string& id) {
  static const std::vector<GalleryTile> kEmpty;
  auto it = g_galleries.find(id);
  return it == g_galleries.end() ? kEmpty : it->second;
}

// one border edge of a tile, drawn inside the tile like a cell border
void TileEdge(ImDrawList* dl, const ImRect& r, int side, const GalleryTile::Edge& e) {
  if (e.width <= 0 && !e.dbl) return;
  const float t = e.dbl ? 1.0f : S(static_cast<float>(e.width));
  auto bar = [&](float off) {
    switch (side) {
      case 0: dl->AddRectFilled(ImVec2(r.Min.x + off, r.Min.y), ImVec2(r.Min.x + off + t, r.Max.y), e.col); break;
      case 1: dl->AddRectFilled(ImVec2(r.Min.x, r.Min.y + off), ImVec2(r.Max.x, r.Min.y + off + t), e.col); break;
      case 2: dl->AddRectFilled(ImVec2(r.Max.x - off - t, r.Min.y), ImVec2(r.Max.x - off, r.Max.y), e.col); break;
      default: dl->AddRectFilled(ImVec2(r.Min.x, r.Max.y - off - t), ImVec2(r.Max.x, r.Max.y - off), e.col); break;
    }
  };
  bar(0.0f);
  if (e.dbl) bar(std::max(2.0f, S(2)));
}

void DrawTileFace(ImDrawList* dl, const GalleryTile& tile, const ImRect& r) {
  if (tile.picture) {
    dl->AddRectFilled(r.Min, r.Max, IM_COL32_WHITE);
    tile.picture(dl, r);
    return;
  }
  dl->AddRectFilled(r.Min, r.Max, tile.fill ? tile.fill : IM_COL32_WHITE);
  for (int side = 0; side < 4; ++side) TileEdge(dl, r, side, tile.edge[side]);
  ImFont* font = CellFont("Calibri", tile.bold, tile.italic);
  const float px = FontPx(13.0f) * std::clamp(tile.font_scale, 0.8f, 1.4f);
  const std::string label = OneLine(LabelOf(tile.cmd));
  const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label.c_str());
  const ImVec2 tp(IM_ROUND(r.Min.x + S(5)), IM_ROUND(r.GetCenter().y - sz.y * 0.5f));
  dl->PushClipRect(ImVec2(r.Min.x + S(2), r.Min.y), ImVec2(r.Max.x - S(3), r.Max.y), true);
  dl->AddText(font, px, tp, tile.text, label.c_str());
  if (tile.underline) dl->AddRectFilled(ImVec2(tp.x, tp.y + sz.y - S(2)), ImVec2(tp.x + sz.x, tp.y + sz.y - S(1)), tile.text);
  dl->PopClipRect();
}

std::string g_tile_gallery;  // the gallery whose tiles are being drawn (for the right-click menu)

// a clickable tile; hovering draws Excel's highlight frame
bool TileButton(const GalleryTile& tile, const ImRect& r, int index) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(index);
  const bool enabled = IsEnabled(tile.cmd);
  const Hit h = HitRegion("##tile", r.Min, r.GetSize(), enabled);
  if (ImGui::GetCurrentWindow()->ClipRect.Contains(r)) Seen(tile.cmd, r, "tile");  // a tile scrolled (partly) out of the gallery isn't there to click
  ScreenTip(tile.cmd);
  ImGui::PopID();
  DrawTileFace(dl, tile, r);
  if (h.hovered) g_state.hovered_tile = tile.cmd;
  if (h.hovered) {
    dl->AddRect(ImVec2(r.Min.x - 1, r.Min.y - 1), ImVec2(r.Max.x + 1, r.Max.y + 1), T().accent, 0.0f, std::max(1.0f, S(2)));
  }
  if (h.clicked) Exec(tile.cmd);
  if (g_state.tile_menu && ImGui::BeginPopupContextItem("##tilemenu")) {
    g_state.tile_menu(g_tile_gallery, tile.cmd);
    ImGui::EndPopup();
  }
  return h.clicked;
}

// the expanded gallery: every tile grouped under its section heading (Excel's Cell Styles drop-down)
void GalleryPopup(const char* popup_id, const std::string& id) {
  const auto& tiles = GalleryOf(id);
  g_tile_gallery = id;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6), S(6)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  // a gallery of pictures taller than the window scrolls (Format as Table)
  if (g_gallery_layouts.count(id)) ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetMainViewport()->Size.y * 0.75f));
  if (ImGui::BeginPopup(popup_id)) {
    NoteMenuShown();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto lay = g_gallery_layouts.find(id);
    const int ncols = lay != g_gallery_layouts.end() ? lay->second.cols : kPopupCols;
    const float tw = lay != g_gallery_layouts.end() ? S(lay->second.w) : S(kTileW + 12), th = lay != g_gallery_layouts.end() ? S(lay->second.h) : S(kTileH - 4);
    const float gap = S(5);
    const float width = tw * ncols + gap * (ncols - 1);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float y = o.y;
    int col = 0;
    std::string section = "\x01";
    for (size_t i = 0; i < tiles.size(); ++i) {
      const GalleryTile& t = tiles[i];
      if (t.section != section) {
        if (col > 0) y += th + gap;
        col = 0;
        section = t.section;
        if (!section.empty()) {
          const float hh = S(22);
          dl->AddRectFilled(ImVec2(o.x - S(6), y), ImVec2(o.x + width + S(6), y + hh), T().btn_hover);
          ImGui::PushFont(F().ui_bold, F().ui_size);
          dl->AddText(ImVec2(o.x, y + (hh - ImGui::GetFontSize()) * 0.5f), T().btn_text, section.c_str());
          ImGui::PopFont();
          y += hh + gap;
        }
      }
      const float x = o.x + (tw + gap) * static_cast<float>(col);
      if (TileButton(t, ImRect(ImVec2(x, y), ImVec2(x + tw, y + th)), static_cast<int>(i))) ImGui::CloseCurrentPopup();
      if (++col == ncols) {
        col = 0;
        y += th + gap;
      }
    }
    if (col > 0) y += th + gap;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(width, std::max(1.0f, y - o.y - gap)));
    if (auto ft = g_gallery_footers.find(id); ft != g_gallery_footers.end()) {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(4), S(4)));
      ImGui::Separator();
      for (const auto& cmd : ft->second) {
        const Command* c = Commands().Find(cmd);
        if (c && ImGui::Selectable(c->label.c_str())) {
          Exec(cmd);
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::PopStyleVar();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
}

float GalleryWidth(int cols) { return S(kTileW) * cols + S(kTileGap) * (cols - 1) + S(kGalleryScrollW) + S(4); }

// in-ribbon gallery: two rows of tiles, scrolled a row at a time; the last button expands the whole gallery
void DrawGallery(const Item& it, ImRect r, int cols) {
  g_tile_gallery = it.gallery;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& tiles = GalleryOf(it.gallery);
  const int total_rows = std::max(1, (static_cast<int>(tiles.size()) + cols - 1) / cols);
  constexpr int kRows = 2;
  int& first = g_gallery_row[it.gallery + "#" + std::to_string(cols)];
  first = std::clamp(first, 0, std::max(0, total_rows - kRows));
  ImGui::PushID(it.cmd.c_str());
  const ImRect frame(r.Min, ImVec2(r.Max.x, r.Min.y + S(kTileH) * kRows + S(kTileGap) * (kRows + 1)));
  dl->AddRectFilled(frame.Min, frame.Max, IM_COL32_WHITE);
  dl->AddRect(frame.Min, frame.Max, IM_COL32(200, 200, 200, 255));
  // tiles
  for (int row = 0; row < kRows; ++row) {
    for (int c = 0; c < cols; ++c) {
      const int i = (first + row) * cols + c;
      if (i >= static_cast<int>(tiles.size())) break;
      const float x = frame.Min.x + S(kTileGap) + (S(kTileW) + S(kTileGap)) * static_cast<float>(c);
      const float y = frame.Min.y + S(kTileGap) + (S(kTileH) + S(kTileGap)) * static_cast<float>(row);
      TileButton(tiles[static_cast<size_t>(i)], ImRect(ImVec2(x, y), ImVec2(x + S(kTileW), y + S(kTileH))), i);
    }
  }
  // scroll column: previous row, next row, expand
  const float sx = frame.Max.x - S(kGalleryScrollW);
  const float bh = frame.GetHeight() / 3.0f;
  dl->AddRectFilled(ImVec2(sx, frame.Min.y + 1), ImVec2(sx + 1, frame.Max.y - 1), IM_COL32(220, 220, 220, 255));
  auto scroll_btn = [&](const char* id, int k, bool enabled, const char* tip) {
    const ImRect br(ImVec2(sx + 1, frame.Min.y + bh * static_cast<float>(k)), ImVec2(frame.Max.x - 1, frame.Min.y + bh * static_cast<float>(k + 1)));
    const Hit h = HitRegion(id, br.Min, br.GetSize(), enabled);
    if (h.hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    FillState(dl, br, h, false);
    const ImU32 col = TextCol(enabled);
    const ImVec2 c = br.GetCenter();
    const float w = S(3.5f), hh = S(2.0f);
    if (k == 0) dl->AddTriangleFilled(ImVec2(c.x - w, c.y + hh), ImVec2(c.x + w, c.y + hh), ImVec2(c.x, c.y - hh), col);
    else DrawCaret(dl, c, col);
    if (k == 2) dl->AddRectFilled(ImVec2(c.x - w, c.y - hh - S(3)), ImVec2(c.x + w, c.y - hh - S(2)), col);
    return h.clicked;
  };
  if (scroll_btn("##up", 0, first > 0, TR("Previous row"))) --first;
  if (scroll_btn("##down", 1, first + kRows < total_rows, TR("Next row"))) ++first;
  if (scroll_btn("##more", 2, true, TR("More"))) ImGui::OpenPopup("##gallery");
  if (g_open_gallery == it.gallery) {  // OpenGallery (the Cell Styles command run by a key or from Python)
    ImGui::OpenPopup("##gallery");
    g_open_gallery.clear();
  }
  // open below the in-ribbon gallery so the click that opened it can't land on a tile
  ImGui::SetNextWindowPos(ImVec2(frame.Min.x, frame.Max.y + S(2)), ImGuiCond_Appearing);
  GalleryPopup("##gallery", it.gallery);
  ItemExtras(it, r, "##gallery");
  ImGui::PopID();
}

// ---- Layout columns -----------------------------------------------------------------
enum class ColKind { Large, Stack, Rows, Gallery };

struct Col {
  ColKind kind = ColKind::Large;
  std::vector<const Item*> items;
  const std::vector<std::vector<Item>>* rows = nullptr;
  bool labels = true;
  bool compact = false;
  int gallery_cols = 0;  // Gallery: tile columns
  float width = 0.0f;
};

float StackItemWidth(const Item& it, bool labels) {
  if (it.kind == ItemKind::Custom) return S(it.combo_width);
  if (it.kind == ItemKind::Check) {
    return S(4 + 14 + 6) + ImGui::CalcTextSize(OneLine(LabelOf(it.cmd)).c_str()).x + S(6);
  }
  float w = S(4 + 16 + 4);
  if (labels) w += ImGui::CalcTextSize(OneLine(LabelOf(it.cmd)).c_str()).x + S(4);
  if (it.kind == ItemKind::Split || it.kind == ItemKind::Menu) w += S(kArrowW);
  return w;
}

float RowItemWidth(const Item& it, bool compact) {
  switch (it.kind) {
    case ItemKind::FontCombo:
    case ItemKind::FormatCombo:
      return S(compact ? it.combo_width * 0.62f : it.combo_width);
    case ItemKind::SizeCombo:
      return S(it.combo_width);
    case ItemKind::Split:
    case ItemKind::ColorSplit:
    case ItemKind::Menu: {
      float w = S(24 + kArrowW);
      if (it.label_in_rows && !compact) w += ImGui::CalcTextSize(OneLine(LabelOf(it.cmd)).c_str()).x + S(4);
      return w;
    }
    case ItemKind::Check:
      return S(24) + ImGui::CalcTextSize(OneLine(LabelOf(it.cmd)).c_str()).x;
    default: {
      float w = S(24);
      if (it.label_in_rows && !compact) w += ImGui::CalcTextSize(OneLine(LabelOf(it.cmd)).c_str()).x + S(6);
      return w;
    }
  }
}

float RowsWidth(const std::vector<std::vector<Item>>& rows, bool compact) {
  float w = 0.0f;
  for (const auto& row : rows) {
    float rw = 0.0f;
    for (size_t i = 0; i < row.size(); ++i) rw += RowItemWidth(row[i], compact) + (i ? S(2) : 0.0f);
    w = std::max(w, rw);
  }
  return w;
}

std::vector<Col> BuildColumns(const Group& g, int stage) {
  std::vector<Col> cols;
  std::vector<const Item*> pending;
  auto flush = [&]() {
    for (size_t i = 0; i < pending.size(); i += 3) {
      Col c;
      c.kind = ColKind::Stack;
      c.labels = stage <= 1;
      for (size_t k = i; k < std::min(pending.size(), i + 3); ++k) c.items.push_back(pending[k]);
      cols.push_back(std::move(c));
    }
    pending.clear();
  };
  for (const Block& b : g.blocks) {
    switch (b.kind) {
      case BlockKind::Large:
        for (const Item& it : b.items) {
          if (stage == 0 || b.keep_large) {
            flush();
            Col c;
            c.kind = ColKind::Large;
            c.items.push_back(&it);
            cols.push_back(std::move(c));
          } else {
            pending.push_back(&it);
            if (pending.size() == 3) flush();
          }
        }
        break;
      case BlockKind::Stack:
        flush();
        for (size_t i = 0; i < b.items.size(); i += 3) {
          Col c;
          c.kind = ColKind::Stack;
          c.labels = stage <= 1;
          for (size_t k = i; k < std::min(b.items.size(), i + 3); ++k) c.items.push_back(&b.items[k]);
          cols.push_back(std::move(c));
        }
        break;
      case BlockKind::Rows: {
        flush();
        Col c;
        c.kind = ColKind::Rows;
        c.rows = &b.rows;
        c.compact = stage >= 2;
        cols.push_back(std::move(c));
        break;
      }
      case BlockKind::Gallery:
        flush();
        for (const Item& it : b.items) {
          Col c;
          c.items.push_back(&it);
          if (stage <= 1) {
            c.kind = ColKind::Gallery;
            c.gallery_cols = stage == 0 ? 4 : 2;
          } else {
            c.kind = ColKind::Large;  // shrunk: a large "Cell Styles" button that opens the gallery
          }
          cols.push_back(std::move(c));
        }
        break;
    }
  }
  flush();
  for (Col& c : cols) {
    switch (c.kind) {
      case ColKind::Large:
        c.width = std::max(S(42), LabelWidth(LabelOf(c.items[0]->cmd)) + S(10));
        if (c.items[0]->kind == ItemKind::Split || c.items[0]->kind == ItemKind::Menu || c.items[0]->kind == ItemKind::Gallery) {
          // single-line text: the arrow goes on the second line; two-line text: the arrow follows the second line
          const std::string l = LabelOf(c.items[0]->cmd);
          if (l.find('\n') != std::string::npos) c.width += S(kArrowW);
        }
        break;
      case ColKind::Stack:
        c.width = 0.0f;
        for (const Item* it : c.items) c.width = std::max(c.width, StackItemWidth(*it, c.labels));
        break;
      case ColKind::Rows:
        c.width = RowsWidth(*c.rows, c.compact);
        break;
      case ColKind::Gallery:
        c.width = GalleryWidth(c.gallery_cols);
        break;
    }
  }
  return cols;
}

float CollapsedWidth(const Group& g) {
  return std::max(S(44), LabelWidth(g.label) + S(16));
}

float GroupWidth(const Group& g, int stage) {
  float label_w = 0.0f;
  {
    ImGui::PushFont(nullptr, F().ui_small);
    label_w = ImGui::CalcTextSize(g.label.c_str()).x + (g.launcher.empty() ? S(12) : S(30));
    ImGui::PopFont();
  }
  if (stage >= static_cast<int>(Stage::Collapsed)) return CollapsedWidth(g) + S(1);
  float w = S(kGroupPadX) * 2;
  const auto cols = BuildColumns(g, stage);
  for (size_t i = 0; i < cols.size(); ++i) w += cols[i].width + (i ? S(kColGap) : 0.0f);
  return std::max(w, label_w) + S(1);  // + separator
}

// ---- Layout cache -----------------------------------------------------------------
struct WidthCache {
  float dpi = -1.0f;
  float font_px = -1.0f;
  std::vector<std::vector<std::array<float, 4>>> per_tab;  // [tab][group][stage]
};
WidthCache g_wcache;

struct SolveCache {
  int tab = -1;
  float width = -1.0f;
  float dpi = -1.0f;
  std::vector<int> stages;
};
SolveCache g_scache;

const std::vector<std::array<float, 4>>& GroupWidths(int tab) {
  const float dpi = Dpi();
  const float fpx = FontPx(F().ui_size);
  if (g_wcache.dpi != dpi || g_wcache.font_px != fpx || g_wcache.per_tab.size() != g_tabs.size()) {
    g_wcache.dpi = dpi;
    g_wcache.font_px = fpx;
    g_wcache.per_tab.assign(g_tabs.size(), {});
  }
  auto& v = g_wcache.per_tab[static_cast<size_t>(tab)];
  if (v.size() != g_tabs[static_cast<size_t>(tab)].groups.size()) {
    v.clear();
    for (const Group& g : g_tabs[static_cast<size_t>(tab)].groups) {
      std::array<float, 4> a{};
      for (int s = 0; s < 4; ++s) a[static_cast<size_t>(s)] = GroupWidth(g, s);
      v.push_back(a);
    }
  }
  return v;
}

// ---- Drawing: large buttons --------------------------------------------------------------
void DrawLarge(const Item& it, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool enabled = IsEnabled(it.cmd);
  const std::string label = LabelOf(it.cmd);
  const bool split = it.kind == ItemKind::Split;
  const bool gallery = it.kind == ItemKind::Gallery;
  const bool menu = it.kind == ItemKind::Menu || gallery;
  ImGui::PushID(it.cmd.c_str());
  Hit top, bottom;
  if (split) {
    const ImRect rt(r.Min, ImVec2(r.Max.x, r.Min.y + S(kLargeTopH)));
    const ImRect rb(ImVec2(r.Min.x, rt.Max.y), r.Max);
    top = HitRegion("##top", rt.Min, rt.GetSize(), enabled);
    ScreenTip(it.cmd);
    bottom = HitRegion("##bottom", rb.Min, rb.GetSize(), enabled);
    ScreenTip(it.cmd);
    const bool any_hover = top.hovered || bottom.hovered;
    if (any_hover) {
      dl->AddRect(r.Min, r.Max, T().btn_checked_border, S(3));
      FillState(dl, rt, top, false);
      FillState(dl, rb, bottom, false);
    }
    if (top.clicked) Exec(it.cmd);
    if (bottom.clicked) ImGui::OpenPopup("##menu");
  } else {
    top = HitRegion("##btn", r.Min, r.GetSize(), enabled);
    ScreenTip(it.cmd);
    FillState(dl, r, top, IsPressed(it));
    if (top.clicked) {
      if (menu) ImGui::OpenPopup("##menu");
      else Exec(it.cmd);
    }
    if (gallery && g_open_gallery == it.gallery) {
      ImGui::OpenPopup("##menu");
      g_open_gallery.clear();
    }
  }
  DrawItemIcon(dl, it, true, ImVec2(r.GetCenter().x, r.Min.y + S(4) + S(16)), F().icon_large, TextCol(enabled));
  // text: up to two lines, centered
  const float lh = ImGui::GetFontSize();
  float y = r.Min.y + S(kLargeTopH) + S(1);
  size_t start = 0;
  int line_no = 0;
  bool caret_drawn = false;
  while (true) {
    const size_t nl = label.find('\n', start);
    const std::string line = label.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    const float w = ImGui::CalcTextSize(line.c_str()).x;
    const bool last = nl == std::string::npos;
    const float extra = ((split || menu) && last && line_no > 0) ? S(kArrowW) : 0.0f;
    const float x = r.GetCenter().x - (w + extra) * 0.5f;
    dl->AddText(ImVec2(IM_ROUND(x), IM_ROUND(y)), TextCol(enabled), line.c_str());
    if (extra > 0.0f) {
      DrawCaret(dl, ImVec2(x + w + S(7), y + lh * 0.5f), TextCol(enabled));
      caret_drawn = true;
    }
    y += lh;
    ++line_no;
    if (last) break;
    start = nl + 1;
  }
  if ((split || menu) && !caret_drawn) DrawCaret(dl, ImVec2(r.GetCenter().x, y + lh * 0.45f), TextCol(enabled));
  if (gallery) {
    ImGui::SetNextWindowPos(ImVec2(r.Min.x, r.Max.y + S(2)), ImGuiCond_Appearing);
    GalleryPopup("##menu", it.gallery);
  } else {
    PopupBelow(r);
    MenuPopup("##menu", it.menu);
  }
  ItemExtras(it, r, (split || menu) ? "##menu" : nullptr);
  ImGui::PopID();
}

// ---- Drawing: medium / small buttons in a stack ------------------------------------------------------
void DrawCheck(const Item& it, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(it.cmd.c_str());
  const Hit h = HitRegion("##chk", r.Min, r.GetSize(), IsEnabled(it.cmd));
  ScreenTip(it.cmd);
  FillState(dl, r, h, false);
  const float box = S(13);
  const ImVec2 b0(r.Min.x + S(4), r.GetCenter().y - box * 0.5f);
  const bool on = it.check && *it.check;
  dl->AddRectFilled(b0, ImVec2(b0.x + box, b0.y + box), on ? T().accent : IM_COL32_WHITE, S(2));
  dl->AddRect(b0, ImVec2(b0.x + box, b0.y + box), on ? T().accent : IM_COL32(110, 110, 110, 255), S(2));
  if (on) {
    const ImVec2 a(b0.x + box * 0.22f, b0.y + box * 0.52f), m(b0.x + box * 0.43f, b0.y + box * 0.72f),
        c(b0.x + box * 0.80f, b0.y + box * 0.28f);
    dl->AddLine(a, m, IM_COL32_WHITE, S(1.6f));
    dl->AddLine(m, c, IM_COL32_WHITE, S(1.6f));
  }
  dl->AddText(ImVec2(b0.x + box + S(6), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), T().btn_text,
              OneLine(LabelOf(it.cmd)).c_str());
  if (h.clicked) {
    if (it.check) *it.check = !*it.check;
    Exec(it.cmd);
  }
  ItemExtras(it, r, nullptr, it.check);
  ImGui::PopID();
}

void DrawStackItem(const Item& it, ImRect r, bool labels) {
  if (it.kind == ItemKind::Custom) {
    if (it.draw) it.draw(r);
    return;
  }
  if (it.kind == ItemKind::Check) {
    DrawCheck(it, r);
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool enabled = IsEnabled(it.cmd);
  const bool split = it.kind == ItemKind::Split;
  const bool menu = it.kind == ItemKind::Menu;
  ImGui::PushID(it.cmd.c_str());
  if (split) {
    const ImRect rm(r.Min, ImVec2(r.Max.x - S(kArrowW), r.Max.y));
    const ImRect ra(ImVec2(rm.Max.x, r.Min.y), r.Max);
    const Hit hm = HitRegion("##main", rm.Min, rm.GetSize(), enabled);
    ScreenTip(it.cmd);
    const Hit ha = HitRegion("##arrow", ra.Min, ra.GetSize(), enabled);
    ScreenTip(it.cmd);
    if (hm.hovered || ha.hovered) dl->AddRect(r.Min, r.Max, T().btn_checked_border, S(3));
    FillState(dl, rm, hm, IsPressed(SplitFace(it)));
    FillState(dl, ra, ha, false);
    if (hm.clicked) Exec(SplitFace(it).cmd);
    if (ha.clicked) ImGui::OpenPopup("##menu");
    DrawCaret(dl, ra.GetCenter(), TextCol(enabled));
  } else {
    const Hit h = HitRegion("##btn", r.Min, r.GetSize(), enabled);
    ScreenTip(it.cmd);
    FillState(dl, r, h, IsPressed(it));
    if (h.clicked) {
      if (menu) ImGui::OpenPopup("##menu");
      else Exec(it.cmd);
    }
    if (menu) DrawCaret(dl, ImVec2(r.Max.x - S(kArrowW) * 0.5f, r.GetCenter().y), TextCol(enabled));
  }
  DrawItemIcon(dl, SplitFace(it), false, ImVec2(r.Min.x + S(4 + 8), r.GetCenter().y), F().icon_small, TextCol(enabled));
  if (labels) {
    dl->AddText(ImVec2(r.Min.x + S(4 + 16 + 4), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), TextCol(enabled),
                OneLine(LabelOf(it.cmd)).c_str());
  }
  g_menu_ran.clear();
  PopupBelow(r);
  MenuPopup("##menu", it.menu);
  NoteSplitChoice(it);
  ItemExtras(it, r, (split || menu) ? "##menu" : nullptr);
  ImGui::PopID();
}

// ---- Drawing: controls in a two-row layout -----------------------------------------------------
void ComboFrame(ImDrawList* dl, ImRect r, bool hovered) {
  dl->AddRectFilled(r.Min, r.Max, IM_COL32_WHITE, S(3));
  dl->AddRect(r.Min, r.Max, hovered ? IM_COL32(150, 150, 150, 255) : IM_COL32(200, 200, 200, 255), S(3));
}

bool ContainsNoCase(const std::string& hay, const char* needle) {
  if (!needle || !*needle) return true;
  std::string h = hay, n = needle;
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return h.find(n) != std::string::npos;
}

std::vector<std::string> g_recent_fonts;  // the Font box's Recently Used Fonts (newest first)

void PickFont(const Item& it, const std::string& fam) {
  g_state.font_name = fam;
  g_recent_fonts.erase(std::remove(g_recent_fonts.begin(), g_recent_fonts.end(), fam), g_recent_fonts.end());
  g_recent_fonts.insert(g_recent_fonts.begin(), fam);
  if (g_recent_fonts.size() > 10) g_recent_fonts.resize(10);
  Exec(it.cmd);
}

// one font name in the Font box, drawn in its own typeface (Excel), so fonts are picked by how they look
bool FontEntry(const std::string& fam, const std::string& shown, bool selected) {
  ImFont* f = CellFont(fam, false, false);
  ImGui::PushFont(f ? f : ImGui::GetFont(), ImGui::GetFontSize() * 1.15f);
  const bool hit = ImGui::Selectable((shown + "##" + fam).c_str(), selected);
  ImGui::PopFont();
  return hit;
}

void DrawFontCombo(const Item& it, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(it.cmd.c_str());
  const Hit h = HitRegion("##combo", r.Min, r.GetSize());
  ScreenTip(it.cmd);
  ComboFrame(dl, r, h.hovered);
  dl->PushClipRect(r.Min, ImVec2(r.Max.x - S(kArrowW + 2), r.Max.y), true);
  dl->AddText(ImVec2(r.Min.x + S(5), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), T().btn_text,
              g_state.font_name.c_str());
  dl->PopClipRect();
  DrawCaret(dl, ImVec2(r.Max.x - S(kArrowW) * 0.5f - S(1), r.GetCenter().y), T().btn_text);
  if (h.clicked) ImGui::OpenPopup("##fonts");
  ImGui::SetNextWindowSize(ImVec2(S(280), S(380)));
  PopupBelow(r);
  if (ImGui::BeginPopup("##fonts")) {
    NoteMenuShown();
    static char filter[128] = "";
    if (ImGui::IsWindowAppearing()) {
      filter[0] = 0;
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    // Enter applies what was typed, as Excel's font box does: the font of that name, else the first one listed
    const bool enter = ImGui::InputTextWithHint("##filter", TR("Search fonts"), filter, sizeof(filter), ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter && filter[0]) {
      std::string pick;
      for (const std::string& fam : SystemFontFamilies()) {
        if (_stricmp(fam.c_str(), filter) == 0) {
          pick = fam;
          break;
        }
        if (pick.empty() && ContainsNoCase(fam, filter)) pick = fam;
      }
      PickFont(it, pick.empty() ? std::string(filter) : pick);
      ImGui::CloseCurrentPopup();
    }
    ImGui::BeginChild("##list");
    // Excel's sections: Theme Fonts (headings, body), Recently Used Fonts, All Fonts
    auto heading = [](const char* text) {
      ImGui::PushFont(F().ui_bold, F().ui_size);
      ImGui::TextDisabled("%s", text);
      ImGui::PopFont();
    };
    if (!filter[0]) {
      const model::Theme& th = model::CurrentTheme();
      heading(TR("Theme Fonts"));
      if (FontEntry(th.major_font, th.major_font + TR(" (Headings)"), false)) PickFont(it, th.major_font), ImGui::CloseCurrentPopup();
      if (FontEntry(th.minor_font, th.minor_font + TR(" (Body)"), false)) PickFont(it, th.minor_font), ImGui::CloseCurrentPopup();
      if (!g_recent_fonts.empty()) {
        heading(TR("Recently Used Fonts"));
        const std::vector<std::string> recent = g_recent_fonts;
        for (const std::string& fam : recent) {
          if (FontEntry(fam, fam, false)) PickFont(it, fam), ImGui::CloseCurrentPopup();
        }
      }
      heading(TR("All Fonts"));
    }
    std::vector<const std::string*> shown;
    for (const std::string& fam : SystemFontFamilies()) {
      if (ContainsNoCase(fam, filter)) shown.push_back(&fam);
    }
    // only the names on the screen are drawn (each loads its font)
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(shown.size()), ImGui::GetTextLineHeightWithSpacing() * 1.15f);
    while (clip.Step()) {
      for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
        const std::string fam = *shown[static_cast<size_t>(k)];
        if (FontEntry(fam, fam, fam == g_state.font_name)) {
          PickFont(it, fam);
          ImGui::CloseCurrentPopup();
        }
      }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
  }
  ItemExtras(it, r, "##fonts");
  ImGui::PopID();
}

void DrawSizeCombo(const Item& it, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(it.cmd.c_str());
  ComboFrame(dl, r, false);
  static char buf[16];
  // don't overwrite the input box while it is being edited
  if (ImGui::GetActiveID() != ImGui::GetID("##size")) std::snprintf(buf, sizeof(buf), "%s", g_state.font_size.c_str());
  ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(1), r.Min.y + S(1)));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), (r.GetHeight() - S(2) - ImGui::GetFontSize()) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
  ImGui::SetNextItemWidth(r.GetWidth() - S(kArrowW + 3));
  // a click selects the size, so typing replaces it (Excel)
  if (ImGui::InputText("##size", buf, sizeof(buf), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
    g_state.font_size = buf;
    Exec(it.cmd);
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  const ImRect ra(ImVec2(r.Max.x - S(kArrowW + 1), r.Min.y), r.Max);
  const Hit ha = HitRegion("##arrow", ra.Min, ra.GetSize());
  ScreenTip(it.cmd);
  FillState(dl, ImRect(ra.Min + ImVec2(0, S(1)), ra.Max - ImVec2(S(1), S(1))), ha, false);
  DrawCaret(dl, ra.GetCenter(), T().btn_text);
  if (ha.clicked) ImGui::OpenPopup("##sizes");
  PopupBelow(r);
  if (ImGui::BeginPopup("##sizes")) {
    NoteMenuShown();
    static const char* kSizes[] = {"8", "9", "10", "11", "12", "14", "16", "18", "20", "22", "24", "26", "28", "36", "48", "72"};
    for (const char* s : kSizes) {
      if (ImGui::Selectable(s, g_state.font_size == s, 0, ImVec2(S(60), 0))) {
        g_state.font_size = s;
        Exec(it.cmd);
      }
    }
    ImGui::EndPopup();
  }
  ItemExtras(it, r, "##sizes");
  ImGui::PopID();
}

void DrawFormatCombo(const Item& it, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushID(it.cmd.c_str());
  const Hit h = HitRegion("##combo", r.Min, r.GetSize());
  ScreenTip(it.cmd);
  ComboFrame(dl, r, h.hovered);
  dl->PushClipRect(r.Min, ImVec2(r.Max.x - S(kArrowW + 2), r.Max.y), true);
  dl->AddText(ImVec2(r.Min.x + S(5), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), T().btn_text,
              g_state.number_format.c_str());
  dl->PopClipRect();
  DrawCaret(dl, ImVec2(r.Max.x - S(kArrowW) * 0.5f - S(1), r.GetCenter().y), T().btn_text);
  if (h.clicked) ImGui::OpenPopup("##formats");
  PopupBelow(r);
  if (ImGui::BeginPopup("##formats")) {
    NoteMenuShown();
    static const char* kFormats[] = {TR("General"), TR("Number"), TR("Currency"), TR("Accounting"), TR("Short Date"), TR("Long Date"), TR("Time"),
                                     TR("Percentage"), TR("Fraction"), TR("Scientific"), TR("Text"), TR("Timestamp (ns)")};
    // each format with the active cell's value in it underneath, as Excel's list shows them
    for (size_t i = 0; i < std::size(kFormats); ++i) {
      const char* f = kFormats[i];
      const std::string sample = i < g_state.number_format_samples.size() ? g_state.number_format_samples[i] : std::string();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable(f, g_state.number_format == f, 0, ImVec2(S(220), sample.empty() ? S(24) : S(38)))) {
        g_state.number_format = f;
        Exec(it.cmd);
      }
      if (!sample.empty()) {
        ImDrawList* pl = ImGui::GetWindowDrawList();
        pl->PushClipRect(at, at + ImVec2(S(220), S(38)), true);
        pl->AddText(at + ImVec2(S(8), ImGui::GetTextLineHeight() + S(2)), ImGui::GetColorU32(ImGuiCol_TextDisabled), sample.c_str());
        pl->PopClipRect();
      }
    }
    ImGui::Separator();
    if (ImGui::Selectable(TR("More Number Formats..."), false, 0, ImVec2(S(200), S(24)))) Exec("FormatCellsNumberDialog");
    ImGui::EndPopup();
  }
  ItemExtras(it, r, "##formats");
  ImGui::PopID();
}

void DrawRowItem(const Item& it, ImRect r, bool compact) {
  switch (it.kind) {
    case ItemKind::FontCombo:
      DrawFontCombo(it, r);
      return;
    case ItemKind::SizeCombo:
      DrawSizeCombo(it, r);
      return;
    case ItemKind::FormatCombo:
      DrawFormatCombo(it, r);
      return;
    case ItemKind::Check:
      DrawCheck(it, r);
      return;
    default:
      break;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool enabled = IsEnabled(it.cmd);
  const bool has_arrow = it.kind == ItemKind::Split || it.kind == ItemKind::ColorSplit || it.kind == ItemKind::Menu;
  const bool show_label = it.label_in_rows && !compact;
  ImGui::PushID(it.cmd.c_str());
  ImRect rm = r;
  if (has_arrow && it.kind != ItemKind::Menu) rm.Max.x -= S(kArrowW);
  const Hit hm = HitRegion("##main", rm.Min, rm.GetSize(), enabled);
  ScreenTip(it.cmd);
  Hit ha;
  ImRect ra;
  if (has_arrow && it.kind != ItemKind::Menu) {
    ra = ImRect(ImVec2(rm.Max.x, r.Min.y), r.Max);
    ha = HitRegion("##arrow", ra.Min, ra.GetSize(), enabled);
    ScreenTip(it.cmd);
    if (hm.hovered || ha.hovered) dl->AddRect(r.Min, r.Max, T().btn_checked_border, S(3));
    FillState(dl, ra, ha, false);
    DrawCaret(dl, ra.GetCenter(), TextCol(enabled));
  }
  const Item face = SplitFace(it);  // a split that remembers shows and runs the entry chosen last
  FillState(dl, rm, hm, IsPressed(face));
  const ImVec2 ic(r.Min.x + S(12), r.GetCenter().y - (it.kind == ItemKind::ColorSplit ? S(2) : 0.0f));
  DrawItemIcon(dl, face, false, ic, F().icon_small, TextCol(enabled));
  if (it.kind == ItemKind::ColorSplit && it.color) {
    if ((*it.color >> IM_COL32_A_SHIFT) == 0) dl->AddRect(ImVec2(ic.x - S(7), ic.y + S(6)), ImVec2(ic.x + S(7), ic.y + S(9)), IM_COL32(150, 150, 150, 255));
    else dl->AddRectFilled(ImVec2(ic.x - S(7), ic.y + S(6)), ImVec2(ic.x + S(7), ic.y + S(9)), *it.color);
  }
  if (show_label) {
    dl->AddText(ImVec2(r.Min.x + S(24), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), TextCol(enabled),
                OneLine(LabelOf(it.cmd)).c_str());
  }
  if (it.kind == ItemKind::Menu) DrawCaret(dl, ImVec2(r.Max.x - S(kArrowW) * 0.5f, r.GetCenter().y), TextCol(enabled));
  if (hm.clicked) {
    if (it.kind == ItemKind::Menu) ImGui::OpenPopup("##menu");
    else Exec(face.cmd);
  }
  if (ha.clicked) ImGui::OpenPopup(it.kind == ItemKind::ColorSplit ? "##palette" : "##menu");
  if (it.kind == ItemKind::ColorSplit && it.color) {
    PopupBelow(r);
    ColorPalettePopup("##palette", it.color, it.theme_ref, it.cmd);
  }
  g_menu_ran.clear();
  PopupBelow(r);
  MenuPopup("##menu", it.menu);
  NoteSplitChoice(it);
  ItemExtras(it, r, it.kind == ItemKind::Menu || it.kind == ItemKind::Split ? "##menu" : nullptr);
  ImGui::PopID();
}

// ---- Drawing: groups ---------------------------------------------------------------
void DrawGroupContent(const Group& g, int stage, ImVec2 origin, float content_h);

void DrawGroupLabel(const Group& g, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::PushFont(nullptr, F().ui_small);
  const float tw = ImGui::CalcTextSize(g.label.c_str()).x;
  const float launcher_w = g.launcher.empty() ? 0.0f : S(16);
  const float y = r.Max.y - S(kLabelH) + (S(kLabelH) - ImGui::GetFontSize()) * 0.5f - S(1);
  dl->AddText(ImVec2(IM_ROUND(r.GetCenter().x - (tw + launcher_w) * 0.5f), IM_ROUND(y)), T().group_label, g.label.c_str());
  ImGui::PopFont();
  if (!g.launcher.empty()) {
    const ImRect lr(ImVec2(r.Max.x - S(18), r.Max.y - S(kLabelH) + S(2)), ImVec2(r.Max.x - S(3), r.Max.y - S(3)));
    ImGui::PushID("##launcher");
    const Hit h = HitRegion("##l", lr.Min, lr.GetSize(), IsEnabled(g.launcher));
    ScreenTip(g.launcher);
    FillState(dl, lr, h, false);
    DrawIcon(dl, icon::DialogLauncher, lr.GetCenter(), 12.0f, T().group_label);
    if (h.clicked) Exec(g.launcher);
    Item li;
    li.cmd = g.launcher;
    ItemExtras(li, lr, nullptr);
    ImGui::PopID();
  }
}

void DrawGroup(const Group& g, int stage, ImRect r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (stage >= static_cast<int>(Stage::Collapsed)) {
    // collapsed into a single button: clicking pops up the whole group
    const ImRect br(ImVec2(r.Min.x + S(2), r.Min.y + S(kContentTop)), ImVec2(r.Max.x - S(3), r.Max.y - S(3)));
    const Hit h = HitRegion("##collapsed", br.Min, br.GetSize());
    FillState(dl, br, h, ImGui::IsPopupOpen("##grp"));
    DrawIcon(dl, g.icon, ImVec2(br.GetCenter().x, br.Min.y + S(4) + S(16)), F().icon_large, T().btn_text);
    const float tw = LabelWidth(g.label);
    const float ly = br.Min.y + S(kLargeTopH) + S(1);
    dl->AddText(ImVec2(IM_ROUND(br.GetCenter().x - tw * 0.5f), IM_ROUND(ly)), T().btn_text, FirstLine(g.label).c_str());
    DrawCaret(dl, ImVec2(br.GetCenter().x, ly + ImGui::GetFontSize() * 1.45f), T().btn_text);
    if (h.clicked) ImGui::OpenPopup("##grp");
    const float w = GroupWidth(g, 0);
    const float content_h = S(kBodyH);
    ImGui::SetNextWindowPos(ImVec2(br.Min.x, r.Max.y));
    ImGui::SetNextWindowSize(ImVec2(w + S(2), content_h + S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(1), S(1)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, T().ribbon_bg);
    if (ImGui::BeginPopup("##grp")) {
      const ImVec2 o = ImGui::GetCursorScreenPos();
      DrawGroupContent(g, 0, o, content_h);
      DrawGroupLabel(g, ImRect(o, ImVec2(o.x + w, o.y + content_h)));
      if (g_executed) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  } else {
    DrawGroupContent(g, stage, r.Min, r.GetHeight());
    DrawGroupLabel(g, ImRect(r.Min, ImVec2(r.Max.x - S(1), r.Max.y)));
  }
  // group separator
  dl->AddRectFilled(ImVec2(r.Max.x - S(1), r.Min.y + S(6)), ImVec2(r.Max.x, r.Max.y - S(6)), T().group_separator);
}

void DrawGroupContent(const Group& g, int stage, ImVec2 origin, float content_h) {
  const auto cols = BuildColumns(g, stage);
  float x = origin.x + S(kGroupPadX);
  const float top = origin.y + S(kContentTop);
  const float area_h = content_h - S(kContentTop) - S(kLabelH);
  for (size_t ci = 0; ci < cols.size(); ++ci) {
    const Col& c = cols[ci];
    ImGui::PushID(static_cast<int>(ci));
    switch (c.kind) {
      case ColKind::Large:
        DrawLarge(*c.items[0], ImRect(ImVec2(x, top), ImVec2(x + c.width, top + area_h)));
        break;
      case ColKind::Gallery:
        DrawGallery(*c.items[0], ImRect(ImVec2(x, top), ImVec2(x + c.width, top + area_h)), c.gallery_cols);
        break;
      case ColKind::Stack:
        for (size_t i = 0; i < c.items.size(); ++i) {
          const float y = top + S(kRowH) * static_cast<float>(i);
          ImGui::PushID(static_cast<int>(i));
          DrawStackItem(*c.items[i], ImRect(ImVec2(x, y), ImVec2(x + c.width, y + S(kRowH))), c.labels);
          ImGui::PopID();
        }
        break;
      case ColKind::Rows: {
        const float row_h = S(24);
        const float ys[2] = {top + S(4), top + S(4) + row_h + S(8)};
        for (size_t ri = 0; ri < c.rows->size() && ri < 2; ++ri) {
          float rx = x;
          const auto& row = (*c.rows)[ri];
          ImGui::PushID(static_cast<int>(ri) + 100);
          for (size_t i = 0; i < row.size(); ++i) {
            const float w = RowItemWidth(row[i], c.compact);
            ImGui::PushID(static_cast<int>(i));
            const float h = (row[i].kind == ItemKind::FontCombo || row[i].kind == ItemKind::SizeCombo ||
                             row[i].kind == ItemKind::FormatCombo)
                                ? S(22)
                                : row_h;
            const float y = ys[ri] + (row_h - h) * 0.5f;
            DrawRowItem(row[i], ImRect(ImVec2(rx, y), ImVec2(rx + w, y + h)), c.compact);
            ImGui::PopID();
            rx += w + S(2);
          }
          ImGui::PopID();
        }
        break;
      }
    }
    ImGui::PopID();
    x += c.width + S(kColGap);
  }
}

}  // namespace

// ============================================================================
State& St() { return g_state; }
void AddKeyTip(const std::string& key, ImVec2 at, std::function<void()> action) {
  if (g_state.keytip_level != 1) return;
  KeyTip k;
  k.key = key;
  k.action = std::move(action);
  g_kt.push_back(std::move(k));
  DrawKeyTipBadge(key, at);
}
std::vector<Tab>& Tabs() { return g_tabs; }
std::vector<ItemRect> VisibleItems() {
  std::vector<ItemRect> out;
  const int now = ImGui::GetFrameCount();
  for (const auto& [key, s] : g_seen) {
    if (now - s.frame <= 2) out.push_back(ItemRect{s.cmd, s.rect, s.where, OneLine(LabelOf(s.cmd))});
  }
  return out;
}

std::string TabOf(const std::string& cmd) {
  auto has = [&](const Item& it) { return it.cmd == cmd || std::find(it.menu.begin(), it.menu.end(), cmd) != it.menu.end(); };
  for (const Tab& t : g_tabs) {
    if (t.backstage || t.visible) continue;  // File, and the contextual tabs
    for (const Group& g : t.groups) {
      for (const Block& b : g.blocks) {
        for (const Item& it : b.items)
          if (has(it)) return t.label;
        for (const auto& row : b.rows)
          for (const Item& it : row)
            if (has(it)) return t.label;
      }
    }
  }
  return {};
}

int ButtonIcon(const std::string& cmd) {
  bool button = false, bare = false;  // one bare button is enough to count
  auto visit = [&](const Item& it) {
    if (it.cmd != cmd) return;
    switch (it.kind) {
      case ItemKind::Button:
      case ItemKind::Split:
      case ItemKind::Toggle:
      case ItemKind::Menu:
      case ItemKind::ColorSplit:
        button = true;
        if (!IconOf(it, false) && !it.icon_large && !it.image) bare = true;
        break;
      default:
        break;
    }
  };
  for (const Tab& t : g_tabs) {
    for (const Group& g : t.groups) {
      for (const Block& b : g.blocks) {
        for (const Item& it : b.items) visit(it);
        for (const auto& row : b.rows)
          for (const Item& it : row) visit(it);
      }
    }
  }
  return !button ? -1 : bare ? 0 : 1;
}
void InvalidateLayout() {
  g_wcache = {};
  g_scache = {};
}
void DrawRibbonIcon(ImDrawList* dl, const char* glyph, ImVec2 center, float base, ImU32 col) { DrawIcon(dl, glyph, center, base, col); }

void SetGallery(const std::string& id, std::vector<GalleryTile> tiles) { g_galleries[id] = std::move(tiles); }
void SetGalleryFooter(const std::string& id, std::vector<std::string> cmds) { g_gallery_footers[id] = std::move(cmds); }
void OpenGallery(const std::string& id) {
  g_open_gallery = id;
  // its tab comes to the front
  for (size_t t = 0; t < g_tabs.size(); ++t) {
    for (const auto& g : g_tabs[t].groups) {
      for (const auto& b : g.blocks) {
        for (const auto& it : b.items) {
          if (it.gallery == id && (!g_tabs[t].visible || *g_tabs[t].visible)) g_state.selected_tab = static_cast<int>(t);
        }
      }
    }
  }
}
void SetGalleryLayout(const std::string& id, int cols, float tile_w, float tile_h) { g_gallery_layouts[id] = GalleryLayout{std::max(1, cols), tile_w, tile_h}; }

float TabRowHeight() { return S(kTabRowH); }
float BodyHeight() { return S(kBodyH); }

std::vector<int> SolveStages(int tab_index, float available_width) {
  if (tab_index < 0 || tab_index >= static_cast<int>(g_tabs.size())) return {};
  ImGui::PushFont(F().ui, F().ui_size);
  const auto& widths = GroupWidths(tab_index);
  ImGui::PopFont();
  const float dpi = Dpi();
  if (g_scache.tab == tab_index && g_scache.dpi == dpi && std::abs(g_scache.width - available_width) < 0.5f &&
      g_scache.stages.size() == widths.size()) {
    return g_scache.stages;
  }
  const int n = static_cast<int>(widths.size());
  std::vector<int> stages(static_cast<size_t>(n), 0);
  float total = 0.0f;
  for (const auto& w : widths) total += w[0];
  // shrink order: shrink the rightmost group one step first, then move left; the three stages run in order (Excel's scaling behavior, details to be calibrated)
  for (int st = 1; st <= 3 && total > available_width; ++st) {
    for (int g = n - 1; g >= 0 && total > available_width; --g) {
      const size_t gi = static_cast<size_t>(g);
      if (stages[gi] >= st) continue;
      total += widths[gi][static_cast<size_t>(st)] - widths[gi][static_cast<size_t>(stages[gi])];
      stages[gi] = st;
    }
  }
  g_scache = {tab_index, available_width, dpi, stages};
  return stages;
}

ContextState& Context() {
  static ContextState c;
  return c;
}

void DrawTabRow(const ImRect& r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  g_executed = false;
  // a contextual tab that went away: back to the tab selected before it opened by itself, else Home
  if (g_state.selected_tab >= 0 && g_state.selected_tab < static_cast<int>(g_tabs.size())) {
    const Tab& sel = g_tabs[static_cast<size_t>(g_state.selected_tab)];
    if (sel.visible && !*sel.visible) {
      const int before = g_state.tab_before_contextual;
      const bool usable = before >= 0 && before < static_cast<int>(g_tabs.size()) && !g_tabs[static_cast<size_t>(before)].backstage &&
                          (!g_tabs[static_cast<size_t>(before)].visible || *g_tabs[static_cast<size_t>(before)].visible);
      g_state.selected_tab = usable ? before : 1;
      g_state.tab_before_contextual = -1;
    }
  }
  // an object just inserted brings its contextual tab: selected as it appears, as Excel's (the first one: Chart
  // Design before Format)
  g_state.tab_was_visible.resize(g_tabs.size(), 0);
  for (size_t i = 0; i < g_tabs.size(); ++i) {
    const bool now = g_tabs[i].visible && *g_tabs[i].visible;
    if (now && g_tabs[i].visible && !g_state.tab_was_visible[i] && ImGui::GetFrameCount() <= g_state.open_contextual_until) {
      const int cur = g_state.selected_tab;
      const bool cur_contextual = cur >= 0 && cur < static_cast<int>(g_tabs.size()) && g_tabs[static_cast<size_t>(cur)].visible;
      if (!cur_contextual) g_state.tab_before_contextual = cur;
      g_state.selected_tab = static_cast<int>(i);
      g_state.open_contextual_until = 0;
    }
    g_state.tab_was_visible[i] = now ? 1 : 0;
  }
  g_state.hovered_tile.clear();  // set again by the tile under the mouse (the ribbon body and its popups are drawn after this)
  g_kt_prev = std::move(g_kt);
  g_kt.clear();
  // a modal dialog owns the keyboard: Alt + a letter there is the dialog's (Excel's underlined keys), never the
  // ribbon's KeyTips behind it, which took the letters typed next and ran Home's commands under the dialog
  // (the ribbon draws before the dialog: "visible" reads false here; a dialog that isn't modal says it had the keyboard
  // last frame)
  if (ImGui::GetTopMostPopupModal() || ImGui::GetFrameCount() - g_state.dialog_keys_frame <= 1) {
    g_state.keytip_level = 0;
    g_state.legacy_keys.clear();
    g_kt_typed.clear();
  } else {
    KeyTipInput();
  }
  dl->AddRectFilled(r.Min, r.Max, T().ribbon_bg);
  ImGui::PushFont(F().ui, F().ui_size + 0.5f);
  float x = r.Min.x + S(6);
  ImGui::PushID("##ribbon_tabs");
  for (size_t i = 0; i < g_tabs.size(); ++i) {
    const Tab& tab = g_tabs[i];
    if (tab.visible && !*tab.visible) continue;
    const float tw = ImGui::CalcTextSize(tab.label.c_str()).x;
    const ImRect tr(ImVec2(x, r.Min.y + S(3)), ImVec2(x + tw + S(20), r.Max.y - S(1)));
    ImGui::PushID(static_cast<int>(i));
    const Hit h = HitRegion("##tab", tr.Min, tr.GetSize());
    if (i == 0) g_state.tab_rects.clear();
    g_state.tab_rects.emplace_back(tab.label, tr);
    const bool selected = !tab.backstage && static_cast<int>(i) == g_state.selected_tab && !g_state.backstage;
    if (h.hovered) dl->AddRectFilled(tr.Min, tr.Max, T().tab_hover_bg, S(4));
    dl->AddText(ImVec2(IM_ROUND(tr.GetCenter().x - tw * 0.5f), IM_ROUND(tr.GetCenter().y - ImGui::GetFontSize() * 0.5f)),
                tab.visible ? T().tab_accent : (selected ? T().tab_text_selected : T().tab_text), tab.label.c_str());
    if (selected && !(g_state.collapsed && !g_state.peek)) {
      dl->AddRectFilled(ImVec2(tr.GetCenter().x - tw * 0.5f, tr.Max.y - S(3)),
                        ImVec2(tr.GetCenter().x + tw * 0.5f, tr.Max.y), T().tab_accent, S(1.5f));
    }
    // while the ribbon is shown for a moment it is a popup, and ImGui keeps the items behind a popup from being
    // clicked: the tabs and the arrow read the mouse themselves then
    const bool over_peek = g_state.collapsed && g_state.peek && ImGui::IsMouseHoveringRect(tr.Min, tr.Max) &&
                           ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (h.clicked || over_peek) {
      if (tab.backstage) {
        g_state.backstage = true;
        g_state.backstage_page = 6;  // File opens on Home, as Excel's does
        if (g_state.peek) ImGui::ClosePopupsExceptModals(), g_state.peek = false;
      } else {
        // collapsed: a tab shows the ribbon for a moment; the shown tab clicked again hides it (like Excel)
        const bool again = g_state.collapsed && g_state.peek && g_state.selected_tab == static_cast<int>(i);
        g_state.selected_tab = static_cast<int>(i);
        if (g_state.collapsed) g_state.peek = !again;
        if (again) ImGui::ClosePopupsExceptModals();  // its menus too
        else g_peek_opened = false;  // ImGui closes the popup on this click (outside it): it opens again with the new tab
      }
    }
    // lesson-ok: double-click a tab collapses the ribbon on the second press, as Excel's (not a button)
    if (h.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !tab.backstage) {
      g_state.collapsed = !g_state.collapsed;
      g_state.peek = false;
    }
    if (g_state.keytip_level == 1 && i == 0 && !g_state.legacy_keys.empty()) {
      std::string typed = "ALT";
      for (char ch : g_state.legacy_keys) typed += std::string(", ") + ch;
      ImGui::SetTooltip("%s", (TR("Office 2003 access key: ") + typed + TR(", continue typing the menu key sequence, or press Esc to cancel.")).c_str());
    }
    if (g_state.keytip_level == 1) {
      // File F, Home H, Insert N, Page Layout P, Formulas M, Data A, Review R, View W, Developer L, contextual tabs J + a letter
      static const std::map<std::string, std::string> kTabKeys = {{"File", "F"}, {"Home", "H"}, {"Insert", "N"}, {"Page Layout", "P"},
                                                                  {"Formulas", "M"}, {"Data", "A"}, {"Review", "R"}, {"View", "W"},
                                                                  {"Developer", "L"}};
      std::string key;
      for (const auto& [en, k] : kTabKeys) {
        if (tab.label == TR(en.c_str())) key = k;
      }
      if (key.empty()) {
        key = "J";
        for (char ch : tab.label) {
          if (std::isalpha(static_cast<unsigned char>(ch))) {
            key += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            break;
          }
        }
        if (key.size() < 2) key += static_cast<char>('A' + i % 26);
      }
      KeyTip k;
      k.key = key;
      k.tab = static_cast<int>(i);
      g_kt.push_back(std::move(k));
      DrawKeyTipBadge(key, ImVec2(tr.GetCenter().x, tr.Max.y));
    }
    ImGui::PopID();
    x += tr.GetWidth() + S(2);
  }
  // [YC] About, after the tabs: a menu with the version and the program's path (drawn by the app), opened right here
  // so its position is set after the entry it belongs to
  if (g_state.about_menu) {
    const char* label = TR("About");
    const float tw = ImGui::CalcTextSize(label).x;
    const ImRect ar(ImVec2(x, r.Min.y + S(3)), ImVec2(x + tw + S(20), r.Max.y - S(1)));
    const Hit h = HitRegion("##about", ar.Min, ar.GetSize());
    g_state.tab_rects.emplace_back(label, ar);
    const bool open = ImGui::IsPopupOpen("##about_menu");
    if (h.hovered || open) dl->AddRectFilled(ar.Min, ar.Max, T().tab_hover_bg, S(4));
    dl->AddText(ImVec2(IM_ROUND(ar.GetCenter().x - tw * 0.5f), IM_ROUND(ar.GetCenter().y - ImGui::GetFontSize() * 0.5f)),
                T().tab_text, label);
    if (h.clicked || g_state.about_open_request) {
      g_state.about_open_request = false;
      if (g_state.peek) ImGui::ClosePopupsExceptModals(), g_state.peek = false;
      ImGui::OpenPopup("##about_menu");
    }
    ImGui::SetNextWindowPos(ImVec2(ar.Min.x, ar.Max.y));
    ImGui::PushFont(F().ui, F().ui_size);
    if (ImGui::BeginPopup("##about_menu")) {
      g_state.about_menu();
      ImGui::EndPopup();
    }
    ImGui::PopFont();
  }
  // right side: collapse / expand the ribbon
  {
    const ImRect cr(ImVec2(r.Max.x - S(34), r.Min.y + S(4)), ImVec2(r.Max.x - S(8), r.Max.y - S(3)));
    const Hit h = HitRegion("##collapse", cr.Min, cr.GetSize());
    g_state.arrow_rect = cr;
    FillState(dl, cr, h, false);
    // shown for a moment (collapsed + peek): the arrow puts it away again; pinning is Ctrl+F1 or a double-click on a tab
    const bool down = g_state.collapsed && !g_state.peek;
    DrawIcon(dl, down ? icon::ChevronDown : icon::ChevronUp, cr.GetCenter(), 14.0f, T().btn_text);
    const bool over_peek = g_state.collapsed && g_state.peek && ImGui::IsMouseHoveringRect(cr.Min, cr.Max) &&
                           ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (h.clicked || over_peek) {
      if (g_state.collapsed && g_state.peek) {
        g_state.peek = false;
        ImGui::ClosePopupsExceptModals();  // the ribbon and any of its menus
      } else {
        g_state.collapsed = !g_state.collapsed;
        g_state.peek = false;
      }
    }
    if (h.hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(down ? TR("Pin the Ribbon (Ctrl+F1)") : TR("Collapse the Ribbon (Ctrl+F1)"));
    }
  }
  ImGui::PopID();
  ImGui::PopFont();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F1)) {
    g_state.collapsed = !g_state.collapsed;
    g_state.peek = false;
  }

  // temporary expansion while collapsed: opened once when it starts; when ImGui closes it (a click elsewhere, even with
  // one of its menus open) it stays closed
  if (!(g_state.collapsed && g_state.peek)) g_peek_opened = false;
  if (g_state.collapsed && g_state.peek) {
    if (!g_peek_opened) {
      ImGui::OpenPopup("##ribbon_peek");
      g_peek_opened = true;
    }
    ImGui::SetNextWindowPos(ImVec2(r.Min.x, r.Max.y));
    ImGui::SetNextWindowSize(ImVec2(r.GetWidth(), BodyHeight()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    if (ImGui::BeginPopup("##ribbon_peek", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
      const ImVec2 o = ImGui::GetCursorScreenPos();
      DrawBody(ImRect(o, ImVec2(o.x + r.GetWidth(), o.y + BodyHeight())));
      if (g_executed || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        g_state.peek = false;
      }
      ImGui::EndPopup();
    } else {
      g_state.peek = false;
    }
    ImGui::PopStyleVar(2);
  }
}

void DrawBody(const ImRect& r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(r.Min, r.Max, T().ribbon_bg);
  dl->AddRectFilled(ImVec2(r.Min.x, r.Max.y - 1), r.Max, T().ribbon_border);
  if (g_state.selected_tab < 0 || g_state.selected_tab >= static_cast<int>(g_tabs.size())) return;
  const Tab& tab = g_tabs[static_cast<size_t>(g_state.selected_tab)];
  ImGui::PushFont(F().ui, F().ui_size);
  const float avail = r.GetWidth() - S(8);
  const std::vector<int> stages = SolveStages(g_state.selected_tab, avail);
  g_state.last_stages = stages;
  g_state.last_available_width = avail;
  const auto& widths = GroupWidths(g_state.selected_tab);
  float x = r.Min.x + S(4);
  dl->PushClipRect(r.Min, r.Max, true);
  ImGui::PushID("##ribbon_body");
  ImGui::PushID(g_state.selected_tab);
  for (size_t gi = 0; gi < tab.groups.size(); ++gi) {
    const int stage = stages[gi];
    const float w = widths[gi][static_cast<size_t>(stage)];
    ImGui::PushID(static_cast<int>(gi));
    DrawGroup(tab.groups[gi], stage, ImRect(ImVec2(x, r.Min.y), ImVec2(x + w, r.Max.y)));
    ImGui::PopID();
    x += w;
  }
  ImGui::PopID();
  ImGui::PopID();
  dl->PopClipRect();
  ImGui::PopFont();
}

void DrawBackstage(const ImRect& r) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float nav_w = S(200);
  dl->AddRectFilled(r.Min, r.Max, T().backstage_bg);
  dl->AddRectFilled(r.Min, ImVec2(r.Min.x + nav_w, r.Max.y), T().backstage_nav_bg);
  struct Entry {
    const char* label;
    const char* icon;
    const char* cmd;  // not empty: run the command and close Backstage
    int page;         // >= 0: switch to this page
  };
  // Excel's order: Home, New, Open, then Info, Save, Save As, Print, Export, Close; Options at the bottom
  static const Entry kEntries[] = {
      {TR("Back"), icon::Back, nullptr, -1},       {TR("Home"), icon::ViewNormal, nullptr, 6}, {TR("New"), icon::New, nullptr, 1},
      {TR("Open"), icon::Open, nullptr, 2},        {TR("Info"), icon::Info, nullptr, 0},       {TR("Save"), icon::Save, "FileSave", -1},
      {TR("Save As"), icon::SaveAs, "FileSaveAs", -1}, {TR("Print"), icon::Print, nullptr, 5}, {TR("Export"), icon::Pdf, nullptr, 3},
      {TR("Close"), icon::Close, "FileClose", -1}, {TR("Options"), icon::Options, nullptr, 4},
  };
  static const char* kTitles[] = {TR("Info"), TR("New"), TR("Open"), TR("Export"), TR("Options"), TR("Print"), TR("Home")};
  ImGui::PushFont(F().ui, F().ui_size + 2.0f);
  float y = r.Min.y + S(12);
  ImGui::PushID("##backstage");
  for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kEntries)); ++i) {
    const Entry& e = kEntries[i];
    // Options sits at the bottom of the list, as in Excel
    const bool bottom = e.page == 4;
    const float ey = bottom ? std::max(y + S(12), r.Max.y - S(52)) : y;
    const ImRect er(ImVec2(r.Min.x, ey), ImVec2(r.Min.x + nav_w, ey + S(40)));
    ImGui::PushID(i);
    const Hit h = HitRegion("##e", er.Min, er.GetSize());
    // pages too, by a fixed id (tests open File > Options as a person does: yc.ui.ribbon_items)
    static const char* kPageIds[] = {"BackstageInfo", "BackstageNew", "BackstageOpen", "BackstageExport", "BackstageOptions", "BackstagePrint", "BackstageHome"};
    if (e.cmd) Seen(e.cmd, er, "backstage");
    else if (e.page >= 0 && e.page < 7) Seen(kPageIds[e.page], er, "backstage");
    const bool selected = e.page >= 0 && g_state.backstage_page == e.page;
    if (h.hovered || selected) dl->AddRectFilled(er.Min, er.Max, T().backstage_nav_hover);
    DrawIcon(dl, e.icon, ImVec2(er.Min.x + S(26), er.GetCenter().y), 20.0f, T().backstage_nav_text);
    dl->AddText(ImVec2(er.Min.x + S(48), er.GetCenter().y - ImGui::GetFontSize() * 0.5f), T().backstage_nav_text, e.label);
    if (h.clicked) {
      if (i == 0) {
        g_state.backstage = false;
      } else if (e.cmd) {
        g_state.backstage = false;
        Exec(e.cmd);
      } else {
        g_state.backstage_page = e.page;
      }
    }
    ImGui::PopID();
    y += S(40) + (i == 0 || e.page == 2 ? S(12) : 0.0f);  // a gap after Back and after Open (Excel's divider)
  }
  ImGui::PopID();
  ImGui::PopFont();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_state.backstage = false;

  // content on the right: provided by the app (Info, recent files, Options...)
  const int page = std::clamp(g_state.backstage_page, 0, 6);
  const ImRect content(ImVec2(r.Min.x + nav_w + S(40), r.Min.y + S(28)), ImVec2(r.Max.x - S(40), r.Max.y - S(20)));
  ImGui::SetCursorScreenPos(content.Min);
  ImGui::BeginGroup();
  ImGui::PushFont(F().ui, 28.0f);
  ImGui::TextUnformatted(kTitles[page]);
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(1, S(12)));
  ImGui::PushFont(F().ui, F().ui_size + 1.0f);
  if (g_state.backstage_content) {
    g_state.backstage_content(page, ImRect(ImGui::GetCursorScreenPos(), content.Max));
  } else {
    ImGui::Text("YCGrid %s", YC_VERSION_STRING);
  }
  ImGui::PopFont();
  ImGui::EndGroup();
}

bool DrawItemElsewhere(const std::string& cmd, const ImRect& r) {
  const Item* found = nullptr;
  for (const Tab& t : g_tabs) {
    for (const Group& g : t.groups) {
      for (const Block& b : g.blocks) {
        for (const Item& it : b.items) found = !found && it.cmd == cmd ? &it : found;
        for (const auto& row : b.rows) {
          for (const Item& it : row) found = !found && it.cmd == cmd ? &it : found;
        }
      }
    }
    if (found) break;
  }
  if (!found) return false;
  Item it = *found;
  it.label_in_rows = false;
  g_seen_where = "minitoolbar";
  ImGui::PushID("##elsewhere");
  DrawRowItem(it, r, true);
  ImGui::PopID();
  g_seen_where = nullptr;
  return true;
}

void BuildDefaultTabs(bool* show_gridlines, bool* show_headings, bool* show_formula_bar) {
  using K = ItemKind;
  auto btn = [](const char* cmd, K kind = K::Button) {
    Item it;
    it.kind = kind;
    it.cmd = cmd;
    return it;
  };
  auto split = [](const char* cmd, std::vector<std::string> menu, K kind = K::Split) {
    Item it;
    it.kind = kind;
    it.cmd = cmd;
    it.menu = std::move(menu);
    return it;
  };
  auto large = [](std::vector<Item> items, bool keep = false) {
    Block b;
    b.kind = BlockKind::Large;
    b.items = std::move(items);
    b.keep_large = keep;
    return b;
  };
  auto stack = [](std::vector<Item> items) {
    Block b;
    b.kind = BlockKind::Stack;
    b.items = std::move(items);
    return b;
  };
  auto rows = [](std::vector<std::vector<Item>> r) {
    Block b;
    b.kind = BlockKind::Rows;
    b.rows = std::move(r);
    return b;
  };
  auto labeled = [](Item it) {
    it.label_in_rows = true;
    return it;
  };
  auto remember = [](Item it) {
    it.remember_last = true;
    return it;
  };
  auto gallery = [](const char* cmd) {
    Item it;
    it.kind = K::Gallery;
    it.cmd = cmd;
    it.gallery = cmd;
    Block b;
    b.kind = BlockKind::Gallery;
    b.items.push_back(std::move(it));
    return b;
  };
  auto combo = [](const char* cmd, K kind, float w) {
    Item it;
    it.kind = kind;
    it.cmd = cmd;
    it.combo_width = w;
    return it;
  };
  auto color = [](const char* cmd, ImU32* c, uint32_t* ref = nullptr) {
    Item it;
    it.kind = K::ColorSplit;
    it.cmd = cmd;
    it.color = c;
    it.theme_ref = ref;
    return it;
  };
  auto check = [](const char* cmd, bool* v) {
    Item it;
    it.kind = K::Check;
    it.cmd = cmd;
    it.check = v;
    return it;
  };

  g_tabs.clear();
  {
    Tab t;
    t.label = TR("File");
    t.backstage = true;
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Home");
    t.groups.push_back({TR("Clipboard"), icon::Clipboard,
                        {large({split("Paste", {std::string("=") + TR("Paste"), "Paste", "PasteFormulas", "PasteFormulasNumberFormats", "PasteKeepSource", "PasteNoBorders",
                                                 "PasteKeepWidths", "PasteTranspose", std::string("=") + TR("Paste Values"), "PasteValues",
                                                 "PasteValuesNumberFormats", "PasteValuesSourceFormats", std::string("=") + TR("Other Paste Options"),
                                                 "PasteFormats", "PasteLink", "PasteAsPicture", "PasteLinkedPicture", "-", "PasteSpecial"})}, true),
                         stack({btn("Cut"), split("Copy", {"Copy", "CopyAsPicture"}), btn("FormatPainter")})},
                        "ClipboardPane"});
    t.groups.push_back(
        {TR("Font"), icon::FontGroup,
         {rows({{combo("FontName", K::FontCombo, 128), combo("FontSize", K::SizeCombo, 46), btn("FontSizeIncrease"),
                 btn("FontSizeDecrease")},
                {btn("Bold", K::Toggle), btn("Italic", K::Toggle), remember(split("Underline", {"Underline", "UnderlineDouble"})),
                 remember(split("Borders", {"BorderBottom", "BorderTop", "BorderLeft", "BorderRight", "-", "BorderNone", "BorderAll", "BorderOutside",
                                            "BorderThickOutside", "-", "BorderDoubleBottom", "BorderThickBottom", "BorderTopBottom", "BorderTopThickBottom",
                                            "BorderTopDoubleBottom", "-", "BorderDraw", "BorderDrawGrid", "BorderErase", ">BorderLineColor", "<",
                                            ">BorderLineStyle", "BorderLineStyle1", "BorderLineStyle2", "BorderLineStyle3", "BorderLineStyle4",
                                            "BorderLineStyle5", "BorderLineStyle6", "BorderLineStyle7", "BorderLineStyle8", "BorderLineStyle9",
                                            "BorderLineStyle10", "BorderLineStyle11", "BorderLineStyle12", "BorderLineStyle13", "<", "MoreBorders"})),
                 color("FillColor", &g_state.fill_color, &g_state.fill_theme), color("FontColor", &g_state.font_color, &g_state.font_theme)}})},
         "FormatCellsFontDialog"});
    t.groups.push_back(
        {TR("Alignment"), icon::AlignGroup,
         {rows({{btn("AlignTop", K::Toggle), btn("AlignMiddle", K::Toggle), btn("AlignBottom", K::Toggle), split("TextOrientationMenu", {"AngleCounterclockwise", "AngleClockwise", "VerticalText", "RotateTextUp", "RotateTextDown", "-", "FormatCellsAlignmentDialog"}, K::Menu),
                 labeled(btn("WrapText", K::Toggle))},
                {btn("AlignLeft", K::Toggle), btn("AlignCenter", K::Toggle), btn("AlignRight", K::Toggle),
                 btn("IndentDecrease"), btn("IndentIncrease"),
                 labeled(split("MergeCenter", {"MergeCenter", "MergeAcross", "MergeCells", "UnmergeCells"}))}})},
         "FormatCellsAlignmentDialog"});
    t.groups.push_back({TR("Number"), icon::NumberGroup,
                        {rows({{combo("NumberFormat", K::FormatCombo, 118)},
                               {split("AccountingFormat", {"AccountingDollar", "AccountingPound", "AccountingEuro", "AccountingYuan", "AccountingNTD", "AccountingCHF", "-",
                                                   "MoreAccountingFormats"}), btn("PercentStyle"),
                                btn("CommaStyle"), btn("DecimalIncrease"), btn("DecimalDecrease")}})},
                        "FormatCellsNumberDialog"});
    t.groups.push_back({TR("Styles"), icon::StylesGroup,
                        {large({btn("ConditionalFormatting", K::Menu), btn("FormatAsTable", K::Gallery)}, true), gallery("CellStyles")},
                        ""});
    // grouped as in Excel, so the menu fits on the screen (it was one list of 30 entries, taller than a laptop screen)
    t.groups.back().blocks[0].items[0].menu = {">CfHighlightRules", "CfGreater", "CfLess", "CfBetween", "CfEqual", "CfTextContains", "CfDateOccurring", "CfDuplicate", "-", "CfMoreRulesHighlight", "<",
                                               ">CfTopBottomRules", "CfTop10", "CfTop10Percent", "CfBottom10", "CfBottom10Percent", "CfAboveAverage", "CfBelowAverage", "-", "CfMoreRulesTopBottom", "<",
                                               "-",
                                               ">CfDataBars", "CfBarBlue", "CfBarGreen", "CfBarRed", "CfBarOrange", "CfBarLightBlue", "CfBarPurple", "-",
                                               "CfBarSolidBlue", "CfBarSolidGreen", "CfBarSolidRed", "CfBarSolidOrange", "CfBarSolidLightBlue", "CfBarSolidPurple", "-",
                                               "CfMoreRulesBars", "<",
                                               ">CfColorScales", "CfScaleGYR", "CfScaleRYG", "CfScaleGWR", "CfScaleRWG", "CfScaleBWR", "CfScaleRWB", "CfScaleWR", "CfScaleRW",
                                               "CfScaleGW", "CfScaleWG", "CfScaleGY", "CfScaleYG", "-", "CfMoreRulesScales", "<",
                                               ">CfIconSets", "CfIcon3Arrows", "CfIcon3ArrowsGray", "CfIcon3Triangles", "CfIcon4ArrowsGray", "CfIcon4Arrows",
                                               "CfIcon5ArrowsGray", "CfIcon5Arrows", "-", "CfIcon3TrafficLights", "CfIcon3TrafficLightsRimmed", "CfIcon3Signs", "CfIcon4TrafficLights",
                                               "CfIcon4RedToBlack", "-", "CfIcon3Symbols", "CfIcon3Symbols2", "CfIcon3Flags", "-", "CfIcon3Stars", "CfIcon4Ratings", "CfIcon5Quarters",
                                               "CfIcon5Ratings", "CfIcon5Boxes", "-", "CfMoreRulesIcons", "<",
                                               "-", "CfNewRule", ">CfClearRules", "CfClearSelection", "CfClearSheet", "CfClearTable", "CfClearPivot", "<", "CfManageRules"};
    // Format as Table: Excel's gallery of style pictures in Light / Medium / Dark (the tiles are set by tables_ui.cpp)
    t.groups.back().blocks[0].items[1].gallery = "FormatAsTable";
    t.groups.push_back({TR("Cells"), icon::CellsGroup,
                        {large({split("InsertCells", {"InsertCells", "InsertRows", "InsertColumns", "InsertSheet"}),
                                split("DeleteCells", {"DeleteCells", "DeleteRows", "DeleteColumns", "DeleteSheet"}),
                                btn("FormatCellsMenu", K::Menu)})},
                        ""});
    // Excel's sections: Cell Size, Visibility, Organize Sheets, Protection
    t.groups.back().blocks[0].items[2].menu = {std::string("~") + TR("Cell Size"), "RowHeight", "AutoFitRows", "ColumnWidth", "AutoFitColumns", "DefaultWidth",
                                               std::string("~") + TR("Visibility"), ">HideUnhideMenu", "HideRows", "HideColumns", "HideSheet", "-",
                                               "UnhideRows", "UnhideColumns", "UnhideSheet", "<",
                                               std::string("~") + TR("Organize Sheets"), "RenameSheet", "CopySheet", ">TabColor", "<",
                                               std::string("~") + TR("Protection"), "ProtectSheet", "LockCell", "FormatCellsDialog"};
    t.groups.push_back({TR("Editing"), icon::EditGroup,
                        {stack({split("AutoSum", {"AutoSum", "AutoAverage", "AutoCount", "AutoMax", "AutoMin", "AutoSumMoreFunctions"}),
                                btn("FillMenu", K::Menu), btn("ClearMenu", K::Menu)}),
                         large({btn("SortFilter", K::Menu), btn("FindSelect", K::Menu)})},
                        ""});
    {
      auto& b = t.groups.back().blocks;
      b[0].items[1].menu = {"FillDown", "FillRight", "FillUp", "FillLeft", "FillAcrossWorksheets", "FillSeries", "FillJustify", "FlashFill"};
      b[0].items[2].menu = {"ClearAll", "ClearFormats", "ClearContents", "ClearComments", "ClearHyperlinks", "RemoveHyperlinks"};
      b[1].items[0].menu = {"SortAscending", "SortDescending", "CustomSort", "-", "ToggleFilter", "ClearFilter", "ReapplyFilter"};
      b[1].items[1].menu = {"Find", "Replace", "GoTo", "GoToSpecial", "-", "SelectFormulas", "SelectNotes", "SelectConditionalFormats", "SelectConstants",
                            "SelectDataValidation", "-", "SelectObjects", "SelectionPane"};
    }
    // Share: pictures of cells and windows (the menu lists the open chart windows too, screenshot_ui.cpp)
    t.groups.push_back({TR("Share"), icon::Screenshot,
                        {large({split("Screenshot", {"ScreenshotSelection", "ScreenshotTable", "ScreenshotSheet", "ScreenshotWorkbook", "-",
                                                     "ScreenshotVisible", "ScreenshotWindow", "-", "ScreenshotHeadings", "ScreenshotHiRes", "-",
                                                     "ScreenshotSaveLast"})})},
                        ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Insert");
    t.groups.push_back({TR("Tables"), icon::Table, {large({btn("InsertPivotTable"), btn("RecommendedPivotTables"), btn("InsertTable")})}, ""});
    t.groups.push_back({TR("Illustrations"), icon::Picture, {large({btn("InsertPicture"), btn("InsertShapesMenu", K::Menu), btn("InsertIcons")})}, ""});
    t.groups.back().blocks[0].items[1].menu = {"InsertShapeRectangle", "InsertShapeRoundedRectangle", "InsertShapeOval", "InsertShapeTriangle",
                                               "InsertShapeRightArrow", "-", "InsertShapeLine", "InsertShapeArrow"};
    // Excel's chart buttons: Recommended Charts, then galleries of every variant (Column or Bar, Line or Area...)
    t.groups.push_back({TR("Charts"), icon::ChartColumn,
                        {large({btn("RecommendedCharts")}),
                         large({btn("InsertChartColumnMenu", K::Menu), btn("InsertChartLineMenu", K::Menu), btn("InsertChartPieMenu", K::Menu),
                                btn("InsertChartScatterMenu", K::Menu)}),
                         stack({btn("InsertChartHierarchyMenu", K::Menu), btn("InsertChartStatisticMenu", K::Menu), btn("InsertChartWaterfallMenu", K::Menu)}),
                         stack({btn("InsertChartComboMenu", K::Menu), btn("InsertChartMap")}), large({btn("InsertPivotChart")})},
                        "InsertChartDialog"});
    {
      // Excel's galleries: a picture of each chart under its heading (2-D Column, 3-D Column, 2-D Bar...)
      auto& b = t.groups.back().blocks;
      b[1].items[0].menu = {std::string("=") + TR("2-D Column"), "InsertChartColumn", "InsertChartColumnStacked", "InsertChartColumnStacked100",
                            std::string("=") + TR("3-D Column"), "InsertChart3DClusteredColumn", "InsertChart3DStackedColumn", "InsertChart3DColumn",
                            std::string("=") + TR("2-D Bar"), "InsertChartBar", "InsertChartBarStacked", "InsertChartBarStacked100",
                            std::string("=") + TR("3-D Bar"), "InsertChart3DBar", "-", "InsertChartMoreColumn"};
      b[1].items[1].menu = {std::string("=") + TR("2-D Line"), "InsertChartLine", "InsertChartLineStacked", "InsertChartLineStacked100", "InsertChartLineMarkers",
                            "InsertChartLineMarkersStacked", "InsertChartLineMarkersStacked100",
                            std::string("=") + TR("3-D Line"), "InsertChart3DLine",
                            std::string("=") + TR("2-D Area"), "InsertChartArea", "InsertChartAreaStacked", "InsertChartAreaStacked100",
                            std::string("=") + TR("3-D Area"), "InsertChart3DArea", "-", "InsertChartMoreLine"};
      b[1].items[2].menu = {std::string("=") + TR("2-D Pie"), "InsertChartPie", "InsertChartPieOfPie", "InsertChartBarOfPie", std::string("=") + TR("3-D Pie"), "InsertChart3DPie",
                            std::string("=") + TR("Doughnut"), "InsertChartDoughnut", "-", "InsertChartMorePie"};
      b[1].items[3].menu = {std::string("=") + TR("Scatter"), "InsertChartScatter", "InsertChartScatterSmooth", "InsertChartScatterSmoothNoMarkers", "InsertChartScatterLines",
                            "InsertChartScatterLinesNoMarkers", std::string("=") + TR("Bubble"), "InsertChartBubble", "-", "InsertChartMoreScatter"};
      b[2].items[0].menu = {std::string("=") + TR("Treemap"), "InsertChartTreemap", std::string("=") + TR("Sunburst"), "InsertChartSunburst"};
      b[2].items[1].menu = {std::string("=") + TR("Histogram"), "InsertChartHistogram", "InsertChartPareto", std::string("=") + TR("Box and Whisker"), "InsertChartBoxWhisker"};
      b[2].items[2].menu = {std::string("=") + TR("Waterfall"), "InsertChartWaterfall", "InsertChartFunnel",
                            std::string("=") + TR("Stock"), "InsertChartStockHLC", "InsertChartStock", "InsertChartStockVHLC", "InsertChartStockVOHLC",
                            std::string("=") + TR("Surface"), "InsertChartSurface", "InsertChartWireframe", "InsertChartContour",
                            std::string("=") + TR("Radar"), "InsertChartRadar", "InsertChartRadarMarkers", "InsertChartRadarFilled"};
      b[3].items[0].menu = {std::string("=") + TR("Combo"), "InsertChartCombo", "InsertChartComboSecondary", "InsertChartComboArea", "-", "InsertChartComboCustom"};
    }
    t.groups.push_back({TR("Sparklines"), icon::SparkLine, {large({btn("InsertSparkLine"), btn("InsertSparkColumn"), btn("InsertSparkWinLoss")})}, ""});
    t.groups.push_back({TR("Filters##ctx_insert"), icon::Filter, {large({btn("InsertSlicer"), btn("InsertTimeline")})}, ""});
    t.groups.push_back({TR("Links"), icon::Link, {large({btn("InsertHyperlink")})}, ""});
    t.groups.push_back({TR("Comments"), icon::Comment, {large({btn("NewComment")})}, ""});
    // Excel's Text group: Text Box, Header & Footer, WordArt (its gallery is filled by the app)
    t.groups.push_back({TR("Text"), icon::TextBox, {large({btn("InsertTextBox"), btn("InsertHeaderFooter"), btn("InsertWordArtMenu", K::Menu)})}, ""});
    t.groups.push_back({TR("Symbols"), icon::Formula, {large({btn("InsertSymbol")})}, ""});
    t.groups.push_back({TR("Controls"), icon::ControlsGroup, {stack({btn("InsertButton"), btn("InsertComboBox")})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Page Layout");
    // the menus list the built-in themes, color sets and font pairs (commands registered by the app)
    auto ids = [](const char* prefix, size_t n) {
      std::vector<std::string> v;
      for (size_t i = 0; i < n; ++i) v.push_back(prefix + std::to_string(i));
      return v;
    };
    t.groups.push_back({TR("Themes"), icon::Themes,
                        {large({split("ThemesMenu", ids("ThemeApply", model::BuiltinThemes().size()), K::Menu)}),
                         stack({split("ThemeColorsMenu", ids("ThemeColors", model::BuiltinThemeColors().size()), K::Menu),
                                split("ThemeFontsMenu", ids("ThemeFonts", model::BuiltinThemeFonts().size()), K::Menu),
                                btn("ThemeEffectsMenu", K::Menu)})},
                        ""});
    // boxes drawn by the app (the Scale spinner, Sheet Options' View / Print check boxes)
    auto layout_box = [](const char* cmd, float w) {
      Item it;
      it.kind = K::Custom;
      it.cmd = cmd;
      it.combo_width = w;
      it.draw = [id = std::string(cmd)](const ImRect& r) {
        if (Context().layout_box) Context().layout_box(id, r);
      };
      return it;
    };
    t.groups.push_back({TR("Page Setup"), icon::Margins,
                        {large({btn("MarginsMenu", K::Menu), btn("OrientationMenu", K::Menu), btn("PaperSizeMenu", K::Menu), btn("PrintAreaMenu", K::Menu),
                                btn("BreaksMenu", K::Menu), btn("SheetBackground"), btn("PrintTitles")})},
                        "PageSetupDialog"});
    {
      // the app updates Size (the printer's papers), Margins (Last Custom Setting) and Print Area (Add to Print Area)
      auto& b = t.groups.back().blocks[0].items;
      b[0].menu = {"MarginsNormal", "MarginsWide", "MarginsNarrow", "-", "MarginsCustom"};
      b[1].menu = {"OrientPortrait", "OrientLandscape"};
      b[2].menu = {"Paper1", "Paper5", "Paper7", "Paper3", "Paper8", "Paper9", "Paper11", "Paper12", "Paper13"};
      b[3].menu = {"SetPrintArea", "ClearPrintArea"};
      b[4].menu = {"InsertPageBreak", "RemovePageBreak", "ResetPageBreaks"};
    }
    // Excel's Scale to Fit: Width and Height (Automatic, 1-9 pages, More Pages...) and the Scale spinner
    t.groups.push_back({TR("Scale to Fit"), icon::PaperSize,
                        {stack({btn("ScaleWidthMenu", K::Menu), btn("ScaleHeightMenu", K::Menu), layout_box("ScaleAdjust", 150)})},
                        "PageSetupDialog"});
    {
      auto pages = [](const char* prefix, const char* more) {
        std::vector<std::string> m{std::string(prefix) + "0"};
        for (int i = 1; i <= 9; ++i) m.push_back(prefix + std::to_string(i));
        m.push_back("-");
        m.push_back(more);
        return m;
      };
      auto& b = t.groups.back().blocks[0].items;
      b[0].menu = pages("ScaleWidth", "ScaleMorePages");
      b[1].menu = pages("ScaleHeight", "ScaleMorePages");
    }
    // Sheet Options: Gridlines and Headings, each with View and Print (Excel's two columns of check boxes)
    t.groups.push_back({TR("Sheet Options"), icon::DisplayGroup,
                        {stack({layout_box("SheetOptionsHead", 150), layout_box("SheetOptionsView", 150), layout_box("SheetOptionsPrint", 150)})},
                        "PageSetupSheet"});
    // Excel's Arrange: the selected objects' order, alignment, grouping and rotation (as on Shape Format)
    t.groups.push_back({TR("Arrange"), icon::Group,
                        {stack({split("BringForward", {"BringForward", "BringToFront"}), split("SendBackward", {"SendBackward", "SendToBack"}), btn("SelectionPane")}),
                         stack({btn("ObjAlignMenu", K::Menu), btn("ObjGroupMenu", K::Menu), btn("ObjRotateMenu", K::Menu)})},
                        ""});
    {
      auto& b = t.groups.back().blocks[1].items;
      b[0].menu = {"ObjAlignLeft", "ObjAlignCenter", "ObjAlignRight", "-", "ObjAlignTop", "ObjAlignMiddle", "ObjAlignBottom", "-", "ObjDistributeH", "ObjDistributeV"};
      b[1].menu = {"GroupObjects", "UngroupObjects"};
      b[2].menu = {"RotateRight90", "RotateLeft90", "-", "FlipVertical", "FlipHorizontal"};
    }
    t.groups.push_back({TR("Print"), icon::Print, {large({btn("FilePrint"), btn("ExportPdf")})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Formulas");
    // Excel's Function Library: a drop-down of the functions of each category (choosing one opens Function
    // Arguments); More Functions holds the other categories as submenus. Recently Used is filled by the app.
    auto fn_menu = [](const char* sample) {
      std::vector<std::string> m;
      const formula::FuncHelp* s = formula::FindFuncHelp(sample);
      for (const auto& h : formula::AllFuncHelp()) {
        if (s && std::strcmp(h.category, s->category) == 0) m.push_back(std::string("InsertFn_") + h.name);
      }
      std::sort(m.begin(), m.end());
      return m;
    };
    std::vector<std::string> more;
    for (const auto& [head, sample] : std::vector<std::pair<const char*, const char*>>{{"FunctionsStatistical", "AVERAGE"}, {"FunctionsEngineering", "BIN2DEC"},
                                                                                      {"FunctionsInformation", "ISBLANK"}, {"FunctionsCompatibility", "TDIST"},
                                                                                      {"FunctionsDatabase", "DSUM"}, {"FunctionsWeb", "ENCODEURL"}}) {
      const std::vector<std::string> m = fn_menu(sample);
      if (m.empty()) continue;
      more.push_back(std::string(">") + head);
      more.insert(more.end(), m.begin(), m.end());
      more.push_back("<");
    }
    t.groups.push_back({TR("Function Library"), icon::Formula,
                        {large({btn("InsertFunction"), split("AutoSum", {"AutoSum", "AutoAverage", "AutoCount", "AutoMax", "AutoMin", "AutoSumMoreFunctions"}),
                                split("FunctionsRecentlyUsed", {}, K::Menu), split("FunctionsFinancial", fn_menu("PV"), K::Menu),
                                split("FunctionsLogical", fn_menu("IF"), K::Menu), split("FunctionsText", fn_menu("LEFT"), K::Menu),
                                split("FunctionsDateTime", fn_menu("NOW"), K::Menu), split("FunctionsLookup", fn_menu("VLOOKUP"), K::Menu),
                                split("FunctionsMath", fn_menu("SUM"), K::Menu), split("FunctionsMore", more, K::Menu)})},
                        ""});
    // Use in Formula: a drop-down of the names (filled by the app) and Paste Names... (F3), as in Excel
    t.groups.push_back({TR("Defined Names"), icon::NameManager,
                        {large({btn("NameManager")}), stack({split("DefineName", {"DefineName", "ApplyNames"}), split("UseInFormula", {"PasteName"}, K::Menu), btn("CreateNamesFromSelection")})}, ""});
    // Error Checking: the dialog, Trace Error and the circular references (filled by the app)
    t.groups.push_back({TR("Formula Auditing"), icon::Eye,
                        {stack({btn("TracePrecedents"), btn("TraceDependents"), split("RemoveArrows", {"RemoveArrows", "RemovePrecedentArrows", "RemoveDependentArrows"})}),
                         stack({btn("ShowFormulas", K::Toggle), split("ErrorChecking", {"ErrorChecking", "TraceError", "CircularReferences"}), btn("EvaluateFormula")}),
                         large({btn("WatchWindow")})},
                        ""});
    t.groups.push_back({TR("Calculation"), icon::Calculator, {large({btn("CalculationOptions", K::Menu)}), stack({btn("Calculate"), btn("CalculateSheet")})}, ""});
    t.groups.back().blocks[0].items[0].menu = {"CalcAutomatic", "CalcSemiautomatic", "CalcManual"};
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Data");
    // Excel's Get & Transform Data: Get Data (From File > From Text/CSV) and From Text/CSV
    t.groups.push_back({TR("Get & Transform Data"), icon::Data, {large({btn("GetDataMenu", K::Menu), btn("ImportText")})}, ""});
    t.groups.back().blocks[0].items[0].menu = {std::string(">") + TR("From File"), "ImportText", "<"};
    t.groups.push_back({TR("Queries & Connections"), icon::Refresh, {large({btn("RefreshAll")}), stack({btn("EditLinks")})}, ""});
    t.groups.push_back({TR("Sort & Filter"), icon::SortFilter,
                        {stack({btn("SortAscending"), btn("SortDescending")}), large({btn("CustomSort"), btn("ToggleFilter")}),
                         stack({btn("ClearFilter"), btn("ReapplyFilter"), btn("AdvancedFilter")})},
                        ""});
    t.groups.push_back({TR("Data Tools"), icon::Validation,
                        {large({btn("TextToColumns"), btn("FlashFill"), btn("RemoveDuplicates"),
                                split("DataValidation", {"DataValidation", "CircleInvalidData", "ClearValidationCircles"})}),
                         stack({btn("Consolidate")})},
                        ""});
    t.groups.push_back({TR("Forecast"), icon::Forecast, {large({btn("WhatIfMenu", K::Menu), btn("ForecastSheet")})}, ""});
    t.groups.back().blocks[0].items[0].menu = {"ScenarioManager", "GoalSeek", "DataTableDialog"};
    t.groups.push_back({TR("Analysis"), icon::Solver, {large({btn("DataAnalysis"), btn("Solver")})}, ""});
    t.groups.push_back({TR("Outline"), icon::Group,
                        {large({split("GroupOutline", {"GroupOutline", "AutoOutline"}), split("UngroupOutline", {"UngroupOutline", "ClearOutline"}), btn("Subtotal")}),
                         stack({btn("ShowDetail"), btn("HideDetail")})},
                        "OutlineSettings"});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Review");
    // Excel's order: Proofing, Comments, Notes (one drop-down), Protect
    t.groups.push_back({TR("Proofing"), icon::Spelling, {large({btn("Spelling")})}, ""});
    t.groups.push_back({TR("Comments"), icon::Comment,
                        {large({btn("NewComment")}), stack({btn("DeleteCommentThread"), btn("PreviousComment"), btn("NextComment")}), large({btn("ShowComments")})},
                        ""});
    t.groups.push_back({TR("Notes"), icon::Comment, {large({btn("NotesMenu", K::Menu)})}, ""});
    t.groups.back().blocks[0].items[0].menu = {"InsertComment", "PreviousNote", "NextNote", "ShowHideNote", "ShowAllNotes", "-", "ConvertToComments"};
    t.groups.push_back({TR("Protect"), icon::ProtectSheet, {large({btn("ProtectSheet"), btn("ProtectWorkbook"), btn("AllowEditRanges")})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("View");
    t.groups.push_back(
        {TR("Workbook Views"), icon::ViewNormal, {large({btn("ViewNormal"), btn("ViewPageBreakPreview"), btn("ViewPageLayout"), btn("CustomViews")})}, ""});
    // Excel's Show group: Ruler and Gridlines, Formula Bar and Headings
    t.groups.push_back({TR("Show"), icon::DisplayGroup,
                        {stack({check("ToggleRuler", &Context().ruler), check("ToggleGridlines", show_gridlines)}),
                         stack({check("ToggleFormulaBar", show_formula_bar), check("ToggleHeadings", show_headings)}), large({btn("FocusCell", K::Toggle)})},
                        ""});
    t.groups.push_back({TR("Zoom"), icon::Zoom, {large({btn("ZoomDialog"), btn("Zoom100"), btn("ZoomToSelection")})}, ""});
    // Excel's Window group: New Window, Arrange All, Freeze Panes; Split / Hide / Unhide; View Side by Side /
    // Synchronous Scrolling / Reset Window Position; Switch Windows (then YCGrid's containers and layouts)
    t.groups.push_back({TR("Window"), icon::WindowGroup,
                        {large({btn("NewWindow"), btn("ArrangeAll"),
                                split("FreezePanes", {"FreezePanes", "FreezeTopRow", "FreezeFirstColumn", "FreezeCenterColumns"}, K::Menu)}),
                         stack({btn("SplitWindow", K::Toggle), btn("HideWindow"), btn("UnhideWindow")}),
                         stack({btn("ViewSideBySide", K::Toggle), btn("SyncScrolling", K::Toggle), btn("ResetWindowPosition")}),
                         large({btn("SwitchWindowsMenu", K::Menu)}), large({btn("NewContainer")}), stack({btn("LayoutSave"), btn("LayoutLoad"), btn("LayoutReset")})},
                        ""});
    t.groups.push_back({TR("Macros"), icon::Play, {large({btn("ViewMacros")})}, ""});
    t.groups.push_back({"Python", icon::Console, {large({btn("ImmediateWindow"), btn("ErrorLog")})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Developer");
    t.groups.push_back({TR("Code"), icon::Code,
                        {large({btn("OpenVSCode"), btn("DebugInVSCode"), btn("Macros")}), stack({btn("ReloadPython"), btn("ImmediateWindow"), btn("ErrorLog")}),
                         large({btn("MacroSecurity")})},
                        ""});
    t.groups.push_back({TR("Controls"), icon::ControlsGroup,
                        {large({btn("InsertControlsMenu", K::Menu), btn("DesignMode", K::Toggle)}), stack({btn("ControlProperties"), btn("ViewCode")})},
                        ""});
    t.groups.back().blocks[0].items[0].menu = {"InsertFormButton",  "InsertFormComboBox",   "InsertFormCheckBox", "InsertFormSpinButton", "InsertFormListBox",
                                               "InsertFormOptionButton", "InsertFormGroupBox", "InsertFormLabel", "InsertFormScrollBar", "-",
                                               "InsertButton",      "InsertComboBox",       "InsertCheckBox",     "InsertOptionButton",   "InsertToggleButton",
                                               "InsertTextBoxControl", "InsertLabel",       "InsertListBox",      "InsertSpinButton",     "InsertScrollBar",
                                               "InsertImageControl"};
    g_tabs.push_back(std::move(t));
  }
  {
    // [YC] trading: safety for live screens (safety_ui.cpp)
    Tab t;
    t.label = TR("Trading");
    t.groups.push_back({TR("Safety"), icon::ProtectSheet, {large({btn("OperatorMode", K::Toggle), btn("KillSwitch")}), stack({btn("Heartbeats")})}, ""});
    t.groups.push_back({TR("Parameters"), icon::Validation, {large({btn("MarkParameter"), btn("ChangeLog")})}, ""});
    t.groups.push_back({TR("Recording"), icon::History, {large({split("RecordData", {"RecordData", "RecordEverything"}), btn("ReplayData"), btn("ReplayLive")})}, ""});
    t.groups.push_back({TR("Monitor"), icon::Forecast, {large({btn("LatencyMonitor", K::Toggle), btn("AlertRules", K::Toggle)})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Table Design");
    t.visible = &Context().table;
    ContextState& cx = Context();
    {
      // Excel's Properties group: Table Name: over a box to type the name in, then Resize Table
      Item label, box;
      label.kind = box.kind = K::Custom;
      label.cmd = "TableNameLabel";
      box.cmd = "RenameTable";
      label.combo_width = box.combo_width = 110;
      label.draw = [](const ImRect& r) {
        if (Context().table_name_box) Context().table_name_box(0, r);
      };
      box.draw = [](const ImRect& r) {
        if (Context().table_name_box) Context().table_name_box(1, r);
      };
      t.groups.push_back({TR("Properties"), icon::TableDesign, {stack({label, box, btn("ResizeTable")})}, ""});
    }
    t.groups.push_back({TR("Tools"), icon::TableDesign, {stack({btn("InsertPivotTable"), btn("RemoveDuplicates"), btn("ConvertToRange")}), large({btn("InsertSlicer")})}, ""});
    t.groups.push_back({TR("Table Style Options"), icon::TableDesign,
                        {stack({check("TblHeaderRow", &cx.tbl_header), check("TblTotalRow", &cx.tbl_totals), check("TblBandedRows", &cx.tbl_banded_rows)}),
                         stack({check("TblFirstColumn", &cx.tbl_first_col), check("TblLastColumn", &cx.tbl_last_col), check("TblBandedColumns", &cx.tbl_banded_cols)}),
                         stack({check("TblFilterButton", &cx.tbl_filter)})},
                        ""});
    t.groups.push_back({TR("Table Styles"), icon::FormatAsTable, {large({btn("TableStylesMenu", K::Gallery)}, true)}, ""});
    t.groups.back().blocks[0].items[0].gallery = "TableStyles";
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("PivotTable Analyze");
    t.visible = &Context().pivot;
    ContextState& cx = Context();
    {
      // Excel's PivotTable group: "PivotTable Name:" over a box to type the name in, then Options
      Item label, box;
      label.kind = box.kind = K::Custom;
      label.cmd = "PivotNameLabel";
      box.cmd = "PivotRename";
      label.combo_width = box.combo_width = 130;
      label.draw = [](const ImRect& r) {
        if (Context().pivot_name_box) Context().pivot_name_box(0, r);
      };
      box.draw = [](const ImRect& r) {
        if (Context().pivot_name_box) Context().pivot_name_box(1, r);
      };
      t.groups.push_back({TR("PivotTable"), icon::PivotTable, {stack({label, box, btn("PivotOptions")})}, ""});
    }
    {
      // Active Field: the field's name (a box: typing renames it), Field Settings, Expand / Collapse Field
      Item box;
      box.kind = K::Custom;
      box.cmd = "PivotActiveField";
      box.combo_width = 170;
      box.draw = [](const ImRect& r) {
        if (Context().pivot_field_box) Context().pivot_field_box(r);
      };
      t.groups.push_back({TR("Active Field"), icon::PivotTable, {stack({box, btn("PivotFieldSettings")}), stack({btn("PivotExpandField"), btn("PivotCollapseField")})}, ""});
    }
    t.groups.push_back({TR("Group"), icon::Group, {stack({btn("PivotGroupSelection"), btn("PivotUngroup"), btn("PivotGroupField")})}, ""});
    t.groups.push_back({TR("Filter"), icon::Filter, {large({btn("InsertSlicer"), btn("InsertTimeline")}), stack({btn("PivotFilterConnections")})}, ""});
    t.groups.push_back({TR("Data"), icon::Refresh, {large({btn("PivotRefresh"), btn("RefreshAll"), btn("PivotChangeSource")})}, ""});
    t.groups.push_back({TR("Actions"), icon::Table, {large({btn("PivotClearMenu", K::Menu), btn("PivotSelectMenu", K::Menu), btn("PivotMove"), btn("PivotDelete")})}, ""});
    {
      auto& b = t.groups.back().blocks[0].items;
      b[0].menu = {"PivotClear", "PivotClearFilters"};
      b[1].menu = {"PivotSelectLabelsValues", "PivotSelectValues", "PivotSelectLabels", "-", "PivotSelectEntire", "PivotEnableSelection"};
    }
    t.groups.push_back({TR("Calculations"), icon::Formula, {large({btn("PivotCalcMenu", K::Menu)})}, ""});
    t.groups.back().blocks[0].items[0].menu = {"PivotCalcField", "PivotCalcItem"};
    t.groups.push_back({TR("Tools"), icon::ChartColumn, {large({btn("PivotChart"), btn("RecommendedPivotTables")})}, ""});
    t.groups.push_back({TR("Show"), icon::Panel,
                        {large({btn("PivotFieldList", K::Toggle)}),
                         stack({check("PivotToggleButtons", &cx.pv_buttons), check("PivotToggleFieldHeaders", &cx.pv_headers), check("PivotChartButtons", &cx.pv_chart_buttons)})},
                        ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Design##ctx_pivot");
    t.visible = &Context().pivot;
    t.groups.push_back({TR("Layout"), icon::FormatAsTable,
                        {large({btn("PivotSubtotalsMenu", K::Menu), btn("PivotGrandMenu", K::Menu), btn("PivotLayoutMenu", K::Menu), btn("PivotBlankRowsMenu", K::Menu)})}, ""});
    {
      auto& b = t.groups.back().blocks[0].items;
      b[0].menu = {"PivotSubtotalsNone", "PivotSubtotalsBottom", "PivotSubtotalsTop"};
      b[1].menu = {"PivotGrandOff", "PivotGrandOn", "PivotGrandRows", "PivotGrandCols"};
      b[2].menu = {"PivotLayoutCompact", "PivotLayoutOutline", "PivotLayoutTabular", "-", "PivotLayoutRepeat", "PivotLayoutNoRepeat"};
      b[3].menu = {"PivotBlankRowsInsert", "PivotBlankRowsRemove"};
    }
    {
      ContextState& pcx = Context();
      t.groups.push_back({TR("PivotTable Style Options"), icon::FormatAsTable,
                          {stack({check("PivotStyleRowHeaders", &pcx.pv_row_headers), check("PivotStyleColHeaders", &pcx.pv_col_headers)}),
                           stack({check("PivotStyleBandedRows", &pcx.pv_banded_rows), check("PivotStyleBandedCols", &pcx.pv_banded_cols)})},
                          ""});
    }
    // Excel's gallery of every PivotTable style as a picture (the tiles are set by pivot_ui.cpp)
    t.groups.push_back({TR("PivotTable Styles"), icon::FormatAsTable, {large({btn("PivotStylesMenu", K::Gallery)}, true)}, ""});
    t.groups.back().blocks[0].items[0].gallery = "PivotStyles";
    g_tabs.push_back(std::move(t));
  }
  // Shape Format / Picture Format / chart Format (contextual): the Arrange and Size groups are shared
  auto arrange = [&](Tab& t) {
    t.groups.push_back({TR("Arrange"), icon::Group,
                        {stack({split("BringForward", {"BringForward", "BringToFront"}), split("SendBackward", {"SendBackward", "SendToBack"}), btn("SelectionPane")}),
                         stack({btn("ObjAlignMenu", K::Menu), btn("ObjGroupMenu", K::Menu), btn("ObjRotateMenu", K::Menu)})},
                        ""});
    auto& b = t.groups.back().blocks[1].items;
    b[0].menu = {"ObjAlignLeft", "ObjAlignCenter", "ObjAlignRight", "-", "ObjAlignTop", "ObjAlignMiddle", "ObjAlignBottom", "-", "ObjDistributeH", "ObjDistributeV"};
    b[1].menu = {"GroupObjects", "UngroupObjects"};
    b[2].menu = {"RotateRight90", "RotateLeft90", "-", "FlipVertical", "FlipHorizontal"};
    Item h, w;
    h.kind = w.kind = K::Custom;
    h.cmd = "ShapeHeight";
    w.cmd = "ShapeWidth";
    h.combo_width = w.combo_width = 120;
    h.draw = [](const ImRect& r) {
      if (Context().size_box) Context().size_box(0, r);
    };
    w.draw = [](const ImRect& r) {
      if (Context().size_box) Context().size_box(1, r);
    };
    t.groups.push_back({TR("Size##ctx_shape"), icon::Margins, {stack({h, w})}, "FormatShapePane"});
  };
  {
    Tab t;
    t.label = TR("Chart Design");
    t.visible = &Context().chart;
    t.groups.push_back({TR("Chart Layouts"), icon::ChartColumn, {large({btn("ChartAddElementMenu", K::Menu), btn("ChartQuickLayoutMenu", K::Menu)})}, ""});
    for (int k = 1; k <= 11; ++k) t.groups.back().blocks[0].items[1].menu.push_back("ChartQuickLayout" + std::to_string(k));
    // Add Chart Element: Excel's submenus (Axes, Axis Titles, Chart Title, Data Labels, Error Bars, Gridlines, Legend,
    // Trendline), each with its choices
    t.groups.back().blocks[0].items[0].menu = {
        std::string(">") + TR("Axes"), "ChartElemAxisCat", "ChartElemAxisVal", "ChartElemAxisVal2", "<",
        std::string(">") + TR("Axis Titles"), "ChartElemAxisTitleCat", "ChartElemAxisTitleVal", "ChartElemAxisTitleVal2", "<",
        std::string(">") + TR("Chart Title"), "ChartElemTitleNone", "ChartElemTitleAbove", "ChartElemTitleOverlay", "ChartElemTitle", "<",
        std::string(">") + TR("Data Labels"), "ChartElemLabelsNone", "ChartElemLabelsCenter", "ChartElemLabelsInsideEnd", "ChartElemLabelsInsideBase",
        "ChartElemLabelsOutsideEnd", "ChartElemDataLabels", "<",
        std::string(">") + TR("Error Bars"), "ChartElemErrNone", "ChartElemErrStdErr", "ChartElemErrPercent", "ChartElemErrStdDev", "<",
        std::string(">") + TR("Gridlines"), "ChartElemGridVal", "ChartElemGridCat", "ChartElemGridValMinor", "ChartElemGridCatMinor", "<",
        std::string(">") + TR("Legend"), "ChartElemLegendNone", "ChartElemLegendRight", "ChartElemLegendTop", "ChartElemLegendLeft", "ChartElemLegendBottom",
        "ChartElemLegendCorner", "<",
        std::string(">") + TR("Lines"), "ChartElemLinesNone", "ChartElemHiLoLines", "<",
        std::string(">") + TR("Trendline"), "ChartElemTrendNone", "ChartElemTrendLinear", "ChartElemTrendExp", "ChartElemTrendForecast", "ChartElemTrendMovingAvg", "<",
        std::string(">") + TR("Up/Down Bars"), "ChartElemUpDownNone", "ChartElemUpDownBars", "<"};
    t.groups.push_back({TR("Chart Styles"), icon::ChartColumn, {large({btn("ChartChangeColorsMenu", K::Menu), btn("ChartStylesMenu", K::Menu)})}, ""});
    {
      auto& m = t.groups.back().blocks[0].items[0].menu;
      for (int k = 0; k < 10; ++k) {
        if (k == 4) m.push_back("-");
        m.push_back("ChartColors" + std::to_string(k));
      }
      // every style of the largest set; the app shows those of the selected chart's type (menu_entry_shown)
      for (int k = 1; k <= 15; ++k) t.groups.back().blocks[0].items[1].menu.push_back("ChartStyle" + std::to_string(k));
    }
    t.groups.push_back({TR("Data"), icon::Rows, {large({btn("ChartSwitchRowCol"), btn("ChartSelectData")})}, ""});
    t.groups.push_back({TR("Type"), icon::ChartCombo, {large({btn("ChartChangeTypeMenu", K::Split)})}, ""});
    {
      // grouped like the Insert tab's chart buttons, so the menu fits on the screen (31 entries were taller than it)
      auto ct = [](int i) { return "ChartType" + std::to_string(i); };
      auto& m = t.groups.back().blocks[0].items[0].menu;
      // the kinds with their variants (model::ChartType numbers; the variants added later are 31-45)
      m.insert(m.end(), {std::string(">") + TR("Column"), ct(2), ct(3), ct(31), "<"});
      m.insert(m.end(), {std::string(">") + TR("Bar"), ct(4), ct(32), ct(33), "<"});
      m.insert(m.end(), {std::string(">") + TR("Line"), ct(0), ct(34), ct(35), ct(1), ct(36), ct(37), "<"});
      m.insert(m.end(), {std::string(">") + TR("Area"), ct(6), ct(38), ct(39), "<"});
      m.insert(m.end(), {std::string(">") + TR("X Y (Scatter)"), ct(5), ct(42), ct(43), ct(40), ct(41), ct(44), "<"});
      m.insert(m.end(), {std::string(">") + TR("Pie"), ct(7), ct(46), ct(47), ct(8), "<"});
      m.insert(m.end(), {std::string(">") + TR("Stock"), ct(45), ct(9), ct(48), ct(49), "<"});
      m.insert(m.end(), {std::string(">") + TR("Radar"), ct(10), ct(11), ct(12), "<"});
      m.insert(m.end(), {">InsertChartWaterfallMenu", ct(13), ct(19), "<"});
      m.insert(m.end(), {">InsertChartStatisticMenu", ct(14), ct(15), ct(16), "<"});
      m.insert(m.end(), {">InsertChartHierarchyMenu", ct(17), ct(18), "<"});
      m.push_back(">InsertChart3DMenu");
      for (int i = 20; i <= 26; ++i) m.push_back(ct(i));
      m.insert(m.end(), {"<", ">InsertChartSurfaceMenu", ct(27), ct(28), ct(29), "<", ct(30)});
      static_assert(model::kChartTypeCount == 50, "a new chart type goes into this menu");
    }
    t.groups.push_back({TR("Location"), icon::Chart, {large({btn("MoveChart")})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Format##ctx_chart");
    t.visible = &Context().chart;
    // Excel's Current Selection: the Chart Elements box (every element of the chart by name, the selected one shown),
    // Format Selection and Reset to Match Style under it
    Item elems;
    elems.kind = K::Custom;
    elems.cmd = "ChartElementsMenu";
    elems.combo_width = 150;
    elems.draw = [](const ImRect& r) {
      if (Context().chart_elements_box) Context().chart_elements_box(r);
    };
    t.groups.push_back({TR("Current Selection"), icon::Properties, {stack({elems, btn("ChartFormatSelection"), btn("ChartResetStyle")})}, ""});
    arrange(t);  // Excel's Arrange and Size of the chart
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Sparkline");
    t.visible = &Context().sparkline;
    ContextState& cx = Context();
    // Excel's Sparkline tab: Sparkline (Edit Data), Type, Show, Style, Group
    t.groups.push_back({TR("Sparkline##ctx_spark"), icon::SparkLine,
                        {large({split("SparkEditGroup", {"SparkEditGroup", "SparkEditSingle", "-", "SparkHiddenEmpty", "SparkSwitchRowCol"})})}, ""});
    t.groups.push_back({TR("Type"), icon::SparkLine, {large({btn("SparkTypeLine"), btn("SparkTypeColumn"), btn("SparkTypeWinLoss")})}, ""});
    t.groups.push_back({TR("Show"), icon::SparkLine,
                        {stack({check("SparkHigh", &cx.sp_high), check("SparkLow", &cx.sp_low), check("SparkNegative", &cx.sp_negative)}),
                         stack({check("SparkFirst", &cx.sp_first), check("SparkLast", &cx.sp_last), check("SparkMarkers", &cx.sp_markers)})},
                        ""});
    t.groups.push_back({TR("Style##ctx_spark"), icon::FillColor,
                        {large({btn("SparkStylesMenu", K::Menu)}), stack({btn("SparkColorMenu", K::Menu), btn("SparkMarkerColorMenu", K::Menu)})}, ""});
    {
      auto& g = t.groups.back().blocks;
      g[0].items[0].menu = {"="};
      for (int k = 0; k < 36; ++k) g[0].items[0].menu.push_back("SparkStyle" + std::to_string(k));
      g[1].items[0].menu = {"#SparkColor", "-", ">SparkWeightMenu", "SparkWeight0", "SparkWeight1", "SparkWeight2", "SparkWeight3", "SparkWeight4",
                            "SparkWeight5", "SparkWeight6", "<"};
      g[1].items[1].menu = {">SparkMarkerNegative", "#SparkMarkerColorNegative", "<", ">SparkMarkerMarkers", "#SparkMarkerColorMarkers", "<",
                            ">SparkMarkerHigh",     "#SparkMarkerColorHigh",     "<", ">SparkMarkerLow",     "#SparkMarkerColorLow",     "<",
                            ">SparkMarkerFirst",    "#SparkMarkerColorFirst",    "<", ">SparkMarkerLast",    "#SparkMarkerColorLast",    "<"};
    }
    t.groups.push_back({TR("Group##ctx_spark"), icon::SparkLine,
                        {large({btn("SparkAxisMenu", K::Menu)}), stack({btn("SparkGroup"), btn("SparkUngroup"), split("SparkClear", {"SparkClear", "SparkClearGroups"})})},
                        ""});
    // Excel's headings over the three parts: the two Automatic / Same / Custom sets looked the same without them
    t.groups.back().blocks[0].items[0].menu = {std::string("~") + TR("Horizontal Axis Options"), "SparkAxisGeneral", "SparkAxisDate", "SparkAxis",
                                               "SparkRightToLeft", std::string("~") + TR("Vertical Axis Minimum Value Options"), "SparkMinAuto",
                                               "SparkMinSame", "SparkMinCustom", std::string("~") + TR("Vertical Axis Maximum Value Options"),
                                               "SparkMaxAuto", "SparkMaxSame", "SparkMaxCustom"};
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Shape Format");
    t.visible = &Context().shape;
    ContextState& cx = Context();
    t.groups.push_back({TR("Insert Shapes"), icon::Shapes, {large({btn("InsertShapesMenu", K::Menu), btn("InsertTextBox")})}, ""});
    t.groups.back().blocks[0].items[0].menu = {"InsertShapeRectangle", "InsertShapeRoundedRectangle", "InsertShapeOval", "InsertShapeTriangle",
                                               "InsertShapeRightArrow", "-", "InsertShapeLine", "InsertShapeArrow"};
    t.groups.push_back({TR("Shape Styles"), icon::FillColor,
                        {rows({{labeled(color("ShapeFill", &cx.shape_fill)), btn("ShapeNoFill")},
                               {labeled(color("ShapeOutline", &cx.shape_line)), btn("ShapeNoOutline")},
                               {btn("ShapeWeightMenu", K::Menu), btn("ShapeDashesMenu", K::Menu), btn("ShapeArrowsMenu", K::Menu)}}),
                         large({btn("ShapeQuickStylesMenu", K::Menu)}), stack({btn("ShapeEffectsMenu", K::Menu)})},
                        "FormatShapePane"});
    {
      auto& r = t.groups.back().blocks[0].rows[2];
      r[0].menu = {"ShapeWeight0", "ShapeWeight1", "ShapeWeight2", "ShapeWeight3", "ShapeWeight4", "ShapeWeight5", "ShapeWeight6", "ShapeWeight7"};
      r[1].menu = {"ShapeDash0", "ShapeDash1", "ShapeDash2", "ShapeDash3", "ShapeDash4", "ShapeDash5"};
      r[2].menu = {"ShapeArrow0", "ShapeArrow1", "ShapeArrow2", "ShapeArrow3"};
    }
    t.groups.push_back({TR("Text##ctx_shape"), icon::AlignGroup, {large({btn("ShapeTextAlignMenu", K::Menu)})}, ""});
    t.groups.back().blocks[0].items[0].menu = {"ShapeTextLeft", "ShapeTextCenter", "ShapeTextRight", "-", "ShapeTextTop", "ShapeTextMiddle", "ShapeTextBottom"};
    arrange(t);
    g_tabs.push_back(std::move(t));
  }
  {
    Tab t;
    t.label = TR("Picture Format");
    t.visible = &Context().picture;
    ContextState& cx = Context();
    t.groups.push_back({TR("Adjust"), icon::Picture, {large({btn("ResetPicture"), btn("ChangePicture")})}, ""});
    t.groups.push_back({TR("Picture Styles"), icon::FillColor, {rows({{labeled(color("PictureBorder", &cx.picture_line)), btn("ShapeNoOutline")}})}, "FormatShapePane"});
    arrange(t);
    t.groups.push_back({TR("Crop##ctx_pic"), icon::PaperSize, {large({btn("CropPicture")})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    // Header & Footer (contextual): a header or footer being typed in Page Layout view (Excel's Header & Footer tab)
    Tab t;
    t.label = TR("Header & Footer");
    t.visible = &Context().header_footer;
    ContextState& cx = Context();
    t.groups.push_back({TR("Header & Footer##ctx_hf"), icon::Margins, {large({btn("HfHeaderMenu", K::Menu), btn("HfFooterMenu", K::Menu)})}, ""});
    t.groups.push_back({TR("Header & Footer Elements"), icon::Margins,
                        {large({btn("HfPageNumber"), btn("HfNumberOfPages"), btn("HfCurrentDate"), btn("HfCurrentTime"), btn("HfFilePath"), btn("HfFileName"),
                                btn("HfSheetName"), btn("HfPicture"), btn("HfFormatPicture")})},
                        ""});
    t.groups.push_back({TR("Navigation##ctx_hf"), icon::Margins, {large({btn("HfGoToHeader"), btn("HfGoToFooter")})}, ""});
    t.groups.push_back({TR("Options##ctx_hf"), icon::DisplayGroup,
                        {stack({check("HfDifferentFirst", &cx.hf_first), check("HfDifferentOddEven", &cx.hf_odd_even)}),
                         stack({check("HfScaleWithDoc", &cx.hf_scale), check("HfAlignMargins", &cx.hf_align)})},
                        ""});
    g_tabs.push_back(std::move(t));
  }
  {
    // Slicer (contextual): a selected slicer or timeline
    Tab t;
    t.label = TR("Slicer");
    t.visible = &Context().slicer;
    auto box = [](const char* cmd, int which, float w) {
      Item it;
      it.kind = K::Custom;
      it.cmd = cmd;
      it.combo_width = w;
      it.draw = [which](const ImRect& r) {
        if (Context().slicer_box) Context().slicer_box(which, r);
      };
      return it;
    };
    t.groups.push_back({TR("Slicer##ctx_slicer"), icon::Filter, {stack({box("SlicerCaption", 0, 200)}), large({btn("SlicerSettings"), btn("SlicerReportConnections")})}, "SlicerSettings"});
    t.groups.push_back({TR("Buttons##ctx_slicer"), icon::Rows, {stack({box("SlicerColumns", 1, 130)})}, ""});
    t.groups.push_back({TR("Size##ctx_slicer"), icon::Margins, {stack({box("SlicerHeight", 2, 130), box("SlicerWidth", 3, 130)})}, ""});
    g_tabs.push_back(std::move(t));
  }
  {
    // Timeline (contextual): a selected timeline; its caption, settings, connections and Show options
    Tab t;
    t.label = TR("Timeline");
    t.visible = &Context().timeline;
    ContextState& cx = Context();
    Item cap;
    cap.kind = K::Custom;
    cap.cmd = "SlicerCaption";
    cap.combo_width = 200;
    cap.draw = [](const ImRect& r) {
      if (Context().slicer_box) Context().slicer_box(0, r);
    };
    t.groups.push_back({TR("Timeline##ctx_timeline"), icon::Timeline, {stack({cap}), large({btn("SlicerSettings"), btn("SlicerReportConnections")})}, "SlicerSettings"});
    t.groups.push_back({TR("Show##ctx_timeline"), icon::Panel,
                        {stack({check("TimelineShowHeader", &cx.tl_header), check("TimelineShowSelectionLabel", &cx.tl_label)}),
                         stack({check("TimelineShowScrollbar", &cx.tl_scroll), check("TimelineShowTimeLevel", &cx.tl_level)})},
                        ""});
    g_tabs.push_back(std::move(t));
  }
  g_wcache = {};
  g_scache = {};
}

}  // namespace yc::ui::ribbon
