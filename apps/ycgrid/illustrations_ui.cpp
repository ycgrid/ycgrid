// SPDX-License-Identifier: Apache-2.0
// illustrations_ui.cpp - Insert -> Pictures / Shapes / Text Box / Sparklines, and the Sparkline tab
#include "app_state.h"
#include "core/i18n.h"
#include "engine/shape_geometry.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "yc_ui_items.h"

#include <shobjidl.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <cstring>
#include <fstream>
#include <iterator>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

constexpr double kPx = 4.0 / 3.0;  // points -> sheet pixels

struct IllUi {
  int spark_type = 0;
  int spark_mode = 0;  // the sparkline range dialog: 0 Create Sparklines, 1 Edit Sparklines (group), 2 Edit Sparkline Data (one), 3 Date Range
  uint32_t spark_group = 0, spark_r = 0, spark_c = 0;
  bool hidden_request = false, axis_request = false;
  int hidden_blanks = 1;
  bool hidden_show = false;
  bool axis_max = false;  // Sparkline Vertical Axis Settings: the maximum (else the minimum)
  char axis_value[32] = "0.0";
  char data[256] = "", location[256] = "";
  int focus = -1;  // the box the caret starts in: 0 Data Range, 1 Location Range
  // Insert > Symbol: the window, the subset shown, the character picked (its code typed in Character code)
  bool symbol_open = false;
  int symbol_subset = 0;
  uint32_t symbol_pick = 0x25B2;
  char symbol_code[16] = "25B2";
  std::vector<uint32_t> symbol_recent;
  bool symbol_loaded = false;
  int symbol_tab = 0;           // 0 Symbols, 1 Special Characters
  std::string symbol_font;      // "" = (normal text)
  int special_pick = 0;
  bool symbol_inserted = false;  // Cancel reads Close once something was inserted (Excel)
  int symbol_from = 0;           // Character code from: 0 Unicode (hex), 1 ASCII (decimal), 2 ASCII (hex)
  bool shapes_menu_dirty = true;
};

