// SPDX-License-Identifier: Apache-2.0
// settings.h - global settings of this computer: %LOCALAPPDATA%\ycgrid\settings.json
//
// One JSON file for everything File -> Options changes (see docs/settings.md). Missing or invalid keys keep their
// defaults, unknown keys are kept when the file is written again, so newer and older YCGrid versions can share it.
// Project settings that travel with a folder stay in that folder's ycgrid.toml.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace yc::app {

struct Settings {
  std::string language;          // "en", "zh-TW", "" = Windows display language (applied after a restart)
  // view
  bool show_gridlines = true;
  bool show_headings = true;
  bool show_formula_bar = true;
  // Page Layout > Margins > Last Custom Setting: left, right, top, bottom, header, footer in points (-1: none yet)
  std::array<double, 6> last_margins{-1, -1, -1, -1, -1, -1};
  int formula_bar_lines = 1;  // the formula bar's height in lines (Ctrl+Shift+U: 1 <-> 3; Application.FormulaBarHeight)
  // formulas (File > Options > Formulas / Advanced): the list of names while a formula is typed, the argument hint
  // (Application.DisplayFormulaAutoComplete, DisplayFunctionToolTips)
  bool formula_autocomplete = true, function_tooltips = true;
  // background error checking (File > Options > Formulas; Application.ErrorCheckingOptions): the green triangles
  bool error_checking = true, err_evaluate_to_error = true, err_number_as_text = true, err_inconsistent_formula = true;
  bool err_text_date = true, err_omitted_cells = true, err_unlocked_formula = true, err_empty_cell_reference = false, err_list_data_validation = true,
       err_inconsistent_list_formula = true, err_misleading_format = true;
  // Insert Function's Most Recently Used list (also the Name Box while a formula is typed), newest first
  std::vector<std::string> recent_functions = {"SUM", "AVERAGE", "IF", "HYPERLINK", "COUNT", "MAX", "SIN", "SUMIF", "PMT", "STDEV"};
  // Insert > Symbol's Recently used symbols (UTF-8 characters) and Insert > Shapes' Recently Used Shapes (command ids)
  std::vector<std::string> recent_symbols, recent_shapes;
  // File > Options > Proofing (Spelling > Options...): words skipped by the spelling checker
  bool spell_ignore_upper = true, spell_ignore_numbers = true, spell_ignore_internet = true, spell_repeated = true;
  // AutoCorrect: a word typed is replaced when the cell is entered (Spelling > AutoCorrect, Application.AutoCorrect)
  std::vector<std::pair<std::string, std::string>> autocorrect;
  std::vector<std::string> autocorrect_removed;  // built-in replacements the user deleted
  bool autocorrect_replace = true;               // Replace text as you type (Application.AutoCorrect.ReplaceText)
  // the status bar's selection statistics (right-click the status bar, as in Excel)
  bool status_average = true, status_count = true, status_numerical_count = false, status_min = false, status_max = false, status_sum = true;
  // ... and its other entries, as Excel's Customize Status Bar: the cell mode (Ready / Enter / Edit / Point), Caps Lock,
  // Num Lock, Scroll Lock, End Mode, the selection modes (Extend / Add to Selection), the view buttons, the zoom
  bool status_cell_mode = true, status_caps_lock = true, status_num_lock = false, status_scroll_lock = true, status_end_mode = true,
       status_selection_mode = true, status_view_shortcuts = true, status_zoom_slider = true, status_zoom = true;
  // [YC] the Error Log's entry on the status bar (always shown; red with the count while errors are unseen)
  bool status_error_log = true;
  // File > Options > General: Quick Analysis on a selection; a new workbook's font, font size and number of sheets
  // (Application.ShowQuickAnalysis, StandardFont, StandardFontSize, SheetsInNewWorkbook; "" / 0 = YCGrid's own)
  bool show_quick_analysis = true;
  std::string standard_font;
  double standard_font_size = 0;
  int sheets_in_new_workbook = 1;
  // File > Options > Advanced, Cut, copy, and paste / Editing: the Paste Options and Insert Options buttons, the
  // question before cells dropped on data replace it (Application.AlertBeforeOverwriting)
  bool show_paste_options = true, show_insert_options = true, alert_before_overwriting = true;
  // File > Options > Advanced > Editing: the question before a paste or a Fill Down / Right over more cells than this many
  // thousand (Application.EnableLargeOperationAlert, LargeOperationCellThousandCount; Excel 16's default, measured)
  bool large_operation_alert = true;
  int large_operation_thousands = 33554;
  // File > Options > Advanced > Chart: the tip over a chart names the element under the mouse and a point's value
  // (Application.ShowChartTipNames, ShowChartTipValues)
  bool show_chart_tip_names = true, show_chart_tip_values = true;
  // File > Options > Formulas: recalculate before saving in manual calculation, stale cells struck through in it
  // (Application.CalculateBeforeSave, FormatStaleValues), multi-threaded calculation and its threads (0 = every
  // processor; Application.MultiThreadedCalculation), and pointing at a PivotTable's value cell while typing a formula
  // writes GETPIVOTDATA (Application.GenerateGetPivotData)
  bool calculate_before_save = true, format_stale_values = true;
  bool multi_threaded_calc = true;
  int calc_thread_count = 0;
  bool generate_getpivotdata = true;
  // calculation
  bool auto_calc = true;
  bool auto_data_tables = true;  // false: Automatic except for data tables (with auto_calc)
  bool excel_15_digit_input = false;  // true: digits beyond 15 become 0 like Excel; false: keep them as Decimal
  bool iterative = false;  // iterative calculation of circular references
  int max_iterations = 100;
  double max_change = 0.001;
  // formulas: R1C1 reference style (column headings are numbers, formulas are shown and typed as R[-1]C)
  bool r1c1 = false;
  // live data: minimum ms between screen updates caused by Python / data feeds (0 = every change)
  int publish_interval_ms = 100;
  // projects: where a hidden instance's Alt+F11 / yc.project creates the project folder of a workbook that was never
  // saved ("" = Documents\YCGrid); with a window, YCGrid asks the user to save the workbook first (PrepareProject)
  std::wstring projects_folder;
  // editor: VS Code executable ("" = find it automatically)
  std::wstring vscode_path;
  // saving a file of the project: false = swap its functions and classes in the running host (state stays),
  // true = restart the Python host
  bool restart_on_save = false;
  // editing: drag the selection's border to move cells, and the fill handle (Application.CellDragAndDrop)
  bool cell_drag_and_drop = true;
  // File > Options > Data: "Disable automatic grouping of Date/Time columns in PivotTables" off (a date field added to
  // Rows or Columns is grouped by Years, Quarters and Months)
  bool pivot_auto_group_dates = true;
  bool follow_links_on_click = true;  // one click on a link cell follows it (Excel); false: Ctrl+click only
  // editing (File > Options > Advanced): Enter moves the selection (Application.MoveAfterReturn) in a direction (0 down,
  // 1 right, 2 up, 3 left: MoveAfterReturnDirection), F2 and double-click edit in the cell (EditDirectlyInCell, else in
  // the formula bar), AutoComplete finishes text from the column (EnableAutoComplete)
  bool move_after_return = true;
  int move_after_return_dir = 0;
  bool edit_directly_in_cell = true;
  bool enable_autocomplete = true;
  bool auto_flash_fill = true;  // Advanced > Automatically Flash Fill (Application.FlashFill): grey suggestions while typing
  // editing: the user's custom lists for AutoFill and Sort (File > Options > Edit Custom Lists)
  std::vector<std::vector<std::string>> custom_lists;
  // save: AutoRecover information every N minutes into autorecover_folder ("" = %LOCALAPPDATA%\ycgrid\AutoRecover)
  bool autorecover = true;
  int autorecover_minutes = 10;
  std::wstring autorecover_folder;
  // the Quick Access Toolbar's commands and the ribbon display option (0 always show, 1 tabs only, 2 auto-hide)
  std::vector<std::string> qat = {"Save", "Undo", "Redo", "Screenshot"};
  int ribbon_mode = 0;
  // errors: every error is also appended to <errors_log_folder>\errors-YYYY-MM-DD.log ("" = %LOCALAPPDATA%\ycgrid\logs);
  // hidden instances (tests, agents) don't write it
  bool errors_log_to_file = true;
  std::wstring errors_log_folder;
  // Trust Center (File > Options > Trust Center > Trust Center Settings; trust.cpp): a workbook's Python code, as
  // Excel's macro security. macro_setting: 0 disable with notification (the Message Bar), 1 disable without
  // notification, 2 enable all. trusted_locations empty and !trusted_locations_set: YCGrid's own folders (trust.cpp)
  struct TrustedLocation {
    std::string path;  // as stored: %YCGRID_DIR% (ycgrid.exe's folder), %YCGRID_PROJECTS% and %VARIABLES% are expanded
    bool subfolders = true;
    std::string description, modified;
  };
  int macro_setting = 0;
  bool message_bar = true;  // Message Bar: show it when Python code has been disabled
  std::vector<TrustedLocation> trusted_locations;
  bool trusted_locations_set = false;
  bool allow_network_locations = false, disable_trusted_locations = false;
  bool allow_network_documents = true, disable_trusted_documents = false;
  // Protected View: files from the Internet (Mark of the Web), in potentially unsafe locations, Outlook attachments
  bool pv_internet = true, pv_unsafe_locations = true, pv_outlook = true;
};

