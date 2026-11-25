// SPDX-License-Identifier: Apache-2.0
// tables_ui.cpp - tables (ListObjects): Insert -> Table, Format as Table, the Table Design tab, the totals row menu
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"
#include "yc_ui_items.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

struct TablesUi {
  char range[128] = "";
  bool headers = true;
  std::string style = "TableStyleMedium2";
  char name[128] = "";
  uint32_t sheet = 0, id = 0;
  // totals row menu
  uint32_t tot_sheet = 0, tot_id = 0, tot_col = 0;
  ImVec2 tot_pos{};
  // New Table Style / Modify Table Style: the style being made, its old name (modify), the element chosen
  bool ts_open = false, ts_request = false;
  model::TableStyleDef ts;
  std::string ts_original;
  char ts_name[128] = "";
  int ts_element = 0;
  bool ts_default = false;  // Set as default table style for this document
  // Table Design > Table Name: the box's text while it is typed in
  char name_box[128] = "";
  bool name_box_active = false;
  // the galleries were built for this store value
  std::string galleries_for = "\x01";
};

TablesUi& T() {
  static TablesUi t;
  return t;
}

const ImGuiID kTotalsPopup = ImHashStr("##yc_totals_popup");

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

std::string RangeText(const Rect& r) { return "$" + core::ColumnName(r.c0) + "$" + std::to_string(r.r0 + 1) + ":$" + core::ColumnName(r.c1) + "$" + std::to_string(r.r1 + 1); }

// the table at the active cell
const model::Table* ActiveTable(AppState& s) {
  if (!s.snap) return nullptr;
  const auto& sel = s.FocusView().sel;
  return model::TableAt(s.snap->wb, s.focus_sheet, sel.act_r, sel.act_c);
}

// Insert -> Table / Format as Table: the Create Table dialog for the current region
void OpenCreate(AppState& s, const std::string& style) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  if (const model::Table* t = ActiveTable(s)) {
    // inside a table, a style just restyles it
    const uint32_t sheet = s.focus_sheet, id = t->id;
    s.engine->PostUser(TR("Table Style"), [=](Document& d) { d.UpdateTable(sheet, id, [&](model::Table& x) { x.style = style; }); });
    return;
  }
  Rect r = SelRect(s);
  if (r.IsCell()) r = engine::CurrentRegion(*sh, r.r0, r.c0);
  TablesUi& u = T();
  std::snprintf(u.range, sizeof(u.range), "%s", RangeText(r).c_str());
  // a first row of text is the header (like Excel's guess)
  u.headers = true;
  for (uint32_t c = r.c0; c <= r.c1; ++c) {
    const auto& v = sh->GetValue(r.r0, c);
    if (!v.IsEmpty() && !v.IsString()) u.headers = false;
  }
  u.style = style;
  s.OpenDialog(Dialog::CreateTable);
}

const std::map<std::string, std::string>& Store(AppState& s) {
  static const std::map<std::string, std::string> kNone;
  return s.snap ? *s.snap->wb.store : kNone;
}

// the workbook's own table styles (New Table Style)
std::vector<model::TableStyleDef> CustomStyles(AppState& s) {
  const auto& st = Store(s);
  auto it = st.find(model::kTableStylesKey);
  return it == st.end() ? std::vector<model::TableStyleDef>{} : model::DecodeTableStyles(it->second);
}

void SaveCustomStyles(AppState& s, const std::vector<model::TableStyleDef>& defs, const char* label) {
  const std::string blob = defs.empty() ? std::string() : model::EncodeTableStyles(defs);
  s.engine->PostUser(label, [blob](Document& d) {
    auto& st = d.wb_mut().store.mut();
    if (blob.empty()) st.erase(model::kTableStylesKey);
    else st[model::kTableStylesKey] = blob;
    d.wb_mut().dirty = true;
  });
}

// the command of a style's gallery tile: TableStyleMedium2..., the workbook's own without spaces
std::string StyleCmd(const std::string& name) {
  std::string id = "TableStyle";
  for (char ch : name) {
    if (ch != ' ') id += ch;
  }
  return name.rfind("TableStyle", 0) == 0 ? name : id;
}

ImU32 Col(uint32_t argb) {
  const uint32_t c = model::ResolveColor(argb);
  return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
}

