// SPDX-License-Identifier: Apache-2.0
// main.cpp - YCGrid main program
//
// Win32 + D3D11 (flip model, frame latency waitable) + ImGui 1.93 docking + ImPlot.
// The engine runs on its own thread; the UI reads the latest snapshot every frame and sends commands with Engine::Post (PLAN §4).
//
// Command line:
//   ycgrid.exe [file.ycg] [--frames N] [--warp] [--width W] [--height H] [--report out.json] [--screenshot out.png] [--hidden]
//             [--demo] [--big-data] [--expand-rows] [--top-row N] [--ribbon-tab N] [--float-sheet N] [--new-container]
//             [--backstage] [--collapsed] [--save-layout] [--load-layout] [--dock-into-container]
//             [--install [--no-desktop]|--uninstall] [--silent] [--no-handoff]
#include "app_state.h"
#include "core/i18n.h"
#include "python_bridge.h"
#include "settings.h"
#include "trust.h"

#include "app/shell_integration.h"
#include "commands/command_registry.h"
#include "dock/dock_layout.h"
#include "engine/error_check.h"
#include "engine/navigate.h"
#include "fonts/cell_fonts.h"
#include "fonts/fonts.h"
#include "formula/numfmt.h"
#include "frame/frame_context.h"
#include "icons/yc_icons.h"
#include "ribbon/ribbon.h"
#include "theme/theme.h"
#include "titlebar/titlebar.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"
#include "screenshot.h"
#include "error_log.h"
#include "watchdog.h"
#include "implot.h"
#include "net.h"
#include "yc_ui_items.h"

#include <d3d11.h>
#include <dxgi1_5.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <deque>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using namespace yc;
using app::AppState;
using core::Rect;

constexpr int kIconFontResource = 101;
constexpr UINT WM_YC_SNAPSHOT = WM_APP + 1;

// ---- Command line -------------------------------------------------------------------
struct Args {
  std::wstring file;
  int frames = 0;
  bool warp = false;
  int width = 0, height = 0;
  std::wstring report;
  std::wstring screenshot;
  bool install = false, uninstall = false, silent = false, no_handoff = false, new_window = false;
  bool no_desktop = false;  // --install without the desktop shortcut (install.cmd /nodesktop)
  int ribbon_tab = -1;
  long long top_row = -1;
  bool expand_rows = false, big_data = false, demo = false, new_container = false, backstage = false, collapsed = false;
  int float_sheet = -1;
  bool save_layout = false, load_layout = false, dock_into_container = false;
  std::wstring type;  // --type "text": type into A1 after startup (tests the editing flow)
  std::wstring keys;  // --keys "t:abc|k:Enter|k:c-B|w:3": simulated input (automated UI tests)
  std::wstring save_as;  // --save-as path.ycg: save before exiting (tests)
  bool hidden = false;   // --hidden: no window on screen (CI and agents drive it over YCP; screenshots still work)
  bool as_new = false;   // --as-new: the file opened becomes a new, unsaved workbook and is deleted (a report made for it)
  DWORD parent_pid = 0;  // --parent-pid: ends with that process (File > New in a hidden instance, yc.launch(hidden=True))
  // --automation-security N: started by a script (yc.launch): the workbook on the command line follows
  // Application.AutomationSecurity (1 Low: its code runs; 2 ByUI: the Trust Center decides; 3 ForceDisable) (trust.cpp)
  int automation_security = 0;
};

Args ParseArgs() {
  Args a;
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) {
    const std::wstring s = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
    if (s == L"--frames") a.frames = _wtoi(next().c_str());
    else if (s == L"--warp") a.warp = true;
    else if (s == L"--width") a.width = _wtoi(next().c_str());
    else if (s == L"--height") a.height = _wtoi(next().c_str());
    else if (s == L"--report") a.report = next();
    else if (s == L"--screenshot") a.screenshot = next();
    else if (s == L"--ribbon-tab") a.ribbon_tab = _wtoi(next().c_str());
    else if (s == L"--top-row") a.top_row = _wtoi64(next().c_str());
    else if (s == L"--expand-rows") a.expand_rows = true;
    else if (s == L"--big-data") a.big_data = true;
    else if (s == L"--demo") a.demo = true;
    else if (s == L"--float-sheet") a.float_sheet = _wtoi(next().c_str());
    else if (s == L"--new-container") a.new_container = true;
    else if (s == L"--backstage") a.backstage = true;
    else if (s == L"--collapsed") a.collapsed = true;
    else if (s == L"--save-layout") a.save_layout = true;
    else if (s == L"--load-layout") a.load_layout = true;
    else if (s == L"--dock-into-container") a.dock_into_container = true;
    else if (s == L"--type") a.type = next();
    else if (s == L"--keys") a.keys = next();
    else if (s == L"--save-as") a.save_as = next();
    else if (s == L"--install") a.install = true;
    else if (s == L"--no-desktop") a.no_desktop = true;
    else if (s == L"--uninstall") a.uninstall = true;
    else if (s == L"--silent" || s == L"/silent") a.silent = true;
    else if (s == L"--no-handoff") a.no_handoff = true;
    else if (s == L"--new-window") a.new_window = a.no_handoff = true;  // File > Open / New in a running YCGrid: a window of its own
    else if (s == L"--hidden") a.hidden = true;
    else if (s == L"--as-new") a.as_new = true;  // the file is a report made for a new workbook (Book2): read, then deleted
    else if (s == L"--parent-pid") a.parent_pid = static_cast<DWORD>(_wtoi64(next().c_str()));
    else if (s == L"--automation-security") a.automation_security = _wtoi(next().c_str());
    else if (s == L"--lang") next();  // UI language: core/i18n reads the command line itself
    else if (!s.empty() && s[0] != L'-') a.file = s;
  }
  ::LocalFree(argv);
  if (!a.screenshot.empty() && a.frames == 0) a.frames = 30;
  return a;
}

// ---- Direct3D 11 --------------------------------------------------------------
struct Gfx {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain2* swap = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  HANDLE frame_waitable = nullptr;
  UINT swap_flags = 0;
  UINT resize_w = 0, resize_h = 0;
} g_gfx;

void CreateRtv() {
  ID3D11Texture2D* back = nullptr;
  g_gfx.swap->GetBuffer(0, IID_PPV_ARGS(&back));
  if (back) {
    g_gfx.device->CreateRenderTargetView(back, nullptr, &g_gfx.rtv);
    back->Release();
  }
}

void ReleaseRtv() {
  if (g_gfx.rtv) {
    g_gfx.rtv->Release();
    g_gfx.rtv = nullptr;
  }
}

bool CreateDevice(HWND hwnd, bool warp) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  HRESULT hr = ::D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, &g_gfx.device, &got, &g_gfx.ctx);
  if (FAILED(hr) && !warp) {
    hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &g_gfx.device, &got,
                             &g_gfx.ctx);
  }
  if (FAILED(hr)) return false;
  IDXGIFactory2* factory = nullptr;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  BOOL tearing = FALSE;
  IDXGIFactory5* f5 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) {
    f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    f5->Release();
  }
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Scaling = DXGI_SCALING_NONE;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
  IDXGISwapChain1* sc1 = nullptr;
  hr = factory->CreateSwapChainForHwnd(g_gfx.device, hwnd, &sd, nullptr, nullptr, &sc1);
  if (FAILED(hr)) {
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForHwnd(g_gfx.device, hwnd, &sd, nullptr, nullptr, &sc1);
  }
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  factory->Release();
  if (FAILED(hr) || !sc1) return false;
  sc1->QueryInterface(IID_PPV_ARGS(&g_gfx.swap));
  sc1->Release();
  if (!g_gfx.swap) return false;
  g_gfx.swap_flags = sd.Flags;
  g_gfx.swap->SetMaximumFrameLatency(1);
  g_gfx.frame_waitable = g_gfx.swap->GetFrameLatencyWaitableObject();
  CreateRtv();
  return true;
}

void DestroyDevice() {
  ReleaseRtv();
  if (g_gfx.frame_waitable) ::CloseHandle(g_gfx.frame_waitable);
  if (g_gfx.swap) g_gfx.swap->Release();
  if (g_gfx.ctx) g_gfx.ctx->Release();
  if (g_gfx.device) g_gfx.device->Release();
  g_gfx = {};
}

// ---- pictures: image file bytes -> 32-bit BGRA pixels -> texture ----
namespace {
bool DecodeImageImpl(const std::string& bytes, std::vector<uint8_t>* px, UINT* w, UINT* h) {
  IWICImagingFactory* f = nullptr;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) || !f) return false;
  bool ok = false;
  IWICStream* st = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  if (SUCCEEDED(f->CreateStream(&st)) &&
      SUCCEEDED(st->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())), static_cast<DWORD>(bytes.size()))) &&
      SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &frame)) &&
      SUCCEEDED(f->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(conv->GetSize(w, h)) && *w > 0 && *h > 0 && *w <= 16384 && *h <= 16384) {
    px->resize(static_cast<size_t>(*w) * *h * 4);
    ok = SUCCEEDED(conv->CopyPixels(nullptr, *w * 4, static_cast<UINT>(px->size()), px->data()));
  }
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  f->Release();
  return ok;
}
}  // namespace
bool YcDecodeImage(const std::string& bytes, std::vector<uint8_t>* px, UINT* w, UINT* h) { return DecodeImageImpl(bytes, px, w, h); }
ID3D11Device* YcDevice() { return g_gfx.device; }


// back buffer -> PNG (only reads back YCGrid's own window)
// save the main window's back buffer as PNG; whole image when crop is empty, scaled down proportionally when max_width > 0
bool SaveBackbufferPng(const std::wstring& path, RECT crop = {}, int max_width = 0, int* out_w = nullptr, int* out_h = nullptr) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_gfx.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  D3D11_TEXTURE2D_DESC d{};
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  bool ok = SUCCEEDED(g_gfx.device->CreateTexture2D(&d, nullptr, &staging));
  if (ok) g_gfx.ctx->CopyResource(staging, back);
  back->Release();
  if (!ok) return false;
  if (crop.right <= crop.left || crop.bottom <= crop.top) crop = RECT{0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
  crop.left = std::clamp<LONG>(crop.left, 0, static_cast<LONG>(d.Width) - 1);
  crop.top = std::clamp<LONG>(crop.top, 0, static_cast<LONG>(d.Height) - 1);
  crop.right = std::clamp<LONG>(crop.right, crop.left + 1, static_cast<LONG>(d.Width));
  crop.bottom = std::clamp<LONG>(crop.bottom, crop.top + 1, static_cast<LONG>(d.Height));
  const UINT cw = static_cast<UINT>(crop.right - crop.left), ch = static_cast<UINT>(crop.bottom - crop.top);
  D3D11_MAPPED_SUBRESOURCE m{};
  ok = SUCCEEDED(g_gfx.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
  std::vector<BYTE> bgra;
  if (ok) {
    bgra.resize(static_cast<size_t>(cw) * ch * 4);
    for (UINT y = 0; y < ch; ++y) {
      const BYTE* src = static_cast<const BYTE*>(m.pData) + static_cast<size_t>(y + static_cast<UINT>(crop.top)) * m.RowPitch + static_cast<size_t>(crop.left) * 4;
      BYTE* dst = bgra.data() + static_cast<size_t>(y) * cw * 4;
      for (UINT x = 0; x < cw; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = 255;
      }
    }
    g_gfx.ctx->Unmap(staging, 0);
  }
  staging->Release();
  if (!ok) return false;
  UINT ow = cw, oh = ch;
  if (max_width > 0 && cw > static_cast<UINT>(max_width)) {
    ow = static_cast<UINT>(max_width);
    oh = std::max<UINT>(1, static_cast<UINT>(static_cast<uint64_t>(ch) * ow / cw));
  }
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IWICImagingFactory* f = nullptr;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IWICBitmap* bmp = nullptr;
  IWICBitmapScaler* scaler = nullptr;
  ok = SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&stream)) &&
       SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
       SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
       SUCCEEDED(frame->SetSize(ow, oh)) &&
       SUCCEEDED(f->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppBGRA, cw * 4, static_cast<UINT>(bgra.size()), bgra.data(), &bmp)) &&
       SUCCEEDED(f->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(bmp, ow, oh, WICBitmapInterpolationModeFant));
  if (ok) {
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WriteSource(scaler, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
  }
  if (scaler) scaler->Release();
  if (bmp) bmp->Release();
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (f) f->Release();
  if (SUCCEEDED(init)) ::CoUninitialize();
  if (out_w) *out_w = static_cast<int>(ow);
  if (out_h) *out_h = static_cast<int>(oh);
  return ok;
}

