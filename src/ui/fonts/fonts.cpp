// SPDX-License-Identifier: Apache-2.0
#include "fonts/fonts.h"

#include <windows.h>
#include <dwrite.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>

#pragma comment(lib, "dwrite.lib")

namespace yc::ui {
namespace {

std::string FontsDir() {
  wchar_t* path = nullptr;
  std::string out = "C:\\Windows\\Fonts\\";
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &path)) && path) {
    out = std::filesystem::path(path).string() + "\\";
  }
  if (path) ::CoTaskMemFree(path);
  return out;
}

bool Exists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec);
}

// load a "Latin font + CJK fallback" combination; files that don't exist are skipped. ImGui's embedded fonts are
// compiled out (yc_imconfig.h), so a missing Latin font falls back to other fonts every Windows has; nullptr = none.
ImFont* AddCombo(const std::string& dir, const char* latin, const char* cjk, int cjk_index) {
  ImGuiIO& io = ImGui::GetIO();
  ImFont* font = nullptr;
  for (const char* file : {latin, "tahoma.ttf", "arial.ttf"}) {
    if (Exists(dir + file)) font = io.Fonts->AddFontFromFileTTF((dir + file).c_str(), 0.0f);
    if (font) break;
  }
  if (cjk && Exists(dir + cjk)) {
    ImFontConfig cfg;
    cfg.MergeMode = font != nullptr;
    cfg.FontNo = cjk_index;
    ImFont* added = io.Fonts->AddFontFromFileTTF((dir + cjk).c_str(), 0.0f, &cfg);
    if (!font) font = added;
  }
  // symbol fallback (e.g. check marks, crosses, arrows)
  if (font && Exists(dir + "seguisym.ttf")) {
    ImFontConfig cfg;
    cfg.MergeMode = true;
    io.Fonts->AddFontFromFileTTF((dir + "seguisym.ttf").c_str(), 0.0f, &cfg);
  }
  return font;
}

}  // namespace

Fonts& F() {
  static Fonts fonts;
  return fonts;
}

bool LoadFonts(int icon_resource_id) {
  const std::string dir = FontsDir();
  Fonts& f = F();
  // msjh.ttc: index 0 = Microsoft JhengHei, 1 = Microsoft JhengHei UI
  f.ui = AddCombo(dir, "segoeui.ttf", "msjh.ttc", 1);
  f.ui_bold = AddCombo(dir, "segoeuib.ttf", "msjhbd.ttc", 1);
  f.grid = AddCombo(dir, "calibri.ttf", "msjh.ttc", 0);

  // Fluent icon font: the resource memory stays valid until the process exits, so the atlas doesn't take ownership.
  HRSRC res = ::FindResourceW(nullptr, MAKEINTRESOURCEW(icon_resource_id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
  if (res) {
    HGLOBAL handle = ::LoadResource(nullptr, res);
    void* data = handle ? ::LockResource(handle) : nullptr;
    const DWORD size = ::SizeofResource(nullptr, res);
    if (data && size > 0) {
      ImFontConfig cfg;
      cfg.FontDataOwnedByAtlas = false;
      f.icons = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), 0.0f, &cfg);
    }
  }
  if (!f.icons) f.icons = f.ui;
  ImGui::GetIO().FontDefault = f.ui;
  return f.ui != nullptr;
}

const std::vector<std::string>& SystemFontFamilies() {
  static std::vector<std::string> families;
  static bool loaded = false;
  if (loaded) return families;
  loaded = true;

  IDWriteFactory* factory = nullptr;
  if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&factory)))) {
    return families;
  }
  IDWriteFontCollection* collection = nullptr;
  if (SUCCEEDED(factory->GetSystemFontCollection(&collection, FALSE)) && collection) {
    const UINT32 count = collection->GetFontFamilyCount();
    families.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
      IDWriteFontFamily* family = nullptr;
      if (FAILED(collection->GetFontFamily(i, &family)) || !family) continue;
      IDWriteLocalizedStrings* names = nullptr;
      if (SUCCEEDED(family->GetFamilyNames(&names)) && names) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        names->FindLocaleName(L"zh-tw", &index, &exists);
        if (!exists) names->FindLocaleName(L"en-us", &index, &exists);
        if (!exists) index = 0;
        UINT32 len = 0;
        if (SUCCEEDED(names->GetStringLength(index, &len))) {
          std::wstring w(len + 1, L'\0');
          if (SUCCEEDED(names->GetString(index, w.data(), len + 1))) {
            w.resize(len);
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            if (!s.empty() && s[0] != '@') families.push_back(std::move(s));  // names starting with '@' are vertical fonts
          }
        }
        names->Release();
      }
      family->Release();
    }
    collection->Release();
  }
  factory->Release();
  std::sort(families.begin(), families.end());
  families.erase(std::unique(families.begin(), families.end()), families.end());
  return families;
}

}  // namespace yc::ui
