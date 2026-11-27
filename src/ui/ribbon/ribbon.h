// SPDX-License-Identifier: Apache-2.0
// ribbon.h - the ribbon (PLAN §11)
//
// - Everything is drawn with ImDrawList and looks like Excel; ImGui's TabBar / Button look isn't used.
// - The layout is data (Tab -> Group -> Block -> Item), to be loaded from ui/ribbon/ribbon.yaml later (the prototype uses a C++ table).
// - Adaptive scaling: every group has 4 size stages (Large -> Medium -> Small -> Collapsed),
//   shrinking step by step in the shrink order as the window narrows; results are cached by (width, DPI, tab).
// - Every click only calls the CommandRegistry (PLAN §11.5).
#pragma once

#include "imgui.h"
#include "core/i18n.h"
#include "imgui_internal.h"

#include <functional>
#include <string>
#include <vector>

namespace yc::ui::ribbon {

enum class ItemKind {
  Button,       // normal button
  Split,        // split button: the body runs cmd, the arrow opens menu
  Toggle,       // toggle button (B, I, U)
  Menu,         // the whole button is a drop-down menu
  FontCombo,    // font name (searchable)
  SizeCombo,    // font size (editable)
  FormatCombo,  // number format
  ColorSplit,   // color split button (the color bar shows the last color)
  Check,        // check box (View -> Gridlines...)
  Gallery,      // in-ribbon gallery of preview tiles (Cell Styles); a large drop-down button when the group shrinks
  Custom,       // drawn by the app (RibbonX dropDown, comboBox, editBox, labelControl): draw(r), width = combo_width
};

// one tile of a gallery: a preview of what its command applies (fill, font, borders), like Excel's Cell Styles
struct GalleryTile {
  struct Edge {
    ImU32 col = 0;
    int width = 0;     // 0 none, 1 thin, 2 medium, 3 thick
    bool dbl = false;  // double line
  };
  std::string cmd;      // command run on click; its label is the tile text
  std::string section;  // heading in the expanded gallery
  ImU32 fill = 0;       // 0 = no fill
  ImU32 text = IM_COL32(0, 0, 0, 255);
  bool bold = false, italic = false, underline = false;
  float font_scale = 1.0f;  // relative to the default cell font size
  Edge edge[4];             // left, top, right, bottom
  // a picture drawn by the app instead of the text face (Format as Table: the style on a small table)
  std::function<void(ImDrawList* dl, const ImRect& r)> picture;
};

// tiles of the gallery named id (set by the app; looked up when drawing)
void SetGallery(const std::string& id, std::vector<GalleryTile> tiles);
// commands listed under the expanded gallery (New Cell Style...)
void SetGalleryFooter(const std::string& id, std::vector<std::string> cmds);
// the expanded gallery's columns and tile size (unscaled pixels); the default is Cell Styles' 5 columns of text tiles
void SetGalleryLayout(const std::string& id, int cols, float tile_w, float tile_h);
// open the expanded gallery id on the next frame (its tab is brought to the front): a gallery's command run by a key
void OpenGallery(const std::string& id);

struct Item {
  ItemKind kind = ItemKind::Button;
  std::string cmd;                // main command id
  const char* icon = nullptr;     // 16 px icon
  const char* icon_large = nullptr;  // 32 px icon (empty = use icon)
  std::vector<std::string> menu;  // command ids of Split / Menu; "-" = separator
  float combo_width = 0.0f;       // combo box width (base px)
  ImU32* color = nullptr;         // current color of ColorSplit
  uint32_t* theme_ref = nullptr;  // ColorSplit: the theme color picked (model::MakeThemeColor), 0 = the RGB in *color
  bool* check = nullptr;          // state of Check
  bool label_in_rows = false;     // whether text is shown next to the icon in a Rows layout
  std::string gallery;            // Gallery: name passed to SetGallery
  std::function<void(const ImRect&)> draw;  // Custom: draws (and handles) the item in its box
  // a picture instead of the icon glyph (RibbonX image / getImage); returns 0 while it isn't loaded
  std::function<ImTextureID()> image;
  // Split: the main part runs (and shows) the entry chosen last from its menu, as Excel's Borders and Underline do
  bool remember_last = false;
};

enum class BlockKind {
  Large,  // one large button (32 px icon + two lines of text)
  Stack,  // up to 3 vertically stacked medium / small buttons
  Rows,   // controls arranged in two horizontal rows (e.g. the Font group)
  Gallery,  // one Gallery item: tiles in two rows (4 columns at Large, 2 at Medium), a large button below that
};

struct Block {
  BlockKind kind = BlockKind::Large;
  std::vector<Item> items;               // Large / Stack
  std::vector<std::vector<Item>> rows;   // Rows
  bool keep_large = false;               // stays a large button when shrinking (e.g. Paste)
};

struct Group {
  std::string label;
  const char* icon = nullptr;  // icon of the group when collapsed into one button
  std::vector<Block> blocks;
  std::string launcher;        // command id of the dialog launcher (empty = none)
};

struct Tab {
  std::string label;
  std::vector<Group> groups;
  bool backstage = false;      // "File": opens Backstage instead of showing a ribbon
  const bool* visible = nullptr;  // contextual tab (e.g. Table Design): shown only while *visible
};

// state of the contextual tabs and their check boxes, updated by the app every frame
struct ContextState {
  bool table = false;  // the active cell is in a table
  bool tbl_header = true, tbl_totals = false, tbl_banded_rows = true, tbl_banded_cols = false, tbl_first_col = false, tbl_last_col = false,
       tbl_filter = true;
  bool sparkline = false;  // the active cell has a sparkline
  bool pg_gridlines = false, pg_headings = false;
  bool chart = false;  // a chart is selected (or a chart sheet is active)
  bool pivot = false;  // the active cell is in a PivotTable  // Page Layout -> Sheet Options of the active sheet
  // PivotTable Design > Style Options, Analyze > Show of the active PivotTable
  bool pv_row_headers = true, pv_col_headers = true, pv_banded_rows = false, pv_banded_cols = false, pv_buttons = true, pv_headers = true,
       pv_chart_buttons = true;
  bool sp_high = false, sp_low = false, sp_negative = false, sp_first = false, sp_last = false, sp_markers = false, sp_axis = false;
  // Shape Format / Picture Format: a shape, text box or picture is selected
  bool shape = false, picture = false;
  ImU32 shape_fill = IM_COL32(0x44, 0x72, 0xC4, 255), shape_line = IM_COL32(0x2F, 0x52, 0x8F, 255), picture_line = IM_COL32(0, 0, 0, 255);
  // the Size group's Height / Width boxes (drawn by the app): which 0 = height, 1 = width
  std::function<void(int which, const ImRect& r)> size_box;
  // Chart Format > Current Selection: the Chart Elements box (drawn by the app)
  std::function<void(const ImRect& r)> chart_elements_box;
  // Slicer (contextual): a slicer or timeline is selected; its boxes (drawn by the app): 0 caption, 1 columns, 2 height,
  // 3 width
  bool slicer = false;
  std::function<void(int which, const ImRect& r)> slicer_box;
  // Page Layout and View (drawn by the app's layout_box: "ScaleAdjust", "SheetOptionsHead" / "View" / "Print"); the
  // Header & Footer tab while a header or footer is typed in Page Layout view, and its Options check boxes
  std::function<void(const std::string& id, const ImRect& r)> layout_box;
  bool ruler = true;
  bool header_footer = false;
  bool hf_first = false, hf_odd_even = false, hf_scale = true, hf_align = true;
  // Timeline (contextual): a timeline is selected; its Show options
  bool timeline = false;
  bool tl_header = true, tl_label = true, tl_scroll = true, tl_level = true;
  // PivotTable Analyze > Active Field: the box (drawn by the app); > Select > Enable Selection
  std::function<void(const ImRect& r)> pivot_field_box;
  // PivotTable Analyze > PivotTable: 0 the "PivotTable Name:" label, 1 the box to type the name in (drawn by the app)
  std::function<void(int which, const ImRect& r)> pivot_name_box;
  // Table Design > Properties: 0 the "Table Name:" label, 1 the box to type the name in (drawn by the app)
  std::function<void(int which, const ImRect& r)> table_name_box;
  bool pv_enable_selection = true;
};
ContextState& Context();

enum class Stage : int { Large = 0, Medium = 1, Small = 2, Collapsed = 3 };

struct State {
  int selected_tab = 1;
  // an insert command just ran (Ctrl+T, Alt+F1, a shape): the contextual tab its object brings is selected when it
  // appears within this frame (Excel opens Table Design, Chart Design, Shape Format); and the tab that was selected
  // before it comes back when the contextual tab goes away
  int open_contextual_until = 0;
  int tab_before_contextual = -1;
  std::vector<char> tab_was_visible;
  bool collapsed = false;      // show tabs only (double-click a tab or Ctrl+F1)
  bool peek = false;           // temporarily expanded while collapsed
  // where the tabs and the collapse arrow were drawn last frame (tests and agents click them: ui.state "ribbon")
  std::vector<std::pair<std::string, ImRect>> tab_rects;
  ImRect arrow_rect;
  bool backstage = false;
  int backstage_page = 0;  // 0 Info, 1 New, 2 Open, 3 Export, 4 Options
  std::function<void(int page, const ImRect& area)> backstage_content;  // page contents are drawn by the app
  // [YC] the About menu at the end of the tab row (the version and the program's path): its contents are drawn by the
  // app; no About entry while it isn't set. about_open_request opens it (the About command).
  std::function<void()> about_menu;
  bool about_open_request = false;
  // command of the gallery tile under the mouse this frame ("" = none): the app shows a live preview of it
  std::string hovered_tile;
  // current values of the Font group (prototype: stored here; to be derived from the active cell's style, PLAN §11.6)
  std::string font_name = "Calibri";
  std::string font_size = "11";
  std::string number_format = TR("General");
  // the active cell's value shown in each format of the Number Format list, in its order (Excel's samples)
  std::vector<std::string> number_format_samples;
  ImU32 fill_color = IM_COL32(255, 255, 0, 255);
  ImU32 font_color = IM_COL32(192, 0, 0, 255);
  uint32_t fill_theme = 0, font_theme = 0;  // theme colors picked from the palettes (0 = the RGB above)
  // right-click on a gallery tile: the app draws the menu items (inside an open popup)
  std::function<void(const std::string& gallery, const std::string& tile_cmd)> tile_menu;
  // right-click on a ribbon command (Add to Quick Access Toolbar...): the app draws the menu items
  std::function<void(const std::string& cmd)> item_menu;
  // a picture drawn for a menu entry instead of its icon (Data Bars, Color Scales, Icon Sets): draws at the entry's
  // left (at, height h) and returns its width, 0 for entries without one (dl nullptr: only the width)
  std::function<float(const std::string& cmd, ImDrawList* dl, ImVec2 at, float h)> menu_picture;
  // a taller entry for a bigger picture (chart styles and layouts); 0 = the usual height
  std::function<float(const std::string& cmd)> menu_entry_height;
  // the size of the tiles of a drop-down's picture section (base px) by its first command; 0 = the default
  std::function<float(const std::string& first_cmd)> menu_tile_size;
  // whether a menu entry is shown now (Chart Styles: the selected chart type's set); unset = every entry
  std::function<bool(const std::string& cmd)> menu_entry_shown;
  // a submenu whose body the app draws (Borders > Line Color: the palette); true = drawn
  std::function<bool(const std::string& submenu)> submenu_body;
  // the color picked from a palette inside a drop-down ("#Cmd" entries: Sparkline Color, Marker Color), read by Cmd
  uint32_t menu_color = 0;
  // KeyTips: 0 off, 1 over the tabs (Alt / F10), 2 over the selected tab's commands
  int keytip_level = 0;
  // the frame a dialog that isn't modal (Find and Replace) last had the keyboard: Alt + a letter is the dialog's then
  int dialog_keys_frame = -10;
  // the frame a drop-down of the ribbon (a menu, a gallery, a combo's list) last showed: the arrow keys and Enter move
  // in it and pick, as in Excel's (Alt, H, N, arrows, Enter), not on the sheet behind it
  int menu_frame = -10;
  std::string legacy_keys;  // Excel 2003's menu keys typed so far after Alt ("ES"), shown as Excel shows them
  // measurements
  double last_cpu_ms = 0.0;
  std::vector<int> last_stages;  // stage of every group of the current tab (for --report and tests)
  float last_available_width = 0.0f;
};

State& St();
// a KeyTip on something outside the ribbon (the Quick Access Toolbar): shown and matched at level 1
void AddKeyTip(const std::string& key, ImVec2 at, std::function<void()> action);
std::vector<Tab>& Tabs();
// how the ribbon shows command cmd: -1 not as a button (absent, or only as a check box, combo or gallery tile),
// 0 as a button without an icon, 1 as a button with one (for yc.ui.commands() and the tests)
int ButtonIcon(const std::string& cmd);
// the built-in tab whose groups hold cmd, as a button or a menu entry ("" for none): tests run every command of a tab
std::string TabOf(const std::string& cmd);
// [YC] tests: where each command's button, menu entry, gallery tile or Backstage entry was drawn in the last two
// frames (window pixels), so a test clicks it as a person does (ui.ribbon_items)
struct ItemRect {
  std::string cmd;
  ImRect rect;
  const char* where;  // "ribbon" (a button), "dropdown" (opens a menu or gallery), "combo", "menu", "tile", "backstage"
};
std::vector<ItemRect> VisibleItems();
// tabs or groups were added / removed after BuildDefaultTabs (RibbonX): drop the cached layout
void InvalidateLayout();
// a ribbon icon glyph centered at center (base = icon size in base px)
void DrawRibbonIcon(ImDrawList* dl, const char* glyph, ImVec2 center, float base, ImU32 col);

// Excel's color palette inside an open popup or window: Theme Colors (each column with its own shades, from the
// workbook's theme) and Standard Colors, every swatch named in its tooltip ("Accent 1, Lighter 80%"). Returns true
// when one is clicked: *ref is model::MakeThemeColor(...) for a theme color (it follows the theme), 0xFFRRGGBB for a
// standard color, 0 for none_label ("No Fill", "Automatic", "No Color"; nullptr: no such entry).
bool PaletteSwatches(uint32_t* ref, const char* none_label = nullptr);

// a Home tab control drawn elsewhere by its command id (the cell menu's mini toolbar: fonts, sizes, borders, colors,
// toggles), with the ribbon's look, menus and pickers; listed by VisibleItems as "minitoolbar". False: no such item.
bool DrawItemElsewhere(const std::string& cmd, const ImRect& r);

// called by the app after registering commands; creates the default tabs and groups.
void BuildDefaultTabs(bool* show_gridlines, bool* show_headings, bool* show_formula_bar);

float TabRowHeight();   // DPI included
float BodyHeight();     // DPI included

// draws inside the current ImGui window.
void DrawTabRow(const ImRect& r);
void DrawBody(const ImRect& r);
// Backstage (File) page: returns false when the user pressed Back.
void DrawBackstage(const ImRect& r);

// stage of every group for an available width (pure computation, callable from tests).
std::vector<int> SolveStages(int tab_index, float available_width);

}  // namespace yc::ui::ribbon
