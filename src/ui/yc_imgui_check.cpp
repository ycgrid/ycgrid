// SPDX-License-Identifier: Apache-2.0
// yc_imgui_check.cpp - compile-time checks of the third-party versions (the last of three locks)
//
// CMake reads the expected values from third_party/deps.lock and passes them as macros;
// even if someone skips CMake's configure-time check, compilation fails here.
#include "imgui.h"
#include "implot.h"

#include <cstdio>
#include <cstdlib>

#ifndef YC_EXPECTED_IMGUI_VERSION_NUM
#error "YC_EXPECTED_IMGUI_VERSION_NUM is not defined: build through CMake (cmake/YcImGui.cmake)"
#endif
#ifndef IMGUI_HAS_DOCK
#error "third_party/imgui is not the docking branch"
#endif

static_assert(IMGUI_VERSION_NUM == YC_EXPECTED_IMGUI_VERSION_NUM,
              "the imgui version doesn't match third_party/deps.lock");
static_assert(IMPLOT_VERSION_NUM == YC_EXPECTED_IMPLOT_VERSION_NUM,
              "the implot version doesn't match third_party/deps.lock");
static_assert(sizeof(ImDrawIdx) == 4, "ImDrawIdx must be 32-bit (see yc_imconfig.h)");
static_assert(sizeof(ImWchar) == 4, "ImWchar must be 32-bit (IMGUI_USE_WCHAR32)");

#if !defined(_WIN32)
#error "YCGrid only supports Windows"
#endif
#include <windows.h>

// ImGui's checks that failed since the start: a release build goes on after one, so tests read this count
// (ui.state()["imgui_asserts"]) to catch what a debug build stops on
int yc_imgui_assert_count = 0;

void yc_imgui_assert_failed(const char* expr, const char* file, int line) {
  ++yc_imgui_assert_count;
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "[imgui assert] %s\n  at %s:%d\n", expr, file, line);
  ::OutputDebugStringA(buf);
  std::fputs(buf, stderr);
  if (::IsDebuggerPresent()) {
    __debugbreak();
  }
#if defined(_DEBUG)
  std::abort();
#endif
}
