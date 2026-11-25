// SPDX-License-Identifier: Apache-2.0
#include "app/shell_integration.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cwctype>
#include <filesystem>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace yc::app::shell {
namespace {

constexpr wchar_t kProgId[] = L"YCGrid.Workbook";
constexpr wchar_t kClasses[] = L"Software\\Classes\\";
constexpr wchar_t kAppKey[] = L"Software\\YCGrid";
constexpr wchar_t kPropHash[] = L"YCGrid.AppDirHash";

bool SetValue(const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LONG rc = ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

std::wstring GetValue(const std::wstring& subkey, const wchar_t* name) {
  wchar_t buf[2048];
  DWORD size = sizeof(buf);
  if (::RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
  return buf;
}

// YCGrid.lnk in one of the user's folders ("" when Windows doesn't give the folder)
std::wstring ShortcutIn(REFKNOWNFOLDERID folder) {
  wchar_t* dir = nullptr;
  std::wstring out;
  if (SUCCEEDED(::SHGetKnownFolderPath(folder, 0, nullptr, &dir)) && dir) out = std::wstring(dir) + L"\\YCGrid.lnk";
  if (dir) ::CoTaskMemFree(dir);
  return out;
}

std::wstring StartMenuShortcut() { return ShortcutIn(FOLDERID_Programs); }
// the user's own desktop (FOLDERID_Desktop follows it when OneDrive or a policy moved it); no administrator needed
std::wstring DesktopShortcutPath() { return ShortcutIn(FOLDERID_Desktop); }

bool SameText(const std::wstring& a, const std::wstring& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::towlower(a[i]) != std::towlower(b[i])) return false;
  }
  return true;
}

bool SameFile(const std::wstring& a, const std::wstring& b) {
  auto info = [](const std::wstring& p, BY_HANDLE_FILE_INFORMATION& out) {
    HANDLE h = ::CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = ::GetFileInformationByHandle(h, &out);
    ::CloseHandle(h);
    return ok != FALSE;
  };
  BY_HANDLE_FILE_INFORMATION ia{}, ib{};
  if (!info(a, ia) || !info(b, ib)) return false;
  return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber && ia.nFileIndexHigh == ib.nFileIndexHigh &&
         ia.nFileIndexLow == ib.nFileIndexLow;
}

uint64_t HashDir(const std::wstring& dir) {
  uint64_t h = 1469598103934665603ULL;  // FNV-1a
  for (wchar_t ch : dir) {
    wchar_t c = static_cast<wchar_t>(std::towlower(ch));
    if (c == L'/') c = L'\\';
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::wstring MutexName(const std::wstring& app_dir) {
  wchar_t buf[64];
  swprintf_s(buf, L"Local\\YCGrid.Instance.%016llx", static_cast<unsigned long long>(HashDir(app_dir)));
  return buf;
}

void Message(bool silent, const std::wstring& text, UINT flags = MB_ICONINFORMATION) {
  if (!silent) ::MessageBoxW(nullptr, text.c_str(), L"YCGrid", MB_OK | flags);
}

}  // namespace

bool CreateShortcut(const std::wstring& lnk, const std::wstring& target) {
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool ok = false;
  IShellLinkW* link = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(ParentDir(target).c_str());
    link->SetDescription(L"YCGrid");
    link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
      ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
      file->Release();
    }
    link->Release();
  }
  if (SUCCEEDED(init)) ::CoUninitialize();
  return ok;
}

std::wstring ShortcutTarget(const std::wstring& lnk) {
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::wstring out;
  IShellLinkW* link = nullptr;
  if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
      wchar_t path[MAX_PATH * 4] = L"";
      // SLGP_RAWPATH: the path as written, not resolved (a moved YCGrid's old shortcut still names ycgrid.exe)
      if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)) &&
          SUCCEEDED(link->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH))) {
        out = path;
      }
      file->Release();
    }
    link->Release();
  }
  if (SUCCEEDED(init)) ::CoUninitialize();
  return out;
}

