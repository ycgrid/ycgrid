# SPDX-License-Identifier: Apache-2.0
# YcImGui.cmake - builds imgui (docking) + implot into one static library, yc_imgui
#
# Rules:
# - Submodule contents are never modified; every customization lives in src/ui/yc_imconfig.h (through IMGUI_USER_CONFIG).
# - IMGUI_USER_CONFIG is a PUBLIC definition for every consumer, so every TU sees the same configuration (ODR).
# - Third-party sources don't get our warning level (/W0); our own code keeps /W4 /WX.

set(_im "${YC_IMGUI_DIR}")
set(_ip "${YC_IMPLOT_DIR}")

add_library(yc_imgui STATIC
  "${_im}/imgui.cpp"
  "${_im}/imgui_draw.cpp"
  "${_im}/imgui_tables.cpp"
  "${_im}/imgui_widgets.cpp"
  "${_im}/backends/imgui_impl_win32.cpp"
  "${_im}/backends/imgui_impl_dx11.cpp"
  "${_ip}/implot.cpp"
  "${_ip}/implot_items.cpp"
  "${PROJECT_SOURCE_DIR}/src/ui/yc_imgui_check.cpp"
  "${PROJECT_SOURCE_DIR}/src/ui/yc_ui_items.cpp"
)
if(YC_IMGUI_DEMOS)
  target_sources(yc_imgui PRIVATE "${_im}/imgui_demo.cpp" "${_ip}/implot_demo.cpp")
  target_compile_definitions(yc_imgui PUBLIC YC_IMGUI_DEMOS=1)
endif()

target_include_directories(yc_imgui SYSTEM PUBLIC
  "${_im}"
  "${_im}/backends"
  "${_ip}"
)
target_include_directories(yc_imgui PUBLIC "${PROJECT_SOURCE_DIR}/src/ui")

target_compile_definitions(yc_imgui PUBLIC
  "IMGUI_USER_CONFIG=\"yc_imconfig.h\""
  YC_EXPECTED_IMGUI_VERSION_NUM=${YC_EXPECTED_IMGUI_VERSION_NUM}
  YC_EXPECTED_IMPLOT_VERSION_NUM=${YC_EXPECTED_IMPLOT_VERSION_NUM}
)

if(YC_USE_FREETYPE)
  find_package(freetype CONFIG REQUIRED)
  target_sources(yc_imgui PRIVATE "${_im}/misc/freetype/imgui_freetype.cpp")
  target_include_directories(yc_imgui SYSTEM PUBLIC "${_im}/misc/freetype")
  target_compile_definitions(yc_imgui PUBLIC YC_USE_FREETYPE=1)
  target_link_libraries(yc_imgui PUBLIC freetype)
endif()

target_link_libraries(yc_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

if(MSVC)
  # third-party sources: warnings off; yc_imgui_check.cpp is our file and stays strict.
  set_source_files_properties(
    "${_im}/imgui.cpp" "${_im}/imgui_draw.cpp" "${_im}/imgui_tables.cpp" "${_im}/imgui_widgets.cpp"
    "${_im}/imgui_demo.cpp" "${_im}/backends/imgui_impl_win32.cpp" "${_im}/backends/imgui_impl_dx11.cpp"
    "${_im}/misc/freetype/imgui_freetype.cpp"
    "${_ip}/implot.cpp" "${_ip}/implot_items.cpp" "${_ip}/implot_demo.cpp"
    PROPERTIES COMPILE_OPTIONS "/W0")
endif()
