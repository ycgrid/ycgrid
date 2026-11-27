// SPDX-License-Identifier: Apache-2.0
// print_ui.cpp - File -> Print (with the preview), Export -> PDF, the Page Layout tab, Page Setup and page-break lines
#include "print.h"

#include "app_state.h"
#include "settings.h"
#include "core/i18n.h"

#include "commands/command_registry.h"
#include "fonts/fonts.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "yc_ui_items.h"

#include "imgui.h"

#include <shobjidl.h>
#include <windows.h>
#include <winspool.h>

#include <nlohmann/json.hpp>

#include "model/shape_effects.h"
#include "model/theme.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>

namespace yc::app {

void ArrangeOsWindows(AppState& s, int mode);  // windows_ui.cpp
using engine::Document;
using ui::S;

namespace {

struct PrintUi {
  // File -> Print
  int what = 0;  // 0 active sheet, 1 entire workbook, 2 selection
  bool ignore_area = false;
  int copies = 1, from = 1, to = 0;
  bool collate = true;
  int printer = -1;
  std::vector<std::wstring> printers;
  std::wstring default_printer;
  int page = 0;  // preview page (0-based, whole job)
  // preview texture
  ImTextureID tex{};
  int tex_w = 0, tex_h = 0;
  std::string tex_key;
  // the page-break lines' layout (shown on the sheets whose SheetWin::page_breaks is on)
  uint64_t breaks_version = ~0ull;
  uint32_t breaks_sheet = 0;
  engine::PrintLayout breaks_layout;
  // what the layout was made from: the same objects keep it, and only the cells' values (the used range) refresh it on
  // a clock, since a live feed brings a snapshot every frame
  std::array<const void*, 6> breaks_from{};
  double breaks_at = 0, breaks_cost = 0;  // when the layout was made, and how long it took (seconds)
  // Page Setup dialog
  model::PageSetup edit;
  uint32_t edit_sheet = 0;
  int tab = 0;
  char area[512] = "", title_rows[64] = "", title_cols[64] = "";
  char hf[6][256] = {};
  char hf_first[6][256] = {}, hf_even[6][256] = {};  // Different first page / odd and even pages
  int hf_page = 0;  // the custom header / footer being typed: 0 the (odd) pages, 1 the first page, 2 even pages
  bool first_auto = true;
  int margin_focus = -1;  // Margins tab: the box being typed in (its line is shown on the page picture)
  bool fit_error = false;  // OK found margins that leave no room on the paper
  // File -> Print: Print One Sided / on Both Sides, Printer Properties (the driver's DEVMODE), Show Margins, the page box
  int duplex = 0;
  std::vector<BYTE> devmode;
  std::wstring devmode_printer;
  bool show_margins = false;
  int margin_drag = -1;  // 0 left, 1 right, 2 top, 3 bottom, 4 header, 5 footer
  double margin_drag_value = 0;
  int page_box = 1;
  int chart = -1;  // Print Selected Chart: the chart selected when Print opened (index in the sheet's charts)
};

PrintUi& U() {
  static PrintUi u;
  return u;
}

// paper sizes Excel lists without a printer; with one, the printer's own papers (DeviceCapabilities)
struct Paper {
  int code;
  const char* name;
};
const Paper kPapers[] = {{1, "Letter"}, {5, "Legal"}, {7, "Executive"}, {3, "Tabloid"}, {8, "A3"}, {9, "A4"}, {11, "A5"}, {12, "B4 (JIS)"}, {13, "B5 (JIS)"}};

// Windows' measurement system: centimeters (metric) or inches, as Excel's Page Setup uses
bool MetricUnits() {
  DWORD v = 0;
  if (::GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&v), sizeof(v) / sizeof(wchar_t)) == 0) return false;
  return v == 0;
}

// a paper's name as Excel shows it (translated for the papers Windows knows), and its size in the user's units
const char* KnownPaperName(int code) {
  switch (code) {
    case 1: return TR("Letter");
    case 2: return TR("Letter Small");
    case 3: return TR("Tabloid");
    case 4: return TR("Ledger");
    case 5: return TR("Legal");
    case 6: return TR("Statement");
    case 7: return TR("Executive");
    case 8: return "A3";
    case 9: return "A4";
    case 10: return TR("A4 Small");
    case 11: return "A5";
    case 12: return TR("B4 (JIS)");
    case 13: return TR("B5 (JIS)");
    case 14: return TR("Folio");
    case 15: return TR("Quarto");
    case 16: return "10x14";
    case 17: return "11x17";
    case 18: return TR("Note");
    case 20: return TR("Envelope #10");
    case 27: return TR("Envelope DL");
    case 28: return TR("Envelope C5");
    case 34: return TR("Envelope B5");
    case 37: return TR("Envelope Monarch");
    case 43: return TR("Japanese Postcard");
    case 70: return "A6";
    case 88: return TR("B6 (JIS)");
    default: return nullptr;
  }
}

std::string PaperSizeText(double w_pt, double h_pt) {
  char b[64];
  if (MetricUnits()) std::snprintf(b, sizeof b, "%.3g cm x %.3g cm", w_pt / 72.0 * 2.54, h_pt / 72.0 * 2.54);
  else std::snprintf(b, sizeof b, "%.3g\" x %.3g\"", w_pt / 72.0, h_pt / 72.0);
  return b;
}

struct PaperEntry {
  int code;
  std::string name;  // "A4  21 cm x 29.7 cm"
};

// the papers of the default printer (or Excel's list without one); looked up once on a worker thread
std::vector<PaperEntry> LoadPapers() {
  std::vector<PaperEntry> out;
  std::wstring def;
  PrinterNames(&def);
  std::vector<WORD> codes;
  std::vector<wchar_t> names;
  std::vector<POINT> sizes;
  if (!def.empty()) {
    const int n = ::DeviceCapabilitiesW(def.c_str(), nullptr, DC_PAPERS, nullptr, nullptr);
    if (n > 0 && n < 1000) {
      codes.resize(static_cast<size_t>(n));
      names.resize(static_cast<size_t>(n) * 64);
      sizes.resize(static_cast<size_t>(n));
      if (::DeviceCapabilitiesW(def.c_str(), nullptr, DC_PAPERS, reinterpret_cast<LPWSTR>(codes.data()), nullptr) != n ||
          ::DeviceCapabilitiesW(def.c_str(), nullptr, DC_PAPERNAMES, names.data(), nullptr) != n ||
          ::DeviceCapabilitiesW(def.c_str(), nullptr, DC_PAPERSIZE, reinterpret_cast<LPWSTR>(sizes.data()), nullptr) != n)
        codes.clear();
    }
  }
  std::mutex* mu = nullptr;
  auto& registry = model::PrinterPaperSizes(&mu);
  for (size_t i = 0; i < codes.size(); ++i) {
    const int code = codes[i];
    const double w = sizes[i].x / 254.0 * 72.0, h = sizes[i].y / 254.0 * 72.0;  // tenths of a millimeter
    if (w <= 0 || h <= 0) continue;
    {
      std::lock_guard<std::mutex> g(*mu);
      registry[code] = {w, h};
    }
    const char* known = KnownPaperName(code);
    std::wstring wn(names.data() + i * 64, wcsnlen(names.data() + i * 64, 64));
    out.push_back({code, (known ? std::string(known) : ToUtf8(wn)) + "  " + PaperSizeText(w, h)});
  }
  if (out.empty()) {
    for (const Paper& p : kPapers) {
      double w = 0, h = 0;
      model::PaperSize(p.code, &w, &h);
      out.push_back({p.code, std::string(KnownPaperName(p.code)) + "  " + PaperSizeText(w, h)});
    }
  }
  return out;
}

std::vector<PaperEntry>& Papers() {
  static std::vector<PaperEntry> papers = [] {
    std::vector<PaperEntry> v;
    for (const Paper& p : kPapers) {
      double w = 0, h = 0;
      model::PaperSize(p.code, &w, &h);
      v.push_back({p.code, std::string(KnownPaperName(p.code)) + "  " + PaperSizeText(w, h)});
    }
    return v;
  }();
  return papers;
}

void ShowBreaks(AppState& s, uint32_t sheet) {
  s.Win(sheet).page_breaks = true;
  U().breaks_version = ~0ull;
  U().breaks_from = {};
}

void EditSetup(AppState& s, const char* label, std::function<void(model::PageSetup&)> fn) {
  const uint32_t sheet = s.focus_sheet;
  s.engine->PostUser(label, [sheet, fn](Document& d) { d.UpdatePageSetup(sheet, fn); });
  ShowBreaks(s, sheet);
}

const model::PageSetup* ActiveSetup(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  return sh ? &*sh->page_setup : nullptr;
}

std::wstring PickPdf(HWND owner, const std::wstring& initial) {
  if (std::wstring a; NoFileDialog(owner, TR("Export to PDF"), &a)) return a;
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::wstring result;
  IFileSaveDialog* dlg = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    const COMDLG_FILTERSPEC types[] = {{L"PDF (*.pdf)", L"*.pdf"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"pdf");
    dlg->SetFileName(initial.c_str());
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

std::vector<uint32_t> JobSheets(AppState& s, int what) {
  std::vector<uint32_t> ids;
  if (what == 1) {
    for (const auto& sh : s.snap->wb.sheets) {
      if (sh->visible) ids.push_back(sh->id);
    }
  } else {
    ids.push_back(s.focus_sheet);
  }
  return ids;
}

std::vector<PrintSheetJob> CurrentJobs(AppState& s) {
  PrintUi& u = U();
  if (u.what == 0 && u.chart >= 0) {
    auto jobs = BuildChartJob(*s.snap, s.focus_sheet, u.chart);
    if (!jobs.empty()) return jobs;
  }
  const Rect sel = SelRect(s);
  return BuildPrintJobs(*s.snap, JobSheets(s, u.what), u.what == 2 ? &sel : nullptr, u.ignore_area);
}

void RefreshPrinters() {
  PrintUi& u = U();
  u.printers = PrinterNames(&u.default_printer);
  if (u.printer < 0 || u.printer >= static_cast<int>(u.printers.size())) {
    u.printer = 0;
    for (size_t i = 0; i < u.printers.size(); ++i) {
      if (u.printers[i] == u.default_printer) u.printer = static_cast<int>(i);
    }
  }
}

std::string RowsText(bool has, uint32_t a, uint32_t b) { return has ? "$" + std::to_string(a + 1) + ":$" + std::to_string(b + 1) : std::string(); }
std::string ColsText(bool has, uint32_t a, uint32_t b) { return has ? "$" + core::ColumnName(a) + ":$" + core::ColumnName(b) : std::string(); }

// "$1:$2" / "A:B" / "" -> span; false when it can't be read
bool ParseSpan(AppState& s, uint32_t sheet, const char* text, bool rows, bool* has, uint32_t* a, uint32_t* b) {
  std::string t = text;
  t.erase(std::remove_if(t.begin(), t.end(), [](char ch) { return ch == ' '; }), t.end());
  if (t.empty()) {
    *has = false;
    return true;
  }
  uint32_t sid = 0;
  Rect r;
  if (!ResolveRangeText(s.snap->wb, sheet, t, &sid, &r)) return false;  // the sheet being set up, not the active one
  *has = true;
  *a = rows ? r.r0 : r.c0;
  *b = rows ? r.r1 : r.c1;
  return true;
}

// ---- Header / Footer tab: Excel's preset lists, a preview, and Custom Header / Footer with insert buttons ----
struct HfPreset {
  const char* l;
  const char* c;
  const char* r;
};

std::vector<HfPreset> HfPresets() {
  return {{"", "", ""},
          {"", TR("Page &P"), ""},
          {"", TR("Page &P of &N"), ""},
          {"", "&A", ""},
          {TR("Confidential"), "&D", TR("Page &P")},
          {"", "&F", ""},
          {"", "&Z&F", ""},
          {"", "&A", TR("Page &P")},
          {TR("Confidential"), "&A", TR("Page &P")},
          {"", "&F", TR("Page &P")},
          {"", "&Z&F", TR("Page &P")},
          {"", TR("Page &P"), "&A"},
          {"", TR("Page &P"), "&F"},
          {"", TR("Page &P"), "&Z&F"}};
}

std::string HfShown(AppState& s, PrintUi& u, const std::string& code) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(u.edit_sheet) : nullptr;
  std::string file = "Book1", path;
  if (!s.file_path.empty()) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.file_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string full(static_cast<size_t>(std::max(0, n - 1)), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.file_path.c_str(), -1, full.data(), n, nullptr, nullptr);
    const size_t slash = full.find_last_of("\\/");
    file = slash == std::string::npos ? full : full.substr(slash + 1);
    path = slash == std::string::npos ? std::string() : full.substr(0, slash + 1);
  }
  char date[32], time[32];
  const std::time_t now = std::time(nullptr);
  std::tm tmv{};
  localtime_s(&tmv, &now);
  std::strftime(date, sizeof date, "%Y/%m/%d", &tmv);
  std::strftime(time, sizeof time, "%H:%M", &tmv);
  return engine::ExpandHeaderFooter(code, 1, 1, file, path, sh ? sh->name : std::string(), date, time);
}

// the three parts as Excel shows them on the page
void HfPreview(AppState& s, PrintUi& u, int base) {
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = S(34);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p0 + ImVec2(w, h), IM_COL32_WHITE);
  dl->AddRect(p0, p0 + ImVec2(w, h), IM_COL32(0xB0, 0xB0, 0xB0, 255));
  const ImU32 col = IM_COL32(0x20, 0x20, 0x20, 255);
  for (int k = 0; k < 3; ++k) {
    const std::string tx = HfShown(s, u, u.hf[base + k]);
    const ImVec2 ts = ImGui::CalcTextSize(tx.c_str());
    const float x = k == 0 ? p0.x + S(8) : k == 1 ? p0.x + (w - ts.x) * 0.5f : p0.x + w - ts.x - S(8);
    dl->AddText(ImVec2(x, p0.y + (h - ts.y) * 0.5f), col, tx.c_str());
  }
  ImGui::Dummy(ImVec2(w, h));
}