// a style on a small table (5 columns, a header and four rows), as Excel's gallery shows it; nullptr = None
void DrawStylePicture(ImDrawList* dl, const ImRect& r, const model::TableStyleDef* d, bool first_col = false, bool last_col = false, bool banded_cols = false,
                      bool totals = false) {
  model::Table t;
  const uint32_t rows = totals ? 6 : 5;
  t.range = core::Rect{0, 0, rows - 1, 4};
  t.header = true;
  t.totals = totals;
  t.first_col = first_col;
  t.last_col = last_col;
  t.banded_cols = banded_cols;
  const ImRect in(ImVec2(r.Min.x + S(4), r.Min.y + S(5)), ImVec2(r.Max.x - S(4), r.Max.y - S(5)));
  const float cw = in.GetWidth() / 5.0f, rh = in.GetHeight() / static_cast<float>(rows);
  for (uint32_t i = 0; i < rows; ++i) {
    for (uint32_t j = 0; j < 5; ++j) {
      model::Style st;
      if (d) model::ApplyTableStyle(t, *d, i, j, &st);
      const ImRect c(ImVec2(in.Min.x + cw * j, in.Min.y + rh * i), ImVec2(in.Min.x + cw * (j + 1), in.Min.y + rh * (i + 1)));
      if (st.fill_color >> 24) dl->AddRectFilled(c.Min, c.Max, Col(st.fill_color));
      if (!d) dl->AddRect(c.Min, ImVec2(c.Max.x + 1, c.Max.y + 1), IM_COL32(0xD0, 0xD0, 0xD0, 255));
      // a short dash stands for the text, in the text's color
      const ImU32 ink = (st.font_color >> 24) ? Col(st.font_color) : IM_COL32(0x40, 0x40, 0x40, 255);
      const float y = c.GetCenter().y;
      dl->AddLine(ImVec2(c.Min.x + cw * 0.22f, y), ImVec2(c.Max.x - cw * 0.22f, y), ink, st.bold ? S(2) : 1.0f);
      auto line = [&](const model::BorderSide& b, ImVec2 a, ImVec2 z) {
        if (b.style == model::LineStyle::None) return;
        const float w = b.style == model::LineStyle::Thick ? S(2.5f) : b.style == model::LineStyle::Medium ? S(1.6f) : 1.0f;
        const ImU32 lc = b.color ? Col(b.color) : IM_COL32(0, 0, 0, 255);
        dl->AddLine(a, z, lc, w);
        if (b.style == model::LineStyle::Double) dl->AddLine(ImVec2(a.x, a.y + 2), ImVec2(z.x, z.y + 2), lc, 1.0f);
      };
      line(st.border[model::kTop], c.Min, ImVec2(c.Max.x, c.Min.y));
      line(st.border[model::kBottom], ImVec2(c.Min.x, c.Max.y), c.Max);
      line(st.border[model::kLeft], c.Min, ImVec2(c.Min.x, c.Max.y));
      line(st.border[model::kRight], ImVec2(c.Max.x, c.Min.y), c.Max);
    }
  }
}

// the galleries of Format as Table (Home) and Table Styles (Table Design): Custom, Light, Medium, Dark
void BuildTableGalleries(AppState& s) {
  TablesUi& u = T();
  const auto& st = Store(s);
  auto it = st.find(model::kTableStylesKey);
  const std::string blob = it == st.end() ? std::string() : it->second;
  if (blob == u.galleries_for) return;
  u.galleries_for = blob;
  auto& R = ui::Commands();
  std::vector<ui::ribbon::GalleryTile> home, design;
  auto tile = [&](const model::TableStyleDef* d, const std::string& cmd, const char* section) {
    ui::ribbon::GalleryTile g;
    g.cmd = cmd;
    g.section = section;
    const model::TableStyleDef copy = d ? *d : model::TableStyleDef{};
    const bool none = d == nullptr;
    g.picture = [copy, none](ImDrawList* dl, const ImRect& r) { DrawStylePicture(dl, r, none ? nullptr : &copy); };
    return g;
  };
  for (const auto& d : model::DecodeTableStyles(blob)) {
    const std::string cmd = StyleCmd(d.name);
    if (!R.Find(cmd)) {
      ui::Command c;
      c.id = cmd;
      c.label = d.name;
      const std::string name = d.name;
      R.Add(c).execute = [&s, name](ui::Command&) { OpenCreate(s, name); };
    }
    home.push_back(tile(&d, cmd, TR("Custom")));
    design.push_back(home.back());
  }
  design.push_back(tile(nullptr, "TableStyleNone", TR("Light")));
  for (const auto& d : model::BuiltinTableStyles()) {
    const char* section = d.group == 1 ? TR("Light") : d.group == 2 ? TR("Medium") : TR("Dark");
    home.push_back(tile(&d, d.name, section));
    design.push_back(home.back());
  }
  ui::ribbon::SetGallery("FormatAsTable", home);
  ui::ribbon::SetGallery("TableStyles", design);
  ui::ribbon::SetGalleryLayout("FormatAsTable", 7, 61, 46);
  ui::ribbon::SetGalleryLayout("TableStyles", 7, 61, 46);
  ui::ribbon::SetGalleryFooter("FormatAsTable", {"NewTableStyle"});
  ui::ribbon::SetGalleryFooter("TableStyles", {"NewTableStyle", "TableStyleClear"});
}

// the style named, for the dialogs (a built-in one, or the workbook's own)
const model::TableStyleDef* StyleNamed(AppState& s, const std::string& name) {
  if (name == "TableStyleNone") return nullptr;
  return model::FindTableStyle(Store(s), name);
}

