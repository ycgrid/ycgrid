# SPDX-License-Identifier: Apache-2.0
# engine-side libraries (no ImGui dependency; dependency direction core <- model <- formula <- engine, PLAN §16)

add_library(yc_core STATIC
  core/int128.cpp
  core/value.cpp
  core/decimal.cpp
  core/geometry.cpp
  core/i18n.cpp
  core/parallel.cpp
  core/paths.cpp
  core/py_source.cpp
)
target_include_directories(yc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
  target_link_libraries(yc_core PUBLIC shell32)  # CommandLineToArgvW (--lang)
endif()
yc_target_defaults(yc_core)

add_library(yc_model STATIC
  model/dim_index.cpp
  model/table.cpp
  model/table_style.cpp
  model/style.cpp
  model/rich_text.cpp
  model/theme.cpp
  model/cell_styles.cpp
  model/sheet.cpp
  model/workbook.cpp
)
target_link_libraries(yc_model PUBLIC yc_core)
yc_target_defaults(yc_model)

add_library(yc_formula STATIC
  formula/parser.cpp
  formula/collate.cpp
  formula/eval.cpp
  formula/functions.cpp
  formula/func_help.cpp
  formula/datetime.cpp
  formula/numfmt.cpp
  formula/fn_fin.cpp
  formula/fn_dist.cpp
  formula/fn_array.cpp
  formula/fn_lambda.cpp
  formula/fn_math.cpp
  formula/fn_stat.cpp
  formula/fn_logic.cpp
  formula/fn_lookup.cpp
  formula/fn_text.cpp
  formula/fn_date.cpp
  formula/fn_info.cpp
  formula/fn_yc.cpp
  formula/fn_eng.cpp
  formula/fn_db.cpp
  formula/fn_fin2.cpp
  formula/fn_math2.cpp
  formula/fn_group.cpp
  formula/fn_text2.cpp
  formula/fn_online.cpp
  formula/fn_cube.cpp
)
find_package(pugixml CONFIG REQUIRED)  # FILTERXML
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(yc_formula PUBLIC yc_model PRIVATE pugixml::pugixml nlohmann_json::nlohmann_json)
yc_target_defaults(yc_formula)

add_library(yc_engine STATIC
  engine/custom_lists.cpp
  engine/external.cpp
  engine/geo.cpp
  engine/bind.cpp
  engine/tape.cpp
  engine/analysis.cpp
  engine/document_analysis.cpp
  engine/dep_index.cpp
  engine/engine.cpp
  engine/document.cpp
  engine/document_format.cpp
  engine/document_struct.cpp
  engine/document_data.cpp
  engine/document_protect.cpp
  engine/document_bind.cpp
  engine/document_safety.cpp
  engine/document_table.cpp
  engine/document_shapes.cpp
  engine/shape_geometry.cpp
  engine/sparkline.cpp
  engine/page_layout.cpp
  engine/pivot_engine.cpp
  engine/document_pivot.cpp
  engine/document_rules.cpp
  engine/document_names.cpp
  engine/document_trace.cpp
  engine/error_check.cpp
  engine/navigate.cpp
  engine/rule_eval.cpp
  engine/document_clip.cpp
  engine/format_match.cpp
  engine/document_whatif.cpp
  engine/document_datatools.cpp
  engine/chart_layout.cpp
)
target_link_libraries(yc_engine PUBLIC yc_formula)
yc_target_defaults(yc_engine)

# ---- Files (.ycg, CSV) ----
find_package(zstd CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
add_library(yc_io STATIC
  io/msgpack.cpp
  io/ycg.cpp
  io/content_dump.cpp
  io/csv.cpp
  io/crypto.cpp
  io/xlsx.cpp
)
target_link_libraries(yc_io PUBLIC yc_engine PRIVATE bcrypt ZLIB::ZLIB pugixml::pugixml nlohmann_json::nlohmann_json odbc32)
if(TARGET zstd::libzstd_static)
  target_link_libraries(yc_io PRIVATE zstd::libzstd_static)
else()
  target_link_libraries(yc_io PRIVATE zstd::libzstd)
endif()
yc_target_defaults(yc_io)

# ---- UI (ImGui) ----
find_package(nlohmann_json CONFIG REQUIRED)
add_library(yc_ui STATIC
  ui/theme/theme.cpp
  ui/fonts/fonts.cpp
  ui/fonts/cell_fonts.cpp
  ui/frame/frame_context.cpp
  ui/commands/command_registry.cpp
  ui/ribbon/ribbon.cpp
  ui/titlebar/titlebar.cpp
  ui/dock/dock_layout.cpp
  render/grid/grid.cpp
  render/grid/grid_edit.cpp
  app/shell_integration.cpp
)
yc_target_defaults(yc_ui)
target_include_directories(yc_ui PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/ui
  ${CMAKE_CURRENT_SOURCE_DIR}/render
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(yc_ui PUBLIC yc_engine yc_io yc_imgui nlohmann_json::nlohmann_json dwrite)
target_compile_definitions(yc_ui PUBLIC "YC_VERSION_STRING=\"${PROJECT_VERSION}\"")