// a region of the back buffer as RGBA pixels (top-down); the whole buffer when crop is empty
bool ReadBackbuffer(RECT crop, std::vector<uint8_t>* rgba, int* out_w, int* out_h) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_gfx.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  D3D11_TEXTURE2D_DESC d{};
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  bool ok = SUCCEEDED(g_gfx.device->CreateTexture2D(&d, nullptr, &staging));
  if (ok) g_gfx.ctx->CopyResource(staging, back);
  back->Release();
  if (!ok) return false;
  if (crop.right <= crop.left || crop.bottom <= crop.top) crop = RECT{0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
  crop.left = std::clamp<LONG>(crop.left, 0, static_cast<LONG>(d.Width) - 1);
  crop.top = std::clamp<LONG>(crop.top, 0, static_cast<LONG>(d.Height) - 1);
  crop.right = std::clamp<LONG>(crop.right, crop.left + 1, static_cast<LONG>(d.Width));
  crop.bottom = std::clamp<LONG>(crop.bottom, crop.top + 1, static_cast<LONG>(d.Height));
  const UINT cw = static_cast<UINT>(crop.right - crop.left), ch = static_cast<UINT>(crop.bottom - crop.top);
  D3D11_MAPPED_SUBRESOURCE m{};
  ok = SUCCEEDED(g_gfx.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
  if (ok) {
    rgba->resize(static_cast<size_t>(cw) * ch * 4);
    for (UINT y = 0; y < ch; ++y) {
      const BYTE* src = static_cast<const BYTE*>(m.pData) + static_cast<size_t>(y + static_cast<UINT>(crop.top)) * m.RowPitch + static_cast<size_t>(crop.left) * 4;
      BYTE* dst = rgba->data() + static_cast<size_t>(y) * cw * 4;
      std::memcpy(dst, src, static_cast<size_t>(cw) * 4);
      for (UINT x = 0; x < cw; ++x) dst[x * 4 + 3] = 255;
    }
    g_gfx.ctx->Unmap(staging, 0);
  }
  staging->Release();
  if (out_w) *out_w = static_cast<int>(cw);
  if (out_h) *out_h = static_cast<int>(ch);
  return ok;
}

// Copy as Picture: put a region of the back buffer on the clipboard (CF_DIB, 32-bit)
HWND ClipboardOwner();
bool CopyBackbufferToClipboard(RECT crop, int* out_w, int* out_h) {
  if (!app::UserStateSaved()) {  // a hidden instance leaves the user's clipboard alone
    if (out_w) *out_w = static_cast<int>(crop.right - crop.left);
    if (out_h) *out_h = static_cast<int>(crop.bottom - crop.top);
    return true;
  }
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_gfx.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  D3D11_TEXTURE2D_DESC d{};
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  bool ok = SUCCEEDED(g_gfx.device->CreateTexture2D(&d, nullptr, &staging));
  if (ok) g_gfx.ctx->CopyResource(staging, back);
  back->Release();
  if (!ok) return false;
  crop.left = std::clamp<LONG>(crop.left, 0, static_cast<LONG>(d.Width) - 1);
  crop.top = std::clamp<LONG>(crop.top, 0, static_cast<LONG>(d.Height) - 1);
  crop.right = std::clamp<LONG>(crop.right, crop.left + 1, static_cast<LONG>(d.Width));
  crop.bottom = std::clamp<LONG>(crop.bottom, crop.top + 1, static_cast<LONG>(d.Height));
  const UINT cw = static_cast<UINT>(crop.right - crop.left), ch = static_cast<UINT>(crop.bottom - crop.top);
  D3D11_MAPPED_SUBRESOURCE m{};
  ok = SUCCEEDED(g_gfx.ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
  HGLOBAL h = nullptr;
  if (ok) {
    const size_t bytes = sizeof(BITMAPINFOHEADER) + static_cast<size_t>(cw) * ch * 4;
    h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
      auto* p = static_cast<BYTE*>(::GlobalLock(h));
      BITMAPINFOHEADER bih{};
      bih.biSize = sizeof(bih);
      bih.biWidth = static_cast<LONG>(cw);
      bih.biHeight = static_cast<LONG>(ch);  // bottom-up
      bih.biPlanes = 1;
      bih.biBitCount = 32;
      bih.biCompression = BI_RGB;
      std::memcpy(p, &bih, sizeof(bih));
      BYTE* px = p + sizeof(bih);
      for (UINT y = 0; y < ch; ++y) {
        const BYTE* src = static_cast<const BYTE*>(m.pData) + static_cast<size_t>(crop.top + static_cast<LONG>(y)) * m.RowPitch + static_cast<size_t>(crop.left) * 4;
        BYTE* dst = px + static_cast<size_t>(ch - 1 - y) * cw * 4;
        for (UINT x = 0; x < cw; ++x) {  // RGBA -> BGRA
          dst[x * 4 + 0] = src[x * 4 + 2];
          dst[x * 4 + 1] = src[x * 4 + 1];
          dst[x * 4 + 2] = src[x * 4 + 0];
          dst[x * 4 + 3] = 255;
        }
      }
      ::GlobalUnlock(h);
    }
    g_gfx.ctx->Unmap(staging, 0);
  }
  staging->Release();
  if (!h) return false;
  ok = ::OpenClipboard(ClipboardOwner()) != 0;
  if (ok) {
    ::EmptyClipboard();
    ok = ::SetClipboardData(CF_DIB, h) != nullptr;
    ::CloseClipboard();
  }
  if (!ok) ::GlobalFree(h);
  if (out_w) *out_w = static_cast<int>(cw);
  if (out_h) *out_h = static_cast<int>(ch);
  return ok;
}

// ---- Application -----------------------------------------------------------------
AppState g_app;
HWND ClipboardOwner() { return g_app.hwnd; }
Args g_args;
bool g_in_sizemove = false;
bool g_rendering = false;
int64_t g_frame_count = 0;

double NowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::wstring g_pending_open;  // WM_COPYDATA / drag and drop: open at the start of the next frame (never switch workbooks in the middle of message handling)

void OpenFileRequest(const std::wstring& file) {
  if (!file.empty()) g_pending_open = file;
  if (::IsIconic(g_app.hwnd)) ::ShowWindow(g_app.hwnd, SW_RESTORE);
  ::SetForegroundWindow(g_app.hwnd);
}

// the snapshot's sheet list changed: add views for new sheets, drop deleted ones, fix the active sheet
void SyncWindows(AppState& s) {
  if (!s.snap) return;
  const auto& sheets = s.snap->wb.sheets;
  for (auto it = s.wins.begin(); it != s.wins.end();) {
    it = s.snap->Sheet(it->first) ? std::next(it) : s.wins.erase(it);
  }
  for (const auto& sh : sheets) {
    auto& w = s.Win(sh->id);
    w.view.sheet = sh->id;
  }
  auto valid_docked = [&](uint32_t id) {
    const model::Sheet* sh = s.snap->Sheet(id);
    return sh && sh->visible && !s.Win(id).floating;
  };
  // the active sheet hidden or deleted: the next visible sheet to its right, else to its left (Excel), the same one
  // Python's Delete / Visible pick in their request (the first sheet was taken here, and the two raced)
  static int32_t last_index = 0;
  if (valid_docked(s.active_sheet)) {
    last_index = s.snap->wb.IndexOfId(s.active_sheet);
  } else {
    const int32_t n = static_cast<int32_t>(sheets.size());
    const int32_t at = s.snap->wb.IndexOfId(s.active_sheet);  // -1: deleted, the next one now sits at last_index
    const int32_t right = at >= 0 ? at + 1 : last_index;
    int32_t pick = -1;
    for (int32_t k = std::max(0, right); k < n && pick < 0; ++k) {
      if (valid_docked(sheets[static_cast<size_t>(k)]->id)) pick = k;
    }
    for (int32_t k = std::min(right, n) - 1; k >= 0 && pick < 0; --k) {
      if (valid_docked(sheets[static_cast<size_t>(k)]->id)) pick = k;
    }
    if (pick >= 0) {
      s.active_sheet = sheets[static_cast<size_t>(pick)]->id;
      last_index = pick;
    }
  }
  if (!s.snap->Sheet(s.focus_sheet) || !s.snap->Sheet(s.focus_sheet)->visible) s.focus_sheet = s.active_sheet;
  if (s.edit.active && !s.snap->Sheet(s.edit.sheet)) s.edit.active = false;
}

void Redock(uint32_t id) {
  if (!g_app.snap || !g_app.snap->Sheet(id)) return;
  g_app.Win(id).floating = false;
  g_app.active_sheet = g_app.focus_sheet = id;
  ui::dock::UndockWindow(g_app.SheetWindowName(id).c_str());
  g_app.SetStatus(TR("\"") + g_app.snap->Sheet(id)->name + TR("\" is back in the main window"));
}

bool KeyboardTarget() {
  ImGuiContext& g = *GImGui;
  if (!g.NavWindow) return true;
  const ImGuiWindow* root = g.NavWindow->RootWindowDockTree ? g.NavWindow->RootWindow : g.NavWindow;
  const char* n = root->Name;
  // a figure the user clicked (Python plots) has the keyboard: keys typed there went into the active cell behind it.
  // One that only popped up (a script made it) leaves the keys with the sheet.
  if (std::strstr(n, "###yc_fig")) return root->ID != g_app.figure_keys;
  g_app.figure_keys = 0;  // the keys went elsewhere: the next figure has them only when clicked
  if (std::strstr(n, "Dear ImGui") || std::strstr(n, "ImPlot")) return false;
  // YCGrid's own windows: only those showing a sheet pass what is typed to its cells. A pane, a tool window or Find
  // that was clicked keeps the keyboard, as Excel's task panes do (chaos 101: a click on the Alert Rules pane over
  // the sheet, then typing, filled six million selected cells)
  if (std::strstr(n, "###yc_")) {
    // (a container itself is not one: what it holds has the focus when clicked, and a click on its own free part,
    // then typing, filled ten million selected cells behind it: chaos seed 5501)
    static const char* const kSheets[] = {"###yc_gridhost", "###yc_sheet_", "###yc_xwin", "###yc_range_pick"};
    for (const char* k : kSheets) {
      if (std::strstr(n, k)) return true;
    }
    return false;
  }
  return true;  // the ribbon, the formula bar, the status bar...: typing goes on into the active cell
}

// WEBSERVICE downloads and IMAGE pictures (created in WinMain, live as long as the engine)
std::unique_ptr<app::NetFetcher> g_net;
std::unique_ptr<app::ImageCache> g_images;

// Show Formulas (Ctrl+`) draws the sheet's columns twice as wide, as Excel does: a copy of the snapshot whose sheet
// has its column widths doubled (only for drawing that window; the stored widths don't change), made once per
// snapshot
std::shared_ptr<const engine::Snapshot> FormulaViewSnapshot(const std::shared_ptr<const engine::Snapshot>& src, uint32_t id) {
  struct Cached {
    std::weak_ptr<const engine::Snapshot> from;
    std::shared_ptr<const engine::Snapshot> view;
  };
  static std::map<uint32_t, Cached> cache;
  Cached& c = cache[id];
  if (c.view && c.from.lock() == src) return c.view;
  auto copy = std::make_shared<engine::Snapshot>(*src);
  if (model::Sheet* ms = copy->wb.MutSheetById(id)) {
    model::DimIndex wide = *ms->cols;
    std::vector<model::DimIndex::Entry> entries = wide.Entries();
    for (auto& e : entries) {
      e.size *= 2;
      e.saved *= 2;
    }
    wide.Load(wide.default_size() * 2, entries);
    wide.Finalize();
    ms->cols = core::Cow<model::DimIndex>(std::make_shared<model::DimIndex>(std::move(wide)));
  }
  c.from = src;
  c.view = copy;
  return copy;
}

render::GridResult DrawSheet(uint32_t id, const ImRect& r, render::TabBarIO* tabs, uint32_t win = 0) {
  const model::Sheet* sh = g_app.snap->Sheet(id);
  if (!sh) return {};
  // picking cells for a dialog's range box while a cell is being edited (Insert Function's arguments): the grid
  // leaves the entry alone meanwhile, so a drag selects for the box instead of pointing in the formula and Enter
  // brings the dialog back instead of entering the cell
  struct PauseEdit {
    bool paused = false;
    PauseEdit() {
      if (app::RangePicking() && g_app.edit.active) paused = true, g_app.edit.active = false;
    }
    ~PauseEdit() {
      if (paused) g_app.edit.active = true;
    }
  } pause_edit;
  render::GridView* wv = win ? g_app.ExtraView(win) : &g_app.Win(id).view;
  if (!wv) return {};
  // Show Formulas: this window draws (and hit-tests, with its overlays) the sheet with columns twice as wide
  struct SnapSwap {
    std::shared_ptr<const engine::Snapshot> saved;
    ~SnapSwap() {
      if (saved) g_app.snap = saved;
    }
  } swap;
  const bool wide_cols = g_app.Win(id).formulas && !sh->chart_sheet;
  if (wide_cols) {
    swap.saved = g_app.snap;
    g_app.snap = FormulaViewSnapshot(swap.saved, id);
    sh = g_app.snap->Sheet(id);
    if (!sh) return {};
  }
  render::GridOptions opt;
  opt.view_id = win;
  opt.host_away_edit = win == 0 && tabs != nullptr && id == g_app.active_sheet;
  const app::SheetWin& sw = g_app.Win(id);
  opt.show_gridlines = sw.gridlines;
  opt.show_headings = sw.headings && wv->page_view != 2;
  opt.show_formulas = sw.formulas;  // this sheet's window only (Excel's Window.DisplayFormulas)
  opt.show_outline = sw.outline;    // Ctrl+8 (Window.DisplayOutline)
  opt.show_zeros = sw.zeros;
  opt.gridline_color = sw.gridline_color;
  opt.show_vscroll = g_app.show_vscroll;
  opt.show_hscroll = g_app.show_hscroll;
  if (!g_app.show_sheet_tabs) tabs = nullptr;  // Options: Show sheet tabs off (Window.DisplayWorkbookTabs)
  opt.scroll_lock = app::ScrollLockOn();
  // the fill handle's tip: the value its drag gives the last cell (Excel)
  opt.fill_tip = [id](const Rect& src, const Rect& dest, bool ctrl) { return app::FillTipFor(g_app, id, src, dest, ctrl); };
  static engine::ErrorCheckOptions eco;  // File > Options > Formulas: background error checking
  eco = app::ErrorCheckingOptions();
  opt.error_check = &eco;
  opt.r1c1 = app::Config().r1c1;
  {  // File > Options: After pressing Enter, move selection (down, right, up, left)
    static const int kEnterDir[4] = {1, 3, 2, 4};
    opt.enter_dir = app::Config().move_after_return ? kEnterDir[std::clamp(app::Config().move_after_return_dir, 0, 3)] : 0;
  }
  opt.keyboard = g_app.focus_sheet == id && g_app.focus_win == win && KeyboardTarget() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
                 ui::ribbon::St().keytip_level == 0;
  if (g_app.marquee) {
    opt.copy_marquee = &g_app.marquee_rect;
    opt.copy_sheet = g_app.marquee_sheet;
  } else if (g_app.format_painter) {
    // Format Painter's source cells get the marching ants, as in Excel
    opt.copy_marquee = &g_app.painter_rect;
    opt.copy_sheet = g_app.painter_sheet;
    opt.marquee_no_paste = true;
  }
  if (app::ObjectsOwnKeyboard(g_app, id)) opt.keyboard = false;  // an object is selected: arrow keys and Delete go to the object
  if (app::NoteEditing()) opt.keyboard = false;  // typing a note beside its cell
  // picking cells for a dialog's range box: Enter and Escape bring the dialog back instead of moving the cell cursor
  if (app::RangePicking() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Escape)))
    opt.keyboard = false;
  opt.design_mode = g_app.design_mode;
  // a protected sheet offers no drags it would refuse (like Excel), and neither does read-only mode (no "There's already
  // data here" question for a move it then refuses); Application.CellDragAndDrop turns drag and drop off
  const model::SheetProtection& prot = *sh->protection;
  opt.cell_drag_drop = app::Config().cell_drag_and_drop && !prot.on && g_app.operator_mode;
  opt.resize_cols = !prot.on || prot.format_columns;
  opt.resize_rows = !prot.on || prot.format_rows;
  if (prot.on) {
    // Protect Sheet's "Select locked cells" / "Select unlocked cells", and Tab between the unlocked cells (Excel)
    const model::StyleTable* styles = &g_app.snap->wb.styles.get();
    const bool sel_locked = prot.select_locked && prot.select_unlocked, sel_unlocked = prot.select_unlocked;
    opt.unlocked = [sh, styles](uint32_t r, uint32_t c) { return !styles->Get(sh->GetStyle(r, c)).locked; };
    if (!sel_locked) {
      opt.selectable = [sh, styles, sel_unlocked](uint32_t r, uint32_t c) { return sel_unlocked && !styles->Get(sh->GetStyle(r, c)).locked; };
    }
  }
  if (sh->chart_sheet) {
    opt.chart_sheet = true;
    opt.keyboard = false;
    opt.show_headings = false;
  }
  opt.image_for = [](const std::string& url, ImVec2* size) { return g_images ? g_images->Get(url, size) : ImTextureID{}; };
  opt.point_text = [](uint32_t sheet, uint32_t r, uint32_t c) { return app::GetPivotDataFor(g_app, sheet, r, c); };
  opt.background = app::SheetBackgroundTexture(g_app, id, &opt.background_size);  // Page Layout > Background
  opt.overlay = [id](const render::OverlayGeom& g) {
    app::DrawFocusCell(g_app, id, g);
    app::DrawRuleOverlays(g_app, id, g);
    app::DrawCellTips(g_app, id, g);
    app::DrawAuditOverlay(g_app, id, g);
    app::DrawErrorCheckButton(g_app, id, g);
    app::DrawTableOverlay(g_app, id, g);
    app::DrawPageBreakOverlay(g_app, id, g);
    app::DrawStaleOverlay(g_app, id, g);
    app::DrawPivotOverlay(g_app, id, g);
    app::DrawFillOptions(g_app, id, g);
    app::DrawPasteOptions(g_app, id, g);
    app::DrawInsertOptions(g_app, id, g);
    app::DrawQuickAnalysis(g_app, id, g);
    app::DrawSafetyOverlay(g_app, id, g);
    app::DrawTradingOverlay(g_app, id, g);
    if (!g_app.hide_objects) {  // Ctrl+6 hides them (Workbook.DisplayDrawingObjects = xlHide)
      app::DrawSheetObjects(g_app, id, g);
      app::DrawSlicers(g_app, id, g);
    }
    app::DrawSelectObjectsOverlay(g_app, id, g);  // Find & Select > Select Objects
    app::DrawPageViews(g_app, id, g);  // Page Layout covers everything else
  };
  auto& view = *wv;
  // live preview like Excel: hovering a Cell Styles tile shows it on the selection without changing the workbook
  if (const std::string& tile = ui::ribbon::St().hovered_tile; !tile.empty() && id == g_app.focus_sheet && !g_app.edit.active) {
    opt.preview_style = app::CellStylePreview(tile);
    if (opt.preview_style) opt.preview_areas = &view.sel.areas;
  }
  if (id == g_app.focus_sheet && !g_app.edit.active) {
    opt.preview_rule = app::CfPreviewRule(g_app, ui::ribbon::St().hovered_tile);
    opt.preview_table = app::TableStylePreview(g_app, ui::ribbon::St().hovered_tile);
  }
  if (!g_app.interactive) opt.keyboard = false;
  // Worksheet.ScrollArea: the selection and the scroll position stay inside the range
  auto clamp_to_area = [&]() {
    auto it = g_app.scroll_areas.find(id);
    if (it == g_app.scroll_areas.end()) return;
    const Rect a = it->second;
    auto& sel = view.sel;
    sel.act_r = std::clamp(sel.act_r, a.r0, a.r1);
    sel.act_c = std::clamp(sel.act_c, a.c0, a.c1);
    sel.anc_r = std::clamp(sel.anc_r, a.r0, a.r1);
    sel.anc_c = std::clamp(sel.anc_c, a.c0, a.c1);
    for (auto& ar : sel.areas) {
      ar = Rect{std::clamp(ar.r0, a.r0, a.r1), std::clamp(ar.c0, a.c0, a.c1), std::clamp(ar.r1, a.r0, a.r1), std::clamp(ar.c1, a.c0, a.c1)};
    }
    view.v.first = std::clamp<int64_t>(view.v.first, std::max<int64_t>(a.r0, view.freeze_rows ? view.frozen_top + view.freeze_rows : 0), a.r1);
    view.h.first = std::clamp<int64_t>(view.h.first, std::max<int64_t>(a.c0, view.freeze_cols ? view.frozen_left + view.freeze_cols : 0), a.c1);
  };
  clamp_to_area();
  render::GridResult res = render::DrawGrid(*g_app.snap, *sh, view, g_app.edit, r, opt, tabs);
  clamp_to_area();
  if (wide_cols && res.act.resize_kind == 1) res.act.resize_px = std::max(0, res.act.resize_px / 2);  // the stored width
  if (res.act.clicked) {
    app::ClearObjectSelection();
    g_app.focus_win = win;
  }
  app::HandleGridActions(g_app, id, res.act);
  // Worksheet_SelectionChange: TrackSheetEvents (end of the frame) sees every change, whatever made it
  return res;
}

