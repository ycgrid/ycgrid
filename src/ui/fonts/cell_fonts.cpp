// SPDX-License-Identifier: Apache-2.0
#include "fonts/cell_fonts.h"

#include "fonts/fonts.h"

#include <windows.h>
#include <dwrite.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace yc::ui {
namespace {

struct Key {
  std::string family;
  bool bold, italic;
  bool operator==(const Key& o) const { return family == o.family && bold == o.bold && italic == o.italic; }
};
struct KeyHash {
  size_t operator()(const Key& k) const { return std::hash<std::string>()(k.family) ^ (k.bold ? 0x9e37 : 0) ^ (k.italic ? 0x7f4a1 : 0); }
};

std::unordered_map<Key, ImFont*, KeyHash> g_loaded;
std::vector<Key> g_pending;

std::wstring Wide(const std::string& s) {
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string Narrow(const std::wstring& w) {
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// DirectWrite: family name + bold/italic -> font file path and .ttc index
bool FindFontFile(const std::string& family, bool bold, bool italic, std::string* path, int* index) {
  IDWriteFactory* factory = nullptr;
  if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory)))) return false;
  bool ok = false;
  IDWriteFontCollection* coll = nullptr;
  if (SUCCEEDED(factory->GetSystemFontCollection(&coll, FALSE)) && coll) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    coll->FindFamilyName(Wide(family).c_str(), &idx, &exists);
    IDWriteFontFamily* fam = nullptr;
    if (exists && SUCCEEDED(coll->GetFontFamily(idx, &fam)) && fam) {
      IDWriteFont* font = nullptr;
      if (SUCCEEDED(fam->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font)) &&
          font) {
        IDWriteFontFace* face = nullptr;
        if (SUCCEEDED(font->CreateFontFace(&face)) && face) {
          UINT32 nfiles = 1;
          IDWriteFontFile* file = nullptr;
          if (SUCCEEDED(face->GetFiles(&nfiles, &file)) && file) {
            const void* key = nullptr;
            UINT32 key_size = 0;
            IDWriteFontFileLoader* loader = nullptr;
            IDWriteLocalFontFileLoader* local = nullptr;
            if (SUCCEEDED(file->GetReferenceKey(&key, &key_size)) && SUCCEEDED(file->GetLoader(&loader)) && loader &&
                SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), reinterpret_cast<void**>(&local))) && local) {
              UINT32 len = 0;
              if (SUCCEEDED(local->GetFilePathLengthFromKey(key, key_size, &len))) {
                std::wstring w(len + 1, L'\0');
                if (SUCCEEDED(local->GetFilePathFromKey(key, key_size, w.data(), len + 1))) {
                  w.resize(len);
                  *path = Narrow(w);
                  *index = static_cast<int>(face->GetIndex());
                  ok = true;
                }
              }
              local->Release();
            }
            if (loader) loader->Release();
            file->Release();
          }
          face->Release();
        }
        font->Release();
      }
      fam->Release();
    }
    coll->Release();
  }
  factory->Release();
  return ok;
}

ImFont* Load(const Key& k) {
  std::string path;
  int index = 0;
  if (!FindFontFile(k.family, k.bold, k.italic, &path, &index)) return nullptr;
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.FontNo = index;
  ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), 0.0f, &cfg);
  if (!f) return nullptr;
  // CJK fallback (bold uses the bold face)
  std::string cjk_path;
  int cjk_index = 0;
  if (FindFontFile("Microsoft JhengHei", k.bold, false, &cjk_path, &cjk_index) && cjk_path != path) {
    ImFontConfig m;
    m.MergeMode = true;
    m.FontNo = cjk_index;
    io.Fonts->AddFontFromFileTTF(cjk_path.c_str(), 0.0f, &m);
  }
  std::string sym;
  int sym_index = 0;
  if (FindFontFile("Segoe UI Symbol", false, false, &sym, &sym_index)) {
    ImFontConfig m;
    m.MergeMode = true;
    m.FontNo = sym_index;
    io.Fonts->AddFontFromFileTTF(sym.c_str(), 0.0f, &m);
  }
  return f;
}

}  // namespace

ImFont* CellFont(const std::string& family, bool bold, bool italic) {
  Key k{family.empty() ? std::string("Calibri") : family, bold, italic};
  auto it = g_loaded.find(k);
  if (it != g_loaded.end()) return it->second ? it->second : (bold ? F().ui_bold : F().grid);
  if (std::find(g_pending.begin(), g_pending.end(), k) == g_pending.end()) g_pending.push_back(k); // lesson-ok: find-push the fonts waiting to load
  return bold ? F().ui_bold : F().grid;
}

bool LoadPendingCellFonts() {
  if (g_pending.empty()) return false;
  for (const Key& k : g_pending) {
    ImFont* f = Load(k);
    // the default font (Calibri regular) reuses the grid font already loaded, so it isn't loaded twice
    if (!f && k.family == "Calibri" && !k.bold && !k.italic) f = F().grid;
    g_loaded[k] = f;
  }
  g_pending.clear();
  return true;
}

}  // namespace yc::ui