IllUi& I() {
  static IllUi i;
  return i;
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

namespace {
std::wstring PickOpenFile(HWND owner, const char* title, bool pictures);
}  // namespace

std::wstring PickImageFile(HWND owner) { return PickOpenFile(owner, TR("Insert Picture"), true); }
std::wstring PickAnyFile(HWND owner, const char* title) { return PickOpenFile(owner, title, false); }

namespace {
std::wstring PickOpenFile(HWND owner, const char* title, bool pictures) {
  if (std::wstring a; NoFileDialog(owner, title, &a)) return a;
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::wstring result;
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    const COMDLG_FILTERSPEC types[] = {{TRW(L"Pictures"), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff"}, {TRW(L"All files"), L"*.*"}};
    if (pictures) dlg->SetFileTypes(2, types);
    else dlg->SetFileTypes(1, types + 1);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (SUCCEEDED(dlg->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item)) && item) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
          result = p;
          ::CoTaskMemFree(p);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(init)) ::CoUninitialize();
  return result;
}
}  // namespace

namespace {

// the top-left corner of the active cell, in points
void ActivePoint(AppState& s, double* left, double* top) {
  const model::Sheet* sh = s.FocusSheet();
  const auto& sel = s.FocusView().sel;
  *left = sh ? static_cast<double>(sh->cols->Offset(sel.act_c)) / kPx : 0;
  *top = sh ? static_cast<double>(sh->rows->Offset(sel.act_r)) / kPx : 0;
}

void InsertShape(AppState& s, model::ShapeKind kind) {
  model::ShapeObj sh;
  sh.kind = kind;
  ActivePoint(s, &sh.left, &sh.top);
  if (kind == model::ShapeKind::TextBox) {
    sh.fill_color = 0xFFFFFFFF;
    sh.line_color = 0xFF000000;
    sh.font_color = 0xFF000000;
    sh.halign = 0;
    sh.valign = 0;
  }
  if (kind == model::ShapeKind::Line || kind == model::ShapeKind::Arrow) {
    sh.line_color = 0xFF4472C4;
    sh.line_weight = 1.5f;
    sh.height = 72;
  }
  const uint32_t sheet = s.focus_sheet;
  const uint32_t id = s.engine->Call([&](Document& d) {
    d.Begin(TR("Insert Shape"));
    const uint32_t v = d.AddShape(sheet, sh);
    d.Commit();
    return v;
  });
  SelectInsertedShape(s, sheet, id);
}

const model::SparklineGroup* ActiveSparkline(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh || sh->sparklines->empty()) return nullptr;
  const auto& sel = s.FocusView().sel;
  for (const auto& g : *sh->sparklines) {
    for (const auto& it : g.items) {
      if (it.r == sel.act_r && it.c == sel.act_c) return &g;
    }
  }
  return nullptr;
}

void EditSpark(AppState& s, void (*fn)(model::SparklineGroup&)) {
  const model::SparklineGroup* g = ActiveSparkline(s);
  if (!g) return;
  const uint32_t sheet = s.focus_sheet, id = g->id;
  s.engine->PostUser(TR("Sparkline"), [=](Document& d) { d.UpdateSparklineGroup(sheet, id, fn); });
}

// a change with a value (a color, a weight, a style) to the active cell's sparkline group
void EditSparkWith(AppState& s, std::function<void(model::SparklineGroup&)> fn) {
  const model::SparklineGroup* g = ActiveSparkline(s);
  if (!g) return;
  const uint32_t sheet = s.focus_sheet, id = g->id;
  s.engine->PostUser(TR("Sparkline"), [=](Document& d) { d.UpdateSparklineGroup(sheet, id, fn); });
}

// a theme or standard color from the ribbon's palette, as the ARGB the sparklines keep
uint32_t PickedColor(AppState& s) {
  const uint32_t ref = ui::ribbon::St().menu_color;
  (void)s;
  return 0xFF000000u | (model::ResolveColor(ref) & 0xFFFFFF);
}

// Excel's 36 sparkline styles: six accents, each darker, plain, lighter and with contrasting markers
struct SparkStyle {
  uint32_t series, markers;
};
SparkStyle SparkStyleOf(int k) {
  static const uint32_t kAccent[6] = {0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47};
  auto mix = [](uint32_t c, double f) {  // f < 0 darker, > 0 lighter
    auto ch = [&](int sh) {
      const double v = (c >> sh) & 0xFF;
      return static_cast<uint32_t>(std::clamp(f < 0 ? v * (1 + f) : v + (255 - v) * f, 0.0, 255.0)) << sh;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
  };
  const int row = k / 6, col = k % 6;
  const uint32_t a = kAccent[col], b = kAccent[(col + 1) % 6];
  switch (row) {
    case 0: return {mix(a, -0.5), mix(a, -0.5)};
    case 1: return {mix(a, -0.25), mix(a, -0.25)};
    case 2: return {mix(a, 0), mix(b, 0)};
    case 3: return {mix(a, 0.4), mix(a, -0.25)};
    case 4: return {mix(a, -0.5), mix(b, 0)};
    default: return {0xFF404040, mix(a, 0)};
  }
}

}  // namespace

namespace {

// Insert > Symbol: Unicode subsets as Excel's Symbol dialog lists them
struct Subset {
  const char* name;
  uint32_t first, last;
};
const Subset* Subsets() {
  static const Subset kSubsets[] = {{TR("Basic Latin"), 0x20, 0x7E},          {TR("Latin-1 Supplement"), 0xA0, 0xFF},     {TR("Greek and Coptic"), 0x370, 0x3FF},
                           {TR("General Punctuation"), 0x2010, 0x205E}, {TR("Currency Symbols"), 0x20A0, 0x20C0},  {TR("Letterlike Symbols"), 0x2100, 0x214F},
                           {TR("Arrows"), 0x2190, 0x21FF},               {TR("Mathematical Operators"), 0x2200, 0x22FF}, {TR("Box Drawing"), 0x2500, 0x257F},
                           {TR("Geometric Shapes"), 0x25A0, 0x25FF},     {TR("Miscellaneous Symbols"), 0x2600, 0x26FF}, {TR("Dingbats"), 0x2700, 0x27BF}};
  return kSubsets;
}
constexpr int kSubsetCount = 12;

std::string Utf8Of(uint32_t cp) {
  char buf[8] = {};
  ImTextCharToUtf8(buf, cp);
  return buf;
}

// the Unicode names of the characters the dialog lists (scripts/unicode_names.py)
struct UName {
  uint32_t cp;
  const char* name;
};
const UName kUnicodeNames[] = {
#include "unicode_names.inc"
};

const char* UnicodeName(uint32_t cp) {
  const auto* end = kUnicodeNames + std::size(kUnicodeNames);
  const auto* it = std::lower_bound(kUnicodeNames, end, cp, [](const UName& a, uint32_t v) { return a.cp < v; });
  return it != end && it->cp == cp ? it->name : "";
}

// the Character code box in the base "from:" says (Unicode hex "25B2", ASCII decimal "065", ASCII hex "41")
void SetSymbolCode(uint32_t cp) {
  IllUi& u = I();
  if (u.symbol_from == 1) std::snprintf(u.symbol_code, sizeof(u.symbol_code), "%03u", cp);
  else if (u.symbol_from == 2) std::snprintf(u.symbol_code, sizeof(u.symbol_code), "%02X", cp);
  else std::snprintf(u.symbol_code, sizeof(u.symbol_code), "%04X", cp);  // recursed into itself: a click on a character crashed (chaos 5094)
}

// the character goes where the user types: at the caret of the cell being edited, else into the active cell
void InsertSymbolChar(AppState& s, uint32_t cp, bool remember = true) {
  IllUi& u = I();
  const std::string ch = Utf8Of(cp);
  if (s.edit.active) {
    EditInsertText(s, ch);
  } else if (const model::Sheet* sh = s.FocusSheet()) {
    const auto& v = s.FocusView().sel;
    const uint32_t sheet = sh->id, r = v.act_r, c = v.act_c;
    const std::string font = u.symbol_font;
    s.engine->PostUser(TR("Insert Symbol"), [sheet, r, c, ch, font](Document& d) {
      d.SetInput(sheet, r, c, ch);
      // a symbol from another font (Wingdings, Segoe UI Symbol) brings its font, as in Excel
      if (!font.empty()) {
        const uint32_t fid = d.wb_mut().styles.mut().InternString(font);
        d.ModifyStyle(sheet, core::Rect::Cell(r, c), [fid](model::Style& st) { st.font_name = fid; });
      }
    });
  }
  u.symbol_inserted = true;
  if (!remember) return;
  u.symbol_recent.erase(std::remove(u.symbol_recent.begin(), u.symbol_recent.end(), cp), u.symbol_recent.end());
  u.symbol_recent.insert(u.symbol_recent.begin(), cp);
  if (u.symbol_recent.size() > 20) u.symbol_recent.resize(20);
  // kept for the next time (settings.json), as Excel's Recently used symbols
  auto& rs = Config().recent_symbols;
  rs.clear();
  for (uint32_t x : u.symbol_recent) rs.push_back(Utf8Of(x));
  SaveSettings();
}

// Excel's Special Characters tab: the character, its name and its shortcut key
struct Special {
  uint32_t cp;
  const char* name;
  const char* keys;
};
const Special* Specials() {
  static const Special k[] = {{0x2014, TR("Em Dash"), "Alt+Ctrl+Num -"},     {0x2013, TR("En Dash"), "Ctrl+Num -"},
                              {0x2011, TR("Nonbreaking Hyphen"), "Ctrl+_"}, {0x00AD, TR("Optional Hyphen"), "Ctrl+-"},
                              {0x2003, TR("Em Space"), ""},                  {0x2002, TR("En Space"), ""},
                              {0x2005, TR("1/4 Em Space"), ""},              {0x00A0, TR("Nonbreaking Space"), "Ctrl+Shift+Space"},
                              {0x00A9, TR("Copyright"), "Alt+Ctrl+C"},       {0x00AE, TR("Registered"), "Alt+Ctrl+R"},
                              {0x2122, TR("Trademark"), "Alt+Ctrl+T"},       {0x00A7, TR("Section"), ""},
                              {0x00B6, TR("Paragraph"), ""},                 {0x2026, TR("Ellipsis"), "Alt+Ctrl+."},
                              {0x2018, TR("Single Opening Quote"), "Ctrl+`,`"}, {0x2019, TR("Single Closing Quote"), "Ctrl+','"},
                              {0x201C, TR("Double Opening Quote"), "Ctrl+`,\""}, {0x201D, TR("Double Closing Quote"), "Ctrl+',\""},
                              {0x200B, TR("No-Width Optional Break"), ""},   {0x200D, TR("No-Width Non Break"), ""}};
  return k;
}
constexpr int kSpecialCount = 20;

void DrawSymbolWindow(AppState& s) {
  IllUi& u = I();
  if (!u.symbol_open) return;
  if (!u.symbol_loaded) {
    u.symbol_loaded = true;
    for (const std::string& ch : Config().recent_symbols) {
      unsigned int cp = 0;
      if (!ch.empty() && ImTextCharFromUtf8(&cp, ch.c_str(), nullptr) > 0 && cp) u.symbol_recent.push_back(cp);
    }
  }
  ImGui::SetNextWindowSize(ImVec2(S(500), S(470)), ImGuiCond_FirstUseEver);
  PlaceNewWindow(WindowPlace::Right);
  if (!ImGui::Begin(TR("Symbol###yc_symbol"), &u.symbol_open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  // Escape is its Cancel / Close button, as in Excel's Symbol dialog
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) u.symbol_open = false;
  if (BeginDlgTabBar("##symtabs")) {
    if (ImGui::BeginTabItem(TR("Symbols"))) {
      u.symbol_tab = 0;
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Special Characters"))) {
      u.symbol_tab = 1;
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  if (u.symbol_tab == 1) {
    // Special Characters: Character / Shortcut key, a double-click or Insert puts it in
    if (ImGui::BeginTable("##special", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders, ImVec2(-1, S(330)))) {
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, S(30));
      ImGui::TableSetupColumn(TR("Character"));
      ImGui::TableSetupColumn(TR("Shortcut key:"));
      ImGui::TableHeadersRow();
      for (int k = 0; k < kSpecialCount; ++k) {
        const Special& sp = Specials()[k];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(k);
        if (ImGui::Selectable(Utf8Of(sp.cp).c_str(), u.special_pick == k, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
          u.special_pick = k;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) InsertSymbolChar(s, sp.cp, false);
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sp.name);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sp.keys);
      }
      ImGui::EndTable();
    }
    if (ImGui::Button(TR("Insert"), ImVec2(S(90), 0))) InsertSymbolChar(s, Specials()[std::clamp(u.special_pick, 0, kSpecialCount - 1)].cp, false);
    ImGui::SameLine();
    if (ImGui::Button(u.symbol_inserted ? TR("Close") : TR("Cancel"), ImVec2(S(90), 0))) u.symbol_open = false;
    ImGui::End();
    return;
  }
  // Font: the grid shows the characters of the font picked ((normal text): the cell font)
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Font:"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(170));
  if (ui::BeginCombo("##symfont", u.symbol_font.empty() ? TR("(normal text)") : u.symbol_font.c_str())) {
    if (ImGui::Selectable(TR("(normal text)"), u.symbol_font.empty())) u.symbol_font.clear();
    for (const std::string& f : ui::SystemFontFamilies()) {
      if (ImGui::Selectable(f.c_str(), u.symbol_font == f)) u.symbol_font = f;
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(190));
  if (ui::BeginCombo(TR("Subset"), Subsets()[u.symbol_subset].name)) {
    for (int k = 0; k < kSubsetCount; ++k) {
      if (ImGui::Selectable(Subsets()[k].name, k == u.symbol_subset)) u.symbol_subset = k;
    }
    ImGui::EndCombo();
  }
  const Subset& sub = Subsets()[u.symbol_subset];
  ImFont* font = u.symbol_font.empty() ? ImGui::GetFont() : ui::CellFont(u.symbol_font, false, false);
  const float cell = S(26);
  ImGui::BeginChild("##symbols", ImVec2(-1, S(230)), ImGuiChildFlags_Borders);
  const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
  int col = 0;
  // ASCII (decimal / hex) lists the first 256 characters only, as Excel
  for (uint32_t cp = sub.first; cp <= sub.last && (u.symbol_from == 0 || cp <= 0xFF); ++cp) {
    if (!font->IsGlyphInFont(static_cast<ImWchar>(cp))) continue;
    if (col) ImGui::SameLine(0, 0);
    const std::string ch = Utf8Of(cp);
    ImGui::PushID(static_cast<int>(cp));
    ImGui::PushFont(font, 0.0f);
    const bool hit = ImGui::Selectable(ch.c_str(), u.symbol_pick == cp, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cell - S(2), cell - S(2)));
    ImGui::PopFont();
    if (hit) {
      u.symbol_pick = cp;
      SetSymbolCode(cp);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) InsertSymbolChar(s, cp);
    }
    ImGui::PopID();
    col = (col + 1) % per_row;
  }
  ImGui::EndChild();
  if (!u.symbol_recent.empty()) {
    ImGui::TextUnformatted(TR("Recently used symbols:"));
    for (size_t k = 0; k < u.symbol_recent.size(); ++k) {
      if (k) ImGui::SameLine(0, 0);
      ImGui::PushID(static_cast<int>(10000 + k));
      if (ImGui::Selectable(Utf8Of(u.symbol_recent[k]).c_str(), false, 0, ImVec2(cell - S(2), cell - S(2)))) {
        u.symbol_pick = u.symbol_recent[k];
        SetSymbolCode(u.symbol_pick);
      }
      ImGui::PopID();
    }
  }
  // Unicode name, Character code and from:, as Excel's Symbol dialog
  ImGui::TextUnformatted(TR("Unicode name:"));
  ImGui::SameLine();
  ImGui::TextUnformatted(UnicodeName(u.symbol_pick));
  ui::NoteUiItem((std::string("unicode name:") + UnicodeName(u.symbol_pick)).c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(TR("Character code:"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(90));
  if (ImGui::InputText("##code", u.symbol_code, sizeof(u.symbol_code), u.symbol_from == 1 ? ImGuiInputTextFlags_CharsDecimal : ImGuiInputTextFlags_CharsHexadecimal)) {
    const unsigned long v = std::strtoul(u.symbol_code, nullptr, u.symbol_from == 1 ? 10 : 16);
    if (v >= 0x20 && v <= (u.symbol_from == 0 ? 0x10FFFFul : 0xFFul)) u.symbol_pick = static_cast<uint32_t>(v);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(TR("from:"));
  ImGui::SameLine();
  static const char* kFrom[] = {TR("Unicode (hex)"), TR("ASCII (decimal)"), TR("ASCII (hex)")};
  ImGui::SetNextItemWidth(S(150));
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ui::NoteUiItem("##symfrom", ImRect(at, at + ImVec2(S(150), ImGui::GetFrameHeight())));
  }
  if (ui::Combo("##symfrom", &u.symbol_from, kFrom, 3)) {
    if (u.symbol_from != 0 && u.symbol_pick > 0xFF) u.symbol_pick = 0x41;
    SetSymbolCode(u.symbol_pick);
  }
  if (ImGui::Button(TR("Insert"), ImVec2(S(90), 0))) InsertSymbolChar(s, u.symbol_pick);
  ImGui::SameLine();
  // Excel's Cancel becomes Close once a symbol was inserted
  if (ImGui::Button(u.symbol_inserted ? TR("Close") : TR("Cancel"), ImVec2(S(90), 0))) u.symbol_open = false;
  ImGui::End();
}

// ---- Insert > Shapes: Excel's gallery (Recently Used Shapes, Lines, Rectangles, Basic Shapes, Block Arrows,
// Equation Shapes, Flowchart, Stars and Banners, Callouts); a pick is drawn on the sheet ----
std::string ShapeCmdId(int code) {
  switch (code) {
    case 1: return "InsertShapeRectangle";
    case 5: return "InsertShapeRoundedRectangle";
    case 9: return "InsertShapeOval";
    case 7: return "InsertShapeTriangle";
    case 33: return "InsertShapeRightArrow";
    case 0: return "InsertTextBox";
    case engine::kLine: return "InsertShapeLine";
    case engine::kLineArrow: return "InsertShapeArrow";
    case engine::kLineDoubleArrow: return "InsertShapeDoubleArrow";
    case engine::kElbow: return "InsertShapeElbow";
    case engine::kElbowArrow: return "InsertShapeElbowArrow";
    case engine::kElbowDoubleArrow: return "InsertShapeElbowDoubleArrow";
    case engine::kCurved: return "InsertShapeCurved";
    case engine::kCurvedArrow: return "InsertShapeCurvedArrow";
    case engine::kCurvedDoubleArrow: return "InsertShapeCurvedDoubleArrow";
    case engine::kCurve: return "InsertShapeCurve";
    case engine::kFreeform: return "InsertShapeFreeform";
    case engine::kScribble: return "InsertShapeScribble";
    default: return "InsertShape" + std::to_string(code);
  }
}

std::map<std::string, int>& ShapeCodes() {
  static std::map<std::string, int> m;
  return m;
}

// the gallery's tiles: each shape's outline in a square
float ShapeTile(const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) {
  auto it = ShapeCodes().find(cmd);
  if (it == ShapeCodes().end()) return 0.0f;
  if (!dl) return h;
  const int code = it->second;
  const ImU32 col = IM_COL32(0x40, 0x40, 0x40, 255);
  const float m = std::max(2.0f, h * 0.12f), w = h - 2 * m;
  const ImVec2 o(at.x + m, at.y + m);
  auto line = [&](std::vector<ImVec2> v, bool head_end, bool head_begin) {
    dl->AddPolyline(v.data(), static_cast<int>(v.size()), col, 1.2f, ImDrawFlags_None);
    auto head = [&](ImVec2 from, ImVec2 tip) {
      const float l = std::max(1.0f, std::hypot(tip.x - from.x, tip.y - from.y));
      const ImVec2 u2((tip.x - from.x) / l, (tip.y - from.y) / l), n(-u2.y, u2.x);
      dl->AddTriangleFilled(tip, tip - u2 * 5 + n * 2.5f, tip - u2 * 5 - n * 2.5f, col);
    };
    if (head_end) head(v[v.size() - 2], v.back());
    if (head_begin) head(v[1], v.front());
  };
  if (code < 0) {
    const ImVec2 a = o, b(o.x + w, o.y + w);
    using namespace engine;
    const bool end = code == kLineArrow || code == kLineDoubleArrow || code == kElbowArrow || code == kElbowDoubleArrow || code == kCurvedArrow ||
                     code == kCurvedDoubleArrow;
    const bool beg = code == kLineDoubleArrow || code == kElbowDoubleArrow || code == kCurvedDoubleArrow;
    const int conn = code <= kElbow && code >= kElbowDoubleArrow ? 2 : code <= kCurved && code >= kCurvedDoubleArrow ? 3 : 1;
    if (code >= kCurvedDoubleArrow) {
      std::vector<ImVec2> v;
      for (const auto& q : ConnectorPath(conn, {a.x, b.y}, {b.x, a.y})) v.push_back(ImVec2(static_cast<float>(q.x), static_cast<float>(q.y)));
      line(v, end, beg);
    } else if (code == kCurve) {
      std::vector<ImVec2> v;
      for (const auto& q : FreeformPath({0, 0.9, 0.3, 0.1, 0.65, 0.9, 1, 0.15}, true, w, w)) v.push_back(o + ImVec2(static_cast<float>(q.x), static_cast<float>(q.y)));
      line(v, false, false);
    } else if (code == kFreeform) {
      std::vector<ImVec2> v = {o + ImVec2(0.1f * w, w), o + ImVec2(0.2f * w, 0.3f * w), o + ImVec2(0.55f * w, 0.55f * w), o + ImVec2(0.7f * w, 0), o + ImVec2(w, 0.8f * w)};
      line(v, false, false);
    } else {
      std::vector<ImVec2> v;
      for (int i = 0; i <= 16; ++i) v.push_back(o + ImVec2(w * i / 16.0f, w * 0.5f + w * 0.35f * std::sin(i * 0.9f)));
      line(v, false, false);
    }
    return h;
  }
  if (code == 0) {  // Text Box
    dl->AddRect(o, o + ImVec2(w, w), col, 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k) dl->AddLine(o + ImVec2(w * 0.2f, w * (0.3f + 0.2f * k)), o + ImVec2(w * (k == 2 ? 0.55f : 0.8f), w * (0.3f + 0.2f * k)), col, 1.0f);
    return h;
  }
  engine::ShapeGeom g;
  // wide shapes (arrows across, waves, scrolls) look best a little wider than tall
  if (!engine::AutoShapeGeometry(code, w, w * 0.8f, &g)) return h;
  const ImVec2 oo(o.x, o.y + w * 0.1f);
  for (const auto& st : g.strokes) {
    std::vector<ImVec2> v;
    for (const auto& q : st.pts) {
      // callout leaders reach outside the box: kept inside the tile
      v.push_back(oo + ImVec2(std::clamp(static_cast<float>(q.x), -m, w + m), std::clamp(static_cast<float>(q.y), -m, w + m * 0.5f)));
    }
    dl->AddPolyline(v.data(), static_cast<int>(v.size()), col, 1.0f, st.closed ? ImDrawFlags_Closed : ImDrawFlags_None);
  }
  return h;
}

// Sparkline > Style's tiles: a small line in the style's colors with its markers
float SparkStyleTile(const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) {
  if (cmd.rfind("SparkStyle", 0) != 0 || cmd.size() <= 10 || !std::isdigit(static_cast<unsigned char>(cmd[10]))) return 0.0f;
  if (!dl) return h;
  const SparkStyle st = SparkStyleOf(std::atoi(cmd.c_str() + 10));
  auto col = [](uint32_t c) { return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255); };
  static const float kV[] = {0.6f, 0.3f, 0.5f, 0.15f, 0.7f, 0.35f, 0.8f};
  ImVec2 pts[7];
  for (int i = 0; i < 7; ++i) pts[i] = ImVec2(at.x + h * (0.08f + 0.84f * i / 6.0f), at.y + h * (0.15f + 0.7f * kV[i]));
  dl->AddRect(at, at + ImVec2(h, h), IM_COL32(0xD0, 0xD0, 0xD0, 255));
  dl->AddPolyline(pts, 7, col(st.series), std::max(1.0f, h * 0.04f), ImDrawFlags_None);
  dl->AddCircleFilled(pts[3], std::max(1.5f, h * 0.06f), col(st.markers));
  dl->AddCircleFilled(pts[6], std::max(1.5f, h * 0.06f), col(st.markers));
  return h;
}

// Insert > Shapes' drop-down: Recently Used Shapes first, then Excel's groups
void UpdateShapesMenu() {
  IllUi& u = I();
  static std::vector<std::string> menu;
  if (u.shapes_menu_dirty) {
    u.shapes_menu_dirty = false;
    menu.clear();
    std::vector<std::string> recent;
    for (const std::string& id : Config().recent_shapes) {
      if (ShapeCodes().count(id)) recent.push_back(id);
    }
    if (!recent.empty()) {
      menu.push_back(std::string("=") + TR("Recently Used Shapes"));
      menu.insert(menu.end(), recent.begin(), recent.end());
    }
    const char* group = nullptr;
    for (const auto& e : engine::ShapeGallery()) {
      if (!group || std::strcmp(group, e.group) != 0) {
        group = e.group;
        menu.push_back(std::string("=") + group);
      }
      menu.push_back(ShapeCmdId(e.type));
    }
  }
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "InsertShapesMenu" && it.menu != menu) it.menu = menu;
        }
      }
    }
  }
}

}  // namespace

