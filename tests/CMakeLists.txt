# SPDX-License-Identifier: Apache-2.0
find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(yc_unit_tests
  unit/test_listener.cpp
  unit/test_coverage_text.cpp
  unit/test_coverage_msgpack.cpp
  unit/test_coverage_rules.cpp
  unit/test_coverage_misc.cpp
  unit/test_coverage_i18n.cpp
  unit/test_coverage_bind.cpp
  unit/test_coverage_pivot.cpp
  unit/test_coverage_whatif.cpp
  unit/test_coverage_names.cpp
  unit/test_coverage_online.cpp
  unit/test_coverage_xlsx.cpp
  unit/test_ref_registry.cpp
  unit/test_model_roundtrip.cpp
  unit/test_fuzz_found.cpp
  unit/test_coverage_pivot_doc.cpp
  unit/test_coverage_analysis.cpp
  unit/test_coverage_rules_doc.cpp
  unit/test_coverage_misc2.cpp
  unit/test_coverage_print.cpp
  unit/test_coverage_datatools.cpp
  unit/test_coverage_shapes.cpp
  unit/test_int128.cpp
  unit/test_value.cpp
  unit/test_utf8.cpp
  unit/test_functions_basic.cpp
  unit/test_engine_basic.cpp
  unit/test_engine_objects.cpp
  unit/test_geometry.cpp
  unit/test_paths.cpp
  unit/test_py_source.cpp
  unit/test_shortcuts.cpp
  unit/test_model.cpp
  unit/test_parser.cpp
  unit/test_eval.cpp
  unit/test_numfmt.cpp
  unit/test_document.cpp
  unit/test_io.cpp
  unit/test_rules.cpp
  unit/test_names.cpp
  unit/test_i18n.cpp
  unit/test_parallel_calc.cpp
  unit/test_fin.cpp
  unit/test_fn2.cpp
  unit/test_whatif.cpp
  unit/test_chart.cpp
  unit/test_bind.cpp
  unit/test_analysis.cpp
  unit/test_safety.cpp
  unit/test_xlsx.cpp
  unit/test_rich.cpp
  unit/test_theme.cpp
  unit/test_fill.cpp
  unit/test_external.cpp
  unit/test_dist.cpp
  unit/test_spill.cpp
  unit/test_lambda.cpp
  unit/test_3d.cpp
  unit/test_data.cpp
  unit/test_protect.cpp
  unit/test_table.cpp
  unit/test_shapes.cpp
  unit/test_print.cpp
  unit/test_pivot.cpp
  unit/test_parity_pivots.cpp
  unit/test_undo.cpp
  unit/test_parity_cells.cpp
  unit/test_parity_formulas.cpp
  unit/test_parity_data.cpp
  unit/test_parity_insert.cpp
  unit/test_parity_formatting.cpp
  unit/test_parity_charts.cpp
  unit/test_round2_excel_b.cpp
  unit/test_round2_excel_a.cpp
  unit/test_round3_format.cpp
  unit/test_round2_events.cpp
  unit/test_round3_trust.cpp
  unit/test_round3_data.cpp
  unit/test_round3_layout.cpp
  unit/test_round3_formulas.cpp
  unit/test_round3_pivots.cpp
  unit/test_round3_cells.cpp
  unit/test_round3_charts_b.cpp
  unit/test_round3_chartsA.cpp
  unit/test_round3_insert.cpp
  unit/test_round5_sheets.cpp
  unit/test_round5_nav.cpp
  unit/test_round6_fill.cpp
  unit/test_round6_refs.cpp
  unit/test_round6_input.cpp
  unit/test_round6_tables.cpp
  unit/test_round6_calc.cpp
  unit/test_round6_typed.cpp
  unit/test_typed_measured.cpp
  unit/test_round7_formats.cpp
  unit/test_round8_fixes.cpp
  unit/test_round8_mathdate.cpp
  unit/test_round8_pivot_rules.cpp
  unit/test_round8_tables.cpp
  unit/test_round8_readonly.cpp
  unit/test_round8_files.cpp
  unit/test_excel_measured.cpp
  unit/test_excel_measured_formulas.cpp
  unit/test_measured_moves.cpp
  unit/test_array_self.cpp
  unit/test_round8_calc.cpp
  unit/test_round8_data_tail.cpp
  unit/test_round8_formula_tail.cpp
  unit/test_content_dump.cpp
  unit/test_basics_fill.cpp
  unit/test_basics_refs.cpp
  unit/test_basics_dims.cpp
  unit/test_excel_gaps.cpp
  unit/test_basics_core.cpp
  unit/test_basics_merge.cpp
  unit/test_round9_typing.cpp
  unit/test_round9_data.cpp
  unit/test_round9_bigdata.cpp
  unit/test_round9_files.cpp
)
target_link_libraries(yc_unit_tests PRIVATE yc_engine yc_io yc_ui nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
yc_target_defaults(yc_unit_tests)
if(MSVC)
  # an incremental link of this large test binary with its manifest resource fails with LNK1104 (MSVC 14.51)
  target_link_options(yc_unit_tests PRIVATE /INCREMENTAL:NO)
endif()

include(Catch)
catch_discover_tests(yc_unit_tests)
target_compile_definitions(yc_unit_tests PRIVATE "YC_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")

# the Document under random operations, in-process: Undo / Redo, .ycg round trips and full recalculation checked
# after every step (tests/fuzz/doc_fuzz.cpp; check_parallel's doc_fuzz job runs it)
add_executable(yc_doc_fuzz fuzz/doc_fuzz.cpp)
target_link_libraries(yc_doc_fuzz PRIVATE yc_engine yc_io yc_ui)
yc_target_defaults(yc_doc_fuzz)
