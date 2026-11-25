// SPDX-License-Identifier: Apache-2.0
// screenshot_ui.cpp - Home > Screenshot: share a picture of the selection, a table, a sheet, the whole workbook, the
// window or one of its floating windows. Cells are drawn by the print renderer (any size, no selection marks);
// windows are copied from the screen. The picture goes to the Clipboard and a preview offers Save As / Copy as File.
#include "screenshot.h"

#include "app_state.h"
#include "core/i18n.h"
#include "print.h"
#include "python_bridge.h"
#include "settings.h"

#include "commands/command_registry.h"
#include "engine/navigate.h"
#include "engine/page_layout.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "yc_ui_items.h"

#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace yc::app {

namespace {

constexpr int kWindowSlots = 16;
constexpr uint32_t kMaxRows = 3000, kMaxCols = 200;  // cells in one picture (more is unreadable anyway)

struct ShotUi {
  bool headings = false, hires = false;
  Image last;
  std::string name, what;  // file name (without .png), what was captured
  ImTextureID tex{};
  int tex_w = 0, tex_h = 0;
  bool open = false;
  double shown_at = 0;
  std::wstring saved, temp_file;
  std::string note;  // e.g. "first 3,000 rows"
  struct Win {
    std::string label, name;
  };
  std::vector<Win> wins;
  double wins_at = -1;
};
ShotUi& U() {
  static ShotUi u;
  return u;
}

double Now() { return ImGui::GetTime(); }

std::string Sanitize(std::string s) {
  for (char& ch : s) {
    if (std::string("\\/:*?\"<>|").find(ch) != std::string::npos) ch = ' ';
  }
  return s;
}

std::string BookName(const AppState& s) {
  if (s.file_path.empty()) return "Book1";
  return ToUtf8(std::filesystem::path(s.file_path).stem().wstring());
}

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return b;
}

// a smaller copy for the preview's texture (box filter)
Image Thumb(const Image& img, int max_w, int max_h) {
  const double k = std::min({1.0, static_cast<double>(max_w) / img.w, static_cast<double>(max_h) / img.h});
  Image t;
  t.w = std::max(1, static_cast<int>(img.w * k));
  t.h = std::max(1, static_cast<int>(img.h * k));
  t.rgba.assign(static_cast<size_t>(t.w) * t.h * 4, 255);
  for (int y = 0; y < t.h; ++y) {
    const int y0 = static_cast<int>(y / k), y1 = std::max(y0 + 1, std::min(img.h, static_cast<int>((y + 1) / k)));
    for (int x = 0; x < t.w; ++x) {
      const int x0 = static_cast<int>(x / k), x1 = std::max(x0 + 1, std::min(img.w, static_cast<int>((x + 1) / k)));
      unsigned sum[3] = {0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1; yy += std::max(1, (y1 - y0) / 4)) {
        for (int xx = x0; xx < x1; xx += std::max(1, (x1 - x0) / 4)) {
          const uint8_t* p = &img.rgba[(static_cast<size_t>(yy) * img.w + xx) * 4];
          sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], ++n;
        }
      }
      uint8_t* q = &t.rgba[(static_cast<size_t>(y) * t.w + x) * 4];
      for (int c = 0; c < 3; ++c) q[c] = static_cast<uint8_t>(sum[c] / std::max(1u, n));
    }
  }
  return t;
}

std::wstring TempDir() {
  wchar_t tmp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tmp);
  std::filesystem::path p = std::filesystem::path(tmp) / L"YCGrid" / L"Screenshots";
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  return p.wstring();
}

std::wstring PicturesDir() {
  PWSTR p = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)) && p) out = p;
  if (p) ::CoTaskMemFree(p);
  return out;
}

