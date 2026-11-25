// SPDX-License-Identifier: Apache-2.0
// yc_imconfig.h - compile-time configuration of Dear ImGui / ImPlot (injected into every TU through IMGUI_USER_CONFIG)
//
// Never edit third_party/imgui/imconfig.h; every setting goes here (rule 17).
// This file affects the ABI of ImGui structures, so every TU must agree; CMake passes it as a PUBLIC definition.
#pragma once

// + - * operators for ImVec2 / ImVec4 (must be defined before imgui.h; defined here so every TU agrees).
#define IMGUI_DEFINE_MATH_OPERATORS

// 32-bit indices: a single draw list of the grid or of charts with many points exceeds 65,535 vertices.
#define ImDrawIdx unsigned int

// catch API drift early: anything marked obsolete fails to compile.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMPLOT_DISABLE_OBSOLETE_FUNCTIONS

// ImGui's embedded fonts (ProggyClean, ProggyForever) are left out: YCGrid draws text with the fonts in Windows' Fonts
// folder (ui/fonts/fonts.cpp), so the embedded ones would never show.
#define IMGUI_DISABLE_DEFAULT_FONT

// Dear ImGui's test engine hooks: tests ask where each labelled control was drawn (yc_ui_items.cpp, ui.items) to click
// it as a person does. The hooks only run while g.TestEngineHookItems is on, which a test turns on.
#define IMGUI_ENABLE_TEST_ENGINE

// CJK extension planes (U+20000 and above) and some symbols need 32-bit characters.
#define IMGUI_USE_WCHAR32

// rasterize with FreeType (better hinting, closer to how Windows renders text). Controlled by the CMake option YC_USE_FREETYPE.
#if defined(YC_USE_FREETYPE)
#define IMGUI_ENABLE_FREETYPE
#endif

// ImPlot only instantiates the numeric types we use, to cut compile time.
// the ImPlot demo uses every integer type, so there is no restriction when the demo is built (YC_IMGUI_DEMOS).
#if !defined(YC_IMGUI_DEMOS)
#define IMPLOT_CUSTOM_NUMERIC_TYPES (float)(double)
#endif

// assertions go to our own handler (records the location, then breaks); YC_ASSERT is hooked up later.
void yc_imgui_assert_failed(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : yc_imgui_assert_failed(#_EXPR, __FILE__, __LINE__))