// Header / Footer > Format Text: Excel's font dialog for the section; OK puts the codes in at the caret
// (&"Font,Bold Italic"&12, &U, &S, &Krrggbb)
void DrawHfFontPopup(const std::function<void(const std::string&)>& insert) {
  static char font[64] = "Calibri";
  static int style = 0, size = 11;
  static bool underline = false, strike = false;
  static float color[3] = {0, 0, 0};
  if (!ImGui::BeginPopup("##hffont")) return;
  ImGui::TextUnformatted(TR("Font:"));
  ImGui::SetNextItemWidth(S(220));
  if (ui::BeginCombo("##font", font)) {
    for (const std::string& f : ui::SystemFontFamilies()) {
      if (ImGui::Selectable(f.c_str(), f == font)) std::snprintf(font, sizeof font, "%s", f.c_str());
    }
    ImGui::EndCombo();
  }
  const char* styles[] = {TR("Regular"), TR("Italic"), TR("Bold"), TR("Bold Italic")};
  ImGui::SetNextItemWidth(S(140));
  ui::Combo(TR("Font style:"), &style, styles, 4);
  ImGui::SetNextItemWidth(S(100));
  if (ImGui::InputInt(TR("Size:"), &size)) size = std::clamp(size, 1, 409);
  ImGui::Checkbox(TR("Underline"), &underline);
  ImGui::SameLine();
  ImGui::Checkbox(TR("Strikethrough"), &strike);
  ImGui::ColorEdit3(TR("Color:"), color, ImGuiColorEditFlags_NoInputs);
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    static const char* const kStyles[] = {"Regular", "Italic", "Bold", "Bold Italic"};
    std::string code = std::string("&\"") + font + "," + kStyles[style] + "\"&" + std::to_string(size);
    if (underline) code += "&U";
    if (strike) code += "&S";
    const int rr = static_cast<int>(color[0] * 255 + 0.5f), gg = static_cast<int>(color[1] * 255 + 0.5f), bb = static_cast<int>(color[2] * 255 + 0.5f);
    if (rr || gg || bb) {
      char k[16];
      std::snprintf(k, sizeof k, "&K%02X%02X%02X", rr, gg, bb);
      code += k;
    }
    insert(code);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// Header / Footer > Format Picture: its height and width (Lock aspect ratio) or a scale of its own size
void DrawHfPicturePopup(model::PageSetup::HfPicture& pic) {
  if (!ImGui::BeginPopup("##hfpic")) return;
  int pw = 0, ph = 0;
  ImageSize(pic.image, &pw, &ph);
  const double nat_w = pw * 0.75, nat_h = ph * 0.75;  // its own size in points (96 dpi)
  static bool lock = true;
  const bool metric = MetricUnits();
  const double unit = metric ? 72.0 / 2.54 : 72.0;
  double w = (pic.w > 0 ? pic.w : nat_w) / unit, h = (pic.h > 0 ? pic.h : nat_h) / unit;
  ImGui::SeparatorText(TR("Size and rotate"));
  ImGui::SetNextItemWidth(S(110));
  if (ImGui::InputDouble(TR("Height:"), &h, 0.1, 1.0, metric ? "%.2f cm" : "%.2f\"") && h > 0) {
    const double old_h = pic.h > 0 ? pic.h : nat_h;
    pic.h = h * unit;
    if (lock && old_h > 0) pic.w = (pic.w > 0 ? pic.w : nat_w) * pic.h / old_h;
  }
  ImGui::SetNextItemWidth(S(110));
  if (ImGui::InputDouble(TR("Width:"), &w, 0.1, 1.0, metric ? "%.2f cm" : "%.2f\"") && w > 0) {
    const double old_w = pic.w > 0 ? pic.w : nat_w;
    pic.w = w * unit;
    if (lock && old_w > 0) pic.h = (pic.h > 0 ? pic.h : nat_h) * pic.w / old_w;
  }
  ImGui::SeparatorText(TR("Scale"));
  int sc = nat_h > 0 ? static_cast<int>(std::lround((pic.h > 0 ? pic.h : nat_h) / nat_h * 100)) : 100;
  ImGui::SetNextItemWidth(S(110));
  if (ImGui::InputInt(TR("% of its size"), &sc, 5, 25) && sc > 0 && nat_h > 0) {
    pic.h = nat_h * sc / 100.0;
    pic.w = nat_w * sc / 100.0;
  }
  ImGui::Checkbox(TR("Lock aspect ratio"), &lock);
  if (ImGui::Button(TR("Reset"))) pic.w = pic.h = 0;
  ImGui::SameLine();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void DrawHeaderFooterTab(AppState& s, PrintUi& u) {
  const auto presets = HfPresets();
  auto preset_label = [&](const HfPreset& p) {
    std::string t;
    for (const char* part : {p.l, p.c, p.r}) {
      if (!*part) continue;
      if (!t.empty()) t += ", ";
      t += HfShown(s, u, part);
    }
    return t.empty() ? std::string(TR("(none)")) : t;
  };
  // which preset the three parts are (else "custom": the parts as they are)
  auto current = [&](int base) {
    for (size_t k = 0; k < presets.size(); ++k) {
      if (presets[k].l == std::string(u.hf[base]) && presets[k].c == std::string(u.hf[base + 1]) && presets[k].r == std::string(u.hf[base + 2])) return static_cast<int>(k);
    }
    return -1;
  };
  static int custom = -1;  // 0 header, 1 footer: its three boxes are shown
  static int box = 0, caret = -1;  // the box the insert buttons go into, and its caret
  for (int part = 0; part < 2; ++part) {
    const int base = part * 3;
    ImGui::PushID(part);
    if (part == 0) HfPreview(s, u, 0);
    ImGui::TextUnformatted(part == 0 ? TR("Header:") : TR("Footer:"));
    const int cur = current(base);
    ImGui::SetNextItemWidth(-1);
    if (ui::BeginCombo("##preset", cur >= 0 ? preset_label(presets[static_cast<size_t>(cur)]).c_str() : TR("(custom)"))) {
      for (size_t k = 0; k < presets.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        if (ImGui::Selectable(preset_label(presets[k]).c_str(), cur == static_cast<int>(k))) {
          std::snprintf(u.hf[base], sizeof(u.hf[base]), "%s", presets[k].l);
          std::snprintf(u.hf[base + 1], sizeof(u.hf[base + 1]), "%s", presets[k].c);
          std::snprintf(u.hf[base + 2], sizeof(u.hf[base + 2]), "%s", presets[k].r);
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if (part == 0) {
      if (ImGui::Button(TR("Custom Header..."))) custom = custom == 0 ? -1 : 0;
      ImGui::SameLine();
      if (ImGui::Button(TR("Custom Footer..."))) custom = custom == 1 ? -1 : 1;
      if (custom >= 0) {
        // Excel's Custom Header / Footer: three sections and the buttons that put a code where the caret is; with
        // Different first page / odd and even pages, a tab for each kind of page
        const int cb = custom * 3;
        ImGui::SeparatorText(custom == 0 ? TR("Header") : TR("Footer"));
        if (u.edit.different_first || u.edit.different_odd_even) {
          if (BeginDlgTabBar("##hfpages")) {
            const char* names[3][2] = {{TR("Header"), TR("Footer")}, {TR("First Page Header"), TR("First Page Footer")}, {TR("Even Page Header"), TR("Even Page Footer")}};
            const char* odd[2] = {TR("Odd Page Header"), TR("Odd Page Footer")};
            for (int pg = 0; pg < 3; ++pg) {
              if (pg == 1 && !u.edit.different_first) continue;
              if (pg == 2 && !u.edit.different_odd_even) continue;
              const char* label = pg == 0 && u.edit.different_odd_even ? odd[custom] : names[pg][custom];
              if (ImGui::BeginTabItem(label)) {
                u.hf_page = pg;
                ImGui::EndTabItem();
              }
            }
            ImGui::EndTabBar();
          }
        } else {
          u.hf_page = 0;
        }
        char (*bufs)[256] = u.hf_page == 1 ? u.hf_first : u.hf_page == 2 ? u.hf_even : u.hf;
        struct Code {
          const char* label;
          const char* code;
        };
        const Code codes[] = {{TR("Page Number"), "&P"}, {TR("Number of Pages"), "&N"}, {TR("Date"), "&D"}, {TR("Time"), "&T"},
                              {TR("File Path"), "&Z&F"}, {TR("File Name"), "&F"}, {TR("Sheet Name"), "&A"}};
        for (size_t k = 0; k < std::size(codes); ++k) {
          if (k) ImGui::SameLine();
          if (ImGui::SmallButton(codes[k].label)) {
            std::string tx = bufs[cb + box];
            const size_t at = caret >= 0 && static_cast<size_t>(caret) <= tx.size() ? static_cast<size_t>(caret) : tx.size();
            tx.insert(at, codes[k].code);
            caret = static_cast<int>(at + std::strlen(codes[k].code));
            std::snprintf(bufs[cb + box], sizeof(bufs[cb + box]), "%s", tx.c_str());
          }
        }
        // Format Text, Insert Picture, Format Picture (Excel's first and last buttons of the row)
        auto insert_code = [&](const std::string& code) {
          std::string tx = bufs[cb + box];
          const size_t at = caret >= 0 && static_cast<size_t>(caret) <= tx.size() ? static_cast<size_t>(caret) : tx.size();
          tx.insert(at, code);
          caret = static_cast<int>(at + code.size());
          std::snprintf(bufs[cb + box], sizeof(bufs[cb + box]), "%s", tx.c_str());
        };
        if (ImGui::SmallButton(TR("Format Text"))) ImGui::OpenPopup("##hffont");
        DrawHfFontPopup(insert_code);
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Insert Picture"))) {
          std::string bytes;
          if (PickPictureFile(s, &bytes)) {
            auto& pic = u.edit.hf_pictures[cb + box];
            pic.image = std::move(bytes);
            pic.w = pic.h = 0;
            if (std::string(bufs[cb + box]).find("&G") == std::string::npos) insert_code("&G");
          }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(u.edit.hf_pictures[cb + box].image.empty());
        if (ImGui::SmallButton(TR("Format Picture"))) ImGui::OpenPopup("##hfpic");
        ImGui::EndDisabled();
        DrawHfPicturePopup(u.edit.hf_pictures[cb + box]);
        const char* sec[] = {TR("Left section:"), TR("Center section:"), TR("Right section:")};
        const float sw = (ImGui::GetContentRegionAvail().x - S(16)) / 3;
        for (int k = 0; k < 3; ++k) {
          if (k) ImGui::SameLine();
          ImGui::BeginGroup();
          ImGui::TextUnformatted(sec[k]);
          ImGui::PushID(10 + k + 100 * u.hf_page);
          struct Ctx {
            int k;
          } ctx{k};
          ImGui::InputTextMultiline("##sec", bufs[cb + k], sizeof(bufs[cb + k]), ImVec2(sw, S(60)), ImGuiInputTextFlags_CallbackAlways,
                                    [](ImGuiInputTextCallbackData* d) {
                                      box = static_cast<Ctx*>(d->UserData)->k;
                                      caret = d->CursorPos;
                                      return 0;
                                    },
                                    &ctx);
          ImGui::PopID();
          ImGui::EndGroup();
        }
        ImGui::TextDisabled("%s", TR("Click in a section, then a button to insert its field. Type && for an ampersand."));
      }
    } else {
      HfPreview(s, u, 3);
    }
    ImGui::PopID();
  }
  // Excel's four check boxes under the header and footer
  ImGui::Spacing();
  ImGui::Checkbox(TR("Different odd and even pages"), &u.edit.different_odd_even);
  ImGui::Checkbox(TR("Different first page"), &u.edit.different_first);
  bool scale = !u.edit.no_scale_with_doc, align = !u.edit.no_align_margins;
  if (ImGui::Checkbox(TR("Scale with document"), &scale)) u.edit.no_scale_with_doc = !scale;
  if (ImGui::Checkbox(TR("Align with page margins"), &align)) u.edit.no_align_margins = !align;
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

void OpenPageSetup(AppState& s, int tab) {
  PrintUi& u = U();
  const model::PageSetup* ps = ActiveSetup(s);
  if (!ps) return;
  u.edit = *ps;
  u.edit_sheet = s.focus_sheet;
  u.tab = tab;
  std::string area;
  if (ps->has_area) {
    std::vector<Rect> all{ps->area};
    all.insert(all.end(), ps->more_areas.begin(), ps->more_areas.end());
    for (const Rect& a : all) area += (area.empty() ? "" : ",") + core::CellA1(a.r0, a.c0, true, true) + ":" + core::CellA1(a.r1, a.c1, true, true);
  }
  std::snprintf(u.area, sizeof(u.area), "%s", area.c_str());
  std::snprintf(u.title_rows, sizeof(u.title_rows), "%s", RowsText(ps->has_title_rows, ps->title_r0, ps->title_r1).c_str());
  std::snprintf(u.title_cols, sizeof(u.title_cols), "%s", ColsText(ps->has_title_cols, ps->title_c0, ps->title_c1).c_str());
  const std::string* texts[] = {&ps->header_left, &ps->header_center, &ps->header_right, &ps->footer_left, &ps->footer_center, &ps->footer_right};
  for (int i = 0; i < 6; ++i) std::snprintf(u.hf[i], sizeof(u.hf[i]), "%s", texts[i]->c_str());
  for (int i = 0; i < 6; ++i) std::snprintf(u.hf_first[i], sizeof(u.hf_first[i]), "%s", ps->first_hf[i].c_str());
  for (int i = 0; i < 6; ++i) std::snprintf(u.hf_even[i], sizeof(u.hf_even[i]), "%s", ps->even_hf[i].c_str());
  u.hf_page = 0;
  u.fit_error = false;
  u.first_auto = ps->first_page_number <= 0;
  s.OpenDialog(Dialog::PageSetup);
}

// common settings shared by File -> Print and the Page Setup dialog
bool OrientationCombo(model::PageSetup& ps, float w) {
  int v = ps.landscape ? 1 : 0;
  const char* items[] = {TR("Portrait Orientation"), TR("Landscape Orientation")};
  ImGui::SetNextItemWidth(w);
  if (!ui::Combo("##orient", &v, items, 2)) return false;
  ps.landscape = v == 1;
  return true;
}

bool PaperCombo(model::PageSetup& ps, float w) {
  const auto& papers = Papers();
  int cur = -1;
  for (int i = 0; i < static_cast<int>(papers.size()); ++i) {
    if (papers[static_cast<size_t>(i)].code == ps.paper) cur = i;
  }
  std::string shown;
  if (cur >= 0) {
    shown = papers[static_cast<size_t>(cur)].name;
  } else {
    double pw = 0, ph = 0;
    model::PaperSize(ps.paper, &pw, &ph);
    const char* known = KnownPaperName(ps.paper);
    shown = (known ? std::string(known) : std::to_string(ps.paper)) + "  " + PaperSizeText(pw, ph);
  }
  ui::NoteUiItem("Paper size", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(w, ImGui::GetFrameHeight())));
  ImGui::SetNextItemWidth(w);
  bool changed = false;
  if (ui::BeginCombo("##paper", shown.c_str())) {
    for (int i = 0; i < static_cast<int>(papers.size()); ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(papers[static_cast<size_t>(i)].name.c_str(), i == cur)) {
        ps.paper = papers[static_cast<size_t>(i)].code;
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void SetMargins(model::PageSetup& ps, int kind) {
  // Normal / Wide / Narrow, in inches like Excel; 3: the Last Custom Setting (settings.json)
  const double in = 72;
  if (kind == 0) ps.left = ps.right = 0.7 * in, ps.top = ps.bottom = 0.75 * in, ps.header = ps.footer = 0.3 * in;
  if (kind == 1) ps.left = ps.right = 1.0 * in, ps.top = ps.bottom = 1.0 * in, ps.header = ps.footer = 0.5 * in;
  if (kind == 2) ps.left = ps.right = 0.25 * in, ps.top = ps.bottom = 0.75 * in, ps.header = ps.footer = 0.3 * in;
  if (kind == 3 && Config().last_margins[0] >= 0) {
    const auto& m = Config().last_margins;
    ps.left = m[0], ps.right = m[1], ps.top = m[2], ps.bottom = m[3], ps.header = m[4], ps.footer = m[5];
  }
}

// Margins > Last Custom Setting: margins that are none of Excel's three are remembered (as Excel does)
void RememberCustomMargins(const model::PageSetup& ps);

int MarginKind(const model::PageSetup& ps) {
  for (int k = 0; k < 3; ++k) {
    model::PageSetup t = ps;
    SetMargins(t, k);
    auto same = [](double a, double b) { return std::fabs(a - b) < 0.01; };
    if (same(t.left, ps.left) && same(t.right, ps.right) && same(t.top, ps.top) && same(t.bottom, ps.bottom) && same(t.header, ps.header) &&
        same(t.footer, ps.footer))
      return k;
  }
  return 3;
}

void RememberCustomMargins(const model::PageSetup& ps) {
  if (MarginKind(ps) != 3) return;
  Config().last_margins = {ps.left, ps.right, ps.top, ps.bottom, ps.header, ps.footer};
  SaveSettings();
}

bool MarginsCombo(AppState& s, model::PageSetup& ps, float w) {
  // Excel's list: Last Custom Setting (once there is one), Normal, Wide, Narrow, Custom Margins...
  const bool last = Config().last_margins[0] >= 0;
  std::vector<const char*> items;
  if (last) items.push_back(TR("Last Custom Setting"));
  for (const char* t : {TR("Normal Margins"), TR("Wide Margins"), TR("Narrow Margins"), TR("Custom Margins")}) items.push_back(t);
  const int kind = MarginKind(ps);  // 0-2 presets, 3 custom
  int v = kind == 3 ? static_cast<int>(items.size()) - 1 : kind + (last ? 1 : 0);
  ImGui::SetNextItemWidth(w);
  if (!ui::Combo("##margins", &v, items.data(), static_cast<int>(items.size()))) return false;
  if (v == static_cast<int>(items.size()) - 1) {  // Custom Margins...: Page Setup on its Margins tab, as in Excel
    OpenPageSetup(s, 1);
    return false;
  }
  SetMargins(ps, last ? (v == 0 ? 3 : v - 1) : v);
  return true;
}

int ScaleKind(const model::PageSetup& ps) {
  if (ps.zoom == 100) return 0;
  if (ps.zoom == 0 && ps.fit_wide == 1 && ps.fit_tall == 1) return 1;
  if (ps.zoom == 0 && ps.fit_wide == 1 && ps.fit_tall == 0) return 2;
  if (ps.zoom == 0 && ps.fit_wide == 0 && ps.fit_tall == 1) return 3;
  return 4;
}

void SetScale(model::PageSetup& ps, int kind) {
  if (kind == 0) ps.zoom = 100;
  if (kind == 1) ps.zoom = 0, ps.fit_wide = 1, ps.fit_tall = 1;
  if (kind == 2) ps.zoom = 0, ps.fit_wide = 1, ps.fit_tall = 0;
  if (kind == 3) ps.zoom = 0, ps.fit_wide = 0, ps.fit_tall = 1;
}

bool ScaleCombo(AppState& s, model::PageSetup& ps, float w) {
  int v = ScaleKind(ps);
  const char* items[] = {TR("No Scaling"), TR("Fit Sheet on One Page"), TR("Fit All Columns on One Page"), TR("Fit All Rows on One Page"), TR("Custom Scaling")};
  ImGui::SetNextItemWidth(w);
  if (!ui::Combo("##scale", &v, items, 5)) return false;
  if (v == 4) {  // Custom Scaling Options...: Page Setup on its Page tab
    OpenPageSetup(s, 0);
    return false;
  }
  SetScale(ps, v);
  return true;
}

}  // namespace

void DrawPrintBackstage(AppState& s, const ImRect& area) {
  PrintUi& u = U();
  if (u.printers.empty()) RefreshPrinters();
  ShowBreaks(s, s.focus_sheet);
  const float left_w = S(300);
  const std::vector<PrintSheetJob> jobs = CurrentJobs(s);
  int total = 0;
  for (const auto& j : jobs) total += static_cast<int>(j.layout.pages.size());
  // ---- settings column (a child window keeps its separators out of the preview) ----
  ImGui::BeginChild("##yc_print_settings", ImVec2(left_w + S(16), std::max(S(200), area.Max.y - ImGui::GetCursorScreenPos().y - S(8))), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  ImGui::PushID("##yc_print");
  if (ImGui::Button(TR("Print##do"), ImVec2(S(96), S(72)))) ui::Commands().Execute("PrintNow");
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(TR("Copies:"));
  ImGui::SetNextItemWidth(S(110));
  if (ImGui::InputInt("##copies", &u.copies)) u.copies = std::clamp(u.copies, 1, 999);
  ImGui::EndGroup();
  ImGui::Dummy(ImVec2(1, S(6)));
  ImGui::SeparatorText(TR("Printer"));
  ImGui::SetNextItemWidth(left_w);
  const std::string cur = u.printer >= 0 && u.printer < static_cast<int>(u.printers.size()) ? ToUtf8(u.printers[static_cast<size_t>(u.printer)]) : TR("(no printer)");
  ui::NoteUiItem("Printer", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(left_w, ImGui::GetFrameHeight())));
  if (ui::BeginCombo("##printer", cur.c_str())) {
    for (size_t i = 0; i < u.printers.size(); ++i) {
      if (ImGui::Selectable(ToUtf8(u.printers[i]).c_str(), static_cast<int>(i) == u.printer)) u.printer = static_cast<int>(i);
    }
    ImGui::EndCombo();
  }
  // Excel's "Printer Properties" link: the driver's own settings dialog (kept for this print)
  if (ImGui::SmallButton(TR("Printer Properties"))) ui::Commands().Execute("PrinterProperties");
  ImGui::SeparatorText(TR("Settings"));
  {
    // Print Selected Chart replaces Print Active Sheets while a chart is selected (Excel)
    const bool chart = u.chart >= 0;
    const char* items[] = {chart ? TR("Print Selected Chart") : TR("Print Active Sheets"), TR("Print Entire Workbook"), TR("Print Selection")};
    ui::NoteUiItem("Print what", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(left_w, ImGui::GetFrameHeight())));
    ImGui::SetNextItemWidth(left_w);
    if (ui::Combo("##what", &u.what, items, 3)) u.page = 0;
  }
  ImGui::Checkbox(TR("Ignore Print Area"), &u.ignore_area);
  ImGui::TextUnformatted(TR("Pages:"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(90));
  if (ImGui::InputInt("##from", &u.from)) u.from = std::max(1, u.from);
  ImGui::SameLine();
  ImGui::TextUnformatted(TR("to"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(90));
  if (ImGui::InputInt("##to", &u.to)) u.to = std::max(0, u.to);
  {
    const char* sides[] = {TR("Print One Sided"), TR("Print on Both Sides (flip pages on long edge)"), TR("Print on Both Sides (flip pages on short edge)")};
    ui::NoteUiItem("Sides", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(left_w, ImGui::GetFrameHeight())));
    ImGui::SetNextItemWidth(left_w);
    ui::Combo("##sides", &u.duplex, sides, 3);
    int collate = u.collate ? 0 : 1;
    const char* coll[] = {TR("Collated  1,2,3  1,2,3  1,2,3"), TR("Uncollated  1,1,1  2,2,2  3,3,3")};
    ui::NoteUiItem("Collation", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(left_w, ImGui::GetFrameHeight())));
    ImGui::SetNextItemWidth(left_w);
    if (ui::Combo("##collate", &collate, coll, 2)) u.collate = collate == 0;
  }
  if (const model::PageSetup* cps = ActiveSetup(s)) {
    model::PageSetup ps = *cps;
    bool changed = OrientationCombo(ps, left_w);
    changed |= PaperCombo(ps, left_w);
    changed |= MarginsCombo(s, ps, left_w);
    changed |= ScaleCombo(s, ps, left_w);
    if (changed) EditSetup(s, TR("Page Setup"), [ps](model::PageSetup& p) { p = ps; });
  }
  if (ImGui::Button(TR("Page Setup..."))) OpenPageSetup(s, 0);
  ImGui::PopID();
  ImGui::EndChild();
  ImGui::SameLine(0, S(40));

  // ---- preview ----
  ImGui::BeginGroup();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float avail_w = area.Max.x - origin.x - S(10), avail_h = area.Max.y - origin.y - S(44);
  if (total == 0) {
    ImGui::TextDisabled("%s", TR("We didn't find anything to print."));
    ImGui::EndGroup();
    return;
  }
  u.page = std::clamp(u.page, 0, total - 1);
  size_t ji = 0;
  int local = u.page;
  while (ji < jobs.size() && local >= static_cast<int>(jobs[ji].layout.pages.size())) local -= static_cast<int>(jobs[ji++].layout.pages.size());
  const PrintSheetJob& job = jobs[std::min(ji, jobs.size() - 1)];
  const float aspect = static_cast<float>(job.layout.paper_w / job.layout.paper_h);
  float pw = std::max(S(100), std::min(avail_w, avail_h * aspect));
  const int px_w = static_cast<int>(pw * ImGui::GetIO().DisplayFramebufferScale.x);
  char key[256];
  std::snprintf(key, sizeof(key), "%llu/%u/%d/%d/%d/%d/%d", static_cast<unsigned long long>(s.snap->version), job.sheet, local, px_w, u.what, u.ignore_area ? 1 : 0,
                static_cast<int>(jobs.size()));
  if (u.tex_key != key) {
    if (u.tex) ReleaseTexture(u.tex);
    u.tex = ImTextureID{};
    std::vector<uint8_t> rgba;
    PrintInfo info = MakePrintInfo(s.file_path);
    info.page_number = (job.ps.first_page_number > 0 ? job.ps.first_page_number : 1) + local;
    info.page_count = static_cast<int>(job.layout.pages.size());
    if (RenderPagePreview(*s.snap, job, local, info, px_w, &rgba, &u.tex_w, &u.tex_h)) u.tex = TextureFromRgba(rgba.data(), u.tex_w, u.tex_h);
    u.tex_key = key;
  }
  const float ph = pw / aspect;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0(origin.x + (avail_w - pw) * 0.5f, origin.y);
  dl->AddRectFilled(ImVec2(p0.x + S(3), p0.y + S(3)), ImVec2(p0.x + pw + S(3), p0.y + ph + S(3)), IM_COL32(0, 0, 0, 40));
  if (u.tex) dl->AddImage(u.tex, p0, ImVec2(p0.x + pw, p0.y + ph));
  dl->AddRect(p0, ImVec2(p0.x + pw, p0.y + ph), IM_COL32(0xC8, 0xC8, 0xC8, 255));
  ui::NoteUiItem("Print preview page", ImRect(p0, ImVec2(p0.x + pw, p0.y + ph)));
  // Show Margins: the margin lines (and the header / footer ones) on the page, each dragged to change it (Excel)
  if (u.show_margins && job.chart < 0) {
    const model::PageSetup& mps = job.ps;
    const float k = pw / static_cast<float>(job.layout.paper_w);
    const ImU32 mc = IM_COL32(0x40, 0x40, 0x40, 255);
    const float ys[4] = {p0.y + static_cast<float>(mps.top) * k, p0.y + ph - static_cast<float>(mps.bottom) * k, p0.y + static_cast<float>(mps.header) * k,
                         p0.y + ph - static_cast<float>(mps.footer) * k};
    const float xs[2] = {p0.x + static_cast<float>(mps.left) * k, p0.x + pw - static_cast<float>(mps.right) * k};
    static const char* const kIds[] = {"Left margin", "Right margin", "Top margin", "Bottom margin", "Header margin", "Footer margin"};
    for (int m = 0; m < 6; ++m) {
      const bool vertical = m < 2;
      const float at = vertical ? xs[m] : ys[m - 2];
      const ImVec2 a = vertical ? ImVec2(at, p0.y) : ImVec2(p0.x, at), b = vertical ? ImVec2(at, p0.y + ph) : ImVec2(p0.x + pw, at);
      dl->AddLine(a, b, mc, 1.0f);
      // the handle: a small black box at the page's edge, the whole line takes the drag
      const ImVec2 h0 = vertical ? ImVec2(at - S(3), p0.y - S(8)) : ImVec2(p0.x + pw + S(2), at - S(3));
      dl->AddRectFilled(h0, h0 + ImVec2(S(6), S(6)), IM_COL32_BLACK);
      ImGui::SetCursorScreenPos(vertical ? ImVec2(at - S(3), p0.y) : ImVec2(p0.x, at - S(3)));
      ImGui::PushID(m);
      ImGui::InvisibleButton("##margin", vertical ? ImVec2(S(6), ph) : ImVec2(pw, S(6)));
      ui::NoteUiItem(kIds[m], ImGui::GetCurrentContext()->LastItemData.Rect);
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
      if (ImGui::IsItemActive()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        double v = vertical ? (m == 0 ? mp.x - p0.x : p0.x + pw - mp.x) / k : (m == 2 || m == 4 ? mp.y - p0.y : p0.y + ph - mp.y) / k;
        u.margin_drag = m;
        u.margin_drag_value = std::clamp(v, 0.0, (vertical ? job.layout.paper_w : job.layout.paper_h) * 0.45);
        const float shown = vertical ? (m == 0 ? p0.x : p0.x + pw) + static_cast<float>(m == 0 ? u.margin_drag_value : -u.margin_drag_value) * k
                                     : (m == 2 || m == 4 ? p0.y : p0.y + ph) + static_cast<float>(m == 2 || m == 4 ? u.margin_drag_value : -u.margin_drag_value) * k;
        if (vertical) dl->AddLine(ImVec2(shown, p0.y), ImVec2(shown, p0.y + ph), IM_COL32(0x21, 0x73, 0x46, 255), S(2));
        else dl->AddLine(ImVec2(p0.x, shown), ImVec2(p0.x + pw, shown), IM_COL32(0x21, 0x73, 0x46, 255), S(2));
      }
      if (ImGui::IsItemDeactivated() && u.margin_drag == m) {
        const double v = u.margin_drag_value;
        const uint32_t sheet = job.sheet;
        s.engine->PostUser(TR("Margins"), [sheet, m, v](Document& d) {
          d.UpdatePageSetup(sheet, [m, v](model::PageSetup& p) {
            double* f[] = {&p.left, &p.right, &p.top, &p.bottom, &p.header, &p.footer};
            *f[m] = v;
          });
        });
        u.margin_drag = -1;
      }
      ImGui::PopID();
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x, p0.y + ph));
  ImGui::Dummy(ImVec2(avail_w, S(8)));
  // page navigation: the arrows and the page number box ("1 of 3"; type a page number and press Enter)
  ImGui::SetCursorScreenPos(ImVec2(p0.x, ImGui::GetCursorScreenPos().y));
  if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) u.page = std::max(0, u.page - 1);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(S(48));
  ui::NoteUiItem("Current Page", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(48), ImGui::GetFrameHeight())));
  static int typed = 1;
  if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && ImGui::GetActiveID() != ImGui::GetID("##pagebox")) typed = u.page + 1;
  if (ImGui::InputInt("##pagebox", &typed, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) u.page = std::clamp(typed - 1, 0, total - 1);
  ImGui::SameLine();
  ImGui::Text(TR("of %d"), total);
  ImGui::SameLine();
  if (ImGui::ArrowButton("##next", ImGuiDir_Right)) u.page = std::min(total - 1, u.page + 1);
  // Show Margins and Zoom to Page at the bottom right (Excel)
  ImGui::SameLine(0, S(30));
  ImGui::Checkbox(TR("Show Margins"), &u.show_margins);
  if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) u.page = std::clamp(u.page - static_cast<int>(ImGui::GetIO().MouseWheel), 0, total - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) u.page = std::min(total - 1, u.page + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) u.page = std::max(0, u.page - 1);
  ImGui::EndGroup();
}

void DrawExportPdfButton(AppState& s) {
  if (ImGui::Button(TR("Create PDF Document"), ImVec2(S(320), S(36)))) {
    ui::ribbon::St().backstage = false;
    ui::Commands().Execute("ExportPdf");
  }
  ImGui::TextDisabled("%s", TR("Saves the active sheet as PDF with its page setup (through Microsoft Print to PDF)."));
  (void)s;
}

bool ExportPdf(AppState& s, const std::wstring& path, const std::vector<uint32_t>& sheets, bool ignore_area, int from, int to, std::string* err,
               const Rect* area) {
  std::wstring def;
  const auto names = PrinterNames(&def);
  if (std::find(names.begin(), names.end(), std::wstring(kPdfPrinter)) == names.end()) {
    *err = TR("Microsoft Print to PDF isn't installed (Windows Features -> Microsoft Print to PDF).");
    return false;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);  // the driver doesn't overwrite reliably
  const auto jobs = BuildPrintJobs(*s.snap, sheets, area, ignore_area);
  PrintRequest rq;
  rq.printer = kPdfPrinter;
  rq.output = path;
  rq.from = from;
  rq.to = to;
  return PrintJobs(*s.snap, jobs, rq, MakePrintInfo(s.file_path), err);
}

bool PrintSheets(AppState& s, const std::vector<uint32_t>& sheets, const std::wstring& printer, int from, int to, int copies, bool collate, const std::wstring& to_file,
                 std::string* err) {
  std::wstring def;
  PrinterNames(&def);
  PrintRequest rq;
  rq.printer = printer.empty() ? def : printer;
  rq.output = to_file;
  rq.from = from;
  rq.to = to;
  rq.copies = copies;
  rq.collate = collate;
  if (rq.printer.empty()) {
    *err = TR("No printer is installed.");
    return false;
  }
  return PrintJobs(*s.snap, BuildPrintJobs(*s.snap, sheets), rq, MakePrintInfo(s.file_path), err);
}

int PageCount(AppState& s, uint32_t sheet) {
  const auto jobs = BuildPrintJobs(*s.snap, {sheet});
  return jobs.empty() ? 0 : static_cast<int>(jobs[0].layout.pages.size());
}

void ShowPageBreaks(AppState& s, uint32_t sheet) { ShowBreaks(s, sheet); }

void UpdateLayoutContext(AppState& s);

void UpdatePrintContext(AppState& s) {
  UpdateShapeEffects(s);
  auto& cx = ui::ribbon::Context();
  if (const model::PageSetup* ps = ActiveSetup(s)) {
    cx.pg_gridlines = ps->gridlines;
    cx.pg_headings = ps->headings;
  }
  UpdateLayoutContext(s);
}

// dashed lines where pages break (automatic and manual), like Excel's normal view after printing
void DrawPageBreakOverlay(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  PrintUi& u = U();
  if (s.Win(sheet_id).view.page_view != 0) return;  // the page views draw their own lines
  if (!s.snap || !s.Win(sheet_id).page_breaks) return;
  const model::Sheet* sh = s.snap->Sheet(sheet_id);
  if (!sh) return;
  const std::array<const void*, 6> from{&*sh->rows, &*sh->cols, &*sh->page_setup, &*sh->shapes, &*sh->charts, &*sh->sparklines};
  const double now = ImGui::GetTime();
  // a layout of many pages takes long (600 ms on a debug build): the clock waits for a multiple of that, or every
  // frame made it again (chaos seed 5117)
  const double every = std::max(0.5, 20 * u.breaks_cost);
  if (u.breaks_sheet != sheet_id || u.breaks_from != from || (u.breaks_version != s.snap->version && now - u.breaks_at > every)) {
    const auto t0 = std::chrono::steady_clock::now();
    u.breaks_layout = engine::LayoutPages(*sh, *sh->page_setup);
    u.breaks_cost = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    u.breaks_version = s.snap->version;
    u.breaks_sheet = sheet_id;
    u.breaks_from = from;
    u.breaks_at = ImGui::GetTime() + u.breaks_cost;
  }
  const engine::PrintLayout& L = u.breaks_layout;
  if (L.empty) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  const auto& ps = *sh->page_setup;
  const double y0 = static_cast<double>(sh->rows->Offset(L.area.r0)), y1 = static_cast<double>(sh->rows->Offset(static_cast<int64_t>(L.area.r1) + 1));
  const double x0 = static_cast<double>(sh->cols->Offset(L.area.c0)), x1 = static_cast<double>(sh->cols->Offset(static_cast<int64_t>(L.area.c1) + 1));
  // only the part of a line over the cells shown, its dashes in their places along the whole line: dashing every
  // break from the print area's first row to its last drew millions of hidden dashes a frame on a small paper size,
  // and each step of chaos seed 5117 took 10 s
  const ImRect view = g.cells;
  auto dashed = [&](ImVec2 a, ImVec2 b, ImU32 col, float w) {
    const float len = std::hypot(b.x - a.x, b.y - a.y), dash = S(4), gap = S(3);
    if (len <= 0) return;
    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
    float t0 = 0, t1 = len;
    auto keep = [&](float p, float dp, float lo, float hi) {
      if (std::fabs(dp) < 1e-6f) {
        if (p < lo || p > hi) t1 = -1;  // beside the cells shown
        return;
      }
      const float ta = (lo - p) / dp, tb = (hi - p) / dp;
      t0 = std::max(t0, std::min(ta, tb));
      t1 = std::min(t1, std::max(ta, tb));
    };
    keep(a.x, d.x, view.Min.x, view.Max.x);
    keep(a.y, d.y, view.Min.y, view.Max.y);
    if (t1 <= t0) return;
    for (float t = std::floor(t0 / (dash + gap)) * (dash + gap); t < t1; t += dash + gap) {
      const float e = std::min(len, t + dash);
      dl->AddLine(ImVec2(a.x + d.x * t, a.y + d.y * t), ImVec2(a.x + d.x * e, a.y + d.y * e), col, w);
    }
  };
  const ImU32 col = IM_COL32(0x60, 0x60, 0x60, 255), manual = IM_COL32(0x20, 0x40, 0xA0, 255);
  for (const auto& seg : L.col_segs) {
    const double x = static_cast<double>(sh->cols->Offset(static_cast<int64_t>(seg.second) + 1));
    const bool m = std::find(ps.col_breaks.begin(), ps.col_breaks.end(), seg.second + 1) != ps.col_breaks.end();
    dashed(g.to_screen(x, y0), g.to_screen(x, y1), m ? manual : col, m ? 2.0f : 1.0f);
  }
  for (const auto& seg : L.row_segs) {
    const double y = static_cast<double>(sh->rows->Offset(static_cast<int64_t>(seg.second) + 1));
    const bool m = std::find(ps.row_breaks.begin(), ps.row_breaks.end(), seg.second + 1) != ps.row_breaks.end();
    dashed(g.to_screen(x0, y), g.to_screen(x1, y), m ? manual : col, m ? 2.0f : 1.0f);
  }
  dl->PopClipRect();
}

// ---- Page Break Preview and Page Layout view (View -> Workbook Views) ----

namespace {

struct PageViewUi {
  // Page Break Preview: the line being dragged
  int drag_kind = 0;  // 0 none, 1 column break, 2 row break, 3 area left, 4 area right, 5 area top, 6 area bottom
  uint32_t drag_from = 0;
  bool drag_manual = false;
  // Page Layout view
  struct PageTex {
    std::string key;
    ImTextureID tex{};
  };
  std::map<uint32_t, std::map<int, PageTex>> pages;  // sheet -> page -> texture
  std::map<uint32_t, ImVec2> scroll;                 // sheet -> scroll offset (pixels)
  uint64_t jobs_version = ~0ull;
  std::map<uint32_t, PrintSheetJob> jobs;
  // Page Layout view: a drag over the cells selects them; the header / footer is typed on the page (Excel)
  bool sel_drag = false;
  int hf_which = -1;  // 0 header, 1 footer being typed on page hf_page of sheet hf_sheet
  int hf_page = 0, hf_box = 1, hf_caret = -1;
  uint32_t hf_sheet = 0;
  bool hf_focus = false;
  char hf_buf[3][256] = {};
  std::string hf_pending;  // a field button's code, put in at the caret by the box's next callback
  int hf_prev_tab = -1;    // the ribbon tab shown before the Header & Footer tab (back to it when typing ends)
  bool hf_pic_popup = false;  // Header & Footer > Format Picture asked for its popup
  model::PageSetup::HfPicture hf_pic_edit;
  int hf_pic_section = -1;
  // Page Layout view like a sheet: the fill handle and the selection's border dragged on the pages
  int drag = 0;  // 0 none, 1 fill handle, 2 move (drag and drop)
  Rect drag_src{}, drag_dst{};
  uint32_t grab_r = 0, grab_c = 0;  // the grabbed cell, relative to the moved range
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> last_active;  // sheet -> the active cell last frame (the view follows it)
  bool clicked_on_page = false;  // the active cell changed by a click on the page (no scrolling to it)
  int ruler_drag = -1;           // a margin dragged on the ruler: 0 left, 1 right, 2 top, 3 bottom
  bool ruler = true;             // View > Ruler (Page Layout view)
  bool hf_scroll = false;        // the header / footer just started: the view brings it on screen (Go to Footer)
  double ruler_value = 0;
};

PageViewUi& PV() {
  static PageViewUi u;
  return u;
}

// the sheet's pages for the page views (an empty sheet too: Page Layout view shows its first page); view_gridlines:
// the pictures show the cells' gridlines in the screen's light gray (View > Gridlines without Print Gridlines)
const PrintSheetJob* JobFor(AppState& s, uint32_t sheet, bool view_gridlines = false) {
  PageViewUi& u = PV();
  if (u.jobs_version != s.snap->version) {
    u.jobs.clear();
    u.jobs_version = s.snap->version;
  }
  const uint32_t key = sheet << 1 | (view_gridlines ? 1u : 0u);
  auto it = u.jobs.find(key);
  if (it == u.jobs.end()) {
    const model::Sheet* sh = s.snap->Sheet(sheet);
    if (!sh || sh->chart_sheet) return nullptr;
    PrintSheetJob j;
    j.sheet = sheet;
    j.ps = *sh->page_setup;
    j.layout = engine::LayoutPages(*sh, j.ps);
    AddNotesPages(*sh, &j);
    if (view_gridlines && !j.ps.gridlines) {
      j.ps.gridlines = true;
      j.gridline_rgb = 0xD9D9D9;
    }
    it = u.jobs.emplace(key, std::move(j)).first;
  }
  return &it->second;
}

// the column / row boundary (an index where a page can start) nearest to a sheet coordinate
uint32_t NearestBoundary(const model::DimIndex& dim, double pos, uint32_t limit) {
  const int64_t i = dim.IndexAt(static_cast<int64_t>(pos));
  const double a = static_cast<double>(dim.Offset(i)), b = static_cast<double>(dim.Offset(i + 1));
  return static_cast<uint32_t>(std::min<int64_t>(pos - a < b - pos ? i : i + 1, limit));
}

void DrawBreakPreview(AppState& s, const model::Sheet& sh, const render::OverlayGeom& g) {
  PageViewUi& u = PV();
  const PrintSheetJob* job = JobFor(s, sh.id);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(g.cells.Min, g.cells.Max, true);
  const ImU32 shade = IM_COL32(0x80, 0x80, 0x80, 110), blue = IM_COL32(0x1F, 0x4E, 0xC8, 255);
  if (!job || job->layout.empty) {
    dl->AddRectFilled(g.cells.Min, g.cells.Max, shade);
    dl->PopClipRect();
    return;
  }
  const engine::PrintLayout& L = job->layout;
  const model::PageSetup& ps = job->ps;
  auto X = [&](uint32_t c) { return static_cast<double>(sh.cols->Offset(c)); };
  auto Y = [&](uint32_t r) { return static_cast<double>(sh.rows->Offset(r)); };
  const ImVec2 a0 = g.to_screen(X(L.area.c0), Y(L.area.r0)), a1 = g.to_screen(X(L.area.c1 + 1), Y(L.area.r1 + 1));
  // gray outside the print area
  dl->AddRectFilled(g.cells.Min, ImVec2(g.cells.Max.x, a0.y), shade);
  dl->AddRectFilled(ImVec2(g.cells.Min.x, a1.y), g.cells.Max, shade);
  dl->AddRectFilled(ImVec2(g.cells.Min.x, std::max(a0.y, g.cells.Min.y)), ImVec2(a0.x, std::min(a1.y, g.cells.Max.y)), shade);
  dl->AddRectFilled(ImVec2(a1.x, std::max(a0.y, g.cells.Min.y)), ImVec2(g.cells.Max.x, std::min(a1.y, g.cells.Max.y)), shade);
  // "Page N" in the middle of every page
  ImFont* font = ui::F().ui;
  for (size_t i = 0; i < L.pages.size(); ++i) {
    const auto& P = L.pages[i];
    const ImVec2 p0 = g.to_screen(X(P.c0), Y(P.r0)), p1 = g.to_screen(X(P.c1 + 1), Y(P.r1 + 1));
    if (p1.x < g.cells.Min.x || p0.x > g.cells.Max.x || p1.y < g.cells.Min.y || p0.y > g.cells.Max.y) continue;
    char tb[64];
    std::snprintf(tb, sizeof tb, TR("Page %d"), static_cast<int>(i + 1));
    const std::string t = tb;
    const float size = std::clamp(std::min(p1.x - p0.x, p1.y - p0.y) * 0.22f, S(12), S(72));
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t.c_str());
    dl->AddText(font, size, ImVec2((p0.x + p1.x - ts.x) / 2, (p0.y + p1.y - ts.y) / 2), IM_COL32(0x90, 0x90, 0x90, 120), t.c_str());
  }
  auto dashed = [&](ImVec2 p, ImVec2 q, float w) {
    const float len = std::hypot(q.x - p.x, q.y - p.y), dash = S(6), gap = S(4);
    if (len <= 0) return;
    const ImVec2 d((q.x - p.x) / len, (q.y - p.y) / len);
    for (float t = 0; t < len; t += dash + gap) {
      const float e = std::min(len, t + dash);
      dl->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * e, p.y + d.y * e), blue, w);
    }
  };
  // lines: the area border (solid), manual breaks (solid), automatic breaks (dashed); each one can be dragged
  struct Line {
    int kind;  // as drag_kind
    uint32_t at;
    bool manual;
  };
  std::vector<Line> lines{{3, L.area.c0, true}, {4, L.area.c1 + 1, true}, {5, L.area.r0, true}, {6, L.area.r1 + 1, true}};
  for (size_t i = 0; i + 1 < L.col_segs.size(); ++i) {
    const uint32_t at = L.col_segs[i].second + 1;
    lines.push_back({1, at, std::find(ps.col_breaks.begin(), ps.col_breaks.end(), at) != ps.col_breaks.end()});
  }
  for (size_t i = 0; i + 1 < L.row_segs.size(); ++i) {
    const uint32_t at = L.row_segs[i].second + 1;
    lines.push_back({2, at, std::find(ps.row_breaks.begin(), ps.row_breaks.end(), at) != ps.row_breaks.end()});
  }
  const float grab = S(4);
  ImGuiIO& io = ImGui::GetIO();
  for (size_t i = 0; i < lines.size(); ++i) {
    const Line& ln = lines[i];
    const bool vertical = ln.kind == 1 || ln.kind == 3 || ln.kind == 4;
    ImVec2 p, q;
    if (vertical) {
      const float x = g.to_screen(X(ln.at), 0).x;
      p = ImVec2(x, std::max(a0.y, g.cells.Min.y));
      q = ImVec2(x, std::min(a1.y, g.cells.Max.y));
    } else {
      const float y = g.to_screen(0, Y(ln.at)).y;
      p = ImVec2(std::max(a0.x, g.cells.Min.x), y);
      q = ImVec2(std::min(a1.x, g.cells.Max.x), y);
    }
    if (ln.manual) dl->AddLine(p, q, blue, S(2.5f));
    else dashed(p, q, S(2));
    if (q.x - p.x < 1 && q.y - p.y < 1) continue;
    // a thin button along the line takes the mouse before the grid
    ImGui::PushID(static_cast<int>(i) + 7000);
    const ImVec2 b0 = vertical ? ImVec2(p.x - grab, p.y) : ImVec2(p.x, p.y - grab);
    const ImVec2 b1 = vertical ? ImVec2(p.x + grab, q.y) : ImVec2(q.x, p.y + grab);
    ImGui::SetCursorScreenPos(b0);
    ImGui::InvisibleButton("##brk", ImVec2(std::max(1.0f, b1.x - b0.x), std::max(1.0f, b1.y - b0.y)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActivated()) {
      u.drag_kind = ln.kind;
      u.drag_from = ln.at;
      u.drag_manual = ln.manual;
    }
    if (ImGui::IsItemActive() && u.drag_kind == ln.kind && u.drag_from == ln.at) {
      const ImVec2 sp = g.to_sheet(io.MousePos);
      const uint32_t to = vertical ? NearestBoundary(*sh.cols, sp.x, core::kMaxCols) : NearestBoundary(*sh.rows, sp.y, core::kMaxRows);
      const float pos = vertical ? g.to_screen(X(to), 0).x : g.to_screen(0, Y(to)).y;
      if (vertical) dl->AddLine(ImVec2(pos, g.cells.Min.y), ImVec2(pos, g.cells.Max.y), blue, S(3));
      else dl->AddLine(ImVec2(g.cells.Min.x, pos), ImVec2(g.cells.Max.x, pos), blue, S(3));
    }
    if (ImGui::IsItemDeactivated() && u.drag_kind == ln.kind && u.drag_from == ln.at) {
      const ImVec2 sp = g.to_sheet(io.MousePos);
      const uint32_t to = vertical ? NearestBoundary(*sh.cols, sp.x, core::kMaxCols) : NearestBoundary(*sh.rows, sp.y, core::kMaxRows);
      const int kind = ln.kind;
      const uint32_t from = ln.at;
      const Rect area = L.area;
      if (to != from) {
        EditSetup(s, kind <= 2 ? TR("Move Page Break") : TR("Set Print Area"), [kind, from, to, area](model::PageSetup& x) {
          if (kind <= 2) {
            // the moved break becomes a manual one; dragged out of the print area it disappears
            auto& v = kind == 1 ? x.col_breaks : x.row_breaks;
            v.erase(std::remove(v.begin(), v.end(), from), v.end());
            const uint32_t lo = kind == 1 ? area.c0 : area.r0, hi = kind == 1 ? area.c1 : area.r1;
            if (to > lo && to <= hi) {
              v.push_back(to);
              std::sort(v.begin(), v.end());
            }
          } else {
            Rect a = area;
            if (kind == 3) a.c0 = std::min(to, a.c1);
            if (kind == 4) a.c1 = std::max(a.c0, to == 0 ? 0 : to - 1);
            if (kind == 5) a.r0 = std::min(to, a.r1);
            if (kind == 6) a.r1 = std::max(a.r0, to == 0 ? 0 : to - 1);
            x.has_area = true;
            x.area = a;
          }
        });
      }
      u.drag_kind = 0;
    }
    ImGui::PopID();
  }
  dl->PopClipRect();
}

// the header or footer of a page typed in place (Page Layout view): its three sections over the page's margin, Excel's
// field buttons under them; a click elsewhere, Enter or Escape keeps what was typed
void CommitPageHeaderFooter(AppState& s) {
  PageViewUi& u = PV();
  if (u.hf_which < 0) return;
  const int which = u.hf_which, page = u.hf_page;
  std::string t[3];
  for (int k = 0; k < 3; ++k) t[k] = model::HeaderCodesFromDisplay(u.hf_buf[k]);
  u.hf_which = -1;
  u.hf_pending.clear();
  const model::Sheet* sh = s.snap ? s.snap->Sheet(u.hf_sheet) : nullptr;
  if (!sh) return;
  const model::PageSetup& ps = *sh->page_setup;
  const int number = (ps.first_page_number > 0 ? ps.first_page_number : 1) + page;
  bool same = true;
  for (int k = 0; k < 3; ++k) same = same && t[k] == model::HeaderFooterText(ps, which * 3 + k, page, number);
  if (same) return;
  const uint32_t sheet = u.hf_sheet;
  s.focus_sheet = sheet;
  EditSetup(s, which == 0 ? TR("Header") : TR("Footer"), [which, page, number, t](model::PageSetup& x) {
    std::string* dst[3];
    for (int k = 0; k < 3; ++k) {
      const int w = which * 3 + k;
      if (x.different_first && page == 0) dst[k] = &x.first_hf[w];
      else if (x.different_odd_even && number % 2 == 0) dst[k] = &x.even_hf[w];
      else {
        std::string* t6[] = {&x.header_left, &x.header_center, &x.header_right, &x.footer_left, &x.footer_center, &x.footer_right};
        dst[k] = t6[w];
      }
    }
    for (int k = 0; k < 3; ++k) *dst[k] = t[k];
  });
}

void StartPageHeaderFooter(const model::PageSetup& ps, uint32_t sheet, int which, int page, int box) {
  PageViewUi& u = PV();
  ImGui::ClearActiveID();  // a section box still active would keep its old text over the new one
  u.hf_scroll = true;
  u.hf_which = which;
  u.hf_page = page;
  u.hf_sheet = sheet;
  u.hf_box = box;
  u.hf_caret = -1;
  u.hf_focus = true;
  const int number = (ps.first_page_number > 0 ? ps.first_page_number : 1) + page;
  for (int k = 0; k < 3; ++k) {
    std::snprintf(u.hf_buf[k], sizeof u.hf_buf[k], "%s", model::HeaderCodesToDisplay(model::HeaderFooterText(ps, which * 3 + k, page, number)).c_str());
  }
}

// draws the three sections in band (the page's header or footer margin) and the field buttons beside it; returns
// whether the mouse is over them
bool DrawPageHeaderFooterBoxes(const ImRect& band, bool below) {
  (void)below;
  PageViewUi& u = PV();
  bool over = false;
  const float w3 = band.GetWidth() / 3;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_BLACK);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  const float fh = ImGui::GetFrameHeight();
  const float y = std::max(band.Min.y, band.Max.y - fh - S(2));
  for (int k = 0; k < 3; ++k) {
    ImGui::PushID(k);
    ImGui::SetCursorScreenPos(ImVec2(band.Min.x + w3 * static_cast<float>(k) + S(2), y));
    ImGui::SetNextItemWidth(w3 - S(4));
    if (u.hf_focus && k == u.hf_box) {
      ImGui::SetKeyboardFocusHere();
      u.hf_focus = false;
    }
    struct Ctx {
      int k;
    } ctx{k};
    const bool enter = ImGui::InputText("##hfsec", u.hf_buf[k], sizeof u.hf_buf[k], ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_EnterReturnsTrue,
                                        [](ImGuiInputTextCallbackData* d) {
                                          PageViewUi& v = PV();
                                          v.hf_box = static_cast<Ctx*>(d->UserData)->k;
                                          if (!v.hf_pending.empty()) {
                                            d->InsertChars(d->CursorPos, v.hf_pending.c_str());
                                            v.hf_pending.clear();
                                          }
                                          v.hf_caret = d->CursorPos;
                                          return 0;
                                        },
                                        &ctx);
    static const char* const kNotes[] = {"Left section", "Center section", "Right section"};
    ui::NoteUiItem(kNotes[k], ImGui::GetCurrentContext()->LastItemData.Rect);
    over = over || ImGui::IsItemHovered();
    if (enter) u.hf_focus = false, u.hf_box = -2;  // Enter: done
    ImGui::PopID();
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
  // the fields (Page Number, Sheet Name...) are on the Header & Footer tab, as in Excel
  return over;
}

// Excel's Page Layout view shows every cell of a page, not only the printed range: the cells after the last printed
// row / column go on (at the page's scale) to the bottom / right margin, on the last page row / column
void ExtendPageCells(const model::Sheet& sh, const model::PageSetup& ps, const engine::PrintLayout& L, bool rows, bool cols, PageCellMap* m) {
  auto extend = [&](std::vector<PageCellMap::Slot>& v, const model::DimIndex& dims, double start, double limit, uint32_t max_index) {
    double scale = L.scale > 0 ? L.scale : 1.0, pos = start;
    uint32_t next = 0;
    if (!v.empty()) {
      const auto& last = v.back();
      const double natural = static_cast<double>(dims.Offset(static_cast<int64_t>(last.index) + 1) - dims.Offset(last.index)) * 0.75;
      if (natural > 0 && last.size > 0) scale = last.size / natural;
      pos = last.pos + last.size;
      next = last.index + 1;
    }
    for (int k = 0; k < 1000 && pos < limit && next < max_index; ++k, ++next) {
      const double size = static_cast<double>(dims.Offset(static_cast<int64_t>(next) + 1) - dims.Offset(next)) * 0.75 * scale;
      if (size <= 0) continue;  // hidden
      v.push_back({next, pos, size});
      pos += size;
    }
  };
  const bool empty = m->rows.empty() || m->cols.empty();
  if (empty) m->rows.clear(), m->cols.clear();
  if (rows || empty) extend(m->rows, *sh.rows, ps.top, L.paper_h - ps.bottom, sh.row_limit);
  if (cols || empty) extend(m->cols, *sh.cols, ps.left, L.paper_w - ps.right, sh.col_limit);
}

// rows / columns from index first, laid from start to limit points at the given scale (hidden ones skipped)
void SlotsFrom(const model::DimIndex& dims, uint32_t first, double start, double limit, double scale, uint32_t max_index, std::vector<PageCellMap::Slot>* v) {
  double pos = start;
  uint32_t next = first;
  for (int k = 0; k < 1000 && pos < limit && next < max_index; ++k, ++next) {
    const double size = static_cast<double>(dims.Offset(static_cast<int64_t>(next) + 1) - dims.Offset(next)) * 0.75 * scale;
    if (size <= 0) continue;
    v->push_back({next, pos, size});
    pos += size;
  }
}

// Page Layout view's pages: the printed ones in their page columns / rows, then one more column and row of
// "Click to add data" pages, as Excel shows them (several print areas or pages of notes: the pages one under another)
struct PageGrid {
  int ncols = 1, nrows = 1;
  int extra_cols = 1, extra_rows = 1;  // added pages after the printed ones (more while the active cell is further away)
  bool single = false;
  int Cols() const { return single ? 1 : ncols + extra_cols; }
  int Rows(int n) const { return single ? n : nrows + extra_rows; }
};

PageGrid GridOf(const PrintSheetJob& job) {
  PageGrid pg;
  const auto& L = job.layout;
  if (L.empty) return pg;
  pg.ncols = std::max<int>(1, static_cast<int>(L.col_segs.size()));
  pg.nrows = std::max<int>(1, static_cast<int>(L.row_segs.size()));
  if (static_cast<int>(L.pages.size()) != pg.ncols * pg.nrows) pg.single = true;
  return pg;
}

// the printed page at (gx, gy) of the grid, or -1 for a "Click to add data" page
int PageAt(const PrintSheetJob& job, const PageGrid& pg, int gx, int gy) {
  if (pg.single) return gx == 0 && gy < static_cast<int>(job.layout.pages.size()) ? gy : -1;
  if (gx >= pg.ncols || gy >= pg.nrows) return -1;
  if (job.layout.empty) return 0;
  return job.ps.over_then_down ? gy * pg.ncols + gx : gx * pg.nrows + gy;
}

// the cells of the page at (gx, gy) on the paper (points): the last printed page column / row goes on to the margins,
// an added page continues after it
void GridPageMap(AppState& s, const model::Sheet& sh, const PrintSheetJob& job, const PageGrid& pg, int gx, int gy, PageCellMap* m) {
  m->rows.clear();
  m->cols.clear();
  const auto& L = job.layout;
  const auto& ps = job.ps;
  const int i = PageAt(job, pg, gx, gy);
  if (i >= 0) {
    if (!L.empty) PageCellMapOf(*s.snap, job, i, m);
    if (!pg.single) ExtendPageCells(sh, ps, L, gy == pg.nrows - 1, gx == pg.ncols - 1, m);
    return;
  }
  if (pg.single) return;
  const double scale = L.scale > 0 ? L.scale : 1.0;
  PageCellMap other;
  if (gy < pg.nrows) {
    GridPageMap(s, sh, job, pg, pg.ncols - 1, gy, &other);
    m->rows = other.rows;
  } else {
    GridPageMap(s, sh, job, pg, 0, gy - 1, &other);  // the rows go on after the page above
    SlotsFrom(*sh.rows, other.rows.empty() ? 0 : other.rows.back().index + 1, ps.top, L.paper_h - ps.bottom, scale, sh.row_limit, &m->rows);
  }
  if (gx < pg.ncols) {
    GridPageMap(s, sh, job, pg, gx, pg.nrows - 1, &other);
    m->cols = other.cols;
  } else {
    GridPageMap(s, sh, job, pg, gx - 1, 0, &other);  // the columns go on after the page at the left
    SlotsFrom(*sh.cols, other.cols.empty() ? 0 : other.cols.back().index + 1, ps.left, L.paper_w - ps.right, scale, sh.col_limit, &m->cols);
  }
}

// a page on the screen this frame: where it is and its cells
struct ShownPage {
  ImVec2 p0;
  PageCellMap map;
};
// the pages Page Layout view drew last, per sheet, and their scale: ui.cell_rect finds a cell where it shows on them
std::map<uint32_t, std::pair<std::vector<ShownPage>, float>> g_shown_last;

// the cell under a screen point (false: none)
bool CellUnder(const std::vector<ShownPage>& pages, float px_per_pt, ImVec2 at, uint32_t* r, uint32_t* c) {
  for (const ShownPage& p : pages) {
    if (p.map.rows.empty() || p.map.cols.empty()) continue;
    if (p.map.CellAt((at.x - p.p0.x) / px_per_pt, (at.y - p.p0.y) / px_per_pt, r, c)) return true;
  }
  return false;
}

// the screen rectangle of the cells r0..r1 x c0..c1 shown on a page (clipped to the page's cells); false: not on it
bool RangeOnPage(const ShownPage& p, float k, const Rect& a, ImVec2* q0, ImVec2* q1) {
  if (p.map.rows.empty() || p.map.cols.empty()) return false;
  const uint32_t r0 = std::max(a.r0, p.map.rows.front().index), r1 = std::min(a.r1, p.map.rows.back().index);
  const uint32_t c0 = std::max(a.c0, p.map.cols.front().index), c1 = std::min(a.c1, p.map.cols.back().index);
  double ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
  if (r0 > r1 || c0 > c1 || !p.map.CellRect(r0, c0, &ax0, &ay0, &ax1, &ay1) || !p.map.CellRect(r1, c1, &bx0, &by0, &bx1, &by1)) return false;
  *q0 = ImVec2(p.p0.x + static_cast<float>(ax0) * k, p.p0.y + static_cast<float>(ay0) * k);
  *q1 = ImVec2(p.p0.x + static_cast<float>(bx1) * k, p.p0.y + static_cast<float>(by1) * k);
  return true;
}

// a ruler along one side of a page (Page Layout view): the margins gray, the inside white, a tick every
// centimeter / quarter inch with the numbers; the margin edges are dragged to change them (Excel)
void DrawRuler(AppState& s, uint32_t sheet, ImDrawList* dl, bool horizontal, float a0, float a1, float lo_margin, float hi_margin, float band0, float band1,
               float px_per_pt, int id) {
  PageViewUi& u = PV();
  const ImU32 gray = IM_COL32(0xC8, 0xC8, 0xC8, 255), white = IM_COL32_WHITE, ink = IM_COL32(0x50, 0x50, 0x50, 255);
  auto box = [&](float x0, float x1, ImU32 col) {
    if (horizontal) dl->AddRectFilled(ImVec2(x0, band0), ImVec2(x1, band1), col);
    else dl->AddRectFilled(ImVec2(band0, x0), ImVec2(band1, x1), col);
  };
  const float m0 = a0 + lo_margin * px_per_pt, m1 = a1 - hi_margin * px_per_pt;
  box(a0, m0, gray);
  box(m0, m1, white);
  box(m1, a1, gray);
  const bool metric = MetricUnits();
  const float step = (metric ? 72.0f / 2.54f : 72.0f / 4.0f) * px_per_pt;  // 1 cm or 1/4 inch
  if (step > 3) {
    int k = 0;
    for (float x = m0; x <= a1; x += step, ++k) {
      const bool major = metric || k % 4 == 0;
      const float len = (major ? 0.45f : 0.25f) * (band1 - band0);
      if (horizontal) dl->AddLine(ImVec2(x, band1 - len), ImVec2(x, band1), ink);
      else dl->AddLine(ImVec2(band1 - len, x), ImVec2(band1, x), ink);
      const int number = metric ? k : k / 4;
      if (major && number > 0 && (metric ? k % 1 == 0 : true) && step * (metric ? 1 : 4) > S(14)) {
        const std::string t = std::to_string(number);
        const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        if (horizontal) dl->AddText(ImVec2(x - ts.x / 2, band0), ink, t.c_str());
        else dl->AddText(ImVec2(band0 + 1, x - ts.y / 2), ink, t.c_str());
      }
    }
  }
  // the two margin edges: dragged to change the margin
  ImGuiIO& io = ImGui::GetIO();
  for (int e = 0; e < 2; ++e) {
    const float at = e == 0 ? m0 : m1;
    const int which = horizontal ? e : 2 + e;  // 0 left, 1 right, 2 top, 3 bottom
    ImGui::PushID(id * 4 + e + 9000);
    ImGui::SetCursorScreenPos(horizontal ? ImVec2(at - S(3), band0) : ImVec2(band0, at - S(3)));
    ImGui::InvisibleButton("##rulermargin", horizontal ? ImVec2(S(6), band1 - band0) : ImVec2(band1 - band0, S(6)));
    static const char* const kNames[] = {"Ruler left margin", "Ruler right margin", "Ruler top margin", "Ruler bottom margin"};
    ui::NoteUiItem(kNames[which], ImGui::GetCurrentContext()->LastItemData.Rect);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
      ImGui::SetTooltip("%s", which == 0 ? TR("Left Margin") : which == 1 ? TR("Right Margin") : which == 2 ? TR("Top Margin") : TR("Bottom Margin"));
    }
    if (ImGui::IsItemActive()) {
      const float mp = horizontal ? io.MousePos.x : io.MousePos.y;
      u.ruler_drag = which;
      u.ruler_value = std::clamp(static_cast<double>(e == 0 ? mp - a0 : a1 - mp) / px_per_pt, 0.0, static_cast<double>((a1 - a0) / px_per_pt) * 0.45);
      const float shown = e == 0 ? a0 + static_cast<float>(u.ruler_value) * px_per_pt : a1 - static_cast<float>(u.ruler_value) * px_per_pt;
      if (horizontal) dl->AddLine(ImVec2(shown, band0), ImVec2(shown, band1 + S(4000)), IM_COL32(0x40, 0x40, 0x40, 200));
      else dl->AddLine(ImVec2(band0, shown), ImVec2(band1 + S(4000), shown), IM_COL32(0x40, 0x40, 0x40, 200));
    }
    if (ImGui::IsItemDeactivated() && u.ruler_drag == which) {
      const double v = u.ruler_value;
      s.engine->PostUser(TR("Margins"), [sheet, which, v](Document& d) {
        d.UpdatePageSetup(sheet, [which, v](model::PageSetup& p) {
          double* f[] = {&p.left, &p.right, &p.top, &p.bottom};
          *f[which] = v;
        });
      });
      u.ruler_drag = -1;
    }
    ImGui::PopID();
  }
}

