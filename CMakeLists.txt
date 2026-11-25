# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)

# single source of the version number: VERSION in the repo root (scripts\package.cmd reads it too)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" YC_VERSION LIMIT_COUNT 1)
project(YCGrid VERSION ${YC_VERSION} LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "YCGrid only supports Windows (rule: the product ships for Windows only)")
endif()

option(YC_USE_FREETYPE     "Rasterize fonts with FreeType (via vcpkg)" ON)
option(YC_IMGUI_DEMOS      "Build the ImGui / ImPlot demo windows" ON)
option(YC_ALLOW_DEP_DRIFT  "Only warn when third-party versions don't match deps.lock (for upgrade tests)" OFF)
option(YC_WARNINGS_AS_ERRORS "Treat warnings as errors in our own code" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # no C++20 modules: skip the per-file dependency scan

# static CRT: no separate VC++ runtime installation needed for distribution (§13.16 portable distribution).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  # every TU (third-party included) reads sources and the execution character set as UTF-8.
  add_compile_options(/utf-8 /Zc:__cplusplus /permissive- /MP)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(YcDeps)
include(YcImGui)

# strict warnings for our own targets.
function(yc_target_defaults target)
  if(MSVC)
    # /bigobj: the Python bridge's request table outgrew the default object file's 65,279 sections (C1128)
    target_compile_options(${target} PRIVATE /W4 /bigobj)
    if(YC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  endif()
endfunction()

option(YC_BUILD_TESTS "Build the unit tests" ON)

add_subdirectory(src)
add_subdirectory(apps/ycgrid)
# Visual Studio solution: ycgrid is the default startup project, debugging opens the demo data with --demo
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ycgrid)
set_target_properties(ycgrid PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--demo")
if(YC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