// ---- Formula bar ----
void DrawFormulaBar(const ImRect& r) {
  using ui::S;
  const auto& t = ui::T();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(r.Min, r.Max, t.ribbon_bg);
  ImGui::PushFont(ui::F().ui, ui::F().ui_size + 0.5f);
  // Name Box (type an address or a name and press Enter to go there); its width is dragged at the handle on its
  // right, as in Excel
  static float nb_width = 134.0f;  // base px
  const ImRect nb(ImVec2(r.Min.x + S(6), r.Min.y + S(4)), ImVec2(r.Min.x + S(6) + S(nb_width), r.Max.y - S(4)));
  const float nb_arrow = S(18);
  static char name_buf[256];
  const ImGuiID nb_id = ImGui::GetID("##namebox");
  if (ImGui::GetActiveID() != nb_id) {
    std::string addr = g_app.edit.active ? core::CellA1(g_app.edit.r, g_app.edit.c) : app::SelectionAddress(g_app);
    if (app::Config().r1c1) {
      const auto& as = g_app.FocusView().sel;
      addr = g_app.edit.active ? formula::RangeR1C1(Rect::Cell(g_app.edit.r, g_app.edit.c), nullptr) : formula::RangeR1C1(Rect::Cell(as.act_r, as.act_c), nullptr);
    }
    const auto& sel = g_app.FocusView().sel;
    if (g_app.FocusView().dragging && !sel.Active().IsCell()) {
      addr = std::to_string(sel.Active().Rows()) + "R x " + std::to_string(sel.Active().Cols()) + "C";
    }
    if (!g_app.edit.active && !g_app.FocusView().dragging) {
      const std::string nm = app::NameForSelection(g_app);  // the selection is exactly a defined name: show the name (like Excel)
      if (!nm.empty()) addr = nm;
      app::SeriesNameBoxText(g_app, &addr);  // a selected chart series: Series "Sales"
    }
    // typing a formula: the Name Box becomes the function list, showing the function used last (Excel)
    if (g_app.edit.active && formula::LooksLikeFormula(g_app.edit.text)) addr = app::RecentFunctions().empty() ? std::string("SUM") : app::RecentFunctions().front();
    std::snprintf(name_buf, sizeof(name_buf), "%s", addr.c_str());
  }
  ImGui::SetCursorScreenPos(nb.Min);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(6), (nb.GetHeight() - ImGui::GetFontSize()) * 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
  ImGui::PushStyleColor(ImGuiCol_Border, t.formula_border);
  ImGui::SetNextItemWidth(nb.GetWidth() - nb_arrow);
  if (ImGui::InputText("##namebox", name_buf, sizeof(name_buf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
    // address / name -> go there; a new valid name -> define it for the selection (like Excel)
    app::NameBoxEnter(g_app, name_buf);
    ImGui::SetWindowFocus(nullptr);
  }
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
  {
    // drop-down arrow of the name list
    const ImRect ar(ImVec2(nb.Max.x - nb_arrow, nb.Min.y), nb.Max);
    ImGui::SetCursorScreenPos(ar.Min);
    const bool clicked = ImGui::InvisibleButton("##namebox_dd", ar.GetSize());
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(ar.Min, ar.Max, hov ? t.btn_hover : IM_COL32_WHITE);
    dl->AddRect(ImVec2(ar.Min.x - 1, ar.Min.y), ar.Max, t.formula_border);
    const ImVec2 c = ar.GetCenter();
    const float a = S(3.5f);
    dl->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.5f), ImVec2(c.x + a, c.y - a * 0.5f), ImVec2(c.x, c.y + a * 0.6f), t.btn_text);
    if (clicked) ImGui::OpenPopup("##yc_namebox_list");
    ImGui::SetNextWindowPos(ImVec2(nb.Min.x, nb.Max.y + 1));
    ImGui::SetNextWindowSizeConstraints(ImVec2(nb.GetWidth(), 0), ImVec2(S(320), S(360)));
    if (ImGui::BeginPopup("##yc_namebox_list")) {
      if (g_app.edit.active && formula::LooksLikeFormula(g_app.edit.text)) {
        // the functions used last, and More Functions... (Insert Function); one opens its Function Arguments
        static const std::vector<std::string> kStart = {"SUM", "AVERAGE", "IF", "HYPERLINK", "COUNT", "MAX", "SIN", "SUMIF", "PMT", "STDEV"};
        for (const std::string& fn : app::RecentFunctions().empty() ? kStart : app::RecentFunctions()) {
          if (ImGui::Selectable(fn.c_str())) app::OpenFunctionArgs(g_app, fn, {}, false);
        }
        ImGui::Separator();
        if (ImGui::Selectable(TR("More Functions..."))) g_app.OpenDialog(app::Dialog::InsertFunction);
      } else {
        app::DrawNameBoxList(g_app);
      }
      ImGui::EndPopup();
    }
  }
  // the handle between the Name Box and the bar (Excel's three dots): drag it to widen the Name Box
  {
    const ImRect hr(ImVec2(nb.Max.x + S(2), r.Min.y + S(4)), ImVec2(nb.Max.x + S(10), r.Min.y + S(26)));
    ImGui::SetCursorScreenPos(hr.Min);
    ImGui::InvisibleButton("##nb_resize", hr.GetSize());
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) nb_width = std::clamp(nb_width + ImGui::GetIO().MouseDelta.x / std::max(0.01f, S(1)), 60.0f, 600.0f);
    const ImVec2 hc = hr.GetCenter();
    for (int k = -1; k <= 1; ++k) dl->AddCircleFilled(ImVec2(hc.x, hc.y + static_cast<float>(k) * S(4)), std::max(1.0f, S(1)), t.btn_text_disabled);
  }
  // ✕ ✓ fx: Cancel, Enter, Insert Function (their tooltips, as Excel's)
  float x = nb.Max.x + S(18);
  const char* glyphs[] = {ui::icon::Dismiss, ui::icon::Check, ui::icon::Formula};
  const char* const fb_ids[] = {"##fb_cancel", "##fb_enter", "##fb_fx"};  // no label: "Cancel" is a dialog's button for tests
  const char* const fb_tips[] = {TR("Cancel"), TR("Enter"), TR("Insert Function")};
  for (int k = 0; k < 3; ++k) {
    ImFont* f = ui::F().icons;
    const float px = ui::FontPx(16.0f);
    const ImRect br(ImVec2(x - S(4), r.Min.y + S(4)), ImVec2(x + S(20), r.Max.y - S(4)));
    ImGui::SetCursorScreenPos(br.Min);
    ImGui::PushID(k);
    const bool enabled = k == 2 || g_app.edit.active;
    const bool clicked = ImGui::InvisibleButton(fb_ids[k], br.GetSize()) && enabled;
    if (ImGui::IsItemHovered() && enabled) dl->AddRectFilled(br.Min, br.Max, t.btn_hover, S(2));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", fb_tips[k]);
    ImGui::PopID();
    const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, glyphs[k]);
    dl->AddText(f, px, ImVec2(x, r.GetCenter().y - sz.y * 0.5f), enabled ? t.btn_text : t.btn_text_disabled, glyphs[k]);
    if (clicked) {
      if (k == 0) app::CancelEdit(g_app);
      else if (k == 1) app::CommitEdit(g_app, false, 0);
      else ui::Commands().Execute("InsertFunction");
    }
    x += S(26);
  }
  // the expand / collapse chevron at the right end (Ctrl+Shift+U), as in Excel
  const int fb_lines = app::Config().formula_bar_lines;
  {
    const ImRect chev(ImVec2(r.Max.x - S(26), r.Min.y + S(4)), ImVec2(r.Max.x - S(6), r.Min.y + S(26)));
    ImGui::SetCursorScreenPos(chev.Min);
    const bool clicked = ImGui::InvisibleButton(fb_lines > 1 ? "Collapse Formula Bar##fbar_expand" : "Expand Formula Bar##fbar_expand", chev.GetSize());
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(chev.Min, chev.Max, t.btn_hover, S(2));
      ImGui::SetTooltip("%s", fb_lines > 1 ? TR("Collapse Formula Bar (Ctrl+Shift+U)") : TR("Expand Formula Bar (Ctrl+Shift+U)"));
    }
    const ImVec2 c = chev.GetCenter();
    const float a = S(4);
    if (fb_lines > 1) dl->AddTriangleFilled(ImVec2(c.x - a, c.y + a * 0.5f), ImVec2(c.x + a, c.y + a * 0.5f), ImVec2(c.x, c.y - a * 0.6f), t.btn_text);
    else dl->AddTriangleFilled(ImVec2(c.x - a, c.y - a * 0.5f), ImVec2(c.x + a, c.y - a * 0.5f), ImVec2(c.x, c.y + a * 0.6f), t.btn_text);
    if (clicked) ui::Commands().Execute("ExpandFormulaBar");
    // its bottom edge: drag to show more or fewer lines
    const ImRect edge(ImVec2(r.Min.x, r.Max.y - S(4)), r.Max);
    ImGui::SetCursorScreenPos(edge.Min);
    ImGui::InvisibleButton("##fbar_resize", edge.GetSize());
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) {
      const float extra = ImGui::GetIO().MousePos.y - (r.Min.y + S(30));
      app::Config().formula_bar_lines = std::clamp(1 + static_cast<int>(std::lround(extra / S(17))), 1, 20);
    }
    if (ImGui::IsItemDeactivated()) app::SaveSettings();
  }
  // contents
  const ImRect fb(ImVec2(x + S(4), r.Min.y + S(4)), ImVec2(r.Max.x - S(30), r.Max.y - S(4)));
  dl->AddRectFilled(fb.Min, fb.Max, IM_COL32_WHITE);
  dl->AddRect(fb.Min, fb.Max, t.formula_border);
  auto& e = g_app.edit;
  if (!e.active && app::DrawSeriesFormulaBar(g_app, fb)) {
    // a selected chart series: its =SERIES(...) formula, edited here as in Excel
  } else if (e.active) {
    render::GridActions act;
    ImGui::SetCursorScreenPos(ImVec2(fb.Min.x + 1, fb.Min.y + 1));
    // the first line where the collapsed bar has it; more lines below it when the bar is expanded
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(5), (S(22) - 2 - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32_WHITE);
    render::EditField(e, "##fbar", ImVec2(fb.GetWidth() - 2, fb.GetHeight() - 2), act, true);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    app::HandleGridActions(g_app, e.sheet, act);
  } else if (g_app.snap) {
    const auto& sel = g_app.FocusView().sel;
    const bool r1c1 = app::Config().r1c1;
    std::string text = engine::InputTextOf(g_app.snap->wb, g_app.focus_sheet, sel.act_r, sel.act_c, r1c1);
    // a hidden formula (Format Cells -> Protection) on a protected sheet isn't shown
    if (const model::Sheet* hs = g_app.FocusSheet(); hs && hs->protection->on && hs->GetFormula(sel.act_r, sel.act_c) &&
                                                       g_app.snap->wb.styles->Get(hs->GetStyle(sel.act_r, sel.act_c)).hidden) {
      text.clear();
    }
    ImU32 color = t.cell_text;
    // a member of a dynamic array shows its anchor's formula, grayed (like Excel); an array formula shows {=...}
    if (const model::Sheet* sh = g_app.FocusSheet(); sh && !sh->spills->empty()) {
      uint32_t ar = 0, ac = 0;
      if (const model::SpillRange* sp = sh->SpillAt(sel.act_r, sel.act_c, &ar, &ac)) {
        const std::string f = engine::FormulaTextOf(g_app.snap->wb, sh->id, ar, ac, r1c1);
        if (sp->cse) {
          text = "{" + f + "}";
        } else if (ar != sel.act_r || ac != sel.act_c) {
          text = f;
          color = t.btn_text_disabled;
        }
      }
    }
    // a formula with line breaks (Alt+Enter): its lines, as many as the bar shows (collapsed: the first one)
    dl->PushClipRect(fb.Min, fb.Max, true);
    const float ly0 = fb.Min.y + (S(22) - ImGui::GetFontSize()) * 0.5f;
    std::vector<std::pair<size_t, size_t>> lines;  // [begin, end) of each line drawn
    float ly = ly0;
    for (size_t p = 0; p <= text.size() && static_cast<int>(lines.size()) < fb_lines;) {
      size_t q = text.find('\n', p);
      if (q == std::string::npos) q = text.size();
      dl->AddText(ImVec2(fb.Min.x + S(6), ly), color, text.c_str() + p, text.c_str() + q);
      lines.emplace_back(p, q);
      ly += ImGui::GetFontSize();
      p = q + 1;
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(fb.Min);
    if (ImGui::InvisibleButton("##fbar_click", fb.GetSize())) {
      // Excel: the caret goes where the click was (the character nearest to it), not to the end
      const ImVec2 m = ImGui::GetIO().MousePos;
      int caret = -1;
      if (!lines.empty()) {
        const size_t li = static_cast<size_t>(std::clamp(static_cast<int>((m.y - ly0) / ImGui::GetFontSize()), 0, static_cast<int>(lines.size()) - 1));
        const auto [lb, le] = lines[li];
        size_t k = lb;
        float cx = fb.Min.x + S(6);
        while (k < le) {
          size_t n = 1;
          while (k + n < le && (static_cast<unsigned char>(text[k + n]) & 0xC0) == 0x80) ++n;
          const float w = ImGui::CalcTextSize(text.c_str() + k, text.c_str() + k + n).x;
          if (m.x < cx + w * 0.5f) break;
          cx += w;
          k += n;
        }
        caret = static_cast<int>(k);
      }
      app::StartEdit(g_app, false, {}, true);
      // the text edited is the one shown (not an array's braces or a spill member's grayed anchor formula)
      if (caret >= 0 && g_app.edit.active && g_app.edit.text == text) {
        g_app.edit.cursor_to_end = false;
        g_app.edit.pending_caret = caret;
        g_app.edit.caret = caret;
      }
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  }
  ImGui::PopFont();
}

// ---- Status bar ----
struct StatsCache {
  uint64_t version = 0;
  std::vector<Rect> areas;
  uint32_t sheet = 0;
  engine::RangeStats st;
  uint64_t cells = 0;
  double at = 0;  // when it was worked out (ImGui time)
  double cost = 0;  // how long that took (s): a big selection waits a multiple of it before the next one
} g_stats;

void DrawStatusBar(const ImRect& r, double last_frame_ms) {
  using ui::S;
  const auto& t = ui::T();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(r.Min, r.Max, t.statusbar_bg);
  dl->AddRectFilled(r.Min, ImVec2(r.Max.x, r.Min.y + 1), t.ribbon_border);
  ImGui::PushFont(ui::F().ui, ui::F().ui_size);
  const float ty = r.GetCenter().y - ImGui::GetFontSize() * 0.5f;
  const app::Settings& sb = app::Config();  // the entries chosen on the status bar's right-click menu
  const auto& e = g_app.edit;
  const char* mode = TR("Ready");
  if (e.active) mode = (!e.text.empty() && e.text[0] == '=' && e.ref_begin >= 0) ? TR("Point") : (e.enter_mode ? TR("Enter") : TR("Edit"));
  float left = r.Min.x + S(10);
  if (sb.status_cell_mode) {
    dl->AddText(ImVec2(left, ty), t.statusbar_text, mode);
    left = r.Min.x + S(60);
  }
  {
    // Excel's keyboard modes (F8, Shift+F8, End)
    const auto& fv = g_app.FocusView();
    const char* km = fv.extend_mode && sb.status_selection_mode  ? TR("Extend Selection")
                     : fv.add_mode && sb.status_selection_mode ? TR("Add to Selection")
                     : fv.end_mode && sb.status_end_mode       ? TR("End Mode")
                                                               : nullptr;
    if (km) {
      dl->AddText(ImVec2(left, ty), t.statusbar_text, km);
      left += ImGui::CalcTextSize(km).x + S(16);
    }
    // Caps Lock / Num Lock while on (Excel)
    for (const auto& [on, label] : {std::pair<bool, const char*>{sb.status_caps_lock && (::GetKeyState(VK_CAPITAL) & 1), TR("Caps Lock")},
                                    std::pair<bool, const char*>{sb.status_num_lock && (::GetKeyState(VK_NUMLOCK) & 1), TR("Num Lock")}}) {
      if (!on) continue;
      dl->AddText(ImVec2(left, ty), t.statusbar_text, label);
      left += ImGui::CalcTextSize(label).x + S(16);
    }
    if (app::ScrollLockOn() && sb.status_scroll_lock) {  // the arrows scroll the window (Excel shows it here)
      const char* sl = TR("Scroll Lock");
      dl->AddText(ImVec2(left, ty), t.statusbar_text, sl);
      ui::NoteUiText(sl, ImRect(ImVec2(left, r.Min.y), ImVec2(left + ImGui::CalcTextSize(sl).x, r.Max.y)));
      left += ImGui::CalcTextSize(sl).x + S(16);
    }
  }
  if (g_app.snap && g_app.snap->manual_calc && g_app.snap->calculating) {  // Excel: formulas wait for F9
    const char* calc = TR("Calculate");
    dl->AddText(ImVec2(left, ty), t.statusbar_text, calc);
    left += ImGui::CalcTextSize(calc).x + S(16);
  }
  if (g_app.snap && g_app.snap->circular) {
    const std::string msg = app::CircularStatusText(g_app);
    dl->AddText(ImVec2(left, ty), t.statusbar_text, msg.c_str());
    left += ImGui::CalcTextSize(msg.c_str()).x + S(16);
  }
  left = app::DrawSafetyStatus(g_app, dl, r, left, ty);
  left = app::DrawReplayStatus(g_app, dl, r, left, ty);
  if (sb.status_error_log) {
    // the Error Log's entry, always there so people find the window: red with the count while errors are unseen,
    // the number of new messages beside its name otherwise (what an Excel workbook didn't bring along, say)
    // Drawn like Excel's own status bar entries: plain text, a light highlight under the mouse, and a small round
    // badge with the count after it (red for errors, the accent color for messages) only while there is something new.
    const int ne = app::ErrorsUnseen(), nm = app::MessagesUnseen();
    const int count = ne > 0 ? ne : nm;
    const char* label = TR("Error Log");
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const std::string num = count > 99 ? std::string("99+") : std::to_string(count);
    const float badge_h = ImGui::GetFontSize() + S(2);
    const float badge_w = count > 0 ? std::max(badge_h, ImGui::CalcTextSize(num.c_str()).x + S(8)) : 0.0f;
    const float pad = S(6);
    const ImRect cr(ImVec2(left - pad, r.Min.y + S(2)), ImVec2(left + ts.x + (count > 0 ? S(5) + badge_w : 0.0f) + pad, r.Max.y - S(2)));
    const bool hot = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(cr.Min, cr.Max);
    if (hot) dl->AddRectFilled(cr.Min, cr.Max, t.btn_hover, S(2));
    dl->AddText(ImVec2(left, ty), t.statusbar_text, label);
    if (count > 0) {
      const float bx = left + ts.x + S(5);
      const float by = r.GetCenter().y - badge_h * 0.5f;
      const ImRect br(ImVec2(bx, by), ImVec2(bx + badge_w, by + badge_h));
      dl->AddRectFilled(br.Min, br.Max, ne > 0 ? IM_COL32(0xC6, 0x28, 0x28, 255) : t.accent, badge_h * 0.5f);
      const ImVec2 ns = ImGui::CalcTextSize(num.c_str());
      dl->AddText(ImVec2(br.GetCenter().x - ns.x * 0.5f, br.GetCenter().y - ns.y * 0.5f), IM_COL32_WHITE, num.c_str());
    }
    ui::NoteUiItem("Error Log##status", cr);
    if (hot) {
      std::string tip = TR("Every error and message YCGrid showed or received, Python included (View > Error Log)");
      if (ne > 0) tip += std::string("\n") + std::to_string(ne) + (ne == 1 ? TR(" error") : TR(" errors")) + TR(" not seen yet");
      else if (nm > 0) tip += std::string("\n") + std::to_string(nm) + (nm == 1 ? TR(" message") : TR(" messages")) + TR(" not seen yet");
      ImGui::SetTooltip("%s", tip.c_str());
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app::OpenErrorWindow();
    }
    left = cr.Max.x + S(10);
  }
  // a refusal said here in red instead of a message box (a protected sheet): a click opens the Error Log, which has it
  if (!g_app.refusal.empty() && ImGui::GetTime() < g_app.refusal_until) {
    const char* msg = g_app.refusal.c_str();
    const ImRect mr(ImVec2(left, r.Min.y), ImVec2(left + ImGui::CalcTextSize(msg).x, r.Max.y));
    dl->AddText(ImVec2(left, ty), IM_COL32(0xC6, 0x28, 0x28, 255), msg);
    ui::NoteUiText(msg, mr);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(mr.Min, mr.Max)) {
      ImGui::SetTooltip("%s", TR("Open the Error Log for the whole message"));
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) app::OpenErrorWindow();
    }
    left = mr.Max.x + S(16);
  }
  if (g_app.format_painter) {
    dl->AddText(ImVec2(left, ty), t.statusbar_text, TR("Format Painter: select the destination"));
    left += S(160);
  }
  if (const char* pen = app::BorderDrawStatus()) {
    dl->AddText(ImVec2(left, ty), t.statusbar_text, pen);
    left += ImGui::CalcTextSize(pen).x + S(16);
  }
  if (!g_app.python_status.empty()) {
    dl->AddText(ImVec2(left, ty), t.statusbar_text, g_app.python_status.c_str());
    left += ImGui::CalcTextSize(g_app.python_status.c_str()).x + S(16);
  }
  // the zoom slider at the right end, as in Excel: - [----|----] + 100% (100% in the middle; 10% to 400%)
  float zoom_left = r.Max.x - S(12);
  if (sb.status_zoom || sb.status_zoom_slider) {
    auto& view = g_app.FocusView();
    auto to_t = [](float z) { return z <= 1.0f ? (z - 0.1f) / 0.9f * 0.5f : 0.5f + (z - 1.0f) / 3.0f * 0.5f; };
    auto to_z = [](float t) { return t <= 0.5f ? 0.1f + t / 0.5f * 0.9f : 1.0f + (t - 0.5f) / 0.5f * 3.0f; };
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(view.zoom * 100.0f + 0.5f));
    const float pw = ImGui::CalcTextSize("400%").x;
    ImVec2 pct_min(r.Max.x - S(4), r.Min.y);
    if (sb.status_zoom) {
      pct_min = ImVec2(r.Max.x - S(10) - pw, r.Min.y);
      const ImVec2 pct_max(r.Max.x - S(4), r.Max.y);
      dl->AddText(ImVec2(pct_max.x - S(6) - ImGui::CalcTextSize(pct).x, ty), t.statusbar_text, pct);
      ImGui::SetCursorScreenPos(pct_min);
      if (ImGui::InvisibleButton("##zoom_pct", pct_max - pct_min)) ui::Commands().Execute("ZoomDialog");  // the percentage opens Zoom
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Zoom level"));
      zoom_left = pct_min.x - S(12);
    }
    const float bw = S(16), track = S(100);
    const float plus_x = pct_min.x - S(4) - bw, track_x1 = plus_x - S(4), track_x0 = track_x1 - track, minus_x = track_x0 - S(4) - bw;
    const float cy = r.GetCenter().y;
    auto step_button = [&](const char* id, float x, bool plus) {
      ImGui::SetCursorScreenPos(ImVec2(x, r.Min.y + S(2)));
      const bool clicked = ImGui::InvisibleButton(id, ImVec2(bw, r.GetHeight() - S(4)));
      const ImU32 col = ImGui::IsItemHovered() ? t.statusbar_text : (t.statusbar_text & 0x00FFFFFF) | 0xB0000000;
      dl->AddLine(ImVec2(x + S(4), cy), ImVec2(x + bw - S(4), cy), col, S(1.5f));
      if (plus) dl->AddLine(ImVec2(x + bw * 0.5f, cy - S(4)), ImVec2(x + bw * 0.5f, cy + S(4)), col, S(1.5f));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", plus ? TR("Zoom In") : TR("Zoom Out"));
      if (clicked) view.zoom = std::clamp(std::round((view.zoom + (plus ? 0.1f : -0.1f)) * 10.0f) / 10.0f, 0.1f, 4.0f);
    };
    if (sb.status_zoom_slider) {
    step_button("##zoom_out", minus_x, false);
    step_button("##zoom_in", plus_x, true);
    ImGui::SetCursorScreenPos(ImVec2(track_x0 - S(4), r.Min.y + S(2)));
    ImGui::InvisibleButton("##zoom_slider", ImVec2(track + S(8), r.GetHeight() - S(4)));
    if (ImGui::IsItemActive()) {
      const float tt = std::clamp((ImGui::GetIO().MousePos.x - track_x0) / track, 0.0f, 1.0f);
      float z = to_z(tt);
      if (std::fabs(tt - 0.5f) < 0.03f) z = 1.0f;  // snaps to 100% at the middle mark
      view.zoom = std::clamp(std::round(z * 100.0f) / 100.0f, 0.1f, 4.0f);
    }
    const ImU32 line = (t.statusbar_text & 0x00FFFFFF) | 0x90000000;
    dl->AddLine(ImVec2(track_x0, cy), ImVec2(track_x1, cy), line, S(1));
    dl->AddLine(ImVec2(track_x0 + track * 0.5f, cy - S(4)), ImVec2(track_x0 + track * 0.5f, cy + S(4)), line, S(1));
    const float thumb = track_x0 + track * std::clamp(to_t(view.zoom), 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(thumb - S(2), cy - S(6)), ImVec2(thumb + S(2), cy + S(6)), t.statusbar_text);
    zoom_left = minus_x - S(12);
    }
  }
  // Normal / Page Layout / Page Break Preview, left of the zoom slider (Excel)
  if (sb.status_view_shortcuts) zoom_left = app::DrawStatusViewButtons(g_app, r, zoom_left);
  // selection statistics: the ones chosen on the status bar's right-click menu (Excel: Average, Count, Sum)
  std::string right;
  std::string stat_text[6];  // the values shown in the menu, as Excel does
  const model::Sheet* sh = g_app.FocusSheet();
  if (sh && g_app.snap) {
    const auto areas = app::SelAreas(g_app);
    bool multi = areas.size() > 1 || (!areas.empty() && !areas[0].IsCell());
    if (multi) {
      // a new snapshot (live data, a recalculation in slices) with the same cells selected: a selection of 200,000
      // filled cells or more is gathered again once a second, not every frame (Sum of a whole column under live data
      // took the frame's time)
      const bool same_cells = g_stats.sheet == g_app.focus_sheet && g_stats.areas.size() == areas.size() &&
                              std::equal(areas.begin(), areas.end(), g_stats.areas.begin());
      const bool big = g_stats.st.count >= 200000;
      const bool stale = g_stats.version != g_app.snap->version;
      // a gathering that takes longer than its second (millions of filled cells, a debug build) waits four times its own
      // cost: once a second, the frames did nothing else (chaos seed 6089, the UI stuck on B10:J1048576's statistics)
      const double wait = std::max(1.0, 4.0 * g_stats.cost);
      if (!same_cells || (stale && (!big || ImGui::GetTime() - g_stats.at >= wait))) {
        const auto t0 = std::chrono::steady_clock::now();
        g_stats.at = ImGui::GetTime();
        g_stats.version = g_app.snap->version;
        g_stats.sheet = g_app.focus_sheet;
        g_stats.areas = areas;
        g_stats.st = {};
        for (const Rect& a : areas) {
          const auto st = engine::ComputeStats(*sh, a, true);  // the cells shown: hidden and filtered rows don't count
          if (g_stats.st.numbers == 0) {
            g_stats.st.min = st.min;
            g_stats.st.max = st.max;
          } else if (st.numbers) {
            g_stats.st.min = std::min(g_stats.st.min, st.min);
            g_stats.st.max = std::max(g_stats.st.max, st.max);
          }
          g_stats.st.count += st.count;
          g_stats.st.numbers += st.numbers;
          g_stats.st.sum += st.sum;
        }
        g_stats.cost = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      } else if (stale) {
        ui::RequestRedrawIn(std::max(0.05, wait - (ImGui::GetTime() - g_stats.at)));  // the newer values once the wait is up
      }
      const auto& st = g_stats.st;
      if (st.numbers > 0) {
        // like Excel: in the number format of the selection's first number (prices $1,234.50, times 12:30:00)
        std::string fmt;
        uint64_t looked = 0;
        for (const Rect& a : areas) {
          for (uint32_t rr = a.r0; fmt.empty() && rr <= a.r1 && looked < 20000; ++rr) {
            for (uint32_t cc = a.c0; cc <= a.c1 && looked < 20000; ++cc, ++looked) {
              if (!sh->GetValue(rr, cc).IsNumber()) continue;
              fmt = g_app.snap->wb.styles->NumberFormat(g_app.snap->wb.styles->Get(sh->GetStyle(rr, cc)));
              break;
            }
          }
          if (!fmt.empty()) break;
        }
        auto show = [&](double x) {
          if (fmt.empty() || fmt == "General") return formula::FormatGeneral(x, 15);
          return engine::DisplayTextOf(core::Value::Number(x), fmt);
        };
        stat_text[0] = show(st.sum / static_cast<double>(st.numbers));
        stat_text[3] = show(st.min);
        stat_text[4] = show(st.max);
        stat_text[5] = show(st.sum);
      }
      if (st.count > 1 || st.numbers > 0) stat_text[1] = std::to_string(st.count);
      if (st.numbers > 0) stat_text[2] = std::to_string(st.numbers);
    }
  }
  const app::Settings& cfg = app::Config();
  const bool shown[6] = {cfg.status_average, cfg.status_count, cfg.status_numerical_count, cfg.status_min, cfg.status_max, cfg.status_sum};
  const char* stat_names[6] = {TR("Average"), TR("Count"), TR("Numerical Count"), TR("Min"), TR("Max"), TR("Sum")};
  for (int i = 0; i < 6; ++i) {
    if (shown[i] && !stat_text[i].empty()) right += std::string(stat_names[i]) + ": " + stat_text[i] + "    ";
  }
  g_app.status_stats = right;
  if (g_app.python) {
    const std::string ps = g_app.python->StatusText();
    if (!ps.empty()) right += ps + "    ";
  }
  if (g_app.show_perf) {
    char tail[256];
    std::snprintf(tail, sizeof(tail), TR("Visible cells %lld | frame %.2f ms | grid %.2f ms | recalc %.2f ms (%u cells)"), static_cast<long long>(g_app.visible_cells),
                  last_frame_ms, g_app.grid_ms, g_app.snap ? g_app.snap->stats.ms : 0.0, g_app.snap ? g_app.snap->stats.evaluated : 0u);
    right += tail;
  }
  const float rw = ImGui::CalcTextSize(right.c_str()).x;
  const float right_x = zoom_left - rw;
  dl->AddText(ImVec2(right_x, ty), t.statusbar_text, right.c_str());
  // a click on a statistic copies its value, as Excel's (Sum: 18 puts 18 on the clipboard); each is listed for tests
  {
    std::string before;
    for (int i = 0; i < 6; ++i) {
      if (!shown[i] || stat_text[i].empty()) continue;
      const std::string item = std::string(stat_names[i]) + ": " + stat_text[i];
      const float x0 = right_x + ImGui::CalcTextSize(before.c_str()).x;
      const ImRect hit(ImVec2(x0, r.Min.y), ImVec2(x0 + ImGui::CalcTextSize(item.c_str()).x, r.Max.y));
      ui::NoteUiItem(item.c_str(), hit);
      if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(hit.Min, hit.Max) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        app::SetClipboardTextUtf8(g_app.hwnd, stat_text[i]);
        g_app.has_clip = false;  // the cells copied before are no longer what a paste pastes
        g_app.SetStatus(TR("Copied to clipboard"), 2.0);
      }
      before += item + "    ";
    }
  }
  // right-click: Excel's Customize Status Bar, each statistic with its value
  if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r.Min, r.Max) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##yc_status_menu");
  if (ImGui::BeginPopup("##yc_status_menu")) {
    ImGui::TextDisabled("%s", TR("Customize Status Bar"));
    ImGui::Separator();
    app::Settings& c = app::Config();
    auto entry = [&](const char* label, const char* value, bool* flag) {
      if (!ImGui::MenuItem(label, value, *flag)) return;
      *flag = !*flag;
      if (!app::SaveSettings()) g_app.SetStatus(TR("Couldn't save the settings: ") + app::ToUtf8(app::SettingsPath()), 8.0);
    };
    const auto& fv = g_app.FocusView();
    auto on_off = [](bool on) { return on ? TR("On") : TR("Off"); };
    entry(TR("Cell Mode"), mode, &c.status_cell_mode);
    entry(TR("Caps Lock"), on_off(::GetKeyState(VK_CAPITAL) & 1), &c.status_caps_lock);
    entry(TR("Num Lock"), on_off(::GetKeyState(VK_NUMLOCK) & 1), &c.status_num_lock);
    entry(TR("Scroll Lock"), on_off(app::ScrollLockOn()), &c.status_scroll_lock);
    entry(TR("End Mode"), on_off(fv.end_mode), &c.status_end_mode);
    entry(TR("Selection Mode"), fv.extend_mode ? TR("Extend Selection") : fv.add_mode ? TR("Add to Selection") : "", &c.status_selection_mode);
    const std::string unseen = std::to_string(app::ErrorsUnseen() + app::MessagesUnseen()) + TR(" new");
    entry(TR("Error Log"), unseen.c_str(), &c.status_error_log);
    ImGui::Separator();
    bool* flags[6] = {&c.status_average, &c.status_count, &c.status_numerical_count, &c.status_min, &c.status_max, &c.status_sum};
    for (int i = 0; i < 6; ++i) entry(stat_names[i], stat_text[i].c_str(), flags[i]);
    ImGui::Separator();
    entry(TR("View Shortcuts"), "", &c.status_view_shortcuts);
    entry(TR("Zoom Slider"), "", &c.status_zoom_slider);
    char zp[16];
    std::snprintf(zp, sizeof(zp), "%d%%", static_cast<int>(g_app.FocusView().zoom * 100.0f + 0.5f));
    entry(TR("Zoom"), zp, &c.status_zoom);
    ImGui::EndPopup();
  }
  // a message, or (like Excel) the paste hint for as long as copied cells are outlined (Esc or a paste ends it)
  const bool msg = ImGui::GetTime() < g_app.status_until && !g_app.status.empty();
  if (msg || g_app.marquee) {
    dl->PushClipRect(ImVec2(left, r.Min.y), ImVec2(right_x - S(16), r.Max.y), true);
    dl->AddText(ImVec2(left, ty), t.statusbar_text, msg ? g_app.status.c_str() : TR("Select destination and press Enter or choose Paste"));
    dl->PopClipRect();
  }
  ImGui::PopFont();
}