void DrawPageLayoutView(AppState& s, const model::Sheet& sh, render::GridView& view, const render::OverlayGeom& g) {
  PageViewUi& u = PV();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImRect full = g.cells;
  dl->AddRectFilled(full.Min, full.Max, IM_COL32(0xE6, 0xE6, 0xE6, 255));
  const PrintSheetJob* job0 = JobFor(s, sh.id);
  if (!job0) return;
  // View > Gridlines: the cells' gridlines are on the pages (in light gray; they print only with Print Gridlines)
  const bool view_grid = s.Win(sh.id).gridlines;
  const PrintSheetJob* job = view_grid && !job0->ps.gridlines ? JobFor(s, sh.id, true) : job0;
  const engine::PrintLayout& L = job->layout;
  const model::PageSetup& ps = job->ps;
  ImGuiIO& io = ImGui::GetIO();
  // the mouse belongs to this view (the grid underneath is hidden)
  ImGui::SetCursorScreenPos(full.Min);
  ImGui::SetNextItemAllowOverlap();  // the header / footer sections, rulers and handles drawn over the pages take their clicks
  ImGui::InvisibleButton("##pagelayout", full.GetSize(), ImGuiButtonFlags_MouseButtonLeft);
  const bool hovered = ImGui::IsItemHovered();
  // Ctrl+wheel zooms through the sheet's own handler (the grid under the pages): zoomed here too, it went twice as far
  const float px_per_pt = view.zoom * ui::Dpi() * (96.0f / 72.0f);
  const bool headings = s.Win(sh.id).headings;  // View > Headings: column letters above each page, row numbers beside it
  const bool ruler = u.ruler;  // View > Ruler
  const float rs = ruler ? S(20) : 0.0f;
  const ImRect R(ImVec2(full.Min.x + rs, full.Min.y + rs), full.Max);
  const float pw = static_cast<float>(L.paper_w) * px_per_pt, ph = static_cast<float>(L.paper_h) * px_per_pt, gap = headings ? S(40) : S(18);
  const int n = std::max<int>(1, static_cast<int>(L.pages.size()));
  PageGrid pg = GridOf(*job);
  if (!pg.single) {
    // the active cell further right or down than the added pages: as many more pages as it takes (Excel's view goes on)
    PageCellMap first;
    GridPageMap(s, sh, *job, pg, 0, pg.nrows, &first);
    if (!first.rows.empty() && view.sel.act_r > first.rows.back().index) {
      const uint32_t per = std::max<uint32_t>(1, static_cast<uint32_t>(first.rows.size()));
      pg.extra_rows = std::min(200, 2 + static_cast<int>((view.sel.act_r - first.rows.back().index) / per));
    }
    GridPageMap(s, sh, *job, pg, pg.ncols, 0, &first);
    if (!first.cols.empty() && view.sel.act_c > first.cols.back().index) {
      const uint32_t per = std::max<uint32_t>(1, static_cast<uint32_t>(first.cols.size()));
      pg.extra_cols = std::min(100, 2 + static_cast<int>((view.sel.act_c - first.cols.back().index) / per));
    }
  }
  const int gcols = pg.Cols(), grows = pg.Rows(n);
  const float total_w = static_cast<float>(gcols) * (pw + gap) + gap, total_h = static_cast<float>(grows) * (ph + gap) + gap;
  ImVec2& sc = u.scroll[sh.id];
  if (hovered && io.MouseWheel != 0 && !io.KeyCtrl) {
    if (io.KeyShift) sc.x -= io.MouseWheel * S(90);
    else sc.y -= io.MouseWheel * S(90);
  }
  auto page_origin = [&](int gx, int gy, float ox, float oy) { return ImVec2(ox + gap + static_cast<float>(gx) * (pw + gap), oy + gap + static_cast<float>(gy) * (ph + gap)); };
  // the view follows the active cell when the keys (or Python) move it, as the sheet does
  {
    auto& last = u.last_active[sh.id];
    const std::pair<uint32_t, uint32_t> act{view.sel.act_r, view.sel.act_c};
    if (last != act) {
      if (!u.clicked_on_page && gcols * grows <= 4000) {
        PageCellMap m;
        bool found = false;
        for (int gy = 0; gy < grows && !found; ++gy) {
          for (int gx = 0; gx < gcols && !found; ++gx) {
            GridPageMap(s, sh, *job, pg, gx, gy, &m);
            double x0, y0, x1, y1;
            if (!m.CellRect(act.first, act.second, &x0, &y0, &x1, &y1)) continue;
            found = true;
            const ImVec2 o = page_origin(gx, gy, 0, 0);
            const float cx0 = o.x + static_cast<float>(x0) * px_per_pt, cy0 = o.y + static_cast<float>(y0) * px_per_pt;
            const float cx1 = o.x + static_cast<float>(x1) * px_per_pt, cy1 = o.y + static_cast<float>(y1) * px_per_pt;
            if (cx0 < sc.x) sc.x = cx0 - S(20);
            if (cx1 > sc.x + R.GetWidth()) sc.x = cx1 - R.GetWidth() + S(20);
            // a page's first row: the page's top (its header) comes into view with it, as Excel scrolls
            if (y0 <= job->ps.top + 1 && o.y < sc.y) sc.y = o.y - gap;
            else if (cy0 < sc.y) sc.y = cy0 - S(20);
            if (cy1 > sc.y + R.GetHeight()) sc.y = cy1 - R.GetHeight() + S(20);
          }
        }
      }
      last = act;
    }
    u.clicked_on_page = false;
  }
  // the header or footer being typed is brought on screen (Go to Footer on a page taller than the window)
  if (u.hf_scroll && u.hf_which >= 0 && u.hf_sheet == sh.id) {
    u.hf_scroll = false;
    for (int gy = 0; gy < grows; ++gy) {
      for (int gx = 0; gx < gcols; ++gx) {
        if (PageAt(*job, pg, gx, gy) != u.hf_page) continue;
        const ImVec2 o = page_origin(gx, gy, 0, 0);
        const double a = u.hf_which == 0 ? ps.header : L.paper_h - ps.bottom, b = u.hf_which == 0 ? ps.top : L.paper_h - ps.footer;
        const float y0 = o.y + static_cast<float>(a) * px_per_pt - S(40), y1 = o.y + static_cast<float>(b) * px_per_pt + S(40);
        if (y0 < sc.y) sc.y = y0;
        if (y1 > sc.y + R.GetHeight()) sc.y = y1 - R.GetHeight();
      }
    }
  }
  sc.x = std::clamp(sc.x, 0.0f, std::max(0.0f, total_w - R.GetWidth()));
  sc.y = std::clamp(sc.y, 0.0f, std::max(0.0f, total_h - R.GetHeight()));
  const float ox = R.Min.x + std::max(0.0f, (R.GetWidth() - total_w) / 2) - sc.x, oy = R.Min.y - sc.y;
  dl->PushClipRect(R.Min, R.Max, true);
  const int px_w = static_cast<int>(pw * io.DisplayFramebufferScale.x);
  int rendered = 0;
  auto& texes = u.pages[sh.id];
  const auto& v = view.sel;
  const Rect last_area = v.areas.empty() ? Rect::Cell(v.act_r, v.act_c) : v.areas.back();
  bool hf_over = false, hf_drawn = false, hf_started = false;
  if (u.sel_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) u.sel_drag = false;
  std::vector<ShownPage> shown;
  const ImU32 green = IM_COL32(0x21, 0x73, 0x46, 255);
  for (int gy = 0; gy < grows; ++gy) {
    for (int gx = 0; gx < gcols; ++gx) {
      const ImVec2 p0 = page_origin(gx, gy, ox, oy), p1(p0.x + pw, p0.y + ph);
      if (p1.x < R.Min.x || p0.x > R.Max.x || p1.y < R.Min.y || p0.y > R.Max.y) continue;
      const int i = PageAt(*job, pg, gx, gy);
      const bool added = i < 0;  // a "Click to add data" page
      dl->AddRectFilled(ImVec2(p0.x + S(3), p0.y + S(3)), ImVec2(p1.x + S(3), p1.y + S(3)), IM_COL32(0, 0, 0, 40));
      dl->AddRectFilled(p0, p1, IM_COL32_WHITE);
      {
        const std::string page_label = added ? std::string("Click to add data") : "Page " + std::to_string(i + 1);  // tests find a page as a person sees it
        ui::NoteUiItem(page_label.c_str(), ImRect(p0, p1));
      }
      PageCellMap map;
      GridPageMap(s, sh, *job, pg, gx, gy, &map);
      const bool have_map = !map.rows.empty() && !map.cols.empty();
      shown.push_back({p0, map});
      if (!added && !L.empty) {
        char key[128];
        std::snprintf(key, sizeof key, "%llu/%d/%d/%d", static_cast<unsigned long long>(s.snap->version), i, px_w, job == job0 ? 0 : 1);
        PageViewUi::PageTex& t = texes[i];
        if (t.key != key && rendered < 2) {  // a couple of pages per frame keeps scrolling smooth
          std::vector<uint8_t> rgba;
          int w = 0, h = 0;
          PrintInfo info = MakePrintInfo(s.file_path);
          info.page_number = (ps.first_page_number > 0 ? ps.first_page_number : 1) + i;
          info.page_count = n;
          if (RenderPagePreview(*s.snap, *job, i, info, px_w, &rgba, &w, &h)) {
            if (t.tex) ReleaseTexture(t.tex);
            t.tex = TextureFromRgba(rgba.data(), w, h);
          }
          t.key = key;
          ++rendered;
        } else if (t.key != key) {
          ui::RequestRedrawIn(0.0);
        }
        if (t.tex) dl->AddImage(t.tex, p0, p1);
      }
      // gridlines of the cells the picture doesn't hold (after the printed area, the added pages)
      if (view_grid && have_map) {
        const ImU32 gc = IM_COL32(0xD9, 0xD9, 0xD9, 255);
        for (const auto& rsl : map.rows) {
          for (const auto& csl : map.cols) {
            if (!added && !L.empty && rsl.index <= L.area.r1 && csl.index <= L.area.c1 && rsl.index >= L.area.r0 && csl.index >= L.area.c0) continue;
            const float x0 = p0.x + static_cast<float>(csl.pos) * px_per_pt, y0 = p0.y + static_cast<float>(rsl.pos) * px_per_pt;
            const float x1 = x0 + static_cast<float>(csl.size) * px_per_pt, y1 = y0 + static_cast<float>(rsl.size) * px_per_pt;
            dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y1), gc);
            dl->AddLine(ImVec2(x0, y1), ImVec2(x1, y1), gc);
          }
        }
      }
      if (added) {
        const char* t = TR("Click to add data");
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddRectFilled(ImVec2((p0.x + p1.x - ts.x) / 2 - S(6), p0.y + ph * 0.3f - S(3)), ImVec2((p0.x + p1.x + ts.x) / 2 + S(6), p0.y + ph * 0.3f + ts.y + S(3)),
                          IM_COL32_WHITE);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) / 2, p0.y + ph * 0.3f), IM_COL32(0x90, 0x90, 0x90, 255), t);
      }
      double x0, y0, x1, y1;
      // the selection on this page (the active cell's frame is drawn after)
      for (const Rect& a : v.areas) {
        ImVec2 q0, q1;
        if (a.IsCell() || !RangeOnPage(shown.back(), px_per_pt, a, &q0, &q1)) continue;
        dl->AddRectFilled(q0, q1, IM_COL32(0x21, 0x73, 0x46, 40));
        dl->AddRect(q0, q1, green, 0.0f, S(1.5f));
      }
      if (map.CellRect(v.act_r, v.act_c, &x0, &y0, &x1, &y1)) {
        dl->AddRect(ImVec2(p0.x + static_cast<float>(x0) * px_per_pt, p0.y + static_cast<float>(y0) * px_per_pt),
                    ImVec2(p0.x + static_cast<float>(x1) * px_per_pt, p0.y + static_cast<float>(y1) * px_per_pt), green, 0.0f, S(2));
      }
      // the fill handle at the selection's bottom-right corner, where that cell is
      if (map.CellRect(last_area.r1, last_area.c1, &x0, &y0, &x1, &y1) && !s.edit.active) {
        const ImVec2 hc(p0.x + static_cast<float>(x1) * px_per_pt, p0.y + static_cast<float>(y1) * px_per_pt);
        const ImRect hr(hc - ImVec2(S(3.5f), S(3.5f)), hc + ImVec2(S(3.5f), S(3.5f)));
        dl->AddRectFilled(hr.Min - ImVec2(1, 1), hr.Max + ImVec2(1, 1), IM_COL32_WHITE);
        dl->AddRectFilled(hr.Min, hr.Max, green);
        ui::NoteUiItem("Fill handle", hr);
        if (Config().cell_drag_and_drop && s.operator_mode && u.drag == 0 && hovered && hr.Contains(io.MousePos)) {
          ImGui::SetMouseCursor(render::kCursorFill);
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            u.drag = 1;
            u.drag_src = u.drag_dst = last_area;
          }
        }
      }
      // the selection's border: dragged, it moves the cells (Ctrl: copies them)
      {
        ImVec2 q0, q1;
        if (Config().cell_drag_and_drop && s.operator_mode && u.drag == 0 && hovered && !s.edit.active && RangeOnPage(shown.back(), px_per_pt, last_area, &q0, &q1)) {
          const ImVec2 m = io.MousePos;
          const float t = S(3);
          const bool inside_outer = m.x >= q0.x - t && m.x <= q1.x + t && m.y >= q0.y - t && m.y <= q1.y + t;
          const bool inside_inner = m.x > q0.x + t && m.x < q1.x - t && m.y > q0.y + t && m.y < q1.y - t;
          if (inside_outer && !inside_inner && !ImRect(q1 - ImVec2(S(5), S(5)), q1 + ImVec2(S(5), S(5))).Contains(m)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            uint32_t r = 0, c = 0;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && CellUnder(shown, px_per_pt, m, &r, &c)) {
              u.drag = 2;
              u.drag_src = u.drag_dst = last_area;
              u.grab_r = std::clamp(r, last_area.r0, last_area.r1) - last_area.r0;
              u.grab_c = std::clamp(c, last_area.c0, last_area.c1) - last_area.c0;
            }
          }
        }
      }
      // headings: the column letters above the page, the row numbers at its left (Excel's Page Layout view)
      if (headings && have_map) {
        const ImU32 hc = IM_COL32(0x60, 0x60, 0x60, 255);
        for (const auto& c : map.cols) {
          const float hx0 = p0.x + static_cast<float>(c.pos) * px_per_pt, hx1 = hx0 + static_cast<float>(c.size) * px_per_pt;
          const std::string lab = core::ColumnName(c.index);
          const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
          if (ts.x < hx1 - hx0) dl->AddText(ImVec2((hx0 + hx1 - ts.x) / 2, p0.y - ts.y - S(2)), hc, lab.c_str());
          dl->AddLine(ImVec2(hx0, p0.y - S(4)), ImVec2(hx0, p0.y - 1), hc);
        }
        for (const auto& r : map.rows) {
          const float hy0 = p0.y + static_cast<float>(r.pos) * px_per_pt, hy1 = hy0 + static_cast<float>(r.size) * px_per_pt;
          const std::string lab = std::to_string(r.index + 1);
          const ImVec2 ts = ImGui::CalcTextSize(lab.c_str());
          if (ts.y <= hy1 - hy0 + S(2)) dl->AddText(ImVec2(p0.x - ts.x - S(4), (hy0 + hy1 - ts.y) / 2), hc, lab.c_str());
        }
      }
      // the cell being typed: Excel types in the cell on the page (the grid's editor keeps the keys, out of sight)
      if (s.edit.active && s.edit.sheet == sh.id && !s.edit.in_formula_bar && have_map && map.CellRect(s.edit.r, s.edit.c, &x0, &y0, &x1, &y1)) {
        const ImVec2 e0(p0.x + static_cast<float>(x0) * px_per_pt, p0.y + static_cast<float>(y0) * px_per_pt);
        const float ew = std::max(static_cast<float>(x1 - x0) * px_per_pt, ImGui::CalcTextSize(s.edit.text.c_str()).x + S(8));
        const ImVec2 e1(e0.x + ew, p0.y + static_cast<float>(y1) * px_per_pt);
        dl->AddRectFilled(e0, e1, IM_COL32_WHITE);
        dl->AddRect(e0, e1, green, 0.0f, S(2));
        const float ty = (e0.y + e1.y - ImGui::GetTextLineHeight()) / 2;
        dl->AddText(ImVec2(e0.x + S(3), ty), IM_COL32_BLACK, s.edit.text.c_str());
        const int caret = std::clamp(s.edit.caret, 0, static_cast<int>(s.edit.text.size()));
        const float cx = e0.x + S(3) + ImGui::CalcTextSize(s.edit.text.c_str(), s.edit.text.c_str() + caret).x;
        if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) dl->AddLine(ImVec2(cx, ty), ImVec2(cx, ty + ImGui::GetTextLineHeight()), IM_COL32_BLACK);
        ui::RequestRedrawIn(0.3);
      }
      const int number = (ps.first_page_number > 0 ? ps.first_page_number : 1) + std::max(0, i);
      // "Add header" / "Add footer" on a page whose header / footer is empty (Excel's placeholders)
      if (!added) {
        for (int which = 0; which < 2; ++which) {
          if (u.hf_which == which && u.hf_page == i && u.hf_sheet == sh.id) continue;
          bool empty = true;
          for (int k = 0; k < 3; ++k) empty = empty && model::HeaderFooterText(ps, which * 3 + k, i, number).empty();
          if (!empty) continue;
          const char* t = which == 0 ? TR("Add header") : TR("Add footer");
          const ImVec2 ts = ImGui::CalcTextSize(t);
          const float hy = which == 0 ? p0.y + static_cast<float>(ps.header) * px_per_pt : p1.y - static_cast<float>(ps.footer) * px_per_pt - ts.y;
          dl->AddText(ImVec2((p0.x + p1.x - ts.x) / 2, hy), IM_COL32(0xA0, 0xA0, 0xA0, 255), t);
        }
      }
      // the header or footer being typed on this page
      if (!added && u.hf_which >= 0 && u.hf_sheet == sh.id && u.hf_page == i) {
        const bool head = u.hf_which == 0;
        const float lx = p0.x + static_cast<float>(ps.left) * px_per_pt, rx = p1.x - static_cast<float>(ps.right) * px_per_pt;
        const ImRect band = head ? ImRect(ImVec2(lx, p0.y + static_cast<float>(ps.header) * px_per_pt - S(4)), ImVec2(rx, p0.y + static_cast<float>(ps.top) * px_per_pt))
                                 : ImRect(ImVec2(lx, p1.y - static_cast<float>(ps.bottom) * px_per_pt), ImVec2(rx, p1.y - static_cast<float>(ps.footer) * px_per_pt + S(4)));
        dl->AddRectFilled(band.Min, band.Max, IM_COL32(0xFF, 0xFF, 0xFF, 255));
        dl->AddRect(band.Min, band.Max, IM_COL32(0x80, 0x80, 0x80, 255));
        hf_over = DrawPageHeaderFooterBoxes(band, head);
        hf_drawn = true;
        // a click in the margin being typed (not on a box) goes to the section under it, as in Excel
        const ImRect margin_band = head ? ImRect(ImVec2(lx, p0.y), ImVec2(rx, p0.y + static_cast<float>(ps.top) * px_per_pt))
                                        : ImRect(ImVec2(lx, p1.y - static_cast<float>(ps.bottom) * px_per_pt), ImVec2(rx, p1.y));
        if (!hf_over && hovered && margin_band.Contains(io.MousePos)) {
          hf_over = true;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            u.hf_box = std::clamp(static_cast<int>((io.MousePos.x - lx) / std::max(1.0f, (rx - lx) / 3)), 0, 2);
            u.hf_focus = true;
          }
        }
      }
      // clicks: a cell selects it (a drag selects a range, Shift extends, a double-click types in it); the header /
      // footer margins are typed in place; an added page selects its first cell
      if (hovered && u.drag == 0 && ImRect(p0, p1).Contains(io.MousePos) && !hf_over) {
        const double mx = (io.MousePos.x - p0.x) / px_per_pt, my = (io.MousePos.y - p0.y) / px_per_pt;
        const bool in_header = !added && my < ps.top, in_footer = !added && my > L.paper_h - ps.bottom;
        uint32_t r = 0, c = 0;
        const bool on_cell = !in_header && !in_footer && have_map && map.CellAt(mx, my, &r, &c);
        if ((in_header || in_footer) && !(u.hf_which >= 0 && u.hf_page == i && u.hf_sheet == sh.id && (in_header == (u.hf_which == 0)))) {
          // the three sections outlined, the one under the mouse darker (Excel)
          const float lx = p0.x + static_cast<float>(ps.left) * px_per_pt, rx = p1.x - static_cast<float>(ps.right) * px_per_pt, w3 = (rx - lx) / 3;
          const float hy0 = in_header ? p0.y + static_cast<float>(ps.header) * px_per_pt - S(2) : p1.y - static_cast<float>(ps.bottom) * px_per_pt + S(2);
          const float hy1 = in_header ? p0.y + static_cast<float>(ps.top) * px_per_pt - S(2) : p1.y - static_cast<float>(ps.footer) * px_per_pt + S(2);
          const double third = (L.paper_w - ps.left - ps.right) / 3;
          const int box = std::clamp(static_cast<int>((mx - ps.left) / std::max(1.0, third)), 0, 2);
          for (int k = 0; k < 3; ++k) {
            dl->AddRect(ImVec2(lx + w3 * static_cast<float>(k), hy0), ImVec2(lx + w3 * static_cast<float>(k + 1), hy1),
                        k == box ? IM_COL32(0x60, 0x60, 0x60, 255) : IM_COL32(0xC0, 0xC0, 0xC0, 255));
          }
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            CommitPageHeaderFooter(s);
            StartPageHeaderFooter(ps, sh.id, in_header ? 0 : 1, i, box);
            hf_started = true;
          }
        } else if (on_cell && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          CommitPageHeaderFooter(s);
          if (added) r = map.rows.front().index, c = map.cols.front().index;  // Excel: the page's first cell
          if (s.edit.active && !(s.edit.sheet == sh.id && s.edit.r == r && s.edit.c == c)) {
            render::GridActions a;  // a click on another cell keeps what was typed, as in Excel
            a.commit_edit = true;
            HandleGridActions(s, sh.id, a);
          }
          u.clicked_on_page = true;
          if (io.KeyShift) {
            view.sel.areas = {Rect{std::min(view.sel.anc_r, r), std::min(view.sel.anc_c, c), std::max(view.sel.anc_r, r), std::max(view.sel.anc_c, c)}};
            view.sel.act_r = view.sel.anc_r, view.sel.act_c = view.sel.anc_c;
          } else {
            view.sel.areas = {Rect::Cell(r, c)};
            view.sel.anc_r = view.sel.act_r = r;
            view.sel.anc_c = view.sel.act_c = c;
            u.sel_drag = !added;
          }
          view.sel.mode = render::SelectMode::Cells;
          if (!added && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
            u.sel_drag = false;
            StartEdit(s, false, {}, !Config().edit_directly_in_cell);
          }
        } else if (on_cell && u.sel_drag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          // dragging over the cells of the page: the range from the anchor
          view.sel.areas = {Rect{std::min(view.sel.anc_r, r), std::min(view.sel.anc_c, c), std::max(view.sel.anc_r, r), std::max(view.sel.anc_c, c)}};
          view.sel.mode = render::SelectMode::Cells;
          u.clicked_on_page = true;
        }
      }
    }
  }
  g_shown_last[sh.id] = {shown, px_per_pt};
  // the fill handle or the selection's border being dragged: the outline of where the cells go, done on release
  if (u.drag != 0) {
    uint32_t r = 0, c = 0;
    if (CellUnder(shown, px_per_pt, io.MousePos, &r, &c)) {
      const Rect src = u.drag_src;
      if (u.drag == 1) {
        // Excel's fill: along the direction the mouse went furthest past the selection
        const int64_t dr = r > src.r1 ? static_cast<int64_t>(r) - src.r1 : r < src.r0 ? static_cast<int64_t>(r) - src.r0 : 0;
        const int64_t dc = c > src.c1 ? static_cast<int64_t>(c) - src.c1 : c < src.c0 ? static_cast<int64_t>(c) - src.c0 : 0;
        Rect d = src;
        if (std::llabs(dr) >= std::llabs(dc)) {
          if (dr > 0) d.r1 = r;
          if (dr < 0) d.r0 = r;
        } else {
          if (dc > 0) d.c1 = c;
          if (dc < 0) d.c0 = c;
        }
        u.drag_dst = d;
        ImGui::SetMouseCursor(render::kCursorFill);
      } else {
        const uint32_t h = src.r1 - src.r0, w = src.c1 - src.c0;
        const uint32_t r0 = r >= u.grab_r ? r - u.grab_r : 0, c0 = c >= u.grab_c ? c - u.grab_c : 0;
        u.drag_dst = Rect{r0, c0, r0 + h, c0 + w};
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      }
    }
    for (const ShownPage& p : shown) {
      ImVec2 q0, q1;
      if (RangeOnPage(p, px_per_pt, u.drag_dst, &q0, &q1)) dl->AddRect(q0, q1, IM_COL32(0x60, 0x60, 0x60, 255), 0.0f, S(2));
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      render::GridActions a;
      if (u.drag == 1 && !(u.drag_dst == u.drag_src)) {
        a.autofill = true;
        a.autofill_src = u.drag_src;
        a.autofill_dest = u.drag_dst;
        a.autofill_ctrl = io.KeyCtrl;
        HandleGridActions(s, sh.id, a);
        view.sel.areas = {u.drag_dst};
      } else if (u.drag == 2 && !(u.drag_dst == u.drag_src)) {
        a.move_cells = true;
        a.move_copy = io.KeyCtrl;
        a.move_src = u.drag_src;
        a.move_r = u.drag_dst.r0;
        a.move_c = u.drag_dst.c0;
        HandleGridActions(s, sh.id, a);
        view.sel.areas = {u.drag_dst};
        view.sel.anc_r = view.sel.act_r = u.drag_dst.r0;
        view.sel.anc_c = view.sel.act_c = u.drag_dst.c0;
      }
      u.clicked_on_page = true;
      u.drag = 0;
    }
  }
  dl->PopClipRect();
  // View > Ruler: a ruler above each page column and beside each page row, the margins draggable on it
  if (ruler) {
    dl->AddRectFilled(full.Min, ImVec2(full.Max.x, R.Min.y), IM_COL32(0xF0, 0xF0, 0xF0, 255));
    dl->AddRectFilled(ImVec2(full.Min.x, R.Min.y), ImVec2(R.Min.x, full.Max.y), IM_COL32(0xF0, 0xF0, 0xF0, 255));
    dl->PushClipRect(ImVec2(R.Min.x, full.Min.y), ImVec2(full.Max.x, R.Min.y), true);
    for (int gx = 0; gx < gcols; ++gx) {
      const ImVec2 p0 = page_origin(gx, 0, ox, oy);
      if (p0.x + pw < R.Min.x || p0.x > R.Max.x) continue;
      DrawRuler(s, sh.id, dl, true, p0.x, p0.x + pw, static_cast<float>(ps.left), static_cast<float>(ps.right), full.Min.y + S(2), R.Min.y - S(2), px_per_pt, gx);
    }
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(full.Min.x, R.Min.y), ImVec2(R.Min.x, full.Max.y), true);
    for (int gy = 0; gy < grows; ++gy) {
      const ImVec2 p0 = page_origin(0, gy, ox, oy);
      if (p0.y + ph < R.Min.y || p0.y > R.Max.y) continue;
      DrawRuler(s, sh.id, dl, false, p0.y, p0.y + ph, static_cast<float>(ps.top), static_cast<float>(ps.bottom), full.Min.x + S(2), R.Min.x - S(2), px_per_pt, 1000 + gy);
    }
    dl->PopClipRect();
  }
  // the header / footer being typed lets go when its page scrolled away or something else on the pages was clicked
  // (a click on the ribbon's Header & Footer tab keeps it)
  if (u.hf_which >= 0 && u.hf_sheet == sh.id) {
    const bool clicked_away = !hf_over && !hf_started && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if ((!hf_drawn && !hf_started) || clicked_away || u.hf_box == -2 || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) CommitPageHeaderFooter(s);
  }
}

}  // namespace