void OpenTableStyleDialog(AppState& s, const model::TableStyleDef& start, const std::string& original, const std::string& name) {
  TablesUi& u = T();
  u.ts = start;
  u.ts.builtin = false;
  u.ts.group = 0;
  u.ts_original = original;
  std::snprintf(u.ts_name, sizeof(u.ts_name), "%s", name.c_str());
  u.ts_element = 0;
  u.ts_default = !original.empty() && model::DefaultTableStyle(Store(s)) == original;
  u.ts_request = true;
}

void SetDefaultStyle(AppState& s, const std::string& name) {
  s.engine->PostUser(TR("Set As Default"), [name](Document& d) {
    auto& st = d.wb_mut().store.mut();
    if (name.empty() || name == "TableStyleMedium2") st.erase(model::kDefaultTableStyleKey);
    else st[model::kDefaultTableStyleKey] = model::EncodeStoreString(name);
    d.wb_mut().dirty = true;
  });
}

std::string FreeStyleName(AppState& s, const std::string& base) {
  const auto defs = CustomStyles(s);
  for (int k = 1;; ++k) {
    const std::string n = base + " " + std::to_string(k);
    if (std::none_of(defs.begin(), defs.end(), [&](const model::TableStyleDef& d) { return d.name == n; })) return n;
  }
}

void Toggle(AppState& s, void (*fn)(model::Table&)) {
  const model::Table* t = ActiveTable(s);
  if (!t) return;
  const uint32_t sheet = s.focus_sheet, id = t->id;
  s.engine->PostUser(TR("Table Style Options"), [=](Document& d) { d.UpdateTable(sheet, id, fn); });
}

}  // namespace

const model::Table* TableStylePreview(AppState& s, const std::string& hovered) {
  if (hovered.rfind("TableStyle", 0) != 0 || hovered == "TableStylesMenu" || hovered == "TableStyleClear" || !s.snap) return nullptr;
  // the style of the tile: a built-in one, None, or the workbook's own (its tile's command has no spaces)
  std::string name;
  if (hovered == "TableStyleNone" || model::BuiltinTableStyle(hovered)) name = hovered;
  for (const auto& d : CustomStyles(s)) {
    if (StyleCmd(d.name) == hovered) name = d.name;
  }
  if (name.empty()) return nullptr;
  static model::Table preview;
  if (const model::Table* t = ActiveTable(s)) {
    preview = *t;  // the table in that style (Excel's live preview)
  } else {
    // the range Format as Table would make into a table, with a header row and banded rows
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return nullptr;
    Rect r = SelRect(s);
    if (r.IsCell()) r = engine::CurrentRegion(*sh, r.r0, r.c0);
    if (r.Count() > 1000000) return nullptr;
    preview = model::Table{};
    preview.range = r;
  }
  preview.style = name;
  return &preview;
}

void UpdateTableContext(AppState& s) {
  auto& cx = ui::ribbon::Context();
  BuildTableGalleries(s);
  const model::Table* t = ActiveTable(s);
  cx.table = t != nullptr;
  if (!t) return;
  cx.tbl_header = t->header;
  cx.tbl_totals = t->totals;
  cx.tbl_banded_rows = t->banded_rows;
  cx.tbl_banded_cols = t->banded_cols;
  cx.tbl_first_col = t->first_col;
  cx.tbl_last_col = t->last_col;
  cx.tbl_filter = t->filter_button;
}

void RegisterTableCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon = nullptr, const char* shortcut = "", const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.shortcut = shortcut;
    c.tip_desc = desc;
    return c;
  };
  // Ctrl+T and Format as Table start with the document's default table style (New Table Style's "Set as default")
  R.Add(make("InsertTable", TR("Table"), ui::icon::Table, "Ctrl+T", TR("Create a table to organize and analyze related data."))).execute = [&s](ui::Command&) {
    OpenCreate(s, model::DefaultTableStyle(Store(s)));
  };
  R.Add(make("FormatAsTable", TR("Format as\nTable"), ui::icon::FormatAsTable, "", TR("Quickly format a range as a table with its own style."))).execute = [&s](ui::Command&) {
    OpenCreate(s, model::DefaultTableStyle(Store(s)));
  };
  R.Add(make("TableStylesMenu", TR("Quick\nStyles"), ui::icon::FormatAsTable)).execute = [](ui::Command&) {};
  // Excel's 60 built-in styles (Light 1-21, Medium 1-28, Dark 1-11) and the Trading look; the tooltip is Excel's
  // ("Blue, Table Style Medium 2")
  for (const auto& d : model::BuiltinTableStyles()) {
    const std::string style = d.name;
    R.Add(make(d.name.c_str(), "")).execute = [&s, style](ui::Command&) { OpenCreate(s, style); };
    if (ui::Command* c = R.Find(d.name)) {
      // "Blue, Table Style Medium 2" in the user's language
      std::string label = model::TableStyleLabel(d);
      const size_t comma = label.find(", Table Style ");
      if (comma != std::string::npos) {
        const std::pair<const char*, const char*> kColors[] = {{"Black", TR("Black")}, {"Blue", TR("Blue")}, {"Orange", TR("Orange")}, {"Gray", TR("Gray")},
                                                               {"Gold", TR("Gold")}, {"Light Blue", TR("Light Blue")}, {"Green", TR("Green")}};
        std::string color = label.substr(0, comma);
        for (const auto& [en, tr] : kColors) {
          if (color == en) color = tr;
        }
        label = color + ", " + TR("Table Style") + " " + label.substr(comma + 14);
      }
      c->label = label == "Trading (black)" ? std::string(TR("Trading (black)")) : label;
    }
  }
  R.Add(make("TableStyleNone", TR("None##ctx_tablestyle"))).execute = [&s](ui::Command&) { OpenCreate(s, "TableStyleNone"); };
  // Table Styles > Clear: the table keeps its data and options, without a style (Excel)
  R.Add(make("TableStyleClear", TR("Clear##ctx_tablestyle"), nullptr, "", TR("Remove the table style; the table stays."))).execute = [&s](ui::Command&) {
    OpenCreate(s, "TableStyleNone");
  };
  R.Add(make("NewTableStyle", TR("New Table Style..."), nullptr, "", TR("Make a table style of your own, element by element."))).execute = [&s](ui::Command&) {
    OpenTableStyleDialog(s, model::TableStyleDef{}, "", FreeStyleName(s, "Table Style"));
  };
  // right-click on a table style: Apply, Modify..., Duplicate..., Delete
  ui::ribbon::St().tile_menu = [&s, prev = ui::ribbon::St().tile_menu](const std::string& gallery, const std::string& cmd) {
    if (gallery != "FormatAsTable" && gallery != "TableStyles") {
      if (prev) prev(gallery, cmd);
      return;
    }
    std::string name = cmd;
    for (const auto& d : CustomStyles(s)) {
      if (StyleCmd(d.name) == cmd) name = d.name;
    }
    const model::TableStyleDef* d = StyleNamed(s, name);
    if (ImGui::MenuItem(TR("Apply"))) OpenCreate(s, name);
    const bool own = d && !d->builtin;
    if (ImGui::MenuItem(TR("Modify..."), nullptr, false, own)) OpenTableStyleDialog(s, *d, d->name, d->name);
    if (ImGui::MenuItem(TR("Duplicate..."), nullptr, false, d != nullptr)) {
      const std::string base = d->builtin ? model::TableStyleLabel(*d) : d->name;
      OpenTableStyleDialog(s, *d, "", FreeStyleName(s, base.substr(base.find(", ") == std::string::npos ? 0 : base.find(", ") + 2)));
    }
    if (ImGui::MenuItem(TR("Set As Default"), nullptr, model::DefaultTableStyle(Store(s)) == name, d != nullptr)) SetDefaultStyle(s, name);
    if (ImGui::MenuItem(TR("Delete"), nullptr, false, own)) {
      auto defs = CustomStyles(s);
      defs.erase(std::remove_if(defs.begin(), defs.end(), [&](const model::TableStyleDef& x) { return x.name == name; }), defs.end());
      SaveCustomStyles(s, defs, TR("Delete Table Style"));
    }
  };
  // Table Design > Properties: "Table Name:" over the box; Enter renames the table (formulas follow), Esc puts it back
  ui::ribbon::Context().table_name_box = [&s](int which, const ImRect& r) {
    const model::Table* t = ActiveTable(s);
    if (which == 0) {
      ImGui::GetWindowDrawList()->AddText(ImVec2(r.Min.x + S(2), r.GetCenter().y - ImGui::GetFontSize() * 0.5f), ui::T().btn_text, TR("Table Name:"));
      return;
    }
    if (!t) return;
    TablesUi& u = T();
    if (!u.name_box_active) std::snprintf(u.name_box, sizeof(u.name_box), "%s", t->name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(r.Min.x, r.GetCenter().y - ImGui::GetFrameHeight() * 0.5f));
    ImGui::SetNextItemWidth(r.GetWidth());
    ImGui::PushID("##table_name_box");
    const bool enter = ImGui::InputText("##table_name", u.name_box, sizeof(u.name_box), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ui::NoteUiItem("##table_name", ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
    u.name_box_active = ImGui::IsItemActive();
    if (u.name_box_active && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      std::snprintf(u.name_box, sizeof(u.name_box), "%s", t->name.c_str());
      ImGui::ClearActiveID();
      u.name_box_active = false;
    }
    if ((enter || ImGui::IsItemDeactivatedAfterEdit()) && t->name != u.name_box && RefuseReadOnlyUi(s)) {
      std::snprintf(u.name_box, sizeof(u.name_box), "%s", t->name.c_str());  // read-only mode: the name stays
      u.name_box_active = false;
    } else if ((enter || ImGui::IsItemDeactivatedAfterEdit()) && t->name != u.name_box) {
      std::string err;
      const uint32_t sheet = s.focus_sheet, id = t->id;
      const std::string name = u.name_box;
      const bool ok = s.engine->Call([&](Document& d) {
        d.Begin(TR("Table Name"));
        const bool r2 = d.RenameTable(sheet, id, name, &err);
        d.Commit();
        return r2;
      });
      if (!ok) s.ShowError("YCGrid", err.empty() ? std::string(TR("That name isn't valid.")) : err);
      u.name_box_active = false;
    }
    ImGui::PopID();
  };
  R.Add(make("RenameTable", TR("Table\nName"), ui::icon::TableDesign, "", TR("Rename the table (formulas that use the name follow)."))).execute = [&s](ui::Command&) {
    const model::Table* t = ActiveTable(s);
    if (!t) return;
    TablesUi& u = T();
    u.sheet = s.focus_sheet;
    u.id = t->id;
    std::snprintf(u.name, sizeof(u.name), "%s", t->name.c_str());
    s.OpenDialog(Dialog::RenameTable);
  };
  R.Add(make("ResizeTable", TR("Resize\nTable"), ui::icon::ResizeTable, "", TR("Change the range of the table."))).execute = [&s](ui::Command&) {
    const model::Table* t = ActiveTable(s);
    if (!t) return;
    TablesUi& u = T();
    u.sheet = s.focus_sheet;
    u.id = t->id;
    std::snprintf(u.range, sizeof(u.range), "%s", RangeText(t->range).c_str());
    s.OpenDialog(Dialog::ResizeTable);
  };
  R.Add(make("ConvertToRange", TR("Convert to Range"), ui::icon::ConvertToRange, "", TR("Turn the table into a normal range; its look stays as cell formats."))).execute = [&s](ui::Command&) {
    const model::Table* t = ActiveTable(s);
    if (!t) return;
    const uint32_t sheet = s.focus_sheet, id = t->id;
    // Excel asks first (OK / Cancel)
    AskYesNo(s, "YCGrid", TR("Do you want to convert the table to a normal range?"), [&s, sheet, id] {
      s.engine->PostUser(TR("Convert to Range"), [=](Document& d) { d.DeleteTable(sheet, id, true); });
    });
  };
  R.Add(make("TblHeaderRow", TR("Header Row"))).execute = [&s](ui::Command&) {
    Toggle(s, [](model::Table& t) { t.header = !t.header; });
  };
  R.Add(make("TblTotalRow", TR("Total Row"))).execute = [&s](ui::Command&) { Toggle(s, [](model::Table& t) { t.totals = !t.totals; }); };
  R.Add(make("TblBandedRows", TR("Banded Rows"))).execute = [&s](ui::Command&) { Toggle(s, [](model::Table& t) { t.banded_rows = !t.banded_rows; }); };
  R.Add(make("TblBandedColumns", TR("Banded Columns"))).execute = [&s](ui::Command&) { Toggle(s, [](model::Table& t) { t.banded_cols = !t.banded_cols; }); };
  R.Add(make("TblFirstColumn", TR("First Column"))).execute = [&s](ui::Command&) { Toggle(s, [](model::Table& t) { t.first_col = !t.first_col; }); };
  R.Add(make("TblLastColumn", TR("Last Column"))).execute = [&s](ui::Command&) { Toggle(s, [](model::Table& t) { t.last_col = !t.last_col; }); };
  R.Add(make("TblFilterButton", TR("Filter Button"))).execute = [&s](ui::Command&) {
    const model::Table* t = ActiveTable(s);
    if (!t) return;
    const uint32_t sheet = s.focus_sheet, id = t->id;
    const bool on = !t->filter_button;
    // the sheet's AutoFilter when it is free, otherwise the table's own buttons (every table has its own in Excel)
    s.engine->PostUser(TR("Filter Button"), [=](Document& d) { d.SetTableFilterButton(sheet, id, on); });
  };
}

void DrawTableDialogs(AppState& s) {
  TablesUi& u = T();
  if (BeginDlg(s, Dialog::CreateTable, TR("Create Table###yc_dlg_createtable"), 340)) {
    ImGui::TextUnformatted(TR("Where is the data for your table?"));
    RangeBox(s, "table.create", u.range, sizeof(u.range), 0, RangeBox_Equals, TR("Where is the data for your table?"));
    ImGui::Checkbox(TR("My table has headers"), &u.headers);
    bool close = false;
    const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
    // read-only mode came on while a dialog was open: it closes with the status bar's message, never "A table can't
    // be created here." or "That name isn't valid." (the engine's refusal read as a failure)
    if (ok_pressed && RefuseReadOnlyUi(s)) {
      close = true;
    } else if (ok_pressed) {
      uint32_t rs = 0;
      Rect r{};
      if (!ResolveRangeText(s.snap->wb, s.focus_sheet, u.range, &rs, &r)) {
        s.ShowError("YCGrid", TR("The reference isn't valid."));
      } else {
        const bool headers = u.headers;
        const std::string style = u.style;
        std::string err;
        bool refused = false;
        const uint32_t id = s.engine->Call([&](Document& d) {
          const uint32_t blocks = d.protect_blocks();
          d.Begin(TR("Create Table"));
          const uint32_t n = d.AddTable(rs, r, headers, style, &err);
          d.Commit();
          refused = d.protect_blocks() != blocks;
          return n;
        });
        // a refusal the engine counted (protection, a header row's insertion across a filter) shows its own message:
        // a second one here said "A table can't be created here." over it (basics seq270352)
        if (!id && (!err.empty() || !refused)) s.ShowError("YCGrid", err.empty() ? std::string(TR("A table can't be created here.")) : err);
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::RenameTable, TR("Table Name###yc_dlg_renametable"), 320)) {
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##name", u.name, sizeof(u.name), ImGuiInputTextFlags_EnterReturnsTrue);
    bool close = false;
    const bool ok_pressed = ImGui::Button(TR("OK"), ImVec2(S(80), 0)) || enter;
    if (ok_pressed && RefuseReadOnlyUi(s)) {
      close = true;
    } else if (ok_pressed) {
      std::string err;
      const std::string name = u.name;
      const bool ok = s.engine->Call([&](Document& d) {
        d.Begin(TR("Table Name"));
        const bool r = d.RenameTable(u.sheet, u.id, name, &err);
        d.Commit();
        return r;
      });
      if (!ok) s.ShowError("YCGrid", err.empty() ? std::string(TR("That name isn't valid.")) : err);
      else close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  if (BeginDlg(s, Dialog::ResizeTable, TR("Resize Table###yc_dlg_resizetable"), 340)) {
    ImGui::TextUnformatted(TR("Select the new data range for your table:"));
    RangeBox(s, "table.resize", u.range, sizeof(u.range), 0, RangeBox_Equals, TR("Select the new data range for your table:"));
    // wrapped at the dialog's width: on one line it made the dialog as wide as the sentence
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", TR("The headers must remain in the same row, and the resulting table range must overlap the original table range."));
    ImGui::PopStyleColor();
    bool close = false;
    const bool ok_pressed = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
    if (ok_pressed && RefuseReadOnlyUi(s)) {
      close = true;
    } else if (ok_pressed) {
      uint32_t rs = 0;
      Rect r{};
      std::string err;
      if (!ResolveRangeText(s.snap->wb, u.sheet, u.range, &rs, &r) || rs != u.sheet) {
        s.ShowError("YCGrid", TR("The reference isn't valid."));
      } else if (!s.engine->Call([&](Document& d) {
                   d.Begin(TR("Resize Table"));
                   const bool ok = d.ResizeTable(u.sheet, u.id, r, &err);
                   d.Commit();
                   return ok;
                 })) {
        if (!err.empty()) s.ShowError("YCGrid", err);  // a refusal (protection) says why itself: never a blank message
        else close = true;
      } else {
        close = true;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
    EndDlg(s, close);
  }
  // ---- New Table Style / Modify Table Style (Excel's dialog) ----
  {
    const char* title = u.ts_original.empty() ? TR("New Table Style###yc_dlg_tablestyle") : TR("Modify Table Style###yc_dlg_tablestyle");
    if (u.ts_request && s.dialog == Dialog::None) {
      ImGui::OpenPopup(title);
      u.ts_request = false;
      u.ts_open = true;
    }
    ImGui::SetNextWindowSize(ImVec2(S(560), 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (u.ts_open && BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
      bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(TR("Name:"));
      ImGui::SameLine(S(70));
      ImGui::SetNextItemWidth(S(300));
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::InputText("##ts_name", u.ts_name, sizeof(u.ts_name));
      // Table Element: Excel's list; Format / Clear; the preview of the whole style beside it
      const char* kElems[model::kTeCount] = {TR("Whole Table"), TR("First Column Stripe"), TR("Second Column Stripe"), TR("First Row Stripe"),
                                             TR("Second Row Stripe"), TR("Last Column"), TR("First Column"), TR("Header Row"), TR("Total Row")};
      ImGui::TextUnformatted(TR("Table Element:"));
      ImGui::BeginGroup();
      ImGui::BeginChild("##ts_elems", ImVec2(S(220), S(200)), ImGuiChildFlags_Borders);
      for (int e = 0; e < model::kTeCount; ++e) {
        // an element with a format of its own is bold in the list, as in Excel
        if (!u.ts.el[e].Empty()) ImGui::PushFont(ui::F().ui_bold, ui::F().ui_size);
        if (ImGui::Selectable(kElems[e], u.ts_element == e)) u.ts_element = e;
        ui::NoteUiItem(kElems[e], ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        if (!u.ts.el[e].Empty()) ImGui::PopFont();
      }
      ImGui::EndChild();
      if (ImGui::Button(TR("Format"), ImVec2(S(90), 0))) {
        // Format Cells with Font, Border and Fill; the dialog comes back when it closes
        const int e = u.ts_element;
        const model::TableStyleElement& x = u.ts.el[e];
        engine::FormatSpec sp;
        sp.style.fill_color = x.fill;
        sp.style.font_color = x.font_color;
        sp.style.bold = x.bold == 1;
        sp.style.italic = x.italic == 1;
        sp.style.border[model::kTop] = x.top;
        sp.style.border[model::kBottom] = x.bottom;
        sp.style.border[model::kLeft] = x.left;
        sp.style.border[model::kRight] = x.right;
        if (x.fill) sp.mask |= engine::kFmFill;
        if (x.font_color) sp.mask |= engine::kFmFontColor;
        if (x.bold >= 0) sp.mask |= engine::kFmBold;
        if (x.italic >= 0) sp.mask |= engine::kFmItalic;
        ImGui::CloseCurrentPopup();
        u.ts_open = false;
        OpenFormatCellsFor(s, TR("Format Cells"), sp, 4u | 8u | 16u, x.inner_h, x.inner_v,
                           [&s, e](const engine::FormatSpec& out, model::BorderSide ih, model::BorderSide iv) {
                             TablesUi& w = T();
                             model::TableStyleElement& y = w.ts.el[e];
                             y.fill = (out.mask & engine::kFmFill) ? out.style.fill_color : 0;
                             y.font_color = (out.mask & engine::kFmFontColor) ? out.style.font_color : 0;
                             y.bold = (out.mask & engine::kFmBold) ? (out.style.bold ? 1 : 0) : -1;
                             y.italic = (out.mask & engine::kFmItalic) ? (out.style.italic ? 1 : 0) : -1;
                             y.top = out.style.border[model::kTop];
                             y.bottom = out.style.border[model::kBottom];
                             y.left = out.style.border[model::kLeft];
                             y.right = out.style.border[model::kRight];
                             y.inner_h = ih;
                             y.inner_v = iv;
                             w.ts_request = true;
                             (void)s;
                           });
        // reopen even when Format Cells is cancelled
        u.ts_request = true;
      }
      ImGui::SameLine();
      if (ImGui::Button(TR("Clear"), ImVec2(S(90), 0))) u.ts.el[u.ts_element] = model::TableStyleElement{};
      ImGui::EndGroup();
      ImGui::SameLine(0, S(20));
      ImGui::BeginGroup();
      ImGui::TextUnformatted(TR("Preview"));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const ImRect pr(p, ImVec2(p.x + S(220), p.y + S(170)));
      ImGui::GetWindowDrawList()->AddRectFilled(pr.Min, pr.Max, IM_COL32_WHITE);
      DrawStylePicture(ImGui::GetWindowDrawList(), pr, &u.ts, false, false, false, true);
      ImGui::GetWindowDrawList()->AddRect(pr.Min, pr.Max, IM_COL32(0xA0, 0xA0, 0xA0, 255));
      ImGui::Dummy(pr.GetSize());
      // Stripe Size: for the stripes
      const int e = u.ts_element;
      if (e >= model::kTeFirstColumnStripe && e <= model::kTeSecondRowStripe) {
        int* size = e <= model::kTeSecondColumnStripe ? &u.ts.col_stripe : &u.ts.row_stripe;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Stripe Size"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(80));
        if (ImGui::InputInt("##stripe", size)) *size = std::clamp(*size, 1, 9);
      }
      ImGui::EndGroup();
      ImGui::Checkbox(TR("Set as default table style for this document"), &u.ts_default);
      ImGui::Separator();
      bool ok = DlgOkButton(TR("OK"), ImVec2(S(80), 0));
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
      if (ok) {
        std::string name = u.ts_name;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        auto defs = CustomStyles(s);
        const bool taken = std::any_of(defs.begin(), defs.end(), [&](const model::TableStyleDef& x) { return x.name == name && x.name != u.ts_original; }) ||
                           model::BuiltinTableStyle(name) != nullptr;
        if (name.empty() || taken || name.find('|') != std::string::npos) {
          s.ShowError("YCGrid", TR("Table style name already exists or isn't valid. Type a different name."));
        } else {
          model::TableStyleDef d = u.ts;
          d.name = name;
          bool replaced = false;
          for (auto& x : defs) {
            if (!u.ts_original.empty() && x.name == u.ts_original) x = d, replaced = true;
          }
          if (!replaced) defs.push_back(d);
          SaveCustomStyles(s, defs, u.ts_original.empty() ? TR("New Table Style") : TR("Modify Table Style"));
          const std::string was = model::DefaultTableStyle(Store(s));
          if (u.ts_default) SetDefaultStyle(s, name);
          else if (!u.ts_original.empty() && was == u.ts_original) SetDefaultStyle(s, "");
          // tables using the old name follow the renamed style
          if (!u.ts_original.empty() && u.ts_original != name) {
            const std::string from = u.ts_original;
            s.engine->PostUser(TR("Modify Table Style"), [from, name](Document& doc) {
              for (const auto& sh : doc.wb().sheets) {
                for (const auto& t : *sh->tables) {
                  if (t.style == from) doc.UpdateTable(sh->id, t.id, [&](model::Table& x) { x.style = name; });
                }
              }
            });
          }
          close = true;
        }
      }
      if (close) {
        ImGui::CloseCurrentPopup();
        u.ts_open = false;
        u.ts_request = false;
      }
      ImGui::EndPopup();
    } else if (u.ts_open && !ImGui::IsPopupOpen(title)) {
      u.ts_open = false;
    }
  }
  // totals row: the function of one column
  ImGui::SetNextWindowPos(u.tot_pos, ImGuiCond_Appearing);
  if (ImGui::BeginPopupEx(kTotalsPopup, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
    struct F {
      const char* label;
      int func;
    };
    static const F kFuncs[] = {{TR("None##ctx_totals"), 0}, {TR("Average"), 101}, {TR("Count"), 103}, {TR("Count Numbers"), 102}, {TR("Max"), 104},
                               {TR("Min"), 105},          {TR("Sum"), 109},      {TR("StdDev"), 107}, {TR("Var"), 110}};
    // the column's function is marked, as in Excel's list
    int current = -1;
    if (const model::Sheet* tsh = s.snap ? s.snap->Sheet(u.tot_sheet) : nullptr) {
      for (const auto& t : *tsh->tables) {
        if (t.id == u.tot_id && u.tot_col < t.columns.size()) current = t.columns[u.tot_col].totals_func;
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    for (const F& f : kFuncs) {
      if (ImGui::Selectable(f.label, f.func == current)) {
        const uint32_t sheet = u.tot_sheet, id = u.tot_id, col = u.tot_col;
        const int fn = f.func;
        s.engine->PostUser(TR("Total Row"), [=](Document& d) { d.SetTableTotal(sheet, id, col, fn, std::string()); });
      }
    }
    // More Functions...: Insert Function on the totals cell
    if (ImGui::Selectable(TR("More Functions..."))) {
      ImGui::CloseCurrentPopup();
      ui::Commands().Execute("InsertFunction");
    }
    ImGui::EndPopup();
  }
}

// a drop-down button on the active cell when it is in a totals row
void DrawTableOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  DrawDataToolOverlay(s, sheet_id, g);  // the tables' resize handles, Flash Fill's preview and options button
  if (!s.snap || sheet_id != s.focus_sheet) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh || sh->tables->empty()) return;
  const auto& sel = s.FocusView().sel;
  const model::Table* t = model::TableAt(s.snap->wb, sheet_id, sel.act_r, sel.act_c);
  if (!t || !t->totals || sel.act_r != t->range.r1) return;
  const ImVec2 a = g.to_screen(static_cast<double>(sh->cols->Offset(sel.act_c)), static_cast<double>(sh->rows->Offset(sel.act_r)));
  const float w = static_cast<float>(sh->cols->Size(sel.act_c) * g.sc), h = static_cast<float>(sh->rows->Size(sel.act_r) * g.sc);
  const float bs = std::floor(std::min(h - 2, S(17)));
  const ImRect b(ImVec2(a.x + w + 2, a.y + h - bs - 1), ImVec2(a.x + w + 2 + bs, a.y + h - 1));
  if (!b.Overlaps(g.cells)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(b.Min);
  ImGui::PushID("##totals_btn");
  ImGui::InvisibleButton("##tot", ImMax(b.GetSize(), ImVec2(1, 1)));
  const bool hov = ImGui::IsItemHovered();
  // the button, or Alt+Down on the totals cell (Excel), opens the list
  const bool alt_down = !s.edit.active && ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) && !ImGui::GetIO().WantTextInput &&
                        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
  if (ImGui::IsItemClicked() || alt_down) {
    TablesUi& u = T();
    u.tot_sheet = sheet_id;
    u.tot_id = t->id;
    u.tot_col = sel.act_c - t->range.c0;
    u.tot_pos = ImVec2(b.Min.x, b.Max.y + 2);
    ImGui::OpenPopupEx(kTotalsPopup);
  }
  ImGui::PopID();
  dl->AddRectFilled(b.Min, b.Max, hov ? ui::T().btn_hover : IM_COL32_WHITE);
  dl->AddRect(b.Min, b.Max, IM_COL32(0xA6, 0xA6, 0xA6, 255));
  const ImVec2 c = b.GetCenter();
  const float k = bs * 0.22f;
  dl->AddTriangleFilled(ImVec2(c.x - k, c.y - k * 0.5f), ImVec2(c.x + k, c.y - k * 0.5f), ImVec2(c.x, c.y + k * 0.6f), IM_COL32(0x40, 0x40, 0x40, 255));
}

}  // namespace yc::app