void WindowBegin(const char* name, const ImRect& r, ImGuiWindowFlags extra = 0) {
  ImGui::SetNextWindowPos(r.Min);
  ImGui::SetNextWindowSize(r.GetSize());
  ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin(name, nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse | extra);
  ImGui::PopStyleVar(3);
}

ImRect RangeScreenRect(const model::Sheet& sh, const render::GridView& v, const Rect& r);
bool g_esc_closed_popup = false;  // this frame's Escape closed a menu: it closes nothing else (Excel)

void Shortcuts() {
  ImGuiIO& io = ImGui::GetIO();
  if (!g_app.interactive) return;  // Application.Interactive = False
  if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_K)) ui::Commands().Execute("KillSwitch");  // [YC] emergency
  if (!g_app.edit.active && !io.WantTextInput && app::HandleOnKeys(g_app)) return;  // Application.OnKey comes before built-in keys
  // F3: paste a name while editing a formula too (like Excel)
  if (g_app.edit.active && ImGui::IsKeyChordPressed(ImGuiKey_F3)) ui::Commands().Execute("PasteName");
  if (g_app.edit.active && ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F3)) ui::Commands().Execute("InsertFunction");
  // while editing: font shortcuts change the selected characters (Format Cells shows only the Font tab)
  if (g_app.edit.active) {
    const std::pair<ImGuiKeyChord, const char*> edit_keys[] = {{ImGuiMod_Ctrl | ImGuiKey_B, "Bold"}, {ImGuiMod_Ctrl | ImGuiKey_I, "Italic"},
                                                               {ImGuiMod_Ctrl | ImGuiKey_U, "Underline"}, {ImGuiMod_Ctrl | ImGuiKey_5, "Strikethrough"},
                                                               {ImGuiMod_Ctrl | ImGuiKey_2, "Bold"}, {ImGuiMod_Ctrl | ImGuiKey_3, "Italic"},
                                                               {ImGuiMod_Ctrl | ImGuiKey_4, "Underline"},
                                                               {ImGuiMod_Ctrl | ImGuiKey_1, "FormatCellsDialog"}};
    for (const auto& [chord, cmd] : edit_keys) {
      if (ImGui::IsKeyChordPressed(chord) && app::EditHasTextSelection(g_app)) ui::Commands().Execute(cmd);
    }
    // Ctrl+; / Ctrl+Shift+;: today's date / the time at the caret, while typing too (Excel: =A1+ then Ctrl+;)
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Semicolon)) app::EditInsertText(g_app, app::TodayText());
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Semicolon)) app::EditInsertText(g_app, app::NowTimeText());
    // Ctrl+PgDn / Ctrl+PgUp while typing a formula: the next / previous sheet, to point at its cells (Excel), as a
    // click on its tab does; Enter or Escape comes back
    const bool next = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageDown), prev = !next && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_PageUp);
    if ((next || prev) && g_app.edit.active && !g_app.edit.text.empty() && g_app.edit.text[0] == '=' && g_app.snap) {
      const auto& v = g_app.snap->wb.sheets;
      const int32_t i = g_app.snap->wb.IndexOfId(g_app.active_sheet);
      for (int32_t k = i + (next ? 1 : -1); k >= 0 && k < static_cast<int32_t>(v.size()); k += next ? 1 : -1) {
        const uint32_t id = v[static_cast<size_t>(k)]->id;
        if (!v[static_cast<size_t>(k)]->visible || g_app.Win(id).floating) continue;
        g_app.active_sheet = g_app.focus_sheet = id;
        g_app.focus_win = 0;
        break;
      }
    }
  }
  // Ctrl+Shift+U expands / collapses the formula bar, also while a cell is edited (Excel)
  if ((g_app.edit.active || !io.WantTextInput) && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_U)) ui::Commands().Execute("ExpandFormulaBar");
  if (io.WantTextInput || g_app.edit.active || ui::ribbon::St().keytip_level) return;
  // an object is selected: Ctrl+C/X/V/D/1 and the font keys act on it (Excel); the keys that change cells are skipped
  const bool on_object = app::ObjectShortcuts(g_app);
  static const char* const kObjectSafe[] = {"FileSave", "Screenshot", "FileOpen", "FileNew", "Undo", "Redo", "Find", "Replace", "GoTo",
                                            "Calculate", "CalculateSheet", "CalculateFull", "ShowFormulas", "FileClose", "NextSheet", "PrevSheet",
                                            "NameManager", "OpenVSCode", "CommandSearch", "Macros", "FilePrint", "Spelling", "InsertSheet",
                                            "InsertChartSheet", "InsertChartObject", "ToggleObjectsDisplay", "ToggleOutlineSymbols", "NextPane",
                                            "PrevPane", "SelectNextSheet", "SelectPrevSheet", "FileSaveAs", "NextWindow", "PrevWindow"};
  struct Sc {
    ImGuiKeyChord chord;
    const char* cmd;
  };
  static const Sc kShortcuts[] = {
      {ImGuiMod_Ctrl | ImGuiKey_B, "Bold"},
      {ImGuiMod_Ctrl | ImGuiKey_I, "Italic"},
      {ImGuiMod_Ctrl | ImGuiKey_U, "Underline"},
      {ImGuiMod_Ctrl | ImGuiKey_5, "Strikethrough"},
      {ImGuiMod_Ctrl | ImGuiKey_S, "FileSave"},
      // Excel's older keys for them
      {ImGuiMod_Ctrl | ImGuiKey_2, "Bold"},
      {ImGuiMod_Ctrl | ImGuiKey_3, "Italic"},
      {ImGuiMod_Ctrl | ImGuiKey_4, "Underline"},
      {ImGuiMod_Ctrl | ImGuiKey_6, "ToggleObjectsDisplay"},
      {ImGuiMod_Ctrl | ImGuiKey_8, "ToggleOutlineSymbols"},
      {ImGuiMod_Ctrl | ImGuiKey_Backslash, "SelectRowDifferences"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Backslash, "SelectColumnDifferences"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_PageDown, "SelectNextSheet"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_PageUp, "SelectPrevSheet"},
      {ImGuiKey_F6, "NextPane"},
      {ImGuiMod_Shift | ImGuiKey_F6, "PrevPane"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, "Screenshot"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, "Screenshot"},
      {ImGuiMod_Ctrl | ImGuiKey_O, "FileOpen"},
      {ImGuiMod_Ctrl | ImGuiKey_N, "FileNew"},
      {ImGuiMod_Ctrl | ImGuiKey_Z, "Undo"},
      {ImGuiMod_Ctrl | ImGuiKey_C, "Copy"},
      {ImGuiMod_Ctrl | ImGuiKey_X, "Cut"},
      {ImGuiMod_Ctrl | ImGuiKey_V, "Paste"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, "PasteValues"},
      {ImGuiMod_Alt | ImGuiKey_Semicolon, "SelectVisibleCells"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, "SelectNotes"},
      {ImGuiMod_Shift | ImGuiKey_F5, "Find"},
      {ImGuiMod_Shift | ImGuiKey_F4, "FindNext"},
      {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_V, "PasteSpecial"},
      {ImGuiMod_Ctrl | ImGuiKey_1, "FormatCellsDialog"},
      {ImGuiMod_Ctrl | ImGuiKey_F, "Find"},
      {ImGuiMod_Ctrl | ImGuiKey_H, "Replace"},
      {ImGuiMod_Ctrl | ImGuiKey_G, "GoTo"},
      {ImGuiKey_F5, "GoTo"},
      {ImGuiMod_Ctrl | ImGuiKey_D, "FillDown"},
      {ImGuiMod_Ctrl | ImGuiKey_E, "FlashFill"},
      {ImGuiMod_Ctrl | ImGuiKey_R, "FillRight"},
      {ImGuiMod_Ctrl | ImGuiKey_Semicolon, "InsertDate"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Semicolon, "InsertTime"},
      {ImGuiMod_Ctrl | ImGuiKey_GraveAccent, "ShowFormulas"},
      {ImGuiMod_Ctrl | ImGuiKey_9, "HideRows"},
      {ImGuiMod_Ctrl | ImGuiKey_0, "HideColumns"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_5, "PercentStyle"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_4, "CurrencyFormat"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_GraveAccent, "GeneralFormat"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_1, "NumberStyle"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_2, "TimeFormat"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_3, "DateFormat"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_6, "ScientificFormat"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_7, "BorderOutside"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Minus, "BorderNone"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_9, "UnhideRows"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_0, "UnhideColumns"},
      {ImGuiMod_Ctrl | ImGuiKey_W, "FileClose"},
      {ImGuiMod_Ctrl | ImGuiKey_F4, "FileClose"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, "InsertCells"},
      {ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, "InsertCells"},
      {ImGuiMod_Ctrl | ImGuiKey_Minus, "DeleteCells"},
      {ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, "DeleteCells"},
      {ImGuiMod_Alt | ImGuiKey_Equal, "AutoSum"},
      {ImGuiKey_F9, "Calculate"},
      {ImGuiMod_Shift | ImGuiKey_F9, "CalculateSheet"},
      {ImGuiMod_Ctrl | ImGuiKey_Apostrophe, "CopyFormulaAbove"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Apostrophe, "CopyValueAbove"},
      {ImGuiMod_Ctrl | ImGuiKey_LeftBracket, "SelectPrecedents"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_LeftBracket, "SelectAllPrecedents"},
      {ImGuiMod_Ctrl | ImGuiKey_RightBracket, "SelectDependents"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_RightBracket, "SelectAllDependents"},
      {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_F9, "CalculateFull"},
      {ImGuiMod_Alt | ImGuiKey_F5, "PivotRefresh"},             // Excel: refresh the active PivotTable
      {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_F5, "RefreshAll"},
      {ImGuiMod_Shift | ImGuiKey_F11, "InsertSheet"},
      {ImGuiKey_F11, "InsertChartSheet"},
      {ImGuiMod_Alt | ImGuiKey_F1, "InsertChartObject"},
      {ImGuiMod_Ctrl | ImGuiKey_PageDown, "NextSheet"},
      {ImGuiMod_Ctrl | ImGuiKey_PageUp, "PrevSheet"},
      {ImGuiMod_Shift | ImGuiKey_F3, "InsertFunction"},
      {ImGuiMod_Ctrl | ImGuiKey_Slash, "SelectCurrentArray"},
      {ImGuiMod_Ctrl | ImGuiKey_F3, "NameManager"},
      {ImGuiKey_F3, "PasteName"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F3, "CreateNamesFromSelection"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Period, "FontSizeIncrease"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Comma, "FontSizeDecrease"},
      {ImGuiMod_Alt | ImGuiKey_F11, "OpenVSCode"},
      {ImGuiMod_Alt | ImGuiKey_Q, "CommandSearch"},
      {ImGuiMod_Alt | ImGuiKey_F8, "Macros"},
      {ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_RightArrow, "GroupOutline"},
      {ImGuiMod_Ctrl | ImGuiKey_T, "InsertTable"},
      {ImGuiMod_Ctrl | ImGuiKey_L, "InsertTable"},
      // shortcuts the ribbon advertises, as in Excel
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, "ToggleFilter"},
      {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_L, "ReapplyFilter"},
      {ImGuiMod_Ctrl | ImGuiKey_K, "InsertHyperlink"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, "FormatCellsFontDialog"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P, "FormatCellsFontDialog"},
      {ImGuiMod_Ctrl | ImGuiKey_P, "FilePrint"},
      {ImGuiMod_Ctrl | ImGuiKey_F2, "FilePrint"},
      {ImGuiKey_F7, "Spelling"},
      {ImGuiMod_Shift | ImGuiKey_F2, "InsertComment"},
      {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_M, "NewComment"},
      {ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_LeftArrow, "UngroupOutline"},
      // the file keys of Excel's function-key row
      {ImGuiKey_F12, "FileSaveAs"},
      {ImGuiMod_Alt | ImGuiKey_F2, "FileSaveAs"},
      {ImGuiMod_Shift | ImGuiKey_F12, "FileSave"},
      {ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_F2, "FileSave"},
      {ImGuiMod_Ctrl | ImGuiKey_F12, "FileOpen"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F12, "FilePrint"},
      {ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_F1, "InsertSheet"},
      // the next / previous workbook window (Excel's Ctrl+F6, Ctrl+Tab)
      {ImGuiMod_Ctrl | ImGuiKey_F6, "NextWindow"},
      {ImGuiMod_Ctrl | ImGuiKey_Tab, "NextWindow"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F6, "PrevWindow"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab, "PrevWindow"},
      {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T, "TblTotalRow"},  // a table's total row on / off
      {ImGuiMod_Alt | ImGuiKey_Apostrophe, "NewCellStyle"},          // the Style dialog for the active cell's formats
      {ImGuiMod_Ctrl | ImGuiKey_Q, "QuickAnalysis"},
  };
  for (const Sc& s : kShortcuts) {
    if (!ImGui::IsKeyChordPressed(s.chord)) continue;
    if (on_object && std::none_of(std::begin(kObjectSafe), std::end(kObjectSafe), [&](const char* c) { return std::strcmp(c, s.cmd) == 0; })) continue;
    ui::Commands().Execute(s.cmd);
  }
  // Shift+F10 or the Menu key: the cell's context menu next to the active cell; the arrows and Enter choose in it
  if ((ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F10) || ImGui::IsKeyChordPressed(ImGuiKey_Menu)) && g_app.FocusSheet()) {
    const auto& v = g_app.FocusView();
    const ImRect rc = RangeScreenRect(*g_app.FocusSheet(), v, Rect::Cell(v.sel.act_r, v.sel.act_c));
    // a cell scrolled out of view would put the menu off the grid (an OS window elsewhere with viewports on)
    const ImVec2 at(ImClamp(rc.Min.x + rc.GetWidth() * 0.5f, v.cells_rect.Min.x, v.cells_rect.Max.x), ImClamp(rc.Max.y, v.cells_rect.Min.y, v.cells_rect.Max.y));
    app::OpenCellContextMenuAt(at);
  }
  // Ctrl+Y and F4: Redo after an Undo, otherwise Repeat (Excel)
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || (!on_object && ImGui::IsKeyChordPressed(ImGuiKey_F4))) app::RedoOrRepeat(g_app);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !g_esc_closed_popup) {
    g_app.marquee = false;
    g_app.format_painter = false;
    app::BorderDrawOff();
  }
  // Ctrl+F1 is the ribbon's own key (ribbon.cpp): a second toggle here undid it in the same frame
}

void DrawPerfWindow() {
  if (!g_app.show_perf) return;
  ImGui::SetNextWindowSize(ImVec2(ui::S(320), ui::S(220)), ImGuiCond_FirstUseEver);
  app::PlaceNewWindow(app::WindowPlace::Right);
  if (ImGui::Begin(TR("Performance###yc_perf"), &g_app.show_perf)) {
    const auto& st = g_app.snap->stats;
    ImGui::Text(TR("Snapshot version: %llu"), static_cast<unsigned long long>(g_app.snap->version));
    ImGui::Text(TR("Visible cells (this frame): %lld"), static_cast<long long>(g_app.visible_cells));
    ImGui::Text(TR("Grid: %.2f ms  Ribbon: %.2f ms"), g_app.grid_ms, g_app.ribbon_ms);
    ImGui::Text(TR("Last recalc: %.3f ms, evaluated %u cells, %u retries"), st.ms, st.evaluated, st.retries);
    ImGui::Text(TR("Circular references: %zu"), g_app.snap->circular);
    if (const model::Sheet* sh = g_app.FocusSheet()) {
      ImGui::Text(TR("Row limit: %u  Used: %s"), sh->row_limit, sh->has_used ? core::RectA1(sh->used).c_str() : TR("(blank)"));
      ImGui::Text(TR("Custom row heights: %zu  Custom column widths: %zu"), sh->rows->custom_count(), sh->cols->custom_count());
    }
    ImGui::Text(TR("Styles: %zu  Formula definitions: %u"), g_app.snap->wb.styles->size(), g_app.snap->wb.formulas->size());
  }
  ImGui::End();
}

// The snapshot a frame stops using is often the last owner of the tiles live data replaced (hundreds of 64 KB tiles a
// second): it is released on this worker thread, so freeing them never shows up in a frame.
void ReleaseLater(std::shared_ptr<const engine::Snapshot> old) {
  struct Releaser {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::shared_ptr<const engine::Snapshot>> q;
    Releaser() {
      std::thread([this] {
        ::SetThreadDescription(::GetCurrentThread(), L"yc-snapshot-release");
        std::unique_lock<std::mutex> lk(mu);
        while (true) {
          cv.wait(lk, [this] { return !q.empty(); });
          auto batch = std::move(q);
          q.clear();
          lk.unlock();
          batch.clear();  // the release happens here
          lk.lock();
        }
      }).detach();
    }
  };
  if (!old || old.use_count() > 1) return;  // someone else still holds it: nothing would be freed here
  static Releaser* r = new Releaser();  // never destroyed: the thread runs until the process ends
  {
    std::lock_guard<std::mutex> lk(r->mu);
    if (r->q.size() >= 64) return;  // bounded (hard rule 6): the worker is behind, this one is freed here
    r->q.push_back(std::move(old));
  }
  r->cv.notify_one();
}