bool PageLayoutRangeRect(uint32_t sheet, const Rect& a, ImRect* out) {
  const auto it = g_shown_last.find(sheet);
  if (it == g_shown_last.end()) return false;
  for (const ShownPage& p : it->second.first) {  // the page its first cell is on
    ImVec2 q0, q1;
    if (p.map.rows.empty() || p.map.cols.empty() || a.r0 < p.map.rows.front().index || a.r0 > p.map.rows.back().index ||
        a.c0 < p.map.cols.front().index || a.c0 > p.map.cols.back().index)
      continue;
    if (RangeOnPage(p, it->second.second, a, &q0, &q1)) {
      *out = ImRect(q0, q1);
      return true;
    }
  }
  return false;
}

void OpenPageSetupTab(AppState& s, int tab) { OpenPageSetup(s, tab); }

void DrawPageViews(AppState& s, uint32_t sheet_id, const render::OverlayGeom& g) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet_id) : nullptr;
  if (!sh) return;
  render::GridView& view = s.Win(sheet_id).view;
  if (view.page_view == 1) DrawBreakPreview(s, *sh, g);
  else if (view.page_view == 2) DrawPageLayoutView(s, *sh, view, g);
}

// ---- Page Layout tab, View tab, Header & Footer tab, Custom Views, document properties ----