Settings& Config();
std::wstring SettingsPath();
void LoadSettings();   // call once at startup
// the calculation's threads from the settings (Application.MultiThreadedCalculation): one when it is off, else the
// count chosen (0: every processor)
void ApplyCalcThreads();
bool SaveSettings();   // writes the file (atomically); returns false when it can't be written
void EnableSettingsSave(bool on);  // off for hidden instances: what they change stays in memory
bool UserStateSaved();             // false in hidden instances: no settings, recent files, default layout or projects in the user's folders
// Opens a file, a folder or a program on the user's desktop through the shell (ShellExecute "open"); a hidden
// instance (tests, agents) opens nothing there and answers false, so the caller says in the status bar what it would
// open. A click on a screenshot's picture in a chaos round opened the picture viewer on the user's screen, again and
// again. owner is the HWND of the window asking (or null).
bool OpenOnDesktop(const std::wstring& target, const std::wstring& params = L"", void* owner = nullptr, int show = 1);
// the last target OpenOnDesktop was asked to open, opened or not (tests: a hidden instance opens nothing)
std::wstring LastDesktopOpen();
int DesktopOpens();  // how many times it was asked
// the temp folder, never throwing: the standard library's temp folder call throws when %TMP% names a folder that isn't
// there, and a hidden instance asked it while starting (YCGrid ended with abort() before its first frame). A missing
// one is made; one that can't be is replaced by %LOCALAPPDATA%\ycgrid\temp
std::wstring TempFolder();
std::wstring ProjectsDir();  // projects_folder, or Documents\YCGrid
std::wstring AutoRecoverDir();
// A folder for a message: the user's own folders become %TEMP%, %LOCALAPPDATA%... (no user name in screenshots)
std::string ShownPath(const std::wstring& path);  // autorecover_folder, or %LOCALAPPDATA%\ycgrid\AutoRecover

}  // namespace yc::app