namespace {
// a shortcut that starts a ycgrid.exe (any folder's: an old place's too)
bool StartsYcGrid(const std::wstring& lnk) {
  const std::wstring target = ShortcutTarget(lnk);
  const size_t slash = target.find_last_of(L"\\/");
  return SameText(slash == std::wstring::npos ? target : target.substr(slash + 1), L"ycgrid.exe");
}
}  // namespace

bool RemoveYcGridShortcut(const std::wstring& lnk) {
  std::error_code ec;
  if (lnk.empty() || !std::filesystem::exists(lnk, ec)) return true;
  if (!StartsYcGrid(lnk)) return false;
  return ::DeleteFileW(lnk.c_str()) != FALSE;
}

std::wstring ExePath() {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  return std::wstring(buf, n);
}

std::wstring ParentDir(const std::wstring& path) {
  const size_t pos = path.find_last_of(L"\\/");
  return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring ExeDir() { return ParentDir(ExePath()); }

// The install, uninstall and file association messages are English whatever the UI language (no TRW), as
// install.cmd and uninstall.cmd are: they show before any settings are read.
int Install(bool silent, DesktopShortcut desktop) {
  const std::wstring exe = ExePath();
  const std::wstring cls = kClasses;
  const std::wstring open_cmd = L"\"" + exe + L"\" \"%1\"";
  bool ok = true;
  ok &= SetValue(cls + L".ycg", nullptr, kProgId);
  ok &= SetValue(cls + L".ycg", L"Content Type", L"application/x-ycgrid-workbook");
  ok &= SetValue(cls + L".ycg\\OpenWithProgids", kProgId, L"");
  ok &= SetValue(cls + kProgId, nullptr, L"YCGrid Workbook");
  ok &= SetValue(cls + kProgId, L"FriendlyTypeName", L"YCGrid Workbook");
  ok &= SetValue(cls + kProgId + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
  ok &= SetValue(cls + kProgId + L"\\shell\\open\\command", nullptr, open_cmd);
  ok &= SetValue(cls + L"Applications\\ycgrid.exe", L"FriendlyAppName", L"YCGrid");
  ok &= SetValue(cls + L"Applications\\ycgrid.exe\\SupportedTypes", L".ycg", L"");
  ok &= SetValue(cls + L"Applications\\ycgrid.exe\\shell\\open\\command", nullptr, open_cmd);
  ok &= SetValue(kAppKey, L"AssociatedExe", exe);
  const std::wstring lnk = StartMenuShortcut();
  if (!lnk.empty()) CreateShortcut(lnk, exe);
  bool on_desktop = false;
  if (const std::wstring dl = DesktopShortcutPath(); !dl.empty()) {
    std::error_code ec;
    const bool ours = std::filesystem::exists(dl, ec) && StartsYcGrid(dl);
    if (desktop == DesktopShortcut::Create || (desktop == DesktopShortcut::UpdateIfThere && ours)) {
      on_desktop = CreateShortcut(dl, exe);
    }
  }
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  if (!ok) {
    Message(silent, L"Writing to the registry failed; the file association may be incomplete.", MB_ICONWARNING);
    return 2;
  }
  // if the user picked another program for .ycg in Windows before, UserChoice is hash-protected and can't be changed by a program
  const std::wstring choice =
      GetValue(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.ycg\\UserChoice", L"ProgId");
  if (!choice.empty() && choice != kProgId) {
    Message(silent,
            L"YCGrid is registered, but Windows is currently set to use \"" + choice +
                L"\" to open .ycg files.\nGo to Settings → Apps → Default apps and change .ycg to YCGrid.",
            MB_ICONWARNING);
  } else {
    Message(silent, std::wstring(L"Done: double-clicking a .ycg file opens it in YCGrid.\n\n") +
                        (on_desktop ? L"YCGrid's shortcut is on the desktop and in the Start menu.\n\n"
                                    : L"YCGrid's shortcut is in the Start menu.\n\n") +
                        exe);
  }
  return 0;
}

int Uninstall(bool silent) {
  const std::wstring cls = kClasses;
  if (GetValue(cls + L".ycg", nullptr) == kProgId) {
    ::RegDeleteTreeW(HKEY_CURRENT_USER, (cls + L".ycg").c_str());
  } else {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, (cls + L".ycg\\OpenWithProgids").c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
      ::RegDeleteValueW(key, kProgId);
      ::RegCloseKey(key);
    }
  }
  ::RegDeleteTreeW(HKEY_CURRENT_USER, (cls + kProgId).c_str());
  ::RegDeleteTreeW(HKEY_CURRENT_USER, (cls + L"Applications\\ycgrid.exe").c_str());
  ::RegDeleteTreeW(HKEY_CURRENT_USER, kAppKey);
  const std::wstring lnk = StartMenuShortcut();
  if (!lnk.empty()) ::DeleteFileW(lnk.c_str());
  RemoveYcGridShortcut(DesktopShortcutPath());
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  Message(silent, L"Removed YCGrid's file association and its shortcuts (desktop, Start menu).");
  return 0;
}

std::wstring FolderExeFor(const std::wstring& file) {
  std::error_code ec;
  const std::wstring full = std::filesystem::absolute(file, ec).wstring();
  const std::wstring dir = ParentDir(full);
  if (const std::wstring local = dir + L"\\ycgrid.exe"; std::filesystem::exists(local, ec)) return local;
  // <YCGrid>\examples\<name>\<name>.ycg: the YCGrid the example ships with
  const std::wstring examples = ParentDir(dir);
  const size_t slash = examples.find_last_of(L"\\/");
  if (slash != std::wstring::npos && SameText(examples.substr(slash + 1), L"examples")) {
    if (const std::wstring owner = ParentDir(examples) + L"\\ycgrid.exe"; std::filesystem::exists(owner, ec)) return owner;
  }
  return {};
}

bool HandOffToFolderExe(const std::wstring& file) {
  std::error_code ec;
  const std::wstring full = std::filesystem::absolute(file, ec).wstring();
  const std::wstring local = FolderExeFor(full);
  if (local.empty() || SameFile(local, ExePath())) return false;
  std::wstring cmd = L"\"" + local + L"\" --no-handoff \"" + full + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = ParentDir(full);
  if (!::CreateProcessW(local.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) return false;
  ::AllowSetForegroundWindow(pi.dwProcessId);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

bool ForwardToRunningInstance(const std::wstring& app_dir, const std::wstring& file) {
  HANDLE m = ::OpenMutexW(SYNCHRONIZE, FALSE, MutexName(app_dir).c_str());
  if (!m) return false;
  ::CloseHandle(m);
  struct Ctx {
    uint64_t hash;
    HWND found;
  } ctx{HashDir(app_dir), nullptr};
  ::EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t cls[64];
        if (::GetClassNameW(hwnd, cls, 64) && wcscmp(cls, kWindowClass) == 0 &&
            reinterpret_cast<uint64_t>(::GetPropW(hwnd, kPropHash)) == c->hash) {
          c->found = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  if (!ctx.found) return false;
  DWORD pid = 0;
  ::GetWindowThreadProcessId(ctx.found, &pid);
  ::AllowSetForegroundWindow(pid);
  COPYDATASTRUCT cds{kCopyDataOpenFile, static_cast<DWORD>((file.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(file.c_str())};
  DWORD_PTR result = 0;
  ::SendMessageTimeoutW(ctx.found, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 2000, &result);
  return true;
}

void RegisterInstance(HWND hwnd, const std::wstring& app_dir) {
  static HANDLE mutex = nullptr;
  if (!mutex) mutex = ::CreateMutexW(nullptr, FALSE, MutexName(app_dir).c_str());
  ::SetPropW(hwnd, kPropHash, reinterpret_cast<HANDLE>(HashDir(app_dir)));
}

void CheckAssociationMoved(bool silent) {
  const std::wstring assoc = GetValue(kAppKey, L"AssociatedExe");
  if (assoc.empty() || silent) return;
  std::error_code ec;
  if (std::filesystem::exists(assoc, ec)) return;
  const std::wstring msg = std::wstring(L"The YCGrid used to open .ycg files no longer exists:\n") + assoc + L"\n\nUse this YCGrid instead?\n" + ExePath();
  if (::MessageBoxW(nullptr, msg.c_str(), L"YCGrid", MB_YESNO | MB_ICONQUESTION) == IDYES) Install(true, DesktopShortcut::UpdateIfThere);
}

}  // namespace yc::app::shell