namespace {

using json = nlohmann::json;

// the workbook store's view.* entries (msgpack, Python sees them in ThisWorkbook.Store)
json StoreJson(const model::Workbook& wb, const std::string& key) {
  auto it = wb.store->find(key);
  if (it == wb.store->end()) return json();
  try {
    return json::from_msgpack(it->second);
  } catch (...) {
    return json();
  }
}

void PostStoreJson(AppState& s, const char* label, const std::string& key, const json& j) {
  if (RefuseReadOnlyUi(s)) return;  // read-only mode: custom views and document properties stay as they are
  std::string bytes;
  if (!j.is_null()) {
    const std::vector<uint8_t> b = json::to_msgpack(j);
    bytes.assign(b.begin(), b.end());
  }
  s.engine->PostUser(label, [key, bytes](Document& d) {
    auto& st = d.wb_mut().store.mut();
    if (bytes.empty()) st.erase(key);
    else st[key] = bytes;
    d.wb_mut().dirty = true;
  });
}

// Width / Height of Scale to Fit: 0 Automatic, else that many pages
int FitPages(const model::PageSetup& ps, bool wide) { return ps.zoom > 0 ? 0 : (wide ? ps.fit_wide : ps.fit_tall); }

void SetFitPages(AppState& s, bool wide, int pages) {
  EditSetup(s, TR("Scale to Fit"), [wide, pages](model::PageSetup& p) {
    if (p.zoom > 0) p.fit_wide = p.fit_tall = 0;  // from a zoom: the other direction starts Automatic
    (wide ? p.fit_wide : p.fit_tall) = pages;
    p.zoom = p.fit_wide == 0 && p.fit_tall == 0 ? 100 : 0;  // both Automatic: back to a scale (Excel)
  });
}

std::string PagesLabel(int pages) {
  if (pages <= 0) return TR("Automatic");
  if (pages == 1) return TR("1 page");
  char b[32];
  std::snprintf(b, sizeof b, TR("%d pages"), pages);
  return b;
}

// the in-place header / footer being typed gets a field at its caret (the Header & Footer tab's buttons)
void HfInsertField(const char* code) {
  PageViewUi& u = PV();
  if (u.hf_which < 0) return;
  u.hf_box = std::clamp(u.hf_box, 0, 2);
  u.hf_pending = code;
  u.hf_focus = true;
}

const model::PageSetup* HfSetup(AppState& s) {
  const model::Sheet* sh = s.snap ? s.snap->Sheet(PV().hf_sheet) : nullptr;
  return sh ? &*sh->page_setup : nullptr;
}

// ---- Custom Views (View > Workbook Views): the views saved in the workbook (store key view.custom_views) ----
struct CustomViewsUi {
  int sel = 0;
  bool adding = false, focus = false;
  char name[128] = "";
  bool print = true, hidden = true;
};
CustomViewsUi& CV() {
  static CustomViewsUi c;
  return c;
}

json CaptureView(AppState& s, const std::string& name, bool print, bool hidden) {
  json v;
  v["name"] = name;
  v["sheet"] = s.focus_sheet;
  const render::GridView& gv = s.FocusView();
  json areas = json::array();
  for (const Rect& a : gv.sel.areas) areas.push_back({a.r0, a.c0, a.r1, a.c1});
  v["areas"] = areas;
  v["active"] = {gv.sel.act_r, gv.sel.act_c};
  v["scroll"] = {gv.v.first, gv.h.first};
  v["zoom"] = gv.zoom;
  v["page_view"] = gv.page_view;
  v["gridlines"] = s.Win(s.focus_sheet).gridlines;
  v["headings"] = s.Win(s.focus_sheet).headings;
  v["print"] = print;
  v["hidden"] = hidden;
  json sheets = json::array();
  for (const auto& sh : s.snap->wb.sheets) {
    json one;
    one["id"] = sh->id;
    if (hidden) {
      // the hidden rows and columns (as spans) of the used part of the sheet and a margin after it
      auto spans = [](const model::DimIndex& d, uint32_t limit) {
        json out = json::array();
        for (uint32_t i = 0; i < limit; ++i) {
          if (!d.IsHidden(i)) continue;
          uint32_t e = i;
          while (e + 1 < limit && d.IsHidden(e + 1)) ++e;
          out.push_back({i, e});
          i = e;
        }
        return out;
      };
      const uint32_t rl = std::min<uint32_t>(sh->row_limit, (sh->has_used ? sh->used.r1 : 0) + 1000);
      const uint32_t cl = std::min<uint32_t>(sh->col_limit, (sh->has_used ? sh->used.c1 : 0) + 200);
      one["rows"] = spans(*sh->rows, rl);
      one["cols"] = spans(*sh->cols, cl);
      one["row_limit"] = rl;
      one["col_limit"] = cl;
    }
    if (print) {
      const model::PageSetup& ps = *sh->page_setup;
      one["ps"] = {ps.landscape, ps.paper, ps.left, ps.right, ps.top, ps.bottom, ps.header, ps.footer, ps.zoom, ps.fit_wide, ps.fit_tall, ps.has_area,
                   ps.area.r0, ps.area.c0, ps.area.r1, ps.area.c1, ps.gridlines, ps.headings, ps.center_h, ps.center_v};
    }
    sheets.push_back(one);
  }
  v["sheets"] = sheets;
  return v;
}

void ShowView(AppState& s, const json& v) {
  // the sheets' hidden rows / columns and print settings first (one undo step), then the window
  if (v.contains("sheets")) {
    const json sheets = v["sheets"];
    s.engine->PostUser(TR("Custom Views"), [sheets](Document& d) {
      for (const json& one : sheets) {
        const uint32_t id = one.value("id", 0u);
        if (!d.S(id)) continue;
        if (one.contains("rows")) {
          const uint32_t rl = one.value("row_limit", 0u), cl = one.value("col_limit", 0u);
          if (rl) d.HideRows(id, 0, rl - 1, false);
          if (cl) d.HideCols(id, 0, cl - 1, false);
          for (const json& sp : one["rows"]) d.HideRows(id, sp[0].get<uint32_t>(), sp[1].get<uint32_t>(), true);
          for (const json& sp : one["cols"]) d.HideCols(id, sp[0].get<uint32_t>(), sp[1].get<uint32_t>(), true);
        }
        if (one.contains("ps") && one["ps"].size() >= 20) {
          const json p = one["ps"];
          d.UpdatePageSetup(id, [&p](model::PageSetup& ps) {
            ps.landscape = p[0].get<bool>();
            ps.paper = p[1].get<int>();
            ps.left = p[2].get<double>(), ps.right = p[3].get<double>(), ps.top = p[4].get<double>(), ps.bottom = p[5].get<double>();
            ps.header = p[6].get<double>(), ps.footer = p[7].get<double>();
            ps.zoom = p[8].get<int>(), ps.fit_wide = p[9].get<int>(), ps.fit_tall = p[10].get<int>();
            ps.has_area = p[11].get<bool>();
            ps.area = Rect{p[12].get<uint32_t>(), p[13].get<uint32_t>(), p[14].get<uint32_t>(), p[15].get<uint32_t>()};
            ps.gridlines = p[16].get<bool>(), ps.headings = p[17].get<bool>(), ps.center_h = p[18].get<bool>(), ps.center_v = p[19].get<bool>();
          });
        }
      }
    });
  }
  const uint32_t sheet = v.value("sheet", 0u);
  if (!s.snap->Sheet(sheet)) return;
  ActivateSheet(s, sheet);
  render::GridView& gv = s.Win(sheet).view;
  gv.sel.areas.clear();
  for (const json& a : v.value("areas", json::array())) gv.sel.areas.push_back(Rect{a[0].get<uint32_t>(), a[1].get<uint32_t>(), a[2].get<uint32_t>(), a[3].get<uint32_t>()});
  if (gv.sel.areas.empty()) gv.sel.areas.push_back(Rect{});
  if (v.contains("active")) {
    gv.sel.act_r = gv.sel.anc_r = v["active"][0].get<uint32_t>();
    gv.sel.act_c = gv.sel.anc_c = v["active"][1].get<uint32_t>();
  }
  gv.sel.mode = render::SelectMode::Cells;
  if (v.contains("scroll")) {
    gv.v.first = v["scroll"][0].get<int64_t>();
    gv.h.first = v["scroll"][1].get<int64_t>();
  }
  gv.zoom = std::clamp(v.value("zoom", 1.0f), 0.1f, 4.0f);
  gv.page_view = std::clamp(v.value("page_view", 0), 0, 2);
  s.Win(sheet).gridlines = v.value("gridlines", true);
  s.Win(sheet).headings = v.value("headings", true);
  s.show_gridlines = s.Win(sheet).gridlines;
  s.show_headings = s.Win(sheet).headings;
}

json Views(AppState& s) {
  json v = StoreJson(s.snap->wb, "view.custom_views");
  return v.is_array() ? v : json::array();
}

void AddView(AppState& s, const std::string& name, bool print, bool hidden) {
  json all = Views(s);
  json v = CaptureView(s, name, print, hidden);
  bool replaced = false;
  for (json& e : all) {
    if (e.value("name", "") == name) e = v, replaced = true;
  }
  if (!replaced) all.push_back(v);
  PostStoreJson(s, TR("Custom Views"), "view.custom_views", all);
}

void DeleteView(AppState& s, const std::string& name) {
  json all = Views(s), kept = json::array();
  for (const json& e : all) {
    if (e.value("name", "") != name) kept.push_back(e);
  }
  PostStoreJson(s, TR("Custom Views"), "view.custom_views", kept.empty() ? json() : kept);
}

// ---- File > Info: the built-in document properties (store key view.docprops) ----
const char* const kDocProps[] = {"Title", "Subject", "Author", "Manager", "Company", "Category", "Keywords", "Comments", "Status"};

std::string UserName() {
  wchar_t name[256];
  DWORD n = 256;
  return ::GetUserNameW(name, &n) ? ToUtf8(name) : std::string();
}

std::string DateText(double epoch) {
  if (epoch <= 0) return std::string();
  const std::time_t t = static_cast<std::time_t>(epoch);
  std::tm tmv{};
  localtime_s(&tmv, &t);
  char b[64];
  std::strftime(b, sizeof b, "%Y/%m/%d %H:%M", &tmv);
  return b;
}

}  // namespace

json DocPropsJson(const model::Workbook& wb) {
  json j = StoreJson(wb, "view.docprops");
  return j.is_object() ? j : json::object();
}

void StampDocProperties(engine::Document& d) {
  // on save (Excel): Created once, Last Modified By and the save time every time; a new workbook's Author is its creator
  json j = DocPropsJson(d.wb());
  const double now = static_cast<double>(std::time(nullptr));
  if (!j.contains("Created")) j["Created"] = now;
  if (!j.contains("Author")) j["Author"] = UserName();
  j["LastModifiedBy"] = UserName();
  j["LastSaved"] = now;
  const std::vector<uint8_t> b = json::to_msgpack(j);
  d.wb_mut().store.mut()["view.docprops"] = std::string(b.begin(), b.end());
}