// a picture file chosen by the user (Change Picture): its bytes, when it can be read
bool PickPictureFile(AppState& s, std::string* bytes) {
  const std::wstring path = PickImageFile(s.hwnd);
  if (path.empty()) return false;
  std::ifstream f(path, std::ios::binary);
  bytes->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int w = 0, h = 0;
  if (bytes->empty() || !ImageSize(*bytes, &w, &h)) {
    s.ShowError("YCGrid", TR("This picture can't be read."));
    return false;
  }
  return true;
}

void UpdateSparklineContext(AppState& s) {
  UpdateShapesMenu();
  UpdateWordArtMenu();
  auto& cx = ui::ribbon::Context();
  const model::SparklineGroup* g = ActiveSparkline(s);
  cx.sparkline = g != nullptr;
  if (!g) return;
  // the Axis menu's choices show which is on
  auto set = [](const char* id, bool v) {
    if (auto* c = ui::Commands().Find(id)) c->toggle = true, c->pressed = v;
  };
  set("SparkAxisGeneral", g->date_range.empty());
  set("SparkAxisDate", !g->date_range.empty());
  set("SparkAxis", g->axis);
  set("SparkRightToLeft", g->right_to_left);
  set("SparkMinAuto", g->min_scale == 2);
  set("SparkMinSame", g->min_scale == 1);
  set("SparkMinCustom", g->min_scale == 3);
  set("SparkMaxAuto", g->max_scale == 2);
  set("SparkMaxSame", g->max_scale == 1);
  set("SparkMaxCustom", g->max_scale == 3);
  cx.sp_high = g->high;
  cx.sp_low = g->low;
  cx.sp_negative = g->negative;
  cx.sp_first = g->first;
  cx.sp_last = g->last;
  cx.sp_markers = g->markers;
  cx.sp_axis = g->axis;
}