void BuildFrame(double last_frame_ms) {
 
  using ui::S;
  ImGuiIO& io = ImGui::GetIO();
  if (g_app.new_workbook_later) {
    g_app.new_workbook_later = false;
    app::NewWorkbook(g_app);
  }
  if (!g_pending_open.empty()) {
    const std::wstring f = g_pending_open;
    g_pending_open.clear();
    // a file sent by Explorer or dropped: its own window when this one holds a workbook (like Excel)
    if (app::WorkbookInUse(g_app)) app::OpenInNewWindow(f);
    else if (app::ConfirmDiscard(g_app)) app::OpenWorkbook(g_app, f);
  }
  if (g_app.python) g_app.python->ProcessUiQueue();
  if (auto cur = g_app.engine->Current(); cur != g_app.snap) {
    ReleaseLater(std::exchange(g_app.snap, std::move(cur)));
  }
  app::ApplyUndoMove(g_app);
  app::ApplyPendingCommit(g_app);
  app::ApplyPendingCopy(g_app);
  app::RunDeferred(g_app);
  app::CheckCircularWarning(g_app);
  if (g_app.snap && g_app.snap->version != g_app.last_shown_version) {
    g_app.last_shown_version = g_app.snap->version;
    ++g_app.snapshots_shown;
    if (g_app.snap->input_newest_ns) g_app.pending_input_ns = g_app.snap->input_newest_ns;
  }
  app::CheckProtectBlocks(g_app);
  app::UpdateTableContext(g_app);
  app::UpdateSparklineContext(g_app);
  app::UpdatePrintContext(g_app);
  app::UpdatePivotContext(g_app);
  app::UpdateStylesUi(g_app);
  app::UpdateAutoRecover(g_app);
  app::FinishBackgroundSave(g_app, false);  // Save / Save As writing on a worker: done yet?
  app::UpdateUiCustom(g_app);
  app::UpdateWindowsUi(g_app);
  app::UpdateLinksUi(g_app);
  app::UpdateIme(g_app);
  app::UpdateCustomRibbon(g_app);
  {
    // formula autocomplete: defined names and Python UDFs (rebuilt when the workbook or the host changes)
    static uint64_t last_ver = ~0ull;
    static size_t last_udf = ~size_t{0};
    const std::vector<std::string> udfs = g_app.python ? g_app.python->UdfNames() : std::vector<std::string>{};
    if (g_app.snap && (g_app.snap->version != last_ver || udfs.size() != last_udf)) {
      last_ver = g_app.snap->version;
      last_udf = udfs.size();
      std::vector<render::Completion> items;
      for (const auto& n : *g_app.snap->wb.names) {
        std::string scope = n.sheet_scope >= 0 && g_app.snap->Sheet(static_cast<uint32_t>(n.sheet_scope)) ? TR(" (") + g_app.snap->Sheet(static_cast<uint32_t>(n.sheet_scope))->name + TR(")") : "";
        items.push_back({n.name, "=" + (n.refers_to.rfind('=', 0) == 0 ? n.refers_to.substr(1) : n.refers_to) + scope, 1});
      }
      for (const auto& u : udfs) items.push_back({u, TR("Python user-defined function (@udf)"), 2});
      // tables, with their columns (listed after "Table1[")
      for (const auto& sh : g_app.snap->wb.sheets) {
        for (const auto& tb : *sh->tables) {
          render::Completion c{tb.name, core::QuoteSheetName(sh->name) + "!" + core::CellA1(tb.range.r0, tb.range.c0) + ":" + core::CellA1(tb.range.r1, tb.range.c1), 3};
          for (const auto& col : tb.columns) c.columns.push_back(col.name);
          items.push_back(std::move(c));
        }
      }
      render::SetFormulaCompletions(std::move(items));
    }
  }
  SyncWindows(g_app);
  g_app.SyncSheetView();
  app::SyncCommandState(g_app);
  app::PollProjectChanges(g_app);
  ui::dock::ApplyPendingRestore();
  // the sheets' window always sits in the main dock space (it can't be undocked): a saved layout without it in its
  // tree mustn't leave it floating
  if (ImGuiWindow* gh = ImGui::FindWindowByName(app::kGridHostWindow); gh && gh->DockId == 0 && !gh->DockIsActive) {
    ui::dock::DockWindow(app::kGridHostWindow, ui::dock::MainDockspaceId());
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 p0 = vp->Pos, sz = vp->Size;

  ImGui::PushFont(ui::F().ui, ui::F().ui_size);
  const float title_h = ui::titlebar::Height();
  const bool backstage = ui::ribbon::St().backstage;
  // Auto-hide Ribbon: a thin bar until it is clicked; then shown until the next click in the sheet
  const bool ribbon_hidden = app::RibbonHidden(g_app);
  // Application.DisplayFullScreen: no ribbon, formula bar or status bar (the backstage still opens over the sheets)
  const bool full_screen = g_app.full_screen && !backstage;
  const float tabs_h = full_screen ? 0.0f : ribbon_hidden ? S(14) : ui::ribbon::TabRowHeight();
  const bool body_visible = !full_screen && !ribbon_hidden && !ui::ribbon::St().collapsed && !backstage;
  const float body_h = body_visible ? ui::ribbon::BodyHeight() : 0.0f;
  // expanded (Ctrl+Shift+U, or its bottom edge dragged), the formula bar shows several lines
  const float fbar_h = (g_app.show_formula_bar && !backstage && !full_screen) ? S(30) + static_cast<float>(app::Config().formula_bar_lines - 1) * S(17) : 0.0f;
  const float status_h = (g_app.show_status_bar && !g_app.full_screen) || backstage ? S(24) : 0.0f;  // Application.DisplayStatusBar
  const float pkg_h = backstage ? 0.0f : app::DrawPackageBar(g_app, ImRect(), true);
  // the Trust Center's Message Bar ("SECURITY WARNING  Python code has been disabled.  [Enable Content]"): under the
  // ribbon, above the formula bar, as in Excel
  const float sec_h = backstage ? 0.0f : app::trust::DrawMessageBar(g_app, ImRect(), true);
  const float chrome_h = title_h + tabs_h + body_h + sec_h + fbar_h + pkg_h;

  WindowBegin("##yc_chrome", ImRect(p0, ImVec2(p0.x + sz.x, p0.y + chrome_h)));
  {
    const std::vector<std::string> qat = app::QatCommands();
    std::vector<const char*> qat_ids;
    for (const auto& q : qat) qat_ids.push_back(q.c_str());
    ui::titlebar::Draw(g_app.hwnd, ImRect(p0, ImVec2(p0.x + sz.x, p0.y + title_h)), qat_ids.data(), static_cast<int>(qat_ids.size()));
    const ImRect tabs_r(ImVec2(p0.x, p0.y + title_h), ImVec2(p0.x + sz.x, p0.y + title_h + tabs_h));
    const double t0 = NowMs();
    if (full_screen) {
      // no tab row in full screen
    } else if (ribbon_hidden) {
      // the bar that brings the ribbon back
      ImDrawList* cdl = ImGui::GetWindowDrawList();
      cdl->AddRectFilled(tabs_r.Min, tabs_r.Max, IM_COL32(0xE4, 0xE4, 0xE4, 255));
      for (int i = -1; i <= 1; ++i) cdl->AddCircleFilled(ImVec2(tabs_r.GetCenter().x + i * S(6), tabs_r.GetCenter().y), S(2.0f), IM_COL32(0x40, 0x40, 0x40, 255));
      ImGui::SetCursorScreenPos(tabs_r.Min);
      if (ImGui::InvisibleButton("##ribbon_reveal", tabs_r.GetSize())) g_app.ribbon_revealed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("Show the ribbon"));
    } else {
      ui::ribbon::DrawTabRow(tabs_r);
    }
    if (body_visible) ui::ribbon::DrawBody(ImRect(ImVec2(p0.x, tabs_r.Max.y), ImVec2(p0.x + sz.x, tabs_r.Max.y + body_h)));
    // a revealed auto-hide ribbon goes away with the next click below it
    if (g_app.ribbon_revealed && ImGui::IsMouseClicked(0) && ImGui::GetIO().MousePos.y > p0.y + chrome_h && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
      g_app.ribbon_revealed = false;
    g_app.ribbon_ms = NowMs() - t0;
    g_app.ribbon_ms_hist.push_back(g_app.ribbon_ms);
    if (g_app.ribbon_ms_hist.size() > 120000) g_app.ribbon_ms_hist.erase(g_app.ribbon_ms_hist.begin(), g_app.ribbon_ms_hist.begin() + 60000);
    if (sec_h > 0) {
      const float y = tabs_r.Max.y + body_h;
      app::trust::DrawMessageBar(g_app, ImRect(ImVec2(p0.x, y), ImVec2(p0.x + sz.x, y + sec_h)), false);
    }
    if (fbar_h > 0) {
      const float y = tabs_r.Max.y + body_h + sec_h;
      DrawFormulaBar(ImRect(ImVec2(p0.x, y), ImVec2(p0.x + sz.x, y + fbar_h)));
    }
    if (pkg_h > 0) {
      const float y = tabs_r.Max.y + body_h + sec_h + fbar_h;
      app::DrawPackageBar(g_app, ImRect(ImVec2(p0.x, y), ImVec2(p0.x + sz.x, y + pkg_h)), false);
    }
  }
  ImGui::End();

  // ---- Workspace (main dock space) ----
  const ImRect work(ImVec2(p0.x, p0.y + chrome_h), ImVec2(p0.x + sz.x, p0.y + sz.y - status_h));
  g_app.work_rect = work;  // Application.UsableWidth / UsableHeight
  g_app.chart_tip.clear();  // set again by a chart's tip drawn this frame
  WindowBegin("##yc_workspace", work, ImGuiWindowFlags_NoBackground);
  const ImGuiID main_ds = ui::dock::MainDockspaceId();
  if (g_app.first_frame && !g_app.layout_restored) ui::dock::BuildDefaultLayout(app::kGridHostWindow, app::kChartWindow, work.GetSize());
  if (g_app.req_layout_reset) {
    g_app.req_layout_reset = false;
    for (auto& [id, w] : g_app.wins) {
      if (w.floating) Redock(id);
    }
    ui::dock::BuildDefaultLayout(app::kGridHostWindow, app::kChartWindow, work.GetSize());
    app::ForgetPanelDocking();  // panels go back beside the sheets, on their Dock side
    g_app.SetStatus(TR("Layout reset"));
  }
  if (g_app.first_frame && g_args.dock_into_container && !g_app.containers.empty()) {
    const ImGuiID cds = ui::dock::ContainerDockspaceId(g_app.containers.front().id);
    ImGui::DockBuilderRemoveNode(cds);
    ImGui::DockBuilderAddNode(cds, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(cds, ImVec2(S(700), S(420)));
    for (auto& [id, w] : g_app.wins) {
      if (w.floating) {
        ui::dock::DockWindow(g_app.SheetWindowName(id).c_str(), cds);
        break;
      }
    }
    ImGui::DockBuilderFinish(cds);
  }
  // every docked window shows its tab (title, close button): it is how a window is taken out again (dock_tabs.cpp).
  // Workbook.Protect(Windows) locks the workbook's own windows one by one (LockWindow), not the whole dock space: a
  // YCGrid window the user docks (the Error Log...) still comes out
  ImGui::DockSpace(main_ds, work.GetSize(), backstage ? ImGuiDockNodeFlags_KeepAliveOnly : 0);
  ImGui::End();

  int64_t visible_cells = 0;
  double grid_ms = 0.0;

  // ---- Grid of the main window ----
  render::TabBarIO tabs;
  {
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoTabBar) | static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoUndocking) |
                                  static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDockingOverMe);
    ImGui::SetNextWindowClass(&wc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(app::kGridHostWindow, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
      if (!g_app.edit.active) {
        g_app.focus_sheet = g_app.active_sheet;
        g_app.focus_win = 0;
      }
    }
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    for (const auto& sh : g_app.snap->wb.sheets) {
      if (!sh->visible) continue;
      tabs.tabs.push_back(render::SheetTab{sh->id, sh->name, sh->tab_color, g_app.Win(sh->id).floating});
    }
    tabs.active = g_app.active_sheet;
    tabs.selected = g_app.group_sheets;  // grouped sheets: their tabs are white too
    {
      uint32_t rid = 0;
      if (app::SheetRenaming(&rid, &tabs.rename_buf, &tabs.rename_cap, &tabs.rename_focus)) tabs.renaming = rid;
    }
    tabs.split_ratio = &g_app.tab_split;
    tabs.first_tab_scroll = &g_app.tab_scroll;
    if (g_app.main_hidden) {
      // View > Hide: the workbook's window is hidden (Excel's empty workspace); View > Unhide shows it again
      ImGui::GetWindowDrawList()->AddRectFilled(cp, cp + avail, IM_COL32(0xC8, 0xC8, 0xC8, 255));
      ImGui::Dummy(avail);
    } else {
      const double t0 = NowMs();
      const auto st = DrawSheet(g_app.active_sheet, ImRect(cp, cp + avail), &tabs);
      grid_ms += NowMs() - t0;
      visible_cells += st.stats.cells_processed;
    }
    ImGui::End();
  }
  if (tabs.add_sheet) ui::Commands().Execute("NewSheetAfter");
  if (tabs.activate >= 0) {
    const uint32_t id = static_cast<uint32_t>(tabs.activate);
    // clicking another sheet while typing a formula: switch the view but keep editing (cross-sheet reference); any
    // other entry is written first, as in Excel (it stayed open on a sheet that couldn't take its keys)
    if (g_app.edit.active && (g_app.edit.text.empty() || g_app.edit.text[0] != '=')) app::CommitEdit(g_app, false, 0);
    g_app.active_sheet = id;
    g_app.focus_sheet = id;
    g_app.focus_win = 0;
    if (!g_app.edit.active) app::SheetTabClicked(g_app, id, 0);  // a group stays or ends as in Excel
  }
  // Ctrl / Shift + click on tabs: grouped sheets (Excel's [Group]); right-click on the arrows: the Activate list
  if (tabs.group_toggle >= 0 && !g_app.edit.active) app::SheetTabClicked(g_app, static_cast<uint32_t>(tabs.group_toggle), 1);
  if (tabs.group_extend >= 0 && !g_app.edit.active) app::SheetTabClicked(g_app, static_cast<uint32_t>(tabs.group_extend), 2);
  if (tabs.activate_list) ui::Commands().Execute("ActivateSheetList");
  if (tabs.tear_off >= 0 && g_app.WindowsProtected()) {
    g_app.SayWindowsLocked();
  } else if (tabs.tear_off >= 0 && !g_app.Win(static_cast<uint32_t>(tabs.tear_off)).floating) {
    const uint32_t id = static_cast<uint32_t>(tabs.tear_off);
    g_app.Win(id).floating = true;
    g_app.just_torn.push_back(id);
    g_app.focus_sheet = id;
    SyncWindows(g_app);
    g_app.SetStatus(TR("\"") + g_app.snap->Sheet(id)->name + TR("\" is now a floating window: drag it back to the tab bar or double-click its title bar to put it back"));
  }
  if (tabs.activate_floating >= 0) {
    ImGui::SetWindowFocus(g_app.SheetWindowName(static_cast<uint32_t>(tabs.activate_floating)).c_str());
    g_app.focus_sheet = static_cast<uint32_t>(tabs.activate_floating);
  }
  if (tabs.redock >= 0) Redock(static_cast<uint32_t>(tabs.redock));
  if (tabs.rename >= 0) app::BeginRenameSheet(g_app, static_cast<uint32_t>(tabs.rename));
  app::SheetRenameResult(g_app, tabs.rename_drawn, tabs.rename_commit, tabs.rename_cancel);
  if (tabs.context >= 0) app::OpenSheetTabContextMenu(static_cast<uint32_t>(tabs.context));
  if (tabs.move_from >= 0) {
    const uint32_t id = static_cast<uint32_t>(tabs.move_from);
    const bool to_end = tabs.move_to >= static_cast<int64_t>(tabs.tabs.size());
    const uint32_t target_id = to_end ? 0 : tabs.tabs[static_cast<size_t>(tabs.move_to)].id;
    if (tabs.move_copy) {
      // Ctrl+drag: a copy before the tab dropped on (or at the end), and it becomes the active sheet
      const uint32_t made = g_app.engine->Call([&](engine::Document& d) {
        d.Begin(TR("Copy Sheet"));
        const uint32_t v = d.CopySheet(id, to_end ? static_cast<int32_t>(d.wb().sheets.size()) : d.wb().IndexOfId(target_id));
        d.Commit();
        return v;
      });
      g_app.engine->Sync();
      g_app.snap = g_app.engine->Current();
      if (made) app::ActivateSheet(g_app, made);
    } else {
      g_app.engine->PostUser(TR("Move Sheet"), [id, target_id, to_end](engine::Document& d) {
        d.MoveSheet(id, to_end ? static_cast<int>(d.wb().sheets.size()) - 1 : d.wb().IndexOfId(target_id));
      });
    }
  }

  // ---- Containers ----
  for (auto it = g_app.containers.begin(); it != g_app.containers.end();) {
    const std::string name = g_app.ContainerWindowName(*it);
    if (!ui::dock::ApplyNextWindowRect(name.c_str())) {
      ImGui::SetNextWindowSize(ImVec2(S(720), S(460)), ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowPos(ImVec2(p0.x + sz.x * 0.25f, p0.y + sz.y * 0.25f), ImGuiCond_FirstUseEver);
    }
    bool open = true;
    const bool locked = g_app.WindowLocked("container:" + std::to_string(it->id));
    const int lock_flags = app::LockWindow(locked);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(name.c_str(), locked ? nullptr : &open, ImGuiWindowFlags_NoCollapse | lock_flags);
    ImGui::PopStyleVar();
    const ImGuiID ds = ui::dock::ContainerDockspaceId(it->id);
    // locked: what Python docked stays, and nothing else docks in (it couldn't come out again)
    ImGui::DockSpace(ds, ImVec2(0, 0), locked ? ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoDocking : 0);
    if (const ImGuiDockNode* node = ImGui::DockBuilderGetNode(ds); node && node->IsEmpty()) {
      ImDrawList* dl = node->HostWindow ? node->HostWindow->DrawList : ImGui::GetWindowDrawList();
      const ImVec2 c = node->Pos + node->Size * 0.5f;
      const char* hint = TR("Drag sheet windows or charts here to dock them");
      const ImVec2 ts = ImGui::CalcTextSize(hint);
      dl->AddText(c - ts * 0.5f, IM_COL32(0x80, 0x80, 0x80, 255), hint);
    }
    ImGui::End();
    if (!open) {
      ImGui::DockBuilderRemoveNode(ds);
      it = g_app.containers.erase(it);
    } else {
      ++it;
    }
  }

  // ---- Floating sheet windows ----
  for (const auto& sh : g_app.snap->wb.sheets) {
    const uint32_t id = sh->id;
    if (!g_app.Win(id).floating || !sh->visible) continue;
    const std::string name = g_app.SheetWindowName(id);
    const bool torn = std::find(g_app.just_torn.begin(), g_app.just_torn.end(), id) != g_app.just_torn.end();
    if (torn) {
      ImGui::SetNextWindowPos(io.MousePos - ImVec2(S(80), S(12)), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(S(760), S(480)), ImGuiCond_Always);
    } else if (!ui::dock::ApplyNextWindowRect(name.c_str())) {
      ImGui::SetNextWindowSize(ImVec2(S(760), S(480)), ImGuiCond_FirstUseEver);
    }
    bool open = true;
    const bool locked = g_app.WindowsProtected() && !torn;
    const int lock_flags = app::LockWindow(locked);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(name.c_str(), locked ? nullptr : &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | lock_flags);
    ImGui::PopStyleVar();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (torn && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::StartMouseMovingWindow(w);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_app.edit.active) {
      g_app.focus_sheet = id;
      g_app.focus_win = 0;
    }
    const bool title_dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && w->TitleBarRect().Contains(io.MousePos) && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootWindow);
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const double t0 = NowMs();
    const auto st = DrawSheet(id, ImRect(cp, cp + avail), nullptr);
    grid_ms += NowMs() - t0;
    visible_cells += st.stats.cells_processed;
    ImGui::End();
    if (!open || title_dbl) Redock(id);
  }
  g_app.just_torn.clear();
  // ---- View > New Window: more windows on the workbook ----
  const bool arranging = g_app.arrange_windows;
  int arrange_k = 0;  // the place of this window among the shown ones (Arrange Windows)
  for (auto it = g_app.extra_wins.begin(); it != g_app.extra_wins.end();) {
    auto& xw = *it;
    if (xw.hidden) {  // View > Hide
      ++it;
      continue;
    }
    ++arrange_k;
    const model::Sheet* xs = g_app.snap->Sheet(xw.view.sheet);
    if (!xs || !xs->visible) {
      xw.view = render::GridView{};
      xw.view.sheet = g_app.active_sheet;
      xs = g_app.snap->Sheet(xw.view.sheet);
    }
    const std::string book = g_app.file_path.empty() ? std::string("Book1") : app::ToUtf8(std::filesystem::path(g_app.file_path).filename().wstring());
    const std::string name = book + ":" + std::to_string(xw.id) + " - " + (xs ? xs->name : std::string()) + "###yc_xwin" + std::to_string(xw.id);
    ImGui::SetNextWindowSize(ImVec2(S(760), S(480)), ImGuiCond_FirstUseEver);
    if (ImGuiWindow* host = ImGui::FindWindowByName(app::kGridHostWindow)) {
      // a new window opens over the sheets, a step down and right of the one before (never over the ribbon)
      const float step = S(28) * static_cast<float>((xw.id + 5) % 6);
      ImGui::SetNextWindowPos(ImVec2(host->Pos.x + S(40) + step, host->Pos.y + S(30) + step), ImGuiCond_FirstUseEver);
      if (g_app.arrange_windows) {
        // Arrange Windows over the sheets (the main one in the first place): Tiled, Horizontal, Vertical, Cascade
        int shown = 1;
        for (const auto& e : g_app.extra_wins) shown += e.hidden ? 0 : 1;
        const float n = static_cast<float>(shown), k = static_cast<float>(arrange_k);
        ImVec2 pos = host->Pos, size = host->Size;
        if (g_app.arrange_mode == 0) {
          const int cols = static_cast<int>(std::ceil(std::sqrt(n))), rows = (shown + cols - 1) / cols;
          size = ImVec2(host->Size.x / static_cast<float>(cols), host->Size.y / static_cast<float>(rows));
          pos = host->Pos + ImVec2(size.x * static_cast<float>(arrange_k % cols), size.y * static_cast<float>(arrange_k / cols));
        } else if (g_app.arrange_mode == 1) {
          size = ImVec2(host->Size.x, host->Size.y / n);
          pos = host->Pos + ImVec2(0, size.y * k);
        } else if (g_app.arrange_mode == 2) {
          size = ImVec2(host->Size.x / n, host->Size.y);
          pos = host->Pos + ImVec2(size.x * k, 0);
        } else {
          size = host->Size * 0.7f;
          pos = host->Pos + ImVec2(S(28), S(28)) * k;
        }
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
      }
    }
    if (xw.focus_request) {
      ImGui::SetNextWindowFocus();
      xw.focus_request = false;
    }
    bool open = true;
    const bool locked = g_app.WindowsProtected();
    const int lock_flags = app::LockWindow(locked);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(name.c_str(), locked ? nullptr : &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | lock_flags);
    ImGui::PopStyleVar();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_app.edit.active) {
      g_app.focus_sheet = xw.view.sheet;
      g_app.focus_win = xw.id;
    }
    render::TabBarIO xtabs;
    for (const auto& sh : g_app.snap->wb.sheets) {
      if (sh->visible) xtabs.tabs.push_back(render::SheetTab{sh->id, sh->name, sh->tab_color, false});
    }
    xtabs.active = xw.view.sheet;
    xtabs.split_ratio = &xw.tab_split;
    xtabs.first_tab_scroll = &xw.tab_scroll;
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const uint32_t xid = xw.id;
    if (xs) DrawSheet(xw.view.sheet, ImRect(cp, cp + avail), &xtabs, xid);
    ImGui::End();
    render::GridView* v = g_app.ExtraView(xid);
    if (v && xtabs.activate >= 0 && static_cast<uint32_t>(xtabs.activate) != v->sheet) {
      // another sheet in this window
      *v = render::GridView{};
      v->sheet = static_cast<uint32_t>(xtabs.activate);
      g_app.focus_sheet = v->sheet;
      g_app.focus_win = xid;
    }
    if (!open) {
      if (g_app.focus_win == xid) {
        g_app.focus_win = 0;
        g_app.focus_sheet = g_app.active_sheet;
      }
      it = g_app.extra_wins.erase(std::find_if(g_app.extra_wins.begin(), g_app.extra_wins.end(), [xid](const auto& w) { return w.id == xid; }));
    } else {
      it = std::find_if(g_app.extra_wins.begin(), g_app.extra_wins.end(), [xid](const auto& w) { return w.id == xid; }) + 1;
    }
  }
  {
    ImGuiContext& g = *GImGui;
    if (g.MovingWindow) {
      g_app.last_moving_window = g.MovingWindow->RootWindowDockTree ? g.MovingWindow->RootWindowDockTree->Name : g.MovingWindow->Name;
    } else if (!g_app.last_moving_window.empty()) {
      if (tabs.bar_rect.Contains(io.MousePos)) {
        for (auto& [id, w] : g_app.wins) {
          if (w.floating && g_app.last_moving_window == g_app.SheetWindowName(id)) Redock(id);
        }
      }
      g_app.last_moving_window.clear();
    }
  }

  if (arranging) g_app.arrange_windows = false;
  // ---- Charts, demos, performance ----
  if (g_app.show_chart) {
    if (!ui::dock::ApplyNextWindowRect(app::kChartWindow)) ImGui::SetNextWindowSize(ImVec2(S(640), S(360)), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(app::kChartWindow, &g_app.show_chart, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) app::DrawChartWindow(g_app);
    ImGui::End();
  }
  if (g_app.show_imgui_demo) ImGui::ShowDemoWindow(&g_app.show_imgui_demo);
  if (g_app.show_implot_demo) ImPlot::ShowDemoWindow(&g_app.show_implot_demo);
  DrawPerfWindow();
  app::DrawObjectWindows(g_app);
  app::DrawPivotFieldList(g_app);
  app::DrawRulePopups(g_app);
  app::DrawMiscWindows(g_app);
  app::DrawRecoveryPane(g_app);
  app::DrawUiCustom(g_app);
  app::DrawImeComposition(g_app);
  app::DrawSpellingWindow(g_app);
  app::DrawDataWindows(g_app);
  app::DrawPythonUi(g_app);
  app::DrawErrorWindow(g_app);

  // ---- Backstage ----
  if (backstage) {
    WindowBegin("##yc_backstage", ImRect(ImVec2(p0.x, p0.y + title_h), ImVec2(p0.x + sz.x, p0.y + sz.y - status_h)), 0);
    // cover the other windows; not while a popup (e.g. a drop-down list) is open, or the popup would end up behind
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ui::ribbon::DrawBackstage(ImRect(ImVec2(p0.x, p0.y + title_h), ImVec2(p0.x + sz.x, p0.y + sz.y - status_h)));
    ImGui::End();
  }

  // Ctrl+Pause as ImGui sees it (scripted input; a real Ctrl+Break arrives as VK_CANCEL in WndProc)
  if (ImGui::IsKeyPressed(ImGuiKey_Pause, false) && ImGui::GetIO().KeyCtrl && g_app.python) g_app.python->NotifyHost("host.interrupt", io::MV::Map());
  // Application.CalculationInterruptKey: Esc, or any key, stops a calculation that has run for a while (the UI never
  // waits for it; the cells it didn't reach stay pending, and the next calculation or F9 goes on)
  if (g_app.calc_interrupt_key != 0 && g_app.engine && g_app.engine->BusySeconds() > 0.3) {
    bool hit = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    for (int k = ImGuiKey_NamedKey_BEGIN; g_app.calc_interrupt_key == 2 && !hit && k < ImGuiKey_GamepadStart; ++k) {
      hit = ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false);
    }
    if (hit) g_app.engine->InterruptCalc();
  }

  // ---- Status bar ----
  g_app.visible_cells = visible_cells;
  g_app.grid_ms = grid_ms;
  if (status_h > 0) {
    WindowBegin("##yc_status", ImRect(ImVec2(p0.x, p0.y + sz.y - status_h), ImVec2(p0.x + sz.x, p0.y + sz.y)));
    DrawStatusBar(ImRect(ImVec2(p0.x, p0.y + sz.y - status_h), ImVec2(p0.x + sz.x, p0.y + sz.y)), last_frame_ms);
    ImGui::End();
  }

  app::DrawContextMenus(g_app);
  app::DrawDialogs(g_app);
  // Alt+Down on a cell whose list, filter or totals button didn't take it (each opens its own popup while the sheet
  // draws): Pick From Drop-down List, the column's entries, as in Excel (the shortcut only showed on the menu)
  if (ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) &&
      !ImGui::GetIO().WantTextInput && !g_app.edit.active && !backstage && KeyboardTarget() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    ui::Commands().Execute("PickFromList");
  app::DrawCellsUi(g_app);  // the drop menus, the Activate list, Fill Across Worksheets, Pick From List
  app::DrawPythonDialogs(g_app);
  app::DrawFileDialogs(g_app);
  ImGui::PopFont();

  Shortcuts();
  if (g_app.painter_armed && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    g_app.painter_armed = false;
    app::ApplyFormatPainter(g_app);
  }
  app::BorderDrawFrame(g_app);

  if (g_app.req_layout_save) {
    g_app.req_layout_save = false;
    g_app.SetStatus(app::SaveLayout(g_app) ? TR("Saved layout: ") + app::ToUtf8(g_app.LayoutPath()) : TR("Couldn't save the layout"), 6.0);
  }
  if (g_app.req_layout_load) {
    g_app.req_layout_load = false;
    g_app.SetStatus(app::LoadLayout(g_app, false) ? TR("Layout loaded") : TR("Layout file not found: ") + app::ToUtf8(g_app.LayoutPath()), 6.0);
  }
  app::TrackSheetEvents(g_app);  // SelectionChange, Activate / Deactivate after what this frame changed
  g_app.first_frame = false;
}