// File > Info's Properties column: Size, Title, Tags, Categories (typed in place), the dates, Author and Last
// Modified By; Properties > Advanced Properties opens the Summary dialog
void DrawInfoProperties(AppState& s) {
  const json j = DocPropsJson(s.snap->wb);
  ImGui::SeparatorText(TR("Properties"));
  if (ImGui::SmallButton(TR("Advanced Properties"))) {
    ui::ribbon::St().backstage = false;
    s.OpenDialog(Dialog::DocProperties);
  }
  auto row = [](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(S(150));
  };
  std::error_code ec;
  const auto size = s.file_path.empty() ? 0 : std::filesystem::file_size(s.file_path, ec);
  row(TR("Size"));
  if (size && !ec) ImGui::Text("%.1f KB", static_cast<double>(size) / 1024.0);
  else ImGui::TextDisabled("%s", TR("Not saved yet"));
  // Title / Tags / Categories: typed in place; a change goes in when the box is left (one undo step)
  struct Edit {
    const char* label;
    const char* key;
    const char* hint;
  };
  static const Edit kEdits[] = {{TR("Title"), "Title", TR("Add a title")}, {TR("Tags"), "Keywords", TR("Add a tag")}, {TR("Categories"), "Category", TR("Add a category")}};
  static char bufs[3][256];
  static std::string shown[3];
  for (int k = 0; k < 3; ++k) {
    const std::string cur = j.value(kEdits[k].key, std::string());
    ImGui::PushID(k);
    if (shown[k] != cur && ImGui::GetActiveID() != ImGui::GetID("##prop")) {
      std::snprintf(bufs[k], sizeof bufs[k], "%s", cur.c_str());
      shown[k] = cur;
    }
    row(kEdits[k].label);
    ImGui::SetNextItemWidth(S(260));
    ui::NoteUiItem(kEdits[k].key, ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(260), ImGui::GetFrameHeight())));
    ImGui::InputTextWithHint("##prop", kEdits[k].hint, bufs[k], sizeof bufs[k]);
    if (ImGui::IsItemDeactivatedAfterEdit() && cur != bufs[k]) {
      json n = j;
      n[kEdits[k].key] = std::string(bufs[k]);
      shown[k] = bufs[k];
      PostStoreJson(s, TR("Properties"), "view.docprops", n);
    }
    ImGui::PopID();
  }
  ImGui::SeparatorText(TR("Related Dates"));
  row(TR("Last Modified"));
  ImGui::TextUnformatted(DateText(j.value("LastSaved", 0.0)).c_str());
  row(TR("Created"));
  ImGui::TextUnformatted(DateText(j.value("Created", 0.0)).c_str());
  ImGui::SeparatorText(TR("Related People"));
  row(TR("Author"));
  ImGui::TextUnformatted(j.value("Author", UserName()).c_str());
  row(TR("Last Modified By"));
  ImGui::TextUnformatted(j.value("LastModifiedBy", std::string()).c_str());
}

namespace {

// File > Info > Properties > Advanced Properties: Excel's Summary tab (and the dates on Statistics); OK writes them
void DrawDocPropertiesDialog(AppState& s) {
  static char bufs[std::size(kDocProps)][512];
  if (s.dialog == Dialog::DocProperties && s.dialog_open_request) {
    const json j = DocPropsJson(s.snap->wb);
    for (size_t k = 0; k < std::size(kDocProps); ++k) std::snprintf(bufs[k], sizeof bufs[k], "%s", j.value(kDocProps[k], std::string()).c_str());
  }
  if (!BeginDlg(s, Dialog::DocProperties, TR("Properties###yc_dlg_docprops"), 460)) return;
  bool close = false;
  if (BeginDlgTabBar("##dptabs")) {
    if (ImGui::BeginTabItem(TR("Summary"))) {
      const char* labels[] = {TR("Title:"), TR("Subject:"), TR("Author:"), TR("Manager:"), TR("Company:"), TR("Category:"), TR("Keywords:"), TR("Comments:"), TR("Status:")};
      for (size_t k = 0; k < std::size(kDocProps); ++k) {
        ImGui::PushID(static_cast<int>(k));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(labels[k]);
        ImGui::SameLine(S(100));
        ImGui::SetNextItemWidth(S(320));
        ui::NoteUiItem(kDocProps[k], ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(320), ImGui::GetFrameHeight())));
        if (k == 7) ImGui::InputTextMultiline("##v", bufs[k], sizeof bufs[k], ImVec2(S(320), S(60)));
        else ImGui::InputText("##v", bufs[k], sizeof bufs[k]);
        ImGui::PopID();
      }
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(TR("Statistics"))) {
      const json j = DocPropsJson(s.snap->wb);
      ImGui::Text("%s %s", TR("Created:"), DateText(j.value("Created", 0.0)).c_str());
      ImGui::Text("%s %s", TR("Modified:"), DateText(j.value("LastSaved", 0.0)).c_str());
      ImGui::Text("%s %s", TR("Last saved by:"), j.value("LastModifiedBy", std::string()).c_str());
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::Separator();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0))) {
    json j = DocPropsJson(s.snap->wb);
    for (size_t k = 0; k < std::size(kDocProps); ++k) j[kDocProps[k]] = std::string(bufs[k]);
    PostStoreJson(s, TR("Properties"), "view.docprops", j);
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// View > Custom Views: the list with Show / Close / Add... / Delete; Add asks for a name and what to include
void DrawCustomViewsDialog(AppState& s) {
  CustomViewsUi& c = CV();
  if (!BeginDlg(s, Dialog::CustomViews, TR("Custom Views###yc_dlg_customviews"), 420)) return;
  bool close = false;
  const json all = Views(s);
  if (!c.adding) {
    ImGui::TextUnformatted(TR("Views:"));
    if (ImGui::BeginListBox("##views", ImVec2(S(280), S(160)))) {
      for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        const std::string name = all[static_cast<size_t>(i)].value("name", "");
        if (ImGui::Selectable(name.c_str(), c.sel == i, ImGuiSelectableFlags_AllowDoubleClick)) {
          c.sel = i;
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ShowView(s, all[static_cast<size_t>(i)]);
            close = true;
          }
        }
      }
      ImGui::EndListBox();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    const bool any = c.sel >= 0 && c.sel < static_cast<int>(all.size());
    ImGui::BeginDisabled(!any);
    if (DlgOkButton(TR("Show"), ImVec2(S(90), 0)) && any) {
      ShowView(s, all[static_cast<size_t>(c.sel)]);
      close = true;
    }
    ImGui::EndDisabled();
    if (ImGui::Button(TR("Close"), ImVec2(S(90), 0))) close = true;
    if (ImGui::Button(TR("Add..."), ImVec2(S(90), 0))) {
      c.adding = true;
      c.focus = true;
      c.name[0] = 0;
      c.print = c.hidden = true;
    }
    ImGui::BeginDisabled(!any);
    if (ImGui::Button(TR("Delete"), ImVec2(S(90), 0)) && any) {
      DeleteView(s, all[static_cast<size_t>(c.sel)].value("name", ""));
      c.sel = std::max(0, c.sel - 1);
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
  } else {
    // Add View
    ImGui::TextUnformatted(TR("Name:"));
    ImGui::SetNextItemWidth(S(300));
    if (c.focus) ImGui::SetKeyboardFocusHere();
    c.focus = false;
    ui::NoteUiItem("View name", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(300), ImGui::GetFrameHeight())));
    const bool enter = ImGui::InputText("##name", c.name, sizeof c.name, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SeparatorText(TR("Include in view"));
    ImGui::Checkbox(TR("Print settings"), &c.print);
    ImGui::Checkbox(TR("Hidden rows, columns and filter settings"), &c.hidden);
    if ((DlgOkButton(TR("OK"), ImVec2(S(80), 0)) || enter) && c.name[0]) {
      AddView(s, c.name, c.print, c.hidden);
      close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
  }
  if (close) {
    c.adding = false;
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// Header & Footer > Format Picture from the ribbon (Page Layout view): the picture of the section being typed
void DrawHfPictureFromRibbon(AppState& s) {
  PageViewUi& u = PV();
  static bool was_open = false;
  if (u.hf_pic_popup) {
    ImGui::OpenPopup("##hfpic");
    u.hf_pic_popup = false;
  }
  const bool open = ImGui::IsPopupOpen("##hfpic");
  if (open) DrawHfPicturePopup(u.hf_pic_edit);
  if (was_open && !ImGui::IsPopupOpen("##hfpic") && u.hf_pic_section >= 0) {
    const int k = u.hf_pic_section;
    const model::PageSetup::HfPicture pic = u.hf_pic_edit;
    s.focus_sheet = u.hf_sheet;
    EditSetup(s, TR("Format Picture"), [k, pic](model::PageSetup& p) { p.hf_pictures[k] = pic; });
    u.hf_pic_section = -1;
  }
  was_open = open || ImGui::IsPopupOpen("##hfpic");
}

void RegisterLayoutCommands(AppState& s) {
  auto& R = ui::Commands();
  auto make = [](const std::string& id, const std::string& label, const char* icon = nullptr, const char* desc = "") {
    ui::Command c;
    c.id = id;
    c.label = label;
    c.icon = icon;
    c.tip_desc = desc;
    return c;
  };
  // ---- Scale to Fit: Width / Height ----
  R.Add(make("ScaleWidthMenu", TR("Width:"), ui::icon::ScaleWidth, TR("Shrink the printout's width to fit a number of pages."))).execute = [](ui::Command&) {};
  R.Add(make("ScaleHeightMenu", TR("Height:"), ui::icon::ScaleHeight, TR("Shrink the printout's height to fit a number of pages."))).execute = [](ui::Command&) {};
  for (int k = 0; k <= 9; ++k) {
    R.Add(make("ScaleWidth" + std::to_string(k), PagesLabel(k))).execute = [&s, k](ui::Command&) { SetFitPages(s, true, k); };
    R.Add(make("ScaleHeight" + std::to_string(k), PagesLabel(k))).execute = [&s, k](ui::Command&) { SetFitPages(s, false, k); };
  }
  R.Add(make("ScaleMorePages", TR("More Pages..."))).execute = [&s](ui::Command&) { OpenPageSetup(s, 0); };
  // ---- View > Show > Ruler (Page Layout view) ----
  R.Add(make("ToggleRuler", TR("Ruler"), nullptr, TR("Show the rulers beside the pages in Page Layout view."))).execute = [](ui::Command&) {
    // the ribbon's check box flipped Context().ruler already; run by its id it flips it here
    auto& cx = ui::ribbon::Context();
    PV().ruler = cx.ruler != PV().ruler ? cx.ruler : !PV().ruler;
    cx.ruler = PV().ruler;
  };
  // ---- View > Custom Views ----
  R.Add(make("CustomViews", TR("Custom\nViews"), ui::icon::CustomViews, TR("Save the current display and print settings as a view you can go back to."))).execute =
      [&s](ui::Command&) {
        CV().adding = false;
        s.OpenDialog(Dialog::CustomViews);
      };
  // ---- Header & Footer (contextual tab while a header or footer is typed in Page Layout view) ----
  struct Field {
    const char* id;
    const char* label;
    const char* code;
  };
  static const Field kFields[] = {{"HfPageNumber", TR("Page\nNumber"), "&[Page]"},     {"HfNumberOfPages", TR("Number\nof Pages"), "&[Pages]"},
                                  {"HfCurrentDate", TR("Current\nDate"), "&[Date]"},   {"HfCurrentTime", TR("Current\nTime"), "&[Time]"},
                                  {"HfFilePath", TR("File\nPath"), "&[Path]&[File]"}, {"HfFileName", TR("File\nName"), "&[File]"},
                                  {"HfSheetName", TR("Sheet\nName"), "&[Tab]"}};
  for (const Field& f : kFields) {
    const char* code = f.code;
    R.Add(make(f.id, f.label, ui::icon::Margins)).execute = [code](ui::Command&) { HfInsertField(code); };
  }
  R.Add(make("HfPicture", TR("Picture"), ui::icon::Picture, TR("Insert a picture in this section (&[Picture]); Format Picture sets its size."))).execute = [&s](ui::Command&) {
    PageViewUi& u = PV();
    if (u.hf_which < 0) return;
    const int which = u.hf_which, box = std::clamp(u.hf_box, 0, 2);
    std::string bytes;
    if (!PickPictureFile(s, &bytes)) return;
    const int k = which * 3 + box;
    s.focus_sheet = u.hf_sheet;
    EditSetup(s, TR("Insert Picture"), [k, bytes](model::PageSetup& p) {
      p.hf_pictures[k].image = bytes;
      p.hf_pictures[k].w = p.hf_pictures[k].h = 0;
    });
    u.hf_box = box;
    if (std::string(u.hf_buf[box]).find("&[Picture]") == std::string::npos) HfInsertField("&[Picture]");
  };
  R.Add(make("HfFormatPicture", TR("Format\nPicture"), ui::icon::ImageEdit)).execute = [&s](ui::Command&) {
    PageViewUi& u = PV();
    const model::PageSetup* ps = HfSetup(s);
    if (u.hf_which < 0 || !ps) return;
    const int k = u.hf_which * 3 + std::clamp(u.hf_box, 0, 2);
    if (ps->hf_pictures[k].image.empty()) return;
    u.hf_pic_edit = ps->hf_pictures[k];
    u.hf_pic_section = k;
    u.hf_pic_popup = true;
  };
  R.Add(make("HfGoToHeader", TR("Go to\nHeader"), ui::icon::GoToHeader)).execute = [&s](ui::Command&) {
    PageViewUi& u = PV();
    const model::PageSetup* ps = HfSetup(s);
    if (u.hf_which < 0 || !ps) return;
    if (u.hf_which == 0) {  // already in the header: its section keeps the keyboard
      u.hf_focus = true;
      return;
    }
    const int page = u.hf_page;
    const uint32_t sheet = u.hf_sheet;
    const model::PageSetup copy = *ps;
    CommitPageHeaderFooter(s);
    StartPageHeaderFooter(copy, sheet, 0, page, 1);
  };
  R.Add(make("HfGoToFooter", TR("Go to\nFooter"), ui::icon::GoToFooter)).execute = [&s](ui::Command&) {
    PageViewUi& u = PV();
    const model::PageSetup* ps = HfSetup(s);
    if (u.hf_which < 0 || !ps) return;
    if (u.hf_which == 1) {
      u.hf_focus = true;
      return;
    }
    const int page = u.hf_page;
    const uint32_t sheet = u.hf_sheet;
    const model::PageSetup copy = *ps;
    CommitPageHeaderFooter(s);
    StartPageHeaderFooter(copy, sheet, 1, page, 1);
  };
  // Options: the check box flipped the context's bool (ribbon) or, run by id, the setting flips
  struct Opt {
    const char* id;
    const char* label;
    int which;
  };
  static const Opt kOpts[] = {{"HfDifferentFirst", TR("Different First Page"), 0},
                              {"HfDifferentOddEven", TR("Different Odd & Even Pages"), 1},
                              {"HfScaleWithDoc", TR("Scale with Document"), 2},
                              {"HfAlignMargins", TR("Align with Page Margins"), 3}};
  for (const Opt& o : kOpts) {
    const int which = o.which;
    R.Add(make(o.id, o.label)).execute = [&s, which](ui::Command&) {
      const model::Sheet* sh = s.snap ? s.snap->Sheet(PV().hf_which >= 0 ? PV().hf_sheet : s.focus_sheet) : nullptr;
      if (!sh) return;
      auto& cx = ui::ribbon::Context();
      const model::PageSetup& ps = *sh->page_setup;
      const bool cur[] = {ps.different_first, ps.different_odd_even, !ps.no_scale_with_doc, !ps.no_align_margins};
      bool* ctx[] = {&cx.hf_first, &cx.hf_odd_even, &cx.hf_scale, &cx.hf_align};
      const bool want = *ctx[which] != cur[which] ? *ctx[which] : !cur[which];
      *ctx[which] = want;
      // the text being typed goes in first (it belongs to the pages it was typed for)
      CommitPageHeaderFooter(s);
      s.focus_sheet = sh->id;
      EditSetup(s, TR("Header & Footer"), [which, want](model::PageSetup& p) {
        if (which == 0) p.different_first = want;
        if (which == 1) p.different_odd_even = want;
        if (which == 2) p.no_scale_with_doc = !want;
        if (which == 3) p.no_align_margins = !want;
      });
    };
  }
  // Header / Footer presets (Excel's drop-downs on the tab)
  R.Add(make("HfHeaderMenu", TR("Header"), ui::icon::Header)).execute = [](ui::Command&) {};
  R.Add(make("HfFooterMenu", TR("Footer"), ui::icon::Footer)).execute = [](ui::Command&) {};
  const auto presets = HfPresets();
  for (size_t k = 0; k < presets.size(); ++k) {
    for (int which = 0; which < 2; ++which) {
      const std::string id = std::string(which == 0 ? "HfHeader" : "HfFooter") + std::to_string(k);
      R.Add(make(id, "")).execute = [&s, k, which](ui::Command&) {
        const auto ps = HfPresets();
        const HfPreset& p = ps[k];
        PageViewUi& u = PV();
        if (u.hf_which == which) {  // the sections being typed take the preset
          const char* parts[] = {p.l, p.c, p.r};
          ImGui::ClearActiveID();  // the active box would keep its own text
          for (int b = 0; b < 3; ++b) std::snprintf(u.hf_buf[b], sizeof u.hf_buf[b], "%s", model::HeaderCodesToDisplay(parts[b]).c_str());
          u.hf_focus = true;
          return;
        }
        if (u.hf_which >= 0) s.focus_sheet = u.hf_sheet;
        const std::string l = p.l, c = p.c, r = p.r;
        EditSetup(s, which == 0 ? TR("Header") : TR("Footer"), [which, l, c, r](model::PageSetup& x) {
          (which == 0 ? x.header_left : x.footer_left) = l;
          (which == 0 ? x.header_center : x.footer_center) = c;
          (which == 0 ? x.header_right : x.footer_right) = r;
        });
      };
    }
  }
}

// the ribbon's boxes of the Page Layout tab (layout_box): the Scale spinner and Sheet Options' check boxes
// the scale Fit To gives, as the ribbon's Scale box shows it: a layout of the sheet, kept while its inputs are the
// same objects and made again on a clock that waits for a multiple of its own cost, as the page breaks' layout is. Made
// every frame, a large sheet's layout froze the window with the Page Layout tab open (chaos seed 6803)
int FitScalePercent(AppState& s, const model::Sheet& sh, const model::PageSetup& ps) {
  static uint32_t sheet = ~0u;
  static std::array<const void*, 6> from{};
  static uint64_t version = ~0ull;
  static double at = 0, cost = 0;
  static int pct = 100;
  const std::array<const void*, 6> now_from{&*sh.rows, &*sh.cols, &ps, &*sh.shapes, &*sh.charts, &*sh.sparklines};
  const double now = ImGui::GetTime();
  const double every = std::max(0.5, 20 * cost);
  const uint64_t v = s.snap ? s.snap->version : 0;
  if (sheet != sh.id || from != now_from || (version != v && now - at > every)) {
    const auto t0 = std::chrono::steady_clock::now();
    pct = static_cast<int>(std::lround(engine::LayoutPages(sh, ps).scale * 100));
    cost = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    sheet = sh.id;
    from = now_from;
    version = v;
    at = ImGui::GetTime() + cost;
  }
  return pct;
}

void DrawLayoutBox(AppState& s, const std::string& id, const ImRect& r) {
  const model::PageSetup* ps = ActiveSetup(s);
  if (!ps) return;
  ImGui::PushID(id.c_str());
  const float fh = ImGui::GetFrameHeight();
  const float y = r.GetCenter().y - fh / 2;
  if (id == "ScaleAdjust") {
    // Scale: the zoom percent, only while Width and Height are Automatic (else the scale Fit to gives, grayed)
    ImGui::SetCursorScreenPos(ImVec2(r.Min.x + S(4), y + (fh - ImGui::GetTextLineHeight()) / 2 - ImGui::GetStyle().FramePadding.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Scale:"));
    ImGui::SameLine();
    const bool fit = ps->zoom == 0;
    int pct = ps->zoom;
    if (fit) {
      const model::Sheet* sh = s.FocusSheet();
      pct = sh ? FitScalePercent(s, *sh, *ps) : 100;
    }
    ImGui::SetNextItemWidth(r.Max.x - ImGui::GetCursorScreenPos().x - S(4));
    ui::NoteUiItem("Scale", ImRect(ImGui::GetCursorScreenPos(), ImVec2(r.Max.x - S(4), ImGui::GetCursorScreenPos().y + fh)));
    ImGui::BeginDisabled(fit);
    if (ImGui::InputInt("##pct", &pct, 5, 10, ImGuiInputTextFlags_EnterReturnsTrue) || (ImGui::IsItemDeactivatedAfterEdit() && !fit)) {
      const int z = std::clamp(pct, 10, 400);
      EditSetup(s, TR("Scale"), [z](model::PageSetup& p) { p.zoom = z; });
    }
    ImGui::EndDisabled();
  } else {
    // Sheet Options: "Gridlines" / "Headings" over two columns of View / Print check boxes
    const float half = r.GetWidth() / 2;
    if (id == "SheetOptionsHead") {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
      dl->AddText(ImVec2(r.Min.x + S(4), r.GetCenter().y - ImGui::GetFontSize() / 2), col, TR("Gridlines"));
      dl->AddText(ImVec2(r.Min.x + half + S(4), r.GetCenter().y - ImGui::GetFontSize() / 2), col, TR("Headings"));
    } else {
      const bool print = id == "SheetOptionsPrint";
      for (int k = 0; k < 2; ++k) {
        ImGui::PushID(k);
        ImGui::SetCursorScreenPos(ImVec2(r.Min.x + half * static_cast<float>(k) + S(2), y));
        bool v = print ? (k == 0 ? ps->gridlines : ps->headings) : (k == 0 ? s.show_gridlines : s.show_headings);
        const char* label = print ? TR("Print") : TR("View");
        static const char* const kIds[2][2] = {{"Gridlines View", "Headings View"}, {"Gridlines Print", "Headings Print"}};
        if (ImGui::Checkbox(label, &v)) {
          if (print) ui::Commands().Execute(k == 0 ? "PrintGridlines" : "PrintHeadings");
          else (k == 0 ? s.show_gridlines : s.show_headings) = v;  // the View tab's Gridlines / Headings (this sheet's window)
        }
        ui::NoteUiItem(kIds[print ? 1 : 0][k], ImGui::GetCurrentContext()->LastItemData.Rect);
        ImGui::PopID();
      }
    }
  }
  ImGui::PopID();
}

// a ribbon item found by its command (to change its menu or its command as the state changes)
template <typename F>
void ForRibbonItem(const std::string& cmd, F fn) {
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& g : t.groups) {
      for (auto& b : g.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == cmd) fn(it);
        }
      }
    }
  }
}

}  // namespace

