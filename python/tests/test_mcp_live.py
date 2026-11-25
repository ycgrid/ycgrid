# SPDX-License-Identifier: Apache-2.0
"""The MCP tools (yc.mcp.tools) against a hidden YCGrid: each tool is called the way an agent calls it and checked by
what it did to the workbook or what it reported. Skipped without a built ycgrid.exe or with YC_NO_LIVE=1."""
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import yc  # noqa: E402
from yc.mcp import tools  # noqa: E402
from yc.mcp.tools import ToolError  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.environ.get("YCGRID_EXE") or next(
    (p for p in (os.path.join(ROOT, d, "ycgrid.exe") for d in ("dist_release", "dist_debug")) if os.path.exists(p)), None)


@unittest.skipIf(not EXE or os.environ.get("YC_NO_LIVE"), "needs a built ycgrid.exe")
class McpToolsTest(unittest.TestCase):
    """One hidden YCGrid for the class; every test works on a sheet of its own."""

    @classmethod
    def setUpClass(cls):
        cls.proc = yc.launch(hidden=True, exe=EXE, lang="en")
        cls.conn = yc.connect(target=cls.proc.pid)
        tools.STATE["target"] = cls.proc.pid
        from yc import safety

        safety.set_operator_mode(True)
        end = time.time() + 60
        while not cls.conn.request("python.status", {}).get("workbook_open_done") and time.time() < end:
            time.sleep(0.2)
        cls.n = 0
        # the workbook gets a project folder of its own, with a macro: the host, macros, panels and reloads have
        # something to run (an unsaved workbook has no program folder)
        from yc import excel

        cls.app_dir = tempfile.mkdtemp(prefix="yc_mcp_")
        with open(os.path.join(cls.app_dir, "main.py"), "w", encoding="utf-8") as f:
            f.write("from yc.excel import *\n\n\ndef hello(x=1):\n    return x + 1\n")
        path = os.path.join(cls.app_dir, "McpProj.ycg")
        excel.ActiveWorkbook.SaveAs(path)
        end = time.time() + 20
        while not os.path.exists(path) and time.time() < end:
            time.sleep(0.2)
        tools.yc_reload_python(timeout_ms=60000)
        tools._host_ready()

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(10)

    def setUp(self):
        type(self).n += 1
        self.sheet = f"T{type(self).n}"
        tools.yc_manage_sheets("add", new_name=self.sheet)
        tools.yc_manage_sheets("activate", sheet=self.sheet)

    def grid(self):
        tools.yc_write_range("A1:C4", self.sheet, values=[["Sym", "Qty", "Px"], ["TXF", 1, 100.5], ["MXF", 2, 50], ["TXF", 3, 101]])

    # ---- workbook and reading ----
    def test_workbook_info_lists_sheets_selection_and_layout(self):
        self.grid()
        info = tools.yc_get_workbook_info()
        me = next(s for s in info["sheets"] if s["name"] == self.sheet)
        self.assertEqual(me["used_range"], "A1:C4")
        self.assertEqual(me["module"], f"sheets/{me['code_name']}.py")
        self.assertIn("areas", info["selection"])
        self.assertIn("auto", info["calc"])
        self.assertIn("containers", info["layout"])
        only = tools.yc_get_workbook_info(include=["file"])
        self.assertEqual(list(only), ["file"])

    def test_read_range_as_table_json_fields_and_pages(self):
        self.grid()
        tools.yc_write_range("D2", self.sheet, values=[["=B2*C2"]])
        text = tools.yc_read_range("A1:D2", self.sheet)
        self.assertIn('| 1 | "Sym" | "Qty" | "Px" |', text)
        self.assertIn("| 2 | \"TXF\" | 1 | 100.5 | 100.5 |", text)
        j = tools.yc_read_range("A1:D2", self.sheet, output="json", fields=["value", "formula"])
        self.assertEqual(j["range"], "A1:D2")
        self.assertIn("=B2*C2", j["rows"][1][3])
        page = tools.yc_read_range("A:C", self.sheet, output="json", max_cells=3)  # whole columns clip to the used range
        self.assertTrue(page["truncated"])
        self.assertEqual(page["next_row_offset"], 1)
        page2 = tools.yc_read_range("A:C", self.sheet, output="json", max_cells=3, row_offset=page["next_row_offset"])
        self.assertEqual(page2["rows"][0][0], '"TXF"')
        with self.assertRaises(ToolError):
            tools.yc_read_range("A1:C4", self.sheet, row_offset=99)
        summary = tools.yc_read_range("A1:C4", self.sheet, summary_only=True)
        self.assertEqual(summary["column_kinds"], {"A": "text", "B": "number", "C": "number"})
        tools.yc_write_range("E1", self.sheet, values=[["2026-03-04"]])
        self.assertIn("2026-03-04", tools.yc_read_range("E1", self.sheet))

    def test_inspect_cells_reports_formula_style_merge_validation_and_rules(self):
        self.grid()
        tools.yc_write_range("D2", self.sheet, values=[["=B2*C2"]])
        tools.yc_format_range("D2", self.sheet, number_format="0.00", font={"bold": True})
        tools.yc_format_range("A6:B6", self.sheet, merge="merge")
        from yc import excel

        ws = excel.Worksheets(self.sheet)
        ws.Range("C2").Validation.Add(Type=excel.xlValidateWholeNumber, Operator=excel.xlBetween, Formula1="1", Formula2="9")
        ws.Range("B2:B4").FormatConditions.Add(Type=excel.xlCellValue, Operator=excel.xlGreater, Formula1="=1")
        cells = tools.yc_inspect_cells("D2", self.sheet)["cells"]
        self.assertEqual(cells[0]["formula"], "=B2*C2")
        self.assertEqual(cells[0]["formula_r1c1"], "=RC[-2]*RC[-1]")
        self.assertEqual(cells[0]["number_format"], "0.00")
        self.assertTrue(cells[0]["style"]["bold"])
        self.assertEqual(cells[0]["type"], "float")
        m = tools.yc_inspect_cells("A6", self.sheet, include=["merge"])["cells"][0]
        self.assertEqual(m["merge_area"], "A6:B6")
        v = tools.yc_inspect_cells("C2", self.sheet, include=["validation"])["cells"][0]
        self.assertEqual(v["validation"]["formula1"], "1")
        r = tools.yc_inspect_cells("B2", self.sheet, include=["conditional_format"])["cells"][0]
        self.assertEqual(len(r["conditional_formats"]), 1)
        d = tools.yc_inspect_cells("A1:C4", self.sheet, include=["precedents"], max_cells=2)
        self.assertTrue(d["truncated"])
        self.assertIn("unavailable", d["cells"][0])

    def test_find_with_wildcards_whole_words_regex_and_formulas(self):
        self.grid()
        tools.yc_write_range("D2", self.sheet, values=[["=B2*C2"]])
        self.assertEqual([h["address"] for h in tools.yc_find("txf", sheet=self.sheet)["matches"]], ["A2", "A4"])
        self.assertEqual(tools.yc_find("TX?", sheet=self.sheet, look_at="whole")["total"], 2)
        self.assertEqual(tools.yc_find("TX", sheet=self.sheet, look_at="whole")["total"], 0)
        self.assertEqual(tools.yc_find("^M.F$", sheet=self.sheet, regex=True)["matches"][0]["address"], "A3")
        self.assertEqual(tools.yc_find("B2*", sheet=self.sheet, look_in="formulas")["matches"][0]["address"], "D2")
        self.assertEqual(tools.yc_find("txf", sheet=self.sheet, match_case=True)["total"], 0)
        self.assertGreaterEqual(tools.yc_find("TXF", scope="workbook")["total"], 2)
        r = tools.yc_find("T*", sheet=self.sheet, max_results=1)
        self.assertTrue(r["truncated"])
        with self.assertRaises(ToolError):
            tools.yc_find("x", look_in="comments")

    # ---- writing and formatting ----
    def test_write_range_values_fill_modes_and_shape_check(self):
        from yc import excel

        ws = excel.Worksheets(self.sheet)
        out = tools.yc_write_range("A1:B2", self.sheet, values=[[1, 2], [3, 4]])
        self.assertIn("Wrote", out)
        self.assertEqual(ws.Range("B2").Value, 4)
        tools.yc_write_range("C1:C3", self.sheet, fill_value=7)
        self.assertEqual([ws.Range(f"C{i}").Value for i in (1, 2, 3)], [7, 7, 7])
        tools.yc_write_range("D1", self.sheet, values=[["=A1+B1"]], mode="formulas")
        self.assertEqual(ws.Range("D1").Value, 3)
        tools.yc_write_range("D2", self.sheet, values=[["=A1+B1"]], mode="values")
        self.assertEqual(ws.Range("D2").Value, "=A1+B1")  # kept as text
        with self.assertRaises(ToolError):
            tools.yc_write_range("A1:B2", self.sheet, values=[[1, 2, 3]])
        with self.assertRaises(ToolError):
            tools.yc_write_range("A1", self.sheet)
        note = tools.yc_write_range("E1", self.sheet, values=[[1]], fire_events=False, undoable=False)
        self.assertIn("fire_events", note)
        self.assertIn("undoable", note)

    def test_format_range_applies_every_kind_of_format(self):
        from yc import excel

        ws = excel.Worksheets(self.sheet)
        self.grid()
        out = tools.yc_format_range("C2:C4", self.sheet, number_format="#,##0.00", font={"name": "Arial", "size": 12, "bold": True, "italic": True,
                                    "underline": "single", "color": "#FF0000", "strikethrough": False},
                                    fill={"color": "#00FF00"}, borders=[{"edges": ["outline"], "style": "thin", "color": "#0000FF"}, {"edges": ["inside"], "style": "dashed"}],
                                    alignment={"horizontal": "center", "vertical": "top", "wrap_text": True, "indent": 1}, column_width=20, row_height=30)
        for word in ("number format", "font", "fill", "borders", "alignment", "column width", "row height"):
            self.assertIn(word, out)
        c = ws.Range("C2")
        self.assertEqual(c.NumberFormat, "#,##0.00")
        self.assertTrue(c.Font.Bold and c.Font.Italic)
        self.assertEqual(c.Font.Name, "Arial")
        self.assertEqual(c.Font.Color, 0xFF)
        self.assertEqual(c.Interior.Color, 0xFF00)
        self.assertEqual(c.HorizontalAlignment, excel.xlCenter)
        self.assertTrue(c.WrapText)
        self.assertEqual(ws.Columns("C").ColumnWidth, 20)
        self.assertEqual(ws.Rows(2).RowHeight, 30)
        tools.yc_format_range("A6:C6", self.sheet, merge="merge_center")
        self.assertEqual(ws.Range("A6").MergeArea.Address(False, False), "A6:C6")
        tools.yc_format_range("A6", self.sheet, merge="unmerge")
        self.assertEqual(ws.Range("A6").MergeArea.Address(False, False), "A6")
        tools.yc_format_range("A1:C4", self.sheet, autofit="both")
        self.assertIn("cleared formats", tools.yc_format_range("C2", self.sheet, clear="formats"))
        self.assertEqual(ws.Range("C2").NumberFormat, "General")
        self.assertEqual(ws.Range("C2").Value, 100.5)
        tools.yc_format_range("C2", self.sheet, clear="contents")
        self.assertIsNone(ws.Range("C2").Value)
        tools.yc_format_range("A1", self.sheet, fill={"color": "none"})

    def test_edit_structure_rows_columns_cells_hide_freeze_and_groups(self):
        from yc import excel

        ws = excel.Worksheets(self.sheet)
        self.grid()
        tools.yc_edit_structure("insert_rows", "2:2", self.sheet)
        self.assertEqual(ws.Range("A3").Value, "TXF")
        tools.yc_edit_structure("delete_rows", "2:2", self.sheet)
        self.assertEqual(ws.Range("A2").Value, "TXF")
        tools.yc_edit_structure("insert_columns", "A:A", self.sheet)
        self.assertEqual(ws.Range("B1").Value, "Sym")
        tools.yc_edit_structure("delete_columns", "A:A", self.sheet)
        tools.yc_edit_structure("insert_cells", "A1", self.sheet, shift="right")
        self.assertEqual(ws.Range("B1").Value, "Sym")
        tools.yc_edit_structure("delete_cells", "A1", self.sheet, shift="left")
        self.assertEqual(ws.Range("A1").Value, "Sym")
        tools.yc_edit_structure("hide", "B:B", self.sheet)
        self.assertTrue(ws.Columns("B").Hidden)
        tools.yc_edit_structure("unhide", "B:B", self.sheet)
        self.assertFalse(ws.Columns("B").Hidden)
        tools.yc_edit_structure("hide", "3:3", self.sheet)
        self.assertTrue(ws.Rows(3).Hidden)
        tools.yc_edit_structure("unhide", "3:3", self.sheet)
        tools.yc_edit_structure("freeze_panes", "B2", self.sheet)
        self.assertEqual(tools.yc_scroll_view(self.sheet)["freeze"], [1, 1])
        tools.yc_edit_structure("unfreeze_panes", sheet=self.sheet)
        self.assertEqual(tools.yc_scroll_view(self.sheet)["freeze"], [0, 0])
        tools.yc_edit_structure("group", "2:3", self.sheet)
        self.assertEqual(ws.Rows(2).OutlineLevel, 2)
        tools.yc_edit_structure("ungroup", "2:3", self.sheet)
        self.assertEqual(ws.Rows(2).OutlineLevel, 1)
        tools.yc_edit_structure("group", "B:C", self.sheet)
        self.assertEqual(ws.Columns("B").OutlineLevel, 2)
        tools.yc_edit_structure("ungroup", "B:C", self.sheet)
        with self.assertRaises(ToolError):
            tools.yc_edit_structure("insert_rows", sheet=self.sheet)
        with self.assertRaises(ToolError):
            tools.yc_edit_structure("explode", "A1", self.sheet)

    def test_manage_sheets_add_rename_move_copy_hide_color_and_delete(self):
        from yc import excel

        out = tools.yc_manage_sheets("add", new_name=self.sheet + "b", after=self.sheet)
        self.assertIn(self.sheet + "b", out)
        names = [s.Name for s in excel.Worksheets]
        self.assertEqual(names.index(self.sheet + "b"), names.index(self.sheet) + 1)
        tools.yc_manage_sheets("rename", sheet=self.sheet + "b", new_name=self.sheet + "c")
        self.assertIn(self.sheet + "c", [s.Name for s in excel.Worksheets])
        tools.yc_manage_sheets("move", sheet=self.sheet + "c", before=self.sheet)
        names = [s.Name for s in excel.Worksheets]
        self.assertEqual(names.index(self.sheet + "c"), names.index(self.sheet) - 1)
        before = excel.Worksheets.Count
        tools.yc_manage_sheets("copy", sheet=self.sheet + "c", after=self.sheet)
        self.assertEqual(excel.Worksheets.Count, before + 1)
        tools.yc_manage_sheets("hide", sheet=self.sheet + "c")
        self.assertEqual(excel.Worksheets(self.sheet + "c").Visible, 0)
        tools.yc_manage_sheets("unhide", sheet=self.sheet + "c")
        self.assertEqual(excel.Worksheets(self.sheet + "c").Visible, -1)
        tools.yc_manage_sheets("set_tab_color", sheet=self.sheet + "c", tab_color="#FF0000")
        self.assertEqual(excel.Worksheets(self.sheet + "c").Tab.Color, 0xFF)
        tools.yc_manage_sheets("set_tab_color", sheet=self.sheet + "c")
        with self.assertRaises(ToolError):
            tools.yc_manage_sheets("delete", sheet=self.sheet + "c")  # without confirm
        tools.yc_manage_sheets("delete", sheet=self.sheet + "c", confirm=True)
        self.assertNotIn(self.sheet + "c", [s.Name for s in excel.Worksheets])
        with self.assertRaises(ToolError):
            tools.yc_manage_sheets("delete", sheet="no_such_sheet", confirm=True)
        with self.assertRaises(ToolError):
            tools.yc_manage_sheets("explode", sheet=self.sheet)
        self.assertIn("Switched", tools.yc_manage_sheets("activate", sheet="1"))  # by index too

    # ---- controls, charts, panels, layout ----
    def test_controls_add_list_modify_trigger_and_delete(self):
        c = tools.yc_add_control(self.sheet, name="btnGo", caption="Go", cover_range="E2:F3", kind="button", font={"bold": True},
                                 back_color="#C0C0C0", fore_color="#000080")
        self.assertEqual(c["name"], "btnGo")
        self.assertIn("def btnGo_Click", c["handler_template"])
        lst = tools.yc_list_controls(self.sheet)
        self.assertEqual([x["name"] for x in lst], ["btnGo"])
        self.assertEqual(lst[0]["caption"], "Go")
        self.assertFalse(lst[0]["handler_exists"])
        self.assertIn("btnGo_Click", lst[0]["handler"])
        cb = tools.yc_add_control(self.sheet, name="chkOn", kind="checkbox", left=10, top=10, value=True, linked_cell="H1")
        self.assertEqual(cb["name"], "chkOn")
        self.assertEqual([x["name"] for x in tools.yc_list_controls(self.sheet, kind="checkbox")], ["chkOn"])
        m = tools.yc_modify_control("btnGo", self.sheet, caption="Run", width=120, z_order="bring_to_front", enabled=False)
        self.assertEqual(m["control"]["caption"], "Run")
        self.assertEqual(m["control"]["width"], 120)
        r = tools.yc_modify_control("btnGo", self.sheet, new_name="btnRun")
        self.assertIn("reminder", r)
        t = tools.yc_trigger_control("btnRun", self.sheet)
        self.assertIsInstance(t, dict)
        self.assertIn("Deleted", tools.yc_modify_control("btnRun", self.sheet, delete=True))
        self.assertEqual([x["name"] for x in tools.yc_list_controls(self.sheet)], ["chkOn"])
        with self.assertRaises(ToolError):
            tools.yc_add_control(self.sheet, kind="hologram")
        st = tools.yc_python_status(include=["controls"])
        self.assertTrue(any(x["control"] == f"{self.sheet}.chkOn" for x in st["controls"]))

    def test_charts_add_list_modify_series_and_delete(self):
        self.grid()
        ch = tools.yc_add_chart("xlColumnClustered", self.sheet, name="chQty", source=f"{self.sheet}!B1:C4", title="Qty", cover_range="E5:H12")
        self.assertEqual(ch["name"], "chQty")
        self.assertEqual(ch["chart_type"], "xlColumnClustered")
        self.assertEqual(ch["title"], "Qty")
        self.assertEqual(len(ch["series"]), 2)
        self.assertEqual([c["name"] for c in tools.yc_list_charts(self.sheet)], ["chQty"])
        m = tools.yc_modify_chart("chQty", self.sheet, set={"chart_type": "xlLine", "title": "Lines", "legend": False, "width": 300})
        self.assertEqual(m["chart_type"], "xlLine")
        self.assertEqual(m["title"], "Lines")
        self.assertFalse(m["legend"])
        self.assertEqual(m["width"], 300)
        m = tools.yc_modify_chart("chQty", self.sheet, series=[{"index": 1, "name": "Quantity", "axis": "secondary", "color": "#FF0000"},
                                                                {"op": "delete", "index": 2}])
        self.assertEqual([s["name"] for s in m["series"]], ["Quantity"])
        self.assertEqual(m["series"][0]["axis"], "secondary")
        m = tools.yc_modify_chart("chQty", self.sheet, series=[{"name": "Py", "y": [1, 2, 3]}])
        py = next(s for s in m["series"] if s["source"] == "python")
        self.assertEqual(py["points"], 3)
        with self.assertRaises(ToolError):
            tools.yc_modify_chart("chQty", self.sheet, set={"flavour": 1})
        with self.assertRaises(ToolError):
            tools.yc_add_chart("xlHologram", self.sheet)
        self.assertIn("Deleted", tools.yc_modify_chart("chQty", self.sheet, delete=True))
        self.assertEqual(tools.yc_list_charts(self.sheet), [])

    def test_panels_add_list_input_and_delete(self):
        name = "pnl" + self.sheet
        r = tools.yc_manage_panels("add", name=name, dock="right", controls=[{"type": "ycButton", "name": "btnA", "caption": "A"},
                                                                              {"type": "spin", "name": "spQty", "value": 2, "min": 0, "max": 5}])
        self.assertIsInstance(r, dict)
        self.assertIn(name, [p["name"] for p in tools.yc_manage_panels("list")])
        self.assertEqual(tools.yc_panel_input(name, "spQty", "set_value", "9")["action"], "set_value")
        tools.yc_panel_input(name, "btnA")
        self.assertIn("deleted", tools.yc_manage_panels("delete", name=name))
        self.assertNotIn(name, [p["name"] for p in tools.yc_manage_panels("list")])
        with self.assertRaises(ToolError):
            tools.yc_manage_panels("add")

    def test_layout_describe_float_containers_and_saved_layouts(self):
        d = tools.yc_manage_layout("describe")
        self.assertIn(self.sheet, [v["sheet"] for v in d["sheets"]])
        self.assertIn("saved_layouts", d)
        self.assertIn("float_sheet", tools.yc_manage_layout("float_sheet", sheet=self.sheet, rect={"left": 100, "top": 100, "width": 500, "height": 300}))
        self.assertTrue(next(v for v in tools.yc_manage_layout("describe")["sheets"] if v["sheet"] == self.sheet)["floating"])
        tools.yc_manage_layout("dock_sheet", sheet=self.sheet)
        self.assertFalse(next(v for v in tools.yc_manage_layout("describe")["sheets"] if v["sheet"] == self.sheet)["floating"])
        c = tools.yc_manage_layout("add_container", container="box" + self.sheet, rect={"left": 50, "top": 50, "width": 600, "height": 400})
        self.assertIsInstance(c, (dict, str, int))
        self.assertIn("docked", tools.yc_manage_layout("dock_into", container="box" + self.sheet, item="sheet:" + self.sheet))
        tools.yc_manage_layout("remove_container", container="box" + self.sheet)
        self.assertIn("Saved", tools.yc_manage_layout("save_layout", layout_name="L" + self.sheet))
        self.assertIn("L" + self.sheet, tools.yc_manage_layout("describe")["saved_layouts"])
        self.assertIn("Restored", tools.yc_manage_layout("restore_layout", layout_name="L" + self.sheet))
        with self.assertRaises(ToolError):
            tools.yc_manage_layout("teleport")

    def test_scroll_view_goto_select_zoom_and_freeze(self):
        self.grid()
        v = tools.yc_scroll_view(self.sheet, goto="B30")
        self.assertEqual(v["active_cell"], "B30")
        self.assertTrue(v["visible_range"].startswith("B30"))
        v = tools.yc_scroll_view(self.sheet, select="A1:C2", zoom=150)
        self.assertEqual(v["selection"], ["A1:C2"])
        self.assertEqual(v["zoom"], 150)
        v = tools.yc_scroll_view(self.sheet, freeze="B2")
        self.assertEqual(v["freeze"], [1, 1])
        v = tools.yc_scroll_view(self.sheet, freeze="none", zoom=100, scroll_row=5, scroll_column=2)
        self.assertEqual(v["freeze"], [0, 0])
        self.assertTrue(v["visible_range"].startswith("B5"))

    # ---- commands, python, formulas, console ----
    def test_execute_command_runs_queries_and_refuses_unknown(self):
        from yc import excel

        tools.yc_scroll_view(self.sheet, select="A1")
        tools.yc_write_range("A1", self.sheet, values=[["x"]])
        self.assertEqual(tools.yc_execute_command("Bold"), "Ran Bold")
        self.assertTrue(excel.Worksheets(self.sheet).Range("A1").Font.Bold)
        st = tools.yc_execute_command("Bold", query=True)
        self.assertIsInstance(st, dict)
        with self.assertRaises(ToolError):
            tools.yc_execute_command("NoSuchCommandAnywhere")

    def test_python_status_env_exec_and_console(self):
        st = tools.yc_python_status()
        self.assertIn("mode", st["host"])
        for k in ("modules", "macros", "udfs", "events", "load_errors", "controls"):
            self.assertIn(k, st)
        self.assertIn("feeds", tools.yc_python_status(include=["feeds"]))
        env = tools.yc_python_env()
        self.assertTrue(env["python"].startswith("3."))
        self.assertIn("installed", env)
        with self.assertRaises(ToolError):
            tools.yc_python_env("upgrade", version="3.13")
        r = tools.yc_exec_python(f"ws = Worksheets({self.sheet!r})\nws.Range('A1').Value = 41\nprint('hi')\nws.Range('A1').Value + 1")
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["result"], 42)
        self.assertIn("hi", r["output"])
        bad = tools.yc_exec_python("1/0")
        self.assertFalse(bad["ok"])
        self.assertIn("ZeroDivisionError", bad["error"])
        host = tools.yc_exec_python("Range('A2').Value = 'from host'\n'ok'", target="host")
        self.assertIsInstance(host, dict)
        con = tools.yc_read_console(max_lines=5)
        self.assertIn("next_cursor=", con)
        tools.yc_read_console(min_level="error", sources=["python"])

    def test_evaluate_formula_scalar_array_and_error(self):
        self.grid()
        self.assertEqual(tools.yc_evaluate_formula("SUM(B2:B4)", self.sheet)["value"], 6)
        self.assertEqual(tools.yc_evaluate_formula("=1/0", self.sheet)["type"], "error")
        r = tools.yc_evaluate_formula("=SEQUENCE(3,2)", self.sheet)  # the whole array (it gave its first cell only)
        self.assertEqual((r["type"], r["value"]), ("array", [[1, 2], [3, 4], [5, 6]]))
        r = tools.yc_evaluate_formula("=SEQUENCE(300)", self.sheet, max_cells=10)
        self.assertTrue(r["truncated"])
        self.assertEqual(len(r["rows"]), 10)
        self.assertIn("note", tools.yc_evaluate_formula("=1+1", self.sheet, as_cell="B2"))
        self.assertEqual(tools.yc_evaluate_formula('="a"&"b"', self.sheet)["value"], "ab")

    def test_reload_python_refuses_a_syntax_error_in_the_project(self):
        app_dir = tools.rq("python.status").get("app_dir") or ""
        self.assertEqual(os.path.normcase(app_dir), os.path.normcase(self.app_dir))
        bad = os.path.join(app_dir, "broken_tmp.py")
        with open(bad, "w", encoding="utf-8") as f:
            f.write("def (:\n")
        try:
            r = tools.yc_reload_python()
            self.assertFalse(r["reloaded"])
            self.assertEqual(r["errors"][0]["file"], "broken_tmp.py")
        finally:
            os.remove(bad)
        r = tools.yc_reload_python(timeout_ms=60000)
        self.assertTrue(r["reloaded"], r)
        m = tools.yc_run_macro("hello", [2])
        self.assertTrue(m.get("ok"), m)
        self.assertEqual(m["result"], 3)
        self.assertIn("error", tools.yc_run_macro("no_such_macro"))

    # ---- UI ----
    def test_ui_input_types_selects_and_presses_keys(self):
        from yc import excel

        r = tools.yc_ui_input([{"type": "select", "address": "B2"}, {"type": "type", "text": "=1+2"}, {"type": "type", "text": "abc", "commit": "tab"},
                               {"type": "keys", "keys": "Ctrl+Home"}, {"type": "click", "address": "C5"}, {"type": "wait", "ms": 50},
                               {"type": "double_click", "address": "C6"}, {"type": "keys", "keys": "Escape"}, {"type": "right_click", "address": "A1"},
                               {"type": "keys", "keys": "esc"}, {"type": "scroll_to", "address": "A1"}, {"type": "design_mode", "on": False}],
                              sheet=self.sheet)
        self.assertTrue(r["done"])
        ws = excel.Worksheets(self.sheet)
        self.assertEqual(ws.Range("B2").Value, 3)
        self.assertEqual(ws.Range("B3").Value, "abc")
        self.assertIn("view", r)
        self.assertIn("console_tail", r)
        self.assertEqual(tools._key_token("Ctrl+Shift+Down"), "k:c-s-DownArrow")
        self.assertEqual(tools._key_token("alt+F4"), "k:a-F4")
        self.assertEqual(tools._key_token("`"), "k:GraveAccent")

    def test_screenshot_returns_an_image(self):
        self.grid()
        img = tools.yc_screenshot()
        self.assertTrue(img.data.startswith(b"\x89PNG"))
        rng = tools.yc_screenshot(target="range", address="A1:C4", sheet=self.sheet, max_width=600)
        self.assertTrue(rng.data.startswith(b"\x89PNG"))
        self.assertIn("x", rng.text)
        with self.assertRaises(ToolError):
            tools.yc_screenshot(target="range")

    def test_ui_state_calculate_topics_tables_and_replay(self):
        st = tools.yc_ui_state()
        self.assertIn("sheets", st)
        c = tools.yc_calculate()
        self.assertIn("auto", c)
        self.assertIn("auto", tools.yc_calculate("full"))
        self.assertIn("auto", tools.yc_calculate("stats_only"))
        tools.yc_write_range("A1", self.sheet, values=[['=QUOTE("TXF.last")']])
        self.assertIn("Pushed", tools.yc_topics("push", topics=["TXF.last"], values=[123.5]))
        look = tools.yc_topics("lookup", address=f"{self.sheet}!A1")
        self.assertIn("QUOTE", look["formulas"][0][0])
        with self.assertRaises(ToolError):
            tools.yc_topics("push", topics=["a"], values=[1, 2])
        with self.assertRaises(ToolError):
            tools.yc_topics("list")
        self.assertIsInstance(tools.yc_bound_tables(), list)
        with self.assertRaises(ToolError):
            tools.yc_bound_tables("read")
        with self.assertRaises(ToolError):
            tools.yc_bound_tables("drop")
        self.assertIsInstance(tools.yc_replay(), dict)
        with self.assertRaises(ToolError):
            tools.yc_replay("play")
        with self.assertRaises(ToolError):
            tools.yc_replay("seek")
        with self.assertRaises(ToolError):
            tools.yc_replay("dance")
        self.assertEqual(tools._plain(float("nan")), None)
        self.assertEqual(tools._plain(3), 3)

    def test_bound_table_read_and_replay_record(self):
        from yc import data

        t = data.bind(self.sheet, {"a": [1, 2, 3], "b": ["x", "y", "z"]}, key="b", name="mcp_tbl" + self.sheet, at="A1")
        time.sleep(0.3)
        names = [x["name"] if isinstance(x, dict) else x for x in tools.yc_bound_tables()]
        self.assertIn("mcp_tbl" + self.sheet, names)
        r = tools.yc_bound_tables("read", table="mcp_tbl" + self.sheet, max_rows=2)
        self.assertEqual(r["columns"], ["a", "b"])
        self.assertEqual(r["rows"][1], [2, "y"])
        self.assertTrue(r["truncated"])
        tape = os.path.join(tempfile.mkdtemp(), "t.yctape")
        rec = tools.yc_replay("record", path=tape, tables=["mcp_tbl" + self.sheet])
        self.assertIn("recording_to", rec)
        t.update({"a": [4], "b": ["w"]}, wait=True)
        time.sleep(0.3)
        st = tools.yc_replay("stop_recording")
        self.assertIsInstance(st, dict)

    def test_workbook_file_save_as_csv_and_options(self):
        from yc import excel

        self.grid()
        folder = tempfile.mkdtemp()
        csv = os.path.join(folder, "out")
        self.assertIn("CSV", tools.yc_workbook_file("save_as", path=csv, format="csv"))
        end = time.time() + 10
        while not os.path.exists(csv + ".csv") and time.time() < end:
            time.sleep(0.2)
        with open(csv + ".csv", encoding="utf-8-sig") as f:
            self.assertIn("TXF", f.read())
        pdf = os.path.join(folder, "sheet.pdf")
        self.assertIn("Exported", tools.yc_workbook_file("export_pdf", path=pdf, sheet=self.sheet))
        self.assertTrue(os.path.exists(pdf))
        with self.assertRaises(ToolError):
            tools.yc_workbook_file("save_as")
        with self.assertRaises(ToolError):
            tools.yc_workbook_file("open")
        with self.assertRaises(ToolError):
            tools.yc_workbook_file("burn")
        opts = tools.yc_app_options(set={"zoom": 120, "display_gridlines": False, "display_formulas": True, "status_bar": "busy"})
        self.assertEqual(opts["zoom"], 120)
        self.assertFalse(opts["display_gridlines"])
        self.assertTrue(opts["display_formulas"])
        tools.yc_app_options(set={"zoom": 100, "display_gridlines": True, "display_formulas": False, "status_bar": None, "calculation": "manual"})
        self.assertEqual(tools.yc_app_options()["calculation"], "manual")
        tools.yc_app_options(set={"calculation": "automatic"})
        self.assertEqual(excel.Application.Calculation, excel.xlCalculationAutomatic)
        with self.assertRaises(ToolError):
            tools.yc_app_options(set={"gravity": 0})

    def test_workbook_store_and_document_properties(self):
        self.assertIn("Wrote", tools.yc_workbook_store("set", key="k1", value={"a": 1}))
        self.assertEqual(tools.yc_workbook_store("get", key="k1"), {"key": "k1", "value": {"a": 1}, "exists": True})
        self.assertIn("k1", tools.yc_workbook_store("list"))
        tools.yc_workbook_store("set", key="s1", value=5, scope="sheet", sheet=self.sheet)
        self.assertEqual(tools.yc_workbook_store("list", scope="sheet", sheet=self.sheet)["s1"], 5)
        self.assertIn("Deleted", tools.yc_workbook_store("delete", key="k1"))
        self.assertFalse(tools.yc_workbook_store("get", key="k1")["exists"])
        with self.assertRaises(ToolError):
            tools.yc_workbook_store("get")
        with self.assertRaises(ToolError):
            tools.yc_workbook_store("squash", key="k")
        self.assertEqual(tools.yc_workbook_store("set", key="Owner", value="me", scope="document_properties"), "OK")
        self.assertEqual(tools.yc_workbook_store("get", key="Owner", scope="document_properties"), "me")
        self.assertEqual(tools.yc_workbook_store("list", scope="document_properties")["Owner"], "me")
        self.assertEqual(tools.yc_workbook_store("delete", key="Owner", scope="document_properties"), "OK")

    def test_plot_input_reaches_a_figure(self):
        from yc import plot

        fig = plot.figure("mcp " + self.sheet, size=(400, 300))
        fig[0].line([1, 2, 3], [1, 4, 9])
        time.sleep(0.3)
        r = tools.yc_plot_input(str(fig.id), [{"action": "click", "x": 2, "y": 4}, {"action": "wheel"}])
        self.assertEqual(r["done"], ["click", "wheel"])
        with self.assertRaises(yc.YcError):
            tools.yc_plot_input("no-such-figure", [{"action": "click", "x": 1, "y": 1}])
        fig.close()

    def test_connect_and_api_lookup(self):
        r = tools.yc_connect(str(self.proc.pid))  # a fresh connection: the class's old one is closed by it
        self.assertEqual(r["selected"]["pid"], self.proc.pid)
        self.assertTrue(r["instances"])
        type(self).conn = tools._ensure()
        hits = tools.yc_api_lookup("AutoFilter", max_results=3)
        self.assertTrue(hits)
        self.assertEqual(tools._fmt_value(None), "")
        self.assertEqual(tools._fmt_value(True), "TRUE")
        self.assertEqual(tools._fmt_value(2.0), "2")
        self.assertEqual(tools._fmt_value("a|b"), '"a|b"')
        self.assertEqual(tools._color("#FF8000"), 0x0080FF)
        self.assertIsNone(tools._color("none"))
        self.assertEqual(tools._addr((0, 0, 0, 0)), "A1")
        self.assertEqual(tools._addr((1, 2, 3, 4)), "C2:E4")


if __name__ == "__main__":
    unittest.main()