// ---- Simulated input (--keys): one step per frame ----
struct ScriptStep {
  enum Kind { Text, Key, Wait, Click, Drag, DblClick, Sleep, Move, Command, Goto, CellClick, RightClick, ImeComp, ImeCommit, Press, Release, Hold, Unhold, Wheel,
              RightPress, RightRelease } kind;
  int clicks = 1;  // CellClick: 1 click, 2 double-click, 3 right-click
  std::string text;
  ImGuiKey key = ImGuiKey_None;
  bool ctrl = false, shift = false, alt = false;
  int n = 1;
  float x = 0, y = 0, x2 = 0, y2 = 0;
};
std::deque<ScriptStep> g_script;  // deque: appending steps does not invalidate pointers to existing steps
size_t g_script_pos = 0;
int g_script_wait = 0;
ScriptStep* g_key_release = nullptr;
int g_click_phase = 0;
// a scripted double-click: ImGui takes the two presses frames apart, and on a slow frame (a debug build under load)
// they were further apart than its double-click time, two single clicks (chaos 7138: the Error Log's tab stayed docked).
// The time is widened for the few frames the presses take, then put back
int g_dbl_frames = 0;
float g_dbl_time = 0.30f, g_single_delay = 0.32f;
ScriptStep* g_drag = nullptr;
int g_drag_phase = 0;

// a key by its ImGui name ("DownArrow", "F4", "A"), in any case, or a common short name ("Down", "Esc", "PgDn", ";")
ImGuiKey KeyByName(const std::string& name) {
  auto lower = [](std::string v) {
    for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return v;
  };
  static const std::pair<const char*, const char*> kAlias[] = {
      {"down", "DownArrow"}, {"up", "UpArrow"},     {"left", "LeftArrow"}, {"right", "RightArrow"}, {"esc", "Escape"},
      {"del", "Delete"},     {"return", "Enter"},   {"pgup", "PageUp"},    {"pgdn", "PageDown"},    {"ins", "Insert"},
      {";", "Semicolon"},    {"=", "Equal"},        {"-", "Minus"},        {",", "Comma"},          {".", "Period"},
      {"/", "Slash"},        {"`", "GraveAccent"},  {"'", "Apostrophe"},   {"[", "LeftBracket"},    {"]", "RightBracket"},
      {"\\", "Backslash"},   {"space", "Space"},    {"alt", "LeftAlt"},    {"ctrl", "LeftCtrl"},    {"shift", "LeftShift"}};
  const std::string want = lower(name);
  std::string n = name;
  for (const auto& a : kAlias)
    if (want == a.first) n = a.second;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
    if (lower(n) == lower(ImGui::GetKeyName(static_cast<ImGuiKey>(k)))) return static_cast<ImGuiKey>(k);
  }
  return ImGuiKey_None;
}

// parse a ui.input script and append its steps; returns "" or why it was refused (then nothing is appended)
std::string ParseScript(const std::string& src) {
  std::vector<ScriptStep> steps;
  size_t start = 0;
  while (start <= src.size()) {
    size_t end = src.find('|', start);
    if (end == std::string::npos) end = src.size();
    const std::string tok = src.substr(start, end - start);
    start = end + 1;
    if (tok.size() < 2) {
      if (end == src.size()) break;
      continue;
    }
    ScriptStep st{};
    const std::string arg = tok.substr(2);
    switch (tok[0]) {
      case 't': st.kind = ScriptStep::Text; st.text = arg; break;
      case 'w': st.kind = ScriptStep::Wait; st.n = std::max(1, std::atoi(arg.c_str())); break;
      case 's': st.kind = ScriptStep::Sleep; st.n = std::max(1, std::atoi(arg.c_str())); break;
      case 'x': st.kind = ScriptStep::Command; st.text = arg; break;  // run a command (CommandRegistry id)
      case 'I': st.kind = ScriptStep::ImeComp; st.text = arg; break;    // IME composition in progress (tests)
      case 'r': st.kind = ScriptStep::RightClick; break;                 // right-click where the mouse is (after m:x,y)
      case 'J': st.kind = ScriptStep::ImeCommit; st.text = arg; break;  // IME result (tests)
      case 'g': st.kind = ScriptStep::Goto; st.text = arg; break;     // select an address (same as the Name Box)
      case 'A': st.kind = ScriptStep::CellClick; st.text = arg; st.clicks = 1; break;  // click a cell
      case 'Q': st.kind = ScriptStep::CellClick; st.text = arg; st.clicks = 2; break;  // double-click a cell
      case 'R': st.kind = ScriptStep::CellClick; st.text = arg; st.clicks = 3; break;  // right-click a cell
      case 'u': st.kind = ScriptStep::Release; break;  // release the left button (after p:x,y and moves)
      case 'U': st.kind = ScriptStep::RightRelease; break;  // release the right button (after P:x,y and moves)
      case 'H': st.kind = ScriptStep::Hold; st.text = arg; break;  // hold a modifier down (H:ctrl, H:shift, H:alt) through later steps
      case 'h': st.kind = ScriptStep::Unhold; break;               // release the held modifiers
      case 'W': st.kind = ScriptStep::Wheel; st.x = static_cast<float>(std::atof(arg.c_str())); break;  // mouse wheel notches where the mouse is (W:-3 scrolls down)
      case 'm':
      case 'p':
      case 'P':
      case 'c':
      case 'd':
      case 'D': {
        // p:x,y presses the left button and holds it (m: moves it, u: releases): a drag a hand makes in steps
        // P:x,y the right button (a right-drag: Excel's drop menus of the selection's border and the fill handle)
        st.kind = tok[0] == 'm'   ? ScriptStep::Move
                  : tok[0] == 'p' ? ScriptStep::Press
                  : tok[0] == 'P' ? ScriptStep::RightPress
                  : tok[0] == 'c' ? ScriptStep::Click
                  : (tok[0] == 'd' ? ScriptStep::Drag : ScriptStep::DblClick);
        float v[4] = {0, 0, 0, 0};
        size_t p = 0;
        for (int i = 0; i < 4 && p <= arg.size(); ++i) {
          const size_t comma = arg.find(',', p);
          v[i] = static_cast<float>(std::atof(arg.substr(p, comma == std::string::npos ? std::string::npos : comma - p).c_str()));
          if (comma == std::string::npos) break;
          p = comma + 1;
        }
        st.x = v[0];
        st.y = v[1];
        st.x2 = v[2];
        st.y2 = v[3];
        break;
      }
      default: {
        st.kind = ScriptStep::Key;
        std::string k = arg;
        while (k.size() > 2 && k[1] == '-') {
          if (k[0] == 'c') st.ctrl = true;
          if (k[0] == 's') st.shift = true;
          if (k[0] == 'a') st.alt = true;
          k = k.substr(2);
        }
        st.text = k;
        st.key = KeyByName(k);
        if (st.key == ImGuiKey_None) return "Unknown key \"" + k + "\" in \"" + tok + "\" (use ImGui key names: A, F4, DownArrow, Enter, Semicolon...)";
        break;
      }
    }
    steps.push_back(st);
    if (end == src.size()) break;
  }
  g_script.insert(g_script.end(), steps.begin(), steps.end());
  return {};
}

// screen rectangle of a range (window of the active sheet; inside/outside frozen panes are converted separately)
ImRect RangeScreenRect(const model::Sheet& sh, const render::GridView& v, const Rect& r) {
  const double sc = v.px_scale;
  const uint32_t fr = v.frozen_top + v.freeze_rows, fc = v.frozen_left + v.freeze_cols;
  const auto& rows = *sh.rows;
  const auto& cols = *sh.cols;
  const double top_h = v.freeze_rows ? static_cast<double>(rows.Offset(fr) - rows.Offset(v.frozen_top)) * sc : 0.0;
  const double left_w = v.freeze_cols ? static_cast<double>(cols.Offset(fc) - cols.Offset(v.frozen_left)) * sc : 0.0;
  auto ry = [&](int64_t row) {
    return (v.freeze_rows && row < fr) ? v.cells_rect.Min.y + static_cast<double>(rows.Offset(row) - rows.Offset(v.frozen_top)) * sc
                                       : v.cells_rect.Min.y + top_h + static_cast<double>(rows.Offset(row) - rows.Offset(v.v.first)) * sc;
  };
  // T layout: the columns before the frozen ones sit in a left pane, right-aligned at its last column
  const bool tee = v.TLayout();
  const double mw = tee ? v.mirror_w : 0.0;
  const int64_t mir_last = tee ? int64_t{v.frozen_left} - 1 - (v.h.first - fc) : -1;
  auto cx = [&](int64_t col) {
    if (tee && col < v.frozen_left) return v.cells_rect.Min.x + mw + static_cast<double>(cols.Offset(col) - cols.Offset(mir_last + 1)) * sc;
    return (v.freeze_cols && col < fc) ? v.cells_rect.Min.x + mw + static_cast<double>(cols.Offset(col) - cols.Offset(v.frozen_left)) * sc
                                       : v.cells_rect.Min.x + mw + left_w + static_cast<double>(cols.Offset(col) - cols.Offset(v.h.first)) * sc;
  };
  // the far edges from the near ones plus the size: the column after a frozen one is in the scrolled pane, so its x
  // isn't this cell's right edge (a click on a frozen cell landed on the far left once the main pane scrolled)
  const double x0 = cx(r.c0), y0 = ry(r.r0);
  const double x1 = x0 + static_cast<double>(cols.Offset(int64_t{r.c1} + 1) - cols.Offset(r.c0)) * sc;
  const double y1 = y0 + static_cast<double>(rows.Offset(int64_t{r.r1} + 1) - rows.Offset(r.r0)) * sc;
  return ImRect(ImVec2(static_cast<float>(x0), static_cast<float>(y0)), ImVec2(static_cast<float>(x1), static_cast<float>(y1)));
}

// screen coordinates of a cell's center (window of the active sheet); scrolls first when not visible
bool CellScreenCenter(const std::string& addr, ImVec2* out) {
  const model::Sheet* sh = g_app.FocusSheet();
  Rect r;
  if (!sh || !core::ParseRectA1(addr, &r)) return false;
  auto& v = g_app.FocusView();
  render::EnsureVisible(*sh, v, r.r0, r.c0);
  const ImRect rc = RangeScreenRect(*sh, v, Rect::Cell(r.r0, r.c0));
  *out = rc.GetCenter();
  return true;
}

// MCP screenshot request: first switch sheet and scroll, capture after the next frame is drawn
void ProcessShots() {
  auto& shots = g_app.shots;
  for (auto it = shots.begin(); it != shots.end();) {
    auto& q = *it;
    if (q.wait_frames == 2) {
      if (q.sheet && g_app.snap->Sheet(q.sheet) && q.sheet != g_app.focus_sheet) app::ActivateSheet(g_app, q.sheet);
      const model::Sheet* sh = g_app.FocusSheet();
      Rect r;
      if (q.target == "range" && sh && core::ParseRectA1(q.address, &r)) {
        auto& v = g_app.FocusView();
        // put the range's upper-left corner at the upper-left of the visible area (so the headers line up)
        v.v.first = std::max<int64_t>(r.r0, v.frozen_top + v.freeze_rows);
        v.h.first = std::max<int64_t>(r.c0, v.frozen_left + v.freeze_cols);
      }
      uint32_t cr = 0, cc = 0;
      if (q.target == "chart" && sh && !sh->chart_sheet && app::ChartTopLeftCell(g_app, sh->id, q.address, &cr, &cc)) {
        // a chart's picture (Chart.CopyPicture / Export, Paste as Picture): the chart in view first
        auto& v = g_app.FocusView();
        v.v.first = std::max<int64_t>(cr > 0 ? cr - 1 : 0, v.frozen_top + v.freeze_rows);
        v.h.first = std::max<int64_t>(cc > 0 ? cc - 1 : 0, v.frozen_left + v.freeze_cols);
      }
    }
    if (--q.wait_frames > 0) {
      ++it;
      continue;
    }
    const ImVec2 vp = ImGui::GetMainViewport()->Pos;
    RECT crop{};
    std::string info;
    const model::Sheet* sh = g_app.FocusSheet();
    const auto& v = g_app.FocusView();
    auto to_rect = [&](const ImRect& rc) {
      return RECT{static_cast<LONG>(rc.Min.x - vp.x), static_cast<LONG>(rc.Min.y - vp.y), static_cast<LONG>(rc.Max.x - vp.x), static_cast<LONG>(rc.Max.y - vp.y)};
    };
    if (q.target == "imgui_window") {
      if (ImGuiWindow* win = ImGui::FindWindowByName(q.address.c_str()); win && win->WasActive) crop = to_rect(win->Rect());
    } else if (q.target == "chart" && sh) {
      ImRect rc;
      if (app::ChartScreenRect(g_app, sh->id, q.address, &rc)) crop = to_rect(rc);
    } else if (q.target == "grid" && sh) {
      crop = to_rect(q.headers ? ImRect(v.grid_rect.Min, v.cells_rect.Max) : v.cells_rect);
    } else if (q.target == "range" && sh) {
      Rect r;
      if (core::ParseRectA1(q.address, &r)) {
        ImRect rc = RangeScreenRect(*sh, v, r);
        rc.ClipWithFull(v.cells_rect);
        if (q.headers) rc.Min = ImVec2(std::min(rc.Min.x, v.grid_rect.Min.x), std::min(rc.Min.y, v.grid_rect.Min.y));
        crop = to_rect(rc);
      }
    }
    if (sh) {
      info = TR("Sheet \"") + sh->name + TR("\" visible range ") + core::CellA1(static_cast<uint32_t>(v.v.first), static_cast<uint32_t>(v.h.first)) + ":" +
             core::CellA1(static_cast<uint32_t>(v.last_bottom_row), static_cast<uint32_t>(v.last_right_col)) + TR(", zoom ") +
             std::to_string(static_cast<int>(std::lround(v.zoom * 100))) + "%";
    }
    int w = 0, h = 0;
    bool ok = false;
    if (q.on_pixels) {
      std::vector<uint8_t> px;
      ok = ReadBackbuffer(crop, &px, &w, &h);
      if (ok) q.on_pixels(std::move(px), w, h);
    } else {
      ok = q.to_clipboard ? CopyBackbufferToClipboard(crop, &w, &h) : SaveBackbufferPng(q.path, crop, q.max_width, &w, &h);
    }
    if (q.done) q.done(ok, w, h, ok ? info : TR("Can't write PNG"));
    it = shots.erase(it);
  }
}

// while a script runs, the mouse position comes from the script (the Win32 backend overwrites it with the real cursor every frame)
bool g_script_mouse_set = false;
ImVec2 g_script_mouse;
void ScriptMouse(float x, float y) {
  g_script_mouse = ImVec2(x, y);
  g_script_mouse_set = true;
  ImGui::GetIO().AddMousePosEvent(x, y);
}

// the window under a scripted mouse: the backend asks Windows where the real cursor is, which a script (or a hidden
// YCGrid) doesn't move, so dragging a window found no dock target. Topmost first; a window being dragged lets the
// mouse through (NoInputs), so the one under it is found, as Windows does for a hand
ImGuiID ScriptViewportAt(ImVec2 pos) {
  ImGuiViewportP* best = nullptr;
  for (ImGuiViewportP* vp : GImGui->Viewports) {
    if ((vp->Flags & ImGuiViewportFlags_NoInputs) || !vp->GetMainRect().Contains(pos)) continue;
    if (!best || vp->LastFocusedStampCount > best->LastFocusedStampCount) best = vp;
  }
  return best ? best->ID : 0;
}

void FeedScript() {
  if (g_script_mouse_set) {
    if (app::UiScriptBusy()) ImGui::GetIO().AddMousePosEvent(g_script_mouse.x, g_script_mouse.y);
    else g_script_mouse_set = false;
  }
  ImGuiIO& io = ImGui::GetIO();
  if (g_dbl_frames > 0 && --g_dbl_frames == 0) {
    io.MouseDoubleClickTime = g_dbl_time;
    io.MouseSingleClickDelay = g_single_delay;
  }
  if (g_key_release) {
    io.AddKeyEvent(g_key_release->key, false);
    if (g_key_release->ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false), io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
    if (g_key_release->shift) io.AddKeyEvent(ImGuiMod_Shift, false), io.AddKeyEvent(ImGuiKey_LeftShift, false);
    if (g_key_release->alt) io.AddKeyEvent(ImGuiMod_Alt, false), io.AddKeyEvent(ImGuiKey_LeftAlt, false);
    g_key_release = nullptr;
    return;
  }
  if (g_click_phase == 5) {  // press one frame after moving there (like a real mouse: hover first, then click)
    io.AddMouseButtonEvent(0, true);
    g_click_phase = 3;
    return;
  }
  if (g_click_phase == 3) {  // hold one extra frame (text boxes lose focus first, then the button receives the click)
    g_click_phase = 1;
    return;
  }
  if (g_click_phase == 1 || g_click_phase == 2) {
    io.AddMouseButtonEvent(g_click_phase == 2 ? 1 : 0, false);
    g_click_phase = 0;
    return;
  }
  if (g_drag) {
    // drag: move to the end point over 8 frames, hold there 4 frames (as a hand does over a drop target: docking
    // shows its preview there), then release
    const ImVec2 o = ImGui::GetMainViewport()->Pos;
    if (g_drag_phase < 12) {
      const float t = std::min(1.0f, (g_drag_phase + 1) / 8.0f);
      ScriptMouse(o.x + g_drag->x + (g_drag->x2 - g_drag->x) * t, o.y + g_drag->y + (g_drag->y2 - g_drag->y) * t);
      ++g_drag_phase;
    } else {
      io.AddMouseButtonEvent(0, false);
      g_drag = nullptr;
    }
    return;
  }
  if (g_script_wait > 0) {
    --g_script_wait;
    return;
  }
  if (app::UiWaitsForEngine(g_app)) return;  // a command put off until the engine caught up runs before the next key
  if (g_script_pos >= g_script.size()) return;
  ScriptStep& st = g_script[g_script_pos++];
  const ImVec2 origin = ImGui::GetMainViewport()->Pos;
  switch (st.kind) {
    case ScriptStep::Text: io.AddInputCharactersUTF8(st.text.c_str()); break;
    case ScriptStep::Wheel: io.AddMouseWheelEvent(0.0f, st.x); break;
    case ScriptStep::ImeComp: app::SimulateIme(st.text, false); break;
    case ScriptStep::ImeCommit: app::SimulateIme(st.text, true); break;
    case ScriptStep::Wait: g_script_wait = st.n - 1; break;
    case ScriptStep::Sleep: ::Sleep(static_cast<DWORD>(st.n)); break;
    case ScriptStep::Move: ScriptMouse(origin.x + st.x, origin.y + st.y); break;
    case ScriptStep::Command: ui::Commands().Execute(st.text); break;
    case ScriptStep::Goto: app::Goto(g_app, st.text); break;
    case ScriptStep::RightClick:
      io.AddMouseButtonEvent(1, true);
      g_click_phase = 2;
      break;
    case ScriptStep::Press:
      ScriptMouse(origin.x + st.x, origin.y + st.y);
      io.AddMouseButtonEvent(0, true);
      break;
    case ScriptStep::Release:
      io.AddMouseButtonEvent(0, false);
      break;
    case ScriptStep::RightPress:
      ScriptMouse(origin.x + st.x, origin.y + st.y);
      io.AddMouseButtonEvent(1, true);
      break;
    case ScriptStep::RightRelease:
      io.AddMouseButtonEvent(1, false);
      break;
    case ScriptStep::CellClick: {
      // compute the cell center (scroll it into view first), expand into two steps: move -> click
      ImVec2 c;
      if (!CellScreenCenter(st.text, &c)) break;
      ScriptStep mv{};
      mv.kind = ScriptStep::Move;
      mv.x = c.x - origin.x;
      mv.y = c.y - origin.y;
      ScriptStep ck = mv;
      ck.kind = st.clicks == 2 ? ScriptStep::DblClick : st.clicks == 3 ? ScriptStep::RightClick : ScriptStep::Click;
      g_script.insert(g_script.begin() + static_cast<std::ptrdiff_t>(g_script_pos), {mv, ck});
      break;
    }
    case ScriptStep::Click:
      ScriptMouse(origin.x + st.x, origin.y + st.y);
      g_click_phase = 5;
      break;
    case ScriptStep::Drag:
      ScriptMouse(origin.x + st.x, origin.y + st.y);
      io.AddMouseButtonEvent(0, true);
      g_drag = &st;
      g_drag_phase = 0;
      break;
    case ScriptStep::DblClick:
      // two clicks (ImGui detects double-clicks by time: widened while ImGui takes them, see g_dbl_frames)
      if (g_dbl_frames == 0) g_dbl_time = io.MouseDoubleClickTime, g_single_delay = io.MouseSingleClickDelay;
      io.MouseDoubleClickTime = 30.0f;
      io.MouseSingleClickDelay = 31.0f;  // ImGui checks it stays above the double-click time
      g_dbl_frames = 8;
      // its first press is a first click: within the widened time one double-click after another counted on to a
      // triple and a fourth click, and the second wasn't a double-click (DockTest)
      io.MouseClickedTime[0] = -FLT_MAX;
      io.MouseClickedLastCount[0] = 0;
      ScriptMouse(origin.x + st.x, origin.y + st.y);
      io.AddMouseButtonEvent(0, true);
      io.AddMouseButtonEvent(0, false);
      io.AddMouseButtonEvent(0, true);
      g_click_phase = 1;
      break;
    case ScriptStep::Hold:
    case ScriptStep::Unhold: {
      // a modifier held while the mouse clicks and drags (Ctrl+drag copies, Shift+click extends)
      static int held = 0;  // 1 ctrl, 2 shift, 4 alt
      auto set = [&](int bit, ImGuiKey mod, ImGuiKey key, bool down) {
        io.AddKeyEvent(mod, down);
        io.AddKeyEvent(key, down);
        held = down ? (held | bit) : (held & ~bit);
      };
      if (st.kind == ScriptStep::Hold) {
        const std::string m = st.text;
        if (m == "ctrl") set(1, ImGuiMod_Ctrl, ImGuiKey_LeftCtrl, true);
        if (m == "shift") set(2, ImGuiMod_Shift, ImGuiKey_LeftShift, true);
        if (m == "alt") set(4, ImGuiMod_Alt, ImGuiKey_LeftAlt, true);
      } else {
        if (held & 1) set(1, ImGuiMod_Ctrl, ImGuiKey_LeftCtrl, false);
        if (held & 2) set(2, ImGuiMod_Shift, ImGuiKey_LeftShift, false);
        if (held & 4) set(4, ImGuiMod_Alt, ImGuiKey_LeftAlt, false);
      }
      break;
    }
    case ScriptStep::Key:
      if (st.key == ImGuiKey_None) st.key = KeyByName(st.text);
      // a chord holds the modifier keys themselves too, as a keyboard does (Windows sends VK_MENU for Alt+W): code that
      // asks for the Alt key (the ribbon's Alt+W) saw only the flag, and Alt+W, F, F typed FF into the cell
      if (st.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true), io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
      if (st.shift) io.AddKeyEvent(ImGuiMod_Shift, true), io.AddKeyEvent(ImGuiKey_LeftShift, true);
      if (st.alt) io.AddKeyEvent(ImGuiMod_Alt, true), io.AddKeyEvent(ImGuiKey_LeftAlt, true);
      io.AddKeyEvent(st.key, true);
      // Windows sends Shift+Space as a typed space as well (WM_CHAR): the grid must not type it into the cell
      if (st.key == ImGuiKey_Space && st.shift && !st.ctrl && !st.alt) io.AddInputCharacter(' ');
      g_key_release = &st;
      break;
  }
}