// Page Layout > Themes / Colors / Fonts: a picture of each entry (the theme's accents, the color set's swatches, the
// fonts' "Aa"), and the sheet drawn with the entry under the mouse (Excel's live preview)
namespace {

struct ThemePreview {
  bool active = false;   // the current theme is a preview (put back at the next frame's start)
  model::Theme theme;
};
ThemePreview& TP() {
  static ThemePreview t;
  return t;
}

float ThemeMenuPicture(AppState& s, const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) {
  int which = -1;
  size_t index = 0;
  for (const auto& [prefix, w] : {std::pair<const char*, int>{"ThemeApply", 0}, {"ThemeColors", 1}, {"ThemeFonts", 2}}) {
    const size_t n = std::strlen(prefix);
    if (cmd.compare(0, n, prefix) == 0 && cmd.size() > n && std::isdigit(static_cast<unsigned char>(cmd[n]))) {
      which = w;
      index = static_cast<size_t>(std::atoi(cmd.c_str() + n));
    }
  }
  if (which < 0 || !s.snap) return -1.0f;
  model::Theme t = *s.snap->wb.theme;
  if (which == 0 && index < model::BuiltinThemes().size()) t = model::BuiltinThemes()[index];
  if (which == 1 && index < model::BuiltinThemeColors().size()) t.colors = model::BuiltinThemeColors()[index].colors, t.colors_name = model::BuiltinThemeColors()[index].name;
  if (which == 2 && index < model::BuiltinThemeFonts().size()) {
    t.major_font = model::BuiltinThemeFonts()[index].major;
    t.minor_font = model::BuiltinThemeFonts()[index].minor;
    t.fonts_name = model::BuiltinThemeFonts()[index].name;
  }
  const float w = which == 1 ? S(96) : S(56);
  if (!dl) return w;
  auto col = [&](int k) {
    const uint32_t c = t.colors[static_cast<size_t>(k)];
    return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
  };
  const ImVec2 p0(at.x, at.y + S(4)), p1(at.x + w, at.y + h - S(4));
  if (which == 1) {
    // the color set: text / background dark and light, then the six accents
    const int ks[] = {0, 2, 3, 4, 5, 6, 7, 8};
    const float sw = (p1.x - p0.x) / 8;
    for (int i = 0; i < 8; ++i) dl->AddRectFilled(ImVec2(p0.x + sw * static_cast<float>(i), p0.y), ImVec2(p0.x + sw * static_cast<float>(i + 1) - 1, p1.y), col(ks[i]));
  } else {
    dl->AddRectFilled(p0, p1, IM_COL32_WHITE);
    dl->AddRect(p0, p1, IM_COL32(0xA0, 0xA0, 0xA0, 255));
    dl->AddText(ImVec2(p0.x + S(5), p0.y + S(2)), which == 2 ? IM_COL32_BLACK : col(2), "Aa");
    if (which == 0) {
      const float bw = (p1.x - p0.x - S(8)) / 6;
      for (int i = 0; i < 6; ++i) dl->AddRectFilled(ImVec2(p0.x + S(4) + bw * static_cast<float>(i), p1.y - S(8)), ImVec2(p0.x + S(4) + bw * static_cast<float>(i + 1) - 1, p1.y - S(3)), col(4 + i));
    }
  }
  // live preview: the sheet shows this theme while the entry is under the mouse
  if (ImGui::IsItemHovered()) {
    TP().active = true;
    TP().theme = t;
    model::SetCurrentTheme(t);
  }
  return w;
}

}  // namespace

// every frame: the Page Layout tab's menus and labels follow the sheet; the Header & Footer tab while one is typed
void UpdateLayoutContext(AppState& s) {
  auto& cx = ui::ribbon::Context();
  PageViewUi& u = PV();
  static bool once = false;
  if (!once) {
    once = true;
    cx.layout_box = [&s](const std::string& id, const ImRect& r) { DrawLayoutBox(s, id, r); };
    auto& st0 = ui::ribbon::St();
    st0.menu_picture = [&s, prev = st0.menu_picture](const std::string& cmd, ImDrawList* dl, ImVec2 at, float h) -> float {
      const float w = ThemeMenuPicture(s, cmd, dl, at, h);
      if (w >= 0) return w;
      return prev ? prev(cmd, dl, at, h) : 0.0f;
    };
  }
  // the live preview of a theme lasts while its entry is under the mouse (the menu sets it again each frame)
  if (TP().active && s.snap) {
    TP().active = false;
    model::SetCurrentTheme(*s.snap->wb.theme);
  }
  cx.ruler = u.ruler;
  if (auto* c = ui::Commands().Find("ToggleRuler")) c->enabled = s.FocusView().page_view == 2;
  // the printer's papers (looked up once on a worker thread) become the Size menu
  static std::future<std::vector<PaperEntry>> papers;
  static bool papers_asked = false;
  if (!papers_asked) {
    papers_asked = true;
    papers = std::async(std::launch::async, LoadPapers);
  }
  if (papers.valid() && papers.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    Papers() = papers.get();
    std::vector<std::string> menu;
    for (const PaperEntry& p : Papers()) {
      const std::string id = "Paper" + std::to_string(p.code);
      ui::Command* c = ui::Commands().Find(id);
      if (!c) {
        ui::Command nc;
        nc.id = id;
        const int code = p.code;
        nc.execute = [&s, code](ui::Command&) { EditSetup(s, TR("Size"), [code](model::PageSetup& x) { x.paper = code; }); };
        c = &ui::Commands().Add(std::move(nc));
      }
      c->label = p.name;
      menu.push_back(id);
    }
    ForRibbonItem("PaperSizeMenu", [&](ui::ribbon::Item& it) { it.menu = menu; });
  }
  const model::PageSetup* ps = ActiveSetup(s);
  const model::Sheet* sh = s.FocusSheet();
  // Background <-> Delete Background; Margins' Last Custom Setting; Print Area's Add to Print Area
  const bool has_bg = sh && !sh->background->empty();
  const bool has_last = Config().last_margins[0] >= 0;
  const bool has_area = ps && ps->has_area;
  static int seen = -1;
  const int state = (has_bg ? 1 : 0) | (has_last ? 2 : 0) | (has_area ? 4 : 0);
  if (state != seen) {
    seen = state;
    ForRibbonItem(has_bg ? "SheetBackground" : "DeleteBackground", [&](ui::ribbon::Item& it) { it.cmd = has_bg ? "DeleteBackground" : "SheetBackground"; });
    ForRibbonItem("MarginsMenu", [&](ui::ribbon::Item& it) {
      it.menu = {"MarginsNormal", "MarginsWide", "MarginsNarrow", "-", "MarginsCustom"};
      if (has_last) it.menu.insert(it.menu.begin(), "MarginsLast");
    });
    ForRibbonItem("PrintAreaMenu", [&](ui::ribbon::Item& it) {
      it.menu = {"SetPrintArea", "ClearPrintArea"};
      if (has_area) it.menu.push_back("AddToPrintArea");
    });
  }
  if (has_last) {
    if (auto* c = ui::Commands().Find("MarginsLast")) {
      const auto& m = Config().last_margins;
      const bool metric = MetricUnits();
      const double unit = metric ? 72.0 / 2.54 : 72.0;
      char b[160];
      std::snprintf(b, sizeof b, metric ? "%s  (%s %.2f cm %s %.2f cm)" : "%s  (%s %.2f\" %s %.2f\")", TR("Last Custom Setting"), TR("Top:"), m[2] / unit, TR("Left:"),
                    m[0] / unit);
      c->label = b;
    }
  }
  if (ps) {
    if (auto* c = ui::Commands().Find("ScaleWidthMenu")) c->label = std::string(TR("Width:")) + " " + PagesLabel(FitPages(*ps, true));
    if (auto* c = ui::Commands().Find("ScaleHeightMenu")) c->label = std::string(TR("Height:")) + " " + PagesLabel(FitPages(*ps, false));
    for (int k = 0; k <= 9; ++k) {
      if (auto* c = ui::Commands().Find("ScaleWidth" + std::to_string(k))) c->toggle = true, c->pressed = FitPages(*ps, true) == k;
      if (auto* c = ui::Commands().Find("ScaleHeight" + std::to_string(k))) c->toggle = true, c->pressed = FitPages(*ps, false) == k;
    }
  }
  // the Header & Footer tab: shown (and brought to the front) while a header or footer is typed in Page Layout view
  const bool hf = u.hf_which >= 0;
  auto& st = ui::ribbon::St();
  auto& tabs = ui::ribbon::Tabs();
  int hf_tab = -1;
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].visible == &cx.header_footer) hf_tab = static_cast<int>(i);
  }
  if (hf && !cx.header_footer && hf_tab >= 0) {
    u.hf_prev_tab = st.selected_tab;
    st.selected_tab = hf_tab;
  } else if (!hf && cx.header_footer && hf_tab >= 0 && st.selected_tab == hf_tab) {
    st.selected_tab = u.hf_prev_tab >= 0 ? u.hf_prev_tab : st.selected_tab;
  }
  cx.header_footer = hf;
  if (const model::PageSetup* hps = hf ? HfSetup(s) : ps) {
    cx.hf_first = hps->different_first;
    cx.hf_odd_even = hps->different_odd_even;
    cx.hf_scale = !hps->no_scale_with_doc;
    cx.hf_align = !hps->no_align_margins;
    if (hf) {
      const auto presets = HfPresets();
      for (size_t k = 0; k < presets.size(); ++k) {
        std::string label;
        for (const char* part : {presets[k].l, presets[k].c, presets[k].r}) {
          if (!*part) continue;
          if (!label.empty()) label += ", ";
          label += engine::ExpandHeaderFooter(part, 1, 1, "Book1", "", sh ? sh->name : std::string(), "", "");
        }
        if (label.empty()) label = TR("(none)");
        for (const char* pre : {"HfHeader", "HfFooter"}) {
          if (auto* c = ui::Commands().Find(pre + std::to_string(k))) c->label = label;
        }
      }
      static bool menus = false;
      if (!menus) {
        menus = true;
        for (int which = 0; which < 2; ++which) {
          std::vector<std::string> m;
          for (size_t k = 0; k < presets.size(); ++k) m.push_back(std::string(which == 0 ? "HfHeader" : "HfFooter") + std::to_string(k));
          ForRibbonItem(which == 0 ? "HfHeaderMenu" : "HfFooterMenu", [&](ui::ribbon::Item& it) { it.menu = m; });
        }
      }
    }
  }
  if (auto* c = ui::Commands().Find("HfFormatPicture")) {
    const model::PageSetup* hps = hf ? HfSetup(s) : nullptr;
    c->enabled = hps && !hps->hf_pictures[u.hf_which * 3 + std::clamp(u.hf_box, 0, 2)].image.empty();
  }
}

ImTextureID SheetBackgroundTexture(AppState& s, uint32_t sheet, ImVec2* size) {
  struct Tex {
    std::shared_ptr<const std::string> data;
    ImTextureID tex{};
    ImVec2 size{};
  };
  static std::map<uint32_t, Tex> cache;
  const model::Sheet* sh = s.snap ? s.snap->Sheet(sheet) : nullptr;
  *size = ImVec2(0, 0);
  if (!sh || sh->background->empty()) {
    auto it = cache.find(sheet);
    if (it != cache.end()) {
      if (it->second.tex) ReleaseTexture(it->second.tex);
      cache.erase(it);
    }
    return ImTextureID{};
  }
  Tex& t = cache[sheet];
  const auto shared = sh->background.share();
  if (t.data != shared) {
    if (t.tex) ReleaseTexture(t.tex);
    t = Tex{};
    t.data = shared;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (DecodeImageRgba(*shared, &rgba, &w, &h) && w > 0 && h > 0) {
      t.tex = TextureFromRgba(rgba.data(), w, h);
      t.size = ImVec2(static_cast<float>(w), static_cast<float>(h));
    }
  }
  *size = t.size;
  return t.tex;
}

io::MV LayoutRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err) {
  using io::MV;
  const uint32_t sheet = p.Has("sheet") && s.snap->Sheet(static_cast<uint32_t>(p.GetInt("sheet"))) ? static_cast<uint32_t>(p.GetInt("sheet")) : s.focus_sheet;
  if (m == "layout.background") {
    // Worksheet.SetBackgroundPicture(Filename): "" deletes it
    if (!p.Has("path")) {
      const model::Sheet* sh = s.snap->Sheet(sheet);
      return MV::Bool(sh && !sh->background->empty());
    }
    std::string bytes;
    if (const std::string path = p.GetStr("path"); !path.empty()) {
      std::ifstream f(FromUtf8(path), std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      int w = 0, h = 0;
      if (bytes.empty() || !ImageSize(bytes, &w, &h)) {
        *err = "The picture can't be read: " + path;
        return MV();
      }
    }
    s.engine->PostUser(bytes.empty() ? TR("Delete Background") : TR("Background"), [sheet, bytes](Document& d) { d.SetSheetBackground(sheet, bytes); });
    s.engine->Sync();
    return MV::Bool(true);
  }
  if (m == "layout.window") {
    if (p.Has("ruler")) PV().ruler = p.GetBool("ruler");
    if (p.Has("with")) SetSideBySideWith(p.GetStr("with"));
    if (p.Has("side_by_side") && p.GetBool("side_by_side") != s.side_by_side) ui::Commands().Execute("ViewSideBySide");
    if (p.Has("sync_scrolling")) s.sync_scrolling = p.GetBool("sync_scrolling");
    if (p.Has("visible")) {
      // Window.Visible of the window (0 = the workbook's first one, else an extra window's id)
      const uint32_t win = static_cast<uint32_t>(p.GetInt("window"));
      if (win == 0) s.main_hidden = !p.GetBool("visible");
      for (auto& w : s.extra_wins) {
        if (w.id == win) w.hidden = !p.GetBool("visible");
      }
    }
    if (p.Has("arrange")) {
      s.arrange_mode = static_cast<int>(std::clamp<int64_t>(p.GetInt("arrange"), 0, 3));
      s.arrange_windows = true;
      // Windows.Arrange(ActiveWorkbook:=False), as the dialog without "Windows of active workbook": the other YCGrids too
      if (!p.GetBool("active_workbook")) ArrangeOsWindows(s, s.arrange_mode);
    }
    if (p.GetBool("reset_positions")) ui::Commands().Execute("ResetWindowPosition");
    MV o = MV::Map();
    o.Set("ruler", MV::Bool(PV().ruler));
    o.Set("side_by_side", MV::Bool(s.side_by_side));
    o.Set("sync_scrolling", MV::Bool(s.sync_scrolling));
    o.Set("main_visible", MV::Bool(!s.main_hidden));
    o.Set("partner", MV::Int(static_cast<int64_t>(SideBySidePartner())));
    MV wins = MV::Array();
    for (const auto& w : s.extra_wins) {
      MV x = MV::Map();
      x.Set("id", MV::Int(w.id));
      x.Set("visible", MV::Bool(!w.hidden));
      x.Set("sheet", MV::Int(w.view.sheet));
      wins.Push(std::move(x));
    }
    o.Set("windows", std::move(wins));
    return o;
  }
  if (m == "layout.custom_views") {
    const std::string op = p.GetStr("op"), name = p.GetStr("name");
    const json all = Views(s);
    if (op == "add") {
      AddView(s, name, p.GetBool("print", true), p.GetBool("hidden", true));
      s.engine->Sync();
      return MV::Bool(true);
    }
    for (const json& v : all) {
      if (v.value("name", "") != name) continue;
      if (op == "show") ShowView(s, v);
      if (op == "delete") DeleteView(s, name);
      if (op == "get") {
        MV o = MV::Map();
        o.Set("name", MV::Str(name));
        o.Set("print", MV::Bool(v.value("print", false)));
        o.Set("hidden", MV::Bool(v.value("hidden", false)));
        return o;
      }
      s.engine->Sync();
      return MV::Bool(true);
    }
    if (op == "list") {
      MV o = MV::Array();
      for (const json& v : all) o.Push(MV::Str(v.value("name", "")));
      return o;
    }
    *err = "No custom view named " + name;
    return MV();
  }
  if (m == "layout.docprops") {
    // Workbook.BuiltinDocumentProperties: Title, Subject, Author, Keywords, Comments, Category, Manager, Company...
    json j = DocPropsJson(s.snap->wb);
    if (p.Has("name") && p.Has("value")) {
      j[p.GetStr("name")] = p.GetStr("value");
      PostStoreJson(s, TR("Properties"), "view.docprops", j);
      s.engine->Sync();
      return MV::Bool(true);
    }
    MV o = MV::Map();
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it->is_string()) o.Set(it.key(), MV::Str(it->get<std::string>()));
      else if (it->is_number()) o.Set(it.key(), MV::Double(it->get<double>()));
    }
    return o;
  }
  if (m == "layout.side_by_side") {  // from the other YCGrid (View Side by Side started / ended there)
    PartnerSideBySide(s, static_cast<unsigned long>(p.GetInt("pid")), p.GetBool("on"));
    return MV::Bool(true);
  }
  if (m == "layout.sync_scroll") {  // the other YCGrid side by side scrolled
    PartnerScrolled(s, p.GetInt("dv"), p.GetInt("dh"));
    return MV::Bool(true);
  }
  if (m == "layout.shape_style") {
    ApplyShapeQuickStyle(s, sheet, static_cast<uint32_t>(p.GetInt("id")), static_cast<int>(p.GetInt("style")));
    s.engine->Sync();
    return MV::Bool(true);
  }
  if (m == "layout.theme_effects") {
    // Workbook.Theme.ThemeEffectScheme: its name; Load(name) applies a built-in scheme
    if (p.Has("name") && !SetThemeEffects(s, p.GetStr("name"))) {
      *err = "Unknown theme effects: " + p.GetStr("name");
      return MV();
    }
    if (p.Has("name")) s.engine->Sync();
    MV o = MV::Map();
    o.Set("name", MV::Str(ThemeEffectsName(s)));
    MV all = MV::Array();
    for (const auto& e : model::BuiltinThemeEffects()) all.Push(MV::Str(e.name));
    o.Set("builtin", std::move(all));
    return o;
  }
  if (m == "layout.backstage") {
    // tests: File's pages as a person opens them (0 Info, 1 New, 2 Open, 3 Export, 4 Options, 5 Print)
    ui::ribbon::St().backstage = true;
    ui::ribbon::St().backstage_page = static_cast<int>(p.GetInt("page"));
    return MV::Bool(true);
  }
  if (m == "layout.print") {
    // File > Print's choices and its preview (tests; Python prints with PrintOut)
    PrintUi& u = U();
    int total = 0;
    for (const auto& j : CurrentJobs(s)) total += static_cast<int>(j.layout.pages.size());
    MV o = MV::Map();
    o.Set("what", MV::Int(u.what));
    o.Set("chart", MV::Int(u.chart));
    o.Set("page", MV::Int(u.page));
    o.Set("pages", MV::Int(total));
    o.Set("duplex", MV::Int(u.duplex));
    o.Set("collate", MV::Bool(u.collate));
    o.Set("show_margins", MV::Bool(u.show_margins));
    return o;
  }
  if (m == "layout.page_view") {
    // tests and Python: what Page Layout view shows (pages, the header being typed)
    MV o = MV::Map();
    o.Set("hf_which", MV::Int(PV().hf_which));
    o.Set("hf_page", MV::Int(PV().hf_page));
    o.Set("ruler", MV::Bool(PV().ruler));
    return o;
  }
  *err = "Unknown request " + m;
  return MV();
}

