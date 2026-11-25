# SPDX-License-Identifier: Apache-2.0
"""Generate src/ui/icons/yc_icons.h from the codepoint map of Fluent UI System Icons.

Usage (repo root): python tools/gen_icons.py
- Only the icons listed in ICONS are written, so the header doesn't carry ten thousand codepoints.
- Unknown names make the script fail, so the UI never shows blank icons.
"""
from __future__ import annotations

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
MAP = ROOT / "third_party" / "fluent-icons" / "FluentSystemIcons-Regular.json"
OUT = ROOT / "src" / "ui" / "icons" / "yc_icons.h"

# constant name -> Fluent icon name (without the ic_fluent_ prefix and the _regular suffix)
ICONS: dict[str, str] = {
    # Developer, Formulas, Data
    "Code": "code_24",
    "Play": "play_24",
    "Reload": "arrow_clockwise_24",
    "Console": "window_console_20",
    "DesignMode": "ruler_24",
    "Properties": "text_bullet_list_square_24",
    "Calculator": "calculator_24",
    "CalculateFull": "calculator_multiple_24",
    "SortAscending": "text_sort_ascending_24",
    "SortDescending": "text_sort_descending_24",
    "FilterReapply": "filter_sync_24",
    "ShowDetail": "add_square_24",
    "HideDetail": "subtract_square_24",
    "ConvertToRange": "table_switch_24",
    "NameManager": "tag_24",
    "Filter": "filter_24",
    "FlashFill": "flash_24",
    "AdvancedFilter": "options_24",
    "Consolidate": "layer_24",
    "WhatIf": "branch_fork_24",
    "GoalSeek": "target_arrow_24",
    "DataTable": "table_lightning_24",
    "Scenario": "arrow_routing_24",
    "Solver": "beaker_24",
    "Forecast": "arrow_trending_lines_24",
    "Validation": "checkmark_square_24",
    "Eye": "eye_24",
    "Panel": "panel_right_24",
    # title bar and QAT
    "Save": "save_20",
    "Undo": "arrow_undo_20",
    "Redo": "arrow_redo_20",
    "Search": "search_20",
    "Minimize": "subtract_16",
    "Maximize": "maximize_16",
    "Restore": "square_multiple_16",
    "Close": "dismiss_16",
    "ChevronDown": "chevron_down_16",
    "ChevronUp": "chevron_up_16",
    "ChevronLeft": "chevron_left_16",
    "ChevronRight": "chevron_right_16",
    "CaretDown": "caret_down_16",
    "CaretUp": "caret_up_16",
    "CaretLeft": "caret_left_16",
    "CaretRight": "caret_right_16",
    "Add": "add_16",
    "Grid": "grid_20",
    # Home -> Clipboard
    "Paste": "clipboard_paste_24",
    "PasteSmall": "clipboard_paste_20",
    "Cut": "cut_20",
    "Copy": "copy_20",
    "FormatPainter": "paint_brush_20",
    # Home -> Font
    "Bold": "text_bold_20",
    "Italic": "text_italic_20",
    "Underline": "text_underline_20",
    "Strikethrough": "text_strikethrough_20",
    "FontIncrease": "font_increase_20",
    "FontDecrease": "font_decrease_20",
    "FontGroup": "text_font_size_24",
    "BorderBottom": "border_bottom_20",
    "BorderAll": "border_all_20",
    "BorderNone": "border_none_20",
    "BorderOutside": "border_outside_20",
    "BorderTop": "border_top_20",
    "BorderLeft": "border_left_20",
    "BorderRight": "border_right_20",
    "BorderThick": "border_outside_thick_20",
    "FillColor": "paint_bucket_20",
    "FontColor": "text_color_20",
    # Home -> Alignment
    "AlignTop": "align_top_20",
    "AlignMiddle": "align_center_vertical_20",
    "AlignBottom": "align_bottom_20",
    "AlignLeft": "text_align_left_20",
    "AlignCenter": "text_align_center_20",
    "AlignRight": "text_align_right_20",
    "IndentDecrease": "text_indent_decrease_20",
    "IndentIncrease": "text_indent_increase_20",
    "WrapText": "text_wrap_20",
    "Merge": "table_cells_merge_20",
    "AlignGroup": "text_align_center_24",
    # Home -> Number
    "Currency": "money_20",
    "Percent": "text_percent_20",
    "Comma": "comma_20",
    "DecimalIncrease": "decimal_arrow_right_20",
    "DecimalDecrease": "decimal_arrow_left_20",
    "NumberGroup": "text_number_format_24",
    # Home -> Styles, Cells, Editing
    "CondFormat": "data_bar_horizontal_24",
    "FormatAsTable": "table_settings_24",
    "CellStyles": "paint_brush_sparkle_24",
    "InsertCells": "table_insert_row_24",
    "DeleteCells": "table_delete_row_24",
    "FormatCells": "table_cell_edit_24",
    "AutoSum": "autosum_20",
    "FillDown": "arrow_down_20",
    "Clear": "eraser_20",
    "SortFilter": "arrow_sort_24",
    "FindSelect": "search_24",
    "Clipboard": "clipboard_paste_24",
    "StylesGroup": "paint_brush_sparkle_24",
    "CellsGroup": "table_24",
    "EditGroup": "search_24",
    "ControlsGroup": "button_20",
    "DisplayGroup": "eye_24",
    "WindowGroup": "window_24",
    "TestGroup": "apps_24",
    # Insert
    "Table": "table_24",
    "ChartColumn": "data_bar_vertical_24",
    "ChartLine": "data_line_24",
    "ChartPie": "data_pie_24",
    "ChartScatter": "data_scatter_24",
    "ChartRadar": "hexagon_24",
    "ChartWaterfall": "data_waterfall_24",
    "ChartFunnel": "data_funnel_24",
    "ChartHistogram": "data_histogram_24",
    "ChartBoxWhisker": "data_whisker_24",
    "ChartTreemap": "data_treemap_24",
    "ChartSunburst": "data_sunburst_24",
    "ChartCombo": "chart_multiple_24",
    "Link": "link_24",
    "Comment": "comment_24",
    "CommentNew": "comment_add_24",
    "Spelling": "text_proofing_tools_24",
    "GroupBox": "group_24",
    "Timeline": "timeline_24",
    "ViewNormal": "grid_24",
    "Themes": "design_ideas_24",
    "SplitWindow": "split_horizontal_24",
    "NewWindow": "window_new_24",
    "ArrangeAll": "board_24",
    "SwitchWindows": "window_multiple_24",
    "ThemeColors": "color_24",
    "ThemeFonts": "text_font_24",
    "ViewPageBreak": "document_page_break_24",
    "ViewPageLayout": "document_one_page_24",
    "CommentDelete": "comment_dismiss_24",
    "CommentPrevious": "comment_arrow_left_24",
    "CommentNext": "comment_arrow_right_24",
    "CommentsPane": "comment_multiple_24",
    "CommentConvert": "comment_note_24",
    "Button": "button_20",
    "ComboBox": "text_box_settings_20",
    # View
    "Zoom": "zoom_in_24",
    "Zoom100": "zoom_fit_24",
    "NewContainer": "window_apps_24",
    "Freeze": "table_freeze_row_24",
    "LayoutSave": "save_24",
    "LayoutLoad": "folder_open_24",
    "LayoutReset": "arrow_reset_24",
    "Rows": "table_stack_below_24",
    "Data": "database_24",
    "Demo": "apps_24",
    "Chart": "data_line_24",
    "SelectData": "data_bar_vertical_add_24",
    "RangePick": "table_arrow_up_20",
    # File (Backstage)
    "Back": "arrow_left_24",
    "Info": "info_24",
    "New": "document_add_24",
    "Open": "folder_open_24",
    "SaveAs": "save_edit_24",
    "Options": "settings_24",
    "Formula": "math_formula_20",
    "Check": "checkmark_16",
    "Dismiss": "dismiss_16",
    "DialogLauncher": "arrow_down_right_16",
    # Data tools, outline and formula auditing
    "RemoveDuplicates": "table_dismiss_24",
    "TextToColumns": "split_vertical_24",
    "Group": "group_24",
    "Ungroup": "group_dismiss_24",
    "TracePrecedents": "arrow_previous_24",
    "TraceDependents": "arrow_next_24",
    "RemoveArrows": "eraser_24",
    "EvaluateFormula": "math_formula_24",
    "WatchWindow": "glasses_24",
    # protection (Review tab), filters
    "ProtectSheet": "shield_lock_24",
    "ProtectWorkbook": "document_lock_24",
    "AllowEditRanges": "person_lock_24",
    "FilterClear": "filter_dismiss_24",
    # tables, illustrations, sparklines
    "TableDesign": "table_settings_24",
    "Picture": "image_add_24",
    "Shapes": "shapes_24",
    "TextBox": "textbox_24",
    "SparkLine": "data_trending_24",
    "SparkColumn": "data_bar_vertical_ascending_24",
    "SparkWinLoss": "data_usage_24",
    # printing and page layout
    "Print": "print_24",
    "Pdf": "document_pdf_24",
    "Margins": "document_margins_24",
    "Orientation": "orientation_24",
    "PaperSize": "resize_24",
    "PrintArea": "page_fit_24",
    "PageBreak": "document_page_break_24",
    "PrintTitles": "text_header_1_lines_24",
    # more controls
    "CheckBox": "checkbox_checked_24",
    "OptionButton": "radio_button_24",
    "ToggleButton": "toggle_left_24",
    "TextBoxCtl": "text_field_24",
    "LabelCtl": "text_t_24",
    "ListBox": "list_24",
    "SpinButton": "arrow_sort_up_24",
    "ScrollBar": "navigation_24",
    "ImageCtl": "image_24",
    # pivot tables
    "PivotTable": "table_simple_24",
    "Refresh": "arrow_sync_24",
    # trading: recordings and replay
    "Record": "record_24",
    "Pause": "pause_24",
    "Previous": "previous_24",
    "Live": "live_24",
    "History": "history_24",
    "FastForward": "fast_forward_24",
    # Data > Data Analysis
    "DataAnalysis": "data_histogram_24",
    # Home > Screenshot
    "Screenshot": "screenshot_24",
    # Developer > Debug in VS Code
    "Bug": "bug_24",
    # View > Errors
    "Warning": "warning_24",
    # one icon per command where Excel has its own (2026-11-03: commands no longer share a glyph)
    "HideWindow": "eye_off_24",
    "SideBySide": "board_split_24",
    "SyncScroll": "arrow_sync_circle_24",
    "HeaderFooter": "document_header_footer_24",
    "Header": "document_header_24",
    "Footer": "document_footer_24",
    "GoToHeader": "document_header_arrow_down_24",
    "Icons": "icons_24",
    "Background": "image_multiple_24",
    "ChangePicture": "image_arrow_counterclockwise_24",
    "ResetPicture": "image_arrow_back_24",
    "Symbol": "symbols_24",
    "Delete": "delete_24",
    "Connections": "plug_connected_24",
    "CalcOptions": "calculator_arrow_clockwise_24",
    "NewNote": "note_add_24",
    "Note": "note_24",
    "NoteEdit": "note_edit_24",
    "Crop": "crop_24",
    "ScaleWidth": "arrow_autofit_width_24",
    "ScaleHeight": "arrow_autofit_height_24",
    "WordArt": "text_effects_24",
    "Effects": "sparkle_24",
    "ResizeTable": "table_resize_column_24",
    "Map": "globe_24",
    "RecommendedCharts": "data_bar_vertical_star_24",
    "ImportText": "document_csv_24",
    "Rotate": "arrow_rotate_clockwise_24",
    "PivotLayout": "table_simple_multiple_24",
    "Brush": "paint_brush_24",
    "PivotChart": "data_area_24",
    "ShowFormulas": "math_symbols_24",
    "UseInFormula": "braces_24",
    "CustomViews": "layout_cell_four_24",
    "FocusCell": "scan_dash_24",
    "ZoomSelection": "arrow_expand_24",
    "WindowPosition": "window_arrow_up_24",
    "ViewCode": "code_block_24",
    "GroupDates": "calendar_ltr_24",
    "RefreshAll": "arrow_repeat_all_24",
    "Move": "arrow_move_24",
    "SelectCells": "cursor_click_24",
    "Edit": "edit_24",
    "Axis": "arrow_bidirectional_up_down_24",
    "GoToFooter": "document_page_bottom_center_24",
    "ImageEdit": "image_edit_24",
    "Pin": "pin_24",
    "Unpin": "pin_off_24",
    "Shield": "shield_24",
    "MathTrig": "math_format_professional_24",
    "MoreFunctions": "more_horizontal_24",
}


def main() -> int:
    data: dict[str, int] = json.loads(MAP.read_text(encoding="utf-8"))
    lines = [
        "// generated by tools/gen_icons.py, do not edit by hand.",
        "// source: third_party/fluent-icons/FluentSystemIcons-Regular.json (MIT).",
        "#pragma once",
        "",
        "namespace yc::ui::icon {",
        "",
    ]
    missing = []
    for const, name in ICONS.items():
        key = f"ic_fluent_{name}_regular"
        cp = data.get(key)
        if cp is None:
            missing.append(key)
            continue
        utf8 = chr(cp).encode("utf-8")
        esc = "".join(f"\\x{b:02X}" for b in utf8)
        lines.append(f'inline constexpr const char* {const} = "{esc}";  // {name} U+{cp:04X}')
    lines += ["", "}  // namespace yc::ui::icon", ""]
    if missing:
        print("These icons were not found:", *missing, sep="\n  ", file=sys.stderr)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"Generated {OUT.relative_to(ROOT)} ({len(ICONS)} icons)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
