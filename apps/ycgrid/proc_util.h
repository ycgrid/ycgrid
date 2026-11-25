// SPDX-License-Identifier: Apache-2.0
// proc_util.h - starting child processes that inherit only the handles meant for them
//
// CreateProcess with bInheritHandles = TRUE hands every inheritable handle of YCGrid to the child - including the
// output pipe of another child being started at the same moment. A pipe whose write end leaked into a long-running
// process (a data feed) never reports its end, and whoever reads it waits forever. With an explicit handle list each
// child inherits only its own pipe.
#pragma once

#include <windows.h>

#include <vector>

namespace yc::app {

inline BOOL CreateProcessInheriting(const wchar_t* app, wchar_t* cmdline, const std::vector<HANDLE>& inherit, DWORD flags,
                                    void* env, const wchar_t* cwd, STARTUPINFOW& si, PROCESS_INFORMATION* pi) {
  SIZE_T size = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> buf(size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
  if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &size)) return FALSE;
  std::vector<HANDLE> handles = inherit;
  const bool listed = !handles.empty() &&
                      ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), handles.size() * sizeof(HANDLE), nullptr, nullptr);
  STARTUPINFOEXW six{};
  six.StartupInfo = si;
  six.StartupInfo.cb = sizeof(six);
  six.lpAttributeList = attrs;
  const BOOL ok = ::CreateProcessW(app, cmdline, nullptr, nullptr, listed ? TRUE : FALSE, flags | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0), env, cwd,
                                   listed ? &six.StartupInfo : &si, pi);
  ::DeleteProcThreadAttributeList(attrs);
  return ok;
}

}  // namespace yc::app
