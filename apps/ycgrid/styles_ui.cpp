// SPDX-License-Identifier: Apache-2.0
// styles_ui.cpp - Home > Cell Styles (built-in and custom named styles) and Page Layout > Themes
#include "app_state.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "model/theme.h"
#include "io/ycg.h"
#include "ribbon/ribbon.h"
#include "yc_ui_items.h"

#include "imgui.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>

namespace yc::app {

using engine::Document;
using ui::S;

namespace {

// the display names of the built-in styles (the workbook keeps Excel's English names)
std::string StyleLabel(const std::string& name) {
  static const std::map<std::string, const char*> m = {
      {"Normal", TR("Normal")},           {"Bad", TR("Bad")},
      {"Good", TR("Good")},               {"Neutral", TR("Neutral")},
      {"Calculation", TR("Calculation##ctx_mode")}, {"Check Cell", TR("Check Cell")},
      {"Explanatory Text", TR("Explanatory Text")}, {"Hyperlink", TR("Hyperlink")},
      {"Input", TR("Input##ctx_style")},  {"Linked Cell", TR("Linked Cell")},
      {"Note", TR("Note##ctx_style")},    {"Output", TR("Output")},
      {"Warning Text", TR("Warning Text")}, {"Heading 1", TR("Heading 1")},
      {"Heading 2", TR("Heading 2")},     {"Heading 3", TR("Heading 3")},
      {"Heading 4", TR("Heading 4")},     {"Title", TR("Title##ctx_style")},
      {"Total", TR("Total")},             {"Comma", TR("Comma")},
      {"Comma [0]", TR("Comma [0]")},     {"Currency", TR("Currency")},
      {"Currency [0]", TR("Currency [0]")}, {"Percent", TR("Percent")},
  };
  if (auto it = m.find(name); it != m.end()) {
    // TR keeps the "##" context out of the shown text
    std::string t = it->second;
    if (auto k = t.find("##"); k != std::string::npos) t.erase(k);
    return t;
  }
  // "20% - Accent1" ... "Accent6"
  if (name.rfind("Accent", 0) == 0) return TR("Accent") + name.substr(6);
  if (name.size() > 6 && name.find(" - Accent") != std::string::npos) return name.substr(0, name.find(" - ")) + " - " + TR("Accent") + name.substr(name.size() - 1);
  return name;
}

std::string SectionLabel(const std::string& section) {
  if (section == "Good, Bad and Neutral") return TR("Good, Bad and Neutral");
  if (section == "Data and Model") return TR("Data and Model");
  if (section == "Titles and Headings") return TR("Titles and Headings");
  if (section == "Themed Cell Styles") return TR("Themed Cell Styles");
  if (section == "Number Format") return TR("Number Format");
  return TR("Custom");
}

// command id of a style: the ids Excel's built-in styles had before stay the same
std::string StyleCmd(const std::string& name, bool builtin) {
  static const std::map<std::string, const char*> legacy = {
      {"Normal", "CellStyleNormal"},    {"Bad", "CellStyleBad"},         {"Good", "CellStyleGood"},         {"Neutral", "CellStyleNeutral"},
      {"Calculation", "CellStyleCalculation"}, {"Check Cell", "CellStyleCheck"}, {"Explanatory Text", "CellStyleExplanatory"},
      {"Hyperlink", "CellStyleHyperlink"}, {"Input", "CellStyleInput"}, {"Note", "CellStyleNote"},  {"Output", "CellStyleOutput"},
      {"Warning Text", "CellStyleWarning"}, {"Heading 1", "CellStyleHeading1"}, {"Heading 2", "CellStyleHeading2"},
      {"Heading 3", "CellStyleHeading3"}, {"Heading 4", "CellStyleHeading4"}, {"Title", "CellStyleTitle"}, {"Total", "CellStyleTotal"},
  };
  if (builtin) {
    if (auto it = legacy.find(name); it != legacy.end()) return it->second;
    std::string id = "CellStyle";
    for (char c : name) {
      if (std::isalnum(static_cast<unsigned char>(c))) id += c;
      else if (c == '%') id += "Pct";
    }
    return id;
  }
  return "CellStyleCustom:" + name;
}

AppState* g_s = nullptr;
// every style the gallery shows: custom ones first, built-in ones in Excel's order (a changed built-in style replaces the original)
std::vector<model::NamedStyle> AllStyles(const AppState& s) {
  std::vector<model::NamedStyle> out;
  const auto& custom = *s.snap->wb.cell_styles;
  for (const auto& ns : custom) {
    if (!ns.builtin) out.push_back(ns);
  }
  for (model::NamedStyle b : model::BuiltinCellStyles()) {
    for (const auto& c : custom) {
      if (c.builtin && c.name == b.name) {
        const std::string sec = b.section, nf = b.number_format;
        b = c;
        b.section = sec;
        if (b.number_format.empty()) b.number_format = nf;
      }
    }
    out.push_back(std::move(b));
  }
  return out;
}

std::optional<model::NamedStyle> StyleForCmd(const AppState& s, const std::string& cmd) {
  for (const auto& ns : AllStyles(s)) {
    if (StyleCmd(ns.name, ns.builtin) == cmd) return ns;
  }
  return std::nullopt;
}

void ApplyNamed(AppState& s, const std::string& name) {
  if (s.edit.active || !s.engine) return;
  const auto areas = SelAreas(s);
  const uint32_t sheet = s.focus_sheet;
  // rows that size themselves grow with a bigger font (Title, Heading 1), as Excel's do
  std::vector<std::pair<uint32_t, int32_t>> fit, widen;
  for (const auto& ns : AllStyles(s)) {
    if (ns.name != name) continue;
    const model::NamedStyle look = ns;
    fit = FitRowsForStyle(s, sheet, areas, [look](model::Style& x) { model::ApplyNamedStyle(x, look, 0); });
    // a number style (Comma, Currency, Percent) widens a column nobody sized, as the number formats do (Excel 16)
    if (!look.number_format.empty() && look.number_format != "General") {
      widen = ColumnsToWidenForFormat(s, sheet, areas, look.number_format);
      for (const auto& [c, px] : widen) s.auto_widened[{sheet, c}] = px;
    }
    break;
  }
  s.engine->PostUser(TR("Cell style: ") + StyleLabel(name), [areas, sheet, name, fit, widen](Document& d) {
    for (const Rect& a : areas) d.ApplyCellStyle(sheet, a, name);
    for (const auto& [row, h] : fit) d.SetRowHeight(sheet, row, row, h);
    for (const auto& [c, px] : widen) d.SetColWidth(sheet, c, c, px);
  });
}

ImU32 ImCol(uint32_t c, ImU32 fallback) {
  c = model::ResolveColor(c);
  if (!(c >> 24)) return fallback;
  return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
}

// ---- the Style dialog (New Cell Style / Modify) ----
struct StyleDlg {
  std::string original;  // the style being modified ("" = a new one)
  char name[128] = {};
  model::NamedStyle ns;
  bool format_pending = false;  // Format... is open: the Format Cells dialog edits ns.style
};
StyleDlg g_dlg;

void RegisterStyleCommand(AppState& s, const model::NamedStyle& ns) {
  const std::string id = StyleCmd(ns.name, ns.builtin);
  if (ui::Commands().Find(id)) return;
  ui::Command c;
  c.id = id;
  c.label = ns.builtin ? StyleLabel(ns.name) : ns.name;
  const std::string name = ns.name;
  ui::Commands().Add(std::move(c)).execute = [&s, name](ui::Command&) { ApplyNamed(s, name); };
}

// gallery tiles: rebuilt when the theme or the workbook's styles change
const void* g_styles_seen = nullptr;
model::Theme g_theme_seen;
void RebuildGallery(AppState& s) {
  std::vector<ui::ribbon::GalleryTile> tiles;
  for (const auto& ns : AllStyles(s)) {
    RegisterStyleCommand(s, ns);
    // the Number Format styles get tiles too (Excel): right-click > Modify / Duplicate works on them as on the others
    model::Style st;
    model::ApplyNamedStyle(st, ns, 0);
    ui::ribbon::GalleryTile t;
    t.cmd = StyleCmd(ns.name, ns.builtin);
    t.section = SectionLabel(ns.builtin ? ns.section : std::string());
    t.fill = st.fill_color == model::kColorAuto ? 0 : ImCol(st.fill_color, 0);
    t.text = ImCol(st.font_color, IM_COL32(0, 0, 0, 255));
    t.bold = st.bold;
    t.italic = st.italic;
    t.underline = st.underline != model::Underline::None;
    t.font_scale = st.font_size > 0 ? st.font_size / 11.0f : 1.0f;
    const int edges[4] = {model::kLeft, model::kTop, model::kRight, model::kBottom};
    for (int i = 0; i < 4; ++i) {
      const model::BorderSide& b = st.border[edges[i]];
      auto& out = t.edge[i];
      out.col = ImCol(b.color, IM_COL32(0, 0, 0, 255));
      switch (b.style) {
        case model::LineStyle::None: out.width = 0; break;
        case model::LineStyle::Double: out.width = 1; out.dbl = true; break;
        case model::LineStyle::Thick: out.width = 3; break;
        case model::LineStyle::Medium:
        case model::LineStyle::MediumDashed:
        case model::LineStyle::MediumDashDot:
        case model::LineStyle::MediumDashDotDot: out.width = 2; break;
        default: out.width = 1; break;
      }
    }
    tiles.push_back(std::move(t));
  }
  ui::ribbon::SetGallery("CellStyles", std::move(tiles));
  ui::ribbon::SetGalleryFooter("CellStyles", {"NewCellStyle", "MergeStyles"});
}

std::string Describe(const AppState& s, const model::NamedStyle& ns, int cat) {
  const auto& t = *s.snap->wb.styles;
  const model::Style& st = ns.style;
  char b[256];
  switch (cat) {
    case 0: return ns.number_format.empty() ? t.NumberFormat(st) : ns.number_format;
    case 1: {
      const char* h[] = {TR("General"), TR("Left (Indent)"), TR("Center"), TR("Right (Indent)"), TR("Fill"), TR("Justify"), TR("Center Across Selection"), TR("Distributed (Indent)")};
      const char* v[] = {TR("Bottom Aligned"), TR("Center Aligned"), TR("Top Aligned"), TR("Justify"), TR("Distributed")};
      std::snprintf(b, sizeof b, "%s, %s", h[static_cast<int>(st.halign) % 8], v[static_cast<int>(st.valign) % 5]);
      return b;
    }
    case 2: {
      std::snprintf(b, sizeof b, "%s %.0f%s%s", t.FontName(st).c_str(), t.FontSize(st), st.bold ? TR(", Bold") : "", st.italic ? TR(", Italic") : "");
      return b;
    }
    case 3: {
      bool any = false;
      for (const auto& e : st.border) any = any || e.style != model::LineStyle::None;
      return any ? TR("Borders") : TR("No Borders");
    }
    case 4: return st.HasFill() ? TR("Shaded") : TR("No Shading");
    default: return st.locked ? (st.hidden ? TR("Locked, Hidden") : TR("Locked")) : TR("Unlocked");
  }
}

}  // namespace

model::Style* StyleFormatTarget() { return g_dlg.format_pending ? &g_dlg.ns.style : nullptr; }
std::string StyleFormatCode() { return g_dlg.format_pending ? g_dlg.ns.number_format : std::string(); }

void StyleFormatDone(AppState& s, bool ok) {
  if (!g_dlg.format_pending) return;
  g_dlg.format_pending = false;
  (void)ok;
  s.OpenDialog(Dialog::CellStyle);  // back to the Style dialog
}

// Merge Styles: the custom (and changed built-in) cell styles of another open workbook, read from its file
struct MergeDlg {
  bool request = false, open = false;
  std::vector<OtherWorkbook> books;
  int pick = 0;
} g_merge;

// asks another running YCGrid for its cell styles (YCP "cellstyle.export": a small .ycg of its styles)
bool StylesFromInstance(const std::string& pipe, std::string* bytes) {
  if (pipe.empty()) return false;
  HANDLE h = ::CreateFileW(FromUtf8(pipe).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  auto send = [&](int id, const char* method) {
    io::MsgWriter w;
    w.Array(4);
    w.Int(0);
    w.Int(id);
    w.Str(method);
    w.Map(0);
    const uint32_t n = static_cast<uint32_t>(w.buf().size());
    std::string frame(reinterpret_cast<const char*>(&n), 4);
    frame += w.buf();
    DWORD done = 0;
    return ::WriteFile(h, frame.data(), static_cast<DWORD>(frame.size()), &done, nullptr) && done == frame.size();
  };
  auto read_exact = [&](char* p, DWORD n) {
    while (n) {
      DWORD got = 0;
      if (!::ReadFile(h, p, n, &got, nullptr) || !got) return false;
      p += got;
      n -= got;
    }
    return true;
  };
  // the reply to id (notifications and other replies are skipped); up to 5 seconds for it to start
  auto reply = [&](int id, io::MV* out) {
    const ULONGLONG until = ::GetTickCount64() + 5000;
    while (true) {
      DWORD avail = 0;
      while (::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail < 4) {
        if (::GetTickCount64() > until) return false;
        ::Sleep(5);
      }
      uint32_t n = 0;
      if (!read_exact(reinterpret_cast<char*>(&n), 4) || n > (256u << 20)) return false;
      std::string data(n, '\0');
      if (!read_exact(data.data(), n)) return false;
      io::MsgReader r(data);
      io::MV mv;
      if (!io::Decode(r, &mv) || mv.a.size() < 4) continue;
      if (mv.a[0].i == 1 && mv.a[1].i == id) {
        if (!mv.a[2].IsNil()) return false;
        *out = mv.a[3];
        return true;
      }
    }
  };
  io::MV hello, styles;
  const bool ok = send(1, "hello") && reply(1, &hello) && send(2, "cellstyle.export") && reply(2, &styles) && !styles.s.empty();
  if (ok) *bytes = styles.s;
  ::CloseHandle(h);
  return ok;
}

void MergeStylesFrom(AppState& s, const std::wstring& file, bool ask, const std::string& pipe) {
  std::string bytes;
  if (!StylesFromInstance(pipe, &bytes)) {
    if (file.empty()) {
      s.ShowError("YCGrid", TR("The workbook couldn't be read."));
      return;
    }
    std::ifstream f(std::filesystem::path(file), std::ios::binary);
    if (!f) {
      s.ShowError("YCGrid", TR("The workbook couldn't be read."));
      return;
    }
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  io::LoadResult lr = io::LoadYcgFromBytes(bytes, std::string());
  if (!lr.ok) {
    s.ShowError("YCGrid", TR("The workbook couldn't be read."));
    return;
  }
  // the styles as they are there: number format codes and fonts carried by name
  std::vector<model::NamedStyle> styles;
  std::vector<std::string> fonts;  // each style's font (by name: the string tables differ)
  const auto& t = *lr.wb.styles;
  for (model::NamedStyle ns : *lr.wb.cell_styles) {
    if (ns.style.numfmt && ns.number_format.empty()) ns.number_format = t.Str(ns.style.numfmt);
    fonts.push_back(ns.style.font_name ? t.Str(ns.style.font_name) : std::string());
    ns.style.numfmt = 0;
    ns.style.font_name = 0;
    styles.push_back(std::move(ns));
  }
  if (styles.empty()) {
    s.ShowMessage("YCGrid", TR("That workbook has no styles of its own to merge."));
    return;
  }
  bool clash = false;
  for (const auto& ns : styles) {
    for (const auto& mine : *s.snap->wb.cell_styles) clash = clash || mine.name == ns.name;
    for (const auto& b : model::BuiltinCellStyles()) clash = clash || (ns.builtin && b.name == ns.name);
  }
  auto merge = [&s, styles, fonts](bool same_names) {
    std::vector<std::pair<model::NamedStyle, std::string>> take;
    for (size_t i = 0; i < styles.size(); ++i) {
      const auto& ns = styles[i];
      bool exists = ns.builtin;
      for (const auto& mine : *s.snap->wb.cell_styles) exists = exists || mine.name == ns.name;
      if (!exists || same_names) take.emplace_back(ns, fonts[i]);
    }
    s.engine->PostUser(TR("Merge Styles"), [take](Document& d) {
      for (auto [ns, font] : take) {
        if (!font.empty()) ns.style.font_name = d.wb_mut().styles.mut().InternString(font);
        d.SaveCellStyle(ns);
      }
    });
  };
  if (!clash || !ask) {
    merge(!ask);
    return;
  }
  // Excel: "Merge styles that have same names?" Yes replaces them, No keeps this workbook's
  AskYesNo(s, "YCGrid", TR("Merge styles that have same names?"), [merge] { merge(true); });
  merge(false);
}

std::function<void(model::Style&)> CellStylePreview(const std::string& cmd) {
  if (!g_s || !g_s->snap) return {};
  auto ns = StyleForCmd(*g_s, cmd);
  if (!ns) return {};
  return [n = *ns](model::Style& x) {
    model::NamedStyle preview = n;
    if (!preview.number_format.empty()) preview.includes &= ~model::kIncNumber;  // codes are interned when applied
    model::ApplyNamedStyle(x, preview, x.cell_style);
  };
}

void RegisterStyleCommands(AppState& s) {
  g_s = &s;
  auto& R = ui::Commands();
  auto make = [](const char* id, const char* label, const char* icon = nullptr, const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  for (const auto& ns : model::BuiltinCellStyles()) RegisterStyleCommand(s, ns);
  R.Add(make("CellStyles", TR("Cell\nStyles"), ui::icon::CellStyles, TR("Apply built-in or custom cell styles such as Good, Bad, Heading 1 and Total."))).execute =
      [](ui::Command&) { ui::ribbon::OpenGallery("CellStyles"); };  // Excel: the gallery opens
  R.Add(make("MergeStyles", TR("Merge Styles..."), nullptr, TR("Copy the cell styles of another open workbook into this one."))).execute = [](ui::Command&) {
    g_merge = MergeDlg{};
    g_merge.books = OtherWorkbooks();
    g_merge.request = true;
  };
  R.Add(make("NewCellStyle", TR("New Cell Style..."), nullptr, TR("Save the formats of the active cell as a named style."))).execute = [&s](ui::Command&) {
    const model::Sheet* sh = s.FocusSheet();
    if (!sh) return;
    const auto& sel = s.FocusView().sel;
    g_dlg = StyleDlg{};
    g_dlg.ns.style = s.snap->wb.styles->Get(sh->GetStyle(sel.act_r, sel.act_c));
    g_dlg.ns.style.cell_style = 0;
    g_dlg.ns.includes = model::kIncAll;
    int k = 1;
    for (;; ++k) {
      bool used = false;
      for (const auto& ns : AllStyles(s)) used = used || ns.name == "Style " + std::to_string(k);
      if (!used) break;
    }
    std::snprintf(g_dlg.name, sizeof g_dlg.name, "Style %d", k);
    s.OpenDialog(Dialog::CellStyle);
  };
  // right-click on a style tile: Apply / Modify / Duplicate / Delete
  ui::ribbon::St().tile_menu = [&s, prev = ui::ribbon::St().tile_menu](const std::string& gallery, const std::string& cmd) {
    if (gallery != "CellStyles") {
      if (prev) prev(gallery, cmd);
      return;
    }
    const auto ns = StyleForCmd(s, cmd);
    if (!ns) return;
    if (ImGui::MenuItem(TR("Apply"))) ApplyNamed(s, ns->name);
    if (ImGui::MenuItem(TR("Modify..."))) {
      g_dlg = StyleDlg{};
      g_dlg.original = ns->name;
      g_dlg.ns = *ns;
      std::snprintf(g_dlg.name, sizeof g_dlg.name, "%s", ns->builtin ? StyleLabel(ns->name).c_str() : ns->name.c_str());
      s.OpenDialog(Dialog::CellStyle);
    }
    if (ImGui::MenuItem(TR("Duplicate..."))) {
      g_dlg = StyleDlg{};
      g_dlg.ns = *ns;
      g_dlg.ns.builtin = false;
      std::snprintf(g_dlg.name, sizeof g_dlg.name, "%s 2", ns->builtin ? StyleLabel(ns->name).c_str() : ns->name.c_str());
      s.OpenDialog(Dialog::CellStyle);
    }
    if (ImGui::MenuItem(TR("Delete"), nullptr, false, ns->name != "Normal")) {
      const std::string name = ns->name;
      s.engine->PostUser(TR("Delete Style"), [name](Document& d) { d.DeleteCellStyle(name); });
    }
  };

  // ---- Page Layout > Themes ----
  R.Add(make("ThemesMenu", TR("Themes"), ui::icon::Themes, TR("Change the overall design of the workbook: colors, fonts and the cell styles that use them."))).execute =
      [](ui::Command&) {};
  R.Add(make("ThemeColorsMenu", TR("Colors"), ui::icon::ThemeColors, TR("Change the theme colors."))).execute = [](ui::Command&) {};
  R.Add(make("ThemeFontsMenu", TR("Fonts"), ui::icon::ThemeFonts, TR("Change the theme fonts."))).execute = [](ui::Command&) {};
  auto set_theme = [&s](std::function<void(model::Theme&)> fn) {
    model::Theme t = *s.snap->wb.theme;
    fn(t);
    s.engine->PostUser(TR("Theme"), [t](Document& d) { d.SetTheme(t); });
  };
  const auto& themes = model::BuiltinThemes();
  for (size_t i = 0; i < themes.size(); ++i) {
    const std::string id = "ThemeApply" + std::to_string(i);
    R.Add(make(id.c_str(), themes[i].name.c_str())).execute = [set_theme, i](ui::Command&) {
      set_theme([i](model::Theme& t) { t = model::BuiltinThemes()[i]; });
    };
  }
  const auto& colors = model::BuiltinThemeColors();
  for (size_t i = 0; i < colors.size(); ++i) {
    const std::string id = "ThemeColors" + std::to_string(i);
    R.Add(make(id.c_str(), colors[i].name.c_str())).execute = [set_theme, i](ui::Command&) {
      set_theme([i](model::Theme& t) {
        t.colors = model::BuiltinThemeColors()[i].colors;
        t.colors_name = model::BuiltinThemeColors()[i].name;
      });
    };
  }
  const auto& fonts = model::BuiltinThemeFonts();
  for (size_t i = 0; i < fonts.size(); ++i) {
    const std::string id = "ThemeFonts" + std::to_string(i);
    const std::string label = fonts[i].name + "  (" + fonts[i].major + " / " + fonts[i].minor + ")";
    R.Add(make(id.c_str(), label.c_str())).execute = [set_theme, i](ui::Command&) {
      set_theme([i](model::Theme& t) {
        t.major_font = model::BuiltinThemeFonts()[i].major;
        t.minor_font = model::BuiltinThemeFonts()[i].minor;
        t.fonts_name = model::BuiltinThemeFonts()[i].name;
      });
    };
  }
}

std::vector<std::string> ThemeMenuIds(int which) {
  std::vector<std::string> v;
  const size_t n = which == 0 ? model::BuiltinThemes().size() : which == 1 ? model::BuiltinThemeColors().size() : model::BuiltinThemeFonts().size();
  const char* prefix = which == 0 ? "ThemeApply" : which == 1 ? "ThemeColors" : "ThemeFonts";
  for (size_t i = 0; i < n; ++i) v.push_back(prefix + std::to_string(i));
  return v;
}

void UpdateStylesUi(AppState& s) {
  if (!s.snap) return;
  const model::Theme& th = *s.snap->wb.theme;
  const void* styles = &*s.snap->wb.cell_styles;
  const bool theme_changed = !(th == g_theme_seen);
  if (theme_changed) {
    model::SetCurrentTheme(th);
    g_theme_seen = th;
  }
  if (theme_changed || styles != g_styles_seen) {
    g_styles_seen = styles;
    RebuildGallery(s);
  }
  // the checked entry of the theme menus
  const auto& themes = model::BuiltinThemes();
  for (size_t i = 0; i < themes.size(); ++i) {
    if (auto* c = ui::Commands().Find("ThemeApply" + std::to_string(i))) c->pressed = themes[i].name == th.name;
  }
}

void DrawStyleDialogs(AppState& s) {
  // Merge Styles: "Merge styles from:" the other open workbooks (Excel's list)
  {
    const char* title = TR("Merge Styles###yc_dlg_mergestyles");
    if (g_merge.request && s.dialog == Dialog::None) {
      ImGui::OpenPopup(title);
      g_merge.request = false;
      g_merge.open = true;
    }
    ImGui::SetNextWindowSize(ImVec2(ui::S(320), 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (g_merge.open && BeginModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
      bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
      ImGui::TextUnformatted(TR("Merge styles from:"));
      ImGui::BeginChild("##books", ImVec2(ui::S(300), ui::S(140)), ImGuiChildFlags_Borders);
      if (g_merge.books.empty()) ImGui::TextDisabled("%s", TR("No other workbook is open."));
      for (int i = 0; i < static_cast<int>(g_merge.books.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::Selectable(g_merge.books[static_cast<size_t>(i)].name.c_str(), g_merge.pick == i)) g_merge.pick = i;
        ui::NoteUiItem(g_merge.books[static_cast<size_t>(i)].name.c_str(), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()));
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::BeginDisabled(g_merge.books.empty());
      const bool ok = ImGui::Button(TR("OK"), ImVec2(ui::S(80), 0)) || (!g_merge.books.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter));
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button(TR("Cancel"), ImVec2(ui::S(80), 0))) close = true;
      OtherWorkbook from;
      bool merge = false;
      if (ok) {
        from = g_merge.books[static_cast<size_t>(std::clamp(g_merge.pick, 0, static_cast<int>(g_merge.books.size()) - 1))];
        merge = true;
        close = true;
      }
      if (close) {
        ImGui::CloseCurrentPopup();
        g_merge.open = false;
      }
      ImGui::EndPopup();
      if (merge) MergeStylesFrom(s, from.file, true, from.pipe);
    } else if (g_merge.open && !ImGui::IsPopupOpen(title)) {
      g_merge.open = false;
    }
  }
  if (s.dialog != Dialog::CellStyle) return;
  if (!ImGui::IsPopupOpen(TR("Style")) && !s.dialog_open_request) {
    // its popup went away without the dialog closing it: closed, not a dialog nobody can see or close
    s.dialog = Dialog::None;
    return;
  }
  if (s.dialog_open_request) {
    ImGui::OpenPopup(TR("Style"));
    s.dialog_open_request = false;
  }
  ImGui::SetNextWindowSize(ImVec2(S(460), 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  bool open = true;
  if (!BeginModal(TR("Style"), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
  StyleDlg& d = g_dlg;
  bool close = !open || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::TextUnformatted(TR("Style name:"));
  ImGui::SetNextItemWidth(S(300));
  ImGui::BeginDisabled(!d.original.empty());  // like Excel, Modify keeps the name
  ImGui::InputText("##name", d.name, sizeof d.name);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(TR("Format..."))) {
    d.format_pending = true;
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    s.format_tab = 0;
    s.OpenDialog(Dialog::FormatCells);
    return;
  }
  ImGui::Separator();
  ImGui::TextUnformatted(TR("Style Includes (By Example)"));
  const char* cats[] = {TR("Number"), TR("Alignment"), TR("Font"), TR("Border"), TR("Fill"), TR("Protection")};
  for (int i = 0; i < 6; ++i) {
    bool on = (d.ns.includes >> i) & 1;
    ImGui::PushID(i);
    if (ImGui::Checkbox(cats[i], &on)) d.ns.includes = static_cast<uint8_t>(on ? d.ns.includes | (1 << i) : d.ns.includes & ~(1 << i));
    ImGui::SameLine(S(140));
    ImGui::TextDisabled("%s", Describe(s, d.ns, i).c_str());
    ImGui::PopID();
  }
  ImGui::Separator();
  const float w = S(88);
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w * 2 - S(24));
  if (DlgOkButton(TR("OK"), ImVec2(w, 0))) {
    std::string name = d.original.empty() ? std::string(d.name) : d.original;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    bool clash = false;
    if (d.original.empty()) {
      for (const auto& ns : AllStyles(s)) clash = clash || engine::Lower(ns.name) == engine::Lower(name) || StyleLabel(ns.name) == name;
    }
    if (name.empty() || clash) {
      s.ShowMessage("YCGrid", TR("A style with this name already exists, or the name is empty."));
    } else {
      model::NamedStyle ns = d.ns;
      ns.name = name;
      s.engine->PostUser(d.original.empty() ? TR("New Cell Style") : TR("Modify Cell Style"), [ns](Document& doc) { doc.SaveCellStyle(ns); });
      close = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(w, 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

}  // namespace yc::app