std::wstring SaveDialog(HWND owner, const std::wstring& name) {
  if (std::wstring a; NoFileDialog(owner, TR("Save Screenshot"), &a)) return a;
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::wstring result;
  IFileSaveDialog* dlg = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
    const COMDLG_FILTERSPEC types[] = {{TRW(L"PNG Picture (*.png)"), L"*.png"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"png");
    dlg->SetFileName(name.c_str());
    IShellItem* folder = nullptr;
    const std::wstring pics = PicturesDir();
    if (!pics.empty() && SUCCEEDED(::SHCreateItemFromParsingName(pics.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
      dlg->SetDefaultFolder(folder);
      folder->Release();
    }
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

const model::Table* TableAt(const model::Sheet& sh, uint32_t r, uint32_t c) {
  for (const auto& t : *sh.tables) {
    if (t.range.Contains(r, c)) return &t;
  }
  return nullptr;
}

// a sheet's printable cells (data, formats, objects), capped
Rect Cap(Rect r, std::string* note) {
  if (r.r1 - r.r0 + 1 > kMaxRows) {
    r.r1 = r.r0 + kMaxRows - 1;
    *note = TR("the first 3,000 rows");
  }
  if (r.c1 - r.c0 + 1 > kMaxCols) {
    r.c1 = r.c0 + kMaxCols - 1;
    *note = note->empty() ? TR("the first 200 columns") : TR("the first 3,000 rows and 200 columns");
  }
  return r;
}

ImageOptions Options(AppState& s) {
  ImageOptions o;
  const auto& v = s.FocusView();
  o.px_per_pt = std::max(0.5, v.px_scale / 0.75) * (U().hires ? 2.0 : 1.0);
  o.headings = U().headings;
  o.gridlines = s.show_gridlines;
  return o;
}

}  // namespace

// ---- encoding and the Clipboard ----
bool EncodePng(const Image& img, std::string* bytes) {
  if (img.w <= 0 || img.h <= 0) return false;
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  IWICImagingFactory* f = nullptr;
  IStream* mem = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && f &&
      SUCCEEDED(::CreateStreamOnHGlobal(nullptr, TRUE, &mem)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
      SUCCEEDED(enc->Initialize(mem, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
      SUCCEEDED(frame->SetSize(static_cast<UINT>(img.w), static_cast<UINT>(img.h)))) {
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    frame->SetPixelFormat(&fmt);
    std::vector<uint8_t> px;
    const uint8_t* src = img.rgba.data();
    if (fmt != GUID_WICPixelFormat32bppRGBA) {  // the encoder wants BGRA: swap red and blue, keep the alpha
      // (an alpha of 255 made every transparent picture opaque: Insert Icons drew dark gray squares)
      px.resize(img.rgba.size());
      for (size_t i = 0; i < px.size(); i += 4) px[i] = src[i + 2], px[i + 1] = src[i + 1], px[i + 2] = src[i], px[i + 3] = src[i + 3];
      src = px.data();
    }
    ok = SUCCEEDED(frame->WritePixels(static_cast<UINT>(img.h), static_cast<UINT>(img.w) * 4, static_cast<UINT>(img.rgba.size()), const_cast<BYTE*>(src))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    if (ok) {
      HGLOBAL h = nullptr;
      STATSTG st{};
      ok = SUCCEEDED(::GetHGlobalFromStream(mem, &h)) && SUCCEEDED(mem->Stat(&st, STATFLAG_NONAME));
      if (ok) {
        const void* p = ::GlobalLock(h);
        bytes->assign(static_cast<const char*>(p), static_cast<size_t>(st.cbSize.QuadPart));
        ::GlobalUnlock(h);
      }
    }
  }
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (mem) mem->Release();
  if (f) f->Release();
  if (SUCCEEDED(init)) ::CoUninitialize();
  return ok;
}

bool SavePngFile(const std::wstring& path, const Image& img) {
  std::string bytes;
  if (!EncodePng(img, &bytes)) return false;
  std::ofstream f(std::filesystem::path(path), std::ios::binary);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

bool CopyImageToClipboard(HWND owner, const Image& img) {
  if (!UserStateSaved()) {  // a hidden instance leaves the user's clipboard alone: it keeps the picture to itself
    std::string enc;
    if (EncodePng(img, &enc)) SetPrivateClipboardImage(enc);
    return true;
  }
  if (img.w <= 0 || img.h <= 0) return false;
  // CF_DIB (every program) and "PNG" (chats, mail and Office keep the exact pixels)
  const size_t bytes = sizeof(BITMAPINFOHEADER) + static_cast<size_t>(img.w) * img.h * 4;
  HGLOBAL dib = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!dib) return false;
  {
    auto* p = static_cast<BYTE*>(::GlobalLock(dib));
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = img.w;
    bih.biHeight = img.h;  // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    std::memcpy(p, &bih, sizeof(bih));
    BYTE* px = p + sizeof(bih);
    for (int y = 0; y < img.h; ++y) {
      const uint8_t* src = &img.rgba[static_cast<size_t>(y) * img.w * 4];
      BYTE* dst = px + static_cast<size_t>(img.h - 1 - y) * img.w * 4;
      for (int x = 0; x < img.w; ++x) dst[x * 4] = src[x * 4 + 2], dst[x * 4 + 1] = src[x * 4 + 1], dst[x * 4 + 2] = src[x * 4], dst[x * 4 + 3] = 255;
    }
    ::GlobalUnlock(dib);
  }
  HGLOBAL png = nullptr;
  std::string enc;
  if (EncodePng(img, &enc) && (png = ::GlobalAlloc(GMEM_MOVEABLE, enc.size()))) {
    std::memcpy(::GlobalLock(png), enc.data(), enc.size());
    ::GlobalUnlock(png);
  }
  if (!::OpenClipboard(owner)) {
    ::GlobalFree(dib);
    if (png) ::GlobalFree(png);
    return false;
  }
  ::EmptyClipboard();
  const bool ok = ::SetClipboardData(CF_DIB, dib) != nullptr;
  if (!ok) ::GlobalFree(dib);
  if (png && !::SetClipboardData(::RegisterClipboardFormatW(L"PNG"), png)) ::GlobalFree(png);
  ::CloseClipboard();
  return ok;
}

bool CopyFilesToClipboard(HWND owner, const std::vector<std::wstring>& paths) {
  if (!UserStateSaved()) return true;  // a hidden instance leaves the user's clipboard alone
  size_t chars = 1;
  for (const auto& p : paths) chars += p.size() + 1;
  HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
  if (!h) return false;
  auto* df = static_cast<DROPFILES*>(::GlobalLock(h));
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  auto* w = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
  for (const auto& p : paths) {
    std::memcpy(w, p.c_str(), p.size() * sizeof(wchar_t));
    w += p.size() + 1;
  }
  ::GlobalUnlock(h);
  if (!::OpenClipboard(owner)) {
    ::GlobalFree(h);
    return false;
  }
  ::EmptyClipboard();
  const bool ok = ::SetClipboardData(CF_HDROP, h) != nullptr;
  if (!ok) ::GlobalFree(h);
  if (HGLOBAL eff = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {  // paste in Explorer copies, not moves
    *static_cast<DWORD*>(::GlobalLock(eff)) = DROPEFFECT_COPY;
    ::GlobalUnlock(eff);
    if (!::SetClipboardData(::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), eff)) ::GlobalFree(eff);
  }
  ::CloseClipboard();
  return ok;
}

// ---- what a picture of cells shows ----
bool CellParts(AppState& s, ShotWhat what, std::vector<ImagePart>* parts, std::string* label, std::string* note) {
  const model::Sheet* sh = s.FocusSheet();
  if (!sh) return false;
  const auto& v = s.FocusView();
  auto add = [&](const model::Sheet& x, Rect r, bool caption) {
    if (!x.chart_sheet) r = Cap(r, note);
    parts->push_back(ImagePart{x.id, r, caption ? x.name : std::string()});
  };
  bool empty = false;
  switch (what) {
    case ShotWhat::Selection: {
      Rect r = SelRect(s);
      if (r.Count() <= 1) {  // one cell: its table, else its data region
        if (const model::Table* t = TableAt(*sh, r.r0, r.c0)) r = t->range;
        else r = engine::CurrentRegion(*sh, r.r0, r.c0);
      }
      r.r1 = std::min(r.r1, sh->row_limit - 1);
      add(*sh, r, false);
      *label = sh->name + "!" + core::RectA1(r);
      return true;
    }
    case ShotWhat::Table: {
      const Rect sel = SelRect(s);
      const model::Table* t = TableAt(*sh, sel.r0, sel.c0);
      const Rect r = t ? t->range : engine::CurrentRegion(*sh, sel.r0, sel.c0);
      add(*sh, r, false);
      *label = t ? t->name : sh->name + "!" + core::RectA1(r);
      return true;
    }
    case ShotWhat::Sheet: {
      const Rect r = engine::PrintableRange(*sh, &empty);
      if (empty && !sh->chart_sheet) return false;
      add(*sh, r, false);
      *label = sh->name;
      return true;
    }
    case ShotWhat::Workbook: {
      for (const auto& x : s.snap->wb.sheets) {
        if (!x->visible) continue;
        const Rect r = engine::PrintableRange(*x, &empty);
        if (empty && !x->chart_sheet) continue;
        add(*x, r, true);
      }
      *label = BookName(s);
      return !parts->empty();
    }
    default:
      (void)v;
      return false;
  }
}

namespace {

void Finish(AppState& s, Image&& img, const std::string& what, const std::string& note) {
  ShotUi& u = U();
  u.last = std::move(img);
  u.what = what;
  u.note = note;
  u.name = Sanitize(BookName(s) + " - " + what + " " + Stamp());
  u.saved.clear();
  u.temp_file.clear();
  if (u.tex) ReleaseTexture(u.tex);
  const Image t = Thumb(u.last, 720, 480);
  u.tex = TextureFromRgba(t.rgba.data(), t.w, t.h);
  u.tex_w = t.w;
  u.tex_h = t.h;
  // a hidden test instance leaves the system Clipboard alone (Python's ExportImage(None) still copies)
  const bool copied = CopyImageToClipboard(s.hwnd, u.last) || s.headless;  // a hidden instance keeps it to itself
  u.open = true;
  u.shown_at = Now();
  s.SetStatus(copied ? TR("Screenshot copied: paste it with Ctrl+V") : TR("The screenshot couldn't be copied to the Clipboard"), 5.0);
  ui::RequestRedrawIn(0.0);
}

std::string WindowLabel(const char* name) {
  std::string l = name;
  if (auto k = l.find("##"); k != std::string::npos) l.erase(k);
  return l;
}

// a window in a dock group: ImGui's DockIsActive can read false for the single window of a node in a floating group
// while it shows there with its tab (Begin gave it the ChildWindow flag inside the node's host)
bool InDock(const ImGuiWindow* w) {
  return w->DockIsActive || (w->DockNode && w->DockNode->HostWindow && (w->Flags & ImGuiWindowFlags_ChildWindow));
}

// a dock group of its own over the sheets (not in the main window's dock space)
bool InFloatingGroup(ImGuiWindow* w) {
  if (!w->DockNode) return false;
  const ImGuiDockNode* root = ImGui::DockNodeGetRootNode(w->DockNode);
  return root && root->IsFloatingNode();
}

// the floating windows that can be captured (figures, monitors, panes)
void RefreshWindows(AppState& s) {
  ShotUi& u = U();
  if (Now() - u.wins_at < 0.5) return;
  u.wins_at = Now();
  u.wins.clear();
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (ImGuiWindow* w : g.Windows) {
    // docked windows count (ImGui makes them child windows without a title bar)
    if (!w->WasActive || w->Hidden || (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) ||
        (!InDock(w) && (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoTitleBar))) || w->Rect().GetWidth() < 40) {
      continue;
    }
    const std::string title = ui::LiveWindowTitle(w->Name);  // the title shown now (a ###id window's name stays as first given)
    const std::string label = WindowLabel(title.c_str());
    if (label.empty() || label.rfind("Debug", 0) == 0) continue;
    u.wins.push_back({label, title});
  }
  std::vector<std::string> menu = {"ScreenshotSelection", "ScreenshotTable", "ScreenshotSheet", "ScreenshotWorkbook", "-", "ScreenshotVisible", "ScreenshotWindow"};
  for (int i = 0; i < kWindowSlots; ++i) {
    ui::Command* c = ui::Commands().Find("ScreenshotWin" + std::to_string(i));
    if (!c) continue;
    const bool used = i < static_cast<int>(u.wins.size());
    c->label = used ? TR("Window: ") + u.wins[static_cast<size_t>(i)].label : "";
    if (used) menu.push_back(c->id);
  }
  menu.insert(menu.end(), {"-", "ScreenshotHeadings", "ScreenshotHiRes", "-", "ScreenshotSaveLast"});
  for (auto& t : ui::ribbon::Tabs()) {
    for (auto& gr : t.groups) {
      for (auto& b : gr.blocks) {
        for (auto& it : b.items) {
          if (it.cmd == "Screenshot") it.menu = menu;
        }
      }
    }
  }
  (void)s;
}

}  // namespace

void TakeScreenshot(AppState& s, ShotWhat what, const std::string& window) {
  if (what == ShotWhat::Visible || what == ShotWhat::Window || what == ShotWhat::FloatingWindow) {
    AppState::ShotRequest q;
    q.target = what == ShotWhat::Visible ? "grid" : what == ShotWhat::Window ? "window" : "imgui_window";
    q.address = window;
    q.headers = U().headings;
    const std::string label = what == ShotWhat::Visible ? (s.FocusSheet() ? s.FocusSheet()->name : std::string("Sheet"))
                              : what == ShotWhat::Window ? std::string("YCGrid")
                                                         : WindowLabel(window.c_str());
    q.on_pixels = [&s, label](std::vector<uint8_t>&& px, int w, int h) {
      Image img;
      img.w = w;
      img.h = h;
      img.rgba = std::move(px);
      Finish(s, std::move(img), label, "");
    };
    U().open = false;  // the preview isn't in the picture
    s.shots.push_back(std::move(q));
    ui::RequestRedrawIn(0.0);
    return;
  }
  std::vector<ImagePart> parts;
  std::string label, note;
  if (!CellParts(s, what, &parts, &label, &note)) {
    s.SetStatus(TR("There's nothing to take a picture of here"));
    return;
  }
  Image img;
  bool scaled = false;
  if (!RenderImage(*s.snap, parts, Options(s), &img.rgba, &img.w, &img.h, &scaled)) {
    s.SetStatus(TR("The screenshot couldn't be made"));
    return;
  }
  if (scaled) note = note.empty() ? TR("lower resolution (large picture)") : note + TR(", lower resolution");
  Finish(s, std::move(img), label, note);
}

// ---- Python: Range / Worksheet / Workbook.ExportImage, yc.ui.windows ----
io::MV ScreenshotRequest(AppState& s, const std::string& m, const io::MV& p, std::string* err) {
  if (m == "image.windows") {
    U().wins_at = -1;
    if (!ui::RecordingUiItems()) ui::RecordUiItems(true);  // the titles shown come from the item recorder (tests)
    RefreshWindows(s);
    io::MV a = io::MV::Array();
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    auto rect = [&](ImVec2 p, ImVec2 sz) {
      io::MV r = io::MV::Array();
      for (float v : {p.x - origin.x, p.y - origin.y, p.x + sz.x - origin.x, p.y + sz.y - origin.y}) r.Push(io::MV::Double(v));
      return r;
    };
    // a docked window's own tab, where a user grabs it (the first tab of a group can belong to another window)
    auto tab = [&](io::MV& x, ImGuiWindow* iw) {
      if (!iw->DockNode || !iw->DockNode->TabBar) return;
      ImGuiTabBar* tb = iw->DockNode->TabBar;
      if (const ImGuiTabItem* t = ImGui::TabBarFindTabByID(tb, iw->TabId)) {
        // the part a person sees: a tab wider than its narrow node is cut at the node's edge, and its middle lay on the
        // splitter beside it (chaos seed 5338 grabbed it there and resized the node)
        const float x0 = std::max(tb->BarRect.Min.x, tb->BarRect.Min.x + t->Offset - tb->ScrollingAnim);
        const float x1 = std::min(tb->BarRect.Max.x, tb->BarRect.Min.x + t->Offset - tb->ScrollingAnim + t->Width);
        if (x1 <= x0) return;
        x.Set("tab", rect(ImVec2(x0, tb->BarRect.Min.y), ImVec2(x1 - x0, tb->BarRect.GetHeight())));
        x.Set("tab_selected", io::MV::Bool(tb->SelectedTabId == iw->TabId));
      }
    };
    std::vector<std::string> listed;
    for (const auto& w : U().wins) {
      io::MV x = io::MV::Map();
      x.Set("label", io::MV::Str(w.label));
      x.Set("name", io::MV::Str(w.name));
      x.Set("visible", io::MV::Bool(true));
      listed.push_back(w.name);
      // where it is, in the window pixels ui.input takes (tests check that a locked window stays put)
      if (ImGuiWindow* iw = ImGui::FindWindowByName(w.name.c_str())) {
        x.Set("rect", rect(iw->Pos, iw->Size));
        x.Set("docked", io::MV::Bool(InDock(iw)));
        if (InDock(iw)) {
          tab(x, iw);
          x.Set("floating", io::MV::Bool(InFloatingGroup(iw)));  // its group floats over the sheets
        }
      }
      a.Push(std::move(x));
    }
    // the other tabs of a group: docked but not shown, reachable by their tab (not in the Screenshot menu: they are
    // hidden behind the selected tab)
    for (ImGuiWindow* iw : ImGui::GetCurrentContext()->Windows) {
      if (!iw->DockNode || !iw->DockNode->TabBar || !iw->DockNode->IsVisible) continue;  // a group on the screen
      if (std::find(listed.begin(), listed.end(), iw->Name) != listed.end()) continue;
      if (!ImGui::TabBarFindTabByID(iw->DockNode->TabBar, iw->TabId)) continue;
      const std::string label = WindowLabel(iw->Name);
      if (label.empty() || label.rfind("Debug", 0) == 0) continue;
      io::MV x = io::MV::Map();
      x.Set("label", io::MV::Str(label));
      x.Set("name", io::MV::Str(iw->Name));
      x.Set("visible", io::MV::Bool(false));
      x.Set("rect", rect(iw->DockNode->Pos, iw->DockNode->Size));
      x.Set("docked", io::MV::Bool(true));
      x.Set("floating", io::MV::Bool(InFloatingGroup(iw)));
      tab(x, iw);
      a.Push(std::move(x));
    }
    return a;
  }
  // image.export: what = range / sheet / workbook
  const std::string what = p.GetStr("what", "range");
  std::vector<ImagePart> parts;
  std::string note;
  bool empty = false;
  auto cap = [&](Rect r) { return Cap(r, &note); };
  if (what == "workbook") {
    for (const auto& x : s.snap->wb.sheets) {
      if (!x->visible) continue;
      const Rect r = engine::PrintableRange(*x, &empty);
      if (empty && !x->chart_sheet) continue;
      parts.push_back({x->id, x->chart_sheet ? r : cap(r), x->name});
    }
  } else {
    const uint32_t sheet = static_cast<uint32_t>(p.GetInt("sheet"));
    const model::Sheet* sh = s.snap->Sheet(sheet);
    if (!sh) {
      *err = "No such sheet";
      return {};
    }
    Rect r;
    if (what == "sheet") {
      r = engine::PrintableRange(*sh, &empty);
      if (empty && !sh->chart_sheet) {
        *err = "The sheet is empty";
        return {};
      }
    } else if (const io::MV* rr = p.Get("rect"); rr && rr->a.size() >= 4) {
      r = Rect{static_cast<uint32_t>(rr->a[0].i), static_cast<uint32_t>(rr->a[1].i), static_cast<uint32_t>(rr->a[2].i), static_cast<uint32_t>(rr->a[3].i)};
      r.r1 = std::min(r.r1, sh->row_limit - 1);
    }
    parts.push_back({sheet, sh->chart_sheet ? r : cap(r), ""});
  }
  if (parts.empty()) {
    *err = "Nothing to draw";
    return {};
  }
  ImageOptions o;
  o.px_per_pt = 96.0 / 72.0 * std::clamp(p.GetDouble("scale", 1.0), 0.1, 8.0);
  o.headings = p.GetBool("headings", false);
  o.gridlines = p.GetBool("gridlines", s.show_gridlines);
  Image img;
  bool scaled = false;
  if (!RenderImage(*s.snap, parts, o, &img.rgba, &img.w, &img.h, &scaled)) {
    *err = "The picture couldn't be drawn";
    return {};
  }
  const std::wstring path = FromUtf8(p.GetStr("path"));
  const bool ok = path.empty() ? CopyImageToClipboard(s.hwnd, img) : SavePngFile(path, img);
  if (!ok) {
    *err = path.empty() ? "The Clipboard is busy" : "The PNG couldn't be written";
    return {};
  }
  io::MV x = io::MV::Map();
  x.Set("width", io::MV::Int(img.w));
  x.Set("height", io::MV::Int(img.h));
  x.Set("scaled_down", io::MV::Bool(scaled));
  x.Set("note", io::MV::Str(note));
  return x;
}

void RegisterScreenshotCommands(AppState& s) {
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
  R.Add(make("Screenshot", TR("Screenshot"), ui::icon::Screenshot, "Ctrl+Shift+S",
             TR("Copy a picture of the selection (one cell: its table or data region) to share in chats, mail or documents. "
                "The arrow: a table, the sheet, the whole workbook, the window or a chart window.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Selection); };
  R.Add(make("ScreenshotSelection", TR("Selection"), nullptr, "", TR("The selected cells, as they look, without the selection marks.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Selection); };
  R.Add(make("ScreenshotTable", TR("Table or Data Region"), nullptr, "", TR("The table around the active cell, or its block of data.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Table); };
  R.Add(make("ScreenshotSheet", TR("Whole Sheet"), nullptr, "", TR("Every cell, chart and picture of this sheet, even what is off the screen.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Sheet); };
  R.Add(make("ScreenshotWorkbook", TR("Whole Workbook"), nullptr, "", TR("Every visible sheet, one under another with its name.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Workbook); };
  R.Add(make("ScreenshotVisible", TR("Visible Area"), nullptr, "", TR("The cells on the screen now, exactly as you see them.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Visible); };
  R.Add(make("ScreenshotWindow", TR("YCGrid Window"), nullptr, "", TR("The whole YCGrid window with its charts and panes.")))
      .execute = [&s](ui::Command&) { TakeScreenshot(s, ShotWhat::Window); };
  for (int i = 0; i < kWindowSlots; ++i) {
    static std::vector<std::string> ids;
    ids.push_back("ScreenshotWin" + std::to_string(i));
    R.Add(make(ids.back().c_str(), "")).execute = [&s, i](ui::Command&) {
      if (i < static_cast<int>(U().wins.size())) TakeScreenshot(s, ShotWhat::FloatingWindow, U().wins[static_cast<size_t>(i)].name);
    };
  }
  ui::Command head = make("ScreenshotHeadings", TR("Include Row and Column Headings"));
  head.toggle = true;
  R.Add(head).execute = [](ui::Command& c) { U().headings = c.pressed; };
  ui::Command hi = make("ScreenshotHiRes", TR("High Resolution (2x)"), nullptr, "", TR("Twice the pixels: sharp when zoomed in or on high-DPI screens."));
  hi.toggle = true;
  R.Add(hi).execute = [](ui::Command& c) { U().hires = c.pressed; };
  R.Add(make("ScreenshotSaveLast", TR("Save Last Screenshot..."), nullptr, "", TR("Save the last screenshot as a PNG file.")))
      .execute = [&s](ui::Command&) {
        ShotUi& u = U();
        if (u.last.w <= 0) {
          s.SetStatus(TR("No screenshot yet"));
          return;
        }
        const std::wstring path = SaveDialog(s.hwnd, FromUtf8(u.name + ".png"));
        if (path.empty()) return;
        if (SavePngFile(path, u.last)) {
          u.saved = path;
          s.SetStatus(TR("Saved: ") + ToUtf8(path), 6.0);
        } else {
          s.SetStatus(TR("The PNG couldn't be written"), 6.0);
        }
      };
}

// the preview after a screenshot: bottom right, over everything, closes by itself
void DrawScreenshot(AppState& s) {
  RefreshWindows(s);
  ShotUi& u = U();
  if (!u.open || !u.tex) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float pad = ui::S(16);
  const float tw = std::min(static_cast<float>(u.tex_w), ui::S(340)), th = tw * u.tex_h / std::max(1, u.tex_w);
  const float th_c = std::min(th, ui::S(220));
  const float tw_c = th_c < th ? th_c * u.tex_w / std::max(1, u.tex_h) : tw;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + vp->WorkSize.y - pad - ui::S(26)), ImGuiCond_Always, ImVec2(1, 1));
  ImGui::SetNextWindowBgAlpha(0.98f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::S(8));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::S(12), ui::S(10)));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
  bool hovered = false;
  if (ImGui::Begin("##yc_screenshot", nullptr, flags)) {
    // lesson-ok: hover-popups the toast opens no popup; it only stays while the mouse is over it
    hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::TextColored(ImVec4(0.13f, 0.55f, 0.25f, 1), "%s", TR("Copied to the Clipboard"));
    // the x at the picture's right edge (a place taken from the window's own width made the window narrow every frame)
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetStyle().WindowPadding.x + tw_c - ui::S(16)));
    if (ImGui::SmallButton("x")) u.open = false;
    ImGui::Image(u.tex, ImVec2(tw_c, th_c));
    if (ImGui::IsItemClicked()) {  // open it in the picture viewer (to mark it up or share from there)
      if (u.temp_file.empty()) {
        u.temp_file = (std::filesystem::path(TempDir()) / FromUtf8(u.name + ".png")).wstring();
        SavePngFile(u.temp_file, u.last);
      }
      if (!OpenOnDesktop(u.temp_file)) s.SetStatus(TR("Open: ") + ToUtf8(u.temp_file), 4.0);  // a hidden instance opens no viewer
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Click to open it in the picture viewer"));
    ImGui::TextDisabled("%s  -  %d x %d px%s%s", u.what.c_str(), u.last.w, u.last.h, u.note.empty() ? "" : "  -  ", u.note.c_str());
    ImGui::TextDisabled("%s", TR("Paste with Ctrl+V into a chat, mail or document."));
    if (ImGui::Button(TR("Save As..."))) {
      const std::wstring path = SaveDialog(s.hwnd, FromUtf8(u.name + ".png"));
      if (!path.empty()) {
        if (SavePngFile(path, u.last)) u.saved = path, s.SetStatus(TR("Saved: ") + ToUtf8(path), 6.0);
        else s.SetStatus(TR("The PNG couldn't be written"), 6.0);
      }
      u.shown_at = Now();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Copy as File"))) {  // for chats and mail that take files (full quality, no recompression)
      if (u.temp_file.empty()) {
        u.temp_file = (std::filesystem::path(TempDir()) / FromUtf8(u.name + ".png")).wstring();
        SavePngFile(u.temp_file, u.last);
      }
      s.SetStatus(CopyFilesToClipboard(s.hwnd, {u.temp_file}) ? TR("The PNG file is on the Clipboard: paste it into a chat, a mail or a folder")
                                                              : TR("The Clipboard is busy"), 6.0);
      u.shown_at = Now();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Copy Picture"))) {
      CopyImageToClipboard(s.hwnd, u.last);
      s.SetStatus(TR("Screenshot copied: paste it with Ctrl+V"), 4.0);
      u.shown_at = Now();
    }
    if (!u.saved.empty()) {
      ImGui::SameLine();
      if (ImGui::Button(TR("Show in Folder"))) {
        const std::wstring arg = L"/select,\"" + u.saved + L"\"";
        if (!OpenOnDesktop(L"explorer.exe", arg)) s.SetStatus(TR("Open: ") + ToUtf8(u.saved), 4.0);
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  if (hovered) u.shown_at = Now();
  if (Now() - u.shown_at > 12.0) u.open = false;
  else ui::RequestRedrawIn(0.5);
}

}  // namespace yc::app
