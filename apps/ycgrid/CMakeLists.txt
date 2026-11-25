# SPDX-License-Identifier: Apache-2.0
# YCGrid main program: the output file name is ycgrid.exe
set(YC_ICON_PATH "${PROJECT_SOURCE_DIR}/resources/ycgrid.ico")
set(YC_FLUENT_TTF "${PROJECT_SOURCE_DIR}/third_party/fluent-icons/FluentSystemIcons-Regular.ttf")
configure_file(resources.rc.in "${CMAKE_CURRENT_BINARY_DIR}/resources.rc" @ONLY)
# the resource compiler doesn't report the files it embeds: rebuild the resources when the icon or the icon font changes
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/resources.rc" PROPERTIES OBJECT_DEPENDS "${YC_ICON_PATH};${YC_FLUENT_TTF}")

add_executable(ycgrid WIN32
  main.cpp
  commands_setup.cpp
  sheet_actions.cpp
  cells_ui.cpp
  dialogs.cpp
  chart_window.cpp
  layout_io.cpp
  file_ops.cpp
  python_bridge.cpp
  events_ui.cpp
  python_ui.cpp
  objects_ui.cpp
  chart_data_ui.cpp
  chart_type_ui.cpp
  range_box_ui.cpp
  python_bridge_objects.cpp
  python_bridge_mcp.cpp
  python_bridge_rules.cpp
  python_bridge_data.cpp
  replay.cpp
  replay_ui.cpp
  analysis_ui.cpp
  latency_ui.cpp
  screenshot_ui.cpp
  npy.cpp
  dataview_ui.cpp
  error_log.cpp
  dock_tabs.cpp
  topic_subs.cpp
  watchdog.cpp
  notebook_ui.cpp
  alerts_ui.cpp
  code_complete.cpp
  plot_theme.cpp
  safety_ui.cpp
  trading_ui.cpp
  figures_ui.cpp
  python_feeds.cpp
  rules_ui.cpp
  misc_ui.cpp
  data_ui.cpp
  whatif_ui.cpp
  datatools_ui.cpp
  spelling.cpp
  protect_ui.cpp
  tables_ui.cpp
  illustrations_ui.cpp
  insert_gallery.cpp
  print_render.cpp
  print_ui.cpp
  shape_effects.cpp
  custom_ribbon.cpp
  pivot_ui.cpp
  styles_ui.cpp
  border_draw.cpp
  editing_ui.cpp
  paste_ui.cpp
  quick_analysis.cpp
  ui_custom.cpp
  windows_ui.cpp
  links_ui.cpp
  names_ui.cpp
  settings.cpp
  trust.cpp
  net.cpp
  ycgrid.manifest
  "${CMAKE_CURRENT_BINARY_DIR}/resources.rc"
)
yc_target_defaults(ycgrid)
target_include_directories(ycgrid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
find_package(pugixml CONFIG REQUIRED)
target_link_libraries(ycgrid PRIVATE yc_ui winspool winhttp imm32 dbghelp pugixml::pugixml)

# after building, lay out folders you can run directly: Debug -> dist_debug, other configurations -> dist_release (cmake/YcStageDist.cmake)
option(YC_STAGE_DIST "Lay out dist_debug / dist_release after building" ON)
if(YC_STAGE_DIST)
  add_custom_command(TARGET ycgrid POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR}
            "-DDEST=${PROJECT_SOURCE_DIR}/$<IF:$<CONFIG:Debug>,dist_debug,dist_release>"
            "-DEXE=$<TARGET_FILE:ycgrid>" "-DPDB=$<TARGET_PDB_FILE:ycgrid>"
            -P ${PROJECT_SOURCE_DIR}/cmake/YcStageDist.cmake
    COMMENT "Staging dist (double-click ycgrid.exe to use it)"
    VERBATIM)
  # runs on every build (dist is updated even when only Python, the demo or the skill changed and ycgrid.exe wasn't relinked)
  add_custom_target(yc_dist ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR}
            "-DDEST=${PROJECT_SOURCE_DIR}/$<IF:$<CONFIG:Debug>,dist_debug,dist_release>"
            "-DEXE=$<TARGET_FILE:ycgrid>" "-DPDB=$<TARGET_PDB_FILE:ycgrid>"
            -P ${PROJECT_SOURCE_DIR}/cmake/YcStageDist.cmake
    DEPENDS ycgrid
    COMMENT "Staging dist"
    VERBATIM)
endif()