void InsertPictureBytes(AppState& s, std::string bytes) {
    int w = 0, h = 0;
    if (bytes.empty() || !ImageSize(bytes, &w, &h)) {
      s.ShowError("YCGrid", TR("This picture can't be read."));
      return;
    }
    model::ShapeObj sh;
    sh.kind = model::ShapeKind::Picture;
    ActivePoint(s, &sh.left, &sh.top);
    // its own size in points (pixels x 3/4), no wider than 480 pt
    sh.width = w * 0.75;
    sh.height = h * 0.75;
    if (sh.width > 480) {
      sh.height *= 480 / sh.width;
      sh.width = 480;
    }
    sh.image = std::move(bytes);
    const uint32_t sheet = s.focus_sheet;
    const uint32_t id = s.engine->Call([&](Document& d) {
      d.Begin(TR("Insert Picture"));
      const uint32_t v = d.AddShape(sheet, sh);
      d.Commit();
      return v;
    });
    SelectInsertedShape(s, sheet, id);
}

void RegisterIllustrationCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon = nullptr, const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  R.Add(make("InsertPicture", TR("Pictures"), ui::icon::Picture, TR("Insert a picture from a file (PNG, JPEG, BMP, GIF)."))).execute = [&s](ui::Command&) {
    const std::wstring path = PickImageFile(s.hwnd);
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    InsertPictureBytes(s, std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
  };
  R.Add(make("InsertShapesMenu", TR("Shapes"), ui::icon::Shapes, TR("Insert a shape: rectangle, oval, line, arrow..."))).execute = [](ui::Command&) {};
  // Excel's Shapes gallery: picking one waits for a drag (or a click) on the sheet
  for (const auto& e : engine::ShapeGallery()) {
    const int code = e.type;
    const std::string id = ShapeCmdId(code);
    ShapeCodes()[id] = code;
    if (code == 0) continue;  // the Text Box command below
    const char* label = code == engine::kLine ? TR("Line##ctx_shape") : e.label;
    R.Add(make(id.c_str(), label)).execute = [&s, code, id](ui::Command&) {
      auto& rs = Config().recent_shapes;
      rs.erase(std::remove(rs.begin(), rs.end(), id), rs.end());
      rs.insert(rs.begin(), id);
      if (rs.size() > 24) rs.resize(24);
      SaveSettings();
      I().shapes_menu_dirty = true;
      ArmShapeDrawing(s, code);
    };
  }
  R.Add(make("InsertTextBox", TR("Text\nBox"), ui::icon::TextBox, TR("Draw a text box anywhere: drag on the sheet (a click makes one line), then type."))).execute =
      [&s](ui::Command&) { ArmShapeDrawing(s, 0); };
  {
    auto& st = ui::ribbon::St();
    st.menu_picture = [prev = st.menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
      float w = ShapeTile(cmd, dl, at, h);
      if (w <= 0) w = SparkStyleTile(cmd, dl, at, h);
      return w > 0 ? w : prev ? prev(cmd, dl, at, h) : 0.0f;
    };
  }
  // Insert > Header & Footer: Page Layout view, the caret in the header's center section (Excel)
  R.Add(make("InsertHeaderFooter", TR("Header\n& Footer"), ui::icon::HeaderFooter, TR("Edit the header or footer of your document. The header and footer show at the top and bottom of every printed page."))).execute =
      [&s](ui::Command&) { StartHeaderFooterEditing(s); };
  RegisterInsertGallery(s);
  R.Add(make("InsertSymbol", TR("Symbol"), ui::icon::Symbol, TR("Insert characters that aren't on your keyboard: arrows, math, currency, box drawing..."))).execute =
      [](ui::Command&) { I().symbol_open = !I().symbol_open; };
  // sparklines
  auto spark = [&s](int type) {
    return [&s, type](ui::Command&) {
      IllUi& u = I();
      u.spark_type = type;
      const Rect r = SelRect(s);
      // like Excel: blank cells selected are where the sparklines go (Location Range, absolute) and the caret waits
      // in Data Range; data selected fills Data Range and the caret waits in Location Range
      bool blank = true;
      if (const model::Sheet* sh = s.FocusSheet()) {
        uint64_t seen = 0;
        for (uint32_t i = r.r0; blank && i <= r.r1 && seen < 100000; ++i) {
          for (uint32_t j = r.c0; blank && j <= r.c1 && seen < 100000; ++j, ++seen) blank = sh->GetValue(i, j).IsEmpty();
        }
      }
      const std::string a1 = core::CellA1(r.r0, r.c0, blank, blank) + (r.IsCell() ? "" : ":" + core::CellA1(r.r1, r.c1, blank, blank));
      std::snprintf(blank ? u.location : u.data, sizeof(u.data), "%s", a1.c_str());
      (blank ? u.data : u.location)[0] = 0;
      u.focus = blank ? 0 : 1;
      u.spark_mode = 0;
      s.OpenDialog(Dialog::CreateSparklines);
    };
  };
  R.Add(make("InsertSparkLine", TR("Line##ctx_spark"), ui::icon::SparkLine, TR("Insert line sparklines: a tiny chart in a cell."))).execute = spark(0);
  R.Add(make("InsertSparkColumn", TR("Column##ctx_spark"), ui::icon::SparkColumn, TR("Insert column sparklines."))).execute = spark(1);
  R.Add(make("InsertSparkWinLoss", TR("Win/\nLoss"), ui::icon::SparkWinLoss, TR("Insert win / loss sparklines."))).execute = spark(2);
  // Sparkline tab
  R.Add(make("SparkTypeLine", TR("Line##ctx_spark"), ui::icon::SparkLine)).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.type = 0; }); };
  R.Add(make("SparkTypeColumn", TR("Column##ctx_spark"), ui::icon::SparkColumn)).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.type = 1; }); };
  R.Add(make("SparkTypeWinLoss", TR("Win/\nLoss"), ui::icon::SparkWinLoss)).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.type = 2; }); };
  R.Add(make("SparkHigh", TR("High Point"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.high = !g.high; }); };
  R.Add(make("SparkLow", TR("Low Point"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.low = !g.low; }); };
  R.Add(make("SparkNegative", TR("Negative Points"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.negative = !g.negative; }); };
  R.Add(make("SparkFirst", TR("First Point"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.first = !g.first; }); };
  R.Add(make("SparkLast", TR("Last Point"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.last = !g.last; }); };
  R.Add(make("SparkMarkers", TR("Markers"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.markers = !g.markers; }); };
  R.Add(make("SparkAxis", TR("Show Axis"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.axis = !g.axis; }); };
  // ---- Sparkline > Edit Data ----
  auto spark_dialog = [&s](int mode) {
    const model::SparklineGroup* g = ActiveSparkline(s);
    if (!g) return;
    IllUi& u = I();
    u.spark_mode = mode;
    u.spark_group = g->id;
    u.spark_r = s.FocusView().sel.act_r;
    u.spark_c = s.FocusView().sel.act_c;
    u.data[0] = u.location[0] = 0;
    uint32_t r0 = UINT32_MAX, c0 = UINT32_MAX, r1 = 0, c1 = 0, sr0 = UINT32_MAX, sc0 = UINT32_MAX, sr1 = 0, sc1 = 0;
    std::string src_sheet;
    for (const auto& it : g->items) {
      const bool mine = mode != 2 || (it.r == u.spark_r && it.c == u.spark_c);
      if (!mine) continue;
      r0 = std::min(r0, it.r), c0 = std::min(c0, it.c), r1 = std::max(r1, it.r), c1 = std::max(c1, it.c);
      sr0 = std::min(sr0, it.src.r0), sc0 = std::min(sc0, it.src.c0), sr1 = std::max(sr1, it.src.r1), sc1 = std::max(sc1, it.src.c1);
      if (const model::Sheet* ss = s.snap->Sheet(it.src_sheet); ss && it.src_sheet != s.focus_sheet) src_sheet = ss->name;
    }
    if (r0 == UINT32_MAX) return;
    const std::string data = (src_sheet.empty() ? std::string() : "'" + src_sheet + "'!") + core::RectA1(Rect{sr0, sc0, sr1, sc1});
    std::snprintf(u.location, sizeof(u.location), "%s", core::RectA1(Rect{r0, c0, r1, c1}).c_str());
    std::snprintf(u.data, sizeof(u.data), "%s", mode == 3 ? g->date_range.c_str() : data.c_str());
    u.focus = 0;
    s.OpenDialog(Dialog::CreateSparklines);
  };
  R.Add(make("SparkEditGroup", TR("Edit\nData"), ui::icon::Edit, TR("Edit the location and data source of the sparkline group."))).execute =
      [spark_dialog](ui::Command&) { spark_dialog(1); };
  R.Add(make("SparkEditSingle", TR("Edit Single Sparkline's Data..."))).execute = [spark_dialog](ui::Command&) { spark_dialog(2); };
  R.Add(make("SparkAxisDate", TR("Date Axis Type..."))).execute = [spark_dialog](ui::Command&) { spark_dialog(3); };
  R.Add(make("SparkHiddenEmpty", TR("Hidden & Empty Cells..."))).execute = [&s](ui::Command&) {
    const model::SparklineGroup* g = ActiveSparkline(s);
    if (!g) return;
    I().hidden_blanks = g->display_blanks;
    I().hidden_show = g->show_hidden;
    I().spark_group = g->id;
    I().hidden_request = true;
  };
  R.Add(make("SparkSwitchRowCol", TR("Switch Row/Column"))).execute = [&s](ui::Command&) {
    // the group's data read by columns instead of rows (or back), when the data is as wide as it is tall
    const model::SparklineGroup* g = ActiveSparkline(s);
    if (!g || g->items.empty()) return;
    Rect all = g->items[0].src;
    bool rows = true, cols = true;
    for (const auto& it : g->items) {
      all = all.Union(it.src);
      rows = rows && it.src.r0 == it.src.r1;
      cols = cols && it.src.c0 == it.src.c1;
    }
    const uint32_t n = static_cast<uint32_t>(g->items.size());
    const bool by_rows = rows && !cols;
    if (all.Rows() != n || all.Cols() != n) {
      s.ShowMessage("YCGrid", TR("The data can't be switched: it must have as many rows as columns as there are sparklines."));
      return;
    }
    std::vector<model::Sparkline> items = g->items;
    for (uint32_t k = 0; k < n; ++k) {
      items[k].src = by_rows ? Rect{all.r0, all.c0 + k, all.r1, all.c0 + k} : Rect{all.r0 + k, all.c0, all.r0 + k, all.c1};
    }
    const uint32_t sheet = s.focus_sheet, id = g->id;
    s.engine->PostUser(TR("Switch Row/Column"), [=](Document& d) { d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& x) { x.items = items; }); });
  };
  // ---- Style ----
  R.Add(make("SparkStylesMenu", TR("Style##ctx_spark"), ui::icon::Brush, TR("Choose a look for the sparklines."))).execute = [](ui::Command&) {};
  for (int k = 0; k < 36; ++k) {
    const std::string id = "SparkStyle" + std::to_string(k);
    R.Add(make(id.c_str(), (std::string(TR("Sparkline Style ")) + std::to_string(k + 1)).c_str())).execute = [&s, k](ui::Command&) {
      const SparkStyle st = SparkStyleOf(k);
      EditSparkWith(s, [st](model::SparklineGroup& g) {
        g.color = st.series;
        g.marker_color = g.high_color = g.low_color = g.first_color = g.last_color = g.negative_color = st.markers;
      });
    };
  }
  R.Add(make("SparkColorMenu", TR("Sparkline Color"), ui::icon::FontColor, TR("Choose the color of the sparklines and their line weight."))).execute = [](ui::Command&) {};
  R.Add(make("SparkColor", TR("Sparkline Color"))).execute = [&s](ui::Command&) {
    const uint32_t c = PickedColor(s);
    EditSparkWith(s, [c](model::SparklineGroup& g) { g.color = c; });
  };
  R.Add(make("SparkWeightMenu", TR("Weight"))).execute = [](ui::Command&) {};
  static const float kWeights[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.25f, 3.0f};
  for (int k = 0; k < 7; ++k) {
    char label[16];
    std::snprintf(label, sizeof(label), "%g pt", kWeights[k]);
    const float w = kWeights[k];
    R.Add(make(("SparkWeight" + std::to_string(k)).c_str(), label)).execute = [&s, w](ui::Command&) {
      EditSparkWith(s, [w](model::SparklineGroup& g) { g.line_weight = w; });
    };
  }
  R.Add(make("SparkMarkerColorMenu", TR("Marker Color"), ui::icon::FillColor, TR("Choose the color of the markers and points shown."))).execute = [](ui::Command&) {};
  struct MarkerCmd {
    const char* head;
    const char* label;
    const char* color_cmd;
    int which;
  };
  static const MarkerCmd kMarkers[] = {{"SparkMarkerNegative", TR("Negative Points"), "SparkMarkerColorNegative", 0}, {"SparkMarkerMarkers", TR("Markers"), "SparkMarkerColorMarkers", 1},
                                       {"SparkMarkerHigh", TR("High Point"), "SparkMarkerColorHigh", 2},             {"SparkMarkerLow", TR("Low Point"), "SparkMarkerColorLow", 3},
                                       {"SparkMarkerFirst", TR("First Point"), "SparkMarkerColorFirst", 4},          {"SparkMarkerLast", TR("Last Point"), "SparkMarkerColorLast", 5}};
  for (const MarkerCmd& mc : kMarkers) {
    R.Add(make(mc.head, mc.label)).execute = [](ui::Command&) {};
    const int which = mc.which;
    R.Add(make(mc.color_cmd, mc.label)).execute = [&s, which](ui::Command&) {
      const uint32_t c = PickedColor(s);
      EditSparkWith(s, [c, which](model::SparklineGroup& g) {
        uint32_t* cols[] = {&g.negative_color, &g.marker_color, &g.high_color, &g.low_color, &g.first_color, &g.last_color};
        bool* show[] = {&g.negative, &g.markers, &g.high, &g.low, &g.first, &g.last};
        *cols[which] = c;
        *show[which] = true;  // a color picked shows the points (Excel ticks the box)
      });
    };
  }
  // ---- Group > Axis ----
  R.Add(make("SparkAxisMenu", TR("Axis"), ui::icon::Axis, TR("Change the scale and visibility of the sparkline axes."))).execute = [](ui::Command&) {};
  R.Add(make("SparkAxisGeneral", TR("General Axis Type"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.date_range.clear(); }); };
  R.Add(make("SparkRightToLeft", TR("Plot Data Right-to-Left"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.right_to_left = !g.right_to_left; }); };
  R.Add(make("SparkMinAuto", TR("Automatic for Each Sparkline##min"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.min_scale = 2; }); };
  R.Add(make("SparkMinSame", TR("Same for All Sparklines##min"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.min_scale = 1; }); };
  R.Add(make("SparkMaxAuto", TR("Automatic for Each Sparkline##max"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.max_scale = 2; }); };
  R.Add(make("SparkMaxSame", TR("Same for All Sparklines##max"))).execute = [&s](ui::Command&) { EditSpark(s, [](model::SparklineGroup& g) { g.max_scale = 1; }); };
  auto custom = [&s](bool max) {
    return [&s, max](ui::Command&) {
      const model::SparklineGroup* g = ActiveSparkline(s);
      if (!g) return;
      IllUi& u = I();
      u.axis_max = max;
      u.spark_group = g->id;
      std::snprintf(u.axis_value, sizeof(u.axis_value), "%g", max ? (g->max_scale == 3 ? g->max_value : 0.0) : (g->min_scale == 3 ? g->min_value : 0.0));
      u.axis_request = true;
    };
  };
  R.Add(make("SparkMinCustom", TR("Custom Value...##min"))).execute = custom(false);
  R.Add(make("SparkMaxCustom", TR("Custom Value...##max"))).execute = custom(true);
  // ---- Group / Ungroup / Clear ----
  R.Add(make("SparkGroup", TR("Group##spark"), ui::icon::Group, TR("Group the selected sparklines, so they share their formatting and scaling."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet, r = s.FocusView().sel.act_r, c = s.FocusView().sel.act_c;
    const auto areas = SelAreas(s);
    s.engine->PostUser(TR("Group Sparklines"), [=](Document& d) { d.GroupSparklines(sheet, areas, r, c); });
  };
  R.Add(make("SparkUngroup", TR("Ungroup##spark"), ui::icon::Ungroup, TR("Ungroup the selected sparklines, so each can be formatted on its own."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    s.engine->PostUser(TR("Ungroup Sparklines"), [=](Document& d) { d.UngroupSparklines(sheet, areas); });
  };
  R.Add(make("SparkClearGroups", TR("Clear Selected Sparkline Groups"))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    std::vector<Rect> cells;
    for (const auto& g : *sh->sparklines) {
      bool hit = false;
      for (const auto& it : g.items) {
        for (const Rect& a : areas) hit = hit || a.Contains(it.r, it.c);
      }
      if (hit) {
        for (const auto& it : g.items) cells.push_back(Rect::Cell(it.r, it.c));
      }
    }
    s.engine->PostUser(TR("Clear Sparklines"), [=](Document& d) {
      for (const Rect& a : cells) d.ClearSparklines(sheet, a);
    });
  };
  R.Add(make("SparkClear", TR("Clear Selected Sparklines"), ui::icon::Clear, TR("Clear the selected sparklines."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    const auto areas = SelAreas(s);
    s.engine->PostUser(TR("Clear Sparklines"), [=](Document& d) {
      for (const Rect& a : areas) d.ClearSparklines(sheet, a);
    });
  };
}

void DrawIllustrationDialogs(AppState& s) {
  IllUi& u = I();
  DrawSymbolWindow(s);
  DrawInsertGallery(s);
  // Hidden and Empty Cell Settings
  if (u.hidden_request) {
    u.hidden_request = false;
    ImGui::OpenPopup(TR("Hidden and Empty Cell Settings###yc_spark_hidden"));
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (BeginModal(TR("Hidden and Empty Cell Settings###yc_spark_hidden"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::TextUnformatted(TR("Show empty cells as:"));
    ImGui::RadioButton(TR("Gaps"), &u.hidden_blanks, 1);
    ImGui::RadioButton(TR("Zero"), &u.hidden_blanks, 2);
    ImGui::RadioButton(TR("Connect data points with line"), &u.hidden_blanks, 3);
    ImGui::Checkbox(TR("Show data in hidden rows and columns"), &u.hidden_show);
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const uint32_t sheet = s.focus_sheet, id = u.spark_group;
      const int b = u.hidden_blanks;
      const bool h = u.hidden_show;
      s.engine->PostUser(TR("Hidden & Empty Cells"), [=](Document& d) {
        d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& g) {
          g.display_blanks = b;
          g.show_hidden = h;
        });
      });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  // Sparkline Vertical Axis Settings (Custom Value)
  if (u.axis_request) {
    u.axis_request = false;
    ImGui::OpenPopup(TR("Sparkline Vertical Axis Settings###yc_spark_axis"));
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (BeginModal(TR("Sparkline Vertical Axis Settings###yc_spark_axis"), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::TextUnformatted(u.axis_max ? TR("Enter the maximum value for the vertical axis:") : TR("Enter the minimum value for the vertical axis:"));
    ImGui::SetNextItemWidth(S(220));
    ImGui::InputText("##axisv", u.axis_value, sizeof(u.axis_value), ImGuiInputTextFlags_CharsScientific);
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      const uint32_t sheet = s.focus_sheet, id = u.spark_group;
      const bool mx = u.axis_max;
      const double v = std::strtod(u.axis_value, nullptr);
      s.engine->PostUser(TR("Axis"), [=](Document& d) {
        d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& g) {
          if (mx) g.max_scale = 3, g.max_value = v;
          else g.min_scale = 3, g.min_value = v;
        });
      });
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    if (close) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (s.dialog == Dialog::CreateSparklines && u.spark_mode != 0) {
    // Edit Sparklines (the group), Edit Sparkline Data (the active cell's) and Sparkline Date Range
    const char* title = u.spark_mode == 1 ? TR("Edit Sparklines###yc_dlg_spark") : u.spark_mode == 2 ? TR("Edit Sparkline Data###yc_dlg_spark")
                                                                                                       : TR("Sparkline Date Range###yc_dlg_spark");
    if (BeginDlg(s, Dialog::CreateSparklines, title, 380)) {
      bool close = false;
      if (u.spark_mode == 3) {
        ImGui::TextUnformatted(TR("Select the range that contains the date values for the sparkline data:"));
      } else if (u.spark_mode == 2) {
        ImGui::Text(TR("Select a data source for the sparkline (%s):"), core::CellA1(u.spark_r, u.spark_c, true, true).c_str());
      } else {
        ImGui::TextUnformatted(TR("Choose the data that you want"));
        ImGui::TextDisabled("%s", TR("Data Range"));
      }
      if (u.focus == 0) ImGui::SetKeyboardFocusHere(), u.focus = -1;
      RangeBox(s, "spark.data", u.data, sizeof(u.data), S(340), 0, TR("Data Range"));
      if (u.spark_mode == 1) {
        ImGui::Spacing();
        ImGui::TextUnformatted(TR("Choose where you want the sparklines to be placed"));
        ImGui::TextDisabled("%s", TR("Location Range"));
        RangeBox(s, "spark.location", u.location, sizeof(u.location), S(340), 0, TR("Location Range"));
      }
      if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
        const uint32_t sheet = s.focus_sheet, id = u.spark_group;
        if (u.spark_mode == 3) {
          uint32_t ds = 0;
          Rect dr{};
          if (u.data[0] && !ResolveRangeText(s.snap->wb, s.focus_sheet, u.data, &ds, &dr)) {
            s.ShowError("YCGrid", TR("The reference isn't valid."));
          } else {
            const model::Sheet* dsh = u.data[0] ? s.snap->Sheet(ds) : nullptr;
            const std::string range = dsh ? "'" + dsh->name + "'!" + core::RectA1(dr) : std::string();
            s.engine->PostUser(TR("Date Axis Type"), [=](Document& d) { d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& g) { g.date_range = range; }); });
            close = true;
          }
        } else {
          uint32_t ds = 0, ls = 0;
          Rect data{}, loc{};
          std::vector<model::Sparkline> items;
          const std::string location = u.spark_mode == 2 ? core::CellA1(u.spark_r, u.spark_c) : std::string(u.location);
          if (!ResolveRangeText(s.snap->wb, s.focus_sheet, u.data, &ds, &data) || !ResolveRangeText(s.snap->wb, s.focus_sheet, location, &ls, &loc) ||
              ls != s.focus_sheet) {
            s.ShowError("YCGrid", TR("The reference isn't valid."));
          } else if (!engine::BuildSparklines(ds, data, loc, &items)) {
            s.ShowMessage("YCGrid", TR("Location reference is not valid because the cells are not all in the same column or same row; or the data doesn't match the location."));
          } else {
            const bool one = u.spark_mode == 2;
            s.engine->PostUser(TR("Edit Sparklines"), [=](Document& d) {
              d.UpdateSparklineGroup(sheet, id, [&](model::SparklineGroup& g) {
                if (!one) {
                  g.items = items;
                  return;
                }
                for (auto& it : g.items) {
                  if (it.r == items[0].r && it.c == items[0].c) it = items[0];
                }
              });
            });
            close = true;
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
      EndDlg(s, close);
      if (s.dialog != Dialog::CreateSparklines) u.spark_mode = 0;
    } else if (s.dialog != Dialog::CreateSparklines) {
      u.spark_mode = 0;
    }
    return;
  }
  if (BeginDlg(s, Dialog::CreateSparklines, TR("Create Sparklines###yc_dlg_spark"), 380)) {
    ImGui::TextUnformatted(TR("Choose the data that you want"));
    ImGui::TextDisabled("%s", TR("Data Range"));
    if (u.focus == 0) ImGui::SetKeyboardFocusHere(), u.focus = -1;
    RangeBox(s, "spark.data", u.data, sizeof(u.data), S(340), 0, TR("Data Range"));
    ImGui::Spacing();
    ImGui::TextUnformatted(TR("Choose where you want the sparklines to be placed"));
    ImGui::TextDisabled("%s", TR("Location Range"));
    if (u.focus == 1) ImGui::SetKeyboardFocusHere(), u.focus = -1;
    RangeBox(s, "spark.location", u.location, sizeof(u.location), S(340), 0, TR("Location Range"));
    bool close = false;
    if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
      uint32_t ds = 0, ls = 0;
      Rect data{}, loc{};
      std::vector<model::Sparkline> items;
      if (!ResolveRangeText(s.snap->wb, s.focus_sheet, u.data, &ds, &data) || !ResolveRangeText(s.snap->wb, s.focus_sheet, u.location, &ls, &loc) ||
          ls != s.focus_sheet) {
        s.ShowError("YCGrid", TR("The reference isn't valid."));
      } else if (!engine::BuildSparklines(ds, data, loc, &items)) {
        s.ShowMessage("YCGrid", TR("Location reference is not valid because the cells are not all in the same column or same row; or the data doesn't match the location."));
      } else {
        model::SparklineGroup g;
        g.type = u.spark_type;
        g.items = std::move(items);
        const uint32_t sheet = s.focus_sheet;
        s.engine->PostUser(TR("Insert Sparklines"), [sheet, g](Document& d) { d.AddSparklineGroup(sheet, g); });
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
}

}  // namespace yc::app