// A hidden instance's platform frame after its first: the window's size and the time step, as the Win32 backend sets
// them, without asking Windows for the mouse. The backend calls WindowFromPoint at the real cursor every frame, which a
// hidden window is never under (its mouse and keys come from scripts), and on a desktop slow to answer that call took
// 500 ms: every frame of every hidden instance took as long, and trader_session reported frozen screens that were
// the desktop's. Nor does it read the real keyboard (the backend's Shift / Win key workarounds).
void HeadlessPlatformFrame() {
  static LARGE_INTEGER freq{}, last{};
  if (!freq.QuadPart) ::QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now{};
  ::QueryPerformanceCounter(&now);
  ImGuiIO& io = ImGui::GetIO();
  RECT rc{};
  ::GetClientRect(g_app.hwnd, &rc);
  io.DisplaySize = ImVec2(static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top));
  io.DeltaTime = last.QuadPart ? std::max(1e-6f, static_cast<float>(static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(freq.QuadPart)))
                               : 1.0f / 60.0f;
  last = now;
  io.AddMouseViewportEvent(0);  // what the backend reports for a window the real mouse is never over
}

void RenderFrame() {
  if (g_rendering || !g_gfx.swap) return;
  g_rendering = true;
  if (g_gfx.frame_waitable) ::WaitForSingleObjectEx(g_gfx.frame_waitable, 1000, TRUE);
  if (g_gfx.resize_w && g_gfx.resize_h) {
    ReleaseRtv();
    g_gfx.swap->ResizeBuffers(0, g_gfx.resize_w, g_gfx.resize_h, DXGI_FORMAT_UNKNOWN, g_gfx.swap_flags);
    g_gfx.resize_w = g_gfx.resize_h = 0;
    CreateRtv();
  }
  const double t0 = NowMs();
  static double last_ms = 0.0;
  // new cell fonts can only be added to the atlas between frames
  if (ui::LoadPendingCellFonts()) ui::RequestRedrawIn(0.0);
  ImGui_ImplDX11_NewFrame();
  static bool platform_started = false;
  if (!g_app.headless || !platform_started) {
    ImGui_ImplWin32_NewFrame();
    platform_started = true;
  } else {
    HeadlessPlatformFrame();
  }
  {  // Excel's own cursors (cells, headers, unhide): the backend shows an arrow for them, so set them here
    static int shown = ImGuiMouseCursor_Arrow;
    const int want = render::LastFrameCursor();
    if (want != shown) {
      shown = want;
      if (void* h = render::NativeCursor(want)) ::SetCursor(static_cast<HCURSOR>(h));
    }
  }
  if (!g_script.empty()) FeedScript();
  if (g_script_mouse_set) ImGui::GetIO().AddMouseViewportEvent(ScriptViewportAt(g_script_mouse));
  // in a dialog, Tab goes through every control and Space ticks a check box, as in Excel (Enter stays the default
  // button); elsewhere the keys belong to the grid
  if (g_app.dialog != app::Dialog::None || app::ContextMenuOpen()) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  else ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  // a dialog or menu open as the frame starts: an Escape now closes it and nothing else (Excel: the first Escape puts
  // the context menu or dialog away, the copied cells stay outlined). Read before NewFrame, where ImGui's keyboard
  // navigation already closes a menu
  const bool popup_before = g_app.dialog != app::Dialog::None || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  // a drop-down (combo, menu) open over a dialog: its Escape closes the drop-down only, as in Excel, not the dialog
  // too (ImGui closes the drop-down inside NewFrame, and every dialog then saw the same Escape and closed itself)
  bool over_dialog = false;
  {
    const ImGuiContext& g = *GImGui;
    int top_modal = -1;
    for (int i = 0; i < g.OpenPopupStack.Size; ++i) {
      if (g.OpenPopupStack[i].Window && (g.OpenPopupStack[i].Window->Flags & ImGuiWindowFlags_Modal)) top_modal = i;
    }
    over_dialog = top_modal >= 0 && top_modal + 1 < g.OpenPopupStack.Size;
  }
  ImGui::NewFrame();
  if (popup_before && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_esc_closed_popup = true;
  if (over_dialog && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    ImGui::SetKeyOwner(ImGuiKey_Escape, ImHashStr("yc.esc.dropdown"), ImGuiInputFlags_LockThisFrame);
  }
  {
    // Escape closes an open menu, drop-down or list (ribbon galleries, font boxes, context menus) even when a text box in
    // it has the keyboard, as in Excel; dialogs close themselves
    const ImGuiKeyData* esc = ImGui::GetKeyData(ImGuiKey_Escape);
    if (esc->Down && esc->DownDuration == 0.0f && g_app.dialog == app::Dialog::None &&
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !ImGui::GetTopMostPopupModal()) {
      ImGui::ClosePopupsExceptModals();
      g_esc_closed_popup = true;
    }
  }
  if (g_app.dialog != app::Dialog::None && ImGui::IsKeyDown(ImGuiKey_Enter)) {
    // Enter is the dialog's default button (DlgOkButton): it doesn't also tick the check box the keyboard is on
    ImGuiContext& g = *GImGui;
    if (g.NavActivateId && !(g.NavIdItemFlags & ImGuiItemFlags_Inputable) && !ImGui::IsKeyDown(ImGuiKey_Space))
      g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = 0;
  }
  BuildFrame(last_ms);
  g_esc_closed_popup = false;
  app::DrawSafetyFrame(g_app);
  app::DrawTradingFrame(g_app);
  app::DrawAlertRules(g_app);
  app::DrawFigures(g_app);
  app::DrawReplayFrame(g_app);
  app::DrawLatencyMonitor(g_app);
  app::DrawScreenshot(g_app);
  app::DrawDataViews(g_app);
  if (!g_app.interactive && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    // Application.Interactive = False: an invisible window over everything takes the mouse (dialogs still work)
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::Begin("##yc_input_blocked", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::InvisibleButton("##blocked", vp->Size);
    ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    ImGui::End();
  }
  app::DrawDockTabs(g_app);
  app::FinishModalCopy(g_app);
  render::LastFrameCursor() = ImGui::GetMouseCursor();
  {  // a window fitting its contents that narrows 8 frames in a row is shrinking by itself (see AppState::shrinking_windows)
    static std::unordered_map<ImGuiID, std::pair<float, int>> widths;
    g_app.window_rects.clear();
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
      // (ImGui's implicit Debug window is begun every frame and dropped at its end unless something was drawn in it:
      // listed only then, when it shows over the sheet)
      if (w->IsFallbackWindow && !w->WriteAccessed) continue;
      if (w->Active && !(w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)))
        g_app.window_rects.push_back({w->Name, w->Pos.x, w->Pos.y, w->SizeFull.x, w->SizeFull.y,
                                      (w->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0, (w->Flags & ImGuiWindowFlags_Popup) != 0});
      if (!w->Active || !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize) || (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip))) continue;
      auto& [last, run] = widths[w->ID];
      run = w->SizeFull.x < last - 0.25f ? run + 1 : 0;
      if (run >= 8) g_app.shrinking_windows.insert(w->Name);
      // contents wider than the room a dialog gives them, frame after frame: cut off on the right (a width fixed too
      // small; one frame while a dialog widens to new contents doesn't count)
      static std::unordered_map<ImGuiID, int> clipped_run;
      const float room = w->SizeFull.x - w->WindowPadding.x * 2.0f - (w->ScrollbarY ? w->ScrollbarSizes.x : 0.0f);
      int& cr = clipped_run[w->ID];
      cr = (w->Flags & ImGuiWindowFlags_Modal) && !w->Appearing && w->ContentSizeIdeal.x > room + 1.0f ? cr + 1 : 0;
      if (cr >= 3) g_app.clipped_windows.insert(w->Name);
      last = w->SizeFull.x;
    }
  }
  ImGui::Render();
  const float clear[4] = {1, 1, 1, 1};
  g_gfx.ctx->OMSetRenderTargets(1, &g_gfx.rtv, nullptr);
  g_gfx.ctx->ClearRenderTargetView(g_gfx.rtv, clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  if (!g_args.screenshot.empty() && g_args.frames > 0 && g_frame_count + 1 == g_args.frames) SaveBackbufferPng(g_args.screenshot);
  if (!g_app.shots.empty()) ProcessShots();
  if (!g_app.shots.empty() || (!g_script.empty() && app::UiScriptBusy())) ui::RequestRedrawIn(0.0);
  last_ms = NowMs() - t0;
  g_app.frame_ms.push_back(last_ms);
  if (g_app.frame_ms.size() > 120000) g_app.frame_ms.erase(g_app.frame_ms.begin(), g_app.frame_ms.begin() + 60000);
  if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
  }
  g_gfx.swap->Present(g_app.vsync && g_app.perf_hz <= 0 ? 1 : 0, 0);
  ++g_frame_count;
  ++g_app.frames_presented;
  if (g_app.pending_input_ns) {
    g_app.input_latency_ms.push_back(static_cast<double>(engine::PerfCounterNs() - g_app.pending_input_ns) / 1e6);
    if (g_app.input_latency_ms.size() > 4096) g_app.input_latency_ms.erase(g_app.input_latency_ms.begin(), g_app.input_latency_ms.begin() + 2048);
    g_app.pending_input_ns = 0;
  }
  g_rendering = false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && g_app.cursor != -4143) {
    // Application.Cursor set by a macro (xlWait while it works) wins over every cursor of the frame, as in Excel
    ::SetCursor(::LoadCursorW(nullptr, g_app.cursor == 2 ? IDC_WAIT : g_app.cursor == 3 ? IDC_IBEAM : IDC_ARROW));
    return TRUE;
  }
  if (msg == WM_KEYDOWN && wparam == VK_CANCEL && g_app.python) {
    // Ctrl+Break interrupts the running macro (Application.EnableCancelKey decides how, in the host)
    g_app.python->NotifyHost("host.interrupt", io::MV::Map());
  }
  if (msg == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {  // the grid's Excel cursors (the backend knows only ImGui's)
    if (void* h = render::NativeCursor(render::LastFrameCursor())) {
      ::SetCursor(static_cast<HCURSOR>(h));
      return TRUE;
    }
  }
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
  LRESULT result = 0;
  if (app::HandleImeMessage(g_app, hwnd, msg, wparam, lparam, &result)) return result;
  if (ui::titlebar::HandleMessage(hwnd, msg, wparam, lparam, &result)) return result;
  switch (msg) {
    case WM_YC_SNAPSHOT:
      return 0;  // only used to wake up the main loop
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        g_gfx.resize_w = LOWORD(lparam);
        g_gfx.resize_h = HIWORD(lparam);
      }
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = ::GetDpiForWindow(hwnd);
      mmi->ptMinTrackSize = POINT{static_cast<LONG>(::MulDiv(560, static_cast<int>(dpi), 96)), static_cast<LONG>(::MulDiv(420, static_cast<int>(dpi), 96))};
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_ENTERSIZEMOVE:
      g_in_sizemove = true;
      ::SetTimer(hwnd, 1, 16, nullptr);
      return 0;
    case WM_EXITSIZEMOVE:
      g_in_sizemove = false;
      ::KillTimer(hwnd, 1);
      return 0;
    case WM_TIMER:
      if (wparam == 1 && g_in_sizemove) RenderFrame();
      return 0;
    case WM_COPYDATA: {
      const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
      if (cds && cds->dwData == app::shell::kCopyDataOpenFile && cds->cbData >= sizeof(wchar_t)) {
        OpenFileRequest(std::wstring(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1));
        return TRUE;
      }
      return FALSE;
    }
    case WM_SYSCOMMAND:
      if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
      break;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wparam);
      wchar_t path[MAX_PATH * 4];
      if (::DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path)))) OpenFileRequest(path);
      ::DragFinish(drop);
      return 0;
    }
    case WM_ENABLE:
      // a modal dialog (message box, file dialog) just closed: it swallowed the mouse-up / key-up of the click or key
      // that opened it, so ImGui would still see the button held and a click would turn into a range selection
      if (wparam && ImGui::GetCurrentContext()) {
        ImGui::GetIO().ClearInputMouse();
        ImGui::GetIO().ClearInputKeys();
      }
      break;
    case WM_CLOSE:
      // Workbook_BeforeClose first, as in Excel (before the save prompt): its answer sends WM_CLOSE again unless cancelled
      if (g_args.frames == 0 && !g_app.close_without_asking && app::AskBeforeClose(g_app)) return 0;
      if (g_args.frames == 0 && !g_app.close_without_asking && !app::ConfirmDiscard(g_app)) {
        app::BeforeCloseUndone(g_app);
        return 0;
      }
      ::DestroyWindow(hwnd);
      return 0;
    case WM_RENDERFORMAT:  // a program pastes the cells this window copied: write their text now
      if (wparam == CF_UNICODETEXT) app::RenderClipboardText(g_app, false);
      return 0;
    case WM_RENDERALLFORMATS:
      app::RenderClipboardText(g_app, true);
      return 0;
    case WM_DESTROYCLIPBOARD:
      app::ClipboardReleased();
      return 0;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void WriteReport() {
  if (g_args.report.empty()) return;
  auto stats = [](std::vector<double> v) {
    nlohmann::json j;
    if (v.empty()) return j;
    if (v.size() > 20) v.erase(v.begin(), v.begin() + 10);
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    j["avg"] = sum / static_cast<double>(v.size());
    j["p50"] = v[v.size() / 2];
    j["p99"] = v[std::min(v.size() - 1, v.size() * 99 / 100)];
    j["max"] = v.back();
    return j;
  };
  RECT rc;
  ::GetClientRect(g_app.hwnd, &rc);
  nlohmann::json j;
  j["frames"] = g_frame_count;
  j["frame_cpu_ms"] = stats(g_app.frame_ms);
  j["ribbon_cpu_ms"] = stats(g_app.ribbon_ms_hist);
  j["ribbon_stages"] = ui::ribbon::St().last_stages;
  j["ribbon_available_width"] = ui::ribbon::St().last_available_width;
  j["visible_cells_last_frame"] = g_app.visible_cells;
  j["client_width"] = rc.right - rc.left;
  j["client_height"] = rc.bottom - rc.top;
  j["dpi_scale"] = ImGui::GetMainViewport()->DpiScale;
  j["imgui_version"] = IMGUI_VERSION;
  j["implot_version"] = IMPLOT_VERSION;
  j["warp"] = g_args.warp;
  if (g_app.snap) {
    j["snapshot_version"] = g_app.snap->version;
    j["last_recalc_ms"] = g_app.snap->stats.ms;
    const model::Sheet* sh = g_app.snap->Sheet(g_app.active_sheet);
    if (sh) {
      j["active_sheet"] = sh->name;
      j["row_limit"] = sh->row_limit;
      j["used"] = sh->has_used ? core::RectA1(sh->used) : "";
      j["A1"] = engine::InputTextOf(g_app.snap->wb, sh->id, 0, 0);
      // cells in the used range (up to 200): input text and displayed text
      nlohmann::json cells = nlohmann::json::object();
      if (sh->has_used) {
        int n = 0;
        for (uint32_t r = sh->used.r0; r <= sh->used.r1 && n < 200; ++r) {
          for (uint32_t c = sh->used.c0; c <= sh->used.c1 && n < 200; ++c) {
            if (sh->IsEmptyCell(r, c)) continue;
            const auto& st = g_app.snap->wb.styles->Get(sh->GetStyle(r, c));
            cells[core::CellA1(r, c)] = {engine::InputTextOf(g_app.snap->wb, sh->id, r, c),
                                         render::FormatCellText(*g_app.snap, st, sh->GetValue(r, c), -1, nullptr, nullptr, nullptr)};
            ++n;
          }
        }
      }
      j["cells"] = cells;
      const auto& v = g_app.Win(sh->id).view;
      j["selection"] = core::RectA1(v.sel.Active());
      j["active_cell"] = core::CellA1(v.sel.act_r, v.sel.act_c);
      j["editing"] = g_app.edit.active;
      j["edit_text"] = g_app.edit.text;
      j["can_undo"] = g_app.snap->can_undo;
      j["sheets"] = nlohmann::json::array();
      for (const auto& x : g_app.snap->wb.sheets) j["sheets"].push_back(x->name);
    }
  }
  std::ofstream f(g_args.report, std::ios::binary | std::ios::trunc);
  f << j.dump(2);
}

// demo data (--demo): quote table, formulas, formatting, merged cells, nanosecond timestamps
void FillDemo(engine::Document& d) {
  const uint32_t s1 = d.wb().sheets[0]->id;
  d.RenameSheet(s1, TR("Quotes"));
  const char* heads[] = {TR("Symbol"), TR("Name"), TR("Bid"), TR("Ask"), TR("Last"), TR("Change"), TR("Qty"), TR("Timestamp (ns)"), TR("Amount")};
  for (uint32_t c = 0; c < 9; ++c) d.SetInput(s1, 1, c, heads[c]);
  d.SetInput(s1, 0, 0, TR("YCGrid Quote Board Demo"));
  d.Merge(s1, Rect{0, 0, 0, 8}, false, true);
  d.ModifyStyle(s1, Rect{0, 0, 0, 8}, [](model::Style& x) {
    x.bold = true;
    x.font_size = 16;
    x.fill_color = 0xFF1F4E79;
    x.font_color = 0xFFFFFFFF;
  });
  d.SetRowHeight(s1, 0, 0, 32);
  d.ModifyStyle(s1, Rect{1, 0, 1, 8}, [](model::Style& x) {
    x.bold = true;
    x.fill_color = 0xFFDDEBF7;
    x.halign = model::HAlign::Center;
  });
  for (uint32_t i = 0; i < 40; ++i) {
    const uint32_t r = 2 + i;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "TX%04u", i + 1);
    d.SetInput(s1, r, 0, buf);
    std::snprintf(buf, sizeof(buf), TR("Product %u"), i + 1);
    d.SetInput(s1, r, 1, buf);
    const double bid = 23000.0 + i * 12.25;
    d.SetValue(s1, r, 2, core::Value::Number(bid));
    d.SetFormula(s1, r, 3, "C" + std::to_string(r + 1) + "+0.5");
    d.SetFormula(s1, r, 4, "ROUND((C" + std::to_string(r + 1) + "+D" + std::to_string(r + 1) + ")/2,2)");
    d.SetValue(s1, r, 5, core::Value::Number((static_cast<int>(i * 37 % 200) - 100) / 1000.0));
    d.SetValue(s1, r, 6, core::Value::Number((i + 1) * 100.0));
    std::snprintf(buf, sizeof(buf), "1729658400.%09u", static_cast<unsigned>((i * 123456789ull) % 1000000000ull));
    d.SetInput(s1, r, 7, buf);
    d.SetFormula(s1, r, 8, "E" + std::to_string(r + 1) + "*G" + std::to_string(r + 1));
  }
  auto fmt = [&](uint32_t c, const char* code) {
    const uint32_t id = d.wb_mut().styles.mut().InternString(code);
    d.ModifyStyle(s1, Rect{2, c, 41, c}, [id](model::Style& x) { x.numfmt = id; });
  };
  fmt(2, "#,##0.00");
  fmt(3, "#,##0.00");
  fmt(4, "#,##0.00");
  fmt(5, "0.0%;[Red]-0.0%");
  fmt(6, "#,##0");
  fmt(7, "[epoch][tz=+08:00]yyyy-mm-dd hh:mm:ss.000000000");
  fmt(8, "#,##0");
  d.SetColWidth(s1, 1, 1, 90);
  d.SetColWidth(s1, 7, 7, 230);
  d.SetColWidth(s1, 8, 8, 110);
  d.SetInput(s1, 43, 7, TR("Total"));
  d.SetFormula(s1, 43, 8, "SUM(I3:I42)");
  d.ModifyStyle(s1, Rect{43, 7, 43, 8}, [](model::Style& x) { x.bold = true; });
  d.SetBorders(s1, Rect{1, 0, 41, 8}, engine::BorderPreset::All, model::LineStyle::Thin, 0xFFBFBFBF);
  d.SetBorders(s1, Rect{43, 7, 43, 8}, engine::BorderPreset::TopDoubleBottom);
  const uint32_t s2 = d.AddSheet(TR("Analysis"));
  d.SetInput(s2, 0, 0, TR("Avg Trade"));
  d.SetFormula(s2, 0, 1, TR("AVERAGE(Quotes!E3:E42)"));
  d.SetInput(s2, 1, 0, TR("High"));
  d.SetFormula(s2, 1, 1, TR("MAX(Quotes!E3:E42)"));
  d.SetInput(s2, 2, 0, TR("Exact difference (ns)"));
  d.SetFormula(s2, 2, 1, TR("Quotes!H4-Quotes!H3"));
  d.SetInput(s2, 3, 0, TR("This text is long and overflows into the empty cells to the right (Excel behavior)"));
  d.AddSheet(TR("Settings"));
  d.ClearUndo();
}