void RegisterPrintCommands(AppState& s) {
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
  R.Add(make("FilePrint", TR("Print"), ui::icon::Print, "Ctrl+P", TR("Preview the pages and print them."))).execute = [&s](ui::Command&) {
    U().page = 0;
    // a selected chart: Print Selected Chart (Excel)
    U().chart = -1;
    if (HasSelectedChart() && s.snap) {
      const auto ids = SelectedObjectIds();
      if (const model::Sheet* sh = s.FocusSheet(); sh && !ids.empty() && ids[0].first == 2) {
        for (size_t i = 0; i < sh->charts->size(); ++i) {
          if ((*sh->charts)[i].id == ids[0].second) U().chart = static_cast<int>(i);
        }
      }
    }
    if (U().chart >= 0) U().what = 0;
    ui::ribbon::St().backstage = true;
    ui::ribbon::St().backstage_page = 5;
    ShowBreaks(s, s.focus_sheet);
  };
  R.Add(make("PrintPreview", TR("Print Preview"), ui::icon::Print)).execute = [](ui::Command&) { ui::Commands().Execute("FilePrint"); };
  auto view_mode = [&s](int mode) {
    return [&s, mode](ui::Command&) {
      if (s.edit.active) CommitEdit(s, false, 0);
      s.FocusView().page_view = mode;
    };
  };
  R.Add(make("ViewNormal", TR("Normal"), ui::icon::ViewNormal, "", TR("The normal view of the sheet."))).execute = view_mode(0);
  R.Add(make("ViewPageBreakPreview", TR("Page Break Preview"), ui::icon::ViewPageBreak, "",
             TR("See where the pages break when printed; drag the blue lines to move page breaks or to change the print area."))).execute = view_mode(1);
  R.Add(make("ViewPageLayout", TR("Page Layout"), ui::icon::ViewPageLayout, "", TR("See the sheet as printed pages, with headers and footers; click a cell to select it."))).execute =
      view_mode(2);
  // the button of the current view shows pressed (Excel); UpdateCommandStates sets them every frame
  for (const char* id : {"ViewNormal", "ViewPageBreakPreview", "ViewPageLayout"}) R.Find(id)->toggle = true;
  R.Add(make("PrintNow", TR("Print"), ui::icon::Print)).execute = [&s](ui::Command&) {
    PrintUi& u = U();
    if (u.printers.empty()) RefreshPrinters();
    if (u.printer < 0 || u.printer >= static_cast<int>(u.printers.size())) {
      s.ShowMessage("YCGrid", TR("No printer is installed."));
      return;
    }
    const std::wstring printer = u.printers[static_cast<size_t>(u.printer)];
    std::wstring out;
    if (printer == kPdfPrinter) {
      out = PickPdf(s.hwnd, std::filesystem::path(s.file_path.empty() ? L"Book1" : s.file_path).stem().wstring() + L".pdf");
      if (out.empty()) return;
    }
    PrintRequest rq;
    rq.printer = printer;
    rq.output = out;
    rq.from = u.from;
    rq.to = u.to;
    rq.copies = u.copies;
    rq.collate = u.collate;
    rq.duplex = u.duplex;
    if (u.devmode_printer == printer) rq.devmode = u.devmode;
    std::string err;
    if (!PrintJobs(*s.snap, CurrentJobs(s), rq, MakePrintInfo(s.file_path), &err)) s.ShowError("YCGrid", err);
    else ui::ribbon::St().backstage = false;
  };
  R.Add(make("ExportPdf", TR("Export\nPDF"), ui::icon::Pdf, "", TR("Save the active sheet as a PDF file."))).execute = [&s](ui::Command&) {
    const std::wstring path = PickPdf(s.hwnd, std::filesystem::path(s.file_path.empty() ? L"Book1" : s.file_path).stem().wstring() + L".pdf");
    if (path.empty()) return;
    std::string err;
    if (!ExportPdf(s, path, {s.focus_sheet}, false, 1, 0, &err)) s.ShowError("YCGrid", err);
    else s.SetStatus(TR("PDF saved: ") + ToUtf8(path));
  };
  // ---- Page Layout -> Page Setup ----
  R.Add(make("MarginsMenu", TR("Margins"), ui::icon::Margins, "", TR("Set the page margins."))).execute = [](ui::Command&) {};
  struct M {
    const char* id;
    const char* label;
    int kind;
  };
  static const M kMargins[] = {{"MarginsNormal", TR("Normal Margins"), 0}, {"MarginsWide", TR("Wide Margins"), 1}, {"MarginsNarrow", TR("Narrow Margins"), 2}};
  for (const M& m : kMargins) {
    const int kind = m.kind;
    R.Add(make(m.id, m.label)).execute = [&s, kind](ui::Command&) { EditSetup(s, TR("Margins"), [kind](model::PageSetup& p) { SetMargins(p, kind); }); };
  }
  R.Add(make("MarginsCustom", TR("Custom Margins..."))).execute = [&s](ui::Command&) { OpenPageSetup(s, 1); };
  R.Add(make("OrientationMenu", TR("Orientation"), ui::icon::Orientation, "", TR("Portrait or landscape pages."))).execute = [](ui::Command&) {};
  R.Add(make("OrientPortrait", TR("Portrait"))).execute = [&s](ui::Command&) { EditSetup(s, TR("Orientation"), [](model::PageSetup& p) { p.landscape = false; }); };
  R.Add(make("OrientLandscape", TR("Landscape"))).execute = [&s](ui::Command&) { EditSetup(s, TR("Orientation"), [](model::PageSetup& p) { p.landscape = true; }); };
  R.Add(make("PaperSizeMenu", TR("Size"), ui::icon::PaperSize, "", TR("Choose a paper size."))).execute = [](ui::Command&) {};
  for (const Paper& pp : kPapers) {
    const int code = pp.code;
    R.Add(make((std::string("Paper") + std::to_string(code)).c_str(), pp.name)).execute = [&s, code](ui::Command&) {
      EditSetup(s, TR("Size"), [code](model::PageSetup& p) { p.paper = code; });
    };
  }
  R.Add(make("PrintAreaMenu", TR("Print\nArea"), ui::icon::PrintArea, "", TR("Mark the selection as the area to print."))).execute = [](ui::Command&) {};
  R.Add(make("SetPrintArea", TR("Set Print Area"))).execute = [&s](ui::Command&) {
    // every range of the selection (Ctrl+click several): each prints on its own pages (Excel)
    std::vector<Rect> areas = s.FocusView().sel.areas;
    if (areas.empty()) areas.push_back(SelRect(s));
    EditSetup(s, TR("Set Print Area"), [areas](model::PageSetup& p) {
      p.has_area = true;
      p.area = areas[0];
      p.more_areas.assign(areas.begin() + 1, areas.end());
    });
  };
  R.Add(make("AddToPrintArea", TR("Add to Print Area"))).execute = [&s](ui::Command&) {
    // Excel: the selection joins the print area (as another range printed on its own pages); without one it sets it
    std::vector<Rect> areas = s.FocusView().sel.areas;
    if (areas.empty()) areas.push_back(SelRect(s));
    EditSetup(s, TR("Add to Print Area"), [areas](model::PageSetup& p) {
      size_t k = 0;
      if (!p.has_area) {
        p.has_area = true;
        p.area = areas[0];
        p.more_areas.clear();
        k = 1;
      }
      for (; k < areas.size(); ++k) {
        if (areas[k] == p.area || std::find(p.more_areas.begin(), p.more_areas.end(), areas[k]) != p.more_areas.end()) continue;
        p.more_areas.push_back(areas[k]);
      }
    });
  };
  R.Add(make("ClearPrintArea", TR("Clear Print Area"))).execute = [&s](ui::Command&) {
    EditSetup(s, TR("Clear Print Area"), [](model::PageSetup& p) {
      p.has_area = false;
      p.more_areas.clear();
    });
  };
  // ---- Page Layout > Background (a picture tiled behind the cells, on screen only) ----
  R.Add(make("SheetBackground", TR("Background"), ui::icon::Background, "", TR("Choose a picture to show behind the cells of the sheet."))).execute =
      [&s](ui::Command&) {
        std::string bytes;
        if (!PickPictureFile(s, &bytes)) return;
        const uint32_t sheet = s.focus_sheet;
        s.engine->PostUser(TR("Background"), [sheet, bytes](Document& d) { d.SetSheetBackground(sheet, bytes); });
      };
  R.Add(make("DeleteBackground", TR("Delete Background"), ui::icon::Picture, "", TR("Remove the sheet's background picture."))).execute = [&s](ui::Command&) {
    const uint32_t sheet = s.focus_sheet;
    s.engine->PostUser(TR("Delete Background"), [sheet](Document& d) { d.SetSheetBackground(sheet, std::string()); });
  };
  // ---- Margins > Last Custom Setting ----
  R.Add(make("MarginsLast", TR("Last Custom Setting"))).execute = [&s](ui::Command&) {
    if (Config().last_margins[0] < 0) return;
    EditSetup(s, TR("Margins"), [](model::PageSetup& p) { SetMargins(p, 3); });
  };
  // ---- File > Print > Printer Properties: the printer driver's own dialog ----
  R.Add(make("PrinterProperties", TR("Printer Properties"))).execute = [&s](ui::Command&) {
    PrintUi& u = U();
    if (u.printers.empty()) RefreshPrinters();
    if (u.printer < 0 || u.printer >= static_cast<int>(u.printers.size())) {
      s.ShowMessage("YCGrid", TR("No printer is installed."));
      return;
    }
    if (!s.hwnd || !::IsWindowVisible(s.hwnd)) {  // a hidden instance: nobody could answer the driver's dialog
      s.SetStatus(TR("Printer Properties needs a visible window."));
      return;
    }
    const std::wstring name = u.printers[static_cast<size_t>(u.printer)];
    HANDLE hp = nullptr;
    if (!::OpenPrinterW(const_cast<wchar_t*>(name.c_str()), &hp, nullptr)) return;
    const LONG size = ::DocumentPropertiesW(s.hwnd, hp, const_cast<wchar_t*>(name.c_str()), nullptr, nullptr, 0);
    if (size > 0) {
      std::vector<BYTE> out(static_cast<size_t>(size));
      DEVMODEW* in = u.devmode_printer == name && u.devmode.size() >= sizeof(DEVMODEW) ? reinterpret_cast<DEVMODEW*>(u.devmode.data()) : nullptr;
      const LONG r = ::DocumentPropertiesW(s.hwnd, hp, const_cast<wchar_t*>(name.c_str()), reinterpret_cast<DEVMODEW*>(out.data()), in,
                                           DM_IN_PROMPT | DM_OUT_BUFFER | (in ? DM_IN_BUFFER : 0));
      if (r == IDOK) {
        u.devmode = std::move(out);
        u.devmode_printer = name;
        const auto* dm = reinterpret_cast<const DEVMODEW*>(u.devmode.data());
        if (dm->dmFields & DM_DUPLEX) u.duplex = dm->dmDuplex == DMDUP_VERTICAL ? 1 : dm->dmDuplex == DMDUP_HORIZONTAL ? 2 : 0;
        if (dm->dmFields & DM_COLLATE) u.collate = dm->dmCollate == DMCOLLATE_TRUE;
      }
    }
    ::ClosePrinter(hp);
  };
  R.Add(make("BreaksMenu", TR("Breaks"), ui::icon::PageBreak, "", TR("Start a new page at the active cell."))).execute = [](ui::Command&) {};
  R.Add(make("InsertPageBreak", TR("Insert Page Break"))).execute = [&s](ui::Command&) {
    const auto& sel = s.FocusView().sel;
    const Rect r = SelRect(s);
    const model::Sheet* sh = s.FocusSheet();
    const bool whole_rows = sh && r.c0 == 0 && r.c1 + 1 >= sh->col_limit, whole_cols = sh && r.r0 == 0 && r.r1 + 1 >= sh->row_limit;
    const uint32_t row = whole_cols ? 0 : (whole_rows ? r.r0 : sel.act_r), col = whole_rows ? 0 : (whole_cols ? r.c0 : sel.act_c);
    EditSetup(s, TR("Insert Page Break"), [row, col](model::PageSetup& p) {
      if (row > 0 && std::find(p.row_breaks.begin(), p.row_breaks.end(), row) == p.row_breaks.end()) p.row_breaks.push_back(row);
      if (col > 0 && std::find(p.col_breaks.begin(), p.col_breaks.end(), col) == p.col_breaks.end()) p.col_breaks.push_back(col);
    });
  };
  R.Add(make("RemovePageBreak", TR("Remove Page Break"))).execute = [&s](ui::Command&) {
    const auto& sel = s.FocusView().sel;
    const uint32_t row = sel.act_r, col = sel.act_c;
    EditSetup(s, TR("Remove Page Break"), [row, col](model::PageSetup& p) {
      p.row_breaks.erase(std::remove(p.row_breaks.begin(), p.row_breaks.end(), row), p.row_breaks.end());
      p.col_breaks.erase(std::remove(p.col_breaks.begin(), p.col_breaks.end(), col), p.col_breaks.end());
    });
  };
  R.Add(make("ResetPageBreaks", TR("Reset All Page Breaks"))).execute = [&s](ui::Command&) {
    EditSetup(s, TR("Reset All Page Breaks"), [](model::PageSetup& p) {
      p.row_breaks.clear();
      p.col_breaks.clear();
    });
  };
  R.Add(make("PrintTitles", TR("Print\nTitles"), ui::icon::PrintTitles, "", TR("Rows and columns to repeat on every printed page."))).execute = [&s](ui::Command&) {
    OpenPageSetup(s, 3);
  };
  R.Add(make("PageSetupDialog", TR("Page Setup"), ui::icon::Options)).execute = [&s](ui::Command&) { OpenPageSetup(s, 0); };
  R.Add(make("PageSetupSheet", TR("Sheet Options"), ui::icon::DisplayGroup)).execute = [&s](ui::Command&) { OpenPageSetup(s, 3); };
  // ---- Scale to Fit ----
  R.Add(make("ScaleMenu", TR("Scale to\nFit"), ui::icon::PaperSize, "", TR("Shrink the printout to fit on fewer pages."))).execute = [](ui::Command&) {};
  struct Sc {
    const char* id;
    const char* label;
    int kind, zoom;
  };
  static const Sc kScales[] = {{"ScaleNone", TR("No Scaling"), 0, 100},       {"ScaleFitSheet", TR("Fit Sheet on One Page"), 1, 0},
                               {"ScaleFitColumns", TR("Fit All Columns on One Page"), 2, 0}, {"ScaleFitRows", TR("Fit All Rows on One Page"), 3, 0},
                               {"Scale90", "90%", -1, 90},  {"Scale75", "75%", -1, 75},  {"Scale50", "50%", -1, 50}};
  for (const Sc& sc : kScales) {
    const int kind = sc.kind, zoom = sc.zoom;
    R.Add(make(sc.id, sc.label)).execute = [&s, kind, zoom](ui::Command&) {
      EditSetup(s, TR("Scale to Fit"), [kind, zoom](model::PageSetup& p) {
        if (kind >= 0) SetScale(p, kind);
        else p.zoom = zoom;
      });
    };
  }
  R.Add(make("PrintGridlines", TR("Print Gridlines"))).execute = [&s](ui::Command&) {
    EditSetup(s, TR("Print Gridlines"), [](model::PageSetup& p) { p.gridlines = !p.gridlines; });
  };
  R.Add(make("PrintHeadings", TR("Print Headings"))).execute = [&s](ui::Command&) {
    EditSetup(s, TR("Print Headings"), [](model::PageSetup& p) { p.headings = !p.headings; });
  };
  RegisterLayoutCommands(s);
  RegisterShapeEffectCommands(s);
}

namespace {

// Page Setup > Margins as in Excel: Top / Header above the page picture, Left and Right beside it, Bottom / Footer below;
// in the user's units (cm or inches, from Windows); the margin being typed is drawn on the picture
void DrawMarginsTab(PrintUi& u, model::PageSetup& ps) {
  const bool metric = MetricUnits();
  const double unit = metric ? 72.0 / 2.54 : 72.0;  // points per cm / inch
  const char* fmt = metric ? "%.1f" : "%.2f";
  const ImVec2 o = ImGui::GetCursorPos();
  const float bw = S(90), pic_w = S(150);
  double paper_w = 0, paper_h = 0;
  model::PaperSize(ps.paper, &paper_w, &paper_h);
  if (ps.landscape) std::swap(paper_w, paper_h);
  const float pic_h = std::min(S(200), pic_w * static_cast<float>(paper_h / paper_w)), pw = pic_h * static_cast<float>(paper_w / paper_h);
  const float lh = ImGui::GetTextLineHeightWithSpacing(), fh = ImGui::GetFrameHeight();
  const float col_left = o.x, col_pic = o.x + bw + S(30), col_right = col_pic + pic_w + S(30);
  const float row_top = o.y, row_pic = row_top + lh + fh + S(10), row_bottom = row_pic + std::max(pic_h, S(150)) + S(10);
  struct Box {
    const char* label;
    const char* id;
    double* v;
    float x, y;
  };
  const Box boxes[] = {{TR("Left:"), "Left", &ps.left, col_left, row_pic + pic_h * 0.4f},        {TR("Right:"), "Right", &ps.right, col_right, row_pic + pic_h * 0.4f},
                       {TR("Top:"), "Top", &ps.top, col_pic + (pic_w - bw) / 2, row_top},            {TR("Bottom:"), "Bottom", &ps.bottom, col_pic + (pic_w - bw) / 2, row_bottom},
                       {TR("Header:"), "Header", &ps.header, col_right, row_top},                     {TR("Footer:"), "Footer", &ps.footer, col_right, row_bottom}};
  int focus = -1;
  for (int k = 0; k < 6; ++k) {
    const Box& b = boxes[k];
    ImGui::SetCursorPos(ImVec2(b.x, b.y));
    ImGui::TextUnformatted(b.label);
    ImGui::SetCursorPos(ImVec2(b.x, b.y + lh));
    double v = *b.v / unit;
    ImGui::PushID(k);
    ImGui::SetNextItemWidth(bw);
    ui::NoteUiItem(b.id, ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(bw, fh)));
    if (ImGui::InputDouble("##m", &v, metric ? 0.1 : 0.05, metric ? 0.5 : 0.25, fmt)) {
      *b.v = std::clamp(v, 0.0, metric ? 25.0 : 10.0) * unit;
      u.fit_error = false;
    }
    if (ImGui::IsItemActive() || ImGui::IsItemFocused()) focus = k;
    ImGui::PopID();
  }
  if (focus >= 0) u.margin_focus = focus;
  // the page picture
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos() - ImVec2(ImGui::GetScrollX(), ImGui::GetScrollY());
  const ImVec2 p0 = wp + ImVec2(col_pic + (pic_w - pw) / 2, row_pic), p1 = p0 + ImVec2(pw, pic_h);
  dl->AddRectFilled(p0 + ImVec2(S(3), S(3)), p1 + ImVec2(S(3), S(3)), IM_COL32(0, 0, 0, 50));
  dl->AddRectFilled(p0, p1, IM_COL32_WHITE);
  dl->AddRect(p0, p1, IM_COL32(0x80, 0x80, 0x80, 255));
  const float sx = pw / static_cast<float>(paper_w), sy = pic_h / static_cast<float>(paper_h);
  const float l = p0.x + static_cast<float>(ps.left) * sx, r = p1.x - static_cast<float>(ps.right) * sx;
  const float t = p0.y + static_cast<float>(ps.top) * sy, b = p1.y - static_cast<float>(ps.bottom) * sy;
  // the printed area: a few rows of "cells"
  if (r > l && b > t) {
    for (float y = t; y + S(4) < b; y += S(6)) dl->AddLine(ImVec2(l, y), ImVec2(r, y), IM_COL32(0xC0, 0xC0, 0xC0, 255));
    dl->AddRect(ImVec2(l, t), ImVec2(r, b), IM_COL32(0xA0, 0xA0, 0xA0, 255));
  }
  const int f = u.margin_focus;
  if (f >= 0) {
    const ImU32 hl = IM_COL32(0x21, 0x73, 0x46, 255);
    if (f == 0) dl->AddLine(ImVec2(l, p0.y), ImVec2(l, p1.y), hl, S(2));
    if (f == 1) dl->AddLine(ImVec2(r, p0.y), ImVec2(r, p1.y), hl, S(2));
    if (f == 2) dl->AddLine(ImVec2(p0.x, t), ImVec2(p1.x, t), hl, S(2));
    if (f == 3) dl->AddLine(ImVec2(p0.x, b), ImVec2(p1.x, b), hl, S(2));
    if (f == 4) dl->AddLine(ImVec2(p0.x, p0.y + static_cast<float>(ps.header) * sy), ImVec2(p1.x, p0.y + static_cast<float>(ps.header) * sy), hl, S(2));
    if (f == 5) dl->AddLine(ImVec2(p0.x, p1.y - static_cast<float>(ps.footer) * sy), ImVec2(p1.x, p1.y - static_cast<float>(ps.footer) * sy), hl, S(2));
  }
  ImGui::SetCursorPos(ImVec2(o.x, row_bottom + lh + fh + S(6)));
  ImGui::TextDisabled("%s", metric ? TR("Margins in centimeters (Windows' measurement system).") : TR("Margins in inches (Windows' measurement system)."));
  if (u.fit_error) ImGui::TextColored(ImVec4(0.8f, 0.1f, 0.1f, 1), "%s", TR("Margins do not fit page size."));
}

}  // namespace

void DrawPageSetupDialog(AppState& s) {
  DrawCustomViewsDialog(s);
  DrawDocPropertiesDialog(s);
  DrawHfPictureFromRibbon(s);
  DrawWindowDialogs(s);
  PrintUi& u = U();
  if (!BeginDlg(s, Dialog::PageSetup, TR("Page Setup###yc_dlg_pagesetup"), 520)) return;
  model::PageSetup& ps = u.edit;
  const float w = S(260);
  bool close = false;
  if (BeginDlgTabBar("##pstabs")) {
    const char* tabs[] = {TR("Page"), TR("Margins"), TR("Header/Footer"), TR("Sheet")};
    for (int i = 0; i < 4; ++i) {
      if (!ImGui::BeginTabItem(tabs[i], nullptr, u.tab == i ? ImGuiTabItemFlags_SetSelected : 0)) continue;
      if (u.tab == i) u.tab = -1;
      ImGui::Dummy(ImVec2(S(480), S(4)));
      if (i == 0) {
        ImGui::SeparatorText(TR("Orientation"));
        OrientationCombo(ps, w);
        ImGui::SeparatorText(TR("Scaling"));
        bool fit = ps.zoom == 0;
        if (ImGui::RadioButton(TR("Adjust to:"), !fit)) {
          if (fit) ps.zoom = 100;
          fit = false;
        }
        ImGui::SameLine();
        int zoom = fit ? 100 : ps.zoom;
        ImGui::SetNextItemWidth(S(100));
        ImGui::BeginDisabled(fit);
        if (ImGui::InputInt(TR("% normal size"), &zoom, 5, 10)) ps.zoom = std::clamp(zoom, 10, 400);
        ImGui::EndDisabled();
        if (ImGui::RadioButton(TR("Fit to:"), fit)) ps.zoom = 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(!fit);
        ImGui::SetNextItemWidth(S(80));
        if (ImGui::InputInt(TR("page(s) wide by"), &ps.fit_wide)) ps.fit_wide = std::max(0, ps.fit_wide);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(80));
        if (ImGui::InputInt(TR("tall"), &ps.fit_tall)) ps.fit_tall = std::max(0, ps.fit_tall);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", TR("0 = automatic (as many pages as needed)."));
        ImGui::SeparatorText(TR("Paper size"));
        PaperCombo(ps, w);
        ImGui::Checkbox(TR("First page number: Auto"), &u.first_auto);
        if (!u.first_auto) {
          ImGui::SameLine();
          ImGui::SetNextItemWidth(S(100));
          if (ps.first_page_number <= 0) ps.first_page_number = 1;
          ImGui::InputInt("##first", &ps.first_page_number);
        }
      } else if (i == 1) {
        DrawMarginsTab(u, ps);
        ImGui::SeparatorText(TR("Center on page"));
        ImGui::Checkbox(TR("Horizontally"), &ps.center_h);
        ImGui::SameLine();
        ImGui::Checkbox(TR("Vertically"), &ps.center_v);
      } else if (i == 2) {
        DrawHeaderFooterTab(s, u);
      } else {
        // select them on the sheet with each box's button (the dialog shrinks while picking), as in Excel
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Print area:"));
        ImGui::SameLine(S(200));
        RangeBox(s, "print.area", u.area, sizeof(u.area), S(260), 0, TR("Print area:"));
        ImGui::SeparatorText(TR("Print titles"));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Rows to repeat at top:"));
        ImGui::SameLine(S(200));
        RangeBox(s, "print.rows", u.title_rows, sizeof(u.title_rows), S(260), 0, TR("Rows to repeat at top:"));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("Columns to repeat at left:"));
        ImGui::SameLine(S(200));
        RangeBox(s, "print.cols", u.title_cols, sizeof(u.title_cols), S(260), 0, TR("Columns to repeat at left:"));
        ImGui::SeparatorText(TR("Print"));
        // Excel's two columns: Gridlines / Black and white / Draft quality / Row and column headings, then Comments
        // and Cell errors as
        ImGui::BeginGroup();
        ImGui::Checkbox(TR("Gridlines"), &ps.gridlines);
        ImGui::Checkbox(TR("Black and white"), &ps.black_and_white);
        ImGui::Checkbox(TR("Draft quality"), &ps.draft);
        ImGui::Checkbox(TR("Row and column headings"), &ps.headings);
        ImGui::EndGroup();
        ImGui::SameLine(S(250));
        ImGui::BeginGroup();
        {
          const char* notes[] = {TR("(None)"), TR("At end of sheet"), TR("As displayed on sheet")};
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Comments and notes:"));
          ImGui::SameLine(S(130));
          ui::NoteUiItem("Comments and notes", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(150), ImGui::GetFrameHeight())));
          ImGui::SetNextItemWidth(S(150));
          ui::Combo("##comments", &ps.print_comments, notes, 3);
          const char* errs[] = {TR("displayed"), TR("<blank>"), "--", "#N/A"};
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(TR("Cell errors as:"));
          ImGui::SameLine(S(130));
          ui::NoteUiItem("Cell errors as", ImRect(ImGui::GetCursorScreenPos(), ImGui::GetCursorScreenPos() + ImVec2(S(150), ImGui::GetFrameHeight())));
          ImGui::SetNextItemWidth(S(150));
          ui::Combo("##errors", &ps.print_errors, errs, 4);
        }
        ImGui::EndGroup();
        ImGui::SeparatorText(TR("Page order"));
        int order = ps.over_then_down ? 1 : 0;
        ImGui::RadioButton(TR("Down, then over"), &order, 0);
        ImGui::SameLine();
        ImGui::RadioButton(TR("Over, then down"), &order, 1);
        ps.over_then_down = order == 1;
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::Separator();
  if (ImGui::Button(TR("Print Preview"), ImVec2(S(110), 0))) {
    close = true;
  }
  const bool preview = close;
  ImGui::SameLine();
  if (DlgOkButton(TR("OK"), ImVec2(S(80), 0)) || preview) {
    // read the text fields
    model::PageSetup out = ps;
    bool ok = true;
    std::string a = u.area;
    a.erase(std::remove(a.begin(), a.end(), ' '), a.end());
    out.more_areas.clear();
    if (a.empty()) {
      out.has_area = false;
    } else {
      // several ranges separated by commas: each prints on its own pages
      std::vector<Rect> all;
      size_t at = 0;
      while (ok) {
        const size_t comma = a.find(',', at);
        uint32_t sid = 0;
        Rect r;
        ok = ResolveRangeText(s.snap->wb, u.edit_sheet, a.substr(at, comma == std::string::npos ? std::string::npos : comma - at), &sid, &r) && sid == u.edit_sheet;
        if (ok) all.push_back(r);
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
      out.has_area = ok && !all.empty();
      if (out.has_area) {
        out.area = all[0];
        out.more_areas.assign(all.begin() + 1, all.end());
      }
    }
    ok = ok && ParseSpan(s, u.edit_sheet, u.title_rows, true, &out.has_title_rows, &out.title_r0, &out.title_r1);
    ok = ok && ParseSpan(s, u.edit_sheet, u.title_cols, false, &out.has_title_cols, &out.title_c0, &out.title_c1);
    std::string* texts[] = {&out.header_left, &out.header_center, &out.header_right, &out.footer_left, &out.footer_center, &out.footer_right};
    for (int k = 0; k < 6; ++k) *texts[k] = u.hf[k];
    for (int k = 0; k < 6; ++k) out.first_hf[k] = u.hf_first[k], out.even_hf[k] = u.hf_even[k];
    if (u.first_auto) out.first_page_number = 0;
    // Excel's check of the Margins tab: the margins must leave room on the paper
    double paper_w = 0, paper_h = 0;
    model::PaperSize(out.paper, &paper_w, &paper_h);
    if (out.landscape) std::swap(paper_w, paper_h);
    const bool fits = out.left + out.right < paper_w - 18 && out.top + out.bottom < paper_h - 18;
    if (!ok) {
      s.ShowError("YCGrid", TR("The reference isn't valid."));
      close = false;
    } else if (!fits) {
      u.fit_error = true;  // shown on the Margins tab; the dialog stays open, as after Excel's message
      u.tab = 1;
      close = false;
    } else {
      RememberCustomMargins(out);
      const uint32_t sheet = u.edit_sheet;
      s.engine->PostUser(TR("Page Setup"), [sheet, out](Document& d) { d.UpdatePageSetup(sheet, [&out](model::PageSetup& p) { p = out; }); });
      ShowBreaks(s, sheet);
      close = true;
      if (preview) ui::Commands().Execute("FilePrint");
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(TR("Cancel"), ImVec2(S(80), 0))) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    s.dialog = Dialog::None;
  }
  ImGui::EndPopup();
}

// Insert > Header & Footer (illustrations_ui.cpp): Page Layout view with the caret in the first page's center header,
// as Excel
void StartHeaderFooterEditing(AppState& s) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return;
  if (s.FocusView().page_view != 2) ui::Commands().Execute("ViewPageLayout");
  StartPageHeaderFooter(*sh->page_setup, sh->id, 0, 0, 1);
}

}  // namespace yc::app
