// SPDX-License-Identifier: Apache-2.0
#include "core/paths.h"

#include <algorithm>
#include <iterator>

namespace yc::core {

namespace {

bool IsSep(char c) { return c == '\\' || c == '/'; }

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// the length of `root` (without a trailing separator) when it starts `path` at a folder boundary, else 0
size_t MatchLength(std::string_view path, std::string_view root) {
  while (!root.empty() && IsSep(root.back())) root.remove_suffix(1);
  if (root.empty() || root.size() > path.size()) return 0;
  for (size_t i = 0; i < root.size(); ++i) {
    const char a = path[i], b = root[i];
    if (IsSep(a) && IsSep(b)) continue;
    if (Lower(a) != Lower(b)) return 0;
  }
  if (root.size() < path.size() && !IsSep(path[root.size()])) return 0;
  return root.size();
}

}  // namespace

std::string AbbreviatePath(std::string_view path, const std::vector<PathRoot>& roots) {
  size_t best = 0;
  const PathRoot* hit = nullptr;
  for (const auto& r : roots) {
    const size_t n = MatchLength(path, r.second);
    if (n > best) {
      best = n;
      hit = &r;
    }
  }
  if (!hit) return std::string(path);
  return hit->first + std::string(path.substr(best));
}

namespace {

wchar_t LowerW(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

bool EqualsNoCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const wchar_t x = a[i] == L'/' ? L'\\' : a[i], y = b[i] == L'/' ? L'\\' : b[i];
    if (LowerW(x) != LowerW(y)) return false;
  }
  return true;
}

bool EndsWithNoCase(std::wstring_view s, std::wstring_view tail) {
  return s.size() >= tail.size() && EqualsNoCase(s.substr(s.size() - tail.size()), tail);
}

std::wstring Trim(std::wstring_view s) {
  while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'"')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'"')) s.remove_suffix(1);
  return std::wstring(s);
}

std::wstring Join(std::wstring_view dir, std::wstring_view name) {
  std::wstring out(dir);
  while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
  if (out.empty()) return {};
  out += L'\\';
  out += name;
  return out;
}

struct Edition {
  const wchar_t* folder;  // the installers' folder name
  const wchar_t* exe;
  const wchar_t* cmd;    // bin\<cmd> on PATH
  const wchar_t* scoop;  // Scoop's app name
};

}  // namespace

std::wstring CommandProgram(std::wstring_view command) {
  while (!command.empty() && (command.front() == L' ' || command.front() == L'\t')) command.remove_prefix(1);
  if (command.empty()) return {};
  if (command.front() == L'"') {
    const size_t end = command.find(L'"', 1);
    return std::wstring(command.substr(1, end == std::wstring_view::npos ? std::wstring_view::npos : end - 1));
  }
  // unquoted: up to the program's extension when one is there (paths with spaces), else up to the first space
  for (std::wstring_view ext : {L".exe", L".cmd", L".bat"}) {
    for (size_t at = 0; at + ext.size() <= command.size(); ++at) {
      if (EqualsNoCase(command.substr(at, ext.size()), ext) &&
          (at + ext.size() == command.size() || command[at + ext.size()] == L' ' || command[at + ext.size()] == L'\t')) {
        return std::wstring(command.substr(0, at + ext.size()));
      }
    }
  }
  return std::wstring(command.substr(0, command.find_first_of(L" \t")));
}

bool IsProgramPath(std::wstring_view path) {
  return EndsWithNoCase(path, L".exe") || EndsWithNoCase(path, L".cmd") || EndsWithNoCase(path, L".bat");
}

std::vector<std::wstring> VsCodeCandidates(const VsCodeSearch& in) {
  std::vector<std::wstring> out;
  auto add = [&](std::wstring p) {
    if (p.empty()) return;
    for (const auto& q : out) {
      if (EqualsNoCase(q, p)) return;
    }
    out.push_back(std::move(p));
  };
  std::vector<std::wstring> path_dirs;
  for (size_t at = 0; at <= in.path.size();) {
    const size_t end = std::min(in.path.find(L';', at), in.path.size());
    if (std::wstring d = Trim(std::wstring_view(in.path).substr(at, end - at)); !d.empty()) path_dirs.push_back(std::move(d));
    at = end + 1;
  }
  static const Edition kEditions[] = {{L"Microsoft VS Code", L"Code.exe", L"code.cmd", L"vscode"},
                                      {L"Microsoft VS Code Insiders", L"Code - Insiders.exe", L"code-insiders.cmd", L"vscode-insiders"}};
  for (size_t e = 0; e < std::size(kEditions); ++e) {
    const Edition& ed = kEditions[e];
    for (const auto& d : path_dirs) add(Join(d, ed.cmd));
    add(Join(Join(in.local_app_data, L"Programs"), Join(ed.folder, ed.exe)));
    add(Join(in.program_files, Join(ed.folder, ed.exe)));
    add(Join(in.program_files_x86, Join(ed.folder, ed.exe)));
    const std::wstring scoop_app = Join(Join(L"apps", ed.scoop), Join(L"current", ed.exe));
    add(Join(in.scoop, scoop_app));
    add(Join(Join(in.user_profile, L"scoop"), scoop_app));
    add(Join(Join(in.program_data, L"scoop"), scoop_app));
    if (e == 0) {
      // what Windows lists: the program itself, or the folder it is in
      for (const auto& r : in.registered) {
        const std::wstring p = CommandProgram(r);
        add(IsProgramPath(p) ? p : Join(Trim(r), ed.exe));
      }
    }
  }
  return out;
}

}  // namespace yc::core