void FillBigData(engine::Document& d) {
  // 1,048,576 rows x 4 columns of numbers (verifies that only the visible area is rendered)
  const uint32_t sheet = d.wb().sheets[0]->id;
  std::vector<core::Value> vals;
  const uint32_t chunk = 65536;
  for (uint32_t r0 = 0; r0 < core::kDefaultRows; r0 += chunk) {
    vals.clear();
    for (uint32_t r = r0; r < r0 + chunk; ++r) {
      for (uint32_t c = 0; c < 4; ++c) vals.push_back(core::Value::Number(static_cast<double>(r + 1) * (c + 1)));
    }
    d.SetValues(sheet, Rect{r0, 0, r0 + chunk - 1, 3}, vals, engine::kSrcPython);
  }
  d.ClearUndo();
}

}  // namespace

namespace yc::app {
std::string QueueUiScript(const std::string& script) {
  std::string err = ParseScript(script);
  ui::RequestRedrawIn(0.0);
  return err;
}
// (a paste waiting for a copy the engine still makes counts: the script's result is the pasted cells)
bool UiScriptBusy() {
  return g_script_pos < g_script.size() || g_script_wait > 0 || g_key_release != nullptr || g_click_phase != 0 || g_drag != nullptr ||
         g_app.after_copy != nullptr || !g_app.deferred.empty() || g_app.pending_commit != nullptr;
}

bool RangeWindowRect(const std::string& addr, float out[4]) {
  const model::Sheet* sh = g_app.FocusSheet();
  // Show Formulas: the sheet as its window draws it (columns twice as wide)
  std::shared_ptr<const engine::Snapshot> wide;
  if (sh && g_app.snap && g_app.Win(g_app.focus_sheet).formulas && !sh->chart_sheet) {
    wide = FormulaViewSnapshot(g_app.snap, g_app.focus_sheet);
    sh = wide->Sheet(g_app.focus_sheet);
  }
  Rect r;
  if (!sh || !core::ParseRectA1(addr, &r)) return false;
  auto& v = g_app.FocusView();
  ImRect rc;
  // Page Layout view draws the cells on its pages, not where the grid would: a click at the grid's place selected
  // another cell (ui.cell_rect("D8") clicked B1)
  if (!(v.page_view == 2 && PageLayoutRangeRect(g_app.focus_sheet, r, &rc))) {
    render::EnsureVisible(*sh, v, r.r0, r.c0);
    rc = RangeScreenRect(*sh, v, r);
  }
  const ImVec2 o = ImGui::GetMainViewport()->Pos;
  out[0] = rc.Min.x - o.x;
  out[1] = rc.Min.y - o.y;
  out[2] = rc.Max.x - o.x;
  out[3] = rc.Max.y - o.y;
  return true;
}

int LockWindow(bool locked) {
  // the class is set every frame, unlocked too: ImGui keeps a window's last class, so an unlocked window would stay
  // unable to come out of its dock
  if (!locked) {
    ImGuiWindowClass plain;
    ImGui::SetNextWindowClass(&plain);
    return 0;
  }
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoUndocking) | static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoDocking) |
                                static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoCloseButton) |
                                static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoWindowMenuButton) | static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoResize);
  ImGui::SetNextWindowClass(&wc);
  return ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
}
}  // namespace yc::app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  g_args = ParseArgs();
  if (g_args.install) {
    return app::shell::Install(g_args.silent, g_args.no_desktop ? app::shell::DesktopShortcut::Skip : app::shell::DesktopShortcut::Create);
  }
  if (g_args.uninstall) return app::shell::Uninstall(g_args.silent);

  std::wstring file;
  if (!g_args.file.empty()) {
    std::error_code ec;
    file = std::filesystem::absolute(g_args.file, ec).wstring();
    if (!g_args.no_handoff && app::shell::HandOffToFolderExe(file)) return 0;
  }
  g_app.file_path = file;
  g_app.app_dir = file.empty() ? app::shell::ExeDir() : app::shell::ParentDir(file);
  if (g_args.frames == 0 && !g_args.new_window && !g_args.hidden && app::shell::ForwardToRunningInstance(g_app.app_dir, file)) return 0;

  ImGui_ImplWin32_EnableDpiAwareness();
  const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = app::shell::kWindowClass;
  ::RegisterClassExW(&wc);
  const int w = g_args.width > 0 ? g_args.width : static_cast<int>(1440 * dpi);
  const int h = g_args.height > 0 ? g_args.height : static_cast<int>(880 * dpi);
  HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"YCGrid", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, instance, nullptr);
  g_app.hwnd = hwnd;
  ui::titlebar::Install(hwnd);
  if (!CreateDevice(hwnd, g_args.warp)) {
    ::MessageBoxW(nullptr, TRW(L"Couldn't create a Direct3D 11 device."), L"YCGrid", MB_ICONERROR);
    return 1;
  }

  // ---- Global settings (%LOCALAPPDATA%\ycgrid\settings.json) ----
  app::LoadSettings();
  app::EnableSettingsSave(!g_args.hidden);  // tests and agents never change the user's settings file
  // ... nor depend on how the user shows the ribbon: a hidden instance starts with it always shown (the user's
  // "Show tabs only" made every test that clicks a ribbon button fail)
  if (g_args.hidden && !::GetEnvironmentVariableW(L"YCGRID_SETTINGS", nullptr, 0)) {
    // ... and the user's choices that change what a test sees start as Excel's defaults
    app::Settings& c = app::Config();
    c.ribbon_mode = 0;
    c.show_quick_analysis = c.show_paste_options = c.show_insert_options = c.alert_before_overwriting = true;
    c.standard_font.clear();
    c.standard_font_size = 0;
    c.sheets_in_new_workbook = 1;
    c.show_chart_tip_names = c.show_chart_tip_values = c.calculate_before_save = true;
    // no question over a large paste or fill: tests and agents send keys over whole sheets (a test of the alert turns it on)
    c.large_operation_alert = false;
    c.format_stale_values = c.multi_threaded_calc = c.generate_getpivotdata = true;
    c.large_operation_thousands = 33554;
    c.calc_thread_count = 0;
  }
  app::ApplyCalcThreads();
  app::EnableErrorLogFile(!g_args.hidden);  // hidden instances (tests, agents) don't write the log file
  // the Trust Center: a script's instance (yc.launch, a hidden one, a timed test run) follows AutomationSecurity
  app::trust::Init(g_args.hidden, g_args.automation_security > 0 || g_args.frames > 0, g_args.automation_security);
  if (g_args.parent_pid) {
    // nobody can see or reach this window: when the process that opened it (a hidden instance's File > New, a script's
    // yc.launch(hidden=True)) ends, it ends too
    if (HANDLE parent = ::OpenProcess(SYNCHRONIZE, FALSE, g_args.parent_pid)) {
      std::thread([parent] {
        ::WaitForSingleObject(parent, INFINITE);
        ::ExitProcess(0);
      }).detach();
    } else {
      return 0;
    }
  }
  app::StartWatchdog(hwnd);  // a dump of every thread when the window stops responding, or on a crash
  app::ApplyCustomListSettings();
  {
    const app::Settings& cfg = app::Config();
    g_app.show_gridlines = g_app.default_gridlines = cfg.show_gridlines;
    g_app.show_headings = g_app.default_headings = cfg.show_headings;
    g_app.show_formula_bar = cfg.show_formula_bar;
  }

  // ---- Engine ----
  g_app.engine = std::make_unique<engine::Engine>(app::NewUserWorkbook());
  g_app.engine->SetPublishInterval(app::Config().publish_interval_ms);
  g_app.engine->Post([cfg = app::Config()](engine::Document& d) {
    d.auto_calc = cfg.auto_calc;
    d.excel_15_digit_input = cfg.excel_15_digit_input;
    d.iterate = cfg.iterative;
    d.max_iterations = cfg.max_iterations;
    d.max_change = cfg.max_change;
  });
  if (g_args.demo) g_app.engine->Post(FillDemo);
  if (g_args.big_data) g_app.engine->Post(FillBigData);
  g_app.engine->on_publish = [hwnd] { ::PostMessageW(hwnd, WM_YC_SNAPSHOT, 0, 0); };
  g_app.python = std::make_unique<app::PythonBridge>(g_app);
  engine::Document::on_python_needed = [] { g_app.python->StartHostForFormulas(); };
  g_app.engine->on_changes = [](const std::vector<engine::CellChange>& ch, const engine::Snapshot& snap) {
    if (!g_app.python) return;
    if (!ch.empty()) g_app.python->OnChanges(ch, 0);
    if (!snap.added_sheets.empty() || !snap.calc_sheets.empty()) g_app.python->OnSheetEvents(snap.added_sheets, snap.calc_sheets);
  };
  g_net = std::make_unique<app::NetFetcher>(g_app.engine.get());
  g_app.engine->SetNet(g_net.get());
  g_images = std::make_unique<app::ImageCache>(hwnd);
  g_app.engine->Start();
  g_app.snap = g_app.engine->Current();
  g_app.active_sheet = g_app.focus_sheet = g_app.snap->wb.sheets[0]->id;
  SyncWindows(g_app);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  // Ctrl+Tab / Ctrl+F6 switch YCGrid's windows as Excel's do (NextWindow), not ImGui's panes
  ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
  ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
  // --hidden: every window stays inside the main window (no separate OS windows), so screenshots show everything
  if (g_args.hidden) io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
  if (g_args.hidden) {
    // text boxes and Copy buttons share the hidden instance's own clipboard (the user's stays as it was)
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) { app::SetClipboardTextUtf8(nullptr, text ? text : ""); };
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
      static std::string buf;
      buf = app::ClipboardTextUtf8(nullptr);
      return buf.c_str();
    };
  }
  io.ConfigDpiScaleFonts = true;
  io.ConfigDpiScaleViewports = true;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  io.ConfigDockingWithShift = false;
  app::InitDockTabs();
  ui::ApplyImGuiStyle(dpi);
  ImGui::GetStyle().FontScaleDpi = dpi;
  ImPlot::StyleColorsLight();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_gfx.device, g_gfx.ctx);
  if (!ui::LoadFonts(kIconFontResource)) {
    ::MessageBoxW(nullptr, TRW(L"No usable font was found in the Windows Fonts folder (Segoe UI, Tahoma or Arial)."), L"YCGrid", MB_ICONERROR);
    return 1;
  }

  app::RegisterCommands(g_app);
  app::RegisterPythonCommands(g_app);
  app::RegisterErrorCommands(g_app);
  app::RegisterObjectCommands(g_app);
  app::RegisterRuleCommands(g_app);
  app::RegisterMiscCommands(g_app);
  app::RegisterDataCommands(g_app);
  app::RegisterWhatIfCommands(g_app);
  app::RegisterAnalysisCommands(g_app);
  app::RegisterLatencyCommands(g_app);
  app::RegisterScreenshotCommands(g_app);
  app::RegisterAlertCommands(g_app);
  app::RegisterSafetyCommands(g_app);
  app::RegisterReplayCommands(g_app);
  app::RegisterSpellingCommands(g_app);
  app::RegisterProtectCommands(g_app);
  app::RegisterTableCommands(g_app);
  app::RegisterIllustrationCommands(g_app);
  app::RegisterPrintCommands(g_app);
  app::RegisterPivotCommands(g_app);
  app::RegisterStyleCommands(g_app);
  app::RegisterBorderDrawCommands(g_app);
  app::RegisterUiCustomCommands(g_app);
  app::RegisterWindowCommands(g_app);
  app::RegisterLinkCommands(g_app);
  app::RegisterNameCommands(g_app);
  app::RegisterCellCommands(g_app);
  app::RegisterPasteCommands(g_app);
  app::RegisterQuickAnalysisCommands(g_app);
  app::trust::RegisterCommands(g_app);
  app::RegisterFunctionLibrary(g_app);
  g_app.headless = g_args.hidden;  // before anything can ask a question: Workbook_Open's MsgBox, file dialogs
  g_app.python->SetStartupOpen(!file.empty());  // Workbooks.Open in another window waits until the file is read
  g_app.python->StartListener();
  app::SafetyOnOpen(g_app, false);
  ui::ribbon::BuildDefaultTabs(&g_app.show_gridlines, &g_app.show_headings, &g_app.show_formula_bar);
  app::UpdateFunctionLibraryMenu();
  ui::ribbon::St().backstage_content = [](int page, const ImRect& r) { app::DrawBackstageContent(g_app, page, r); };
  ui::ribbon::St().about_menu = [] { app::DrawAboutMenu(g_app); };
  ::DragAcceptFiles(hwnd, TRUE);
  bool opened = false;
  if (!file.empty()) {
    // Workbooks.Open(..., Password) in another window passes the password in the environment (cleared at once)
    std::string password;
    if (const DWORD n = ::GetEnvironmentVariableW(L"YCGRID_OPEN_PASSWORD", nullptr, 0); n > 0) {
      std::wstring pw(n, L'\0');
      pw.resize(::GetEnvironmentVariableW(L"YCGRID_OPEN_PASSWORD", pw.data(), n));
      password = app::ToUtf8(pw);
      ::SetEnvironmentVariableW(L"YCGRID_OPEN_PASSWORD", nullptr);
    }
    std::string open_error;
    opened = app::OpenWorkbook(g_app, file, password, true, &open_error);
    if (!opened && g_app.file_path == file && g_app.engine->Current()->wb.path.empty() && g_app.password_path.empty()) {
      // the file couldn't be read: the blank workbook isn't that file, so Save mustn't write over it, and the window
      // that started this one (Workbooks.Open) learns the error instead of taking the blank workbook for the file
      g_app.file_path.clear();
      g_app.opened_from.clear();
      app::UpdateWindowTitle(g_app);
      g_app.python->NoteOpenFailed(file, open_error.empty() ? std::string("The workbook couldn't be opened") : open_error);
    }
    g_app.python->SetStartupOpen(false);
    g_app.python->WriteInstanceFile();
    if (opened && g_args.as_new) {
      // a new, unsaved workbook (Allow Edit Ranges > Paste permissions information into a new workbook)
      std::error_code rm_ec;
      std::filesystem::remove(file, rm_ec);
      g_app.file_path.clear();
      g_app.opened_from.clear();
      g_app.engine->Call([](engine::Document& d) {
        d.wb_mut().path.clear();
        d.wb_mut().dirty = true;  // a new workbook, not saved yet
      });
    }
  }
  app::StartPythonForWorkbook(g_app);
  app::trust::StartupDone();  // later opens from the File menu are a person's (Trust Center)
  app::UpdateWindowTitle(g_app);
  if (g_args.ribbon_tab >= 0) ui::ribbon::St().selected_tab = g_args.ribbon_tab;
  if (g_args.backstage) ui::ribbon::St().backstage = true;
  if (g_args.collapsed) ui::ribbon::St().collapsed = true;
  if (g_args.expand_rows) {
    const uint32_t sheet = g_app.active_sheet;
    g_app.engine->Post([sheet](engine::Document& d) { d.SetRowLimit(sheet, 100663296); });
  }
  g_app.engine->Sync();
  g_app.snap = g_app.engine->Current();
  SyncWindows(g_app);
  if (g_args.top_row >= 0) {
    auto& v = g_app.Win(g_app.active_sheet).view;
    const model::Sheet* sh = g_app.snap->Sheet(g_app.active_sheet);
    v.v.first = std::min<int64_t>(g_args.top_row, sh->row_limit - 1);
    render::SelectCell(v, static_cast<uint32_t>(v.v.first), 0);
  }
  if (g_args.float_sheet > 0 && g_args.float_sheet < static_cast<int>(g_app.snap->wb.sheets.size())) {
    g_app.Win(g_app.snap->wb.sheets[static_cast<size_t>(g_args.float_sheet)]->id).floating = true;
    SyncWindows(g_app);
  }
  if (!g_args.type.empty()) {
    const std::string text = app::ToUtf8(g_args.type);
    const uint32_t sheet = g_app.active_sheet;
    g_app.engine->Call([&](engine::Document& d) { return d.SetInput(sheet, 0, 0, text); });
  }
  if (g_args.new_container) ui::Commands().Execute("NewContainer");
  if (!g_args.keys.empty())
    if (const std::string e = ParseScript(app::ToUtf8(g_args.keys)); !e.empty()) app::ReportError("keys", "--keys", e);
  if (!g_args.hidden) app::shell::RegisterInstance(hwnd, g_app.app_dir);  // a hidden instance never receives the user's files
  if (g_args.frames == 0) app::shell::CheckAssociationMoved(g_args.silent);

  // without a file, reuse the last default layout (main window position etc.); with a file, use the file's layout
  const bool restored = opened ? g_app.layout_restored : ((g_args.frames == 0 || g_args.load_layout) && app::LoadLayout(g_app, true));
  if (g_args.hidden) ::ShowWindow(hwnd, SW_HIDE);
  else if (!restored) ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(hwnd);

  // ---- Main loop: event-driven redraw (CPU close to 0 when idle) ----
  bool quit = false;
  int frames_left = 3;
  double deadline = -1.0;
  double next_frame = 0;  // perf_hz pacing
  while (!quit) {
    const bool continuous = g_args.frames > 0 || g_app.perf_continuous || g_app.perf_hz > 0;
    if (g_app.perf_hz > 0) {
      // a fixed frame rate (benchmarks of a display of that rate, also in a hidden window): sleep, then spin the last ms
      const double now = NowMs(), period = 1000.0 / g_app.perf_hz;
      if (next_frame <= 0 || now - next_frame > period * 4) next_frame = now;
      while (NowMs() < next_frame - 1.5) ::Sleep(1);
      while (NowMs() < next_frame) ::SwitchToThread();
      next_frame += period;
    }
    if (!continuous && frames_left <= 0) {
      DWORD timeout = INFINITE;
      if (deadline >= 0.0) timeout = static_cast<DWORD>(std::max(0.0, (deadline - ImGui::GetTime()) * 1000.0));
      ::MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      frames_left = 1;
    }
    MSG msg;
    bool got = false;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quit = true;
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
      got = true;
    }
    if (quit) break;
    if (got) frames_left = std::max(frames_left, 3);
    if (::IsIconic(hwnd)) {
      // minimised: a frame every 250 ms keeps the UI queue moving (OnUi from scripts used to time out after 30 s)
      static double last_iconic_frame = 0.0;
      frames_left = 0;
      deadline = -1.0;
      if (NowMs() - last_iconic_frame < 250.0) continue;
      last_iconic_frame = NowMs();
    }
    RenderFrame();
    --frames_left;
    deadline = ui::ConsumeRedrawDeadline();
    if (deadline >= 0.0 && deadline <= ImGui::GetTime() + 0.001) frames_left = std::max(frames_left, 1);
    // caret blinking while editing
    if (g_app.edit.active && deadline < 0) deadline = ImGui::GetTime() + 0.5;
    if (g_args.frames > 0 && g_frame_count >= g_args.frames) break;
  }

  app::FinishBackgroundSave(g_app, true);  // a save still writing completes before YCGrid ends
  if (g_app.python) g_app.python->CloseUi();  // Python requests waiting for the UI thread fail now (no 30 s waits)
  if (g_args.save_layout) app::SaveLayout(g_app);
  if (!g_args.save_as.empty()) {
    // tests: Save As before exiting (no dialog)
    g_app.file_path = g_args.save_as;
    app::SaveWorkbook(g_app, false);
    g_app.engine->Sync();
  }
  app::StopWatchdog();  // shutting down takes the UI thread away from its messages
  WriteReport();
  app::AutoRecoverOnExit();
  g_app.engine->Post([](engine::Document&) { engine::Document::on_python_needed = nullptr; });  // the engine thread calls it
  g_app.engine->Sync();
  g_app.python.reset();
  if (g_net) g_net->Shutdown();
  g_app.engine->Stop();
  g_images.reset();  // textures go before the D3D device
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
  DestroyDevice();
  if (::IsWindow(hwnd)) ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, instance);
  return 0;
}

// declared in an anonymous namespace above; defined at file scope so other files can link to them
namespace yc::app {
ImRect CellsScreenRect(const model::Sheet& sh, const render::GridView& v, const Rect& r) { return RangeScreenRect(sh, v, r); }

bool ImageSize(const std::string& bytes, int* w, int* h) {
  std::vector<uint8_t> px;
  UINT iw = 0, ih = 0;
  // callable from any thread (the Python bridge too): COM is set up here when the thread hasn't
  const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool ok = YcDecodeImage(bytes, &px, &iw, &ih);
  if (SUCCEEDED(co)) ::CoUninitialize();
  if (!ok) return false;
  *w = static_cast<int>(iw);
  *h = static_cast<int>(ih);
  return true;
}

bool DecodeImageRgba(const std::string& bytes, std::vector<uint8_t>* px, int* w, int* h) {
  UINT iw = 0, ih = 0;
  const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool ok = YcDecodeImage(bytes, px, &iw, &ih);
  if (SUCCEEDED(co)) ::CoUninitialize();
  *w = static_cast<int>(iw);
  *h = static_cast<int>(ih);
  return ok;
}

ImTextureID TextureFromRgba(const uint8_t* px, int w, int h) {
  if (!YcDevice() || !px || w <= 0 || h <= 0) return ImTextureID{};
  D3D11_TEXTURE2D_DESC d{};
  d.Width = static_cast<UINT>(w);
  d.Height = static_cast<UINT>(h);
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{px, static_cast<UINT>(w) * 4, 0};
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(YcDevice()->CreateTexture2D(&d, &sd, &tex)) || !tex) return ImTextureID{};
  ID3D11ShaderResourceView* srv = nullptr;
  YcDevice()->CreateShaderResourceView(tex, nullptr, &srv);
  tex->Release();
  return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(srv));
}

ImTextureID TextureFromImage(const std::string& bytes, int* w, int* h) {
  std::vector<uint8_t> px;
  UINT iw = 0, ih = 0;
  if (!YcDevice() || !YcDecodeImage(bytes, &px, &iw, &ih)) return ImTextureID{};
  D3D11_TEXTURE2D_DESC d{};
  d.Width = iw;
  d.Height = ih;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{px.data(), iw * 4, 0};
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(YcDevice()->CreateTexture2D(&d, &sd, &tex)) || !tex) return ImTextureID{};
  ID3D11ShaderResourceView* srv = nullptr;
  YcDevice()->CreateShaderResourceView(tex, nullptr, &srv);
  tex->Release();
  if (w) *w = static_cast<int>(iw);
  if (h) *h = static_cast<int>(ih);
  return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(srv));
}

void ReleaseTexture(ImTextureID t) {
  if (auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<uintptr_t>(t))) srv->Release();
}
}  // namespace yc::app

// this frame's Escape closed a menu or flyout (objects_ui.cpp keeps the selected object then)
bool yc::app::EscClosedPopup() { return g_esc_closed_popup; }
